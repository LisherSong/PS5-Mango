/* Bits of the zipx_* contract that are not specific to a container format.

   The limit profiles and the status-to-text mapping describe the *engine
   family*, not ZIP, so they live here rather than inside zip_extract.c.  All
   three engines (ZIP, RAR, 7z) link this one object; keeping them in the ZIP
   file would force the RAR and 7z test builds to drag in minizip-ng and zlib
   for the sake of three functions. */

#include "zip_extract.h"

/* Default limits.
 *
 * Tuned to cover real-world PS5 workloads without prompting:
 *   - PS5 system backup archives (~200-300 GiB total, individual chunks
 *     well under 64 GiB)
 *   - 3A-game archives with a single ~300 GiB uncompressed file
 *
 * Safety against decompression bombs is delegated to:
 *   1. `check_space()` (statvfs-based real disk space check) before extract
 *   2. `max_ratio` below (declared compression ratio cap)
 * The size caps here are an early-fail UX guard, not a security boundary.
 */
static const zipx_limits_t k_default_limits = {
  .max_entries = 200000,
  .max_total_bytes = 2ULL * 1024 * 1024 * 1024 * 1024,
  .max_file_bytes = 512ULL * 1024 * 1024 * 1024,
  .max_ratio = 500,
  /* Only entries that would individually materialise >=1 GiB are screened
     by ratio; anything smaller is harmless (bounded by declared size + the
     real free-space check) and is commonly highly compressible in
     legitimate archives. */
  .ratio_min_bytes = 1ULL * 1024 * 1024 * 1024,
  .max_depth = 32,
  .max_name_len = 255,
  .max_path_len = 1024
};

/* Large profile for archives that exceed the default cap.
 *
 *   - max_file_bytes  = 1 TiB   (single uncompressed file)
 *   - max_total_bytes = 4 TiB   (whole archive)
 *   - max_ratio       = 1000    (relaxed ratio cap; check_space still applies)
 *
 * Requires the user to opt in via the web UI (large=1) before these take
 * effect. Default limits must always be strictly smaller than large so the
 * large profile is unambiguously a relaxation.
 */
static const zipx_limits_t k_large_limits = {
  .max_entries = 500000,
  .max_total_bytes = 4ULL * 1024 * 1024 * 1024 * 1024,
  .max_file_bytes = 1ULL * 1024 * 1024 * 1024 * 1024,
  .max_ratio = 1000,
  .ratio_min_bytes = 1ULL * 1024 * 1024 * 1024,
  .max_depth = 32,
  .max_name_len = 255,
  .max_path_len = 1024
};

const zipx_limits_t *
zipx_default_limits(void) {
  return &k_default_limits;
}

const zipx_limits_t *
zipx_limits_profile(int profile) {
  switch(profile) {
  case ZIPX_LIMITS_LARGE:
    return &k_large_limits;
  case ZIPX_LIMITS_DEFAULT:
  default:
    return &k_default_limits;
  }
}

const char *
zipx_status_string(zipx_status_t status) {
  switch(status) {
  case ZIPX_OK: return "ok";
  case ZIPX_ERR_CANCELED: return "canceled";
  case ZIPX_ERR_OPEN: return "cannot open archive";
  case ZIPX_ERR_FORMAT: return "corrupt archive";
  case ZIPX_ERR_UNSUPPORTED: return "unsupported archive";
  case ZIPX_ERR_UNSAFE_NAME: return "unsafe entry name";
  case ZIPX_ERR_SPECIAL: return "unsupported entry type";
  case ZIPX_ERR_DUPLICATE: return "duplicate entry name";
  case ZIPX_ERR_LIMIT_ENTRIES: return "too many entries";
  case ZIPX_ERR_LIMIT_FILE: return "entry too large";
  case ZIPX_ERR_LIMIT_TOTAL: return "archive contents too large";
  case ZIPX_ERR_LIMIT_RATIO: return "compression ratio too high";
  case ZIPX_ERR_LIMIT_DEPTH: return "path too deep";
  case ZIPX_ERR_LIMIT_NAME: return "path too long";
  case ZIPX_ERR_LIMIT_DICT: return "dictionary too large";
  case ZIPX_ERR_CONFLICT: return "target already exists";
  case ZIPX_ERR_PASSWORD: return "password required or wrong";
  case ZIPX_ERR_SPACE: return "not enough space";
  case ZIPX_ERR_IO: return "read or write failed";
  case ZIPX_ERR_CRC: return "crc mismatch";
  default: return "internal error";
  }
}
