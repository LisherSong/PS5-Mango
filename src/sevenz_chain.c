/* sevenz_chain -- 7z folder (coder chain) decoder.
   part of ps5-web-file-manager

   See sevenz_chain.h for the rationale.  Implementation notes:

   Header parsing follows SzGetNextFolderItem() from the LZMA SDK byte for
   byte, only with dynamic arrays instead of the fixed CSzFolder.  The coder
   graph is then driven as a pull pipeline: every coder answers "give me up to
   N bytes of your output", pulling from its own upstream.  Nothing is decoded
   ahead of what the sink asked for, so a solid folder streams.

   Filters are applied chunk by chunk.  The 7z branch converters are documented
   to be restartable -- they return the position after the last byte they could
   fully process and leave at most LookAhead bytes for the next call -- so the
   unconverted tail is carried across chunk boundaries while the program
   counter keeps advancing.  That makes a filtered chain stream as well as a
   plain one.

   BCJ2 is the one shape that cannot stream end to end: it needs its CALL,
   JUMP and RC inputs available at arbitrary points, so those three are
   materialised (capped by sz_chain_limits_t::max_side_bytes) while the MAIN
   stream keeps streaming through.

   Every rejection names the coder and the method, and every resource limit
   reports what the archive asked for and what was allowed.
*/

#include "sevenz_chain.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7zTypes.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"
#include "Ppmd7.h"
#include "Delta.h"
#include "Bra.h"
#include "Bcj2.h"
#include "Aes.h"
#include "Sha256.h"

/* ------------------------------------------------------------------ ids */

#define SZ_M_COPY 0x00u
#define SZ_M_DELTA 0x03u
#define SZ_M_LZMA2 0x21u
#define SZ_M_LZMA 0x030101u
#define SZ_M_PPMD 0x030401u
#define SZ_M_AES 0x06F10701u
#define SZ_M_BCJ 0x03030103u
#define SZ_M_PPC 0x03030205u
#define SZ_M_IA64 0x03030401u
#define SZ_M_ARM 0x03030501u
#define SZ_M_ARMT 0x03030701u
#define SZ_M_SPARC 0x03030805u
#define SZ_M_BCJ2 0x0303011Bu

enum {
  SZ_N_PACK = 0,
  SZ_N_COPY,
  SZ_N_LZMA,
  SZ_N_LZMA2,
  SZ_N_PPMD,
  SZ_N_FILTER,
  SZ_N_BCJ2,
  SZ_N_AES
};

enum {
  SZ_F_DELTA = 1,
  SZ_F_X86,
  SZ_F_PPC,
  SZ_F_IA64,
  SZ_F_SPARC,
  SZ_F_ARM,
  SZ_F_ARMT
};

/* Pull granularity.  7z stores compressed data in LZMA2 chunks of at most
   64 KiB plus a 6 byte header, so a 256 KiB staging buffer always holds a
   complete chunk and the decoder can never stall for lack of contiguity. */
#define SZ_IN_CHUNK (1u << 18)
#define SZ_FILT_CHUNK (1u << 17)
#define SZ_BCJ2_MAIN_CHUNK (1u << 17)
/* How much the top of the chain is asked for per pull.  Raising this from
   64 KiB to 1 MiB was measured and changed nothing (1.39 s vs 1.40 s on a
   330 MiB folder), so the cost is not in per-pull dispatch -- see
   docs/EXTRACTION-PERF.md.  Kept at 64 KiB: smaller cache footprint. */
#define SZ_OUT_CHUNK (1u << 16)
/* Branch converters keep a few tail bytes for their next call. */
#define SZ_FILT_LOOKAHEAD 8
/* Refill BCJ2's MAIN stream while this few bytes are left. */
#define SZ_BCJ2_KEEP 8

static const ISzAlloc g_alloc = { SzAlloc, SzFree };

/* ------------------------------------------------------------ utilities */

static void err_set(sz_chain_err_t *err, sz_chain_status_t status,
                    int32_t coder, uint32_t method, uint64_t offset,
                    const char *fmt, ...) {
  va_list ap;

  if(!err) return;
  err->status = status;
  err->coder = coder;
  err->method = method;
  err->offset = offset;
  if(fmt) {
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
  } else {
    err->message[0] = 0;
  }
}

const char *sz_chain_status_string(sz_chain_status_t status) {
  switch(status) {
  case SZ_CHAIN_OK: return "ok";
  case SZ_CHAIN_ERR_PARAM: return "invalid argument";
  case SZ_CHAIN_ERR_MEM: return "out of memory";
  case SZ_CHAIN_ERR_HEADER: return "corrupt 7z folder header";
  case SZ_CHAIN_ERR_METHOD: return "unsupported compression method";
  case SZ_CHAIN_ERR_LAYOUT: return "unsupported coder chain";
  case SZ_CHAIN_ERR_LIMIT: return "resource limit exceeded";
  case SZ_CHAIN_ERR_PASSWORD: return "password required or wrong";
  case SZ_CHAIN_ERR_READ: return "read failed";
  case SZ_CHAIN_ERR_WRITE: return "write failed";
  case SZ_CHAIN_ERR_DATA: return "corrupt compressed data";
  case SZ_CHAIN_ERR_CANCELED: return "canceled";
  case SZ_CHAIN_ERR_INTERNAL: return "internal error";
  }
  return "unknown error";
}

const char *sz_chain_method_name(uint32_t method) {
  switch(method) {
  case SZ_M_COPY: return "Copy";
  case SZ_M_DELTA: return "Delta";
  case SZ_M_LZMA2: return "LZMA2";
  case SZ_M_LZMA: return "LZMA";
  case SZ_M_PPMD: return "PPMd";
  case SZ_M_BCJ: return "BCJ";
  case SZ_M_PPC: return "PPC";
  case SZ_M_IA64: return "IA64";
  case SZ_M_ARM: return "ARM";
  case SZ_M_ARMT: return "ARMT";
  case SZ_M_SPARC: return "SPARC";
  case SZ_M_BCJ2: return "BCJ2";
  case SZ_M_AES: return "7zAES";
  default: return NULL;
  }
}

static const char *method_label(uint32_t method, char *scratch,
                                size_t scratch_size) {
  const char *name = sz_chain_method_name(method);
  if(name) return name;
  snprintf(scratch, scratch_size, "unknown 0x%X", (unsigned)method);
  return scratch;
}

/* ---------------------------------------------------------------- 7zAES */

/* The SDK documents the AES entry points as wanting 16-byte aligned pointers
   (its SSE path loads the round keys and the data with aligned moves), so the
   per-node AES state is declared with that alignment. */
#if defined(__GNUC__) || defined(__clang__)
#define SZ_AES_ALIGN __attribute__((aligned(16)))
#else
#define SZ_AES_ALIGN
#endif

/* 7-Zip always uses AES-256 for 7zAES (CAesCbcDecoder(kKeySize)). */
#define SZ_AES_KEY_SIZE 32
/* numCyclesPower 0x3F means "no key derivation": the key is salt + password. */
#define SZ_AES_CYCLES_NO_KDF 0x3F
/* Longer than anything a person types; the password is converted to UTF-16LE
   before it is hashed, so this bounds that buffer too. */
#define SZ_AES_MAX_PASSWORD 4096

/* Splits the 7zAES property bytes into the three values they carry.  Mirrors
   CDecoder::SetDecoderProperties2() in 7-Zip:

   data[0]  bit 0..5   numCyclesPower
            bit 7       a salt is present   (16 + high nibble of data[1] bytes)
            bit 6       an IV is present    (16 + low  nibble of data[1] bytes)
   data[1]  high nibble extends saltSize, low nibble extends ivSize
   data[2]  salt, then IV, each only as long as its flag says

   Returns 0 on success and -1 when the bytes contradict the lengths they
   declare.  A 0 byte property block is legal: it means the two defaults. */
static int aes_props_layout(const uint8_t *p, uint32_t size, uint32_t *cycles,
                            uint32_t *salt_size, uint32_t *iv_size) {
  uint32_t b0, b1;

  *cycles = 0;
  *salt_size = 0;
  *iv_size = 0;
  if(size == 0) return 0;
  b0 = p[0];
  *cycles = b0 & 0x3Fu;
  if((b0 & 0xC0u) == 0) return size == 1 ? 0 : -1;
  if(size <= 1) return -1;
  b1 = p[1];
  *salt_size = ((b0 >> 7) & 1u) + (b1 >> 4);
  *iv_size = ((b0 >> 6) & 1u) + (b1 & 0x0Fu);
  return (2 + *salt_size + *iv_size == size) ? 0 : -1;
}

static void aes_put16(uint8_t *dst, size_t *pos, uint32_t unit) {
  dst[(*pos)++] = (uint8_t)(unit & 0xFFu);
  dst[(*pos)++] = (uint8_t)((unit >> 8) & 0xFFu);
}

/* UTF-8 to UTF-16LE, the encoding 7-Zip hashes the password in.  Returns the
   number of bytes written, or 0 when src is not valid UTF-8 or does not fit.
   An embedded NUL ends the password, as it does in 7-Zip. */
static size_t utf8_to_utf16le(const char *src, uint8_t *dst,
                              size_t dst_size) {
  const uint8_t *p = (const uint8_t *)src;
  size_t out = 0;

  while(*p) {
    uint32_t cp;
    unsigned extra, i;

    if(*p < 0x80) {
      cp = *p++;
      extra = 0;
    } else if((*p & 0xE0) == 0xC0) {
      cp = (*p++) & 0x1Fu;
      extra = 1;
    } else if((*p & 0xF0) == 0xE0) {
      cp = (*p++) & 0x0Fu;
      extra = 2;
    } else if((*p & 0xF8) == 0xF0) {
      cp = (*p++) & 0x07u;
      extra = 3;
    } else {
      return 0;
    }
    for(i = 0; i < extra; i++) {
      if((*p & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (uint32_t)((*p++) & 0x3Fu);
    }
    if(cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
    if(cp >= 0x10000u) {
      uint32_t v = cp - 0x10000u;
      if(out + 4 > dst_size) return 0;
      aes_put16(dst, &out, 0xD800u | (v >> 10));
      aes_put16(dst, &out, 0xDC00u | (v & 0x3FFu));
    } else {
      if(out + 2 > dst_size) return 0;
      aes_put16(dst, &out, cp);
    }
  }
  return out;
}

/* CKeyInfo::CalcKey(): one running SHA-256 over (salt || password || counter)
   repeated 1 << cycles times, where counter is a little endian 64-bit index. */
static void aes_derive_key(const uint8_t *salt, uint32_t salt_size,
                           const uint8_t *pwd, size_t pwd_size,
                           uint32_t cycles, uint8_t key[SZ_AES_KEY_SIZE]) {
  CSha256 sha;
  uint32_t rounds, i;
  uint8_t tail[8];

  if(cycles == SZ_AES_CYCLES_NO_KDF) {
    size_t pos = 0, k;
    for(i = 0; i < salt_size && pos < SZ_AES_KEY_SIZE; i++) key[pos++] = salt[i];
    for(k = 0; k < pwd_size && pos < SZ_AES_KEY_SIZE; k++) key[pos++] = pwd[k];
    for(; pos < SZ_AES_KEY_SIZE; pos++) key[pos] = 0;
    return;
  }

  Sha256_Init(&sha);
  rounds = (uint32_t)1 << cycles;
  for(i = 0; i < rounds; i++) {
    if(salt_size) Sha256_Update(&sha, salt, (size_t)salt_size);
    if(pwd_size) Sha256_Update(&sha, pwd, pwd_size);
    tail[0] = (uint8_t)i;
    tail[1] = (uint8_t)(i >> 8);
    tail[2] = (uint8_t)(i >> 16);
    tail[3] = (uint8_t)(i >> 24);
    tail[4] = 0;
    tail[5] = 0;
    tail[6] = 0;
    tail[7] = 0;
    Sha256_Update(&sha, tail, sizeof(tail));
  }
  Sha256_Final(&sha, key);
}

/* -------------------------------------------------------------- limits */

static const sz_chain_limits_t g_limits_default = {
  (uint64_t)512 << 20,  /* LZMA / LZMA2 dictionary */
  (uint64_t)256 << 20,  /* PPMd model */
  (uint64_t)256 << 20,  /* BCJ2 side streams */
  24                    /* 7zAES: 2^24 SHA-256 passes is already ~8 s of work,
                           and numCyclesPower comes from the archive */
};

static const sz_chain_limits_t g_limits_large = {
  (uint64_t)1536 << 20,
  (uint64_t)1024 << 20,
  (uint64_t)1024 << 20,
  24
};

const sz_chain_limits_t *sz_chain_limits_profile(int profile) {
  return profile == SZ_CHAIN_LIMITS_LARGE ? &g_limits_large
                                          : &g_limits_default;
}

const sz_chain_limits_t *sz_chain_default_limits(void) {
  return &g_limits_default;
}

/* ------------------------------------------------------- reader helpers */

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} sz_rdr;

static int rdr_byte(sz_rdr *r, uint8_t *out) {
  if(r->p >= r->end) return -1;
  *out = *r->p++;
  return 0;
}

/* 7z variable length number: a unary length prefix in the top bits of the
   first byte, then little endian payload bytes.  Mirrors ReadNumber(). */
static int rdr_num(sz_rdr *r, uint64_t *out) {
  uint8_t first, tmp, mask;
  unsigned i;
  uint64_t value;

  if(rdr_byte(r, &first) != 0) return -1;
  if((first & 0x80) == 0) {
    *out = first;
    return 0;
  }
  if(rdr_byte(r, &tmp) != 0) return -1;
  if((first & 0x40) == 0) {
    *out = ((uint64_t)(first & 0x3F) << 8) | tmp;
    return 0;
  }
  if(rdr_byte(r, &mask) != 0) return -1;
  value = (uint64_t)tmp | ((uint64_t)mask << 8);
  mask = 0x20;
  for(i = 2 * 8; i < 8 * 8; i += 8) {
    uint8_t b;
    if((first & mask) == 0) {
      value |= ((uint64_t)(first & (uint8_t)(mask - 1))) << i;
      break;
    }
    mask >>= 1;
    if(rdr_byte(r, &b) != 0) return -1;
    value |= ((uint64_t)b << i);
  }
  *out = value;
  return 0;
}

static int rdr_num32(sz_rdr *r, uint32_t *out) {
  uint64_t v;
  if(rdr_num(r, &v) != 0) return -1;
  if(v > 0x7FFFFFFFu) return -1;
  *out = (uint32_t)v;
  return 0;
}

/* --------------------------------------------------------------- folder */

typedef struct {
  uint32_t method;
  uint32_t num_in_streams;
  uint32_t first_in_stream;
  uint32_t props_off;
  uint32_t props_size;
} sz_coder;

typedef struct {
  uint32_t in_index;
  uint32_t out_coder;
} sz_bond;

typedef struct {
  int32_t from_coder; /* >= 0: output of that coder, -1: packed stream */
  uint32_t pack_index;
} sz_source;

struct sz_chain {
  uint8_t *blob;
  size_t blob_size;
  uint32_t num_coders;
  uint32_t num_bonds;
  uint32_t num_pack_streams;
  uint32_t unpack_coder;
  uint32_t total_streams;
  uint64_t unpack_size;
  sz_chain_limits_t limits;
  uint64_t pack_positions[SZ_CHAIN_MAX_STREAMS + 1];
  uint32_t pack_stream_idx[SZ_CHAIN_MAX_STREAMS];
  uint64_t coder_unpack_sizes[SZ_CHAIN_MAX_CODERS];
  sz_coder coder[SZ_CHAIN_MAX_CODERS];
  sz_bond bond[SZ_CHAIN_MAX_CODERS];
  sz_source source[SZ_CHAIN_MAX_STREAMS];
};

static int method_kind(uint32_t method) {
  switch(method) {
  case SZ_M_COPY: return SZ_N_COPY;
  case SZ_M_LZMA: return SZ_N_LZMA;
  case SZ_M_LZMA2: return SZ_N_LZMA2;
  case SZ_M_PPMD: return SZ_N_PPMD;
  case SZ_M_BCJ2: return SZ_N_BCJ2;
  case SZ_M_AES: return SZ_N_AES;
  case SZ_M_DELTA:
  case SZ_M_BCJ:
  case SZ_M_PPC:
  case SZ_M_IA64:
  case SZ_M_SPARC:
  case SZ_M_ARM:
  case SZ_M_ARMT: return SZ_N_FILTER;
  default: return -1;
  }
}

static int filter_kind(uint32_t method) {
  switch(method) {
  case SZ_M_DELTA: return SZ_F_DELTA;
  case SZ_M_BCJ: return SZ_F_X86;
  case SZ_M_PPC: return SZ_F_PPC;
  case SZ_M_IA64: return SZ_F_IA64;
  case SZ_M_SPARC: return SZ_F_SPARC;
  case SZ_M_ARM: return SZ_F_ARM;
  case SZ_M_ARMT: return SZ_F_ARMT;
  default: return 0;
  }
}

int sz_chain_parse(sz_chain **out, const uint8_t *blob, size_t blob_size,
                   const uint64_t *pack_positions, uint32_t num_pack_streams,
                   const uint64_t *coder_unpack_sizes, uint64_t unpack_size,
                   const sz_chain_limits_t *limits, sz_chain_err_t *err) {
  sz_chain *c;
  sz_rdr r;
  uint32_t i, j, running_in = 0;
  uint32_t num_unused;

  if(!out || !blob || !pack_positions || !coder_unpack_sizes) {
    err_set(err, SZ_CHAIN_ERR_PARAM, -1, 0, 0, "missing folder metadata");
    return -1;
  }
  *out = NULL;
  if(num_pack_streams > SZ_CHAIN_MAX_STREAMS) {
    err_set(err, SZ_CHAIN_ERR_LIMIT, -1, 0, 0,
            "folder declares %u packed streams, at most %u are supported",
            (unsigned)num_pack_streams, (unsigned)SZ_CHAIN_MAX_STREAMS);
    return -1;
  }

  c = (sz_chain *)calloc(1, sizeof(*c));
  if(!c) {
    err_set(err, SZ_CHAIN_ERR_MEM, -1, 0, 0, "cannot allocate folder state");
    return -1;
  }
  c->limits = limits ? *limits : g_limits_default;
  c->unpack_size = unpack_size;
  c->num_pack_streams = num_pack_streams;
  c->blob = (uint8_t *)malloc(blob_size ? blob_size : 1);
  if(!c->blob) {
    free(c);
    err_set(err, SZ_CHAIN_ERR_MEM, -1, 0, 0, "cannot copy the folder header");
    return -1;
  }
  memcpy(c->blob, blob, blob_size);
  c->blob_size = blob_size;
  for(i = 0; i <= num_pack_streams; i++)
    c->pack_positions[i] = pack_positions[i];

  r.p = c->blob;
  r.end = c->blob + c->blob_size;

  if(rdr_num32(&r, &c->num_coders) != 0 || c->num_coders == 0) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
            "folder header does not start with a coder count");
    goto bad;
  }
  if(c->num_coders > SZ_CHAIN_MAX_CODERS) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
            "folder has %u coders, at most %u are supported",
            (unsigned)c->num_coders, (unsigned)SZ_CHAIN_MAX_CODERS);
    goto bad;
  }

  for(i = 0; i < c->num_coders; i++) {
    sz_coder *cd = &c->coder[i];
    uint8_t main_byte;
    uint32_t id_size, k;
    uint64_t id = 0;

    if(rdr_byte(&r, &main_byte) != 0) {
      err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, 0, 0,
              "truncated folder header at coder %u", (unsigned)i);
      goto bad;
    }
    if(main_byte & 0xC0) {
      err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, 0, 0,
              "coder %u uses reserved flag bits", (unsigned)i);
      goto bad;
    }
    id_size = main_byte & 0x0F;
    if(id_size > 4u) {
      err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, 0, 0,
              "coder %u has a %u byte method id", (unsigned)i,
              (unsigned)id_size);
      goto bad;
    }
    for(k = 0; k < id_size; k++) {
      uint8_t b;
      if(rdr_byte(&r, &b) != 0) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, 0, 0,
                "truncated method id for coder %u", (unsigned)i);
        goto bad;
      }
      id = (id << 8) | b;
    }
    cd->method = (uint32_t)id;
    cd->num_in_streams = 1;

    if(main_byte & 0x10) {
      uint32_t in_streams, out_streams;
      if(rdr_num32(&r, &in_streams) != 0 || rdr_num32(&r, &out_streams) != 0) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, cd->method, 0,
                "truncated stream counts for coder %u", (unsigned)i);
        goto bad;
      }
      if(out_streams != 1 || in_streams == 0 ||
         in_streams > SZ_CHAIN_MAX_STREAMS) {
        err_set(err, SZ_CHAIN_ERR_LAYOUT, (int32_t)i, cd->method, 0,
                "coder %u declares %u input / %u output streams", (unsigned)i,
                (unsigned)in_streams, (unsigned)out_streams);
        goto bad;
      }
      cd->num_in_streams = in_streams;
    }
    if(cd->num_in_streams > SZ_CHAIN_MAX_STREAMS - running_in) {
      err_set(err, SZ_CHAIN_ERR_LAYOUT, (int32_t)i, cd->method, 0,
              "folder has too many input streams");
      goto bad;
    }
    cd->first_in_stream = running_in;
    running_in += cd->num_in_streams;

    if(main_byte & 0x20) {
      uint32_t props_size;
      if(rdr_num32(&r, &props_size) != 0) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, cd->method, 0,
                "truncated properties size for coder %u", (unsigned)i);
        goto bad;
      }
      if((size_t)(r.end - r.p) < (size_t)props_size) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, cd->method, 0,
                "properties of coder %u run past the folder header",
                (unsigned)i);
        goto bad;
      }
      cd->props_off = (uint32_t)(r.p - c->blob);
      cd->props_size = props_size;
      r.p += props_size;
    }
  }

  c->total_streams = running_in;
  if(c->total_streams == 0) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0, "folder has no input streams");
    goto bad;
  }

  /* numCoders - 1 bonds, each (input stream index, source coder index). */
  c->num_bonds = c->num_coders - 1;
  if(c->total_streams < c->num_bonds) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
            "folder has fewer streams (%u) than bonds (%u)",
            (unsigned)c->total_streams, (unsigned)c->num_bonds);
    goto bad;
  }
  for(i = 0; i < c->num_bonds; i++) {
    uint32_t in_index, out_coder;
    if(rdr_num32(&r, &in_index) != 0 || rdr_num32(&r, &out_coder) != 0) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
              "truncated bond list at bond %u", (unsigned)i);
      goto bad;
    }
    if(in_index >= c->total_streams || out_coder >= c->num_coders) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
              "bond %u refers to stream %u / coder %u out of range",
              (unsigned)i, (unsigned)in_index, (unsigned)out_coder);
      goto bad;
    }
    c->bond[i].in_index = in_index;
    c->bond[i].out_coder = out_coder;
  }

  if((c->total_streams - c->num_bonds) != num_pack_streams) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
            "folder header implies %u packed streams, the archive declares %u",
            (unsigned)(c->total_streams - c->num_bonds),
            (unsigned)num_pack_streams);
    goto bad;
  }
  num_unused = c->total_streams - c->num_bonds;

  for(i = 0; i < num_unused; i++) {
    uint32_t idx;
    if(num_unused == 1) {
      uint32_t s;
      for(s = 0; s < c->total_streams; s++) {
        int used = 0;
        for(j = 0; j < c->num_bonds; j++)
          if(c->bond[j].in_index == s) used = 1;
        if(!used) break;
      }
      if(s == c->total_streams) {
        err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
                "folder has no free stream for its packed data");
        goto bad;
      }
      idx = s;
    } else {
      if(rdr_num32(&r, &idx) != 0 || idx >= c->total_streams) {
        err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
                "packed stream %u is out of range", (unsigned)i);
        goto bad;
      }
    }
    for(j = 0; j < i; j++) {
      if(c->pack_stream_idx[j] == idx) {
        err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
                "packed stream %u is listed twice", (unsigned)idx);
        goto bad;
      }
    }
    c->pack_stream_idx[i] = idx;
  }

  /* Stream -> source table. */
  for(i = 0; i < c->total_streams; i++) {
    c->source[i].from_coder = -1;
    c->source[i].pack_index = 0;
  }
  for(i = 0; i < c->num_bonds; i++) {
    uint32_t s = c->bond[i].in_index;
    if(c->source[s].from_coder >= 0) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
              "stream %u is fed by two coders", (unsigned)s);
      goto bad;
    }
    c->source[s].from_coder = (int32_t)c->bond[i].out_coder;
  }
  for(i = 0; i < num_unused; i++) {
    uint32_t s = c->pack_stream_idx[i];
    if(c->source[s].from_coder >= 0) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
              "packed stream %u is also fed by a coder", (unsigned)s);
      goto bad;
    }
    c->source[s].pack_index = i;
  }
  for(i = 0; i < c->total_streams; i++) {
    int is_pack = 0;
    if(c->source[i].from_coder >= 0) continue;
    for(j = 0; j < num_unused; j++)
      if(c->pack_stream_idx[j] == i) is_pack = 1;
    if(!is_pack) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0,
              "input stream %u has no source", (unsigned)i);
      goto bad;
    }
  }

  {
    int found = 0;
    for(i = 0; i < c->num_coders; i++) {
      int used = 0;
      for(j = 0; j < c->num_bonds; j++)
        if(c->bond[j].out_coder == i) used = 1;
      if(!used) {
        c->unpack_coder = i;
        found = 1;
        break;
      }
    }
    if(!found) {
      err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0, "folder has no output coder");
      goto bad;
    }
  }

  for(i = 0; i < c->num_coders; i++)
    c->coder_unpack_sizes[i] = coder_unpack_sizes[i];

  if(coder_unpack_sizes[c->unpack_coder] != unpack_size) {
    err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)c->unpack_coder,
            c->coder[c->unpack_coder].method, 0,
            "folder unpack size %llu does not match the output coder size %llu",
            (unsigned long long)unpack_size,
            (unsigned long long)coder_unpack_sizes[c->unpack_coder]);
    goto bad;
  }

  *out = c;
  return 0;

bad:
  sz_chain_free(c);
  return -1;
}

void sz_chain_free(sz_chain *c) {
  if(!c) return;
  free(c->blob);
  free(c);
}

uint32_t sz_chain_num_coders(const sz_chain *c) {
  return c ? c->num_coders : 0;
}

uint32_t sz_chain_num_pack_streams(const sz_chain *c) {
  return c ? c->num_pack_streams : 0;
}

/* True when the folder is exactly one LZMA2 coder: the shape the SDK's
   multithreaded decoder (Lzma2DecMt) covers, and the shape 7-Zip writes by
   default (-m0=lzma2 -ms=on).  Returns the coder's props byte (the dictionary
   size selector) and the packed input size for it. */
int sz_chain_lzma2_root(const sz_chain *c, uint8_t *prop, uint64_t *in_size) {
  if(!c || c->num_coders != 1 || c->num_bonds != 0 ||
     c->num_pack_streams != 1) {
    return 0;
  }
  if(c->coder[0].method != SZ_M_LZMA2 || c->unpack_coder != 0 ||
     c->coder[0].props_size != 1) {
    return 0;
  }
  *prop = c->blob[c->coder[0].props_off];
  *in_size = c->pack_positions[1] - c->pack_positions[0];
  return 1;
}

int sz_chain_needs_password(const sz_chain *c) {
  uint32_t i;
  if(!c) return 0;
  for(i = 0; i < c->num_coders; i++)
    if(c->coder[i].method == SZ_M_AES) return 1;
  return 0;
}

int64_t sz_chain_coder_method(const sz_chain *c, uint32_t index) {
  if(!c || index >= c->num_coders) return -1;
  return (int64_t)c->coder[index].method;
}

void sz_chain_describe(const sz_chain *c, char *buf, size_t size) {
  size_t used = 0;
  uint32_t i;

  if(!buf || size == 0) return;
  buf[0] = 0;
  if(!c) return;

  for(i = 0; i < c->num_coders; i++) {
    char scratch[32];
    const char *name = method_label(c->coder[i].method, scratch,
                                    sizeof(scratch));
    int n = snprintf(buf + used, size - used, "%s%s", i ? " + " : "", name);
    if(n < 0 || (size_t)n >= size - used) return;
    used += (size_t)n;
  }
  snprintf(buf + used, size - used, " (%u coder%s, %u packed stream%s)",
           (unsigned)c->num_coders, c->num_coders == 1 ? "" : "s",
           (unsigned)c->num_pack_streams,
           c->num_pack_streams == 1 ? "" : "s");
}

int sz_chain_check(const sz_chain *c, sz_chain_err_t *err) {
  uint32_t i, j;

  if(!c) {
    err_set(err, SZ_CHAIN_ERR_PARAM, -1, 0, 0, "no folder");
    return -1;
  }

  for(i = 0; i < c->num_coders; i++) {
    const sz_coder *cd = &c->coder[i];
    int kind = method_kind(cd->method);

    if(kind < 0) {
      char scratch[32];
      err_set(err, SZ_CHAIN_ERR_METHOD, (int32_t)i, cd->method, 0,
              "coder %u uses %s, which this build cannot decode", (unsigned)i,
              method_label(cd->method, scratch, sizeof(scratch)));
      return -1;
    }
    if(kind == SZ_N_BCJ2) {
      if(cd->num_in_streams != 4) {
        err_set(err, SZ_CHAIN_ERR_LAYOUT, (int32_t)i, cd->method, 0,
                "BCJ2 coder %u has %u input streams, expected 4", (unsigned)i,
                (unsigned)cd->num_in_streams);
        return -1;
      }
    } else if(kind == SZ_N_AES) {
      uint32_t cycles, salt_size, iv_size;
      if(cd->num_in_streams != 1) {
        err_set(err, SZ_CHAIN_ERR_LAYOUT, (int32_t)i, cd->method, 0,
                "7zAES coder %u has %u input streams, expected 1", (unsigned)i,
                (unsigned)cd->num_in_streams);
        return -1;
      }
      if(aes_props_layout(c->blob + cd->props_off, cd->props_size, &cycles,
                          &salt_size, &iv_size) != 0) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, cd->method, 0,
                "7zAES coder %u carries %u property bytes, which do not match "
                "the salt and IV lengths they declare",
                (unsigned)i, (unsigned)cd->props_size);
        return -1;
      }
    } else if(cd->num_in_streams != 1) {
      err_set(err, SZ_CHAIN_ERR_LAYOUT, (int32_t)i, cd->method, 0,
              "%s coder %u has %u input streams, expected 1",
              sz_chain_method_name(cd->method), (unsigned)i,
              (unsigned)cd->num_in_streams);
      return -1;
    }
    for(j = 0; j < cd->num_in_streams; j++) {
      uint32_t s = cd->first_in_stream + j;
      uint32_t k;
      int is_pack = 0;
      if(c->source[s].from_coder >= 0) continue;
      for(k = 0; k < c->num_pack_streams; k++)
        if(c->pack_stream_idx[k] == s) is_pack = 1;
      if(!is_pack) {
        err_set(err, SZ_CHAIN_ERR_HEADER, (int32_t)i, cd->method, 0,
                "coder %u input stream %u has no source", (unsigned)i,
                (unsigned)s);
        return -1;
      }
    }
  }

  if(c->unpack_coder >= c->num_coders) {
    err_set(err, SZ_CHAIN_ERR_HEADER, -1, 0, 0, "invalid output coder");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------- decoding */

typedef struct sz_node sz_node;

typedef struct {
  IByteIn vt;
  sz_node *node;
} sz_ppmd_in;

struct sz_node {
  int kind;
  uint32_t method;
  const uint8_t *props;
  uint32_t props_size;
  const sz_chain_limits_t *lim;
  uint64_t out_size;
  uint64_t produced;
  sz_node *in;

  sz_chain_read_fn read_at;
  void *read_ctx;
  uint64_t pack_pos;
  uint64_t pack_left;

  uint8_t *inbuf;
  size_t in_cap;
  size_t in_len;
  size_t in_off;

  int in_eof;
  int finished;
  int failed;
  sz_chain_err_t *err;

  union {
    struct {
      CLzmaDec st;
    } lzma;
    struct {
      CLzma2Dec st;
    } lzma2;
    struct {
      CPpmd7 *p;
      sz_ppmd_in in;
      uint8_t *buf;
      size_t len, off;
      int in_eof;
    } ppmd;
    struct {
      int kind;
      unsigned delta;
      uint32_t pc;
      size_t fpos;
      uint32_t xstate;
      uint8_t dstate[DELTA_STATE_SIZE];
    } filt;
    struct {
      CBcj2Dec st;
      sz_node *side[3];
      uint8_t *side_buf[3];
      size_t side_len[3];
      uint8_t *mbuf;
      size_t mlen, moff;
      int prepared;
    } b2;
    struct {
      /* iv | keyMode | AES-256 round keys, then the ciphertext block being
         decrypted and the plaintext block it just produced.  AES_NUM_IVMRK_WORDS
         is a multiple of four words, so block/pbuf inherit the 16-byte
         alignment the AES entry points want. */
      UInt32 ivAes[AES_NUM_IVMRK_WORDS];
      uint8_t block[AES_BLOCK_SIZE];
      uint8_t pbuf[AES_BLOCK_SIZE];
      size_t plen, poff;
    } SZ_AES_ALIGN aes;
  } u;
};

typedef struct {
  sz_chain *chain;
  sz_node *by_coder[SZ_CHAIN_MAX_CODERS];
  sz_node *by_pack[SZ_CHAIN_MAX_STREAMS];
  uint8_t visiting[SZ_CHAIN_MAX_CODERS];
  sz_chain_read_fn read_at;
  void *read_ctx;
  sz_chain_err_t *err;
  const char *password;
  int has_aes; /* a 7zAES coder was built: a failure may be a wrong password */
  int failed;
  sz_chain_status_t fail_status;
  int32_t fail_coder;
} sz_build;

static int node_fail(sz_node *n, sz_chain_status_t status, const char *message) {
  if(n->failed) return -1;
  n->failed = 1;
  if(n->err)
    err_set(n->err, status, -1, n->method, n->produced, "%s", message);
  return -1;
}

static void node_destroy(sz_node *n) {
  if(!n) return;
  switch(n->kind) {
  case SZ_N_LZMA:
    LzmaDec_Free(&n->u.lzma.st, &g_alloc);
    break;
  case SZ_N_LZMA2:
    Lzma2Dec_Free(&n->u.lzma2.st, &g_alloc);
    break;
  case SZ_N_PPMD:
    if(n->u.ppmd.p) {
      Ppmd7_Free(n->u.ppmd.p, &g_alloc);
      free(n->u.ppmd.p);
    }
    free(n->u.ppmd.buf);
    break;
  case SZ_N_BCJ2: {
    int i;
    for(i = 0; i < 3; i++) free(n->u.b2.side_buf[i]);
    free(n->u.b2.mbuf);
    break;
  }
  default: break;
  }
  free(n->inbuf);
  free(n);
}

static int node_pull(sz_node *n, uint8_t *dst, size_t want, size_t *got);

/* Refills n->inbuf from the single upstream node.
   Returns 0 = data, 1 = upstream exhausted, -1 = error. */
static int node_refill(sz_node *n) {
  size_t got = 0;
  int rc;

  n->in_off = 0;
  n->in_len = 0;
  if(n->in_eof) return 1;
  rc = node_pull(n->in, n->inbuf, n->in_cap, &got);
  if(rc != 0) return -1;
  if(got == 0) {
    n->in_eof = 1;
    return 1;
  }
  n->in_len = got;
  return 0;
}

/* Applies the filter to data[0..size), in place, and returns the number of
   bytes converted.  The unconverted tail is left for the next call. */
static size_t filter_apply(sz_node *n, uint8_t *data, size_t size) {
  size_t consumed = size;

  switch(n->u.filt.kind) {
  case SZ_F_DELTA:
    Delta_Decode(n->u.filt.dstate, n->u.filt.delta, data, (SizeT)size);
    break;
  case SZ_F_X86: {
    Byte *tail = Z7_BRANCH_CONV_ST_DEC(X86)(data, (SizeT)size, n->u.filt.pc,
                                            &n->u.filt.xstate);
    consumed = (size_t)(tail - data);
    break;
  }
  case SZ_F_PPC:
    consumed = (size_t)(Z7_BRANCH_CONV_DEC(PPC)(data, (SizeT)size,
                                               n->u.filt.pc) -
                        data);
    break;
  case SZ_F_IA64:
    consumed = (size_t)(Z7_BRANCH_CONV_DEC(IA64)(data, (SizeT)size,
                                                n->u.filt.pc) -
                        data);
    break;
  case SZ_F_SPARC:
    consumed = (size_t)(Z7_BRANCH_CONV_DEC(SPARC)(data, (SizeT)size,
                                                 n->u.filt.pc) -
                        data);
    break;
  case SZ_F_ARM:
    consumed = (size_t)(Z7_BRANCH_CONV_DEC(ARM)(data, (SizeT)size,
                                               n->u.filt.pc) -
                        data);
    break;
  case SZ_F_ARMT:
    consumed = (size_t)(Z7_BRANCH_CONV_DEC(ARMT)(data, (SizeT)size,
                                                n->u.filt.pc) -
                        data);
    break;
  default: break;
  }
  return consumed;
}

static Byte ppmd_read(IByteInPtr pp) {
  Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(sz_ppmd_in)
  sz_node *n = p->node;

  if(n->u.ppmd.off == n->u.ppmd.len) {
    size_t got = 0;
    if(n->u.ppmd.in_eof) return 0;
    if(node_pull(n->in, n->u.ppmd.buf, SZ_IN_CHUNK, &got) != 0) {
      n->failed = 1;
      return 0;
    }
    if(got == 0) {
      n->u.ppmd.in_eof = 1;
      return 0;
    }
    n->u.ppmd.len = got;
    n->u.ppmd.off = 0;
  }
  return n->u.ppmd.buf[n->u.ppmd.off++];
}

/* Reads the three BCJ2 side streams in full.  Returns 0 or -1. */
static int bcj2_prepare(sz_node *n) {
  uint64_t budget = n->lim ? n->lim->max_side_bytes : 0;
  uint64_t used = 0;
  int i;

  for(i = 0; i < 3; i++) {
    sz_node *s = n->u.b2.side[i];
    uint64_t size = s->out_size;
    size_t got = 0;

    if(i < 2 && (size & 3) != 0)
      return node_fail(n, SZ_CHAIN_ERR_DATA,
                       "BCJ2 address stream is not a multiple of 4 bytes");
    if(used > budget || size > budget - used) {
      if(n->err)
        err_set(n->err, SZ_CHAIN_ERR_LIMIT, -1, n->method, 0,
                "BCJ2 needs %llu MB for its side streams, the limit is %llu MB",
                (unsigned long long)((used + size) >> 20),
                (unsigned long long)(budget >> 20));
      n->failed = 1;
      return -1;
    }
    used += size;
    n->u.b2.side_len[i] = (size_t)size;
    n->u.b2.side_buf[i] = (uint8_t *)malloc(size ? (size_t)size : 1);
    if(!n->u.b2.side_buf[i])
      return node_fail(n, SZ_CHAIN_ERR_MEM,
                       "cannot allocate the BCJ2 side streams");
    while(got < (size_t)size) {
      size_t g = 0;
      if(node_pull(s, n->u.b2.side_buf[i] + got, (size_t)size - got, &g) != 0)
        return -1;
      if(g == 0)
        return node_fail(n, SZ_CHAIN_ERR_DATA,
                         "BCJ2 side stream is truncated");
      got += g;
    }
  }

  n->u.b2.mbuf = (uint8_t *)malloc(SZ_BCJ2_MAIN_CHUNK);
  if(!n->u.b2.mbuf)
    return node_fail(n, SZ_CHAIN_ERR_MEM, "cannot allocate the BCJ2 buffer");

  {
    CBcj2Dec *p = &n->u.b2.st;
    Bcj2Dec_Init(p);
    p->bufs[BCJ2_STREAM_CALL] = n->u.b2.side_buf[0];
    p->lims[BCJ2_STREAM_CALL] = n->u.b2.side_buf[0] + n->u.b2.side_len[0];
    p->bufs[BCJ2_STREAM_JUMP] = n->u.b2.side_buf[1];
    p->lims[BCJ2_STREAM_JUMP] = n->u.b2.side_buf[1] + n->u.b2.side_len[1];
    p->bufs[BCJ2_STREAM_RC] = n->u.b2.side_buf[2];
    p->lims[BCJ2_STREAM_RC] = n->u.b2.side_buf[2] + n->u.b2.side_len[2];
  }
  n->u.b2.prepared = 1;
  return 0;
}

/* Refills BCJ2's MAIN staging buffer, keeping the unconsumed tail. */
static int bcj2_refill_main(sz_node *n) {
  size_t tail = n->u.b2.mlen - n->u.b2.moff;
  size_t got = 0;

  if(tail && n->u.b2.moff)
    memmove(n->u.b2.mbuf, n->u.b2.mbuf + n->u.b2.moff, tail);
  n->u.b2.moff = 0;
  n->u.b2.mlen = tail;
  if(n->in_eof) return 0;
  if(node_pull(n->in, n->u.b2.mbuf + tail, SZ_BCJ2_MAIN_CHUNK - tail,
               &got) != 0)
    return -1;
  if(got == 0) n->in_eof = 1;
  else n->u.b2.mlen = tail + got;
  return 0;
}

static int node_pull(sz_node *n, uint8_t *dst, size_t want, size_t *got) {
  size_t total = 0;
  uint64_t remain;

  *got = 0;
  if(n->failed) return -1;
  if(want == 0) return 0;
  remain = n->out_size - n->produced;
  if((uint64_t)want > remain) want = (size_t)remain;
  if(want == 0) return 0;

  switch(n->kind) {
  case SZ_N_PACK:
    if((uint64_t)want > n->pack_left) want = (size_t)n->pack_left;
    if(want == 0) return 0;
    if(n->read_at(n->read_ctx, n->pack_pos, dst, want) != 0)
      return node_fail(n, SZ_CHAIN_ERR_READ, "cannot read packed data");
    n->pack_pos += want;
    n->pack_left -= want;
    total = want;
    break;

  case SZ_N_COPY:
    if(node_pull(n->in, dst, want, &total) != 0) return -1;
    break;

  case SZ_N_LZMA:
  case SZ_N_LZMA2: {
    int eof_seen = 0;
    while(total < want) {
      SizeT dest_len, src_len;
      ELzmaStatus status;
      SRes res;

      if(n->in_off == n->in_len) {
        int rc = node_refill(n);
        if(rc < 0) return -1;
        if(rc == 1) eof_seen = 1;
      }
      dest_len = (SizeT)(want - total);
      src_len = (SizeT)(n->in_len - n->in_off);
      if(n->kind == SZ_N_LZMA)
        res = LzmaDec_DecodeToBuf(&n->u.lzma.st, dst + total, &dest_len,
                                  n->inbuf + n->in_off, &src_len,
                                  LZMA_FINISH_ANY, &status);
      else
        res = Lzma2Dec_DecodeToBuf(&n->u.lzma2.st, dst + total, &dest_len,
                                   n->inbuf + n->in_off, &src_len,
                                   LZMA_FINISH_ANY, &status);
      if(res != SZ_OK)
        return node_fail(n, SZ_CHAIN_ERR_DATA,
                         n->kind == SZ_N_LZMA ? "LZMA stream is corrupt"
                                              : "LZMA2 stream is corrupt");

      n->in_off += (size_t)src_len;
      total += (size_t)dest_len;

      if(status == LZMA_STATUS_FINISHED_WITH_MARK) {
        n->finished = 1;
        break;
      }
      if(dest_len == 0 && src_len == 0) {
        if(eof_seen) break; /* truncated: the caller sees a short read */
        if(n->in_len >= n->in_cap)
          return node_fail(n, SZ_CHAIN_ERR_DATA,
                           "LZMA chunk does not fit the staging buffer");
      }
    }
    break;
  }

  case SZ_N_PPMD:
    if(!n->u.ppmd.p)
      return node_fail(n, SZ_CHAIN_ERR_HEADER, "PPMd coder has no model");
    while(total < want) {
      int sym = Ppmd7z_DecodeSymbol(n->u.ppmd.p);
      if(sym < 0)
        return node_fail(n, SZ_CHAIN_ERR_DATA,
                         sym == PPMD7_SYM_END ? "PPMd stream ended early"
                                              : "PPMd stream is corrupt");
      dst[total++] = (uint8_t)sym;
    }
    break;

  case SZ_N_FILTER:
    while(total < want) {
      size_t avail;

      if(n->in_off < n->u.filt.fpos) {
        avail = n->u.filt.fpos - n->in_off;
        if(avail > want - total) avail = want - total;
        memcpy(dst + total, n->inbuf + n->in_off, avail);
        n->in_off += avail;
        total += avail;
        continue;
      }
      if(n->finished) break;

      /* Drop the retired prefix; the unconverted carry stays at the front.
         Retiring N bytes is what advances the program counter by N, because
         inbuf[0] now sits N bytes further into the stream. */
      if(n->in_off > 0) {
        size_t rest = n->in_len - n->in_off;
        if(rest) memmove(n->inbuf, n->inbuf + n->in_off, rest);
        n->u.filt.pc += (uint32_t)n->in_off;
        n->in_len = rest;
        n->in_off = 0;
        n->u.filt.fpos = 0;
      }
      if(n->in_len < n->in_cap && !n->in_eof) {
        size_t g = 0;
        int rc = node_pull(n->in, n->inbuf + n->in_len,
                           n->in_cap - n->in_len, &g);
        if(rc != 0) return -1;
        if(g == 0) n->in_eof = 1;
        else n->in_len += g;
      }
      if(n->in_len == 0) {
        n->finished = 1;
        break;
      }
      n->u.filt.fpos = filter_apply(n, n->inbuf, n->in_len);
      if(n->u.filt.fpos == 0) {
        if(n->in_eof) {
          /* The tail is too short to contain a full instruction: the 7z
             encoder leaves it unconverted, so pass it through. */
          n->u.filt.fpos = n->in_len;
        } else if(n->in_len >= n->in_cap) {
          return node_fail(n, SZ_CHAIN_ERR_DATA,
                           "filter cannot make progress on its input");
        }
      }
    }
    break;

  case SZ_N_AES: {
    /* 7-Zip encrypts the packed stream with AES-256-CBC and rounds its length
       up to a whole number of blocks, so the ciphertext is always block
       aligned; the coder's declared output size is the true (unrounded)
       length, and the tail of the final block is simply never delivered. */
    while(total < want) {
      size_t avail;

      if(n->u.aes.poff < n->u.aes.plen) {
        avail = n->u.aes.plen - n->u.aes.poff;
        if(avail > want - total) avail = want - total;
        memcpy(dst + total, n->u.aes.pbuf + n->u.aes.poff, avail);
        n->u.aes.poff += avail;
        total += avail;
        continue;
      }
      if(n->in_off > 0) {
        size_t rest = n->in_len - n->in_off;
        if(rest) memmove(n->inbuf, n->inbuf + n->in_off, rest);
        n->in_len = rest;
        n->in_off = 0;
      }
      if(n->in_len < AES_BLOCK_SIZE) {
        size_t g = 0;
        if(n->in_eof)
          return node_fail(n, SZ_CHAIN_ERR_DATA,
                           "the encrypted 7zAES stream ends in a partial block");
        if(node_pull(n->in, n->inbuf + n->in_len, n->in_cap - n->in_len,
                     &g) != 0)
          return -1;
        if(g == 0) n->in_eof = 1;
        else n->in_len += g;
        continue;
      }
      memcpy(n->u.aes.block, n->inbuf, AES_BLOCK_SIZE);
      n->in_off = AES_BLOCK_SIZE;
      g_AesCbc_Decode(n->u.aes.ivAes, n->u.aes.block, 1);
      memcpy(n->u.aes.pbuf, n->u.aes.block, AES_BLOCK_SIZE);
      n->u.aes.plen = AES_BLOCK_SIZE;
      n->u.aes.poff = 0;
    }
    break;
  }

  case SZ_N_BCJ2: {
    CBcj2Dec *p = &n->u.b2.st;
    size_t want0 = want;

    if(!n->u.b2.prepared && bcj2_prepare(n) != 0) return -1;

    p->dest = dst;
    p->destLim = dst + want0;
    for(;;) {
      const Byte *before_main;
      int progressed;

      if(p->dest == p->destLim) break;
      if(n->u.b2.mlen - n->u.b2.moff <= SZ_BCJ2_KEEP && !n->in_eof) {
        if(bcj2_refill_main(n) != 0) return -1;
      }
      if(n->u.b2.moff == n->u.b2.mlen && n->in_eof) break;

      p->bufs[BCJ2_STREAM_MAIN] = n->u.b2.mbuf + n->u.b2.moff;
      p->lims[BCJ2_STREAM_MAIN] = n->u.b2.mbuf + n->u.b2.mlen;
      before_main = p->bufs[BCJ2_STREAM_MAIN];
      progressed = 0;

      if(Bcj2Dec_Decode(p) != SZ_OK)
        return node_fail(n, SZ_CHAIN_ERR_DATA, "BCJ2 stream is corrupt");

      n->u.b2.moff = (size_t)(p->bufs[BCJ2_STREAM_MAIN] - n->u.b2.mbuf);
      if(p->bufs[BCJ2_STREAM_MAIN] != before_main || p->dest != dst)
        progressed = 1;
      if(!progressed) break;
    }
    total = (size_t)(p->dest - dst);
    break;
  }

  default:
    return node_fail(n, SZ_CHAIN_ERR_LAYOUT, "unsupported coder shape");
  }

  n->produced += total;
  *got = total;
  return 0;
}

/* ------------------------------------------------------------- graph */

static sz_node *build_stream(sz_build *b, uint32_t stream);
static sz_node *build_coder(sz_build *b, uint32_t index);

static void build_fail(sz_build *b, sz_chain_status_t status,
                       int32_t coder, uint32_t method, const char *fmt,
                       ...) {
  va_list ap;
  b->failed = 1;
  b->fail_status = status;
  b->fail_coder = coder;
  if(!b->err || b->err->message[0]) return;
  b->err->status = status;
  b->err->coder = coder;
  b->err->method = method;
  b->err->offset = 0;
  if(fmt) {
    va_start(ap, fmt);
    vsnprintf(b->err->message, sizeof(b->err->message), fmt, ap);
    va_end(ap);
  } else {
    b->err->message[0] = 0;
  }
}

static sz_node *node_new(sz_build *b, int kind, uint32_t method,
                         const sz_coder *cd) {
  sz_node *n = (sz_node *)calloc(1, sizeof(*n));
  if(!n) {
    build_fail(b, SZ_CHAIN_ERR_MEM, -1, method, "out of memory");
    return NULL;
  }
  n->kind = kind;
  n->method = method;
  n->lim = &b->chain->limits;
  n->err = b->err;
  if(cd) {
    n->props = b->chain->blob + cd->props_off;
    n->props_size = cd->props_size;
  }
  return n;
}

static int alloc_inbuf(sz_node *n, size_t cap) {
  n->inbuf = (uint8_t *)malloc(cap);
  if(!n->inbuf) return -1;
  n->in_cap = cap;
  return 0;
}

static sz_node *build_pack(sz_build *b, uint32_t pack_index) {
  sz_node *n;
  uint64_t start, end;

  if(b->by_pack[pack_index]) return b->by_pack[pack_index];
  n = node_new(b, SZ_N_PACK, 0, NULL);
  if(!n) return NULL;
  start = b->chain->pack_positions[pack_index];
  end = b->chain->pack_positions[pack_index + 1];
  if(end < start) {
    node_destroy(n);
    build_fail(b, SZ_CHAIN_ERR_HEADER, -1, 0, "packed stream offsets go back");
    return NULL;
  }
  n->pack_pos = start;
  n->pack_left = end - start;
  n->out_size = end - start;
  n->read_at = b->read_at;
  n->read_ctx = b->read_ctx;
  b->by_pack[pack_index] = n;
  return n;
}

static sz_node *build_stream(sz_build *b, uint32_t stream) {
  const sz_source *src;
  if(stream >= b->chain->total_streams) return NULL;
  src = &b->chain->source[stream];
  if(src->from_coder >= 0) return build_coder(b, (uint32_t)src->from_coder);
  return build_pack(b, src->pack_index);
}

static int lzma_dict_size(const uint8_t *props, uint32_t size, uint64_t *out) {
  uint64_t d;
  if(size != 5) return -1;
  d = (uint64_t)props[1] | ((uint64_t)props[2] << 8) |
      ((uint64_t)props[3] << 16) | ((uint64_t)props[4] << 24);
  *out = d;
  return 0;
}

static int lzma2_dict_size(const uint8_t *props, uint32_t size, uint64_t *out) {
  unsigned p;
  if(size != 1) return -1;
  p = props[0];
  if(p > 40) return -1;
  *out = (p == 40) ? 0xFFFFFFFFull : ((uint64_t)(2 | (p & 1)) << (p / 2 + 11));
  return 0;
}

static sz_node *build_coder(sz_build *b, uint32_t index) {
  sz_chain *c = b->chain;
  const sz_coder *cd = &c->coder[index];
  sz_node *n;
  int kind;

  if(b->by_coder[index]) return b->by_coder[index];
  if(b->visiting[index]) {
    build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
               "cycle in the coder graph");
    return NULL;
  }
  b->visiting[index] = 1;

  kind = method_kind(cd->method);
  if(kind < 0) {
    char scratch[32];
    build_fail(b, SZ_CHAIN_ERR_METHOD, (int32_t)index, cd->method,
               "coder %u uses %s, which this build cannot decode",
               (unsigned)index,
               method_label(cd->method, scratch, sizeof(scratch)));
    b->visiting[index] = 0;
    return NULL;
  }

  n = node_new(b, kind, cd->method, cd);
  if(!n) {
    b->visiting[index] = 0;
    return NULL;
  }

  /* Each coder's output size comes from the archive's UNPACK_INFO, one entry
     per coder output stream; that is what lets the chain stream with exact
     boundaries.  It is *not* the folder's unpack size: in a BCJ2 folder the
     four LZMA2 coders feeding MAIN/CALL/JUMP/RC all have sizes of their own
     (MAIN can even be larger than the folder's final size, e.g. 300066 vs
     300000), so using the folder size would silently truncate them. */
  n->out_size = c->coder_unpack_sizes[index];

  switch(kind) {
  case SZ_N_COPY:
  case SZ_N_LZMA:
  case SZ_N_LZMA2:
  case SZ_N_PPMD:
  case SZ_N_FILTER:
  case SZ_N_AES:
    n->in = build_stream(b, cd->first_in_stream);
    if(!n->in) {
      node_destroy(n);
      b->visiting[index] = 0;
      return NULL;
    }
    break;
  case SZ_N_BCJ2: {
    int i;
    n->in = build_stream(b, cd->first_in_stream);
    if(!n->in) {
      node_destroy(n);
      b->visiting[index] = 0;
      return NULL;
    }
    for(i = 0; i < 3; i++) {
      n->u.b2.side[i] = build_stream(b, cd->first_in_stream + 1 + (uint32_t)i);
      if(!n->u.b2.side[i]) {
        node_destroy(n);
        b->visiting[index] = 0;
        return NULL;
      }
    }
    break;
  }
  default:
    node_destroy(n);
    b->visiting[index] = 0;
    return NULL;
  }

  /* Method specific setup. */
  switch(kind) {
  case SZ_N_COPY:
    break;
  case SZ_N_LZMA: {
    uint64_t dict = 0;
    if(lzma_dict_size(n->props, n->props_size, &dict) != 0) {
      build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                 "LZMA coder %u has invalid properties", (unsigned)index);
      goto fail;
    }
    if(dict > c->limits.max_dict_bytes) {
      build_fail(b, SZ_CHAIN_ERR_LIMIT, (int32_t)index, cd->method,
                 "LZMA dictionary is %llu MB, the limit is %llu MB",
                 (unsigned long long)(dict >> 20),
                 (unsigned long long)(c->limits.max_dict_bytes >> 20));
      goto fail;
    }
    LzmaDec_CONSTRUCT(&n->u.lzma.st);
    if(LzmaDec_Allocate(&n->u.lzma.st, n->props, n->props_size, &g_alloc) !=
       SZ_OK) {
      build_fail(b, SZ_CHAIN_ERR_MEM, (int32_t)index, cd->method,
                 "cannot allocate the LZMA dictionary");
      goto fail;
    }
    LzmaDec_Init(&n->u.lzma.st);
    if(alloc_inbuf(n, SZ_IN_CHUNK) != 0) goto oom;
    break;
  }
  case SZ_N_LZMA2: {
    uint64_t dict = 0;
    if(lzma2_dict_size(n->props, n->props_size, &dict) != 0) {
      build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                 "LZMA2 coder %u has invalid properties", (unsigned)index);
      goto fail;
    }
    if(dict > c->limits.max_dict_bytes) {
      build_fail(b, SZ_CHAIN_ERR_LIMIT, (int32_t)index, cd->method,
                 "LZMA2 dictionary is %llu MB, the limit is %llu MB",
                 (unsigned long long)(dict >> 20),
                 (unsigned long long)(c->limits.max_dict_bytes >> 20));
      goto fail;
    }
    Lzma2Dec_CONSTRUCT(&n->u.lzma2.st);
    if(Lzma2Dec_Allocate(&n->u.lzma2.st, n->props[0], &g_alloc) != SZ_OK) {
      build_fail(b, SZ_CHAIN_ERR_MEM, (int32_t)index, cd->method,
                 "cannot allocate the LZMA2 dictionary");
      goto fail;
    }
    Lzma2Dec_Init(&n->u.lzma2.st);
    if(alloc_inbuf(n, SZ_IN_CHUNK) != 0) goto oom;
    break;
  }
  case SZ_N_PPMD: {
    unsigned order;
    uint32_t mem;
    CPpmd7 *p;

    if(n->props_size != 5) {
      build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                 "PPMd coder %u must carry 5 property bytes", (unsigned)index);
      goto fail;
    }
    order = n->props[0];
    mem = (uint32_t)n->props[1] | ((uint32_t)n->props[2] << 8) |
          ((uint32_t)n->props[3] << 16) | ((uint32_t)n->props[4] << 24);
    if(order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER ||
       mem < PPMD7_MIN_MEM_SIZE) {
      build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                 "PPMd coder %u has invalid properties", (unsigned)index);
      goto fail;
    }
    if((uint64_t)mem > c->limits.max_ppmd_bytes) {
      build_fail(b, SZ_CHAIN_ERR_LIMIT, (int32_t)index, cd->method,
                 "PPMd model is %llu MB, the limit is %llu MB",
                 (unsigned long long)((uint64_t)mem >> 20),
                 (unsigned long long)(c->limits.max_ppmd_bytes >> 20));
      goto fail;
    }
    p = (CPpmd7 *)malloc(sizeof(*p));
    if(!p) goto oom;
    n->u.ppmd.p = p;
    Ppmd7_Construct(p);
    if(!Ppmd7_Alloc(p, mem, &g_alloc)) {
      build_fail(b, SZ_CHAIN_ERR_MEM, (int32_t)index, cd->method,
                 "cannot allocate the PPMd model");
      goto fail;
    }
    Ppmd7_Init(p, order);
    n->u.ppmd.buf = (uint8_t *)malloc(SZ_IN_CHUNK);
    if(!n->u.ppmd.buf) goto oom;
    n->u.ppmd.in.node = n;
    n->u.ppmd.in.vt.Read = ppmd_read;
    p->rc.dec.Stream = &n->u.ppmd.in.vt;
    if(!Ppmd7z_RangeDec_Init(&p->rc.dec)) {
      build_fail(b, SZ_CHAIN_ERR_DATA, (int32_t)index, cd->method,
                 "PPMd range decoder could not start (truncated stream?)");
      goto fail;
    }
    break;
  }
  case SZ_N_FILTER: {
    int fk = filter_kind(cd->method);
    n->u.filt.kind = fk;
    if(fk == SZ_F_DELTA) {
      if(n->props_size != 1) {
        build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                   "Delta coder %u must carry 1 property byte",
                   (unsigned)index);
        goto fail;
      }
      n->u.filt.delta = (unsigned)n->props[0] + 1;
      Delta_Init(n->u.filt.dstate);
    } else if(n->props_size != 0) {
      build_fail(b, SZ_CHAIN_ERR_LAYOUT, (int32_t)index, cd->method,
                 "%s coder %u must not carry properties",
                 sz_chain_method_name(cd->method), (unsigned)index);
      goto fail;
    }
    n->u.filt.xstate = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    if(alloc_inbuf(n, SZ_FILT_CHUNK + SZ_FILT_LOOKAHEAD) != 0) goto oom;
    break;
  }
  case SZ_N_AES: {
    uint32_t cycles = 0, salt_size = 0, iv_size = 0;
    const uint8_t *salt, *iv_bytes;
    const char *pwd = b->password;
    size_t pwd_len, pwd16_size;
    uint8_t *pwd16;
    uint8_t key[SZ_AES_KEY_SIZE];
    uint8_t iv[AES_BLOCK_SIZE];

    b->has_aes = 1;
    if(aes_props_layout(n->props, n->props_size, &cycles, &salt_size,
                        &iv_size) != 0) {
      build_fail(b, SZ_CHAIN_ERR_HEADER, (int32_t)index, cd->method,
                 "7zAES coder %u carries %u malformed property bytes",
                 (unsigned)index, (unsigned)n->props_size);
      goto fail;
    }
    if(cycles != SZ_AES_CYCLES_NO_KDF && cycles > c->limits.max_aes_cycles) {
      build_fail(b, SZ_CHAIN_ERR_LIMIT, (int32_t)index, cd->method,
                 "7zAES key derivation asks for %llu SHA-256 passes, the limit "
                 "is %llu",
                 (unsigned long long)((uint64_t)1 << cycles),
                 (unsigned long long)((uint64_t)1 << c->limits.max_aes_cycles));
      goto fail;
    }
    if(!pwd || !*pwd) {
      build_fail(b, SZ_CHAIN_ERR_PASSWORD, (int32_t)index, cd->method,
                 "the archive is encrypted with 7zAES; a password is required");
      goto fail;
    }
    pwd_len = strlen(pwd);
    if(pwd_len > SZ_AES_MAX_PASSWORD) {
      build_fail(b, SZ_CHAIN_ERR_PASSWORD, (int32_t)index, cd->method,
                 "the password is %llu bytes long, at most %u are supported",
                 (unsigned long long)pwd_len, (unsigned)SZ_AES_MAX_PASSWORD);
      goto fail;
    }
    /* Every UTF-8 byte yields at most one UTF-16 code unit, so twice the byte
       count always fits the converted password. */
    pwd16 = (uint8_t *)malloc(2 * (pwd_len + 1));
    if(!pwd16) goto oom;
    pwd16_size = utf8_to_utf16le(pwd, pwd16, 2 * (pwd_len + 1));
    if(pwd16_size == 0) {
      free(pwd16);
      build_fail(b, SZ_CHAIN_ERR_PASSWORD, (int32_t)index, cd->method,
                 "the password is not valid UTF-8");
      goto fail;
    }

    salt = n->props + 2;
    iv_bytes = salt + salt_size;
    aes_derive_key(salt, salt_size, pwd16, pwd16_size, cycles, key);
    free(pwd16);

    /* 7-Zip zeroes the full 16 byte IV before copying ivSize bytes into it, so
       a short IV is zero padded rather than repeated. */
    memset(iv, 0, sizeof(iv));
    if(iv_size) memcpy(iv, iv_bytes, iv_size);
    Aes_SetKey_Dec(n->u.aes.ivAes + 4, key, SZ_AES_KEY_SIZE);
    AesCbc_Init(n->u.aes.ivAes, iv);
    memset(key, 0, sizeof(key));
    if(alloc_inbuf(n, SZ_IN_CHUNK) != 0) goto oom;
    break;
  }
  case SZ_N_BCJ2:
    if(n->props_size != 0) {
      build_fail(b, SZ_CHAIN_ERR_LAYOUT, (int32_t)index, cd->method,
                 "BCJ2 coder %u must not carry properties", (unsigned)index);
      goto fail;
    }
    break;
  default: break;
  }

  b->visiting[index] = 0;
  b->by_coder[index] = n;
  return n;

oom:
  build_fail(b, SZ_CHAIN_ERR_MEM, (int32_t)index, cd->method, "out of memory");
fail:
  node_destroy(n);
  b->visiting[index] = 0;
  return NULL;
}

/* ------------------------------------------------------------- decode */

static void free_nodes(sz_build *b) {
  uint32_t i;
  for(i = 0; i < SZ_CHAIN_MAX_CODERS; i++)
    if(b->by_coder[i]) node_destroy(b->by_coder[i]);
  for(i = 0; i < SZ_CHAIN_MAX_STREAMS; i++)
    if(b->by_pack[i]) node_destroy(b->by_pack[i]);
}

/* Final structural checks that only make sense once the whole output has been
   produced: a BCJ2 folder must have consumed every one of its streams. */
static int finish_check(sz_node *n, sz_chain_err_t *err) {
  if(n->kind != SZ_N_BCJ2) return 0;
  {
    CBcj2Dec *p = &n->u.b2.st;
    int i;
    for(i = 0; i < 3; i++) {
      if(p->bufs[i] != p->lims[i]) {
        err_set(err, SZ_CHAIN_ERR_DATA, -1, n->method, n->produced,
                "BCJ2 %s stream was not fully consumed",
                i == 0 ? "CALL" : (i == 1 ? "JUMP" : "RC"));
        return -1;
      }
    }
    if(n->u.b2.moff != n->u.b2.mlen) {
      err_set(err, SZ_CHAIN_ERR_DATA, -1, n->method, n->produced,
              "BCJ2 MAIN stream was not fully consumed");
      return -1;
    }
    if(!Bcj2Dec_IsMaybeFinished(p)) {
      err_set(err, SZ_CHAIN_ERR_DATA, -1, n->method, n->produced,
              "BCJ2 stream ended in the middle of an instruction");
      return -1;
    }
  }
  return 0;
}

/* A password that decrypts to rubbish is indistinguishable from corrupt data,
   so when a folder carried a 7zAES coder and the decode still failed, say so
   in case the caller would rather retry with another password. */
static void err_hint_password(sz_chain_err_t *err, const sz_build *b) {
  size_t used;

  if(!err || !b->has_aes || !b->password || !b->password[0]) return;
  if(err->status != SZ_CHAIN_ERR_DATA) return;
  used = strlen(err->message);
  if(used + 32 >= sizeof(err->message)) return;
  snprintf(err->message + used, sizeof(err->message) - used,
           " (a wrong password looks like this)");
}

int sz_chain_decode(sz_chain *c, sz_chain_read_fn read_at, void *read_ctx,
                    sz_chain_sink_fn sink, void *sink_ctx,
                    sz_chain_cancel_fn cancel, void *cancel_ctx,
                    const char *password, uint32_t *crc_out,
                    sz_chain_err_t *err) {
  sz_build b;
  sz_node *root;
  uint8_t *out;
  uint64_t produced = 0;
  uint32_t crc = CRC_INIT_VAL;
  static int crc_table_ready;
  static int aes_table_ready;

  if(err) memset(err, 0, sizeof(*err));
  if(!c || !read_at || !sink) {
    err_set(err, SZ_CHAIN_ERR_PARAM, -1, 0, 0, "missing folder or callbacks");
    return -1;
  }
  if(sz_chain_check(c, err) != 0) return -1;
  if(!crc_table_ready) {
    CrcGenerateTable();
    crc_table_ready = 1;
  }
  if(!aes_table_ready) {
    AesGenTables();
    Sha256Prepare();
    aes_table_ready = 1;
  }

  memset(&b, 0, sizeof(b));
  b.chain = c;
  b.err = err;
  b.password = password;
  b.read_at = read_at;
  b.read_ctx = read_ctx;
  root = build_coder(&b, c->unpack_coder);
  if(!root) {
    free_nodes(&b);
    if(err && !err->message[0])
      err_set(err, SZ_CHAIN_ERR_INTERNAL, -1, 0, 0,
              "the coder chain could not be built");
    return -1;
  }

  out = (uint8_t *)malloc(SZ_OUT_CHUNK);
  if(!out) {
    free_nodes(&b);
    err_set(err, SZ_CHAIN_ERR_MEM, -1, 0, 0,
            "cannot allocate the output buffer");
    return -1;
  }

  while(produced < c->unpack_size) {
    uint64_t remain = c->unpack_size - produced;
    size_t want = (remain > SZ_OUT_CHUNK) ? (size_t)SZ_OUT_CHUNK
                                          : (size_t)remain;
    size_t got = 0;

    if(cancel && cancel(cancel_ctx)) {
      err_set(err, SZ_CHAIN_ERR_CANCELED, -1, 0, produced, "canceled");
      free(out);
      free_nodes(&b);
      return -1;
    }
    if(node_pull(root, out, want, &got) != 0) {
      err_hint_password(err, &b);
      free(out);
      free_nodes(&b);
      return -1;
    }
    if(got != want) {
      /* The output coder stopped early.  That is a truncated folder, not
         necessarily truncated packed data: an inner coder can also run out
         when its own declared size is larger than the stream it carries. */
      err_set(err, SZ_CHAIN_ERR_DATA, (int32_t)c->unpack_coder, root->method,
              produced,
              "the %s output stream ended %llu bytes early, after %llu of %llu "
              "bytes",
              sz_chain_method_name(root->method),
              (unsigned long long)(want - got),
              (unsigned long long)(produced + got),
              (unsigned long long)c->unpack_size);
      err_hint_password(err, &b);
      free(out);
      free_nodes(&b);
      return -1;
    }
    crc = CrcUpdate(crc, out, got);
    if(sink(sink_ctx, out, got) != 0) {
      err_set(err, SZ_CHAIN_ERR_WRITE, -1, root->method, produced,
              "the output sink rejected data");
      free(out);
      free_nodes(&b);
      return -1;
    }
    produced += got;
  }

  if(finish_check(root, err) != 0) {
    err_hint_password(err, &b);
    free(out);
    free_nodes(&b);
    return -1;
  }

  if(crc_out) *crc_out = CRC_GET_DIGEST(crc);
  free(out);
  free_nodes(&b);
  return 0;
}
