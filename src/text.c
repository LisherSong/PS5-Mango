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

/* Read up to `max` bytes. If the file is larger, read the leading `max` bytes and
 * set *truncated. Returns 0 on success, -1 on error (errno set). */
static int
read_file_bytes(const char *path, size_t max, unsigned char **data,
               size_t *size, struct stat *st, int *truncated) {
  FILE *file;
  size_t toread, rs;

  *data = NULL;
  *size = 0;
  if(truncated) *truncated = 0;
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

/* NEXUS /api/fs/read: returns JSON {ok, mime, size, truncated, version, data(base64)}. */
enum MHD_Result
api_text(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = req_param(conn, body, body_size, "path");
  const char *mime;
  unsigned char *data = NULL;
  size_t size = 0;
  struct stat st;
  int truncated = 0;
  unsigned long long version;
  char ver_txt[24];
  char *b64 = NULL;
  strbuf_t b = {0};

  if(!path) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  mime = mime_for_path(path);
  if(read_file_bytes(path,
                     strncmp(mime, "image/", 6) == 0 ? IMAGE_FILE_MAX_SIZE
                                                      : TEXT_FILE_MAX_SIZE,
                     &data, &size, &st, &truncated)) {
    int err = errno;
    free(path);
    free(data);
    if(err == EFBIG) {
      return send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                             "file is too large");
    }
    errno = err;
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  free(path);
  version = text_version(data, size);
  snprintf(ver_txt, sizeof(ver_txt), "%016llx", version);
  if(!(b64 = base64_encode(data, size))) {
    free(data);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  free(data);

  strbuf_printf(&b, "{\"ok\":true,\"mime\":");
  json_escape(&b, mime);
  strbuf_printf(&b, ",\"size\":%lld,\"truncated\":%s,\"version\":",
                (long long)st.st_size, truncated ? "true" : "false");
  json_escape(&b, ver_txt);
  strbuf_append(&b, ",\"data\":");
  json_escape(&b, b64);
  strbuf_append(&b, "}");
  free(b64);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
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
  n = snprintf(temp, sizeof(temp), "%s.wfm-%ld-%ld.tmp", path,
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
  if(read_file_bytes(path, TEXT_FILE_MAX_SIZE, &current, &current_size, &st,
                     NULL)) {
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
