// ddraw7.cpp - IDirectDraw7 and IDirectDrawSurface7.
//
// The surface model and its coherence rules are described at the top of
// d3d7_internal.h. In short: a surface's pixels live on the backend (the GPU
// renderer, or the software reference renderer) once anything draws with them,
// and in guest memory once the guest locks them; each side is copied from the
// other only when the other is the current one.
//
// Presentation: Flip EXCHANGES the buffers, as Windows does: the back buffer's
// pixels become the front buffer's and are shown, and the back buffer then
// holds what the front buffer held, the frame before. Nothing is copied: the
// two surfaces trade their guest memory and their backend resources. A program
// that keeps one save-under per buffer (Empire Earth's menu cursor restores,
// on each buffer, what it saved on that buffer two frames before) needs this;
// with a copy every restore would land one frame late and leave a trail.
// A Blt whose destination is the primary surface shows the destination
// rectangle, which is the windowed present.
#include "d3d7_internal.h"
#include "host_api.h"
#include "../runtime/display_seam.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"
#include "game_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// The GDI device context over a surface's memory (runtime/gdi32.cpp).
uint32_t gdi_new_dc();
void gdi_bind_surface_dc(uint32_t dc, int w, int h, int bpp, uint32_t pitch, uint32_t bits,
                         const uint32_t *palette);
void gdi_unbind_surface_dc(uint32_t dc);
// The mode USER32 and GDI report as the screen (ddraw.cpp).
void ddraw_note_mode_impl(uint32_t w, uint32_t h, uint32_t bpp);

namespace d7 {

// ===========================================================================
// Pixel formats
// ===========================================================================
int mask_shift(uint32_t mask) {
    return mask ? __builtin_ctz(mask) : 0;
}
int mask_bits(uint32_t mask) {
    return mask ? __builtin_popcount(mask) : 0;
}

namespace {
// A channel of `bits` bits widened to 8, rounding to nearest.
inline uint32_t widen(uint32_t v, int bits) {
    if (bits <= 0)
        return 0;
    if (bits >= 8)
        return (v >> (bits - 8)) & 0xff;
    const uint32_t max = (1u << bits) - 1;
    return (v * 255 + max / 2) / max;
}
// And narrowed back from 8.
inline uint32_t narrow(uint32_t v8, int bits) {
    if (bits <= 0)
        return 0;
    if (bits >= 8)
        return v8 << (bits - 8);
    const uint32_t max = (1u << bits) - 1;
    return (v8 * max + 127) / 255;
}
inline uint32_t read_px(const uint8_t *p, uint32_t bytes) {
    switch (bytes) {
    case 1:
        return p[0];
    case 2:
        return uint32_t(p[0]) | uint32_t(p[1]) << 8;
    case 3:
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    default:
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
}
inline void write_px(uint8_t *p, uint32_t bytes, uint32_t v) {
    for (uint32_t i = 0; i < bytes; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
bool standard_8888(const Format &f) {
    return f.bpp == 32 && f.r == 0xff0000 && f.g == 0xff00 && f.b == 0xff;
}
} // namespace

Format read_format(uint32_t a) {
    Format f;
    if (!a || !gm_valid(a, DDPF_SIZE_))
        return f;
    f.flags = rd32(a + DDPF_dwFlags);
    f.fourcc = rd32(a + DDPF_dwFourCC);
    f.bpp = rd32(a + DDPF_dwRGBBitCount);
    f.r = rd32(a + DDPF_dwRBitMask);
    f.g = rd32(a + DDPF_dwGBitMask);
    f.b = rd32(a + DDPF_dwBBitMask);
    f.a = rd32(a + DDPF_dwRGBAlphaBitMask);
    if (!(f.flags & DDPF_ALPHAPIXELS))
        f.a = 0;
    return f;
}
void write_format(uint32_t a, const Format &f) {
    if (!a || !gm_valid(a, DDPF_SIZE_))
        return;
    wr32(a + DDPF_dwSize, DDPF_SIZE_);
    wr32(a + DDPF_dwFlags, f.flags);
    wr32(a + DDPF_dwFourCC, f.fourcc);
    wr32(a + DDPF_dwRGBBitCount, f.bpp);
    wr32(a + DDPF_dwRBitMask, f.r);
    wr32(a + DDPF_dwGBitMask, f.g);
    wr32(a + DDPF_dwBBitMask, f.b);
    wr32(a + DDPF_dwRGBAlphaBitMask, f.a);
}
bool format_supported(const Format &f) {
    if (f.depth())
        return f.bpp == 16 || f.bpp == 24 || f.bpp == 32;
    if (!f.rgb() ||
        (f.flags & (DDPF_FOURCC | DDPF_PALETTEINDEXED8 | DDPF_LUMINANCE | DDPF_BUMPDUDV)))
        return false;
    if (f.bpp != 16 && f.bpp != 32)
        return false;
    // Each mask must be one contiguous run inside the pixel.
    for (uint32_t m : {f.r, f.g, f.b, f.a}) {
        if (!m)
            continue;
        uint32_t run = m >> mask_shift(m);
        if (run & (run + 1))
            return false;
        if (f.bpp < 32 && (m >> f.bpp))
            return false;
    }
    return f.r && f.g && f.b;
}
Format format_x8r8g8b8() {
    Format f;
    f.flags = DDPF_RGB;
    f.bpp = 32;
    f.r = 0xff0000;
    f.g = 0xff00;
    f.b = 0xff;
    return f;
}
Format format_a8r8g8b8() {
    Format f = format_x8r8g8b8();
    f.flags |= DDPF_ALPHAPIXELS;
    f.a = 0xff000000u;
    return f;
}
Format format_r5g6b5() {
    Format f;
    f.flags = DDPF_RGB;
    f.bpp = 16;
    f.r = 0xf800;
    f.g = 0x07e0;
    f.b = 0x001f;
    return f;
}
Format format_depth(uint32_t bits) {
    Format f;
    f.flags = DDPF_ZBUFFER;
    f.bpp = bits == 24 ? 32 : bits;
    f.g = bits >= 32 ? 0xffffffffu : (1u << bits) - 1;
    return f;
}

uint32_t pixel_to_bgra(const Format &f, uint32_t p) {
    const uint32_t r = widen((p & f.r) >> mask_shift(f.r), mask_bits(f.r));
    const uint32_t g = widen((p & f.g) >> mask_shift(f.g), mask_bits(f.g));
    const uint32_t b = widen((p & f.b) >> mask_shift(f.b), mask_bits(f.b));
    const uint32_t a = f.has_alpha() ? widen((p & f.a) >> mask_shift(f.a), mask_bits(f.a)) : 255;
    return b | g << 8 | r << 16 | a << 24;
}
uint32_t bgra_to_pixel(const Format &f, uint32_t c) {
    uint32_t p = 0;
    p |= (narrow((c >> 16) & 0xff, mask_bits(f.r)) << mask_shift(f.r)) & f.r;
    p |= (narrow((c >> 8) & 0xff, mask_bits(f.g)) << mask_shift(f.g)) & f.g;
    p |= (narrow(c & 0xff, mask_bits(f.b)) << mask_shift(f.b)) & f.b;
    if (f.has_alpha())
        p |= (narrow(c >> 24, mask_bits(f.a)) << mask_shift(f.a)) & f.a;
    return p;
}
void row_to_bgra(const Format &f, const uint8_t *src, uint32_t *dst, int n) {
    if (standard_8888(f)) {
        memcpy(dst, src, size_t(n) * 4);
        if (!f.has_alpha())
            for (int i = 0; i < n; ++i)
                dst[i] |= 0xff000000u;
        return;
    }
    const uint32_t bytes = f.bytes();
    for (int i = 0; i < n; ++i)
        dst[i] = pixel_to_bgra(f, read_px(src + i * bytes, bytes));
}
void row_from_bgra(const Format &f, const uint32_t *src, uint8_t *dst, int n) {
    if (standard_8888(f)) {
        memcpy(dst, src, size_t(n) * 4);
        return;
    }
    const uint32_t bytes = f.bytes();
    for (int i = 0; i < n; ++i)
        write_px(dst + i * bytes, bytes, bgra_to_pixel(f, src[i]));
}
HostD7Key make_key(const Format &f, uint32_t lo, uint32_t hi) {
    HostD7Key k;
    memset(&k, 0, sizeof k);
    k.enabled = 1;
    const uint32_t masks[3] = {f.r, f.g, f.b};
    for (int i = 0; i < 3; ++i) {
        const int bits = std::min(mask_bits(masks[i]), 8);
        const int drop = mask_bits(masks[i]) - bits;
        k.bits[i] = uint8_t(bits);
        k.lo[i] = uint8_t(((lo & masks[i]) >> mask_shift(masks[i])) >> drop);
        k.hi[i] = uint8_t(((hi & masks[i]) >> mask_shift(masks[i])) >> drop);
    }
    return k;
}
bool key_matches(const Format &f, uint32_t p, uint32_t lo, uint32_t hi) {
    for (uint32_t m : {f.r, f.g, f.b}) {
        const uint32_t v = p & m, l = lo & m, h = hi & m;
        if (v < l || v > h)
            return false;
    }
    return true;
}

// ===========================================================================
// Backends
// ===========================================================================
namespace {
int seam_available() {
    return host_d7_available();
}
uint32_t seam_generation() {
    return host_d7_generation();
}
const Backend g_seam = {
    "gpu",          seam_available,   seam_generation, host_d7_create,     host_d7_destroy,
    host_d7_upload, host_d7_readback, host_d7_fill,    host_d7_fill_depth, host_d7_blit,
    host_d7_swap,   host_d7_draw,     host_d7_present, host_d7_flush};
int g_soft_choice = -1; // -1: read RECOMP_D3D7_SOFTWARE once
} // namespace

const Backend *seam_backend() {
    return &g_seam;
}
void use_soft_backend(bool on) {
    g_soft_choice = on ? 1 : 0;
}
const Backend *backend() {
    if (g_soft_choice < 0) {
        const char *v = recomp_env("D3D7_SOFTWARE");
        g_soft_choice = v && *v && strcmp(v, "0") != 0;
    }
    if (g_soft_choice > 0)
        return soft_backend();
    return host_d7_available() ? &g_seam : nullptr;
}

// ===========================================================================
// Surfaces
// ===========================================================================
namespace {
std::unordered_map<uint32_t, std::unique_ptr<Surface>> &surfaces() {
    static auto *m = new std::unordered_map<uint32_t, std::unique_ptr<Surface>>();
    return *m;
}
// Which backend made each root's resource, so a switch between backends is
// seen as a lost resource rather than a matching generation number.
std::unordered_map<uint32_t, const Backend *> &resource_owner() {
    static auto *m = new std::unordered_map<uint32_t, const Backend *>();
    return *m;
}
DisplayState g_display;
} // namespace

DisplayState &display_state() {
    return g_display;
}

Surface *surface(uint32_t id) {
    auto it = surfaces().find(id);
    return it == surfaces().end() ? nullptr : it->second.get();
}
Surface *surface_of(ComObj *o) {
    return o && o->kind == K_SURFACE7 ? surface(o->id) : nullptr;
}
Surface *surface_from_guest(uint32_t p) {
    return surface_of(com_this(p, IF_DDSURFACE7));
}
uint32_t surface_view(Surface *s) {
    return s ? com_view(com_get(s->id), IF_DDSURFACE7) : 0;
}

uint32_t surface_memory(Surface *s) {
    if (s->mem)
        return s->mem;
    const uint64_t bytes = uint64_t(s->pitch) * s->height;
    if (!bytes || bytes > 0x10000000u)
        return 0;
    s->mem = heap_alloc(uint32_t(bytes), true, 16);
    if (!s->mem)
        LOGW("ddraw7: no guest memory for a %ux%u surface", s->width, s->height);
    return s->mem;
}

bool surface_on_backend(const Surface *s) {
    return backend() && !(s->caps & DDSCAPS_SYSTEMMEMORY);
}

namespace {
std::vector<Surface *> levels_of(Surface *root) {
    std::vector<Surface *> out{root};
    if (root->caps & DDSCAPS_MIPMAP)
        for (uint32_t id : root->chain)
            if (Surface *l = surface(id))
                out.push_back(l);
    return out;
}
Surface *root_of(Surface *s) {
    Surface *r = surface(s->root);
    return r ? r : s;
}
// RECOMP_D3D7_TRANSFER_LOG=1 names every readback and what asked for it, and
// every upload and what wrote the guest copy: a readback waits for the GPU to
// finish what it was given and an upload copies the whole level, so a game
// that does either to a surface every frame pays it every frame.
char g_readback_via[96] = "something else";
struct ReadbackVia {
    explicit ReadbackVia(const char *what) {
        snprintf(g_readback_via, sizeof g_readback_via, "%s", what);
    }
    ~ReadbackVia() {
        snprintf(g_readback_via, sizeof g_readback_via, "something else");
    }
};
bool transfer_log() {
    static const bool on = [] {
        const char *v = recomp_env("D3D7_TRANSFER_LOG");
        return v && *v && strcmp(v, "0") != 0;
    }();
    return on;
}
// Reads level `s` from the backend into its guest memory.
bool read_back(const Backend *b, Surface *s) {
    if (!surface_memory(s))
        return false;
    if (transfer_log())
        LOGW("ddraw7: readback of surface %u (%ux%u, %u bits, caps %08x, level %d) for %s", s->id,
             s->width, s->height, s->format.bpp, s->caps, s->level, g_readback_via);
    if (s->format.depth()) {
        log_once("ddraw7.depthlock", "ddraw7: depth buffer contents are not readable; zeros");
        memset(gm_ptr(s->mem), 0, size_t(s->pitch) * s->height);
        return true;
    }
    HostD7Rect r{0, 0, int32_t(s->width), int32_t(s->height)};
    // A 32-bit BGRA surface has the backend's own layout (row_from_bgra is a
    // copy of each row): read it straight into guest memory, rows at the
    // surface's pitch, which a back buffer's GetDC does several times a second.
    if (standard_8888(s->format) && s->pitch >= s->width * 4)
        return b->readback(root_of(s)->id, s->level, &r, gm_ptr(s->mem), int(s->pitch));
    std::vector<uint32_t> bgra(size_t(s->width) * s->height);
    if (!b->readback(root_of(s)->id, s->level, &r, bgra.data(), int(s->width * 4)))
        return false;
    for (uint32_t y = 0; y < s->height; ++y)
        row_from_bgra(s->format, bgra.data() + size_t(y) * s->width, gm_ptr(s->mem + y * s->pitch),
                      int(s->width));
    return true;
}
void upload(const Backend *b, Surface *s) {
    if (transfer_log())
        LOGW("ddraw7: upload of surface %u (%ux%u, %u bits, caps %08x, level %d) written by %s",
             s->id, s->width, s->height, s->format.bpp, s->caps, s->level, s->cpu_writer);
    if (s->format.depth()) {
        log_once("ddraw7.depthupload", "ddraw7: a depth buffer written by the CPU is not uploaded");
        return;
    }
    HostD7Rect r{0, 0, int32_t(s->width), int32_t(s->height)};
    // 32-bit BGRA goes up from guest memory as it is: row_to_bgra would only
    // copy it and, without an alpha channel, set the alpha byte, which every
    // backend does itself for the opaque resource ensure_resource made.
    if (s->mem && standard_8888(s->format) && s->pitch >= s->width * 4) {
        b->upload(root_of(s)->id, s->level, &r, gm_ptr(s->mem), int(s->pitch));
        return;
    }
    std::vector<uint32_t> bgra(size_t(s->width) * s->height);
    if (s->mem)
        for (uint32_t y = 0; y < s->height; ++y)
            row_to_bgra(s->format, gm_ptr(s->mem + y * s->pitch),
                        bgra.data() + size_t(y) * s->width, int(s->width));
    b->upload(root_of(s)->id, s->level, &r, bgra.data(), int(s->width * 4));
}
// The root's backend resource exists under the current generation. Levels whose
// content was only on a lost resource fall back to their guest copies.
bool ensure_resource(const Backend *b, Surface *root) {
    const uint32_t gen = b->generation();
    auto owner = resource_owner().find(root->id);
    if (root->resource_generation == gen && owner != resource_owner().end() && owner->second == b)
        return true;
    const uint32_t flags = root->format.depth()        ? HOST_D7_CREATE_DEPTH
                           : !root->format.has_alpha() ? HOST_D7_CREATE_OPAQUE
                                                       : 0;
    if (!b->create(root->id, int(root->width), int(root->height), root->levels, flags))
        return false;
    resource_owner()[root->id] = b;
    root->resource_generation = gen;
    for (Surface *l : levels_of(root)) {
        if (!l->cpu_valid) {
            log_once("ddraw7.lost", "ddraw7: the renderer was reset; surface contents were lost");
            l->cpu_valid = true;
        }
        // Untouched guest memory is zero, and so is a new resource.
        l->gpu_valid = l->mem == 0;
    }
    return true;
}
} // namespace

bool surface_cpu_current(Surface *s) {
    if (!surface_memory(s))
        return false;
    if (s->cpu_valid)
        return true;
    const Backend *b = backend();
    Surface *root = root_of(s);
    if (b && root->resource_generation == b->generation() && read_back(b, s)) {
        s->cpu_valid = true;
        return true;
    }
    log_once("ddraw7.readback", "ddraw7: a surface's rendered contents could not be read back");
    s->cpu_valid = true;
    return true;
}
void surface_cpu_wrote(Surface *s) {
    s->cpu_valid = true;
    s->gpu_valid = false;
    ++s->uniqueness;
}
bool surface_backend_current(Surface *s) {
    const Backend *b = backend();
    if (!b)
        return false;
    Surface *root = root_of(s);
    if (!ensure_resource(b, root))
        return false;
    for (Surface *l : levels_of(root)) {
        if (l->gpu_valid)
            continue;
        upload(b, l);
        l->gpu_valid = true;
    }
    return true;
}
void surface_backend_wrote(Surface *s) {
    s->gpu_valid = true;
    s->cpu_valid = false;
    ++s->uniqueness;
}

// ===========================================================================
// Display modes
// ===========================================================================
namespace {
struct Mode {
    uint32_t w, h, bpp;
};
std::vector<Mode> &mode_table() {
    static auto *v = new std::vector<Mode>();
    return *v;
}
bool parse_modes(const char *spec, std::vector<Mode> *out) {
    out->clear();
    const char *p = spec;
    while (*p) {
        unsigned w = 0, h = 0, bpp = 0;
        int used = 0;
        if (sscanf(p, "%ux%ux%u%n", &w, &h, &bpp, &used) != 3 || w == 0 || h == 0 || w > 16384 ||
            h > 16384 || (bpp != 16 && bpp != 32))
            return false;
        out->push_back({w, h, bpp});
        p += used;
        if (*p == ',')
            ++p;
        else if (*p)
            return false;
    }
    return !out->empty();
}
const std::vector<Mode> &modes() {
    auto &t = mode_table();
    if (!t.empty())
        return t;
    if (const char *spec = recomp_env("D3D7_MODES"))
        if (!parse_modes(spec, &t))
            LOGW("ddraw7: RECOMP_D3D7_MODES=\"%s\" is malformed; using the built-in modes", spec);
    if (t.empty()) {
        static const uint32_t sizes[][2] = {
            {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720}, {1280, 800},
            {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900}, {1600, 1200},
            {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}};
        int32_t sw = 0, sh = 0;
        const bool screen = host_display_screen(&sw, &sh) && sw > 0 && sh > 0;
        for (uint32_t bpp : {16u, 32u}) {
            for (auto &s : sizes)
                if (!screen || (s[0] <= uint32_t(sw) && s[1] <= uint32_t(sh)) || s[0] <= 1024)
                    t.push_back({s[0], s[1], bpp});
            if (screen && std::none_of(t.begin(), t.end(), [&](const Mode &m) {
                    return m.w == uint32_t(sw) && m.h == uint32_t(sh) && m.bpp == bpp;
                }))
                t.push_back({uint32_t(sw), uint32_t(sh), bpp});
        }
    }
    return t;
}
// What the desktop is in before any SetDisplayMode: the runtime's screen.
Mode desktop_mode() {
    uint32_t w = 1024, h = 768, bpp = 32;
    win32_display_mode(&w, &h, &bpp);
    if (bpp != 16 && bpp != 32)
        bpp = 32;
    return {w, h, bpp};
}
Mode current_mode() {
    if (g_display.mode_set)
        return {g_display.w, g_display.h, g_display.bpp};
    return desktop_mode();
}
} // namespace

Format display_format() {
    return current_mode().bpp == 16 ? format_r5g6b5() : format_x8r8g8b8();
}

// ===========================================================================
// Guest structure helpers
// ===========================================================================
namespace {
uint32_t pitch_for(uint32_t w, const Format &f) {
    return (w * f.bytes() + 3) & ~3u;
}
bool read_rect(uint32_t addr, const Surface *s, HostD7Rect *r) {
    if (!addr) {
        *r = {0, 0, int32_t(s->width), int32_t(s->height)};
        return true;
    }
    if (!gm_valid(addr, 16))
        return false;
    *r = {int32_t(rd32(addr)), int32_t(rd32(addr + 4)), int32_t(rd32(addr + 8)),
          int32_t(rd32(addr + 12))};
    return r->x0 < r->x1 && r->y0 < r->y1;
}
void write_desc(uint32_t a, Surface *s, uint32_t lp_surface) {
    gm_zero(a, DDSD2_SIZE);
    uint32_t flags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    wr32(a + DDSD2_dwSize, DDSD2_SIZE);
    wr32(a + DDSD2_dwWidth, s->width);
    wr32(a + DDSD2_dwHeight, s->height);
    if (!s->format.depth()) {
        flags |= DDSD_PITCH;
        wr32(a + DDSD2_lPitch, s->pitch);
    } else {
        flags |= DDSD_PITCH;
        wr32(a + DDSD2_lPitch, s->pitch);
    }
    if (s->is_root() && (s->caps & DDSCAPS_MIPMAP)) {
        flags |= DDSD_MIPMAPCOUNT;
        wr32(a + DDSD2_dwMipMapCount, uint32_t(s->levels));
    } else if (!s->is_root() && (s->caps & DDSCAPS_MIPMAP)) {
        Surface *root = surface(s->root);
        flags |= DDSD_MIPMAPCOUNT;
        wr32(a + DDSD2_dwMipMapCount, root ? uint32_t(root->levels - s->level) : 1);
    }
    if ((s->caps & DDSCAPS_FLIP) && !s->parent && !s->chain.empty()) {
        flags |= DDSD_BACKBUFFERCOUNT;
        wr32(a + DDSD2_dwBackBufferCount, uint32_t(s->chain.size()));
    }
    if (s->has_src_key) {
        flags |= DDSD_CKSRCBLT;
        wr32(a + DDSD2_ddckCKSrcBlt, s->src_key_lo);
        wr32(a + DDSD2_ddckCKSrcBlt + 4, s->src_key_hi);
    }
    if (s->has_dst_key) {
        flags |= DDSD_CKDESTBLT;
        wr32(a + DDSD2_ddckCKDestBlt, s->dst_key_lo);
        wr32(a + DDSD2_ddckCKDestBlt + 4, s->dst_key_hi);
    }
    if (lp_surface) {
        flags |= DDSD_LPSURFACE;
        wr32(a + DDSD2_lpSurface, lp_surface);
    }
    write_format(a + DDSD2_ddpfPixelFormat, s->format);
    wr32(a + DDSD2_ddsCaps, s->caps);
    wr32(a + DDSD2_ddsCaps + 4, s->caps2);
    wr32(a + DDSD2_dwFlags, flags);
}
void write_mode_desc(uint32_t a, const Mode &m, uint32_t refresh) {
    gm_zero(a, DDSD2_SIZE);
    const Format f = m.bpp == 16 ? format_r5g6b5() : format_x8r8g8b8();
    wr32(a + DDSD2_dwSize, DDSD2_SIZE);
    wr32(a + DDSD2_dwFlags,
         DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    wr32(a + DDSD2_dwWidth, m.w);
    wr32(a + DDSD2_dwHeight, m.h);
    wr32(a + DDSD2_lPitch, pitch_for(m.w, f));
    wr32(a + DDSD2_dwMipMapCount, refresh);
    write_format(a + DDSD2_ddpfPixelFormat, f);
}
} // namespace

// ===========================================================================
// CPU surface operations
// ===========================================================================
namespace {
struct BlitOptions {
    bool key = false;
    uint32_t key_lo = 0, key_hi = 0;
    bool mirror_x = false, mirror_y = false;
};
void cpu_fill(Surface *s, const HostD7Rect &r, uint32_t pixel) {
    ReadbackVia why("a fill on the CPU");
    if (!surface_cpu_current(s))
        return;
    const uint32_t bytes = s->format.bytes();
    for (int32_t y = r.y0; y < r.y1; ++y) {
        uint8_t *row = gm_ptr(s->mem + uint32_t(y) * s->pitch);
        for (int32_t x = r.x0; x < r.x1; ++x)
            write_px(row + uint32_t(x) * bytes, bytes, pixel);
    }
    surface_cpu_wrote(s);
    s->cpu_writer = "a fill on the CPU";
}
// Nearest-neighbour copy/stretch with an optional source key, in guest memory.
// The source pixel for destination pixel x is the one whose centre the
// destination pixel's centre maps onto: floor((x + 0.5) * sw / dw).
void cpu_blit(Surface *d, const HostD7Rect &dr, Surface *s, const HostD7Rect &sr,
              const BlitOptions &o) {
    ReadbackVia why("a Blt on the CPU");
    if (!surface_cpu_current(s) || !surface_cpu_current(d))
        return;
    const int32_t dw = dr.x1 - dr.x0, dh = dr.y1 - dr.y0;
    const int32_t sw = sr.x1 - sr.x0, sh = sr.y1 - sr.y0;
    const uint32_t sb = s->format.bytes(), db = d->format.bytes();
    // A copy of the source region, so an overlapping blit reads the original.
    std::vector<uint8_t> src(size_t(sw) * sh * sb);
    for (int32_t y = 0; y < sh; ++y)
        memcpy(src.data() + size_t(y) * sw * sb,
               gm_ptr(s->mem + uint32_t(sr.y0 + y) * s->pitch + uint32_t(sr.x0) * sb),
               size_t(sw) * sb);
    const bool same = s->format.bpp == d->format.bpp && s->format.r == d->format.r &&
                      s->format.g == d->format.g && s->format.b == d->format.b &&
                      s->format.a == d->format.a;
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = int32_t((int64_t(2 * y + 1) * sh) / (2 * dh));
        if (o.mirror_y)
            sy = sh - 1 - sy;
        uint8_t *row = gm_ptr(d->mem + uint32_t(dr.y0 + y) * d->pitch);
        const uint8_t *srow = src.data() + size_t(sy) * sw * sb;
        if (same && dw == sw && !o.key && !o.mirror_x) {
            memcpy(row + uint32_t(dr.x0) * db, srow, size_t(sw) * sb);
            continue;
        }
        for (int32_t x = 0; x < dw; ++x) {
            int32_t sx = int32_t((int64_t(2 * x + 1) * sw) / (2 * dw));
            if (o.mirror_x)
                sx = sw - 1 - sx;
            const uint32_t p = read_px(srow + size_t(sx) * sb, sb);
            if (o.key && key_matches(s->format, p, o.key_lo, o.key_hi))
                continue;
            const uint32_t out = same ? p : bgra_to_pixel(d->format, pixel_to_bgra(s->format, p));
            write_px(row + uint32_t(dr.x0 + x) * db, db, out);
        }
    }
    surface_cpu_wrote(d);
    d->cpu_writer = "a Blt on the CPU";
}
// Shows region `r` of `s`: on the backend, or as an ARGB snapshot.
void present(Surface *s, const HostD7Rect &r) {
    const Mode m = current_mode();
    const Backend *b = backend();
    if (b && surface_backend_current(s)) {
        b->present(root_of(s)->id, &r, int(m.w), int(m.h));
        return;
    }
    if (!surface_cpu_current(s))
        return;
    const int w = r.x1 - r.x0, h = r.y1 - r.y0;
    std::vector<uint32_t> argb(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        row_to_bgra(
            s->format,
            gm_ptr(s->mem + uint32_t(r.y0 + y) * s->pitch + uint32_t(r.x0) * s->format.bytes()),
            argb.data() + size_t(y) * w, w);
    // BGRA8 bytes are ARGB words on a little-endian host. Layer 0 of the
    // display: the program's GDI windows are composed over it.
    display_present_frame(argb.data(), w, h);
}
// A Blt onto the primary surface of a windowed program is its present. The
// guest aims at its window's client area in desktop coordinates; the host's
// window is that client area, so what is shown is the source rectangle,
// scaled to the window by the presenter.
void present_windowed(Surface *src, HostD7Rect r) {
    r.x0 = std::max(r.x0, 0);
    r.y0 = std::max(r.y0, 0);
    r.x1 = std::min(r.x1, int32_t(src->width));
    r.y1 = std::min(r.y1, int32_t(src->height));
    if (r.x0 < r.x1 && r.y0 < r.y1)
        present(src, r);
}
// A Blt onto the primary surface is a present, and what it shows depends on
// who owns the screen. A windowed program's primary is the desktop, and what
// it blits there is its window: the source rectangle (present_windowed). A
// fullscreen program owns the screen, the primary is the frame, and a Blt
// changes part of it - a loading screen's spinner, a cursor - so the frame is
// the whole primary with that part updated, never the part on its own.
void present_primary_blt(Surface *primary, Surface *src, const HostD7Rect &shown) {
    if (g_display.coop & DDSCL_FULLSCREEN) {
        present(primary, HostD7Rect{0, 0, int32_t(primary->width), int32_t(primary->height)});
        return;
    }
    present_windowed(src, shown);
}
} // namespace

// ===========================================================================
// Surface creation and destruction
// ===========================================================================
namespace {
Surface *new_surface(uint32_t dd, uint32_t caps, uint32_t caps2, const Format &f, uint32_t w,
                     uint32_t h) {
    ComObj *o = com_new(K_SURFACE7);
    auto s = std::make_unique<Surface>();
    s->id = o->id;
    s->root = o->id;
    s->dd = dd;
    s->caps = caps;
    s->caps2 = caps2;
    s->format = f;
    s->width = w;
    s->height = h;
    s->pitch = pitch_for(w, f);
    // The generic fields other shims read (BinkDDSurfaceType).
    o->width = w;
    o->height = h;
    o->bpp = f.bpp;
    o->pitch = s->pitch;
    o->rmask = f.r;
    o->gmask = f.g;
    o->bmask = f.b;
    o->amask = f.a;
    o->caps = caps;
    Surface *raw = s.get();
    surfaces()[o->id] = std::move(s);
    return raw;
}

void release_surface_dc(Surface *s);

void surface_destroy(ComObj *o) {
    auto it = surfaces().find(o->id);
    if (it == surfaces().end())
        return;
    Surface *s = it->second.get();
    if (s->id == g_display.primary) {
        // No frame of the program's is on screen any more: the windows are
        // shown on their own again.
        g_display.primary = 0;
        g_display.gdi_showing = false;
        display_layers_changed();
    }
    if (s->dc)
        release_surface_dc(s);
    if (s->attached_z) {
        Surface *z = surface(s->attached_z);
        s->attached_z = 0;
        if (z) {
            z->attached_to = 0;
            com_release(com_get(z->id));
        }
    }
    if (s->attached_to)
        if (Surface *owner = surface(s->attached_to))
            if (owner->attached_z == s->id)
                owner->attached_z = 0;
    if (s->is_root()) {
        auto owner = resource_owner().find(s->id);
        if (owner != resource_owner().end()) {
            if (s->resource_generation == owner->second->generation())
                owner->second->destroy(s->id);
            resource_owner().erase(owner);
        }
        const std::vector<uint32_t> chain = s->chain;
        s->chain.clear();
        for (uint32_t id : chain)
            if (ComObj *child = com_get(id))
                com_destroy(child);
    }
    if (s->mem)
        heap_free(s->mem);
    surfaces().erase(it);
}

void ddraw7_destroy(ComObj *) {}

// CreateSurface(lpDDSurfaceDesc2, lplpDDSurface, pUnkOuter)
uint32_t create_surface(uint32_t dd, uint32_t desc, Surface **out) {
    if (!desc || !gm_valid(desc, DDSD2_SIZE) || rd32(desc + DDSD2_dwSize) != DDSD2_SIZE)
        return DDERR_INVALIDPARAMS;
    const uint32_t flags = rd32(desc + DDSD2_dwFlags);
    uint32_t caps = (flags & DDSD_CAPS) ? rd32(desc + DDSD2_ddsCaps) : 0;
    uint32_t caps2 = (flags & DDSD_CAPS) ? rd32(desc + DDSD2_ddsCaps + 4) : 0;
    uint32_t w = (flags & DDSD_WIDTH) ? rd32(desc + DDSD2_dwWidth) : 0;
    uint32_t h = (flags & DDSD_HEIGHT) ? rd32(desc + DDSD2_dwHeight) : 0;
    Format f;
    if (flags & DDSD_PIXELFORMAT)
        f = read_format(desc + DDSD2_ddpfPixelFormat);
    else if (caps & DDSCAPS_ZBUFFER)
        f = format_depth((flags & DDSD_ZBUFFERBITDEPTH) ? rd32(desc + DDSD2_dwMipMapCount) : 16);
    else
        f = display_format();
    if (caps & DDSCAPS_ZBUFFER)
        f.flags |= DDPF_ZBUFFER;

    const bool primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    if (primary) {
        const Mode m = current_mode();
        w = m.w;
        h = m.h;
        f = display_format();
    }
    if (!format_supported(f)) {
        log_once("ddraw7.format",
                 "ddraw7: CreateSurface with an unsupported pixel format "
                 "(flags %x, fourcc %08x, %u bpp, masks %x/%x/%x/%x)",
                 f.flags, f.fourcc, f.bpp, f.r, f.g, f.b, f.a);
        return DDERR_INVALIDPIXELFORMAT;
    }
    if (!w || !h || w > 16384 || h > 16384)
        return DDERR_INVALIDPARAMS;
    if (caps & (DDSCAPS_OVERLAY | DDSCAPS_PALETTE))
        return DDERR_INVALIDCAPS;
    if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY)) &&
        !(caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;

    // Mip chain: every level down to 1x1 unless the count says otherwise.
    int levels = 1;
    if ((caps & DDSCAPS_MIPMAP) && (caps & DDSCAPS_TEXTURE)) {
        int full = 1;
        for (uint32_t x = std::max(w, h); x > 1; x >>= 1)
            ++full;
        levels = (flags & DDSD_MIPMAPCOUNT) ? int(rd32(desc + DDSD2_dwMipMapCount)) : full;
        if (levels < 1 || levels > full)
            return DDERR_INVALIDPARAMS;
        if (levels > 1)
            caps |= DDSCAPS_COMPLEX;
    } else {
        caps &= ~DDSCAPS_MIPMAP;
    }
    uint32_t back_count = 0;
    if (primary && (caps & DDSCAPS_FLIP)) {
        back_count = (flags & DDSD_BACKBUFFERCOUNT) ? rd32(desc + DDSD2_dwBackBufferCount) : 0;
        if (back_count < 1 || back_count > 3 || !(caps & DDSCAPS_COMPLEX))
            return DDERR_INVALIDPARAMS;
    }

    uint32_t root_caps = caps;
    if (primary)
        root_caps |= DDSCAPS_VISIBLE | (back_count ? DDSCAPS_FRONTBUFFER : 0);
    Surface *root = new_surface(dd, root_caps, caps2, f, w, h);
    root->levels = levels;
    for (int i = 1; i < levels; ++i) {
        const uint32_t lw = std::max(1u, w >> i), lh = std::max(1u, h >> i);
        Surface *l = new_surface(dd, (caps & ~DDSCAPS_COMPLEX) | DDSCAPS_MIPMAP | DDSCAPS_TEXTURE,
                                 caps2 | DDSCAPS2_MIPMAPSUBLEVEL, f, lw, lh);
        l->root = root->id;
        l->parent = root->id;
        l->level = i;
        l->implicit = true;
        root->chain.push_back(l->id);
    }
    for (uint32_t i = 0; i < back_count; ++i) {
        uint32_t bc = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER)) |
                      (i == 0 ? DDSCAPS_BACKBUFFER : 0);
        Surface *b = new_surface(dd, bc, caps2, f, w, h);
        b->parent = root->id;
        b->implicit = true;
        root->chain.push_back(b->id);
    }
    if (flags & DDSD_CKSRCBLT) {
        root->has_src_key = true;
        root->src_key_lo = rd32(desc + DDSD2_ddckCKSrcBlt);
        root->src_key_hi = rd32(desc + DDSD2_ddckCKSrcBlt + 4);
    }
    if (flags & DDSD_CKDESTBLT) {
        root->has_dst_key = true;
        root->dst_key_lo = rd32(desc + DDSD2_ddckCKDestBlt);
        root->dst_key_hi = rd32(desc + DDSD2_ddckCKDestBlt + 4);
    }
    if (flags & DDSD_LPSURFACE)
        log_once("ddraw7.lpsurface", "ddraw7: CreateSurface with client memory (DDSD_LPSURFACE) "
                                     "is not supported; the shim allocates its own");
    if (primary)
        g_display.primary = root->id;
    *out = root;
    return DD_OK;
}
} // namespace

// ===========================================================================
// IDirectDraw7
// ===========================================================================
namespace {
ComObj *this_dd(X86 *c) {
    ComObj *o = com_this_arg(c);
    return o && o->kind == K_DDRAW7 ? o : nullptr;
}
Surface *this_surface(X86 *c) {
    return surface_of(com_this_arg(c, IF_DDSURFACE7));
}

void DD7_CreateSurface(X86 *c) {
    ComObj *dd = this_dd(c);
    const uint32_t desc = arg(c, 1), out = arg(c, 2), outer = arg(c, 3);
    if (!dd || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, 0);
    if (outer) {
        com_ret(c, CLASS_E_NOAGGREGATION_);
        return;
    }
    Surface *s = nullptr;
    uint32_t hr = create_surface(dd->id, desc, &s);
    if (hr != DD_OK) {
        com_ret(c, hr);
        return;
    }
    const uint32_t view = surface_view(s);
    if (!view) {
        com_destroy(com_get(s->id));
        com_ret(c, E_OUTOFMEMORY_);
        return;
    }
    wr32(out, view);
    LOGV("ddraw7: CreateSurface caps %08x/%08x %ux%u %u bpp levels %d -> %08x", s->caps, s->caps2,
         s->width, s->height, s->format.bpp, s->levels, view);
    com_ret(c, DD_OK);
}

// EnumDisplayModes(dwFlags, lpDDSurfaceDesc2, lpContext, lpEnumModesCallback)
void DD7_EnumDisplayModes(X86 *c) {
    const uint32_t flags = arg(c, 1), filter = arg(c, 2), ctx = arg(c, 3), cb = arg(c, 4);
    if (!cb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    uint32_t fw = 0, fh = 0, fbpp = 0;
    if (filter && gm_valid(filter, DDSD2_SIZE)) {
        const uint32_t ff = rd32(filter + DDSD2_dwFlags);
        if (ff & DDSD_WIDTH)
            fw = rd32(filter + DDSD2_dwWidth);
        if (ff & DDSD_HEIGHT)
            fh = rd32(filter + DDSD2_dwHeight);
        if (ff & DDSD_PIXELFORMAT)
            fbpp = rd32(filter + DDSD2_ddpfPixelFormat + DDPF_dwRGBBitCount);
    }
    const uint32_t desc = heap_alloc(DDSD2_SIZE, true, 16);
    if (!desc) {
        com_ret(c, E_OUTOFMEMORY_);
        return;
    }
    const std::vector<Mode> list = modes();
    for (const Mode &m : list) {
        if ((fw && m.w != fw) || (fh && m.h != fh) || (fbpp && m.bpp != fbpp))
            continue;
        // Every mode at the display's one rate, 60 Hz, whether or not
        // DDEDM_REFRESHRATES asked for rates: the flag only decides whether
        // a mode is listed once per rate, and there is one. A game that
        // keeps the enumerated rate reports and sets 60 Hz, as on the
        // Windows reference (a 0 here made one log "at 0Hz").
        (void)flags;
        write_mode_desc(desc, m, 60);
        if (guest_call(c, cb, desc, ctx) == DDENUMRET_CANCEL)
            break;
    }
    heap_free(desc);
    com_ret(c, DD_OK);
}

void fill_caps(uint32_t a) {
    if (!a || !gm_valid(a, 4))
        return;
    const uint32_t size = rd32(a);
    if (size < 0x6c || !gm_valid(a, size))
        return;
    gm_zero(a + 4, size - 4);
    const uint32_t caps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY |
                          DDCAPS_COLORKEYHWASSIST | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL |
                          DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED | DDCAPS_CANBLTSYSMEM;
    wr32(a + DDCAPS_dwCaps, caps);
    wr32(a + DDCAPS_dwCaps2, DDCAPS2_WIDESURFACES | DDCAPS2_CANRENDERWINDOWED |
                                 DDCAPS2_FLIPINTERVAL | DDCAPS2_FLIPNOVSYNC |
                                 DDCAPS2_CANMANAGETEXTURE);
    wr32(a + DDCAPS_dwCKeyCaps, DDCKEYCAPS_SRCBLT);
    wr32(a + DDCAPS_dwFXCaps,
         DDFXCAPS_BLTARITHSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKXN |
             DDFXCAPS_BLTSHRINKY | DDFXCAPS_BLTSHRINKYN | DDFXCAPS_BLTSTRETCHX |
             DDFXCAPS_BLTSTRETCHXN | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSTRETCHYN);
    wr32(a + DDCAPS_dwZBufferBitDepths, DDBD_16 | DDBD_24 | DDBD_32);
    wr32(a + DDCAPS_dwVidMemTotal, 256u << 20);
    wr32(a + DDCAPS_dwVidMemFree, 240u << 20);
    const uint32_t scaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP |
                           DDSCAPS_FRONTBUFFER | DDSCAPS_MIPMAP | DDSCAPS_OFFSCREENPLAIN |
                           DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE |
                           DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_LOCALVIDMEM;
    wr32(a + DDCAPS_ddsOldCaps, scaps);
    if (size >= DDCAPS_dwSSBCaps + 4) {
        wr32(a + DDCAPS_dwSVBCaps, caps);
        wr32(a + DDCAPS_dwVSBCaps, caps);
        wr32(a + DDCAPS_dwSSBCaps, caps);
    }
    if (size >= DDCAPS7_SIZE)
        wr32(a + DDCAPS_ddsCaps, scaps);
}
// GetCaps(lpDDDriverCaps, lpDDHELCaps)
void DD7_GetCaps(X86 *c) {
    const uint32_t hw = arg(c, 1), hel = arg(c, 2);
    if (!hw && !hel) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    fill_caps(hw);
    fill_caps(hel);
    com_ret(c, DD_OK);
}
void DD7_GetDisplayMode(X86 *c) {
    const uint32_t desc = arg(c, 1);
    if (!desc || !gm_valid(desc, DDSD2_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    write_mode_desc(desc, current_mode(), g_display.refresh ? g_display.refresh : 60);
    com_ret(c, DD_OK);
}
// SetCooperativeLevel(hWnd, dwFlags). The flags are recorded; there is no
// exclusive mode to take on a host whose window the presenter owns.
void DD7_SetCooperativeLevel(X86 *c) {
    const uint32_t hwnd = arg(c, 1), flags = arg(c, 2);
    const bool exclusive = (flags & DDSCL_EXCLUSIVE) != 0, full = (flags & DDSCL_FULLSCREEN) != 0;
    if ((exclusive != full) && !(flags & DDSCL_SETFOCUSWINDOW)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (flags == DDSCL_SETFOCUSWINDOW || (flags & DDSCL_SETFOCUSWINDOW && !(flags & ~0x480u))) {
        g_display.focus_hwnd = hwnd;
    } else {
        g_display.coop = flags;
        if (hwnd)
            g_display.hwnd = hwnd;
    }
    LOGV("ddraw7: SetCooperativeLevel(%08x, %x)", hwnd, flags);
    com_ret(c, DD_OK);
}
// SetDisplayMode(dwWidth, dwHeight, dwBPP, dwRefreshRate, dwFlags)
void DD7_SetDisplayMode(X86 *c) {
    const uint32_t w = arg(c, 1), h = arg(c, 2), bpp = arg(c, 3), refresh = arg(c, 4);
    if (!w || !h || w > 16384 || h > 16384 || (bpp != 16 && bpp != 32)) {
        com_ret(c, DDERR_INVALIDMODE);
        return;
    }
    g_display.w = w;
    g_display.h = h;
    g_display.bpp = bpp;
    g_display.refresh = refresh;
    g_display.mode_set = true;
    ddraw_note_mode_impl(w, h, bpp);
    host_set_display_mode(int(w), int(h), int(bpp));
    com_ret(c, DD_OK);
}
void DD7_RestoreDisplayMode(X86 *c) {
    g_display.mode_set = false;
    com_ret(c, DD_OK);
}
// GetAvailableVidMem(lpDDSCaps2, lpdwTotal, lpdwFree)
void DD7_GetAvailableVidMem(X86 *c) {
    const uint32_t total = arg(c, 2), free = arg(c, 3);
    if (total && gm_valid(total, 4))
        wr32(total, 256u << 20);
    if (free && gm_valid(free, 4))
        wr32(free, 240u << 20);
    com_ret(c, DD_OK);
}
// GetDeviceIdentifier(lpdddi, dwFlags). Vendor and device 0: no known chip,
// so a program's per-card workarounds stay off.
void DD7_GetDeviceIdentifier(X86 *c) {
    const uint32_t a = arg(c, 1);
    if (!a || !gm_valid(a, DDDI2_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    gm_zero(a, DDDI2_SIZE);
    gm_put_str(a + DDDI2_szDriver, "d3d7metal.dll", 512);
    gm_put_str(a + DDDI2_szDescription, "Metal Direct3D 7 adapter", 512);
    wr32(a + DDDI2_liDriverVersion, 0x00010000u);
    wr32(a + DDDI2_liDriverVersion + 4, 0x00070000u);
    wr32(a + DDDI2_guidDeviceIdentifier, 0x6d657431u);
    com_ret(c, DD_OK);
}
void DD7_GetFourCCCodes(X86 *c) {
    const uint32_t count = arg(c, 1);
    if (count && gm_valid(count, 4))
        wr32(count, 0);
    com_ret(c, DD_OK);
}
void DD7_GetGDISurface(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, DDERR_NOTFOUND);
}
void DD7_GetMonitorFrequency(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 60);
    com_ret(c, DD_OK);
}
void DD7_GetScanLine(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, DD_OK);
}
void DD7_GetVerticalBlankStatus(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 1);
    com_ret(c, DD_OK);
}
void DD7_GetSurfaceFromDC(X86 *c) {
    const uint32_t out = arg(c, 2);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, DDERR_NOTFOUND);
}
void DD7_ok(X86 *c) {
    com_ret(c, DD_OK);
}
// FlipToGDISurface: the program is about to show GDI windows over its display
// and stops presenting. The frame it presented last stays on screen as layer 0
// (runtime/display_seam.h) until its next Flip; its windows are composed over
// it from now on. The GDI surface itself is never handed out (GetGDISurface).
void DD7_FlipToGDISurface(X86 *c) {
    g_display.gdi_showing = true;
    ++g_display.flips_to_gdi;
    display_layers_changed();
    com_ret(c, DD_OK);
}
DX_STUB(DD7_CreateClipper, DDERR_UNSUPPORTED)
DX_STUB(DD7_CreatePalette, DDERR_UNSUPPORTED)
DX_STUB(DD7_DuplicateSurface, DDERR_UNSUPPORTED)
DX_STUB(DD7_EnumSurfaces, DDERR_UNSUPPORTED)
DX_STUB(DD7_Initialize, d7::DDERR_ALREADYINITIALIZED)
DX_STUB(DD7_StartModeTest, DDERR_UNSUPPORTED)
DX_STUB(DD7_EvaluateMode, DDERR_UNSUPPORTED)

const ComMethod g_ddraw7[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"Compact", 1, DD7_ok},
    {"CreateClipper", 4, DD7_CreateClipper},
    {"CreatePalette", 5, DD7_CreatePalette},
    {"CreateSurface", 4, DD7_CreateSurface},
    {"DuplicateSurface", 3, DD7_DuplicateSurface},
    {"EnumDisplayModes", 5, DD7_EnumDisplayModes},
    {"EnumSurfaces", 5, DD7_EnumSurfaces},
    {"FlipToGDISurface", 1, DD7_FlipToGDISurface},
    {"GetCaps", 3, DD7_GetCaps},
    {"GetDisplayMode", 2, DD7_GetDisplayMode},
    {"GetFourCCCodes", 3, DD7_GetFourCCCodes},
    {"GetGDISurface", 2, DD7_GetGDISurface},
    {"GetMonitorFrequency", 2, DD7_GetMonitorFrequency},
    {"GetScanLine", 2, DD7_GetScanLine},
    {"GetVerticalBlankStatus", 2, DD7_GetVerticalBlankStatus},
    {"Initialize", 2, DD7_Initialize},
    {"RestoreDisplayMode", 1, DD7_RestoreDisplayMode},
    {"SetCooperativeLevel", 3, DD7_SetCooperativeLevel},
    {"SetDisplayMode", 6, DD7_SetDisplayMode},
    {"WaitForVerticalBlank", 3, DD7_ok},
    {"GetAvailableVidMem", 4, DD7_GetAvailableVidMem},
    {"GetSurfaceFromDC", 3, DD7_GetSurfaceFromDC},
    {"RestoreAllSurfaces", 1, DD7_ok},
    {"TestCooperativeLevel", 1, DD7_ok},
    {"GetDeviceIdentifier", 3, DD7_GetDeviceIdentifier},
    {"StartModeTest", 4, DD7_StartModeTest},
    {"EvaluateMode", 3, DD7_EvaluateMode},
};
static_assert(sizeof g_ddraw7 / sizeof g_ddraw7[0] == 30, "IDirectDraw7 has 30 slots");

// ===========================================================================
// IDirectDrawSurface7
// ===========================================================================
// AddAttachedSurface(lpDDSAttachedSurface): a depth buffer onto a colour surface.
void S7_AddAttachedSurface(X86 *c) {
    Surface *s = this_surface(c);
    Surface *z = surface_from_guest(arg(c, 1));
    if (!s || !z) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (!z->format.depth() || s->format.depth()) {
        com_ret(c, DDERR_CANNOTATTACHSURFACE);
        return;
    }
    if (s->attached_z) {
        com_ret(c, DDERR_SURFACEALREADYATTACHED);
        return;
    }
    s->attached_z = z->id;
    z->attached_to = s->id;
    com_addref(com_get(z->id));
    com_ret(c, DD_OK);
}
// DeleteAttachedSurface(dwFlags, lpDDSAttachedSurface)
void S7_DeleteAttachedSurface(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t which = arg(c, 2);
    if (!s) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    Surface *z = which ? surface_from_guest(which) : surface(s->attached_z);
    if (!z || s->attached_z != z->id) {
        com_ret(c, DDERR_SURFACENOTATTACHED);
        return;
    }
    s->attached_z = 0;
    z->attached_to = 0;
    com_release(com_get(z->id));
    com_ret(c, DD_OK);
}

// The attached surfaces of `s`, in the order GetAttachedSurface considers them.
std::vector<Surface *> attachments(Surface *s) {
    std::vector<Surface *> out;
    Surface *root = s->parent ? surface(s->parent) : s;
    if (root && (root->caps & DDSCAPS_MIPMAP)) {
        // The next level down.
        const size_t next = size_t(s->level); // chain holds levels 1..n-1
        if (next < root->chain.size())
            if (Surface *l = surface(root->chain[next]))
                out.push_back(l);
    } else if (root && (root->caps & DDSCAPS_FLIP)) {
        // A flip chain is a ring: front, back 1, ..., back n, front.
        std::vector<uint32_t> ring{root->id};
        ring.insert(ring.end(), root->chain.begin(), root->chain.end());
        for (size_t i = 0; i < ring.size(); ++i)
            if (ring[i] == s->id && ring.size() > 1)
                if (Surface *n = surface(ring[(i + 1) % ring.size()]))
                    out.push_back(n);
    }
    if (s->attached_z)
        if (Surface *z = surface(s->attached_z))
            out.push_back(z);
    return out;
}
// GetAttachedSurface(lpDDSCaps2, lplpDDAttachedSurface)
void S7_GetAttachedSurface(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t caps = arg(c, 1), out = arg(c, 2);
    if (!s || !caps || !gm_valid(caps, 16) || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, 0);
    uint32_t want = rd32(caps), want2 = rd32(caps + 4);
    // These name a memory pool, not an attachment.
    want &= ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_SYSTEMMEMORY);
    want2 &= DDSCAPS2_MIPMAPSUBLEVEL;
    for (Surface *a : attachments(s)) {
        if ((want & a->caps) == want && (want2 & a->caps2) == want2) {
            com_addref(com_get(a->id));
            wr32(out, surface_view(a));
            com_ret(c, DD_OK);
            return;
        }
    }
    com_ret(c, DDERR_NOTFOUND);
}
// EnumAttachedSurfaces(lpContext, lpEnumSurfacesCallback7)
void S7_EnumAttachedSurfaces(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t ctx = arg(c, 1), cb = arg(c, 2);
    if (!s || !cb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t desc = heap_alloc(DDSD2_SIZE, true, 16);
    for (Surface *a : attachments(s)) {
        com_addref(com_get(a->id));
        write_desc(desc, a, 0);
        if (guest_call(c, cb, surface_view(a), desc, ctx) == DDENUMRET_CANCEL)
            break;
    }
    heap_free(desc);
    com_ret(c, DD_OK);
}

// Blt(lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx)
void S7_Blt(X86 *c) {
    Surface *d = this_surface(c);
    const uint32_t drect = arg(c, 1), src_p = arg(c, 2), srect = arg(c, 3), flags = arg(c, 4),
                   fx = arg(c, 5);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    HostD7Rect dr;
    if (!read_rect(drect, d, &dr)) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    const bool has_fx = fx && gm_valid(fx, DDBLTFX_SIZE);
    if ((flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL | DDBLT_KEYSRCOVERRIDE | DDBLT_DDFX)) &&
        !has_fx) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const bool on_backend = surface_on_backend(d);
    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        HostD7Rect r = dr;
        r.x0 = std::max(r.x0, 0);
        r.y0 = std::max(r.y0, 0);
        r.x1 = std::min(r.x1, int32_t(d->width));
        r.y1 = std::min(r.y1, int32_t(d->height));
        if (r.x0 >= r.x1 || r.y0 >= r.y1) {
            com_ret(c, DD_OK);
            return;
        }
        const uint32_t value = rd32(fx + DDBLTFX_dwFillColor);
        if (flags & DDBLT_DEPTHFILL) {
            if (!d->format.depth()) {
                com_ret(c, DDERR_INVALIDPARAMS);
                return;
            }
            const double max =
                d->format.g ? double(d->format.g >> mask_shift(d->format.g)) : 65535.0;
            const float z = float(std::min(1.0, double(value) / max));
            if (on_backend && surface_backend_current(d)) {
                backend()->fill_depth(root_of(d)->id, &r, z);
                surface_backend_wrote(d);
            } else {
                cpu_fill(d, r, value);
            }
        } else if (on_backend && surface_backend_current(d)) {
            backend()->fill(root_of(d)->id, &r, pixel_to_bgra(d->format, value));
            surface_backend_wrote(d);
        } else {
            cpu_fill(d, r, value);
        }
        // A fullscreen program's frame is the whole primary (present_primary_blt).
        if (d->caps & DDSCAPS_PRIMARYSURFACE)
            present(d, (g_display.coop & DDSCL_FULLSCREEN)
                           ? HostD7Rect{0, 0, int32_t(d->width), int32_t(d->height)}
                           : r);
        com_ret(c, DD_OK);
        return;
    }
    Surface *s = surface_from_guest(src_p);
    if (!s) {
        log_once("ddraw7.bltnosrc", "ddraw7: Blt flags %x without a source is not supported",
                 flags);
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    HostD7Rect sr;
    if (!read_rect(srect, s, &sr)) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    // What a windowed present shows: the whole source rectangle, wherever on
    // the primary (the desktop) the window's client area is.
    const HostD7Rect shown = sr;
    if (s->format.depth() || d->format.depth()) {
        log_once("ddraw7.zblt", "ddraw7: Blt between depth buffers is not supported");
        com_ret(c, DDERR_UNSUPPORTED);
        return;
    }
    // Clip the destination to the surface, carrying the source along.
    {
        const double sx = double(sr.x1 - sr.x0) / (dr.x1 - dr.x0);
        const double sy = double(sr.y1 - sr.y0) / (dr.y1 - dr.y0);
        if (dr.x0 < 0) {
            sr.x0 += int32_t(std::lround(-dr.x0 * sx));
            dr.x0 = 0;
        }
        if (dr.y0 < 0) {
            sr.y0 += int32_t(std::lround(-dr.y0 * sy));
            dr.y0 = 0;
        }
        if (dr.x1 > int32_t(d->width)) {
            sr.x1 -= int32_t(std::lround((dr.x1 - int32_t(d->width)) * sx));
            dr.x1 = int32_t(d->width);
        }
        if (dr.y1 > int32_t(d->height)) {
            sr.y1 -= int32_t(std::lround((dr.y1 - int32_t(d->height)) * sy));
            dr.y1 = int32_t(d->height);
        }
    }
    if (dr.x0 >= dr.x1 || dr.y0 >= dr.y1) {
        // Nothing of the window is on the desktop; it is still the frame.
        if (d->caps & DDSCAPS_PRIMARYSURFACE)
            present_primary_blt(d, s, shown);
        com_ret(c, DD_OK);
        return;
    }
    if (sr.x0 < 0 || sr.y0 < 0 || sr.x1 > int32_t(s->width) || sr.y1 > int32_t(s->height) ||
        sr.x0 >= sr.x1 || sr.y0 >= sr.y1) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    BlitOptions o;
    if (flags & DDBLT_KEYSRCOVERRIDE) {
        o.key = true;
        o.key_lo = rd32(fx + DDBLTFX_ddckSrcColorkey);
        o.key_hi = rd32(fx + DDBLTFX_ddckSrcColorkey + 4);
    } else if ((flags & DDBLT_KEYSRC) && s->has_src_key) {
        o.key = true;
        o.key_lo = s->src_key_lo;
        o.key_hi = s->src_key_hi;
    }
    if (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE))
        log_once("ddraw7.keydest", "ddraw7: destination colour keys are ignored");
    if ((flags & DDBLT_ROP) && has_fx && rd32(fx + DDBLTFX_dwROP) != SRCCOPY_ROP)
        log_once("ddraw7.rop", "ddraw7: Blt raster operation %08x is copied as SRCCOPY",
                 rd32(fx + DDBLTFX_dwROP));
    if ((flags & DDBLT_DDFX) && has_fx) {
        const uint32_t ddfx = rd32(fx + DDBLTFX_dwDDFX);
        o.mirror_x = (ddfx & DDBLTFX_MIRRORLEFTRIGHT) != 0;
        o.mirror_y = (ddfx & DDBLTFX_MIRRORUPDOWN) != 0;
    }
    if (on_backend && surface_backend_current(s) && surface_backend_current(d)) {
        HostD7Blit b;
        memset(&b, 0, sizeof b);
        b.dst = root_of(d)->id;
        b.dst_level = d->level;
        b.dst_rect = dr;
        b.src = root_of(s)->id;
        b.src_level = s->level;
        b.src_rect = sr;
        if (o.key)
            b.key = make_key(s->format, o.key_lo, o.key_hi);
        b.src_has_alpha = s->format.has_alpha();
        b.mirror_x = o.mirror_x;
        b.mirror_y = o.mirror_y;
        backend()->blit(&b);
        surface_backend_wrote(d);
    } else {
        cpu_blit(d, dr, s, sr, o);
    }
    if (d->caps & DDSCAPS_PRIMARYSURFACE)
        present_primary_blt(d, s, shown);
    com_ret(c, DD_OK);
}
// BltFast(dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans)
void S7_BltFast(X86 *c) {
    Surface *d = this_surface(c);
    const int32_t x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    Surface *s = surface_from_guest(arg(c, 3));
    const uint32_t srect = arg(c, 4), trans = arg(c, 5);
    if (!d || !s) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    HostD7Rect sr;
    if (!read_rect(srect, s, &sr) || sr.x0 < 0 || sr.y0 < 0 || sr.x1 > int32_t(s->width) ||
        sr.y1 > int32_t(s->height)) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    HostD7Rect dr{x, y, x + (sr.x1 - sr.x0), y + (sr.y1 - sr.y0)};
    if (dr.x0 < 0 || dr.y0 < 0 || dr.x1 > int32_t(d->width) || dr.y1 > int32_t(d->height)) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    BlitOptions o;
    if ((trans & DDBLTFAST_SRCCOLORKEY) && s->has_src_key) {
        o.key = true;
        o.key_lo = s->src_key_lo;
        o.key_hi = s->src_key_hi;
    }
    if (surface_on_backend(d) && surface_backend_current(s) && surface_backend_current(d)) {
        HostD7Blit b;
        memset(&b, 0, sizeof b);
        b.dst = root_of(d)->id;
        b.dst_level = d->level;
        b.dst_rect = dr;
        b.src = root_of(s)->id;
        b.src_level = s->level;
        b.src_rect = sr;
        if (o.key)
            b.key = make_key(s->format, o.key_lo, o.key_hi);
        b.src_has_alpha = s->format.has_alpha();
        backend()->blit(&b);
        surface_backend_wrote(d);
    } else {
        cpu_blit(d, dr, s, sr, o);
    }
    if (d->caps & DDSCAPS_PRIMARYSURFACE)
        present_primary_blt(d, s, sr);
    com_ret(c, DD_OK);
}

bool vsync_enabled() {
    static int on = -1;
    if (on < 0) {
        const char *v = recomp_env("D3D7_VSYNC");
        on = !(v && !strcmp(v, "0"));
    }
    return on != 0;
}

// Two buffers of a flip chain (the same size and format, each its own root)
// trade their pixels: guest memory, backend resources and which copy is
// current. What the guest holds - the interface pointers, the caps, an
// attached depth buffer, a device rendering to the back buffer - stays with
// the surface, as on Windows, so the back buffer is now the other memory.
void exchange_buffers(Surface *a, Surface *b) {
    // Every backend holding one of the two resources exchanges them (one that
    // holds only one of them moves it).
    const Backend *owners[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        Surface *s = i ? b : a;
        auto it = resource_owner().find(s->id);
        if (it != resource_owner().end() && s->resource_generation == it->second->generation())
            owners[i] = it->second;
    }
    for (int i = 0; i < 2; ++i)
        if (owners[i] && (i == 0 || owners[1] != owners[0]))
            owners[i]->swap(a->id, b->id);
    auto oa = resource_owner().find(a->id), ob = resource_owner().find(b->id);
    const Backend *was_a = oa == resource_owner().end() ? nullptr : oa->second;
    const Backend *was_b = ob == resource_owner().end() ? nullptr : ob->second;
    resource_owner().erase(a->id);
    resource_owner().erase(b->id);
    if (was_b)
        resource_owner()[a->id] = was_b;
    if (was_a)
        resource_owner()[b->id] = was_a;
    std::swap(a->mem, b->mem);
    std::swap(a->cpu_valid, b->cpu_valid);
    std::swap(a->gpu_valid, b->gpu_valid);
    std::swap(a->resource_generation, b->resource_generation);
    ++a->uniqueness;
    ++b->uniqueness;
}

// Flip(lpDDSurfaceTargetOverride, dwFlags): the back buffer becomes the front
// buffer and is shown, and the front buffer becomes the back buffer, as on
// Windows: the buffers exchange their memory, so after a Flip the back buffer
// holds the frame shown before. With more back buffers the memory rotates
// (front <- back 1 <- back 2 ... <- front); a target override exchanges the
// front buffer with that buffer.
void S7_Flip(X86 *c) {
    Surface *front = this_surface(c);
    if (!front || !(front->caps & DDSCAPS_FLIP) || !front->is_root() || front->chain.empty()) {
        com_ret(c, DDERR_NOTFLIPPABLE);
        return;
    }
    std::vector<Surface *> ring{front};
    if (arg(c, 1)) {
        Surface *target = surface_from_guest(arg(c, 1));
        if (!target ||
            std::find(front->chain.begin(), front->chain.end(), target->id) == front->chain.end()) {
            com_ret(c, DDERR_INVALIDPARAMS);
            return;
        }
        ring.push_back(target);
    } else {
        for (uint32_t id : front->chain)
            if (Surface *s = surface(id))
                ring.push_back(s);
    }
    for (Surface *s : ring)
        if (s->lock_count > 0 || s->dc) {
            com_ret(c, DDERR_SURFACEBUSY);
            return;
        }
    const HostD7Rect all{0, 0, int32_t(front->width), int32_t(front->height)};
    g_display.gdi_showing = false;
    for (size_t i = 0; i + 1 < ring.size(); ++i)
        exchange_buffers(ring[i], ring[i + 1]);
    present(front, all);
    // A Flip without DDFLIP_NOVSYNC happens at the vertical blank, which is
    // what paces a game that flips as fast as it can draw. RECOMP_D3D7_VSYNC=0
    // turns that pacing off. The settings page's Frame limit holds every Flip,
    // synced or not: a game set not to wait for the blank (Empire Earth's
    // "Wait for VSync" = 0) would otherwise run uncapped. The wait is a
    // scheduler sleep, so the guest's other threads run through it.
    const uint32_t flags = arg(c, 2);
    const bool vblank = !(flags & DDFLIP_NOVSYNC) && vsync_enabled();
    const double delay =
        host_present_refresh_delay(vblank ? std::max(1, int((flags >> 24) & 7)) : 0);
    if (delay > 0)
        guest_sleep_ms(uint32_t(delay * 1000.0));
    com_ret(c, DD_OK);
}

void S7_GetCaps(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t a = arg(c, 1);
    if (!s || !a || !gm_valid(a, 16)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(a, s->caps);
    wr32(a + 4, s->caps2);
    wr32(a + 8, 0);
    wr32(a + 12, 0);
    com_ret(c, DD_OK);
}
// GetColorKey(dwFlags, lpDDColorKey) / SetColorKey(dwFlags, lpDDColorKey)
void S7_GetColorKey(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t flags = arg(c, 1), key = arg(c, 2);
    if (!s || !key || !gm_valid(key, 8)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (flags & DDCKEY_SRCBLT) {
        if (!s->has_src_key) {
            com_ret(c, DDERR_NOCOLORKEY);
            return;
        }
        wr32(key, s->src_key_lo);
        wr32(key + 4, s->src_key_hi);
    } else if (flags & DDCKEY_DESTBLT) {
        if (!s->has_dst_key) {
            com_ret(c, DDERR_NOCOLORKEY);
            return;
        }
        wr32(key, s->dst_key_lo);
        wr32(key + 4, s->dst_key_hi);
    } else {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_ret(c, DD_OK);
}
void S7_SetColorKey(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t flags = arg(c, 1), key = arg(c, 2);
    if (!s || (key && !gm_valid(key, 8))) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t lo = key ? rd32(key) : 0;
    const uint32_t hi = key ? ((flags & DDCKEY_COLORSPACE) ? rd32(key + 4) : lo) : 0;
    if (flags & DDCKEY_SRCBLT) {
        s->has_src_key = key != 0;
        s->src_key_lo = lo;
        s->src_key_hi = hi;
    } else if (flags & DDCKEY_DESTBLT) {
        s->has_dst_key = key != 0;
        s->dst_key_lo = lo;
        s->dst_key_hi = hi;
    } else {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_ret(c, DD_OK);
}

// GetDC(lphDC): a GDI DC over the surface's guest memory. Formats GDI cannot
// address directly draw into a 32-bit shadow that ReleaseDC converts back.
bool gdi_addressable(const Format &f) {
    if (f.bpp == 32)
        return f.r == 0xff0000 && f.g == 0xff00 && f.b == 0xff;
    return f.bpp == 16 && f.r == 0xf800 && f.g == 0x07e0 && f.b == 0x001f;
}
void S7_GetDC(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t out = arg(c, 1);
    if (!s || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (s->dc) {
        com_ret(c, DDERR_DCALREADYCREATED);
        return;
    }
    ReadbackVia why("GetDC");
    if (s->format.depth() || !surface_cpu_current(s)) {
        com_ret(c, DDERR_CANTCREATEDC);
        return;
    }
    s->dc = gdi_new_dc();
    if (gdi_addressable(s->format)) {
        gdi_bind_surface_dc(s->dc, int(s->width), int(s->height), int(s->format.bpp), s->pitch,
                            s->mem, nullptr);
    } else {
        s->dc_mem = heap_alloc(s->width * s->height * 4, false, 16);
        if (!s->dc_mem) {
            s->dc = 0;
            com_ret(c, DDERR_CANTCREATEDC);
            return;
        }
        for (uint32_t y = 0; y < s->height; ++y)
            row_to_bgra(s->format, gm_ptr(s->mem + y * s->pitch),
                        reinterpret_cast<uint32_t *>(gm_ptr(s->dc_mem + y * s->width * 4)),
                        int(s->width));
        gdi_bind_surface_dc(s->dc, int(s->width), int(s->height), 32, s->width * 4, s->dc_mem,
                            nullptr);
    }
    wr32(out, s->dc);
    com_ret(c, DD_OK);
}
void release_surface_dc(Surface *s) {
    gdi_unbind_surface_dc(s->dc);
    if (s->dc_mem) {
        // GDI writes colour, not alpha: keep each pixel's own alpha bits.
        const uint32_t bytes = s->format.bytes();
        for (uint32_t y = 0; y < s->height; ++y) {
            uint8_t *row = gm_ptr(s->mem + y * s->pitch);
            const uint32_t *shadow =
                reinterpret_cast<const uint32_t *>(gm_ptr(s->dc_mem + y * s->width * 4));
            for (uint32_t x = 0; x < s->width; ++x) {
                const uint32_t old = read_px(row + x * bytes, bytes);
                const uint32_t p =
                    (bgra_to_pixel(s->format, shadow[x] | 0xff000000u) & ~s->format.a) |
                    (old & s->format.a);
                write_px(row + x * bytes, bytes, p);
            }
        }
        heap_free(s->dc_mem);
        s->dc_mem = 0;
    }
    s->dc = 0;
    surface_cpu_wrote(s);
    s->cpu_writer = "GetDC";
}
void S7_ReleaseDC(X86 *c) {
    Surface *s = this_surface(c);
    if (!s) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (!s->dc || (arg(c, 1) && arg(c, 1) != s->dc)) {
        com_ret(c, DDERR_NODC);
        return;
    }
    release_surface_dc(s);
    com_ret(c, DD_OK);
}

void S7_GetPixelFormat(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t a = arg(c, 1);
    if (!s || !a || !gm_valid(a, DDPF_SIZE_)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    write_format(a, s->format);
    com_ret(c, DD_OK);
}
void S7_GetSurfaceDesc(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t a = arg(c, 1);
    if (!s || !a || !gm_valid(a, DDSD2_SIZE) || rd32(a) != DDSD2_SIZE) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    write_desc(a, s, 0);
    com_ret(c, DD_OK);
}
// Lock(lpDestRect, lpDDSurfaceDesc2, dwFlags, hEvent)
void S7_Lock(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t rect = arg(c, 1), desc = arg(c, 2), flags = arg(c, 3);
    if (!s || !desc || !gm_valid(desc, DDSD2_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    HostD7Rect r;
    if (!read_rect(rect, s, &r) || r.x0 < 0 || r.y0 < 0 || r.x1 > int32_t(s->width) ||
        r.y1 > int32_t(s->height)) {
        com_ret(c, DDERR_INVALIDRECT);
        return;
    }
    const bool whole =
        r.x0 == 0 && r.y0 == 0 && r.x1 == int32_t(s->width) && r.y1 == int32_t(s->height);
    if (whole && (flags & DDLOCK_DISCARDCONTENTS) && surface_memory(s))
        s->cpu_valid = true; // the guest promises to overwrite everything
    char via[96];
    snprintf(via, sizeof via, "Lock (flags %08x, rect %d,%d-%d,%d)", flags, r.x0, r.y0, r.x1, r.y1);
    ReadbackVia why(via);
    if (!surface_cpu_current(s)) {
        com_ret(c, E_OUTOFMEMORY_);
        return;
    }
    ++s->lock_count;
    s->lock_read_only = (flags & DDLOCK_READONLY) != 0;
    write_desc(desc, s, s->mem + uint32_t(r.y0) * s->pitch + uint32_t(r.x0) * s->format.bytes());
    com_ret(c, DD_OK);
}
void S7_Unlock(X86 *c) {
    Surface *s = this_surface(c);
    if (!s) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (s->lock_count <= 0) {
        com_ret(c, DDERR_NOTLOCKED);
        return;
    }
    --s->lock_count;
    if (!s->lock_read_only) {
        surface_cpu_wrote(s);
        s->cpu_writer = "Lock";
    }
    s->lock_read_only = false;
    com_ret(c, DD_OK);
}
void S7_GetDDInterface(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t out = arg(c, 1);
    ComObj *dd = s ? com_get(s->dd) : nullptr;
    if (!dd || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_addref(dd);
    wr32(out, com_view(dd, IF_DIRECTDRAW7));
    com_ret(c, DD_OK);
}
// SetPrivateData(guidTag, lpData, cbSize, dwFlags) / GetPrivateData / FreePrivateData
std::string guid_key(uint32_t g) {
    return gm_valid(g, 16) ? std::string(reinterpret_cast<const char *>(gm_ptr(g)), 16)
                           : std::string();
}
void S7_SetPrivateData(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t g = arg(c, 1), data = arg(c, 2), size = arg(c, 3), flags = arg(c, 4);
    if (!s || !gm_valid(g, 16) || (size && !gm_valid(data, size))) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (flags & 0x2) // DDSPD_IUNKNOWNPOINTER: stored as the pointer
        log_once("ddraw7.privunk", "ddraw7: private IUnknown data is not reference counted");
    s->private_data[guid_key(g)] = std::vector<uint8_t>(gm_ptr(data), gm_ptr(data) + size);
    com_ret(c, DD_OK);
}
void S7_GetPrivateData(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t g = arg(c, 1), buf = arg(c, 2), size_p = arg(c, 3);
    if (!s || !gm_valid(g, 16) || !size_p || !gm_valid(size_p, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    auto it = s->private_data.find(guid_key(g));
    if (it == s->private_data.end()) {
        com_ret(c, DDERR_NOTFOUND);
        return;
    }
    const uint32_t have = rd32(size_p), need = uint32_t(it->second.size());
    wr32(size_p, need);
    if (!buf || have < need) {
        com_ret(c, 0x887602b2u); // DDERR_MOREDATA
        return;
    }
    if (need && gm_valid(buf, need))
        memcpy(gm_ptr(buf), it->second.data(), need);
    com_ret(c, DD_OK);
}
void S7_FreePrivateData(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t g = arg(c, 1);
    if (!s || !gm_valid(g, 16)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_ret(c, s->private_data.erase(guid_key(g)) ? DD_OK : DDERR_NOTFOUND);
}
void S7_GetUniquenessValue(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t out = arg(c, 1);
    if (!s || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, s->uniqueness);
    com_ret(c, DD_OK);
}
void S7_ChangeUniquenessValue(X86 *c) {
    if (Surface *s = this_surface(c))
        ++s->uniqueness;
    com_ret(c, DD_OK);
}
void S7_SetPriority(X86 *c) {
    if (Surface *s = this_surface(c))
        s->priority = arg(c, 1);
    com_ret(c, DD_OK);
}
void S7_GetPriority(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t out = arg(c, 1);
    if (!s || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, s->priority);
    com_ret(c, DD_OK);
}
void S7_SetLOD(X86 *c) {
    if (Surface *s = this_surface(c))
        s->lod = arg(c, 1);
    com_ret(c, DD_OK);
}
void S7_GetLOD(X86 *c) {
    Surface *s = this_surface(c);
    const uint32_t out = arg(c, 1);
    if (!s || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, s->lod);
    com_ret(c, DD_OK);
}
void S7_GetClipper(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, 0x887600cdu); // DDERR_NOCLIPPERATTACHED
}
void S7_GetPalette(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, DDERR_NOPALETTEATTACHED);
}
void S7_ok(X86 *c) {
    com_ret(c, DD_OK);
}
DX_STUB(S7_AddOverlayDirtyRect, DDERR_UNSUPPORTED)
DX_STUB(S7_BltBatch, DDERR_UNSUPPORTED)
DX_STUB(S7_EnumOverlayZOrders, DDERR_UNSUPPORTED)
DX_STUB(S7_GetOverlayPosition, DDERR_NOTAOVERLAYSURFACE)
DX_STUB(S7_Initialize, d7::DDERR_ALREADYINITIALIZED)
DX_STUB(S7_SetClipper, DDERR_UNSUPPORTED)
DX_STUB(S7_SetOverlayPosition, DDERR_NOTAOVERLAYSURFACE)
DX_STUB(S7_SetPalette, DDERR_UNSUPPORTED)
DX_STUB(S7_UpdateOverlay, DDERR_NOTAOVERLAYSURFACE)
DX_STUB(S7_UpdateOverlayDisplay, DDERR_UNSUPPORTED)
DX_STUB(S7_UpdateOverlayZOrder, DDERR_NOTAOVERLAYSURFACE)
DX_STUB(S7_SetSurfaceDesc, DDERR_UNSUPPORTED)

const ComMethod g_surface7[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"AddAttachedSurface", 2, S7_AddAttachedSurface},
    {"AddOverlayDirtyRect", 2, S7_AddOverlayDirtyRect},
    {"Blt", 6, S7_Blt},
    {"BltBatch", 4, S7_BltBatch},
    {"BltFast", 6, S7_BltFast},
    {"DeleteAttachedSurface", 3, S7_DeleteAttachedSurface},
    {"EnumAttachedSurfaces", 3, S7_EnumAttachedSurfaces},
    {"EnumOverlayZOrders", 4, S7_EnumOverlayZOrders},
    {"Flip", 3, S7_Flip},
    {"GetAttachedSurface", 3, S7_GetAttachedSurface},
    {"GetBltStatus", 2, S7_ok},
    {"GetCaps", 2, S7_GetCaps},
    {"GetClipper", 2, S7_GetClipper},
    {"GetColorKey", 3, S7_GetColorKey},
    {"GetDC", 2, S7_GetDC},
    {"GetFlipStatus", 2, S7_ok},
    {"GetOverlayPosition", 3, S7_GetOverlayPosition},
    {"GetPalette", 2, S7_GetPalette},
    {"GetPixelFormat", 2, S7_GetPixelFormat},
    {"GetSurfaceDesc", 2, S7_GetSurfaceDesc},
    {"Initialize", 3, S7_Initialize},
    {"IsLost", 1, S7_ok},
    {"Lock", 5, S7_Lock},
    {"ReleaseDC", 2, S7_ReleaseDC},
    {"Restore", 1, S7_ok},
    {"SetClipper", 2, S7_SetClipper},
    {"SetColorKey", 3, S7_SetColorKey},
    {"SetOverlayPosition", 3, S7_SetOverlayPosition},
    {"SetPalette", 2, S7_SetPalette},
    {"Unlock", 2, S7_Unlock},
    {"UpdateOverlay", 6, S7_UpdateOverlay},
    {"UpdateOverlayDisplay", 2, S7_UpdateOverlayDisplay},
    {"UpdateOverlayZOrder", 3, S7_UpdateOverlayZOrder},
    {"GetDDInterface", 2, S7_GetDDInterface},
    {"PageLock", 2, S7_ok},
    {"PageUnlock", 2, S7_ok},
    {"SetSurfaceDesc", 3, S7_SetSurfaceDesc},
    {"SetPrivateData", 5, S7_SetPrivateData},
    {"GetPrivateData", 4, S7_GetPrivateData},
    {"FreePrivateData", 2, S7_FreePrivateData},
    {"GetUniquenessValue", 2, S7_GetUniquenessValue},
    {"ChangeUniquenessValue", 1, S7_ChangeUniquenessValue},
    {"SetPriority", 2, S7_SetPriority},
    {"GetPriority", 2, S7_GetPriority},
    {"SetLOD", 2, S7_SetLOD},
    {"GetLOD", 2, S7_GetLOD},
};
static_assert(sizeof g_surface7 / sizeof g_surface7[0] == 49, "IDirectDrawSurface7 has 49 slots");
} // namespace

// ===========================================================================
// Creation, registration and reset
// ===========================================================================
namespace {
int g_ddraw7_override = -1;
}
bool ddraw7_enabled() {
    if (g_ddraw7_override >= 0)
        return g_ddraw7_override != 0;
    const char *e = recomp_env("DDRAW7");
    if (e && *e)
        return strcmp(e, "0") != 0;
    return RECOMP_DDRAW7_DEFAULT != 0;
}
void ddraw7_enable_for_test(int on) {
    g_ddraw7_override = on;
}

uint32_t ddraw7_create(uint32_t out) {
    ComObj *dd = com_new(K_DDRAW7);
    const uint32_t view = com_view(dd, IF_DIRECTDRAW7);
    if (!view) {
        com_destroy(dd);
        return E_OUTOFMEMORY_;
    }
    wr32(out, view);
    LOGV("ddraw7: DirectDrawCreateEx -> %08x", view);
    return DD_OK;
}

void ddraw7_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    com_define(IF_DIRECTDRAW7, "DDRAW.dll", "IDirectDraw7", g_ddraw7, std::size(g_ddraw7));
    com_define(IF_DDSURFACE7, "DDRAW.dll", "IDirectDrawSurface7", g_surface7,
               std::size(g_surface7));
    com_bind(IF_DIRECTDRAW7, K_DDRAW7);
    com_bind(IF_DDSURFACE7, K_SURFACE7);
    com_register_iid(IF_DIRECTDRAW7, IID_IDirectDraw7);
    com_register_iid(IF_DDSURFACE7, IID_IDirectDrawSurface7);
    com_set_destructor(K_SURFACE7, surface_destroy);
    com_set_destructor(K_DDRAW7, ddraw7_destroy);
}

void ddraw7_reset() {
    // The guest arena the surfaces lived in is gone; so are their resources.
    for (auto &kv : resource_owner())
        kv.second->destroy(kv.first);
    resource_owner().clear();
    surfaces().clear();
    mode_table().clear();
    g_display = DisplayState{};
    display_layers_changed();
}

} // namespace d7

// The owner of display layer 0 (runtime/display_seam.h): a primary surface in
// exclusive fullscreen mode. Its cooperative-level window is the window the
// frame covers; the windows stacked above it are shown over the frame.
extern "C" bool ddraw7_screen_owner(uint32_t *hwnd) {
    const d7::DisplayState &d = d7::display_state();
    if (!d.primary || !(d.coop & d7::DDSCL_FULLSCREEN) || !d7::surface(d.primary))
        return false;
    if (hwnd)
        *hwnd = d.hwnd;
    return true;
}
// For tests: FlipToGDISurface calls, and whether the GDI surface is showing.
extern "C" uint32_t ddraw7_flips_to_gdi(bool *showing) {
    const d7::DisplayState &d = d7::display_state();
    if (showing)
        *showing = d.gdi_showing;
    return d.flips_to_gdi;
}

// The test seam for the offered mode list (RECOMP_D3D7_MODES syntax). A null or
// empty spec restores the built-in list. Returns 0 on a malformed spec.
extern "C" int ddraw7_set_modes(const char *spec) {
    auto &t = d7::mode_table();
    if (!spec || !*spec) {
        t.clear();
        return 1;
    }
    std::vector<d7::Mode> parsed;
    if (!d7::parse_modes(spec, &parsed))
        return 0;
    t = parsed;
    return 1;
}
