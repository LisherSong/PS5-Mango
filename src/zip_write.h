/* zip_write.h — streaming STORE-only ZIP writer (original implementation)
 *
 * Builds a ZIP archive one entry at a time with no compression (method 0),
 * using data-descriptor records so the CRC can be computed while the file is
 * being read. Pure lib style: no HTTP / MHD / device dependencies, compiles
 * and runs on the host build (used by the save-manager download/backup paths
 * and directly exercised by hosttest).
 */
#ifndef ZIP_WRITE_H
#define ZIP_WRITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sink abstraction: every emitted byte goes through write() (0 ok, <0 fail). */
typedef int (*zipw_sink_fn)(void *ctx, const void *buf, size_t len);
typedef struct {
  zipw_sink_fn write;
  void *ctx;
  uint64_t bytes; /* total bytes delivered so far */
} zipw_sink_t;

/* One archive entry. path = on-disk location, name = path inside the archive
 * (forward-slash separated, relative to the walked base). Both are heap
 * strings owned by the caller / by zipw_walk_dir's output array. */
typedef struct {
  char *path;
  char *name;
} zipw_file_t;

/* Recursively collect every regular file under `base` into *out / *out_n.
 * Skips dot files, symbolic links and anything that is not a regular file.
 * Archive names are paths relative to `base`. The recursion uses heap-allocated
 * path buffers (no large stack arrays — LTO-inlined recursion frames blow the
 * stack on the device). Returns 0 on success (caller must free each .path /
 * .name and the array itself) or -1 (e.g. limit reached, OOM). */
int zipw_walk_dir(const char *base, zipw_file_t **out, int *out_n);

/* Stream a set of files into the sink as one STORE ZIP (flag 0x08 data
 * descriptors, CRC computed while reading). iobuf is a caller-owned scratch
 * buffer (e.g. 256 KiB). Files that cannot be stat()ed/opened are emitted as
 * empty entries. Returns 0 or -1. */
int zipw_write_files(zipw_sink_t *sink, const zipw_file_t *files, int nfiles,
                     char *iobuf, size_t iobuf_sz);

/* Convenience: zipw_walk_dir + zipw_write_files + free. Returns 0 or -1. */
int zipw_write_tree(zipw_sink_t *sink, const char *base, char *iobuf,
                    size_t iobuf_sz);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_WRITE_H */
