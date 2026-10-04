/* NEXUS-only endpoints, adapted to the owendswang framework.
 *
 * The NEXUS front-end calls routes this back-end did not have:
 *   /api/status /api/app/register /api/fetch
 *   /api/save/list|backup|restore
 *   /api/pkg/scan /api/pkg/enqueue /api/install/poll
 *
 * They are implemented here against the existing task list (file_task_t) rather
 * than NEXUS's own task_engine, so /api/tasks, the progress plumbing and the
 * single-active-task rule all stay in one place.
 *
 * Save domain NOTE: only the file-level snapshot / restore half is implemented
 * here (forced snapshot, atomic landing, rollback — the safety-critical part the
 * NEXUS audit found missing everywhere). The PFS mount / decrypt / re-sign half
 * needs the PS5 kernel and the save AuthID; it is PS5-only and marked below.
 * Paths can be redirected for the host test via WFM_SAVE_ROOT / WFM_SNAPSHOT_DIR.
 */
#include "filemgr_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app_installer.h"
#include "json_util.h"
#include "path_util.h"
#include "pkg_info.h"
#include "pkg_installer.h"
#include "transfer.h"
#include "websrv.h"

/* ====================================================================== */
/* shared helpers                                                          */
/* ====================================================================== */

/* Forward decl: copy_tree_r（save 域树复制）取证埋点要用 save_debug_log，
 * 但 save_debug_log 的定义在下面存档域里。前向声明让 copy_tree_r 也能调它。 */
static void save_debug_log(const char *fmt, ...);

/* Atomic single-file copy: land into a temp name + rename, so a crash never
 * leaves a half-written destination. Returns 0 on success. */
static int
copy_file_atomic(const char *src, const char *dst) {
  char temp[PATH_MAX];
  FILE *in = NULL, *out = NULL;
  char buf[65536];
  size_t n;
  int ret = -1;
  int fd = -1;

  if(strlen(dst) + 32 >= sizeof(temp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  snprintf(temp, sizeof(temp), "%s.wfm-port-%ld.tmp", dst, (long)getpid());
  if(!(in = fopen(src, "rb"))) return -1;
  if((fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) goto done;
  if(!(out = fdopen(fd, "wb"))) goto done;
  fd = -1;
  while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if(fwrite(buf, 1, n, out) != n) goto done;
  }
  if(ferror(in)) goto done;
  if(fflush(out) || fsync(fileno(out)) || fclose(out)) { out = NULL; goto done; }
  out = NULL;
  if(rename(temp, dst)) goto done;
  ret = 0;

done:
  if(out) fclose(out);
  if(fd >= 0) close(fd);
  if(in) fclose(in);
  if(ret) unlink(temp);
  return ret;
}

static void
mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t n = strlen(path);

  if(!n || n >= sizeof(tmp)) return;
  memcpy(tmp, path, n + 1);
  for(size_t i = 1; i < n; i++) {
    if(tmp[i] == '/') {
      tmp[i] = 0;
      mkdir(tmp, 0777);
      chmod(tmp, 0777);
      tmp[i] = '/';
    }
  }
  mkdir(tmp, 0777);
  chmod(tmp, 0777);
}

/* ---- 存档树递归的深度上限 ----------------------------------------------
 * 🪤 copy_tree **原先没有深度上限**，而同仓的 NAS(NEXUS_NAS_MAX_DEPTH 32)、
 * PKG(12)、RAR(128) 全都有。为什么要命：每帧栈占用 copy_tree = 2×PATH_MAX(8KB)，
 * 而本仓**全局没有任何 pthread_attr_setstacksize** ⇒ 任务线程吃默认栈。深层嵌套
 * 一旦把栈撑爆 → SIGSEGV → 整个进程没；PS5 无 service manager，进程退出
 * 没人拉起（只能重新投 payload）。栈溢出不会返回错误码，只会让进程消失。
 * 上限取 32 与 NAS 侧一致：真实存档嵌套远小于此，纯属兜底。
 * （删除/计数递归已移除：删除改走 filemgr_delete_run()，见 save_delete_worker。） */
#define NEXUS_SAVE_MAX_DEPTH 32

/* Recursive copy; files land atomically. Returns 0 on success. */
static int copy_tree_r(const char *src, const char *dst, unsigned depth) {
  struct stat st;
  DIR *d;
  struct dirent *ent;

  /* 🪤 取证埋点：备份/恢复与（已移除的）删除内联快照共用本函数。真机上一旦
   *   在这里崩（fault，非错误返回），最后一行被 fclose 刷出的日志就是杀进程
   *   的那个文件/调用 —— DIR/OPENDIR-FAIL/CHILD/FILE-BEFORE 各对应不同死法。
   *   save_debug_log 每次 fopen/fclose 即瞬时落盘，fault 也丢不了上一行。 */
  if(depth >= NEXUS_SAVE_MAX_DEPTH) {
    save_debug_log("cpy DEPTH-CAP depth=%u src=%s", depth, src);
    return -1;
  }
  if(lstat(src, &st)) {
    save_debug_log("cpy LSTAT-FAIL depth=%u src=%s", depth, src);
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    save_debug_log("cpy DIR depth=%u src=%s", depth, src);
    mkdir_p(dst);
    if(!(d = opendir(src))) {
      save_debug_log("cpy OPENDIR-FAIL depth=%u src=%s", depth, src);
      return -1;
    }
    while((ent = readdir(d))) {
      char s[PATH_MAX], t[PATH_MAX];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(path_join(s, sizeof(s), src, ent->d_name) ||
         path_join(t, sizeof(t), dst, ent->d_name) ||
         copy_tree_r(s, t, depth + 1)) {
        save_debug_log("cpy CHILD-FAIL depth=%u name=%s", depth + 1, ent->d_name);
        closedir(d);
        return -1;
      }
    }
    closedir(d);
    save_debug_log("cpy DIR-DONE depth=%u src=%s", depth, src);
    return 0;
  }
  if(!S_ISREG(st.st_mode)) {
    save_debug_log("cpy SKIP-SPECIAL depth=%u src=%s", depth, src);
    return 0;   /* skip specials / symlinks */
  }
  /* FILE-BEFORE 在 copy_file_atomic 之前打：若 copy_file_atomic 内 fault，
   * 这行就是案发记录（文件名+大小）。FILE-AFTER 走到 = 这次文件复制正常返回。 */
  save_debug_log("cpy FILE-BEFORE depth=%u size=%lld src=%s",
                 depth, (long long)st.st_size, src);
  {
    int rc = copy_file_atomic(src, dst);
    save_debug_log("cpy FILE-AFTER depth=%u rc=%d src=%s", depth, rc, src);
    return rc;
  }
}

static int
copy_tree(const char *src, const char *dst) { return copy_tree_r(src, dst, 0); }

/* Recursive delete, best effort. Used to take the restore safety copy back out
 * of the save leaf once it is no longer needed — leaving it there bloats the
 * user's save directory and makes /api/save/scan list a bogus "title". */
static void
remove_tree(const char *path) {
  struct stat st;
  DIR *d;
  struct dirent *ent;

  if(lstat(path, &st)) return;
  if(S_ISDIR(st.st_mode)) {
    if(!(d = opendir(path))) return;
    while((ent = readdir(d))) {
      char child[PATH_MAX];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(path_join(child, sizeof(child), path, ent->d_name)) continue;
      remove_tree(child);
    }
    closedir(d);
    rmdir(path);
    return;
  }
  unlink(path);
}

/* Marker suffix for the in-leaf safety copy taken right before a restore
 * overwrites the live save. Reserved: the save scan skips it so a copy left by
 * an interrupted restore never shows up as a game title. */
#define SAVE_PRERESTORE_SUFFIX ".wfm-pre-restore"

static void
local_ip(char *out, size_t n) {
  int fd;
  struct sockaddr_in a;
  socklen_t len = sizeof(a);

  out[0] = 0;
  if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
  if(!connect(fd, (struct sockaddr *)&a, sizeof(a)) &&
     !getsockname(fd, (struct sockaddr *)&a, &len)) {
    snprintf(out, n, "%s", inet_ntoa(a.sin_addr));
  }
  close(fd);
}

/* Queue a background task driven by `fn`. Returns NULL when another task is
 * already running (the single-active-task rule is shared with the file ops).
 *
 * `srcs` (may be NULL) is TAKEN OVER on every path — success, "another task is
 * running", and failure alike — because the worker may already be built around
 * it. It has to be installed here and not by the caller after the call: the
 * thread is created below, so assigning task->srcs afterwards would race with
 * a worker that reads it in its first instruction. */
static file_task_t *
queue_task_ex(task_op_t op, const char *src, const char *dst, const char *label,
              char **srcs, size_t src_count, void *(*fn)(void *)) {
  file_task_t *task = calloc(1, sizeof(*task));

  if(!task) {
    free_paths(srcs, src_count);
    return NULL;
  }
  task->op = op;
  task->state = TASK_QUEUED;
  task->srcs = srcs;
  task->src_count = src_count;
  snprintf(task->src, sizeof(task->src), "%s", src ? src : "");
  snprintf(task->dst, sizeof(task->dst), "%s", dst ? dst : "");
  snprintf(task->current, sizeof(task->current), "%s",
           label ? label : (src ? src : ""));
  task->created_at = task->updated_at = time(NULL);

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);          /* frees task->srcs too */
    return NULL;
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  if(pthread_create(&task->thread, NULL, fn, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }
  return task;
}

static file_task_t *
queue_task(task_op_t op, const char *src, const char *dst, const char *label,
           void *(*fn)(void *)) {
  return queue_task_ex(op, src, dst, label, NULL, 0, fn);
}

static enum MHD_Result
task_id_response(struct MHD_Connection *conn, unsigned long id) {
  strbuf_t b = {0};
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* ====================================================================== */
/* /api/status · /api/app/register · /api/fetch                            */
/* ====================================================================== */

enum MHD_Result
api_status(struct MHD_Connection *conn, const char *body, size_t body_size) {
  strbuf_t b = {0};
  char ip[64];

  (void)body; (void)body_size;
  local_ip(ip, sizeof(ip));
  strbuf_printf(&b, "{\"ok\":true,\"http\":true,\"port\":%u,\"addr\":", g_listen_port);
  json_escape(&b, ip);
  /* Report the version that is actually LIVE, so it can be read back from any
   * browser with no tools:  http://<ps5>:2026/api/status
   *
   * Why this is worth a field: the payload manager can only show the version
   * baked into the FILE NAME, i.e. the version you *deployed*. That is exactly
   * the wrong answer when an upgrade silently did nothing (stale build, old
   * instance still holding the port), which is the failure this project has
   * actually hit. The value here comes from the running binary's own -D. */
  strbuf_append(&b, ",\"version\":");
  json_escape(&b, VERSION_TAG);
  strbuf_append(&b, "}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_app_register(struct MHD_Connection *conn, const char *body, size_t body_size) {
  (void)body; (void)body_size;
#ifdef __SCE__
  /* Manual "add to home screen" (sceAppInstUtil*). The front-end confirms with
   * the user first — never run this while a game is running. */
  if(app_register_manual()) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "home-screen registration failed");
  }
  return send_json_ok(conn);
#else
  return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                "home-screen registration is only available on PS5",
                                "app_register_unsupported", NULL);
#endif
}

/* ====================================================================== */
/* /api/fetch — pull a file off SMB / NFS onto a local path.               */
/* ====================================================================== */

/* The dialog posts {src, dst, scheme}: `src` is a NAS address in the same shape
 * a bookmark stores it ("host/share/sub/path", optionally
 * "user:pass@host:port/…"), `dst` is either a directory or a full path.
 *
 * Shape credited to Loopayeh/pkg-sender (MIT), whose PS5 receiver performs the
 * same job over HTTP (/api/files/pull, with "overwrite"/"resume" modes). Three
 * of its calls are kept because they are right for a console:
 *   - RESUME is the default, not restart. A multi-GB game over Wi-Fi is the
 *     normal case, and an interrupted pull must not discard what already
 *     landed. The resume offset is read back from the destination's length, so
 *     the partial file itself is the checkpoint.
 *   - Landing is therefore NOT rename-atomic — for a 40 GB file on a FAT
 *     volume the rename would be a second full copy. The partial file is the
 *     state; a later pull continues it.
 *   - Progress is byte-based, so the existing task UI and the 「拉取」(kind 0)
 *     label work with no new plumbing.
 * The transport is this project's own transfer layer (libsmb2 / libnfs), which
 * reuses the NAS credentials the bookmark already holds. */

/* 拉取分块不再写死：transfer_read_chunk() 回后端协商出的读上限（SMB2 ≥1MB）。
   旧值 256KB 把复制速度钉死在「每往返 256KB」——千兆内网实测 ~7.5MB/s，正是
   「25GB 传十分钟才 18%」的来源。缓存一份查询结果（同一连接协商不变）。 */
#define FETCH_CHUNK_FALLBACK (256U * 1024U)

static int
write_all_fd(int fd, const void *buf, size_t len) {
  const char *p = buf;

  while(len) {
    ssize_t n = write(fd, p, len);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(!n) {
      errno = EIO;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* 取一个指向 `url` 的连接：优先复用 slot 里那个（同一个服务器/共享/凭据才允许，
 * 见 transfer_repoint），否则新建。返回 NULL = 地址解析不了或后端没编进来。 */
static transfer_src_t *
nas_source_for(transfer_src_t **slot, const char *url) {
  const char *sep;
  char scheme[8];
  size_t n;

  if(*slot && transfer_repoint(*slot, url)) return *slot;
  if(*slot) { transfer_close(*slot); *slot = NULL; }
  sep = strstr(url, "://");
  if(!sep || (n = (size_t)(sep - url)) >= sizeof(scheme)) return NULL;
  memcpy(scheme, url, n); scheme[n] = 0;
  *slot = transfer_open(scheme, sep + 3);
  return *slot;
}

/* NAS URL → 可回显的脱敏路径：剥掉 scheme://user:pass@host:port，只留
 * /share/path。task->current 会经 /api/tasks 的 label/note 原样回显到页面
 * （filemgr.c api_tasks），而拼进 task->srcs 的 URL 带 user:pass ——
 * 不脱敏就是把 NAS 密码贴到任务列表上。解析必须走 transfer_url_parse
 * （与读取同一条解析器）：手写找 '/' 在密码含 '/' 时会切进密码段。 */
static void
nas_url_label(const char *url, char *out, size_t out_size) {
  transfer_nas_t nas;

  if(!transfer_url_parse(url, &nas)) {
    /* 解析失败的 URL 可能带凭据，绝不能原样回显。 */
    snprintf(out, out_size, "?");
    return;
  }
  if(nas.path[0])
    snprintf(out, out_size, "/%s/%s", nas.share, nas.path);
  else if(nas.share[0])
    snprintf(out, out_size, "/%s", nas.share);
  else
    snprintf(out, out_size, "/");
}

/* 一个远程文件 → 本地文件，带续传。
 *
 * ★ 单文件拉取（/api/fetch）与 NAS→本地复制（/api/nas/copy）必须走**同一条**路。
 *   续传口径、0777 落盘、取消语义、错误文本一旦各写一份，下一次修 bug 只会修到
 *   其中一份 —— 这正是本项目反复踩的「两份实现各自漂移」。
 *
 * `*slot` 是连接槽（可为 NULL，函数会填上）。返回 0 成功 / -1 失败 / 1 被取消，
 * 失败原因写进 why。 */
static int
nas_pull_file(file_task_t *task, transfer_src_t **slot, const char *url,
              const char *dst, char *why, size_t why_size) {
  unsigned char *buf;
  transfer_src_t *src;
  uint64_t remote_size = 0;
  uint64_t offset = 0;
  size_t chunk;
  struct stat st;
  int fd = -1;
  int outcome = -1;

  chunk = FETCH_CHUNK_FALLBACK;
  if(!(buf = malloc(chunk))) {
    snprintf(why, why_size, "out of memory");
    return -1;
  }
  if(!(src = nas_source_for(slot, url))) {
    snprintf(why, why_size, "NAS 地址无法解析（写成 主机/共享/路径）");
    free(buf);
    return -1;
  }
  if(transfer_size(src, &remote_size) != NEXUS_OK) {
    snprintf(why, why_size, "%s", transfer_last_error(src)
             ? transfer_last_error(src) : "读不到远端文件大小");
    free(buf);
    return -1;
  }
  /* 连接已建立（transfer_size 连上了）才能问协商结果；问不到就退回旧值。 */
  {
    size_t mx = transfer_read_chunk(src);
    if(mx > chunk) {
      unsigned char *nb = realloc(buf, mx);
      if(nb) { buf = nb; chunk = mx; }
    }
  }
  /* ⚠️ 这里**不能**把 size == 0 当失败：空文件（.nomedia、锁文件、空的存档槽）
     是合法的，上一版 fetch 把它判成「拉取中断」，等于整棵目录树里有一个空文件
     就复制不过去。真·不存在的情况由 transfer_size 自己报错。
     current 必须在这里换掉扫描阶段的「正在统计远端目录…」——否则整棵树拉完，
     任务标题还停在统计文案（真机用户报的「一直显示正在统计远端目录」就是它）。 */
  {
    char lbl[PATH_MAX];
    nas_url_label(url, lbl, sizeof(lbl));
    task_update(task, TASK_RUNNING, lbl, 0, NULL);
  }

  /* Resume. A destination holding part of the file is continued; one that is
     not smaller than the source is foreign (or the source shrank) and is
     started over. */
  if(!stat(dst, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
     (uint64_t)st.st_size < remote_size) {
    offset = (uint64_t)st.st_size;
  }
  if((fd = open(dst, offset ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC),
                offset ? 0 : 0666)) < 0) {
    snprintf(why, why_size, "%s", strerror(errno));
    goto out;
  }
  fchmod_0777(fd);
  if(offset && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    snprintf(why, why_size, "%s", strerror(errno));
    goto out;
  }

  while(offset < remote_size) {
    size_t want = (remote_size - offset) < chunk
                    ? (size_t)(remote_size - offset) : chunk;
    size_t got = 0;

    if(task_cancel_requested(task)) { outcome = 1; goto out; }
    if(transfer_read(src, offset, buf, want, &got) != NEXUS_OK || !got) {
      snprintf(why, why_size, "%s", transfer_last_error(src)
               ? transfer_last_error(src) : "拉取中断");
      goto out;
    }
    if(write_all_fd(fd, buf, got)) {
      snprintf(why, why_size, "%s", strerror(errno));
      goto out;
    }
    offset += got;
    task_update(task, TASK_RUNNING, NULL, got, NULL);
  }
  outcome = (offset < remote_size) ? -1 : 0;

out:
  if(fd >= 0) close(fd);
  free(buf);
  return outcome;
}

/* /api/fetch 的 worker：task->src 就是 "<scheme>://<nas address>"。 */
static void *
fetch_worker(void *arg) {
  file_task_t *task = arg;
  /* task->src carries "<scheme>://<nas address>", composed by api_fetch so no
     extra field has to be added to file_task_t (which is shared with the file
     ops). */
  char composed[PATH_MAX];
  transfer_src_t *src = NULL;
  char why[256] = {0};
  int rc;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  snprintf(composed, sizeof(composed), "%s", task->src);

  rc = nas_pull_file(task, &src, composed, task->dst, why, sizeof(why));
  if(src) transfer_close(src);
  if(rc == 1) task_update(task, TASK_CANCELED, task->dst, 0, "canceled");
  else if(rc) task_update(task, TASK_FAILED, task->dst, 0,
                          why[0] ? why : "拉取中断");
  else task_update(task, TASK_DONE, task->dst, 0, NULL);
  return NULL;
}

/* ====================================================================== */
/* NAS → 本地：把远端文件/目录整棵复制到 PS5 的磁盘上                       */
/* ====================================================================== */

/* 递归深度上限：远端目录里有互相指过来的符号链接/mount point 时，
   transfer_list 会一圈圈列下去，直到把内存和栈吃光。32 层对游戏存档、
   备份目录都远远够用，撞上就是真出了问题，明说比挂死好。 */
#define NEXUS_NAS_MAX_DEPTH 32

typedef struct nas_copy_ctx {
  file_task_t    *task;
  transfer_src_t *src;      /* 复用一个连接，见 transfer_repoint */
  char            why[256];
} nas_copy_ctx_t;

/* 扫一棵远端子树，把**文件字节总数**累加进 *bytes；`out_is_dir` 回告那个 URL
 * 到底是目录还是文件。返回 0 正常 / -1 失败 / 1 取消。
 *
 * 为什么要先扫：没有分母，进度条只能是 0% 或者一个「进行中」的幌子 ——
 * 用户上一轮刚抱怨过「解压没进度」，同一个毛病不能换个功能再犯一次。
 * 列目录本身很便宜（每层一次 readdir）；贵的是逐文件传输，不是逐目录列举。
 * 扫描结果不缓存，拉取时重新列一遍：多一遍 readdir，换的是与目录规模无关的
 * 常驻内存 —— PS5 上不能为了「少列一次目录」把几万个路径全部揣在 RAM 里。 */
static int
nas_scan_tree(nas_copy_ctx_t *c, const char *url, uint64_t *bytes,
              int *out_is_dir, int depth) {
  transfer_entry_t *list = NULL;
  int count = 0;
  int i;

  if(out_is_dir) *out_is_dir = 0;
  if(depth >= NEXUS_NAS_MAX_DEPTH) {
    snprintf(c->why, sizeof(c->why), "远端目录超过 %d 层，已停止（可能有环）",
             NEXUS_NAS_MAX_DEPTH);
    return -1;
  }
  if(task_cancel_requested(c->task)) return 1;
  /* 每列一个目录就把 current 推到这个路径（脱敏）—— 与 filemgr 删除管线的
   * count_path_bytes_sync 同一口径：扫描阶段没有分母，进度数字纹丝不动，
   * 唯一能证明「没卡死」的就是正在扫的路径在变。 */
  {
    char lbl[PATH_MAX];
    nas_url_label(url, lbl, sizeof(lbl));
    task_update(c->task, TASK_RUNNING, lbl, 0, NULL);
  }
  if(!nas_source_for(&c->src, url)) {
    snprintf(c->why, sizeof(c->why), "NAS 地址无法解析（写成 主机/共享/路径）");
    return -1;
  }
  if(transfer_list(c->src, url, &list, &count) != NEXUS_OK) {
    /* 列不动 = 不是目录（transfer_list 对普通文件就是失败）。
       ★ 这里必须把**这个文件自己的字节数**累加进去：否则形如「源本身就是文件」
       的用法（用户在 NAS 视图里勾几个文件复制过来 —— 最常见的情形）分母恒为 0，
       进度条要么 0% 要么一个「进行中」的幌子，正是本项目反复踩的
       「有分子没分母」。真不存在时 transfer_size 同样读不到，那就什么也不加，
       最终由拉取那一步给出真原因（错误文本不在这里编）。 */
    uint64_t file_size = 0;

    free(list);
    if(transfer_size(c->src, &file_size) == NEXUS_OK) *bytes += file_size;
    return 0;
  }
  if(out_is_dir) *out_is_dir = 1;
  for(i = 0; i < count; i++) {
    char child[PATH_MAX];
    int is_dir = 0;
    int rc;

    if(list[i].is_dir) {
      if(snprintf(child, sizeof(child), "%s/%s", url, list[i].name) >=
         (int)sizeof(child)) continue;
      rc = nas_scan_tree(c, child, bytes, &is_dir, depth + 1);
      if(rc) { free(list); return rc; }
    } else {
      *bytes += list[i].size;
    }
  }
  free(list);
  return 0;
}

/* 逐条拉取（目录递归）。返回 0 成功 / -1 失败 / 1 取消。 */
static int
nas_pull_tree(nas_copy_ctx_t *c, const char *url, const char *dst, int depth) {
  transfer_entry_t *list = NULL;
  int count = 0;
  int i;
  int rc;

  if(depth >= NEXUS_NAS_MAX_DEPTH) {
    snprintf(c->why, sizeof(c->why), "远端目录超过 %d 层，已停止（可能有环）",
             NEXUS_NAS_MAX_DEPTH);
    return -1;
  }
  if(task_cancel_requested(c->task)) return 1;
  if(!nas_source_for(&c->src, url)) {
    snprintf(c->why, sizeof(c->why), "NAS 地址无法解析（写成 主机/共享/路径）");
    return -1;
  }
  if(transfer_list(c->src, url, &list, &count) != NEXUS_OK) {
    free(list);
    return nas_pull_file(c->task, &c->src, url, dst, c->why, sizeof(c->why));
  }
  /* 目录：先建（0777 —— umask 会削 mode，mkdir_p 里创建后再 chmod 补上），
     再逐个孩子。**不许**把目录做成原子 rename：目录树是逐个文件落地的，
     中断后保留已拉下来的部分正是续传的前提。 */
  mkdir_p(dst);
  chmod(dst, 0777);
  task_update(c->task, TASK_RUNNING, dst, 0, NULL);
  for(i = 0; i < count; i++) {
    char child_url[PATH_MAX];
    char child_dst[PATH_MAX];

    if(task_cancel_requested(c->task)) { free(list); return 1; }
    if(snprintf(child_url, sizeof(child_url), "%s/%s", url, list[i].name) >=
       (int)sizeof(child_url) || path_join(child_dst, sizeof(child_dst), dst,
                                          list[i].name)) continue;
    if(list[i].is_dir) {
      rc = nas_pull_tree(c, child_url, child_dst, depth + 1);
    } else {
      rc = nas_pull_file(c->task, &c->src, child_url, child_dst,
                         c->why, sizeof(c->why));
    }
    if(rc) { free(list); return rc; }
  }
  free(list);
  return 0;
}

/* 取 URL 里最后一段当落地名。⚠️ 必须走 transfer_url_parse 而不是自己 strrchr：
 * URL 头部可能带 "user:pass@"（含 '/' 不合法但 ':' 合法），而且**共享根**这种
 * 源（path 为空）要退回共享名，否则落地名会是一个空串。 */
static void
nas_basename_of(const char *url, char *out, size_t out_size) {
  transfer_nas_t nas;
  const char *p;

  out[0] = 0;
  if(!transfer_url_parse(url, &nas)) return;
  p = nas.path[0] ? nas.path : nas.share;
  {
    const char *slash = strrchr(p, '/');
    if(slash && slash[1]) p = slash + 1;
  }
  snprintf(out, out_size, "%s", p);
}

static void *
nas_copy_worker(void *arg) {
  file_task_t *task = arg;
  nas_copy_ctx_t ctx;
  uint64_t bytes = 0;
  size_t i;
  int rc = 0;

  memset(&ctx, 0, sizeof(ctx));
  ctx.task = task;
  task_update(task, TASK_RUNNING, "正在统计远端目录…", 0, NULL);

  for(i = 0; i < task->src_count && !rc; i++) {
    int is_dir = 0;
    rc = nas_scan_tree(&ctx, task->srcs[i], &bytes, &is_dir, 0);
  }
  if(rc) goto done;

  /* 分母。0（全是空目录/空文件）时保持 0 —— 前端对 total == 0 显示「进行中」
     而不是 0%，那是诚实的；写个假的 100 出来才是骗人。 */
  task->total = bytes;

  for(i = 0; i < task->src_count && !rc; i++) {
    char base[256];
    char dst[PATH_MAX];

    nas_basename_of(task->srcs[i], base, sizeof(base));
    if(!base[0]) {
      snprintf(ctx.why, sizeof(ctx.why), "源路径没有文件名");
      rc = -1;
      break;
    }
    if(path_join(dst, sizeof(dst), task->dst, base)) {
      snprintf(ctx.why, sizeof(ctx.why), "目标路径过长");
      rc = -1;
      break;
    }
    rc = nas_pull_tree(&ctx, task->srcs[i], dst, 0);
  }

done:
  if(ctx.src) transfer_close(ctx.src);
  if(rc == 1) task_update(task, TASK_CANCELED, task->dst, 0, "canceled");
  else if(rc) task_update(task, TASK_FAILED, task->dst, 0,
                          ctx.why[0] ? ctx.why : "复制中断");
  else task_update(task, TASK_DONE, task->dst, 0, NULL);
  return NULL;
}

/* 把 (scheme, 凭据, 端口, 用户填的地址) 拼成 transfer_url_parse 认的形状：
 *     smb://user:pass@host:port/share/path
 * 端口必须插在**主机之后、路径之前**，而用户给的是「主机/共享/路径」（没有端口位），
 * 所以按第一个分隔符切开再插 —— 直接前后缀拼不上去。
 * 返回 0 成功 / -1（地址为空或拼出来过长）。 */
static int
nas_compose_url(const char *scheme, const char *user, const char *pass,
                const char *port, const char *addr, char *out, size_t out_size) {
  size_t hostlen = strcspn(addr, "/\\");
  int n;

  if(!hostlen) { out[0] = 0; return -1; }
  n = snprintf(out, out_size, "%s://%s%s%s%s%.*s%s%s%s",
               scheme,
               (user && *user) ? user : "",
               (user && *user) ? ":" : "",
               (user && *user) ? (pass ? pass : "") : "",
               (user && *user) ? "@" : "",
               (int)hostlen, addr,
               (port && *port) ? ":" : "",
               (port && *port) ? port : "",
               addr + hostlen);
  if(n < 0 || (size_t)n >= out_size) { out[0] = 0; return -1; }
  return 0;
}

/* POST /api/nas/copy —— 把 NAS 上的文件/目录复制到 PS5 的本地目录。
 *
 * 为什么不是 /api/copy：那条路的第一个动作就是 lstat(src)，而 NAS 路径在本地
 * 根本不存在 ⇒ 用户看到的正是 "source not found"。远程源必须走 transfer 层。
 *
 * 参数：scheme=smb|nfs，paths=换行分隔的 NAS 路径（和地址栏里那串同形：
 * 主机/共享/路径），dst=本地目标目录，user/pass/port 可选。
 * 凭据由这里拼进每个源的 URL；**task->src / current 只用不带凭据的路径**，
 * 因为 /api/tasks 会把它们当 label 回显到页面上。 */
enum MHD_Result
api_nas_copy(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *scheme = req_param(conn, body, body_size, "scheme");
  char *paths = req_param(conn, body, body_size, "paths");
  char *dst = req_param(conn, body, body_size, "dst");
  char *user = req_param(conn, body, body_size, "user");
  char *pass = req_param(conn, body, body_size, "pass");
  char *port = req_param(conn, body, body_size, "port");
  char **srcs = NULL;
  size_t src_count = 0;
  char label[PATH_MAX];
  char label_plain[PATH_MAX];
  struct stat st;
  file_task_t *task;

  if(!paths || !*paths || !dst || !*dst) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "源路径与目标目录都要填");
  }
  if(!scheme || !*scheme) {
    free(scheme);
    scheme = strdup("smb");
  }
  if(!scheme) {
    free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!transfer_scheme_supported(scheme)) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "NAS backend not compiled in (smb/nfs)",
                                  "nas_unsupported", NULL);
  }
  /* 目标必须是已存在的**本地目录**。让它的父目录落在一个远程 URL 上会写出
     一份相对路径垃圾（"192.168.1.3/PS5_Games/x" 会被当成当前目录下的目录）。 */
  if(dst[0] != '/' || stat(dst, &st) || !S_ISDIR(st.st_mode)) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "目标必须是已存在的本地目录", "bad_target", NULL);
  }

  /* 一次分配够：先数行数，避免 realloc 的两处失败清理分支。
     故意不用 strtok_r —— 那是 POSIX 扩展，PS5 的 libc 上有没有**没验过**，
     而这里本来就只是按 '\n'/'\r' 切一刀（`lstat` / `getmntinfo` 已经给过一次教训：
     宿主有、SDK 没有的东西，编译能过、上机才炸）。strpbrk 是 C89，安全。 */
  label[0] = 0;
  label_plain[0] = 0;
  {
    const char *q;
    size_t lines = 1;

    for(q = paths; *q; q++) if(*q == '\n') lines++;
    if(!(srcs = calloc(lines, sizeof(*srcs)))) {
      free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
    }
  }
  {
    const char *p = paths;

    while(*p) {
      char addr[PATH_MAX];
      char url[PATH_MAX];
      const char *eol = strpbrk(p, "\r\n");
      size_t len = eol ? (size_t)(eol - p) : strlen(p);
      size_t k;
      int dup = 0;

      if(len >= sizeof(addr)) len = sizeof(addr) - 1;
      memcpy(addr, p, len);
      addr[len] = 0;
      p = eol ? eol + 1 : p + len;

      /* 前后空白（从别处粘进来时常见）不参与比对，也不该进 URL。 */
      {
        char *s = addr;
        while(*s == ' ' || *s == '\t') s++;
        if(s != addr) memmove(addr, s, strlen(s) + 1);
      }
      len = strlen(addr);
      while(len && (addr[len - 1] == ' ' || addr[len - 1] == '\t')) addr[--len] = 0;
      if(!len) continue;

      if(nas_compose_url(scheme, user, pass, port, addr, url, sizeof(url)))
        continue;
      /* 去重按**拼好的 URL** 比：同一个路径被重复勾选/重复粘一行时不该拉两遍。 */
      for(k = 0; k < src_count; k++)
        if(!strcmp(srcs[k], url)) { dup = 1; break; }
      if(dup) continue;
      if(!(srcs[src_count] = strdup(url))) {
        free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
        free_paths(srcs, src_count);
        return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
      }
      src_count++;
      if(!label_plain[0]) snprintf(label_plain, sizeof(label_plain), "%s", addr);
    }
  }
  free(scheme); free(paths); free(user); free(pass); free(port);
  if(!src_count) {
    free(dst);
    free(srcs);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "没有可复制的源路径");
  }

  /* 标签里**不带凭据**（/api/tasks 会回显它）。 */
  if(src_count > 1)
    snprintf(label, sizeof(label), "%s 等 %zu 项", label_plain, src_count);
  else
    snprintf(label, sizeof(label), "%s", label_plain);

  task = queue_task_ex(TASK_DOWNLOAD, label_plain, dst, label, srcs, src_count,
                       nas_copy_worker);
  free(dst);
  if(!task) {
    free_paths(srcs, src_count);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  return task_id_response(conn, task->id);
}

enum MHD_Result
api_fetch(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *src_path = req_param(conn, body, body_size, "src");
  char *dst = req_param(conn, body, body_size, "dst");
  char *scheme = req_param(conn, body, body_size, "scheme");
  char composed[PATH_MAX];
  char target[PATH_MAX];
  const char *base;
  struct stat st;
  file_task_t *task;
  enum MHD_Result result;
  size_t needed;
  size_t dst_len;

  if(!src_path || !*src_path || !dst || !*dst) {
    free(src_path); free(dst); free(scheme);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "源路径与目标目录都要填");
  }
  if(!scheme || !*scheme) {
    free(scheme);
    scheme = strdup("smb");
  }
  if(!scheme) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!transfer_scheme_supported(scheme)) {
    free(src_path); free(dst); free(scheme);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "NAS backend not compiled in (smb/nfs)",
                                  "fetch_unsupported", NULL);
  }

  needed = (size_t)snprintf(composed, sizeof(composed), "%s://%s",
                            scheme, src_path);
  free(scheme);
  if(needed >= sizeof(composed)) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "NAS 地址过长");
  }

  /* A destination that names a directory keeps the remote basename.
     ⚠️ 两种分隔符都要找：粘进来的 Windows 路径
     （"192.168.1.3\Games\PPSA00000-app.pkg"）里一个 '/' 都没有，只找 '/'
     的话 strrchr 返回 NULL，于是「文件名」变成整个 URL，落盘名会带着主机名。 */
  dst_len = strlen(dst);
  base = strrchr(src_path, '/');
  {
    const char *bs = strrchr(src_path, '\\');
    if(bs && (!base || bs > base)) base = bs;
  }
  base = (base && base[1]) ? base + 1 : src_path;
  if(!base[0]) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "源路径没有文件名");
  }
  if((!stat(dst, &st) && S_ISDIR(st.st_mode)) ||
     (dst_len && dst[dst_len - 1] == '/')) {
    needed = (size_t)snprintf(target, sizeof(target), "%s/%s", dst, base);
  } else {
    needed = (size_t)snprintf(target, sizeof(target), "%s", dst);
  }
  free(dst);
  if(needed >= sizeof(target)) {
    free(src_path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "目标路径过长");
  }

  task = queue_task(TASK_DOWNLOAD, composed, target, base, fetch_worker);
  free(src_path);
  if(!task) {
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  result = task_id_response(conn, task->id);
  return result;
}

/* ====================================================================== */
/* Save domain: /api/save/list · /api/save/backup · /api/save/restore       */
/* ====================================================================== */

#define SAVEMGR_DEFAULT_SNAPSHOT_DIR "/data/savesnap"

static const char *
save_snapshot_dir(void) {
  const char *e = getenv("WFM_SNAPSHOT_DIR");
  return (e && *e) ? e : SAVEMGR_DEFAULT_SNAPSHOT_DIR;
}

/* 存档域的提权。定义在下面（backup/restore 旁边），但**扫描/查询也要用** ——
 * 见 api_save_scan 的说明：不先提权，/user/home 下的 opendir 会被拒，
 * 于是「有没有存档」这个问题永远回答「没有」。 */
static int save_escalate(void);

/* Where save data actually lives.
 *
 * ⚠️ The previous list (/user/data/<tid>/savedata_prospero, /data/<tid>/…) was a
 * guess and matches nothing on a real console. PS5 keeps user saves under the
 * ACCOUNT's home directory — one level deeper than a title id can express:
 *     /user/home/<account-hex>/savedata_prospero/<TITLE_ID>       ← the data
 *     /user/home/<account-hex>/savedata_prospero_meta/<TITLE_ID>  ← index/meta
 * (extended storage mirrors it under /mnt/ext1/home/). The account id cannot be
 * derived from a title id, so that level has to be ENUMERATED rather than
 * templated — which is why the old probe could never succeed, and why a manually
 * typed title id looked like "this console has no save for that game".
 *
 * The flat roots are kept as a cheap fallback (one stat each) for firmware that
 * stores saves directly under the data partition. */
static const char *const g_save_home_roots[] = {
  "/user/home/", "/mnt/ext1/home/"
};
static const char *const g_save_flat_roots[] = {
  "/user/data/", "/data/", "/mnt/ext1/user/data/"
};
static const char *const g_save_leaves[] = {
  "savedata_prospero", "savedata_prospero_meta",
  /* PS4 世代标题在 PS5 上落在同级的 savedata/ · savedata_meta/（psdevwiki
     Save Data：「PS4 titles keep using savedata」）。本机跑的是 PS5 原生标题，
     但用户也可能通过兼容层装 PS4 游戏 —— 这两个多一次 opendir 的成本，
     换来的是「PS4 游戏的存档也能备份」，且顺序放在 prospero 之后，
     同名标题不会被认错世代。 */
  "savedata", "savedata_meta"
};

static int
file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* Does the directory hold anything (beyond "."/"..")? Unreadable counts as
 * empty — callers treat "empty" as "no save data", and an unreadable dir has
 * no readable save data either way.
 *
 * ★ 真机删除的教训：删除本机存档时**不能 rmdir TITLE_ID 叶目录本身**——它由
 * 内核 save daemon 管着，rmdir 掉它是真机「删存档 ⇒ 进程消失」的头号嫌疑
 * （备份/恢复从不 rmdir 它，也从不崩）。改为「清空内容、保留目录」后，全链
 * 必须把「目录存在但为空」读成「没有存档」，这一处就是那个判定：
 *   - probe_save_leaves()/probe_save_flat()：find_save_dir ⇒ save_found；
 *   - scan_save_base()：scan 列表不再列出空标题。
 * 三处共用本函数，谁也不能只改一处（顺序契约见 probe_save_leaves 注释）。 */
static int
save_dir_has_content(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;
  int has = 0;

  if(!d) return 0;
  while(!has && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    has = 1;
  }
  closedir(d);
  return has;
}

/* "<base><leaf>/<title_id>" for every leaf; first hit wins. `base` ends with a
 * slash (or is the WFM_SAVE_ROOT host-test root, see find_save_dir).
 *
 * ★ 层级顺序是 leaf 在前、title id 在后 —— 真机就是
 *     /user/home/<account-hex>/savedata_prospero/<TITLE_ID>
 * （psdevwiki Save Data + gbatemp 实机 FTP 双源确认）。曾经写成
 * <base><title_id>/<leaf>，在真机上永远 stat 不到 ⇒ 备份/恢复与「有没有存档」
 * 一律落到「找不到存档」，手动敲 TITLE_ID 也一样 —— 正是用户报的现象。
 * 这里的顺序必须与 scan_save_base() 保持一致：同一个存档不允许一处说有、
 * 一处说没有。 */
static int
probe_save_leaves(const char *base, const char *title_id, char *out,
                  size_t outsz) {
  size_t i;

  for(i = 0; i < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); i++) {
    int n = snprintf(out, outsz, "%s%s/%s", base, g_save_leaves[i], title_id);
    if(n < 0 || (size_t)n >= outsz) continue;
    /* 存在且非空才算「有存档」：删除 worker 现在保留（空的）叶目录，
     * 一个空壳目录不能让 save_found/备份/恢复继续认它。 */
    if(file_exists(out) && save_dir_has_content(out)) return 1;
  }
  return 0;
}

/* Flat roots = firmware variants that keep saves straight under the data
 * partition. Both orders get one stat each instead of betting on one: this is
 * only a cheap fallback, and a miss here is indistinguishable from "no save". */
static int
probe_save_flat(const char *root, const char *title_id, char *out, size_t outsz) {
  size_t i;
  int n;

  if(probe_save_leaves(root, title_id, out, outsz)) return 1;
  for(i = 0; i < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); i++) {
    n = snprintf(out, outsz, "%s%s/%s", root, title_id, g_save_leaves[i]);
    if(n > 0 && (size_t)n < outsz && file_exists(out) &&
       save_dir_has_content(out)) return 1;
  }
  n = snprintf(out, outsz, "%s%s", root, title_id);
  if(n > 0 && (size_t)n < outsz && file_exists(out) &&
     save_dir_has_content(out)) return 1;
  return 0;
}

/* Walk one /user/home level — the account directory is the part we do not know. */
static int
probe_save_home(const char *home_root, const char *title_id, char *out,
                size_t outsz) {
  DIR *d = opendir(home_root);
  struct dirent *e;
  char base[PATH_MAX];
  int found = 0;

  if(!d) return 0;
  while(!found && (e = readdir(d))) {
    if(e->d_name[0] == '.') continue;
    if(snprintf(base, sizeof(base), "%s%s/", home_root, e->d_name) >=
       (int)sizeof(base)) continue;
    found = probe_save_leaves(base, title_id, out, outsz);
  }
  closedir(d);
  return found;
}

/* Locate a title's live save directory. WFM_SAVE_ROOT narrows the probe to a
 * single account home (host test). */
static int
find_save_dir(const char *title_id, char *out, size_t outsz) {
  const char *env = getenv("WFM_SAVE_ROOT");
  size_t i;

  if(!title_id || !*title_id) return 0;
  if(env && *env) {
    char base[PATH_MAX];
    size_t n = strlen(env);
    if(snprintf(base, sizeof(base), "%s%s", env,
                (n && env[n - 1] == '/') ? "" : "/") < (int)sizeof(base) &&
       probe_save_leaves(base, title_id, out, outsz)) return 1;
    snprintf(out, outsz, "%s/%s", env, title_id);
    return file_exists(out);
  }
  for(i = 0; i < sizeof(g_save_home_roots) / sizeof(g_save_home_roots[0]); i++) {
    if(probe_save_home(g_save_home_roots[i], title_id, out, outsz)) return 1;
  }
  for(i = 0; i < sizeof(g_save_flat_roots) / sizeof(g_save_flat_roots[0]); i++) {
    if(probe_save_flat(g_save_flat_roots[i], title_id, out, outsz)) return 1;
  }
  return 0;
}

/* Recursive size of a save directory. A save is a handful of small files; this
 * only labels a row in the UI, so an unreadable entry is skipped instead of
 * failing the whole scan. */
static unsigned long long
save_dir_size(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;
  unsigned long long total = 0;

  if(!d) return 0;
  while((e = readdir(d))) {
    char child[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
       (int)sizeof(child)) continue;
    if(lstat(child, &st)) continue;
    total += S_ISDIR(st.st_mode) ? save_dir_size(child)
                                 : (unsigned long long)st.st_size;
  }
  closedir(d);
  return total;
}

/* 扫描诊断：把「读不到」和「这台机器上没有」在页面上分开。
 *
 * 🪤 这两件事以前在 UI 上是同一句话（一个空列表），而用户该做的事完全不同 ——
 * 一个是权限/挂载（要提权、要换 AuthID），另一个是「确实没玩过这个游戏」。
 * 用户的机器上明明有游戏，页面却只会说「没找到存档」，我们连该问什么都不知道：
 * 是 /user/home 打不开？还是打开了但里面没有账号目录？
 * 有了这个数组，回来的就是「哪个根 opendir 失败 / 看到几个账号 / 命中几个标题」，
 * 一句话就能定位。 */
typedef struct save_scan_stat {
  char root[PATH_MAX];
  int  opened;     /* opendir 成功 */
  int  accounts;   /* 只有 home 根才有意义 */
  int  titles;     /* 只有叶目录才有意义 */
} save_scan_stat_t;

#define SAVE_SCAN_MAX_STATS 48

static void
save_scan_note(save_scan_stat_t *stats, int *n, const char *root, int opened) {
  if(*n >= SAVE_SCAN_MAX_STATS) return;
  snprintf(stats[*n].root, sizeof(stats[*n].root), "%s", root);
  stats[*n].opened = opened;
  stats[*n].accounts = 0;
  stats[*n].titles = 0;
  (*n)++;
}

static void
save_scan_add_title(save_scan_stat_t *stats, int n, int titles) {
  if(n <= 0) return;
  stats[n - 1].titles += titles;
}

/* A save leaf holds more than games: sce_backupN (system backup slots),
 * per-account "user"/meta trees, restore safety copies. Users read the scan
 * list as "these are my games' saves", so anything that is not shaped like a
 * title id (4 uppercase letters + 5 digits: PPSA…, CUSA…, SCUS…) is noise and
 * is filtered out here — the exact rows the user circled in the save page
 * screenshot ("sce_backup4 · 0 B · savedata", "user · 7.9 MB · meta"). */
static int
save_name_is_title(const char *name) {
  size_t i, n = strlen(name);

  if(n != 9) return 0;
  for(i = 0; i < 4; i++)
    if(name[i] < 'A' || name[i] > 'Z') return 0;
  for(; i < 9; i++)
    if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}

/* Emit every title directory under "<base><leaf>". */
static void
scan_save_base(const char *base, strbuf_t *b, int *first,
               save_scan_stat_t *stats, int *nstats) {
  size_t l;

  for(l = 0; l < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); l++) {
    char leaf_dir[PATH_MAX];
    DIR *titles;
    struct dirent *td;
    int hits = 0;

    if(snprintf(leaf_dir, sizeof(leaf_dir), "%s%s", base, g_save_leaves[l]) >=
       (int)sizeof(leaf_dir)) continue;
    save_scan_note(stats, nstats, leaf_dir, 0);
    if(!(titles = opendir(leaf_dir))) continue;
    stats[*nstats - 1].opened = 1;
    while((td = readdir(titles))) {
      char title_dir[PATH_MAX];
      struct stat st;

      if(td->d_name[0] == '.') continue;
      /* 恢复前的安全副本住在这个叶里，它不是「一个游戏」（见上面那个 #define） */
      {
        size_t nl = strlen(td->d_name);
        size_t sl = strlen(SAVE_PRERESTORE_SUFFIX);
        if(nl > sl && !strcmp(td->d_name + nl - sl, SAVE_PRERESTORE_SUFFIX))
          continue;
      }
      if(snprintf(title_dir, sizeof(title_dir), "%s/%s", leaf_dir, td->d_name) >=
         (int)sizeof(title_dir)) continue;
      if(lstat(title_dir, &st) || !S_ISDIR(st.st_mode)) continue;
      if(!save_name_is_title(td->d_name)) continue;
      /* 空壳标题目录（删除后保留的叶目录、或人为建的空目录）不是「一个游戏」。
       * 与 probe_save_leaves() 共用 save_dir_has_content()：一处说有、一处说没
       * 有就是回归（顺序契约见 probe_save_leaves 注释）。 */
      if(!save_dir_has_content(title_dir)) continue;
      /* 空壳标题目录（删除后保留的叶目录、或人为建的空目录）不是「一个游戏」。
       * 与 probe_save_leaves() 共用 save_dir_has_content()：一处说有、一处说没
       * 有就是回归（顺序契约见 probe_save_leaves 注释）。 */
      if(!*first) strbuf_append(b, ",");
      *first = 0;
      strbuf_append(b, "{\"title_id\":");
      json_escape(b, td->d_name);
      strbuf_append(b, ",\"path\":");
      json_escape(b, title_dir);
      strbuf_printf(b, ",\"size\":%llu,\"mtime\":%lld,\"kind\":",
                    save_dir_size(title_dir), (long long)st.st_mtime);
      json_escape(b, g_save_leaves[l]);
      strbuf_append(b, "}");
      hits++;
    }
    closedir(titles);
    save_scan_add_title(stats, *nstats, hits);
  }
}

/* A home root holds one directory per account; every account is a base.
 * `stats`/`nstats` 可为 NULL（其它调用方不关心诊断）。 */
static void
scan_save_accounts(const char *home_root, strbuf_t *b, int *first,
                   save_scan_stat_t *stats, int *nstats) {
  DIR *d = opendir(home_root);
  struct dirent *e;
  char base[PATH_MAX];
  int accounts = 0;
  int local_n = 0;
  save_scan_stat_t local_stats[SAVE_SCAN_MAX_STATS];

  if(!stats) { stats = local_stats; nstats = &local_n; }
  save_scan_note(stats, nstats, home_root, d ? 1 : 0);
  if(!d) return;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.') continue;
    if(snprintf(base, sizeof(base), "%s%s/", home_root, e->d_name) >=
       (int)sizeof(base)) continue;
    accounts++;
    scan_save_base(base, b, first, stats, nstats);
  }
  closedir(d);
  stats[*nstats - 1].accounts = accounts;
}

/* Every title with save data on this console.
 *
 * The save page had no way to answer "what is on here": its only endpoint was
 * /api/save/list, which lists SNAPSHOTS — the things you already backed up — so
 * a console that had never been backed up showed an empty page no matter what.
 * This is the scan that page was missing, and it is also what makes a manually
 * typed title id answerable (see api_save_list's save_found).
 *
 * ★ 提权必须在扫描**之前**：/user/home/<账号hex> 属存档域，没有存档 AuthID
 *   （0x4800000000000010）时 opendir 会被拒 —— 于是扫描恒返回空列表，
 *   页面永远是「没有存档」，而备份/恢复那边因为先调了 save_escalate() 却是好的。
 *   这就是用户「手动输 ID 也查不到」的直接原因：查询和备份走的不是同一条权限路径。 */
enum MHD_Result
api_save_scan(struct MHD_Connection *conn, const char *body, size_t body_size) {
  const char *env = getenv("WFM_SAVE_ROOT");
  strbuf_t b = {0};
  save_scan_stat_t stats[SAVE_SCAN_MAX_STATS];
  int nstats = 0;
  int esc;
  int first = 1;
  size_t i;
  int k;

  (void)body;
  (void)body_size;

  esc = save_escalate();
  strbuf_append(&b, "{\"ok\":true,\"escalated\":");
  strbuf_append(&b, esc ? "false" : "true");
  strbuf_append(&b, ",\"saves\":[");
  if(env && *env) {
    /* Host test: one account home. */
    char base[PATH_MAX];
    size_t n = strlen(env);
    if(snprintf(base, sizeof(base), "%s%s", env,
                (n && env[n - 1] == '/') ? "" : "/") < (int)sizeof(base))
      scan_save_base(base, &b, &first, stats, &nstats);
  } else {
    for(i = 0; i < sizeof(g_save_home_roots) / sizeof(g_save_home_roots[0]); i++)
      scan_save_accounts(g_save_home_roots[i], &b, &first, stats, &nstats);
  }
  strbuf_append(&b, "],\"roots\":[");
  for(k = 0; k < nstats; k++) {
    if(k) strbuf_append(&b, ",");
    strbuf_append(&b, "{\"root\":");
    json_escape(&b, stats[k].root);
    strbuf_printf(&b, ",\"opened\":%s,\"accounts\":%d,\"titles\":%d}",
                  stats[k].opened ? "true" : "false",
                  stats[k].accounts, stats[k].titles);
  }
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* Escalate to the save AuthID. PS5-only: it drives kernel ucred, which the host
 * does not have. The file-level snapshot/restore below works without it. */
static int
save_escalate(void) {
#ifdef __linux__
  return 0;
#else
  extern int kernel_set_ucred_authid(unsigned long long);
  return kernel_set_ucred_authid(0x4800000000000010ULL) ? -1 : 0;
#endif
}

enum MHD_Result
api_save_list(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char dir[PATH_MAX];
  DIR *d;
  struct dirent *ent;
  strbuf_t b = {0};
  int first = 1;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  snprintf(dir, sizeof(dir), "%s/%s", save_snapshot_dir(), tid);
  strbuf_append(&b, "{\"ok\":true,\"title_id\":");
  json_escape(&b, tid);
  /* 「有没有存档」和「有没有快照」是两件不同的事。只报 snapshots 的时候，一台
     从没备份过的机器看起来永远是「这台机器上没有这个游戏的存档」——手动输入
     TITLE_ID 也一样是空的，用户根本无法区分「没存档」和「只是还没备份」。 */
  {
    char live[PATH_MAX];
    struct stat st;
    /* ★ 提权必须在 find_save_dir 之前，理由同 api_save_scan：没有存档 AuthID
       时 /user/home/<账号hex> 根本 opendir 不了。备份/恢复那边一直是对的，
       只有「查询」漏了这一步 —— 于是同一个存档「备份能备份、查询说没有」。 */
    save_escalate();
    int found = find_save_dir(tid, live, sizeof(live)) == 1 &&
                !lstat(live, &st) && S_ISDIR(st.st_mode);
    strbuf_append(&b, ",\"save_found\":");
    strbuf_append(&b, found ? "true" : "false");
    strbuf_append(&b, ",\"save_path\":");
    json_escape(&b, found ? live : "");
    strbuf_printf(&b, ",\"save_size\":%llu", found ? save_dir_size(live) : 0ULL);
  }
  strbuf_append(&b, ",\"snapshots\":[");
  if((d = opendir(dir))) {
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(!first) strbuf_append(&b, ",");
      first = 0;
      json_escape(&b, ent->d_name);
    }
    closedir(d);
  }
  strbuf_append(&b, "]}");
  free(tid);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* Backup worker: FORCED snapshot of the title's save dir into the snapshot
 * root. There is no flag to skip it — an irreversible write-back without a
 * snapshot is exactly the failure mode this domain must not have. */
static void *
save_backup_worker(void *arg) {
  file_task_t *task = arg;
  char src[PATH_MAX], snap_root[PATH_MAX], snap[PATH_MAX], stamp[32];
  struct timespec ts;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(save_escalate() || !find_save_dir(task->src, src, sizeof(src))) {
    task_update(task, TASK_FAILED, task->src, 0, "save data not found");
    return NULL;
  }
  /* 快照名 = 毫秒级时间戳（秒%03u毫秒）。秒级 time(NULL) 会让同一秒内两次
   * backup 撞名合并成一份 —— 用户快速连点「备份」静默丢快照，hosttest 的
   * 批量删测试也抓到了。仍是纯数字，save_snapshot_name_ok 不拒绝。 */
  clock_gettime(CLOCK_REALTIME, &ts);
  snprintf(stamp, sizeof(stamp), "%llu%03u",
           (unsigned long long)ts.tv_sec, (unsigned)(ts.tv_nsec / 1000000));
  snprintf(snap_root, sizeof(snap_root), "%s/%s", save_snapshot_dir(), task->src);
  snprintf(snap, sizeof(snap), "%s/%s", snap_root, stamp);
  mkdir_p(snap_root);
  if(copy_tree(src, snap)) {
    task_update(task, TASK_FAILED, src, 0, "snapshot failed");
    return NULL;
  }
  task_update(task, TASK_DONE, snap, 0, NULL);
  return NULL;
}

/* Restore worker: safety-snapshot the CURRENT state, then write the chosen
 * snapshot back; roll back to the safety snapshot if the write-in fails. */
static void *
save_restore_worker(void *arg) {
  file_task_t *task = arg;
  char dst[PATH_MAX], snap[PATH_MAX], bak[PATH_MAX];

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(save_escalate() || !find_save_dir(task->src, dst, sizeof(dst))) {
    task_update(task, TASK_FAILED, task->src, 0, "save data not found");
    return NULL;
  }
  snprintf(snap, sizeof(snap), "%s/%s/%s", save_snapshot_dir(), task->src, task->dst);
  if(!file_exists(snap)) {
    task_update(task, TASK_FAILED, snap, 0, "snapshot not found");
    return NULL;
  }
  snprintf(bak, sizeof(bak), "%s" SAVE_PRERESTORE_SUFFIX, dst);
  /* 上一轮的残留（进程被杀）/ 上一次失败的旧副本，先清干净再拍新的。
     ⚠️ 这里原来是 unlink()：对**目录**恒失败，所以安全副本一旦落地就再也不走，
     越攒越多、还会被 /api/save/scan 当成一个「游戏」。 */
  remove_tree(bak);
  if(copy_tree(dst, bak)) {
    task_update(task, TASK_FAILED, dst, 0, "could not safety-snapshot current save");
    return NULL;
  }
  if(copy_tree(snap, dst)) {
    copy_tree(bak, dst);   /* ROLLBACK */
    task_update(task, TASK_FAILED, dst, 0, "restore failed, rolled back");
    return NULL;
  }
  /* 回写成功 = 这次恢复已经结束，安全副本的使命完成：快照才是长期的备份，
     副本留在存档叶里只会占地方（而且用户会把它当成第二个存档）。 */
  remove_tree(bak);
  task_update(task, TASK_DONE, dst, 0, NULL);
  return NULL;
}

enum MHD_Result
api_save_backup(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char src[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  if(!find_save_dir(tid, src, sizeof(src))) {
    free(tid);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  t = queue_task(TASK_COPY, tid, "", tid, save_backup_worker);
  free(tid);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

enum MHD_Result
api_save_restore(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char *snap = req_param(conn, body, body_size, "snapshot");
  char src[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid || !snap || !*snap) {
    free(tid); free(snap);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid snapshot");
  }
  if(!find_save_dir(tid, src, sizeof(src))) {
    free(tid); free(snap);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  t = queue_task(TASK_COPY, tid, snap, tid, save_restore_worker);
  free(tid); free(snap);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

/* ====================================================================== */
/* 存档删除：/api/save/delete（删本机存档）· /api/save/snapdelete（删快照）  */
/* ====================================================================== */

/* Phase log for the irreversible path. The console kills the payload process
 * without a message when it dies, and stdout is lost with it — an append-only
 * file under /data is the only way to know WHICH phase was last seen. Every
 * failure here (no /data, unwritable) is silently ignored: the log is
 * forensics, never a dependency. WFM_SAVE_LOG relocates it (host tests). */
static void
save_debug_log(const char *fmt, ...) {
  const char *path = getenv("WFM_SAVE_LOG");
  char default_path[] = "/data/nexus-save.log";
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

/* 删除本体不再自研递归 —— 改走 filemgr_delete_run()（filemgr.c 导出），
 * 即文件管理「删除」按钮同一条 count_path_bytes + remove_path 管线。
 * 为什么改：存档删除在真机上反复崩（备份/恢复同走 copy_tree 却正常），
 * 自研的 save_rm_task_r 排查三轮未根除；而 filemgr 的删除是这台机器上
 * **被验证过无数次的唯一删除实现**（文件页大目录删除真机稳定）。
 * 与其继续猜内核行为，不如让存档删除跟它共用一份代码 —— 剩下的差异
 * 只剩「对 /user/home 下的路径执行删除」这一个变量，真机再崩也好定位。
 * keep_top 语义保留：live 删除只把**叶目录的子条目**交给删除管线，
 * TITLE_ID 目录本身不由我们 rmdir（内核 save daemon 管理的目录）。 */

static void *
save_delete_worker(void *arg) {
  file_task_t *task = arg;
  char target[PATH_MAX];
  int rc;
  int canceled = 0;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  /* ★★ worker 线程必须自己提权 —— 见下方长注释。 */
  {
    int esc = save_escalate();
    /* 提权结果必须落日志：真机上 rc!=0 = 权限根本没拿到，别再猜。 */
    save_debug_log("delete ESCALATE rc=%d (0=ok)", esc);
  }
  /* ★★ worker 线程必须自己提权 —— 真机日志钉死的结论：
   *   kernel_set_ucred_authid() 只对**调用线程**生效。api_save_delete 在
   *   MHD 线程里调的那次只够 find_save_dir 用；本 worker 是新 pthread，
   *   没有存档 AuthID，copy_tree 一碰 /user/home 进程就被内核带走 ——
   *   真机 /data/nexus-save.log 只停在第一条 "delete live" 行，
   *   SNAPSHOT-OK 永远不出现，且无任何错误码。
   * 备份/恢复 worker 一直正常，正因为它们**在 worker 里**调了
   * save_escalate()。这已是「漏提权」同类 bug 第三犯：
   * ①备份 vs 查询、②扫描恒空、③这次删除 —— 铁律：**每个碰存档域的
   * worker，第一行业务代码之前必须自己 save_escalate()**。
   * 宿主盲区：__linux__ 下本函数 return 0，宿主测试验不出漏调，
   * 防回归只能靠 hosttest 的源码级断言。 */
  snprintf(target, sizeof(target), "%s", task->src);
  if(task->dst[0]) {
    /* ★ live 删除不做内联快照（真机六连崩根因铲除，见下），也不再强制
     *   「必须有快照才能删」—— 用户拍板（2026-10-04）：「不要强制我备份再
     *   删除，可以直接删除也可以备份」。安全网 = 独立「备份」按钮 + 删除前
     *   的 confirm 文案提示，选择权在用户。
     *
     * 历史教训（保留防回归）：强制内联 copy_tree 是真机六连崩的真凶——日志
     * 每次停在 SNAPROOT-READY、SNAPSHOT-OK/FAILED 永不出现 ⇒ 进程在 copy_tree
     * 复制进 /data/savesnap 时被内核带走（fault），删除路径根本没执行到。
     * copy_tree 已从本 worker 彻底移除；删除走 filemgr_delete_run()
     * （与文件管理「删除」同一条被真机验证过无数次的管线，2026-10-04 真机
     * 日志 RM-BEGIN → DONE 实证存活）。
     *
     * ★ keep_top 仍然成立：TITLE_ID 叶目录由内核 save daemon 管理，
     *   RM 阶段只把子条目交给删除管线（下方不变）。 */
    save_debug_log("delete live tid=%s target=%s", task->dst, target);
  } else {
    save_debug_log("delete snapshot target=%s", target);
  }
  /* RM 阶段：交给 filemgr_delete_run() —— 与文件管理「删除」按钮完全同一条
   * count_path_bytes + remove_path 管线（真机上被验证过无数次的唯一删除实现）。
   * live 传叶目录的**子条目**（keep_top：TITLE_ID 目录本身留给内核 save daemon，
   * 不由我们 rmdir）；快照传目标自身（/data/savesnap 是自己的目录，连目录删）。 */
  {
    char **paths = NULL;
    size_t count = 0, i;
    if(task->dst[0]) {
      DIR *d = opendir(target);
      struct dirent *e;
      if(d) {
        while((e = readdir(d))) {
          char child[PATH_MAX];
          char **np;
          if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
          if(snprintf(child, sizeof(child), "%s/%s", target, e->d_name) >=
             (int)sizeof(child)) continue;
          np = realloc(paths, (count + 1) * sizeof(*np));
          if(!np) break;
          paths = np;
          if((paths[count] = strdup(child))) count++;
        }
        closedir(d);
      }
    } else if((paths = malloc(sizeof(*paths)))) {
      if((paths[0] = strdup(target))) count = 1;
    }
    save_debug_log("delete RM-BEGIN kind=%s paths=%zu",
                   task->dst[0] ? "live" : "snapshot", count);
    rc = filemgr_delete_run(task, paths, count);
    /* errno 要在 save_debug_log（fopen 会改写它）之前取样。 */
    canceled = (errno == ECANCELED) || task_cancel_requested(task);
    for(i = 0; i < count; i++) free(paths[i]);
    free(paths);
  }
  if(!rc) {
    save_debug_log("delete DONE target=%s", target);
    task_update(task, TASK_DONE, target, 0, NULL);
  } else if(canceled) {
    save_debug_log("delete CANCELED target=%s", target);
    task_update(task, TASK_CANCELED, target, 0, "canceled");
  } else {
    save_debug_log("delete FAILED rc=%d errno=%s target=%s",
                   rc, strerror(errno), target);
    task_update(task, TASK_FAILED, target, 0, "delete failed");
  }
  return NULL;
}

/* Snapshot names are timestamps produced by save_backup_worker ("%llu").
 * Restricting to digits keeps the resolved path inside the snapshot root no
 * matter what the request body said (no "..", no separators, no absolute). */
static int
save_snapshot_name_ok(const char *name) {
  size_t i;

  if(!name || !*name) return 0;
  for(i = 0; name[i]; i++)
    if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}

/* POST /api/save/delete {title_id} —— 删掉这个游戏的本机存档。
 * ★ 用户拍板（2026-10-04）：不强制先备份，直接删除与先备份两条路都通。
 *   安全网 = 独立「备份」按钮 + 前端 confirm 提示。worker 不做内联快照
 *   （真机六连崩根因，见 save_delete_worker 注释）。 */
enum MHD_Result
api_save_delete(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char live[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  save_escalate();
  if(!find_save_dir(tid, live, sizeof(live))) {
    free(tid);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  /* task->src = 存档绝对路径（worker 删它），task->dst = TITLE_ID（worker
   * 用它落快照目录）。标签只放 TITLE_ID —— /api/tasks 会回显。 */
  t = queue_task_ex(TASK_DELETE, live, tid, tid, NULL, 0, save_delete_worker);
  free(tid);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

/* 批量快照删除 worker。task->src = 逗号分隔的**完整路径**列表（api 层已把
 * 每个名字校验成纯数字时间戳并 stat 过存在，路径是服务端拼的，不是用户原文）。
 * 一次任务删多份快照：前端勾选 N 条只入队一次 —— 逐条入队会撞 "another task
 * is running"，而且列表还没刷新就连点同一行会拿到误导性的 snapshot_not_found。 */
static void *
save_snapdelete_worker(void *arg) {
  file_task_t *task = arg;
  char buf[PATH_MAX];
  char **paths = NULL;
  size_t count = 0, i;
  int rc, canceled = 0;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  save_debug_log("snapdelete BEGIN list=%.260s", task->src);
  snprintf(buf, sizeof(buf), "%s", task->src);
  for(char *p = buf; *p; ) {
    char *comma = strchr(p, ',');
    char **np;
    if(comma) *comma = 0;
    if(*p) {
      np = realloc(paths, (count + 1) * sizeof(*np));
      if(!np || !(paths = np) || !(paths[count] = strdup(p))) break;
      count++;
    }
    p = comma ? comma + 1 : p + strlen(p);
  }
  if(!count) {
    save_debug_log("snapdelete FAILED reason=empty-list");
    task_update(task, TASK_FAILED, task->src, 0, "no snapshots given");
    free(paths);
    return NULL;
  }
  /* /data/savesnap 是本进程自己的目录，不需要存档 AuthID；走与文件页删除
   * 同一条 count_path_bytes + remove_path 管线（filemgr_delete_run）。 */
  rc = filemgr_delete_run(task, paths, count);
  /* errno 要在 save_debug_log（fopen 会改写它）之前取样。 */
  canceled = (errno == ECANCELED) || task_cancel_requested(task);
  for(i = 0; i < count; i++) free(paths[i]);
  free(paths);
  if(!rc) {
    save_debug_log("snapdelete DONE count=%zu", count);
    task_update(task, TASK_DONE, task->current, 0, NULL);
  } else if(canceled) {
    save_debug_log("snapdelete CANCELED");
    task_update(task, TASK_CANCELED, task->current, 0, "canceled");
  } else {
    save_debug_log("snapdelete FAILED rc=%d errno=%s", rc, strerror(errno));
    task_update(task, TASK_FAILED, task->current, 0, "delete failed");
  }
  return NULL;
}

/* POST /api/save/snapdelete {title_id, snapshot|snapshots} —— 删快照（不带存档）。
 * snapshot="111" 单删（兼容旧前端）；snapshots="111,222,333" 批量删（多选）。
 * 快照是用户显式选择丢弃的备份，不做二次快照，但名字必须是纯数字时间戳。 */
enum MHD_Result
api_save_snapdelete(struct MHD_Connection *conn, const char *body,
                    size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char *names = req_param(conn, body, body_size, "snapshots");
  char *single = NULL;
  char list[PATH_MAX];
  char dir[PATH_MAX];
  struct stat st;
  file_task_t *t;
  size_t used = 0, ok_count = 0;

  if(!names || !*names) {
    single = req_param(conn, body, body_size, "snapshot");
    names = single;
  }
  if(!tid || !*tid || !names || !*names) {
    free(tid); free(names);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid snapshot");
  }
  snprintf(dir, sizeof(dir), "%s/%s", save_snapshot_dir(), tid);
  /* 逐个校验：纯数字 + stat 存在且是目录。拼成完整路径的逗号列表交给 worker。
   * 任一不存在 ⇒ 404 并点名是哪一个 —— 用户最烦的「snapshot not found」
   * 不说明是哪条、列表也不刷新，等于让人对着过期行反复撞墙。 */
  list[0] = 0;
  {
    char dup[PATH_MAX];
    char *p, *save = NULL;
    snprintf(dup, sizeof(dup), "%s", names);
    for(p = strtok_r(dup, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
      char target[PATH_MAX];
      int nl;
      if(!save_snapshot_name_ok(p)) {
        free(tid); free(single);
        return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                      "invalid snapshot name", "invalid_snapshot",
                                      NULL);
      }
      nl = snprintf(target, sizeof(target), "%s/%s", dir, p);
      if(nl < 0 || nl >= (int)sizeof(target) || stat(target, &st) ||
         !S_ISDIR(st.st_mode)) {
        free(tid); free(single);
        return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                      "snapshot not found", "snapshot_not_found",
                                      p);
      }
      if(used && used < sizeof(list) - 1) list[used++] = ',';
      nl = snprintf(list + used, sizeof(list) - used, "%s", target);
      if(nl < 0 || nl >= (int)(sizeof(list) - used)) break;  /* 列表装不下就到此为止 */
      used += (size_t)nl;
      ok_count++;
    }
  }
  free(single);
  if(!ok_count) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "no valid snapshots");
  }
  save_debug_log("snapdelete QUEUE tid=%s count=%zu", tid, ok_count);
  /* task->src = 逗号分隔完整路径（worker 解析）；dst 留空 = 不拍快照。 */
  t = queue_task_ex(TASK_DELETE, list, "", tid, NULL, 0, save_snapdelete_worker);
  free(tid);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

/* ====================================================================== */
/* PKG library: /api/pkg/scan · /api/pkg/enqueue · /api/install/poll        */
/* ====================================================================== */

#define NEXUS_DEFAULT_HOMEBREW_DIR "/data/homebrew"
#define PKG_SCAN_MAX_DEPTH 12

/* Where fPKG images are deployed for ShadowMount. Overridable for the host
 * test; on the console it is the real homebrew drop. */
static const char *
homebrew_dir(void) {
  const char *e = getenv("WFM_HOMEBREW_DIR");
  return (e && *e) ? e : NEXUS_DEFAULT_HOMEBREW_DIR;
}

static int
ends_with_ci(const char *s, const char *suffix) {
  size_t n = strlen(s), e = strlen(suffix);
  if(n <= e) return 0;
  for(size_t i = 0; i < e; i++) {
    char a = s[n - e + i];
    if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if(a != suffix[i]) return 0;
  }
  return 1;
}

static int
is_sony_pkg_name(const char *name) {
  return ends_with_ci(name, ".pkg");
}

static int
is_fpkg_name(const char *name) {
  return ends_with_ci(name, ".ffpkg") || ends_with_ci(name, ".ffpfsc") ||
         ends_with_ci(name, ".exfat");
}

static const char *
base_name(const char *path) {
  const char *b = strrchr(path, '/');
  return b ? b + 1 : path;
}

/* File name with the last extension removed. */
static void
stem_of(const char *path, char *out, size_t outsz) {
  const char *base = base_name(path);
  const char *dot = strrchr(base, '.');
  size_t n = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
  if(n >= outsz) n = outsz - 1;
  memcpy(out, base, n);
  out[n] = 0;
}

typedef struct {
  strbuf_t *b;
  int first;
} scan_out_t;

static void
scan_dir(const char *dir, scan_out_t *out, int depth);

static void
scan_emit(const char *full, const char *name, const struct stat *st, scan_out_t *out) {
  int fpkg = is_fpkg_name(name);
  char title_id[16] = {0}, title[256] = {0}, version[32] = {0};
  unsigned long long size = (unsigned long long)st->st_size;
  int have_meta = 0;

  if(fpkg) {
    stem_of(full, title, sizeof(title));
  } else if(is_sony_pkg_name(name)) {
    have_meta = pkg_info_read_meta(full, title_id, sizeof(title_id),
                                   title, sizeof(title), version, sizeof(version),
                                   &size) == 0;
    if(!have_meta) stem_of(full, title, sizeof(title));
  } else {
    return;
  }

  if(!out->first) strbuf_append(out->b, ",");
  out->first = 0;
  strbuf_append(out->b, "{\"title_id\":");
  json_escape(out->b, title_id);
  strbuf_append(out->b, ",\"title\":");
  json_escape(out->b, title);
  strbuf_append(out->b, ",\"version\":");
  json_escape(out->b, version);
  strbuf_printf(out->b, ",\"size\":%llu,\"is_fpkg\":%s,\"path\":",
                size, fpkg ? "true" : "false");
  json_escape(out->b, full);
  strbuf_append(out->b, "}");
}

static void
scan_dir(const char *dir, scan_out_t *out, int depth) {
  DIR *d;
  struct dirent *ent;

  if(depth > PKG_SCAN_MAX_DEPTH) return;
  if(!(d = opendir(dir))) return;
  while((ent = readdir(d))) {
    char full[PATH_MAX];
    struct stat st;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(path_join(full, sizeof(full), dir, ent->d_name) || lstat(full, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      scan_dir(full, out, depth + 1);
    } else if(S_ISREG(st.st_mode) &&
              (is_sony_pkg_name(ent->d_name) || is_fpkg_name(ent->d_name))) {
      scan_emit(full, ent->d_name, &st, out);
    }
  }
  closedir(d);
}

enum MHD_Result
api_pkg_scan(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *root = req_param(conn, body, body_size, "root");
  strbuf_t b = {0};
  scan_out_t out;

  if(!root || !*root) {
    free(root);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid root");
  }
  strbuf_append(&b, "{\"ok\":true,\"packages\":[");
  out.b = &b;
  out.first = 1;
  scan_dir(root, &out, 0);
  strbuf_append(&b, "]}");
  free(root);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* fPKG deploy worker: copy the image into the homebrew dir (ShadowMount picks
 * it up). Progress is byte-based, which the front-end's install dialog reads
 * from total/progress. */
static void *
fpkg_copy_worker(void *arg) {
  file_task_t *task = arg;
  struct stat st;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(stat(task->src, &st) || !S_ISREG(st.st_mode)) {
    task_update(task, TASK_FAILED, task->src, 0, "image not found");
    return NULL;
  }
  mkdir_p(homebrew_dir());
  task->total = (unsigned long long)st.st_size;
  if(copy_tree(task->src, task->dst)) {
    task_update(task, TASK_FAILED, task->dst, 0, "copy to homebrew failed");
    return NULL;
  }
  task_update(task, TASK_DONE, task->dst, task->total, NULL);
  return NULL;
}

enum MHD_Result
api_pkg_enqueue(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  char dst[PATH_MAX];
  file_task_t *t;
  struct stat st;

  if(!path || !*path) {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(stat(path, &st) || !S_ISREG(st.st_mode)) {
    free(path);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "package not found", "file_not_found", NULL);
  }

  if(is_fpkg_name(path)) {
    snprintf(dst, sizeof(dst), "%s/%s", homebrew_dir(), base_name(path));
    t = queue_task(TASK_COPY, path, dst, base_name(path), fpkg_copy_worker);
    free(path);
    if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
    return task_id_response(conn, t->id);
  }

  if(is_sony_pkg_name(path)) {
    unsigned long id = 0;
    int rc = filemgr_queue_pkg(path, &id);
    free(path);
    if(rc == PKG_INSTALL_UNSUPPORTED) {
      return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                    "package installation is only available on PS5",
                                    "pkg_install_unsupported", NULL);
    }
    if(rc) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  "could not queue package installation");
    return task_id_response(conn, id);
  }

  free(path);
  return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                "not a PKG or fPKG image", "pkg_type_invalid", NULL);
}

/* Install straight from a URL (typically http://<pc>:<port>/<file.pkg> served
 * by tools/serve-pkg.py). sceAppInstUtilInstallByPackage accepts http:// URIs
 * and the system installer downloads the file itself (Range requests), so the
 * PKG never touches the console disk. The task queue treats the URL like a
 * local path: pkg_installer_install() passes any non-/data/ string through to
 * metadata.uri verbatim. */
enum MHD_Result
api_pkg_install_url(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *url = req_param(conn, body, body_size, "url");
  unsigned long id = 0;
  int rc;

  if(!url || strlen(url) < sizeof("http://x/") - 1 ||
     strlen(url) > 512 || strncmp(url, "http://", 7) ||
     !memchr(url + 7, '/', strlen(url) - 7)) {
    free(url);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "url must start with http://",
                                  "pkg_url_invalid", NULL);
  }
  rc = filemgr_queue_pkg(url, &id);
  free(url);
  if(rc == PKG_INSTALL_UNSUPPORTED) {
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "package installation is only available on PS5",
                                  "pkg_install_unsupported", NULL);
  }
  if(rc) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "could not queue package installation");
  }
  return task_id_response(conn, id);
}

/* Map an internal task state to the NEXUS front-end's state_name. */
static const char *
task_state_name_nexus(task_state_t s) {
  switch(s) {
    case TASK_QUEUED:   return "queued";
    case TASK_RUNNING:  return "running";
    case TASK_DONE:     return "done";
    case TASK_FAILED:   return "failed";
    case TASK_CANCELED: return "cancelled";
    default:            return "queued";
  }
}

enum MHD_Result
api_install_poll(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *idstr = req_param(conn, body, body_size, "id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task = NULL;
  strbuf_t b = {0};
  int percent = 0;

  free(idstr);
  pthread_mutex_lock(&g_tasks_lock);
  task = find_task_locked(id);
  if(task) {
    percent = task->total ? (int)(task->done * 100 / task->total)
                          : (task->state == TASK_DONE ? 100 : 0);
    /* speed/eta/created_at 必须带上：任务坞的速度/已用/剩余三格全指望它们
     * （fmtSpeed(t)/dockMeta(t)），缺了就是恒 "—" —— 真机上传 PKG 时用户
     * 只能看到字节数看不到速度，就是这里漏的。 */
    strbuf_printf(&b, "{\"ok\":true,\"id\":%lu,\"state_name\":\"%s\","
                      "\"progress\":%llu,\"total\":%llu,\"percent\":%d,"
                      "\"speed\":%llu,\"eta\":%llu,\"created_at\":%lld,"
                      "\"label\":",
                  task->id, task_state_name_nexus(task->state),
                  task->done, task->total, percent,
                  task->speed, task->eta, (long long)task->created_at);
    json_escape(&b, task->current[0] ? task->current : task->src);
    strbuf_append(&b, ",\"note\":");
    json_escape(&b, task->error[0] ? task->error : "");
    if(task->error_code[0]) {
      strbuf_append(&b, ",\"error_code\":");
      json_escape(&b, task->error_code);
      if(task->error_arg[0]) {
        strbuf_append(&b, ",\"error_arg\":");
        json_escape(&b, task->error_arg);
      }
    }
    strbuf_append(&b, "}");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!task) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "task not found");
  }
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}
