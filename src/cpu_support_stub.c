/* PS5-only stub for the libgcc CPU model symbols.
 *
 * unrar's rijndael.cpp / system.cpp call __builtin_cpu_supports() to pick
 * AES-NI fast paths. Clang lowers that to a reference on __cpu_model (data)
 * and __cpu_indicator_init() (function), which the FreeBSD-style PS5
 * sysroot does not provide (no libgcc). This TU supplies both so the link
 * succeeds; the detection result is unused because we always build the
 * portable C path.
 *
 * Do NOT add this file to host/linux builds: libstdc++/libgcc already
 * define __cpu_model there and the symbols would collide. */
#if defined(__x86_64__) && !defined(__linux__) && !defined(_WIN32)

struct __cpu_model {
  int __cpu_vendor;
  int __cpu_type;
  int __cpu_subtype;
};

int __cpu_indicator_init(void) {
  return 0;
}

struct __cpu_model __cpu_model = { 0, 0, 0 };

#endif
