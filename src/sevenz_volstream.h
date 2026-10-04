/* sevenz_volstream -- present a multi-file 7z volume set as one stream.
   part of ps5-web-file-manager

   A split 7z is a plain byte split: `name.7z.001`, `name.7z.002`, ... are
   consecutive slices of one archive, so byte N of the logical archive is byte
   N of the concatenation and every offset stored inside the stream header is
   already absolute. Nothing has to be merged on disk -- a 160 GiB set would
   otherwise need a second 160 GiB scratch copy.

   The LZMA SDK reads through ISeekInStream, so this module implements that
   interface over the ordered part list produced by zipx_volume. The ordered
   list is what makes a set with a hole in it fail loudly instead of decoding
   garbage: zipx_volume names the missing part. */

#ifndef SEVENZ_VOLSTREAM_H
#define SEVENZ_VOLSTREAM_H

#include <stdint.h>

#include "7zTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sevenz_volstream sevenz_volstream;

/* Opens `path` together with every volume of the set it belongs to and exposes
   them as one seekable byte stream. `path` may be any member of the set; an
   ordinary single-file archive is the degenerate one-file case.

   Returns 0 on success, with *is_set set to 1 when the path was part of a
   multi-file set (non-NULL only). Returns -1 on failure and, when `err` is
   non-NULL, stores a malloc'd message the caller must free -- an incomplete
   set reports the missing volume by name. */
int sevenz_volstream_open(sevenz_volstream **out, const char *path, int *is_set,
                          char **err);

/* The stream to hand to SzArEx_Open(); valid until sevenz_volstream_free(). */
ISeekInStream *sevenz_volstream_stream(sevenz_volstream *v);

/* Number of files backing the stream (1 for an ordinary archive). */
uint32_t sevenz_volstream_count(const sevenz_volstream *v);

/* Size of the whole logical archive. */
uint64_t sevenz_volstream_size(const sevenz_volstream *v);

/* Human readable description, e.g. "name.7z (3 volumes)"; writes into buf and
   returns buf. */
const char *sevenz_volstream_describe(const sevenz_volstream *v, char *buf,
                                      unsigned size);

void sevenz_volstream_free(sevenz_volstream *v);

#ifdef __cplusplus
}
#endif

#endif
