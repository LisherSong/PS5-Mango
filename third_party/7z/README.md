# 7z decoder sources (LZMA SDK)

Vendored subset of the **LZMA SDK 26.03** (2026-09-03) by Igor Pavlov.
Public domain — see `DOC/lzma-sdk.txt`:

> LZMA SDK is written and placed in the public domain by Igor Pavlov.

Upstream: <https://www.7-zip.org/sdk.html> — release archive
`lzma2603.7z` from <https://github.com/ip7z/7zip/releases>.

These files back the 7z branch of the extraction engine (`src/sevenz_*.c`).
The SDK ships both a C and a C++ implementation; only the **C** one is used
here, because it builds with the plain `prospero-clang` C toolchain on PS5 and
has no dependency on the C++ runtime.

## What is vendored

| Group | Files |
|---|---|
| container | `7z.h`, `7zArcIn.c`, `7zDec.c` |
| streams / infra | `7zTypes.h`, `7zStream.c`, `7zFile.c`, `7zAlloc.c`, `7zBuf.c`, `7zBuf2.c`, `Alloc.c` |
| checksums | `7zCrc.c`, `7zCrcOpt.c`, `SwapBytes.c` |
| CPU / portability | `CpuArch.c`, `Compiler.h`, `Precomp.h`, `RotateDefs.h`, `7zWindows.h` |
| codecs | `LzmaDec.c`, `Lzma2Dec.c`, `Ppmd7.c`, `Ppmd7Dec.c`, `Ppmd.h`, `Bcj2.c`, `Bra.c`, `Bra86.c`, `BraIA64.c`, `Delta.c` |
| crypto | `Aes.c`, `AesOpt.c`, `Sha256.c`, `Sha256Opt.c` |
| misc | `DllSecur.c` (only referenced by the Windows path of `7zFile.c`), `7zVersion.h` |

The encoder half of the SDK (`LzmaEnc.c`, `Lzma2Enc.c`, `Xz*.c`, `Sort.c`,
`Threads.c`, `Mt*.c`, `Ppmd*Enc.c`, …) is deliberately **not** vendored: the
engine only ever decodes.

Build flags that matter:

* `-DZ7_PPMD_SUPPORT` — without it `7zDec.c` drops the PPMd coder entirely.
* `-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE`
* `-w` — the sources are not expected to be warning clean.

## Known SDK limitations the engine has to work around

Both were confirmed empirically against real 7-Zip 26.03 output, see
`tests/sevenz_e2e.c` and `tests/make_sevenz_fixtures.py`.

### 1. `CSzFolder` caps a folder at 4 coders / 3 bonds

`7z.h` defines `SZ_NUM_CODERS_IN_FOLDER_MAX 4` and
`SZ_NUM_BONDS_IN_FOLDER_MAX 3`, and `SzGetNextFolderItem()` refuses anything
larger. But the chain 7-Zip produces for `-m0=bcj2` is **BCJ2 + 4×LZMA2 = 5
coders / 4 bonds**, so `SzAr_DecodeFolder()` returns `SZ_ERROR_UNSUPPORTED` on
an archive that 7-Zip itself created without complaint.

Note this only affects *decoding*: `SzArEx_Open()` parses folder blobs with a
separate, far more permissive scanner (`k_Scan_NumCoders_MAX 64`, in
`7zArcIn.c`), so the file list and all unpack sizes are still correct — the
failure shows up per-entry, at extract time.

Consequence: the engine parses folder blobs with its own dynamic parser and
drives the codec chain itself instead of calling `SzAr_DecodeFolder()`.

### 2. There is no 7zAES coder in the C decoder

`IS_SUPPORTED_CODER()` in `7zDec.c` accepts only `Copy`, `LZMA`, `LZMA2`,
`PPMd` and the branch/delta/BCJ2 filters. `Aes.c` is present but never wired
up, so both `-p<password>` archives and `-mhe=on` (encrypted header) archives
are rejected.

Consequence: the engine implements the 7zAES coder itself on top of the
vendored `Aes.c` / `Sha256.c`. The reference implementation is in the SDK's
C++ tree at `CPP/7zip/Crypto/7zAes.cpp` (not vendored, kept in the SDK tarball):

* properties: `b0 & 0x3F` = numCyclesPower, `b0 bit7` and `b1>>4` add to the
  salt size, `b0 bit6` and `b1 & 0x0F` add to the IV size
* key: `SHA-256(salt || password_utf16le || counter_le64)` iterated
  `1 << numCyclesPower` times, unless `numCyclesPower == 0x3F` in which case
  the key is `salt || password` truncated/zero-padded to 32 bytes
* then AES-256-CBC with that key and the (zero-padded) IV

## Regenerating this directory

```sh
curl -L -o lzma2603.7z https://github.com/ip7z/7zip/releases/download/26.03/lzma2603.7z
7zr x lzma2603.7z -osdk
# copy the files listed above out of sdk/C/
```

`tests/run-sevenz-tests.sh` builds this subset and runs the fixture matrix.
