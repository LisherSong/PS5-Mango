#include "archive_extract.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "path_util.h"

static const char *const g_archive_suffixes[] = {
  ".7z", ".001", ".zip", ".zipx", ".rar", ".arj", ".bz2",
  ".bzip2", ".tbz", ".tbz2", ".cab", ".gz", ".gzip", ".tgz",
  ".tpz", ".lzh", ".lha", ".tar", ".xz", ".txz", ".z", ".taz",
  ".zst", ".tzst", ".xar", ".xip", ".cpio", ".lzma", ".pmd"
};

static int
suffix_equal(const char *text, const char *suffix) {
  size_t text_len = strlen(text);
  size_t suffix_len = strlen(suffix);

  return text_len >= suffix_len &&
         !strcasecmp(text + text_len - suffix_len, suffix);
}

int
archive_path_supported(const char *path) {
  const char *dot;
  const char *part;
  size_t i;

  if(!path || !(dot = strrchr(path, '.')) || !dot[1]) return 0;
  for(part = path; (part = strchr(part, '.')); part++) {
    if(!strncasecmp(part, ".part", 5)) {
      const char *number = part + 5;
      const char *end = number;
      unsigned long value = 0;
      while(isdigit((unsigned char)*end)) {
        if(value <= 1) value = value * 10 + (unsigned int)(*end - '0');
        end++;
      }
      if(end > number && !strcasecmp(end, ".rar")) return value == 1;
    }
  }
  for(i = 0; i < sizeof(g_archive_suffixes) /
                 sizeof(g_archive_suffixes[0]); i++) {
    if(!strcasecmp(dot, g_archive_suffixes[i])) return 1;
  }
  return 0;
}

/* ---- multi-part RAR ----
 * 老式分卷：第一卷 xxx.rar，后续卷 xxx.r00 … xxx.r99。
 * 新式分卷：第一卷 xxx.part1.rar，后续卷 xxx.partN.rar（N > 1）。
 * 用户在文件列表里常常把整个分卷集一起选中（或只点到后续卷）——此前
 * 后续卷不在后缀表里，直接 400 "unsupported archive type"，报错完全没
 * 告诉用户「这是分卷、要选第一卷」。这三个入口把任意成员归一成它的
 * 第一卷，归一后去重；第一卷不存在时保留原路径，由具体的错误码
 * （archive_first_part_missing）告诉用户缺了什么。 */

int
archive_multipart_member(const char *path) {
  const char *dot;

  if(!path || !(dot = strrchr(path, '.')) || !dot[1]) return 0;
  /* xxx.rNN（r00 – r99）：定长 4 字符的后缀 */
  if(strlen(dot) == 4 &&
     (dot[1] == 'r' || dot[1] == 'R') &&
     isdigit((unsigned char)dot[2]) && isdigit((unsigned char)dot[3]))
    return 1;
  if(!strcasecmp(dot, ".rar")) {
    /* 向前吞掉数字段，看是否紧贴 ".part" —— xxx.partNN.rar */
    const char *p = dot;
    while(p > path && isdigit((unsigned char)p[-1])) p--;
    if(p != dot && p - path >= 5 && !strncasecmp(p - 5, ".part", 5)) {
      unsigned long value = 0;
      const char *q;
      for(q = p; q < dot; q++) {
        if(value > 1000000) return 1;    /* 数字段异常长：当后续卷处理 */
        value = value * 10 + (unsigned long)(*q - '0');
      }
      return value > 1;                  /* part1 本来就是第一卷 */
    }
  }
  return 0;
}

char *
archive_multipart_first(const char *path) {
  const char *dot;
  size_t stem;
  char *out;

  if(!path || !(dot = strrchr(path, '.'))) return NULL;
  /* xxx.rNN → 同词干的 .rar（哪怕卷号是 00：.r00 的第一卷也是 .rar） */
  if(strlen(dot) == 4 &&
     (dot[1] == 'r' || dot[1] == 'R') &&
     isdigit((unsigned char)dot[2]) && isdigit((unsigned char)dot[3])) {
    stem = (size_t)(dot - path);
    if(!(out = malloc(stem + 5))) return NULL;
    memcpy(out, path, stem);
    memcpy(out + stem, ".rar", 5);
    return out;
  }
  if(!strcasecmp(dot, ".rar")) {
    const char *p = dot;
    while(p > path && isdigit((unsigned char)p[-1])) p--;
    if(p != dot && p - path >= 5 && !strncasecmp(p - 5, ".part", 5)) {
      /* 把 N 归一成 1：只重写数字段，".part" 前缀原样保留。
         ★ 必须保留原来的补零宽度：xxx.part02.rar 的第一卷是
         xxx.part01.rar，写成 xxx.part1.rar 会 lstat 失败，用户看到
         「找不到第一卷」——而其实它就在旁边。 */
      size_t prefix = (size_t)(p - path);
      size_t width = (size_t)(dot - p);
      size_t i;
      char *num;

      if(!(out = malloc(prefix + width + 5))) return NULL;  /* + ".rar" + NUL */
      memcpy(out, path, prefix);
      num = out + prefix;
      for(i = 0; i + 1 < width; i++) num[i] = '0';
      num[width - 1] = '1';
      memcpy(out + prefix + width, ".rar", 5);
      return out;
    }
  }
  return NULL;
}

int
archive_normalize_sources(char **paths, size_t *count) {
  size_t i, j;

  if(!paths || !count) {
    errno = EINVAL;
    return -1;
  }
  for(i = 0; i < *count; i++) {
    struct stat st;
    char *first;
    if(!archive_multipart_member(paths[i])) continue;
    if(!(first = archive_multipart_first(paths[i]))) {
      errno = ENOMEM;
      return -1;
    }
    /* 第一卷不存在 ⇒ 保留原路径，让后面的校验给出针对性的错误 */
    if(lstat(first, &st) || !S_ISREG(st.st_mode)) {
      free(first);
      continue;
    }
    free(paths[i]);
    paths[i] = first;
  }
  /* 归一后去重：全选分卷集时所有成员都指向了同一个第一卷。
     数组由调用方按原始 count 分配，压缩后尾部空位无害。 */
  for(i = 0, j = 0; i < *count; i++) {
    size_t k;
    int dup = 0;
    for(k = 0; k < j; k++) {
      if(!strcmp(paths[k], paths[i])) {
        dup = 1;
        break;
      }
    }
    if(dup) free(paths[i]);
    else paths[j++] = paths[i];
  }
  *count = j;
  return 0;
}

static void
remove_suffix(char *name, const char *suffix) {
  if(suffix_equal(name, suffix)) name[strlen(name) - strlen(suffix)] = 0;
}

static void
archive_folder_name(const char *path, char *name, size_t size) {
  static const char *const compound[] = {
    ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tbz", ".tbz2",
    ".tgz", ".tpz", ".txz", ".taz", ".tzst"
  };
  char *part;
  size_t i;

  path_basename_copy(path, name, size);
  part = name;
  while((part = strchr(part, '.'))) {
    if(!strncasecmp(part, ".part", 5)) {
      char *p = part + 5;
      while(isdigit((unsigned char)*p)) p++;
      if(p > part + 5 && !strcasecmp(p, ".rar")) {
        *part = 0;
        break;
      }
    }
    part++;
  }
  remove_suffix(name, ".001");
  for(i = 0; i < sizeof(compound) / sizeof(compound[0]); i++) {
    if(suffix_equal(name, compound[i])) {
      remove_suffix(name, compound[i]);
      return;
    }
  }
  for(i = 0; i < sizeof(g_archive_suffixes) /
                 sizeof(g_archive_suffixes[0]); i++) {
    if(suffix_equal(name, g_archive_suffixes[i])) {
      remove_suffix(name, g_archive_suffixes[i]);
      return;
    }
  }
  {
    size_t len = strlen(name);
    if(len >= 4 && name[len - 4] == '.' &&
       (name[len - 3] == 'r' || name[len - 3] == 'R' ||
        name[len - 3] == 'z' || name[len - 3] == 'Z') &&
       isdigit((unsigned char)name[len - 2]) &&
       isdigit((unsigned char)name[len - 1])) name[len - 4] = 0;
  }
  if(!name[0]) path_basename_copy(path, name, size);
}

int
archive_output_path(const char *archive, const char *base, int separate,
                    char *out, size_t out_size) {
  char name[PATH_MAX];

  if(!archive || !base || base[0] != '/') {
    errno = EINVAL;
    return -1;
  }
  if(!separate) {
    if(strlen(base) >= out_size) {
      errno = ENAMETOOLONG;
      return -1;
    }
    strcpy(out, base);
    return 0;
  }
  archive_folder_name(archive, name, sizeof(name));
  return path_join(out, out_size, base, name);
}
