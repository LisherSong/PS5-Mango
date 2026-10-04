/* zipx_volume -- see zipx_volume.h for what this groups and why. */

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "zipx_volume.h"

typedef struct {
  long num;
  char *path;
} vol_part_t;

/* ------------------------------------------------------------------------- */
/* path helpers                                                              */

static const char *
file_base(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *back = strrchr(path, '\\');

  if(back && (!slash || back > slash)) {
    slash = back;
  }
  return slash ? slash + 1 : path;
}

/* Directory part without a trailing separator; "" for a bare filename. */
static void
file_dir(const char *path, char *buf, size_t size) {
  const char *base = file_base(path);
  size_t len = (size_t)(base - path);

  while(len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
    len--;
  }
  if(len >= size) {
    len = size - 1;
  }
  memcpy(buf, path, len);
  buf[len] = 0;
}

static int
ends_with_ci(const char *s, const char *suffix) {
  size_t ls = strlen(s);
  size_t lf = strlen(suffix);

  if(lf > ls) {
    return 0;
  }
  return strcasecmp(s + ls - lf, suffix) == 0;
}

static int
file_exists(const char *path) {
  struct stat st;

  return stat(path, &st) == 0;
}

static char *
vol_join(const char *dir, const char *name) {
  size_t need = strlen(dir) + strlen(name) + 2;
  char *out = (char *)malloc(need);

  if(!out) {
    return NULL;
  }
  if(dir[0]) {
    snprintf(out, need, "%s/%s", dir, name);
  } else {
    snprintf(out, need, "%s", name);
  }
  return out;
}

static char *
err_printf(const char *fmt, ...) {
  va_list ap;
  char *buf;
  int need;

  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if(need < 0) {
    return NULL;
  }
  buf = (char *)malloc((size_t)need + 1);
  if(!buf) {
    return NULL;
  }
  va_start(ap, fmt);
  vsnprintf(buf, (size_t)need + 1, fmt, ap);
  va_end(ap);
  return buf;
}

/* ------------------------------------------------------------------------- */
/* part collection                                                           */

/* Matches "PREFIX<digits>SUFFIX"; returns 1 and the value on a match. */
static int
match_numbered(const char *entry, const char *prefix, const char *suffix,
               long *num) {
  size_t plen = strlen(prefix);
  size_t slen = strlen(suffix);
  const char *p;
  char *end = NULL;
  long value;

  if(strncmp(entry, prefix, plen) != 0) {
    return 0;
  }
  p = entry + plen;
  if(!isdigit((unsigned char)*p)) {
    return 0;
  }
  value = strtol(p, &end, 10);
  if(end == p) {
    return 0;
  }
  if(slen > 0) {
    if(strcmp(end, suffix) != 0) {
      return 0;
    }
  } else if(*end != 0) {
    return 0;
  }
  *num = value;
  return 1;
}

static int
part_compare(const void *a, const void *b) {
  const vol_part_t *pa = (const vol_part_t *)a;
  const vol_part_t *pb = (const vol_part_t *)b;

  if(pa->num < pb->num) {
    return -1;
  }
  if(pa->num > pb->num) {
    return 1;
  }
  return 0;
}

static void
free_parts(vol_part_t *parts, int count) {
  int i;

  for(i = 0; i < count; i++) {
    free(parts[i].path);
    parts[i].path = NULL;
  }
}

/* Collects every entry in `dir` matching PREFIX<digits>SUFFIX, ordered by the
   number. Returns the count, or -1 when the directory cannot be listed (with
   *err set) or the set is larger than `max`. */
static int
scan_parts(const char *dir, const char *prefix, const char *suffix,
           vol_part_t *parts, int max, char **err) {
  const char *target = dir[0] ? dir : ".";
  DIR *d = opendir(target);
  struct dirent *ent;
  int count = 0;

  if(!d) {
    if(err && !*err) {
      *err = err_printf("cannot list the directory '%s' that holds the other "
                        "volumes", target);
    }
    return -1;
  }
  while((ent = readdir(d)) != NULL) {
    long num = 0;

    if(!match_numbered(ent->d_name, prefix, suffix, &num)) {
      continue;
    }
    if(count >= max) {
      if(err && !*err) {
        *err = err_printf("volume set has more than %d parts, the supported "
                          "maximum", max);
      }
      free_parts(parts, count);
      closedir(d);
      return -1;
    }
    parts[count].num = num;
    parts[count].path = vol_join(dir, ent->d_name);
    if(!parts[count].path) {
      free_parts(parts, count + 1);
      closedir(d);
      return -1;
    }
    count++;
  }
  closedir(d);
  qsort(parts, (size_t)count, sizeof(*parts), part_compare);
  return count;
}

/* Verifies the parts are numbered 1..count with no gap (and no duplicate),
   filling *err with the exact missing name when they are not. */
static int
check_contiguous(vol_part_t *parts, int count, const char *dir,
                 const char *prefix, const char *suffix, int width,
                 char **err) {
  int i;

  for(i = 0; i < count; i++) {
    if(parts[i].num != (long)(i + 1)) {
      if(err && !*err) {
        char name[512];
        char *full;

        snprintf(name, sizeof(name), "%s%0*ld%s", prefix, width,
                 (long)(i + 1), suffix);
        full = vol_join(dir, name);
        *err = err_printf("volume set is incomplete: '%s' is missing",
                          full ? full : name);
        free(full);
      }
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* volume set bookkeeping                                                    */

static void
volume_reset(zipx_volume_t *vol) {
  memset(vol, 0, sizeof(*vol));
  vol->index = -1;
}

static int
volume_take(zipx_volume_t *vol, vol_part_t *parts, int count, int mode,
            const char *selected) {
  int i;

  vol->paths = (char **)calloc((size_t)count, sizeof(*vol->paths));
  if(!vol->paths) {
    return -1;
  }
  for(i = 0; i < count; i++) {
    vol->paths[i] = parts[i].path;
    parts[i].path = NULL;   /* ownership moves into vol */
  }
  vol->count = count;
  vol->mode = mode;
  vol->is_set = 1;
  vol->index = -1;
  for(i = 0; i < count; i++) {
    if(selected && strcmp(vol->paths[i], selected) == 0) {
      vol->index = i;
      break;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* detection: name.zip.001 / name.7z.001 / name.rar.001                      */

static int
detect_digit_suffix(const char *dir, const char *base, const char *selected,
                    zipx_volume_t *vol, char **err) {
  static const char *const known[] = { "zip", "7z", "rar", NULL };
  const char *dot = strrchr(base, '.');
  vol_part_t parts[ZIPX_VOL_MAX_PARTS];
  char stem[2048];
  char prefix[2100];
  size_t stem_len;
  int count;
  int i;
  int known_ext = 0;

  if(!dot || dot == base || !dot[1]) {
    return 0;
  }
  for(i = 1; dot[i]; i++) {
    if(!isdigit((unsigned char)dot[i])) {
      return 0;
    }
  }
  stem_len = (size_t)(dot - base);
  if(stem_len >= sizeof(stem)) {
    return 0;
  }
  memcpy(stem, base, stem_len);
  stem[stem_len] = 0;
  for(i = 0; known[i]; i++) {
    char tail[8];

    snprintf(tail, sizeof(tail), ".%s", known[i]);
    if(ends_with_ci(stem, tail)) {
      known_ext = 1;
      break;
    }
  }
  if(!known_ext) {
    return 0;   /* "backup.001" style names are not archive volumes */
  }
  snprintf(prefix, sizeof(prefix), "%s.", stem);
  count = scan_parts(dir, prefix, "", parts, ZIPX_VOL_MAX_PARTS, err);
  if(count < 0) {
    return -1;
  }
  if(count <= 1) {
    free_parts(parts, count > 0 ? count : 0);
    if(count == 1 && err && !*err) {
      *err = err_printf("'%s' is the first volume of a split archive but no "
                        "other volumes ('%s.002', ...) are present", base,
                        stem);
    }
    return count == 1 ? -1 : 0;
  }
  if(check_contiguous(parts, count, dir, prefix, "", 3, err)) {
    free_parts(parts, count);
    return -1;
  }
  if(volume_take(vol, parts, count, ZIPX_VOL_MODE_CONCAT, selected)) {
    free_parts(parts, count);
    return -1;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* detection: name.z01 ... name.zip                                          */

static int
detect_z_suffix(const char *dir, const char *base, const char *selected,
                zipx_volume_t *vol, char **err) {
  const char *dot = strrchr(base, '.');
  vol_part_t parts[ZIPX_VOL_MAX_PARTS];
  char stem[2048];
  char prefix[2100];
  size_t stem_len;
  int count;

  if(!dot || dot == base) {
    return 0;
  }
  if((dot[1] != 'z' && dot[1] != 'Z') || !isdigit((unsigned char)dot[2])) {
    return 0;
  }
  {
    int i;

    for(i = 2; dot[i]; i++) {
      if(!isdigit((unsigned char)dot[i])) {
        return 0;
      }
    }
  }
  stem_len = (size_t)(dot - base);
  if(stem_len >= sizeof(stem)) {
    return 0;
  }
  memcpy(stem, base, stem_len);
  stem[stem_len] = 0;

  snprintf(prefix, sizeof(prefix), "%s.z", stem);
  count = scan_parts(dir, prefix, "", parts, ZIPX_VOL_MAX_PARTS - 1, err);
  if(count < 0) {
    return -1;
  }
  if(count == 0) {
    return 0;
  }
  if(check_contiguous(parts, count, dir, prefix, "", 2, err)) {
    free_parts(parts, count);
    return -1;
  }
  /* The central directory always lives in "name.zip", the final volume. */
  {
    char name[2100];
    char *last;

    snprintf(name, sizeof(name), "%s.zip", stem);
    last = vol_join(dir, name);
    if(!last) {
      free_parts(parts, count);
      return -1;
    }
    if(!file_exists(last)) {
      if(err && !*err) {
        *err = err_printf("volume set is incomplete: the last volume '%s' that "
                          "holds the archive index is missing", name);
      }
      free(last);
      free_parts(parts, count);
      return -1;
    }
    parts[count].num = (long)count + 1;
    parts[count].path = last;
    count++;
  }
  if(volume_take(vol, parts, count, ZIPX_VOL_MODE_DISK, selected)) {
    free_parts(parts, count);
    return -1;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* detection: the final "name.zip" of a name.z01 ... name.zip set            */

static int
detect_zip_tail(const char *dir, const char *base, const char *selected,
                zipx_volume_t *vol, char **err) {
  vol_part_t parts[ZIPX_VOL_MAX_PARTS];
  char stem[2048];
  char prefix[2100];
  size_t stem_len;
  int count;

  if(!ends_with_ci(base, ".zip")) {
    return 0;
  }
  stem_len = strlen(base) - 4;
  if(stem_len == 0 || stem_len >= sizeof(stem)) {
    return 0;
  }
  memcpy(stem, base, stem_len);
  stem[stem_len] = 0;

  snprintf(prefix, sizeof(prefix), "%s.z", stem);
  count = scan_parts(dir, prefix, "", parts, ZIPX_VOL_MAX_PARTS - 1, err);
  if(count < 0) {
    return -1;
  }
  if(count == 0) {
    return 0;   /* an ordinary single volume archive */
  }
  if(check_contiguous(parts, count, dir, prefix, "", 2, err)) {
    free_parts(parts, count);
    return -1;
  }
  {
    char *last = vol_join(dir, base);

    if(!last) {
      free_parts(parts, count);
      return -1;
    }
    parts[count].num = (long)count + 1;
    parts[count].path = last;
    count++;
  }
  if(volume_take(vol, parts, count, ZIPX_VOL_MODE_DISK, selected)) {
    free_parts(parts, count);
    return -1;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* detection: name.part1.zip ...                                             */

static int
detect_part_suffix(const char *dir, const char *base, const char *selected,
                   zipx_volume_t *vol, char **err) {
  vol_part_t parts[ZIPX_VOL_MAX_PARTS];
  char stem[2048];
  char prefix[2100];
  char trimmed[2048];
  const char *dot;
  size_t len;
  int count;

  if(!ends_with_ci(base, ".zip")) {
    return 0;
  }
  len = strlen(base) - 4;
  if(len == 0 || len >= sizeof(trimmed)) {
    return 0;
  }
  memcpy(trimmed, base, len);
  trimmed[len] = 0;
  dot = strrchr(trimmed, '.');
  if(!dot || dot == trimmed || strncasecmp(dot, ".part", 5) != 0 ||
     !isdigit((unsigned char)dot[5])) {
    return 0;
  }
  {
    size_t stem_len = (size_t)(dot - trimmed);

    if(stem_len >= sizeof(stem)) {
      return 0;
    }
    memcpy(stem, trimmed, stem_len);
    stem[stem_len] = 0;
  }
  snprintf(prefix, sizeof(prefix), "%s.part", stem);
  count = scan_parts(dir, prefix, ".zip", parts, ZIPX_VOL_MAX_PARTS, err);
  if(count < 0) {
    return -1;
  }
  if(count <= 1) {
    free_parts(parts, count > 0 ? count : 0);
    if(count == 1 && err && !*err) {
      *err = err_printf("'%s' is a volume of a split archive but the other "
                        "volumes ('%s.part1.zip', ...) are missing", base, stem);
    }
    return count == 1 ? -1 : 0;
  }
  if(check_contiguous(parts, count, dir, prefix, ".zip", 1, err)) {
    free_parts(parts, count);
    return -1;
  }
  if(volume_take(vol, parts, count, ZIPX_VOL_MODE_DISK, selected)) {
    free_parts(parts, count);
    return -1;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */

int
zipx_volume_detect(const char *path, zipx_volume_t *out, char **err) {
  char dir[4096];
  const char *base;
  int rc;

  if(err) {
    *err = NULL;
  }
  if(!path || !out) {
    return -1;
  }
  volume_reset(out);
  base = file_base(path);
  file_dir(path, dir, sizeof(dir));

  rc = detect_digit_suffix(dir, base, path, out, err);
  if(rc != 0) {
    return rc;
  }
  rc = detect_z_suffix(dir, base, path, out, err);
  if(rc != 0) {
    return rc;
  }
  rc = detect_zip_tail(dir, base, path, out, err);
  if(rc != 0) {
    return rc;
  }
  return detect_part_suffix(dir, base, path, out, err);
}

void
zipx_volume_free(zipx_volume_t *vol) {
  int i;

  if(!vol) {
    return;
  }
  if(vol->paths) {
    for(i = 0; i < vol->count; i++) {
      free(vol->paths[i]);
    }
    free(vol->paths);
  }
  memset(vol, 0, sizeof(*vol));
  vol->index = -1;
}

int
zipx_volume_is_first(const char *path) {
  const char *base = file_base(path);
  const char *dot = strrchr(base, '.');

  if(!dot) {
    return 0;
  }
  if(strcmp(dot, ".001") == 0) {
    return 1;
  }
  if((dot[1] == 'z' || dot[1] == 'Z') && dot[2] == '0' && dot[3] == '1' &&
     dot[4] == 0) {
    return 1;
  }
  if(ends_with_ci(base, ".part1.zip")) {
    return 1;
  }
  return 0;
}
