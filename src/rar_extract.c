/* Safe RAR extraction engine used by the /api/extract task.
   Wraps the vendored rarlab UnRAR 7.x library (third_party/unrar7) through
   its C-compatible DLL API (dll.hpp / unrar_c_api.h facade).

   The publish / staging / rollback / normalize-name / dedup machinery is
   mirrored from zip_extract.c so that any consumer of zipx_result_t gets a
   consistent error and progress contract regardless of archive format.

   Backend notes (v1.9, unrar 7.20.1):
     * RAR4 and RAR5 (any compression version, incl. WinRAR 6/7 "v6")
       single-volume archives.
     * Multi-volume archives: unrar auto-merges subsequent volumes by name
       pattern when all .partNN.rar files sit next to the opened volume.
     * Encrypted archives (`-p` data encryption and `-hp` header encryption):
       the password is handed to the engine via RARSetPassword immediately
       after RAROpenArchiveEx and before the first RARReadHeaderEx, which is
       the order unrar needs to decrypt a RAR5 header. A missing or wrong
       password surfaces as ZIPX_ERR_PASSWORD.

   See third_party/unrar7/VENDORED.md for the full integration notes. */

#include "rar_extract.h"
#include "zip_extract.h"  /* for the shared status / progress / limits API */
#include "nx_fs.h"        /* NX_CREATE_MODE: unrar's own choice is not 0777 */

#include "unrar_c_api.h"  /* facade — links against the unrar7 static lib at build time */

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

/* (intentionally no `#include "rar.hpp"` etc. here — those are C++ headers.
   rar_extract.c talks to unrar exclusively through the extern "C" DLL API in
   dll.hpp, and the unrar sources are compiled as their own translation units
   (RARDLL mode) and joined at link time. This keeps the host test build's
   tests/posix_compat.h renames (open->wfm_open) from leaking into unrar.) */

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define RARX_TMP_ATTEMPTS 64
#define RARX_STAGING_PREFIX ".wfm-extract-"
#define RARX_PUBLISH_MAX_DEPTH 128
#define RARX_SPACE_SLACK_PER_ENTRY 512

/**************************************************************************
 * context + small helpers (mirror of zipx_ctx_*; names prefixed rarx_)
 **************************************************************************/

typedef struct {
  zipx_conflict_t conflict;
  zipx_limits_t limits;
  zipx_cancel_fn cancel;
  zipx_progress_fn progress;
  void *userdata;
  zipx_result_t *result;
  /* NULL when no password was supplied. Owned by the caller for the whole
     call; RARSetPassword copies it into the engine, so it never dangles. */
  const char *password;
  uint64_t entries_total;
  uint64_t entries_done;
  uint64_t bytes_total;
  uint64_t bytes_done;
  uint64_t files_created;
  uint64_t dirs_created;
  uint64_t progress_floor;
  /* Set by the UCM_LARGEDICT callback only: the dictionary the archive asks
     for and the limit we refuse above, both in KiB (0 = never raised, i.e. the
     archive's dictionary was within Cmd->WinSizeLimit). unrar hands these over
     as p1/p2, so the refusal can name the real numbers instead of blaming the
     entry that happened to be in flight. */
  uint64_t dict_kb;
  uint64_t dict_limit_kb;
  struct timespec last_report;
  char staging[ZIPX_PATH_MAX];
  char **created;
  size_t created_count;
  size_t created_cap;
  unsigned char *created_is_dir;
  int staging_created;
} rarx_ctx_t;

static void
rarx_set_detail(rarx_ctx_t *c, const char *path) {
  if(path) {
    snprintf(c->result->detail, sizeof(c->result->detail), "%s", path);
  }
}

static int
rarx_fail(rarx_ctx_t *c, zipx_status_t status, const char *detail,
          const char *fmt, ...) {
  va_list ap;

  c->result->status = status;
  c->result->sys_errno = errno;
  rarx_set_detail(c, detail);
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
mono_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
report(rarx_ctx_t *c, int phase, const char *current, int force) {
  zipx_progress_t p;
  long long now;

  if(!c->progress) {
    return;
  }
  now = mono_ms();
  if(!force && c->last_report.tv_sec &&
     now - ((long long)c->last_report.tv_sec * 1000 +
             0) < 200 &&
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
canceled(rarx_ctx_t *c) {
  return c->cancel && c->cancel(c->userdata);
}

static int
remember_published(rarx_ctx_t *c, const char *path, int is_dir) {
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
free_published(rarx_ctx_t *c) {
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
 * duplicate detection (FNV-1a hash table; same trick as zip_extract.c)
 **************************************************************************/

typedef struct {
  uint64_t *hash;
  unsigned char *is_dir;
  size_t cap;
  size_t count;
} rarx_nameset_t;

static uint64_t
rarx_fnv1a(const char *s, size_t len) {
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for(i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h ? h : 1;
}

static int
nameset_init(rarx_nameset_t *set, size_t hint) {
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
nameset_free(rarx_nameset_t *set) {
  free(set->hash);
  free(set->is_dir);
  set->hash = NULL;
  set->is_dir = NULL;
  set->cap = set->count = 0;
}

static size_t
nameset_slot(rarx_nameset_t *set, uint64_t h) {
  size_t mask = set->cap - 1;
  size_t i = (size_t)(h & mask);

  while(set->hash[i] && set->hash[i] != h) {
    i = (i + 1) & mask;
  }
  return i;
}

static int
nameset_grow(rarx_nameset_t *set) {
  rarx_nameset_t bigger;
  size_t i;

  if(nameset_init(&bigger, set->cap)) {
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

static int
nameset_get(rarx_nameset_t *set, uint64_t h) {
  size_t i = nameset_slot(set, h);
  if(!set->hash[i]) {
    return 0;
  }
  return set->is_dir[i] ? 1 : 2;
}

static int
nameset_put(rarx_nameset_t *set, uint64_t h, int is_dir) {
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
 * entry name validation (mirrors zip_extract.c::normalize_name but the
 * RAR side does not have a trailing separator, so dir entries are only
 * detected via the RHDF_DIRECTORY header flag).
 **************************************************************************/

static int
normalize_name(const char *in, char *out, size_t out_size, int *is_dir,
               uint32_t *depth, rarx_ctx_t *c) {
  size_t in_len = strlen(in);
  size_t out_len = 0;
  size_t i = 0;
  size_t seg_len = 0;
  uint32_t levels = 0;

  /* NOTE: is_dir is owned by the caller — unrar announces directories via
     the RHDF_DIRECTORY header flag, not via a trailing separator, so we
     must NOT clear it here (a stray `*is_dir = 0` previously turned every
     directory entry into a file and tripped the duplicate detector when an
     explicit directory header followed files beneath it). */
  *depth = 0;
  if(!in_len) {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(in_len > c->limits.max_path_len) {
    return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
  }
  if(in[0] == '/' || in[0] == '\\') {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "absolute entry name");
  }
  if(in_len >= 2 && ((in[0] >= 'A' && in[0] <= 'Z') ||
                     (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':') {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "drive letter in entry name");
  }

  while(i < in_len) {
    char ch = in[i];

    if((unsigned char)ch < 0x20 || (unsigned char)ch == 0x7f) {
      return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in,
                       "control character in entry name");
    }
    if(ch == '\\') {
      /* RAR technically permits '\' as a separator on Windows archives. We
         normalize to '/' so the staging tree is consistent. */
      ch = '/';
    }
    if(ch == '/') {
      if(!seg_len) {
        return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty path segment");
      }
      if(seg_len == 1 && out[out_len - 1] == '.') {
        return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
      }
      if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
        return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
      }
      if(seg_len > c->limits.max_name_len) {
        return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "path component is too long");
      }
      out[out_len++] = '/';
      levels++;
      seg_len = 0;
      i++;
      continue;
    }
    if(out_len + 2 >= out_size) {
      return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "entry name is too long");
    }
    out[out_len++] = ch;
    seg_len++;
    i++;
  }

  if(!seg_len) {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "empty entry name");
  }
  if(seg_len == 1 && out[out_len - 1] == '.') {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'.' path segment");
  }
  if(seg_len == 2 && !memcmp(out + out_len - 2, "..", 2)) {
    return rarx_fail(c, ZIPX_ERR_UNSAFE_NAME, in, "'..' path segment");
  }
  if(seg_len > c->limits.max_name_len) {
    return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, in, "path component is too long");
  }
  out[out_len] = 0;
  levels++;

  if(levels > c->limits.max_depth) {
    return rarx_fail(c, ZIPX_ERR_LIMIT_DEPTH, in, "entry path is too deep");
  }
  *depth = levels;
  return 0;
}

static int
nameset_add_path(rarx_nameset_t *set, const char *name, int is_dir,
                 rarx_ctx_t *c) {
  char buf[ZIPX_PATH_MAX];
  size_t len = strlen(name);
  size_t i;

  if(len >= sizeof(buf)) {
    return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "entry name is too long");
  }
  memcpy(buf, name, len + 1);

  for(i = 0; i < len; i++) {
    if(buf[i] != '/') {
      continue;
    }
    buf[i] = 0;
    if(nameset_get(set, rarx_fnv1a(buf, i)) == 2) {
      return rarx_fail(c, ZIPX_ERR_DUPLICATE, name,
                       "entry uses a file as a directory");
    }
    if(nameset_put(set, rarx_fnv1a(buf, i), 1)) {
      return rarx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
    }
    buf[i] = '/';
  }

  if(nameset_put(set, rarx_fnv1a(name, len), is_dir)) {
    return rarx_fail(c, ZIPX_ERR_INTERNAL, name, "out of memory");
  }
  return 0;
}

static int
nameset_check_duplicate(rarx_nameset_t *set, const char *name, int is_dir,
                        rarx_ctx_t *c) {
  int existing = nameset_get(set, rarx_fnv1a(name, strlen(name)));

  if(existing == 2 || (existing == 1 && !is_dir)) {
    return rarx_fail(c, ZIPX_ERR_DUPLICATE, name, "duplicate entry name");
  }
  return 0;
}

/**************************************************************************
 * unrar DLL error -> zipx error translation
 **************************************************************************/

static int
rar_translate_error(int code, const char *detail, rarx_ctx_t *c) {
  switch(code) {
  case ERAR_SUCCESS:
    return ZIPX_OK;

  /* The unrar engine handles multi-volume automatically (it merges the
     next .partNN.rar by name); a missing next volume surfaces as EOPEN. */
  case ERAR_EOPEN:
    return rarx_fail(c, ZIPX_ERR_OPEN, detail, "%s", strerror(errno));

  case ERAR_ECREATE:
  case ERAR_ECLOSE:
  case ERAR_EREAD:
  case ERAR_EWRITE:
    return rarx_fail(c, ZIPX_ERR_IO, detail, "%s", strerror(errno));

  case ERAR_MISSING_PASSWORD:
  case ERAR_BAD_PASSWORD:
    /* The archive needs a password we do not have, or the one supplied was
       wrong. ZIPX_ERR_PASSWORD lets the caller prompt and retry. */
    return rarx_fail(c, ZIPX_ERR_PASSWORD, detail,
                     "the archive is encrypted and the password is missing "
                     "or wrong");

  case ERAR_SMALL_BUF:
    return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, detail, "name buffer is too small");

  case ERAR_LARGE_DICT: {
    /* Not "entry too large": the *dictionary* is, and that is a property of the
       archive (RAR7 headers can ask for up to 64 GiB), not of the entry that
       happened to be in flight. Report both numbers; unrar gave us exactly
       these when it asked. */
    char need[96];

    if(c->dict_kb) {
      snprintf(need, sizeof(need), "%llu MiB (limit %llu MiB)",
               (unsigned long long)(c->dict_kb / 1024),
               (unsigned long long)(c->dict_limit_kb / 1024));
    } else {
      snprintf(need, sizeof(need), "more than 4096 MiB");
    }
    return rarx_fail(c, ZIPX_ERR_LIMIT_DICT, need,
                     "the archive needs a dictionary larger than this build "
                     "supports (%s)", need);
  }

  case ERAR_BAD_DATA:
    return rarx_fail(c, ZIPX_ERR_CRC, detail, "checksum mismatch in entry data");

  case ERAR_BAD_ARCHIVE:
  case ERAR_UNKNOWN_FORMAT:
  case ERAR_UNKNOWN:
  case ERAR_NO_MEMORY:
  default:
    return rarx_fail(c, ZIPX_ERR_FORMAT, detail,
                     "invalid or corrupt RAR archive");
  }
}

/**************************************************************************
 * staging helpers (same as zip_extract.c)
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
    return -1;
  }
  if(i >= out_size) {
    return -1;
  }
  memcpy(out, path, i);
  out[i] = 0;
  return 0;
}

static int
make_staging(rarx_ctx_t *c, const char *parent) {
  unsigned int attempt;

  for(attempt = 0; attempt < RARX_TMP_ATTEMPTS; attempt++) {
    int n = snprintf(c->staging, sizeof(c->staging), "%s/%s%ld-%lld-%u",
                     parent, RARX_STAGING_PREFIX, (long)getpid(),
                     (long long)time(NULL), attempt);

    if(n < 0 || (size_t)n >= sizeof(c->staging)) {
      return rarx_fail(c, ZIPX_ERR_INTERNAL, NULL, "staging path is too long");
    }
    if(!mkdir(c->staging, 0777)) {
      c->staging_created = 1;
      return 0;
    }
    if(errno != EEXIST) {
      return rarx_fail(c, ZIPX_ERR_IO, c->staging, "cannot create staging: %s",
                       strerror(errno));
    }
  }
  return rarx_fail(c, ZIPX_ERR_IO, parent, "cannot create staging directory");
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
cleanup_staging(rarx_ctx_t *c) {
  if(c->staging_created) {
    remove_tree(c->staging);
    c->staging_created = 0;
  }
}

/* Force everything under `path` to NX_CREATE_MODE, depth first so a directory
   is still writable while its children are visited.
 *
 * Needed because unrar writes the files itself: it applies the archive's own
 * attribute word, and a RAR made on Windows carries only DOS attributes, so
 * what lands is the umask default (0666/0644). On the console that is a game
 * that will not start. Walking the staging tree once, after extraction and
 * before publish, is the only place we get to decide - the ZIP engine reaches
 * the same end state by chmod-ing each entry as it creates it.
 *
 * Entirely best effort: a filesystem that refuses chmod (FAT/exFAT) must not
 * fail an otherwise good extraction, so nothing here is reported. */
static void
chmod_tree_0777(const char *path) {
  DIR           *dir;
  struct dirent *ent;

  (void)nx_chmod_create(path);
  dir = opendir(path);
  if(!dir) {
    return;
  }
  while((ent = readdir(dir))) {
    char        child[ZIPX_PATH_MAX];
    struct stat st;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
      continue;
    }
    if(snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
         (int)sizeof(child)) {
      continue;
    }
    if(!lstat(child, &st) && S_ISDIR(st.st_mode)) {
      chmod_tree_0777(child);
    } else {
      (void)nx_chmod_create(child);
    }
  }
  closedir(dir);
}

static void
rollback_published(rarx_ctx_t *c) {
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
check_space(rarx_ctx_t *c, const char *target) {
  struct statvfs vfs;
  unsigned long long available;
  unsigned long long block;
  unsigned long long required = c->bytes_total +
                                c->entries_total * RARX_SPACE_SLACK_PER_ENTRY;

  if(statvfs(target, &vfs)) {
    return rarx_fail(c, ZIPX_ERR_SPACE, target, "cannot read free space: %s",
                     strerror(errno));
  }
  block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  available = (unsigned long long)vfs.f_bavail * block;
  if(available < required) {
    return rarx_fail(c, ZIPX_ERR_SPACE, target,
                     "not enough space, required %llu bytes, available %llu bytes",
                     required, available);
  }
  return 0;
}

static int
scan_archive(HANDLE hArc, rarx_ctx_t *c) {
  rarx_nameset_t set;
  int ret = 0;

  if(nameset_init(&set, 4096)) {
    return rarx_fail(c, ZIPX_ERR_INTERNAL, NULL, "out of memory");
  }

  for(;;) {
    struct RARHeaderDataEx hdr;
    uint64_t uncomp;
    char name[ZIPX_PATH_MAX];
    int is_dir;
    uint32_t depth = 0;
    int rc;

    memset(&hdr, 0, sizeof(hdr));
    rc = RARReadHeaderEx(hArc, &hdr);
    if(rc == ERAR_END_ARCHIVE) {
      break;
    }
    if(rc != ERAR_SUCCESS) {
      rar_translate_error(rc, NULL, c);
      ret = -1;
      break;
    }

    /* unrar hands out the header name as a NUL-terminated string
       (UTF-8 on the PS5 / POSIX build). */
    if(hdr.FileName[0] == 0) {
      ret = rarx_fail(c, ZIPX_ERR_FORMAT, NULL, "empty entry name");
      break;
    }
    is_dir = (hdr.Flags & RHDF_DIRECTORY) ? 1 : 0;

    /* Encrypted entries are fine as long as a password is in play: the engine
       decrypts them during RARProcessFile once RARSetPassword has run. With no
       password, fail here — before anything is written to staging — so the
       caller can prompt and retry. */
    if((hdr.Flags & RHDF_ENCRYPTED) && !c->password) {
      ret = rarx_fail(c, ZIPX_ERR_PASSWORD, hdr.FileName,
                      "the archive is encrypted and no password was supplied");
      break;
    }

    /* Multi-volume: a file spanning volumes is presented as several header
       segments with the SAME name. Segments after the first carry
       RHDF_SPLITBEFORE; they must drive the engine forward (SKIP) but must
       not be counted, deduped or size-accumulated again. The first segment
       already carries the full file size. */
    if(hdr.Flags & RHDF_SPLITBEFORE) {
      rc = RARProcessFile(hArc, RAR_SKIP, NULL, NULL);
      if(rc != ERAR_SUCCESS && rc != ERAR_END_ARCHIVE) {
        ret = -1;
        rar_translate_error(rc, hdr.FileName, c);
        break;
      }
      continue;
    }

    if(normalize_name(hdr.FileName, name, sizeof(name), &is_dir, &depth, c)) {
      ret = -1;
      break;
    }
    (void)depth;

    if(nameset_check_duplicate(&set, name, is_dir, c) ||
       nameset_add_path(&set, name, is_dir, c)) {
      ret = -1;
      break;
    }

    if(!is_dir) {
      uncomp = (uint64_t)hdr.UnpSizeHigh << 32 | hdr.UnpSize;

      if(uncomp > c->limits.max_file_bytes) {
        ret = rarx_fail(c, ZIPX_ERR_LIMIT_FILE, name,
                       "entry is larger than %llu bytes",
                       (unsigned long long)c->limits.max_file_bytes);
        break;
      }
      if(uncomp >= c->limits.max_total_bytes ||
         c->bytes_total > c->limits.max_total_bytes - uncomp) {
        ret = rarx_fail(c, ZIPX_ERR_LIMIT_TOTAL, name,
                       "archive contents are larger than %llu bytes",
                       (unsigned long long)c->limits.max_total_bytes);
        break;
      }
      c->bytes_total += uncomp;
    }

    c->entries_total++;
    if(c->entries_total > c->limits.max_entries) {
      ret = rarx_fail(c, ZIPX_ERR_LIMIT_ENTRIES, name,
                     "archive has more than %llu entries",
                     (unsigned long long)c->limits.max_entries);
      break;
    }
    if(c->entries_total % 4096 == 0) {
      if(canceled(c)) {
        ret = rarx_fail(c, ZIPX_ERR_CANCELED, name, NULL);
        break;
      }
      report(c, ZIPX_PHASE_SCAN, name, 0);
    }

    /* Advance to the next file header (also drives multi-volume merges). */
    rc = RARProcessFile(hArc, RAR_SKIP, NULL, NULL);
    if(rc != ERAR_SUCCESS) {
      if(rc != ERAR_END_ARCHIVE) {
        ret = -1;
        rar_translate_error(rc, name, c);
        break;
      }
      break;
    }
  }

  nameset_free(&set);
  return ret;
}

/**************************************************************************
 * extract phase
 **************************************************************************/

/* unrar invokes this for every decompressed chunk while RARProcessFile is
   extracting an entry to disk. Without it the engine could only account
   bytes_done after a whole entry completed, which froze the progress bar
   for the entire duration of a multi-GB entry spanning several volumes.

   Returning -1 is how unrar aborts a run, but the break path is only armed
   when console break handling is enabled, which never happens in DLL mode —
   so we always return 0 and cancellation stays entry-granular.

   The same callback is the only channel through which unrar asks permission
   for an oversized dictionary (UCM_LARGEDICT); see below. */
static int CALLBACK
rar_data_cb(UINT msg, LPARAM user, LPARAM p1, LPARAM p2) {
  rarx_ctx_t *c = (rarx_ctx_t *)user;

  if(msg == UCM_PROCESSDATA) {
    c->bytes_done += (uint64_t)(unsigned long)p2;
    report(c, ZIPX_PHASE_EXTRACT, NULL, 0);
    return 0;
  }

  if(msg == UCM_LARGEDICT) {
    /* unrar asks permission before it allocates a window bigger than
       Cmd->WinSizeLimit (default 4 GiB, options.cpp:13). Answering 1 would let
       the run continue -- and that is a *trap*, not a fix: the window is one
       contiguous allocation of the full dictionary size, and rarlab's own CLI
       refuses the same case with "8 GB dictionary exceeds the 4 GB limit and
       needs more than 8 GB of memory; use -md8g or -mdx8g". A PS5 has 16 GB of
       shared memory, so >4 GiB dictionaries are not extractable there anyway;
       failing cleanly beats being OOM-killed mid-extraction with the UI gone.
       Record the numbers (p1/p2, both KiB) so the refusal can explain itself. */
    c->dict_kb = (uint64_t)(unsigned long)p1;
    c->dict_limit_kb = (uint64_t)(unsigned long)p2;
    return 0;
  }

  return 0;
}

/* unrar extracts each entry directly under the staging root and creates
   parent directories itself. All entry names were validated (normalize_name)
   during scan, so what lands in the staging tree is safe by construction. */
static int
extract_archive(HANDLE hArc, rarx_ctx_t *c) {
  int ret = 0;

  RARSetCallback(hArc, rar_data_cb, (LPARAM)(intptr_t)c);

  for(;;) {
    struct RARHeaderDataEx hdr;
    int rc;
    int is_dir;

    memset(&hdr, 0, sizeof(hdr));
    rc = RARReadHeaderEx(hArc, &hdr);
    if(rc == ERAR_END_ARCHIVE) {
      break;
    }
    if(rc != ERAR_SUCCESS) {
      rar_translate_error(rc, NULL, c);
      ret = -1;
      break;
    }

    if(canceled(c)) {
      ret = rarx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
      break;
    }
    if(hdr.FileName[0] == 0) {
      ret = rarx_fail(c, ZIPX_ERR_FORMAT, NULL, "empty entry name");
      break;
    }
    if((hdr.Flags & RHDF_ENCRYPTED) && !c->password) {
      /* scan already rejected password-less encrypted sets; defensive only. */
      ret = rarx_fail(c, ZIPX_ERR_PASSWORD, hdr.FileName,
                      "the archive is encrypted and no password was supplied");
      break;
    }

    is_dir = (hdr.Flags & RHDF_DIRECTORY) ? 1 : 0;
    /* Split continuation segments still need RARProcessFile(EXTRACT) so the
       volume chain is driven and the file is completed, but only the first
       segment is counted / reported / size-accumulated. */
    if(hdr.Flags & RHDF_SPLITBEFORE) {
      rc = RARProcessFile(hArc, RAR_EXTRACT, c->staging, NULL);
      if(rc != ERAR_SUCCESS) {
        rar_translate_error(rc, hdr.FileName, c);
        ret = -1;
        break;
      }
      continue;
    }
    rc = RARProcessFile(hArc, RAR_EXTRACT, c->staging, NULL);
    if(rc != ERAR_SUCCESS) {
      rar_translate_error(rc, hdr.FileName, c);
      ret = -1;
      break;
    }

    if(is_dir) {
      c->dirs_created++;
    } else {
      c->files_created++;
    }
    c->entries_done++;
    report(c, ZIPX_PHASE_EXTRACT, hdr.FileName, 0);
  }
  /* unrar picked the modes (see chmod_tree_0777) - normalize the tree while it
     is still staging, so whatever gets published is executable. */
  if(ret == 0) chmod_tree_0777(c->staging);
  return ret;
}

/**************************************************************************
 * publish phase (identical to zip_extract.c — kept inline so that the
 * two engines stay self-contained)
 **************************************************************************/

static int publish_dir(const char *src, const char *dst, int depth,
                       rarx_ctx_t *c);

static int
publish_entry(const char *src, const char *dst, const char *name, int depth,
              rarx_ctx_t *c) {
  char src_child[ZIPX_PATH_MAX];
  char dst_child[ZIPX_PATH_MAX];
  struct stat st;
  struct stat src_st;
  int src_is_dir;

  if(snprintf(src_child, sizeof(src_child), "%s/%s", src, name) >=
       (int)sizeof(src_child) ||
     snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, name) >=
       (int)sizeof(dst_child)) {
    return rarx_fail(c, ZIPX_ERR_LIMIT_NAME, name, "path is too long");
  }
  if(lstat(src_child, &src_st)) {
    return rarx_fail(c, ZIPX_ERR_IO, src_child, "cannot read staging: %s",
                     strerror(errno));
  }
  src_is_dir = S_ISDIR(src_st.st_mode) ? 1 : 0;

  if(lstat(dst_child, &st)) {
    if(errno != ENOENT) {
      return rarx_fail(c, ZIPX_ERR_IO, dst_child, "cannot check target: %s",
                       strerror(errno));
    }
    if(rename(src_child, dst_child)) {
      return rarx_fail(c, ZIPX_ERR_IO, dst_child, "cannot publish: %s",
                       strerror(errno));
    }
    return remember_published(c, dst_child, src_is_dir);
  }

  if(S_ISDIR(st.st_mode)) {
    if(!src_is_dir) {
      return rarx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                       "file collides with an existing directory");
    }
    if(c->conflict != ZIPX_CONFLICT_MERGE) {
      return rarx_fail(c, ZIPX_ERR_CONFLICT, dst_child,
                       "directory already exists");
    }
    if(depth >= RARX_PUBLISH_MAX_DEPTH) {
      return rarx_fail(c, ZIPX_ERR_LIMIT_DEPTH, dst_child, "path is too deep");
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
          return rarx_fail(c, ZIPX_ERR_IO, dst_child, "cannot replace: %s",
                           strerror(errno));
        }
        return remember_published(c, dst_child, 0);
      }
      if(c->conflict == ZIPX_CONFLICT_MERGE) {
        if(unlink(src_child)) {
          return rarx_fail(c, ZIPX_ERR_IO, dst_child,
                          "cannot drop staged file: %s", strerror(errno));
        }
        return 0;
      }
    }
    return rarx_fail(c, ZIPX_ERR_CONFLICT, dst_child, "target already exists");
  }
  return rarx_fail(c, ZIPX_ERR_CONFLICT, dst_child, "target already exists");
}

static int
publish_dir(const char *src, const char *dst, int depth, rarx_ctx_t *c) {
  DIR *dir = opendir(src);
  struct dirent *ent;
  char **names = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t i;
  int ret = 0;

  if(!dir) {
    return rarx_fail(c, ZIPX_ERR_IO, src, "cannot read staging: %s",
                     strerror(errno));
  }
  while((ent = readdir(dir))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
      continue;
    }
    if(count == cap) {
      size_t next = cap ? cap * 2 : 64;
      char **grown_arr = realloc(names, next * sizeof(*grown_arr));
      if(!grown_arr) {
        ret = rarx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
        break;
      }
      names = grown_arr;
      cap = next;
    }
    if(!(names[count] = strdup(ent->d_name))) {
      ret = rarx_fail(c, ZIPX_ERR_INTERNAL, src, "out of memory");
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
publish_staging(rarx_ctx_t *c, const char *dst_dir, int dst_existed) {
  int ret;

  report(c, ZIPX_PHASE_PUBLISH, dst_dir, 1);
  if(!dst_existed) {
    if(rename(c->staging, dst_dir)) {
      return rarx_fail(c, ZIPX_ERR_IO, dst_dir, "cannot publish: %s",
                       strerror(errno));
    }
    c->staging_created = 0;
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

zipx_status_t
rar_extract(const char *rar_path, const char *dst_dir,
            zipx_conflict_t conflict, const zipx_limits_t *limits,
            zipx_cancel_fn cancel, zipx_progress_fn progress,
            void *userdata, const char *password, zipx_result_t *result) {
  rarx_ctx_t ctx;
  rarx_ctx_t *c = &ctx;
  char parent[ZIPX_PATH_MAX];
  char dst_copy[ZIPX_PATH_MAX];
  struct stat st;
  HANDLE hArc = NULL;
  int dst_existed = 0;
  int status;

  if(!result || !rar_path || !dst_dir || !dst_dir[0]) {
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
  /* An empty string means "no password" so that callers can pass the raw
     form field without a separate emptiness check. */
  c->password = (password && password[0]) ? password : NULL;

  snprintf(dst_copy, sizeof(dst_copy), "%s", dst_dir);
  {
    size_t len = strlen(dst_copy);
    while(len > 1 && dst_copy[len - 1] == '/') {
      dst_copy[--len] = 0;
    }
  }
  if(path_parent(dst_copy, parent, sizeof(parent))) {
    status = rarx_fail(c, ZIPX_ERR_INTERNAL, dst_dir, "invalid destination");
    goto done;
  }
  if(stat(parent, &st) || !S_ISDIR(st.st_mode)) {
    status = rarx_fail(c, ZIPX_ERR_IO, parent, "destination parent is missing");
    goto done;
  }
  dst_existed = !lstat(dst_copy, &st);
  if(dst_existed && !S_ISDIR(st.st_mode)) {
    status = rarx_fail(c, ZIPX_ERR_CONFLICT, dst_copy,
                       "destination is not a directory");
    goto done;
  }

  /* Open with RAR_OM_EXTRACT for both passes: the unrar engine drives
     multi-volume merges identically while scanning (skip) and extracting,
     and encrypted headers surface here as a missing/bad password. */
  {
    struct RAROpenArchiveDataEx od;
    memset(&od, 0, sizeof(od));
    od.ArcName = (char *)rar_path;
    od.OpenMode = RAR_OM_EXTRACT;
    hArc = RAROpenArchiveEx(&od);
    if(!hArc) {
      status = rar_translate_error((int)od.OpenResult, rar_path, c);
      goto done;
    }
    /* Must precede the first RARReadHeaderEx: unrar needs the password in
       place to decrypt a -hp (encrypted header) archive. */
    if(c->password) {
      WFM_RAR_PASSWORD(hArc, (char *)c->password);
    }
  }

  if(scan_archive(hArc, c)) {
    status = (int)c->result->status;
    RARCloseArchive(hArc);
    hArc = NULL;
    goto done;
  }
  if(canceled(c)) {
    status = rarx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    RARCloseArchive(hArc);
    hArc = NULL;
    goto done;
  }
  if(check_space(c, dst_existed ? dst_copy : parent)) {
    status = (int)c->result->status;
    RARCloseArchive(hArc);
    hArc = NULL;
    goto done;
  }
  if(make_staging(c, parent)) {
    status = (int)c->result->status;
    RARCloseArchive(hArc);
    hArc = NULL;
    goto done;
  }

  /* Pass 2: re-open and extract each entry straight into the staging tree
     (unrar creates parent directories itself; names were validated during
     scan). The staging root is pre-verified below for fast failure. */
  RARCloseArchive(hArc);
  hArc = NULL;
  {
    struct RAROpenArchiveDataEx od;
    memset(&od, 0, sizeof(od));
    od.ArcName = (char *)rar_path;
    od.OpenMode = RAR_OM_EXTRACT;
    hArc = RAROpenArchiveEx(&od);
    if(!hArc) {
      status = rar_translate_error((int)od.OpenResult, rar_path, c);
      goto done;
    }
    if(c->password) {
      WFM_RAR_PASSWORD(hArc, (char *)c->password);
    }
  }

  if(extract_archive(hArc, c)) {
    status = (int)c->result->status;
    goto done;
  }
  if(canceled(c)) {
    status = rarx_fail(c, ZIPX_ERR_CANCELED, NULL, NULL);
    goto done;
  }
  if(publish_staging(c, dst_copy, dst_existed)) {
    status = (int)c->result->status;
    goto done;
  }

  result->status = ZIPX_OK;
  status = ZIPX_OK;

done:
  if(hArc) {
    RARCloseArchive(hArc);
  }
  cleanup_staging(c);
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