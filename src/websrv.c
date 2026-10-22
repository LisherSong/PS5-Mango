#include <errno.h>
#include <poll.h>
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
#include "mango_log.h"
#include "pkg_stream.h"
#include "save_domain.h"
#include "websrv.h"

#define REQUEST_BODY_MAX (4 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_LIMIT (8 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_INCREMENT (2 * 1024 * 1024)
#define HTTP_SOCKET_RCVBUF_SIZE (4 * 1024 * 1024)
#define HTTP_SOCKET_SNDBUF_SIZE (4 * 1024 * 1024)
/* 2026-10-07 真机反馈「网页打不开但进程还在，重启也一样」：
 * 线程每连接一 thread-per-connection 的模型下，连接数 = 线程数。浏览器的 keep-alive
 * 连接池 + 轮询 + 慢上传同时压过来时，新连接拿不到线程 → MHD_add_connection 失败 →
 * 旧代码 break 出 accept 循环 → MHD_stop_daemon → **服务从此不再应答，进程还活着**。
 * 两道闸：
 *   · CONNECTION_LIMIT 把并发连接（≈线程）钉在 48 —— 超限时 MHD 自己回 MHD_NO，
 *     我们丢弃那一个连接而不是整个 daemon（浏览器会自动重试）；
 *   · CONNECTION_TIMEOUT 120s 回收死连接/空闲 keep-alive，线程不会只增不减。 */
#define HTTP_CONNECTION_LIMIT 48
#define HTTP_CONNECTION_TIMEOUT_SEC 120

static volatile sig_atomic_t g_stop_requested;
static volatile sig_atomic_t g_parked;
static int g_listen_fd = -1;
unsigned short g_listen_port = 0;
/* The port the loopback PKG stream (/stream/install/, src/pkg_stream.c) hands
 * to the console installer. Since v1.0.25 the stream is a route inside this
 * daemon, so its uri carries THIS service's port — the installer then pulls
 * the payload over the same connection the web UI uses. */
unsigned
websrv_service_port(void) {
  return g_listen_port;
}
/* 连续 accept 失败计数：任何一次成功清零。到 1000 才允许拆 daemon 重来。 */
static unsigned g_accept_errors;

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
  size_t dropped;    /* too_large 之后继续涌进来的 body 字节（不再缓冲） */
  int upload_stream;
  void *upload_ctx;
  char url[160];
  unsigned long seq;            /* diag 台账序号：完成回调按它精确销账 */
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

static unsigned long
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
  return seq;
}

/* ⚠️ 按 seq 销账，不要按 URL 匹配。旧版是"找第一条 used && !done && url 相同"
   的槽 —— 而 GET /api/status、GET /api/tasks 这类同名请求是并发打进来的，
   后到的完成回调会把标记打到**更早的那个**槽上。于是台账里 "* = in flight"
   变得不可信：崩溃现场看到一个完成的请求，真正在飞的那条反被标成 done。
   2026-10-11 定位真机 SIGSEGV 时就卡在这里（三条记录全是 D，没有一条在飞），
   只能改用"最后一条请求"来推断。 */
static void
diag_record_done(unsigned long seq, int toe) {
  diag_slot_t *s;

  pthread_mutex_lock(&g_diag_lock);
  s = &g_diag[seq % DIAG_SLOTS];
  if(s->used && s->seq == seq && !s->done) {
    s->done = 1;
    s->toe = toe;
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

/* Crash-path snapshot of the request ring, called from the fatal-signal
 * handler in main.c. On a dying process this is the only place the culprit
 * request will ever be recorded: the last ring entry WITHOUT a done mark is
 * the request that was in flight when the signal hit. It try-locks because
 * the crash itself may have happened with g_diag_lock held — in that case the
 * "(lock held)" line IS the clue (crash inside a request handler), so the
 * dump is skipped rather than risk a deadlock on an already-doomed process.
 * Nothing here may allocate; the caller's buffer is all it gets. */
void
websrv_diag_recent(char *out, size_t n) {
  const unsigned long last = g_diag_seq;
  unsigned long start;
  size_t used = 0;

  if(n == 0) {
    return;
  }
  if(pthread_mutex_trylock(&g_diag_lock) != 0) {
    snprintf(out, n, "  (diag lock held at crash time: "
                     "the process died inside a request handler)\n");
    return;
  }

  if(last == 0) {
    snprintf(out, n, "  (no requests served yet)\n");
  } else {
    int w = snprintf(out, n, "last requests (* = in flight):\n");
    if(w > 0 && (size_t)w < n) {
      used = (size_t)w;
    }
    start = last > (DIAG_SLOTS - 1) ? last - (DIAG_SLOTS - 1) : 1;
    for(unsigned long k = start; k <= last; k++) {
      diag_slot_t *s = &g_diag[k % DIAG_SLOTS];
      if(used >= n - 1) break;
      if(!s->used || s->seq != k) continue;
      w = snprintf(out + used, n - used, "  %lu %c %s %s\n",
                   s->seq, s->done ? 'D' : '*', s->method, s->url);
      if(w <= 0) break;
      used += (size_t)w;
    }
  }
  pthread_mutex_unlock(&g_diag_lock);
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
  /* ★ 只兜底，**不覆盖**：调用方自己声明过 Cache-Control 就以它为准。
   *
   *   🪤 曾经这里无条件 add "no-store"，于是响应里出现**两个** Cache-Control
   *   头（`private, max-age=300` + `no-store`）。浏览器按最严的那条执行 ⇒
   *   no-store 完全禁止缓存 ⇒ 存档封面那条 max-age 等于没写：每次进存档页
   *   每张封面都要重跑一遍候选链（6 条路径 access + 跨槽 opendir + homebrew
   *   扫描），用户看到的就是"先占位图、过几秒才一张张冒出来"，开页预热也白做。
   *
   *   ★ 默认仍然是 no-store —— 绝大多数响应是动态的 JSON（任务快照、目录
   *   列表、存档信息），本来就不该被浏览器缓存。只有显式声明过的那几条
   *   （目前是 /api/save_mgr/icon 这类静态图片）才走缓存。 */
  if(!MHD_get_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL)) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  }
  return MHD_queue_response(conn, status, resp);
}

void
websrv_stop(void) {
  /* 台账：这一行是「关停真的走到了」的凭据。它只在 websrv_stop() 里写 ——
     没有它，说明关停请求压根没走到这一步（而不是进程退出后被重新注入）。 */
  mango_log("websrv_stop: arming stop flag (listen_fd=%d, port=%u)",
            g_listen_fd, g_listen_port);
  g_stop_requested = 1;
  if(g_listen_fd >= 0) {
    shutdown(g_listen_fd, SHUT_RDWR);
  }
}

int
websrv_stop_requested(void) {
  return g_stop_requested;
}

/* ---- 停车（park）-----------------------------------------------------------
 * 2026-10-11 用户报障的最后一环：「进程崩了 → payload 里进程还在 → 手动结束
 * 又自动出现 → 插件功能全不能用」。拼图如下：
 *   · 内核挂载楔死 ⇒ 线程不可中断睡眠，SIGKILL 带不走 ⇒ 进程"在"但不应答；
 *   · 注入器看到进程**死亡**（真崩溃或被杀）就重新注入 ⇒ "手动结束还会再出现"；
 *   · 重新注入的新实例被楔死残躯占着 2026（bind 不上）⇒ 落位 2027 ⇒
 *     用户敲 2026 打不开 ⇒ "插件功能都不能用"。
 * 代码侧改不了注入器（它检测的是进程生死，不是健康度），但可以换一个姿态：
 * **楔住后进程不退出、也不退出服务——停车**。close(listen fd) 释放 2026，
 * accept 循环退出、MHD 拆完在飞连接后 main 驻留睡眠。效果：
 *   · 进程活着 ⇒ 注入器不重注入 ⇒ 「别再自启」达成；
 *   · 用户手动结束它 ⇒ sleep 驻留不是不可中断睡眠，SIGKILL 干净带走；
 *     端口已释放 ⇒ 重注入的新实例正常接管 2026 ⇒ 「功能全废」消失。
 * parked 是**终身状态**：没有"解除停车"的路径 —— 内核状态已经不可信，唯一
 * 正确的恢复是换一个全新进程（重启主机最干净）。 */
void
websrv_park(const char *reason) {
  if(g_parked) {
    return;
  }
  g_parked = 1;
  mango_log("PARKED: %s -- the process will stay alive (so the payload "
            "manager does NOT re-inject) but stop serving; the port is "
            "released when the accept loop unwinds. End it manually when "
            "ready (it dies cleanly).", reason);
}

int
websrv_parked(void) {
  return g_parked;
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
    ctx->seq = diag_record_request(method, url);
    *con_cls = ctx;
    return MHD_YES;
  }

  if(*upload_data_size) {
    size_t chunk_size = *upload_data_size;

    /* 停车兜底：accept 循环退出前已挂进 MHD 的连接仍会把 body 送上来，
       一律拒收，让它们尽快结束（MHD_stop_daemon 才 join 得动）。 */
    if(g_parked) {
      return MHD_NO;
    }

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
    if(ctx->too_large) {
      /* 2026-10-10：超限后不能陪客户端把 body 传完 —— 真机上一次 60MB 镜像
       * 一步式 POST 会把这条连接挂住几分钟（前端一直 pending），用户反复重试
       * 就把 48 个连接全占满，「进程还在、网页打不开」。已丢弃量再超一次
       * 上限（= body 明显是巨型的）就直接断开连接，把线程还回去；小超限的
       * 请求仍走下方正常 413 JSON 应答。 */
      ctx->dropped += chunk_size;
      if(ctx->dropped > REQUEST_BODY_MAX) {
        *upload_data_size = 0;
        return MHD_NO;
      }
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
#ifdef __linux__
  /* Host-only: turns "how many times has the save domain been entered" into a
   * readable interface. The console ELF does not have this route at all.
   *
   * Why it exists: the kernel call behind save_domain_enter() is a no-op on the
   * host, so "did this worker escalate?" has no behavioural trace to assert on
   * -- and the bug it guards has happened three times (backup works while the
   * same save lists as absent, because only one of the two paths escalated).
   * Without this, hosttest has to read src/mango_port.c and look for the string
   * `save_domain_enter();` in a window after the function name, which keeps the
   * invariant in the source formatting instead of in the interface. */
  if(!strcmp(url, "/api/save-domain")) {
    char js[96];
    int n = snprintf(js, sizeof(js), "{\"ok\":true,\"enters\":%lu}",
                     save_domain_enters());
    struct MHD_Response *resp;
    enum MHD_Result sr;

    if(n < 0) n = 0;
    if((size_t)n >= sizeof(js)) n = (int)sizeof(js) - 1;
    resp = MHD_create_response_from_buffer((size_t)n, js, MHD_RESPMEM_MUST_COPY);
    if(!resp) return MHD_NO;
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json");
    sr = websrv_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return sr;
  }
#endif
  /* 停车兜底：accept 循环退出前已挂进 MHD 的最后一个请求。503 而不是断开，
     浏览器/前端能看到明确的 JSON 错误而不是网络错误。 */
  if(g_parked) {
    static const char parked_json[] =
      "{\"ok\":false,\"error\":\"service parked: a kernel call wedged; the "
      "process is idling so the payload manager will not re-inject. End the "
      "process manually and re-inject.\",\"parked\":true}";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
      sizeof(parked_json) - 1, (void *)parked_json, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result pr;

    if(!resp) {
      return MHD_NO;
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json");
    pr = websrv_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, resp);
    MHD_destroy_response(resp);
    return pr;
  }
  if(!strncmp(url, "/api/", 5)) {
    return filemgr_api_request(conn, url, method, ctx->body, ctx->size);
  }
  /* The console's own package installer pulls /stream/install/<session>.pkg
   * from here (v1.0.25: the stream used to be a second listener on its own
   * port; it is this route now — see pkg_stream.h). The query string is
   * already stripped by MHD, and the route itself re-checks that the peer is
   * loopback, so reaching this line from the LAN only earns a 403. */
  if(!strncmp(url, "/stream/install/", 16)) {
    return pkg_stream_request(conn, url, method);
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
    diag_record_done(ctx->seq, (int)toe);
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
    mango_log("websrv_listen(%u): bind failed: %s", port, strerror(errno));
    perror("bind");
    close(srvfd);
    return -1;
  }
  if(listen(srvfd, 16)) {
    mango_log("websrv_listen(%u): listen failed: %s", port, strerror(errno));
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
                                MHD_OPTION_CONNECTION_LIMIT,
                                (unsigned int)HTTP_CONNECTION_LIMIT,
                                MHD_OPTION_CONNECTION_TIMEOUT,
                                (unsigned int)HTTP_CONNECTION_TIMEOUT_SEC,
                                MHD_OPTION_NOTIFY_COMPLETED,
                                &websrv_on_completed, NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    mango_log("websrv_listen(%u): MHD_start_daemon failed: %s", port,
              strerror(errno));
    close(srvfd);
    return -1;
  }

  while(!g_stop_requested && !g_parked) {
    fd_set rfds;
    struct timeval tv;
    int ready;

    /* The loopback PKG stream (src/pkg_stream.c) is serviced from HERE, not
     * from its own thread. Since v1.0.25 the BYTES are MHD's business — the
     * stream is the /stream/install/ route, served by MHD's per-connection
     * threads at full speed with no main-loop involvement. All that is left
     * for this loop is lifecycle: withdraw an offer whose last byte moved
     * more than 30 minutes ago. The call is a cheap no-op otherwise.
     *
     * ⚠️ 仍然必须每一轮都调：撤流检查不能只在 select 可读时跑 —— 安装器拉完后
     *   前端页面安静下来，select 就永远不可读，超时的 offer 会一直挂着。 */
    if(pkg_stream_tick() > 0) {
      /* Work was done and more may be pending: poll with no wait so the
       * stream runs at link speed instead of 4 MB per 5 ms. */
      tv.tv_sec = 0;
      tv.tv_usec = 0;
      FD_ZERO(&rfds);
      FD_SET(srvfd, &rfds);
      ready = select(srvfd + 1, &rfds, NULL, NULL, &tv);
    } else {
      /* The 5 ms poll: it bounds how long shutdown takes to notice
       * g_stop_requested, and it is the stream's idle heartbeat. */
      FD_ZERO(&rfds);
      FD_SET(srvfd, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 5000;
      ready = select(srvfd + 1, &rfds, NULL, NULL, &tv);
    }
    if(ready < 0) {
      if(errno == EINTR) continue;
      if(!g_stop_requested) perror("select");
      break;
    }
    if(ready == 0) continue;

    addr_len = sizeof(client_addr);
    if((connfd = accept(srvfd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
      /* ⚠️ 一次 accept 失败曾直接 break 掉整个监听循环（进程活着、服务死了，
       * 且因为 MHD_stop_daemon 要 join 所有在飞连接线程，main 的 3 秒重试
       * 也可能永远起不来）。accept 的失败几乎都是瞬时的：对端在完成握手前
       * 掉了（ECONNABORTED/EPROTO）、fd 用尽（EMFILE/ENFILE）……这些只能
       * 丢掉这一个连接，绝不能丢掉监听 socket。唯一还会走到下面的出口是
       * 连续 1000 次失败 —— 让 main 拆掉整个 daemon 重来，而不是原地僵死。 */
      if(g_stop_requested) break;
      if(++g_accept_errors >= 1000) {
        perror("accept (giving up after 1000 consecutive failures)");
        break;
      }
      if(errno == EMFILE || errno == ENFILE || errno == ENOMEM ||
         errno == ENOBUFS || errno == EINTR || errno == ECONNABORTED ||
         errno == EAGAIN || errno == EWOULDBLOCK || errno == EPROTO) {
        poll(NULL, 0, 20);      /* 资源紧张时歇 20ms 再来，避免热循环 */
        continue;
      }
      perror("accept");
      poll(NULL, 0, 20);
      continue;
    }
    g_accept_errors = 0;
    websrv_tune_connection_socket(connfd);
    /* MHD 拒收单个连接（超过 CONNECTION_LIMIT / 内部线程起不来）只是这一个
     * 连接没戏 —— 浏览器会重试。旧代码在这里 close + break，等于让整个站点
     * 永久下线，这是「进程还在、网页打不开」的最短路径。 */
    if(MHD_add_connection(httpd, connfd, (struct sockaddr *)&client_addr,
                          addr_len) != MHD_YES) {
      close(connfd);
      continue;
    }
  }

  mango_log("websrv_listen(%u): accept loop left (stop_requested=%d, "
            "accept_errors=%u); stopping daemon", port,
            g_stop_requested ? 1 : 0, g_accept_errors);
  MHD_stop_daemon(httpd);
  g_listen_fd = -1;
  mango_log("websrv_listen(%u): daemon stopped, listen socket closed", port);
  return close(srvfd);
}
