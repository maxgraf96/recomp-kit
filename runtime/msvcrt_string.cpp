// msvcrt_string.cpp - str*, mem*, wcs* and the _mbs* family.
//
// Return values follow msvcrt where they are observable: strcmp, memcmp,
// wcscmp and _mbscmp give -1/0/1; strncmp, _stricmp and _strnicmp give the
// difference of the first differing (lower-cased) bytes.
//
// The multibyte code page is 1252, which has no lead bytes, and for such a
// code page msvcrt's _mbs* functions are the corresponding str* functions:
// _mbsicmp is _stricmp, _mbslwr is _strlwr, and so on. Their case-insensitive
// forms therefore follow LC_CTYPE, as _stricmp does.
#include "msvcrt_internal.h"
#include <algorithm>

namespace crt {

// Scans stop where readable guest memory does: the arena's end, or the end of
// a range mem_open opened above it.
uint32_t gstrlen(uint32_t a) {
    const uint32_t limit = mem_readable_end(a);
    if (a >= limit)
        return 0;
    const void *end = memchr(g_mem + a, 0, limit - a);
    return end ? uint32_t((const uint8_t *)end - (g_mem + a)) : limit - a;
}

uint32_t gwcslen(uint32_t a) {
    const uint32_t limit = mem_readable_end(a);
    uint32_t n = 0;
    while ((uint64_t)a + 2 * n + 1 < limit && rd16(a + 2 * n))
        ++n;
    return n;
}

namespace {

inline int sign(int v) {
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// mem*
// ---------------------------------------------------------------------------
// msvcrt's memcpy tolerates overlap like memmove; a program that relied on
// that on Windows must keep working.
void crt_memcpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    if (n && gm_valid(dst, n) && gm_valid(src, n))
        memmove(g_mem + dst, g_mem + src, n);
    set_eax(c, dst);
}
void crt_memset(X86 *c) {
    uint32_t dst = arg(c, 0), n = arg(c, 2);
    if (n && gm_valid(dst, n))
        memset(g_mem + dst, int(arg(c, 1) & 0xff), n);
    set_eax(c, dst);
}
void crt_memcmp(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
    if (!n || !gm_valid(a, n) || !gm_valid(b, n)) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, (uint32_t)sign(memcmp(g_mem + a, g_mem + b, n)));
}
void crt_memchr(X86 *c) {
    uint32_t a = arg(c, 0), n = arg(c, 2);
    if (!n || !gm_valid(a, n)) {
        set_eax(c, 0);
        return;
    }
    const void *hit = memchr(g_mem + a, int(arg(c, 1) & 0xff), n);
    set_eax(c, hit ? a + uint32_t((const uint8_t *)hit - (g_mem + a)) : 0);
}

// ---------------------------------------------------------------------------
// str*
// ---------------------------------------------------------------------------
void crt_strlen(X86 *c) {
    set_eax(c, gstrlen(arg(c, 0)));
}
void crt_strcpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1);
    uint32_t n = gstrlen(src) + 1;
    if (gm_valid(dst, n))
        memmove(g_mem + dst, g_mem + src, n);
    set_eax(c, dst);
}
void crt_strcat(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1);
    uint32_t at = dst + gstrlen(dst);
    uint32_t n = gstrlen(src) + 1;
    if (gm_valid(at, n))
        memmove(g_mem + at, g_mem + src, n);
    set_eax(c, dst);
}
// strncpy copies at most n bytes and pads the rest of the n with NULs; it
// does not terminate a source of n or more bytes.
uint32_t copy_n_padded(uint32_t dst, uint32_t src, uint32_t n) {
    uint32_t len = std::min(gstrlen(src), n);
    if (!gm_valid(dst, n))
        return dst;
    memmove(g_mem + dst, g_mem + src, len);
    memset(g_mem + dst + len, 0, n - len);
    return dst;
}
void crt_strncpy(X86 *c) {
    set_eax(c, copy_n_padded(arg(c, 0), arg(c, 1), arg(c, 2)));
}
// strncat-style append of at most n bytes, always terminated.
uint32_t append_n(uint32_t dst, uint32_t src, uint32_t n) {
    uint32_t at = dst + gstrlen(dst);
    uint32_t len = std::min(gstrlen(src), n);
    if (gm_valid(at, len + 1)) {
        memmove(g_mem + at, g_mem + src, len);
        wr8(at + len, 0);
    }
    return dst;
}
int str_cmp(uint32_t a, uint32_t b) {
    return sign(strcmp(gchars(a), gchars(b)));
}
void crt_strcmp(X86 *c) {
    set_eax(c, (uint32_t)str_cmp(arg(c, 0), arg(c, 1)));
}
int str_ncmp(uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t x = rd8(a + i), y = rd8(b + i);
        if (x != y || !x)
            return int(x) - int(y);
    }
    return 0;
}
void crt_strncmp(X86 *c) {
    set_eax(c, (uint32_t)str_ncmp(arg(c, 0), arg(c, 1), arg(c, 2)));
}
int str_nicmp(uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        int x = ctype_tolower(rd8(a + i)), y = ctype_tolower(rd8(b + i));
        if (x != y || !x)
            return x - y;
    }
    return 0;
}
void crt_stricmp(X86 *c) {
    set_eax(c, (uint32_t)str_nicmp(arg(c, 0), arg(c, 1), 0xffffffffu));
}
void crt_strnicmp(X86 *c) {
    set_eax(c, (uint32_t)str_nicmp(arg(c, 0), arg(c, 1), arg(c, 2)));
}
// strchr finds the terminator too, as the C standard says.
uint32_t str_chr(uint32_t s, uint8_t ch) {
    uint32_t len = gstrlen(s);
    if (!ch)
        return s + len;
    const void *hit = memchr(g_mem + s, ch, len);
    return hit ? s + uint32_t((const uint8_t *)hit - (g_mem + s)) : 0;
}
void crt_strchr(X86 *c) {
    set_eax(c, str_chr(arg(c, 0), uint8_t(arg(c, 1))));
}
void crt_strrchr(X86 *c) {
    uint32_t s = arg(c, 0);
    uint8_t ch = uint8_t(arg(c, 1));
    uint32_t len = gstrlen(s);
    if (!ch) {
        set_eax(c, s + len);
        return;
    }
    for (uint32_t i = len; i-- > 0;)
        if (rd8(s + i) == ch) {
            set_eax(c, s + i);
            return;
        }
    set_eax(c, 0);
}
void crt_strstr(X86 *c) {
    uint32_t h = arg(c, 0), n = arg(c, 1);
    uint32_t hl = gstrlen(h), nl = gstrlen(n);
    if (!nl) {
        set_eax(c, h);
        return;
    }
    const uint8_t *hp = g_mem + h, *np = g_mem + n;
    for (uint32_t i = 0; i + nl <= hl; ++i)
        if (hp[i] == np[0] && memcmp(hp + i, np, nl) == 0) {
            set_eax(c, h + i);
            return;
        }
    set_eax(c, 0);
}
void crt_strcspn(X86 *c) {
    uint32_t s = arg(c, 0), set = arg(c, 1);
    uint32_t sl = gstrlen(s), setl = gstrlen(set);
    uint32_t i = 0;
    for (; i < sl; ++i)
        if (memchr(g_mem + set, g_mem[s + i], setl))
            break;
    set_eax(c, i);
}
// strtok keeps its position per thread.
void crt_strtok(X86 *c) {
    uint32_t s = arg(c, 0), delim = arg(c, 1);
    ThreadData &td = thread_data();
    if (!s)
        s = td.strtok_next;
    if (!s) {
        set_eax(c, 0);
        return;
    }
    uint32_t dl = gstrlen(delim);
    auto is_delim = [&](uint8_t ch) { return ch && memchr(g_mem + delim, ch, dl) != nullptr; };
    while (is_delim(rd8(s)))
        ++s;
    if (!rd8(s)) {
        td.strtok_next = 0;
        set_eax(c, 0);
        return;
    }
    uint32_t start = s;
    while (rd8(s) && !is_delim(rd8(s)))
        ++s;
    if (rd8(s)) {
        wr8(s, 0);
        td.strtok_next = s + 1;
    } else {
        td.strtok_next = s;
    }
    set_eax(c, start);
}
void crt_strdup(X86 *c) {
    uint32_t s = arg(c, 0);
    if (!s) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, mem_strdup(std::string(gchars(s), gstrlen(s))));
}
uint32_t str_case(uint32_t s, bool upper) {
    uint32_t len = gstrlen(s);
    for (uint32_t i = 0; i < len; ++i) {
        int ch = rd8(s + i);
        wr8(s + i, uint8_t(upper ? ctype_toupper(ch) : ctype_tolower(ch)));
    }
    return s;
}

// ---------------------------------------------------------------------------
// wcs*
// ---------------------------------------------------------------------------
void crt_wcslen(X86 *c) {
    set_eax(c, gwcslen(arg(c, 0)));
}
void crt_wcscpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1);
    uint32_t n = 2 * (gwcslen(src) + 1);
    if (gm_valid(dst, n))
        memmove(g_mem + dst, g_mem + src, n);
    set_eax(c, dst);
}
void crt_wcscmp(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1);
    for (uint32_t i = 0;; ++i) {
        uint16_t x = rd16(a + 2 * i), y = rd16(b + 2 * i);
        if (x != y) {
            set_eax(c, x < y ? 0xffffffffu : 1u);
            return;
        }
        if (!x)
            break;
    }
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// _mbs*: the single-byte code page forms.
// ---------------------------------------------------------------------------
void crt_mbscmp(X86 *c) {
    set_eax(c, (uint32_t)str_cmp(arg(c, 0), arg(c, 1)));
}
void crt_mbsicmp(X86 *c) {
    set_eax(c, (uint32_t)str_nicmp(arg(c, 0), arg(c, 1), 0xffffffffu));
}
void crt_mbsncmp(X86 *c) { // counts characters, which are bytes here
    set_eax(c, (uint32_t)str_ncmp(arg(c, 0), arg(c, 1), arg(c, 2)));
}
void crt_mbsnbcmp(X86 *c) {
    set_eax(c, (uint32_t)str_ncmp(arg(c, 0), arg(c, 1), arg(c, 2)));
}
void crt_mbsnbicmp(X86 *c) {
    set_eax(c, (uint32_t)str_nicmp(arg(c, 0), arg(c, 1), arg(c, 2)));
}
void crt_mbschr(X86 *c) {
    set_eax(c, str_chr(arg(c, 0), uint8_t(arg(c, 1))));
}
void crt_mbslen(X86 *c) {
    set_eax(c, gstrlen(arg(c, 0)));
}
void crt_mbsinc(X86 *c) {
    set_eax(c, arg(c, 0) + 1);
}
// _mbsdec(start, current): the character before current, or NULL at start.
void crt_mbsdec(X86 *c) {
    uint32_t start = arg(c, 0), cur = arg(c, 1);
    set_eax(c, cur > start ? cur - 1 : 0);
}
// _mbsninc(s, n): s advanced by n characters, but not past the terminator.
void crt_mbsninc(X86 *c) {
    uint32_t s = arg(c, 0);
    if (!s) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, s + std::min(gstrlen(s), arg(c, 1)));
}
// _mbsnbcnt(s, n): bytes in the first n characters; _mbsnccnt(s, n):
// characters in the first n bytes. The same count in a single-byte code page.
void crt_mbsnbcnt(X86 *c) {
    set_eax(c, std::min(gstrlen(arg(c, 0)), arg(c, 1)));
}
void crt_mbsnccnt(X86 *c) {
    set_eax(c, std::min(gstrlen(arg(c, 0)), arg(c, 1)));
}
void crt_mbsncpy(X86 *c) {
    set_eax(c, copy_n_padded(arg(c, 0), arg(c, 1), arg(c, 2)));
}
void crt_mbsnbcat(X86 *c) {
    set_eax(c, append_n(arg(c, 0), arg(c, 1), arg(c, 2)));
}
void crt_mbslwr(X86 *c) {
    set_eax(c, arg(c, 0) ? str_case(arg(c, 0), false) : 0);
}
void crt_mbsupr(X86 *c) {
    set_eax(c, arg(c, 0) ? str_case(arg(c, 0), true) : 0);
}

// ---------------------------------------------------------------------------
// _splitpath(path, drive, dir, fname, ext): each output optional, limited to
// _MAX_DRIVE (3), _MAX_DIR, _MAX_FNAME and _MAX_EXT (256) with the NUL.
// ---------------------------------------------------------------------------
void put_part(uint32_t out, const char *p, size_t n, size_t cap) {
    if (!out)
        return;
    n = std::min(n, cap - 1);
    memcpy(g_mem + out, p, n);
    wr8(out + uint32_t(n), 0);
}
void crt_splitpath(X86 *c) {
    uint32_t path = arg(c, 0);
    std::string s(gchars(path), gstrlen(path));
    const char *p = s.c_str();
    size_t len = s.size(), at = 0;
    if (len >= 2 && p[1] == ':') {
        put_part(arg(c, 1), p, 2, 3);
        at = 2;
    } else {
        put_part(arg(c, 1), p, 0, 3);
    }
    size_t last_sep = std::string::npos, last_dot = std::string::npos;
    for (size_t i = at; i < len; ++i) {
        if (p[i] == '\\' || p[i] == '/')
            last_sep = i;
        else if (p[i] == '.')
            last_dot = i;
    }
    size_t name_at = at;
    if (last_sep != std::string::npos) {
        put_part(arg(c, 2), p + at, last_sep + 1 - at, 256);
        name_at = last_sep + 1;
    } else {
        put_part(arg(c, 2), p, 0, 256);
    }
    if (last_dot != std::string::npos && last_dot >= name_at) {
        put_part(arg(c, 3), p + name_at, last_dot - name_at, 256);
        put_part(arg(c, 4), p + last_dot, len - last_dot, 256);
    } else {
        put_part(arg(c, 3), p + name_at, len - name_at, 256);
        put_part(arg(c, 4), p, 0, 256);
    }
}

const ImportShim shims[] = {
    {"msvcrt.dll", "memcpy", ARGC_CDECL, crt_memcpy},
    {"msvcrt.dll", "memmove", ARGC_CDECL, crt_memcpy},
    {"msvcrt.dll", "memset", ARGC_CDECL, crt_memset},
    {"msvcrt.dll", "memcmp", ARGC_CDECL, crt_memcmp},
    {"msvcrt.dll", "memchr", ARGC_CDECL, crt_memchr},
    {"msvcrt.dll", "strlen", ARGC_CDECL, crt_strlen},
    {"msvcrt.dll", "strcpy", ARGC_CDECL, crt_strcpy},
    {"msvcrt.dll", "strcat", ARGC_CDECL, crt_strcat},
    {"msvcrt.dll", "strncpy", ARGC_CDECL, crt_strncpy},
    {"msvcrt.dll", "strcmp", ARGC_CDECL, crt_strcmp},
    {"msvcrt.dll", "strncmp", ARGC_CDECL, crt_strncmp},
    {"msvcrt.dll", "_stricmp", ARGC_CDECL, crt_stricmp},
    {"msvcrt.dll", "_strnicmp", ARGC_CDECL, crt_strnicmp},
    {"msvcrt.dll", "strchr", ARGC_CDECL, crt_strchr},
    {"msvcrt.dll", "strrchr", ARGC_CDECL, crt_strrchr},
    {"msvcrt.dll", "strstr", ARGC_CDECL, crt_strstr},
    {"msvcrt.dll", "strcspn", ARGC_CDECL, crt_strcspn},
    {"msvcrt.dll", "strtok", ARGC_CDECL, crt_strtok},
    {"msvcrt.dll", "_strdup", ARGC_CDECL, crt_strdup},
    {"msvcrt.dll", "wcslen", ARGC_CDECL, crt_wcslen},
    {"msvcrt.dll", "wcscpy", ARGC_CDECL, crt_wcscpy},
    {"msvcrt.dll", "wcscmp", ARGC_CDECL, crt_wcscmp},
    {"msvcrt.dll", "_mbscmp", ARGC_CDECL, crt_mbscmp},
    {"msvcrt.dll", "_mbsicmp", ARGC_CDECL, crt_mbsicmp},
    {"msvcrt.dll", "_mbsncmp", ARGC_CDECL, crt_mbsncmp},
    {"msvcrt.dll", "_mbsnbcmp", ARGC_CDECL, crt_mbsnbcmp},
    {"msvcrt.dll", "_mbsnbicmp", ARGC_CDECL, crt_mbsnbicmp},
    {"msvcrt.dll", "_mbschr", ARGC_CDECL, crt_mbschr},
    {"msvcrt.dll", "_mbslen", ARGC_CDECL, crt_mbslen},
    {"msvcrt.dll", "_mbsinc", ARGC_CDECL, crt_mbsinc},
    {"msvcrt.dll", "_mbsdec", ARGC_CDECL, crt_mbsdec},
    {"msvcrt.dll", "_mbsninc", ARGC_CDECL, crt_mbsninc},
    {"msvcrt.dll", "_mbsnbcnt", ARGC_CDECL, crt_mbsnbcnt},
    {"msvcrt.dll", "_mbsnccnt", ARGC_CDECL, crt_mbsnccnt},
    {"msvcrt.dll", "_mbsncpy", ARGC_CDECL, crt_mbsncpy},
    {"msvcrt.dll", "_mbsnbcat", ARGC_CDECL, crt_mbsnbcat},
    {"msvcrt.dll", "_mbslwr", ARGC_CDECL, crt_mbslwr},
    {"msvcrt.dll", "_mbsupr", ARGC_CDECL, crt_mbsupr},
    {"msvcrt.dll", "_splitpath", ARGC_CDECL, crt_splitpath},
};

} // namespace

void register_string() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

} // namespace crt
