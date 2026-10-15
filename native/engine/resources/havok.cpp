#include "resources/havok.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace sspc::res {
namespace {

constexpr u8 kMagic[8] = {0x57, 0xe0, 0xe0, 0x57, 0x10, 0xc0, 0xc0, 0x10};

template <class T>
bool ReadAt(std::span<const u8> d, u64 o, T& out) {
    if (o > d.size() || d.size() - o < sizeof(T)) return false;
    std::memcpy(&out, d.data() + o, sizeof(T));
    return true;
}

struct Section {
    std::string tag;
    u32 start = 0, local = 0, global = 0, virt = 0, exports = 0, imports = 0, end = 0;
};

// hknpCompressedMeshShape field offsets (object relative), measured on the shipped packfiles.
constexpr u32 kCmsDomainMin = 0xa0, kCmsDomainMax = 0xb0;
constexpr u32 kCmsSections = 0xd0, kCmsPrimitives = 0xe0, kCmsSharedIndex = 0xf0, kCmsPacked = 0x100,
              kCmsShared = 0x110;
constexpr u32 kSectionSize = 0x60;
// hknpStaticCompoundShape: instance array (free list) and the instance layout.
constexpr u32 kCompoundInstances = 0x60;
constexpr u32 kInstanceSize = 0x80, kInstanceScale = 0x40, kInstanceShape = 0x50;

}  // namespace

bool HavokPackfile::Parse(std::span<const u8> data, u64 offset, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    objects_.clear();
    pointers_.clear();
    if (offset > data.size() || data.size() - offset < 0x40) return fail("packfile header out of range");
    const std::span<const u8> f = data.subspan(offset);
    if (std::memcmp(f.data(), kMagic, 8) != 0) return fail("not a Havok packfile");
    u32 file_version = 0, num_sections = 0;
    ReadAt(f, 0x0c, file_version);
    ReadAt(f, 0x14, num_sections);
    if (f[0x10] != 8 || f[0x11] != 1) return fail("unsupported layout rules (need 64-bit little endian)");
    if (file_version < 9 || num_sections == 0 || num_sections > 16) return fail(std::format("bad version/sections {}/{}", file_version, num_sections));
    char ver[17] = {};
    std::memcpy(ver, f.data() + 0x28, 16);
    version_ = ver;
    s16 pad0 = 0, pad1 = 0;
    ReadAt(f, 0x3c, pad0);
    ReadAt(f, 0x3e, pad1);
    const u64 header = 0x40 + (pad0 != -1 && pad1 > 0 ? static_cast<u64>(pad1) : 0);
    const u64 section_size = file_version >= 11 ? 0x40 : 0x30;
    std::vector<Section> secs(num_sections);
    for (u32 i = 0; i < num_sections; ++i) {
        const u64 o = header + i * section_size;
        if (o + 48 > f.size()) return fail("section table out of range");
        char tag[20] = {};
        std::memcpy(tag, f.data() + o, 19);
        secs[i].tag = tag;
        u32 v[7];
        std::memcpy(v, f.data() + o + 20, sizeof(v));
        secs[i] = {tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
        if (static_cast<u64>(v[0]) + v[6] > f.size() || v[1] > v[2] || v[2] > v[3] || v[3] > v[4] || v[4] > v[6]) {
            return fail("section '" + secs[i].tag + "' out of range");
        }
    }
    const auto find = [&](const char* t) -> int {
        for (u32 i = 0; i < num_sections; ++i)
            if (secs[i].tag == t) return static_cast<int>(i);
        return -1;
    };
    const int cn = find("__classnames__"), ds = find("__data__");
    if (cn < 0 || ds < 0) return fail("missing __classnames__ or __data__");
    // Class names: {u32 signature, u8 0x09, name\0}.
    std::unordered_map<u32, std::string> names;
    {
        const Section& s = secs[static_cast<size_t>(cn)];
        u64 o = s.start;
        const u64 end = static_cast<u64>(s.start) + s.local;
        while (o + 6 <= end && f[o + 4] == 0x09) {
            const u64 name = o + 5;
            u64 z = name;
            while (z < end && f[z] != 0) ++z;
            if (z >= end) break;
            names[static_cast<u32>(name - s.start)] = std::string(reinterpret_cast<const char*>(f.data() + name), z - name);
            o = z + 1;
        }
    }
    const Section& s = secs[static_cast<size_t>(ds)];
    data_ = f.subspan(s.start, s.local);
    data_size_ = s.local;
    for (u64 o = static_cast<u64>(s.start) + s.local; o + 8 <= static_cast<u64>(s.start) + s.global; o += 8) {
        u32 src = 0, dst = 0;
        ReadAt(f, o, src);
        ReadAt(f, o + 4, dst);
        if (src != 0xffffffffu && src < data_size_ && dst < data_size_) pointers_.emplace_back(src, dst);
    }
    for (u64 o = static_cast<u64>(s.start) + s.global; o + 12 <= static_cast<u64>(s.start) + s.virt; o += 12) {
        u32 src = 0, sec = 0, dst = 0;
        ReadAt(f, o, src);
        ReadAt(f, o + 4, sec);
        ReadAt(f, o + 8, dst);
        if (src != 0xffffffffu && static_cast<int>(sec) == ds && src < data_size_ && dst < data_size_) pointers_.emplace_back(src, dst);
    }
    std::sort(pointers_.begin(), pointers_.end());
    for (u64 o = static_cast<u64>(s.start) + s.virt; o + 12 <= static_cast<u64>(s.start) + s.exports; o += 12) {
        u32 src = 0, sec = 0, name = 0;
        ReadAt(f, o, src);
        ReadAt(f, o + 4, sec);
        ReadAt(f, o + 8, name);
        if (src == 0xffffffffu) continue;
        const auto it = names.find(name);
        if (static_cast<int>(sec) != cn || it == names.end() || src >= data_size_) return fail("bad virtual fixup");
        objects_.push_back({src, it->second});
    }
    std::sort(objects_.begin(), objects_.end(), [](const Object& a, const Object& b) { return a.offset < b.offset; });
    return true;
}

bool HavokPackfile::Read(u64 offset, void* out, u64 size) const {
    if (offset > data_.size() || data_.size() - offset < size) {
        std::memset(out, 0, static_cast<size_t>(size));
        return false;
    }
    std::memcpy(out, data_.data() + offset, static_cast<size_t>(size));
    return true;
}

std::optional<u32> HavokPackfile::PointerAt(u32 offset) const {
    const auto it = std::lower_bound(pointers_.begin(), pointers_.end(), std::pair<u32, u32>{offset, 0});
    if (it == pointers_.end() || it->first != offset) return std::nullopt;
    return it->second;
}

std::vector<u64> FindHavokPackfiles(std::span<const u8> data) {
    std::vector<u64> out;
    for (u64 o = 0; o + 0x40 <= data.size(); o += 4) {
        if (data[o] == kMagic[0] && std::memcmp(data.data() + o, kMagic, 8) == 0) out.push_back(o);
    }
    return out;
}

namespace {

// Triangles of one hknpCompressedMeshShape in its own frame; false when any index is out of range.
bool DecodeCompressedMesh(const HavokPackfile& pf, u32 o, std::vector<Vec3>& tris, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = std::format("compressed mesh @{:#x}: {}", o, why);
        return false;
    };
    struct Arr {
        u32 ptr = 0, count = 0;
    };
    auto arr = [&](u32 field, u32 elem) -> std::optional<Arr> {
        const u32 count = pf.Get<u32>(o + field + 8);
        if (count == 0) return Arr{};
        const auto p = pf.PointerAt(o + field);
        if (!p || static_cast<u64>(*p) + static_cast<u64>(count) * elem > pf.DataSize()) return std::nullopt;
        return Arr{*p, count};
    };
    const auto sections = arr(kCmsSections, kSectionSize), prims = arr(kCmsPrimitives, 4),
               shared_index = arr(kCmsSharedIndex, 2), packed = arr(kCmsPacked, 4), shared = arr(kCmsShared, 8);
    if (!sections || !prims || !shared_index || !packed || !shared) return fail("array out of range");
    float dmin[4], dmax[4];
    pf.Read(o + kCmsDomainMin, dmin, 16);
    pf.Read(o + kCmsDomainMax, dmax, 16);
    const float shared_scale[3] = {(dmax[0] - dmin[0]) / 2097151.0f, (dmax[1] - dmin[1]) / 2097151.0f,
                                   (dmax[2] - dmin[2]) / 4194303.0f};
    for (u32 s = 0; s < sections->count; ++s) {
        const u32 so = sections->ptr + s * kSectionSize;
        float cp[6];
        pf.Read(so + 0x30, cp, sizeof(cp));
        const u32 first_packed = pf.Get<u32>(so + 0x48);
        const u32 shared_data = pf.Get<u32>(so + 0x4c);  // (first shared index << 8) | packed count
        const u32 prim_data = pf.Get<u32>(so + 0x50);    // (first primitive << 8) | count
        const u32 num_packed = shared_data & 0xff, first_shared = shared_data >> 8;
        const u32 num_shared = pf.Get<u8>(so + 0x59);
        const u32 first_prim = prim_data >> 8, num_prims = prim_data & 0xff;
        if (first_packed + num_packed > packed->count || first_shared + num_shared > shared_index->count ||
            first_prim + num_prims > prims->count) {
            return fail(std::format("section {} out of range", s));
        }
        auto vertex = [&](u32 i, Vec3& out) {
            if (i < num_packed) {
                const u32 v = pf.Get<u32>(packed->ptr + 4 * (first_packed + i));
                out = {cp[0] + static_cast<float>(v & 0x7ff) * cp[3], cp[1] + static_cast<float>((v >> 11) & 0x7ff) * cp[4],
                       cp[2] + static_cast<float>(v >> 22) * cp[5]};
                return true;
            }
            if (i - num_packed >= num_shared) return false;
            const u16 si = pf.Get<u16>(shared_index->ptr + 2 * (first_shared + i - num_packed));
            if (si >= shared->count) return false;
            const u64 v = pf.Get<u64>(shared->ptr + 8ull * si);
            out = {dmin[0] + static_cast<float>(v & 0x1fffff) * shared_scale[0],
                   dmin[1] + static_cast<float>((v >> 21) & 0x1fffff) * shared_scale[1],
                   dmin[2] + static_cast<float>(v >> 42) * shared_scale[2]};
            return true;
        };
        for (u32 p = first_prim; p < first_prim + num_prims; ++p) {
            u8 idx[4];
            pf.Read(prims->ptr + 4ull * p, idx, 4);
            if (idx[0] == 0xde && idx[1] == 0xad && idx[2] == 0xde && idx[3] == 0xad) continue;  // deleted primitive
            Vec3 a, b, c, d;
            if (!vertex(idx[0], a) || !vertex(idx[1], b) || !vertex(idx[2], c)) return fail("vertex index out of range");
            tris.insert(tris.end(), {a, b, c});
            if (idx[3] != idx[2]) {
                if (!vertex(idx[3], d)) return fail("vertex index out of range");
                tris.insert(tris.end(), {a, c, d});
            }
        }
    }
    return true;
}

}  // namespace

bool DecodeStaticCollision(const HavokPackfile& pf, std::vector<Vec3>& triangles, HavokCollisionStats& stats,
                           std::string* error) {
    std::unordered_map<u32, const std::string*> class_at;
    for (const auto& obj : pf.Objects()) class_at[obj.offset] = &obj.class_name;
    std::unordered_map<u32, std::vector<Vec3>> mesh_cache;
    for (const auto& obj : pf.Objects()) {
        if (obj.class_name != "hknpStaticCompoundShape") continue;
        ++stats.compound_shapes;
        const u32 count = pf.Get<u32>(obj.offset + kCompoundInstances + 8);
        const auto base = pf.PointerAt(obj.offset + kCompoundInstances);
        if (!count) continue;
        if (!base || static_cast<u64>(*base) + static_cast<u64>(count) * kInstanceSize > pf.DataSize()) {
            if (error) *error = "compound instance array out of range";
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 e = *base + i * kInstanceSize;
            const auto shape = pf.PointerAt(e + kInstanceShape);
            if (!shape) continue;  // free-list hole
            const auto cls = class_at.find(*shape);
            if (cls == class_at.end()) continue;
            if (*cls->second == "hknpConvexPolytopeShape") {
                ++stats.convex_shapes_skipped;
                continue;
            }
            if (*cls->second != "hknpCompressedMeshShape") continue;
            auto it = mesh_cache.find(*shape);
            if (it == mesh_cache.end()) {
                std::vector<Vec3> local;
                if (!DecodeCompressedMesh(pf, *shape, local, error)) return false;
                it = mesh_cache.emplace(*shape, std::move(local)).first;
                ++stats.mesh_shapes;
            }
            // hkTransform: rotation columns at +0x00/+0x10/+0x20, translation +0x30 (w lanes carry
            // other data), scale +0x40.
            float m[16], sc[4];
            pf.Read(e, m, sizeof(m));
            pf.Read(e + kInstanceScale, sc, sizeof(sc));
            bool finite = true;
            for (float v : m) finite = finite && std::isfinite(v);
            for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(sc[k]);
            if (!finite) continue;
            ++stats.instances;
            for (const Vec3& p : it->second) {
                const Vec3 q{p.x * sc[0], p.y * sc[1], p.z * sc[2]};
                triangles.push_back({m[0] * q.x + m[4] * q.y + m[8] * q.z + m[12], m[1] * q.x + m[5] * q.y + m[9] * q.z + m[13],
                                     m[2] * q.x + m[6] * q.y + m[10] * q.z + m[14]});
            }
        }
    }
    stats.triangles = static_cast<u32>(triangles.size() / 3);
    return true;
}

}  // namespace sspc::res
