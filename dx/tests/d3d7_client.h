// d3d7_client.h - a small DirectDraw 7 / Direct3D 7 program, driven through
// the guest ABI exactly as translated code would call it, for the headless
// shim tests (d3d7_tests.cpp) and the GPU renderer tests
// (host/tests/d3d7_gpu_tests.cpp).
//
// Include guest_abi.h first. Every call goes through the interface's real
// vtable with call_method, so a wrong slot or argument count fails here.
#pragma once
#include "guest_abi.h"
#include "../d3d7_internal.h"
#include "../d3d7_types.h"
#include "../../runtime/memory.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifndef CHECK
static void d7t_check(bool ok, const char *what, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}
#define CHECK(x) d7t_check((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        uint64_t va = (uint64_t)(a), vb = (uint64_t)(b);                                           \
        ++g_checks;                                                                                \
        if (va != vb) {                                                                            \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a,   \
                    #b, (unsigned long long)va, (unsigned long long)vb);                           \
        }                                                                                          \
    } while (0)
#endif

namespace d7t {

// Vtable slots, counted with IUnknown as 0..2 (DirectX 7 SDK order).
enum : uint32_t {
    QI = 0,
    AddRef = 1,
    Release = 2,
    // IDirectDraw7
    DD_CreateSurface = 6,
    DD_EnumDisplayModes = 8,
    DD_GetCaps = 11,
    DD_GetDisplayMode = 12,
    DD_SetCooperativeLevel = 20,
    DD_SetDisplayMode = 21,
    DD_GetAvailableVidMem = 23,
    DD_RestoreAllSurfaces = 25,
    DD_TestCooperativeLevel = 26,
    DD_GetDeviceIdentifier = 27,
    // IDirectDrawSurface7
    S_AddAttachedSurface = 3,
    S_Blt = 5,
    S_BltFast = 7,
    S_DeleteAttachedSurface = 8,
    S_Flip = 11,
    S_GetAttachedSurface = 12,
    S_GetCaps = 14,
    S_GetColorKey = 16,
    S_GetDC = 17,
    S_GetPixelFormat = 21,
    S_GetSurfaceDesc = 22,
    S_IsLost = 24,
    S_Lock = 25,
    S_ReleaseDC = 26,
    S_Restore = 27,
    S_SetColorKey = 29,
    S_Unlock = 32,
    S_GetDDInterface = 36,
    // IDirect3D7
    D3D_EnumDevices = 3,
    D3D_CreateDevice = 4,
    D3D_CreateVertexBuffer = 5,
    D3D_EnumZBufferFormats = 6,
    // IDirect3DDevice7
    DEV_GetCaps = 3,
    DEV_EnumTextureFormats = 4,
    DEV_BeginScene = 5,
    DEV_EndScene = 6,
    DEV_SetRenderTarget = 8,
    DEV_Clear = 10,
    DEV_SetTransform = 11,
    DEV_SetViewport = 13,
    DEV_SetMaterial = 16,
    DEV_SetLight = 18,
    DEV_GetLight = 19,
    DEV_SetRenderState = 20,
    DEV_DrawPrimitive = 25,
    DEV_DrawIndexedPrimitive = 26,
    DEV_DrawPrimitiveVB = 31,
    DEV_DrawIndexedPrimitiveVB = 32,
    DEV_SetTexture = 35,
    DEV_SetTextureStageState = 37,
    DEV_ValidateDevice = 38,
    DEV_LightEnable = 44,
    // IDirect3DVertexBuffer7
    VB_Lock = 3,
    VB_Unlock = 4,
};

inline uint32_t gmem(uint32_t bytes) {
    return heap_alloc(bytes, true, 16);
}
inline uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Pixel formats, as the guest describes them.
struct Pf {
    uint32_t flags, bpp, r, g, b, a;
};
const Pf PF_A8R8G8B8{d7::DDPF_RGB | d7::DDPF_ALPHAPIXELS, 32, 0xff0000, 0xff00, 0xff, 0xff000000u};
const Pf PF_X8R8G8B8{d7::DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0};
const Pf PF_R5G6B5{d7::DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0};
const Pf PF_A4R4G4B4{d7::DDPF_RGB | d7::DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000};
const Pf PF_A1R5G5B5{d7::DDPF_RGB | d7::DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000};
const Pf PF_Z16{d7::DDPF_ZBUFFER, 16, 0, 0xffff, 0, 0};
const Pf PF_Z24{d7::DDPF_ZBUFFER, 32, 0, 0xffffff, 0, 0};
const Pf PF_Z32{d7::DDPF_ZBUFFER, 32, 0, 0xffffffffu, 0, 0};

// An image read back through Lock, as BGRA8 words (0xAARRGGBB).
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    uint32_t at(int x, int y) const {
        return px[size_t(y) * size_t(w) + size_t(x)];
    }
};
inline int channel(uint32_t c, int i) { // 0 b, 1 g, 2 r, 3 a
    return int((c >> (8 * i)) & 0xff);
}
inline int max_diff(uint32_t a, uint32_t b, bool alpha = false) {
    int m = 0;
    for (int i = 0; i < (alpha ? 4 : 3); ++i)
        m = std::max(m, std::abs(channel(a, i) - channel(b, i)));
    return m;
}

// ---------------------------------------------------------------------------
// DirectDraw
// ---------------------------------------------------------------------------
inline uint32_t create_dd() {
    d7::ddraw7_enable_for_test(1);
    const uint32_t iid = gmem(16), out = gmem(4);
    memcpy(gm_ptr(iid), d7::IID_IDirectDraw7, 16);
    const uint32_t hr = call_shim(tramp("DDRAW.dll", "DirectDrawCreateEx"), {0, out, iid, 0});
    CHECK_EQ(hr, d7::DD_OK);
    const uint32_t dd = rd32(out);
    heap_free(iid);
    heap_free(out);
    return dd;
}
inline uint32_t query(uint32_t iface, const uint8_t iid[16]) {
    const uint32_t g = gmem(16), out = gmem(4);
    memcpy(gm_ptr(g), iid, 16);
    const uint32_t hr = call_method(iface, QI, {g, out});
    const uint32_t r = hr == 0 ? rd32(out) : 0;
    heap_free(g);
    heap_free(out);
    return r;
}

struct SurfaceSpec {
    uint32_t caps = 0, caps2 = 0;
    uint32_t w = 0, h = 0;
    const Pf *pf = nullptr;
    uint32_t back_buffers = 0;
    int mips = -1; // -1: no DDSD_MIPMAPCOUNT
    bool src_key = false;
    uint32_t key_lo = 0, key_hi = 0;
};
inline uint32_t write_pf(uint32_t a, const Pf &p) {
    wr32(a + d7::DDPF_dwSize, 32);
    wr32(a + d7::DDPF_dwFlags, p.flags);
    wr32(a + d7::DDPF_dwRGBBitCount, p.bpp);
    wr32(a + d7::DDPF_dwRBitMask, p.r);
    wr32(a + d7::DDPF_dwGBitMask, p.g);
    wr32(a + d7::DDPF_dwBBitMask, p.b);
    wr32(a + d7::DDPF_dwRGBAlphaBitMask, p.a);
    return a;
}
// CreateSurface; returns the IDirectDrawSurface7 pointer, 0 with *hr set on failure.
inline uint32_t create_surface(uint32_t dd, const SurfaceSpec &s, uint32_t *hr_out = nullptr) {
    const uint32_t d = gmem(d7::DDSD2_SIZE), out = gmem(4);
    uint32_t flags = d7::DDSD_CAPS;
    wr32(d + d7::DDSD2_dwSize, d7::DDSD2_SIZE);
    if (s.w) {
        flags |= d7::DDSD_WIDTH | d7::DDSD_HEIGHT;
        wr32(d + d7::DDSD2_dwWidth, s.w);
        wr32(d + d7::DDSD2_dwHeight, s.h);
    }
    if (s.pf) {
        flags |= d7::DDSD_PIXELFORMAT;
        write_pf(d + d7::DDSD2_ddpfPixelFormat, *s.pf);
    }
    if (s.back_buffers) {
        flags |= d7::DDSD_BACKBUFFERCOUNT;
        wr32(d + d7::DDSD2_dwBackBufferCount, s.back_buffers);
    }
    if (s.mips >= 0) {
        flags |= d7::DDSD_MIPMAPCOUNT;
        wr32(d + d7::DDSD2_dwMipMapCount, uint32_t(s.mips));
    }
    if (s.src_key) {
        flags |= d7::DDSD_CKSRCBLT;
        wr32(d + d7::DDSD2_ddckCKSrcBlt, s.key_lo);
        wr32(d + d7::DDSD2_ddckCKSrcBlt + 4, s.key_hi);
    }
    wr32(d + d7::DDSD2_dwFlags, flags);
    wr32(d + d7::DDSD2_ddsCaps, s.caps);
    wr32(d + d7::DDSD2_ddsCaps + 4, s.caps2);
    const uint32_t hr = call_method(dd, DD_CreateSurface, {d, out, 0});
    if (hr_out)
        *hr_out = hr;
    const uint32_t r = hr == 0 ? rd32(out) : 0;
    heap_free(d);
    heap_free(out);
    return r;
}
// An offscreen colour surface, optionally a render target.
inline uint32_t offscreen(uint32_t dd, uint32_t w, uint32_t h, const Pf &pf, bool target = true,
                          uint32_t memory = d7::DDSCAPS_VIDEOMEMORY) {
    SurfaceSpec s;
    s.caps = d7::DDSCAPS_OFFSCREENPLAIN | memory | (target ? d7::DDSCAPS_3DDEVICE : 0);
    s.w = w;
    s.h = h;
    s.pf = &pf;
    return create_surface(dd, s);
}
inline uint32_t texture(uint32_t dd, uint32_t w, uint32_t h, const Pf &pf, int mips = 0,
                        bool target = false) {
    SurfaceSpec s;
    s.caps = d7::DDSCAPS_TEXTURE | (mips ? d7::DDSCAPS_MIPMAP | d7::DDSCAPS_COMPLEX : 0) |
             (target ? d7::DDSCAPS_3DDEVICE | d7::DDSCAPS_VIDEOMEMORY : 0);
    s.caps2 = target ? 0 : d7::DDSCAPS2_TEXTUREMANAGE | d7::DDSCAPS2_HINTSTATIC;
    s.w = w;
    s.h = h;
    s.pf = &pf;
    s.mips = mips;
    return create_surface(dd, s);
}
inline uint32_t zbuffer(uint32_t dd, uint32_t w, uint32_t h, const Pf &pf = PF_Z16) {
    SurfaceSpec s;
    s.caps = d7::DDSCAPS_ZBUFFER | d7::DDSCAPS_VIDEOMEMORY;
    s.w = w;
    s.h = h;
    s.pf = &pf;
    return create_surface(dd, s);
}
inline uint32_t attached(uint32_t surf, uint32_t caps, uint32_t caps2 = 0, uint32_t *hr = nullptr) {
    const uint32_t c = gmem(16), out = gmem(4);
    wr32(c, caps);
    wr32(c + 4, caps2);
    const uint32_t r = call_method(surf, S_GetAttachedSurface, {c, out});
    if (hr)
        *hr = r;
    const uint32_t s = r == 0 ? rd32(out) : 0;
    heap_free(c);
    heap_free(out);
    return s;
}

// The surface's pixels through Lock, in BGRA8.
inline Image read(uint32_t surf) {
    Image img;
    const uint32_t d = gmem(d7::DDSD2_SIZE);
    wr32(d, d7::DDSD2_SIZE);
    if (call_method(surf, S_Lock, {0, d, d7::DDLOCK_READONLY, 0}) != 0) {
        heap_free(d);
        CHECK(!"Lock failed");
        return img;
    }
    img.w = int(rd32(d + d7::DDSD2_dwWidth));
    img.h = int(rd32(d + d7::DDSD2_dwHeight));
    const uint32_t pitch = rd32(d + d7::DDSD2_lPitch), mem = rd32(d + d7::DDSD2_lpSurface);
    const d7::Format f = d7::read_format(d + d7::DDSD2_ddpfPixelFormat);
    img.px.resize(size_t(img.w) * size_t(img.h));
    for (int y = 0; y < img.h; ++y)
        d7::row_to_bgra(f, gm_ptr(mem + uint32_t(y) * pitch),
                        img.px.data() + size_t(y) * size_t(img.w), img.w);
    call_method(surf, S_Unlock, {0});
    heap_free(d);
    return img;
}
// Writes every pixel through Lock: fn(x, y) gives the raw pixel value.
inline void write(uint32_t surf, const std::function<uint32_t(int, int)> &fn) {
    const uint32_t d = gmem(d7::DDSD2_SIZE);
    wr32(d, d7::DDSD2_SIZE);
    CHECK_EQ(call_method(surf, S_Lock, {0, d, 0, 0}), 0);
    const int w = int(rd32(d + d7::DDSD2_dwWidth)), h = int(rd32(d + d7::DDSD2_dwHeight));
    const uint32_t pitch = rd32(d + d7::DDSD2_lPitch), mem = rd32(d + d7::DDSD2_lpSurface);
    const uint32_t bytes = rd32(d + d7::DDSD2_ddpfPixelFormat + d7::DDPF_dwRGBBitCount) / 8;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t v = fn(x, y);
            memcpy(gm_ptr(mem + uint32_t(y) * pitch + uint32_t(x) * bytes), &v, bytes);
        }
    CHECK_EQ(call_method(surf, S_Unlock, {0}), 0);
    heap_free(d);
}
inline uint32_t rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const uint32_t r = gmem(16);
    wr32(r, uint32_t(x0));
    wr32(r + 4, uint32_t(y0));
    wr32(r + 8, uint32_t(x1));
    wr32(r + 12, uint32_t(y1));
    return r;
}
inline uint32_t fill(uint32_t surf, uint32_t value, int32_t x0 = 0, int32_t y0 = 0, int32_t x1 = -1,
                     int32_t y1 = -1) {
    const uint32_t fx = gmem(d7::DDBLTFX_SIZE);
    wr32(fx, d7::DDBLTFX_SIZE);
    wr32(fx + d7::DDBLTFX_dwFillColor, value);
    const uint32_t r = x1 < 0 ? 0 : rect(x0, y0, x1, y1);
    const uint32_t hr =
        call_method(surf, S_Blt, {r, 0, 0, d7::DDBLT_COLORFILL | d7::DDBLT_WAIT, fx});
    if (r)
        heap_free(r);
    heap_free(fx);
    return hr;
}
inline uint32_t set_src_key(uint32_t surf, uint32_t value) {
    const uint32_t k = gmem(8);
    wr32(k, value);
    wr32(k + 4, value);
    const uint32_t hr = call_method(surf, S_SetColorKey, {d7::DDCKEY_SRCBLT, k});
    heap_free(k);
    return hr;
}

// ---------------------------------------------------------------------------
// Direct3D
// ---------------------------------------------------------------------------
// The vertex the tests draw: FVF XYZ | NORMAL | DIFFUSE | TEX1 (0x152).
struct Vertex {
    float x, y, z, nx, ny, nz;
    uint32_t diffuse;
    float u, v;
};
static_assert(sizeof(Vertex) == 36, "D3DFVF 0x152 is 36 bytes");
const uint32_t FVF_152 = 0x152;

using Matrix = std::array<float, 16>;
inline Matrix identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
// The 2D projection: x' = 2x/W - 1, y' = 1 - 2y/H, z unchanged.
inline Matrix ortho2d(float w, float h) {
    return {2 / w, 0, 0, 0, 0, -2 / h, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
}
// A left-handed perspective projection (D3DXMatrixPerspectiveFovLH).
inline Matrix perspective(float fov, float aspect, float zn, float zf) {
    const float ys = 1.0f / std::tan(fov / 2), xs = ys / aspect, q = zf / (zf - zn);
    return {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -zn * q, 0};
}
inline Matrix translate(float x, float y, float z) {
    Matrix m = identity();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

struct Device {
    uint32_t dd = 0, d3d = 0, dev = 0, rt = 0, z = 0;
};
inline uint32_t create_device(uint32_t d3d, uint32_t rt, const uint8_t *guid = nullptr) {
    const uint32_t g = gmem(16), out = gmem(4);
    memcpy(gm_ptr(g), guid ? guid : d7::IID_IDirect3DHALDevice, 16);
    const uint32_t hr = call_method(d3d, D3D_CreateDevice, {g, rt, out});
    CHECK_EQ(hr, 0);
    const uint32_t dev = hr == 0 ? rd32(out) : 0;
    heap_free(g);
    heap_free(out);
    return dev;
}
inline void set_transform(uint32_t dev, uint32_t type, const Matrix &m) {
    const uint32_t a = gmem(64);
    memcpy(gm_ptr(a), m.data(), 64);
    CHECK_EQ(call_method(dev, DEV_SetTransform, {type, a}), 0);
    heap_free(a);
}
inline void rs(uint32_t dev, uint32_t state, uint32_t value) {
    CHECK_EQ(call_method(dev, DEV_SetRenderState, {state, value}), 0);
}
inline void tss(uint32_t dev, uint32_t stage, uint32_t type, uint32_t value) {
    CHECK_EQ(call_method(dev, DEV_SetTextureStageState, {stage, type, value}), 0);
}
inline void viewport(uint32_t dev, int x, int y, int w, int h, float minz = 0, float maxz = 1) {
    const uint32_t v = gmem(24);
    wr32(v, uint32_t(x));
    wr32(v + 4, uint32_t(y));
    wr32(v + 8, uint32_t(w));
    wr32(v + 12, uint32_t(h));
    wr32(v + 16, fbits(minz));
    wr32(v + 20, fbits(maxz));
    CHECK_EQ(call_method(dev, DEV_SetViewport, {v}), 0);
    heap_free(v);
}
inline void material(uint32_t dev, const float diffuse[4], const float ambient[4],
                     const float specular[4] = nullptr, const float emissive[4] = nullptr,
                     float power = 0) {
    const uint32_t m = gmem(68);
    static const float zero[4] = {0, 0, 0, 0};
    memcpy(gm_ptr(m), diffuse, 16);
    memcpy(gm_ptr(m + 16), ambient, 16);
    memcpy(gm_ptr(m + 32), specular ? specular : zero, 16);
    memcpy(gm_ptr(m + 48), emissive ? emissive : zero, 16);
    wr32(m + 64, fbits(power));
    CHECK_EQ(call_method(dev, DEV_SetMaterial, {m}), 0);
    heap_free(m);
}
struct Light {
    uint32_t type = d7::LIGHT_DIRECTIONAL;
    float diffuse[4] = {1, 1, 1, 1}, specular[4] = {0, 0, 0, 0}, ambient[4] = {0, 0, 0, 0};
    float position[3] = {0, 0, 0}, direction[3] = {0, 0, 1};
    float range = 1000, falloff = 1, att0 = 1, att1 = 0, att2 = 0, theta = 0, phi = 0;
};
inline void set_light(uint32_t dev, uint32_t index, const Light &l) {
    const uint32_t a = gmem(d7::D3DLIGHT7_SIZE);
    wr32(a, l.type);
    memcpy(gm_ptr(a + d7::L7_dcvDiffuse), l.diffuse, 16);
    memcpy(gm_ptr(a + d7::L7_dcvSpecular), l.specular, 16);
    memcpy(gm_ptr(a + d7::L7_dcvAmbient), l.ambient, 16);
    memcpy(gm_ptr(a + d7::L7_dvPosition), l.position, 12);
    memcpy(gm_ptr(a + d7::L7_dvDirection), l.direction, 12);
    const float rest[7] = {l.range, l.falloff, l.att0, l.att1, l.att2, l.theta, l.phi};
    memcpy(gm_ptr(a + d7::L7_dvRange), rest, 28);
    CHECK_EQ(call_method(dev, DEV_SetLight, {index, a}), 0);
    CHECK_EQ(call_method(dev, DEV_LightEnable, {index, 1}), 0);
    heap_free(a);
}
inline uint32_t clear(uint32_t dev, uint32_t flags, uint32_t color, float z = 1.0f) {
    return call_method(dev, DEV_Clear, {0, 0, flags, color, fbits(z), 0});
}
// A vertex buffer holding `v`, created through IDirect3D7 and filled by Lock.
inline uint32_t vertex_buffer(uint32_t d3d, const std::vector<Vertex> &v, uint32_t count = 0) {
    const uint32_t desc = gmem(16), out = gmem(4);
    wr32(desc, 16);
    wr32(desc + 4, d7::VBCAPS_SYSTEMMEMORY | d7::VBCAPS_WRITEONLY);
    wr32(desc + 8, FVF_152);
    wr32(desc + 12, count ? count : uint32_t(v.size()));
    CHECK_EQ(call_method(d3d, D3D_CreateVertexBuffer, {desc, out, 0}), 0);
    const uint32_t vb = rd32(out);
    wr32(out, 0);
    CHECK_EQ(call_method(vb, VB_Lock, {0x2020, out, 0}), 0);
    memcpy(gm_ptr(rd32(out)), v.data(), v.size() * sizeof(Vertex));
    CHECK_EQ(call_method(vb, VB_Unlock, {}), 0);
    heap_free(desc);
    heap_free(out);
    return vb;
}
inline uint32_t draw(uint32_t dev, uint32_t prim, const std::vector<Vertex> &v,
                     const std::vector<uint16_t> &indices = {}) {
    const uint32_t vb = gmem(uint32_t(v.size() * sizeof(Vertex)));
    memcpy(gm_ptr(vb), v.data(), v.size() * sizeof(Vertex));
    uint32_t hr;
    if (indices.empty()) {
        hr = call_method(dev, DEV_DrawPrimitive, {prim, FVF_152, vb, uint32_t(v.size()), 0});
    } else {
        const uint32_t ib = gmem(uint32_t(indices.size() * 2));
        memcpy(gm_ptr(ib), indices.data(), indices.size() * 2);
        hr = call_method(dev, DEV_DrawIndexedPrimitive,
                         {prim, FVF_152, vb, uint32_t(v.size()), ib, uint32_t(indices.size()), 0});
        heap_free(ib);
    }
    heap_free(vb);
    return hr;
}
// A screen-space quad in the 2D projection: two triangles, indices 0,1,2,2,1,3.
inline std::vector<Vertex> quad2d(float x0, float y0, float x1, float y1, uint32_t color,
                                  float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1,
                                  float z = 0.5f) {
    return {{x0, y0, z, 0, 0, -1, color, u0, v0},
            {x1, y0, z, 0, 0, -1, color, u1, v0},
            {x0, y1, z, 0, 0, -1, color, u0, v1},
            {x1, y1, z, 0, 0, -1, color, u1, v1}};
}
const std::vector<uint16_t> QUAD_INDICES = {0, 1, 2, 2, 1, 3};

// A device on a w x h render target (with a depth buffer if asked), set up the
// way a 2D/3D engine does: viewport, 2D projection, lighting off.
inline Device make_device(int w, int h, const Pf &pf = PF_X8R8G8B8, bool depth = true,
                          const uint8_t *guid = nullptr) {
    Device d;
    d.dd = create_dd();
    CHECK_EQ(call_method(d.dd, DD_SetCooperativeLevel, {0, d7::DDSCL_NORMAL}), 0);
    d.d3d = query(d.dd, d7::IID_IDirect3D7);
    d.rt = offscreen(d.dd, uint32_t(w), uint32_t(h), pf, true);
    if (depth) {
        d.z = zbuffer(d.dd, uint32_t(w), uint32_t(h));
        CHECK_EQ(call_method(d.rt, S_AddAttachedSurface, {d.z}), 0);
    }
    d.dev = create_device(d.d3d, d.rt, guid);
    viewport(d.dev, 0, 0, w, h);
    set_transform(d.dev, d7::TS_PROJECTION, ortho2d(float(w), float(h)));
    rs(d.dev, d7::RS_LIGHTING, 0);
    rs(d.dev, d7::RS_CULLMODE, d7::CULL_NONE);
    return d;
}
inline void release_all(Device &d, std::initializer_list<uint32_t> extra = {}) {
    for (uint32_t e : extra)
        if (e)
            call_method(e, Release, {});
    if (d.dev)
        call_method(d.dev, Release, {});
    if (d.z)
        call_method(d.z, Release, {});
    if (d.rt)
        call_method(d.rt, Release, {});
    if (d.d3d)
        call_method(d.d3d, Release, {});
    if (d.dd)
        call_method(d.dd, Release, {});
    d = Device{};
}

} // namespace d7t
