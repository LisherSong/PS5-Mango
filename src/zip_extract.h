#pragma once

/* Standalone ZIP extraction engine.
   No HTTP / task dependencies: it can be compiled and tested on its own. */

#include <stdint.h>
#include <stddef.h>

#define ZIPX_PATH_MAX 4096

typedef enum {
  ZIPX_PHASE_SCAN = 0,
  ZIPX_PHASE_EXTRACT = 1,
  ZIPX_PHASE_PUBLISH = 2,
  ZIPX_PHASE_CLEANUP = 3
} zipx_phase_t;

typedef enum {
  ZIPX_OK = 0,
  ZIPX_ERR_CANCELED,
  ZIPX_ERR_OPEN,       /* cannot open the archive */
  ZIPX_ERR_FORMAT,     /* corrupt central directory / truncated */
  ZIPX_ERR_UNSUPPORTED,/* multipart or unsupported compression method */
  ZIPX_ERR_UNSAFE_NAME,/* traversal, absolute path, control chars, NUL */
  ZIPX_ERR_SPECIAL,    /* symlink / device / fifo / socket entry */
  ZIPX_ERR_DUPLICATE,  /* repeated entry or file/dir name clash inside zip */
  ZIPX_ERR_LIMIT_ENTRIES,
  ZIPX_ERR_LIMIT_FILE,
  ZIPX_ERR_LIMIT_TOTAL,
  ZIPX_ERR_LIMIT_RATIO,
  ZIPX_ERR_LIMIT_DEPTH,
  ZIPX_ERR_LIMIT_NAME,
  ZIPX_ERR_LIMIT_DICT, /* the archive's dictionary exceeds what we allow
                          (RAR7 headers may ask for up to 64 GiB) */
  ZIPX_ERR_CONFLICT,   /* target already exists for the chosen policy */
  ZIPX_ERR_PASSWORD,   /* the archive is encrypted and the password is missing
                          or wrong; the caller can prompt and retry */
  ZIPX_ERR_SPACE,
  ZIPX_ERR_IO,
  ZIPX_ERR_CRC,
  ZIPX_ERR_INTERNAL
} zipx_status_t;

typedef enum {
  ZIPX_CONFLICT_FAIL = 0,
  ZIPX_CONFLICT_OVERWRITE = 1,
  ZIPX_CONFLICT_MERGE = 2
} zipx_conflict_t;

typedef struct {
  uint64_t max_entries;
  uint64_t max_total_bytes;
  uint64_t max_file_bytes;
  uint32_t max_ratio;   /* uncompressed/compressed, 0 disables */
  /* Entries whose uncompressed size is below this are never ratio-screened.
     Small highly-compressible entries are common in legitimate archives
     (zero-filled placeholders, sparse blobs) and are harmless because the
     actual bytes written are bounded by the declared size and by the real
     free-space check; the ratio screen only needs to catch entries large
     enough to matter. */
  uint64_t ratio_min_bytes;
  uint32_t max_depth;
  uint32_t max_name_len;
  uint32_t max_path_len;
} zipx_limits_t;

typedef struct {
  int phase;
  uint64_t entries_total;
  uint64_t entries_done;
  uint64_t bytes_total;
  uint64_t bytes_done;
  const char *current;  /* entry name being processed, may be NULL */
} zipx_progress_t;

/* Returns non-zero when the caller wants the operation to stop. */
typedef int (*zipx_cancel_fn)(void *userdata);
typedef void (*zipx_progress_fn)(void *userdata, const zipx_progress_t *progress);

typedef struct {
  zipx_status_t status;
  int sys_errno;
  uint64_t entries_total;
  uint64_t entries_done;
  uint64_t bytes_total;
  uint64_t files_created;
  uint64_t dirs_created;
  char detail[ZIPX_PATH_MAX]; /* offending path or the staging directory */
  char message[192];
} zipx_result_t;

const zipx_limits_t *zipx_default_limits(void);

/* Pre-built limit profiles. Use zipx_limits_profile() to look one up.
   ZIPX_LIMITS_DEFAULT is the safe profile shipped by zipx_default_limits().
   ZIPX_LIMITS_LARGE allows archives up to 2 TiB total / 1 TiB per file and
   a 1000:1 compression ratio. The caller is responsible for verifying that
   the PS5 has enough free disk space. */
#define ZIPX_LIMITS_DEFAULT 0
#define ZIPX_LIMITS_LARGE   1

const zipx_limits_t *zipx_limits_profile(int profile);
const char *zipx_status_string(zipx_status_t status);

/* Extract zip_path into dst_dir.
   `password` may be NULL or empty when the archive is not encrypted; it is
   used for both ZIP encryption schemes, traditional PKWARE ("ZipCrypto") and
   WinZip AES. A missing or wrong password is reported as ZIPX_ERR_PASSWORD,
   which the caller is expected to turn into a prompt and retry.
   Returns ZIPX_OK or an error code; *result is always filled in.
   On any failure the staging directory is removed and dst_dir is left as it
   was, except for objects already published with the overwrite policy. */
zipx_status_t zipx_extract(const char *zip_path, const char *dst_dir,
                           zipx_conflict_t conflict,
                           const zipx_limits_t *limits,
                           zipx_cancel_fn cancel,
                           zipx_progress_fn progress,
                           void *userdata,
                           const char *password,
                           zipx_result_t *result);
