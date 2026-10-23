#include "mango_log.h"

#ifdef __SCE__
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* glibc marks write() with __attribute__((warn_unused_result)) and -Werror
 * rejects the (void) cast, so the writes go through this loop, which also
 * finishes short writes. Same helper as main.c's crash handler — both write to
 * the same file, one line at a time, and neither can afford a partial line. */
static void
ledger_write(int fd, const char *s, size_t n) {
  while(n > 0) {
    ssize_t w = write(fd, s, n);

    if(w <= 0) {
      return;
    }
    s += w;
    n -= (size_t)w;
  }
}

void mango_log(const char *fmt, ...) {
  char buf[512];
  char hdr[96];
  va_list ap;
  int n;
  int h;
  int fd;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if(n < 0) {
    return;
  }
  if((size_t)n >= sizeof(buf)) {
    n = (int)sizeof(buf) - 1;
  }
  fd = open(MANGO_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if(fd < 0) {
    return;
  }
  /* 每行自带 pid：一次「关了又起来」会留下两段不同 pid 的记录，这是分辨
     "真的重启过" 还是 "压根没关掉" 的唯一依据。 */
  h = snprintf(hdr, sizeof(hdr), "\n[log %ld pid=%d] ", (long)time(NULL),
               (int)getpid());
  if(h > 0) {
    ledger_write(fd, hdr, (size_t)h);
  }
  ledger_write(fd, buf, (size_t)n);
  ledger_write(fd, "\n", 1);
  (void)close(fd);
}
#else /* !__SCE__ */
void mango_log(const char *fmt, ...) {
  (void)fmt;
}
#endif
