// SecondSonPC native runtime - geometry records inside KCAP packages.
//
// Geometry record layout (confirmed on hero.xpps against eboot DrawSingle 0x1a1960, see
// docs/native/RESOURCE_FORMATS.md):
//   +0x00 u8[16]  id
//   +0x10 ptr     (owner/description, not decoded yet)
//   +0x18 u64     bits 0..15 triangle count, 16..31 view mask, 48..55 s8 LOD min, 56..63 s8 LOD max
//   +0x20 f32[3]  AABB min, +0x2C f32[3] AABB max (same units as positions)
//   +0x58 ptr     index buffer (16-bit indices, triangle list)
//   +0x60 ptr     V# array (16 bytes per stream), +0x68 u32 index count, +0x6C u8 stream count
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/gcn_buffer.h"
#include "resources/kcap.h"

namespace sspc::res {

struct VertexStream {
    u32 location = 0;  // data offset of the V#
    VSharp vsharp;     // vsharp.base = data offset of the buffer
};

struct GeometryRecord {
    u32 offset = 0;
    std::array<u8, 16> id{};
    Vec3 aabb_min, aabb_max;
    u32 index_buffer = 0;
    u32 index_count = 0;
    u32 vsharp_array = 0;
    u16 triangle_count = 0;
    u16 view_mask = 0;
    s8 lod_min = 0;
    s8 lod_max = 0;
    std::vector<VertexStream> streams;
};

// Root of a character package (offset 0): +0x58 pointer to the model's geometry list, +0x60 count.
// Observed on hero.xpps; other package kinds may differ (the function returns nullopt then).
struct RootGeometryList {
    u32 list = 0;
    u32 count = 0;
};
std::optional<RootGeometryList> ReadRootGeometryList(const KcapPackage& pkg);

// Every geometry record reachable through the link table: a pointer at +0x60 that targets a V#
// link, an index-buffer pointer at +0x58, sane counts and in-range streams.
std::vector<GeometryRecord> FindGeometry(const KcapPackage& pkg);
// Reads one record at `offset`; nullopt if it does not validate.
std::optional<GeometryRecord> ReadGeometry(const KcapPackage& pkg, u32 offset);

struct MeshData {
    std::array<u8, 16> id{};
    u32 source_offset = 0;
    u16 view_mask = 0;
    Vec3 bounds_min, bounds_max;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;  // empty if the geometry has no recognised normal stream
    std::vector<Vec2> uv0;      // empty if no UV stream
    std::vector<u32> indices;
};

// Decodes positions (stream 0), normals (first 2_10_10_10 snorm stream) and UV0 (first 16_16 float
// stream) plus the index buffer. Positions are checked against the record's AABB. Returns false and
// sets `error` if anything is out of range or inconsistent.
bool DecodeGeometry(const KcapPackage& pkg, const GeometryRecord& rec, MeshData& out, std::string* error);

}  // namespace sspc::res
