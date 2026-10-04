/* Multithreaded LZMA2 decode for the 7z engine.
 *
 * Most 7z archives are a single plain LZMA2 coder (7-Zip's -m0=lzma2
 * default).  For that shape the SDK's own parallel decoder -- the same code
 * 7-Zip runs for -mmt -- replaces the single-threaded chain walk and decodes
 * consecutive LZMA2 blocks on worker threads while the main thread streams
 * the output into the staging sink.  Measured on a 329 MiB fixture this is
 * worth ~1.7x on an 8-core host, on top of the assembly kernel.
 *
 * Threads are rented, not owned: any thread error falls back to the caller's
 * single-threaded path, so a platform without working pthreads only ever
 * loses speed, never correctness. */

#ifndef SEVENZ_MT_H
#define SEVENZ_MT_H

#include "sevenz_chain.h"

/* 8-core Zen 2 on the PS5: 4 decoders leave the HTTP server, the task
   system and the kernel half of the machine. */
#define SZX_MT_THREADS 8

/* Decodes one folder that sz_chain_lzma2_root() has recognised.  The
   callbacks mirror sz_chain_decode()'s: read_at/ctx for the packed data,
   sink/ctx for the decoded bytes (both run on the calling thread; the sink
   sees the same ordered byte stream the chain would have produced).
   cancel/ctx is polled from the progress callback and may be NULL.
   crc_out, when not NULL, receives the CRC-32 of the delivered bytes.
   err, when not NULL, receives a chain-style error description.
   Returns 0 on success; SZX_MT_ERR_THREADS means "no working thread pool"
   and the caller should retry single-threaded; other failures are terminal. */
#define SZX_MT_ERR_THREADS 2

int szx_mt_decode(sz_chain_read_fn read_at, void *read_ctx,
                  uint64_t in_offset, uint64_t in_size, uint8_t prop,
                  uint64_t out_size, sz_chain_sink_fn sink, void *sink_ctx,
                  sz_chain_cancel_fn cancel, void *cancel_ctx,
                  uint32_t *crc_out, sz_chain_err_t *err);

#endif /* SEVENZ_MT_H */
