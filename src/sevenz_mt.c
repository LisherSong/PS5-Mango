#include "sevenz_mt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "7zTypes.h"
#include "7zCrc.h"
#include "Alloc.h"
#include "Lzma2DecMt.h"

/* ------------------------------------------------------------ adapters --
   The SDK's decoders speak ISeqInStream / ISeqOutStream / ICompressProgress;
   the engine speaks plain callbacks.  These three structs translate.  All of
   them run on the calling thread -- MtDec only ever hands output to the
   thread that called Lzma2DecMt_Decode, which is what makes the plain sink
   safe to reuse here. */

typedef struct {
  ISeqInStream vt;
  sz_chain_read_fn read_at;
  void *read_ctx;
  uint64_t pos; /* absolute offset of the next byte to hand out */
  uint64_t end; /* one past the last byte of the packed stream */
} mt_seq_in;

static SRes mt_seq_read(const ISeqInStream *pp, void *buf, size_t *size) {
  mt_seq_in *s = (mt_seq_in *)pp;
  size_t want = *size;

  *size = 0;
  if(want == 0) {
    return SZ_OK;
  }
  if(s->end - s->pos < (uint64_t)want) {
    want = (size_t)(s->end - s->pos);
  }
  if(want != 0 && s->read_at(s->read_ctx, s->pos, buf, want) != 0) {
    return SZ_ERROR_READ;
  }
  s->pos += want;
  *size = want;
  /* A short read means "end of stream" to the SDK; since we only ever hand
     it exactly in_size bytes, hitting the end early is the caller's bug and
     the decoder's outSize check will flag it. */
  return SZ_OK;
}

typedef struct {
  ISeqOutStream vt;
  sz_chain_sink_fn sink;
  void *sink_ctx;
  uint32_t crc;
  int failed;
} mt_seq_out;

static size_t mt_seq_write(const ISeqOutStream *pp, const void *buf,
                           size_t size) {
  mt_seq_out *s = (mt_seq_out *)pp;

  if(s->failed) {
    return 0;
  }
  s->crc = CrcUpdate(s->crc, buf, size);
  if(s->sink(s->sink_ctx, buf, size) != 0) {
    s->failed = 1;
    /* Returning less than `size` tells the SDK the output side is done; it
       reports SZ_ERROR_WRITE. */
    return 0;
  }
  return size;
}

typedef struct {
  ICompressProgress vt;
  sz_chain_cancel_fn cancel;
  void *cancel_ctx;
} mt_progress;

static SRes mt_progress_report(const ICompressProgress *pp, UInt64 in_size,
                               UInt64 out_size) {
  mt_progress *s = (mt_progress *)pp;

  (void)in_size;
  (void)out_size;
  if(s->cancel && s->cancel(s->cancel_ctx)) {
    return SZ_ERROR_PROGRESS;
  }
  return SZ_OK;
}

/* --------------------------------------------------------------- decode --
   Errors are mapped onto sz_chain_err_t so the facade's reporting stays in
   one vocabulary.  SZX_MT_ERR_THREADS is reserved for "the platform cannot
   give me a thread pool": Lzma2DecMt returns SZ_ERROR_THREAD only from its
   threading primitives, everything else is data or memory. */

static void mt_fail(sz_chain_err_t *err, sz_chain_status_t status,
                    const char *fmt, ...) {
  va_list ap;

  if(!err) {
    return;
  }
  err->status = status;
  err->coder = 0;
  err->method = 0x21; /* SZ_M_LZMA2; kept literal to avoid dragging chain.c in */
  err->offset = 0;
  va_start(ap, fmt);
  vsnprintf(err->message, sizeof(err->message), fmt, ap);
  va_end(ap);
}

int szx_mt_decode(sz_chain_read_fn read_at, void *read_ctx, uint64_t in_offset,
                  uint64_t in_size, uint8_t prop, uint64_t out_size,
                  sz_chain_sink_fn sink, void *sink_ctx,
                  sz_chain_cancel_fn cancel, void *cancel_ctx,
                  uint32_t *crc_out, sz_chain_err_t *err) {
  mt_seq_in in;
  mt_seq_out out;
  mt_progress progress;
  CLzma2DecMtProps props;
  CLzma2DecMtHandle mt;
  UInt64 in_processed = 0;
  int is_mt = 0;
  SRes res;

  memset(&in, 0, sizeof(in));
  in.vt.Read = mt_seq_read;
  in.read_at = read_at;
  in.read_ctx = read_ctx;
  in.pos = in_offset;
  in.end = in_offset + in_size;

  memset(&out, 0, sizeof(out));
  out.vt.Write = mt_seq_write;
  out.sink = sink;
  out.sink_ctx = sink_ctx;
  out.crc = CRC_INIT_VAL;

  memset(&progress, 0, sizeof(progress));
  progress.vt.Progress = mt_progress_report;
  progress.cancel = cancel;
  progress.cancel_ctx = cancel_ctx;

  Lzma2DecMtProps_Init(&props);
  props.numThreads = SZX_MT_THREADS;
  props.inBufSize_MT = 1 << 20;

  mt = Lzma2DecMt_Create(&g_Alloc, &g_MidAlloc);
  if(!mt) {
    mt_fail(err, SZ_CHAIN_ERR_INTERNAL, "LZMA2 MT: out of memory");
    return -1;
  }
  res = Lzma2DecMt_Decode(mt, prop, &props, &out.vt, &out_size, 1, &in.vt,
                          &in_processed, &is_mt,
                          cancel ? &progress.vt : NULL);
  Lzma2DecMt_Destroy(mt);

  if(res == SZ_ERROR_THREAD) {
    /* No usable thread pool (pthread init failure, thread creation denied).
       The caller retries on the single-threaded chain path. */
    return SZX_MT_ERR_THREADS;
  }
  if(out.failed) {
    mt_fail(err, SZ_CHAIN_ERR_WRITE, "LZMA2 MT: sink rejected decoded data");
    return -1;
  }
  if(res != SZ_OK) {
    switch(res) {
    case SZ_ERROR_PROGRESS:
      mt_fail(err, SZ_CHAIN_ERR_CANCELED, "canceled");
      break;
    case SZ_ERROR_MEM:
      mt_fail(err, SZ_CHAIN_ERR_INTERNAL, "LZMA2 MT: out of memory");
      break;
    case SZ_ERROR_WRITE:
      mt_fail(err, SZ_CHAIN_ERR_WRITE, "LZMA2 MT: output stream failed");
      break;
    default:
      mt_fail(err, SZ_CHAIN_ERR_DATA, "LZMA2 MT: decode failed (res=%d)",
              (int)res);
      break;
    }
    return -1;
  }
  if(in_processed != in_size) {
    mt_fail(err, SZ_CHAIN_ERR_DATA,
            "LZMA2 MT: consumed %llu of %llu packed bytes",
            (unsigned long long)in_processed, (unsigned long long)in_size);
    return -1;
  }
  if(crc_out) {
    *crc_out = out.crc;
  }
  return 0;
}
