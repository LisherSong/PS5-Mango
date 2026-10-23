/* zip_write.c — streaming STORE-only ZIP writer (original implementation)
 *
 * Ported in spirit from the reference save-manager's streaming zip writer
 * (garlic-savemgr src/main.c zip_write_files): store method, data-descriptor
 * mode, CRC computed while reading. No code was copied verbatim; the on-disk
 * ZIP structures (local header / central directory / EOCD / data descriptor)
 * are the public PKWARE APPNOTE format.
 *
 * Design rules honoured here:
 *   - The directory walker is recursive; its path buffers are heap-allocated
 *     at each level (malloc/free), never large stack arrays — LTO inlining
 *     of recursive frames caused a real stack-overflow crash on the PS5.
 *   - Pure lib: no HTTP / device / MHD dependencies, so hosttest can link and
 *     exercise it directly.
 */
#include "zip_write.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZIPW_MAX_FILES 4096
#define ZIPW_PATH_CAP 4096

/* ---- CRC32 (bitwise, table-free; fine for 256 KiB blocks) ---------------- */

static uint32_t
zipw_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1)));
  }
  return crc;
}

/* ---- sink helper --------------------------------------------------------- */

static int
zipw_sink_write(zipw_sink_t *sink, const void *buf, size_t len) {
  if (!sink || !sink->write) return -1;
  if (len == 0) return 0;
  if (sink->write(sink->ctx, buf, len) < 0) return -1;
  sink->bytes += len;
  return 0;
}

/* ---- directory walker (heap path buffers, recursion-safe) ---------------- */

static int
zipw_files_add(zipw_file_t **files, int *n, int *cap, const char *path,
               const char *rel) {
  if (*n >= ZIPW_MAX_FILES) return -1;
  if (*n >= *cap) {
    int nc = *cap ? *cap * 2 : 64;
    zipw_file_t *nf = realloc(*files, (size_t)nc * sizeof(zipw_file_t));
    if (!nf) return -1;
    *files = nf;
    *cap = nc;
  }
  zipw_file_t *e = &(*files)[*n];
  e->path = strdup(path);
  e->name = strdup(rel);
  if (!e->path || !e->name) {
    free(e->path);
    free(e->name);
    e->path = NULL;
    e->name = NULL;
    return -1;
  }
  (*n)++;
  return 0;
}

/* Recursive collector. `rel` is the entry's archive path relative to `base`
 * ("" for the root level) and is heap-allocated by the caller — every level
 * allocates its own copy so no fixed-size stack array ever enters the
 * recursion. */
static int
zipw_walk_rec(const char *base, const char *rel, zipw_file_t **files, int *n,
              int *cap) {
  char dirpath[PATH_MAX];
  if (rel[0]) {
    if (snprintf(dirpath, sizeof(dirpath), "%s/%s", base, rel) >=
        (int)sizeof(dirpath))
      return 0;
  } else {
    if (snprintf(dirpath, sizeof(dirpath), "%s", base) >= (int)sizeof(dirpath))
      return 0;
  }

  DIR *d = opendir(dirpath);
  if (!d) return 0; /* unreadable subtree: contribute nothing, keep going */

  struct dirent *ent;
  int rc = 0;
  while ((ent = readdir(d))) {
    if (ent->d_name[0] == '.') continue; /* dot files (incl. . and ..) */

    char *rel_child = NULL;
    if (rel[0]) {
      size_t rl = strlen(rel);
      size_t nl = strlen(ent->d_name);
      rel_child = malloc(rl + 1 + nl + 1);
      if (!rel_child) { rc = -1; break; }
      memcpy(rel_child, rel, rl);
      rel_child[rl] = '/';
      memcpy(rel_child + rl + 1, ent->d_name, nl + 1);
    } else {
      rel_child = strdup(ent->d_name);
      if (!rel_child) { rc = -1; break; }
    }

    size_t dl = strlen(dirpath);
    size_t nl = strlen(ent->d_name);
    if (dl + 1 + nl + 1 > ZIPW_PATH_CAP) { free(rel_child); continue; }
    char *fullpath = malloc(dl + 1 + nl + 1);
    if (!fullpath) { free(rel_child); rc = -1; break; }
    memcpy(fullpath, dirpath, dl);
    fullpath[dl] = '/';
    memcpy(fullpath + dl + 1, ent->d_name, nl + 1);

    struct stat st;
    /* lstat: do not follow symlinks (they are skipped, not dereferenced). */
    if (lstat(fullpath, &st) < 0) { free(rel_child); free(fullpath); continue; }

    if (S_ISDIR(st.st_mode)) {
      rc = zipw_walk_rec(base, rel_child, files, n, cap);
      free(rel_child);
      free(fullpath);
      if (rc < 0) break;
    } else if (S_ISREG(st.st_mode)) {
      rc = zipw_files_add(files, n, cap, fullpath, rel_child);
      free(rel_child);
      free(fullpath);
      if (rc < 0) break;
    } else {
      /* symlink / fifo / device / socket: skip */
      free(rel_child);
      free(fullpath);
    }
  }
  closedir(d);
  return rc;
}

int
zipw_walk_dir(const char *base, zipw_file_t **out, int *out_n) {
  if (!base || !out || !out_n) return -1;
  zipw_file_t *files = NULL;
  int n = 0;
  int cap = 0;
  if (zipw_walk_rec(base, "", &files, &n, &cap) < 0) {
    for (int i = 0; i < n; i++) {
      free(files[i].path);
      free(files[i].name);
    }
    free(files);
    return -1;
  }
  *out = files;
  *out_n = n;
  return 0;
}

/* ---- ZIP writing --------------------------------------------------------- */

int
zipw_write_files(zipw_sink_t *sink, const zipw_file_t *files, int nfiles,
                 char *iobuf, size_t iobuf_sz) {
  if (!sink || !files || nfiles < 0) return -1;
  if (nfiles == 0) return 0;
  if (!iobuf || iobuf_sz == 0) return -1;

  typedef struct {
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  } entry_meta_t;
  entry_meta_t *meta = malloc((size_t)nfiles * sizeof(entry_meta_t));
  if (!meta) return -1;

  uint32_t offset = 0;
  for (int i = 0; i < nfiles; i++) {
    const zipw_file_t *f = &files[i];
    if (!f->name) { free(meta); return -1; }
    uint16_t nlen = (uint16_t)strlen(f->name);
    if (strlen(f->name) > 0xFFFF) { free(meta); return -1; }

    meta[i].offset = offset;
    meta[i].size = 0;
    meta[i].crc = 0;

    struct stat st;
    int fd = -1;
    if (f->path && stat(f->path, &st) == 0 && S_ISREG(st.st_mode))
      fd = open(f->path, O_RDONLY);

    /* Local file header: store method, bit 3 (data descriptor follows). */
    uint8_t lh[30];
    memset(lh, 0, sizeof(lh));
    lh[0] = 0x50; lh[1] = 0x4b; lh[2] = 0x03; lh[3] = 0x04;
    lh[4] = 20;            /* version needed */
    lh[6] = 0x08;          /* general purpose bit 3: sizes+CRC in descriptor */
    memcpy(lh + 26, &nlen, 2);
    if (zipw_sink_write(sink, lh, sizeof(lh)) < 0 ||
        zipw_sink_write(sink, f->name, nlen) < 0) {
      if (fd >= 0) close(fd);
      free(meta);
      return -1;
    }

    if (fd >= 0) {
      uint32_t crc = 0xFFFFFFFF;
      uint32_t size = 0;
      ssize_t nr;
      while ((nr = read(fd, iobuf, iobuf_sz)) > 0) {
        crc = zipw_crc32_update(crc, (const uint8_t *)iobuf, (size_t)nr);
        size += (uint32_t)nr;
        if (zipw_sink_write(sink, iobuf, (size_t)nr) < 0) {
          close(fd);
          free(meta);
          return -1;
        }
      }
      close(fd);
      crc ^= 0xFFFFFFFF;
      meta[i].crc = crc;
      meta[i].size = size;
    }

    /* Data descriptor (16 bytes, signature PK\x07\x08). */
    uint8_t dd[16];
    dd[0] = 0x50; dd[1] = 0x4b; dd[2] = 0x07; dd[3] = 0x08;
    memcpy(dd + 4, &meta[i].crc, 4);
    memcpy(dd + 8, &meta[i].size, 4);
    memcpy(dd + 12, &meta[i].size, 4);
    if (zipw_sink_write(sink, dd, sizeof(dd)) < 0) {
      free(meta);
      return -1;
    }

    offset += 30 + nlen + meta[i].size + 16;
  }

  /* Central directory. */
  uint32_t cd_offset = offset;
  for (int i = 0; i < nfiles; i++) {
    const zipw_file_t *f = &files[i];
    uint16_t nlen = (uint16_t)strlen(f->name);
    uint8_t cd[46];
    memset(cd, 0, sizeof(cd));
    cd[0] = 0x50; cd[1] = 0x4b; cd[2] = 0x01; cd[3] = 0x02;
    cd[4] = 20; cd[6] = 20;
    /* bit 3 必须同步到 central directory：本地头用了 data descriptor，CD 也要
       声明。否则 Windows 自带 zip 等严格阅读器会报「头部错误」。 */
    cd[8] = 0x08;
    memcpy(cd + 16, &meta[i].crc, 4);
    memcpy(cd + 20, &meta[i].size, 4);
    memcpy(cd + 24, &meta[i].size, 4);
    memcpy(cd + 28, &nlen, 2);
    memcpy(cd + 42, &meta[i].offset, 4);
    if (zipw_sink_write(sink, cd, sizeof(cd)) < 0 ||
        zipw_sink_write(sink, f->name, nlen) < 0) {
      free(meta);
      return -1;
    }
    offset += 46 + nlen;
  }
  uint32_t cd_size = offset - cd_offset;

  /* End of central directory. */
  uint8_t ecd[22];
  memset(ecd, 0, sizeof(ecd));
  ecd[0] = 0x50; ecd[1] = 0x4b; ecd[2] = 0x05; ecd[3] = 0x06;
  uint16_t cnt16 = (uint16_t)nfiles;
  memcpy(ecd + 8, &cnt16, 2);
  memcpy(ecd + 10, &cnt16, 2);
  memcpy(ecd + 12, &cd_size, 4);
  memcpy(ecd + 16, &cd_offset, 4);
  if (zipw_sink_write(sink, ecd, sizeof(ecd)) < 0) {
    free(meta);
    return -1;
  }

  free(meta);
  return 0;
}

int
zipw_write_tree(zipw_sink_t *sink, const char *base, char *iobuf,
                size_t iobuf_sz) {
  zipw_file_t *files = NULL;
  int n = 0;
  if (zipw_walk_dir(base, &files, &n) < 0) return -1;
  int rc = zipw_write_files(sink, files, n, iobuf, iobuf_sz);
  for (int i = 0; i < n; i++) {
    free(files[i].path);
    free(files[i].name);
  }
  free(files);
  return rc;
}
