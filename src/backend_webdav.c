/* backend_webdav.c —— WebDAV 的 transfer 后端（HTTP/1.1，明文 + 可选 TLS）
 *
 * 本文件由 transfer.c **原样搬出**（2026-10-27）：代码逐行等价，只把
 * 挂在 struct transfer_src 上的 wd_* 字段收进后端私有 wd_state_t。
 *
 * transport seam（2026-10-27 第 2 轮）：同一套 HTTP/1.1 逻辑跑在两种传输上
 *   - 明文 TCP（st->tls == 0）：webdav:// —— 与迁移前逐字节等价；
 *   - mbedTLS（st->tls == 1）：webdavs:// —— 复用四家网盘已在链上的
 *     mbedtls 静态库（CLOUD_NET=1 时才编进 include 路径与链接行；
 *     否则 webdavs 请求报「未编入」，明文 webdav 照常可用）。
 *
 * 证书策略（刻意）：VERIFY_NONE。NAS/群晖的自签证书在公网 CA 模型下必挂，
 * 全校验等于让「自家 NAS 5006」这个主场景全灭；内网链路上 TLS 的价值是防
 * 误配而非防攻击。SNI（mbedtls_ssl_set_hostname）**必须**设 —— 群晖/nginx
 * 多站点没有 SNI 会返回默认站点与默认证书。
 *
 * Wire map (same for every WebDAV server):
 *   list   PROPFIND Depth:1  -> 207 multistatus XML
 *   size   HEAD              -> Content-Length (fallback PROPFIND Depth:0)
 *   read   GET + Range       -> 206 partial content
 *   remove DELETE            -> 204/200
 *
 * The TCP connection is reused across calls. wd_finish() keeps it ONLY when
 * the response body was consumed exactly; an unread body left in the socket
 * would be parsed as the NEXT response — the classic keep-alive landmine.
 */
#include "transfer_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>

#if defined(MANGO_HAVE_WEBDAV)
#include <netdb.h>

/* TLS 传输的前提：mbedTLS 在链上（Makefile 的 MBTLS=1 把 include 路径与静态
 * 库编进来）。MBTLS=0 + WEBDAV=1 的组合仍然可用 —— 明文 webdav 照跑，只有
 * webdavs 请求会被拒。
 * v1.0.8：原先判 MANGO_HAVE_CLOUD（mbedtls 当年是随云盘进链的），云盘移除
 * 后改判 MANGO_HAVE_MBEDTLS —— 否则砍云盘会把 webdavs 一并砍掉。 */
#if defined(MANGO_HAVE_MBEDTLS)
#define WD_HAVE_TLS 1
#include <mbedtls/version.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <pthread.h>
#endif

#define WD_RBUF        8192
#define WD_TIMEOUT_SEC 15
#define WD_MAX_BODY    (16u * 1024u * 1024u)

/* 缓冲读器（原 transfer_internal.h 的 wd_rd_t —— 随后端一起搬进来）。
 * fd 不在这里了：I/O 一律走 wd_t 的 transport（明文/TLS 在那里分叉）。 */
typedef struct { char buf[WD_RBUF]; size_t pos, len; } wd_rd_t;

/* 后端私有状态。原先是 transfer_src 上的六个 wd_* 字段 + Host 缓冲。 */
typedef struct wd_state {
  int     fd;          /* lazy TCP socket; -1 = closed */
  wd_rd_t rd;          /* buffered reader bound to fd */
  long    left;        /* fixed-length body bytes left */
  int     chunked;     /* body uses Transfer-Encoding: chunked */
  long    chunk_left;  /* bytes left in the current chunk (-1 = need header) */
  int     done;        /* body fully consumed */
  char    host[320];   /* Host: header value (host[:port]) */
  int     tls;         /* 1 = fd 上已架 TLS，读写走 ssl */
#if defined(WD_HAVE_TLS)
  mbedtls_ssl_context ssl;
#endif
} wd_state_t;

/* 后端函数的工作视图：nas/err 来自 ctx（生命期 = 整个 transfer_src），
 * st 是堆上的私有状态。比传 transfer_ctx_t 少一层间接。 */
typedef struct {
  const transfer_nas_t *nas;
  wd_state_t           *st;
  char                 *err;
  size_t                err_cap;
} wd_t;

static wd_t
wd_of(transfer_ctx_t *c) {
  wd_t w;
  w.nas     = c->loc;
  w.err     = c->err;
  w.err_cap = c->err_cap;
  w.st      = (wd_state_t *)c->state;
  return w;
}

/* Record why the backend failed. Same contract as the old set_err(). */
static void
wd_err(wd_t *s, const char *msg) {
  if(!s || !s->err || !s->err_cap) return;
  snprintf(s->err, s->err_cap, "%s", (msg && *msg) ? msg : "unknown error");
}

static const char WD_PROPFIND_BODY[] =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
  "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
  "<d:displayname/><d:getcontentlength/><d:getlastmodified/><d:resourcetype/>"
  "</d:prop></d:propfind>";

static void wd_copy(char *dst, size_t cap, const char *src);

static void
wd_set_conn_state(wd_t *s, int fd) {
  s->st->fd = fd;
  s->st->rd.pos = s->st->rd.len = 0;
  s->st->left = 0;
  s->st->chunked = 0;
  s->st->chunk_left = -1;
  s->st->done = 0;
  s->st->tls = 0;
}

#if defined(WD_HAVE_TLS)
static double
wd_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}
#endif

/* ---- transport：读一块 / 写一块。明文与 TLS 在这里分叉。 ------------- */

/* >0 = n 字节；0 = 对端正常关闭；-1 = 错误（含超时）。 */
static ssize_t
wd_tr_recv(wd_t *s, void *buf, size_t len) {
  if(!s->st->tls) {
    ssize_t n;
    do { n = recv(s->st->fd, buf, len, 0); } while(n < 0 && errno == EINTR);
    return n;   /* <=0 = 关闭/错误，与迁移前语义一致 */
  }
#if defined(WD_HAVE_TLS)
  {
    /* fd 是阻塞 socket（SO_RCVTIMEO=15s）：mbedtls 在 BIO 里等数据，
     * 超时回 WANT_READ —— 循环 + 无进展封顶，不忙转。 */
    double t0 = wd_now_ms();
    for(;;) {
      int rc = mbedtls_ssl_read(&s->st->ssl, (unsigned char *)buf, len);
      if(rc > 0) return (ssize_t)rc;
      if(rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
      if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if(wd_now_ms() - t0 > (double)WD_TIMEOUT_SEC * 1000.0) return -1;
        continue;
      }
      return -1;
    }
  }
#else
  (void)buf; (void)len;
  return -1;
#endif
}

/* 全量发送。1 = 成功，0 = 失败。 */
static int
wd_tr_send(wd_t *s, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  size_t done = 0;

  if(!s->st->tls) {
    while(done < len) {
      ssize_t n;
#ifdef MSG_NOSIGNAL
      n = send(s->st->fd, p + done, len - done, MSG_NOSIGNAL);
#else
      n = send(s->st->fd, p + done, len - done, 0);
#endif
      if(n <= 0) {
        if(n < 0 && errno == EINTR) continue;
        return 0;
      }
      done += (size_t)n;
    }
    return 1;
  }
#if defined(WD_HAVE_TLS)
  while(done < len) {
    int rc = mbedtls_ssl_write(&s->st->ssl, (const unsigned char *)p + done,
                               len - done);
    if(rc > 0) { done += (size_t)rc; continue; }
    if(rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ)
      continue;   /* BIO 在阻塞 fd 上等，超时由 SO_SNDTIMEO 兜底 */
    return 0;
  }
  return 1;
#else
  (void)p;
  return 0;
#endif
}

#if defined(WD_HAVE_TLS)
/* 握手期 BIO：直接对着阻塞 fd 收发。与 wd_tr_send/recv 分开 —— 握手期间
 * st->tls 还没置 1，tr 层会走明文分支。模式照抄 cloud_net.c 的 cn_bio_*。 */
static int
wd_bio_send_plain(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  size_t off = 0;
  while(off < len) {
    ssize_t n;
#ifdef MSG_NOSIGNAL
    n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
#else
    n = send(fd, buf + off, len - off, 0);
#endif
    if(n > 0) { off += (size_t)n; continue; }
    if(n < 0 && errno == EINTR) continue;
    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)len;
}

static int
wd_bio_recv_plain(void *ctx, unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  ssize_t n = recv(fd, buf, len, 0);
  if(n > 0) return (int)n;
  if(n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
  if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
    return MBEDTLS_ERR_SSL_WANT_READ;
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* TLS 一次性初始化。复用 cloud_net 的既有事实：全局 conf + 带锁 DRBG。
 * VERIFY_NONE 是刻意决策（见文件头），不需要 CA bundle。 */
static mbedtls_ssl_config wd_tls_conf;
static mbedtls_entropy_context wd_entropy;
static mbedtls_ctr_drbg_context wd_drbg;
static pthread_mutex_t wd_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static int wd_tls_ready = 0;

static int
wd_rng_cb(void *ctx, unsigned char *out, size_t len) {
  int rc;
  pthread_mutex_lock(&wd_tls_lock);
  rc = mbedtls_ctr_drbg_random((mbedtls_ctr_drbg_context *)ctx, out, len);
  pthread_mutex_unlock(&wd_tls_lock);
  return rc;
}

static int
wd_tls_init_locked(void) {
  if(wd_tls_ready) return 1;
  mbedtls_ssl_config_init(&wd_tls_conf);
  mbedtls_entropy_init(&wd_entropy);
  mbedtls_ctr_drbg_init(&wd_drbg);
  if(mbedtls_ctr_drbg_seed(&wd_drbg, mbedtls_entropy_func, &wd_entropy,
          (const unsigned char *)"ps5-mango-webdav", 16) != 0) return 0;
  if(mbedtls_ssl_config_defaults(&wd_tls_conf, MBEDTLS_SSL_IS_CLIENT,
          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) return 0;
  /* 自签 NAS 必须能连（见文件头）。 */
  mbedtls_ssl_conf_authmode(&wd_tls_conf, MBEDTLS_SSL_VERIFY_NONE);
  /* 必须是 wd_rng_cb（带锁）—— A 线程握手时 B 线程也会进 RNG。 */
  mbedtls_ssl_conf_rng(&wd_tls_conf, wd_rng_cb, &wd_drbg);
  wd_tls_ready = 1;
  return 1;
}
#endif /* WD_HAVE_TLS */

/* TCP 已连上之后架 TLS。0 = 失败（fd 由调用方断开）。 */
static int
wd_tls_establish(wd_t *s) {
#if defined(WD_HAVE_TLS)
  wd_state_t *st = s->st;
  double t0;
  int rc;

  pthread_mutex_lock(&wd_tls_lock);
  rc = wd_tls_init_locked();
  pthread_mutex_unlock(&wd_tls_lock);
  if(!rc) { wd_err(s, "webdav: tls init failed"); return 0; }

  mbedtls_ssl_init(&st->ssl);
  if(mbedtls_ssl_setup(&st->ssl, &wd_tls_conf) != 0) {
    wd_err(s, "webdav: tls setup failed");
    return 0;
  }
  /* SNI 必设：群晖/nginx 多站点没有 SNI 会返回默认站点与默认证书。
   * IP 地址时 mbedtls 不发 SNI 但仍记录主机名 —— 行为正确。 */
  if(mbedtls_ssl_set_hostname(&st->ssl, s->nas->server) != 0) {
    wd_err(s, "webdav: tls sni setup failed");
    return 0;
  }
  mbedtls_ssl_set_bio(&st->ssl, &st->fd,
                      wd_bio_send_plain, wd_bio_recv_plain, NULL);
  t0 = wd_now_ms();
  while((rc = mbedtls_ssl_handshake(&st->ssl)) != 0) {
    if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      /* BIO 在阻塞 fd 上等：这里只是被超时打断后的续跑，封顶防死循环。 */
      if(wd_now_ms() - t0 > (double)WD_TIMEOUT_SEC * 4 * 1000.0) {
        wd_err(s, "webdav: tls handshake timeout");
        return 0;
      }
      continue;
    }
    {
      char msg[96];
      snprintf(msg, sizeof(msg), "webdav: tls handshake failed (-0x%04x)",
               (unsigned)-rc);
      wd_err(s, msg);
    }
    return 0;
  }
  return 1;
#else
  (void)s;
  wd_err(s, "webdav: https not compiled in (TLS unavailable)");
  return 0;
#endif
}

static int
wd_rd_fill(wd_t *s) {
  wd_rd_t *r = &s->st->rd;
  ssize_t n;
  if(r->pos < r->len) return 1;
  n = wd_tr_recv(s, r->buf, sizeof(r->buf));
  if(n <= 0) return 0;
  r->pos = 0;
  r->len = (size_t)n;
  return 1;
}

static int
wd_rd_getc(wd_t *s) {
  if(!wd_rd_fill(s)) return -1;
  return (unsigned char)s->st->rd.buf[s->st->rd.pos++];
}

/* One CRLF/LF-terminated line without the EOL. 1 on success, 0 on EOF. */
static int
wd_rd_line(wd_t *s, char *out, size_t cap) {
  size_t o = 0;
  int c;
  while((c = wd_rd_getc(s)) >= 0) {
    if(c == '\n') break;
    if(c == '\r') continue;
    if(o + 1 < cap) out[o++] = (char)c;
  }
  out[o] = 0;
  return c >= 0 || o > 0;
}

/* Read exactly `want` bytes unless EOF. `*got` is the actual count. */
static int
wd_rd_read(wd_t *s, void *dst, size_t want, size_t *got) {
  size_t done = 0;
  char *p = (char *)dst;
  wd_rd_t *r = &s->st->rd;
  while(done < want) {
    size_t avail;
    if(!wd_rd_fill(s)) break;
    avail = r->len - r->pos;
    if(avail > want - done) avail = want - done;
    memcpy(p + done, r->buf + r->pos, avail);
    r->pos += avail;
    done += avail;
  }
  *got = done;
  return done == want;
}

static void
wd_disconnect(wd_t *s) {
  if(s->st->fd >= 0) {
#if defined(WD_HAVE_TLS)
    if(s->st->tls) mbedtls_ssl_free(&s->st->ssl);
#endif
    shutdown(s->st->fd, SHUT_RDWR);
    close(s->st->fd);
    s->st->fd = -1;
    s->st->tls = 0;
  }
}

static int
wd_connect(wd_t *s) {
  struct addrinfo hints, *res = NULL, *ai;
  char port[16];
  int fd = -1, rc, want_tls = s->nas->tls;

  if(s->st->fd >= 0) return 1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d",
           s->nas->port > 0 ? s->nas->port : (want_tls ? 443 : 80));
  rc = getaddrinfo(s->nas->server, port, &hints, &res);
  if(rc != 0 || !res) { wd_err(s, "webdav: cannot resolve host"); return 0; }
  for(ai = res; ai; ai = ai->ai_next) {
    struct timeval tv;
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if(fd < 0) continue;
    tv.tv_sec = WD_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if(fd < 0) { wd_err(s, "webdav: connect failed"); return 0; }

  if(s->nas->port > 0 && s->nas->port != 80)
    snprintf(s->st->host, sizeof(s->st->host), "%s:%d", s->nas->server, s->nas->port);
  else
    wd_copy(s->st->host, sizeof(s->st->host), s->nas->server);

  wd_set_conn_state(s, fd);
  if(want_tls) {
    if(!wd_tls_establish(s)) { wd_disconnect(s); return 0; }
    s->st->tls = 1;
  }
  return 1;
}

static int
wd_ci_pfx(const char *s, const char *pfx) {
  while(*pfx) {
    char a = *s++, b = *pfx++;
    if(!a) return 0;
    if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if(a != b) return 0;
  }
  return 1;
}

static void
wd_copy(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if(n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int
wd_enc_safe(unsigned char c) {
  if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
     (c >= '0' && c <= '9')) return 1;
  switch(c) {
    case '-': case '.': case '_': case '~': case '/': case ':': case '@':
      return 1;
    default:
      return 0;
  }
}

static void
wd_url_enc(const char *in, char *out, size_t cap) {
  static const char hx[] = "0123456789ABCDEF";
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)in;
  for(; *p && o + 4 < cap; p++) {
    if(wd_enc_safe(*p)) out[o++] = (char)*p;
    else {
      out[o++] = '%';
      out[o++] = hx[*p >> 4];
      out[o++] = hx[*p & 15];
    }
  }
  out[o] = 0;
}

static int
wd_hexval(int c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void
wd_url_dec(const char *in, char *out, size_t cap) {
  size_t o = 0;
  while(*in && o + 1 < cap) {
    if(in[0] == '%' && in[1] && in[2]) {
      int hi = wd_hexval(in[1]), lo = wd_hexval(in[2]);
      if(hi >= 0 && lo >= 0) { out[o++] = (char)((hi << 4) | lo); in += 3; continue; }
    }
    out[o++] = *in++;
  }
  out[o] = 0;
}

static void
wd_build_path(wd_t *s, const char *dir, char *out, size_t cap, int want_slash) {
  transfer_nas_t nas = *s->nas;
  char raw[MANGO_PATH_MAX + 300];
  char enc[3 * (MANGO_PATH_MAX + 300)];
  size_t i, n;

  if(dir) {
    transfer_nas_t tmp;
    if(transfer_url_parse(dir, &tmp)) nas = tmp;
  }
  raw[0] = 0;
  wd_copy(raw, sizeof(raw), "/");
  if(nas.share[0]) {
    wd_copy(raw + strlen(raw), sizeof(raw) - strlen(raw), nas.share);
    if(nas.path[0]) {
      wd_copy(raw + strlen(raw), sizeof(raw) - strlen(raw), "/");
      wd_copy(raw + strlen(raw), sizeof(raw) - strlen(raw), nas.path);
    }
  } else if(nas.path[0]) {
    wd_copy(raw + strlen(raw), sizeof(raw) - strlen(raw), nas.path);
  }
  for(i = 0; raw[i]; i++) if(raw[i] == '\\') raw[i] = '/';

  wd_url_enc(raw, enc, sizeof(enc));
  n = strlen(enc);
  if(n == 0) { enc[0] = '/'; enc[1] = 0; n = 1; }
  if(want_slash && enc[n - 1] != '/' && n + 2 < sizeof(enc)) {
    enc[n++] = '/';
    enc[n] = 0;
  }
  wd_copy(out, cap, enc);
}

static void
wd_b64(const unsigned char *in, size_t len, char *out, size_t cap) {
  static const char T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i, o = 0;
  for(i = 0; i < len && o + 4 < cap; i += 3) {
    unsigned int v = (unsigned int)in[i] << 16;
    if(i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
    if(i + 2 < len) v |= (unsigned int)in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? T[v & 63] : '=';
  }
  out[o] = 0;
}

static const char *
wd_status_text(int status) {
  switch(status) {
    case 200: return "webdav: OK";
    case 204: return "webdav: no content";
    case 206: return "webdav: partial content";
    case 207: return "webdav: multi-status";
    case 301: case 302: case 307: return "webdav: redirected (use the final URL)";
    case 400: return "webdav: bad request";
    case 401: return "webdav: authentication required";
    case 403: return "webdav: forbidden";
    case 404: return "webdav: not found";
    case 405: return "webdav: method not allowed";
    case 409: return "webdav: conflict";
    case 423: return "webdav: locked";
    case 507: return "webdav: insufficient storage";
    default:  return "webdav: request failed";
  }
}

static mango_err_t
wd_status_err(int status) {
  if(status == 401 || status == 403) return MANGO_ERR_PERM;
  if(status == 404) return MANGO_ERR_NOTFOUND;
  if(status == 409) return MANGO_ERR_CONFLICT;
  return MANGO_ERR_GENERIC;
}

/* 报错时把「打的是哪个路径」一起写进错误串。
 *
 * 为什么（2026-10-29 真机）：用户只拿到一句 `webdav: not found`，而这句话
 * 至少对应三种完全不同的原因 —— 共享名错、文件名被改了、服务器不认这个
 * 方法。以前要定位只能靠猜。错误里带上传出去的那串 path（已是百分号编码，
 * 直接就是 HTTP 请求行里的东西），一眼能分出「名字写错了」还是「整个目录
 * 都不对」：前者只在叶子段有差异，后者前缀就不同。
 * 缓冲固定 224：请求线程栈不许跟着 MANGO_PATH_MAX 放大（chk_stack_buf）。 */
static void
wd_err_status(wd_t *s, int status, const char *path) {
  char msg[224];
  const char *leaf = path ? strrchr(path, '/') : NULL;

  leaf = leaf ? leaf + 1 : (path ? path : "");
  if(path && strlen(path) <= 160) {
    snprintf(msg, sizeof(msg), "%s [%s]", wd_status_text(status), path);
  } else {
    snprintf(msg, sizeof(msg), "%s [.../%s]", wd_status_text(status), leaf);
  }
  wd_err(s, msg);
}

static int
wd_request(wd_t *s, const char *method, const char *path,
           const char *extra, const char *body, size_t body_len,
           int *status, long *clen, int *chunked) {
  char hdr[4096];
  char line[512];
  size_t n = 0;
  int is_head = !strcmp(method, "HEAD");

  if(!wd_connect(s)) return 0;

  if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
  n += (size_t)snprintf(hdr + n, sizeof(hdr) - n, "%s %s HTTP/1.1\r\n", method, path);
  if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
  n += (size_t)snprintf(hdr + n, sizeof(hdr) - n, "Host: %s\r\n", s->st->host);
  if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
  n += (size_t)snprintf(hdr + n, sizeof(hdr) - n,
                        "User-Agent: PS5-Mango\r\nAccept: */*\r\nConnection: keep-alive\r\n");
  if(s->nas->user[0]) {
    char cred[300], b64[512];
    snprintf(cred, sizeof(cred), "%s:%s", s->nas->user, s->nas->pass);
    wd_b64((const unsigned char *)cred, strlen(cred), b64, sizeof(b64));
    if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
    n += (size_t)snprintf(hdr + n, sizeof(hdr) - n, "Authorization: Basic %s\r\n", b64);
  }
  if(extra && *extra) {
    if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
    n += (size_t)snprintf(hdr + n, sizeof(hdr) - n, "%s", extra);
  }
  if(body_len) {
    if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
    n += (size_t)snprintf(hdr + n, sizeof(hdr) - n,
                          "Content-Length: %llu\r\n", (unsigned long long)body_len);
  }
  if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }
  n += (size_t)snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
  if(n >= sizeof(hdr)) { wd_disconnect(s); return 0; }

  if(!wd_tr_send(s, hdr, n)) { wd_disconnect(s); return 0; }
  if(body_len && !wd_tr_send(s, body, body_len)) { wd_disconnect(s); return 0; }

  if(!wd_rd_line(s, line, sizeof(line))) { wd_disconnect(s); return 0; }

  *status = 0;
  if(sscanf(line, "HTTP/%*d.%*d %d", status) != 1) { wd_disconnect(s); return 0; }

  *clen = -1;
  *chunked = 0;
  for(;;) {
    if(!wd_rd_line(s, line, sizeof(line))) { wd_disconnect(s); return 0; }
    if(!line[0]) break;
    if(wd_ci_pfx(line, "content-length:")) *clen = atol(line + 15);
    else if(wd_ci_pfx(line, "transfer-encoding:") && strstr(line, "chunked")) *chunked = 1;
  }

  s->st->chunked = *chunked;
  s->st->chunk_left = -1;
  s->st->done = 0;
  if(is_head) s->st->left = 0;                /* HEAD has no body */
  else if(*chunked) s->st->left = 0;          /* chunked reader tracks itself */
  else if(*clen >= 0) s->st->left = *clen;
  else s->st->left = -1;                      /* close-delimited */
  return 1;
}

static int
wd_body_read(wd_t *s, void *dst, size_t want, size_t *got) {
  size_t done = 0;
  char *p = (char *)dst;
  *got = 0;
  if(s->st->done) return 1;

  if(!s->st->chunked) {
    size_t n, rn;
    if(s->st->left == 0) { s->st->done = 1; return 1; }
    n = want;
    if(s->st->left > 0 && (long)n > s->st->left) n = (size_t)s->st->left;
    if(!wd_rd_read(s, p, n, &rn) && s->st->left > 0) return 0;
    *got = rn;
    if(s->st->left > 0) {
      s->st->left -= (long)rn;
      if(s->st->left == 0) s->st->done = 1;
    } else if(rn == 0) {
      s->st->done = 1;
    }
    return 1;
  }

  while(done < want && !s->st->done) {
    size_t n, rn;
    if(s->st->chunk_left < 0) {
      char line[64];
      unsigned long sz;
      if(!wd_rd_line(s, line, sizeof(line))) return 0;
      sz = strtoul(line, NULL, 16);
      if(sz == 0) {
        while(wd_rd_line(s, line, sizeof(line)) && line[0]) { /* trailers */ }
        s->st->done = 1;
        break;
      }
      s->st->chunk_left = (long)sz;
    }
    n = (size_t)s->st->chunk_left;
    if(n > want - done) n = want - done;
    if(!wd_rd_read(s, p + done, n, &rn) || rn != n) return 0;
    done += n;
    s->st->chunk_left -= (long)n;
    if(s->st->chunk_left == 0) {
      char line[8];
      wd_rd_line(s, line, sizeof(line));
      s->st->chunk_left = -1;
    }
  }
  *got = done;
  return 1;
}

/* Keep the socket only when the body was consumed exactly. Anything else
 * (unknown length, short read, chunk error) closes it — a half-read body
 * would be mistaken for the next response. */
static void
wd_finish(wd_t *s) {
  int keep = s->st->done && s->st->left >= 0;
  if(!keep) { wd_disconnect(s); return; }
  s->st->left = 0;
  s->st->chunked = 0;
  s->st->chunk_left = -1;
  s->st->done = 0;
}

static int
wd_read_all(wd_t *s, char **out, size_t *outlen) {
  size_t len = 0, cap = 0, got = 0;
  char *buf = NULL;

  while(!s->st->done) {
    if(len == cap) {
      size_t nc = cap ? cap * 2 : 8192;
      char *nb;
      if(nc > WD_MAX_BODY) nc = WD_MAX_BODY;
      if(nc <= len) { free(buf); return 0; }
      nb = (char *)realloc(buf, nc);
      if(!nb) { free(buf); return 0; }
      buf = nb;
      cap = nc;
    }
    if(!wd_body_read(s, buf + len, cap - len, &got)) { free(buf); return 0; }
    if(got == 0) break;
    len += got;
    if(len >= WD_MAX_BODY) { free(buf); return 0; }
  }
  if(!buf) { buf = (char *)malloc(1); if(!buf) return 0; }
  else if(len == cap) {
    char *nb = (char *)realloc(buf, cap + 1);
    if(!nb) { free(buf); return 0; }
    buf = nb;
  }
  buf[len] = 0;
  *out = buf;
  *outlen = len;
  return 1;
}

static int
wd_skip(wd_t *s, uint64_t n) {
  char tmp[4096];
  while(n > 0) {
    size_t want = (n > sizeof(tmp)) ? sizeof(tmp) : (size_t)n;
    size_t got = 0;
    if(!wd_body_read(s, tmp, want, &got)) return 0;
    if(got == 0) return 1;
    n -= got;
  }
  return 1;
}

static void
wd_drain(wd_t *s) {
  char tmp[1024];
  size_t got = 0;
  int guard = 0;
  while(!s->st->done && guard++ < 4096) {
    if(!wd_body_read(s, tmp, sizeof(tmp), &got)) { wd_disconnect(s); return; }
    if(got == 0) break;
  }
}

/* ---- Minimal XML cursor (namespace-prefix agnostic) ------------------ */

/* Returns the char just after the opening tag whose LOCAL name == local. */
static const char *
wd_find_open(const char *p, const char *local) {
  size_t ll = strlen(local);
  while((p = strchr(p, '<')) != NULL) {
    const char *q = p + 1, *nm, *en;
    if(*q == '/' || *q == '?' || *q == '!') { p = q; continue; }
    nm = q;
    while(*q && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' &&
          *q != '\r' && *q != '\n') q++;
    en = q;
    {
      const char *x = en;
      while(x > nm && x[-1] != ':') x--;
      nm = x;
    }
    if((size_t)(en - nm) == ll && !strncmp(nm, local, ll)) {
      while(*q && *q != '>') q++;
      return *q ? q + 1 : NULL;
    }
    p = en;
  }
  return NULL;
}

/* Text content of the first <local> element (prefix ignored). */
static int
wd_tag_text(const char *p, const char *local, char *out, size_t cap) {
  const char *t = wd_find_open(p, local);
  const char *e;
  size_t ll = strlen(local);
  if(!t) return 0;
  e = t;
  while((e = strchr(e, '<')) != NULL) {
    if(e[1] == '/') {
      const char *q = e + 2, *nm, *en;
      while(*q && *q != '>') q++;
      en = q;
      {
        const char *x = en;
        while(x > e + 2 && x[-1] != ':') x--;
        nm = x;
      }
      if((size_t)(en - nm) == ll && !strncmp(nm, local, ll)) {
        size_t n = (size_t)(e - t);
        if(n >= cap) n = cap - 1;
        memcpy(out, t, n);
        out[n] = 0;
        return 1;
      }
    }
    e++;
  }
  return 0;
}

/* Next <response>…</response> block; advances *cursor past it. */
static const char *
wd_next_response(const char **cursor, size_t *blen) {
  const char *p = *cursor, *o, *c;
  size_t ll = 8; /* "response" */
  o = wd_find_open(p, "response");
  if(!o) return NULL;
  c = o;
  while((c = strstr(c, "</")) != NULL) {
    const char *q = c + 2, *nm, *en;
    while(*q && *q != '>') q++;
    en = q;
    {
      const char *x = en;
      while(x > c + 2 && x[-1] != ':') x--;
      nm = x;
    }
    if((size_t)(en - nm) == ll && !strncmp(nm, "response", ll)) {
      *blen = (size_t)(c - o);
      *cursor = (*en == '>') ? en + 1 : en;
      return o;
    }
    c++;
  }
  return NULL;
}

/* <resourcetype><D:collection/></resourcetype> -> directory. */
static int
wd_is_collection(const char *resp) {
  const char *o = wd_find_open(resp, "resourcetype");
  const char *c;
  size_t ll = 12; /* "resourcetype" */
  if(!o) return 0;
  c = o;
  while((c = strstr(c, "</")) != NULL) {
    const char *q = c + 2, *nm, *en;
    while(*q && *q != '>') q++;
    en = q;
    {
      const char *x = en;
      while(x > c + 2 && x[-1] != ':') x--;
      nm = x;
    }
    if((size_t)(en - nm) == ll && !strncmp(nm, "resourcetype", ll)) {
      const char *t = o;
      while(t + 10 <= c) {
        if(!strncmp(t, "collection", 10)) return 1;
        t++;
      }
      return 0;
    }
    c++;
  }
  return 0;
}

/* XML 文本转义还原。
 * ⚠️ 这不是润色：href / displayname 是 XML **文本**，文件名里的 '&' 在报文里
 *    必然写成 "&amp;"。不还原就等于拿 "a&amp;b.mp4" 去发 GET，而服务器只认
 *    "a&b.mp4" ⇒ 404 ⇒ 前端显示「列表里看得见、点开说找不到」。
 *    支持 &amp; &lt; &gt; &quot; &apos; 与 &#NN; / &#xHH;（含两位 UTF-8 编码）。
 * 只会变短（实体串总是长于替换结果），因此可以原地写。 */
static void
wd_xml_unescape(char *s) {
  char *r = s, *w = s;

  while(*r) {
    char *semi;
    size_t n;

    if(*r != '&' || !(semi = strchr(r, ';'))) { *w++ = *r++; continue; }
    n = (size_t)(semi - (r + 1));
    if(n >= 2 && n <= 9) {
      char ent[12];

      memcpy(ent, r + 1, n);
      ent[n] = 0;
      if(!strcmp(ent, "amp"))  { *w++ = '&';  r = semi + 1; continue; }
      if(!strcmp(ent, "lt"))   { *w++ = '<';  r = semi + 1; continue; }
      if(!strcmp(ent, "gt"))   { *w++ = '>';  r = semi + 1; continue; }
      if(!strcmp(ent, "quot")) { *w++ = '"';  r = semi + 1; continue; }
      if(!strcmp(ent, "apos")) { *w++ = '\''; r = semi + 1; continue; }
      if(ent[0] == '#') {
        unsigned long v = (ent[1] == 'x' || ent[1] == 'X')
          ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10);
        if(v >= 1 && v < 0x800) {
          if(v < 0x80) *w++ = (char)v;
          else {
            *w++ = (char)(0xC0 | (v >> 6));
            *w++ = (char)(0x80 | (v & 0x3F));
          }
          r = semi + 1;
          continue;
        }
      }
    }
    *w++ = *r++;   /* 不认识的实体原样保留 */
  }
  *w = 0;
}

static void
wd_href_path(const char *href, char *out, size_t cap) {
  const char *p = href;
  char tmp[MANGO_PATH_MAX * 3];
  char dec[MANGO_PATH_MAX * 3];
  size_t i = 0;

  if(!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
    const char *q = strstr(p, "://");
    q = q ? q + 3 : p;
    p = strchr(q, '/');
    if(!p) p = "/";
  }
  while(*p && *p != '?' && *p != '#' && i + 1 < sizeof(tmp)) tmp[i++] = *p++;
  tmp[i] = 0;
  wd_url_dec(tmp, dec, sizeof(dec));
  wd_copy(out, cap, dec);
}

static void
wd_trim_slash(char *p) {
  size_t n = strlen(p);
  while(n > 1 && p[n - 1] == '/') { p[--n] = 0; }
}

static void
wd_last_seg(const char *path, char *out, size_t cap) {
  const char *s = strrchr(path, '/');
  wd_copy(out, cap, s ? s + 1 : path);
}

static int
wd_month(const char *m) {
  static const char *names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
  int i;
  for(i = 0; i < 12; i++) if(!strncmp(m, names[i], 3)) return i + 1;
  return 0;
}

/* RFC 1123 date -> epoch seconds. No timegm dependency. */
static uint64_t
wd_date_epoch(const char *s) {
  int d = 0, y = 0, hh = 0, mm = 0, ss = 0, mo;
  char mon[4];
  const char *p = strchr(s, ',');
  const char *q = p ? p + 1 : s;
  while(*q == ' ') q++;
  memset(mon, 0, sizeof(mon));
  if(sscanf(q, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
  mo = wd_month(mon);
  if(mo <= 0 || d <= 0 || d > 31) return 0;
  {
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    if(days < 0) return 0;
    return (uint64_t)days * 86400u + (uint64_t)hh * 3600u +
           (uint64_t)mm * 60u + (uint64_t)ss;
  }
}

/* ---- The four backend entry points ---------------------------------- */

static mango_err_t
webdav_list(wd_t *s, const char *dir,
            transfer_entry_t **out, int *count) {
  char path[MANGO_PATH_MAX], req_path[MANGO_PATH_MAX];
  char *body = NULL;
  size_t blen = 0;
  int status = 0, chunked = 0;
  long clen = -1;
  transfer_entry_t *list = NULL;
  int n = 0, cap = 0;
  const char *cur;

  if(!out || !count) return MANGO_ERR_GENERIC;
  if(!wd_connect(s)) return MANGO_ERR_GENERIC;

  /* ★ 先问「这个 URL 是什么」，再决定列不列。
   *   列目录用的 `want_slash=1` 会给路径补尾斜杠 —— 对目录是 RFC 4991 的
   *   常规写法，对**文件**等于在问一个不存在的集合：宽松的服务端会回
   *   207 + 「你是空集合」，严格的回 404。前一种情况下，调用方看到的是
   *   `ok + 0 条`，而 nas_pull_tree() 的判据正是「列成功 ⇒ 是目录」，
   *   于是 mkdir_p() 建出一个空目录 —— 2026-10-29 真机「NAS 上的照片粘贴
   *   到 PS5，变成空文件夹、点开什么也没有」就是这个。
   *
   *   Depth:0 只回该资源自己一条、resourcetype 如实反映类型，报文极小；
   *   一次列目录多这一个请求，换掉一整类「文件被当成目录」的错。
   *   ⚠️ 探针路径**不带**尾斜杠，这正是它可信的前提。 */
  {
    char probe[MANGO_PATH_MAX];
    char *pb = NULL;
    size_t plen = 0;
    int pstatus = 0, pchunked = 0;
    long pclen = -1;

    wd_build_path(s, dir, probe, sizeof(probe), 0);
    if(!wd_request(s, "PROPFIND", probe,
                   "Depth: 0\r\nContent-Type: application/xml; charset=utf-8\r\n",
                   WD_PROPFIND_BODY, sizeof(WD_PROPFIND_BODY) - 1,
                   &pstatus, &pclen, &pchunked)) {
      wd_disconnect(s);
      return MANGO_ERR_GENERIC;
    }
    if(pstatus != 207 && pstatus != 200) {
      wd_err_status(s, pstatus, probe);
      wd_finish(s);
      return wd_status_err(pstatus);
    }
    if(!wd_read_all(s, &pb, &plen)) { wd_disconnect(s); return MANGO_ERR_GENERIC; }
    wd_finish(s);
    if(!wd_is_collection(pb)) {
      free(pb);
      wd_err(s, "webdav: not a directory");
      return MANGO_ERR_NOTFOUND;
    }
    free(pb);
  }

  wd_build_path(s, dir, path, sizeof(path), 1);
  if(!wd_request(s, "PROPFIND", path,
                 "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
                 WD_PROPFIND_BODY, sizeof(WD_PROPFIND_BODY) - 1,
                 &status, &clen, &chunked)) {
    wd_disconnect(s);
    return MANGO_ERR_GENERIC;
  }
  if(status != 207 && status != 200) {
    wd_err_status(s, status, path);
    wd_finish(s);
    return wd_status_err(status);
  }
  if(!wd_read_all(s, &body, &blen)) { wd_disconnect(s); return MANGO_ERR_GENERIC; }
  wd_finish(s);

  wd_url_dec(path, req_path, sizeof(req_path));
  wd_trim_slash(req_path);

  cur = body;
  while(cur && *cur) {
    size_t rlen = 0;
    const char *block = wd_next_response(&cur, &rlen);
    char *rb;
    char href_raw[MANGO_PATH_MAX], hp[MANGO_PATH_MAX];
    char name[MANGO_NAS_NAME_MAX], clbuf[64], lm[128];
    uint64_t sz = 0, mt = 0;
    int isdir;

    if(!block) break;
    rb = (char *)malloc(rlen + 1);
    if(!rb) break;
    memcpy(rb, block, rlen);
    rb[rlen] = 0;

    if(!wd_tag_text(rb, "href", href_raw, sizeof(href_raw))) { free(rb); continue; }
    wd_xml_unescape(href_raw);
    wd_href_path(href_raw, hp, sizeof(hp));
    wd_trim_slash(hp);
    if(!strcmp(hp, req_path)) {
      /* 请求目标**自己**这一条。是集合 ⇒ 正常，跳过（列目录本来会带上自己）；
         是普通文件 ⇒ 这个 URL 根本不是目录，必须**失败**。
         ★ 2026-10-29 真机：NAS 上的照片「粘贴」到 PS5，落地变成一个空文件夹、
         点开什么也没有。成因就是这里原先无条件 continue —— 服务端对文件做
         PROPFIND 同样回 207，报文里恰好**只有文件自己这一条**，跳过之后
         count=0 但函数仍返回 MANGO_OK；而 nas_pull_tree() 的判据正是
         「transfer_list() 成功 ⇒ 是目录」⇒ mkdir_p() 建出空目录，孩子 0 个，
         还向用户报成功。transfer_list() 的契约是「列目录」，对文件就该失败，
         把这条契约修正在源头，nas_scan_tree() 的分母判断也一并恢复。 */
      if(!wd_is_collection(rb)) {
        wd_err(s, "webdav: not a directory");
        free(rb);
        free(body);
        free(list);
        return MANGO_ERR_NOTFOUND;
      }
      free(rb);
      continue;
    }

    /* ★ 名字取 **href 的最后一段**，不取 displayname。
     *   href 是这个资源在服务器上的唯一标识；displayname 只是给人看的**标签**，
     *   服务器可以随意改写它（求过实体的 "&amp;"、大小写、Unicode NFC/NFD）。
     *   拿标签当路径去 GET，后果就是「列表里有、打开说找不到」——
     *   2026-10-29 真机 mp4 播不了报 webdav: not found，最可能的入口就是这条。
     *   目录列表本来就按 href 建路径，只有文件这一级用了标签，于是同一个名字
     *   在「列目录」时对、在「打开」时错 —— 这种只在文件级出错的分裂最容易被
     *   误判成后端坏了。 */
    wd_last_seg(hp, name, sizeof(name));
    if(!name[0]) { free(rb); continue; }
    if(wd_tag_text(rb, "getcontentlength", clbuf, sizeof(clbuf)))
      sz = strtoull(clbuf, NULL, 10);
    if(wd_tag_text(rb, "getlastmodified", lm, sizeof(lm)))
      mt = wd_date_epoch(lm);
    isdir = wd_is_collection(rb);

    if(transfer_list_push(&list, &n, &cap, name, sz, mt, 0, isdir) != MANGO_OK) {
      free(rb);
      free(body);
      free(list);
      return MANGO_ERR_NOMEM;
    }
    free(rb);
  }
  free(body);
  *out = list;
  *count = n;
  return MANGO_OK;
}

static mango_err_t
webdav_size(wd_t *s, uint64_t *size) {
  char path[MANGO_PATH_MAX];
  int status = 0, chunked = 0;
  long clen = -1;

  if(!size) return MANGO_ERR_GENERIC;
  if(!wd_connect(s)) return MANGO_ERR_GENERIC;
  wd_build_path(s, NULL, path, sizeof(path), 0);
  if(!wd_request(s, "HEAD", path, "", NULL, 0, &status, &clen, &chunked)) {
    wd_disconnect(s);
    return MANGO_ERR_GENERIC;
  }
  if(status == 200 && clen >= 0) {
    *size = (uint64_t)clen;
    wd_finish(s);
    return MANGO_OK;
  }
  wd_finish(s);

  /* Some servers reject HEAD; fall back to PROPFIND Depth:0. */
  {
    char *body = NULL;
    size_t blen = 0;
    if(!wd_connect(s)) return MANGO_ERR_GENERIC;
    if(!wd_request(s, "PROPFIND", path,
                   "Depth: 0\r\nContent-Type: application/xml; charset=utf-8\r\n",
                   WD_PROPFIND_BODY, sizeof(WD_PROPFIND_BODY) - 1,
                   &status, &clen, &chunked)) {
      wd_disconnect(s);
      return MANGO_ERR_GENERIC;
    }
    if(status == 207 || status == 200) {
      char tmp[64];
      if(!wd_read_all(s, &body, &blen)) { wd_disconnect(s); return MANGO_ERR_GENERIC; }
      wd_finish(s);
      if(wd_tag_text(body, "getcontentlength", tmp, sizeof(tmp))) {
        *size = strtoull(tmp, NULL, 10);
        free(body);
        return MANGO_OK;
      }
      free(body);
      wd_err(s, "webdav: size unknown");
      return MANGO_ERR_GENERIC;
    }
    wd_err_status(s, status, path);
    wd_finish(s);
    return wd_status_err(status);
  }
}

static mango_err_t
webdav_read(wd_t *s, uint64_t offset, void *buf, size_t len, size_t *got) {
  char path[MANGO_PATH_MAX], extra[96];
  int status = 0, chunked = 0;
  long clen = -1;

  if(!s || !buf || !got) return MANGO_ERR_GENERIC;
  *got = 0;
  if(!len) return MANGO_OK;
  if(!wd_connect(s)) return MANGO_ERR_GENERIC;
  wd_build_path(s, NULL, path, sizeof(path), 0);
  snprintf(extra, sizeof(extra), "Range: bytes=%llu-%llu\r\n",
           (unsigned long long)offset, (unsigned long long)(offset + (uint64_t)len - 1));
  if(!wd_request(s, "GET", path, extra, NULL, 0, &status, &clen, &chunked)) {
    wd_disconnect(s);
    return MANGO_ERR_GENERIC;
  }
  if(status != 200 && status != 206) {
    wd_err_status(s, status, path);
    wd_finish(s);
    return wd_status_err(status);
  }
  /* Server ignored Range: stream past the offset first. Slow but correct. */
  if(status == 200 && offset > 0) {
    if(!wd_skip(s, offset)) { wd_disconnect(s); return MANGO_ERR_GENERIC; }
  }
  if(!wd_body_read(s, buf, len, got)) { wd_disconnect(s); return MANGO_ERR_GENERIC; }
  wd_finish(s);
  return MANGO_OK;
}

static mango_err_t
webdav_remove(wd_t *s, const char *path, int is_dir) {
  char hp[MANGO_PATH_MAX];
  int status = 0, chunked = 0;
  long clen = -1;
  (void)is_dir;   /* HTTP DELETE handles files and collections alike */

  if(!wd_connect(s)) return MANGO_ERR_GENERIC;
  wd_build_path(s, path, hp, sizeof(hp), 0);
  if(!wd_request(s, "DELETE", hp, "", NULL, 0, &status, &clen, &chunked)) {
    wd_disconnect(s);
    return MANGO_ERR_GENERIC;
  }
  wd_drain(s);
  wd_finish(s);
  if(status == 200 || status == 204) return MANGO_OK;
  wd_err_status(s, status, hp);
  return wd_status_err(status);
}

static void
webdav_close(wd_t *s) {
  wd_disconnect(s);
}

/* ---- vtable adapters ------------------------------------------------- */

static const char *const webdav_schemes[] =
  { "webdav", "webdavs", "dav", "davs", NULL };

static int
webdav_be_open(transfer_ctx_t *c) {
  wd_state_t *st = (wd_state_t *)calloc(1, sizeof(*st));
  if(!st) {
    transfer_ctx_err(c, "out of memory");
    return 0;
  }
  st->fd = -1;      /* 懒连接：这里只把「未连接」状态立起来 */
  c->state = st;
  return 1;
}

static void
webdav_be_close(transfer_ctx_t *c) {
  wd_state_t *st = (wd_state_t *)c->state;
  if(st) {
    wd_t w = wd_of(c);
    webdav_close(&w);
    free(st);
    c->state = NULL;
  }
}

static mango_err_t
webdav_be_list(transfer_ctx_t *c, const char *d, transfer_entry_t **o, int *n) {
  wd_t w = wd_of(c);
  return webdav_list(&w, d, o, n);
}
static mango_err_t
webdav_be_size(transfer_ctx_t *c, uint64_t *sz) {
  wd_t w = wd_of(c);
  return webdav_size(&w, sz);
}
static mango_err_t
webdav_be_remove(transfer_ctx_t *c, const char *p, int is_dir) {
  wd_t w = wd_of(c);
  return webdav_remove(&w, p, is_dir);
}
static mango_err_t
webdav_be_read(transfer_ctx_t *c, uint64_t off, void *b, size_t l, size_t *g) {
  wd_t w = wd_of(c);
  return webdav_read(&w, off, b, l, g);
}

const transfer_backend_t mango_backend_webdav = {
  "webdav", SCHEME_WEBDAV, webdav_schemes,
  webdav_be_open, webdav_be_close, NULL,   /* GET 无每文件句柄，换文件不用作废 */
  webdav_be_list, webdav_be_size, webdav_be_remove, webdav_be_read, NULL
};
#endif /* MANGO_HAVE_WEBDAV */
