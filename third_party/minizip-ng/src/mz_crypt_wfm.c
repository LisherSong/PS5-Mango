/* mz_crypt_wfm.c -- crypto/hash backend for the vendored minizip-ng
   part of the PS5 web file manager

   minizip-ng keeps its hash and AES primitives behind a small pluggable
   provider (mz_crypt_openssl.c, mz_crypt_brg.c, mz_crypt_apple.c, ...).
   This file is the provider used here.  It implements exactly the primitives
   the ZIP decompression path needs and nothing else.

   What the two ZIP encryption schemes require:

     * Traditional PKWARE ("ZipCrypto", i.e. `zip -e`) needs neither AES nor a
       hash: only crc32, which mz_crypt.c already provides, plus
       mz_crypt_rand() on the write path.  mz_strm_pkcrypt.c carries the
       algorithm itself.

     * WinZip AES (the 0x9901 extra field, `-Z aes`) needs
         - PBKDF2-HMAC-SHA1 to derive the keys   -> mz_crypt.c (HAVE_WZAES)
         - HMAC-SHA1 over the ciphertext         -> here
         - AES-128/192/256 in ECB on the counter -> here
       mz_strm_wzaes.c turns the ECB primitive into the CTR-like keystream
       WinZip specifies, so only single-block ECB is on the hot path.

   The primitives are implemented locally instead of delegating to another
   vendored library so that this subtree stays self contained: it builds and
   is testable with nothing but zlib and libc, on both prospero-clang and the
   MinGW host test harness.  It also deliberately avoids the 16-byte alignment
   contract that third_party/7z/Aes.h imposes, because mz_strm_wzaes.c
   encrypts a counter block that lives inside a heap struct.

   Only MZ_HASH_SHA1 is implemented.  That is the only algorithm minizip-ng
   asks for on these code paths (mz_strm_wzaes.c and mz_crypt_pbkdf2()); every
   other MZ_HASH_* identifier is rejected with MZ_SUPPORT_ERROR rather than
   silently producing a wrong digest.

   The AES tables are derived at first use and live in .bss, so the .rodata
   cost of this file is zero.
*/

#include "mz.h"
#include "mz_crypt.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/**************************************************************************
 * entropy
 **************************************************************************/

int32_t mz_crypt_rand(uint8_t *buf, int32_t size) {
    /* Only the archive *writing* paths reach this (mz_strm_wzaes.c generates
       the salt and mz_strm_pkcrypt.c the encryption header); this payload only
       ever reads archives. It is served from /dev/urandom rather than
       minizip's mz_os_rand() ladder on purpose:
         * mz_os_rand() falls back to libc rand()/srand() here, which would add
           a load-time dependency on two symbols for a path we never take --
           and rand() is not a sound source for an encryption key anyway;
         * open/read/close are already imported by the rest of the payload, so
           this adds no new symbol.
       If /dev/urandom cannot be opened the caller gets MZ_INTERNAL_ERROR
       instead of silently weak randomness. */
    int fd;
    int32_t done = 0;

    if (!buf || size < 0)
        return MZ_PARAM_ERROR;
    if (size == 0)
        return 0;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return MZ_INTERNAL_ERROR;

    while (done < size) {
        ssize_t n = read(fd, buf + done, (size_t)(size - done));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return MZ_INTERNAL_ERROR;
        }
        if (n == 0)
            break;
        done += (int32_t)n;
    }

    close(fd);
    return done == size ? size : MZ_INTERNAL_ERROR;
}

/**************************************************************************
 * SHA-1 (FIPS 180-4)
 **************************************************************************/

#define WFM_SHA1_SIZE  20
#define WFM_SHA1_BLOCK 64

typedef struct wfm_sha1_s {
    uint32_t h[5];
    uint64_t total; /* message bytes consumed so far */
    uint8_t block[WFM_SHA1_BLOCK];
    uint32_t fill;  /* bytes currently buffered in block */
} wfm_sha1_t;

static uint32_t wfm_rol32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void wfm_sha1_init(wfm_sha1_t *s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->total = 0;
    s->fill = 0;
}

static void wfm_sha1_compress(wfm_sha1_t *s, const uint8_t *p) {
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
               (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++)
        w[i] = wfm_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        t = wfm_rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = wfm_rol32(b, 30);
        b = a;
        a = t;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void wfm_sha1_update(wfm_sha1_t *s, const void *buf, size_t size) {
    const uint8_t *p = (const uint8_t *)buf;

    s->total += size;

    if (s->fill) {
        uint32_t need = WFM_SHA1_BLOCK - s->fill;
        if (size < need) {
            memcpy(s->block + s->fill, p, size);
            s->fill += (uint32_t)size;
            return;
        }
        memcpy(s->block + s->fill, p, need);
        wfm_sha1_compress(s, s->block);
        p += need;
        size -= need;
        s->fill = 0;
    }

    while (size >= WFM_SHA1_BLOCK) {
        wfm_sha1_compress(s, p);
        p += WFM_SHA1_BLOCK;
        size -= WFM_SHA1_BLOCK;
    }

    if (size) {
        memcpy(s->block, p, size);
        s->fill = (uint32_t)size;
    }
}

/* Writes the full 20-byte digest.  The context is left in a finalized state. */
static void wfm_sha1_final(wfm_sha1_t *s, uint8_t out[WFM_SHA1_SIZE]) {
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80;
    uint8_t len[8];
    uint32_t i;

    wfm_sha1_update(s, &pad, 1);
    pad = 0x00;
    while (s->fill != WFM_SHA1_BLOCK - 8)
        wfm_sha1_update(s, &pad, 1);

    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    wfm_sha1_update(s, len, 8);

    for (i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

/**************************************************************************
 * HMAC-SHA1 (RFC 2104)
 **************************************************************************/

typedef struct wfm_hmac_s {
    uint16_t algorithm;
    int32_t initialized;
    wfm_sha1_t inner;                 /* running hash of ipad || message */
    uint8_t opad[WFM_SHA1_BLOCK];     /* key ^ 0x5c, zero padded */
} wfm_hmac_t;

/* ipad = key ^ 0x36 and opad = key ^ 0x5c, so ipad can be recovered from
   opad without storing the key twice. */
#define WFM_HMAC_OPAD 0x5c
#define WFM_HMAC_PAD_DIFF (0x5c ^ 0x36)

int32_t mz_crypt_hmac_init(void *handle, const void *key, int32_t key_length) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)handle;
    uint8_t ipad[WFM_SHA1_BLOCK];
    uint8_t digest[WFM_SHA1_SIZE];
    wfm_sha1_t k;
    uint32_t i;

    if (!hmac || (!key && key_length > 0) || key_length < 0)
        return MZ_PARAM_ERROR;
    if (hmac->algorithm != MZ_HASH_SHA1)
        return MZ_SUPPORT_ERROR;

    memset(hmac->opad, 0, sizeof(hmac->opad));

    if (key_length > WFM_SHA1_BLOCK) {
        /* The key is hashed down to the digest size first. */
        wfm_sha1_init(&k);
        wfm_sha1_update(&k, key, (size_t)key_length);
        wfm_sha1_final(&k, digest);
        memcpy(hmac->opad, digest, WFM_SHA1_SIZE);
    } else if (key_length > 0) {
        memcpy(hmac->opad, key, (size_t)key_length);
    }

    /* opad = K ^ 0x5c, and ipad = K ^ 0x36 = opad ^ 0x5c ^ 0x36, so the key
       only has to be stored once. */
    for (i = 0; i < WFM_SHA1_BLOCK; i++)
        hmac->opad[i] ^= WFM_HMAC_OPAD;
    for (i = 0; i < WFM_SHA1_BLOCK; i++)
        ipad[i] = (uint8_t)(hmac->opad[i] ^ WFM_HMAC_PAD_DIFF);

    wfm_sha1_init(&hmac->inner);
    wfm_sha1_update(&hmac->inner, ipad, WFM_SHA1_BLOCK);

    hmac->initialized = 1;
    return MZ_OK;
}

int32_t mz_crypt_hmac_update(void *handle, const void *buf, int32_t size) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)handle;

    if (!hmac || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (!hmac->initialized)
        return MZ_PARAM_ERROR;

    wfm_sha1_update(&hmac->inner, buf, (size_t)size);
    return MZ_OK;
}

int32_t mz_crypt_hmac_end(void *handle, uint8_t *digest, int32_t digest_size) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)handle;
    uint8_t inner_digest[WFM_SHA1_SIZE];
    wfm_sha1_t outer;

    if (!hmac || !digest)
        return MZ_PARAM_ERROR;
    if (!hmac->initialized)
        return MZ_PARAM_ERROR;
    if (digest_size < WFM_SHA1_SIZE)
        return MZ_BUF_ERROR;

    wfm_sha1_final(&hmac->inner, inner_digest);

    wfm_sha1_init(&outer);
    wfm_sha1_update(&outer, hmac->opad, WFM_SHA1_BLOCK);
    wfm_sha1_update(&outer, inner_digest, WFM_SHA1_SIZE);
    wfm_sha1_final(&outer, digest);

    return MZ_OK;
}

int32_t mz_crypt_hmac_copy(void *src_handle, void *target_handle) {
    wfm_hmac_t *source = (wfm_hmac_t *)src_handle;
    wfm_hmac_t *target = (wfm_hmac_t *)target_handle;

    if (!source || !target)
        return MZ_PARAM_ERROR;
    /* The context owns no pointers, so a plain copy is a deep copy.  This is
       what mz_crypt_pbkdf2() relies on when it clones the mid state. */
    memcpy(target, source, sizeof(*target));
    return MZ_OK;
}

void mz_crypt_hmac_set_algorithm(void *handle, uint16_t algorithm) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)handle;
    if (hmac)
        hmac->algorithm = algorithm;
}

void mz_crypt_hmac_reset(void *handle) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)handle;
    if (!hmac)
        return;
    memset(&hmac->inner, 0, sizeof(hmac->inner));
    memset(hmac->opad, 0, sizeof(hmac->opad));
    hmac->initialized = 0;
}

void *mz_crypt_hmac_create(void) {
    wfm_hmac_t *hmac = (wfm_hmac_t *)calloc(1, sizeof(wfm_hmac_t));
    if (hmac)
        hmac->algorithm = MZ_HASH_SHA1;
    return hmac;
}

void mz_crypt_hmac_delete(void **handle) {
    wfm_hmac_t *hmac = NULL;
    if (!handle)
        return;
    hmac = (wfm_hmac_t *)*handle;
    if (hmac) {
        memset(hmac, 0, sizeof(*hmac));
        free(hmac);
    }
    *handle = NULL;
}

/**************************************************************************
 * mz_crypt_sha_* -- the standalone hash API declared by mz_crypt.h.
 *                      Only SHA-1 is available (see the file header).
 **************************************************************************/

typedef struct wfm_sha_handle_s {
    uint16_t algorithm;
    int32_t initialized;
    wfm_sha1_t ctx;
} wfm_sha_handle_t;

int32_t mz_crypt_sha_set_algorithm(void *handle, uint16_t algorithm) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)handle;
    if (!sha)
        return MZ_PARAM_ERROR;
    if (algorithm != MZ_HASH_SHA1)
        return MZ_SUPPORT_ERROR;
    sha->algorithm = algorithm;
    return MZ_OK;
}

int32_t mz_crypt_sha_begin(void *handle) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)handle;
    if (!sha)
        return MZ_PARAM_ERROR;
    if (sha->algorithm != MZ_HASH_SHA1)
        return MZ_SUPPORT_ERROR;
    wfm_sha1_init(&sha->ctx);
    sha->initialized = 1;
    return MZ_OK;
}

int32_t mz_crypt_sha_update(void *handle, const void *buf, int32_t size) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)handle;
    if (!sha || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (!sha->initialized)
        return MZ_PARAM_ERROR;
    wfm_sha1_update(&sha->ctx, buf, (size_t)size);
    return MZ_OK;
}

int32_t mz_crypt_sha_end(void *handle, uint8_t *digest, int32_t digest_size) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)handle;
    if (!sha || !digest)
        return MZ_PARAM_ERROR;
    if (!sha->initialized)
        return MZ_PARAM_ERROR;
    if (digest_size < WFM_SHA1_SIZE)
        return MZ_PARAM_ERROR;
    wfm_sha1_final(&sha->ctx, digest);
    sha->initialized = 0;
    return MZ_OK;
}

void mz_crypt_sha_reset(void *handle) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)handle;
    if (!sha)
        return;
    wfm_sha1_init(&sha->ctx);
    sha->initialized = 0;
}

void *mz_crypt_sha_create(void) {
    wfm_sha_handle_t *sha = (wfm_sha_handle_t *)calloc(1, sizeof(wfm_sha_handle_t));
    if (sha)
        sha->algorithm = MZ_HASH_SHA1;
    return sha;
}

void mz_crypt_sha_delete(void **handle) {
    wfm_sha_handle_t *sha = NULL;
    if (!handle)
        return;
    sha = (wfm_sha_handle_t *)*handle;
    if (sha) {
        memset(sha, 0, sizeof(*sha));
        free(sha);
    }
    *handle = NULL;
}

/**************************************************************************
 * AES-128/192/256 (FIPS 197)
 *
 * The tables are derived once from the field arithmetic instead of being
 * spelled out, which keeps the .rodata cost at zero and removes any chance of
 * a transcription error in a 256 entry table.
 **************************************************************************/

#define WFM_AES_BLOCK       16
#define WFM_AES_MAX_WORDS   60 /* (14 rounds + 1) * 4 */

static uint8_t wfm_aes_sbox[256];
static uint8_t wfm_aes_inv_sbox[256];
static uint8_t wfm_aes_log[256];  /* log base 3 */
static uint8_t wfm_aes_alog[256]; /* 3^i, alog[255] wraps to 1 */
static uint32_t wfm_aes_te[4][256];
static int wfm_aes_tables_ready;

static uint8_t wfm_aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static uint8_t wfm_rol8(uint8_t value, unsigned bits) {
    return (uint8_t)((value << bits) | (value >> (8 - bits)));
}

static void wfm_aes_build_tables(void) {
    uint8_t x = 1;
    int i;

    if (wfm_aes_tables_ready)
        return;
    wfm_aes_tables_ready = 1;

    for (i = 0; i < 255; i++) {
        wfm_aes_alog[i] = x;
        wfm_aes_log[x] = (uint8_t)i;
        x ^= wfm_aes_xtime(x); /* x *= 3 */
    }
    wfm_aes_alog[255] = 1;
    wfm_aes_log[0] = 0; /* log is never used for 0 */

    for (i = 0; i < 256; i++) {
        /* Multiplicative inverse in GF(2^8), then the affine transform. */
        uint8_t inv = (i == 0) ? 0 : wfm_aes_alog[255 - wfm_aes_log[(uint8_t)i]];
        uint8_t r = (uint8_t)(inv ^ wfm_rol8(inv, 1) ^ wfm_rol8(inv, 2) ^ wfm_rol8(inv, 3) ^ wfm_rol8(inv, 4));
        r ^= 0x63;

        wfm_aes_sbox[i] = r;
        wfm_aes_inv_sbox[r] = (uint8_t)i;
    }

    for (i = 0; i < 256; i++) {
        uint8_t s = wfm_aes_sbox[i];
        uint8_t s2 = wfm_aes_xtime(s);
        uint8_t s3 = (uint8_t)(s2 ^ s);

        wfm_aes_te[0][i] = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        wfm_aes_te[1][i] = ((uint32_t)s3 << 24) | ((uint32_t)s2 << 16) | ((uint32_t)s << 8) | s;
        wfm_aes_te[2][i] = ((uint32_t)s << 24) | ((uint32_t)s3 << 16) | ((uint32_t)s2 << 8) | s;
        wfm_aes_te[3][i] = ((uint32_t)s << 24) | ((uint32_t)s << 16) | ((uint32_t)s3 << 8) | s2;
    }
}

static uint8_t wfm_aes_gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0)
        return 0;
    /* The sum of two logarithms reaches 508, so the modulo has to happen in
       int arithmetic: truncating to uint8_t first would wrap a large sum. */
    return wfm_aes_alog[(wfm_aes_log[a] + wfm_aes_log[b]) % 255];
}

typedef struct wfm_aes_s {
    uint32_t rk[WFM_AES_MAX_WORDS];
    uint8_t iv[WFM_AES_BLOCK];
    int32_t mode;
    int32_t nr;          /* number of rounds */
    int32_t keyed;
} wfm_aes_t;

static uint32_t wfm_load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wfm_store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wfm_aes_set_key(wfm_aes_t *a, const uint8_t *key, int32_t key_length) {
    int32_t nk = key_length / 4;
    int32_t total = 4 * (nk + 7);
    int32_t i;
    uint8_t rcon = 1;
    uint32_t temp;

    wfm_aes_build_tables();

    for (i = 0; i < nk; i++)
        a->rk[i] = wfm_load_be32(key + 4 * i);

    for (i = nk; i < total; i++) {
        temp = a->rk[i - 1];
        if (i % nk == 0) {
            /* SubWord(RotWord(temp)) ^ Rcon */
            temp = ((uint32_t)wfm_aes_sbox[(temp >> 16) & 0xff] << 24) |
                   ((uint32_t)wfm_aes_sbox[(temp >> 8) & 0xff] << 16) |
                   ((uint32_t)wfm_aes_sbox[temp & 0xff] << 8) | (uint32_t)wfm_aes_sbox[(temp >> 24) & 0xff];
            temp ^= (uint32_t)rcon << 24;
            rcon = wfm_aes_xtime(rcon);
        } else if (nk > 6 && (i % nk) == 4) {
            /* SubWord(temp) for AES-256 only */
            temp = ((uint32_t)wfm_aes_sbox[(temp >> 24) & 0xff] << 24) |
                   ((uint32_t)wfm_aes_sbox[(temp >> 16) & 0xff] << 16) |
                   ((uint32_t)wfm_aes_sbox[(temp >> 8) & 0xff] << 8) | (uint32_t)wfm_aes_sbox[temp & 0xff];
        }
        a->rk[i] = a->rk[i - nk] ^ temp;
    }

    a->nr = nk + 6;
    a->keyed = 1;
}

static void wfm_aes_encrypt_block(const wfm_aes_t *a, const uint8_t *in, uint8_t *out) {
    const uint32_t *rk = a->rk;
    uint32_t s0, s1, s2, s3, t0, t1, t2, t3;
    int32_t r;

    s0 = wfm_load_be32(in) ^ rk[0];
    s1 = wfm_load_be32(in + 4) ^ rk[1];
    s2 = wfm_load_be32(in + 8) ^ rk[2];
    s3 = wfm_load_be32(in + 12) ^ rk[3];

    for (r = 1; r < a->nr; r++) {
        t0 = wfm_aes_te[0][(s0 >> 24) & 0xff] ^ wfm_aes_te[1][(s1 >> 16) & 0xff] ^ wfm_aes_te[2][(s2 >> 8) & 0xff] ^
             wfm_aes_te[3][s3 & 0xff] ^ rk[4 * r + 0];
        t1 = wfm_aes_te[0][(s1 >> 24) & 0xff] ^ wfm_aes_te[1][(s2 >> 16) & 0xff] ^ wfm_aes_te[2][(s3 >> 8) & 0xff] ^
             wfm_aes_te[3][s0 & 0xff] ^ rk[4 * r + 1];
        t2 = wfm_aes_te[0][(s2 >> 24) & 0xff] ^ wfm_aes_te[1][(s3 >> 16) & 0xff] ^ wfm_aes_te[2][(s0 >> 8) & 0xff] ^
             wfm_aes_te[3][s1 & 0xff] ^ rk[4 * r + 2];
        t3 = wfm_aes_te[0][(s3 >> 24) & 0xff] ^ wfm_aes_te[1][(s0 >> 16) & 0xff] ^ wfm_aes_te[2][(s1 >> 8) & 0xff] ^
             wfm_aes_te[3][s2 & 0xff] ^ rk[4 * r + 3];

        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }

    /* Final round: SubBytes, ShiftRows, AddRoundKey. */
    t0 = ((uint32_t)wfm_aes_sbox[(s0 >> 24) & 0xff] << 24) | ((uint32_t)wfm_aes_sbox[(s1 >> 16) & 0xff] << 16) |
         ((uint32_t)wfm_aes_sbox[(s2 >> 8) & 0xff] << 8) | (uint32_t)wfm_aes_sbox[s3 & 0xff];
    t1 = ((uint32_t)wfm_aes_sbox[(s1 >> 24) & 0xff] << 24) | ((uint32_t)wfm_aes_sbox[(s2 >> 16) & 0xff] << 16) |
         ((uint32_t)wfm_aes_sbox[(s3 >> 8) & 0xff] << 8) | (uint32_t)wfm_aes_sbox[s0 & 0xff];
    t2 = ((uint32_t)wfm_aes_sbox[(s2 >> 24) & 0xff] << 24) | ((uint32_t)wfm_aes_sbox[(s3 >> 16) & 0xff] << 16) |
         ((uint32_t)wfm_aes_sbox[(s0 >> 8) & 0xff] << 8) | (uint32_t)wfm_aes_sbox[s1 & 0xff];
    t3 = ((uint32_t)wfm_aes_sbox[(s3 >> 24) & 0xff] << 24) | ((uint32_t)wfm_aes_sbox[(s0 >> 16) & 0xff] << 16) |
         ((uint32_t)wfm_aes_sbox[(s1 >> 8) & 0xff] << 8) | (uint32_t)wfm_aes_sbox[s2 & 0xff];

    wfm_store_be32(out, t0 ^ rk[4 * a->nr + 0]);
    wfm_store_be32(out + 4, t1 ^ rk[4 * a->nr + 1]);
    wfm_store_be32(out + 8, t2 ^ rk[4 * a->nr + 2]);
    wfm_store_be32(out + 12, t3 ^ rk[4 * a->nr + 3]);
}

/* Inverse cipher straight out of FIPS 197.  Byte oriented on purpose: only
   the encryption direction is on the hot path (WinZip AES encrypts the
   counter block) and the decoder must stay small. */
static void wfm_aes_decrypt_block(const wfm_aes_t *a, const uint8_t *in, uint8_t *out) {
    const uint32_t *rk = a->rk;
    uint8_t st[WFM_AES_BLOCK];
    uint8_t tmp[WFM_AES_BLOCK];
    int32_t round;
    int32_t c, r;

    /* AddRoundKey with the last round key. */
    for (c = 0; c < 4; c++) {
        uint32_t k = rk[4 * a->nr + c];
        st[4 * c + 0] = (uint8_t)(in[4 * c + 0] ^ (k >> 24));
        st[4 * c + 1] = (uint8_t)(in[4 * c + 1] ^ (k >> 16));
        st[4 * c + 2] = (uint8_t)(in[4 * c + 2] ^ (k >> 8));
        st[4 * c + 3] = (uint8_t)(in[4 * c + 3] ^ k);
    }

    for (round = a->nr - 1; round >= 1; round--) {
        /* InvShiftRows: row r rotates right by r. */
        for (c = 0; c < 4; c++) {
            for (r = 0; r < 4; r++)
                tmp[4 * c + r] = st[4 * ((c - r + 4) & 3) + r];
        }
        /* InvSubBytes */
        for (c = 0; c < WFM_AES_BLOCK; c++)
            tmp[c] = wfm_aes_inv_sbox[tmp[c]];
        /* AddRoundKey */
        for (c = 0; c < 4; c++) {
            uint32_t k = rk[4 * round + c];
            st[4 * c + 0] = (uint8_t)(tmp[4 * c + 0] ^ (k >> 24));
            st[4 * c + 1] = (uint8_t)(tmp[4 * c + 1] ^ (k >> 16));
            st[4 * c + 2] = (uint8_t)(tmp[4 * c + 2] ^ (k >> 8));
            st[4 * c + 3] = (uint8_t)(tmp[4 * c + 3] ^ k);
        }
        /* InvMixColumns */
        for (c = 0; c < 4; c++) {
            uint8_t a0 = st[4 * c + 0], a1 = st[4 * c + 1], a2 = st[4 * c + 2], a3 = st[4 * c + 3];
            tmp[4 * c + 0] = (uint8_t)(wfm_aes_gf_mul(a0, 14) ^ wfm_aes_gf_mul(a1, 11) ^ wfm_aes_gf_mul(a2, 13) ^
                                       wfm_aes_gf_mul(a3, 9));
            tmp[4 * c + 1] = (uint8_t)(wfm_aes_gf_mul(a0, 9) ^ wfm_aes_gf_mul(a1, 14) ^ wfm_aes_gf_mul(a2, 11) ^
                                       wfm_aes_gf_mul(a3, 13));
            tmp[4 * c + 2] = (uint8_t)(wfm_aes_gf_mul(a0, 13) ^ wfm_aes_gf_mul(a1, 9) ^ wfm_aes_gf_mul(a2, 14) ^
                                       wfm_aes_gf_mul(a3, 11));
            tmp[4 * c + 3] = (uint8_t)(wfm_aes_gf_mul(a0, 11) ^ wfm_aes_gf_mul(a1, 13) ^ wfm_aes_gf_mul(a2, 9) ^
                                       wfm_aes_gf_mul(a3, 14));
        }
        memcpy(st, tmp, WFM_AES_BLOCK);
    }

    /* Final round without InvMixColumns. */
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++)
            tmp[4 * c + r] = st[4 * ((c - r + 4) & 3) + r];
    }
    for (c = 0; c < WFM_AES_BLOCK; c++)
        tmp[c] = wfm_aes_inv_sbox[tmp[c]];
    for (c = 0; c < 4; c++) {
        uint32_t k = rk[c];
        out[4 * c + 0] = (uint8_t)(tmp[4 * c + 0] ^ (k >> 24));
        out[4 * c + 1] = (uint8_t)(tmp[4 * c + 1] ^ (k >> 16));
        out[4 * c + 2] = (uint8_t)(tmp[4 * c + 2] ^ (k >> 8));
        out[4 * c + 3] = (uint8_t)(tmp[4 * c + 3] ^ k);
    }
}

/**************************************************************************
 * mz_crypt_aes_* -- the streaming AES API declared by mz_crypt.h
 **************************************************************************/

void *mz_crypt_aes_create(void) {
    wfm_aes_t *aes = (wfm_aes_t *)calloc(1, sizeof(wfm_aes_t));
    if (aes)
        aes->mode = MZ_AES_MODE_ECB;
    return aes;
}

void mz_crypt_aes_delete(void **handle) {
    wfm_aes_t *aes = NULL;
    if (!handle)
        return;
    aes = (wfm_aes_t *)*handle;
    if (aes) {
        memset(aes, 0, sizeof(*aes));
        free(aes);
    }
    *handle = NULL;
}

void mz_crypt_aes_reset(void *handle) {
    wfm_aes_t *aes = (wfm_aes_t *)handle;
    if (!aes)
        return;
    /* Deliberately keeps the mode: mz_strm_wzaes.c resets and then sets the
       key, relying on ECB being the default. */
    memset(aes->iv, 0, sizeof(aes->iv));
    aes->keyed = 0;
}

void mz_crypt_aes_set_mode(void *handle, int32_t mode) {
    wfm_aes_t *aes = (wfm_aes_t *)handle;
    if (!aes)
        return;
    aes->mode = mode;
}

int32_t mz_crypt_aes_set_encrypt_key(void *handle, const void *key, int32_t key_length, const void *iv,
                                     int32_t iv_length) {
    wfm_aes_t *aes = (wfm_aes_t *)handle;

    if (!aes || !key)
        return MZ_PARAM_ERROR;
    if (key_length != 16 && key_length != 24 && key_length != 32)
        return MZ_PARAM_ERROR;
    if (iv_length != 0 && (iv_length != WFM_AES_BLOCK || !iv))
        return MZ_PARAM_ERROR;
    if (aes->mode != MZ_AES_MODE_ECB && aes->mode != MZ_AES_MODE_CBC)
        return MZ_SUPPORT_ERROR;

    wfm_aes_set_key(aes, (const uint8_t *)key, key_length);
    memset(aes->iv, 0, sizeof(aes->iv));
    if (iv_length == WFM_AES_BLOCK)
        memcpy(aes->iv, iv, WFM_AES_BLOCK);
    return MZ_OK;
}

int32_t mz_crypt_aes_set_decrypt_key(void *handle, const void *key, int32_t key_length, const void *iv,
                                     int32_t iv_length) {
    /* The key schedule is the encryption one in both directions; the inverse
       cipher uses it directly. */
    return mz_crypt_aes_set_encrypt_key(handle, key, key_length, iv, iv_length);
}

int32_t mz_crypt_aes_encrypt(void *handle, const void *aad, int32_t aad_size, uint8_t *buf, int32_t size) {
    wfm_aes_t *aes = (wfm_aes_t *)handle;
    int32_t done;

    if (!aes || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (aad_size != 0)
        return MZ_SUPPORT_ERROR; /* no AEAD modes are implemented */
    if (!aes->keyed)
        return MZ_PARAM_ERROR;
    if (size % WFM_AES_BLOCK != 0)
        return MZ_PARAM_ERROR;

    if (aes->mode == MZ_AES_MODE_ECB) {
        /* Every block is encrypted independently, which is exactly what
           mz_strm_wzaes.c wants for its counter. */
        for (done = 0; done < size; done += WFM_AES_BLOCK)
            wfm_aes_encrypt_block(aes, buf + done, buf + done);
        return MZ_OK;
    }

    for (done = 0; done < size; done += WFM_AES_BLOCK) {
        uint8_t block[WFM_AES_BLOCK];
        int32_t i;

        for (i = 0; i < WFM_AES_BLOCK; i++)
            block[i] = (uint8_t)(buf[done + i] ^ aes->iv[i]);
        wfm_aes_encrypt_block(aes, block, block);
        memcpy(aes->iv, block, WFM_AES_BLOCK);
        memcpy(buf + done, block, WFM_AES_BLOCK);
    }
    return MZ_OK;
}

int32_t mz_crypt_aes_decrypt(void *handle, const void *aad, int32_t aad_size, uint8_t *buf, int32_t size) {
    wfm_aes_t *aes = (wfm_aes_t *)handle;
    int32_t done;

    if (!aes || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (aad_size != 0)
        return MZ_SUPPORT_ERROR;
    if (!aes->keyed)
        return MZ_PARAM_ERROR;
    if (size % WFM_AES_BLOCK != 0)
        return MZ_PARAM_ERROR;

    if (aes->mode == MZ_AES_MODE_ECB) {
        for (done = 0; done < size; done += WFM_AES_BLOCK)
            wfm_aes_decrypt_block(aes, buf + done, buf + done);
        return MZ_OK;
    }

    for (done = 0; done < size; done += WFM_AES_BLOCK) {
        uint8_t ctext[WFM_AES_BLOCK];
        uint8_t ptext[WFM_AES_BLOCK];
        int32_t i;

        memcpy(ctext, buf + done, WFM_AES_BLOCK);
        wfm_aes_decrypt_block(aes, ctext, ptext);
        for (i = 0; i < WFM_AES_BLOCK; i++)
            ptext[i] ^= aes->iv[i];
        memcpy(aes->iv, ctext, WFM_AES_BLOCK);
        memcpy(buf + done, ptext, WFM_AES_BLOCK);
    }
    return MZ_OK;
}

int32_t mz_crypt_aes_encrypt_final(void *handle, uint8_t *buf, int32_t size, uint8_t *tag, int32_t tag_size) {
    if (!handle || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (tag && tag_size > 0)
        memset(tag, 0, (size_t)tag_size);
    /* ECB/CBC are block modes with no trailing state to flush. */
    return (size == 0) ? MZ_OK : MZ_PARAM_ERROR;
}

int32_t mz_crypt_aes_decrypt_final(void *handle, uint8_t *buf, int32_t size, const uint8_t *tag, int32_t tag_size) {
    if (!handle || !buf || size < 0)
        return MZ_PARAM_ERROR;
    if (tag && tag_size > 0)
        return MZ_SUPPORT_ERROR; /* no authentication is implemented */
    return (size == 0) ? MZ_OK : MZ_PARAM_ERROR;
}

/***************************************************************************/
