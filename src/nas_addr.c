#include "nas_addr.h"

#include <string.h>

/* 分隔符有两种：UNC 形状用 '\\'（\\host\share），手打或复制路径用 '/'
 * （192.168.1.3/dav）。两种都要认，只认 '/' 的话反斜杠会整段落进 host。 */
int
nas_addr_split(const char *path, char *host, size_t host_size,
               const char **rest) {
  size_t cut;

  if(!path || !host || !host_size) return -1;
  /* ★ 跳过前导分隔符：UNC 的第一个字符就是分隔符，不跳的话主机段长度 0，
   * 后面 transfer_url_parse 会把整串丢给 getaddrinfo，报
   * "Invalid address:… Can not resolve into IPv4/v6"。 */
  while(*path == '/' || *path == '\\') path++;
  cut = strcspn(path, "/\\");
  if(!cut || cut >= host_size) return -1;
  memcpy(host, path, cut);
  host[cut] = 0;
  *rest = path + cut;                 /* 指向 '/…' 或 ""（NUL 处） */
  return 0;
}
