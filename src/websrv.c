#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "asset.h"
#include "filemgr.h"
#include "filemgr_internal.h"
#include "json_util.h"
#include "pkg_stream.h"
#include "websrv.h"

#define REQUEST_BODY_MAX (4 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_LIMIT (8 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_INCREMENT (2 * 1024 * 1024)
#define HTTP_SOCKET_RCVBUF_SIZE (4 * 1024 * 1024)
#define HTTP_SOCKET_SNDBUF_SIZE (4 * 1024 * 1024)

static volatile sig_atomic_t g_stop_requested;
static int g_listen_fd = -1;
unsigned short g_listen_port = 0;

static void
websrv_tune_connection_socket(int fd) {
  const int sndbuf = HTTP_SOCKET_SNDBUF_SIZE;
  const int nodelay = 1;

  /* OrbisOS HTTP sockets need an explicit send buffer to fill a GbE link. */
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

typedef struct request_context {
  char *body;
  size_t size;
  int too_large;
  int upload_stream;
  void *upload_ctx;
  char url[160];
} request_context_t;

/* ---- request ledger ---------------------------------------------------
 * A small ring of the most recent requests. When the process dies mid-request
 * on the console there is no core to read, so the last entry WITHOUT a matching
 * done record IS the culprit. GET /api/diag returns the ring.
 *
 * The console's WebKit is the only place the "opens the page, then dies on one
 * click" class of bug shows up; this is how it gets localised. */
#define DIAG_SLOTS 64

typedef struct diag_slot {
  unsigned long seq;
  int used;
  char method[8];
  char url[160];
  int done;          /* completion callback ran */
  int toe;           /* MHD_RequestTerminationCode */
} diag_slot_t;

static diag_slot_t g_diag[DIAG_SLOTS];
static unsigned long g_diag_seq;
static pthread_mutex_t g_diag_lock = PTHREAD_MUTEX_INITIALIZER;

static void
diag_record_request(const char *method, const char *url) {
  diag_slot_t *s;
  unsigned long seq;

  pthread_mutex_lock(&g_diag_lock);
  seq = ++g_diag_seq;                 /* slot index must match the reader's n */
  s = &g_diag[seq % DIAG_SLOTS];
  memset(s, 0, sizeof(*s));
  s->seq = seq;
  s->used = 1;
  snprintf(s->method, sizeof(s->method), "%s", method ? method : "");
  snprintf(s->url, sizeof(s->url), "%s", url ? url : "");
  pthread_mutex_unlock(&g_diag_lock);
}

static void
diag_record_done(const char *url, int toe) {
  diag_slot_t *s;

  pthread_mutex_lock(&g_diag_lock);
  for(unsigned i = 0; i < DIAG_SLOTS; i++) {
    s = &g_diag[i];
    if(s->used && !s->done && !strcmp(s->url, url ? url : "")) {
      s->done = 1;
      s->toe = toe;
      break;
    }
  }
  pthread_mutex_unlock(&g_diag_lock);
}

static enum MHD_Result
websrv_diag_response(struct MHD_Connection *conn) {
  strbuf_t b = {0};
  unsigned long start = g_diag_seq > DIAG_SLOTS ? g_diag_seq - DIAG_SLOTS + 1 : 1;

  strbuf_printf(&b, "{\"ok\":true,\"count\":%lu,\"last_seq\":%lu,\"requests\":[",
                g_diag_seq, g_diag_seq);
  {
    int first = 1;
    pthread_mutex_lock(&g_diag_lock);
    for(unsigned long n = start; n <= g_diag_seq; n++) {
      diag_slot_t *s = &g_diag[n % DIAG_SLOTS];
      if(!s->used || s->seq != n) continue;
      if(!first) strbuf_append(&b, ",");
      first = 0;
      strbuf_printf(&b, "{\"seq\":%lu,\"method\":", s->seq);
      json_escape(&b, s->method);
      strbuf_append(&b, ",\"url\":");
      json_escape(&b, s->url);
      strbuf_printf(&b, ",\"done\":%s,\"toe\":%d}",
                    s->done ? "true" : "false", s->toe);
    }
    pthread_mutex_unlock(&g_diag_lock);
  }
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
websrv_body_too_large(struct MHD_Connection *conn) {
  static const char json[] =
    "{\"ok\":false,\"error\":\"request body is too large\","
    "\"error_code\":\"request_body_too_large\",\"error_arg\":\"\"}";
  struct MHD_Response *resp =
    MHD_create_response_from_buffer(sizeof(json) - 1, (void *)json,
                                    MHD_RESPMEM_PERSISTENT);
  enum MHD_Result ret;

  if(!resp) {
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  ret = websrv_queue_response(conn, MHD_HTTP_CONTENT_TOO_LARGE, resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
                      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  return MHD_queue_response(conn, status, resp);
}

void
websrv_stop(void) {
  g_stop_requested = 1;
  if(g_listen_fd >= 0) {
    shutdown(g_listen_fd, SHUT_RDWR);
  }
}

int
websrv_stop_requested(void) {
  return g_stop_requested;
}

static enum MHD_Result
websrv_on_request(void *cls, struct MHD_Connection *conn, const char *url,
                  const char *method, const char *version,
                  const char *upload_data, size_t *upload_data_size,
                  void **con_cls) {
  request_context_t *ctx = *con_cls;
  (void)cls;
  (void)version;

  if(strcmp(method, MHD_HTTP_METHOD_GET) &&
     strcmp(method, MHD_HTTP_METHOD_POST) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return MHD_NO;
  }

  if(!ctx) {
    if(!(ctx = calloc(1, sizeof(*ctx)))) {
      return MHD_NO;
    }
    ctx->upload_stream = !strcmp(url, "/api/upload-file") &&
                         !strcmp(method, MHD_HTTP_METHOD_POST);
    snprintf(ctx->url, sizeof(ctx->url), "%s", url);
    diag_record_request(method, url);
    *con_cls = ctx;
    return MHD_YES;
  }

  if(*upload_data_size) {
    size_t chunk_size = *upload_data_size;

    if(ctx->upload_stream) {
      if(!ctx->upload_ctx && filemgr_upload_begin(conn, &ctx->upload_ctx)) {
        ctx->too_large = 1;
      }
      if(ctx->upload_ctx && filemgr_upload_data(ctx->upload_ctx, upload_data,
                                                chunk_size)) {
        ctx->too_large = 1;
      }
      *upload_data_size = 0;
      return MHD_YES;
    }

    if(chunk_size > REQUEST_BODY_MAX - ctx->size) {
      ctx->too_large = 1;
    } else if(!ctx->too_large) {
      char *body = realloc(ctx->body, ctx->size + chunk_size + 1);
      if(!body) {
        return MHD_NO;
      }
      ctx->body = body;
      memcpy(ctx->body + ctx->size, upload_data, chunk_size);
      ctx->size += chunk_size;
      ctx->body[ctx->size] = 0;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  if(ctx->too_large) {
    return ctx->upload_stream ?
      filemgr_upload_finish(conn, ctx->upload_ctx) :
      websrv_body_too_large(conn);
  }

  if(ctx->upload_stream) {
    if(!ctx->upload_ctx && filemgr_upload_begin(conn, &ctx->upload_ctx)) {
      return filemgr_upload_finish(conn, ctx->upload_ctx);
    }
    return filemgr_upload_finish(conn, ctx->upload_ctx);
  }

  if(!strcmp(url, "/api/diag")) {
    return websrv_diag_response(conn);
  }
  if(!strncmp(url, "/api/", 5)) {
    return filemgr_api_request(conn, url, method, ctx->body, ctx->size);
  }
  if(!strcmp(url, "/fs")) {
    return filemgr_fs_request(conn);
  }
  if(!strcmp(url, "/") || !url[0]) {
    return asset_request(conn, "/index.html");
  }
  if(!strcmp(url, "/favicon.ico")) {
    return asset_request(conn, "/icon0.png");
  }
  return asset_request(conn, url);
}

static void
websrv_on_completed(void *cls, struct MHD_Connection *connection,
                    void **con_cls, enum MHD_RequestTerminationCode toe) {
  (void)cls;
  (void)connection;
  (void)toe;
  request_context_t *ctx = *con_cls;

  if(ctx) {
    diag_record_done(ctx->url, (int)toe);
    if(ctx->upload_ctx) {
      filemgr_upload_free(ctx->upload_ctx);
    }
    free(ctx->body);
    free(ctx);
  }
  *con_cls = NULL;
}

int
websrv_listen(unsigned short port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  signal(SIGPIPE, SIG_IGN);

  if((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }
  {
    const int rcvbuf = HTTP_SOCKET_RCVBUF_SIZE;

    /* Set before listen so accepted PS5 sockets inherit the larger window. */
    (void)setsockopt(srvfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
    perror("bind");
    close(srvfd);
    return -1;
  }
  if(listen(srvfd, 16)) {
    perror("listen");
    close(srvfd);
    return -1;
  }
  g_stop_requested = 0;
  g_listen_fd = srvfd;
  g_listen_port = port;

  if(!(httpd = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ITC |
                                MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG |
                                MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_TURBO,
                                0, NULL, NULL, &websrv_on_request, NULL,
                                MHD_OPTION_CONNECTION_MEMORY_LIMIT,
                                (size_t)HTTP_CONNECTION_MEMORY_LIMIT,
                                MHD_OPTION_CONNECTION_MEMORY_INCREMENT,
                                (size_t)HTTP_CONNECTION_MEMORY_INCREMENT,
                                MHD_OPTION_NOTIFY_COMPLETED,
                                &websrv_on_completed, NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    return -1;
  }

  while(!g_stop_requested) {
    fd_set rfds;
    struct timeval tv;
    int ready;

    /* The console's own installer pulls the package through the loopback stream
     * server in the background, so something has to keep it fed. This accept
     * loop is the only place that happens — src/pkg_stream.c is deliberately
     * single-threaded and pumped, never threaded (see its header). The call is
     * a no-op until a package is published. */
    pkg_stream_tick();

    /* ⚠️ A bare blocking accept() would starve the stream: the pump above only
     * runs between connections. A 5 ms poll costs 200 wakeups/s and still
     * leaves the accept path immediate (select() returns as soon as a client
     * connects). */
    FD_ZERO(&rfds);
    FD_SET(srvfd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 5000;
    ready = select(srvfd + 1, &rfds, NULL, NULL, &tv);
    if(ready < 0) {
      if(errno == EINTR) continue;
      if(!g_stop_requested) perror("select");
      break;
    }
    if(ready == 0) continue;

    addr_len = sizeof(client_addr);
    if((connfd = accept(srvfd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
      if(!g_stop_requested) perror("accept");
      break;
    }
    websrv_tune_connection_socket(connfd);
    if(MHD_add_connection(httpd, connfd, (struct sockaddr *)&client_addr,
                          addr_len) != MHD_YES) {
      perror("MHD_add_connection");
      close(connfd);
      break;
    }
  }

  MHD_stop_daemon(httpd);
  g_listen_fd = -1;
  return close(srvfd);
}
