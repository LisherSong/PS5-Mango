#include "filemgr_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "json_util.h"
#include "path_util.h"
#include "readsrc.h"
#include "transfer.h"
#include "websrv.h"

#define TEXT_FILE_MAX_SIZE (1024 * 1024)
#define IMAGE_FILE_MAX_SIZE (4 * 1024 * 1024)

static unsigned long long
text_version(const unsigned char *data, size_t size) {
  unsigned long long hash = 1469598103934665603ULL;
  size_t i;

  for(i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static const char *
mime_for_path(const char *path) {
  const char *e = strrchr(path, '.');

  if(!e) {
    return "text/plain";
  }
  if(!strcasecmp(e, ".png")) return "image/png";
  if(!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg")) return "image/jpeg";
  if(!strcasecmp(e, ".gif")) return "image/gif";
  if(!strcasecmp(e, ".webp")) return "image/webp";
  if(!strcasecmp(e, ".bmp")) return "image/bmp";
  return "text/plain";
}

static const char B64_ALPHABET[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
base64_encode(const unsigned char *in, size_t len) {
  size_t out_len = ((len + 2) / 3) * 4 + 1;
  char *out = malloc(out_len);
  size_t i, o = 0;

  if(!out) {
    return NULL;
  }
  for(i = 0; i < len; i += 3) {
    unsigned int v = (unsigned int)in[i] << 16;
    if(i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
    if(i + 2 < len) v |= (unsigned int)in[i + 2];
    out[o++] = B64_ALPHABET[(v >> 18) & 63];
    out[o++] = B64_ALPHABET[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? B64_ALPHABET[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? B64_ALPHABET[v & 63] : '=';
  }
  out[o] = 0;
  return out;
}

/* NAS/SMB/NFS/WebDAV 上的同名读取：走 transfer 抽象，不碰 open()。
 *
 * 为什么非得单独一支（2026-10-27 真机）：WebDAV 列目录正常、点图片却报
 * 「读取失败 / file not found」—— 因为 /api/fs/read 只走 lstat+fopen，
 * 而 NAS 文件传过来的是 host/share/path（凭据另在 user/pass 字段里）。
 * 跟 PKG 直装那次是同一个根因家族（本地专用路径收到了 NAS 地址）。
 *
 * ⚠️ scheme 与 url 分开收：nas_build_url 只对 webdavs 补 `webdavs://`
 * 前缀，smb/nfs/webdav 出来的是**不带前缀**的 user:pass@host/share/path
 * —— 所以不能靠 strstr(url,"://") 反推 scheme，必须从调用方拿。 */
static int
read_nas_bytes(const char *scheme, const char *url, size_t max,
               unsigned char **data, size_t *size, struct stat *st,
               int *truncated) {
  size_t want;
  transfer_src_t *src;
  uint64_t total = 0;
  mango_err_t rc;

  *data = NULL;
  *size = 0;
  if(truncated) *truncated = 0;
  memset(st, 0, sizeof(*st));
  if(!scheme || !*scheme || !transfer_scheme_supported(scheme)) return -1;
  src = transfer_open(scheme, url);
  if(!src) return -1;
  if(transfer_size(src, &total) != MANGO_OK) {
    transfer_close(src);
    return -1;
  }
  want = (total > (uint64_t)max) ? max : (size_t)total;
  if(truncated && total > (uint64_t)max) *truncated = 1;
  if(!(*data = malloc(want + 1))) {
    transfer_close(src);
    return -1;
  }
  if(want) {
    rc = transfer_read(src, 0, *data, want, size);
    if(rc != MANGO_OK || *size != want) {
      transfer_close(src);
      free(*data);
      *data = NULL;
      *size = 0;
      return -1;
    }
  }
  transfer_close(src);
  (*data)[*size] = 0;
  st->st_mode = S_IFREG | 0644;
  st->st_size = (off_t)total;
  return 0;
}

/* Read up to `max` bytes. If the file is larger, read the leading `max` bytes and
 * set *truncated. Returns 0 on success, -1 on error (errno set).
 *
 * `scheme`/`url` 非空 ⇒ NAS 源（url 是 nas_build_url 拼好的地址，可能不带
 * scheme 前缀，所以两个参数都得给）。两者都空 ⇒ 本地文件。 */
static int
read_file_bytes(const char *path, const char *scheme, const char *url,
                size_t max, unsigned char **data, size_t *size,
                struct stat *st, int *truncated) {
  FILE *file;
  size_t toread, rs;

  *data = NULL;
  *size = 0;
  if(truncated) *truncated = 0;
  if(scheme && *scheme && url && *url)
    return read_nas_bytes(scheme, url, max, data, size, st, truncated);
  if(lstat(path, st) || !S_ISREG(st->st_mode)) {
    return -1;
  }
  if(st->st_size < 0) {
    return -1;
  }
  toread = ((unsigned long long)st->st_size > max) ? max
                                                  : (size_t)st->st_size;
  if(truncated && (unsigned long long)st->st_size > max) {
    *truncated = 1;
  }
  if(!(file = fopen(path, "rb"))) {
    return -1;
  }
  if(!(*data = malloc(toread + 1))) {
    fclose(file);
    return -1;
  }
  rs = fread(*data, 1, toread, file);
  if(rs != toread) {
    free(*data);
    *data = NULL;
    fclose(file);
    return -1;
  }
  fclose(file);
  (*data)[rs] = 0;
  *size = rs;
  return 0;
}

/* MANGO /api/fs/read: returns JSON {ok, mime, size, truncated, version, data(base64)}. */
enum MHD_Result
api_text(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = req_param(conn, body, body_size, "path");
  char *scheme = req_param(conn, body, body_size, "scheme");
  char *user = req_param(conn, body, body_size, "user");
  char *pass = req_param(conn, body, body_size, "pass");
  char *port = req_param(conn, body, body_size, "port");
  const char *mime;
  unsigned char *data = NULL;
  size_t size = 0;
  struct stat st;
  int truncated = 0;
  unsigned long long version;
  char ver_txt[24];
  char *b64 = NULL;
  char url[MANGO_PATH_MAX + 600];
  int is_nas = 0;
  strbuf_t b = {0};

  if(!path) {
    free(scheme); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  /* NAS 作用域：前端把 scheme/user/pass/port 与 /api/fs/list 一样随路带上，
   * 拼 URL 复用 list.c 那一份实现（凭据从不落进前端的路径栏）。
   * 也兼容「直接给了完整 URL」的调用（scheme 省略、path 自带 ://）——
   * 那种形态自己把 scheme 摘出来，url 只留 :// 之后的部分。 */
  if(scheme && *scheme && strcmp(scheme, "local")) {
    nas_build_url(path, user, pass, port, scheme, url, sizeof(url));
    is_nas = 1;
  } else if((!scheme || !*scheme) && strstr(path, "://")) {
    const char *sep = strstr(path, "://");
    size_t n = (size_t)(sep - path);
    if(n && n < 16) {
      free(scheme);
      if(!(scheme = malloc(n + 1))) {
        free(path); free(user); free(pass); free(port);
        return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "out of memory");
      }
      memcpy(scheme, path, n);
      scheme[n] = 0;
      snprintf(url, sizeof(url), "%s", sep + 3);
      is_nas = 1;
    }
  }
  mime = mime_for_path(path);
  if(read_file_bytes(path, is_nas ? scheme : NULL, is_nas ? url : NULL,
                     strncmp(mime, "image/", 6) == 0 ? IMAGE_FILE_MAX_SIZE
                                                      : TEXT_FILE_MAX_SIZE,
                     &data, &size, &st, &truncated)) {
    int err = errno;
    free(path);
    free(scheme); free(user); free(pass); free(port);
    free(data);
    if(err == EFBIG) {
      return send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                             "file is too large");
    }
    /* NAS 上的失败别报成「文件不存在」：文件明明列得出来，真实原因是共享
     * 打不开 / 凭据不对 / 后端不支持。报 502 + 指向 NAS 的说法，免得用户
     * 以为是路径写错了。 */
    if(is_nas) {
      return send_json_error(conn, MHD_HTTP_BAD_GATEWAY,
                             "cannot read this file from the NAS");
    }
    errno = err;
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  free(path);
  free(scheme); free(user); free(pass); free(port);
  version = text_version(data, size);
  snprintf(ver_txt, sizeof(ver_txt), "%016llx", version);
  if(!(b64 = base64_encode(data, size))) {
    free(data);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  free(data);

  strbuf_printf(&b, "{\"ok\":true,\"mime\":");
  json_escape(&b, mime);
  /* size = **实际返回**的字节数，total = 文件完整大小。
   * 🪤 早年只发 st.st_size，于是截断时前端把「文件一共 7.1 MB」当成
   *    「显示了前 7.1 MB」（真机原话：IMG_…jpeg（已截断，仅显示前 7.1 MB），
   *    而实际只取了 4 MiB）—— 数字本身是错的，用户据此判断不出截了多少。 */
  strbuf_printf(&b, ",\"size\":%lld,\"total\":%lld,\"truncated\":%s,\"version\":",
                (long long)size, (long long)st.st_size,
                truncated ? "true" : "false");
  json_escape(&b, ver_txt);
  strbuf_append(&b, ",\"data\":");
  json_escape(&b, b64);
  strbuf_append(&b, "}");
  free(b64);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* -------------------------------------------------------------------------
 * /api/fs/raw：同一个只读源的**二进制**出口（参数与 /api/fs/read 一致；
 * POST body 或 GET query 都收 —— 媒体元素只能发 GET）。
 *
 * 为什么非得再来一条（2026-10-28 真机）：图片预览走的是 /api/fs/read 的
 * base64，于是必须封 cap（4 MiB）——手机相册单张就 7 MB，一截断用户以为
 * 「图片坏了」。base64 还让内存翻 3 倍（原始 + base64 + JSON 缓冲），
 * 单纯把 cap 放大在 PS5 上不安全。改成流式吐字节：服务端只留一块读缓冲，
 * 前端拿 Blob 做 objectURL，图片大小不再受限。
 *
 * 2026-10-29 增补 Range：视频/音频改从这里取流（原先走 /api/download 的
 * 下载任务）。三个理由，都来自真机反馈：
 *   ① 下载任务会点亮右下角任务坞 —— 用户「播放时不想看见进度条窗口」；
 *   ② 下载任务全局只允许一个：播着视频去点下载就是 409；
 *   ③ 任务流不认 Range —— 拖进度条等于整份重拉，mp3 起播也慢。
 * 现在按 RFC 7233 处理单段 Range（多段只取第一段，<video> 从不发多段），
 * 应答 206 + Content-Range，并宣告 Accept-Ranges: bytes。
 * ------------------------------------------------------------------------- */
typedef struct raw_stream {
  read_src_t *src;
  uint64_t   base;     /* 响应体第 0 字节对应的文件偏移（Range 起点） */
  uint64_t   len;      /* 本次响应体的字节数 */
  int        failed;
} raw_stream_t;

static ssize_t
raw_stream_read(void *cls, uint64_t pos, char *buf, size_t max) {
  raw_stream_t *s = cls;
  size_t got = 0, want;

  if(!s || !s->src) return MHD_CONTENT_READER_END_WITH_ERROR;
  if(pos >= s->len) return MHD_CONTENT_READER_END_OF_STREAM;
  want = (size_t)(s->len - pos);
  if(want > max) want = max;
  if(read_src_pread(s->src, s->base + pos, buf, want, &got)) {
    s->failed = 1;
    return MHD_CONTENT_READER_END_WITH_ERROR;
  }
  if(!got) return MHD_CONTENT_READER_END_OF_STREAM;
  return (ssize_t)got;
}

static void
raw_stream_close(void *cls) {
  raw_stream_t *s = cls;

  if(!s) return;
  if(s->src) read_src_close(s->src);
  free(s);
}

/* 解析单段 Range。返回 1 = 按 Range 应答，0 = 没有 / 不认识（整份应答），
 * -1 = 不可满足（416）。只认 bytes=，其余单位按「不认」处理。 */
static int
raw_range_parse(const char *hdr, uint64_t total, uint64_t *start,
                uint64_t *end) {
  const char *p;
  char *ep = NULL;
  unsigned long long a = 0, b = 0;
  int has_b = 0;

  if(!hdr || !total) return 0;
  while(*hdr == ' ') hdr++;
  if(strncmp(hdr, "bytes", 5)) return 0;
  p = hdr + 5;
  while(*p == ' ') p++;
  if(*p != '=') return 0;
  p++;
  while(*p == ' ') p++;

  if(*p == '-') {                      /* bytes=-N：最后 N 字节 */
    p++;
    b = strtoull(p, &ep, 10);
    if(ep == p || !b) return ep == p ? 0 : -1;
    if(b > total) b = total;
    *start = total - b;
    *end = total - 1;
    return 1;
  }
  a = strtoull(p, &ep, 10);
  if(ep == p) return 0;
  if(*ep != '-') return 0;
  p = ep + 1;
  if(*p >= '0' && *p <= '9') {
    b = strtoull(p, &ep, 10);
    has_b = 1;
  }
  if(a >= total) return -1;            /* 起点在文件尾之后 */
  *start = a;
  *end = has_b ? (b < total ? b : total - 1) : total - 1;
  if(*end < *start) return -1;
  return 1;
}

static enum MHD_Result
raw_range_unsatisfiable(struct MHD_Connection *conn, uint64_t total) {
  char cr[64];
  struct MHD_Response *resp;
  enum MHD_Result ret;

  snprintf(cr, sizeof(cr), "bytes */%llu", (unsigned long long)total);
  resp = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, cr);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
  ret = websrv_queue_response(conn, MHD_HTTP_RANGE_NOT_SATISFIABLE,
                              resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
api_fs_raw(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = req_param(conn, body, body_size, "path");
  char *scheme = req_param(conn, body, body_size, "scheme");
  char *user = req_param(conn, body, body_size, "user");
  char *pass = req_param(conn, body, body_size, "pass");
  char *port = req_param(conn, body, body_size, "port");
  read_src_t *src = NULL;
  raw_stream_t *st = NULL;
  struct MHD_Response *resp;
  enum MHD_Result ret;
  const char *mime;
  const char *range_hdr;
  char why[384];
  char cr[80];
  uint64_t size = 0, start = 0, end = 0;
  int was_nas, ranged, rr;

  if(!path) {
    free(path); free(scheme); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  /* mime 必须在 free(path) 之前取：mime_for_path 返回的是静态字面量，
   * 指针本身活得比 path 久，但别养成「释放后再解引用」的习惯。 */
  mime = mime_for_path(path);
  src = read_src_open(path, scheme, user, pass, port);
  was_nas = read_src_is_nas(src);
  free(path);
  free(scheme); free(user); free(pass); free(port);
  if(read_src_bad(src)) {
    snprintf(why, sizeof(why), "NAS 文件读取失败：%s", read_src_error(src));
    read_src_close(src);
    /* 与 /api/fs/read 同一口径：NAS 上的失败不伪装成 file not found ——
     * 后者会让用户以为是路径写错了，而真实原因是凭据/共享/网络。 */
    if(was_nas) {
      return send_json_error_detail(conn, MHD_HTTP_BAD_GATEWAY, why,
                                    "nas_read_failed", NULL);
    }
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  size = read_src_size(src);
  if(!size) {                    /* 0 字节：MHD 的 callback 不接受 0 长度 */
    struct MHD_Response *empty =
      MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
    read_src_close(src);
    if(!empty) return MHD_NO;
    MHD_add_response_header(empty, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    ret = websrv_queue_response(conn, MHD_HTTP_OK, empty);
    MHD_destroy_response(empty);
    return ret;
  }

  range_hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Range");
  rr = raw_range_parse(range_hdr, size, &start, &end);
  if(rr < 0) {
    read_src_close(src);
    return raw_range_unsatisfiable(conn, size);
  }
  ranged = rr > 0;
  if(!ranged) { start = 0; end = size - 1; }

  if(!(st = calloc(1, sizeof(*st)))) {
    read_src_close(src);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  st->src = src;
  st->base = start;
  st->len = end - start + 1;
  resp = MHD_create_response_from_callback(st->len, 256 * 1024,
                                           raw_stream_read, st,
                                           raw_stream_close);
  if(!resp) {
    raw_stream_close(st);
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  if(ranged) {
    snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu",
             (unsigned long long)start, (unsigned long long)end,
             (unsigned long long)size);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, cr);
  }
  ret = websrv_queue_response(conn,
                              ranged ? MHD_HTTP_PARTIAL_CONTENT : MHD_HTTP_OK,
                              resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
api_text_create(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = req_param(conn, body, body_size, "path");
  char *name = req_param(conn, body, body_size, "name");
  char target[PATH_MAX];
  int fd = -1;
  int ret = -1;
  int error = 0;
  int created = 0;

  if(!path || !name || path_join(target, sizeof(target), path, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if((fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0777)) >= 0) {
    created = 1;
    ret = fchmod_0777(fd);
    if(close(fd) && !ret) ret = -1;
    fd = -1;
  }
  if(ret) {
    error = errno;
    if(fd >= 0) close(fd);
    if(created) unlink(target);
  }
  free(path); free(name);
  if(!ret) {
    return send_json_ok(conn);
  }
  errno = error;
  return error == EEXIST ?
    send_json_error(conn, MHD_HTTP_CONFLICT, "file already exists") :
    send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL);
}

static int
write_text_atomic(const char *path, const char *body, size_t body_size,
                  mode_t mode) {
  struct timespec now;
  char temp[PATH_MAX];
  size_t written = 0;
  int fd = -1;
  int ret = -1;
  int n;

  clock_gettime(CLOCK_MONOTONIC, &now);
  n = snprintf(temp, sizeof(temp), "%s.nex-%ld-%ld.tmp", path,
               (long)getpid(), now.tv_nsec);
  if(n < 0 || (size_t)n >= sizeof(temp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if((fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
    return -1;
  }
  while(written < body_size) {
    ssize_t count = write(fd, body + written, body_size - written);
    if(count <= 0) {
      goto done;
    }
    written += (size_t)count;
  }
  if(fchmod(fd, mode & 07777) && !ignore_chmod_error(errno)) {
    goto done;
  }
  if(fsync(fd)) {
    goto done;
  }
  if(close(fd)) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if(rename(temp, path)) {
    goto done;
  }
  ret = 0;

done:
  if(fd >= 0) close(fd);
  if(ret) unlink(temp);
  return ret;
}

enum MHD_Result
api_text_save(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = req_param(conn, body, body_size, "path");
  char *expected = req_param(conn, body, body_size, "version");
  char *content = req_param(conn, body, body_size, "content");
  unsigned char *current = NULL;
  size_t current_size = 0;
  struct stat st;
  char version_text[24];
  char parent[PATH_MAX];
  int ret;

  if(!path) {
    free(expected); free(content);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(!content) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid content");
  }
  if(body_size > TEXT_FILE_MAX_SIZE + (TEXT_FILE_MAX_SIZE / 4)) {
    free(path); free(expected); free(content);
    return send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                           "text file is too large");
  }
  /* 保存只对本地文件做乐观锁校验（NAS 上的编辑不在本接口范围内）：
     scheme/url 传空 ⇒ 走本地分支。 */
  if(read_file_bytes(path, NULL, NULL, TEXT_FILE_MAX_SIZE, &current,
                     &current_size, &st, NULL)) {
    free(path); free(expected); free(content);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  snprintf(version_text, sizeof(version_text), "%016llx",
           text_version(current, current_size));
  free(current);
  if(expected && strcmp(expected, version_text)) {
    free(path); free(expected); free(content);
    return send_json_error(conn, MHD_HTTP_CONFLICT,
                           "file changed since it was opened");
  }
  if(path_dirname(path, parent, sizeof(parent)) ||
     mode_access(path, W_OK) || mode_access(parent, W_OK | X_OK)) {
    free(path); free(expected); free(content);
    return send_json_error(conn, MHD_HTTP_FORBIDDEN,
                           "text file is not writable");
  }
  ret = write_text_atomic(path, content, strlen(content), st.st_mode);
  free(path); free(expected); free(content);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}
