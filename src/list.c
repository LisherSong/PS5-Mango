#include "filemgr_internal.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json_util.h"
#include "path_util.h"
#include "transfer.h"

/* ---------------------------------------------------------------------------
 * NAS address assembly.
 *
 * The front-end stores a bookmark as {addr, scheme, port, user, pass} and sends
 * scheme/path plus (for non-local only) user/pass/port on every /api/fs/list.
 * transfer_url_parse() understands ONE url, so splice the fields back together
 * here: "user:pass@host:port/share/sub". Doing it per request (instead of
 * persisting the composed string) is deliberate — the credentials then never
 * end up in the browser's path bar / crumbs, which is what the user sees and
 * could copy around.
 * ------------------------------------------------------------------------- */
static void
nas_build_url(const char *path, const char *user, const char *pass,
              const char *port, char *out, size_t out_size) {
  char host[300];
  const char *rest;
  const char *slash;
  size_t hl, cut;

  /* Split "host[:port]/share/sub" at the first separator — everything before it
     is the address part, everything from it on is the share-relative remainder.
     🪤 分隔符有两种：用户从资源管理器「复制路径」拿到的是
     \\192.168.1.3\PS5_Games，手打的常见形状是 192.168.1.3\PS5_Games。
     原来只认 '/'，于是整串（含反斜杠）落进 host、share 为空 —— 后面
     transfer_url_parse 把它整个交给 getaddrinfo，报的就是
     "Invalid address:… Can not resolve into IPv4/v6"。
     ⚠️ 同时跳过**前导**分隔符：UNC 形状 \\host\share 的第一个字符就是分隔符，
     不跳的话 host 会被切成长度 0，变成一句「地址无法解析」。 */
  while(*path == '/' || *path == '\\') path++;
  cut = strcspn(path, "/\\");
  slash = path + cut;
  hl = cut;
  if(hl >= sizeof(host)) hl = sizeof(host) - 1;
  memcpy(host, path, hl);
  host[hl] = 0;
  rest = *slash ? slash : "";

  if(user && *user) {
    if(port && *port)
      snprintf(out, out_size, "%s:%s@%s:%s%s", user, pass ? pass : "", host, port, rest);
    else
      snprintf(out, out_size, "%s:%s@%s%s", user, pass ? pass : "", host, rest);
  } else {
    if(port && *port)
      snprintf(out, out_size, "%s:%s%s", host, port, rest);
    else
      snprintf(out, out_size, "%s%s", host, rest);
  }
}

/* List a NAS directory through the transfer layer. Errors are forwarded
 * verbatim (auth failure / mount refused / no route) instead of a bare
 * "not found", because a NAS that refuses is a different problem from a path
 * that does not exist and the UI shows this string to the user. */
static enum MHD_Result
nas_list(struct MHD_Connection *conn, const char *scheme, const char *path,
         const char *user, const char *pass, const char *port) {
  transfer_src_t *s;
  transfer_entry_t *list = NULL;
  int count = 0;
  int first = 1;
  strbuf_t b = {0};
  char url[NEXUS_PATH_MAX + 600];
  char why[384];

  if(!path || !*path) {
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "NAS 地址为空", "nas_bad_address", NULL);
  }

  /* 只给了主机、没有共享名的 SMB 地址：libsmb2 会转去连 IPC$，而相当一批
     NAS/Samba 对「IPC$ 上列根目录」的回应是直接断开 TCP —— 传出去的错误码是
     CONNECTION_REFUSED (0xc0000236)，提示语却叫用户去查 SMB 服务和网络，
     把人带进沟里（真机实测 2026-10-02：AppleNas/Samba TRIM 即此行为，
     PC 侧 vendored libsmb2 复现一致）。共享名缺失应在入口就拦下。 */
  if(scheme && !strcmp(scheme, "smb")) {
    const char *pp = path, *sep;
    while(*pp == '/' || *pp == '\\') pp++;
    sep = pp + strcspn(pp, "/\\");
    while(*sep == '/' || *sep == '\\') sep++;
    if(!*sep) {
      /* ⚠️ 这句话以前写作「需包含共享名」，用户读成「必须指定一个文件夹」，于是问
         「我的 SMB 共享本来就是根目录怎么办」。要讲清的是**共享名和文件夹不是一回事**：
         共享名必须给（SMB 的树连接就按共享名建立，协议层没有「不指定共享」的连接），
         而共享名**后面**的文件夹可以完全不写 —— 那时进去的就是该共享的根目录。
         至于「只填主机、列出这台 NAS 有哪些共享」：libsmb2 只能走 IPC$ 去问，
         而实测相当一批 NAS/Samba 对「IPC$ 上列根目录」是直接断连（见上），
         所以宁可在这里把话说透，也不要让用户撞上一个与原因毫不相干的网络错误。
         ⚠️ error_code 与「共享名」这个措辞是 hosttest 钉住的，不要改。 */
      return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
        "NAS 地址必须带共享名（SMB 按共享名建立连接），如 192.168.1.3/PS5_Games。"
        "共享名后面的文件夹可以不写：只写 主机/共享名 进去的就是该共享的根目录",
        "nas_bad_address", NULL);
    }
  }

  nas_build_url(path, user, pass, port, url, sizeof(url));
  s = transfer_open(scheme, url);
  if(!s) {
    /* Two distinct causes, told apart so the user knows what to change. */
    return send_json_error_detail(
      conn, MHD_HTTP_BAD_GATEWAY,
      transfer_scheme_supported(scheme)
        ? "NAS 地址无法解析：请写成 主机/共享（如 192.168.1.3/media）；端口只能写数字"
        : "NAS backend not compiled in (smb/nfs)",
      "nas_bad_address", NULL);
  }

  if(transfer_list(s, path, &list, &count) != NEXUS_OK) {
    const char *be = transfer_last_error(s);
    /* 0xc0000236 (CONNECTION_REFUSED) = 主机层就没有应答/拒绝：跟共享名、
       账号密码都无关，先查 SMB 服务和网络可达性 —— 不给提示用户只会
       反复检查密码。 */
    if(be && strstr(be, "CONNECTION_REFUSED"))
      snprintf(why, sizeof(why), "NAS 连接失败：%s"
               "（主机没有应答：请确认 NAS 的 SMB 服务已开启、"
               "地址和端口正确，且 PS5 与 NAS 在同一网络）", be);
    else if(be && strstr(be, "BAD_NETWORK_NAME"))
      /* 0xc00000cc：Samba 对「匿名/无权会话 + 受限共享」也回这个码
         （故意不区分共享不存在与无权访问，防信息泄露），所以两条提示都要给。 */
      snprintf(why, sizeof(why), "NAS 连接失败：%s"
               "（共享名不存在，或该账号无权访问此共享 —— "
               "核对共享名拼写；需要账号时在添加 NAS 里填用户名/密码）", be);
    else
      snprintf(why, sizeof(why), "NAS 连接失败：%s", be ? be : "未知原因");
    transfer_close(s);
    return send_json_error_detail(conn, MHD_HTTP_BAD_GATEWAY, why,
                                  "nas_list_failed", NULL);
  }

  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_escape(&b, path);
  strbuf_append(&b, ",\"entries\":[");
  for(int i = 0; i < count; i++) {
    if(!first) strbuf_append(&b, ",");
    first = 0;
    strbuf_append(&b, "{\"name\":");
    json_escape(&b, list[i].name);
    strbuf_printf(&b, ",\"is_dir\":%s,\"size\":%llu,\"mode\":%u,\"mtime\":%llu}",
                  list[i].is_dir ? "true" : "false",
                  (unsigned long long)list[i].size,
                  (unsigned)list[i].mode,
                  (unsigned long long)list[i].mtime);
  }
  strbuf_append(&b, "]}");

  free(list);
  transfer_close(s);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_list(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *scheme = req_param(conn, body, body_size, "scheme");
  char *path = req_param(conn, body, body_size, "path");
  char *user = NULL, *pass = NULL, *port = NULL;
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  strbuf_t b = {0};
  int first = 1;

  /* Non-local scheme => NAS backend. The merge's local lister below is the
     tested owendswang path and stays untouched for scheme=local. */
  if(scheme && *scheme && strcmp(scheme, "local")) {
    enum MHD_Result r;
    user = req_param(conn, body, body_size, "user");
    pass = req_param(conn, body, body_size, "pass");
    port = req_param(conn, body, body_size, "port");
    r = nas_list(conn, scheme, path, user, pass, port);
    free(scheme); free(path); free(user); free(pass); free(port);
    return r;
  }
  free(scheme);

  if(!path) {
    path = strdup("/");
  }
  /* A device root that is not actually mounted must not look listable. The
     front-end reveals its shortcut buttons straight off `ok` (probeRoots in
     assets/index.html), so a successful listing of the pre-created, empty
     /mnt/usb0 advertised a USB drive the user had not plugged in. Only
     "<prefix>/<name>" is judged; anything deeper (/mnt/ext1/games/...) is an
     ordinary directory inside a device and is unaffected. */
  if(path && !fs_device_root_mounted(path)) {
    enum MHD_Result not_mounted = send_json_error_detail(
      conn, MHD_HTTP_NOT_FOUND, "设备未挂载 / device not mounted",
      "device_not_mounted", path);
    free(path);
    return not_mounted;
  }
  if(!(dir = opendir(path))) {
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, NULL);
  }

  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_escape(&b, path);
  strbuf_append(&b, ",\"entries\":[");

  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       lstat(child, &st)) {
      continue;
    }

    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_append(&b, "{\"name\":");
    json_escape(&b, entry->d_name);
    strbuf_printf(&b, ",\"is_dir\":%s,\"size\":%lld,\"mode\":%u,\"mtime\":%lld}",
                  S_ISDIR(st.st_mode) ? "true" : "false",
                  (long long)st.st_size,
                  (unsigned int)(st.st_mode & 07777),
                  (long long)st.st_mtime);
  }

  closedir(dir);
  free(path);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}
