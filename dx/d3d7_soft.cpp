// d3d7_soft.cpp - a software implementation of the d3d7_host.h seam, written
// as a reference for the Direct3D 7 fixed-function pipeline.
//
// It is the oracle the GPU renderer is tested against, and a headless backend
// (RECOMP_D3D7_SOFTWARE=1) that renders without a GPU. It is written for
// clarity, not speed: one vertex, one pixel at a time, in double precision
// where the GPU uses float, following the DirectX 7 documentation's
// equations rather than the GPU renderer's code.
//
// Conventions it fixes, which the GPU renderer must reproduce:
//   * Pixel centres are at integer screen coordinates (Direct3D 7, not 10),
//     sampled one subpixel step (1/256) right and down of them (kNudge).
//   * Coverage: the top-left rule on exact ties, vertex positions snapped to
//     1/256 pixel.
//   * Depth is interpolated linearly in screen space; every other attribute
//     perspective-correctly.
//   * Lighting runs in camera space, normals through the inverse transpose of
//     the world-view matrix.
//   * Colour channels are stored as 8 bits, rounded to nearest.
#include "d3d7_internal.h"
#include "d3d7_prim.h"
#include "../runtime/display_seam.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace d7soft {
namespace {

struct Res {
    int w = 0, h = 0, levels = 1;
    bool depth = false, opaque = false;
    std::vector<std::vector<uint32_t>> color; // BGRA8 per level
    std::vector<int> lw, lh;
    std::vector<float> z;
};
std::unordered_map<uint32_t, Res> &resources() {
    static auto *m = new std::unordered_map<uint32_t, Res>();
    return *m;
}
uint32_t g_generation = 1;

Res *find(uint32_t id) {
    auto it = resources().find(id);
    return it == resources().end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Small vector maths, double precision.
// ---------------------------------------------------------------------------
struct V4 {
    double x = 0, y = 0, z = 0, w = 0;
};
struct C4 {
    double r = 0, g = 0, b = 0, a = 0;
};
inline C4 operator+(C4 a, C4 b) {
    return {a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
}
inline C4 operator*(C4 a, C4 b) {
    return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
}
inline C4 operator*(C4 a, double s) {
    return {a.r * s, a.g * s, a.b * s, a.a * s};
}
inline double clamp01(double v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
inline C4 sat(C4 c) {
    return {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
}
inline C4 unpack(uint32_t bgra) {
    return {((bgra >> 16) & 0xff) / 255.0, ((bgra >> 8) & 0xff) / 255.0, (bgra & 0xff) / 255.0,
            (bgra >> 24) / 255.0};
}
inline uint32_t to8(double v) {
    return uint32_t(std::floor(clamp01(v) * 255.0 + 0.5));
}
inline uint32_t pack(C4 c) {
    return to8(c.b) | to8(c.g) << 8 | to8(c.r) << 16 | to8(c.a) << 24;
}
inline C4 from_floats(const float *f) {
    return {f[0], f[1], f[2], f[3]};
}
// D3DCOLOR is ARGB.
inline C4 from_d3dcolor(uint32_t c) {
    return unpack(c);
}
// v (row vector) times row-major 4x4 M.
inline V4 mul(V4 v, const double *m) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8] + v.w * m[12],
            v.x * m[1] + v.y * m[5] + v.z * m[9] + v.w * m[13],
            v.x * m[2] + v.y * m[6] + v.z * m[10] + v.w * m[14],
            v.x * m[3] + v.y * m[7] + v.z * m[11] + v.w * m[15]};
}
inline void matmul(const double *a, const double *b, double *out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}
inline double dot3(V4 a, V4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V4 norm3(V4 a) {
    const double l = std::sqrt(dot3(a, a));
    return l > 0 ? V4{a.x / l, a.y / l, a.z / l, 0} : a;
}
// Inverse transpose of the upper 3x3 of row-major m, for normals.
void normal_matrix(const double *m, double *n) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                 i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double D = -(b * i - c * h), E = a * i - c * g, F = -(a * h - b * g);
    const double G = b * f - c * e, H = -(a * f - c * d), I = a * e - b * d;
    double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-30)
        det = 1e-30;
    // inverse = adj^T / det; its transpose is the cofactor matrix / det.
    const double cof[9] = {A, B, C, D, E, F, G, H, I};
    for (int k = 0; k < 9; ++k)
        n[k] = cof[k] / det;
}
inline V4 mul3(V4 v, const double *n) { // row vector times 3x3
    return {v.x * n[0] + v.y * n[3] + v.z * n[6], v.x * n[1] + v.y * n[4] + v.z * n[7],
            v.x * n[2] + v.y * n[5] + v.z * n[8], 0};
}

bool compare(uint32_t func, double a, double b) {
    switch (func) {
    case d7::CMP_NEVER:
        return false;
    case d7::CMP_LESS:
        return a < b;
    case d7::CMP_EQUAL:
        return a == b;
    case d7::CMP_LESSEQUAL:
        return a <= b;
    case d7::CMP_GREATER:
        return a > b;
    case d7::CMP_NOTEQUAL:
        return a != b;
    case d7::CMP_GREATEREQUAL:
        return a >= b;
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
// Texture sampling
// ---------------------------------------------------------------------------
struct SamplerState {
    uint32_t address_u = d7::TADDRESS_WRAP, address_v = d7::TADDRESS_WRAP;
    uint32_t mag = d7::TFG_POINT, min = d7::TFN_POINT, mip = d7::TFP_NONE;
    double bias = 0;
    uint32_t max_level = 0;
    uint32_t border = 0;
};
// Texel index `i` under an address mode; -1 for a border texel.
int address(int i, int n, uint32_t mode) {
    switch (mode) {
    case d7::TADDRESS_MIRROR: {
        int p = i % (2 * n);
        if (p < 0)
            p += 2 * n;
        return p < n ? p : 2 * n - 1 - p;
    }
    case d7::TADDRESS_CLAMP:
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case d7::TADDRESS_BORDER:
        return i < 0 || i >= n ? -1 : i;
    default: {
        int p = i % n;
        return p < 0 ? p + n : p;
    }
    }
}
C4 texel(const Res &t, int level, int x, int y, const SamplerState &s, bool has_alpha) {
    const int w = t.lw[size_t(level)], h = t.lh[size_t(level)];
    const int ax = address(x, w, s.address_u), ay = address(y, h, s.address_v);
    C4 c = ax < 0 || ay < 0 ? from_d3dcolor(s.border)
                            : unpack(t.color[size_t(level)][size_t(ay) * size_t(w) + size_t(ax)]);
    if (!has_alpha)
        c.a = 1;
    return c;
}
C4 filter_level(const Res &t, int level, double u, double v, bool linear, const SamplerState &s,
                bool has_alpha) {
    const int w = t.lw[size_t(level)], h = t.lh[size_t(level)];
    if (!linear)
        return texel(t, level, int(std::floor(u * w)), int(std::floor(v * h)), s, has_alpha);
    const double x = u * w - 0.5, y = v * h - 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    const C4 a = texel(t, level, x0, y0, s, has_alpha),
             b = texel(t, level, x0 + 1, y0, s, has_alpha);
    const C4 c = texel(t, level, x0, y0 + 1, s, has_alpha),
             d = texel(t, level, x0 + 1, y0 + 1, s, has_alpha);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
// `rho` is the texel footprint of one pixel at level 0.
C4 sample(const Res &t, double u, double v, double rho, const SamplerState &s, bool has_alpha) {
    // MAXMIPLEVEL names the largest level used (0 is the top): the level of
    // detail is clamped to it, and magnification happens only at level 0.
    const int base = std::min(t.levels - 1, int(s.max_level));
    double lambda = (rho > 0 ? std::log2(rho) : -100.0) + s.bias;
    if (base > 0)
        lambda = std::max(lambda, double(base));
    if (lambda <= 0) // magnification
        return filter_level(t, 0, u, v, s.mag != d7::TFG_POINT, s, has_alpha);
    const bool linear = s.min != d7::TFN_POINT;
    if (s.mip == d7::TFP_NONE || s.mip == 0 || t.levels == 1)
        return filter_level(t, base, u, v, linear, s, has_alpha);
    const double l = std::min(lambda, double(t.levels - 1));
    if (s.mip == d7::TFP_POINT)
        return filter_level(t, int(std::floor(l + 0.5)), u, v, linear, s, has_alpha);
    const int l0 = int(std::floor(l)), l1 = std::min(t.levels - 1, l0 + 1);
    const double f = l - l0;
    return filter_level(t, l0, u, v, linear, s, has_alpha) * (1 - f) +
           filter_level(t, l1, u, v, linear, s, has_alpha) * f;
}

// ---------------------------------------------------------------------------
// Vertex processing
// ---------------------------------------------------------------------------
struct PV {
    V4 clip; // clip-space position (or, pre-transformed, screen x, y, z and w)
    double eye_z = 0;
    C4 diffuse, specular;
    double u = 0, v = 0;
    double fog = 1; // 1 is no fog
};

struct Prepared {
    double world[16], view[16], proj[16], wv[16], wvp[16];
    double nrm[9];   // inverse transpose of the world-view 3x3, for normals
    double view3[9]; // the view's 3x3, for light directions
    bool lighting = false;
};
inline float rdf(const uint8_t *p, int off) {
    float f;
    memcpy(&f, p + off, 4);
    return f;
}
inline uint32_t rdu(const uint8_t *p, int off) {
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

C4 material_source(uint32_t source, const C4 &mat, bool has_diff, const C4 &vdiff, bool has_spec,
                   const C4 &vspec) {
    if (source == d7::MCS_COLOR1 && has_diff)
        return vdiff;
    if (source == d7::MCS_COLOR2 && has_spec)
        return vspec;
    return mat;
}

double fog_factor(uint32_t mode, double d, const HostD7State &st) {
    float start, end, density;
    memcpy(&start, &st.rs[d7::RS_FOGSTART], 4);
    memcpy(&end, &st.rs[d7::RS_FOGEND], 4);
    memcpy(&density, &st.rs[d7::RS_FOGDENSITY], 4);
    switch (mode) {
    case d7::FOG_LINEAR:
        if (end == start)
            return d < start ? 1 : 0;
        return clamp01((end - d) / (double(end) - start));
    case d7::FOG_EXP:
        return clamp01(std::exp(-density * d));
    case d7::FOG_EXP2:
        return clamp01(std::exp(-(density * d) * (density * d)));
    default:
        return 1;
    }
}

PV process(const uint8_t *vx, const d7::FvfLayout &L, const HostD7State &st, const Prepared &m) {
    PV o;
    const C4 white{1, 1, 1, 1}, zero{0, 0, 0, 0};
    const bool has_diff = L.diffuse >= 0, has_spec = L.specular >= 0;
    const C4 vdiff = has_diff ? from_d3dcolor(rdu(vx, L.diffuse)) : white;
    const C4 vspec = has_spec ? from_d3dcolor(rdu(vx, L.specular)) : zero;
    const uint32_t set = st.tss[0][d7::TSS_TEXCOORDINDEX] & 0xffff;
    if (set < uint32_t(L.tex_count) && L.texcoord_size[set] >= 2) {
        o.u = rdf(vx, L.texcoord[set]);
        o.v = rdf(vx, L.texcoord[set] + 4);
    } else if (set < uint32_t(L.tex_count)) {
        o.u = rdf(vx, L.texcoord[set]);
    }
    if (L.rhw) {
        // Pre-transformed: screen x, y, z, then 1/w. No lighting, no vertex fog
        // computation: the specular alpha is the fog factor.
        const double rhw = rdf(vx, 12);
        o.clip = {rdf(vx, 0), rdf(vx, 4), rdf(vx, 8), rhw != 0 ? 1.0 / rhw : 1.0};
        o.diffuse = vdiff;
        o.specular = vspec;
        o.fog = vspec.a;
        return o;
    }
    const V4 p{rdf(vx, 0), rdf(vx, 4), rdf(vx, 8), 1};
    const V4 pe = mul(p, m.wv);
    o.clip = mul(p, m.wvp);
    o.eye_z = pe.z;
    if (!m.lighting) {
        o.diffuse = vdiff;
        o.specular = vspec;
    } else {
        const bool cv = st.rs[d7::RS_COLORVERTEX] != 0;
        const C4 md = from_floats(st.material + 0), ma = from_floats(st.material + 4),
                 ms = from_floats(st.material + 8), me = from_floats(st.material + 12);
        const double power = st.material[16];
        const C4 cd = cv ? material_source(st.rs[d7::RS_DIFFUSEMATERIALSOURCE], md, has_diff, vdiff,
                                           has_spec, vspec)
                         : md;
        const C4 ca = cv ? material_source(st.rs[d7::RS_AMBIENTMATERIALSOURCE], ma, has_diff, vdiff,
                                           has_spec, vspec)
                         : ma;
        const C4 cs = cv ? material_source(st.rs[d7::RS_SPECULARMATERIALSOURCE], ms, has_diff,
                                           vdiff, has_spec, vspec)
                         : ms;
        const C4 ce = cv ? material_source(st.rs[d7::RS_EMISSIVEMATERIALSOURCE], me, has_diff,
                                           vdiff, has_spec, vspec)
                         : me;
        V4 n{0, 0, 0, 0};
        if (L.normal >= 0) {
            n = mul3({rdf(vx, L.normal), rdf(vx, L.normal + 4), rdf(vx, L.normal + 8), 0}, m.nrm);
            if (st.rs[d7::RS_NORMALIZENORMALS])
                n = norm3(n);
        }
        const C4 ga = from_d3dcolor(st.rs[d7::RS_AMBIENT]);
        C4 amb = ga, dif{0, 0, 0, 0}, spc{0, 0, 0, 0};
        const V4 eye =
            st.rs[d7::RS_LOCALVIEWER] ? norm3({-pe.x, -pe.y, -pe.z, 0}) : V4{0, 0, -1, 0};
        for (uint32_t i = 0; i < st.light_count && i < HOST_D7_MAX_LIGHTS; ++i) {
            const HostD7Light &l = st.lights[i];
            V4 to_light;
            double atten = 1, spot = 1;
            if (l.type == d7::LIGHT_DIRECTIONAL) {
                const V4 dir = mul3({l.direction[0], l.direction[1], l.direction[2], 0}, m.view3);
                to_light = norm3({-dir.x, -dir.y, -dir.z, 0});
            } else {
                const V4 lp = mul({l.position[0], l.position[1], l.position[2], 1}, m.view);
                const V4 d{lp.x - pe.x, lp.y - pe.y, lp.z - pe.z, 0};
                const double dist = std::sqrt(dot3(d, d));
                if (dist > l.range)
                    continue;
                to_light = dist > 0 ? V4{d.x / dist, d.y / dist, d.z / dist, 0} : V4{0, 0, 0, 0};
                const double den =
                    l.attenuation[0] + l.attenuation[1] * dist + l.attenuation[2] * dist * dist;
                atten = den > 0 ? 1.0 / den : 1.0;
                if (l.type == d7::LIGHT_SPOT) {
                    const V4 sd =
                        norm3(mul3({l.direction[0], l.direction[1], l.direction[2], 0}, m.view3));
                    const double rho = -dot3(sd, to_light);
                    const double ct = std::cos(l.theta / 2), cp = std::cos(l.phi / 2);
                    if (rho > ct)
                        spot = 1;
                    else if (rho <= cp)
                        spot = 0;
                    else
                        spot = std::pow((rho - cp) / (ct - cp), double(l.falloff));
                }
            }
            const double k = atten * spot;
            amb = amb + from_floats(l.ambient) * k;
            const double ndl = std::max(0.0, dot3(n, to_light));
            dif = dif + from_floats(l.diffuse) * (ndl * k);
            if (st.rs[d7::RS_SPECULARENABLE] && ndl > 0) {
                const V4 hv =
                    norm3({to_light.x + eye.x, to_light.y + eye.y, to_light.z + eye.z, 0});
                const double ndh = std::max(0.0, dot3(n, hv));
                spc = spc + from_floats(l.specular) * (std::pow(ndh, power) * k);
            }
        }
        C4 out = ce + ca * amb + cd * dif;
        out.a = cd.a;
        o.diffuse = sat(out);
        C4 so = cs * spc;
        so.a = 0;
        o.specular = sat(so);
    }
    if (st.rs[d7::RS_FOGENABLE] && st.rs[d7::RS_FOGTABLEMODE] == d7::FOG_NONE &&
        st.rs[d7::RS_FOGVERTEXMODE] != d7::FOG_NONE) {
        const double d = st.rs[d7::RS_RANGEFOGENABLE] ? std::sqrt(dot3(pe, pe)) : pe.z;
        o.fog = fog_factor(st.rs[d7::RS_FOGVERTEXMODE], d, st);
    } else {
        o.fog = o.specular.a;
    }
    return o;
}

// ---------------------------------------------------------------------------
// Clipping against 0 <= z <= w (and w > 0), in clip space.
// ---------------------------------------------------------------------------
PV lerp(const PV &a, const PV &b, double t) {
    PV o;
    o.clip = {a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
              a.clip.z + (b.clip.z - a.clip.z) * t, a.clip.w + (b.clip.w - a.clip.w) * t};
    o.eye_z = a.eye_z + (b.eye_z - a.eye_z) * t;
    o.diffuse = a.diffuse * (1 - t) + b.diffuse * t;
    o.specular = a.specular * (1 - t) + b.specular * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    o.fog = a.fog + (b.fog - a.fog) * t;
    return o;
}
std::vector<PV> clip_polygon(std::vector<PV> poly) {
    // Planes as dot(plane, clip) >= 0: z >= 0, w - z >= 0, w - eps >= 0.
    const double planes[3][4] = {{0, 0, 1, 0}, {0, 0, -1, 1}, {0, 0, 0, 1}};
    const double offsets[3] = {0, 0, -1e-9};
    for (int p = 0; p < 3 && !poly.empty(); ++p) {
        std::vector<PV> out;
        auto dist = [&](const PV &v) {
            return planes[p][0] * v.clip.x + planes[p][1] * v.clip.y + planes[p][2] * v.clip.z +
                   planes[p][3] * v.clip.w + offsets[p];
        };
        for (size_t i = 0; i < poly.size(); ++i) {
            const PV &a = poly[i], &b = poly[(i + 1) % poly.size()];
            const double da = dist(a), db = dist(b);
            if (da >= 0)
                out.push_back(a);
            if ((da >= 0) != (db >= 0))
                out.push_back(lerp(a, b, da / (da - db)));
        }
        poly.swap(out);
    }
    return poly;
}

// ---------------------------------------------------------------------------
// The pixel pipeline
// ---------------------------------------------------------------------------
struct Target {
    Res *rt = nullptr;
    Res *zb = nullptr;
    int x0, y0, x1, y1; // the viewport, clipped to the target
};
struct Screen {
    double x, y, z, invw; // screen position, depth, 1/w
};

struct PixelContext {
    const HostD7Draw *d;
    const HostD7State *st;
    const Res *tex;
    SamplerState samp;
    bool flat = false;
    bool table_fog = false, fog_w = false;
};

C4 arg_value(uint32_t a, const C4 &diffuse, const C4 &current, const C4 &tex, const C4 &tfactor,
             const C4 &spec) {
    C4 v;
    switch (a & d7::TA_SELECTMASK) {
    case d7::TA_CURRENT:
        v = current;
        break;
    case d7::TA_TEXTURE:
        v = tex;
        break;
    case d7::TA_TFACTOR:
        v = tfactor;
        break;
    case d7::TA_SPECULAR:
        v = spec;
        break;
    default:
        v = diffuse;
        break;
    }
    if (a & d7::TA_COMPLEMENT)
        v = {1 - v.r, 1 - v.g, 1 - v.b, 1 - v.a};
    if (a & d7::TA_ALPHAREPLICATE)
        v = {v.a, v.a, v.a, v.a};
    return v;
}
// One texture operation on one channel group (rgb when `alpha` is false).
C4 texop(uint32_t op, const C4 &a1, const C4 &a2, const C4 &diffuse, const C4 &tex,
         const C4 &tfactor, const C4 &current) {
    switch (op) {
    case d7::TOP_SELECTARG1:
        return a1;
    case d7::TOP_SELECTARG2:
        return a2;
    case d7::TOP_MODULATE:
        return a1 * a2;
    case d7::TOP_MODULATE2X:
        return sat(a1 * a2 * 2);
    case d7::TOP_MODULATE4X:
        return sat(a1 * a2 * 4);
    case d7::TOP_ADD:
        return sat(a1 + a2);
    case d7::TOP_ADDSIGNED:
        return sat({a1.r + a2.r - 0.5, a1.g + a2.g - 0.5, a1.b + a2.b - 0.5, a1.a + a2.a - 0.5});
    case d7::TOP_ADDSIGNED2X:
        return sat({(a1.r + a2.r - 0.5) * 2, (a1.g + a2.g - 0.5) * 2, (a1.b + a2.b - 0.5) * 2,
                    (a1.a + a2.a - 0.5) * 2});
    case d7::TOP_SUBTRACT:
        return sat({a1.r - a2.r, a1.g - a2.g, a1.b - a2.b, a1.a - a2.a});
    case d7::TOP_ADDSMOOTH:
        return sat(a1 + a2 + a1 * a2 * -1.0);
    case d7::TOP_BLENDDIFFUSEALPHA:
        return a1 * diffuse.a + a2 * (1 - diffuse.a);
    case d7::TOP_BLENDTEXTUREALPHA:
        return a1 * tex.a + a2 * (1 - tex.a);
    case d7::TOP_BLENDFACTORALPHA:
        return a1 * tfactor.a + a2 * (1 - tfactor.a);
    case d7::TOP_BLENDTEXTUREALPHAPM:
        return sat(a1 + a2 * (1 - tex.a));
    case d7::TOP_BLENDCURRENTALPHA:
        return a1 * current.a + a2 * (1 - current.a);
    default:
        return a1 * a2;
    }
}

double blend_factor(uint32_t f, const C4 &src, const C4 &dst, int channel /* 0..2 rgb, 3 a */) {
    const double s[4] = {src.r, src.g, src.b, src.a}, d[4] = {dst.r, dst.g, dst.b, dst.a};
    switch (f) {
    case d7::BLEND_ZERO:
        return 0;
    case d7::BLEND_ONE:
        return 1;
    case d7::BLEND_SRCCOLOR:
        return s[channel];
    case d7::BLEND_INVSRCCOLOR:
        return 1 - s[channel];
    case d7::BLEND_SRCALPHA:
        return src.a;
    case d7::BLEND_INVSRCALPHA:
        return 1 - src.a;
    case d7::BLEND_DESTALPHA:
        return dst.a;
    case d7::BLEND_INVDESTALPHA:
        return 1 - dst.a;
    case d7::BLEND_DESTCOLOR:
        return d[channel];
    case d7::BLEND_INVDESTCOLOR:
        return 1 - d[channel];
    case d7::BLEND_SRCALPHASAT:
        return channel == 3 ? 1 : std::min(src.a, 1 - dst.a);
    default:
        return 1;
    }
}

void shade_pixel(const PixelContext &px, Target &t, int x, int y, double z, const PV &a,
                 const PV &flat_v, double rho) {
    const HostD7State &st = *px.st;
    const uint32_t *rs = st.rs;
    // Depth test first only decides; nothing is written until the pixel survives.
    float *zp = nullptr;
    const bool ztest = t.zb && rs[d7::RS_ZENABLE] != d7::ZB_FALSE;
    if (ztest) {
        zp = &t.zb->z[size_t(y) * size_t(t.zb->w) + size_t(x)];
        if (!compare(rs[d7::RS_ZFUNC], float(z), *zp))
            return;
    }
    const C4 diffuse = px.flat ? flat_v.diffuse : a.diffuse;
    const C4 spec = px.flat ? flat_v.specular : a.specular;
    const C4 tfactor = from_d3dcolor(rs[d7::RS_TEXTUREFACTOR]);
    C4 tex{1, 1, 1, 1};
    const HostD7Texture &T = px.d->texture;
    if (px.tex) {
        tex = sample(*px.tex, a.u, a.v, rho, px.samp, T.has_alpha != 0);
        if (rs[d7::RS_COLORKEYENABLE] && T.key.enabled) {
            // The nearest texel at level 0, quantised to the source format.
            SamplerState nearest = px.samp;
            const C4 k = texel(*px.tex, 0, int(std::floor(a.u * px.tex->w)),
                               int(std::floor(a.v * px.tex->h)), nearest, true);
            const double ch[3] = {k.r, k.g, k.b};
            bool match = true;
            for (int i = 0; i < 3; ++i) {
                const double max = double((1u << T.key.bits[i]) - 1);
                const double q = std::floor(ch[i] * max + 0.5);
                match = match && q >= T.key.lo[i] && q <= T.key.hi[i];
            }
            if (match)
                return;
        }
    }
    // Stage 0. DISABLE on the colour operation turns the whole cascade off.
    const uint32_t *ts = st.tss[0];
    C4 c = diffuse;
    if (ts[d7::TSS_COLOROP] != d7::TOP_DISABLE) {
        const C4 current = diffuse;
        const C4 c1 = arg_value(ts[d7::TSS_COLORARG1], diffuse, current, tex, tfactor, spec);
        const C4 c2 = arg_value(ts[d7::TSS_COLORARG2], diffuse, current, tex, tfactor, spec);
        const C4 rgb = texop(ts[d7::TSS_COLOROP], c1, c2, diffuse, tex, tfactor, current);
        double alpha = current.a;
        if (ts[d7::TSS_ALPHAOP] != d7::TOP_DISABLE) {
            const C4 a1 = arg_value(ts[d7::TSS_ALPHAARG1], diffuse, current, tex, tfactor, spec);
            const C4 a2 = arg_value(ts[d7::TSS_ALPHAARG2], diffuse, current, tex, tfactor, spec);
            alpha = texop(ts[d7::TSS_ALPHAOP], a1, a2, diffuse, tex, tfactor, current).a;
        }
        c = {rgb.r, rgb.g, rgb.b, alpha};
    }
    c = sat(c);
    if (rs[d7::RS_SPECULARENABLE])
        c = {clamp01(c.r + spec.r), clamp01(c.g + spec.g), clamp01(c.b + spec.b), c.a};
    if (rs[d7::RS_FOGENABLE]) {
        double f = a.fog;
        if (px.table_fog) {
            const double d = px.fog_w ? 1.0 / a.clip.w : z;
            f = fog_factor(rs[d7::RS_FOGTABLEMODE], d, st);
        }
        const C4 fc = from_d3dcolor(rs[d7::RS_FOGCOLOR]);
        c = {c.r * f + fc.r * (1 - f), c.g * f + fc.g * (1 - f), c.b * f + fc.b * (1 - f), c.a};
    }
    if (rs[d7::RS_ALPHATESTENABLE] &&
        !compare(rs[d7::RS_ALPHAFUNC], double(to8(c.a)), double(rs[d7::RS_ALPHAREF] & 0xff)))
        return;
    uint32_t &dst = t.rt->color[0][size_t(y) * size_t(t.rt->w) + size_t(x)];
    C4 out = c;
    if (rs[d7::RS_ALPHABLENDENABLE]) {
        C4 d = unpack(dst);
        if (t.rt->opaque)
            d.a = 1;
        uint32_t sb = rs[d7::RS_SRCBLEND], db = rs[d7::RS_DESTBLEND];
        if (sb == d7::BLEND_BOTHSRCALPHA) {
            sb = d7::BLEND_SRCALPHA;
            db = d7::BLEND_INVSRCALPHA;
        } else if (sb == d7::BLEND_BOTHINVSRCALPHA) {
            sb = d7::BLEND_INVSRCALPHA;
            db = d7::BLEND_SRCALPHA;
        }
        out = {c.r * blend_factor(sb, c, d, 0) + d.r * blend_factor(db, c, d, 0),
               c.g * blend_factor(sb, c, d, 1) + d.g * blend_factor(db, c, d, 1),
               c.b * blend_factor(sb, c, d, 2) + d.b * blend_factor(db, c, d, 2),
               c.a * blend_factor(sb, c, d, 3) + d.a * blend_factor(db, c, d, 3)};
    }
    uint32_t packed = pack(out);
    if (t.rt->opaque)
        packed = (packed & 0x00ffffffu) | (dst & 0xff000000u);
    dst = packed;
    if (ztest && rs[d7::RS_ZWRITEENABLE])
        *zp = float(z);
}

// Pixels are sampled one subpixel step (1/256) right of and below Direct3D's
// integer centres. On an axis-aligned edge through a centre this is exactly
// the top-left rule; elsewhere it decides only exact ties, and it puts an
// attribute sampled on a vertex row or column just inside the primitive. The
// GPU renderer shifts its geometry by the same amount (d3d7_render.cpp).
const double kNudge = 1.0 / 256;

inline double snap(double v) {
    return std::floor(v * 256.0 + 0.5) / 256.0;
}
Screen to_screen(const PV &v, const HostD7State &st, bool rhw) {
    if (rhw)
        return {snap(v.clip.x), snap(v.clip.y), v.clip.z, 1.0 / v.clip.w};
    const double iw = 1.0 / v.clip.w;
    const double vx = st.viewport[0], vy = st.viewport[1], vw = st.viewport[2], vh = st.viewport[3];
    return {snap(vx + (1 + v.clip.x * iw) * vw / 2), snap(vy + (1 - v.clip.y * iw) * vh / 2),
            st.viewport_minz + v.clip.z * iw * (st.viewport_maxz - st.viewport_minz), iw};
}

// Interpolates every attribute at screen point (x, y), perspective-correctly.
PV interpolate(const PV *v, const Screen *s, double l0, double l1, double l2) {
    const double w0 = l0 * s[0].invw, w1 = l1 * s[1].invw, w2 = l2 * s[2].invw;
    const double sum = w0 + w1 + w2;
    const double a = w0 / sum, b = w1 / sum, c = w2 / sum;
    PV o;
    o.diffuse = v[0].diffuse * a + v[1].diffuse * b + v[2].diffuse * c;
    o.specular = v[0].specular * a + v[1].specular * b + v[2].specular * c;
    o.u = v[0].u * a + v[1].u * b + v[2].u * c;
    o.v = v[0].v * a + v[1].v * b + v[2].v * c;
    o.fog = v[0].fog * a + v[1].fog * b + v[2].fog * c;
    o.clip.w = sum; // 1/w, for table fog
    return o;
}

void raster_triangle(const PixelContext &px, Target &t, const PV *v, const PV &provoking,
                     bool rhw) {
    const HostD7State &st = *px.st;
    Screen s[3];
    for (int i = 0; i < 3; ++i)
        s[i] = to_screen(v[i], st, rhw);
    const double area =
        (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    if (area == 0)
        return;
    const uint32_t cull = st.rs[d7::RS_CULLMODE];
    // Clockwise on screen (y down) is area > 0: CCW culling removes area < 0.
    if ((cull == d7::CULL_CCW && area < 0) || (cull == d7::CULL_CW && area > 0))
        return;
    int order[3] = {0, 1, 2};
    if (area < 0)
        std::swap(order[1], order[2]);
    const Screen S[3] = {s[order[0]], s[order[1]], s[order[2]]};
    const PV V[3] = {v[order[0]], v[order[1]], v[order[2]]};
    const double A = std::fabs(area);
    // Edge i runs from S[i] to S[i+1]; it is opposite vertex i+2.
    auto edge = [&](int i, double x, double y) {
        const Screen &a = S[i], &b = S[(i + 1) % 3];
        return (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
    };
    auto top_left = [&](int i) {
        const double dx = S[(i + 1) % 3].x - S[i].x, dy = S[(i + 1) % 3].y - S[i].y;
        return dy < 0 || (dy == 0 && dx > 0);
    };
    const double minx = std::min({S[0].x, S[1].x, S[2].x}),
                 maxx = std::max({S[0].x, S[1].x, S[2].x});
    const double miny = std::min({S[0].y, S[1].y, S[2].y}),
                 maxy = std::max({S[0].y, S[1].y, S[2].y});
    const int x0 = std::max(t.x0, int(std::ceil(minx - kNudge))),
              x1 = std::min(t.x1 - 1, int(std::floor(maxx - kNudge)));
    const int y0 = std::max(t.y0, int(std::ceil(miny - kNudge))),
              y1 = std::min(t.y1 - 1, int(std::floor(maxy - kNudge)));
    const double tw = px.tex ? px.tex->w : 0, th = px.tex ? px.tex->h : 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            double e[3];
            bool inside = true;
            for (int i = 0; i < 3 && inside; ++i) {
                e[i] = edge(i, x + kNudge, y + kNudge);
                inside = e[i] > 0 || (e[i] == 0 && top_left(i));
            }
            if (!inside)
                continue;
            // Barycentric weight of vertex k is the edge opposite it.
            const double l0 = e[1] / A, l1 = e[2] / A, l2 = e[0] / A;
            const double z = S[0].z * l0 + S[1].z * l1 + S[2].z * l2;
            const PV a = interpolate(V, S, l0, l1, l2);
            double rho = 0;
            if (px.tex) {
                auto uv_at = [&](double qx, double qy, double *u, double *vv) {
                    const double m0 = edge(1, qx, qy) / A, m1 = edge(2, qx, qy) / A,
                                 m2 = edge(0, qx, qy) / A;
                    const PV q = interpolate(V, S, m0, m1, m2);
                    *u = q.u;
                    *vv = q.v;
                };
                // The level of detail comes from differences across the 2x2
                // pixel quad the pixel belongs to, the same for all four, as
                // a GPU takes it: x from the quad's top pair, y from its left.
                const int qx = x & ~1, qy = y & ~1;
                double u0, v0, ux, vx, uy, vy;
                uv_at(qx + kNudge, qy + kNudge, &u0, &v0);
                uv_at(qx + 1 + kNudge, qy + kNudge, &ux, &vx);
                uv_at(qx + kNudge, qy + 1 + kNudge, &uy, &vy);
                const double dux = (ux - u0) * tw, dvx = (vx - v0) * th;
                const double duy = (uy - u0) * tw, dvy = (vy - v0) * th;
                rho = std::max(std::sqrt(dux * dux + dvx * dvx), std::sqrt(duy * duy + dvy * dvy));
            }
            shade_pixel(px, t, x, y, z, a, provoking, rho);
        }
    }
}

// Lines step along their major axis from the first endpoint, which is drawn,
// to the last, which is not (the GPU's rule; Direct3D 7's LASTPIXEL, which
// would add that last pixel, is not honoured).
void raster_line(const PixelContext &px, Target &t, const PV *v, bool rhw) {
    const HostD7State &st = *px.st;
    const Screen a = to_screen(v[0], st, rhw), b = to_screen(v[1], st, rhw);
    const double dx = b.x - a.x, dy = b.y - a.y;
    const int steps = int(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    for (int i = 0; i < steps; ++i) {
        const double f = double(i) / steps;
        const int x = int(std::floor(a.x + dx * f - kNudge + 0.5)),
                  y = int(std::floor(a.y + dy * f - kNudge + 0.5));
        if (x < t.x0 || x >= t.x1 || y < t.y0 || y >= t.y1)
            continue;
        const Screen s3[3] = {a, b, b};
        const PV v3[3] = {v[0], v[1], v[1]};
        const PV p = interpolate(v3, s3, 1 - f, f, 0);
        shade_pixel(px, t, x, y, a.z + (b.z - a.z) * f, p, v[0], 0);
    }
}

void raster_point(const PixelContext &px, Target &t, const PV &v, bool rhw) {
    const Screen s = to_screen(v, *px.st, rhw);
    const int x = int(std::floor(s.x - kNudge + 0.5)), y = int(std::floor(s.y - kNudge + 0.5));
    if (x < t.x0 || x >= t.x1 || y < t.y0 || y >= t.y1)
        return;
    shade_pixel(px, t, x, y, s.z, v, v, 0);
}

void draw(const HostD7Draw *d) {
    Res *rt = find(d->target);
    if (!rt || rt->depth || !d->state)
        return;
    const HostD7State &st = *d->state;
    const d7::FvfLayout L = d7::fvf_layout(d->fvf);
    if (!L.valid)
        return;
    Target t;
    t.rt = rt;
    t.zb = d->depth ? find(d->depth) : nullptr;
    if (t.zb && (!t.zb->depth || t.zb->w < rt->w || t.zb->h < rt->h))
        t.zb = nullptr;
    t.x0 = std::max(0, st.viewport[0]);
    t.y0 = std::max(0, st.viewport[1]);
    t.x1 = std::min(rt->w, st.viewport[0] + st.viewport[2]);
    t.y1 = std::min(rt->h, st.viewport[1] + st.viewport[3]);
    if (t.x0 >= t.x1 || t.y0 >= t.y1)
        return;

    Prepared m;
    for (int i = 0; i < 16; ++i) {
        m.world[i] = st.world[i];
        m.view[i] = st.view[i];
        m.proj[i] = st.projection[i];
    }
    matmul(m.world, m.view, m.wv);
    matmul(m.wv, m.proj, m.wvp);
    normal_matrix(m.wv, m.nrm);
    const int rows[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
    for (int i = 0; i < 9; ++i)
        m.view3[i] = m.view[rows[i]];
    m.lighting = st.rs[d7::RS_LIGHTING] != 0 && !L.rhw;

    std::vector<PV> pv(d->vertex_count);
    for (uint32_t i = 0; i < d->vertex_count; ++i)
        pv[i] = process(d->vertices + size_t(i) * d->stride, L, st, m);

    PixelContext px;
    px.d = d;
    px.st = &st;
    px.tex = d->texture.id ? find(d->texture.id) : nullptr;
    if (px.tex && px.tex->depth)
        px.tex = nullptr;
    const uint32_t *ts = st.tss[0];
    px.samp.address_u = ts[d7::TSS_ADDRESSU] ? ts[d7::TSS_ADDRESSU] : d7::TADDRESS_WRAP;
    px.samp.address_v = ts[d7::TSS_ADDRESSV] ? ts[d7::TSS_ADDRESSV] : d7::TADDRESS_WRAP;
    px.samp.mag = ts[d7::TSS_MAGFILTER];
    px.samp.min = ts[d7::TSS_MINFILTER];
    px.samp.mip = ts[d7::TSS_MIPFILTER];
    float bias;
    memcpy(&bias, &ts[d7::TSS_MIPMAPLODBIAS], 4);
    px.samp.bias = bias;
    px.samp.max_level = ts[d7::TSS_MAXMIPLEVEL];
    px.samp.border = ts[d7::TSS_BORDERCOLOR];
    px.flat = st.rs[d7::RS_SHADEMODE] == d7::SHADE_FLAT;
    px.table_fog = st.rs[d7::RS_FOGTABLEMODE] != d7::FOG_NONE;
    px.fog_w = st.projection[11] != 0; // a perspective projection: fog on eye depth

    std::vector<uint32_t> list;
    const uint32_t count = d->indices ? d->index_count : d->vertex_count;
    d7::expand_primitive(d->primitive, count, list);
    auto vertex = [&](uint32_t k) -> const PV * {
        const uint32_t i = d->indices ? d->indices[k] : k;
        return i < pv.size() ? &pv[i] : nullptr;
    };
    switch (d7::topology_of(d->primitive)) {
    case d7::Topology::Triangles:
        for (size_t i = 0; i + 2 < list.size(); i += 3) {
            const PV *a = vertex(list[i]), *b = vertex(list[i + 1]), *c = vertex(list[i + 2]);
            if (!a || !b || !c)
                continue;
            if (L.rhw) {
                const PV tri[3] = {*a, *b, *c};
                raster_triangle(px, t, tri, *a, true);
                continue;
            }
            const std::vector<PV> poly = clip_polygon({*a, *b, *c});
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                const PV tri[3] = {poly[0], poly[k], poly[k + 1]};
                raster_triangle(px, t, tri, *a, false);
            }
        }
        break;
    case d7::Topology::Lines:
        for (size_t i = 0; i + 1 < list.size(); i += 2) {
            const PV *a = vertex(list[i]), *b = vertex(list[i + 1]);
            if (!a || !b)
                continue;
            if (!L.rhw && (a->clip.w <= 0 || b->clip.w <= 0))
                continue;
            const PV seg[2] = {*a, *b};
            raster_line(px, t, seg, L.rhw);
        }
        break;
    case d7::Topology::Points:
        for (uint32_t k : list) {
            const PV *a = vertex(k);
            if (a && (L.rhw || (a->clip.w > 0 && a->clip.z >= 0 && a->clip.z <= a->clip.w)))
                raster_point(px, t, *a, L.rhw);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------
int s_available() {
    return 1;
}
uint32_t s_generation() {
    return g_generation;
}
int s_create(uint32_t id, int w, int h, int levels, uint32_t flags) {
    if (w <= 0 || h <= 0 || levels < 1)
        return 0;
    Res r;
    r.w = w;
    r.h = h;
    r.levels = levels;
    r.depth = (flags & HOST_D7_CREATE_DEPTH) != 0;
    r.opaque = (flags & HOST_D7_CREATE_OPAQUE) != 0;
    if (r.depth) {
        r.z.assign(size_t(w) * size_t(h), 1.0f);
        r.lw.push_back(w);
        r.lh.push_back(h);
    } else {
        for (int i = 0; i < levels; ++i) {
            const int lw = std::max(1, w >> i), lh = std::max(1, h >> i);
            r.lw.push_back(lw);
            r.lh.push_back(lh);
            r.color.emplace_back(size_t(lw) * size_t(lh), r.opaque ? 0xff000000u : 0u);
        }
    }
    resources()[id] = std::move(r);
    return 1;
}
void s_destroy(uint32_t id) {
    resources().erase(id);
}
bool level_rect(const Res &r, int level, const HostD7Rect *rect) {
    return !r.depth && level >= 0 && level < r.levels && rect && rect->x0 >= 0 && rect->y0 >= 0 &&
           rect->x1 <= r.lw[size_t(level)] && rect->y1 <= r.lh[size_t(level)] &&
           rect->x0 < rect->x1 && rect->y0 < rect->y1;
}
void s_upload(uint32_t id, int level, const HostD7Rect *rect, const void *bgra, int pitch) {
    Res *r = find(id);
    if (!r || !level_rect(*r, level, rect))
        return;
    const int lw = r->lw[size_t(level)];
    for (int y = rect->y0; y < rect->y1; ++y) {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(
            static_cast<const uint8_t *>(bgra) + size_t(y - rect->y0) * size_t(pitch));
        uint32_t *dst = r->color[size_t(level)].data() + size_t(y) * size_t(lw) + size_t(rect->x0);
        for (int x = 0; x < rect->x1 - rect->x0; ++x)
            dst[x] = r->opaque ? (src[x] | 0xff000000u) : src[x];
    }
}
int s_readback(uint32_t id, int level, const HostD7Rect *rect, void *bgra, int pitch) {
    Res *r = find(id);
    if (!r || !level_rect(*r, level, rect))
        return 0;
    const int lw = r->lw[size_t(level)];
    for (int y = rect->y0; y < rect->y1; ++y)
        memcpy(static_cast<uint8_t *>(bgra) + size_t(y - rect->y0) * size_t(pitch),
               r->color[size_t(level)].data() + size_t(y) * size_t(lw) + size_t(rect->x0),
               size_t(rect->x1 - rect->x0) * 4);
    return 1;
}
void s_fill(uint32_t id, const HostD7Rect *rect, uint32_t bgra) {
    Res *r = find(id);
    if (!r || !level_rect(*r, 0, rect))
        return;
    if (r->opaque)
        bgra |= 0xff000000u;
    for (int y = rect->y0; y < rect->y1; ++y)
        for (int x = rect->x0; x < rect->x1; ++x)
            r->color[0][size_t(y) * size_t(r->w) + size_t(x)] = bgra;
}
void s_fill_depth(uint32_t id, const HostD7Rect *rect, float z) {
    Res *r = find(id);
    if (!r || !r->depth || !rect)
        return;
    for (int y = std::max(0, rect->y0); y < std::min(r->h, rect->y1); ++y)
        for (int x = std::max(0, rect->x0); x < std::min(r->w, rect->x1); ++x)
            r->z[size_t(y) * size_t(r->w) + size_t(x)] = z;
}
// Copy or stretch with the same source mapping as the CPU path: destination
// pixel x samples source floor((x + 0.5) * sw / dw).
void s_blit(const HostD7Blit *b) {
    Res *d = find(b->dst), *s = find(b->src);
    if (!d || !s || d->depth || s->depth || !level_rect(*d, b->dst_level, &b->dst_rect) ||
        !level_rect(*s, b->src_level, &b->src_rect))
        return;
    const HostD7Rect &dr = b->dst_rect, &sr = b->src_rect;
    const int dw = dr.x1 - dr.x0, dh = dr.y1 - dr.y0, sw = sr.x1 - sr.x0, sh = sr.y1 - sr.y0;
    const int slw = s->lw[size_t(b->src_level)], dlw = d->lw[size_t(b->dst_level)];
    const std::vector<uint32_t> src = s->color[size_t(b->src_level)]; // overlap-safe
    for (int y = 0; y < dh; ++y) {
        int sy = int((int64_t(2 * y + 1) * sh) / (2 * dh));
        if (b->mirror_y)
            sy = sh - 1 - sy;
        for (int x = 0; x < dw; ++x) {
            int sx = int((int64_t(2 * x + 1) * sw) / (2 * dw));
            if (b->mirror_x)
                sx = sw - 1 - sx;
            uint32_t p = src[size_t(sr.y0 + sy) * size_t(slw) + size_t(sr.x0 + sx)];
            if (b->key.enabled) {
                const uint32_t ch[3] = {(p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff};
                bool match = true;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t max = (1u << b->key.bits[i]) - 1;
                    const uint32_t q = (ch[i] * max + 127) / 255;
                    match = match && q >= b->key.lo[i] && q <= b->key.hi[i];
                }
                if (match)
                    continue;
            }
            if (!b->src_has_alpha)
                p |= 0xff000000u;
            uint32_t &out =
                d->color[size_t(b->dst_level)][size_t(dr.y0 + y) * size_t(dlw) + size_t(dr.x0 + x)];
            out = d->opaque ? ((p & 0x00ffffffu) | (out & 0xff000000u)) : p;
        }
    }
}
void s_draw(const HostD7Draw *d) {
    draw(d);
}
void s_present(uint32_t id, const HostD7Rect *rect, int, int) {
    Res *r = find(id);
    if (!r || !level_rect(*r, 0, rect))
        return;
    const int w = rect->x1 - rect->x0, h = rect->y1 - rect->y0;
    std::vector<uint32_t> argb(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            argb[size_t(y) * size_t(w) + size_t(x)] =
                r->color[0][size_t(rect->y0 + y) * size_t(r->w) + size_t(rect->x0 + x)] |
                0xff000000u;
    display_present_frame(argb.data(), w, h);
}
void s_flush() {}

const d7::Backend g_soft = {"software", s_available, s_generation, s_create,     s_destroy,
                            s_upload,   s_readback,  s_fill,       s_fill_depth, s_blit,
                            s_draw,     s_present,   s_flush};
} // namespace

const uint32_t *pixels(uint32_t id, int level, int *w, int *h) {
    Res *r = find(id);
    if (!r || r->depth || level < 0 || level >= r->levels)
        return nullptr;
    *w = r->lw[size_t(level)];
    *h = r->lh[size_t(level)];
    return r->color[size_t(level)].data();
}
const float *depth(uint32_t id, int *w, int *h) {
    Res *r = find(id);
    if (!r || !r->depth)
        return nullptr;
    *w = r->w;
    *h = r->h;
    return r->z.data();
}
void reset() {
    resources().clear();
    ++g_generation;
}
} // namespace d7soft

namespace d7 {
const Backend *soft_backend() {
    return &d7soft::g_soft;
}
} // namespace d7
