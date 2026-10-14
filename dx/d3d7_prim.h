// d3d7_prim.h - Direct3D 7 primitive topologies as independent lists.
//
// Strips and fans become lists so that every renderer draws only lists, and
// so that each triangle's FIRST index is the vertex flat shading takes its
// colour from: the first vertex of the triangle in strip order, and the
// second vertex of a fan triangle. Reordering a triangle's vertices
// cyclically keeps its winding, which is what culling looks at.
#pragma once
#include <stdint.h>
#include <vector>

namespace d7 {

enum class Topology { Points, Lines, Triangles, None };

inline Topology topology_of(uint32_t primitive) {
    switch (primitive) {
    case 1:
        return Topology::Points;
    case 2:
    case 3:
        return Topology::Lines;
    case 4:
    case 5:
    case 6:
        return Topology::Triangles;
    default:
        return Topology::None;
    }
}

// Appends the expanded list for `count` vertices (or indices) of `primitive`
// to `out`, as positions 0..count-1 in the draw's own order.
inline void expand_primitive(uint32_t primitive, uint32_t count, std::vector<uint32_t> &out) {
    switch (primitive) {
    case 1: // POINTLIST
        for (uint32_t i = 0; i < count; ++i)
            out.push_back(i);
        break;
    case 2: // LINELIST
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            out.push_back(i);
            out.push_back(i + 1);
        }
        break;
    case 3: // LINESTRIP
        for (uint32_t i = 0; i + 1 < count; ++i) {
            out.push_back(i);
            out.push_back(i + 1);
        }
        break;
    case 4: // TRIANGLELIST
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            out.push_back(i);
            out.push_back(i + 1);
            out.push_back(i + 2);
        }
        break;
    case 5: // TRIANGLESTRIP: odd triangles reverse their winding
        for (uint32_t i = 0; i + 2 < count; ++i) {
            out.push_back(i);
            out.push_back((i & 1) ? i + 2 : i + 1);
            out.push_back((i & 1) ? i + 1 : i + 2);
        }
        break;
    case 6: // TRIANGLEFAN
        for (uint32_t i = 1; i + 1 < count; ++i) {
            out.push_back(i);
            out.push_back(i + 1);
            out.push_back(0);
        }
        break;
    default:
        break;
    }
}

// Vertex layout of a flexible vertex format. Offsets in bytes; -1 when absent.
struct FvfLayout {
    bool valid = false;
    bool rhw = false; // pre-transformed: x, y in pixels, z in 0..1, then 1/w
    int position = 0, normal = -1, diffuse = -1, specular = -1;
    int texcoord[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int texcoord_size[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // floats
    int tex_count = 0;
    uint32_t stride = 0;
};
inline FvfLayout fvf_layout(uint32_t fvf) {
    FvfLayout l;
    uint32_t off = 0;
    switch (fvf & 0x00e) {
    case 0x002: // XYZ
        off = 12;
        break;
    case 0x004: // XYZRHW
        off = 16;
        l.rhw = true;
        break;
    case 0x006:
    case 0x008:
    case 0x00a:
    case 0x00c:
    case 0x00e: // XYZB1..5: the blend weights are skipped, vertex blending is off
        off = 12 + 4 * (((fvf & 0x00e) - 0x004) / 2);
        break;
    default:
        return l;
    }
    if (fvf & 0x010) {
        l.normal = int(off);
        off += 12;
    }
    if (fvf & 0x020) // D3DFVF_RESERVED1
        off += 4;
    if (fvf & 0x040) {
        l.diffuse = int(off);
        off += 4;
    }
    if (fvf & 0x080) {
        l.specular = int(off);
        off += 4;
    }
    l.tex_count = int((fvf >> 8) & 0xf);
    if (l.tex_count > 8)
        return l;
    for (int i = 0; i < l.tex_count; ++i) {
        static const int sizes[4] = {2, 3, 4, 1};
        const int n = sizes[(fvf >> (16 + 2 * i)) & 3];
        l.texcoord[i] = int(off);
        l.texcoord_size[i] = n;
        off += uint32_t(4 * n);
    }
    l.stride = off;
    l.valid = true;
    return l;
}

} // namespace d7
