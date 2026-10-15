// d3d7_render.cpp - the DirectDraw 7 / Direct3D 7 renderer over gpu.h.
//
// One open command buffer and at most one open render pass at a time. A pass
// is keyed by its colour and depth targets, so DirectDraw fills and blits into
// the surface being drawn to (a software cursor between two draws) are encoded
// into the same pass instead of splitting it. Uploads and readbacks end the
// pass; everything stays in submission order on the device's one queue.
//
// Per-draw data (vertices, the expanded index list, staging for uploads) goes
// into transient chunks of shared buffers, recycled once the command buffer
// that used them has completed. Uniforms go inline (set_bytes).
//
// The vertex program implements Direct3D 7's fixed-function transform,
// lighting and fog; the fragment program the stage-0 texture combiner, alpha
// test and fog blend; blending, depth and culling are pipeline and encoder
// state. dx/d3d7_soft.cpp is the reference both are tested against.
#include "d3d7_render.h"
#include "../dx/d3d7_prim.h"
#include "../dx/d3d7_types.h"
#include "../platform/os.h"
#include "../runtime/display_seam.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Program interfaces: must match kD3D7Source (gpu/metal/shaders_msl.h).
// ---------------------------------------------------------------------------
struct alignas(16) Light7 {
    float diffuse[4], specular[4], ambient[4];
    float position[4];  // xyz eye space, w type
    float direction[4]; // xyz eye space, w range
    float atten[4];     // a0, a1, a2, falloff
    float spot[4];      // cos(theta/2), cos(phi/2)
};
struct alignas(16) VS7 {
    float wvp[16], wv[16];
    float nrm[12];
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
    float ambient[4];
    float ndc[4];
    float viewport[4];
    float fog[4];
    uint32_t flags, offsets, stride, light_count;
    uint32_t sources, fog_mode, pad0, pad1;
    Light7 lights[8];
};
static_assert(sizeof(Light7) == 112, "Light7 layout");
static_assert(sizeof(VS7) == 1232, "VS7 layout");
struct alignas(16) FS7 {
    float fog_color[4], tfactor[4], fog[4];
    uint32_t colorop, colorarg1, colorarg2, alphaop;
    uint32_t alphaarg1, alphaarg2, textured, tex_has_alpha;
    uint32_t alpha_func, alpha_ref, fog_enable, table_fog;
    uint32_t fog_w, flat, specular, colorkey;
    uint32_t key_max[4], key_lo[4], key_hi[4];
    float lod_bias, min_lod, pad0, pad1;
};
static_assert(sizeof(FS7) == 176, "FS7 layout");
struct alignas(16) Q7 {
    float dst[4];
    int32_t src[4];
    float color[4];
    float target[2];
    float depth;
    uint32_t mode;
    uint32_t level, key_enabled, src_has_alpha, mirror;
    uint32_t key_max[4], key_lo[4], key_hi[4];
};
static_assert(sizeof(Q7) == 128, "Q7 layout");

enum : uint32_t {
    F_LIGHTING = 1,
    F_NORMAL = 2,
    F_DIFFUSE = 4,
    F_SPECULAR = 8,
    F_TEX = 16,
    F_RHW = 32,
    F_SPECULARENABLE = 64,
    F_LOCALVIEWER = 128,
    F_NORMALIZE = 256,
    F_COLORVERTEX = 512,
    F_RANGEFOG = 1024,
    F_VERTEXFOG = 2048,
    F_TEX1D = 4096,
};
enum : uint32_t { Q_COPY = 0, Q_FILL = 1, Q_DEPTH = 2, Q_LINEAR = 3 };

void log_once(const char *key, const char *msg) {
    static std::mutex m;
    static std::vector<std::string> seen;
    std::lock_guard lock(m);
    for (const auto &k : seen)
        if (k == key)
            return;
    seen.emplace_back(key);
    fprintf(stderr, "[d3d7] %s\n", msg);
}

inline void unpack(uint32_t argb, float *out) {
    out[0] = float((argb >> 16) & 0xff) / 255.0f;
    out[1] = float((argb >> 8) & 0xff) / 255.0f;
    out[2] = float(argb & 0xff) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}
inline float as_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
void matmul(const float *a, const float *b, float *out) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(out, r, sizeof r);
}
// Rows of the inverse transpose of m's upper 3x3 (row-vector convention).
void normal_rows(const float *m, float *rows12) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                 i = m[10];
    const double cof[9] = {e * i - f * h,    -(d * i - f * g), d * h - e * g,
                           -(b * i - c * h), a * i - c * g,    -(a * h - b * g),
                           b * f - c * e,    -(a * f - c * d), a * e - b * d};
    double det = a * cof[0] + b * cof[1] + c * cof[2];
    if (std::fabs(det) < 1e-30)
        det = 1e-30;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k)
            rows12[r * 4 + k] = float(cof[r * 3 + k] / det);
        rows12[r * 4 + 3] = 0;
    }
}
void xform_point(const float *p, const float *m, float *out) {
    for (int j = 0; j < 3; ++j)
        out[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
}
void xform_dir(const float *v, const float *m, float *out) {
    for (int j = 0; j < 3; ++j)
        out[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j];
}
void normalize3(float *v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (int i = 0; i < 3; ++i)
            v[i] /= l;
}

gpu::Blend blend_of(uint32_t f) {
    switch (f) {
    case d7::BLEND_ZERO:
        return gpu::Blend::Zero;
    case d7::BLEND_SRCCOLOR:
        return gpu::Blend::SrcColor;
    case d7::BLEND_INVSRCCOLOR:
        return gpu::Blend::OneMinusSrcColor;
    case d7::BLEND_SRCALPHA:
        return gpu::Blend::SrcAlpha;
    case d7::BLEND_INVSRCALPHA:
        return gpu::Blend::OneMinusSrcAlpha;
    case d7::BLEND_DESTALPHA:
        return gpu::Blend::DstAlpha;
    case d7::BLEND_INVDESTALPHA:
        return gpu::Blend::OneMinusDstAlpha;
    case d7::BLEND_DESTCOLOR:
        return gpu::Blend::DstColor;
    case d7::BLEND_INVDESTCOLOR:
        return gpu::Blend::OneMinusDstColor;
    case d7::BLEND_SRCALPHASAT:
        return gpu::Blend::SrcAlphaSaturated;
    default:
        return gpu::Blend::One;
    }
}
gpu::Compare compare_of(uint32_t f) {
    switch (f) {
    case d7::CMP_NEVER:
        return gpu::Compare::Never;
    case d7::CMP_LESS:
        return gpu::Compare::Less;
    case d7::CMP_EQUAL:
        return gpu::Compare::Equal;
    case d7::CMP_LESSEQUAL:
        return gpu::Compare::LessEqual;
    case d7::CMP_GREATER:
        return gpu::Compare::Greater;
    case d7::CMP_NOTEQUAL:
        return gpu::Compare::NotEqual;
    case d7::CMP_GREATEREQUAL:
        return gpu::Compare::GreaterEqual;
    default:
        return gpu::Compare::Always;
    }
}
gpu::Address address_of(uint32_t a) {
    switch (a) {
    case d7::TADDRESS_MIRROR:
        return gpu::Address::MirrorRepeat;
    case d7::TADDRESS_CLAMP:
        return gpu::Address::ClampToEdge;
    case d7::TADDRESS_BORDER:
        return gpu::Address::ClampToBorder;
    default:
        return gpu::Address::Repeat;
    }
}

std::atomic<uint32_t> g_generation{1};

} // namespace

// ===========================================================================
// The renderer
// ===========================================================================
struct D3D7Renderer::Impl {
    gpu::Device *dev;
    uint32_t generation;
    bool ok = false;

    struct Res {
        gpu::Texture texture;
        int w = 0, h = 0, levels = 1;
        bool depth = false, opaque = false;
    };
    std::unordered_map<uint32_t, Res> res;
    gpu::Texture dummy;   // bound when a draw has no texture
    gpu::Texture scratch; // present regions and same-surface blits
    int scratch_w = 0, scratch_h = 0;

    gpu::CommandBuffer cb, last;
    struct Pass {
        bool open = false;
        uint32_t color = 0, depth = 0;
        int w = 0, h = 0;
    } pass;

    struct Chunk {
        gpu::Buffer buffer;
        uint64_t size = 0, used = 0;
        gpu::CommandBuffer fence;
    };
    std::vector<Chunk> free_chunks, retired;
    std::vector<Chunk> active; // referenced by the open command buffer; back() is current

    std::unordered_map<uint64_t, gpu::Pipeline> pipelines;
    HostD7Stats stats{};
    std::vector<uint32_t> list; // index-list scratch

    explicit Impl(gpu::Device *d) : dev(d), generation(g_generation.fetch_add(1) + 1) {
        gpu::RenderState rs;
        rs.color_format[0] = gpu::Format::BGRA8;
        ok = dev->render_pipeline("d3d7", rs) && dev->render_pipeline("d3d7_quad", rs);
        if (!ok)
            return;
        dummy = dev->create_texture({1, 1, gpu::Format::BGRA8, gpu::UsageSampled, 1});
        const uint32_t white = 0xffffffffu;
        if (dummy)
            dev->upload(dummy, {0, 0, 1, 1}, &white, 4);
    }
    ~Impl() {
        finish();
        for (auto &kv : res)
            dev->destroy(kv.second.texture);
        if (dummy)
            dev->destroy(dummy);
        if (scratch)
            dev->destroy(scratch);
        for (auto *chunks : {&free_chunks, &retired, &active})
            for (Chunk &c : *chunks)
                dev->destroy(c.buffer);
    }

    // --- the command stream ------------------------------------------------
    gpu::CommandBuffer command() {
        if (!cb)
            cb = dev->begin();
        return cb;
    }
    void end_pass() {
        if (pass.open) {
            dev->end_render_pass(cb);
            pass.open = false;
        }
    }
    void commit() {
        end_pass();
        if (!cb)
            return;
        dev->commit(cb);
        last = cb;
        for (Chunk &c : active) {
            c.fence = cb;
            retired.push_back(c);
        }
        active.clear();
        cb = {};
    }
    void finish() {
        commit();
        if (last) {
            dev->wait(last);
            last = {};
        }
    }
    // Transient bytes, valid in the open command buffer.
    struct Slice {
        gpu::Buffer buffer;
        uint64_t offset = 0;
    };
    Slice transient(const void *data, uint64_t bytes) {
        bytes = std::max<uint64_t>(bytes, 4);
        const uint64_t aligned = (bytes + 255) & ~uint64_t(255);
        if (active.empty() || active.back().used + aligned > active.back().size) {
            for (size_t i = 0; i < retired.size();) {
                if (dev->status(retired[i].fence) != gpu::CommandStatus::Pending) {
                    retired[i].used = 0;
                    retired[i].fence = {};
                    free_chunks.push_back(retired[i]);
                    retired.erase(retired.begin() + long(i));
                } else {
                    ++i;
                }
            }
            Chunk c;
            for (size_t i = 0; i < free_chunks.size(); ++i)
                if (free_chunks[i].size >= aligned) {
                    c = free_chunks[i];
                    free_chunks.erase(free_chunks.begin() + long(i));
                    break;
                }
            if (!c.buffer) {
                c.size = std::max<uint64_t>(aligned, uint64_t(4) << 20);
                c.buffer = dev->create_buffer(c.size, nullptr);
                if (!c.buffer)
                    return {};
            }
            active.push_back(c);
        }
        Chunk &c = active.back();
        Slice s{c.buffer, c.used};
        if (data)
            dev->update(c.buffer, c.used, data, bytes);
        c.used += aligned;
        return s;
    }

    Res *find(uint32_t id) {
        auto it = res.find(id);
        return it == res.end() ? nullptr : &it->second;
    }

    // A pass drawing to `color` (0: none) with `depth` (0: none). An open pass
    // with the same targets is reused.
    bool use_pass(uint32_t color, uint32_t depth) {
        if (pass.open && pass.color == color && pass.depth == depth)
            return true;
        Res *c = color ? find(color) : nullptr;
        Res *z = depth ? find(depth) : nullptr;
        if ((color && !c) || (depth && !z))
            return false;
        end_pass();
        gpu::RenderPass rp;
        rp.color_count = c ? 1 : 0;
        if (c)
            rp.color[0].texture = c->texture;
        if (z)
            rp.depth.texture = z->texture;
        dev->begin_render_pass(command(), rp);
        pass.open = true;
        pass.color = color;
        pass.depth = depth;
        pass.w = c ? c->w : z->w;
        pass.h = c ? c->h : z->h;
        ++stats.passes;
        return true;
    }
    gpu::Pipeline pipeline(bool quad, const gpu::RenderState &rs) {
        const uint64_t key = rs.key() | (quad ? uint64_t(1) << 63 : 0);
        auto it = pipelines.find(key);
        if (it != pipelines.end())
            return it->second;
        gpu::Pipeline p = dev->render_pipeline(quad ? "d3d7_quad" : "d3d7", rs);
        pipelines[key] = p;
        ++stats.pipelines;
        return p;
    }
    gpu::RenderState pass_state() const {
        gpu::RenderState rs;
        rs.color_format[0] = gpu::Format::BGRA8;
        rs.color_count = pass.color ? 1 : 0;
        rs.depth_attachment = pass.depth != 0;
        return rs;
    }

    // --- resources ---------------------------------------------------------
    bool create(uint32_t id, int w, int h, int levels, uint32_t flags) {
        destroy(id);
        Res r;
        r.w = w;
        r.h = h;
        r.depth = (flags & HOST_D7_CREATE_DEPTH) != 0;
        r.opaque = (flags & HOST_D7_CREATE_OPAQUE) != 0;
        r.levels = r.depth ? 1 : levels;
        gpu::TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = r.depth ? gpu::Format::Depth32F : gpu::Format::BGRA8;
        desc.usage = r.depth ? gpu::UsageRenderTarget : gpu::UsageSampled | gpu::UsageRenderTarget;
        desc.mip_levels = r.levels;
        r.texture = dev->create_texture(desc);
        if (!r.texture)
            return false;
        r.levels = std::min(r.levels, dev->describe(r.texture).mip_levels);
        res[id] = r;
        // Initial contents: depth 1.0, colour transparent or opaque black.
        if (r.depth) {
            end_pass();
            gpu::RenderPass rp;
            rp.color_count = 0;
            rp.depth.texture = r.texture;
            rp.depth.load = gpu::Load::Clear;
            rp.depth.clear = 1.0f;
            dev->begin_render_pass(command(), rp);
            dev->end_render_pass(cb);
        } else {
            end_pass();
            for (int level = 0; level < r.levels; ++level) {
                const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
                std::vector<uint32_t> init(size_t(lw) * size_t(lh), r.opaque ? 0xff000000u : 0u);
                const Slice s = transient(init.data(), init.size() * 4);
                if (s.buffer)
                    dev->copy_buffer_to_texture_level(command(), s.buffer, s.offset, lw * 4,
                                                      r.texture, {0, 0, lw, lh}, level);
            }
        }
        return true;
    }
    void destroy(uint32_t id) {
        auto it = res.find(id);
        if (it == res.end())
            return;
        if (pass.open && (pass.color == id || pass.depth == id))
            end_pass();
        // Encoded commands hold their own reference to the texture.
        dev->destroy(it->second.texture);
        res.erase(it);
    }
    void upload(uint32_t id, int level, const HostD7Rect &r, const void *bgra, int pitch) {
        Res *t = find(id);
        if (!t || t->depth || level < 0 || level >= t->levels)
            return;
        const int w = r.x1 - r.x0, h = r.y1 - r.y0;
        if (w <= 0 || h <= 0)
            return;
        std::vector<uint32_t> packed;
        const void *src = bgra;
        if (pitch != w * 4 || t->opaque) {
            packed.resize(size_t(w) * size_t(h));
            for (int y = 0; y < h; ++y)
                memcpy(packed.data() + size_t(y) * size_t(w),
                       static_cast<const uint8_t *>(bgra) + size_t(y) * size_t(pitch),
                       size_t(w) * 4);
            if (t->opaque)
                for (uint32_t &p : packed)
                    p |= 0xff000000u;
            src = packed.data();
        }
        end_pass();
        const Slice s = transient(src, uint64_t(w) * uint64_t(h) * 4);
        if (!s.buffer)
            return;
        dev->copy_buffer_to_texture_level(command(), s.buffer, s.offset, w * 4, t->texture,
                                          {r.x0, r.y0, w, h}, level);
        ++stats.uploads;
        stats.upload_bytes += uint64_t(w) * uint64_t(h) * 4;
    }
    bool readback(uint32_t id, int level, const HostD7Rect &r, void *out, int pitch) {
        Res *t = find(id);
        if (!t || t->depth || level < 0 || level >= t->levels)
            return false;
        const int w = r.x1 - r.x0, h = r.y1 - r.y0;
        if (w <= 0 || h <= 0)
            return false;
        ++stats.readbacks;
        if (level == 0) {
            commit();
            return dev->readback(t->texture, {r.x0, r.y0, w, h}, out, pitch);
        }
        gpu::Buffer staging = dev->create_buffer(uint64_t(w) * uint64_t(h) * 4, nullptr);
        if (!staging)
            return false;
        end_pass();
        const bool copied = dev->copy_texture_to_buffer_level(
            command(), t->texture, {r.x0, r.y0, w, h}, level, staging, 0, w * 4);
        finish();
        const uint8_t *bytes =
            copied ? static_cast<const uint8_t *>(dev->map_read(staging)) : nullptr;
        if (bytes)
            for (int y = 0; y < h; ++y)
                memcpy(static_cast<uint8_t *>(out) + size_t(y) * size_t(pitch),
                       bytes + size_t(y) * size_t(w) * 4, size_t(w) * 4);
        dev->destroy(staging);
        return bytes != nullptr;
    }

    // --- quads: fills, depth fills, blits ----------------------------------
    void quad(const Q7 &q, gpu::Texture tex, const gpu::RenderState &rs, gpu::DepthState ds) {
        gpu::Pipeline p = pipeline(true, rs);
        if (!p)
            return;
        dev->set_pipeline(cb, p);
        dev->set_depth(cb, ds);
        dev->set_cull(cb, gpu::Cull::None);
        dev->set_viewport(cb, {0, 0, double(pass.w), double(pass.h), 0, 1});
        dev->set_bytes(cb, gpu::Stage::Vertex, 0, &q, sizeof q);
        dev->set_bytes(cb, gpu::Stage::Fragment, 0, &q, sizeof q);
        dev->set_texture(cb, gpu::Stage::Fragment, 0, tex ? tex : dummy);
        gpu::SamplerState s;
        s.mag = s.min = (q.mode == Q_LINEAR) ? gpu::Filter::Linear : gpu::Filter::Nearest;
        dev->set_sampler(cb, gpu::Stage::Fragment, 0, s);
        dev->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    }
    Q7 quad_for(const HostD7Rect &r) const {
        Q7 q;
        memset(&q, 0, sizeof q);
        q.dst[0] = float(r.x0);
        q.dst[1] = float(r.y0);
        q.dst[2] = float(r.x1);
        q.dst[3] = float(r.y1);
        q.target[0] = float(pass.w);
        q.target[1] = float(pass.h);
        return q;
    }
    void fill(uint32_t id, const HostD7Rect &r, uint32_t bgra) {
        Res *t = find(id);
        if (!t || t->depth)
            return;
        if (!(pass.open && pass.color == id) && !use_pass(id, 0))
            return;
        Q7 q = quad_for(r);
        q.mode = Q_FILL;
        unpack(bgra, q.color);
        gpu::RenderState rs = pass_state();
        rs.write_alpha = !t->opaque;
        quad(q, {}, rs, {});
        ++stats.fills;
    }
    void fill_depth(uint32_t id, const HostD7Rect &r, float z) {
        Res *t = find(id);
        if (!t || !t->depth)
            return;
        if (!(pass.open && pass.depth == id) && !use_pass(0, id))
            return;
        Q7 q = quad_for(r);
        q.mode = Q_DEPTH;
        q.depth = z;
        gpu::RenderState rs = pass_state();
        rs.write_color = false;
        quad(q, {}, rs, {gpu::Compare::Always, true});
        ++stats.fills;
    }
    gpu::Texture scratch_texture(int w, int h) {
        if (!scratch || scratch_w < w || scratch_h < h) {
            if (scratch) {
                finish();
                dev->destroy(scratch);
            }
            scratch_w = std::max(w, scratch_w);
            scratch_h = std::max(h, scratch_h);
            scratch = dev->create_texture({scratch_w, scratch_h, gpu::Format::BGRA8,
                                           gpu::UsageSampled | gpu::UsageRenderTarget, 1});
        }
        return scratch;
    }
    void blit(const HostD7Blit &b) {
        Res *d = find(b.dst), *s = find(b.src);
        if (!d || !s || d->depth || s->depth || b.dst_level != 0 || b.src_level < 0 ||
            b.src_level >= s->levels)
            return;
        gpu::Texture src = s->texture;
        int src_level = b.src_level;
        HostD7Rect sr = b.src_rect;
        const int sw = sr.x1 - sr.x0, sh = sr.y1 - sr.y0;
        if (pass.open && pass.color == b.src)
            end_pass();
        if (b.src == b.dst) {
            // A surface cannot be sampled while it is the pass's target.
            gpu::Texture tmp = scratch_texture(sw, sh);
            if (!tmp || src_level != 0)
                return;
            end_pass();
            dev->blit(command(), src, {sr.x0, sr.y0, sw, sh}, tmp, 0, 0);
            src = tmp;
            sr = {0, 0, sw, sh};
        }
        if (!(pass.open && pass.color == b.dst) && !use_pass(b.dst, 0))
            return;
        Q7 q = quad_for(b.dst_rect);
        q.mode = b.linear ? Q_LINEAR : Q_COPY;
        q.src[0] = sr.x0;
        q.src[1] = sr.y0;
        q.src[2] = sr.x1;
        q.src[3] = sr.y1;
        q.level = uint32_t(src_level);
        q.src_has_alpha = b.src_has_alpha;
        q.mirror = (b.mirror_x ? 1u : 0u) | (b.mirror_y ? 2u : 0u);
        if (b.key.enabled) {
            q.key_enabled = 1;
            for (int i = 0; i < 3; ++i) {
                q.key_max[i] = (1u << b.key.bits[i]) - 1;
                q.key_lo[i] = b.key.lo[i];
                q.key_hi[i] = b.key.hi[i];
            }
        }
        gpu::RenderState rs = pass_state();
        rs.write_alpha = !d->opaque;
        quad(q, src, rs, {});
        ++stats.blits;
    }

    // --- draws -------------------------------------------------------------
    void draw(const HostD7Draw &d);
    void present(uint32_t id, const HostD7Rect &r, int mode_w, int mode_h);
};

void D3D7Renderer::Impl::draw(const HostD7Draw &d) {
    Res *rt = find(d.target);
    if (!rt || rt->depth || !d.state)
        return;
    const HostD7State &st = *d.state;
    const uint32_t *rs = st.rs;
    const d7::FvfLayout L = d7::fvf_layout(d.fvf);
    const d7::Topology topo = d7::topology_of(d.primitive);
    if (!L.valid || topo == d7::Topology::None || !d.vertex_count)
        return;
    Res *zb = d.depth ? find(d.depth) : nullptr;
    if (zb && (!zb->depth || zb->w != rt->w || zb->h != rt->h)) {
        log_once("depthsize",
                 "a depth buffer whose size differs from its render target is not used");
        zb = nullptr;
    }
    // The viewport, clipped to the target; Direct3D's NDC is remapped onto it.
    const double vx = st.viewport[0], vy = st.viewport[1], vw = st.viewport[2], vh = st.viewport[3];
    const int mx0 = std::max(0, int(st.viewport[0])), my0 = std::max(0, int(st.viewport[1]));
    const int mx1 = std::min(rt->w, int(st.viewport[0] + st.viewport[2]));
    const int my1 = std::min(rt->h, int(st.viewport[1] + st.viewport[3]));
    if (mx0 >= mx1 || my0 >= my1 || vw <= 0 || vh <= 0)
        return;
    const double mw = mx1 - mx0, mh = my1 - my0;

    // The expanded list, through the guest's indices.
    list.clear();
    d7::expand_primitive(d.primitive, d.indices ? d.index_count : d.vertex_count, list);
    if (list.empty())
        return;
    for (uint32_t &i : list) {
        if (d.indices)
            i = d.indices[i];
        if (i >= d.vertex_count)
            i = d.vertex_count - 1;
    }

    if (!use_pass(d.target, zb ? d.depth : 0))
        return;
    const Slice vb = transient(d.vertices, uint64_t(d.vertex_count) * d.stride);
    const Slice ib = transient(list.data(), list.size() * 4);
    if (!vb.buffer || !ib.buffer)
        return;

    VS7 v;
    memset(&v, 0, sizeof v);
    float wv[16];
    matmul(st.world, st.view, wv);
    matmul(wv, st.projection, v.wvp);
    memcpy(v.wv, wv, sizeof wv);
    normal_rows(wv, v.nrm);
    memcpy(v.mat_diffuse, st.material + 0, 16);
    memcpy(v.mat_ambient, st.material + 4, 16);
    memcpy(v.mat_specular, st.material + 8, 16);
    memcpy(v.mat_emissive, st.material + 12, 16);
    unpack(rs[d7::RS_AMBIENT], v.ambient);
    // Direct3D 7 pixel centres are at integer coordinates, Metal's half a
    // pixel further on: move the geometry half a pixel right and down, less
    // one subpixel step. Then a centre on an axis-aligned left or top edge
    // falls just inside and one on a right or bottom edge just outside - the
    // top-left rule, with no tie for float rounding to break - and an
    // attribute at a centre on a vertex row or column is evaluated just
    // inside the primitive, as Direct3D evaluates it exactly there. (A tie
    // broken the other way samples a wrapped texture's opposite edge.)
    const double shift = 0.5 - 1.0 / 256;
    v.ndc[0] = float(vw / mw);
    v.ndc[1] = float(vh / mh);
    v.ndc[2] = float((vx + vw / 2 + shift - mx0) * 2 / mw - 1);
    v.ndc[3] = float(1 - (vy + vh / 2 + shift - my0) * 2 / mh);
    v.viewport[0] = float(vx);
    v.viewport[1] = float(vy);
    v.viewport[2] = float(vw);
    v.viewport[3] = float(vh);
    v.fog[0] = as_float(rs[d7::RS_FOGSTART]);
    v.fog[1] = as_float(rs[d7::RS_FOGEND]);
    v.fog[2] = as_float(rs[d7::RS_FOGDENSITY]);
    v.fog[3] = st.material[16];
    uint32_t flags = 0;
    if (L.rhw)
        flags |= F_RHW;
    else if (rs[d7::RS_LIGHTING])
        flags |= F_LIGHTING;
    if (L.normal >= 0)
        flags |= F_NORMAL;
    if (L.diffuse >= 0)
        flags |= F_DIFFUSE;
    if (L.specular >= 0)
        flags |= F_SPECULAR;
    const uint32_t set = st.tss[0][d7::TSS_TEXCOORDINDEX] & 0xffff;
    if (set < uint32_t(L.tex_count)) {
        flags |= F_TEX;
        if (L.texcoord_size[set] < 2)
            flags |= F_TEX1D;
    }
    if (rs[d7::RS_SPECULARENABLE])
        flags |= F_SPECULARENABLE;
    if (rs[d7::RS_LOCALVIEWER])
        flags |= F_LOCALVIEWER;
    if (rs[d7::RS_NORMALIZENORMALS])
        flags |= F_NORMALIZE;
    if (rs[d7::RS_COLORVERTEX])
        flags |= F_COLORVERTEX;
    if (rs[d7::RS_RANGEFOGENABLE])
        flags |= F_RANGEFOG;
    if (rs[d7::RS_FOGENABLE] && rs[d7::RS_FOGTABLEMODE] == d7::FOG_NONE &&
        rs[d7::RS_FOGVERTEXMODE] != d7::FOG_NONE)
        flags |= F_VERTEXFOG;
    v.flags = flags;
    v.offsets = uint32_t(std::max(L.normal, 0)) | uint32_t(std::max(L.diffuse, 0)) << 8 |
                uint32_t(std::max(L.specular, 0)) << 16 |
                uint32_t(set < uint32_t(L.tex_count) ? L.texcoord[set] : 0) << 24;
    v.stride = d.stride;
    v.sources =
        (rs[d7::RS_DIFFUSEMATERIALSOURCE] & 3) | (rs[d7::RS_AMBIENTMATERIALSOURCE] & 3) << 2 |
        (rs[d7::RS_SPECULARMATERIALSOURCE] & 3) << 4 | (rs[d7::RS_EMISSIVEMATERIALSOURCE] & 3) << 6;
    v.fog_mode = rs[d7::RS_FOGVERTEXMODE];
    const float *view = st.view;
    for (uint32_t i = 0; i < st.light_count && i < HOST_D7_MAX_LIGHTS; ++i) {
        const HostD7Light &l = st.lights[i];
        Light7 &o = v.lights[v.light_count++];
        memcpy(o.diffuse, l.diffuse, 16);
        memcpy(o.specular, l.specular, 16);
        memcpy(o.ambient, l.ambient, 16);
        xform_point(l.position, view, o.position);
        o.position[3] = float(l.type);
        xform_dir(l.direction, view, o.direction);
        normalize3(o.direction);
        if (l.type == d7::LIGHT_DIRECTIONAL)
            for (int k = 0; k < 3; ++k)
                o.direction[k] = -o.direction[k];
        o.direction[3] = l.range;
        o.atten[0] = l.attenuation[0];
        o.atten[1] = l.attenuation[1];
        o.atten[2] = l.attenuation[2];
        o.atten[3] = l.falloff;
        o.spot[0] = std::cos(l.theta / 2);
        o.spot[1] = std::cos(l.phi / 2);
    }

    FS7 f;
    memset(&f, 0, sizeof f);
    unpack(rs[d7::RS_FOGCOLOR], f.fog_color);
    unpack(rs[d7::RS_TEXTUREFACTOR], f.tfactor);
    f.fog[0] = v.fog[0];
    f.fog[1] = v.fog[1];
    f.fog[2] = v.fog[2];
    const uint32_t *ts = st.tss[0];
    f.colorop = ts[d7::TSS_COLOROP];
    f.colorarg1 = ts[d7::TSS_COLORARG1];
    f.colorarg2 = ts[d7::TSS_COLORARG2];
    f.alphaop = ts[d7::TSS_ALPHAOP];
    f.alphaarg1 = ts[d7::TSS_ALPHAARG1];
    f.alphaarg2 = ts[d7::TSS_ALPHAARG2];
    Res *tex = d.texture.id ? find(d.texture.id) : nullptr;
    if (tex && tex->depth)
        tex = nullptr;
    f.textured = tex ? 1 : 0;
    f.tex_has_alpha = d.texture.has_alpha;
    f.alpha_func = rs[d7::RS_ALPHATESTENABLE] ? rs[d7::RS_ALPHAFUNC] : 0;
    f.alpha_ref = rs[d7::RS_ALPHAREF] & 0xff;
    f.fog_enable = rs[d7::RS_FOGENABLE] ? 1 : 0;
    f.table_fog = rs[d7::RS_FOGTABLEMODE];
    f.fog_w = st.projection[11] != 0 ? 1 : 0;
    f.flat = rs[d7::RS_SHADEMODE] == d7::SHADE_FLAT ? 1 : 0;
    f.specular = rs[d7::RS_SPECULARENABLE] ? 1 : 0;
    if (tex && rs[d7::RS_COLORKEYENABLE] && d.texture.key.enabled) {
        f.colorkey = 1;
        for (int i = 0; i < 3; ++i) {
            f.key_max[i] = (1u << d.texture.key.bits[i]) - 1;
            f.key_lo[i] = d.texture.key.lo[i];
            f.key_hi[i] = d.texture.key.hi[i];
        }
    }
    f.lod_bias = as_float(ts[d7::TSS_MIPMAPLODBIAS]);
    f.min_lod =
        float(std::min<uint32_t>(ts[d7::TSS_MAXMIPLEVEL], uint32_t(tex ? tex->levels - 1 : 0)));

    gpu::RenderState prs = pass_state();
    prs.write_alpha = !rt->opaque;
    prs.writes_point_size = topo == d7::Topology::Points;
    if (rs[d7::RS_ALPHABLENDENABLE]) {
        prs.blend_enabled = true;
        uint32_t sb = rs[d7::RS_SRCBLEND], db = rs[d7::RS_DESTBLEND];
        if (sb == d7::BLEND_BOTHSRCALPHA) {
            sb = d7::BLEND_SRCALPHA;
            db = d7::BLEND_INVSRCALPHA;
        } else if (sb == d7::BLEND_BOTHINVSRCALPHA) {
            sb = d7::BLEND_INVSRCALPHA;
            db = d7::BLEND_SRCALPHA;
        }
        prs.src_rgb = prs.src_alpha = blend_of(sb);
        prs.dst_rgb = prs.dst_alpha = blend_of(db);
    }
    gpu::Pipeline p = pipeline(false, prs);
    if (!p)
        return;
    gpu::DepthState ds;
    if (zb && rs[d7::RS_ZENABLE] != d7::ZB_FALSE) {
        ds.compare = compare_of(rs[d7::RS_ZFUNC]);
        ds.write = rs[d7::RS_ZWRITEENABLE] != 0;
    }
    // Direct3D's front faces are clockwise on screen, as are Metal's here.
    const uint32_t cull = rs[d7::RS_CULLMODE];
    const gpu::Cull mode = cull == d7::CULL_CCW  ? gpu::Cull::Back
                           : cull == d7::CULL_CW ? gpu::Cull::Front
                                                 : gpu::Cull::None;
    gpu::SamplerState s;
    s.u = address_of(ts[d7::TSS_ADDRESSU]);
    s.v = address_of(ts[d7::TSS_ADDRESSV]);
    s.mag = ts[d7::TSS_MAGFILTER] == d7::TFG_POINT ? gpu::Filter::Nearest : gpu::Filter::Linear;
    s.min = ts[d7::TSS_MINFILTER] == d7::TFN_POINT ? gpu::Filter::Nearest : gpu::Filter::Linear;
    s.mip = ts[d7::TSS_MIPFILTER] == d7::TFP_POINT    ? gpu::MipFilter::Nearest
            : ts[d7::TSS_MIPFILTER] == d7::TFP_LINEAR ? gpu::MipFilter::Linear
                                                      : gpu::MipFilter::None;
    if (ts[d7::TSS_MAGFILTER] == d7::TFG_ANISOTROPIC ||
        ts[d7::TSS_MINFILTER] == d7::TFN_ANISOTROPIC)
        s.anisotropy =
            int(std::min<uint32_t>(16, std::max<uint32_t>(1, ts[d7::TSS_MAXANISOTROPY])));
    if (ts[d7::TSS_ADDRESSU] == d7::TADDRESS_BORDER || ts[d7::TSS_ADDRESSV] == d7::TADDRESS_BORDER)
        log_once("border", "texture border colours other than transparent black are not honoured");
    gpu::SamplerState nearest = s;
    nearest.mag = nearest.min = gpu::Filter::Nearest;
    nearest.mip = gpu::MipFilter::None;
    nearest.anisotropy = 1;

    dev->set_pipeline(cb, p);
    dev->set_depth(cb, ds);
    dev->set_cull(cb, mode);
    dev->set_viewport(cb, {double(mx0), double(my0), mw, mh, L.rhw ? 0.0 : double(st.viewport_minz),
                           L.rhw ? 1.0 : double(st.viewport_maxz)});
    dev->set_buffer(cb, gpu::Stage::Vertex, 0, vb.buffer, vb.offset);
    dev->set_bytes(cb, gpu::Stage::Vertex, 1, &v, sizeof v);
    dev->set_buffer(cb, gpu::Stage::Vertex, 2, ib.buffer, ib.offset);
    dev->set_bytes(cb, gpu::Stage::Fragment, 1, &f, sizeof f);
    dev->set_texture(cb, gpu::Stage::Fragment, 0, tex ? tex->texture : dummy);
    dev->set_sampler(cb, gpu::Stage::Fragment, 0, s);
    dev->set_sampler(cb, gpu::Stage::Fragment, 1, nearest);
    const gpu::Primitive prim = topo == d7::Topology::Points  ? gpu::Primitive::Points
                                : topo == d7::Topology::Lines ? gpu::Primitive::Lines
                                                              : gpu::Primitive::Triangles;
    dev->draw(cb, prim, 0, int(list.size()));
    ++stats.draws;
    stats.primitives += topo == d7::Topology::Triangles ? list.size() / 3
                        : topo == d7::Topology::Lines   ? list.size() / 2
                                                        : list.size();
}

// ---------------------------------------------------------------------------
// Presentation and frame dumps
// ---------------------------------------------------------------------------
namespace {
std::mutex g_dump_lock;
std::string g_dump_next;
uint64_t g_present_serial = 0;
std::atomic<void (*)(const uint32_t *, int, int)> g_frame_tap{nullptr};

bool write_ppm(const std::string &path, const uint32_t *bgra, int w, int h) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = bgra[size_t(y) * size_t(w) + size_t(x)];
            row[size_t(x) * 3 + 0] = uint8_t(p >> 16);
            row[size_t(x) * 3 + 1] = uint8_t(p >> 8);
            row[size_t(x) * 3 + 2] = uint8_t(p);
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    return fclose(f) == 0;
}
// The path the present numbered `serial` is dumped to, or empty.
std::string dump_path(uint64_t serial) {
    {
        std::lock_guard lock(g_dump_lock);
        if (!g_dump_next.empty()) {
            std::string p = g_dump_next;
            g_dump_next.clear();
            return p;
        }
    }
    const char *dir = recomp_env("D3D7_DUMP_DIR");
    if (!dir || !*dir)
        return {};
    long every = 1;
    if (const char *e = recomp_env("D3D7_DUMP_EVERY"))
        every = std::max(1L, strtol(e, nullptr, 10));
    if (serial % uint64_t(every))
        return {};
    char name[64];
    snprintf(name, sizeof name, "/frame-%06llu.ppm", (unsigned long long)serial);
    return std::string(dir) + name;
}
} // namespace

extern "C" void host_d7_set_frame_tap(void (*tap)(const uint32_t *, int, int)) {
    g_frame_tap.store(tap);
}

void D3D7Renderer::Impl::present(uint32_t id, const HostD7Rect &r, int mode_w, int mode_h) {
    Res *t = find(id);
    if (!t || t->depth)
        return;
    const int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w <= 0 || h <= 0)
        return;
    ++stats.presents;
    const uint64_t serial = ++g_present_serial;
    const std::string dump = dump_path(serial);
    const auto tap = g_frame_tap.load();
    if (!dump.empty() || tap) {
        std::vector<uint32_t> px(size_t(w) * size_t(h));
        if (readback(id, 0, r, px.data(), w * 4)) {
            if (!dump.empty() && !write_ppm(dump, px.data(), w, h))
                fprintf(stderr, "[d3d7] could not write %s\n", dump.c_str());
            if (tap)
                tap(px.data(), w, h);
        }
    }
    end_pass();
    gpu::Texture shown = t->texture;
    if (r.x0 != 0 || r.y0 != 0 || w != t->w || h != t->h) {
        // The presenter takes a whole texture: copy the region out first.
        gpu::Texture region = dev->create_texture(
            {w, h, gpu::Format::BGRA8, gpu::UsageSampled | gpu::UsageRenderTarget, 1});
        if (!region)
            return;
        dev->blit(command(), t->texture, {r.x0, r.y0, w, h}, region, 0, 0);
        shown = region;
        if (host_d7_show(shown, w, h, mode_w, mode_h, command())) {
            commit();
            host_d7_shown(w, h);
        } else {
            std::vector<uint32_t> px(size_t(w) * size_t(h));
            commit();
            if (dev->readback(region, {0, 0, w, h}, px.data(), w * 4))
                display_present_frame(px.data(), w, h);
        }
        dev->destroy(region);
        return;
    }
    if (host_d7_show(shown, w, h, mode_w, mode_h, command())) {
        commit();
        host_d7_shown(w, h);
        return;
    }
    std::vector<uint32_t> px(size_t(w) * size_t(h));
    if (readback(id, 0, r, px.data(), w * 4))
        display_present_frame(px.data(), w, h);
}

// ===========================================================================
// Public wrapper
// ===========================================================================
D3D7Renderer::D3D7Renderer(gpu::Device *device) : impl_(std::make_unique<Impl>(device)) {}
D3D7Renderer::~D3D7Renderer() = default;
bool D3D7Renderer::ok() const {
    return impl_->ok;
}
gpu::Device *D3D7Renderer::device() const {
    return impl_->dev;
}
uint32_t D3D7Renderer::generation() const {
    return impl_->generation;
}
bool D3D7Renderer::create(uint32_t id, int w, int h, int levels, uint32_t flags) {
    return impl_->create(id, w, h, levels, flags);
}
void D3D7Renderer::destroy(uint32_t id) {
    impl_->destroy(id);
}
void D3D7Renderer::upload(uint32_t id, int level, const HostD7Rect &r, const void *bgra,
                          int pitch) {
    impl_->upload(id, level, r, bgra, pitch);
}
bool D3D7Renderer::readback(uint32_t id, int level, const HostD7Rect &r, void *bgra, int pitch) {
    return impl_->readback(id, level, r, bgra, pitch);
}
void D3D7Renderer::fill(uint32_t id, const HostD7Rect &r, uint32_t bgra) {
    impl_->fill(id, r, bgra);
}
void D3D7Renderer::fill_depth(uint32_t id, const HostD7Rect &r, float z) {
    impl_->fill_depth(id, r, z);
}
void D3D7Renderer::blit(const HostD7Blit &b) {
    impl_->blit(b);
}
void D3D7Renderer::draw(const HostD7Draw &d) {
    impl_->draw(d);
}
void D3D7Renderer::present(uint32_t id, const HostD7Rect &r, int mode_w, int mode_h) {
    impl_->present(id, r, mode_w, mode_h);
}
void D3D7Renderer::flush() {
    impl_->commit();
}
void D3D7Renderer::finish() {
    impl_->finish();
}
HostD7Stats D3D7Renderer::stats() const {
    return impl_->stats;
}

// ===========================================================================
// The seam
// ===========================================================================
namespace {
std::recursive_mutex g_lock;
D3D7Renderer *g_installed = nullptr;
std::unique_ptr<D3D7Renderer> g_auto;

D3D7Renderer *active() {
    if (g_installed)
        return g_installed;
    gpu::Device *d = host_d7_presenter_device();
    if (!d) {
        g_auto.reset();
        return nullptr;
    }
    if (!g_auto || g_auto->device() != d) {
        g_auto.reset();
        auto r = std::make_unique<D3D7Renderer>(d);
        if (!r->ok()) {
            log_once("noprograms", "this GPU backend has no Direct3D 7 programs");
            return nullptr;
        }
        g_auto = std::move(r);
    }
    return g_auto.get();
}
} // namespace

D3D7Renderer *D3D7Renderer::shared() {
    std::lock_guard lock(g_lock);
    return active();
}
void D3D7Renderer::setShared(D3D7Renderer *renderer) {
    std::lock_guard lock(g_lock);
    g_installed = renderer;
}

__attribute__((weak)) bool host_d7_show(gpu::Texture, int, int, int, int, gpu::CommandBuffer) {
    return false;
}
__attribute__((weak)) void host_d7_shown(int, int) {}
__attribute__((weak)) gpu::Device *host_d7_presenter_device() {
    return nullptr;
}

extern "C" {
void host_d7_dump_next(const char *path) {
    std::lock_guard lock(g_dump_lock);
    g_dump_next = path ? path : "";
}
int host_d7_available(void) {
    std::lock_guard lock(g_lock);
    return active() != nullptr;
}
uint32_t host_d7_generation(void) {
    std::lock_guard lock(g_lock);
    D3D7Renderer *r = active();
    return r ? r->generation() : 0;
}
int host_d7_create(uint32_t id, int w, int h, int levels, uint32_t flags) {
    std::lock_guard lock(g_lock);
    D3D7Renderer *r = active();
    return r && r->create(id, w, h, levels, flags) ? 1 : 0;
}
void host_d7_destroy(uint32_t id) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active())
        r->destroy(id);
}
void host_d7_upload(uint32_t id, int level, const HostD7Rect *rect, const void *bgra, int pitch) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && rect && bgra)
        r->upload(id, level, *rect, bgra, pitch);
}
int host_d7_readback(uint32_t id, int level, const HostD7Rect *rect, void *bgra, int pitch) {
    std::lock_guard lock(g_lock);
    D3D7Renderer *r = active();
    return r && rect && bgra && r->readback(id, level, *rect, bgra, pitch) ? 1 : 0;
}
void host_d7_fill(uint32_t id, const HostD7Rect *rect, uint32_t bgra) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && rect)
        r->fill(id, *rect, bgra);
}
void host_d7_fill_depth(uint32_t id, const HostD7Rect *rect, float z) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && rect)
        r->fill_depth(id, *rect, z);
}
void host_d7_blit(const HostD7Blit *b) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && b)
        r->blit(*b);
}
void host_d7_draw(const HostD7Draw *d) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && d)
        r->draw(*d);
}
void host_d7_present(uint32_t id, const HostD7Rect *rect, int w, int h) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active(); r && rect)
        r->present(id, *rect, w, h);
}
void host_d7_flush(void) {
    std::lock_guard lock(g_lock);
    if (D3D7Renderer *r = active())
        r->flush();
}
void host_d7_stats(HostD7Stats *out) {
    std::lock_guard lock(g_lock);
    if (!out)
        return;
    D3D7Renderer *r = active();
    if (r)
        *out = r->stats();
    else
        memset(out, 0, sizeof *out);
}
// The presenter is stopping and its device may go with it: finish the work and
// drop the renderer made on it. A later use makes a new one (a new generation).
void host_d7_release_device(void) {
    std::lock_guard lock(g_lock);
    if (g_auto)
        g_auto->finish();
    g_auto.reset();
}
} // extern "C"
