// TrueType faces for GDI text: the program's own (AddFontMemResourceEx), the
// platform's installed copies of Windows faces (gdi32_fontdb.cpp), and the
// kit's bundled Open Sans. stb_truetype reads and rasterizes them; the
// measuring is GDI's, from the font's own Windows tables (see the header).
#include "gdi32_truetype.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace gdi {
extern const unsigned char bundled_sans_regular[];
extern const size_t bundled_sans_regular_size;
extern const unsigned char bundled_sans_semibold[];
extern const size_t bundled_sans_semibold_size;
struct TrueTypeFace {
    std::shared_ptr<std::vector<uint8_t>> data; // null for the bundled faces
    size_t size = 0;                            // bytes readable at info.data
    stbtt_fontinfo info{};
    std::string family;  // lower case
    std::string display; // as the font spells it
    // Table offsets from the start of the file, 0 when absent or too short.
    uint32_t head = 0, hhea = 0, os2 = 0, os2_size = 0, post = 0, hdmx = 0, vdmx = 0;
};
namespace {
std::vector<std::unique_ptr<TrueTypeFace>> &faces() {
    static std::vector<std::unique_ptr<TrueTypeFace>> list;
    return list;
}
std::string lower_ascii(std::string s) {
    for (char &ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return s;
}
// The family name (name ID 1), from the Windows Unicode record the way GDI
// matches it, else the Macintosh Roman one.
std::string family_of(const stbtt_fontinfo &info) {
    int length = 0;
    if (const char *utf16 =
            stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                    STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1)) {
        std::string name;
        for (int i = 0; i + 1 < length; i += 2) {
            uint16_t unit = uint16_t((uint8_t(utf16[i]) << 8) | uint8_t(utf16[i + 1]));
            name.push_back(unit < 128 ? char(unit) : '?');
        }
        return name;
    }
    if (const char *roman = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC,
                                                    STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, 1))
        return std::string(roman, size_t(length));
    return {};
}

uint16_t u16(const TrueTypeFace *f, uint32_t at) {
    return at + 2 <= f->size ? uint16_t((f->info.data[at] << 8) | f->info.data[at + 1]) : 0;
}
int16_t s16(const TrueTypeFace *f, uint32_t at) {
    return int16_t(u16(f, at));
}
uint32_t u32(const TrueTypeFace *f, uint32_t at) {
    return (uint32_t(u16(f, at)) << 16) | u16(f, at + 2);
}

// A table's offset, when its whole minimum length lies inside the file.
uint32_t table(const TrueTypeFace *f, const char *tag, uint32_t min_size,
               uint32_t *size = nullptr) {
    const uint32_t at = stbtt__find_table(f->info.data, uint32_t(f->info.fontstart), tag);
    if (!at)
        return 0;
    // The directory entry's length sits 4 bytes after its offset field; find
    // it again to bound the table.
    const uint32_t start = uint32_t(f->info.fontstart);
    const uint32_t count = u16(f, start + 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = start + 12 + 16 * i;
        if (u32(f, entry + 8) != at)
            continue;
        const uint32_t length = u32(f, entry + 12);
        if (length < min_size || uint64_t(at) + length > f->size)
            return 0;
        if (size)
            *size = length;
        return at;
    }
    return 0;
}

bool init_face(TrueTypeFace *f, int offset) {
    if (!stbtt_InitFont(&f->info, f->info.data, offset))
        return false;
    f->display = family_of(f->info);
    f->family = lower_ascii(f->display);
    f->head = table(f, "head", 54);
    f->hhea = table(f, "hhea", 36);
    f->os2 = table(f, "OS/2", 78, &f->os2_size);
    f->post = table(f, "post", 16);
    f->hdmx = table(f, "hdmx", 8);
    f->vdmx = table(f, "VDMX", 6);
    return f->head && !f->family.empty();
}
} // namespace

uint32_t truetype_add_memory(const uint8_t *data, size_t size) {
    if (!data || size < 12 || size > (64u << 20))
        return 0;
    auto copy = std::make_shared<std::vector<uint8_t>>(data, data + size);
    int count = stbtt_GetNumberOfFonts(copy->data());
    uint32_t added = 0;
    for (int i = 0; i < count; ++i) {
        int offset = stbtt_GetFontOffsetForIndex(copy->data(), i);
        if (offset < 0 || size_t(offset) >= size)
            continue;
        auto face = std::make_unique<TrueTypeFace>();
        face->data = copy;
        face->size = size;
        face->info.data = copy->data();
        if (!init_face(face.get(), offset))
            continue;
        faces().push_back(std::move(face));
        ++added;
    }
    return added;
}

const TrueTypeFace *truetype_find(const std::string &family) {
    const std::string key = lower_ascii(family);
    for (auto it = faces().rbegin(); it != faces().rend(); ++it) // the latest registration wins
        if ((*it)->family == key)
            return it->get();
    return nullptr;
}

const TrueTypeFace *truetype_adopt_file(std::vector<uint8_t> &&bytes, int index) {
    static std::vector<std::unique_ptr<TrueTypeFace>> adopted;
    auto copy = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    if (copy->size() < 12)
        return nullptr;
    const int offset = stbtt_GetFontOffsetForIndex(copy->data(), index);
    if (offset < 0 || size_t(offset) >= copy->size())
        return nullptr;
    auto face = std::make_unique<TrueTypeFace>();
    face->data = copy;
    face->size = copy->size();
    face->info.data = copy->data();
    if (!init_face(face.get(), offset))
        return nullptr;
    adopted.push_back(std::move(face));
    return adopted.back().get();
}

namespace {
// A bundled face, read once from the embedded bytes. The data is the kit's
// own and lives as long as the process, so it is not copied.
std::unique_ptr<TrueTypeFace> bundled_face(const unsigned char *data, size_t size) {
    auto face = std::make_unique<TrueTypeFace>();
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    face->info.data = const_cast<unsigned char *>(data);
    face->size = size;
    if (offset < 0 || size_t(offset) >= size || !init_face(face.get(), offset))
        return nullptr;
    return face;
}
} // namespace

const TrueTypeFace *truetype_windows_substitute(const std::string &family, int32_t weight) {
    // The sans-serif interface faces a Windows 7 installation has, and the
    // names that resolve to them: the dialog aliases, the raster MS Sans
    // Serif, and Helvetica through FontSubstitutes. Serif, fixed-pitch and
    // the system raster fonts (System, Fixedsys, Terminal) are not here -
    // the 8x16 cells are the closer stand-in for those.
    static const char *const windows_sans[] = {
        "segoe ui",      "tahoma",       "microsoft sans serif",
        "ms sans serif", "ms shell dlg", "ms shell dlg 2",
        "arial",         "verdana",      "calibri",
        "trebuchet ms",  "helvetica",
    };
    const std::string key = lower_ascii(family);
    if (std::find(std::begin(windows_sans), std::end(windows_sans), key) == std::end(windows_sans))
        return nullptr;
    static const std::unique_ptr<TrueTypeFace> regular =
        bundled_face(bundled_sans_regular, bundled_sans_regular_size);
    static const std::unique_ptr<TrueTypeFace> semibold =
        bundled_face(bundled_sans_semibold, bundled_sans_semibold_size);
    const TrueTypeFace *face = weight >= 600 && semibold ? semibold.get() : regular.get();
    return face;
}

const std::string &truetype_family_name(const TrueTypeFace *face) {
    static const std::string none;
    return face ? face->display : none;
}

// ---------------------------------------------------------------------------
// Realization
// ---------------------------------------------------------------------------
namespace {
// FreeType's fixed-point helpers, which GDI-compatible rasterizers (FreeType
// itself under Wine) use to scale font units: a 16.16 scale, 26.6 results,
// rounded half away from zero.
int64_t div_fix(int64_t a, int64_t b) {
    const bool neg = (a < 0) != (b < 0);
    a = std::llabs(a);
    b = std::llabs(b);
    const int64_t q = b ? ((a << 16) + (b >> 1)) / b : 0x7fffffff;
    return neg ? -q : q;
}
int64_t mul_fix(int64_t a, int64_t b) {
    const bool neg = (a < 0) != (b < 0);
    const int64_t c = (std::llabs(a) * std::llabs(b) + 0x8000) >> 16;
    return neg ? -c : c;
}
// Font units to whole pixels, as (26.6 + 32) >> 6.
int32_t to_pixels(int64_t units, int64_t scale) {
    const int64_t v = mul_fix(units, scale) + 32;
    return int32_t(v >= 0 ? v >> 6 : -((-v + 63) >> 6));
}
// Windows' MulDiv: a * b / c rounded half away from zero.
int32_t mul_div(int64_t a, int64_t b, int64_t c) {
    if (!c)
        return -1;
    const int64_t n = a * b;
    const bool neg = (n < 0) != (c < 0);
    const int64_t q = (std::llabs(n) + std::llabs(c) / 2) / std::llabs(c);
    return int32_t(neg ? -q : q);
}

// The VDMX record GDI uses for a 1:1 device: for a negative LOGFONT height
// the record of that ppem; for a positive (cell) height the largest ppem whose
// yMax - yMin does not exceed it. False when the font has no such record.
// Only a group for the Windows ANSI subset (bCharSet 1) counts: Windows 11
// measures Tahoma, whose only group is for all glyphs (bCharSet 0), from its
// Windows ascent and descent alone, and Arial, whose groups are ANSI, by them
// (tools/gdi_metrics: 164 of 164 heights, ascents and descents agree).
bool vdmx_lookup(const TrueTypeFace *f, int32_t lf_height, int32_t *ppem, int32_t *y_max,
                 int32_t *y_min) {
    if (!f->vdmx)
        return false;
    const uint32_t base = f->vdmx;
    const uint16_t ratios = u16(f, base + 4);
    uint32_t group = 0;
    for (uint16_t i = 0; i < ratios; ++i) {
        const uint32_t r = base + 6 + 4 * i;
        const uint8_t charset = f->info.data[r], x = f->info.data[r + 1], y0 = f->info.data[r + 2],
                      y1 = f->info.data[r + 3];
        if (charset != 1)
            continue;
        // The ratio a 96x96-dpi device has, x:y = 1:1, or the catch-all 0:0:0.
        if ((x == 0 && y0 == 0 && y1 == 0) || (y0 <= x && y1 >= x)) {
            group = base + u16(f, base + 6 + 4 * ratios + 2 * i);
            break;
        }
    }
    if (!group || group + 4 > f->size)
        return false;
    const uint16_t recs = u16(f, group);
    const uint8_t start = f->info.data[group + 2], end = f->info.data[group + 3];
    if (group + 4 + 6ull * recs > f->size)
        return false;
    auto at = [&](uint16_t i, uint16_t *pel, int16_t *hi, int16_t *lo) {
        const uint32_t e = group + 4 + 6 * i;
        *pel = u16(f, e);
        *hi = s16(f, e + 2);
        *lo = s16(f, e + 4);
    };
    if (lf_height > 0) {
        for (uint16_t i = 0; i < recs; ++i) {
            uint16_t pel;
            int16_t hi, lo;
            at(i, &pel, &hi, &lo);
            if (hi - lo == lf_height) {
                *ppem = pel, *y_max = hi, *y_min = lo;
                return true;
            }
            if (hi - lo > lf_height) {
                if (i == 0)
                    return false;
                at(uint16_t(i - 1), &pel, &hi, &lo);
                *ppem = pel, *y_max = hi, *y_min = lo;
                return true;
            }
        }
        return false;
    }
    const int32_t want = -lf_height;
    if (want < start || want > end)
        return false;
    for (uint16_t i = 0; i < recs; ++i) {
        uint16_t pel;
        int16_t hi, lo;
        at(i, &pel, &hi, &lo);
        if (pel > want)
            return false;
        if (pel == want) {
            *ppem = pel, *y_max = hi, *y_min = lo;
            return true;
        }
    }
    return false;
}

// The hdmx device widths for this ppem, or null.
const uint8_t *hdmx_record(const TrueTypeFace *f, int32_t ppem) {
    if (!f->hdmx || ppem <= 0 || ppem > 255)
        return nullptr;
    const int16_t count = s16(f, f->hdmx + 2);
    const int32_t record = int32_t(u32(f, f->hdmx + 4));
    if (count <= 0 || record < 2 + f->info.numGlyphs)
        return nullptr;
    for (int16_t i = 0; i < count; ++i) {
        const uint64_t at = uint64_t(f->hdmx) + 8 + uint64_t(i) * uint64_t(record);
        if (at + uint64_t(record) > f->size)
            return nullptr;
        if (f->info.data[at] == ppem)
            return f->info.data + at + 2;
    }
    return nullptr;
}

// The y scale GDI's rasterizer uses for a ppem, as FreeType's
// FT_Set_Pixel_Sizes computes it.
int64_t y_scale_of(const TrueTypeFace *f, int32_t ppem) {
    return div_fix(int64_t(ppem) << 6, std::max<int64_t>(1, u16(f, f->head + 18)));
}
} // namespace

GdiFont truetype_realize(const TrueTypeFace *f, const GdiLogFont &lf, bool synth_bold,
                         bool synth_italic) {
    GdiFont out;
    out.face = f;
    if (!f)
        return out;
    const int32_t upem = std::max<int32_t>(1, u16(f, f->head + 18));
    // The Windows ascent and descent, else the horizontal header's.
    int32_t win_ascent = f->os2 ? u16(f, f->os2 + 74) : 0;
    int32_t win_descent = f->os2 ? u16(f, f->os2 + 76) : 0;
    const int32_t hhea_ascent = f->hhea ? s16(f, f->hhea + 4) : upem * 4 / 5;
    const int32_t hhea_descent = f->hhea ? s16(f, f->hhea + 6) : -upem / 5;
    const int32_t line_gap = f->hhea ? s16(f, f->hhea + 8) : 0;
    if (win_ascent + win_descent == 0) {
        win_ascent = hhea_ascent;
        win_descent = -hhea_descent;
    }
    // The em in whole pixels: |height| for a negative height; for a positive
    // one the cell height spans the Windows ascent and descent. A height of 0
    // is GDI's default, a 16-pixel cell. A VDMX record, when the font has one
    // for the size, decides both the em and the ascent and descent.
    const int32_t height = lf.height ? lf.height : 16;
    int32_t ppem = 0, y_max = 0, y_min = 0;
    const bool vdmx = vdmx_lookup(f, height, &ppem, &y_max, &y_min);
    // A cell height with no VDMX record: the em part of the cell, rounded.
    // (Windows 11 agrees for 18 of 21 Tahoma cell heights; at 20, 26 and 32 it
    // takes one pixel less, by a rule not yet known.)
    if (!vdmx)
        ppem = height < 0 ? -height : mul_div(upem, height, win_ascent + win_descent);
    ppem = std::max<int32_t>(1, ppem);
    out.ppem = ppem;
    out.sy = double(ppem) / upem;
    const int64_t ys = y_scale_of(f, ppem);
    const int32_t avg_units = f->os2 ? s16(f, f->os2 + 2) : 0;
    // A LOGFONT width stretches the face so the average width comes out as
    // asked; 0 keeps it square.
    int64_t xs = ys;
    out.sx = out.sy;
    if (lf.width > 0 && avg_units > 0) {
        xs = div_fix(int64_t(lf.width) << 6, avg_units);
        out.sx = double(lf.width) / avg_units;
    }
    out.device_widths = xs == ys && hdmx_record(f, ppem) != nullptr;

    if (vdmx) {
        out.ascent = y_max;
        out.descent = -y_min;
        out.internal = out.ascent + out.descent - ppem;
    } else if (height > 0) {
        // A cell height is the TEXTMETRIC height, split by the Windows ascent.
        out.ascent = mul_div(win_ascent, height, win_ascent + win_descent);
        out.descent = height - out.ascent;
        out.internal = height - ppem;
    } else {
        out.ascent = to_pixels(win_ascent, ys);
        out.descent = to_pixels(win_descent, ys);
        out.internal = to_pixels(win_ascent + win_descent - upem, ys);
    }
    out.height = out.ascent + out.descent;
    out.external = std::max<int32_t>(
        0, to_pixels(line_gap - ((win_ascent + win_descent) - (hhea_ascent - hhea_descent)), ys));
    out.ave_width = std::max<int32_t>(1, to_pixels(avg_units, xs));
    out.max_width = to_pixels(s16(f, f->head + 40) - s16(f, f->head + 36), xs);

    const bool face_italic =
        (u16(f, f->head + 44) & 2) != 0 || (f->os2 && (u16(f, f->os2 + 62) & 1) != 0);
    out.bold_sim = synth_bold;
    out.italic_sim = synth_italic;
    const int32_t face_weight = f->os2 ? u16(f, f->os2 + 4) : 400;
    out.weight = synth_bold ? 700 : (face_weight ? face_weight : 400);
    if (synth_bold) {
        out.ave_width += 1;
        out.max_width += 1;
    }
    out.italic = (face_italic || synth_italic) ? 255 : 0;
    out.underlined = lf.underline ? 255 : 0;
    out.struck = lf.strikeout ? 255 : 0;
    out.bilevel = lf.quality == 3; // NONANTIALIASED_QUALITY

    // TEXTMETRICW's character range: the font's own, the C0 control before
    // the space as the default character, and the space as the break.
    out.first = f->os2 ? u16(f, f->os2 + 64) : 0x20;
    out.last = f->os2 ? u16(f, f->os2 + 66) : 0xff;
    out.def = 0x1f;
    out.brk = 0x20;
    out.charset = (lf.charset == 1 /* DEFAULT_CHARSET */) ? 0 : lf.charset;

    // TMPF_FIXED_PITCH is set for a VARIABLE pitch font (a Windows oddity),
    // TMPF_VECTOR and TMPF_TRUETYPE for every outline font; the family comes
    // from the PANOSE classification, as GDI derives it.
    const bool fixed = f->post && u32(f, f->post + 12) != 0;
    uint8_t pitch = uint8_t((fixed ? 0 : 1) | 2 | 4);
    uint8_t family = 0x10; // FF_ROMAN
    if (f->os2) {
        const uint8_t type = f->info.data[f->os2 + 32], serif = f->info.data[f->os2 + 33];
        if (type == 3)
            family = 0x40; // FF_SCRIPT
        else if (type == 4)
            family = 0x50; // FF_DECORATIVE
        else if (fixed)
            family = 0x30; // FF_MODERN
        else if (serif >= 11 && serif <= 13)
            family = 0x20; // FF_SWISS: normal, obtuse and perpendicular sans
        else if (serif <= 1) {
            const uint8_t cls = uint8_t(u16(f, f->os2 + 30) >> 8);
            family = cls == 8 ? 0x20 : cls == 10 ? 0x40 : cls == 12 ? 0x50 : 0x10;
        }
    } else if (fixed)
        family = 0x30;
    out.pitch_family = uint8_t(pitch | family);

    // Underline from the post table, strikeout from OS/2, both in pixels.
    const int32_t ul_pos = f->post ? s16(f, f->post + 8) : -upem / 10;
    const int32_t ul_size = f->post ? s16(f, f->post + 10) : upem / 20;
    out.underline_pos = std::max<int32_t>(1, -to_pixels(ul_pos, ys));
    out.underline_size = std::max<int32_t>(1, to_pixels(ul_size, ys));
    const int32_t st_size = f->os2 ? s16(f, f->os2 + 26) : upem / 20;
    const int32_t st_pos = f->os2 ? s16(f, f->os2 + 28) : upem / 4;
    out.strike_size = std::max<int32_t>(1, to_pixels(st_size, ys));
    out.strike_pos = -to_pixels(st_pos, ys);
    return out;
}

int32_t truetype_gdi_advance(const GdiFont &font, uint32_t codepoint) {
    const TrueTypeFace *f = font.face;
    if (!f)
        return 0;
    const int glyph = stbtt_FindGlyphIndex(&f->info, int(codepoint));
    int32_t advance = 0;
    const uint8_t *widths = font.device_widths ? hdmx_record(f, font.ppem) : nullptr;
    if (widths && glyph >= 0 && glyph < f->info.numGlyphs)
        advance = widths[glyph];
    else {
        int units = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&f->info, glyph, &units, &bearing);
        // The same 16.16 scale realization used; a stretched font's x scale.
        const int64_t xs = font.sx == font.sy ? y_scale_of(f, font.ppem)
                                              : int64_t(std::llround(font.sx * 65536.0 * 64.0));
        advance = to_pixels(units, xs);
    }
    return advance + (font.bold_sim ? 1 : 0);
}

const TrueTypeGlyph &truetype_gdi_glyph(const GdiFont &font, uint32_t codepoint) {
    using Key = std::tuple<const TrueTypeFace *, double, double, uint32_t, int>;
    static std::map<Key, TrueTypeGlyph> cache;
    static const TrueTypeGlyph empty;
    const TrueTypeFace *f = font.face;
    if (!f)
        return empty;
    const int flags = (font.bold_sim ? 1 : 0) | (font.italic_sim ? 2 : 0) | (font.bilevel ? 4 : 0);
    const Key key{f, font.sx, font.sy, codepoint, flags};
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    if (cache.size() > 8192) // atlases and captions repeat; a bound, not an eviction policy
        cache.clear();
    TrueTypeGlyph glyph;
    const int index = stbtt_FindGlyphIndex(&f->info, int(codepoint));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&f->info, index, float(font.sx), float(font.sy), &x0, &y0, &x1, &y1);
    if (x1 > x0 && y1 > y0 && int64_t(x1 - x0) * (y1 - y0) <= (1 << 22)) {
        const int w = x1 - x0, h = y1 - y0;
        std::vector<uint8_t> cover(size_t(w) * size_t(h), 0);
        stbtt_MakeGlyphBitmap(&f->info, cover.data(), w, h, w, float(font.sx), float(font.sy),
                              index);
        // A simulated italic leans the glyph by GDI's 1:3 or so (about 18
        // degrees): each row shifts right by its height above the baseline.
        const int lean = font.italic_sim ? (h + 2) / 3 : 0;
        // A simulated bold is the glyph and itself one pixel to the right.
        const int extra = (font.bold_sim ? 1 : 0) + lean;
        glyph.w = w + extra;
        glyph.h = h;
        glyph.x = x0;
        glyph.y = y0;
        glyph.coverage.assign(size_t(glyph.w) * size_t(h), 0);
        for (int j = 0; j < h; ++j) {
            const int shift = lean ? lean * (h - 1 - j) / std::max(1, h - 1) : 0;
            for (int i = 0; i < w; ++i) {
                const uint8_t c = cover[size_t(j) * size_t(w) + size_t(i)];
                if (!c)
                    continue;
                uint8_t &a = glyph.coverage[size_t(j) * size_t(glyph.w) + size_t(i + shift)];
                a = std::max(a, c);
                if (font.bold_sim) {
                    uint8_t &b =
                        glyph.coverage[size_t(j) * size_t(glyph.w) + size_t(i + shift + 1)];
                    b = std::max(b, c);
                }
            }
        }
        if (font.bilevel)
            for (uint8_t &c : glyph.coverage)
                c = c >= 128 ? 255 : 0;
    }
    return cache.emplace(key, std::move(glyph)).first->second;
}
} // namespace gdi
