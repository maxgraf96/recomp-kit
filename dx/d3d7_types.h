// d3d7_types.h - DirectDraw 7 and Direct3D 7 constants and structure layouts.
//
// Everything here is the DirectX 7 SDK's (ddraw.h, d3d.h, d3dtypes.h,
// d3dcaps.h): names, values and byte offsets, written out because the guest
// hands the shim raw 32-bit structures. The names live in namespace d7 so that
// the version 1-6 constants dxtypes.h already defines keep their meaning.
#pragma once
#include <stdint.h>

namespace d7 {

// ---------------------------------------------------------------------------
// GUIDs, in their in-memory byte order.
// ---------------------------------------------------------------------------
#define D7_GUID(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                           \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}
static const uint8_t IID_IDirectDraw7[16] =
    D7_GUID(0x15e65ec0, 0x3b9c, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
static const uint8_t IID_IDirectDrawSurface7[16] =
    D7_GUID(0x06675a80, 0x3b9b, 0x11d2, 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
static const uint8_t IID_IDirect3D7[16] =
    D7_GUID(0xf5049e77, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
static const uint8_t IID_IDirect3DDevice7[16] =
    D7_GUID(0xf5049e79, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
static const uint8_t IID_IDirect3DVertexBuffer7[16] =
    D7_GUID(0xf5049e7d, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
// Device types: what CreateDevice and EnumDevices name.
static const uint8_t IID_IDirect3DHALDevice[16] =
    D7_GUID(0x84e63de0, 0x46aa, 0x11cf, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e);
static const uint8_t IID_IDirect3DTnLHalDevice[16] =
    D7_GUID(0xf5049e78, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
static const uint8_t IID_IDirect3DRGBDevice[16] =
    D7_GUID(0xa4665c60, 0x2673, 0x11cf, 0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56);
static const uint8_t IID_IDirect3DRefDevice[16] =
    D7_GUID(0x50936643, 0x13e9, 0x11d1, 0x89, 0xaa, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29);

// ---------------------------------------------------------------------------
// HRESULTs this shim returns beyond dxtypes.h's.
// ---------------------------------------------------------------------------
static const uint32_t DD_OK = 0;
static const uint32_t DDERR_ALREADYINITIALIZED = 0x88760005u;
static const uint32_t DDERR_NOTFOUND = 0x887600ffu;
static const uint32_t DDERR_INVALIDRECT = 0x88760096u;
static const uint32_t DDERR_NOTLOCKED = 0x88760248u;
static const uint32_t DDERR_NOCOLORKEY = 0x887600d7u;
static const uint32_t DDERR_NOTFLIPPABLE = 0x88760246u;
static const uint32_t DDERR_INVALIDPIXELFORMAT = 0x88760091u;
static const uint32_t DDERR_INVALIDCAPS = 0x88760064u;
static const uint32_t DDERR_INVALIDMODE = 0x88760078u;
static const uint32_t DDERR_SURFACEALREADYATTACHED = 0x8876019au;
static const uint32_t DDERR_CANNOTATTACHSURFACE = 0x8876000au;
static const uint32_t DDERR_SURFACENOTATTACHED = 0x887601ccu;
static const uint32_t DDERR_NODC = 0x8876024au;
static const uint32_t DDERR_DCALREADYCREATED = 0x8876026cu;
static const uint32_t DDERR_CANTCREATEDC = 0x88760249u;
static const uint32_t D3DERR_INVALIDVERTEXFORMAT = 0x887607fau;
static const uint32_t D3DERR_SCENE_IN_SCENE = 0x887602f8u;
static const uint32_t D3DERR_SCENE_NOT_IN_SCENE = 0x887602f9u;
static const uint32_t D3DERR_VBUF_CREATE_FAILED = 0x8876080du;
static const uint32_t D3DERR_VERTEXBUFFERLOCKED = 0x8876080eu;
static const uint32_t D3DERR_LIGHT_SET_FAILED = 0x887602eeu;
static const uint32_t D3DERR_INVALID_DEVICE = 0x887602c1u;
static const uint32_t E_NOTIMPL_ = 0x80004001u;
static const uint32_t E_NOINTERFACE_ = 0x80004002u;
static const uint32_t E_OUTOFMEMORY_ = 0x8007000eu;
static const uint32_t E_INVALIDARG_ = 0x80070057u;
static const uint32_t CLASS_E_NOAGGREGATION_ = 0x80040110u;
// The enumeration callbacks' "go on" and "stop".
static const uint32_t DDENUMRET_OK = 1, DDENUMRET_CANCEL = 0;

// ---------------------------------------------------------------------------
// DDSURFACEDESC2 (124 bytes) and DDPIXELFORMAT (32 bytes).
// ---------------------------------------------------------------------------
enum : uint32_t {
    DDSD2_SIZE = 124,
    DDSD2_dwSize = 0x00,
    DDSD2_dwFlags = 0x04,
    DDSD2_dwHeight = 0x08,
    DDSD2_dwWidth = 0x0c,
    DDSD2_lPitch = 0x10,
    DDSD2_dwBackBufferCount = 0x14,
    DDSD2_dwMipMapCount = 0x18, // also dwRefreshRate
    DDSD2_dwAlphaBitDepth = 0x1c,
    DDSD2_lpSurface = 0x24,
    DDSD2_ddckCKDestOverlay = 0x28,
    DDSD2_ddckCKDestBlt = 0x30,
    DDSD2_ddckCKSrcOverlay = 0x38,
    DDSD2_ddckCKSrcBlt = 0x40,
    DDSD2_ddpfPixelFormat = 0x48,
    DDSD2_ddsCaps = 0x68, // DDSCAPS2: dwCaps, dwCaps2, dwCaps3, dwCaps4
    DDSD2_dwTextureStage = 0x78,

    DDPF_SIZE_ = 32,
    DDPF_dwSize = 0x00,
    DDPF_dwFlags = 0x04,
    DDPF_dwFourCC = 0x08,
    DDPF_dwRGBBitCount = 0x0c, // also dwZBufferBitDepth
    DDPF_dwRBitMask = 0x10,    // also dwStencilBitDepth
    DDPF_dwGBitMask = 0x14,    // also dwZBitMask
    DDPF_dwBBitMask = 0x18,    // also dwStencilBitMask
    DDPF_dwRGBAlphaBitMask = 0x1c,
};
static const uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                      DDSD_BACKBUFFERCOUNT = 0x20, DDSD_ZBUFFERBITDEPTH = 0x40,
                      DDSD_ALPHABITDEPTH = 0x80, DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000,
                      DDSD_CKDESTOVERLAY = 0x2000, DDSD_CKDESTBLT = 0x4000,
                      DDSD_CKSRCOVERLAY = 0x8000, DDSD_CKSRCBLT = 0x10000,
                      DDSD_MIPMAPCOUNT = 0x20000, DDSD_REFRESHRATE = 0x40000,
                      DDSD_LINEARSIZE = 0x80000, DDSD_TEXTURESTAGE = 0x100000;
static const uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4,
                      DDPF_PALETTEINDEXED8 = 0x20, DDPF_RGB = 0x40, DDPF_ZBUFFER = 0x400,
                      DDPF_STENCILBUFFER = 0x4000, DDPF_LUMINANCE = 0x20000,
                      DDPF_BUMPDUDV = 0x80000;

// DDSCAPS2.dwCaps
static const uint32_t DDSCAPS_ALPHA = 0x2, DDSCAPS_BACKBUFFER = 0x4, DDSCAPS_COMPLEX = 0x8,
                      DDSCAPS_FLIP = 0x10, DDSCAPS_FRONTBUFFER = 0x20,
                      DDSCAPS_OFFSCREENPLAIN = 0x40, DDSCAPS_OVERLAY = 0x80,
                      DDSCAPS_PALETTE = 0x100, DDSCAPS_PRIMARYSURFACE = 0x200,
                      DDSCAPS_SYSTEMMEMORY = 0x800, DDSCAPS_TEXTURE = 0x1000,
                      DDSCAPS_3DDEVICE = 0x2000, DDSCAPS_VIDEOMEMORY = 0x4000,
                      DDSCAPS_VISIBLE = 0x8000, DDSCAPS_WRITEONLY = 0x10000,
                      DDSCAPS_ZBUFFER = 0x20000, DDSCAPS_OWNDC = 0x40000, DDSCAPS_MIPMAP = 0x400000,
                      DDSCAPS_ALLOCONLOAD = 0x4000000, DDSCAPS_LOCALVIDMEM = 0x10000000,
                      DDSCAPS_NONLOCALVIDMEM = 0x20000000;
// DDSCAPS2.dwCaps2
static const uint32_t DDSCAPS2_HINTDYNAMIC = 0x4, DDSCAPS2_HINTSTATIC = 0x8,
                      DDSCAPS2_TEXTUREMANAGE = 0x10, DDSCAPS2_OPAQUE = 0x80,
                      DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_MIPMAPSUBLEVEL = 0x10000,
                      DDSCAPS2_D3DTEXTUREMANAGE = 0x20000;

// ---------------------------------------------------------------------------
// IDirectDraw7 arguments.
// ---------------------------------------------------------------------------
static const uint32_t DDSCL_FULLSCREEN = 0x1, DDSCL_ALLOWREBOOT = 0x2, DDSCL_NOWINDOWCHANGES = 0x4,
                      DDSCL_NORMAL = 0x8, DDSCL_EXCLUSIVE = 0x10, DDSCL_ALLOWMODEX = 0x40,
                      DDSCL_SETFOCUSWINDOW = 0x80, DDSCL_SETDEVICEWINDOW = 0x100,
                      DDSCL_CREATEDEVICEWINDOW = 0x200, DDSCL_MULTITHREADED = 0x400,
                      DDSCL_FPUSETUP = 0x800, DDSCL_FPUPRESERVE = 0x1000;
static const uint32_t DDEDM_REFRESHRATES = 0x1, DDEDM_STANDARDVGAMODES = 0x2;

// DDCAPS_DX7, 380 bytes.
enum : uint32_t {
    DDCAPS7_SIZE = 0x17c,
    DDCAPS_dwSize = 0x00,
    DDCAPS_dwCaps = 0x04,
    DDCAPS_dwCaps2 = 0x08,
    DDCAPS_dwCKeyCaps = 0x0c,
    DDCAPS_dwFXCaps = 0x10,
    DDCAPS_dwZBufferBitDepths = 0x38,
    DDCAPS_dwVidMemTotal = 0x3c,
    DDCAPS_dwVidMemFree = 0x40,
    DDCAPS_ddsOldCaps = 0x84,
    DDCAPS_dwSVBCaps = 0xac,
    DDCAPS_dwVSBCaps = 0xd8,
    DDCAPS_dwSSBCaps = 0x104,
    DDCAPS_ddsCaps = 0x16c, // DDSCAPS2
};
static const uint32_t DDCAPS_3D = 0x1, DDCAPS_BLT = 0x40, DDCAPS_BLTSTRETCH = 0x200,
                      DDCAPS_COLORKEY = 0x400000, DDCAPS_COLORKEYHWASSIST = 0x1000000,
                      DDCAPS_BLTCOLORFILL = 0x4000000, DDCAPS_BLTDEPTHFILL = 0x10000000,
                      DDCAPS_CANCLIP = 0x20000000, DDCAPS_CANCLIPSTRETCHED = 0x40000000,
                      DDCAPS_CANBLTSYSMEM = 0x80000000u;
static const uint32_t DDCAPS2_WIDESURFACES = 0x1000, DDCAPS2_CANRENDERWINDOWED = 0x80000,
                      DDCAPS2_FLIPINTERVAL = 0x200000, DDCAPS2_FLIPNOVSYNC = 0x400000,
                      DDCAPS2_CANMANAGETEXTURE = 0x800000;
static const uint32_t DDCKEYCAPS_SRCBLT = 0x200;
static const uint32_t DDFXCAPS_BLTARITHSTRETCHY = 0x20, DDFXCAPS_BLTSHRINKX = 0x400,
                      DDFXCAPS_BLTSHRINKXN = 0x800, DDFXCAPS_BLTSHRINKY = 0x1000,
                      DDFXCAPS_BLTSHRINKYN = 0x2000, DDFXCAPS_BLTSTRETCHX = 0x4000,
                      DDFXCAPS_BLTSTRETCHXN = 0x8000, DDFXCAPS_BLTSTRETCHY = 0x10000,
                      DDFXCAPS_BLTSTRETCHYN = 0x20000;
// DDBD_ bit depths
static const uint32_t DDBD_8 = 0x800, DDBD_16 = 0x400, DDBD_24 = 0x200, DDBD_32 = 0x100;

// DDDEVICEIDENTIFIER2, 1068 bytes.
enum : uint32_t {
    DDDI2_SIZE = 0x42c,
    DDDI2_szDriver = 0x000,
    DDDI2_szDescription = 0x200,
    DDDI2_liDriverVersion = 0x400,
    DDDI2_dwVendorId = 0x408,
    DDDI2_dwDeviceId = 0x40c,
    DDDI2_dwSubSysId = 0x410,
    DDDI2_dwRevision = 0x414,
    DDDI2_guidDeviceIdentifier = 0x418,
    DDDI2_dwWHQLLevel = 0x428,
};

// ---------------------------------------------------------------------------
// IDirectDrawSurface7 arguments.
// ---------------------------------------------------------------------------
static const uint32_t DDLOCK_WAIT = 0x1, DDLOCK_EVENT = 0x2, DDLOCK_READONLY = 0x10,
                      DDLOCK_WRITEONLY = 0x20, DDLOCK_NOSYSLOCK = 0x800,
                      DDLOCK_NOOVERWRITE = 0x1000, DDLOCK_DISCARDCONTENTS = 0x2000,
                      DDLOCK_DONOTWAIT = 0x4000;
static const uint32_t DDBLT_ALPHA_MASK = 0x1ff, DDBLT_ASYNC = 0x200, DDBLT_COLORFILL = 0x400,
                      DDBLT_DDFX = 0x800, DDBLT_DDROPS = 0x1000, DDBLT_KEYDEST = 0x2000,
                      DDBLT_KEYDESTOVERRIDE = 0x4000, DDBLT_KEYSRC = 0x8000,
                      DDBLT_KEYSRCOVERRIDE = 0x10000, DDBLT_ROP = 0x20000,
                      DDBLT_ROTATIONANGLE = 0x40000, DDBLT_ZBUFFER = 0x80000,
                      DDBLT_WAIT = 0x1000000, DDBLT_DEPTHFILL = 0x2000000,
                      DDBLT_DONOTWAIT = 0x8000000;
static const uint32_t DDBLTFAST_NOCOLORKEY = 0x0, DDBLTFAST_SRCCOLORKEY = 0x1,
                      DDBLTFAST_DESTCOLORKEY = 0x2, DDBLTFAST_WAIT = 0x10,
                      DDBLTFAST_DONOTWAIT = 0x20;
static const uint32_t DDFLIP_WAIT = 0x1, DDFLIP_EVEN = 0x2, DDFLIP_ODD = 0x4, DDFLIP_NOVSYNC = 0x8,
                      DDFLIP_DONOTWAIT = 0x20;
static const uint32_t DDCKEY_COLORSPACE = 0x1, DDCKEY_DESTBLT = 0x2, DDCKEY_DESTOVERLAY = 0x4,
                      DDCKEY_SRCBLT = 0x8, DDCKEY_SRCOVERLAY = 0x10;
static const uint32_t SRCCOPY_ROP = 0x00cc0020u;
// DDBLTFX, 100 bytes.
enum : uint32_t {
    DDBLTFX_SIZE = 100,
    DDBLTFX_dwDDFX = 0x04,
    DDBLTFX_dwROP = 0x08,
    DDBLTFX_dwFillColor = 0x50, // also dwFillDepth
    DDBLTFX_ddckDestColorkey = 0x54,
    DDBLTFX_ddckSrcColorkey = 0x5c,
};
static const uint32_t DDBLTFX_MIRRORLEFTRIGHT = 0x2, DDBLTFX_MIRRORUPDOWN = 0x4;

// ---------------------------------------------------------------------------
// Direct3D 7.
// ---------------------------------------------------------------------------
// D3DDEVICEDESC7, 236 bytes; D3DPRIMCAPS, 56 bytes.
enum : uint32_t {
    D3DDD7_SIZE = 0xec,
    D3DDD7_dwDevCaps = 0x00,
    D3DDD7_dpcLineCaps = 0x04,
    D3DDD7_dpcTriCaps = 0x3c,
    D3DDD7_dwDeviceRenderBitDepth = 0x74,
    D3DDD7_dwDeviceZBufferBitDepth = 0x78,
    D3DDD7_dwMinTextureWidth = 0x7c,
    D3DDD7_dwMinTextureHeight = 0x80,
    D3DDD7_dwMaxTextureWidth = 0x84,
    D3DDD7_dwMaxTextureHeight = 0x88,
    D3DDD7_dwMaxTextureRepeat = 0x8c,
    D3DDD7_dwMaxTextureAspectRatio = 0x90,
    D3DDD7_dwMaxAnisotropy = 0x94,
    D3DDD7_dvGuardBandLeft = 0x98,
    D3DDD7_dvGuardBandTop = 0x9c,
    D3DDD7_dvGuardBandRight = 0xa0,
    D3DDD7_dvGuardBandBottom = 0xa4,
    D3DDD7_dvExtentsAdjust = 0xa8,
    D3DDD7_dwStencilCaps = 0xac,
    D3DDD7_dwFVFCaps = 0xb0,
    D3DDD7_dwTextureOpCaps = 0xb4,
    D3DDD7_wMaxTextureBlendStages = 0xb8,
    D3DDD7_wMaxSimultaneousTextures = 0xba,
    D3DDD7_dwMaxActiveLights = 0xbc,
    D3DDD7_dvMaxVertexW = 0xc0,
    D3DDD7_deviceGUID = 0xc4,
    D3DDD7_wMaxUserClipPlanes = 0xd4,
    D3DDD7_wMaxVertexBlendMatrices = 0xd6,
    D3DDD7_dwVertexProcessingCaps = 0xd8,

    D3DPRIMCAPS_SIZE = 56,
    PC_dwSize = 0x00,
    PC_dwMiscCaps = 0x04,
    PC_dwRasterCaps = 0x08,
    PC_dwZCmpCaps = 0x0c,
    PC_dwSrcBlendCaps = 0x10,
    PC_dwDestBlendCaps = 0x14,
    PC_dwAlphaCmpCaps = 0x18,
    PC_dwShadeCaps = 0x1c,
    PC_dwTextureCaps = 0x20,
    PC_dwTextureFilterCaps = 0x24,
    PC_dwTextureBlendCaps = 0x28,
    PC_dwTextureAddressCaps = 0x2c,
    PC_dwStippleWidth = 0x30,
    PC_dwStippleHeight = 0x34,
};
static const uint32_t D3DDEVCAPS_FLOATTLVERTEX = 0x1, D3DDEVCAPS_EXECUTESYSTEMMEMORY = 0x10,
                      D3DDEVCAPS_TLVERTEXSYSTEMMEMORY = 0x40, D3DDEVCAPS_TLVERTEXVIDEOMEMORY = 0x80,
                      D3DDEVCAPS_TEXTURESYSTEMMEMORY = 0x100, D3DDEVCAPS_TEXTUREVIDEOMEMORY = 0x200,
                      D3DDEVCAPS_DRAWPRIMTLVERTEX = 0x400, D3DDEVCAPS_CANRENDERAFTERFLIP = 0x800,
                      D3DDEVCAPS_DRAWPRIMITIVES2 = 0x2000, D3DDEVCAPS_DRAWPRIMITIVES2EX = 0x8000,
                      D3DDEVCAPS_HWTRANSFORMANDLIGHT = 0x10000,
                      D3DDEVCAPS_CANBLTSYSTONONLOCAL = 0x20000,
                      D3DDEVCAPS_HWRASTERIZATION = 0x80000;
static const uint32_t D3DPMISCCAPS_MASKZ = 0x2, D3DPMISCCAPS_CULLNONE = 0x10,
                      D3DPMISCCAPS_CULLCW = 0x20, D3DPMISCCAPS_CULLCCW = 0x40;
static const uint32_t D3DPRASTERCAPS_DITHER = 0x1, D3DPRASTERCAPS_ZTEST = 0x10,
                      D3DPRASTERCAPS_SUBPIXEL = 0x20, D3DPRASTERCAPS_FOGVERTEX = 0x80,
                      D3DPRASTERCAPS_FOGTABLE = 0x100,
                      D3DPRASTERCAPS_ANTIALIASSORTINDEPENDENT = 0x800,
                      D3DPRASTERCAPS_MIPMAPLODBIAS = 0x2000, D3DPRASTERCAPS_FOGRANGE = 0x10000,
                      D3DPRASTERCAPS_ANISOTROPY = 0x20000, D3DPRASTERCAPS_WFOG = 0x100000,
                      D3DPRASTERCAPS_ZFOG = 0x200000;
static const uint32_t D3DPSHADECAPS_COLORFLATRGB = 0x2, D3DPSHADECAPS_COLORGOURAUDRGB = 0x8,
                      D3DPSHADECAPS_SPECULARGOURAUDRGB = 0x200,
                      D3DPSHADECAPS_ALPHAFLATBLEND = 0x1000,
                      D3DPSHADECAPS_ALPHAGOURAUDBLEND = 0x4000, D3DPSHADECAPS_FOGFLAT = 0x10000,
                      D3DPSHADECAPS_FOGGOURAUD = 0x20000;
static const uint32_t D3DPTEXTURECAPS_PERSPECTIVE = 0x1, D3DPTEXTURECAPS_ALPHA = 0x4,
                      D3DPTEXTURECAPS_TRANSPARENCY = 0x8, D3DPTEXTURECAPS_BORDER = 0x10;
static const uint32_t D3DPTFILTERCAPS_ALL = 0x1f03073fu;
static const uint32_t D3DPTADDRESSCAPS_ALL = 0x1f;
static const uint32_t D3DFVFCAPS_TEXCOORDCOUNTMASK = 0xffff;
static const uint32_t D3DVTXPCAPS_MATERIALSOURCE7 = 0x2, D3DVTXPCAPS_VERTEXFOG = 0x4,
                      D3DVTXPCAPS_DIRECTIONALLIGHTS = 0x8, D3DVTXPCAPS_POSITIONALLIGHTS = 0x10,
                      D3DVTXPCAPS_LOCALVIEWER = 0x20;

// D3DRENDERSTATETYPE
enum : uint32_t {
    RS_ANTIALIAS = 2,
    RS_TEXTUREPERSPECTIVE = 4,
    RS_ZENABLE = 7,
    RS_FILLMODE = 8,
    RS_SHADEMODE = 9,
    RS_LINEPATTERN = 10,
    RS_ZWRITEENABLE = 14,
    RS_ALPHATESTENABLE = 15,
    RS_LASTPIXEL = 16,
    RS_SRCBLEND = 19,
    RS_DESTBLEND = 20,
    RS_CULLMODE = 22,
    RS_ZFUNC = 23,
    RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25,
    RS_DITHERENABLE = 26,
    RS_ALPHABLENDENABLE = 27,
    RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29,
    RS_ZVISIBLE = 30,
    RS_STIPPLEDALPHA = 33,
    RS_FOGCOLOR = 34,
    RS_FOGTABLEMODE = 35,
    RS_FOGSTART = 36,
    RS_FOGEND = 37,
    RS_FOGDENSITY = 38,
    RS_EDGEANTIALIAS = 40,
    RS_COLORKEYENABLE = 41,
    RS_ZBIAS = 47,
    RS_RANGEFOGENABLE = 48,
    RS_STENCILENABLE = 52,
    RS_TEXTUREFACTOR = 60,
    RS_WRAP0 = 128,
    RS_CLIPPING = 136,
    RS_LIGHTING = 137,
    RS_EXTENTS = 138,
    RS_AMBIENT = 139,
    RS_FOGVERTEXMODE = 140,
    RS_COLORVERTEX = 141,
    RS_LOCALVIEWER = 142,
    RS_NORMALIZENORMALS = 143,
    RS_COLORKEYBLENDENABLE = 144,
    RS_DIFFUSEMATERIALSOURCE = 145,
    RS_SPECULARMATERIALSOURCE = 146,
    RS_AMBIENTMATERIALSOURCE = 147,
    RS_EMISSIVEMATERIALSOURCE = 148,
    RS_VERTEXBLEND = 151,
    RS_CLIPPLANEENABLE = 152,
    RS_COUNT = 256,
};
// D3DTEXTURESTAGESTATETYPE
enum : uint32_t {
    TSS_COLOROP = 1,
    TSS_COLORARG1 = 2,
    TSS_COLORARG2 = 3,
    TSS_ALPHAOP = 4,
    TSS_ALPHAARG1 = 5,
    TSS_ALPHAARG2 = 6,
    TSS_TEXCOORDINDEX = 11,
    TSS_ADDRESS = 12,
    TSS_ADDRESSU = 13,
    TSS_ADDRESSV = 14,
    TSS_BORDERCOLOR = 15,
    TSS_MAGFILTER = 16,
    TSS_MINFILTER = 17,
    TSS_MIPFILTER = 18,
    TSS_MIPMAPLODBIAS = 19,
    TSS_MAXMIPLEVEL = 20,
    TSS_MAXANISOTROPY = 21,
    TSS_TEXTURETRANSFORMFLAGS = 24,
    TSS_COUNT = 32,
};
// D3DTEXTUREOP
enum : uint32_t {
    TOP_DISABLE = 1,
    TOP_SELECTARG1 = 2,
    TOP_SELECTARG2 = 3,
    TOP_MODULATE = 4,
    TOP_MODULATE2X = 5,
    TOP_MODULATE4X = 6,
    TOP_ADD = 7,
    TOP_ADDSIGNED = 8,
    TOP_ADDSIGNED2X = 9,
    TOP_SUBTRACT = 10,
    TOP_ADDSMOOTH = 11,
    TOP_BLENDDIFFUSEALPHA = 12,
    TOP_BLENDTEXTUREALPHA = 13,
    TOP_BLENDFACTORALPHA = 14,
    TOP_BLENDTEXTUREALPHAPM = 15,
    TOP_BLENDCURRENTALPHA = 16,
    TOP_PREMODULATE = 17,
    TOP_MODULATEALPHA_ADDCOLOR = 18,
    TOP_MODULATECOLOR_ADDALPHA = 19,
    TOP_MODULATEINVALPHA_ADDCOLOR = 20,
    TOP_MODULATEINVCOLOR_ADDALPHA = 21,
    TOP_DOTPRODUCT3 = 24,
};
// D3DTA_
static const uint32_t TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3,
                      TA_SPECULAR = 4, TA_SELECTMASK = 0xf, TA_COMPLEMENT = 0x10,
                      TA_ALPHAREPLICATE = 0x20;
// D3DTEXTUREADDRESS, filters
static const uint32_t TADDRESS_WRAP = 1, TADDRESS_MIRROR = 2, TADDRESS_CLAMP = 3,
                      TADDRESS_BORDER = 4;
static const uint32_t TFG_POINT = 1, TFG_LINEAR = 2, TFG_FLATCUBIC = 3, TFG_GAUSSIANCUBIC = 4,
                      TFG_ANISOTROPIC = 5;
static const uint32_t TFN_POINT = 1, TFN_LINEAR = 2, TFN_ANISOTROPIC = 3;
static const uint32_t TFP_NONE = 1, TFP_POINT = 2, TFP_LINEAR = 3;
// D3DBLEND, D3DCMPFUNC, D3DCULL, D3DFILLMODE, D3DSHADEMODE, D3DFOGMODE, D3DZBUFFERTYPE
static const uint32_t BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SRCCOLOR = 3, BLEND_INVSRCCOLOR = 4,
                      BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, BLEND_DESTALPHA = 7,
                      BLEND_INVDESTALPHA = 8, BLEND_DESTCOLOR = 9, BLEND_INVDESTCOLOR = 10,
                      BLEND_SRCALPHASAT = 11, BLEND_BOTHSRCALPHA = 12, BLEND_BOTHINVSRCALPHA = 13;
static const uint32_t CMP_NEVER = 1, CMP_LESS = 2, CMP_EQUAL = 3, CMP_LESSEQUAL = 4,
                      CMP_GREATER = 5, CMP_NOTEQUAL = 6, CMP_GREATEREQUAL = 7, CMP_ALWAYS = 8;
static const uint32_t CULL_NONE = 1, CULL_CW = 2, CULL_CCW = 3;
static const uint32_t FILL_POINT = 1, FILL_WIREFRAME = 2, FILL_SOLID = 3;
static const uint32_t SHADE_FLAT = 1, SHADE_GOURAUD = 2, SHADE_PHONG = 3;
static const uint32_t FOG_NONE = 0, FOG_EXP = 1, FOG_EXP2 = 2, FOG_LINEAR = 3;
static const uint32_t ZB_FALSE = 0, ZB_TRUE = 1, ZB_USEW = 2;
static const uint32_t MCS_MATERIAL = 0, MCS_COLOR1 = 1, MCS_COLOR2 = 2;
// D3DPRIMITIVETYPE
static const uint32_t PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRIANGLELIST = 4,
                      PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6;
// D3DTRANSFORMSTATETYPE
static const uint32_t TS_WORLD = 1, TS_VIEW = 2, TS_PROJECTION = 3, TS_WORLD1 = 4, TS_TEXTURE0 = 16;
// D3DCLEAR_
static const uint32_t CLEAR_TARGET = 0x1, CLEAR_ZBUFFER = 0x2, CLEAR_STENCIL = 0x4;
// D3DLIGHTTYPE
static const uint32_t LIGHT_POINT = 1, LIGHT_SPOT = 2, LIGHT_DIRECTIONAL = 3,
                      LIGHT_PARALLELPOINT = 4;
// D3DFVF_
static const uint32_t FVF_POSITION_MASK = 0x00e, FVF_XYZ = 0x002, FVF_XYZRHW = 0x004,
                      FVF_XYZB1 = 0x006, FVF_XYZB5 = 0x00e, FVF_NORMAL = 0x010,
                      FVF_RESERVED1 = 0x020, FVF_DIFFUSE = 0x040, FVF_SPECULAR = 0x080,
                      FVF_TEXCOUNT_MASK = 0xf00, FVF_TEXCOUNT_SHIFT = 8;
// D3DVERTEXBUFFERDESC caps
static const uint32_t VBCAPS_SYSTEMMEMORY = 0x800, VBCAPS_WRITEONLY = 0x10000,
                      VBCAPS_OPTIMIZED = 0x80000000u, VBCAPS_DONOTCLIP = 0x1;
static const uint32_t D3DVBDESC_SIZE = 16;
// Structure sizes the guest passes.
static const uint32_t D3DVIEWPORT7_SIZE = 24, D3DMATERIAL7_SIZE = 68, D3DLIGHT7_SIZE = 104,
                      D3DMATRIX_SIZE = 64;
// D3DLIGHT7 field offsets
enum : uint32_t {
    L7_dltType = 0x00,
    L7_dcvDiffuse = 0x04,
    L7_dcvSpecular = 0x14,
    L7_dcvAmbient = 0x24,
    L7_dvPosition = 0x34,
    L7_dvDirection = 0x40,
    L7_dvRange = 0x4c,
    L7_dvFalloff = 0x50,
    L7_dvAttenuation0 = 0x54,
    L7_dvAttenuation1 = 0x58,
    L7_dvAttenuation2 = 0x5c,
    L7_dvTheta = 0x60,
    L7_dvPhi = 0x64,
};
static const uint32_t D3DENUMRET_OK = 1, D3DENUMRET_CANCEL = 0;

} // namespace d7
