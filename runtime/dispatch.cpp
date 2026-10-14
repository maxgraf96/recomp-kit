// dispatch.cpp - dispatch for a process whose image table is not linked in.
//
// Normally the image's generated table.c defines recomp_call, recomp_jump,
// recomp_unknown_jump and the tables beside them, and every host links it
// whole (pop_link_gen). A freestanding build (tools/recomp/bundle.py,
// RECOMP_FREESTANDING) links each translated module - the image included -
// into a bundle the host dlopen()s; its table.c registers with the module
// registry (cpu.cpp) instead, so the host has to dispatch without it. These
// weak definitions do that through the registry, and the generated strong
// ones replace them whenever an image table is linked. A static archive is
// not searched for a symbol that is already defined, even weakly, which is
// why the generated table must stay WHOLE_ARCHIVE-linked.
//
// The recomp_libm_* functions are the library half of freestanding.h: the
// math a bundle cannot take from a C library it does not link. They are the
// host's libm today, so a bundle computes exactly what the linked build does.
#include "discovery.h"
#include "interp.h"
#include "seh.h"
#include "thunks.h"
#include "x86.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {

// The image's tables, empty: nothing is linked, every entry is a module's.
__attribute__((weak)) extern const uint32_t recomp_func_addrs[1] = {0};
__attribute__((weak)) extern const uint32_t recomp_func_count = 0;
__attribute__((weak)) extern void (*const recomp_base_ptrs[1])(X86 *) = {nullptr};
__attribute__((weak)) extern void (*const recomp_raw_ptrs[1])(X86 *) = {nullptr};
__attribute__((weak)) RecompHookFn recomp_hook_ptrs[1] = {nullptr};
__attribute__((weak)) uint8_t recomp_hooked[1];

__attribute__((weak)) int32_t recomp_index_of(uint32_t) {
    return -1;
}
__attribute__((weak)) int recomp_is_call_return(uint32_t) {
    return 0;
}
__attribute__((weak)) uint32_t recomp_override_count(void) {
    return 0;
}
__attribute__((weak)) uint64_t recomp_override_hash(void) {
    return 1469598103934665603ull; // the generated table's hash of an empty override set
}

// Indirect CALL: a registered module's entry, an import shim, or unknown.
static void dispatch_call(X86 *c, uint32_t target) {
    if (recomp_module_call(c, target))
        return;
    if (target >= GUEST_SHIM_BASE && target < GUEST_SHIM_END) {
        recomp_shim_call(c, target);
        return;
    }
    recomp_unknown_call(c, target);
}

__attribute__((weak)) void recomp_call(X86 *c, uint32_t target) {
    uint32_t ebp = recomp_frame_watch ? c->r[5] : 0u;
    dispatch_call(c, target);
    if (recomp_frame_watch && c->r[5] != ebp)
        recomp_frame_changed(c, target, ebp, c->r[5]);
}

// Indirect JMP: a block entry, a shim, a CALL continuation that returns to
// the pending host caller, or the fatal path.
__attribute__((weak)) void recomp_jump(X86 *c, uint32_t target) {
    if (recomp_seh_pending_target())
        recomp_seh_intercept(c, target);
    if (recomp_module_call(c, target))
        return;
    if (target >= GUEST_SHIM_BASE && target < GUEST_SHIM_END) {
        recomp_shim_call(c, target);
        return;
    }
    if (recomp_module_is_call_return(target)) {
        c->eip = target;
        return;
    }
    recomp_unknown_jump(c, target);
}

// The generated table's fatal path, here so a bundle needs no stdio: record
// the address for --discovered, let the interpreter carry the run past the
// gap if it can, otherwise stop with the target and the jumping instruction.
__attribute__((weak)) void recomp_unknown_jump(X86 *c, uint32_t target) {
    if (recomp_run_thunk(c, target))
        return;
    if (recomp_strict())
        recomp_strict_stop(c, "jump", target, c->eip);
    discovery_note("jump", target, c->eip);
    if (interp_call(c, target))
        return;
    discovery_write();
    fprintf(stderr, "recomp: no block entry for indirect jump to 0x%08x from 0x%08x\n", target,
            c->eip);
    abort();
}

// freestanding.h's library functions. `used` keeps them through dead-code
// stripping: nothing in the host calls them, the bundles it loads do.
#define RECOMP_LIBM __attribute__((used, visibility("default")))
RECOMP_LIBM double recomp_libm_fmod(double x, double y) {
    return std::fmod(x, y);
}
RECOMP_LIBM double recomp_libm_ldexp(double x, int e) {
    return std::ldexp(x, e);
}
RECOMP_LIBM double recomp_libm_scalbn(double x, int e) {
    return std::scalbn(x, e);
}
RECOMP_LIBM double recomp_libm_frexp(double x, int *e) {
    return std::frexp(x, e);
}
RECOMP_LIBM int recomp_libm_ilogb(double x) {
    return std::ilogb(x);
}
RECOMP_LIBM float recomp_libm_nextafterf(float x, float y) {
    return std::nextafter(x, y);
}
RECOMP_LIBM double recomp_libm_sin(double x) {
    return std::sin(x);
}
RECOMP_LIBM double recomp_libm_cos(double x) {
    return std::cos(x);
}
RECOMP_LIBM double recomp_libm_tan(double x) {
    return std::tan(x);
}
RECOMP_LIBM double recomp_libm_atan2(double y, double x) {
    return std::atan2(y, x);
}
RECOMP_LIBM double recomp_libm_log2(double x) {
    return std::log2(x);
}
RECOMP_LIBM double recomp_libm_log1p(double x) {
    return std::log1p(x);
}
RECOMP_LIBM double recomp_libm_expm1(double x) {
    return std::expm1(x);
}

} // extern "C"
