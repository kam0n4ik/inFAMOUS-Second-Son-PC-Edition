// SecondSonPC native runtime - Havok 2013.2 binary packfiles inside game packages (collision).
//
// World chunks carry their static collision as Havok packfiles ('hk_2013.2.0-r1', layout rules
// 8/1/1/1) stored in KCAP data blocks. This reader parses the packfile container (sections,
// local/global/virtual fixups, class names) and decodes the shapes used for the village terrain:
//   hknpStaticCompoundShape   instances {rotation columns, translation, scale, shape pointer}
//   hknpCompressedMeshShape   hkcdStaticMeshTree: sections with quantised packed vertices
//                             (11/11/10 bits, per-section offset+scale), shared vertices (21/21/22
//                             bits in the mesh domain), primitives of 4 vertex indices (c == d:
//                             triangle, else quad)
// Field offsets were measured on the shipped data (docs/native/RESOURCE_FORMATS.md "Havok
// collision"); no Havok code is used. Coordinates are metres, Z up (same frame as the chunks).
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace sspc::res {

class HavokPackfile {
public:
    struct Object {
        u32 offset = 0;  // data-section relative
        std::string class_name;
    };
    // Parses the packfile starting at `offset` of `data`; false with `error` when malformed.
    bool Parse(std::span<const u8> data, u64 offset, std::string* error = nullptr);

    const std::vector<Object>& Objects() const { return objects_; }
    const std::string& Version() const { return version_; }
    u32 DataSize() const { return data_size_; }

    // Data-section accessors (bounds-checked; false/0 when out of range).
    bool Read(u64 offset, void* out, u64 size) const;
    template <class T>
    T Get(u64 offset) const {
        T v{};
        Read(offset, &v, sizeof(T));
        return v;
    }
    // Target of a pointer field stored at `offset` (local or global fixup into the data section).
    std::optional<u32> PointerAt(u32 offset) const;

private:
    std::span<const u8> data_;  // the data section
    u32 data_size_ = 0;
    std::string version_;
    std::vector<Object> objects_;
    std::vector<std::pair<u32, u32>> pointers_;  // sorted {location, target}
};

// Offsets of every Havok packfile (magic 57E0E057 10C0C010) in `data`.
std::vector<u64> FindHavokPackfiles(std::span<const u8> data);

struct HavokCollisionStats {
    u32 compound_shapes = 0;
    u32 instances = 0;
    u32 mesh_shapes = 0;
    u32 convex_shapes_skipped = 0;
    u32 triangles = 0;
};

// Triangles (3 vertices each, metres) of every compressed mesh placed by the static compound
// shapes of the packfile. Convex polytopes (props) are counted but not decoded yet.
bool DecodeStaticCollision(const HavokPackfile& pf, std::vector<Vec3>& triangles, HavokCollisionStats& stats,
                           std::string* error = nullptr);

}  // namespace sspc::res
