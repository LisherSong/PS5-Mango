#pragma once

#include <stddef.h>
#include <limits.h>

#include <microhttpd.h>

/* path_remove_tree() is declared in its own dependency-free header: the four
 * archive engines call it and are compiled with ARCH_C_FLAGS, which has no
 * libmicrohttpd -I. Re-exported here so existing callers keep compiling. */
#include "path_ops.h"

char *query_value(struct MHD_Connection *conn, const char *key);
char *header_value(struct MHD_Connection *conn, const char *key);
char *request_value(struct MHD_Connection *conn, const char *header,
                    const char *query);
char *form_decode(const char *src, size_t len);
char *body_form_value(const char *body, size_t body_size, const char *key);
/* Parse a flat JSON object body (as sent by the MANGO front-end) and return the
 * string value for `key`, with full JSON string unescaping (\", \\, \/, \uXXXX ->
 * UTF-8, including surrogate pairs). Caller frees the result. Returns NULL if the
 * key is absent or the value is not a string. */
char *body_json_value(const char *body, size_t body_size, const char *key);

void free_paths(char **paths, size_t count);
int parse_paths(const char *raw, char ***out_paths, size_t *out_count);

const char *path_basename(const char *path);
void path_basename_copy(const char *path, char *out, size_t size);
int path_dirname(const char *path, char *out, size_t size);
int path_join(char *out, size_t size, const char *dir, const char *name);
int relative_path_safe(const char *path);
int path_join_relative(char *out, size_t size, const char *dir, const char *rel);
char *fs_path_value(char *path);
