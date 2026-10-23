/* savemgr.c — PS5 save manager engine (original implementation)
 *
 * See savemgr.h for the licensing / design-boundary note. This module is an
 * independent reimplementation of save import / replace / export built on the
 * documented PS5 save-data APIs (sceFs*), the /dev/pfsmgr key-decrypt ioctl,
 * and the Sony PSF param.sfo binary format. No third-party source was copied.
 */
#include "savemgr.h"
#include "save_domain.h"
#include "filemgr_internal.h"
#include "json_util.h"
#include "path_util.h"
#include "websrv.h"
#include "mango_log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>

#include "zip_extract.h"
/* 还原要**直接从备份 zip 里逐条目流式搬**到存档区，不经过"整包摊到 /data"
   这一步，所以这里得拿到 minizip 的读接口（locate + 流式 read）。 */
#include "mz.h"
#include "mz_zip.h"
#include "mz_strm.h"
#include "mz_strm_os.h"

#include "sqlite3.h"

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 1024
#endif

/* GCC's warn_unused_result is not silenced by a (void) cast; consuming the
 * value in a comparison is. Use this for I/O we deliberately do not recover
 * from (best-effort cache reads, non-critical writes). */
#define SM_IGNORE(rc) do { if ((rc) < 0) {} } while (0)

/* ---- in-flight import progress log (PFS build / sign / register) ----------
 * Build a PFS image and registering it is a blocking, multi-second job with no
 * step feedback to the caller. While the import request is still running (it
 * holds its own MHD per-connection thread), the browser can issue a PARALLEL
 * request to /api/save_mgr/import_log and read what has happened so far.
 * Host-tested for shape on the Linux build even though the device-only steps
 * return -1 there. */
#define SM_IMPORT_LOG_CAP 8192
static char g_import_log[SM_IMPORT_LOG_CAP];

/* 列表路由的 JSON 缓冲大小。它们曾经是 `char out[16384]` 放在**栈**上 ——
 * 而 websrv 用的是 MHD_USE_THREAD_PER_CONNECTION（每连接一条线程），这
 * 16 KB 就压在**每一条请求线程的栈**上，再叠加 MAX_PATH_LEN 那几个路径数组
 * 和 sfo_info_t（~1.7 KB）：真机线程栈一旦偏小就有压穿的风险。而栈溢出的
 * SIGSEGV 在装上 SA_ONSTACK（见 main.c 的 install_crash_handler）之前是
 * **完全观测不到**的 —— 台账里什么都不留，只剩用户一句"崩了"。
 * 改成堆分配：卸掉栈压力，同时省掉一次 strdup（缓冲本身就是"交出去"的那块）。 */
#define SM_LIST_JSON_CAP 16384
/* 扫描时按 title_id 缓存游戏名的行数上限：一个游戏有几个槽位就开几次 app.db
   太亏，缓存后一个游戏只查一次。超了就退化成每次直查（不影响正确性）。 */
#define SM_LIST_GT_CACHE 64
static size_t g_import_log_len;

/* 结构化进度：前端一次轮询就同时拿到「日志行」和「进度条要的三个数」。
   阶段号是接口契约（assets/index.html 的 startSmLogPoll 按它决定画什么）：
     0 没有任务在跑 · 1 解包（done/total = 已处理字节/总字节）
     2 逐个读取存档信息（done/total = 第几个/共几个）
     3 逐个替换落盘（同上）
   ★ 为什么非得有这个：整包导入可以是分钟级的，只给一句「请勿关页」时用户
   分不清「在干活」和「已经死了」。真机上就有人刷了页面，从此再也连不上。 */
#define SM_IMP_IDLE    0
#define SM_IMP_EXTRACT 1
#define SM_IMP_PROBE   2
#define SM_IMP_APPLY   3

static int  g_import_phase = SM_IMP_IDLE;
static long long g_import_done;
static long long g_import_total;
static char g_import_text[160];
static pthread_mutex_t g_import_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* 追加一行日志（不动进度）。line 已按非空结尾。调用方必须已持锁。 */
static void sm_log_append_locked(const char *line, size_t n) {
  if(g_import_log_len + n + 1 >= SM_IMPORT_LOG_CAP) {
    /* Ring-start the very first step so a runaway import cannot grow forever;
       the last steps always survive. */
    g_import_log_len = 0;
    g_import_log[0] = 0;
  }
  memcpy(g_import_log + g_import_log_len, line, n);
  g_import_log_len += n;
  g_import_log[g_import_log_len++] = '\n';
  g_import_log[g_import_log_len] = 0;
}

void savemgr_import_log(const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if(n < 0) return;
  if((size_t)n > sizeof(line) - 1) n = (int)sizeof(line) - 1;
  pthread_mutex_lock(&g_import_log_lock);
  sm_log_append_locked(line, (size_t)n);
  pthread_mutex_unlock(&g_import_log_lock);
}

/* 记一行 + 更新进度条。done/total 是**展示用**的估计值：total 未知时传 0，
   前端就只显示「当前步骤」的跑马灯而不是百分比（不编造结束时间）。
   口径是 long long —— 整包可以超过 2 GB，int 会翻负号。 */
void savemgr_import_progress(int phase, long long done, long long total,
                             const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if(n < 0) return;
  if((size_t)n > sizeof(line) - 1) n = (int)sizeof(line) - 1;
  pthread_mutex_lock(&g_import_log_lock);
  g_import_phase = phase;
  g_import_done = done;
  g_import_total = total;
  snprintf(g_import_text, sizeof(g_import_text), "%s", line);
  sm_log_append_locked(line, (size_t)n);
  pthread_mutex_unlock(&g_import_log_lock);
}

/* 任务收尾：阶段归零（前端据此收进度条），日志留着让用户回看。
   ⚠️ 每个可能提前 return 的出口都必须走到这里，否则页面会永远停在
   「进行中」—— 这正是「导入失败后进度条卡死」那类反馈的来源。 */
void savemgr_import_progress_end(void) {
  pthread_mutex_lock(&g_import_log_lock);
  g_import_phase = SM_IMP_IDLE;
  g_import_done = 0;
  g_import_total = 0;
  g_import_text[0] = 0;
  pthread_mutex_unlock(&g_import_log_lock);
}

/* 开一条长任务：**清空日志缓冲**再置空闲。
   ⚠️ 为什么必须有它：前端是在发请求**之前**就开窗并开始轮询的，而日志缓冲从
   来不清 —— 于是新窗第一帧会把上一次操作的整段日志先重放一遍（用户看到的
   是「刚点了替换，怎么又在解包整包？」，紧接着又跳回本次任务的行）。本轮
   用户报障里那个「窗口内容对不上操作」就是这么来的。
   这里只清日志与进度三元组，不动别的状态；dispatch 每个长路由前调一次。 */
void savemgr_import_progress_begin(void) {
  pthread_mutex_lock(&g_import_log_lock);
  g_import_log_len = 0;
  g_import_log[0] = 0;
  g_import_phase = SM_IMP_IDLE;
  g_import_done = 0;
  g_import_total = 0;
  g_import_text[0] = 0;
  pthread_mutex_unlock(&g_import_log_lock);
}

/* Emit the accumulated import-progress lines as
   {"lines":["a","b",...],"phase":N,"done":D,"total":T,"text":"..."} */
enum MHD_Result api_save_mgr_import_log(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size) {
  (void)url; (void)body; (void)body_size;
  /* 这条路由是**导入期间被高频轮询**的那一条（进度弹窗每隔几百毫秒来一次），
   * 也就是"确认覆盖、弹出进度窗"那一步真正在跑的代码。8256 字节同样不该压在
   * 请求线程的栈上 —— 理由与 SM_LIST_JSON_CAP 处相同，改用堆。 */
  char *buf = malloc(SM_IMPORT_LOG_CAP + 64);
  if (!buf) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  size_t n = 0;
  int phase;
  long long done, total;
  char text[160];
  pthread_mutex_lock(&g_import_log_lock);
  n = g_import_log_len < SM_IMPORT_LOG_CAP ? g_import_log_len : SM_IMPORT_LOG_CAP;
  if(n) memcpy(buf, g_import_log, n);
  buf[n] = 0;
  phase = g_import_phase;
  done = g_import_done;
  total = g_import_total;
  snprintf(text, sizeof(text), "%s", g_import_text);
  pthread_mutex_unlock(&g_import_log_lock);
  strbuf_t b = {0};
  /* ⚠️ json_escape() **自带两侧引号**（见 src/json_util.c）。这里再手工加一层
     引号会让整个响应变成非法 JSON —— 前端 r.json() 静默失败、.catch 吃掉异常，
     于是「日志弹窗」看起来一直开着却永远空的。它就是这么废掉的：整个导入流程
     明明在写日志，用户却只看到一句「请勿关页」。 */
  strbuf_printf(&b, "{\"phase\":%d,\"done\":%lld,\"total\":%lld,\"text\":",
                phase, done, total);
  json_escape(&b, text);
  strbuf_append(&b, ",\"lines\":[");
  int first = 1;
  char *save = NULL;
  char *tok = strtok_r(buf, "\n", &save);
  while(tok) {
    if(!first) strbuf_append(&b, ",");
    first = 0;
    json_escape(&b, tok);
    tok = strtok_r(NULL, "\n", &save);
  }
  strbuf_append(&b, "]}");
  free(buf);   /* strtok_r 已经切完，这块只是用来切行的临时拷贝 */
  enum MHD_Result r = send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
  return r;
}

/* ---- Sony save-data / kernel prototypes (device-only) ----------------- */
/* MountOpt / UmountOpt / CreateOpt layouts per the working open-source
 * reference (garlic-savemgr), which creates and mounts saves on real hardware
 * with exactly these definitions. CreateOpt is SIX bytes — the 512-byte
 * placeholders this file used before put every field at the wrong offset:
 * copt.flags[1] = 0x02 landed INSIDE blockSize (offset 1 of 4) instead of the
 * flag byte at offset 5, so sceFsCreatePprPfsSaveDataImage was handed a
 * corrupted block size and every import died with "create pfs failed". */
#ifndef __linux__
typedef struct MountOpt {
  uint8_t reserved;
  char *budgetid;
} MountOpt;
typedef struct UmountOpt {
  uint8_t dummy;
} UmountOpt;
typedef struct CreateOpt {
  int blockSize;
  uint8_t flags[2];
} CreateOpt;

int sceFsInitMountSaveDataOpt(MountOpt *opt);
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount, uint8_t *key);
int sceFsInitUmountSaveDataOpt(UmountOpt *opt);
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle, int ignore);
int sceFsInitCreatePfsSaveDataOpt(CreateOpt *opt);
int sceFsCreatePfsSaveDataImage(CreateOpt *opt, const char *path, int x, uint64_t size, uint8_t *key);
int sceFsUfsAllocateSaveData(int fd, uint64_t size, uint64_t flags, int ext);

/* sceFsCreatePprPfsSaveDataImage is NOT in the linkable stub set: the reference
 * resolves it at runtime from libSceFsInternalForVsh.sprx. */
typedef int (*ppr_create_fn)(CreateOpt *opt, const char *path, int x,
                             uint64_t size, uint8_t *key);
static pthread_mutex_t g_ppr_lock = PTHREAD_MUTEX_INITIALIZER;
static ppr_create_fn g_ppr_create;

int sceRegMgrGetStr(int key, char *out, size_t len);
int sceRegMgrGetBin(int key, void *out, size_t len);
int sceUserServiceGetForegroundUser(int *user_id);
/* UserService 必须先 Initialize 才能用 —— 口径取自 n0llptr/Playstation-5-Save-Mounter
 * 的 mounter_payload（GPLv3，其 main() 启动时显式调用）。未初始化时
 * sceUserServiceGetForegroundUser 不报错，只是把 *user_id 留在 0。 */
int sceUserServiceInitialize(const int *priority);
#endif

/* ============================================================ helpers === */

static int recursive_mkdir(const char *path) {
  char tmp[MAX_PATH_LEN];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  return mkdir(tmp, 0777);
}

static int copy_file(const char *src, const char *dst)
  __attribute__((unused)); /* device-only helper; not used on host build */
static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (out < 0) { close(in); return -1; }
  /* ⚠️ 缓冲必须在**堆**上。这个函数唯一的非导入调用点是
   * sm_mount_image_inner() —— 而它跑在 sm_kern_run() 起的那条**看护线程**里
   * （pthread_create 默认栈）。64KB 压在那条栈上，一旦默认栈比预期小就是
   * 栈溢出：表现是「点一下进程就没了」，且崩溃点离真正的挂载调用很远，
   * 光看台账根本定位不到。 */
  uint8_t *buf = malloc(65536);
  if (!buf) { close(in); close(out); return -1; }
  ssize_t n;
  while ((n = read(in, buf, 65536)) > 0) {
    ssize_t w = 0;
    while (w < n) {
      ssize_t r = write(out, buf + w, (size_t)(n - w));
      if (r <= 0) { free(buf); close(in); close(out); return -1; }
      w += r;
    }
  }
  free(buf);
  close(in);
  close(out);
  return 0;
}

/* Extract a query parameter (?key=value&) from a URL string. Returns a
 * static buffer (caller must copy). */
static const char *url_param(const char *url, const char *key) {
  static char out[512];
  out[0] = 0;
  char needle[64];
  snprintf(needle, sizeof(needle), "%s=", key);
  const char *p = strstr(url, needle);
  if (!p) return out;
  p += strlen(needle);
  int i = 0;
  while (*p && *p != '&' && i < (int)sizeof(out) - 1)
    out[i++] = *p++;
  out[i] = 0;
  return out;
}

/* Robust parameter lookup. ⚠️ MHD hands the access handler a `url` that does
 * NOT carry the query string, so url_param(url, ...) silently returns "" for
 * every ?key= the front end sends — a fault that only shows up as "the
 * parameter is ignored" (e.g. /api/save_mgr/files?path= always listing the
 * mount root). req_param() reads the JSON body, then the form body, then MHD's
 * GET arguments, which is where the query string actually lives. The raw url
 * is kept only as a fallback for hosts that splice the query in.
 * Caller frees; NULL when absent. */
static char *sm_param(struct MHD_Connection *conn, const char *url,
                      const char *body, size_t body_size, const char *key) {
  char *v = req_param(conn, body, body_size, key);
  if(v) return v;
  {
    const char *u = url_param(url, key);
    return (u && u[0]) ? strdup(u) : NULL;
  }
}

/* ============================================================ PSF ====== */

int savemgr_parse_sfo(const char *path, sfo_info_t *out) {
  memset(out, 0, sizeof(*out));
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 20) { close(fd); return -1; }

  uint8_t *buf = malloc(st.st_size);
  if (!buf) { close(fd); return -1; }
  if (read(fd, buf, st.st_size) != st.st_size) { free(buf); close(fd); return -1; }
  close(fd);

  /* all-zero => corrupted / zeroed SFO (homebrew-created images) */
  int all_zero = 1;
  size_t check = st.st_size < 256 ? (size_t)st.st_size : 256;
  for (size_t i = 0; i < check; i++)
    if (buf[i]) { all_zero = 0; break; }
  if (all_zero) { free(buf); return -2; }

  /* magic: 0x00 'P' 'S' 'F' */
  if (buf[0] != 0x00 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') {
    free(buf); return -1;
  }

  uint32_t key_off, data_off, n_entries;
  size_t fsize = (size_t)st.st_size;
  memcpy(&key_off, buf + 8, 4);
  memcpy(&data_off, buf + 12, 4);
  memcpy(&n_entries, buf + 16, 4);

  /* ★ 这是一个**不可信输入**解析器。param.sfo 来自用户导入的存档镜像，也
   * 可能是 /user/home 下被第三方工具写坏的文件，而 /api/save_mgr/list 会
   * 扫过整库的每一个 param.sfo —— 只要有一个坏掉，整个进程就没了。
   * 2026-10-11 真机台账正是这样：三次 SIGSEGV，最后一条请求都是
   * POST /api/save_mgr/list。当时这里把三个偏移直接当指针/循环边界用：
   *     buf + key_off + koff   -> strcmp 一路读到堆外
   *     buf + data_off + doff  -> 同上
   *     buf + 20 + i * 16      -> 目录项循环完全不参考文件大小
   * 现在每个偏移都夹进 [0, fsize]，每个长度都夹到剩余空间。
   * 判据是 .build/sfo_unit_test.c（四个畸形输入，ASan 下必须全绿）。 */
  if (key_off >= fsize || data_off >= fsize) { free(buf); return -1; }
  if (n_entries > (fsize - 20) / 16) { free(buf); return -1; }

  for (uint32_t i = 0; i < n_entries; i++) {
    uint8_t *idx = buf + 20 + i * 16;   /* n_entries 已按 fsize 夹过 */
    uint16_t koff;
    uint32_t param_len, doff;
    size_t kpos, dpos, kavail, davail;
    const char *key;
    uint8_t *val;

    memcpy(&koff, idx, 2);
    memcpy(&param_len, idx + 4, 4);
    memcpy(&doff, idx + 12, 4);

    kpos = (size_t)key_off + koff;
    if (kpos >= fsize) continue;
    kavail = fsize - kpos;
    key = (const char *)(buf + kpos);
    if (strnlen(key, kavail) == kavail) continue;   /* 没终止符：丢弃这条 */

    dpos = (size_t)data_off + doff;
    if (dpos >= fsize) continue;
    davail = fsize - dpos;
    val = buf + dpos;
    if (param_len > davail) param_len = (uint32_t)davail;          /* 夹到剩余空间 */
    if (param_len > (uint32_t)INT_MAX) param_len = (uint32_t)INT_MAX;  /* 别让 (int) 变负 */

    if (strcmp(key, "TITLE_ID") == 0)
      snprintf(out->title_id, sizeof(out->title_id), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SAVEDATA_DIRECTORY") == 0)
      snprintf(out->dir_name, sizeof(out->dir_name), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "MAINTITLE") == 0 || strcmp(key, "TITLE") == 0)
      snprintf(out->main_title, sizeof(out->main_title), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SUBTITLE") == 0)
      snprintf(out->sub_title, sizeof(out->sub_title), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "DETAIL") == 0)
      snprintf(out->detail, sizeof(out->detail), "%.*s", (int)param_len, (char *)val);
    else if (strcmp(key, "SAVEDATA_BLOCKS") == 0 && param_len >= 4)
      memcpy(&out->blocks, val, 4);
    else if (strcmp(key, "PARAMS") == 0 || strcmp(key, "USER_PARAM") == 0) {
      if (param_len >= 4) memcpy(&out->user_param, val, 4);
    } else if (strcmp(key, "ACCOUNT_ID") == 0 && param_len >= 8)
      memcpy(&out->account_id, val, 8);
    else if (strcmp(key, "USER_ID") == 0 && param_len >= 4)
      memcpy(&out->user_id, val, 4);
    else if (strcmp(key, "FORMAT") == 0)
      snprintf(out->format, sizeof(out->format), "%.*s", (int)param_len, (char *)val);
  }
  if (!out->game_title_id[0])
    strncpy(out->game_title_id, out->title_id, sizeof(out->game_title_id) - 1);

  free(buf);
  return 0;
}

static uint8_t g_params_hmac[32] = {0};
static int g_params_hmac_valid = 0;

void savemgr_cache_params_hmac(const char *sfo_path) {
  if (g_params_hmac_valid) return;
  int fd = open(sfo_path, O_RDONLY);
  if (fd < 0) return;
  uint8_t hdr[20];
  if (read(fd, hdr, 20) != 20) { close(fd); return; }
  if (hdr[0] != 0 || hdr[1] != 'P' || hdr[2] != 'S' || hdr[3] != 'F') { close(fd); return; }
  uint32_t koff, doff, nent;
  memcpy(&koff, hdr + 8, 4);
  memcpy(&doff, hdr + 12, 4);
  memcpy(&nent, hdr + 16, 4);
  for (uint32_t i = 0; i < nent; i++) {
    uint8_t idx[16];
    SM_IGNORE(pread(fd, idx, 16, 20 + i * 16));
    uint16_t ko; memcpy(&ko, idx, 2);
    char kname[32] = {0};
    SM_IGNORE(pread(fd, kname, 31, koff + ko));
    if (strcmp(kname, "PARAMS") == 0) {
      uint32_t d_off; memcpy(&d_off, idx + 12, 4);
      uint8_t cand[32];
      SM_IGNORE(pread(fd, cand, 32, doff + d_off + 0x08));
      int valid = 0;
      for (int b = 0; b < 32; b++) if (cand[b]) { valid = 1; break; }
      if (valid) {
        memcpy(g_params_hmac, cand, 32);
        g_params_hmac_valid = 1;
      }
      break;
    }
  }
  close(fd);
}

int savemgr_generate_sfo(const char *path, const sfo_info_t *info) {
  /* SFO header + 16-byte index entries + key table + data table.
   * fmt: 0x0004 = raw binary, 0x0204 = utf8 string, 0x0404 = uint32. */
  uint8_t buf[4096];
  memset(buf, 0, sizeof(buf));

  const char *keys[] = {
    "ACCOUNT_ID", "ATTRIBUTE", "CATEGORY", "DETAIL", "FORMAT",
    "LINEAGE_ID", "MAINTITLE", "PARAMS", "SAVEDATA_BLOCKS",
    "SAVEDATA_DIRECTORY", "SAVEDATA_LIST_PARAM", "SAVEDATA_REVISION",
    "SUBTITLE", "SYSTEM_BLOCKS", "TITLE_ID"
  };
  int nkeys = 15;

  uint32_t key_off = 20 + nkeys * 16;
  uint32_t key_total = 0;
  for (int i = 0; i < nkeys; i++) key_total += (uint32_t)strlen(keys[i]) + 1;
  uint32_t data_off = key_off + key_total;
  if (data_off % 4) data_off += 4 - (data_off % 4);

  buf[0] = 0x00; buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
  buf[4] = 0x01; buf[5] = 0x01;
  memcpy(buf + 8, &key_off, 4);
  memcpy(buf + 12, &data_off, 4);
  uint32_t ne = nkeys;
  memcpy(buf + 16, &ne, 4);

  uint32_t kpos = 0;
  for (int i = 0; i < nkeys; i++) {
    size_t kl = strlen(keys[i]) + 1;
    memcpy(buf + key_off + kpos, keys[i], kl);
    kpos += (uint32_t)kl;
  }

  int is_ps4 = (strcmp(info->format, "obs") == 0);
  const char *fmt_str = is_ps4 ? "obs" : "ppr";

  uint32_t dpos = 0;
  kpos = 0;
  for (int i = 0; i < nkeys; i++) {
    uint8_t *idx = buf + 20 + i * 16;
    uint16_t ko = (uint16_t)kpos;
    memcpy(idx, &ko, 2);

    uint32_t param_len = 0, param_max = 0;
    uint16_t param_fmt = 0x0204;

    if (strcmp(keys[i], "ACCOUNT_ID") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      memcpy(buf + data_off + dpos, &info->account_id, 8);
    } else if (strcmp(keys[i], "ATTRIBUTE") == 0) {
      param_fmt = 0x0404; param_len = 4; param_max = 4;
    } else if (strcmp(keys[i], "CATEGORY") == 0) {
      param_len = 3; param_max = 4;
      memcpy(buf + data_off + dpos, "sd", 3);
    } else if (strcmp(keys[i], "DETAIL") == 0) {
      param_len = (info->detail[0] ? (uint32_t)strlen(info->detail) + 1 : 1);
      param_max = 1024;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, info->detail, param_len);
    } else if (strcmp(keys[i], "FORMAT") == 0) {
      param_len = 3; param_max = 4;
      memcpy(buf + data_off + dpos, fmt_str, 3);
    } else if (strcmp(keys[i], "LINEAGE_ID") == 0) {
      param_fmt = 0x0004; param_len = 16; param_max = 16;
    } else if (strcmp(keys[i], "MAINTITLE") == 0) {
      const char *t = info->main_title[0] ? info->main_title : info->title_id;
      param_len = (uint32_t)strlen(t) + 1; param_max = 128;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, t, param_len);
    } else if (strcmp(keys[i], "PARAMS") == 0) {
      param_fmt = 0x0004; param_len = 1024; param_max = 1024;
      uint8_t *p = buf + data_off + dpos;
      uint32_t uid = info->user_id;
      memcpy(p + 0x04, &uid, 4);
      if (g_params_hmac_valid) memcpy(p + 0x08, g_params_hmac, 32);
      uint32_t flags = 1;
      memcpy(p + 0x28, &flags, 4);
      strncpy((char *)(p + 0x2C), info->title_id, 12);
      const char *gtid = info->game_title_id[0] ? info->game_title_id : info->title_id;
      strncpy((char *)(p + 0x38), gtid, 12);
      uint32_t unk2 = 2;
      memcpy(p + 0x54, &unk2, 4);
    } else if (strcmp(keys[i], "SAVEDATA_BLOCKS") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      uint64_t b = info->blocks ? info->blocks : 96;
      memcpy(buf + data_off + dpos, &b, 8);
    } else if (strcmp(keys[i], "SAVEDATA_DIRECTORY") == 0) {
      const char *d = info->dir_name[0] ? info->dir_name : "savedata0";
      param_len = (uint32_t)strlen(d) + 1; param_max = 32;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, d, param_len);
    } else if (strcmp(keys[i], "SAVEDATA_LIST_PARAM") == 0) {
      param_fmt = 0x0404; param_len = 4; param_max = 4;
    } else if (strcmp(keys[i], "SAVEDATA_REVISION") == 0) {
      param_fmt = 0x0004; param_len = 16; param_max = 16;
    } else if (strcmp(keys[i], "SUBTITLE") == 0) {
      const char *st = info->sub_title[0] ? info->sub_title : "";
      param_len = (uint32_t)strlen(st) + 1; param_max = 128;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, st, param_len);
    } else if (strcmp(keys[i], "SYSTEM_BLOCKS") == 0) {
      param_fmt = 0x0004; param_len = 8; param_max = 8;
      uint64_t sb = info->system_blocks;
      memcpy(buf + data_off + dpos, &sb, 8);
    } else if (strcmp(keys[i], "TITLE_ID") == 0) {
      param_len = (uint32_t)strlen(info->title_id) + 1; param_max = 12;
      if (param_len > param_max) param_len = param_max;
      memcpy(buf + data_off + dpos, info->title_id, param_len);
    }

    memcpy(idx + 2, &param_fmt, 2);
    memcpy(idx + 4, &param_len, 4);
    memcpy(idx + 8, &param_max, 4);
    memcpy(idx + 12, &dpos, 4);
    dpos += param_max;
    kpos += (uint32_t)strlen(keys[i]) + 1;
  }

  uint32_t total = data_off + dpos;
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return -1;
  SM_IGNORE(write(fd, buf, total));
  close(fd);
  return 0;
}

/* 就地改写 param.sfo 的 ACCOUNT_ID / USER_ID / PARAMS[0x04]，**其余字节一个都不动**。
 * 返回被写入的字段数（0..3），负数为失败（读不动 / 不是 SFO / 偏移越界）。
 *
 * 为什么必须有这条路（真机实证，2026-10-12）：
 *   v1.0.21 台账显示整条导入链全绿（3/3 mount rc=0 / sfo pr=0 / finalize rc=0），
 *   但**进游戏提示「游戏数据损坏」、存档没了**。根因是旧的 savemgr_resign_sfo()
 *   无条件整文件重写 param.sfo —— 用 15 键白名单重新合成一个「标准 SFO」，
 *   于是原厂文件里凡是我们没解析的字段全被清零：
 *       SYSTEM_BLOCKS          parse_sfo 根本不读 ⇒ 恒写 0
 *       LINEAGE_ID / SAVEDATA_REVISION / ATTRIBUTE / SAVEDATA_LIST_PARAM
 *                              生成时只声明长度、从不写数据 ⇒ 全 0
 *       PARAMS（1024 字节）     只填 0x04/0x08/0x28/0x2C/0x38/0x54 六处，
 *                              其余 900+ 字节归零
 *   键白名单本身和 Garlic 逐字相同（两边都是那 15 个键），所以问题不在"少写了
 *   哪个键"，而在"**什么时候该重写**"。
 *
 * 对照 Garlic（唯一真调 sceFs* 的 GPLv3 参考实现）得出来的口径：
 *   - generate_sfo 只在 parse_sfo 返回 **-2（SFO 全零）** 时调用；
 *   - 正常导入（解析成功）**根本不重建 SFO**，原样保留；
 *   - 只有用户主动改归属时才 main.c:5601 那样 `pwrite(fd, new_aid, 8, ...)`
 *     就地改 8 个字节，再 sync()。
 * 我们原来是无条件重建的那个 —— 这就是唯一偏差。
 *
 * 比 Garlic 更稳的一点：它用的是硬编码偏移 0x1B8（只对它自己生成过的 SFO 成立），
 * 我们按**键名**在索引表里定位，对任意布局的原厂 SFO 都成立。
 *
 * ⚠️ param.sfo 是**不可信输入**（来自用户导入的镜像），这里沿用 savemgr_parse_sfo
 * 那套夹取纪律：每个偏移夹进 [0, fsize]、每段长度夹到剩余空间，否则 pread/pwrite
 * 会越过文件边界。 */
static int sm_sfo_patch_inplace(const char *path, uint64_t aid, uint32_t uid) {
  int fd = open(path, O_RDWR);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 20) { close(fd); return -1; }
  size_t fsize = (size_t)st.st_size;

  uint8_t hdr[20];
  if (read(fd, hdr, 20) != 20) { close(fd); return -1; }
  if (hdr[0] != 0x00 || hdr[1] != 'P' || hdr[2] != 'S' || hdr[3] != 'F') {
    close(fd);
    return -1;
  }

  uint32_t key_off, data_off, n_entries;
  memcpy(&key_off, hdr + 8, 4);
  memcpy(&data_off, hdr + 12, 4);
  memcpy(&n_entries, hdr + 16, 4);
  if (key_off >= fsize || data_off >= fsize) { close(fd); return -1; }
  if (n_entries > (fsize - 20) / 16) { close(fd); return -1; }

  int written = 0;
  for (uint32_t i = 0; i < n_entries; i++) {
    uint8_t idx[16];
    if (pread(fd, idx, 16, 20 + (off_t)i * 16) != 16) break;

    uint16_t koff;
    uint32_t param_len, doff;
    memcpy(&koff, idx, 2);
    memcpy(&param_len, idx + 4, 4);
    memcpy(&doff, idx + 12, 4);

    size_t kpos = (size_t)key_off + koff;
    if (kpos >= fsize) continue;
    char key[64];
    memset(key, 0, sizeof(key));           /* 读不满也保证有终止符 */
    if (pread(fd, key, sizeof(key) - 1, (off_t)kpos) <= 0) continue;

    size_t dpos = (size_t)data_off + doff;
    if (dpos >= fsize) continue;

    if (strcmp(key, "ACCOUNT_ID") == 0) {
      if (param_len >= 8 && dpos + 8 <= fsize &&
          pwrite(fd, &aid, 8, (off_t)dpos) == 8)
        written++;
    } else if (strcmp(key, "USER_ID") == 0) {
      /* PS4/PS5 部分存档把 uid 放在独立键里（Garlic 的 parse 也读这个键）。 */
      if (param_len >= 4 && dpos + 4 <= fsize &&
          pwrite(fd, &uid, 4, (off_t)dpos) == 4)
        written++;
    } else if (strcmp(key, "PARAMS") == 0) {
      /* Garlic 的 PARAMS 内部结构（generate_sfo 里逐字节对照过）：0x04 = user_id。 */
      if (param_len >= 8 && dpos + 8 <= fsize &&
          pwrite(fd, &uid, 4, (off_t)(dpos + 0x04)) == 4)
        written++;
    }
  }
  close(fd);
  return written;
}

int savemgr_resign_sfo(const char *sfo_path, int is_ps4, uint64_t aid,
                       uint32_t uid, const sfo_info_t *hint) {
  sfo_info_t info;
  int r = savemgr_parse_sfo(sfo_path, &info);

  /* ── 正常存档：就地改字节，绝不重建 ────────────────────────────── */
  if (r == 0) {
    /* 本机本用户的备份：ACCOUNT_ID 与 uid 本来就是对的 ⇒ 一个字节都不写。
       这是「把存档原样还原」的最强保证，也正是 Garlic 正常导入的行为。 */
    if (info.account_id == aid && info.user_id == uid) {
      mango_log("resign: %s already bound to aid=%llu uid=%u -> untouched",
                sfo_path, (unsigned long long)aid, uid);
      return 0;
    }
    int n = sm_sfo_patch_inplace(sfo_path, aid, uid);
    if (n < 0) return -1;
    /* Garlic 改完就地 sync()（main.c:5603）：挂载点上的写要落回镜像文件，
       而 finalize 是在 unmount 之后才拷这个文件的。 */
    sync();
    mango_log("resign: %s patched %d field(s) -> aid=%llu uid=%u",
              sfo_path, n, (unsigned long long)aid, uid);
    return 0;
  }
  if (r < 0 && r != -2) return r;

  /* ── SFO 全零 / 读不出来：只能合成重建 ───────────────────────────
     调用方手上有从包内目录名推出来的 title_id / dir_name —— 镜像里没有可用
     SFO 时这是**唯一**的来源。旧实现在这里 memset 成 "UNKNOWN"，于是 TITLE_ID
     被写成 UNKNOWN、存档落进错误目录（真机上表现为「导入成功但游戏里没有」）。 */
  memset(&info, 0, sizeof(info));
  if (hint) info = *hint;                 /* 调用方填了多少用多少 */
  else strncpy(info.title_id, "UNKNOWN", sizeof(info.title_id) - 1);
  info.account_id = aid;
  info.user_id = uid;
  snprintf(info.format, sizeof(info.format), "%s", is_ps4 ? "obs" : "ppr");
  if (!info.game_title_id[0])
    strncpy(info.game_title_id, info.title_id, sizeof(info.game_title_id) - 1);
  mango_log("resign: %s rebuild (sfo rc=%d) tid=%s dir=%s", sfo_path, r,
            info.title_id, info.dir_name);
  return savemgr_generate_sfo(sfo_path, &info);
}

/* ============================================================ zip ====== */

/* 路径映射在文件后半段定义（宿主上用 WFM_DATA_ROOT / WFM_SAVE_ROOT 重定向），
   这里提前声明：解包的临时文件也必须落在被映射的目录里。2026-10-10 加批量
   归档导入时，宿主测试第一次真的走到了这里 —— 原来写死的 "/data/Mango/tmp"
   会让测试去写真实根文件系统的 /data（WSL 下是 root，还真写成功了），
   既污染环境又让「解包落哪儿」这件事在两套环境下不是同一个答案。 */
static void sm_map_path(const char *in, char *out, size_t outsz);
static void sm_arc_rm_rf(const char *path);   /* 定义在归档段，启动清理要先用到 */
static long sm_read_file(const char *path, char *buf, size_t cap); /* 定义在
文件后半段：游戏名缓存（sm_title_cache_*）在前半段就要用它读 titles.json。 */
static void sm_json_esc(const char *in, char *out, size_t outsz); /* 同上：
sm_title_cache_put 写 titles.json 时要转义游戏名（文件后半段才定义）。 */
static void sm_json_str(const char *j, const char *key, char *out,
                        size_t outsz); /* 同上：appmeta 名字兜底读
defaultLanguage 用（文件后半段才定义）。 */


/* 上一次解包失败的原因（status + 出问题的条目名），由 savemgr_extract_zip 填。
 * 真机上没有调试器，只回一句「解包失败」等于没回 —— 排障时要知道是"不是 zip"、
 * "条目名不安全" 还是 "磁盘满"。 */
static char g_zip_err[256];

/* zipx 解包进度 → 日志行 + 进度条。zipx 自己已按 ~200ms / 1MiB 节流，这里再
   按「整百分点跨 10」压一道 —— 8KB 的日志环装不下逐文件刷屏，压过了就会把
   前面的步骤挤掉，用户反而看不到全程。 */
static int g_zip_prog_phase = -1;
static int g_zip_prog_pct = -1;
static pthread_mutex_t g_zip_prog_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *
sm_zip_phase_name(int phase) {
  switch (phase) {
    case ZIPX_PHASE_SCAN:    return "扫描条目";
    case ZIPX_PHASE_EXTRACT: return "解压";
    case ZIPX_PHASE_PUBLISH: return "落盘";
    case ZIPX_PHASE_CLEANUP: return "收尾";
    default:                 return "处理";
  }
}

static void
sm_zip_on_progress(void *ud, const zipx_progress_t *p) {
  int pct = (p->bytes_total > 0)
              ? (int)((p->bytes_done * 100) / p->bytes_total) : -1;
  int emit;
  char text[160];

  (void)ud;
  pthread_mutex_lock(&g_zip_prog_lock);
  emit = (p->phase != g_zip_prog_phase) || pct < 0 || g_zip_prog_pct < 0 ||
         pct >= g_zip_prog_pct + 10;
  g_zip_prog_phase = p->phase;
  g_zip_prog_pct = pct;
  pthread_mutex_unlock(&g_zip_prog_lock);
  if (!emit) return;

  if (pct >= 0)
    snprintf(text, sizeof(text), "解包（%s）%d%%：%s",
             sm_zip_phase_name(p->phase), pct,
             p->current ? p->current : "…");
  else
    snprintf(text, sizeof(text), "解包（%s）：%s",
             sm_zip_phase_name(p->phase), p->current ? p->current : "…");

  /* done/total 走字节口径 —— 整包几个 GB 时这是唯一有意义的「还要多久」。 */
  savemgr_import_progress(SM_IMP_EXTRACT, (long long)p->bytes_done,
                          (long long)p->bytes_total, "%s", text);
}

/* 真正解包：源是**盘上的** ZIP 路径。三处 ZIP 入口都走这里，任何一处都不再
   把整包读进内存 —— 真机上几百 MB 的整包先 malloc(zsize) 再写一份临时副本，
   OOM / 把 /data 写爆就是这么来的（用户反馈「进程崩了、自启后功能都不能用」）。 */
int savemgr_extract_zip_path(const char *dest, const char *zip_path) {
  zipx_result_t result;

  recursive_mkdir(dest);
  pthread_mutex_lock(&g_zip_prog_lock);
  g_zip_prog_phase = -1;
  g_zip_prog_pct = -1;
  pthread_mutex_unlock(&g_zip_prog_lock);

  memset(&result, 0, sizeof(result));
  zipx_status_t st = zipx_extract(zip_path, dest, ZIPX_CONFLICT_OVERWRITE,
                                  NULL, NULL, sm_zip_on_progress, NULL, NULL,
                                  &result);
  /* 分档返回，别一律 -1：真机上「不是 zip / 包坏了 / 磁盘满」必须分得开，
   * 否则排障只能靠猜（调用方拿到 -100-st 就能反查 zipx_status_t）。 */
  if (st != ZIPX_OK) {
    snprintf(g_zip_err, sizeof(g_zip_err), "%s: %s%s%s", zipx_status_string(st),
             result.message[0] ? result.message : "?",
             result.detail[0] ? " @ " : "", result.detail);
    return -100 - (int)st;
  }
  g_zip_err[0] = 0;
  return (int)result.files_created;
}

/* ---- 还原通道：从备份 zip 里**逐条目流式搬运** -------------------------
 * ★ 为什么不再「先整包解压到 /data/Mango/tmp 再拷过去」：
 *   还原要搬的是几百 MB 的 sdimg_，整包摊在 /data 上等于把存档体积在 /data
 *   上**再写一遍** —— /data 写爆的后果真机已经见过：进程直接没了、杀不掉、
 *   自启后功能全废（2026-10-13「备份完再点还原就崩」）。
 *   备份侧是流式直读、从不摊包，还原没理由反过来摊。
 * ★ 用 minizip 直接 `mz_zip_locate_entry()` 定位 + 流式读：/data 上只多出
 *   zip 自己（它本来就在那儿）。附带好处：少一次全量落盘，还原更快。
 * ★ CRC 由 minizip 在读的过程中校验；坏条目一律**不落盘**（返回 -1），
 *   宁可还原失败，也不能把一份坏镜像盖到存档区。 */

static int sm_zip_open_read(const char *path, void **zip_out, void **stm_out) {
  void *stm = mz_stream_os_create();
  void *zip = mz_zip_create();
  *zip_out = NULL;
  *stm_out = NULL;
  if (!stm || !zip) {
    if (stm) mz_stream_delete(&stm);
    if (zip) mz_zip_delete(&zip);
    return -1;
  }
  if (mz_stream_open(stm, path, MZ_OPEN_MODE_READ) != MZ_OK ||
      mz_zip_open(zip, stm, MZ_OPEN_MODE_READ) != MZ_OK) {
    mz_zip_close(zip);
    mz_zip_delete(&zip);
    mz_stream_close(stm);
    mz_stream_delete(&stm);
    return -1;
  }
  *zip_out = zip;
  *stm_out = stm;
  return 0;
}

static void sm_zip_close_read(void *zip, void *stm) {
  if (zip) { mz_zip_close(zip); mz_zip_delete(&zip); }
  if (stm) { mz_stream_close(stm); mz_stream_delete(&stm); }
}

/* 小条目（manifest.json）读进内存。out 保证以 0 结尾。 */
static int sm_zip_read_small(void *zip, const char *entry, char *out,
                             size_t outsz) {
  int32_t n = 0;
  size_t got = 0;

  out[0] = 0;
  if (mz_zip_locate_entry(zip, entry, 0) != MZ_OK) return -1;
  if (mz_zip_entry_read_open(zip, 0, NULL) != MZ_OK) return -1;
  while (got + 1 < outsz) {
    n = mz_zip_entry_read(zip, out + got, (int32_t)(outsz - 1 - got));
    if (n <= 0) break;
    got += (size_t)n;
  }
  mz_zip_entry_read_close(zip, NULL, NULL, NULL);
  out[got] = 0;
  return n < 0 ? -1 : 0;
}

/* 把一个条目流式写成 dst（64KB 堆缓冲；写失败/CRC 错都返回 -1）。 */
static int sm_zip_write_entry(void *zip, const char *entry, const char *dst) {
  uint32_t crc = 0;
  int64_t csz = 0, usz = 0;
  int fd, rc = 0, bad = 0;

  if (mz_zip_locate_entry(zip, entry, 0) != MZ_OK) return -1;
  if (mz_zip_entry_read_open(zip, 0, NULL) != MZ_OK) return -1;
  fd = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    mz_zip_entry_read_close(zip, &crc, &csz, &usz);
    return -1;
  }
  uint8_t *buf = malloc(65536);   /* 堆：请求线程栈只有 2MB，别压上去 */
  if (!buf) {
    close(fd);
    mz_zip_entry_read_close(zip, &crc, &csz, &usz);
    return -1;
  }
  while (rc == 0) {
    int32_t n = mz_zip_entry_read(zip, buf, 65536);
    if (n == 0) break;
    if (n < 0) { rc = -1; bad = 1; break; }   /* CRC/格式错 ⇒ 不落盘 */
    ssize_t w = 0;
    while (w < n) {
      ssize_t r = write(fd, buf + w, (size_t)(n - w));
      if (r <= 0) { rc = -1; break; }
      w += r;
    }
  }
  free(buf);
  close(fd);
  mz_zip_entry_read_close(zip, &crc, &csz, &usz);
  if (bad || rc != 0) unlink(dst);   /* 半截文件绝不能留在存档区 */
  return rc;
}

int savemgr_extract_zip(const char *dest, const uint8_t *data, size_t len) {
  char tmp[MAX_PATH_LEN];
  char tmp_dir[MAX_PATH_LEN];

  recursive_mkdir(dest);
  sm_map_path("/data/Mango/tmp", tmp_dir, sizeof(tmp_dir));
  recursive_mkdir(tmp_dir);
  snprintf(tmp, sizeof(tmp), "%s/_nex_zip_%ld.tmp", tmp_dir, (long)getpid());
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return -2;                    /* 摊包临时文件建不出来（目录/权限） */
  size_t written = 0;
  while (written < len) {
    ssize_t w = write(fd, data + written, len - written);
    if (w <= 0) { close(fd); unlink(tmp); return -3; }   /* 写不进去（磁盘满？） */
    written += (size_t)w;
  }
  close(fd);

  int rc = savemgr_extract_zip_path(dest, tmp);
  unlink(tmp);
  return rc;
}

const char *savemgr_last_zip_error(void) { return g_zip_err; }

/* ============================================================ account == */

uint64_t savemgr_account_id_for_user(const char *uid_hex) {
#ifdef __linux__
  (void)uid_hex;
  return 0;
#else
  char upath[MAX_PATH_LEN];
  snprintf(upath, sizeof(upath), "/user/home/%s/username.dat", uid_hex);
  char username[17] = {0};
  int fd = open(upath, O_RDONLY);
  if (fd >= 0) { SM_IGNORE(read(fd, username, 16)); username[16] = 0; close(fd); }
  if (!username[0]) return 0;
  for (int i = 1; i <= 16; i++) {
    int key_name = (i - 1) * 65536 + 125829632;
    char reg_name[32] = {0};
    if (sceRegMgrGetStr(key_name, reg_name, sizeof(reg_name)) < 0) continue;
    if (!reg_name[0]) continue;
    if (strcmp(username, reg_name) == 0) {
      int key_aid = (i - 1) * 65536 + 125830400;
      uint64_t aid = 0;
      sceRegMgrGetBin(key_aid, &aid, sizeof(aid));
      return aid;
    }
  }
  return 0;
#endif
}

#ifndef __linux__
/* UserService 一次性初始化。放在这里而不是某个路由里，是因为
 * savemgr_foreground_uid() 在本文件里有 9 个调用点，逐个去补必然漏。
 * pthread_once 保证只跑一次；Initialize 本身幂等。 */
static pthread_once_t g_usersvc_once = PTHREAD_ONCE_INIT;
static void
sm_usersvc_init_once(void) {
  sceUserServiceInitialize(NULL);
}
#endif

int savemgr_foreground_uid(void) {
#ifdef __linux__
  return 0;
#else
  int uid = 0;
  /* ★ 2026-10-12：必须先 Initialize。
   * 真机台账实证：`import_archive_finish: uid=1ea2f4d9 fuid=0 aid=... resolved`
   * —— 真实用户是 1ea2f4d9，fuid 却解析成 0。原因就是本进程从未初始化过
   * UserService：未初始化时 GetForegroundUser **不报错**，只是把 *user_id
   * 留在 0。这个 0 随后被 savemgr_resign_sfo() 写进存档 SFO 的 USER_ID，
   * 等于把存档签给一个不存在的用户（Garlic 也记过 PS5 必须同时写 AID 与 UID）。
   * 口径与调用点取自 n0llptr/Playstation-5-Save-Mounter 的 mounter_payload
   * （GPLv3，其 main() 启动时显式调用 sceUserServiceInitialize(NULL)）。 */
  pthread_once(&g_usersvc_once, sm_usersvc_init_once);
  sceUserServiceGetForegroundUser(&uid);
  return uid;
#endif
}

/* ============================================================ db ======= */

int savemgr_register_db(const char *uid_hex, int is_ps4,
                        const sfo_info_t *info) {
  char raw[MAX_PATH_LEN], db_path[MAX_PATH_LEN], db_dir[MAX_PATH_LEN];
  snprintf(raw, sizeof(raw), "/system_data/%s/%s/db/user/savedata.db",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  sm_map_path(raw, db_path, sizeof(db_path));
  snprintf(raw, sizeof(raw), "/system_data/%s/%s/db/user",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  sm_map_path(raw, db_dir, sizeof(db_dir));
  recursive_mkdir(db_dir);

  sqlite3 *db = NULL;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;
  /* ★ 2026-10-12 真机：finish 卡在 finalize 这一整段且**连 finalize rc 都没
   * 打出来**。/system_data 下这个库是**系统存档服务自己也在用**的那一个，
   * 而我们从不设置 busy 上限 —— 默认行为下任何锁竞争都不可控。显式给 5 秒
   * 上限：宁可拿到 SQLITE_BUSY 失败并如实报错，也不要无限期等下去（等待期间
   * 请求线程还持着 g_savemgr_lock，整条 /api/save_mgr/ 都得跟着陪葬）。 */
  sqlite3_busy_timeout(db, 5000);
  char *err = NULL;
  sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS savedata ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
    "title_id NOT NULL, dir_name NOT NULL, main_title NOT NULL,"
    "sub_title, detail, tmp_dir_name, is_broken, user_param,"
    "blocks, free_blocks, size_kib, mtime NOT NULL,"
    "fake_broken, account_id, user_id, faked_owner,"
    "cloud_icon_url, cloud_revision, game_title_id NOT NULL, system_blocks)",
    NULL, 0, &err);
  if (err) { sqlite3_free(err); err = NULL; }

  char esc[512];
  int j = 0;
  const char *s = info->main_title;
  while (*s && j < (int)sizeof(esc) - 2) { if (*s == '\'') esc[j++] = '\''; esc[j++] = *s++; }
  esc[j] = 0;

  char mtime[64];
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now);
  strftime(mtime, sizeof(mtime), "%Y-%m-%dT%H:%M:%S.00Z", tm);

  uint64_t aid = info->account_id;
  char check[512];
  snprintf(check, sizeof(check),
           "SELECT COUNT(*) FROM savedata WHERE title_id='%s' AND dir_name='%s'",
           info->title_id, info->dir_name);
  int exists = 0;
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, check, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) exists = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  char sql[2048];
  if (exists) {
    snprintf(sql, sizeof(sql),
      "UPDATE savedata SET main_title='%s', mtime='%s', account_id=%llu, user_id=%u "
      "WHERE title_id='%s' AND dir_name='%s'",
      esc, mtime, (unsigned long long)aid, info->user_id,
      info->title_id, info->dir_name);
  } else {
    snprintf(sql, sizeof(sql),
      "INSERT INTO savedata (title_id, dir_name, main_title, sub_title, detail,"
      "is_broken, user_param, blocks, free_blocks, size_kib, mtime,"
      "fake_broken, account_id, user_id, faked_owner, cloud_revision,"
      "game_title_id, system_blocks) VALUES ("
      "'%s','%s','%s','','',0,%u,%u,%u,%u,'%s',0,%llu,%u,0,0,'%s',0)",
      info->title_id, info->dir_name, esc,
      info->user_param, info->blocks, info->blocks, info->blocks * 0x40, mtime,
      (unsigned long long)aid, info->user_id, info->title_id);
  }
  int rc = sqlite3_exec(db, sql, NULL, 0, &err);
  if (err) sqlite3_free(err);
  sqlite3_close(db);
  return rc == SQLITE_OK ? 0 : -1;
}

/* ---- 删除单份存档 ------------------------------------------------------- */

/* 名字必须是「一个路径分量」：不能带分隔符、不能是 . 或 ..、只能是可见的
 * ASCII。title_id / dir_name 都会进路径拼接，这层校验是唯一的入口 ——
 * 少了它，`dir_name=../../..` 就能把 unlink 指到镜像目录之外。 */
static int sm_safe_name(const char *s) {
  if (!s || !*s) return 0;
  for (const char *p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x21 || c > 0x7e) return 0;   /* 控制字符与非 ASCII */
    if (c == '/' || c == '\\') return 0;  /* 路径分隔 */
    if (c == '\'' || c == '"') return 0;  /* SQL 字符串定界 */
  }
  if (!strcmp(s, ".") || !strcmp(s, "..")) return 0;
  return 1;
}

/* 从目录项名推导存档目录名：`sdimg_<dir>` -> `<dir>`，没有前缀就原样返回。
 * ★ 为什么独立成函数、且**刻意不放在设备分支里**：扫描页（api_save_mgr_list）
 *   只会看到文件名，而镜像是加密 PFS 容器、不挂载读不到 param.sfo，所以目录
 *   名只能从文件名推。旧实现在这里拿镜像去读 SFO，读不到就把整个文件名
 *   （`sdimg_xxx`）当 dir_name 返回，备份再拼一次前缀变成 sdimg_sdimg_xxx
 *   ⇒ 真机 v1.0.24 的备份全废。放在公共区是为了让宿主单测能直接驱动它
 *   （.build/savemgr-dirname-unit-wsl.sh）—— 列表页这条路径属于
 *   `#ifndef __linux__`，宿主端到端根本跑不到。 */
void sm_dir_name_from_entry(const char *entry, char *out, size_t outsz) {
  if (!out || outsz == 0) return;
  out[0] = 0;
  if (!entry) return;
  const char *p = entry;
  if (strncmp(p, "sdimg_", 6) == 0) p += 6;
  snprintf(out, outsz, "%s", p);
}

/* app.db 里 title→name 的候选表/列。不同固件、不同用户数量下表名不同
 *（如 tbl_contentinfo、tbl_appbrowse_?_appinfo 等），硬编码会漏掉游戏名。
 * 这里按「有 titleId 列 + 有显示名列」动态发现，参考 ps5upload 的 content_db
 * 实现，但只取第一个有结果的候选表。 */
typedef struct {
  char table[64];
  char col_title[64];
  char col_name[64];
  int score;
  int like_match;   /* 1 = 标题列是 contentId 这类「包含」语义，要 LIKE '%tid%' */
} sm_app_schema_t;

#define SM_APP_SCHEMA_MAX 12

static int sm_cdb_has_column(sqlite3 *db, const char *table, const char *want,
                             char *out, size_t outcap) {
  char sql[192];
  snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
  int found = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *col = sqlite3_column_text(st, 1);
    if (col && strcasecmp((const char *)col, want) == 0) {
      snprintf(out, outcap, "%s", (const char *)col);
      found = 1; break;
    }
  }
  sqlite3_finalize(st);
  return found;
}

static int sm_cdb_discover_app_schemas(sqlite3 *db, sm_app_schema_t *out,
                                       int max) {
  /* 名称列：不同固件/地区叫法不一样，全列一遍，谁先命中用谁。 */
  static const char *name_cols[] = {"appName", "titleName", "name",
                                    "title_name", "canonicalTitle", "title",
                                    "app_name", "contentName"};
  /* 标题标识列：前两个是"精确等于 title_id"，后两个是"contentId 里含 title_id"
     （PS5 的 contentId 形如 XX0000-PPSA23226_00-...，只能 LIKE）。 */
  static const char *title_cols[] = {"titleId", "title_id", "contentId",
                                     "content_id"};
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'",
                         -1, &st, NULL) != SQLITE_OK) return 0;
  int n = 0;
  while (n < max && sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *tbl = sqlite3_column_text(st, 0);
    if (!tbl) continue;
    sm_app_schema_t c; memset(&c, 0, sizeof(c));
    snprintf(c.table, sizeof(c.table), "%s", (const char *)tbl);
    if (strncmp(c.table, "sqlite_", 7) == 0) continue;
    int ti = -1;
    for (size_t k = 0; k < sizeof(title_cols)/sizeof(*title_cols); k++) {
      if (sm_cdb_has_column(db, c.table, title_cols[k],
                            c.col_title, sizeof(c.col_title))) { ti = (int)k; break; }
    }
    if (ti < 0) continue;                 /* 连标题号都没有 ⇒ 这张表查不出游戏名 */
    c.like_match = (ti >= 2) ? 1 : 0;
    c.score = (ti < 2) ? 3 : 1;
    for (size_t i = 0; i < sizeof(name_cols)/sizeof(*name_cols); i++) {
      if (sm_cdb_has_column(db, c.table, name_cols[i],
                            c.col_name, sizeof(c.col_name))) {
        c.score += 2; break;
      }
    }
    out[n++] = c;
  }
  sqlite3_finalize(st);
  /* 按 score 降序排：有名字列的优先。 */
  for (int i = 1; i < n; i++) {
    sm_app_schema_t key = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].score < key.score) { out[j+1] = out[j]; j--; }
    out[j+1] = key;
  }
  return n;
}

/* 只读开 app.db。
 *
 * ★ 打开顺序是**真机反证**排出来的（v1.0.12），不是拍脑袋：
 *     1) immutable=1 —— 明确告诉 SQLite「这个文件不会变」：不加锁、不建
 *        journal、不碰 -wal/-shm。查系统库（系统服务自己也在写它）最稳。
 *     2) 只读打开 —— 常规路径。
 *     3) 读写打开 + `PRAGMA query_only=1` —— 真机上 sm_db_savedata_info 就是
 *        用 sqlite3_open（读写）开 /system_data 下的 savedata.db 且实测能读，
 *        所以这条路在真机一定走得通。放最后，并用 query_only 堵死任何写。
 *
 * 🪤 旧实现的第一句是 `access(path, R_OK) != 0 ⇒ return SQLITE_CANTOPEN`。
 *    宿主上拿真机 app.db 跑同一段代码，三个 title_id 全部命中（黑神话：悟空 /
 *    小小梦魇3 / YouTube），真机上却 game_title 恒空 —— 差别只剩这一行：payload
 *    的 libc 里 access() 不可靠。它一票否决，后面两条路永远走不到。
 *    调用方（sm_db_game_title）进来前已经 stat 过，这里再问一遍 access 没有
 *    任何额外信息量，只会多一个失败点 ⇒ 直接删。
 * ★ why：最后一种方式的失败原因写回这里，交给调用方记台账。真机没有调试器，
 *    静默 return 的分支等于永远查不出来。 */
static int sm_cdb_open_ro(const char *path, sqlite3 **out,
                          char *why, size_t whysz) {
  if (out) *out = NULL;
  if (why && whysz) why[0] = 0;
  sqlite3 *db = NULL;

  /* 1) mode=ro&nolock=1 —— ★ 口径来自 ps4mount（gasb07/ps4mount, GPLv3+）。
   *    它对该写法给出的理由正是我们踩的那个坑的原话：哪怕只持锁一瞬，也会让
   *    ShellCore 的写失败，而 ShellCore 随后会把 app.db **标记为损坏**。
   *    🪤 比 immutable=1 更贴切的地方：immutable 不仅免锁，还向 SQLite 断言
   *    「这个文件永不改变」—— 于是 SQLite **完全无视 -wal**，WAL 模式的库会
   *    读到写回主库之前的旧页（甚至读到不一致的页）。nolock=1 只免锁，WAL
   *    照读。我们要的只是「不打扰系统服务」，不是「假装它不会变」。 */
  {
    char uri[640];
    int n = snprintf(uri, sizeof(uri), "file:%s?mode=ro&nolock=1", path);
    if (n > 0 && (size_t)n < sizeof(uri)) {
      int rc = sqlite3_open_v2(uri, &db,
                               SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
      if (rc == SQLITE_OK && db) {
        sqlite3_stmt *probe = NULL;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master LIMIT 1", -1,
                               &probe, NULL) == SQLITE_OK) {
          int step = sqlite3_step(probe);
          sqlite3_finalize(probe);
          if (step == SQLITE_ROW || step == SQLITE_DONE) { *out = db; return SQLITE_OK; }
          if (why && whysz) snprintf(why, whysz, "nolock probe rc=%d", step);
        } else if (why && whysz) {
          snprintf(why, whysz, "nolock prepare: %s", sqlite3_errmsg(db));
        }
      } else if (why && whysz) {
        snprintf(why, whysz, "nolock open rc=%d%s%s", rc,
                 db ? ": " : "", db ? sqlite3_errmsg(db) : "");
      }
      if (db) { sqlite3_close(db); db = NULL; }
    }
  }

  /* 2) 只读 */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && db) {
    sqlite3_busy_timeout(db, 2000);
    sqlite3_stmt *probe = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master LIMIT 1", -1,
                           &probe, NULL) == SQLITE_OK) {
      int step = sqlite3_step(probe);
      sqlite3_finalize(probe);
      if (step == SQLITE_ROW || step == SQLITE_DONE) { *out = db; return SQLITE_OK; }
      if (why && whysz) snprintf(why, whysz, "readonly probe rc=%d", step);
    } else if (why && whysz) {
      snprintf(why, whysz, "readonly prepare: %s", sqlite3_errmsg(db));
    }
    sqlite3_close(db); db = NULL;
  } else if (why && whysz) {
    snprintf(why, whysz, "readonly open%s%s", db ? ": " : "",
             db ? sqlite3_errmsg(db) : "");
    if (db) { sqlite3_close(db); db = NULL; }
  }

  /* 3) 读写 + query_only：真机上被证明能开系统库的那条路，兜底用。
   *    query_only 让任何 INSERT/UPDATE/journal 尝试直接报错，绝不写系统库。 */
  if (sqlite3_open(path, &db) == SQLITE_OK && db) {
    sqlite3_busy_timeout(db, 2000);
    (void)sqlite3_exec(db, "PRAGMA query_only=1", NULL, NULL, NULL);
    sqlite3_stmt *probe = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master LIMIT 1", -1,
                           &probe, NULL) == SQLITE_OK) {
      int step = sqlite3_step(probe);
      sqlite3_finalize(probe);
      if (step == SQLITE_ROW || step == SQLITE_DONE) { *out = db; return SQLITE_OK; }
      if (why && whysz) snprintf(why, whysz, "query_only probe rc=%d", step);
    } else if (why && whysz) {
      snprintf(why, whysz, "query_only prepare: %s", sqlite3_errmsg(db));
    }
    sqlite3_close(db);
  } else if (why && whysz) {
    snprintf(why, whysz, "query_only open%s%s", db ? ": " : "",
             db ? sqlite3_errmsg(db) : "");
    if (db) sqlite3_close(db);
  }
  return SQLITE_CANTOPEN;
}

/* 系统应用库里的**真游戏名**。
 * ★ 为什么非查这里不可：savedata.db 的 main_title 是 param.sfo 的 TITLE 字段，
 *   那是「这份存档自己的名字」—— 意区机器上是 "Dati salvati"、日区是「セーブ
 *   データ」，跟游戏叫什么毫无关系。用户看到的 "Dati salvati · PPSA23226"
 *   就是这么来的：不是识别错，是取错了列。真游戏名只存在于
 *   /system_data/priv/mms/app.db。
 * ★ 只读打开：这是系统自己也在用的库，写模式打开会在系统服务持锁时 BUSY。
 * ★ 动态 schema 发现：不同固件表名/列名不同，硬编码 tbl_contentinfo 会漏。
 * 库不存在 / 表结构对不上 / 没有这一行 ⇒ 一律留空，调用方回落显示 title_id
 * —— 查名字这种事绝不能挡住备份/还原的主路径。
 * 刻意放在公共区（不在 #ifndef __linux__ 内），好让宿主单测直接驱动它。 */
static void sm_db_game_title_ex(const char *tid, char *out, size_t outsz,
                                int *row); /* 定义在下方：games 的 installed
                               判据要它回传「app.db 有没有这一行」。 */
void sm_db_game_title(const char *tid, char *out, size_t outsz) {
  sm_db_game_title_ex(tid, out, outsz, NULL);
}

/* ex 版：row 非空时回传「app.db 里有没有这一行」—— 名字列可能为空但行在，
 * installed 判据（games 列表的「已卸载」标记）需要这个信号而不是名字。
 * 名字与「行存在」是两个独立信号：行在名字空 ⇒ 游戏装着，只是库没写名字。 */
static void sm_db_game_title_ex(const char *tid, char *out, size_t outsz,
                                int *row) {
  if (row) *row = 0;
  if (!out || outsz == 0) return;
  out[0] = 0;
  if (!tid || !tid[0] || !sm_safe_name(tid)) return;
  char raw[MAX_PATH_LEN], db_path[MAX_PATH_LEN];
  snprintf(raw, sizeof(raw), "/system_data/priv/mms/app.db");
  sm_map_path(raw, db_path, sizeof(db_path));
  /* 打开失败只记一次：一个游戏三四个槽位会进来三四次，逐条记会把台账刷满。 */
  static int gt_logged = 0;
  struct stat ds;
  if (stat(db_path, &ds) != 0) {
    if (!gt_logged) {
      gt_logged = 1;
      mango_log("sm_db_game_title: app.db not found: %s", db_path);
    }
    return;
  }
  sqlite3 *db = NULL;
  char why[256];
  if (sm_cdb_open_ro(db_path, &db, why, sizeof(why)) != SQLITE_OK || !db) {
    if (!gt_logged) {
      gt_logged = 1;
      mango_log("sm_db_game_title: cannot open %s (%s)", db_path, why);
    }
    return;
  }
  sm_app_schema_t cands[SM_APP_SCHEMA_MAX];
  int ncand = sm_cdb_discover_app_schemas(db, cands, SM_APP_SCHEMA_MAX);
  sqlite3_stmt *st = NULL;
  for (int i = 0; i < ncand && !out[0]; i++) {
    char sql[512], pat[96];
    const char *ncol = cands[i].col_name[0] ? cands[i].col_name : "''";
    if (cands[i].like_match)
      snprintf(sql, sizeof(sql),
               "SELECT \"%s\" FROM \"%s\" WHERE \"%s\" LIKE ? LIMIT 1",
               ncol, cands[i].table, cands[i].col_title);
    else
      snprintf(sql, sizeof(sql),
               "SELECT \"%s\" FROM \"%s\" WHERE LOWER(\"%s\")=LOWER(?) LIMIT 1",
               ncol, cands[i].table, cands[i].col_title);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) continue;
    if (cands[i].like_match) {
      snprintf(pat, sizeof(pat), "%%%s%%", tid);
      sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *s = sqlite3_column_text(st, 0);
      if (row) *row = 1;
      if (s && s[0]) snprintf(out, outsz, "%s", (const char *)s);
    }
    sqlite3_finalize(st); st = NULL;
  }
  sqlite3_close(db);
}

/* ---- 游戏名/封面本地缓存（/data/Mango/covers/） --------------------------
 * 为什么非有不可：名字（app.db）与封面（appmeta / homebrew / 存档 meta）都是
 * 「游戏装着才有」的货源 —— 游戏一卸载它们立刻消失，而存档残留会在列表里
 * 停留很久，用户看到的就是一排 PPSA 编号 + 灰占位图（2026-10-21 真机报障：
 * PPSA16608/26344/17599 三张卡全是裸 tid、封面 404）。把每次成功查到的
 * 名字/封面落盘，卸载后回放缓存，列表就始终是「看得懂的游戏」。
 * 布局：
 *   /data/Mango/covers/titles.json   {"<tid>":"<名字>",...}
 *   /data/Mango/covers/<tid>.png     封面字节缓存（icon 路由命中时顺手存）
 * 并发：games 与 icon 都在 dispatch_save_mgr 的进程锁内跑，读写无需另加锁。
 * 宿主：/data 与 /user/appmeta 经 WFM_DATA_ROOT / WFM_APPMETA_ROOT 映射，可测。 */
#define SM_META_CACHE_DIR  "/data/Mango/covers"
#define SM_TITLES_JSON_CAP 65536
/* 封面缓存写盘的拷贝块大小（堆分配，见 sm_icon_cache_store）。 */
#define SM_ICON_CP_BUF 16384
#define SM_TITLES_MAX      512

static void sm_meta_dir(char *out, size_t n) {
  sm_map_path(SM_META_CACHE_DIR, out, n);
}

/* 建缓存目录（两级：父 /data/Mango 由 main.c 建过，但宿主夹具没有 ——
 * mkdir 不是递归的，少一级就在宿主测试里静默全挂）。 */
static void sm_meta_mkdir(void) {
  char dir[MAX_PATH_LEN];
  sm_meta_dir(dir, sizeof(dir));
  char *slash = strrchr(dir, '/');
  if (slash && slash != dir) {
    *slash = 0;
    mkdir(dir, 0777);
    *slash = '/';
  }
  mkdir(dir, 0777);
}

/* sce_backup* 是 PS5 给 PS4 存档准备的系统备份容器（/user/home/<uid>/
 * savedata/sce_backup<N>）：里面没有游戏镜像可挂、没有名字/封面可查，挂在
 * 列表里只会成为一张「裸 tid + 占位图」的卡片 —— games 扫描直接跳过。
 * 刻意做成纯函数：真机分支宿主测不到，判断逻辑必须可单测。 */
int sm_is_system_container(const char *tid) {
  return tid && strncmp(tid, "sce_backup", 10) == 0;
}

/* ---- JSON 值读取的小工具 ------------------------------------------------ */
static int sm_hex4(const char *p, unsigned *cp) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *cp = v;
  return 1;
}
static size_t sm_cp_to_utf8(unsigned cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

/* 从 buf 读一个 JSON 字符串（p 指向开引号之后），解码基本转义与 \uXXXX，
 * 返回消费掉的字节数（指向闭引号之后），0 = 非法。 */
static size_t sm_json_read_str(const char *p, char *out, size_t outsz) {
  size_t v = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') {
        unsigned cp;
        if (!sm_hex4(p + 1, &cp)) return 0;
        p += 5;
        v += sm_cp_to_utf8(cp, out + v);   /* outsz 足够大，调用方保证 */
        continue;
      }
      if (*p == 'n') { out[v++] = '\n'; p++; continue; }
      if (*p == 't') { out[v++] = '\t'; p++; continue; }
      if (*p == 'r') { out[v++] = '\r'; p++; continue; }
    }
    out[v++] = *p++;
    if (v >= outsz - 1) return 0;          /* 名字 256 字节封顶，超长放弃 */
  }
  if (*p != '"') return 0;
  out[v] = 0;
  return v + 1;                            /* +1 = 闭引号也算消费 */
}

/* 解析 {"tid":"name",...}。返回条数；超出 max 截断。 */
static int sm_titles_parse(const char *buf, char (*tids)[64],
                           char (*names)[256], int max) {
  if (!buf || !tids || !names || max <= 0) return 0;
  const char *p = buf;
  int n = 0;
  while (*p && n < max) {
    if (*p != '"') { p++; continue; }
    p++;
    char key[64];
    size_t k = 0;
    while (*p && *p != '"' && k < sizeof(key) - 1) key[k++] = *p++;
    key[k] = 0;
    if (*p != '"') break;
    p++;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') continue;
    p++;
    char val[256];
    size_t used = sm_json_read_str(p, val, sizeof(val));
    if (!used) break;
    p += used;
    if (k && val[0] && n < max) {
      snprintf(tids[n], 64, "%s", key);
      snprintf(names[n], 256, "%s", val);
      n++;
    }
  }
  return n;
}

static long sm_titles_read(char *buf, size_t cap) {
  char dir[MAX_PATH_LEN], path[MAX_PATH_LEN];
  sm_meta_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/titles.json", dir);
  long n = sm_read_file(path, buf, cap);
  return (n > 0) ? n : 0;
}

/* 缓存里查名字：1 查到 / 0 没有 / -1 参数或 IO 问题。 */
int sm_title_cache_get(const char *tid, char *out, size_t outsz) {
  if (out && outsz) out[0] = 0;
  if (!tid || !tid[0] || !sm_safe_name(tid) || !out || outsz == 0) return -1;
  /* ⚠️ 必须堆分配：见 SM_LIST_JSON_CAP 上方那段注释 —— 请求跑在
   * MHD_USE_THREAD_PER_CONNECTION 的连接线程上，栈很浅。v1.0.26 把它写成
   * `char buf[SM_TITLES_JSON_CAP]`（64 KB 栈），而 put() 内部还会调 get()，
   * 一帧叠到 128 KB+ ⇒ 真机上一进存档页就爆栈、整个 daemon 失联
   * （宿主栈 8 MB 看不出来）。 */
  char *buf = malloc(SM_TITLES_JSON_CAP);
  if (!buf) return -1;
  if (sm_titles_read(buf, SM_TITLES_JSON_CAP) <= 0) { free(buf); return 0; }
  char (*tids)[64] = malloc(sizeof(*tids) * SM_TITLES_MAX);
  char (*names)[256] = malloc(sizeof(*names) * SM_TITLES_MAX);
  if (!tids || !names) { free(tids); free(names); free(buf); return -1; }
  int n = sm_titles_parse(buf, tids, names, SM_TITLES_MAX);
  int rc = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(tids[i], tid) == 0) {
      snprintf(out, outsz, "%s", names[i]);
      rc = out[0] ? 1 : 0;
      break;
    }
  }
  free(tids);
  free(names);
  free(buf);
  return rc;
}

/* 记名字（幂等：值相同不重写文件）。512 条封顶，满了保旧弃新 —— 一个用户
 * 累计装过的游戏数量级远到不了 512。 */
void sm_title_cache_put(const char *tid, const char *name) {
  if (!tid || !tid[0] || !sm_safe_name(tid) || !name || !name[0]) return;
  char dir[MAX_PATH_LEN], path[MAX_PATH_LEN], tmp[MAX_PATH_LEN];
  sm_meta_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/titles.json", dir);
  sm_meta_mkdir();
  char cur[256];
  if (sm_title_cache_get(tid, cur, sizeof(cur)) == 1 &&
      strcmp(cur, name) == 0) return;                  /* 无变化不写 */
  /* 同样必须堆分配（同 get()：请求线程栈很浅，且这里已经压了一帧 get()）。 */
  char *buf = calloc(SM_TITLES_JSON_CAP, 1);
  if (!buf) return;
  sm_titles_read(buf, SM_TITLES_JSON_CAP);
  char (*tids)[64] = calloc(SM_TITLES_MAX, sizeof(*tids));
  char (*names)[256] = calloc(SM_TITLES_MAX, sizeof(*names));
  if (!tids || !names) { free(tids); free(names); free(buf); return; }
  int n = sm_titles_parse(buf, tids, names, SM_TITLES_MAX);
  int hit = -1;
  for (int i = 0; i < n; i++)
    if (strcmp(tids[i], tid) == 0) { hit = i; break; }
  if (hit >= 0) snprintf(names[hit], 256, "%s", name);
  else if (n < SM_TITLES_MAX) {
    snprintf(tids[n], 64, "%s", tid);
    snprintf(names[n], 256, "%s", name);
    n++;
  } else { free(tids); free(names); free(buf); return; }
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) { free(tids); free(names); return; }
  fputc('{', f);
  for (int i = 0; i < n; i++) {
    char esc[600];
    sm_json_esc(names[i], esc, sizeof(esc));
    fprintf(f, "%s\"%s\":\"%s\"", i ? "," : "", tids[i], esc);
  }
  fputc('}', f);
  int wok = (fflush(f) == 0);
  fclose(f);
  if (wok) rename(tmp, path);
  else unlink(tmp);
  free(tids);
  free(names);
  free(buf);
}

/* ---- appmeta 兜底名 ------------------------------------------------------
 * PS5 的 param.json **不含 TITLE 字段** —— 名字全在 localizedParameters 的
 * 各语言 titleName 里（真机取证：PPSA23226 en-US="Black Myth: Wukong"、
 * ja-JP=黒神話：悟空，这台机器的包没有 zh-CN）。语言优先级 zh-CN → zh-TW →
 * defaultLanguage 指定的语言。app.db 的名字通常已本地化（黑神话：悟空），
 * 所以本函数只是 app.db 查不到时的兜底（fPKG 装了但 app.db 没写行）。
 * 1 查到 / 0 没查到。 */
static int sm_pick_title_name(const char *hay, const char *lang,
                              char *out, size_t outsz) {
  char sec[48];
  snprintf(sec, sizeof(sec), "\"%s\"", lang);
  const char *p = strstr(hay, sec);
  if (!p) return 0;
  p += strlen(sec);
  const char *tn = strstr(p, "\"titleName\"");
  /* titleName 必须落在**这一个**语言段里：语言键与其 titleName 相邻几十字节；
   * 超过 512 说明已经越过下一段了（语言段之间还有别的键）。 */
  if (!tn || (size_t)(tn - p) > 512) return 0;
  const char *c = strchr(tn + 11, ':');
  if (!c) return 0;
  c++;
  while (*c == ' ' || *c == '\r' || *c == '\n') c++;
  if (*c != '"') return 0;
  c++;
  size_t used = sm_json_read_str(c, out, outsz);
  return (used && out[0]) ? 1 : 0;
}

int sm_appmeta_title(const char *tid, char *out, size_t outsz) {
  if (out && outsz) out[0] = 0;
  if (!tid || !tid[0] || !sm_safe_name(tid) || !out || outsz == 0) return 0;
  char vp[MAX_PATH_LEN], rp[MAX_PATH_LEN];
  snprintf(vp, sizeof(vp), "/user/appmeta/%s/param.json", tid);
  sm_map_path(vp, rp, sizeof(rp));
  char *j = malloc(SM_TITLES_JSON_CAP);
  if (!j) return 0;
  if (sm_read_file(rp, j, SM_TITLES_JSON_CAP) <= 0) { free(j); return 0; }
  const char *lp = strstr(j, "\"localizedParameters\"");
  int rc = 0;
  if (lp) {
    static const char *pref[] = {"zh-CN", "zh-TW"};
    for (size_t i = 0; i < sizeof(pref) / sizeof(pref[0]) && !rc; i++)
      rc = sm_pick_title_name(lp, pref[i], out, outsz);
    if (!rc) {
      char dl[24];
      sm_json_str(j, "defaultLanguage", dl, sizeof(dl));
      if (dl[0]) rc = sm_pick_title_name(lp, dl, out, outsz);
    }
  }
  free(j);
  return rc;
}

/* ---- 封面缓存（<tid>.png）---------------------------------------------- */
int sm_icon_cache_fetch(const char *tid, char *out, size_t n) {
  if (!tid || !tid[0] || !sm_safe_name(tid) || !out || n == 0) return 0;
  char dir[MAX_PATH_LEN], path[MAX_PATH_LEN];
  sm_meta_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/%s.png", dir, tid);
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return 0;
  snprintf(out, n, "%s", path);
  return 1;
}

/* 命中即缓存（原子：tmp + rename）。源就是缓存本身、或已缓存同大小文件时跳过
 * —— icon 路由每次命中 appmeta 都会进来，不能每次都真拷一遍。 */
void sm_icon_cache_store(const char *tid, const char *src) {
  if (!tid || !tid[0] || !sm_safe_name(tid) || !src || !src[0]) return;
  char dir[MAX_PATH_LEN], path[MAX_PATH_LEN], tmp[MAX_PATH_LEN];
  sm_meta_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/%s.png", dir, tid);
  if (strcmp(src, path) == 0) return;
  sm_meta_mkdir();
  struct stat ss;
  if (stat(src, &ss) != 0 || !S_ISREG(ss.st_mode) ||
      ss.st_size <= 0 || ss.st_size > (8 << 20)) return;
  struct stat sc;
  if (stat(path, &sc) == 0 && sc.st_size == ss.st_size) return;
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int in = open(src, O_RDONLY);
  if (in < 0) return;
  FILE *f = fopen(tmp, "wb");
  if (!f) { close(in); return; }
  /* 同上：拷贝缓冲不能放栈上（连接线程栈很浅），走堆。 */
  char *cpbuf = malloc(SM_ICON_CP_BUF);
  if (!cpbuf) { fclose(f); unlink(tmp); close(in); return; }
  ssize_t r;
  long long total = 0;
  while ((r = read(in, cpbuf, SM_ICON_CP_BUF)) > 0) {
    if ((long long)fwrite(cpbuf, 1, (size_t)r, f) != r) { total = -1; break; }
    total += r;
  }
  free(cpbuf);
  close(in);
  int bad = (total != (long long)ss.st_size);
  fclose(f);                              /* fclose 才落 stdio 缓冲 */
  if (bad) { unlink(tmp); return; }
  rename(tmp, path);
}

/* 游戏显示名的统一出口（games 扫描每个 tid 走一次）：
 *   1) app.db       —— 系统应用库，名字已按机器语言本地化（黑神话：悟空），最准
 *   2) appmeta      —— param.json 的 localizedParameters（fPKG 装了但 app.db
 *                      没写行时唯一的货源；名字可能是英文）
 *   3) titles.json  —— 本地缓存（游戏卸载后回放）
 * 查到任何来源都顺手写缓存（值相同则不重写文件）。installed 回传真信号：
 * app.db 有行 **或** appmeta 目录在（游戏本体装着）；两者都没了 = 已卸载。 */
void sm_game_display_name(const char *tid, char *out, size_t outsz,
                          int *installed) {
  if (installed) *installed = 1;
  if (!out || outsz == 0) return;
  out[0] = 0;
  if (!tid || !tid[0] || !sm_safe_name(tid)) return;
  int row = 0;
  sm_db_game_title_ex(tid, out, outsz, &row);
  char vp[MAX_PATH_LEN], rp[MAX_PATH_LEN];
  snprintf(vp, sizeof(vp), "/user/appmeta/%s", tid);
  sm_map_path(vp, rp, sizeof(rp));
  struct stat as;
  int have_meta = (stat(rp, &as) == 0 && S_ISDIR(as.st_mode));
  if (!out[0]) sm_appmeta_title(tid, out, outsz);
  if (!out[0]) sm_title_cache_get(tid, out, outsz);
  if (out[0]) sm_title_cache_put(tid, out);
  if (installed) *installed = (row || have_meta) ? 1 : 0;
}

/* 一个游戏最多认多少个槽位。真机上见过 4 个（主档 + sce_sdmemory + bu_*），
 * 给 64 是宽松上限；超了就截断，宁可少备几个也不越界写。 */
#define SM_GAME_SLOTS_MAX 64

/* 枚举一个游戏（title_id）在本机上的**全部存档槽位**。
 * 「一个游戏 = 一个整体」是用户定的操作粒度：备份/删除/还原都以游戏为单位。
 * 扫的是 savedata_prospero（PS5）与 savedata（PS4）两个目录里的 sdimg_*，
 * 返回去掉前缀后的 dir_name；.bin 是 PS4 镜像的附属密钥，不是槽位，跳过。
 * 返回槽位数（0 = 这个游戏在本机没有存档）；max 是 dirs 的容量，超出就截断
 * （宁可少备几个，也不越界写 —— 真机上越界就是静默死）。
 * 刻意放在公共区：宿主用 WFM_SAVE_ROOT 映射 /user/home 后可以直接驱动。 */
int sm_game_slots(const char *uid_hex, const char *tid, char dirs[][64],
                  int max, int *ps4_flags) {
  if (!uid_hex || !tid || !dirs || max <= 0) return 0;
  if (!sm_safe_name(uid_hex) || !sm_safe_name(tid)) return 0;
  int n = 0;
  const char *bases[] = {"savedata_prospero", "savedata"};
  for (int b = 0; b < 2 && n < max; b++) {
    char tp[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
    snprintf(tp, sizeof(tp), "/user/home/%s/%s/%s", uid_hex, bases[b], tid);
    sm_map_path(tp, mapped, sizeof(mapped));
    DIR *d = opendir(mapped);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
      if (e->d_name[0] == '.') continue;
      size_t l = strlen(e->d_name);
      if (l > 4 && strcmp(e->d_name + l - 4, ".bin") == 0) continue;
      char fp[MAX_PATH_LEN];
      snprintf(fp, sizeof(fp), "%s/%s", mapped, e->d_name);
      struct stat st;
      if (stat(fp, &st) < 0 || !S_ISREG(st.st_mode)) continue;
      sm_dir_name_from_entry(e->d_name, dirs[n], 64);
      if (!dirs[n][0]) continue;
      if (ps4_flags) ps4_flags[n] = (b == 1) ? 1 : 0;
      n++;
    }
    closedir(d);
  }
  return n;
}

/* 注销一条存档记录（DELETE FROM savedata）。
 * ★ 只删文件不注销 = 存档损坏：系统列表里那条还在，游戏按列表去找文件就
 *   找不到 ⇒ 表现就是「数据损坏」。所以删存档必须文件和记录一起抹。 */
int savemgr_unregister_db(const char *uid_hex, int is_ps4,
                          const char *title_id, const char *dir_name) {
  if (!sm_safe_name(title_id) || !sm_safe_name(dir_name)) return -1;
  char raw[MAX_PATH_LEN], db_path[MAX_PATH_LEN];
  snprintf(raw, sizeof(raw), "/system_data/%s/%s/db/user/savedata.db",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  sm_map_path(raw, db_path, sizeof(db_path));
  struct stat ds;
  if (stat(db_path, &ds) != 0) return 0;   /* 库都还没有 ⇒ 没有记录可注销 */
  sqlite3 *db = NULL;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;
  sqlite3_busy_timeout(db, 5000);
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(
      db, "DELETE FROM savedata WHERE title_id=? AND dir_name=?", -1, &st, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, dir_name, -1, SQLITE_TRANSIENT);
    rc = (sqlite3_step(st) == SQLITE_DONE) ? SQLITE_OK : -1;
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return rc == SQLITE_OK ? 0 : -1;
}

/* 删掉一份存档：镜像 +（PS4）封密钥 .bin + 元数据 icon + 系统库里那条记录。
 * ★ 全程不挂载 —— 挂载是为读写存档**内容**，删整个存档不需要打开它，也就
 *   不必付一次内核调用（一次 = 一次可能永不返回）的风险。
 * 0=已删；-1=参数非法；-2=没找到；-3=文件已删但库记录没注销；-4=删不掉。 */
int savemgr_delete_save(const char *uid_hex, const char *title_id,
                        const char *dir_name) {
  if (!uid_hex || !sm_safe_name(uid_hex)) return -1;
  if (!sm_safe_name(title_id) || !sm_safe_name(dir_name)) return -1;
  char cand[MAX_PATH_LEN], mapped[MAX_PATH_LEN], img[MAX_PATH_LEN];
  struct stat st;
  int is_ps4 = 0, found = 0;
  snprintf(cand, sizeof(cand), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
           uid_hex, title_id, dir_name);
  sm_map_path(cand, mapped, sizeof(mapped));
  if (stat(mapped, &st) == 0 && S_ISREG(st.st_mode)) {
    snprintf(img, sizeof(img), "%s", mapped);
    found = 1;
  } else {
    snprintf(cand, sizeof(cand), "/user/home/%s/savedata/%s/sdimg_%s",
             uid_hex, title_id, dir_name);
    sm_map_path(cand, mapped, sizeof(mapped));
    if (stat(mapped, &st) == 0 && S_ISREG(st.st_mode)) {
      snprintf(img, sizeof(img), "%s", mapped);
      found = 1;
      is_ps4 = 1;
    }
  }
  if (!found) return -2;
  if (unlink(img) != 0) return -4;
  /* PS4 存档的封密钥：与镜像同目录，名字就是 dir_name 去掉 sdimg_ 前缀 */
  if (is_ps4) {
    char bin[MAX_PATH_LEN];
    snprintf(cand, sizeof(cand), "/user/home/%s/savedata/%s/%s.bin",
             uid_hex, title_id, dir_name);
    sm_map_path(cand, bin, sizeof(bin));
    if (stat(bin, &st) == 0) unlink(bin);
  }
  /* 元数据 icon（只有 PS5 有 savedata_prospero_meta） */
  char ico[MAX_PATH_LEN];
  snprintf(cand, sizeof(cand),
           "/user/home/%s/savedata_prospero_meta/user/%s/%s_icon0.png",
           uid_hex, title_id, dir_name);
  sm_map_path(cand, ico, sizeof(ico));
  if (stat(ico, &st) == 0) unlink(ico);
  return savemgr_unregister_db(uid_hex, is_ps4, title_id, dir_name) == 0 ? 0 : -3;
}

/* 按游戏整体删除（「一个游戏 = 一个整体」是用户定的操作粒度）。
 * 逐个槽位复用 savemgr_delete_save —— 不另写一份删除逻辑，免得两处口径分家
 * （删镜像/删 .bin/删 icon/注销 db 行这套动作已经在单槽位里被单测钉住）。
 * 某个槽位失败**不中断**：一个游戏里有一个槽位删不掉，不代表剩下的也不该删。
 * 返回成功删掉的槽位数；failed（可选）累计失败数。 */
int savemgr_delete_game(const char *uid_hex, const char *title_id,
                        int *failed) {
  if (failed) *failed = 0;
  if (!uid_hex || !sm_safe_name(uid_hex)) return -1;
  if (!sm_safe_name(title_id)) return -1;
  char dirs[SM_GAME_SLOTS_MAX][64];
  int ps4[SM_GAME_SLOTS_MAX];
  int n = sm_game_slots(uid_hex, title_id, dirs, SM_GAME_SLOTS_MAX, ps4);
  int ok = 0;
  for (int i = 0; i < n; i++) {
    if (savemgr_delete_save(uid_hex, title_id, dirs[i]) == 0) ok++;
    else if (failed) (*failed)++;
  }
  return ok;
}

/* ============================================================ device === */

#ifndef __linux__

static int g_mounted = 0;
static char g_mount_point[512];
static char g_mounted_img[512];
static char g_local_copy[512];
static int g_is_ps4 = 0;
static int g_mounted_idx = -1;


/* ⚠️ ioctl 0xc0845302 声明的载荷是 2117 字节（(cmd >> 16) & 0x1fff == 0x845），
 * 而我们真正用到的只有开头 0x60 字节。参考的存档管理器故意把这块缓冲放到堆上，
 * 它自己的注释就写着「ioctl 可以越过边界破坏栈」。多写约 1.8KB：在堆上只是踩到
 * 相邻 malloc 块（能活），在栈上是踩返回地址（进程静默死亡 —— 真机「点一下人就没
 * 了」正是这个画像）。这一版移植时把它放回了栈上，现在改回堆，并按 ioctl 声明的
 * 尺寸给 0x1000，两种口径都装得下。 */
static int decrypt_key(const char *mount_src, int is_ps4, uint8_t *out_key) {
  uint8_t *data;
  int rc = 0;

  data = calloc(1, 0x1000);
  if (!data) return -6;

  if (is_ps4) {
    const char *bname = strrchr(mount_src, '/');
    bname = bname ? bname + 1 : mount_src;
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", mount_src);
    char *sl = strrchr(dir, '/');
    if (sl) *(sl + 1) = 0; else dir[0] = 0;
    const char *savename = (strncmp(bname, "sdimg_", 6) == 0) ? bname + 6 : bname;
    char binp[MAX_PATH_LEN];
    snprintf(binp, sizeof(binp), "%s%s.bin", dir, savename);
    int fd = open(binp, O_RDONLY);
    if (fd < 0) { rc = -4; goto out; }
    if (read(fd, data, 0x60) != 0x60) { close(fd); rc = -5; goto out; }
    close(fd);
  } else {
    int fd = open(mount_src, O_RDONLY);
    if (fd < 0) { rc = -4; goto out; }
    if (pread(fd, data, 0x60, 0x800) != 0x60) { close(fd); rc = -5; goto out; }
    close(fd);
  }
  {
    int pfsmgr = open("/dev/pfsmgr", O_RDWR);
    int ret;
    if (pfsmgr < 0) { rc = -6; goto out; }
    ret = ioctl(pfsmgr, 0xc0845302, data);
    close(pfsmgr);
    if (ret < 0) { rc = -7; goto out; }
  }
  memcpy(out_key, data + 0x60, 0x20);

out:
  free(data);
  return rc;
}

static int sm_mount_image_inner(const char *img_path, char *mount_point,
                                size_t mp_len) {
  if (g_mounted) return -1;
  int is_ps4 = 0;
  int hfd = open(img_path, O_RDONLY);
  if (hfd >= 0) { uint8_t h; if (pread(hfd, &h, 1, 0) == 1 && h == 0x01) is_ps4 = 1; close(hfd); }

  (void)save_domain_escalate_full();
  g_is_ps4 = is_ps4;

  const char *mount_src = img_path;
  g_local_copy[0] = 0;
  if (strncmp(img_path, "/data/", 6) != 0) {
    /* ★ 这条分支是「浏览页挂载」独有的，批量导入**不走这里**（导入的暂存镜像
     * 已经在 /data/Mango/tmp/_nex_arc 下）。真机 2026-10-12 反馈「导入没问题，
     * 但浏览页点挂载就崩」，两者的全部差异就集中在下面这几行 —— 所以每一步
     * 都必须落台账，否则下次还是只能靠猜。 */
    snprintf(g_local_copy, sizeof(g_local_copy), "/data/Mango/tmp/%s",
             strrchr(img_path, '/') ? strrchr(img_path, '/') + 1 : "sdimg_import");
    mango_log("sm_mount_image_inner: staging copy %s -> %s (ps4=%d)",
              img_path, g_local_copy, is_ps4);
    recursive_mkdir("/data/Mango/tmp");
    if (copy_file(img_path, g_local_copy) < 0) {
      mango_log("sm_mount_image_inner: staging copy FAILED %s", img_path);
      return -3;
    }
    chmod(g_local_copy, 0755);
    mount_src = g_local_copy;
    if (is_ps4) {
      const char *bname = strrchr(img_path, '/') ? strrchr(img_path, '/') + 1 : img_path;
      const char *sv = (strncmp(bname, "sdimg_", 6) == 0) ? bname + 6 : bname;
      char srcdir[MAX_PATH_LEN]; snprintf(srcdir, sizeof(srcdir), "%s", img_path);
      char *sl = strrchr(srcdir, '/'); if (sl) *(sl + 1) = 0;
      char sb[MAX_PATH_LEN], db[MAX_PATH_LEN];
      snprintf(sb, sizeof(sb), "%s%s.bin", srcdir, sv);
      snprintf(db, sizeof(db), "/data/Mango/tmp/%s.bin", sv);
      copy_file(sb, db); chmod(db, 0755);
    }
  }

  uint8_t key[0x20];
  memset(key, 0, sizeof(key));
  int kr = decrypt_key(mount_src, is_ps4, key);
  if (kr < 0) {
    mango_log("sm_mount_image_inner: decrypt_key rc=%d src=%s", kr, mount_src);
    return kr;
  }

  snprintf(g_mount_point, sizeof(g_mount_point), "/data/mount_sd");
  mkdir(g_mount_point, 0777);
  if (mount_point) snprintf(mount_point, mp_len, "%s", g_mount_point);

  MountOpt mopt;
  memset(&mopt, 0, sizeof(mopt));
  sceFsInitMountSaveDataOpt(&mopt);
  /* budgetid is a POINTER in the real layout (it was a char[32] stand-in
   * before); "system" is a literal, so the assignment stays valid forever. */
  mopt.budgetid = (char *)"system";

  /* ⚠️ 这里**绝不**把 SIGPIPE 恢复成 SIG_DFL。
   *
   * 这两行是从 Garlic 照搬的（main.c:1664 等 5 处都是同一个包裹）。在它那里
   * 成立：Garlic 是**单线程** GUI，主线程就阻塞在这一行，期间没有任何别的
   * 线程在写 socket。
   *
   * 我们这里是 MHD_USE_THREAD_PER_CONNECTION —— 挂载跑在 sm_kern_run() 起的
   * 看护线程里，**其它请求线程照常在写 HTTP 响应**。只要有一条连接在这段
   * 时间里被对端关掉（前端轮询超时、用户刷新页面、PS5 浏览器切走标签），
   * write() 返回 EPIPE 并给进程投递 SIGPIPE；而此刻 disposition 恰好是
   * SIG_DFL ⇒ **整个进程被 SIGPIPE 当场杀死**。
   *
   * 更糟的是它不留痕迹：崩溃处理器只挂了 SIGSEGV/SIGBUS/SIGABRT/SIGILL/
   * SIGFPE（main.c:153-155），SIGPIPE 不在其中 ⇒ 台账里**没有崩溃段**。真机
   * 画像就是「点一下人就没了，日志什么都没多出来」，只能靠猜。
   *
   * SIG_IGN 下 write() 照样返回 EPIPE（该报错的调用方还是会报错），唯一的
   * 区别是进程不被杀。所以保持 main.c 设好的全局 SIG_IGN，这里什么都不做。 */
  int ret = sceFsMountSaveData(&mopt, mount_src, g_mount_point, key);
  mango_log("sm_mount_image_inner: sceFsMountSaveData src=%s rc=%d", mount_src, ret);
  if (ret < 0) return ret;

  snprintf(g_mounted_img, sizeof(g_mounted_img), "%s", img_path);
  g_mounted = 1;
  g_mounted_idx = -1;
  return 0;
}

static int sm_unmount_inner(void) {
  int r;

  if (!g_mounted) return -1;
  UmountOpt uopt;
  memset(&uopt, 0, sizeof(uopt));
  sceFsInitUmountSaveDataOpt(&uopt);
  r = sceFsUmountSaveData(&uopt, g_mount_point, 0, 0);
  if (r < 0) {
    /* 内核说没卸掉 ⇒ **保持挂载状态原样**，把错误交给上层定夺：
       savemgr_mount_image() 会据此回 -104，拒绝叠加第二次挂载。
       🪤 旧写法丢掉这个返回值、恒返回 0，于是上层那道
       `if (savemgr_unmount() != 0) return -104;` 的门**永远进不去** ——
       卸载失败被当成成功，第二次 mount 照样进内核，两个挂载叠在 PFS 上
       ⇒ 真机「点替换覆盖后整机假死、SIGKILL 都带不走」。
       宿主测试此前一条都覆盖不到这段代码（它在 #ifndef __linux__ 里），
       是 .build/savemgr-mount-unit-wsl.sh 第一次跑真机分支才抓到的。 */
    return r;
  }
  sync();
  if (g_local_copy[0] && g_mounted_img[0]) {
    copy_file(g_local_copy, g_mounted_img);
    unlink(g_local_copy);
  }
  g_mounted = 0;
  g_mounted_idx = -1;
  return 0;
}

/* 无条件对固定挂载点做一次 umount —— 只给启动期「清上次留下的残留挂载」用。
   正常路径（sm_unmount_inner）会先看 g_mounted，而残留挂载恰恰是本进程
   根本没挂过的，所以必须绕过那个判断。 */
static int
sm_force_unmount_inner(void) {
  UmountOpt uopt;
  memset(&uopt, 0, sizeof(uopt));
  sceFsInitUmountSaveDataOpt(&uopt);
  int r = sceFsUmountSaveData(&uopt, "/data/mount_sd", 0, 0);
  sync();
  g_mounted = 0;
  g_mounted_idx = -1;
  return r;
}

/* ==================== 挂载看护：内核调用可能永不返回 ====================
 * 真机画像（2026-10-10 反馈）：批量导入逐个挂载时，某一次
 * sceFsMountSaveData 卡在内核里不返回 —— 线程进入**不可中断睡眠**，连
 * SIGKILL 都杀不掉（「结束进程他还会自启」），而它正持着 g_savemgr_lock，
 * 于是 /api/save_mgr/ 整条永久全黑，自启多少次都一样废。
 *
 * 对策：真正的 mount/umount 放到一条独立线程里跑，父线程只等
 * SM_MOUNT_TIMEOUT_MS。超时就立刻返回错误、放锁 —— 服务活着，用户还能用
 * 别的功能、还能关掉插件。**在那条线程自己回来之前，绝不再往内核塞第二次
 * 挂载**：第二次并发 mount 往往就是整机假死的真正起点。
 *
 * 返回值约定（只在设备构建里出现）：
 *   -100 内核调用超时（已放弃该线程）
 *   -101 上一次内核调用还没回来，本次直接拒绝
 *   -103 看护线程起不来                                                        */
#define SM_MOUNT_TIMEOUT_MS 60000

typedef struct {
  int  op;                     /* 0 = mount, 1 = umount */
  char img[MAX_PATH_LEN];
  char mp[512];                /* 让 inner 写进**这里**：超时后调用方的栈就没
                                  了，绝不能让内核回来时往调用方栈上写 */
  int  rc;
  volatile int done;
} sm_kern_job_t;

static sm_kern_job_t    g_kern_job;     /* g_kern_busy 保证同时只有一个使用者 */
static pthread_mutex_t  g_kern_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_kern_busy;
static unsigned         g_kern_stuck;   /* 累计超时次数，进台账 */

static void *
sm_kern_worker(void *arg) {
  sm_kern_job_t *j = (sm_kern_job_t *)arg;

  if (j->op == 0)      j->rc = sm_mount_image_inner(j->img, j->mp, sizeof(j->mp));
  else if (j->op == 2) j->rc = sm_force_unmount_inner();
  else                 j->rc = sm_unmount_inner();
  j->done = 1;
  pthread_mutex_lock(&g_kern_lock);
  g_kern_busy = 0;
  pthread_mutex_unlock(&g_kern_lock);
  return NULL;
}

/* 0 = 线程已回来（*out_rc 有效）；-1 = 超时（线程被放弃）；-2 = 上一次还没回来；
 * -3 = 线程起不来。 */
static int
sm_kern_run(int op, const char *img, char *out_mp, size_t out_mp_len, int *out_rc) {
  pthread_t th;
  int waited;

  pthread_mutex_lock(&g_kern_lock);
  if (g_kern_busy) { pthread_mutex_unlock(&g_kern_lock); return -2; }
  memset(&g_kern_job, 0, sizeof(g_kern_job));
  g_kern_job.op = op;
  if (img) snprintf(g_kern_job.img, sizeof(g_kern_job.img), "%s", img);
  g_kern_busy = 1;
  pthread_mutex_unlock(&g_kern_lock);

  /* ★ 显式给这条线程 1MiB 栈：它要跑 sm_mount_image_inner()（里面有多条
   * MAX_PATH_LEN 路径缓冲 + 一次几十 MB 的暂存拷贝），而 pthread_create 的
   * 默认栈在本环境里并没有保证到「路由线程」那个量级。栈不够的表现是
   * SIGSEGV，而且崩在看似无害的函数里 —— 不显式指定就只能靠运气。 */
  pthread_attr_t attr;
  int have_attr = (pthread_attr_init(&attr) == 0);
  if (have_attr) {
    if (pthread_attr_setstacksize(&attr, 1u << 20) != 0) { /* 1 MiB */
      pthread_attr_destroy(&attr);
      have_attr = 0;
    }
  }
  int cr = have_attr ? pthread_create(&th, &attr, sm_kern_worker, &g_kern_job)
                     : pthread_create(&th, NULL, sm_kern_worker, &g_kern_job);
  if (have_attr) pthread_attr_destroy(&attr);
  if (cr) {
    pthread_mutex_lock(&g_kern_lock);
    g_kern_busy = 0;
    pthread_mutex_unlock(&g_kern_lock);
    return -3;
  }
  pthread_detach(th);

  for (waited = 0; waited < SM_MOUNT_TIMEOUT_MS; waited += 50) {
    if (g_kern_job.done) break;
    usleep(50 * 1000);
  }
  if (!g_kern_job.done) {
    g_kern_stuck++;
    mango_log("savemgr: %s still in kernel after %dms (stuck #%u) — "
              "refusing further kernel mount calls until it returns",
              op ? "umount" : "mount", SM_MOUNT_TIMEOUT_MS, g_kern_stuck);
    /* ★ 楔住即停车（2026-10-11 用户拍板「崩了别再自启」）：线程已进不可中断
       睡眠，SIGKILL 都带不走。旧姿态是"进程硬撑着继续跑"——用户手动结束它，
       注入器看到进程死亡就重注入，新实例又被楔死残躯占着 2026 ⇒ 落位 2027
       ⇒「杀了又出现、功能全废」。现在停车：进程驻留（注入器不重注入），
       端口随 accept 循环收尾释放，用户手动结束时它死得干净、新实例接得回
       2026。park 是终身的——内核状态已不可信。 */
    websrv_park("kernel mount call never returned (uninterruptible sleep)");
    return -1;
  }
  if (out_rc) *out_rc = g_kern_job.rc;
  if (out_mp && out_mp_len)
    snprintf(out_mp, out_mp_len, "%s", g_kern_job.mp);
  return 0;
}

int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len) {
  int rc = 0, r;

  /* ★ 挂载会话（2026-10-11 方案 B，事实来自 Garlic 的 mount_save：
   *   `if (g_mounted) return -1;`）。
   *
   * 旧写法把"先卸了再挂"推给调用方：调用方 `if (savemgr_is_mounted())
   * savemgr_unmount();` 之后**不看卸载结果**就往下挂。一旦那次 unmount 超时
   * （-100），g_mounted 仍是 1，第二次 mount 照样发进内核 ⇒ 内核里两个挂载叠
   * 在一起。PFS 挂载是全局互斥的，这就是"整机假死、SIGKILL 都带不走"的起点。
   *
   * 现在：替用户卸一次可以，但**卸不干净就绝不追加第二次挂载**，直接把
   * -104（楔住）交上去，让路由层停止这一批。 */
  if (g_mounted) {
    if (savemgr_unmount() != 0) {
      mango_log("savemgr_mount_image: previous mount did not release -- "
                "refusing to stack a second mount");
      savemgr_import_log("⚠ 上一次挂载没有卸干净（内核未返回），已停止后续挂载；"
                         "请重启主机后再试");
      return -104;
    }
  }

  r = sm_kern_run(0, img_path, mount_point, mp_len, &rc);

  if (r == -2) {
    mango_log("savemgr_mount_image refused: previous kernel call still stuck");
    savemgr_import_log("⚠ 上一次挂载还卡在内核里，本次挂载被拒绝（不再往内核塞第二次）");
    return -101;
  }
  if (r == -3) return -103;
  if (r == -1) {
    savemgr_import_log("⚠ 挂载超时（%d 秒）：内核没有返回，已放弃这条线程并停止后续挂载",
                       SM_MOUNT_TIMEOUT_MS / 1000);
    mango_log("savemgr_mount_image timeout after %dms", SM_MOUNT_TIMEOUT_MS);
    return -100;
  }
  return rc;
}

int savemgr_unmount(void) {
  int rc = 0, r = sm_kern_run(1, NULL, NULL, 0, &rc);

  if (r == -2) return -101;     /* 上一次还没回来：别再塞第二次 */
  if (r == -3) return -103;
  if (r == -1) {
    mango_log("savemgr_unmount timeout after %dms (stuck #%u)",
              SM_MOUNT_TIMEOUT_MS, g_kern_stuck);
    return -100;
  }
  return rc;
}

/* Resolves sceFsCreatePprPfsSaveDataImage once, from the Vsh-internal fs
 * library (bare name first, then the system path — same fallback pattern the
 * installer uses for libSceIpmi/libSceAppInstUtil). */
static ppr_create_fn
ppr_create_once(void) {
  ppr_create_fn fn;

  pthread_mutex_lock(&g_ppr_lock);
  if(!g_ppr_create) {
    void *vsh = dlopen("libSceFsInternalForVsh.sprx", RTLD_LAZY);
    if(!vsh)
      vsh = dlopen("/system/common/lib/libSceFsInternalForVsh.sprx", RTLD_LAZY);
    if(vsh)
      g_ppr_create = (ppr_create_fn)dlsym(vsh, "sceFsCreatePprPfsSaveDataImage");
  }
  fn = g_ppr_create;
  pthread_mutex_unlock(&g_ppr_lock);
  return fn;
}

/* Returns:
 *    0                image created (and out_key, when requested, carries the
 *                     key it is encrypted with)
 *   -2                could not create/truncate the image file
 *   -8/-9/-10         PS4 only: sealed key generation / pfsmgr open / decrypt
 *   -11               PS5 only: Ppr create symbol unavailable
 *   anything negative from the sceFs create call itself (Orbis error) */
int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key,
                       int is_ps4) {
  (void)save_domain_escalate_full();
  recursive_mkdir("/data/Mango/tmp");
  uint64_t img_size = data_len + data_len / 4 + (4 * 1024 * 1024);
  if (img_size < 32 * 1024 * 1024) img_size = 32 * 1024 * 1024;
  img_size = ((img_size + 32767) / 32768) * 32768;

  uint8_t key[0x20];
  memset(key, 0, sizeof(key));

  if (is_ps4) {
    /* PS4 images are keyed by a FRESH sealed key: generate (sbl_srv, falling
     * back to pfsmgr), persist the sealed form as <img>.bin for later mounts,
     * then create with the DECRYPTED key via the non-Ppr call and no
     * compression flag. Mirrors the reference implementation. */
    /* 同 decrypt_key()：0x40845303 与 0xc0845302 声明的载荷都是 2117 字节，
     * 缓冲必须来自堆（原来是 uint8_t sbuf[0x100], dbuf[0x100]，在栈上）。 */
    uint8_t *sbuf, *dbuf;
    int kfd, pfsmgr, ret;

    sbuf = calloc(1, 0x1000);
    dbuf = calloc(1, 0x1000);
    if (!sbuf || !dbuf) { free(sbuf); free(dbuf); return -8; }

    int gen_ok = 0;
    kfd = open("/dev/sbl_srv", O_RDWR);
    if (kfd >= 0) {
      if (ioctl(kfd, 0x40845303, sbuf) >= 0) gen_ok = 1;
      close(kfd);
    }
    if (!gen_ok) {
      memset(sbuf, 0, 0x1000);
      kfd = open("/dev/pfsmgr", O_RDWR);
      if (kfd >= 0) {
        if (ioctl(kfd, 0x40845303, sbuf) >= 0) gen_ok = 1;
        close(kfd);
      }
    }
    if (!gen_ok) { free(sbuf); free(dbuf); return -8; }

    char binp[MAX_PATH_LEN];
    snprintf(binp, sizeof(binp), "%s.bin", img_path);
    int bfd = open(binp, O_CREAT | O_TRUNC | O_WRONLY, 0777);
    if (bfd >= 0) {
      SM_IGNORE(write(bfd, sbuf, 0x60));
      close(bfd);
    }

    memcpy(dbuf, sbuf, 0x60);
    pfsmgr = open("/dev/pfsmgr", O_RDWR);
    if (pfsmgr < 0) { free(sbuf); free(dbuf); return -9; }
    ret = ioctl(pfsmgr, 0xc0845302, dbuf);
    close(pfsmgr);
    if (ret < 0) { free(sbuf); free(dbuf); return -10; }
    memcpy(key, dbuf + 0x60, 0x20);
    free(sbuf);
    free(dbuf);

    int fd = open(img_path, O_CREAT | O_TRUNC | O_RDWR, 0777);
    if (fd < 0) return -2;
    ret = sceFsUfsAllocateSaveData(fd, img_size, 0, 0);
    if (ret < 0) ftruncate(fd, (off_t)img_size);
    close(fd);

    CreateOpt copt;
    memset(&copt, 0, sizeof(copt));
    sceFsInitCreatePfsSaveDataOpt(&copt);
    ret = sceFsCreatePfsSaveDataImage(&copt, img_path, 0, img_size, key);
    if (ret < 0) return ret;
    if (out_key) memcpy(out_key, key, 0x20);
    return 0;
  }

  /* PS5: PPR image, compression flag, zero key. */
  int fd = open(img_path, O_CREAT | O_TRUNC | O_RDWR, 0777);
  if (fd < 0) return -2;
  int ret = sceFsUfsAllocateSaveData(fd, img_size, 0, 0);
  if (ret < 0) ftruncate(fd, (off_t)img_size);
  close(fd);

  ppr_create_fn ppr = ppr_create_once();
  if (!ppr) return -11;

  CreateOpt copt;
  memset(&copt, 0, sizeof(copt));
  sceFsInitCreatePfsSaveDataOpt(&copt);
  copt.flags[1] = 0x02;
  uint8_t zkey[0x20] = {0};
  ret = ppr(&copt, img_path, 0, img_size, zkey);
  if (ret < 0) return ret;
  if (out_key) memcpy(out_key, zkey, 0x20);
  return 0;
}

int savemgr_is_mounted(void) { return g_mounted; }
const char *savemgr_mount_point(void) { return g_mounted ? g_mount_point : ""; }
const char *savemgr_mounted_image(void) { return g_mounted ? g_mounted_img : ""; }
int savemgr_mounted_is_ps4(void) { return g_is_ps4; }

/* ---- 带台账进度的拷贝（2026-10-12 finalize 卡死定位用）------------------
 * 真机台账停在 `[1/3] unmount rc=0`，**连 finalize rc 都没打出来** ⇒ 卡点被
 * 夹在 finalize 的 mkdir 与 sqlite 之间，而那一整段此前一行日志都没有，于是
 * 无法区分「拷贝很慢」和「拷贝死了」——这两种情况的处置完全相反。
 * 这里每 8 MiB 落一行台账，下次卡住时能直接读出停在第几个字节。
 * 缓冲区走堆：64 KiB 压在请求线程栈上本来就不该（MHD 每连接一线程）。 */
#define SM_COPY_BUF      (64 * 1024)
#define SM_COPY_LOG_STEP (8ULL * 1024 * 1024)

static int
copy_file_logged(const char *src, const char *dst, const char *tag) {
  int in = open(src, O_RDONLY);
  if (in < 0) { mango_log("finalize: %s open src failed", tag); return -1; }
  int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (out < 0) {
    mango_log("finalize: %s open dst failed", tag);
    close(in);
    return -1;
  }
  uint8_t *buf = (uint8_t *)malloc(SM_COPY_BUF);
  if (!buf) { close(in); close(out); return -1; }

  uint64_t total = 0, next_at = SM_COPY_LOG_STEP;
  ssize_t n;
  int rc = 0;
  mango_log("finalize: %s copy begin", tag);
  while ((n = read(in, buf, SM_COPY_BUF)) > 0) {
    ssize_t w = 0;
    while (w < n) {
      ssize_t r = write(out, buf + w, (size_t)(n - w));
      if (r <= 0) {
        mango_log("finalize: %s write failed at %llu", tag,
                  (unsigned long long)(total + (uint64_t)w));
        rc = -1;
        goto out;
      }
      w += r;
    }
    total += (uint64_t)n;
    if (total >= next_at) {
      mango_log("finalize: %s copy %llu bytes", tag, (unsigned long long)total);
      next_at += SM_COPY_LOG_STEP;
    }
  }
out:
  free(buf);
  close(in);
  close(out);
  mango_log("finalize: %s copy end %llu bytes rc=%d", tag,
            (unsigned long long)total, rc);
  return rc;
}

int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info) {
  char sd_dir[MAX_PATH_LEN], sd_dst[MAX_PATH_LEN];
  snprintf(sd_dir, sizeof(sd_dir), "/user/home/%s/%s/%s", uid_hex,
           is_ps4 ? "savedata" : "savedata_prospero", info->title_id);
  snprintf(sd_dst, sizeof(sd_dst), "%s/sdimg_%s", sd_dir, info->dir_name);
  mango_log("finalize: begin tid=%s dir=%s ps4=%d uid=%s",
            info->title_id, info->dir_name, is_ps4, uid_hex);
  mango_log("finalize: mkdir %s", sd_dir);
  recursive_mkdir(sd_dir);
  if (copy_file_logged(src_img, sd_dst, "image") < 0) {
    mango_log("finalize: image copy failed -> abort");
    return -1;
  }
  chmod(sd_dst, 0644);
  if (is_ps4) {
    /* PS4 saves keep their sealed key BESIDE the image: <dir_name>.bin (the
     * mount path strips the sdimg_ prefix and looks for exactly this name). */
    char sb[MAX_PATH_LEN], db[MAX_PATH_LEN];
    snprintf(sb, sizeof(sb), "%s.bin", src_img);
    snprintf(db, sizeof(db), "%s/%s.bin", sd_dir, info->dir_name);
    mango_log("finalize: bin copy");
    SM_IGNORE(copy_file(sb, db));
  }
  /* icon 不在**这里**拷 —— 本函数是卸载之后才调的，而 icon 只能从挂载点里读。
   * 旧实现正是这里按**镜像文件路径**去 stat 图标：镜像是加密的 PFS 容器，路径
   * 下没有那一层 ⇒ 恒失败、icon 从来拷不出来。现在改由 capture_icon 在每条
   * 导入路径**卸载之前**抓（守卫 chk_backup_restore.py 盯着这个顺序）。 */
  mango_log("finalize: db begin");
  int rc = savemgr_register_db(uid_hex, is_ps4, info);
  mango_log("finalize: db rc=%d", rc);
  return rc;
}

#else /* __linux__ host stubs */

int savemgr_mount_image(const char *img_path, char *mount_point, size_t mp_len) {
  (void)img_path; (void)mount_point; (void)mp_len; return -1;
}
int savemgr_unmount(void) { return -1; }
int savemgr_create_pfs(const char *img_path, uint64_t data_len, uint8_t *out_key,
                       int is_ps4) {
  (void)img_path; (void)data_len; (void)out_key; (void)is_ps4; return -1;
}
int savemgr_is_mounted(void) { return 0; }
const char *savemgr_mount_point(void) { return ""; }
const char *savemgr_mounted_image(void) { return ""; }
int savemgr_mounted_is_ps4(void) { return 0; }
int savemgr_import_finalize(const char *src_img, const char *uid_hex,
                            int is_ps4, const sfo_info_t *info) {
  (void)src_img; (void)uid_hex; (void)is_ps4; (void)info; return -1;
}

#endif

/* ==================== 用户信息解析看护（2026-10-11 夜，v1.0.17） ==========
 * 真机实证：点「替换覆盖」后窗口冻在「准备中…」，台账既无 SIGSEGV 也无
 * PARKED。「准备中…」= finish 路由已被 dispatch 接管（进度被 begin 清 0），
 * 但**第一条进度都还没写出来** ⇒ 卡点被夹在 begin() 与第一次
 * savemgr_import_progress 之间。那一段只有：
 *   sm_param（纯字符串解析，不可能卡）、savemgr_foreground_uid()、
 *   savemgr_account_id_for_user() —— 后两个在真机上要进系统服务
 *   （sceUserServiceGetForegroundUser / sceRegMgrGetStr / sceRegMgrGetBin）
 *   和 /user/home 文件系统，与 mount/umount 同属「可能永不返回」的一类，
 *   此前却完全裸奔：卡住 = 窗口无限期「准备中…」+ 台账一片空白。
 *
 * 对策与挂载看护同型：独立线程跑，父线程只等 SM_USERINFO_TIMEOUT_MS。
 * 超时：放弃线程（随它睡在内核里）、置 g_userinfo_stuck 拒绝后续调用、
 * 给前端 503 与明确文案、进台账。**不 park**：RegMgr 楔住时网页服务本身
 * 还能活，处置权交给用户（重启主机清系统服务状态）。
 *
 * ⚠️ 位置：必须放在 1390 行 #endif（host stubs 结束）**之后** —— 挂载看护
 * 那一段整体在 #ifndef __linux__ 里，最初把这块插在它中间，宿主一编译就
 * implicit declaration（2026-10-11 夜 verify 抓到）。 */
#define SM_USERINFO_TIMEOUT_MS 15000

typedef struct {
  char uid_in[16];             /* 调用方给的 uid **拷贝进来**：超时后调用方的
                                  内存就没了，线程绝不能再碰它 */
  char uid_hex[16];
  int  fuid;
  uint64_t aid;
  volatile int done;
} sm_userinfo_job_t;

static sm_userinfo_job_t g_userinfo_job;    /* 同 g_kern_job：静态，线程独用 */
static int               g_userinfo_stuck;  /* 超时后终身拒绝（进程重启才清） */

static void *
sm_userinfo_worker(void *arg) {
  sm_userinfo_job_t *j = (sm_userinfo_job_t *)arg;

  /* UserService 的初始化在 savemgr_foreground_uid() 内部（pthread_once），
   * 这里不再重复 —— 见该函数的注释。 */

  if (!j->uid_in[0]) {
    int f = savemgr_foreground_uid();
    j->fuid = f;
    snprintf(j->uid_hex, sizeof(j->uid_hex), "%x", (unsigned)f);
  } else {
    snprintf(j->uid_hex, sizeof(j->uid_hex), "%s", j->uid_in);
    j->fuid = savemgr_foreground_uid();
    /* 兜底：即使 Initialize 之后仍拿到 0（前台是系统进程、或该服务不可用），
     * 也绝不能把 0 当用户写进 SFO —— 回退到调用方给的那个 uid。 */
    if (j->fuid == 0)
      j->fuid = (int)strtoul(j->uid_hex, NULL, 16);
  }
  j->aid = savemgr_account_id_for_user(j->uid_hex);
  j->done = 1;
  return NULL;
}

/* 0 = 成功（*out_uid 必有效；out_fuid/out_aid 可为 NULL）；
 * -1 = 本次超时或线程起不来；-2 = 之前超时过，拒绝再进系统服务。 */
static int
sm_userinfo_run(const char *uid_in, char *out_uid,
                int *out_fuid, uint64_t *out_aid) {
  pthread_t th;
  int waited;

  if (g_userinfo_stuck) return -2;
  memset(&g_userinfo_job, 0, sizeof(g_userinfo_job));
  if (uid_in && uid_in[0])
    snprintf(g_userinfo_job.uid_in, sizeof(g_userinfo_job.uid_in), "%.15s", uid_in);

  if (pthread_create(&th, NULL, sm_userinfo_worker, &g_userinfo_job) != 0)
    return -1;
  pthread_detach(th);

  for (waited = 0; waited < SM_USERINFO_TIMEOUT_MS; waited += 50) {
    if (g_userinfo_job.done) break;
    usleep(50 * 1000);
  }
  if (!g_userinfo_job.done) {
    g_userinfo_stuck = 1;
    mango_log("user-info syscall still in kernel/service after %dms "
              "(foreground uid / regmgr) — refusing further user-info calls "
              "until process restart (console reboot recommended)",
              SM_USERINFO_TIMEOUT_MS);
    return -1;
  }
  snprintf(out_uid, 16, "%s", g_userinfo_job.uid_hex);
  if (out_fuid) *out_fuid = g_userinfo_job.fuid;
  if (out_aid)  *out_aid  = g_userinfo_job.aid;
  return 0;
}

/* ========================= 启动期自恢复 ================================
 * 上一次异常退出会留下三样东西，任何一样都能让**自启后的新进程什么都干不了**
 * （用户 2026-10-10 反馈「结束进程他还会自启，而且插件功能也不能用」）：
 *   1. /data/Mango/tmp 下的 _nex_arc（整包解压结果，可几百 MB）与各种 _nex_*
 *      临时件 —— 把 /data 占满之后，任何写盘都失败，看着就像"整个插件坏了"；
 *   2. /data/mount_sd 上可能还挂着上次那个镜像 —— PFS 挂载是全局互斥的，
 *      新进程再 mount 同一个挂载点会直接失败，存档功能全废；
 *   3. 没人记录过当时还剩多少空间，出问题只能靠猜。
 * 所以启动时做一次「清残留 + 报空闲 + 清残留挂载」，结果全部写进台账。
 * ⚠️ 只动 /data/Mango/tmp 下**本工具自己**的前缀，且整个进程只跑一次。 */
static const char *const sm_tmp_prefixes[] = {
  "_nex_arc", "_nex_zip_", "_nex_up_", "_nex_new_", "_nex_enc_",
  "_nex_bak_", "_nex_dl_", "_nex_encdl_", "_nex_resigndl_",
  /* ★ _nex_restore_ 必须在这里：还原是把整包摊到 /data/Mango/tmp 再逐个 copy
   * 回 /user/home，几百 MB 的摊包结果只要上一次没走完就留在盘上。漏登记 ⇒
   * 崩溃残留永远清不掉 ⇒ /data 被占满 ⇒ 自启后的新进程任何写盘都失败，
   * 用户看到的就是「结束掉又起来，而且功能全不能用」（2026-10-13 报障）。 */
  "_nex_restore_",
};

void savemgr_boot_reconcile(void) {
  char tmp[MAX_PATH_LEN];
  struct statvfs vfs;
  DIR *d;
  int removed = 0;
  unsigned long long freed_kb = 0;

  sm_map_path("/data/Mango/tmp", tmp, sizeof(tmp));
  recursive_mkdir(tmp);

  d = opendir(tmp);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      size_t i;
      int hit = 0;
      char p[MAX_PATH_LEN];
      struct stat st;

      for (i = 0; i < sizeof(sm_tmp_prefixes) / sizeof(sm_tmp_prefixes[0]); i++)
        if (!strncmp(e->d_name, sm_tmp_prefixes[i], strlen(sm_tmp_prefixes[i]))) {
          hit = 1;
          break;
        }
      if (!hit) continue;
      snprintf(p, sizeof(p), "%s/%s", tmp, e->d_name);
      if (lstat(p, &st)) continue;
      if (S_ISDIR(st.st_mode)) sm_arc_rm_rf(p);
      else { freed_kb += (unsigned long long)st.st_size / 1024; unlink(p); }
      removed++;
    }
    closedir(d);
  }

  if (statvfs(tmp, &vfs) == 0) {
    unsigned long long free_mb =
      (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize /
      (1024ULL * 1024ULL);
    mango_log("boot reconcile: cleared %d stale entries (~%llu KB); "
              "%llu MB free on the data volume", removed, freed_kb, free_mb);
    if (free_mb < 64)
      savemgr_import_log("⚠ 数据分区只剩 %llu MB —— 导入大存档很可能失败，"
                         "请先删掉不需要的存档包 / 备份", free_mb);
  } else {
    mango_log("boot reconcile: cleared %d stale entries (~%llu KB); "
              "statvfs(%s) failed: %s", removed, freed_kb, tmp, strerror(errno));
  }

#ifndef __linux__
  {
    /* 残留挂载也一样用看护线程清（它同样可能卡在内核里）。rc 只是参考：
       本来就没挂载时这条会返回错误，属于正常。 */
    int rc = 0, r = sm_kern_run(2, NULL, NULL, 0, &rc);
    mango_log("boot reconcile: stale /data/mount_sd umount -> r=%d rc=%d", r, rc);
  }
#endif
}

/* ========================= 暂存源包自动清理 ============================
 * 用户 2026-10-11 反馈：「上传了一些临时文件如果不用了可以自动删除掉」。
 * 前端的长导入都是「先把包流式传到盘上，再让后端按路径读」——
 * 传进来的那份源包用完就没人管了（旧文案甚至写着「可到文件页删除」，
 * 等于把清理工作推给用户）。这里在**操作成功之后**把它删掉。
 *
 * 三条自我约束，越界即 bug：
 *   ① 只认两个**本工具自己的暂存根**（/data/Mango/incoming、/data/Mango/tmp）；
 *      用户自己放在别处（U 盘、/data/pkg、自己的目录）的包一律不碰；
 *   ② 只删普通文件，不递归、不删目录；
 *   ③ 只在**成功**之后调用 —— 失败要留着重试与排查，和 PKG「装完自动清理、
 *      失败保留」是同一条纪律。 */
static int
sm_is_staged_src(const char *path) {
  static const char *const roots[] = {"/data/Mango/incoming/", "/data/Mango/tmp/"};
  size_t i;

  if (!path) return 0;
  for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    if (!strncmp(path, roots[i], strlen(roots[i]))) return 1;
  }
  return 0;
}

static void
sm_cleanup_staged_src(const char *path) {
  char mapped[MAX_PATH_LEN];
  struct stat st;

  if (!sm_is_staged_src(path)) return;
  sm_map_path(path, mapped, sizeof(mapped));
  if (stat(mapped, &st) || !S_ISREG(st.st_mode)) return;
  if (unlink(mapped) == 0)
    savemgr_import_log("· 已自动清理上传到暂存的源包（%s）", path);
  /* ENOENT 是正常的（兼容入口 _nex_up_*.zip 自己也会 unlink），不再多嘴；
     真正删不掉（只读挂载等）才提醒用户手动去文件页处理。 */
  else if (errno != ENOENT)
    savemgr_import_log("⚠ 暂存源包没清掉（可到文件页手动删除）：%s", path);
}

/* ============================================================ API ====== */

/* ⚠️ send_buffer() 是**接管所有权**的（MHD_RESPMEM_MUST_FREE）—— 传进去的
 * 指针必须是把命交出去的堆内存。曾经这里直接传栈上的 buf，于是 MHD 后来对
 * 一个栈地址调 free()：glibc 把指针前面那个字当块头读，看着像就把它挂进
 * free 链表、正常返回 —— 堆元数据就此烂掉，abort（free(): invalid pointer）
 * 落到**之后某个毫不相干的请求**上。实际就是这么暴露的：
 * /api/save_mgr/import_archive_cancel 回了 200，下一个请求把进程带走。
 * 全仓库发送点由 .build/chk_send_buffer.py 机械守住。 */
static enum MHD_Result json_ok(struct MHD_Connection *conn, const char *extra) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "{\"ok\":true%s%s}", extra ? "," : "", extra ? extra : "");
  return send_buffer(conn, MHD_HTTP_OK, strdup(buf), "application/json");
}

/* ★ 往固定大小的 JSON 缓冲里追加一段时，累加器必须是**实际写入的字节数**，
   而不是 snprintf() 的返回值。snprintf 返回的是"假如空间足够、本应写入的
   长度"，所以经典的
       pos += snprintf(buf + pos, sizeof(buf) - pos, ...);
   一旦内容总量超过 out，pos 就大于 sizeof(out)，而下一个 sizeof(out) - pos
   在 size_t 域里**下溢**成一个天文数字 —— 紧接着的
   snprintf(out + pos, 天文数字, ...) 就是一次越界写。out 在栈上，踩掉的是
   返回地址，表现为"进程在处理另一个请求时突然 SIGSEGV"。

   2026-10-11 真机台账三次崩溃的最后一条请求都是 POST /api/save_mgr/list，
   正是这个形状。这条路径宿主上跑不到（list 整段在 #ifndef __linux__ 里），
   所以另由 .build/chk_json_append.py 机械禁止裸的 `pos += snprintf`。 */
static void
sm_json_append(char *out, size_t cap, size_t *pos, const char *fmt, ...) {
  va_list ap;
  int w;

  if (*pos >= cap - 1) return;            /* 已经满了：只丢弃，绝不越界 */
  va_start(ap, fmt);
  w = vsnprintf(out + *pos, cap - *pos, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  if ((size_t)w >= cap - *pos) { *pos = cap - 1; return; }   /* 被截断 */
  *pos += (size_t)w;
}

/* ★ 内核调用「楔住」的统一判据（2026-10-11 方案 B：照 Garlic 的挂载会话模型）。
 *
 * 挂载/卸载一旦命中这几条返回码，就说明**内核那边已经不可信了**，此时唯一正确
 * 的动作是**立刻停止继续往内核塞调用**，而不是像旧代码那样"这一张失败、接着试
 * 下一张"。理由很硬：sceFsMountSaveData 的 PFS 层是全局互斥的，第一次卡住往往
 * 只是丢了一条不可中断睡眠的线程；再塞第二次就会把整台主机的挂载子系统一起拖住
 * —— 那就是用户描述的「进程还在、杀了还自启、网页全废、只能重启主机」。
 *
 * 取值来源（sm_kern_run / savemgr_mount_image / savemgr_unmount）：
 *   -100 看护超时（内核没返回，线程已放弃）
 *   -101 上一次内核调用还没回来，本次被拒
 *   -103 看护线程起不来
 *   -104 前一次挂载没有卸干净，拒绝叠加第二次挂载（见 savemgr_mount_image）
 * 宿主构建里挂载恒失败（stub 返回 -1），永远走不到这几条 —— 所以这个助手本身
 * 在两边都编译，只为了让路由层能用同一句话判断。 */
static int
sm_kern_wedged(int mount_rc) {
  return mount_rc == -100 || mount_rc == -101 ||
         mount_rc == -103 || mount_rc == -104;
}

/* 前向声明：下面辅助函数定义在文件后半段，扫描路径要复用它们。 */
static void sm_db_savedata_info(const char *uid_hex, int is_ps4,
                                const char *tid, const char *dir,
                                uint32_t *blocks, char *mt, size_t mtsz);
static void sm_json_esc(const char *in, char *out, size_t outsz);
static long sm_json_num(const char *j, const char *key, long def);

enum MHD_Result api_save_mgr_list(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  /* Device-only scan of /user/home/<uid>/savedata_prospero and /savedata. */
#ifndef __linux__
  char *out = malloc(SM_LIST_JSON_CAP);
  if (!out) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  size_t pos = 0;
  sm_json_append(out, SM_LIST_JSON_CAP, &pos, "{\"saves\":[");
  int first = 1;
  /* 游戏名缓存：key = title_id。查一次 app.db 要开一次系统库，一个游戏 3~4 个
     槽位时逐条查就是白付三四倍开销。 */
  struct { char tid[64]; char gt[256]; } gtcache[SM_LIST_GT_CACHE];
  int ngtc = 0;
  DIR *home = opendir("/user/home");
  if (home) {
    struct dirent *hu;
    while ((hu = readdir(home))) {
      if (hu->d_name[0] == '.') continue;
      const char *bases[] = {"savedata_prospero", "savedata"};
      for (int b = 0; b < 2; b++) {
        char tp[MAX_PATH_LEN];
        snprintf(tp, sizeof(tp), "/user/home/%s/%s", hu->d_name, bases[b]);
        DIR *td = opendir(tp);
        if (!td) continue;
        struct dirent *te;
        while ((te = readdir(td))) {
          if (te->d_name[0] == '.') continue;
          char title_path[MAX_PATH_LEN];
          snprintf(title_path, sizeof(title_path), "%s/%s", tp, te->d_name);
          struct stat tst; if (stat(title_path, &tst) < 0 || !S_ISDIR(tst.st_mode)) continue;
          DIR *sd = opendir(title_path);
          if (!sd) continue;
          struct dirent *se;
          while ((se = readdir(sd))) {
            if (se->d_name[0] == '.') continue;
            size_t l = strlen(se->d_name);
            if (l > 4 && strcmp(se->d_name + l - 4, ".bin") == 0) continue;
            if (b == 1 && strncmp(se->d_name, "sdimg_", 6) != 0) continue;
            char fp[MAX_PATH_LEN];
            snprintf(fp, sizeof(fp), "%s/%s", title_path, se->d_name);
            struct stat fst; if (stat(fp, &fst) < 0 || !S_ISREG(fst.st_mode)) continue;
            /* 平台由父目录决定：savedata = PS4，savedata_prospero = PS5。
             * 以前靠读镜像头字节 0x01 当启发式，会把部分 PS5 镜像误判成 PS4，
             * 导致卡片平台标签/路径显示错。 */
            int ps4 = (b == 1);
            /* ★ dir_name = **文件名去掉 sdimg_ 前缀**；title_id = **父目录名**。
             * 🪤 旧实现在这里把镜像文件当目录，去读 `<镜像>/sce_sys/param.sfo`：
             *   镜像是加密 PFS 容器，**不挂载就读不到 SFO** ⇒ savemgr_parse_sfo
             *   恒失败，dir_name 退化成 "sdimg_xxx"，备份再拼一次前缀就成了
             *   sdimg_sdimg_xxx ⇒ 真机 v1.0.24 的备份全废（界面显示
             *   "name not found"，即 ENOENT 的本地化文本）。
             *   扫描阶段一律**不挂载**（Garlic 模型：挂载是每次用户动作一次的高
             *   风险内核调用，扫一遍全库就挂几十次是不可接受的）。 */
            char dirnm[256];
            sm_dir_name_from_entry(se->d_name, dirnm, sizeof(dirnm));
            /* 游戏名不靠 SFO（上面说了读不到），改查系统存档库；库里没有就留空，
               前端自己回落显示 title_id —— 不能让它挡住备份这条主路径。 */
            char mt_raw[256], mt[512];
            mt_raw[0] = 0;
            {
              uint32_t blks = 0;
              sm_db_savedata_info(hu->d_name, ps4, te->d_name, dirnm, &blks,
                                  mt_raw, sizeof(mt_raw));
            }
            /* 游戏名来自数据库，是**用户可见的自由文本**：直接拼进 JSON 的话，
               一个引号就毁掉整份 list 响应（扫描页全空）。必须先转义。 */
            sm_json_esc(mt_raw, mt, sizeof(mt));
            /* 真游戏名查 app.db。**按 title_id 缓存**：一个游戏有几个槽位就开
               几次系统库太亏（app.db 每次都要打开、且只读模式也要走 busy
               timeout），缓存之后一个游戏只查一次。 */
            char gt_raw[256], gt[512];
            gt_raw[0] = 0;
            {
              int known = 0;
              for (int k = 0; k < ngtc; k++) {
                if (strcmp(gtcache[k].tid, te->d_name) == 0) {
                  snprintf(gt_raw, sizeof(gt_raw), "%s", gtcache[k].gt);
                  known = 1;
                  break;
                }
              }
              if (!known && ngtc < SM_LIST_GT_CACHE) {
                sm_db_game_title(te->d_name, gt_raw, sizeof(gt_raw));
                snprintf(gtcache[ngtc].tid, sizeof(gtcache[ngtc].tid), "%s",
                         te->d_name);
                snprintf(gtcache[ngtc].gt, sizeof(gtcache[ngtc].gt), "%s",
                         gt_raw);
                ngtc++;
              }
            }
            sm_json_esc(gt_raw, gt, sizeof(gt));
            sm_json_append(out, SM_LIST_JSON_CAP, &pos, "%s", first ? "" : ",");
            first = 0;
            sm_json_append(out, SM_LIST_JSON_CAP, &pos,
              "{\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"%s\","
              "\"game_title\":\"%s\","
              "\"title_name\":\"%s\","
              "\"is_ps4\":%s,\"uid\":\"%s\",\"path\":\"%s\","
              "\"size\":%lld,\"mtime\":%lld}",
              te->d_name, dirnm, mt, gt,
              gt_raw[0] ? gt : (mt_raw[0] ? mt : te->d_name),
              ps4 ? "true" : "false", hu->d_name, fp,
              (long long)fst.st_size, (long long)fst.st_mtime);
          }
          closedir(sd);
        }
        closedir(td);
      }
    }
    closedir(home);
  }
  sm_json_append(out, SM_LIST_JSON_CAP, &pos, "]}");
  /* 所有权直接交给 send_buffer（它按 MUST_FREE 释放）—— out 本来就是堆，
     不必再 strdup 一份：这块缓冲从头到尾只服务这一个响应。 */
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
#else
  return send_buffer(conn, MHD_HTTP_OK,
    strdup("{\"saves\":[],\"note\":\"save scan is device-only; run on PS5\"}"),
    "application/json");
#endif
}

/* ---- 快速游戏级存档列表（ps5upload 风格）-------------------------------- */
/* 512 KB：~600 个游戏条目才截断。ps5upload 实测这个容量足够普通用户库。 */
#define SM_GAMES_JSON_CAP (512 * 1024)

#ifndef __linux__
/* 只扫描 root 目录下两层（immediate files + 子目录里的文件，如 sce_sys/）。
 * 不递归更深，也不挂载任何 PFS 镜像 —— 这就是它比 /api/save_mgr/list 快的根因。
 * 同时返回：总大小、第一个 sdimg_* 槽位名（去掉前缀）、槽位总数。 */
static void
sm_dir_scan_approx(const char *root, long long *total_out,
                   char *dir_out, size_t dir_out_sz, int *slots_out) {
  long long total = 0;
  int slots = 0;
  if (dir_out && dir_out_sz) dir_out[0] = 0;
  DIR *dp = opendir(root);
  if (!dp) { *total_out = 0; if (slots_out) *slots_out = 0; return; }
  struct dirent *e;
  while ((e = readdir(dp)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof(p), "%s/%s", root, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISREG(st.st_mode)) {
      total += st.st_size;
      /* 槽位 = 以 sdimg_ 开头的镜像文件；.bin 是密钥不算槽位。 */
      if (strncmp(e->d_name, "sdimg_", 6) == 0) {
        slots++;
        if (dir_out && dir_out_sz && !dir_out[0]) {
          const char *pname = e->d_name + 6;
          snprintf(dir_out, dir_out_sz, "%s", pname);
        }
      }
    } else if (S_ISDIR(st.st_mode)) {
      DIR *cd = opendir(p);
      if (cd) {
        struct dirent *ce;
        while ((ce = readdir(cd)) != NULL) {
          if (ce->d_name[0] == '.') continue;
          char cp[MAX_PATH_LEN];
          snprintf(cp, sizeof(cp), "%s/%s", p, ce->d_name);
          struct stat cst;
          if (stat(cp, &cst) == 0 && S_ISREG(cst.st_mode))
            total += cst.st_size;
        }
        closedir(cd);
      }
    }
  }
  closedir(dp);
  *total_out = total;
  if (slots_out) *slots_out = slots;
}
#endif

enum MHD_Result api_save_mgr_games(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  (void)url;
  long filter_uid = 0;
  int have_filter = 0;
  if (body && body_size > 0 && body_size < 512) {
    char buf[520];
    memcpy(buf, body, body_size);
    buf[body_size] = '\0';
    /* 优先认 Mango 现有 frontend 用的 uid（16 进制字符串）。 */
    const char *p = strstr(buf, "\"uid\"");
    if (p) {
      p += strlen("\"uid\"");
      while (*p == ' ' || *p == ':' || *p == '"') p++;
      char tmp[16];
      int i = 0;
      while (*p && *p != '"' && i < (int)sizeof(tmp) - 1) tmp[i++] = *p++;
      tmp[i] = '\0';
      if (tmp[0]) {
        filter_uid = (long)strtoul(tmp, NULL, 16);
        have_filter = 1;
      }
    } else {
      /* ps5upload 风格：user_id 是十进制整数。 */
      filter_uid = sm_json_num(buf, "user_id", 0);
      if (filter_uid != 0) have_filter = 1;
    }
  }

#ifndef __linux__
  char *out = malloc(SM_GAMES_JSON_CAP);
  if (!out) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  size_t pos = 0;
  sm_json_append(out, SM_GAMES_JSON_CAP, &pos, "{\"saves\":[");
  int first = 1;

  /* title_id -> (game_title, installed) 缓存；每个游戏只查一次 app.db。 */
  struct { char tid[64]; char gt[256]; int installed; } gtcache[SM_LIST_GT_CACHE];
  int ngtc = 0;

  DIR *home = opendir("/user/home");
  if (home) {
    struct dirent *hu;
    while ((hu = readdir(home)) != NULL) {
      if (hu->d_name[0] == '.') continue;
      char *endp = NULL;
      unsigned long uid_u32 = strtoul(hu->d_name, &endp, 16);
      if (!endp || *endp != '\0' || uid_u32 == 0) continue;
      long uid_for_json = (long)(uid_u32 & 0x7FFFFFFFu);
      if (have_filter && uid_for_json != filter_uid) continue;

      const char *bases[2] = {"savedata_prospero", "savedata"};
      for (int b = 0; b < 2; b++) {
        char root[MAX_PATH_LEN];
        snprintf(root, sizeof(root), "/user/home/%s/%s", hu->d_name, bases[b]);
        DIR *dp = opendir(root);
        if (!dp) continue;
        struct dirent *te;
        while ((te = readdir(dp)) != NULL) {
          if (te->d_name[0] == '.') continue;
          char path[MAX_PATH_LEN];
          snprintf(path, sizeof(path), "%s/%s", root, te->d_name);
          struct stat st;
          if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
          if (pos >= SM_GAMES_JSON_CAP - 1024) break;

          /* 系统备份容器（sce_backup<N>）不是游戏：里面没有可挂的镜像、没有
           * 名字/封面可查，列出来只会成为一张「裸 tid + 占位图」卡 —— 跳过。 */
          if (sm_is_system_container(te->d_name)) continue;

          /* 游戏名：统一走 sm_game_display_name（app.db → appmeta → 本地缓存，
           * 查到的顺手写缓存，卸载后仍能显示）。installed 供前端打「已卸载」标。 */
          char gt_raw[256] = {0};
          int installed = 1;
          if (ngtc < SM_LIST_GT_CACHE) {
            int known = 0;
            for (int k = 0; k < ngtc; k++) {
              if (strcmp(gtcache[k].tid, te->d_name) == 0) {
                snprintf(gt_raw, sizeof(gt_raw), "%s", gtcache[k].gt);
                installed = gtcache[k].installed;
                known = 1;
                break;
              }
            }
            if (!known) {
              sm_game_display_name(te->d_name, gt_raw, sizeof(gt_raw),
                                   &installed);
              snprintf(gtcache[ngtc].tid, sizeof(gtcache[ngtc].tid), "%s",
                       te->d_name);
              snprintf(gtcache[ngtc].gt, sizeof(gtcache[ngtc].gt), "%s",
                       gt_raw);
              gtcache[ngtc].installed = installed;
              ngtc++;
            }
          } else {
            sm_game_display_name(te->d_name, gt_raw, sizeof(gt_raw),
                                 &installed);
          }

          char gt[512] = {0}, path_esc[2048] = {0}, dir_esc[256] = {0};
          sm_json_esc(gt_raw, gt, sizeof(gt));
          sm_json_esc(path, path_esc, sizeof(path_esc));

          long long total = 0;
          char first_dir[256] = {0};
          int slots = 0;
          sm_dir_scan_approx(path, &total, first_dir, sizeof(first_dir), &slots);
          sm_json_esc(first_dir, dir_esc, sizeof(dir_esc));
          sm_json_append(out, SM_GAMES_JSON_CAP, &pos, "%s", first ? "" : ",");
          first = 0;
          sm_json_append(out, SM_GAMES_JSON_CAP, &pos,
            "{\"title_id\":\"%s\",\"user_id\":%ld,\"uid\":\"%s\","
            "\"path\":\"%s\",\"size\":%lld,\"mtime\":%lld,"
            "\"kind\":\"%s\",\"is_ps4\":%s,\"dir_name\":\"%s\","
            "\"slots\":%d,\"installed\":%s%s%s%s}",
            te->d_name, uid_for_json, hu->d_name, path_esc, total,
            (long long)st.st_mtime,
            b == 1 ? "ps4" : "ps5",
            b == 1 ? "true" : "false",
            dir_esc, slots,
            installed ? "true" : "false",
            gt_raw[0] ? ",\"game_title\":\"" : "",
            gt_raw[0] ? gt : "",
            gt_raw[0] ? "\"" : "");
        }
        closedir(dp);
        if (pos >= SM_GAMES_JSON_CAP - 1024) break;
      }
    }
    closedir(home);
  }
  sm_json_append(out, SM_GAMES_JSON_CAP, &pos, "]}");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
#else
  (void)have_filter;
  return send_buffer(conn, MHD_HTTP_OK,
    strdup("{\"saves\":[],\"note\":\"save scan is device-only; run on PS5\"}"),
    "application/json");
#endif
}

enum MHD_Result api_save_mgr_mount(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  /* sm_param()（以及它兜底的 req_param()）返回的是**堆内存** —— 底层是
   * strdup / form_decode / json_decode_string，调用方负责释放。这一族函数
   * 历史上统一写成 `const char *x = req_param(...)` 且从不释放：参数每次
   * 调用都泄漏一条（本文件另外 45 处同类取值都规规矩矩 free，只有这里漏了）。
   * 三个参数在拼出 img 之后即无用处，就地 free。 */
  char *tid = sm_param(conn, url, body, body_size, "title_id");
  char *dir = sm_param(conn, url, body, body_size, "dir_name");
  char *uid = sm_param(conn, url, body, body_size, "uid");
  if (!tid || !dir) {
    free(tid); free(dir); free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "title_id/dir_name required");
  }
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
           uid_hex, tid, dir);
  /* ★ 浏览页挂载此前**一行日志都没有**：真机「点挂载就崩」时台账里什么都留不
   * 下，只能靠猜。这里把入参拼出的完整镜像路径先落一行 —— 路径拼错（uid 空、
   * dir 带分隔符、镜像不是 sdimg_ 前缀）是这条路由最常见的失败形态。 */
  mango_log("save_mgr/mount: enter tid=%s dir=%s uid=%s img=%s", tid, dir,
            uid_hex, img);
  free(tid); free(dir); free(uid);   /* 参数到此为止 */
  struct stat ist;
  if (stat(img, &ist) < 0)
    mango_log("save_mgr/mount: image does not exist: %s", img);
  else
    mango_log("save_mgr/mount: image size=%lld", (long long)ist.st_size);
  char mp[512];
  int r = savemgr_mount_image(img, mp, sizeof(mp));
  mango_log("save_mgr/mount: mount rc=%d mp=%s", r, r < 0 ? "" : mp);
  if (r < 0) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  char sfo[MAX_PATH_LEN];
  snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", mp);
  savemgr_cache_params_hmac(sfo);
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_unmount(struct MHD_Connection *conn, const char *url,
                                     const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  if (savemgr_unmount() < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "not mounted");
  return json_ok(conn, NULL);
}

/* /api/save_mgr/capture_icon —— 手动把镜像里的封面提出来。
 *
 * ★ 为什么必须单独开一条路由：卡片上那张图读的是
 *   /user/home/<uid>/savedata_prospero_meta/user/<tid>/<dir>_icon0.png，
 *   那是**系统**拷过去的一份副本；系统没拷过（真机 PPSA23226 的四个槽位下
 *   只有一个 .sfo，一张 icon0.png 都没有），卡片就永远是空的 —— 用户再点什么
 *   也点不出来，因为 Mango 原来只是"读现成的"，没有任何写那一层的入口。
 *   封面的**唯一出处**是镜像内部的 sce_sys/icon0.png，而镜像是加密 PFS 容器，
 *   不挂载读不到。所以"让它显示出来"这个动作必然等价于：
 *       挂一次 → 从挂载点拷 icon0.png 到 meta 目录 → 卸掉
 *   这正是 savemgr_capture_icon() 干的事，此前只在导入/重签流程里被调用过，
 *   普通用户没有任何入口能触发它。
 * ★ 只做 PS5：PS4 没有 savedata_prospero_meta 这一层，capture_icon 直接返回 -1。
 * ★ 挂载是高风险内核调用（可能永不返回），但 savemgr_mount_image 自带看护
 *   线程与 park，与 /api/save_mgr/mount 同一条路，不额外引入风险。
 * ★ 无论拷没拷到，**一定卸挂载**：留着一个挂载点等于把下一次挂载卡死。 */
enum MHD_Result api_save_mgr_capture_icon(struct MHD_Connection *conn,
                                          const char *url, const char *body,
                                          size_t body_size) {
  char *tid = sm_param(conn, url, body, body_size, "title_id");
  char *dir = sm_param(conn, url, body, body_size, "dir_name");
  char *uid = sm_param(conn, url, body, body_size, "uid");
  if (!tid || !dir) {
    free(tid); free(dir); free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "title_id/dir_name required");
  }
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);
  free(uid);
  if (!sm_safe_name(tid) || !sm_safe_name(dir)) {
    mango_log("save_mgr/capture_icon: unsafe tid=%s dir=%s", tid, dir);
    free(tid); free(dir);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad title_id/dir_name");
  }
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
           uid_hex, tid, dir);
  mango_log("save_mgr/capture_icon: enter tid=%s dir=%s uid=%s img=%s",
            tid, dir, uid_hex, img);
  struct stat ist;
  if (stat(img, &ist) < 0) {
    mango_log("save_mgr/capture_icon: no image %s", img);
    free(tid); free(dir);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "save image not found");
  }
  char mp[512];
  int r = savemgr_mount_image(img, mp, sizeof(mp));
  mango_log("save_mgr/capture_icon: mount rc=%d mp=%s", r, r < 0 ? "" : mp);
  if (r < 0) {
    free(tid); free(dir);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "mount failed");
  }
  sfo_info_t fin;
  memset(&fin, 0, sizeof(fin));
  snprintf(fin.title_id, sizeof(fin.title_id), "%s", tid);
  snprintf(fin.dir_name, sizeof(fin.dir_name), "%s", dir);
  int rc = savemgr_capture_icon(uid_hex, 0, &fin);
  mango_log("save_mgr/capture_icon: capture rc=%d", rc);
  savemgr_unmount();      /* 拷没拷到都要卸 */
  free(tid); free(dir);
  if (rc != 0)
    return send_json_error(conn, MHD_HTTP_NOT_FOUND,
                           "镜像里没有 sce_sys/icon0.png");
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_files(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  (void)body; (void)body_size;
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  char *sub = sm_param(conn, url, body, body_size, "path");
  char base[MAX_PATH_LEN];
  snprintf(base, sizeof(base), "%s/%s", savemgr_mount_point(), sub ? sub : "");
  char *out = malloc(SM_LIST_JSON_CAP);
  if (!out) { free(sub); return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
  size_t pos = 0;
  sm_json_append(out, SM_LIST_JSON_CAP, &pos,
                  "{\"root\":\"%s\",\"files\":[", savemgr_mount_point());
  DIR *d = opendir(base);
  int first = 1;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char fp[MAX_PATH_LEN]; snprintf(fp, sizeof(fp), "%s/%s", base, e->d_name);
      struct stat st; if (stat(fp, &st) < 0) continue;
      sm_json_append(out, SM_LIST_JSON_CAP, &pos, "%s", first ? "" : ",");
      first = 0;
      sm_json_append(out, SM_LIST_JSON_CAP, &pos,
        "{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
        e->d_name, S_ISDIR(st.st_mode) ? "true" : "false", (long long)st.st_size);
    }
    closedir(d);
  }
  sm_json_append(out, SM_LIST_JSON_CAP, &pos, "]}");
  free(sub);
  /* 同 list：out 是堆，直接交出去，不再 strdup。 */
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

enum MHD_Result api_save_mgr_read(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size) {
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  char *p = sm_param(conn, url, body, body_size, "path");
  if (!p || !p[0]) { free(p); return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required"); }
  char fp[MAX_PATH_LEN];
  snprintf(fp, sizeof(fp), "%s/%s", savemgr_mount_point(), p);
  free(p);
  int fd = open(fp, O_RDONLY);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  struct stat st;
  if (fstat(fd, &st) < 0) { close(fd); return send_json_error(conn, MHD_HTTP_NOT_FOUND, "stat failed"); }
  char *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
  if (!buf) { close(fd); return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
  {
    size_t got = 0; ssize_t r;
    while (got < (size_t)st.st_size &&
           (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
      got += (size_t)r;
  }
  close(fd);
  /* 所有权交给 send_buffer（MHD 负责 free）—— 这里**不能**再 free 一次。
   * 上一版就是这么写的：send_buffer 接管后又 free(buf)，标准的双重释放。 */
  return send_buffer(conn, MHD_HTTP_OK, buf, "application/octet-stream");
}

enum MHD_Result api_save_mgr_write(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  char *p = sm_param(conn, url, body, body_size, "path");
  if (!p || !p[0]) { free(p); return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required"); }
  char fp[MAX_PATH_LEN];
  snprintf(fp, sizeof(fp), "%s/%s", savemgr_mount_point(), p);
  free(p);
  int fd = open(fp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "write failed");
  {
    size_t off = 0; ssize_t w;
    while (off < body_size &&
           (w = write(fd, body + off, body_size - off)) > 0)
      off += (size_t)w;
  }
  close(fd);
  return json_ok(conn, NULL);
}

/* Import path 1: upload a zip of plaintext save files, build a fresh PFS. */
/* Shared import pipeline: build a fresh PFS image from a plaintext-save ZIP
 * already resident in memory, install it into the caller's account. Used by
 * BOTH entry points:
 *   /api/save_mgr/upload     — ZIP arrives in the HTTP body (buffered; the
 *                              web server caps that at 4 MB, which is exactly
 *                              why real-world zips died with
 *                              "request body is too large")
 *   /api/save_mgr/import_zip — ZIP was streamed to disk first via the
 *                              /api/upload-file channel (no size cap), then
 *                              read back here. This is the path the UI uses.
 * Returns the final MHD response.
 * ⚠️ Parameters come from sm_param() (JSON body / form / MHD GET args), NOT
 * from parsing the URL string: MHD's callback url never contains the query
 * string, so the old url_param(url, ...) parser silently saw NOTHING — the
 * only reason it went unnoticed is that the 4 MB body cap killed every real
 * request before parameter parsing could run. */

static enum MHD_Result
savemgr_import_zip_common(struct MHD_Connection *conn, const char *url,
                          const char *body, size_t body_size,
                          const char *zip_path, size_t zsize) {
  enum MHD_Result ret;
  /* 五个参数都是**堆内存**（sm_param 会 strdup），且要用到函数末尾（拼
   * param.sfo / finalize / 响应），所以统一在 done: 处释放一次。四个中途
   * 出口全改 goto done —— 历史版本每条都直接 return 且不释放，等于每逢导入
   * 失败就泄漏五条。 */
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char *tid = sm_param(conn, url, body, body_size, "title_id");
  char *dir = sm_param(conn, url, body, body_size, "dir_name");
  char *mt = sm_param(conn, url, body, body_size, "main_title");
  char *ps4s = sm_param(conn, url, body, body_size, "ps4");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  free(ps4s);                    /* 只用来推 is_ps4，到此为止 */
  if (!tid || !tid[0]) {
    ret = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "title_id required");
    goto done;
  }

  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  savemgr_import_log("导入开始：%s (uid=%s, is_ps4=%d)", tid, uid_hex, is_ps4);

  /* create a fresh PFS image in /data/Mango/tmp */
  char img[MAX_PATH_LEN];
  char imgbin[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/data/Mango/tmp/_nex_new_%s", tid);
  snprintf(imgbin, sizeof(imgbin), "%s.bin", img);
  uint8_t key[0x20];
  savemgr_import_log("第 1/4 步：创建新 PFS 镜像（%zu 字节）…", zsize);
  int cr = savemgr_create_pfs(img, zsize, key, is_ps4);
  if (cr < 0) {
    /* The hex code is the actionable part: Orbis errors pass through as-is,
     * internal codes are -2 (image open) / -8..-11 (key chain / Ppr symbol). */
    char why[96];
    snprintf(why, sizeof(why), "create pfs failed (0x%X)", (unsigned)cr);
    unlink(img);
    unlink(imgbin);
    savemgr_import_log("✗ create pfs 失败：%s", why);
    ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, why);
    goto done;
  }
  savemgr_import_log("✓ 镜像已创建");
  char mp[512];
  /* 2026-10-10：挂新镜像前务必卸掉旧挂载 —— 上次校验后没点 finish（关页/
   * 重试）会留下残留挂载，PS5 的 PFS 挂载是全局互斥的，重复 mount 会在内核
   * 调用里卡死这条线程。
   * 2026-10-11（方案 B）：这活儿现在由 savemgr_mount_image 内部承担，而且它
   * **卸不干净就不挂**（回 -104）；调用方再"先卸一次"只是白白多摸一次内核。 */
  savemgr_import_log("第 2/4 步：挂载镜像…");
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    unlink(imgbin);
    savemgr_import_log("✗ 挂载失败");
    ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
    goto done;
  }
  savemgr_import_log("✓ 已挂载");
  savemgr_import_progress(SM_IMP_EXTRACT, 0, (long long)zsize,
                          "第 3/4 步：解压 ZIP 到镜像（%lld MB）…",
                          (long long)(zsize / (1024 * 1024)));
  int n = savemgr_extract_zip_path(mp, zip_path);
  savemgr_import_log("✓ 解压完成：%d 个文件", n);
  /* build/repair param.sfo */
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  sfo_info_t info;
  if (savemgr_parse_sfo(sfo_path, &info) != 0) {
    memset(&info, 0, sizeof(info));
    strncpy(info.title_id, tid, sizeof(info.title_id) - 1);
    if (dir && dir[0]) strncpy(info.dir_name, dir, sizeof(info.dir_name) - 1);
    if (mt && mt[0]) strncpy(info.main_title, mt, sizeof(info.main_title) - 1);
    snprintf(info.format, sizeof(info.format), "%s", is_ps4 ? "obs" : "ppr");
  }
  savemgr_import_log("第 4/4 步：重新签名到本机账号并注册系统存档库…");
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int fuid = savemgr_foreground_uid();
  savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid, &info);
  sfo_info_t fin;
  memset(&fin, 0, sizeof(fin));
  strncpy(fin.title_id, tid, sizeof(fin.title_id) - 1);
  if (dir && dir[0]) strncpy(fin.dir_name, dir, sizeof(fin.dir_name) - 1);
  else strncpy(fin.dir_name, "savedata0", sizeof(fin.dir_name) - 1);
  if (mt && mt[0]) strncpy(fin.main_title, mt, sizeof(fin.main_title) - 1);
  snprintf(fin.format, sizeof(fin.format), "%s", is_ps4 ? "obs" : "ppr");
  fin.account_id = aid;
  fin.user_id = (uint32_t)fuid;
  /* icon 必须在卸载**之前**抓：卸载后挂载点就没了，icon 再也读不出来。 */
  savemgr_capture_icon(uid_hex, is_ps4, &fin);
  savemgr_unmount();
  if (savemgr_import_finalize(img, uid_hex, is_ps4, &fin) < 0) {
    unlink(img);
    unlink(imgbin);
    savemgr_import_log("✗ finalize 失败");
    ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "finalize failed");
    goto done;
  }
  unlink(img);
  unlink(imgbin);
  savemgr_import_log("✓ 导入完成：%s（%d 个文件）已注册到账号 %s", tid, n, uid_hex);
  /* 源 ZIP 已经吃干榨净（内容在镜像里、镜像在账号里）—— 只清理本工具暂存根下
     的那一份，用户自己放在别处的包不碰（见 sm_is_staged_src）。 */
  sm_cleanup_staged_src(zip_path);
  char extra[256];
  snprintf(extra, sizeof(extra), "\"files\":%d,\"title_id\":\"%s\"", n, tid);
  ret = json_ok(conn, extra);
done:
  free(uid); free(tid); free(dir); free(mt);
  return ret;
}

/* 兼容入口：整个 ZIP 塞在 HTTP body 里（4 MB 上限）。body 本来就在内存里，
   所以这里把它落到临时文件再交给统一的按路径管线 —— 不再额外多留一份内存副本。 */
enum MHD_Result api_save_mgr_upload(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size) {
  char tmp[MAX_PATH_LEN], tmp_dir[MAX_PATH_LEN];
  int fd;
  size_t off = 0;

  if (body_size == 0)
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");

  sm_map_path("/data/Mango/tmp", tmp_dir, sizeof(tmp_dir));
  recursive_mkdir(tmp_dir);
  snprintf(tmp, sizeof(tmp), "%s/_nex_up_%ld.zip", tmp_dir, (long)getpid());
  fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot stage upload");
  while (off < body_size) {
    ssize_t w = write(fd, body + off, body_size - off);
    if (w <= 0) {
      close(fd); unlink(tmp);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "stage write failed");
    }
    off += (size_t)w;
  }
  close(fd);

  enum MHD_Result ret = savemgr_import_zip_common(conn, url, body, body_size,
                                                  tmp, body_size);
  unlink(tmp);
  return ret;
}

/* Import path 1b: the ZIP was already streamed to the device (via the
 * /api/upload-file channel, same one PKG uploads use), so it can be far
 * bigger than the HTTP body cap. `path` (query param) points at it on disk. */
enum MHD_Result api_save_mgr_import_zip(struct MHD_Connection *conn,
                                        const char *url, const char *body,
                                        size_t body_size) {
  /* path 是堆内存，open 之后即无用处，就地释放（同 api_save_mgr_mount）。 */
  char *path = sm_param(conn, url, body, body_size, "path");
  if (!path || path[0] != '/') {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  }
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "zip not found");
  }
  size_t zsize = (size_t)st.st_size;
  /* ★ 整包留在盘上，直接按路径解包 —— 旧代码 malloc(zsize) 把整个 ZIP 读进
     内存，真机大包就是这么把进程送走的。zsize 只用来给 PFS 镜像定尺寸。 */
  enum MHD_Result ret = savemgr_import_zip_common(conn, url, body, body_size,
                                                  path, zsize);
  free(path);
  return ret;
}

/* Import path 2: upload an encrypted/decrypted image, mount to inspect.
 * Two ways in:
 *   ?path=/data/Mango/...  — image was already streamed to disk via the
 *     /api/upload-file channel (no body cap; the UI always uses this). We
 *     mount it in place; import_finish copies it into the user's home, so
 *     the incoming copy stays until the user deletes it (UI says so).
 *   (legacy) raw image in the HTTP body — buffered, 4 MB cap, kept only for
 *     compatibility with old front ends. */
enum MHD_Result api_save_mgr_import_encrypted(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_size) {
  enum MHD_Result ret;
  /* 三个参数都是堆内存（sm_param）；uid/path 统一在 done: 处释放，ps4s 只
   * 用来推 is_ps4，更早就地 free。出口较多，全走 goto done 以免漏掉某条。 */
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char *ps4s = sm_param(conn, url, body, body_size, "ps4");
  char *path = sm_param(conn, url, body, body_size, "path");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  free(ps4s);                    /* 只用来推 is_ps4 */
  char img[MAX_PATH_LEN];
  char img_from_body = 0;

  if (path && path[0]) {
    /* Only images under the tool's own data area are acceptable — anything
     * else is a path traversal attempt. The check is on the LOGICAL path;
     * the stat/mount use the sm_map_path'ed one (WFM_DATA_ROOT redirect on
     * the host). The front end always streams via /api/upload-file into
     * /data/Mango. */
    if (strncmp(path, "/data/Mango/", 12) != 0) {
      ret = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path not allowed");
      goto done;
    }
    char mapped[MAX_PATH_LEN];
    sm_map_path(path, mapped, sizeof(mapped));
    struct stat st;
    if (stat(mapped, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
      ret = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "image not found");
      goto done;
    }
    snprintf(img, sizeof(img), "%s", mapped);
  } else {
    if (body_size == 0) {
      ret = send_json_error(conn, MHD_HTTP_BAD_REQUEST, "empty body");
      goto done;
    }
    img_from_body = 1;
  }
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);

  if (img_from_body) {
    recursive_mkdir("/data/Mango/tmp");
    snprintf(img, sizeof(img), "/data/Mango/tmp/_nex_imp_%s", is_ps4 ? "ps4" : "ps5");
    int fd = open(img, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0) {
      ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                            "tmp create failed");
      goto done;
    }
    {
      size_t off = 0; ssize_t w;
      while (off < body_size &&
             (w = write(fd, body + off, body_size - off)) > 0)
        off += (size_t)w;
    }
    close(fd);
  }

  char mp[512];
  /* 同上：残留挂载会让重复 mount 在内核里卡死（见 import_zip_common）。 */
  /* 这一步要挂内核镜像、可能几十秒 —— 也让前端有话说，别只留一句「请勿关页」。 */
  savemgr_import_progress(SM_IMP_PROBE, 0, 0,
                          "挂载镜像并读取存档信息…（大镜像需要几十秒）");
  /* 2026-10-11（方案 B）：不再"先卸一次" —— savemgr_mount_image 内部保证
     上一次已卸干净，卸不干净直接回 -104，不会叠第二次挂载。 */
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    savemgr_import_log("✗ 挂载失败（镜像损坏 / 密钥不对 / 内核挂载超时）");
    if (img_from_body) unlink(img);   /* path 模式：incoming 里的镜像留给用户重试 */
    ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
    goto done;
  }
  savemgr_import_log("✓ 已挂载，正在读 param.sfo…");
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  savemgr_cache_params_hmac(sfo_path);
  sfo_info_t sfo;
  int pr = savemgr_parse_sfo(sfo_path, &sfo);
  if (pr == -2) {
    ret = send_buffer(conn, MHD_HTTP_OK,
      strdup("{\"ok\":true,\"sfo_zeroed\":true,\"match\":true,\"exists\":false}"),
      "application/json");
    goto done;
  }
  if (pr < 0) {
    savemgr_unmount();
    if (img_from_body) unlink(img);   /* path 模式：镜像留在 incoming，可换账号重验 */
    ret = send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "parse sfo failed");
    goto done;
  }
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int match = (aid != 0 && sfo.account_id == aid) ? 1 : 0;
  char sd[MAX_PATH_LEN];
  snprintf(sd, sizeof(sd), "/user/home/%s/%s/%s/sdimg_%s", uid_hex,
           is_ps4 ? "savedata" : "savedata_prospero", sfo.title_id, sfo.dir_name);
  struct stat ex; int exists = (stat(sd, &ex) == 0) ? 1 : 0;
  char out[512];
  snprintf(out, sizeof(out),
    "{\"ok\":true,\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"%s\","
    "\"match\":%s,\"exists\":%s}",
    sfo.title_id, sfo.dir_name, sfo.main_title, match ? "true" : "false",
    exists ? "true" : "false");
  ret = send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
done:
  free(uid); free(path);
  return ret;
}

enum MHD_Result api_save_mgr_import_finish(struct MHD_Connection *conn, const char *url,
                                           const char *body, size_t body_size) {
  (void)body; (void)body_size;
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);
  free(uid);

  int is_ps4 = savemgr_mounted_is_ps4();
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", savemgr_mount_point());
  sfo_info_t sfo;
  if (savemgr_parse_sfo(sfo_path, &sfo) != 0) {
    savemgr_unmount();
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "parse sfo failed");
  }
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int fuid = savemgr_foreground_uid();
  savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid, &sfo);
  /* 卸载前抓 icon（卸载后挂载点消失，sce_sys/icon0.png 就读不到了）。 */
  savemgr_capture_icon(uid_hex, is_ps4, &sfo);
  savemgr_unmount();

  const char *src_img = savemgr_mounted_image();
  if (savemgr_import_finalize(src_img, uid_hex, is_ps4, &sfo) < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "finalize failed");
  /* 镜像已经复制进用户账号目录，暂存那一份没用了。成功之后才清：失败时它是
     重试与排查的唯一凭据（和 PKG「装完自动清理、失败保留」同一条纪律）。 */
  sm_cleanup_staged_src(src_img);
  return json_ok(conn, NULL);
}

/* ---- 批量归档导入：一个 ZIP 里含多个 sdimg_* 镜像 ------------------------
 * 真机场景（2026-10-10）：用户从主机里整目录打包出来的存档是**一个 ZIP 里三个
 * sdimg_* 加密镜像**（PPSA23226/sdimg_UserSettingSaveGame 等）。原来的
 * 「加密镜像 · 替换」一次只吃一个镜像，用户得先在电脑上手动解压、再点三次。
 *
 * 这里把整包摊到暂存目录，扫出所有 sdimg_*，**逐个**挂载读 param.sfo（同一
 * 时刻只允许一个挂载 —— PS5 的 PFS 挂载是全局互斥的，并行 mount 会在内核里
 * 卡死，见 import_zip_common 的注释），把清单交前端确认；确认后再逐个重新
 * 签名 → finalize → 注册系统存档库。
 *
 * 两段式（scan → finish）而不是一次做完：覆盖不可逆，用户必须在真正落盘之前
 * 看到「哪几个本机已有、会被盖掉」。
 *
 * 宿主上 savemgr_mount_image 是 stub（恒 -1），probed 恒为 false ⇒ 标题号退回
 * 从包内 <tid>/ 这一层目录名推导（PPSA#####/CUSA#####）。这一层在宿主上可测，
 * 也是真机上「镜像内 PARAMS 被清零」时的兜底。 */

#define SM_ARC_MAX 64

typedef struct {
  char name[160];            /* 基名 sdimg_<dir> */
  char stage[MAX_PATH_LEN];  /* 暂存目录里的绝对路径（已 sm_map_path） */
  char dir[128];             /* 去掉 sdimg_ 前缀的目录名 */
  char tid[16];              /* 从包内 <tid>/ 推出来；推不出为空 */
  char sfo_tid[32];          /* 挂载后从 param.sfo 读到的（宿主上为空） */
  char sfo_dir[64];
  char main_title[256];
  int  hasbin;               /* 旁边有 <dir>.bin ⇒ 这是 PS4 存档 */
  int  probed;               /* 真的挂载读到过 sfo */
  int  sfo_zeroed;           /* 镜像内 PARAMS 全零 */
  int  match;                /* 属于本机账号 */
  int  exists;               /* 本机已有同名存档 */
} sm_arc_item_t;

static sm_arc_item_t g_arc_items[SM_ARC_MAX];
static int g_arc_count;
static int g_arc_is_ps4;
static char g_arc_stage[MAX_PATH_LEN];
/* 上传的那份源 ZIP（**逻辑**路径）。它必须留到这次批量导入真的成功为止：
   摊包之后暂存里那份解压结果会被清掉（收尾 / 取消都清），要重新扫一遍就得重新
   解包，而解包的输入只有这个源包 —— 用户刚传上来的是几百 MB，取消一下就要重传
   一遍实在说不过去。所以「成功才清、取消与失败保留」，和 PKG「装完自动清理、
   失败保留」是同一条纪律（见 src/savemgr.c sm_cleanup_staged_src 的注释）。 */
static char g_arc_src[MAX_PATH_LEN];

/* 暂存目录整个删掉：里面是刚上传整包的解压结果（可达几百 MB），不清会在
 * /data/Mango/tmp 里越积越多。 */
static void
sm_arc_rm_rf(const char *path) {
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char child[MAX_PATH_LEN];
      snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
      struct stat st;
      if (!lstat(child, &st) && S_ISDIR(st.st_mode)) sm_arc_rm_rf(child);
      else unlink(child);
    }
    closedir(d);
  }
  rmdir(path);
}

/* "PPSA23226" / "CUSA04874"：四个大写字母 + 五个数字。 */
static int
sm_arc_looks_like_tid(const char *s) {
  int i;
  if (!s || strlen(s) != 9) return 0;
  for (i = 0; i < 4; i++) if (s[i] < 'A' || s[i] > 'Z') return 0;
  for (i = 4; i < 9; i++) if (s[i] < '0' || s[i] > '9') return 0;
  return 1;
}

/* 递归收 sdimg_*（整包布局 <tid>/sdimg_<dir>，但也容忍任意嵌套）。 */
static void
sm_arc_collect(const char *base, const char *rel, int depth) {
  char here[MAX_PATH_LEN];

  if (depth > 6 || g_arc_count >= SM_ARC_MAX) return;
  if (rel[0]) snprintf(here, sizeof(here), "%s/%s", base, rel);
  else snprintf(here, sizeof(here), "%s", base);

  DIR *d = opendir(here);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && g_arc_count < SM_ARC_MAX) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

    char child_rel[MAX_PATH_LEN];
    if (rel[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name);
    else snprintf(child_rel, sizeof(child_rel), "%s", e->d_name);

    char full[MAX_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", base, child_rel);

    struct stat st;
    if (lstat(full, &st)) continue;
    if (S_ISDIR(st.st_mode)) { sm_arc_collect(base, child_rel, depth + 1); continue; }
    if (strncmp(e->d_name, "sdimg_", 6) != 0 || st.st_size <= 0) continue;

    sm_arc_item_t *it = &g_arc_items[g_arc_count];
    memset(it, 0, sizeof(*it));
    snprintf(it->name, sizeof(it->name), "%s", e->d_name);
    snprintf(it->stage, sizeof(it->stage), "%s", full);
    snprintf(it->dir, sizeof(it->dir), "%s", e->d_name + 6);

    /* 上一层目录名就是标题号。 */
    if (rel[0]) {
      const char *sl = strrchr(rel, '/');
      const char *parent = sl ? sl + 1 : rel;
      if (sm_arc_looks_like_tid(parent))
        snprintf(it->tid, sizeof(it->tid), "%s", parent);
    }
    /* 旁边躺着 <dir>.bin ⇒ PS4（PS5 的密钥封在镜像自己里头）。 */
    {
      char bp[MAX_PATH_LEN];
      snprintf(bp, sizeof(bp), "%s", full);
      char *sl = strrchr(bp, '/');
      if (sl) snprintf(sl + 1, sizeof(bp) - (size_t)(sl + 1 - bp), "%s.bin", it->dir);
      struct stat bs;
      if (stat(bp, &bs) == 0 && S_ISREG(bs.st_mode)) {
        it->hasbin = 1;
        if (!g_arc_is_ps4) g_arc_is_ps4 = 1;
      }
    }
    g_arc_count++;
  }
  closedir(d);
}

/* 有界拼接：清单最多 SM_ARC_MAX 行，用 snprintf 直接推进游标的话，一旦算漏
 * 了容量就是越界写（这里宁可截断也不能写坏堆）。 */
static void
sm_arc_cat(char *out, size_t cap, int *pos, const char *text) {
  size_t len = strlen(text);

  if ((size_t)*pos + len + 1 > cap)
    len = (cap > (size_t)*pos + 1) ? cap - (size_t)*pos - 1 : 0;
  memcpy(out + *pos, text, len);
  *pos += (int)len;
  out[*pos] = 0;
}

/* 第 1 段：摊包 + 逐个挂载读信息，只报清单，不动本机存档。 */
enum MHD_Result
api_save_mgr_import_archive(struct MHD_Connection *conn, const char *url,
                            const char *body, size_t body_size) {
  char *path = sm_param(conn, url, body, body_size, "path");
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char *ps4s = sm_param(conn, url, body, body_size, "ps4");
  int is_ps4_req = (ps4s && ps4s[0] == '1');
  char zpath[MAX_PATH_LEN];
  struct stat zst;
  uint8_t magic[4] = {0};
  char uid_hex[16];
  char *out = NULL;
  size_t cap;
  int pos = 0, i, overwrite = 0, fd, n = 0;

  free(ps4s);

  if (!path || path[0] != '/') {
    free(path); free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  }
  /* 只接受工具自己数据区里的文件 —— 其余一律当路径穿越挡掉。校验用**逻辑**
   * 路径，stat/读盘用 sm_map_path 之后的（宿主上重定向到 WFM_DATA_ROOT）。 */
  if (strncmp(path, "/data/Mango/", 12) != 0) {
    free(path); free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path not allowed");
  }
  sm_map_path(path, zpath, sizeof(zpath));
  mango_log("import_archive: enter path=%s", path);
  snprintf(g_arc_src, sizeof(g_arc_src), "%s", path);  /* 成功收尾时才清（见 g_arc_src） */
  free(path);
  if (stat(zpath, &zst) || !S_ISREG(zst.st_mode) || zst.st_size <= 0) {
    free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "zip not found");
  }

  /* 是不是 ZIP 看魔数，不看扩展名。前端也嗅过一遍了，这里是兜底。 */
  fd = open(zpath, O_RDONLY);
  if (fd < 0) {
    free(uid);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "open failed");
  }
  {
    ssize_t got = read(fd, magic, 4);
    close(fd);
    if (got < 4 || memcmp(magic, "PK\x03\x04", 4) != 0) {
      free(uid);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "not a zip");
    }
  }

  {
    size_t zsize = (size_t)zst.st_size;

    /* ★ 这里**不再** malloc(整包) 再读进内存。真机反馈「进程崩了、自启后功能
       都不能用」就出在这一步：几百 MB 的整包一次性进堆，加上下面又写一份临时
       副本，PS5 上直接 OOM / 把 /data 写爆。改成把盘上的路径直接交给解包器。 */
    savemgr_import_progress(SM_IMP_EXTRACT, 0, (long long)zsize,
                            "解包中：整包 %lld MB，正在摊开…",
                            (long long)(zsize / (1024 * 1024)));
    if (g_arc_stage[0]) {           /* 上一次的暂存先清掉，只留一份 */
      sm_arc_rm_rf(g_arc_stage);
      g_arc_stage[0] = 0;
      g_arc_count = 0;
    }
    sm_map_path("/data/Mango/tmp/_nex_arc", g_arc_stage, sizeof(g_arc_stage));
    n = savemgr_extract_zip_path(g_arc_stage, zpath);
    if (n < 0) {
      /* 带上内部返回码与 zipx 的原因：真机排障时「不是 zip / 包损坏 / 磁盘满」
       * 必须分得开。 */
      char why[320];
      snprintf(why, sizeof(why), "extract failed (%d): %s", n,
               savemgr_last_zip_error());
      g_arc_stage[0] = 0;
      savemgr_import_log("✗ 解包失败（不是 ZIP，或包已损坏）：%s", why);
      free(uid);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, why);
    }
    savemgr_import_log("✓ 解包完成：%d 个文件", n);
    mango_log("import_archive: extract done n=%d", n);
  }

  g_arc_count = 0;
  g_arc_is_ps4 = is_ps4_req;
  sm_arc_collect(g_arc_stage, "", 0);
  if (g_arc_count <= 0) {
    sm_arc_rm_rf(g_arc_stage);
    g_arc_stage[0] = 0;
    savemgr_import_log("✗ 包里没有 sdimg_* 存档镜像");
    free(uid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "no save image in zip");
  }

  /* 同 finish：取前台 uid 也要进系统服务，同样看护（见 sm_userinfo_run）。 */
  if (sm_userinfo_run(uid, uid_hex, NULL, NULL) != 0) {
    free(uid);
    mango_log("import_archive: user-info resolution failed (stuck=%d)",
              g_userinfo_stuck);
    savemgr_import_log("✗ 读取用户信息卡住（15 秒无响应）——系统服务疑似楔死，"
                       "请**重启主机**后再试");
    return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, "user info timeout");
  }
  free(uid);

  /* ★ 扫描阶段**不再挂载内核镜像**（2026-10-11 方案 B —— 照搬 Garlic 的口径）。
   *
   * 旧版本在这里为包里每一张存档 mount → 读 param.sfo → unmount：3 张的包就是
   * 3 次内核挂载调用；用户一确认，finish 又来 3 次 ⇒ **一次导入摸内核 6 次**。
   * 而 sceFsMountSaveData / sceFsUmountSaveData 正是唯一会「永不返回」的那一类
   * 调用，调用次数就是风险次数，砍掉一半是最划算的一刀。
   *
   * Garlic 的清单页从不为了"列个表"去挂载：它列目录，真要动的时候才 mount。
   * 这里照同一口径：标题号与目录名**从包内目录结构推**（`<tid>/sdimg_<dir>`，
   * sm_arc_collect 已经在做），"本机是否已有"直接用这个路径 stat。真正要写盘时
   * （finish）才挂载 —— 那时读到的 param.sfo 才是权威。
   *
   * 代价：清单里不再显示 param.sfo 的 main_title。列表 UI 本来也不展示它
   * （只显示 title_id / dir / 是否已存在 / 是否 PS4），用户看不到差别；
   * "其实是覆盖"这一条在 finish 里挂载后仍会补记进日志。 */
  {
    savemgr_import_progress(SM_IMP_PROBE, 0, g_arc_count,
                            "包里 %d 份存档，正在核对本机已有存档…", g_arc_count);
    for (i = 0; i < g_arc_count; i++) {
      sm_arc_item_t *it = &g_arc_items[i];

      /* tid 推不出来（包内缺 `<tid>/` 这一层）就保持 exists=0、按新增处理。
         真到 finish 挂载后会按 param.sfo 的真实 title_id 落盘，并把
         "其实是覆盖已有存档"补记一行 —— 不静默。 */
      if (it->tid[0]) {
        char sd[MAX_PATH_LEN];
        struct stat ex;
        snprintf(sd, sizeof(sd), "/user/home/%s/%s/%s/sdimg_%s", uid_hex,
                 g_arc_is_ps4 ? "savedata" : "savedata_prospero", it->tid, it->dir);
        it->exists = (stat(sd, &ex) == 0) ? 1 : 0;
      }
      savemgr_import_progress(SM_IMP_PROBE, i + 1, g_arc_count,
                              "（%d/%d）已核对 %s", i + 1, g_arc_count, it->name);
    }
  }

  for (i = 0; i < g_arc_count; i++) if (g_arc_items[i].exists) overwrite++;
  savemgr_import_progress(SM_IMP_PROBE, g_arc_count, g_arc_count,
                          "存档信息读取完成：%d 份", g_arc_count);

  cap = 768 + (size_t)g_arc_count * 768;
  out = malloc(cap);
  if (!out) {
    sm_arc_rm_rf(g_arc_stage);
    g_arc_stage[0] = 0;
    g_arc_count = 0;
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  out[0] = 0;
  {
    char head[128];
    snprintf(head, sizeof(head), "{\"ok\":true,\"count\":%d,\"ps4\":%s,\"overwrite\":%d,\"items\":[",
             g_arc_count, g_arc_is_ps4 ? "true" : "false", overwrite);
    sm_arc_cat(out, cap, &pos, head);
  }
  for (i = 0; i < g_arc_count; i++) {
    sm_arc_item_t *it = &g_arc_items[i];
    char row[768];
    snprintf(row, sizeof(row),
      "%s{\"name\":\"%s\",\"dir\":\"%s\",\"title_id\":\"%s\",\"main_title\":\"%s\","
      "\"probed\":%s,\"sfo_zeroed\":%s,\"match\":%s,\"exists\":%s,\"has_bin\":%s}",
      i ? "," : "", it->name, it->dir,
      it->sfo_tid[0] ? it->sfo_tid : it->tid, it->main_title,
      it->probed ? "true" : "false", it->sfo_zeroed ? "true" : "false",
      it->match ? "true" : "false", it->exists ? "true" : "false",
      it->hasbin ? "true" : "false");
    sm_arc_cat(out, cap, &pos, row);
  }
  sm_arc_cat(out, cap, &pos, "]}");

  savemgr_import_log("归档导入：清单就绪 —— %d 个存档，其中 %d 个本机已存在（等待确认）",
                     g_arc_count, overwrite);
  mango_log("import_archive: list ready count=%d overwrite=%d", g_arc_count, overwrite);
  /* ⚠️ 这里**不**清源 ZIP。第一版把它清在这里（"摊包之后就用不到了"），但那时
     用户还没点确认 —— 取消之后暂存里的解压结果会被删掉，要重扫就只能从源包重新
     解包，而源包已经被我们删了 ⇒ 用户得把几百 MB 重传一遍。
     现在改成收尾时按结果决定（见 _finish：全部成功才清），取消则原样保留。 */
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

/* 第 2 段：用户确认后逐张替换。全程一张一张来，张张都走
 * 「挂载 → 重新签名 → 卸载 → finalize → 注册数据库」这条已验证过的老路。 */
enum MHD_Result
api_save_mgr_import_archive_finish(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char uid_hex[16];
  char *out = NULL;
  size_t cap;
  /* wedged: 内核挂载/卸载楔住 ⇒ 整批停在这里，剩余条目记 skipped 而不是
     failed（"没做"和"做失败了"要分得开，用户才知道该不该重试）。
     skipped: 因为 wedged 而没被处理的条目数。 */
  int pos = 0, i, done = 0, failed = 0, skipped = 0, wedged = 0;
  int is_ps4;
  uint64_t aid;
  int fuid;

  /* ★ 台账里程碑（v1.0.17）：窗口「准备中…」= 这条路由已被接管但还没写出
     第一条进度。以前这里一片空白没法排障；现在进入/解析/逐张/收尾全进台账。 */
  mango_log("import_archive_finish: enter count=%d uid=%s", g_arc_count,
            uid ? uid : "(null)");
  if (g_arc_count <= 0) {
    free(uid);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no archive staged");
  }

  /* ★ 用户信息解析走带超时的看护（背景见 sm_userinfo_run 上方的大注释）：
     2026-10-11 真机实证，冻在「准备中…」的卡点就在这一段（此前裸奔）。 */
  savemgr_import_progress(SM_IMP_APPLY, 0, g_arc_count, "正在准备用户信息…");
  {
    int uirc = sm_userinfo_run(uid, uid_hex, &fuid, &aid);
    free(uid);
    uid = NULL;
    if (uirc != 0) {
      mango_log("import_archive_finish: user-info resolution failed rc=%d "
                "(stuck=%d)", uirc, g_userinfo_stuck);
      savemgr_import_log("✗ 读取用户信息卡住（前台用户/注册表 15 秒无响应）——"
                         "系统服务疑似楔死。请**重启主机**后再试；"
                         "把 /data/Mango/logs/mango-crash.log 发我。");
      return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
                             uirc == -2 ? "user info still stuck (reboot console)"
                                        : "user info timeout");
    }
  }
  mango_log("import_archive_finish: uid=%s fuid=%d aid=%llu resolved",
            uid_hex, fuid, (unsigned long long)aid);

  is_ps4 = g_arc_is_ps4;

  cap = 768 + (size_t)g_arc_count * 768;
  out = malloc(cap);
  if (!out)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  out[0] = 0;
  {
    char head[128];
    snprintf(head, sizeof(head), "{\"ok\":true,\"count\":%d,\"results\":[", g_arc_count);
    sm_arc_cat(out, cap, &pos, head);
  }

  savemgr_import_progress(SM_IMP_APPLY, 0, g_arc_count,
                          "开始逐张替换（共 %d 张）…", g_arc_count);
  for (i = 0; i < g_arc_count; i++) {
    sm_arc_item_t *it = &g_arc_items[i];
    char mp[512];
    char err[128] = "";
    char row[768];
    int mrc;

    /* ★ 内核一旦楔住就**停在这里**，剩下的条目不再碰内核（判据见 sm_kern_wedged）。
       旧代码是"这张失败、接着试下一张" —— 而挂住的是整台主机的 PFS 挂载层，
       继续往里塞只会把它一起拖死。停下来的代价只是"这次没做完"，重启回来再来
       一次就行；继续塞的代价是用户得重启主机。 */
    if (wedged) {
      skipped++;
      savemgr_import_log("· %s：跳过（内核挂载已卡住，本次不再继续）", it->name);
      snprintf(row, sizeof(row),
        "%s{\"name\":\"%s\",\"title_id\":\"%s\",\"dir\":\"%s\",\"ok\":false,"
        "\"skipped\":true,\"error\":\"kernel wedged\"}",
        i ? "," : "", it->name, it->tid, it->dir);
      sm_arc_cat(out, cap, &pos, row);
      continue;
    }

    savemgr_import_progress(SM_IMP_APPLY, i, g_arc_count, "（%d/%d）替换 %s …",
                            i + 1, g_arc_count, it->name);
    mango_log("import_archive_finish: [%d/%d] mount %s", i + 1, g_arc_count,
              it->name);

    /* 这里**不再**写"先卸再挂"：savemgr_mount_image 自己保证上一次已经卸干净，
       卸不干净它会回 -104（按楔住处理），而不是硬叠第二次挂载。 */
    mrc = savemgr_mount_image(it->stage, mp, sizeof(mp));
    mango_log("import_archive_finish: [%d/%d] mount rc=%d", i + 1, g_arc_count, mrc);
    if (sm_kern_wedged(mrc)) {
      wedged = 1;
      snprintf(err, sizeof(err), "kernel wedged (%d)", mrc);
    } else if (mrc < 0) {
      snprintf(err, sizeof(err), "mount failed");
    } else {
      char sfo_path[MAX_PATH_LEN];
      sfo_info_t sfo;
      int pr, urc = 0;

      savemgr_import_progress(SM_IMP_APPLY, i, g_arc_count,
                              "（%d/%d）已挂载 %s，读取存档信息…",
                              i + 1, g_arc_count, it->name);
      snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
      savemgr_cache_params_hmac(sfo_path);
      pr = savemgr_parse_sfo(sfo_path, &sfo);
      if (pr != 0) {
        /* 镜像内 PARAMS 全零（-2）或读不动：用包里的目录名兜底。**但标题号
         * 必须推得出来** —— 推不出就宁可失败，也不能把存档写进别的游戏。 */
        memset(&sfo, 0, sizeof(sfo));
        snprintf(sfo.title_id, sizeof(sfo.title_id), "%s", it->tid);
        snprintf(sfo.dir_name, sizeof(sfo.dir_name), "%s", it->dir);
        snprintf(sfo.main_title, sizeof(sfo.main_title), "%s", it->dir);
        snprintf(sfo.format, sizeof(sfo.format), "%s", is_ps4 ? "obs" : "ppr");
      }
      if (!sfo.title_id[0])
        snprintf(sfo.title_id, sizeof(sfo.title_id), "%s", it->tid);
      if (!sfo.dir_name[0])
        snprintf(sfo.dir_name, sizeof(sfo.dir_name), "%s", it->dir);

      mango_log("import_archive_finish: [%d/%d] sfo pr=%d tid=%s dir=%s",
                i + 1, g_arc_count, pr, sfo.title_id, sfo.dir_name);

      if (!sfo.title_id[0] || !sfo.dir_name[0]) {
        snprintf(err, sizeof(err), "no title/dir (param zeroed)");
        urc = savemgr_unmount();
        /* 卸载也楔住了 ⇒ 后面的条目同样不能再碰内核。 */
        if (sm_kern_wedged(urc)) wedged = 1;
      } else {
        /* 权威值回填：响应与日志都用挂载后读到的，而不是包内目录名。 */
        snprintf(it->sfo_tid, sizeof(it->sfo_tid), "%s", sfo.title_id);
        snprintf(it->sfo_dir, sizeof(it->sfo_dir), "%s", sfo.dir_name);

        /* 扫描阶段不再挂载 ⇒ "其实是覆盖"只有到这里才确定。补记一行，
           不让覆盖静默发生（覆盖是不可逆的）。 */
        {
          char sd[MAX_PATH_LEN];
          struct stat ex;
          snprintf(sd, sizeof(sd), "/user/home/%s/%s/%s/sdimg_%s", uid_hex,
                   is_ps4 ? "savedata" : "savedata_prospero",
                   sfo.title_id, sfo.dir_name);
          if (stat(sd, &ex) == 0)
            savemgr_import_log("⚠ %s/%s 本机已有，正在覆盖", sfo.title_id, sfo.dir_name);
        }

        sfo.account_id = aid;
        sfo.user_id = (uint32_t)fuid;
        savemgr_import_log("替换 %s/%s（%s）…", sfo.title_id, sfo.dir_name, it->name);
        savemgr_import_progress(SM_IMP_APPLY, i, g_arc_count,
                                "（%d/%d）签名并写回 %s …",
                                i + 1, g_arc_count, it->name);
        savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid, &sfo);
        /* 卸载前抓 icon（同上：icon 只存在于挂载点里）。 */
        savemgr_capture_icon(uid_hex, is_ps4, &sfo);
        urc = savemgr_unmount();
        mango_log("import_archive_finish: [%d/%d] unmount rc=%d",
                  i + 1, g_arc_count, urc);
        if (sm_kern_wedged(urc)) {
          wedged = 1;
          snprintf(err, sizeof(err), "kernel wedged (%d)", urc);
        } else {
          int frc = savemgr_import_finalize(it->stage, uid_hex, is_ps4, &sfo);
          mango_log("import_archive_finish: [%d/%d] finalize rc=%d",
                    i + 1, g_arc_count, frc);
          if (frc < 0) snprintf(err, sizeof(err), "finalize failed");
        }
      }
    }

    mango_log("import_archive_finish: [%d/%d] %s => %s", i + 1, g_arc_count,
              it->name, err[0] ? err : "ok");
    if (!err[0]) {
      done++;
      unlink(it->stage);      /* 已落盘，暂存的这一张可以删了 */
      savemgr_import_log("✓ %s → %s/%s", it->name,
                         it->sfo_tid[0] ? it->sfo_tid : it->tid,
                         it->sfo_dir[0] ? it->sfo_dir : it->dir);
    } else {
      failed++;
      savemgr_import_log("✗ %s：%s", it->name, err);
    }

    /* 每张之间留一点喘息：连续 mount/unmount 冲太快正是 PFS 层最容易出问题的
       地方（Garlic 的 unmount_save 也是先 sync() 再收工）。 */
    if (!wedged && i + 1 < g_arc_count) usleep(300 * 1000);

    snprintf(row, sizeof(row),
      "%s{\"name\":\"%s\",\"title_id\":\"%s\",\"dir\":\"%s\",\"ok\":%s,\"error\":\"%s\"}",
      i ? "," : "", it->name,
      it->sfo_tid[0] ? it->sfo_tid : it->tid,
      it->sfo_dir[0] ? it->sfo_dir : it->dir,
      err[0] ? "false" : "true", err);
    sm_arc_cat(out, cap, &pos, row);
  }
  {
    char tail[160];
    snprintf(tail, sizeof(tail),
             "],\"done\":%d,\"failed\":%d,\"skipped\":%d,\"wedged\":%s}",
             done, failed, skipped, wedged ? "true" : "false");
    sm_arc_cat(out, cap, &pos, tail);
  }

  /* 收尾：整包暂存删掉（几百 MB），状态清空 —— 下一次导入从零开始。
     ⚠️ 但**内核楔住时不收尾**：那时的处置权在用户手上（多半要重启主机），
     暂存与源包都留着，重试时不用重传几百 MB，也能拿同一批条目再试一次。 */
  if (wedged) {
    savemgr_import_log("⚠ 内核挂载卡住，本次导入已停止：成功 %d、失败 %d、跳过 %d。"
                       "请**重启主机**后再来一次（源包与暂存都留着，不用重传）。",
                       done, failed, skipped);
  } else {
    /* ★ 源 ZIP 的清理放在**这里**、而且只在整批都成功时做（2026-10-11 用户要求
       「上传了一些临时文件如果不用了可以自动删除掉」）：
         · 全部成功 ⇒ 这次导入已经彻底结束，用户不必再去文件页手动删源包；
         · 有失败 ⇒ 保留源包：暂存里的解压结果已经删了，重试的唯一输入就是它
           （宿主上永远走这条分支 —— 没有 PS5 内核，每张都 mount failed）。
       判据用 failed==0 而不是"跑完了"，和 PKG「装完自动清理、失败保留」同一个口径。 */
    if (g_arc_stage[0]) {
      sm_arc_rm_rf(g_arc_stage);
      g_arc_stage[0] = 0;
    }
    g_arc_count = 0;
    if (failed == 0 && done > 0 && g_arc_src[0]) {
      sm_cleanup_staged_src(g_arc_src);
      g_arc_src[0] = 0;
    } else if (g_arc_src[0]) {
      savemgr_import_log("· 源 ZIP 已保留（有失败项，重试要用）：%s", g_arc_src);
    }
  }
  mango_log("import_archive_finish: finished done=%d failed=%d skipped=%d wedged=%d",
            done, failed, skipped, wedged);
  savemgr_import_progress(SM_IMP_APPLY, done + failed, done + failed,
                          "归档导入结束：成功 %d 个，失败 %d 个%s",
                          done, failed, wedged ? "（内核卡住，已停止）" : "");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

/* 放弃这次批量导入：卸载可能残留的挂载 + 删暂存。
 * ⚠️ 源 ZIP **不删**：取消之后用户多半是去改个标题号再扫一遍，而暂存里的解压结果
 *    已经清掉了 —— 重新解包的输入只有源包。留着它，用户不用为重扫再传一次几百 MB。
 *    真正的清理交给下一次成功的收尾（_finish），或用户在文件页自己删。 */
enum MHD_Result
api_save_mgr_import_archive_cancel(struct MHD_Connection *conn, const char *url,
                                   const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  if (savemgr_is_mounted()) savemgr_unmount();
  if (g_arc_stage[0]) {
    sm_arc_rm_rf(g_arc_stage);
    g_arc_stage[0] = 0;
  }
  g_arc_count = 0;
  savemgr_import_log("归档导入：已取消，暂存已清理");
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_create_pfs(struct MHD_Connection *conn, const char *url,
                                        const char *body, size_t body_size) {
  (void)body; (void)body_size;
  char *tid = sm_param(conn, url, body, body_size, "title_id");
  if (!tid || !tid[0]) { free(tid); return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "title_id required"); }
  char *uid = sm_param(conn, url, body, body_size, "uid");
  char uid_hex[16];
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(uid_hex, sizeof(uid_hex), "%x", (unsigned)f);
  } else snprintf(uid_hex, sizeof(uid_hex), "%s", uid);
  free(uid);
  (void)uid_hex;

  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/data/Mango/tmp/_nex_new_%s", tid);
  free(tid);
  uint8_t key[0x20];
  /* PS5 flavor: this endpoint mounts a scratch PFS for direct file editing;
   * the PS4 import path lives in import_zip_common. */
  int cr = savemgr_create_pfs(img, 1024 * 1024, key, 0);
  if (cr < 0) {
    char why[96];
    snprintf(why, sizeof(why), "create pfs failed (0x%X)", (unsigned)cr);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, why);
  }
  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  return json_ok(conn, NULL);
}

enum MHD_Result api_save_mgr_export(struct MHD_Connection *conn, const char *url,
                                    const char *body, size_t body_size) {
  (void)body; (void)body_size;
  /* Export the currently mounted save as a zip download (device-only). */
  if (!savemgr_is_mounted())
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no save mounted");
  const char *p = url_param(url, "path");
  (void)p;
  return send_json_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
    "export not yet wired (mount + read individual files via /api/save_mgr/read)");
}

/* ============================================= archive port (download/backup)
 * Streaming ZIP export of save images. The device-only steps (mount / PFS
 * create) stay inside #ifndef __linux__; the host build returns the error
 * codes the front end must handle. The ZIP writer itself (src/zip_write.c) is
 * pure lib and is exercised directly by hosttest. */

#include "zip_write.h"

/* Working dir for temp zips / images under /data/Mango/tmp (host: mapped via
 * WFM_DATA_ROOT by sm_map_path below). */
static char g_tmpdir[MAX_PATH_LEN];

/* State kept between encrypt_upload -> encrypt_download and
 * resign_upload -> resign_download: the temp image path + flavor. Cleared when
 * the download stream finishes. */
static char g_enc_img[MAX_PATH_LEN];
static int g_enc_is_ps4;
static char g_resign_img[MAX_PATH_LEN];
static int g_resign_is_ps4;

/* ---- host path mapping ---------------------------------------------------
 * The console uses fixed mount paths (/user/home, /data, /mnt/usbX). Host
 * tests redirect them with the same WFM_* env vars mango_port.c uses, so the
 * backup endpoint can be verified without a device. */
static void sm_map_path(const char *in, char *out, size_t outsz) {
#ifdef __linux__
  const char *sr = getenv("WFM_SAVE_ROOT");
  if (sr && *sr && strncmp(in, "/user/home/", 11) == 0) {
    snprintf(out, outsz, "%s/%s", sr, in + 11);
    return;
  }
  const char *dr = getenv("WFM_DATA_ROOT");
  if (dr && *dr && strncmp(in, "/data/", 6) == 0) {
    snprintf(out, outsz, "%s/%s", dr, in + 6);
    return;
  }
  /* /system_data 下那个 savedata.db 是系统存档服务也在用的库：宿主上建不出
   * 这个目录（非 root），不映射的话存档注册/注销在宿主测试里根本跑不到。 */
  const char *yr = getenv("WFM_SYSTEM_ROOT");
  if (yr && *yr && strncmp(in, "/system_data/", 13) == 0) {
    snprintf(out, outsz, "%s/%s", yr, in + 13);
    return;
  }
  /* /user/appmeta（游戏元数据：param.json / icon0.png）同理：宿主测试用
   * WFM_APPMETA_ROOT 指到夹具目录，名字兜底链（sm_appmeta_title）才可测。 */
  const char *ar = getenv("WFM_APPMETA_ROOT");
  if (ar && *ar && strncmp(in, "/user/appmeta/", 14) == 0) {
    snprintf(out, outsz, "%s/%s", ar, in + 14);
    return;
  }
#endif
  snprintf(out, outsz, "%s", in);
}

/* ---- 备份自述（manifest）与「零挂载还原」 --------------------------------
 * 盘上一份 PS5 存档其实是**两份东西**（教程口径，已与 delete/register 的实现核对）：
 *   /user/home/<uid>/savedata_prospero/<tid>/sdimg_<dir>                镜像
 *   /user/home/<uid>/savedata_prospero_meta/user/<tid>/<dir>_icon0.png   元数据
 * PS4 还有一份封密钥 savedata/<tid>/<dir>.bin。
 *
 * 两件事由此而来：
 *   ① 「备份」必须带走 icon —— 否则 备份 → 删除 → 还原 之后，系统存档列表的
 *      缩略图永远空着（delete 是会删它的，备份却不打包，这一进一出就丢了）。
 *   ② 「还原」只要是**同机同账号**，把这几样复制回原路径就能用 —— 不必解密、
 *      不必改签、尤其不必挂载。省掉的正是 create_pfs → mount → unmount 这
 *      三次内核调用（每一次都可能永不返回）。换机器/换账号才需要走导入重签。 */

static void
sm_json_esc(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  if (!in) { if (outsz) out[0] = 0; return; }
  for (const unsigned char *p = (const unsigned char *)in; *p && j + 7 < outsz; p++) {
    if (*p == '"' || *p == '\\') { out[j++] = '\\'; out[j++] = (char)*p; }
    else if (*p < 0x20)
      j += (size_t)snprintf(out + j, outsz - j, "\\u%04x", (unsigned)*p);
    else out[j++] = (char)*p;
  }
  out[j] = 0;
}

int savemgr_write_backup_manifest2(const char *path, const char *uid_hex,
                                   const char *title_id,
                                   const sm_backup_slot_t *slots, int nslots,
                                   const char *main_title,
                                   uint32_t user_param,
                                   const char *game_title) {
  if (!path || !uid_hex || !title_id || !slots || nslots < 1) return -1;
  if (!sm_safe_name(title_id)) return -1;
  char mt[512], ts[32];
  char gt[512];
  sm_json_esc(main_title && main_title[0] ? main_title : title_id,
              mt, sizeof(mt));
  sm_json_esc(game_title && game_title[0] ? game_title
              : (main_title && main_title[0] ? main_title : title_id),
              gt, sizeof(gt));
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  /* image 条目恒为 <tid>/sdimg_<dir>（与既有备份、以及整包导入的扫描口径一致），
     所以不写进 manifest —— 写死一条规则，免得两边各拼各的、哪天对不上。
     ★ 顶层那四个单槽位字段**刻意保留**（取 slots[0]）：老版本的还原逻辑只认
     它们，留着才能让 v1.0.24/25 打的包和这一版的整体包走同一条还原路径。 */
  int n = fprintf(f,
    "{\"format\":\"mango-backup-2\",\"created\":\"%s\",\"uid\":\"%s\","
    "\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"%s\","
    "\"game_title\":\"%s\",\"is_ps4\":%d,\"blocks\":%u,"
    "\"user_param\":%u,\"icon\":\"%s\",\"slots\":[",
    ts, uid_hex, title_id, slots[0].dir_name, mt, gt,
    slots[0].is_ps4 ? 1 : 0, (unsigned)slots[0].blocks,
    (unsigned)user_param, slots[0].icon);
  for (int i = 0; i < nslots && n > 0; i++) {
    char d[128], ic[512];
    sm_json_esc(slots[i].dir_name, d, sizeof(d));
    sm_json_esc(slots[i].icon, ic, sizeof(ic));
    n += fprintf(f, "%s{\"dir\":\"%s\",\"is_ps4\":%d,\"blocks\":%u,"
                    "\"icon\":\"%s\"}",
                 i ? "," : "", d, slots[i].is_ps4 ? 1 : 0,
                 (unsigned)slots[i].blocks, ic);
  }
  if (n > 0) n += fprintf(f, "]}\n");
  fclose(f);
  return n > 0 ? 0 : -1;
}

int savemgr_write_backup_manifest(const char *path, const char *uid_hex,
                                  const char *title_id, const char *dir_name,
                                  const char *main_title, int is_ps4,
                                  uint32_t blocks, uint32_t user_param,
                                  const char *icon_entry,
                                  const char *game_title) {
  if (!path || !uid_hex || !title_id || !dir_name) return -1;
  sm_backup_slot_t s;
  memset(&s, 0, sizeof(s));
  snprintf(s.dir_name, sizeof(s.dir_name), "%s", dir_name);
  if (icon_entry) snprintf(s.icon, sizeof(s.icon), "%s", icon_entry);
  s.is_ps4 = is_ps4 ? 1 : 0;
  s.blocks = blocks;
  return savemgr_write_backup_manifest2(path, uid_hex, title_id, &s, 1,
                                        main_title, user_param, game_title);
}

/* blocks / main_title 优先取自系统存档库：它们要写进还原后的 savedata 记录，
 * 猜错了系统列表里的大小就显示成 0。库不存在或表里没这一行 ⇒ 交给调用方回落。 */
static void
sm_db_savedata_info(const char *uid_hex, int is_ps4, const char *tid,
                    const char *dir, uint32_t *blocks, char *mt, size_t mtsz) {
  char raw[MAX_PATH_LEN], db_path[MAX_PATH_LEN];
  snprintf(raw, sizeof(raw), "/system_data/%s/%s/db/user/savedata.db",
           is_ps4 ? "savedata" : "savedata_prospero", uid_hex);
  sm_map_path(raw, db_path, sizeof(db_path));
  struct stat ds;
  if (stat(db_path, &ds) != 0) return;   /* 库都没有：别为了查一行去建个空库 */
  sqlite3 *db = NULL;
  if (sqlite3_open(db_path, &db) != SQLITE_OK) return;
  sqlite3_busy_timeout(db, 5000);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db,
        "SELECT blocks, main_title FROM savedata WHERE title_id=? AND dir_name=?",
        -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, dir, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
      if (sqlite3_column_type(st, 0) == SQLITE_INTEGER)
        *blocks = (uint32_t)sqlite3_column_int(st, 0);
      const unsigned char *s = sqlite3_column_text(st, 1);
      if (s) snprintf(mt, mtsz, "%s", (const char *)s);
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
}

/* ---- icon 抓取（必须在 unmount 之前） ---------------------------------- */

int savemgr_capture_icon(const char *uid_hex, int is_ps4,
                         const sfo_info_t *info) {
  if (is_ps4) return -1;            /* PS4 没有 savedata_prospero_meta 这一层 */
  if (!info || !info->title_id[0] || !info->dir_name[0]) return -1;
  if (!sm_safe_name(info->title_id) || !sm_safe_name(info->dir_name)) return -1;
  if (!savemgr_is_mounted()) return -1;
  const char *mp = savemgr_mount_point();
  if (!mp || !mp[0]) return -1;
  char src[MAX_PATH_LEN];
  snprintf(src, sizeof(src), "%s/sce_sys/icon0.png", mp);
  struct stat st;
  if (stat(src, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    mango_log("capture_icon: skip (no %s)", src);
    return -1;
  }
  char cand[MAX_PATH_LEN], icdir[MAX_PATH_LEN], dst[MAX_PATH_LEN];
  snprintf(cand, sizeof(cand), "/user/home/%s/savedata_prospero_meta/user/%s",
           uid_hex, info->title_id);
  sm_map_path(cand, icdir, sizeof(icdir));
  recursive_mkdir(icdir);
  snprintf(dst, sizeof(dst), "%s/%s_icon0.png", icdir, info->dir_name);
  mango_log("capture_icon: %s -> %s", src, dst);
  return copy_file(src, dst);
}

/* ---- 极小的 JSON 取值（只服务我们自己写出来的 manifest） ---------------- */

static int
sm_json_field(const char *j, const char *key, const char **val, size_t *len) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return -1;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != ':') return -1;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p == '"') {
    const char *s = ++p;
    while (*p && *p != '"') { if (*p == '\\') p++; p++; }
    *val = s;
    *len = (size_t)(p - s);
    return 1;                        /* 1 = 字符串 */
  }
  const char *s = p;
  while (*p && strchr("0123456789-+.", *p)) p++;
  *val = s;
  *len = (size_t)(p - s);
  return 0;                          /* 0 = 数字 */
}

static void
sm_json_str(const char *j, const char *key, char *out, size_t outsz) {
  const char *v;
  size_t n;
  out[0] = 0;
  if (sm_json_field(j, key, &v, &n) != 1 || n == 0) return;
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < outsz; i++) {
    if (v[i] == '\\' && i + 1 < n) {
      i++;
      char c = v[i];
      if (c == 'n') c = '\n';
      else if (c == 't') c = '\t';
      out[o++] = c;
    } else out[o++] = v[i];
  }
  out[o] = 0;
}

static long
sm_json_num(const char *j, const char *key, long def) {
  const char *v;
  size_t n;
  if (sm_json_field(j, key, &v, &n) != 0 || n == 0) return def;
  char tmp[32];
  if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  memcpy(tmp, v, n);
  tmp[n] = 0;
  return strtol(tmp, NULL, 10);
}

static long
sm_read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, cap > 0 ? cap - 1 : 0);
  close(fd);
  if (n < 0) return -1;
  buf[n] = 0;
  return (long)n;
}

/* ---- 从备份还原（全程不挂载） ------------------------------------------ */

/* 解析备份自述里的 slots[] —— 整体备份时一个包里有多个槽位。
 * 返回槽位数；**0 = 旧版单槽位包**（没有 slots[] 这一段），调用方据此回落到
 * 顶层那几个字段 —— v1.0.24/25 打的包必须还能还原，兼容性就系在这个返回值上。
 * 每个槽位的 dir 都要过 sm_safe_name：它要进落盘路径，而包可能是别人给的。
 * 刻意放在公共区：宿主单测可以直接喂 JSON 串驱动它，不必真解一个 zip。 */
int sm_manifest_slots(const char *mj, char dirs[][64], int *ps4,
                      uint32_t *blocks, char icons[][256], int max) {
  if (!mj || !dirs || !ps4 || !blocks || !icons || max <= 0) return 0;
  const char *p = strstr(mj, "\"slots\":[");
  if (!p) return 0;
  p += 9;
  int n = 0;
  char obj[1024];
  while (*p && n < max) {
    if (*p != '{') { p++; continue; }
    const char *e = strchr(p, '}');
    if (!e) break;
    size_t len = (size_t)(e - p + 1);
    if (len >= sizeof(obj)) { p = e + 1; continue; }
    memcpy(obj, p, len);
    obj[len] = 0;
    char d[128];
    d[0] = 0;
    sm_json_str(obj, "dir", d, sizeof(d));
    if (d[0] && sm_safe_name(d)) {
      snprintf(dirs[n], 64, "%s", d);
      ps4[n] = (sm_json_num(obj, "is_ps4", 0) != 0) ? 1 : 0;
      long b = sm_json_num(obj, "blocks", 96);
      blocks[n] = (b > 0 && b < 0x7fffffff) ? (uint32_t)b : 96;
      icons[n][0] = 0;
      sm_json_str(obj, "icon", icons[n], 256);
      n++;
    }
    p = e + 1;
  }
  return n;
}

int savemgr_restore_backup(const char *zip_path, const char *uid_hex) {
  char scratch[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
  struct stat st;

  if (!zip_path || !zip_path[0]) return -1;
  if (!uid_hex || !uid_hex[0] || !sm_safe_name(uid_hex)) return -1;
  sm_map_path(zip_path, mapped, sizeof(mapped));
  if (stat(mapped, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return -2;

  /* ★ 不再把整包摊到 /data/Mango/tmp：直接从 zip 里逐条目流式搬到存档区
     （理由见 sm_zip_open_read 上方那段）。/data 上只多出 zip 自己。 */
  void *zip = NULL, *stm = NULL;
  if (sm_zip_open_read(mapped, &zip, &stm) != 0) {
    mango_log("restore: cannot open %s as a zip", mapped);
    return -3;
  }
  mango_log("restore: streaming from %s", mapped);
  char mj[4096];
  if (sm_zip_read_small(zip, "manifest.json", mj, sizeof(mj)) != 0) {
    mango_log("restore: no manifest.json in %s", mapped);
    sm_zip_close_read(zip, stm);
    return -4;
  }
  char mtid[64], mdir[128], mmt[256], muid[32], mic[256];
  sm_json_str(mj, "title_id", mtid, sizeof(mtid));
  sm_json_str(mj, "dir_name", mdir, sizeof(mdir));
  sm_json_str(mj, "main_title", mmt, sizeof(mmt));
  sm_json_str(mj, "uid", muid, sizeof(muid));
  sm_json_str(mj, "icon", mic, sizeof(mic));
  long blocks = sm_json_num(mj, "blocks", 96);
  long uparam = sm_json_num(mj, "user_param", 0);
  int is_ps4 = (sm_json_num(mj, "is_ps4", 0) != 0);
  if (blocks <= 0) blocks = 96;
  if (!mtid[0] || !mdir[0] || !muid[0]) { sm_zip_close_read(zip, stm); return -4; }
  if (!sm_safe_name(mtid) || !sm_safe_name(mdir) || !sm_safe_name(muid)) {
    sm_zip_close_read(zip, stm);
    return -1;
  }
  /* ★ 同账号才成立：镜像的密钥跟账号绑定，跨账号直拷回去的结果就是游戏里一句
   * 「存档损坏」—— 到那一步再排查，比现在拦住贵得多。 */
  if (strcmp(muid, uid_hex) != 0) {
    mango_log("restore: uid mismatch backup=%s local=%s", muid, uid_hex);
    sm_zip_close_read(zip, stm);
    return -5;
  }

  /* 槽位清单：整体备份的 manifest 里有 slots[]；旧版单槽位包没有 ⇒ 回落到顶层
     那几个字段（v1.0.24/25 打的包必须还能还原）。数组走堆：槽位数不定。 */
  char (*sdirs)[64] = calloc(SM_GAME_SLOTS_MAX, 64);
  int *sps4 = calloc(SM_GAME_SLOTS_MAX, sizeof(int));
  uint32_t *sblks = calloc(SM_GAME_SLOTS_MAX, sizeof(uint32_t));
  char (*sicons)[256] = calloc(SM_GAME_SLOTS_MAX, 256);
  if (!sdirs || !sps4 || !sblks || !sicons) {
    free(sdirs); free(sps4); free(sblks); free(sicons);
    sm_zip_close_read(zip, stm);
    return -1;
  }
  int nslots = sm_manifest_slots(mj, sdirs, sps4, sblks, sicons,
                                 SM_GAME_SLOTS_MAX);
  if (nslots <= 0) {
    nslots = 1;
    snprintf(sdirs[0], 64, "%s", mdir);
    sps4[0] = is_ps4;
    sblks[0] = (uint32_t)blocks;
    snprintf(sicons[0], 256, "%s", mic);
  }
  savemgr_import_log("还原备份 %s（%s）：共 %d 个槽位", mtid, mapped, nslots);
  savemgr_import_progress(SM_IMP_APPLY, 0, nslots,
                          "准备还原 %d 个存档槽位…", nslots);
  int done = 0, rc = 0;

  /* ★ 2026-10-13 真机报障「备份完再点还原，进程就崩了，还杀不掉」的根因：
   * savemgr_account_id_for_user() 真机上要进 RegMgr 系统服务
   * （sceRegMgrGetStr / sceRegMgrGetBin），与 sceFsMount/UmountSaveData 同属
   * 「可能永不返回」的一类。裸调它 = 请求线程直接睡进内核：
   *   - 进程看着像"崩了"，其实**没崩**——是 D-state 楔死，所以 SIGKILL 杀不透；
   *   - 请求线程还攥着 g_savemgr_lock，整条 /api/save_mgr/ 陪葬；
   *   - 杀不透 ⇒ payload 管理器重注入，用户看到的就是"结束掉又自己起来"。
   * 其余入口（import_finish 等）早就换成 sm_userinfo_run() 看护版了，只有还原
   * 这条新加的路漏了。看护版超时就放弃线程并返回 -8，不 park（RegMgr 楔住时
   * 网页服务还能活，处置权交给用户）。
   * 放在循环**外**只取一次：同一个 uid 的 aid 不会在槽位之间变，多槽位逐次进
   * 系统服务纯属把楔死概率乘 n 倍。 */
  char ru_uid[16];
  int ru_fuid = 0;
  uint64_t ru_aid = 0;
  if (sm_userinfo_run(uid_hex, ru_uid, &ru_fuid, &ru_aid) != 0) {
    mango_log("restore: user-info service unavailable (rc from guard)");
    free(sdirs); free(sps4); free(sblks); free(sicons);
    sm_zip_close_read(zip, stm);
    return -8;
  }
  mango_log("restore: userinfo ok uid=%s aid=%llu", ru_uid,
            (unsigned long long)ru_aid);

  for (int i = 0; i < nslots; i++) {
    if (!sm_safe_name(sdirs[i])) { rc = -1; break; }
    savemgr_import_progress(SM_IMP_APPLY, i, nslots,
                            "（%d/%d）还原 %s …", i + 1, nslots, sdirs[i]);
    char sd_dir[MAX_PATH_LEN], sd_dir_m[MAX_PATH_LEN];
    snprintf(sd_dir, sizeof(sd_dir), "/user/home/%s/%s/%s", uid_hex,
             sps4[i] ? "savedata" : "savedata_prospero", mtid);
    sm_map_path(sd_dir, sd_dir_m, sizeof(sd_dir_m));
    recursive_mkdir(sd_dir_m);

    /* 条目名：备份包里是 <tid>/sdimg_<dir>；更早的包是裸 sdimg_<dir>。
       两个都试一遍 —— 旧包必须还能还原。 */
    char entry[MAX_PATH_LEN], sd_dst[MAX_PATH_LEN];
    snprintf(entry, sizeof(entry), "%s/sdimg_%s", mtid, sdirs[i]);
    if (mz_zip_locate_entry(zip, entry, 0) != MZ_OK)
      snprintf(entry, sizeof(entry), "sdimg_%s", sdirs[i]);
    if (mz_zip_locate_entry(zip, entry, 0) != MZ_OK) {
      mango_log("restore: image missing for slot %s in %s", sdirs[i], mapped);
      savemgr_import_log("✗ 槽位 %s 在包里没有镜像条目", sdirs[i]);
      rc = -4;
      break;
    }
    snprintf(sd_dst, sizeof(sd_dst), "%s/sdimg_%s", sd_dir_m, sdirs[i]);

    /* 覆盖前先把现有的挪开：搬运失败能整体回滚，不让「还原失败」变成「存档没了」。 */
    char oldp[MAX_PATH_LEN];
    int had_old = 0;
    if (stat(sd_dst, &st) == 0) {
      snprintf(oldp, sizeof(oldp), "%s.nex-old-%ld", sd_dst, (long)time(NULL));
      if (rename(sd_dst, oldp) == 0) had_old = 1;
    }
    mango_log("restore: stream %s -> %s (old=%d)", entry, sd_dst, had_old);
    if (sm_zip_write_entry(zip, entry, sd_dst) != 0) {
      if (had_old) rename(oldp, sd_dst);
      savemgr_import_log("✗ 复制 %s 失败，已回滚原有存档", sdirs[i]);
      rc = -6;
      break;
    }
    chmod(sd_dst, 0644);
    if (had_old) unlink(oldp);

    if (sps4[i]) {
      char bentry[MAX_PATH_LEN], dbin[MAX_PATH_LEN];
      snprintf(bentry, sizeof(bentry), "%s/%s.bin", mtid, sdirs[i]);
      if (mz_zip_locate_entry(zip, bentry, 0) == MZ_OK) {
        snprintf(dbin, sizeof(dbin), "%s/%s.bin", sd_dir_m, sdirs[i]);
        SM_IGNORE(sm_zip_write_entry(zip, bentry, dbin));
      }
    }
    /* 元数据 icon：备份里有就放回 meta 目录（教程口径：一份存档 = 镜像 + meta）。
     * 没拷成功不算失败 —— 缺 icon 只是列表里没缩略图，存档本身照样能进。 */
    {
      char icentry[MAX_PATH_LEN], icdir[MAX_PATH_LEN], icdst[MAX_PATH_LEN];
      const char *want = NULL;
      if (sicons[i][0] && sicons[i][0] != '/' && !strstr(sicons[i], "..") &&
          mz_zip_locate_entry(zip, sicons[i], 0) == MZ_OK)
        want = sicons[i];
      if (!want) {
        snprintf(icentry, sizeof(icentry), "meta/%s/%s_icon0.png", mtid,
                 sdirs[i]);
        if (mz_zip_locate_entry(zip, icentry, 0) == MZ_OK) want = icentry;
      }
      if (want && !sps4[i]) {
        snprintf(scratch, sizeof(scratch),
                 "/user/home/%s/savedata_prospero_meta/user/%s", uid_hex, mtid);
        sm_map_path(scratch, icdir, sizeof(icdir));
        recursive_mkdir(icdir);
        snprintf(icdst, sizeof(icdst), "%s/%s_icon0.png", icdir, sdirs[i]);
        SM_IGNORE(sm_zip_write_entry(zip, want, icdst));
      }
    }

    sfo_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.title_id, sizeof(info.title_id), "%s", mtid);
    snprintf(info.dir_name, sizeof(info.dir_name), "%s", sdirs[i]);
    snprintf(info.main_title, sizeof(info.main_title), "%s", mmt[0] ? mmt : mtid);
    snprintf(info.format, sizeof(info.format), "%s", sps4[i] ? "obs" : "ppr");
    info.blocks = sblks[i];
    info.user_param = (uint32_t)uparam;
    /* account_id 取循环外那个看护结果 —— 这里**绝不能**再调
       savemgr_account_id_for_user()：见上方 sm_userinfo_run 的注释。 */
    info.account_id = ru_aid;
    info.user_id = (uint32_t)strtoul(uid_hex, NULL, 16);
    int r2 = savemgr_register_db(uid_hex, sps4[i], &info);
    mango_log("restore: register db rc=%d tid=%s dir=%s", r2, mtid, sdirs[i]);
    if (r2 != 0) {
      savemgr_import_log("✗ 注册系统存档库失败：%s/%s", mtid, sdirs[i]);
      rc = -7;
    } else {
      savemgr_import_log("✓ %s/%s 已还原", mtid, sdirs[i]);
    }
    done++;
  }
  savemgr_import_progress(SM_IMP_APPLY, done, nslots,
                          done == nslots ? "还原完成" : "还原中断");
  free(sdirs); free(sps4); free(sblks); free(sicons);
  sm_zip_close_read(zip, stm);
  if (rc < 0) return rc;
  return done > 0 ? 0 : -4;
}

/* ---- streaming file response (callback mode, temp file unlinked on close) */

typedef struct {
  FILE *f;
  char unlink_paths[4][MAX_PATH_LEN];
  int n_unlink;
  int clear_enc;    /* clear the encrypt_upload state when the stream ends */
  int clear_resign; /* clear the resign_upload state when the stream ends */
} sm_stream_ctx_t;

static ssize_t
sm_stream_read(void *cls, uint64_t pos, char *buf, size_t max) {
  sm_stream_ctx_t *c = cls;
  if (fseek(c->f, (long)pos, SEEK_SET)) return MHD_CONTENT_READER_END_WITH_ERROR;
  size_t n = fread(buf, 1, max, c->f);
  if (!n) return ferror(c->f) ? MHD_CONTENT_READER_END_WITH_ERROR :
                                MHD_CONTENT_READER_END_OF_STREAM;
  return (ssize_t)n;
}

static void
sm_stream_close(void *cls) {
  sm_stream_ctx_t *c = cls;
  if (c->f) fclose(c->f);
  for (int i = 0; i < c->n_unlink; i++)
    if (c->unlink_paths[i][0]) unlink(c->unlink_paths[i]);
  if (c->clear_enc) g_enc_img[0] = 0;
  if (c->clear_resign) g_resign_img[0] = 0;
  free(c);
}

/* Serve `path` (opened streaming; unlinked after the response completes).
 * `unlink_extra` may add one more path (the temp zip) to remove too. */
static enum MHD_Result
sm_stream_file(struct MHD_Connection *conn, const char *path,
               const char *dispo, int clear_enc, int clear_resign,
               const char *unlink_extra) {
  FILE *f = fopen(path, "rb");
  if (!f) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 "open temp failed");
  struct stat st;
  if (fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode)) {
    fclose(f);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "stat failed");
  }
  sm_stream_ctx_t *c = calloc(1, sizeof(*c));
  if (!c) { fclose(f); return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory"); }
  c->f = f;
  c->clear_enc = clear_enc;
  c->clear_resign = clear_resign;
  if (unlink_extra && unlink_extra[0])
    snprintf(c->unlink_paths[c->n_unlink++], MAX_PATH_LEN, "%s", unlink_extra);
  struct MHD_Response *resp =
    MHD_create_response_from_callback((uint64_t)st.st_size, 32 * 0x4000,
                                      sm_stream_read, c, sm_stream_close);
  if (!resp) {
    fclose(f);
    free(c);
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/octet-stream");
  if (dispo && dispo[0])
    MHD_add_response_header(resp, "Content-Disposition", dispo);
  enum MHD_Result r = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return r;
}

/* ---- uid resolution helper (uid ?: foreground user as %x) ---------------- */

static void
sm_uid_hex(const char *uid, char *out, size_t outsz) {
  if (!uid || !uid[0]) {
    int f = savemgr_foreground_uid();
    snprintf(out, outsz, "%x", (unsigned)f);
  } else {
    snprintf(out, outsz, "%s", uid);
  }
}

/* ---- file sink for zipw -------------------------------------------------- */

typedef struct {
  int fd;
} sm_file_sink_t;

static int
sm_file_sink_write(void *ctx, const void *buf, size_t len) {
  sm_file_sink_t *s = ctx;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(s->fd, (const char *)buf + off, len - off);
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

/* ---- /api/save_mgr/download ----------------------------------------------
 * Zip the mounted save (or the image at /user/home/<uid>/savedata_prospero/
 * <tid>/sdimg_<dir>, mounted on demand) into a temp file, then stream it back
 * with a callback response. The temp zip is unlinked when the response ends. */
enum MHD_Result api_save_mgr_download(struct MHD_Connection *conn,
                                      const char *url, const char *body,
                                      size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *tid = req_param(conn, body, body_size, "title_id");
  char *dir = req_param(conn, body, body_size, "dir_name");

  if (!savemgr_is_mounted()) {
    if (!tid || !tid[0] || !dir || !dir[0]) {
      free(uid); free(tid); free(dir);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                             "title_id/dir_name required");
    }
    char uid_hex[16];
    sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
    char img[MAX_PATH_LEN];
    snprintf(img, sizeof(img), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
             uid_hex, tid, dir);
    char mp[512];
    if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
      free(uid); free(tid); free(dir);
      return send_json_error(conn, MHD_HTTP_CONFLICT, "mount failed");
    }
  }
  free(uid); free(tid); free(dir);

  sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
  recursive_mkdir(g_tmpdir);
  char zpath[MAX_PATH_LEN];
  snprintf(zpath, sizeof(zpath), "%s/_nex_dl_%ld.zip", g_tmpdir, (long)getpid());
  sm_file_sink_t sink = {0};
  sink.fd = open(zpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (sink.fd < 0)
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "temp create failed");
  zipw_sink_t zs;
  zs.write = sm_file_sink_write;
  zs.ctx = &sink;
  zs.bytes = 0;
  char *iobuf = malloc(256 * 1024);
  int wr = iobuf ? zipw_write_tree(&zs, savemgr_mount_point(), iobuf,
                                   256 * 1024) : -1;
  free(iobuf);
  close(sink.fd);
  savemgr_unmount();
  if (wr < 0) {
    unlink(zpath);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "zip failed");
  }
  char dispo[128];
  snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s.zip\"", "save");
  return sm_stream_file(conn, zpath, dispo, 0, 0, NULL);
}

/* ---- encrypt_upload / encrypt_download ----------------------------------- */

enum MHD_Result api_save_mgr_encrypt_upload(struct MHD_Connection *conn,
                                            const char *url, const char *body,
                                            size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *tid = req_param(conn, body, body_size, "title_id");
  char *dir = req_param(conn, body, body_size, "dir_name");
  char *mt = req_param(conn, body, body_size, "main_title");
  char *ps4s = req_param(conn, body, body_size, "ps4");
  char *path = req_param(conn, body, body_size, "path");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  if (!tid || !tid[0] || !path || path[0] != '/') {
    free(uid); free(tid); free(dir); free(mt); free(ps4s); free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "title_id/path required");
  }
  savemgr_import_log("加密导入：读取明文 ZIP %s …", path);
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    free(uid); free(tid); free(dir); free(mt); free(ps4s); free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "zip not found");
  }
  size_t zsize = (size_t)st.st_size;
  /* ★ 整包不进内存：留一个映射后的源路径就够（真机大包 malloc(zsize) 会送命）。 */
  char zsrc[MAX_PATH_LEN];
  sm_map_path(path, zsrc, sizeof(zsrc));

  sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
  recursive_mkdir(g_tmpdir);
  char img[MAX_PATH_LEN], imgbin[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "%s/_nex_enc_%ld", g_tmpdir, (long)getpid());
  snprintf(imgbin, sizeof(imgbin), "%s.bin", img);
  uint8_t key[0x20];
  savemgr_import_log("第 1/4 步：创建新 PFS 镜像（is_ps4=%d）…", is_ps4);
  int cr = savemgr_create_pfs(img, zsize, key, is_ps4);
  if (cr < 0) {
    char why[96];
    snprintf(why, sizeof(why), "create pfs failed (0x%X)", (unsigned)cr);
    unlink(img);
    unlink(imgbin);
    savemgr_import_log("✗ create pfs 失败：%s", why);
    free(uid); free(tid); free(dir); free(mt); free(ps4s); free(path);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, why);
  }
  savemgr_import_log("✓ 镜像已创建");
  char mp[512];
  /* 2026-10-10：挂新镜像前务必卸掉旧挂载 —— 上次校验后没点 finish（关页/
   * 重试）会留下残留挂载，PS5 的 PFS 挂载是全局互斥的，重复 mount 会在内核
   * 调用里卡死这条线程。
   * 2026-10-11（方案 B）：这活儿现在由 savemgr_mount_image 内部承担，而且它
   * **卸不干净就不挂**（回 -104）；调用方再"先卸一次"只是白白多摸一次内核。 */
  savemgr_import_log("第 2/4 步：挂载镜像…");
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    unlink(imgbin);
    savemgr_import_log("✗ 挂载失败");
    free(uid); free(tid); free(dir); free(mt); free(ps4s); free(path);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  savemgr_import_log("✓ 已挂载");
  savemgr_import_log("第 3/4 步：解压 ZIP 到镜像…");
  savemgr_extract_zip_path(mp, zsrc);
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  sfo_info_t info;
  if (savemgr_parse_sfo(sfo_path, &info) != 0) {
    memset(&info, 0, sizeof(info));
    strncpy(info.title_id, tid, sizeof(info.title_id) - 1);
    if (dir && dir[0]) strncpy(info.dir_name, dir, sizeof(info.dir_name) - 1);
    if (mt && mt[0]) strncpy(info.main_title, mt, sizeof(info.main_title) - 1);
    snprintf(info.format, sizeof(info.format), "%s", is_ps4 ? "obs" : "ppr");
    savemgr_generate_sfo(sfo_path, &info);
  }
  savemgr_import_log("第 4/4 步：完成（不重签，保留原账号绑定）");
  savemgr_unmount();
  /* Keep the image for encrypt_download; do NOT unlink it. */
  snprintf(g_enc_img, sizeof(g_enc_img), "%s", img);
  g_enc_is_ps4 = is_ps4;
  char name[160];
  snprintf(name, sizeof(name), "%s_%s.img", tid,
           (dir && dir[0]) ? dir : "savedata0");
  char out[512];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"download\":\"/api/save_mgr/encrypt_download\","
           "\"name\":\"%s\",\"is_ps4\":%s}",
           name, is_ps4 ? "true" : "false");
  /* 明文 ZIP 已经摊进新镜像（镜像留给 encrypt_download），源包用完即清。 */
  sm_cleanup_staged_src(path);
  free(uid); free(tid); free(dir); free(mt); free(ps4s); free(path);
  return send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
}

enum MHD_Result api_save_mgr_encrypt_download(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_size) {
  (void)url;
  char *name = req_param(conn, body, body_size, "name");
  if (!name || !name[0]) {
    free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "name required");
  }
  if (!g_enc_img[0]) {
    free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no encrypted image ready");
  }
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "%s", g_enc_img);
  int is_ps4 = g_enc_is_ps4;
  char dispo[192];
  if (is_ps4) {
    /* PS4: zip <img> + <img>.bin into one archive and stream it. */
    sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
    recursive_mkdir(g_tmpdir);
    char zpath[MAX_PATH_LEN];
    snprintf(zpath, sizeof(zpath), "%s/_nex_encdl_%ld.zip", g_tmpdir,
             (long)getpid());
    sm_file_sink_t sink = {0};
    sink.fd = open(zpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (sink.fd < 0) {
      free(name);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "temp create failed");
    }
    zipw_file_t files[2];
    files[0].path = img;
    files[0].name = name;
    char binname[192], binpath[MAX_PATH_LEN];
    snprintf(binname, sizeof(binname), "%s.bin", name);
    snprintf(binpath, sizeof(binpath), "%s.bin", img);
    files[1].path = binpath;
    files[1].name = binname;
    zipw_sink_t zs;
    zs.write = sm_file_sink_write;
    zs.ctx = &sink;
    zs.bytes = 0;
    char *iobuf = malloc(256 * 1024);
    int wr = iobuf ? zipw_write_files(&zs, files, 2, iobuf, 256 * 1024) : -1;
    free(iobuf);
    close(sink.fd);
    if (wr < 0) {
      unlink(zpath);
      free(name);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "zip failed");
    }
    snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
    free(name);
    return sm_stream_file(conn, zpath, dispo, 1, 0, img);
  }
  snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
  free(name);
  return sm_stream_file(conn, img, dispo, 1, 0, NULL);
}

/* ---- resign_upload / resign_download ------------------------------------- */

enum MHD_Result api_save_mgr_resign_upload(struct MHD_Connection *conn,
                                           const char *url, const char *body,
                                           size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *ps4s = req_param(conn, body, body_size, "ps4");
  char *path = req_param(conn, body, body_size, "path");
  char *key_path = req_param(conn, body, body_size, "key_path");
  int is_ps4 = (ps4s && ps4s[0] == '1');
  if (!path || path[0] != '/') {
    free(uid); free(ps4s); free(path); free(key_path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  }
  savemgr_import_log("重签：复制加密镜像 %s …", path);
  sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
  recursive_mkdir(g_tmpdir);
  char img[MAX_PATH_LEN], imgbin[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "%s/_nex_resign_%ld.img", g_tmpdir, (long)getpid());
  snprintf(imgbin, sizeof(imgbin), "%s.bin", img);
  if (copy_file(path, img) < 0) {
    free(uid); free(ps4s); free(path); free(key_path);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "copy failed");
  }
  if (is_ps4) {
    const char *kp = (key_path && key_path[0]) ? key_path :
                     "/data/Mango/tmp/_nex_key.bin";
    char km[MAX_PATH_LEN];
    sm_map_path(kp, km, sizeof(km));
    if (copy_file(km, imgbin) < 0) {
      unlink(img);
      free(uid); free(ps4s); free(path); free(key_path);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "key copy failed");
    }
  }
  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    unlink(img);
    unlink(imgbin);
    free(uid); free(ps4s); free(path); free(key_path);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "mount failed");
  }
  char sfo_path[MAX_PATH_LEN];
  snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", mp);
  sfo_info_t sfo;
  int pr = savemgr_parse_sfo(sfo_path, &sfo);
  if (pr < 0) {
    savemgr_unmount();
    unlink(img);
    unlink(imgbin);
    free(uid); free(ps4s); free(path); free(key_path);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "parse sfo failed");
  }
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  uint64_t aid = savemgr_account_id_for_user(uid_hex);
  int fuid = savemgr_foreground_uid();
  savemgr_resign_sfo(sfo_path, is_ps4, aid, (uint32_t)fuid, &sfo);
  savemgr_unmount();
  snprintf(g_resign_img, sizeof(g_resign_img), "%s", img);
  g_resign_is_ps4 = is_ps4;
  const char *bn = strrchr(path, '/');
  bn = bn ? bn + 1 : path;
  char out[512];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"download\":\"/api/save_mgr/resign_download\","
           "\"name\":\"%s\",\"is_ps4\":%s}",
           bn, is_ps4 ? "true" : "false");
  /* 源镜像已经完整复制进重签用的新镜像，上传的那份用完即清。
     ⚠️ key_path 绝不动 —— 那是用户自己选的密钥文件，本来就指望它能复用。 */
  sm_cleanup_staged_src(path);
  free(uid); free(ps4s); free(path); free(key_path);
  return send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
}

enum MHD_Result api_save_mgr_resign_download(struct MHD_Connection *conn,
                                             const char *url, const char *body,
                                             size_t body_size) {
  (void)url;
  char *name = req_param(conn, body, body_size, "name");
  if (!name || !name[0]) {
    free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "name required");
  }
  if (!g_resign_img[0]) {
    free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "no resign image ready");
  }
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "%s", g_resign_img);
  int is_ps4 = g_resign_is_ps4;
  char dispo[192];
  if (is_ps4) {
    sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
    recursive_mkdir(g_tmpdir);
    char zpath[MAX_PATH_LEN];
    snprintf(zpath, sizeof(zpath), "%s/_nex_resigndl_%ld.zip", g_tmpdir,
             (long)getpid());
    sm_file_sink_t sink = {0};
    sink.fd = open(zpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (sink.fd < 0) {
      free(name);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "temp create failed");
    }
    zipw_file_t files[2];
    files[0].path = img;
    files[0].name = name;
    char binname[192], binpath[MAX_PATH_LEN];
    snprintf(binname, sizeof(binname), "%s.bin", name);
    snprintf(binpath, sizeof(binpath), "%s.bin", img);
    files[1].path = binpath;
    files[1].name = binname;
    zipw_sink_t zs;
    zs.write = sm_file_sink_write;
    zs.ctx = &sink;
    zs.bytes = 0;
    char *iobuf = malloc(256 * 1024);
    int wr = iobuf ? zipw_write_files(&zs, files, 2, iobuf, 256 * 1024) : -1;
    free(iobuf);
    close(sink.fd);
    if (wr < 0) {
      unlink(zpath);
      free(name);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "zip failed");
    }
    snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
    free(name);
    return sm_stream_file(conn, zpath, dispo, 0, 1, img);
  }
  snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
  free(name);
  return sm_stream_file(conn, img, dispo, 0, 1, NULL);
}

/* ---- /api/save_mgr/backup ------------------------------------------------
 * Pack a save image (+ PS4 .bin) into a timestamped ZIP under
 *   local: /data/Mango/backups/<tid>/<dir>_<ts>.zip
 *   usb:   /mnt/<usb>/PS5/SAVEDATA/GARLIC/<tid>/<dir>_<ts>.zip
 * Works on the host build too (the source image just has to exist). */
/* ---- delete one save slot ----------------------------------------------
 * ★ 不挂载：挂载是为读写存档**内容**，删整个槽位没有理由打开它。 */

enum MHD_Result api_save_mgr_delete(struct MHD_Connection *conn,
                                    const char *url, const char *body,
                                    size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *tid = req_param(conn, body, body_size, "title_id");
  char *dir = req_param(conn, body, body_size, "dir_name");
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  /* dir_name 可空 —— 空 = 删除这个游戏的**所有**槽位（「一个游戏 = 一个整体」）。
     ⚠️ 整体删除是破坏性操作：前端必须二次确认并列出将被删掉的槽位。 */
  if (!tid || !tid[0] || !uid_hex[0]) {
    free(uid); free(tid); free(dir);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "uid/title_id required");
  }
  mango_log("save_mgr/delete: enter uid=%s tid=%s dir=%s", uid_hex, tid,
            dir ? dir : "(whole game)");
  if (dir && dir[0]) {
    int r = savemgr_delete_save(uid_hex, tid, dir);
    if (r == 0) {
      mango_log("save_mgr/delete: removed uid=%s tid=%s dir=%s", uid_hex, tid,
                dir);
      free(uid); free(tid); free(dir);
      return send_json_ok(conn);
    }
    if (r == -3)
      mango_log("save_mgr/delete: WARN files gone but db row survived "
                "uid=%s tid=%s dir=%s", uid_hex, tid, dir);
    free(uid); free(tid); free(dir);
    if (r == -1) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad name");
    if (r == -2) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "save not found");
    if (r == -4) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                        "unlink failed");
    return send_buffer(conn, MHD_HTTP_OK,
                       strdup("{\"ok\":true,\"db_row_survived\":true}"),
                       "application/json");
  }
  /* 整体删除：逐个槽位删，失败的记数但**不中断**（一个槽位删不掉不代表剩下
     的也不该删）。返回删掉了几个、失败了几个，前端如实告诉用户。 */
  int failed = 0;
  int n = savemgr_delete_game(uid_hex, tid, &failed);
  free(uid); free(tid); free(dir);
  if (n < 0) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad name");
  if (n == 0) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "save not found");
  mango_log("save_mgr/delete: game uid=%s tid=%s deleted=%d failed=%d",
            uid_hex, tid, n, failed);
  char out[192];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"deleted\":%d,\"failed\":%d}", n, failed);
  return send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
}

enum MHD_Result api_save_mgr_backup(struct MHD_Connection *conn,
                                    const char *url, const char *body,
                                    size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *tid = req_param(conn, body, body_size, "title_id");
  char *dir = req_param(conn, body, body_size, "dir_name");
  char *target = req_param(conn, body, body_size, "target");
  char *usb = req_param(conn, body, body_size, "usb");
  /* dir_name **可以为空** —— 空 = 按游戏整体备份（这个 title_id 下所有槽位）。
     「一个游戏 = 一个整体」是用户定的粒度：一个游戏 3 个槽位要备份 3 次是不可接受的。 */
  if (!tid || !tid[0] || !target || !target[0]) {
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "title_id/target required");
  }
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  /* title_id 直接进路径拼接（备份目录名、包内条目名），也进还原时的落盘路径
     —— 与 delete / restore 同一口径，先过一道名字校验。dir_name 的校验在下面
     真正用到它的分支里做（整体模式下它本来就是空的）。 */
  if (!sm_safe_name(tid)) {
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad name");
  }

  /* ★ 「一个游戏 = 一个整体」（用户定的粒度）：dir_name 省略 ⇒ 打包这个
   * title_id 下的**所有**槽位；给了 dir_name 就只打这一个（旧入口仍在）。
   * 槽位数不定 ⇒ 数组一律走堆：请求线程是每连接一条栈，在这里压 64 ×
   * MAX_PATH_LEN 等于拿真机的线程栈赌命（项目纪律：路由里的大数组上台）。 */
  char (*sdirs)[64] = calloc(SM_GAME_SLOTS_MAX, 64);
  int *sps4 = calloc(SM_GAME_SLOTS_MAX, sizeof(int));
  char (*imgs)[MAX_PATH_LEN] = calloc(SM_GAME_SLOTS_MAX, MAX_PATH_LEN);
  if (!sdirs || !sps4 || !imgs) {
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  }
  int nslots = 0;
  if (dir && dir[0]) {
    if (!sm_safe_name(dir)) {
      free(sdirs); free(sps4); free(imgs);
      free(uid); free(tid); free(dir); free(target); free(usb);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad name");
    }
    snprintf(sdirs[0], 64, "%s", dir);
    nslots = 1;
  } else {
    nslots = sm_game_slots(uid_hex, tid, sdirs, SM_GAME_SLOTS_MAX, sps4);
    if (nslots <= 0) {
      free(sdirs); free(sps4); free(imgs);
      free(uid); free(tid); free(dir); free(target); free(usb);
      return send_json_error(conn, MHD_HTTP_NOT_FOUND, "no save for this title");
    }
  }
  /* 逐槽位定位镜像：savedata_prospero（PS5）优先，savedata（PS4）次之。
     目录里可能有 sdimg_ 残骸（文件已被删、目录还在）⇒ 找不到的槽位直接丢掉，
     不让它变成包里一个 0 字节的坏条目。 */
  {
    int n = 0;
    for (int i = 0; i < nslots; i++) {
      char cand[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
      struct stat ist;
      int ps4 = 0;
      snprintf(cand, sizeof(cand), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
               uid_hex, tid, sdirs[i]);
      sm_map_path(cand, mapped, sizeof(mapped));
      if (stat(mapped, &ist) == 0 && S_ISREG(ist.st_mode)) {
        snprintf(imgs[i], MAX_PATH_LEN, "%s", mapped);
      } else {
        snprintf(cand, sizeof(cand), "/user/home/%s/savedata/%s/sdimg_%s",
                 uid_hex, tid, sdirs[i]);
        sm_map_path(cand, mapped, sizeof(mapped));
        if (stat(mapped, &ist) != 0 || !S_ISREG(ist.st_mode)) continue;
        snprintf(imgs[i], MAX_PATH_LEN, "%s", mapped);
        ps4 = 1;
      }
      if (n != i) {
        memcpy(sdirs[n], sdirs[i], 64);
        snprintf(imgs[n], MAX_PATH_LEN, "%s", imgs[i]);
      }
      sps4[n] = ps4;
      n++;
    }
    nslots = n;
  }
  if (nslots <= 0) {
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "image not found");
  }

  /* Timestamp for the file name. */
  char ts[32];
  {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
  }

  char dst_dir[MAX_PATH_LEN], dst_dir_mapped[MAX_PATH_LEN];
  if (strcmp(target, "usb") == 0 && usb && usb[0]) {
    snprintf(dst_dir, sizeof(dst_dir),
             "/mnt/%s/PS5/SAVEDATA/GARLIC/%s", usb, tid);
  } else {
    snprintf(dst_dir, sizeof(dst_dir), "/data/Mango/backups/%s", tid);
  }
  sm_map_path(dst_dir, dst_dir_mapped, sizeof(dst_dir_mapped));
  recursive_mkdir(dst_dir_mapped);
  char dst[MAX_PATH_LEN];
  /* 整体备份时 dir_name 为空 ⇒ 文件名退到 title_id，否则会生成 "_<ts>.zip"
     这种看不出是哪个游戏的包。 */
  snprintf(dst, sizeof(dst), "%s/%s_%s.zip", dst_dir_mapped,
           (dir && dir[0]) ? dir : tid, ts);

  /* 元数据 icon：一份 PS5 存档在盘上是「镜像 + savedata_prospero_meta 里的
   * icon」两份东西（教程口径）。★ 备份必须带走它 —— 删除是会删这一份的
   * （savemgr_delete_save），若备份不打包，「备份 → 删除 → 还原」之后系统
   * 存档列表的缩略图就永久空了。整体备份时**每个槽位各带一份**自己的 icon。 */
  char (*icons)[MAX_PATH_LEN] = calloc((size_t)nslots, MAX_PATH_LEN);
  sm_backup_slot_t *slots = calloc((size_t)nslots, sizeof(sm_backup_slot_t));
  if (!icons || !slots) {
    free(icons); free(slots);
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  }
  char mt[256], gt[256];
  snprintf(mt, sizeof(mt), "%s", tid);
  gt[0] = 0;
  for (int i = 0; i < nslots; i++) {
    snprintf(slots[i].dir_name, sizeof(slots[i].dir_name), "%s", sdirs[i]);
    slots[i].is_ps4 = sps4[i] ? 1 : 0;
    /* blocks / main_title 取自系统存档库：还原时要照原样写回系统记录，猜不得
       （blocks 猜错就是列表里大小显示 0，看着像坏了）。取不到才回落。 */
    char mt_i[256];
    mt_i[0] = 0;
    uint32_t blocks = 96;
    sm_db_savedata_info(uid_hex, sps4[i], tid, sdirs[i], &blocks, mt_i,
                        sizeof(mt_i));
    if (blocks == 0) blocks = 96;
    slots[i].blocks = blocks;
    /* 游戏名取第一个槽位查到的即可：同一个游戏的 main_title 一般一致，而
       「最后一个」在半路查不到的情况下反而更容易是回落值。 */
    if (i == 0 && mt_i[0]) snprintf(mt, sizeof(mt), "%s", mt_i);
    if (!sps4[i]) {
      char cand[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
      snprintf(cand, sizeof(cand),
               "/user/home/%s/savedata_prospero_meta/user/%s/%s_icon0.png",
               uid_hex, tid, sdirs[i]);
      sm_map_path(cand, mapped, sizeof(mapped));
      struct stat ist;
      if (stat(mapped, &ist) == 0 && S_ISREG(ist.st_mode)) {
        snprintf(icons[i], MAX_PATH_LEN, "%s", mapped);
        snprintf(slots[i].icon, sizeof(slots[i].icon),
                 "meta/%s/%s_icon0.png", tid, sdirs[i]);
      }
    }
  }
  /* 真游戏名来自 app.db（/system_data/priv/mms/app.db）：savedata.db 里的
     main_title 只是 sfo TITLE，像 "Dati salvati" 这种，不能当游戏名显示。 */
  sm_db_game_title(tid, gt, sizeof(gt));

  /* Pack every slot (image + PS4 .bin + meta icon) + manifest.json. */
  sm_map_path("/data/Mango/tmp", g_tmpdir, sizeof(g_tmpdir));
  recursive_mkdir(g_tmpdir);
  char zpath[MAX_PATH_LEN], mpath[MAX_PATH_LEN];
  snprintf(zpath, sizeof(zpath), "%s/_nex_bak_%ld.zip", g_tmpdir,
           (long)getpid());
  snprintf(mpath, sizeof(mpath), "%s/_nex_bak_%ld.json", g_tmpdir,
           (long)getpid());
  /* 自述文件是还原通道的唯一信息来源：镜像是加密的，不挂载读不到 param.sfo。 */
  if (savemgr_write_backup_manifest2(mpath, uid_hex, tid, slots, nslots,
                                     mt, 0, gt) != 0) {
    free(icons); free(slots);
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "manifest failed");
  }
  sm_file_sink_t sink = {0};
  sink.fd = open(zpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (sink.fd < 0) {
    unlink(mpath);
    free(icons); free(slots);
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "temp create failed");
  }
  int rc = -1;
  {
    /* 每个槽位最多三条（镜像 / PS4 .bin / icon），再加 manifest。槽位数不定
       ⇒ 数组走堆：请求线程的栈留给真正的小变量。 */
    int cap = nslots * 3 + 1;
    zipw_file_t *files = calloc((size_t)cap, sizeof(zipw_file_t));
    char *names = calloc((size_t)cap, 192);
    char *binpaths = calloc((size_t)nslots, MAX_PATH_LEN);
    if (!files || !names || !binpaths) {
      free(files); free(names); free(binpaths);
      rc = -1;
    } else {
      int nfiles = 0;
      for (int i = 0; i < nslots; i++) {
        snprintf(names + (size_t)nfiles * 192, 192, "%s/sdimg_%s", tid,
                 sdirs[i]);
        files[nfiles].path = imgs[i];
        files[nfiles].name = names + (size_t)nfiles * 192;
        nfiles++;
        if (sps4[i]) {
          /* .bin sits beside sdimg_<dir> as <dir>.bin (import_finalize 口径) */
          char *bp = binpaths + (size_t)i * MAX_PATH_LEN;
          snprintf(bp, MAX_PATH_LEN, "%s", imgs[i]);
          char *sl = strrchr(bp, '/');
          if (sl) *(sl + 1) = 0;
          snprintf(bp + strlen(bp), MAX_PATH_LEN - strlen(bp), "%s.bin",
                   sdirs[i]);
          snprintf(names + (size_t)nfiles * 192, 192, "%s/%s.bin", tid,
                   sdirs[i]);
          files[nfiles].path = bp;
          files[nfiles].name = names + (size_t)nfiles * 192;
          nfiles++;
        }
        /* icon 与 manifest 都不是 sdimg_ 开头 ⇒ 整包导入的扫描器
           （sm_arc_collect）会自然跳过它们，不会把 png 当成镜像去挂载。 */
        if (icons[i][0]) {
          files[nfiles].path = icons[i];
          files[nfiles].name = slots[i].icon;
          nfiles++;
        }
      }
      files[nfiles].path = mpath;
      files[nfiles].name = "manifest.json";
      nfiles++;
      zipw_sink_t zs;
      zs.write = sm_file_sink_write;
      zs.ctx = &sink;
      zs.bytes = 0;
      char *iobuf = malloc(256 * 1024);
      rc = iobuf ? zipw_write_files(&zs, files, nfiles, iobuf, 256 * 1024) : -1;
      free(iobuf);
      free(files); free(names); free(binpaths);
    }
  }
  close(sink.fd);
  unlink(mpath);
  if (rc < 0) {
    unlink(zpath);
    free(icons); free(slots);
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "zip failed");
  }
  if (rename(zpath, dst) < 0) {
    unlink(zpath);
    free(icons); free(slots);
    free(sdirs); free(sps4); free(imgs);
    free(uid); free(tid); free(dir); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "move failed");
  }
  /* 包旁再留一份同名 .json 侧车：列备份时**不必解开 zip**（几十个包时差别是
     秒级 vs 分钟级），而包内那份让拷到 PC 的备份仍然自描述。 */
  char sdfile[MAX_PATH_LEN];
  snprintf(sdfile, sizeof(sdfile), "%.*s.json",
           (int)strlen(dst) > 4 ? (int)strlen(dst) - 4 : 0, dst);
  int have_sd = 0;
  if (strlen(dst) > 4 &&
      savemgr_write_backup_manifest2(sdfile, uid_hex, tid, slots, nslots,
                                     mt, 0, gt) == 0)
    have_sd = 1;
  int have_icon = 0;
  for (int i = 0; i < nslots; i++) if (icons[i][0]) have_icon = 1;
  char out[1280];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"path\":\"%s\",\"manifest\":%s,\"icon\":%s,"
           "\"slots\":%d}",
           dst, have_sd ? "true" : "false", have_icon ? "true" : "false",
           nslots);
  mango_log("save_mgr/backup: tid=%s slots=%d -> %s (icon=%d)", tid, nslots,
            dst, have_icon);
  free(icons); free(slots);
  free(sdirs); free(sps4); free(imgs);
  free(uid); free(tid); free(dir); free(target); free(usb);
  return send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
}

/* ---- /api/save_mgr/icon -------------------------------------------------
 * 存档封面：/user/home/<uid>/savedata_prospero_meta/user/<tid>/<dir>_icon0.png。
 * 浏览页按游戏分组要显示封面，而前端拿不到 /user 下的东西（那是系统区，文件
 * 管理页不给进），所以单独开一条只读取 png 的端点。
 * ⚠️ 不能走 send_buffer()：它按 strlen 判长度，png 里第一个 0 字节就把响应
 * 截断了。这里自己按 stat 出来的字节数构造响应。 */
static enum MHD_Result
sm_send_png(struct MHD_Connection *conn, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return send_json_error(conn, MHD_HTTP_NOT_FOUND, "no icon");
  struct stat st;
  /* 封面是 png，正常几十 KB；给 8 MiB 上限纯粹是防呆（stat 到异常值时不至于
     去 malloc 一个荒唐的大小）。 */
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (8 << 20)) {
    close(fd);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "no icon");
  }
  size_t n = (size_t)st.st_size;
  char *buf = malloc(n);
  if (!buf) {
    close(fd);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, buf + got, n - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  if (got != n) {
    free(buf);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "short read");
  }
  struct MHD_Response *resp =
    MHD_create_response_from_buffer(n, buf, MHD_RESPMEM_MUST_FREE);
  if (!resp) {
    free(buf);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "image/png");
  /* ★ 缓存策略：`private, max-age=300` 而不是 no-cache。
   *
   *   🪤 no-cache 的代价是**每次进存档页每张封面都要重新走一遍候选链**
   *   （6 条静态路径 access + 跨槽 opendir + homebrew 目录扫描），用户看到的就是
   *   「先占位图、过几秒才冒出来」。改成长缓存后，同一个 URL 在 5 分钟内
   *   浏览器直接用,*不再回源* ⇒ 配合开页预热，封面真正能做到无感出现。
   *
   *   ★ 为什么不会被「刚提取出来的新封面」坑到：capture_icon 成功后前端把 src
   *   换成 `<原 URL>&_=<时间戳>`（见 smAutoCaptureIcon），URL 变了自然绕过缓存。
   *   新旧两套地址互不干扰，各走各的路径。
   *
   *   ⚠️ max-age 不能无限大：删掉某个存档后再装同 tid，封面 URL 完全相同，
   *   无限缓存会把旧图一直留在浏览器里。5 分钟是「够跨一次翻页浏览」又
   *   「不至于让变更看不见」的平衡点。 */
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                          "private, max-age=300");
  enum MHD_Result ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* ---- 封面候选路径 -------------------------------------------------------
 * 第 0 项是「这一个存档槽」自己的 meta 副本 —— 系统在存档被写过时顺手拷的。
 * 后几项是**游戏本体**的图标，不依赖"系统有没有拷过"。
 *
 * 🪤 为什么后几项要列这么多种拼法：PS5 上 appmeta 的真实布局随固件、随
 *   PS4/PS5 游戏而变；更麻烦的是 hombrew 进程读 /user/appmeta 还不一定被
 *   sandbox 放行。而**这两种失败完全长一样**（access() 都返回 -1）—— 区分
 *   不了就永远修不动。所以：errno 必须由 debug=1 吐出来（ENOENT=没这个文件，
 *   EACCES/EPERM=路径在但被挡）。
 *
 * ★ 这里还有一个容易被忽略的事实：所有这些静态候选查的都是"以 tid 命名的
 *   位置"。但一个游戏有**多个存档槽**（PPSA23226 有 4 个），每个槽各自有一份
 *   meta 副本。第 0 项只认当前这一个槽 ⇒ 只要这一个槽的镜像里没 icon，整组
 *   就永远空着。**跨槽借用**在下面单独处理。
 *
 * ★ 再补一条很容易漏的货源：fPKG / homebrew 安装目录。PS5 上
 *   /data/homebrew/<TID>-<名称>/sce_sys/icon0.png 就躺着封面图，
 *   但它不是 tid 命名的固定路径，得扫 /data/homebrew 里以 tid 开头的
 *   子目录。见 sm_icon_probe_homebrew()。 */
static const char *SM_ICON_PATS[] = {
  "/user/home/%s/savedata_prospero_meta/user/%s/%s_icon0.png", /* uid,tid,dir */
  "/user/appmeta/%s/icon0.png",
  "/user/appmeta/%s/sce_sys/icon0.png",
  "/system_data/priv/appmeta/%s/icon0.png",
  "/system_data/priv/appmeta/%s/sce_sys/icon0.png",
  "/user/app/%s/sce_sys/icon0.png",
};
#define SM_ICON_NPATS (sizeof(SM_ICON_PATS) / sizeof(SM_ICON_PATS[0]))

static void
sm_icon_cand(size_t k, const char *uid_hex, const char *tid, const char *dir,
             char *virt, size_t vsz, char *real, size_t rsz) {
  if (k == 0) snprintf(virt, vsz, SM_ICON_PATS[0], uid_hex, tid, dir);
  else        snprintf(virt, vsz, SM_ICON_PATS[k], tid);
  sm_map_path(virt, real, rsz);
}

/* 扫描 /data/homebrew 下以 tid 开头的目录，找 sce_sys/icon0.png。
 * 这是 fPKG/homebrew 安装路径，命名格式通常是 <TID>-<名称>，
 * 所以不能放进静态候选表，必须枚举。结果写进 mapped。 */
static const char *
sm_homebrew_dir(void) {
  const char *e = getenv("WFM_HOMEBREW_DIR");
  return (e && *e) ? e : "/data/homebrew";
}
static int
sm_icon_probe_homebrew(const char *tid, char *mapped, size_t mapsz,
                       char *hit_dir, size_t hit_dirsz) {
  const char *hb = sm_homebrew_dir();
  char hb_real[MAX_PATH_LEN];
  sm_map_path(hb, hb_real, sizeof(hb_real));
  DIR *d = opendir(hb_real);
  if (!d) return 0;
  size_t tid_len = strlen(tid);
  struct dirent *e;
  int found = 0;
  while ((e = readdir(d))) {
    if (!sm_safe_name(e->d_name)) continue;
    if (strncmp(e->d_name, tid, tid_len) != 0) continue;
    /* 防止 PPSA23226 匹配到 PPSA232260 之类的前缀陷阱：后面必须是
     * 目录名分隔边界（'-'、'_'、'.' 或 '\0'）。 */
    char c = e->d_name[tid_len];
    if (c && c != '-' && c != '_' && c != '.') continue;
    char full[MAX_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s/sce_sys/icon0.png",
             hb_real, e->d_name);
    if (access(full, R_OK) == 0) {
      snprintf(mapped, mapsz, "%s", full);
      if (hit_dir && hit_dirsz)
        snprintf(hit_dir, hit_dirsz, "%s/%s", hb, e->d_name);
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}

/* debug=1：把每个候选的存在性 + errno + meta 目录实况一次吐出来。
 *
 * 真机上只回一句 "no icon" 等于没回 —— 排障时要知道的是"根本没这个文件"、
 * "路径在但 sandbox 挡了" 还是 "tid/dir 拼错了"，三者的修法完全不同。 */
#define SM_ICON_DBG_CAP 16384
static enum MHD_Result
sm_icon_debug(struct MHD_Connection *conn, const char *uid_hex,
              const char *tid, const char *dir) {
  char  *out = malloc(SM_ICON_DBG_CAP);
  if (!out) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  size_t pos = 0;

  sm_json_append(out, SM_ICON_DBG_CAP, &pos,
                 "{\"uid\":\"%s\",\"title_id\":\"%s\",\"dir\":\"%s\",\"probes\":[",
                 uid_hex, tid, dir);
  size_t k;
  for (k = 0; k < SM_ICON_NPATS; k++) {
    char virt[MAX_PATH_LEN], real[MAX_PATH_LEN];
    sm_icon_cand(k, uid_hex, tid, dir, virt, sizeof(virt), real, sizeof(real));
    errno = 0;
    int ok = (access(real, R_OK) == 0);
    int e  = errno;
    unsigned long long sz = 0;
    if (ok) {
      struct stat st;
      if (stat(real, &st) == 0) sz = (unsigned long long)st.st_size;
    }
    if (k) sm_json_append(out, SM_ICON_DBG_CAP, &pos, ",");
    sm_json_append(out, SM_ICON_DBG_CAP, &pos,
                   "{\"path\":\"%s\",\"mapped\":\"%s\",\"ok\":%d,"
                   "\"errno\":%d,\"size\":%llu}",
                   virt, real, ok ? 1 : 0, e, sz);
  }

  /* meta 目录实况：这里到底躺着什么，是判断"缺文件"还是"名字对不上"的关键。 */
  char mv[MAX_PATH_LEN], mr[MAX_PATH_LEN], esc[512];
  snprintf(mv, sizeof(mv), "/user/home/%s/savedata_prospero_meta/user/%s",
           uid_hex, tid);
  sm_map_path(mv, mr, sizeof(mr));
  errno = 0;
  DIR *d = opendir(mr);
  int de = errno;
  sm_json_append(out, SM_ICON_DBG_CAP, &pos,
                 "],\"meta_dir\":{\"path\":\"%s\",\"open\":%d,\"errno\":%d,"
                 "\"entries\":[", mr, d ? 1 : 0, de);
  if (d) {
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if (!first) sm_json_append(out, SM_ICON_DBG_CAP, &pos, ",");
      first = 0;
      sm_json_esc(e->d_name, esc, sizeof(esc));
      sm_json_append(out, SM_ICON_DBG_CAP, &pos, "\"%s\"", esc);
    }
    closedir(d);
  }
  sm_json_append(out, SM_ICON_DBG_CAP, &pos, "]}");

  /* 存档槽实况：这台机器上一个 tid 到底有几份 sdimg_* —— 决定了"跨槽借用"
   * 有没有货源，也解释了为什么某个槽抠不到 icon 而别的槽可能能。 */
  snprintf(mv, sizeof(mv), "/user/home/%s/savedata_prospero/%s", uid_hex, tid);
  sm_map_path(mv, mr, sizeof(mr));
  errno = 0;
  d = opendir(mr);
  sm_json_append(out, SM_ICON_DBG_CAP, &pos,
                 ",\"slots_dir\":{\"path\":\"%s\",\"open\":%d,\"errno\":%d,"
                 "\"entries\":[", mr, d ? 1 : 0, errno);
  if (d) {
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d))) {
      if (!first) sm_json_append(out, SM_ICON_DBG_CAP, &pos, ",");
      first = 0;
      sm_json_esc(e->d_name, esc, sizeof(esc));
      sm_json_append(out, SM_ICON_DBG_CAP, &pos, "\"%s\"", esc);
    }
    closedir(d);
  }
  sm_json_append(out, SM_ICON_DBG_CAP, &pos, "]}");

  /* homebrew 目录实况：fPKG 安装目录里可能有封面。 */
  char hb_map[MAX_PATH_LEN], hb_hit[256];
  errno = 0;
  int hb_found = sm_icon_probe_homebrew(tid, hb_map, sizeof(hb_map),
                                        hb_hit, sizeof(hb_hit));
  sm_json_append(out, SM_ICON_DBG_CAP, &pos,
                 ",\"homebrew\":{\"found\":%d,\"path\":\"%s\","
                 "\"real\":\"%s\",\"errno\":%d}}",
                 hb_found, hb_hit, hb_map, errno);

  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

enum MHD_Result api_save_mgr_icon(struct MHD_Connection *conn, const char *url,
                                  const char *body, size_t body_size) {
  (void)url; (void)body; (void)body_size;
  char *uid = query_value(conn, "uid");
  char *tid = query_value(conn, "title_id");
  char *dir = query_value(conn, "dir");
  char *game = query_value(conn, "game");
  char *dbg = query_value(conn, "debug");
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  const int game_level = (game && (game[0] == '1' || !strcmp(game, "true")));
  /* 默认槽位级封面：dir 必填。带 game=1 时进入游戏级封面模式，只按 title_id
   * 从 appmeta / homebrew 等路径查找，不依赖具体存档槽。 */
  if (!tid || !tid[0] || !uid_hex[0] || !sm_safe_name(tid) ||
      (!game_level && (!dir || !dir[0])) ||
      (dir && dir[0] && !sm_safe_name(dir))) {
    free(uid); free(tid); free(dir); free(game); free(dbg);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "uid/title_id[/dir] required");
  }
  /* 排障通道：?debug=1 不返回图片，返回每个候选的探测结果（见上面注释）。 */
  if (dbg && dbg[0] && strcmp(dbg, "0") != 0) {
    enum MHD_Result r = sm_icon_debug(conn, uid_hex, tid, dir ? dir : "");
    free(uid); free(tid); free(dir); free(game); free(dbg);
    return r;
  }
  free(dbg);
  char cand[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
  int found = 0;
  {
    size_t k;
    for (k = 0; k < SM_ICON_NPATS; k++) {
      /* 游戏级列表没传 dir 时，跳过 savedata_prospero_meta 下以 dir 命名的候选，
       * 直接走 appmeta / homebrew 等 title_id 级路径。 */
      if (k == 0 && (!dir || !dir[0])) continue;
      sm_icon_cand(k, uid_hex, tid, dir ? dir : "", cand, sizeof(cand),
                   mapped, sizeof(mapped));
      if (access(mapped, R_OK) == 0) { found = 1; break; }
    }
  }
  /* ★ 跨槽借用：同一个游戏的**其它存档槽**有封面就拿来用。
   *   只有传了具体 dir 时才做；游戏级封面请求直接用上面 appmeta/homebrew 结果。
   *
   *   真机场景（PPSA23226《黑神话》）：它有 4 个槽，纯数据槽 ArchiveSaveFile1
   *   的镜像里确实没有 sce_sys/icon0.png（capture rc=-1 在日志里复现多次），
   *   但 UserSettingSaveGame 这类"设置存档"在系统口径上是带 icon 的。
   *   旧口径只认当前这一个槽的 meta 副本 ⇒ 这一个槽抠不到，整组就永远空着，
   *   哪怕兄弟槽里明明躺着一张封面。
   *
   *   封面是用来让人**认出游戏**的，不是槽的身份标识 —— 所以借用是合理的，
   *   而且这也最可能是"以前某个版本显示得出来"的那条路：那时还好用的那张图，
   *   本来就存在，只是不在我们正在查的这个槽名下。 */
  if (!found && dir && dir[0]) {
    char mdir[MAX_PATH_LEN], mreal[MAX_PATH_LEN];
    snprintf(mdir, sizeof(mdir), "/user/home/%s/savedata_prospero_meta/user/%s",
             uid_hex, tid);
    sm_map_path(mdir, mreal, sizeof(mreal));
    DIR *d = opendir(mreal);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 11 || strcmp(e->d_name + l - 10, "_icon0.png") != 0) continue;
        if (!sm_safe_name(e->d_name)) continue;
        snprintf(mapped, sizeof(mapped), "%s/%s", mreal, e->d_name);
        if (access(mapped, R_OK) == 0) { found = 1; break; }
      }
      closedir(d);
    }
  }
  /* homebrew / fPKG 安装目录兜底。 */
  if (!found) {
    char hb_virt[256];
    if (sm_icon_probe_homebrew(tid, mapped, sizeof(mapped),
                               hb_virt, sizeof(hb_virt)) == 1) {
      found = 1;
    }
  }
  /* 本地缓存兜底：游戏卸载后 appmeta / homebrew / 存档 meta 全没了，但只要
   * 这台机器在卸载前显示过一次封面，/data/Mango/covers/<tid>.png 就还在 ——
   * 存档残留的卡片不该因为游戏被卸载就永远变成灰占位图。 */
  if (!found) {
    if (sm_icon_cache_fetch(tid, mapped, sizeof(mapped)) == 1) found = 1;
  }
  if (!found) {
    free(uid); free(tid); free(dir); free(game);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "no icon");
  }
  /* 命中即缓存（幂等：源就是缓存、或已缓存同大小文件时是 no-op）—— 下一次
   * 游戏卸载后，这里存下的那份就是唯一货源。 */
  sm_icon_cache_store(tid, mapped);
  free(uid); free(tid); free(dir); free(game);
  return sm_send_png(conn, mapped);
}

/* ---- /api/save_mgr/backups ----------------------------------------------
 * 列备份：只读每个包旁边那份同名 .json 侧车，**一个 zip 都不解开** —— 备份
 * 目录里躺着几十个包时，逐个开包是分钟级，读侧车是毫秒级。 */
#define SM_BAK_JSON_CAP 32768

static void
sm_bak_collect(const char *base, const char *rel, int depth, char *out,
               size_t cap, size_t *pos, int *first) {
  char here[MAX_PATH_LEN];
  if (depth > 4) return;
  if (rel[0]) snprintf(here, sizeof(here), "%s/%s", base, rel);
  else snprintf(here, sizeof(here), "%s", base);
  DIR *d = opendir(here);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child_rel[MAX_PATH_LEN], full[MAX_PATH_LEN];
    if (rel[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name);
    else snprintf(child_rel, sizeof(child_rel), "%s", e->d_name);
    snprintf(full, sizeof(full), "%s/%s", base, child_rel);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      sm_bak_collect(base, child_rel, depth + 1, out, cap, pos, first);
      continue;
    }
    size_t l = strlen(e->d_name);
    if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0) continue;
    char mj[4096];
    if (sm_read_file(full, mj, sizeof(mj)) <= 0) continue;
    char tid[64], dir[128], mt[256], gt[256], uid[32], created[64];
    sm_json_str(mj, "title_id", tid, sizeof(tid));
    sm_json_str(mj, "dir_name", dir, sizeof(dir));
    sm_json_str(mj, "main_title", mt, sizeof(mt));
    sm_json_str(mj, "game_title", gt, sizeof(gt));
    sm_json_str(mj, "uid", uid, sizeof(uid));
    sm_json_str(mj, "created", created, sizeof(created));
    if (!tid[0] || !dir[0]) continue;
    long blocks = sm_json_num(mj, "blocks", 0);
    int is_ps4 = (sm_json_num(mj, "is_ps4", 0) != 0);
    char zpath[MAX_PATH_LEN], esc_mt[512], esc_gt[512], esc_cr[128];
    snprintf(zpath, sizeof(zpath), "%.*s.zip", (int)strlen(full) - 5, full);
    sm_json_esc(mt[0] ? mt : tid, esc_mt, sizeof(esc_mt));
    sm_json_esc(gt[0] ? gt : (mt[0] ? mt : tid), esc_gt, sizeof(esc_gt));
    sm_json_esc(created, esc_cr, sizeof(esc_cr));
    sm_json_append(out, cap, pos,
      "%s{\"path\":\"%s\",\"title_id\":\"%s\",\"dir_name\":\"%s\","
      "\"main_title\":\"%s\",\"game_title\":\"%s\",\"uid\":\"%s\","
      "\"created\":\"%s\",\"is_ps4\":%s,\"blocks\":%ld}",
      *first ? "" : ",", zpath, tid, dir, esc_mt, esc_gt, uid, esc_cr,
      is_ps4 ? "true" : "false", blocks);
    *first = 0;
  }
  closedir(d);
}

enum MHD_Result api_save_mgr_backups(struct MHD_Connection *conn,
                                     const char *url, const char *body,
                                     size_t body_size) {
  (void)url;
  char *target = req_param(conn, body, body_size, "target");
  char *usb = req_param(conn, body, body_size, "usb");
  char root[MAX_PATH_LEN], mapped[MAX_PATH_LEN];
  if (target && !strcmp(target, "usb") && usb && usb[0])
    snprintf(root, sizeof(root), "/mnt/%s/PS5/SAVEDATA/GARLIC", usb);
  else
    snprintf(root, sizeof(root), "/data/Mango/backups");
  free(target); free(usb);
  sm_map_path(root, mapped, sizeof(mapped));
  char *out = malloc(SM_BAK_JSON_CAP);
  if (!out) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
  size_t pos = 0;
  int first = 1;
  sm_json_append(out, SM_BAK_JSON_CAP, &pos, "{\"backups\":[");
  sm_bak_collect(mapped, "", 0, out, SM_BAK_JSON_CAP, &pos, &first);
  sm_json_append(out, SM_BAK_JSON_CAP, &pos, "]}");
  return send_buffer(conn, MHD_HTTP_OK, out, "application/json");
}

/* ---- /api/save_mgr/backup_delete ----------------------------------------
 * 删一份备份（zip + 它旁边那份 .json 侧车）。
 * ★ 侧车必须一起删：列表只读侧车，留着它就会出现「zip 已经不在、列表里还有
 *   这一条」的幽灵条目 —— 用户点还原拿到 404，比没有删除功能更让人困惑。 */
enum MHD_Result api_save_mgr_backup_delete(struct MHD_Connection *conn,
                                           const char *url, const char *body,
                                           size_t body_size) {
  (void)url;
  char *path = req_param(conn, body, body_size, "path");
  char *target = req_param(conn, body, body_size, "target");
  char *usb = req_param(conn, body, body_size, "usb");
  char mapped[MAX_PATH_LEN], root[MAX_PATH_LEN], root_m[MAX_PATH_LEN];

  if (!path || !path[0]) {
    free(path); free(target); free(usb);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path required");
  }
  sm_map_path(path, mapped, sizeof(mapped));
  if (target && !strcmp(target, "usb") && usb && usb[0])
    snprintf(root, sizeof(root), "/mnt/%s/PS5/SAVEDATA/GARLIC", usb);
  else
    snprintf(root, sizeof(root), "/data/Mango/backups");
  free(target); free(usb);
  sm_map_path(root, root_m, sizeof(root_m));

  /* ★ 路径校验不能省：path 虽然来自列表，但那是**前端回传**的参数，不可信。
   * 少了这层，`path=/data/Mango/whatever` 就能顺着这个接口删任意文件。
   * 允许落脚的只有两处 —— 本地备份根、USB 备份根（与 backups 列表同源），
   * 且必须是 .zip、且不能含 ".."。 */
  size_t rl = strlen(root_m);
  int ok = (strncmp(mapped, root_m, rl) == 0 && mapped[rl] == '/');
  if (ok && strstr(mapped, "..")) ok = 0;
  {
    size_t l = strlen(mapped);
    if (l < 5 || strcmp(mapped + l - 4, ".zip") != 0) ok = 0;
  }
  if (!ok) {
    mango_log("save_mgr/backup_delete: refused path=%s", mapped);
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "refusing to delete outside the backup folder");
  }

  struct stat st;
  if (stat(mapped, &st) != 0 || !S_ISREG(st.st_mode)) {
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "backup not found");
  }
  unlink(mapped);
  char side[MAX_PATH_LEN];
  snprintf(side, sizeof(side), "%.*s.json", (int)strlen(mapped) - 4, mapped);
  unlink(side);
  mango_log("save_mgr/backup_delete: removed %s (+sidecar)", mapped);
  free(path);
  return send_json_ok(conn);
}

/* ---- /api/save_mgr/restore ----------------------------------------------
 * 从备份包还原：把加密镜像复制回原路径 + 注册系统存档库，**全程不挂载**。
 * 只对「同机同账号」成立 —— 换机器/换账号的镜像密钥对不上，必须走导入重签。 */
enum MHD_Result api_save_mgr_restore(struct MHD_Connection *conn,
                                     const char *url, const char *body,
                                     size_t body_size) {
  (void)url;
  char *uid = req_param(conn, body, body_size, "uid");
  char *path = req_param(conn, body, body_size, "path");
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  if (!path || !path[0] || !uid_hex[0]) {
    free(uid); free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "path/uid required");
  }
  mango_log("save_mgr/restore: enter path=%s uid=%s", path, uid_hex);
  int r = savemgr_restore_backup(path, uid_hex);
  mango_log("save_mgr/restore: rc=%d", r);
  free(uid); free(path);
  if (r == 0) return send_json_ok(conn);
  if (r == -1) return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "bad name");
  if (r == -2) return send_json_error(conn, MHD_HTTP_NOT_FOUND,
                                      "backup not found");
  if (r == -3) {
    char why[192];
    snprintf(why, sizeof(why), "extract failed: %s", savemgr_last_zip_error());
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, why);
  }
  if (r == -4)
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "not a mango backup (missing manifest.json/image)");
  if (r == -5)
    return send_json_error(conn, MHD_HTTP_CONFLICT,
                           "backup belongs to another user; use import instead");
  if (r == -6) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                      "copy failed");
  /* -8 = RegMgr/UserService 不回应。**不是**我们崩了：看护线程已放弃，请求线程
   * 活着、锁已释放。给 503 + 明确处置建议，别让前端把它显示成"操作失败"。 */
  if (r == -8)
    return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
                           "user info service not responding; reboot console");
  return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         "db register failed");
}

/* ---- /api/save_mgr/dump_usb (device-only) -------------------------------- */

#ifndef __linux__
/* Small-frame recursive copy for dump_usb: path buffers and the copy buffer
 * come from the heap so a deep save tree cannot blow the 2 MiB task-thread
 * stack (see HANDOVER §10 for the LTO frame-size rule). */
static int
copy_tree_small(const char *src, const char *dst) {
  struct stat st;
  if (lstat(src, &st) < 0) return -1;
  if (S_ISDIR(st.st_mode)) {
    if (mkdir(dst, 0777) < 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) && rc == 0) {
      size_t sl, dl, nl;
      char *sp, *dp;
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      sl = strlen(src); dl = strlen(dst); nl = strlen(e->d_name);
      if (sl + nl + 2 > MAX_PATH_LEN || dl + nl + 2 > MAX_PATH_LEN) {
        rc = -1;
        break;
      }
      sp = malloc(sl + nl + 2);
      dp = malloc(dl + nl + 2);
      if (!sp || !dp) {
        free(sp);
        free(dp);
        rc = -1;
        break;
      }
      snprintf(sp, sl + nl + 2, "%s/%s", src, e->d_name);
      snprintf(dp, dl + nl + 2, "%s/%s", dst, e->d_name);
      rc = copy_tree_small(sp, dp);
      free(sp);
      free(dp);
    }
    closedir(d);
    return rc;
  }
  if (S_ISREG(st.st_mode)) {
    int in = open(src, O_RDONLY);
    int out;
    uint8_t *buf;
    ssize_t n;
    int rc = 0;
    if (in < 0) return -1;
    out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    buf = malloc(16384);
    if (!buf) { close(in); close(out); return -1; }
    while ((n = read(in, buf, 16384)) > 0) {
      size_t off = 0;
      while (off < (size_t)n) {
        ssize_t w = write(out, buf + off, (size_t)n - off);
        if (w <= 0) { rc = -1; break; }
        off += (size_t)w;
      }
      if (rc) break;
    }
    if (n < 0) rc = -1;
    free(buf);
    close(in);
    close(out);
    return rc;
  }
  return 0; /* sockets / devices / symlinks: skip */
}
#endif /* __linux__ */

enum MHD_Result api_save_mgr_dump_usb(struct MHD_Connection *conn,
                                      const char *url, const char *body,
                                      size_t body_size) {
  (void)url;
#ifndef __linux__
  char *uid = req_param(conn, body, body_size, "uid");
  char *tid = req_param(conn, body, body_size, "title_id");
  char *dir = req_param(conn, body, body_size, "dir_name");
  char *usb = req_param(conn, body, body_size, "usb");
  if (!tid || !tid[0] || !dir || !dir[0] || !usb || !usb[0]) {
    free(uid); free(tid); free(dir); free(usb);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "title_id/dir_name/usb required");
  }
  char uid_hex[16];
  sm_uid_hex(uid, uid_hex, sizeof(uid_hex));
  char img[MAX_PATH_LEN];
  snprintf(img, sizeof(img), "/user/home/%s/savedata_prospero/%s/sdimg_%s",
           uid_hex, tid, dir);
  char mp[512];
  if (savemgr_mount_image(img, mp, sizeof(mp)) < 0) {
    free(uid); free(tid); free(dir); free(usb);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "mount failed");
  }
  char dst[MAX_PATH_LEN];
  snprintf(dst, sizeof(dst), "/mnt/%s/PS5/SAVEDATA/GARLIC/%s/%s",
           usb, tid, dir);
  recursive_mkdir(dst);
  copy_tree_small(mp, dst);
  savemgr_unmount();
  free(uid); free(tid); free(dir); free(usb);
  char out[1024];
  snprintf(out, sizeof(out), "{\"ok\":true,\"path\":\"%s\"}", dst);
  return send_buffer(conn, MHD_HTTP_OK, strdup(out), "application/json");
#else
  (void)body; (void)body_size;
  return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "device-only");
#endif
}

/* ========================================================== dispatch =====
 * 参考的存档管理器把每一个 /api/ 请求都放在同一把进程级互斥锁下面执行
 * （pthread_mutex_lock(&g_mtx) 包住 handle_request_inner）。那是它与本移植版
 * 最大的结构差异：这里的 HTTP 服务跑的是 MHD_USE_THREAD_PER_CONNECTION，
 * 没有等价的锁，两条 HTTP 线程就能同时 mount/unmount，或者往一个正在卸载的
 * 存档里写文件 —— 真机上表现为「PS5 内核挂载死锁 / 进程还在但网页打不开」。
 * 下面这把锁把参考工具的串行语义还给整个 /api/save_mgr/ 命名空间。
 *
 * 唯一的例外是 /api/save_mgr/import_log：界面是在**长导入持有锁的期间**轮询
 * 它的，而它只读日志环（自带 g_import_log_lock）。把它也锁上会让进度面板冻住。
 */
static pthread_mutex_t g_savemgr_lock = PTHREAD_MUTEX_INITIALIZER;

/* 这些路由会写结构化进度（SM_IMP_*）。它们返回之后统一把阶段归零 ——
 * 单点收口，任何一条提前 return 的出口都不会把「进行中」漏给下一次导入
 * （漏了的话页面会在新任务还没开始时就先画上一轮的进度条）。 */
static int
sm_route_reports_progress(const char *url) {
  return !strcmp(url, "/api/save_mgr/import_archive") ||
         !strcmp(url, "/api/save_mgr/import_archive_finish") ||
         !strcmp(url, "/api/save_mgr/import_zip") ||
         !strcmp(url, "/api/save_mgr/import_encrypted") ||
         !strcmp(url, "/api/save_mgr/import_finish") ||
         !strcmp(url, "/api/save_mgr/upload") ||
         !strcmp(url, "/api/save_mgr/encrypt_upload") ||
         !strcmp(url, "/api/save_mgr/resign_upload") ||
         /* 还原也摊 zip（sm_zip_on_progress 会写字节口径进度）；不登记的话
          * begin() 不做，上一轮导入的日志会整段重放给前端，且本轮进度条不出现
          * —— 用户看到的就是「点了没反应」，然后以为进程崩了。 */
         !strcmp(url, "/api/save_mgr/restore");
}

static enum MHD_Result
save_mgr_route(struct MHD_Connection *conn, const char *url,
               const char *body, size_t body_size) {
  if (!strcmp(url, "/api/save_mgr/list"))
    return api_save_mgr_list(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/games"))
    return api_save_mgr_games(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/mount"))
    return api_save_mgr_mount(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/capture_icon"))
    return api_save_mgr_capture_icon(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/unmount"))
    return api_save_mgr_unmount(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/files"))
    return api_save_mgr_files(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/read"))
    return api_save_mgr_read(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/write"))
    return api_save_mgr_write(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/upload"))
    return api_save_mgr_upload(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_zip"))
    return api_save_mgr_import_zip(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_encrypted"))
    return api_save_mgr_import_encrypted(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_finish"))
    return api_save_mgr_import_finish(conn, url, body, body_size);
  /* 批量归档导入（一个 ZIP 含多个 sdimg_*）。三段都在锁内，保证摊包/挂载/
   * 落盘期间没有第二条连接能插进来动同一个挂载点。 */
  if (!strcmp(url, "/api/save_mgr/import_archive"))
    return api_save_mgr_import_archive(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_archive_finish"))
    return api_save_mgr_import_archive_finish(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_archive_cancel"))
    return api_save_mgr_import_archive_cancel(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/create_pfs"))
    return api_save_mgr_create_pfs(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/export"))
    return api_save_mgr_export(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/import_log"))
    return api_save_mgr_import_log(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/download"))
    return api_save_mgr_download(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/encrypt_upload"))
    return api_save_mgr_encrypt_upload(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/encrypt_download"))
    return api_save_mgr_encrypt_download(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/resign_upload"))
    return api_save_mgr_resign_upload(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/resign_download"))
    return api_save_mgr_resign_download(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/backup"))
    return api_save_mgr_backup(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/backups"))
    return api_save_mgr_backups(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/backup_delete"))
    return api_save_mgr_backup_delete(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/restore"))
    return api_save_mgr_restore(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/delete"))
    return api_save_mgr_delete(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/icon"))
    return api_save_mgr_icon(conn, url, body, body_size);
  if (!strcmp(url, "/api/save_mgr/dump_usb"))
    return api_save_mgr_dump_usb(conn, url, body, body_size);
  return send_json_error(conn, MHD_HTTP_NOT_FOUND, "unknown save_mgr route");
}

/* ★ 「浏览类」路由：只读或者毫秒级的操作，**绝不该为了等一次正在进行的批量
 * 导入而在锁上排几分钟**。
 *
 * 旧写法是除 import_log 之外全部 pthread_mutex_lock —— 于是一次 3 张存档的
 * 批量导入（每张 mount+umount，每次看护 60 秒）最坏能把 g_savemgr_lock 独占
 * 6 分钟。这期间「刷新列表」「强制卸载」「放弃导入」全部排在锁上不动。
 *
 * 后果比"按钮没反应"严重得多：浏览器对同一个 origin 只有 ~6 条并发连接，
 * 存档页同时在飞的请求（list / files / unmount / 取消 / status / tasks）很容易
 * 就把这 6 条占满 ⇒ **连 /api/status 都发不出去** ⇒ 用户看到的就是「进程还在、
 * 网页全都打不开、点了没用」。而服务端每条连接一个线程，这些线程也一直挂着。
 *
 * 改成 trylock：拿不到锁就立刻回 503 {"error":"busy"}，前端据此说人话
 * （「后台正在导入，请稍候」），页面保持可用。这是 2026-10-11 真机报障的直接对策。 */
static int
sm_route_is_browsable(const char *url) {
  return !strcmp(url, "/api/save_mgr/list") ||
         !strcmp(url, "/api/save_mgr/games") ||
         !strcmp(url, "/api/save_mgr/files") ||
         !strcmp(url, "/api/save_mgr/unmount") ||
         /* 列备份只读包旁那份 .json 侧车（不开 zip），与列存档同级：后台在跑
            批量导入时也该让页面刷得出来。 */
         !strcmp(url, "/api/save_mgr/backups") ||
         /* 删备份：两个 unlink，毫秒级。后台在跑批量导入时也该让页面刷得出来
            —— 否则「删除」按钮点了转圈，用户会以为又卡死了。 */
         !strcmp(url, "/api/save_mgr/backup_delete") ||
         /* 取封面：只读一个几十 KB 的 png，与列存档同级 —— 后台在跑批量导入时
            页面也该刷得出来（封面是浏览页的第一眼，刷不出来等于页面坏了）。 */
         !strcmp(url, "/api/save_mgr/icon") ||
         !strcmp(url, "/api/save_mgr/import_archive_cancel");
}

enum MHD_Result
dispatch_save_mgr(struct MHD_Connection *conn, const char *url,
                  const char *body, size_t body_size) {
  const int log_only = !strcmp(url, "/api/save_mgr/import_log");
  const int prog = !log_only && sm_route_reports_progress(url);
  const int soft = !log_only && sm_route_is_browsable(url);
  enum MHD_Result r;

  if (soft) {
    /* ★ 封面路由单独对待：有界等待，不是瞬死 trylock（2026-10-23 真机诊断
       docs/diagnosis-2026-10-22 §3.2 的修法 2）。
       开页一瞬间 5 张封面 + 存档页 games 全目录扫描互相抢锁，trylock 抢不到
       的那几张直接 503；前端旧行为是 onerror 即 remove、无重试 ⇒ 这两张卡
       的封面在整个页面生命周期里消失（PPSA05143 / CUSA20499 就是这么没的）。
       封面只读一个几十 KB 的 png，值得等：2s 足够任何一次 games 扫描放锁；
       真赶上分钟级批量导入时 2s 后照样回 503 busy，前端有退避重试兜底，
       行为相比旧版只赢不输。 */
    if (!strcmp(url, "/api/save_mgr/icon")) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;
      if (pthread_mutex_timedlock(&g_savemgr_lock, &ts) != 0)
        return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, "busy");
    } else if (pthread_mutex_trylock(&g_savemgr_lock) != 0) {
      return send_json_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, "busy");
    }
  } else if (!log_only) {
    pthread_mutex_lock(&g_savemgr_lock);
  }
  /* ⚠️ begin/end 必须成对：begin 清掉上一轮的日志与进度。前端是先开窗再发
     请求的，不清就会把上一次操作的整段日志先重放一遍。 */
  if (prog) savemgr_import_progress_begin();
  r = save_mgr_route(conn, url, body, body_size);
  if (!log_only) pthread_mutex_unlock(&g_savemgr_lock);
  if (prog) savemgr_import_progress_end();
  return r;
}

