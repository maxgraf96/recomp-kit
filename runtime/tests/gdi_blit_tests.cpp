// GDI's raster fast paths: differential tests against the generic per-pixel
// path, and a benchmark of a custom-drawn toolkit's repaint. No game image.
//
//   gdi_blit_tests          the differential suite
//   gdi_blit_tests bench    the repaint benchmark (fails above a generous limit)
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../gdi32_internal.h"
#include "../user32_internal.h"
#include "../display_seam.h"
#include "../../platform/os.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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

// ---------------------------------------------------------------------------
// Guest calls: the arguments and a return address on the guest stack, then the
// shim, as a translated caller would reach it.
// ---------------------------------------------------------------------------
static X86 g_cpu;
static uint32_t call(const char *dll, const char *name, const std::vector<uint32_t> &args) {
    const uint32_t tramp = imports_resolve(dll, name);
    if (!tramp) {
        check(false, "no trampoline for %s!%s", dll, name);
        return 0;
    }
    const uint32_t before = g_cpu.r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    g_cpu.r[R_ESP] = esp;
    imports_dispatch(&g_cpu, tramp);
    g_cpu.r[R_ESP] = before;
    return g_cpu.r[R_EAX];
}
static uint32_t g32(const char *name, const std::vector<uint32_t> &args) {
    return call("GDI32.dll", name, args);
}
static uint32_t u32(const char *name, const std::vector<uint32_t> &args) {
    return call("USER32.dll", name, args);
}

// Scratch guest memory for the structures the calls take.
constexpr uint32_t kScratch = 0x00310000;
constexpr uint32_t kClassName = kScratch + 0x800;
constexpr uint32_t kString = kScratch + 0x1000;
constexpr uint32_t kInfo = kScratch + 0x2000;
constexpr uint32_t kRect = kScratch + 0x3000;
constexpr uint32_t kPaint = kScratch + 0x3100;
constexpr uint32_t kPoints = kScratch + 0x3200;
constexpr uint32_t kBits = kScratch + 0x4000;

// A deterministic generator, so every run draws the same pictures.
struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed * 0x9e3779b97f4a7c15ull + 1) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return uint32_t(state >> 16);
    }
    int32_t range(int32_t lo, int32_t hi) { // [lo, hi]
        return lo + int32_t(next() % uint32_t(hi - lo + 1));
    }
    bool chance(int percent) {
        return int(next() % 100) < percent;
    }
};

// ---------------------------------------------------------------------------
// The objects a custom-drawn toolkit makes: windows, memory DCs holding DIBs.
// ---------------------------------------------------------------------------
static uint32_t register_class(uint32_t style) {
    static int serial = 0;
    char name[32];
    snprintf(name, sizeof name, "BlitTest%d", serial++);
    memset(g_mem + kScratch, 0, 40);
    wr32(kScratch + 0, style);
    wr32(kScratch + 4, imports_resolve("USER32.dll", "DefWindowProcW"));
    gm_put_wstr(kClassName, name, 32);
    wr32(kScratch + 36, kClassName);
    if (!u32("RegisterClassW", {kScratch}))
        check(false, "RegisterClassW %s", name);
    return kClassName;
}
static uint32_t make_window(uint32_t style, int x, int y, int w, int h, uint32_t parent,
                            uint32_t class_style = 0) {
    const uint32_t cls = register_class(class_style);
    return u32("CreateWindowExW", {0, cls, 0, style, uint32_t(x), uint32_t(y), uint32_t(w),
                                   uint32_t(h), parent, 0, IMAGE_BASE, 0});
}

// A DIB through CreateDIBitmap(CBM_INIT) with a BITMAPINFO: `bpp` 32 or 1,
// top-down when `top_down`, the pixels from `fill` (32 bpp: 0x00RRGGBB words;
// 1 bpp: a bit per pixel, MSB first). Monochrome takes `palette`'s two entries.
static uint32_t make_dib(uint32_t dc, int w, int h, int bpp, bool top_down,
                         const std::vector<uint32_t> &pixels, uint32_t palette0 = 0,
                         uint32_t palette1 = 0xffffff) {
    memset(g_mem + kInfo, 0, 48);
    wr32(kInfo, 40);
    wr32(kInfo + 4, uint32_t(w));
    wr32(kInfo + 8, uint32_t(top_down ? -h : h));
    wr16(kInfo + 12, 1);
    wr16(kInfo + 14, uint16_t(bpp));
    if (bpp == 1) {
        wr32(kInfo + 40, palette0);
        wr32(kInfo + 44, palette1);
    }
    const uint32_t stride = (uint32_t(w) * bpp + 31) / 32 * 4;
    const uint32_t bits = heap_alloc(stride * uint32_t(h), true);
    // `pixels` is top-down; the DIB's rows follow its own orientation.
    for (int y = 0; y < h; ++y) {
        const uint32_t row = bits + uint32_t(top_down ? y : h - 1 - y) * stride;
        for (int x = 0; x < w; ++x) {
            const uint32_t p = pixels[size_t(y) * w + x];
            if (bpp == 32)
                wr32(row + 4 * x, p);
            else if (p)
                wr8(row + x / 8, uint8_t(rd8(row + x / 8) | (0x80 >> (x % 8))));
        }
    }
    const uint32_t bitmap = g32("CreateDIBitmap", {dc, kInfo, 4, bits, kInfo, 0});
    heap_free(bits);
    return bitmap;
}
// A memory DC holding `bitmap`, as an image object keeps one.
static uint32_t dc_for(uint32_t screen_dc, uint32_t bitmap) {
    const uint32_t dc = g32("CreateCompatibleDC", {screen_dc});
    g32("SelectObject", {dc, bitmap});
    return dc;
}

// ---------------------------------------------------------------------------
// The benchmark: a 1024x768 custom-drawn window repainted the way a GDI
// toolkit does it (WONAPI's MSGraphics, the multiplayer lobby of a 2001 RTS):
// a back buffer from CreateCompatibleBitmap, a background picture, translucent
// panels (AlphaBlend at constant alpha 127, tile by tile), ~150 masked images
// (a 1-bpp mask with SRCAND after SetTextColor(0)/SetBkColor(white), then the
// image with SRCPAINT), rectangles with a pen and brush made per colour, lines,
// round widgets, text runs, clip rectangles per component; then WM_PAINT's one
// BitBlt(SRCCOPY) of the whole back buffer to the window DC, and the present
// that composes the window over a DirectDraw program's last frame.
// ---------------------------------------------------------------------------
static uint32_t g_base_window = 0;
static int g_overlay_host = 0;
static uint64_t g_overlay_bytes = 0;
static std::vector<uint32_t> g_overlay_copy;
extern "C" bool ddraw7_screen_owner(uint32_t *hwnd) {
    if (!g_base_window)
        return false;
    *hwnd = g_base_window;
    return true;
}
extern "C" int host_display_overlay_supported() {
    return g_overlay_host;
}
// What a GPU host does with it: one copy into a texture.
extern "C" void host_display_present_overlay(const uint32_t *argb, int w, int h) {
    if (argb) {
        g_overlay_copy.assign(argb, argb + size_t(w) * h);
        g_overlay_bytes += uint64_t(w) * h * 4;
    }
}
static std::vector<uint32_t> g_presented;
extern "C" void host_display_present_window(const uint32_t *argb, int w, int h) {
    if (argb)
        g_presented.assign(argb, argb + size_t(w) * h);
}

struct Phase {
    const char *name;
    uint64_t ns = 0;
};
enum { kBackground, kPanels, kImages, kShapes, kTextPhase, kCopy, kPresent, kPhases };
static Phase g_phase[kPhases] = {{"background SRCCOPY"},
                                 {"AlphaBlend panels"},
                                 {"masked images"},
                                 {"rectangles, lines, arcs"},
                                 {"text"},
                                 {"WM_PAINT SRCCOPY"},
                                 {"present"}};
struct Timer {
    int phase;
    uint64_t start = os_monotonic_ns();
    explicit Timer(int p) : phase(p) {}
    ~Timer() {
        g_phase[phase].ns += os_monotonic_ns() - start;
    }
};

struct Lobby {
    uint32_t hwnd = 0, window_dc = 0;
    uint32_t back = 0, back_dc = 0;
    uint32_t background_dc = 0, tile_dc = 0;
    std::vector<uint32_t> image_dc, mask_dc;
    std::vector<std::pair<int, int>> image_size;
    std::vector<uint32_t> fonts;
};

static Lobby make_lobby() {
    Lobby l;
    g_base_window = make_window(0x80000000u, 0, 0, 1024, 768, 0);
    u32("ShowWindow", {g_base_window, 5});
    // WndClass0: CS_OWNDC | CS_DBLCLKS, WS_POPUP, owned by the game window.
    l.hwnd = make_window(0x80000000u, 0, 0, 1024, 768, g_base_window, 0x28);
    u32("ShowWindow", {l.hwnd, 5});
    l.window_dc = u32("GetDC", {l.hwnd});
    l.back = g32("CreateCompatibleBitmap", {l.window_dc, 1024, 768});
    l.back_dc = dc_for(l.window_dc, l.back);
    Random r(7);
    std::vector<uint32_t> px(1024 * 768);
    for (int y = 0; y < 768; ++y)
        for (int x = 0; x < 1024; ++x)
            px[size_t(y) * 1024 + x] =
                uint32_t((x * 255 / 1023) << 16 | (y * 255 / 767) << 8 | ((x ^ y) & 255));
    l.background_dc = dc_for(l.window_dc, make_dib(l.window_dc, 1024, 768, 32, true, px));
    // The translucent fill's tile: a 100x100x32 DIB section.
    memset(g_mem + kInfo, 0, 40);
    wr32(kInfo, 40);
    wr32(kInfo + 4, 100);
    wr32(kInfo + 8, uint32_t(-100));
    wr16(kInfo + 12, 1);
    wr16(kInfo + 14, 32);
    const uint32_t tile = g32("CreateDIBSection", {l.window_dc, kInfo, 0, kInfo + 64, 0, 0});
    const uint32_t tile_bits = rd32(kInfo + 64);
    for (int i = 0; i < 100 * 100; ++i)
        wr32(tile_bits + 4 * i, 0x00203040);
    l.tile_dc = dc_for(l.window_dc, tile);
    // The skin's images, each with its mask: buttons, icons, frame pieces.
    const std::pair<int, int> sizes[] = {{16, 16},  {24, 24}, {32, 32},  {64, 24},
                                         {120, 28}, {13, 13}, {200, 20}, {48, 48}};
    for (int i = 0; i < 24; ++i) {
        const auto [w, h] = sizes[i % 8];
        std::vector<uint32_t> image(size_t(w) * h), mask(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int dx = 2 * x - w, dy = 2 * y - h;
                const bool inside = dx * dx * h * h + dy * dy * w * w <= w * w * h * h;
                image[size_t(y) * w + x] = inside ? (r.next() & 0xffffff) : 0;
                mask[size_t(y) * w + x] = inside ? 0 : 1; // white where transparent
            }
        l.image_dc.push_back(dc_for(l.window_dc, make_dib(l.window_dc, w, h, 32, true, image)));
        l.mask_dc.push_back(dc_for(l.window_dc, make_dib(l.window_dc, w, h, 1, true, mask)));
        l.image_size.push_back({w, h});
    }
    for (int height : {-13, -15, -16}) {
        gm_put_str(kInfo, "Tahoma", 32);
        l.fonts.push_back(
            g32("CreateFontA", {uint32_t(height), 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, kInfo}));
    }
    return l;
}

// MSGraphics::SetColor: a new brush and pen for the colour, the old ones deleted.
static void set_color(uint32_t dc, uint32_t color, uint32_t *brush, uint32_t *pen) {
    const uint32_t b = g32("CreateSolidBrush", {color}), p = g32("CreatePen", {0, 0, color});
    g32("SelectObject", {dc, b});
    g32("SelectObject", {dc, p});
    if (*brush)
        g32("DeleteObject", {*brush});
    if (*pen)
        g32("DeleteObject", {*pen});
    *brush = b;
    *pen = p;
    g32("SetTextColor", {dc, color});
}
static void clip_to(uint32_t dc, int l, int t, int r, int b) {
    g32("SelectClipRgn", {dc, 0});
    g32("IntersectClipRect", {dc, uint32_t(l), uint32_t(t), uint32_t(r), uint32_t(b)});
}

// One whole-window repaint.
static void repaint(Lobby &l) {
    const uint32_t dc = l.back_dc;
    g32("SetBkMode", {dc, 1});
    {
        Timer t(kBackground);
        g32("SelectClipRgn", {dc, 0});
        g32("BitBlt", {dc, 0, 0, 1024, 768, l.background_dc, 0, 0, 0x00cc0020u});
    }
    {
        Timer t(kPanels);
        // Four translucent panels, 100x100 tiles at constant alpha 127.
        const int panels[4][4] = {
            {16, 120, 600, 420}, {620, 120, 1008, 420}, {16, 440, 600, 700}, {620, 440, 1008, 700}};
        for (auto &p : panels) {
            clip_to(dc, p[0], p[1], p[2], p[3]);
            for (int y = p[1]; y < p[3]; y += 100)
                for (int x = p[0]; x < p[2]; x += 100) {
                    const int w = std::min(100, p[2] - x), h = std::min(100, p[3] - y);
                    call("msimg32.dll", "AlphaBlend",
                         {dc, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), l.tile_dc, 0, 0,
                          uint32_t(w), uint32_t(h), 127u << 16});
                }
        }
    }
    {
        Timer t(kImages);
        // 150 masked images in a grid of components, each clipped to itself.
        for (int i = 0; i < 150; ++i) {
            const size_t k = size_t(i) % l.image_dc.size();
            const auto [w, h] = l.image_size[k];
            const int x = 8 + (i * 97) % (1024 - 216), y = 8 + (i * 61) % (768 - 56);
            clip_to(dc, x, y, x + w, y + h);
            g32("SetTextColor", {dc, 0});
            g32("SetBkColor", {dc, 0xffffff});
            g32("BitBlt", {dc, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), l.mask_dc[k], 0,
                           0, 0x008800c6u});
            g32("BitBlt", {dc, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), l.image_dc[k], 0,
                           0, 0x00ee0086u});
        }
    }
    uint32_t brush = 0, pen = 0;
    {
        Timer t(kShapes);
        for (int i = 0; i < 30; ++i) {
            const int x = 20 + (i * 131) % 800, y = 130 + (i * 71) % 520;
            clip_to(dc, x, y, x + 180, y + 40);
            set_color(dc, 0x101010u * uint32_t(i % 15 + 1), &brush, &pen);
            g32("Rectangle", {dc, uint32_t(x), uint32_t(y), uint32_t(x + 180), uint32_t(y + 40)});
            wr32(kPoints, uint32_t(x));
            wr32(kPoints + 4, uint32_t(y + 39));
            wr32(kPoints + 8, uint32_t(x + 179));
            wr32(kPoints + 12, uint32_t(y + 39));
            wr32(kPoints + 16, uint32_t(x + 179));
            wr32(kPoints + 20, uint32_t(y));
            set_color(dc, 0xc0c0c0u, &brush, &pen);
            g32("Polyline", {dc, kPoints, 3});
        }
        for (int i = 0; i < 12; ++i) {
            const int x = 40 + i * 70, y = 720;
            clip_to(dc, x, y, x + 13, y + 13);
            set_color(dc, 0x00ffffffu, &brush, &pen);
            g32("Ellipse", {dc, uint32_t(x), uint32_t(y), uint32_t(x + 13), uint32_t(y + 13)});
            set_color(dc, 0x00404040u, &brush, &pen);
            g32("Arc", {dc, uint32_t(x), uint32_t(y), uint32_t(x + 13), uint32_t(y + 13),
                        uint32_t(x + 13), uint32_t(y), uint32_t(x), uint32_t(y + 13)});
        }
        g32("SelectClipRgn", {dc, 0});
        for (int i = 0; i < 20; ++i)
            g32("SetPixel", {dc, uint32_t(500 + i), 10, 0x00ffff00u});
    }
    {
        Timer t(kTextPhase);
        // 80 runs of text: the chat history, the player list, the options.
        static const char *const lines[] = {
            "Unknown144: hello mac",     "Game: mac-ref",
            "Map Type: Continental",     "Starting Age: Prehistoric",
            "Players 1/8 (computers 0)", "Resources: Standard"};
        for (int i = 0; i < 80; ++i) {
            const int x = 24 + (i % 2) * 610, y = 130 + (i / 2) * 14;
            clip_to(dc, x, y, x + 380, y + 16);
            g32("SelectObject", {dc, l.fonts[size_t(i) % l.fonts.size()]});
            set_color(dc, i % 3 ? 0x00ffffffu : 0x0000d0ffu, &brush, &pen);
            const std::string text = lines[i % 6];
            gm_put_wstr(kString, text, 64);
            g32("TextOutW", {dc, uint32_t(x), uint32_t(y), kString, uint32_t(text.size())});
        }
    }
    g32("SelectClipRgn", {dc, 0});
    g32("DeleteObject", {brush});
    g32("DeleteObject", {pen});
    // WM_PAINT: the whole back buffer to the window, then EndPaint presents.
    const uint32_t paint_dc = u32("BeginPaint", {l.hwnd, kPaint});
    {
        Timer t(kCopy);
        g32("BitBlt", {paint_dc, 0, 0, 1024, 768, dc, 0, 0, 0x00cc0020u});
    }
    {
        Timer t(kPresent);
        u32("EndPaint", {l.hwnd, kPaint});
    }
}

static int bench(int argc, char **argv) {
    const int repaints = argc > 2 ? std::max(1, atoi(argv[2])) : 10;
    Lobby lobby = make_lobby();
    double best_ms = 1e9;
    {
        // The per-pixel path, for reference: what every repaint cost before.
        gdi::fast_paths() = false;
        g_overlay_host = 1;
        repaint(lobby);
        const uint64_t t0 = os_monotonic_ns();
        repaint(lobby);
        printf("repaint on the per-pixel path (RECOMP_GDI_GENERIC=1): %.2f ms\n",
               double(os_monotonic_ns() - t0) / 1e6);
        gdi::fast_paths() = true;
    }
    for (int host = 0; host < 2; ++host) {
        g_overlay_host = host;
        for (auto &p : g_phase)
            p.ns = 0;
        repaint(lobby); // warm: glyph caches, the overlay's storage
        for (auto &p : g_phase)
            p.ns = 0;
        uint64_t best = UINT64_MAX;
        const uint64_t start = os_monotonic_ns();
        for (int i = 0; i < repaints; ++i) {
            const uint64_t t0 = os_monotonic_ns();
            repaint(lobby);
            best = std::min(best, os_monotonic_ns() - t0);
        }
        const double mean = double(os_monotonic_ns() - start) / repaints / 1e6;
        printf("repaint, %s: %.2f ms mean, %.2f ms best (%d repaints)\n",
               host ? "overlay handed to the host (GPU presenter)"
                    : "overlay blended on the CPU (headless)",
               mean, double(best) / 1e6, repaints);
        for (auto &p : g_phase)
            printf("  %-26s %8.3f ms\n", p.name, double(p.ns) / repaints / 1e6);
        best_ms = std::min(best_ms, double(best) / 1e6);
    }
    // Generous: the fast paths make a repaint a few milliseconds on a laptop;
    // the generic per-pixel path took hundreds.
    const double limit = argc > 3 ? atof(argv[3]) : 50.0;
    check(best_ms < limit, "a whole-window repaint in %.2f ms (best), under %.0f ms", best_ms,
          limit);
    return 0;
}

// ---------------------------------------------------------------------------
// The differential suite: a world of DCs over every kind of storage, random DC
// state, and random drawing operations, each run twice from the same pixels -
// on the per-pixel path, then on the fast paths - and compared: the result and
// every pixel of every bitmap and window surface, and the surfaces' dirty flags.
// ---------------------------------------------------------------------------
struct Storage {
    uint32_t bitmap = 0, bits = 0, bytes = 0;
};
struct World {
    std::vector<Storage> bitmaps;
    std::vector<uint32_t> windows;
    std::vector<uint32_t> dcs;
    std::vector<std::string> names;
    std::vector<uint32_t> fonts;
};
struct Snapshot {
    std::vector<std::vector<uint8_t>> bits;
    std::vector<std::vector<uint32_t>> surfaces;
    std::vector<uint8_t> dirty;
    bool operator==(const Snapshot &o) const {
        return bits == o.bits && surfaces == o.surfaces && dirty == o.dirty;
    }
};
static Snapshot capture(const World &w) {
    Snapshot s;
    for (const Storage &b : w.bitmaps)
        s.bits.emplace_back(g_mem + b.bits, g_mem + b.bits + b.bytes);
    for (uint32_t h : w.windows) {
        const auto *win = user32::find_window(h);
        s.surfaces.push_back(win->surface.argb);
        s.dirty.push_back(win->surface.dirty);
    }
    return s;
}
static void restore(const World &w, const Snapshot &s) {
    for (size_t i = 0; i < w.bitmaps.size(); ++i)
        memcpy(g_mem + w.bitmaps[i].bits, s.bits[i].data(), s.bits[i].size());
    for (size_t i = 0; i < w.windows.size(); ++i) {
        auto *win = user32::find_window(w.windows[i]);
        win->surface.argb = s.surfaces[i];
        win->surface.dirty = s.dirty[i] != 0;
    }
}
// Where two snapshots first differ, for the report.
static std::string first_difference(const World &w, const Snapshot &a, const Snapshot &b) {
    char out[160];
    for (size_t i = 0; i < a.bits.size(); ++i)
        for (size_t k = 0; k < a.bits[i].size(); ++k)
            if (a.bits[i][k] != b.bits[i][k]) {
                snprintf(out, sizeof out, "bitmap %08x byte %zu: %02x per-pixel, %02x fast",
                         w.bitmaps[i].bitmap, k, a.bits[i][k], b.bits[i][k]);
                return out;
            }
    for (size_t i = 0; i < a.surfaces.size(); ++i) {
        for (size_t k = 0; k < a.surfaces[i].size(); ++k)
            if (a.surfaces[i][k] != b.surfaces[i][k]) {
                snprintf(out, sizeof out, "window %08x pixel %zu: %08x per-pixel, %08x fast",
                         w.windows[i], k, a.surfaces[i][k], b.surfaces[i][k]);
                return out;
            }
        if (a.dirty[i] != b.dirty[i]) {
            snprintf(out, sizeof out, "window %08x dirty: %d per-pixel, %d fast", w.windows[i],
                     a.dirty[i], b.dirty[i]);
            return out;
        }
    }
    return "nothing";
}
static Storage storage_of(uint32_t bitmap) {
    Storage s;
    s.bitmap = bitmap;
    g32("GetObjectA", {bitmap, 24, kInfo + 0x100});
    s.bits = rd32(kInfo + 0x100 + 20);
    s.bytes = rd32(kInfo + 0x100 + 12) * rd32(kInfo + 0x100 + 8);
    return s;
}
// A DIB section of any layout: `table` the colour table or BI_BITFIELDS masks,
// `used` biClrUsed (0: the format's default count).
static uint32_t section(uint32_t screen, int w, int h, int bpp, uint32_t compression,
                        const std::vector<uint32_t> &table, uint32_t used = 0) {
    memset(g_mem + kInfo, 0, 40 + 4 * 256);
    wr32(kInfo, 40);
    wr32(kInfo + 4, uint32_t(w));
    wr32(kInfo + 8, uint32_t(h));
    wr16(kInfo + 12, 1);
    wr16(kInfo + 14, uint16_t(bpp));
    wr32(kInfo + 16, compression);
    wr32(kInfo + 32, used);
    for (size_t i = 0; i < table.size(); ++i)
        wr32(kInfo + 40 + 4 * uint32_t(i), table[i]);
    return g32("CreateDIBSection", {screen, kInfo, 0, kInfo + 0x500, 0, 0});
}
static void fill_random(const Storage &s, Random &r) {
    for (uint32_t i = 0; i < s.bytes; ++i)
        wr8(s.bits + i, uint8_t(r.next()));
}
static World make_world(Random &r) {
    World w;
    // A top-level window clipping its children, one child inside it and one
    // reaching past its edge: window DCs with client bounds and exclusions.
    const uint32_t top = make_window(0x92000000u, 3, 5, 70, 50, 0);
    const uint32_t inner = make_window(0x50000000u, 10, 8, 30, 20, top);
    const uint32_t outer = make_window(0x50000000u, 50, 30, 40, 30, top);
    for (uint32_t h : {top, inner, outer})
        u32("ShowWindow", {h, 5});
    const uint32_t screen = u32("GetDC", {top});
    for (uint32_t h : {inner, outer}) {
        w.dcs.push_back(u32("GetDC", {h}));
        w.names.push_back(h == inner ? "child window DC" : "clipped child window DC");
    }
    w.dcs.push_back(screen);
    w.names.push_back("WS_CLIPCHILDREN window DC");
    g32("GetPixel", {screen, 0, 0}); // sizes the surface
    w.windows.push_back(top);
    for (uint32_t &p : user32::find_window(top)->surface.argb)
        p = r.next();
    auto add = [&](uint32_t bitmap, const char *name, bool twice = false) {
        if (!bitmap) {
            check(false, "made the %s", name);
            return;
        }
        const Storage s = storage_of(bitmap);
        fill_random(s, r);
        w.bitmaps.push_back(s);
        for (int i = 0; i < (twice ? 2 : 1); ++i) {
            w.dcs.push_back(dc_for(screen, bitmap));
            w.names.push_back(std::string(name) + (i ? " (second DC)" : ""));
        }
    };
    add(g32("CreateCompatibleBitmap", {screen, 53, 41}), "bottom-up 32-bit bitmap", true);
    add(section(screen, 47, -33, 32, 0, {}), "top-down 32-bit section");
    add(section(screen, 1, -9, 32, 0, {}), "one-column 32-bit section");
    add(section(screen, 61, 1, 32, 0, {}), "one-row 32-bit section");
    add(section(screen, 29, -23, 32, 3, {0xff0000, 0xff00, 0xff}), "BI_BITFIELDS 32-bit section");
    add(section(screen, 25, -19, 16, 0, {}), "16-bit section");
    add(section(screen, 23, 17, 24, 0, {}), "24-bit section");
    std::vector<uint32_t> palette(256);
    for (uint32_t &p : palette)
        p = r.next() & 0xffffff;
    add(section(screen, 21, -15, 8, 0, palette), "8-bit section");
    // Monochrome, of odd widths (odd strides) and colour tables: black and
    // white, two colours with the reserved byte set, one entry only.
    const int widths[] = {1, 7, 9, 31, 33, 45, 70};
    for (int i = 0; i < 7; ++i) {
        const int width = widths[i], height = r.range(1, 40) * (r.chance(50) ? -1 : 1);
        std::vector<uint32_t> table = {0, 0xffffff};
        uint32_t used = 0;
        if (i % 3 == 1)
            table = {r.next(), r.next()};
        if (i % 3 == 2) {
            table = {r.next() & 0xffffff, 0};
            used = 1;
        }
        char name[64];
        snprintf(name, sizeof name, "1-bit section %dx%d", width, height);
        add(section(screen, width, height, 1, 0, table, used), name);
    }
    w.dcs.push_back(g32("CreateCompatibleDC", {screen}));
    w.names.push_back("memory DC without a bitmap");
    for (int height : {-13, -16}) {
        gm_put_str(kInfo, "Tahoma", 32);
        w.fonts.push_back(
            g32("CreateFontA", {uint32_t(height), 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, kInfo}));
    }
    return w;
}
static uint32_t colorref(Random &r) {
    switch (r.range(0, 4)) {
    case 0:
        return 0;
    case 1:
        return 0xffffff;
    case 2:
        return r.next() & 0x00ffffffu;
    default:
        return r.next(); // a high byte GDI ignores
    }
}
// Random state for one DC; not an operation that is compared.
static void change_state(const World &w, uint32_t dc, Random &r) {
    switch (r.range(0, 9)) {
    case 0:
        g32("SetViewportOrgEx", {dc, uint32_t(r.range(-6, 6)), uint32_t(r.range(-6, 6)), 0});
        break;
    case 1:
        g32("SetWindowOrgEx", {dc, uint32_t(r.range(-6, 6)), uint32_t(r.range(-6, 6)), 0});
        break;
    case 2:
        g32("SelectClipRgn", {dc, 0});
        g32("SetViewportOrgEx", {dc, 0, 0, 0});
        g32("SetWindowOrgEx", {dc, 0, 0, 0});
        break;
    case 3:
        // Each one narrows the clip further; start again from the whole DC
        // half the time, so the clip stays a region worth drawing through.
        if (r.chance(50))
            g32("SelectClipRgn", {dc, 0});
        g32(r.chance(60) ? "IntersectClipRect" : "ExcludeClipRect",
            {dc, uint32_t(r.range(-5, 25)), uint32_t(r.range(-5, 20)), uint32_t(r.range(20, 75)),
             uint32_t(r.range(15, 55))});
        break;
    case 4: {
        const uint32_t region =
            g32("CreateRectRgn", {uint32_t(r.range(-5, 30)), uint32_t(r.range(-5, 30)),
                                  uint32_t(r.range(5, 70)), uint32_t(r.range(5, 50))});
        g32("SelectClipRgn", {dc, region});
        g32("DeleteObject", {region});
        break;
    }
    case 5:
        g32("SetTextColor", {dc, colorref(r)});
        g32("SetBkColor", {dc, colorref(r)});
        break;
    case 6:
        g32("SetBkMode", {dc, uint32_t(r.range(1, 2))});
        g32("SetROP2", {dc, uint32_t(r.chance(50) ? 13 : r.range(1, 16))});
        break;
    case 7:
        g32("SelectObject", {dc, r.chance(20) ? g32("GetStockObject", {5}) // NULL_BRUSH
                                              : g32("CreateSolidBrush", {colorref(r)})});
        break;
    case 8:
        g32("SelectObject", {dc, g32("CreatePen", {uint32_t(r.chance(15) ? 5 : 0),
                                                   uint32_t(r.range(0, 3)), colorref(r)})});
        break;
    default:
        g32("SelectObject", {dc, w.fonts[size_t(r.range(0, int(w.fonts.size()) - 1))]});
        break;
    }
}
struct Operation {
    std::string what;
    std::function<uint32_t()> run;
    std::string kind() const {
        return what.substr(0, what.find(" into"));
    }
};
static Operation random_operation(const World &w, Random &r) {
    // Mostly a destination the fast paths can write and a source they can
    // read; any DC sometimes, to see them decline it.
    auto pick = [&](bool destination) {
        for (;;) {
            const size_t i = size_t(r.range(0, int(w.dcs.size()) - 1));
            gdi::Canvas canvas;
            gdi::fast_paths() = true;
            gdi::resolve(w.dcs[i], &canvas);
            const bool modelled =
                destination ? canvas.writable() : canvas.kind != gdi::Canvas::None;
            if (modelled || r.chance(15))
                return i;
        }
    };
    const size_t a = pick(true);
    const size_t b = r.chance(15) ? a : pick(false);
    const uint32_t dst = w.dcs[a], src = w.dcs[b];
    // Rectangles around what each DC shows (its clip box, in its logical
    // coordinates): inside it, across its edges, past them.
    auto area = [](uint32_t dc) {
        gdi::Rect box = gdi::clip_box(dc);
        if (box.r <= box.l || box.b <= box.t)
            box = {0, 0, 40, 30};
        return box;
    };
    const gdi::Rect da = area(dst), sa = area(src);
    const int32_t x = r.range(da.l - 6, da.r), y = r.range(da.t - 6, da.b);
    int32_t cx = r.chance(3) ? 0 : r.range(-12, da.r - da.l + 8),
            cy = r.chance(3) ? 0 : r.range(-12, da.b - da.t + 8);
    if (r.chance(85)) { // mostly the forward rectangles a toolkit draws
        cx = std::abs(cx);
        cy = std::abs(cy);
    }
    const int32_t sx = r.range(sa.l - 6, sa.r - 1), sy = r.range(sa.t - 6, sa.b - 1);
    auto name = [&](const char *op) {
        char what[256];
        const gdi::Rect dc = gdi::clip_box(dst), sc = gdi::clip_box(src);
        snprintf(what, sizeof what,
                 "%s into the %s (%d,%d %dx%d; clip %d,%d-%d,%d) from the %s (%d,%d; clip "
                 "%d,%d-%d,%d)",
                 op, w.names[a].c_str(), x, y, cx, cy, dc.l, dc.t, dc.r, dc.b, w.names[b].c_str(),
                 sx, sy, sc.l, sc.t, sc.r, sc.b);
        return std::string(what);
    };
    static const uint32_t rops[] = {0x00cc0020, 0x008800c6, 0x00ee0086, 0x00660046,
                                    0x00330008, 0x00f00021, 0x00000042, 0x00ff0062,
                                    0x00550009, 0x00c000ca, 0x00bb0226, 0x005a0049};
    const uint32_t rop = r.chance(85) ? rops[r.range(0, 11)] : (r.next() & 0x00ff0000u);
    const uint32_t c = 0x00ffffffu & r.next();
    switch (r.range(0, 12)) {
    case 0:
    case 1:
    case 2: // the lobby's three, SRCCOPY, SRCAND and SRCPAINT, among the rest
        return {name("BitBlt") + " rop " + std::to_string(rop >> 16), [=] {
                    return g32("BitBlt", {dst, uint32_t(x), uint32_t(y), uint32_t(cx), uint32_t(cy),
                                          src, uint32_t(sx), uint32_t(sy), rop});
                }};
    case 3: {
        const bool same = r.chance(60);
        const int32_t sw = same ? cx : r.range(-30, 60), sh = same ? cy : r.range(-30, 60);
        return {name("StretchBlt"), [=] {
                    return g32("StretchBlt",
                               {dst, uint32_t(x), uint32_t(y), uint32_t(cx), uint32_t(cy), src,
                                uint32_t(sx), uint32_t(sy), uint32_t(sw), uint32_t(sh), rop});
                }};
    }
    case 4:
    case 5: {
        const bool same = r.chance(80);
        const int32_t sw = same ? cx : r.range(1, 60), sh = same ? cy : r.range(1, 60);
        const uint32_t blend =
            (r.chance(25) ? 1u << 24 : 0u) | uint32_t(r.chance(40) ? 127 : r.range(0, 255)) << 16;
        return {name("AlphaBlend"), [=] {
                    return call("msimg32.dll", "AlphaBlend",
                                {dst, uint32_t(x), uint32_t(y), uint32_t(cx), uint32_t(cy), src,
                                 uint32_t(sx), uint32_t(sy), uint32_t(sw), uint32_t(sh), blend});
                }};
    }
    case 6: {
        static const uint32_t pat_rops[] = {0x00f00021, 0x005a0049, 0x00550009, 0x00000042,
                                            0x00ff0062};
        const uint32_t prop = pat_rops[r.range(0, 4)];
        return {name("PatBlt"), [=] {
                    return g32("PatBlt",
                               {dst, uint32_t(x), uint32_t(y), uint32_t(cx), uint32_t(cy), prop});
                }};
    }
    case 7: {
        const bool ellipse = r.chance(30);
        return {name(ellipse ? "Ellipse" : "Rectangle"), [=] {
                    return g32(ellipse ? "Ellipse" : "Rectangle",
                               {dst, uint32_t(x), uint32_t(y), uint32_t(x + cx), uint32_t(y + cy)});
                }};
    }
    case 8: {
        const int32_t xs = r.range(-5, 70), ys = r.range(-5, 50), xe = r.range(-5, 70),
                      ye = r.range(-5, 50);
        return {name("Arc"), [=] {
                    return g32("Arc",
                               {dst, uint32_t(x), uint32_t(y), uint32_t(x + cx), uint32_t(y + cy),
                                uint32_t(xs), uint32_t(ys), uint32_t(xe), uint32_t(ye)});
                }};
    }
    case 9: {
        std::vector<int32_t> points;
        const int n = r.range(2, 5);
        for (int i = 0; i < 2 * n; ++i)
            points.push_back(r.range(-8, 75));
        const bool polygon = r.chance(50);
        return {name(polygon ? "Polygon" : "MoveToEx, LineTo, Polyline"), [=] {
                    for (size_t i = 0; i < points.size(); ++i)
                        wr32(kPoints + 4 * uint32_t(i), uint32_t(points[i]));
                    if (polygon)
                        return g32("Polygon", {dst, kPoints, uint32_t(points.size() / 2)});
                    g32("MoveToEx", {dst, uint32_t(points[0]), uint32_t(points[1]), 0});
                    const uint32_t line =
                        g32("LineTo", {dst, uint32_t(points[2]), uint32_t(points[3])});
                    return line ^
                           (g32("Polyline", {dst, kPoints, uint32_t(points.size() / 2)}) << 1);
                }};
    }
    case 10: {
        static const char *const texts[] = {"Unknown144: hello mac", "Wy", "@#|", "i", "Map Type"};
        const std::string text = texts[r.range(0, 4)];
        return {name("TextOutW"), [=] {
                    gm_put_wstr(kString, text, 64);
                    return g32("TextOutW",
                               {dst, uint32_t(x), uint32_t(y), kString, uint32_t(text.size())});
                }};
    }
    case 11:
        return {name("FillRect"), [=] {
                    wr32(kRect, uint32_t(x));
                    wr32(kRect + 4, uint32_t(y));
                    wr32(kRect + 8, uint32_t(x + cx));
                    wr32(kRect + 12, uint32_t(y + cy));
                    const uint32_t brush = g32("CreateSolidBrush", {c});
                    const uint32_t ok = u32("FillRect", {dst, kRect, brush});
                    g32("DeleteObject", {brush});
                    return ok;
                }};
    default:
        return {name("SetPixel"),
                [=] { return g32("SetPixel", {dst, uint32_t(x), uint32_t(y), c}); }};
    }
}
static void test_random_operations(uint64_t seed, int count) {
    Random r(seed);
    World w = make_world(r);
    int failures = 0, drew = 0;
    std::map<std::string, std::pair<int, int>> kinds; // operations, of them drew
    for (int i = 0; i < count; ++i) {
        if (r.chance(30))
            change_state(w, w.dcs[size_t(r.range(0, int(w.dcs.size()) - 1))], r);
        const Operation op = random_operation(w, r);
        for (uint32_t h : w.windows)
            user32::find_window(h)->surface.dirty = false;
        const Snapshot before = capture(w);
        gdi::fast_paths() = false;
        const uint32_t generic_result = op.run();
        const Snapshot generic = capture(w);
        restore(w, before);
        gdi::fast_paths() = true;
        const uint32_t fast_result = op.run();
        const Snapshot fast = capture(w);
        drew += !(generic == before);
        auto &tally = kinds[op.kind()];
        ++tally.first;
        tally.second += !(generic == before);
        if (generic_result != fast_result || !(generic == fast)) {
            if (++failures <= 10)
                check(false, "operation %d, %s: result %u per-pixel, %u fast; %s", i,
                      op.what.c_str(), generic_result, fast_result,
                      first_difference(w, generic, fast).c_str());
        }
    }
    check(failures == 0,
          "seed %llu: the fast paths equal the per-pixel path in %d of %d random operations (%d "
          "of them drew)",
          (unsigned long long)seed, count - failures, count, drew);
    std::string line;
    for (const auto &k : kinds)
        line += " " + k.first + " " + std::to_string(k.second.second) + "/" +
                std::to_string(k.second.first) + ";";
    printf("       drew/ran:%s\n", line.c_str());
}
// The benchmark's whole repaint, once on each path: the back buffer, the
// window surface and the overlay handed to the host are the same pixels.
static void test_lobby_scene() {
    Lobby lobby = make_lobby();
    g_overlay_host = 1;
    const Storage back = storage_of(lobby.back);
    auto shot = [&] {
        std::vector<uint32_t> out(back.bytes / 4);
        memcpy(out.data(), g_mem + back.bits, back.bytes);
        const auto &surface = user32::find_window(lobby.hwnd)->surface.argb;
        out.insert(out.end(), surface.begin(), surface.end());
        out.insert(out.end(), g_overlay_copy.begin(), g_overlay_copy.end());
        return out;
    };
    gdi::fast_paths() = false;
    repaint(lobby);
    const std::vector<uint32_t> generic = shot();
    // The second repaint starts from other pixels: it has to draw them all.
    memset(g_mem + back.bits, 0x5a, back.bytes);
    gdi::fast_paths() = true;
    repaint(lobby);
    const std::vector<uint32_t> fast = shot();
    size_t first = 0;
    while (first < generic.size() && first < fast.size() && generic[first] == fast[first])
        ++first;
    check(generic == fast && generic.size() == size_t(1024 * 768 * 3),
          "the benchmark's repaint (back buffer, window surface, overlay) is the same on both "
          "paths (first difference at word %zu of %zu)",
          first, generic.size());
    g_base_window = 0;
}

// The overlay a present composes, once on each path: windows stacked over a
// DirectDraw program's window, partly drawn (alpha 0 where nothing was), one
// of them a layered window with a colour key and a constant alpha.
static void test_overlay_composition() {
    Random r(11);
    g_base_window = make_window(0x80000000u, 0, 0, 1024, 768, 0);
    u32("ShowWindow", {g_base_window, 5});
    std::vector<uint32_t> windows;
    const int boxes[3][4] = {{0, 0, 1024, 768}, {100, 90, 300, 200}, {600, 500, 500, 400}};
    for (auto &b : boxes) {
        const uint32_t h = make_window(0x80000000u, b[0], b[1], b[2], b[3], g_base_window);
        u32("ShowWindow", {h, 5});
        g32("GetPixel", {u32("GetDC", {h}), 0, 0}); // sizes its surface
        for (uint32_t &p : user32::find_window(h)->surface.argb)
            p = r.chance(30) ? 0 : r.next() | (r.chance(50) ? 0xff000000u : 0);
        windows.push_back(h);
    }
    auto *layered = user32::find_window(windows[2]);
    layered->exstyle |= 0x80000; // WS_EX_LAYERED
    layered->layered_flags = 3;
    layered->layered_key = layered->surface.argb[5] & 0xffffff;
    layered->layered_alpha = 100;
    g_overlay_host = 1;
    std::vector<uint32_t> composed[2];
    for (int fast = 0; fast < 2; ++fast) {
        gdi::fast_paths() = fast != 0;
        display_layers_changed();
        g_overlay_copy.clear();
        gdi_present_windows();
        composed[fast] = g_overlay_copy;
    }
    check(!composed[0].empty() && composed[0] == composed[1],
          "the overlay of three stacked windows, one layered, is the same on both paths (%zu "
          "pixels)",
          composed[0].size());
    for (uint32_t h : windows)
        u32("DestroyWindow", {h});
    u32("DestroyWindow", {g_base_window});
    g_base_window = 0;
}

int main(int argc, char **argv) {
    os_setenv("RECOMP_SYSTEM_FONTS", "0");
    mem_init();
    imports_init();
    loader_init_context(&g_cpu);
    if (argc > 1 && strcmp(argv[1], "bench") == 0)
        bench(argc, argv);
    else {
        const int count = argc > 1 ? std::max(1, atoi(argv[1])) : 10000;
        for (uint64_t seed = 1; seed <= 3; ++seed)
            test_random_operations(seed, count);
        test_lobby_scene();
        test_overlay_composition();
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
