#pragma once

/* ---- process ledger -------------------------------------------------------
 * 真机唯一的排障通道。浏览器注入的 payload 没有 shell、没有 coredump，用户能
 * 描述出来的只有三种现象：「打不开」「关不掉」「关了又自己起来」。这三句都不
 * 指向具体哪一环，所以每一环的结论都落盘：单实例治理（线程名设置 / 旧进程匹配
 * / kill / 端口占用）、启动、以及**关闭进程**这条路径。
 *
 * 为什么关闭路径也要记：主机上没有任何 service manager 会拉起这个进程，所以
 * 「关了又在」只有两种可能 —— (a) 根本没关掉（关停动作没走到），(b) 真的退出
 * 了但被外部重新注入。这两者的区别只有台账能分辨：入口一律先写一行；真的退出
 * 过，下一次启动必然留下新的 **[log ...] 段**；没有新段就只有 (a) 成立。
 * ⚠️ 这里曾经写成 `[start] 段` —— **代码从来没有写过 [start 这个前缀**
 *    （mango_log() 写死的头就是 `[log <时间> pid=N]`）。于是现场台账里出现
 *    `[start ...]` 时，唯一合理的解释是：那是**更早的一份构建**留下的历史行
 *    （日志是 O_APPEND，跨版本累积）。2026-10-11 曾据此把旧版的崩溃当成新版的，
 *    所以：**读台账先看 `[log ...] PS5-Mango vX.Y.Z starting` 那一行确认版本**。
 *
 * 文件与崩溃日志**共用一个路径**（真机 /data/Mango/logs/mango-crash.log），
 * 这是故意的：真机上只可能拿到一个文件，分家会让人少看一半。
 *
 * ⚠️ 宿主构建是**编译期空操作**。hosttest 判崩溃的判据就是「工作目录里出现了
 * mango-crash.log」（见 .build/run-hosttest-wsl.sh），宿主上写台账会把那条判据
 * 污染成恒真。所以这里不是"少记一点"，是故意不记。 */
#ifdef __SCE__
#define MANGO_LOG_PATH "/data/Mango/logs/mango-crash.log"
#else
#define MANGO_LOG_PATH "mango-crash.log"
#endif

void mango_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
