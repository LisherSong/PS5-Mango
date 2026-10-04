/* mz_config.h -- hand maintained configuration for the vendored minizip-ng.
   part of the minizip-ng project

   This file replaces the CMake generated mz_config.h. The vendored copy only
   builds the decompression path: HAVE_ZLIB is defined by the project Makefile
   and no other compression or crypto backend is enabled.
*/

#ifndef MZ_CONFIG_H
#define MZ_CONFIG_H

/* Define to 1 if you have the <dirent.h> header file. */
#define HAVE_DIRENT_H 1

/* Define to 1 if you have the <sys/dirent.h> header file. */
#define HAVE_SYS_DIRENT_H 0

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if DIR* is defined. */
#define HAVE_PDIR 1

/* Define to 1 if fseeko() is defined. */
#define HAVE_FSEEKO 1

/* Define to 1 if symlink() is defined. */
#define HAVE_SYMLINK 1

/* Define to 1 if readlink() is defined. */
#define HAVE_READLINK 1

#endif
