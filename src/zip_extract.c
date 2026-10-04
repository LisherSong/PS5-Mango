/* Safe ZIP extraction engine used by the /api/extract task.
   POSIX only; see tests/ for the host harness that exercises it. */

#include "zip_extract.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "mz.h"
#include "mz_zip.h"
#include "mz_strm.h"
#include "mz_strm_os.h"

#include "zipx_volume.h"
#include "zipx_volstream.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define ZIPX_IO_BUFFER (128 * 1024)
#define ZIPX_TMP_ATTEMPTS 64
#define ZIPX_STAGING_PREFIX ".wfm-extract-"
#define ZIPX_PART_PREFIX ".wfm-part-"
#define ZIPX_PUBLISH_MAX_DEPTH 128
#define ZIPX_SPACE_SLACK_PER_ENTRY 512

/* The limit profiles and zipx_status_string() are format independent and live
   in src/zipx_common.c, which every engine links. */

/**************************************************************************
 * small helpers
 **************************************************************************/

typedef struct {
  zipx_conflict_t conflict;
  zipx_limits_t limits;
  zipx_cancel_fn cancel;
  zipx_progress_fn progress;
  void *userdata;
  /* NULL or empty means "no password supplied". Both ZIP encryption schemes
     (traditional PKWARE and WinZip AES) only need it for the entry data: the
     central directory is never encrypted, so the scan phase can read every
     header without it. */
  const char *password;
  zipx_result_t *result;
  uint64_t entries_total;
  uint64_t entries_done;
  uint64_t bytes_total;
  uint64_t bytes_done;
  uint64_t files_created;
  uint64_t dirs_created;
  uint64_t progress_floor;
  struct timespec last_report;
  char staging[ZIPX_PATH_MAX];
  char **created;        /* published destination paths, oldest first */
  size_t created_count;
  size_t created_cap;
  unsigned char *created_is_dir;
  int staging_created;
  unsigned int part_counter;
} zipx_ctx_t;

static void
ctx_set_detail(zipx_ctx_t *c, const char *path) {
  if(path) {
    snprintf(c->result->detail, sizeof(c->result->detail), "%s", path);
  }
}

static int
ctx_fail(zipx_ctx_t *c, zipx_status_t status, const char *detail,
         const char *fmt, ...) {
  va_list ap;

  c->result->status = status;
  c->result->sys_errno = errno;
  ctx_set_detail(c, detail);
  va_start(ap, fmt);
  if(fmt) {
    vsnprintf(c->result->message, sizeof(c->result->message), fmt, ap);
  }
  va_end(ap);
  if(!c->result->message[0]) {
    snprintf(c->result->message, sizeof(c->result->message), "%s",
             zipx_status_string(status));
  }
  /* Every error code is non-zero, so callers can test the return value. */
  return (int)status;
}

static long long
mono_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
report(zipx_ctx_t *c, int phase, const char *current, int force) {
  zipx_progress_t p;
  long long now;

  if(!c->progress) {
    return;
  }
  now = mono_ms();
  if(!force && c->last_report.tv_sec &&
     now - ((long long)c->last_report.tv_sec * 1000 +
            c->last_report.tv_nsec / 1000000) < 200 &&
     c->bytes_done - c->progress_floor < (1024 * 1024)) {
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &c->last_report);
  c->progress_floor = c->bytes_done;

  p.phase = phase;
  p.entries_total = c->entries_total;
  p.entries_done = c->entries_done;
  p.bytes_total = c->bytes_total;
  p.bytes_done = c->bytes_done;
  p.current = current;
  c->progress(c->userdata, &p);
}

static int
canceled(zipx_ctx_t *c) {
  return c->cancel && c->cancel(c->userdata);
}

static int
remember_published(zipx_ctx_t *c, const char *path, int is_dir) {
  if(c->created_count == c->created_cap) {
    size_t cap = c->created_cap ? c->created_cap * 2 : 64;
    char **paths = realloc(c->created, cap * sizeof(*paths));
    unsigned char *flags = realloc(c->created_is_dir, cap * sizeof(*flags));

    if(!paths || !flags) {
      free(paths);
      free(flags);
      return -1;
    }
    c->created = paths;
    c->created_is_dir = flags;
    c->created_cap = cap;
  }
  if(!(c->created[c->created_count] = strdup(path))) {
    return -1;
  }
  c->created_is_dir[c->created_count] = is_dir ? 1 : 0;
  c->created_count++;
  return 0;
}

static void
free_published(zipx_ctx_t *c) {
  size_t i;

  for(i = 0; i < c->created_count; i++) {
    free(c->created[i]);
  }
  free(c->created);
  free(c->created_is_dir);
  c->created = NULL;
  c->created_is_dir = NULL;
  c->created_count = c->created_cap = 0;
}

/**************************************************************************
 * duplicate / file-vs-directory clash detection
 **************************************************************************/

typedef struct {
  uint64_t *hash;
  unsigned char *is_dir;
  size_t cap;   /* power of two */
  size_t count;
} zipx_nameset_t;

static uint64_t
fnv1a(const char *s, size_t len) {
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for(i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h ? h : 1; /* 0 is the empty slot marker */
}

static int
nameset_init(zipx_nameset_t *set, size_t hint) {
  size_t cap = 1024;

  while(cap < hint * 2) {
    cap *= 2;
  }
  set->hash = calloc(cap, sizeof(*set->hash));
  set->is_dir = calloc(cap, sizeof(*set->is_dir));
  if(!set->hash || !set->is_dir) {
    free(set->hash);
    free(set->is_dir);
    set->hash = NULL;
    set->is_dir = NULL;
    return -1;
  }
  set->cap = cap;
  set->count = 0;
  return 0;
}

static void
nameset_free(zipx_nameset_t *set) {
  free(set->hash);
  free(set->is_dir);
  set->hash = NULL;
  set->is_dir = NULL;
  set->cap = set->count = 0;
}

static size_t
nameset_slot(zipx_nameset_t *set, uint64_t h) {
  size_t mask = set->cap - 1;
  size_t i = (size_t)(h & mask);

  while(set->hash[i] && set->hash[i] != h) {
    i = (i + 1) & mask;
  }
  return i;
}

static int
nameset_grow(zipx_nameset_t *set) {
  zipx_nameset_t bigger;
  size_t i;

  if(nameset_init(&bigger, set->cap) ) {
    return -1;
  }
  for(i = 0; i < set->cap; i++) {
    if(!set->hash[i]) {
      continue;
    }
    bigger.hash[nameset_slot(&bigger, set->hash[i])] = set->hash[i];
    bigger.is_dir[nameset_slot(&bigger, set->hash[i])] = set->is_dir[i];
    bigger.count++;
  }
  nameset_free(set);
  *set = bigger;
  return 0;
}

/* 0 = not present, 1 = present as directory, 2 = present as file */
static int
nameset_get(zipx_nameset_t *set, uint64_t h) {
  size_t i = nameset_slot(set, h);

  if(!set->hash[i]) {
    return 0;
  }
  return set->is_dir[i] ? 1 : 2;
}

static int
nameset_put(zipx_nameset_t *set, uint64_t h, int is_dir) {
  size_t i;

  if(set->count * 4 >= set->cap * 3 && nameset_grow(set)) {
    return -1;
  }
  i = nameset_slot(set, h);
  if(!set->hash[i]) {
    set->hash[i] = h;
    set->count++;
  }
  set->is_dir[i] = is_dir ? 1 : 0;
  return 0;
}

/**************************************************************************
 * entry name validation
 **************************************************************************/

/* Normalizes a ZIP entry name into out using '/' separators.
   Returns 0 on success and sets *is_dir / *depth. */
static int
normalize_name(const char *in, char *out, size_t out_size, int *is_dir,
               uint32_t *depth, zipx_ctx_t *c) {
  size_t in_len = strlen(in);
  size_t out_len = 0;
  size_t i = 0;
  size_t seg_len = 0;
  uint32_t levels = 0;

  *is_dir = 0;
  *depth = 0;
  if(!in_len) {
    return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(in_len > c->limits.max_path_len) {
    return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
  }
  /* Absolute paths, drive letters and UNC prefixes are never accepted. */
  if(in[0] == '/' || in[0] == '\\') {
    return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "absolute entry name");
  }
  if(in_len >= 2 && ((in[0] >= 'A' && in[0] <= 'Z') ||
                     (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':') {
    return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "drive letter in entry name");
  }

  while(i < in_len) {
    char ch = in[i];

    if((unsigned char)ch < 0x20 || (unsigned char)ch == 0x7f) {
      return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in,
                      "control character in entry name");
    }
    if(ch == '/' || ch == '\\') {
      if(!seg_len) {
        return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty path segment");
      }
      if(seg_len == 1 && out[out_len - 1] == '.') {
        return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
      }
      if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
        return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
      }
      if(seg_len > c->limits.max_name_len) {
        return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "path component is too long");
      }
      out[out_len++] = '/';
      levels++;
      seg_len = 0;
      i++;
      continue;
    }
    if(out_len + 2 >= out_size) {
      return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
    }
    out[out_len++] = ch;
    seg_len++;
    i++;
  }

  if(seg_len) {
    if(seg_len == 1 && out[out_len - 1] == '.') {
      return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
    }
    if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
      return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
    }
    if(seg_len > c->limits.max_name_len) {
      return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "path component is too long");
    }
    levels++;
  } else {
    *is_dir = 1; /* trailing separator */
  }
  out[out_len] = 0;

  if(!levels) {
    return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(levels > c->limits.max_depth) {
    return ctx_fail(c, ZIPX_ERR_LIMIT_DEPTH, in, "entry path is too deep");
  }
  *depth = levels;
  return 0;
}

/* Registers every ancestor of name as a directory and rejects clashes. */
static int
nameset_add_path(zipx_nameset_t *set, const char *name, int is_dir,
                 zipx_ctx_t *c) {
  char buf[ZIPX_PATH_MAX];
  size_t len = strlen(name);
  size_t i;

  if(len >= sizeof(buf)) {
    return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "entry name is too long");
  }
  memcpy(buf, name, len + 1);

  for(i = 0; i < len; i++) {
    if(buf[i] != '/') {
      continue;
    }
    buf[i] = 0;
    if(nameset_get(set, fnv1a(buf, i)) == 2) {
      return ctx_fail(c, ZIPX_ERR_DUPLICATE, name,
                      "entry uses a file as a directory");
    }
    if(nameset_put(set, fnv1a(buf, i), 1)) {
      return ctx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
    }
    buf[i] = '/';
  }

  if(nameset_put(set, fnv1a(name, len), is_dir)) {
    return ctx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
  }
  return 0;
}

/* Duplicate detection: a repeated file is rejected, a repeated directory is
   harmless. A name used both as a file and as a directory is rejected. */
static int
nameset_check_duplicate(zipx_nameset_t *set, const char *name, int is_dir,
                        zipx_ctx_t *c) {
  int existing = nameset_get(set, fnv1a(name, strlen(name)));

  if(existing == 2 || (existing == 1 && !is_dir)) {
    return ctx_fail(c, ZIPX_ERR_DUPLICATE, name, "duplicate entry name");
  }
  return 0;
}

/**************************************************************************
 * staging helpers
 **************************************************************************/

static int
path_parent(const char *path, char *out, size_t out_size) {
  size_t len = strlen(path);
  size_t i;

  if(!len) {
    return -1;
  }
  i = len;
  while(i && path[i - 1] == '/') {
    i--;
  }
  while(i && path[i - 1] != '/') {
    i--;
  }
  while(i > 1 && path[i - 1] == '/') {
    i--;
  }
  if(!i) {
    return -1; /* relative path without a directory part */
  }
  if(i >= out_size) {
    return -1;
  }
  memcpy(out, path, i);
  out[i] = 0;
  return 0;
}

static void
chmod_0777_fd(int fd) {
  int err = errno;

  /* FAT/exFAT style filesystems reject fchmod; that is not a failure. */
  if(fchmod(fd, 0777)) {
    errno = err;
  }
}

static int
make_staging(zipx_ctx_t *c, const char *parent) {
  unsigned int attempt;

  for(attempt = 0; attempt < ZIPX_TMP_ATTEMPTS; attempt++) {
    int n = snprintf(c->staging, sizeof(c->staging), "%s/%s%ld-%lld-%u.tmp",
                     parent, ZIPX_STAGING_PREFIX, (long)getpid(),
                     (long long)time(NULL), attempt);

    if(n < 0 || (size_t)n >= sizeof(c->staging)) {
      return ctx_fail(c, ZIPX_ERR_INTERNAL, NULL, "staging path is too long");
    }
    if(!mkdir(c->staging, 0777)) {
      c->staging_created = 1;
      return 0;
    }
    if(errno != EEXIST) {
      return ctx_fail(c, ZIPX_ERR_IO, c->staging, "cannot create staging: %s",
                      strerror(errno));
    }
  }
  return ctx_fail(c, ZIPX_ERR_IO, parent, "cannot create staging directory");
}

static void
remove_tree(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *ent;

  if(!dir) {
    rmdir(path);
    return;
  }
  while((ent = readdir(dir))) {
    char child[ZIPX_PATH_MAX];
    struct stat st;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
      continue;
    }
    if(snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
       (int)sizeof(child)) {
      continue;
    }
    if(!lstat(child, &st) && S_ISDIR(st.st_mode)) {
      remove_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(dir);
  rmdir(path);
}

static void
cleanup_staging(zipx_ctx_t *c) {
  if(c->staging_created) {
    remove_tree(c->staging);
    c->staging_created = 0;
  }
}

/* Undo the objects this task already published, newest first. */
static void
rollback_published(zipx_ctx_t *c) {
  size_t i = c->created_count;

  while(i) {
    i--;
    if(c->created_is_dir[i]) {
      rmdir(c->created[i]);
    } else {
      unlink(c->created[i]);
    }
  }
}

/**************************************************************************
 * scan phase
 **************************************************************************/

static int
check_space(zipx_ctx_t *c, const char *target) {
  struct statvfs vfs;
  unsigned long long available;
  unsigned long long block;
  unsigned long long required = c->bytes_total +
                                c->entries_total * ZIPX_SPACE_SLACK_PER_ENTRY;

  if(statvfs(target, &vfs)) {
    return ctx_fail(c, ZIPX_ERR_SPACE, target, "cannot read free space: %s",
                    strerror(errno));
  }
  block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  available = (unsigned long long)vfs.f_bavail * block;
  if(available < required) {
    return ctx_fail(c, ZIPX_ERR_SPACE, target,
                    "not enough space, required %llu bytes, available %llu bytes",
                    required, available);
  }
  return 0;
}

static int
scan_archive(void *zip, zipx_ctx_t *c) {
  zipx_nameset_t set;
  int32_t err;
  int ret = 0;

  if(nameset_init(&set, 4096)) {
    return ctx_fail(c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
  }

  err = mz_zip_goto_first_entry(zip);
  while(err == MZ_OK) {
    mz_zip_file *info = NULL;
    char name[ZIPX_PATH_MAX];
    int is_dir = 0;
    uint32_t depth = 0;
    uint64_t uncompressed;

    if(mz_zip_entry_get_info(zip, &info) || !info || !info->filename) {
      ret = ctx_fail(c, ZIPX_ERR_FORMAT, NULL, "cannot read entry header");
      break;
    }
    if(strlen(info->filename) != info->filename_size) {
      ret = ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, NULL,
                     "entry name contains a NUL byte");
      break;
    }
    ret = normalize_name(info->filename, name, sizeof(name), &is_dir, &depth,
                         c);
    (void)depth;
    if(ret) {
      break;
    }
    if((info->flag & MZ_ZIP_FLAG_ENCRYPTED) || info->aes_version) {
      /* Encrypted entries are readable, but only with a password. The
         password itself is verified when the entry data is opened -- for
         ZipCrypto against the 1-2 byte header check, for WinZip AES against
         the 2 byte PBKDF2 verifier -- so a wrong password surfaces as
         ZIPX_ERR_PASSWORD from the extract phase rather than here. */
      if(!c->password) {
        ret = ctx_fail(c, ZIPX_ERR_PASSWORD, name,
                       "entry is encrypted and no password was given");
        break;
      }
    }
    if(info->compression_method != MZ_COMPRESS_METHOD_STORE &&
       info->compression_method != MZ_COMPRESS_METHOD_DEFLATE) {
      ret = ctx_fail(c, ZIPX_ERR_UNSUPPORTED, name,
                     "unsupported compression method %d",
                     (int)info->compression_method);
      break;
    }
    if(mz_zip_attrib_is_symlink(info->external_fa,
                                info->version_madeby) == MZ_OK) {
      ret = ctx_fail(c, ZIPX_ERR_SPECIAL, name, "symbolic link entry");
      break;
    }
    is_dir = is_dir || (mz_zip_attrib_is_dir(info->external_fa,
                                             info->version_madeby) == MZ_OK);
    if(!is_dir && MZ_HOST_SYSTEM(info->version_madeby) == MZ_HOST_SYSTEM_UNIX) {
      mode_t mode = (mode_t)(info->external_fa >> 16);

      if(mode && (mode & S_IFMT) != S_IFREG) {
        ret = ctx_fail(c, ZIPX_ERR_SPECIAL, name, "special file entry");
        break;
      }
    }
    if(nameset_check_duplicate(&set, name, is_dir, c) ||
       nameset_add_path(&set, name, is_dir, c)) {
      ret = -1;
      break;
    }

    uncompressed = info->uncompressed_size > 0 ?
                     (uint64_t)info->uncompressed_size : 0;
    if(!is_dir) {
      if(uncompressed > c->limits.max_file_bytes) {
        ret = ctx_fail(c, ZIPX_ERR_LIMIT_FILE, name,
                       "entry is larger than %llu bytes",
                       (unsigned long long)c->limits.max_file_bytes);
        break;
      }
      if(c->limits.max_ratio && info->compressed_size > 0 &&
         uncompressed >= c->limits.ratio_min_bytes &&
         uncompressed > (uint64_t)info->compressed_size *
                        c->limits.max_ratio) {
        ret = ctx_fail(c, ZIPX_ERR_LIMIT_RATIO, name,
                       "compression ratio is above %u (%llu -> %llu bytes)",
                       c->limits.max_ratio,
                       (unsigned long long)info->compressed_size,
                       (unsigned long long)uncompressed);
        break;
      }
      if(uncompressed >= c->limits.max_total_bytes ||
         c->bytes_total > c->limits.max_total_bytes - uncompressed) {
        ret = ctx_fail(c, ZIPX_ERR_LIMIT_TOTAL, name,
                       "archive contents are larger than %llu bytes",
                       (unsigned long long)c->limits.max_total_bytes);
        break;
      }
      c->bytes_total += uncompressed;
    }

    c->entries_total++;
    if(c->entries_total > c->limits.max_entries) {
      ret = ctx_fail(c, ZIPX_ERR_LIMIT_ENTRIES, name,
                     "archive has more than %llu entries",
                     (unsigned long long)c->limits.max_entries);
      break;
    }
    if(c->entries_total % 4096 == 0) {
      if(canceled(c)) {
        ret = ctx_fail(c, ZIPX_ERR_CANCELED, name, NULL);
        break;
      }
      report(c, ZIPX_PHASE_SCAN, name, 0);
    }

    err = mz_zip_goto_next_entry(zip);
  }

  if(!ret && err != MZ_END_OF_LIST && err != MZ_OK) {
    ret = ctx_fail(c, ZIPX_ERR_FORMAT, NULL, "corrupt central directory");
  }
  nameset_free(&set);
  return ret;
}

/**************************************************************************
 * extract phase
 **************************************************************************/

/* Builds "<staging>/<rel>[/<leaf>]" for the plain-call fallbacks below.
   rel may be empty (staging root); leaf may be NULL. */
static void
staging_path(const zipx_ctx_t *c, const char *rel, const char *leaf,
             char *out, size_t cap) {
  if(rel[0] && leaf) {
    snprintf(out, cap, "%s/%s/%s", c->staging, rel, leaf);
  } else if(rel[0]) {
    snprintf(out, cap, "%s/%s", c->staging, rel);
  } else if(leaf) {
    snprintf(out, cap, "%s/%s", c->staging, leaf);
  } else {
    snprintf(out, cap, "%s", c->staging);
  }
}

/* The *at() family can be present in the target libc yet fail at runtime
   without setting errno (observed on PS5 hardware: mkdirat() returns -1
   with errno 0, while plain path-based calls work). Every *at() call in
   the extract phase therefore falls back to a full-path call built from
   the staging root before reporting an I/O error. */

/* Opens (creating when needed) every directory of rel below root_fd.
   Returns an open descriptor for the deepest directory. */
static int
open_parent_dirs(int root_fd, const char *rel, zipx_ctx_t *c) {
  char buf[ZIPX_PATH_MAX];
  char cur[ZIPX_PATH_MAX] = "";
  int fd = root_fd;
  char *seg;
  char *save = NULL;

  if(strlen(rel) >= sizeof(buf)) {
    ctx_fail(c, ZIPX_ERR_LIMIT_NAME, rel, "path is too long");
    return -1;
  }
  strcpy(buf, rel);

  for(seg = strtok_r(buf, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
    int next;
    int made = 0;

    if(cur[0]) {
      strncat(cur, "/", sizeof(cur) - strlen(cur) - 1);
    }
    strncat(cur, seg, sizeof(cur) - strlen(cur) - 1);

    if(!mkdirat(fd, seg, 0777)) {
      made = 1;
    } else if(errno != EEXIST) {
      char full[ZIPX_PATH_MAX];

      staging_path(c, cur, NULL, full, sizeof(full));
      if(mkdir(full, 0777) && errno != EEXIST) {
        ctx_fail(c, ZIPX_ERR_IO, rel, "cannot create directory '%s': %s "
                 "(errno=%d)", seg, strerror(errno), errno);
        if(fd != root_fd) {
          close(fd);
        }
        return -1;
      }
      made = 1;
    }

    next = openat(fd, seg, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(next < 0) {
      char full[ZIPX_PATH_MAX];

      staging_path(c, cur, NULL, full, sizeof(full));
      next = open(full, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if(next < 0) {
        ctx_fail(c, ZIPX_ERR_IO, rel, "cannot open directory '%s': %s "
                 "(errno=%d)", seg, strerror(errno), errno);
        if(fd != root_fd) {
          close(fd);
        }
        return -1;
      }
    }
    if(made) {
      chmod_0777_fd(next);
      c->dirs_created++;
    }
    if(fd != root_fd) {
      close(fd);
    }
    fd = next;
  }
  return fd;
}

static int
write_entry(void *zip, zipx_ctx_t *c, int root_fd, const char *name,
            uint64_t declared) {
  char dir_part[ZIPX_PATH_MAX];
  char base[ZIPX_PATH_MAX];
  char tmp[ZIPX_PATH_MAX];
  uint64_t remaining = declared ? declared : c->limits.max_file_bytes;
  uint64_t written = 0;
  char *buf = malloc(ZIPX_IO_BUFFER);
  char *slash;
  int dir_fd;
  int fd = -1;
  int ret = 0;

  if(!buf) {
    return ctx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
  }
  snprintf(dir_part, sizeof(dir_part), "%s", name);
  slash = strrchr(dir_part, '/');
  if(slash) {
    snprintf(base, sizeof(base), "%s", slash + 1);
    *slash = 0;
  } else {
    snprintf(base, sizeof(base), "%s", name);
    dir_part[0] = 0;
  }
  if(!base[0]) {
    free(buf);
    return ctx_fail(c, ZIPX_ERR_UNSAFE_NAME, name, "empty file name");
  }

  dir_fd = open_parent_dirs(root_fd, dir_part, c);
  if(dir_fd < 0) {
    free(buf);
    return -1;
  }

  snprintf(tmp, sizeof(tmp), "%s%u", ZIPX_PART_PREFIX, ++c->part_counter);
  fd = openat(dir_fd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if(fd < 0) {
    /* *at() fallback (see open_parent_dirs). */
    char full[ZIPX_PATH_MAX];

    staging_path(c, dir_part, tmp, full, sizeof(full));
    fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  }
  if(fd < 0) {
    ctx_fail(c, ZIPX_ERR_IO, name, "cannot create file '%s': %s (errno=%d)",
             tmp, strerror(errno), errno);
    if(dir_fd != root_fd) {
      close(dir_fd);
    }
    free(buf);
    return -1;
  }

  {
    /* Opening the entry data is also where a supplied password is checked:
       mz_strm_pkcrypt.c compares the decrypted header byte(s) and
       mz_strm_wzaes.c the 2 byte PBKDF2 verifier, both returning
       MZ_PASSWORD_ERROR on a mismatch. */
    int err = mz_zip_entry_read_open(zip, 0, c->password);

    if(err != MZ_OK) {
      ctx_fail(c, err == MZ_PASSWORD_ERROR ? ZIPX_ERR_PASSWORD : ZIPX_ERR_FORMAT,
               name, "%s",
               err == MZ_PASSWORD_ERROR ? "the password is wrong"
                                        : "cannot read entry data");
      ret = -1;
      goto done;
    }
  }

  while(ret == 0) {
    int32_t n = mz_zip_entry_read(zip, buf, (int32_t)ZIPX_IO_BUFFER);

    if(n == 0) {
      break;
    }
    if(n < 0) {
      ctx_fail(c, n == MZ_CRC_ERROR ? ZIPX_ERR_CRC : ZIPX_ERR_FORMAT, name,
               "%s", n == MZ_CRC_ERROR ? "crc mismatch" : "corrupt entry data");
      ret = -1;
      break;
    }
    if((uint64_t)n > remaining || written + (uint64_t)n > remaining) {
      ctx_fail(c, ZIPX_ERR_LIMIT_FILE, name,
               "entry is larger than the declared size");
      ret = -1;
      break;
    }
    if(written + (uint64_t)n > c->limits.max_total_bytes - c->bytes_done) {
      ctx_fail(c, ZIPX_ERR_LIMIT_TOTAL, name,
               "archive contents are larger than %llu bytes",
               (unsigned long long)c->limits.max_total_bytes);
      ret = -1;
      break;
    }
    while(n > 0) {
      ssize_t w = write(fd, buf, (size_t)n);

      if(w <= 0) {
        ctx_fail(c, ZIPX_ERR_IO, name, "write failed: %s",
                 strerror(errno ? errno : EIO));
        ret = -1;
        break;
      }
      n -= (int32_t)w;
      written += (uint64_t)w;
      c->bytes_done += (uint64_t)w;
    }
    if(ret) {
      break;
    }
    if(canceled(c)) {
      ctx_fail(c, ZIPX_ERR_CANCELED, name, NULL);
      ret = -1;
      break;
    }
    report(c, ZIPX_PHASE_EXTRACT, name, 0);
  }

  {
    uint32_t crc = 0;
    int64_t comp = 0;
    int64_t uncomp = 0;
    int32_t close_err = mz_zip_entry_read_close(zip, &crc, &comp, &uncomp);

    if(!ret && close_err != MZ_OK) {
      ctx_fail(c, close_err == MZ_CRC_ERROR ? ZIPX_ERR_CRC : ZIPX_ERR_FORMAT,
               name, "%s", close_err == MZ_CRC_ERROR ?
               "crc mismatch" : "corrupt entry data");
      ret = -1;
    }
  }

done:
  if(!ret) {
    /* No fsync here, on purpose.  The per-entry flush used to cost 20-30
       minutes on a 95k-file archive (measured on PS5-class storage) and buys
       nothing the design needs: a crash mid-extract leaves the staging tree,
       which is discarded on the next run, and publish is a rename-only phase
       (see publish_entry).  The RAR and 7z engines never flushed per entry
       either; all three now share the same "sync nothing, rename everything"
       policy. */
    if(close(fd)) {
      ctx_fail(c, ZIPX_ERR_IO, name, "cannot close file: %s", strerror(errno));
      ret = -1;
    }
    fd = -1;
  }
  if(!ret && renameat(dir_fd, tmp, dir_fd, base)) {
    /* *at() fallback (see open_parent_dirs). */
    char src_full[ZIPX_PATH_MAX];
    char dst_full[ZIPX_PATH_MAX];

    staging_path(c, dir_part, tmp, src_full, sizeof(src_full));
    staging_path(c, dir_part, base, dst_full, sizeof(dst_full));
    if(rename(src_full, dst_full)) {
      ctx_fail(c, ZIPX_ERR_IO, name, "cannot move file into place: %s "
               "(errno=%d)", strerror(errno), errno);
      ret = -1;
    }
  }
  if(fd >= 0) {
    close(fd);
  }
  if(ret) {
    if(unlinkat(dir_fd, tmp, 0)) {
      /* *at() fallback (see open_parent_dirs). */
      char full[ZIPX_PATH_MAX];

      staging_path(c, dir_part, tmp, full, sizeof(full));
      unlink(full);
    }
  } else {
    c->files_created++;
  }
  if(dir_fd != root_fd) {
    close(dir_fd);
  }
  free(buf);
  return ret;
}

static int
extract_archive(void *zip, zipx_ctx_t *c, int root_fd) {
  int32_t err = mz_zip_goto_first_entry(zip);
  int ret = 0;
  uint64_t index = 0;

  while(err == MZ_OK && !ret) {
    mz_zip_file *info = NULL;
    char name[ZIPX_PATH_MAX];
    int is_dir = 0;
    uint32_t depth = 0;

    if(mz_zip_entry_get_info(zip, &info) || !info || !info->filename) {
      return ctx_fail(c, ZIPX_ERR_FORMAT, NULL, "cannot read entry header");
    }
    if(strlen(info->filename) != info->filename_size ||
       normalize_name(info->filename, name, sizeof(name), &is_dir, &depth, c)) {
      return (int)c->result->status;
    }
    (void)depth;
    is_dir = is_dir || (mz_zip_attrib_is_dir(info->external_fa,
                                             info->version_madeby) == MZ_OK);
    if(canceled(c)) {
      ret = ctx_fail(c, ZIPX_ERR_CANCELED, name, NULL);
      break;
    }
    if(is_dir) {
      char copy[ZIPX_PATH_MAX];
      int dir_fd;

      snprintf(copy, sizeof(copy), "%s", name);
      dir_fd = open_parent_dirs(root_fd, copy, c);
      if(dir_fd < 0) {
        ret = -1;
        break;
      }
      if(dir_fd != root_fd) {
        close(dir_fd);
      }
    } else {
      uint64_t declared = info->uncompressed_size > 0 ?
                            (uint64_t)info->uncompressed_size : 0;

      if(write_entry(zip, c, root_fd, name, declared)) {
        ret = -1;
        break;
      }
    }
    c->entries_done = ++index;
    report(c, ZIPX_PHASE_EXTRACT, name, 0);
    err = mz_zip_goto_next_entry(zip);
  }

  if(!ret && err != MZ_END_OF_LIST) {
    ret = ctx_fail(c, ZIPX_ERR_FORMAT, NULL, "corrupt central directory");
  }
  return ret;
}

/**************************************************************************
 * publish phase
 **************************************************************************/

static int publish_dir(const char *src, const char *dst, int depth,
                       zipx_ctx_t *c);

static int
publish_entry(const char *src, const char *dst, const char *name, int depth,
              zipx_ctx_t *c) {
  char src_child[ZIPX_PATH_MAX];
  char dst_child[ZIPX_PATH_MAX];
  struct stat st;
  struct stat src_st;
  int src_is_dir;

  if(snprintf(src_child, sizeof(src_child), "%s/%s", src, name) >=
       (int)sizeof(src_child) ||
     snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, name) >=
       (int)sizeof(dst_child)) {
    return ctx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "path is too long");
  }
  if(lstat(src_child, &src_st)) {
    return ctx_fail(c, ZIPX_ERR_IO, src_child, "cannot read staging: %s",
                    strerror(errno));
  }
  src_is_dir = S_ISDIR(src_st.st_mode) ? 1 : 0;

  if(lstat(dst_child, &st)) {
    if(errno != ENOENT) {
      return ctx_fail(c, ZIPX_ERR_IO, dst_child, "cannot check target: %s",
                      strerror(errno));
    }
    if(rename(src_child, dst_child)) {
      return ctx_fail(c, ZIPX_ERR_IO, dst_child, "cannot publish: %s",
                      strerror(errno));
    }
    return remember_published(c, dst_child, src_is_dir);
  }

  if(S_ISDIR(st.st_mode)) {
    if(!src_is_dir) {
      /* A file in the archive collides with a directory on disk. */
      return ctx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                      "file collides with an existing directory");
    }
    if(c->conflict != ZIPX_CONFLICT_MERGE) {
      return ctx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                      "directory already exists");
    }
    if(depth >= ZIPX_PUBLISH_MAX_DEPTH) {
      return ctx_fail(c, ZIPX_ERR_LIMIT_DEPTH, dst_child, "path is too deep");
    }
    if(publish_dir(src_child, dst_child, depth + 1, c)) {
      return -1;
    }
    rmdir(src_child);
    return 0;
  }

  if(S_ISREG(st.st_mode)) {
    if(!src_is_dir) {
      if(c->conflict == ZIPX_CONFLICT_OVERWRITE) {
        if(rename(src_child, dst_child)) {
          return ctx_fail(c, ZIPX_ERR_IO, dst_child, "cannot replace: %s",
                          strerror(errno));
        }
        return remember_published(c, dst_child, 0);
      }
      if(c->conflict == ZIPX_CONFLICT_MERGE) {
        /* Preserve the existing file; drop the staged copy. */
        if(unlink(src_child)) {
          return ctx_fail(c, ZIPX_ERR_IO, dst_child,
                          "cannot drop staged file: %s", strerror(errno));
        }
        return 0;
      }
    }
    return ctx_fail(c, ZIPX_ERR_CONFLICT, dst_child, "target already exists");
  }
  return ctx_fail(c, ZIPX_ERR_CONFLICT, dst_child, "target already exists");
}

static int
publish_dir(const char *src, const char *dst, int depth, zipx_ctx_t *c) {
  DIR *dir = opendir(src);
  struct dirent *ent;
  char **names = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t i;
  int ret = 0;

  if(!dir) {
    return ctx_fail(c, ZIPX_ERR_IO, src, "cannot read staging: %s",
                    strerror(errno));
  }
  while((ent = readdir(dir))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
      continue;
    }
    if(count == cap) {
      size_t next = cap ? cap * 2 : 64;
      char **grown = realloc(names, next * sizeof(*grown));

      if(!grown) {
        ret = ctx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
        break;
      }
      names = grown;
      cap = next;
    }
    if(!(names[count] = strdup(ent->d_name))) {
      ret = ctx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
      break;
    }
    count++;
  }
  closedir(dir);

  for(i = 0; i < count && !ret; i++) {
    report(c, ZIPX_PHASE_PUBLISH, names[i], 0);
    ret = publish_entry(src, dst, names[i], depth, c);
  }
  for(i = 0; i < count; i++) {
    free(names[i]);
  }
  free(names);
  return ret;
}

static int
publish_staging(zipx_ctx_t *c, const char *dst_dir, int dst_existed) {
  int ret;

  report(c, ZIPX_PHASE_PUBLISH, dst_dir, 1);
  if(!dst_existed) {
    if(rename(c->staging, dst_dir)) {
      return ctx_fail(c, ZIPX_ERR_IO, dst_dir, "cannot publish: %s",
                      strerror(errno));
    }
    c->staging_created = 0; /* the directory now belongs to the user */
    return 0;
  }
  ret = publish_dir(c->staging, dst_dir, 0, c);
  if(ret) {
    rollback_published(c);
  }
  return ret;
}

/**************************************************************************
 * public entry point
 **************************************************************************/

/* Opens the archive for scanning. A split set is served by the volume stream;
   because tools disagree on whether a split keeps absolute offsets or offsets
   relative to each volume, both layouts are attempted before giving up. */
static int
open_archive(const char *zip_path, zipx_volume_t *vol, int vol_set,
             zipx_ctx_t *c, void **zip_out, void **stream_out) {
  int attempts = vol_set ? 2 : 1;
  int attempt;

  for(attempt = 0; attempt < attempts; attempt++) {
    void *stream = NULL;
    void *zip = NULL;
    int rc;

    if(vol_set) {
      int mode = vol->mode;

      if(attempt == 1) {
        mode = (vol->mode == ZIPX_VOL_MODE_DISK) ? ZIPX_VOL_MODE_CONCAT
                                                 : ZIPX_VOL_MODE_DISK;
      }
      stream = zipx_volstream_create(mode);
      if(stream && zipx_volstream_set_parts(
                     stream, (const char *const *)vol->paths,
                     vol->count) != MZ_OK) {
        zipx_volstream_delete(&stream);
        stream = NULL;
      }
    } else {
      stream = mz_stream_os_create();
    }
    zip = mz_zip_create();
    if(!stream || !zip) {
      if(stream) {
        mz_stream_delete(&stream);
      }
      if(zip) {
        mz_zip_delete(&zip);
      }
      ctx_fail(c, ZIPX_ERR_INTERNAL, zip_path, "out of memory");
      return (int)c->result->status;
    }
    rc = mz_stream_open(stream, vol_set ? vol->paths[0] : zip_path,
                        MZ_OPEN_MODE_READ);
    if(rc == MZ_OK) {
      rc = mz_zip_open(zip, stream, MZ_OPEN_MODE_READ);
    }
    if(rc == MZ_OK) {
      *zip_out = zip;
      *stream_out = stream;
      return ZIPX_OK;
    }
    /* Wrong layout for this set (or a corrupt archive): drop it and retry. */
    mz_zip_close(zip);
    mz_zip_delete(&zip);
    mz_stream_close(stream);
    mz_stream_delete(&stream);
  }

  if(vol_set) {
    ctx_fail(c, ZIPX_ERR_OPEN, zip_path,
             "cannot read the split archive starting at '%s' (%d volumes): %s",
             vol->paths[0], vol->count,
             errno ? strerror(errno) : "no known volume layout matched");
  } else {
    ctx_fail(c, ZIPX_ERR_OPEN, zip_path, "%s", strerror(errno ? errno : EIO));
  }
  return (int)c->result->status;
}

zipx_status_t
zipx_extract(const char *zip_path, const char *dst_dir,
             zipx_conflict_t conflict, const zipx_limits_t *limits,
             zipx_cancel_fn cancel, zipx_progress_fn progress,
             void *userdata, const char *password, zipx_result_t *result) {
  zipx_ctx_t ctx;
  zipx_ctx_t *c = &ctx;
  char parent[ZIPX_PATH_MAX];
  char dst_copy[ZIPX_PATH_MAX];
  struct stat st;
  void *zip = NULL;
  void *stream = NULL;
  int root_fd = -1;
  int dst_existed = 0;
  int status = ZIPX_OK;
  zipx_volume_t vol;
  int vol_set = 0;

  if(!result || !zip_path || !dst_dir || !dst_dir[0]) {
    if(result) {
      memset(result, 0, sizeof(*result));
      result->status = ZIPX_ERR_INTERNAL;
      snprintf(result->message, sizeof(result->message), "invalid argument");
    }
    return ZIPX_ERR_INTERNAL;
  }

  memset(&ctx, 0, sizeof(ctx));
  memset(&vol, 0, sizeof(vol));
  vol.index = -1;
  memset(result, 0, sizeof(*result));
  c->result = result;
  c->conflict = conflict;
  c->limits = limits ? *limits : *zipx_default_limits();
  c->cancel = cancel;
  c->progress = progress;
  c->userdata = userdata;
  c->password = (password && password[0]) ? password : NULL;

  snprintf(dst_copy, sizeof(dst_copy), "%s", dst_dir);
  {
    size_t len = strlen(dst_copy);

    while(len > 1 && dst_copy[len - 1] == '/') {
      dst_copy[--len] = 0;
    }
  }
  if(path_parent(dst_copy, parent, sizeof(parent))) {
    status = ctx_fail(c, ZIPX_ERR_INTERNAL, dst_dir, "invalid destination");
    goto done;
  }
  if(stat(parent, &st) || !S_ISDIR(st.st_mode)) {
    status = ctx_fail(c, ZIPX_ERR_IO, parent, "destination parent is missing");
    goto done;
  }
  dst_existed = !lstat(dst_copy, &st);
  if(dst_existed && !S_ISDIR(st.st_mode)) {
    status = ctx_fail(c, ZIPX_ERR_CONFLICT, dst_copy,
                      "destination is not a directory");
    goto done;
  }

  {
    char *vol_err = NULL;
    int vrc = zipx_volume_detect(zip_path, &vol, &vol_err);

    if(vrc < 0) {
      /* A volume of a set that is incomplete gets a precise message here
         instead of a generic "cannot open" further down. */
      status = ctx_fail(c, ZIPX_ERR_OPEN, zip_path, "%s",
                        vol_err ? vol_err : "cannot read the volume set");
      free(vol_err);
      goto done;
    }
    free(vol_err);
    if(vrc > 0) {
      vol_set = 1;
    }
  }
  status = open_archive(zip_path, &vol, vol_set, c, &zip, &stream);
  if(status != ZIPX_OK) {
    goto done;
  }

  if(scan_archive(zip, c)) {
    status = (int)c->result->status;
    goto done;
  }
  if(canceled(c)) {
    status = ctx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    goto done;
  }
  if(check_space(c, dst_existed ? dst_copy : parent)) {
    status = (int)c->result->status;
    goto done;
  }
  if(make_staging(c, parent)) {
    status = (int)c->result->status;
    goto done;
  }

  root_fd = open(c->staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if(root_fd < 0) {
    status = ctx_fail(c, ZIPX_ERR_IO, c->staging, "cannot open staging: %s",
                      strerror(errno));
    goto done;
  }

  if(extract_archive(zip, c, root_fd)) {
    status = (int)c->result->status;
    goto done;
  }
  if(root_fd >= 0) {
    close(root_fd);
    root_fd = -1;
  }
  if(canceled(c)) {
    status = ctx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    goto done;
  }
  if(publish_staging(c, dst_copy, dst_existed)) {
    status = (int)c->result->status;
    goto done;
  }

  result->status = ZIPX_OK;
  status = ZIPX_OK;

done:
  if(root_fd >= 0) {
    close(root_fd);
  }
  cleanup_staging(c);
  if(zip) {
    mz_zip_close(zip);
    mz_zip_delete(&zip);
  }
  if(stream) {
    mz_stream_close(stream);
    mz_stream_delete(&stream);
  }
  zipx_volume_free(&vol);
  result->entries_total = c->entries_total;
  result->entries_done = c->entries_done;
  result->bytes_total = c->bytes_total;
  result->files_created = c->files_created;
  result->dirs_created = c->dirs_created;
  if(status != ZIPX_OK && !result->message[0]) {
    snprintf(result->message, sizeof(result->message), "%s",
             zipx_status_string(result->status));
  }
  if(status != ZIPX_OK) {
    result->status = (zipx_status_t)status;
  }
  report(c, ZIPX_PHASE_CLEANUP, NULL, 1);
  free_published(c);
  return result->status;
}
