// shaders_msl.h - the MSL behind every program gpu/shaders.md names. Moved
// verbatim from compositor.mm, performance_overlay.h and d3d_render.mm; the
// slot numbers are the ones shaders.md documents.
#pragma once

namespace gpu::metal {

// "compositor": compositor_vertex / compositor_fragment
inline const char *const kCompositorSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 rect; float4 uv; float2 drawable; uint opaque; uint pad; };
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying compositor_vertex(uint id [[vertex_id]], constant Quad& q [[buffer(0)]]) {
    const float2 corners[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 p=q.rect.xy+corners[id]*q.rect.zw;
    Varying v;
    v.position=float4(p.x/q.drawable.x*2-1,1-p.y/q.drawable.y*2,0,1);
    v.uv=q.uv.xy+corners[id]*q.uv.zw;
    return v;
}
fragment float4 compositor_fragment(Varying v [[stage_in]],
                                    constant Quad& q [[buffer(0)]],
                                    texture2d<float> tex [[texture(0)]]) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    float4 c=tex.sample(nearest,v.uv);
    if(q.opaque) c.a=1;
    return c;
}
)MSL";

// "hud": hud_v / hud_f
inline const char *const kHudSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V hud_v(uint i [[vertex_id]], constant float4& r [[buffer(0)]]) {
    float2 q = float2(i & 1, i >> 1);
    V v; v.p = float4(r.xy + q * r.zw, 0, 1); v.uv = q; return v;
}
fragment float4 hud_f(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::linear);
    return t.sample(s, v.uv);
}
)MSL";

// "d3d", "surface_upload", "guest_readback", "guest_readback_fused", "native_brightness"
inline const char *const kD3DSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct HVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
    float sr, sg, sb, sa;
};
struct Uniforms {
    float4x4 mvp;
    uint  pretransformed;
    uint  textured;
    uint  texblend;
    uint  alphatest;
    uint  alphafunc;
    float alpharef;
    uint  specular;
    uint  texture_has_alpha;
    uint  fogmode;        // 0 off, 1 vertex, 2 exp, 3 exp2, 4 linear
    float fogstart, fogend, fogdensity;
    float fogr, fogg, fogb;
    float pointsize;
    uint terrain_detail;
};
struct VOut {
    float4 position [[position]];
    float  size [[point_size]];
    float2 uv;
    float4 color;
    float4 spec;
    float  fogdist;
};
// Colour, and the coverage mask that says this pixel was rasterized. A
// discarded fragment writes neither.
struct FOut {
    float4 color    [[color(0)]];
    float  coverage [[color(1)]];
};

vertex VOut d3d_vertex(uint vid [[vertex_id]],
                       const device HVertex* v [[buffer(0)]],
                       constant Uniforms& u [[buffer(1)]]) {
    HVertex s = v[vid];
    VOut o;
    o.position = u.pretransformed ? float4(s.x, s.y, s.z, s.w)
                                  : u.mvp * float4(s.x, s.y, s.z, 1.0);
    o.size = u.pointsize;
    o.uv = float2(s.u, s.v);
    o.color = float4(s.r, s.g, s.b, s.a);
    o.spec = float4(s.sr, s.sg, s.sb, s.sa);
    // Table fog is a function of eye-space distance, which after a standard
    // projection is the clip w.
    o.fogdist = abs(o.position.w);
    return o;
}

// Shade a world fragment using the guest material and optional terrain detail.
// Apply the legacy texture/alpha rules before writing color and coverage attachments.
fragment FOut d3d_fragment(VOut in [[stage_in]],
                           constant Uniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]],
                           texture2d<float> detail [[texture(1)]],
                           sampler detail_samp [[sampler(1)]]) {
    float4 c = in.color;
    if (u.textured != 0) {
        float4 t = tex.sample(samp, in.uv);
        if (u.terrain_detail != 0) {
            // An independent material sample supplies real fine structures;
            // enlarging the guest's 16/32 texels cannot create those. The
            // source remains the color/lighting/coastline mask. Blue water
            // fades this land-only layer out, including mixed shore texels.
            float3 chroma = t.rgb / max(max(t.r, t.g), max(t.b, 0.01f));
            float land = 1.0f - smoothstep(0.02f, 0.22f, chroma.b - chroma.r);
            float grass = smoothstep(-0.03f, 0.16f, chroma.g - chroma.r);
            float stone = 1.0f - smoothstep(0.06f, 0.28f, max(chroma.r, chroma.g) - min(chroma.r, min(chroma.g, chroma.b)));
            float sand = smoothstep(0.28f, 0.62f, max(t.r, t.g));
            float4 structure = (detail.sample(detail_samp, in.uv) * 255.0f - 128.0f) / 128.0f;
            float material = mix(structure.a, structure.g, sand);
            material = mix(material, structure.b, stone);
            material = mix(material, structure.r, grass);
            t.rgb = saturate(t.rgb * (1.0f + material * 0.85f * land));
        }
        switch (u.texblend) {
            case 1: case 7:                                   // DECAL, COPY
                c = t; break;
            case 2:                                           // MODULATE
                // The legacy rule: the texture's alpha when it has one,
                // otherwise the diffuse's. Taking the vertex alpha from a
                // vertex that never set it - and plenty of D3DTLVERTEX
                // colours leave it at zero - makes every blended pixel
                // invisible and the whole scene the colour of the clear.
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
            case 3:                                           // DECALALPHA
                c = float4(mix(in.color.rgb, t.rgb, t.a), in.color.a); break;
            case 5:                                           // DECALMASK
                c = float4(t.rgb, in.color.a); break;
            case 8:                                           // ADD
                c = float4(saturate(t.rgb + in.color.rgb), in.color.a); break;
            case 4:                                           // MODULATEALPHA
                c = float4(t.rgb * in.color.rgb, t.a * in.color.a); break;
            default:
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
        }
    }
    if (u.specular != 0) c = float4(saturate(c.rgb + in.spec.rgb), c.a);
    if (u.alphatest != 0) {
        bool pass;
        switch (u.alphafunc) {
            case 1: pass = false;                break;   // NEVER
            case 2: pass = c.a <  u.alpharef;    break;   // LESS
            case 3: pass = c.a == u.alpharef;    break;   // EQUAL
            case 4: pass = c.a <= u.alpharef;    break;   // LESSEQUAL
            case 5: pass = c.a >  u.alpharef;    break;   // GREATER
            case 6: pass = c.a != u.alpharef;    break;   // NOTEQUAL
            case 7: pass = c.a >= u.alpharef;    break;   // GREATEREQUAL
            default: pass = true;                break;   // ALWAYS
        }
        if (!pass) discard_fragment();
    }
    if (u.fogmode != 0) {
        // The fog factor is how much of the fragment survives: 1 is clear.
        float f = 1.0;
        float d = in.fogdist;
        switch (u.fogmode) {
            case 1: f = in.spec.a; break;                          // vertex fog
            case 2: f = exp(-u.fogdensity * d); break;             // EXP
            case 3: f = exp(-(u.fogdensity * d) * (u.fogdensity * d)); break;
            default:                                               // LINEAR
                f = (u.fogend - d) / max(u.fogend - u.fogstart, 1e-6);
                break;
        }
        f = saturate(f);
        c = float4(mix(float3(u.fogr, u.fogg, u.fogb), c.rgb, f), c.a);
    }
    FOut o;
    o.color = c;
    o.coverage = 1.0;
    return o;
}
// Reduce guest-visible reads on the GPU; never decompress a 4K texture on
// the CPU merely to select 640x480 pixel centres. Each lane also counts its
// disjoint native cell so existing full-resolution diagnostics stay exact.
kernel void guest_readback_fused(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    uint2 lo=(guest*native_size+guest_size-1)/guest_size;
    uint2 hi=((guest+1)*native_size+guest_size-1)/guest_size;
    uint lit=0;
    for(uint y=lo.y;y<hi.y;++y) for(uint x=lo.x;x<hi.x;++x)
        lit+=any(round(color.read(uint2(x,y)).rgb*255.0f)>8.0f) ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),lit);
}
// Guest sampling and native brightness reduction have different grids.
// Keeping the sampling lanes short avoids a variable native-cell loop in
// every guest lane. Packing and coverage rules match the fused reference.
kernel void guest_readback(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),0);
}
// A 16x16 threadgroup covers a 32x32 native tile. Each lane reads four
// adjacent pixels and each SIMD group writes one sum. Padded lanes contribute
// zero but participate in the reduction; no contended global atomic is used.
kernel void native_brightness(texture2d<float, access::read> color [[texture(0)]],
                              device uint* output [[buffer(0)]],
                              constant uint4* p [[buffer(1)]],
                              uint2 tid [[thread_position_in_grid]],
                              uint2 group [[threadgroup_position_in_grid]],
                              uint lane [[thread_index_in_simdgroup]],
                              uint simd_group [[simdgroup_index_in_threadgroup]]) {
    uint2 pos=p[0].xy+tid*2;
    uint lit=0;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        uint2 q=pos+uint2(x,y);
        if(all(q<p[0].zw)) lit+=any(round(color.read(q).rgb*255.0f)>8.0f) ? 1:0;
    }
    uint sum=simd_sum(lit);
    if(lane==0) output[p[1].x+(group.y*p[1].y+group.x)*p[1].z+simd_group]=sum;
}
// Seed a native target directly from an immutable guest-sized buffer. Integer
// channel expansion and floor(x * guest / native) preserve the CPU upload's
// exact pixels without expanding the whole 4K image on the guest thread.
vertex float4 surface_upload_vertex(uint id [[vertex_id]]) {
    float2 corner=float2((id<<1)&2,id&2);
    return float4(corner*2.0f-1.0f,0,1);
}
fragment float4 surface_upload_fragment(float4 position [[position]],
    device const uchar* pixels [[buffer(0)]],
    constant uint4* p [[buffer(1)]], constant uint* palette [[buffer(2)]]) {
    uint2 xy=uint2(position.xy)*p[0].xy/p[0].zw;
    uint offset=xy.y*p[1].x+xy.x*(p[1].y==8 ? 1:(p[1].y<=16 ? 2:4));
    uint3 rgb;
    if(p[1].y==8) {
        uint index=pixels[offset];
        uint c=p[1].z ? palette[index] : index*0x010101u;
        rgb=uint3((c>>16)&255,(c>>8)&255,c&255);
    } else {
        uint value=uint(pixels[offset])|(uint(pixels[offset+1])<<8);
        if(p[1].y>16) value|=(uint(pixels[offset+2])<<16)|(uint(pixels[offset+3])<<24);
        rgb=((uint3(value)&p[2].xyz)>>p[3].xyz)*255/p[4].xyz;
    }
    return float4(float3(rgb)/255.0f,1);
}
)MSL";

// "d3d7": d3d7_vertex / d3d7_fragment, the Direct3D 7 fixed-function
// pipeline (host/d3d7_render.cpp). "d3d7_quad": d3d7_quad_vertex /
// d3d7_quad_fragment, DirectDraw fills and blits. Layouts: d3d7_render.cpp's
// VS7, FS7 and Q7, which this source must match field for field.
inline const char *const kD3D7Source = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Light7 {
    float4 diffuse, specular, ambient;
    float4 position;  // xyz eye space; w: D3DLIGHTTYPE
    float4 direction; // xyz eye space: towards a directional light, a spot's axis; w: range
    float4 atten;     // attenuation 0, 1, 2; falloff
    float4 spot;      // cos(theta / 2), cos(phi / 2)
};
struct VS7 {
    float4x4 wvp;
    float4x4 wv;
    float4 nrm[3]; // rows of the inverse transpose of the world-view 3x3
    float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
    float4 ambient;
    float4 ndc;      // xy scale, zw offset: Direct3D NDC to this pass's NDC
    float4 viewport; // Direct3D viewport x, y, w, h, for pre-transformed vertices
    float4 fog;      // start, end, density, material power
    uint flags, offsets, stride, light_count;
    uint sources, fog_mode, pad0, pad1;
    Light7 lights[8];
};
struct V7 {
    float4 position [[position]];
    float point_size [[point_size]];
    float4 diffuse;
    float4 specular;
    float4 diffuse_flat [[flat]];
    float4 specular_flat [[flat]];
    float2 uv;
    float fog;
    float eye_w;
};
constant uint F_LIGHTING = 1u, F_NORMAL = 2u, F_DIFFUSE = 4u, F_SPECULAR = 8u, F_TEX = 16u,
              F_RHW = 32u, F_SPECULARENABLE = 64u, F_LOCALVIEWER = 128u, F_NORMALIZE = 256u,
              F_COLORVERTEX = 512u, F_RANGEFOG = 1024u, F_VERTEXFOG = 2048u, F_TEX1D = 4096u;

static float4 d3dcolor(uint c) {
    return float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0f;
}
static float ldf(device const uchar *p, uint off) {
    return *(device const float *)(p + off);
}
static float4 pick(uint source, float4 material, bool has_diffuse, float4 vdiffuse,
                   bool has_specular, float4 vspecular) {
    if (source == 1u && has_diffuse) return vdiffuse;
    if (source == 2u && has_specular) return vspecular;
    return material;
}
static float fog_of(uint mode, float d, float4 fog) {
    if (mode == 3u) return fog.y == fog.x ? (d < fog.x ? 1.0f : 0.0f) : saturate((fog.y - d) / (fog.y - fog.x));
    if (mode == 1u) return saturate(exp(-fog.z * d));
    if (mode == 2u) return saturate(exp(-(fog.z * d) * (fog.z * d)));
    return 1.0f;
}

vertex V7 d3d7_vertex(uint vid [[vertex_id]], device const uchar *vb [[buffer(0)]],
                      constant VS7 &u [[buffer(1)]], device const uint *list [[buffer(2)]]) {
    device const uchar *v = vb + list[vid] * u.stride;
    const uint o_normal = u.offsets & 255u, o_diffuse = (u.offsets >> 8) & 255u,
               o_specular = (u.offsets >> 16) & 255u, o_tex = u.offsets >> 24;
    const bool has_diffuse = (u.flags & F_DIFFUSE) != 0u, has_specular = (u.flags & F_SPECULAR) != 0u;
    const float4 vdiffuse = has_diffuse ? d3dcolor(*(device const uint *)(v + o_diffuse)) : float4(1.0f);
    const float4 vspecular = has_specular ? d3dcolor(*(device const uint *)(v + o_specular)) : float4(0.0f);
    V7 o;
    o.point_size = 1.0f;
    o.uv = float2(0.0f);
    if ((u.flags & F_TEX) != 0u)
        o.uv = float2(ldf(v, o_tex), (u.flags & F_TEX1D) != 0u ? 0.0f : ldf(v, o_tex + 4u));
    float4 clip;
    if ((u.flags & F_RHW) != 0u) {
        const float rhw = ldf(v, 12u);
        const float w = rhw != 0.0f ? 1.0f / rhw : 1.0f;
        const float2 ndc = float2((ldf(v, 0u) - u.viewport.x) / u.viewport.z * 2.0f - 1.0f,
                                  1.0f - (ldf(v, 4u) - u.viewport.y) / u.viewport.w * 2.0f);
        clip = float4(ndc * w, ldf(v, 8u) * w, w);
        o.diffuse = vdiffuse;
        o.specular = vspecular;
        o.fog = vspecular.a;
        o.eye_w = w;
    } else {
        const float4 p = float4(ldf(v, 0u), ldf(v, 4u), ldf(v, 8u), 1.0f);
        clip = u.wvp * p;
        const float3 pe = (u.wv * p).xyz;
        o.eye_w = clip.w;
        if ((u.flags & F_LIGHTING) == 0u) {
            o.diffuse = vdiffuse;
            o.specular = vspecular;
        } else {
            const bool cv = (u.flags & F_COLORVERTEX) != 0u;
            const float4 cd = cv ? pick(u.sources & 3u, u.mat_diffuse, has_diffuse, vdiffuse, has_specular, vspecular) : u.mat_diffuse;
            const float4 ca = cv ? pick((u.sources >> 2) & 3u, u.mat_ambient, has_diffuse, vdiffuse, has_specular, vspecular) : u.mat_ambient;
            const float4 cs = cv ? pick((u.sources >> 4) & 3u, u.mat_specular, has_diffuse, vdiffuse, has_specular, vspecular) : u.mat_specular;
            const float4 ce = cv ? pick((u.sources >> 6) & 3u, u.mat_emissive, has_diffuse, vdiffuse, has_specular, vspecular) : u.mat_emissive;
            float3 n = float3(0.0f);
            if ((u.flags & F_NORMAL) != 0u) {
                const float3 m = float3(ldf(v, o_normal), ldf(v, o_normal + 4u), ldf(v, o_normal + 8u));
                n = m.x * u.nrm[0].xyz + m.y * u.nrm[1].xyz + m.z * u.nrm[2].xyz;
                if ((u.flags & F_NORMALIZE) != 0u && dot(n, n) > 0.0f)
                    n = normalize(n);
            }
            const float3 eye = (u.flags & F_LOCALVIEWER) != 0u ? normalize(-pe) : float3(0.0f, 0.0f, -1.0f);
            float3 amb = u.ambient.rgb, dif = float3(0.0f), spc = float3(0.0f);
            for (uint i = 0u; i < u.light_count; ++i) {
                const Light7 l = u.lights[i];
                float3 L;
                float k = 1.0f;
                if (l.position.w == 3.0f) {
                    L = l.direction.xyz;
                } else {
                    const float3 d = l.position.xyz - pe;
                    const float dist = length(d);
                    if (dist > l.direction.w)
                        continue;
                    L = dist > 0.0f ? d / dist : float3(0.0f);
                    const float den = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
                    k = den > 0.0f ? 1.0f / den : 1.0f;
                    if (l.position.w == 2.0f) {
                        const float rho = -dot(l.direction.xyz, L);
                        k *= rho > l.spot.x ? 1.0f
                             : rho <= l.spot.y ? 0.0f
                             : pow((rho - l.spot.y) / (l.spot.x - l.spot.y), l.atten.w);
                    }
                }
                amb += l.ambient.rgb * k;
                const float ndl = max(0.0f, dot(n, L));
                dif += l.diffuse.rgb * (ndl * k);
                if ((u.flags & F_SPECULARENABLE) != 0u && ndl > 0.0f) {
                    const float ndh = max(0.0f, dot(n, normalize(L + eye)));
                    // pow(0, 0) is 1, as Direct3D's specular power 0 means.
                    spc += l.specular.rgb * ((u.fog.w == 0.0f ? 1.0f : pow(ndh, u.fog.w)) * k);
                }
            }
            o.diffuse = saturate(float4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));
            o.specular = saturate(float4(cs.rgb * spc, 0.0f));
        }
        if ((u.flags & F_VERTEXFOG) != 0u)
            o.fog = fog_of(u.fog_mode, (u.flags & F_RANGEFOG) != 0u ? length(pe) : pe.z, u.fog);
        else
            o.fog = o.specular.a;
    }
    // Into this pass's viewport, half a pixel down and right of Direct3D's
    // pixel-centre convention (folded into ndc.zw by the renderer).
    clip.xy = clip.xy * u.ndc.xy + u.ndc.zw * clip.w;
    o.position = clip;
    o.diffuse_flat = o.diffuse;
    o.specular_flat = o.specular;
    return o;
}

struct FS7 {
    float4 fog_color;
    float4 tfactor;
    float4 fog; // start, end, density
    uint colorop, colorarg1, colorarg2, alphaop;
    uint alphaarg1, alphaarg2, textured, tex_has_alpha;
    uint alpha_func, alpha_ref, fog_enable, table_fog;
    uint fog_w, flat, specular, colorkey;
    uint4 key_max, key_lo, key_hi;
    float lod_bias, min_lod, pad0, pad1;
};
static bool cmp(uint f, float a, float b) {
    switch (f) {
    case 1u: return false;
    case 2u: return a < b;
    case 3u: return a == b;
    case 4u: return a <= b;
    case 5u: return a > b;
    case 6u: return a != b;
    case 7u: return a >= b;
    default: return true;
    }
}
static float4 arg(uint a, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 spec) {
    float4 v;
    switch (a & 15u) {
    case 1u: v = current; break;
    case 2u: v = tex; break;
    case 3u: v = tfactor; break;
    case 4u: v = spec; break;
    default: v = diffuse; break;
    }
    if ((a & 16u) != 0u) v = 1.0f - v;
    if ((a & 32u) != 0u) v = v.aaaa;
    return v;
}
static float4 op(uint o, float4 a1, float4 a2, float4 diffuse, float4 tex, float4 tfactor, float4 current) {
    switch (o) {
    case 2u: return a1;
    case 3u: return a2;
    case 4u: return a1 * a2;
    case 5u: return saturate(a1 * a2 * 2.0f);
    case 6u: return saturate(a1 * a2 * 4.0f);
    case 7u: return saturate(a1 + a2);
    case 8u: return saturate(a1 + a2 - 0.5f);
    case 9u: return saturate((a1 + a2 - 0.5f) * 2.0f);
    case 10u: return saturate(a1 - a2);
    case 11u: return saturate(a1 + a2 - a1 * a2);
    case 12u: return a1 * diffuse.a + a2 * (1.0f - diffuse.a);
    case 13u: return a1 * tex.a + a2 * (1.0f - tex.a);
    case 14u: return a1 * tfactor.a + a2 * (1.0f - tfactor.a);
    case 15u: return saturate(a1 + a2 * (1.0f - tex.a));
    case 16u: return a1 * current.a + a2 * (1.0f - current.a);
    default: return a1 * a2;
    }
}
fragment float4 d3d7_fragment(V7 in [[stage_in]], constant FS7 &u [[buffer(1)]],
                              texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]],
                              sampler nearest [[sampler(1)]]) {
    const float4 diffuse = u.flat != 0u ? in.diffuse_flat : in.diffuse;
    const float4 spec = u.flat != 0u ? in.specular_flat : in.specular;
    float4 t = float4(1.0f);
    if (u.textured != 0u) {
        t = u.min_lod > 0.0f ? tex.sample(s, in.uv, bias(u.lod_bias), min_lod_clamp(u.min_lod))
                             : tex.sample(s, in.uv, bias(u.lod_bias));
        if (u.tex_has_alpha == 0u)
            t.a = 1.0f;
        if (u.colorkey != 0u) {
            const float4 k = tex.sample(nearest, in.uv, level(0.0f));
            const uint3 c8 = uint3(round(saturate(k.rgb) * 255.0f));
            const uint3 q = (c8 * u.key_max.xyz + 127u) / 255u;
            if (all(q >= u.key_lo.xyz) && all(q <= u.key_hi.xyz))
                discard_fragment();
        }
    }
    const float4 tfactor = u.tfactor;
    float4 c = diffuse;
    if (u.colorop != 1u) {
        const float4 current = diffuse;
        const float3 rgb = op(u.colorop, arg(u.colorarg1, diffuse, current, t, tfactor, spec),
                              arg(u.colorarg2, diffuse, current, t, tfactor, spec), diffuse, t, tfactor, current).rgb;
        float a = current.a;
        if (u.alphaop != 1u)
            a = op(u.alphaop, arg(u.alphaarg1, diffuse, current, t, tfactor, spec),
                   arg(u.alphaarg2, diffuse, current, t, tfactor, spec), diffuse, t, tfactor, current).a;
        c = float4(rgb, a);
    }
    c = saturate(c);
    if (u.specular != 0u)
        c.rgb = saturate(c.rgb + spec.rgb);
    if (u.fog_enable != 0u) {
        float f = in.fog;
        if (u.table_fog != 0u)
            f = fog_of(u.table_fog, u.fog_w != 0u ? in.eye_w : in.position.z, u.fog);
        c.rgb = c.rgb * f + u.fog_color.rgb * (1.0f - f);
    }
    if (u.alpha_func != 0u) {
        const float a8 = floor(saturate(c.a) * 255.0f + 0.5f);
        if (!cmp(u.alpha_func, a8, float(u.alpha_ref)))
            discard_fragment();
    }
    return c;
}

struct Q7 {
    float4 dst;   // x0, y0, x1, y1 in target pixels
    int4 src;     // texel rectangle of the source level
    float4 color; // fill colour
    float2 target;
    float depth;
    uint mode;    // 0 nearest copy, 1 colour fill, 2 depth fill, 3 bilinear copy
    uint level, key_enabled, src_has_alpha, mirror;
    uint4 key_max, key_lo, key_hi;
};
vertex float4 d3d7_quad_vertex(uint vid [[vertex_id]], constant Q7 &q [[buffer(0)]]) {
    const float2 corner = float2(float(vid & 1u), float(vid >> 1));
    const float2 p = mix(q.dst.xy, q.dst.zw, corner);
    return float4(p.x / q.target.x * 2.0f - 1.0f, 1.0f - p.y / q.target.y * 2.0f, q.depth, 1.0f);
}
fragment float4 d3d7_quad_fragment(float4 pos [[position]], constant Q7 &q [[buffer(0)]],
                                   texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    if (q.mode == 1u || q.mode == 2u)
        return q.color;
    const int2 d = int2(floor(pos.xy)) - int2(q.dst.xy);
    const int2 dsz = int2(q.dst.zw - q.dst.xy), ssz = q.src.zw - q.src.xy;
    float4 c;
    if (q.mode == 0u) {
        int2 idx = ((2 * d + 1) * ssz) / (2 * dsz);
        if ((q.mirror & 1u) != 0u) idx.x = ssz.x - 1 - idx.x;
        if ((q.mirror & 2u) != 0u) idx.y = ssz.y - 1 - idx.y;
        c = tex.read(uint2(q.src.xy + idx), q.level);
    } else {
        const float2 size = float2(float(tex.get_width(q.level)), float(tex.get_height(q.level)));
        const float2 uv = (float2(q.src.xy) + (float2(d) + 0.5f) * float2(ssz) / float2(dsz)) / size;
        c = tex.sample(s, uv, level(float(q.level)));
    }
    if (q.key_enabled != 0u) {
        const uint3 c8 = uint3(round(saturate(c.rgb) * 255.0f));
        const uint3 k = (c8 * q.key_max.xyz + 127u) / 255u;
        if (all(k >= q.key_lo.xyz) && all(k <= q.key_hi.xyz))
            discard_fragment();
    }
    if (q.src_has_alpha == 0u)
        c.a = 1.0f;
    return c;
}
)MSL";

} // namespace gpu::metal
