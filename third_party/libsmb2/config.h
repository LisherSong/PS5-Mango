/* config.h -- hand-maintained configuration for vendored libsmb2 v6.0.0.
 *
 * Upstream normally generates this file with autotools/cmake. We cannot run
 * autotools on the SDK machine (no autoconf/automake/libtool in the payload
 * toolchain), so this is a hand-written minimal set modelled on the reference
 * instance in include/apple/config.h, tuned for the PS5 payload sysroot
 * (FreeBSD-based libc).
 *
 * Rules of thumb applied here:
 *   * FreeBSD has every standard POSIX header listed below -> define 1.
 *   * Kerberos/GSSAPI are NOT available in the sysroot -> both HAVE_LIBKRB5
 *     and HAVE_GSSAPI_GSSAPI_H stay undefined; lib/krb5-wrapper.c and
 *     lib/spnego-wrapper.c then compile their non-krb5 fallbacks and NTLMSSP
 *     (the path we use against home NAS boxes) still works.
 *   * No Windows -> none of the NEED_* (NEED_POLL/NEED_READV/...) macros that
 *     lib/compat.c keys off are defined, so compat.c compiles to nothing.
 *
 * If the SDK machine integration trips over a genuinely missing header, flip
 * the corresponding HAVE_* here (they are all just "does <x.h> exist" facts).
 */

/* Whether or not TCP sockets should be allowed to linger after closure */
#define CONFIGURE_OPTION_TCP_LINGER 1

/* Define to 1 if you have the <arpa/inet.h> header file. */
#define HAVE_ARPA_INET_H 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the <errno.h> header file. */
#define HAVE_ERRNO_H 1

/* Define to 1 if you have the <fcntl.h> header file. */
#define HAVE_FCNTL_H 1

/* Define to 1 if you have the <gssapi/gssapi.h> header file. */
/* #undef HAVE_GSSAPI_GSSAPI_H */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Whether we use gssapi_krb5 or not */
/* #undef HAVE_LIBKRB5 */

/* Define to 1 if you have the `nsl' library (-lnsl). */
/* #undef HAVE_LIBNSL */

/* Define to 1 if you have the `socket' library (-lsocket). */
/* #undef HAVE_LIBSOCKET */

/* Whether we have linger */
#define HAVE_LINGER 1

/* Define to 1 if you have the <netdb.h> header file. */
#define HAVE_NETDB_H 1

/* Define to 1 if you have the <netinet/in.h> header file. */
#define HAVE_NETINET_IN_H 1

/* Define to 1 if you have the <netinet/tcp.h> header file. */
#define HAVE_NETINET_TCP_H 1

/* Define to 1 if you have the <poll.h> header file. */
#define HAVE_POLL_H 1

/* Whether sockaddr struct has sa_len */
#define HAVE_SOCKADDR_LEN 1

/* Whether we have sockaddr_storage */
#define HAVE_SOCKADDR_STORAGE 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/errno.h> header file. */
#define HAVE_SYS_ERRNO_H 1

/* Define to 1 if you have the <sys/fcntl.h> header file. */
#define HAVE_SYS_FCNTL_H 1

/* Define to 1 if you have the <sys/ioctl.h> header file. */
#define HAVE_SYS_IOCTL_H 1

/* Define to 1 if you have the <sys/poll.h> header file. */
#define HAVE_SYS_POLL_H 1

/* Define to 1 if you have the <sys/socket.h> header file. */
#define HAVE_SYS_SOCKET_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/uio.h> header file. */
#define HAVE_SYS_UIO_H 1

/* Define to 1 if you have the <sys/unistd.h> header file. */
#define HAVE_SYS_UNISTD_H 1

/* Define to 1 if you have the <sys/_iovec.h> header file. */
/* #undef HAVE_SYS__IOVEC_H */

/* Define to 1 if you have the <time.h> header file. */
#define HAVE_TIME_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "libsmb2"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "ronniesahlberg@gmail.com"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libsmb2"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libsmb2 6.0.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libsmb2"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "6.0.0"

/* Define to 1 if all of the C90 standard headers exist (not just the ones
   required in a freestanding environment). This macro is provided for
   backward compatibility; new code need not use it. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "6.0.0"
