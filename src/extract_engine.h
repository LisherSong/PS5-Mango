#ifndef EXTRACT_ENGINE_H
#define EXTRACT_ENGINE_H
/* Mango-facing entry point for the archive engines (ZIP / RAR / 7z, incl.
 * encrypted + multi-volume — the inherited, host-tested leaves).
 *
 * Why a new header instead of the inherited extract.h: that one is the OLD
 * libmicrohttpd endpoint (`api_extract(MHD_Connection *, …)`), so it drags the
 * whole server in and cannot be called from the worker or tested off-target.
 * This is the replacement contract — transport-free, one call, progress and
 * cancel by callback.
 *
 * The adapter (src/extract_engine.c) keeps the dispatch rules that the old
 * endpoint already proved: format by file name, volume-set detection by member
 * name, and the one honest refusal ('x.rar.001' sets, which unrar cannot open
 * because it chains 'x.partNN.rar' itself).
 *
 * `progress` / `cancel` may be NULL.
 *
 * progress() gets the phase, the running byte count AND the total the engine
 * learned during its scan pass, plus the entry currently being written.
 * Forwarding the byte count alone (what this used to do) left the task centre
 * with no denominator, so every extraction bar sat at 0 % and the only honest
 * thing the UI could show was a spinner. Returning non-zero aborts the unpack
 * (the engines are also polled through `cancel`, which is the cheaper channel).
 * During MANGO_PHASE_SCAN `total` is still growing — the caller must not
 * publish it yet. */
#include "mango_common.h"

typedef enum {
  MANGO_PHASE_SCAN = 0,
  MANGO_PHASE_EXTRACT = 1,
  MANGO_PHASE_PUBLISH = 2,
  MANGO_PHASE_CLEANUP = 3
} mango_extract_phase_t;

typedef int (*mango_extract_progress_fn)(int phase, uint64_t done,
                                         uint64_t total, const char *current,
                                         void *ctx);
typedef int (*mango_extract_cancel_fn)(void *ctx);

typedef enum {
  MANGO_CONFLICT_FAIL = 0,
  MANGO_CONFLICT_OVERWRITE = 1,
  MANGO_CONFLICT_MERGE = 2
} mango_conflict_t;

typedef struct {
  mango_conflict_t           conflict;
  const char                *password; /* NULL or "" when the caller has none */
  int                        large;    /* !=0 -> the LARGE limit profile
                                          (2 TiB total / 1 TiB per file). The
                                          caller must have checked free space;
                                          the safe profile is the default. */
  mango_extract_progress_fn  progress;
  mango_extract_cancel_fn    cancel;
  void                      *ctx;
} mango_extract_req_t;

typedef struct {
  mango_err_t err;
  char message[192]; /* human-readable, already translated by the engines */
} mango_extract_result_t;

mango_err_t mango_extract(const char *archive_path, const char *dst_dir,
                          const mango_extract_req_t *req,
                          mango_extract_result_t *out /* may be NULL */);
#endif
