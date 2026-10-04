#pragma once

/* Standalone 7z extraction engine, the third sibling of zip_extract.c and
   rar_extract.c.  Like them it has no HTTP or task dependencies, and it fills
   in the same zipx_result_t so a caller can treat every format alike.

   Input may be a single `name.7z` or a byte-split set (`name.7z.001`, ...):
   both reach the decoder through src/sevenz_volstream.c.

   The publish / staging / rollback / name-validation machinery is mirrored
   from rar_extract.c on purpose -- three self-contained engines is the shape
   this project has settled on, so that a format's bugs stay inside its file.

   Backend notes (LZMA SDK 26.03 + src/sevenz_chain.c):
     * Copy / LZMA / LZMA2 / PPMd, the Delta filter and the x86 / PPC / IA64 /
       ARM / ARMT / SPARC branch converters, BCJ2, and 7zAES.
     * An encrypted *header* (`-mhe=on`) is decrypted by src/sevenz_header.c
       first: the SDK refuses such an archive before any folder is known, so
       the header has to be readable before the SDK is asked to read it.
*/

#include "zip_extract.h"

/* Extract sevenz_path into dst_dir.
   `password` is the archive password as UTF-8, or NULL / "" when the caller
   has none.  It is only consulted by archives that encrypt their streams.

   Returns ZIPX_OK or an error code; *result is always filled in.  A missing or
   wrong password comes back as ZIPX_ERR_PASSWORD so the caller can ask for one
   and retry.  On any failure the staging directory is removed and dst_dir is
   left as it was, except for objects already published under the overwrite
   policy. */
zipx_status_t sevenz_extract(const char *sevenz_path, const char *dst_dir,
                             zipx_conflict_t conflict,
                             const zipx_limits_t *limits,
                             zipx_cancel_fn cancel,
                             zipx_progress_fn progress, void *userdata,
                             const char *password, zipx_result_t *result);
