// msvcrt_math.cpp - the C runtime's floating-point functions.
//
// cdecl functions take doubles on the stack and return in ST(0); the _CI*
// intrinsics take their operands on the x87 stack (the first argument in
// ST(1) for the two-operand ones) and replace them with the result. _ftol
// truncates ST(0) to EDX:EAX and pops it.
//
// sqrt (and _CIsqrt) run as x87 FSQRT in msvcrt, so their result follows the
// caller's control word: IEEE double at PC=53, a 24-bit significand with the
// extended exponent range at PC=24, 64 bits at PC=64, rounded per RC. They run
// the x87 layer's own FSQRT (runtime/x86.h), exact for all of these. _ftol is
// its FISTP m64 under a truncating control word. The remaining functions use
// the host's libm; on Windows they are msvcrt's own SSE2 or x87 code, not guaranteed
// correctly rounded, so they can differ in the last bit. None feeds a
// simulation in the programs this kit serves today; the Windows vectors in
// the game repository measure them.
//
// Invalid operations return the x87 "indefinite" NaN (negative, quiet,
// payload zero), which is what msvcrt hands back for sqrt(-1) or log(-1),
// not the host's positive default NaN, and set errno to EDOM.
//
// _controlfp and the other control-word functions are msvcrt_float.cpp.
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

// A double argument as msvcrt sees it: its x87 code loads the argument with
// FLD, which turns a signalling NaN into the quiet one.
double stack_double(X86 *c, int slot) {
    double v = rdf64(c->r[R_ESP] + 4 + 4 * (uint32_t)slot);
    return std::isnan(v) ? quiet(v) : v;
}

// msvcrt reports a NaN or negative operand as a domain error; FSQRT gives
// the quiet NaN or the indefinite.
void sqrt_errno(double x) {
    if (std::isnan(x) || x < 0)
        set_errno(E_DOM);
}

void crt_sqrt(X86 *c) {
    double x = stack_double(c, 0);
    sqrt_errno(x);
    fpush(c, x);
    x87_fsqrt(c);
}
// fabs of a NaN is a domain error in msvcrt, and returns the (quiet) NaN with
// its sign: Windows hands back -nan(ind) for -nan(ind).
void crt_fabs(X86 *c) {
    double x = stack_double(c, 0);
    if (std::isnan(x)) {
        set_errno(E_DOM);
        fpush(c, x);
        return;
    }
    fpush(c, std::fabs(x));
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
    if (std::isinf(r) && std::isfinite(x)) // an underflow to zero is not reported
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
// In place: ST(0) may hold more than a double, and FSQRT takes all of it.
void crt_CIsqrt(X86 *c) {
    sqrt_errno(ST(c, 0));
    x87_fsqrt(c);
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
// msvcrt's own code: FNSTCW, FLDCW with RC = chop, FISTP m64, FLDCW. ST(0)
// may hold more than a double, and FISTP takes all of it.
void crt_ftol(X86 *c) {
    uint16_t cw = c->fpu_cw;
    x87_set_cw(c, (uint16_t)(cw | 0x0c00u));
    set_eax64(c, (uint64_t)x87_fist64(c));
    x87_set_cw(c, cw);
    fdrop(c);
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
