#include "pkg_info.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "json_util.h"
#include "nas_addr.h"
#include "path_util.h"
#include "transfer.h"
#include "websrv.h"

#define PKG_CNT_MAGIC 0x7f434e54u
#define PKG_FIH_MAGIC 0x7f464948u
#define PKG_LIH_MAGIC 0x7f4c4948u
#define PKG_HEADER_SIZE 0xa0
#define PKG_ENTRY_SIZE 0x20
#define PKG_ENTRY_PARAM_SFO 0x1000u
#define PKG_ENTRY_ICON0_PNG 0x1200u
#define PKG_ENTRY_ICON0_LOCALIZED_LAST 0x121fu
#define PKG_ENTRY_PARAM_JSON 0x2000u
#define PKG_ENTRY_FLAG_ENCRYPTED 0x80000000u
#define PKG_ENTRY_MAX 65536u
#define PKG_PARAM_MAX (4u * 1024u * 1024u)
#define PKG_ICON_MAX (32u * 1024u * 1024u)
#define SFO_MAGIC 0x46535000u
#define SFO_ENTRY_SIZE 0x10
#define SFO_ENTRY_MAX 256u
#define JSON_TOKEN_MAX 4096u

typedef enum pkg_param_type {
  PKG_PARAM_SFO,
  PKG_PARAM_JSON,
} pkg_param_type_t;

typedef struct pkg_source {
  int fd;                 /* local file, -1 when served via src */
  transfer_src_t *src;    /* NAS source (smb://... / nfs://...), NULL when local */
  uint64_t size;
  uint32_t content_type;
  uint32_t content_flags;
  char content_id[37];
  uint64_t param_offset;
  uint32_t param_size;
  pkg_param_type_t param_type;
  uint64_t icon_offset;
  uint32_t icon_size;
  /* 打开失败的原因（2026-10-28）：NAS 上的包读不出来有四种完全不同的成因
   * —— 协议没编译进来 / 地址拼不出来 / 打不开 / 读不到大小，当年全塌成一句
   * "could not read package information"，用户无从判断该改地址还是改凭据。
   * 挂在 source 上（不是全局）：一次打开一份，跟请求走。 */
  char why[160];
} pkg_source_t;

typedef enum json_token_type {
  JSON_OBJECT,
  JSON_ARRAY,
  JSON_STRING,
  JSON_PRIMITIVE,
} json_token_type_t;

typedef struct json_token {
  json_token_type_t type;
  size_t start;
  size_t end;
  int parent;
} json_token_t;

static uint16_t
read_le16(const unsigned char *p) {
  return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t
read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
read_le64(const unsigned char *p) {
  return (uint64_t)read_le32(p) | (uint64_t)read_le32(p + 4) << 32;
}

static uint32_t
read_be32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static int
range_valid(uint64_t offset, uint64_t size, uint64_t file_size) {
  return offset <= file_size && size <= file_size - offset;
}

static int
pkg_read(pkg_source_t *pkg, void *buffer, size_t size, uint64_t offset) {
  unsigned char *p = buffer;
  size_t done = 0;

  if(pkg->fd >= 0) {
    while(done < size) {
      ssize_t n = pread(pkg->fd, p + done, size - done, (off_t)(offset + done));
      if(n <= 0) {
        if(n < 0 && errno == EINTR) continue;
        return -1;
      }
      done += (size_t)n;
    }
    return 0;
  }
  /* NAS source: the transfer backends may return short reads, so loop to the
   * exact extent — SFO/entry parsing below has no tolerance for holes. */
  while(done < size) {
    size_t got = 0;
    if(transfer_read(pkg->src, offset + done, p + done, size - done, &got) != MANGO_OK ||
       !got)
      return -1;
    done += got;
  }
  return 0;
}

static int
png_signature_valid(const unsigned char *data, size_t size) {
  static const unsigned char signature[] = "\x89PNG\r\n\x1a\n";

  return size >= sizeof(signature) - 1 &&
         !memcmp(data, signature, sizeof(signature) - 1);
}

static int
pkg_icon_is_png(pkg_source_t *pkg, uint64_t offset, uint32_t size) {
  unsigned char signature[8];

  return size >= sizeof(signature) &&
         !pkg_read(pkg, signature, sizeof(signature), offset) &&
         png_signature_valid(signature, sizeof(signature));
}

static void
pkg_source_close(pkg_source_t *pkg) {
  if(pkg->fd >= 0) close(pkg->fd);
  pkg->fd = -1;
  if(pkg->src) { transfer_close(pkg->src); pkg->src = NULL; }
}

/* Compose "smb://user:pass@host:port/share/path" the same way the NAS copy
 * API does: the port goes between host and path, not in front of the whole
 * address. Returns 0 on success. */
static int
pkg_nas_compose_url(const char *scheme, const char *user, const char *pass,
                    const char *port, const char *addr, char *out, size_t out_size) {
  char host[300];
  const char *rest;
  int n;

  /* 与 list.c / mango_port.c 共用同一份切分规则（跳过前导分隔符）——
   * 2026-10-28 真机：PKG 元信息当年因为 hostlen==0 判死，表现为
   * 「could not read package information」，而同一地址读图是好的。 */
  if(nas_addr_split(addr, host, sizeof(host), &rest)) return -1;
  n = snprintf(out, out_size, "%s://%s%s%s%s%s%s%s%s",
               scheme,
               (user && *user) ? user : "",
               (user && *user) ? ":" : "",
               (user && *user) ? (pass ? pass : "") : "",
               (user && *user) ? "@" : "",
               host,
               (port && *port) ? ":" : "",
               (port && *port) ? port : "",
               rest);
  if(n < 0 || (size_t)n >= out_size) return -1;
  return 0;
}

/* Open a PKG either locally (`url` NULL) or from a NAS share. The NAS branch
 * is why this exists: the old code stopped at open(), so every SMB package
 * surfaced as "could not read package information" in the info panel. */
static int
pkg_source_open_nas(const char *path, const char *scheme, const char *user,
                    const char *pass, const char *port, pkg_source_t *pkg) {
  char url[MANGO_PATH_MAX];
  uint64_t size = 0;

  if(!scheme || !*scheme || !path || !*path) return -1;
  if(!transfer_scheme_supported(scheme)) {
    snprintf(pkg->why, sizeof(pkg->why), "协议 %s 未编译进本版", scheme);
    return -1;
  }
  if(pkg_nas_compose_url(scheme, user, pass, port, path,
                         url, sizeof(url))) {
    snprintf(pkg->why, sizeof(pkg->why), "NAS 地址拼不出来（%s）",
             path && *path ? path : "(空)");
    return -1;
  }
  pkg->src = transfer_open(scheme, url + strlen(scheme) + 3);
  if(!pkg->src) {
    snprintf(pkg->why, sizeof(pkg->why), "打不开（凭据/共享名/网络）");
    return -1;
  }
  if(transfer_size(pkg->src, &size) != MANGO_OK || size <= PKG_HEADER_SIZE) {
    transfer_close(pkg->src);
    pkg->src = NULL;
    snprintf(pkg->why, sizeof(pkg->why), "读不到文件大小或文件过小（%llu B）",
             (unsigned long long)size);
    return -1;
  }
  pkg->size = size;
  return 0;
}

/* A fully composed NAS URL — `smb://user:pass@host:port/share/x.pkg` — carries
 * everything the transfer layer needs, so it can be opened without a separate
 * scheme/credential triple. This is what makes the install worker work: it
 * only ever has the URL (that is what the queue holds), and before this it
 * fell through to open() and failed, so every PKG on a share was submitted to
 * the installer with an empty title id and an empty content id. */
static int
pkg_source_open_url(const char *url, pkg_source_t *pkg) {
  const char *sep;
  char        scheme[8];
  size_t      n;
  uint64_t    size = 0;

  sep = strstr(url, "://");
  if(!sep) return -1;
  n = (size_t)(sep - url);
  if(!n || n >= sizeof(scheme)) return -1;
  memcpy(scheme, url, n);
  scheme[n] = 0;
  if(!transfer_scheme_supported(scheme)) return -1;
  pkg->src = transfer_open(scheme, sep + 3);
  if(!pkg->src) return -1;
  if(transfer_size(pkg->src, &size) != MANGO_OK || size <= PKG_HEADER_SIZE) {
    transfer_close(pkg->src);
    pkg->src = NULL;
    return -1;
  }
  pkg->size = size;
  return 0;
}

/* `scheme` non-empty ⇒ the package lives on a NAS and `path` is the NAS-side
 * address (host/share/path) with `user`/`pass`/`port` optional. A path that is
 * ALREADY a composed `smb://`/`nfs://` URL is recognised on its own. The body of
 * the parse is identical from here on: pkg_read() abstracts the source. */
static int
pkg_source_open_ex(const char *path, const char *scheme, const char *user,
                   const char *pass, const char *port, pkg_source_t *pkg) {
  unsigned char header[PKG_HEADER_SIZE];
  unsigned char *table = NULL;
  struct stat st;
  uint64_t container_offset = 0;
  uint64_t container_size;
  uint32_t magic;
  uint32_t entry_count;
  uint32_t table_offset;
  size_t table_size;
  int ret = -1;

  memset(pkg, 0, sizeof(*pkg));
  pkg->fd = -1;
  if(scheme && *scheme) {
    if(pkg_source_open_nas(path, scheme, user, pass, port, pkg)) goto done;
  } else if(strstr(path, "://")) {
    /* 任何已注册后端的 URL（smb/nfs/webdav/webdavs…）都走 URL 分支。
     * ⚠️ 判定不能写死 smb/nfs 清单：webdav 当年不在清单里 ⇒ 掉进下面的
     * open() ⇒ ENOENT ⇒ PKG 信息面板「无法读取包信息」且装上去
     * title_id/content_id 全空（真机 mango-install.log: meta=(none)）。
     * 未注册的 scheme 由 pkg_source_open_url() 内的
     * transfer_scheme_supported() 挡掉，也不会误导成「文件不存在」。 */
    if(pkg_source_open_url(path, pkg)) goto done;
  } else if((pkg->fd = open(path, O_RDONLY)) < 0 || fstat(pkg->fd, &st) ||
     !S_ISREG(st.st_mode) || st.st_size < PKG_HEADER_SIZE ||
     pkg_read(pkg, header, sizeof(header), 0)) goto done;

  if(pkg->fd >= 0) pkg->size = (uint64_t)st.st_size;
  /* The local branch read the header inside its open condition; the NAS branch
   * opened the source only, so read it here for both. */
  if(pkg_read(pkg, header, sizeof(header), 0)) goto done;
  magic = read_be32(header);
  if(magic == PKG_FIH_MAGIC) {
    container_offset = read_le64(header + 0x58);
  } else if(magic == PKG_LIH_MAGIC) {
    container_offset = read_le64(header + 0x30);
  } else if(magic != PKG_CNT_MAGIC) {
    goto done;
  }
  if(container_offset) {
    if(!range_valid(container_offset, sizeof(header), pkg->size) ||
       pkg_read(pkg, header, sizeof(header), container_offset) ||
       read_be32(header) != PKG_CNT_MAGIC) goto done;
  }
  container_size = pkg->size - container_offset;

  memcpy(pkg->content_id, header + 0x40, 36);
  pkg->content_id[36] = 0;
  pkg->content_type = read_be32(header + 0x74);
  pkg->content_flags = read_be32(header + 0x78);
  entry_count = read_be32(header + 0x10);
  table_offset = read_be32(header + 0x18);
  if(!entry_count || entry_count > PKG_ENTRY_MAX) goto done;
  table_size = (size_t)entry_count * PKG_ENTRY_SIZE;
  if(!range_valid(table_offset, table_size, container_size) ||
     !(table = malloc(table_size)) ||
     pkg_read(pkg, table, table_size, container_offset + table_offset)) {
    goto done;
  }

  for(uint32_t i = 0; i < entry_count; i++) {
    const unsigned char *entry = table + (size_t)i * PKG_ENTRY_SIZE;
    uint32_t id = read_be32(entry);
    uint32_t flags1 = read_be32(entry + 0x08);
    uint32_t offset = read_be32(entry + 0x10);
    uint32_t size = read_be32(entry + 0x14);
    int encrypted = (flags1 & PKG_ENTRY_FLAG_ENCRYPTED) != 0;

    if(!range_valid(offset, size, container_size)) goto done;
    if(encrypted || !size) continue;
    if(id == PKG_ENTRY_PARAM_SFO && size <= PKG_PARAM_MAX) {
      pkg->param_offset = container_offset + offset;
      pkg->param_size = size;
      pkg->param_type = PKG_PARAM_SFO;
    } else if(id == PKG_ENTRY_PARAM_JSON && size <= PKG_PARAM_MAX) {
      pkg->param_offset = container_offset + offset;
      pkg->param_size = size;
      pkg->param_type = PKG_PARAM_JSON;
    } else if(id >= PKG_ENTRY_ICON0_PNG &&
              id <= PKG_ENTRY_ICON0_LOCALIZED_LAST &&
              size <= PKG_ICON_MAX &&
              (id == PKG_ENTRY_ICON0_PNG || !pkg->icon_size) &&
              pkg_icon_is_png(pkg, container_offset + offset, size)) {
      /* Prefer icon0.png; otherwise keep the first valid localized icon. */
      pkg->icon_offset = container_offset + offset;
      pkg->icon_size = size;
    }
  }
  if(!pkg->param_size) goto done;
  ret = 0;

done:
  free(table);
  if(ret) {
    errno = EINVAL;
    pkg_source_close(pkg);
  }
  return ret;
}

/* Not local-only any more: the install worker hands it the composed NAS URL
 * from the queue, and the URL branch in pkg_source_open_ex() is what lets a
 * package on a share yield its title id and content id. */
static int
pkg_source_open(const char *path, pkg_source_t *pkg) {
  return pkg_source_open_ex(path, NULL, NULL, NULL, NULL, pkg);
}

static int
append_sfo_fields(strbuf_t *json, const unsigned char *sfo, size_t size) {
  uint32_t key_offset;
  uint32_t value_offset;
  uint32_t count;
  uint64_t index_end;
  int first = 1;

  if(size < 20 || read_le32(sfo) != SFO_MAGIC) return -1;
  key_offset = read_le32(sfo + 8);
  value_offset = read_le32(sfo + 12);
  count = read_le32(sfo + 16);
  index_end = 20 + (uint64_t)count * SFO_ENTRY_SIZE;
  if(count > SFO_ENTRY_MAX || index_end > size || key_offset < index_end ||
     value_offset < key_offset || value_offset > size) return -1;

  strbuf_append(json, "[");
  for(uint32_t i = 0; i < count; i++) {
    const unsigned char *entry = sfo + 20 + (size_t)i * SFO_ENTRY_SIZE;
    uint16_t name_offset = read_le16(entry);
    uint16_t format = read_le16(entry + 2);
    uint32_t value_size = read_le32(entry + 4);
    uint32_t value_max = read_le32(entry + 8);
    uint32_t data_offset = read_le32(entry + 12);
    uint64_t key_pos = (uint64_t)key_offset + name_offset;
    uint64_t value_pos = (uint64_t)value_offset + data_offset;
    const unsigned char *key_end;
    char number[32];
    char *value = NULL;

    if(key_pos >= value_offset || value_pos > size ||
       (value_max && value_size > value_max)) continue;
    key_end = memchr(sfo + key_pos, 0, value_offset - (size_t)key_pos);
    if(!key_end) continue;
    if(format == 0x0204) {
      size_t length;
      if(!value_size || !range_valid(value_pos, value_size, size)) continue;
      length = strnlen((const char *)sfo + value_pos, value_size);
      if(!(value = malloc(length + 1))) continue;
      memcpy(value, sfo + value_pos, length);
      value[length] = 0;
    } else if(format == 0x0404 && range_valid(value_pos, 4, size)) {
      snprintf(number, sizeof(number), "%u", read_le32(sfo + value_pos));
      value = strdup(number);
    }
    if(!value) continue;
    if(!first) strbuf_append(json, ",");
    first = 0;
    strbuf_append(json, "{\"name\":");
    json_escape(json, (const char *)sfo + key_pos);
    strbuf_append(json, ",\"value\":");
    json_escape(json, value);
    strbuf_append(json, "}");
    free(value);
  }
  strbuf_append(json, "]");
  return 0;
}

static int
json_add_token(json_token_t *tokens, size_t *count, json_token_type_t type,
               size_t start, int parent) {
  if(*count >= JSON_TOKEN_MAX) return -1;
  tokens[*count] = (json_token_t){
    .type = type, .start = start, .end = 0, .parent = parent
  };
  return (int)(*count)++;
}

static int
parse_json_tokens(const unsigned char *data, size_t size, json_token_t *tokens,
                  size_t *token_count) {
  int parent = -1;
  size_t count = 0;

  for(size_t i = 0; i < size; i++) {
    unsigned char c = data[i];
    if(c == '{' || c == '[') {
      int index = json_add_token(tokens, &count,
                                 c == '{' ? JSON_OBJECT : JSON_ARRAY,
                                 i, parent);
      if(index < 0) return -1;
      parent = index;
    } else if(c == '}' || c == ']') {
      json_token_type_t type = c == '}' ? JSON_OBJECT : JSON_ARRAY;
      if(parent < 0 || tokens[parent].type != type) return -1;
      tokens[parent].end = i + 1;
      parent = tokens[parent].parent;
    } else if(c == '"') {
      int index = json_add_token(tokens, &count, JSON_STRING, i + 1, parent);
      if(index < 0) return -1;
      for(i++; i < size && data[i] != '"'; i++) {
        if(data[i] < 0x20) return -1;
        if(data[i] == '\\') {
          if(++i >= size || !strchr("\"\\/bfnrtu", data[i])) return -1;
          if(data[i] == 'u') {
            for(unsigned int n = 0; n < 4; n++) {
              if(++i >= size || !((data[i] >= '0' && data[i] <= '9') ||
                                  (data[i] >= 'a' && data[i] <= 'f') ||
                                  (data[i] >= 'A' && data[i] <= 'F'))) return -1;
            }
          }
        }
      }
      if(i >= size) return -1;
      tokens[index].end = i;
    } else if(c == ':' || c == ',' || c == ' ' || c == '\t' ||
              c == '\r' || c == '\n') {
      continue;
    } else {
      int index = json_add_token(tokens, &count, JSON_PRIMITIVE, i, parent);
      if(index < 0) return -1;
      while(i < size && data[i] != ',' && data[i] != ']' && data[i] != '}' &&
            data[i] != ' ' && data[i] != '\t' && data[i] != '\r' &&
            data[i] != '\n') i++;
      if(tokens[index].start == i) return -1;
      tokens[index].end = i;
      i--;
    }
  }
  if(parent >= 0 || !count || tokens[0].type != JSON_OBJECT ||
     !tokens[0].end) return -1;
  *token_count = count;
  return 0;
}

static int
json_token_equals(const unsigned char *data, const json_token_t *token,
                  const char *text) {
  size_t length = strlen(text);
  return token->type == JSON_STRING && token->end - token->start == length &&
         !memcmp(data + token->start, text, length);
}

static int
json_next_child(const json_token_t *tokens, size_t count, int parent,
                size_t start) {
  for(size_t i = start; i < count; i++) {
    if(tokens[i].parent == parent) return (int)i;
  }
  return -1;
}

static int
json_object_value(const unsigned char *data, const json_token_t *tokens,
                  size_t count, int object, const char *key) {
  int item = json_next_child(tokens, count, object, (size_t)object + 1);

  while(item >= 0) {
    int value = json_next_child(tokens, count, object, (size_t)item + 1);
    if(value < 0) return -1;
    if(json_token_equals(data, &tokens[item], key)) return value;
    item = json_next_child(tokens, count, object, (size_t)value + 1);
  }
  return -1;
}

static int
json_object_value_token(const unsigned char *data, const json_token_t *tokens,
                        size_t count, int object, const json_token_t *key) {
  int item = json_next_child(tokens, count, object, (size_t)object + 1);
  size_t key_size = key->end - key->start;

  while(item >= 0) {
    int value = json_next_child(tokens, count, object, (size_t)item + 1);
    if(value < 0) return -1;
    if(tokens[item].type == JSON_STRING &&
       tokens[item].end - tokens[item].start == key_size &&
       !memcmp(data + tokens[item].start, data + key->start, key_size)) {
      return value;
    }
    item = json_next_child(tokens, count, object, (size_t)value + 1);
  }
  return -1;
}

static void
append_json_token_string(strbuf_t *json, const unsigned char *data,
                         const json_token_t *token) {
  strbuf_append(json, "\"");
  strbuf_printf(json, "%.*s", (int)(token->end - token->start),
                data + token->start);
  strbuf_append(json, "\"");
}

static void
append_json_field(strbuf_t *json, const unsigned char *data,
                  const json_token_t *key, const json_token_t *value,
                  int *first) {
  if(!*first) strbuf_append(json, ",");
  *first = 0;
  strbuf_append(json, "{\"name\":");
  append_json_token_string(json, data, key);
  strbuf_append(json, ",\"value\":");
  append_json_token_string(json, data, value);
  strbuf_append(json, "}");
}

static void
append_named_json_field(strbuf_t *json, const char *name,
                        const unsigned char *data, const json_token_t *value,
                        int *first) {
  if(!*first) strbuf_append(json, ",");
  *first = 0;
  strbuf_append(json, "{\"name\":");
  json_escape(json, name);
  strbuf_append(json, ",\"value\":");
  append_json_token_string(json, data, value);
  strbuf_append(json, "}");
}

static int
append_param_json_fields(strbuf_t *json, const unsigned char *data,
                         size_t size) {
  json_token_t *tokens = calloc(JSON_TOKEN_MAX, sizeof(*tokens));
  size_t count = 0;
  int first = 1;
  int localized;
  int title = -1;

  if(!tokens || parse_json_tokens(data, size, tokens, &count)) {
    free(tokens);
    return -1;
  }
  strbuf_append(json, "[");

  localized = json_object_value(data, tokens, count, 0, "localizedParameters");
  if(localized >= 0 && tokens[localized].type == JSON_OBJECT) {
    int language = json_object_value(data, tokens, count, localized,
                                     "defaultLanguage");
    int language_data = language >= 0 && tokens[language].type == JSON_STRING ?
      json_object_value_token(data, tokens, count, localized,
                              &tokens[language]) : -1;
    if(language_data >= 0 && tokens[language_data].type == JSON_OBJECT) {
      title = json_object_value(data, tokens, count, language_data,
                                "titleName");
    }
    if(title < 0) {
      int english = json_object_value(data, tokens, count, localized, "en-US");
      if(english >= 0 && tokens[english].type == JSON_OBJECT) {
        title = json_object_value(data, tokens, count, english, "titleName");
      }
    }
    if(title < 0) {
      int item = json_next_child(tokens, count, localized,
                                 (size_t)localized + 1);
      while(item >= 0 && title < 0) {
        int value = json_next_child(tokens, count, localized,
                                    (size_t)item + 1);
        if(value < 0) break;
        if(tokens[value].type == JSON_OBJECT) {
          title = json_object_value(data, tokens, count, value, "titleName");
        }
        item = json_next_child(tokens, count, localized, (size_t)value + 1);
      }
    }
  }
  if(title >= 0 && tokens[title].type == JSON_STRING) {
    append_named_json_field(json, "titleName", data, &tokens[title], &first);
  }

  for(int item = json_next_child(tokens, count, 0, 1); item >= 0;) {
    int value = json_next_child(tokens, count, 0, (size_t)item + 1);
    if(value < 0) break;
    if(tokens[item].type == JSON_STRING &&
       (tokens[value].type == JSON_STRING ||
        tokens[value].type == JSON_PRIMITIVE)) {
      append_json_field(json, data, &tokens[item], &tokens[value], &first);
    }
    item = json_next_child(tokens, count, 0, (size_t)value + 1);
  }
  strbuf_append(json, "]");
  free(tokens);
  return 0;
}

/* ---- metadata reader for the PKG library scan ------------------------- */
static void
copy_bounded(char *dst, size_t dstsz, const unsigned char *src, size_t len) {
  if(!dst || !dstsz) return;
  if(len >= dstsz) len = dstsz - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
}

static int
sfo_lookup(const unsigned char *sfo, size_t size, const char *key,
           char *out, size_t outsz) {
  uint32_t key_offset;
  uint32_t value_offset;
  uint32_t count;
  uint64_t index_end;

  if(size < 20 || read_le32(sfo) != SFO_MAGIC) return -1;
  key_offset = read_le32(sfo + 8);
  value_offset = read_le32(sfo + 12);
  count = read_le32(sfo + 16);
  index_end = 20 + (uint64_t)count * SFO_ENTRY_SIZE;
  if(count > SFO_ENTRY_MAX || index_end > size || key_offset < index_end ||
     value_offset < key_offset || value_offset > size) return -1;

  for(uint32_t i = 0; i < count; i++) {
    const unsigned char *entry = sfo + 20 + (size_t)i * SFO_ENTRY_SIZE;
    uint16_t name_offset = read_le16(entry);
    uint16_t format = read_le16(entry + 2);
    uint32_t value_size = read_le32(entry + 4);
    uint32_t value_max = read_le32(entry + 8);
    uint32_t data_offset = read_le32(entry + 12);
    uint64_t key_pos = (uint64_t)key_offset + name_offset;
    uint64_t value_pos = (uint64_t)value_offset + data_offset;

    if(key_pos >= value_offset || value_pos > size ||
       (value_max && value_size > value_max) || format != 0x0204) continue;
    if(strcmp((const char *)sfo + key_pos, key)) continue;
    if(!value_size || !range_valid(value_pos, value_size, size)) return -1;
    copy_bounded(out, outsz, sfo + value_pos,
                 strnlen((const char *)sfo + value_pos, value_size));
    return 0;
  }
  return -1;
}

/* Recover title id / title / version (and true container size) from the PKG's
 * embedded PARAM.SFO (PS4) or param.json (PS5), plus the container's own
 * content id. Returns 0 when a title id or title was recovered, so the library
 * can label the card.
 *
 * content_id comes from the container header, not from the params — it is what
 * the system installer keys its download task on when the uri is an http URL,
 * so an install submitted with an empty one is accepted (rc=0) and then fails
 * in the background with nothing to attribute it to. */
int
pkg_info_read_meta(const char *path, char *title_id, size_t tid_sz,
                   char *title, size_t title_sz, char *version, size_t ver_sz,
                   char *content_id, size_t cid_sz,
                   unsigned long long *total_size) {
  pkg_source_t pkg = {.fd = -1};
  unsigned char *param = NULL;
  int ret = -1;

  if(title_id && tid_sz) title_id[0] = 0;
  if(title && title_sz) title[0] = 0;
  if(version && ver_sz) version[0] = 0;
  if(content_id && cid_sz) content_id[0] = 0;
  if(!path || pkg_source_open(path, &pkg)) return -1;
  if(total_size) *total_size = pkg.size;
  if(content_id && cid_sz && pkg.content_id[0])
    copy_bounded(content_id, cid_sz, (const unsigned char *)pkg.content_id,
                 strlen(pkg.content_id));
  if(!(param = malloc(pkg.param_size)) ||
     pkg_read(&pkg, param, pkg.param_size, pkg.param_offset)) goto done;

  if(pkg.param_type == PKG_PARAM_SFO) {
    sfo_lookup(param, pkg.param_size, "TITLE_ID", title_id, tid_sz);
    sfo_lookup(param, pkg.param_size, "TITLE", title, title_sz);
    sfo_lookup(param, pkg.param_size, "VERSION", version, ver_sz);
    ret = (title_id && title_id[0]) || (title && title[0]) ? 0 : -1;
  } else {
    json_token_t *tokens = calloc(JSON_TOKEN_MAX, sizeof(*tokens));
    size_t count = 0;

    if(!tokens || parse_json_tokens(param, pkg.param_size, tokens, &count)) {
      free(tokens);
      goto done;
    }
    {
      int t;
      t = json_object_value(param, tokens, count, 0, "titleId");
      if(t >= 0 && tokens[t].type == JSON_STRING)
        copy_bounded(title_id, tid_sz, param + tokens[t].start,
                     tokens[t].end - tokens[t].start);
      t = json_object_value(param, tokens, count, 0, "contentVersion");
      if(t >= 0 && tokens[t].type == JSON_STRING)
        copy_bounded(version, ver_sz, param + tokens[t].start,
                     tokens[t].end - tokens[t].start);
      t = json_object_value(param, tokens, count, 0, "localizedParameters");
      if(t >= 0 && tokens[t].type == JSON_OBJECT) {
        int lang = json_object_value(param, tokens, count, t, "defaultLanguage");
        int lang_data = (lang >= 0 && tokens[lang].type == JSON_STRING) ?
          json_object_value_token(param, tokens, count, t, &tokens[lang]) : -1;
        int title_tok = -1;
        if(lang_data >= 0 && tokens[lang_data].type == JSON_OBJECT)
          title_tok = json_object_value(param, tokens, count, lang_data, "titleName");
        if(title_tok < 0) {
          int en = json_object_value(param, tokens, count, t, "en-US");
          if(en >= 0 && tokens[en].type == JSON_OBJECT)
            title_tok = json_object_value(param, tokens, count, en, "titleName");
        }
        if(title_tok >= 0 && tokens[title_tok].type == JSON_STRING)
          copy_bounded(title, title_sz, param + tokens[title_tok].start,
                       tokens[title_tok].end - tokens[title_tok].start);
      }
    }
    free(tokens);
    ret = (title_id && title_id[0]) || (title && title[0]) ? 0 : -1;
  }

done:
  free(param);
  pkg_source_close(&pkg);
  return ret;
}

static enum MHD_Result
pkg_error(struct MHD_Connection *conn, const char *path, const char *why) {
  char msg[256];

  /* 有原因就把原因说在报错里：一句「包信息读不出来」把「地址拼错」和
   * 「密码不对」混成一谈，用户只能靠猜。 */
  if(why && *why)
    snprintf(msg, sizeof(msg), "%s：%s",
             "could not read package information", why);
  else
    snprintf(msg, sizeof(msg), "%s", "could not read package information");
  return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, msg,
                                "pkg_info_failed", path);
}

/* Open the package the info/icon API was asked about. A `scheme` in the body
 * means the package lives on a NAS share and `path` is the NAS-side address
 * (host/share/path); absent/empty means a local file. Returns 0 on success. */
static int
pkg_api_open(struct MHD_Connection *conn, const char *body, size_t body_size,
             const char *path, pkg_source_t *pkg) {
  char *scheme, *user = NULL, *pass = NULL, *port = NULL;
  int ret;

  scheme = req_param(conn, body, body_size, "scheme");
  if(!scheme || !*scheme) { free(scheme); return pkg_source_open(path, pkg); }
  user = req_param(conn, body, body_size, "user");
  pass = req_param(conn, body, body_size, "pass");
  port = req_param(conn, body, body_size, "port");
  ret = pkg_source_open_ex(path, scheme, user, pass, port, pkg);
  free(scheme); free(user); free(pass); free(port);
  return ret;
}

enum MHD_Result
api_pkg_info(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  pkg_source_t pkg = {.fd = -1};
  unsigned char *param;
  strbuf_t json = {0};

  if(!path || pkg_api_open(conn, body, body_size, path, &pkg)) {
    enum MHD_Result ret = pkg_error(conn, path, pkg.why);
    free(path);
    return ret;
  }
  if(!(param = malloc(pkg.param_size)) ||
     pkg_read(&pkg, param, pkg.param_size, pkg.param_offset)) {
    enum MHD_Result ret = pkg_error(conn, path, NULL);
    free(param);
    pkg_source_close(&pkg);
    free(path);
    return ret;
  }

  /* ★ title_id / title / version（2026-10-11 补）。
     前端的信息面板（pkgInfoSpecRows / pkgInfoSub）本来就按**这三个字段**渲染
     TITLE ID 与内容版本，而这个接口从来没返回过它们 —— 于是弹窗的「规格」页
     永远缺两行最关键的身份信息（合并到用户报的「PKG 的信息显示不全」）。
     字段名与 /api/pkg/scan 的卡片元数据保持一致：同源、同名字。 */
  {
    char tid[32] = "", ttl[256] = "", ver[48] = "";
    (void)pkg_info_read_meta(path, tid, sizeof(tid), ttl, sizeof(ttl),
                             ver, sizeof(ver), NULL, 0, NULL);

    strbuf_printf(&json,
                  "{\"ok\":true,\"size\":%llu,\"content_id\":",
                  (unsigned long long)pkg.size);
    json_escape(&json, pkg.content_id);
    strbuf_printf(&json, ",\"title_id\":");
    json_escape(&json, tid);
    strbuf_printf(&json, ",\"title\":");
    json_escape(&json, ttl);
    strbuf_printf(&json, ",\"version\":");
    json_escape(&json, ver);
  }
  strbuf_printf(&json,
                ",\"platform\":\"%s\",\"content_type\":%u,"
                "\"content_flags\":%u,\"has_icon\":%s,\"fields\":",
                pkg.param_type == PKG_PARAM_JSON ? "PS5" : "PS4",
                pkg.content_type, pkg.content_flags,
                pkg.icon_size ? "true" : "false");
  if((pkg.param_type == PKG_PARAM_JSON &&
      append_param_json_fields(&json, param, pkg.param_size)) ||
     (pkg.param_type == PKG_PARAM_SFO &&
      append_sfo_fields(&json, param, pkg.param_size))) {
    enum MHD_Result ret;
    free(json.data);
    ret = pkg_error(conn, path, NULL);
    free(param);
    pkg_source_close(&pkg);
    free(path);
    return ret;
  }
  strbuf_append(&json, "}");

  free(param);
  pkg_source_close(&pkg);
  free(path);
  return send_buffer(conn, MHD_HTTP_OK, json.data, "application/json");
}

enum MHD_Result
api_pkg_icon(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  pkg_source_t pkg = {.fd = -1};
  unsigned char *icon;
  struct MHD_Response *response;
  enum MHD_Result ret;

  if(!path || pkg_api_open(conn, body, body_size, path, &pkg) || !pkg.icon_size ||
     !(icon = malloc(pkg.icon_size)) ||
     pkg_read(&pkg, icon, pkg.icon_size, pkg.icon_offset)) {
    if(path && (pkg.fd >= 0 || pkg.src)) pkg_source_close(&pkg);
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "package icon not found");
  }
  if(!png_signature_valid(icon, pkg.icon_size)) {
    free(icon);
    pkg_source_close(&pkg);
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND,
                           "package icon not found");
  }
  pkg_source_close(&pkg);
  free(path);

  response = MHD_create_response_from_buffer(pkg.icon_size, icon,
                                              MHD_RESPMEM_MUST_FREE);
  if(!response) {
    free(icon);
    return MHD_NO;
  }
  MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "image/png");
  ret = websrv_queue_response(conn, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}
