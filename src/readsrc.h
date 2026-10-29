#ifndef READSRC_H
#define READSRC_H
/* 统一只读源：本地文件（open/pread）与 NAS（transfer 抽象）共用一套接口。
 *
 * 为什么要有这一层（2026-10-28 真机三条反馈）：
 *   ① WebDAV 上点图片「已截断」——/api/fs/read 把整块读进内存再 base64，
 *      于是只能封 4 MiB 的 cap（再大内存翻 3 倍）；
 *   ② WebDAV 上点下载 / 播视频报「下载失败：file not found」——
 *      /api/download/prepare 对每个 path 做 lstat()，而 NAS 的 path 是
 *      host/share/x，本地 lstat 必然 ENOENT。
 *   「本地专用路径收到了 NAS 地址」这个根因家族已经是第三次出现（PKG 直装、
 *   /api/fs/read、/api/download）。与其每处各写一份 if(scheme)，不如把
 *   「按作用域打开一个可随机读的字节流」收敛成这一个模块：调用方只认
 *   open/pread/size/close，加后端不用改调用方。
 *
 * ★ 不提供写路径：下载/预览/播放都只读，写入语义留给 /api/nas/copy 之类。 */
#include <stddef.h>
#include <stdint.h>

typedef struct read_src read_src_t;

/* 打开一个只读源。
 *   scheme 为空/"local" ⇒ 本地文件；否则按 NAS 处理（scheme 必须是后端
 *   注册表里已登记的，未登记直接失败，不让调用方偷偷退化成本地路径）。
 * 🪤 总是返回对象（只有 calloc 失败才返回 NULL）：失败用 read_src_bad() 判，
 *    原因用 read_src_error() 取 —— 早年「打开失败就返回 NULL」的写法让
 *    调用方只能报一句 file not found，把「凭据错」说成「文件不存在」。
 * 无论成败，用完都必须 read_src_close()。 */
read_src_t *read_src_open(const char *path, const char *scheme,
                          const char *user, const char *pass,
                          const char *port);
int read_src_bad(read_src_t *s);
/* 随机读。成功返回 0（*got 为实际读到的字节，短读不视为错误），失败返回 -1。 */
int read_src_pread(read_src_t *s, uint64_t off, void *buf, size_t len,
                   size_t *got);
uint64_t read_src_size(read_src_t *s);
/* 1 = NAS 源（transfer），0 = 本地 fd。调用方靠它决定报错口径：NAS 的失败
 * 不许说成「文件不存在」。 */
int read_src_is_nas(read_src_t *s);
/* 失败原因：NAS 走后端的原话（NT_STATUS_… / mount refused / HTTP 状态），
 * 本地走 strerror。 */
const char *read_src_error(read_src_t *s);
void read_src_close(read_src_t *s);
#endif
