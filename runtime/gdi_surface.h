// gdi_surface.h - GDI over pixels the caller owns: the bridge a DirectDraw
// surface's GetDC and ReleaseDC stand on.
//
// A game calls IDirectDrawSurface7::GetDC to draw GDI text into a texture
// (a glyph atlas, a caption) and ReleaseDC when it is done. The surface's
// pixels already live in the guest arena, so the DC is a DIB over them: every
// GDI call the game makes on it - SelectObject of its fonts, SetTextColor,
// SetBkMode, ExtTextOutA, GetTextExtentPoint32A, BitBlt - reads and writes
// that memory directly, measured and drawn exactly as on a window or a DIB
// section. Nothing is copied in or out.
//
// The DirectDraw side's part:
//
//   GetDC:      lock the surface's memory (it must not move or be freed while
//               the DC exists), then
//                   GdiSurfaceDesc d = {w, h, bpp, pitch, bits, masks..., palette};
//                   uint32_t hdc = gdi_surface_dc_open(d);
//               and hand hdc to the guest.
//   ReleaseDC:  gdi_surface_dc_close(hdc), then treat the pixels as written
//               (a new texture revision).
//
// The DC starts in Windows' default state: the system font, black text on an
// opaque white background, no clipping. Pixel formats: 8 bits with a palette
// (256 RGBQUADs; a grey ramp when null), 16 bits with any 5/6-bit or 4-bit
// channel masks (R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, ...), 24 bits, 32 bits
// with any 8-bit masks (X8R8G8B8, A8R8G8B8). GDI knows nothing of alpha, as on
// Windows: a channel outside the colour masks is written as 0.
//
// Call under the guest baton, like every GDI entry point.
#pragma once
#include <cstdint>

struct GdiSurfaceDesc {
    int32_t width = 0, height = 0;
    uint32_t bpp = 32;   // 8, 16, 24 or 32
    uint32_t pitch = 0;  // bytes per row; row 0 is the top
    uint32_t bits = 0;   // guest address of row 0
    uint32_t r_mask = 0; // 16 and 32 bits: the colour channels; all zero
    uint32_t g_mask = 0; // means R5G6B5 for 16 bits and X8R8G8B8 for 32
    uint32_t b_mask = 0;
    const uint32_t *palette = nullptr; // 8 bits: 256 entries 0x00RRGGBB
};

// A new HDC over the pixels, or 0 when the description is unusable.
uint32_t gdi_surface_dc_open(const GdiSurfaceDesc &desc);
// Forgets the DC; the pixels are untouched and stay the caller's. False when
// `hdc` is not an open surface DC.
bool gdi_surface_dc_close(uint32_t hdc);
// Whether `hdc` is an open surface DC.
bool gdi_surface_dc_is(uint32_t hdc);
