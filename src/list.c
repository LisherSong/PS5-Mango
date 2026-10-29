#include "filemgr_internal.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json_util.h"
#include "nas_addr.h"
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
/* v1.0.8：不再 static —— /api/fs/read 也要拼同一个 URL（NAS 上的图片预览当年
 * 走的是本地 fopen，把 host/share/path 当成本地路径，于是「列得出来、点不开」）。
 * 拆 URL 的活只有这一处，别在 text.c 里另写一份。 */
void
nas_build_url(const char *path, const char *user, const char *pass,
              const char *port, const char *scheme, char *out, size_t out_size) {
  char host[300];
  const char *rest;

  /* 切分规则只有 nas_addr_split 一份（跳过前导分隔符、认 '/' 与 '\\'）。
   * 2026-10-28 之前这里和 PKG 那两份各写一遍 strcspn，且只有这份跳前导
   * 分隔符 ⇒ 同一个 `/192.168.1.3/dav/x.pkg` 列目录过、PKG 判死。 */
  out[0] = 0;
  if(nas_addr_split(path, host, sizeof(host), &rest)) return;

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

  /* webdavs：把 scheme 前缀补进 URL —— transfer_url_parse 靠 "webdavs://"
   * 前缀置 nas.tls=1（默认端口 443、选 TLS 传输）。其余 scheme 不需要前缀，
   * 不带前缀的解析结果与历史行为完全一致。 */
  if(scheme && !strcmp(scheme, "webdavs")) {
    size_t ol = strlen(out);
    if(ol + 10 < out_size) {
      memmove(out + 10, out, ol + 1);
      memcpy(out, "webdavs://", 10);
    }
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
  char url[MANGO_PATH_MAX + 600];
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
    char host[300];
    const char *rest, *sep;
    /* 切分同样走共享的 nas_addr_split：这里要的是「主机之后还有没有共享名」，
     * 跟拼接用的是同一条「主机在哪结束」的规则（UNC 前导分隔符一并跳过）。 */
    if(nas_addr_split(path, host, sizeof(host), &rest)) {
      return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                    "NAS 地址为空", "nas_bad_address", NULL);
    }
    sep = rest;
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
        "NAS 地址必须带共享名（SMB 按共享名建立连接），如 192.168.1.100/PS5_Games。"
        "共享名后面的文件夹可以不写：只写 主机/共享名 进去的就是该共享的根目录",
        "nas_bad_address", NULL);
    }
  }

  nas_build_url(path, user, pass, port, scheme, url, sizeof(url));
  s = transfer_open(scheme, url);
  if(!s) {
    /* Two distinct causes, told apart so the user knows what to change. */
    return send_json_error_detail(
      conn, MHD_HTTP_BAD_GATEWAY,
      transfer_scheme_supported(scheme)
        ? "NAS 地址无法解析：请写成 主机/共享（如 192.168.1.100/media）；端口只能写数字"
        : "NAS backend not compiled in (smb/nfs/webdav)",
      "nas_bad_address", NULL);
  }

  if(transfer_list(s, path, &list, &count) != MANGO_OK) {
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

/* Recursive byte total for a directory, bounded so that listing a handful of
 * deep folders cannot wedge the request on the console. Files count fully;
 * directories are walked up to DIRSZ_MAX_ENTRIES entries / DIRSZ_MAX_DEPTH
 * levels and then contribution is cut off (reported total is then an upper-ish
 * floor, not exact). Symbolic links and non-regular files are skipped. */
#define DIRSZ_MAX_ENTRIES 8192
#define DIRSZ_MAX_DEPTH   8
static long long
dir_total_bytes(const char *path, int depth, long long *entries) {
  DIR *d;
  struct dirent *e;
  struct stat st;
  long long total = 0;

  if(depth > DIRSZ_MAX_DEPTH || !entries || *entries >= DIRSZ_MAX_ENTRIES) {
    return 0;
  }
  if(!(d = opendir(path))) {
    return 0;
  }
  while((e = readdir(d))) {
    char child[PATH_MAX];

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(++(*entries) >= DIRSZ_MAX_ENTRIES) break;
    if(path_join(child, sizeof(child), path, e->d_name)) continue;
    if(lstat(child, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      total += dir_total_bytes(child, depth + 1, entries);
    } else if(S_ISREG(st.st_mode)) {
      total += (long long)st.st_size;
    }
  }
  closedir(d);
  return total;
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
    /* Directories report their recursive byte total (bounded) so the UI can
       show how big a folder really is, not the inode's stub size. */
    long long esize = S_ISDIR(st.st_mode)
      ? dir_total_bytes(child, 1, &(long long){0})
      : (long long)st.st_size;
    strbuf_append(&b, "{\"name\":");
    json_escape(&b, entry->d_name);
    strbuf_printf(&b, ",\"is_dir\":%s,\"size\":%lld,\"mode\":%u,\"mtime\":%lld}",
                  S_ISDIR(st.st_mode) ? "true" : "false",
                  esize,
                  (unsigned int)(st.st_mode & 07777),
                  (long long)st.st_mtime);
  }

  closedir(dir);
  free(path);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}
