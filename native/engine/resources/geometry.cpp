#include "resources/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>

namespace sspc::res {
namespace {

constexpr u32 kMaxStreams = 16;
constexpr u32 kMaxIndices = 1u << 24;

bool ReadVec3(const KcapPackage& pkg, u32 off, Vec3& v) {
    float f[3];
    if (!pkg.Read(off, f)) return false;
    v = {f[0], f[1], f[2]};
    return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
}

}  // namespace

std::optional<RootGeometryList> ReadRootGeometryList(const KcapPackage& pkg) {
    auto list = pkg.PointerAt(0x58);
    u32 count = 0;
    if (!list || !pkg.Read(0x60, count) || count == 0 || count > 4096) return std::nullopt;
    if (!pkg.InRange(*list, static_cast<u64>(count) * 8)) return std::nullopt;
    return RootGeometryList{*list, count};
}

std::optional<GeometryRecord> ReadGeometry(const KcapPackage& pkg, u32 g) {
    GeometryRecord r;
    r.offset = g;
    if (!pkg.InRange(g, 0x80)) return std::nullopt;
    const auto ib = pkg.PointerAt(g + 0x58);
    const auto vs = pkg.PointerAt(g + 0x60);
    if (!ib || !vs) return std::nullopt;
    const u8* p = pkg.Data().data() + g;
    std::memcpy(r.id.data(), p, 16);
    u64 lod_word = 0;
    pkg.Read(g + 0x18, lod_word);
    r.triangle_count = static_cast<u16>(lod_word);
    r.view_mask = static_cast<u16>(lod_word >> 16);
    r.lod_min = static_cast<s8>(lod_word >> 48);
    r.lod_max = static_cast<s8>(lod_word >> 56);
    if (!ReadVec3(pkg, g + 0x20, r.aabb_min) || !ReadVec3(pkg, g + 0x2c, r.aabb_max)) return std::nullopt;
    if (!ReadVec3(pkg, g + 0x38, r.position_offset) || !pkg.Read(g + 0x44, r.position_scale) ||
        !std::isfinite(r.position_scale) || r.position_scale <= 0.0f) {
        return std::nullopt;
    }
    r.index_buffer = *ib;
    r.vsharp_array = *vs;
    pkg.Read(g + 0x68, r.index_count);
    u8 stream_count = 0;
    pkg.Read(g + 0x6c, stream_count);
    if (r.index_count == 0 || r.index_count % 3 != 0 || r.index_count > kMaxIndices) return std::nullopt;
    if (stream_count == 0 || stream_count > kMaxStreams) return std::nullopt;
    if (!pkg.InRange(r.index_buffer, static_cast<u64>(r.index_count) * 2)) return std::nullopt;
    for (u32 i = 0; i < stream_count; ++i) {
        const u32 loc = r.vsharp_array + i * 16;
        const KcapLink* link = pkg.LinkAt(loc);
        if (!link || link->kind != LinkKind::BufferBase) return std::nullopt;
        u32 dw[4];
        if (!pkg.Read(loc, dw)) return std::nullopt;
        VertexStream s;
        s.location = loc;
        s.vsharp = DecodeVSharp(dw);
        s.vsharp.base = link->target;
        const u32 esize = ElementSize(s.vsharp.dfmt);
        if (esize == 0 || s.vsharp.stride < esize || s.vsharp.num_records == 0) return std::nullopt;
        const u64 bytes = static_cast<u64>(s.vsharp.stride) * (s.vsharp.num_records - 1) + esize;
        if (!pkg.InRange(s.vsharp.base, bytes)) return std::nullopt;
        r.streams.push_back(s);
    }
    return r;
}

std::vector<GeometryRecord> FindGeometry(const KcapPackage& pkg) {
    std::unordered_set<u32> vsharp_locations;
    for (const KcapLink& l : pkg.Links()) {
        if (l.kind == LinkKind::BufferBase) vsharp_locations.insert(l.location);
    }
    std::vector<GeometryRecord> out;
    for (const KcapLink& l : pkg.Links()) {
        if ((l.kind != LinkKind::Pointer && l.kind != LinkKind::Pointer64) || l.location < 0x60) continue;
        if (!vsharp_locations.contains(l.target)) continue;
        if (auto r = ReadGeometry(pkg, l.location - 0x60)) out.push_back(std::move(*r));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const auto& a, const auto& b) { return a.offset == b.offset; }),
              out.end());
    return out;
}

bool DecodeGeometry(const KcapPackage& pkg, const GeometryRecord& rec, MeshData& out, std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::format("geometry @{:#x}: {}", rec.offset, msg);
        return false;
    };
    out = MeshData{};
    out.id = rec.id;
    out.source_offset = rec.offset;
    out.view_mask = rec.view_mask;
    if (rec.streams.empty()) return fail("no vertex streams");

    const VSharp& pos = rec.streams[0].vsharp;
    const u32 vertex_count = pos.num_records;
    const bool pos_ok = (pos.dfmt == DataFormat::D16_16_16_16 &&
                         (pos.nfmt == NumFormat::Snorm || pos.nfmt == NumFormat::SnormNz)) ||
                        (pos.dfmt == DataFormat::D32_32_32 && pos.nfmt == NumFormat::Float) ||
                        (pos.dfmt == DataFormat::D32_32_32_32 && pos.nfmt == NumFormat::Float);
    if (!pos_ok) return fail(std::format("stream 0 is {}/{}, not a position format", ToString(pos.dfmt), ToString(pos.nfmt)));

    const u8* data = pkg.Data().data();
    out.positions.resize(vertex_count);
    // The quantised positions were produced as floor(x * 32767) (AABB min/max match raw/32767), so
    // decode them as plain snorm even when the V# says SNORM_OGL.
    const NumFormat pos_nfmt = pos.nfmt == NumFormat::SnormNz ? NumFormat::Snorm : pos.nfmt;
    const float tol = 2.0f / 32767.0f + 1e-5f;
    u32 outside = 0;
    for (u32 i = 0; i < vertex_count; ++i) {
        float v[4];
        DecodeElement(data + pos.base + static_cast<u64>(i) * pos.stride, pos.dfmt, pos_nfmt, v);
        out.positions[i] = {v[0], v[1], v[2]};
        if (v[0] < rec.aabb_min.x - tol || v[1] < rec.aabb_min.y - tol || v[2] < rec.aabb_min.z - tol ||
            v[0] > rec.aabb_max.x + tol || v[1] > rec.aabb_max.y + tol || v[2] > rec.aabb_max.z + tol) {
            ++outside;
        }
    }
    if (outside > 0) return fail(std::format("{} of {} positions outside the record AABB", outside, vertex_count));
    // Dequantise: centimetres = offset + scale * normalised; the engine works in metres.
    const float scale_m = rec.position_scale * 0.01f;
    const Vec3 o = rec.position_offset * 0.01f;
    for (Vec3& p : out.positions) p = o + p * scale_m;
    out.bounds_min = o + rec.aabb_min * scale_m;
    out.bounds_max = o + rec.aabb_max * scale_m;

    for (size_t s = 1; s < rec.streams.size(); ++s) {
        const VSharp& v = rec.streams[s].vsharp;
        if (v.num_records != vertex_count) continue;
        if (out.normals.empty() && v.dfmt == DataFormat::D2_10_10_10 && v.nfmt == NumFormat::Snorm) {
            out.normals.resize(vertex_count);
            for (u32 i = 0; i < vertex_count; ++i) {
                float n[4];
                DecodeElement(data + v.base + static_cast<u64>(i) * v.stride, v.dfmt, v.nfmt, n);
                out.normals[i] = Normalize(Vec3{n[0], n[1], n[2]});
            }
        } else if (out.uv0.empty() && v.dfmt == DataFormat::D16_16 && v.nfmt == NumFormat::Float) {
            out.uv0.resize(vertex_count);
            for (u32 i = 0; i < vertex_count; ++i) {
                float t[4];
                DecodeElement(data + v.base + static_cast<u64>(i) * v.stride, v.dfmt, v.nfmt, t);
                out.uv0[i] = {t[0], t[1]};
            }
        }
    }

    // Skin: sint16x4 joint indices, followed (+8, same stride) by unorm8x4 weights of joints 1..3.
    for (size_t s = 1; s < rec.streams.size() && out.joints.empty(); ++s) {
        const VSharp& j = rec.streams[s].vsharp;
        if (j.num_records != vertex_count || j.dfmt != DataFormat::D16_16_16_16 || j.nfmt != NumFormat::Sint) continue;
        const VSharp* w = nullptr;
        for (const auto& c : rec.streams) {
            if (c.vsharp.base == j.base + 8 && c.vsharp.stride == j.stride && c.vsharp.dfmt == DataFormat::D8_8_8_8 &&
                c.vsharp.nfmt == NumFormat::Unorm && c.vsharp.num_records == vertex_count) {
                w = &c.vsharp;
            }
        }
        if (!w) continue;
        out.joints.resize(vertex_count);
        out.weights.resize(vertex_count);
        bool consistent = true;
        for (u32 i = 0; i < vertex_count && consistent; ++i) {
            s16 ji[4];
            u8 wb[4];
            std::memcpy(ji, data + j.base + static_cast<u64>(i) * j.stride, 8);
            std::memcpy(wb, data + w->base + static_cast<u64>(i) * w->stride, 4);
            const int rest = 255 - (wb[0] + wb[1] + wb[2]);
            consistent = rest >= 0 && ji[0] >= 0 && ji[1] >= 0 && ji[2] >= 0 && ji[3] >= 0;
            out.joints[i] = {static_cast<u16>(ji[0]), static_cast<u16>(ji[1]), static_cast<u16>(ji[2]), static_cast<u16>(ji[3])};
            out.weights[i] = {static_cast<u8>(std::max(rest, 0)), wb[0], wb[1], wb[2]};
        }
        if (!consistent) {
            // Another skin encoding (seen on one head part of hero.xpps: third byte ~always 255):
            // not decoded yet, the part stays rigid instead of being rejected.
            out.joints.clear();
            out.weights.clear();
            break;
        }
    }

    out.indices.resize(rec.index_count);
    for (u32 i = 0; i < rec.index_count; ++i) {
        u16 idx;
        std::memcpy(&idx, data + rec.index_buffer + static_cast<u64>(i) * 2, 2);
        if (idx >= vertex_count) return fail(std::format("index {} = {} >= vertex count {}", i, idx, vertex_count));
        out.indices[i] = idx;
    }
    return true;
}

}  // namespace sspc::res
