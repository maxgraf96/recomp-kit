// MSVC exception handling (msvc_eh.cpp) over synthetic guest functions.
//
// Each "guest function" here is written the way the translator emits one:
// guest registers and memory only, CALL as push-return-address plus
// recomp_call, RET through recomp_return, and - in a frame with a try block or
// an __except scope - the checkpoint the translator emits at the frame's
// `MOV FS:[0],ESP` and the unlink after its `MOV FS:[0],ECX`. The data the
// runtime reads (FuncInfo, unwind and try-block maps, handler types,
// ThrowInfo, catchable types, scope tables) is built in guest memory the way
// MSVC 6 lays it out.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../intrinsics.h"
#include "../seh.h"
#include "../msvcrt_internal.h"
#include "../../platform/os.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

uint32_t msvc_eh_test_catch_count(X86 *c);

static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// A tiny address space of guest functions, and the dispatch the generated
// tables would provide.
// ---------------------------------------------------------------------------
typedef void (*GuestFn)(X86 *);
static constexpr uint32_t CODE = 0x0d040000;     // guest function addresses
static constexpr uint32_t RET_ADDR = 0x0d05fff0; // the return address every call site pushes
static std::map<uint32_t, GuestFn> &functions() {
    static std::map<uint32_t, GuestFn> m;
    return m;
}
static std::map<uint32_t, uint32_t> &stubs() { // EH stub -> FuncInfo
    static std::map<uint32_t, uint32_t> m;
    return m;
}
static uint32_t next_code = CODE;
static uint32_t def(GuestFn fn) {
    uint32_t a = next_code;
    next_code += 0x10;
    functions()[a] = fn;
    return a;
}
static uint32_t frame_handler_import, throw_import, eh3_import, setjmp3_import, longjmp_import,
    cxx_longjmp_unwind_import, raise_import, set_terminate_import, exit_import;

extern "C" int recomp_is_call_return(uint32_t target) {
    return target == RET_ADDR;
}
extern "C" int32_t recomp_index_of(uint32_t target) {
    return functions().count(target) || stubs().count(target) ? 0 : -1;
}
extern "C" void recomp_call(X86 *c, uint32_t target) {
    auto f = functions().find(target);
    if (f != functions().end()) {
        f->second(c);
        return;
    }
    auto s = stubs().find(target);
    if (s != stubs().end()) {
        // MOV EAX, FuncInfo; JMP __CxxFrameHandler
        c->r[R_EAX] = s->second;
        CHECK(imports_dispatch(c, frame_handler_import));
        return;
    }
    if (!imports_dispatch(c, target)) {
        fprintf(stderr, "eh_tests: call to unknown %08x\n", target);
        CHECK(false);
        abort();
    }
}
extern "C" void recomp_jump(X86 *c, uint32_t target) {
    recomp_call(c, target);
}

// ---------------------------------------------------------------------------
// Instruction-level helpers, as the generated code spells them.
// ---------------------------------------------------------------------------
static void push(X86 *c, uint32_t v) {
    c->r[R_ESP] -= 4;
    wr32(c->r[R_ESP], v);
}
static uint32_t pop(X86 *c) {
    uint32_t v = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    return v;
}
static void call(X86 *c, uint32_t target) {
    push(c, RET_ADDR);
    recomp_call(c, target);
}
static void ret(X86 *c, uint32_t n = 0) {
    c->eip = pop(c);
    c->r[R_ESP] += n;
    recomp_return(c);
}
static uint32_t ebp(X86 *c, int32_t off) {
    return c->r[R_EBP] + (uint32_t)off;
}

// push ebp; mov ebp,esp; push -1; push stub; mov eax,fs:[0]; push eax;
// mov fs:[0],esp [checkpoint]; sub esp,locals; push ebx; push esi; push edi;
// mov [ebp-10h],esp. `locals` includes the saved-ESP slot at [ebp-10h].
#define CXX_PROLOGUE(stub, locals, checkpoint)                                                     \
    push(c, c->r[R_EBP]);                                                                          \
    c->r[R_EBP] = c->r[R_ESP];                                                                     \
    push(c, 0xffffffffu);                                                                          \
    push(c, (stub));                                                                               \
    push(c, rd32(c->fs_base));                                                                     \
    wr32(c->fs_base, c->r[R_ESP]);                                                                 \
    if (checkpoint) {                                                                              \
        jmp_buf *b_ = recomp_eh_frame_enter(c);                                                    \
        if (setjmp(*b_)) {                                                                         \
            recomp_seh_land(c);                                                                    \
            return;                                                                                \
        }                                                                                          \
    }                                                                                              \
    c->r[R_ESP] -= (locals);                                                                       \
    push(c, c->r[R_EBX]);                                                                          \
    push(c, c->r[R_ESI]);                                                                          \
    push(c, c->r[R_EDI]);                                                                          \
    wr32(ebp(c, -0x10), c->r[R_ESP])

// The _except_handler3 frame: push ebp; mov ebp,esp; push -1; push scope;
// push handler; mov eax,fs:[0]; push eax; mov fs:[0],esp [checkpoint];
// sub esp,locals; push ebx; push esi; push edi; mov [ebp-18h],esp.
#define SEH_PROLOGUE(scope, locals)                                                                \
    push(c, c->r[R_EBP]);                                                                          \
    c->r[R_EBP] = c->r[R_ESP];                                                                     \
    push(c, 0xffffffffu);                                                                          \
    push(c, (scope));                                                                              \
    push(c, eh3_import);                                                                           \
    push(c, rd32(c->fs_base));                                                                     \
    wr32(c->fs_base, c->r[R_ESP]);                                                                 \
    {                                                                                              \
        jmp_buf *b_ = recomp_eh_frame_enter(c);                                                    \
        if (setjmp(*b_)) {                                                                         \
            recomp_seh_land(c);                                                                    \
            return;                                                                                \
        }                                                                                          \
    }                                                                                              \
    c->r[R_ESP] -= (locals);                                                                       \
    push(c, c->r[R_EBX]);                                                                          \
    push(c, c->r[R_ESI]);                                                                          \
    push(c, c->r[R_EDI]);                                                                          \
    wr32(ebp(c, -0x18), c->r[R_ESP])

// mov ecx,[ebp-0Ch]; mov fs:[0],ecx [unlink]; pop edi; pop esi; pop ebx;
// mov esp,ebp; pop ebp; ret n. `top` is where the saved registers start.
static void epilogue(X86 *c, uint32_t top, uint32_t n, bool unlink, int32_t next_at = -0x0c) {
    wr32(c->fs_base, rd32(ebp(c, next_at)));
    if (unlink)
        recomp_eh_frame_unlink(c);
    c->r[R_ESP] = top - 12;
    c->r[R_EDI] = pop(c);
    c->r[R_ESI] = pop(c);
    c->r[R_EBX] = pop(c);
    c->r[R_ESP] = c->r[R_EBP];
    c->r[R_EBP] = pop(c);
    ret(c, n);
}
static void state(X86 *c, int32_t s) {
    wr32(ebp(c, -4), (uint32_t)s);
}

// ---------------------------------------------------------------------------
// The compiler's tables, in guest memory.
// ---------------------------------------------------------------------------
static uint32_t galloc(uint32_t n) {
    uint32_t a = heap_alloc(n, true);
    CHECK(a != 0);
    return a;
}
static uint32_t type_descriptor(const char *name) {
    uint32_t n = (uint32_t)strlen(name) + 1, td = galloc(8 + n);
    memcpy(gm_ptr(td + 8), name, n);
    return td;
}
struct Handler {
    uint32_t adjectives, type;
    int32_t disp;
    uint32_t funclet;
};
struct Try {
    int32_t low, high, catch_high;
    std::vector<Handler> handlers;
};
static uint32_t func_info(std::vector<std::pair<int32_t, uint32_t>> unwind,
                          std::vector<Try> tries) {
    uint32_t fi = galloc(28), map = galloc(8 * (uint32_t)unwind.size() + 8),
             tmap = galloc(20 * (uint32_t)tries.size() + 4);
    wr32(fi, 0x19930520);
    wr32(fi + 4, (uint32_t)unwind.size());
    wr32(fi + 8, map);
    wr32(fi + 12, (uint32_t)tries.size());
    wr32(fi + 16, tmap);
    for (size_t i = 0; i < unwind.size(); ++i) {
        wr32(map + 8 * (uint32_t)i, (uint32_t)unwind[i].first);
        wr32(map + 8 * (uint32_t)i + 4, unwind[i].second);
    }
    for (size_t i = 0; i < tries.size(); ++i) {
        const Try &t = tries[i];
        uint32_t e = tmap + 20 * (uint32_t)i, hs = galloc(16 * (uint32_t)t.handlers.size() + 4);
        wr32(e, (uint32_t)t.low);
        wr32(e + 4, (uint32_t)t.high);
        wr32(e + 8, (uint32_t)t.catch_high);
        wr32(e + 12, (uint32_t)t.handlers.size());
        wr32(e + 16, hs);
        for (size_t k = 0; k < t.handlers.size(); ++k) {
            const Handler &h = t.handlers[k];
            uint32_t a = hs + 16 * (uint32_t)k;
            wr32(a, h.adjectives);
            wr32(a + 4, h.type);
            wr32(a + 8, (uint32_t)h.disp);
            wr32(a + 12, h.funclet);
        }
    }
    return fi;
}
static uint32_t stub_for(uint32_t fi) {
    uint32_t a = next_code;
    next_code += 0x10;
    stubs()[a] = fi;
    return a;
}
static uint32_t catchable(uint32_t properties, uint32_t type, int32_t mdisp, uint32_t size,
                          uint32_t copy) {
    uint32_t ct = galloc(28);
    wr32(ct, properties);
    wr32(ct + 4, type);
    wr32(ct + 8, (uint32_t)mdisp);
    wr32(ct + 12, 0xffffffffu);
    wr32(ct + 16, 0);
    wr32(ct + 20, size);
    wr32(ct + 24, copy);
    return ct;
}
static uint32_t throw_info(uint32_t dtor, std::vector<uint32_t> types, uint32_t attributes = 0) {
    uint32_t ti = galloc(16), array = galloc(4 + 4 * (uint32_t)types.size());
    wr32(array, (uint32_t)types.size());
    for (size_t i = 0; i < types.size(); ++i)
        wr32(array + 4 + 4 * (uint32_t)i, types[i]);
    wr32(ti, attributes);
    wr32(ti + 4, dtor);
    wr32(ti + 12, array);
    return ti;
}
static uint32_t scope_table(std::vector<std::vector<uint32_t>> entries) {
    uint32_t t = galloc(12 * (uint32_t)entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        for (size_t k = 0; k < 3; ++k)
            wr32(t + 12 * (uint32_t)i + 4 * (uint32_t)k, entries[i][k]);
    return t;
}

// ---------------------------------------------------------------------------
// Observations.
// ---------------------------------------------------------------------------
static std::vector<std::string> events;
static void ev(const char *fmt, uint32_t v = 0) {
    char b[64];
    snprintf(b, sizeof b, fmt, v);
    events.push_back(b);
}
static bool events_are(std::vector<std::string> want) {
    if (events == want)
        return true;
    fprintf(stderr, "events:");
    for (auto &e : events)
        fprintf(stderr, " %s", e.c_str());
    fprintf(stderr, "\nwanted:");
    for (auto &e : want)
        fprintf(stderr, " %s", e.c_str());
    fprintf(stderr, "\n");
    return false;
}

// The types: Base {tag}; Derived {pad, Base at +4}; Other; int; pointers.
static uint32_t td_base, td_derived, td_other, td_int, td_pderived, td_pbase, td_exception,
    td_bad_cast;
static uint32_t ti_derived, ti_int, ti_pderived, ti_other;
static uint32_t obj_dtor, derived_dtor, derived_copy;

// A local object's destructor (thiscall): logs its tag.
static void obj_dtor_fn(X86 *c) {
    ev("dtor:%x", rd32(c->r[R_ECX]));
    ret(c);
}
// Derived::~Derived and Derived::Derived(const Derived &).
static void derived_dtor_fn(X86 *c) {
    ev("xdtor:%x", rd32(c->r[R_ECX] + 4));
    ret(c);
}
static void derived_copy_fn(X86 *c) {
    uint32_t self = c->r[R_ECX], from = rd32(c->r[R_ESP] + 4);
    wr32(self, rd32(from));
    wr32(self + 4, rd32(from + 4) | 0x100);
    ev("copy:%x", rd32(from + 4));
    c->r[R_EAX] = self;
    ret(c, 4);
}

// Throws (in a function of its own, as the compiler's helpers do): builds the
// object on its stack and calls _CxxThrowException.
static uint32_t pending_tag, pending_kind; // what thrower throws next
static void thrower(X86 *c) {
    c->r[R_ESP] -= 16;
    uint32_t object = c->r[R_ESP];
    uint32_t info = 0;
    switch (pending_kind) {
    case 0: // Derived
        wr32(object, 0xd0d0d0d0);
        wr32(object + 4, pending_tag);
        info = ti_derived;
        break;
    case 1: // int
        wr32(object, pending_tag);
        info = ti_int;
        break;
    case 2: // Derived *: a pointer to a heap Derived
    {
        uint32_t d = galloc(8);
        wr32(d + 4, pending_tag);
        wr32(object, d);
        info = ti_pderived;
        break;
    }
    case 3: // Other
        wr32(object, pending_tag);
        info = ti_other;
        break;
    }
    push(c, info);
    push(c, object);
    call(c, throw_import);
    CHECK(false); // _CxxThrowException does not return
}
static uint32_t thrower_fn;

// ---------------------------------------------------------------------------
// 1. A throw two frames down, caught by reference to a base class; the frame
// in between destroys its local; the exception object is destroyed after the
// catch block; the continuation runs with the frame's ESP and EBP.
// ---------------------------------------------------------------------------
static uint32_t f1, f1_catch, f1_cont, g1, g1_unwind, f1_stub, g1_stub;
static uint32_t f1_caught_ref, f1_saved_esp;
static void g1_fn(X86 *c) {
    CXX_PROLOGUE(g1_stub, 0x10, false);
    uint32_t top = c->r[R_ESP] + 12;
    wr32(ebp(c, -0x14), 0x61); // Obj o(0x61)
    state(c, 0);
    call(c, thrower_fn);
    state(c, -1);
    c->r[R_ECX] = ebp(c, -0x14);
    call(c, obj_dtor);
    epilogue(c, top, 0, false);
}
static void g1_unwind_fn(X86 *c) { // lea ecx,[ebp-14h]; jmp Obj::~Obj
    c->r[R_ECX] = ebp(c, -0x14);
    recomp_call(c, obj_dtor);
}
static void f1_fn(X86 *c) {
    CXX_PROLOGUE(f1_stub, 0x20, true);
    f1_saved_esp = c->r[R_ESP];
    wr32(ebp(c, -0x24), 0x1234); // a local the continuation reads back
    state(c, 0);
    call(c, g1);
    state(c, -1);
    ev("no throw");
    recomp_call(c, f1_cont);
}
static void f1_catch_fn(X86 *c) {
    ev("catch1:%x", rd32(rd32(ebp(c, -0x18)))); // the Base subobject's tag
    f1_caught_ref = rd32(ebp(c, -0x18));
    CHECK(rd32(ebp(c, -0x24)) == 0x1234); // the frame's EBP
    CHECK(rd32(ebp(c, -4)) == 1);         // tryHigh + 1
    c->r[R_EAX] = f1_cont;
    ret(c);
}
static void f1_cont_fn(X86 *c) {
    ev("cont1");
    CHECK(c->r[R_ESP] == f1_saved_esp);
    CHECK(rd32(ebp(c, -0x24)) == 0x1234);
    CHECK(rd32(c->fs_base) == ebp(c, -0x0c));
    epilogue(c, f1_saved_esp + 12, 0, true);
}

static void test_basic(X86 *c) {
    events.clear();
    pending_kind = 0;
    pending_tag = 0x77;
    uint32_t esp = c->r[R_ESP], head = rd32(c->fs_base);
    c->r[R_EBX] = 0xb0b0b0b0;
    c->r[R_EBP] = 0xebebebeb;
    // The x87 control word is the guest's: nothing on the way to the
    // continuation saves or restores it (24-bit precision, round down).
    uint16_t cw = c->fpu_cw;
    x87_set_cw(c, 0x047f);
    guest_call(c, f1);
    CHECK(c->fpu_cw == 0x047f);
    x87_set_cw(c, cw);
    CHECK(events_are({"dtor:61", "catch1:77", "xdtor:77", "cont1"}));
    CHECK(c->r[R_ESP] == esp && rd32(c->fs_base) == head);
    CHECK(c->r[R_EBX] == 0xb0b0b0b0 && c->r[R_EBP] == 0xebebebeb);
    CHECK(recomp_seh_test_frame_count(c) == 0);
    CHECK(msvc_eh_test_catch_count(c) == 0);
    CHECK(recomp_seh_dispatch_depth() == 0);
    CHECK(recomp_callback_depth() == 0);
}

// ---------------------------------------------------------------------------
// 2. One try block, four handlers: int by value, Derived by value (copy
// constructor), Base * (pointer conversion), catch(...).
// ---------------------------------------------------------------------------
static uint32_t f2, f2_stub, f2_int, f2_value, f2_pointer, f2_all, f2_cont, f2_saved_esp;
static void f2_fn(X86 *c) {
    CXX_PROLOGUE(f2_stub, 0x30, true);
    f2_saved_esp = c->r[R_ESP];
    state(c, 0);
    call(c, thrower_fn);
    state(c, -1);
    recomp_call(c, f2_cont);
}
static void f2_int_fn(X86 *c) {
    ev("int:%x", rd32(ebp(c, -0x18)));
    c->r[R_EAX] = f2_cont;
    ret(c);
}
static void f2_value_fn(X86 *c) {
    ev("value:%x", rd32(ebp(c, -0x20) + 4));
    c->r[R_ECX] = ebp(c, -0x20); // the catch block destroys its own copy
    call(c, derived_dtor);
    c->r[R_EAX] = f2_cont;
    ret(c);
}
static void f2_pointer_fn(X86 *c) {
    ev("pointer:%x", rd32(rd32(ebp(c, -0x1c)))); // Base * points at the tag
    c->r[R_EAX] = f2_cont;
    ret(c);
}
static void f2_all_fn(X86 *c) {
    ev("all");
    c->r[R_EAX] = f2_cont;
    ret(c);
}
static void f2_cont_fn(X86 *c) {
    ev("cont2");
    CHECK(c->r[R_ESP] == f2_saved_esp);
    epilogue(c, f2_saved_esp + 12, 0, true);
}

static void test_handlers(X86 *c) {
    struct {
        uint32_t kind, tag;
        std::vector<std::string> want;
    } cases[] = {
        {1, 5, {"int:5", "cont2"}},
        {0, 0x22, {"copy:22", "value:122", "xdtor:122", "xdtor:22", "cont2"}},
        {2, 0x33, {"pointer:33", "cont2"}},
        {3, 0x44, {"all", "cont2"}},
    };
    for (auto &k : cases) {
        events.clear();
        pending_kind = k.kind;
        pending_tag = k.tag;
        uint32_t esp = c->r[R_ESP];
        guest_call(c, f2);
        CHECK(events_are(k.want));
        CHECK(c->r[R_ESP] == esp && rd32(c->fs_base) == 0xffffffffu);
        CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
    }
}

// ---------------------------------------------------------------------------
// 3. Nested try blocks in one function: the inner one does not match; the
// outer one does; the object inside the inner try is destroyed first.
// try { try { Obj o; throw Derived; } catch (Other &) {} } catch (Base &) {}
// ---------------------------------------------------------------------------
static uint32_t f3, f3_stub, f3_unwind, f3_inner, f3_outer, f3_cont, f3_saved_esp;
static void f3_fn(X86 *c) {
    CXX_PROLOGUE(f3_stub, 0x20, true);
    f3_saved_esp = c->r[R_ESP];
    state(c, 0);
    state(c, 1);
    wr32(ebp(c, -0x14), 0x63);
    state(c, 2);
    call(c, thrower_fn);
    CHECK(false);
}
static void f3_unwind_fn(X86 *c) {
    c->r[R_ECX] = ebp(c, -0x14);
    recomp_call(c, obj_dtor);
}
static void f3_inner_fn(X86 *c) {
    ev("inner");
    c->r[R_EAX] = f3_cont;
    ret(c);
}
static void f3_outer_fn(X86 *c) {
    ev("outer:%x", rd32(ebp(c, -4)));
    c->r[R_EAX] = f3_cont;
    ret(c);
}
static void f3_cont_fn(X86 *c) {
    ev("cont3");
    CHECK(c->r[R_ESP] == f3_saved_esp);
    epilogue(c, f3_saved_esp + 12, 0, true);
}
static void test_nested_try(X86 *c) {
    events.clear();
    pending_kind = 0;
    pending_tag = 0x99;
    guest_call(c, f3);
    CHECK(events_are({"dtor:63", "outer:4", "xdtor:99", "cont3"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}

// ---------------------------------------------------------------------------
// 4. Rethrow: the inner catch block `throw;`s, the outer catches the same
// object by its most derived type, and it is destroyed once, after that.
// 5. A new exception from a catch block: the one it was handling is
// destroyed when the outer catch unwinds past it.
// ---------------------------------------------------------------------------
static uint32_t f4, f4_stub, f4_catch, f4_cont, f4i, f4i_stub, f4i_catch, f4_saved_esp;
static uint32_t f4i_mode; // 0: rethrow, 1: throw int 7
static void f4i_fn(X86 *c) {
    CXX_PROLOGUE(f4i_stub, 0x18, true);
    state(c, 0);
    call(c, thrower_fn);
    CHECK(false);
}
static void f4i_catch_fn(X86 *c) {
    ev("inner:%x", rd32(rd32(ebp(c, -0x14))));
    if (f4i_mode == 0) {
        push(c, 0);
        push(c, 0);
        call(c, throw_import); // throw;
    } else {
        pending_kind = 1;
        pending_tag = 7;
        call(c, thrower_fn);
    }
    CHECK(false);
}
static void f4_fn(X86 *c) {
    CXX_PROLOGUE(f4_stub, 0x18, true);
    f4_saved_esp = c->r[R_ESP];
    state(c, 0);
    call(c, f4i);
    CHECK(false);
}
static void f4_catch_fn(X86 *c) {
    if (f4i_mode == 0)
        ev("outer:%x", rd32(rd32(ebp(c, -0x14)) + 4)); // Derived &
    else
        ev("outer-int:%x", rd32(ebp(c, -0x14)));
    c->r[R_EAX] = f4_cont;
    ret(c);
}
static void f4_cont_fn(X86 *c) {
    ev("cont4");
    CHECK(c->r[R_ESP] == f4_saved_esp);
    epilogue(c, f4_saved_esp + 12, 0, true);
}
static void test_rethrow(X86 *c) {
    events.clear();
    f4i_mode = 0;
    pending_kind = 0;
    pending_tag = 0x44;
    guest_call(c, f4);
    CHECK(events_are({"inner:44", "outer:44", "xdtor:44", "cont4"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}
static void test_throw_from_catch(X86 *c) {
    events.clear();
    f4i_mode = 1;
    pending_kind = 0;
    pending_tag = 0x55;
    guest_call(c, f4);
    CHECK(events_are({"inner:55", "xdtor:55", "outer-int:7", "cont4"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}

// ---------------------------------------------------------------------------
// 6. A try block inside a catch block: its continuation is inside the catch
// block, which then finishes and returns its own continuation.
// try { throw Derived; } catch (Base &) { try { throw 9; } catch (int) {} ... }
// ---------------------------------------------------------------------------
static uint32_t f6, f6_stub, f6_outer, f6_mid, f6_inner, f6_cont, f6_saved_esp, f6_catch_esp;
static void f6_fn(X86 *c) {
    CXX_PROLOGUE(f6_stub, 0x18, true);
    f6_saved_esp = c->r[R_ESP];
    state(c, 0);
    pending_kind = 0;
    pending_tag = 0x66;
    call(c, thrower_fn);
    CHECK(false);
}
static void f6_outer_fn(X86 *c) {
    wr32(ebp(c, -0x10), c->r[R_ESP]); // the catch block's own stack, for its try block
    f6_catch_esp = c->r[R_ESP];
    ev("outer:%x", rd32(rd32(ebp(c, -0x14))));
    state(c, 2);
    pending_kind = 1;
    pending_tag = 9;
    call(c, thrower_fn);
    CHECK(false);
}
static void f6_inner_fn(X86 *c) {
    ev("inner:%x", rd32(ebp(c, -0x18)));
    c->r[R_EAX] = f6_mid;
    ret(c);
}
static void f6_mid_fn(X86 *c) { // the rest of the outer catch block
    ev("mid");
    CHECK(c->r[R_ESP] == f6_catch_esp);
    CHECK(msvc_eh_test_catch_count(c) == 1);
    state(c, 1);
    c->r[R_EAX] = f6_cont;
    ret(c);
}
static void f6_cont_fn(X86 *c) {
    ev("cont6");
    CHECK(c->r[R_ESP] == f6_saved_esp);
    epilogue(c, f6_saved_esp + 12, 0, true);
}
static void test_try_in_catch(X86 *c) {
    events.clear();
    guest_call(c, f6);
    CHECK(events_are({"outer:66", "inner:9", "mid", "xdtor:66", "cont6"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}

// ---------------------------------------------------------------------------
// 7. A loop that throws and catches: the continuation jumps back into the
// body. The host stack must not grow with the iterations.
// ---------------------------------------------------------------------------
static uint32_t f7, f7_stub, f7_loop, f7_catch, f7_cont, f7_saved_esp, f7_count;
static uintptr_t f7_low, f7_high;
static void f7_loop_fn(X86 *c) {
    state(c, 0);
    pending_kind = 1;
    pending_tag = f7_count;
    call(c, thrower_fn);
    CHECK(false);
}
static void f7_fn(X86 *c) {
    CXX_PROLOGUE(f7_stub, 0x18, true);
    f7_saved_esp = c->r[R_ESP];
    recomp_call(c, f7_loop); // jmp loop
}
static void f7_catch_fn(X86 *c) {
    char here;
    uintptr_t at = (uintptr_t)&here;
    if (!f7_low || at < f7_low)
        f7_low = at;
    if (at > f7_high)
        f7_high = at;
    CHECK(rd32(ebp(c, -0x14)) == f7_count);
    c->r[R_EAX] = f7_cont;
    ret(c);
}
static void f7_cont_fn(X86 *c) {
    CHECK(c->r[R_ESP] == f7_saved_esp);
    if (++f7_count < 200) {
        recomp_call(c, f7_loop); // jmp loop
        return;
    }
    epilogue(c, f7_saved_esp + 12, 0, true);
}
static void test_loop(X86 *c) {
    f7_count = 0;
    f7_low = f7_high = 0;
    guest_call(c, f7);
    CHECK(f7_count == 200);
    // One dispatch deep, every time: no host frames pile up.
    CHECK(f7_high - f7_low < 4096);
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}

// ---------------------------------------------------------------------------
// 8. _except_handler3: __try { __try { raise } __finally {} } __except (filter)
// - a software raise, a divide error, and a filter that declines in favour of
// an outer C++ catch(...).
// ---------------------------------------------------------------------------
static uint32_t f8, f8_scope, f8_filter, f8_finally, f8_except, f8_saved_esp, f8_mode;
static uint32_t f8o, f8o_stub, f8o_all, f8o_cont, f8o_saved_esp;
static void f8_fn(X86 *c) {
    SEH_PROLOGUE(f8_scope, 0x10);
    f8_saved_esp = c->r[R_ESP];
    wr32(ebp(c, -4), 0); // __try (level 0)
    wr32(ebp(c, -4), 1); // __try (level 1)
    if (f8_mode == 1) {
        recomp_div_error(c, 0x0d0400ff); // a #DE, as x86.h reports one
    } else {
        uint32_t info = galloc(4);
        wr32(info, 0x5150);
        push(c, info);
        push(c, 1);
        push(c, 0);
        push(c, 0xe0001234);
        call(c, raise_import);
    }
    CHECK(false);
}
static void f8_filter_fn(X86 *c) {
    uint32_t pointers = rd32(ebp(c, -0x14)), record = rd32(pointers);
    ev("filter:%x", rd32(record));
    wr32(ebp(c, -0x1c), rd32(record)); // GetExceptionCode() for the __except body
    c->r[R_EAX] = f8_mode == 2 ? 0 : 1;
    ret(c);
}
static void f8_finally_fn(X86 *c) {
    ev("finally");
    ret(c);
}
static void f8_except_fn(X86 *c) {
    c->r[R_ESP] = rd32(ebp(c, -0x18)); // mov esp,[ebp-18h]
    ev("except:%x", rd32(ebp(c, -0x1c)));
    CHECK(c->r[R_ESP] == f8_saved_esp);
    CHECK(rd32(ebp(c, -4)) == 0xffffffffu); // the enclosing level
    wr32(ebp(c, -4), 0xffffffffu);
    epilogue(c, f8_saved_esp + 12, 0, true, -0x10);
}
static void f8o_fn(X86 *c) {
    CXX_PROLOGUE(f8o_stub, 0x14, true);
    f8o_saved_esp = c->r[R_ESP];
    state(c, 0);
    call(c, f8);
    CHECK(false);
}
static void f8o_all_fn(X86 *c) {
    ev("catch-all");
    c->r[R_EAX] = f8o_cont;
    ret(c);
}
static void f8o_cont_fn(X86 *c) {
    ev("cont8");
    CHECK(c->r[R_ESP] == f8o_saved_esp);
    epilogue(c, f8o_saved_esp + 12, 0, true);
}
static void test_seh(X86 *c) {
    events.clear();
    f8_mode = 0;
    guest_call(c, f8);
    CHECK(events_are({"filter:e0001234", "finally", "except:e0001234"}));
    events.clear();
    f8_mode = 1;
    guest_call(c, f8);
    CHECK(events_are({"filter:c0000094", "finally", "except:c0000094"}));
    events.clear();
    f8_mode = 2;
    guest_call(c, f8o);
    CHECK(events_are({"filter:e0001234", "finally", "catch-all", "cont8"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
    CHECK(rd32(c->fs_base) == 0xffffffffu);
}

// ---------------------------------------------------------------------------
// 9. _setjmp3/longjmp across a C++ frame: the frames between unwind, the
// setjmp frame unwinds to its state at the setjmp (__CxxLongjmpUnwind), and
// the callee-saved registers come back.
// ---------------------------------------------------------------------------
static uint32_t f9, f9_stub, f9_fi, f9_unwind_a, f9_unwind_b, g9, g9_stub, g9_unwind, f9_buf;
static uint32_t f9_esp, f9_returned;
static void f9_fn(X86 *c) {
    CXX_PROLOGUE(f9_stub, 0x18, false);
    uint32_t top = c->r[R_ESP] + 12;
    wr32(ebp(c, -0x14), 0xa);
    state(c, 0);
    c->r[R_EBX] = 0x1111;
    c->r[R_ESI] = 0x2222;
    c->r[R_EDI] = 0x3333;
    push(c, f9_fi);
    push(c, 0); // the state at the setjmp
    push(c, cxx_longjmp_unwind_import);
    push(c, 3);
    push(c, f9_buf);
    f9_esp = c->r[R_ESP];
    push(c, RET_ADDR);
    {
        jmp_buf *b_ = recomp_setjmp3_prepare(c);
        if (setjmp(*b_) == 0)
            recomp_setjmp_return(c, 0);
        else
            recomp_setjmp3_resume(c);
    }
    CHECK(c->r[R_ESP] == f9_esp && c->eip == RET_ADDR);
    c->r[R_ESP] += 20;
    if (c->r[R_EAX] == 0) {
        c->r[R_EBX] = c->r[R_ESI] = c->r[R_EDI] = 0;
        wr32(ebp(c, -0x18), 0xb);
        state(c, 1);
        call(c, g9);
        CHECK(false);
    }
    f9_returned = c->r[R_EAX];
    ev("returned:%x", c->r[R_EAX]);
    CHECK(c->r[R_EBX] == 0x1111 && c->r[R_ESI] == 0x2222 && c->r[R_EDI] == 0x3333);
    CHECK(rd32(ebp(c, -4)) == 0);
    state(c, -1);
    c->r[R_ECX] = ebp(c, -0x14);
    call(c, obj_dtor);
    epilogue(c, top, 0, false);
}
static void f9_unwind_a_fn(X86 *c) {
    c->r[R_ECX] = ebp(c, -0x14);
    recomp_call(c, obj_dtor);
}
static void f9_unwind_b_fn(X86 *c) {
    c->r[R_ECX] = ebp(c, -0x18);
    recomp_call(c, obj_dtor);
}
static void g9_fn(X86 *c) {
    CXX_PROLOGUE(g9_stub, 0x10, false);
    wr32(ebp(c, -0x14), 0xc);
    state(c, 0);
    push(c, 0x42);
    push(c, f9_buf);
    call(c, longjmp_import);
    CHECK(false);
}
static void g9_unwind_fn(X86 *c) {
    c->r[R_ECX] = ebp(c, -0x14);
    recomp_call(c, obj_dtor);
}
static void test_longjmp(X86 *c) {
    events.clear();
    uint32_t esp = c->r[R_ESP];
    guest_call(c, f9);
    CHECK(events_are({"dtor:c", "dtor:b", "returned:42", "dtor:a"}));
    CHECK(c->r[R_ESP] == esp && rd32(c->fs_base) == 0xffffffffu);
    CHECK(recomp_seh_test_frame_count(c) == 0 && recomp_callback_depth() == 0);
}

// ---------------------------------------------------------------------------
// 10. A failed dynamic_cast to a reference throws std::bad_cast, which a
// catch (exception &) takes.
// ---------------------------------------------------------------------------
static uint32_t f10, f10_stub, f10_catch, f10_cont, f10_saved_esp, f10_object, rtdynamic_import;
static void f10_fn(X86 *c) {
    CXX_PROLOGUE(f10_stub, 0x14, true);
    f10_saved_esp = c->r[R_ESP];
    state(c, 0);
    push(c, 1);          // isReference
    push(c, td_derived); // TargetType: not in the object's hierarchy
    push(c, td_other);   // SrcType
    push(c, 0);          // VfDelta
    push(c, f10_object);
    call(c, rtdynamic_import);
    CHECK(false);
}
static void f10_catch_fn(X86 *c) {
    uint32_t e = rd32(ebp(c, -0x14));
    ev("caught:");
    events.back() += gm_str(rd32(e + 4));
    c->r[R_EAX] = f10_cont;
    ret(c);
}
static void f10_cont_fn(X86 *c) {
    ev("cont10");
    epilogue(c, f10_saved_esp + 12, 0, true);
}
static void test_bad_cast(X86 *c) {
    // An object of class Other: vfptr -> [COL] with a one-class hierarchy.
    uint32_t vtable = galloc(16), col = galloc(20), chd = galloc(16), bca = galloc(4),
             bcd = galloc(28);
    f10_object = galloc(8);
    wr32(f10_object, vtable + 4);
    wr32(vtable, col);
    wr32(col + 12, td_other);
    wr32(col + 16, chd);
    wr32(chd + 8, 1);
    wr32(chd + 12, bca);
    wr32(bca, bcd);
    wr32(bcd, td_other);
    wr32(bcd + 12, 0xffffffffu);
    events.clear();
    guest_call(c, f10);
    CHECK(events_are({"caught:Bad dynamic_cast!", "cont10"}));
    CHECK(recomp_seh_test_frame_count(c) == 0 && msvc_eh_test_catch_count(c) == 0);
}

// ---------------------------------------------------------------------------
// Fatal paths, each in a child process.
// ---------------------------------------------------------------------------
static uint32_t throwing_dtor, terminate_handler;
static void throwing_dtor_fn(X86 *c) {
    pending_kind = 1;
    pending_tag = 1;
    call(c, thrower_fn);
}
static void terminate_handler_fn(X86 *c) {
    push(c, 7);
    call(c, exit_import);
}

static int fatal_case(X86 *c, const char *which) {
    if (!strcmp(which, "unhandled")) {
        pending_kind = 0;
        pending_tag = 1;
        guest_call(c, thrower_fn); // nothing catches it: terminate(), exit code 3
    } else if (!strcmp(which, "terminate-handler")) {
        push(c, terminate_handler);
        call(c, set_terminate_import);
        c->r[R_ESP] += 4;
        pending_kind = 1;
        guest_call(c, thrower_fn); // the handler exits with 7
    } else if (!strcmp(which, "dtor-throws")) {
        // G1's local destructor throws while the exception from below unwinds it.
        obj_dtor = throwing_dtor;
        pending_kind = 0;
        guest_call(c, f1);
    } else if (!strcmp(which, "no-checkpoint")) {
        // A catching frame whose establishment was not translated as one.
        functions()[f1] = [](X86 *c) {
            CXX_PROLOGUE(f1_stub, 0x20, false);
            wr32(ebp(c, -0x24), 0x1234);
            state(c, 0);
            call(c, g1);
        };
        pending_kind = 0;
        guest_call(c, f1);
    } else if (!strcmp(which, "longjmp-unset")) {
        uint32_t buf = galloc(64);
        push(c, 1);
        push(c, buf);
        call(c, longjmp_import);
    } else if (!strcmp(which, "setjmp-import")) {
        push(c, 0);
        push(c, galloc(64));
        call(c, setjmp3_import);
    }
    return 99;
}

// ---------------------------------------------------------------------------
static void setup() {
    frame_handler_import = imports_resolve("msvcrt.dll", "__CxxFrameHandler");
    throw_import = imports_resolve("msvcrt.dll", "_CxxThrowException");
    eh3_import = imports_resolve("msvcrt.dll", "_except_handler3");
    setjmp3_import = imports_resolve("msvcrt.dll", "_setjmp3");
    longjmp_import = imports_resolve("msvcrt.dll", "longjmp");
    cxx_longjmp_unwind_import = imports_resolve("msvcrt.dll", "__CxxLongjmpUnwind");
    raise_import = imports_resolve("KERNEL32.dll", "RaiseException");
    set_terminate_import = imports_resolve("msvcrt.dll", "?set_terminate@@YAP6AXXZP6AXXZ@Z");
    exit_import = imports_resolve("KERNEL32.dll", "ExitProcess");
    rtdynamic_import = imports_resolve("msvcrt.dll", "__RTDynamicCast");
    CHECK(frame_handler_import && throw_import && eh3_import && setjmp3_import && longjmp_import &&
          cxx_longjmp_unwind_import && raise_import && set_terminate_import && rtdynamic_import);

    td_base = type_descriptor(".?AVBase@@");
    td_derived = type_descriptor(".?AVDerived@@");
    td_other = type_descriptor(".?AVOther@@");
    td_int = type_descriptor(".H");
    td_pderived = type_descriptor(".PAVDerived@@");
    td_pbase = type_descriptor(".PAVBase@@");
    td_exception = type_descriptor(".?AVexception@@");
    td_bad_cast = type_descriptor(".?AVbad_cast@@");
    obj_dtor = def(obj_dtor_fn);
    derived_dtor = def(derived_dtor_fn);
    derived_copy = def(derived_copy_fn);
    thrower_fn = def(thrower);
    // A handler names its type with a descriptor of its own module: matching
    // is by name, so give the catch side separate copies.
    uint32_t h_base = type_descriptor(".?AVBase@@"), h_derived = type_descriptor(".?AVDerived@@");
    ti_derived = throw_info(derived_dtor, {catchable(0, td_derived, 0, 8, derived_copy),
                                           catchable(0, td_base, 4, 4, 0)});
    ti_int = throw_info(0, {catchable(1, td_int, 0, 4, 0)});
    ti_pderived =
        throw_info(0, {catchable(1, td_pderived, 0, 4, 0), catchable(1, td_pbase, 4, 4, 0)});
    ti_other = throw_info(0, {catchable(0, td_other, 0, 4, 0)});

    f1 = def(f1_fn);
    f1_catch = def(f1_catch_fn);
    f1_cont = def(f1_cont_fn);
    g1 = def(g1_fn);
    g1_unwind = def(g1_unwind_fn);
    f1_stub = stub_for(func_info({{-1, 0}, {-1, 0}}, {{0, 0, 1, {{8, h_base, -0x18, f1_catch}}}}));
    g1_stub = stub_for(func_info({{-1, g1_unwind}}, {}));

    f2 = def(f2_fn);
    f2_int = def(f2_int_fn);
    f2_value = def(f2_value_fn);
    f2_pointer = def(f2_pointer_fn);
    f2_all = def(f2_all_fn);
    f2_cont = def(f2_cont_fn);
    f2_stub = stub_for(
        func_info({{-1, 0}, {-1, 0}}, {{0,
                                        0,
                                        1,
                                        {{0, type_descriptor(".H"), -0x18, f2_int},
                                         {0, h_derived, -0x20, f2_value},
                                         {0, type_descriptor(".PAVBase@@"), -0x1c, f2_pointer},
                                         {0, 0, 0, f2_all}}}}));

    f3 = def(f3_fn);
    f3_unwind = def(f3_unwind_fn);
    f3_inner = def(f3_inner_fn);
    f3_outer = def(f3_outer_fn);
    f3_cont = def(f3_cont_fn);
    f3_stub = stub_for(func_info(
        {{-1, 0}, {0, 0}, {1, f3_unwind}, {0, 0}, {-1, 0}},
        {{1, 2, 3, {{8, td_other, 0, f3_inner}}}, {0, 3, 4, {{8, h_base, 0, f3_outer}}}}));

    f4 = def(f4_fn);
    f4_catch = def(f4_catch_fn);
    f4_cont = def(f4_cont_fn);
    f4i = def(f4i_fn);
    f4i_catch = def(f4i_catch_fn);
    f4_stub = stub_for(func_info(
        {{-1, 0}, {-1, 0}},
        {{0,
          0,
          1,
          {{8, h_derived, -0x14, f4_catch}, {0, type_descriptor(".H"), -0x14, f4_catch}}}}));
    f4i_stub =
        stub_for(func_info({{-1, 0}, {-1, 0}}, {{0, 0, 1, {{8, h_base, -0x14, f4i_catch}}}}));

    f6 = def(f6_fn);
    f6_outer = def(f6_outer_fn);
    f6_mid = def(f6_mid_fn);
    f6_inner = def(f6_inner_fn);
    f6_cont = def(f6_cont_fn);
    f6_stub = stub_for(func_info({{-1, 0}, {-1, 0}, {1, 0}, {1, 0}},
                                 {{2, 2, 3, {{0, type_descriptor(".H"), -0x18, f6_inner}}},
                                  {0, 0, 3, {{8, h_base, -0x14, f6_outer}}}}));

    f7 = def(f7_fn);
    f7_loop = def(f7_loop_fn);
    f7_catch = def(f7_catch_fn);
    f7_cont = def(f7_cont_fn);
    f7_stub = stub_for(func_info({{-1, 0}, {-1, 0}}, {{0, 0, 1, {{0, td_int, -0x14, f7_catch}}}}));

    f8 = def(f8_fn);
    f8_filter = def(f8_filter_fn);
    f8_finally = def(f8_finally_fn);
    f8_except = def(f8_except_fn);
    f8_scope = scope_table({{0xffffffffu, f8_filter, f8_except}, {0, 0, f8_finally}});
    f8o = def(f8o_fn);
    f8o_all = def(f8o_all_fn);
    f8o_cont = def(f8o_cont_fn);
    f8o_stub = stub_for(func_info({{-1, 0}, {-1, 0}}, {{0, 0, 1, {{0, 0, 0, f8o_all}}}}));

    f9 = def(f9_fn);
    f9_unwind_a = def(f9_unwind_a_fn);
    f9_unwind_b = def(f9_unwind_b_fn);
    g9 = def(g9_fn);
    g9_unwind = def(g9_unwind_fn);
    f9_fi = func_info({{-1, f9_unwind_a}, {0, f9_unwind_b}}, {});
    f9_stub = stub_for(f9_fi);
    g9_stub = stub_for(func_info({{-1, g9_unwind}}, {}));
    f9_buf = galloc(64);

    f10 = def(f10_fn);
    f10_catch = def(f10_catch_fn);
    f10_cont = def(f10_cont_fn);
    f10_stub =
        stub_for(func_info({{-1, 0}, {-1, 0}}, {{0, 0, 1, {{8, td_exception, -0x14, f10_catch}}}}));
    (void)td_bad_cast;

    throwing_dtor = def(throwing_dtor_fn);
    terminate_handler = def(terminate_handler_fn);
}

int main(int argc, char **argv) {
    mem_init();
    imports_init();
    setup();
    X86 c;
    loader_init_context(&c);
    if (argc > 1)
        return fatal_case(&c, argv[1]);
    test_basic(&c);
    test_handlers(&c);
    test_nested_try(&c);
    test_rethrow(&c);
    test_throw_from_catch(&c);
    test_try_in_catch(&c);
    test_loop(&c);
    test_seh(&c);
    test_longjmp(&c);
    test_bad_cast(&c);
    test_basic(&c); // the state left behind serves the next exception as well
    char exe[4096];
    CHECK(os_exe_path(exe, sizeof exe) == 0);
    struct {
        const char *which;
        int code;
    } fatal[] = {{"unhandled", 3},       {"terminate-handler", 7}, {"dtor-throws", 3},
                 {"no-checkpoint", 134}, {"longjmp-unset", 134},   {"setjmp-import", 134}};
    for (auto &f : fatal) {
        const char *args[] = {exe, f.which, nullptr};
        int64_t pid = 0;
        int code = -1;
        CHECK(os_spawn(args, &pid) == 0 && os_wait(pid, &code) == 0);
        if (code != f.code)
            fprintf(stderr, "FAIL %s: exit code %d, wanted %d\n", f.which, code, f.code);
        CHECK(code == f.code);
    }
    recomp_seh_reset(&c);
    mem_shutdown();
    printf("eh_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
