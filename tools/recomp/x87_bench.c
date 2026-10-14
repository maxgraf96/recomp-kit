/* x87_bench.c - time the x87 arithmetic paths (tools/recomp/x87_bench.py).
 *
 * Compiled twice by the driver: once against the current runtime/x86.h
 * (BENCH_NEW) and once against the header from before the exact x87
 * (BENCH_OLD), each kernel making the calls the translator emits for the
 * instruction. Every operation goes through a context in memory, as
 * translated code keeps it, and stores its results to guest memory. */
#include <stdint.h>
#include <string.h>

#include "x86.h"

#ifdef BENCH_NEW
#define NAME(x) x##_new
/* FMUL/FADD m32 and m64 as emitted now */
#define ARITH_M(c, op, v) x87_arith_m(c, op, v)
#define FST32(c) x87_fst_f32(c)
#define SET_CW(c, cw) x87_set_cw(c, cw)
#else
#define NAME(x) x##_old
#define X87_MUL 0
#define X87_ADD 1
/* as emitted before: fset(c, 0, fx87(c, ST(c, 0) op v_)) */
#define ARITH_M(c, op, v)                                                                          \
    do {                                                                                           \
        double v_ = (v);                                                                           \
        if ((op) == X87_MUL)                                                                       \
            fset(c, 0, fx87(c, ST(c, 0) * v_));                                                    \
        else                                                                                       \
            fset(c, 0, fx87(c, ST(c, 0) + v_));                                                    \
    } while (0)
#define FST32(c) fto_float(c, ST(c, 0))
#define SET_CW(c, cw) x87_set_cw(c, cw)
#endif

static X86 ctx;

/* A dependency chain on ST(0): n times FMUL m64, FADD m64. */
double NAME(bench_chain)(uint16_t cw, const double *mul, const double *add, uint32_t len,
                         uint32_t n) {
    X86 *volatile cp = &ctx;
    X86 *c = cp;
    uint32_t i;
    memset(c, 0, sizeof *c);
    c->fpu_tag = 0xffffu;
    SET_CW(c, cw);
    fpush(c, 1.0);
    for (i = 0; i < n; i++) {
        ARITH_M(c, X87_MUL, mul[i % len]);
        ARITH_M(c, X87_ADD, add[i % len]);
    }
    return ST(c, 0);
}

/* The game's shape: per element FLD m32, FMUL m32, FADD m32, FSTP m32. */
void NAME(bench_stream)(uint16_t cw, uint32_t src, uint32_t dst, uint32_t len, uint32_t reps) {
    X86 *volatile cp = &ctx;
    X86 *c = cp;
    uint32_t r, i;
    memset(c, 0, sizeof *c);
    c->fpu_tag = 0xffffu;
    SET_CW(c, cw);
    for (r = 0; r < reps; r++)
        for (i = 0; i < len; i++) {
            fpush(c, (double)rdf32(src + 4 * i));
            ARITH_M(c, X87_MUL, (double)rdf32(src + 4 * ((i + 1) % len)));
            ARITH_M(c, X87_ADD, (double)rdf32(src + 4 * ((i + 2) % len)));
            wrf32(dst + 4 * i, FST32(c));
            fdrop(c);
        }
}
