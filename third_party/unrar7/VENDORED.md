# Vendored: unrar 7.20.1 (RARLab UnRAR source)

Replaces `dmc_unrar` (see git history under `third_party/unrar/`) as the RAR
engine. This is the official Alexander Roshal UnRAR source, packaged on
GitHub by opello for easier vendoring.

## Source
- Upstream: https://github.com/opello/unrar — commit `97e1780310`
  ("v7.20.1: Extracted from https://www.rarlab.com/rar/unrarsrc-...", 2025-10-30)
- Version: 7.20.1 (see `version.hpp`: RARVER_MAJOR 7, RARVER_MINOR 20, BETA 1,
  built 2025-10-28)
- Mirror of RARLab https://www.rarlab.com/rar_add.htm — this directory is a
  verbatim copy of the upstream tree (159 files), no modifications except the
  additions listed below.

## What this enables that dmc_unrar could not
- **RAR5 compression version 6** (WinRAR 6.x / 7.x archives, "v6:" algorithm
  string). dmc_unrar 1.7.0 only dispatched `0x5000` (v5) in
  `dmc_unrar_file_unpack`, so any WinRAR 6+ archive hit
  `DMC_UNRAR_FILE_UNSUPPORTED_VERSION` and surfaced as "corrupt archive".
- **Encrypted RAR** (headers and/or entries) — password via `RARSetPassword`.
- **Multi-volume RAR** (`.partNN.rar` chains) — via open-mode + volume
  callback handling.
- RAR4 (all sub-versions) continues to work.

## Build model
Compile the upstream DLL source set as a **static library**, define
`-DRARDLL` (suppresses `rar.cpp`'s `main()` via `#if !defined(RARDLL)`).

Source set (49 .cpp, matching `UnRARDll.vcxproj` ClCompile list):
archive arcread blake2s cmddata consio crc crypt dll encname errhnd extinfo
extract filcreat file filefn filestr find getbits global hash headers isnt
largepage match motw options pathfn qopen rar rarpch rarvm rawread rdwrfn
rijndael rs rs16 scantree secpassword sha1 sha256 smallfn strfn strlist
system threadpool timefn ui unicode unpack volume

Compile flags:
- `-std=c++17 -DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE`
- PS5 (`prospero-clang++`): the toolchain defaults to `-stdlib=libc++`
  (FreeBSD-style sysroot; there is no GNU libstdc++). No explicit `-lc++`
  needed. `_UNIX` is defined by the platform headers.
- Host tests (Windows/MinGW `g++`): libstdc++ default is fine.
- Windows link needs `-lpowrprof` (SystemSuspend in `system.cpp`);
  `SetSuspendState` is referenced only on `_WIN_ALL`.

The upstream C API (`dll.hpp`) is C-compatible (extern "C" + plain C
structs). C translation units include `unrar_c_api.h` (platform shim +
re-export of the entry points used by `src/rar_extract.c`), never the unrar
C++ headers.

## Additions in this directory (not upstream)
- `unrar_c_api.h` — WFM facade shim (see file header).
- `VENDORED.md` — this file.

## License
UnRAR freeware license (`license.txt`): free to use in any software to
handle RAR archives; may NOT be used to build a RAR-compatible *archiver* or
re-create the RAR compression algorithm. Full text must stay with the
source. This replaces the GPL-2.0 dmc_unrar notice in THIRD_PARTY_NOTICES.
