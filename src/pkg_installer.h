#pragma once
/* Self-built package installer: PATH delivery + the AppInstUtil session.
 *
 * ROUTE (v1.0.3, mirrors Loopayeh/pkg-sender, hardware-proven there):
 * the uri handed to InstallByPackage is the package's own path on disk
 * (leading /data/ rewritten to /user/data/). InstallByPackage is ASYNC —
 * rc==0 means "submitted to the system installer", and the home screen is
 * the only ground truth. We deliberately do NOT poll GetInstallStatus (our
 * status struct had no reference and was on the crash path) and do NOT run
 * a loopback stream server any more.
 *
 * WHY A HANDLE AND NOT ONE CALL
 * meta / info / playgo hold pointers into the handle and must outlive the
 * call; the handle is also what carries the already-installed precheck
 * result (finished + note) back to the task centre.
 */
#include "mango_common.h"

#include <stdint.h>

/* Sentinel returned by the enqueue path when the PDK was not built (host). */
#define PKG_INSTALL_UNSUPPORTED 0x7fffffff

typedef struct pkg_install_handle pkg_install_handle_t;

/* AuthID + AppInstUtil session init. Kept for the launcher-app path
 * (app_installer.c), which installs its own title directory. */
int pkg_installer_initialize(void);

/* sceAppInstUtilAppInstallAll through the runtime-loaded library — the
 * launcher self-installer's fallback path. -1 when the library or symbol
 * is unavailable. */
int pkg_appinstutil_app_install_all(void);

/* Hands the package path to the console's installer. Returns NULL when the
 * install could not be STARTED (reason in pkg_install_last_error()); a
 * non-NULL handle always means a terminal outcome for the task: either the
 * already-installed precheck (finished, note set) or a successful submission
 * (pkg_install_submitted() != 0). */
pkg_install_handle_t *pkg_install_begin(const char *pkg_path);

/* Same as pkg_install_begin(), except the bytes come from `stream_uri`
 * (a loopback http:// range source, see src/pkg_stream.c) instead of from the
 * file's own path. The package is still READ from pkg_path for its title id,
 * display name and the already-installed precheck — only the delivery differs.
 *
 * ⚠️ Not the default: whether the console accepts a loopback uri at all is
 * unverified on hardware (docs/pkg-install-without-usb.md §7.5), and a refusal
 * surfaces only as an asynchronous install failure we cannot see. Pass
 * stream_uri = NULL/"" to get plain path delivery. */
pkg_install_handle_t *pkg_install_begin_uri(const char *pkg_path,
                                            const char *stream_uri);

/* 1 when the handle represents a terminal, non-failure outcome: the package
 * was handed to the system installer, or the precheck found it installed
 * already. 0 never escapes pkg_install_begin() in practice. */
int pkg_install_submitted(const pkg_install_handle_t *h);

/* Human-readable progress note or failure reason, or NULL. */
const char *pkg_install_note(const pkg_install_handle_t *h);

/* Why the last pkg_install_begin() returned NULL. */
const char *pkg_install_last_error(void);

/* Uninstall an installed title: sceAppInstUtilAppUnInstall(title_id) through the
 * same runtime-dlopen'd libSceAppInstUtil (it is not statically linked either —
 * see the header note in pkg_installer.c). Returns 0 when the request was
 * ACCEPTED by the system installer; that is NOT a synchronous deletion — the
 * home screen is the ground truth, exactly like the install path.
 * PKG_INSTALL_UNSUPPORTED when the library or the symbol is unavailable
 * (always, on the host) — that is a platform limitation, not a failure of this
 * request, so the route reports it as 501 instead of 500. -1 with
 * pkg_uninstall_last_error() set for a genuine failure. */
int pkg_uninstall_begin(const char *title_id);
const char *pkg_uninstall_last_error(void);

/* 1 when a title directory named after title_id exists under any of the
 * installed-app roots (/user/app, /mnt/ext1/user/app, /data/app). Shared by
 * the install precheck and /api/pkg/scan so the library page can badge a
 * package "已安装" instead of pretending everything is waiting. */
int pkg_already_installed(const char *title_id);

/* Releases the handle. Does NOT end the AppInstUtil session: Terminate's NID
 * was never verified on hardware and the reference never calls it. */
void pkg_install_free(pkg_install_handle_t *h);
