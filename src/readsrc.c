#include "readsrc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "mango_common.h"
#include "transfer.h"

struct read_src {
  int fd;                 /* 本地：>= 0 有效 */
  transfer_src_t *ts;     /* NAS：非 NULL 有效 */
  uint64_t size;
  int bad;                /* 1 = 打开失败，只有 error 有意义 */
  int err;                /* 本地失败时的 errno */
  char msg[256];          /* NAS 失败原因的快照（transfer_last_error 随源释放） */
};

/* NAS 地址拼装复用 list.c 的 nas_build_url —— 拆 URL 的活只有那一处，
 * 这里再写一份就会出现「凭证分隔符两边不一致」这类偏差。
 * 缓冲区在堆上：请求线程栈不许放大缓冲（chk_stack_buf：真机爆栈事故）。 */
static int
rs_nas_url(const char *path, const char *user, const char *pass,
           const char *port, const char *scheme, char **out) {
  size_t n = MANGO_PATH_MAX + 600;
  char *buf;

  if(!(buf = malloc(n))) return -1;
  nas_build_url(path, user, pass, port, scheme, buf, n);
  *out = buf;
  return 0;
}

static void
rs_set_msg(read_src_t *s, const char *m) {
  if(m && *m) {
    snprintf(s->msg, sizeof(s->msg), "%s", m);
  }
}

read_src_t *
read_src_open(const char *path, const char *scheme, const char *user,
              const char *pass, const char *port) {
  read_src_t *s;

  if(!path || !(s = calloc(1, sizeof(*s)))) return NULL;
  s->fd = -1;
  s->bad = 1;

  if(scheme && *scheme && strcmp(scheme, "local")) {
    char *url = NULL;
    uint64_t total = 0;

    /* 未登记的 scheme 直接失败：早年的 bug 正是「不在写死清单里 ⇒ 当成本地
     * 路径 ⇒ fopen 一个 host/share/x ⇒ 报 file not found」。 */
    if(!transfer_scheme_supported(scheme) ||
       rs_nas_url(path, user, pass, port, scheme, &url)) {
      s->err = EPROTONOSUPPORT;
      snprintf(s->msg, sizeof(s->msg), "unsupported source scheme");
      free(url);
      return s;
    }
    s->ts = transfer_open(scheme, url);
    free(url);
    if(!s->ts) {
      s->err = EIO;
      snprintf(s->msg, sizeof(s->msg), "cannot open the NAS source");
      return s;
    }
    if(transfer_size(s->ts, &total) != MANGO_OK) {
      rs_set_msg(s, transfer_last_error(s->ts));
      if(!s->msg[0]) {
        snprintf(s->msg, sizeof(s->msg), "cannot stat the NAS file");
      }
      s->err = EIO;
      return s;
    }
    s->size = total;
  } else {
    struct stat st;

    if((s->fd = open(path, O_RDONLY)) < 0) {
      s->err = errno;
      return s;
    }
    if(fstat(s->fd, &st) || !S_ISREG(st.st_mode)) {
      s->err = (!fstat(s->fd, &st) && S_ISDIR(st.st_mode)) ? EISDIR : EINVAL;
      close(s->fd);
      s->fd = -1;
      return s;
    }
    s->size = (uint64_t)st.st_size;
  }

  s->bad = 0;
  return s;
}

int
read_src_bad(read_src_t *s) {
  return !s || s->bad;
}

int
read_src_pread(read_src_t *s, uint64_t off, void *buf, size_t len,
               size_t *got) {
  if(got) *got = 0;
  if(!s) {
    errno = EINVAL;
    return -1;
  }
  if(s->ts) {
    size_t n = 0;

    if(transfer_read(s->ts, off, buf, len, &n) != MANGO_OK) {
      rs_set_msg(s, transfer_last_error(s->ts));
      s->bad = 1;
      return -1;
    }
    if(got) *got = n;
    return 0;
  }
  if(s->fd < 0) {
    s->err = EBADF;
    return -1;
  }
  {
    ssize_t n = pread(s->fd, buf, len, (off_t)off);

    if(n < 0) {
      s->err = errno;
      return -1;
    }
    if(got) *got = (size_t)n;
  }
  return 0;
}

uint64_t
read_src_size(read_src_t *s) {
  return s ? s->size : 0;
}

int
read_src_is_nas(read_src_t *s) {
  return s && s->ts != NULL;
}

const char *
read_src_error(read_src_t *s) {
  if(!s) return "out of memory";
  if(s->msg[0]) return s->msg;
  if(s->err) return strerror(s->err);
  return "read failed";
}

void
read_src_close(read_src_t *s) {
  if(!s) return;
  if(s->ts) {
    transfer_close(s->ts);
    s->ts = NULL;
  }
  if(s->fd >= 0) {
    close(s->fd);
    s->fd = -1;
  }
  free(s);
}
