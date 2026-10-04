/* Self-built installer: loopback package delivery + the AppInstUtil session.
 *
 * A package goes out over 127.0.0.1 (src/pkg_stream.c) and the console's own
 * installer fetches it like any other download. Handing it a filesystem path —
 * what this file used to do — cannot work: the installer has no way to open a
 * file on /data, so the call either failed outright or left the task centre
 * reporting "done" for a game that never appeared on the home screen.
 *
 * CALL SEQUENCE (from the reference implementation):
 *   Initialize() -> InstallByPackage(&meta, &info, &playgo)
 *                -> (loop) GetInstallStatus(info.content_id, &status)
 *                -> Terminate()
 *
 * THREE DISCIPLINES CARRIED OVER, each one a bug the reference hit first:
 *   1. Every native argument must stay alive and unchanged for as long as the
 *      system may still refer to it — including after later status queries. So
 *      meta / info / playgo and the URI they point at live in the HANDLE, never
 *      on a stack frame that returns.
 *   2. icon_url must be a non-NULL EMPTY string. Passing a URL (or NULL) makes
 *      an otherwise local install collide with PlayGo's network path and fail
 *      with 0x80B21121.
 *   3. The stream URL must be unique per attempt: the console remembers recent
 *      stream URLs ACROSS payload restarts and rejects a repeat.
 *
 * ABI NOTE: pkg_metadata_t is SIX pointers (0x30). The 0x38 layout that keeps
 * reappearing is the Mono managed-layer signature, not the native ABI — an
 * extra slot/is_playgo_enabled tail shifts every remaining field.
 *
 * NOT carried over: the reference runs the native calls in a separate helper
 * process it can SIGKILL, because a stuck system call would otherwise wedge the
 * caller. We call them in-process, like the other working payloads. This is the
 * one place where a hardware hang would cost us the whole payload, and it is
 * recorded as such in docs/pkg-install-without-usb.md. */
#include "pkg_installer.h"

#include "pkg_info.h"
#include "pkg_stream.h"

#ifndef __linux__

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- libSceAppInstUtil (link: -lSceAppInstUtil -lkernel_sys) -------------- */

extern int kernel_set_ucred_authid(unsigned long long authid);
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);

/* Six pointers, in this order. */
typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} pkg_metadata_t;
_Static_assert(sizeof(pkg_metadata_t) == 0x30,
               "pkg_metadata_t must be 6 pointers (0x30) — the 0x38 layout was "
               "the Mono managed-layer signature, not the native ABI");

/* Filled in by InstallByPackage: content_id is what the status query needs. */
typedef struct {
  char content_id[48];
  int  type;
  int  platform;
} pkg_info_t;

/* Large output structure the system writes; we never read it. */
typedef struct {
  char          languages[30][8];
  char          playgo_scenario_ids[64][3];
  char          content_ids[64][48];
  unsigned char unknown[6480];
} playgo_info_t;
_Static_assert(sizeof(playgo_info_t) == 0x2700,
               "sceAppInstUtil PlayGoInfo ABI mismatch");

/* SceAppInstallStatusInstalled. Only the head is relied on; the tail is
 * reproduced so the system has the room it expects to write into.
 * The status strings are compared, not guessed (docs/pkg-install-without-usb.md):
 *   terminal success : "playable" | "completed"
 *   failure          : "error" | "none", or a non-zero error_info.error_code
 */
typedef struct {
  char     status[16];
  char     src_type[8];
  uint32_t remain_time;
  uint64_t downloaded_size;
  uint64_t initial_chunk_size;
  uint64_t total_size;
  uint32_t promote_progress;
  struct {
    int32_t error_code;
    int32_t version;
    char    description[512];
    char    type[9];
  } error_info;
  int32_t local_copy_percent;
  int32_t is_copy_only;
} appinst_status_t;

extern int sceAppInstUtilInstallByPackage(const pkg_metadata_t *meta,
                                          pkg_info_t *info,
                                          playgo_info_t *playgo);
extern int sceAppInstUtilGetInstallStatus(const char *content_id,
                                          appinst_status_t *status);

/* INSTALL AuthID. Distinct from the SAVE domain's 0x4800000000000010 — the two
 * privilege sets must stay in separate modules. */
#define NEXUS_INSTALL_AUTHID 0x4800000000000006ULL

/* The console's status service is not free: one query per tick would hammer it
 * while the install runs for minutes. A progress bar does not need better. */
#define POLL_MIN_INTERVAL_MS 300u

struct pkg_install_handle {
  char path[1024];
  char title_id[16];
  char display_name[160];
  char session[128];
  char uri[192];
  char note[192];
  char fail[192];

  /* Discipline 1: these stay alive for the whole install. `meta` holds POINTERS
   * INTO this handle, so the handle must outlive the install and must not be
   * moved. */
  pkg_metadata_t meta;
  pkg_info_t     info;
  playgo_info_t  playgo;

  int      started;
  int      finished;
  int      cancelling;
  int      session_open;
  int      last_percent;
  uint64_t last_poll_ms;
};

/* Why the last begin() returned NULL — there is no handle to hang it on. */
static const char *g_last_error;

const char *
pkg_install_last_error(void) {
  return g_last_error;
}

/* --- readiness -------------------------------------------------------------
 * Judgment is by path, never by memory: a one-click package may have injected
 * the payload without the user ever "installing" anything. */
static int
file_exists(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0;
}

static int
kstuff_ready(void) {
  if(file_exists("/proc/kstuff")) return 1;                      /* running */
  if(file_exists("/data/ps5_autoloader/autoload.txt")) return 1; /* autoloaded */
  return 0;
}

/* --- already-installed precheck --------------------------------------------
 * Heuristic: an installed title leaves a directory named after its title id.
 * The exact roots differ per firmware and between internal / extended storage. */
static int
already_installed(const char *title_id) {
  static const char *roots[] = {
    "/user/app/", "/mnt/ext1/user/app/", "/data/app/"
  };
  char path[512];

  if(!title_id || !*title_id) return 0;
  for(size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    struct stat st;
    snprintf(path, sizeof(path), "%s%s", roots[i], title_id);
    if(!stat(path, &st) && S_ISDIR(st.st_mode)) return 1;
  }
  return 0;
}

/* --- helpers --------------------------------------------------------------- */

static uint64_t
now_ms(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
  return (uint64_t)time(NULL) * 1000u;
}

static void
set_note(pkg_install_handle_t *h, const char *fmt, ...) {
  va_list ap;
  if(!h) return;
  va_start(ap, fmt);
  vsnprintf(h->note, sizeof(h->note), fmt, ap);
  va_end(ap);
}

int
pkg_installer_initialize(void) {
  if(kernel_set_ucred_authid(NEXUS_INSTALL_AUTHID) != 0) return -1;
  return sceAppInstUtilInitialize();
}

/* --- lifecycle ------------------------------------------------------------- */

pkg_install_handle_t *
pkg_install_begin(const char *pkg_path) {
  struct pkg_install_handle *h;

  g_last_error = NULL;

  if(!pkg_path || !*pkg_path) { g_last_error = "no package path"; return NULL; }
  /* Judgment is by whether the kernel call WORKS, not by marker files: loaders
   * differ in what traces they leave (the user's kstuff was active yet neither
   * /proc/kstuff nor the autoloader marker existed, and install was refused
   * with "kstuff is not running" while it was running). A successful
   * kernel_set_ucred_authid is the only evidence that matters — when it
   * succeeds we proceed even with no markers; when it fails, the marker scan
   * only decides how the error reads. */
  if(kernel_set_ucred_authid(NEXUS_INSTALL_AUTHID) != 0) {
    g_last_error = kstuff_ready()
      ? "could not raise AuthID to 0x4800000000000006"
      : "kstuff is not running (no /proc/kstuff, no autoloader marker; "
        "kernel AuthID raise failed)";
    return NULL;
  }

  h = calloc(1, sizeof(*h));
  if(!h) { g_last_error = "out of memory"; return NULL; }
  snprintf(h->path, sizeof(h->path), "%s", pkg_path);

  /* Parsing gives us both the id for the precheck and the name the console
   * displays. A parse failure is not fatal: the install can still proceed. */
  {
    char title[sizeof(h->display_name)];
    memset(title, 0, sizeof(title));
    if(pkg_info_read_meta(pkg_path, h->title_id, sizeof(h->title_id),
                          title, sizeof(title), NULL, 0, NULL) == 0) {
      if(title[0]) snprintf(h->display_name, sizeof(h->display_name), "%s", title);
    }
  }
  if(!h->display_name[0]) {
    const char *base = strrchr(pkg_path, '/');
    base = base ? base + 1 : pkg_path;
    snprintf(h->display_name, sizeof(h->display_name), "%s", base);
    /* drop the extension so the console does not show "game.pkg" */
    if((base = strrchr(h->display_name, '.')) && base > h->display_name)
      h->display_name[base - h->display_name] = 0;
  }

  /* Precheck: re-installing is a decision, not an accident. Reported through
   * poll() as "finished" with a note, never as a failure — the goal state is
   * already reached. */
  if(already_installed(h->title_id)) {
    h->finished = 1;
    set_note(h, "already installed (%s) — skipped", h->title_id);
    return h;
  }

  /* The delivery channel. Without it the console has nothing to fetch: a path
   * on /data is not something its installer can open. */
  if(pkg_stream_start() != 0) {
    set_note(h, "could not start the loopback stream server");
    return h;
  }
  pkg_stream_make_session(h->session, sizeof(h->session));
  if(pkg_stream_url(h->uri, sizeof(h->uri), h->session) != 0) {
    set_note(h, "could not build the stream uri");
    return h;
  }
  if(pkg_stream_publish(h->session, pkg_path, 0) != 0) {
    /* Failing here is deliberate: an unreadable package must be refused now,
     * not discovered by the console as a mid-stream stall it reports as a
     * generic install error. */
    set_note(h, "cannot read the package: %s", pkg_path);
    return h;
  }
  h->session_open = 1;

  if(sceAppInstUtilInitialize() != 0) {
    set_note(h, "sceAppInstUtilInitialize failed");
    pkg_stream_unpublish();
    h->session_open = 0;
    return h;
  }

  /* Discipline 2: icon_url is an empty string, NOT NULL and not a URL. */
  h->meta.uri                = h->uri;
  h->meta.ex_uri             = "";
  h->meta.playgo_scenario_id = "";
  h->meta.content_id         = "";
  h->meta.content_name       = h->display_name;
  h->meta.icon_url           = "";
  memset(&h->info, 0, sizeof(h->info));
  memset(&h->playgo, 0, sizeof(h->playgo));

  {
    int r = sceAppInstUtilInstallByPackage(&h->meta, &h->info, &h->playgo);
    if(r != 0) {
      set_note(h, "InstallByPackage failed (0x%08X)", (unsigned)r);
      sceAppInstUtilTerminate();
      pkg_stream_unpublish();
      h->session_open = 0;
      h->finished = 1;
      return h;
    }
  }

  h->started      = 1;
  h->last_poll_ms = now_ms();
  /* A missing content_id is not fatal, but it means we cannot follow progress. */
  if(!h->info.content_id[0])
    set_note(h, "installing; the system reported no content id, progress unavailable");
  return h;
}

const char *
pkg_install_note(const pkg_install_handle_t *h) {
  if(!h) return g_last_error;
  if(h->note[0]) return h->note;
  if(h->fail[0]) return h->fail;
  return NULL;
}

nexus_err_t
pkg_install_poll(pkg_install_handle_t *h, int *percent) {
  appinst_status_t st;

  if(!h || !percent) return NEXUS_ERR_GENERIC;
  *percent = 0;

  /* Satisfied without touching the kernel (precheck), or a failure already
   * reported at begin() time. Either way, reporting it as done is what makes
   * the task centre show the right thing. */
  if(h->finished) { *percent = 100; return NEXUS_OK; }

  if(!h->started) return NEXUS_ERR_GENERIC;

  /* Throttle: the install runs for minutes and the status service is a shared
   * system one. Between queries the caller keeps its last known value. */
  {
    uint64_t t = now_ms();
    if(h->last_poll_ms && t - h->last_poll_ms < POLL_MIN_INTERVAL_MS) {
      *percent = h->last_percent;
      return NEXUS_ERR_PARTIAL;
    }
    h->last_poll_ms = t;
  }

  /* Progress comes from the system's own query, never from our byte counter:
   * the install continues in the background, so "bytes we served" and "what is
   * installed" are different things. */
  if(!h->info.content_id[0]) {
    *percent = h->last_percent;
    return NEXUS_ERR_PARTIAL;
  }

  memset(&st, 0, sizeof(st));
  if(sceAppInstUtilGetInstallStatus(h->info.content_id, &st) != 0) {
    /* A failed query is not a failed install — report the last value and let
     * the caller keep polling. */
    *percent = h->last_percent;
    return NEXUS_ERR_PARTIAL;
  }

  if(st.error_info.error_code != 0 || !strcmp(st.status, "error") ||
     !strcmp(st.status, "none")) {
    snprintf(h->fail, sizeof(h->fail),
             "installer reported \"%s\" (error 0x%08X)",
             st.status[0] ? st.status : "error",
             (unsigned)st.error_info.error_code);
    h->finished = 1;
    return NEXUS_ERR_GENERIC;
  }

  if(st.total_size) {
    uint64_t pct = (st.downloaded_size >= st.total_size)
                       ? 100 : (st.downloaded_size * 100) / st.total_size;
    h->last_percent = (int)pct;
  }
  *percent = h->last_percent;

  if(!strcmp(st.status, "playable") || !strcmp(st.status, "completed")) {
    h->last_percent = 100;
    *percent = 100;
    h->finished = 1;
    return NEXUS_OK;
  }
  return NEXUS_ERR_PARTIAL;
}

void
pkg_install_free(pkg_install_handle_t *h) {
  if(!h) return;
  if(h->session_open) {
    pkg_stream_unpublish();
    h->session_open = 0;
  }
  /* Terminate only once we are sure we are finished with the session. The
   * terminal cancel symbol is NOT verified on hardware, so we never call one:
   * dropping the publication is what we can do safely — the console's installer
   * then aborts on its own, having been given no corrupt bytes. */
  if(h->started) sceAppInstUtilTerminate();
  free(h);
}

#else /* __linux__ : the host build has no AppInstUtil — stubs only. */

pkg_install_handle_t *
pkg_install_begin(const char *pkg_path) {
  (void)pkg_path;
  return NULL;
}

nexus_err_t
pkg_install_poll(pkg_install_handle_t *h, int *percent) {
  (void)h;
  if(percent) *percent = 0;
  return NEXUS_ERR_UNSUPPORTED;
}

const char *
pkg_install_note(const pkg_install_handle_t *h) {
  (void)h;
  return "package installation is only available on PS5";
}

const char *
pkg_install_last_error(void) {
  return "package installation is only available on PS5";
}

void
pkg_install_free(pkg_install_handle_t *h) {
  (void)h;
}

int
pkg_installer_initialize(void) {
  return PKG_INSTALL_UNSUPPORTED;
}

#endif
