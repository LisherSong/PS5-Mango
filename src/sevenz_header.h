/* sevenz_header -- reads the 7z header, decrypting it when it is encrypted.
   part of ps5-web-file-manager

   A 7z archive keeps its header at the END of the file, and when that header
   grows past a threshold 7-Zip stores it *compressed*: the next-header region
   then starts with a `k7zIdEncodedHeader` (0x17) record describing a single
   folder whose output is the real header.  That folder is one of two things:

     * LZMA / LZMA2 -- `-mhc=on`, the default.  The vendored SDK decodes it
       itself, so this module reads a few bytes, sees no AES coder and steps
       aside without changing anything.
     * LZMA + 7zAES -- `-mhe=on`.  The C half of the SDK has no 7zAES coder at
       all, so SzArEx_Open() gives up with SZ_ERROR_UNSUPPORTED before a single
       folder is known: the archive cannot even be listed.

   The second case is what this module exists for.  It decodes that one folder
   with src/sevenz_chain.c -- the same decoder the archive's content goes
   through -- and then hands the SDK a stream in which the encoded header has
   been replaced by its plaintext.  The plaintext is longer than the record it
   replaces, so the stream is a small virtual view over the real one:

       [0, 32)                  the start header, rewritten to describe the
                                plaintext (offset, size and CRC)
       [32, hdr_off)            the real archive: packed streams
       [hdr_off, hdr_off + L)   the decrypted header
       beyond that              the real archive again

   `hdr_off` is where the encoded header already lived, so no offset that the
   archive itself stores has to move: the SDK reads the plaintext at exactly
   the position it expected the encoded record, and the main data position it
   derives from the plaintext still points at the real packed streams.

   Nothing on disk is touched, the archive is opened read-only, and an archive
   whose header is not encrypted is never touched at all.
*/

#ifndef SEVENZ_HEADER_H
#define SEVENZ_HEADER_H

#include <stddef.h>

#include "7zTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The plaintext header is tiny in every real archive (kilobytes); the ceiling
   only exists so a hostile encoded header cannot ask for a gigabyte. */
#define SZH_MAX_HEADER ((uint64_t)64 * 1024 * 1024)

typedef enum {
  /* The header is readable as it stands.  Use the stream you passed in and
     let the SDK parse it, exactly as before this module existed. */
  SZH_PLAIN = 0,

  /* The header was encrypted and is now decrypted: hand szh_stream() to
     SzArEx_Open() instead of the raw stream. */
  SZH_PATCHED,

  /* The header is encrypted and the password given was missing or wrong.
     Actionable: the caller should ask for one and retry. */
  SZH_ERR_PASSWORD,

  /* The encoded header uses an arrangement this module does not read. */
  SZH_ERR_UNSUPPORTED,

  /* The encoded header is malformed. */
  SZH_ERR_FORMAT,

  /* Reading the archive failed. */
  SZH_ERR_IO
} szh_status_t;

typedef struct szh_prep szh_prep;

/* Inspects the archive header behind `raw`.

   On SZH_PLAIN *out is NULL and the caller proceeds with `raw` untouched.
   On SZH_PATCHED *out owns everything and szh_stream(*out) must be used.
   Otherwise *out is NULL and `msg` says why, in a form meant for the user.

   `password` is the archive password as UTF-8, or NULL / "" when the caller
   has none.  It is only consulted when the header turns out to be encrypted.

   The function never reports an error for an archive the SDK would diagnose
   better: anything unexpected *before* an AES coder is found -- a short file,
   a bad signature, a header CRC mismatch, an unparsable StreamsInfo -- comes
   back as SZH_PLAIN so the SDK keeps producing the message it always did. */
szh_status_t szh_prepare(szh_prep **out, ISeekInStream *raw, const char *password,
                         char *msg, size_t msg_size);

/* The stream to give SzArEx_Open(); NULL when p is NULL.  Valid until
   szh_prep_free(). */
ISeekInStream *szh_stream(const szh_prep *p);

/* Static description of a status, for messages that have no better text. */
const char *szh_status_string(szh_status_t status);

void szh_prep_free(szh_prep *p);

#ifdef __cplusplus
}
#endif

#endif /* SEVENZ_HEADER_H */
