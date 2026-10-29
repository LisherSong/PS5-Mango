#ifndef TRANSFER_INTERNAL_H
#define TRANSFER_INTERNAL_H
/* 内部 seam —— 只给 transfer.c 与 backend_*.c 使用，不是公开 interface。
 *
 * ⚠️ 公开 interface 在 transfer.h（open/list/size/remove/read/close）。
 *    这里是它背后那层「可替换后端」的插槽定义。
 *
 * ---------------------------------------------------------------- *
 * 为什么存在（重构前的事实）
 * ---------------------------------------------------------------- *
 * transfer.h 开头写着「New backends are added here only — no other module
 * changes」，但实现是反的：加一个后端要改 transfer.c 的 8 处 ——
 *     enum / parse_scheme / scheme_is_known / scheme_supported /
 *     transfer_open 的「未编译进来」分支 / URL parse 分支 / init /
 *     再乘上 list、size、remove、read、close 五处 dispatch。
 * 每加一个源都要把核心文件改一遍，这就是「加功能要重写」的来源。
 *
 * ---------------------------------------------------------------- *
 * 现在：加一个后端 = 新建 src/backend_xxx.c + Makefile SRCS 加一行
 * ---------------------------------------------------------------- *
 *   1. backend_xxx.c 里写一个 `const transfer_backend_t mango_backend_xxx`，
 *      把下表里用得着的函数指针填上，用不着的留 NULL（dispatch 会返回
 *      MANGO_ERR_UNSUPPORTED，不会崩）；
 *   2. 在 transfer.c 的注册表 `g_backends[]` 里加一行 `&mango_backend_xxx`
 *      （用该后端自己的 MANGO_HAVE_* 宏保护）；
 *   3. dispatch 的 6 个函数**一行都不用改**。
 *
 * 后端私有状态统一放 `transfer_ctx_t.state`（transfer_open 时清零）。
 * 这是刻意的：**后端看不到 `struct transfer_src`**，所以核心结构体
 * 不会因为新后端加字段而膨胀，也不会出现「A 后端的字段被 B 后端误用」。
 */

#include "transfer.h"
#include <stdio.h>    /* transfer_ctx_err 用 snprintf */

/* ---------------------------------------------------------------- *
 * 后端看到的全部上下文（刻意做小）
 *
 * Deletion test：删掉这个 struct，后端就只能直接摸 transfer_src，
 * 于是每加一个后端都要往核心 struct 里塞字段 —— 复杂度回流到核心。
 * 它存在的价值就是挡住这件事。
 * ---------------------------------------------------------------- */
typedef struct transfer_ctx {
  const char           *url;    /* in: 原始 URL（含 scheme:// 前缀） */
  const transfer_nas_t *loc;    /* in: 已解析的 server/share/path/user/pass/port；
                                        local 后端为 NULL */
  void                 *state;  /* in/out: 后端私有状态，transfer_open 时置 NULL */
  char                 *err;    /* out: 失败原因（人能读，会原样上抛到 UI） */
  size_t                err_cap;
  /* transfer.c 内部后端（local）用的逃生口：指向 transfer_src。
   * ⚠️ 外部 backend_*.c **不要**碰它 —— 碰了就等于依赖核心结构体，
   *    下次核心结构体一改你这边就断。新后端一律只用 state。 */
  void                 *owner;
} transfer_ctx_t;

/* 写一条失败原因。仿 set_err()，但作用于 ctx，让 backend_*.c 不必知道
 * transfer_src 长什么样。msg 为 NULL/空时写 "unknown error"。 */
static inline void
transfer_ctx_err(transfer_ctx_t *c, const char *msg) {
  if(!c || !c->err || !c->err_cap) return;
  snprintf(c->err, c->err_cap, "%s", (msg && *msg) ? msg : "unknown error");
}

/* ================================================================ *
 * 后端 vtable —— 这就是 seam
 *
 * 判据（codebase-design）：已经有 local / smb / nfs / webdav 四个
 * adapter 落在它上面 ⇒ 它是**真实 seam**，不是给「将来可能」预留的空壳。
 *
 * 约定：
 *   - 连接/句柄等可变状态放 c->state，不要用全局/静态变量（真机上
 *     多任务并发会互相踩）。
 *   - 失败时先 transfer_ctx_err() 写原因再返回错误码；只返回错误码
 *     = 用户只看到「失败」，这正是以前 NAS 连不上时最难查的那类 bug。
 *   - close 只释放连接与 state，不要 free 其它东西。
 *   - chunk 返回 0 表示「用默认值」（transfer_read_chunk 的 1 MB 地板）。
 * ================================================================ */
typedef struct transfer_backend {
  const char        *name;     /* 诊断用，如 "webdav" */
  /* 稳定标识，用于填 transfer_src.scheme 与少量历史分支 */
  int                id;       /* 取值为下面的 SCHEME_* */
  /* NULL 结尾的 scheme 别名表，如 {"webdav","dav",NULL}。
   * 查表按**最长前缀匹配**（"webdav" 先于 "dav" 命中），与旧行为一致。 */
  const char *const *schemes;

  /* 建立/初始化连接。1 成功、0 失败。可为 NULL（local / 懒连接的后端）。 */
  int  (*open)(transfer_ctx_t *c);
  void (*close)(transfer_ctx_t *c);
  /* transfer_repoint() 换文件时通知后端：把旧文件的句柄 / 在途请求作废。
   * 可为 NULL（无每文件状态的后端，如 local / webdav 的无状态 GET）。
   * ⚠️ 必须同步作废：迟到的旧请求会把上一份文件的字节交给新偏移。 */
  void (*repoint)(transfer_ctx_t *c);

  mango_err_t (*list)(transfer_ctx_t *c, const char *dir,
                      transfer_entry_t **out, int *count);
  mango_err_t (*size)(transfer_ctx_t *c, uint64_t *size);
  mango_err_t (*remove)(transfer_ctx_t *c, const char *path, int is_dir);
  mango_err_t (*read)(transfer_ctx_t *c, uint64_t offset,
                      void *buf, size_t len, size_t *got);
  size_t (*chunk)(transfer_ctx_t *c);
} transfer_backend_t;

/* ---------------------------------------------------------------- *
 * scheme 枚举 —— 仅为兼容 transfer.c 内部的老分支（repoint/open）保留。
 * dispatch 已不再读它，而是直接走 `s->be`。
 * ---------------------------------------------------------------- */
typedef enum {
  SCHEME_LOCAL = 0,
  SCHEME_SMB,
  SCHEME_NFS,
  SCHEME_WEBDAV
} transfer_scheme_t;

/* ---------------------------------------------------------------- *
 * 共享助手 —— 由 transfer.c 提供
 * ---------------------------------------------------------------- */
/* 向列目录结果追加一项（自动扩容、截断过长名字，兼容非 NUL 结尾输入）。
 * 所有后端共用，避免各写一份 realloc。 */
mango_err_t transfer_list_push(transfer_entry_t **list, int *n, int *cap,
                               const char *name, uint64_t size,
                               uint64_t mtime, uint32_t mode, int is_dir);

#endif /* TRANSFER_INTERNAL_H */
