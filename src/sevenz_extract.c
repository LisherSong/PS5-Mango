/* Safe 7z extraction engine used by the /api/extract task.

   Input arrives through src/sevenz_volstream.c, so a single `name.7z` and a
   byte-split set (`name.7z.001`, `.002`, ...) take the same path: the SDK's
   header reader sees one continuous stream either way.

   Folders are decoded by src/sevenz_chain.c, which parses the folder
   descriptor itself and drives the coder chain in a pull pipeline.  That is
   what makes solid 7z archives usable on a console: the SDK's own
   SzArEx_Extract() wants the whole folder in memory, and a solid block is
   routinely gigabytes.

   The publish / staging / rollback / name-validation machinery is mirrored
   from rar_extract.c, which in turn mirrors zip_extract.c, so that any
   consumer of zipx_result_t gets the same error and progress contract
   regardless of archive format. */

#include "sevenz_extract.h"
#include "nx_fs.h"   /* NX_CREATE_MODE: extracted files must be 0777 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"

#include "sevenz_chain.h"
#include "sevenz_header.h"
#include "sevenz_mt.h"
#include "sevenz_volstream.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define SZX_STAGING_PREFIX ".wfm-extract-"
#define SZX_TMP_ATTEMPTS 64
#define SZX_PUBLISH_MAX_DEPTH 128
#define SZX_SPACE_SLACK_PER_ENTRY 512
/* 256 KiB: big enough that a solid folder is not refilled constantly, small
   enough that a hostile dictionary size cannot turn into an allocation storm. */
#define SZX_INPUT_BUF_SIZE (1u << 18)
/* Room for a name plus room to notice that it did not fit. */
#define SZX_NAME_MAX (ZIPX_PATH_MAX + 16)

/* 7-Zip's attribute word: bit 15 says "the high half is a Unix mode", bit 4 is
   the DOS directory flag.  Both come from PropID::kWinAttrib. */
#define SZX_ATTRIB_UNIX_EXTENSION 0x8000u
#define SZX_ATTRIB_DIRECTORY 0x0010u

/* Windows FILETIME counts 100 ns ticks from 1601-01-01; Unix time starts at
   1970.  The offset is the standard 11644473600 seconds. */
#define SZX_FILETIME_UNIX_DELTA 11644473600ULL

static ISzAlloc g_sz_alloc = { SzAlloc, SzFree };

/**************************************************************************
 * context + small helpers (mirrors of rarx_*; names prefixed szx_)
 **************************************************************************/

typedef struct {
  zipx_conflict_t conflict;
  zipx_limits_t limits;
  zipx_cancel_fn cancel;
  zipx_progress_fn progress;
  void *userdata;
  zipx_result_t *result;
  const char *password;
  const char *sevenz_path;  /* original archive path, surfaced in errors */
  uint64_t entries_total;
  uint64_t entries_done;
  uint64_t bytes_total;
  uint64_t bytes_done;
  uint64_t files_created;
  uint64_t dirs_created;
  uint64_t progress_floor;
  long long last_report_ms;
  char staging[ZIPX_PATH_MAX];
  char **created;
  size_t created_count;
  size_t created_cap;
  unsigned char *created_is_dir;
  int staging_created;
} szx_ctx_t;

static void
szx_set_detail(szx_ctx_t *c, const char *path) {
  if(path) {
    snprintf(c->result->detail, sizeof(c->result->detail), "%s", path);
  }
}

static int
szx_fail(szx_ctx_t *c, zipx_status_t status, const char *detail,
         const char *fmt, ...) {
  va_list ap;

  c->result->status = status;
  c->result->sys_errno = errno;
  szx_set_detail(c, detail);
  va_start(ap, fmt);
  if(fmt) {
    vsnprintf(c->result->message, sizeof(c->result->message), fmt, ap);
  }
  va_end(ap);
  if(!c->result->message[0]) {
    snprintf(c->result->message, sizeof(c->result->message), "%s",
             zipx_status_string(status));
  }
  return (int)status;
}

static long long
szx_mono_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The decoder calls this from its innermost loop, so a report goes out only
   every 200 ms, or every 1 MiB when the clock has not moved. */
static void
szx_report(szx_ctx_t *c, int phase, const char *current, int force) {
  zipx_progress_t p;
  long long now;

  if(!c->progress) {
    return;
  }
  now = szx_mono_ms();
  if(!force && c->last_report_ms && now - c->last_report_ms < 200 &&
     c->bytes_done - c->progress_floor < (1024 * 1024)) {
    return;
  }
  c->last_report_ms = now;
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
szx_canceled(szx_ctx_t *c) {
  return c->cancel && c->cancel(c->userdata);
}

/* sz_chain_cancel_fn shape, for callbacks that want a void* context. */
static int
szx_cancel_cb(void *ctx) {
  return szx_canceled((szx_ctx_t *)ctx);
}

/* Remembers what the publish phase put in dst_dir, so a later failure can undo
   it.  Paths are recorded in publish order; rollback walks them backwards. */
static int
szx_remember_published(szx_ctx_t *c, const char *path, int is_dir) {
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
szx_free_published(szx_ctx_t *c) {
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
 * duplicate detection (FNV-1a hash set; same trick as zip_extract.c)
 **************************************************************************/

typedef struct {
  uint64_t *hash;
  unsigned char *is_dir;
  size_t cap;
  size_t count;
} szx_nameset_t;

static uint64_t
szx_fnv1a(const char *s, size_t len) {
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for(i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h ? h : 1;
}

static int
szx_nameset_init(szx_nameset_t *set, size_t hint) {
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
szx_nameset_free(szx_nameset_t *set) {
  free(set->hash);
  free(set->is_dir);
  set->hash = NULL;
  set->is_dir = NULL;
  set->cap = set->count = 0;
}

static size_t
szx_nameset_slot(szx_nameset_t *set, uint64_t h) {
  size_t mask = set->cap - 1;
  size_t i = (size_t)(h & mask);

  while(set->hash[i] && set->hash[i] != h) {
    i = (i + 1) & mask;
  }
  return i;
}

static int
szx_nameset_grow(szx_nameset_t *set) {
  szx_nameset_t bigger;
  size_t i;

  if(szx_nameset_init(&bigger, set->cap)) {
    return -1;
  }
  for(i = 0; i < set->cap; i++) {
    size_t slot;

    if(!set->hash[i]) {
      continue;
    }
    slot = szx_nameset_slot(&bigger, set->hash[i]);
    bigger.hash[slot] = set->hash[i];
    bigger.is_dir[slot] = set->is_dir[i];
    bigger.count++;
  }
  szx_nameset_free(set);
  *set = bigger;
  return 0;
}

static int
szx_nameset_get(szx_nameset_t *set, uint64_t h) {
  size_t i = szx_nameset_slot(set, h);

  if(!set->hash[i]) {
    return 0;
  }
  return set->is_dir[i] ? 1 : 2;
}

static int
szx_nameset_put(szx_nameset_t *set, uint64_t h, int is_dir) {
  size_t i;

  if(set->count * 4 >= set->cap * 3 && szx_nameset_grow(set)) {
    return -1;
  }
  i = szx_nameset_slot(set, h);
  if(!set->hash[i]) {
    set->hash[i] = h;
    set->count++;
  }
  set->is_dir[i] = is_dir ? 1 : 0;
  return 0;
}

/* Registers `name` plus every directory on the way to it, and refuses a name
   that reuses an existing file as a directory. */
static int
szx_nameset_add_path(szx_nameset_t *set, const char *name, int is_dir,
                     szx_ctx_t *c) {
  char buf[ZIPX_PATH_MAX];
  size_t len = strlen(name);
  size_t i;

  if(len >= sizeof(buf)) {
    return szx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "entry name is too long");
  }
  memcpy(buf, name, len + 1);

  for(i = 0; i < len; i++) {
    if(buf[i] != '/') {
      continue;
    }
    buf[i] = 0;
    if(szx_nameset_get(set, szx_fnv1a(buf, i)) == 2) {
      return szx_fail(c, ZIPX_ERR_DUPLICATE, name,
                      "entry uses a file as a directory");
    }
    if(szx_nameset_put(set, szx_fnv1a(buf, i), 1)) {
      return szx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
    }
    buf[i] = '/';
  }

  if(szx_nameset_put(set, szx_fnv1a(name, len), is_dir)) {
    return szx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
  }
  return 0;
}

static int
szx_nameset_check_duplicate(szx_nameset_t *set, const char *name, int is_dir,
                            szx_ctx_t *c) {
  int existing = szx_nameset_get(set, szx_fnv1a(name, strlen(name)));

  if(existing == 2 || (existing == 1 && !is_dir)) {
    return szx_fail(c, ZIPX_ERR_DUPLICATE, name, "duplicate entry name");
  }
  return 0;
}

/**************************************************************************
 * entry name validation (mirrors zip_extract.c::normalize_name; 7z stores
 * names as UTF-16, so the caller converts first)
 **************************************************************************/

/* Rewrites an archive name into a safe relative path.  A name that would
   escape the destination, hide a control character, or nest past the depth
   limit is refused with a message that names it. */
static int
szx_normalize_name(const char *in, char *out, size_t out_size, uint32_t *depth,
                   szx_ctx_t *c) {
  size_t in_len = strlen(in);
  size_t out_len = 0;
  size_t i = 0;
  size_t seg_len = 0;
  uint32_t levels = 0;

  *depth = 0;
  if(!in_len) {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(in_len > c->limits.max_path_len) {
    return szx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
  }
  if(in[0] == '/' || in[0] == '\\') {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "absolute entry name");
  }
  if(in_len >= 2 && ((in[0] >= 'A' && in[0] <= 'Z') ||
                     (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':') {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "drive letter in entry name");
  }

  while(i < in_len) {
    char ch = in[i];

    if((unsigned char)ch < 0x20 || (unsigned char)ch == 0x7f) {
      return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in,
                      "control character in entry name");
    }
    /* 7-Zip writes '/', but a name added from a Windows path can still carry
       a backslash. Normalize so the staging tree is consistent. */
    if(ch == '\\') {
      ch = '/';
    }
    if(ch == '/') {
      if(!seg_len) {
        return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty path segment");
      }
      if(seg_len == 1 && out[out_len - 1] == '.') {
        return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
      }
      if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
        return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
      }
      if(seg_len > c->limits.max_name_len) {
        return szx_fail(c, ZIPX_ERR_LIMIT_NAME, in,
                        "path component is too long");
      }
      out[out_len++] = '/';
      levels++;
      seg_len = 0;
      i++;
      continue;
    }
    if(out_len + 2 >= out_size) {
      return szx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
    }
    out[out_len++] = ch;
    seg_len++;
    i++;
  }

  if(!seg_len) {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(seg_len == 1 && out[out_len - 1] == '.') {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
  }
  if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
    return szx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
  }
  if(seg_len > c->limits.max_name_len) {
    return szx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "path component is too long");
  }
  out[out_len] = 0;
  levels++;

  if(levels > c->limits.max_depth) {
    return szx_fail(c, ZIPX_ERR_LIMIT_DEPTH, in, "entry path is too deep");
  }
  *depth = levels;
  return 0;
}

/**************************************************************************
 * staging helpers (same as zip_extract.c / rar_extract.c)
 **************************************************************************/

static int
szx_path_parent(const char *path, char *out, size_t out_size) {
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
  if(!i || i >= out_size) {
    return -1;
  }
  memcpy(out, path, i);
  out[i] = 0;
  return 0;
}

static int
szx_make_staging(szx_ctx_t *c, const char *parent) {
  unsigned int attempt;

  for(attempt = 0; attempt < SZX_TMP_ATTEMPTS; attempt++) {
    int n = snprintf(c->staging, sizeof(c->staging), "%s/%s%ld-%lld-%u",
                     parent, SZX_STAGING_PREFIX, (long)getpid(),
                     (long long)time(NULL), attempt);

    if(n < 0 || (size_t)n >= sizeof(c->staging)) {
      return szx_fail(c, ZIPX_ERR_INTERNAL, NULL, "staging path is too long");
    }
    if(!mkdir(c->staging, 0777)) {
      c->staging_created = 1;
      return 0;
    }
    if(errno != EEXIST) {
      return szx_fail(c, ZIPX_ERR_IO, c->staging, "cannot create staging: %s",
                      strerror(errno));
    }
  }
  return szx_fail(c, ZIPX_ERR_IO, parent, "cannot create staging directory");
}

static void
szx_remove_tree(const char *path) {
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
      szx_remove_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(dir);
  rmdir(path);
}

static void
szx_cleanup_staging(szx_ctx_t *c) {
  if(c->staging_created) {
    szx_remove_tree(c->staging);
    c->staging_created = 0;
  }
}

static void
szx_rollback_published(szx_ctx_t *c) {
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

/* Creates every directory between the staging root and `rel`, but not rel
   itself.  Names were validated during scan, so no segment can escape. */
static int
szx_make_dirs(szx_ctx_t *c, const char *rel) {
  char buf[ZIPX_PATH_MAX];
  char full[ZIPX_PATH_MAX];
  size_t i;

  if(snprintf(buf, sizeof(buf), "%s", rel) >= (int)sizeof(buf)) {
    return szx_fail(c, ZIPX_ERR_LIMIT_NAME, rel, "path is too long");
  }
  for(i = 0; buf[i]; i++) {
    char saved;

    if(buf[i] != '/') {
      continue;
    }
    saved = buf[i];
    buf[i] = 0;
    if(snprintf(full, sizeof(full), "%s/%s", c->staging, buf) <
         (int)sizeof(full) &&
       mkdir(full, 0777) && errno != EEXIST) {
      return szx_fail(c, ZIPX_ERR_IO, full, "cannot create directory: %s",
                      strerror(errno));
    }
    buf[i] = saved;
  }
  return 0;
}

static int
szx_check_space(szx_ctx_t *c, const char *target) {
  struct statvfs vfs;
  unsigned long long available;
  unsigned long long block;
  unsigned long long required = c->bytes_total +
                                c->entries_total * SZX_SPACE_SLACK_PER_ENTRY;

  if(statvfs(target, &vfs)) {
    return szx_fail(c, ZIPX_ERR_SPACE, target, "cannot read free space: %s",
                    strerror(errno));
  }
  block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  available = (unsigned long long)vfs.f_bavail * block;
  if(available < required) {
    return szx_fail(c, ZIPX_ERR_SPACE, target,
                    "not enough space, required %llu bytes, available %llu bytes",
                    required, available);
  }
  return 0;
}

/**************************************************************************
 * 7z entry inspection
 **************************************************************************/

/* UTF-16LE (the 7z name table encoding) to UTF-8.  Returns -1 when the result
   would not fit, so the caller can refuse the entry instead of writing a name
   that does not match the archive.  Unpaired surrogates become U+FFFD: a
   mangled name is still a name, and the entry may be perfectly good. */
static int
szx_utf16_to_utf8(const UInt16 *src, char *dst, size_t dst_size) {
  size_t out = 0;

  while(*src) {
    UInt32 c = *src++;

    if(c >= 0xD800 && c <= 0xDBFF) {
      if(*src >= 0xDC00 && *src <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (*src++ - 0xDC00);
      } else {
        c = 0xFFFD;
      }
    } else if(c >= 0xDC00 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if(c < 0x80) {
      if(out + 1 >= dst_size) {
        return -1;
      }
      dst[out++] = (char)c;
    } else if(c < 0x800) {
      if(out + 2 >= dst_size) {
        return -1;
      }
      dst[out++] = (char)(0xC0 | (c >> 6));
      dst[out++] = (char)(0x80 | (c & 0x3F));
    } else if(c < 0x10000) {
      if(out + 3 >= dst_size) {
        return -1;
      }
      dst[out++] = (char)(0xE0 | (c >> 12));
      dst[out++] = (char)(0x80 | ((c >> 6) & 0x3F));
      dst[out++] = (char)(0x80 | (c & 0x3F));
    } else {
      if(out + 4 >= dst_size) {
        return -1;
      }
      dst[out++] = (char)(0xF0 | (c >> 18));
      dst[out++] = (char)(0x80 | ((c >> 12) & 0x3F));
      dst[out++] = (char)(0x80 | ((c >> 6) & 0x3F));
      dst[out++] = (char)(0x80 | (c & 0x3F));
    }
  }
  dst[out] = 0;
  return 0;
}

/* Converts entry `index`'s name into `out`.  Returns non-zero with the message
   already set when the name is unusable. */
static int
szx_entry_name(const CSzArEx *db, UInt32 index, char *out, size_t out_size,
               szx_ctx_t *c) {
  size_t units = SzArEx_GetFileNameUtf16(db, index, NULL);
  UInt16 *wide;

  if(units == 0) {
    return szx_fail(c, ZIPX_ERR_FORMAT, NULL, "entry %u has an empty name",
                    (unsigned)index);
  }
  wide = (UInt16 *)malloc((units + 1) * sizeof(UInt16));
  if(!wide) {
    return szx_fail(c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
  }
  SzArEx_GetFileNameUtf16(db, index, wide);
  if(szx_utf16_to_utf8(wide, out, out_size) == 0 && out[0]) {
    free(wide);
    return 0;
  }
  free(wide);
  return szx_fail(c, ZIPX_ERR_LIMIT_NAME, NULL,
                  "entry %u has a name that is empty or too long",
                  (unsigned)index);
}

/* The 7z attribute word: DOS bits in the low half, Unix mode in the high half
   when bit 15 is set.  Returns 0 when the archive stores no attributes. */
static UInt32
szx_entry_attrib(const CSzArEx *db, UInt32 index) {
  if(!SzBitWithVals_Check(&db->Attribs, index)) {
    return 0;
  }
  return db->Attribs.Vals[index];
}

static UInt32
szx_unix_mode(UInt32 attrib) {
  return (attrib & SZX_ATTRIB_UNIX_EXTENSION) ? (attrib >> 16) : 0;
}

static int
szx_entry_is_dir(const CSzArEx *db, UInt32 index, UInt32 attrib) {
  UInt32 mode;

  if(SzArEx_IsDir(db, index)) {
    return 1;
  }
  if(attrib & SZX_ATTRIB_DIRECTORY) {
    return 1;
  }
  mode = szx_unix_mode(attrib);
  return (mode && (mode & S_IFMT) == S_IFDIR) ? 1 : 0;
}

static uint64_t
szx_entry_size(const CSzArEx *db, UInt32 index) {
  return (uint64_t)(db->UnpackPositions[(size_t)index + 1] -
                    db->UnpackPositions[index]);
}

/* -1 when the archive stores no time for this entry. */
static time_t
szx_entry_mtime(const CSzArEx *db, UInt32 index) {
  UInt64 ft;

  if(!SzBitWithVals_Check(&db->MTime, index)) {
    return (time_t)-1;
  }
  ft = ((UInt64)db->MTime.Vals[index].High << 32) | db->MTime.Vals[index].Low;
  if(ft < SZX_FILETIME_UNIX_DELTA * 10000000ULL) {
    return (time_t)-1;
  }
  return (time_t)(ft / 10000000ULL - SZX_FILETIME_UNIX_DELTA);
}

/* Applies the creation mode and modification time to a staged object.
 *
 * The mode is always 0777, unconditionally - that matches the ZIP engine, which
 * has always called chmod_0777_fd() on everything it creates. This used to
 * apply the archive's Unix mode when it carried one, which sounds reasonable
 * and is useless in practice: a 7z written on Windows (the normal case for a
 * repacked game) stores no Unix mode at all, so nothing was applied and the
 * file simply kept the umask default of 0666/0644 - a game that will not run.
 * Honouring a stored 0644 was just as wrong for the same reason.
 *
 * Both are best effort: a filesystem that refuses chmod (FAT/exFAT) must not
 * fail an otherwise good extraction. */
static void
szx_apply_metadata(szx_ctx_t *c, const char *rel, UInt32 attrib, time_t mtime) {
  char full[ZIPX_PATH_MAX];

  (void)attrib;   /* still passed in by the call sites; the mode no longer
                     depends on it - see the comment above */
  if(snprintf(full, sizeof(full), "%s/%s", c->staging, rel) >=
     (int)sizeof(full)) {
    return;
  }
  (void)nx_chmod_create(full);   /* one implementation point; skips FAT/exFAT */
  if(mtime != (time_t)-1) {
    struct timeval tv[2];

    tv[0].tv_sec = mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    (void)utimes(full, tv);
  }
}

/**************************************************************************
 * scan phase
 **************************************************************************/

/* A first pass over the whole archive that writes nothing: it validates every
   name, rejects the entry types and limits we cannot honour, and totals the
   bytes so the free-space check happens before a single file is created.
   Anything that is going to fail should fail here, not three gigabytes in. */
static int
scan_entries(const CSzArEx *db, szx_ctx_t *c) {
  szx_nameset_t set;
  UInt32 i;
  int ret = 0;

  if(szx_nameset_init(&set, db->NumFiles ? db->NumFiles : 16)) {
    return szx_fail(c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
  }

  for(i = 0; i < db->NumFiles; i++) {
    char raw[SZX_NAME_MAX];
    char name[ZIPX_PATH_MAX];
    UInt32 attrib;
    UInt32 mode;
    uint64_t size;
    uint32_t depth = 0;
    int is_dir;
    int rc;

    if(szx_entry_name(db, i, raw, sizeof(raw), c)) {
      ret = (int)c->result->status;
      break;
    }
    attrib = szx_entry_attrib(db, i);
    mode = szx_unix_mode(attrib);
    is_dir = szx_entry_is_dir(db, i, attrib);
    size = szx_entry_size(db, i);

    rc = szx_normalize_name(raw, name, sizeof(name), &depth, c);
    (void)depth;
    if(rc) {
      ret = rc;
      break;
    }

    /* 7-Zip records a symbolic link as a file whose bytes are the target
       path, with S_IFLNK in the attribute word.  Honouring one would need
       link(), which is not usable for a plain unzip target on the PS5
       filesystem; dropping the type would silently turn the link into a
       regular file holding a path.  Refuse it by name instead. */
    if((mode & S_IFMT) == S_IFLNK) {
      ret = szx_fail(c, ZIPX_ERR_SPECIAL, name, "symbolic link entry");
      break;
    }
    if((mode & S_IFMT) && (mode & S_IFMT) != S_IFREG &&
       (mode & S_IFMT) != S_IFDIR) {
      ret = szx_fail(c, ZIPX_ERR_SPECIAL, name, "special file entry");
      break;
    }

    if(szx_nameset_check_duplicate(&set, name, is_dir, c) ||
       szx_nameset_add_path(&set, name, is_dir, c)) {
      ret = (int)c->result->status;
      break;
    }

    if(!is_dir && size) {
      if(size > c->limits.max_file_bytes) {
        ret = szx_fail(c, ZIPX_ERR_LIMIT_FILE, name,
                       "entry is larger than %llu bytes",
                       (unsigned long long)c->limits.max_file_bytes);
        break;
      }
      if(size >= c->limits.max_total_bytes ||
         c->bytes_total > c->limits.max_total_bytes - size) {
        ret = szx_fail(c, ZIPX_ERR_LIMIT_TOTAL, name,
                       "archive contents are larger than %llu bytes",
                       (unsigned long long)c->limits.max_total_bytes);
        break;
      }
      c->bytes_total += size;
    }

    c->entries_total++;
    if(c->entries_total > c->limits.max_entries) {
      ret = szx_fail(c, ZIPX_ERR_LIMIT_ENTRIES, name,
                     "archive has more than %llu entries",
                     (unsigned long long)c->limits.max_entries);
      break;
    }
    /* Once per ~256 entries is plenty for a header walk that only needs to
       keep the spinner alive; szx_report() also rate-limits at 200 ms. */
    if(c->entries_total % 256 == 0) {
      if(szx_canceled(c)) {
        ret = szx_fail(c, ZIPX_ERR_CANCELED, name, NULL);
        break;
      }
      szx_report(c, ZIPX_PHASE_SCAN, name, 0);
    }
  }

  /* For archives below the per-N-entry report threshold the scan never fires a
     throttled callback; push the final totals out so the UI sees total/entries
     populated before extraction begins. */
  szx_report(c, ZIPX_PHASE_SCAN, NULL, 1);

  szx_nameset_free(&set);
  return ret;
}

/* Fills `pack_positions` for a folder, after checking that the folder does not
   use more packed streams than the chain builder can hold. */
static int
szx_folder_packs(const CSzArEx *db, UInt32 folder, uint64_t *pack_positions,
                 UInt32 *pack_count, szx_ctx_t *c) {
  const UInt32 first = db->db.FoStartPackStreamIndex[folder];
  const UInt32 count = db->db.FoStartPackStreamIndex[(size_t)folder + 1] - first;
  UInt32 k;

  if(count > SZ_CHAIN_MAX_STREAMS) {
    return szx_fail(c, ZIPX_ERR_UNSUPPORTED, NULL,
                    "folder %u uses %u packed streams, at most %d are "
                    "supported",
                    (unsigned)folder, (unsigned)count, SZ_CHAIN_MAX_STREAMS);
  }
  for(k = 0; k <= count; k++) {
    pack_positions[k] = db->db.PackPositions[first + k];
  }
  *pack_count = count;
  return 0;
}

/* Walk every folder and parse its coder chain once, without decoding:
     * a coder graph we cannot drive is reported by folder and by chain name,
       before any output exists;
     * a folder that needs a password is reported the same way, so a protected
       archive does not first waste a whole extraction pass.
   The chains are freed again; the extract phase re-parses them per folder, so
   an archive with a million folders never holds more than one at a time. */
static int
precheck_folders(const CSzArEx *db, szx_ctx_t *c) {
  UInt32 folder;

  /* The decode stack for every folder has to round-trip here before the first
     byte is written, so a multi-thousand-folder archive does not look stalled
     between the entry scan ending and extraction starting. */
  szx_report(c, ZIPX_PHASE_SCAN, "validating folders", 1);
  for(folder = 0; folder < db->db.NumFolders; folder++) {
    uint64_t pack_positions[SZ_CHAIN_MAX_STREAMS + 1];
    UInt32 pack_count = 0;
    const uint8_t *blob = db->db.CodersData + db->db.FoCodersOffsets[folder];
    size_t blob_size = db->db.FoCodersOffsets[(size_t)folder + 1] -
                       db->db.FoCodersOffsets[folder];
    const uint64_t *cus =
        &db->db.CoderUnpackSizes[db->db.FoToCoderUnpackSizes[folder]];
    uint64_t unpack_size = SzAr_GetFolderUnpackSize(&db->db, folder);
    sz_chain *chain = NULL;
    sz_chain_err_t cerr;
    char desc[256];

    if(szx_folder_packs(db, folder, pack_positions, &pack_count, c)) {
      return (int)c->result->status;
    }
    if(sz_chain_parse(&chain, blob, blob_size, pack_positions, pack_count, cus,
                      unpack_size, sz_chain_default_limits(), &cerr) != 0) {
      return szx_fail(c,
                      (cerr.status == SZ_CHAIN_ERR_METHOD ||
                       cerr.status == SZ_CHAIN_ERR_LAYOUT)
                          ? ZIPX_ERR_UNSUPPORTED
                          : ZIPX_ERR_FORMAT,
                      NULL, "folder %u: %s", (unsigned)folder, cerr.message);
    }
    sz_chain_describe(chain, desc, sizeof(desc));

    if(sz_chain_needs_password(chain) && (!c->password || !c->password[0])) {
      sz_chain_free(chain);
      return szx_fail(c, ZIPX_ERR_PASSWORD, NULL,
                      "the archive is encrypted: folder %u holds %s and needs "
                      "a password",
                      (unsigned)folder, desc);
    }
    sz_chain_free(chain);
  }
  return 0;
}

/* A solid folder's ratio cannot be judged entry by entry: the packed stream
   belongs to all of them.  Comparing the folder's declared output against the
   bytes it actually occupies is the honest screen, and it is the one that
   catches a decompression bomb before the disk fills. */
static int
check_folder_ratio(const CSzArEx *db, UInt32 folder, szx_ctx_t *c) {
  uint64_t unpacked = SzAr_GetFolderUnpackSize(&db->db, folder);
  const UInt32 first = db->db.FoStartPackStreamIndex[folder];
  const UInt32 last = db->db.FoStartPackStreamIndex[(size_t)folder + 1];
  uint64_t packed;

  if(!c->limits.max_ratio || !unpacked ||
     unpacked < c->limits.ratio_min_bytes) {
    return 0;
  }
  /* The vendored decoder subset does not include SzArEx_GetFolderFullPackSize,
     and the folder's packed extent is a plain difference of PackPositions
     anyway. */
  packed = (uint64_t)(db->db.PackPositions[last] - db->db.PackPositions[first]);
  if(!packed) {
    return 0;
  }
  if(unpacked > packed * (uint64_t)c->limits.max_ratio) {
    return szx_fail(c, ZIPX_ERR_LIMIT_RATIO, NULL,
                    "folder %u expands %llu bytes into %llu, above the %u:1 "
                    "ratio limit",
                    (unsigned)folder, (unsigned long long)packed,
                    (unsigned long long)unpacked, c->limits.max_ratio);
  }
  return 0;
}

/**************************************************************************
 * extract phase
 **************************************************************************/

typedef struct {
  UInt32 file_index;
  uint64_t size;
} szx_plan_slot_t;

typedef struct {
  szx_ctx_t *c;
  const CSzArEx *db;

  /* The ordered entries of the folder being decoded, plus how far into them
     the byte stream has reached.  Exactly one folder is live at a time. */
  szx_plan_slot_t *plan;
  size_t plan_len;
  size_t plan_pos;

  uint64_t written;
  int fd;
  char cur_name[ZIPX_PATH_MAX];
  uint32_t entry_crc;
} szx_sink_t;

/* Reads `size` bytes of packed data at `offset`, which is counted from the
   first packed byte of the archive (db.dataPos). */
typedef struct {
  ISeekInStream *stream;
  UInt64 base;
} szx_reader_t;

static int
szx_read_at(void *ctx, uint64_t offset, void *dst, size_t size) {
  szx_reader_t *r = (szx_reader_t *)ctx;
  Int64 pos = (Int64)(r->base + offset);
  size_t done = 0;

  if(r->stream->Seek(r->stream, &pos, SZ_SEEK_SET) != SZ_OK) {
    return -1;
  }
  while(done < size) {
    size_t want = size - done;

    if(r->stream->Read(r->stream, (Byte *)dst + done, &want) != SZ_OK) {
      return -1;
    }
    if(want == 0) {
      return -1;
    }
    done += want;
  }
  return 0;
}

static void
szx_plan_free(szx_sink_t *s) {
  free(s->plan);
  s->plan = NULL;
  s->plan_len = s->plan_pos = 0;
}

/* Collects the non-empty entries of `folder` in stream order. */
static int
szx_plan_build(szx_sink_t *s, UInt32 folder) {
  const CSzArEx *db = s->db;
  UInt32 first = db->FolderToFile[folder];
  UInt32 last = db->FolderToFile[(size_t)folder + 1];
  UInt32 i;

  szx_plan_free(s);
  /* A folder abandoned mid-entry leaves `written` pointing into a file it
     never finished; carrying that into the next folder would make every later
     entry look partly written. */
  s->written = 0;
  if(last <= first) {
    return 0;
  }
  s->plan = (szx_plan_slot_t *)malloc(sizeof(szx_plan_slot_t) *
                                      (size_t)(last - first));
  if(!s->plan) {
    return szx_fail(s->c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
  }

  for(i = first; i < last; i++) {
    uint64_t size = szx_entry_size(db, i);

    if(db->FileToFolder[i] != folder || size == 0) {
      continue;
    }
    s->plan[s->plan_len].file_index = i;
    s->plan[s->plan_len].size = size;
    s->plan_len++;
  }
  return 0;
}

static void
szx_sink_close(szx_sink_t *s) {
  if(s->fd >= 0) {
    close(s->fd);
    s->fd = -1;
  }
}

/* Opens the next entry of the plan inside the staging tree. */
static int
szx_sink_open(szx_sink_t *s, UInt32 file_index) {
  char raw[SZX_NAME_MAX];
  char rel[ZIPX_PATH_MAX];
  char full[ZIPX_PATH_MAX];
  uint32_t depth = 0;

  if(szx_entry_name(s->db, file_index, raw, sizeof(raw), s->c)) {
    return -1;
  }
  /* The name was validated during scan, so this can only fail if the archive
     changed underneath us; still, never write a path we have not checked. */
  if(szx_normalize_name(raw, rel, sizeof(rel), &depth, s->c)) {
    return -1;
  }
  if(szx_make_dirs(s->c, rel)) {
    return -1;
  }
  if(snprintf(full, sizeof(full), "%s/%s", s->c->staging, rel) >=
     (int)sizeof(full)) {
    return szx_fail(s->c, ZIPX_ERR_LIMIT_NAME, rel, "path is too long");
  }

  s->fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if(s->fd < 0) {
    int saved = errno;

    return szx_fail(s->c, ZIPX_ERR_IO, rel, "cannot create: %s (errno=%d)",
                    strerror(saved), saved);
  }
  snprintf(s->cur_name, sizeof(s->cur_name), "%s", rel);
  s->entry_crc = CRC_INIT_VAL;
  return 0;
}

/* The sink handed to sz_chain_decode().  A folder's decoded bytes are a single
   stream that has to be split across the entries living in it, which is
   exactly what makes a solid block solid. */
static int
szx_sink_write(void *ctx, const void *data, size_t size) {
  szx_sink_t *s = (szx_sink_t *)ctx;
  const uint8_t *p = (const uint8_t *)data;

  while(size > 0) {
    szx_plan_slot_t *e;
    uint64_t remain;
    size_t take;

    if(s->plan_pos >= s->plan_len) {
      return szx_fail(s->c, ZIPX_ERR_FORMAT, NULL,
                      "folder produced %llu bytes more than its entries hold",
                      (unsigned long long)size);
    }
    e = &s->plan[s->plan_pos];
    remain = e->size - s->written;
    take = (size_t)((uint64_t)size < remain ? (uint64_t)size : remain);

    if(s->fd < 0 && szx_sink_open(s, e->file_index)) {
      return -1;
    }
    if(take) {
      size_t done = 0;

      while(done < take) {
        ssize_t n = write(s->fd, p + done, take - done);

        if(n <= 0) {
          if(n < 0 && errno == EINTR) {
            continue;
          }
          return szx_fail(s->c, ZIPX_ERR_IO, s->cur_name, "cannot write: %s",
                          strerror(errno));
        }
        done += (size_t)n;
      }
      s->entry_crc = CrcUpdate(s->entry_crc, p, take);
    }
    s->written += take;
    s->c->bytes_done += take;
    p += take;
    size -= take;
    szx_report(s->c, ZIPX_PHASE_EXTRACT, s->cur_name, 0);

    if(s->written == e->size) {
      UInt32 index = e->file_index;
      UInt32 attrib = szx_entry_attrib(s->db, index);

      szx_sink_close(s);
      if(SzBitWithVals_Check(&s->db->CRCs, index) &&
         CRC_GET_DIGEST(s->entry_crc) != s->db->CRCs.Vals[index]) {
        return szx_fail(s->c, ZIPX_ERR_CRC, s->cur_name,
                        "checksum mismatch in entry data");
      }
      szx_apply_metadata(s->c, s->cur_name, attrib,
                         szx_entry_mtime(s->db, index));
      if(szx_entry_is_dir(s->db, index, attrib)) {
        s->c->dirs_created++;
      } else {
        s->c->files_created++;
      }
      s->c->entries_done++;
      s->plan_pos++;
      s->written = 0;
    }
  }
  return 0;
}

/* Maps a sz_chain failure onto the zipx contract.  7zAES gets its own code:
   "password required or wrong" is actionable, "corrupt archive" is not.  A
   wrong password decrypts to plausible rubbish, so an encrypted folder whose
   data then fails to decode is reported as a password problem, with the same
   hint the chain itself appends. */
static int
szx_decode_error(szx_ctx_t *c, UInt32 folder, const sz_chain_err_t *derr,
                 const char *desc, int encrypted) {
  const char *detail = encrypted ? c->sevenz_path : NULL;

  switch(derr->status) {
  case SZ_CHAIN_ERR_PASSWORD:
    return szx_fail(c, ZIPX_ERR_PASSWORD, detail, "folder %u (%s): %s",
                    (unsigned)folder, desc, derr->message);
  case SZ_CHAIN_ERR_METHOD:
  case SZ_CHAIN_ERR_LAYOUT:
    return szx_fail(c, ZIPX_ERR_UNSUPPORTED, NULL, "folder %u (%s): %s",
                    (unsigned)folder, desc,
                    sz_chain_status_string(derr->status));
  case SZ_CHAIN_ERR_LIMIT:
    return szx_fail(c, ZIPX_ERR_LIMIT_FILE, NULL, "folder %u (%s): %s",
                    (unsigned)folder, desc, derr->message);
  case SZ_CHAIN_ERR_CANCELED:
    return szx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
  case SZ_CHAIN_ERR_MEM:
    return szx_fail(c, ZIPX_ERR_INTERNAL, NULL, "folder %u (%s): %s",
                    (unsigned)folder, desc, derr->message);
  case SZ_CHAIN_ERR_DATA:
    if(encrypted) {
      return szx_fail(c, ZIPX_ERR_PASSWORD, detail,
                      "folder %u (%s): %s (the password is wrong, or the "
                      "archive is damaged)",
                      (unsigned)folder, desc, derr->message);
    }
    return szx_fail(c, ZIPX_ERR_FORMAT, NULL, "folder %u (%s): %s",
                    (unsigned)folder, desc, derr->message);
  default:
    return szx_fail(c, ZIPX_ERR_FORMAT, NULL, "folder %u (%s): %s",
                    (unsigned)folder, desc, derr->message);
  }
}

/* Decodes every folder into the staging tree. */
static int
extract_folders(const CSzArEx *db, szx_ctx_t *c, szx_reader_t *reader) {
  szx_sink_t sink;
  UInt32 folder;
  int ret = 0;

  memset(&sink, 0, sizeof(sink));
  sink.c = c;
  sink.db = db;
  sink.fd = -1;

  for(folder = 0; folder < db->db.NumFolders; folder++) {
    uint64_t pack_positions[SZ_CHAIN_MAX_STREAMS + 1];
    UInt32 pack_count = 0;
    const uint8_t *blob = db->db.CodersData + db->db.FoCodersOffsets[folder];
    size_t blob_size = db->db.FoCodersOffsets[(size_t)folder + 1] -
                       db->db.FoCodersOffsets[folder];
    const uint64_t *cus =
        &db->db.CoderUnpackSizes[db->db.FoToCoderUnpackSizes[folder]];
    uint64_t unpack_size = SzAr_GetFolderUnpackSize(&db->db, folder);
    sz_chain *chain = NULL;
    sz_chain_err_t cerr;
    char desc[256];
    uint32_t folder_crc = 0;
    int encrypted = 0;

    if(szx_canceled(c)) {
      ret = szx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
      break;
    }
    if(check_folder_ratio(db, folder, c)) {
      ret = (int)c->result->status;
      break;
    }
    if(szx_folder_packs(db, folder, pack_positions, &pack_count, c)) {
      ret = (int)c->result->status;
      break;
    }
    if(sz_chain_parse(&chain, blob, blob_size, pack_positions, pack_count, cus,
                      unpack_size, sz_chain_default_limits(), &cerr) != 0) {
      ret = szx_decode_error(c, folder, &cerr, "parse", 0);
      break;
    }
    sz_chain_describe(chain, desc, sizeof(desc));
    encrypted = sz_chain_needs_password(chain);

    /* An empty folder has no bytes to split and no entries to place; its
       (zero length) files arrive through the empty-entry pass below. */
    if(unpack_size == 0) {
      sz_chain_free(chain);
      continue;
    }
    if(szx_plan_build(&sink, folder)) {
      szx_plan_free(&sink);
      sz_chain_free(chain);
      ret = (int)c->result->status;
      break;
    }

    {
      /* A single plain LZMA2 coder (the default 7-Zip layout) goes through
         the SDK's parallel decoder; everything else -- BCJ2 chains, encrypted
         folders, exotic method stacks -- stays on the chain walk.  A thread
         failure downgrades to the chain path, so the MT decoder can only
         ever add speed, never take correctness away. */
      uint8_t lzma2_prop = 0;
      uint64_t lzma2_in = 0;
      int decoded = 0;

      if(!encrypted && sz_chain_lzma2_root(chain, &lzma2_prop, &lzma2_in)) {
        sz_chain_err_t merr;
        int mrc = szx_mt_decode(szx_read_at, reader, pack_positions[0],
                                lzma2_in, lzma2_prop, unpack_size,
                                szx_sink_write, &sink, szx_cancel_cb, c,
                                &folder_crc, &merr);
        if(mrc == 0) {
          decoded = 1;
        } else if(mrc != SZX_MT_ERR_THREADS) {
          if(c->result->status == ZIPX_OK) {
            szx_decode_error(c, folder, &merr, desc, encrypted);
          }
          szx_sink_close(&sink);
          szx_plan_free(&sink);
          sz_chain_free(chain);
          ret = (int)c->result->status;
          break;
        }
      }

      if(!decoded &&
         sz_chain_decode(chain, szx_read_at, reader, szx_sink_write, &sink,
                         NULL, NULL, c->password, &folder_crc, &cerr) != 0) {
        /* A failure the sink already explained wins over the decoder's summary,
           which can only say that the sink rejected data. */
        if(c->result->status == ZIPX_OK) {
          szx_decode_error(c, folder, &cerr, desc, encrypted);
        }
        szx_sink_close(&sink);
        szx_plan_free(&sink);
        sz_chain_free(chain);
        ret = (int)c->result->status;
        break;
      }
    }
    szx_sink_close(&sink);

    if(sink.plan_pos != sink.plan_len) {
      ret = szx_fail(c, ZIPX_ERR_FORMAT, NULL,
                     "folder %u: only %u of %u entries were produced",
                     (unsigned)folder, (unsigned)sink.plan_pos,
                     (unsigned)sink.plan_len);
      szx_plan_free(&sink);
      sz_chain_free(chain);
      break;
    }
    if(SzBitWithVals_Check(&db->db.FolderCRCs, folder) &&
       folder_crc != db->db.FolderCRCs.Vals[folder]) {
      ret = szx_fail(c, ZIPX_ERR_CRC, NULL,
                     "folder %u: checksum mismatch (%08X, expected %08X)",
                     (unsigned)folder, (unsigned)folder_crc,
                     (unsigned)db->db.FolderCRCs.Vals[folder]);
      szx_plan_free(&sink);
      sz_chain_free(chain);
      break;
    }

    szx_plan_free(&sink);
    sz_chain_free(chain);
  }

  szx_plan_free(&sink);
  return ret;
}

/* Creates everything a folder stream does not carry: the directories and the
   zero-byte files.  They have no bytes to place, so they go straight into the
   staging tree. */
static int
extract_empty_entries(const CSzArEx *db, szx_ctx_t *c) {
  UInt32 i;

  for(i = 0; i < db->NumFiles; i++) {
    char raw[SZX_NAME_MAX];
    char rel[ZIPX_PATH_MAX];
    char full[ZIPX_PATH_MAX];
    UInt32 attrib = szx_entry_attrib(db, i);
    uint32_t depth = 0;
    int is_dir = szx_entry_is_dir(db, i, attrib);

    if(szx_entry_size(db, i) != 0) {
      continue;
    }
    if(szx_entry_name(db, i, raw, sizeof(raw), c)) {
      return (int)c->result->status;
    }
    if(szx_normalize_name(raw, rel, sizeof(rel), &depth, c)) {
      return (int)c->result->status;
    }
    (void)depth;

    if(szx_make_dirs(c, rel)) {
      return (int)c->result->status;
    }
    if(snprintf(full, sizeof(full), "%s/%s", c->staging, rel) >=
       (int)sizeof(full)) {
      return szx_fail(c, ZIPX_ERR_LIMIT_NAME, rel, "path is too long");
    }

    if(is_dir) {
      if(mkdir(full, 0777) && errno != EEXIST) {
        return szx_fail(c, ZIPX_ERR_IO, rel, "cannot create directory: %s",
                        strerror(errno));
      }
      c->dirs_created++;
    } else {
      int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);

      if(fd < 0) {
        return szx_fail(c, ZIPX_ERR_IO, rel, "cannot create: %s",
                        strerror(errno));
      }
      close(fd);
      c->files_created++;
    }
    szx_apply_metadata(c, rel, attrib, szx_entry_mtime(db, i));
    c->entries_done++;
    szx_report(c, ZIPX_PHASE_EXTRACT, rel, 0);
  }
  return 0;
}

/**************************************************************************
 * publish phase (identical to zip_extract.c / rar_extract.c)
 **************************************************************************/

static int szx_publish_dir(const char *src, const char *dst, int depth,
                           szx_ctx_t *c);

static int
szx_publish_entry(const char *src, const char *dst, const char *name, int depth,
                  szx_ctx_t *c) {
  char src_child[ZIPX_PATH_MAX];
  char dst_child[ZIPX_PATH_MAX];
  struct stat st;
  struct stat src_st;
  int src_is_dir;

  if(snprintf(src_child, sizeof(src_child), "%s/%s", src, name) >=
       (int)sizeof(src_child) ||
     snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, name) >=
       (int)sizeof(dst_child)) {
    return szx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "path is too long");
  }
  if(lstat(src_child, &src_st)) {
    return szx_fail(c, ZIPX_ERR_IO, src_child, "cannot read staging: %s",
                    strerror(errno));
  }
  src_is_dir = S_ISDIR(src_st.st_mode) ? 1 : 0;

  if(lstat(dst_child, &st)) {
    if(errno != ENOENT) {
      return szx_fail(c, ZIPX_ERR_IO, dst_child, "cannot check target: %s",
                      strerror(errno));
    }
    if(rename(src_child, dst_child)) {
      return szx_fail(c, ZIPX_ERR_IO, dst_child, "cannot publish: %s",
                      strerror(errno));
    }
    return szx_remember_published(c, dst_child, src_is_dir);
  }

  if(S_ISDIR(st.st_mode)) {
    if(!src_is_dir) {
      return szx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                      "file collides with an existing directory");
    }
    /* Both MERGE and OVERWRITE treat a matching directory tree as a recursion:
       the user's choice between them only matters at the file leaves (see
       below).  Refusing up front would make OVERWRITE useless for any archive
       that overlaps a directory already on disk. */
    if(c->conflict != ZIPX_CONFLICT_MERGE &&
       c->conflict != ZIPX_CONFLICT_OVERWRITE) {
      return szx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                      "directory already exists");
    }
    if(depth >= SZX_PUBLISH_MAX_DEPTH) {
      return szx_fail(c, ZIPX_ERR_LIMIT_DEPTH, dst_child, "path is too deep");
    }
    if(szx_publish_dir(src_child, dst_child, depth + 1, c)) {
      return -1;
    }
    rmdir(src_child);
    return 0;
  }

  if(S_ISREG(st.st_mode) && !src_is_dir) {
    if(c->conflict == ZIPX_CONFLICT_OVERWRITE) {
      if(rename(src_child, dst_child)) {
        return szx_fail(c, ZIPX_ERR_IO, dst_child, "cannot replace: %s",
                        strerror(errno));
      }
      return szx_remember_published(c, dst_child, 0);
    }
    if(c->conflict == ZIPX_CONFLICT_MERGE) {
      if(unlink(src_child)) {
        return szx_fail(c, ZIPX_ERR_IO, dst_child, "cannot drop staged file: %s",
                        strerror(errno));
      }
      return 0;
    }
  }
  return szx_fail(c, ZIPX_ERR_CONFLICT, dst_child, "target already exists");
}

static int
szx_publish_dir(const char *src, const char *dst, int depth, szx_ctx_t *c) {
  DIR *dir = opendir(src);
  struct dirent *ent;
  char **names = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t i;
  int ret = 0;

  if(!dir) {
    return szx_fail(c, ZIPX_ERR_IO, src, "cannot read staging: %s",
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
        ret = szx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
        break;
      }
      names = grown;
      cap = next;
    }
    if(!(names[count] = strdup(ent->d_name))) {
      ret = szx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
      break;
    }
    count++;
  }
  closedir(dir);

  for(i = 0; i < count && !ret; i++) {
    szx_report(c, ZIPX_PHASE_PUBLISH, names[i], 0);
    ret = szx_publish_entry(src, dst, names[i], depth, c);
  }
  for(i = 0; i < count; i++) {
    free(names[i]);
  }
  free(names);
  return ret;
}

static int
szx_publish_staging(szx_ctx_t *c, const char *dst_dir, int dst_existed) {
  int ret;

  szx_report(c, ZIPX_PHASE_PUBLISH, dst_dir, 1);
  if(!dst_existed) {
    if(rename(c->staging, dst_dir)) {
      return szx_fail(c, ZIPX_ERR_IO, dst_dir, "cannot publish: %s",
                      strerror(errno));
    }
    c->staging_created = 0;
    return 0;
  }
  ret = szx_publish_dir(c->staging, dst_dir, 0, c);
  if(ret) {
    szx_rollback_published(c);
  }
  return ret;
}

/**************************************************************************
 * public entry point
 **************************************************************************/

zipx_status_t
sevenz_extract(const char *sevenz_path, const char *dst_dir,
               zipx_conflict_t conflict, const zipx_limits_t *limits,
               zipx_cancel_fn cancel, zipx_progress_fn progress, void *userdata,
               const char *password, zipx_result_t *result) {
  szx_ctx_t ctx;
  szx_ctx_t *c = &ctx;
  char parent[ZIPX_PATH_MAX];
  char dst_copy[ZIPX_PATH_MAX];
  struct stat st;
  CLookToRead2 look_stream;
  Byte *look_buf = NULL;
  CSzArEx db;
  int db_open = 0;
  sevenz_volstream *vol = NULL;
  char *vol_err = NULL;
  char vol_desc[512] = "";
  szx_reader_t reader;
  SRes res;
  int dst_existed = 0;
  int status;
  static int tables_ready;
  szh_prep *hdr_prep = NULL;
  ISeekInStream *hdr_stream = NULL;
  char hdr_msg[256] = "";

  if(!result || !sevenz_path || !sevenz_path[0] || !dst_dir || !dst_dir[0]) {
    if(result) {
      memset(result, 0, sizeof(*result));
      result->status = ZIPX_ERR_INTERNAL;
      snprintf(result->message, sizeof(result->message), "invalid argument");
    }
    return ZIPX_ERR_INTERNAL;
  }

  memset(&ctx, 0, sizeof(ctx));
  memset(result, 0, sizeof(*result));
  c->result = result;
  c->conflict = conflict;
  c->limits = limits ? *limits : *zipx_default_limits();
  c->cancel = cancel;
  c->progress = progress;
  c->userdata = userdata;
  c->password = password;
  c->sevenz_path = sevenz_path;

  snprintf(dst_copy, sizeof(dst_copy), "%s", dst_dir);
  {
    size_t len = strlen(dst_copy);

    while(len > 1 && dst_copy[len - 1] == '/') {
      dst_copy[--len] = 0;
    }
  }
  if(szx_path_parent(dst_copy, parent, sizeof(parent))) {
    status = szx_fail(c, ZIPX_ERR_INTERNAL, dst_dir, "invalid destination");
    goto done;
  }
  if(stat(parent, &st) || !S_ISDIR(st.st_mode)) {
    status = szx_fail(c, ZIPX_ERR_IO, parent, "destination parent is missing");
    goto done;
  }
  dst_existed = !lstat(dst_copy, &st);
  if(dst_existed && !S_ISDIR(st.st_mode)) {
    status = szx_fail(c, ZIPX_ERR_CONFLICT, dst_copy,
                      "destination is not a directory");
    goto done;
  }

  if(sevenz_volstream_open(&vol, sevenz_path, NULL, &vol_err) != 0) {
    /* The volume detector knows which part is missing and names it; the
       generic message is only for the cases it has no opinion about. */
    status = szx_fail(c, ZIPX_ERR_OPEN, sevenz_path, "%s",
                      vol_err ? vol_err : "cannot open the archive");
    free(vol_err);
    goto done;
  }
  free(vol_err);
  sevenz_volstream_describe(vol, vol_desc, sizeof(vol_desc));

  if(!tables_ready) {
    CrcGenerateTable();
    tables_ready = 1;
  }
  LookToRead2_CreateVTable(&look_stream, 0);
  look_buf = (Byte *)ISzAlloc_Alloc(&g_sz_alloc, SZX_INPUT_BUF_SIZE);
  if(!look_buf) {
    status = szx_fail(c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
    goto done;
  }
  look_stream.buf = look_buf;
  look_stream.bufSize = SZX_INPUT_BUF_SIZE;

  /* An encrypted header (-mhe=on) hides the whole folder table, so it has to be
     decrypted before the SDK can read anything at all.  szh_prepare() reports
     SZH_PLAIN for every other archive, and then the SDK sees exactly the stream
     it always did. */
  hdr_stream = sevenz_volstream_stream(vol);
  switch(szh_prepare(&hdr_prep, hdr_stream, c->password, hdr_msg,
                     sizeof(hdr_msg))) {
  case SZH_PATCHED:
    hdr_stream = szh_stream(hdr_prep);
    break;
  case SZH_PLAIN:
    break;
  case SZH_ERR_PASSWORD:
    status = szx_fail(c, ZIPX_ERR_PASSWORD, sevenz_path, "%s: %s", vol_desc,
                      hdr_msg);
    goto done;
  case SZH_ERR_UNSUPPORTED:
    status = szx_fail(c, ZIPX_ERR_UNSUPPORTED, sevenz_path, "%s: %s", vol_desc,
                      hdr_msg);
    goto done;
  case SZH_ERR_IO:
    status = szx_fail(c, ZIPX_ERR_IO, sevenz_path, "%s: %s", vol_desc, hdr_msg);
    goto done;
  default:
    status = szx_fail(c, ZIPX_ERR_FORMAT, sevenz_path, "%s: %s", vol_desc,
                      hdr_msg);
    goto done;
  }
  look_stream.realStream = hdr_stream;
  LookToRead2_INIT(&look_stream);
  {
    /* Inspecting the start header moved the stream around, and LookToRead2
       reads from wherever it finds the stream on its first call -- it does not
       seek.  Put it back at byte 0. */
    Int64 zero = 0;

    if(hdr_stream->Seek(hdr_stream, &zero, SZ_SEEK_SET) != SZ_OK) {
      status = szx_fail(c, ZIPX_ERR_IO, sevenz_path, "%s: seek failed",
                        vol_desc);
      goto done;
    }
  }

  SzArEx_Init(&db);
  res = SzArEx_Open(&db, &look_stream.vt, &g_sz_alloc, &g_sz_alloc);
  if(res != SZ_OK) {
    /* SzArEx_Open already released everything it allocated. */
    if(res == SZ_ERROR_UNSUPPORTED) {
      status = szx_fail(c, ZIPX_ERR_UNSUPPORTED, sevenz_path,
                        "%s: the archive header is compressed with a method the "
                        "bundled decoder does not have",
                        vol_desc);
    } else {
      status = szx_fail(c, ZIPX_ERR_FORMAT, sevenz_path,
                        "%s: cannot read the 7z header (code %d)", vol_desc,
                        (int)res);
    }
    goto done;
  }
  db_open = 1;

  if(scan_entries(&db, c)) {
    status = (int)c->result->status;
    goto done;
  }
  if(szx_canceled(c)) {
    status = szx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    goto done;
  }
  if(precheck_folders(&db, c)) {
    status = (int)c->result->status;
    goto done;
  }
  if(szx_check_space(c, dst_existed ? dst_copy : parent)) {
    status = (int)c->result->status;
    goto done;
  }
  if(szx_make_staging(c, parent)) {
    status = (int)c->result->status;
    goto done;
  }

  reader.stream = sevenz_volstream_stream(vol);
  reader.base = db.dataPos;

  if(extract_folders(&db, c, &reader)) {
    status = (int)c->result->status;
    goto done;
  }
  if(extract_empty_entries(&db, c)) {
    status = (int)c->result->status;
    goto done;
  }
  if(szx_canceled(c)) {
    status = szx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    goto done;
  }
  if(c->entries_done != c->entries_total) {
    status = szx_fail(c, ZIPX_ERR_FORMAT, NULL,
                      "%llu of %llu entries were written; the archive's entry "
                      "table and its folders disagree",
                      (unsigned long long)c->entries_done,
                      (unsigned long long)c->entries_total);
    goto done;
  }

  SzArEx_Free(&db, &g_sz_alloc);
  db_open = 0;
  sevenz_volstream_free(vol);
  vol = NULL;

  if(szx_publish_staging(c, dst_copy, dst_existed)) {
    status = (int)c->result->status;
    goto done;
  }

  result->status = ZIPX_OK;
  status = ZIPX_OK;

done:
  if(db_open) {
    SzArEx_Free(&db, &g_sz_alloc);
  }
  if(look_buf) {
    ISzAlloc_Free(&g_sz_alloc, look_buf);
  }
  szh_prep_free(hdr_prep);
  sevenz_volstream_free(vol);
  szx_cleanup_staging(c);
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
  szx_report(c, ZIPX_PHASE_CLEANUP, NULL, 1);
  szx_free_published(c);
  return result->status;
}
