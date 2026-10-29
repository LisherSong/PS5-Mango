#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __SCE__
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <ucontext.h>   /* crash_handler 读 mc_rip/mc_rsp（FreeBSD 字段名） */
#endif

#include "app_installer.h"
#include "filemgr.h"
#include "mango_log.h"
#include "mango_port.h"
#include "notify.h"
#include "savemgr.h"
#include "websrv.h"
#ifdef CLOUD_NET
#endif

/* ⚠️ Deliberately version-free, while the shipped file name is versioned
 * (PS5-Mango-<tag>.elf, see the Makefile). The payload manager lists payloads by
 * file name, so the version lives there; this string is the *identity* used to
 * reap an older instance, so it must stay stable across version bumps — a
 * versioned name here would fail to match the previous build and leave two
 * processes competing for the same port. Also note it is the basename (".elf"
 * included) because that is what the PS5 loader names the process/thread. */
#define PROCESS_NAME "PS5-Mango.elf"
#define DEFAULT_PORT 2026

/* ---- crash ledger ---------------------------------------------------------
 * A crashing payload on the console just vanishes — there is no core file to
 * read and the user's only symptom is "the page died / the port stopped
 * answering". This handler is the sole forensic channel: it appends the fatal
 * signal plus a snapshot of the in-flight requests to a log, then RESTORES the
 * default disposition and re-raises, so the process still dies honestly (the
 * crash is never swallowed). Best-effort by design: only open/write/close and
 * a try-locked snapshot run here; anything that needs the heap is already
 * gone. The buffer is static so the handler survives a nearly-exhausted
 * stack (a stack-overflow SIGSEGV leaves no room for a stack buffer). */
/* 台账与崩溃日志同一个文件（mango_log.h 里定义，并解释了为什么共用）。 */
#define CRASH_LOG_PATH MANGO_LOG_PATH

static volatile sig_atomic_t g_in_crash;
static char g_crash_buf[3072];

static const char *
crash_signal_name(int sig) {
  switch(sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS:  return "SIGBUS";
  case SIGABRT: return "SIGABRT";
  case SIGILL:  return "SIGILL";
  case SIGFPE:  return "SIGFPE";
  default:      return "SIG?";
  }
}

/* glibc marks write() with __attribute__((warn_unused_result)) and -Werror
 * rejects the (void) cast, so the writes go through this loop, which also
 * finishes short writes. */
static void
crash_write(int fd, const char *s, size_t n) {
  while(n > 0) {
    ssize_t w = write(fd, s, n);
    if(w <= 0) {
      return;
    }
    s += w;
    n -= (size_t)w;
  }
}

#ifdef __SCE__
/* ---- startup ledger -------------------------------------------------------
 * 单实例治理每一环（线程名设置 / 旧进程匹配 / kill / 端口占用）的结果都追加到
 * 同一个日志文件 —— 真机跑一次就能看出断在哪一环，不用猜。实现搬进了
 * src/mango_log.c，因为关闭路径（filemgr.c / websrv.c）也要往同一个台账里写，
 * 那是分辨「关不掉」还是「关了又被外部拉起来」的唯一依据。 */
#endif

static void
crash_handler(int sig, siginfo_t *si, void *ucv) {
#ifdef __SCE__
  ucontext_t *uc = (ucontext_t *)ucv;
#else
  (void)ucv;
#endif
  if(!g_in_crash) {
    int fd = open(CRASH_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);

    g_in_crash = 1;
    if(fd >= 0) {
      /* si_addr = 出错的那条地址。越界读写时它躺在被踩的内存上，栈溢出时它
       * 落在栈边界附近 —— 这两件事的排查方向正好相反（前者查指针和长度，
       * 后者查栈上缓冲有多大），而光看 "SIGSEGV" 三个字分不出来。
       * si_code 同理：SEGV_MAPERR(1) 多半是解引用坏指针，SEGV_ACCERR(2) 多半
       * 是踩到了不该写的页。2026-10-11 就是卡在这里 —— 只知道崩了，不知道崩
       * 在哪一类，只能靠猜。 */
      int w = snprintf(g_crash_buf, sizeof(g_crash_buf),
                       "\n=== %s %s pid=%d time=%ld addr=%p code=%d ===\n",
                       PROCESS_NAME, crash_signal_name(sig), (int)getpid(),
                       (long)time(NULL),
                       si ? si->si_addr : (void *)0, si ? si->si_code : 0);
      if(w > 0) {
        crash_write(fd, g_crash_buf, (size_t)w);
      }
#ifdef __SCE__
      /* PC / SP / BP —— 定位「崩在哪个函数」的唯一线索。
       * 为什么要锚点：产物是 PIE 且不带符号表，单看绝对地址没有意义。
       * 把本函数的运行时地址一起打出来，`pc - anchor` 就是代码段内的相对
       * 偏移，拿**同一份构建**做 `objdump -d` 就能反查到具体的 call 位置。
       * 只读 ucontext，不碰堆，信号上下文里安全。 */
      if(uc) {
        int w2 = snprintf(g_crash_buf, sizeof(g_crash_buf),
                          "  pc=%llx sp=%llx bp=%llx anchor=%llx\n",
                          (unsigned long long)uc->uc_mcontext.mc_rip,
                          (unsigned long long)uc->uc_mcontext.mc_rsp,
                          (unsigned long long)uc->uc_mcontext.mc_rbp,
                          (unsigned long long)(uintptr_t)&crash_handler);
        if(w2 > 0) crash_write(fd, g_crash_buf, (size_t)w2);
      }
#endif
      websrv_diag_recent(g_crash_buf, sizeof(g_crash_buf));
      crash_write(fd, g_crash_buf, strlen(g_crash_buf));
      (void)close(fd);
    }
  }

  signal(sig, SIG_DFL);
  raise(sig);
  /* 正常到不了这里：SIGSEGV/SIGABRT 的默认动作是终止**整个进程**。
   * 但真机上确实出现过「崩了，进程却结束不掉」—— 网页不再响应、PS5 上
   * 也没有能把它关掉的入口，只能重启主机。可能的原因是 raise 只把信号
   * 发给当前线程（崩的通常正是 MHD 的某个 worker 线程），而进程其余的
   * 线程还活着，于是留下一个半损坏、又不肯死的进程。
   * _exit 是 async-signal-safe 的，这里补一刀：不管怎样都别留卡死的尸体。 */
  _exit(1);
}

/* 独立信号栈（static —— 处理器里绝不能 malloc）。
 * 栈溢出时，内核是在**线程栈已经被压穿**的状态下递送 SIGSEGV 的：处理器若
 * 用那条栈执行，第一条 push 就再次溢出 ⇒ 处理器自己都进不来 ⇒ 台账上一片
 * 空白，用户只看到「崩了，可是没有任何记录」。上面那段注释早就写了
 * "a stack-overflow SIGSEGV leaves no room for a stack buffer"，但只把缓冲
 * 改成 static **并不够** —— 缺的是这块独立的**执行栈**。
 * SA_ONSTACK + sigaltstack 是能在栈溢出下留下证据的唯一办法。
 * ⚠️ 32768 写死：PS5 的 SIGSTKSZ 未必是编译期常量（glibc 2.34+ 已改成
 * sysconf 取值），拿它当数组长度会编译不过。 */
static char g_altstack[32768];

static void
install_crash_handler(void) {
  stack_t ss;
  struct sigaction sa;

  ss.ss_sp = g_altstack;
  ss.ss_size = sizeof(g_altstack);
  ss.ss_flags = 0;
  /* 装不上独立栈也继续：至少还能抓住非栈溢出的那些崩溃。 */
  (void)sigaltstack(&ss, NULL);

  memset(&sa, 0, sizeof(sa));
  /* ⚠️ SA_SIGINFO ⇒ 必须挂 sa_sigaction（三参），不是 sa_handler（单参）。
     glibc / FreeBSD 把这两个成员放在同一个 union 里，写错在**宿主** linux
     构建下根本不会报（那条路径不编译这个函数）—— 是 prospero-clang 在设备
     分支里报出来的：
       error: incompatible function pointer types assigning to 'void (*)(int)' */
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_ONSTACK | SA_SIGINFO;   /* SA_SIGINFO 才能拿到 si_addr / si_code */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS,  &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGILL,  &sa, NULL);
  sigaction(SIGFPE,  &sa, NULL);
}

/* ---- home-screen registration, off the startup critical path --------------
 * app_install_if_needed() 走 PS5 安装器 API（dlopen libSceAppInstUtil →
 * Initialize → InstallTitleDir/AppInstallAll）。这些调用一旦阻塞（系统安装
 * 会话卡住 / 系统弹窗没人应答 / 动态库加载僵死），main 就永远走不到
 * websrv_listen —— 用户看到的就是「进程在、网页一直转圈」，而且**每次重启
 * 都复现**（注册没成功，下次启动还会再装一遍）。所以：真正要紧的只有
 * /icon0.png 的资产注册（app_register_assets，同步、纯内存），安装器整段
 * 挪到后台线程 —— 它就算天荒地老地卡住，HTTP 服务照常起。 */
#ifdef __SCE__
static void *
app_install_thread(void *unused) {
  (void)unused;
  app_install_if_needed();
  return NULL;
}

static void
start_app_install_thread(void) {
  pthread_t t;

  if(pthread_create(&t, NULL, app_install_thread, NULL) == 0) {
    pthread_detach(t);
  }
}
#else  /* !__SCE__ */
static void
start_app_install_thread(void) {
  /* app_installer.c 只进 PS5 构建；宿主上没有安装器这条路。 */
}
#endif

static int
port_available(unsigned short port) {
  struct sockaddr_in addr;
  int fd;
  int ret;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  /* 判断"端口是否真有人在监听"不能靠 bind：BSD 系下两个 socket 都带
     SO_REUSEADDR 时，第二个 bind 到同一端口也会成功 —— 旧进程还活着时
     会误判"空闲"，新进程于是也 bind+listen 2026，双进程同端口、连接被
     内核随机分给旧进程，用户看到的就是"两个进程 + 打不开"。
     改成 connect 探测：connect 成功 = 端口确实被监听（占用，返回 0）；
     ECONNREFUSED = 没人监听（空闲，返回 1）。TIME_WAIT 残留 socket 不会
     accept，探测同样判空闲，不影响 SO_REUSEADDR 的正常重绑。 */
  ret = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) ? 0 : 1;
  close(fd);
  return ret;
}

static unsigned short
find_available_port(unsigned short start) {
  unsigned int port;

  for(port = start; port <= 65535; port++) {
    int available = port_available((unsigned short)port);
    if(available < 0) {
      return 0;
    }
    if(available) {
      return (unsigned short)port;
    }
  }
  return 0;
}

#ifdef __SCE__
static pid_t
find_pid(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }
  if(!(buf = malloc(buf_size))) {
    perror("malloc");
    return -1;
  }
  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    free(buf);
    return -1;
  }

  for(uint8_t *ptr = buf; ptr < buf + buf_size;) {
    int ki_structsize = *(int *)ptr;
    pid_t ki_pid = *(pid_t *)&ptr[72];
    char *ki_tdname = (char *)&ptr[447];

    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && ki_pid != mypid) {
      pid = ki_pid;
    }
  }

  free(buf);
  return pid;
}
#endif

int
main(int argc, char **argv) {
  unsigned short port;
#ifdef __SCE__
  unsigned short notified_port = 0;
  pid_t pid;
#endif

  (void)argc;
  (void)argv;

#ifdef __SCE__
  {
    /* ---- 统一工作目录 /data/Mango/ ---- 日志目录先行建好：start/crash 日志
       从第一行就要能落盘（写失败静默，但目录在启动时建一次成本极低）。 */
    mkdir("/data/Mango", 0777);
    mkdir("/data/Mango/logs", 0777);

    /* ---- 单实例治理（2026-10-06 真机反馈：加载后两个进程 + 2026 打不开）----
       find_pid 靠线程名匹配旧进程，名字对不上就杀不掉；而 BSD 的
       SO_REUSEADDR 会让新进程照样 bind+listen 同一个 2026 —— 双进程同端口、
       连接被内核随机分给旧进程 = "打不开"。每一环的结果都追加进
       /data/Mango/logs/mango-crash.log（启动日志复用崩溃日志文件），真机一次加载就能看出断点。 */
    int thr_ok = (syscall(SYS_thr_set_name, -1, PROCESS_NAME) == 0);
    mango_log("thr_set_name(%s) = %s", PROCESS_NAME,
                    thr_ok ? "ok" : "FAILED");
    /* Reap anything left over from an earlier load.
     *
     * PROCESS_NAME alone is not enough the first time a user upgrades: the
     * console may still be running an instance whose thread name came from the
     * previous build's file name. Nothing else on the PS5 will clean it up (no
     * service manager), and two live instances mean the newcomer silently falls
     * through to port 2027 instead of taking over 2026 — which looks like
     * "the update did not apply". So match the historical names too.
     * find_pid() skips our own pid, so listing PROCESS_NAME here is safe. */
    static const char *const stale_names[] = {
      PROCESS_NAME,    /* PS5-Mango.elf — this build */
      "PS5-Nexus.elf", /* 改名前的构建（真机上大概率还跑着，不杀就占着 2026） */
      "NEXUS.elf",     /* 更早的裸名构建 */
      "ps5-nexus.elf", /* 小写变体 */
      "PS5-Mango.elf", /* 与 PROCESS_NAME 同值，find_pid 会跳过自己，留着无害 */
    };
    size_t i;

    for(i = 0; i < sizeof(stale_names) / sizeof(stale_names[0]); i++) {
      pid = find_pid(stale_names[i]);
      if(pid > 0) {
        mango_log("found stale '%s' pid=%d -> SIGKILL",
                        stale_names[i], (int)pid);
        if(kill(pid, SIGKILL)) {
          /* 卡在内核调用里的进程处于不可中断睡眠，SIGKILL 也带不走它。
             ⚠️ 旧代码在这里 return 1：新进程当场退出 ⇒ 插件管理器看它"死了"就
             再注入 ⇒ 无限重启，而老进程还占着端口和 /data/mount_sd 挂载点 ⇒
             用户看到「一直在自启、功能全不能用」。现在只记台账、继续往下走：
             下面那 10 秒端口复核会把真实情况判出来，落到下一个可用端口，
             至少保证**有一个能用的实例**。 */
          mango_log("kill(%d) FAILED: %s (likely stuck in an uninterruptible "
                    "kernel call) — continuing to the port probe",
                    (int)pid, strerror(errno));
          perror("kill");
        } else {
          sleep(1);
        }
      } else {
        mango_log("no stale '%s' (find_pid=%d)",
                        stale_names[i], (int)pid);
      }
    }
  }
#else
#endif

  puts(PROCESS_NAME);
  printf("PS5 Mango %s\n", VERSION_TAG);
  /* 台账里也留一行版本。真机上换 ELF 是家常便饭，而这个日志文件是 O_APPEND
   * 累积的 —— 没有这一行，「这段记录是哪个版本写的」就永远说不清。
   * 2026-10-11 就是为此把**旧版**的崩溃当成了新版的：台账里那些 [start ...] 段
   * 其实是更早的构建留下的（当前源码只写 [log ...]，见 mango_log.h 的更正）。
   * VERSION_TAG 来自 Makefile 的 -D，__DATE__/__TIME__ 是编译时刻。 */
  mango_log("PS5-Mango %s starting (built %s %s)", VERSION_TAG, __DATE__, __TIME__);

  /* 图标资产先注册（纯内存、不会阻塞）；安装器在服务起来之后再后台跑
   * （app_installer.c 只进 PS5 构建，宿主的 /icon0.png 由 gen 资产模块供）。 */
#ifdef __SCE__
  app_register_assets();
  start_app_install_thread();
#endif

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
  install_crash_handler();

  /* 崩溃/异常退出后的自恢复：清掉上次留下的暂存与残留挂载，报一次空闲空间。
     放在服务起来之前 —— 后面所有导入都依赖「/data 有地方写、挂载点是干净的」。 */
  savemgr_boot_reconcile();

#ifdef __SCE__
  /* 2026 兜底：单实例杀完仍被监听 = 有个名字匹配不上的进程占着端口（旧版没
     设线程名 / 内核偏移不匹配 / kill 权限不足）。
     ⚠️ 这里**不再直接退出**（2026-10-10 拍板）。原实现死等 10 秒后 return 1，
     用户面前就是「打开插件，什么都没有」；而下面 find_available_port() 走的是
     connect 探测（见 port_available 的注释），2026 有人监听时它必然跳过、落到
     2027，再把真实端口经 notify_user 报出来。退出的代价远大于「多起一个落位
     端口」。
     ⚠️ 但也不能一发现占用就立刻落位（2026-10-07 真机反馈「强杀进程后再运行，
     网页一直转圈」）：强制结束后内核侧的连接残留在几秒内可能让 connect 探测
     仍然成功 —— 那几秒后残连接会自己消失，所以先重试一小段，只有确认 10 秒
     后仍在，才判定「另有一个活着的监听者」并落位下一端口。 */
  {
    int occupied = 0;

    for(int attempt = 0; attempt < 10; attempt++) {
      int avail = port_available(DEFAULT_PORT);

      if(avail < 0) {
        /* socket() 都建不出来 = 网络栈异常，逐端口重试只会空转。 */
        mango_log("port probe on %u failed: %s; cannot continue", DEFAULT_PORT,
                  strerror(errno));
        fprintf(stderr, "socket() failed on port %u: %s\n", DEFAULT_PORT,
                strerror(errno));
        return 1;
      }
      if(avail) {
        occupied = 0;
        break;
      }
      occupied = 1;
      if(attempt == 0) {
        mango_log("port %u occupied; rechecking for 10s before falling back",
                        DEFAULT_PORT);
      }
      sleep(1);
    }
    if(occupied) {
      mango_log("%u still occupied after 10s; falling back to the next free port "
                "(an older Mango may still be listening)", DEFAULT_PORT);
      fprintf(stderr, "port %u in use by another process; falling back to the "
                      "next free port (log: " MANGO_LOG_PATH ")\n", DEFAULT_PORT);
    }
  }
#endif

  start_app_install_thread();

  /* 起始端口可用 WFM_PORT 覆盖：宿主上 Windows/Hyper-V 的动态保留范围会整段
   * 吃掉 2024-2123（connect 探测说空闲、bind 却 EADDRINUSE），逐端口重试要
   * 每轮 sleep(3) 慢慢爬几分钟。hosttest 检测到 2026 绑不上时设这个变量
   * 直达可用端口。真机上没人设它，行为不变。 */
  unsigned short probe_from = DEFAULT_PORT;
  {
    const char *e = getenv("WFM_PORT");
    if(e && e[0]) {
      int v = atoi(e);
      if(v > 0 && v < 65536) probe_from = (unsigned short)v;
    }
  }

  while(1) {
    port = find_available_port(probe_from);
    if(!port) {
      fprintf(stderr, "no available port from %u\n", probe_from);
#ifdef __SCE__
      mango_log("no available port from %u; retrying in 3s", probe_from);
#endif
      sleep(3);
      continue;
    }

    printf("listening on port %u\n", port);
#ifdef __SCE__
    mango_log("selected port %u", port);
    if(notified_port != port) {
      /* 报完整 URL，不只有端口号（2026-10-23 用户要求）：IP 取不到时降级回
         旧格式，别在通知里留一个 "http://:2026" 的破形状。 */
      char ip[64];
      mango_local_ip(ip, sizeof(ip));
      if(ip[0]) {
        mango_log("serving http://%s:%u", ip, port);
        notify_user("PS5 Mango %s\nhttp://%s:%u", VERSION_TAG, ip, port);
      } else {
        notify_user("PS5 Mango %s\nPort: %u", VERSION_TAG, port);
      }
      notified_port = port;
    }
#endif

    /* 启动排障（2026-10-07 真机反馈「网页打不开但进程还在」）：listen 返回值
       直接落盘 —— -1 = socket/bind/listen/MHD 起不来（端口被占、资源耗尽），
       0 且 stop=0 = accept 循环异常退出（连接风暴/内核把连接拒了）。 */
    {
      int lrc = websrv_listen(port);
      (void)lrc;              /* 宿主构建没有启动日志，返回值只进日志 */
#ifdef __SCE__
      mango_log("websrv_listen(%u) returned %d; stop_requested=%d parked=%d", port,
                      lrc, websrv_stop_requested() ? 1 : 0, websrv_parked() ? 1 : 0);
#endif
      /* 停车（2026-10-11「崩了别再自启」）：内核楔死后 websrv_park 已被调用，
         服务已拆、端口已释放。这里**必须驻留**而不是 return/重试 —— 进程一死
         注入器就重新注入，新实例撞上内核里的坏状态，循环就回来了。sleep 驻留
         不是不可中断睡眠：payload 管理器手动结束时它一杀就净，端口已释放，
         重注入的新实例能正常接管 2026。 */
      if(websrv_parked()) {
        mango_log("main: parked after wedge; idling until manually reaped");
        fprintf(stderr, "service parked (kernel wedged); end the process to "
                        "recover (log: " MANGO_LOG_PATH ")\n");
        for(;;) {
          sleep(60);
        }
      }
      if(websrv_stop_requested()) {
        break;
      }
      /* bind 失败的端口下一轮要跳过：connect 探测判「空闲」但 bind 被拒的
        * 情况真实存在（宿主上 Windows/Hyper-V 的动态保留范围 2024-2123 就是
        * connect 拒绝、bind EADDRINUSE），不推进的话会永远钉死在同一个端口
        * 上打转。 */
      if(probe_from < 65535) probe_from = port + 1;
    }
    sleep(3);
  }

  /* 关闭路径的收尾：给 stop_websrv_later 里那个"回桌面"（真机才编译）留一段
     **有界**的时间。⚠️ 必须有界 —— 那条路要走 ShellUI，可能长时间不返回，等它
     就等于把"关不掉"重新引回来。1 秒足够 ShellUI 收下 URI；无论它有没有回来，
     进程到这里都退出。服务本身在这之前就已经停了（websrv_listen 里关的 fd）。 */
  sleep(1);

  /* 台账的最后一行。它和 filemgr.c/websrv.c 写的那几行连起来读，就能分辨
     「关闭没生效」和「真的退出了、又被外部重新注入」：后者会留下新的启动段，
     而前者连这一行都不会出现。 */
  mango_log("main loop exited (stop_requested=%d); process returning 0",
            websrv_stop_requested() ? 1 : 0);
  return 0;
}
