/* NAS 地址切分：把「用户填的地址」拆成 (主机[:端口], 剩余路径)。
 *
 * 为什么非要单独一个文件（2026-10-28 真机）：这条切分规则当年被抄了三份
 * （list.c 的 nas_build_url、mango_port.c 的 nas_compose_url、pkg_info.c 的
 * pkg_nas_compose_url），而且**规则不一致** —— list.c 那份会跳过前导分隔符，
 * 另两份见到开头是 '/' 就直接判死。结果同一个地址：
 *     /192.168.1.3:5005/dav/game.pkg
 * 列目录、读图、下载全过（跳过前导 '/' ⇒ host=192.168.1.3:5005），
 * PKG 读元信息失败、PKG 安装入队 400 "invalid NAS address"（hostlen==0）。
 * 用户看到的就是「图片能看了，PKG 却读不出来」。
 *
 * ⚠️ 前导分隔符不是垃圾：用户从资源管理器「复制路径」拿到的就是
 * \\192.168.1.3\dav 这种 UNC 形状，前端归一化后正是 //192.168.1.3/dav。
 * 所以正确做法是**跳过**它们，而不是拒绝。
 *
 * 只有这一处规则；三处拼接都必须调它，不许再各写一份 strcspn。
 */
#ifndef MANGO_NAS_ADDR_H
#define MANGO_NAS_ADDR_H

#include <stddef.h>

/* 在 path 里切出主机段写进 host（保证 NUL 结尾），*rest 指向从第一个分隔符
 * 起的剩余部分（没有分隔符则指向 ""）。
 * 返回 0 成功；-1 表示地址里根本没有主机（空串 / 只剩分隔符 / 主机段过长）。
 * ⚠️ *rest 指向 path 内部，调用方必须在 path 有效期内使用。 */
int nas_addr_split(const char *path, char *host, size_t host_size,
                   const char **rest);

#endif /* MANGO_NAS_ADDR_H */
