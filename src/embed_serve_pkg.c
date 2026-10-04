/* The Windows helper the payload hands out: assets/serve-pkg.py.
 *
 * Served straight off the payload's web root so the PS5 page can offer it as a
 * download — the whole point is that a user who can open http://<ps5>:2026/ has
 * no other place to get it from.
 *
 * WHY .incbin AND NOT gen-asset-module.py
 * The generator emits a C array with one initialiser per byte and then gzip's
 * the blob — fine for index.html, but a raw script the web layer can stream
 * straight to the browser is simpler and keeps it as one plaintext blob the
 * ELF content assertion (check-elf-gzip.py RAW_NEED) can grep directly.
 * `.incbin` splices the same bytes into .rodata with none of the C-array cost.
 *
 * ⚠️ The path is resolved by the ASSEMBLER, relative to the directory make runs
 * in (the project root), not to this file. Do not "fix" it to ../assets/...
 *
 * ⚠️ serve-pkg.exe (the PyInstaller one-file build, ~9.2 MB of bundled CPython)
 * is deliberately NOT embedded here any more — it would bloat the ELF to
 * ~10.8 MB for zero runtime gain. It is shipped via the GitHub Release instead,
 * and assets/index.html links to it as an external download (target=_blank).
 * Keep this file embedding only the .py; the .exe must never come back in.
 */
#include <stddef.h>

void asset_register(const char *path, const void *data, size_t size,
                    const char *mime, const char *encoding);

__asm__(
  ".section .rodata\n"
  ".balign 16\n"
  ".globl wfm_serve_pkg_py_start\n"
  "wfm_serve_pkg_py_start:\n"
  ".incbin \"assets/serve-pkg.py\"\n"
  ".globl wfm_serve_pkg_py_end\n"
  "wfm_serve_pkg_py_end:\n"
  ".previous\n");

extern const unsigned char wfm_serve_pkg_py_start[];
extern const unsigned char wfm_serve_pkg_py_end[];

__attribute__((constructor)) static void
embed_serve_pkg_constructor(void) {
  asset_register("/serve-pkg.py", wfm_serve_pkg_py_start,
                 (size_t)(wfm_serve_pkg_py_end - wfm_serve_pkg_py_start),
                 "application/octet-stream", 0);
}
