/* Transfer abstraction: local FS / SMB(libsmb2) / NFS(libnfs) all expose the
 * same "readable stream + listable directory" interface, so NAS browse / pull /
 * verify / atomic land / resume reuse ONE code path.
 *
 * Adding a backend means implementing the four functions below for that scheme
 * — no other module changes. SMB and NFS are compiled in only when the
 * libraries are vendored (MANGO_HAVE_LIBSMB2 / MANGO_HAVE_LIBNFS); without
 * them transfer_open() returns NULL for that scheme so the caller fails
 * cleanly instead of silently pretending the backend works.
 *
 * The local backend is the reference implementation and is what the host test
 * matrix exercises. The NAS backends connect lazily on first use (mirroring
 * the lazy local fd): transfer_open() only validates the URL shape and fills
 * transfer_nas_t, so URL errors fail at open time while real connection
 * errors surface on the first list/read/size call.
 *
 * transfer_url_parse() is deliberately macro-free: it is pure string work, so
 * it is compiled and host-tested on every build even when the NAS libraries
 * are absent. Only the connection/IO glue sits behind the HAVE_ guards, and
 * that part is verified on the SDK machine. */
#include "transfer.h"
#include "mango_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

#if defined(MANGO_HAVE_LIBSMB2)
/* ==================================================================== *
 * SMB 异步预读窗口 —— 为什么必须有这一层
 *
 * libsmb2 的 smb2_pread() 就是「提交一个 READ + 原地等到回应」（见
 * third_party/libsmb2/lib/sync.c：smb2_pread = smb2_pread_async +
 * wait_for_reply）。也就是说**同一时刻只有一个 READ 在空中**，WiFi 的
 * 带宽×延迟积根本填不满 —— 真机实测 NAS→PS5 只有 ~11MB/s，而同一条链路
 * PS5→PC 能到 40MB/s，瓶颈在并发度，不在链路、不在 NAS、不在写盘。
 *
 * 上游 owendswang/ps5-web-file-manager（GPLv3+，与本项目同源）用
 * smb2_pread_async 维持 8 个未完成请求解决了同一个问题。这里在
 * transfer_read() 的**同步**语义内做同样的事：提前提交 SMB_RA_SLOTS 个
 * READ，用 smb2_service() 驱动事件循环，攒到就交付。调用方（预取线程）
 * 一行都不用改。
 *
 * ★ 生命周期铁律：在途请求的缓冲区**只能**在 smb2_destroy_context() 之后
 *   释放 —— 迟到的回调会写进已 free 的内存。所以槽缓冲挂在
 *   transfer_src_t 上，由 smb_close() 释放；中途「重置窗口」只等、不 free。
 * ★ 作废机制：调用方跳位读取（或 transfer_repoint 换文件）时递增 gen，
 *   旧代请求回来后结果一律丢弃，绝不能把上一代的字节交给新偏移。
 * ==================================================================== */
/* 在途窗口 = SMB_RA_SLOTS × 每次 READ 的字节数（≤ SMB_RA_BLOCK_MAX）。
 *
 * 🪤 旧值 4 × 1MB = 4MB。它看起来「对千兆有线早就够了」（RTT 1ms 的带宽×
 *    延迟积才 125KB），但那个算法漏了两件事：
 *      (1) 窗口不是只为带宽服务的，更是为了**吸收抖动**：落盘那一侧（PS5
 *          内部 SSD 的写、其它进程的 IO）一停顿，链路就空转。窗口只有 4MB
 *          时一次 20ms 的落盘停顿就把管道抽干，而每次重新填满都要一个 RTT。
 *      (2) SMB2 的 credit 是按 64KB 一个算的（libsmb2: needed_credits =
 *          (count-1)/65536 + 1），MAX_CREDITS 是 1024 ⇒ 单次 READ 理论上
 *          可以到 64MB，credit 从来不是这里的限制；4MB 纯粹是我们自己按的。
 *    12 × 2MB = 24MB 是**申请量**，不是真能同时在链路上飞的量 —— 见下面
 *    credit 那条：实际在途被服务器授予的 credit 卡成 credits × 64KB。
 * ★ 每次 READ 的实际字节数仍取 min(服务端协商的 MaxReadSize, BLOCK_MAX)：
 *   服务端只报 64KB 时就发 64KB，超出协商值的 READ 会被直接拒。
 *
 * 🪤🪤 credit 才是真正的闸门，而且它会**静默**把我们的请求砍短（真机
 *    v1.0.13 上「12 个在途、本块已收 0 字节、连续 60s 无回包」就是这么来的）：
 *      · libsmb2 的 smb2_pread_async() 里有
 *            if (needed_credits > smb2->credits) count = smb2->credits * 65536;
 *        请求被砍短**没有任何返回值或错误告诉我们**；credits==0 时 count 变 0，
 *        于是发出一个长度为 0 的 READ（credit_charge 还因为 uint16 溢出成 0，
 *        连"付不起就不发"这道闸都拦不住它）。
 *      · 旧代码按 want 推进 next_off，被砍掉的那一截**永远没人再读**，而
 *        got < want 又被当成 EOF ⇒ 复制出来的文件是静默截断的。
 *      · 更糟的是我们按"提交了 12 个"就算 12 个在途，其实大部分还压在
 *        outqueue 里等 credit，服务器根本没收到 —— 于是 60s 无回包。
 *    对策：自己用 smb2_get_credits() 定长度、没有 credit 就不提交，并且用
 *    文件大小（fstat）判真 EOF，而不是靠"读短了"。 */
/* 🪤🪤🪤 2026-10-16 真机日志**推翻**了上面「credit 不够」的归因：
 *    stall 那一次 smb2_get_credits() = 671，而 12 × 2MB 只需 384 个 —— 充足。
 *    ⇒ 12 个请求**真的发出去了**，卡住的是**对端 NAS 不回包**，
 *      不是我们压在 outqueue 里没发出去。闸门（credits<=0 时不提交）那次根本没触发。
 *
 *    对照上游 owendswang/ps5-web-file-manager（GPL-3.0，与本项目同源）src/：
 *      · 窗口 = SMB_IO_REQUESTS(8) × SMB_IO_BLOCK(1MB) = **8MB**（我们曾是 24MB）
 *      · 所有 SMB 操作（open/read/opendir/close）串在同一把**全局锁**里
 *      · smb2_set_timeout(ctx, **5**) —— 请求级超时
 *      · 小文件不开流水线（window=1）；小请求把块降到 256KB
 *    单任务工具一次只发一个 READ，所以从来不会把家用 NAS 打爆 —— 我们原来会。
 *
 *    现在的口径：默认窗口对齐上游的 8MB；一旦 stall 过就**降级到 1**，
 *    并**跨连接保持**（degraded 挂在 ra 上，不受 smb_ra_reset() 影响）。 */
#define SMB_RA_SLOTS      8
#define SMB_RA_BLOCK_MAX  (1u * 1024u * 1024u)
#define SMB_RA_BLOCK_DEF  (64u * 1024u)
#define SMB_RA_PUMP_MS    1000
/* ★ 「毫无进展」的最长等待 = **单调时钟上的 60 秒**，不是一次 transfer_read
 *   的总时长：只要还在往里收数据就永不超时，真断流 60s 才放弃。
 *
 * 🪤🪤 2026-10-19 修掉的真 bug：这里原来数的是「pump 循环次数」
 *    （`if(++spins > SMB_RA_MAX_SPINS)`），文案却写「连续 60 秒」。
 *    v1.0.19 真机日志当场证伪 —— 同一个日志秒里连打两条「连续 60 秒一个回包
 *    都没有」（1792381139 那一秒：在途 8 个 → 降级重连 → 在途 1 个）。
 *    两次各等 60 秒不可能落在同一秒。真相是 socket 一热（poll 反复报就绪、
 *    smb2_service 却还没凑出一个完整 PDU），60 次循环几毫秒就跑完 ⇒
 *    请求**还在飞**就被判成「对端断流」，于是降级、重连、再毫秒级判死。
 *    这就是「从 NAS 复制必失败、从 NAS 装 PKG 必失败」的真正原因，
 *    与块大小 / 槽位数 / dialect 全都无关（所以 v1.0.13→v1.0.19 改了那么多
 *    都没用）。
 *
 * ★ 为什么敢放心等：libsmb2 自带 PDU 级超时 —— smb2_set_timeout(ctx, 5) 后，
 *   每次 smb2_service() 返回前都会调 smb2_timeout_pdus()，请求真没回包，
 *   5 秒后一定用 SMB2_STATUS_IO_TIMEOUT 叫醒回调。所以我们不需要自己掐表，
 *   60 秒只是「回调一直在动但一直读不出东西」的兜底。 */
#define SMB_STALL_MS      60000
/* ★ stall 之后的重试预算。为什么只给 1 次：降级到单请求窗口之后**再** stall，
 *   基本可以判定是对端 NAS 真的不回包，而不是被我们的突发打爆 —— 那时继续
 *   重试只是把界面吊死。而 1 次重试足以把「我们把它打崩了」这一类救回来，
 *   因为降级之后发出的量级跟单任务工具一样。 */
#define SMB_READ_STALL_RETRY 1
/* socket 收发缓冲：★ 并发窗口只是"愿意发多少"，真正能同时在链路上飞多少由
   TCP 窗口决定，而 TCP 窗口上界就是 SO_RCVBUF。40MB/s @ 20ms RTT 的带宽×
   延迟积是 800KB —— 512KB 的收缓冲正好卡在这个量级（真机 11MB/s 的现场），
   收缓冲不放够，预读窗口开再大也吃不到。现在直接给 8MB（10× 过供）：
   SO_RCVBUF 只是**上界**，内核仍按实际流量分配内存，多给不会白占。 */
#define SMB_SOCK_BUF      (8u * 1024u * 1024u)
/* ★ libsmb2 的**请求级**超时（秒）。上游 owendswang/ps5-web-file-manager 里的
 *   smb.c 就是 smb2_set_timeout(ctx, 5)，这是它区别于我们旧代码的关键一处 ——
 *   我们从来没设过，于是「对端彻底不回包」这件事只能靠我们自己的 60 秒
 *   粗粒度兜底去发现，慢且粒度太粗（一次 stall 直接判整个任务失败）。
 * 🪤 别把它理解成「5 秒读不完就报错」：这个超时指的是**连接上没有任何
 *    一个 PDU 回来**的空闲时长；大文件持续有数据在收就永远不会触发。
 * 🪤 也别设成 1~2 秒：家用 NAS 的机械盘在随机读上有几百毫秒到数秒的
 *    寻道 + 缓存抖动，太激进会把正常传输误判成断流。5 秒是上游在真机上
 *    跑过的取值，直接采用，不自作聪明。 */
#define SMB2_TIMEOUT_SEC  5

struct smb_ra_op;
typedef struct smb_ra_op smb_ra_op_t;

typedef struct {
  unsigned char *buf[SMB_RA_SLOTS];
  uint32_t       bufsz[SMB_RA_SLOTS];  /* buf[i] 实际分配字节数；block 会动态缩小 */
  uint32_t       want[SMB_RA_SLOTS];
  uint32_t       got[SMB_RA_SLOTS];
  uint64_t       off[SMB_RA_SLOTS];    /* 这一槽实际请求的偏移（判 EOF 要用） */
  size_t         used[SMB_RA_SLOTS];
  int            state[SMB_RA_SLOTS];  /* 0 空闲 1 在途 2 完成 3 出错 */
  int            busy[SMB_RA_SLOTS];   /* 有请求在飞（含被作废的旧代） */
  unsigned       sgen[SMB_RA_SLOTS];
  int            err[SMB_RA_SLOTS];
  smb_ra_op_t   *op[SMB_RA_SLOTS];     /* 回调用的 (ra, idx) 配对，激活时填好 */
  int            head;
  int            inflight;
  unsigned       progress;     /* 每来一个回调 +1：读端"还在动"的唯一证据 */
  uint64_t       next_off;   /* 下一个要提交的偏移 */
  uint64_t       deliver;    /* 下一个要交给调用方的偏移 */
  uint32_t       block;
  uint64_t       size;       /* fstat 出来的文件大小；判真 EOF 的唯一依据 */
  int            size_known;
  unsigned       gen;
  int            active;
  int            eof;
  int            dead;
  /* ★ stall 过一次（连续 SMB_STALL_MS 毫秒零回包）⇒ 此后**跨连接**都只发
   *   1 个 READ。挂在 ra 而不是窗口局部变量上，就是为了让
   *   smb_ra_reset()（它只清槽状态）**不会**把它一起清掉 —— 否则每次
   *   重新 activate 又拿 24MB 去砸那台已经扛不住的 NAS。 */
  int            degraded;
} smb_ra_t;

struct smb_ra_op {
  smb_ra_t *ra;
  int       idx;
};

/* ---- 按 NAS 主机记住「这台扛不住大窗口」 ----
 *
 * ★ 为什么必须是**全局**而不是挂在 ra 上：一次复制 stall 之后，上层会
 *   **新建** transfer_src_t 来重来（新建的 ra 状态是清零的）⇒ 挂在 ra 上
 *   等于没记，第二回合又拿 24MB 去轰同一台已经扛不住的 NAS，于是
 *   「重试几次都失败」，看起来像网络抽风而不是我们能修的东西。
 * ★ 为什么记**主机**而不是完整路径：同一台 NAS 上的不同共享走的是同一块
 *   网卡、同一颗硬盘 —— 一个共享扛不住，别的也扛不住。
 * ★ 为什么不直接一律开小窗口：局域网 + 好 NAS 上，8 槽窗口是实打实的吞吐
 *   来源（见上面「吸收抖动」那条）。只有**证据说它扛不住**才收窄。 */
#define SMB_RA_HOST_MAX 8
typedef struct {
  char host[128];
  int  degraded;
} smb_ra_host_t;

static smb_ra_host_t g_ra_hosts[SMB_RA_HOST_MAX];
static pthread_mutex_t g_ra_hosts_lock = PTHREAD_MUTEX_INITIALIZER;

/* 这台主机之前 stall 过吗？有 ⇒ 本次连接从 1 槽起步。 */
static int
smb_ra_host_degraded(const char *host) {
  int i, out = 0;
  if(!host || !*host) return 0;
  pthread_mutex_lock(&g_ra_hosts_lock);
  for(i = 0; i < SMB_RA_HOST_MAX; i++) {
    if(g_ra_hosts[i].host[0] && !strcmp(g_ra_hosts[i].host, host)) {
      out = g_ra_hosts[i].degraded;
      break;
    }
  }
  pthread_mutex_unlock(&g_ra_hosts_lock);
  return out;
}

/* stall 过一次 ⇒ 记住这台主机，之后所有到它的传输都走保守窗口。 */
static void
smb_ra_host_mark(const char *host) {
  int i, slot = -1;
  if(!host || !*host) return;
  pthread_mutex_lock(&g_ra_hosts_lock);
  for(i = 0; i < SMB_RA_HOST_MAX; i++) {
    if(g_ra_hosts[i].host[0] && !strcmp(g_ra_hosts[i].host, host)) {
      slot = i;
      break;
    }
    if(slot < 0 && !g_ra_hosts[i].host[0]) slot = i;   /* 先记下第一个空位 */
  }
  if(slot >= 0) {
    snprintf(g_ra_hosts[slot].host, sizeof(g_ra_hosts[slot].host), "%s", host);
    g_ra_hosts[slot].degraded = 1;
  }
  pthread_mutex_unlock(&g_ra_hosts_lock);
}

/* 作废并等待在途请求收完；结果一律丢弃。 */
static void smb_ra_reset(transfer_src_t *s);
#endif

#if defined(MANGO_HAVE_LIBSMB2)
/* smb2.h (raw protocol defs: SMB2_GUID_SIZE, smb2_lease_key, ...) must come
 * first -- libsmb2.h references those names. */
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#endif
#if defined(MANGO_HAVE_LIBNFS)
#include <nfsc/libnfs.h>
#endif

typedef enum { SCHEME_LOCAL = 0, SCHEME_SMB, SCHEME_NFS } transfer_scheme_t;

struct transfer_src {
  transfer_scheme_t scheme;
  char   path[MANGO_PATH_MAX];
  int    fd;        /* local: lazy, opened on first transfer_read */
  int    opened;
  char   err[256];  /* last backend error, human-readable (smb2/nfs_get_error) */
#if defined(MANGO_HAVE_LIBSMB2) || defined(MANGO_HAVE_LIBNFS)
  transfer_nas_t nas;   /* filled at transfer_open when scheme != local */
#endif
#if defined(MANGO_HAVE_LIBSMB2)
  struct smb2_context *smb2;    /* lazy: connected on first use */
  struct smb2fh       *smb2_fh; /* lazy: file opened on first read */
  smb_ra_t             ra;      /* 异步预读窗口；缓冲由 smb_close 释放 */
#endif
#if defined(MANGO_HAVE_LIBNFS)
  struct nfs_context  *nfs;     /* lazy: mounted on first use */
  struct nfsfh        *nfs_fh;  /* lazy: file opened on first read */
#endif
};

/* Record why the NAS backend failed. The API layer forwards this verbatim to
 * the UI, so "NAS 联不上" becomes "mountd refused export" / "auth failed" /
 * "no route to host" instead of a bare "list failed". */
static void
set_err(transfer_src_t *s, const char *msg) {
  if(!s) return;
  snprintf(s->err, sizeof(s->err), "%s", (msg && *msg) ? msg : "unknown error");
}

const char *
transfer_last_error(transfer_src_t *s) {
  return (s && s->err[0]) ? s->err : NULL;
}

static int
parse_scheme(const char *scheme) {
  if(!scheme || !*scheme) return SCHEME_LOCAL;
  if(!strncmp(scheme, "local", 5)) return SCHEME_LOCAL;
  if(!strncmp(scheme, "smb", 3))   return SCHEME_SMB;
  if(!strncmp(scheme, "nfs", 3))   return SCHEME_NFS;
  return SCHEME_LOCAL; /* unknown -> treat as local */
}

static int
scheme_is_known(const char *scheme) {
  if(!scheme || !*scheme) return 1; /* empty/NULL -> default local */
  if(!strncmp(scheme, "local", 5)) return 1;
  if(!strncmp(scheme, "smb", 3))   return 1;
  if(!strncmp(scheme, "nfs", 3))   return 1;
  return 0;
}

int
transfer_scheme_supported(const char *scheme) {
  int sc = parse_scheme(scheme);

  /* parse_scheme folds unknown schemes into SCHEME_LOCAL (transfer_open's
   * "unrecognised prefix = local path" contract), but "supported" must not:
   * an unsupported scheme is a different failure from a bad local path. */
  if(!scheme_is_known(scheme)) return 0;
  if(sc == SCHEME_LOCAL) return 1;
#if defined(MANGO_HAVE_LIBSMB2)
  if(sc == SCHEME_SMB) return 1;
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(sc == SCHEME_NFS) return 1;
#endif
  return 0; /* known-but-unbuilt backend */
}

/* '/' and '\' are interchangeable as path separators here.
 *
 * Windows users both type and paste backslash-separated SMB paths, and the NAS
 * address field is exactly where that lands: "192.168.1.100\PS5_Games". Before
 * this the server-name scan ran straight past the backslash to end-of-string,
 * so the whole thing went to getaddrinfo, which answered
 *   Invalid address:192.168.1.100\PS5_Games Can not resolve into IPv4/v6.
 * -- an error naming neither the offending character nor the field at fault.
 *
 * ⚠️ Credentials are peeled off BEFORE any of this and are deliberately NOT
 * translated: a '\' is a legal password character, not a separator.
 * ⚠️ smb_path() and nfs_list() call this too, so fixing it here covers every
 * caller at once -- that is the point of keeping the rule in one place. */
static int
path_sep(char c) { return c == '/' || c == '\\'; }

int
transfer_url_parse(const char *url, transfer_nas_t *out) {
  const char *server, *share, *p;
  size_t      sl, shl;

  if(!url || !*url || !out) return 0;
  memset(out, 0, sizeof(*out));
  p = url;

  /* Strip an optional scheme:// prefix (smb://, nfs://) and a leading //. */
  if(!strncmp(p, "smb://", 6) || !strncmp(p, "nfs://", 6)) p += 6;
  if(!strncmp(p, "//", 2)) p += 2;

  /* Optional "user[:pass]@" credentials prefix. The server scan below stops at
   * the first ':' which would otherwise grab "user" as the hostname, so the
   * credentials must be peeled off before it. */
  {
    const char *at = strchr(p, '@');
    if(at) {
      const char *sep = memchr(p, ':', (size_t)(at - p));
      size_t ul = sep ? (size_t)(sep - p) : (size_t)(at - p);
      if(ul > sizeof(out->user) - 1) ul = sizeof(out->user) - 1;
      memcpy(out->user, p, ul); out->user[ul] = 0;
      if(sep) {
        size_t pl = (size_t)(at - (sep + 1));
        if(pl > sizeof(out->pass) - 1) pl = sizeof(out->pass) - 1;
        memcpy(out->pass, sep + 1, pl); out->pass[pl] = 0;
      }
      p = at + 1;
    }
  }

  server = p;
  while(*p && !path_sep(*p) && *p != ':') p++;
  sl = (size_t)(p - server);
  if(!sl) return 0;

  /* host[:port]/export/path -- a colon after the server is an optional port
   * (digits only; "host:x" with a non-digit is still rejected). */
  if(*p == ':') {
    int v = 0, digits = 0;
    p++;
    while(*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; }
    out->port = v;
    /* 0 位数字本身不算错：`host:/export` 是合法的「不写端口」写法（冒号只是分隔符）。
       只挡两种真·坏输入 —— 冒号后面既不是数字也不是分隔符（"host:x"），以及冒号后
       什么都没有（"host:"）。⚠️ 后者以前是被「share 必须非空」顺带挡住的，现在缺
       share 合法了，必须自己挡，否则 "host:" 会被当成「只填服务器」悄悄接受。 */
    if(*p && !path_sep(*p)) return 0;
    if(!digits && !*p) return 0;
  }
  if(path_sep(*p)) p++;             /* single separator before the share */

  share = p;
  while(*p && !path_sep(*p)) p++;
  shl = (size_t)(p - share);
  /* 缺 share 不再是错误：标成 server_only，SMB 后端改连 IPC$ 去枚举共享名。
     这里以前是 `return 0`，上层把它翻成一句 "cannot open source (bad path)" ——
     用户只填 NAS 的 IP（或末尾多打了个 "/"）时撞的正是这条，且看不出该改什么。 */
  if(!shl) out->server_only = 1;
  if(path_sep(*p)) p++;             /* remainder (may be empty) is the path */

  if(sl  >= sizeof(out->server)) sl  = sizeof(out->server)  - 1;
  if(shl >= sizeof(out->share))  shl = sizeof(out->share)   - 1;
  memcpy(out->server, server, sl); out->server[sl] = 0;
  memcpy(out->share,  share,  shl); out->share[shl]  = 0;
  /* The remainder keeps whatever the user typed, so translate it on the way in:
     every consumer downstream (libsmb2 create name, libnfs export path) wants
     '/', and a stray '\' that reaches SMB2 is at best a literal filename
     character -- silently opening the wrong thing, which is worse than failing. */
  {
    size_t i = 0;
    for(; p[i] && i < sizeof(out->path) - 1; i++)
      out->path[i] = path_sep(p[i]) ? '/' : p[i];
    out->path[i] = 0;
  }
  return 1;
}

transfer_src_t *
transfer_open(const char *scheme, const char *path) {
  transfer_src_t *s;
  int             sc = parse_scheme(scheme);

  /* Backends not compiled in fail loudly at open time. */
#if !defined(MANGO_HAVE_LIBSMB2)
  if(sc == SCHEME_SMB) return NULL;
#endif
#if !defined(MANGO_HAVE_LIBNFS)
  if(sc == SCHEME_NFS) return NULL;
#endif
  if(!path || !*path) return NULL;

  s = calloc(1, sizeof(*s));
  if(!s) return NULL;
  s->scheme = sc;
  s->fd = -1;
#if defined(MANGO_HAVE_LIBSMB2) || defined(MANGO_HAVE_LIBNFS)
  if(sc == SCHEME_SMB || sc == SCHEME_NFS) {
    if(!transfer_url_parse(path, &s->nas)) { free(s); return NULL; }
  }
#endif
  strncpy(s->path, path, MANGO_PATH_MAX - 1);
  s->path[MANGO_PATH_MAX - 1] = 0;
  return s;
}

/* Point an already-open source at another path, reusing the connection.
 * See transfer.h for why; the short version is that the SMB/NFS contexts live
 * inside this struct, so transfer_open() per file would reconnect per file. */
int
transfer_repoint(transfer_src_t *s, const char *path) {
  if(!s || !path || !*path) return 0;

  if(s->scheme == SCHEME_LOCAL) {
    /* No connection to reuse, but the local fd was opened lazily from the OLD
     * path and would otherwise serve the wrong file. */
    if(s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->opened = 0;
  } else {
#if defined(MANGO_HAVE_LIBSMB2) || defined(MANGO_HAVE_LIBNFS)
    transfer_nas_t tmp;

    if(!transfer_url_parse(path, &tmp)) return 0;
    /* Same server / share / credentials / port, or the connection must not be
     * reused: a tree connect is bound to ONE share, so pointing a connection
     * at another share would read from the wrong one. */
    if(strcmp(s->nas.server, tmp.server) || strcmp(s->nas.share, tmp.share) ||
       strcmp(s->nas.user, tmp.user) || strcmp(s->nas.pass, tmp.pass) ||
       s->nas.port != tmp.port || s->nas.server_only != tmp.server_only ||
       s->nas.server_only) return 0;
    s->nas = tmp;
#else
    return 0;   /* backend not compiled in; transfer_open already refused it */
#endif
#if defined(MANGO_HAVE_LIBSMB2)
    /* 换文件 = 换 fh：先作废预读窗口（旧的在途 READ 属于上一个文件，
       结果必须丢弃），否则会拿到上一份文件的字节。 */
    smb_ra_reset(s);
    if(s->smb2_fh) { smb2_close(s->smb2, s->smb2_fh); s->smb2_fh = NULL; }
#endif
#if defined(MANGO_HAVE_LIBNFS)
    if(s->nfs_fh) { nfs_close(s->nfs, s->nfs_fh); s->nfs_fh = NULL; }
#endif
  }

  strncpy(s->path, path, MANGO_PATH_MAX - 1);
  s->path[MANGO_PATH_MAX - 1] = 0;
  s->err[0] = 0;
  return 1;
}

/* Shared list-append used by all three backends. */
static mango_err_t
list_push(transfer_entry_t **list, int *n, int *cap,
          const char *name, uint64_t size, uint64_t mtime, uint32_t mode, int is_dir) {
  size_t nl;

  if(*n == *cap) {
    int newcap = *cap ? *cap * 2 : 32;
    transfer_entry_t *tmp = realloc(*list, (size_t)newcap * sizeof(**list));
    if(!tmp) return MANGO_ERR_NOMEM;
    *list = tmp; *cap = newcap;
  }
  memset(&(*list)[*n], 0, sizeof((*list)[*n]));
  nl = strlen(name);
  if(nl > sizeof((*list)[*n].name) - 1) nl = sizeof((*list)[*n].name) - 1;
  memcpy((*list)[*n].name, name, nl);
  (*list)[*n].name[nl] = 0;
  (*list)[*n].size = size;
  (*list)[*n].mtime = mtime;
  (*list)[*n].mode = mode;
  (*list)[*n].is_dir = is_dir ? 1 : 0;
  (*n)++;
  return MANGO_OK;
}

#if defined(MANGO_HAVE_LIBSMB2)
/* 发起一次连接尝试。0 成功（s->smb2 已就位）/-1 失败（已销毁并置空）。
 *
 * ★ password 的 NULL 与 "" 在 libsmb2 里发的是**不同的 NTLM 报文**（上游
 *   smb.c:connect_address 的原话："NULL and \"\" select different NTLM
 *   messages in libsmb2"），所以没配密码的 NAS 必须两级都试。只试一级时，
 *   一部分 NAS（尤其是开了 guest 的 Samba）会稳定拒绝其中一种 —— 真机日志里
 *   「同一个 share 上一秒 0xc00000cc、下一秒 connected」就是这个形态。 */
static int
smb_connect_once(transfer_src_t *s, const char *target, const char *share,
                 const char *password) {
  const char *user = s->nas.user[0] ? s->nas.user : NULL;

  s->smb2 = smb2_init_context();
  if(!s->smb2) return -1;
  /* ★ 请求级超时兜底（对齐上游 smb.c）。放在 init 之后、connect 之前：
     协商过程中的 PDU 同样受它保护，否则卡在 Negotiate 阶段时无人接手。 */
  smb2_set_timeout(s->smb2, SMB2_TIMEOUT_SEC);
  /* ★ 上游 smb.c 里我们一直没照搬的一处：协商时声明支持签名，缺省是不声明。
     不签名时部分 NAS / Windows 共享会在**后续的大 READ** 上直接不回包 ——
     协商能过、一读就死，现象和"断流"完全一样，极难归因。按上游口径打开。 */
  smb2_set_security_mode(s->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  /* Empty strings = anonymous; the URL "user:pass@host/share" prefix fills
   * these when the NAS bookmark carries credentials. */
  if(user) smb2_set_user(s->smb2, user);
  smb2_set_password(s->smb2, password);
  if(smb2_connect_share(s->smb2, target, share, user) < 0) {
    /* 真机上「SMB 超时」和「连不上」在界面上是同一句话，排查时却完全是两个
       方向：前者是读端断流，后者是地址/共享名/账号。这里落台账分开。 */
    const char *le = smb2_get_error(s->smb2);
    mango_log("smb: connect %s share=%s auth=%s failed: %s", target, share,
              password ? "NTLM" : "anonymous", (le && le[0]) ? le : "?");
    set_err(s, le);                     /* e.g. NT_STATUS_LOGON_FAILURE */
    smb2_destroy_context(s->smb2);
    s->smb2 = NULL;
    return -1;
  }
  return 0;
}

static mango_err_t
smb_ensure_connected(transfer_src_t *s) {
  char        target[320];
  const char *share;
  int         attempts, attempt;

  if(s->smb2) return MANGO_OK;
  /* libsmb2 accepts "<host>[:<port>]" in the server string and falls back to
   * 445 — that is the ONLY way to give it a non-default port (there is no
   * smb2_set_port in this vendored tree), so splice the bookmark's port in
   * here. Without this the UI's 端口 field was silently ignored for SMB while
   * NFS honoured it, which is the kind of asymmetry that reads as "SMB 不通". */
  if(s->nas.port > 0)
    snprintf(target, sizeof(target), "%s:%d", s->nas.server, s->nas.port);
  else
    snprintf(target, sizeof(target), "%s", s->nas.server);
  /* 只填了服务器 ⇒ 连 IPC$：它的根目录在 Windows / 多数 NAS 上会把共享名当目录项
     列出来 —— 用户从此能自己发现共享名，而不是收到一句 bad path 就卡住。 */
  share = s->nas.share[0] ? s->nas.share : "IPC$";
  /* 配了密码就只走 NTLM；没配密码则先空密码 NTLM，再退到匿名。 */
  attempts = s->nas.pass[0] ? 1 : 2;
  for(attempt = 0; attempt < attempts; attempt++) {
    if(!smb_connect_once(s, target, share, attempt ? NULL : s->nas.pass)) {
      /* 成功：上一次失败留下的错误串必须清掉，否则调用方读到的 last_error
         是"上一次尝试"的失败原因，界面上会显示一条根本没发生的错误。 */
      s->err[0] = 0;
      mango_log("smb: connected %s share=%s auth=%s dialect=0x%04x "
                "max_read=%u max_write=%u",
                target, share, attempt ? "anonymous" : "NTLM",
                (unsigned)smb2_get_dialect(s->smb2),
                (unsigned)smb2_get_max_read_size(s->smb2),
                (unsigned)smb2_get_max_write_size(s->smb2));
      {
    /* ★ 吞吐调优（口径来自上游 owendswang/ps5-web-file-manager 的 smb.c，其又参考
     *   zftpd 的 PS5 TCP 缓冲经验）：默认 socket 缓冲偏小，同步 1MB pread 一发一收
     *   的往返延迟就是全部速度 —— 真机实测 NAS→PS5 只有 10MB/s。512KB 收发缓冲
     *   让 TCP 窗口在局域网上撑得起来。设不上也无妨（内核会自动调）。 */
        int sock = smb2_get_fd(s->smb2);
        int buf = (int)SMB_SOCK_BUF;
        if(sock >= 0) {
          (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
          (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        }
      }
      return MANGO_OK;
    }
  }
  return MANGO_ERR_NOTFOUND;
}

/* Path inside the connected share: the explicit dir, else the URL remainder,
 * else the empty name (= the share root).
 * The API layer forwards the frontend's full "host/share/rest" URL as `dir`
 * (that is what the UI path bar holds), so parse it back into share-relative
 * form before opening — otherwise "nas.lan/games/sub" is taken literally and
 * opendir fails (this is exactly why NAS browse appeared dead on the device).
 *
 * 🪤 SMB2 names the share root with an EMPTY create name, and libsmb2 rewrites
 * every '/' into '\' verbatim (see lib/smb2-cmd-create.c). A leading slash
 * therefore arrives at the server as a leading backslash and the answer is
 * STATUS_INVALID_PARAMETER — i.e. "/" does NOT mean "the share root" here, it
 * means "this request is malformed". That single character is why listing a
 * share root always failed (502 nas_list_failed, "NAS 连接失败：未知原因")
 * while descending into "subdir" worked: the remainder had no leading slash.
 * Strip leading slashes; interior separators are fine, the library converts
 * them. Proved by .build/libsmb2-uaf-probe.c step A/B on a real smbd. */
static void
smb_path(transfer_src_t *s, const char *dir, char *buf, size_t bufsz) {
  const char *p = dir ? dir : (s->nas.path[0] ? s->nas.path : "");
  if(dir) {
    transfer_nas_t tmp;
    if(transfer_url_parse(dir, &tmp))
      p = tmp.path;
  }
  /* Both separators, and translate interior ones: the `dir == NULL` fallback
     and the "URL parse failed" fallback can both still carry backslashes, and a
     leading '\' at the share root is the same STATUS_INVALID_PARAMETER as a
     leading '/' (see the note above). */
  while(path_sep(*p)) p++;
  {
    size_t i = 0;
    for(; p[i] && i < bufsz - 1; i++) buf[i] = path_sep(p[i]) ? '/' : p[i];
    buf[i] = 0;
  }
}

static mango_err_t
smb_list(transfer_src_t *s, const char *dir,
         transfer_entry_t **out, int *count) {
  struct smb2dir    *d;
  struct smb2dirent *ent;
  transfer_entry_t  *list = NULL;
  int               n = 0, cap = 0;
  mango_err_t       rc;
  char              target[MANGO_PATH_MAX];

  rc = smb_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  smb_path(s, dir, target, sizeof(target));
  d = smb2_opendir(s->smb2, target);
  if(!d) {
    /* libsmb2 already recorded a precise reason ("Opendir failed with
     * (0xc0000034) STATUS_OBJECT_NAME_NOT_FOUND"). Forward it: the API layer
     * puts it in error_arg, so a failed browse is diagnosable from the UI
     * instead of the blank "未知原因" it used to show. */
    set_err(s, smb2_get_error(s->smb2));
    return MANGO_ERR_NOTFOUND;
  }
  while((ent = smb2_readdir(s->smb2, d))) {
    if(!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) continue;
    rc = list_push(&list, &n, &cap, ent->name,
                   (uint64_t)ent->st.smb2_size,
                   (uint64_t)ent->st.smb2_mtime,
                   0,  /* SMB2 stat has no Unix mode bits */
                   ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
    if(rc != MANGO_OK) { free(list); smb2_closedir(s->smb2, d); return rc; }
  }
  smb2_closedir(s->smb2, d);
  *out = list;
  *count = n;
  return MANGO_OK;
}

static mango_err_t
smb_size(transfer_src_t *s, uint64_t *size) {
  struct smb2_stat_64 st;
  char                target[MANGO_PATH_MAX];
  mango_err_t         rc;

  rc = smb_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  smb_path(s, NULL, target, sizeof(target));
  if(smb2_stat(s->smb2, target, &st) < 0) return MANGO_ERR_NOTFOUND;
  *size = (uint64_t)st.smb2_size;
  return MANGO_OK;
}

/* 远端删除。★ 必须存在：本地的 unlink()/rmdir() 打在 "host/share/x" 这种
 * 相对路径上时，errno 是 ENOENT —— 界面上表现为「点了删除，文件还在」，
 * 而且**没有任何错误**（删除任务按「已删除」汇报）。所以远端必须走 SMB 的
 * SET_INFO/DELETE_ON_CLOSE，不能借道本地 POSIX。
 *
 * 目录用 rmdir（只能删空目录），文件用 unlink；libsmb2 对两者是不同调用，
 * 且对调会拿到 STATUS_DIRECTORY_NOT_EMPTY / STATUS_FILE_IS_A_DIRECTORY，
 * 反过来正好是「这是目录还是文件」最可靠的判据。 */
static mango_err_t
smb_remove(transfer_src_t *s, const char *path, int is_dir) {
  char        target[MANGO_PATH_MAX];
  mango_err_t rc;

  rc = smb_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  smb_path(s, path, target, sizeof(target));
  if(is_dir ? smb2_rmdir(s->smb2, target) : smb2_unlink(s->smb2, target)) {
    set_err(s, smb2_get_error(s->smb2));
    return MANGO_ERR_GENERIC;
  }
  return MANGO_OK;
}

/* ---- 异步预读窗口的实现 ------------------------------------------------ */

/* 单调时钟（毫秒）。⚠️ 必须用单调时钟：墙钟会被 NTP / 时区 / 用户改时间拨动，
 *    而这个数只用来算「距上次有回包过了多久」。没有 CLOCK_MONOTONIC 的构建
 *    （少数 MinGW 工具链）退回秒级墙钟 —— 精度差但方向仍对，不会把 0 当回事。 */
static unsigned long long
mono_ms(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
#endif
  return (unsigned long long)time(NULL) * 1000ull;
}

/* 把 libsmb2 异步回调的 status 翻成 errno。
 *
 * 🪤 这里有**两套**编码混在一起，不区分就会出两种坏结果：
 *    · 传输层错误：libsmb2 传的是 -errno（小负数，如 -ETIMEDOUT）
 *    · SMB 错误：传的是 NTSTATUS（如超时的 0xC00000B5，当 int 看是
 *      -1073742155）。旧代码一律 `-status` 当 errno ⇒ 得到 1073742155 这种
 *      垃圾值，`stalled = (err == ETIMEDOUT)` 永远不成立 ⇒ **超时不重试**，
 *      而且界面上显示「errno=1073742155」。
 *    判据：-4096..-1 是 errno 形态，其余负数按 NTSTATUS 查表。 */
static int
smb_status_errno(int status) {
  if(status >= 0) return 0;
  if(status > -4096) return -status;
#if defined(MANGO_HAVE_LIBSMB2)
  {
    int e = (int)nterror_to_errno((uint32_t)status);
    return e > 0 ? e : EIO;
  }
#else
  return EIO;
#endif
}

static void
smb_ra_cb(struct smb2_context *ctx, int status, void *cmd_data, void *priv) {
  smb_ra_op_t *op = (smb_ra_op_t *)priv;
  smb_ra_t    *ra = op->ra;
  int          i  = op->idx;

  (void)ctx; (void)cmd_data;
  ra->busy[i] = 0;
  if(ra->inflight > 0) ra->inflight--;
  ra->progress++;          /* 有回包 = 链路在动，读端的空闲计时归零 */
  /* 旧代请求（调用方跳位 / 换了文件）的结果一律丢弃。 */
  if(ra->sgen[i] != ra->gen) return;
  if(status < 0) {
    ra->err[i] = smb_status_errno(status);
    ra->state[i] = 3;
    return;
  }
  ra->got[i] = (uint32_t)status;
  ra->state[i] = 2;
}

/* 驱动一次事件循环。0 ok / <0 连接已不可恢复（调用方须置 dead）。 */
static int
smb_ra_pump(transfer_src_t *s, int timeout_ms) {
  struct pollfd pfd;

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = (int)smb2_get_fd(s->smb2);
  pfd.events = (short)smb2_which_events(s->smb2);
  if(poll(&pfd, 1, timeout_ms) < 0) {
    if(errno == EINTR) return 0;
    return -1;
  }
  if(!pfd.revents) return 0;
  if(smb2_service(s->smb2, pfd.revents) < 0) return -1;
  return 0;
}

static void
smb_ra_reset(transfer_src_t *s) {
  smb_ra_t *ra = &s->ra;
  unsigned long long t0;
  int       i;

  ra->gen++;
  /* 只等、不 free：在途回调还会写这些缓冲，缓冲归 smb_close 释放。
     ⚠️ 同样是**单调时钟**，不是次数：这里原来数 spins，和主循环犯的是同一个
        错（见 SMB_STALL_MS）。作废旧一代请求最多等 1 秒就够了 —— 它们只是
        要等一个回包让 libsmb2 把 PDU 从队列里摘掉，等不到也不影响正确性
        （gen 已经变了，迟到的回调会被丢弃）。 */
  if(s->smb2) {
    t0 = mono_ms();
    while(ra->inflight > 0 && !ra->dead && mono_ms() - t0 < 1000ull)
      if(smb_ra_pump(s, 100) < 0) { ra->dead = 1; break; }
  }
  for(i = 0; i < SMB_RA_SLOTS; i++) {
    ra->state[i] = 0;
    ra->used[i] = 0;
    ra->got[i] = 0;
    ra->err[i] = 0;
  }
  ra->head = 0;
  ra->active = 0;
  ra->eof = 0;
}

/* 丢弃整条 SMB 连接，把所有在途记账清零。
 *
 * ★ 为什么 stall 之后必须断链重连，而不是原地继续读：
 *     stall 的现场是「12 个请求发出去了，一个回包都没有」。此时连接上还有
 *     12 个 PDU 挂在 waitqueue，服务端随时可能把它们陆续送回来 —— 但那些字节
 *     对应的偏移已经被我们作废了，而且它们还会还回 credit，让本地记账和实际
 *     链路状态对不上。唯一干净的做法就是整条连接丢掉重来：新连接自带全新的
 *     credit 和 message id 序列，没有任何历史包袱。
 * ★ 为什么**不能**走 smb_ra_reset()：它会 pump 事件循环来等在途请求回来，
 *    而这里我们要销毁的就是那个循环赖以存在的 context ⇒ 空指针解引用。
 * ★ 顺序是硬约束：必须**先** gen++ 再销毁。destroy_context 会给队列里所有
 *    PDU 补发 SMB2_STATUS_CANCELLED 回调，那些回调会写 ra[]；gen 已经变了
 *    它们就会在 sgen 检查处直接返回，不会污染我们随后清零的状态。
 * 🪤 不要在销毁后调用 smb2_close(fh)：那是一次同步往返，对面正在不回包，
 *    会白等一个完整超时周期。fd 一关，TCP 断开，服务端自己会回收句柄。 */
static void
smb_hard_reset(transfer_src_t *s) {
  smb_ra_t *ra = &s->ra;
  int       i;

  ra->gen++;
  for(i = 0; i < SMB_RA_SLOTS; i++) {
    ra->state[i] = 0;
    ra->busy[i]  = 0;
    ra->used[i]  = 0;
    ra->got[i]   = 0;
    ra->err[i]   = 0;
  }
  ra->inflight = 0;
  ra->head     = 0;
  ra->active   = 0;
  ra->eof      = 0;
  ra->dead     = 0;
  if(s->smb2) smb2_destroy_context(s->smb2);
  s->smb2    = NULL;
  s->smb2_fh = NULL;
}

/* 把窗口对准 offset：算出块大小、铺好回调用的 (ra,idx) 配对。
 *
 * ★ 块大小的三道闸，口径全部来自上游 owendswang/ps5-web-file-manager 的
 *   vfs.c:smb_io()。我们原来只有第一道，缺后两道 —— 真机 v1.0.18 的日志
 *   「在途 1 个、块 1048576 字节、本块已收 0 字节」就是缺第二道的现场：
 *   协商到 SMB 2.0.2 的对端按规范只能吃 64KB 的 READ，1MB 的 READ 它
 *   既不回数据也不报错，于是我们等满 60 秒零回包。
 *     ① 服务端协商的 MaxReadSize（libsmb2: smb2_get_max_read_size）
 *     ② dialect <= 0x0202 ⇒ 硬上限 64KB（SMB 2.0.2 规范上限）
 *     ③ 小请求不开 1MB 大块：调用方一次只要 <= 1MB 时降到 256KB，
 *        免得尾部那一小截非要拆成一个巨大的 READ 才拿得到
 */
static void
smb_ra_activate(transfer_src_t *s, uint64_t offset, size_t req) {
  smb_ra_t *ra = &s->ra;
  uint32_t  mx, block;
  int       i;

  smb_ra_reset(s);
  /* ★ 从全局记忆恢复：这台主机之前 stall 过 ⇒ 本次连接只用 1 槽起步。
       smb_ra_reset() 刻意不动 degraded，所以新值在这里定。 */
  ra->degraded = smb_ra_host_degraded(s->nas.server);
  mx = smb2_get_max_read_size(s->smb2);
  block = mx ? (mx < SMB_RA_BLOCK_MAX ? mx : SMB_RA_BLOCK_MAX)
             : SMB_RA_BLOCK_DEF;
  if(!block) block = SMB_RA_BLOCK_DEF;
  /* ② SMB 2.0.2：不管服务端报多大的 MaxReadSize，请求都不能超过 64KB。 */
  if(smb2_get_dialect(s->smb2) <= SMB2_VERSION_0202 && block > 65536u)
    block = 65536u;
  /* ③ 只在开流水线（窗口 > 1）时生效：单请求窗口本来就是一个 READ，
        再降块只会白白多花往返。 */
  if(!ra->degraded && req && req <= SMB_RA_BLOCK_MAX && block > 256u * 1024u)
    block = 256u * 1024u;
  ra->block = block;
  for(i = 0; i < SMB_RA_SLOTS; i++) {
    if(!ra->op[i]) {
      ra->op[i] = (smb_ra_op_t *)malloc(sizeof(*ra->op[i]));
      if(!ra->op[i]) { ra->block = 0; return; }
    }
    ra->op[i]->ra = ra;
    ra->op[i]->idx = i;
  }
  ra->deliver = offset;
  ra->next_off = offset;
  ra->active = 1;
}

/* 尽量把窗口填满：空闲槽一律补一个 READ。
 *
 * ★ 长度必须**我们自己**定，不能交给 smb2_pread_async()：它发现 credit 不够会
 *   把 count 静默砍到 credits*65536（砍到 0 就发出一个 0 长度的 READ），而我
 *   们按 want 推进 next_off —— 被砍掉的那一截就再也没人读，文件被静默截断。
 *   （真机 v1.0.13 的「12 个在途 / 收到 0 字节 / 60s 无回包」就是这条路。）
 * ★ 一个 credit 都没有时**不提交**：libsmb2 在 credits==0 时会发出长度 0 的
 *   READ，而 credit_charge=(0-1)/65536+1 溢出成 0，连"付不起就先不发"这道闸
 *   都拦不住它；服务端老实回 0 字节，读端就会误判成 EOF。
 * ★ credits 是**本地**记账：真扣减发生在把 PDU 交给 socket 的那一刻，所以我
 *   们按"提交即占用"往下减，下一次调用重新读真值，天然保守、不会超发。 */
static int
smb_ra_submit(transfer_src_t *s, smb_ra_t *ra) {
  int credits = smb2_get_credits(s->smb2);
  /* ★ 动态窗口上限：正常时对齐上游的 8 槽；stall 过就退化成 1 槽。
   *   降级**保持到本次文件传输结束**（见 ra->degraded 的注释）。 */
  int limit = ra->degraded ? 1 : SMB_RA_SLOTS;
  while(!ra->eof && !ra->dead && ra->inflight < limit) {
    int i, charge;
    uint32_t want;
    for(i = 0; i < limit; i++)
      if(!ra->busy[i] && ra->state[i] == 0) break;
    if(i == limit) break;
    /* block 现在会随 dialect / 请求大小变化（见 smb_ra_activate），缓冲可能
       不够大 —— 按需扩，不缩（缩了也是浪费一次 malloc）。 */
    if(!ra->buf[i] || ra->bufsz[i] < ra->block) {
      unsigned char *nb = (unsigned char *)realloc(ra->buf[i], ra->block);
      if(!nb) break;              /* OOM：就用已经在途的那些，不致命 */
      ra->buf[i] = nb;
      ra->bufsz[i] = ra->block;
    }
    want = ra->block;
    /* 别越过文件尾去要字节：越界的 READ 只会白花一个来回。 */
    if(ra->size_known) {
      uint64_t left = ra->next_off < ra->size ? ra->size - ra->next_off : 0;
      if(!left) { ra->eof = 1; break; }
      if(left < (uint64_t)want) want = (uint32_t)left;
    }
    charge = (int)((want - 1u) / 65536u + 1u);
    if(charge > credits) {
      if(credits <= 0) break;                     /* 一个都付不起，等回包还 credit */
      want = (uint32_t)credits * 65536u;
      if(want > ra->block) want = ra->block;
      charge = credits;
    }
    credits -= charge;
    ra->want[i] = want;
    ra->off[i] = ra->next_off;
    ra->got[i] = 0;
    ra->used[i] = 0;
    ra->err[i] = 0;
    ra->sgen[i] = ra->gen;
    ra->state[i] = 1;
    ra->busy[i] = 1;
    if(smb2_pread_async(s->smb2, s->smb2_fh, ra->buf[i], ra->want[i],
                        ra->off[i], smb_ra_cb, ra->op[i]) < 0) {
      ra->busy[i] = 0;
      ra->state[i] = 0;
      ra->dead = 1;
      set_err(s, smb2_get_error(s->smb2));
      return -1;
    }
    ra->inflight++;
    ra->next_off += want;
  }
  return 0;
}

/* 一次真正的读取尝试。
 * ★ *stalled 是这个函数的**关键返回值**：置 1 表示「这次是对端断流 / 请求超时」，
 *   交给外层降级后重试；置 0 表示真错（权限、文件没了、共享没了）。少了这个
 *   区分，外层要么一律重试（真错上白等几轮），要么一律不重试（本可以救回来的
 *   突发打爆也直接判死）—— 两边都是用户看到的「点 NAS 里的 pkg 一直失败」。 */
static mango_err_t
smb_read_once(transfer_src_t *s, uint64_t offset, void *buf, size_t len,
              size_t *got, int *stalled) {
  smb_ra_t    *ra = &s->ra;
  char         target[MANGO_PATH_MAX];
  mango_err_t  rc;
  int          h;
  size_t       avail, n;

  *stalled = 0;
  rc = smb_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  if(!len) { *got = 0; return MANGO_OK; }
  if(!s->smb2_fh) {
    smb_path(s, NULL, target, sizeof(target));
    s->smb2_fh = smb2_open(s->smb2, target, O_RDONLY);
    if(!s->smb2_fh) return MANGO_ERR_NOTFOUND;
    /* ★ 文件大小是判「真 EOF」的唯一依据。
       🪤 不能再用「读短了 = 到文件尾」：SMB2 的 credit 按 64KB 一个算，
       libsmb2 会在 credit 不够时把 READ 静默砍短（砍到 0 甚至发出 0 长度
       READ），那种短读跟文件尾一点关系都没有 —— 照旧判断就是把一个好好
       的文件复制成截断文件。多花一个 fstat 来回，换"读满为止"的确定性。 */
    {
      struct smb2_stat_64 st;
      memset(&st, 0, sizeof(st));
      if(smb2_fstat(s->smb2, s->smb2_fh, &st) == 0) {
        ra->size = st.smb2_size;
        ra->size_known = 1;
      } else {
        ra->size = 0;
        ra->size_known = 0;   /* 拿不到就退回"读短即 EOF"的老口径 */
      }
    }
  }
  /* 跳位 / 首次 / 连接已废 ⇒ 重新对准窗口（旧在途请求会被作废）。 */
  if(!ra->active || ra->deliver != offset || ra->dead)
    smb_ra_activate(s, offset, len);
  if(ra->dead || !ra->block) {
    set_err(s, smb2_get_error(s->smb2));
    return MANGO_ERR_GENERIC;
  }
  {
    /* 「毫无进展」的计时基准：只要回调还在一个个回来（progress 在变），就说明
       链路在动，这一次读不算超时 —— 慢不是错，断才是。 */
    unsigned last_prog = ra->progress;
    /* ★ 计时基准是**单调时钟**，不是循环次数。旧代码数的是 spins，文案却写
       「连续 60 秒」—— 见 SMB_STALL_MS 上方那段：socket 一热，60 次循环几
       毫秒就过去了，于是一次正常的读被当场判成「对端断流」。 */
    unsigned long long t_last = mono_ms();
    for(;;) {
      if(smb_ra_submit(s, ra) < 0) return MANGO_ERR_GENERIC;
      h = ra->head;
      if(ra->state[h] == 2) break;                 /* 有数据可交 */
      if(ra->state[h] == 3) {
        /* 🪤 异步回调把真实错误码存在 err[] 里。smb2_get_error() 是"最后一次
           错误"，异步路径上经常是上一次的残留甚至 NULL —— 原来直接拿它兜底，
           用户看到的就是「未知原因」或者一条完全不相干的旧错误。 */
        const char *le = smb2_get_error(s->smb2);
        char msg[256];
        if(ra->err[h])
          snprintf(msg, sizeof(msg), "SMB 读取失败：%s（errno=%d）",
                   strerror(ra->err[h]), ra->err[h]);
        else
          snprintf(msg, sizeof(msg), "SMB 读取失败：%s",
                   (le && le[0]) ? le : "未知原因");
        mango_log("smb_read: %s", msg);
        /* ★ 只有「超时」才值得重试：对面只是这一会儿没回包，重连之后大概率能
           走完。EACCES / ENOENT / STATUS_BAD_NETWORK_NAME 这类是真错，重试
           只是让用户多等一轮再看到同一句报错。 */
        *stalled = (ra->err[h] == ETIMEDOUT) ? 1 : 0;
        ra->dead = 1;
        set_err(s, msg);
        return MANGO_ERR_GENERIC;
      }
      if(ra->inflight == 0 && ra->eof) { *got = 0; return MANGO_OK; }  /* 干净 EOF */
      if(ra->progress != last_prog) { last_prog = ra->progress; t_last = mono_ms(); }
      if(mono_ms() - t_last >= (unsigned long long)SMB_STALL_MS) {
        char msg[256];
        unsigned long long waited = mono_ms() - t_last;
        /* 🪤 上一版这里写的是「几乎总是 credit 不够、请求压在 outqueue」。
         *   2026-10-16 真机日志**证伪**：stall 那次 credit=671（12×2MB 只要 384），
         *   闸门（credits<=0 就不提交）根本没触发 ⇒ 请求是真的发出去了，
         *   卡住的是**对端 NAS 不回包**。
         *   仍然带上这个数字：它是唯一能当场区分「我们没发出去」与
         *   「对端不回」两个排查方向的证据。 */
        /* ★ 报的是**实测**毫秒数，不再拿常量冒充时长；顺带带上文件和偏移 ——
           真机上「哪一次读卡住了」此前完全看不出来（日志里只有块大小和
           credit，没有文件名），这是排查时最缺的一条。 */
        snprintf(msg, sizeof(msg),
                 "SMB 读取超时：连续 %llu 秒一个回包都没有"
                 "（在途 %d 个，块 %u 字节，本块已收 %u 字节，可用 credit %d，"
                 "文件 %s，偏移 %llu）",
                 waited / 1000ull, ra->inflight, ra->block,
                 (unsigned)ra->got[h], smb2_get_credits(s->smb2),
                 s->path[0] ? s->path : "(未知)",
                 (unsigned long long)ra->off[h]);
        mango_log("smb_read: %s", msg);
        /* 连续 60 秒一个字节都没收到 = 对端断流，交给外层：降级成单请求窗口
           + 断链重连，然后再试一次。降级的具体位置在 smb_read() 里，理由见
           那里 —— 关键是**降级要跨连接**，而连接就是在这里断的。 */
        *stalled = 1;
        ra->dead = 1;
        set_err(s, msg);
        return MANGO_ERR_GENERIC;
      }
      {
        /* 睡到「下一个该判定的时刻」为止，不多睡：固定睡 1 秒会让 60 秒的
           兜底在边界上拖成 61 秒，而睡 0 秒又会把 CPU 烧成忙等（旧代码在
           socket 一热时正是这样空转 60 圈）。下限 50ms 只是防止极端情况下的
           忙等，不影响判定时刻。 */
        unsigned long long left = (unsigned long long)SMB_STALL_MS -
                                  (mono_ms() - t_last);
        int slice = left > (unsigned long long)SMB_RA_PUMP_MS
                  ? SMB_RA_PUMP_MS : (int)left;
        unsigned long long t_before = mono_ms();
        if(slice < 50) slice = 50;
        if(smb_ra_pump(s, slice) < 0) {
          const char *le = smb2_get_error(s->smb2);
          ra->dead = 1;
          set_err(s, (le && le[0]) ? le : "SMB 连接已断开");
          return MANGO_ERR_GENERIC;
        }
        /* ★ CPU 下限：poll 明明报了就绪、smb2_service 却还没凑出完整 PDU 时
           （真机实测 socket 就是这么"热"的），这一圈会在微秒级返回。旧代码
           把这个当成了「时间在走」。现在时间由 mono_ms 决定，循环本身仍要
           限速，否则判死的 60 秒会变成 60 秒满核空转。 */
        if(mono_ms() - t_before < 5ull) {
          struct timespec ts;
          ts.tv_sec = 0;
          ts.tv_nsec = 5 * 1000 * 1000;
          nanosleep(&ts, NULL);
        }
      }
    }
  }
  avail = (size_t)ra->got[h] - ra->used[h];
  n = len < avail ? len : avail;
  if(n) memcpy(buf, ra->buf[h] + ra->used[h], n);
  ra->used[h] += n;
  ra->deliver += n;
  if(ra->used[h] >= ra->got[h]) {
    uint64_t end = ra->off[h] + ra->got[h];
    /* ★ EOF 只认偏移，不认"读短了"：size 已知时以越过文件尾为准；拿不到
       size（fstat 失败）才退回"读短即 EOF"的老口径。
       🪤 反过来还有一种坏情况：还没到文件尾却一个字节都没收到。那不是 EOF，
       是服务端在糊弄（或 credit 被砍到 0），如实报错，别把 0 字节当成功
       交上去 —— 调用方会把 0 当成"读完了"，复制出半个文件还报成功。 */
    if(ra->size_known) {
      if(end >= ra->size) ra->eof = 1;
      else if(!ra->got[h]) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "SMB 读取中断：偏移 %llu 处收到 0 字节（文件 %llu 字节，"
                 "本块要 %u 字节）—— 不是文件尾，连接已不可信",
                 (unsigned long long)ra->off[h], (unsigned long long)ra->size,
                 ra->want[h]);
        mango_log("smb_read: %s", msg);
        ra->dead = 1;
        set_err(s, msg);
        return MANGO_ERR_GENERIC;
      } else if(ra->got[h] < ra->want[h]) {
        /* ★ 短读的缺口必须补，否则复制出来的是**静默截断**的文件。
           🪤 提交时 next_off 是按 want（申请量）往前推的，而对端可能只给了
              got 字节（credit 砍短 / 服务端限流 / 慢链路）。那一截
              [end, off+want) 从此再没人去要 —— 上层看到的是"复制成功"，
              文件却是坏的，且没有任何报错。
           对策：作废整个窗口、把光标退回实际收到的 end 重开。gen++ 会让
           仍在途的高位槽结果被丢弃，所以不会把后面的字节错拼到这里。
           （上游 vfs.c:smb_io_complete() 是原地补发剩余区间；我们的槽模型
           做不到原地补，但"退回重开"的效果等价，且只在短读时才付代价。） */
        mango_log("smb_read: 短读 %u/%u 字节 @%llu，退回 %llu 重开窗口补缺口",
                  (unsigned)ra->got[h], (unsigned)ra->want[h],
                  (unsigned long long)ra->off[h], (unsigned long long)end);
        ra->active = 0;
      }
    } else {
      /* 短读 = 到文件尾，别再往文件尾之后发 READ。 */
      if(ra->got[h] < ra->want[h]) ra->eof = 1;
    }
    ra->state[h] = 0;
    ra->head = (h + 1) % SMB_RA_SLOTS;
  }
  *got = n;
  return MANGO_OK;
}

/* 对外的读取入口：尝试 → stall 就降级断链 → 再试，预算用尽才报错。
 *
 * ★🪤🪤 这里为什么要包一层重试：**降级必须发生在断链之后，而不是之前**。
 *   一次 stall 之后，连接上还挂着那些再也不会（或很晚才会）回来的 PDU，credit
 *   账也乱了。如果只是把 ra->degraded 置 1 然后继续用同一条连接，等于对着一条
 *   已经半死的连接降低语速 —— 该不回包还是不回包。必须把连接丢了重来，降级
 *   才有意义。而重连这件事只能在 smb_read_once() 外面做（它内部第一件事就是
 *   smb_ensure_connected，由它自己重建最干净）。
 *
 * ★ 预算用完仍然失败时，错误串保留最后一次的现场（含在途数、块大小、credit），
 *   那是真机上唯一能区分「我们没发出去」和「对端不回」的证据。 */
static mango_err_t
smb_read(transfer_src_t *s, uint64_t offset, void *buf, size_t len, size_t *got) {
  int          attempt, stalled = 0;
  mango_err_t  rc;

  for(attempt = 0; attempt <= SMB_READ_STALL_RETRY; attempt++) {
    rc = smb_read_once(s, offset, buf, len, got, &stalled);
    if(rc == MANGO_OK || !stalled) return rc;
    if(attempt == SMB_READ_STALL_RETRY) return rc;
    /* ★ 降级①：本次文件剩下的部分每次只发 1 个 READ。
         家用 NAS 被一次性 24MB 突发打死，是我们这边唯一能改的自变量 ——
         对端慢我们管不了，但一次放出去多少是我们决定的。
       ★ 降级②：把这台主机记进全局表。只写 ra->degraded 不够：上层失败后会
         新建 transfer_src_t 重来，新 ra 是清零的，第二回合又拿 8MB 去轰
         同一台刚 stall 过的 NAS，表现就是「重试好几次都失败」。 */
    s->ra.degraded = 1;
    smb_ra_host_mark(s->nas.server);
    mango_log("smb_read: %s 断流，降级为单请求窗口并重连后重试（第 %d 次）",
              s->nas.server, attempt + 1);
    smb_hard_reset(s);
  }
  return MANGO_OK;          /* 不可达：循环内必然 return */
}

static void
smb_close(transfer_src_t *s) {
  int i;

  smb_ra_reset(s);
  if(s->smb2_fh) smb2_close(s->smb2, s->smb2_fh);
  if(s->smb2) smb2_destroy_context(s->smb2);
  /* 缓冲必须在 context 销毁之后释放：迟到的回调会写这些内存。
     op[] 同理（回调拿它当 private_data）。 */
  for(i = 0; i < SMB_RA_SLOTS; i++) { free(s->ra.buf[i]); free(s->ra.op[i]); }
  memset(&s->ra, 0, sizeof(s->ra));
}
#endif /* MANGO_HAVE_LIBSMB2 */

#if defined(MANGO_HAVE_LIBNFS)
/* nfsio_* prefix: nfs_read/nfs_close are already public libnfs API names. */
static mango_err_t
nfsio_ensure_connected(transfer_src_t *s) {
  char export_name[MANGO_PATH_MAX];

  if(s->nfs) return MANGO_OK;
  /* NFS export names are absolute paths; the URL form host:/export/path
   * parses to a share without the leading slash, so re-add it for mountd. */
  if(s->nas.share[0] == '/') {
    strncpy(export_name, s->nas.share, sizeof(export_name) - 1);
    export_name[sizeof(export_name) - 1] = 0;
  } else {
    snprintf(export_name, sizeof(export_name), "/%s", s->nas.share);
  }

  /* libnfs defaults to NFSv3. Many modern NAS export ONLY v4, so try v3 first
   * and fall back to NFSv4.0. A fresh context per attempt: a failed mount can
   * leave the context unusable. NFS_V4 == 4 is defined in libnfs's internal
   * headers (nfs4/libnfs-raw-nfs4.h), NOT the public libnfs.h, so use 4. */
  for(int attempt = 0; attempt < 2; attempt++) {
    s->nfs = nfs_init_context();
    if(!s->nfs) return MANGO_ERR_NOMEM;
    /* A custom port MUST be set before mount: nfs_mount connects immediately,
     * so setting it afterwards (the old order) silently had no effect. */
    if(s->nas.port > 0) nfs_set_nfsport(s->nfs, s->nas.port);
    if(attempt == 1) nfs_set_version(s->nfs, 4 /* NFSv4.0 */);
    if(nfs_mount(s->nfs, s->nas.server, export_name) == 0) return MANGO_OK;
    set_err(s, nfs_get_error(s->nfs));   /* capture before destroying the ctx */
    nfs_destroy_context(s->nfs);
    s->nfs = NULL;
  }
  return MANGO_ERR_NOTFOUND;
}

static mango_err_t
nfsio_list(transfer_src_t *s, const char *dir,
         transfer_entry_t **out, int *count) {
  struct nfsdir    *d = NULL;
  struct nfsdirent *ent;
  transfer_entry_t *list = NULL;
  int              n = 0, cap = 0;
  mango_err_t      rc;
  const char       *target = dir ? dir : s->nas.path;

  rc = nfsio_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  /* Same host/share/rest handling as smb_path: the frontend passes the full
   * URL, so strip the server+share before nfs_opendir (share-relative). */
  if(dir) {
    transfer_nas_t tmp;
    if(transfer_url_parse(dir, &tmp))
      target = tmp.path[0] ? tmp.path : "/";
  }
  if(nfs_opendir(s->nfs, target, &d) != 0) return MANGO_ERR_NOTFOUND;
  while((ent = nfs_readdir(s->nfs, d))) {
    int is_dir;
    if(!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) continue;
    /* libnfs fills type when the server answers READDIRPLUS; fall back to the
     * mode bits for old servers (NF3DIR == 2 in the NFSv3 protocol). */
    is_dir = ((ent->type == 2) || S_ISDIR(ent->mode)) ? 1 : 0;
    rc = list_push(&list, &n, &cap, ent->name, (uint64_t)ent->size,
                   (uint64_t)ent->mtime.tv_sec, (uint32_t)(ent->mode & 07777), is_dir);
    if(rc != MANGO_OK) { free(list); nfs_closedir(s->nfs, d); return rc; }
  }
  nfs_closedir(s->nfs, d);
  *out = list;
  *count = n;
  return MANGO_OK;
}

static mango_err_t
nfsio_size(transfer_src_t *s, uint64_t *size) {
  /* libnfs itself splits nfs_stat's struct on _WIN32 (struct __stat64); mirror
   * that here so the glue compiles on a Win32 host too. The PS5 payload uses
   * the POSIX branch. */
#if defined(_WIN32)
  struct __stat64 st;
#else
  struct stat st;
#endif
  mango_err_t rc;

  rc = nfsio_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  if(nfs_stat(s->nfs, s->nas.path, &st) != 0) return MANGO_ERR_NOTFOUND;
  *size = (uint64_t)st.st_size;
  return MANGO_OK;
}

/* 与 smb_remove 同形：libnfs 的 nfs_unlink / nfs_rmdir 各自对应文件与目录。
 * 路径要剥掉 主机/共享 前缀（nfs 的调用都是 export 相对的），与 nfsio_list
 * 同一口径。 */
static mango_err_t
nfsio_remove(transfer_src_t *s, const char *path, int is_dir) {
  const char  *target = path ? path : s->nas.path;
  mango_err_t  rc;

  rc = nfsio_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  if(path) {
    transfer_nas_t tmp;
    if(transfer_url_parse(path, &tmp))
      target = tmp.path[0] ? tmp.path : "/";
  }
  if(is_dir ? nfs_rmdir(s->nfs, target) : nfs_unlink(s->nfs, target)) {
    set_err(s, nfs_get_error(s->nfs));
    return MANGO_ERR_GENERIC;
  }
  return MANGO_OK;
}

static mango_err_t
nfsio_read(transfer_src_t *s, uint64_t offset, void *buf, size_t len, size_t *got) {
  int         n;
  mango_err_t rc;

  rc = nfsio_ensure_connected(s);
  if(rc != MANGO_OK) return rc;
  if(!len) { *got = 0; return MANGO_OK; }
  if(!s->nfs_fh) {
    if(nfs_open(s->nfs, s->nas.path, O_RDONLY, &s->nfs_fh) != 0)
      return MANGO_ERR_NOTFOUND;
  }
  n = nfs_pread(s->nfs, s->nfs_fh, buf, len, offset);
  if(n < 0) return MANGO_ERR_GENERIC;
  *got = (size_t)n;   /* 0 is clean EOF, not an error */
  return MANGO_OK;
}

static void
nfsio_close(transfer_src_t *s) {
  if(s->nfs_fh) nfs_close(s->nfs, s->nfs_fh);
  if(s->nfs) nfs_destroy_context(s->nfs);
}
#endif /* MANGO_HAVE_LIBNFS */

mango_err_t
transfer_list(transfer_src_t *s, const char *dir,
              transfer_entry_t **out, int *count) {
  DIR            *d;
  struct dirent  *ent;
  transfer_entry_t *list = NULL;
  int             n = 0, cap = 0;
  const char     *target;
  char            base[MANGO_PATH_MAX];

  if(!s || !out || !count) return MANGO_ERR_GENERIC;
  target = dir ? dir : s->path;
  if(s->scheme == SCHEME_LOCAL) {
    mango_err_t rc;

    strncpy(base, target, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    d = opendir(base);
    if(!d) return MANGO_ERR_NOTFOUND;
    while((ent = readdir(d))) {
      char full[MANGO_PATH_MAX + 300];
      struct stat st;

      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      snprintf(full, sizeof(full), "%s/%s", base, ent->d_name);
      if(stat(full, &st)) continue;
      rc = list_push(&list, &n, &cap, ent->d_name,
                     (uint64_t)st.st_size, (uint64_t)st.st_mtime,
                     (uint32_t)(st.st_mode & 07777), S_ISDIR(st.st_mode));
      if(rc != MANGO_OK) { free(list); closedir(d); return rc; }
    }
    closedir(d);
    *out = list;
    *count = n;
    return MANGO_OK;
  }
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_list(s, dir, out, count);
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_list(s, dir, out, count);
#endif
  return MANGO_ERR_UNSUPPORTED;
}

mango_err_t
transfer_size(transfer_src_t *s, uint64_t *size) {
  struct stat st;

  if(!s || !size) return MANGO_ERR_GENERIC;
  *size = 0;
  if(s->scheme == SCHEME_LOCAL) {
    if(stat(s->path, &st)) return MANGO_ERR_NOTFOUND;
    *size = (uint64_t)st.st_size;
    return MANGO_OK;
  }
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_size(s, size);
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_size(s, size);
#endif
  return MANGO_ERR_UNSUPPORTED;
}

/* 删除远端的一个条目。is_dir 由调用方从列目录结果里带过来（我们**知道**它是
 * 目录还是文件 —— 那是刚列出来的），不在这里猜：远端 stat 又要一次往返，
 * 而删除任务本身已经为每棵子树列过一次目录了。 */
mango_err_t
transfer_remove(transfer_src_t *s, const char *path, int is_dir) {
  if(!s || !path) return MANGO_ERR_GENERIC;
  if(s->scheme == SCHEME_LOCAL)
    return (is_dir ? rmdir(path) : unlink(path)) ? MANGO_ERR_GENERIC
                                                 : MANGO_OK;
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_remove(s, path, is_dir);
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_remove(s, path, is_dir);
#endif
  return MANGO_ERR_UNSUPPORTED;
}

/* Recommended per-call read size; see transfer.h for why this exists.
 * 取值口径：默认 1MB（libsmb2 自身的协商下限，Samba 老版本常报这个数）；
 * 服务端报得更大就跟着涨到协商上限（封顶 4MB）—— 报 8MB 还只用 1MB 等于白扔
 * 带宽；报得比 1MB 小就尊重服务端（超出协商值的 READ 会被拒）。 */
#define TRANSFER_CHUNK_FLOOR (1u * 1024u * 1024u)
#define TRANSFER_CHUNK_CAP   (4u * 1024u * 1024u)

size_t
transfer_read_chunk(transfer_src_t *s) {
  size_t chunk = TRANSFER_CHUNK_FLOOR;

  if(!s) return TRANSFER_CHUNK_FLOOR;
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB && s->smb2) {
    uint32_t mx = smb2_get_max_read_size(s->smb2);
    if(mx >= 4096) chunk = (size_t)mx;
  }
#endif
  /* NFS: libnfs exposes no negotiated read ceiling; the floor (1 MB) is also
   * the NFSv3 protocol's own per-READ maximum, so it is the right value. */
  if(chunk > TRANSFER_CHUNK_CAP) chunk = TRANSFER_CHUNK_CAP;
  return chunk;
}

mango_err_t
transfer_read(transfer_src_t *s, uint64_t offset,
              void *buf, size_t len, size_t *got) {
  ssize_t n;

  if(!s || !buf || !got) return MANGO_ERR_GENERIC;
  *got = 0;
  if(s->scheme == SCHEME_LOCAL) {
    if(!len) return MANGO_OK;
    if(!s->opened) {
      s->fd = open(s->path, O_RDONLY
#ifdef O_BINARY
                            | O_BINARY
#endif
                   );
      s->opened = 1;
    }
    if(s->fd < 0) return MANGO_ERR_NOTFOUND;
    /* lseek+read rather than pread: pread needs _XOPEN_SOURCE on MinGW and the
     * payload is single-threaded, so there is no concurrent-offset concern. */
    if(lseek(s->fd, (off_t)offset, SEEK_SET) < 0) return MANGO_ERR_GENERIC;
    n = read(s->fd, buf, len);
    if(n < 0) {
      if(errno == EINTR) return MANGO_OK;
      return MANGO_ERR_GENERIC;
    }
    *got = (size_t)n;
    return MANGO_OK; /* n == 0 is clean EOF, not an error */
  }
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) return smb_read(s, offset, buf, len, got);
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) return nfsio_read(s, offset, buf, len, got);
#endif
  return MANGO_ERR_UNSUPPORTED;
}

void
transfer_close(transfer_src_t *s) {
  if(!s) return;
  if(s->scheme == SCHEME_LOCAL) {
    if(s->fd >= 0) close(s->fd);
  }
#if defined(MANGO_HAVE_LIBSMB2)
  if(s->scheme == SCHEME_SMB) smb_close(s);
#endif
#if defined(MANGO_HAVE_LIBNFS)
  if(s->scheme == SCHEME_NFS) nfsio_close(s);
#endif
  free(s);
}
