#include "filemgr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef __linux__
#include <ps5/kernel.h>
#endif

#include "filemgr_internal.h"
#include "archive_extract.h"
#include "extract_engine.h"
#include "elfldr.h"
#include "json_util.h"
#include "mango_log.h"
#include "path_util.h"
#include "pkg_info.h"
#include "pkg_installer.h"
#include "pkg_stream.h"
#include "savemgr.h"
#include "websrv.h"

#define COPY_BUFFER_SIZE (8 * 1024 * 1024)
#define COPY_PIPELINE_SLOTS 3
#define FILEMGR_AGGRESSIVE_COPY 1
#define FILEMGR_PIPELINE_COPY 1
#define SMALL_COPY_WORKERS 3
#define FILE_TASK_QUEUE_LIMIT 128
#define LARGE_FILE_THRESHOLD (256LL * 1024 * 1024)
#define TRANSFER_ALERT_THRESHOLD (10 * 60)

#ifndef __linux__
typedef struct shell_ui_uri_param {
  uint32_t size;
  uint32_t user_id;
} shell_ui_uri_param_t;

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t,
                             void *, int *);
int sceUserServiceInitialize(const int *);
int sceUserServiceGetForegroundUser(int *);

static int
navigate_to_home(void) {
  int (*initialize)(void);
  int (*launch_by_uri)(const char *, shell_ui_uri_param_t *);
  shell_ui_uri_param_t param = {.size = sizeof(param)};
  const char *module_path =
    "/system_ex/common_ex/lib/libSceShellUIUtil.sprx";
  const char *uri = "pshomeui:navigateToHome?bootCondition=psButton";
  const int priority = 256;
  int module;
  int result;

  (void)sceUserServiceInitialize(&priority);
  module = sceKernelLoadStartModule(module_path, 0, NULL, 0, NULL, NULL);
  if(module < 0) {
    printf("load libSceShellUIUtil: 0x%08X\n", (unsigned int)module);
    return module;
  }
  initialize = (void *)kernel_dynlib_dlsym(
    -1, (uint32_t)module, "sceShellUIUtilInitialize");
  launch_by_uri = (void *)kernel_dynlib_dlsym(
    -1, (uint32_t)module, "sceShellUIUtilLaunchByUri");
  if(!initialize || !launch_by_uri) {
    printf("resolve libSceShellUIUtil URI functions failed\n");
    return -1;
  }

  result = initialize();
  if(result < 0) {
    printf("sceShellUIUtilInitialize: 0x%08X\n", (unsigned int)result);
  }
  result = sceUserServiceGetForegroundUser((int *)&param.user_id);
  if(result < 0) {
    printf("sceUserServiceGetForegroundUser: 0x%08X\n",
           (unsigned int)result);
  }
  result = launch_by_uri(uri, &param);
  printf("sceShellUIUtilLaunchByUri: 0x%08X\n", (unsigned int)result);
  return result;
}
#endif

typedef struct task_completion {
  unsigned long id;
  task_op_t op;
  char src[PATH_MAX];
  size_t src_count;
  unsigned long long total;
  size_t file_count;
  time_t elapsed;
} task_completion_t;

static int ensure_copy_dir(const char *path);
static int open_copy_temp(const char *dst, char *temp, size_t temp_size);
#if FILEMGR_PIPELINE_COPY
typedef struct copy_pipeline_slot {
  char *data;
  size_t size;
  int ready;
} copy_pipeline_slot_t;

typedef struct copy_pipeline {
  file_task_t *task;
  int in;
  copy_pipeline_slot_t slots[COPY_PIPELINE_SLOTS];
  pthread_mutex_t lock;
  pthread_cond_t can_read;
  pthread_cond_t can_write;
  int read_index;
  int write_index;
  int done;
  int error;
  int error_number;
} copy_pipeline_t;
#endif

#if FILEMGR_AGGRESSIVE_COPY
typedef struct copy_job {
  char src[PATH_MAX];
  char dst[PATH_MAX];
  struct copy_job *next;
} copy_job_t;

typedef struct copy_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  copy_job_t *head;
  copy_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
} copy_queue_t;
#endif

static task_completion_t g_last_completion;
#ifndef __linux__
static pthread_cond_t g_pkg_tasks_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_pkg_worker_thread;
static int g_pkg_worker_started;
#endif

void
record_task_completion_locked(file_task_t *task, time_t completed_at) {
  if((task->op == TASK_COPY || task->op == TASK_MOVE ||
      task->op == TASK_UPLOAD || task->op == TASK_EXTRACT) &&
     completed_at - task->created_at >= TRANSFER_ALERT_THRESHOLD) {
    g_last_completion.id = task->id;
    g_last_completion.op = task->op;
    snprintf(g_last_completion.src, sizeof(g_last_completion.src), "%s", task->src);
    g_last_completion.src_count = task->src_count;
    g_last_completion.total = task->total;
    g_last_completion.file_count = task->dir_count ? task->file_count :
                                   task->op == TASK_UPLOAD ? task->file_count : 0;
    g_last_completion.elapsed = completed_at - task->created_at;
  }
}

int
ensure_parent_dirs(const char *base, const char *rel) {
  char current[PATH_MAX];
  const char *p = rel;
  size_t base_len;
  char chain[PATH_MAX];
  size_t chain_len;
  size_t i;

  if(!relative_path_safe(rel)) {
    return -1;
  }
  if(strlen(base) >= sizeof(current)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  /* base 本身可能整条不存在（首次上传到 /data/Mango/incoming 就是这种形态：
   * rel 是纯文件名时下面的循环一次都不进，旧实现直接返回 0，目录根本没建，
   * check_target_writable 随即报 "current directory is not writable"——真机
   * 存档导入「一上传就失败」的根因）。先把 base 链按 mkdir -p 语义逐级建出来。 */
  snprintf(chain, sizeof(chain), "%s", base);
  chain_len = strlen(chain);
  while(chain_len > 1 && chain[chain_len - 1] == '/') {
    chain[--chain_len] = 0;
  }
  for(i = 1; i <= chain_len; i++) {
    if(chain[i] == '/' || i == chain_len) {
      int last = chain[i];
      chain[i] = 0;
      if(ensure_copy_dir(chain)) {
        return -1;
      }
      chain[i] = (char)last;
    }
  }
  strcpy(current, base);
  base_len = strlen(current);
  while(*p) {
    const char *slash = strchr(p, '/');
    size_t len;

    if(!slash) {
      return 0;
    }
    len = (size_t)(slash - rel);
    if(base_len + (strcmp(base, "/") ? 1 : 0) + len >= sizeof(current)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(current, sizeof(current), "%s%s%.*s", base,
             strcmp(base, "/") ? "/" : "", (int)len, rel);
    if(ensure_copy_dir(current)) {
      return -1;
    }
    p = slash + 1;
  }
  return 0;
}

static void
task_set_error_code(file_task_t *task, const char *code, const char *arg) {
  pthread_mutex_lock(&g_tasks_lock);
  if(code) {
    snprintf(task->error_code, sizeof(task->error_code), "%s", code);
  }
  if(arg) {
    snprintf(task->error_arg, sizeof(task->error_arg), "%s", arg);
  }
  pthread_mutex_unlock(&g_tasks_lock);
}

void
task_set_total(file_task_t *task, unsigned long long total) {
  pthread_mutex_lock(&g_tasks_lock);
  task->total = total;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_finish_bytes(file_task_t *task, const char *current) {
  pthread_mutex_lock(&g_tasks_lock);
  task->state = TASK_RUNNING;
  if(current) {
    snprintf(task->current, sizeof(task->current), "%s", current);
  }
  if(task->total) {
    task->done = task->total;
  }
  task->speed = 0;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

/* ⚠️ OWNERSHIP: `data` must be heap memory the caller is handing over --
 * send_buffer() passes it to MHD with MHD_RESPMEM_MUST_FREE, so MHD will
 * free() it once the body is written. Callers therefore pass `strdup(...)`,
 * `malloc(...)`, or a finished strbuf's `.data`, and must NOT free it again.
 *
 * Passing a stack array or a string literal is not a leak but heap corruption:
 * free() reads the word in front of the pointer as a chunk header, and when
 * that looks plausible it links the address into a free bin and returns
 * *normally*. The damage surfaces later, as an abort on an unrelated request.
 * savemgr.c shipped seven such call sites (one of them a double free) and the
 * resulting `free(): invalid pointer` landed two requests after the actual
 * mistake -- see .build/chk_send_buffer.py, which guards this mechanically. */
enum MHD_Result
send_buffer(struct MHD_Connection *conn, unsigned int status, char *data,
            const char *mime) {
  struct MHD_Response *resp;
  enum MHD_Result ret = MHD_NO;
  size_t len = data ? strlen(data) : 0;

  if((resp = MHD_create_response_from_buffer(len, data ? data : "",
                                             data ? MHD_RESPMEM_MUST_FREE :
                                                    MHD_RESPMEM_PERSISTENT))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else {
    free(data);
  }

  return ret;
}

enum MHD_Result
send_json_ok(struct MHD_Connection *conn) {
  return send_buffer(conn, MHD_HTTP_OK, strdup("{\"ok\":true}"),
                     "application/json");
}

static const char *
api_error_code(const char *msg) {
  if(!msg) return "system_error";
  if(!strcmp(msg, "another task is running")) return "active_task";
  if(!strcmp(msg, "active task not found")) return "active_task_not_found";
  if(!strcmp(msg, "source and destination are the same")) return "source_destination_same";
  if(!strcmp(msg, "destination is inside source directory")) return "destination_inside_source";
  if(!strcmp(msg, "invalid path")) return "invalid_path";
  if(!strcmp(msg, "file not found")) return "file_not_found";
  if(!strcmp(msg, "invalid method")) return "invalid_method";
  if(!strcmp(msg, "unknown api")) return "unknown_api";
  if(!strcmp(msg, "out of memory")) return "out_of_memory";
  if(!strcmp(msg, "no source paths")) return "no_source_paths";
  if(!strcmp(msg, "file type is not editable")) return "text_type_not_editable";
  if(!strcmp(msg, "text file is too large")) return "text_file_too_large";
  if(!strcmp(msg, "file is not valid UTF-8")) return "text_invalid_utf8";
  if(!strcmp(msg, "file changed since it was opened")) return "text_file_changed";
  if(!strcmp(msg, "text file is not writable")) return "text_file_not_writable";
  if(!strcmp(msg, "file already exists")) return "file_already_exists";
  if(!strcmp(msg, "destination must be a directory for multiple items")) {
    return "destination_must_be_directory";
  }
  return "system_error";
}

enum MHD_Result
send_json_error(struct MHD_Connection *conn, unsigned int status,
                const char *msg) {
  return send_json_error_detail(conn, status, msg, api_error_code(msg), NULL);
}

enum MHD_Result
send_json_error_detail(struct MHD_Connection *conn, unsigned int status,
                       const char *msg, const char *code, const char *arg) {
  strbuf_t b = {0};
  const char *fallback = msg ? msg : strerror(errno);

  strbuf_append(&b, "{\"ok\":false,\"error\":");
  json_escape(&b, fallback);
  strbuf_append(&b, ",\"error_code\":");
  json_escape(&b, code ? code : api_error_code(msg));
  strbuf_append(&b, ",\"error_arg\":");
  json_escape(&b, arg ? arg : fallback);
  strbuf_append(&b, "}");
  return send_buffer(conn, status, b.data, "application/json");
}

static int task_target_path(file_task_t *task, const char *src,
                            char *out, size_t size);
static int chmod_task_path(file_task_t *task, const char *path,
                           unsigned int mode, int recursive);

static int count_path_bytes_sync(file_task_t *task, const char *path,
                                 const char *display,
                                 unsigned long long *total,
                                 size_t *file_count, size_t *dir_count);

static int
count_dir_bytes_sync(file_task_t *task, const char *path, const char *display,
                     unsigned long long *total, size_t *file_count,
                     size_t *dir_count) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int ret = -1;

  if(!dir) {
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_path_bytes_sync(task, child, display ? display_child : NULL,
                             total, file_count, dir_count)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
count_path_bytes_sync(file_task_t *task, const char *path, const char *display,
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
  struct stat st;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    if(dir_count) (*dir_count)++;
    return count_dir_bytes_sync(task, path, display, total, file_count,
                                dir_count);
  }
  if(S_ISREG(st.st_mode)) {
    *total += (unsigned long long)st.st_size;
    if(file_count) (*file_count)++;
  }
  return 0;
}

#if FILEMGR_AGGRESSIVE_COPY
typedef struct count_job {
  char path[PATH_MAX];
  char display[PATH_MAX];
  int has_display;
  struct count_job *next;
} count_job_t;

typedef struct count_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  count_job_t *head;
  count_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
  unsigned long long total;
  size_t file_count;
  size_t dir_count;
} count_queue_t;

static void
count_queue_add(count_queue_t *queue, unsigned long long total, size_t file_count,
                size_t dir_count) {
  pthread_mutex_lock(&queue->lock);
  queue->total += total;
  queue->file_count += file_count;
  queue->dir_count += dir_count;
  pthread_mutex_unlock(&queue->lock);
}

static void *
count_queue_worker(void *arg) {
  count_queue_t *queue = arg;

  for(;;) {
    count_job_t *job;
    unsigned long long total = 0;
    size_t file_count = 0;
    size_t dir_count = 0;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = count_path_bytes_sync(queue->task, job->path,
                                job->has_display ? job->display : NULL,
                                &total, &file_count, &dir_count);
    if(!ret) {
      count_queue_add(queue, total, file_count, dir_count);
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
count_queue_init(count_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(task_thread_create(&queue->workers[i], count_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
count_queue_enqueue(count_queue_t *queue, const char *path, const char *display) {
  count_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->path, sizeof(job->path), "%s", path);
  if(display) {
    snprintf(job->display, sizeof(job->display), "%s", display);
    job->has_display = 1;
  }

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
count_queue_finish(count_queue_t *queue, int abort_pending) {
  count_job_t *job;
  int ret = abort_pending ? -1 : 0;
  int i;

  pthread_mutex_lock(&queue->lock);
  while(!abort_pending && !queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  queue->stopping = 1;
  if(abort_pending || ret) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);

  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
count_path_bytes(file_task_t *task, const char *path, const char *display,
                 unsigned long long *total, size_t *file_count,
                 size_t *dir_count) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  int ret = -1;
  int queue_finished = 0;
  count_queue_t queue;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    return count_path_bytes_sync(task, path, display, total, file_count,
                                 dir_count);
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(dir_count) (*dir_count)++;
  if(count_queue_init(&queue, task)) {
    return -1;
  }
  if(!(dir = opendir(path))) {
    count_queue_finish(&queue, 1);
    return -1;
  }

  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      errno = ECANCELED;
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_queue_enqueue(&queue, child, display ? display_child : NULL)) {
      goto done;
    }
  }

  ret = count_queue_finish(&queue, 0);
  queue_finished = 1;
  if(!ret) {
    *total += queue.total;
    if(file_count) {
      *file_count += queue.file_count;
    }
    if(dir_count) {
      *dir_count += queue.dir_count;
    }
  }
done:
  closedir(dir);
  if(ret && !queue_finished) {
    count_queue_finish(&queue, 1);
  }
  return ret;
}
#else
#define count_path_bytes count_path_bytes_sync
#endif

int
count_task_path_bytes(file_task_t *task, const char *path, const char *display,
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
  return count_path_bytes(task, path, display, total, file_count, dir_count);
}

static int
open_copy_temp(const char *dst, char *temp, size_t temp_size) {
  char parent[PATH_MAX];
  char name[96];
  int attempt;

  if(path_dirname(dst, parent, sizeof(parent))) {
    return -1;
  }
  for(attempt = 0; attempt < 32; attempt++) {
    int fd;

    snprintf(name, sizeof(name), ".nex-copy-%ld-%lld-%d.tmp",
             (long)getpid(), (long long)time(NULL), attempt);
    if(path_join(temp, temp_size, parent, name)) {
      return -1;
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd >= 0) {
      return fd;
    }
    if(errno != EEXIST) {
      return -1;
    }
  }
  errno = EEXIST;
  return -1;
}

static int
copy_file_buffered(file_task_t *task, const char *src, const char *dst) {
  char *buf = NULL;
  char temp[PATH_MAX] = {0};
  int in = -1;
  int out = -1;
  int ret = -1;
  ssize_t n;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
    goto done;
  }
  if(!(buf = malloc(COPY_BUFFER_SIZE))) {
    errno = ENOMEM;
    goto done;
  }

  while((n = read(in, buf, COPY_BUFFER_SIZE)) > 0) {
    ssize_t left = n;
    char *p = buf;
    if(task_cancel_requested(task)) {
      errno = ECANCELED;
      goto done;
    }
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w <= 0) {
        goto done;
      }
      p += w;
      left -= w;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)w, NULL);
    }
  }
  if(n < 0) {
    goto done;
  }
  if(fchmod_0777(out)) {
    goto done;
  }
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    goto done;
  }
  ret = 0;

done:
  free(buf);
  if(in >= 0) close(in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
  return ret;
}

#if FILEMGR_PIPELINE_COPY
static void
pipeline_fail_locked(copy_pipeline_t *p, int error) {
  p->error = 1;
  p->done = 1;
  p->error_number = error ? error : EIO;
  pthread_cond_broadcast(&p->can_read);
  pthread_cond_broadcast(&p->can_write);
}

static void *
copy_pipeline_reader(void *arg) {
  copy_pipeline_t *p = arg;

  for(;;) {
    int slot;
    ssize_t n;

    pthread_mutex_lock(&p->lock);
    while(!p->error && !p->done && p->slots[p->read_index].ready) {
      pthread_cond_wait(&p->can_read, &p->lock);
    }
    if(p->error || p->done || task_cancel_requested(p->task)) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
      pthread_mutex_unlock(&p->lock);
      return NULL;
    }
    slot = p->read_index;
    p->read_index = (p->read_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_mutex_unlock(&p->lock);

    n = read(p->in, p->slots[slot].data, COPY_BUFFER_SIZE);

    pthread_mutex_lock(&p->lock);
    if(n < 0) {
      pipeline_fail_locked(p, errno);
    } else if(n == 0) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
    } else {
      p->slots[slot].size = (size_t)n;
      p->slots[slot].ready = 1;
      pthread_cond_signal(&p->can_write);
    }
    pthread_mutex_unlock(&p->lock);
  }
}

static int
copy_file_pipeline(file_task_t *task, const char *src, const char *dst) {
  copy_pipeline_t p;
  pthread_t reader;
  char temp[PATH_MAX] = {0};
  int out = -1;
  int ret = -1;
  int reader_started = 0;
  int lock_ready = 0;
  int can_read_ready = 0;
  int can_write_ready = 0;
  int i;

  memset(&p, 0, sizeof(p));
  p.task = task;
  p.in = -1;
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((p.in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
    goto done;
  }
  if(pthread_mutex_init(&p.lock, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  lock_ready = 1;
  if(pthread_cond_init(&p.can_read, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_read_ready = 1;
  if(pthread_cond_init(&p.can_write, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_write_ready = 1;
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    if(posix_memalign((void **)&p.slots[i].data, 4096, COPY_BUFFER_SIZE)) {
      errno = ENOMEM;
      goto done;
    }
  }
  if(pthread_create(&reader, NULL, copy_pipeline_reader, &p)) {
    errno = EAGAIN;
    goto done;
  }
  reader_started = 1;

  for(;;) {
    int slot;
    char *buf;
    size_t size;
    size_t off = 0;

    pthread_mutex_lock(&p.lock);
    while(!p.error && !p.done && !p.slots[p.write_index].ready) {
      pthread_cond_wait(&p.can_write, &p.lock);
    }
    if(p.error) {
      errno = p.error_number;
      pthread_mutex_unlock(&p.lock);
      goto done;
    }
    if(p.done && !p.slots[p.write_index].ready) {
      pthread_mutex_unlock(&p.lock);
      break;
    }
    slot = p.write_index;
    buf = p.slots[slot].data;
    size = p.slots[slot].size;
    pthread_mutex_unlock(&p.lock);

    while(off < size) {
      ssize_t n;
      if(task_cancel_requested(task)) {
        errno = ECANCELED;
        goto done;
      }
      n = write(out, buf + off, size - off);
      if(n <= 0) {
        if(!n) errno = EIO;
        goto done;
      }
      off += (size_t)n;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)n, NULL);
    }

    pthread_mutex_lock(&p.lock);
    p.slots[slot].ready = 0;
    p.write_index = (p.write_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_cond_signal(&p.can_read);
    pthread_mutex_unlock(&p.lock);
  }

  if(fchmod_0777(out)) {
    goto done;
  }
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    goto done;
  }
  ret = 0;

done:
  if(reader_started) {
    pthread_mutex_lock(&p.lock);
    p.done = 1;
    p.error = 1;
    pthread_cond_broadcast(&p.can_read);
    pthread_cond_broadcast(&p.can_write);
    pthread_mutex_unlock(&p.lock);
    pthread_join(reader, NULL);
  }
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    free(p.slots[i].data);
  }
  if(can_write_ready) pthread_cond_destroy(&p.can_write);
  if(can_read_ready) pthread_cond_destroy(&p.can_read);
  if(lock_ready) pthread_mutex_destroy(&p.lock);
  if(p.in >= 0) close(p.in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
  return ret;
}
#endif

static int
copy_file(file_task_t *task, const char *src, const char *dst) {
#if FILEMGR_PIPELINE_COPY
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(st.st_size >= LARGE_FILE_THRESHOLD) {
    return copy_file_pipeline(task, src, dst);
  }
#endif
  return copy_file_buffered(task, src, dst);
}

static int copy_path(file_task_t *task, const char *src, const char *dst);
static int remove_path(file_task_t *task, const char *path);
static int check_remove_path_writable(file_task_t *task, const char *path);
static int
ensure_copy_dir(const char *path) {
  struct stat st;

  if(mkdir(path, 0777)) {
    if(errno != EEXIST) {
      return -1;
    }
    if(lstat(path, &st)) {
      return -1;
    }
    if(!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
  }
  return chmod_path_0777(path);
}

static int
check_remove_entry_writable(const char *path) {
  char parent[PATH_MAX];

  if(path_dirname(path, parent, sizeof(parent))) {
    return -1;
  }
  return mode_access(parent, W_OK | X_OK);
}

static int
check_remove_dir_writable(file_task_t *task, const char *path) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  if(mode_access(path, R_OK | W_OK | X_OK)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(!(dir = opendir(path))) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       check_remove_path_writable(task, child)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
check_remove_path_writable(file_task_t *task, const char *path) {
  struct stat st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(check_remove_entry_writable(path)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return check_remove_dir_writable(task, path);
  }
  return 0;
}

static int
finish_copied_move(file_task_t *task, const char *src) {
  int ret;

  task_finish_bytes(task, "checking source permissions");
  ret = check_remove_path_writable(task, src);
  if(!ret) {
    task_finish_bytes(task, "removing source");
    ret = remove_path(task, src);
  }
  return ret;
}

#if FILEMGR_AGGRESSIVE_COPY
static int copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                           copy_queue_t *queue);

static void *
copy_queue_worker(void *arg) {
  copy_queue_t *queue = arg;

  for(;;) {
    copy_job_t *job;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = task_cancel_requested(queue->task) ? -1 :
      copy_file_buffered(queue->task, job->src, job->dst);
    if(ret && task_cancel_requested(queue->task)) {
      errno = ECANCELED;
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      pthread_cond_broadcast(&queue->has_space);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
copy_queue_init(copy_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(task_thread_create(&queue->workers[i], copy_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
copy_queue_enqueue(copy_queue_t *queue, const char *src, const char *dst) {
  copy_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->src, sizeof(job->src), "%s", src);
  snprintf(job->dst, sizeof(job->dst), "%s", dst);

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
copy_queue_wait(copy_queue_t *queue) {
  int ret = 0;

  pthread_mutex_lock(&queue->lock);
  while(!queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  pthread_mutex_unlock(&queue->lock);
  return ret;
}

static int
copy_queue_finish(copy_queue_t *queue, int abort_pending) {
  copy_job_t *job;
  int ret = abort_pending ? -1 : copy_queue_wait(queue);
  int i;

  pthread_mutex_lock(&queue->lock);
  queue->stopping = 1;
  if(abort_pending) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);
  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  copy_queue_t queue;
  int ret;

  if(copy_queue_init(&queue, task)) {
    return -1;
  }
  ret = copy_dir_queued(task, src, dst, &queue);
  if(copy_queue_finish(&queue, ret)) {
    ret = -1;
  }
  return ret;
}

static int
copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                copy_queue_t *queue) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((entry = readdir(dir))) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(from, sizeof(from), src, entry->d_name) ||
       path_join(to, sizeof(to), dst, entry->d_name)) {
      goto done;
    }
    if(lstat(from, &st)) {
      goto done;
    }
    if(S_ISDIR(st.st_mode)) {
      if(copy_dir_queued(task, from, to, queue)) {
        goto done;
      }
    } else if(S_ISREG(st.st_mode)) {
#if FILEMGR_PIPELINE_COPY
      if(st.st_size >= LARGE_FILE_THRESHOLD) {
        if(copy_queue_wait(queue) || copy_file_pipeline(task, from, to)) {
          goto done;
        }
      } else
#endif
      if(copy_queue_enqueue(queue, from, to)) {
        goto done;
      }
    } else {
      errno = ENOTSUP;
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}
#else
static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((entry = readdir(dir))) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(from, sizeof(from), src, entry->d_name) ||
       path_join(to, sizeof(to), dst, entry->d_name) ||
       copy_path(task, from, to)) {
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}
#endif

static int
copy_path(file_task_t *task, const char *src, const char *dst) {
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return copy_dir(task, src, dst);
  }
  if(S_ISREG(st.st_mode)) {
    return copy_file(task, src, dst);
  }
  errno = ENOTSUP;
  return -1;
}

static int
move_path(file_task_t *task, const char *src, const char *dst) {
  struct stat src_st;
  struct stat dst_st;
  int dst_exists;
  int ret;

  if(lstat(src, &src_st)) {
    return -1;
  }
  dst_exists = !lstat(dst, &dst_st);
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(dst_exists && S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src);
    }
    return ret;
  }

  ret = rename(src, dst);
  if(ret && errno == EXDEV) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src);
    }
  }
  return ret;
}

static int
remove_path(file_task_t *task, const char *path) {
  struct stat st;

  /* 每处理一个条目就 +1 —— 只有删除任务的 done 是**条目数**（分母由
     task_worker 的 TASK_DELETE 分支先数出来）。跨设备移动的兜底路径
     (finish_copied_move) 也会走到这里，而 move 的 done 是**字节**，
     混进条目数会把它的进度打乱 ⇒ 按 op 分叉，不是无条件加。
     传 0 时 task_update 不碰 done，行为与改前完全一致。 */
  task_update(task, TASK_RUNNING, path, task->op == TASK_DELETE ? 1 : 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(task_cancel_requested(task)) {
        closedir(dir);
        return -1;
      }
      if(path_join(child, sizeof(child), path, entry->d_name) ||
         remove_path(task, child)) {
        closedir(dir);
        return -1;
      }
    }
    closedir(dir);
    return rmdir(path);
  }
  return unlink(path);
}

/* Run the exact task_worker TASK_DELETE pipeline (count_path_bytes for the
 * entry-count denominator, then remove_path for the actual unlink/rmdir walk)
 * on an explicit path list. The save domain (mango_port.c save_delete_worker)
 * calls this instead of its own recursion so that save deletion and the file
 * manager's delete button share ONE proven code path — the save domain kept
 * crashing on-device for reasons the host could not reproduce, and this walk
 * is the one deletion implementation the device has verifiably survived.
 * Returns 0 on success, -1 on failure (errno ECANCELED = canceled). */
int
filemgr_delete_run(file_task_t *task, char **paths, size_t count) {
  size_t i;
  unsigned long long ignored_bytes = 0;
  int ret = 0;

  for(i = 0; i < count && !ret; i++) {
    if(count_path_bytes(task, paths[i], paths[i], &ignored_bytes,
                        &task->file_count, &task->dir_count)) {
      return -1;  /* errno preserved: ECANCELED or the real failure */
    }
  }
  task_set_total(task, task->file_count + task->dir_count);
  for(i = 0; i < count && !ret; i++) {
    ret = remove_path(task, paths[i]);
  }
  return ret ? -1 : 0;
}

static int
resolve_destination(const char *src, const char *dst, char *out, size_t size) {
  struct stat st;

  if(!stat(dst, &st) && S_ISDIR(st.st_mode)) {
    return path_join(out, size, dst, path_basename(src));
  }
  if(strlen(dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, dst);
  return 0;
}

static int
validate_task_target(const char *src, const char *target, int overwrite,
                     char *error, size_t error_size) {
  struct stat src_st;
  struct stat target_st;

  if(!strcmp(src, target)) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(lstat(src, &src_st)) {
    /* NAS 上的文件走到这里是必然的：本机根本不认识 "192.168.1.3/PS5_Games/x"
       这种路径。原来一律回 "source not found"，用户对着一个明明看得见的文件
       被告知「源不存在」—— 真正的原因（远程源要走 NAS 拉取）一个字都没说。
       判据用「不以 '/' 开头」：本地路径在本进程里恒是绝对路径。 */
    if(src[0] != '/')
      snprintf(error, error_size,
               "source not found（NAS 上的文件请用「复制到本机」）");
    else
      snprintf(error, error_size, "source not found");
    return -1;
  }
  if(S_ISDIR(src_st.st_mode)) {
    size_t src_len = strlen(src);

    while(src_len > 1 && src[src_len - 1] == '/') {
      src_len--;
    }
    if(!strncmp(src, target, src_len) &&
       (target[src_len] == 0 || target[src_len] == '/')) {
      snprintf(error, error_size, "destination is inside source directory");
      errno = EINVAL;
      return -1;
    }
  }
  if(lstat(target, &target_st)) {
    if(errno == ENOENT) {
      return 0;
    }
    snprintf(error, error_size, "cannot check destination");
    return -1;
  }
  if(src_st.st_dev == target_st.st_dev && src_st.st_ino == target_st.st_ino) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(!overwrite) {
    snprintf(error, error_size, "destination exists");
    errno = EEXIST;
    return -1;
  }
  if(S_ISDIR(src_st.st_mode) != S_ISDIR(target_st.st_mode)) {
    snprintf(error, error_size, "remove conflicting file or directory first");
    errno = EEXIST;
    return -1;
  }
  return 0;
}

static int
move_requires_space_check(const char *src, const char *target) {
  struct stat src_st;
  struct stat target_st;
  struct stat dst_dir_st;
  char parent[PATH_MAX];

  if(lstat(src, &src_st)) {
    return -1;
  }
  if(!lstat(target, &target_st)) {
    if(S_ISDIR(src_st.st_mode) && S_ISDIR(target_st.st_mode)) {
      return 1;
    }
    return src_st.st_dev != target_st.st_dev;
  }
  if(path_dirname(target, parent, sizeof(parent)) || stat(parent, &dst_dir_st)) {
    return -1;
  }
  return src_st.st_dev != dst_dir_st.st_dev;
}

static int
check_task_targets_writable(file_task_t *task, char *error, size_t error_size,
                            char *code, size_t code_size, char *arg, size_t arg_size) {
  char **checked_dirs = NULL;
  size_t checked_dir_count = 0;
  size_t i;

  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];

    if(task_cancel_requested(task)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      snprintf(error, error_size, "target path is too long");
      snprintf(code, code_size, "target_path_too_long");
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(check_target_writable(target, &checked_dirs, &checked_dir_count,
                             error, error_size, code, code_size, arg, arg_size)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
  }
  free_paths(checked_dirs, checked_dir_count);
  return 0;
}

static int
task_target_path(file_task_t *task, const char *src, char *out, size_t size) {
  if(task->src_count > 1) {
    return path_join(out, size, task->dst, path_basename(src));
  }
  if(strlen(task->dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, task->dst);
  return 0;
}

static int
request_target_path(const char *src, const char *dst, size_t src_count,
                    char *out, size_t size) {
  if(src_count > 1) {
    return path_join(out, size, dst, path_basename(src));
  }
  return resolve_destination(src, dst, out, size);
}

static enum MHD_Result
task_request_error(struct MHD_Connection *conn, file_task_t *task,
                   char **srcs, size_t src_count,
                   unsigned int status, const char *msg) {
  free_paths(srcs, src_count);
  free(task);
  return send_json_error(conn, status, msg);
}

typedef struct extract_progress_context {
  file_task_t *task;
  unsigned long long base;  /* bytes reported by the archives already done */
  unsigned long long done;  /* last absolute byte count reported */
  unsigned long long total; /* base + the current archive's declared total */
} extract_progress_context_t;

/* The engines report bytes written for the CURRENT archive only; the worker
 * loops over the (first-volume-collapsed) sources and folds them into one
 * running task total itself. Returning non-zero is the engine's abort
 * channel, so this doubles as the cancel path -- no separate callback needed.
 *
 * The denominator matters as much as the count: with no total the task centre
 * can only print "—", which is why every extraction used to look frozen even
 * while it was working. The engines learn the total during their scan pass, so
 * it is published only from MANGO_PHASE_EXTRACT onwards — during the scan it is
 * still growing and an early, small value would make the bar jump backwards. */
static int
extract_progress(int phase, uint64_t done, uint64_t total, const char *current,
                 void *arg) {
  extract_progress_context_t *ctx = arg;

  ctx->done = ctx->base + (unsigned long long)done;
  if(phase != MANGO_PHASE_SCAN && total)
    ctx->total = ctx->base + (unsigned long long)total;
  task_set_progress(ctx->task, ctx->done, ctx->total,
                    (current && current[0]) ? current : NULL);
  return task_cancel_requested(ctx->task);
}

static void *
extract_task_worker(file_task_t *task) {
  extract_progress_context_t context;
  size_t i;
  int canceled = 0;

  memset(&context, 0, sizeof(context));
  context.task = task;
  task_update(task, TASK_RUNNING, task->srcs[0], 0, NULL);

  for(i = 0; i < task->src_count; i++) {
    mango_extract_req_t req;
    mango_extract_result_t result;
    mango_err_t err;

    memset(&req, 0, sizeof(req));
    req.conflict = task->extract_overwrite ? MANGO_CONFLICT_OVERWRITE :
                                             MANGO_CONFLICT_FAIL;
    req.password = task->password ? task->password : NULL;
    req.progress = extract_progress;
    req.ctx = &context;
    /* Fresh denominator per archive: each engine reports its own byte total,
       and the worker folds them into one running task. */
    context.done = context.base;
    context.total = context.base;
    task_update(task, TASK_RUNNING, task->srcs[i], 0, NULL);
    memset(&result, 0, sizeof(result));
    err = mango_extract(task->srcs[i], task->extract_destinations[i],
                        &req, &result);
    if(err == MANGO_OK) {
      context.base = context.done;
      continue;
    }
    if(err == MANGO_ERR_CANCELLED || task_cancel_requested(task)) {
      canceled = 1;
      break;
    }
    if(err == MANGO_ERR_PASSWORD) {
      task_set_error_code(task, "archive_password_required", task->srcs[i]);
    } else if(err == MANGO_ERR_CONFLICT) {
      task_set_error_code(task, "archive_conflict", task->extract_destinations[i]);
    } else {
      /* Volume-set failures (missing first part etc.) carry the precise
         reason in the engine message; surface it verbatim. */
      task_set_error_code(task,
                          strstr(result.message, "volume") ? "archive_missing_volume" :
                          "archive_extract_failed",
                          strstr(result.message, "volume") ? result.message : NULL);
    }
    task_update(task, TASK_FAILED,
                task->current[0] ? task->current : task->srcs[i], 0,
                result.message[0] ? result.message : "archive extraction failed");
    return NULL;
  }

  if(canceled) {
    task_update(task, TASK_CANCELED,
                task->current[0] ? task->current : task->src,
                0, "canceled");
    return NULL;
  }
  {
    time_t completed_at = time(NULL);
    pthread_mutex_lock(&g_tasks_lock);
    task->upload_completed = task->src_count;
    task->state = TASK_DONE;
    if(task->total) task->done = task->total;
    else if(task->done) task->total = task->done;
    task->updated_at = completed_at;
    record_task_completion_locked(task, completed_at);
    pthread_mutex_unlock(&g_tasks_lock);
  }
  return NULL;
}

static void *
task_worker(void *arg) {
  file_task_t *task = arg;
  unsigned long long total = 0;
  unsigned long long required = 0;
  int ret = -1;

  task_update(task, TASK_RUNNING, "preparing", 0, NULL);

  if(task_cancel_requested(task)) {
    task_update(task, TASK_CANCELED, task->src, 0, "canceled");
    return NULL;
  }
  if(task->op == TASK_EXTRACT) {
    return extract_task_worker(task);
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    char error[160] = {0};
    char code[64] = {0};
    char arg[PATH_MAX + 96] = {0};

    task_update(task, TASK_RUNNING, "checking target permissions", 0, NULL);
    if(check_task_targets_writable(task, error, sizeof(error),
                                   code, sizeof(code), arg, sizeof(arg))) {
      if(errno == ECANCELED || task_cancel_requested(task)) {
        task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                    0, "canceled");
      } else {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->current[0] ? task->current : task->dst,
                    0, error[0] ? error : strerror(errno));
      }
      return NULL;
    }
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    size_t i;
    for(i = 0; i < task->src_count; i++) {
      unsigned long long before = total;
      int needs_space = task->op == TASK_COPY;
      char target[PATH_MAX];

      if(task->op == TASK_COPY || task->op == TASK_MOVE) {
        if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          return NULL;
        }
      }
      if(task->op == TASK_MOVE) {
        needs_space = move_requires_space_check(task->srcs[i], target);
        if(needs_space < 0) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          return NULL;
        }
        if(!needs_space) {
          continue;
        }
      }
      if(count_path_bytes(task, task->srcs[i], task->srcs[i], &total,
                          &task->file_count, &task->dir_count)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->srcs[i], 0, "canceled");
        } else {
          if(errno == ENAMETOOLONG) {
            task_set_error_code(task, "path_too_long", NULL);
          }
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
        }
        return NULL;
      }
      if(needs_space) {
        required += total - before;
      }
    }
    task_set_total(task, total);

    if(task->op == TASK_COPY || task->op == TASK_MOVE) {
      char error[128] = {0};
      char code[64] = {0};
      char arg[PATH_MAX] = {0};

      if(check_target_space(task->dst, required, error, sizeof(error),
                            code, sizeof(code), arg, sizeof(arg))) {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->dst, 0, error[0] ? error : strerror(errno));
        return NULL;
      }
    }
  }

  if(task->op == TASK_COPY) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = copy_path(task, task->srcs[i], target);
    }
  } else if(task->op == TASK_MOVE) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = move_path(task, task->srcs[i], target);
      if(!ret && task->src_count == 1) {
        task_update(task, TASK_RUNNING, target, total, NULL);
      }
    }
  } else if(task->op == TASK_DELETE) {
    size_t i;
    unsigned long long ignored_bytes = 0;

    /* 删除是**条目计数**型任务（前端 isCountKind(7) 按「项」显示倒数），
       所以必须先数一遍再删。原来这里直接进删除循环：没有 task_set_total、
       remove_path() 的 add_done 又恒为 0 ⇒ total 与 progress 双 0，
       进度条永远 0%，对话框上写着「0 / 0 项」—— 用户报的正是这个。
       代价是多走一遍目录，换来的是「还剩多少」这个唯一能让人安心的信息，
       对一个上万条目的游戏目录完全值得（复制路径本来也是先 count 再拷）。
       ⚠️ 用的分母是 file_count + dir_count，**不是** count_path_bytes 返回的字节数：
       remove_path() 对每个条目（文件或目录）恰好被调用一次，两个计数之和就等于
       调用总次数 ⇒ 进度天然对齐、不会超过 100%。 */
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      if(count_path_bytes(task, task->srcs[i], task->srcs[i], &ignored_bytes,
                          &task->file_count, &task->dir_count)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->srcs[i], 0, "canceled");
        } else {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
        }
        return NULL;
      }
    }
    task_set_total(task, task->file_count + task->dir_count);
    for(i = 0; i < task->src_count && !ret; i++) {
      ret = remove_path(task, task->srcs[i]);
    }
  } else if(task->op == TASK_CHMOD) {
    unsigned long long ignored_bytes = 0;
    size_t i;

    ret = 0;
    if(task->recursive) {
      for(i = 0; i < task->src_count; i++) {
        size_t files_before = task->file_count;
        size_t dirs_before = task->dir_count;

        if(count_task_path_bytes(task, task->srcs[i], task->srcs[i],
                                 &ignored_bytes, &task->file_count,
                                 &task->dir_count)) {
          ret = -1;
          break;
        }
        if(files_before == task->file_count && dirs_before == task->dir_count) {
          task->file_count++;
        }
      }
      total = task->file_count + task->dir_count;
    } else {
      total = task->src_count;
    }
    if(!ret) {
      task_set_total(task, total);
      for(i = 0; i < task->src_count && !ret; i++) {
        ret = chmod_task_path(task, task->srcs[i], task->chmod_mode,
                              task->recursive);
      }
    }
  }

  if(ret) {
    if(errno == ECANCELED || task_cancel_requested(task)) {
      task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                  0, "canceled");
    } else {
      if(task->op == TASK_CHMOD) {
        task_set_error_code(task, "chmod_failed",
                            task->current[0] ? task->current : task->src);
      }
      task_update(task, TASK_FAILED, task->current[0] ? task->current : task->src,
                  0, strerror(errno));
    }
  } else {
    time_t completed_at = time(NULL);
    pthread_mutex_lock(&g_tasks_lock);
    task->state = TASK_DONE;
    if(task->total) {
      task->done = task->total;
    }
    task->updated_at = completed_at;
    record_task_completion_locked(task, completed_at);
    pthread_mutex_unlock(&g_tasks_lock);
  }

  return NULL;
}

/* Extraction runs in-process (src/extract_engine.c + the vendored zip/rar/7z
 * leaves), so there is no helper-side task list to recover after a restart:
 * an interrupted unpack dies with the process, exactly like every other task. */

static enum MHD_Result
create_task_response(struct MHD_Connection *conn, task_op_t op,
                     char **srcs, size_t src_count, const char *dst,
                     int overwrite, unsigned int chmod_mode, int recursive) {
  file_task_t *task = calloc(1, sizeof(file_task_t));
  strbuf_t b = {0};
  struct stat st;
  size_t i;

  if(!task) {
    free_paths(srcs, src_count);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!src_count) {
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_BAD_REQUEST, "no source paths");
  }
  if(dst && src_count > 1) {
    if(stat(dst, &st) || !S_ISDIR(st.st_mode)) {
      return task_request_error(conn, task, srcs, src_count,
                                MHD_HTTP_BAD_REQUEST,
                                "destination must be a directory for multiple items");
    }
  }
  if(dst) {
    /* 目标必须是一个**本地绝对路径**。用户从 NAS 视图点「粘贴」时 dst 是
       "192.168.1.3/PS5_Games" 这种相对形状 —— 不挡的话 path_join 会照着写，
       于是在进程当前目录下真的建出一棵 "192.168.1.3/PS5_Games" 目录树：
       既不是用户要的（他要传到 NAS），又污染了本地磁盘，而且任务还报成功。
       transfer 层只读（没有写接口），所以这个方向此刻只能明确拒绝。 */
    if(dst[0] != '/') {
      return task_request_error(conn, task, srcs, src_count,
                                MHD_HTTP_BAD_REQUEST,
                                "目标必须是本地路径（暂不支持向 NAS 写入）");
    }
    for(i = 0; i < src_count; i++) {
      char target[PATH_MAX];
      char error[128] = {0};
      if(request_target_path(srcs[i], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      if(validate_task_target(srcs[i], target, overwrite, error, sizeof(error))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_CONFLICT, error[0] ? error : NULL);
      }
    }
  }

  task->op = op;
  task->state = TASK_QUEUED;
  task->chmod_mode = chmod_mode;
  task->recursive = recursive;
  task->srcs = srcs;
  task->src_count = src_count;
  snprintf(task->src, sizeof(task->src), "%s%s",
           srcs[0], src_count > 1 ? " ..." : "");
  if(dst) {
    if(src_count == 1) {
      char target[PATH_MAX];
      if(request_target_path(srcs[0], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      snprintf(task->dst, sizeof(task->dst), "%s", target);
    } else {
      snprintf(task->dst, sizeof(task->dst), "%s", dst);
    }
  }
  task->created_at = time(NULL);
  task->updated_at = task->created_at;

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_CONFLICT, "another task is running");
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  if(task_thread_create(&task->thread, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }

  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* Unified request parameter reader: the MANGO front-end posts either JSON
 * (application/json via apiJson) or URL-encoded form (application/x-www-form-
 * urlencoded via apiPost). This tries JSON first, then form, then the GET query
 * string, so any endpoint accepts either encoding. */
char *
req_param(struct MHD_Connection *conn, const char *body, size_t body_size,
          const char *key) {
  char *v = NULL;

  if(body && body_size && body[0] == '{') {
    v = body_json_value(body, body_size, key);
  }
  if(!v) {
    v = body_form_value(body, body_size, key);
  }
  if(!v) {
    v = query_value(conn, key);
  }
  return v;
}

/* Map an owendswang task_op_t to the MANGO front-end's task_kind_t numbering
 * (0 fetch, 1 extract, 2 upload, 3 install, 4 backup, 5 restore, 6 fPKG copy,
 * 7 delete). Copy/move/chmod have no dedicated MANGO kind, so they reuse 6. */
static int
task_kind_of(task_op_t op) {
  switch(op) {
    case TASK_UPLOAD: return 2;
    case TASK_DELETE: return 7;
    case TASK_EXTRACT: return 1;
    case TASK_PKG_INSTALL: return 3;
    case TASK_DOWNLOAD: return 0;
    case TASK_COPY: return 6;
    case TASK_MOVE: return 6;
    case TASK_CHMOD: return 6;
    default: return 6;
  }
}

/* MANGO task_state_t numbering (QUEUED=0, RUNNING=1, PAUSED=2, DONE=3, FAILED=4,
 * CANCELLED=5). Only the save-manager tracker consumes the numeric state. */
static int
task_num_state(task_state_t s) {
  switch(s) {
    case TASK_QUEUED: return 0;
    case TASK_RUNNING: return 1;
    case TASK_DONE: return 3;
    case TASK_FAILED: return 4;
    case TASK_CANCELED: return 5;
    default: return 0;
  }
}

enum MHD_Result
api_tasks(struct MHD_Connection *conn, const char *body, size_t body_size) {
  strbuf_t b = {0};
  file_task_t *task;
  int first = 1;

  (void)body;
  (void)body_size;
  strbuf_append(&b, "{\"ok\":true,\"tasks\":[");
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    const char *note = task->error[0] ? task->error : task->current;
    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_printf(&b, "{\"id\":%lu,\"kind\":%d,\"state\":%d,\"state_name\":\"%s\",",
                  task->id, task_kind_of(task->op),
                  task_num_state(task->state), task_state_name(task->state));
    strbuf_append(&b, "\"label\":");
    json_escape(&b, task->current[0] ? task->current : task->src);
    strbuf_printf(&b, ",\"note\":");
    json_escape(&b, note);
    /* speed/eta 由 task_update() 已经在采样（见 task.c），以前只差把它们吐出去：
       「传输速度 / 已用时 / 剩余时间」正是任务窗口缺的那三格。speed 的单位与
       任务口径一致（字节型任务 = 字节/秒，条目型 = 条目/秒），前端按 kind 挑单位。 */
    strbuf_printf(&b, ",\"total\":%llu,\"progress\":%llu,\"speed\":%llu,"
                      "\"eta\":%llu,\"created_at\":%lld}",
                  task->total, task->done, task->speed, task->eta,
                  (long long)task->created_at);
    if(!task_is_active(task)) task->reported = 1;
  }
  remove_finished_tasks_locked();
  pthread_mutex_unlock(&g_tasks_lock);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_cancel(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *idstr = req_param(conn, body, body_size, "id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task;
  int found = 0;

  free(idstr);
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    if(task->id == id && task->op != TASK_PKG_INSTALL && task_is_active(task)) {
      task->cancel_requested = 1;
      if(task->op == TASK_DOWNLOAD && task->state == TASK_QUEUED) {
        task->state = TASK_CANCELED;
        snprintf(task->error, sizeof(task->error), "canceled");
      }
      task->updated_at = time(NULL);
      found = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_tasks_lock);

  return found ? send_json_ok(conn) :
                 send_json_error(conn, MHD_HTTP_NOT_FOUND, "active task not found");
}

static void *
stop_websrv_later(void *arg) {
  (void)arg;

  /* 先让 /api/shutdown 的响应发出去：MHD 停服会 join 所有在飞连接，抢在响应
     之前停会把那个 200 掐掉（前端就报"关闭失败"）。 */
  usleep(250000);
  mango_log("shutdown: 250ms elapsed, stopping webserver now");

  /* ★ 唯一真正让「关闭进程」生效的一步，必须排在最前。
     ⚠️ 老顺序是 navigate_to_home() 在前、websrv_stop() 在后（v1.0.0 起没动过），
     而 navigate_to_home 只对真机编译（__linux__ 下整段被跳过）⇒ hosttest 结构上
     **不可能**覆盖这个顺序。真机上它要走 ShellUI：LoadStartModule 系统模块 +
     sceUserServiceInitialize + LaunchByUri 等应答，任一步长时间不返回，
     websrv_stop() 就永远执行不到 —— 进程活着、页面照常响应，而前端早已弹出
     "进程已关闭"（它发完请求就自己宣布，从不核实）。用户看到的就是
     "结束了进程，进程又自动启动了"。不变量由 .build/chk_shutdown_order.py 守。 */
  websrv_stop();

#ifndef __linux__
  /* 回桌面是**装饰**。排在停服之后：它卡住也影响不了停机（进程照常退出，只是
     不会自动跳回桌面）。main 退出前会留 1 秒有界窗口给它发完那个 URI。 */
  (void)navigate_to_home();
#endif
  mango_log("shutdown: stop armed, thread done");
  return NULL;
}

enum MHD_Result
api_exit(struct MHD_Connection *conn, const char *body, size_t body_size) {
  pthread_t thread;
  int err;

  (void)body; (void)body_size;

  err = pthread_create(&thread, NULL, stop_websrv_later, NULL);
  if(err) {
    /* ⚠️ 老代码把 pthread_create 的失败**丢掉了**：线程起不来 ⇒ 没人会去调
       websrv_stop()，而这里照样回 {"ok":true}，前端于是弹出"进程已关闭"。
       关停请求是最不该被静默吞掉的东西，所以同步把停机标志置上（只
       shutdown(listen fd)，不碰 MHD，不会和当前这条连接互锁），并记账。 */
    mango_log("shutdown: pthread_create failed (%d); stopping inline", err);
    websrv_stop();
  } else {
    pthread_detach(thread);
    mango_log("shutdown: requested; stop armed in ~250ms");
  }
  return send_json_ok(conn);
}

enum MHD_Result
api_copy(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = req_param(conn, body, body_size, "paths");
  char *dst = fs_path_value(req_param(conn, body, body_size, "dst"));
  char *overwrite = req_param(conn, body, body_size, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(count == 1) {
    char target[PATH_MAX];
    if(request_target_path(paths[0], dst, count, target, sizeof(target)) ||
       !strcmp(paths[0], target)) {
      free_paths(paths, count); free(paths_raw); free(dst); free(overwrite);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "source and destination are the same");
    }
  }
  ret = create_task_response(conn, TASK_COPY, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"), 0, 0);
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

enum MHD_Result
api_move(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = req_param(conn, body, body_size, "paths");
  char *dst = fs_path_value(req_param(conn, body, body_size, "dst"));
  char *overwrite = req_param(conn, body, body_size, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  ret = create_task_response(conn, TASK_MOVE, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"), 0, 0);
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

enum MHD_Result
api_delete(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = req_param(conn, body, body_size, "paths");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  for(size_t i = 0; i < count; i++) {
    if(!strcmp(paths[i], "/")) {
      free_paths(paths, count); free(paths_raw);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
    }
  }
  ret = create_task_response(conn, TASK_DELETE, paths, count, NULL, 0, 0, 0);
  free(paths_raw);
  return ret;
}

enum MHD_Result
api_rename(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  char *name = fs_path_value(req_param(conn, body, body_size, "name"));
  char parent[PATH_MAX];
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_dirname(path, parent, sizeof(parent)) ||
     path_join(target, sizeof(target), parent, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }

  errno = 0;
  ret = rename(path, target);
  {
    int err = errno;
    free(path); free(name);
    return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 err ? strerror(err) : "rename failed")
               : send_json_ok(conn);
  }
}

enum MHD_Result
api_mkdir(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  char *name = fs_path_value(req_param(conn, body, body_size, "name"));
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_join(target, sizeof(target), path, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  errno = 0;
  ret = mkdir(target, 0777);
  if(!ret) {
    ret = chmod_path_0777(target);
  }
  {
    int err = errno;
    free(path); free(name);
    return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 err ? strerror(err) : "mkdir failed")
               : send_json_ok(conn);
  }
}

static int
parse_permission_mode(const char *text, unsigned int *mode) {
  size_t i;

  if(!text || strlen(text) != 4 || text[0] != '0') {
    return -1;
  }
  for(i = 1; i < 4; i++) {
    if(text[i] < '0' || text[i] > '7') {
      return -1;
    }
  }
  *mode = (unsigned int)((text[1] - '0') << 6) |
          (unsigned int)((text[2] - '0') << 3) |
          (unsigned int)(text[3] - '0');
  return 0;
}

static int
chmod_task_path(file_task_t *task, const char *path,
                unsigned int mode, int recursive) {
  struct stat st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  task_update(task, TASK_RUNNING, path, 0, NULL);
  if(lstat(path, &st)) {
    return -1;
  }
  if(recursive && S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;

    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      int error;

      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(task_cancel_requested(task) ||
         path_join(child, sizeof(child), path, entry->d_name) ||
         lstat(child, &st)) {
        error = errno;
        closedir(dir);
        errno = error;
        return -1;
      }
      /* A symlink has no portable mode of its own; never change its target. */
      if(!S_ISLNK(st.st_mode) && chmod_task_path(task, child, mode, 1)) {
        error = errno;
        closedir(dir);
        errno = error;
        return -1;
      }
    }
    closedir(dir);
  }
  task_update(task, TASK_RUNNING, path, 0, NULL);
  if(chmod_path_mode(path, mode)) {
    return -1;
  }
  task_update(task, TASK_RUNNING, path, 1, NULL);
  return 0;
}

static enum MHD_Result
api_launch_elf(struct MHD_Connection *conn, const char *body,
               size_t body_size) {
  char *path = fs_path_value(body_form_value(body, body_size, "path"));
  const char *extension = path ? strrchr(path, '.') : NULL;
  struct stat st;
  enum MHD_Result result;

  if(!path || path[0] != '/' || !extension || strcasecmp(extension, ".elf")) {
    free(path);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "file is not an ELF payload",
                                  "elf_type_invalid", NULL);
  }
  if(lstat(path, &st) || !S_ISREG(st.st_mode)) {
    result = send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                    "ELF payload not found",
                                    "file_not_found", path);
    free(path);
    return result;
  }
  if(has_active_task()) {
    free(path);
    return send_json_error(conn, MHD_HTTP_CONFLICT,
                           "another task is running");
  }

  if(elfldr_send(path)) {
    int error = errno;
    result = send_json_error_detail(
      conn, error == ENOTSUP ? MHD_HTTP_NOT_IMPLEMENTED :
            error == ENOEXEC ? MHD_HTTP_BAD_REQUEST :
                               MHD_HTTP_INTERNAL_SERVER_ERROR,
      error == ENOTSUP ? "ELF launching is only available on PS5" :
      error == ENOEXEC ? "file is not a valid ELF payload" :
                         "could not send ELF payload to elfldr",
      error == ENOTSUP ? "elf_launch_unsupported" :
      error == ENOEXEC ? "elf_invalid" : "elf_launch_failed", path);
  } else {
    result = send_json_ok(conn);
  }
  free(path);
  return result;
}

/* First present key wins. The MANGO extract dialog and owendswang's own form
 * name the same fields differently; accepting both keeps the route working for
 * either front-end instead of silently ignoring the user's choice. */
static char *
req_param_alias(struct MHD_Connection *conn, const char *body, size_t body_size,
                const char *key1, const char *key2) {
  char *value = req_param(conn, body, body_size, key1);
  return value ? value : req_param(conn, body, body_size, key2);
}

enum MHD_Result
api_extract(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = req_param(conn, body, body_size, "path");
  /* MANGO names the fields dst_dir / subdir / conflict; owendswang's form used
   * destination / separate / overwrite. Read both.
   * ⚠️ 2026-10-01 — reading only the owendswang names meant the folder picked
   * in the 解压到 picker was DISCARDED and every archive was unpacked into its
   * own parent directory with no subfolder. A game therefore landed as a heap
   * of loose files instead of one mountable PPSA#####-app directory, which is
   * the "解压完的游戏不能玩" report. Both halves of the contract are fixed here
   * and in submitExtract() in assets/index.html. */
  char *destination = fs_path_value(req_param_alias(conn, body, body_size,
                                                    "dst_dir", "destination"));
  char *separate_text = req_param_alias(conn, body, body_size,
                                        "subdir", "separate");
  char *overwrite_text = req_param(conn, body, body_size, "overwrite");
  char *conflict_text = req_param(conn, body, body_size, "conflict");
  char *password = req_param(conn, body, body_size, "password");
  char **paths = NULL;
  char **destinations = NULL;
  size_t count = 0;
  struct stat st;
  enum MHD_Result result;
  int destination_stat;
  int separate;
  int overwrite;
  char default_dest[PATH_MAX];

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    result = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
    goto done;
  }
  /* 分卷 RAR 归一：用户全选 part1+part2+… 或点中 .r00 时，把成员收敛成
     第一卷并去重 —— 7z 引擎只会从第一卷读起，后续卷不该当独立任务。 */
  if(archive_normalize_sources(paths, &count)) {
    result = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "out of memory");
    goto done;
  }
  /* MANGO front-end only sends `path` + `password`; default the extraction
   * destination to the archive's parent directory. */
  if(!destination && count &&
     !path_dirname(paths[0], default_dest, sizeof(default_dest))) {
    destination = strdup(default_dest);
  }
  /* Default to a same-named subfolder. Scattering an archive's thousands of
     entries into a shared directory (/mnt/ext1/games) is never what someone
     unpacking a game wants, and it is unrecoverable by inspection afterwards.
     The dialog's 「以压缩包名新建子目录」 checkbox turns it off. */
  if(!separate_text) separate_text = strdup("1");
  if(!overwrite_text) overwrite_text = strdup("0");
  if(!destination || destination[0] != '/') {
    result = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid destination");
    goto done;
  }
  separate = !strcmp(separate_text, "1") ||
             !strcasecmp(separate_text, "true") ||
             !strcasecmp(separate_text, "on");
  overwrite = (overwrite_text && !strcmp(overwrite_text, "1")) ||
              (conflict_text && (!strcasecmp(conflict_text, "overwrite") ||
                                 !strcasecmp(conflict_text, "replace") ||
                                 !strcasecmp(conflict_text, "merge")));
  if(destination[0] != '/') {
    result = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
    goto done;
  }
  destination_stat = lstat(destination, &st);
  if(!destination_stat && !S_ISDIR(st.st_mode)) {
    result = send_json_error_detail(
      conn, MHD_HTTP_CONFLICT, "destination is not a directory",
      "destination_must_be_directory", destination);
    goto done;
  }
  if(destination_stat && errno != ENOENT) {
    result = send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
    goto done;
  }
  if(!(destinations = calloc(count, sizeof(*destinations)))) {
    result = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "out of memory");
    goto done;
  }
  for(size_t i = 0; i < count; i++) {
    char target[PATH_MAX];

    if(lstat(paths[i], &st) || !S_ISREG(st.st_mode)) {
      result = send_json_error_detail(
        conn, MHD_HTTP_NOT_FOUND, "archive file not found",
        "file_not_found", paths[i]);
      goto done;
    }
    if(!archive_path_supported(paths[i])) {
      if(archive_multipart_member(paths[i])) {
        /* 归一没救回来 = 第一卷真的不在旁边：直说缺什么 */
        result = send_json_error_detail(
          conn, MHD_HTTP_BAD_REQUEST,
          "multi-part archive is missing its first part",
          "archive_first_part_missing", paths[i]);
      } else {
        result = send_json_error_detail(
          conn, MHD_HTTP_BAD_REQUEST, "unsupported archive type",
          "archive_type_unsupported", paths[i]);
      }
      goto done;
    }
    if(archive_output_path(paths[i], destination, separate,
                           target, sizeof(target)) ||
       !(destinations[i] = strdup(target))) {
      result = send_json_error_detail(
        conn, errno == ENOMEM ? MHD_HTTP_INTERNAL_SERVER_ERROR :
                               MHD_HTTP_BAD_REQUEST,
        errno == ENOMEM ? "out of memory" : "target path is too long",
        errno == ENOMEM ? "out_of_memory" : "target_path_too_long",
        errno == ENOMEM ? NULL : destination);
      goto done;
    }
  }
  {
    file_task_t *task = calloc(1, sizeof(*task));
    strbuf_t response = {0};

    if(!task || (password && password[0] &&
                 !(task->password = strdup(password)))) {
      free_task(task);
      result = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "out of memory");
      goto done;
    }
    task->op = TASK_EXTRACT;
    task->state = TASK_QUEUED;
    task->srcs = paths;
    task->src_count = count;
    task->extract_destinations = destinations;
    task->extract_overwrite = overwrite;
    snprintf(task->src, sizeof(task->src), "%s%s",
             paths[0], count > 1 ? " ..." : "");
    snprintf(task->dst, sizeof(task->dst), "%s", destination);
    task->created_at = time(NULL);
    task->updated_at = task->created_at;
    paths = NULL;
    destinations = NULL;
    count = 0;

    pthread_mutex_lock(&g_tasks_lock);
    remove_finished_tasks_locked();
    if(has_active_task_locked()) {
      pthread_mutex_unlock(&g_tasks_lock);
      free_task(task);
      result = send_json_error(conn, MHD_HTTP_CONFLICT,
                               "another task is running");
      goto done;
    }
    task->id = g_next_task_id++;
    task->next = g_tasks;
    g_tasks = task;
    pthread_mutex_unlock(&g_tasks_lock);

    if(task_thread_create(&task->thread, task_worker, task)) {
      task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
    } else {
      pthread_detach(task->thread);
    }
    strbuf_printf(&response, "{\"ok\":true,\"task_id\":%lu}", task->id);
    result = send_buffer(conn, MHD_HTTP_OK, response.data, "application/json");
  }

done:
  if(password) memset(password, 0, strlen(password));
  free(paths_raw);
  free(destination);
  free(separate_text);
  free(overwrite_text);
  free(conflict_text);
  free(password);
  free_paths(paths, count);
  free_paths(destinations, count);
  return result;
}

enum MHD_Result
api_chmod(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = req_param(conn, body, body_size, "paths");
  char *mode_text = req_param(conn, body, body_size, "mode");
  char *recursive_text = req_param(conn, body, body_size, "recursive");
  char **paths = NULL;
  size_t count = 0;
  unsigned int mode;
  enum MHD_Result ret;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(mode_text); free(recursive_text);
    free_paths(paths, count);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(parse_permission_mode(mode_text, &mode)) {
    free(paths_raw); free(mode_text); free(recursive_text);
    free_paths(paths, count);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "invalid permission mode",
                                  "invalid_mode", NULL);
  }
  ret = create_task_response(conn, TASK_CHMOD, paths, count, NULL, 0, mode,
                             recursive_text && !strcmp(recursive_text, "1"));
  free(paths_raw); free(mode_text); free(recursive_text);
  return ret;
}

#ifndef __linux__
/* 与 pkg_installer.c 的 install_log 同一落点（/data/Mango/logs/mango-install.log）：
 * worker 层只记终态与句柄去向，阶段细节归 pkg_installer.c。fopen/fclose
 * 逐行落盘，任何崩溃都保得住上一行；写失败静默 —— 取证不成为依赖。 */
static void
pkg_task_log(const char *fmt, ...) {
  const char *path = getenv("WFM_INSTALL_LOG");
  char default_path[] = "/data/Mango/logs/mango-install.log";
  FILE *f;
  va_list ap;

  if(!path || !*path) path = default_path;
  f = fopen(path, "a");
  if(!f) return;
  fprintf(f, "[%llu] ", (unsigned long long)time(NULL));
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

static file_task_t *
next_pkg_task_locked(void) {
  file_task_t *task;
  file_task_t *next = NULL;

  for(task = g_tasks; task; task = task->next) {
    if(task->op == TASK_PKG_INSTALL && task->state == TASK_QUEUED &&
       (!next || task->id < next->id)) next = task;
  }
  return next;
}

static void *
pkg_task_worker(void *arg) {
  (void)arg;

  while(1) {
    file_task_t *task;
    pkg_install_handle_t *handle;
    const char *note;

    pthread_mutex_lock(&g_tasks_lock);
    while(!(task = next_pkg_task_locked())) {
      pthread_cond_wait(&g_pkg_tasks_cond, &g_tasks_lock);
    }
    task->state = TASK_RUNNING;
    /* Install progress is a PERCENTAGE, not bytes: it comes from the console's
     * own status query, which reports how much of the package IT has pulled.
     * /api/install/poll and the task centre both branch on kind 3 for this. */
    task->total = 100;
    snprintf(task->current, sizeof(task->current), "%s", task->src);
    task->updated_at = time(NULL);
    pthread_mutex_unlock(&g_tasks_lock);

  /* Delivery: the file's own path (proven) or a loopback stream uri (opt-in,
   * not yet verified on hardware). The uri is built BEFORE the install begins
   * and the publication outlives the call, because InstallByPackage is
   * asynchronous — the console pulls the bytes after we have returned.
   * A failed publish is not an error for a LOCAL file: path delivery is a
   * complete fallback. For a NAS source it IS an error — the installer cannot
   * open smb://, so "degrade to path delivery" would just hand it garbage. */
  {
    char stream_url[1088];
    int  streamed = 0;
    int  remote = pkg_src_is_remote(task->srcs[0]);

    if(task->pkg_stream &&
       (remote ? pkg_stream_prepare_nas(task->srcs[0], stream_url, sizeof(stream_url))
               : pkg_stream_prepare(task->srcs[0], stream_url, sizeof(stream_url))) == 0) {
      streamed = 1;
      pkg_task_log("task %lu delivery=loopback-stream remote=%d uri=%s", task->id,
                   remote, stream_url);
    } else if(task->pkg_stream) {
      if(remote) {
        pkg_task_log("task %lu FAILED: NAS package unreadable, no path fallback",
                     task->id);
        task_set_error_code(task, "pkg_stream_unavailable",
                            "NAS 上的包无法读取（凭据/共享/网络），流式安装未启动");
        task_update(task, TASK_FAILED, task->srcs[0], 0,
                    "NAS package unreadable; streaming install not started");
        continue;
      }
      pkg_task_log("task %lu delivery=path (stream publish failed)", task->id);
    }

    handle = pkg_install_begin_uri(task->srcs[0], streamed ? stream_url : NULL);
    if(!handle && streamed) {
      /* Nothing will ever fetch it: take the offer down now instead of
       * waiting out the idle timeout. */
      pkg_stream_unpublish();
    }
  }
  if(!handle) {
      /* Nothing was started: library load failure, Initialize failure, or
       * InstallByPackage returned an error. The reason is specific and
       * actionable (hex code in /data/Mango/logs/mango-install.log), so it goes to the
       * user verbatim. */
      note = pkg_install_last_error();
      pkg_task_log("task %lu FAILED at begin: %s", task->id,
                   note ? note : "(no reason)");
      task_set_error_code(task, "pkg_install_failed", note);
      task_update(task, TASK_FAILED, task->src, 0,
                  note ? note : "package installation failed");
      continue;
    }

    /* begin() is now terminal by contract (v1.0.3): NULL = failed to start,
     * a handle = submitted to the system installer or precheck-skipped. The
     * console installs in the background; GetInstallStatus polling is gone
     * (unreferenced status struct, crash suspect), so the task ends HERE and
     * the note tells the user where the ground truth lives. */
    note = pkg_install_note(handle);
    task_set_progress(task, 100, 100, note);
    if(pkg_install_submitted(handle)) {
      pkg_task_log("task %lu SUBMITTED note=%s", task->id,
                   note ? note : "-");
      task_update(task, TASK_DONE, task->src, 0, note);
    } else {
      pkg_task_log("task %lu FAILED note=%s", task->id,
                   note ? note : "(no reason)");
      task_set_error_code(task, "pkg_install_failed",
                          note ? note : "package installation failed");
      task_update(task, TASK_FAILED, task->src, 0,
                  note ? note : "package installation failed");
    }

    pkg_install_free(handle);
  }
  return NULL;
}

static file_task_t *
active_pkg_task_locked(const char *path) {
  file_task_t *task;

  for(task = g_tasks; task; task = task->next) {
    if(task->op == TASK_PKG_INSTALL && task_is_active(task) &&
       !strcmp(task->srcs[0], path)) return task;
  }
  return NULL;
}
#endif

static int
enqueue_pkg_tasks(char **paths, size_t count, unsigned long *ids, int stream) {
#ifdef __linux__
  (void)paths; (void)count; (void)ids; (void)stream;
  return PKG_INSTALL_UNSUPPORTED;
#else
  file_task_t **pending = calloc(count, sizeof(*pending));
  int result = 0;

  if(!pending) return -1;
  for(size_t i = 0; i < count; i++) {
    file_task_t *task = calloc(1, sizeof(*task));
    if(!task || !(task->srcs = calloc(1, sizeof(*task->srcs))) ||
       !(task->srcs[0] = strdup(paths[i]))) {
      free_task(task);
      result = -1;
      goto done;
    }
    task->op = TASK_PKG_INSTALL;
    task->state = TASK_QUEUED;
    task->pkg_stream = stream ? 1 : 0;
    task->src_count = 1;
    snprintf(task->src, sizeof(task->src), "%s", paths[i]);
    task->created_at = time(NULL);
    task->updated_at = task->created_at;
    pending[i] = task;
  }

  pthread_mutex_lock(&g_tasks_lock);
  if(!g_pkg_worker_started) {
    result = task_thread_create(&g_pkg_worker_thread, pkg_task_worker, NULL);
    if(!result) {
      pthread_detach(g_pkg_worker_thread);
      g_pkg_worker_started = 1;
    }
  }
  if(!result) {
    for(size_t i = 0; i < count; i++) {
      file_task_t *existing = active_pkg_task_locked(paths[i]);
      if(existing) {
        ids[i] = existing->id;
        free_task(pending[i]);
      } else {
        pending[i]->id = g_next_task_id++;
        ids[i] = pending[i]->id;
        pending[i]->next = g_tasks;
        g_tasks = pending[i];
      }
      pending[i] = NULL;
    }
    pthread_cond_signal(&g_pkg_tasks_cond);
  }
  pthread_mutex_unlock(&g_tasks_lock);

done:
  for(size_t i = 0; i < count; i++) free_task(pending[i]);
  free(pending);
  return result;
#endif
}

static enum MHD_Result
api_install_pkg(struct MHD_Connection *conn, const char *body,
                size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char **paths = NULL;
  unsigned long *ids = NULL;
  size_t count = 0;
  strbuf_t json = {0};
  int result;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count) ||
     !(ids = calloc(count, sizeof(*ids)))) {
    free(paths_raw); free_paths(paths, count); free(ids);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  for(size_t i = 0; i < count; i++) {
    const char *extension = strrchr(paths[i], '.');
    struct stat st;

    if(!extension || strcasecmp(extension, ".pkg")) {
      enum MHD_Result ret = send_json_error_detail(
        conn, MHD_HTTP_BAD_REQUEST, "file is not a PKG package",
        "pkg_type_invalid", paths[i]);
      free(paths_raw); free_paths(paths, count); free(ids);
      return ret;
    }
    if(stat(paths[i], &st) || !S_ISREG(st.st_mode)) {
      free(paths_raw); free_paths(paths, count); free(ids);
      return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
    }
  }

  result = enqueue_pkg_tasks(paths, count, ids, 0);
  free(paths_raw); free_paths(paths, count);
  if(result == PKG_INSTALL_UNSUPPORTED) {
    free(ids);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "package installation is only available on PS5",
                                  "pkg_install_unsupported", NULL);
  }
  if(result) {
    free(ids);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "could not queue package installation");
  }

  /* Keep the action from completing instantly after an accidental press. */
  usleep(600000);
  strbuf_append(&json, "{\"ok\":true,\"task_ids\":[");
  for(size_t i = 0; i < count; i++) {
    if(i) strbuf_append(&json, ",");
    strbuf_printf(&json, "%lu", ids[i]);
  }
  strbuf_append(&json, "]}");
  free(ids);
  return send_buffer(conn, MHD_HTTP_OK, json.data, "application/json");
}

enum MHD_Result
filemgr_api_request(struct MHD_Connection *conn, const char *url,
                    const char *method, const char *body, size_t body_size) {
  /* MANGO front-end route names (preferred) and owendswang legacy aliases. */
  if(!strcmp(url, "/api/fs/list") || !strcmp(url, "/api/list"))
    return api_list(conn, body, body_size);
  if(!strcmp(url, "/api/fs/read") || !strcmp(url, "/api/text"))
    return api_text(conn, body, body_size);
  if(!strcmp(url, "/api/tasks")) return api_tasks(conn, body, body_size);
  if(!strcmp(url, "/api/space")) return api_space(conn, body, body_size);
  if(!strcmp(url, "/api/task/cancel") || !strcmp(url, "/api/cancel"))
    return api_cancel(conn, body, body_size);
  if(!strcmp(url, "/api/shutdown") || !strcmp(url, "/api/exit"))
    return api_exit(conn, body, body_size);
  if(!strcmp(url, "/api/app/register"))
    return api_app_register(conn, body, body_size);
  if(!strcmp(url, "/api/status")) return api_status(conn, body, body_size);
  if(!strcmp(url, "/api/fetch")) return api_fetch(conn, body, body_size);
  if(!strcmp(url, "/api/nas/copy")) return api_nas_copy(conn, body, body_size);
  if(!strcmp(url, "/api/nas/delete"))
    return api_nas_delete(conn, body, body_size);
  if(!strcmp(url, "/api/save/scan")) return api_save_scan(conn, body, body_size);
  if(!strcmp(url, "/api/save/users")) return api_save_users(conn, body, body_size);
  if(!strcmp(url, "/api/save/list")) return api_save_list(conn, body, body_size);
  if(!strcmp(url, "/api/save/backup")) return api_save_backup(conn, body, body_size);
  if(!strcmp(url, "/api/save/restore")) return api_save_restore(conn, body, body_size);
  if(!strcmp(url, "/api/save/delete")) return api_save_delete(conn, body, body_size);
  if(!strcmp(url, "/api/save/snapdelete")) return api_save_snapdelete(conn, body, body_size);
  /* save-manager (import / replace / export) — see src/savemgr.c.
   * 整段交给 dispatch_save_mgr()：它在进程级锁下串行执行（对齐参考的存档管理
   * 器——它把每个 /api/ 请求都放在同一把互斥锁里），只有 import_log 例外。 */
  if (!strncmp(url, "/api/save_mgr/", 14))
    return dispatch_save_mgr(conn, url, body, body_size);
  if (!strcmp(url, "/api/pkg/scan")) return api_pkg_scan(conn, body, body_size);
  if(!strcmp(url, "/api/pkg/enqueue")) return api_pkg_enqueue(conn, body, body_size);
  if(!strcmp(url, "/api/pkg/uninstall")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_pkg_uninstall(conn, body, body_size);
  }
  if(!strcmp(url, "/api/pkg/install-url")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_pkg_install_url(conn, body, body_size);
  }
  if(!strcmp(url, "/api/install/poll")) return api_install_poll(conn, body, body_size);
  if(!strcmp(url, "/api/copy")) return api_copy(conn, body, body_size);
  if(!strcmp(url, "/api/move")) return api_move(conn, body, body_size);
  if(!strcmp(url, "/api/delete")) return api_delete(conn, body, body_size);
  if(!strcmp(url, "/api/download/prepare")) return api_download_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/download")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_download(conn);
  }
  if(!strcmp(url, "/api/upload/prepare")) return api_upload_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/upload/finish")) return api_upload_finish(conn);
  if(!strcmp(url, "/api/upload/progress")) return api_upload_progress(conn);
  if(!strcmp(url, "/api/rename")) return api_rename(conn, body, body_size);
  if(!strcmp(url, "/api/mkdir")) return api_mkdir(conn, body, body_size);
  if(!strcmp(url, "/api/launch-elf")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_launch_elf(conn, body, body_size);
  }
  if(!strcmp(url, "/api/chmod")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_chmod(conn, body, body_size);
  }
  if(!strcmp(url, "/api/extract")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_extract(conn, body, body_size);
  }
  if(!strcmp(url, "/api/pkg-info") || !strcmp(url, "/api/pkg/info")) {
    /* The MANGO front-end POSTs this with a form body; GET with ?path= is kept
     * as an alias. */
    return api_pkg_info(conn, body, body_size);
  }
  if(!strcmp(url, "/api/pkg-icon") || !strcmp(url, "/api/pkg/icon")) {
    return api_pkg_icon(conn, body, body_size);
  }
  if(!strcmp(url, "/api/install-pkg")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_install_pkg(conn, body, body_size);
  }
  if(!strcmp(url, "/api/text/create")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_create(conn, body, body_size);
  }
  if(!strcmp(url, "/api/text/save")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_save(conn, body, body_size);
  }
  return send_json_error(conn, MHD_HTTP_NOT_FOUND, "unknown api");
}

/* The MANGO-only endpoints (/api/status, /api/app/register, /api/fetch, the
 * save routes, the pkg routes and /api/install/poll) live in mango_port.c. */

/* Queue a single Sony .pkg for installation through the shared PKG worker.
 * Exposed for mango_port.c so /api/pkg/enqueue reuses the same queue as
 * /api/install-pkg instead of opening a second one. */
static int
queue_one_pkg(const char *path, unsigned long *out_id, int stream) {
  char *paths[1];
  unsigned long ids[1] = {0};
  int result;

  if(!path || !*path) return -1;
  paths[0] = (char *)path;
  result = enqueue_pkg_tasks(paths, 1, ids, stream);
  if(!result && out_id) *out_id = ids[0];
  return result;
}

int
filemgr_queue_pkg(const char *path, unsigned long *out_id) {
  return queue_one_pkg(path, out_id, 0);
}

int
filemgr_queue_pkg_stream(const char *path, unsigned long *out_id) {
  return queue_one_pkg(path, out_id, 1);
}

/* A remote source (smb:// / nfs:// URL or a UNC-style //host/share path).
 * Used by the install worker: remote packages MUST stream, there is no
 * path-delivery fallback the installer could accept. */
int
pkg_src_is_remote(const char *path) {
  if(!path) return 0;
  return !strncmp(path, "smb://", 6) || !strncmp(path, "nfs://", 6) ||
         (!strncmp(path, "//", 2) && path[2] && path[2] != '/');
}
