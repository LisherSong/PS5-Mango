/* Loopback HTTP range server for the console's own package installer.
 * Rationale, protocol shape and the three disciplines: see pkg_stream.h.
 *
 * This file is deliberately host-buildable (MinGW + POSIX): it is the one part
 * of the install path that can be proven correct on a PC, via
 * tests/test_pkg_stream.c, without a console. */
#include "pkg_stream.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
typedef SOCKET         ps_sock_t;
typedef int            ps_socklen_t;
#  define PS_BAD        INVALID_SOCKET
#  define ps_close      closesocket
#  define ps_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#  define ps_fd_close   _close
#  define ps_fd_read(f, b, n)   ((int)_read((f), (b), (unsigned)(n)))
#  define ps_fd_seek(f, o)      ((long long)_lseeki64((f), (long long)(o), SEEK_SET))
#  define PS_MSG_NOSIGNAL 0
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
typedef int            ps_sock_t;
typedef socklen_t      ps_socklen_t;
#  define PS_BAD        (-1)
#  define ps_close      close
#  define ps_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#  define ps_fd_close   close
#  define ps_fd_read(f, b, n)   ((int)read((f), (b), (size_t)(n)))
#  define ps_fd_seek(f, o)      ((long long)lseek((f), (off_t)(o), SEEK_SET))
#  ifdef MSG_NOSIGNAL
#    define PS_MSG_NOSIGNAL MSG_NOSIGNAL
#  else
#    define PS_MSG_NOSIGNAL 0
#  endif
#endif

/* The header phase opens several short connections in a burst, and the bulk
 * phase runs two in parallel — eight slots covers both with headroom. A ninth
 * is refused rather than allowed to grow the table without bound. */
#define PS_MAX_CONNS 8
#define PS_REQ_MAX   4096
#define PS_HDR_MAX   768
#define PS_IO_CHUNK  (128u * 1024)
/* Per-tick ceiling on streamed bytes. Keeps one tick short enough that the
 * accept loop and the rest of the task driver still get serviced. */
#define PS_TICK_BUDGET (4u * 1024 * 1024)
/* The port the reference implementation pins. Tried first so our behaviour
 * matches the one known to work, then ephemeral — the uri always carries the
 * port we actually bound, so a fallback can never hand out a wrong address. */
#define PS_PREFERRED_PORT 18841

typedef struct {
  ps_sock_t fd;
  int       used;

  /* request being accumulated */
  char      req[PS_REQ_MAX + 1];
  size_t    reqlen;
  size_t    scanned;      /* how far the CRLFCRLF search has already run */
  int       ready;        /* request parsed, response prepared */

  /* response being drained */
  char      hdr[PS_HDR_MAX];
  size_t    hdrlen, hdrsent;
  uint64_t  body_pos;     /* absolute offset of the next body byte */
  uint64_t  body_left;
  int       close_after;
} ps_conn_t;

typedef struct {
  ps_sock_t lfd;
  uint16_t  port;
  int       fd;           /* open payload descriptor, -1 when none */
  char      session[128];
  uint64_t  size;
  int       published;
  int       started;
  ps_conn_t conns[PS_MAX_CONNS];
} ps_state_t;

/* One published payload at a time, process-wide. The install session is a
 * global singleton on the console — two concurrent streams would race it —
 * so a second instance would buy nothing and cost a whole class of bug. */
static ps_state_t g_ps;

/* --- small portability shims ---------------------------------------------- */

static void
ps_set_nonblock(ps_sock_t fd) {
#ifdef _WIN32
  u_long one = 1;
  ioctlsocket(fd, FIONBIO, &one);
#else
  int fl = fcntl(fd, F_GETFL, 0);
  if(fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int
ps_recv(ps_sock_t fd, void *buf, size_t n) {
#ifdef _WIN32
  return recv(fd, (char *)buf, (int)n, 0);
#else
  return (int)recv(fd, buf, n, 0);
#endif
}

static int
ps_send(ps_sock_t fd, const void *buf, size_t n) {
#ifdef _WIN32
  return send(fd, (const char *)buf, (int)n, 0);
#else
  return (int)send(fd, buf, n, PS_MSG_NOSIGNAL);
#endif
}

static int
ps_fd_open(const char *path) {
#ifdef _WIN32
  return _open(path, _O_RDONLY | _O_BINARY);
#else
  return open(path, O_RDONLY);
#endif
}

/* --- header text helpers -------------------------------------------------- */
/* The console's header casing and ordering are not part of the contract, so
 * every lookup here is case-insensitive and order-independent. */

static const char *
ci_strstr(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  if(!nl || !hay) return NULL;
  for(; *hay; hay++) {
    size_t i = 0;
    while(i < nl && hay[i] &&
          tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
    if(i == nl) return hay;
  }
  return NULL;
}

static const char *
hdr_value(const char *req, const char *name) {
  const char *p = ci_strstr(req, name);
  if(!p) return NULL;
  p += strlen(name);
  while(*p == ' ' || *p == '\t') p++;
  return p;
}

/* --- request parsing ------------------------------------------------------ */

static void
conn_reset(ps_conn_t *c) {
  c->reqlen = 0;
  c->scanned = 0;
  c->ready = 0;
  c->hdrlen = c->hdrsent = 0;
  c->body_pos = c->body_left = 0;
  c->close_after = 0;
}

static void
conn_shutdown(ps_conn_t *c) {
  if(c->fd != PS_BAD) ps_close(c->fd);
  c->fd = PS_BAD;
  c->used = 0;
  conn_reset(c);
}

/* Is the request head fully in? Returns 1 once CRLFCRLF has been seen.
 * `scanned` makes the repeated search O(1) amortised — a 4 KiB head would
 * otherwise be re-scanned per received byte. */
static int
req_complete(ps_conn_t *c) {
  size_t start = c->scanned > 3 ? c->scanned - 3 : 0;

  if(c->reqlen < 4) { c->scanned = c->reqlen; return 0; }
  for(size_t i = start; i + 4 <= c->reqlen; i++)
    if(!memcmp(c->req + i, "\r\n\r\n", 4)) {
      c->scanned = i + 4;
      return 1;
    }
  c->scanned = c->reqlen;
  return 0;
}

static void
resp_404(ps_conn_t *c, int keep) {
  c->hdrlen = (size_t)snprintf(c->hdr, sizeof(c->hdr),
      "HTTP/1.1 404 Not Found\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 0\r\n"
      "Connection: %s\r\n"
      "\r\n", keep ? "keep-alive" : "close");
  if(c->hdrlen >= sizeof(c->hdr)) c->hdrlen = sizeof(c->hdr) - 1;
  c->hdrsent = 0;
  c->body_pos = c->body_left = 0;
  c->close_after = !keep;
  c->ready = 1;
}

/* "bytes=a-b" | "bytes=a-" | "bytes=-n" -> inclusive [*from, *to]. */
static int
parse_range(const char *v, uint64_t total, uint64_t *from, uint64_t *to) {
  const char *p = v;
  char       *end;

  if(!v || !total) return 0;
  if(strncmp(p, "bytes", 5)) return 0;
  p += 5;
  if(*p == '=') p++;
  while(*p == ' ' || *p == '\t') p++;

  if(*p == '-') {                          /* suffix range: the last n bytes */
    uint64_t n = strtoull(p + 1, &end, 10);
    if(end == p + 1 || n == 0) return 0;
    if(n > total) n = total;
    *from = total - n;
    *to   = total - 1;
    return 1;
  }

  {
    uint64_t a = strtoull(p, &end, 10);
    if(end == p) return 0;
    p = end;
    if(*p != '-') return 0;
    p++;
    if(a >= total) return 0;               /* unsatisfiable */
    if(!isdigit((unsigned char)*p)) {      /* open-ended: a- */
      *from = a;
      *to   = total - 1;
      return 1;
    }
    {
      uint64_t b = strtoull(p, &end, 10);
      if(end == p) return 0;
      if(b >= total) b = total - 1;
      if(b < a) return 0;
      *from = a;
      *to   = b;
      return 1;
    }
  }
}

static int
resp_serve(ps_conn_t *c, uint64_t from, uint64_t to, int keep) {
  uint64_t len = to - from + 1;

  c->hdrlen = (size_t)snprintf(c->hdr, sizeof(c->hdr),
      "HTTP/1.1 206 Partial Content\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Length: %llu\r\n"
      "Content-Range: bytes %llu-%llu/%llu\r\n"
      "Accept-Ranges: bytes\r\n"
      "Connection: %s\r\n"
      "\r\n",
      (unsigned long long)len, (unsigned long long)from,
      (unsigned long long)to, (unsigned long long)g_ps.size,
      keep ? "keep-alive" : "close");
  if(c->hdrlen >= sizeof(c->hdr)) return -1;
  c->hdrsent  = 0;
  c->body_pos = from;
  c->body_left = len;
  c->close_after = !keep;
  c->ready = 1;
  return 0;
}

static void
conn_parse(ps_conn_t *c) {
  char        path[512];
  const char *p, *sp;
  const char *rv;
  uint64_t    from = 0, to = 0;
  size_t      n;
  int         keep = 1;
  char        want[256];

  c->req[c->reqlen] = 0;

  /* Only GET is ever used, and a HEAD would produce no useful answer here, so
   * anything else is simply not found. */
  sp = strchr(c->req, ' ');
  if(!sp || (size_t)(sp - c->req) != 3 || memcmp(c->req, "GET", 3)) {
    resp_404(c, 0);
    return;
  }
  p  = sp + 1;
  sp = strchr(p, ' ');
  if(!sp) { resp_404(c, 0); return; }
  n = (size_t)(sp - p);
  if(n >= sizeof(path)) n = sizeof(path) - 1;
  memcpy(path, p, n);
  path[n] = 0;

  /* The console appends ?product=0287&serverIpAddr=127.0.0.1&r=00000000 to
   * EVERY request, so the query has to come off before the route is matched. */
  {
    char *q = strchr(path, '?');
    if(q) *q = 0;
  }

  /* Honour an explicit close even though the console always asks to keep the
   * connection alive — a probe client that says close must not be kept. */
  if(ci_strstr(c->req, "connection: close")) keep = 0;

  if(!g_ps.published || !g_ps.session[0]) { resp_404(c, keep); return; }

  snprintf(want, sizeof(want), "/stream/install/%s.pkg", g_ps.session);
  if(strcmp(path, want)) {
    /* Everything else — in practice the Range-less "<content_id>.crc" sidecar
     * probe — MUST be a 404. Answering 206 with PKG bytes there corrupts the
     * installer's chunk checks and aborts the whole install. */
    resp_404(c, keep);
    return;
  }

  rv = hdr_value(c->req, "range:");
  /* A malformed range is ignored rather than rejected: the console never sends
   * one, and a full 200 is always a valid answer. */
  if(!rv || !parse_range(rv, g_ps.size, &from, &to))
    resp_serve(c, 0, g_ps.size - 1, keep);
  else
    resp_serve(c, from, to, keep);
}

/* --- per-connection I/O --------------------------------------------------- */

static int
conn_read(ps_conn_t *c) {
  int work = 0;

  for(;;) {
    int n;

    if(c->reqlen >= PS_REQ_MAX) {   /* head far larger than anything real */
      resp_404(c, 0);
      return work + 1;
    }
    n = ps_recv(c->fd, c->req + c->reqlen, PS_REQ_MAX - c->reqlen);
    if(n > 0) {
      c->reqlen += (size_t)n;
      work++;
      if(req_complete(c)) {
        conn_parse(c);
        return work;
      }
      continue;
    }
    if(n == 0) {                    /* peer closed */
      conn_shutdown(c);
      return work;
    }
    if(ps_wouldblock()) return work;
    conn_shutdown(c);
    return work;
  }
}

static int
conn_send(ps_conn_t *c, unsigned *budget) {
  int work = 0;

  while(c->hdrsent < c->hdrlen) {
    int n = ps_send(c->fd, c->hdr + c->hdrsent, c->hdrlen - c->hdrsent);
    if(n > 0) { c->hdrsent += (size_t)n; work++; continue; }
    if(n < 0 && ps_wouldblock()) return work;
    conn_shutdown(c);
    return work;
  }

  while(c->body_left && *budget) {
    unsigned char buf[PS_IO_CHUNK];
    size_t        want = c->body_left < sizeof(buf) ? (size_t)c->body_left
                                                    : sizeof(buf);
    int           got, n;

    if(want > *budget) want = *budget;
    if(ps_fd_seek(g_ps.fd, (long long)c->body_pos) < 0) { conn_shutdown(c); return work; }
    got = ps_fd_read(g_ps.fd, buf, want);
    if(got <= 0) { conn_shutdown(c); return work; }   /* truncated under us */
    n = ps_send(c->fd, buf, (size_t)got);
    if(n > 0) {
      /* Advance by exactly what went out, so a short send is simply re-read
       * from the new offset on the next pass — no buffered remainder needed. */
      c->body_pos  += (uint64_t)n;
      c->body_left -= (uint64_t)n;
      *budget = ((unsigned)n >= *budget) ? 0 : *budget - (unsigned)n;
      work++;
      continue;
    }
    if(n < 0 && ps_wouldblock()) return work;
    conn_shutdown(c);
    return work;
  }

  if(!c->body_left && c->hdrsent == c->hdrlen) {
    if(c->close_after) conn_shutdown(c);
    else               conn_reset(c);   /* keep-alive: next request */
  }
  return work;
}

/* --- public API ----------------------------------------------------------- */

int
pkg_stream_start(void) {
  static const unsigned short try_ports[] = { PS_PREFERRED_PORT, 0 };

  if(g_ps.started) return 0;

#ifdef _WIN32
  {
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
  }
#endif

  g_ps.lfd = PS_BAD;
  g_ps.fd  = -1;
  for(int i = 0; i < PS_MAX_CONNS; i++) g_ps.conns[i].fd = PS_BAD;

  for(size_t t = 0; t < sizeof(try_ports) / sizeof(try_ports[0]); t++) {
    struct sockaddr_in sa;
    ps_socklen_t       slen;
    ps_sock_t          fd;
    int                one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == PS_BAD) continue;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(try_ports[t]);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* loopback ONLY, never 0.0.0.0 */

    if(bind(fd, (struct sockaddr *)&sa, (ps_socklen_t)sizeof(sa)) != 0) {
      ps_close(fd);
      continue;
    }
    if(listen(fd, 16) != 0) { ps_close(fd); continue; }

    slen = (ps_socklen_t)sizeof(sa);
    if(getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) { ps_close(fd); continue; }

    ps_set_nonblock(fd);
    g_ps.lfd     = fd;
    g_ps.port    = ntohs(sa.sin_port);
    g_ps.started = 1;
    return 0;
  }

#ifdef _WIN32
  WSACleanup();
#endif
  return -1;
}

uint16_t
pkg_stream_port(void) {
  return g_ps.port;
}

int
pkg_stream_running(void) {
  return g_ps.started && g_ps.lfd != PS_BAD;
}

int
pkg_stream_publishing(void) {
  return g_ps.published;
}

int
pkg_stream_publish(const char *session, const char *path, uint64_t size) {
  struct stat st;
  unsigned char probe;

  if(!session || !*session || !path || !*path) return -1;
  if(!g_ps.started && pkg_stream_start() != 0) return -1;

  g_ps.published  = 0;
  g_ps.session[0] = 0;
  if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }

  g_ps.fd = ps_fd_open(path);
  if(g_ps.fd < 0) return -1;

  if(size == 0) {
    if(stat(path, &st) != 0 || st.st_size <= 0) {
      ps_fd_close(g_ps.fd);
      g_ps.fd = -1;
      return -1;
    }
    size = (uint64_t)st.st_size;
  }

  /* Touch the first byte now. An unreadable payload must fail HERE, where the
   * caller can refuse to start an install, and not as a mid-stream stall the
   * console would surface as a generic, unattributable install error. */
  if(ps_fd_seek(g_ps.fd, 0) < 0 || ps_fd_read(g_ps.fd, &probe, 1) != 1) {
    ps_fd_close(g_ps.fd);
    g_ps.fd = -1;
    return -1;
  }

  snprintf(g_ps.session, sizeof(g_ps.session), "%s", session);
  g_ps.size      = size;
  g_ps.published = 1;
  return 0;
}

void
pkg_stream_unpublish(void) {
  g_ps.published  = 0;
  g_ps.session[0] = 0;
  g_ps.size       = 0;
  if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }
}

void
pkg_stream_make_session(char *out, size_t n) {
  static unsigned seq;

  if(!out || !n) return;
  /* Time-based, not a bare counter: the console remembers recently used stream
   * URLs across payload restarts, so a name that merely restarts at 1 with the
   * process can be rejected as a repeat. */
  snprintf(out, n, "package-%lu-%u", (unsigned long)time(NULL), ++seq);
}

int
pkg_stream_url(char *out, size_t n, const char *session) {
  if(!out || !n || !session || !*session || !g_ps.port) return -1;
  if((size_t)snprintf(out, n, "http://127.0.0.1:%u/stream/install/%s.pkg",
                      (unsigned)g_ps.port, session) >= n)
    return -1;
  return 0;
}

int
pkg_stream_tick(void) {
  unsigned budget = PS_TICK_BUDGET;
  int      work   = 0;

  if(!pkg_stream_running()) return 0;

  /* Drain the accept backlog: the header phase opens several short-lived
   * connections at once, and taking only one per tick would serialise them. */
  for(;;) {
    ps_sock_t fd = accept(g_ps.lfd, NULL, NULL);
    int       slot = -1;

    if(fd == PS_BAD) break;
    ps_set_nonblock(fd);
    for(int i = 0; i < PS_MAX_CONNS; i++)
      if(!g_ps.conns[i].used) { slot = i; break; }
    if(slot < 0) { ps_close(fd); continue; }   /* refuse rather than grow */
    memset(&g_ps.conns[slot], 0, sizeof(g_ps.conns[slot]));
    g_ps.conns[slot].fd   = fd;
    g_ps.conns[slot].used = 1;
    work++;
  }

  for(int i = 0; i < PS_MAX_CONNS; i++) {
    ps_conn_t *c = &g_ps.conns[i];
    if(!c->used) continue;
    if(!c->ready) work += conn_read(c);
    else          work += conn_send(c, &budget);
  }
  return work;
}

void
pkg_stream_stop(void) {
  for(int i = 0; i < PS_MAX_CONNS; i++)
    if(g_ps.conns[i].used) conn_shutdown(&g_ps.conns[i]);
  if(g_ps.lfd != PS_BAD) { ps_close(g_ps.lfd); g_ps.lfd = PS_BAD; }
  pkg_stream_unpublish();
  g_ps.port    = 0;
  g_ps.started = 0;
}
