// msvcp60.cpp - the parts of msvcp60.dll a program's own copy of the
// Dinkumware C++ library imports: the library lock, std::bad_cast, and the
// locale hooks (_Getcoll, _Getctype, _Getcvt and the conversions that take
// their results).
//
// The locale vectors carry a handle (the LCID, 0 for the "C" locale) and a
// code page; the conversions below decide by that handle, as msvcp60 does,
// so a facet built under one locale keeps its behaviour after setlocale.
//
// std::_Lockit is one process-wide recursive lock. It is a guest critical
// section, entered through the runtime's own EnterCriticalSection, because a
// guest thread can yield at any import call inside the locked region and
// another thread must then wait for it rather than walk in.
#include "msvcrt_internal.h"
#include <algorithm>

namespace crt {
namespace {

// ---------------------------------------------------------------------------
// _Lockit
// ---------------------------------------------------------------------------
struct Lock {
    uint32_t cs = 0;
    uint32_t epoch = 0;
};
Lock &lock() {
    static Lock l;
    return l;
}
uint32_t lock_section(X86 *c) {
    Lock &l = lock();
    if (l.epoch != heap_epoch()) {
        l.cs = 0;
        l.epoch = heap_epoch();
    }
    if (!l.cs) {
        l.cs = mem_alloc(24); // sizeof(CRITICAL_SECTION)
        if (l.cs) {
            memset(g_mem + l.cs, 0, 24);
            guest_call(c, imports_resolve("KERNEL32.dll", "InitializeCriticalSection"), l.cs);
        }
    }
    return l.cs;
}
void lockit_ctor(X86 *c) {
    uint32_t self = c->r[R_ECX];
    if (uint32_t cs = lock_section(c))
        guest_call(c, imports_resolve("KERNEL32.dll", "EnterCriticalSection"), cs);
    set_eax(c, self);
}
void lockit_dtor(X86 *c) {
    if (uint32_t cs = lock_section(c))
        guest_call(c, imports_resolve("KERNEL32.dll", "LeaveCriticalSection"), cs);
}
// _Lockit &operator=(const _Lockit &): the lock object has no state.
void lockit_assign(X86 *c) {
    set_eax(c, c->r[R_ECX]);
}

// ---------------------------------------------------------------------------
// std::bad_cast: msvcrt's exception layout, vftable then the message and
// whether the object owns it.
// ---------------------------------------------------------------------------
uint32_t g_bad_cast_vftable = 0;

void exception_release(uint32_t self) {
    uint32_t what = rd32(self + 4);
    if (what && rd32(self + 8))
        mem_free(what, "std::bad_cast::~bad_cast");
    wr32(self + 4, 0);
    wr32(self + 8, 0);
}
// bad_cast::bad_cast(const char *message)
void bad_cast_ctor(X86 *c) {
    uint32_t self = c->r[R_ECX], msg = arg(c, 0);
    wr32(self, g_bad_cast_vftable);
    uint32_t copy = msg ? mem_strdup(std::string(gchars(msg), gstrlen(msg))) : 0;
    wr32(self + 4, copy);
    wr32(self + 8, copy ? 1 : 0);
    set_eax(c, self);
}
void bad_cast_dtor(X86 *c) {
    exception_release(c->r[R_ECX]);
}
// The vftable's scalar deleting destructor: flags bit 0 frees the object.
void bad_cast_deleting_dtor(X86 *c) {
    uint32_t self = c->r[R_ECX];
    exception_release(self);
    if (arg(c, 0) & 1)
        mem_free(self, "std::bad_cast::`scalar deleting destructor'");
    set_eax(c, self);
}
// exception::what: the message, or "Unknown exception".
void bad_cast_what(X86 *c) {
    uint32_t self = c->r[R_ECX];
    uint32_t what = rd32(self + 4);
    if (!what) {
        static uint32_t unknown = 0, unknown_epoch = 0;
        if (!unknown || unknown_epoch != heap_epoch()) {
            unknown = mem_strdup("Unknown exception");
            unknown_epoch = heap_epoch();
        }
        what = unknown;
    }
    set_eax(c, what);
}
void init_bad_cast_vftable(uint32_t addr) {
    g_bad_cast_vftable = addr;
    wr32(addr,
         imports_alloc_trampoline("msvcp60.dll", "std::bad_cast::`scalar deleting destructor'",
                                  bad_cast_deleting_dtor, 1));
    wr32(addr + 4,
         imports_alloc_trampoline("msvcp60.dll", "std::bad_cast::what", bad_cast_what, 0));
}

// ---------------------------------------------------------------------------
// Locale vectors.
// ---------------------------------------------------------------------------
// The handle and code page of the current LC_CTYPE (or LC_COLLATE, which
// changes together with it in every locale this runtime offers).
uint64_t handle_and_page() {
    if (ctype_is_c_locale())
        return 0;
    return (uint64_t)1252 << 32 | 0x0409;
}

// _Collvec _Getcoll(void): 8 bytes, returned in EDX:EAX.
void crt_Getcoll(X86 *c) {
    set_eax64(c, handle_and_page());
}
// _Cvtvec _Getcvt(void): likewise.
void crt_Getcvt(X86 *c) {
    set_eax64(c, handle_and_page());
}
// _Ctypevec _Getctype(void): 16 bytes through the hidden return pointer:
// handle, page, a heap copy of the classification table (256 shorts, which
// the facet frees because _Delfl is 1).
void crt_Getctype(X86 *c) {
    uint32_t out = arg(c, 0);
    uint64_t hp = handle_and_page();
    uint32_t table = mem_alloc(512);
    if (table)
        for (int i = 0; i < 256; ++i)
            wr16(table + 2 * (uint32_t)i, ctype_bits(i));
    wr32(out, (uint32_t)hp);
    wr32(out + 4, (uint32_t)(hp >> 32));
    wr32(out + 8, table);
    wr32(out + 12, table ? 1 : 0);
    set_eax(c, out);
}

bool vec_is_c(uint32_t vec) {
    return !vec || rd32(vec) == 0;
}

// int _Mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *st,
//              const _Cvtvec *vec): one byte is one character here.
void crt_Mbrtowc(X86 *c) {
    uint32_t pwc = arg(c, 0), s = arg(c, 1), n = arg(c, 2), vec = arg(c, 4);
    if (!s) {
        set_eax(c, 0);
        return;
    }
    if (!n) {
        set_eax(c, 0xfffffffeu); // -2: incomplete
        return;
    }
    uint8_t b = rd8(s);
    uint16_t w = vec_is_c(vec) ? b : cp1252_to_unicode(b);
    if (pwc)
        wr16(pwc, w);
    set_eax(c, b ? 1 : 0);
}
// int _Wcrtomb(char *s, wchar_t wc, mbstate_t *st, const _Cvtvec *vec)
void crt_Wcrtomb(X86 *c) {
    uint32_t s = arg(c, 0), vec = arg(c, 3);
    uint32_t w = arg(c, 1) & 0xffff;
    int b = vec_is_c(vec) ? (w < 256 ? (int)w : -1) : unicode_to_cp1252(w);
    if (b < 0) {
        set_errno(42); // EILSEQ
        set_eax(c, 0xffffffffu);
        return;
    }
    if (s)
        wr8(s, (uint8_t)b);
    set_eax(c, 1);
}
// int _Strcoll(s1, s1end, s2, s2end, vec): byte order, the shorter first on
// a common prefix. (A Windows locale sorts linguistically; byte order is the
// "C" locale's answer and the one this runtime gives everywhere.)
void crt_Strcoll(X86 *c) {
    uint32_t a = arg(c, 0), ae = arg(c, 1), b = arg(c, 2), be = arg(c, 3);
    uint32_t na = ae - a, nb = be - b;
    int r = memcmp(g_mem + a, g_mem + b, std::min(na, nb));
    if (r)
        set_eax(c, r < 0 ? 0xffffffffu : 1u);
    else
        set_eax(c, na == nb ? 0 : na < nb ? 0xffffffffu : 1u);
}
// size_t _Strxfrm(char *dst, char *dst_end, const char *src,
//                 const char *src_end, vec): the identity transform; the
// length needed is returned whether or not it fitted.
void crt_Strxfrm(X86 *c) {
    uint32_t d = arg(c, 0), de = arg(c, 1), s = arg(c, 2), se = arg(c, 3);
    uint32_t n = se - s;
    if (n <= de - d)
        memmove(g_mem + d, g_mem + s, n);
    set_eax(c, n);
}
// int _Tolower(int c, const _Ctypevec *vec), _Toupper likewise.
void crt_Tolower(X86 *c) {
    int ch = (int)arg(c, 0);
    if (vec_is_c(arg(c, 1)))
        set_eax(c, (uint32_t)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch));
    else
        set_eax(c, (uint32_t)((ch >= 0 && ch < 256) ? mbc_lower((uint8_t)ch) : ch));
}
void crt_Toupper(X86 *c) {
    int ch = (int)arg(c, 0);
    if (vec_is_c(arg(c, 1)))
        set_eax(c, (uint32_t)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch));
    else
        set_eax(c, (uint32_t)((ch >= 0 && ch < 256) ? mbc_upper((uint8_t)ch) : ch));
}
// wint_t btowc(int c); int wctob(wint_t c): under the current LC_CTYPE.
void crt_btowc(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, ch == -1 ? 0xffffu : (uint32_t)mb_to_wide((uint8_t)ch));
}
void crt_wctob(X86 *c) {
    uint32_t w = arg(c, 0) & 0xffff;
    int b = w == 0xffff ? -1 : wide_to_mb(w);
    set_eax(c, (uint32_t)b);
}

const ImportShim shims[] = {
    // __thiscall members: `this` in ECX; the count is of stack arguments.
    {"msvcp60.dll", "??0_Lockit@std@@QAE@XZ", 0, lockit_ctor},
    {"msvcp60.dll", "??1_Lockit@std@@QAE@XZ", 0, lockit_dtor},
    {"msvcp60.dll", "??4_Lockit@std@@QAEAAV01@ABV01@@Z", 1, lockit_assign},
    {"msvcp60.dll", "??0bad_cast@std@@QAE@PBD@Z", 1, bad_cast_ctor},
    {"msvcp60.dll", "??1bad_cast@std@@UAE@XZ", 0, bad_cast_dtor},
    {"msvcp60.dll", "_Getcoll", ARGC_CDECL, crt_Getcoll},
    {"msvcp60.dll", "_Getctype", ARGC_CDECL, crt_Getctype},
    {"msvcp60.dll", "_Getcvt", ARGC_CDECL, crt_Getcvt},
    {"msvcp60.dll", "_Mbrtowc", ARGC_CDECL, crt_Mbrtowc},
    {"msvcp60.dll", "_Wcrtomb", ARGC_CDECL, crt_Wcrtomb},
    {"msvcp60.dll", "_Strcoll", ARGC_CDECL, crt_Strcoll},
    {"msvcp60.dll", "_Strxfrm", ARGC_CDECL, crt_Strxfrm},
    {"msvcp60.dll", "_Tolower", ARGC_CDECL, crt_Tolower},
    {"msvcp60.dll", "_Toupper", ARGC_CDECL, crt_Toupper},
    {"msvcp60.dll", "btowc", ARGC_CDECL, crt_btowc},
    {"msvcp60.dll", "wctob", ARGC_CDECL, crt_wctob},
};

} // namespace
} // namespace crt

void msvcp60_register() {
    imports_register(crt::shims, sizeof crt::shims / sizeof crt::shims[0]);
    imports_register_data_init("msvcp60.dll", "??_7bad_cast@std@@6B@", 8,
                               crt::init_bad_cast_vftable);
}
