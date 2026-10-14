// crt_tests.cpp - the msvcrt.dll / msvcp60.dll HLE against synthetic guest
// memory: every shim is called through its trampoline with cdecl or thiscall
// arguments, as translated code would call it. No game files are needed.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../msvcrt_internal.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[768];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    if (!ok || getenv("CRT_TESTS_VERBOSE"))
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// ---------------------------------------------------------------------------
// Calling shims.
// ---------------------------------------------------------------------------
static X86 g_cpu;
static const uint32_t SCRATCH = 0x00300000; // strings and buffers the tests build
static uint32_t g_scratch_top = SCRATCH;

// A guest copy of `s`, in a bump region that each test resets.
static uint32_t gstr(const std::string &s) {
    uint32_t a = g_scratch_top;
    memcpy(g_mem + a, s.c_str(), s.size() + 1);
    g_scratch_top += ((uint32_t)s.size() + 16) & ~15u;
    return a;
}
static uint32_t gbuf(uint32_t n) {
    uint32_t a = g_scratch_top;
    memset(g_mem + a, 0xcc, n);
    g_scratch_top += (n + 15) & ~15u;
    return a;
}
static std::string str_at(uint32_t a) {
    return a ? std::string((const char *)g_mem + a) : std::string("<null>");
}

// Pushes dword arguments (a double takes two, low first) and dispatches.
// thiscall callers pass `self`, which goes to ECX.
static uint32_t call(const char *dll, const char *name, const std::vector<uint32_t> &args,
                     uint32_t self = 0) {
    X86 *c = &g_cpu;
    uint32_t tramp = imports_resolve(dll, name);
    if (!check(tramp != 0, "%s!%s has a shim", dll, name))
        return 0;
    uint32_t before = c->r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0x00401234); // return address
    c->r[R_ESP] = esp;
    c->r[R_ECX] = self;
    imports_dispatch(c, tramp);
    uint8_t argc = imports_argc(tramp);
    uint32_t expected = argc == ARGC_CDECL ? before - 4u * (uint32_t)args.size() : before;
    check(c->r[R_ESP] == expected, "%s!%s leaves ESP balanced (argc %u)", dll, name, argc);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}
static uint32_t ms(const char *name, const std::vector<uint32_t> &args, uint32_t self = 0) {
    return call("msvcrt.dll", name, args, self);
}
static void push_double(std::vector<uint32_t> &v, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    v.push_back((uint32_t)bits);
    v.push_back((uint32_t)(bits >> 32));
}
static uint64_t bits_of(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}
static double from_bits(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}
// A shim returning in ST(0): the popped value.
static double st0_result() {
    return fpop(&g_cpu);
}
static int guest_errno() {
    uint32_t a = ms("_errno", {});
    return (int)rd32(a);
}
static void clear_errno() {
    wr32(ms("_errno", {}), 0);
}

// sprintf with up to a few dword arguments.
static std::string sprintf_guest(const std::string &fmt, const std::vector<uint32_t> &args) {
    uint32_t buf = gbuf(512);
    std::vector<uint32_t> all = {buf, gstr(fmt)};
    all.insert(all.end(), args.begin(), args.end());
    uint32_t n = ms("sprintf", all);
    std::string out = str_at(buf);
    check(n == out.size(), "sprintf(\"%s\") returns its length (%u)", fmt.c_str(), n);
    return out;
}
static std::string fmt_double(const std::string &fmt, double d) {
    std::vector<uint32_t> a;
    push_double(a, d);
    return sprintf_guest(fmt, a);
}
static void expect_fmt(const std::string &fmt, double d, const std::string &want) {
    std::string got = fmt_double(fmt, d);
    check(got == want, "sprintf(\"%s\", %.17g) = \"%s\" (want \"%s\")", fmt.c_str(), d, got.c_str(),
          want.c_str());
}
static void expect_fmti(const std::string &fmt, const std::vector<uint32_t> &args,
                        const std::string &want) {
    std::string got = sprintf_guest(fmt, args);
    check(got == want, "sprintf(\"%s\") = \"%s\" (want \"%s\")", fmt.c_str(), got.c_str(),
          want.c_str());
}

static void reset_world() {
    imports_init();
    mem_init();
    win32_init(".");
    loader_init_context(&g_cpu);
    g_scratch_top = SCRATCH;
}

// ---------------------------------------------------------------------------
// Heap.
// ---------------------------------------------------------------------------
static void test_heap() {
    printf("heap\n");
    reset_world();
    uint32_t a = ms("malloc", {10});
    uint32_t b = ms("malloc", {10});
    check(a && b && a != b && (a & 15) == 0 && (b & 15) == 0, "malloc: distinct 16-byte aligned");
    check(ms("_msize", {a}) == 10, "_msize reports the requested size");
    check(ms("_msize", {ms("malloc", {0})}) == 1, "malloc(0) is a one-byte block");
    memset(g_mem + a, 0x5a, 10);
    uint32_t a2 = ms("realloc", {a, 12});
    check(a2 == a && rd8(a2 + 9) == 0x5a && ms("_msize", {a2}) == 12, "realloc grows in place");
    uint32_t a3 = ms("realloc", {a2, 100});
    check(a3 != a2 && rd8(a3 + 9) == 0x5a && ms("_msize", {a3}) == 100,
          "realloc moves to a larger class and copies");
    check(ms("realloc", {a3, 0}) == 0, "realloc(p, 0) frees and returns NULL");
    uint32_t big = ms("malloc", {100000});
    check(big && ms("_msize", {big}) == 100000, "a large block");
    memset(g_mem + big, 0x77, 100000);
    uint32_t big2 = ms("realloc", {big, 200000});
    check(big2 && rd8(big2 + 99999) == 0x77, "realloc of a large block keeps its contents");
    uint32_t z = ms("calloc", {7, 9});
    bool zero = true;
    for (uint32_t i = 0; i < 63; ++i)
        zero = zero && rd8(z + i) == 0;
    check(z && zero && ms("_msize", {z}) == 63, "calloc zeroes count*size bytes");
    check(ms("calloc", {0x10000, 0x10000}) == 0, "calloc overflow returns NULL");

    // Interior and double frees are logged and ignored.
    crt::HeapCounters before = crt::heap_counters();
    uint32_t blk = ms("malloc", {64});
    ms("free", {blk + 16});
    ms("free", {big2 + 640});
    ms("free", {0x00300000}); // not a heap address at all
    crt::HeapCounters mid = crt::heap_counters();
    check(mid.bad_frees == before.bad_frees + 3, "interior and foreign frees are counted");
    check(ms("_msize", {blk}) == 64, "the block survives a free of its interior");
    ms("free", {blk});
    ms("free", {blk});
    check(crt::heap_counters().bad_frees == mid.bad_frees + 1, "a double free is caught");
    uint32_t again = ms("malloc", {64});
    check(again == blk, "a freed slot is reused (LIFO)");
    check(heap_check().empty(), "the guest heap is consistent: %s", heap_check().c_str());

    // operator new / delete and the new handler.
    uint32_t n = ms("??2@YAPAXI@Z", {24});
    check(n && ms("_msize", {n}) == 24, "operator new");
    ms("??3@YAXPAX@Z", {n});
    check(ms("_msize", {n}) == 0xffffffffu, "operator delete frees");
    check(ms("_callnewh", {16}) == 0, "_callnewh without a handler fails");
    check(ms("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", {0x1234}) == 0 &&
              ms("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", {0}) == 0x1234,
          "_set_new_handler returns the previous handler");

    // Determinism: the same calls after a reset give the same addresses.
    std::vector<uint32_t> first, second;
    for (int run = 0; run < 2; ++run) {
        reset_world();
        auto &v = run ? second : first;
        for (uint32_t i = 1; i < 200; ++i) {
            uint32_t p = ms("malloc", {i * 13 % 3000});
            v.push_back(p);
            if (i % 3 == 0)
                ms("free", {v[i / 2]});
        }
    }
    check(first == second, "allocation addresses are deterministic");

    // A hot loop stays cheap: the slab path is a vector push and pop.
    reset_world();
    uint64_t t0 = os_monotonic_ns();
    for (int i = 0; i < 200000; ++i) {
        uint32_t p = ms("??2@YAPAXI@Z", {48});
        ms("??3@YAXPAX@Z", {p});
    }
    double ns = double(os_monotonic_ns() - t0) / 400000.0;
    check(ns < 2000, "new/delete through the trampoline: %.0f ns per call", ns);
}

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------
static void test_strings() {
    printf("strings\n");
    reset_world();
    uint32_t s = gstr("Hello, World");
    check(ms("strlen", {s}) == 12, "strlen");
    uint32_t d = gbuf(64);
    check(ms("strcpy", {d, s}) == d && str_at(d) == "Hello, World", "strcpy");
    ms("strcat", {d, gstr("!!")});
    check(str_at(d) == "Hello, World!!", "strcat");
    check((int)ms("strcmp", {gstr("abc"), gstr("abd")}) == -1, "strcmp gives -1");
    check((int)ms("strcmp", {gstr("abd"), gstr("abc")}) == 1, "strcmp gives 1");
    check(ms("strcmp", {gstr("abc"), gstr("abc")}) == 0, "strcmp gives 0");
    check((int)ms("strcmp", {gstr("a\xe9"), gstr("az")}) == 1, "strcmp compares unsigned");
    check((int)ms("strncmp", {gstr("abcx"), gstr("abcz"), 4}) == 'x' - 'z',
          "strncmp gives the byte difference");
    check(ms("strncmp", {gstr("abcx"), gstr("abcz"), 3}) == 0, "strncmp stops at n");
    check((int)ms("_stricmp", {gstr("HELLO"), gstr("help")}) == 'l' - 'p', "_stricmp");
    check(ms("_strnicmp", {gstr("HELLO"), gstr("help"), 3}) == 0, "_strnicmp");
    check(ms("strchr", {s, 'W'}) == s + 7 && ms("strchr", {s, 0}) == s + 12 &&
              ms("strchr", {s, 'z'}) == 0,
          "strchr finds characters and the terminator");
    check(ms("strrchr", {s, 'o'}) == s + 8, "strrchr");
    check(ms("strstr", {s, gstr("World")}) == s + 7 && ms("strstr", {s, gstr("")}) == s &&
              ms("strstr", {s, gstr("xyz")}) == 0,
          "strstr");
    check(ms("strcspn", {s, gstr(" ,")}) == 5, "strcspn");
    uint32_t n = gbuf(8);
    ms("strncpy", {n, gstr("ab"), 6});
    check(memcmp(g_mem + n, "ab\0\0\0\0\xcc", 7) == 0, "strncpy pads with NULs and no further");
    uint32_t t = gstr("a,b,,c");
    uint32_t delim = gstr(",");
    std::string toks;
    for (uint32_t p = ms("strtok", {t, delim}); p; p = ms("strtok", {0, delim}))
        toks += str_at(p) + "|";
    check(toks == "a|b|c|", "strtok skips empty fields: %s", toks.c_str());
    uint32_t dup = ms("_strdup", {s});
    check(dup && str_at(dup) == "Hello, World" && ms("_msize", {dup}) == 13, "_strdup");

    // mem*
    uint32_t m = gstr("0123456789");
    ms("memcpy", {m + 2, m, 5});
    check(str_at(m) == "0101234789", "memcpy tolerates overlap like memmove");
    ms("memset", {m, 'x', 3});
    check(str_at(m) == "xxx1234789", "memset");
    check(ms("memchr", {m, '2', 10}) == m + 4 && ms("memchr", {m, 'q', 10}) == 0, "memchr");
    check((int)ms("memcmp", {gstr("ab"), gstr("ac"), 2}) == -1 &&
              ms("memcmp", {gstr("ab"), gstr("ab"), 2}) == 0,
          "memcmp");

    // wcs*
    uint32_t w = gbuf(32);
    gm_put_wstr(w, "wide", 16);
    check(ms("wcslen", {w}) == 4, "wcslen");
    uint32_t w2 = gbuf(32);
    ms("wcscpy", {w2, w});
    check(ms("wcscmp", {w, w2}) == 0, "wcscpy and wcscmp");
    wr16(w2, 'x');
    check(ms("wcscmp", {w, w2}) == 0xffffffffu, "wcscmp gives -1");

    // _mbs*: code page 1252 has no lead bytes.
    uint32_t mb = gstr("Caf\xe9 au lait");
    check(ms("_mbslen", {mb}) == 12, "_mbslen");
    check(ms("_mbsinc", {mb}) == mb + 1 && ms("_mbsdec", {mb, mb + 3}) == mb + 2 &&
              ms("_mbsdec", {mb, mb}) == 0,
          "_mbsinc and _mbsdec");
    check(ms("_mbsninc", {mb, 100}) == mb + 12, "_mbsninc stops at the terminator");
    check(ms("_mbsnbcnt", {mb, 4}) == 4 && ms("_mbsnccnt", {mb, 50}) == 12, "_mbsnbcnt/_mbsnccnt");
    check(ms("_mbschr", {mb, 0xe9}) == mb + 3, "_mbschr finds a high byte");
    uint32_t cat = gbuf(32);
    gm_put_str(cat, "ab", 32);
    ms("_mbsnbcat", {cat, gstr("cdef"), 2});
    check(str_at(cat) == "abcd", "_mbsnbcat");
    uint32_t up = gstr("mixed Case \xe9");
    ms("_mbsupr", {up});
    check(str_at(up) == "MIXED CASE \xe9", "_mbsupr in the C locale leaves high bytes alone");

    // _splitpath
    uint32_t dr = gbuf(8), di = gbuf(260), fn = gbuf(260), ex = gbuf(260);
    ms("_splitpath", {gstr("C:\\games\\data\\file.name.txt"), dr, di, fn, ex});
    check(str_at(dr) == "C:" && str_at(di) == "\\games\\data\\" && str_at(fn) == "file.name" &&
              str_at(ex) == ".txt",
          "_splitpath: %s|%s|%s|%s", str_at(dr).c_str(), str_at(di).c_str(), str_at(fn).c_str(),
          str_at(ex).c_str());
    ms("_splitpath", {gstr("noext"), dr, di, fn, ex});
    check(str_at(dr) == "" && str_at(di) == "" && str_at(fn) == "noext" && str_at(ex) == "",
          "_splitpath of a bare name");
}

// ---------------------------------------------------------------------------
// Locale and ctype.
// ---------------------------------------------------------------------------
static void test_locale() {
    printf("locale\n");
    reset_world();
    check(ms("isalpha", {'A'}) && !ms("isalpha", {'1'}) && ms("isdigit", {'7'}) &&
              ms("isspace", {'\t'}) && ms("isxdigit", {'f'}) && !ms("isxdigit", {'g'}) &&
              ms("isalnum", {'z'}),
          "ASCII classification");
    check(!ms("isalpha", {0xe9}) && ms("toupper", {0xe9}) == 0xe9,
          "the C locale knows nothing above 127");
    check(ms("toupper", {'q'}) == 'Q' && ms("tolower", {'Q'}) == 'q', "ASCII case");
    check(!ms("isalpha", {0xffffffffu}), "EOF is in no class");
    check(str_at(ms("setlocale", {0, 0})) == "C", "the start-up locale is C");
    uint32_t r = ms("setlocale", {0, gstr("English")});
    check(str_at(r) == "English_United States.1252", "setlocale(LC_ALL, \"English\") = %s",
          str_at(r).c_str());
    check(ms("isalpha", {0xe9}) && ms("toupper", {0xe9}) == 0xc9 && ms("tolower", {0xc9}) == 0xe9,
          "code page 1252 letters after setlocale");
    check(ms("toupper", {0x9a}) == 0x8a && ms("toupper", {0xff}) == 0x9f,
          "case pairs through Unicode (\xc5\xa1, \xc3\xbf)");
    check(ms("toupper", {0xdf}) == 0xdf, "sharp s has no single-byte capital");
    check(ms("isalpha", {(uint32_t)-23}) != 0, "a signed char argument is its unsigned byte");
    check(ms("isdigit", {0xb2}) != 0, "superscript two counts as a digit");
    check(ms("isspace", {0xa0}) != 0, "no-break space is space");
    uint32_t up = gstr("caf\xe9");
    ms("_mbsupr", {up});
    check(str_at(up) == "CAF\xc9", "_mbsupr follows LC_CTYPE");
    check(ms("_mbsicmp", {gstr("\xc9t\xc9"), gstr("\xe9t\xe9")}) == 0, "_mbsicmp folds accents");
    ms("setlocale", {4, gstr("German")});
    std::string all = str_at(ms("setlocale", {0, 0}));
    check(all == "LC_COLLATE=English_United States.1252;LC_CTYPE=English_United States.1252;"
                 "LC_MONETARY=English_United States.1252;LC_NUMERIC=German_Germany.1252;"
                 "LC_TIME=English_United States.1252",
          "a mixed LC_ALL lists its categories: %s", all.c_str());
    uint32_t lc = ms("localeconv", {});
    check(str_at(rd32(lc)) == "," && str_at(rd32(lc + 4)) == ".", "localeconv follows LC_NUMERIC");
    check(fmt_double("%.2f", 1.5) == "1,50", "printf uses LC_NUMERIC's separator");
    {
        uint32_t text = gstr("2,25x"), end = gbuf(4);
        ms("strtod", {text, end});
        check(st0_result() == 2.25 && rd32(end) == text + 4, "strtod reads LC_NUMERIC's separator");
    }
    ms("setlocale", {0, all.size() ? gstr(all) : 0});
    check(str_at(ms("setlocale", {0, 0})) == all, "a composite LC_ALL string round-trips");
    check(ms("setlocale", {0, gstr("Klingon")}) == 0, "an unknown locale is refused");
    ms("setlocale", {0, gstr("C")});
    check(!ms("isalpha", {0xe9}), "back to C");

    // Wide characters.
    check(ms("towupper", {0xe9}) == 0xe9 && ms("towupper", {'a'}) == 'A',
          "towupper changes only ASCII in the C locale");
    ms("setlocale", {0, gstr("")});
    check(ms("towupper", {0xe9}) == 0xc9 && ms("towupper", {0x3b1}) == 0x391 &&
              ms("towlower", {0x416}) == 0x436,
          "towupper/towlower through Unicode in a real locale");
    check(ms("iswctype", {0x416, 0x103}) != 0 && ms("iswctype", {'5', 0x4}) != 0 &&
              ms("iswctype", {0x3000, 0x8}) != 0,
          "iswctype");
    uint32_t wide = gbuf(32);
    check(ms("mbstowcs", {wide, gstr("\x80\xe9"), 8}) == 2 && rd16(wide) == 0x20ac &&
              rd16(wide + 2) == 0xe9,
          "mbstowcs maps code page 1252");
    uint32_t back = gbuf(8);
    check(ms("wcstombs", {back, wide, 8}) == 2 && rd8(back) == 0x80, "wcstombs maps back");
    wr16(wide, 0x416);
    check(ms("wcstombs", {back, wide, 8}) == 0xffffffffu, "wcstombs refuses an unmappable char");
    check(rd32(imports_alloc_data("msvcrt.dll", "__mb_cur_max")) == 1, "__mb_cur_max is 1");
}

// ---------------------------------------------------------------------------
// printf.
// ---------------------------------------------------------------------------
static void test_printf() {
    printf("printf\n");
    reset_world();
    expect_fmt("%f", 1.5, "1.500000");
    expect_fmt("%.0f", 2.5, "3"); // half up on the digit string
    expect_fmt("%.0f", 0.5, "1");
    expect_fmt("%.0f", 0.4, "0");
    expect_fmt("%.2f", 0.125, "0.13");
    expect_fmt("%.2f", 2.675, "2.67"); // 2.67499999999999982236431605997495353221893310546875
    expect_fmt("%.1f", 0.96, "1.0");
    expect_fmt("%.2f", 0.096, "0.10");
    expect_fmt("%.3f", 0.0004, "0.000");
    expect_fmt("%.20f", 0.1, "0.10000000000000001000"); // 17 digits, then zeros
    expect_fmt("%.0f", 1e20, "100000000000000000000");
    expect_fmt("%f", 123456789012345678.0, "123456789012345680.000000");
    expect_fmt("%e", 100.0, "1.000000e+002");
    expect_fmt("%E", 1.5e-10, "1.500000E-010");
    expect_fmt("%.0e", 5.0, "5e+000");
    expect_fmt("%#.0e", 5.0, "5.e+000");
    expect_fmt("%e", 0.0, "0.000000e+000");
    expect_fmt("%e", 9.9999999e300, "1.000000e+301");
    expect_fmt("%g", 100.0, "100");
    expect_fmt("%g", 0.0001, "0.0001");
    expect_fmt("%g", 0.00001, "1e-005");
    expect_fmt("%g", 123456789.0, "1.23457e+008");
    expect_fmt("%g", 999999.5, "1e+006");
    expect_fmt("%G", 1e-20, "1E-020");
    expect_fmt("%#g", 1.0, "1.00000");
    expect_fmt("%.3g", 3.14159, "3.14");
    expect_fmt("%g", 0.0, "0");
    expect_fmt("%f", -0.0, "-0.000000");
    expect_fmt("%+.1f", 1.0, "+1.0");
    expect_fmt("% .1f", 1.0, " 1.0");
    expect_fmt("%8.3f", 3.14159, "   3.142");
    expect_fmt("%-8.2f|", 3.14159, "3.14    |");
    expect_fmt("%08.2f", -3.14159, "-0003.14");
    expect_fmt("%6.6f", 12.5, "12.500000");
    // Infinities and NaNs as msvcrt spells them.
    double inf = HUGE_VAL;
    expect_fmt("%f", inf, "1.#INF00");
    expect_fmt("%f", -inf, "-1.#INF00");
    expect_fmt("%.2f", inf, "1.#J");
    expect_fmt("%.1f", inf, "1.$");
    expect_fmt("%.0f", inf, "1");
    expect_fmt("%e", inf, "1.#INF00e+000");
    expect_fmt("%g", inf, "1.#INF");
    expect_fmt("%f", from_bits(0xfff8000000000000ull), "-1.#IND00");
    expect_fmt("%f", from_bits(0x7ff8000000000000ull), "1.#QNAN0");
    expect_fmt("%f", from_bits(0x7ff0000000000001ull), "1.#SNAN0");

    expect_fmti("%d|%i|%u", {(uint32_t)-5, 7, 0xffffffffu}, "-5|7|4294967295");
    expect_fmti("[%5d][%-5d][%05d]", {42, 42, 42}, "[   42][42   ][00042]");
    expect_fmti("%x %X %#x %#o %o", {255, 255, 255, 8, 0}, "ff FF 0xff 010 0");
    expect_fmti("%#x", {0}, "0");
    expect_fmti("%p", {0xabcd}, "0000ABCD");
    expect_fmti("%.3d|%.0d|%5.3d", {7, 0, 7}, "007||  007");
    expect_fmti("%+d % d %+d", {5, 5, (uint32_t)-5}, "+5  5 -5");
    expect_fmti("%hd %hu", {0x12345, 0xffff}, "9029 65535");
    expect_fmti("%I64d %I64x", {0xffffffffu, 0xffffffffu, 0x89abcdef, 0x01234567},
                "-1 123456789abcdef");
    expect_fmti("%ld %lu", {(uint32_t)-1, 1}, "-1 1");
    expect_fmti("%s|%.3s|%5s|%-5s|", {gstr("text"), gstr("abcdef"), gstr("ab"), gstr("ab")},
                "text|abc|   ab|ab   |");
    expect_fmti("%s", {0}, "(null)");
    expect_fmti("%c%c%3c", {'a', 'b', 'c'}, "ab  c");
    expect_fmti("100%% %y", {}, "100% y");
    expect_fmti("%*d|%-*d|%.*f", {4, 1, 4, 1, 2, 0, 0x3ff80000}, "   1|1   |1.50");
    uint32_t wide = gbuf(16);
    gm_put_wstr(wide, "wide", 8);
    expect_fmti("%S|%ls|%C", {wide, wide, 0x41}, "wide|wide|A");
    uint32_t count = gbuf(4);
    expect_fmti("abc%n!", {count}, "abc!");
    check(rd32(count) == 3, "%%n stores the count so far");

    // _snprintf's three cases.
    uint32_t b = gbuf(16);
    check(ms("_snprintf", {b, 8, gstr("%s"), gstr("abc")}) == 3 && str_at(b) == "abc",
          "_snprintf that fits");
    memset(g_mem + b, 'z', 16);
    check(ms("_snprintf", {b, 3, gstr("%s"), gstr("abc")}) == 3 && rd8(b + 3) == 'z',
          "_snprintf of exactly count bytes: no terminator, returns count");
    check(ms("_snprintf", {b, 2, gstr("%s"), gstr("abc")}) == 0xffffffffu && rd8(b + 2) == 'c',
          "_snprintf that overflows returns -1");
    uint32_t va = gbuf(16);
    wr32(va, gstr("va"));
    wr32(va + 4, 9);
    uint32_t vb = gbuf(32);
    check(ms("vsprintf", {vb, gstr("%s=%d"), va}) == 4 && str_at(vb) == "va=9", "vsprintf");
}

// ---------------------------------------------------------------------------
// Number parsing.
// ---------------------------------------------------------------------------
static double strtod_guest(const std::string &s, uint32_t *consumed) {
    uint32_t str = gstr(s), end = gbuf(4);
    ms("strtod", {str, end});
    double v = st0_result();
    if (consumed)
        *consumed = rd32(end) - str;
    return v;
}

static void test_parsing() {
    printf("parsing\n");
    reset_world();
    uint32_t used = 0;
    check(strtod_guest("1.5", &used) == 1.5 && used == 3, "strtod");
    check(bits_of(strtod_guest("0.1", nullptr)) == 0x3fb999999999999aull, "strtod rounds 0.1");
    check(strtod_guest("  -2.5e3xyz", &used) == -2500 && used == 8, "strtod with an exponent");
    check(strtod_guest("1.5d2", &used) == 150 && used == 5, "strtod takes 'd' as an exponent");
    check(strtod_guest("1e", &used) == 1 && used == 1, "a bare exponent marker is not consumed");
    check(strtod_guest("1e+", &used) == 1 && used == 1, "nor is one with only a sign");
    check(strtod_guest("inf", &used) == 0 && used == 0, "no infinity spelling");
    check(strtod_guest("nan", &used) == 0 && used == 0, "no NaN spelling");
    check(strtod_guest("0x10", &used) == 0 && used == 1, "no hexadecimal floats");
    check(strtod_guest(".5", &used) == 0.5 && used == 2, "a leading point");
    check(strtod_guest("5.", &used) == 5 && used == 2, "a trailing point");
    check(strtod_guest(".", &used) == 0 && used == 0, "a point alone is no number");
    check(std::signbit(strtod_guest("-0", nullptr)), "negative zero");
    clear_errno();
    check(std::isinf(strtod_guest("1e400", nullptr)) && guest_errno() == 34,
          "overflow gives HUGE_VAL and ERANGE");
    clear_errno();
    check(strtod_guest("1e-400", nullptr) == 0 && guest_errno() == 34, "underflow gives ERANGE");
    check(strtod_guest("4.9406564584124654e-324", nullptr) == from_bits(1), "the least denormal");
    // Hard cases: correctly rounded.
    check(bits_of(strtod_guest("9007199254740993", nullptr)) == 0x4340000000000000ull,
          "2^53+1 ties to even");
    check(bits_of(strtod_guest("9007199254740993.00000000001", nullptr)) == 0x4340000000000001ull,
          "just above the tie rounds up");
    check(bits_of(strtod_guest("2.2250738585072011e-308", nullptr)) == 0x000fffffffffffffull,
          "the largest denormal");
    ms("atof", {gstr("  3.25 rest")});
    check(st0_result() == 3.25, "atof");

    // sscanf.
    uint32_t i1 = gbuf(4), i2 = gbuf(4), f1 = gbuf(4), d1 = gbuf(8), s1 = gbuf(32);
    check(ms("sscanf",
             {gstr("  42 -7 0.1 2.5 word"), gstr("%d %d %f %lf %s"), i1, i2, f1, d1, s1}) == 5,
          "sscanf converts five fields");
    check((int32_t)rd32(i1) == 42 && (int32_t)rd32(i2) == -7, "%%d");
    uint32_t fbits = rd32(f1);
    check(fbits == 0x3dcccccd, "%%f rounds 0.1 straight to float (%08x)", fbits);
    check(rdf64(d1) == 2.5 && str_at(s1) == "word", "%%lf and %%s");
    // A decimal just above a float midpoint that rounds to the midpoint as a
    // double: converting through double would tie to even and go down.
    check(ms("sscanf", {gstr("1.00000005960464477625"), gstr("%f"), f1}) == 1 &&
              rd32(f1) == 0x3f800001,
          "%%f is not rounded twice (%08x)", rd32(f1));
    check(ms("sscanf", {gstr(""), gstr("%d"), i1}) == 0xffffffffu, "EOF before any conversion");
    check(ms("sscanf", {gstr("   "), gstr("%d"), i1}) == 0xffffffffu, "only spaces is EOF too");
    check(ms("sscanf", {gstr("abc"), gstr("%d"), i1}) == 0, "a matching failure gives 0");
    check(ms("sscanf", {gstr("12345"), gstr("%3d%d"), i1, i2}) == 2 && rd32(i1) == 123 &&
              rd32(i2) == 45,
          "widths");
    check(ms("sscanf", {gstr("0x1F 010 0x10"), gstr("%x %i %i"), i1, i2, f1}) == 3 &&
              rd32(i1) == 31 && rd32(i2) == 8 && rd32(f1) == 16,
          "%%x and %%i prefixes");
    check(ms("sscanf", {gstr("key=value;"), gstr("%[a-z]=%[^;]"), s1, gbuf(32)}) == 2 &&
              str_at(s1) == "key",
          "scansets");
    uint32_t n = gbuf(4);
    check(ms("sscanf", {gstr("ab 12"), gstr("%*s %d%n"), i1, n}) == 1 && rd32(i1) == 12 &&
              rd32(n) == 5,
          "assignment suppression and %%n");
    check(ms("sscanf", {gstr("4294967297"), gstr("%d"), i1}) == 1 && rd32(i1) == 1,
          "%%d wraps on overflow");
    check(ms("sscanf", {gstr("1ex"), gstr("%f%s"), f1, s1}) == 2 && rdf32(f1) == 1.0f &&
              str_at(s1) == "x",
          "%%f consumes an exponent marker it cannot use");
    check(ms("sscanf", {gstr("7 x"), gstr("%d y"), i1}) == 1, "a literal mismatch after a field");

    // Integers.
    uint32_t end = gbuf(4);
    clear_errno();
    check(ms("strtol", {gstr(" -123abc"), end, 10}) == (uint32_t)-123, "strtol");
    check(ms("strtol", {gstr("0x7fffffff"), 0, 0}) == 0x7fffffff, "strtol base 0 hex");
    check(ms("strtol", {gstr("077"), 0, 0}) == 63, "strtol base 0 octal");
    check(ms("strtol", {gstr("99999999999"), 0, 10}) == 0x7fffffff && guest_errno() == 34,
          "strtol saturates with ERANGE");
    check(ms("strtol", {gstr("-99999999999"), 0, 10}) == 0x80000000u, "strtol saturates low");
    uint32_t none = gstr("xyz");
    check(ms("strtol", {none, end, 10}) == 0 && rd32(end) == none, "no digits: end is start");
    check(ms("strtoul", {gstr("-1"), 0, 10}) == 0xffffffffu, "strtoul negates");
    check(ms("strtoul", {gstr("zz"), 0, 36}) == 35 * 36 + 35, "strtoul base 36");
    check(ms("atoi", {gstr("  +42xyz")}) == 42, "atoi");
    check(ms("atoi", {gstr("4294967297")}) == 1, "atoi wraps");
    ms("_atoi64", {gstr("-9000000000")});
    check(((uint64_t)g_cpu.r[R_EDX] << 32 | g_cpu.r[R_EAX]) == (uint64_t)-9000000000ll, "_atoi64");
    uint32_t ib = gbuf(40);
    ms("_itoa", {(uint32_t)-255, ib, 10});
    check(str_at(ib) == "-255", "_itoa base 10");
    ms("_itoa", {(uint32_t)-1, ib, 16});
    check(str_at(ib) == "ffffffff", "_itoa base 16 is unsigned");
    ms("_itoa", {5, ib, 2});
    check(str_at(ib) == "101", "_itoa base 2");
    check(ms("abs", {(uint32_t)-9}) == 9, "abs");
}

// ---------------------------------------------------------------------------
// Maths.
// ---------------------------------------------------------------------------
static double math1(const char *name, double x) {
    std::vector<uint32_t> a;
    push_double(a, x);
    ms(name, a);
    return st0_result();
}

static void test_math() {
    printf("math\n");
    reset_world();
    g_cpu.fpu_cw = 0x027f;
    check(math1("sqrt", 2.0) == std::sqrt(2.0), "sqrt at PC=53 is the IEEE root");
    clear_errno();
    check(bits_of(math1("sqrt", -1.0)) == 0xfff8000000000000ull && guest_errno() == 33,
          "sqrt(-1) is the indefinite NaN with EDOM");
    check(bits_of(math1("sqrt", from_bits(0x7ff0000000000001ull))) == 0x7ff8000000000001ull,
          "sqrt quiets a signalling NaN and keeps its payload");
    g_cpu.fpu_cw = 0x007f; // PC=24
    double r24 = math1("sqrt", 2.0);
    check(r24 == (double)(float)std::sqrt(2.0), "sqrt at PC=24 has a 24-bit significand");
    {
        int e;
        double m = std::frexp(std::sqrt(1e300), &e);
        double want = std::ldexp(std::nearbyint(std::ldexp(m, 24)), e - 24);
        check(math1("sqrt", 1e300) == want && want > 3.4e38,
              "sqrt at PC=24 keeps the extended exponent range");
    }
    // Squares of 24-bit numbers sit exactly where double rounding would err if
    // it happened; the result must be the exactly rounded root.
    int bad = 0;
    for (uint32_t i = 1; i < 20000; ++i) {
        double x = (double)(0x800000 + i * 397) * (double)(0x800000 + i * 397 + 1);
        double got = math1("sqrt", x);
        // Reference: round sqrt to 24 bits using exact integer arithmetic.
        double s = std::sqrt(x);
        int e;
        double m = std::frexp(s, &e);
        double lo = std::floor(std::ldexp(m, 24));
        double cand_lo = std::ldexp(lo, e - 24), cand_hi = std::ldexp(lo + 1, e - 24);
        double mid = (cand_lo + cand_hi) / 2;
        double want = x > mid * mid   ? cand_hi
                      : x < mid * mid ? cand_lo
                                      : (std::fmod(lo, 2) == 0 ? cand_lo : cand_hi);
        if (got != want)
            ++bad;
    }
    check(bad == 0, "sqrt at PC=24 rounds once: %d wrong of 19999", bad);
    g_cpu.fpu_cw = 0x027f;
    check(math1("fabs", -2.5) == 2.5, "fabs");
    check(math1("log", std::exp(1.0)) == 1.0 || std::fabs(math1("log", std::exp(1.0)) - 1) < 1e-15,
          "log");
    clear_errno();
    check(bits_of(math1("log", -1)) == 0xfff8000000000000ull && guest_errno() == 33, "log(-1)");
    check(math1("log", 0) == -HUGE_VAL, "log(0)");
    check(std::fabs(math1("sin", 0.5) - std::sin(0.5)) == 0, "sin");
    std::vector<uint32_t> pa;
    push_double(pa, 2.0);
    push_double(pa, 10.0);
    ms("pow", pa);
    check(st0_result() == 1024.0, "pow");
    std::vector<uint32_t> pb;
    push_double(pb, -8.0);
    push_double(pb, 1.0 / 3);
    ms("pow", pb);
    check(bits_of(st0_result()) == 0xfff8000000000000ull, "pow of a negative base to a fraction");
    std::vector<uint32_t> la;
    push_double(la, 1.5);
    la.push_back(4);
    ms("ldexp", la);
    check(st0_result() == 24.0, "ldexp");
    std::vector<uint32_t> fa;
    push_double(fa, HUGE_VAL);
    check(ms("_finite", fa) == 0, "_finite(inf)");
    std::vector<uint32_t> fb;
    push_double(fb, 1.0);
    check(ms("_finite", fb) == 1, "_finite(1)");

    // _CI* take ST(0) (and ST(1)).
    fpush(&g_cpu, 9.0);
    ms("_CIsqrt", {});
    check(st0_result() == 3.0 && g_cpu.fpu_top == 0, "_CIsqrt replaces ST(0)");
    fpush(&g_cpu, 2.0); // x, ST(1)
    fpush(&g_cpu, 3.0); // y, ST(0)
    ms("_CIpow", {});
    check(st0_result() == 8.0 && g_cpu.fpu_top == 0, "_CIpow is ST(1) to the ST(0)");
    fpush(&g_cpu, 1.0);  // y, ST(1)
    fpush(&g_cpu, -1.0); // x, ST(0)
    ms("_CIatan2", {});
    check(st0_result() == std::atan2(1.0, -1.0), "_CIatan2 is atan2(ST(1), ST(0))");
    fpush(&g_cpu, 0.0);
    ms("_CIcos", {});
    check(st0_result() == 1.0, "_CIcos");
    fpush(&g_cpu, -2.75);
    ms("_ftol", {});
    check(g_cpu.r[R_EAX] == (uint32_t)-2 && g_cpu.r[R_EDX] == 0xffffffffu && g_cpu.fpu_top == 0,
          "_ftol truncates and pops");
    fpush(&g_cpu, 1e30);
    ms("_ftol", {});
    check(g_cpu.r[R_EAX] == 0 && g_cpu.r[R_EDX] == 0x80000000u, "_ftol out of range");
}

// ---------------------------------------------------------------------------
// rand, errno and per-thread state.
// ---------------------------------------------------------------------------
static void test_rand() {
    printf("rand\n");
    reset_world();
    ms("srand", {1});
    std::vector<uint32_t> got;
    for (int i = 0; i < 5; ++i)
        got.push_back(ms("rand", {}));
    // The sequence every msvcrt program sees after srand(1).
    check(got == std::vector<uint32_t>{41, 18467, 6334, 26500, 19169}, "rand after srand(1)");
    ms("srand", {12345});
    uint32_t a = ms("rand", {});
    uint32_t seed = 12345u * 214013u + 2531011u;
    check(a == ((seed >> 16) & 0x7fff), "the LCG");
    // Another host thread is another guest thread: its own seed, from 1.
    uint32_t other = 0;
    std::thread([&] {
        X86 saved = g_cpu;
        other = ms("rand", {});
        g_cpu = saved;
    }).join();
    check(other == 41, "a new thread starts at seed 1 (%u)", other);
    check(ms("rand", {}) == (((seed * 214013u + 2531011u) >> 16) & 0x7fff),
          "this thread's sequence is unaffected");
}

// ---------------------------------------------------------------------------
// Start-up and exit.
// ---------------------------------------------------------------------------
static void test_startup() {
    printf("startup\n");
    reset_world();
    // __dllonexit grows the DLL's own table and hands it back.
    uint32_t pbegin = gbuf(4), pend = gbuf(4);
    uint32_t table = ms("malloc", {32});
    wr32(pbegin, table);
    wr32(pend, table);
    for (uint32_t i = 0; i < 40; ++i)
        check(ms("__dllonexit", {0x1000 + i, pbegin, pend}) == 0x1000 + i, "__dllonexit %u", i);
    uint32_t b = rd32(pbegin), e = rd32(pend);
    check(e - b == 160 && rd32(b) == 0x1000 && rd32(b + 156) == 0x1000 + 39,
          "the table holds all forty in order");
    check(ms("_msize", {b}) >= 160, "and is a heap block the DLL can free");
    ms("free", {b});
    check(ms("_onexit", {0x2000}) == 0x2000, "_onexit");
    uint32_t fm = ms("__p__fmode", {});
    check(fm && rd32(fm) == 0 && ms("__p__fmode", {}) == fm, "__p__fmode is stable, text");
    check(ms("__p__commode", {}) != 0, "__p__commode");
    uint32_t pargc = gbuf(4), pargv = gbuf(4), penvp = gbuf(4);
    check(ms("__getmainargs", {pargc, pargv, penvp, 0, gbuf(4)}) == 0, "__getmainargs");
    check(rd32(pargc) >= 1 && rd32(rd32(pargv) + 4 * rd32(pargc)) == 0 && rd32(rd32(penvp)),
          "argv is NULL-terminated and the environment is not empty");
    auto args = crt::command_line_arguments(
        "\"C:\\Program Files\\a.exe\" one \"two three\" a\\\\\"b c\" d\\\"e f\\\\g \"\"");
    check(args == std::vector<std::string>{"C:\\Program Files\\a.exe", "one", "two three", "a\\b c",
                                           "d\"e", "f\\\\g", ""},
          "command-line splitting rules (%zu args)", args.size());
    uint32_t path = ms("getenv", {gstr("path")});
    check(path && str_at(path).size() > 0, "getenv matches without case");
    check(ms("getenv", {gstr("NO_SUCH_VARIABLE")}) == 0, "getenv of an unset variable");
    uint32_t acmdln = imports_alloc_data("msvcrt.dll", "_acmdln");
    check(acmdln && str_at(rd32(acmdln)).size() > 0, "_acmdln points at the command line");
    uint32_t iob = imports_alloc_data("msvcrt.dll", "_iob");
    check(iob && rd32(iob + 16) == 0 && rd32(iob + 32 + 16) == 1 && rd32(iob + 32 + 12) == 2,
          "_iob holds stdin, stdout (_IOWRT) and stderr");
    check(ms("fputs", {gstr("to nowhere\n"), iob + 32}) == 0, "stdout accepts output");
    check(ms("fgetc", {iob}) == 0xffffffffu && (rd32(iob + 12) & 0x10), "stdin is at EOF");
}

// ---------------------------------------------------------------------------
// stdio through the guest file system.
// ---------------------------------------------------------------------------
static void test_stdio() {
    printf("stdio\n");
    char dir[256];
    snprintf(dir, sizeof dir, "%s/crt-tests-XXXXXX", os_temp_dir());
    if (!check(os_mkdtemp(dir) == 0, "temporary directory"))
        return;
    reset_world();
    win32_init(dir);
    imports_alloc_data("msvcrt.dll", "_iob");
    uint32_t f = ms("fopen", {gstr("Test.TXT"), gstr("w")});
    check(f != 0, "fopen for writing");
    check(ms("fputs", {gstr("line one\nline two\n"), f}) == 0, "fputs");
    check(ms("fprintf", {f, gstr("%d %s\n"), 42, gstr("three")}) != 0xffffffffu, "fprintf");
    check(ms("fputc", {'Z', f}) == 'Z', "fputc");
    check(ms("fclose", {f}) == 0, "fclose");
    std::string host = std::string(dir) + "/Test.TXT";
    FILE *raw = fopen(host.c_str(), "rb");
    char bytes[64] = {};
    size_t nraw = raw ? fread(bytes, 1, sizeof bytes, raw) : 0;
    if (raw)
        fclose(raw);
    check(nraw == 31 && memcmp(bytes, "line one\r\nline two\r\n42 three\r\nZ", 31) == 0,
          "text mode writes CR LF (%zu bytes)", nraw);

    f = ms("fopen", {gstr("test.txt"), gstr("r")});
    check(f != 0, "fopen finds the file without regard to case");
    uint32_t line = gbuf(64);
    check(ms("fgets", {line, 64, f}) == line && str_at(line) == "line one\n",
          "fgets reads a translated line");
    check(ms("ftell", {f}) == 10, "ftell is the raw offset");
    uint32_t pos = gbuf(8);
    check(ms("fgetpos", {f, pos}) == 0 && rd64(pos) == 10, "fgetpos");
    check(ms("fgetc", {f}) == 'l', "fgetc");
    check(ms("ungetc", {'L', f}) == 'L' && ms("fgetc", {f}) == 'L', "ungetc");
    uint32_t i1 = gbuf(4), s1 = gbuf(32);
    ms("fgets", {line, 64, f});
    check(ms("fscanf", {f, gstr("%d %s"), i1, s1}) == 2 && rd32(i1) == 42 && str_at(s1) == "three",
          "fscanf");
    check(ms("fsetpos", {f, pos}) == 0 && ms("fgetc", {f}) == 'l', "fsetpos");
    check(ms("fseek", {f, 0, 2}) == 0 && ms("ftell", {f}) == 31, "fseek to the end");
    check(ms("fgetc", {f}) == 0xffffffffu && (rd32(f + 12) & 0x10), "EOF sets _IOEOF");
    ms("rewind", {f});
    check(!(rd32(f + 12) & 0x10) && ms("fgetc", {f}) == 'l', "rewind clears EOF");
    uint32_t rb = gbuf(64);
    ms("rewind", {f});
    check(ms("fread", {rb, 1, 64, f}) == 28, "fread in text mode drops the CRs");
    ms("fclose", {f});

    f = ms("fopen", {gstr("bin.dat"), gstr("w+b")});
    check(ms("_putw", {0x0a0b0c0d, f}) == 0x0a0b0c0d, "_putw");
    check(ms("fwrite", {gstr("\n\x1a\n"), 1, 3, f}) == 3, "fwrite binary");
    ms("rewind", {f});
    check(ms("_getw", {f}) == 0x0a0b0c0d, "_getw");
    check(ms("fread", {rb, 1, 10, f}) == 3 && rd8(rb + 1) == 0x1a, "binary mode keeps everything");
    ms("fclose", {f});
    f = ms("fopen", {gstr("bin.dat"), gstr("rt")});
    ms("fseek", {f, 4, 0});
    check(ms("fgetc", {f}) == '\n' && ms("fgetc", {f}) == 0xffffffffu, "text mode stops at Ctrl-Z");
    ms("fclose", {f});

    f = ms("fopen", {gstr("app.txt"), gstr("a")});
    ms("fputs", {gstr("1"), f});
    ms("fclose", {f});
    f = ms("fopen", {gstr("app.txt"), gstr("a+")});
    ms("fputs", {gstr("2"), f});
    ms("rewind", {f});
    ms("fgets", {line, 64, f});
    check(str_at(line) == "12", "append mode");
    ms("fclose", {f});

    check(ms("fopen", {gstr("missing.txt"), gstr("r")}) == 0 && guest_errno() == 2,
          "a missing file is ENOENT");
    uint32_t st = gbuf(36);
    check(ms("_stat", {gstr("app.txt"), st}) == 0 && rd32(st + 20) == 2 &&
              (rd16(st + 6) & 0xf000) == 0x8000 && rd16(st + 6) == 0x81b6,
          "_stat of a file (mode %04x)", rd16(st + 6));
    check(ms("_stat", {gstr("."), st}) == 0 && (rd16(st + 6) & 0x4000), "_stat of a directory");
    check(ms("_stat", {gstr("nope"), st}) == 0xffffffffu, "_stat of nothing");
    uint32_t ut = gbuf(8);
    wr32(ut, 1000000000);
    wr32(ut + 4, 1000000000);
    check(ms("_utime", {gstr("app.txt"), ut}) == 0 && ms("_stat", {gstr("app.txt"), st}) == 0 &&
              rd32(st + 28) == 1000000000,
          "_utime sets the modification time");
    check(ms("_chmod", {gstr("app.txt"), 0x80}) == 0, "_chmod");
    uint32_t full = ms("_fullpath", {0, gstr("sub\\x.txt"), 0});
    check(full && str_at(full).find("sub\\x.txt") != std::string::npos, "_fullpath: %s",
          str_at(full).c_str());
    check(ms("remove", {gstr("app.txt")}) == 0 && ms("remove", {gstr("app.txt")}) == 0xffffffffu,
          "remove");
    uint32_t tmp = ms("tmpfile", {});
    check(tmp && ms("fputs", {gstr("scratch"), tmp}) == 0 && ms("fclose", {tmp}) == 0, "tmpfile");
    remove((std::string(dir) + "/Test.TXT").c_str());
    remove((std::string(dir) + "/bin.dat").c_str());
    os_rmdir(dir);
}

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
static void test_time() {
    printf("time\n");
    reset_world();
    uint32_t t = gbuf(4);
    uint32_t now = ms("time", {t});
    check(now > 1700000000u && rd32(t) == now, "time");
    wr32(t, 315626635); // 1980-01-02 02:03:55 UTC
    uint32_t tm = ms("gmtime", {t});
    check(tm && rd32(tm + 20) == 80 && rd32(tm + 16) == 0 && rd32(tm + 12) == 2 &&
              rd32(tm + 8) == 2 && rd32(tm + 24) == 3,
          "gmtime");
    check(str_at(ms("asctime", {tm})) == "Wed Jan 02 02:03:55 1980\n",
          "asctime pads the day with a zero: %s", str_at(ms("asctime", {tm})).c_str());
    check(ms("localtime", {t}) == tm, "localtime and gmtime share a buffer");
    wr32(t, 0xffffffffu);
    check(ms("gmtime", {t}) == 0, "gmtime refuses a negative time");
    uint32_t buf = gbuf(128);
    wr32(t, 315626635);
    tm = ms("gmtime", {t});
    check(ms("strftime", {buf, 128, gstr("%Y-%m-%d %H:%M:%S %a %B %j %#d %p"), tm}) > 0 &&
              str_at(buf) == "1980-01-02 02:03:55 Wed January 002 2 AM",
          "strftime: %s", str_at(buf).c_str());
    check(ms("strftime", {buf, 128, gstr("%c"), tm}) > 0 && str_at(buf) == "01/02/80 02:03:55",
          "%%c in the C locale: %s", str_at(buf).c_str());
    check(ms("strftime", {buf, 5, gstr("%Y-%m"), tm}) == 0, "strftime that does not fit");
    uint32_t local = gbuf(36);
    memcpy(g_mem + local, g_mem + tm, 36);
    wr32(local + 32, 0xffffffffu); // tm_isdst: unknown
    uint32_t back = ms("mktime", {local});
    check(back != 0xffffffffu, "mktime");
    uint32_t c0 = ms("clock", {});
    check(c0 < 600000, "clock counts milliseconds since start-up (%u)", c0);
    uint32_t days = ms("_Getdays", {});
    check(str_at(days).rfind(":Sun:Sunday:Mon:Monday", 0) == 0, "_Getdays");
    check(str_at(ms("_Getmonths", {})).rfind(":Jan:January:Feb", 0) == 0, "_Getmonths");
    uint32_t names = ms("_Gettnames", {});
    check(names && str_at(rd32(names)) == "Sun" && str_at(rd32(names + 4 * 38)) == "AM",
          "_Gettnames is one block of pointers and strings");
    ms("free", {names});
}

// ---------------------------------------------------------------------------
// C++ runtime: type_info, dynamic_cast, msvcp60.
// ---------------------------------------------------------------------------
static void test_cxx() {
    printf("c++\n");
    reset_world();
    struct Case {
        const char *raw, *want;
    } cases[] = {
        {".?AVFoo@@", "class Foo"},
        {".?AUBar@ns@@", "struct ns::Bar"},
        {".?AVInner@Outer@ns@@", "class ns::Outer::Inner"},
        {".H", "int"},
        {".PAX", "void *"},
        {".?AW4Kind@@", "enum Kind"},
        {".?AV?$vector@HV?$allocator@H@std@@@std@@",
         "class std::vector<int,class std::allocator<int> >"},
        {".?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@",
         "class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >"},
        {".?AV?$Array@$03@@", "class Array<4>"},
        {".PBD", "char const *"},
    };
    for (auto &k : cases) {
        std::string got = crt::undecorated_type_name(k.raw);
        check(got == k.want, "undecorate %s = %s", k.raw, got.c_str());
    }
    uint32_t ti = gbuf(64);
    wr32(ti, 0);
    wr32(ti + 4, 0);
    gm_put_str(ti + 8, ".?AVWidget@@", 32);
    uint32_t name = ms("?name@type_info@@QBEPBDXZ", {}, ti);
    check(str_at(name) == "class Widget" && rd32(ti + 4) == name &&
              ms("?name@type_info@@QBEPBDXZ", {}, ti) == name,
          "type_info::name caches its result");
    ms("??1type_info@@UAE@XZ", {}, ti);
    check(rd32(ti + 4) == 0, "~type_info releases the name");

    // A hand-built hierarchy: class D : public A, public B (multiple
    // inheritance). D's layout: A at 0, B at 8.
    uint32_t base = gbuf(0x400);
    auto td = [&](uint32_t at, const char *n) {
        wr32(at, 0);
        wr32(at + 4, 0);
        gm_put_str(at + 8, n, 32);
        return at;
    };
    uint32_t tdA = td(base + 0x000, ".?AVA@@"), tdB = td(base + 0x040, ".?AVB@@"),
             tdD = td(base + 0x080, ".?AVD@@"), tdX = td(base + 0x0c0, ".?AVX@@");
    auto bcd = [&](uint32_t at, uint32_t type, uint32_t contained, int32_t mdisp) {
        wr32(at, type);
        wr32(at + 4, contained);
        wr32(at + 8, (uint32_t)mdisp);
        wr32(at + 12, (uint32_t)-1);
        wr32(at + 16, 0);
        wr32(at + 20, 0);
        return at;
    };
    uint32_t bD = bcd(base + 0x100, tdD, 2, 0), bA = bcd(base + 0x120, tdA, 0, 0),
             bB = bcd(base + 0x140, tdB, 0, 8);
    uint32_t arr = base + 0x160;
    wr32(arr, bD);
    wr32(arr + 4, bA);
    wr32(arr + 8, bB);
    uint32_t chd = base + 0x170;
    wr32(chd, 0);
    wr32(chd + 4, 1); // multiple inheritance
    wr32(chd + 8, 3);
    wr32(chd + 12, arr);
    auto col = [&](uint32_t at, uint32_t offset) {
        wr32(at, 0);
        wr32(at + 4, offset);
        wr32(at + 8, 0);
        wr32(at + 12, tdD);
        wr32(at + 16, chd);
        return at;
    };
    uint32_t colA = col(base + 0x180, 0), colB = col(base + 0x1a0, 8);
    uint32_t vtA = base + 0x1c4, vtB = base + 0x1d4; // vftable[-1] is the locator
    wr32(vtA - 4, colA);
    wr32(vtB - 4, colB);
    uint32_t obj = base + 0x200;
    wr32(obj, vtA);
    wr32(obj + 8, vtB);
    check(ms("__RTDynamicCast", {obj, 0, tdA, tdD, 0}) == obj, "downcast A* -> D*");
    check(ms("__RTDynamicCast", {obj + 8, 0, tdB, tdD, 0}) == obj, "downcast B* -> D*");
    check(ms("__RTDynamicCast", {obj, 0, tdA, tdB, 0}) == obj + 8, "cross cast A* -> B*");
    check(ms("__RTDynamicCast", {obj + 8, 0, tdB, tdA, 0}) == obj, "cross cast B* -> A*");
    check(ms("__RTDynamicCast", {obj, 0, tdA, tdX, 0}) == 0, "an unrelated target fails");
    check(ms("__RTDynamicCast", {0, 0, tdA, tdD, 0}) == 0, "NULL stays NULL");
    // Single inheritance: E : A, COL attributes 0. Matching is by name too.
    uint32_t tdE = td(base + 0x240, ".?AVE@@"), tdA2 = td(base + 0x280, ".?AVA@@");
    uint32_t bE = bcd(base + 0x2c0, tdE, 1, 0), bA2 = bcd(base + 0x2e0, tdA, 0, 0);
    uint32_t arr2 = base + 0x300;
    wr32(arr2, bE);
    wr32(arr2 + 4, bA2);
    uint32_t chd2 = base + 0x310;
    wr32(chd2, 0);
    wr32(chd2 + 4, 0);
    wr32(chd2 + 8, 2);
    wr32(chd2 + 12, arr2);
    uint32_t colE = base + 0x320;
    wr32(colE, 0);
    wr32(colE + 4, 0);
    wr32(colE + 8, 0);
    wr32(colE + 12, tdE);
    wr32(colE + 16, chd2);
    uint32_t vtE = base + 0x344;
    wr32(vtE - 4, colE);
    uint32_t objE = base + 0x380;
    wr32(objE, vtE);
    check(ms("__RTDynamicCast", {objE, 0, tdA2, tdE, 0}) == objE, "single inheritance downcast");
    check(ms("__RTDynamicCast", {objE, 0, tdE, tdA2, 0}) == objE,
          "a type descriptor from another module matches by name");
    check(ms("__RTDynamicCast", {objE, 0, tdA2, tdD, 0}) == 0, "single inheritance failure");

    // msvcp60.
    uint32_t lk = gbuf(4);
    check(call("msvcp60.dll", "??0_Lockit@std@@QAE@XZ", {}, lk) == lk, "_Lockit()");
    call("msvcp60.dll", "??0_Lockit@std@@QAE@XZ", {}, lk); // recursive
    call("msvcp60.dll", "??1_Lockit@std@@QAE@XZ", {}, lk);
    call("msvcp60.dll", "??1_Lockit@std@@QAE@XZ", {}, lk);
    check(call("msvcp60.dll", "??4_Lockit@std@@QAEAAV01@ABV01@@Z", {lk}, lk) == lk,
          "_Lockit::operator=");
    uint32_t vft = imports_alloc_data("msvcp60.dll", "??_7bad_cast@std@@6B@");
    uint32_t bc = gbuf(12);
    call("msvcp60.dll", "??0bad_cast@std@@QAE@PBD@Z", {gstr("bad")}, bc);
    check(rd32(bc) == vft && str_at(rd32(bc + 4)) == "bad" && rd32(bc + 8) == 1,
          "bad_cast(const char *)");
    // what() and the destructor through the vftable, as a thrown copy would.
    uint32_t what_tramp = rd32(vft + 4);
    g_cpu.r[R_ECX] = bc;
    uint32_t esp = g_cpu.r[R_ESP];
    wr32(esp - 4, 0x00401234);
    g_cpu.r[R_ESP] = esp - 4;
    imports_dispatch(&g_cpu, what_tramp);
    check(g_cpu.r[R_ESP] == esp && str_at(g_cpu.r[R_EAX]) == "bad", "bad_cast::what via vftable");
    call("msvcp60.dll", "??1bad_cast@std@@UAE@XZ", {}, bc);
    check(rd32(bc + 4) == 0, "~bad_cast frees the message");

    ms("setlocale", {0, gstr("C")});
    call("msvcp60.dll", "_Getcvt", {});
    check(g_cpu.r[R_EAX] == 0 && g_cpu.r[R_EDX] == 0, "_Getcvt in the C locale");
    uint32_t vec = gbuf(16);
    check(call("msvcp60.dll", "_Getctype", {vec}) == vec && rd32(vec) == 0 && rd32(vec + 12) == 1 &&
              (rd16(rd32(vec + 8) + 2 * 'A') & 1),
          "_Getctype hands back a heap table");
    ms("free", {rd32(vec + 8)});
    ms("setlocale", {0, gstr("English")});
    call("msvcp60.dll", "_Getcoll", {});
    check(g_cpu.r[R_EAX] == 0x0409 && g_cpu.r[R_EDX] == 1252, "_Getcoll in English");
    uint32_t cvt = gbuf(8);
    wr32(cvt, 0x0409);
    wr32(cvt + 4, 1252);
    uint32_t wc = gbuf(4);
    check(call("msvcp60.dll", "_Mbrtowc", {wc, gstr("\x80"), 1, 0, cvt}) == 1 && rd16(wc) == 0x20ac,
          "_Mbrtowc in code page 1252");
    uint32_t mb = gbuf(4);
    check(call("msvcp60.dll", "_Wcrtomb", {mb, 0x20ac, 0, cvt}) == 1 && rd8(mb) == 0x80,
          "_Wcrtomb");
    wr32(cvt, 0);
    check(call("msvcp60.dll", "_Wcrtomb", {mb, 0x20ac, 0, cvt}) == 0xffffffffu,
          "_Wcrtomb in the C locale refuses U+20AC");
    uint32_t x = gstr("abc"), y = gstr("abd");
    check(call("msvcp60.dll", "_Strcoll", {x, x + 3, y, y + 3, 0}) == 0xffffffffu &&
              call("msvcp60.dll", "_Strcoll", {x, x + 2, y, y + 2, 0}) == 0 &&
              call("msvcp60.dll", "_Strcoll", {x, x + 3, y, y + 2, 0}) == 1,
          "_Strcoll");
    uint32_t xf = gbuf(8);
    check(call("msvcp60.dll", "_Strxfrm", {xf, xf + 8, x, x + 3, 0}) == 3, "_Strxfrm");
    check(call("msvcp60.dll", "_Toupper", {0xe9, vec}) == 0xe9, "_Toupper with a C vector");
    wr32(vec, 0x0409);
    check(call("msvcp60.dll", "_Toupper", {0xe9, vec}) == 0xc9 &&
              call("msvcp60.dll", "_Tolower", {0xc9, vec}) == 0xe9,
          "_Toupper/_Tolower with a 1252 vector");
    check(call("msvcp60.dll", "btowc", {0x80}) == 0x20ac &&
              call("msvcp60.dll", "wctob", {0x20ac}) == 0x80 &&
              call("msvcp60.dll", "wctob", {0xffff}) == 0xffffffffu,
          "btowc/wctob");
}

// Every msvcrt/msvcp60 entry point has a registered shim with a calling
// convention, except the ones other layers own.
static void test_registration() {
    printf("registration\n");
    reset_world();
    const char *elsewhere[] = {"_CxxThrowException", "__CxxFrameHandler", "__CxxLongjmpUnwind",
                               "_except_handler3",   "_setjmp3",          "longjmp",
                               "_controlfp"};
    for (const char *n : elsewhere)
        check(imports_resolve("msvcrt.dll", n) == 0, "%s is left to its own layer", n);
    for (const char *n : {"malloc", "sprintf", "??2@YAPAXI@Z", "__RTDynamicCast", "_CIpow"})
        check(imports_argc(imports_resolve("MSVCRT.dll", n)) == ARGC_CDECL, "%s is cdecl", n);
}

// ---------------------------------------------------------------------------
// The behaviour vectors: `crt_tests --vectors in.tsv out.tsv` runs the file a
// Windows harness runs against msvcrt.dll, through the HLE, and prints the
// results in the same format (the game repository's tests/crt describes both
// and compares them). Each vector starts in the C locale.
// ---------------------------------------------------------------------------
namespace vec {

std::string g_line;
void raw(const std::string &s) {
    g_line += s;
}
void field(const std::string &s) {
    g_line += '\t';
    g_line += s;
}
void field_hex(const void *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n && i < 4096; ++i) {
        out.push_back(digits[((const uint8_t *)p)[i] >> 4]);
        out.push_back(digits[((const uint8_t *)p)[i] & 15]);
    }
    field(out);
}
void field_gstr(uint32_t a) {
    if (!a)
        field("NULL");
    else
        field_hex(g_mem + a, crt::gstrlen(a));
}
void field_int(int64_t v) {
    field(std::to_string(v));
}
void field_hexw(uint32_t v, int width) {
    char buf[16];
    snprintf(buf, sizeof buf, "%0*x", width, v);
    field(buf);
}
void field_bits64(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    field(buf);
}
// The x87 extended form of a double, as FSTP m80 writes it.
void field_ext80(double d) {
    uint64_t b = bits_of(d);
    uint16_t sign = (uint16_t)((b >> 63) << 15);
    int exp = (int)((b >> 52) & 0x7ff);
    uint64_t frac = b & 0xfffffffffffffull, mant;
    uint16_t se;
    if (exp == 0x7ff) {
        se = sign | 0x7fff;
        mant = 0x8000000000000000ull | (frac << 11);
    } else if (exp == 0 && frac == 0) {
        se = sign;
        mant = 0;
    } else if (exp == 0) {
        int shift = 0;
        while (!(frac & (1ull << 52))) {
            frac <<= 1;
            ++shift;
        }
        se = sign | (uint16_t)(1 - 1023 - shift + 16383);
        mant = frac << 11;
    } else {
        se = sign | (uint16_t)(exp - 1023 + 16383);
        mant = 0x8000000000000000ull | (frac << 11);
    }
    char buf[24];
    snprintf(buf, sizeof buf, "%04x%016llx", se, (unsigned long long)mant);
    field(buf);
}

std::string unhex(const std::string &h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back((char)std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}
uint64_t hex64(const std::string &h) {
    return std::stoull(h, nullptr, 16);
}
void set_locale(const std::string &name) {
    ms("setlocale", {0, gstr(name)});
}
uint32_t errno_addr() {
    return ms("_errno", {});
}

void run(const std::vector<std::string> &f) {
    const std::string &op = f[1];
    if (op == "rand") {
        ms("srand", {(uint32_t)std::stoull(f[2])});
        for (int i = 0, n = std::stoi(f[3]); i < n; ++i)
            field_int((int32_t)ms("rand", {}));
    } else if (op == "rand_thread") {
        std::vector<uint32_t> vals;
        int n = std::stoi(f[2]);
        std::thread([&] {
            X86 saved = g_cpu;
            for (int i = 0; i < n; ++i)
                vals.push_back(ms("rand", {}));
            g_cpu = saved;
        }).join();
        for (uint32_t v : vals)
            field_int((int32_t)v);
    } else if (op == "strtod" || op == "atof") {
        set_locale(f[2]);
        uint32_t str = gstr(unhex(f[3])), end = gbuf(4);
        wr32(errno_addr(), 0);
        if (op == "strtod")
            ms("strtod", {str, end});
        else
            ms("atof", {str});
        field_bits64(bits_of(st0_result()));
        if (op == "strtod")
            field_int((int32_t)(rd32(end) - str));
        field_int((int32_t)rd32(errno_addr()));
    } else if (op == "sscanf_f" || op == "sscanf_lf" || op == "sscanf_d") {
        bool d = op == "sscanf_d";
        if (!d)
            set_locale(f[2]);
        uint32_t str = gstr(unhex(f[d ? 2 : 3]));
        uint32_t v = gbuf(8), n = gbuf(4);
        wr32(n, 0xffffffffu);
        const char *fmt = op == "sscanf_f" ? "%f%n" : op == "sscanf_lf" ? "%lf%n" : "%d%n";
        int32_t ret = (int32_t)ms("sscanf", {str, gstr(fmt), v, n});
        field_int(ret);
        if (op == "sscanf_f")
            field_hexw(rd32(v), 8);
        else if (op == "sscanf_lf")
            field_bits64(rd64(v));
        else
            field_int((int32_t)rd32(v));
        field_int((int32_t)rd32(n));
    } else if (op.rfind("sprintf_", 0) == 0) {
        uint32_t buf = gbuf(8192);
        memset(g_mem + buf, 0, 8192);
        std::vector<uint32_t> args;
        if (op == "sprintf_d") {
            set_locale(f[2]);
            uint64_t b = hex64(f[4]);
            args = {buf, gstr(unhex(f[3])), (uint32_t)b, (uint32_t)(b >> 32)};
        } else if (op == "sprintf_i") {
            args = {buf, gstr(unhex(f[2])), (uint32_t)std::stoull(f[3])};
        } else if (op == "sprintf_i64") {
            uint64_t b = hex64(f[3]);
            args = {buf, gstr(unhex(f[2])), (uint32_t)b, (uint32_t)(b >> 32)};
        } else {
            args = {buf, gstr(unhex(f[2])), gstr(unhex(f[3]))};
        }
        int32_t ret = (int32_t)ms("sprintf", args);
        field_gstr(buf);
        field_int(ret);
    } else if (op == "snprintf") {
        uint32_t count = (uint32_t)std::stoi(f[2]);
        uint32_t buf = gbuf(64);
        memset(g_mem + buf, 0xcc, 64);
        int32_t ret = (int32_t)ms("_snprintf", {buf, count, gstr(unhex(f[3])), gstr(unhex(f[4]))});
        field_int(ret);
        field_hex(g_mem + buf, count + 4);
    } else if (op == "strtol" || op == "strtoul") {
        uint32_t str = gstr(unhex(f[2])), end = gbuf(4);
        wr32(end, str);
        wr32(errno_addr(), 0);
        uint32_t v = ms(op.c_str(), {str, end, (uint32_t)std::stoi(f[3])});
        field_int(op == "strtol" ? (int64_t)(int32_t)v : (int64_t)v);
        field_int((int32_t)(rd32(end) - str));
        field_int((int32_t)rd32(errno_addr()));
    } else if (op == "atoi") {
        field_int((int32_t)ms("atoi", {gstr(unhex(f[2]))}));
    } else if (op == "atoi64") {
        ms("_atoi64", {gstr(unhex(f[2]))});
        field_int((int64_t)((uint64_t)g_cpu.r[R_EDX] << 32 | g_cpu.r[R_EAX]));
    } else if (op == "itoa") {
        uint32_t buf = gbuf(64);
        memset(g_mem + buf, 0, 64);
        ms("_itoa", {(uint32_t)std::stoi(f[2]), buf, (uint32_t)std::stoi(f[3])});
        field_gstr(buf);
    } else if (op == "math") {
        const std::string &fn = f[2];
        g_cpu.fpu_cw = (uint16_t)std::stoul(f[3], nullptr, 16);
        std::vector<uint32_t> a;
        uint64_t xb = hex64(f[4]);
        a.push_back((uint32_t)xb);
        a.push_back((uint32_t)(xb >> 32));
        if (fn == "pow") {
            uint64_t yb = hex64(f[5]);
            a.push_back((uint32_t)yb);
            a.push_back((uint32_t)(yb >> 32));
        } else if (fn == "ldexp") {
            a.push_back((uint32_t)std::stoi(f[5]));
        }
        wr32(errno_addr(), 0);
        uint32_t r = ms(fn.c_str(), a);
        if (fn == "_finite") {
            field_bits64(r);
            field("00000000000000000000");
        } else {
            double v = st0_result();
            field_bits64(bits_of(v));
            field_ext80(v);
        }
        field_int((int32_t)rd32(errno_addr()));
        g_cpu.fpu_cw = 0x027f;
    } else if (op == "ci") {
        const std::string &fn = f[2];
        g_cpu.fpu_cw = (uint16_t)std::stoul(f[3], nullptr, 16);
        fpush(&g_cpu, from_bits(hex64(f[4])));
        if (fn == "_CIpow" || fn == "_CIatan2")
            fpush(&g_cpu, from_bits(hex64(f[5])));
        ms(fn.c_str(), {});
        double v = st0_result();
        field_bits64(bits_of(v));
        field_ext80(v);
        g_cpu.fpu_cw = 0x027f;
    } else if (op == "ftol") {
        g_cpu.fpu_cw = (uint16_t)std::stoul(f[2], nullptr, 16);
        fpush(&g_cpu, from_bits(hex64(f[3])));
        ms("_ftol", {});
        field_bits64((uint64_t)g_cpu.r[R_EDX] << 32 | g_cpu.r[R_EAX]);
        g_cpu.fpu_cw = 0x027f;
    } else if (op == "ctype") {
        set_locale(f[2]);
        uint32_t ch = (uint32_t)std::stoi(f[3]);
        for (const char *n : {"isalnum", "isalpha", "isdigit", "isspace", "isxdigit"})
            field_hexw(ms(n, {ch}), 1);
        field_int((int32_t)ms("tolower", {ch}));
        field_int((int32_t)ms("toupper", {ch}));
    } else if (op == "wctype") {
        set_locale(f[2]);
        uint32_t w = (uint32_t)std::stoi(f[3]) & 0xffff;
        field_hexw(ms("towupper", {w}) & 0xffff, 4);
        field_hexw(ms("towlower", {w}) & 0xffff, 4);
        field_hexw(ms("iswctype", {w, 0x1ff}), 1);
    } else if (op == "mbconv") {
        set_locale(f[2]);
        uint32_t in = gbuf(4), w = gbuf(8), back = gbuf(4);
        wr8(in, (uint8_t)std::stoi(f[3]));
        wr8(in + 1, 0);
        memset(g_mem + w, 0xcc, 8);
        memset(g_mem + back, 0xcc, 4);
        field_int((int32_t)ms("mbstowcs", {w, in, 4}));
        field_hexw(rd16(w), 4);
        field_int((int32_t)ms("wcstombs", {back, w, 4}));
        field_hexw(rd8(back), 2);
    } else if (op == "mbcase") {
        set_locale(f[2]);
        std::string s = unhex(f[3]);
        uint32_t a = gstr(s);
        ms("_mbsupr", {a});
        field_hex(g_mem + a, s.size());
        uint32_t b = gstr(s);
        ms("_mbslwr", {b});
        field_hex(g_mem + b, s.size());
    } else if (op == "cmp") {
        const std::string &fn = f[2];
        set_locale(f[3]);
        std::string a = unhex(f[4]), b = unhex(f[5]);
        uint32_t n = (uint32_t)std::stoi(f[6]);
        uint32_t r;
        if (fn == "wcscmp") {
            uint32_t wa = gbuf(2 * (uint32_t)a.size() + 2), wb = gbuf(2 * (uint32_t)b.size() + 2);
            for (size_t i = 0; i <= a.size(); ++i)
                wr16(wa + 2 * (uint32_t)i, i < a.size() ? (uint8_t)a[i] : 0);
            for (size_t i = 0; i <= b.size(); ++i)
                wr16(wb + 2 * (uint32_t)i, i < b.size() ? (uint8_t)b[i] : 0);
            r = ms("wcscmp", {wa, wb});
        } else if (fn == "strcmp" || fn == "_stricmp" || fn == "_mbscmp" || fn == "_mbsicmp") {
            r = ms(fn.c_str(), {gstr(a), gstr(b)});
        } else {
            r = ms(fn.c_str(), {gstr(a), gstr(b), n});
        }
        field_int((int32_t)r);
    } else if (op == "setlocale") {
        set_locale("C");
        uint32_t r = ms("setlocale", {(uint32_t)std::stoi(f[2]), gstr(unhex(f[3]))});
        field_gstr(r);
        field_gstr(ms("setlocale", {0, 0}));
        uint32_t lc = ms("localeconv", {});
        for (int i = 0; i < 10; ++i)
            field_gstr(rd32(lc + 4 * (uint32_t)i));
        for (int i = 0; i < 8; ++i)
            field_int(rd8(lc + 40 + (uint32_t)i));
    } else if (op == "asctime" || op == "strftime") {
        int base = op == "asctime" ? 2 : 5;
        uint32_t tm = gbuf(36);
        for (int i = 0; i < 9; ++i)
            wr32(tm + 4 * (uint32_t)i, (uint32_t)std::stoi(f[base + i]));
        if (op == "asctime") {
            field_gstr(ms("asctime", {tm}));
        } else {
            set_locale(f[2]);
            uint32_t buf = gbuf(512);
            memset(g_mem + buf, 0, 512);
            uint32_t r = ms("strftime", {buf, (uint32_t)std::stoi(f[4]), gstr(unhex(f[3])), tm});
            field_int(r);
            if (r)
                field_gstr(buf);
            else
                field("");
        }
    } else if (op == "gmtime") {
        uint32_t t = gbuf(4);
        wr32(t, (uint32_t)std::stoi(f[2]));
        uint32_t tm = ms("gmtime", {t});
        if (!tm)
            field("NULL");
        else
            for (int i = 0; i < 9; ++i)
                field_int((int32_t)rd32(tm + 4 * (uint32_t)i));
    } else if (op == "msize") {
        uint32_t p = ms("malloc", {(uint32_t)std::stoi(f[2])});
        field_int(p ? (int64_t)ms("_msize", {p}) : -1);
        field_int(p & 15);
        ms("free", {p});
    } else if (op == "splitpath") {
        uint32_t out[4];
        for (uint32_t &o : out) {
            o = gbuf(1024);
            memset(g_mem + o, 0, 1024);
        }
        ms("_splitpath", {gstr(unhex(f[2])), out[0], out[1], out[2], out[3]});
        for (uint32_t o : out)
            field_gstr(o);
    } else if (op == "typename") {
        uint32_t ti = gbuf(600);
        wr32(ti, ti);
        wr32(ti + 4, 0);
        gm_put_str(ti + 8, unhex(f[2]).c_str(), 512);
        field_gstr(ms("?name@type_info@@QBEPBDXZ", {}, ti));
    } else if (op == "textfile") {
        std::string mode = unhex(f[2]), content = unhex(f[3]);
        std::string host = std::string(win32_game_dir()) + "/crtvec.tmp";
        FILE *w = fopen(host.c_str(), "wb");
        if (w) {
            fwrite(content.data(), 1, content.size(), w);
            fclose(w);
        }
        uint32_t file = ms("fopen", {gstr("crtvec.tmp"), gstr(mode)});
        if (!file) {
            field("NULL");
        } else {
            uint32_t line = gbuf(16);
            for (int i = 0; i < 64; ++i) {
                uint32_t r = ms("fgets", {line, 16, file});
                field_gstr(r ? line : 0);
                field_int((int32_t)ms("ftell", {file}));
                if (!r)
                    break;
            }
            field_int((rd32(file + 12) & 0x10) != 0);
            ms("rewind", {file});
            uint32_t rb = gbuf(256);
            memset(g_mem + rb, 0, 256);
            field_int(ms("fread", {rb, 1, 200, file}));
            field_gstr(rb);
            field_int((int32_t)ms("ftell", {file}));
            field_int((int32_t)ms("fseek", {file, 3, 0}));
            field_int((int32_t)ms("fgetc", {file}));
            field_int((int32_t)ms("ungetc", {'Q', file}));
            field_int((int32_t)ms("ftell", {file}));
            field_int((int32_t)ms("fgetc", {file}));
            field_int((int32_t)ms("fgetc", {file}));
            ms("fclose", {file});
        }
        remove(host.c_str());
    } else {
        field("UNKNOWN_OP");
    }
}

} // namespace vec

static int run_vectors(const char *in_path, const char *out_path) {
    FILE *in = fopen(in_path, "rb");
    FILE *out = fopen(out_path, "wb");
    if (!in || !out) {
        fprintf(stderr, "cannot open %s or %s\n", in_path, out_path);
        return 2;
    }
    char dir[256];
    snprintf(dir, sizeof dir, "%s/crt-vectors-XXXXXX", os_temp_dir());
    if (os_mkdtemp(dir) != 0)
        return 2;
    reset_world();
    win32_init(dir);
    imports_alloc_data("msvcrt.dll", "_iob");
    std::string line;
    char chunk[1 << 16];
    size_t count = 0;
    while (fgets(chunk, sizeof chunk, in)) {
        line += chunk;
        if (line.empty() || line.back() != '\n')
            continue;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        std::vector<std::string> f;
        size_t at = 0;
        for (;;) {
            size_t tab = line.find('\t', at);
            f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos)
                break;
            at = tab + 1;
        }
        line.clear();
        if (f.size() < 2)
            continue;
        g_scratch_top = SCRATCH;
        vec::g_line = f[0];
        vec::field(f[1]);
        vec::set_locale("C");
        vec::run(f);
        vec::g_line += '\n';
        fwrite(vec::g_line.data(), 1, vec::g_line.size(), out);
        ++count;
    }
    fclose(in);
    fclose(out);
    os_rmdir(dir);
    fprintf(stderr, "%zu vectors -> %s\n", count, out_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--vectors"))
        return run_vectors(argv[2], argv[3]);
    test_heap();
    test_strings();
    test_locale();
    test_printf();
    test_parsing();
    test_math();
    test_rand();
    test_startup();
    test_stdio();
    test_time();
    test_cxx();
    test_registration();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
