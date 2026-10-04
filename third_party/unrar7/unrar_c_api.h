/* WFM facade over the vendored unrar 7.x library (third_party/unrar7).
 *
 * The upstream code ships its own C-compatible API header (dll.hpp: all
 * structs are plain C, functions are extern "C"), so this facade is only a
 * thin platform shim: it makes HANDLE / PASCAL / LONG available before
 * pulling in dll.hpp, then re-exports the handful of entry points the RAR
 * engine (src/rar_extract.c) actually needs.
 *
 *   - PS5 / POSIX builds:   dll.hpp defines the Win32-ish types itself when
 *                           _UNIX is defined (see dll.hpp:#ifdef _UNIX).
 *   - Windows host builds:  <windows.h> provides HANDLE / PASCAL.
 *
 * Do NOT include the unrar C++ headers (rar.hpp etc.) from C translation
 * units; link against the static library built from the RARDLL source set
 * (see third_party/unrar7/VENDORED.md) instead. */
#ifndef WFM_UNRAR7_C_API_H
#define WFM_UNRAR7_C_API_H

#if defined(_WIN32) && !defined(_UNIX)
#include <windows.h>
#else
/* POSIX / PS5 builds: mirror dll.hpp's _UNIX type shims so the header is
   self-contained when included from a C translation unit (the PS5 SDK does
   not define _UNIX for project sources; unrar defines it internally via
   raros.hpp only when compiling its own C++ files). */
#define CALLBACK
#define PASCAL
#define LONG long
#define HANDLE void *
#define LPARAM long
#define UINT unsigned int
#endif

#include <stddef.h> /* wchar_t for C99 translation units */
#include "dll.hpp"

/* Re-export the exact API surface rar_extract.c consumes, so the facade
 * doubles as documentation of the dependency. */
#define WFM_RAR_OPEN_EX   RAROpenArchiveEx
#define WFM_RAR_CLOSE     RARCloseArchive
#define WFM_RAR_READ_HDR  RARReadHeaderEx
#define WFM_RAR_PROCESS   RARProcessFileW
#define WFM_RAR_PASSWORD  RARSetPassword
#define WFM_RAR_DLL_VER   RARGetDllVersion

#endif /* WFM_UNRAR7_C_API_H */
