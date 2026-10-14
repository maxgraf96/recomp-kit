// msvcrt_locale.cpp - setlocale, localeconv, ctype and the wide/multibyte
// character conversions.
//
// msvcrt classifies bytes through a 256-entry table chosen by LC_CTYPE. In
// the "C" locale only ASCII has properties; in a Western European locale
// (code page 1252) the bytes 0x80..0xff are classified through their Unicode
// characters, and toupper/tolower map through Unicode case pairs that stay
// inside the code page. The _mbs* functions do not follow setlocale: they use
// the multibyte code page, which is the ANSI code page (1252) from start-up.
//
// Wide characters are classified from Unicode in every locale (msvcrt asks
// GetStringTypeW above 255 and uses a Latin-1 table below), but towupper and
// towlower change only ASCII in the "C" locale.
#include "msvcrt_internal.h"
#include "msvcrt_unicode.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace crt {
namespace {

// ---------------------------------------------------------------------------
// Tables.
// ---------------------------------------------------------------------------
uint16_t unicode_bits(uint32_t w) {
    if (w > 0xffff)
        return 0;
    // Binary search for the last run starting at or below w.
    size_t lo = 0, hi = sizeof kCtypeRuns / sizeof kCtypeRuns[0];
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (kCtypeRuns[mid].first <= w)
            lo = mid;
        else
            hi = mid;
    }
    return kCtypeRuns[lo].bits;
}

uint32_t case_map(const CaseRun *runs, size_t n, uint32_t w) {
    if (w > 0xffff)
        return w;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (runs[mid].last < w)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n && runs[lo].first <= w && w <= runs[lo].last &&
        (w - runs[lo].first) % runs[lo].stride == 0)
        return uint32_t(int32_t(w) + runs[lo].delta);
    return w;
}

struct ByteTables {
    uint16_t c_bits[256];
    uint16_t ansi_bits[256];
    uint8_t ansi_lower[256];
    uint8_t ansi_upper[256];
    ByteTables() {
        for (int b = 0; b < 256; ++b) {
            uint16_t u = cp1252_to_unicode((uint8_t)b);
            ansi_bits[b] = kAnsiBits[b];
            // The "C" locale's table is msvcrt's static one, which predates the
            // separate alphabetic bit: letters are only upper or lower.
            c_bits[b] = b < 128 ? (uint16_t)(ansi_bits[b] & ~CT_ALPHA) : 0;
            if (b < 128 && !(c_bits[b] & (CT_UPPER | CT_LOWER)))
                c_bits[b] = ansi_bits[b];
            int lo = unicode_to_cp1252(case_map(kLowerRuns, std::size(kLowerRuns), u));
            int up = unicode_to_cp1252(case_map(kUpperRuns, std::size(kUpperRuns), u));
            ansi_lower[b] = (uint8_t)(lo >= 0 ? lo : b);
            ansi_upper[b] = (uint8_t)(up >= 0 ? up : b);
        }
    }
};
const ByteTables &tables() {
    static const ByteTables t;
    return t;
}

// ---------------------------------------------------------------------------
// Locale state.
// ---------------------------------------------------------------------------
struct LocaleDef {
    const char *name;     // the full name setlocale reports
    const char *language; // the first component, as callers spell it
    const char *abbrev;   // the three-letter Windows abbreviation
    char decimal, thousands;
    const char *currency, *int_currency;
    // localeconv's eight char fields: int_frac_digits, frac_digits,
    // p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space,
    // p_sign_posn, n_sign_posn (as Windows reports them).
    uint8_t mon[8];
};
// The "C" locale is index 0. Every other locale shares code page 1252, so
// only the punctuation differs between them.
const LocaleDef kLocales[] = {
    {"C", "C", "", '.', 0, "", "", {127, 127, 127, 127, 127, 127, 127, 127}},
    {"English_United States.1252",
     "english",
     "enu",
     '.',
     ',',
     "$",
     "USD",
     {2, 2, 1, 0, 1, 0, 3, 0}},
    {"English_United Kingdom.1252",
     "english-uk",
     "eng",
     '.',
     ',',
     "\xa3",
     "GBP",
     {2, 2, 1, 0, 1, 0, 3, 3}},
    {"German_Germany.1252", "german", "deu", ',', '.', "\x80", "EUR", {2, 2, 0, 1, 0, 1, 1, 1}},
    {"French_France.1252", "french", "fra", ',', '\xa0', "\x80", "EUR", {2, 2, 0, 1, 0, 1, 1, 1}},
    {"Spanish_Spain.1252", "spanish", "esp", ',', '.', "\x80", "EUR", {2, 2, 0, 1, 0, 1, 1, 1}},
    {"Italian_Italy.1252", "italian", "ita", ',', '.', "\x80", "EUR", {2, 2, 0, 1, 0, 1, 1, 1}},
    {"Dutch_Netherlands.1252", "dutch", "nld", ',', '.', "\x80", "EUR", {2, 2, 1, 1, 1, 1, 4, 4}},
    {"Portuguese_Brazil.1252",
     "portuguese",
     "ptb",
     ',',
     '.',
     "R$",
     "BRL",
     {2, 2, 1, 1, 1, 1, 3, 3}},
};
constexpr int kUserDefault = 1;

struct LocaleState {
    int cat[CAT_COUNT] = {0, 0, 0, 0, 0, 0};
    uint32_t epoch = 0;
    uint32_t name_buf[CAT_COUNT] = {};
    uint32_t lconv = 0;
    uint32_t lconv_strings = 0;
    int lconv_for = -1;
};
LocaleState &state() {
    static LocaleState s;
    return s;
}

std::string lower(std::string s) {
    for (char &ch : s)
        ch = (char)tolower((unsigned char)ch);
    return s;
}

// Parses a locale name in Windows' "language[_country][.codepage]" form.
// Returns the locale index or -1.
int parse_locale_name(const std::string &raw) {
    if (raw.empty())
        return kUserDefault;
    if (raw == "C")
        return 0;
    std::string s = lower(raw), cp;
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        cp = s.substr(dot + 1);
        s = s.substr(0, dot);
        if (cp != "1252" && cp != "acp" && cp != "")
            return -1; // only code page 1252 exists here
    }
    if (s.empty())
        return kUserDefault; // ".1252": the user's language in that code page
    std::string language = s, country;
    size_t us = s.find('_');
    if (us != std::string::npos) {
        language = s.substr(0, us);
        country = s.substr(us + 1);
    }
    if (language == "american" || language == "american english" || language == "us" ||
        language == "english-us" || language == "english-american" || language == "enu" ||
        language == "usa")
        return 1;
    if (language == "english-uk" || language == "eng") // "british" is refused
        return 2;
    if (language == "english") {
        if (country == "united kingdom" || country == "great britain" || country == "uk" ||
            country == "gbr")
            return 2;
        return 1;
    }
    for (int i = 3; i < int(std::size(kLocales)); ++i)
        if (language == kLocales[i].language || language == kLocales[i].abbrev)
            return i;
    // A full name echoed back.
    for (int i = 1; i < int(std::size(kLocales)); ++i)
        if (lower(raw) == lower(kLocales[i].name))
            return i;
    return -1;
}

const char *const kCategoryNames[CAT_COUNT] = {"LC_ALL",      "LC_COLLATE", "LC_CTYPE",
                                               "LC_MONETARY", "LC_NUMERIC", "LC_TIME"};

// The string setlocale reports for a category; LC_ALL is one name when every
// category agrees and the "LC_COLLATE=...;LC_CTYPE=..." list otherwise.
std::string category_name(int category) {
    LocaleState &st = state();
    if (category != CAT_ALL)
        return kLocales[st.cat[category]].name;
    bool same = true;
    for (int i = CAT_COLLATE + 1; i < CAT_COUNT; ++i)
        same = same && st.cat[i] == st.cat[CAT_COLLATE];
    if (same)
        return kLocales[st.cat[CAT_COLLATE]].name;
    std::string out;
    for (int i = CAT_COLLATE; i < CAT_COUNT; ++i) {
        if (i != CAT_COLLATE)
            out += ';';
        out += kCategoryNames[i];
        out += '=';
        out += kLocales[st.cat[i]].name;
    }
    return out;
}

// Stable per-category guest buffers: setlocale returns a pointer into static
// storage that stays valid, and is rewritten, across later calls.
uint32_t name_buffer(int category) {
    LocaleState &st = state();
    if (st.epoch != heap_epoch()) {
        std::fill(std::begin(st.name_buf), std::end(st.name_buf), 0u);
        st.lconv = st.lconv_strings = 0;
        st.lconv_for = -1;
        st.epoch = heap_epoch();
    }
    if (!st.name_buf[category])
        st.name_buf[category] = mem_alloc(256);
    return st.name_buf[category];
}

// LC_ALL=a;LC_CTYPE=b... as a setlocale argument.
bool apply_composite(const std::string &s) {
    LocaleState &st = state();
    int next[CAT_COUNT];
    std::copy(std::begin(st.cat), std::end(st.cat), next);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t semi = s.find(';', pos);
        std::string part =
            s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? s.size() : semi + 1;
        size_t eq = part.find('=');
        if (eq == std::string::npos)
            return false;
        std::string key = part.substr(0, eq);
        int category = -1;
        for (int i = CAT_COLLATE; i < CAT_COUNT; ++i)
            if (key == kCategoryNames[i])
                category = i;
        int loc = parse_locale_name(part.substr(eq + 1));
        if (category < 0 || loc < 0)
            return false;
        next[category] = loc;
    }
    std::copy(std::begin(next), std::end(next), st.cat);
    return true;
}

// char *setlocale(int category, const char *locale)
void crt_setlocale(X86 *c) {
    int category = (int)arg(c, 0);
    uint32_t name = arg(c, 1);
    LocaleState &st = state();
    if (category < CAT_ALL || category >= CAT_COUNT) {
        set_eax(c, 0);
        return;
    }
    if (name) {
        std::string want(gchars(name), gstrlen(name));
        if (category == CAT_ALL && want.find('=') != std::string::npos) {
            if (!apply_composite(want)) {
                set_eax(c, 0);
                return;
            }
        } else {
            int loc = parse_locale_name(want);
            if (loc < 0) {
                set_eax(c, 0);
                return;
            }
            if (category == CAT_ALL)
                std::fill(std::begin(st.cat), std::end(st.cat), loc);
            else
                st.cat[category] = loc;
        }
    }
    uint32_t buf = name_buffer(category);
    if (!buf) {
        set_eax(c, 0);
        return;
    }
    gm_put_str(buf, category_name(category).c_str(), 256);
    set_eax(c, buf);
}

// struct lconv *localeconv(void): ten char* fields and eight chars (48
// bytes), filled from LC_NUMERIC and LC_MONETARY.
void crt_localeconv(X86 *c) {
    set_eax(c, localeconv_addr());
}

// ---------------------------------------------------------------------------
// ctype functions.
// ---------------------------------------------------------------------------
void crt_isalpha(X86 *c) {
    set_eax(c, ctype_bits((int)arg(c, 0)) & (CT_ALPHA | CT_UPPER | CT_LOWER));
}
void crt_isalnum(X86 *c) {
    set_eax(c, ctype_bits((int)arg(c, 0)) & (CT_ALPHA | CT_UPPER | CT_LOWER | CT_DIGIT));
}
void crt_isdigit(X86 *c) {
    set_eax(c, ctype_bits((int)arg(c, 0)) & CT_DIGIT);
}
void crt_isspace(X86 *c) {
    set_eax(c, ctype_bits((int)arg(c, 0)) & CT_SPACE);
}
void crt_isxdigit(X86 *c) {
    set_eax(c, ctype_bits((int)arg(c, 0)) & CT_HEX);
}
void crt_tolower(X86 *c) {
    set_eax(c, (uint32_t)ctype_tolower((int)arg(c, 0)));
}
void crt_toupper(X86 *c) {
    set_eax(c, (uint32_t)ctype_toupper((int)arg(c, 0)));
}
// wint_t towupper(wint_t), towlower(wint_t): the argument is an unsigned
// short widened to a dword.
// Below 256 a lower-case character (by the wide table) goes through the
// narrow toupper of LC_CTYPE, so towupper(0xff) is 0x9f, the code page 1252
// byte of its capital; above it the "C" locale changes nothing and a real
// locale uses the Unicode case pairs.
void crt_towupper(X86 *c) {
    uint32_t w = arg(c, 0) & 0xffff;
    if (w < 256)
        set_eax(c, (wide_ctype_bits(w) & CT_LOWER) ? (uint32_t)ctype_toupper((int)w) : w);
    else
        set_eax(c, ctype_is_c_locale() ? w : wide_toupper(w));
}
void crt_towlower(X86 *c) {
    uint32_t w = arg(c, 0) & 0xffff;
    if (w < 256)
        set_eax(c, (wide_ctype_bits(w) & CT_UPPER) ? (uint32_t)ctype_tolower((int)w) : w);
    else
        set_eax(c, ctype_is_c_locale() ? w : wide_tolower(w));
}
// int iswctype(wint_t c, wctype_t mask)
void crt_iswctype(X86 *c) {
    uint32_t w = arg(c, 0) & 0xffff;
    uint32_t mask = arg(c, 1) & 0xffff;
    set_eax(c, w == 0xffff ? 0 : (wide_ctype_bits(w) & mask));
}

// size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
void crt_mbstowcs(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    uint32_t len = gstrlen(src);
    if (!dst) {
        set_eax(c, len); // the count the conversion would need, without the NUL
        return;
    }
    uint32_t i = 0;
    for (; i < n; ++i) {
        uint8_t b = rd8(src + i);
        wr16(dst + 2 * i, (uint16_t)mb_to_wide(b));
        if (!b)
            break;
    }
    set_eax(c, i);
}
// size_t wcstombs(char *dst, const wchar_t *src, size_t n)
void crt_wcstombs(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    uint32_t i = 0;
    if (!dst) {
        for (;; ++i) {
            uint16_t w = rd16(src + 2 * i);
            if (!w)
                break;
            if (wide_to_mb(w) < 0) {
                set_errno(42); // EILSEQ
                set_eax(c, 0xffffffffu);
                return;
            }
        }
        set_eax(c, i);
        return;
    }
    for (; i < n; ++i) {
        uint16_t w = rd16(src + 2 * i);
        int b = wide_to_mb(w);
        if (b < 0) {
            set_errno(42);
            set_eax(c, 0xffffffffu);
            return;
        }
        wr8(dst + i, (uint8_t)b);
        if (!w)
            break;
    }
    set_eax(c, i);
}

// _ismbblead(c): no lead bytes in a single-byte code page.
void crt_ismbblead(X86 *c) {
    set_eax(c, 0);
}
void crt_ismbcdigit(X86 *c) {
    set_eax(c, mbc_is_digit(arg(c, 0)) ? 1 : 0);
}
void crt_ismbcspace(X86 *c) {
    set_eax(c, mbc_is_space(arg(c, 0)) ? 1 : 0);
}

void init_mb_cur_max(uint32_t addr) {
    wr32(addr, 1); // MB_CUR_MAX: one byte per character in code page 1252
}

const ImportShim shims[] = {
    {"msvcrt.dll", "setlocale", ARGC_CDECL, crt_setlocale},
    {"msvcrt.dll", "localeconv", ARGC_CDECL, crt_localeconv},
    {"msvcrt.dll", "isalpha", ARGC_CDECL, crt_isalpha},
    {"msvcrt.dll", "isalnum", ARGC_CDECL, crt_isalnum},
    {"msvcrt.dll", "isdigit", ARGC_CDECL, crt_isdigit},
    {"msvcrt.dll", "isspace", ARGC_CDECL, crt_isspace},
    {"msvcrt.dll", "isxdigit", ARGC_CDECL, crt_isxdigit},
    {"msvcrt.dll", "tolower", ARGC_CDECL, crt_tolower},
    {"msvcrt.dll", "toupper", ARGC_CDECL, crt_toupper},
    {"msvcrt.dll", "towupper", ARGC_CDECL, crt_towupper},
    {"msvcrt.dll", "towlower", ARGC_CDECL, crt_towlower},
    {"msvcrt.dll", "iswctype", ARGC_CDECL, crt_iswctype},
    {"msvcrt.dll", "mbstowcs", ARGC_CDECL, crt_mbstowcs},
    {"msvcrt.dll", "wcstombs", ARGC_CDECL, crt_wcstombs},
    {"msvcrt.dll", "_ismbblead", ARGC_CDECL, crt_ismbblead},
    {"msvcrt.dll", "_ismbcdigit", ARGC_CDECL, crt_ismbcdigit},
    {"msvcrt.dll", "_ismbcspace", ARGC_CDECL, crt_ismbcspace},
};

} // namespace

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------
uint16_t cp1252_to_unicode(uint8_t b) {
    return (b >= 0x80 && b < 0xa0) ? kCp1252High[b - 0x80] : b;
}
int unicode_to_cp1252(uint32_t w) {
    if (w < 0x80 || (w >= 0xa0 && w < 0x100))
        return (int)w;
    for (int i = 0; i < 32; ++i)
        if (kCp1252High[i] == w)
            return 0x80 + i;
    return -1;
}

bool ctype_is_c_locale() {
    return state().cat[CAT_CTYPE] == 0;
}

uint16_t ctype_bits(int c) {
    if (c < -128 || c > 255 || c == -1)
        return 0;
    uint8_t b = (uint8_t)c;
    return ctype_is_c_locale() ? tables().c_bits[b] : tables().ansi_bits[b];
}

// tolower/toupper. A value outside 0..255, EOF included, is taken by its low
// byte and mapped through code page 1252 in every locale: msvcrt returns 255
// for tolower(EOF) and 0x9f for toupper(EOF), and a negative char comes back
// as its unsigned mapped byte. Inside 0..255 the "C" locale maps ASCII only.
int ctype_tolower(int c) {
    if (c < 0 || c > 255) {
        uint8_t b = (uint8_t)c;
        return (tables().ansi_bits[b] & CT_UPPER) ? tables().ansi_lower[b] : b;
    }
    if (ctype_is_c_locale())
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    uint8_t b = (uint8_t)c;
    if (!(tables().ansi_bits[b] & CT_UPPER))
        return c;
    return tables().ansi_lower[b];
}

int ctype_toupper(int c) {
    if (c < 0 || c > 255) {
        uint8_t b = (uint8_t)c;
        return (tables().ansi_bits[b] & CT_LOWER) ? tables().ansi_upper[b] : b;
    }
    if (ctype_is_c_locale())
        return (c >= 'a' && c <= 'z') ? c - 32 : c;
    uint8_t b = (uint8_t)c;
    if (!(tables().ansi_bits[b] & CT_LOWER))
        return c;
    return tables().ansi_upper[b];
}

uint8_t mbc_lower(uint8_t b) {
    return (tables().ansi_bits[b] & CT_UPPER) ? tables().ansi_lower[b] : b;
}
uint8_t mbc_upper(uint8_t b) {
    return (tables().ansi_bits[b] & CT_LOWER) ? tables().ansi_upper[b] : b;
}
// _ismbcspace/_ismbcdigit: a single-byte character is classified by the
// locale's ctype table, as isspace/isdigit would.
bool mbc_is_space(uint32_t c) {
    return c < 256 && (ctype_bits((int)c) & CT_SPACE);
}
bool mbc_is_digit(uint32_t c) {
    return c < 256 && (ctype_bits((int)c) & CT_DIGIT);
}

char numeric_decimal_point() {
    return kLocales[state().cat[CAT_NUMERIC]].decimal;
}

int mb_to_wide(uint8_t b) {
    return ctype_is_c_locale() ? b : cp1252_to_unicode(b);
}
int wide_to_mb(uint32_t w) {
    if (ctype_is_c_locale())
        return w < 256 ? (int)w : -1;
    return unicode_to_cp1252(w);
}

uint32_t wide_toupper(uint32_t w) {
    return case_map(kUpperRuns, std::size(kUpperRuns), w);
}
uint32_t wide_tolower(uint32_t w) {
    return case_map(kLowerRuns, std::size(kLowerRuns), w);
}
uint16_t wide_ctype_bits(uint32_t w) {
    return unicode_bits(w);
}

void locale_reset() {
    LocaleState &st = state();
    st = LocaleState();
}

uint32_t localeconv_addr() {
    LocaleState &st = state();
    name_buffer(CAT_ALL); // revalidates the guest buffers after a heap reset
    if (!st.lconv) {
        st.lconv = mem_alloc(48);
        st.lconv_strings = mem_alloc(256);
        if (!st.lconv || !st.lconv_strings)
            return 0;
        memset(g_mem + st.lconv, 0, 48);
    }
    int key = st.cat[CAT_NUMERIC] * 16 + st.cat[CAT_MONETARY];
    if (st.lconv_for == key)
        return st.lconv;
    st.lconv_for = key;
    const LocaleDef &num = kLocales[st.cat[CAT_NUMERIC]];
    const LocaleDef &mon = kLocales[st.cat[CAT_MONETARY]];
    bool c_mon = st.cat[CAT_MONETARY] == 0;
    uint32_t at = st.lconv_strings;
    auto put = [&](uint32_t field, const std::string &s) {
        wr32(st.lconv + field, at);
        at += gm_put_str(at, s.c_str(), 32) + 1;
    };
    put(0, std::string(1, num.decimal));
    put(4, num.thousands ? std::string(1, num.thousands) : std::string());
    put(8, num.thousands ? std::string("\x03") : std::string());
    put(12, mon.int_currency);
    put(16, mon.currency);
    put(20, c_mon ? std::string() : std::string(1, mon.decimal));
    put(24, mon.thousands ? std::string(1, mon.thousands) : std::string());
    put(28, mon.thousands ? std::string("\x03") : std::string());
    put(32, "");
    put(36, c_mon ? std::string() : std::string("-"));
    // CHAR_MAX (127) marks "not available" in the C locale.
    const uint8_t *f = mon.mon;
    for (int i = 0; i < 8; ++i)
        wr8(st.lconv + 40 + i, f[i]);
    return st.lconv;
}

void register_locale() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data_init("msvcrt.dll", "__mb_cur_max", 4, init_mb_cur_max);
}

} // namespace crt
