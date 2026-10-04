#pragma once

#include <stddef.h>

int archive_path_supported(const char *path);
/* Multi-part RAR helpers: users tend to select every volume of a split set
 * (game.part1.rar + part2 + …, or the legacy game.rar + game.r00 + …). The
 * 7z engine only reads the first volume, so the API layer collapses members
 * to their first volume instead of failing with "unsupported archive type". */
int archive_multipart_member(const char *path);
char *archive_multipart_first(const char *path);
int archive_normalize_sources(char **paths, size_t *count);
int archive_output_path(const char *archive, const char *base, int separate,
                        char *out, size_t out_size);
