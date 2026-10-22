#ifndef PKG_STREAM_H
#define PKG_STREAM_H
/* Loopback HTTP range server that feeds the console's OWN package installer.
 *
 * WHY THIS EXISTS
 * `sceAppInstUtilInstallByPackage()` takes a metadata struct whose first field
 * is a *uri*. That uri may be an `http://` URL. "A PKG must sit on a USB stick" is
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
 * v1.0.25 SHAPE CHANGE — A ROUTE, NOT A SECOND LISTENER
 * This module used to bind its own loopback socket next to the main service and
 * hand-write every byte of HTTP/1.1 (that is what "the reference pins 18841"
 * was about). It now lives as the /stream/install/ route INSIDE the main
 * libmicrohttpd daemon — the same shape as the reference implementation's
 * filemgr_pkg_source_request(). What that buys:
 *   * the HTTP layer (status lines, keep-alive, HEAD semantics, malformed
 *     requests) is MHD's, not ours — one less hand-written parser to get wrong
 *     on a path whose failure mode is "the whole payload hangs";
 *   * the uri now carries the MAIN service port, so a NAS install works
 *     through whatever port the payload was started on, with no second bind
 *     to fail;
 *   * concurrency is MHD's per-connection thread model, which the bulk phase
 *     needs anyway (two parallel connections).
 * What did NOT change: loopback only, one published payload at a time, the
 * three protocol phases, and the fact that a remote source has no path-delivery
 * fallback.
 *
 * DISCIPLINES:
 *   * loopback ONLY. The route refuses any connection whose peer is not
 *     127.0.0.1 (403) — the payload must not become a file server for the LAN.
 *   * ONE published payload at a time. The install session is a global
 *     singleton (same reasoning as the save domain's /dev/pfsmgr mount); two
 *     concurrent streams would race the same singleton anyway.
 *   * The offer outlives the call that published it: InstallByPackage is
 *     asynchronous and the console pulls the bytes afterwards. It is taken
 *     down 30 minutes after the LAST byte moved (pkg_stream_tick), which is
 *     far past any real gap between two range requests.
 */
#include "mango_common.h"

#include <microhttpd.h>
#include <stdint.h>
#include <stddef.h>

/* The published-payload snapshot the route answers against. A plain value
 * struct (no behaviour) so the planning function can be PURE and unit-tested
 * on the host without standing up a server or touching the transfer backend.
 * The live module fills one from g_ps under its lock before planning. */
typedef struct {
  int      published;       /* 0 => nothing is on offer (every route is 404) */
  char     session[128];    /* unique stream name carried in the uri */
  uint64_t size;            /* total payload bytes */
} ps_published_t;

/* The decision ps_plan_request() reaches: an HTTP status plus, for 200/206,
 * the byte window to serve. head_only flags a HEAD (same headers, no body). */
typedef struct {
  unsigned  status;         /* MHD_HTTP_* */
  uint64_t  start;          /* window offset inside the payload */
  uint64_t  length;         /* window size */
  int       head_only;      /* HEAD: same headers, no body (MHD handles it) */
} ps_plan_t;

/* Pure HTTP decision for the /stream/install/ route — no MHD, no globals.
 * Order of the rules (see the implementation for the rationale):
 *   1. loopback only           -> 403 when !is_loopback
 *   2. GET/HEAD only           -> 405 for any other method
 *   3. session match           -> 404 for ".crc" sidecar probes & unknowns
 *   4. range answers           -> none: 200, usable: 206, unusable: 416
 * Exposed purely so .build/pkg_stream_test.c can exercise every branch
 * exhaustively without a listening socket. */
ps_plan_t ps_plan_request(const char *method, const char *url,
                          const char *range, int is_loopback,
                          const ps_published_t *pub);

/* Offer `path` to the console under `session` (the unique name the uri
 * carries). Replaces any previous publication. Opens the file up front so a
 * missing/unreadable package is reported here rather than as a mid-stream
 * stall the console would surface as a generic install failure.
 * Returns 0 on success, -1 with no state change on failure. */
int pkg_stream_publish(const char *session, const char *path, uint64_t size);

/* Same contract, for a payload on a NAS (`smb://...` / `nfs://...`). The data
 * is served through the transfer abstraction — the installer never sees the
 * NAS, only our loopback uri. A remote source has NO path-delivery fallback
 * (the system installer cannot open smb://), so a -1 here must surface as a
 * failed task, never a silent downgrade. */
int pkg_stream_publish_nas(const char *session, const char *url, uint64_t size);

/* stat + publish + build the uri, atomically. The one call an install path
 * should use: see the note at the definition for why it cannot be left to the
 * caller to assemble. Returns 0 on success, -1 with no state change (the
 * caller then falls back to handing the installer the file's own path). */
int pkg_stream_prepare(const char *path, char *url_out, size_t url_out_n);

/* Same contract, for a payload that lives on a NAS (an smb:// or nfs:// URL).
 * There is NO path-delivery fallback for a remote source — the system
 * installer cannot open smb:// — so a failure here must be reported, never
 * silently downgraded. */
int pkg_stream_prepare_nas(const char *url, char *url_out, size_t url_out_n);

void pkg_stream_unpublish(void);

/* A stream name that is unique across attempts. The console remembers recently
 * used stream URLs ACROSS payload restarts — reusing "package-1.pkg" after a
 * redeploy gets the request rejected — so the name is time-based, not a plain
 * counter (a counter alone restarts at 1 with the process). */
void pkg_stream_make_session(char *out, size_t n);

/* `http://127.0.0.1:<main service port>/stream/install/<session>.pkg` — the
 * value that goes into pkg_metadata_t.uri. Returns 0 on success. */
int pkg_stream_url(char *out, size_t n, const char *session);

/* The /stream/install/ route. Called by websrv.c's dispatcher with the
 * already query-stripped url and the request method; answers with 200 / 206 /
 * 416 / 404 / 403 / 405 per the protocol above. */
enum MHD_Result pkg_stream_request(struct MHD_Connection *conn,
                                   const char *url, const char *method);

/* Lifecycle only: withdraw an offer whose last byte moved more than 30
 * minutes ago. Returns non-zero when it withdrew something ("do not sleep").
 * The bytes themselves are MHD's business since v1.0.25 — this is all that is
 * left for the main loop, and it is a cheap no-op when nothing is published. */
int pkg_stream_tick(void);

#endif
