/* path_ops.h - path-shaped filesystem operations, NO library dependency.
 *
 * Why this is a header of its own (2026-10-16): path_util.h includes
 * <microhttpd.h>, because it is mostly request-parameter parsing. The four
 * archive engines (extract_engine.c, zip_extract.c, rar_extract.c,
 * sevenz_extract.c) are compiled with ARCH_C_FLAGS -- the relaxed flag set for
 * the vendored archive libraries, which carries no libmicrohttpd -I. The moment
 * those TUs started calling path_remove_tree() they dragged the whole HTTP
 * library into their include chain, and `make all` died on
 *     src/path_util.h:6:10: fatal error: 'microhttpd.h' file not found
 * (the host build stayed green: HOST_PKG_CONFIG has libmicrohttpd installed,
 * so only the console build could see the breakage).
 *
 * Deleting a directory tree has nothing to do with HTTP, so it lives here,
 * where an arch TU can reach it. path_util.h includes this header, so existing
 * callers keep compiling unchanged.
 *
 * Rule for anything added here: stay dependency-free. A single <microhttpd.h>
 * would put the console build back in exactly that hole.
 */
#pragma once

/* Recursive delete, best effort: removes `path` and everything under it.
 *
 *   0  -> `path` does not exist now (it was removed, or was already gone)
 *  -1  -> something survived: a child could not be unlinked, or `path` itself
 *         could not be removed. errno is whatever the last failure set.
 *
 * Why this exists as ONE implementation (2026-10-16): the tree had five copies
 * of this walk (`mango_port.c`, `extract_engine.c`, `zip_extract.c`,
 * `rar_extract.c`, `sevenz_extract.c`) and they did NOT behave the same:
 * the four extract-engine copies did `if(!(d = opendir(path))) { rmdir(path); return; }`,
 * so handing them a plain FILE left it on disk -- opendir() fails with ENOTDIR,
 * then rmdir() on a non-directory fails too. Only the mango_port.c copy did the
 * lstat() first and unlink()ed non-directories. Same concept, two behaviours.
 *
 * The `int` return is the point: it lets a host unit test assert the invariant
 * ("after this call the tree is gone") instead of a guard grepping the source
 * for which of the five copies a call site happens to reach. */
int path_remove_tree(const char *path);
