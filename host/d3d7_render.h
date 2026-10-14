// d3d7_render.h - the GPU renderer behind the DirectDraw 7 / Direct3D 7 seam
// (dx/d3d7_host.h), over gpu.h.
//
// Every surface the shim names is a GPU texture here; DirectDraw fills and
// blits, Direct3D draws and Flip's copy all run on the GPU, in one command
// stream, in the order the guest submitted them. Pixels cross to the CPU only
// when the shim asks (a Lock of a surface the GPU wrote), and from it only as
// uploads encoded in the same stream, so a draw submitted before an upload
// samples the old pixels.
#pragma once
#include "../dx/d3d7_host.h"
#include "gpu/gpu.h"

#include <cstdint>
#include <memory>

class D3D7Renderer {
  public:
    explicit D3D7Renderer(gpu::Device *device);
    ~D3D7Renderer();
    D3D7Renderer(const D3D7Renderer &) = delete;
    D3D7Renderer &operator=(const D3D7Renderer &) = delete;

    // False when the shaders or pipelines are missing (a backend without the
    // "d3d7" programs); nothing else may be called then.
    bool ok() const;
    gpu::Device *device() const;
    uint32_t generation() const;

    // The seam, one to one.
    bool create(uint32_t id, int w, int h, int levels, uint32_t flags);
    void destroy(uint32_t id);
    void upload(uint32_t id, int level, const HostD7Rect &r, const void *bgra, int pitch);
    bool readback(uint32_t id, int level, const HostD7Rect &r, void *bgra, int pitch);
    void fill(uint32_t id, const HostD7Rect &r, uint32_t bgra);
    void fill_depth(uint32_t id, const HostD7Rect &r, float z);
    void blit(const HostD7Blit &b);
    void draw(const HostD7Draw &d);
    void present(uint32_t id, const HostD7Rect &r, int mode_w, int mode_h);
    void flush();
    // Commits and waits for everything submitted.
    void finish();
    HostD7Stats stats() const;

    // The renderer the seam uses. Hosts with a presenter get one made on the
    // presenter's device on first use; tests install their own.
    static D3D7Renderer *shared();
    static void setShared(D3D7Renderer *renderer);

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

// How a presented frame reaches the screen. The renderer encodes everything
// that makes `texture` (w x h, BGRA8) into `cb` and then calls show(); a host
// with a GPU presenter stages the texture into its frame on `cb` and returns
// true, after which the renderer commits `cb` and calls shown(). The weak
// defaults return false, and the renderer then reads the pixels back and hands
// them to host_display_present_window.
bool host_d7_show(gpu::Texture texture, int w, int h, int mode_w, int mode_h,
                  gpu::CommandBuffer cb);
void host_d7_shown(int w, int h);
// The presenter's device, when a host has one ready for guest GPU work.
gpu::Device *host_d7_presenter_device();

// Frame dumps for comparison with reference screenshots. With
// RECOMP_D3D7_DUMP_DIR set, every RECOMP_D3D7_DUMP_EVERY-th present (default
// every one) is written there as frame-NNNNNN.ppm (binary PPM, the presented
// region at guest resolution). host_d7_dump_next(path) writes the next
// present to `path` once, whatever the environment says.
extern "C" void host_d7_dump_next(const char *path);
// The presenter is about to stop: finish GPU work and drop the renderer made on
// its device.
extern "C" void host_d7_release_device(void);
