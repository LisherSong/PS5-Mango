#ifndef PKG_STREAM_H
#define PKG_STREAM_H
/* Loopback HTTP range server that feeds the console's OWN package installer.
 *
 * WHY THIS EXISTS
 * `sceAppInstUtilInstallByPackage()` takes six pointers, the first of which is
 * a *uri*. That uri may be an `http://` URL. "A PKG must sit on a USB stick" is
 * a limitation of the console's built-in install screen (Settings > Storage),
 * NOT of the kernel and NOT of the API. Serving the package to the console over
 * 127.0.0.1 therefore needs no elevated capability at all — only a different
 * uri. This module is that server.
 *
 * SHAPE OF THE PROTOCOL (reverse-engineered from captures; see
 * docs/pkg-install-without-usb.md §3.2). The system installer is an HTTP range
 * client, in three phases:
 *   1. header  — a burst of short connections, each re-reading the first 64 KiB
 *                (Range: bytes=0-65535). The same range must be re-servable.
 *   2. sidecar — one Range-less GET of "<content_id>.crc" that MUST 404.
 *                Answering 206 there poisons chunk checks and aborts the
 *                install, so anything we do not recognise must be a 404.
 *   3. bulk    — two parallel long-lived connections serving contiguous 16 MiB
 *                ranges over [65536, end), A/B ping-pong.
 * Every request carries `?product=0287&serverIpAddr=127.0.0.1&r=00000000`, so
 * the route is matched only after the query string is stripped.
 *
 * THREE DISCIPLINES, all taken from the reference implementation's failures:
 *   * loopback ONLY. This is never bound to 0.0.0.0 — the payload must not
 *     become a file server for the LAN.
 *   * ONE published payload at a time. The install session is a global
 *     singleton (same reasoning as the save domain's /dev/pfsmgr mount); two
 *     concurrent streams would race the same singleton anyway.
 *   * NON-BLOCKING, pumped from the single-threaded main loop. No threads: a
 *     PS5 payload spawning threads is a liability, and the cooperative model is
 *     already how every other task is driven.
 *
 * VERIFY ON HARDWARE (Phase 0): the console tolerating a non-standard port is
 * inferred, not observed — the reference pins 18841. We try that port first and
 * fall back to an ephemeral one, and the uri always carries the port we really
 * bound, so a fallback cannot hand the console a wrong address. */
#include "nexus_common.h"

#include <stdint.h>
#include <stddef.h>

/* Bind the loopback listener (idempotent). Returns 0 on success. */
int pkg_stream_start(void);

/* The port actually bound; 0 when not running. Read it back instead of
 * assuming — the listener may have fallen back off the preferred port. */
uint16_t pkg_stream_port(void);

int pkg_stream_running(void);

/* Offer `path` to the console under `session` (the unique name the uri
 * carries). Replaces any previous publication. Opens the file up front so a
 * missing/unreadable package is reported here rather than as a mid-stream
 * stall the console would surface as a generic install failure. */
int pkg_stream_publish(const char *session, const char *path, uint64_t size);

void pkg_stream_unpublish(void);

/* The payload's first byte must be readable before publishing; publishing is
 * the only place a bad path can be caught cheaply. */
int pkg_stream_publishing(void);

/* A stream name that is unique across attempts. The console remembers recently
 * used stream URLs ACROSS payload restarts — reusing "package-1.pkg" after a
 * redeploy gets the request rejected — so the name is time-based, not a plain
 * counter (a counter alone restarts at 1 with the process). */
void pkg_stream_make_session(char *out, size_t n);

/* `http://127.0.0.1:<bound port>/stream/install/<session>.pkg` — the value that
 * goes into pkg_metadata_t.uri. Returns 0 on success. */
int pkg_stream_url(char *out, size_t n, const char *session);

/* Service pending I/O. Returns the number of work units performed: a non-zero
 * return means "there is more to do, do not sleep" — this is how a multi-GB
 * stream reaches full speed without a thread. */
int pkg_stream_tick(void);

void pkg_stream_stop(void);
#endif
