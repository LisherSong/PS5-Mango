/* Loopback HTTP range server for the console's own package installer.
 * Rationale, protocol shape and the disciplines: see pkg_stream.h.
 *
 * v1.0.25: this used to be a self-contained socket server bound next to the
 * main service. It is now a ROUTE inside the main libmicrohttpd daemon
 * (websrv.c registers /stream/install/), which is the shape the reference
 * implementation uses: the HTTP layer (status lines, keep-alive, HEAD, bad
 * Range) is MHD's job, and this file only owns the three things MHD cannot
 * know — which bytes to serve, from which source, and when to withdraw the
 * offer. That split also made the module testable in two layers:
 *   * ps_plan_request() is pure logic (method/url/range/loopback -> status +
 *     window) and is unit-tested exhaustively on the host;
 *   * the MHD glue is covered by hosttest against a real daemon.
 *
 * This file still builds on the host (MinGW + POSIX): .build/pkg_stream_test.c
 * includes it directly and proves the planning + lifecycle half. */
#include "pkg_stream.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "transfer.h"
#include "websrv.h"

/* This file also builds on the host (tests/, .build/pkg_stream_test.c), where
 * there is no console logger to write to. On the console the stream is the only
 * place that can say whether the installer ever fetched a byte — without it a
 * failed install has nothing to attribute it to. */
#ifdef __linux__
#  define ps_logf(...) \
     do { fprintf(stderr, "[pkg-stream] " __VA_ARGS__); fputc('\n', stderr); } while(0)
#else
#  include "mango_log.h"
#  define ps_logf(...) mango_log(__VA_ARGS__)
#endif

/* How long a published package is kept offerable after the LAST byte the
 * console asked for.
 *
 * WHY A TIMEOUT AT ALL: `sceAppInstUtilInstallByPackage` is asynchronous — it
 * returns "submitted" and the system installer pulls the package afterwards,
 * in the background. We deliberately do not poll GetInstallStatus (see
 * pkg_installer.c), so the moment the console is finished is not observable
 * from here. Something has to take the offer down eventually, or a published
 * source would outlive every install.
 *
 * WHY IT IS GENEROUS: unpublishing before the installer has read its last
 * chunk turns a finished download into a failed install, and the user sees
 * only a generic console error. 30 minutes of silence is far past any real
 * gap between two range requests (the bulk phase keeps two connections busy
 * continuously), and short enough that a forgotten session cannot linger.
 *
 * The clock is "last byte moved" (updated in the read callback), NOT "last
 * connection closed": MHD gives us no hook for the latter, and the bulk phase
 * is exactly the period where no new connection appears while gigabytes flow.
 */
#define PS_IDLE_UNPUBLISH_MS (30ull * 60ull * 1000ull)

/* Callback buffer sizes, matching the reference implementation's policy
 * (its transfer.h): a big payload gets a 2 MiB window so one MHD read maps to
 * a meaningful slice of the SMB pipeline; anything smaller does not pay for
 * the memory. */
#define PS_SMALL_FILE_MAX   (256ull * 1024 * 1024)   /* below: no pipeline */
#define PS_BUF_SMALL        (512u * 1024)
#define PS_BUF_PIPELINED    (2u * 1024 * 1024)

/* --- platform shims ------------------------------------------------------- */
/* The console ELF is POSIX; the host test binary also builds on MinGW. Only
 * the payload descriptor path needs shimming — the NAS path goes through
 * transfer_* which is itself portable. */

#ifdef _WIN32
#  include <io.h>
#  include <winsock2.h>
typedef int ps_fd_t;
#  define ps_fd_open(p)         _open((p), _O_RDONLY | _O_BINARY)
#  define ps_fd_close(f)        _close((f))
/* MinGW has no pread(): the local path takes the mutex there (see
 * ps_read_cb) so seek+read stays correct under MHD's per-connection threads. */
#  define PS_FD_NEEDS_LOCK 1
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
typedef int ps_fd_t;
#  define ps_fd_open(p)         open((p), O_RDONLY)
#  define ps_fd_close(f)        close((f))
#endif

/* Monotonic clock (ms). Wall time can jump (NTP, a user changing the clock),
 * and this only ever measures "how long since the last byte moved". Falls back
 * to second-resolution wall time where CLOCK_MONOTONIC is missing — coarse,
 * but still monotonic in direction, and the timeout is 30 minutes. */
static uint64_t
ps_mono_ms(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
  return (uint64_t)time(NULL) * 1000ull;
}

/* --- published payload state --------------------------------------------- */

typedef struct {
  ps_fd_t         fd;      /* open payload descriptor, -1 when served via src */
  transfer_src_t *src;     /* NAS payload source, NULL when served via fd */
  char            session[128];
  uint64_t        size;
  int             published;
  uint64_t        last_active;  /* mono_ms() of the last byte moved */
} ps_state_t;

/* One published payload at a time, process-wide. The install session is a
 * global singleton on the console — two concurrent streams would race it —
 * so a second instance would buy nothing and cost a whole class of bug. */
static ps_state_t g_ps;

/* The console's bulk phase runs TWO connections in parallel, and MHD gives
 * each its own thread (THREAD_PER_CONNECTION). Both can therefore sit inside
 * transfer_read() on the SAME transfer_src_t at the same time — and the SMB
 * read-ahead pipeline inside transfer.c is one shared state machine, not two.
 * Every access to g_ps.src (open/read/close) is therefore serialised here.
 *
 * The local-fd path needs no lock on POSIX (pread() carries its own offset),
 * but takes it on Win32 where the shim is seek+read. */
static pthread_mutex_t g_ps_lock = PTHREAD_MUTEX_INITIALIZER;

static void ps_src_reset_locked(void);

/* --- range parsing -------------------------------------------------------- */

/* "bytes=a-b" | "bytes=a-" | "bytes=-n" -> inclusive [*from, *to].
 * Returns 1 when a usable range was parsed, 0 when the header is absent or
 * malformed. Unsatisfiable (a >= total, b < a) also returns 0 — the caller
 * answers those with 416, which is a different answer from "no Range". */
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

/* --- request planning (pure logic, unit-tested) --------------------------- */

/* The whole HTTP decision, without MHD in the way. Order matters:
 *   1. loopback only — this route must never become a LAN file server;
 *   2. GET/HEAD only — the installer never sends anything else, and a probe
 *      with another method gets the method answer, not a 404;
 *   3. session match — "<content_id>.crc" sidecar probes and every unknown
 *      path are 404. Answering 206 there poisons chunk checks and aborts the
 *      install;
 *   4. three Range answers: none -> 200 (full body), usable -> 206,
 *      unusable -> 416 with the real total.
 * `pub` is a snapshot of the live published state (see ps_published_t); the
 * caller fills it under g_ps_lock so this stays a pure function of its
 * arguments and is unit-testable on the host. */
ps_plan_t
ps_plan_request(const char *method, const char *url, const char *range,
                int is_loopback, const ps_published_t *pub) {
  ps_plan_t plan;
  char      path[512];
  char      want[160];
  const char *p, *sp;
  size_t    n;
  uint64_t  from = 0, to = 0;

  memset(&plan, 0, sizeof(plan));

  if(!is_loopback) {
    plan.status = MHD_HTTP_FORBIDDEN;
    return plan;
  }
  if(!method || (strcmp(method, "GET") && strcmp(method, "HEAD"))) {
    plan.status = MHD_HTTP_METHOD_NOT_ALLOWED;
    return plan;
  }
  plan.head_only = !strcmp(method, "HEAD");

  /* url arrives without the query string (MHD strips it); strip again in case
   * a caller hands us the raw target. */
  if(!url) { plan.status = MHD_HTTP_NOT_FOUND; return plan; }
  p = url;
  sp = strchr(p, '?');
  n = sp ? (size_t)(sp - p) : strlen(p);
  if(n >= sizeof(path)) n = sizeof(path) - 1;
  memcpy(path, p, n);
  path[n] = 0;

  if(!pub || !pub->published || !pub->session[0]) {
    plan.status = MHD_HTTP_NOT_FOUND;
    return plan;
  }
  snprintf(want, sizeof(want), "/stream/install/%s.pkg", pub->session);
  if(strcmp(path, want)) {
    plan.status = MHD_HTTP_NOT_FOUND;
    return plan;
  }

  if(!range) {
    plan.status = MHD_HTTP_OK;
    plan.start  = 0;
    plan.length = pub->size;
    return plan;
  }
  if(!parse_range(range, pub->size, &from, &to)) {
    plan.status = MHD_HTTP_RANGE_NOT_SATISFIABLE;
    return plan;
  }
  plan.status = MHD_HTTP_PARTIAL_CONTENT;
  plan.start  = from;
  plan.length = to - from + 1;
  return plan;
}

/* --- per-connection streaming -------------------------------------------- */

typedef struct {
  int      is_nas;
  uint64_t start;    /* absolute payload offset of this window's first byte */
  uint64_t length;   /* window size; MHD clamps pos to it */
} ps_stream_t;

/* Read one slice of the published payload. `pos` is relative to the window
 * start (MHD's convention for callback responses). */
static ssize_t
ps_read_cb(void *cls, uint64_t pos, char *buf, size_t max) {
  ps_stream_t *s = cls;

  if(pos >= s->length) return MHD_CONTENT_READER_END_OF_STREAM;
  {
    uint64_t want64 = s->length - pos;
    size_t   want   = want64 < (uint64_t)max ? (size_t)want64 : max;

    if(!want) return MHD_CONTENT_READER_END_OF_STREAM;

    if(s->is_nas) {
      size_t got = 0;
      int    rc;

      /* One mutex for the whole SMB pipeline: two bulk connections otherwise
       * drive the same read-ahead state machine from two threads. The lock is
       * held for one transfer_read (a slice of the wire), not for the whole
       * window, so the two connections still interleave. */
      pthread_mutex_lock(&g_ps_lock);
      if(!g_ps.src) {
        pthread_mutex_unlock(&g_ps_lock);
        return MHD_CONTENT_READER_END_WITH_ERROR;
      }
      rc = transfer_read(g_ps.src, s->start + pos, buf, want, &got);
      if(rc == MANGO_OK && got) g_ps.last_active = ps_mono_ms();
      pthread_mutex_unlock(&g_ps_lock);
      if(rc != MANGO_OK || !got) return MHD_CONTENT_READER_END_WITH_ERROR;
      return (ssize_t)got;
    } else {
      ssize_t n;

      if(g_ps.fd < 0) return MHD_CONTENT_READER_END_WITH_ERROR;
#ifdef PS_FD_NEEDS_LOCK
      pthread_mutex_lock(&g_ps_lock);
      if(g_ps.fd < 0 ||
         _lseeki64(g_ps.fd, (long long)(s->start + pos), SEEK_SET) < 0) {
        pthread_mutex_unlock(&g_ps_lock);
        return MHD_CONTENT_READER_END_WITH_ERROR;
      }
      n = _read(g_ps.fd, buf, (unsigned)want);
      pthread_mutex_unlock(&g_ps_lock);
      if(n > 0) g_ps.last_active = ps_mono_ms();
#else
      n = pread(g_ps.fd, buf, want, (off_t)(s->start + pos));
      if(n > 0) g_ps.last_active = ps_mono_ms();
#endif
      if(n <= 0) return MHD_CONTENT_READER_END_WITH_ERROR;
      return n;
    }
  }
}

/* MHD calls this when the response is done (body sent, HEAD skipped, or the
 * connection dropped). The published source itself belongs to g_ps and is
 * taken down by pkg_stream_unpublish(), never by a single connection. */
static void
ps_close_cb(void *cls) {
  free(cls);
}

/* --- the route ------------------------------------------------------------ */

/* Answer a request for /stream/install/<session>.pkg. Registered by websrv.c;
 * see pkg_stream.h for why this is a route and not a second listener. */
enum MHD_Result
pkg_stream_request(struct MHD_Connection *conn, const char *url,
                   const char *method) {
  const union MHD_ConnectionInfo *info;
  const struct sockaddr_in       *peer;
  const char *range;
  ps_plan_t   plan;
  int         is_loopback = 0;
  ps_stream_t *stream;
  struct MHD_Response *resp;
  size_t      buf_size;
  char        content_range[100];
  enum MHD_Result ret;

  info = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  peer = info ? (const struct sockaddr_in *)info->client_addr : NULL;
  if(peer && peer->sin_family == AF_INET &&
     ntohl(peer->sin_addr.s_addr) == INADDR_LOOPBACK)
    is_loopback = 1;

  range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                      MHD_HTTP_HEADER_RANGE);
  {
    ps_published_t snap;
    pthread_mutex_lock(&g_ps_lock);
    snap.published = g_ps.published;
    memcpy(snap.session, g_ps.session, sizeof(snap.session));
    snap.size = g_ps.size;
    pthread_mutex_unlock(&g_ps_lock);
    plan = ps_plan_request(method, url, range, is_loopback, &snap);
  }

  if(plan.status != MHD_HTTP_OK && plan.status != MHD_HTTP_PARTIAL_CONTENT) {
    /* 403 / 405 / 404 / 416 — no body. 416 carries the real total so the
     * client can re-issue sanely instead of assuming a truncated source. */
    resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    if(!resp) return MHD_NO;
    if(plan.status == MHD_HTTP_RANGE_NOT_SATISFIABLE) {
      pthread_mutex_lock(&g_ps_lock);
      snprintf(content_range, sizeof(content_range), "bytes */%llu",
               (unsigned long long)g_ps.size);
      pthread_mutex_unlock(&g_ps_lock);
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE,
                              content_range);
      MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
    }
    ps_logf("%s %s -> %u", method ? method : "?", url ? url : "?", plan.status);
    ret = websrv_queue_response(conn, plan.status, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  stream = malloc(sizeof(*stream));
  if(!stream) return MHD_NO;

  pthread_mutex_lock(&g_ps_lock);
  stream->is_nas = g_ps.src != NULL;
  pthread_mutex_unlock(&g_ps_lock);
  stream->start  = plan.start;
  stream->length = plan.length;

  buf_size = g_ps.size >= PS_SMALL_FILE_MAX ? PS_BUF_PIPELINED : PS_BUF_SMALL;
  if(plan.length < (uint64_t)buf_size)
    buf_size = plan.length ? (size_t)plan.length : 1;

  resp = MHD_create_response_from_callback((uint64_t)plan.length, buf_size,
                                           ps_read_cb, stream, ps_close_cb);
  if(!resp) { free(stream); return MHD_NO; }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/octet-stream");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
  if(plan.status == MHD_HTTP_PARTIAL_CONTENT) {
    snprintf(content_range, sizeof(content_range), "bytes %llu-%llu/%llu",
             (unsigned long long)plan.start,
             (unsigned long long)(plan.start + plan.length - 1),
             (unsigned long long)g_ps.size);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE,
                            content_range);
  }

  ps_logf("%s %s -> %u [%llu,+%llu)", method, url, plan.status,
          (unsigned long long)plan.start, (unsigned long long)plan.length);
  ret = websrv_queue_response(conn, plan.status, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* --- published payload lifecycle ----------------------------------------- */

static void
ps_src_reset_locked(void) {
  if(g_ps.src) { transfer_close(g_ps.src); g_ps.src = NULL; }
}

void
pkg_stream_unpublish(void) {
  pthread_mutex_lock(&g_ps_lock);
  g_ps.published  = 0;
  g_ps.session[0] = 0;
  g_ps.size       = 0;
  if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }
  ps_src_reset_locked();
  pthread_mutex_unlock(&g_ps_lock);
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

/* `http://127.0.0.1:<main service port>/stream/install/<session>.pkg` — the
 * value that goes into pkg_metadata_t.uri. The port is the MAIN service's:
 * the stream is a route inside it, not a second listener. Returns 0 on
 * success. */
int
pkg_stream_url(char *out, size_t n, const char *session) {
  unsigned port;

  if(!out || !n || !session || !*session) return -1;
  port = websrv_service_port();
  if(!port) return -1;
  if((size_t)snprintf(out, n, "http://127.0.0.1:%u/stream/install/%s.pkg",
                      port, session) >= n)
    return -1;
  return 0;
}

int
pkg_stream_publish(const char *session, const char *path, uint64_t size) {
  struct stat st;
  unsigned char probe;

  if(!session || !*session || !path || !*path) return -1;

  pthread_mutex_lock(&g_ps_lock);
  g_ps.published  = 0;
  g_ps.session[0] = 0;
  if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }
  ps_src_reset_locked();

  g_ps.fd = ps_fd_open(path);
  if(g_ps.fd < 0) { pthread_mutex_unlock(&g_ps_lock); return -1; }

  if(size == 0) {
    if(stat(path, &st) != 0 || st.st_size <= 0) {
      ps_fd_close(g_ps.fd);
      g_ps.fd = -1;
      pthread_mutex_unlock(&g_ps_lock);
      return -1;
    }
    size = (uint64_t)st.st_size;
  }

  /* Touch the first byte now. An unreadable payload must fail HERE, where the
   * caller can refuse to start an install, and not as a mid-stream stall the
   * console would surface as a generic, unattributable install error. */
  if(pread(g_ps.fd, &probe, 1, 0) != 1) {
    ps_fd_close(g_ps.fd);
    g_ps.fd = -1;
    pthread_mutex_unlock(&g_ps_lock);
    return -1;
  }

  snprintf(g_ps.session, sizeof(g_ps.session), "%s", session);
  g_ps.size        = size;
  g_ps.published   = 1;
  g_ps.last_active = ps_mono_ms();
  pthread_mutex_unlock(&g_ps_lock);
  return 0;
}

/* Publish a NAS payload (smb://... / nfs://...). Same contract as publish():
 * on failure the state is exactly as before, so the caller can fall back. */
int
pkg_stream_publish_nas(const char *session, const char *url, uint64_t size) {
  unsigned char probe;
  size_t got = 0;
  char scheme[8] = "smb";
  const char *sep;

  if(!session || !*session || !url || !*url) return -1;
  sep = strstr(url, "://");
  if(sep && (size_t)(sep - url) < sizeof(scheme)) {
    memcpy(scheme, url, (size_t)(sep - url));
    scheme[sep - url] = 0;
  } else if(sep) {
    return -1;
  }
  if(!transfer_scheme_supported(scheme)) return -1;

  pthread_mutex_lock(&g_ps_lock);
  g_ps.published  = 0;
  g_ps.session[0] = 0;
  if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }
  ps_src_reset_locked();

  g_ps.src = transfer_open(scheme, sep ? sep + 3 : url);
  if(!g_ps.src) { pthread_mutex_unlock(&g_ps_lock); return -1; }
  if(!size) {
    uint64_t nas_size = 0;
    if(transfer_size(g_ps.src, &nas_size) != MANGO_OK || !nas_size) {
      ps_src_reset_locked();
      pthread_mutex_unlock(&g_ps_lock);
      return -1;
    }
    size = nas_size;
  }
  /* Same touch-the-first-byte discipline as publish(): a bad share, wrong
   * credentials or an unreadable file must fail here, not mid-install. */
  if(transfer_read(g_ps.src, 0, &probe, 1, &got) != MANGO_OK || got != 1) {
    ps_src_reset_locked();
    pthread_mutex_unlock(&g_ps_lock);
    return -1;
  }

  snprintf(g_ps.session, sizeof(g_ps.session), "%s", session);
  g_ps.size        = size;
  g_ps.published   = 1;
  g_ps.last_active = ps_mono_ms();
  pthread_mutex_unlock(&g_ps_lock);
  return 0;
}

/* Offer `path` under a fresh session and hand the caller the uri to put into
 * pkg_metadata_t.uri. One call instead of three so a caller cannot end up with
 * a published file whose uri it then failed to build (a half-published state
 * is invisible until an install silently stalls).
 *
 * Returns 0, or -1 with NO state change when the file is unreadable. The
 * caller is expected to fall back to path delivery — that fallback is the
 * whole reason this cannot be fatal. */
int
pkg_stream_prepare(const char *path, char *url_out, size_t url_out_n) {
  char       session[128];
  struct stat st;

  if(!path || !*path || !url_out || !url_out_n) return -1;
  /* stat first: publish() would open the file, and a directory or a zero-byte
   * file must not become a package the installer is pointed at. */
  if(stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return -1;

  pkg_stream_make_session(session, sizeof(session));
  if(pkg_stream_publish(session, path, (uint64_t)st.st_size) != 0) return -1;
  if(pkg_stream_url(url_out, url_out_n, session) != 0) {
    pkg_stream_unpublish();
    return -1;
  }
  return 0;
}

/* Same contract as prepare(), for a payload that lives on a NAS (an smb:// or
 * nfs:// URL). There is NO path-delivery fallback for a remote source — the
 * system installer cannot open smb:// — so a failure here must be reported,
 * never silently downgraded. */
int
pkg_stream_prepare_nas(const char *url, char *url_out, size_t url_out_n) {
  char session[128];

  if(!url || !*url || !url_out || !url_out_n) return -1;
  pkg_stream_make_session(session, sizeof(session));
  if(pkg_stream_publish_nas(session, url, 0) != 0) return -1;
  if(pkg_stream_url(url_out, url_out_n, session) != 0) {
    pkg_stream_unpublish();
    return -1;
  }
  return 0;
}

/* Service pending lifecycle work. Returns the number of work units
 * performed: a non-zero return means "there is more to do, do not sleep".
 * Since v1.0.25 the bytes are MHD's business (one thread per connection);
 * the only thing left for the main loop is the idle withdrawal below. */
int
pkg_stream_tick(void) {
  int work = 0;

  pthread_mutex_lock(&g_ps_lock);
  if(g_ps.published &&
     ps_mono_ms() - g_ps.last_active > PS_IDLE_UNPUBLISH_MS) {
    /* Nobody has asked for anything for half an hour: the install is over
     * (or was abandoned), and the offer must not outlive it. */
    if(g_ps.fd >= 0) { ps_fd_close(g_ps.fd); g_ps.fd = -1; }
    ps_src_reset_locked();
    g_ps.published  = 0;
    g_ps.session[0] = 0;
    g_ps.size       = 0;
    work++;
  }
  pthread_mutex_unlock(&g_ps_lock);
  return work;
}
