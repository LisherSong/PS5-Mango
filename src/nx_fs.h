/* nx_fs.h - "everything Nexus creates on the console is 0777", in one place.
 *
 * The console is not a desktop. The files a game is made of must be
 * executable, and the homebrew we deploy into /data/homebrew is launched by
 * the system; a 0644 file is a game that does not start. Users hit this as
 * "上传完的文件是 0666" and cannot tell it apart from a transfer failure.
 *
 * Why not just pass 0777 to open()/mkdir(): the process umask still applies,
 * so with a umask of 022 a file requested as 0777 lands as 0755. The payload
 * evidently runs with umask 0 today (a plain fopen("wb") yields exactly 0666,
 * which is what was reported), but depending on another process's umask is not
 * a design. Every helper here chmods *after* creating - that is the only way
 * to be independent of the umask.
 *
 * All of it is best effort. FAT/exFAT (the external USB mounts) refuse chmod
 * outright; that is not a failure and must not fail an otherwise good upload
 * or extraction - the same policy as zip_extract.c's chmod_0777_fd().
 *
 * Header-only and static inline on purpose: every caller used to grow its own
 * copy of the two-argument-mkdir / no-<io.h>-chmod Windows shim, and each new
 * copy is a fresh chance to break the host build (gcc 16 defaults to gnu23,
 * where an implicit declaration is a hard error).
 */
#ifndef NX_FS_H
#define NX_FS_H

#include <errno.h>
#include <string.h>   /* strcmp: the filesystem-type check below */
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#include <io.h>       /* _chmod */
#elif !defined(__linux__)
/* statfs()/fstatfs() and the BSD f_fstypename field. Only this branch needs it:
 * glibc has no f_fstypename, and the host harness is Windows. */
#include <sys/mount.h>
#endif

/* The one and only creation mode. */
#define NX_CREATE_MODE 0777

/* Exposed so a host test can pin the intent: the chmod syscall itself is a
 * no-op in the Windows test harness, so the value is all it can check. */
static inline int
nx_create_mode(void) {
  return NX_CREATE_MODE;
}

/* ---------------------------------------------------------------------------
 * "Does this filesystem have Unix mode bits at all?"
 *
 * FAT/exFAT (the external USB mounts) and friends have none: a chmod against
 * them is refused or silently dropped. Asking first turns a pointless syscall
 * into a deliberate no-op, and it documents *why* every mode helper here is
 * best effort rather than merely ignoring failures.
 *
 * Pure string predicate, split out on purpose so the host can actually test it
 * - the statfs() query around it can only run on the console.
 */
static inline int
nx_fs_type_lacks_unix_modes(const char *type) {
  if(!type) return 0;
  return !strcmp(type, "exfat") || !strcmp(type, "exfatfs") ||
         !strcmp(type, "msdosfs") || !strcmp(type, "fat") ||
         !strcmp(type, "vfat");
}

/* A refused chmod is not automatically a failure: the filesystem may not
 * implement modes, or may be mounted read-only. Callers hand errno to the user,
 * so a refusal that means "there was nothing to do" must not become an error. */
static inline int
nx_chmod_error_is_benign(int err) {
  return err == ENOTSUP || err == EPERM || err == EINVAL || err == EROFS;
}

/* The query itself needs statfs()/fstatfs(). Off target we answer "yes" and let
 * the chmod be the arbiter: guessing "no" is the dangerous direction, because
 * it would silently skip real work on a filesystem that does honour modes. */
static inline int
nx_path_has_unix_modes(const char *path) {
#if defined(__linux__) || defined(_WIN32)
  (void)path;
  return 1;
#else
  struct statfs fs;
  if(!path || statfs(path, &fs) != 0) return 1;   /* cannot tell: assume yes */
  return !nx_fs_type_lacks_unix_modes(fs.f_fstypename);
#endif
}

static inline int
nx_fd_has_unix_modes(int fd) {
#if defined(__linux__) || defined(_WIN32)
  (void)fd;
  return 1;
#else
  struct statfs fs;
  if(fd < 0 || fstatfs(fd, &fs) != 0) return 1;   /* cannot tell: assume yes */
  return !nx_fs_type_lacks_unix_modes(fs.f_fstypename);
#endif
}

/* chmod `path` to NX_CREATE_MODE. Returns 0 on success. errno is restored on
 * the way out so a refused chmod cannot masquerade as the caller's real error
 * (the callers report errno to the user).
 *
 * Best effort by contract: a filesystem with no mode bits is skipped outright,
 * and a benign refusal (read-only mount, modes unsupported) counts as success.
 * Returns -1 only for a refusal the user could actually act on. */
static inline int
nx_chmod_create(const char *path) {
  int err = errno;
  int rc;

  if(!path || !*path) { errno = EINVAL; return -1; }
  if(!nx_path_has_unix_modes(path)) { errno = err; return 0; }
#ifdef _WIN32
  rc = _chmod(path, NX_CREATE_MODE);
#else
  rc = chmod(path, (mode_t)NX_CREATE_MODE);
#endif
  if(rc != 0 && nx_chmod_error_is_benign(errno)) rc = 0;
  errno = err;
  return rc;
}

/* fchmod(2) on an already-open descriptor.
 *
 * Why this exists next to nx_chmod_create(): the path-based call has a window.
 * Between open() and chmod() the file sits on disk with whatever the umask
 * allowed, and for an upload that window is the *entire* transfer - so an
 * interrupted upload leaves a 0666 game file behind, which is exactly what
 * "上传完的文件是 0666" looked like. fchmod() closes the window to microseconds.
 *
 * This is the single implementation point for fd modes; the two wrappers below
 * only supply the mode. Windows has no fchmod(), so the host harness gets a
 * no-op. That is not a shortcut: the host cannot verify modes at all, which is
 * why the final claim is a console item (see HANDOVER). Callers must still
 * create with NX_CREATE_MODE so the Windows path is not left 0644. */
static inline int
nx_fchmod(int fd, unsigned mode) {
  int err = errno;
  int rc  = 0;

  if(fd < 0) { errno = EINVAL; return -1; }
#ifdef _WIN32
  (void)fd; (void)mode;
#else
  if(nx_fd_has_unix_modes(fd)) {
    if(fchmod(fd, (mode_t)(mode & 07777)) != 0 && !nx_chmod_error_is_benign(errno))
      rc = -1;
  }
#endif
  errno = err;
  return rc;
}

static inline int
nx_fchmod_create(int fd) {
  return nx_fchmod(fd, NX_CREATE_MODE);
}

/* mkdir `path` and give it NX_CREATE_MODE.
 *
 * An existing directory is NOT an error and is deliberately left alone:
 * callers walk whole paths (e.g. /data/homebrew/<game>) and re-chmod-ing a
 * directory we did not create would be a rude surprise from a file manager.
 * Returns 0 if the directory exists once we are done. */
static inline int
nx_mkdir_create(const char *path) {
  int created;

  if(!path || !*path) { errno = EINVAL; return -1; }
#ifdef _WIN32
  created = (_mkdir(path) == 0);
#else
  created = (mkdir(path, (mode_t)NX_CREATE_MODE) == 0);
#endif
  if(!created) {
    if(errno == EEXIST) return 0;
    return -1;
  }
  (void)nx_chmod_create(path);   /* the umask may have shaved 0777 down */
  return 0;
}

#endif /* NX_FS_H */
