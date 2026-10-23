#pragma once
/* src/mango_port.c 的对外最小面。
 *
 * 目前只有一个 mango_local_ip()：main.c 的开机通知要用它把
 * 「http://<ip>:<port>」直接报给用户（2026-10-23 用户要求：不只要端口号）。
 * 实现是 UDP connect 8.8.8.8 的选路窍门，不真发包；取不到时出参为空串。 */
#include <stddef.h>

void mango_local_ip(char *out, size_t n);
