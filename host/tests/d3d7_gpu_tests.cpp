// d3d7_gpu_tests.cpp - the Direct3D 7 / DirectDraw 7 GPU renderer against the
// software reference renderer.
//
// Each synthetic scene of dx/tests/d3d7_scenes.h is driven through the guest
// ABI twice - once on dx/d3d7_soft.cpp, once on host/d3d7_render.cpp over the
// platform's GPU device - and the two images are compared channel by channel.
// A scene may differ by its stated tolerance everywhere, and by more only on
// its edge budget (rasterization and filtering at edges are the GPU's).
//
// Also here: Flip's exchange with a 16-bit software cursor that keeps a
// save-under per buffer (GPU blits, no readbacks), and a frame-cost benchmark
// at the call rate an engine produces.
// RECOMP_D3D7_TEST_DUMP=<dir> writes every compared pair as PPM.
#include "../../dx/tests/guest_abi.h"
#include "../../dx/tests/d3d7_scenes.h"
#include "../../dx/dx.h"
#include "../../runtime/display_seam.h"
#include "../../runtime/memory.h"
#include "../../platform/os.h"
#include "../d3d7_render.h"
#include "../gpu/gpu_factory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace d7t;

struct Shown {
    int w = 0, h = 0;
    std::vector<uint32_t> argb;
};
static std::vector<Shown> g_shown;
extern "C" void host_display_present_window(const uint32_t *argb, int w, int h) {
    Shown s;
    s.w = w;
    s.h = h;
    s.argb.assign(argb, argb + size_t(w) * size_t(h));
    g_shown.push_back(std::move(s));
}

static D3D7Renderer *g_renderer = nullptr;

static void dump(const std::string &name, const Image &img) {
    const char *dir = getenv("RECOMP_D3D7_TEST_DUMP");
    if (!dir || !*dir)
        return;
    std::string path = std::string(dir) + "/" + name + ".ppm";
    for (char &c : path)
        if (c == ' ' || c == ',')
            c = '_';
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    for (uint32_t p : img.px) {
        const uint8_t rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static Image run_on(bool soft, Image (*scene)()) {
    cpu_reset();
    d7::use_soft_backend(soft);
    Image img = scene();
    d7::use_soft_backend(false);
    return img;
}

// Every scene: GPU against the reference.
static void test_scenes() {
    for (const Scene &s : scenes()) {
        const Image ref = run_on(true, s.run);
        const Image gpu = run_on(false, s.run);
        ++g_checks;
        if (ref.w != gpu.w || ref.h != gpu.h || ref.px.empty()) {
            ++g_failures;
            fprintf(stderr, "FAIL %s: image sizes differ\n", s.name);
            continue;
        }
        int worst = 0;
        size_t over = 0;
        int wx = 0, wy = 0;
        for (int y = 0; y < ref.h; ++y)
            for (int x = 0; x < ref.w; ++x) {
                const int d = max_diff(ref.at(x, y), gpu.at(x, y), true);
                if (d > s.tolerance)
                    ++over;
                if (d > worst) {
                    worst = d;
                    wx = x;
                    wy = y;
                }
            }
        const double share = double(over) / double(ref.px.size());
        const bool ok = share <= s.edge_budget;
        printf("  %-28s max diff %3d at (%d,%d), %zu px over %d (%.2f%%, budget %.2f%%)%s\n",
               s.name, worst, wx, wy, over, s.tolerance, 100 * share, 100 * s.edge_budget,
               ok ? "" : "  FAILED");
        if (!ok) {
            ++g_failures;
            fprintf(stderr, "FAIL %s: reference %08x, GPU %08x at (%d,%d)\n", s.name,
                    ref.at(wx, wy), gpu.at(wx, wy), wx, wy);
        }
        dump(std::string(s.name) + "-ref", ref);
        dump(std::string(s.name) + "-gpu", gpu);
    }
}

// Empire Earth's menu at 16 bits per pixel: a menu drawn with Direct3D into
// each buffer of a flip chain, then per frame: restore, on the buffer drawn to,
// the background saved on that buffer two frames before (one save surface per
// buffer), save the background at the new position, draw the cursor
// colour-keyed, Flip. Flip exchanges the buffers on the GPU, the blits never
// read the GPU back, and every frame shows the menu and one cursor.
static const int CURSOR_FRAMES = 10;
static void cursor_position(int frame, int *x, int *y) {
    // Overlapping steps, then a rest.
    const int step = std::min(frame, 6);
    *x = 10 + 4 * step;
    *y = 10 + 3 * step;
}
static std::vector<Shown> cursor_frames(bool soft) {
    cpu_reset();
    d7::use_soft_backend(soft);
    g_shown.clear();
    const uint32_t dd = create_dd();
    CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, 0x513}), 0);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {96, 64, 16, 0, 0}), 0);
    SurfaceSpec ps;
    ps.caps = d7::DDSCAPS_PRIMARYSURFACE | d7::DDSCAPS_FLIP | d7::DDSCAPS_COMPLEX |
              d7::DDSCAPS_3DDEVICE | d7::DDSCAPS_VIDEOMEMORY | d7::DDSCAPS_LOCALVIDMEM;
    ps.back_buffers = 1;
    const uint32_t front = create_surface(dd, ps);
    const uint32_t back = attached(front, d7::DDSCAPS_BACKBUFFER);
    const uint32_t d3d = query(dd, d7::IID_IDirect3D7);
    const uint32_t dev = create_device(d3d, back);
    // The "menu": a gradient drawn with Direct3D into each buffer in turn.
    viewport(dev, 0, 0, 96, 64);
    set_transform(dev, d7::TS_PROJECTION, ortho2d(96, 64));
    rs(dev, d7::RS_LIGHTING, 0);
    rs(dev, d7::RS_CULLMODE, d7::CULL_NONE);
    tss(dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    for (int i = 0; i < 2; ++i) {
        call_method(dev, DEV_BeginScene, {});
        clear(dev, d7::CLEAR_TARGET, 0xff102040u);
        draw(dev, d7::PT_TRIANGLESTRIP,
             {{8, 8, 0.5f, 0, 0, -1, 0xffff0000u, 0, 0},
              {88, 8, 0.5f, 0, 0, -1, 0xff00ff00u, 0, 0},
              {8, 56, 0.5f, 0, 0, -1, 0xff0000ffu, 0, 0},
              {88, 56, 0.5f, 0, 0, -1, 0xffffffffu, 0, 0}});
        call_method(dev, DEV_EndScene, {});
        CHECK_EQ(call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT}), 0);
    }
    // The cursor (yellow, keyed on magenta) and a save surface per buffer.
    const uint32_t cursor = offscreen(dd, 8, 8, PF_R5G6B5, false);
    write(cursor, [](int x, int y) { return x + y < 8 ? 0xffe0u : 0xf81fu; });
    CHECK_EQ(set_src_key(cursor, 0xf81fu), 0);
    const uint32_t saves[2] = {offscreen(dd, 8, 8, PF_R5G6B5, false),
                               offscreen(dd, 8, 8, PF_R5G6B5, false)};
    HostD7Stats before{}, after{};
    host_d7_stats(&before);
    int saved_x[2] = {-1, -1}, saved_y[2] = {-1, -1};
    const uint32_t r8 = rect(0, 0, 8, 8);
    for (int frame = 0; frame < CURSOR_FRAMES; ++frame) {
        const int buffer = frame & 1;
        call_method(dev, DEV_BeginScene, {});
        if (saved_x[buffer] >= 0) {
            const uint32_t d =
                rect(saved_x[buffer], saved_y[buffer], saved_x[buffer] + 8, saved_y[buffer] + 8);
            CHECK_EQ(call_method(back, S_Blt, {d, saves[buffer], r8, d7::DDBLT_DONOTWAIT, 0}), 0);
            heap_free(d);
        }
        int px = 0, py = 0;
        cursor_position(frame, &px, &py);
        const uint32_t d = rect(px, py, px + 8, py + 8);
        CHECK_EQ(call_method(saves[buffer], S_Blt, {r8, back, d, d7::DDBLT_DONOTWAIT, 0}), 0);
        CHECK_EQ(
            call_method(back, S_Blt, {d, cursor, r8, d7::DDBLT_DONOTWAIT | d7::DDBLT_KEYSRC, 0}),
            0);
        heap_free(d);
        saved_x[buffer] = px;
        saved_y[buffer] = py;
        call_method(dev, DEV_EndScene, {});
        if (frame == CURSOR_FRAMES - 1 && !soft)
            host_d7_stats(&after);
        CHECK_EQ(call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT}), 0);
    }
    if (!soft) {
        // The blits stayed on the GPU: nothing was read back but by the
        // presents of this test's host, which shows a frame by reading it.
        CHECK(after.blits - before.blits >= uint64_t(3 * CURSOR_FRAMES - 2));
        CHECK_EQ(after.readbacks - before.readbacks, uint64_t(CURSOR_FRAMES - 1));
    }
    heap_free(r8);
    for (uint32_t o : {saves[0], saves[1], cursor, dev, d3d, back, front, dd})
        call_method(o, Release, {});
    d7::use_soft_backend(false);
    return g_shown;
}
static void test_cursor_flips() {
    const std::vector<Shown> ref = cursor_frames(true);
    const std::vector<Shown> gpu = cursor_frames(false);
    const size_t frames = 2 + CURSOR_FRAMES; // the menu twice, then the cursor
    CHECK_EQ(ref.size(), frames);
    CHECK_EQ(gpu.size(), frames);
    for (size_t i = 0; i < std::min(ref.size(), gpu.size()); ++i) {
        CHECK_EQ(ref[i].w, gpu[i].w);
        size_t differ = 0;
        int worst = 0;
        for (size_t k = 0; k < std::min(ref[i].argb.size(), gpu[i].argb.size()); ++k) {
            const int d = max_diff(ref[i].argb[k], gpu[i].argb[k]);
            worst = std::max(worst, d);
            differ += d > 3;
        }
        CHECK(differ == 0);
        if (differ)
            fprintf(stderr, "  frame %zu: %zu pixels differ, worst %d\n", i, differ, worst);
    }
    // Every cursor frame shows the menu and one cursor: outside the cursor's
    // own 8x8 square each pixel is the menu's (the first frame, before any
    // cursor), and no yellow is left anywhere else.
    if (gpu.size() != frames)
        return;
    const Shown &menu = gpu[1];
    for (int f = 0; f < CURSOR_FRAMES; ++f) {
        const Shown &shown = gpu[size_t(2 + f)];
        int cx = 0, cy = 0;
        cursor_position(f, &cx, &cy);
        CHECK_EQ(shown.argb[size_t(cy + 1) * 96 + size_t(cx + 1)] & 0xffffff, 0xffff00);
        size_t trail = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 96; ++x) {
                if (x >= cx && x < cx + 8 && y >= cy && y < cy + 8)
                    continue;
                trail += max_diff(shown.argb[size_t(y) * 96 + size_t(x)],
                                  menu.argb[size_t(y) * 96 + size_t(x)]) > 3;
            }
        CHECK_EQ(trail, size_t(0));
        if (trail)
            fprintf(stderr, "  frame %d: %zu pixels of trail\n", f, trail);
    }
}

// The cost of a frame at an engine's call rate: about 850 COM calls, 76 VB
// locks, 76 draws (1,800 triangles), 4 blits, 2 clears, one Flip.
static void benchmark_frame() {
    cpu_reset();
    d7::use_soft_backend(false);
    const uint32_t dd = create_dd();
    CHECK_EQ(call_method(dd, DD_SetCooperativeLevel, {0x1000, 0x513}), 0);
    CHECK_EQ(call_method(dd, DD_SetDisplayMode, {1024, 768, 32, 0, 0}), 0);
    SurfaceSpec ps;
    ps.caps = d7::DDSCAPS_PRIMARYSURFACE | d7::DDSCAPS_FLIP | d7::DDSCAPS_COMPLEX |
              d7::DDSCAPS_3DDEVICE | d7::DDSCAPS_VIDEOMEMORY;
    ps.back_buffers = 1;
    const uint32_t front = create_surface(dd, ps);
    const uint32_t back = attached(front, d7::DDSCAPS_BACKBUFFER);
    const uint32_t z = zbuffer(dd, 1024, 768);
    call_method(back, S_AddAttachedSurface, {z});
    const uint32_t d3d = query(dd, d7::IID_IDirect3D7);
    const uint32_t dev = create_device(d3d, back);
    std::vector<uint32_t> textures;
    for (int i = 0; i < 8; ++i) {
        const uint32_t t = texture(dd, 128, 128, PF_A8R8G8B8, 8);
        write(t, [i](int x, int y) {
            return 0xff000000u | uint32_t(x * 2 + i) << 16 | uint32_t(y * 2) << 8;
        });
        textures.push_back(t);
    }
    const uint32_t vb = vertex_buffer(d3d, {}, 2442);
    const uint32_t out = gmem(4), ib = gmem(240 * 2);
    for (uint16_t i = 0; i < 240; ++i)
        wr16(ib + 2u * i, uint16_t(i % 40));
    const uint32_t cursor = offscreen(dd, 32, 32, PF_X8R8G8B8, false);
    const uint32_t save = offscreen(dd, 32, 32, PF_X8R8G8B8, false);
    set_src_key(cursor, 0);
    const uint32_t r32 = rect(0, 0, 32, 32), at = rect(500, 400, 532, 432);
    std::vector<Vertex> verts(40);
    for (int i = 0; i < 40; ++i)
        verts[size_t(i)] = {
            float(i % 8) * 10.0f, float(i / 8) * 10.0f, 5.0f + float(i % 3), 0, 0, -1,
            0xffffffffu,          float(i % 8) / 8,     float(i / 8) / 5};
    set_transform(dev, d7::TS_PROJECTION, perspective(1.0f, 1.333f, 1, 1000));
    Light l;
    set_light(dev, 0, l);
    const float white[4] = {1, 1, 1, 1};
    const int frames = 120;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t cursor_vb = 0;
    for (int frame = 0; frame < frames; ++frame) {
        call_method(dev, DEV_Clear, {0, 0, d7::CLEAR_ZBUFFER, 0, fbits(1), 0});
        call_method(dev, DEV_BeginScene, {});
        for (int k = 0; k < 76; ++k) {
            // State churn: 3 render states, 2-3 stage states, 2 textures per draw.
            rs(dev, d7::RS_CLIPPING, k & 1);
            rs(dev, d7::RS_AMBIENT, 0xff000000u | uint32_t(k));
            rs(dev, d7::RS_LIGHTING, k == 10);
            tss(dev, 1, d7::TSS_COLOROP, d7::TOP_DISABLE);
            tss(dev, 1, d7::TSS_ALPHAOP, d7::TOP_DISABLE);
            call_method(dev, DEV_SetTexture, {0, textures[size_t(k) % textures.size()]});
            call_method(dev, DEV_SetTexture, {1, 0});
            if (k % 2 == 0)
                set_transform(dev, d7::TS_WORLD, translate(float(k % 7), 0, float(k % 5)));
            if (k % 3 == 0)
                material(dev, white, white);
            // The VB: NOOVERWRITE lock, copy, unlock, draw from the cursor.
            const uint32_t start = (cursor_vb + 40 > 2442) ? 0 : cursor_vb;
            call_method(vb, VB_Lock, {start ? 0x1020u : 0x2020u, out, 0});
            memcpy(gm_ptr(rd32(out) + start * 36), verts.data(), 40 * 36);
            call_method(vb, VB_Unlock, {});
            if (k < 40)
                call_method(dev, DEV_DrawPrimitiveVB, {d7::PT_TRIANGLELIST, vb, start, 33, 0});
            else
                call_method(dev, DEV_DrawIndexedPrimitiveVB,
                            {d7::PT_TRIANGLELIST, vb, start, 40, ib, 120, 0});
            cursor_vb = start + 40;
        }
        call_method(save, S_Blt, {r32, back, at, d7::DDBLT_DONOTWAIT, 0});
        call_method(back, S_Blt, {at, cursor, r32, d7::DDBLT_DONOTWAIT | d7::DDBLT_KEYSRC, 0});
        call_method(back, S_Blt, {at, save, r32, d7::DDBLT_DONOTWAIT, 0});
        call_method(dev, DEV_EndScene, {});
        call_method(front, S_Flip, {0, d7::DDFLIP_DONOTWAIT});
    }
    const double cpu_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() /
        frames;
    g_renderer->finish();
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() /
        frames;
    const HostD7Stats st = g_renderer->stats();
    printf("  frame benchmark: %.3f ms/frame submitted (guest calls + encoding), %.3f ms/frame "
           "with GPU completion; %llu draws, %llu passes, %llu pipelines in total\n",
           cpu_ms, total_ms, (unsigned long long)st.draws, (unsigned long long)st.passes,
           (unsigned long long)st.pipelines);
    CHECK(cpu_ms < 50);
    for (uint32_t t : textures)
        call_method(t, Release, {});
    for (uint32_t o : {save, cursor, vb, dev, d3d, z, back, front, dd})
        call_method(o, Release, {});
    for (uint32_t a : {out, ib, r32, at})
        heap_free(a);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto device = gpu::create_default_device();
    if (!device) {
        printf("no GPU device: the Direct3D 7 GPU tests did not run\n");
        return 2;
    }
    D3D7Renderer renderer(device.get());
    if (!renderer.ok()) {
        fprintf(stderr, "FAIL: the %s backend has no Direct3D 7 programs\n",
                gpu::default_backend_name());
        return 1;
    }
    g_renderer = &renderer;
    D3D7Renderer::setShared(&renderer);
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"scenes, GPU against the reference", test_scenes},
        {"cursor blits and Flip", test_cursor_flips},
        {"frame benchmark", benchmark_frame},
    };
    for (auto &t : tests) {
        const int before = g_failures;
        printf("%s\n", t.name);
        t.fn();
        printf("%-34s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    renderer.finish();
    D3D7Renderer::setShared(nullptr);
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
