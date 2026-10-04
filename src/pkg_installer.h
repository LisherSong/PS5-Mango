#pragma once
/* Self-built package installer: loopback delivery + the AppInstUtil session.
 *
 * WHY A HANDLE AND NOT ONE CALL
 * The console's own installer pulls the package over HTTP in the background.
 * "sceAppInstUtilInstallByPackage returned 0" therefore means "the download
 * started", NOT "the game is installed" — the old one-call shape reported DONE
 * while nothing had reached the home screen yet, which is exactly the reported
 * "进度走完但游戏没装上". Progress and the terminal state have to come from the
 * system's own status query, polled by a caller that stays alive.
 *
 * WHY LOOPBACK HTTP AND NOT A PATH
 * A filesystem path cannot be handed to the installer: it has no way to open a
 * file on /data (the built-in install screen only reads USB/disc media). The
 * uri it takes may be `http://`, so the package goes out over 127.0.0.1 from
 * src/pkg_stream.c and the console fetches it like any other download. That is
 * what makes local staging (/data/wfm/incoming) and NAS sources installable
 * without a USB stick.
 */
#include "nexus_common.h"

#include <stdint.h>

/* Sentinel returned by the enqueue path when the PDK was not built (host). */
#define PKG_INSTALL_UNSUPPORTED 0x7fffffff

typedef struct pkg_install_handle pkg_install_handle_t;

/* AuthID + AppInstUtil session init. Kept for the launcher-app path
 * (app_installer.c), which installs its own title directory. */
int pkg_installer_initialize(void);

/* Starts delivery and hands the package to the console's installer.
 * Returns NULL only when the install could not be started at all; the reason
 * is then in pkg_install_last_error(). A precheck hit (already installed) is
 * NOT an error: the handle comes back finished, with a note. */
pkg_install_handle_t *pkg_install_begin(const char *pkg_path);

/* NEXUS_OK      the console reported the title playable/completed (100%)
 * NEXUS_ERR_PARTIAL  still running; *percent carries the last known value
 * anything else      the install failed; pkg_install_note() has the reason. */
nexus_err_t pkg_install_poll(pkg_install_handle_t *h, int *percent);

/* Human-readable progress note or failure reason, or NULL. */
const char *pkg_install_note(const pkg_install_handle_t *h);

/* Why the last pkg_install_begin() returned NULL. */
const char *pkg_install_last_error(void);

/* Releases the handle; ends the AppInstUtil session when one was opened. */
void pkg_install_free(pkg_install_handle_t *h);
