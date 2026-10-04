/* extract_engine.c — transport-free adapter onto the inherited archive leaves
 * (zip_extract / rar_extract / sevenz_extract).
 *
 * This file exists so the worker never learns what a zipx_status_t is: the
 * worker depends on nexus_extract() and nothing else. When a fourth format is
 * added the change stops here.
 *
 * The dispatch rules are a straight port of the old MHD endpoint's
 * extract_dispatch(), because those rules were proven on the device (which
 * volume naming unrar actually accepts, and which message a user needs to see).
 */
#include "extract_engine.h"

#include "zip_extract.h"
#include "zipx_volume.h"
#include "rar_extract.h"
#include "sevenz_extract.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- name matching (MinGW has no strcasecmp for our purposes under -Wall) --- */

static int
ends_with_ci(const char *path, const char *suffix) {
  size_t plen = strlen(path), slen = strlen(suffix);
  size_t i;

  if(slen > plen) return 0;
  for(i = 0; i < slen; i++) {
    if(tolower((unsigned char)path[plen - slen + i]) !=
       tolower((unsigned char)suffix[i]))
      return 0;
  }
  return 1;
}

/* Case-insensitive substring search (strcasestr is not available on MinGW). */
static const char *
ci_strstr(const char *hay, const char *needle) {
  size_t nlen = strlen(needle);
  const char *p;

  if(!nlen) return hay;
  for(p = hay; *p; p++) {
    size_t i;
    for(i = 0; i < nlen; i++) {
      if(!p[i] ||
         tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i]))
        break;
    }
    if(i == nlen) return p;
  }
  return NULL;
}

/* Which engine a volume set belongs to, decided from the member names:
   0 zip, 1 rar, 2 7z, -1 unknown. */
static int
volume_format(const zipx_volume_t *vol) {
  static const char *const exts[] = { ".zip", ".rar", ".7z", NULL };
  const char *best = NULL;
  int best_kind = -1;
  int i, j;

  for(i = 0; i < vol->count; i++) {
    for(j = 0; exts[j]; j++) {
      const char *hit = ci_strstr(vol->paths[i], exts[j]);
      if(hit && (!best || hit > best)) { best = hit; best_kind = j; }
    }
  }
  return best_kind;
}

/* --- status mapping ------------------------------------------------------- */

static nexus_err_t
map_status(zipx_status_t s) {
  switch(s) {
  case ZIPX_OK:              return NEXUS_OK;
  case ZIPX_ERR_CANCELED:    return NEXUS_ERR_CANCELLED;
  case ZIPX_ERR_PASSWORD:    return NEXUS_ERR_PASSWORD;
  case ZIPX_ERR_CONFLICT:    return NEXUS_ERR_CONFLICT;
  case ZIPX_ERR_UNSUPPORTED: return NEXUS_ERR_UNSUPPORTED;
  case ZIPX_ERR_OPEN:
  case ZIPX_ERR_FORMAT:      return NEXUS_ERR_NOTFOUND;
  default:                   return NEXUS_ERR_GENERIC;
  }
}

/* --- engine glue --------------------------------------------------------- */

typedef struct {
  const nexus_extract_req_t *req;
} glue_t;

static int
glue_cancel(void *userdata) {
  const glue_t *g = (const glue_t *)userdata;
  return g->req->cancel ? g->req->cancel(g->req->ctx) : 0;
}

static void
glue_progress(void *userdata, const zipx_progress_t *p) {
  const glue_t *g = (const glue_t *)userdata;
  if(g->req->progress)
    g->req->progress(p->phase, p->bytes_done, p->bytes_total, p->current,
                     g->req->ctx);
}

static zipx_status_t
run_engine(int kind, const char *path, const char *dst,
           const nexus_extract_req_t *req, zipx_result_t *res, glue_t *g) {
  const char *pw = (req->password && req->password[0]) ? req->password : NULL;
  zipx_conflict_t conflict = (zipx_conflict_t)req->conflict;
  const zipx_limits_t *limits =
    zipx_limits_profile(req->large ? ZIPX_LIMITS_LARGE : ZIPX_LIMITS_DEFAULT);

  switch(kind) {
  case 0:
    return zipx_extract(path, dst, conflict, limits, glue_cancel, glue_progress,
                        g, pw, res);
  case 1:
    return rar_extract(path, dst, conflict, limits, glue_cancel, glue_progress,
                       g, pw, res);
  case 2:
    return sevenz_extract(path, dst, conflict, limits, glue_cancel,
                          glue_progress, g, pw, res);
  default:
    return ZIPX_ERR_UNSUPPORTED;
  }
}

/* --- stale staging sweep ---------------------------------------------------
 * The engines stage into `.wfm-extract-<pid>-<stamp>-<n>` inside the
 * destination's PARENT and publish by renaming; they always clean the directory
 * up on the way out. A payload that is killed mid-extraction — a redeploy, a
 * console reboot, the process replaced by a newer build — leaves the directory
 * behind with nobody left to remove it, and the user just sees a mysterious
 * dot-folder full of half-extracted files sitting next to the archive with no
 * explanation. That folder can never be published by a later run (its name
 * carries the dead process's pid), so it is pure garbage.
 *
 * A fresh extract into the same parent is the one moment we know that parent is
 * in play, so that is where orphans are collected. The age gate is what keeps
 * this safe: a concurrently running extraction owns a directory whose mtime is
 * seconds old, so only entries older than an hour are touched.
 *
 * The engines each carry a private path_parent(); this file cannot see those,
 * hence the small local copy. */
#define NEXUS_STAGING_PREFIX ".wfm-extract-"
#define NEXUS_STAGING_TTL_SEC (60 * 60)

static int
split_parent(const char *path, char *out, size_t out_size) {
  const char *slash;

  if(!path || path[0] != '/') return -1;
  slash = strrchr(path, '/');
  if(slash == path) {
    if(out_size < 2) return -1;
    out[0] = '/';
    out[1] = 0;
    return 0;
  }
  if((size_t)(slash - path) >= out_size) return -1;
  memcpy(out, path, (size_t)(slash - path));
  out[slash - path] = 0;
  return 0;
}

static void
remove_tree_quiet(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;

  if(d) {
    while((e = readdir(d))) {
      char child[PATH_MAX];
      struct stat st;

      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if((size_t)snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
         sizeof(child)) continue;
      if(!lstat(child, &st) && S_ISDIR(st.st_mode)) remove_tree_quiet(child);
      else unlink(child);
    }
    closedir(d);
  }
  rmdir(path);
}

static void
sweep_stale_staging(const char *dst_dir) {
  char parent[PATH_MAX];
  DIR *d;
  struct dirent *e;
  time_t now = time(NULL);

  if(!dst_dir || !dst_dir[0]) return;
  if(split_parent(dst_dir, parent, sizeof(parent))) return;
  if(!(d = opendir(parent))) return;
  while((e = readdir(d))) {
    char full[PATH_MAX];
    struct stat st;

    if(strncmp(e->d_name, NEXUS_STAGING_PREFIX,
               strlen(NEXUS_STAGING_PREFIX))) continue;
    if((size_t)snprintf(full, sizeof(full), "%s/%s", parent, e->d_name) >=
       sizeof(full)) continue;
    if(lstat(full, &st) || !S_ISDIR(st.st_mode)) continue;
    if(now - st.st_mtime < NEXUS_STAGING_TTL_SEC) continue;
    remove_tree_quiet(full);
  }
  closedir(d);
}

/* --- entry point --------------------------------------------------------- */

nexus_err_t
nexus_extract(const char *archive_path, const char *dst_dir,
              const nexus_extract_req_t *req,
              nexus_extract_result_t *out) {
  nexus_extract_req_t  none;
  zipx_result_t        res;
  zipx_volume_t        vol;
  zipx_status_t        st;
  char                *vol_err = NULL;
  glue_t               g;
  int                  vrc, kind;
  nexus_err_t          e;

  if(!archive_path || !dst_dir) return NEXUS_ERR_GENERIC;
  if(!req) {
    memset(&none, 0, sizeof(none));
    req = &none;
  }
  memset(&res, 0, sizeof(res));
  g.req = req;

  /* Collect any orphaned staging directory from a previous run that was killed
   * before it could publish. Cheap (one opendir of this destination's parent)
   * and the only point at which we know the parent is about to be used. */
  if(dst_dir && dst_dir[0]) sweep_stale_staging(dst_dir);

  vrc = zipx_volume_detect(archive_path, &vol, &vol_err);
  kind = vrc > 0 ? volume_format(&vol) : -1;

  if(vrc < 0) {
    /* A broken set gets the precise reason (which volume is missing …)
     * instead of a generic "unsupported format". */
    st = ZIPX_ERR_OPEN;
    if(vol_err) {
      snprintf(res.message, sizeof(res.message), "%s", vol_err);
      free(vol_err);
    } else {
      snprintf(res.message, sizeof(res.message),
               "the archive volumes are incomplete");
    }
  } else if(vrc > 0) {
    free(vol_err);
    if(kind < 0) {
      st = ZIPX_ERR_UNSUPPORTED;
      snprintf(res.message, sizeof(res.message),
               "unsupported split archive (only .zip, .rar and .7z volumes "
               "are recognised)");
    } else if(kind == 1 && !ci_strstr(archive_path, ".part")) {
      /* unrar chains its own volume naming (x.part1.rar); a byte-contiguous
       * set named x.rar.001 cannot be handed to it as-is.
       * NOTE: the old MHD endpoint refused EVERY rar volume set here. The
       * engine itself merges 'x.partNN.rar' correctly (tests/test_rar_extract.c
       * asserts it against the real fixture), so Nexus only refuses the naming
       * the engine genuinely cannot open. */
      st = ZIPX_ERR_UNSUPPORTED;
      snprintf(res.message, sizeof(res.message),
               "RAR volume sets named 'x.rar.001' are not supported yet "
               "(rename the parts to 'x.part1.rar', 'x.part2.rar', ...)");
    } else {
      st = run_engine(kind, archive_path, dst_dir, req, &res, &g);
    }
    zipx_volume_free(&vol);
  } else {
    free(vol_err);
    if(ends_with_ci(archive_path, ".zip"))      kind = 0;
    else if(ends_with_ci(archive_path, ".rar")) kind = 1;
    else if(ends_with_ci(archive_path, ".7z"))  kind = 2;
    else {
      st = ZIPX_ERR_UNSUPPORTED;
      snprintf(res.message, sizeof(res.message),
               "unsupported archive format (only .zip, .rar and .7z are "
               "accepted)");
      goto done;
    }
    st = run_engine(kind, archive_path, dst_dir, req, &res, &g);
  }

done:
  e = map_status(st);
  if(out) {
    out->err = e;
    if(res.message[0])
      snprintf(out->message, sizeof(out->message), "%s", res.message);
    else
      snprintf(out->message, sizeof(out->message), "%s",
               zipx_status_string(st));
  }
  return e;
}
