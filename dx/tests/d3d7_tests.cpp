// d3d7_tests.cpp - headless tests for the DirectDraw 7 / Direct3D 7 shims.
//
// Everything runs through the guest ABI (guest_abi.h): the object model,
// enumeration, surfaces and their locks, Blt and Flip on the CPU path (no
// renderer) and on the software reference renderer, GDI on surfaces, the
// Direct3D 7 device's state, and the synthetic scenes of d3d7_scenes.h checked
// against values computed by hand from the DirectX 7 equations.
#include "guest_abi.h"
#include "d3d7_scenes.h"
#include "../dx.h"
#include "../host_api.h"
#include "../../runtime/display_seam.h"
#include "../../runtime/memory.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace d7t;

// ---------------------------------------------------------------------------
// Captured host callbacks
// ---------------------------------------------------------------------------
struct Shown {
    int w = 0, h = 0;
    std::vector<uint32_t> argb;
};
static std::vector<Shown> g_shown;
static int g_mode_w = 0, g_mode_h = 0, g_mode_bpp = 0;
extern "C" void host_display_present_window(const uint32_t *argb, int w, int h) {
    Shown s;
    s.w = w;
    s.h = h;
    s.argb.assign(argb, argb + size_t(w) * size_t(h));
    g_shown.push_back(std::move(s));
}
extern "C" void host_set_display_mode(int w, int h, int bpp) {
    g_mode_w = w;
    g_mode_h = h;
    g_mode_bpp = bpp;
}

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        const uint32_t near_a_ = (a), near_b_ = (b);                                               \
        ++g_checks;                                                                                \
        if (max_diff(near_a_, near_b_) > (tol)) {                                                  \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s ~ %s (%08x vs %08x)\n", __FILE__, __LINE__, #a, #b,    \
                    near_a_, near_b_);                                                             \
        }                                                                                          \
    } while (0)

// Runs `fn` once with no renderer (every operation on the CPU) and once on
// the software reference renderer.
static void both_paths(void (*fn)(bool soft)) {
    d7::use_soft_backend(false);
    fn(false);
    d7::use_soft_backend(true);
    fn(true);
    d7::use_soft_backend(false);
}

// ---------------------------------------------------------------------------
// The object model
// ---------------------------------------------------------------------------
static void test_create_and_query() {
    cpu_reset();
    const uint32_t create_ex = tramp("DDRAW.dll", "DirectDrawCreateEx");
    const uint32_t iid = gmem(16), out = gmem(4);
    memcpy(gm_ptr(iid), d7::IID_IDirectDraw7, 16);
    // Off unless the game opts in: the caller falls back to DirectDrawCreate.
    d7::ddraw7_enable_for_test(0);
    CHECK_EQ(call_shim(create_ex, {0, out, iid, 0}), DDERR_UNSUPPORTED);
    CHECK_EQ(rd32(out), 0);
    d7::ddraw7_enable_for_test(1);
    const uint32_t live = com_live_count();
    CHECK_EQ(call_shim(create_ex, {0, out, iid, 0}), 0);
    const uint32_t dd = rd32(out);
    CHECK(dd != 0);
    CHECK_EQ(call_shim(create_ex, {0, out, iid, 1}), CLASS_E_NOAGGREGATION);

    const uint32_t d3d = query(dd, d7::IID_IDirect3D7);
    CHECK(d3d != 0);
    CHECK(d3d != dd);
    CHECK_EQ(query(d3d, d7::IID_IDirectDraw7), dd);
    call_method(dd, Release, {});
    static const uint8_t dd4[16] = {0x9A, 0x50, 0x59, 0x9C, 0xBD, 0x39, 0xD1, 0x11,
                                    0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5};
    CHECK_EQ(query(dd, dd4), 0);

    // GetCaps: a 3D-capable device with colour fills, stretching and keys.
    const uint32_t caps = gmem(d7::DDCAPS7_SIZE);
    wr32(caps, d7::DDCAPS7_SIZE);
    CHECK_EQ(call_method(dd, DD_GetCaps, {caps, 0}), 0);
    CHECK(rd32(caps + d7::DDCAPS_dwCaps) & d7::DDCAPS_3D);
    CHECK(rd32(caps + d7::DDCAPS_dwCaps) & d7::DDCAPS_BLTCOLORFILL);
    CHECK(rd32(caps + d7::DDCAPS_dwCKeyCaps) & d7::DDCKEYCAPS_SRCBLT);
    CHECK(rd32(caps + d7::DDCAPS_dwVidMemTotal) > (8u << 20));
    // GetDeviceIdentifier: no known vendor, and no description a per-card
    // workaround matches on.
    const uint32_t id = gmem(d7::DDDI2_SIZE);
    CHECK_EQ(call_method(dd, DD_GetDeviceIdentifier, {id, 0}), 0);
    std::string desc = gm_str(id + d7::DDDI2_szDescription);
    for (char &ch : desc)
        ch = char(toupper(ch));
    CHECK(desc.find("RAGE") == std::string::npos);
    CHECK(desc.find("PRO") == std::string::npos);
    CHECK_EQ(rd32(id + d7::DDDI2_dwVendorId), 0);
    CHECK_EQ(rd32(id + d7::DDDI2_dwDeviceId), 0);
    const uint32_t total = gmem(4), free = gmem(4);
    CHECK_EQ(call_method(dd, DD_GetAvailableVidMem, {0, total, free}), 0);
    CHECK(rd32(free) > (8u << 20));
    CHECK_EQ(call_method(dd, DD_TestCooperativeLevel, {}), 0);
    CHECK_EQ(call_method(dd, DD_RestoreAllSurfaces, {}), 0);
    call_method(d3d, Release, {});
    CHECK_EQ(call_method(dd, Release, {}), 0);
    CHECK_EQ(com_live_count(), live);
    for (uint32_t a : {iid, out, caps, id, total, free})
        heap_free(a);
}

// EnumDisplayModes reports 5-6-5 and X8R8G8B8 modes, 800x600x16 among them.
struct ModeSeen {
    uint32_t w, h, bpp, pitch, r, g, b;
};
static std::vector<ModeSeen> g_modes;
static int g_mode_cancel_after = -1;
static void mode_callback(X86 *c) {
    const uint32_t d = arg(c, 0);
    g_modes.push_back({rd32(d + d7::DDSD2_dwWidth), rd32(d + d7::DDSD2_dwHeight),
                       rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRGBBitCount),
                       rd32(d + d7::DDSD2_lPitch),
                       rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRBitMask),
                       rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwGBitMask),
                       rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwBBitMask)});
    set_eax(c, int(g_modes.size()) == g_mode_cancel_after ? 0 : 1);
}
extern "C" int ddraw7_set_modes(const char *spec);
static void test_display_modes() {
    cpu_reset();
    const uint32_t dd = create_dd();
    const uint32_t cb = imports_alloc_trampoline("d3d7_tests", "mode_callback", mode_callback, 2);
    g_modes.clear();
    CHECK_EQ(call_method(dd, DD_EnumDisplayModes, {0, 0, 0x1234, cb}), 0);
    bool has800x16 = false, has1024x32 = false;
    for (const ModeSeen &m : g_modes) {
        CHECK(m.bpp == 16 || m.bpp == 32);
        CHECK_EQ(m.pitch, m.w * m.bpp / 8);
        if (m.bpp == 16) {
            CHECK_EQ(m.r, 0xf800);
            CHECK_EQ(m.g, 0x07e0);
            CHECK_EQ(m.b, 0x001f);
        } else {
            CHECK_EQ(m.r, 0xff0000);
            CHECK_EQ(m.g, 0xff00);
            CHECK_EQ(m.b, 0xff);
        }
        has800x16 |= m.w == 800 && m.h == 600 && m.bpp == 16;
        has1024x32 |= m.w == 1024 && m.h == 768 && m.bpp == 32;
    }
    CHECK(has800x16);
    CHECK(has1024x32);
    // A callback that returns DDENUMRET_CANCEL stops the enumeration.
    g_modes.clear();
    g_mode_cancel_after = 2;
    CHECK_EQ(call_method(dd, DD_EnumDisplayModes, {0, 0, 0, cb}), 0);
    CHECK_EQ(g_modes.size(), 2);
    g_mode_cancel_after = -1;
    // A width filter.
    const uint32_t filter = gmem(d7::DDSD2_SIZE);
    wr32(filter, d7::DDSD2_SIZE);
    wr32(filter + d7::DDSD2_dwFlags, d7::DDSD_WIDTH);
    wr32(filter + d7::DDSD2_dwWidth, 640);
    g_modes.clear();
    CHECK_EQ(call_method(dd, DD_EnumDisplayModes, {0, filter, 0, cb}), 0);
    CHECK_EQ(g_modes.size(), 2);
    // The test seam (and RECOMP_D3D7_MODES) replace the list.
    CHECK_EQ(ddraw7_set_modes("1280x720x32,800x600x16"), 1);
    g_modes.clear();
    CHECK_EQ(call_method(dd, DD_EnumDisplayModes, {0, 0, 0, cb}), 0);
    CHECK_EQ(g_modes.size(), 2);
    CHECK_EQ(ddraw7_set_modes("640x480x8"), 0);
    CHECK_EQ(ddraw7_set_modes(nullptr), 1);
    heap_free(filter);
    call_method(dd, Release, {});
}

// SetCooperativeLevel takes every combination a windowed/fullscreen engine
// uses; SetDisplayMode changes what GetDisplayMode and new primaries report.
static void test_cooperative_level_and_mode() {
    cpu_reset();
    const uint32_t dd = create_dd();
    for (uint32_t flags : {0x408u, 0x8u, 0x80u, 0x513u, 0x413u, 0x691u})
        CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, flags}), 0);
    CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, d7::DDSCL_EXCLUSIVE}),
             DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1024, 768, 16, 0, 0}), 0);
    CHECK_EQ(g_mode_w, 1024);
    CHECK_EQ(g_mode_h, 768);
    CHECK_EQ(g_mode_bpp, 16);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1024, 768, 8, 0, 0}), d7::DDERR_INVALIDMODE);
    const uint32_t d = gmem(d7::DDSD2_SIZE);
    wr32(d, d7::DDSD2_SIZE);
    CHECK_EQ(call_method(dd, DD_GetDisplayMode, {d}), 0);
    CHECK_EQ(rd32(d + d7::DDSD2_dwWidth), 1024);
    CHECK_EQ(rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRBitMask), 0xf800);
    SurfaceSpec p;
    p.caps = d7::DDSCAPS_PRIMARYSURFACE;
    const uint32_t primary = create_surface(dd, p);
    CHECK(primary != 0);
    CHECK_EQ(call_method(primary, S_GetSurfaceDesc, {d}), 0);
    CHECK_EQ(rd32(d + d7::DDSD2_dwWidth), 1024);
    CHECK_EQ(rd32(d + d7::DDSD2_dwHeight), 768);
    CHECK_EQ(rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRGBBitCount), 16);
    call_method(primary, Release, {});
    heap_free(d);
    call_method(dd, Release, {});
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
// A flip chain: GetAttachedSurface walks it, and Flip shows the back buffer
// while the back buffer keeps its contents.
static void flip_chain(bool soft) {
    cpu_reset();
    const uint32_t dd = create_dd();
    CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, 0x513}), 0);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {64, 48, 32, 0, 0}), 0);
    SurfaceSpec s;
    s.caps = d7::DDSCAPS_PRIMARYSURFACE | d7::DDSCAPS_FLIP | d7::DDSCAPS_COMPLEX |
             d7::DDSCAPS_3DDEVICE | d7::DDSCAPS_VIDEOMEMORY | d7::DDSCAPS_LOCALVIDMEM;
    s.back_buffers = 1;
    const uint32_t front = create_surface(dd, s);
    CHECK(front != 0);
    const uint32_t back = attached(front, d7::DDSCAPS_BACKBUFFER);
    CHECK(back != 0);
    CHECK_EQ(attached(front, d7::DDSCAPS_FLIP), back);
    call_method(back, Release, {});
    CHECK_EQ(attached(back, d7::DDSCAPS_FLIP), front);
    call_method(front, Release, {});
    uint32_t hr = 0;
    CHECK_EQ(attached(back, d7::DDSCAPS_MIPMAP, 0, &hr), 0);
    CHECK_EQ(hr, d7::DDERR_NOTFOUND);
    CHECK_EQ(call_method(back, S_Flip, {0, d7::DDFLIP_DONOTWAIT}), d7::DDERR_NOTFLIPPABLE);

    // Draw a picture once, then patch it, flipping in between.
    g_shown.clear();
    CHECK_EQ(fill(back, 0x00336699u), 0);
    CHECK_EQ(fill(back, 0x00ffffffu, 10, 10, 20, 20), 0);
    CHECK_EQ(call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT}), 0);
    CHECK_EQ(g_shown.size(), 1);
    if (!g_shown.empty()) {
        CHECK_EQ(g_shown[0].w, 64);
        CHECK_EQ(g_shown[0].h, 48);
        CHECK_EQ(g_shown[0].argb[0] & 0xffffff, 0x336699);
        CHECK_EQ(g_shown[0].argb[15 * 64 + 15] & 0xffffff, 0xffffff);
    }
    // The back buffer still holds the picture, and so does the front.
    Image b = read(back), f = read(front);
    CHECK_EQ(b.at(0, 0) & 0xffffff, 0x336699);
    CHECK_EQ(b.at(15, 15) & 0xffffff, 0xffffff);
    CHECK_EQ(f.at(15, 15) & 0xffffff, 0xffffff);
    // A cursor-sized patch, flipped twice: both presents show picture and patch.
    CHECK_EQ(fill(back, 0x00ff0000u, 40, 30, 48, 38), 0);
    CHECK_EQ(call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT}), 0);
    CHECK_EQ(call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT | d7::DDFLIP_NOVSYNC}), 0);
    CHECK_EQ(g_shown.size(), 3);
    for (size_t i = 1; i < g_shown.size(); ++i) {
        CHECK_EQ(g_shown[i].argb[15 * 64 + 15] & 0xffffff, 0xffffff);
        CHECK_EQ(g_shown[i].argb[32 * 64 + 42] & 0xffffff, 0xff0000);
        CHECK_EQ(g_shown[i].argb[5 * 64 + 5] & 0xffffff, 0x336699);
    }
    // A depth buffer attached to the back buffer is found by its caps.
    const uint32_t z = zbuffer(dd, 64, 48, PF_Z24);
    CHECK(z != 0);
    CHECK_EQ(call_method(back, S_AddAttachedSurface, {z}), 0);
    CHECK_EQ(attached(back, d7::DDSCAPS_ZBUFFER), z);
    call_method(z, Release, {});
    call_method(z, Release, {}); // the attachment holds the last reference
    call_method(back, Release, {});
    CHECK_EQ(call_method(front, Release, {}), 0);
    call_method(dd, Release, {});
    (void)soft;
}

// The windowed present: a Blt from the back buffer onto the primary, aimed at
// the window's client area in desktop coordinates, shows the whole source -
// even where that rectangle runs off the desktop.
static void windowed_present(bool) {
    cpu_reset();
    const uint32_t dd = create_dd();
    CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, 0x408}), 0);
    SurfaceSpec ps;
    ps.caps = d7::DDSCAPS_PRIMARYSURFACE;
    const uint32_t primary = create_surface(dd, ps);
    const uint32_t back = offscreen(dd, 32, 24, PF_X8R8G8B8);
    CHECK_EQ(fill(back, 0x00405060u), 0);
    CHECK_EQ(fill(back, 0x00ff0000u, 30, 20, 32, 24), 0);
    const uint32_t d = gmem(d7::DDSD2_SIZE);
    wr32(d, d7::DDSD2_SIZE);
    CHECK_EQ(call_method(primary, S_GetSurfaceDesc, {d}), 0);
    const int32_t dw = int32_t(rd32(d + d7::DDSD2_dwWidth)),
                  dh = int32_t(rd32(d + d7::DDSD2_dwHeight));
    for (const int32_t x : {16, dw - 8}) {
        g_shown.clear();
        const uint32_t to = rect(x, dh - 12, x + 32, dh + 12), from = rect(0, 0, 32, 24);
        CHECK_EQ(call_method(primary, S_Blt, {to, back, from, 0, 0}), 0);
        CHECK_EQ(g_shown.size(), 1);
        if (!g_shown.empty()) {
            CHECK_EQ(g_shown[0].w, 32);
            CHECK_EQ(g_shown[0].h, 24);
            CHECK_EQ(g_shown[0].argb[0] & 0xffffff, 0x405060);
            CHECK_EQ(g_shown[0].argb[23 * 32 + 31] & 0xffffff, 0xff0000);
        }
        heap_free(to);
        heap_free(from);
    }
    heap_free(d);
    call_method(back, Release, {});
    call_method(primary, Release, {});
    call_method(dd, Release, {});
}

// A full mip chain: GetAttachedSurface(MIPMAP) walks level by level to 1x1.
static void test_mip_chain() {
    cpu_reset();
    const uint32_t dd = create_dd();
    // MIPMAP | COMPLEX without DDSD_MIPMAPCOUNT: every level down to 1x1.
    SurfaceSpec s;
    s.caps = d7::DDSCAPS_TEXTURE | d7::DDSCAPS_MIPMAP | d7::DDSCAPS_COMPLEX;
    s.caps2 = d7::DDSCAPS2_TEXTUREMANAGE | d7::DDSCAPS2_HINTSTATIC;
    s.w = 128;
    s.h = 32;
    s.pf = &PF_A8R8G8B8;
    const uint32_t root = create_surface(dd, s);
    CHECK(root != 0);
    const uint32_t d = gmem(d7::DDSD2_SIZE);
    wr32(d, d7::DDSD2_SIZE);
    CHECK_EQ(call_method(root, S_GetSurfaceDesc, {d}), 0);
    CHECK_EQ(rd32(d + d7::DDSD2_dwMipMapCount), 8);
    uint32_t level = root;
    int count = 1;
    for (;;) {
        uint32_t hr = 0;
        const uint32_t next = attached(level, d7::DDSCAPS_TEXTURE | d7::DDSCAPS_MIPMAP, 0, &hr);
        if (!next) {
            CHECK_EQ(hr, d7::DDERR_NOTFOUND);
            break;
        }
        ++count;
        CHECK_EQ(call_method(next, S_GetSurfaceDesc, {d}), 0);
        CHECK_EQ(rd32(d + d7::DDSD2_dwWidth), std::max(1u, 128u >> (count - 1)));
        CHECK_EQ(rd32(d + d7::DDSD2_dwHeight), std::max(1u, 32u >> (count - 1)));
        CHECK(rd32(d + d7::DDSD2_ddsCaps + 4) & d7::DDSCAPS2_MIPMAPSUBLEVEL);
        if (level != root)
            call_method(level, Release, {});
        level = next;
    }
    CHECK_EQ(count, 8);
    if (level != root)
        call_method(level, Release, {});
    // An explicit count of 3 makes three levels.
    s.mips = 3;
    const uint32_t three = create_surface(dd, s);
    CHECK_EQ(call_method(three, S_GetSurfaceDesc, {d}), 0);
    CHECK_EQ(rd32(d + d7::DDSD2_dwMipMapCount), 3);
    s.mips = 9;
    uint32_t hr = 0;
    CHECK_EQ(create_surface(dd, s, &hr), 0);
    CHECK_EQ(hr, DDERR_INVALIDPARAMS);
    call_method(three, Release, {});
    CHECK_EQ(call_method(root, Release, {}), 0);
    heap_free(d);
    call_method(dd, Release, {});
}

// Depth buffers in each format; attachment rules.
static void test_depth_buffers() {
    cpu_reset();
    const uint32_t dd = create_dd();
    const uint32_t rt = offscreen(dd, 32, 32, PF_X8R8G8B8);
    const uint32_t other = offscreen(dd, 32, 32, PF_X8R8G8B8);
    for (const Pf *pf : {&PF_Z16, &PF_Z24, &PF_Z32}) {
        const uint32_t z = zbuffer(dd, 32, 32, *pf);
        CHECK(z != 0);
        CHECK_EQ(call_method(rt, S_AddAttachedSurface, {z}), 0);
        CHECK_EQ(call_method(rt, S_AddAttachedSurface, {z}), d7::DDERR_SURFACEALREADYATTACHED);
        CHECK_EQ(attached(rt, d7::DDSCAPS_ZBUFFER), z);
        call_method(z, Release, {});
        CHECK_EQ(call_method(rt, S_DeleteAttachedSurface, {0, z}), 0);
        CHECK_EQ(call_method(rt, S_DeleteAttachedSurface, {0, z}), d7::DDERR_SURFACENOTATTACHED);
        CHECK_EQ(call_method(z, Release, {}), 0);
    }
    CHECK_EQ(call_method(rt, S_AddAttachedSurface, {other}), d7::DDERR_CANNOTATTACHSURFACE);
    call_method(other, Release, {});
    call_method(rt, Release, {});
    call_method(dd, Release, {});
}

// Lock: the pointer for a rectangle, pitch, format round trips, NOTLOCKED.
static void test_lock() {
    cpu_reset();
    const uint32_t dd = create_dd();
    for (const Pf *pf : {&PF_A8R8G8B8, &PF_X8R8G8B8, &PF_R5G6B5, &PF_A4R4G4B4, &PF_A1R5G5B5}) {
        const uint32_t s = offscreen(dd, 20, 10, *pf, false);
        CHECK(s != 0);
        const uint32_t d = gmem(d7::DDSD2_SIZE);
        wr32(d, d7::DDSD2_SIZE);
        const uint32_t r = rect(3, 2, 10, 9);
        CHECK_EQ(call_method(s, S_Lock, {r, d, d7::DDLOCK_WAIT, 0}), 0);
        const uint32_t ptr = rd32(d + d7::DDSD2_lpSurface), pitch = rd32(d + d7::DDSD2_lPitch);
        CHECK(pitch >= 20 * pf->bpp / 8);
        CHECK_EQ(rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRBitMask), pf->r);
        CHECK_EQ(call_method(s, S_Unlock, {r}), 0);
        CHECK_EQ(call_method(s, S_Unlock, {0}), d7::DDERR_NOTLOCKED);
        CHECK_EQ(call_method(s, S_Lock, {0, d, 0, 0}), 0);
        const uint32_t base = rd32(d + d7::DDSD2_lpSurface);
        CHECK_EQ(ptr, base + 2 * pitch + 3 * pf->bpp / 8);
        CHECK_EQ(call_method(s, S_Unlock, {0}), 0);
        // A pixel written raw reads back as the colour its masks say.
        write(s, [pf](int x, int) { return x == 5 ? (pf->r | pf->a) : pf->b; });
        Image img = read(s);
        CHECK_EQ(img.at(5, 3) & 0xffffff, 0xff0000);
        CHECK_EQ(img.at(4, 3) & 0xffffff, 0x0000ff);
        CHECK_EQ(img.at(5, 3) >> 24, 0xff); // alpha set, or a format with none
        const uint32_t bad = rect(5, 5, 21, 6);
        CHECK_EQ(call_method(s, S_Lock, {bad, d, 0, 0}), d7::DDERR_INVALIDRECT);
        heap_free(bad);
        heap_free(r);
        heap_free(d);
        call_method(s, Release, {});
    }
    call_method(dd, Release, {});
}

// Blt: colour fills in the destination format, copies and stretches, source
// keys (the surface's and an override), mirroring, format conversion,
// clipping and an overlapping self-blit. CPU and renderer agree exactly.
static std::vector<Image> g_blt_results[2];
static void blt_cases(bool soft) {
    cpu_reset();
    std::vector<Image> &out = g_blt_results[soft ? 1 : 0];
    out.clear();
    const uint32_t dd = create_dd();
    const uint32_t dst = offscreen(dd, 32, 32, PF_R5G6B5);
    CHECK_EQ(fill(dst, 0xf800), 0); // red in 5-6-5
    Image red = read(dst);
    CHECK_EQ(red.at(7, 7), 0xffff0000u);
    out.push_back(red);
    // An 8x8 source with a key colour in its middle.
    const uint32_t src = offscreen(dd, 8, 8, PF_A4R4G4B4, false);
    write(src, [](int x, int y) {
        return (x >= 3 && x <= 4 && y >= 3 && y <= 4) ? 0xf0f0u : 0xf00fu + uint32_t(x << 4);
    });
    CHECK_EQ(set_src_key(src, 0xf0f0u), 0);
    const uint32_t r8 = rect(0, 0, 8, 8);
    // Plain copy.
    uint32_t d = rect(0, 0, 8, 8);
    CHECK_EQ(call_method(dst, S_Blt, {d, src, 0, d7::DDBLT_DONOTWAIT, 0}), 0);
    heap_free(d);
    // Keyed copy, stretched 2x.
    d = rect(8, 0, 24, 16);
    CHECK_EQ(call_method(dst, S_Blt, {d, src, r8, d7::DDBLT_DONOTWAIT | d7::DDBLT_KEYSRC, 0}), 0);
    heap_free(d);
    // Shrunk 2x with an override key, mirrored left-right.
    const uint32_t fx = gmem(d7::DDBLTFX_SIZE);
    wr32(fx, d7::DDBLTFX_SIZE);
    wr32(fx + d7::DDBLTFX_ddckSrcColorkey, 0xf00fu);
    wr32(fx + d7::DDBLTFX_ddckSrcColorkey + 4, 0xf00fu);
    wr32(fx + d7::DDBLTFX_dwDDFX, d7::DDBLTFX_MIRRORLEFTRIGHT);
    d = rect(24, 0, 28, 4);
    CHECK_EQ(call_method(dst, S_Blt, {d, src, r8, d7::DDBLT_KEYSRCOVERRIDE | d7::DDBLT_DDFX, fx}),
             0);
    heap_free(d);
    // Clipped: the destination runs off the bottom-right corner.
    d = rect(28, 28, 36, 36);
    CHECK_EQ(call_method(dst, S_Blt, {d, src, r8, 0, 0}), 0);
    heap_free(d);
    // Overlapping self-blit: shift a region right by 2.
    d = rect(2, 16, 18, 24);
    const uint32_t s2 = rect(0, 16, 16, 24);
    CHECK_EQ(fill(dst, 0x07e0, 0, 16, 4, 24), 0);
    CHECK_EQ(call_method(dst, S_Blt, {d, dst, s2, 0, 0}), 0);
    heap_free(d);
    heap_free(s2);
    // BltFast with the source key.
    CHECK_EQ(call_method(dst, S_BltFast, {20, 20, src, r8, d7::DDBLTFAST_SRCCOLORKEY}), 0);
    Image img = read(dst);
    out.push_back(img);
    // Hand-checked pixels. Source texel (x, y) is blue with green x, and the
    // key colour (opaque green) in the middle; the copy converts to 5-6-5.
    CHECK_EQ(img.at(0, 0), 0xff0000ffu);
    CHECK_EQ(img.at(3, 3) & 0xffffff, 0x00ff00);     // the key colour, unkeyed copy
    CHECK_EQ(img.at(8 + 6, 6) & 0xffffff, 0xff0000); // keyed: the destination shows
    CHECK_EQ(img.at(8 + 2, 2) & 0xffffff, 0x0010ff); // texel (1, 1), stretched 2x
    CHECK_EQ(img.at(24, 0) & 0xffffff, 0x0065ff);    // mirrored: texel (6, 1)
    CHECK_EQ(img.at(27, 0) & 0xffffff, 0xff0000);    // texel (0, 1): the override key
    CHECK_EQ(img.at(28, 28) & 0xffffff, 0x0000ff);   // clipped copy keeps its origin
    CHECK_EQ(img.at(31, 31) & 0xffffff, 0x00ff00);
    CHECK_EQ(img.at(4, 18) & 0xffffff, 0x00ff00); // the self-blit read the original
    CHECK_EQ(img.at(6, 18) & 0xffffff, 0xff0000);
    CHECK_EQ(img.at(23, 23) & 0xffffff, 0xff0000); // BltFast honoured the key
    CHECK_EQ(img.at(22, 20) & 0xffffff, 0x0020ff); // and copied texel (2, 0)
    heap_free(fx);
    heap_free(r8);
    call_method(src, Release, {});
    call_method(dst, Release, {});
    call_method(dd, Release, {});
}
static void test_blt() {
    both_paths(blt_cases);
    CHECK_EQ(g_blt_results[0].size(), g_blt_results[1].size());
    for (size_t i = 0; i < std::min(g_blt_results[0].size(), g_blt_results[1].size()); ++i)
        CHECK(g_blt_results[0][i].px == g_blt_results[1][i].px);
}

// GetDC: GDI draws into the surface; a 4-4-4-4 surface keeps its alpha.
static void test_get_dc() {
    cpu_reset();
    const uint32_t dd = create_dd();
    const uint32_t set_pixel = tramp("GDI32.dll", "SetPixel");
    CHECK(set_pixel != 0);
    for (const Pf *pf : {&PF_R5G6B5, &PF_A4R4G4B4, &PF_X8R8G8B8}) {
        const uint32_t s = texture(dd, 16, 16, *pf);
        write(s, [pf](int, int) { return pf->a & 0x7000; }); // some alpha bits
        const uint32_t out = gmem(4);
        CHECK_EQ(call_method(s, S_GetDC, {out}), 0);
        const uint32_t hdc = rd32(out);
        CHECK(hdc != 0);
        CHECK_EQ(call_method(s, S_GetDC, {out}), d7::DDERR_DCALREADYCREATED);
        call_shim(set_pixel, {hdc, 3, 4, 0x000000ffu}); // COLORREF red
        CHECK_EQ(call_method(s, S_ReleaseDC, {hdc}), 0);
        CHECK_EQ(call_method(s, S_ReleaseDC, {hdc}), d7::DDERR_NODC);
        Image img = read(s);
        CHECK_EQ(img.at(3, 4) & 0xffffff, 0xff0000);
        if (pf->a)
            CHECK_EQ(img.at(3, 4) >> 24, uint32_t(0x7 * 0x11)); // 0x7 of 0xf
        heap_free(out);
        call_method(s, Release, {});
    }
    call_method(dd, Release, {});
}

// ---------------------------------------------------------------------------
// Direct3D 7
// ---------------------------------------------------------------------------
struct DeviceSeen {
    std::string description, name;
    uint32_t caps;
    uint8_t guid[16];
};
static std::vector<DeviceSeen> g_devices;
static void device_callback(X86 *c) {
    DeviceSeen d;
    d.description = gm_str(arg(c, 0));
    d.name = gm_str(arg(c, 1));
    d.caps = rd32(arg(c, 2) + d7::D3DDD7_dwDevCaps);
    memcpy(d.guid, gm_ptr(arg(c, 2) + d7::D3DDD7_deviceGUID), 16);
    g_devices.push_back(d);
    set_eax(c, 1);
}
static std::vector<d7::Format> g_formats;
static void format_callback(X86 *c) {
    g_formats.push_back(d7::read_format(arg(c, 0)));
    set_eax(c, 1);
}
static void test_d3d_enumeration() {
    cpu_reset();
    const uint32_t dd = create_dd();
    const uint32_t d3d = query(dd, d7::IID_IDirect3D7);
    const uint32_t cb =
        imports_alloc_trampoline("d3d7_tests", "device_callback", device_callback, 4);
    g_devices.clear();
    CHECK_EQ(call_method(d3d, D3D_EnumDevices, {cb, 0}), 0);
    CHECK_EQ(g_devices.size(), 2);
    if (g_devices.size() == 2) {
        CHECK(g_devices[0].caps & d7::D3DDEVCAPS_HWRASTERIZATION);
        CHECK(!(g_devices[0].caps & d7::D3DDEVCAPS_HWTRANSFORMANDLIGHT));
        CHECK(!memcmp(g_devices[0].guid, d7::IID_IDirect3DHALDevice, 16));
        CHECK_EQ(g_devices[1].caps & 0x90000u, 0x90000u);
        CHECK(!memcmp(g_devices[1].guid, d7::IID_IDirect3DTnLHalDevice, 16));
    }
    const uint32_t fcb =
        imports_alloc_trampoline("d3d7_tests", "format_callback", format_callback, 2);
    g_formats.clear();
    CHECK_EQ(call_method(d3d, D3D_EnumZBufferFormats, {0, fcb, 0}), 0);
    CHECK_EQ(g_formats.size(), 3);
    for (const d7::Format &f : g_formats)
        CHECK(f.depth());

    const uint32_t rt = offscreen(dd, 16, 16, PF_X8R8G8B8);
    for (const uint8_t *guid :
         {d7::IID_IDirect3DHALDevice, d7::IID_IDirect3DTnLHalDevice, d7::IID_IDirect3DRGBDevice}) {
        const uint32_t dev = create_device(d3d, rt, guid);
        CHECK(dev != 0);
        const uint32_t desc = gmem(d7::D3DDD7_SIZE);
        CHECK_EQ(call_method(dev, DEV_GetCaps, {desc}), 0);
        CHECK(rd32(desc + d7::D3DDD7_dwDeviceRenderBitDepth) & d7::DDBD_32);
        CHECK(rd32(desc + d7::D3DDD7_dwDeviceRenderBitDepth) & d7::DDBD_16);
        CHECK_EQ(rd32(desc + d7::D3DDD7_dwMaxActiveLights), 8);
        g_formats.clear();
        CHECK_EQ(call_method(dev, DEV_EnumTextureFormats, {fcb, 0}), 0);
        bool argb = false, a4 = false;
        for (const d7::Format &f : g_formats) {
            argb |= f.bpp == 32 && f.a == 0xff000000u && f.r == 0xff0000;
            a4 |= f.bpp == 16 && f.a == 0xf000 && f.r == 0x0f00;
        }
        CHECK(argb);
        CHECK(a4);
        const uint32_t passes = gmem(4);
        CHECK_EQ(call_method(dev, DEV_ValidateDevice, {passes}), 0);
        CHECK_EQ(rd32(passes), 1);
        heap_free(passes);
        heap_free(desc);
        call_method(dev, Release, {});
    }
    const uint32_t g = gmem(16), out = gmem(4);
    memset(gm_ptr(g), 0x5a, 16);
    CHECK_EQ(call_method(d3d, D3D_CreateDevice, {g, rt, out}), d7::D3DERR_INVALID_DEVICE);
    // The engine's one vertex buffer: FVF 0x152, 2442 vertices, NOOVERWRITE locks.
    const uint32_t desc = gmem(16);
    wr32(desc, 16);
    wr32(desc + 4, d7::VBCAPS_SYSTEMMEMORY | d7::VBCAPS_WRITEONLY);
    wr32(desc + 8, 0x152);
    wr32(desc + 12, 2442);
    CHECK_EQ(call_method(d3d, D3D_CreateVertexBuffer, {desc, out, 0}), 0);
    const uint32_t vb = rd32(out), size = gmem(4);
    CHECK_EQ(call_method(vb, VB_Lock, {0x1020, out, size}), 0);
    CHECK(rd32(out) != 0);
    CHECK_EQ(rd32(size), 2442u * 36);
    CHECK_EQ(call_method(vb, VB_Unlock, {}), 0);
    wr32(desc + 8, 0x0f0f0f0f);
    CHECK_EQ(call_method(d3d, D3D_CreateVertexBuffer, {desc, out, 0}),
             d7::D3DERR_INVALIDVERTEXFORMAT);
    call_method(vb, Release, {});
    for (uint32_t a : {g, out, desc, size})
        heap_free(a);
    call_method(rt, Release, {});
    call_method(d3d, Release, {});
    call_method(dd, Release, {});
}

// Device state round trips: lights (and the default light LightEnable makes),
// render states, transforms, viewports.
static void test_device_state() {
    cpu_reset();
    Device d = make_device(16, 16);
    Light l;
    l.type = d7::LIGHT_POINT;
    l.position[0] = 3;
    l.range = 7;
    l.att1 = 0.5f;
    set_light(d.dev, 2, l);
    const uint32_t a = gmem(d7::D3DLIGHT7_SIZE);
    CHECK_EQ(call_method(d.dev, DEV_GetLight, {2, a}), 0);
    CHECK_EQ(rd32(a), d7::LIGHT_POINT);
    CHECK_EQ(rd32(a + d7::L7_dvPosition), fbits(3));
    CHECK_EQ(rd32(a + d7::L7_dvRange), fbits(7));
    CHECK_EQ(rd32(a + d7::L7_dvAttenuation1), fbits(0.5f));
    CHECK_EQ(call_method(d.dev, DEV_GetLight, {5, a}), DDERR_INVALIDPARAMS);
    CHECK_EQ(call_method(d.dev, DEV_LightEnable, {5, 1}), 0);
    CHECK_EQ(call_method(d.dev, DEV_GetLight, {5, a}), 0);
    CHECK_EQ(rd32(a), d7::LIGHT_DIRECTIONAL);
    CHECK_EQ(rd32(a + d7::L7_dcvDiffuse), fbits(1));
    wr32(a, 9);
    CHECK_EQ(call_method(d.dev, DEV_SetLight, {1, a}), DDERR_INVALIDPARAMS);
    // Render states.
    const uint32_t v = gmem(64);
    rs(d.dev, d7::RS_AMBIENT, 0xff102030u);
    CHECK_EQ(call_method(d.dev, 21, {d7::RS_AMBIENT, v}), 0); // GetRenderState
    CHECK_EQ(rd32(v), 0xff102030u);
    CHECK_EQ(call_method(d.dev, DEV_SetRenderState, {0, 1}), DDERR_INVALIDPARAMS);
    // A matrix round trip.
    set_transform(d.dev, d7::TS_WORLD, translate(1, 2, 3));
    CHECK_EQ(call_method(d.dev, 12, {d7::TS_WORLD, v}), 0); // GetTransform
    CHECK_EQ(rd32(v + 52), fbits(2));
    viewport(d.dev, 1, 2, 10, 12, 0.25f, 0.25f);
    CHECK_EQ(call_method(d.dev, 15, {v}), 0); // GetViewport
    CHECK_EQ(rd32(v), 1);
    CHECK_EQ(rd32(v + 12), 12);
    CHECK_EQ(rd32(v + 16), fbits(0.25f));
    CHECK_EQ(rd32(v + 20), fbits(0.25f));
    // BeginScene twice, EndScene twice.
    CHECK_EQ(call_method(d.dev, DEV_BeginScene, {}), 0);
    CHECK_EQ(call_method(d.dev, DEV_BeginScene, {}), d7::D3DERR_SCENE_IN_SCENE);
    CHECK_EQ(call_method(d.dev, DEV_EndScene, {}), 0);
    CHECK_EQ(call_method(d.dev, DEV_EndScene, {}), d7::D3DERR_SCENE_NOT_IN_SCENE);
    heap_free(a);
    heap_free(v);
    release_all(d);
}

// Without a renderer, draws are accepted and dropped, and Clear still fills
// the target on the CPU.
static void test_cpu_only_device() {
    cpu_reset();
    d7::use_soft_backend(false);
    Device d = make_device(16, 16);
    CHECK_EQ(clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff123456u), 0);
    call_method(d.dev, DEV_BeginScene, {});
    CHECK_EQ(draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 8, 8, 0xffffffffu), QUAD_INDICES), 0);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    CHECK_EQ(img.at(2, 2) & 0xffffff, 0x123456);
    release_all(d);
}

// Coherence on the renderer: a texture filled by Lock reaches a draw; a
// target the renderer drew is read back by Lock; a renderer Blt between two
// renderer surfaces leaves the guest copies stale rather than reading back.
static void test_coherence() {
    cpu_reset();
    d7::use_soft_backend(true);
    Device d = make_device(16, 16);
    const uint32_t tex = texture(d.dd, 2, 2, PF_X8R8G8B8);
    write(tex, [](int, int) { return 0x00abcdefu; });
    d7::Surface *ts = d7::surface_from_guest(tex), *rt = d7::surface_from_guest(d.rt);
    CHECK(ts && rt);
    int w = 0, h = 0;
    CHECK(d7soft::pixels(ts->id, 0, &w, &h) == nullptr); // not uploaded until used
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 16, 16, 0xffffffffu), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    CHECK(d7soft::pixels(ts->id, 0, &w, &h) != nullptr);
    CHECK(!rt->cpu_valid && rt->gpu_valid);
    CHECK_EQ(read(d.rt).at(5, 5) & 0xffffff, 0xabcdef);
    CHECK(rt->cpu_valid && rt->gpu_valid);
    // Blt from the target into another video-memory surface.
    const uint32_t copy = offscreen(d.dd, 16, 16, PF_X8R8G8B8);
    d7::Surface *cs = d7::surface_from_guest(copy);
    CHECK_EQ(call_method(copy, S_Blt, {0, d.rt, 0, d7::DDBLT_DONOTWAIT, 0}), 0);
    CHECK(!cs->cpu_valid && cs->gpu_valid);
    CHECK(rt->cpu_valid); // the source was not touched
    CHECK_EQ(read(copy).at(1, 1) & 0xffffff, 0xabcdef);
    call_method(copy, Release, {});
    release_all(d, {tex});
    d7::use_soft_backend(false);
}

// ---------------------------------------------------------------------------
// Scenes on the reference renderer, against hand-computed pixels
// ---------------------------------------------------------------------------
static Image run_soft(Image (*scene)()) {
    cpu_reset();
    d7::use_soft_backend(true);
    Image img = scene();
    d7::use_soft_backend(false);
    return img;
}
static void test_scene_values() {
    // Coverage: pixels 8..23 of the quad, and the sliver's two columns.
    Image q = run_soft(scene_quad_coverage);
    CHECK_EQ(q.at(8, 8) & 0xffffff, 0xff8000);
    CHECK_EQ(q.at(23, 23) & 0xffffff, 0xff8000);
    CHECK_EQ(q.at(7, 8) & 0xffffff, 0x000040);
    CHECK_EQ(q.at(24, 8) & 0xffffff, 0x000040);
    CHECK_EQ(q.at(8, 24) & 0xffffff, 0x000040);
    CHECK_EQ(q.at(27, 4) & 0xffffff, 0x00ff00);
    CHECK_EQ(q.at(28, 5) & 0xffffff, 0x00ff00);
    CHECK_EQ(q.at(26, 4) & 0xffffff, 0x000040);
    CHECK_EQ(q.at(29, 4) & 0xffffff, 0x000040);
    CHECK_EQ(q.at(27, 6) & 0xffffff, 0x000040);
    // Flat shading: the first vertex's colour throughout.
    Image g = run_soft(scene_gouraud);
    CHECK_EQ(g.at(50, 50) & 0xffffff, 0xffff00);
    CHECK_EQ(g.at(55, 47) & 0xffffff, 0xffff00);
    CHECK(channel(g.at(6, 6), 2) > 200); // near the red vertex
    // Point sampling: texel (x, y) fills block (4x.., 4y..).
    Image t = run_soft(scene_texture_point);
    CHECK_EQ(t.at(1, 1) & 0xffffff, 0x800000);
    CHECK_EQ(t.at(5, 1) & 0xffffff, 0xffffff);
    CHECK_EQ(t.at(11, 3) & 0xffffff, 0x807800);  // texel (2, 0): x*60 = 120 green
    CHECK_EQ(t.at(15, 15) & 0xffffff, 0x80b4b4); // texel (3, 3)
    // Directional light: 0.75, 0.45, 0.5 facing; 0.5, 0.35, 0.25 at 60 degrees.
    Image l = run_soft(scene_light_directional);
    CHECK_NEAR(l.at(20, 16), 0xffbf7380u, 2);
    CHECK_NEAR(l.at(44, 16), 0xff805940u, 2);
    // Mips: levels 0, 1, 2; MAXMIPLEVEL 3; LOD bias 1.
    Image m = run_soft(scene_mips);
    CHECK_EQ(m.at(32, 32) & 0xffffff, 0xff0000);
    CHECK_EQ(m.at(80, 16) & 0xffffff, 0x00ff00);
    CHECK_EQ(m.at(104, 8) & 0xffffff, 0x0000ff);
    CHECK_EQ(m.at(120, 8) & 0xffffff, 0xffff00);
    CHECK_EQ(m.at(120, 40) & 0xffffff, 0x00ff00);
    // The combiner, column by column.
    Image c = run_soft(scene_combiner);
    const uint32_t expect[8] = {0xc06020, 0x808080, 0x603010, 0x603010,
                                0xffe0a0, 0xbf6020, 0xa07050, 0x808080};
    for (int i = 0; i < 8; ++i)
        CHECK_NEAR(c.at(8 * i + 4, 8), 0xff000000u | expect[i], 1);
    Image ca = run_soft(scene_combiner_alpha);
    const uint32_t alphas[4] = {0x80, 0x40, 0x20, 0xc0};
    for (int i = 0; i < 4; ++i) {
        CHECK(std::abs(int(ca.at(16 * i + 8, 8) >> 24) - int(alphas[i])) <= 1);
        CHECK_EQ(ca.at(16 * i + 8, 8) & 0xffffff, 0xc06020);
    }
    // Alpha test GREATER 0x80, then blends.
    Image a = run_soft(scene_alpha);
    CHECK_EQ(a.at(31, 8) & 0xffffff, 0x000080);
    CHECK_EQ(a.at(32, 8) & 0xffffff, 0xffff00);
    CHECK_NEAR(a.at(29, 20), 0xff777744u, 1); // texel 7: a = 119
    CHECK_NEAR(a.at(8, 28), 0xffffff80u, 1);  // ONE, ONE
    CHECK_NEAR(a.at(61, 28), 0xffeeee09u, 1); // BOTHSRCALPHA, texel 14: a = 238
    // Depth.
    Image z = run_soft(scene_depth);
    CHECK_EQ(z.at(4, 4) & 0xffffff, 0xff0000);
    CHECK_EQ(z.at(20, 4) & 0xffffff, 0x00ff00);
    CHECK_EQ(z.at(12, 12) & 0xffffff, 0x0000ff);
    CHECK_EQ(z.at(30, 12) & 0xffffff, 0x00ff00);
    CHECK_EQ(z.at(44, 20) & 0xffffff, 0x0000ff);
    CHECK_EQ(z.at(60, 4) & 0xffffff, 0xff00ff);
    CHECK_EQ(z.at(53, 4) & 0xffffff, 0x00ffff);
    CHECK_EQ(z.at(45, 28) & 0xffffff, 0xffff00);
    CHECK_EQ(z.at(20, 28) & 0xffffff, 0xff0000);
    CHECK_EQ(z.at(4, 20) & 0xffffff, 0x808080);
    // Culling: none, CW, CCW.
    Image k = run_soft(scene_cull);
    CHECK_EQ(k.at(3, 4) & 0xffffff, 0xff0000);
    CHECK_EQ(k.at(12, 4) & 0xffffff, 0x00ff00);
    CHECK_EQ(k.at(19, 4) & 0xffffff, 0x000000);
    CHECK_EQ(k.at(28, 4) & 0xffffff, 0x00ff00);
    CHECK_EQ(k.at(35, 4) & 0xffffff, 0xff0000);
    CHECK_EQ(k.at(44, 4) & 0xffffff, 0x000000);
    // Colour-keyed texels show the clear colour; a rewritten texture is used.
    Image ck = run_soft(scene_colorkey_texture);
    CHECK_EQ(ck.at(1, 1) & 0xffffff, 0x0000ff);
    CHECK_EQ(ck.at(5, 1) & 0xffffff, 0xff0044);
    CHECK_EQ(ck.at(17, 1) & 0xffffff, 0x0000ff);
    CHECK_EQ(ck.at(21, 1) & 0xffffff, 0x00ee00);
    CHECK_EQ(ck.at(29, 1) & 0xffffff, 0x0000ff);
    // Render to texture, sampled 1:1.
    Image rtt = run_soft(scene_render_to_texture);
    CHECK_EQ(rtt.at(12, 12) & 0xffffff, 0xff0000);
    CHECK_EQ(rtt.at(9, 9) & 0xffffff, 0x00ff00);
    CHECK_EQ(rtt.at(2, 2) & 0xffffff, 0x101010);
    // The 5-6-5 target through the vertex buffer.
    Image vb = run_soft(scene_vb_565);
    CHECK_EQ(vb.at(4, 4) & 0xffffff, 0xff0000);
    CHECK_EQ(vb.at(20, 20) & 0xffffff, 0x00ff00);
    CHECK_EQ(vb.at(4, 20) & 0xffffff, 0x3165ce);
    CHECK_EQ(vb.at(20, 4) & 0xffffff, 0x848284);
    // Every scene runs to completion on the reference renderer.
    for (const Scene &s : scenes()) {
        Image img = run_soft(s.run);
        CHECK(img.w > 0 && img.h > 0);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"create and query", test_create_and_query},
        {"display modes", test_display_modes},
        {"cooperative level and mode", test_cooperative_level_and_mode},
        {"flip chain", [] { both_paths(flip_chain); }},
        {"windowed present", [] { both_paths(windowed_present); }},
        {"mip chain", test_mip_chain},
        {"depth buffers", test_depth_buffers},
        {"lock", test_lock},
        {"blt", test_blt},
        {"GetDC", test_get_dc},
        {"Direct3D enumeration", test_d3d_enumeration},
        {"device state", test_device_state},
        {"CPU-only device", test_cpu_only_device},
        {"coherence", test_coherence},
        {"scenes on the reference renderer", test_scene_values},
    };
    for (auto &t : tests) {
        const int before = g_failures;
        t.fn();
        printf("%-34s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
