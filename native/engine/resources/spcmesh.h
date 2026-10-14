// SecondSonPC native runtime - .spcmesh, the engine's own converted mesh format.
//
// Little-endian. Layout:
//   SpcMeshHeader (64 bytes) | SpcMeshEntry[mesh_count] (96 bytes each) | vertex/index blobs
// Every offset/size is validated against the file size; a CRC-32 covers everything after the
// header. Vertex layout (v1 and v2; v2 stores metres instead of normalised units): position f32x3 [+ normal f32x3] [+ uv0 f32x2]; indices u32.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/types.h"
#include "resources/geometry.h"

namespace sspc::res {

inline constexpr char kSpcMeshMagic[4] = {'S', 'P', 'C', 'M'};
inline constexpr u16 kSpcMeshVersion = 2;  // 2: positions in metres (dequantised)
inline constexpr u16 kSpcMeshByteOrder = 0xFEFF;

enum SpcVertexFlags : u32 {
    kVertexPosition = 1,
    kVertexNormal = 2,
    kVertexUv0 = 4,
};

#pragma pack(push, 1)
struct SpcMeshHeader {
    char magic[4];
    u16 version;
    u16 byte_order;
    u32 header_size;
    u32 flags;
    u32 mesh_count;
    u32 table_offset;
    u32 entry_size;
    u32 crc32;
    u64 file_size;
    u8 source_hash[16];
    u32 reserved[2];
};
struct SpcMeshEntry {
    u8 id[16];
    u32 source_offset;
    u32 vertex_count;
    u32 index_count;
    u32 vertex_flags;
    u32 vertex_stride;
    u64 vertex_offset;
    u64 index_offset;
    float bounds_min[3];
    float bounds_max[3];
    u16 view_mask;
    u16 entry_flags;
    u32 reserved[4];
};
#pragma pack(pop)
static_assert(sizeof(SpcMeshHeader) == 64);
static_assert(sizeof(SpcMeshEntry) == 96);

std::vector<u8> WriteSpcMesh(std::span<const MeshData> meshes, const std::array<u8, 16>& source_hash);
// Returns false and sets `error` on any validation failure; `out` is untouched then.
bool ReadSpcMesh(std::span<const u8> file, std::vector<MeshData>& out, std::string* error);

}  // namespace sspc::res
