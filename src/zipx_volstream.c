/* zipx_volstream -- present a multi-file archive volume set as one stream.
   part of ps5-web-file-manager

   See zipx_volstream.h for the two split layouts this supports. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_strm_os.h"

#include "zipx_volstream.h"

#define VOL_INT32_MAX 0x7fffffffLL

typedef struct {
  mz_stream stream;   /* first member: callbacks cast the handle to this */
  char **paths;       /* ordered part paths */
  int32_t count;
  int64_t *prefix;    /* count + 1 entries, prefix[count] == total */
  int64_t total;
  int32_t mode;       /* ZIPX_VOL_MODE_* */
  int32_t disk;       /* active part index */
  int64_t pos;        /* absolute position inside the concatenated set */
  int32_t os_part;    /* part currently held by os, -1 when nothing is open */
  void *os;
  int32_t opened;
  int32_t error;
} zipx_volstream_t;

/* ------------------------------------------------------------------------- */

static int32_t
vol_use_part(zipx_volstream_t *v, int32_t part) {
  if(v->os_part == part) {
    return MZ_OK;
  }
  if(v->os_part >= 0) {
    mz_stream_close(v->os);
    v->os_part = -1;
  }
  if(mz_stream_open(v->os, v->paths[part], MZ_OPEN_MODE_READ) != MZ_OK) {
    v->error = MZ_OPEN_ERROR;
    return MZ_OPEN_ERROR;
  }
  v->os_part = part;
  return MZ_OK;
}

/* Part holding an absolute offset, walking from `hint` (parts are laid out in
   order and reads are sequential, so this stays O(1) amortised). */
static int32_t
vol_part_of(zipx_volstream_t *v, int64_t pos, int32_t hint) {
  int32_t i;

  if(pos < 0 || pos >= v->total) {
    return -1;
  }
  i = hint;
  if(i < 0) {
    i = 0;
  }
  if(i > v->count - 1) {
    i = v->count - 1;
  }
  while(i > 0 && pos < v->prefix[i]) {
    i--;
  }
  while(i < v->count - 1 && pos >= v->prefix[i + 1]) {
    i++;
  }
  return i;
}

static int32_t
vol_open(void *stream, const char *path, int32_t mode) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;
  int64_t sum = 0;
  int32_t i;

  (void)path;
  (void)mode;
  if(!v || v->count <= 0 || !v->paths) {
    return MZ_PARAM_ERROR;
  }
  v->prefix = (int64_t *)calloc((size_t)v->count + 1, sizeof(*v->prefix));
  if(!v->prefix) {
    v->error = MZ_MEM_ERROR;
    return MZ_MEM_ERROR;
  }
  for(i = 0; i < v->count; i++) {
    int64_t size = mz_os_get_file_size(v->paths[i]);

    if(size < 0) {
      v->error = MZ_OPEN_ERROR;
      return MZ_OPEN_ERROR;
    }
    v->prefix[i] = sum;
    sum += size;
  }
  v->prefix[v->count] = sum;
  v->total = sum;

  if(!v->os) {
    v->os = mz_stream_os_create();
    if(!v->os) {
      v->error = MZ_MEM_ERROR;
      return MZ_MEM_ERROR;
    }
  }
  /* The end-of-central-directory record sits on the last disk, so a split
     disk set starts there; a byte split is read from its first byte. */
  v->disk = (v->mode == ZIPX_VOL_MODE_DISK) ? v->count - 1 : 0;
  v->pos = v->prefix[v->disk];
  v->os_part = -1;
  v->opened = 1;
  v->error = MZ_OK;
  return MZ_OK;
}

static int32_t
vol_is_open(void *stream) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  return (v && v->opened) ? MZ_OK : MZ_OPEN_ERROR;
}

static int32_t
vol_read(void *stream, void *buf, int32_t size) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;
  int32_t done = 0;
  int32_t hint;

  if(!v || !v->opened || !buf) {
    return MZ_PARAM_ERROR;
  }
  if(size <= 0) {
    return 0;
  }
  hint = v->os_part >= 0 ? v->os_part : 0;
  while(done < size) {
    int32_t part = vol_part_of(v, v->pos, hint);
    int64_t in_part;
    int64_t avail;
    int64_t want;
    int32_t got;

    if(part < 0) {
      break;                       /* end of the logical archive */
    }
    hint = part;
    if(vol_use_part(v, part) != MZ_OK) {
      break;
    }
    in_part = v->pos - v->prefix[part];
    avail = (v->prefix[part + 1] - v->prefix[part]) - in_part;
    if(avail <= 0) {               /* empty part: step over it */
      v->pos = v->prefix[part + 1];
      continue;
    }
    want = (int64_t)(size - done);
    if(want > avail) {
      want = avail;
    }
    if(want > VOL_INT32_MAX) {
      want = VOL_INT32_MAX;
    }
    if(mz_stream_tell(v->os) != in_part) {
      if(mz_stream_seek(v->os, in_part, MZ_SEEK_SET) != MZ_OK) {
        v->error = MZ_SEEK_ERROR;
        break;
      }
    }
    got = mz_stream_read(v->os, (uint8_t *)buf + done, (int32_t)want);
    if(got <= 0) {
      if(got < 0) {
        v->error = MZ_READ_ERROR;
      }
      break;
    }
    done += got;
    v->pos += got;
    if(got < (int32_t)want) {
      break;                       /* short read: let the caller come back */
    }
  }
  if(done == 0 && v->error != MZ_OK) {
    return v->error;
  }
  return done;
}

static int32_t
vol_write(void *stream, const void *buf, int32_t size) {
  (void)stream;
  (void)buf;
  (void)size;
  return MZ_SUPPORT_ERROR;
}

static int64_t
vol_tell(void *stream) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  if(!v || !v->opened) {
    return -1;
  }
  if(v->mode == ZIPX_VOL_MODE_DISK) {
    return v->pos - v->prefix[v->disk];
  }
  return v->pos;
}

static int32_t
vol_seek(void *stream, int64_t offset, int32_t origin) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;
  int64_t target;

  if(!v || !v->opened) {
    return MZ_PARAM_ERROR;
  }
  if(origin == MZ_SEEK_SET) {
    target = offset;
    if(v->mode == ZIPX_VOL_MODE_DISK) {
      target += v->prefix[v->disk];   /* offsets are disk relative there */
    }
  } else if(origin == MZ_SEEK_CUR) {
    target = v->pos + offset;
  } else if(origin == MZ_SEEK_END) {
    target = v->total + offset;
  } else {
    return MZ_PARAM_ERROR;
  }
  if(target < 0) {
    target = 0;
  }
  if(target > v->total) {
    target = v->total;
  }
  v->pos = target;
  return MZ_OK;
}

static int32_t
vol_close(void *stream) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  if(!v) {
    return MZ_PARAM_ERROR;
  }
  if(v->os && v->os_part >= 0) {
    mz_stream_close(v->os);
    v->os_part = -1;
  }
  v->opened = 0;
  return MZ_OK;
}

static int32_t
vol_error(void *stream) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  return v ? v->error : MZ_PARAM_ERROR;
}

static int32_t
vol_get_prop(void *stream, int32_t prop, int64_t *value) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  if(!v || !value) {
    return MZ_PARAM_ERROR;
  }
  if(v->mode != ZIPX_VOL_MODE_DISK) {
    /* A byte split keeps absolute offsets, so the disk properties must look
       unsupported: minizip-ng then leaves every offset alone. */
    return MZ_PARAM_ERROR;
  }
  if(prop == MZ_STREAM_PROP_DISK_NUMBER) {
    *value = v->disk;
    return MZ_OK;
  }
  if(prop == MZ_STREAM_PROP_DISK_SIZE) {
    *value = v->prefix[v->disk + 1] - v->prefix[v->disk];
    return MZ_OK;
  }
  return MZ_PARAM_ERROR;
}

static int32_t
vol_set_prop(void *stream, int32_t prop, int64_t value) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  if(!v) {
    return MZ_PARAM_ERROR;
  }
  if(prop != MZ_STREAM_PROP_DISK_NUMBER || v->mode != ZIPX_VOL_MODE_DISK) {
    return MZ_PARAM_ERROR;
  }
  if(value < 0) {
    /* minizip-ng passes -1 for entries that live on the same disk as the
       central directory, which is the final volume of the set. */
    v->disk = v->count - 1;
    v->pos = v->prefix[v->disk];
    return MZ_OK;
  }
  if(value >= v->count) {
    return MZ_PARAM_ERROR;
  }
  v->disk = (int32_t)value;
  v->pos = v->prefix[v->disk];
  return MZ_OK;
}

/* ------------------------------------------------------------------------- */

static void vol_destroy(void **stream);

static mz_stream_vtbl vol_vtbl = {
  vol_open, vol_is_open, vol_read, vol_write, vol_tell, vol_seek, vol_close,
  vol_error, NULL, vol_destroy, vol_get_prop, vol_set_prop,
};

static void *
vol_create(void) {
  zipx_volstream_t *v = (zipx_volstream_t *)calloc(1, sizeof(*v));

  if(!v) {
    return NULL;
  }
  v->stream.vtbl = &vol_vtbl;
  v->os_part = -1;
  return (void *)&v->stream;
}

static void
vol_destroy(void **stream) {
  zipx_volstream_t *v;
  int32_t i;

  if(!stream || !*stream) {
    return;
  }
  v = (zipx_volstream_t *)*stream;
  vol_close(&v->stream);
  if(v->os) {
    mz_stream_os_delete(&v->os);
  }
  if(v->paths) {
    for(i = 0; i < v->count; i++) {
      free(v->paths[i]);
    }
    free(v->paths);
  }
  free(v->prefix);
  free(v);
  *stream = NULL;
}

/* ------------------------------------------------------------------------- */

void *
zipx_volstream_create(int32_t mode) {
  void *stream = vol_create();
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  if(!v) {
    return NULL;
  }
  v->mode = (mode == ZIPX_VOL_MODE_DISK) ? ZIPX_VOL_MODE_DISK :
                                           ZIPX_VOL_MODE_CONCAT;
  return stream;
}

void
zipx_volstream_delete(void **stream) {
  mz_stream_delete(stream);   /* routes through vtbl->destroy */
}

int32_t
zipx_volstream_set_parts(void *stream, const char *const *paths, int32_t count) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;
  int32_t i;

  if(!v || !paths || count <= 0) {
    return MZ_PARAM_ERROR;
  }
  v->paths = (char **)calloc((size_t)count, sizeof(*v->paths));
  if(!v->paths) {
    return MZ_MEM_ERROR;
  }
  v->count = count;
  for(i = 0; i < count; i++) {
    size_t len = strlen(paths[i]) + 1;

    v->paths[i] = (char *)malloc(len);
    if(!v->paths[i]) {
      return MZ_MEM_ERROR;
    }
    memcpy(v->paths[i], paths[i], len);
  }
  return MZ_OK;
}

int64_t
zipx_volstream_total(void *stream) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;

  return v ? v->total : -1;
}

const char *
zipx_volstream_describe(void *stream, char *buf, unsigned int size) {
  zipx_volstream_t *v = (zipx_volstream_t *)stream;
  const char *base;

  if(!buf || size == 0) {
    return "";
  }
  if(!v || v->count <= 0) {
    snprintf(buf, size, "(no volumes)");
    return buf;
  }
  base = strrchr(v->paths[0], '/');
#ifdef _WIN32
  {
    const char *alt = strrchr(v->paths[0], '\\');
    if(alt && (!base || alt > base)) {
      base = alt;
    }
  }
#endif
  base = base ? base + 1 : v->paths[0];
  snprintf(buf, size, "%s (%d volumes)", base, (int)v->count);
  return buf;
}
