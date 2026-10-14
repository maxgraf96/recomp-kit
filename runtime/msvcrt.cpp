// msvcrt.cpp - msvcrt.dll: CRT start-up and exit, threads, errno, rand,
// the environment, and the C++ run-time type information helpers. The other
// areas live in msvcrt_*.cpp; msvcrt_register() installs them all.
//
// Start-up as a module sees it: an EXE's mainCRTStartup calls
// __set_app_type, __p__fmode/__p__commode, __getmainargs and _initterm, and
// reads _acmdln; a DLL's _DllMainCRTStartup allocates its own atexit table,
// runs _initterm over its initialisers and registers destructors with
// __dllonexit, which grows that table and hands it back. The DLL frees the
// table itself at detach, so it must be a block of this heap.
//
// Exception handling (_CxxThrowException, __CxxFrameHandler,
// __CxxLongjmpUnwind, _except_handler3, _setjmp3, longjmp) and the x87
// control word (_controlfp) are implemented elsewhere and not registered
// here.
#include "msvcrt_internal.h"
#include "kernel32_internal.h"
#include "win32.h"
#include <algorithm>
#include <string>
#include <vector>

namespace crt {

// ---------------------------------------------------------------------------
// Per-thread data and errno.
// ---------------------------------------------------------------------------
ThreadData &thread_data() {
    static thread_local ThreadData td;
    if (td.epoch != heap_epoch()) {
        // The heap was reset (a new run in the same process): the guest
        // buffers named here no longer exist. The rand() seed survives,
        // since it belongs to the host thread.
        uint32_t seed = td.holdrand;
        td = ThreadData();
        td.holdrand = seed;
        td.epoch = heap_epoch();
    }
    return td;
}

static uint32_t errno_location() {
    ThreadData &td = thread_data();
    if (!td.errno_addr) {
        td.errno_addr = mem_alloc(8); // errno, then _doserrno
        if (td.errno_addr)
            memset(g_mem + td.errno_addr, 0, 8);
    }
    return td.errno_addr;
}

void set_errno(int e) {
    if (uint32_t a = errno_location())
        wr32(a, (uint32_t)e);
}

namespace {

// ---------------------------------------------------------------------------
// Process state.
// ---------------------------------------------------------------------------
struct Process {
    uint32_t epoch = 0;
    uint32_t fmode = 0, commode = 0; // guest ints behind __p__fmode/__p__commode
    uint32_t app_type = 0;
    uint32_t matherr = 0;
    std::vector<uint32_t> onexit; // the process table, run by exit()
    uint32_t environ_block = 0;
};
Process &process() {
    static auto *p = new Process();
    if (p->epoch != heap_epoch()) {
        *p = Process();
        p->epoch = heap_epoch();
    }
    return *p;
}

uint32_t guest_int(uint32_t &slot) {
    if (!slot) {
        slot = mem_alloc(4);
        if (slot)
            wr32(slot, 0);
    }
    return slot;
}

// void _initterm(PVFV *begin, PVFV *end): calls each non-null entry.
void crt_initterm(X86 *c) {
    uint32_t end = arg(c, 1);
    for (uint32_t p = arg(c, 0); p < end; p += 4) {
        uint32_t fn = rd32(p);
        if (fn)
            guest_call(c, fn);
    }
}

// _onexit(func): the process table. Returns func, or NULL on failure.
void crt_onexit(X86 *c) {
    process().onexit.push_back(arg(c, 0));
    set_eax(c, arg(c, 0));
}

// __dllonexit(func, &begin, &end): appends to a DLL's own table, a heap block
// that grows as needed. The DLL reads *begin..*end at detach and frees it.
void crt_dllonexit(X86 *c) {
    uint32_t fn = arg(c, 0), pbegin = arg(c, 1), pend = arg(c, 2);
    uint32_t begin = rd32(pbegin), end = rd32(pend);
    uint32_t used = (begin && end >= begin) ? end - begin : 0;
    uint32_t cap = begin ? mem_size(begin) : 0;
    if (cap == 0xffffffffu)
        cap = 0;
    if (!begin || used + 4 > cap) {
        uint32_t grow = std::max<uint32_t>(cap, 16);
        uint32_t fresh = mem_realloc(begin, cap + grow);
        if (!fresh) {
            set_eax(c, 0);
            return;
        }
        begin = fresh;
    }
    wr32(begin + used, fn);
    wr32(pbegin, begin);
    wr32(pend, begin + used + 4);
    set_eax(c, fn);
}

void exit_process(X86 *c, uint32_t code) {
    guest_call(c, imports_resolve("KERNEL32.dll", "ExitProcess"), code);
}

// exit(code): the onexit table in reverse, then the process ends.
void crt_exit(X86 *c) {
    run_exit_handlers(c);
    exit_process(c, arg(c, 0));
}
void crt_exit_now(X86 *c) {
    exit_process(c, arg(c, 0));
}
void crt_abort(X86 *c) {
    LOGW("msvcrt: abort() from %08x: abnormal program termination", rd32(c->r[R_ESP]));
    exit_process(c, 3);
}
// A pure virtual call: msvcrt reports run-time error R6025 and exits with 255.
void crt_purecall(X86 *c) {
    LOGW("msvcrt: R6025 - pure virtual function call (return address %08x)", rd32(c->r[R_ESP]));
    exit_process(c, 255);
}
// std::terminate with no handler installed calls abort().
void crt_terminate(X86 *c) {
    LOGW("msvcrt: terminate() called from %08x", rd32(c->r[R_ESP]));
    exit_process(c, 3);
}

void crt_set_app_type(X86 *c) {
    process().app_type = arg(c, 0);
}
void crt_setusermatherr(X86 *c) {
    process().matherr = arg(c, 0);
}
void crt_p_fmode(X86 *c) {
    set_eax(c, guest_int(process().fmode));
}
void crt_p_commode(X86 *c) {
    set_eax(c, guest_int(process().commode));
}
void crt_errno(X86 *c) {
    set_eax(c, errno_location());
}
// The unhandled-exception filter around main: no signal handler is ever
// installed here, so the exception goes on to the runtime's own last-chance
// handling, which reports it.
void crt_XcptFilter(X86 *c) {
    LOGW("msvcrt: _XcptFilter(%08x): no handler, continuing the search", arg(c, 0));
    set_eax(c, 0); // EXCEPTION_CONTINUE_SEARCH
}

// ---------------------------------------------------------------------------
// Command line and environment.
// ---------------------------------------------------------------------------
uint32_t command_line() {
    X86 scratch{};
    get_command_line(&scratch);
    return scratch.r[R_EAX];
}

// Splits a command line by msvcrt's rules: white space separates arguments;
// double quotes group; 2n backslashes before a quote give n backslashes and a
// grouping quote, 2n+1 give n backslashes and a literal quote; other
// backslashes are literal. The program name ends at its closing quote or the
// first white space, with no escapes.
std::vector<std::string> split_command_line(const std::string &line) {
    std::vector<std::string> args;
    size_t i = 0, n = line.size();
    std::string first;
    if (i < n && line[i] == '"') {
        for (++i; i < n && line[i] != '"'; ++i)
            first.push_back(line[i]);
        if (i < n)
            ++i;
    } else {
        for (; i < n && line[i] != ' ' && line[i] != '\t'; ++i)
            first.push_back(line[i]);
    }
    args.push_back(first);
    for (;;) {
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= n)
            break;
        std::string cur;
        bool quoted = false;
        while (i < n && (quoted || (line[i] != ' ' && line[i] != '\t'))) {
            size_t slashes = 0;
            while (i < n && line[i] == '\\') {
                ++slashes;
                ++i;
            }
            if (i < n && line[i] == '"') {
                cur.append(slashes / 2, '\\');
                if (slashes % 2) {
                    cur.push_back('"');
                } else if (quoted && i + 1 < n && line[i + 1] == '"') {
                    cur.push_back('"'); // "" inside quotes: a literal quote
                    ++i;
                } else {
                    quoted = !quoted;
                }
                ++i;
            } else {
                cur.append(slashes, '\\');
                if (i < n && (quoted || (line[i] != ' ' && line[i] != '\t')))
                    cur.push_back(line[i++]);
            }
        }
        args.push_back(cur);
    }
    return args;
}

uint32_t environment_block(X86 *c) {
    Process &p = process();
    if (!p.environ_block)
        p.environ_block = guest_call(c, imports_resolve("KERNEL32.dll", "GetEnvironmentStrings"));
    return p.environ_block;
}

// __getmainargs(&argc, &argv, &envp, dowildcard, &startinfo)
void crt_getmainargs(X86 *c) {
    uint32_t cl = command_line();
    std::vector<std::string> args = split_command_line(std::string(gchars(cl), gstrlen(cl)));
    std::vector<std::string> env;
    if (uint32_t block = environment_block(c)) {
        for (uint32_t at = block; rd8(at);) {
            uint32_t len = gstrlen(at);
            env.emplace_back(gchars(at), len);
            at += len + 1;
        }
    }
    auto build = [](const std::vector<std::string> &v) {
        uint32_t bytes = 4 * ((uint32_t)v.size() + 1);
        for (auto &s : v)
            bytes += (uint32_t)s.size() + 1;
        uint32_t block = mem_alloc(bytes);
        if (!block)
            return 0u;
        uint32_t at = block + 4 * ((uint32_t)v.size() + 1);
        for (size_t i = 0; i < v.size(); ++i) {
            wr32(block + 4 * (uint32_t)i, at);
            at += gm_put_str(at, v[i].c_str(), (uint32_t)v[i].size() + 1) + 1;
        }
        wr32(block + 4 * (uint32_t)v.size(), 0);
        return block;
    };
    uint32_t argv = build(args), envp = build(env);
    if (arg(c, 0))
        wr32(arg(c, 0), (uint32_t)args.size());
    if (arg(c, 1))
        wr32(arg(c, 1), argv);
    if (arg(c, 2))
        wr32(arg(c, 2), envp);
    set_eax(c, argv && envp ? 0 : 0xffffffffu);
}

// getenv(name): a pointer into the environment block, matched without case.
void crt_getenv(X86 *c) {
    uint32_t name = arg(c, 0);
    uint32_t nlen = gstrlen(name);
    uint32_t block = environment_block(c);
    if (!block || !nlen) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t at = block; rd8(at);) {
        uint32_t len = gstrlen(at);
        if (len > nlen && rd8(at + nlen) == '=') {
            bool same = true;
            for (uint32_t i = 0; i < nlen && same; ++i)
                same = ctype_toupper(rd8(at + i)) == ctype_toupper(rd8(name + i));
            if (same) {
                set_eax(c, at + nlen + 1);
                return;
            }
        }
        at += len + 1;
    }
    set_eax(c, 0);
}

void init_acmdln(uint32_t addr) {
    wr32(addr, command_line());
}

// ---------------------------------------------------------------------------
// Threads and rand().
// ---------------------------------------------------------------------------
// _beginthreadex has CreateThread's parameters in the same order.
void crt_beginthreadex(X86 *c) {
    uint32_t args[6];
    for (int i = 0; i < 6; ++i)
        args[i] = arg(c, i);
    uint32_t h = guest_call(c, imports_resolve("KERNEL32.dll", "CreateThread"), args, 6);
    if (!h)
        set_errno(E_INVAL);
    set_eax(c, h);
}
void crt_endthreadex(X86 *c) {
    guest_call(c, imports_resolve("KERNEL32.dll", "ExitThread"), arg(c, 0));
}

// rand(): the per-thread linear congruential generator msvcrt documents by
// behaviour - seed * 214013 + 2531011, bits 16..30 of the new seed.
void crt_rand(X86 *c) {
    ThreadData &td = thread_data();
    td.holdrand = td.holdrand * 214013u + 2531011u;
    set_eax(c, (td.holdrand >> 16) & 0x7fff);
}
void crt_srand(X86 *c) {
    thread_data().holdrand = arg(c, 0);
}

// ---------------------------------------------------------------------------
// type_info and dynamic_cast.
// ---------------------------------------------------------------------------
// type_info: +0 vftable, +4 the undecorated name (allocated on first use),
// +8 the decorated name, ".?AVName@@".
void crt_type_info_dtor(X86 *c) {
    uint32_t self = c->r[R_ECX];
    uint32_t name = rd32(self + 4);
    if (name) {
        mem_free(name, "type_info::~type_info");
        wr32(self + 4, 0);
    }
}

// An undecorator for the type encodings that type_info names carry.
struct Undecorator {
    const char *p;
    std::vector<std::string> names; // back-references 0-9
    bool ok = true;

    std::string fail() {
        ok = false;
        return std::string();
    }
    void remember(const std::string &n) {
        if (names.size() < 10 && std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    }
    // A name fragment: a back-reference digit, a template, or text to '@'.
    std::string fragment() {
        if (*p >= '0' && *p <= '9') {
            size_t i = size_t(*p++ - '0');
            return i < names.size() ? names[i] : fail();
        }
        if (p[0] == '?' && p[1] == '$') {
            // A template's name and arguments share a back-reference table of
            // their own, which starts with the template's name; the whole
            // instance is then one name in the enclosing table.
            Undecorator inner{p + 2};
            std::string base = inner.simple_name();
            inner.remember(base);
            std::string args;
            while (inner.ok && *inner.p && *inner.p != '@') {
                if (!args.empty())
                    args += ',';
                args += inner.template_arg();
            }
            if (!inner.ok || *inner.p != '@')
                return fail();
            p = inner.p + 1;
            std::string full =
                base + "<" + args + (args.empty() || args.back() != '>' ? ">" : " >");
            remember(full);
            return full;
        }
        std::string n = simple_name();
        remember(n);
        return n;
    }
    std::string simple_name() {
        std::string n;
        while (*p && *p != '@')
            n.push_back(*p++);
        if (*p != '@')
            return fail();
        ++p;
        return n;
    }
    // Fragments innermost first, ended by '@'; printed outermost first.
    std::string qualified_name() {
        std::vector<std::string> parts;
        while (ok && *p && *p != '@')
            parts.push_back(fragment());
        if (*p != '@')
            return fail();
        ++p;
        std::string out;
        for (size_t i = parts.size(); i-- > 0;) {
            out += parts[i];
            if (i)
                out += "::";
        }
        return out;
    }
    std::string number() {
        bool neg = false;
        if (*p == '?') {
            neg = true;
            ++p;
        }
        int64_t v = 0;
        if (*p >= '0' && *p <= '9') {
            v = *p++ - '0' + 1;
        } else {
            while (*p >= 'A' && *p <= 'P')
                v = v * 16 + (*p++ - 'A');
            if (*p != '@')
                return fail();
            ++p;
        }
        return std::to_string(neg ? -v : v);
    }
    std::string template_arg() {
        if (p[0] == '$' && p[1] == '0') {
            p += 2;
            return number();
        }
        if (p[0] == '$' && p[1] == '$' && p[2] == 'C') {
            // A cv-qualified type: "int const".
            p += 3;
            std::string q = cv(*p++);
            return type() + q;
        }
        return type();
    }
    std::string cv(char ch) {
        switch (ch) {
        case 'A':
            return "";
        case 'B':
            return " const";
        case 'C':
            return " volatile";
        case 'D':
            return " const volatile";
        default:
            return fail();
        }
    }
    std::string type() {
        char ch = *p++;
        switch (ch) {
        case 'C':
            return "signed char";
        case 'D':
            return "char";
        case 'E':
            return "unsigned char";
        case 'F':
            return "short";
        case 'G':
            return "unsigned short";
        case 'H':
            return "int";
        case 'I':
            return "unsigned int";
        case 'J':
            return "long";
        case 'K':
            return "unsigned long";
        case 'M':
            return "float";
        case 'N':
            return "double";
        case 'O':
            return "long double";
        case 'X':
            return "void";
        case '_': {
            char x = *p++;
            if (x == 'N')
                return "bool";
            if (x == 'J')
                return "__int64";
            if (x == 'K')
                return "unsigned __int64";
            if (x == 'W')
                return "wchar_t";
            return fail();
        }
        case 'T':
            return "union " + qualified_name();
        case 'U':
            return "struct " + qualified_name();
        case 'V':
            return "class " + qualified_name();
        case 'W':
            if (*p++ != '4')
                return fail();
            return "enum " + qualified_name();
        case 'P':
        case 'Q':
        case 'A': {
            std::string q = cv(*p++);
            std::string pointee = type();
            return pointee + q + (ch == 'A' ? " &" : " *");
        }
        default:
            return fail();
        }
    }
};

std::string undecorate_type(const char *decorated) {
    const char *p = decorated;
    if (*p == '.')
        ++p;
    Undecorator u{p};
    std::string out;
    if (u.p[0] == '?' && u.p[1] == 'A') {
        u.p += 2;
        out = u.type();
    } else {
        out = u.type();
    }
    return u.ok && !*u.p ? out : std::string(p);
}

void crt_type_info_name(X86 *c) {
    uint32_t self = c->r[R_ECX];
    uint32_t name = rd32(self + 4);
    if (!name) {
        std::string raw(gchars(self + 8), gstrlen(self + 8));
        name = mem_strdup(undecorate_type(raw.c_str()));
        wr32(self + 4, name);
    }
    set_eax(c, name);
}

// RTTI structures (MSVC x86):
//   complete object locator: signature, offset, cdOffset, TypeDescriptor*,
//     ClassHierarchyDescriptor*; vftable[-1] points at it.
//   class hierarchy descriptor: signature, attributes (1 multiple, 2 virtual
//     inheritance), numBaseClasses, BaseClassDescriptor**.
//   base class descriptor: TypeDescriptor*, numContainedBases, PMD {mdisp,
//     pdisp, vdisp}, attributes (1 not visible, 2 ambiguous).
//   type descriptor: vftable, spare, decorated name.
bool same_type(uint32_t a, uint32_t b) {
    return a == b || (a && b && strcmp(gchars(a + 8), gchars(b + 8)) == 0);
}

int32_t pmd_offset(uint32_t complete, uint32_t bcd) {
    int32_t mdisp = (int32_t)rd32(bcd + 8), pdisp = (int32_t)rd32(bcd + 12),
            vdisp = (int32_t)rd32(bcd + 16);
    int32_t off = mdisp;
    if (pdisp >= 0) {
        uint32_t vbtable = rd32(complete + (uint32_t)pdisp);
        off += pdisp + (int32_t)rd32(vbtable + (uint32_t)vdisp);
    }
    return off;
}

// PVOID __RTDynamicCast(PVOID inptr, LONG VfDelta, PVOID SrcType,
//                       PVOID TargetType, BOOL isReference)
void crt_RTDynamicCast(X86 *c) {
    uint32_t inptr = arg(c, 0);
    int32_t vfdelta = (int32_t)arg(c, 1);
    uint32_t src_type = arg(c, 2), target_type = arg(c, 3), is_ref = arg(c, 4);
    if (!inptr) {
        set_eax(c, 0);
        return;
    }
    uint32_t vfptr_at = inptr + (uint32_t)vfdelta;
    uint32_t col = rd32(rd32(vfptr_at) - 4);
    uint32_t complete = vfptr_at - rd32(col + 4);
    if (uint32_t cd = rd32(col + 8))
        complete -= rd32(vfptr_at - cd);
    uint32_t chd = rd32(col + 16);
    uint32_t nbases = rd32(chd + 8), array = rd32(chd + 12);
    int32_t src_off = (int32_t)(inptr - complete);
    uint32_t result = 0;
    int found = 0;
    if (!(rd32(chd + 4) & 1)) {
        // Single inheritance: every base lies at the start of the object.
        for (uint32_t i = 0; i < nbases; ++i) {
            uint32_t bcd = rd32(array + 4 * i);
            if (same_type(rd32(bcd), target_type)) {
                result = complete + (uint32_t)pmd_offset(complete, bcd);
                found = 1;
                break;
            }
        }
    } else {
        // Find the source subobject, then prefer a target that contains it
        // (a downcast); otherwise the target must be a unique, public base
        // of the complete object (a cross cast).
        int src_index = -1;
        for (uint32_t i = 0; i < nbases && src_index < 0; ++i) {
            uint32_t bcd = rd32(array + 4 * i);
            if (same_type(rd32(bcd), src_type) && pmd_offset(complete, bcd) == src_off)
                src_index = (int)i;
        }
        uint32_t down = 0, cross = 0;
        int ndown = 0, ncross = 0;
        for (uint32_t i = 0; i < nbases; ++i) {
            uint32_t bcd = rd32(array + 4 * i);
            if (!same_type(rd32(bcd), target_type))
                continue;
            uint32_t contained = rd32(bcd + 4);
            uint32_t attrs = rd32(bcd + 20);
            uint32_t at = complete + (uint32_t)pmd_offset(complete, bcd);
            if (src_index >= 0 && (int)i <= src_index && (uint32_t)src_index <= i + contained) {
                if (!ndown || at != down)
                    ++ndown;
                down = at;
            }
            if (!(attrs & 3)) {
                if (!ncross || at != cross)
                    ++ncross;
                cross = at;
            }
        }
        if (ndown == 1) {
            result = down;
            found = 1;
        } else if (ncross == 1) {
            result = cross;
            found = 1;
        }
    }
    if (!found && is_ref)
        LOGW("msvcrt: dynamic_cast to a reference failed at %08x (std::bad_cast is not thrown)",
             rd32(c->r[R_ESP]));
    set_eax(c, found ? result : 0);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "_initterm", ARGC_CDECL, crt_initterm},
    {"msvcrt.dll", "_onexit", ARGC_CDECL, crt_onexit},
    {"msvcrt.dll", "__dllonexit", ARGC_CDECL, crt_dllonexit},
    {"msvcrt.dll", "exit", ARGC_CDECL, crt_exit},
    {"msvcrt.dll", "_exit", ARGC_CDECL, crt_exit_now},
    {"msvcrt.dll", "abort", ARGC_CDECL, crt_abort},
    {"msvcrt.dll", "_purecall", ARGC_CDECL, crt_purecall},
    {"msvcrt.dll", "?terminate@@YAXXZ", ARGC_CDECL, crt_terminate},
    {"msvcrt.dll", "__set_app_type", ARGC_CDECL, crt_set_app_type},
    {"msvcrt.dll", "__setusermatherr", ARGC_CDECL, crt_setusermatherr},
    {"msvcrt.dll", "__p__fmode", ARGC_CDECL, crt_p_fmode},
    {"msvcrt.dll", "__p__commode", ARGC_CDECL, crt_p_commode},
    {"msvcrt.dll", "__getmainargs", ARGC_CDECL, crt_getmainargs},
    {"msvcrt.dll", "_errno", ARGC_CDECL, crt_errno},
    {"msvcrt.dll", "_XcptFilter", ARGC_CDECL, crt_XcptFilter},
    {"msvcrt.dll", "getenv", ARGC_CDECL, crt_getenv},
    {"msvcrt.dll", "_beginthreadex", ARGC_CDECL, crt_beginthreadex},
    {"msvcrt.dll", "_endthreadex", ARGC_CDECL, crt_endthreadex},
    {"msvcrt.dll", "rand", ARGC_CDECL, crt_rand},
    {"msvcrt.dll", "srand", ARGC_CDECL, crt_srand},
    // __thiscall: `this` in ECX, no stack arguments.
    {"msvcrt.dll", "??1type_info@@UAE@XZ", 0, crt_type_info_dtor},
    {"msvcrt.dll", "?name@type_info@@QBEPBDXZ", 0, crt_type_info_name},
    {"msvcrt.dll", "__RTDynamicCast", ARGC_CDECL, crt_RTDynamicCast},
};

} // namespace

uint32_t fmode_value() {
    uint32_t a = process().fmode;
    return a ? rd32(a) : 0;
}

void run_exit_handlers(X86 *c) {
    Process &p = process();
    while (!p.onexit.empty()) {
        uint32_t fn = p.onexit.back();
        p.onexit.pop_back();
        if (fn)
            guest_call(c, fn);
    }
    stdio_flush_all();
}

void register_process() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data_init("msvcrt.dll", "_acmdln", 4, init_acmdln);
    imports_register_data("msvcrt.dll", "_adjust_fdiv", 4); // 0: no Pentium FDIV flaw
}

std::string undecorated_type_name(const char *decorated) {
    return undecorate_type(decorated);
}
std::vector<std::string> command_line_arguments(const std::string &line) {
    return split_command_line(line);
}

} // namespace crt

void msvcrt_register() {
    crt::register_heap();
    crt::register_locale();
    crt::register_string();
    crt::register_format();
    crt::register_stdio();
    crt::register_math();
    crt::register_float();
    crt::register_time();
    crt::register_process();
}
