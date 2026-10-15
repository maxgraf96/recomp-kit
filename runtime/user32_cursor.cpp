// user32_cursor.cpp - the pointer a program asks for, and what a host reads
// to show it.
//
// A program loads stock cursors (LoadCursor with no module and an IDC_*
// number), builds its own from AND/XOR planes (CreateCursor) or from bitmaps
// (CreateIconIndirect, hot spot kept), picks one with SetCursor (NULL hides
// the pointer), counts its visibility with ShowCursor and moves the pointer
// with SetCursorPos. The runtime keeps each shape's pixels; a host reads the
// current choice (host_guest_cursor_state), the shape of a handle
// (host_guest_cursor_shape) and the moves it has not made yet
// (host_take_cursor_warp). RECOMP_TRACE_CURSOR=1 logs every change, which is
// how a headless run records what the pointer looked like.
#include "gdi_image.h"
#include "imports.h"
#include "user32_internal.h"
#include "win32.h"
#include "../platform/os.h"

#include <map>
#include <mutex>
#include <string>

namespace user32 {
extern int32_t g_cursor_show;
}

namespace {

using namespace user32;

// Stock cursors: one handle per IDC_* number, 0x2a000 plus its low 12 bits
// (IDC_ARROW 32512 is 0x2af00). Created cursors and icons count up from
// 0x2c000 and are never reused.
const uint32_t kStockBase = 0x0002a000u;
uint32_t g_next_cursor = 0x0002c000u;

struct Shape {
    int32_t width = 0, height = 0, hot_x = 0, hot_y = 0;
    std::vector<uint32_t> argb; // straight alpha
};
struct CursorStore {
    std::mutex lock; // the host reads shapes on its own thread
    std::map<uint32_t, Shape> shapes;
    bool warp = false;
    int32_t warp_x = 0, warp_y = 0;
};
CursorStore &store() {
    static CursorStore s;
    return s;
}

uint32_t stock_id(uint32_t handle) {
    return (handle & ~0xfffu) == kStockBase && (handle & 0xfff) >= 0xf00 ? 0x7000 | (handle & 0xfff)
                                                                         : 0;
}

const char *stock_name(uint32_t id) {
    switch (id) {
    case 32512:
        return "arrow";
    case 32513:
        return "ibeam";
    case 32514:
        return "wait";
    case 32515:
        return "cross";
    case 32516:
        return "uparrow";
    case 32640:
    case 32646:
        return "sizeall";
    case 32642:
        return "sizenwse";
    case 32643:
        return "sizenesw";
    case 32644:
        return "sizewe";
    case 32645:
        return "sizens";
    case 32648:
        return "no";
    case 32649:
        return "hand";
    case 32650:
        return "appstarting";
    case 32651:
        return "help";
    default:
        return "stock";
    }
}

// What the pointer looks like now, in words: the trace's line and the
// headless record.
std::string describe_current() {
    if (g_cursor_show < 0)
        return "hidden (ShowCursor " + std::to_string(g_cursor_show) + ")";
    if (!g_cursor_chosen)
        return "the platform's arrow (none chosen)";
    if (!g_cursor)
        return "hidden (SetCursor NULL)";
    char buf[96];
    if (uint32_t id = stock_id(g_cursor)) {
        snprintf(buf, sizeof buf, "%s (IDC %u, %08x)", stock_name(id), id, g_cursor);
        return buf;
    }
    std::lock_guard<std::mutex> guard(store().lock);
    auto it = store().shapes.find(g_cursor);
    if (it == store().shapes.end()) {
        snprintf(buf, sizeof buf, "%08x (no picture)", g_cursor);
        return buf;
    }
    snprintf(buf, sizeof buf, "%08x %dx%d hot %d,%d", g_cursor, it->second.width, it->second.height,
             it->second.hot_x, it->second.hot_y);
    return buf;
}

void trace(const char *what) {
    if (recomp_env("TRACE_CURSOR"))
        LOGW("cursor: %s -> %s", what, describe_current().c_str());
}

// A monochrome cursor from its AND and XOR planes, `stride` bytes a row:
// AND 1 XOR 0 is transparent, AND 0 the XOR colour (black or white), and
// AND 1 XOR 1, Windows' inverted screen, black (a colour cursor cannot invert).
Shape from_planes(const uint8_t *and_plane, const uint8_t *xor_plane, int32_t w, int32_t h,
                  int32_t stride) {
    Shape s;
    s.width = w;
    s.height = h;
    s.argb.resize(size_t(w) * size_t(h));
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            const bool a = (and_plane[y * stride + x / 8] >> (7 - x % 8)) & 1;
            const bool o = (xor_plane[y * stride + x / 8] >> (7 - x % 8)) & 1;
            s.argb[size_t(y) * size_t(w) + size_t(x)] =
                a && !o ? 0u : (!a && o ? 0xffffffffu : 0xff000000u);
        }
    return s;
}

// A 1-bpp bitmap's bits as planes: what gdi_read_bitmap reads back as white
// is a set bit.
std::vector<uint8_t> plane_bits(const GdiImage &image, int32_t y0, int32_t h, int32_t stride) {
    std::vector<uint8_t> bits(size_t(stride) * size_t(h));
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < image.width; ++x)
            if (image.pixels[size_t(y0 + y) * size_t(image.width) + size_t(x)] & 0xffffff)
                bits[size_t(y) * size_t(stride) + size_t(x / 8)] |= uint8_t(0x80 >> (x % 8));
    return bits;
}

// LoadCursorA/W: a stock cursor by IDC_* number. A module's own cursor
// resources are not read: its handle is a distinct number with no picture,
// which the host shows as the arrow.
void load_cursor(X86 *c) {
    set_eax(c, kStockBase + (arg(c, 1) & 0xfffu));
}

// CreateCursor(hinst, xHotSpot, yHotSpot, nWidth, nHeight, pvANDPlane,
// pvXORPlane): rows of WORD-aligned bits, as Windows reads them.
void create_cursor(X86 *c) {
    const int32_t hot_x = int32_t(arg(c, 1)), hot_y = int32_t(arg(c, 2));
    const int32_t w = int32_t(arg(c, 3)), h = int32_t(arg(c, 4));
    const uint32_t and_plane = arg(c, 5), xor_plane = arg(c, 6);
    const int32_t stride = ((w + 15) / 16) * 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !and_plane || !xor_plane ||
        !gm_valid(and_plane, uint32_t(stride * h)) || !gm_valid(xor_plane, uint32_t(stride * h))) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    Shape s = from_planes(g_mem + and_plane, g_mem + xor_plane, w, h, stride);
    s.hot_x = hot_x;
    s.hot_y = hot_y;
    const uint32_t handle = g_next_cursor++;
    std::lock_guard<std::mutex> guard(store().lock);
    store().shapes[handle] = std::move(s);
    set_eax(c, handle);
}

// CreateIconIndirect(ICONINFO *): fIcon, xHotspot, yHotspot, hbmMask,
// hbmColor. With a colour bitmap, the mask's set bits are transparent, unless
// the bitmap carries alpha of its own, which then decides; without one, the
// mask is twice as tall, AND above XOR. An icon's hot spot is its centre. The
// handle also names the picture in GDI's icon store (DrawIcon, GetIconInfo).
void create_icon_indirect(X86 *c) {
    const uint32_t info = arg(c, 0);
    if (!info || !gm_valid(info, 20)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    const bool icon = rd32(info) != 0;
    const uint32_t mask_bitmap = rd32(info + 12), color_bitmap = rd32(info + 16);
    GdiImage color, mask;
    const bool has_mask = mask_bitmap && gdi_read_bitmap(mask_bitmap, &mask);
    Shape s;
    GdiImage store_image; // what GDI's icon store keeps: masked pixels are 0
    if (color_bitmap && gdi_read_bitmap(color_bitmap, &color, true)) {
        s.width = color.width;
        s.height = color.height;
        s.argb.resize(color.pixels.size());
        bool alpha = false;
        for (uint32_t p : color.pixels)
            alpha = alpha || (p >> 24) != 0;
        const bool use_mask =
            !alpha && has_mask && mask.width >= color.width && mask.height >= color.height;
        store_image = color;
        for (int32_t y = 0; y < color.height; ++y)
            for (int32_t x = 0; x < color.width; ++x) {
                const size_t i = size_t(y) * size_t(color.width) + size_t(x);
                const bool clear =
                    use_mask &&
                    (mask.pixels[size_t(y) * size_t(mask.width) + size_t(x)] & 0xffffff);
                const uint32_t p = color.pixels[i];
                s.argb[i] = clear ? 0u : (alpha ? p : (p | 0xff000000u));
                store_image.pixels[i] = clear ? 0u : (p & 0x00ffffffu);
            }
    } else if (has_mask && mask.height >= 2 && mask.width > 0) {
        const int32_t h = mask.height / 2, stride = ((mask.width + 15) / 16) * 2;
        const std::vector<uint8_t> and_bits = plane_bits(mask, 0, h, stride);
        const std::vector<uint8_t> xor_bits = plane_bits(mask, h, h, stride);
        s = from_planes(and_bits.data(), xor_bits.data(), mask.width, h, stride);
        store_image.width = s.width;
        store_image.height = s.height;
        store_image.pixels = s.argb;
        for (uint32_t &p : store_image.pixels)
            p &= 0x00ffffffu;
    } else {
        set_eax(c, g_next_cursor++); // no picture GDI can read: a handle all the same
        return;
    }
    s.hot_x = icon ? s.width / 2 : int32_t(rd32(info + 4));
    s.hot_y = icon ? s.height / 2 : int32_t(rd32(info + 8));
    const uint32_t handle = gdi_create_icon(store_image);
    if (!handle) {
        set_eax(c, 0);
        return;
    }
    std::lock_guard<std::mutex> guard(store().lock);
    store().shapes[handle] = std::move(s);
    set_eax(c, handle);
}

// DestroyCursor and DestroyIcon. The current cursor stays chosen; with its
// picture gone the host shows the arrow.
void destroy_cursor(X86 *c) {
    const uint32_t handle = arg(c, 0);
    gdi_delete_icon(handle);
    {
        std::lock_guard<std::mutex> guard(store().lock);
        store().shapes.erase(handle);
    }
    set_eax(c, handle != 0);
}

void set_cursor(X86 *c) {
    const uint32_t prev = g_cursor;
    g_cursor = arg(c, 0);
    const bool changed = prev != g_cursor || !g_cursor_chosen;
    g_cursor_chosen = true;
    if (changed)
        trace("SetCursor");
    set_eax(c, prev);
}

void show_cursor(X86 *c) {
    g_cursor_show += arg(c, 0) ? 1 : -1;
    trace(arg(c, 0) ? "ShowCursor(TRUE)" : "ShowCursor(FALSE)");
    set_eax(c, uint32_t(g_cursor_show));
}

// SetCursorPos(x, y) in screen pixels: GetCursorPos answers it at once, and
// the host moves the platform's pointer there when that is its to move.
void set_cursor_pos(X86 *c) {
    g_cursor_x = int32_t(arg(c, 0));
    g_cursor_y = int32_t(arg(c, 1));
    {
        std::lock_guard<std::mutex> guard(store().lock);
        store().warp = true;
        store().warp_x = g_cursor_x;
        store().warp_y = g_cursor_y;
    }
    if (recomp_env("TRACE_CURSOR"))
        LOGW("cursor: SetCursorPos(%d, %d)", g_cursor_x, g_cursor_y);
    set_eax(c, 1);
}

const ImportShim kCursorShims[] = {
    {"USER32.dll", "LoadCursorA", 2, load_cursor},
    {"USER32.dll", "CreateCursor", 7, create_cursor},
    {"USER32.dll", "CreateIconIndirect", 1, create_icon_indirect},
    {"USER32.dll", "DestroyCursor", 1, destroy_cursor},
    {"USER32.dll", "DestroyIcon", 1, destroy_cursor},
    {"USER32.dll", "SetCursor", 1, set_cursor},
    {"USER32.dll", "ShowCursor", 1, show_cursor},
    {"USER32.dll", "SetCursorPos", 2, set_cursor_pos},
};

} // namespace

void user32_cursor_register() {
    imports_register(kCursorShims, sizeof kCursorShims / sizeof kCursorShims[0]);
}

void user32::cursor_class_chosen(uint32_t cursor) {
    const bool changed = g_cursor != cursor || !g_cursor_chosen;
    g_cursor = cursor;
    g_cursor_chosen = true;
    if (changed)
        trace("WM_SETCURSOR's class cursor");
}

HostCursorState host_guest_cursor_state() {
    HostCursorState s;
    s.handle = g_cursor;
    s.chosen = g_cursor_chosen;
    s.hidden = host_guest_pointer_hidden();
    s.stock = stock_id(g_cursor);
    return s;
}

bool host_guest_cursor_shape(uint32_t handle, HostCursorShape *out) {
    *out = HostCursorShape{};
    if (uint32_t id = stock_id(handle)) {
        out->stock = id;
        return true;
    }
    std::lock_guard<std::mutex> guard(store().lock);
    auto it = store().shapes.find(handle);
    if (it == store().shapes.end())
        return false;
    out->width = it->second.width;
    out->height = it->second.height;
    out->hot_x = it->second.hot_x;
    out->hot_y = it->second.hot_y;
    out->pixels = it->second.argb;
    return true;
}

std::string host_guest_cursor_description() {
    return describe_current();
}

bool host_take_cursor_warp(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> guard(store().lock);
    if (!store().warp)
        return false;
    store().warp = false;
    // Screen pixels to the main window's client pixels: the host's input space.
    int32_t ox = 0, oy = 0;
    if (uint32_t main = host_main_window())
        user32::client_origin(main, &ox, &oy);
    *x = store().warp_x - ox;
    *y = store().warp_y - oy;
    return true;
}
