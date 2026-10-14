// d3d7.cpp - IDirect3D7, IDirect3DDevice7 and IDirect3DVertexBuffer7.
//
// The device records state; it does not rasterize. Every draw becomes one
// HostD7Draw carrying the vertices, the indices and a snapshot of the device
// state (d3d7_host.h), handed to the backend: the GPU renderer, or the
// software reference renderer. Fixed-function transform, lighting and fog run
// in the backend, so untransformed vertices go out exactly as the guest wrote
// them.
//
// IDirect3D7 is a view of the DirectDraw 7 object, as in DirectX: it is what
// IDirectDraw7::QueryInterface(IID_IDirect3D7) returns.
#include "d3d7_internal.h"
#include "d3d7_prim.h"
#include "../runtime/memory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace d7 {
namespace {

// ===========================================================================
// Objects
// ===========================================================================
struct LightSlot {
    HostD7Light light;
    bool enabled = false;
};

struct Device {
    uint32_t id = 0, dd = 0;
    uint32_t target = 0; // render-target surface id
    bool tnl = false;    // created as the T&L HAL device
    bool in_scene = false;
    HostD7State st;
    std::array<uint32_t, 8> textures{}; // surface ids, each holding a reference
    std::map<uint32_t, LightSlot> lights;
    float clip_planes[32][4] = {};
};
struct VertexBuffer {
    uint32_t id = 0;
    uint32_t mem = 0, caps = 0, fvf = 0, stride = 0, count = 0;
    int locks = 0;
};

std::unordered_map<uint32_t, std::unique_ptr<Device>> &devices() {
    static auto *m = new std::unordered_map<uint32_t, std::unique_ptr<Device>>();
    return *m;
}
std::unordered_map<uint32_t, std::unique_ptr<VertexBuffer>> &vbs() {
    static auto *m = new std::unordered_map<uint32_t, std::unique_ptr<VertexBuffer>>();
    return *m;
}
Device *this_device(X86 *c) {
    ComObj *o = com_this_arg(c, IF_D3DDEVICE7);
    if (!o)
        return nullptr;
    auto it = devices().find(o->id);
    return it == devices().end() ? nullptr : it->second.get();
}
VertexBuffer *vb_from(uint32_t p) {
    ComObj *o = com_this(p, IF_D3DVB7);
    if (!o)
        return nullptr;
    auto it = vbs().find(o->id);
    return it == vbs().end() ? nullptr : it->second.get();
}
ComObj *this_dd(X86 *c) {
    ComObj *o = com_this_arg(c);
    return o && o->kind == K_DDRAW7 ? o : nullptr;
}
inline uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
inline float rdf(uint32_t a) {
    const uint32_t u = rd32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
inline void wrf(uint32_t a, float f) {
    wr32(a, fbits(f));
}
void identity(float *m) {
    for (int i = 0; i < 16; ++i)
        m[i] = (i % 5) == 0 ? 1.0f : 0.0f;
}

// ===========================================================================
// Device state defaults (the DirectX 7 SDK's)
// ===========================================================================
void default_state(Device &d, Surface *rt) {
    HostD7State &s = d.st;
    memset(&s, 0, sizeof s);
    identity(s.world);
    identity(s.view);
    identity(s.projection);
    uint32_t *rs = s.rs;
    rs[RS_ZENABLE] = rt && rt->attached_z ? ZB_TRUE : ZB_FALSE;
    rs[RS_FILLMODE] = FILL_SOLID;
    rs[RS_SHADEMODE] = SHADE_GOURAUD;
    rs[RS_ZWRITEENABLE] = 1;
    rs[RS_LASTPIXEL] = 1;
    rs[RS_SRCBLEND] = BLEND_ONE;
    rs[RS_DESTBLEND] = BLEND_ZERO;
    rs[RS_CULLMODE] = CULL_CCW;
    rs[RS_ZFUNC] = CMP_LESSEQUAL;
    rs[RS_ALPHAFUNC] = CMP_ALWAYS;
    rs[RS_TEXTUREPERSPECTIVE] = 1;
    rs[RS_FOGSTART] = fbits(0.0f);
    rs[RS_FOGEND] = fbits(1.0f);
    rs[RS_FOGDENSITY] = fbits(1.0f);
    rs[RS_TEXTUREFACTOR] = 0xffffffffu;
    rs[RS_CLIPPING] = 1;
    rs[RS_LIGHTING] = 1;
    rs[RS_COLORVERTEX] = 1;
    rs[RS_LOCALVIEWER] = 1;
    rs[RS_DIFFUSEMATERIALSOURCE] = MCS_COLOR1;
    rs[RS_SPECULARMATERIALSOURCE] = MCS_COLOR2;
    rs[RS_AMBIENTMATERIALSOURCE] = MCS_MATERIAL;
    rs[RS_EMISSIVEMATERIALSOURCE] = MCS_MATERIAL;
    for (int stage = 0; stage < 2; ++stage) {
        uint32_t *t = s.tss[stage];
        t[TSS_COLOROP] = stage == 0 ? TOP_MODULATE : TOP_DISABLE;
        t[TSS_COLORARG1] = TA_TEXTURE;
        t[TSS_COLORARG2] = TA_CURRENT;
        t[TSS_ALPHAOP] = stage == 0 ? TOP_SELECTARG1 : TOP_DISABLE;
        t[TSS_ALPHAARG1] = TA_TEXTURE;
        t[TSS_ALPHAARG2] = TA_CURRENT;
        t[TSS_TEXCOORDINDEX] = uint32_t(stage);
        t[TSS_ADDRESS] = t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = TADDRESS_WRAP;
        t[TSS_MAGFILTER] = TFG_POINT;
        t[TSS_MINFILTER] = TFN_POINT;
        t[TSS_MIPFILTER] = TFP_NONE;
        t[TSS_MAXANISOTROPY] = 1;
    }
    if (rt) {
        s.viewport[2] = int32_t(rt->width);
        s.viewport[3] = int32_t(rt->height);
    }
    s.viewport_maxz = 1.0f;
}

// ===========================================================================
// Capabilities
// ===========================================================================
void write_primcaps(uint32_t a, bool lines) {
    gm_zero(a, D3DPRIMCAPS_SIZE);
    wr32(a + PC_dwSize, D3DPRIMCAPS_SIZE);
    wr32(a + PC_dwMiscCaps,
         D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW);
    wr32(a + PC_dwRasterCaps, D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST |
                                  D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
                                  D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS |
                                  D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ANISOTROPY |
                                  D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG);
    wr32(a + PC_dwZCmpCaps, 0xff);
    wr32(a + PC_dwSrcBlendCaps, 0x1fff);
    wr32(a + PC_dwDestBlendCaps, 0x1fff);
    wr32(a + PC_dwAlphaCmpCaps, 0xff);
    wr32(a + PC_dwShadeCaps, D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB |
                                 D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND |
                                 D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGFLAT |
                                 D3DPSHADECAPS_FOGGOURAUD);
    wr32(a + PC_dwTextureCaps, D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                                   D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_BORDER);
    wr32(a + PC_dwTextureFilterCaps, D3DPTFILTERCAPS_ALL);
    wr32(a + PC_dwTextureBlendCaps, 0xff);
    wr32(a + PC_dwTextureAddressCaps, D3DPTADDRESSCAPS_ALL);
    (void)lines;
}
// D3DDEVICEDESC7 for the HAL or the T&L HAL device.
void write_device_desc(uint32_t a, bool tnl) {
    gm_zero(a, D3DDD7_SIZE);
    uint32_t caps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                    D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                    D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                    D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                    D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX |
                    D3DDEVCAPS_HWRASTERIZATION;
    if (tnl)
        caps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    wr32(a + D3DDD7_dwDevCaps, caps);
    write_primcaps(a + D3DDD7_dpcLineCaps, true);
    write_primcaps(a + D3DDD7_dpcTriCaps, false);
    wr32(a + D3DDD7_dwDeviceRenderBitDepth, DDBD_16 | DDBD_32);
    wr32(a + D3DDD7_dwDeviceZBufferBitDepth, DDBD_16 | DDBD_24 | DDBD_32);
    wr32(a + D3DDD7_dwMinTextureWidth, 1);
    wr32(a + D3DDD7_dwMinTextureHeight, 1);
    wr32(a + D3DDD7_dwMaxTextureWidth, 4096);
    wr32(a + D3DDD7_dwMaxTextureHeight, 4096);
    wr32(a + D3DDD7_dwMaxTextureRepeat, 8192);
    wr32(a + D3DDD7_dwMaxTextureAspectRatio, 4096);
    wr32(a + D3DDD7_dwMaxAnisotropy, 16);
    wrf(a + D3DDD7_dvGuardBandLeft, -16384.0f);
    wrf(a + D3DDD7_dvGuardBandTop, -16384.0f);
    wrf(a + D3DDD7_dvGuardBandRight, 16384.0f);
    wrf(a + D3DDD7_dvGuardBandBottom, 16384.0f);
    wr32(a + D3DDD7_dwFVFCaps, 8 & D3DFVFCAPS_TEXCOORDCOUNTMASK);
    // The texture operations the stage-0 combiner implements.
    uint32_t ops = 0;
    for (uint32_t op = TOP_DISABLE; op <= TOP_BLENDCURRENTALPHA; ++op)
        ops |= 1u << (op - 1);
    wr32(a + D3DDD7_dwTextureOpCaps, ops);
    wr16(a + D3DDD7_wMaxTextureBlendStages, 1);
    wr16(a + D3DDD7_wMaxSimultaneousTextures, 1);
    wr32(a + D3DDD7_dwMaxActiveLights, HOST_D7_MAX_LIGHTS);
    wrf(a + D3DDD7_dvMaxVertexW, 1e10f);
    memcpy(gm_ptr(a + D3DDD7_deviceGUID), tnl ? IID_IDirect3DTnLHalDevice : IID_IDirect3DHALDevice,
           16);
    wr32(a + D3DDD7_dwVertexProcessingCaps,
         D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_VERTEXFOG | D3DVTXPCAPS_DIRECTIONALLIGHTS |
             D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER);
}

// ===========================================================================
// IDirect3D7
// ===========================================================================
// EnumDevices(lpEnumDevicesCallback7, lpUserArg): the HAL device, then the
// T&L HAL device.
void D3D7_EnumDevices(X86 *c) {
    const uint32_t cb = arg(c, 1), ctx = arg(c, 2);
    if (!cb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t block = heap_alloc(D3DDD7_SIZE + 512, true, 16);
    if (!block) {
        com_ret(c, E_OUTOFMEMORY_);
        return;
    }
    const uint32_t desc = block, text = block + D3DDD7_SIZE + 4, name = text + 256;
    struct Entry {
        bool tnl;
        const char *description, *name;
    } const entries[] = {
        {false, "Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL"},
        {true, "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device",
         "Direct3D T&L HAL"},
    };
    for (const Entry &e : entries) {
        write_device_desc(desc, e.tnl);
        gm_put_str(text, e.description, 256);
        gm_put_str(name, e.name, 256);
        if (guest_call(c, cb, text, name, desc, ctx) == D3DENUMRET_CANCEL)
            break;
    }
    heap_free(block);
    com_ret(c, DD_OK);
}

void release_textures(Device &d) {
    for (uint32_t &t : d.textures) {
        if (t)
            com_release(com_get(t));
        t = 0;
    }
}
void device_destroy(ComObj *o) {
    auto it = devices().find(o->id);
    if (it == devices().end())
        return;
    Device &d = *it->second;
    release_textures(d);
    if (d.target)
        com_release(com_get(d.target));
    devices().erase(it);
}

// CreateDevice(rclsid, lpDDS, lplpD3DDevice)
void D3D7_CreateDevice(X86 *c) {
    ComObj *dd = this_dd(c);
    const uint32_t guid = arg(c, 1), out = arg(c, 3);
    Surface *rt = surface_from_guest(arg(c, 2));
    if (!dd || !out || !gm_valid(out, 4) || !guid || !gm_valid(guid, 16)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, 0);
    const uint8_t *g = gm_ptr(guid);
    const bool tnl = !memcmp(g, IID_IDirect3DTnLHalDevice, 16);
    const bool known = tnl || !memcmp(g, IID_IDirect3DHALDevice, 16) ||
                       !memcmp(g, IID_IDirect3DRGBDevice, 16) ||
                       !memcmp(g, IID_IDirect3DRefDevice, 16);
    if (!known) {
        com_ret(c, D3DERR_INVALID_DEVICE);
        return;
    }
    if (!rt || rt->format.depth()) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (!(rt->caps & DDSCAPS_3DDEVICE))
        log_once("d3d7.rtcaps", "d3d7: CreateDevice on a surface without DDSCAPS_3DDEVICE");
    ComObj *o = com_new(K_D3DDEVICE7);
    auto d = std::make_unique<Device>();
    d->id = o->id;
    d->dd = dd->id;
    d->tnl = tnl;
    d->target = rt->id;
    com_addref(com_get(rt->id));
    default_state(*d, rt);
    devices()[o->id] = std::move(d);
    const uint32_t view = com_view(o, IF_D3DDEVICE7);
    if (!view) {
        com_destroy(o);
        com_ret(c, E_OUTOFMEMORY_);
        return;
    }
    wr32(out, view);
    LOGV("d3d7: CreateDevice(%s) on %ux%u -> %08x", tnl ? "T&L HAL" : "HAL", rt->width, rt->height,
         view);
    com_ret(c, DD_OK);
}

void vb_destroy(ComObj *o) {
    auto it = vbs().find(o->id);
    if (it == vbs().end())
        return;
    if (it->second->mem)
        heap_free(it->second->mem);
    vbs().erase(it);
}
// CreateVertexBuffer(lpVBDesc, lplpD3DVertexBuffer, dwFlags)
void D3D7_CreateVertexBuffer(X86 *c) {
    const uint32_t desc = arg(c, 1), out = arg(c, 2);
    if (!desc || !gm_valid(desc, D3DVBDESC_SIZE) || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, 0);
    const uint32_t caps = rd32(desc + 4), fvf = rd32(desc + 8), count = rd32(desc + 12);
    const FvfLayout L = fvf_layout(fvf);
    if (!L.valid) {
        com_ret(c, D3DERR_INVALIDVERTEXFORMAT);
        return;
    }
    const uint64_t bytes = uint64_t(L.stride) * count;
    if (!count || bytes > 0x4000000u) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t mem = heap_alloc(uint32_t(bytes), true, 16);
    if (!mem) {
        com_ret(c, D3DERR_VBUF_CREATE_FAILED);
        return;
    }
    ComObj *o = com_new(K_D3DVB7);
    auto vb = std::make_unique<VertexBuffer>();
    vb->id = o->id;
    vb->mem = mem;
    vb->caps = caps;
    vb->fvf = fvf;
    vb->stride = L.stride;
    vb->count = count;
    vbs()[o->id] = std::move(vb);
    wr32(out, com_view(o, IF_D3DVB7));
    com_ret(c, DD_OK);
}
// EnumZBufferFormats(riidDevice, lpEnumCallback, lpContext)
void D3D7_EnumZBufferFormats(X86 *c) {
    const uint32_t cb = arg(c, 2), ctx = arg(c, 3);
    if (!cb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t pf = heap_alloc(DDPF_SIZE_, true, 16);
    for (uint32_t bits : {16u, 24u, 32u}) {
        Format f = format_depth(bits);
        write_format(pf, f);
        if (guest_call(c, cb, pf, ctx) == D3DENUMRET_CANCEL)
            break;
    }
    heap_free(pf);
    com_ret(c, DD_OK);
}
void D3D7_ok(X86 *c) {
    com_ret(c, DD_OK);
}

const ComMethod g_d3d7[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"EnumDevices", 3, D3D7_EnumDevices},
    {"CreateDevice", 4, D3D7_CreateDevice},
    {"CreateVertexBuffer", 4, D3D7_CreateVertexBuffer},
    {"EnumZBufferFormats", 4, D3D7_EnumZBufferFormats},
    {"EvictManagedTextures", 1, D3D7_ok},
};

// ===========================================================================
// IDirect3DDevice7: state
// ===========================================================================
void Dev_GetCaps(X86 *c) {
    Device *d = this_device(c);
    const uint32_t a = arg(c, 1);
    if (!d || !a || !gm_valid(a, D3DDD7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    write_device_desc(a, d->tnl);
    com_ret(c, DD_OK);
}
// EnumTextureFormats(lpd3dEnumPixelProc, lpArg)
void Dev_EnumTextureFormats(X86 *c) {
    const uint32_t cb = arg(c, 1), ctx = arg(c, 2);
    if (!this_device(c) || !cb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    struct F {
        uint32_t bpp, r, g, b, a;
    } const formats[] = {
        {16, 0xf800, 0x07e0, 0x001f, 0},      // R5G6B5
        {16, 0x7c00, 0x03e0, 0x001f, 0},      // X1R5G5B5
        {16, 0x7c00, 0x03e0, 0x001f, 0x8000}, // A1R5G5B5
        {16, 0x0f00, 0x00f0, 0x000f, 0xf000}, // A4R4G4B4
        {32, 0xff0000, 0xff00, 0xff, 0},      // X8R8G8B8
        {32, 0xff0000, 0xff00, 0xff, 0xff000000u},
    };
    const uint32_t pf = heap_alloc(DDPF_SIZE_, true, 16);
    for (const F &e : formats) {
        Format f;
        f.flags = DDPF_RGB | (e.a ? DDPF_ALPHAPIXELS : 0);
        f.bpp = e.bpp;
        f.r = e.r;
        f.g = e.g;
        f.b = e.b;
        f.a = e.a;
        write_format(pf, f);
        if (guest_call(c, cb, pf, ctx) == D3DENUMRET_CANCEL)
            break;
    }
    heap_free(pf);
    com_ret(c, DD_OK);
}
void Dev_BeginScene(X86 *c) {
    Device *d = this_device(c);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (d->in_scene) {
        com_ret(c, D3DERR_SCENE_IN_SCENE);
        return;
    }
    d->in_scene = true;
    com_ret(c, DD_OK);
}
void Dev_EndScene(X86 *c) {
    Device *d = this_device(c);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (!d->in_scene) {
        com_ret(c, D3DERR_SCENE_NOT_IN_SCENE);
        return;
    }
    d->in_scene = false;
    // Hand the scene to the GPU now rather than at the next present.
    if (const Backend *b = backend())
        b->flush();
    com_ret(c, DD_OK);
}
void Dev_GetDirect3D(X86 *c) {
    Device *d = this_device(c);
    const uint32_t out = arg(c, 1);
    ComObj *dd = d ? com_get(d->dd) : nullptr;
    if (!dd || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_addref(dd);
    wr32(out, com_view(dd, IF_D3D7));
    com_ret(c, DD_OK);
}
// SetRenderTarget(lpNewRenderTarget, dwFlags): the depth buffer is the one
// attached to the new target, and the viewport becomes the whole target.
void Dev_SetRenderTarget(X86 *c) {
    Device *d = this_device(c);
    Surface *rt = surface_from_guest(arg(c, 1));
    if (!d || !rt || rt->format.depth()) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (rt->id != d->target) {
        com_addref(com_get(rt->id));
        if (d->target)
            com_release(com_get(d->target));
        d->target = rt->id;
    }
    d->st.viewport[0] = d->st.viewport[1] = 0;
    d->st.viewport[2] = int32_t(rt->width);
    d->st.viewport[3] = int32_t(rt->height);
    com_ret(c, DD_OK);
}
void Dev_GetRenderTarget(X86 *c) {
    Device *d = this_device(c);
    const uint32_t out = arg(c, 1);
    Surface *rt = d ? surface(d->target) : nullptr;
    if (!rt || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_addref(com_get(rt->id));
    wr32(out, surface_view(rt));
    com_ret(c, DD_OK);
}

float *transform_slot(Device &d, uint32_t type) {
    switch (type) {
    case TS_WORLD:
        return d.st.world;
    case TS_VIEW:
        return d.st.view;
    case TS_PROJECTION:
        return d.st.projection;
    default:
        return nullptr;
    }
}
// SetTransform(dtstTransformStateType, lpD3DMatrix)
void Dev_SetTransform(X86 *c) {
    Device *d = this_device(c);
    const uint32_t type = arg(c, 1), m = arg(c, 2);
    if (!d || !m || !gm_valid(m, D3DMATRIX_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    float *slot = transform_slot(*d, type);
    if (!slot) {
        log_once("d3d7.transform", "d3d7: transform state %u is accepted and not used", type);
        com_ret(c, DD_OK);
        return;
    }
    memcpy(slot, gm_ptr(m), D3DMATRIX_SIZE);
    com_ret(c, DD_OK);
}
void Dev_GetTransform(X86 *c) {
    Device *d = this_device(c);
    const uint32_t type = arg(c, 1), m = arg(c, 2);
    if (!d || !m || !gm_valid(m, D3DMATRIX_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    float *slot = transform_slot(*d, type);
    float id[16];
    identity(id);
    memcpy(gm_ptr(m), slot ? slot : id, D3DMATRIX_SIZE);
    com_ret(c, DD_OK);
}
// MultiplyTransform: M = lpMatrix * M.
void Dev_MultiplyTransform(X86 *c) {
    Device *d = this_device(c);
    const uint32_t type = arg(c, 1), m = arg(c, 2);
    float *slot = d ? transform_slot(*d, type) : nullptr;
    if (!slot || !m || !gm_valid(m, D3DMATRIX_SIZE)) {
        com_ret(c, d && m ? DD_OK : DDERR_INVALIDPARAMS);
        return;
    }
    float a[16], out[16];
    memcpy(a, gm_ptr(m), 64);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) {
            float s = 0;
            for (int i = 0; i < 4; ++i)
                s += a[r * 4 + i] * slot[i * 4 + k];
            out[r * 4 + k] = s;
        }
    memcpy(slot, out, 64);
    com_ret(c, DD_OK);
}
// SetViewport(lpViewport): D3DVIEWPORT7 {dwX, dwY, dwWidth, dwHeight, dvMinZ, dvMaxZ}.
void Dev_SetViewport(X86 *c) {
    Device *d = this_device(c);
    const uint32_t v = arg(c, 1);
    if (!d || !v || !gm_valid(v, D3DVIEWPORT7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t w = rd32(v + 8), h = rd32(v + 12);
    if (!w || !h || w > 16384 || h > 16384) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    d->st.viewport[0] = int32_t(rd32(v));
    d->st.viewport[1] = int32_t(rd32(v + 4));
    d->st.viewport[2] = int32_t(w);
    d->st.viewport[3] = int32_t(h);
    d->st.viewport_minz = rdf(v + 16);
    d->st.viewport_maxz = rdf(v + 20);
    com_ret(c, DD_OK);
}
void Dev_GetViewport(X86 *c) {
    Device *d = this_device(c);
    const uint32_t v = arg(c, 1);
    if (!d || !v || !gm_valid(v, D3DVIEWPORT7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    for (int i = 0; i < 4; ++i)
        wr32(v + 4 * uint32_t(i), uint32_t(d->st.viewport[i]));
    wrf(v + 16, d->st.viewport_minz);
    wrf(v + 20, d->st.viewport_maxz);
    com_ret(c, DD_OK);
}
void Dev_SetMaterial(X86 *c) {
    Device *d = this_device(c);
    const uint32_t m = arg(c, 1);
    if (!d || !m || !gm_valid(m, D3DMATERIAL7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    memcpy(d->st.material, gm_ptr(m), D3DMATERIAL7_SIZE);
    com_ret(c, DD_OK);
}
void Dev_GetMaterial(X86 *c) {
    Device *d = this_device(c);
    const uint32_t m = arg(c, 1);
    if (!d || !m || !gm_valid(m, D3DMATERIAL7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    memcpy(gm_ptr(m), d->st.material, D3DMATERIAL7_SIZE);
    com_ret(c, DD_OK);
}
HostD7Light read_light(uint32_t a) {
    HostD7Light l;
    memset(&l, 0, sizeof l);
    l.type = rd32(a + L7_dltType);
    for (int i = 0; i < 4; ++i) {
        l.diffuse[i] = rdf(a + L7_dcvDiffuse + 4 * uint32_t(i));
        l.specular[i] = rdf(a + L7_dcvSpecular + 4 * uint32_t(i));
        l.ambient[i] = rdf(a + L7_dcvAmbient + 4 * uint32_t(i));
    }
    for (int i = 0; i < 3; ++i) {
        l.position[i] = rdf(a + L7_dvPosition + 4 * uint32_t(i));
        l.direction[i] = rdf(a + L7_dvDirection + 4 * uint32_t(i));
    }
    l.range = rdf(a + L7_dvRange);
    l.falloff = rdf(a + L7_dvFalloff);
    l.attenuation[0] = rdf(a + L7_dvAttenuation0);
    l.attenuation[1] = rdf(a + L7_dvAttenuation1);
    l.attenuation[2] = rdf(a + L7_dvAttenuation2);
    l.theta = rdf(a + L7_dvTheta);
    l.phi = rdf(a + L7_dvPhi);
    return l;
}
void write_light(uint32_t a, const HostD7Light &l) {
    wr32(a + L7_dltType, l.type);
    for (int i = 0; i < 4; ++i) {
        wrf(a + L7_dcvDiffuse + 4 * uint32_t(i), l.diffuse[i]);
        wrf(a + L7_dcvSpecular + 4 * uint32_t(i), l.specular[i]);
        wrf(a + L7_dcvAmbient + 4 * uint32_t(i), l.ambient[i]);
    }
    for (int i = 0; i < 3; ++i) {
        wrf(a + L7_dvPosition + 4 * uint32_t(i), l.position[i]);
        wrf(a + L7_dvDirection + 4 * uint32_t(i), l.direction[i]);
    }
    wrf(a + L7_dvRange, l.range);
    wrf(a + L7_dvFalloff, l.falloff);
    wrf(a + L7_dvAttenuation0, l.attenuation[0]);
    wrf(a + L7_dvAttenuation1, l.attenuation[1]);
    wrf(a + L7_dvAttenuation2, l.attenuation[2]);
    wrf(a + L7_dvTheta, l.theta);
    wrf(a + L7_dvPhi, l.phi);
}
// SetLight(dwLightIndex, lpLight)
void Dev_SetLight(X86 *c) {
    Device *d = this_device(c);
    const uint32_t index = arg(c, 1), a = arg(c, 2);
    if (!d || !a || !gm_valid(a, D3DLIGHT7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const HostD7Light l = read_light(a);
    if (l.type < LIGHT_POINT || l.type > LIGHT_DIRECTIONAL) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    d->lights[index].light = l;
    com_ret(c, DD_OK);
}
void Dev_GetLight(X86 *c) {
    Device *d = this_device(c);
    const uint32_t index = arg(c, 1), a = arg(c, 2);
    if (!d || !a || !gm_valid(a, D3DLIGHT7_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    auto it = d->lights.find(index);
    if (it == d->lights.end()) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    write_light(a, it->second.light);
    com_ret(c, DD_OK);
}
// LightEnable(dwLightIndex, bEnable). Enabling a light that was never set
// creates the default one: a white directional light pointing down +z.
void Dev_LightEnable(X86 *c) {
    Device *d = this_device(c);
    const uint32_t index = arg(c, 1), enable = arg(c, 2);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    auto it = d->lights.find(index);
    if (it == d->lights.end()) {
        LightSlot s;
        memset(&s.light, 0, sizeof s.light);
        s.light.type = LIGHT_DIRECTIONAL;
        s.light.diffuse[0] = s.light.diffuse[1] = s.light.diffuse[2] = 1.0f;
        s.light.direction[2] = 1.0f;
        it = d->lights.emplace(index, s).first;
    }
    it->second.enabled = enable != 0;
    com_ret(c, DD_OK);
}
void Dev_GetLightEnable(X86 *c) {
    Device *d = this_device(c);
    const uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!d || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    auto it = d->lights.find(index);
    if (it == d->lights.end()) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, it->second.enabled ? 1 : 0);
    com_ret(c, DD_OK);
}
// SetRenderState(dwRenderStateType, dwRenderState)
void Dev_SetRenderState(X86 *c) {
    Device *d = this_device(c);
    const uint32_t state = arg(c, 1), value = arg(c, 2);
    if (!d || state >= RS_COUNT || state == 0) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    d->st.rs[state] = value;
    if (state == RS_FILLMODE && value != FILL_SOLID)
        log_once("d3d7.fillmode", "d3d7: fill mode %u draws solid", value);
    if (state == RS_STENCILENABLE && value)
        log_once("d3d7.stencil", "d3d7: the stencil buffer is not implemented");
    if (state == RS_VERTEXBLEND && value)
        log_once("d3d7.vertexblend", "d3d7: vertex blending is not implemented");
    if (state == RS_CLIPPLANEENABLE && value)
        log_once("d3d7.clipplanes", "d3d7: user clip planes are not implemented");
    if (state == RS_ZENABLE && value == ZB_USEW)
        log_once("d3d7.wbuffer", "d3d7: w-buffering uses the z buffer");
    com_ret(c, DD_OK);
}
void Dev_GetRenderState(X86 *c) {
    Device *d = this_device(c);
    const uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!d || state >= RS_COUNT || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, d->st.rs[state]);
    com_ret(c, DD_OK);
}
// SetTexture(dwStage, lpTexture): the device holds a reference.
void Dev_SetTexture(X86 *c) {
    Device *d = this_device(c);
    const uint32_t stage = arg(c, 1), tex = arg(c, 2);
    if (!d || stage >= 8) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    Surface *s = tex ? surface_from_guest(tex) : nullptr;
    if (tex && !s) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t id = s ? s->id : 0;
    if (d->textures[stage] != id) {
        if (id)
            com_addref(com_get(id));
        if (d->textures[stage])
            com_release(com_get(d->textures[stage]));
        d->textures[stage] = id;
    }
    if (stage > 0 && id)
        log_once("d3d7.stage1", "d3d7: textures on stages above 0 are not sampled");
    com_ret(c, DD_OK);
}
void Dev_GetTexture(X86 *c) {
    Device *d = this_device(c);
    const uint32_t stage = arg(c, 1), out = arg(c, 2);
    if (!d || stage >= 8 || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    Surface *s = surface(d->textures[stage]);
    if (s)
        com_addref(com_get(s->id));
    wr32(out, s ? surface_view(s) : 0);
    com_ret(c, DD_OK);
}
// SetTextureStageState(dwStage, dwState, dwValue)
void Dev_SetTextureStageState(X86 *c) {
    Device *d = this_device(c);
    const uint32_t stage = arg(c, 1), type = arg(c, 2), value = arg(c, 3);
    if (!d || stage >= 8 || type >= TSS_COUNT) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (stage < 2) {
        uint32_t *t = d->st.tss[stage];
        t[type] = value;
        if (type == TSS_ADDRESS)
            t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = value;
        if (stage == 1 && (type == TSS_COLOROP || type == TSS_ALPHAOP) && value != TOP_DISABLE)
            log_once("d3d7.stage1op", "d3d7: texture stage 1 is not blended");
    }
    if (type == TSS_TEXTURETRANSFORMFLAGS && value)
        log_once("d3d7.textransform", "d3d7: texture coordinate transforms are not implemented");
    com_ret(c, DD_OK);
}
void Dev_GetTextureStageState(X86 *c) {
    Device *d = this_device(c);
    const uint32_t stage = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!d || stage >= 8 || type >= TSS_COUNT || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, stage < 2 ? d->st.tss[stage][type] : 0);
    com_ret(c, DD_OK);
}
void Dev_ValidateDevice(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 1);
    com_ret(c, DD_OK);
}
void Dev_SetClipPlane(X86 *c) {
    Device *d = this_device(c);
    const uint32_t i = arg(c, 1), p = arg(c, 2);
    if (!d || i >= 32 || !p || !gm_valid(p, 16)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    memcpy(d->clip_planes[i], gm_ptr(p), 16);
    com_ret(c, DD_OK);
}
void Dev_GetClipPlane(X86 *c) {
    Device *d = this_device(c);
    const uint32_t i = arg(c, 1), p = arg(c, 2);
    if (!d || i >= 32 || !p || !gm_valid(p, 16)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    memcpy(gm_ptr(p), d->clip_planes[i], 16);
    com_ret(c, DD_OK);
}
void Dev_ClipStatus(X86 *c) {
    const uint32_t p = arg(c, 1);
    if (p && gm_valid(p, 28))
        gm_zero(p, 28);
    com_ret(c, DD_OK);
}
void Dev_PreLoad(X86 *c) {
    if (Surface *s = surface_from_guest(arg(c, 1)))
        if (surface_on_backend(s))
            surface_backend_current(s);
    com_ret(c, DD_OK);
}

// ===========================================================================
// IDirect3DDevice7: Clear and draws
// ===========================================================================
// Clear(dwCount, lpRects, dwFlags, dwColor, dvZ, dwStencil). Rectangles are
// clipped to the viewport; none means the whole viewport.
void Dev_Clear(X86 *c) {
    Device *d = this_device(c);
    const uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    const uint32_t zbits = arg(c, 5);
    float z;
    memcpy(&z, &zbits, 4);
    Surface *rt = d ? surface(d->target) : nullptr;
    if (!rt || (count && (!rects || !gm_fits_n(rects, count, 16)))) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const HostD7Rect vp{std::max(0, d->st.viewport[0]), std::max(0, d->st.viewport[1]),
                        std::min(int32_t(rt->width), d->st.viewport[0] + d->st.viewport[2]),
                        std::min(int32_t(rt->height), d->st.viewport[1] + d->st.viewport[3])};
    std::vector<HostD7Rect> list;
    if (!count || !rects) {
        list.push_back(vp);
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t a = rects + 16 * i;
            list.push_back(
                {std::max(vp.x0, int32_t(rd32(a))), std::max(vp.y0, int32_t(rd32(a + 4))),
                 std::min(vp.x1, int32_t(rd32(a + 8))), std::min(vp.y1, int32_t(rd32(a + 12)))});
        }
    }
    Surface *zs = surface(rt->attached_z);
    const Backend *b = backend();
    const bool on_backend = b && surface_on_backend(rt);
    for (const HostD7Rect &r : list) {
        if (r.x0 >= r.x1 || r.y0 >= r.y1)
            continue;
        if (flags & CLEAR_TARGET) {
            if (on_backend && surface_backend_current(rt)) {
                b->fill(rt->root, &r, color);
                surface_backend_wrote(rt);
            } else if (surface_cpu_current(rt)) {
                const uint32_t p = bgra_to_pixel(rt->format, color);
                const uint32_t bytes = rt->format.bytes();
                for (int32_t y = r.y0; y < r.y1; ++y)
                    for (int32_t x = r.x0; x < r.x1; ++x)
                        memcpy(gm_ptr(rt->mem + uint32_t(y) * rt->pitch + uint32_t(x) * bytes), &p,
                               bytes);
                surface_cpu_wrote(rt);
            }
        }
        if ((flags & CLEAR_ZBUFFER) && zs && on_backend && surface_backend_current(zs)) {
            HostD7Rect zr = r;
            zr.x1 = std::min(zr.x1, int32_t(zs->width));
            zr.y1 = std::min(zr.y1, int32_t(zs->height));
            if (zr.x0 < zr.x1 && zr.y0 < zr.y1) {
                b->fill_depth(zs->root, &zr, std::min(1.0f, std::max(0.0f, z)));
                surface_backend_wrote(zs);
            }
        }
    }
    if (flags & CLEAR_STENCIL)
        log_once("d3d7.clearstencil", "d3d7: there is no stencil buffer to clear");
    com_ret(c, DD_OK);
}

// One draw: validates the vertex and index ranges, makes the target, depth
// and texture current on the backend, snapshots the state and submits.
uint32_t submit(Device *d, uint32_t primitive, uint32_t fvf, uint32_t vaddr, uint32_t vcount,
                uint32_t vmax, uint32_t iaddr, uint32_t icount) {
    const FvfLayout L = fvf_layout(fvf);
    if (!L.valid)
        return D3DERR_INVALIDVERTEXFORMAT;
    if (topology_of(primitive) == Topology::None)
        return DDERR_INVALIDPARAMS;
    if (!d->in_scene)
        log_once("d3d7.noscene", "d3d7: a draw outside BeginScene/EndScene is drawn anyway");
    if (iaddr) {
        if (!gm_fits_n(iaddr, icount, 2))
            return DDERR_INVALIDPARAMS;
        // Indices past the stated vertex count still name vertices of the
        // buffer; take them in rather than reading past what was copied.
        uint32_t top = 0;
        for (uint32_t i = 0; i < icount; ++i)
            top = std::max<uint32_t>(top, rd16(iaddr + 2 * i));
        if (top >= vcount) {
            log_once("d3d7.indexrange", "d3d7: an index exceeds the draw's vertex count");
            vcount = std::min(top + 1, vmax);
        }
    }
    if (!vcount || !gm_fits_n(vaddr, vcount, L.stride))
        return vcount ? DDERR_INVALIDPARAMS : DD_OK;
    if (iaddr && !icount)
        return DD_OK;
    const Backend *b = backend();
    Surface *rt = surface(d->target);
    if (!b || !rt || !surface_on_backend(rt)) {
        log_once("d3d7.nobackend", "d3d7: no renderer is available; draws are dropped");
        return DD_OK;
    }
    Surface *zs = surface(rt->attached_z);
    Surface *tex = surface(d->textures[0]);
    if (tex && tex->format.depth())
        tex = nullptr;
    if (tex)
        tex = surface(tex->root) ? surface(tex->root) : tex;
    if (!surface_backend_current(rt) || (zs && !surface_backend_current(zs)) ||
        (tex && !surface_backend_current(tex)))
        return DD_OK;
    HostD7State &st = d->st;
    st.light_count = 0;
    for (auto &kv : d->lights) {
        if (!kv.second.enabled)
            continue;
        if (st.light_count == HOST_D7_MAX_LIGHTS) {
            log_once("d3d7.lights", "d3d7: more than 8 lights are enabled; the rest are ignored");
            break;
        }
        st.lights[st.light_count++] = kv.second.light;
    }
    HostD7Draw draw;
    memset(&draw, 0, sizeof draw);
    draw.target = rt->root;
    draw.depth = zs ? zs->root : 0;
    draw.target_w = int32_t(rt->width);
    draw.target_h = int32_t(rt->height);
    draw.primitive = primitive;
    draw.fvf = fvf;
    draw.stride = L.stride;
    draw.vertices = gm_ptr(vaddr);
    draw.vertex_count = vcount;
    draw.indices = iaddr ? reinterpret_cast<const uint16_t *>(gm_ptr(iaddr)) : nullptr;
    draw.index_count = iaddr ? icount : 0;
    draw.state = &st;
    if (tex) {
        draw.texture.id = tex->id;
        draw.texture.width = int32_t(tex->width);
        draw.texture.height = int32_t(tex->height);
        draw.texture.levels = tex->levels;
        draw.texture.has_alpha = tex->format.has_alpha();
        if (tex->has_src_key)
            draw.texture.key = make_key(tex->format, tex->src_key_lo, tex->src_key_hi);
    }
    b->draw(&draw);
    surface_backend_wrote(rt);
    if (zs && st.rs[RS_ZENABLE] && st.rs[RS_ZWRITEENABLE])
        surface_backend_wrote(zs);
    return DD_OK;
}

// DrawPrimitive(type, fvf, lpvVertices, dwVertexCount, dwFlags)
void Dev_DrawPrimitive(X86 *c) {
    Device *d = this_device(c);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t count = arg(c, 4);
    com_ret(c, submit(d, arg(c, 1), arg(c, 2), arg(c, 3), count, count, 0, 0));
}
// DrawIndexedPrimitive(type, fvf, lpvVertices, dwVertexCount, lpwIndices, dwIndexCount, dwFlags)
void Dev_DrawIndexedPrimitive(X86 *c) {
    Device *d = this_device(c);
    if (!d) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    const uint32_t count = arg(c, 4);
    com_ret(c, submit(d, arg(c, 1), arg(c, 2), arg(c, 3), count, count, arg(c, 5), arg(c, 6)));
}
// DrawPrimitiveVB(type, lpd3dVertexBuffer, dwStartVertex, dwNumVertices, dwFlags)
void Dev_DrawPrimitiveVB(X86 *c) {
    Device *d = this_device(c);
    VertexBuffer *vb = vb_from(arg(c, 2));
    const uint32_t start = arg(c, 3), count = arg(c, 4);
    if (!d || !vb || start > vb->count || count > vb->count - start) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_ret(c, submit(d, arg(c, 1), vb->fvf, vb->mem + start * vb->stride, count, count, 0, 0));
}
// DrawIndexedPrimitiveVB(type, vb, dwStartVertex, dwNumVertices, lpwIndices, dwIndexCount, dwFlags):
// the indices are relative to dwStartVertex.
void Dev_DrawIndexedPrimitiveVB(X86 *c) {
    Device *d = this_device(c);
    VertexBuffer *vb = vb_from(arg(c, 2));
    const uint32_t start = arg(c, 3), count = arg(c, 4);
    if (!d || !vb || start > vb->count || count > vb->count - start) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    com_ret(c, submit(d, arg(c, 1), vb->fvf, vb->mem + start * vb->stride, count, vb->count - start,
                      arg(c, 5), arg(c, 6)));
}
DX_STUB(Dev_DrawPrimitiveStrided, DDERR_UNSUPPORTED)
DX_STUB(Dev_DrawIndexedPrimitiveStrided, DDERR_UNSUPPORTED)
DX_STUB(Dev_ComputeSphereVisibility, DDERR_UNSUPPORTED)
DX_STUB(Dev_BeginStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_EndStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_ApplyStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_CaptureStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_DeleteStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_CreateStateBlock, DDERR_UNSUPPORTED)
DX_STUB(Dev_Load, DDERR_UNSUPPORTED)
DX_STUB(Dev_GetInfo, DDERR_UNSUPPORTED)

const ComMethod g_device7[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetCaps", 2, Dev_GetCaps},
    {"EnumTextureFormats", 3, Dev_EnumTextureFormats},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"GetDirect3D", 2, Dev_GetDirect3D},
    {"SetRenderTarget", 3, Dev_SetRenderTarget},
    {"GetRenderTarget", 2, Dev_GetRenderTarget},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"SetRenderState", 3, Dev_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"BeginStateBlock", 1, Dev_BeginStateBlock},
    {"EndStateBlock", 2, Dev_EndStateBlock},
    {"PreLoad", 2, Dev_PreLoad},
    {"DrawPrimitive", 6, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 8, Dev_DrawIndexedPrimitive},
    {"SetClipStatus", 2, Dev_ClipStatus},
    {"GetClipStatus", 2, Dev_ClipStatus},
    {"DrawPrimitiveStrided", 6, Dev_DrawPrimitiveStrided},
    {"DrawIndexedPrimitiveStrided", 8, Dev_DrawIndexedPrimitiveStrided},
    {"DrawPrimitiveVB", 6, Dev_DrawPrimitiveVB},
    {"DrawIndexedPrimitiveVB", 8, Dev_DrawIndexedPrimitiveVB},
    {"ComputeSphereVisibility", 6, Dev_ComputeSphereVisibility},
    {"GetTexture", 3, Dev_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, Dev_GetTextureStageState},
    {"SetTextureStageState", 4, Dev_SetTextureStageState},
    {"ValidateDevice", 2, Dev_ValidateDevice},
    {"ApplyStateBlock", 2, Dev_ApplyStateBlock},
    {"CaptureStateBlock", 2, Dev_CaptureStateBlock},
    {"DeleteStateBlock", 2, Dev_DeleteStateBlock},
    {"CreateStateBlock", 3, Dev_CreateStateBlock},
    {"Load", 6, Dev_Load},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"GetInfo", 4, Dev_GetInfo},
};
static_assert(sizeof g_device7 / sizeof g_device7[0] == 49, "IDirect3DDevice7 has 49 slots");

// ===========================================================================
// IDirect3DVertexBuffer7
// ===========================================================================
VertexBuffer *this_vb(X86 *c) {
    return vb_from(arg(c, 0));
}
// Lock(dwFlags, lplpData, lpdwSize): the buffer is guest memory, read at draw time.
void VB_Lock(X86 *c) {
    VertexBuffer *vb = this_vb(c);
    const uint32_t out = arg(c, 2), size = arg(c, 3);
    if (!vb || !out || !gm_valid(out, 4)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(out, vb->mem);
    if (size && gm_valid(size, 4))
        wr32(size, vb->stride * vb->count);
    ++vb->locks;
    com_ret(c, DD_OK);
}
void VB_Unlock(X86 *c) {
    VertexBuffer *vb = this_vb(c);
    if (!vb) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    if (vb->locks > 0)
        --vb->locks;
    com_ret(c, DD_OK);
}
void VB_GetVertexBufferDesc(X86 *c) {
    VertexBuffer *vb = this_vb(c);
    const uint32_t a = arg(c, 1);
    if (!vb || !a || !gm_valid(a, D3DVBDESC_SIZE)) {
        com_ret(c, DDERR_INVALIDPARAMS);
        return;
    }
    wr32(a, D3DVBDESC_SIZE);
    wr32(a + 4, vb->caps);
    wr32(a + 8, vb->fvf);
    wr32(a + 12, vb->count);
    com_ret(c, DD_OK);
}
void VB_Optimize(X86 *c) {
    if (VertexBuffer *vb = this_vb(c))
        vb->caps |= VBCAPS_OPTIMIZED;
    com_ret(c, DD_OK);
}
DX_STUB(VB_ProcessVertices, DDERR_UNSUPPORTED)
DX_STUB(VB_ProcessVerticesStrided, DDERR_UNSUPPORTED)

const ComMethod g_vb7[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"Lock", 4, VB_Lock},
    {"Unlock", 1, VB_Unlock},
    {"ProcessVertices", 8, VB_ProcessVertices},
    {"GetVertexBufferDesc", 2, VB_GetVertexBufferDesc},
    {"Optimize", 3, VB_Optimize},
    {"ProcessVerticesStrided", 8, VB_ProcessVerticesStrided},
};

} // namespace

void d3d7_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    com_define(IF_D3D7, "DDRAW.dll", "IDirect3D7", g_d3d7, std::size(g_d3d7));
    com_define(IF_D3DDEVICE7, "DDRAW.dll", "IDirect3DDevice7", g_device7, std::size(g_device7));
    com_define(IF_D3DVB7, "DDRAW.dll", "IDirect3DVertexBuffer7", g_vb7, std::size(g_vb7));
    com_bind(IF_D3D7, K_DDRAW7);
    com_bind(IF_D3DDEVICE7, K_D3DDEVICE7);
    com_bind(IF_D3DVB7, K_D3DVB7);
    com_register_iid(IF_D3D7, IID_IDirect3D7);
    com_register_iid(IF_D3DDEVICE7, IID_IDirect3DDevice7);
    com_register_iid(IF_D3DVB7, IID_IDirect3DVertexBuffer7);
    com_set_destructor(K_D3DDEVICE7, device_destroy);
    com_set_destructor(K_D3DVB7, vb_destroy);
}

void d3d7_reset() {
    devices().clear();
    vbs().clear();
}

} // namespace d7
