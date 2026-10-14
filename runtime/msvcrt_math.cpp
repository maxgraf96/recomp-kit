// msvcrt_math.cpp - the C runtime's floating-point functions.
//
// cdecl functions take doubles on the stack and return in ST(0); the _CI*
// intrinsics take their operands on the x87 stack (the first argument in
// ST(1) for the two-operand ones) and replace them with the result. _ftol
// truncates ST(0) to EDX:EAX and pops it.
//
// sqrt (and _CIsqrt) run as x87 FSQRT in msvcrt, so their result follows the
// caller's precision control: IEEE double at PC=53 and a 24-bit significand
// with the extended exponent range at PC=24. That is computed exactly here,
// including the double-rounding case. The remaining functions use the host's
// libm; on Windows they are msvcrt's own SSE2 or x87 code, not guaranteed
// correctly rounded, so they can differ in the last bit. None feeds a
// simulation in the programs this kit serves today; the Windows vectors in
// the game repository measure them.
//
// Invalid operations return the x87 "indefinite" NaN (negative, quiet,
// payload zero), which is what msvcrt hands back for sqrt(-1) or log(-1),
// not the host's positive default NaN, and set errno to EDOM.
//
// _controlfp and the other control-word functions belong to the x87 layer,
// not to this file.
#include "msvcrt_internal.h"
#include <cfenv>
#include <cmath>

namespace crt {
namespace {

double indefinite() {
    uint64_t bits = 0xfff8000000000000ull;
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

// A NaN operand comes back quiet with its payload, as the x87 does.
double quiet(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    bits |= 0x0008000000000000ull;
    memcpy(&v, &bits, 8);
    return v;
}

// Rounds a positive double to `bits` significant bits, ties to even,
// keeping the double's exponent range (the x87 keeps 15 exponent bits).
double round_significand(double v, int bits) {
    int e;
    double m = std::frexp(v, &e); // v = m * 2^e, 0.5 <= m < 1
    double scaled = std::ldexp(m, bits);
    return std::ldexp(std::nearbyint(scaled), e - bits);
}

// sqrt as FSQRT computes it under the precision-control field of `cw`.
double x87_sqrt(double x, uint16_t cw) {
    if (std::isnan(x)) {
        set_errno(E_DOM); // msvcrt reports a NaN operand as a domain error
        return quiet(x);
    }
    if (x < 0) {
        set_errno(E_DOM);
        return indefinite();
    }
    double r = std::sqrt(x); // correctly rounded to 53 bits
    if (((cw >> 8) & 3u) != 0 || r == 0 || std::isinf(r))
        return r; // PC=53 (and PC=64, which a double register cannot hold)
    // PC=24. Rounding the 53-bit result again is wrong only when the 29
    // discarded bits are exactly a midpoint; then the exact square of the
    // midpoint (25 bits squared fits a double) says which way the true root
    // lies.
    int e;
    double m = std::frexp(r, &e);
    double scaled = std::ldexp(m, 24);
    double floor_part = std::floor(scaled);
    if (scaled - floor_part != 0.5)
        return round_significand(r, 24);
    double mid = std::ldexp(floor_part + 0.5, e - 24);
    double sq = mid * mid;
    if (x > sq)
        return std::ldexp(floor_part + 1, e - 24);
    if (x < sq)
        return std::ldexp(floor_part, e - 24);
    return round_significand(r, 24); // an exact tie: even
}

// A double argument as msvcrt sees it: its x87 code loads the argument with
// FLD, which turns a signalling NaN into the quiet one.
double stack_double(X86 *c, int slot) {
    double v = rdf64(c->r[R_ESP] + 4 + 4 * (uint32_t)slot);
    return std::isnan(v) ? quiet(v) : v;
}

void crt_sqrt(X86 *c) {
    fpush(c, x87_sqrt(stack_double(c, 0), c->fpu_cw));
}
void crt_fabs(X86 *c) {
    fpush(c, std::fabs(stack_double(c, 0)));
}

double crt_pow_value(double x, double y) {
    if (std::isnan(x) || std::isnan(y)) {
        if (y == 0)
            return 1.0;
        return quiet(std::isnan(x) ? x : y);
    }
    if (x < 0 && std::isfinite(y) && std::floor(y) != y) {
        set_errno(E_DOM);
        return indefinite();
    }
    double r = std::pow(x, y);
    if (std::isinf(r) && std::isfinite(x) && std::isfinite(y))
        set_errno(E_RANGE);
    else if (r == 0 && x != 0 && std::isfinite(x) && std::isfinite(y))
        set_errno(E_RANGE);
    return r;
}
void crt_pow(X86 *c) {
    fpush(c, crt_pow_value(stack_double(c, 0), stack_double(c, 2)));
}

double crt_log_value(double x) {
    if (std::isnan(x))
        return quiet(x);
    if (x < 0) {
        set_errno(E_DOM);
        return indefinite();
    }
    if (x == 0) {
        set_errno(E_RANGE);
        return -HUGE_VAL;
    }
    return std::log(x);
}
void crt_log(X86 *c) {
    fpush(c, crt_log_value(stack_double(c, 0)));
}

double crt_trig(double x, double (*fn)(double)) {
    if (std::isnan(x))
        return quiet(x);
    if (std::isinf(x)) {
        set_errno(E_DOM);
        return indefinite();
    }
    return fn(x);
}
void crt_sin(X86 *c) {
    fpush(c, crt_trig(stack_double(c, 0), [](double v) { return std::sin(v); }));
}

void crt_ldexp(X86 *c) {
    double x = stack_double(c, 0);
    int32_t e = (int32_t)arg(c, 2);
    double r = std::ldexp(x, e);
    if ((std::isinf(r) && std::isfinite(x)) || (r == 0 && x != 0))
        set_errno(E_RANGE);
    fpush(c, r);
}

void crt_finite(X86 *c) {
    set_eax(c, std::isfinite(stack_double(c, 0)) ? 1 : 0);
}

// _CI* intrinsics: operands on the x87 stack.
void ci_unary(X86 *c, double (*fn)(double)) {
    double x = fpop(c);
    fpush(c, fn(x));
}
void crt_CIsqrt(X86 *c) {
    double x = fpop(c);
    fpush(c, x87_sqrt(x, c->fpu_cw));
}
void crt_CIsin(X86 *c) {
    ci_unary(c, [](double x) { return crt_trig(x, [](double v) { return std::sin(v); }); });
}
void crt_CIcos(X86 *c) {
    ci_unary(c, [](double x) { return crt_trig(x, [](double v) { return std::cos(v); }); });
}
void crt_CItan(X86 *c) {
    ci_unary(c, [](double x) { return crt_trig(x, [](double v) { return std::tan(v); }); });
}
void crt_CIexp(X86 *c) {
    ci_unary(c, [](double x) {
        if (std::isnan(x))
            return quiet(x);
        double r = std::exp(x);
        if (std::isinf(r) && std::isfinite(x))
            set_errno(E_RANGE);
        return r;
    });
}
void crt_CIlog(X86 *c) {
    ci_unary(c, crt_log_value);
}
void crt_CIpow(X86 *c) {
    double y = fpop(c);
    double x = fpop(c);
    fpush(c, crt_pow_value(x, y));
}
void crt_CIatan2(X86 *c) {
    double x = fpop(c);
    double y = fpop(c);
    if (std::isnan(x) || std::isnan(y))
        fpush(c, quiet(std::isnan(y) ? y : x));
    else
        fpush(c, std::atan2(y, x));
}

// _ftol: ST(0) truncated toward zero into EDX:EAX, popped. Out of range and
// NaN give the integer indefinite, 0x8000000000000000, as FISTP m64 does.
void crt_ftol(X86 *c) {
    double x = fpop(c);
    uint64_t v;
    if (std::isnan(x) || x >= 9223372036854775808.0 || x < -9223372036854775808.0)
        v = 0x8000000000000000ull;
    else
        v = (uint64_t)(int64_t)std::trunc(x);
    set_eax64(c, v);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "sqrt", ARGC_CDECL, crt_sqrt},
    {"msvcrt.dll", "fabs", ARGC_CDECL, crt_fabs},
    {"msvcrt.dll", "pow", ARGC_CDECL, crt_pow},
    {"msvcrt.dll", "log", ARGC_CDECL, crt_log},
    {"msvcrt.dll", "sin", ARGC_CDECL, crt_sin},
    {"msvcrt.dll", "ldexp", ARGC_CDECL, crt_ldexp},
    {"msvcrt.dll", "_finite", ARGC_CDECL, crt_finite},
    {"msvcrt.dll", "_CIsqrt", ARGC_CDECL, crt_CIsqrt},
    {"msvcrt.dll", "_CIsin", ARGC_CDECL, crt_CIsin},
    {"msvcrt.dll", "_CIcos", ARGC_CDECL, crt_CIcos},
    {"msvcrt.dll", "_CItan", ARGC_CDECL, crt_CItan},
    {"msvcrt.dll", "_CIexp", ARGC_CDECL, crt_CIexp},
    {"msvcrt.dll", "_CIlog", ARGC_CDECL, crt_CIlog},
    {"msvcrt.dll", "_CIpow", ARGC_CDECL, crt_CIpow},
    {"msvcrt.dll", "_CIatan2", ARGC_CDECL, crt_CIatan2},
    {"msvcrt.dll", "_ftol", ARGC_CDECL, crt_ftol},
};

} // namespace

void register_math() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

} // namespace crt
