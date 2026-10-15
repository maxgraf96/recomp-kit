// d3d7_host.h - the seam between the DirectDraw 7 / Direct3D 7 shims
// (ddraw7.cpp, d3d7.cpp) and the GPU renderer behind them (host/d3d7_render.cpp).
//
// The shims own the guest-visible objects and every rule the DirectX 7 API
// states; the host owns GPU resources and nothing else. A surface is named by
// the shim's object id of its ROOT: the top of a mip chain, or the surface
// itself. Mip levels are levels of the root's resource.
//
// Weak no-op defaults live in display_stubs.cpp: host_d7_available() is then 0
// and the shims keep every surface in guest memory and do every operation on
// the CPU, which is also how the headless tests exercise them.
//
// Threading: every call is made under the guest baton. Pointers handed to the
// host are valid only for the duration of the call; the host copies what it
// keeps. Rectangles have exclusive maxima and are already clipped to the
// surface by the shim.
//
// Pixel data crosses this seam as BGRA8: B at byte 0, the in-memory order of
// D3DFMT_A8R8G8B8. The shim converts every other surface format on the way.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HostD7Rect {
    int32_t x0, y0, x1, y1;
} HostD7Rect;

// 1 when a GPU renderer serves this seam. Checked on every operation, so a
// renderer that appears or goes away is followed.
int host_d7_available(void);
// Advances whenever the host has dropped every resource (a new device, a
// reset). A shim holding content only on the GPU under an older generation
// has lost it.
uint32_t host_d7_generation(void);

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
// Creates (or recreates, discarding contents) resource `id`: w x h with
// `levels` mip levels, a colour texture or, with HOST_D7_CREATE_DEPTH, a depth
// buffer. A new colour resource reads as transparent black, a new depth buffer
// as 1.0. HOST_D7_CREATE_OPAQUE marks a colour format without alpha: it starts
// opaque black, and nothing written to it changes its alpha, so a blend that
// reads the destination's alpha reads 1 as Direct3D specifies.
// Returns 0 when the resource could not be made.
#define HOST_D7_CREATE_DEPTH 1u
#define HOST_D7_CREATE_OPAQUE 2u
int host_d7_create(uint32_t id, int w, int h, int levels, uint32_t flags);
void host_d7_destroy(uint32_t id);
// Replaces a region of one level with BGRA8 pixels (`pitch` bytes per row).
// Ordered with everything submitted before it: a draw already submitted
// samples the old pixels.
void host_d7_upload(uint32_t id, int level, const HostD7Rect *r, const void *bgra, int pitch);
// Waits for everything submitted that writes `id`, then copies a region of one
// level out as BGRA8. Returns 0 on failure.
int host_d7_readback(uint32_t id, int level, const HostD7Rect *r, void *bgra, int pitch);

// ---------------------------------------------------------------------------
// Surface operations (DirectDraw Blt and Flip)
// ---------------------------------------------------------------------------
void host_d7_fill(uint32_t id, const HostD7Rect *r, uint32_t bgra);
void host_d7_fill_depth(uint32_t id, const HostD7Rect *r, float z);

// A source colour key: a source texel whose colour bits fall in [lo, hi] is
// not copied. The key is compared after quantising the texel to the source
// format's channel widths, so a 5-6-5 key matches exactly the texels that
// held that 5-6-5 value. Alpha does not take part.
typedef struct HostD7Key {
    uint8_t enabled;
    uint8_t bits[3]; // r, g, b channel widths of the source format
    uint8_t lo[3];   // r, g, b of the low key, each in 0..(1<<bits)-1
    uint8_t hi[3];
} HostD7Key;

typedef struct HostD7Blit {
    uint32_t dst;
    int32_t dst_level;
    HostD7Rect dst_rect;
    uint32_t src;
    int32_t src_level;
    HostD7Rect src_rect;
    HostD7Key key;
    uint8_t src_has_alpha; // 0: the source's alpha reads as 1
    uint8_t linear;        // 1: bilinear when stretching, 0: nearest
    uint8_t mirror_x, mirror_y;
} HostD7Blit;
void host_d7_blit(const HostD7Blit *b);
// Exchanges the resources named `a` and `b`: from now on `a` names what `b`
// held and `b` what `a` held, with no pixel copied. This is DirectDraw's Flip,
// which exchanges the memory behind a flip chain's buffers. Either may be
// missing, and the other one then moves. Work submitted before the exchange
// keeps the resource it named when it was submitted.
void host_d7_swap(uint32_t a, uint32_t b);

// Shows region `r` of resource `id` as the next frame, scaled by the host to
// its window. The resource itself is not changed. `w` x `h` is the guest's
// display mode, which sizes the window.
void host_d7_present(uint32_t id, const HostD7Rect *r, int w, int h);
// Commits everything submitted so far.
void host_d7_flush(void);

// ---------------------------------------------------------------------------
// Direct3D 7 draws
// ---------------------------------------------------------------------------
#define HOST_D7_MAX_LIGHTS 8
#define HOST_D7_RS_COUNT 256
#define HOST_D7_TSS_COUNT 32

// One enabled light, as D3DLIGHT7 gave it (world space).
typedef struct HostD7Light {
    uint32_t type; // D3DLIGHT_POINT 1, SPOT 2, DIRECTIONAL 3
    float diffuse[4], specular[4], ambient[4];
    float position[3], direction[3];
    float range, falloff, attenuation[3], theta, phi;
} HostD7Light;

// The device state a draw is made under. Matrices are D3DMATRIX as the guest
// wrote them: row-major, row vectors (v' = v * M).
typedef struct HostD7State {
    float world[16], view[16], projection[16];
    int32_t viewport[4]; // x, y, width, height in render-target pixels
    float viewport_minz, viewport_maxz;
    float material[17]; // diffuse rgba, ambient rgba, specular rgba, emissive rgba, power
    uint32_t light_count;
    HostD7Light lights[HOST_D7_MAX_LIGHTS]; // the enabled lights, in index order
    uint32_t rs[HOST_D7_RS_COUNT];          // indexed by D3DRENDERSTATETYPE
    uint32_t tss[2][HOST_D7_TSS_COUNT];     // stages 0 and 1, by D3DTEXTURESTAGESTATETYPE
} HostD7State;

// The stage-0 texture. id 0 is "no texture".
typedef struct HostD7Texture {
    uint32_t id;
    int32_t width, height, levels;
    uint8_t has_alpha; // 0: the format has no alpha and alpha reads as 1
    HostD7Key key;     // the texture's source colour key, for COLORKEYENABLE
} HostD7Texture;

typedef struct HostD7Draw {
    uint32_t target, depth; // surface ids; depth 0 when none is attached
    int32_t target_w, target_h;
    uint32_t primitive; // D3DPT_*
    uint32_t fvf, stride;
    const uint8_t *vertices; // vertex_count * stride bytes, the first vertex the draw uses
    uint32_t vertex_count;
    const uint16_t *indices; // null when not indexed; relative to `vertices`
    uint32_t index_count;
    const HostD7State *state;
    HostD7Texture texture;
} HostD7Draw;
void host_d7_draw(const HostD7Draw *d);

// ---------------------------------------------------------------------------
// Diagnostics and frame dumps
// ---------------------------------------------------------------------------
typedef struct HostD7Stats {
    uint64_t draws, primitives, passes, uploads, upload_bytes, readbacks, blits, fills, presents,
        pipelines;
} HostD7Stats;
void host_d7_stats(HostD7Stats *out);

#ifdef __cplusplus
}
#endif
