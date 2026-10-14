// d3d7_scenes.h - synthetic Direct3D 7 / DirectDraw 7 scenes, each driven
// through the guest ABI and read back through Lock.
//
// The headless suite (d3d7_tests.cpp) renders them with the software
// reference renderer and checks hand-computed pixels; the GPU suite
// (host/tests/d3d7_gpu_tests.cpp) renders them on both backends and compares.
// Each scene states the features it covers.
#pragma once
#include "d3d7_client.h"

namespace d7t {

struct Scene {
    const char *name;
    Image (*run)();
    // Pixels whose largest channel difference may exceed the tolerance (edge
    // rasterization, filtering, float vs double), as a fraction of the image.
    double edge_budget;
    int tolerance;
};

// Coverage of a 2D quad at integer coordinates: Direct3D 7 pixel centres are
// integers, so the quad (8,8)-(24,24) covers exactly pixels 8..23.
inline Image scene_quad_coverage() {
    Device d = make_device(32, 32);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000040u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(8, 8, 24, 24, 0xffff8000u), QUAD_INDICES);
    // A thin sliver ending between pixel centres, and one exactly on them.
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(26.5f, 4, 28.5f, 6, 0xff00ff00u), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Gouraud interpolation of vertex colours, 2D projection, lighting off.
inline Image scene_gouraud() {
    Device d = make_device(64, 64);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff202020u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    draw(d.dev, d7::PT_TRIANGLELIST,
         {{4, 4, 0.5f, 0, 0, -1, 0xffff0000u, 0, 0},
          {60, 10, 0.5f, 0, 0, -1, 0xff00ff00u, 0, 0},
          {20, 58, 0.5f, 0, 0, -1, 0xff0000ffu, 0, 0}});
    // Flat shading takes the first vertex's colour.
    rs(d.dev, d7::RS_SHADEMODE, d7::SHADE_FLAT);
    draw(d.dev, d7::PT_TRIANGLELIST,
         {{40, 40, 0.5f, 0, 0, -1, 0xffffff00u, 0, 0},
          {62, 44, 0.5f, 0, 0, -1, 0xff00ffffu, 0, 0},
          {50, 62, 0.5f, 0, 0, -1, 0xffff00ffu, 0, 0}});
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// A 4x4 checker texture, point sampled 1:4 onto a 16x16 quad: each texel
// becomes exactly a 4x4 block, with no half-texel shift.
inline Image scene_texture_point() {
    Device d = make_device(32, 32);
    const uint32_t tex = texture(d.dd, 4, 4, PF_A8R8G8B8);
    write(tex, [](int x, int y) {
        return ((x + y) & 1) ? 0xffffffffu : 0xff800000u | uint32_t(x * 60) << 8 | uint32_t(y * 60);
    });
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    tss(d.dev, 0, d7::TSS_MAGFILTER, d7::TFG_POINT);
    tss(d.dev, 0, d7::TSS_MINFILTER, d7::TFN_POINT);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 16, 16, 0xffffffffu), QUAD_INDICES);
    // The same texture bilinear-filtered and stretched 1:4 beside it.
    tss(d.dev, 0, d7::TSS_MAGFILTER, d7::TFG_LINEAR);
    tss(d.dev, 0, d7::TSS_ADDRESS, d7::TADDRESS_CLAMP);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(16, 16, 32, 32, 0xffffffffu), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Directional and ambient lighting from the material, in perspective: a quad
// facing the camera (N.L = 1) and one turned 60 degrees (N.L = 0.5).
inline Image scene_light_directional() {
    Device d = make_device(64, 32);
    set_transform(d.dev, d7::TS_PROJECTION, perspective(1.0f, 2.0f, 1.0f, 100.0f));
    rs(d.dev, d7::RS_LIGHTING, 1);
    rs(d.dev, d7::RS_COLORVERTEX, 0);
    rs(d.dev, d7::RS_NORMALIZENORMALS, 1);
    rs(d.dev, d7::RS_AMBIENT, 0xff404040u);
    const float diffuse[4] = {0.5f, 0.25f, 1.0f, 1.0f}, ambient[4] = {1.0f, 1.0f, 0.0f, 1.0f};
    material(d.dev, diffuse, ambient);
    Light l;
    l.diffuse[0] = 1.0f;
    l.diffuse[1] = 0.8f;
    l.diffuse[2] = 0.5f;
    l.direction[0] = 0;
    l.direction[1] = 0;
    l.direction[2] = 1; // into the screen: lights faces looking at the camera
    set_light(d.dev, 0, l);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    // Left quad, normal (0, 0, -1).
    draw(d.dev, d7::PT_TRIANGLELIST,
         {{-3.5f, 1.5f, 5, 0, 0, -1, 0xffffffffu, 0, 0},
          {-0.5f, 1.5f, 5, 0, 0, -1, 0xffffffffu, 0, 0},
          {-3.5f, -1.5f, 5, 0, 0, -1, 0xffffffffu, 0, 0},
          {-0.5f, -1.5f, 5, 0, 0, -1, 0xffffffffu, 0, 0}},
         QUAD_INDICES);
    // Right quad, the same plane but its normal turned 60 degrees about y, and
    // unnormalized (x2) to exercise NORMALIZENORMALS.
    const float nx = 2 * std::sin(1.0471976f), nz = -2 * std::cos(1.0471976f);
    draw(d.dev, d7::PT_TRIANGLELIST,
         {{0.5f, 1.5f, 5, nx, 0, nz, 0xffffffffu, 0, 0},
          {3.5f, 1.5f, 5, nx, 0, nz, 0xffffffffu, 0, 0},
          {0.5f, -1.5f, 5, nx, 0, nz, 0xffffffffu, 0, 0},
          {3.5f, -1.5f, 5, nx, 0, nz, 0xffffffffu, 0, 0}},
         QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Material sources from the vertex colour (COLORVERTEX with the diffuse and
// ambient sources on COLOR1), a point light with range and attenuation, and a
// second, disabled light that must not contribute.
inline Image scene_light_point() {
    Device d = make_device(64, 64);
    set_transform(d.dev, d7::TS_PROJECTION, perspective(1.2f, 1.0f, 1.0f, 100.0f));
    rs(d.dev, d7::RS_LIGHTING, 1);
    rs(d.dev, d7::RS_COLORVERTEX, 1);
    rs(d.dev, d7::RS_DIFFUSEMATERIALSOURCE, d7::MCS_COLOR1);
    rs(d.dev, d7::RS_AMBIENTMATERIALSOURCE, d7::MCS_COLOR1);
    rs(d.dev, d7::RS_NORMALIZENORMALS, 1);
    rs(d.dev, d7::RS_AMBIENT, 0xff202020u);
    const float white[4] = {1, 1, 1, 1};
    material(d.dev, white, white);
    Light p;
    p.type = d7::LIGHT_POINT;
    p.position[0] = 1;
    p.position[1] = 1;
    p.position[2] = 3;
    p.range = 6;
    p.att0 = 0.5f;
    p.att1 = 0.25f;
    p.att2 = 0.0f;
    p.ambient[0] = 0.1f;
    set_light(d.dev, 0, p);
    Light off;
    set_light(d.dev, 3, off);
    call_method(d.dev, DEV_LightEnable, {3, 0});
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    // A grid of quads so the per-vertex lighting is sampled finely.
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    const int n = 8;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float x = -3 + 6.0f * i / n, y = 3 - 6.0f * j / n;
            const uint32_t c = 0xff000000u | uint32_t(128 + 15 * i) << 16 | uint32_t(200) << 8 |
                               uint32_t(255 - 20 * j);
            v.push_back({x, y, 6, 0, 0, -1, c, 0, 0});
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const uint16_t a = uint16_t(j * (n + 1) + i);
            idx.insert(idx.end(), {a, uint16_t(a + 1), uint16_t(a + n + 1), uint16_t(a + n + 1),
                                   uint16_t(a + 1), uint16_t(a + n + 2)});
        }
    draw(d.dev, d7::PT_TRIANGLELIST, v, idx);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Linear vertex fog on a floor receding from the camera.
inline Image scene_fog() {
    Device d = make_device(64, 64);
    set_transform(d.dev, d7::TS_PROJECTION, perspective(1.2f, 1.0f, 1.0f, 100.0f));
    rs(d.dev, d7::RS_FOGENABLE, 1);
    rs(d.dev, d7::RS_FOGVERTEXMODE, d7::FOG_LINEAR);
    rs(d.dev, d7::RS_FOGCOLOR, 0xffff0000u);
    rs(d.dev, d7::RS_FOGSTART, fbits(4.0f));
    rs(d.dev, d7::RS_FOGEND, fbits(20.0f));
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    for (int j = 0; j <= 10; ++j) {
        const float z = 2 + 3.0f * j;
        v.push_back({-6, -2, z, 0, 1, 0, 0xff00ff80u, 0, 0});
        v.push_back({6, -2, z, 0, 1, 0, 0xff00ff80u, 0, 0});
    }
    for (uint16_t j = 0; j < 10; ++j)
        idx.insert(idx.end(), {uint16_t(2 * j), uint16_t(2 * j + 2), uint16_t(2 * j + 1),
                               uint16_t(2 * j + 1), uint16_t(2 * j + 2), uint16_t(2 * j + 3)});
    draw(d.dev, d7::PT_TRIANGLELIST, v, idx);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// A full mip chain, one colour per level, drawn at 1:1, 1:2 and 1:4 with
// point mip selection (exact levels 0, 1 and 2), and once trilinear.
const uint32_t MIP_COLOURS[7] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u,
                                 0xff00ffffu, 0xffff00ffu, 0xffffffffu};
inline Image scene_mips() {
    Device d = make_device(128, 64);
    const uint32_t tex = texture(d.dd, 64, 64, PF_A8R8G8B8, 7);
    uint32_t level = tex;
    for (int i = 0; i < 7; ++i) {
        write(level, [i](int, int) { return MIP_COLOURS[i]; });
        uint32_t next = attached(level, d7::DDSCAPS_TEXTURE | d7::DDSCAPS_MIPMAP);
        if (level != tex)
            call_method(level, Release, {});
        level = next;
        if (!level)
            break;
    }
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    tss(d.dev, 0, d7::TSS_MINFILTER, d7::TFN_POINT);
    tss(d.dev, 0, d7::TSS_MAGFILTER, d7::TFG_POINT);
    tss(d.dev, 0, d7::TSS_MIPFILTER, d7::TFP_POINT);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 64, 64, 0xffffffffu), QUAD_INDICES);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(64, 0, 96, 32, 0xffffffffu), QUAD_INDICES);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(96, 0, 112, 16, 0xffffffffu), QUAD_INDICES);
    // MAXMIPLEVEL 3 clamps the 1:4 quad to level 3.
    tss(d.dev, 0, d7::TSS_MAXMIPLEVEL, 3);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(112, 0, 128, 16, 0xffffffffu), QUAD_INDICES);
    tss(d.dev, 0, d7::TSS_MAXMIPLEVEL, 0);
    // Trilinear at 1:1.5 (lambda ~ 0.585) and a positive LOD bias of 1 at 1:1.
    tss(d.dev, 0, d7::TSS_MIPFILTER, d7::TFP_LINEAR);
    tss(d.dev, 0, d7::TSS_MINFILTER, d7::TFN_LINEAR);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(64, 32, 64 + 42.6667f, 64, 0xffffffffu, 0, 0, 1, 0.75f),
         QUAD_INDICES);
    tss(d.dev, 0, d7::TSS_MIPFILTER, d7::TFP_POINT);
    tss(d.dev, 0, d7::TSS_MINFILTER, d7::TFN_POINT);
    tss(d.dev, 0, d7::TSS_MIPMAPLODBIAS, fbits(1.0f));
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(112, 32, 128, 48, 0xffffffffu, 0, 0, 0.25f, 0.25f),
         QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// The stage-0 combiner: SELECTARG1, SELECTARG2, MODULATE of texture and
// diffuse, MODULATE with CURRENT, separate alpha operations, and DISABLE.
inline Image scene_combiner() {
    Device d = make_device(64, 16);
    const uint32_t tex = texture(d.dd, 2, 2, PF_A8R8G8B8);
    write(tex, [](int, int) { return 0x80c06020u; }); // a=0x80 r=0xc0 g=0x60 b=0x20
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    const uint32_t diffuse = 0x40808080u;
    struct Op {
        uint32_t cop, ca1, ca2, aop, aa1, aa2;
    } const ops[] = {
        {d7::TOP_SELECTARG1, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_SELECTARG1, d7::TA_TEXTURE,
         d7::TA_DIFFUSE},
        {d7::TOP_SELECTARG2, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_SELECTARG2, d7::TA_TEXTURE,
         d7::TA_DIFFUSE},
        {d7::TOP_MODULATE, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_MODULATE, d7::TA_TEXTURE,
         d7::TA_DIFFUSE},
        {d7::TOP_MODULATE, d7::TA_TEXTURE, d7::TA_CURRENT, d7::TOP_SELECTARG1, d7::TA_TEXTURE,
         d7::TA_CURRENT},
        {d7::TOP_ADD, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_SELECTARG2, d7::TA_TEXTURE,
         d7::TA_DIFFUSE},
        {d7::TOP_MODULATE2X, d7::TA_TEXTURE, d7::TA_DIFFUSE | d7::TA_COMPLEMENT, d7::TOP_MODULATE,
         d7::TA_TEXTURE, d7::TA_DIFFUSE},
        {d7::TOP_BLENDTEXTUREALPHA, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_SELECTARG1,
         d7::TA_TEXTURE | d7::TA_ALPHAREPLICATE, d7::TA_DIFFUSE},
        {d7::TOP_DISABLE, d7::TA_TEXTURE, d7::TA_DIFFUSE, d7::TOP_DISABLE, d7::TA_TEXTURE,
         d7::TA_DIFFUSE},
    };
    for (int i = 0; i < 8; ++i) {
        tss(d.dev, 0, d7::TSS_COLOROP, ops[i].cop);
        tss(d.dev, 0, d7::TSS_COLORARG1, ops[i].ca1);
        tss(d.dev, 0, d7::TSS_COLORARG2, ops[i].ca2);
        tss(d.dev, 0, d7::TSS_ALPHAOP, ops[i].aop);
        tss(d.dev, 0, d7::TSS_ALPHAARG1, ops[i].aa1);
        tss(d.dev, 0, d7::TSS_ALPHAARG2, ops[i].aa2);
        draw(d.dev, d7::PT_TRIANGLELIST, quad2d(float(8 * i), 0, float(8 * i + 8), 16, diffuse),
             QUAD_INDICES);
    }
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}
// The same, onto an A8R8G8B8 target so the alpha the combiner made is kept.
inline Image scene_combiner_alpha() {
    Device d = make_device(64, 16, PF_A8R8G8B8);
    const uint32_t tex = texture(d.dd, 2, 2, PF_A8R8G8B8);
    write(tex, [](int, int) { return 0x80c06020u; });
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0x00000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    const uint32_t alpha_ops[4][3] = {{d7::TOP_SELECTARG1, d7::TA_TEXTURE, d7::TA_DIFFUSE},
                                      {d7::TOP_SELECTARG2, d7::TA_TEXTURE, d7::TA_DIFFUSE},
                                      {d7::TOP_MODULATE, d7::TA_TEXTURE, d7::TA_DIFFUSE},
                                      {d7::TOP_ADD, d7::TA_TEXTURE, d7::TA_DIFFUSE}};
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    for (int i = 0; i < 4; ++i) {
        tss(d.dev, 0, d7::TSS_ALPHAOP, alpha_ops[i][0]);
        tss(d.dev, 0, d7::TSS_ALPHAARG1, alpha_ops[i][1]);
        tss(d.dev, 0, d7::TSS_ALPHAARG2, alpha_ops[i][2]);
        draw(d.dev, d7::PT_TRIANGLELIST,
             quad2d(float(16 * i), 0, float(16 * i + 16), 16, 0x40808080u), QUAD_INDICES);
    }
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Alpha test and alpha blending: a texture whose alpha ramps across, tested
// GREATER 0x80, then blended SRCALPHA / INVSRCALPHA over stripes.
inline Image scene_alpha() {
    Device d = make_device(64, 32);
    const uint32_t tex = texture(d.dd, 16, 1, PF_A8R8G8B8);
    write(tex, [](int x, int) { return uint32_t(x * 17) << 24 | 0x00ffff00u; });
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000080u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    tss(d.dev, 0, d7::TSS_ALPHAOP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_ALPHAARG1, d7::TA_TEXTURE);
    rs(d.dev, d7::RS_ALPHATESTENABLE, 1);
    rs(d.dev, d7::RS_ALPHAFUNC, d7::CMP_GREATER);
    rs(d.dev, d7::RS_ALPHAREF, 0x80);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 64, 16, 0xffffffffu), QUAD_INDICES);
    rs(d.dev, d7::RS_ALPHATESTENABLE, 0);
    rs(d.dev, d7::RS_ALPHABLENDENABLE, 1);
    rs(d.dev, d7::RS_SRCBLEND, d7::BLEND_SRCALPHA);
    rs(d.dev, d7::RS_DESTBLEND, d7::BLEND_INVSRCALPHA);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 16, 64, 24, 0xffffffffu), QUAD_INDICES);
    // Additive (ONE, ONE) and BOTHSRCALPHA.
    rs(d.dev, d7::RS_SRCBLEND, d7::BLEND_ONE);
    rs(d.dev, d7::RS_DESTBLEND, d7::BLEND_ONE);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 24, 32, 32, 0xffffffffu), QUAD_INDICES);
    rs(d.dev, d7::RS_SRCBLEND, d7::BLEND_BOTHSRCALPHA);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(32, 24, 64, 32, 0xffffffffu), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Depth: compare functions, write enable, and viewports whose MinZ = MaxZ
// force a draw's depth to 0 or 1.
inline Image scene_depth() {
    Device d = make_device(64, 32);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u, 1.0f);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    rs(d.dev, d7::RS_ZENABLE, 1);
    rs(d.dev, d7::RS_ZWRITEENABLE, 1);
    rs(d.dev, d7::RS_ZFUNC, d7::CMP_LESSEQUAL);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 40, 32, 0xffff0000u, 0, 0, 1, 1, 0.8f),
         QUAD_INDICES);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(16, 0, 48, 16, 0xff00ff00u, 0, 0, 1, 1, 0.2f),
         QUAD_INDICES);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(8, 8, 56, 24, 0xff0000ffu, 0, 0, 1, 1, 0.5f),
         QUAD_INDICES);
    // MinZ = MaxZ = 1: depth 1 everywhere, passes LESSEQUAL only on cleared z.
    viewport(d.dev, 0, 0, 64, 32, 1.0f, 1.0f);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 24, 64, 32, 0xffffff00u, 0, 0, 1, 1, 0.0f),
         QUAD_INDICES);
    // MinZ = MaxZ = 0: depth 0, passes everywhere and writes 0.
    viewport(d.dev, 0, 0, 64, 32, 0.0f, 0.0f);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(56, 0, 64, 32, 0xffff00ffu, 0, 0, 1, 1, 0.9f),
         QUAD_INDICES);
    viewport(d.dev, 0, 0, 64, 32);
    // Nothing passes LESS against the zeroes just written.
    rs(d.dev, d7::RS_ZFUNC, d7::CMP_LESS);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(52, 0, 64, 32, 0xff00ffffu, 0, 0, 1, 1, 0.0f),
         QUAD_INDICES);
    // Depth test on, write off: a far quad behind red does not update depth.
    rs(d.dev, d7::RS_ZFUNC, d7::CMP_ALWAYS);
    rs(d.dev, d7::RS_ZWRITEENABLE, 0);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 16, 8, 24, 0xff808080u, 0, 0, 1, 1, 0.95f),
         QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Culling: a clockwise and a counter-clockwise triangle under each cull mode.
inline Image scene_cull() {
    Device d = make_device(48, 16);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    const uint32_t modes[3] = {d7::CULL_NONE, d7::CULL_CW, d7::CULL_CCW};
    for (int m = 0; m < 3; ++m) {
        rs(d.dev, d7::RS_CULLMODE, modes[m]);
        const float x = 16.0f * m;
        // Clockwise on screen.
        draw(d.dev, d7::PT_TRIANGLELIST,
             {{x + 1, 1, 0.5f, 0, 0, -1, 0xffff0000u, 0, 0},
              {x + 7, 1, 0.5f, 0, 0, -1, 0xffff0000u, 0, 0},
              {x + 1, 15, 0.5f, 0, 0, -1, 0xffff0000u, 0, 0}});
        // Counter-clockwise on screen.
        draw(d.dev, d7::PT_TRIANGLELIST,
             {{x + 9, 1, 0.5f, 0, 0, -1, 0xff00ff00u, 0, 0},
              {x + 9, 15, 0.5f, 0, 0, -1, 0xff00ff00u, 0, 0},
              {x + 15, 1, 0.5f, 0, 0, -1, 0xff00ff00u, 0, 0}});
    }
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Colour-keyed texturing (COLORKEYENABLE) from an A4R4G4B4 texture, and a
// texture rewritten through Lock between two draws in one scene.
inline Image scene_colorkey_texture() {
    Device d = make_device(32, 16);
    const uint32_t tex = texture(d.dd, 4, 4, PF_A4R4G4B4);
    write(tex, [](int x, int y) { return (x == y) ? 0xf0f0u : 0xff00u | uint32_t(x * 4 + y); });
    CHECK_EQ(set_src_key(tex, 0xf0f0u), 0);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff0000ffu);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    rs(d.dev, d7::RS_COLORKEYENABLE, 1);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 0, 16, 16, 0xffffffffu), QUAD_INDICES);
    // The game rewrites a small texture every frame: the draw after this sees it.
    write(tex,
          [](int x, int y) { return (x + y == 3) ? 0xf0f0u : 0xf0f0u ^ uint32_t(x * 16 + y); });
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(16, 0, 32, 16, 0xffffffffu), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Render to texture: a scene into a 3D-capable texture, then that texture
// sampled into the back buffer.
inline Image scene_render_to_texture() {
    Device d = make_device(32, 32);
    const uint32_t rtex = texture(d.dd, 16, 16, PF_X8R8G8B8, 0, true);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff101010u);
    CHECK_EQ(call_method(d.dev, DEV_SetRenderTarget, {rtex, 0}), 0);
    viewport(d.dev, 0, 0, 16, 16);
    set_transform(d.dev, d7::TS_PROJECTION, ortho2d(16, 16));
    call_method(d.dev, DEV_BeginScene, {});
    clear(d.dev, d7::CLEAR_TARGET, 0xff00ff00u);
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(4, 4, 12, 12, 0xffff0000u), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    CHECK_EQ(call_method(d.dev, DEV_SetRenderTarget, {d.rt, 0}), 0);
    viewport(d.dev, 0, 0, 32, 32);
    set_transform(d.dev, d7::TS_PROJECTION, ortho2d(32, 32));
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, rtex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(8, 8, 24, 24, 0xffffffffu), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {rtex});
    return img;
}

// A 16-bit 5-6-5 render target and the vertex buffer path, as an engine
// draws through one system-memory VB: DrawPrimitiveVB and
// DrawIndexedPrimitiveVB with a start vertex.
inline Image scene_vb_565() {
    Device d = make_device(32, 32, PF_R5G6B5);
    std::vector<Vertex> v = quad2d(0, 0, 16, 16, 0xffff0000u);
    for (const Vertex &q : quad2d(16, 16, 32, 32, 0xff00ff00u))
        v.push_back(q);
    for (const Vertex &q : quad2d(0, 16, 16, 32, 0xff3366ccu))
        v.push_back(q);
    const uint32_t vb = vertex_buffer(d.d3d, v, 64);
    const uint32_t ib = gmem(12);
    memcpy(gm_ptr(ib), QUAD_INDICES.data(), 12);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff808080u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    CHECK_EQ(
        call_method(d.dev, DEV_DrawIndexedPrimitiveVB, {d7::PT_TRIANGLELIST, vb, 0, 4, ib, 6, 0}),
        0);
    CHECK_EQ(
        call_method(d.dev, DEV_DrawIndexedPrimitiveVB, {d7::PT_TRIANGLELIST, vb, 4, 4, ib, 6, 0}),
        0);
    // Vertices 8..11 as a strip: 0,1,2 then 2,1,3 reversed.
    CHECK_EQ(call_method(d.dev, DEV_DrawPrimitiveVB, {d7::PT_TRIANGLESTRIP, vb, 8, 4, 0}), 0);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    heap_free(ib);
    release_all(d, {vb});
    return img;
}

// Specular highlights (SPECULARENABLE, material power, local and infinite
// viewer) and a spot light with its cone and falloff.
inline Image scene_specular_spot() {
    Device d = make_device(64, 32);
    set_transform(d.dev, d7::TS_PROJECTION, perspective(1.0f, 2.0f, 1.0f, 100.0f));
    rs(d.dev, d7::RS_LIGHTING, 1);
    rs(d.dev, d7::RS_COLORVERTEX, 0);
    rs(d.dev, d7::RS_NORMALIZENORMALS, 1);
    rs(d.dev, d7::RS_SPECULARENABLE, 1);
    rs(d.dev, d7::RS_AMBIENT, 0xff101010u);
    const float diffuse[4] = {0.3f, 0.3f, 0.6f, 1.0f}, ambient[4] = {1, 1, 1, 1},
                specular[4] = {1.0f, 0.9f, 0.5f, 1.0f}, emissive[4] = {0.05f, 0, 0, 0};
    material(d.dev, diffuse, ambient, specular, emissive, 12.0f);
    Light l;
    l.direction[0] = 0.3f;
    l.direction[1] = -0.2f;
    l.direction[2] = 1;
    l.specular[0] = l.specular[1] = l.specular[2] = 1;
    set_light(d.dev, 0, l);
    Light spot;
    spot.type = d7::LIGHT_SPOT;
    spot.position[0] = 2;
    spot.position[2] = 2;
    spot.direction[2] = 1;
    spot.diffuse[0] = 0.2f;
    spot.diffuse[1] = 1;
    spot.diffuse[2] = 0.2f;
    spot.theta = 0.3f;
    spot.phi = 0.8f;
    spot.falloff = 2;
    spot.range = 50;
    set_light(d.dev, 1, spot);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    // A bumpy grid: normals vary across it so the highlight has a shape.
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    const int n = 12;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float x = -4 + 8.0f * i / n, y = 2 - 4.0f * j / n;
            v.push_back(
                {x, y, 6, 0.4f * std::sin(x), 0.4f * std::cos(y * 2), -1, 0xffffffffu, 0, 0});
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const uint16_t a = uint16_t(j * (n + 1) + i);
            idx.insert(idx.end(), {a, uint16_t(a + 1), uint16_t(a + n + 1), uint16_t(a + n + 1),
                                   uint16_t(a + 1), uint16_t(a + n + 2)});
        }
    draw(d.dev, d7::PT_TRIANGLELIST, v, idx);
    rs(d.dev, d7::RS_LOCALVIEWER, 0);
    set_transform(d.dev, d7::TS_WORLD, translate(0, 0, 4));
    draw(d.dev, d7::PT_TRIANGLELIST, v, idx);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Table (pixel) fog: EXP, EXP2 and LINEAR on eye depth, and range-based vertex
// fog for comparison.
inline Image scene_table_fog() {
    Device d = make_device(64, 48);
    set_transform(d.dev, d7::TS_PROJECTION, perspective(1.2f, 64.0f / 48, 1.0f, 100.0f));
    rs(d.dev, d7::RS_FOGENABLE, 1);
    rs(d.dev, d7::RS_FOGCOLOR, 0xff8080ffu);
    rs(d.dev, d7::RS_FOGSTART, fbits(3.0f));
    rs(d.dev, d7::RS_FOGEND, fbits(30.0f));
    rs(d.dev, d7::RS_FOGDENSITY, fbits(0.08f));
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    const uint32_t modes[4][2] = {
        {d7::FOG_EXP, 0}, {d7::FOG_EXP2, 0}, {d7::FOG_LINEAR, 0}, {d7::FOG_NONE, d7::FOG_LINEAR}};
    for (int m = 0; m < 4; ++m) {
        rs(d.dev, d7::RS_FOGTABLEMODE, modes[m][0]);
        rs(d.dev, d7::RS_FOGVERTEXMODE, modes[m][1]);
        rs(d.dev, d7::RS_RANGEFOGENABLE, m == 3);
        const float x0 = -4 + 2.0f * m;
        std::vector<Vertex> v;
        std::vector<uint16_t> idx;
        for (int j = 0; j <= 8; ++j) {
            const float z = 2 + 4.0f * j;
            v.push_back({x0, -1.5f, z, 0, 1, 0, 0xff20c020u, 0, 0});
            v.push_back({x0 + 1.8f, -1.5f, z, 0, 1, 0, 0xff20c020u, 0, 0});
        }
        for (uint16_t j = 0; j < 8; ++j)
            idx.insert(idx.end(), {uint16_t(2 * j), uint16_t(2 * j + 2), uint16_t(2 * j + 1),
                                   uint16_t(2 * j + 1), uint16_t(2 * j + 2), uint16_t(2 * j + 3)});
        draw(d.dev, d7::PT_TRIANGLELIST, v, idx);
    }
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// Texture addressing past [0, 1]: WRAP, MIRROR, CLAMP; and TFACTOR / SPECULAR
// as combiner arguments.
inline Image scene_addressing() {
    Device d = make_device(64, 32);
    const uint32_t tex = texture(d.dd, 4, 4, PF_A1R5G5B5);
    write(tex, [](int x, int y) {
        return 0x8000u | uint32_t(x * 8) << 10 | uint32_t(y * 8) << 5 | 0x1fu;
    });
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG1);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    const uint32_t modes[3] = {d7::TADDRESS_WRAP, d7::TADDRESS_MIRROR, d7::TADDRESS_CLAMP};
    for (int m = 0; m < 3; ++m) {
        tss(d.dev, 0, d7::TSS_ADDRESSU, modes[m]);
        tss(d.dev, 0, d7::TSS_ADDRESSV, modes[(m + 1) % 3]);
        draw(d.dev, d7::PT_TRIANGLELIST,
             quad2d(float(16 * m), 0, float(16 * m + 16), 16, 0xffffffffu, -1, -1, 2, 2),
             QUAD_INDICES);
    }
    rs(d.dev, d7::RS_TEXTUREFACTOR, 0x80ff4020u);
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_MODULATE);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_TFACTOR);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(48, 0, 64, 16, 0xffffffffu), QUAD_INDICES);
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_BLENDFACTORALPHA);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(0, 16, 32, 32, 0xff0000ffu), QUAD_INDICES);
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_ADDSIGNED);
    draw(d.dev, d7::PT_TRIANGLELIST, quad2d(32, 16, 64, 32, 0xff406080u), QUAD_INDICES);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Pre-transformed vertices (XYZRHW | DIFFUSE | SPECULAR | TEX1, 32 bytes):
// screen space, perspective-correct by RHW, depth as given, vertex fog from
// the specular alpha.
inline Image scene_pretransformed() {
    Device d = make_device(64, 32);
    const uint32_t tex = texture(d.dd, 8, 8, PF_X8R8G8B8);
    write(tex, [](int x, int y) { return ((x ^ y) & 1) ? 0x00ffffffu : 0x00402010u; });
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    call_method(d.dev, DEV_SetTexture, {0, tex});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_MODULATE);
    tss(d.dev, 0, d7::TSS_COLORARG1, d7::TA_TEXTURE);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    rs(d.dev, d7::RS_FOGENABLE, 1);
    rs(d.dev, d7::RS_FOGCOLOR, 0xff00ff00u);
    struct TL {
        float x, y, z, rhw;
        uint32_t diffuse, specular;
        float u, v;
    };
    // Texture coordinates inset by a fraction of a texel, so no pixel centre
    // samples exactly on a texel boundary, where float and double may part.
    // (Chosen so every pixel centre is at least 1/700 texel from an edge.)
    const TL quad[4] = {{4, 2, 0.2f, 1.0f, 0xffffffffu, 0xff000000u, 0.013f, 0.017f},
                        {60, 6, 0.2f, 0.25f, 0xffff8080u, 0x80000000u, 0.971f, 0.017f},
                        {4, 30, 0.6f, 1.0f, 0xff8080ffu, 0xff000000u, 0.013f, 0.973f},
                        {60, 26, 0.6f, 0.25f, 0xffffffffu, 0x00000000u, 0.971f, 0.973f}};
    const uint32_t vb = gmem(sizeof quad), ib = gmem(12);
    memcpy(gm_ptr(vb), quad, sizeof quad);
    memcpy(gm_ptr(ib), QUAD_INDICES.data(), 12);
    CHECK_EQ(
        call_method(d.dev, DEV_DrawIndexedPrimitive, {d7::PT_TRIANGLELIST, 0x1c4, vb, 4, ib, 6, 0}),
        0);
    call_method(d.dev, DEV_EndScene, {});
    call_method(d.dev, DEV_SetTexture, {0, 0});
    heap_free(vb);
    heap_free(ib);
    Image img = read(d.rt);
    release_all(d, {tex});
    return img;
}

// Lines and points as an engine's grid and markers draw them: line lists and
// strips on pixel centres, and points.
inline Image scene_lines_points() {
    Device d = make_device(48, 32);
    clear(d.dev, d7::CLEAR_TARGET | d7::CLEAR_ZBUFFER, 0xff000000u);
    call_method(d.dev, DEV_BeginScene, {});
    tss(d.dev, 0, d7::TSS_COLOROP, d7::TOP_SELECTARG2);
    tss(d.dev, 0, d7::TSS_COLORARG2, d7::TA_DIFFUSE);
    std::vector<Vertex> lines;
    for (int i = 0; i < 6; ++i) {
        lines.push_back({4, 4.0f + 4 * i, 0.5f, 0, 0, -1, 0xffffffffu, 0, 0});
        lines.push_back({44, 4.0f + 4 * i, 0.5f, 0, 0, -1, 0xffffffffu, 0, 0});
        lines.push_back({4.0f + 8 * i, 2, 0.5f, 0, 0, -1, 0xffff8000u, 0, 0});
        lines.push_back({4.0f + 8 * i, 30, 0.5f, 0, 0, -1, 0xffff8000u, 0, 0});
    }
    draw(d.dev, d7::PT_LINELIST, lines);
    std::vector<Vertex> points;
    for (int i = 0; i < 10; ++i)
        points.push_back({6.0f + 4 * i, 29, 0.5f, 0, 0, -1, 0xff00ffffu, 0, 0});
    draw(d.dev, d7::PT_POINTLIST, points);
    call_method(d.dev, DEV_EndScene, {});
    Image img = read(d.rt);
    release_all(d);
    return img;
}

// A movie frame: a system-memory 5-6-5 surface written through Lock and
// stretched onto a 32-bit render target, as a video player's frame buffer is.
inline Image scene_movie_blit() {
    Device d = make_device(64, 48);
    const uint32_t frame = offscreen(d.dd, 32, 24, PF_R5G6B5, false, d7::DDSCAPS_SYSTEMMEMORY);
    clear(d.dev, d7::CLEAR_TARGET, 0xff000000u);
    for (int f = 0; f < 2; ++f) {
        write(frame, [f](int x, int y) {
            return uint32_t((x + f) & 31) << 11 | uint32_t(y * 2 + f) << 5 | uint32_t(31 - x);
        });
        const uint32_t r = rect(0, 0, 64, 48);
        CHECK_EQ(call_method(d.rt, S_Blt, {r, frame, 0, d7::DDBLT_DONOTWAIT, 0}), 0);
        heap_free(r);
    }
    Image img = read(d.rt);
    release_all(d, {frame});
    return img;
}

inline const std::vector<Scene> &scenes() {
    static const std::vector<Scene> list = {
        {"quad coverage", scene_quad_coverage, 0.0, 1},
        {"gouraud and flat", scene_gouraud, 0.01, 3},
        {"point and bilinear texture", scene_texture_point, 0.0, 3},
        {"directional light", scene_light_directional, 0.01, 2},
        {"point light, vertex colour", scene_light_point, 0.01, 3},
        {"linear vertex fog", scene_fog, 0.02, 3},
        {"mip chain", scene_mips, 0.01, 3},
        {"stage-0 combiner", scene_combiner, 0.0, 2},
        {"combiner alpha", scene_combiner_alpha, 0.0, 2},
        {"alpha test and blend", scene_alpha, 0.0, 3},
        {"depth and MinZ = MaxZ", scene_depth, 0.0, 1},
        {"culling", scene_cull, 0.02, 1},
        {"colour-keyed texture", scene_colorkey_texture, 0.0, 1},
        {"render to texture", scene_render_to_texture, 0.0, 1},
        {"vertex buffer, 5-6-5", scene_vb_565, 0.0, 1},
        {"specular and spot light", scene_specular_spot, 0.01, 3},
        {"table fog", scene_table_fog, 0.02, 3},
        {"addressing, tfactor", scene_addressing, 0.0, 2},
        {"pre-transformed vertices", scene_pretransformed, 0.005, 3},
        {"lines and points", scene_lines_points, 0.0, 1},
        {"movie frame blit", scene_movie_blit, 0.0, 0},
    };
    return list;
}

} // namespace d7t
