/* sevenz_header -- see sevenz_header.h for what this does and why. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zCrc.h"
#include "7zTypes.h"

#include "sevenz_chain.h"
#include "sevenz_header.h"

/* The header property ids we have to recognise.  They are the same enum the
   SDK's header scanner uses (7zArcIn.c), spelled out here so this file does
   not depend on that translation unit's internals. */
#define SZH_ID_END 0x00
#define SZH_ID_HEADER 0x01
#define SZH_ID_PACK_INFO 0x06
#define SZH_ID_UNPACK_INFO 0x07
#define SZH_ID_SIZE 0x09
#define SZH_ID_CRC 0x0A
#define SZH_ID_FOLDER 0x0B
#define SZH_ID_CODERS_UNPACK_SIZE 0x0C
#define SZH_ID_ENCODED_HEADER 0x17

#define SZH_MAX_CODERS SZ_CHAIN_MAX_CODERS
#define SZH_MAX_STREAMS SZ_CHAIN_MAX_STREAMS

/* --------------------------------------------------------------- numbers */

static uint32_t
szh_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t
szh_le64(const uint8_t *p) {
  return (uint64_t)szh_le32(p) | ((uint64_t)szh_le32(p + 4) << 32);
}

static void
szh_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void
szh_put_le64(uint8_t *p, uint64_t v) {
  szh_put_le32(p, (uint32_t)v);
  szh_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* The 7z variable length number: the high bits of the first byte say how many
   more bytes follow, and the remaining bits of the first byte are the high
   part of the value.  This mirrors ReadNumber() in 7zArcIn.c byte for byte,
   including its habit of returning a partial value when all eight flag bits
   are set -- the caller checks the CRC of the whole record anyway. */
static int
szh_num(const uint8_t *d, size_t size, size_t *pos, uint64_t *value) {
  size_t p = *pos;
  unsigned first, mask, v, i;

  if(p >= size) {
    return -1;
  }
  first = d[p++];
  if((first & 0x80) == 0) {
    *value = first;
    *pos = p;
    return 0;
  }
  if(p >= size) {
    return -1;
  }
  v = d[p++];
  if((first & 0x40) == 0) {
    *value = ((uint64_t)(first & 0x3F) << 8) | v;
    *pos = p;
    return 0;
  }
  if(p >= size) {
    return -1;
  }
  mask = d[p++];
  *value = (uint64_t)v | ((uint64_t)mask << 8);
  mask = 0x20;
  for(i = 16; i < 64; i += 8) {
    if((first & mask) == 0) {
      *value |= (uint64_t)(first & (mask - 1)) << i;
      *pos = p;
      return 0;
    }
    mask >>= 1;
    if(p >= size) {
      return -1;
    }
    *value |= (uint64_t)d[p++] << i;
  }
  *pos = p;
  return 0;
}

/* The 32 bit form used for counts and property sizes. */
static int
szh_num32(const uint8_t *d, size_t size, size_t *pos, uint32_t *value) {
  uint64_t v;

  if(*pos < size && (d[*pos] & 0x80) == 0) {
    *value = d[(*pos)++];
    return 0;
  }
  if(szh_num(d, size, pos, &v)) {
    return -1;
  }
  if(v >= (uint64_t)0x80000000u - 1) {
    return -1;
  }
  *value = (uint32_t)v;
  return 0;
}

static uint32_t
szh_count_bits(const uint8_t *d, uint32_t num_items) {
  uint32_t n = 0, i;

  for(i = 0; i < num_items; i++) {
    if(d[i >> 3] & (0x80u >> (i & 7))) {
      n++;
    }
  }
  return n;
}

/* A digest block: one "all are defined" byte, an optional bit vector, then one
   little endian CRC per defined item.  With `first` the value of the first
   defined item comes back, which is the folder CRC when the block covers a
   single folder. */
static int
szh_digests(const uint8_t *d, size_t size, size_t *pos, uint32_t num_items,
            uint32_t *first, int *has_first) {
  size_t p = *pos;
  uint32_t defined = num_items;
  unsigned all;

  if(p >= size) {
    return -1;
  }
  all = d[p++];
  if(!all) {
    size_t bytes = ((size_t)num_items + 7) >> 3;

    if(bytes > size - p) {
      return -1;
    }
    defined = szh_count_bits(d + p, num_items);
    p += bytes;
  }
  if((size_t)defined > (size - p) >> 2) {
    return -1;
  }
  if(first && has_first) {
    if(defined) {
      *first = szh_le32(d + p);
      *has_first = 1;
    } else {
      *has_first = 0;
    }
  }
  p += (size_t)defined * 4;
  *pos = p;
  return 0;
}

/* The SDK's SkipData(): a length, then that many bytes. */
static int
szh_skip_data(const uint8_t *d, size_t size, size_t *pos) {
  uint64_t n;

  if(szh_num(d, size, pos, &n)) {
    return -1;
  }
  if(n > (uint64_t)(size - *pos)) {
    return -1;
  }
  *pos += (size_t)n;
  return 0;
}

/* --------------------------------------------------------- encoded header */

typedef struct {
  uint64_t pack_pos; /* where this folder's packs live, from offset 32 */
  uint32_t num_pack;
  uint64_t pack_sizes[SZH_MAX_STREAMS];
  uint64_t pack_positions[SZH_MAX_STREAMS + 1]; /* cumulative, as the SDK builds */
  const uint8_t *blob;                          /* coder descriptor, in place */
  size_t blob_size;
  uint32_t num_coders;
  uint32_t main_coder;
  uint64_t coder_unpack_sizes[SZH_MAX_CODERS];
  uint64_t unpack_size;
  uint32_t crc;
  int has_crc;
} szh_streams_t;

/* One folder descriptor: the coders, then the bond pairs and the pack-stream
   indices that follow them.  `*pos` ends up just past the last of those, so
   [start, *pos) is exactly the byte range the SDK keeps as
   CSzAr::CodersData[FoCodersOffsets[0] .. FoCodersOffsets[1]). */
static int
szh_parse_folder(const uint8_t *d, size_t size, size_t *pos, szh_streams_t *ss) {
  const size_t start = *pos;
  size_t p = start;
  uint32_t num_coders = 0, num_in = 0, num_bonds, num_pack, i;
  uint8_t coder_used[SZH_MAX_CODERS];
  uint8_t stream_used[SZH_MAX_STREAMS];
  uint32_t main_index = 0;

  if(szh_num32(d, size, &p, &num_coders)) {
    return -1;
  }
  if(num_coders == 0 || num_coders > SZH_MAX_CODERS) {
    return -1;
  }

  for(i = 0; i < num_coders; i++) {
    uint8_t main_byte;
    uint32_t id_size, coder_in = 1;

    if(p >= size) {
      return -1;
    }
    main_byte = d[p++];
    if(main_byte & 0xC0) {
      return -1;
    }
    id_size = main_byte & 0x0F;
    if(id_size > 8 || (size_t)id_size > size - p) {
      return -1;
    }
    p += id_size;
    if(main_byte & 0x10) {
      uint32_t coder_out;

      if(szh_num32(d, size, &p, &coder_in) ||
         szh_num32(d, size, &p, &coder_out)) {
        return -1;
      }
      /* The header scanner accepts exactly one output stream per coder, so a
         coder index and its output-stream index coincide. */
      if(coder_out != 1) {
        return -1;
      }
    }
    if(num_in >= SZH_MAX_STREAMS || coder_in > SZH_MAX_STREAMS - num_in) {
      return -1;
    }
    num_in += coder_in;
    if(main_byte & 0x20) {
      uint32_t props_size;

      if(szh_num32(d, size, &p, &props_size) ||
         (size_t)props_size > size - p) {
        return -1;
      }
      p += props_size;
    }
  }

  num_bonds = num_coders - 1;
  if(num_in < num_bonds) {
    return -1;
  }
  num_pack = num_in - num_bonds;
  if(num_pack != ss->num_pack) {
    return -1;
  }

  memset(coder_used, 0, sizeof(coder_used));
  memset(stream_used, 0, sizeof(stream_used));

  for(i = 0; i < num_bonds; i++) {
    uint32_t in_index, out_index;

    if(szh_num32(d, size, &p, &in_index)) {
      return -1;
    }
    if(in_index >= num_in || stream_used[in_index]) {
      return -1;
    }
    stream_used[in_index] = 1;
    if(szh_num32(d, size, &p, &out_index)) {
      return -1;
    }
    if(out_index >= num_coders || coder_used[out_index]) {
      return -1;
    }
    coder_used[out_index] = 1;
  }
  if(num_pack != 1) {
    for(i = 0; i < num_pack; i++) {
      uint32_t index;

      if(szh_num32(d, size, &p, &index)) {
        return -1;
      }
      if(index >= num_in || stream_used[index]) {
        return -1;
      }
      stream_used[index] = 1;
    }
  }

  /* The coder no bond consumes produces the folder's output. */
  while(main_index < num_coders && coder_used[main_index]) {
    main_index++;
  }
  if(main_index >= num_coders) {
    return -1;
  }

  ss->blob = d + start;
  ss->blob_size = p - start;
  ss->num_coders = num_coders;
  ss->main_coder = main_index;
  *pos = p;
  return 0;
}

/* Reads the StreamsInfo of a k7zIdEncodedHeader record.  Only the parts this
   module acts on are interpreted; anything else is skipped the way the SDK
   skips an id it does not know. */
static int
szh_parse_streams(const uint8_t *d, size_t size, szh_streams_t *ss) {
  size_t pos = 1; /* past k7zIdEncodedHeader */
  uint64_t id;
  uint32_t i;
  int seen_folder = 0, seen_sizes = 0;

  memset(ss, 0, sizeof(*ss));

  if(szh_num(d, size, &pos, &id) || id != SZH_ID_PACK_INFO) {
    return -1;
  }
  if(szh_num(d, size, &pos, &ss->pack_pos) ||
     szh_num32(d, size, &pos, &ss->num_pack)) {
    return -1;
  }
  if(ss->num_pack == 0 || ss->num_pack > SZH_MAX_STREAMS) {
    return -1;
  }
  {
    int got_sizes = 0;

    for(;;) {
      if(szh_num(d, size, &pos, &id)) {
        return -1;
      }
      if(id == SZH_ID_END) {
        break;
      }
      if(id == SZH_ID_SIZE) {
        if(got_sizes) {
          return -1;
        }
        for(i = 0; i < ss->num_pack; i++) {
          if(szh_num(d, size, &pos, &ss->pack_sizes[i])) {
            return -1;
          }
        }
        got_sizes = 1;
        continue;
      }
      if(szh_skip_data(d, size, &pos)) {
        return -1;
      }
    }
    if(!got_sizes) {
      return -1;
    }
  }
  ss->pack_positions[0] = 0;
  for(i = 0; i < ss->num_pack; i++) {
    ss->pack_positions[i + 1] = ss->pack_positions[i] + ss->pack_sizes[i];
    if(ss->pack_positions[i + 1] < ss->pack_positions[i]) {
      return -1;
    }
  }

  if(szh_num(d, size, &pos, &id) || id != SZH_ID_UNPACK_INFO) {
    return -1;
  }
  for(;;) {
    if(szh_num(d, size, &pos, &id)) {
      return -1;
    }
    if(id == SZH_ID_END) {
      break;
    }
    if(id == SZH_ID_FOLDER) {
      uint32_t num_folders;

      if(seen_folder || szh_num32(d, size, &pos, &num_folders)) {
        return -1;
      }
      /* SzArEx_Open2 decodes this record with numFoldersMax = 1, and the
         `external` flag has to be clear for the table to be inline. */
      if(num_folders != 1 || pos >= size || d[pos] != 0) {
        return -1;
      }
      pos++;
      if(szh_parse_folder(d, size, &pos, ss)) {
        return -1;
      }
      seen_folder = 1;
      continue;
    }
    if(id == SZH_ID_CODERS_UNPACK_SIZE) {
      if(!seen_folder || seen_sizes) {
        return -1;
      }
      for(i = 0; i < ss->num_coders; i++) {
        if(szh_num(d, size, &pos, &ss->coder_unpack_sizes[i])) {
          return -1;
        }
      }
      seen_sizes = 1;
      continue;
    }
    if(id == SZH_ID_CRC) {
      uint32_t crc = 0;
      int has = 0;

      if(ss->has_crc || szh_digests(d, size, &pos, 1, &crc, &has)) {
        return -1;
      }
      if(has) {
        ss->crc = crc;
        ss->has_crc = 1;
      }
      continue;
    }
    if(szh_skip_data(d, size, &pos)) {
      return -1;
    }
  }
  if(!seen_folder || !seen_sizes) {
    return -1;
  }
  ss->unpack_size = ss->coder_unpack_sizes[ss->main_coder];

  /* A SubStreamsInfo may hold the folder CRC when UnpackInfo carried none, but
     nothing past this point changes what we decode. */
  return 0;
}

/* ------------------------------------------------------------- stream I/O */

static uint64_t
szh_size_of(ISeekInStream *s) {
  Int64 pos = 0;

  if(s->Seek(s, &pos, SZ_SEEK_END) != SZ_OK || pos < 0) {
    return 0;
  }
  return (uint64_t)pos;
}

static int
szh_read_at(ISeekInStream *s, uint64_t offset, void *dst, size_t size) {
  Int64 pos = (Int64)offset;
  size_t got = 0;

  if(s->Seek(s, &pos, SZ_SEEK_SET) != SZ_OK) {
    return -1;
  }
  while(got < size) {
    size_t want = size - got;

    if(s->Read(s, (uint8_t *)dst + got, &want) != SZ_OK) {
      return -1;
    }
    if(want == 0) {
      return -1;
    }
    got += want;
  }
  return 0;
}

/* The virtual view handed to the SDK.  `vt` has to stay first: the SDK casts
   the interface pointer straight back to this struct, exactly as
   sevenz_volstream.c does. */
typedef struct {
  ISeekInStream vt;

  ISeekInStream *raw;
  uint64_t raw_size;
  uint64_t pos;
  uint64_t total; /* the virtual length; the header may stick out past the file */

  uint8_t sig[k7zStartHeaderSize]; /* start header, rewritten for the plaintext */
  uint64_t hdr_off;
  uint8_t *hdr;
  uint64_t hdr_len;
} szh_view;

struct szh_prep {
  szh_view view;
};

static SRes
szh_view_read(const ISeekInStream *p, void *buf, size_t *size) {
  szh_view *v = (szh_view *)p;
  uint8_t *dst = (uint8_t *)buf;
  const size_t want = *size;
  size_t got = 0;

  *size = 0;
  while(got < want) {
    const uint64_t pos = v->pos;
    const uint64_t hdr_end = v->hdr_off + v->hdr_len;

    if(pos < k7zStartHeaderSize) {
      size_t n = (size_t)(k7zStartHeaderSize - pos);

      if(n > want - got) {
        n = want - got;
      }
      memcpy(dst + got, v->sig + pos, n);
      got += n;
      v->pos += n;
      continue;
    }
    if(pos >= v->hdr_off && pos < hdr_end) {
      size_t n = (size_t)(hdr_end - pos);

      if(n > want - got) {
        n = want - got;
      }
      memcpy(dst + got, v->hdr + (pos - v->hdr_off), n);
      got += n;
      v->pos += n;
      continue;
    }
    /* Everything else is the real archive.  The header region can reach past
       its end, so a read is allowed to stop short here. */
    if(pos >= v->raw_size) {
      break;
    }
    {
      Int64 raw_pos = (Int64)pos;
      size_t n = want - got;
      const uint64_t avail = v->raw_size - pos;

      if((uint64_t)n > avail) {
        n = (size_t)avail;
      }
      if(v->raw->Seek(v->raw, &raw_pos, SZ_SEEK_SET) != SZ_OK) {
        return SZ_ERROR_READ;
      }
      if(v->raw->Read(v->raw, dst + got, &n) != SZ_OK) {
        return SZ_ERROR_READ;
      }
      if(n == 0) {
        break;
      }
      got += n;
      v->pos += n;
    }
  }
  *size = got;
  return SZ_OK;
}

static SRes
szh_view_seek(const ISeekInStream *p, Int64 *pos, ESzSeek origin) {
  szh_view *v = (szh_view *)p;
  Int64 base;
  Int64 next;

  switch(origin) {
  case SZ_SEEK_SET:
    base = 0;
    break;
  case SZ_SEEK_CUR:
    base = (Int64)v->pos;
    break;
  case SZ_SEEK_END:
    base = (Int64)v->total;
    break;
  default:
    return SZ_ERROR_PARAM;
  }
  next = base + *pos;
  if(next < 0) {
    next = 0;
  }
  v->pos = (uint64_t)next;
  *pos = next;
  return SZ_OK;
}

/* ------------------------------------------------------ decoding the header */

typedef struct {
  ISeekInStream *raw;
  uint64_t base;
} szh_reader_t;

static int
szh_chain_read(void *ctx, uint64_t offset, void *dst, size_t size) {
  szh_reader_t *r = (szh_reader_t *)ctx;

  return szh_read_at(r->raw, r->base + offset, dst, size);
}

/* Collects the decrypted header.  It grows on demand rather than trusting the
   declared unpack size, which is attacker controlled. */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  int failed;
} szh_sink_t;

static int
szh_sink_write(void *ctx, const void *data, size_t size) {
  szh_sink_t *s = (szh_sink_t *)ctx;

  if(s->failed) {
    return -1;
  }
  if((uint64_t)size > SZH_MAX_HEADER - (uint64_t)s->len) {
    s->failed = 1;
    return -1;
  }
  if(s->len + size > s->cap) {
    size_t cap = s->cap ? s->cap : 4096;
    uint8_t *grown;

    while(cap < s->len + size) {
      cap *= 2;
    }
    grown = (uint8_t *)realloc(s->data, cap);
    if(!grown) {
      s->failed = 1;
      return -1;
    }
    s->data = grown;
    s->cap = cap;
  }
  memcpy(s->data + s->len, data, size);
  s->len += size;
  return 0;
}

/* ------------------------------------------------------------------ public */

const char *
szh_status_string(szh_status_t status) {
  switch(status) {
  case SZH_PLAIN:
    return "the archive header is readable";
  case SZH_PATCHED:
    return "the archive header was decrypted";
  case SZH_ERR_PASSWORD:
    return "the archive header is encrypted";
  case SZH_ERR_UNSUPPORTED:
    return "the archive header uses an unsupported arrangement";
  case SZH_ERR_FORMAT:
    return "the archive header is malformed";
  case SZH_ERR_IO:
    return "the archive header could not be read";
  }
  return "unknown";
}

static void
szh_set_msg(char *msg, size_t msg_size, const char *text, const char *detail) {
  if(!msg || !msg_size) {
    return;
  }
  if(detail) {
    snprintf(msg, msg_size, "%s: %s", text, detail);
  } else {
    snprintf(msg, msg_size, "%s", text);
  }
}

szh_status_t
szh_prepare(szh_prep **out, ISeekInStream *raw, const char *password,
            char *msg, size_t msg_size) {
  uint8_t sig[k7zStartHeaderSize];
  uint64_t raw_size, next_off, next_size;
  uint32_t next_crc;
  uint8_t *raw_hdr = NULL;
  szh_streams_t ss;
  sz_chain *chain = NULL;
  sz_chain_err_t cerr;
  szh_sink_t sink;
  szh_reader_t reader;
  szh_prep *prep = NULL;
  szh_status_t status = SZH_PLAIN;
  uint32_t decoded_crc = 0;

  if(out) {
    *out = NULL;
  }
  if(!out || !raw) {
    return SZH_ERR_IO;
  }
  if(msg && msg_size) {
    msg[0] = 0;
  }

  /* Sevenz extraction runs this before SzArEx_Open, but the table is what
     every CRC below needs and generating it twice costs nothing. */
  CrcGenerateTable();
  memset(&cerr, 0, sizeof(cerr));
  memset(&sink, 0, sizeof(sink));

  /* --- the start header ------------------------------------------------ */
  raw_size = szh_size_of(raw);
  if(raw_size < k7zStartHeaderSize || szh_read_at(raw, 0, sig, sizeof(sig))) {
    return SZH_PLAIN;
  }
  if(memcmp(sig, k7zSignature, k7zSignatureSize) != 0 || sig[6] != 0) {
    return SZH_PLAIN;
  }
  if(CrcCalc(sig + 12, 20) != szh_le32(sig + 8)) {
    return SZH_PLAIN;
  }

  next_off = szh_le64(sig + 12);
  next_size = szh_le64(sig + 20);
  next_crc = szh_le32(sig + 28);

  if(next_size == 0 || next_size > SZH_MAX_HEADER) {
    return SZH_PLAIN;
  }
  if(next_off > raw_size || next_size > raw_size - next_off) {
    return SZH_PLAIN;
  }

  /* --- the next header ------------------------------------------------- */
  /* One byte decides it: an ordinary header (k7zIdHeader) is none of our
     business, and a big uncompressed one is not worth reading twice. */
  {
    uint8_t first_byte = 0;

    if(szh_read_at(raw, k7zStartHeaderSize + next_off, &first_byte, 1) ||
       first_byte != SZH_ID_ENCODED_HEADER) {
      return SZH_PLAIN;
    }
  }

  raw_hdr = (uint8_t *)malloc((size_t)next_size);
  if(!raw_hdr) {
    szh_set_msg(msg, msg_size, "out of memory reading the archive header",
                NULL);
    return SZH_ERR_IO;
  }
  if(szh_read_at(raw, k7zStartHeaderSize + next_off, raw_hdr,
                 (size_t)next_size) ||
     CrcCalc(raw_hdr, (size_t)next_size) != next_crc) {
    /* Broken or truncated: let the SDK diagnose it exactly as it always has. */
    status = SZH_PLAIN;
    goto done;
  }
  if(szh_parse_streams(raw_hdr, (size_t)next_size, &ss)) {
    status = SZH_PLAIN;
    goto done;
  }

  /* --- the folder behind it -------------------------------------------- */
  if(sz_chain_parse(&chain, ss.blob, ss.blob_size, ss.pack_positions,
                    ss.num_pack, ss.coder_unpack_sizes, ss.unpack_size,
                    sz_chain_default_limits(), &cerr) != 0) {
    /* Not ours to report: the SDK rejects such an archive too, and it is the
       one that knows how to describe it. */
    status = SZH_PLAIN;
    goto done;
  }
  if(!sz_chain_needs_password(chain)) {
    /* An ordinary compressed header (-mhc=on), which the SDK decodes itself. */
    status = SZH_PLAIN;
    goto done;
  }
  if(!password || !password[0]) {
    szh_set_msg(msg, msg_size,
                "the archive header is encrypted (-mhe=on), so the file names, "
                "the folder table and the entry sizes are all inside it and the "
                "archive cannot be listed or unpacked without the password",
                NULL);
    status = SZH_ERR_PASSWORD;
    goto done;
  }

  reader.raw = raw;
  reader.base = (uint64_t)k7zStartHeaderSize + ss.pack_pos;
  if(sz_chain_decode(chain, szh_chain_read, &reader, szh_sink_write, &sink, NULL,
                     NULL, password, &decoded_crc, &cerr) != 0 ||
     sink.failed) {
    switch(cerr.status) {
    case SZ_CHAIN_ERR_PASSWORD:
    case SZ_CHAIN_ERR_DATA:
      szh_set_msg(msg, msg_size,
                  "the encrypted archive header did not decrypt: the password "
                  "is wrong, or the archive is damaged",
                  cerr.message);
      status = SZH_ERR_PASSWORD;
      break;
    case SZ_CHAIN_ERR_LIMIT:
    case SZ_CHAIN_ERR_METHOD:
    case SZ_CHAIN_ERR_LAYOUT:
      szh_set_msg(msg, msg_size, "cannot decode the encrypted archive header",
                  cerr.message);
      status = SZH_ERR_UNSUPPORTED;
      break;
    case SZ_CHAIN_ERR_MEM:
      szh_set_msg(msg, msg_size, "out of memory decoding the archive header",
                  NULL);
      status = SZH_ERR_IO;
      break;
    default:
      szh_set_msg(msg, msg_size, "cannot decode the encrypted archive header",
                  cerr.message);
      status = SZH_ERR_FORMAT;
      break;
    }
    goto done;
  }
  if(sink.len == 0) {
    szh_set_msg(msg, msg_size,
                "the encrypted archive header decrypted to nothing", NULL);
    status = SZH_ERR_FORMAT;
    goto done;
  }
  if(ss.has_crc && decoded_crc != ss.crc) {
    szh_set_msg(msg, msg_size,
                "the encrypted archive header decrypted to data that fails its "
                "CRC -- the password is wrong",
                NULL);
    status = SZH_ERR_PASSWORD;
    goto done;
  }
  if(sink.data[0] != SZH_ID_HEADER) {
    szh_set_msg(msg, msg_size,
                "the encrypted archive header did not decrypt to a 7z header "
                "-- the password is wrong",
                NULL);
    status = SZH_ERR_PASSWORD;
    goto done;
  }

  /* --- hand the plaintext to the SDK ----------------------------------- */
  prep = (szh_prep *)calloc(1, sizeof(*prep));
  if(!prep) {
    szh_set_msg(msg, msg_size, "out of memory reading the archive header",
                NULL);
    status = SZH_ERR_IO;
    goto done;
  }
  prep->view.raw = raw;
  prep->view.raw_size = raw_size;
  prep->view.pos = 0;
  prep->view.hdr_off = (uint64_t)k7zStartHeaderSize + next_off;
  prep->view.hdr = sink.data;
  prep->view.hdr_len = sink.len;
  prep->view.total = prep->view.hdr_off + prep->view.hdr_len;
  if(prep->view.total < raw_size) {
    prep->view.total = raw_size;
  }
  sink.data = NULL; /* owned by the view from here on */

  memcpy(prep->view.sig, sig, sizeof(prep->view.sig));
  szh_put_le64(prep->view.sig + 12, prep->view.hdr_off - k7zStartHeaderSize);
  szh_put_le64(prep->view.sig + 20, prep->view.hdr_len);
  szh_put_le32(prep->view.sig + 28,
               CrcCalc(prep->view.hdr, (size_t)prep->view.hdr_len));
  szh_put_le32(prep->view.sig + 8, CrcCalc(prep->view.sig + 12, 20));

  prep->view.vt.Read = szh_view_read;
  prep->view.vt.Seek = szh_view_seek;

  *out = prep;
  prep = NULL;
  status = SZH_PATCHED;

done:
  free(raw_hdr);
  free(sink.data);
  sz_chain_free(chain);
  if(prep) {
    free(prep->view.hdr);
    free(prep);
  }
  return status;
}

ISeekInStream *
szh_stream(const szh_prep *p) {
  return p ? (ISeekInStream *)&p->view.vt : NULL;
}

void
szh_prep_free(szh_prep *p) {
  if(p) {
    free(p->view.hdr);
    free(p);
  }
}
