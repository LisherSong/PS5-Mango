/* sevenz_chain -- 7z folder (coder chain) decoder.
   part of ps5-web-file-manager

   A 7z archive stores its data as *folders*.  One folder is a small directed
   graph of coders fed by N packed streams and producing a single unpacked
   stream; entries are slices of the folder output (a folder holding several
   entries is what makes an archive "solid").

   The bundled LZMA SDK can decode a folder, but only through `CSzFolder`,
   which is a fixed-size structure capped at 4 coders / 3 bonds.  7-Zip's own
   BCJ2 chain uses 5 coders (BCJ2 plus four LZMA2 streams), so the SDK rejects
   it -- while still listing the archive fine, because its *header* scanner is
   a different, looser parser (64 coders).  The C half of the SDK also has no
   7zAES coder at all.

   This module therefore parses the folder descriptor itself (dynamic arrays,
   up to 64 coders, mirroring the SDK's header scanner) and drives the coder
   graph itself.  Two properties matter:

     * Streaming.  The decoded bytes are pushed into a sink as they are
       produced; a folder is never materialised as a whole, so a multi-gigabyte
       solid block is workable.  The only buffers sized from the archive are
       the LZMA/LZMA2 dictionary, the PPMd model and the three side streams of
       BCJ2 -- each capped by sz_chain_limits_t.

     * Precision.  Every rejection names the coder and the method, and the
       resource limits report the value the archive asked for and the value
       that was allowed, so the UI can say something useful instead of
       "corrupt archive".

   Supported here:
     * Copy, LZMA, LZMA2 and PPMd
     * the Delta filter and the x86 / PPC / IA64 / ARM / ARMT / SPARC branch
       converters
     * BCJ2, whose three side streams are materialised under a limit while
       MAIN keeps streaming
     * 7zAES (method 0x06F10701), the coder 7-Zip wraps around the streams when
       `-p` is used, driven from a caller supplied password

   An encrypted *header* (`-mhe=on`) is not a coder in a folder: it is a second,
   encrypted copy of the archive header that has to be decoded before any folder
   exists at all.  src/sevenz_header.c does that -- by parsing that one folder's
   descriptor and running it through this module, password and all.
*/

#ifndef SEVENZ_CHAIN_H
#define SEVENZ_CHAIN_H

#include <stddef.h>
#include <stdint.h>

/* The SDK's header scanner accepts up to 64 coders per folder; folders in the
   wild have 1-5.  Keeping the same ceiling means "the SDK could list it" and
   "we can decode it" accept the same archives. */
#define SZ_CHAIN_MAX_CODERS 64
#define SZ_CHAIN_MAX_STREAMS 64

/* Ceilings for the buffers whose size comes from the (attacker controlled)
   archive header. */
typedef struct {
  uint64_t max_dict_bytes; /* LZMA / LZMA2 window */
  uint64_t max_ppmd_bytes; /* PPMd model */
  uint64_t max_side_bytes; /* BCJ2 CALL + JUMP + RC together */
  uint32_t max_aes_cycles; /* 7zAES key derivation: 2^n SHA-256 passes */
} sz_chain_limits_t;

#define SZ_CHAIN_LIMITS_DEFAULT 0
#define SZ_CHAIN_LIMITS_LARGE 1
const sz_chain_limits_t *sz_chain_limits_profile(int profile);
const sz_chain_limits_t *sz_chain_default_limits(void);

typedef enum {
  SZ_CHAIN_OK = 0,
  SZ_CHAIN_ERR_PARAM,   /* bad arguments from the caller */
  SZ_CHAIN_ERR_MEM,     /* allocation failed */
  SZ_CHAIN_ERR_HEADER,  /* malformed folder descriptor */
  SZ_CHAIN_ERR_METHOD,  /* coder method not supported */
  SZ_CHAIN_ERR_LAYOUT,  /* coder graph shape not supported */
  SZ_CHAIN_ERR_LIMIT,   /* a sz_chain_limits_t ceiling was hit */
  SZ_CHAIN_ERR_PASSWORD,/* the archive is encrypted and no usable password
                           was supplied (or the one given is wrong) */
  SZ_CHAIN_ERR_READ,    /* the read callback failed */
  SZ_CHAIN_ERR_WRITE,   /* the sink callback failed */
  SZ_CHAIN_ERR_DATA,    /* a decoder rejected the data */
  SZ_CHAIN_ERR_CANCELED,
  SZ_CHAIN_ERR_INTERNAL
} sz_chain_status_t;

typedef struct {
  sz_chain_status_t status;
  int32_t coder;    /* index of the offending coder, -1 when not applicable */
  uint32_t method;  /* its method id, 0 when not applicable */
  uint64_t offset;  /* decoded byte offset at the point of failure */
  char message[192];
} sz_chain_err_t;

const char *sz_chain_status_string(sz_chain_status_t status);

/* "LZMA2", "BCJ2", "7zAES", "unknown 0x1234".  Never returns NULL. */
const char *sz_chain_method_name(uint32_t method);

/* ---------------------------------------------------------------- folder */

typedef struct sz_chain sz_chain;

/* Parses one folder descriptor.

   blob / blob_size
       the CODERS_INFO bytes of this folder, i.e. the range
       `CSzAr::CodersData[FoCodersOffsets[i] .. FoCodersOffsets[i + 1])`.
   pack_positions
       `CSzAr::PackPositions`, num_pack_streams + 1 entries, offsets of the
       packed streams relative to the start of the archive's packed-data area.
   coder_unpack_sizes
       unpacked size of every coder of this folder, in stored coder order, i.e.
       `&CSzAr::CoderUnpackSizes[CSzAr::FoToCoderUnpackSizes[i]]`.
   unpack_size
       `SzAr_GetFolderUnpackSize(&db, i)`.

   Returns 0 on success.  On success *out owns a copy of blob, release it with
   sz_chain_free().  On failure *out is untouched and err describes the
   problem. */
int sz_chain_parse(sz_chain **out, const uint8_t *blob, size_t blob_size,
                   const uint64_t *pack_positions, uint32_t num_pack_streams,
                   const uint64_t *coder_unpack_sizes, uint64_t unpack_size,
                   const sz_chain_limits_t *limits, sz_chain_err_t *err);

void sz_chain_free(sz_chain *c);

uint32_t sz_chain_num_coders(const sz_chain *c);
uint32_t sz_chain_num_pack_streams(const sz_chain *c);
/* Non-zero when the folder contains a 7zAES coder, i.e. when sz_chain_decode()
   will need a password.  Lets a caller ask for one before touching the disk. */
int sz_chain_needs_password(const sz_chain *c);
/* Method id of coder `index`, or -1 when out of range. */
int64_t sz_chain_coder_method(const sz_chain *c, uint32_t index);

/* True when the folder is a single plain LZMA2 coder -- the shape the SDK's
   multithreaded decoder covers.  Fills the coder's props byte and the packed
   input size; both are only valid when this returns non-zero. */
int sz_chain_lzma2_root(const sz_chain *c, uint8_t *prop, uint64_t *in_size);

/* Writes e.g. "LZMA2 + BCJ2 (5 coders, 4 pack streams)" into buf. */
void sz_chain_describe(const sz_chain *c, char *buf, size_t size);

/* Walks every coder and the graph shape without touching any data, so callers
   can refuse an archive before creating anything on disk.  Fills err with the
   same precision sz_chain_decode() would. */
int sz_chain_check(const sz_chain *c, sz_chain_err_t *err);

/* -------------------------------------------------------------- decoding */

/* Fills exactly size bytes at offset inside the packed-data area.
   Returns 0 on success, non-zero on failure. */
typedef int (*sz_chain_read_fn)(void *ctx, uint64_t offset, void *dst,
                                size_t size);

/* Receives the decoded bytes in order.  Returns 0 to continue. */
typedef int (*sz_chain_sink_fn)(void *ctx, const void *data, size_t size);

/* Returns non-zero to abort.  May be NULL. */
typedef int (*sz_chain_cancel_fn)(void *ctx);

/* Decodes the whole folder, pushing the result into sink.

   The bytes are delivered strictly in order and the total is the folder's
   declared unpack size.  When crc_out is not NULL it receives the CRC-32 of
   the delivered bytes, for the caller to compare with the folder CRC.

   `password` is the archive password as UTF-8, or NULL / "" when the caller
   has none.  It is only consulted by folders that contain a 7zAES coder; a
   folder that needs one without a password fails as SZ_CHAIN_ERR_PASSWORD
   before any data is read, so the caller can prompt and retry.  A password
   containing NUL is not supported: 7-Zip stores it as UTF-16LE and the
   conversion stops at the terminator.

   Returns 0 on success, -1 on failure with err filled.  A failing sink is
   reported as SZ_CHAIN_ERR_WRITE; the caller is expected to make its own
   message more specific. */
int sz_chain_decode(sz_chain *c, sz_chain_read_fn read_at, void *read_ctx,
                    sz_chain_sink_fn sink, void *sink_ctx,
                    sz_chain_cancel_fn cancel, void *cancel_ctx,
                    const char *password, uint32_t *crc_out,
                    sz_chain_err_t *err);

#endif /* SEVENZ_CHAIN_H */
