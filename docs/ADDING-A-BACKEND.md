# 新增一个传输后端（transfer backend）

> 本文是 **扩展性契约**：以后再要接一种新协议（WebDAV 之外的：SFTP、FTP、
> 对象存储、云盘原生 API……），按这里的 3 步做，**不需要改 transfer 的核心
> 逻辑，更不需要重写**。
>
> 如果你发现按本文做不下来、必须去动 `transfer.c` 的 dispatch —— 那说明
> seam 又漏了，请先修 seam，不要绕过它。

---

## 0. 一句话

**新建 1 个后端文件 + 3 处单行登记（枚举一项、注册表一行、Makefile 一行）。**
没有一处是改 dispatch 逻辑。

---

## 1. 重构前后的成本对比（为什么会有这份文档）

`transfer` 是「可读流 + 可列目录」的唯一抽象，本地 / SMB / NFS / WebDAV /
以后的云盘都从这一个口子进来，所以复制、安装、校验、断点续传、存档备份
全都复用同一条代码路径。这个设计一直是对的。

问题在**加新后端的时候**：

| | 重构前 | 重构后 |
|---|---|---|
| 要改的地方 | **8~12 处**（scheme 枚举、`parse_scheme`、`scheme_is_known`、`transfer_scheme_supported`、`transfer_open` 的「未编译」判断、URL 解析分支、初始化，再乘 `list`/`size`/`remove`/`read`/`close` 五处 dispatch 各一段 `if`） | **3 处单行**（枚举一项 + 注册表一行 + Makefile 一行） |
| 要改的文件 | `transfer.c`（核心）+ `Makefile` | 新文件 + `transfer.c` 的注册表 + `Makefile` |
| 是否碰 dispatch 逻辑 | 是，5 个函数各加一段 `if (scheme == ...)` | **否，一行都不用碰** |
| 会不会撑大核心结构体 | 会（每个后端往 `struct transfer_src` 塞自己的字段） | 不会（状态放 `c->state`） |
| 只编译 local 时能不能过 | 不能，要为每个 `#if` 补分支 | **能，零改动** |
| 加错后端会不会影响别人 | 会（字段混用、`#if` 嵌套） | 不会（后端看不到核心结构体） |

---

## 2. seam 在哪

```
transfer.h            公开 interface：open/list/size/remove/read/close
        ↑
transfer_internal.h   后端插槽：transfer_backend_t（vtable）+ transfer_ctx_t
        ↑
transfer.c            dispatch：只做「查表 + 转调」，不含任何后端分支
        ↑
backend_*.c           各后端的实现（互不可见）
```

关键约束，也是这套设计能成立的原因：

**后端看不到 `struct transfer_src`。** 它只拿到一个
`transfer_ctx_t c`，里面就 6 个字段：

| 字段 | 方向 | 含义 |
|---|---|---|
| `c->url` | in | 原始 URL（含 `scheme://` 前缀） |
| `c->loc` | in | 已解析的 `server / share / path / user / pass / port`；local 后端为 `NULL` |
| `c->state` | in/out | **你自己**的私有状态。`transfer_open` 时是 `NULL`，你在 `open` 里 malloc，在 `close` 里 free |
| `c->err` / `c->err_cap` | out | 失败原因，用 `transfer_ctx_err(&c, "...")` 写 |
| `c->owner` | — | ⚠️ **仅 transfer.c 内部后端可用**，外部文件不要碰 |

因为看不到核心结构体，所以：**你加后端不会撑大它、不会和别人的字段打架、
别人也不会被你的 `#if` 影响。** 这是「加功能不用重写」的物理保证。

---

## 3. 接入步骤（1 个新文件 + 3 处单行登记）

### 第 1 步：新建 `src/backend_<名字>.c`

```c
/* src/backend_sftp.c —— SFTP 后端（示例：照抄改逻辑即可） */
#include "transfer_internal.h"

/* ---- 你的私有状态。放这里，不要放 struct transfer_src ---- */
typedef struct {
  int  sock;          /* 自己管连接 */
  void *session;      /* 后端库的上下文 */
} sftp_state_t;

#define ST(c) ((sftp_state_t *)((c)->state))

static int
sftp_open(transfer_ctx_t *c) {
  sftp_state_t *st = calloc(1, sizeof(*st));
  if(!st) { transfer_ctx_err(c, "sftp: out of memory"); return 0; }
  st->sock = -1;
  c->state = st;                       /* ← 交回去，之后每次调用都给你 */
  return 1;                            /* 懒连接：这里只置状态，不连网 */
}

static void
sftp_close(transfer_ctx_t *c) {
  sftp_state_t *st = ST(c);
  if(!st) return;
  /* 拆连接、free 后端库上下文 */
  free(st);
  c->state = NULL;
}

static mango_err_t
sftp_list(transfer_ctx_t *c, const char *dir,
          transfer_entry_t **out, int *count) {
  sftp_state_t     *st = ST(c);
  transfer_entry_t *list = NULL;
  int               n = 0, cap = 0;

  (void)st;
  /* 1) 连上（首次）  2) 列目录  3) 逐项 transfer_list_push() */
  if(/* 连不上 */ 0) {
    transfer_ctx_err(c, "sftp: connection refused");   /* ← 一定要写原因 */
    return MANGO_ERR_NOTFOUND;
  }
  /* transfer_list_push(&list, &n, &cap, name, size, mtime, mode, is_dir); */
  *out   = list;
  *count = n;
  return MANGO_OK;
}

static mango_err_t
sftp_size(transfer_ctx_t *c, uint64_t *size) { (void)c; *size = 0; return MANGO_OK; }
static mango_err_t
sftp_remove(transfer_ctx_t *c, const char *p, int is_dir) { (void)c; (void)p; (void)is_dir; return MANGO_OK; }
static mango_err_t
sftp_read(transfer_ctx_t *c, uint64_t off, void *b, size_t l, size_t *got) {
  (void)c; (void)off; (void)b; (void)l; *got = 0; return MANGO_OK;
}

/* ---- 注册项：这就是新后端在 seam 上的「一行」 ---- */
static const char *const sftp_schemes[] = { "sftp", NULL };

const transfer_backend_t mango_backend_sftp = {
  "sftp", SCHEME_SFTP, sftp_schemes,
  sftp_open, sftp_close, NULL,          /* open / close / repoint */
  sftp_list, sftp_size, sftp_remove, sftp_read, NULL
};
```

**写不动的操作填 `NULL`**，dispatch 会返回 `MANGO_ERR_UNSUPPORTED`，
不会崩。

### 第 2 步：`src/transfer_internal.h` 的枚举加一项

只在**全新 scheme** 时需要（改的是枚举，不是逻辑）：

```c
typedef enum {
  SCHEME_LOCAL = 0,
  SCHEME_SMB,
  SCHEME_NFS,
  SCHEME_WEBDAV,
  SCHEME_SFTP          /* ← 加这一项 */
} transfer_scheme_t;
```

这个 `id` 只用于填 `s->scheme` 给少量历史分支看，dispatch 并不依赖它。

### 第 3 步：在 `src/transfer.c` 注册表登记（唯一必须改核心文件的地方）

文件末尾的 `g_backends[]`：

```c
static const transfer_backend_t *const g_backends[] = {
  &mango_backend_local,
#if defined(MANGO_HAVE_LIBSMB2)
  &mango_backend_smb,
#endif
  ...
#if defined(MANGO_HAVE_SFTP)
  &mango_backend_sftp,          /* ← 加这一行 */
#endif
};
```

同时在文件靠近顶部的地方补两行声明：

```c
#if defined(MANGO_HAVE_SFTP)
extern const transfer_backend_t mango_backend_sftp;
#endif
```

> 为什么不用 `__attribute__((constructor))` 自注册省掉这两行？
> 因为 PS5 构建带 `-Wl,--gc-sections`。constructor 依赖 `.init_array`
> 存活，一旦被裁就是**「后端静默不工作」**——本项目在
> `Makefile:243` 已经因为这个坑吃过一次（NAS 静默不连）。
> 显式登记表可读、可 grep、链接期就能发现漏注册，值这两行。

### 第 4 步：`Makefile`

```makefile
COMMON_SRCS += src/backend_sftp.c      # 加到源文件列表
CFLAGS       += -DMANGO_HAVE_SFTP      # 加到 MANGO_HAVE_* 段（必须在 base CFLAGS 之后！）
LINUX_CFLAGS += -DMANGO_HAVE_SFTP
```

⚠️ `MANGO_HAVE_*` 必须写在 `CFLAGS := ...` 那些 `:=` 赋值**之后**。
`:=` 会覆盖 `+=`，放前面会静默失效（见 `Makefile:243` 的注释）。

### 完事。验证一下：

```bash
gcc -fsyntax-only -O2 -Wall -Wextra -Werror -Isrc -DMANGO_HAVE_SFTP src/transfer.c
make linux && make test-host          # 期望 299 passed, 0 failed
```

---

## 4. 红线（做了就会重新变成「加功能要重写」）

| ❌ 不要 | 为什么 |
|---|---|
| 往 `struct transfer_src` 加字段 | 那是老后端的遗留做法。加的人一多，结构体就成了所有后端的垃圾场，改一个字段要看懂全部后端 |
| 在 `transfer_list/size/remove/read/close` 里加 `if(s->scheme == ...)` | dispatch 必须保持「零后端知识」。加一个分支，以后每加后端都得回来改 |
| 用 `c->owner` 摸核心结构体 | `owner` 只给 transfer.c 内部后端用。外部后端用了就等于依赖核心结构体，它一改你就断 |
| 只返回错误码、不写 `c->err` | 用户只会看到「失败」。写清原因（`"sftp: auth failed"`），UI 会原样显示 |
| 用全局/静态变量存连接 | 真机上多任务并发会互相踩。状态一律放 `c->state` |
| 在 `close` 里 `free(s)` | `close` 只释放**你的** `c->state`，`transfer_src` 由 `transfer_close` 回收 |

---

## 5. 验证清单

新后端合并前，至少跑通这四项：

- [ ] `gcc -fsyntax-only -Wall -Wextra -Werror`（三后端组合 + 仅 local 组合各一次）
- [ ] `make linux` 成功（`RC=0`）
- [ ] `make test-host` → `299 passed, 0 failed`
- [ ] 行为测试：起一个本地最小服务端 + 一个 C harness，真实驱动
      `transfer_open → list → size → read(Range) → remove`，逐项断言
      （范式见 `.build/webdav-verify/`）

第 4 项最重要：前三项只证明「编得过」，它证明「真的能用」。

---

## 6. 现在的后端

| 后端 | scheme | 文件 | 有状态 | 懒连接 |
|---|---|---|---|---|
| local | `local:`（默认） | `transfer.c` | 只一个 fd | 是 |
| smb | `smb:` | `transfer.c`（SMB 段） | 连接 + 句柄 + 预读窗口 | 是 |
| nfs | `nfs:` | `transfer.c`（NFS 段） | 挂载 + 句柄 | 是 |
| webdav | `webdav:` / `dav:` | `transfer.c`（WebDAV 段） | TCP 连接 | 是 |

> smb / nfs / webdav 目前仍和 `transfer.c` 同文件（它们是在重构前写的，
> 搬出去属于独立的重构，不与「加后端」这事混在一起）。**它们已经全程走
> vtable**，所以搬家只是把 `owner` 换成 `state` 的机械动作，seam 形状不变。
> 新增后端请一律用独立文件，不要再往 `transfer.c` 里加。
