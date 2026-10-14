// SecondSonPC native runtime - static scene of a world chunk package (a#/b#/c#/d#.xpps).
//
// Layout confirmed on a1.xpps (docs/native/RESOURCE_FORMATS.md, "World chunks"): a scene object
// holds three {pointer, count} pairs:
//   +0xA8/+0xB0  instances, 8 bytes: u16 kind, u16 matrix index, u16 (geometry slot << 2 | mode), u16 extra
//   +0xB8/+0xC0  matrices, 48 bytes: 3x4 row-major float, translation (centimetres) in the 4th column
//   +0xC8/+0xD0  geometry slots, 8-byte pointers to geometry records (null = provided by another
//                package through KNLX external links, not resolved yet)
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/kcap.h"

namespace sspc::res {

struct ChunkInstance {
    u16 kind = 0;      // 0 for the main set; 2/4/6 seen (role not decoded)
    u16 matrix = 0;
    u16 geometry = 0;  // slot index
    u8 mode = 0;       // low 2 bits of the slot field
    u16 extra = 0;
};

struct ChunkScene {
    u32 object = 0;
    std::vector<std::array<float, 12>> matrices;  // as stored
    std::vector<ChunkInstance> instances;
    std::vector<std::optional<u32>> geometry;      // geometry record offset per slot
};

// Every structurally valid scene object in the package (all indices in range, arrays inside the
// data, matrices finite).
std::vector<ChunkScene> FindChunkScenes(const KcapPackage& pkg);

// Stored 3x4 (centimetres) -> engine world matrix (metres, column-major).
Mat4 ChunkMatrixToWorld(const std::array<float, 12>& m);

}  // namespace sspc::res
