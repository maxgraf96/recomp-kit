/* x87.c - the exact x87's out-of-line half.
 *
 * runtime/x86.h keeps the cases a program meets all the time inline: double
 * arithmetic at 53-bit precision and round-to-nearest, and loads, stores,
 * compares and integer conversions of double registers. Everything else
 * lands here and is done exactly on 80-bit values: other precisions and
 * rounding modes, results outside the double's normal range, NaN operands,
 * registers that hold extended values, and the rarer instructions.
 *
 * Rounding arithmetic is Berkeley SoftFloat 3e (third_party/softfloat), with
 * its rounding precision set from PC, which keeps the 15-bit exponent range
 * as x87 does. Operand classes, NaN selection and the instructions SoftFloat
 * has no notion of (FPREM's partial remainder, FXTRACT, FSCALE, FXAM, the
 * stores' integer indefinite) are done here from the Intel SDM, checked
 * against real hardware (tools/recomp/tests/test_x87_conformance.py).
 *
 * Exception flags are set where SoftFloat reports them, which covers what
 * the fast path never computes only partly; C1 after arithmetic is not
 * modelled. Both are best effort: they do not change any result. */
#include <fenv.h>
#include <stdio.h>
#include <string.h>

#include "x86.h"

#include "../third_party/softfloat/platform.h"
#include "../third_party/softfloat/internals.h"
#include "../third_party/softfloat/specialize.h"
#include "../third_party/softfloat/softfloat.h"

#define SW_IE 0x0001u
#define SW_DE 0x0002u
#define SW_ZE 0x0004u
#define SW_OE 0x0008u
#define SW_UE 0x0010u
#define SW_PE 0x0020u
#define SW_C0 0x0100u
#define SW_C1 0x0200u
#define SW_C2 0x0400u
#define SW_C3 0x4000u
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

#define SIG_INT UINT64_C(0x8000000000000000)   /* the explicit integer bit */
#define SIG_QUIET UINT64_C(0x4000000000000000) /* a NaN's quiet bit */

static X87Ext x80(uint16_t signExp, uint64_t signif) {
    X87Ext v;
    v.signif = signif;
    v.signExp = signExp;
    return v;
}
static extFloat80_t to_sf(X87Ext v) {
    extFloat80_t r;
    r.signif = v.signif;
    r.signExp = v.signExp;
    return r;
}
static X87Ext from_sf(extFloat80_t v) {
    return x80(v.signExp, v.signif);
}

static const X87Ext X80_INDEFINITE = {UINT64_C(0xc000000000000000), 0xffffu};

/* The calling thread's counts (x87_stats in x86.h). */
static _Thread_local X87Stats stats;

X87Stats *x87_stats(void) {
    return &stats;
}

/* ---------------------------------------------------------- classes --- */

/* Operand classes, ordered so that everything from XC_QNAN up makes an
 * operation's result a NaN. Unsupported is every encoding with a nonzero
 * exponent and a clear integer bit (unnormal, pseudo-infinity, pseudo-NaN):
 * x87 has rejected those as invalid since the 387. A zero exponent with the
 * integer bit set (a pseudo-denormal) is a denormal of exponent -16382. */
enum { XC_ZERO, XC_DENORMAL, XC_NORMAL, XC_INF, XC_QNAN, XC_SNAN, XC_UNSUPPORTED };

static int x80_class(X87Ext v) {
    unsigned e = v.signExp & 0x7fffu;
    uint64_t m = v.signif;
    if (e == 0)
        return m ? XC_DENORMAL : XC_ZERO;
    if (!(m & SIG_INT))
        return XC_UNSUPPORTED;
    if (e == 0x7fffu) {
        if (!(m << 1))
            return XC_INF;
        return (m & SIG_QUIET) ? XC_QNAN : XC_SNAN;
    }
    return XC_NORMAL;
}

/* The tag x87 gives an 80-bit value. */
static unsigned x80_tag(X87Ext v) {
    switch (x80_class(v)) {
    case XC_ZERO:
        return FTAG_ZERO;
    case XC_NORMAL:
        return FTAG_VALID;
    default:
        return FTAG_SPECIAL;
    }
}

/* -------------------------------------------------- double <-> 80-bit --- */

/* A double's 80-bit value, exactly; a signalling NaN stays signalling. */
static X87Ext x80_from_f64(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    uint16_t sign = (uint16_t)((b >> 48) & 0x8000u);
    unsigned e = (unsigned)(b >> 52) & 0x7ffu;
    uint64_t f = b & UINT64_C(0x000fffffffffffff);
    if (e == 0x7ffu)
        return x80((uint16_t)(sign | 0x7fffu), SIG_INT | f << 11);
    if (e == 0) {
        if (!f)
            return x80(sign, 0);
        /* subnormal: f * 2^-1074 is normal in the extended range */
        unsigned lz = (unsigned)__builtin_clzll(f);
        return x80((uint16_t)(sign | (16383u - 1011u - lz)), f << lz);
    }
    return x80((uint16_t)(sign | (e - 1023u + 16383u)), SIG_INT | f << 11);
}

/* The double equal to v, if there is one. NaNs qualify when their payload
 * fits; unsupported encodings never do. */
static int x80_to_f64_exact(X87Ext v, double *out) {
    uint64_t sign = (uint64_t)(v.signExp >> 15) << 63, b;
    int e = v.signExp & 0x7fff;
    uint64_t m = v.signif;
    if (e == 0) {
        if (m)
            return 0;
        b = sign;
    } else if (!(m & SIG_INT)) {
        return 0;
    } else if (e == 0x7fff) {
        if (m & 0x7ffu)
            return 0;
        b = sign | UINT64_C(0x7ff0000000000000) | ((m << 1) >> 12);
    } else {
        int E = e - 16383;
        if (E > 1023)
            return 0;
        if (E >= -1022) {
            if (m & 0x7ffu)
                return 0;
            b = sign | (uint64_t)(E + 1023) << 52 | ((m << 1) >> 12);
        } else {
            int sh = -1011 - E; /* to the subnormal grid, 2^-1074 */
            if (sh >= 64 || (m & ((UINT64_C(1) << sh) - 1)))
                return 0;
            b = sign | (m >> sh);
        }
    }
    memcpy(out, &b, 8);
    return 1;
}

/* v rounded to the nearest double, for st[] beside an extended slot. Leaves
 * SoftFloat's state as it found it. */
static double x80_round_f64(X87Ext v) {
    if (x80_class(v) == XC_UNSUPPORTED)
        return x87_indefinite();
    uint_fast8_t mode = softfloat_roundingMode, flags = softfloat_exceptionFlags;
    softfloat_roundingMode = softfloat_round_near_even;
    float64_t r = extF80_to_f64(to_sf(v));
    softfloat_roundingMode = mode;
    softfloat_exceptionFlags = flags;
    double d;
    memcpy(&d, &r.v, 8);
    return d;
}

/* -------------------------------------------------------- registers --- */

static X87Ext reg_get(const X86 *c, unsigned phys) {
    if (c->st_kind[phys])
        return c->stx[phys];
    return x80_from_f64(c->st[phys]);
}

/* Write a physical slot in whichever representation is exact, and retag. */
static void reg_put(X86 *c, unsigned phys, X87Ext v) {
    double d;
    if (x80_to_f64_exact(v, &d)) {
        if (c->st_kind[phys])
            x87_kind_clear(c, phys);
        c->st[phys] = d;
        ftag_put(c, phys, ftag_classify(d));
        return;
    }
    if (!c->st_kind[phys])
        stats.ext_created++;
    x87_kind_set(c, phys, v);
    if (c->ext_live > stats.ext_live_max)
        stats.ext_live_max = c->ext_live;
    c->st[phys] = x80_round_f64(v);
    ftag_put(c, phys, x80_tag(v));
}

static void push_ext(X86 *c, X87Ext v) {
    c->fpu_top = (c->fpu_top - 1u) & 7u;
    reg_put(c, c->fpu_top, v);
}

static unsigned top_slot(const X86 *c, unsigned i) {
    return (c->fpu_top + i) & 7u;
}

/* ------------------------------------------------------ environment --- */

int x87_host_env_ok(void) {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    /* FIZ (0), AH (1), RMode (23:22), FZ (24), DN (25) */
    return (fpcr & UINT64_C(0x3c00003)) == 0;
#else
    return fegetround() == FE_TONEAREST;
#endif
}

static void host_env_restore(void) {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~UINT64_C(0x3c00003);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#else
    fesetround(FE_TONEAREST);
#endif
}

static _Thread_local int env_checked;

/* Every out-of-line entry: count it, and check the host environment once
 * per thread. */
static void x87_enter(X86 *c, unsigned kind) {
    (void)c;
    stats.count[kind]++;
    if (RECOMP_UNLIKELY(!env_checked)) {
        env_checked = 1;
        if (!x87_host_env_ok()) {
            fprintf(stderr, "[recomp] x87: host floating-point environment is not "
                            "round-to-nearest without flush-to-zero; restoring it\n");
            host_env_restore();
        }
    }
}

/* SoftFloat's rounding from the control word: RC always, PC only for the
 * instructions it applies to (FADD/FSUB/FMUL/FDIV, their integer and popping
 * forms, and FSQRT). PC=01 is reserved; it rounds like PC=11. */
static void sf_begin(const X86 *c, int pc) {
    static const uint_fast8_t modes[4] = {softfloat_round_near_even, softfloat_round_min,
                                          softfloat_round_max, softfloat_round_minMag};
    static const uint_fast8_t precision[4] = {32, 80, 64, 80};
    softfloat_roundingMode = modes[(c->fpu_cw >> 10) & 3u];
    extF80_roundingPrecision = pc ? precision[(c->fpu_cw >> 8) & 3u] : 80;
    softfloat_exceptionFlags = 0;
}

static void sf_end(X86 *c) {
    unsigned f = softfloat_exceptionFlags, sw = 0;
    if (f & softfloat_flag_invalid)
        sw |= SW_IE;
    if (f & softfloat_flag_infinite)
        sw |= SW_ZE;
    if (f & softfloat_flag_overflow)
        sw |= SW_OE;
    if (f & softfloat_flag_underflow)
        sw |= SW_UE;
    if (f & softfloat_flag_inexact)
        sw |= SW_PE;
    c->fpu_sw |= (uint16_t)sw;
}

/* ------------------------------------------------------------- NaNs --- */

/* The result of an operation with a NaN or unsupported operand (SDM table
 * 4-7): an unsupported operand counts as the QNaN indefinite; of two NaNs a
 * quiet one beats a signalling one, otherwise the larger significand wins and
 * on a tie the positive one; the result is quiet. Raises IE for a signalling
 * or unsupported operand. */
static X87Ext nan_result(X86 *c, const X87Ext *v, const int *cls, int n) {
    X87Ext nan[2];
    int quiet[2], k = 0, i;
    for (i = 0; i < n; i++) {
        if (cls[i] == XC_UNSUPPORTED) {
            nan[k] = X80_INDEFINITE;
            quiet[k++] = 1;
            c->fpu_sw |= SW_IE;
        } else if (cls[i] == XC_QNAN || cls[i] == XC_SNAN) {
            nan[k] = v[i];
            quiet[k++] = cls[i] == XC_QNAN;
            if (cls[i] == XC_SNAN)
                c->fpu_sw |= SW_IE;
        }
    }
    X87Ext best = nan[0];
    if (k == 2) {
        if (quiet[0] != quiet[1]) {
            best = quiet[0] ? nan[0] : nan[1];
        } else {
            uint64_t f0 = nan[0].signif & ~SIG_INT, f1 = nan[1].signif & ~SIG_INT;
            if (f1 > f0 || (f1 == f0 && !(nan[1].signExp & 0x8000u)))
                best = nan[1];
        }
    }
    best.signif |= SIG_QUIET;
    return best;
}

/* ------------------------------------------------------- arithmetic --- */

static X87Ext arith_exact(X86 *c, int op, X87Ext a, X87Ext b) {
    int cls[2] = {x80_class(a), op == X87_SQRT ? XC_ZERO : x80_class(b)};
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        X87Ext v[2] = {a, b};
        return nan_result(c, v, cls, 2);
    }
    if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    extFloat80_t x = to_sf(a), y = to_sf(b), r;
    sf_begin(c, 1);
    switch (op) {
    case X87_ADD:
        r = extF80_add(x, y);
        break;
    case X87_SUB:
        r = extF80_sub(x, y);
        break;
    case X87_SUBR:
        r = extF80_sub(y, x);
        break;
    case X87_MUL:
        r = extF80_mul(x, y);
        break;
    case X87_DIV:
        r = extF80_div(x, y);
        break;
    case X87_DIVR:
        r = extF80_div(y, x);
        break;
    default:
        r = extF80_sqrt(x);
        break;
    }
    sf_end(c);
    return from_sf(r);
}

/* TwoSum: the exact error of the rounded sum s = a + b. Needs strict double
 * evaluation, which -ffp-contract=off guarantees. */
static double two_sum_err(double a, double b, double s) {
    double bb = s - a;
    return (a - (s - bb)) + (b - bb);
}

/* The PC=24 shortcut: the x87 result is the exact result rounded once to 24
 * bits, which rounding the double result again gives, except where the
 * double result sits exactly halfway between two 24-bit values: then the
 * exact result's side of it decides, which TwoSum tells for a sum and the
 * exact path settles otherwise. Results that are zero by underflow, tiny,
 * NaN or beyond the double range take the exact path too. Returns 0 then. */
static int pc24_settle(int op, double a, double b, double *out) {
    double r = x87_host_op(op, a, b);
    uint64_t bits, low;
    memcpy(&bits, &r, 8);
    unsigned e = (unsigned)(bits >> 52) & 0x7ffu;
    int sum = op == X87_ADD || op == X87_SUB || op == X87_SUBR;
    if (e == 0) {
        if (r != 0.0)
            return 0;
        /* exact zeros only: a + -a, or a zero factor or dividend */
        if (!(sum || (op == X87_MUL && (a == 0.0 || b == 0.0)) || (op == X87_DIV && a == 0.0) ||
              (op == X87_DIVR && b == 0.0) || op == X87_SQRT))
            return 0;
        *out = r;
        return 1;
    }
    if (e == 0x7ffu)
        return 0;
    /* |r| == DBL_MIN may have rounded up from the subnormal range */
    if (!sum && op != X87_SQRT &&
        (bits & UINT64_C(0x7fffffffffffffff)) == UINT64_C(0x0010000000000000))
        return 0;
    low = bits & UINT64_C(0x1fffffff);
    int up;
    if (low == UINT64_C(0x10000000)) {
        double err;
        if (!sum)
            return 0;
        if (op == X87_ADD)
            err = two_sum_err(a, b, r);
        else if (op == X87_SUB)
            err = two_sum_err(a, -b, r);
        else
            err = two_sum_err(b, -a, r);
        if (err != err || err - err != 0.0)
            return 0;
        up = err == 0.0 ? (int)((bits >> 29) & 1u) : (err > 0.0) == (r > 0.0);
    } else {
        up = low > UINT64_C(0x10000000);
    }
    bits &= ~UINT64_C(0x1fffffff);
    if (up)
        bits += UINT64_C(0x20000000);
    if (((bits >> 52) & 0x7ffu) == 0x7ffu)
        return 0;
    memcpy(out, &bits, 8);
    return 1;
}

/* The kind of an arithmetic entry: which path sent it out of line, and for
 * the fast path's guard whether its result was a NaN (a NaN operand or an
 * invalid operation) or out of range. */
static void arith_enter(X86 *c, int op, double a, double b) {
    unsigned mode = x87_mode(c), kind = X87_STAT_EXACT;
    if (mode == X87_MODE_PC24) {
        kind = X87_STAT_PC24;
    } else if (mode == X87_MODE_FAST) {
        double r = x87_host_op(op, a, b);
        kind = r != r ? X87_STAT_GUARD_NAN : X87_STAT_GUARD_RANGE;
    }
    x87_enter(c, kind);
}

void x87_arith_slow_st(X86 *c, int op, unsigned pd, unsigned ps) {
    double r;
    arith_enter(c, op, c->st[pd], c->st[ps]);
    if (x87_mode(c) == X87_MODE_PC24 && pc24_settle(op, c->st[pd], c->st[ps], &r)) {
        c->st[pd] = r;
        ftag_put(c, pd, ftag_classify(r));
        return;
    }
    reg_put(c, pd, arith_exact(c, op, reg_get(c, pd), reg_get(c, ps)));
}

void x87_arith_slow_m(X86 *c, int op, double m) {
    unsigned p = top_slot(c, 0);
    double r;
    arith_enter(c, op, c->st[p], m);
    if (x87_mode(c) == X87_MODE_PC24 && pc24_settle(op, c->st[p], m, &r)) {
        c->st[p] = r;
        ftag_put(c, p, ftag_classify(r));
        return;
    }
    /* the operand as it is: a signalling NaN loses to a quiet ST(0), and
     * arith_exact raises IE for it */
    reg_put(c, p, arith_exact(c, op, reg_get(c, p), x80_from_f64(m)));
}

void x87_fsqrt_slow(X86 *c) {
    unsigned p = top_slot(c, 0);
    double r;
    arith_enter(c, X87_SQRT, c->st[p], 0.0);
    if (x87_mode(c) == X87_MODE_PC24 && pc24_settle(X87_SQRT, c->st[p], 0.0, &r)) {
        c->st[p] = r;
        ftag_put(c, p, ftag_classify(r));
        return;
    }
    X87Ext a = reg_get(c, p);
    reg_put(c, p, arith_exact(c, X87_SQRT, a, a));
}

/* ----------------------------------------------------------- stores --- */

int64_t x87_fist_slow(X86 *c, unsigned bits) {
    int64_t lo = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
    int64_t hi = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
    x87_enter(c, X87_STAT_CONVERT);
    X87Ext v = reg_get(c, top_slot(c, 0));
    int cls = x80_class(v);
    if (cls >= XC_INF) {
        c->fpu_sw |= SW_IE;
        return lo;
    }
    if (cls == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    sf_begin(c, 0);
    int64_t n = extF80_to_i64(to_sf(v), softfloat_roundingMode, true);
    int invalid = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
    sf_end(c);
    if (invalid || n < lo || n > hi) {
        c->fpu_sw |= SW_IE;
        return lo;
    }
    return n;
}

float x87_fst_f32_slow(X86 *c) {
    float f;
    x87_enter(c, X87_STAT_CONVERT);
    X87Ext v = reg_get(c, top_slot(c, 0));
    if (x80_class(v) == XC_UNSUPPORTED) {
        uint32_t indefinite = 0xffc00000u;
        c->fpu_sw |= SW_IE;
        memcpy(&f, &indefinite, 4);
        return f;
    }
    sf_begin(c, 0);
    float32_t r = extF80_to_f32(to_sf(v));
    sf_end(c);
    memcpy(&f, &r.v, 4);
    return f;
}

double x87_fst_f64_slow(X86 *c) {
    x87_enter(c, X87_STAT_CONVERT);
    X87Ext v = reg_get(c, top_slot(c, 0));
    if (x80_class(v) == XC_UNSUPPORTED) {
        c->fpu_sw |= SW_IE;
        return x87_indefinite();
    }
    sf_begin(c, 0);
    float64_t r = extF80_to_f64(to_sf(v));
    sf_end(c);
    double d;
    memcpy(&d, &r.v, 8);
    return d;
}

/* FST/FSTP m80 stores the register exactly. Of an empty register it stores
 * the indefinite, as x87's masked stack underflow does (IE, SF). */
void x87_fst_f80(X86 *c, uint32_t a) {
    X87Ext v = reg_get(c, top_slot(c, 0));
    stats.count[X87_STAT_OTHER]++;
    if (ftag_of(c, top_slot(c, 0)) == FTAG_EMPTY) {
        c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_C1) | SW_IE | 0x0040u);
        v = X80_INDEFINITE;
    }
    wr64(a, v.signif);
    wr16(a + 8, v.signExp);
}

/* ------------------------------------------------------------ loads --- */

void x87_fild64_slow(X86 *c, int64_t v) {
    stats.count[X87_STAT_CONVERT]++;
    push_ext(c, from_sf(i64_to_extF80(v)));
}

/* FLD m80 is exact for every encoding, a signalling NaN included. */
void x87_fld_f80(X86 *c, uint32_t a) {
    stats.count[X87_STAT_OTHER]++;
    push_ext(c, x80(rd16(a + 8), rd64(a)));
}

/* FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2: the constant's 64-bit significand
 * rounded per RC. None of the five lies near a halfway point, so nearest is
 * one of the two directed results: `up` says which, and the up/down pair
 * differs in the last bit. FLD1 and FLDZ are exact and never come here. */
void x87_fldconst(X86 *c, unsigned which) {
    stats.count[X87_STAT_OTHER]++;
    static const struct {
        uint16_t signExp;
        uint64_t down; /* truncated */
        int nearest_up;
    } table[] = {
        {0x4000u, UINT64_C(0xd49a784bcd1b8afe), 0}, /* log2(10) */
        {0x3fffu, UINT64_C(0xb8aa3b295c17f0bb), 1}, /* log2(e) */
        {0x4000u, UINT64_C(0xc90fdaa22168c234), 1}, /* pi */
        {0x3ffdu, UINT64_C(0x9a209a84fbcff798), 1}, /* log10(2) */
        {0x3ffeu, UINT64_C(0xb17217f7d1cf79ab), 1}, /* ln(2) */
    };
    unsigned rc = (c->fpu_cw >> 10) & 3u;
    int up = rc == 0 ? table[which].nearest_up : rc == 2; /* the constants are positive */
    push_ext(c, x80(table[which].signExp, table[which].down + (uint64_t)up));
}

/* --------------------------------------------------------- compares --- */

/* Signed comparison of two non-NaN 80-bit values: -1, 0 or 1. The two zeros
 * are equal; a denormal compares at exponent 1, where its significand sits. */
static int x80_compare(X87Ext a, X87Ext b) {
    unsigned ea = a.signExp & 0x7fffu, eb = b.signExp & 0x7fffu;
    int za = x80_class(a) == XC_ZERO, zb = x80_class(b) == XC_ZERO;
    int sa = (a.signExp >> 15) && !za, sb = (b.signExp >> 15) && !zb;
    int mag;
    if (za && zb)
        return 0;
    if (sa != sb)
        return sa ? -1 : 1;
    ea = ea ? ea : 1u;
    eb = eb ? eb : 1u;
    if (za || zb)
        mag = za ? -1 : 1;
    else if (ea != eb)
        mag = ea < eb ? -1 : 1;
    else
        mag = a.signif == b.signif ? 0 : a.signif < b.signif ? -1 : 1;
    return sa ? -mag : mag;
}

/* C3 C2 C0 for ST(0) against b, with IE as FCOM or FUCOM raise it. */
static unsigned compare_cc(X86 *c, X87Ext a, X87Ext b, int quiet) {
    int ca = x80_class(a), cb = x80_class(b);
    if (ca >= XC_QNAN || cb >= XC_QNAN) {
        if (!quiet || ca >= XC_SNAN || cb >= XC_SNAN)
            c->fpu_sw |= SW_IE;
        return SW_C3 | SW_C2 | SW_C0;
    }
    if (ca == XC_DENORMAL || cb == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    int r = x80_compare(a, b);
    return r < 0 ? SW_C0 : r == 0 ? SW_C3 : 0u;
}

void x87_fcom_slow(X86 *c, unsigned pb, double m, int mem, int quiet) {
    stats.count[X87_STAT_COMPARE]++;
    X87Ext a = reg_get(c, top_slot(c, 0));
    X87Ext b = mem ? x80_from_f64(m) : reg_get(c, pb);
    unsigned cc = compare_cc(c, a, b, quiet);
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | cc);
}

void x87_fcomi_slow(X86 *c, unsigned pb, int quiet) {
    stats.count[X87_STAT_COMPARE]++;
    unsigned cc = compare_cc(c, reg_get(c, top_slot(c, 0)), reg_get(c, pb), quiet);
    c->eflags_of = c->eflags_sf = c->eflags_af = 0;
    c->eflags_zf = (cc & SW_C3) != 0;
    c->eflags_pf = (cc & SW_C2) != 0;
    c->eflags_cf = (cc & SW_C0) != 0;
}

void x87_fxam_ext(X86 *c) {
    stats.count[X87_STAT_COMPARE]++;
    X87Ext v = c->stx[top_slot(c, 0)];
    unsigned sw = c->fpu_sw & ~SW_CC;
    if (v.signExp & 0x8000u)
        sw |= SW_C1;
    switch (x80_class(v)) {
    case XC_ZERO:
        sw |= SW_C3;
        break;
    case XC_DENORMAL:
        sw |= SW_C3 | SW_C2;
        break;
    case XC_NORMAL:
        sw |= SW_C2;
        break;
    case XC_INF:
        sw |= SW_C2 | SW_C0;
        break;
    case XC_QNAN:
    case XC_SNAN:
        sw |= SW_C0;
        break;
    default:
        break; /* unsupported: 000 */
    }
    c->fpu_sw = (uint16_t)sw;
}

/* ---------------------------------------------------- non-arithmetic --- */

/* FRNDINT: round to an integer per RC; PC does not apply. */
void x87_frndint(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_OTHER);
    X87Ext v = reg_get(c, p);
    int cls = x80_class(v);
    if (cls >= XC_QNAN) {
        reg_put(c, p, nan_result(c, &v, &cls, 1));
        return;
    }
    if (cls == XC_ZERO || cls == XC_INF)
        return;
    if (cls == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    sf_begin(c, 0);
    extFloat80_t r = extF80_roundToInt(to_sf(v), softfloat_roundingMode, true);
    sf_end(c);
    reg_put(c, p, from_sf(r));
}

/* A finite nonzero value's significand with the integer bit set, and its
 * biased exponent, which a denormal takes below 1. */
static uint64_t x80_normalize(X87Ext v, int32_t *exp) {
    int32_t e = v.signExp & 0x7fff;
    uint64_t m = v.signif;
    if (e == 0) {
        int sh = __builtin_clzll(m);
        m <<= sh;
        e = 1 - sh;
    }
    *exp = e;
    return m;
}

/* FSCALE: ST(0) * 2^trunc(ST(1)), rounded per RC at 64 bits only when the
 * result leaves the extended range. */
void x87_fscale(X86 *c) {
    unsigned p0 = top_slot(c, 0), p1 = top_slot(c, 1);
    x87_enter(c, X87_STAT_OTHER);
    X87Ext v[2] = {reg_get(c, p0), reg_get(c, p1)};
    int cls[2] = {x80_class(v[0]), x80_class(v[1])};
    uint16_t sign = v[0].signExp & 0x8000u;
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        reg_put(c, p0, nan_result(c, v, cls, 2));
        return;
    }
    if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    if (cls[1] == XC_INF) {
        int down = v[1].signExp >> 15;
        if ((!down && cls[0] == XC_ZERO) || (down && cls[0] == XC_INF)) {
            c->fpu_sw |= SW_IE;
            reg_put(c, p0, X80_INDEFINITE);
        } else if (cls[0] != XC_ZERO && cls[0] != XC_INF) {
            reg_put(c, p0, down ? x80(sign, 0) : x80((uint16_t)(sign | 0x7fffu), SIG_INT));
        }
        return;
    }
    if (cls[0] == XC_ZERO || cls[0] == XC_INF)
        return;
    /* n = trunc(ST(1)); anything beyond +-2^17 under- or overflows the same */
    int32_t n = 0;
    if (cls[1] == XC_NORMAL) {
        int E = (v[1].signExp & 0x7fff) - 16383;
        if (E > 17)
            n = 1 << 18;
        else if (E >= 0)
            n = (int32_t)(v[1].signif >> (63 - E));
        if (v[1].signExp & 0x8000u)
            n = -n;
    }
    int32_t e;
    uint64_t m = x80_normalize(v[0], &e);
    sf_begin(c, 0);
    extFloat80_t r = softfloat_roundPackToExtF80(sign != 0, e + n, m, 0, 80);
    sf_end(c);
    reg_put(c, p0, from_sf(r));
}

/* FXTRACT: ST(0) becomes its unbiased exponent and the significand, with the
 * exponent of 1.0, is pushed. A zero gives -infinity (ZE), an infinity
 * +infinity, a NaN itself twice. */
void x87_fxtract(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_OTHER);
    X87Ext v = reg_get(c, p), expo, sig;
    int cls = x80_class(v);
    if (cls >= XC_QNAN) {
        expo = sig = nan_result(c, &v, &cls, 1);
    } else if (cls == XC_ZERO) {
        c->fpu_sw |= SW_ZE;
        expo = x80(0xffffu, SIG_INT);
        sig = v;
    } else if (cls == XC_INF) {
        expo = x80(0x7fffu, SIG_INT);
        sig = v;
    } else {
        int32_t e;
        if (cls == XC_DENORMAL)
            c->fpu_sw |= SW_DE;
        uint64_t m = x80_normalize(v, &e);
        expo = from_sf(i64_to_extF80(e - 16383));
        sig = x80((uint16_t)((v.signExp & 0x8000u) | 0x3fffu), m);
    }
    reg_put(c, p, expo);
    push_ext(c, sig);
}

/* Pack a remainder: magnitude r * 2^(e - 16383 - 63) for a biased exponent e
 * that may be below 1. Remainders are exact, so a denormal result loses no
 * bits: r is a multiple of the divisor's quantum, never finer than 2^-16445. */
static X87Ext pack_exact(uint16_t sign, int32_t e, uint64_t r) {
    if (!r)
        return x80(sign, 0);
    int sh = __builtin_clzll(r);
    r <<= sh;
    e -= sh;
    if (e >= 1)
        return x80((uint16_t)(sign | (uint16_t)e), r);
    return x80(sign, r >> (1 - e));
}

/* FPREM (truncating quotient) and FPREM1 (IEEE, nearest-even quotient).
 *
 * With the exponents D >= 64 apart, x87 reduces only partly and sets C2 for
 * the guest to loop: it takes the quotient by ST(1) * 2^(D - N), truncated,
 * for an "implementation-dependent" N in 32..63. Real hardware (an Intel Core
 * Ultra 9 275HX, every partial vector in the corpus) uses N = 32 + D mod 32:
 * whole 32-bit chunks, leaving 32 to 63 bits of exponent difference. Both
 * instructions reduce that way. A complete reduction sets C0, C3 and C1 to
 * the quotient's bits 2, 1 and 0. */
void x87_fprem(X86 *c, int ieee) {
    unsigned p0 = top_slot(c, 0), p1 = top_slot(c, 1);
    x87_enter(c, X87_STAT_OTHER);
    X87Ext v[2] = {reg_get(c, p0), reg_get(c, p1)};
    int cls[2] = {x80_class(v[0]), x80_class(v[1])};
    uint16_t sign = v[0].signExp & 0x8000u;
    c->fpu_sw &= (uint16_t)~SW_CC;
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        reg_put(c, p0, nan_result(c, v, cls, 2));
        return;
    }
    if (cls[0] == XC_INF || cls[1] == XC_ZERO) {
        c->fpu_sw |= SW_IE;
        reg_put(c, p0, X80_INDEFINITE);
        return;
    }
    if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    if (cls[0] == XC_ZERO || cls[1] == XC_INF)
        return; /* already reduced, quotient 0 */
    int32_t ea, eb;
    uint64_t ma = x80_normalize(v[0], &ea), mb = x80_normalize(v[1], &eb);
    int32_t d = ea - eb;
    if (d >= 64) {
        int32_t shift = 32 * ((d >> 5) - 1);
        unsigned __int128 num = (unsigned __int128)ma << (d - shift);
        reg_put(c, p0, pack_exact(sign, eb + shift, (uint64_t)(num % mb)));
        c->fpu_sw |= SW_C2;
        return;
    }
    uint64_t q, r;
    if (d >= 0) {
        unsigned __int128 num = (unsigned __int128)ma << d;
        q = (uint64_t)(num / mb);
        r = (uint64_t)(num % mb);
    } else {
        /* |ST(0)| < |ST(1)|: quotient 0, unless FPREM1 rounds it up to 1,
         * which needs |ST(0)| > |ST(1)| / 2, so d == -1 and ma > mb */
        if (ieee && d == -1 && ma > mb) {
            uint64_t rest = (uint64_t)(((unsigned __int128)mb << 1) - ma);
            reg_put(c, p0, pack_exact((uint16_t)(sign ^ 0x8000u), eb - 1, rest));
            c->fpu_sw |= SW_C1;
        }
        return;
    }
    if (ieee && (r > mb - r || (r == mb - r && (q & 1)))) {
        r = mb - r;
        q++;
        sign ^= 0x8000u;
    }
    reg_put(c, p0, pack_exact(sign, eb, r));
    if (q & 4)
        c->fpu_sw |= SW_C0;
    if (q & 2)
        c->fpu_sw |= SW_C3;
    if (q & 1)
        c->fpu_sw |= SW_C1;
}

/* FBSTP: ST(0) rounded per RC to an integer, stored as 18 packed BCD digits
 * with the sign in bit 7 of byte 9, then popped. Beyond 18 digits, or a NaN
 * or infinity, stores the BCD indefinite (IE). */
void x87_fbstp(X86 *c, uint32_t a) {
    uint8_t out[10];
    unsigned i;
    x87_enter(c, X87_STAT_CONVERT);
    X87Ext v = reg_get(c, top_slot(c, 0));
    int cls = x80_class(v);
    int64_t n = 0;
    int bad = cls >= XC_INF;
    if (!bad) {
        sf_begin(c, 0);
        n = extF80_to_i64(to_sf(v), softfloat_roundingMode, true);
        bad = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
        sf_end(c);
        bad = bad || n <= -INT64_C(1000000000000000000) || n >= INT64_C(1000000000000000000);
    }
    memset(out, 0, sizeof out);
    if (bad) {
        static const uint8_t indefinite[10] = {0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0xff};
        c->fpu_sw |= SW_IE;
        memcpy(out, indefinite, 10);
    } else {
        uint64_t m = n < 0 ? (uint64_t)-n : (uint64_t)n;
        for (i = 0; i < 9; ++i) {
            uint8_t lo = (uint8_t)(m % 10);
            m /= 10;
            uint8_t hi = (uint8_t)(m % 10);
            m /= 10;
            out[i] = (uint8_t)(hi << 4 | lo);
        }
        if (v.signExp & 0x8000u)
            out[9] = 0x80;
    }
    for (i = 0; i < 10; ++i)
        wr8(a + i, out[i]);
    fdrop(c);
}

/* FNSAVE m108: the 32-bit protected-mode layout. Control, status and tag
 * words each in their own dword, then the four exception pointers (FIP, FCS
 * with the opcode, FDP, FDS), which this model does not track and writes as
 * zero, then ST(0) through ST(7) as 80-bit values in stack order. FNSAVE
 * then reinitialises the FPU, which is why the CRT pairs it with FRSTOR. */
void x87_fnsave(X86 *c, uint32_t a) {
    unsigned i;
    wr32(a, c->fpu_cw);
    wr32(a + 4, fstsw(c));
    wr32(a + 8, c->fpu_tag);
    for (i = 12; i < 28; i += 4)
        wr32(a + i, 0);
    for (i = 0; i < 8; i++) {
        X87Ext v = reg_get(c, top_slot(c, i));
        wr64(a + 28 + 10 * i, v.signif);
        wr16(a + 28 + 10 * i + 8, v.signExp);
    }
    x87_finit(c);
}

/* FRSTOR m108: the inverse. TOP comes out of the saved status word and the
 * eight registers go back into the physical slots that TOP implies, so a
 * later ST(i) reads what FNSAVE wrote as ST(i). */
void x87_frstor(X86 *c, uint32_t a) {
    unsigned i;
    uint16_t sw = rd16(a + 4);
    x87_set_cw(c, rd16(a));
    c->fpu_top = (sw >> 11) & 7u;
    c->fpu_sw = (uint16_t)(sw & (uint16_t)~0x3800u);
    for (i = 0; i < 8; i++)
        reg_put(c, top_slot(c, i), x80(rd16(a + 28 + 10 * i + 8), rd64(a + 28 + 10 * i)));
    c->fpu_tag = rd16(a + 8);
    x87_kind_sync_tags(c);
}

/* --------------------------------------------------- transcendentals --- */

/* FSIN ... FYL2XP1. x87 hardware does not round these correctly: Intel and
 * AMD document them only to within an ulp, and results have changed between
 * CPU generations. What a recent Intel CPU produces is, within an ulp and
 * mostly exactly, the correctly rounded value of the function, with one
 * twist for FSIN, FCOS, FSINCOS and FPTAN: they reduce the argument with a
 * 66-bit pi (P66), so they compute f(x * pi / P66). That is what these
 * evaluate, in a 128-bit-significand format (XF below) whose error is far
 * below the final rounding to 64 bits per RC; PC does not apply. They are
 * therefore near the hardware, not equal to it - the conformance suite
 * reports their distance. Operands that are NaN, unsupported or outside an
 * instruction's domain get x87's results; FSIN, FCOS, FSINCOS and FPTAN leave
 * an operand of 2^63 or more unchanged and set C2. */

typedef unsigned __int128 u128;
#define X87_U128(hi, lo) (((u128)(hi) << 64) | (u128)(lo))

/* sign, exponent, and a significand with bit 127 set (or zero):
 * (-1)^sign * m * 2^(exp - 127), so m's leading bit is worth 2^exp. */
typedef struct {
    int sign;
    int32_t exp;
    u128 m;
} XF;

static int clz128(u128 m) {
    uint64_t hi = (uint64_t)(m >> 64);
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)m);
}

static XF xf_make(int sign, int32_t exp, u128 m) {
    XF r;
    r.sign = sign;
    if (!m) {
        r.exp = 0;
        r.m = 0;
        return r;
    }
    int lz = clz128(m);
    r.m = m << lz;
    r.exp = exp - lz;
    return r;
}

static XF xf_neg(XF a) {
    a.sign ^= 1;
    return a;
}

static XF xf_from_x80(X87Ext v) {
    int32_t e = v.signExp & 0x7fff;
    return xf_make(v.signExp >> 15, (e ? e : 1) - 16383, (u128)v.signif << 64);
}

static XF xf_from_int(int sign, uint64_t n) {
    return xf_make(sign, 127, (u128)n);
}

/* a * b, truncated to 128 bits. */
static XF xf_mul(XF a, XF b) {
    if (!a.m || !b.m)
        return xf_make(a.sign ^ b.sign, 0, 0);
    uint64_t ah = (uint64_t)(a.m >> 64), al = (uint64_t)a.m;
    uint64_t bh = (uint64_t)(b.m >> 64), bl = (uint64_t)b.m;
    u128 hh = (u128)ah * bh, hl = (u128)ah * bl, lh = (u128)al * bh, ll = (u128)al * bl;
    u128 mid = (u128)(uint64_t)hl + (uint64_t)lh + (uint64_t)(ll >> 64);
    u128 hi = hh + (hl >> 64) + (lh >> 64) + (mid >> 64);
    uint64_t lo_top = (uint64_t)mid; /* the next 64 bits below hi */
    int32_t exp = a.exp + b.exp + 1;
    if (!(hi >> 127)) {
        hi = (hi << 1) | (lo_top >> 63);
        exp--;
    }
    XF r = {a.sign ^ b.sign, exp, hi};
    return r;
}

/* a + b. Operands far apart lose the smaller one's low bits, which the
 * 128-bit significand affords. */
static XF xf_add(XF a, XF b) {
    if (!b.m)
        return a;
    if (!a.m)
        return b;
    if (a.exp < b.exp || (a.exp == b.exp && a.m < b.m)) {
        XF t = a;
        a = b;
        b = t;
    }
    int32_t d = a.exp - b.exp;
    u128 bm = d >= 128 ? 0 : b.m >> d;
    if (a.sign == b.sign) {
        u128 s = a.m + bm;
        if (s < a.m) /* carry out of bit 127 */
            return xf_make(a.sign, a.exp + 1, (s >> 1) | ((u128)1 << 127));
        return xf_make(a.sign, a.exp, s);
    }
    return xf_make(a.sign, a.exp, a.m - bm);
}

static XF xf_sub(XF a, XF b) {
    return xf_add(a, xf_neg(b));
}

/* A finite nonzero double, exactly; and an XF truncated to double for a
 * starting guess. Both stay inside the double's normal range here. */
static XF xf_from_double(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    return xf_make((int)(b >> 63), (int32_t)((b >> 52) & 0x7ffu) - 1023,
                   (u128)((b & UINT64_C(0x000fffffffffffff)) | UINT64_C(0x0010000000000000)) << 75);
}
static double xf_to_double(XF a) {
    uint64_t b = (uint64_t)a.sign << 63 | (uint64_t)(a.exp + 1023) << 52 |
                 ((uint64_t)(a.m >> 75) & UINT64_C(0x000fffffffffffff));
    double v;
    memcpy(&v, &b, 8);
    return v;
}

/* 1 / d by Newton's iteration from the double reciprocal: two steps take
 * 53 correct bits past the 128 the format keeps. */
static XF xf_recip(XF d) {
    int32_t e = d.exp;
    XF dn = xf_make(0, 0, d.m), x, two = xf_from_int(0, 2);
    int i;
    x = xf_from_double(1.0 / xf_to_double(dn));
    for (i = 0; i < 2; i++)
        x = xf_mul(x, xf_sub(two, xf_mul(dn, x)));
    x.exp -= e;
    x.sign = d.sign;
    return x;
}

static XF xf_div(XF a, XF b) {
    return xf_mul(a, xf_recip(b));
}

/* a / n for a small integer n. */
static XF xf_div_small(XF a, uint64_t n) {
    return xf_make(a.sign, a.exp, a.m / n);
}

/* sum_{k=0..n} c[step k] x^k by Horner's rule, the odd terms negated when
 * `alternate`. */
static XF xf_horner(XF x, const XF *c, int step, int n, int alternate) {
    XF sum = c[step * n];
    int k;
    if (alternate && (n & 1))
        sum = xf_neg(sum);
    for (k = n - 1; k >= 0; k--) {
        XF ck = c[step * k];
        if (alternate && (k & 1))
            ck = xf_neg(ck);
        sum = xf_add(xf_mul(sum, x), ck);
    }
    return sum;
}

/* Terms for a series in x^2 whose terms shrink by at least x^2 each, to
 * reach 2^-100 relative: |x| < 2^(exp + 1), so x^2 < 2^(2 exp + 2). */
static int xf_terms(XF x, int most) {
    int per = -2 * (x.exp + 1);
    int n = per > 0 ? (100 + per - 1) / per : most;
    return n < most ? n : most;
}

static const XF XF_PI = {0, 1, X87_U128(0xc90fdaa22168c234u, 0xc4c6628b80dc1cd1u)};
static const XF XF_PI_2 = {0, 0, X87_U128(0xc90fdaa22168c234u, 0xc4c6628b80dc1cd1u)};
static const XF XF_LN2 = {0, -1, X87_U128(0xb17217f7d1cf79abu, 0xc9e3b39803f2f6afu)};
static const XF XF_LOG2E = {0, 0, X87_U128(0xb8aa3b295c17f0bbu, 0xbe87fed0691d3e88u)};
static const XF XF_PI_OVER_P66 = {0, 0, X87_U128(0x8000000000000000u, 0x030a31c5b45d1254u)};
/* 1/n!, n = 0..39 */
static const XF XF_INV_FACT[40] = {
    {0, 0, X87_U128(0x8000000000000000u, 0x0000000000000000u)},
    {0, 0, X87_U128(0x8000000000000000u, 0x0000000000000000u)},
    {0, -1, X87_U128(0x8000000000000000u, 0x0000000000000000u)},
    {0, -3, X87_U128(0xaaaaaaaaaaaaaaaau, 0xaaaaaaaaaaaaaaaau)},
    {0, -5, X87_U128(0xaaaaaaaaaaaaaaaau, 0xaaaaaaaaaaaaaaaau)},
    {0, -7, X87_U128(0x8888888888888888u, 0x8888888888888888u)},
    {0, -10, X87_U128(0xb60b60b60b60b60bu, 0x60b60b60b60b60b6u)},
    {0, -13, X87_U128(0xd00d00d00d00d00du, 0x00d00d00d00d00d0u)},
    {0, -16, X87_U128(0xd00d00d00d00d00du, 0x00d00d00d00d00d0u)},
    {0, -19, X87_U128(0xb8ef1d2ab6399c7du, 0x560e4472800b8ef1u)},
    {0, -22, X87_U128(0x93f27dbbc4fae397u, 0x780b69f5333c725bu)},
    {0, -26, X87_U128(0xd7322b3faa271c7fu, 0x3a3f25c1bee38f10u)},
    {0, -29, X87_U128(0x8f76c77fc6c4bdaau, 0x26d4c3d67f425f60u)},
    {0, -33, X87_U128(0xb092309d43684be5u, 0x1c198e91d7b4269du)},
    {0, -37, X87_U128(0xc9cba54603e4e905u, 0xd6f8a2efd1f27546u)},
    {0, -41, X87_U128(0xd73f9f399dc0f88eu, 0xc32b58774657f48fu)},
    {0, -45, X87_U128(0xd73f9f399dc0f88eu, 0xc32b58774657f48fu)},
    {0, -49, X87_U128(0xca963b81856a5359u, 0x3028cbbb8d7ff53bu)},
    {0, -53, X87_U128(0xb413c31dcbecbbddu, 0x8024435161554bc3u)},
    {0, -57, X87_U128(0x97a4da340a0ab926u, 0x50f61dbdcb3a5abfu)},
    {0, -62, X87_U128(0xf2a15d201011283du, 0x4e5695fc785d5dfeu)},
    {0, -66, X87_U128(0xb8dc77b6e7ab8c5fu, 0x78a37e77372290c2u)},
    {0, -70, X87_U128(0x8671cb6dbfc294a2u, 0x86485bf99c763abbu)},
    {0, -75, X87_U128(0xbb0da098b1c0cecbu, 0xdc3826ebfb13cc26u)},
    {0, -80, X87_U128(0xf96780cb97abbe65u, 0x25a033e54ec51033u)},
    {0, -84, X87_U128(0x9f9e66e8b2fd46a7u, 0x22520cbbb7885c49u)},
    {0, -89, X87_U128(0xc4742fe35272cd1cu, 0x790285d3580a4a33u)},
    {0, -94, X87_U128(0xe8d58e16e6751905u, 0x4d0c78aea13b9a50u)},
    {0, -98, X87_U128(0x850c5131a842e9b9u, 0xe2e28e1aa546a152u)},
    {0, -103, X87_U128(0x92cfcc5a1ac56bd5u, 0xf1873bb378948eb3u)},
    {0, -108, X87_U128(0x9c9962823eb07306u, 0x56f6a614c4e2ba58u)},
    {0, -113, X87_U128(0xa1a6973c1fade217u, 0x0f7237d35fe1c89du)},
    {0, -118, X87_U128(0xa1a6973c1fade217u, 0x0f7237d35fe1c89du)},
    {0, -123, X87_U128(0x9cc092a6e86a8da9u, 0xc166ffd4ba113ea8u)},
    {0, -128, X87_U128(0x9388118e07ebd09fu, 0xc515a57ceb5b8644u)},
    {0, -133, X87_U128(0x86e2ce38b6c8f941u, 0x9e3fad3f0311d9d7u)},
    {0, -139, X87_U128(0xefcc194861654958u, 0x35c6895393adf50eu)},
    {0, -144, X87_U128(0xcf6468e4a742d7a6u, 0x3c58ae1ec4e979feu)},
    {0, -149, X87_U128(0xaea565ce061d5748u, 0x9e9b85276273c50cu)},
    {0, -154, X87_U128(0x8f4ca24d25d66f00u, 0x8223b575a61d5979u)},
};
/* 1/(2k+1), k = 0..31 */
static const XF XF_INV_ODD[32] = {
    {0, 0, X87_U128(0x8000000000000000u, 0x0000000000000000u)},
    {0, -2, X87_U128(0xaaaaaaaaaaaaaaaau, 0xaaaaaaaaaaaaaaaau)},
    {0, -3, X87_U128(0xccccccccccccccccu, 0xccccccccccccccccu)},
    {0, -3, X87_U128(0x9249249249249249u, 0x2492492492492492u)},
    {0, -4, X87_U128(0xe38e38e38e38e38eu, 0x38e38e38e38e38e3u)},
    {0, -4, X87_U128(0xba2e8ba2e8ba2e8bu, 0xa2e8ba2e8ba2e8bau)},
    {0, -4, X87_U128(0x9d89d89d89d89d89u, 0xd89d89d89d89d89du)},
    {0, -4, X87_U128(0x8888888888888888u, 0x8888888888888888u)},
    {0, -5, X87_U128(0xf0f0f0f0f0f0f0f0u, 0xf0f0f0f0f0f0f0f0u)},
    {0, -5, X87_U128(0xd79435e50d79435eu, 0x50d79435e50d7943u)},
    {0, -5, X87_U128(0xc30c30c30c30c30cu, 0x30c30c30c30c30c3u)},
    {0, -5, X87_U128(0xb21642c8590b2164u, 0x2c8590b21642c859u)},
    {0, -5, X87_U128(0xa3d70a3d70a3d70au, 0x3d70a3d70a3d70a3u)},
    {0, -5, X87_U128(0x97b425ed097b425eu, 0xd097b425ed097b42u)},
    {0, -5, X87_U128(0x8d3dcb08d3dcb08du, 0x3dcb08d3dcb08d3du)},
    {0, -5, X87_U128(0x8421084210842108u, 0x4210842108421084u)},
    {0, -6, X87_U128(0xf83e0f83e0f83e0fu, 0x83e0f83e0f83e0f8u)},
    {0, -6, X87_U128(0xea0ea0ea0ea0ea0eu, 0xa0ea0ea0ea0ea0eau)},
    {0, -6, X87_U128(0xdd67c8a60dd67c8au, 0x60dd67c8a60dd67cu)},
    {0, -6, X87_U128(0xd20d20d20d20d20du, 0x20d20d20d20d20d2u)},
    {0, -6, X87_U128(0xc7ce0c7ce0c7ce0cu, 0x7ce0c7ce0c7ce0c7u)},
    {0, -6, X87_U128(0xbe82fa0be82fa0beu, 0x82fa0be82fa0be82u)},
    {0, -6, X87_U128(0xb60b60b60b60b60bu, 0x60b60b60b60b60b6u)},
    {0, -6, X87_U128(0xae4c415c9882b931u, 0x0572620ae4c415c9u)},
    {0, -6, X87_U128(0xa72f05397829cbc1u, 0x4e5e0a72f0539782u)},
    {0, -6, X87_U128(0xa0a0a0a0a0a0a0a0u, 0xa0a0a0a0a0a0a0a0u)},
    {0, -6, X87_U128(0x9a90e7d95bc609a9u, 0x0e7d95bc609a90e7u)},
    {0, -6, X87_U128(0x94f2094f2094f209u, 0x4f2094f2094f2094u)},
    {0, -6, X87_U128(0x8fb823ee08fb823eu, 0xe08fb823ee08fb82u)},
    {0, -6, X87_U128(0x8ad8f2fba9386822u, 0xb63cbeea4e1a08adu)},
    {0, -6, X87_U128(0x864b8a7de6d1d608u, 0x64b8a7de6d1d6086u)},
    {0, -6, X87_U128(0x8208208208208208u, 0x2082082082082082u)},
};
/* atan(k/8), k = 1..8 */
static const XF XF_ATAN_K8[8] = {
    {0, -4, X87_U128(0xfeadd4d5617b6e32u, 0xc897989f3e888ef7u)},
    {0, -3, X87_U128(0xfadbafc96406eb15u, 0x6dc79ef5f7a217e5u)},
    {0, -2, X87_U128(0xb7b0ca0f26f78473u, 0x8aa32122dcfe4483u)},
    {0, -2, X87_U128(0xed63382b0dda7b45u, 0x6fe445ecbc3a8d03u)},
    {0, -1, X87_U128(0x8f005d5ef7f59f9bu, 0x5c835e1665c43747u)},
    {0, -1, X87_U128(0xa4bc7d1934f70924u, 0x19a87f2a457dac9eu)},
    {0, -1, X87_U128(0xb8053e2bc2319e73u, 0xcb2da55210a4443du)},
    {0, -1, X87_U128(0xc90fdaa22168c234u, 0xc4c6628b80dc1cd1u)},
};

/* 2 atanh(s) = ln((1 + s) / (1 - s)) = 2 (s + s^3/3 + s^5/5 + ...), |s| < 1/2. */
static XF xf_atanh2(XF s) {
    XF r = xf_mul(s, xf_horner(xf_mul(s, s), XF_INV_ODD, 1, xf_terms(s, 31), 0));
    r.exp += 1;
    return r;
}

/* sin t and cos t for |t| <= pi/4, by their Taylor series:
 * t (1 - t^2/3! + ...) and 1 - t^2/2! + ...; 14 terms reach 2^-107. */
static XF xf_sin(XF t) {
    return xf_mul(t, xf_horner(xf_mul(t, t), XF_INV_FACT + 1, 2, 13, 1));
}
static XF xf_cos(XF t) {
    return xf_horner(xf_mul(t, t), XF_INV_FACT, 2, 13, 1);
}

/* atan z for 0 <= z <= 1: atan(c) + atan((z - c) / (1 + zc)) with c the
 * nearest eighth, then the odd series in |w| <= 1/16. */
static XF xf_atan01(XF z) {
    uint64_t k = 0;
    if (z.m && z.exp >= -4) {
        /* round(8z): 8z = m * 2^(exp + 3 - 127) */
        int sh = 127 - (z.exp + 3);
        u128 eights = sh >= 128 ? 0 : z.m >> sh;
        u128 half = sh >= 1 && sh <= 128 ? (z.m >> (sh - 1)) & 1u : 0;
        k = (uint64_t)eights + (uint64_t)half;
        if (k > 8)
            k = 8;
    }
    XF base = {0, 0, 0}, w = z;
    if (k) {
        XF c = xf_make(0, 127 - 3, (u128)k); /* k / 8 */
        w = xf_div(xf_sub(z, c), xf_add(xf_from_int(0, 1), xf_mul(z, c)));
        base = XF_ATAN_K8[k - 1];
    }
    XF sum = xf_mul(w, xf_horner(xf_mul(w, w), XF_INV_ODD, 1, xf_terms(w, 31), 1));
    return xf_add(base, sum);
}

/* ln x for finite x > 0: e ln 2 + 2 atanh((m - 1) / (m + 1)), m in
 * [sqrt(1/2), sqrt(2)). */
static XF xf_log(XF x) {
    int32_t e = x.exp;
    XF m = xf_make(0, 0, x.m);
    if (x.m >= X87_U128(0xb504f333f9de6484u, 0x597d89b3754abe9fu)) { /* m >= sqrt 2 */
        m.exp = -1;
        e++;
    }
    XF one = xf_from_int(0, 1);
    XF r = xf_atanh2(xf_div(xf_sub(m, one), xf_add(m, one)));
    if (e)
        r = xf_add(r, xf_mul(xf_from_int(e < 0, (uint64_t)(e < 0 ? -(int64_t)e : e)), XF_LN2));
    return r;
}

/* Round to the 80-bit format per RC (64 bits; PC does not apply). */
static X87Ext xf_to_x80(X86 *c, XF a) {
    if (!a.m)
        return x80((uint16_t)(a.sign << 15), 0);
    sf_begin(c, 0);
    uint64_t hi = (uint64_t)(a.m >> 64), lo = (uint64_t)a.m;
    extFloat80_t r = softfloat_roundPackToExtF80(a.sign != 0, a.exp + 16383, hi, lo, 80);
    sf_end(c);
    return from_sf(r);
}

static X87Ext x80_signed(int sign, uint16_t exp, uint64_t m) {
    return x80((uint16_t)((sign ? 0x8000u : 0u) | exp), m);
}

/* x reduced the way FSIN/FCOS/FPTAN reduce it: x = j (P66 / 2) + r, j the
 * nearest integer, exactly, then t = r pi / P66, so that f(x pi / P66) is
 * f(j pi / 2 + t). Returns j mod 4; x is finite and below 2^63. */
static unsigned trig_reduce(X87Ext x, XF *t) {
    int32_t e = (x.signExp & 0x7fff) - 16383;
    XF ax = xf_from_x80(x);
    ax.sign = 0;
    if (!ax.m || e < -1) { /* |x| < 1/2 < pi/4: no reduction */
        *t = xf_mul(ax, XF_PI_OVER_P66);
        return 0;
    }
    /* N = |x| 2^65, an integer below 2^128; P66 / 2 = Q66 2^-65 */
    const u128 Q66 = X87_U128(0x3u, 0x243f6a8885a308d3u);
    u128 N = (u128)x.signif << (e + 2);
    u128 j = N / Q66, rem = N % Q66;
    int neg = 0;
    if (rem > Q66 - rem) {
        j++;
        rem = Q66 - rem;
        neg = 1;
    }
    *t = xf_mul(xf_make(neg, 127 - 65, rem), XF_PI_OVER_P66);
    return (unsigned)(j & 3u);
}

/* sin and cos of x pi / P66 for finite |x| < 2^63; s or co may be null. */
static void trig_eval(X87Ext x, XF *s, XF *co) {
    XF t, st = {0, 0, 0}, ct = {0, 0, 0}, dummy;
    unsigned q = trig_reduce(x, &t);
    int need_sin = (s && !(q & 1)) || (co && (q & 1));
    int need_cos = (s && (q & 1)) || (co && !(q & 1));
    if (need_sin)
        st = xf_sin(t);
    if (need_cos)
        ct = xf_cos(t);
    if (!s)
        s = &dummy;
    if (!co)
        co = &dummy;
    switch (q) {
    case 0:
        *s = st;
        *co = ct;
        break;
    case 1:
        *s = ct;
        *co = xf_neg(st);
        break;
    case 2:
        *s = xf_neg(st);
        *co = xf_neg(ct);
        break;
    default:
        *s = xf_neg(ct);
        *co = st;
        break;
    }
    if (x.signExp & 0x8000u)
        *s = xf_neg(*s);
}

/* The operand checks FSIN, FCOS, FSINCOS and FPTAN share. Returns 1 when the
 * instruction is finished: a NaN result written to ST(0) (pushed twice when
 * `twice`), the indefinite for an infinity, or an out-of-range operand left
 * alone with C2 set. */
static int trig_special(X86 *c, unsigned p, X87Ext v, int twice) {
    int cls = x80_class(v);
    c->fpu_sw &= (uint16_t)~SW_C2;
    if (cls >= XC_QNAN || cls == XC_INF) {
        X87Ext n = cls == XC_INF ? X80_INDEFINITE : nan_result(c, &v, &cls, 1);
        if (cls == XC_INF)
            c->fpu_sw |= SW_IE;
        reg_put(c, p, n);
        if (twice)
            push_ext(c, n);
        return 1;
    }
    if (cls == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    if (cls == XC_NORMAL && (v.signExp & 0x7fff) >= 16383 + 63) {
        c->fpu_sw |= SW_C2;
        return 1;
    }
    return 0;
}

void x87_fsin(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v = reg_get(c, p);
    XF s;
    if (trig_special(c, p, v, 0) || x80_class(v) == XC_ZERO)
        return;
    trig_eval(v, &s, NULL);
    reg_put(c, p, xf_to_x80(c, s));
}

void x87_fcos(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v = reg_get(c, p);
    XF co;
    if (trig_special(c, p, v, 0))
        return;
    if (x80_class(v) == XC_ZERO) {
        reg_put(c, p, x80(0x3fffu, SIG_INT));
        return;
    }
    trig_eval(v, NULL, &co);
    reg_put(c, p, xf_to_x80(c, co));
}

/* FSINCOS: ST(0) becomes the sine and the cosine is pushed. */
void x87_fsincos(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v = reg_get(c, p);
    XF s, co;
    if (trig_special(c, p, v, 1))
        return;
    if (x80_class(v) == XC_ZERO) {
        push_ext(c, x80(0x3fffu, SIG_INT));
        return;
    }
    trig_eval(v, &s, &co);
    reg_put(c, p, xf_to_x80(c, s));
    push_ext(c, xf_to_x80(c, co));
}

/* FPTAN: ST(0) becomes the tangent and 1.0 is pushed. */
void x87_fptan(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v = reg_get(c, p);
    XF s, co;
    if (trig_special(c, p, v, 1))
        return;
    if (x80_class(v) != XC_ZERO) {
        trig_eval(v, &s, &co);
        reg_put(c, p, xf_to_x80(c, xf_div(s, co)));
    }
    push_ext(c, x80(0x3fffu, SIG_INT));
}

/* FPATAN: ST(1) = atan2(ST(1), ST(0)), then pop. The signed zeros and
 * infinities give C99's atan2 results, which are x87's. */
void x87_fpatan(X86 *c) {
    unsigned p0 = top_slot(c, 0), p1 = top_slot(c, 1);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v[2] = {reg_get(c, p0), reg_get(c, p1)}; /* x, y */
    int cls[2] = {x80_class(v[0]), x80_class(v[1])};
    int sx = v[0].signExp >> 15, sy = v[1].signExp >> 15;
    XF r;
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        reg_put(c, p1, nan_result(c, v, cls, 2));
        fdrop(c);
        return;
    }
    if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    if (cls[1] == XC_ZERO) {
        r = sx ? XF_PI : xf_make(0, 0, 0);
    } else if (cls[0] == XC_ZERO) {
        r = XF_PI_2;
    } else if (cls[1] == XC_INF) {
        if (cls[0] == XC_INF) {
            r = XF_ATAN_K8[7]; /* pi/4 */
            if (sx)
                r = xf_sub(XF_PI, r);
        } else {
            r = XF_PI_2;
        }
    } else if (cls[0] == XC_INF) {
        r = sx ? XF_PI : xf_make(0, 0, 0);
    } else {
        XF ax = xf_from_x80(v[0]), ay = xf_from_x80(v[1]);
        ax.sign = ay.sign = 0;
        if (ay.exp > ax.exp || (ay.exp == ax.exp && ay.m > ax.m))
            r = xf_sub(XF_PI_2, xf_atan01(xf_div(ax, ay)));
        else
            r = xf_atan01(xf_div(ay, ax));
        if (sx)
            r = xf_sub(XF_PI, r);
    }
    r.sign = sy;
    reg_put(c, p1, xf_to_x80(c, r));
    fdrop(c);
}

/* F2XM1: 2^x - 1, by expm1's series in x ln 2. The hardware defines it for
 * -1 <= x <= 1; this computes it for any finite x below about 2^14. */
void x87_f2xm1(X86 *c) {
    unsigned p = top_slot(c, 0);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v = reg_get(c, p);
    int cls = x80_class(v);
    if (cls >= XC_QNAN) {
        reg_put(c, p, nan_result(c, &v, &cls, 1));
        return;
    }
    if (cls == XC_ZERO)
        return;
    if (cls == XC_INF) {
        if (v.signExp & 0x8000u)
            reg_put(c, p, x80(0xbfffu, SIG_INT)); /* -1 */
        return;
    }
    if (cls == XC_DENORMAL)
        c->fpu_sw |= SW_DE;
    XF y = xf_mul(xf_from_x80(v), XF_LN2), sum;
    if (y.exp < 0) { /* |x| < 1/ln 2: y (1 + y/2! + y^2/3! + ...), 26 terms reach 2^-107 */
        sum = xf_mul(y, xf_horner(y, XF_INV_FACT + 1, 1, 26, 0));
    } else { /* outside the instruction's domain: the series term by term */
        XF term = y;
        uint64_t n;
        sum = y;
        for (n = 2; n < 4000; n++) {
            term = xf_div_small(xf_mul(term, y), n);
            if (!term.m || term.exp < sum.exp - 110)
                break;
            sum = xf_add(sum, term);
        }
    }
    reg_put(c, p, xf_to_x80(c, sum));
}

/* FYL2X: ST(1) = ST(1) * log2(ST(0)), then pop. */
void x87_fyl2x(X86 *c) {
    unsigned p0 = top_slot(c, 0), p1 = top_slot(c, 1);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v[2] = {reg_get(c, p0), reg_get(c, p1)}; /* x, y */
    int cls[2] = {x80_class(v[0]), x80_class(v[1])};
    int sx = v[0].signExp >> 15, sy = v[1].signExp >> 15;
    X87Ext r;
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        r = nan_result(c, v, cls, 2);
    } else if (sx && cls[0] != XC_ZERO) { /* log of a negative */
        c->fpu_sw |= SW_IE;
        r = X80_INDEFINITE;
    } else if (cls[0] == XC_ZERO) { /* log2(0) = -inf */
        if (cls[1] == XC_ZERO) {
            c->fpu_sw |= SW_IE;
            r = X80_INDEFINITE;
        } else {
            c->fpu_sw |= SW_ZE;
            r = x80_signed(!sy, 0x7fffu, SIG_INT);
        }
    } else if (cls[0] == XC_INF) { /* log2(inf) = inf */
        if (cls[1] == XC_ZERO) {
            c->fpu_sw |= SW_IE;
            r = X80_INDEFINITE;
        } else {
            r = x80_signed(sy, 0x7fffu, SIG_INT);
        }
    } else {
        XF lx = xf_mul(xf_log(xf_from_x80(v[0])), XF_LOG2E);
        if (cls[1] == XC_INF) {
            if (!lx.m) {
                c->fpu_sw |= SW_IE;
                r = X80_INDEFINITE;
            } else {
                r = x80_signed(sy ^ lx.sign, 0x7fffu, SIG_INT);
            }
        } else if (!lx.m || cls[1] == XC_ZERO) {
            r = x80_signed(sy ^ lx.sign, 0, 0);
        } else {
            if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
                c->fpu_sw |= SW_DE;
            r = xf_to_x80(c, xf_mul(xf_from_x80(v[1]), lx));
        }
    }
    reg_put(c, p1, r);
    fdrop(c);
}

/* FYL2XP1: ST(1) = ST(1) * log2(ST(0) + 1), then pop; ln(1 + x) as
 * 2 atanh(x / (2 + x)), which keeps a small x's bits. The hardware defines it
 * for |x| < 1 - sqrt(2)/2. */
void x87_fyl2xp1(X86 *c) {
    unsigned p0 = top_slot(c, 0), p1 = top_slot(c, 1);
    x87_enter(c, X87_STAT_TRANSC);
    X87Ext v[2] = {reg_get(c, p0), reg_get(c, p1)}; /* x, y */
    int cls[2] = {x80_class(v[0]), x80_class(v[1])};
    int sx = v[0].signExp >> 15, sy = v[1].signExp >> 15;
    X87Ext r;
    if (cls[0] >= XC_QNAN || cls[1] >= XC_QNAN) {
        r = nan_result(c, v, cls, 2);
    } else if (cls[0] == XC_INF || cls[1] == XC_INF) {
        c->fpu_sw |= SW_IE; /* outside the instruction's domain */
        r = X80_INDEFINITE;
    } else if (cls[0] == XC_ZERO || cls[1] == XC_ZERO) {
        r = x80_signed(sx ^ sy, 0, 0);
    } else {
        XF x = xf_from_x80(v[0]);
        XF l = xf_atanh2(xf_div(x, xf_add(xf_from_int(0, 2), x)));
        if (cls[0] == XC_DENORMAL || cls[1] == XC_DENORMAL)
            c->fpu_sw |= SW_DE;
        r = xf_to_x80(c, xf_mul(xf_from_x80(v[1]), xf_mul(l, XF_LOG2E)));
    }
    reg_put(c, p1, r);
    fdrop(c);
}

/* ------------------------------------------------- msvcrt float control --- */

/* Microsoft's abstract control bits (float.h). */
#define MS_EM_INVALID 0x00000010u
#define MS_EM_DENORMAL 0x00080000u
#define MS_EM_ZERODIVIDE 0x00000008u
#define MS_EM_OVERFLOW 0x00000004u
#define MS_EM_UNDERFLOW 0x00000002u
#define MS_EM_INEXACT 0x00000001u
#define MS_RC_DOWN 0x00000100u
#define MS_RC_UP 0x00000200u
#define MS_RC_CHOP 0x00000300u
#define MS_MCW_RC 0x00000300u
#define MS_PC_24 0x00020000u
#define MS_PC_53 0x00010000u
#define MS_MCW_PC 0x00030000u
#define MS_IC_AFFINE 0x00040000u

static const struct {
    uint16_t cw;
    uint32_t abstract;
} em_bits[] = {
    {0x01u, MS_EM_INVALID},  {0x02u, MS_EM_DENORMAL},  {0x04u, MS_EM_ZERODIVIDE},
    {0x08u, MS_EM_OVERFLOW}, {0x10u, MS_EM_UNDERFLOW}, {0x20u, MS_EM_INEXACT},
};

/* msvcrt's _abstract_cw: PC=01 (reserved) maps to _PC_64, which is 0. */
uint32_t x87_cw_to_abstract(uint16_t cw) {
    uint32_t a = 0;
    unsigned i;
    for (i = 0; i < sizeof em_bits / sizeof em_bits[0]; i++)
        if (cw & em_bits[i].cw)
            a |= em_bits[i].abstract;
    a |= (uint32_t)((cw >> 10) & 3u) << 8; /* RC: near, down, up, chop */
    if ((cw & 0x0300u) == 0x0000u)
        a |= MS_PC_24;
    else if ((cw & 0x0300u) == 0x0200u)
        a |= MS_PC_53;
    if (cw & 0x1000u)
        a |= MS_IC_AFFINE;
    return a;
}

/* msvcrt's _hw_cw. It builds the managed fields only; the reserved bits
 * (6, 7, 13-15) are kept from the current word, so the 0x037f a thread
 * starts with and _PC_53 give the 0x027f Windows threads run at. */
uint16_t x87_cw_from_abstract(uint32_t a, uint16_t cw) {
    uint16_t r = (uint16_t)(cw & 0xe0c0u);
    unsigned i;
    for (i = 0; i < sizeof em_bits / sizeof em_bits[0]; i++)
        if (a & em_bits[i].abstract)
            r |= em_bits[i].cw;
    r |= (uint16_t)(((a & MS_MCW_RC) >> 8) << 10);
    switch (a & MS_MCW_PC) {
    case 0: /* _PC_64 */
        r |= 0x0300u;
        break;
    case MS_PC_53:
        r |= 0x0200u;
        break;
    default: /* _PC_24, and the undefined 0x30000 */
        break;
    }
    if (a & MS_IC_AFFINE)
        r |= 0x1000u;
    return r;
}

/* _control87(new, mask): the new abstract word, as msvcrt computes it. */
uint32_t x87_control87(X86 *c, uint32_t value, uint32_t mask) {
    uint32_t a = (value & mask) | (x87_cw_to_abstract(c->fpu_cw) & ~mask);
    x87_set_cw(c, x87_cw_from_abstract(a, c->fpu_cw));
    return a;
}

/* _controlfp leaves the denormal mask alone. */
uint32_t x87_controlfp(X86 *c, uint32_t value, uint32_t mask) {
    return x87_control87(c, value, mask & ~MS_EM_DENORMAL);
}

/* _statusfp: the exception flags as _SW_* bits, which reuse the _EM_ values. */
uint32_t x87_statusfp(const X86 *c) {
    uint32_t a = 0;
    unsigned i;
    for (i = 0; i < sizeof em_bits / sizeof em_bits[0]; i++)
        if (c->fpu_sw & em_bits[i].cw)
            a |= em_bits[i].abstract;
    return a;
}

/* _clearfp: the flags as _statusfp reports them, then FNCLEX. */
uint32_t x87_clearfp(X86 *c) {
    uint32_t a = x87_statusfp(c);
    c->fpu_sw &= (uint16_t)~0x80ffu;
    return a;
}

/* _fpreset: FNINIT, then the CRT's default word (_CW_DEFAULT, 0x0009001f). */
void x87_fpreset(X86 *c) {
    x87_finit(c);
    x87_set_cw(c, x87_cw_from_abstract(0x0009001fu, c->fpu_cw));
}
