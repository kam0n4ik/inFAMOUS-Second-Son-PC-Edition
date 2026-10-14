// SecondSonPC native runtime - textures inside KCAP packages.
//
// Confirmed on hero.xpps and a1.xpps (docs/native/RESOURCE_FORMATS.md, "Textures"):
//  * texture object (0x60 bytes): +0x28 T# (8 dwords, AMD GCN image resource), +0x48 u64 id;
//    T# dword0 is a KNLI kind-4 link (address >> 8) to the texel data in a GPU segment.
//  * every texture seen uses tiling index 13 (thin 1D): elements (pixels, or 4x4 blocks for BCn)
//    form 8x8 micro tiles in Morton order (x0 y0 x1 y1 x2 y2), tiles row-major; mip levels follow
//    each other, each padded to whole tiles (70 of 80 hero textures end exactly where the next
//    one starts).
//  * material instance (pointed to by geometry +0x10): +0x30 -> texture bindings (0x30 bytes:
//    +0x00 u64 texture id, +0x10 pointer back to the material, +0x18 u64 category), +0x38 count.
//    Category 1 occurs once per material and holds the BC1/BC3 colour map; 3 = BC5 normal maps
//    (can repeat), 4 = BC4 maps (repeat) [roles of 3/4: hypothesis]. Ids not present in the
//    package belong to other packages (10 of the hero's 37 colour maps).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "resources/kcap.h"

namespace sspc::res {

enum class TexelFormat : u8 { Unknown, BC1, BC2, BC3, BC4, BC5, BC6H, BC7, RGBA8 };
const char* ToString(TexelFormat f);
u32 BytesPerElement(TexelFormat f);  // per 4x4 block for BCn, per pixel otherwise
bool IsBlockCompressed(TexelFormat f);

struct TextureInfo {
    u64 id = 0;
    u32 object = 0;  // texture object offset
    u32 data = 0;    // texel data offset (level base_level)
    u32 width = 0, height = 0;
    u8 base_level = 0, last_level = 0;
    u8 tiling = 0;
    u8 type = 0;      // 8 = 1D, 9 = 2D (SQ_RSRC_IMG_*)
    u16 dst_sel = 0;  // 12 bits, 3 per channel
    u8 data_format = 0, num_format = 0;
    TexelFormat format = TexelFormat::Unknown;
};

std::vector<TextureInfo> FindTextures(const KcapPackage& pkg);

struct TextureData {
    u64 id = 0;
    TexelFormat format = TexelFormat::Unknown;
    u32 width = 0, height = 0;
    std::vector<std::vector<u8>> mips;  // linear: elements row-major, level by level
};

// Supports tiling 13 and the formats above; validates every level against the package data.
bool DecodeTexture(const KcapPackage& pkg, const TextureInfo& info, TextureData& out, std::string* error);

// Thin-1D helpers (exposed for tests).
u64 Thin1dLevelSize(u32 elems_w, u32 elems_h, u32 bpe);
void DetileThin1d(const u8* src, u32 elems_w, u32 elems_h, u32 bpe, u8* dst);

struct MaterialTextures {
    u32 material = 0;
    std::array<u64, 8> slots{};  // first texture id per category, 0 = none
    struct Binding {
        u64 id;
        u8 category;
    };
    std::vector<Binding> bindings;  // all entries in stored order (categories repeat)
};
std::optional<MaterialTextures> ReadMaterialTextures(const KcapPackage& pkg, u32 geometry_offset);

}  // namespace sspc::res
