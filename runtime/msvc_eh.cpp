// msvc_eh.cpp - Microsoft Visual C++ exception handling, as msvcrt.dll gives
// it to a 32-bit program: structured exception frames (_except_handler3 and
// its scope tables), C++ exceptions (_CxxThrowException, __CxxFrameHandler and
// the tables the compiler emits beside every function with an EH frame), and
// setjmp/longjmp across both (_setjmp3, longjmp, __CxxLongjmpUnwind).
//
// Written from the public descriptions of these structures (the compiler's
// own ehdata layout as documented in reverse-engineering literature and
// Microsoft's SEH documentation); nothing here is taken from Microsoft's
// runtime sources.
//
// How a frame looks to this code (guest addresses; `reg` is the registration
// node a frame links into FS:[0]):
//
//   C++ (__CxxFrameHandler)          SEH (_except_handler3)
//   reg-4   saved ESP (try blocks)   reg-8   saved ESP            ([ebp-18h])
//   reg+0   next                     reg-4   EXCEPTION_POINTERS*  ([ebp-14h])
//   reg+4   handler: the stub        reg+0   next
//           MOV EAX,FuncInfo; JMP    reg+4   handler
//   reg+8   state                    reg+8   scope table
//   reg+12  saved EBP (EBP = reg+12) reg+12  try level
//                                    reg+16  saved EBP (EBP = reg+16)
//
// Funclets - unwind actions, catch blocks, filters, __finally blocks - are
// code of the establishing function that the runtime calls with EBP set to
// that function's frame, on the dispatcher's own stack. A catch block returns
// the address to continue at; an __except block is jumped to. Both are
// resumption points in the middle of a translated function, reached through
// the checkpoint the translator emits where the frame links itself in
// (recomp_eh_frame_enter; seh.cpp recomp_seh_resume): the host stack unwinds
// to the establishing activation, which runs the continuation as a tail call.
//
// A catch block in progress links a guard registration of its own above the
// frame, as msvcrt does. The guard is what makes a catch block a resumption
// point too (a try block nested inside it continues inside the catch block),
// and what a later unwind past it uses to destroy the exception object the
// block was handling, unless the exception leaving it is that same object
// rethrown.
#include "imports.h"
#include "memory.h"
#include "mods_seam.h"
#include "msvcrt_internal.h"
#include "profile.h"
#include "seh.h"
#include "win32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

void sched_atomic_unwind_to_esp(uint32_t esp);

namespace {

// ---------------------------------------------------------------------------
// Constants of the data structures.
// ---------------------------------------------------------------------------
constexpr uint32_t END_CHAIN = 0xffffffffu;
constexpr uint32_t CXX_EXCEPTION = 0xe06d7363u; // 0xe0000000 | 'msc'
constexpr uint32_t CXX_MAGIC = 0x19930520u;     // ...21 and ...22 add fields at the end
constexpr uint32_t CXX_MAGIC_LAST = 0x19930522u;
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4;
constexpr uint32_t CONTINUE_SEARCH = 1; // a registration handler's disposition
// HandlerType.adjectives
constexpr uint32_t HT_CONST = 1, HT_VOLATILE = 2, HT_REFERENCE = 8;
// CatchableType.properties
constexpr uint32_t CT_SIMPLE = 1, CT_BY_REFERENCE_ONLY = 2, CT_VIRTUAL_BASE = 4;
// ThrowInfo.attributes
constexpr uint32_t TI_CONST = 1, TI_VOLATILE = 2;
// FuncInfo.EHFlags (magic 0x19930522): compiled /EHs, catch(...) takes C++ only.
constexpr uint32_t FI_SYNCHRONOUS = 1;
// Where a funclet the runtime calls returns to: no entry, no shim, so a
// translated RET to it simply returns to the runtime (x86.h recomp_return).
constexpr uint32_t FUNCLET_RETURN = GUEST_RETURN_SENTINEL + 0x10u;
// _setjmp3's _JUMP_BUFFER, and the cookie that says UnwindFunc is valid.
constexpr uint32_t JB_EBP = 0, JB_EBX = 4, JB_EDI = 8, JB_ESI = 12, JB_ESP = 16, JB_EIP = 20,
                   JB_REGISTRATION = 24, JB_TRYLEVEL = 28, JB_COOKIE = 32, JB_UNWINDFUNC = 36,
                   JB_UNWINDDATA = 40, JB_SIZE = 64;
constexpr uint32_t JB_COOKIE_VALUE = 0x56433230u; // "VC20"

[[noreturn]] void eh_fatal(X86 *c, const char *what, uint32_t a, uint32_t b) {
    LOGW("C++/SEH exception handling: %s (%08x, %08x; EIP=%08x ESP=%08x EBP=%08x FS:[0]=%08x)",
         what, a, b, c ? c->eip : 0, c ? c->r[R_ESP] : 0, c ? c->r[R_EBP] : 0,
         c && c->fs_base && gm_valid(c->fs_base, 4) ? rd32(c->fs_base) : 0);
    fflush(stdout);
    fflush(stderr);
    abort();
}

uint32_t head(X86 *c) {
    return rd32(c->fs_base);
}
void set_head(X86 *c, uint32_t reg) {
    wr32(c->fs_base, reg);
}
bool is_entry(uint32_t a) {
    return recomp_index_of(a) >= 0 || recomp_module_lookup(a) >= 0;
}

// Guest structures, read once and validated where they are read.
struct FuncInfo {
    uint32_t at = 0, magic = 0, unwind_map = 0, try_map = 0, flags = 0;
    int32_t max_state = 0;
    uint32_t try_blocks = 0;
};
bool read_funcinfo(uint32_t at, FuncInfo *fi) {
    if (!at || !gm_valid(at, 28))
        return false;
    fi->at = at;
    fi->magic = rd32(at);
    if (fi->magic < CXX_MAGIC || fi->magic > CXX_MAGIC_LAST)
        return false;
    fi->max_state = (int32_t)rd32(at + 4);
    fi->unwind_map = rd32(at + 8);
    fi->try_blocks = rd32(at + 12);
    fi->try_map = rd32(at + 16);
    fi->flags = fi->magic >= 0x19930522u && gm_valid(at + 32, 4) ? rd32(at + 32) : 0;
    if (fi->max_state < 0 || fi->max_state > 0x100000 || fi->try_blocks > 0x10000)
        return false;
    if (fi->max_state && !gm_valid(fi->unwind_map, (uint32_t)fi->max_state * 8))
        return false;
    return !fi->try_blocks || gm_valid(fi->try_map, fi->try_blocks * 20);
}
struct TryBlock {
    int32_t low, high, catch_high;
    uint32_t catches, handlers;
};
TryBlock try_block(const FuncInfo &fi, uint32_t i) {
    uint32_t e = fi.try_map + 20 * i;
    return {(int32_t)rd32(e), (int32_t)rd32(e + 4), (int32_t)rd32(e + 8), rd32(e + 12),
            rd32(e + 16)};
}

// The exception a C++ record carries; `cxx` false for a foreign (SEH) one.
struct Thrown {
    bool cxx = false;
    uint32_t code = 0, object = 0, info = 0;
};
Thrown thrown_of(uint32_t record) {
    Thrown t;
    t.code = rd32(record);
    if (t.code == CXX_EXCEPTION && rd32(record + 16) >= 3) {
        uint32_t magic = rd32(record + 20);
        if (magic >= CXX_MAGIC && magic <= CXX_MAGIC_LAST) {
            t.cxx = true;
            t.object = rd32(record + 24);
            t.info = rd32(record + 28);
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// Per-thread state: the catch blocks in progress (innermost last), and the
// _setjmp3 checkpoints. Each guest thread runs on its own host thread.
// ---------------------------------------------------------------------------
struct ActiveCatch {
    X86 *cpu;
    uint32_t guard;   // its guard registration on the guest stack
    uint32_t reg;     // the frame the catch block belongs to
    int32_t try_high; // the block's states are (try_high, catch_high]
    int32_t catch_high;
    Thrown exception;   // what it is handling (object 0: foreign, or caught by ...)
    uint32_t saved_esp; // the frame's saved-ESP slot before the block ran
    uint32_t esp;       // ESP before the guard was pushed
    uint32_t slot;      // the funclet's return slot
    uint32_t handler;   // the catch funclet
    uint32_t callback_depth;
};
struct JumpRecord {
    X86 *cpu;
    jmp_buf env;
    uint32_t profile_depth, callback_depth, dispatch_depth;
    size_t catches;
    bool armed;
};
struct EhState {
    std::vector<ActiveCatch *> catches;
    std::map<uint32_t, JumpRecord *> jumps; // keyed by the guest _JUMP_BUFFER
    ~EhState() {
        for (ActiveCatch *a : catches)
            delete a;
        for (auto &j : jumps)
            delete j.second;
    }
};
thread_local EhState eh;

// The process-wide handlers set_terminate/set_unexpected install.
uint32_t g_terminate_handler = 0, g_unexpected_handler = 0;
// Trampolines for the runtime's own registrations, allocated once.
uint32_t g_catch_guard = 0, g_terminate_guard = 0, g_eh3_handler = 0;

ActiveCatch *catch_for_guard(X86 *c, uint32_t guard) {
    for (size_t i = eh.catches.size(); i-- > 0;)
        if (eh.catches[i]->cpu == c && eh.catches[i]->guard == guard)
            return eh.catches[i];
    return nullptr;
}
ActiveCatch *innermost_catch(X86 *c) {
    for (size_t i = eh.catches.size(); i-- > 0;)
        if (eh.catches[i]->cpu == c)
            return eh.catches[i];
    return nullptr;
}
bool object_in_use(X86 *c, uint32_t object) {
    for (ActiveCatch *a : eh.catches)
        if (a->cpu == c && a->exception.cxx && a->exception.object == object)
            return true;
    return false;
}
void erase_catch(ActiveCatch *a) {
    for (size_t i = eh.catches.size(); i-- > 0;)
        if (eh.catches[i] == a) {
            eh.catches.erase(eh.catches.begin() + i);
            break;
        }
    delete a;
}

// ---------------------------------------------------------------------------
// Calling guest code from the handlers.
// ---------------------------------------------------------------------------

// A funclet of the frame whose EBP is `ebp`, called as the C++ runtime calls
// it: on the current (dispatcher's) stack, returning to FUNCLET_RETURN.
// Every register but EAX is restored, so the handler that called it preserves
// what its own caller expects.
uint32_t call_funclet(X86 *c, uint32_t funclet, uint32_t ebp) {
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    uint32_t eip = c->eip;
    c->r[R_ESP] -= 4;
    wr32(c->r[R_ESP], FUNCLET_RETURN);
    c->r[R_EBP] = ebp;
    c->eip = funclet;
    recomp_call(c, funclet);
    uint32_t eax = c->r[R_EAX];
    if (c->r[R_ESP] != saved[R_ESP])
        LOGV("EH: funclet %08x returned with ESP=%08x, expected %08x", funclet, c->r[R_ESP],
             saved[R_ESP]);
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
    return eax;
}

// A __thiscall member - a copy constructor or a destructor - with up to two
// stack arguments, which it pops itself.
void call_thiscall(X86 *c, uint32_t fn, uint32_t self, int nargs, uint32_t a0, uint32_t a1) {
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    uint32_t eip = c->eip;
    if (nargs > 1) {
        c->r[R_ESP] -= 4;
        wr32(c->r[R_ESP], a1);
    }
    if (nargs > 0) {
        c->r[R_ESP] -= 4;
        wr32(c->r[R_ESP], a0);
    }
    c->r[R_ESP] -= 4;
    wr32(c->r[R_ESP], FUNCLET_RETURN);
    c->r[R_ECX] = self;
    c->eip = fn;
    recomp_call(c, fn);
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
}

// Links a runtime registration {next, handler, tag} below ESP and returns it.
uint32_t push_registration(X86 *c, uint32_t handler, uint32_t tag) {
    c->r[R_ESP] -= 12;
    uint32_t reg = c->r[R_ESP];
    wr32(reg, head(c));
    wr32(reg + 4, handler);
    wr32(reg + 8, tag);
    set_head(c, reg);
    return reg;
}
void pop_registration(X86 *c, uint32_t reg) {
    if (head(c) != reg)
        eh_fatal(c, "a runtime registration is no longer the chain head", reg, head(c));
    set_head(c, rd32(reg));
    c->r[R_ESP] = reg + 12;
}

// Destructors and copy constructors the runtime runs on the program's behalf
// may not let an exception escape: msvcrt calls terminate() then (the
// terminate guard's handler).
void call_thiscall_guarded(X86 *c, uint32_t fn, uint32_t self, int nargs, uint32_t a0,
                           uint32_t a1) {
    uint32_t esp = c->r[R_ESP];
    uint32_t reg = push_registration(c, g_terminate_guard, 0);
    call_thiscall(c, fn, self, nargs, a0, a1);
    pop_registration(c, reg);
    c->r[R_ESP] = esp;
}
uint32_t call_funclet_guarded(X86 *c, uint32_t funclet, uint32_t ebp) {
    uint32_t esp = c->r[R_ESP];
    uint32_t reg = push_registration(c, g_terminate_guard, 0);
    uint32_t eax = call_funclet(c, funclet, ebp);
    pop_registration(c, reg);
    c->r[R_ESP] = esp;
    return eax;
}

// ---------------------------------------------------------------------------
// terminate() and unexpected().
// ---------------------------------------------------------------------------
[[noreturn]] void terminate_now(X86 *c, const char *why) {
    LOGW("msvcrt: terminate(): %s (return address %08x)", why, rd32(c->r[R_ESP]));
    if (uint32_t handler = g_terminate_handler) {
        // A terminate handler must not return; when it does, abort().
        uint32_t esp = c->r[R_ESP];
        uint32_t reg = push_registration(c, g_terminate_guard, 0);
        guest_call(c, handler);
        pop_registration(c, reg);
        c->r[R_ESP] = esp;
        LOGW("msvcrt: the terminate handler %08x returned; aborting", handler);
    }
    LOGW("msvcrt: abnormal program termination");
    crt::flush_before_fatal_exit();
    guest_call(c, imports_resolve("KERNEL32.dll", "ExitProcess"), 3);
    exit(3); // ExitProcess does not return; this is for the compiler
}

// ---------------------------------------------------------------------------
// C++ frames.
// ---------------------------------------------------------------------------
int32_t state_of(uint32_t reg) {
    return (int32_t)rd32(reg + 8);
}

// Runs the frame's unwind actions from its current state down to `target`,
// each with the state already set to where it leads (a destructor that
// throws sees the frame as it will be).
void unwind_to_state(X86 *c, uint32_t reg, const FuncInfo &fi, int32_t target) {
    int32_t cur = state_of(reg);
    while (cur != target) {
        if (cur < 0 || cur >= fi.max_state)
            eh_fatal(c, "the frame's EH state is outside its unwind map", reg, (uint32_t)cur);
        uint32_t entry = fi.unwind_map + 8 * (uint32_t)cur;
        int32_t next = (int32_t)rd32(entry);
        uint32_t action = rd32(entry + 4);
        wr32(reg + 8, (uint32_t)next);
        if (action) {
            LOGV("EH unwind: frame %08x state %d -> %d, action %08x", reg, cur, next, action);
            call_funclet_guarded(c, action, reg + 12);
        }
        cur = next;
    }
    wr32(reg + 8, (uint32_t)cur);
}

// The pointer-to-member displacement of a base within an object.
uint32_t adjust_pointer(uint32_t p, uint32_t ct) {
    int32_t mdisp = (int32_t)rd32(ct + 8), pdisp = (int32_t)rd32(ct + 12),
            vdisp = (int32_t)rd32(ct + 16);
    uint32_t r = p + (uint32_t)mdisp;
    if (pdisp >= 0) {
        uint32_t vbtable = rd32(p + (uint32_t)pdisp);
        r += (uint32_t)(int32_t)rd32(vbtable + (uint32_t)vdisp) + (uint32_t)pdisp;
    }
    return r;
}

bool same_name(uint32_t a, uint32_t b) {
    if (a == b)
        return true;
    if (!a || !b || !gm_valid(a + 8, 1) || !gm_valid(b + 8, 1))
        return false;
    const char *x = (const char *)gm_ptr(a + 8), *y = (const char *)gm_ptr(b + 8);
    for (uint32_t i = 0; gm_valid(a + 8 + i, 1) && gm_valid(b + 8 + i, 1); ++i) {
        if (x[i] != y[i])
            return false;
        if (!x[i])
            return true;
    }
    return false;
}
bool is_ellipsis(uint32_t td) {
    return !td || !gm_valid(td + 8, 1) || rd8(td + 8) == 0;
}

// Does handler `h` take catchable type `ct` of a throw described by `info`?
bool type_match(uint32_t h, uint32_t ct, uint32_t info) {
    uint32_t adjectives = rd32(h), td = rd32(h + 4);
    if (is_ellipsis(td))
        return true;
    if (!same_name(td, rd32(ct + 4)))
        return false;
    if ((rd32(ct) & CT_BY_REFERENCE_ONLY) && !(adjectives & HT_REFERENCE))
        return false;
    uint32_t attributes = rd32(info);
    if ((attributes & TI_CONST) && !(adjectives & HT_CONST))
        return false;
    if ((attributes & TI_VOLATILE) && !(adjectives & HT_VOLATILE))
        return false;
    return true;
}

// Copies (or refers to) the thrown object in the catching frame's catch
// object, at EBP + dispCatchObj.
void build_catch_object(X86 *c, uint32_t reg, uint32_t h, uint32_t ct, uint32_t object) {
    uint32_t adjectives = rd32(h), td = rd32(h + 4);
    int32_t disp = (int32_t)rd32(h + 8);
    if (is_ellipsis(td) || disp == 0)
        return;
    uint32_t buffer = reg + 12 + (uint32_t)disp;
    if (adjectives & HT_REFERENCE) {
        wr32(buffer, adjust_pointer(object, ct));
        return;
    }
    uint32_t properties = rd32(ct), size = rd32(ct + 20), copy = rd32(ct + 24);
    if (properties & CT_SIMPLE) {
        // A scalar or a pointer: the bytes, and a pointer converted to the
        // base the handler names.
        memmove(gm_ptr(buffer), gm_ptr(object), size);
        if (size == 4 && rd32(buffer))
            wr32(buffer, adjust_pointer(rd32(buffer), ct));
        return;
    }
    uint32_t source = adjust_pointer(object, ct);
    if (!copy) {
        memmove(gm_ptr(buffer), gm_ptr(source), size);
        return;
    }
    if (properties & CT_VIRTUAL_BASE)
        call_thiscall_guarded(c, copy, buffer, 2, source, 1);
    else
        call_thiscall_guarded(c, copy, buffer, 1, source, 0);
}

void destroy_object(X86 *c, const Thrown &t) {
    if (!t.cxx || !t.object || !t.info)
        return;
    if (uint32_t dtor = rd32(t.info + 4)) {
        LOGV("EH: destroying exception object %08x (destructor %08x)", t.object, dtor);
        call_thiscall_guarded(c, dtor, t.object, 0, 0, 0);
    }
}

// Calls a catch block with its guard linked and returns its continuation.
// The guard is also a checkpoint: a try block nested inside this catch block
// resumes here, and the rest of the block runs from its continuation.
uint32_t call_catch_block(X86 *c, ActiveCatch *a) {
    eh.catches.push_back(a);
    a->guard = push_registration(c, g_catch_guard, 0);
    a->callback_depth = recomp_callback_depth();
    jmp_buf *env = recomp_seh_frame_enter_at(c, a->guard);
    c->r[R_ESP] -= 4;
    a->slot = c->r[R_ESP];
    wr32(a->slot, FUNCLET_RETURN);
    c->r[R_EBP] = a->reg + 12;
    c->eip = a->handler;
    uint32_t target = a->handler;
    if (setjmp(*env)) {
        // Resumed inside this block: a nested catch completed. The resumer
        // set EBP, ESP and FS:[0]; nothing here was modified since setjmp
        // but the guest state and the heap record `a`, so the locals stand.
        target = recomp_seh_take_resume(c);
        wr32(a->slot, FUNCLET_RETURN);
    }
    recomp_call(c, target);
    uint32_t continuation = c->r[R_EAX];
    if (head(c) != a->guard)
        eh_fatal(c, "a catch block returned with another registration at the chain head", a->guard,
                 head(c));
    set_head(c, rd32(a->guard));
    recomp_seh_frame_drop(c, a->guard);
    wr32(a->reg - 4, a->saved_esp);
    c->r[R_ESP] = a->esp;
    return continuation;
}

// The continuation of a catch block is either in the establishing function's
// own body or, for a try block written inside another catch block of the
// same frame, inside that catch block.
ActiveCatch *enclosing_catch(X86 *c, uint32_t reg, const TryBlock &tb) {
    for (size_t i = eh.catches.size(); i-- > 0;) {
        ActiveCatch *a = eh.catches[i];
        if (a->cpu == c && a->reg == reg && tb.low > a->try_high && tb.catch_high <= a->catch_high)
            return a;
    }
    return nullptr;
}

// Takes the exception in handler `h` of try block `tb`: unwind everything
// above, the frame down to the try block, build the catch object, run the
// catch block, release the exception, and resume at the continuation.
[[noreturn]] void catch_it(X86 *c, uint32_t record, uint32_t reg, const FuncInfo &fi,
                           const TryBlock &tb, uint32_t h, uint32_t ct, const Thrown &t) {
    ActiveCatch *outer = enclosing_catch(c, reg, tb);
    uint32_t target = outer ? outer->guard : reg;
    LOGV("EH catch: frame %08x handler %08x (try %d..%d) object %08x%s", reg, rd32(h + 12), tb.low,
         tb.high, t.object, outer ? ", inside a catch block" : "");
    if (ct && t.cxx)
        build_catch_object(c, reg, h, ct, t.object);
    recomp_seh_global_unwind(c, target, record);
    unwind_to_state(c, reg, fi, tb.low);
    wr32(reg + 8, (uint32_t)(tb.high + 1));
    ActiveCatch *a = new ActiveCatch{};
    a->cpu = c;
    a->reg = reg;
    a->try_high = tb.high;
    a->catch_high = tb.catch_high;
    a->exception = t;
    a->saved_esp = rd32(reg - 4);
    a->esp = c->r[R_ESP];
    a->handler = rd32(h + 12);
    uint32_t continuation = call_catch_block(c, a);
    Thrown done = a->exception;
    erase_catch(a);
    // The handler is finished with the object unless an enclosing catch
    // block (one that rethrew it into this one) still has it.
    if (!object_in_use(c, done.object))
        destroy_object(c, done);
    if (!is_entry(continuation))
        eh_fatal(c, "the catch block's continuation is not a translated entry", continuation,
                 rd32(h + 12));
    set_head(c, target);
    c->r[R_EBP] = reg + 12;
    c->r[R_ESP] = rd32(reg - 4);
    c->r[R_EAX] = continuation;
    recomp_seh_resume(c, target, continuation);
}

void cxx_frame_handler(X86 *c) {
    uint32_t record = arg(c, 0), reg = arg(c, 1);
    FuncInfo fi;
    if (!read_funcinfo(c->r[R_EAX], &fi))
        eh_fatal(c, "__CxxFrameHandler: EAX does not hold a FuncInfo", c->r[R_EAX], reg);
    uint32_t flags = rd32(record + 4);
    if (flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if (fi.max_state)
            unwind_to_state(c, reg, fi, -1);
        set_eax(c, CONTINUE_SEARCH);
        return;
    }
    set_eax(c, CONTINUE_SEARCH);
    if (!fi.try_blocks)
        return;
    Thrown t = thrown_of(record);
    if (t.cxx && !t.info) {
        // `throw;` - the exception the innermost catch block on this thread
        // is handling, or nothing to rethrow.
        ActiveCatch *a = innermost_catch(c);
        if (!a)
            return;
        t = a->exception;
    }
    bool foreign_ok = !(fi.flags & FI_SYNCHRONOUS);
    uint32_t types = 0, array = 0;
    if (t.cxx) {
        array = rd32(t.info + 12);
        types = rd32(array);
    }
    int32_t state = state_of(reg);
    if (state < -1 || state >= fi.max_state)
        eh_fatal(c, "the frame's EH state is outside its unwind map", reg, (uint32_t)state);
    for (uint32_t i = 0; i < fi.try_blocks; ++i) {
        TryBlock tb = try_block(fi, i);
        if (state < tb.low || state > tb.high)
            continue;
        for (uint32_t k = 0; k < tb.catches; ++k) {
            uint32_t h = tb.handlers + 16 * k;
            if (!t.cxx) {
                // A structured exception reaches catch(...) under /EHa, which
                // is what every MSVC before 7.1 compiled /GX as.
                if (foreign_ok && is_ellipsis(rd32(h + 4)))
                    catch_it(c, record, reg, fi, tb, h, 0, t);
                continue;
            }
            for (uint32_t j = 0; j < types; ++j) {
                uint32_t ct = rd32(array + 4 + 4 * j);
                if (type_match(h, ct, t.info))
                    catch_it(c, record, reg, fi, tb, h, ct, t);
            }
        }
    }
}

// A catch block's guard. Searching, it declines: the frame's own handler,
// next in the chain, owns try blocks inside the catch block. Unwound past,
// the block is abandoned, and with it the exception - unless that exception
// is what is leaving it.
void catch_guard_handler(X86 *c) {
    uint32_t record = arg(c, 0), reg = arg(c, 1);
    set_eax(c, CONTINUE_SEARCH);
    if (!(rd32(record + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)))
        return;
    ActiveCatch *a = catch_for_guard(c, reg);
    if (!a)
        return;
    Thrown leaving = thrown_of(record);
    bool rethrown =
        leaving.cxx && a->exception.cxx && (!leaving.info || leaving.object == a->exception.object);
    Thrown abandoned = a->exception;
    wr32(a->reg - 4, a->saved_esp);
    LOGV("EH: abandoning catch block %08x of frame %08x%s", a->handler, a->reg,
         rethrown ? " (its exception is rethrown)" : "");
    erase_catch(a);
    if (!rethrown && !object_in_use(c, abandoned.object))
        destroy_object(c, abandoned);
}

// Around a destructor or copy constructor the runtime runs: a C++ exception
// escaping it ends the program.
void terminate_guard_handler(X86 *c) {
    uint32_t record = arg(c, 0);
    set_eax(c, CONTINUE_SEARCH);
    if (rd32(record + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return;
    if (thrown_of(record).cxx)
        terminate_now(c, "an exception left a destructor or copy constructor during unwinding");
}

// ---------------------------------------------------------------------------
// Structured exception frames (_except_handler3).
// ---------------------------------------------------------------------------
bool is_eh3_handler(uint32_t handler) {
    if (handler == g_eh3_handler)
        return true;
    // An import thunk: JMP dword ptr [slot] with the slot bound to it.
    return gm_valid(handler, 6) && rd8(handler) == 0xff && rd8(handler + 1) == 0x25 &&
           gm_valid(rd32(handler + 2), 4) && rd32(rd32(handler + 2)) == g_eh3_handler;
}

// Runs the __finally blocks from the frame's try level out to `stop`.
void local_unwind2(X86 *c, uint32_t reg, int32_t stop) {
    uint32_t scope = rd32(reg + 8);
    for (int guard = 0; guard < 0x10000; ++guard) {
        int32_t level = (int32_t)rd32(reg + 12);
        if (level == -1 || level == stop)
            return;
        uint32_t entry = scope + 12 * (uint32_t)level;
        if (level < 0 || !gm_valid(entry, 12))
            eh_fatal(c, "the frame's try level is outside its scope table", reg, (uint32_t)level);
        uint32_t enclosing = rd32(entry), filter = rd32(entry + 4), handler = rd32(entry + 8);
        wr32(reg + 12, enclosing);
        if (!filter) {
            LOGV("SEH: frame %08x level %d __finally %08x", reg, level, handler);
            call_funclet(c, handler, reg + 16);
        }
    }
    eh_fatal(c, "the frame's scope table does not lead out", reg, rd32(reg + 12));
}

void except_handler3(X86 *c) {
    uint32_t record = arg(c, 0), reg = arg(c, 1), context = arg(c, 2);
    set_eax(c, CONTINUE_SEARCH);
    if (rd32(record + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        local_unwind2(c, reg, -1);
        return;
    }
    // GetExceptionInformation() in a filter reads [ebp-14h].
    uint32_t esp = c->r[R_ESP];
    c->r[R_ESP] -= 8;
    uint32_t pointers = c->r[R_ESP];
    wr32(pointers, record);
    wr32(pointers + 4, context);
    wr32(reg - 4, pointers);
    uint32_t scope = rd32(reg + 8);
    int32_t level = (int32_t)rd32(reg + 12);
    for (int guard = 0; level != -1 && guard < 0x10000; ++guard) {
        uint32_t entry = scope + 12 * (uint32_t)level;
        if (level < 0 || !gm_valid(entry, 12))
            eh_fatal(c, "the frame's try level is outside its scope table", reg, (uint32_t)level);
        int32_t enclosing = (int32_t)rd32(entry);
        uint32_t filter = rd32(entry + 4), handler = rd32(entry + 8);
        if (filter) {
            int32_t verdict = (int32_t)call_funclet(c, filter, reg + 16);
            LOGV("SEH: frame %08x level %d filter %08x -> %d", reg, level, filter, verdict);
            if (verdict < 0) {
                c->r[R_ESP] = esp;
                set_eax(c, 0); // EXCEPTION_CONTINUE_EXECUTION
                return;
            }
            if (verdict > 0) {
                // EXCEPTION_EXECUTE_HANDLER: unwind, then enter the __except
                // block, which restores ESP from [ebp-18h] itself. The
                // unwind carries no record (STATUS_UNWIND), as msvcrt's
                // _global_unwind2 does: no C++ object leaves with it.
                recomp_seh_global_unwind(c, reg, 0);
                local_unwind2(c, reg, level);
                wr32(reg + 12, (uint32_t)enclosing);
                if (!is_entry(handler))
                    eh_fatal(c, "the __except block is not a translated entry", handler, reg);
                set_head(c, reg);
                c->r[R_EBP] = reg + 16;
                c->r[R_ESP] = rd32(reg - 8);
                c->r[R_EAX] = rd32(record);
                recomp_seh_resume(c, reg, handler);
            }
        }
        level = enclosing;
    }
    c->r[R_ESP] = esp;
}

// ---------------------------------------------------------------------------
// Throwing.
// ---------------------------------------------------------------------------
// `address` is where the throw is reported from: the shim's return address.
[[noreturn]] void raise_cxx(X86 *c, uint32_t object, uint32_t info, uint32_t address,
                            const char *where) {
    c->r[R_ESP] -= 12;
    uint32_t params = c->r[R_ESP];
    wr32(params, CXX_MAGIC);
    wr32(params + 4, object);
    wr32(params + 8, info);
    if (info)
        LOGV("EH throw: object %08x info %08x from %08x (%s)", object, info, address, where);
    else
        LOGV("EH rethrow from %08x", address);
    recomp_seh_raise_at(c, CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 3, params, address);
    // Nothing took it.
    Thrown t{true, CXX_EXCEPTION, object, info};
    if (!info)
        if (ActiveCatch *a = innermost_catch(c))
            t = a->exception;
    std::string what = win32_describe_cxx_throw(CXX_EXCEPTION, 3, params);
    if (t.info != info) {
        uint32_t again[3] = {CXX_MAGIC, t.object, t.info};
        memcpy(gm_ptr(params), again, sizeof again);
        what = win32_describe_cxx_throw(CXX_EXCEPTION, 3, params);
    }
    LOGW("msvcrt: unhandled C++ exception thrown from %08x%s%s", address, what.empty() ? "" : ": ",
         what.c_str());
    for (uint32_t ret : win32_return_chain(c->r[R_EBP], 12))
        LOGW("  frame returns to %08x", ret);
    c->r[R_ESP] += 12;
    terminate_now(c,
                  info || t.info ? "unhandled exception" : "throw; with no exception to rethrow");
}

// void __stdcall _CxxThrowException(void *object, const ThrowInfo *info)
void cxx_throw_exception(X86 *c) {
    raise_cxx(c, arg(c, 0), arg(c, 1), rd32(c->r[R_ESP]), "_CxxThrowException");
}

// ---------------------------------------------------------------------------
// setjmp/longjmp.
// ---------------------------------------------------------------------------

// void __stdcall __CxxLongjmpUnwind(_JUMP_BUFFER *): the C++ frame that took
// the setjmp, unwound to the state it had then.
void cxx_longjmp_unwind(X86 *c) {
    uint32_t buf = arg(c, 0);
    FuncInfo fi;
    if (!read_funcinfo(rd32(buf + JB_UNWINDDATA), &fi))
        eh_fatal(c, "__CxxLongjmpUnwind: the jump buffer holds no FuncInfo", buf,
                 rd32(buf + JB_UNWINDDATA));
    unwind_to_state(c, rd32(buf + JB_REGISTRATION), fi, (int32_t)rd32(buf + JB_TRYLEVEL));
}

// Reached only when a call site was not translated into the two-call form.
void setjmp3_import(X86 *c) {
    eh_fatal(c,
             "_setjmp3 was called as an ordinary import; the call site must be translated as "
             "recomp_setjmp3_prepare + setjmp (the host setjmp has to be taken in the caller)",
             arg(c, 0), rd32(c->r[R_ESP]));
}

// void longjmp(jmp_buf, int)
void long_jump(X86 *c) {
    uint32_t buf = arg(c, 0), value = arg(c, 1);
    auto it = eh.jumps.find(buf);
    if (it == eh.jumps.end() || !it->second->armed || it->second->cpu != c)
        eh_fatal(c, "longjmp to a jump buffer no _setjmp3 on this thread filled", buf,
                 rd32(c->r[R_ESP]));
    JumpRecord *j = it->second;
    uint32_t reg = rd32(buf + JB_REGISTRATION);
    LOGV("longjmp(%08x, %u): registration %08x, head %08x", buf, value, reg, head(c));
    // Every frame linked since the setjmp unwinds first: their destructors,
    // __finally blocks and the catch blocks they were in.
    if (reg != head(c))
        recomp_seh_global_unwind(c, reg == END_CHAIN ? 0 : reg, 0);
    if (reg != END_CHAIN && reg) {
        if (rd32(buf + JB_COOKIE) == JB_COOKIE_VALUE) {
            if (uint32_t unwind = rd32(buf + JB_UNWINDFUNC))
                guest_call(c, unwind, buf);
        } else if (is_eh3_handler(rd32(reg + 4))) {
            local_unwind2(c, reg, (int32_t)rd32(buf + JB_TRYLEVEL));
        }
    }
    c->r[R_EBP] = rd32(buf + JB_EBP);
    c->r[R_EBX] = rd32(buf + JB_EBX);
    c->r[R_EDI] = rd32(buf + JB_EDI);
    c->r[R_ESI] = rd32(buf + JB_ESI);
    c->r[R_ESP] = rd32(buf + JB_ESP);
    c->eip = rd32(buf + JB_EIP);
    c->r[R_EAX] = value ? value : 1;
    recomp_eh_frame_unlink(c); // checkpoints of the frames just unwound
    mods_hooks_unwind_to_esp(c->r[R_ESP]);
    sched_atomic_unwind_to_esp(c->r[R_ESP]);
    recomp_profile_truncate(j->profile_depth);
    recomp_seh_callback_leave(c, j->callback_depth + 1);
    recomp_callback_truncate(j->callback_depth);
    recomp_seh_dispatch_truncate(j->dispatch_depth);
    while (eh.catches.size() > j->catches)
        erase_catch(eh.catches.back());
    longjmp(j->env, 1);
}

// ---------------------------------------------------------------------------
// The library's own exception: std::bad_cast as msvcrt's dynamic_cast throws
// it. msvcrt's exception classes are global (".?AVbad_cast@@", derived from
// ".?AVexception@@"): {vftable, what, owns-what}.
// ---------------------------------------------------------------------------
struct BadCastInfo {
    uint32_t epoch = 0, throw_info = 0, vftable = 0;
};
BadCastInfo g_bad_cast;

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = heap_alloc(n);
    if (a)
        memcpy(gm_ptr(a), s, n);
    return a;
}
void exception_release(uint32_t self) {
    if (rd32(self + 8) && rd32(self + 4))
        heap_free(rd32(self + 4));
    wr32(self + 4, 0);
    wr32(self + 8, 0);
}
void exception_dtor(X86 *c) {
    exception_release(c->r[R_ECX]);
}
void exception_deleting_dtor(X86 *c) {
    uint32_t self = c->r[R_ECX];
    exception_release(self);
    if (arg(c, 0) & 1)
        heap_free(self);
    set_eax(c, self);
}
void exception_copy(X86 *c) {
    uint32_t self = c->r[R_ECX], from = arg(c, 0);
    wr32(self, rd32(from));
    uint32_t what = rd32(from + 4);
    wr32(self + 4, what && rd32(from + 8) ? guest_strdup(gm_str(what).c_str()) : what);
    wr32(self + 8, rd32(from + 8));
    set_eax(c, self);
}
void exception_what(X86 *c) {
    uint32_t what = rd32(c->r[R_ECX] + 4);
    set_eax(c, what);
}

uint32_t type_descriptor(const char *name) {
    uint32_t n = (uint32_t)strlen(name) + 1;
    uint32_t td = heap_alloc(8 + n, true);
    memcpy(gm_ptr(td + 8), name, n);
    return td;
}

const BadCastInfo &bad_cast_info() {
    if (g_bad_cast.throw_info && g_bad_cast.epoch == crt::heap_epoch())
        return g_bad_cast;
    BadCastInfo &b = g_bad_cast;
    b.epoch = crt::heap_epoch();
    uint32_t dtor =
        imports_alloc_trampoline("msvcrt.dll", "bad_cast::~bad_cast", exception_dtor, 0);
    uint32_t copy = imports_alloc_trampoline("msvcrt.dll", "bad_cast::bad_cast(const bad_cast &)",
                                             exception_copy, 1);
    b.vftable = heap_alloc(8, true);
    wr32(b.vftable, imports_alloc_trampoline("msvcrt.dll", "bad_cast::`scalar deleting destructor'",
                                             exception_deleting_dtor, 1));
    wr32(b.vftable + 4,
         imports_alloc_trampoline("msvcrt.dll", "bad_cast::what", exception_what, 0));
    const char *names[2] = {".?AVbad_cast@@", ".?AVexception@@"};
    uint32_t array = heap_alloc(12, true);
    wr32(array, 2);
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t ct = heap_alloc(28, true);
        wr32(ct, 0);
        wr32(ct + 4, type_descriptor(names[i]));
        wr32(ct + 8, 0);
        wr32(ct + 12, 0xffffffffu); // pdisp -1: not a virtual base
        wr32(ct + 16, 0);
        wr32(ct + 20, 12);
        wr32(ct + 24, copy);
        wr32(array + 4 + 4 * i, ct);
    }
    b.throw_info = heap_alloc(16, true);
    wr32(b.throw_info + 4, dtor);
    wr32(b.throw_info + 12, array);
    return b;
}

// ---------------------------------------------------------------------------
// terminate/unexpected and their handlers.
// ---------------------------------------------------------------------------
void crt_terminate(X86 *c) {
    terminate_now(c, "called by the program");
}
// terminate_function set_terminate(terminate_function)
void crt_set_terminate(X86 *c) {
    uint32_t old = g_terminate_handler;
    g_terminate_handler = arg(c, 0);
    set_eax(c, old);
}
void crt_unexpected(X86 *c) {
    if (uint32_t handler = g_unexpected_handler)
        guest_call(c, handler);
    terminate_now(c, "unexpected()");
}
void crt_set_unexpected(X86 *c) {
    uint32_t old = g_unexpected_handler;
    g_unexpected_handler = arg(c, 0);
    set_eax(c, old);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "_CxxThrowException", 2, cxx_throw_exception},
    {"msvcrt.dll", "__CxxFrameHandler", ARGC_CDECL, cxx_frame_handler},
    {"msvcrt.dll", "__CxxLongjmpUnwind", 1, cxx_longjmp_unwind},
    {"msvcrt.dll", "_except_handler3", ARGC_CDECL, except_handler3},
    {"msvcrt.dll", "_setjmp3", ARGC_CDECL, setjmp3_import},
    {"msvcrt.dll", "longjmp", ARGC_CDECL, long_jump},
    {"msvcrt.dll", "?terminate@@YAXXZ", ARGC_CDECL, crt_terminate},
    {"msvcrt.dll", "?set_terminate@@YAP6AXXZP6AXXZ@Z", ARGC_CDECL, crt_set_terminate},
    {"msvcrt.dll", "?unexpected@@YAXXZ", ARGC_CDECL, crt_unexpected},
    {"msvcrt.dll", "?set_unexpected@@YAP6AXXZP6AXXZ@Z", ARGC_CDECL, crt_set_unexpected},
};

} // namespace

// ---------------------------------------------------------------------------
// Entry points.
// ---------------------------------------------------------------------------
void msvc_eh_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    if (!g_catch_guard) {
        g_catch_guard = imports_alloc_trampoline("msvcrt.dll", "<catch block guard>",
                                                 catch_guard_handler, ARGC_CDECL);
        g_terminate_guard = imports_alloc_trampoline("msvcrt.dll", "<terminate guard>",
                                                     terminate_guard_handler, ARGC_CDECL);
    }
    g_eh3_handler = imports_resolve("msvcrt.dll", "_except_handler3");
}

// dynamic_cast to a reference that fails: throw std::bad_cast("Bad dynamic_cast!").
void msvc_eh_throw_bad_cast(X86 *c) {
    const BadCastInfo &b = bad_cast_info();
    c->r[R_ESP] -= 16;
    uint32_t object = c->r[R_ESP];
    wr32(object, b.vftable);
    wr32(object + 4, guest_strdup("Bad dynamic_cast!"));
    wr32(object + 8, 1);
    raise_cxx(c, object, b.throw_info, rd32(object + 16), "__RTDynamicCast");
}

void msvc_eh_reset(X86 *c) {
    for (size_t i = eh.catches.size(); i-- > 0;)
        if (!c || eh.catches[i]->cpu == c) {
            delete eh.catches[i];
            eh.catches.erase(eh.catches.begin() + i);
        }
    for (auto it = eh.jumps.begin(); it != eh.jumps.end();)
        if (!c || it->second->cpu == c) {
            delete it->second;
            it = eh.jumps.erase(it);
        } else {
            ++it;
        }
    if (!c)
        g_terminate_handler = g_unexpected_handler = 0;
}

// A callback that returns abandons the catch blocks it ran.
void msvc_eh_callback_leave(X86 *c, uint32_t depth) {
    for (size_t i = eh.catches.size(); i-- > 0;)
        if (eh.catches[i]->cpu == c && eh.catches[i]->callback_depth >= depth) {
            delete eh.catches[i];
            eh.catches.erase(eh.catches.begin() + i);
        }
}

extern "C" {

// The call site has pushed its return address; the guest's arguments are
// above it: (_JUMP_BUFFER *buf, int count, unwind function, try level,
// unwind data...).
jmp_buf *recomp_setjmp3_prepare(X86 *c) {
    uint32_t esp = c->r[R_ESP];
    uint32_t buf = rd32(esp + 4), count = rd32(esp + 8);
    if (!gm_valid(buf, JB_SIZE))
        eh_fatal(c, "_setjmp3 with an invalid jump buffer", buf, rd32(esp));
    JumpRecord *&j = eh.jumps[buf];
    if (!j)
        j = new JumpRecord{};
    j->cpu = c;
    j->profile_depth = recomp_profile_depth();
    j->callback_depth = recomp_callback_depth();
    j->dispatch_depth = recomp_seh_dispatch_depth();
    j->catches = eh.catches.size();
    j->armed = true;
    uint32_t reg = head(c);
    memset(gm_ptr(buf), 0, JB_SIZE);
    wr32(buf + JB_EBP, c->r[R_EBP]);
    wr32(buf + JB_EBX, c->r[R_EBX]);
    wr32(buf + JB_EDI, c->r[R_EDI]);
    wr32(buf + JB_ESI, c->r[R_ESI]);
    wr32(buf + JB_ESP, esp + 4);
    wr32(buf + JB_EIP, rd32(esp));
    wr32(buf + JB_REGISTRATION, reg);
    if (reg == END_CHAIN) {
        wr32(buf + JB_TRYLEVEL, END_CHAIN);
    } else if (!count) {
        wr32(buf + JB_TRYLEVEL, rd32(reg + 12)); // an _except_handler3 frame's try level
    } else {
        wr32(buf + JB_COOKIE, JB_COOKIE_VALUE);
        wr32(buf + JB_UNWINDFUNC, rd32(esp + 12));
        wr32(buf + JB_TRYLEVEL, count > 1 ? rd32(esp + 16) : rd32(reg + 12));
        for (uint32_t i = 0; i + 2 < count && i < 6; ++i)
            wr32(buf + JB_UNWINDDATA + 4 * i, rd32(esp + 20 + 4 * i));
    }
    LOGV("_setjmp3(%08x, %u): registration %08x ESP=%08x", buf, count, reg, esp + 4);
    return &j->env;
}

// longjmp has restored the registers the jump buffer holds and EAX.
void recomp_setjmp3_resume(X86 *c) {
    LOGV("_setjmp3 returns %u from longjmp (ESP=%08x)", c->r[R_EAX], c->r[R_ESP]);
}

} // extern "C"

#ifdef POPM_TESTING
// Catch blocks in progress on this thread for `c`.
uint32_t msvc_eh_test_catch_count(X86 *c) {
    uint32_t n = 0;
    for (ActiveCatch *a : eh.catches)
        n += a->cpu == c;
    return n;
}
#endif
