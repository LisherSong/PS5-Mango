#include "filemgr.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "json_util.h"
#include "path_util.h"

#define UPLOAD_BUFFER_SIZE (1024 * 1024)

typedef struct upload_context {
  file_task_t *task;
  int fd;
  char *buffer;
  size_t buffered;
  /* 双缓冲写线程（搬自上游 owendswang/ps5-web-file-manager bdc7520
     “Improved upload speed”）：一块缓冲收网络包的同时另一块在写盘，
     收与写重叠 —— 单缓冲时每满 1MB 都要停下来等 write() 落盘，GbE 链路
     上的空档全花在这上面。初始化失败则退回原来的同步写。 */
  char *writer_buffer;
  size_t writer_size;
  int writer_started;
  int writer_pending;
  int writer_busy;
  int writer_stop;
  int writer_error;
  pthread_t writer_thread;
  pthread_mutex_t writer_mutex;
  pthread_cond_t writer_cond;
  char temp[PATH_MAX];
  char target[PATH_MAX];
  unsigned long long expected;
  unsigned long long written;
  int failed;
  int error;
  int task_done;
  const char *stage;
  char error_message[256];
} upload_context_t;

/* ---- live upload ledger --------------------------------------------------
 * 服务端视角的「这个上传收到了多少字节」。前端进度条原本只信 XHR 的
 * upload.onprogress —— PS5 WebKit 该事件稀疏到只有首帧（真机 2026-10-07
 * 反馈「速度 1MB/s、进度条冻在 2%」），页面既看不出进度也判断不出死活。
 * 这里按连接（ctx 指针）记账，/api/upload/progress 吐给前端轮询：
 *   · 收到字节在涨 → 进度条跟着涨（真口径，与浏览器事件无关）；
 *   · 收到字节停住 → 前端能判定「服务端没在收」，直接报错而不是永远 2%。
 * 槽位按固定数组 + 互斥锁，上传是串行的少量并发（浏览器每连接一个），4 个
 * 足够；找槽按 ctx 指针匹配，不指针存活期问题（begin/end 都在同一线程）。 */
#define UPLOAD_LIVE_SLOTS 4

typedef struct upload_live {
  const void *owner;      /* upload_context_t*，NULL = 空槽 */
  char target[PATH_MAX];
  unsigned long long total;
  unsigned long long received;
  long long updated_ms;   /* CLOCK_REALTIME 毫秒，用于前端判断停滞 */
} upload_live_t;

static upload_live_t g_upload_live[UPLOAD_LIVE_SLOTS];
static pthread_mutex_t g_upload_live_lock = PTHREAD_MUTEX_INITIALIZER;

static long long
upload_now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void
upload_live_begin(const upload_context_t *ctx, unsigned long long total) {
  pthread_mutex_lock(&g_upload_live_lock);
  for(int i = 0; i < UPLOAD_LIVE_SLOTS; i++) {
    if(!g_upload_live[i].owner) {
      g_upload_live[i].owner = ctx;
      g_upload_live[i].target[0] = 0;
      snprintf(g_upload_live[i].target, sizeof(g_upload_live[i].target),
               "%s", ctx->target);
      g_upload_live[i].total = total;
      g_upload_live[i].received = 0;
      g_upload_live[i].updated_ms = upload_now_ms();
      break;
    }
  }
  pthread_mutex_unlock(&g_upload_live_lock);
}

static void
upload_live_add(const upload_context_t *ctx, size_t n) {
  pthread_mutex_lock(&g_upload_live_lock);
  for(int i = 0; i < UPLOAD_LIVE_SLOTS; i++) {
    if(g_upload_live[i].owner == ctx) {
      g_upload_live[i].received += (unsigned long long)n;
      g_upload_live[i].updated_ms = upload_now_ms();
      break;
    }
  }
  pthread_mutex_unlock(&g_upload_live_lock);
}

static void
upload_live_end(const upload_context_t *ctx) {
  pthread_mutex_lock(&g_upload_live_lock);
  for(int i = 0; i < UPLOAD_LIVE_SLOTS; i++) {
    if(g_upload_live[i].owner == ctx) {
      memset(&g_upload_live[i], 0, sizeof(g_upload_live[i]));
      break;
    }
  }
  pthread_mutex_unlock(&g_upload_live_lock);
}

enum MHD_Result
api_upload_progress(struct MHD_Connection *conn) {
  strbuf_t b = {0};
  int first = 1;

  strbuf_printf(&b, "{\"ok\":true,\"uploads\":[");
  pthread_mutex_lock(&g_upload_live_lock);
  for(int i = 0; i < UPLOAD_LIVE_SLOTS; i++) {
    upload_live_t *u = &g_upload_live[i];

    if(!u->owner) continue;
    if(!first) strbuf_append(&b, ",");
    first = 0;
    strbuf_printf(&b, "{\"received\":%llu,\"total\":%llu,\"updated_ms\":%lld,\"target\":",
                  u->received, u->total, u->updated_ms);
    json_escape(&b, u->target);
    strbuf_append(&b, "}");
  }
  pthread_mutex_unlock(&g_upload_live_lock);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static const char *
upload_error_message(upload_context_t *ctx) {
  if(!ctx->error_message[0]) {
    snprintf(ctx->error_message, sizeof(ctx->error_message), "%s failed%s%s: %s",
             ctx->stage ? ctx->stage : "upload",
             ctx->target[0] ? " for " : "",
             ctx->target[0] ? ctx->target : "",
             strerror(ctx->error ? ctx->error : EIO));
  }
  return ctx->error_message;
}

/* 写一块数据到目标 fd，返回 0 或 errno（不再顺手改 ctx->failed —— 调用方决定）。 */
static int
upload_write(upload_context_t *ctx, const char *data, size_t size) {
  size_t written = 0;

  while(written < size) {
    ssize_t n;

    if(ctx->task && task_cancel_requested(ctx->task)) {
      return ECANCELED;
    }
    n = write(ctx->fd, data + written, size - written);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) {
      return n < 0 ? (errno ? errno : EIO) : EIO;
    }
    written += (size_t)n;
    ctx->written += (unsigned long long)n;
  }
  if(ctx->task && written) {
    task_update(ctx->task, TASK_RUNNING, ctx->target,
                (unsigned long long)written, NULL);
  }
  return 0;
}

static void *
upload_writer(void *arg) {
  upload_context_t *ctx = arg;

  pthread_mutex_lock(&ctx->writer_mutex);
  for(;;) {
    int error;

    while(!ctx->writer_pending && !ctx->writer_stop) {
      pthread_cond_wait(&ctx->writer_cond, &ctx->writer_mutex);
    }
    if(ctx->writer_stop && !ctx->writer_pending) break;
    ctx->writer_pending = 0;
    ctx->writer_busy = 1;
    pthread_mutex_unlock(&ctx->writer_mutex);
    error = upload_write(ctx, ctx->writer_buffer, ctx->writer_size);
    pthread_mutex_lock(&ctx->writer_mutex);
    if(error) ctx->writer_error = error;
    ctx->writer_busy = 0;
    pthread_cond_broadcast(&ctx->writer_cond);
  }
  pthread_mutex_unlock(&ctx->writer_mutex);
  return NULL;
}

static void
upload_writer_start(upload_context_t *ctx) {
  ctx->writer_buffer = malloc(UPLOAD_BUFFER_SIZE);
  if(!ctx->writer_buffer) return;
  if(pthread_mutex_init(&ctx->writer_mutex, NULL) == 0) {
    if(pthread_cond_init(&ctx->writer_cond, NULL) == 0) {
      ctx->writer_started =
        pthread_create(&ctx->writer_thread, NULL, upload_writer, ctx) == 0;
      if(!ctx->writer_started) pthread_cond_destroy(&ctx->writer_cond);
    }
    if(!ctx->writer_started) pthread_mutex_destroy(&ctx->writer_mutex);
  }
  if(!ctx->writer_started) {
    free(ctx->writer_buffer);
    ctx->writer_buffer = NULL;
  }
}

/* drain=1 等最后一块写完；drain=0 丢弃未写内容（出错/取消路径）。返回写错误码。 */
static int
upload_writer_stop(upload_context_t *ctx, int drain) {
  int error = 0;

  if(!ctx->writer_started) return 0;
  pthread_mutex_lock(&ctx->writer_mutex);
  if(!drain) ctx->writer_pending = 0;
  while(ctx->writer_busy || (drain && ctx->writer_pending)) {
    pthread_cond_wait(&ctx->writer_cond, &ctx->writer_mutex);
  }
  error = ctx->writer_error;
  ctx->writer_stop = 1;
  pthread_cond_signal(&ctx->writer_cond);
  pthread_mutex_unlock(&ctx->writer_mutex);
  pthread_join(ctx->writer_thread, NULL);
  pthread_cond_destroy(&ctx->writer_cond);
  pthread_mutex_destroy(&ctx->writer_mutex);
  ctx->writer_started = 0;
  return error;
}

static int
upload_flush(upload_context_t *ctx) {
  int error;

  if(!ctx->buffered) return 0;
  if(ctx->writer_started) {
    pthread_mutex_lock(&ctx->writer_mutex);
    while(ctx->writer_pending || ctx->writer_busy) {
      pthread_cond_wait(&ctx->writer_cond, &ctx->writer_mutex);
    }
    error = ctx->writer_error;
    if(!error) {
      /* 交换两块缓冲：满的那块交给写线程，收包这边立刻有空缓冲可用 */
      char *next = ctx->writer_buffer;
      ctx->writer_buffer = ctx->buffer;
      ctx->buffer = next;
      ctx->writer_size = ctx->buffered;
      ctx->buffered = 0;
      ctx->writer_pending = 1;
      pthread_cond_signal(&ctx->writer_cond);
    }
    pthread_mutex_unlock(&ctx->writer_mutex);
  } else {
    error = upload_write(ctx, ctx->buffer, ctx->buffered);
    if(!error) ctx->buffered = 0;
  }
  if(error) {
    ctx->failed = 1;
    ctx->error = error;
    return -1;
  }
  return 0;
}

static void
finish_upload_task(file_task_t *task, task_state_t state, const char *current,
                   const char *error) {
  if(!task) {
    return;
  }
  pthread_mutex_lock(&g_tasks_lock);
  if(task_is_active(task)) {
    time_t completed_at = time(NULL);
    task->state = state;
    if(current) {
      snprintf(task->current, sizeof(task->current), "%s", current);
    }
    if(state == TASK_DONE && task->total) {
      task->done = task->total;
    }
    if(state == TASK_DONE) {
      record_task_completion_locked(task, completed_at);
    }
    if(error) {
      snprintf(task->error, sizeof(task->error), "%s", error);
    }
    if(state == TASK_FAILED) {
      snprintf(task->error_code, sizeof(task->error_code), "upload_failed");
      snprintf(task->error_arg, sizeof(task->error_arg), "%s",
               current ? current : task->src);
    }
    task->updated_at = completed_at;
  }
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
finish_upload_context_error(upload_context_t *ctx) {
  /* 终态错误一旦发布，就不许再有工作线程去改任务进度。 */
  upload_writer_stop(ctx, 0);
  /* error_message 必须无条件先填：无 task 的单文件上传（存档 ZIP / PKG 暂存
   * 都不带 X-WFM-Task-ID）走不到下面的 task 记录，但 HTTP 响应体要拿它当
   * error 文案 —— 之前 task==NULL 早退导致 409/507 一律回 error:""，
   * 前端只能显示 HTTP 409 而非「文件已存在」这类真实原因。 */
  upload_error_message(ctx);
  if(!ctx->task || ctx->task_done) {
    return;
  }
  finish_upload_task(ctx->task,
                     ctx->error == ECANCELED ? TASK_CANCELED : TASK_FAILED,
                     ctx->target[0] ? ctx->target : ctx->task->current,
                     ctx->error == ECANCELED ? "canceled" : ctx->error_message);
  ctx->task_done = 1;
}

static file_task_t *
upload_task_from_conn(struct MHD_Connection *conn) {
  char *idstr = request_value(conn, "X-WFM-Task-ID", "task_id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task = NULL;

  free(idstr);
  if(!id) {
    return NULL;
  }
  pthread_mutex_lock(&g_tasks_lock);
  task = find_task_locked(id);
  if(!task || task->op != TASK_UPLOAD || !task_is_active(task)) {
    task = NULL;
  }
  pthread_mutex_unlock(&g_tasks_lock);
  return task;
}

static int
check_upload_manifest_space(const char *base, const char *rels,
                            const char *sizes, unsigned long long fallback_total,
                            char *error, size_t error_size,
                            char *code, size_t code_size,
                            char *arg, size_t arg_size) {
  char *rels_copy = NULL;
  char *sizes_copy = NULL;
  char *rel;
  char *size_text;
  char *rel_save;
  char *size_save;
  unsigned long long available;
  int ret = -1;

  if(!rels || !sizes) {
    return check_target_space(base, fallback_total, error, error_size,
                              code, code_size, arg, arg_size);
  }
  if(target_available_space(base, &available)) {
    snprintf(error, error_size, "cannot read target free space");
    snprintf(code, code_size, "space_check_failed");
    snprintf(arg, arg_size, "%s", base);
    return -1;
  }
  if(!(rels_copy = strdup(rels)) || !(sizes_copy = strdup(sizes))) {
    errno = ENOMEM;
    goto done;
  }

  rel = strtok_r(rels_copy, "\n", &rel_save);
  size_text = strtok_r(sizes_copy, "\n", &size_save);
  while(rel || size_text) {
    char target[PATH_MAX];
    unsigned long long size;

    if(!rel || !size_text || path_join_relative(target, sizeof(target), base, rel)) {
      snprintf(error, error_size, "invalid path");
      snprintf(code, code_size, "invalid_path");
      snprintf(arg, arg_size, "%s", rel ? rel : "");
      errno = EINVAL;
      goto done;
    }

    size = strtoull(size_text, NULL, 10);
    if(available < size) {
      snprintf(error, error_size,
               "not enough target space, required %llu bytes, available %llu bytes",
               size, available);
      snprintf(code, code_size, "no_space");
      snprintf(arg, arg_size, "%llu,%llu", size, available);
      errno = ENOSPC;
      goto done;
    }

    available -= size;

    rel = strtok_r(NULL, "\n", &rel_save);
    size_text = strtok_r(NULL, "\n", &size_save);
  }

  ret = 0;

done:
  free(rels_copy);
  free(sizes_copy);
  return ret;
}

enum MHD_Result
api_upload_prepare(struct MHD_Connection *conn, const char *body,
                   size_t body_size) {
  char *path = fs_path_value(body_form_value(body, body_size, "path"));
  char *src = body_form_value(body, body_size, "src");
  char *total_text = body_form_value(body, body_size, "total");
  char *count_text = body_form_value(body, body_size, "count");
  char *rels = body_form_value(body, body_size, "rels");
  char *sizes = body_form_value(body, body_size, "sizes");
  char *overwrite = body_form_value(body, body_size, "overwrite");
  file_task_t *task = calloc(1, sizeof(*task));
  strbuf_t b = {0};
  unsigned long long total = total_text ? strtoull(total_text, NULL, 10) : 0;
  size_t count = count_text ? (size_t)strtoull(count_text, NULL, 10) : 0;
  char error[128] = {0};
  char code[64] = {0};
  char arg[PATH_MAX + 96] = {0};

  if(!task) {
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!path || !src || !count) {
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(check_upload_manifest_space(path, rels, sizes, total, error, sizeof(error),
                                 code, sizeof(code), arg, sizeof(arg))) {
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error_detail(conn,
                                  errno == ENOSPC ? MHD_HTTP_INSUFFICIENT_STORAGE :
                                  MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  error[0] ? error : NULL,
                                  code[0] ? code : NULL,
                                  arg[0] ? arg : NULL);
  }

  task->op = TASK_UPLOAD;
  task->state = TASK_RUNNING;
  task->src_count = count;
  task->file_count = count;
  task->total = total;
  snprintf(task->src, sizeof(task->src), "%s", src);
  snprintf(task->dst, sizeof(task->dst), "%s", path);
  snprintf(task->current, sizeof(task->current), "%s", src);
  task->created_at = time(NULL);
  task->updated_at = task->created_at;

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  free(path); free(src); free(total_text); free(count_text);
  free(rels); free(sizes); free(overwrite);
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_upload_finish(struct MHD_Connection *conn) {
  file_task_t *task = upload_task_from_conn(conn);

  if(!task) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "active task not found");
  }
  if(task_cancel_requested(task)) {
    finish_upload_task(task, TASK_CANCELED,
                       task->current[0] ? task->current : task->src,
                       "canceled");
    return send_json_error(conn, MHD_HTTP_CONFLICT, "canceled");
  }
  finish_upload_task(task, TASK_DONE,
                     task->current[0] ? task->current : task->src, NULL);
  return send_json_ok(conn);
}

int
filemgr_upload_begin(struct MHD_Connection *conn, void **upload_ctx) {
  upload_context_t *ctx;
  char *base = fs_path_value(request_value(conn, "X-WFM-Dir", "X-WFM-Path"));
  char *rel_header = request_value(conn, "X-WFM-Rel", "rel");
  char *name = request_value(conn, "X-WFM-Name", "name");
  char *overwrite = request_value(conn, "X-WFM-Overwrite", "overwrite");
  char *size_text = request_value(conn, "X-WFM-Size", "size");
  const char *rel_raw = rel_header ? rel_header : (name ? name : NULL);
  char *rel = rel_raw ? fs_path_value((char *)rel_raw) : NULL;
  /* ⚠️ 这里**不能**把 rel_header / name 一起 free —— fs_path_value() 在常见
   * 路径上**原样返回入参**（只有路径含非 UTF-8 原始字节时才另 malloc 一份），
   * 所以 rel 通常就**别名** rel_header 或 name 本身。释放按"二选一"来：
   *   free(rel)                       -> 处理被选中的那个（fail: 处）
   *   if(rel_raw != name) free(name)  -> 补上落选的那个
   * 两条分支都不漏。2026-10-10 曾把这里"修"成 `free(rel_header); free(name);`
   * —— 那就是双重释放（rel 与 rel_header 同址），上传必崩。
   * ⚠️ 仅当走"另 malloc"那条窄分支时，原串才漏一条；属于既有的、极窄的遗留
   * 问题，不在本次范围。
   * .build/chk_param_ownership.py 已认识这个别名模式，不会误报。 */
  file_task_t *task = upload_task_from_conn(conn);
  char **checked_dirs = NULL;
  size_t checked_dir_count = 0;
  struct stat st;
  char error[128] = {0};
  char code[64] = {0};
  char arg[PATH_MAX + 96] = {0};
  int n;

  *upload_ctx = NULL;
  if(!(ctx = calloc(1, sizeof(*ctx)))) {
    free(base); free(rel); free(overwrite); free(size_text);
    errno = ENOMEM;
    return -1;
  }
  ctx->fd = -1;
  ctx->stage = "preparing upload";
  *upload_ctx = ctx;

  if(size_text) {
    ctx->expected = strtoull(size_text, NULL, 10);
  }
  ctx->task = task;
  if(task) {
    pthread_mutex_lock(&g_tasks_lock);
    task->active_streams++;
    pthread_mutex_unlock(&g_tasks_lock);
  }
  if(task && task_cancel_requested(task)) {
    ctx->failed = 1;
    ctx->error = ECANCELED;
    goto fail;
  }
  ctx->stage = "validating target path";
  if(!base || !rel || path_join_relative(ctx->target, sizeof(ctx->target),
                                         base, rel)) {
    ctx->failed = 1;
    ctx->error = errno ? errno : EINVAL;
    goto fail;
  }
  ctx->stage = "creating target folders";
  if(ensure_parent_dirs(base, rel)) {
    ctx->failed = 1;
    ctx->error = errno ? errno : EACCES;
    goto fail;
  }
  ctx->stage = "checking target path";
  if(!lstat(ctx->target, &st)) {
    if(S_ISDIR(st.st_mode) || !overwrite || strcmp(overwrite, "1")) {
      ctx->failed = 1;
      ctx->error = S_ISDIR(st.st_mode) ? EISDIR : EEXIST;
      /* 2026-10-07：原来这里不写 error_message ⇒ 前端只能显示一串「HTTP 409」，
         用户不知道到底为什么传不上去（真机原话：「必须手动先删掉 PS5 上的同名
         文件才能传」）。带上原因，前端才能照实转达并给出覆盖/重命名的选择。 */
      snprintf(ctx->error_message, sizeof(ctx->error_message), "%s",
               S_ISDIR(st.st_mode)
                 ? "target is an existing directory"
                 : "target file already exists");
      goto fail;
    }
  } else if(errno != ENOENT) {
    ctx->failed = 1;
    ctx->error = errno;
    goto fail;
  }
  ctx->stage = "checking target permissions";
  if(check_target_writable(ctx->target, &checked_dirs, &checked_dir_count,
                           error, sizeof(error), code, sizeof(code),
                           arg, sizeof(arg))) {
    snprintf(ctx->error_message, sizeof(ctx->error_message), "%s",
             error[0] ? error : "target is not writable");
    ctx->failed = 1;
    ctx->error = errno ? errno : EACCES;
    goto fail;
  }
  ctx->stage = "checking target space";
  if(check_target_space(ctx->target, ctx->expected, error, sizeof(error),
                        code, sizeof(code), arg, sizeof(arg))) {
    snprintf(ctx->error_message, sizeof(ctx->error_message), "%s",
             error[0] ? error : "not enough target space");
    ctx->failed = 1;
    ctx->error = errno ? errno : ENOSPC;
    goto fail;
  }
  ctx->stage = "creating temporary path";
  n = snprintf(ctx->temp, sizeof(ctx->temp), "%s.nex-upload-%ld-%lld.tmp",
               ctx->target, (long)getpid(), (long long)time(NULL));
  if(n < 0 || (size_t)n >= sizeof(ctx->temp)) {
    ctx->failed = 1;
    ctx->error = ENAMETOOLONG;
    goto fail;
  }
  ctx->stage = "opening temporary file";
  ctx->fd = open(ctx->temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if(ctx->fd < 0) {
    ctx->failed = 1;
    ctx->error = errno;
    goto fail;
  }
  ctx->stage = "allocating upload buffer";
  if(!(ctx->buffer = malloc(UPLOAD_BUFFER_SIZE))) {
    ctx->failed = 1;
    ctx->error = ENOMEM;
    goto fail;
  }
  /* 两块有界缓冲让收包与写盘重叠；起不来线程就退回同步写。
     小于一块缓冲的文件开线程没有意义（一次 flush 就写完了）。 */
  if(ctx->expected >= UPLOAD_BUFFER_SIZE) upload_writer_start(ctx);

fail:
  free_paths(checked_dirs, checked_dir_count);
  free(base); free(rel);
  if(name && rel_raw != name) free(name);
  free(overwrite); free(size_text);
  if(ctx->failed) {
    finish_upload_context_error(ctx);
    if(ctx->fd >= 0) {
      close(ctx->fd);
      ctx->fd = -1;
    }
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    errno = ctx->error ? ctx->error : EIO;
    return -1;
  }
  ctx->stage = "writing file";
  upload_live_begin(ctx, ctx->expected);
  return 0;
}

int
filemgr_upload_data(void *upload_ctx, const char *data, size_t size) {
  upload_context_t *ctx = upload_ctx;

  if(!ctx || ctx->failed) {
    return -1;
  }
  if(ctx->task && task_cancel_requested(ctx->task)) {
    ctx->failed = 1;
    ctx->error = ECANCELED;
    finish_upload_context_error(ctx);
    return -1;
  }
  while(size) {
    size_t space = UPLOAD_BUFFER_SIZE - ctx->buffered;
    size_t take = size < space ? size : space;

    memcpy(ctx->buffer + ctx->buffered, data, take);
    ctx->buffered += take;
    upload_live_add(ctx, take);
    data += take;
    size -= take;
    if(ctx->buffered == UPLOAD_BUFFER_SIZE && upload_flush(ctx)) {
      finish_upload_context_error(ctx);
      return -1;
    }
  }
  return 0;
}

enum MHD_Result
filemgr_upload_finish(struct MHD_Connection *conn, void *upload_ctx) {
  upload_context_t *ctx = upload_ctx;
  int ret = -1;

  if(!ctx) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(ctx->failed) {
    upload_live_end(ctx);
    finish_upload_context_error(ctx);
    errno = ctx->error ? ctx->error : EIO;
    return send_json_error_detail(conn,
                                  errno == EEXIST ? MHD_HTTP_CONFLICT :
                                  /* 同名目录也算冲突：前端据此弹「覆盖/重命名/跳过」——
                                     重命名对目录同样有效，所以走 409 而不是 500。 */
                                  errno == EISDIR ? MHD_HTTP_CONFLICT :
                                  errno == EBUSY ? MHD_HTTP_CONFLICT :
                                  errno == ECANCELED ? MHD_HTTP_CONFLICT :
                                  errno == ENOSPC ? MHD_HTTP_INSUFFICIENT_STORAGE :
                                  MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  ctx->error == ECANCELED ? "canceled" : ctx->error_message,
                                  ctx->error == ECANCELED ? NULL : "upload_failed",
                                  ctx->target[0] ? ctx->target : NULL);
  }
  ctx->stage = "writing file";
  if(ctx->buffered && upload_flush(ctx)) {
    ctx->error = ctx->error ? ctx->error : EIO;
    goto done;
  }
  /* 等写线程把最后一块落盘，否则下面的大小校验会比实际写入早 */
  ctx->error = upload_writer_stop(ctx, 1);
  if(ctx->error) {
    ctx->failed = 1;
    goto done;
  }
  if(ctx->task && task_cancel_requested(ctx->task)) {
    ctx->failed = 1;
    ctx->error = ECANCELED;
    goto done;
  }
  ctx->stage = "verifying uploaded size";
  if(ctx->expected && ctx->written != ctx->expected) {
    ctx->error = EIO;
    goto done;
  }
  ctx->stage = "setting file permissions";
  if(fchmod_0777(ctx->fd)) {
    ctx->error = errno;
    goto done;
  }
  ctx->stage = "syncing file data";
  if(fsync(ctx->fd)) {
    ctx->error = errno;
    goto done;
  }
  ctx->stage = "closing temporary file";
  if(close(ctx->fd)) {
    ctx->fd = -1;
    ctx->error = errno;
    goto done;
  }
  ctx->fd = -1;
  ctx->stage = "moving temporary file into place";
  if(rename(ctx->temp, ctx->target)) {
    ctx->error = errno;
    goto done;
  }
  if(ctx->task) {
    pthread_mutex_lock(&g_tasks_lock);
    ctx->task->upload_completed++;
    pthread_mutex_unlock(&g_tasks_lock);
  }
  ret = 0;

done:
  upload_live_end(ctx);
  upload_writer_stop(ctx, 0);
  if(ctx->fd >= 0) {
    close(ctx->fd);
    ctx->fd = -1;
  }
  if(ret) {
    upload_error_message(ctx);
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    finish_upload_context_error(ctx);
    errno = ctx->error ? ctx->error : EIO;
    return send_json_error_detail(conn,
                                  errno == ECANCELED ? MHD_HTTP_CONFLICT :
                                  MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  ctx->error == ECANCELED ? "canceled" : ctx->error_message,
                                  ctx->error == ECANCELED ? NULL : "upload_failed",
                                  ctx->target[0] ? ctx->target : NULL);
  }
  return send_json_ok(conn);
}

void
filemgr_upload_free(void *upload_ctx) {
  upload_context_t *ctx = upload_ctx;

  if(!ctx) {
    return;
  }
  upload_writer_stop(ctx, 0);
  upload_live_end(ctx);   /* finish 里已清过的话是空操作 */
  if(ctx->fd >= 0) {
    close(ctx->fd);
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    if(ctx->task && !ctx->task_done) {
      int canceled = task_cancel_requested(ctx->task);
      finish_upload_task(ctx->task, canceled ? TASK_CANCELED : TASK_FAILED,
                         ctx->target[0] ? ctx->target : ctx->task->current,
                         canceled ? "canceled" : "client disconnected");
      ctx->task_done = 1;
    }
  }
  free(ctx->buffer);
  free(ctx->writer_buffer);
  if(ctx->task) {
    pthread_mutex_lock(&g_tasks_lock);
    if(ctx->task->active_streams) {
      ctx->task->active_streams--;
    }
    pthread_mutex_unlock(&g_tasks_lock);
  }
  free(ctx);
}
