#pragma once

/* Safe RAR extraction engine used by the /api/extract task.
   Wraps the vendored rarlab UnRAR 7.20.1 (third_party/unrar7) through its
   C-compatible DLL API (unrar_c_api.h facade).

   Reuses the zip_extract types so the dispatch layer can call either engine
   through the same status / limits / progress protocol.

   See zip_extract.h for the shared limits, conflict, progress and result types.

   Backend notes (v1.9, unrar 7.20.1):
     * RAR4 and RAR5, any compression version including WinRAR 6/7 "v6".
     * Multi-volume: unrar merges next .partNN.rar by name automatically.
     * Encrypted archives work end-to-end (both `-p` data encryption and
       `-hp` header encryption). Pass the password in `password`; NULL or an
       empty string means "no password supplied". A missing or wrong password
       is reported as ZIPX_ERR_PASSWORD so the caller can prompt and retry.

   See third_party/unrar7/VENDORED.md for full integration notes. */

#include "zip_extract.h"

#include <stdint.h>
#include <stddef.h>

/* Extract rar_path into dst_dir using the same protocol as zipx_extract().
   `password` may be NULL when the archive is not encrypted.
   Returns ZIPX_OK or an error code; *result is always filled in.
   On any failure the staging directory is removed and dst_dir is left as it
   was, except for objects already published with the overwrite policy. */
zipx_status_t rar_extract(const char *rar_path, const char *dst_dir,
                          zipx_conflict_t conflict,
                          const zipx_limits_t *limits,
                          zipx_cancel_fn cancel,
                          zipx_progress_fn progress,
                          void *userdata,
                          const char *password,
                          zipx_result_t *result);