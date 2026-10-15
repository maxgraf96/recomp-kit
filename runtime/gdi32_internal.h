// Shared canvas state. Access only under the guest scheduler baton. Guest DIB
// addresses stay in the arena; window surfaces own top-down ARGB host pixels.
#pragma once
#include "gdi_image.h"
#include "imports.h"
#include <array>
#include <map>
namespace gdi {
struct Surface {
    int w = 0, h = 0;
    bool dirty = false;
    std::vector<uint32_t> argb;
};
struct Rect {
    int32_t l = 0, t = 0, r = 0, b = 0;
};
struct DcState {
    uint32_t surface = 0;
    int32_t org_x = 0, org_y = 0, viewport_x = 0, viewport_y = 0;
    int32_t brush_x = 0, brush_y = 0;
    uint32_t pen = 0x4f107, brush = 0x4f100, font = 0x4f10d;
    uint32_t bitmap = 0, palette = 0, region = 0;
    uint32_t text_color = 0, bk_color = 0xffffff;
    int bk_mode = 2, rop2 = 13, stretch_mode = 1;
    uint32_t text_align = 0; // TA_LEFT | TA_TOP | TA_NOUPDATECP
    int arc_direction = 1;   // AD_COUNTERCLOCKWISE; 2 is AD_CLOCKWISE
    int32_t pos_x = 0, pos_y = 0;
    bool clipped = false;
    std::vector<Rect> clip;
};
struct DeviceContext : DcState {
    bool memory = true;
    uint32_t window = 0; // The acquiring window, possibly a child of surface.
    // WS_CLIPCHILDREN: the visible children's areas, in surface coordinates,
    // taken when the DC is made. Writes there are dropped.
    std::vector<Rect> excluded;
    std::vector<DcState> saved;
};
struct Object {
    enum Kind { Brush, Pen, Font, Region } kind = Brush;
    uint32_t color = 0, style = 0;
    int32_t width = 1;
    std::array<uint8_t, 92> logfont{};
    Rect rect{};
};
std::map<uint32_t, DeviceContext> &dcs();
DeviceContext *dc_of(uint32_t dc);
std::map<uint32_t, Object> &objects();
uint32_t make_object(const Object &object);
uint32_t colorref(uint32_t argb); // Swapping R and B is its own inverse.
uint32_t argb(uint32_t color);
bool brush_color(uint32_t brush, uint32_t *pixel);
bool dc_size(uint32_t dc, int *w, int *h);
// Preserve alpha only for 32-bit BI_RGB DIB storage; normal GDI reads are opaque.
bool dc_has_alpha(uint32_t dc);
// A 1-bit DIB is selected into this DC, so blits to or from it convert between
// colour and monochrome rather than matching the nearest palette entry.
bool dc_is_monochrome(uint32_t dc);
bool read_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t *pixel, bool preserve_alpha = false);
// MaskBlt's mask: true with *set written when `bitmap` is a 1-bit bitmap and
// (x, y) is inside it. A set bit selects the foreground raster operation.
bool mask_bit(uint32_t bitmap, int64_t x, int64_t y, bool *set);
bool write_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t pixel, bool blend = false);
Rect clip_box(uint32_t dc);
bool drawable(uint32_t dc, int64_t x, int64_t y);
void fill(uint32_t dc, Rect r, uint32_t pixel);
void line(uint32_t dc, int32_t x, int32_t y, int32_t x1, int32_t y1);
void register_draw();
void register_text();
uint32_t draw_text(uint32_t dc, uint32_t text, uint32_t count, uint32_t rect, uint32_t flags);

// ---------------------------------------------------------------------------
// Fast paths. read_pixel and write_pixel define what a DC reads and writes:
// they find the DC, its bitmap or window, its origin and its clipping again
// for every pixel, which made a 1024x768 BitBlt tens of milliseconds. A Canvas
// is that resolution done once for a whole call, for the storage the fast
// paths model (32-bit BI_RGB DIBs, 1-bit DIBs as sources, window surfaces);
// any other DC keeps the per-pixel path. Every fast path leaves exactly the
// pixels the per-pixel path leaves: gdi_blit_tests compares the two, operation
// by operation. RECOMP_GDI_GENERIC=1 turns the fast paths off.
// ---------------------------------------------------------------------------
bool &fast_paths();
struct Span {
    int64_t l, r; // [l, r)
};
struct Canvas {
    enum Kind { None, Rgb32, Mono, Window } kind = None;
    const DeviceContext *dc = nullptr;
    uint8_t *row0 = nullptr;     // host address of device row 0
    int64_t pitch = 0;           // bytes from a device row to the next; negative bottom-up
    int64_t dx = 0, dy = 0;      // logical (x, y) is device (x + dx, y + dy)
    Rect bounds;                 // the storage's pixels, within a window DC's client area
    Surface *surface = nullptr;  // Window: marked dirty when written
    uint32_t mono[2] = {0, 0};   // Mono: what its two bit values read as, opaque
    const uint8_t *lo = nullptr; // the host bytes the pixels occupy, for aliasing
    const uint8_t *hi = nullptr;
    uint32_t *at(int64_t x, int64_t y) const { // device coordinates; Rgb32 and Window
        return reinterpret_cast<uint32_t *>(row0 + y * pitch) + x;
    }
    bool bit(int64_t x, int64_t y) const { // device coordinates; Mono
        return (row0[y * pitch + x / 8] >> (7 - x % 8)) & 1;
    }
    // Whether read_pixel (write_pixel when `write`) would touch device (x, y).
    bool visible(int64_t x, int64_t y, bool write) const;
    // Whether the fast paths may write it: a window surface, or a 32-bit DIB
    // while no guest-memory watch or DirectDraw lock range needs each store.
    bool writable() const;
};
// False, with kind None, when the DC does not exist; kind None too when its
// storage is not one a Canvas models, or the fast paths are off.
bool resolve(uint32_t hdc, Canvas *out);
// The device pixels of row y in [l, r) that read_pixel (write_pixel when
// `write`) would touch, as sorted disjoint spans.
void spans(const Canvas &c, int64_t y, int64_t l, int64_t r, bool write, std::vector<Span> &out);
// The pixels of a blit's destination row yy in [l, r), in D's logical
// coordinates, that the per-pixel path draws: S has the sample, at device
// (x + kx, yy + ky), and D writes the pixel.
void blit_spans(const Canvas &S, const Canvas &D, int64_t kx, int64_t ky, int64_t yy, int64_t l,
                int64_t r, std::vector<Span> &out);
// read_pixel and write_pixel with the DC resolved once, for loops that visit
// pixels one at a time (lines, outlines, glyphs): the same results, and the
// per-pixel path itself for a DC a Canvas does not model.
class Pixels {
  public:
    explicit Pixels(uint32_t hdc);
    bool read(int64_t x, int64_t y, uint32_t *p);
    bool write(int64_t x, int64_t y, uint32_t p, bool blend = false);

  private:
    uint32_t hdc_;
    Canvas c_;
};
} // namespace gdi
uint32_t gdi_window_dc(uint32_t window);
bool gdi_release_window_dc(uint32_t window, uint32_t dc);
void gdi_destroy_window(uint32_t window);
// Borrowed DirectDraw memory is never freed by GDI. The owning surface pairs
// bind/unbind around its DC lifetime and publishes changes on ReleaseDC.
void gdi_bind_surface_dc(uint32_t dc, int w, int h, int bpp, uint32_t pitch, uint32_t bits,
                         const uint32_t *palette);
void gdi_unbind_surface_dc(uint32_t dc);
namespace gdi {
Rect to_device(uint32_t dc, Rect rect);
}

uint32_t gdi_new_dc();

// Called only with the guest baton. A present owns the composed pixel copy.
void gdi_present_windows(bool refresh = false);
void gdi_composite_windows(uint32_t *argb, int w, int h);
