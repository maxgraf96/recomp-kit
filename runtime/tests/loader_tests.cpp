// loader_tests.cpp - the multi-module loader on synthetic PE images built here:
// relocation to a configured base, import binding across modules in any
// configuration order, dependency-ordered DllMain and static TLS for static and
// LoadLibrary'd modules, thread notifications, reference counts and failure
// paths; then the guarded arena, heap growth, strict mode, the initial x87
// control word and the import argument counts. Needs no game: every image is
// written to a temporary directory and its DllMain is a native stand-in
// registered as a translated module.
//
//   .venv/bin/python tools/test.py --compile-only && build/recomp/loader_tests
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include "game_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;

static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

static void section(const char *s) {
    printf("\n== %s\n", s);
}

// ---------------------------------------------------------------------------
// A minimal PE32 image: headers, then one read/write/execute section at RVA
// 0x1000 holding code stand-ins, data, the export and import directories, the
// base relocations and optionally a TLS directory.
// ---------------------------------------------------------------------------
namespace pe {

const uint32_t SECTION_RVA = 0x1000, SECTION_SIZE = 0x3000, IMAGE_SIZE = 0x4000;
const uint32_t EXPORTS = 0x2000, IMPORTS = 0x2400, RELOCS = 0x3000, TLS = 0x3800;

struct Import {
    std::string dll;
    std::vector<std::string> names; // "#N" imports ordinal N
};

struct Image {
    uint32_t preferred = 0x10000000;
    bool dll = true, strip = false, tls = false;
    uint32_t entry = 0x1000; // RVA, 0 for none
    std::string name = "image.dll";
    std::vector<std::pair<std::string, uint32_t>> exports; // name, RVA (ordinals from 1)
    std::vector<Import> imports;
    std::vector<uint32_t> relocs; // RVAs of HIGHLOW sites
    std::vector<uint8_t> sec = std::vector<uint8_t>(SECTION_SIZE);

    uint32_t va(uint32_t rva) const {
        return preferred + rva;
    }
    void put32(uint32_t rva, uint32_t v) {
        memcpy(&sec[rva - SECTION_RVA], &v, 4);
    }
    void put_str(uint32_t rva, const std::string &s) {
        memcpy(&sec[rva - SECTION_RVA], s.c_str(), s.size() + 1);
    }
    // An absolute address of this image stored at `rva`, which the linker
    // would have listed as a relocation.
    void pointer(uint32_t rva, uint32_t target_rva) {
        put32(rva, va(target_rva));
        relocs.push_back(rva);
    }
    // The IAT slot RVA of the k-th name of the d-th imported DLL.
    static uint32_t iat_slot(size_t d, size_t k) {
        return IMPORTS + 0x200 + 0x40 * (uint32_t)d + 4 * (uint32_t)k;
    }
    static uint32_t tls_index_slot() {
        return TLS + 0x60;
    }

    std::vector<uint8_t> build() {
        std::vector<uint32_t> rel = relocs;
        uint32_t dirs[16][2] = {};
        if (!exports.empty()) {
            const uint32_t funcs = EXPORTS + 0x40, names = EXPORTS + 0x100, ords = EXPORTS + 0x180;
            uint32_t str = EXPORTS + 0x200;
            put_str(str, name);
            put32(EXPORTS + 12, str);
            str += (uint32_t)name.size() + 1;
            put32(EXPORTS + 16, 1);
            put32(EXPORTS + 20, (uint32_t)exports.size());
            put32(EXPORTS + 24, (uint32_t)exports.size());
            put32(EXPORTS + 28, funcs);
            put32(EXPORTS + 32, names);
            put32(EXPORTS + 36, ords);
            for (size_t i = 0; i < exports.size(); ++i) {
                put32(funcs + 4 * (uint32_t)i, exports[i].second);
                put32(names + 4 * (uint32_t)i, str);
                uint16_t ord = (uint16_t)i;
                memcpy(&sec[ords + 2 * i - SECTION_RVA], &ord, 2);
                put_str(str, exports[i].first);
                str += (uint32_t)exports[i].first.size() + 1;
            }
            dirs[0][0] = EXPORTS;
            dirs[0][1] = 0x400;
        }
        if (!imports.empty()) {
            uint32_t str = IMPORTS + 0x800;
            for (size_t d = 0; d < imports.size(); ++d) {
                const uint32_t desc = IMPORTS + 20 * (uint32_t)d;
                const uint32_t ilt = IMPORTS + 0x100 + 0x20 * (uint32_t)d;
                put32(desc + 0, ilt);
                put_str(str, imports[d].dll);
                put32(desc + 12, str);
                str += (uint32_t)imports[d].dll.size() + 1;
                put32(desc + 16, iat_slot(d, 0));
                for (size_t k = 0; k < imports[d].names.size(); ++k) {
                    const std::string &n = imports[d].names[k];
                    uint32_t thunk;
                    if (n[0] == '#') {
                        thunk = 0x80000000u | (uint32_t)atoi(n.c_str() + 1);
                    } else {
                        thunk = str;
                        put_str(str + 2, n); // hint 0, then the name
                        str += (uint32_t)n.size() + 4;
                        str = (str + 1) & ~1u;
                    }
                    put32(ilt + 4 * (uint32_t)k, thunk);
                    put32(iat_slot(d, k), thunk);
                }
            }
            dirs[1][0] = IMPORTS;
            dirs[1][1] = 20 * (uint32_t)(imports.size() + 1);
        }
        if (tls) {
            // Directory, raw template, index slot, callback array: all VAs, so
            // all relocated like any other stored address.
            put32(TLS + 0, va(TLS + 0x40));
            put32(TLS + 4, va(TLS + 0x50));
            put32(TLS + 8, va(tls_index_slot()));
            put32(TLS + 12, va(TLS + 0x70));
            put32(TLS + 16, 16); // zero fill
            for (uint32_t i = 0; i < 16; ++i)
                sec[TLS + 0x40 + i - SECTION_RVA] = (uint8_t)(0xa0 + i);
            put32(tls_index_slot(), 0xdead);
            put32(TLS + 0x70, va(0x1080)); // the callback; a null ends the array
            for (uint32_t f : {0u, 4u, 8u, 12u, 0x70u})
                rel.push_back(TLS + f);
            dirs[9][0] = TLS;
            dirs[9][1] = 24;
        }
        if (!rel.empty() && !strip) {
            std::sort(rel.begin(), rel.end());
            uint32_t at = RELOCS;
            for (size_t i = 0; i < rel.size();) {
                const uint32_t page = rel[i] & ~0xfffu;
                const uint32_t block = at;
                at += 8;
                for (; i < rel.size() && (rel[i] & ~0xfffu) == page; ++i) {
                    uint16_t e = (uint16_t)(0x3000 | (rel[i] & 0xfff));
                    memcpy(&sec[at - SECTION_RVA], &e, 2);
                    at += 2;
                }
                if ((at - block) % 4)
                    at += 2; // an ABSOLUTE entry pads the block
                put32(block, page);
                put32(block + 4, at - block);
            }
            dirs[5][0] = RELOCS;
            dirs[5][1] = at - RELOCS;
        }
        // Headers: DOS stub pointer, PE signature, file header, optional
        // header with 16 data directories, one section header.
        std::vector<uint8_t> f(0x400 + SECTION_SIZE);
        auto w16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
        auto w32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
        w16(0, 0x5a4d);
        w32(0x3c, 0x40);
        w32(0x40, 0x00004550);
        const size_t fh = 0x44, opt = fh + 20;
        w16(fh + 0, 0x014c);
        w16(fh + 2, 1);
        w16(fh + 16, 224);
        w16(fh + 18, (uint16_t)(0x0102 | (dll ? 0x2000 : 0) | (strip ? 0x0001 : 0)));
        w16(opt + 0, 0x10b);
        w32(opt + 16, entry);
        w32(opt + 28, preferred);
        w32(opt + 32, 0x1000);
        w32(opt + 36, 0x200);
        w32(opt + 56, IMAGE_SIZE);
        w32(opt + 60, 0x400);
        w32(opt + 92, 16);
        for (int i = 0; i < 16; ++i) {
            w32(opt + 96 + 8 * i, dirs[i][0]);
            w32(opt + 100 + 8 * i, dirs[i][1]);
        }
        const size_t sh = opt + 224;
        memcpy(&f[sh], ".text", 5);
        w32(sh + 8, SECTION_SIZE);
        w32(sh + 12, SECTION_RVA);
        w32(sh + 16, SECTION_SIZE);
        w32(sh + 20, 0x400);
        w32(sh + 36, 0xe0000060u);
        memcpy(&f[0x400], sec.data(), sec.size());
        return f;
    }
};

} // namespace pe

static std::string g_dir;
static std::vector<std::string> g_written; // removed again at the end

static std::string write_image(const std::string &file, pe::Image &img) {
    std::string path = g_dir + "/" + file;
    g_written.push_back(path);
    std::vector<uint8_t> bytes = img.build();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f || fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        exit(2);
    }
    fclose(f);
    return path;
}

// ---------------------------------------------------------------------------
// Native stand-ins for the modules' DllMains and TLS callbacks, registered as
// translated code at the addresses the images name.
// ---------------------------------------------------------------------------
struct Event {
    std::string who;
    uint32_t reason, reserved;
};
static std::vector<Event> g_events;
static std::vector<std::string> g_fail; // modules whose DllMain refuses the attach

static uint32_t g_fake_ret = 0x00401000;

static void stdcall_return(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4 + 4 * args;
    recomp_return(c);
}

struct Stub {
    const char *module;
    uint32_t base;
};
// Modules in configuration order; their DllMain is at base + 0x1000 and
// their TLS callback at base + 0x1080.
static const Stub kStubs[] = {{"alpha.dll", 0x10100000},
                              {"beta.dll", 0x10000000},
                              {"gamma.dll", 0x10180000},
                              {"delta.dll", 0x101c0000},
                              {"omega.dll", IMAGE_BASE}};

template <int N> static void dll_main(X86 *c) {
    const char *who = kStubs[N].module;
    const uint32_t reason = arg(c, 1);
    g_events.push_back({who, reason, arg(c, 2)});
    check(arg(c, 0) == kStubs[N].base, "%s DllMain gets its own base as the handle", who);
    const bool refuse = reason == 1 && std::find(g_fail.begin(), g_fail.end(), who) != g_fail.end();
    c->r[R_EAX] = refuse ? 0 : 1;
    stdcall_return(c, 3);
}
template <int N> static void tls_callback(X86 *c) {
    g_events.push_back({std::string("tls:") + kStubs[N].module, arg(c, 1), arg(c, 2)});
    stdcall_return(c, 3);
}

template <int N> static void register_stub() {
    static const uint32_t addrs[] = {kStubs[N].base + 0x1000, kStubs[N].base + 0x1080};
    static void (*const fns[])(X86 *) = {dll_main<N>, tls_callback<N>};
    static RecompHookFn hooks[2]{};
    static uint8_t hooked[2]{};
    static const RecompModule m = {kStubs[N].module,
                                   kStubs[N].base,
                                   kStubs[N].base + pe::IMAGE_SIZE,
                                   addrs,
                                   2,
                                   fns,
                                   hooks,
                                   hooked,
                                   nullptr,
                                   0,
                                   nullptr};
    recomp_module_register(&m);
}

static std::string events() {
    std::string s;
    for (const Event &e : g_events) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s%s:%u/%u", s.empty() ? "" : " ", e.who.c_str(), e.reason,
                 e.reserved);
        s += buf;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Calling imports as the guest does.
// ---------------------------------------------------------------------------
static uint32_t call_import(X86 *c, const char *dll, const char *name,
                            const std::vector<uint32_t> &args) {
    uint32_t tramp = imports_resolve(dll, name);
    if (!check(tramp != 0, "%s!%s has a trampoline", dll, name))
        return 0;
    const uint32_t before = c->r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, g_fake_ret);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

static uint32_t g_scratch = 0x00300000;
static uint32_t guest_str(const char *s) {
    const uint32_t a = g_scratch;
    gm_put_str(a, s, 256);
    g_scratch += 256;
    return a;
}

// ---------------------------------------------------------------------------
// The test images.
// ---------------------------------------------------------------------------
struct Config {
    std::string main;
    std::vector<std::string> paths;
    std::vector<std::string> shas;
    std::vector<LoaderModuleSpec> specs;
};

static Config build_world() {
    Config cfg;
    // The executable: at its preferred base, relocations stripped. Imports
    // alpha by name and by ordinal, a shimmed KERNEL32 function and a Winsock
    // function by ordinal.
    pe::Image exe;
    exe.preferred = IMAGE_BASE;
    exe.dll = false;
    exe.strip = true;
    exe.name = "main.exe";
    exe.imports = {{"alpha.dll", {"AlphaFn", "#2"}},
                   {"KERNEL32.dll", {"GetTickCount"}},
                   {"WS2_32.dll", {"#11", "#10"}},
                   {"WSOCK32.dll", {"#11"}}};
    cfg.main = write_image("main.exe", exe);

    // alpha prefers 0x10000000 like every DLL of its kind and is configured
    // elsewhere; it imports beta, which comes after it in the configuration.
    pe::Image alpha;
    alpha.name = "alpha.dll";
    alpha.exports = {{"AlphaFn", 0x1010}, {"AlphaData", 0x1100}};
    alpha.imports = {{"beta.dll", {"BetaFn"}}};
    alpha.pointer(0x1104, 0x1100);   // a pointer to its own data
    alpha.put32(0x1108, 0x10000000); // a constant that only looks like an address
    alpha.pointer(0x1ffe, 0x1010);   // a relocation straddling a page boundary
    // beta stays at its preferred base.
    pe::Image beta;
    beta.name = "beta.dll";
    beta.exports = {{"BetaFn", 0x1020}};
    beta.pointer(0x1100, 0x1020);
    // gamma and delta are loaded only by LoadLibrary; gamma imports both
    // beta (already loaded) and delta (not yet); delta has static TLS.
    pe::Image gamma;
    gamma.name = "gamma.dll";
    gamma.exports = {{"GammaFn", 0x1030}};
    gamma.imports = {{"beta.dll", {"BetaFn"}}, {"delta.dll", {"DeltaFn"}}};
    pe::Image delta;
    delta.name = "delta.dll";
    delta.exports = {{"DeltaFn", 0x1040}};
    delta.tls = true;

    pe::Image *images[] = {&alpha, &beta, &gamma, &delta};
    for (int i = 0; i < 4; ++i) {
        cfg.paths.push_back(write_image(kStubs[i].module, *images[i]));
        cfg.shas.push_back(loader_hash_file(cfg.paths.back().c_str()));
    }
    for (int i = 0; i < 4; ++i)
        cfg.specs.push_back(
            {kStubs[i].module, cfg.paths[i].c_str(), cfg.shas[i].c_str(), kStubs[i].base, 0x10000});
    return cfg;
}

static bool load(Config &cfg) {
    const std::string sha = loader_hash_file(cfg.main.c_str());
    const bool ok = loader_load_config(cfg.main.c_str(), sha.c_str(), cfg.specs.data(),
                                       (uint32_t)cfg.specs.size());
    if (!ok)
        printf("  loader_error: %s\n", loader_error());
    return ok;
}

// ---------------------------------------------------------------------------

static void test_relocation_and_binding(Config &cfg) {
    section("relocation to the configured base, binding across modules");
    if (!check(load(cfg), "the executable and four DLLs load"))
        exit(1);
    const LoaderModule *alpha = loader_module_named("ALPHA.DLL");
    const LoaderModule *beta = loader_module_named("beta.dll");
    check(alpha && alpha->base == 0x10100000 && alpha->preferred_base == 0x10000000,
          "alpha sits at its configured base, not the 0x10000000 it prefers");
    check(alpha && alpha->relocations == 2, "alpha applied its two relocations (%u)",
          alpha ? alpha->relocations : 0);
    check(rd32(0x10101104) == 0x10101100, "a stored pointer moved with the image (%08x)",
          rd32(0x10101104));
    check(rd32(0x10101108) == 0x10000000, "a constant no relocation names stayed (%08x)",
          rd32(0x10101108));
    check(rd16(0x10101ffe) == 0x1010 && rd16(0x10102000) == 0x1010,
          "a relocation across a page boundary moved as one dword (%08x)", rd32(0x10101ffe));
    check(beta && beta->relocations == 0 && rd32(0x10001100) == 0x10001020,
          "beta at its preferred base needed no relocation");

    const uint32_t exe_iat = IMAGE_BASE + pe::Image::iat_slot(0, 0);
    check(rd32(exe_iat) == 0x10101010, "the executable's AlphaFn slot holds alpha's export (%08x)",
          rd32(exe_iat));
    check(rd32(exe_iat + 4) == 0x10101100, "ordinal 2 binds to AlphaData (%08x)",
          rd32(exe_iat + 4));
    check(rd32(0x10100000 + pe::Image::iat_slot(0, 0)) == 0x10001020,
          "alpha binds BetaFn although beta is configured after it");
    check(rd32(0x10180000 + pe::Image::iat_slot(1, 0)) == 0x101c1040,
          "gamma binds DeltaFn at delta's configured base");
    const uint32_t tick = rd32(IMAGE_BASE + pe::Image::iat_slot(1, 0));
    check(imports_is_trampoline(tick) &&
              !strcmp(imports_describe(tick), "KERNEL32.dll!GetTickCount"),
          "a Win32 import still binds to its shim");
    const uint32_t ws2_11 = rd32(IMAGE_BASE + pe::Image::iat_slot(2, 0));
    const uint32_t ws2_10 = rd32(IMAGE_BASE + pe::Image::iat_slot(2, 1));
    const uint32_t wsock_11 = rd32(IMAGE_BASE + pe::Image::iat_slot(3, 0));
    check(imports_describe(ws2_11) && !strcmp(imports_describe(ws2_11), "WS2_32.dll!inet_addr") &&
              imports_argc(ws2_11) == 1,
          "WS2_32 ordinal 11 binds as inet_addr, one argument (%s)", imports_describe(ws2_11));
    check(imports_describe(ws2_10) && !strcmp(imports_describe(ws2_10), "WS2_32.dll!ioctlsocket") &&
              imports_argc(ws2_10) == 3,
          "WS2_32 ordinal 10 is ioctlsocket (%s)", imports_describe(ws2_10));
    check(imports_describe(wsock_11) &&
              (!strcmp(imports_describe(wsock_11), "WSOCK32.dll!ord11") ||
               !strcmp(imports_describe(wsock_11), "WSOCK32.dll!inet_ntoa")) &&
              imports_argc(wsock_11) == 1,
          "WSOCK32 ordinal 11 is inet_ntoa (%s)", imports_describe(wsock_11));
    const LoaderModule *main = loader_module_named(RECOMP_EXECUTABLE);
    check(main && main->imports == std::vector<std::string>{"alpha.dll"},
          "the executable imports one mapped module");
    check(alpha && alpha->imports == std::vector<std::string>{"beta.dll"}, "alpha imports beta");
    check(loader_module_at_base(0x10180000) == loader_module_named("gamma.dll") &&
              loader_module_at_base(0x10180010) == nullptr,
          "a module handle is its base and nothing else");
}

static void test_static_attach(X86 *c) {
    section("process start: static imports attach, dependencies first");
    g_events.clear();
    check(loader_attach_static(c), "loader_attach_static succeeds");
    check(events() == "beta.dll:1/1 alpha.dll:1/1",
          "beta (alpha's dependency) attaches before alpha, lpReserved nonzero: %s",
          events().c_str());
    check(c->fpu_cw == GUEST_INITIAL_FPU_CW && GUEST_INITIAL_FPU_CW == 0x027f,
          "the process starts with x87 control word %04x", c->fpu_cw);
    const uint32_t alpha_name = guest_str("Alpha.dll");
    check(call_import(c, "KERNEL32.dll", "GetModuleHandleA", {alpha_name}) == 0x10100000,
          "GetModuleHandleA finds the statically loaded alpha");
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "LoadLibraryA", {alpha_name}) == 0x10100000 &&
              call_import(c, "KERNEL32.dll", "FreeLibrary", {0x10100000}) == 1 &&
              call_import(c, "KERNEL32.dll", "FreeLibrary", {0x10100000}) == 1 &&
              g_events.empty() && loader_module_named("alpha.dll")->attached,
          "FreeLibrary never unloads a static import, even past its own references");
}

static void test_dynamic_load(X86 *c) {
    section("LoadLibrary of a translated module and what it imports");
    g_events.clear();
    const uint32_t path = guest_str("C:\\Stub\\GAMMA.DLL");
    const uint32_t h = call_import(c, "KERNEL32.dll", "LoadLibraryExA", {path, 0, 8});
    check(h == 0x10180000, "LoadLibraryExA(path, LOAD_WITH_ALTERED_SEARCH_PATH) -> %08x", h);
    check(events() == "tls:delta.dll:1/0 delta.dll:1/0 gamma.dll:1/0",
          "delta (not yet loaded) attaches first, its TLS callback before its DllMain; beta, "
          "already loaded, does not attach again: %s",
          events().c_str());
    const LoaderModule *delta = loader_module_named("delta.dll");
    const uint32_t index = delta ? delta->tls.index : 0xffffffffu;
    check(index < TLS_SLOTS && rd32(0x101c0000 + pe::Image::tls_index_slot()) == index,
          "delta's TLS index (%u) is where its code reads it", index);
    const uint32_t block = index < TLS_SLOTS ? rd32(TLS_BASE + 4 * index) : 0;
    check(block && rd8(block) == 0xa0 && rd8(block + 15) == 0xaf && rd32(block + 16) == 0,
          "the running thread got a TLS block: the template, then zero fill");
    check(loader_module_named("beta.dll")->load_refs == 2,
          "beta holds a reference for each module importing it, alpha and gamma (%u)",
          loader_module_named("beta.dll")->load_refs);
    const uint32_t gamma_fn = guest_str("GammaFn");
    check(call_import(c, "KERNEL32.dll", "GetProcAddress", {h, gamma_fn}) == 0x10181030,
          "GetProcAddress answers from gamma's export directory");
    const uint32_t buf = g_scratch;
    g_scratch += 256;
    call_import(c, "KERNEL32.dll", "GetModuleFileNameA", {h, buf, 256});
    check(gm_str(buf) == std::string(RECOMP_GUEST_ROOT) + "\\gamma.dll",
          "GetModuleFileNameA names the module beside the image: %s", gm_str(buf).c_str());

    section("thread attach and detach");
    g_events.clear();
    loader_thread_notify(c, 2);
    check(events() == "beta.dll:2/0 alpha.dll:2/0 tls:delta.dll:2/0 delta.dll:2/0 gamma.dll:2/0",
          "DLL_THREAD_ATTACH in load order: %s", events().c_str());
    call_import(c, "KERNEL32.dll", "DisableThreadLibraryCalls", {0x10100000});
    g_events.clear();
    loader_thread_notify(c, 3);
    check(events() == "gamma.dll:3/0 tls:delta.dll:3/0 delta.dll:3/0 beta.dll:3/0",
          "DLL_THREAD_DETACH in reverse, without alpha after DisableThreadLibraryCalls: %s",
          events().c_str());

    section("a guest thread starts at control word 0x027f and notifies the DLLs");
    {
        static uint32_t thread_cw = 0, thread_ran = 0;
        static void (*body)(X86 *) = [](X86 *t) {
            thread_cw = t->fpu_cw;
            thread_ran = 1;
            t->r[R_EAX] = 7;
        };
        const uint32_t start = imports_alloc_trampoline("test", "thread_body", body, 1);
        g_events.clear();
        const uint32_t th = call_import(c, "KERNEL32.dll", "CreateThread", {0, 0, start, 0, 0, 0});
        const uint32_t code = g_scratch;
        g_scratch += 16;
        for (int i = 0;
             i < 2000 && call_import(c, "KERNEL32.dll", "GetExitCodeThread", {th, code}) &&
             rd32(code) == 259;
             ++i)
            call_import(c, "KERNEL32.dll", "Sleep", {1});
        check(thread_ran && thread_cw == 0x027f, "the thread ran at control word %04x", thread_cw);
        check(rd32(code) == 7, "and exited with its body's result (%u)", rd32(code));
        check(events() == "beta.dll:2/0 tls:delta.dll:2/0 delta.dll:2/0 gamma.dll:2/0 "
                          "gamma.dll:3/0 tls:delta.dll:3/0 delta.dll:3/0 beta.dll:3/0",
              "on it: attach before the body, detach after, alpha opted out: %s", events().c_str());
    }

    section("FreeLibrary unloads in reverse and releases the dependencies");
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "FreeLibrary", {h}) == 1, "FreeLibrary(gamma) -> 1");
    check(events() == "gamma.dll:0/0 delta.dll:0/0 tls:delta.dll:0/0",
          "gamma detaches, then delta, which only gamma held: %s", events().c_str());
    check(!loader_module_named("gamma.dll")->attached &&
              !loader_module_named("delta.dll")->attached &&
              loader_module_named("delta.dll")->tls.index == 0xffffffffu &&
              loader_module_named("beta.dll")->load_refs == 1,
          "both are gone, delta's TLS slot is free, beta keeps alpha's reference");
    const uint32_t gamma_name = guest_str("gamma.dll");
    check(call_import(c, "KERNEL32.dll", "GetModuleHandleA", {gamma_name}) == 0 &&
              call_import(c, "KERNEL32.dll", "FreeLibrary", {h}) == 0,
          "an unloaded module has no handle and cannot be freed twice");

    section("a DllMain that refuses");
    g_fail = {"gamma.dll"};
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "LoadLibraryA", {gamma_name}) == 0 &&
              call_import(c, "KERNEL32.dll", "GetLastError", {}) == 1114,
          "LoadLibrary fails with ERROR_DLL_INIT_FAILED");
    check(events() == "tls:delta.dll:1/0 delta.dll:1/0 gamma.dll:1/0 gamma.dll:0/0 delta.dll:0/0 "
                      "tls:delta.dll:0/0",
          "the refusing module is detached and what it loaded is unloaded again: %s",
          events().c_str());
    g_fail.clear();
    check(!loader_module_named("gamma.dll")->attached &&
              !loader_module_named("delta.dll")->attached,
          "nothing stays loaded");
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "LoadLibraryA", {gamma_name}) == 0x10180000 &&
              events() == "tls:delta.dll:1/0 delta.dll:1/0 gamma.dll:1/0",
          "a later load starts again from the pristine image: %s", events().c_str());

    section("a DLL the runtime cannot serve fails the way a missing one does");
    const uint32_t missing = guest_str("C:\\Stub\\Missing Plugin.dll");
    check(call_import(c, "KERNEL32.dll", "LoadLibraryExA", {missing, 0, 8}) == 0 &&
              call_import(c, "KERNEL32.dll", "GetLastError", {}) == 126,
          "LoadLibraryExA -> NULL, ERROR_MOD_NOT_FOUND");

    section("process exit detaches everything, last attached first");
    g_events.clear();
    loader_detach_all(c);
    check(events() == "gamma.dll:0/1 delta.dll:0/1 tls:delta.dll:0/1 alpha.dll:0/1 beta.dll:0/1",
          "DLL_PROCESS_DETACH in reverse attach order with lpReserved set: %s", events().c_str());
}

// gamma and delta importing each other: the cycle attaches once, in
// discovery order, and every reference taken is the one given back.
static void test_import_cycle(Config &cfg) {
    section("an import cycle");
    const std::string saved = g_dir;
    g_dir += "/cycle";
    os_mkdir(g_dir.c_str());
    pe::Image exe;
    exe.preferred = IMAGE_BASE;
    exe.dll = false;
    exe.strip = true;
    exe.imports = {{"KERNEL32.dll", {"GetTickCount"}}};
    const std::string main = write_image("main.exe", exe);
    pe::Image gamma, delta;
    gamma.exports = {{"GammaFn", 0x1030}};
    gamma.imports = {{"delta.dll", {"DeltaFn"}}};
    delta.exports = {{"DeltaFn", 0x1040}};
    delta.imports = {{"gamma.dll", {"GammaFn"}}};
    const std::string gp = write_image("gamma.dll", gamma), dp = write_image("delta.dll", delta);
    const std::string gs = loader_hash_file(gp.c_str()), ds = loader_hash_file(dp.c_str());
    const std::string ms = loader_hash_file(main.c_str());
    g_dir = saved;
    LoaderModuleSpec specs[2] = {{"gamma.dll", gp.c_str(), gs.c_str(), kStubs[2].base, 0x10000},
                                 {"delta.dll", dp.c_str(), ds.c_str(), kStubs[3].base, 0x10000}};
    (void)cfg;
    if (!check(loader_load_config(main.c_str(), ms.c_str(), specs, 2),
               "two DLLs importing each other load"))
        return;
    X86 *c = loader_context();
    g_events.clear();
    const uint32_t g = call_import(c, "KERNEL32.dll", "LoadLibraryA", {guest_str("gamma.dll")});
    const uint32_t d = call_import(c, "KERNEL32.dll", "LoadLibraryA", {guest_str("delta.dll")});
    check(g == kStubs[2].base && d == kStubs[3].base && events() == "delta.dll:1/0 gamma.dll:1/0",
          "loading one attaches the other first, each once: %s", events().c_str());
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "FreeLibrary", {g}) == 1 && events() == "gamma.dll:0/0" &&
              loader_module_named("delta.dll")->attached,
          "freeing gamma detaches it and leaves delta its own reference: %s", events().c_str());
    g_events.clear();
    check(call_import(c, "KERNEL32.dll", "FreeLibrary", {d}) == 1 && events() == "delta.dll:0/0" &&
              !loader_module_named("gamma.dll")->load_refs &&
              !loader_module_named("delta.dll")->load_refs,
          "freeing delta detaches it, and gives back nothing it never took: %s", events().c_str());
}

static void test_dll_main_image(Config &cfg) {
    section("a DLL as the main image, relocated to the image base");
    pe::Image omega;
    omega.name = "omega.dll";
    omega.preferred = 0x10000000;
    omega.imports = {{"beta.dll", {"BetaFn"}}};
    omega.pointer(0x1100, 0x1100);
    const std::string path = write_image("omega.dll", omega);
    const std::string sha = loader_hash_file(path.c_str());
    std::vector<LoaderModuleSpec> specs = {cfg.specs[1]}; // beta
    if (!check(loader_load_config(path.c_str(), sha.c_str(), specs.data(), 1), "loads at %08x: %s",
               IMAGE_BASE, loader_error()))
        return;
    check(rd32(IMAGE_BASE + 0x1100) == IMAGE_BASE + 0x1100,
          "relocated from 0x10000000 to the image base");
    X86 *c = loader_context();
    g_events.clear();
    run_entry(c);
    check(events() == "beta.dll:1/1 omega.dll:1/1",
          "run_entry attaches its imports, then runs its DllMain rather than a process entry: %s",
          events().c_str());

    section("images that cannot be placed");
    pe::Image fixed;
    fixed.preferred = 0x00800000;
    fixed.dll = false;
    fixed.strip = true;
    const std::string fixed_path = write_image("fixed.exe", fixed);
    const std::string fixed_sha = loader_hash_file(fixed_path.c_str());
    check(!loader_load_config(fixed_path.c_str(), fixed_sha.c_str(), nullptr, 0) &&
              strstr(loader_error(), "no relocations"),
          "an executable without relocations away from its base: %s", loader_error());
    LoaderModuleSpec moved = cfg.specs[0];
    pe::Image stripped;
    stripped.strip = true;
    const std::string stripped_path = write_image("stripped.dll", stripped);
    const std::string stripped_sha = loader_hash_file(stripped_path.c_str());
    moved.name = "stripped.dll";
    moved.path = stripped_path.c_str();
    moved.sha256 = stripped_sha.c_str();
    const std::string main_sha = loader_hash_file(cfg.main.c_str());
    check(!loader_load_config(cfg.main.c_str(), main_sha.c_str(), &moved, 1) &&
              strstr(loader_error(), "no relocations"),
          "a DLL without relocations away from its base: %s", loader_error());
    LoaderModuleSpec twice[2] = {cfg.specs[0], cfg.specs[0]};
    twice[1].name = "beta.dll";
    twice[1].path = cfg.specs[1].path;
    twice[1].sha256 = cfg.specs[1].sha256;
    check(!loader_load_config(cfg.main.c_str(), main_sha.c_str(), twice, 2) &&
              strstr(loader_error(), "overlaps"),
          "two modules in one place: %s", loader_error());
}

static void test_heap_growth(Config &cfg) {
    section("the heap grows past its base arena, around module places");
    if (!load(cfg))
        return;
    if (GUEST_SIZE <= GUEST_SHIM_END) {
        check(true, "guest_size leaves no room to grow (skipped)");
        return;
    }
    const uint32_t big = heap_alloc(HEAP_LIMIT - HEAP_BASE - 0x100000);
    check(big != 0, "one block takes nearly all of the base arena (%08x)", big);
    const uint32_t grown = heap_alloc(0x01000000, true);
    check(grown >= GUEST_SHIM_END && grown + 0x01000000 <= GUEST_SIZE,
          "the next 16 MB comes from above the trampolines (%08x)", grown);
    bool clear = true;
    for (const LoaderModuleSpec &s : cfg.specs)
        clear &= grown + 0x01000000 <= s.base || grown >= s.base + s.size;
    check(clear, "and stays clear of every configured module place");
    check(heap_check().empty(), "the heap is consistent: %s", heap_check().c_str());
    check(heap_span() > (uint64_t)(HEAP_LIMIT - HEAP_BASE), "the heap now spans %llu MB",
          (unsigned long long)(heap_span() >> 20));
    wr32(grown + 0x00fffffc, 0x12345678);
    check(rd32(grown + 0x00fffffc) == 0x12345678, "grown memory is ordinary guest memory");
    check(heap_free(grown) && heap_free(big) && heap_check().empty(),
          "freeing both leaves a consistent heap");
}

// What a replay of recorded guest calls needs: memory at the exact addresses
// the recording names, above guest_size included, and a heap that keeps out
// of it while guest code allocates around it.
static void test_exact_ranges(Config &cfg) {
    section("memory at exact addresses: mem_open, heap_exclude");
    if (!load(cfg))
        return;
    if (!mem_guarded()) {
        check(true, "no guarded arena on this host (skipped)");
        return;
    }
    const uint32_t high = 0x40001234u;
    check(!gm_valid(high, 4) && !mem_committed(high, 4) && mem_readable_end(high) == high,
          "%08x, above guest_size, is shut", high);
    check(mem_open(high, 0x2000), "mem_open opens %08x+0x2000", high);
    check(gm_valid(high, 0x2000) && mem_committed(high, 0x2000),
          "the shims' checks accept the opened range");
    wr32(high, 0xfeedfaceu);
    gm_put_str(high + 4, "replayed", 16);
    check(rd32(high) == 0xfeedfaceu && gm_str(high + 4) == "replayed",
          "it is ordinary guest memory, strings included");
    check(mem_readable_end(high) == 0x40010000u, "readable to the end of its 64 KB grain (%08x)",
          mem_readable_end(high));
    check(!gm_valid(0x40010000u, 1) && !gm_valid(high, 0x10000) &&
              mem_readable_end(0x50000000u) == 0x50000000u,
          "and nothing past it");
    check(mem_open(GUEST_SIZE - 0x10, 0x20) && gm_valid(GUEST_SIZE - 8, 16) &&
              mem_readable_end(0x00400000u) > GUEST_SIZE,
          "a range across guest_size continues the arena (readable to %08x)",
          mem_readable_end(0x00400000u));
    check(!mem_open(0xffff8000u, 0x100) && !mem_open(high, 0),
          "nothing past 0xffff0000, and no empty range");

    const uint32_t hole = heap_alloc(0x20000, false, 0x1000);
    const uint64_t span = heap_span();
    check(hole != 0 && heap_free(hole), "a free stretch of the base arena at %08x", hole);
    check(heap_exclude(hole + 0x10, 0x1fff0) && heap_span() == span - 0x20000,
          "heap_exclude takes it, in whole pages");
    std::vector<uint32_t> blocks;
    bool clear = true;
    for (uint32_t i = 0; i < 96; ++i) {
        const uint32_t size = 0x800u * (1 + i % 7);
        const uint32_t b = heap_alloc(size);
        blocks.push_back(b);
        clear &= b != 0 && (b + size <= hole || b >= hole + 0x20000);
    }
    check(clear, "no allocation lands in it");
    check(heap_check().empty(), "the heap is consistent around the hole: %s", heap_check().c_str());
    check(!heap_exclude(blocks[5] + 0x10, 0x10), "a range over a live block is refused");
    for (uint32_t b : blocks)
        heap_free(b);
    const uint32_t again = heap_alloc(0x20000);
    check(heap_check().empty() && (again + 0x20000 <= hole || again >= hole + 0x20000),
          "freeing everything never bridges it: %s", heap_check().c_str());
    heap_free(again);
    if (GUEST_SIZE > GUEST_SHIM_END) {
        check(heap_exclude(GUEST_SHIM_END, GUEST_SIZE - GUEST_SHIM_END),
              "excluding everything above the trampolines");
        check(heap_alloc(HEAP_LIMIT - HEAP_BASE) == 0,
              "leaves growth nowhere to go for a block the base arena cannot hold");
    }
    load(cfg);
    check(!gm_valid(high, 4) && !mem_committed(high, 4), "mem_init shuts what was opened");
    check(heap_span() == (uint64_t)(HEAP_LIMIT - HEAP_BASE) && heap_check().empty(),
          "and gives the heap its whole base arena back");
}

// ---------------------------------------------------------------------------
// Children: the guarded arena and strict mode end the process by design, so
// they run in a copy of this binary. A stop through abort() would also wake
// the system crash reporter; the child turns SIGABRT into a plain exit.
// ---------------------------------------------------------------------------
static void abort_exits(const char *) {
    os_exit_immediately(77);
}

static int child(const char *which, const char *log) {
    if (!freopen(log, "w", stderr))
        return 2;
    os_install_fault_handlers(abort_exits);
    mem_init();
    imports_init();
    X86 c;
    loader_init_context(&c);
    if (!strcmp(which, "--child-wild-read"))
        return (int)rd32(0xf0000000u);
    if (!strcmp(which, "--child-null-read"))
        return (int)rd32(0x00000010u);
    if (!strcmp(which, "--child-strict-call")) {
        c.r[R_ESP] -= 4;
        wr32(c.r[R_ESP], 0x00401234);
        recomp_unknown_call(&c, 0x00987654);
        return 3; // not reached in strict mode
    }
    if (!strcmp(which, "--child-strict-argc")) {
        const uint32_t t =
            imports_alloc_trampoline("mystery.dll", "Mystery", nullptr, ARGC_UNKNOWN);
        c.r[R_ESP] -= 4;
        wr32(c.r[R_ESP], 0x00401234);
        imports_dispatch(&c, t);
        return 3;
    }
    if (!strcmp(which, "--child-lenient-call")) {
        c.r[R_ESP] -= 4;
        wr32(c.r[R_ESP], 0x00401234);
        c.r[R_EAX] = 5;
        recomp_unknown_call(&c, 0x00987654);
        return c.eip == 0x00401234 && c.r[R_EAX] == 0 ? 0 : 4;
    }
    return 2;
}

// Runs a child of this binary with one switch set; its exit code, and its
// stderr in *err.
static int run_child(const char *exe, const char *which, const char *env_name,
                     const char *env_value, std::string *err) {
    const std::string log = g_dir + "/child.log";
    remove(log.c_str());
    if (env_name)
        os_setenv(env_name, env_value);
    const char *args[] = {exe, which, log.c_str(), nullptr};
    int64_t pid = 0;
    int code = -1;
    if (os_spawn(args, &pid) != 0 || os_wait(pid, &code) != 0)
        code = -1;
    if (env_name)
        os_unsetenv(env_name);
    *err = "";
    if (FILE *f = fopen(log.c_str(), "rb")) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        *err = buf;
        fclose(f);
    }
    return code;
}

static void test_guards_and_strict(const char *exe) {
    section("the guarded arena");
    check(sizeof(void *) < 8 || mem_guarded(), "a 64-bit host reserves the 4 GB guest space");
    std::string err;
    int code = run_child(exe, "--child-wild-read", nullptr, nullptr, &err);
    check(code == 5 && err.find("guest memory fault") != std::string::npos &&
              err.find("f0000000") != std::string::npos &&
              err.find("outside the guest arena") != std::string::npos,
          "a read past the arena is reported with its guest address (exit %d)", code);
    code = run_child(exe, "--child-null-read", "RECOMP_NULL_GUARD", "1", &err);
    check(code == 5 && err.find("null page") != std::string::npos,
          "with the null guard armed, a read of guest 0x10 faults as on Windows (exit %d)", code);

    section("strict mode");
    code = run_child(exe, "--child-strict-call", "RECOMP_STRICT", "1", &err);
    check(code == 77 && err.find("strict: call 00987654") != std::string::npos &&
              err.find("from 00401234") != std::string::npos,
          "an undeliverable call stops the run with its target and caller (exit %d)", code);
    code = run_child(exe, "--child-strict-argc", "RECOMP_STRICT", "1", &err);
    check(code == 77 && err.find("unknown stdcall argument count") != std::string::npos &&
              err.find("mystery.dll!Mystery") != std::string::npos,
          "a call through an import of unknown argument count stops it (exit %d)", code);
    code = run_child(exe, "--child-lenient-call", "RECOMP_STRICT", "0", &err);
    check(code == 0, "without strict mode the call returns 0 and the run continues (exit %d)",
          code);
}

// ---------------------------------------------------------------------------
// Import argument counts.
// ---------------------------------------------------------------------------
static void registered_agrees(const ImportShim &s, void *user) {
    std::vector<std::string> &bad = *static_cast<std::vector<std::string> *>(user);
    if (s.argc_stdcall == ARGC_UNKNOWN)
        return;
    const uint8_t known = imports_known_win32_argc(s.dll, s.name);
    if (known != ARGC_UNKNOWN && known != s.argc_stdcall)
        bad.push_back(std::string(s.dll) + "!" + s.name);
}

static void test_argc() {
    section("stdcall argument counts without a shim");
    struct Case {
        const char *dll, *name;
        uint8_t want;
    } cases[] = {
        {"KERNEL32.dll", "CreateFileA", 7},
        {"kernel32", "GetVolumeInformationA", 8},
        {"USER32.dll", "wsprintfA", ARGC_CDECL},
        {"USER32.dll", "WindowFromPoint", 2},
        {"GDI32.dll", "CreateFontA", 14},
        {"WS2_32.dll", "recvfrom", 6},
        {"wsock32.dll", "__WSAFDIsSet", 2},
        {"IPHLPAPI.DLL", "GetAdaptersInfo", 2},
        {"mss32.dll", "_AIL_open_digital_driver@16", ARGC_UNKNOWN}, // decoration: imports.cpp
        {"msvcrt.dll", "strlen", ARGC_CDECL},
        {"MSVCRT.dll", "_CxxThrowException", 2},
        {"msvcrt.dll", "__CxxLongjmpUnwind", 1},
        {"MSVCP60.dll", "_Getctype", ARGC_CDECL},
        {"MSVCP60.dll", "??0_Lockit@std@@QAE@XZ", 0},
        {"MSVCP60.dll", "??1bad_cast@std@@UAE@XZ", 0},
        {"MSVCP60.dll", "??0bad_cast@std@@QAE@PBD@Z", 1},
        {"MSVCP60.dll", "??4_Lockit@std@@QAEAAV01@ABV01@@Z", 1},
        {"MSVCP60.dll", "??_7bad_cast@std@@6B@", ARGC_UNKNOWN}, // a vftable is data
        {"engine.dll", "?f@@YAXH@Z", ARGC_CDECL},
        {"engine.dll", "?g@@YGHHPAD@Z", 2},
        {"engine.dll", "?h@@YGXN_J@Z", 4},
        {"engine.dll", "?v@C@@QAEXHZZ", ARGC_CDECL},
        {"engine.dll", "?k@@YGXVfoo@@@Z", ARGC_UNKNOWN}, // a class by value has no known size
        {"engine.dll", "?m@C@@QAEXPAV1@0@Z", 2},         // a back reference to a parameter
        {"engine.dll", "?n@?$T@H@@QAEXH@Z", 1},          // a template's member
        {"engine.dll", "?p@@YGXP6GHH@ZH@Z", 2},          // a function pointer parameter
        {"KERNEL32.dll", "NoSuchFunction", ARGC_UNKNOWN},
    };
    for (const Case &k : cases) {
        const uint8_t got = imports_infer_argc(k.dll, k.name);
        check(got == k.want, "%s!%s -> %u (want %u)", k.dll, k.name, got, k.want);
    }
    check(imports_known_win32_sorted(), "the Win32 table is sorted (%zu entries)",
          imports_known_win32_count());
    imports_init();
    std::vector<std::string> bad;
    imports_for_each_registered(registered_agrees, &bad);
    std::string list;
    for (const std::string &b : bad)
        list += " " + b;
    check(bad.empty(), "every shim's own count agrees with the table:%s", list.c_str());
    const uint32_t t =
        imports_alloc_trampoline("mss32.dll", "_AIL_open_digital_driver@16", nullptr, ARGC_UNKNOWN);
    check(imports_argc(t) == 4, "a decorated name still gives its count through the trampoline");
    check(!strcmp(imports_ordinal_name("WS2_32.dll", 12), "inet_ntoa") &&
              !strcmp(imports_ordinal_name("WSOCK32.dll", 12), "ioctlsocket") &&
              !strcmp(imports_ordinal_name("ws2_32", 115), "WSAStartup") &&
              !strcmp(imports_ordinal_name("wsock32", 151), "__WSAFDIsSet") &&
              imports_ordinal_name("KERNEL32.dll", 1) == nullptr,
          "Winsock ordinals name their functions, and differ where the two DLLs differ");
}

int main(int argc, char **argv) {
    if (argc == 3 && !strncmp(argv[1], "--child-", 8))
        return child(argv[1], argv[2]);
    os_setenv("RECOMP_LOG", "1");
    os_unsetenv("RECOMP_STRICT");
    os_unsetenv("RECOMP_NULL_GUARD");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/recomp-loader-%08x", os_temp_dir(),
             (unsigned)(os_monotonic_ns() & 0xffffffffu));
    g_dir = tmpl;
    os_mkdir(g_dir.c_str());

    register_stub<0>();
    register_stub<1>();
    register_stub<2>();
    register_stub<3>();
    register_stub<4>();

    test_argc();
    Config cfg = build_world();
    test_relocation_and_binding(cfg);
    X86 *c = loader_context();
    test_static_attach(c);
    test_dynamic_load(c);
    test_heap_growth(cfg);
    test_exact_ranges(cfg);
    test_import_cycle(cfg);
    test_dll_main_image(cfg);
    test_guards_and_strict(argv[0]);

    for (auto it = g_written.rbegin(); it != g_written.rend(); ++it)
        os_unlink(it->c_str());
    os_unlink((g_dir + "/child.log").c_str());
    os_unlink((g_dir + "/cycle/child.log").c_str());
    os_rmdir((g_dir + "/cycle").c_str());
    os_rmdir(g_dir.c_str());
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
