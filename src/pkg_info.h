#pragma once

#include <microhttpd.h>

enum MHD_Result api_pkg_info(struct MHD_Connection *conn, const char *body,
                             size_t body_size);
enum MHD_Result api_pkg_icon(struct MHD_Connection *conn, const char *body,
                             size_t body_size);

/* Recover title id / title / version (and the container size) from a `.pkg`'s
 * embedded PARAM.SFO (PS4) or param.json (PS5). Any of the out buffers may be
 * NULL. Returns 0 when a title id or title was recovered, -1 otherwise. Used by
 * the PKG library scan so the card can be labelled without the front-end having
 * to call /api/pkg/info per file. */
int pkg_info_read_meta(const char *path, char *title_id, size_t tid_sz,
                       char *title, size_t title_sz, char *version, size_t ver_sz,
                       unsigned long long *total_size);
