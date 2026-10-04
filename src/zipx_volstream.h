/* zipx_volstream -- present a multi-file archive volume set as one stream.
   part of ps5-web-file-manager

   Two split layouts exist in the wild and they need different behaviour:

   ZIPX_VOL_MODE_CONCAT (byte split)
     `name.zip.001`, `name.zip.002`, ... (7-Zip) and `name.part1.zip`,
     `name.part2.zip` (WinRAR). Each part is a byte slice of one archive, so
     byte N of the logical archive is byte N of the concatenation and every
     offset stored inside the archive is already absolute. Offsets are passed
     through untouched and the disk properties are reported as unsupported so
     minizip-ng keeps using absolute offsets.

   ZIPX_VOL_MODE_DISK (zip split disks)
     `name.z01`, `name.z02`, ..., `name.zip` (Info-ZIP / PKZIP style). The
     central directory stores the offset of a local header relative to the
     disk it starts on, so minizip-ng switches the active disk through
     MZ_STREAM_PROP_DISK_NUMBER before seeking. Seek/tell are relative to the
     active disk here, which is exactly what mz_zip_entry_seek_local_header
     expects, and the stream starts on the last disk because that is where the
     end-of-central-directory record lives. */

#ifndef ZIPX_VOLSTREAM_H
#define ZIPX_VOLSTREAM_H

#include <stdint.h>

#include "zipx_volume.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates a stream handle; pass it to mz_stream_open() afterwards. */
void *zipx_volstream_create(int32_t mode);

/* Deletes a handle created above (safe with *stream == NULL). */
void zipx_volstream_delete(void **stream);

/* Copies the ordered part paths into the handle. Must be called before the
   stream is opened. Returns MZ_OK (0) or MZ_MEM_ERROR (-4). */
int32_t zipx_volstream_set_parts(void *stream, const char *const *paths,
                                 int32_t count);

/* Total logical size (sum of the part sizes), or -1 when not resolved. */
int64_t zipx_volstream_total(void *stream);

/* Human readable description of the set, e.g. "name.z01 (3 volumes)".
   Writes into buf and returns buf. */
const char *zipx_volstream_describe(void *stream, char *buf, unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
