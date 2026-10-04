/* zipx_volume -- group a multi-file archive volume set into an ordered list.
   part of ps5-web-file-manager

   Supports the naming conventions seen in the wild:

     name.zip.001, name.zip.002, ...   byte split (7-Zip "split to volumes")
     name.part1.zip, name.part2.zip    byte split (WinRAR zip volumes)
     name.z01, name.z02, ..., name.zip zip split disks (Info-ZIP / PKZIP)

   A caller can hand in any member of the set (the user usually clicks one file
   in the browser) and gets back the full ordered list plus the split layout so
   the engine can pick the matching stream behaviour. */

#ifndef ZIPX_VOLUME_H
#define ZIPX_VOLUME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIPX_VOL_MAX_PARTS 512

/* How the set is split. A byte split (`name.zip.001`, `name.7z.001`,
   `name.part1.zip`) is the concatenation of its parts with absolute offsets;
   zip split disks (`name.z01` + `name.zip`) store per-disk offsets instead.
   The constants live here because they describe the *set*, and every consumer
   of zipx_volume_t needs them. */
#define ZIPX_VOL_MODE_CONCAT 0
#define ZIPX_VOL_MODE_DISK   1

typedef struct {
  char **paths;   /* ordered part paths, owned by this struct */
  int count;
  int index;      /* position of the path that was handed in (-1 unknown) */
  int mode;       /* ZIPX_VOL_MODE_CONCAT or ZIPX_VOL_MODE_DISK */
  int is_set;     /* 1 when the path is part of a multi-file set */
} zipx_volume_t;

/* Inspects `path`: 1 when it belongs to a multi-file set (out is filled),
   0 when it is an ordinary single file (out is cleared), -1 on a hard error
   (*err, when non-NULL, receives a malloc'd message the caller must free;
   it is also set for the 0 case when a sibling set looks broken, so callers
   can surface "volumes are incomplete" instead of a generic open failure). */
int zipx_volume_detect(const char *path, zipx_volume_t *out, char **err);

void zipx_volume_free(zipx_volume_t *vol);

/* True when `path` looks like the first volume of a set ("x.zip.001",
   "x.z01", "x.part1.zip"), used by the UI to label the entry. */
int zipx_volume_is_first(const char *path);

#ifdef __cplusplus
}
#endif

#endif
