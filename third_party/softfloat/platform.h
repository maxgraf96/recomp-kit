/* platform.h - local build configuration for the vendored SoftFloat 3e.
 *
 * Not part of the upstream release: SoftFloat expects every build to supply
 * its own platform.h (upstream's build/<target>/platform.h). This one is the
 * Linux-x86_64-GCC template with three changes, all for runtime/x87.c:
 *
 *   - the options upstream passes on the compiler command line are defined
 *     here, so the files build with no flags of their own;
 *   - INLINE is `static inline`: with plain `inline` an -O0 build needs an
 *     external definition of every primitive that the inlining level removed;
 *   - THREAD_LOCAL makes the rounding mode, precision and exception flags
 *     per thread. runtime/x87.c sets them from the guest's control word on
 *     every call, so this only matters if two guest threads ever run x87
 *     code at the same time.
 *
 * Only <stdint.h> and <stdbool.h> are used: the library is freestanding. */
#ifndef RECOMP_SOFTFLOAT_PLATFORM_H
#define RECOMP_SOFTFLOAT_PLATFORM_H

#define LITTLEENDIAN 1
#define INLINE static inline
#define THREAD_LOCAL _Thread_local
#define SOFTFLOAT_FAST_INT64 1
#define SOFTFLOAT_FAST_DIV32TO16 1
#define SOFTFLOAT_FAST_DIV64TO32 1
#define INLINE_LEVEL 5
#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"

#endif
