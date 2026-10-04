/* config.h -- hand-maintained configuration for vendored libnfs 8.0.0.
 *
 * Upstream normally generates this file with autotools/cmake. We cannot run
 * autotools on the SDK machine (no autoconf/automake/libtool in the payload
 * toolchain), so this is a hand-written minimal set modelled on
 * cmake/config.h.cmake, tuned for the PS5 payload sysroot (FreeBSD-based
 * libc).
 *
 * Rules of thumb applied here:
 *   * FreeBSD has every standard POSIX header listed below -> define 1.
 *   * HAVE_SYS_EVENTFD_H stays undefined: the PS5 sysroot may lack eventfd(),
 *     and lib/init.c already degrades gracefully (service-thread wakeups
 *     disabled, fallback path used) when it is missing.
 *   * HAVE_SYS_VFS_H (Linux-only) undefined; nfs_v3.c/nfs_v4.c fall back to
 *     the HAVE_SYS_STATVFS_H path, which FreeBSD has.
 *   * HAVE_NFS4_2 left undefined: NFSv4.1-only client, smaller surface.
 *   * Kerberos (HAVE_LIBKRB5), TLS (HAVE_TLS/HAVE_GNUTLS_*) and talloc/tevent
 *     are all undefined -- none are present in the sysroot and tls/ is not
 *     part of the vendored build. HAVE_MULTITHREADING is undefined on purpose:
 *     the worker is single-threaded and does not need the nfs_mt_* helpers.
 *   * No Windows -> EXTERN in nfsc/libnfs.h is empty, no winsock glue needed.
 *
 * If the SDK machine integration trips over a genuinely missing header, flip
 * the corresponding HAVE_* here (they are all just "does <x.h> exist" facts).
 */

/* Define to 1 if you have the <arpa/inet.h> header file. */
#define HAVE_ARPA_INET_H 1

/* Whether we have clock_gettime */
#define HAVE_CLOCK_GETTIME 1

/* Whether gnutls exports the function gnutls_transport_is_ktls_enabled() */
/* #undef HAVE_GNUTLS_TRANSPORT_IS_KTLS_ENABLED */

/* Whether pthread library is present */
#define HAVE_PTHREAD 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the <fuse.h> header file. */
/* #undef HAVE_FUSE_H */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the `nsl' library (-lnsl). */
/* #undef HAVE_LIBNSL */

/* Define to 1 if you have the `socket' library (-lsocket). */
/* #undef HAVE_LIBSOCKET */

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <netdb.h> header file. */
#define HAVE_NETDB_H 1

/* Define to 1 if you have the <netinet/in.h> header file. */
#define HAVE_NETINET_IN_H 1

/* Define to 1 if you have the <netinet/tcp.h> header file. */
#define HAVE_NETINET_TCP_H 1

/* Define to 1 if you have the <net/if.h> header file. */
#define HAVE_NET_IF_H 1

/* Define to 1 if you have the <poll.h> header file. */
#define HAVE_POLL_H 1

/* Define to 1 if you have the <pwd.h> header file. */
#define HAVE_PWD_H 1

/* Whether sockaddr struct has sa_len */
#define HAVE_SOCKADDR_LEN 1

/* Whether we have sockaddr_storage */
#define HAVE_SOCKADDR_STORAGE 1

/* Whether our sockets support SO_BINDTODEVICE */
/* #undef HAVE_SO_BINDTODEVICE */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <stdatomic.h> header file. */
#define HAVE_STDATOMIC_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if `st_mtim.tv_nsec' is a member of `struct stat'. */
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1

/* Define to 1 if you have the <sys/filio.h> header file. */
/* #undef HAVE_SYS_FILIO_H */

/* Define to 1 if you have the <sys/ioctl.h> header file. */
#define HAVE_SYS_IOCTL_H 1

/* Define to 1 if you have the <sys/socket.h> header file. */
/* #undef HAVE_SYS_EVENTFD_H */
#define HAVE_SYS_SOCKET_H 1

/* Define to 1 if you have the <sys/sockio.h> header file. */
#define HAVE_SYS_SOCKIO_H 1

/* Define to 1 if you have the <sys/statvfs.h> header file. */
#define HAVE_SYS_STATVFS_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/sysmacros.h> header file. */
/* #undef HAVE_SYS_SYSMACROS_H */

/* Define to 1 if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/uio.h> header file. */
#define HAVE_SYS_UIO_H 1

/* Define to 1 if you have the <sys/vfs.h> header file. */
/* #undef HAVE_SYS_VFS_H */

/* Whether we have talloc and tevent support */
/* #undef HAVE_TALLOC_TEVENT */

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the <utime.h> header file. */
#define HAVE_UTIME_H 1

/* Define to 1 if you have the <signal.h> header file. */
#define HAVE_SIGNAL_H 1

/* Define to 1 if you have the <sys/utsname.h> header file. */
#define HAVE_SYS_UTSNAME_H 1

/* NFSv4.2 is left off: NFSv4.1 client only. */
/* #undef HAVE_NFS4_2 */

/* Single-threaded worker: the nfs_mt_* helpers are not needed. */
/* #undef HAVE_MULTITHREADING */

/* Define to 1 if pthread_threadid_np() exists. */
/* #undef HAVE_PTHREAD_THREADID_NP */

/* Define to 1 if all of the C90 standard headers exist (not just the ones
   required in a freestanding environment). */
#define STDC_HEADERS 1

/* Name of package */
#define PACKAGE "libnfs"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "sahlberg@ronnie"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libnfs"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libnfs 8.0.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libnfs"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "8.0.0"

/* Version number of package */
#define VERSION "8.0.0"

/* ========================================================================
 * Host-build overrides (appended by this repo, 2026-10-01).
 *
 * Everything above describes the PS5 payload sysroot, which is FreeBSD-based
 * -- that is the target this config.h was written for. The project also builds
 * a Linux host binary (`make linux`) so the transfer layer can be compiled and
 * regression-tested without a console, and two of the FreeBSD facts above are
 * simply false there. They are not cosmetic: both change struct layout or
 * include a header that does not exist, so the symptom would be a compile
 * error at best and a mis-serialised sockaddr at worst.
 *
 * Keep this block LAST: it must be able to undo the defines above.
 * ======================================================================== */
#if defined(__linux__)
/* Linux has no <sys/sockio.h>; socket options come from <sys/ioctl.h>. */
#undef HAVE_SYS_SOCKIO_H
/* glibc >= 2.28 no longer leaks major()/minor() in from <sys/types.h>, and
 * nfs_v3.c / nfs_v4.c only include <sys/sysmacros.h> when this is defined --
 * without it the link fails on undefined `major` / `minor`. FreeBSD has both
 * macros in <sys/types.h> and ships no <sys/sysmacros.h>, which is why the
 * line above is an #undef: right for the console, wrong for the host build. */
#define HAVE_SYS_SYSMACROS_H 1
/* struct sockaddr on Linux has NO sa_len/sin_len field. Defining this makes
 * libnfs write to offsetof(...)+0 of a sockaddr_in, i.e. sin_family. */
#undef HAVE_SOCKADDR_LEN
#endif
