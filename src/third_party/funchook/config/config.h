/* Hand-written replacement for funchook cmake_config.h.in (ReadyUp vendored build).
 * Target: x86_64 Linux (glibc), distorm disassembler backend. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* glibc with _GNU_SOURCE provides the GNU-specific strerror_r returning char*. */
#define GNU_SPECIFIC_STRERROR_R 1
#define DISASM_DISTORM 1
#define SIZEOF_VOID_P 8
