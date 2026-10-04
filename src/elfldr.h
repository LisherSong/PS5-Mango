#pragma once

/* Send an ELF payload to the console's elfldr (127.0.0.1:9021). Used by the
 * "push ELF payload" route; lives on its own so the archive-helper removal
 * does not take the ELF-launching feature with it. */
int elfldr_send(const char *path);
