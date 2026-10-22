/* Self-built installer: path delivery + the AppInstUtil session.
 *
 * ROUTE REVERSAL (v1.0.3, 2026-10-06): the uri handed to InstallByPackage is
 * now the package's own FILE PATH (with a leading /data/ rewritten to
 * /user/data/), exactly what the reference payload (Loopayeh/pkg-sender) does
 * on real hardware — it never runs an HTTP server and never queries install
 * status. The previous loopback-stream + GetInstallStatus-poll route was our
 * own reverse-engineered protocol that no reference had ever validated: v1.0.1
 * died on click (stack), v1.0.2 died once progress appeared. Every element
 * without a hardware-proven counterpart is now gone:
 *   - no loopback stream server (src/pkg_stream.c is out of the build),
 *   - no GetInstallStatus polling (our appinst_status_t layout had no
 *     reference; a too-small struct on the worker stack is a textbook
 *     stack-smash "crashes once progress shows"),
 *   - no sceAppInstUtilTerminate call (its NID was never verified),
 *   - no AuthID raise before the session (the reference installs without one;
 *     a failed raise used to abort, now a permission problem would surface as
 *     an InstallByPackage error code in the log instead of killing the task).
 * InstallByPackage is asynchronous: rc==0 means "submitted to the system
 * installer", and the home screen is the only ground truth for the result.
 *
 * CALL SEQUENCE (mirror of the reference, nothing more):
 *   Initialize() -> InstallByPackage(&meta, &info, &playgo)
 *
 * TWO DISCIPLINES KEPT FROM THE SAME REFERENCE:
 *   1. Every native argument must stay alive and unchanged for as long as the
 *      system may still refer to it. So meta / info / playgo and the uri they
 *      point at live in the HANDLE, never on a stack frame that returns.
 *   2. icon_url must be a non-NULL EMPTY string. Passing a URL (or NULL) makes
 *      an otherwise local install collide with PlayGo's network path and fail
 *      with 0x80B21121.
 *
 * ABI NOTE: pkg_metadata_t is EIGHT fields (0x38): six pointers, then
 * uint32 slot and uint32 is_playgo_enabled. That is the layout of the
 * reference's native payload (the earlier 0x30 note here mistook it for a
 * Mono managed-layer signature — the reference payload itself is C and
 * installs fine on hardware with the 0x38 form).
 *
 * LIBRARY LOADING (the crash fix, 2026-10-05): AppInstUtil must NOT be
 * statically linked — the reference payload proved on 6.02 that linking it
 * kills the payload, and that libSceIpmi.sprx must be dlopen'd BEFORE
 * libSceAppInstUtil.sprx or AppInstUtil dies on load. Both are loaded at
 * runtime, Ipmi first, and every sceAppInstUtil* symbol is dlsym'd.
 *
 * NOT carried over: the reference runs the native calls in a separate helper
 * process it can SIGKILL, because a stuck system call would otherwise wedge the
 * caller. We call them in-process, like the other working payloads. This is the
 * one place where a hardware hang would cost us the whole payload, and it is
 * recorded as such in docs/pkg-install-without-usb.md. */
#include "pkg_installer.h"

#include "pkg_info.h"

#ifndef __linux__

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- libSceAppInstUtil (runtime dlopen; never statically linked) ----------- */

extern int kernel_set_ucred_authid(unsigned long long authid);

/* Six pointers, then slot / is_playgo_enabled (both zero) — 0x38 total. */
typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
  uint32_t    slot;
  uint32_t    is_playgo_enabled;
} pkg_metadata_t;
_Static_assert(sizeof(pkg_metadata_t) == 0x38,
               "pkg_metadata_t is 8 fields (0x38) per the reference native "
               "payload; the 0x30 layout truncated what the system reads");

/* Filled in by InstallByPackage: content_id is informational only now — the
 * status query is gone, nothing polls with it. */
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

/* INSTALL AuthID. Distinct from the SAVE domain's 0x4800000000000010 — the two
 * privilege sets must stay in separate modules. Still used by
 * pkg_installer_initialize() (the launcher self-install path); the package
 * install itself no longer raises it (the reference installs without one). */
#define MANGO_INSTALL_AUTHID 0x4800000000000006ULL

struct pkg_install_handle {
  char path[1024];
  char title_id[16];
  char display_name[160];
  /* The container's own content id. Handed to InstallByPackage verbatim: with
   * an http uri the system keys its download task on it, and an empty one is
   * what produced "submitted fine, install failed" on real hardware. */
  char content_id[48];
  /* The uri handed to InstallByPackage: pkg_path with a leading /data/
   * rewritten to /user/data/ (what the reference does), or the path verbatim
   * otherwise. Same length class as path plus the longer prefix. */
  char uri[1088];
  char note[192];

  /* Discipline 1: these stay alive for the whole install. `meta` holds POINTERS
   * INTO this handle, so the handle must outlive the install and must not be
   * moved. */
  pkg_metadata_t meta;
  pkg_info_t     info;
  playgo_info_t  playgo;

  /* finished=1 with note set means "nothing to do" (already-installed
   * precheck) — reported as a satisfied task, never as a failure. */
  int      submitted;
  int      finished;
};

/* Why the last begin() returned NULL — there is no handle to hang it on. */
static const char *g_last_error;

const char *
pkg_install_last_error(void) {
  return g_last_error;
}

/* Install-phase forensics: same discipline as the save-delete log that cracked
 * the six-crash mystery. A failed install surfaces only a 2.4s toast on the
 * real machine; this file is what survives. Every phase transition appends one
 * line (fopen/fclose each time, so a kernel panic cannot lose the last line).
 * WFM_INSTALL_LOG relocates it for host runs. Failures to write are ignored:
 * the log is forensics, never a dependency. */
static void
install_log(const char *fmt, ...) {
  const char *path = getenv("WFM_INSTALL_LOG");
  char default_path[] = "/data/Mango/logs/mango-install.log";
  FILE *f;
  va_list ap;

  if(!path || !*path) path = default_path;
  f = fopen(path, "a");
  if(!f) return;
  fprintf(f, "[%llu] ", (unsigned long long)time(NULL));
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

/* --- runtime symbol table --------------------------------------------------
 * Resolved once, Ipmi-first (see the header note). Initialize, InstallByPackage
 * and AppInstallAll are all we ever call: the reference payload uses neither
 * GetInstallStatus nor Terminate, and both of those were on our crash-path
 * suspects for v1.0.2, so they are not resolved at all any more. */
typedef int (*appinst_init_fn)(void);
typedef int (*appinst_install_fn)(const pkg_metadata_t *, pkg_info_t *,
                                  playgo_info_t *);
typedef int (*appinst_install_all_fn)(void *);
/* Uninstall (2026-10-11, 用户在 PKG 管理页要「已安装的游戏加个卸载按钮」).
 * Signature per the public AppInstUtil header — the PS4 (Orbis) and PS5
 * (Prospero) prototypes are identical: int (const char *titleId). Resolved
 * but NOT required by load_appinstutil(): an old firmware that does not export
 * it must still be able to install. */
typedef int (*appinst_uninstall_fn)(const char *);

static pthread_mutex_t g_appinst_lock = PTHREAD_MUTEX_INITIALIZER;
/* Serializes the AppInstUtil session calls themselves (Initialize /
 * InstallByPackage / AppInstallAll): the reference payload holds a mutex
 * across InstallByPackage, and our two entry points — the install worker and
 * the launcher's "add to home screen" — can otherwise meet mid-session. */
static pthread_mutex_t g_inst_session_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_ipmi_lib;
static void *g_appinst_lib;
static appinst_init_fn        p_appinst_init;
static appinst_install_fn     p_appinst_install;
static appinst_install_all_fn p_appinst_install_all;
static appinst_uninstall_fn   p_appinst_uninstall;

/* Returns 0 once Initialize/InstallByPackage are resolved; logs the dlopen
 * outcome for on-console forensics (a NULL library name in that log is the
 * signature of the Ipmi/AppInstUtil loading problem). */
static int
load_appinstutil(void) {
  int ok = 0;

  pthread_mutex_lock(&g_appinst_lock);
  if(g_appinst_lib) { ok = 1; goto out; }

  g_ipmi_lib = dlopen("libSceIpmi.sprx", RTLD_LAZY);
  if(!g_ipmi_lib)
    g_ipmi_lib = dlopen("/system/common/lib/libSceIpmi.sprx", RTLD_LAZY);
  g_appinst_lib = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
  if(!g_appinst_lib)
    g_appinst_lib = dlopen("/system/common/lib/libSceAppInstUtil.sprx",
                           RTLD_LAZY);
  install_log("dlopen ipmi=%p appinst=%p", g_ipmi_lib, g_appinst_lib);
  if(!g_appinst_lib) goto out;

  p_appinst_init = (appinst_init_fn)dlsym(g_appinst_lib,
                                          "sceAppInstUtilInitialize");
  p_appinst_install = (appinst_install_fn)dlsym(
      g_appinst_lib, "sceAppInstUtilInstallByPackage");
  p_appinst_install_all = (appinst_install_all_fn)dlsym(
      g_appinst_lib, "sceAppInstUtilAppInstallAll");
  /* Optional: a firmware without it can still install (see the typedef note). */
  p_appinst_uninstall = (appinst_uninstall_fn)dlsym(
      g_appinst_lib, "sceAppInstUtilAppUnInstall");
  ok = p_appinst_init && p_appinst_install;
out:
  pthread_mutex_unlock(&g_appinst_lock);
  return ok ? 0 : -1;
}

/* --- already-installed precheck --------------------------------------------
 * Heuristic: an installed title leaves a directory named after its title id.
 * The exact roots differ per firmware and between internal / extended storage.
 * Also exported for /api/pkg/scan (library page badges). */
int
pkg_already_installed(const char *title_id) {
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
  int rc;

  if(kernel_set_ucred_authid(MANGO_INSTALL_AUTHID) != 0) return -1;
  pthread_mutex_lock(&g_inst_session_lock);
  rc = load_appinstutil();
  if(rc == 0) rc = p_appinst_init();
  pthread_mutex_unlock(&g_inst_session_lock);
  return rc;
}

/* The launcher self-installer's fallback (app_installer.c). Same dlopen'd
 * library — sceAppInstUtilAppInstallAll is not statically linked either. */
int
pkg_appinstutil_app_install_all(void) {
  int rc;

  pthread_mutex_lock(&g_inst_session_lock);
  rc = load_appinstutil();
  if(rc == 0) {
    if(!p_appinst_install_all) rc = -1;
    else rc = p_appinst_install_all(0);
  }
  pthread_mutex_unlock(&g_inst_session_lock);
  return rc;
}

/* --- uninstall -------------------------------------------------------------
 * Symmetric with install: same runtime-loaded library, same session lock, same
 * "rc==0 means submitted, not deleted" contract. We do NOT poll for completion
 * (GetInstallStatus was deliberately dropped, see the header note) — the UI
 * says so plainly and the home screen is the ground truth. */
int
pkg_uninstall_begin(const char *title_id) {
  int rc;

  g_last_error = NULL;
  if(!title_id || !*title_id) { g_last_error = "no title id"; return -1; }
  if(load_appinstutil() != 0) {
    g_last_error = "could not load libSceAppInstUtil.sprx "
                   "(needs libSceIpmi.sprx loaded first)";
    install_log("uninstall dlopen FAILED -> abort");
    return PKG_INSTALL_UNSUPPORTED;
  }
  if(!p_appinst_uninstall) {
    g_last_error = "sceAppInstUtilAppUnInstall is not exported by this firmware";
    install_log("uninstall symbol missing");
    return PKG_INSTALL_UNSUPPORTED;
  }
  pthread_mutex_lock(&g_inst_session_lock);
  install_log("uninstall title=%s", title_id);
  rc = p_appinst_uninstall(title_id);
  install_log("uninstall title=%s rc=0x%08X", title_id, (unsigned)rc);
  pthread_mutex_unlock(&g_inst_session_lock);
  if(rc != 0) { g_last_error = "sceAppInstUtilAppUnInstall failed"; return -1; }
  return 0;
}

const char *
pkg_uninstall_last_error(void) {
  return g_last_error ? g_last_error : "uninstall failed";
}



/* `pkg_path` is ALWAYS the file on disk — the title id, the display name and
 * the already-installed precheck all read it. `stream_uri`, when non-NULL, is
 * the ONLY thing that changes: what the system installer is told to fetch.
 * NULL keeps path delivery (the hardware-proven v1.0.3 route). */
static pkg_install_handle_t *
pkg_install_begin_impl(const char *pkg_path, const char *stream_uri) {
  struct pkg_install_handle *h;

  g_last_error = NULL;

  if(!pkg_path || !*pkg_path) { g_last_error = "no package path"; return NULL; }
  install_log("begin path=%s delivery=%s (no AuthID raise — reference installs "
              "without one; permission errors now surface as "
              "InstallByPackage rc)",
              pkg_path, stream_uri ? "loopback-stream" : "path");

  /* Load the libraries BEFORE anything else can fail: a NULL here is the
   * "process dies on install click" signature (statically linked AppInstUtil
   * or Ipmi missing — see the header note). */
  if(load_appinstutil() != 0) {
    g_last_error = "could not load libSceAppInstUtil.sprx "
                   "(needs libSceIpmi.sprx loaded first)";
    install_log("dlopen appinst FAILED -> abort");
    return NULL;
  }

  h = calloc(1, sizeof(*h));
  if(!h) { g_last_error = "out of memory"; return NULL; }
  snprintf(h->path, sizeof(h->path), "%s", pkg_path);

  /* Parsing gives us both the id for the precheck and the name the console
   * displays. A parse failure is not fatal: the install can still proceed.
   * ⚠️ content_id must come back too: on a NAS the queue only holds the
   * composed smb:// URL, and until pkg_info learned to open one this call
   * failed outright (open() on an smb:// path), leaving the console to install
   * from a uri with no identity attached. */
  {
    char title[sizeof(h->display_name)];
    memset(title, 0, sizeof(title));
    if(pkg_info_read_meta(pkg_path, h->title_id, sizeof(h->title_id),
                          title, sizeof(title), NULL, 0,
                          h->content_id, sizeof(h->content_id), NULL) == 0) {
      if(title[0]) snprintf(h->display_name, sizeof(h->display_name), "%s", title);
    }
  }
  install_log("meta title_id=%s content_id=%s",
              h->title_id[0] ? h->title_id : "(none)",
              h->content_id[0] ? h->content_id : "(none)");
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
  if(pkg_already_installed(h->title_id)) {
    h->finished = 1;
    set_note(h, "already installed (%s) — skipped", h->title_id);
    install_log("precheck already-installed tid=%s -> skip (done)", h->title_id);
    return h;
  }

  /* Two deliveries, one decision:
   *   · stream_uri set — the installer pulls the bytes from our loopback range
   *     server (src/pkg_stream.c). This is the route that lets a package be
   *     installed without the file ever being in a location the system
   *     services can read, and it is NOT yet hardware-verified (see
   *     docs/pkg-install-without-usb.md §7.5).
   *   · otherwise — the PATH ITSELF (route reversal, v1.0.3): the reference
   *     payload hands InstallByPackage the package's location on disk,
   *     rewriting a leading /data/ to /user/data/ — the same directory through
   *     the path the system services see. No HTTP server, no stream session,
   *     nothing of ours stays between the installer and the file.
   * Keeping the default on the proven route is deliberate: a stream failure is
   * asynchronous and invisible to us, so it must never be the default until
   * the console has been seen to accept it. */
  if(stream_uri && stream_uri[0]) {
    snprintf(h->uri, sizeof(h->uri), "%s", stream_uri);
    install_log("uri (loopback stream) = %s", h->uri);
  } else if(!strncmp(pkg_path, "/data/", 6)) {
    snprintf(h->uri, sizeof(h->uri), "/user/data/%s", pkg_path + 6);
    install_log("uri (path delivery) = %s", h->uri);
  } else {
    snprintf(h->uri, sizeof(h->uri), "%s", pkg_path);
    install_log("uri (path delivery) = %s", h->uri);
  }

  {
    int irc;
    pthread_mutex_lock(&g_inst_session_lock);
    irc = p_appinst_init();
    pthread_mutex_unlock(&g_inst_session_lock);
    if(irc != 0) {
      g_last_error = "sceAppInstUtilInitialize failed";
      install_log("Initialize FAILED rc=0x%08X -> abort", (unsigned)irc);
      free(h);
      return NULL;
    }
  }
  install_log("Initialize ok");

  /* Discipline 2: icon_url is an empty string, NOT NULL and not a URL. */
  h->meta.uri                = h->uri;
  h->meta.ex_uri             = "";
  h->meta.playgo_scenario_id = "";
  /* The container's id, or "" when the package could not be parsed — never
   * NULL, and never a made-up value. */
  h->meta.content_id         = h->content_id[0] ? h->content_id : "";
  h->meta.content_name       = h->display_name;
  h->meta.icon_url           = "";
  h->meta.slot               = 0;
  h->meta.is_playgo_enabled  = 0;
  memset(&h->info, 0, sizeof(h->info));
  memset(&h->playgo, 0, sizeof(h->playgo));

  {
    int r;
    /* 写在调用之前：InstallByPackage 若在内部 fault（真机已发生过一次
     * 「点安装进程就没了」），这行就是最后遗言——上一轮日志只写调用后的
     * rc，进程死在里面时 rc 行永远不出现，无法与 Initialize 之死区分。 */
    install_log("calling InstallByPackage uri=%s name=%s",
                h->uri, h->display_name);
    pthread_mutex_lock(&g_inst_session_lock);
    r = p_appinst_install(&h->meta, &h->info, &h->playgo);
    pthread_mutex_unlock(&g_inst_session_lock);
    install_log("InstallByPackage rc=0x%08X content_id=%s", (unsigned)r,
                h->info.content_id[0] ? h->info.content_id : "(none)");
    if(r != 0) {
      /* The hex code is in the log; the UI shows this stable prefix. */
      g_last_error = "InstallByPackage failed (see /data/Mango/logs/mango-install.log "
                     "for the error code)";
      install_log("InstallByPackage FAILED -> task fails");
      free(h);
      return NULL;
    }
  }

  /* rc==0 means SUBMITTED: InstallByPackage is asynchronous, the system
   * installer works in the background, and the home screen is the ground
   * truth. We deliberately do not poll GetInstallStatus any more (v1.0.3 —
   * our status struct layout had no reference, and "dies once progress
   * shows" pointed straight at that poll). */
  h->submitted = 1;
  set_note(h, "已提交系统安装器，请在主机主屏幕查看安装结果");
  install_log("submitted — install handed to the system installer");
  return h;
}

pkg_install_handle_t *
pkg_install_begin(const char *pkg_path) {
  return pkg_install_begin_impl(pkg_path, NULL);
}

pkg_install_handle_t *
pkg_install_begin_uri(const char *pkg_path, const char *stream_uri) {
  /* A NULL/empty uri is path delivery by contract, not a silent downgrade:
   * callers that could not publish a stream pass what they have. */
  return pkg_install_begin_impl(pkg_path, stream_uri);
}

const char *
pkg_install_note(const pkg_install_handle_t *h) {
  if(!h) return g_last_error;
  if(h->note[0]) return h->note;
  return NULL;
}

/* Whether begin() reached "handed to the system installer" (1) or the
 * already-installed precheck (h->finished, treated by the caller as a
 * satisfied task). A handle that is neither never escapes begin(). */
int
pkg_install_submitted(const pkg_install_handle_t *h) {
  return h ? (h->submitted || h->finished) : 0;
}

void
pkg_install_free(pkg_install_handle_t *h) {
  /* No Terminate, no stream unpublish (v1.0.3): the reference payload calls
   * neither, the Terminate NID was never verified on hardware, and the
   * session simply stays usable for the next install. */
  free(h);
}

#else /* __linux__ : the host build has no AppInstUtil — stubs only. */

#include <stdio.h>
#include <sys/stat.h>

/* The installed-title heuristic only stats title directories, so it works
 * unchanged on the host build — /api/pkg/scan feeds its "installed" badge off
 * it in hosttest just like on the console. */
int
pkg_already_installed(const char *title_id) {
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

pkg_install_handle_t *
pkg_install_begin(const char *pkg_path) {
  (void)pkg_path;
  return NULL;
}

pkg_install_handle_t *
pkg_install_begin_uri(const char *pkg_path, const char *stream_uri) {
  (void)pkg_path; (void)stream_uri;
  return NULL;
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

/* Uninstall has no host analogue either — the route reports the reason from
 * here, so /api/pkg/uninstall stays a deterministic 501 in hosttest instead of
 * pretending to have done something. */
int
pkg_uninstall_begin(const char *title_id) {
  (void)title_id;
  return PKG_INSTALL_UNSUPPORTED;
}

const char *
pkg_uninstall_last_error(void) {
  return "uninstall is only available on PS5";
}

int
pkg_install_submitted(const pkg_install_handle_t *h) {
  (void)h;
  return 0;
}

void
pkg_install_free(pkg_install_handle_t *h) {
  (void)h;
}

int
pkg_installer_initialize(void) {
  return PKG_INSTALL_UNSUPPORTED;
}

int
pkg_appinstutil_app_install_all(void) {
  return -1;
}

#endif
