// d3d7_internal.h - what ddraw7.cpp, d3d7.cpp and d3d7_soft.cpp share.
//
// The DirectDraw 7 surface model:
//
//   * Every surface has a format, a size and, once something asks for its
//     pixels, guest memory in that format (lazily allocated, zeroed).
//   * When a backend renders (the GPU renderer behind d3d7_host.h, or the
//     software reference renderer in d3d7_soft.cpp), a root surface - a
//     single surface or the top of a mip chain - also has a backend resource.
//   * Each surface (each mip level) records which copy is current:
//     `cpu_valid` (guest memory) and `gpu_valid` (the backend's level). At
//     least one is always true. A CPU write (Unlock, ReleaseDC, a CPU Blt)
//     leaves only the guest copy current; a backend write (a draw, a Blt on the
//     backend) leaves only the backend's. Reading the stale side copies first.
//
// So pixels move only when the other side asks for them: a texture filled by
// Lock is uploaded at its first draw, a render target is read back only when
// the guest locks it, and a Blt between two backend surfaces never touches the
// CPU.
#pragma once
#include "com.h"
#include "d3d7_host.h"
#include "d3d7_types.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace d7 {

// ---------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------
struct Format {
    uint32_t flags = 0, fourcc = 0, bpp = 0;
    uint32_t r = 0, g = 0, b = 0, a = 0; // masks; for a depth format g is the depth mask
    bool depth() const {
        return (flags & DDPF_ZBUFFER) != 0;
    }
    bool rgb() const {
        return (flags & DDPF_RGB) != 0;
    }
    bool has_alpha() const {
        return rgb() && (flags & DDPF_ALPHAPIXELS) && a;
    }
    uint32_t bytes() const {
        return (bpp + 7) / 8;
    }
};
// Reads / writes a guest DDPIXELFORMAT.
Format read_format(uint32_t addr);
void write_format(uint32_t addr, const Format &f);
// The formats this shim stores. False for anything else (FOURCC, palettes,
// luminance, bump maps, 24-bit).
bool format_supported(const Format &f);
Format format_x8r8g8b8();
Format format_a8r8g8b8();
Format format_r5g6b5();
Format format_depth(uint32_t bits);
// Mask helpers.
int mask_shift(uint32_t mask);
int mask_bits(uint32_t mask);
// One pixel of `f` as BGRA8 (alpha 255 when the format has none), and back.
uint32_t pixel_to_bgra(const Format &f, uint32_t pixel);
uint32_t bgra_to_pixel(const Format &f, uint32_t bgra);
// Rows of `f` to BGRA8 and back.
void row_to_bgra(const Format &f, const uint8_t *src, uint32_t *dst, int count);
void row_from_bgra(const Format &f, const uint32_t *src, uint8_t *dst, int count);
// A source colour key range in format `f`, as the backend's key.
HostD7Key make_key(const Format &f, uint32_t lo, uint32_t hi);
// Whether `pixel` matches the key range [lo, hi] on its colour bits.
bool key_matches(const Format &f, uint32_t pixel, uint32_t lo, uint32_t hi);

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
struct Surface {
    uint32_t id = 0; // ComObj id
    uint32_t dd = 0; // owning DirectDraw object id
    uint32_t caps = 0, caps2 = 0;
    Format format;
    uint32_t width = 0, height = 0, pitch = 0;
    uint32_t mem = 0; // guest pixels, 0 until something needs them

    // Complex surfaces. A mip chain's root holds its levels; a flip chain's
    // front buffer holds its back buffers. Children are implicit: they live
    // and die with their root.
    uint32_t root = 0;   // the surface whose backend resource holds this one: self, or a mip root
    uint32_t parent = 0; // an implicit child: the complex surface it belongs to
    int level = 0;       // mip level within the root's resource
    int levels = 1;      // root: number of mip levels
    std::vector<uint32_t> chain; // root: the other surfaces of the complex surface, in order
    bool implicit = false;
    uint32_t attached_z = 0;  // a depth surface attached with AddAttachedSurface
    uint32_t attached_to = 0; // depth surface: what it is attached to

    // Colour keys, in this surface's own pixel format.
    bool has_src_key = false, has_dst_key = false;
    uint32_t src_key_lo = 0, src_key_hi = 0, dst_key_lo = 0, dst_key_hi = 0;

    int lock_count = 0;
    bool lock_read_only = false;
    uint32_t dc = 0;     // GDI DC handed out by GetDC, 0 when none
    uint32_t dc_mem = 0; // a 32-bit shadow the DC draws into, for formats GDI cannot address

    // Coherence: see the comment at the top of this file.
    bool cpu_valid = true, gpu_valid = true;
    // What last wrote the guest copy (RECOMP_D3D7_TRANSFER_LOG names it at the
    // upload that follows).
    const char *cpu_writer = "the CPU";
    // The backend generation this surface's root resource was made under; 0
    // when it has none.
    uint32_t resource_generation = 0;

    uint32_t uniqueness = 1, priority = 0, lod = 0;
    std::map<std::string, std::vector<uint8_t>> private_data;

    bool is_root() const {
        return root == id;
    }
};

Surface *surface(uint32_t id);
Surface *surface_of(ComObj *o);
// The surface behind a guest IDirectDrawSurface7 pointer, or null.
Surface *surface_from_guest(uint32_t iface_ptr);
uint32_t surface_view(Surface *s); // the guest IDirectDrawSurface7 pointer

// Guest memory for the surface's pixels, allocated and zeroed on first use.
// 0 when the guest heap is exhausted.
uint32_t surface_memory(Surface *s);
// Makes guest memory current (reading the backend copy back if needed).
bool surface_cpu_current(Surface *s);
// The guest wrote the pixels: the backend copy is stale.
void surface_cpu_wrote(Surface *s);
// Makes the backend copy of `s`'s root current for every level (creating the
// resource and uploading stale levels). False without a backend or on failure.
bool surface_backend_current(Surface *s);
// A backend operation wrote level `s->level` of the root: the guest copy is stale.
void surface_backend_wrote(Surface *s);
// Whether operations on this surface run on the backend.
bool surface_on_backend(const Surface *s);

// ---------------------------------------------------------------------------
// The backend: the GPU seam, the software renderer, or none.
// ---------------------------------------------------------------------------
struct Backend {
    const char *name;
    int (*available)();
    uint32_t (*generation)();
    int (*create)(uint32_t id, int w, int h, int levels, uint32_t flags);
    void (*destroy)(uint32_t id);
    void (*upload)(uint32_t id, int level, const HostD7Rect *r, const void *bgra, int pitch);
    int (*readback)(uint32_t id, int level, const HostD7Rect *r, void *bgra, int pitch);
    void (*fill)(uint32_t id, const HostD7Rect *r, uint32_t bgra);
    void (*fill_depth)(uint32_t id, const HostD7Rect *r, float z);
    void (*blit)(const HostD7Blit *b);
    void (*swap)(uint32_t a, uint32_t b);
    void (*draw)(const HostD7Draw *d);
    void (*present)(uint32_t id, const HostD7Rect *r, int w, int h);
    void (*flush)();
};
// The backend in use: the software renderer when selected, otherwise the
// seam when a host serves it, otherwise null.
const Backend *backend();
const Backend *seam_backend();
const Backend *soft_backend();
// Test and diagnostic switch: route every operation to the software renderer
// (RECOMP_D3D7_SOFTWARE=1 selects it from the environment).
void use_soft_backend(bool on);

// ---------------------------------------------------------------------------
// Shared with d3d7.cpp
// ---------------------------------------------------------------------------
// The display mode and cooperative level of the DirectDraw object `dd`.
struct DisplayState {
    uint32_t w = 0, h = 0, bpp = 0, refresh = 0;
    bool mode_set = false;
    uint32_t coop = 0, hwnd = 0, focus_hwnd = 0;
    // The live primary surface (its root's id), 0 when there is none.
    uint32_t primary = 0;
    // FlipToGDISurface was called and no Flip has happened since: the last
    // presented frame stays on screen under the program's windows.
    bool gdi_showing = false;
    uint32_t flips_to_gdi = 0;
};
DisplayState &display_state();
// Primary and back buffers report the display mode's format.
Format display_format();

// Registration, from ddraw7.cpp and d3d7.cpp.
void ddraw7_register();
void d3d7_register();
void ddraw7_reset();
void d3d7_reset();
// DirectDrawCreateEx(IID_IDirectDraw7): writes the interface and returns DD_OK.
uint32_t ddraw7_create(uint32_t out);
// Whether DirectDrawCreateEx offers version 7: game.toml [runtime] ddraw7,
// overridden by RECOMP_DDRAW7, overridden by the test seam (-1 clears it).
bool ddraw7_enabled();
void ddraw7_enable_for_test(int on);
// The IDirect3D7 view of a DirectDraw 7 object.
void d3d7_define_interfaces();

} // namespace d7

// The software reference renderer (d3d7_soft.cpp). Test code reads its
// resources directly.
namespace d7soft {
// A resource's level 0 as BGRA8 words, row-major, or null.
const uint32_t *pixels(uint32_t id, int level, int *w, int *h);
const float *depth(uint32_t id, int *w, int *h);
void reset();
} // namespace d7soft
