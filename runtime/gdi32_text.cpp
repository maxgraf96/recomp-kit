// Text for guest canvases. A font names a face; the face comes from what the
// program registered, the platform's own copy of a Windows face (with
// [runtime] system_fonts), the bundled stand-in for a Windows interface face,
// or fixed 8x16 bitmap cells, in that order (gdi32_truetype.h). TrueType text
// is measured as GDI measures it: TEXTMETRIC, the character widths and text
// extents come from truetype_realize. The ANSI entry points take code page
// 1252. No host font services or guest pointers escape this file; callbacks
// receive temporary guest-heap records.
#include "gdi32_internal.h"
#include <cstdlib>
#include "../platform/os.h"
#include "gdi32_font8x16.h"
#include "gdi32_truetype.h"
#include "memory.h"
#include "msvcrt_internal.h"
#include "win32.h"
#include <algorithm>
#include <cstring>
#include <climits>
#include <map>
using namespace gdi;
namespace {
uint32_t font_word(const Object &font, int offset) {
    uint32_t v;
    memcpy(&v, font.logfont.data() + offset, 4);
    return v;
}
const Object *font_object(uint32_t hdc) {
    auto *dc = dc_of(hdc);
    auto it = objects().find(dc ? dc->font : 0);
    return it == objects().end() || it->second.kind != Object::Font ? nullptr : &it->second;
}
int32_t font_scale(uint32_t dc) {
    const Object *font = font_object(dc);
    int64_t height = font ? int32_t(font_word(*font, 0)) : 16;
    return int32_t(std::max<int64_t>(1, std::abs(height) / 16));
}
std::string face_name(const Object &font) {
    std::string family;
    for (size_t i = 0; i < 32; ++i) {
        uint16_t unit = uint16_t(font.logfont[28 + 2 * i] | (font.logfont[29 + 2 * i] << 8));
        if (!unit)
            break;
        family.push_back(unit < 128 ? char(unit) : '?');
    }
    return family;
}

// LOGFONTW is what every font object holds: the A creators convert into it.
void put_logfont_w(Object &font, int32_t height, int32_t width, int32_t escapement,
                   int32_t orientation, int32_t weight, uint8_t italic, uint8_t underline,
                   uint8_t strikeout, uint8_t charset, uint8_t out_precision,
                   uint8_t clip_precision, uint8_t quality, uint8_t pitch_family,
                   const uint16_t *face, size_t face_units) {
    uint8_t *p = font.logfont.data();
    memset(p, 0, font.logfont.size());
    const int32_t longs[5] = {height, width, escapement, orientation, weight};
    memcpy(p, longs, sizeof longs);
    const uint8_t bytes[8] = {italic,        underline,      strikeout, charset,
                              out_precision, clip_precision, quality,   pitch_family};
    memcpy(p + 20, bytes, sizeof bytes);
    for (size_t i = 0; i < face_units && i < 31; ++i)
        memcpy(p + 28 + 2 * i, &face[i], 2);
}
// The CP1252 face name of a LOGFONTA or CreateFontA argument, as UTF-16.
std::vector<uint16_t> ansi_face(uint32_t p) {
    std::vector<uint16_t> out;
    for (uint32_t i = 0; p && i < 32 && gm_valid(p + i, 1); ++i) {
        const uint8_t b = rd8(p + i);
        if (!b)
            break;
        out.push_back(crt::cp1252_to_unicode(b));
    }
    return out;
}

void create_font(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 92)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 92);
    set_eax(c, make_object(font));
}
// CreateFontIndirectA: LOGFONTA, the same with a 32-byte ANSI face name.
void create_font_indirect_a(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 60)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    const std::vector<uint16_t> face = ansi_face(p + 28);
    put_logfont_w(font, int32_t(rd32(p)), int32_t(rd32(p + 4)), int32_t(rd32(p + 8)),
                  int32_t(rd32(p + 12)), int32_t(rd32(p + 16)), rd8(p + 20), rd8(p + 21),
                  rd8(p + 22), rd8(p + 23), rd8(p + 24), rd8(p + 25), rd8(p + 26), rd8(p + 27),
                  face.data(), face.size());
    set_eax(c, make_object(font));
}
// CreateFontA's fourteen arguments are LOGFONTA's fields in order.
void create_font_a(X86 *c) {
    Object font;
    font.kind = Object::Font;
    const std::vector<uint16_t> face = ansi_face(arg(c, 13));
    put_logfont_w(font, int32_t(arg(c, 0)), int32_t(arg(c, 1)), int32_t(arg(c, 2)),
                  int32_t(arg(c, 3)), int32_t(arg(c, 4)), uint8_t(arg(c, 5)), uint8_t(arg(c, 6)),
                  uint8_t(arg(c, 7)), uint8_t(arg(c, 8)), uint8_t(arg(c, 9)), uint8_t(arg(c, 10)),
                  uint8_t(arg(c, 11)), uint8_t(arg(c, 12)), face.data(), face.size());
    set_eax(c, make_object(font));
}

// The 8x16 cells' TEXTMETRICW. TEXTMETRICW is 60 bytes on x86: eleven
// DWORDs, four UTF-16 characters, five BYTE fields and three bytes of
// trailing padding.
void metrics(uint32_t out, int32_t scale, uint32_t weight) {
    memset(g_mem + out, 0, 60);
    wr32(out, 16u * scale);
    wr32(out + 4, 13u * scale);
    wr32(out + 8, 3u * scale);
    wr32(out + 20, 8u * scale);
    wr32(out + 24, 8u * scale);
    wr32(out + 28, weight);
    wr32(out + 36, 96);
    wr32(out + 40, 96);
    wr16(out + 44, 32);
    wr16(out + 46, 127);
    wr16(out + 48, '?');
    wr16(out + 50, ' ');
    wr8(out + 55, 0x30);
    wr8(out + 56, 0);
}
// All text entry points share these bitmap glyphs and DC colour/clip rules.
void glyph(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline = false) {
    auto *dc = dc_of(hdc);
    if (ch < 32 || ch > 127)
        ch = '?';
    int64_t scale = font_scale(hdc), width = 8 * scale, height = 16 * scale;
    Rect clip = clip_box(hdc);
    uint32_t fg = argb(dc->text_color), bg = argb(dc->bk_color);
    Pixels px(hdc);
    for (int64_t yy = std::max<int64_t>(y, clip.t); yy < std::min<int64_t>(y + height, clip.b);
         ++yy)
        for (int64_t xx = std::max<int64_t>(x, clip.l); xx < std::min<int64_t>(x + width, clip.r);
             ++xx) {
            uint8_t row = recomp_font::font8x16[ch - 32][size_t((yy - y) / scale)];
            if ((row & (0x80 >> ((xx - x) / scale))) || (underline && yy - y >= 14 * scale))
                px.write(xx, yy, fg);
            else if (dc->bk_mode == 2)
                px.write(xx, yy, bg);
        }
}

// How one DC's text is measured and drawn: a realized TrueType face, or the
// bitmap cells.
struct Pen {
    bool tt = false;
    GdiFont font;
    int64_t cells = 1;
    std::string requested; // the face name the font asked for
    int64_t advance(uint32_t ch) const {
        return tt ? truetype_gdi_advance(font, ch) : 8 * cells;
    }
    int64_t height() const {
        return tt ? font.height : 16 * cells;
    }
    int64_t ascent() const {
        return tt ? font.ascent : 13 * cells;
    }
    // One character with its cell's top-left at (x, y). `underline` is the
    // mnemonic underline DrawText asks for; the font's own underline and
    // strikeout are drawn per run (decorate).
    void draw(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline) const {
        if (!tt) {
            glyph(hdc, x, y, ch, underline);
            return;
        }
        auto *dc = dc_of(hdc);
        if (!dc)
            return;
        const uint32_t fg = argb(dc->text_color) & 0xffffffu;
        const int64_t width = advance(ch);
        if (dc->bk_mode == 2)
            fill(hdc, {int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + font.height)},
                 argb(dc->bk_color));
        const TrueTypeGlyph &g = truetype_gdi_glyph(font, ch);
        const int64_t baseline = y + font.ascent;
        Pixels px(hdc);
        for (int32_t j = 0; j < g.h; ++j)
            for (int32_t i = 0; i < g.w; ++i)
                if (uint8_t cover = g.coverage[size_t(j) * size_t(g.w) + size_t(i)])
                    px.write(x + g.x + i, baseline + g.y + j, (uint32_t(cover) << 24) | fg, true);
        if (underline)
            line_under(hdc, x, x + width, baseline + font.underline_pos, font.underline_size, fg);
    }
    void line_under(uint32_t hdc, int64_t x0, int64_t x1, int64_t top, int32_t size,
                    uint32_t fg) const {
        Pixels px(hdc);
        for (int64_t yy = top; yy < top + std::max(1, size); ++yy)
            for (int64_t xx = x0; xx < x1; ++xx)
                px.write(xx, yy, 0xff000000u | fg);
    }
    // The font's underline and strikeout across a run of text.
    void decorate(uint32_t hdc, int64_t x0, int64_t x1, int64_t y) const {
        auto *dc = dc_of(hdc);
        if (!tt || !dc)
            return;
        const uint32_t fg = argb(dc->text_color) & 0xffffffu;
        const int64_t baseline = y + font.ascent;
        if (font.underlined)
            line_under(hdc, x0, x1, baseline + font.underline_pos, font.underline_size, fg);
        if (font.struck)
            line_under(hdc, x0, x1, baseline + font.strike_pos, font.strike_size, fg);
    }
};

// The face a family-less request gets, and a face no installation has: what
// GDI's mapper picks from the pitch and family bits and the character set
// alone.
bool fixed_pitch(uint8_t pitch_family) {
    return (pitch_family & 3) == 1 /* FIXED_PITCH */ ||
           (pitch_family & 0xf0) == 0x30 /* FF_MODERN */;
}
std::string default_face(uint8_t pitch_family, uint8_t charset = 0) {
    const uint8_t family = pitch_family & 0xf0;
    if (charset == 2 /* SYMBOL_CHARSET */)
        return "Symbol";
    if (fixed_pitch(pitch_family))
        return "Courier New";
    if (family == 0x10 /* FF_ROMAN */)
        return "Times New Roman";
    if (family == 0x40 /* FF_SCRIPT */)
        return "Comic Sans MS";
    return "Arial";
}
// The faces every Windows has whose stand-in, when no TrueType copy is
// installed, stays the 8x16 cells: serif, fixed-pitch, symbol and the raster
// system faces. Any other name GDI does not know is an unknown face.
bool known_non_sans_face(const std::string &family) {
    static const char *const known[] = {
        "times new roman", "courier new", "courier",        "georgia",  "palatino linotype",
        "book antiqua",    "garamond",    "lucida console", "consolas", "symbol",
        "wingdings",       "webdings",    "system",         "fixedsys", "terminal",
        "small fonts",     "modern",      "roman",          "script",   "ms serif",
        "tms rmn",         "times",       "marlett",
    };
    std::string key = family;
    for (char &ch : key)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return std::find(std::begin(known), std::end(known), key) != std::end(known);
}

Pen pen_of(uint32_t hdc) {
    Pen pen;
    pen.cells = font_scale(hdc);
    const Object *object = font_object(hdc);
    if (!object)
        return pen;
    // Realizations by font handle and LOGFONT: a game measures with the same
    // few fonts every frame.
    static std::map<uint32_t, std::pair<std::array<uint8_t, 92>, Pen>> cache;
    const uint32_t handle = dc_of(hdc)->font;
    auto hit = cache.find(handle);
    if (hit != cache.end() && hit->second.first == object->logfont)
        return hit->second.second;
    std::string family = face_name(*object);
    pen.requested = family;
    const int32_t height = int32_t(font_word(*object, 0));
    const int32_t weight = int32_t(font_word(*object, 16));
    const bool italic = object->logfont[20] != 0;
    // A stock font (an empty LOGFONT) keeps the bitmap cells.
    const bool stock = family.empty() && height == 0;
    const TrueTypeFace *face = family.empty() ? nullptr : truetype_find(family);
    bool synth_bold = false, synth_italic = false;
    const uint8_t pitch_family = object->logfont[27], charset = object->logfont[23];
    if (!face && !stock && truetype_system_fonts_enabled()) {
        const std::string want = family.empty() ? default_face(pitch_family, charset) : family;
        face = truetype_system_face(want, weight, italic, &synth_bold, &synth_italic);
        if (!face && !family.empty() && charset != 255 /* OEM_CHARSET */) {
            // A face no installation has: GDI's mapper falls back on the face
            // the pitch, family and character set name (Arial for most).
            const std::string fallback = default_face(pitch_family, charset);
            face = truetype_system_face(fallback, weight, italic, &synth_bold, &synth_italic);
            if (face)
                log_once(("gdi-font-mapped:" + family).c_str(),
                         "gdi: no installed face is \"%s\"; GDI's mapper gives it \"%s\"",
                         family.c_str(), truetype_family_name(face).c_str());
        } else if (face)
            log_once(("gdi-system-font:" + want).c_str(),
                     "gdi: \"%s\" is drawn with the installed \"%s\"", want.c_str(),
                     truetype_family_name(face).c_str());
    }
    if (!face && !family.empty()) {
        // A face every Windows has and the program never supplied: what it
        // gets with no font of its own set, from the VCL's default font.
        face = truetype_windows_substitute(family, weight);
        if (face)
            log_once(("gdi-font:" + family).c_str(),
                     "gdi: \"%s\" is a Windows font the program did not register; drawing it "
                     "with the bundled Open Sans",
                     family.c_str());
    }
    if (!face && !family.empty() && !known_non_sans_face(family) && !fixed_pitch(pitch_family) &&
        charset != 255 && charset != 2) {
        // A name nothing knows, with no installed face to map it to: the
        // bundled sans rather than the 8x16 cells, as GDI never draws an
        // unknown name with a raster font.
        face = truetype_windows_substitute("arial", weight);
        if (face)
            log_once(("gdi-font-unknown:" + family).c_str(),
                     "gdi: no face is \"%s\"; drawing it with the bundled Open Sans",
                     family.c_str());
    }
    if (face) {
        GdiLogFont lf;
        lf.height = height;
        lf.width = int32_t(font_word(*object, 4));
        lf.weight = weight;
        lf.italic = italic;
        lf.underline = object->logfont[21] != 0;
        lf.strikeout = object->logfont[22] != 0;
        lf.charset = object->logfont[23];
        lf.quality = object->logfont[26];
        pen.tt = true;
        pen.font = truetype_realize(face, lf, synth_bold, synth_italic);
    }
    cache[handle] = {object->logfont, pen};
    return pen;
}

// TEXTMETRICW (wide) or TEXTMETRICA. The A record narrows the character
// range to bytes as GDI does: the first character is the default one less
// one, the last at most 0xff.
void write_metrics(uint32_t hdc, uint32_t out, bool wide) {
    const Pen pen = pen_of(hdc);
    const Object *object = font_object(hdc);
    uint32_t weight = object ? font_word(*object, 16) : 400;
    if (!weight)
        weight = 400;
    if (!pen.tt) {
        // The cells' record, shared by both forms (its range is ASCII).
        uint32_t tmp = out;
        if (!wide) {
            tmp = heap_alloc(60, true);
            if (!tmp)
                return;
        }
        metrics(tmp, font_scale(hdc), weight);
        if (!wide) {
            memcpy(g_mem + out, g_mem + tmp, 44);
            wr8(out + 44, 32);
            wr8(out + 45, 127);
            wr8(out + 46, '?');
            wr8(out + 47, ' ');
            memcpy(g_mem + out + 48, g_mem + tmp + 52, 5);
            heap_free(tmp);
        }
        return;
    }
    const GdiFont &f = pen.font;
    const uint32_t size = wide ? 60 : 56;
    memset(g_mem + out, 0, size);
    const int32_t longs[11] = {f.height,   f.ascent,    f.descent,   f.internal,
                               f.external, f.ave_width, f.max_width, f.weight,
                               f.overhang, 96,          96};
    for (uint32_t i = 0; i < 11; ++i)
        wr32(out + 4 * i, uint32_t(longs[i]));
    uint32_t at = 44;
    if (wide) {
        wr16(out + 44, f.first);
        wr16(out + 46, f.last);
        wr16(out + 48, f.def);
        wr16(out + 50, f.brk);
        at = 52;
    } else {
        wr8(out + 44, uint8_t(f.def - 1));
        wr8(out + 45, uint8_t(std::min<uint16_t>(f.last, 0xff)));
        wr8(out + 46, uint8_t(f.def));
        wr8(out + 47, uint8_t(f.brk));
        at = 48;
    }
    wr8(out + at, f.italic);
    wr8(out + at + 1, f.underlined);
    wr8(out + at + 2, f.struck);
    wr8(out + at + 3, f.pitch_family);
    wr8(out + at + 4, f.charset);
}
void get_metrics(X86 *c, bool wide) {
    uint32_t out = arg(c, 1);
    if (!dc_of(arg(c, 0)) || !out || !gm_valid(out, wide ? 60 : 56)) {
        set_eax(c, 0);
        return;
    }
    write_metrics(arg(c, 0), out, wide);
    set_eax(c, 1);
}
void get_metrics_w(X86 *c) {
    get_metrics(c, true);
}
void get_metrics_a(X86 *c) {
    get_metrics(c, false);
}

// A guest string as code points: UTF-16 units for the W entry points, code
// page 1252 bytes for the A ones. False when it does not lie in the arena.
bool read_text(uint32_t text, uint32_t n, bool wide, std::vector<uint32_t> *out) {
    out->clear();
    if (n > GUEST_SIZE / 4 || (n && (!text || !gm_valid(text, n * (wide ? 2 : 1)))))
        return false;
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i)
        out->push_back(wide ? rd16(text + 2 * i) : crt::cp1252_to_unicode(rd8(text + i)));
    return true;
}

// GetTextExtentPoint32A/W: the sum of the advances, and the cell height.
void extent(X86 *c, bool wide) {
    uint32_t dc = arg(c, 0), text = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    std::vector<uint32_t> chars;
    if (!dc_of(dc) || !out || !gm_valid(out, 8) || !read_text(text, n, wide, &chars)) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(dc);
    uint64_t width = 0;
    for (uint32_t ch : chars)
        width += uint64_t(pen.advance(ch));
    if (width > INT_MAX) {
        set_eax(c, 0);
        return;
    }
    wr32(out, uint32_t(width));
    wr32(out + 4, uint32_t(pen.height()));
    set_eax(c, 1);
}
void extent_w(X86 *c) {
    extent(c, true);
}
void extent_a(X86 *c) {
    extent(c, false);
}

// GetCharWidth[32]A/W(hdc, first, last, widths): one advance per character
// in [first, last]; the A forms count code page 1252 bytes.
void char_widths(X86 *c, bool wide) {
    const uint32_t hdc = arg(c, 0), first = arg(c, 1), last = arg(c, 2), out = arg(c, 3);
    if (!dc_of(hdc) || last < first || last - first > 0x10000 || !out ||
        !gm_valid(out, (last - first + 1) * 4) || (!wide && last > 0xff)) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(hdc);
    for (uint32_t ch = first; ch <= last; ++ch)
        wr32(out + 4 * (ch - first),
             uint32_t(pen.advance(wide ? ch : crt::cp1252_to_unicode(uint8_t(ch)))));
    set_eax(c, 1);
}
void char_widths_w(X86 *c) {
    char_widths(c, true);
}
void char_widths_a(X86 *c) {
    char_widths(c, false);
}

// The text output every entry point comes down to: ExtTextOut's options
// (ETO_OPAQUE 2, ETO_CLIPPED 4) and rectangle, optional advances, the DC's
// text alignment, its background mode and colours, and the font's underline
// and strikeout. (x, y) is the reference point the alignment names.
bool text_run(uint32_t hdc, int64_t x, int64_t y, uint32_t flags, uint32_t rp,
              const std::vector<uint32_t> &chars, uint32_t dx) {
    auto *dc = dc_of(hdc);
    int w, h;
    if (!dc || !dc_size(hdc, &w, &h) || (dx && !gm_valid(dx, uint32_t(chars.size()) * 4)) ||
        ((flags & 6) && (!rp || !gm_valid(rp, 16))))
        return false;
    const Pen pen = pen_of(hdc);
    std::vector<int64_t> steps(chars.size());
    int64_t total = 0;
    for (size_t i = 0; i < chars.size(); ++i) {
        steps[i] = dx ? int32_t(rd32(dx + 4 * uint32_t(i))) : pen.advance(chars[i]);
        total += steps[i];
    }
    // TA_UPDATECP draws at the current position and moves it on.
    const uint32_t align = dc->text_align;
    if (align & 1) {
        x = dc->pos_x;
        y = dc->pos_y;
    }
    const uint32_t horizontal = align & 6, vertical = align & 24;
    if (horizontal == 6) // TA_CENTER
        x -= total / 2;
    else if (horizontal == 2) // TA_RIGHT
        x -= total;
    if (vertical == 24) // TA_BASELINE
        y -= pen.ascent();
    else if (vertical == 8) // TA_BOTTOM
        y -= pen.height();
    if (align & 1)
        dc->pos_x = int32_t(horizontal == 2 ? x : x + total);

    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    Rect rect{};
    if (flags & 6)
        rect = {int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
                int32_t(rd32(rp + 12))};
    if (flags & 4) {
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    if (flags & 2)
        fill(hdc, rect, argb(dc->bk_color));
    const int64_t left = x;
    for (size_t i = 0; i < chars.size(); ++i) {
        pen.draw(hdc, x, y, chars[i], false);
        x += steps[i];
    }
    pen.decorate(hdc, left, left + total, y);
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return true;
}
// ExtTextOutA/W(hdc, x, y, options, rect, text, count, dx).
void ext_text_out(X86 *c, bool wide) {
    std::vector<uint32_t> chars;
    if (!read_text(arg(c, 5), arg(c, 6), wide, &chars)) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, text_run(arg(c, 0), int32_t(arg(c, 1)), int32_t(arg(c, 2)), arg(c, 3), arg(c, 4),
                        chars, arg(c, 7)));
}
void ext_text_out_w(X86 *c) {
    ext_text_out(c, true);
}
void ext_text_out_a(X86 *c) {
    ext_text_out(c, false);
}
// TextOutA/W(hdc, x, y, text, count).
void text_out(X86 *c, bool wide) {
    std::vector<uint32_t> chars;
    if (!read_text(arg(c, 3), arg(c, 4), wide, &chars)) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, text_run(arg(c, 0), int32_t(arg(c, 1)), int32_t(arg(c, 2)), 0, 0, chars, 0));
}
void text_out_w(X86 *c) {
    text_out(c, true);
}
void text_out_a(X86 *c) {
    text_out(c, false);
}
void set_text_align(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    if (!dc) {
        set_eax(c, 0xffffffffu); // GDI_ERROR
        return;
    }
    const uint32_t old = dc->text_align;
    dc->text_align = arg(c, 1) & 0x1f;
    set_eax(c, old);
}
void get_text_align(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    set_eax(c, dc ? dc->text_align : 0xffffffffu);
}
// GetTextFaceA/W(hdc, count, out): the realized face's name, or the requested
// one for the bitmap cells; the length copied, or the size needed when `out`
// is null.
void text_face(X86 *c, bool wide) {
    const uint32_t hdc = arg(c, 0), cap = arg(c, 1), out = arg(c, 2);
    if (!dc_of(hdc)) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(hdc);
    const std::string name = pen.tt                  ? truetype_family_name(pen.font.face)
                             : pen.requested.empty() ? std::string("System")
                                                     : pen.requested;
    if (!out) {
        set_eax(c, uint32_t(name.size()) + 1);
        return;
    }
    if (!cap || !gm_valid(out, cap * (wide ? 2 : 1))) {
        set_eax(c, 0);
        return;
    }
    const uint32_t n = std::min<uint32_t>(uint32_t(name.size()), cap - 1);
    for (uint32_t i = 0; i < n; ++i) {
        if (wide)
            wr16(out + 2 * i, uint8_t(name[i]));
        else
            wr8(out + i, uint8_t(name[i]));
    }
    if (wide)
        wr16(out + 2 * n, 0);
    else
        wr8(out + n, 0);
    set_eax(c, n);
}
void text_face_w(X86 *c) {
    text_face(c, true);
}
void text_face_a(X86 *c) {
    text_face(c, false);
}
// The fonts are kept, so a DC whose font names one of them draws with it. Data
// the rasterizer cannot read still gets a handle, as it always has here.
void memory_font(X86 *c) {
    uint32_t data = arg(c, 0), size = arg(c, 1), count = arg(c, 3);
    if (count && !gm_valid(count, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t fonts =
        data && size && gm_valid(data, size) ? truetype_add_memory(g_mem + data, size) : 0;
    Object font;
    font.kind = Object::Font;
    uint32_t handle = make_object(font);
    if (count)
        wr32(count, std::max<uint32_t>(fonts, 1));
    set_eax(c, handle);
}
void enumerate(X86 *c) {
    uint32_t cb = arg(c, 2), param = arg(c, 3);
    if (!dc_of(arg(c, 0)) || !cb) {
        set_eax(c, 0);
        return;
    }
    uint32_t data = heap_alloc(92 + 60, true);
    if (!data) {
        set_eax(c, 0);
        return;
    }
    wr32(data, 16);
    wr32(data + 4, 8);
    wr32(data + 16, 400);
    wr8(data + 27, 0x30);
    gm_put_wstr(data + 28, "recomp", 32);
    metrics(data + 92, 1, 400);
    uint32_t result = guest_call(c, cb, data, data + 92, 1, param);
    heap_free(data);
    set_eax(c, result);
}
void unsupported(X86 *c) {
    set_eax(c, 0);
}
} // namespace
namespace gdi {
// DrawText's basic VCL layout uses the same fixed font as ExtTextOutW.
// Measurement must use that font too; CALCRECT never mutates canvas pixels.
uint32_t draw_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t rp, uint32_t flags) {
    auto *dc = dc_of(hdc);
    if (recomp_env("TRACE_GDI")) {
        int dw = -1, dh = -1;
        dc_size(hdc, &dw, &dh);
        LOGW("gdi: DrawText hdc=%08x(%dx%d)%s flags=%08x \"%s\"", hdc, dw, dh,
             dc ? "" : " NO SUCH DC", flags, gm_wstr(text, 64).c_str());
    }
    if (!dc || !rp || !gm_valid(rp, 16) || !text)
        return 0;
    if (count == UINT32_MAX) {
        count = 0;
        while (uint64_t(text) + 2ull * count + 2 <= GUEST_SIZE && gm_valid(text + count * 2, 2) &&
               rd16(text + count * 2))
            ++count;
    }
    if (!count || count > GUEST_SIZE / 2 || !gm_valid(text, count * 2))
        return 0;
    Rect rect{int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
              int32_t(rd32(rp + 12))};
    const Pen pen = pen_of(hdc);
    const int64_t ch = pen.height();
    struct Cell {
        uint16_t ch;
        bool underline;
    };
    std::vector<std::vector<Cell>> lines(1);
    auto width_of = [&](const std::vector<Cell> &line) {
        int64_t width = 0;
        for (Cell cell : line)
            width += pen.advance(cell.ch);
        return width;
    };
    bool prefix = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t unit = rd16(text + 2 * i);
        if (!(flags & 0x800) && unit == '&') { // DT_NOPREFIX
            if (i + 1 < count && rd16(text + 2 * (i + 1)) == '&')
                ++i;
            else {
                prefix = true;
                continue;
            }
        }
        if (unit == '\r' || unit == '\n') {
            if (unit == '\r' && i + 1 < count && rd16(text + 2 * (i + 1)) == '\n')
                ++i;
            if (!(flags & 0x20)) {
                lines.emplace_back();
                prefix = false;
                continue;
            }
            unit = ' ';
        }
        if (unit == '\t' && (flags & 0x40)) {
            size_t spaces = 8 - lines.back().size() % 8;
            lines.back().insert(lines.back().end(), spaces, Cell{' ', false});
        } else
            lines.back().push_back({unit, prefix && !(flags & 0x100000)});
        prefix = false;
    }
    if ((flags & 0x10) && !(flags & 0x20) && rect.r > rect.l) { // DT_WORDBREAK
        const int64_t limit = int64_t(rect.r) - rect.l;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto &line = lines[i];
            size_t cols = 0; // how many characters fit the width
            for (int64_t used = 0; cols < line.size(); ++cols) {
                used += pen.advance(line[cols].ch);
                if (used > limit)
                    break;
            }
            if (!cols || cols >= line.size())
                continue;
            size_t split = std::min(cols, line.size() - 1);
            while (split && line[split].ch != ' ')
                --split;
            if (!split) { // A long word is never split in the middle.
                split = cols;
                while (split < line.size() && line[split].ch != ' ')
                    ++split;
            }
            if (split < line.size()) {
                std::vector<Cell> tail(line.begin() + split + 1, line.end());
                line.resize(split);
                lines.insert(lines.begin() + i + 1, std::move(tail));
            }
        }
    }
    int64_t height = ch * lines.size(), maxwidth = 0;
    for (const auto &line : lines)
        maxwidth = std::max(maxwidth, width_of(line));
    if (height > INT_MAX || maxwidth > INT_MAX)
        return 0;
    if (flags & 0x400) {
        wr32(rp + 8, uint32_t(int64_t(rect.l) + maxwidth));
        wr32(rp + 12, uint32_t(int64_t(rect.t) + height));
        return uint32_t(height);
    }
    int w, h;
    if (!dc_size(hdc, &w, &h))
        return 0;
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    if (!(flags & 0x100)) { // DT_NOCLIP
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    int64_t y = rect.t;
    if (flags & 0x20) {
        if (flags & 4)
            y += (int64_t(rect.b) - rect.t - height) / 2;
        else if (flags & 8)
            y = int64_t(rect.b) - height;
    }
    for (const auto &line : lines) {
        int64_t x = rect.l, width = width_of(line);
        if (flags & 1)
            x += (int64_t(rect.r) - rect.l - width) / 2;
        else if (flags & 2)
            x = int64_t(rect.r) - width;
        for (Cell cell : line) {
            pen.draw(hdc, x, y, cell.ch, cell.underline);
            x += pen.advance(cell.ch);
        }
        y += ch;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return uint32_t(y - rect.t);
}
void register_text() {
#define G(n, a, f)                                                                                 \
    {                                                                                              \
        "GDI32.dll", n, a, f                                                                       \
    }
    static const ImportShim shims[] = {G("CreateFontIndirectW", 1, create_font),
                                       G("CreateFontIndirectA", 1, create_font_indirect_a),
                                       G("CreateFontA", 14, create_font_a),
                                       G("ExtTextOutW", 8, ext_text_out_w),
                                       G("ExtTextOutA", 8, ext_text_out_a),
                                       G("TextOutW", 5, text_out_w),
                                       G("TextOutA", 5, text_out_a),
                                       G("GetTextExtentPoint32W", 4, extent_w),
                                       G("GetTextExtentPointW", 4, extent_w),
                                       G("GetTextExtentPoint32A", 4, extent_a),
                                       G("GetTextExtentPointA", 4, extent_a),
                                       G("GetTextMetricsW", 2, get_metrics_w),
                                       G("GetTextMetricsA", 2, get_metrics_a),
                                       G("GetCharWidthW", 4, char_widths_w),
                                       G("GetCharWidth32W", 4, char_widths_w),
                                       G("GetCharWidthA", 4, char_widths_a),
                                       G("GetCharWidth32A", 4, char_widths_a),
                                       G("SetTextAlign", 2, set_text_align),
                                       G("GetTextAlign", 1, get_text_align),
                                       G("GetTextFaceW", 3, text_face_w),
                                       G("GetTextFaceA", 3, text_face_a),
                                       G("AddFontMemResourceEx", 4, memory_font),
                                       G("EnumFontsW", 4, enumerate),
                                       G("EnumFontFamiliesExW", 5, enumerate),
                                       G("CreateEnhMetaFileW", 4, unsupported),
                                       G("CreateEnhMetaFileA", 4, unsupported),
                                       G("CloseEnhMetaFile", 1, unsupported),
                                       G("DeleteEnhMetaFile", 1, unsupported),
                                       G("GetEnhMetaFileW", 1, unsupported),
                                       G("GetEnhMetaFileA", 1, unsupported),
                                       G("GetEnhMetaFileBits", 3, unsupported),
                                       G("GetEnhMetaFileHeader", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionW", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionA", 3, unsupported),
                                       G("GetEnhMetaFilePaletteEntries", 3, unsupported),
                                       G("GetEnhMetaFilePixelFormat", 3, unsupported),
                                       G("SetEnhMetaFileBits", 2, unsupported),
                                       G("SetWinMetaFileBits", 4, unsupported),
                                       G("GetWinMetaFileBits", 5, unsupported),
                                       G("PlayEnhMetaFile", 3, unsupported),
                                       G("CopyEnhMetaFileW", 2, unsupported),
                                       G("CreateDCW", 4, unsupported),
                                       G("CreateICW", 4, unsupported),
                                       G("StartDocW", 2, unsupported),
                                       G("EndDoc", 1, unsupported),
                                       G("StartPage", 1, unsupported),
                                       G("EndPage", 1, unsupported),
                                       G("AbortDoc", 1, unsupported),
                                       G("SetAbortProc", 2, unsupported)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
} // namespace gdi
