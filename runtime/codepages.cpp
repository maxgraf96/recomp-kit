// codepages.cpp - MultiByteToWideChar/WideCharToMultiByte rules on host
// buffers (codepages.h). Every rule here is one tools/codepages/vectors.txt
// measured on Windows 11; crt_tests replays the whole capture.
#include "codepages.h"
#include "codepage_tables.h"
#include "guest.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <vector>

namespace codepages {
namespace {

using codepage_tables::Sbcs;

const uint32_t kErrInvalidParameter = 87;
const uint32_t kErrInsufficientBuffer = 122;
const uint32_t kErrInvalidFlags = 1004;
const uint32_t kErrNoUnicodeTranslation = 1113;

const uint32_t kMbPrecomposed = 0x1, kMbComposite = 0x2, kMbUseGlyphChars = 0x4,
               kMbErrInvalidChars = 0x8;
const uint32_t kWcDiscardNs = 0x10, kWcSepChars = 0x20, kWcDefaultChar = 0x40,
               kWcErrInvalidChars = 0x80, kWcCompositeCheck = 0x200, kWcNoBestFitChars = 0x400;
const uint32_t kMbKnown = kMbPrecomposed | kMbComposite | kMbUseGlyphChars | kMbErrInvalidChars;
const uint32_t kWcKnown = kWcDiscardNs | kWcSepChars | kWcDefaultChar | kWcErrInvalidChars |
                          kWcCompositeCheck | kWcNoBestFitChars;

const uint32_t kSymbol = 42, kUtf7 = 65000, kUtf8 = 65001;

// The exact UTF-16 -> byte map of a table: the inverse of its byte table,
// the lowest byte winning (the capture checks that this is Windows' rule).
struct Inverse {
    std::vector<std::pair<uint16_t, uint8_t>> pairs; // sorted by unit
};

const Inverse &inverse_of(const Sbcs &t) {
    static std::mutex lock;
    static Inverse built[std::size(codepage_tables::kSbcs)];
    static bool done[std::size(codepage_tables::kSbcs)];
    size_t index = size_t(&t - codepage_tables::kSbcs);
    std::lock_guard<std::mutex> guard(lock);
    if (!done[index]) {
        Inverse &inv = built[index];
        for (int b = 255; b >= 0; --b)
            inv.pairs.push_back({t.to_unicode[b], uint8_t(b)});
        // Stable sort keeps the reverse byte order per unit; the unique pass
        // then keeps the lowest byte.
        std::stable_sort(inv.pairs.begin(), inv.pairs.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<std::pair<uint16_t, uint8_t>> lowest;
        for (const auto &p : inv.pairs) {
            if (!lowest.empty() && lowest.back().first == p.first)
                lowest.back() = p;
            else
                lowest.push_back(p);
        }
        inv.pairs = std::move(lowest);
        done[index] = true;
    }
    return built[index];
}

const Sbcs *table_for(uint32_t cp) {
    for (const Sbcs &t : codepage_tables::kSbcs)
        if (t.cp == cp)
            return &t;
    return nullptr;
}

// Code pages an English Windows has installed (the Code Page Identifiers
// list), which this runtime has no table for: converted as 1252.
bool windows_has(uint32_t cp) {
    static const uint32_t single[] = {
        37,    500,   708,   720,   737,   775,   855,   857,   858,   860,   861,   862,   863,
        864,   865,   866,   869,   870,   874,   875,   932,   936,   949,   950,   1026,  1047,
        1361,  20000, 20001, 20002, 20003, 20004, 20005, 20105, 20106, 20107, 20108, 20261, 20269,
        20273, 20277, 20278, 20280, 20284, 20285, 20290, 20297, 20420, 20423, 20424, 20833, 20838,
        20866, 20871, 20880, 20905, 20924, 20932, 20936, 20949, 21025, 21866, 29001, 38598, 50220,
        50221, 50222, 50225, 50227, 51932, 51936, 51949, 52936, 54936};
    if (std::find(std::begin(single), std::end(single), cp) != std::end(single))
        return true;
    return (cp >= 1140 && cp <= 1149) || (cp >= 1250 && cp <= 1258) ||
           (cp >= 10001 && cp <= 10082) || (cp >= 28592 && cp <= 28606) ||
           (cp >= 57002 && cp <= 57011);
}

void log_substitute(uint32_t cp) {
    static std::mutex lock;
    static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (seen.insert(cp).second)
        LOGW("codepages: no table for code page %u; converting it as 1252", cp);
}

// The table a conversion uses, or null for UTF-8, CP_SYMBOL and refused code
// pages (`*refused` then says which).
const Sbcs *sbcs_for(uint32_t cp, bool *refused) {
    *refused = false;
    if (const Sbcs *t = table_for(cp))
        return t;
    if (cp == kUtf8 || cp == kSymbol)
        return nullptr;
    if (windows_has(cp)) {
        log_substitute(cp);
        return table_for(kAnsiCodePage);
    }
    if (cp == kUtf7)
        LOGW("codepages: UTF-7 is not converted");
    *refused = true;
    return nullptr;
}

int find_pair(const std::pair<uint16_t, uint8_t> *begin, size_t n, uint16_t unit) {
    const auto *end = begin + n;
    const auto *it =
        std::lower_bound(begin, end, unit, [](const std::pair<uint16_t, uint8_t> &p, uint16_t u) {
            return p.first < u;
        });
    return (it != end && it->first == unit) ? it->second : -1;
}

int find_best_fit(const Sbcs &t, uint16_t unit) {
    const auto *begin = t.best_fit, *end = t.best_fit + t.best_fit_count;
    const auto *it = std::lower_bound(
        begin, end, unit, [](const codepage_tables::Pair &p, uint16_t u) { return p.unit < u; });
    return (it != end && it->unit == unit) ? it->byte : -1;
}

// The byte for one unit: exact, else best fit unless refused, else -1.
int sbcs_byte(const Sbcs &t, uint16_t unit, bool best_fit) {
    const Inverse &inv = inverse_of(t);
    int b = find_pair(inv.pairs.data(), inv.pairs.size(), unit);
    if (b < 0 && best_fit)
        b = find_best_fit(t, unit);
    return b;
}

uint16_t compose(uint16_t base, uint16_t mark) {
    using codepage_tables::Composition;
    const Composition *begin = codepage_tables::kCompositions;
    const Composition *end = begin + std::size(codepage_tables::kCompositions);
    const Composition *it =
        std::lower_bound(begin, end, std::make_pair(base, mark),
                         [](const Composition &c, const std::pair<uint16_t, uint16_t> &k) {
                             return c.base < k.first || (c.base == k.first && c.mark < k.second);
                         });
    return (it != end && it->base == base && it->mark == mark) ? it->composed : 0;
}

// Nonspacing marks, for WC_DISCARDNS and WC_DEFAULTCHAR: the combining blocks.
bool is_nonspacing(uint16_t u) {
    return (u >= 0x0300 && u < 0x0370) || (u >= 0x0483 && u < 0x048a) ||
           (u >= 0x1ab0 && u < 0x1b00) || (u >= 0x1dc0 && u < 0x1e00) ||
           (u >= 0x20d0 && u < 0x2100) || (u >= 0xfe20 && u < 0xfe30);
}

// Output with Windows' short-buffer rule: units go in while they fit.
template <typename T> struct Sink {
    T *dst;
    size_t cap;
    size_t count = 0;
    bool full = false;
    bool put(T v) {
        if (dst) {
            if (count >= cap) {
                full = true;
                return false;
            }
            dst[count] = v;
        }
        ++count;
        return true;
    }
    bool fits(size_t n) const {
        return !dst || count + n <= cap;
    }
};

Conversion fail(uint32_t error, size_t written = 0) {
    Conversion r;
    r.error = error;
    r.written = written;
    return r;
}

Conversion done(size_t count) {
    Conversion r;
    r.result = int(count);
    r.written = count;
    return r;
}

// ---------------------------------------------------------------------------
// MultiByteToWideChar.
// ---------------------------------------------------------------------------
Conversion sbcs_to_wide(const Sbcs &t, uint32_t flags, const uint8_t *src, size_t n, uint16_t *dst,
                        size_t cap) {
    if ((flags & ~kMbKnown) ||
        (flags & (kMbPrecomposed | kMbComposite)) == (kMbPrecomposed | kMbComposite))
        return fail(kErrInvalidFlags);
    Sink<uint16_t> out{dst, cap};
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = src[i];
        if ((flags & kMbErrInvalidChars) && (t.invalid[b >> 3] & (1u << (b & 7))))
            return fail(kErrNoUnicodeTranslation, out.count);
        if (flags & kMbComposite) {
            const codepage_tables::Decomposition *d = nullptr;
            for (uint16_t k = 0; k < t.decomposition_count; ++k)
                if (t.decompositions[k].byte == b)
                    d = &t.decompositions[k];
            if (d) {
                if (!out.put(d->base))
                    return fail(kErrInsufficientBuffer, out.count);
                // Measured: a mark that no longer fits after the last
                // character's base is dropped, and the call succeeds.
                if (!out.put(d->mark)) {
                    if (i + 1 == n)
                        return done(out.count);
                    return fail(kErrInsufficientBuffer, out.count);
                }
                continue;
            }
        }
        uint16_t u = (flags & kMbUseGlyphChars) && t.glyphs ? t.glyphs[b] : t.to_unicode[b];
        if (!out.put(u))
            return fail(kErrInsufficientBuffer, out.count);
    }
    return done(out.count);
}

// UTF-8 as Windows 10 and later decode it: a malformed sequence gives one
// U+FFFD for its valid prefix, except that a lead byte whose second byte is a
// continuation byte out of its range (overlong, surrogate, past U+10FFFF) is
// replaced together with that byte. C0, C1 and F5..FF are bytes of their own.
Conversion utf8_to_wide(uint32_t flags, const uint8_t *src, size_t n, uint16_t *dst, size_t cap) {
    if (flags & ~kMbKnown)
        return fail(kErrInvalidFlags);
    Sink<uint16_t> out{dst, cap};
    bool bad = false;
    auto put = [&](uint16_t u) { return out.put(u); };
    size_t i = 0;
    while (i < n) {
        uint8_t b = src[i];
        uint32_t need = 0, cp = 0, lo = 0x80, hi = 0xbf;
        if (b < 0x80) {
            if (!put(b))
                return fail(kErrInsufficientBuffer, out.count);
            ++i;
            continue;
        } else if (b >= 0xc2 && b <= 0xdf) {
            need = 1, cp = b & 0x1f;
        } else if (b >= 0xe0 && b <= 0xef) {
            need = 2, cp = b & 0x0f;
            if (b == 0xe0)
                lo = 0xa0;
            if (b == 0xed)
                hi = 0x9f;
        } else if (b >= 0xf0 && b <= 0xf4) {
            need = 3, cp = b & 0x07;
            if (b == 0xf0)
                lo = 0x90;
            if (b == 0xf4)
                hi = 0x8f;
        }
        size_t used = 1;
        bool ok = need > 0;
        for (uint32_t k = 0; ok && k < need; ++k) {
            if (i + used >= n) {
                ok = false;
                break;
            }
            uint8_t c = src[i + used];
            uint32_t min = k == 0 ? lo : 0x80, max = k == 0 ? hi : 0xbf;
            if (c < 0x80 || c > 0xbf) {
                ok = false;
                break;
            }
            if (c < min || c > max) {
                ++used; // the out-of-range second byte goes with its lead
                ok = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3f);
            ++used;
        }
        if (!ok) {
            bad = true;
            if (!put(0xfffd))
                return fail(kErrInsufficientBuffer, out.count);
        } else if (cp >= 0x10000) {
            cp -= 0x10000;
            if (!put(uint16_t(0xd800 | (cp >> 10))) || !put(uint16_t(0xdc00 | (cp & 0x3ff))))
                return fail(kErrInsufficientBuffer, out.count);
        } else if (!put(uint16_t(cp))) {
            return fail(kErrInsufficientBuffer, out.count);
        }
        i += used;
    }
    if (bad && (flags & kMbErrInvalidChars))
        return fail(kErrNoUnicodeTranslation, out.count);
    return done(out.count);
}

Conversion symbol_to_wide(uint32_t flags, const uint8_t *src, size_t n, uint16_t *dst, size_t cap) {
    if (flags)
        return fail(kErrInvalidFlags);
    Sink<uint16_t> out{dst, cap};
    for (size_t i = 0; i < n; ++i)
        if (!out.put(src[i] < 0x20 ? src[i] : uint16_t(0xf000 | src[i])))
            return fail(kErrInsufficientBuffer, out.count);
    return done(out.count);
}

// ---------------------------------------------------------------------------
// WideCharToMultiByte.
// ---------------------------------------------------------------------------
Conversion wide_to_sbcs(const Sbcs &t, uint32_t flags, const uint16_t *src, size_t n, uint8_t *dst,
                        size_t cap, const uint8_t *default_char, bool want_used) {
    const uint32_t needs_check = kWcDiscardNs | kWcSepChars | kWcDefaultChar;
    if ((flags & ~kWcKnown) || (flags & kWcErrInvalidChars) ||
        ((flags & needs_check) && !(flags & kWcCompositeCheck)))
        return fail(kErrInvalidFlags);
    const bool best_fit = !(flags & kWcNoBestFitChars);
    const uint8_t fallback = default_char ? default_char[0] : uint8_t('?');
    Sink<uint8_t> out{dst, cap};
    bool used = false;
    auto finish_short = [&]() {
        Conversion r = fail(kErrInsufficientBuffer, out.count);
        r.used_default = used;
        r.used_default_set = want_used;
        return r;
    };
    // One character's byte; `*defaulted` when it had none.
    auto byte_for = [&](uint16_t u, bool *defaulted) -> uint8_t {
        int b = sbcs_byte(t, u, best_fit);
        *defaulted = b < 0;
        return b < 0 ? fallback : uint8_t(b);
    };
    size_t i = 0;
    while (i < n) {
        uint16_t u = src[i];
        uint8_t b;
        bool defaulted = false;
        size_t step = 1;
        if ((flags & kWcCompositeCheck) && i + 1 < n && is_nonspacing(src[i + 1])) {
            if (uint16_t composed = compose(u, src[i + 1])) {
                b = byte_for(composed, &defaulted);
                step = 2;
            } else if (flags & kWcDiscardNs) {
                b = byte_for(u, &defaulted);
                step = 2;
            } else if (flags & kWcDefaultChar) {
                b = fallback;
                defaulted = true;
                step = 2;
            } else {
                b = byte_for(u, &defaulted); // WC_SEPCHARS: the mark follows on its own
            }
        } else {
            b = byte_for(u, &defaulted);
        }
        if (!out.put(b))
            return finish_short();
        used = used || defaulted;
        i += step;
    }
    Conversion r = done(out.count);
    r.used_default = used;
    r.used_default_set = want_used;
    return r;
}

Conversion wide_to_utf8(uint32_t flags, const uint16_t *src, size_t n, uint8_t *dst, size_t cap,
                        bool want_used) {
    if (flags & ~kWcKnown)
        return fail(kErrInvalidFlags);
    Sink<uint8_t> out{dst, cap};
    bool bad = false;
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = src[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && src[i + 1] >= 0xdc00 &&
            src[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (src[i + 1] - 0xdc00);
            ++i;
        } else if (cp >= 0xd800 && cp < 0xe000) {
            cp = 0xfffd;
            bad = true;
        }
        uint8_t bytes[4];
        size_t k = 0;
        if (cp < 0x80) {
            bytes[k++] = uint8_t(cp);
        } else if (cp < 0x800) {
            bytes[k++] = uint8_t(0xc0 | (cp >> 6));
            bytes[k++] = uint8_t(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            bytes[k++] = uint8_t(0xe0 | (cp >> 12));
            bytes[k++] = uint8_t(0x80 | ((cp >> 6) & 0x3f));
            bytes[k++] = uint8_t(0x80 | (cp & 0x3f));
        } else {
            bytes[k++] = uint8_t(0xf0 | (cp >> 18));
            bytes[k++] = uint8_t(0x80 | ((cp >> 12) & 0x3f));
            bytes[k++] = uint8_t(0x80 | ((cp >> 6) & 0x3f));
            bytes[k++] = uint8_t(0x80 | (cp & 0x3f));
        }
        // A character's bytes go in whole or not at all.
        if (!out.fits(k))
            return fail(kErrInsufficientBuffer, out.count);
        for (size_t j = 0; j < k; ++j)
            out.put(bytes[j]);
    }
    if (bad && (flags & kWcErrInvalidChars))
        return fail(kErrNoUnicodeTranslation, out.count);
    Conversion r = done(out.count);
    r.used_default_set = want_used; // Windows 11 answers FALSE rather than refusing
    return r;
}

Conversion wide_to_symbol(uint32_t flags, const uint16_t *src, size_t n, uint8_t *dst, size_t cap,
                          const uint8_t *default_char, bool want_used) {
    if (default_char || want_used)
        return fail(kErrInvalidParameter);
    if (flags)
        return fail(kErrInvalidFlags);
    Sink<uint8_t> out{dst, cap};
    for (size_t i = 0; i < n; ++i) {
        uint16_t u = src[i];
        int b = u < 0x20 ? u : (u >= 0xf020 && u < 0xf100) ? (u & 0xff) : -1;
        if (b < 0)
            return fail(kErrNoUnicodeTranslation, out.count);
        if (!out.put(uint8_t(b)))
            return fail(kErrInsufficientBuffer, out.count);
    }
    return done(out.count);
}

} // namespace

uint32_t resolve(uint32_t cp) {
    switch (cp) {
    case 0: // CP_ACP
    case 3: // CP_THREAD_ACP
        return kAnsiCodePage;
    case 1: // CP_OEMCP
        return kOemCodePage;
    case 2: // CP_MACCP
        return kMacCodePage;
    default:
        return cp;
    }
}

bool is_valid(uint32_t cp) {
    return table_for(cp) || cp == kUtf8;
}

uint32_t max_char_size(uint32_t cp) {
    cp = resolve(cp);
    if (cp == kUtf8)
        return 4;
    if (cp == kUtf7)
        return 5;
    if (table_for(cp) || windows_has(cp))
        return 1;
    return 0;
}

Conversion mb_to_wide(uint32_t cp, uint32_t flags, const uint8_t *src, size_t n, uint16_t *dst,
                      size_t cap) {
    cp = resolve(cp);
    if (cap == 0)
        dst = nullptr;
    bool refused = false;
    if (const Sbcs *t = sbcs_for(cp, &refused))
        return sbcs_to_wide(*t, flags, src, n, dst, cap);
    if (cp == kUtf8)
        return utf8_to_wide(flags, src, n, dst, cap);
    if (cp == kSymbol)
        return symbol_to_wide(flags, src, n, dst, cap);
    return fail(kErrInvalidParameter);
}

Conversion wide_to_mb(uint32_t cp, uint32_t flags, const uint16_t *src, size_t n, uint8_t *dst,
                      size_t cap, const uint8_t *default_char, bool want_used) {
    cp = resolve(cp);
    if (cap == 0)
        dst = nullptr;
    bool refused = false;
    if (const Sbcs *t = sbcs_for(cp, &refused))
        return wide_to_sbcs(*t, flags, src, n, dst, cap, default_char, want_used);
    if (cp == kUtf8)
        return wide_to_utf8(flags, src, n, dst, cap, want_used);
    if (cp == kSymbol)
        return wide_to_symbol(flags, src, n, dst, cap, default_char, want_used);
    return fail(kErrInvalidParameter);
}

uint16_t ansi_to_unicode(uint8_t b) {
    return table_for(kAnsiCodePage)->to_unicode[b];
}

int unicode_to_ansi(uint32_t unit) {
    if (unit > 0xffff)
        return -1;
    return sbcs_byte(*table_for(kAnsiCodePage), uint16_t(unit), false);
}

std::u16string ansi_to_utf16(const std::string &ansi) {
    std::u16string out(ansi.size(), u'\0');
    for (size_t i = 0; i < ansi.size(); ++i)
        out[i] = char16_t(ansi_to_unicode(uint8_t(ansi[i])));
    return out;
}

std::string utf16_to_ansi(const std::u16string &text) {
    std::string out(text.size(), '\0');
    Conversion r =
        wide_to_mb(kAnsiCodePage, 0, reinterpret_cast<const uint16_t *>(text.data()), text.size(),
                   reinterpret_cast<uint8_t *>(&out[0]), out.size(), nullptr, false);
    out.resize(r.written);
    return out;
}

std::u16string utf8_to_utf16(const std::string &utf8) {
    std::u16string out(utf8.size(), u'\0');
    Conversion r = mb_to_wide(kUtf8, 0, reinterpret_cast<const uint8_t *>(utf8.data()), utf8.size(),
                              reinterpret_cast<uint16_t *>(&out[0]), out.size());
    out.resize(r.written);
    return out;
}

std::string utf16_to_utf8(const std::u16string &text) {
    std::string out(text.size() * 3, '\0');
    Conversion r =
        wide_to_mb(kUtf8, 0, reinterpret_cast<const uint16_t *>(text.data()), text.size(),
                   reinterpret_cast<uint8_t *>(&out[0]), out.size(), nullptr, false);
    out.resize(r.written);
    return out;
}

} // namespace codepages
