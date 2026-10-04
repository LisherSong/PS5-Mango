/* sevenz_volstream -- see sevenz_volstream.h for what this does and why. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <wchar.h>
#endif

#include "7zFile.h"

#include "sevenz_volstream.h"
#include "zipx_volume.h"

struct sevenz_volstream {
  ISeekInStream vt;

  zipx_volume_t vol;   /* owns the ordered paths */
  CSzFile *file;       /* one per part */
  uint64_t *start;     /* count + 1 prefix offsets into the logical archive */
  uint32_t count;
  uint32_t open_count; /* how many entries of `file` were opened */
  uint64_t pos;        /* current offset in the logical archive */
  uint32_t cur;        /* part `pos` currently sits in, to skip redundant seeks */
  uint64_t cur_off;    /* file offset within that part */
  char name[256];      /* stem of the set, for messages */
};

/* ---------------------------------------------------------------- helpers */

static char *err_printf(const char *fmt, ...) {
  va_list ap;
  char buf[512];
  char *out;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  out = (char *)malloc(strlen(buf) + 1);
  if(out) memcpy(out, buf, strlen(buf) + 1);
  return out;
}

static const char *file_base(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *back = strrchr(path, '\\');

  if(back && (!slash || back > slash)) slash = back;
  return slash ? slash + 1 : path;
}

#if defined(_WIN32)

/* The SDK opens through CreateFileA otherwise, which cannot see non-ASCII
   entry names. */
static void utf8_to_utf16(const char *src, WCHAR *dst, size_t cap) {
  size_t out = 0;

  while(*src && out + 2 < cap) {
    unsigned char c = (unsigned char)*src++;
    UInt32 cp;

    if(c < 0x80) {
      cp = c;
    } else if((c & 0xE0) == 0xC0 && (src[0] & 0xC0) == 0x80) {
      cp = ((UInt32)(c & 0x1F) << 6) | (UInt32)(*src++ & 0x3F);
    } else if((c & 0xF0) == 0xE0 && (src[0] & 0xC0) == 0x80 &&
              (src[1] & 0xC0) == 0x80) {
      cp = ((UInt32)(c & 0x0F) << 12) | ((UInt32)(src[0] & 0x3F) << 6) |
           (UInt32)(src[1] & 0x3F);
      src += 2;
    } else if((c & 0xF8) == 0xF0 && (src[0] & 0xC0) == 0x80 &&
              (src[1] & 0xC0) == 0x80 && (src[2] & 0xC0) == 0x80) {
      cp = ((UInt32)(c & 0x07) << 18) | ((UInt32)(src[0] & 0x3F) << 12) |
           ((UInt32)(src[1] & 0x3F) << 6) | (UInt32)(src[2] & 0x3F);
      src += 3;
    } else {
      cp = '?';
    }

    if(cp >= 0x10000) {
      cp -= 0x10000;
      dst[out++] = (WCHAR)(0xD800 | (cp >> 10));
      dst[out++] = (WCHAR)(0xDC00 | (cp & 0x3FF));
    } else {
      dst[out++] = (WCHAR)cp;
    }
  }
  dst[out] = 0;
}

static int open_part(CSzFile *file, const char *path) {
  WCHAR wide[4096];

  utf8_to_utf16(path, wide, sizeof(wide) / sizeof(wide[0]));
  return InFile_OpenW(file, wide) == 0 ? 0 : -1;
}

#else

static int open_part(CSzFile *file, const char *path) {
  return InFile_Open(file, path) == 0 ? 0 : -1;
}

#endif

/* ----------------------------------------------------------------- stream */

static uint32_t part_at(const sevenz_volstream *v, uint64_t pos) {
  uint32_t i;

  for(i = 0; i < v->count; i++) {
    if(pos < v->start[i + 1]) return i;
  }
  return v->count;
}

static SRes vol_read(const ISeekInStream *p, void *buf, size_t *size) {
  sevenz_volstream *v = (sevenz_volstream *)p;
  uint8_t *dst = (uint8_t *)buf;
  size_t want = *size;
  size_t got = 0;

  *size = 0;
  while(got < want) {
    uint32_t i = part_at(v, v->pos);
    uint64_t avail, off;
    size_t take;

    if(i >= v->count) break; /* end of the set: a short read means EOF */

    off = v->pos - v->start[i];
    avail = (v->start[i + 1] - v->start[i]) - off;
    take = (size_t)(avail < (uint64_t)(want - got) ? avail
                                                   : (uint64_t)(want - got));
    if(take == 0) break;

    if(v->cur != i || v->cur_off != off) {
      Int64 seek = (Int64)off;
      if(File_Seek(&v->file[i], &seek, SZ_SEEK_SET) != 0) return SZ_ERROR_READ;
      v->cur = i;
      v->cur_off = off;
    }

    {
      size_t part_got = take;
      if(File_Read(&v->file[i], dst + got, &part_got) != 0) return SZ_ERROR_READ;
      if(part_got == 0) break;
      got += part_got;
      v->pos += part_got;
      v->cur_off += part_got;
      if(part_got < take) break;
    }
  }

  *size = got;
  return SZ_OK;
}

static SRes vol_seek(const ISeekInStream *p, Int64 *pos, ESzSeek origin) {
  sevenz_volstream *v = (sevenz_volstream *)p;
  uint64_t total = v->start[v->count];
  uint64_t target;

  switch(origin) {
  case SZ_SEEK_SET:
    if(*pos < 0) return SZ_ERROR_PARAM;
    target = (uint64_t)*pos;
    break;
  case SZ_SEEK_CUR:
    if(*pos < 0) {
      UInt64 back = (UInt64)(-*pos);
      if(back > v->pos) return SZ_ERROR_PARAM;
      target = v->pos - back;
    } else {
      target = v->pos + (UInt64)*pos;
    }
    break;
  case SZ_SEEK_END:
    if(*pos < 0) {
      UInt64 back = (UInt64)(-*pos);
      if(back > total) return SZ_ERROR_PARAM;
      target = total - back;
    } else {
      target = total + (UInt64)*pos;
    }
    break;
  default:
    return SZ_ERROR_PARAM;
  }

  if(target > total) target = total;
  v->pos = target;
  *pos = (Int64)target;
  return SZ_OK;
}

/* -------------------------------------------------------------------- open */

int sevenz_volstream_open(sevenz_volstream **out, const char *path, int *is_set,
                          char **err) {
  sevenz_volstream *v;
  char *vol_err = NULL;
  int detected;
  uint32_t i;

  if(out) *out = NULL;
  if(is_set) *is_set = 0;
  if(err) *err = NULL;
  if(!out || !path) {
    if(err) *err = err_printf("no archive path given");
    return -1;
  }

  v = (sevenz_volstream *)calloc(1, sizeof(*v));
  if(!v) {
    if(err) *err = err_printf("out of memory");
    return -1;
  }

  /* One call does both jobs: it either reports "ordinary file" and clears the
     struct, or fills in the ordered part list. A -1 here already carries the
     message the user needs (a hole in the numbering names the missing part). */
  detected = zipx_volume_detect(path, &v->vol, &vol_err);
  if(detected < 0) {
    if(err) {
      *err = vol_err ? vol_err
                     : err_printf("'%s' cannot be read", file_base(path));
    } else {
      free(vol_err);
    }
    zipx_volume_free(&v->vol);
    free(v);
    return -1;
  }

  if(detected == 0) {
    v->vol.paths = (char **)malloc(sizeof(char *));
    if(v->vol.paths) v->vol.paths[0] = (char *)malloc(strlen(path) + 1);
    if(!v->vol.paths || !v->vol.paths[0]) {
      free(v->vol.paths);
      free(v);
      if(err) *err = err_printf("out of memory");
      return -1;
    }
    memcpy(v->vol.paths[0], path, strlen(path) + 1);
    v->vol.count = 1;
    v->vol.mode = ZIPX_VOL_MODE_CONCAT;
    v->vol.is_set = 0;
  } else if(is_set) {
    *is_set = 1;
  }

  snprintf(v->name, sizeof(v->name), "%s", file_base(path));

  v->count = (uint32_t)v->vol.count;
  v->file = (CSzFile *)calloc(v->count, sizeof(CSzFile));
  v->start = (uint64_t *)calloc((size_t)v->count + 1, sizeof(uint64_t));
  if(!v->file || !v->start) {
    if(err) *err = err_printf("out of memory");
    goto fail;
  }

  for(i = 0; i < v->count; i++) {
    UInt64 length = 0;

    File_Construct(&v->file[i]);
    v->open_count = i + 1; /* File_Close() ignores a never-opened handle */
    if(open_part(&v->file[i], v->vol.paths[i]) != 0) {
      if(err) {
        *err = err_printf("cannot open volume '%s' of '%s'",
                          file_base(v->vol.paths[i]), v->name);
      }
      goto fail;
    }
    if(File_GetLength(&v->file[i], &length) != 0) {
      if(err) {
        *err = err_printf("cannot measure volume '%s' of '%s'",
                          file_base(v->vol.paths[i]), v->name);
      }
      goto fail;
    }
    v->start[i + 1] = v->start[i] + length;
  }

  if(v->start[v->count] == 0) {
    if(err) *err = err_printf("'%s' is empty", v->name);
    goto fail;
  }

  /* 7-Zip cuts equal sized parts and lets only the last one be short. A part
     of a different size in the middle means the set is damaged or was mixed
     with another one, and decoding would fail much later with a message that
     points nowhere useful. */
  for(i = 0; i + 1 < v->count; i++) {
    uint64_t size = v->start[i + 1] - v->start[i];
    if(size != v->start[1]) {
      if(err) {
        *err = err_printf("volume '%s' of '%s' is %llu bytes, but the earlier "
                          "volumes are %llu bytes: the set is not a clean split",
                          file_base(v->vol.paths[i]), v->name,
                          (unsigned long long)size,
                          (unsigned long long)v->start[1]);
      }
      goto fail;
    }
  }

  v->vt.Read = vol_read;
  v->vt.Seek = vol_seek;
  *out = v;
  return 0;

fail:
  sevenz_volstream_free(v);
  return -1;
}

ISeekInStream *sevenz_volstream_stream(sevenz_volstream *v) {
  return v ? &v->vt : NULL;
}

uint32_t sevenz_volstream_count(const sevenz_volstream *v) {
  return v ? v->count : 0;
}

uint64_t sevenz_volstream_size(const sevenz_volstream *v) {
  return v ? v->start[v->count] : 0;
}

const char *sevenz_volstream_describe(const sevenz_volstream *v, char *buf,
                                      unsigned size) {
  if(!buf || size == 0) return buf;
  if(!v) {
    snprintf(buf, size, "no archive");
  } else if(v->count <= 1) {
    snprintf(buf, size, "%s", v->name);
  } else {
    snprintf(buf, size, "%s (%u volumes)", v->name, (unsigned)v->count);
  }
  return buf;
}

void sevenz_volstream_free(sevenz_volstream *v) {
  uint32_t i;

  if(!v) return;
  for(i = 0; i < v->open_count; i++) File_Close(&v->file[i]);
  free(v->file);
  free(v->start);
  zipx_volume_free(&v->vol);
  free(v);
}
