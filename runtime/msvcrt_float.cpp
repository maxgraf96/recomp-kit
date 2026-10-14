// msvcrt_float.cpp - the C runtime's floating-point control: _controlfp,
// _control87, __control87_2, _statusfp/_status87, _clearfp/_clear87 and
// _fpreset.
//
// Each works on the calling guest thread's own x87 state through the x87
// layer (runtime/x87.c), which maps Microsoft's abstract bits to and from the
// control and status words exactly as msvcrt's _abstract_cw/_hw_cw do: no
// system call, no host floating-point state. A program may poll _controlfp
// thousands of times a second; each call is a few integer operations.
//
// MXCSR is not modelled (translated SSE arithmetic never changes it), so the
// SSE2 half of __control87_2 reports msvcrt's default for it, every exception
// masked and round to nearest, and ignores writes.
#include "msvcrt_internal.h"

namespace crt {
namespace {

// _EM_* for every SSE exception masked, _RC_NEAR: the MXCSR 0x1f80 a thread
// starts with, as __control87_2 reports it.
const uint32_t SSE2_DEFAULT = 0x0008001fu;

void crt_controlfp(X86 *c) {
    set_eax(c, x87_controlfp(c, arg(c, 0), arg(c, 1)));
}
void crt_control87(X86 *c) {
    set_eax(c, x87_control87(c, arg(c, 0), arg(c, 1)));
}
// int __control87_2(unsigned new, unsigned mask, unsigned *x86_cw,
// unsigned *sse2_cw): each non-null pointer selects a unit to update and
// receives its new word.
void crt_control87_2(X86 *c) {
    uint32_t x86 = arg(c, 2), sse2 = arg(c, 3);
    if (x86)
        wr32(x86, x87_control87(c, arg(c, 0), arg(c, 1)));
    if (sse2)
        wr32(sse2, SSE2_DEFAULT);
    set_eax(c, 1);
}
void crt_statusfp(X86 *c) {
    set_eax(c, x87_statusfp(c));
}
void crt_clearfp(X86 *c) {
    set_eax(c, x87_clearfp(c));
}
void crt_fpreset(X86 *c) {
    x87_fpreset(c);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "_controlfp", ARGC_CDECL, crt_controlfp},
    {"msvcrt.dll", "_control87", ARGC_CDECL, crt_control87},
    {"msvcrt.dll", "__control87_2", ARGC_CDECL, crt_control87_2},
    {"msvcrt.dll", "_statusfp", ARGC_CDECL, crt_statusfp},
    {"msvcrt.dll", "_status87", ARGC_CDECL, crt_statusfp},
    {"msvcrt.dll", "_clearfp", ARGC_CDECL, crt_clearfp},
    {"msvcrt.dll", "_clear87", ARGC_CDECL, crt_clearfp},
    {"msvcrt.dll", "_fpreset", ARGC_CDECL, crt_fpreset},
};

} // namespace

void register_float() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

} // namespace crt
