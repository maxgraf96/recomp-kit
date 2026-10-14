/* freestanding.h - the C library surface generated code uses, for a build
 * that has no C library: RECOMP_FREESTANDING.
 *
 * tools/recomp/bundle.py compiles a translation with
 *   clang -ffreestanding -nostdinc -isystem <clang resource dir>/include
 * so no SDK header is read, and links each module with ld64.lld into a
 * Mach-O bundle (-bundle -undefined dynamic_lookup, no libSystem stub) that
 * the host dlopen()s. x86.h, intrinsics.h and seh.h include this header
 * instead of <string.h>, <math.h> and <setjmp.h> in that mode.
 *
 * Everything below is either a compiler builtin (no library involved) or a
 * runtime entry point, recomp_libm_* (runtime/dispatch.cpp), which the host
 * process supplies when it loads the bundle; an exact-x87 math library can
 * replace those in one place. What remains of the C library is what every
 * freestanding C program may need from its environment: memcpy, memmove and
 * memset (the compiler emits calls to them for large copies whatever the
 * source says) and memcmp, plus setjmp, which must be the host's own because
 * the runtime longjmps to it. tools/recomp/tests/test_freestanding.py checks
 * that a compiled translation names nothing else.
 *
 * Only the names generated code and the headers it includes use are here;
 * a new libm call in x86.h needs its line below (the test says which). */
#ifndef RECOMP_FREESTANDING_H
#define RECOMP_FREESTANDING_H

/* The compiler's own headers: types and limits, no library. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ <string.h> */

#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, v, n) __builtin_memset((d), (v), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))
#define strlen(s) __builtin_strlen((s))

/* -------------------------------------------------------------- <math.h> */

#define NAN __builtin_nan("")
#define INFINITY __builtin_inf()
#define HUGE_VAL __builtin_huge_val()
/* Distinct values are all that matters: generated code only compares
 * fpclassify's result against these names. */
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x)                                                                              \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))

/* One instruction each on arm64 (frintm/p/z/i/x, fsqrt, fabs, fcvtas), and
 * exactly what the C library returns: these are correctly rounded IEEE
 * operations. -fno-math-errno keeps sqrt a single instruction. */
#define floor(x) __builtin_floor(x)
#define floorf(x) __builtin_floorf(x)
#define ceil(x) __builtin_ceil(x)
#define ceilf(x) __builtin_ceilf(x)
#define trunc(x) __builtin_trunc(x)
#define truncf(x) __builtin_truncf(x)
#define nearbyint(x) __builtin_nearbyint(x)
#define rint(x) __builtin_rint(x)
#define lround(x) __builtin_lround(x)
#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define copysign(x, y) __builtin_copysign((x), (y))

/* Library functions proper: the host's runtime implements them. */
double recomp_libm_fmod(double x, double y);
double recomp_libm_ldexp(double x, int e);
double recomp_libm_scalbn(double x, int e);
double recomp_libm_frexp(double x, int *e);
int recomp_libm_ilogb(double x);
float recomp_libm_nextafterf(float x, float y);
double recomp_libm_sin(double x);
double recomp_libm_cos(double x);
double recomp_libm_tan(double x);
double recomp_libm_atan2(double y, double x);
double recomp_libm_log2(double x);
double recomp_libm_log1p(double x);
double recomp_libm_expm1(double x);
#define fmod(x, y) recomp_libm_fmod((x), (y))
#define ldexp(x, e) recomp_libm_ldexp((x), (e))
#define scalbn(x, e) recomp_libm_scalbn((x), (e))
#define frexp(x, e) recomp_libm_frexp((x), (e))
#define ilogb(x) recomp_libm_ilogb(x)
#define nextafterf(x, y) recomp_libm_nextafterf((x), (y))
#define sin(x) recomp_libm_sin(x)
#define cos(x) recomp_libm_cos(x)
#define tan(x) recomp_libm_tan(x)
#define atan2(y, x) recomp_libm_atan2((y), (x))
#define log2(x) recomp_libm_log2(x)
#define log1p(x) recomp_libm_log1p(x)
#define expm1(x) recomp_libm_expm1(x)

/* ------------------------------------------------------------ <setjmp.h> */

/* The host's setjmp: the runtime longjmps to the buffer it hands out
 * (recomp_seh_frame_enter, recomp_setjmp_prepare), so the two must be the
 * same implementation. Generated code only passes those buffers on and
 * never allocates one, so this size is not storage; it is at least the
 * platform's (Darwin arm64: 48 ints). */
#if defined(__APPLE__)
typedef int jmp_buf[64];
int setjmp(jmp_buf env) __attribute__((returns_twice));
#else
#error "RECOMP_FREESTANDING targets Mach-O bundles (tools/recomp/bundle.py)"
#endif

#ifdef __cplusplus
}
#endif
#endif /* RECOMP_FREESTANDING_H */
