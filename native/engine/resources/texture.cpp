#include "resources/texture.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace sspc::res {
namespace {

constexpr u32 kTilingThin1d = 13;

TexelFormat FromDataFormat(u32 dfmt) {
    switch (dfmt) {
        case 35: return TexelFormat::BC1;
        case 36: return TexelFormat::BC2;
        case 37: return TexelFormat::BC3;
        case 38: return TexelFormat::BC4;
        case 39: return TexelFormat::BC5;
        case 40: return TexelFormat::BC6H;
        case 41: return TexelFormat::BC7;
        case 10: return TexelFormat::RGBA8;
        default: return TexelFormat::Unknown;
    }
}

// Morton index within an 8x8 micro tile -> (x, y).
inline void MicroTileXY(u32 i, u32& x, u32& y) {
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
}

u32 Elements(u32 pixels, bool bc) { return bc ? (pixels + 3) / 4 : pixels; }

}  // namespace

const char* ToString(TexelFormat f) {
    static const char* const kNames[] = {"unknown", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "RGBA8"};
    return kNames[static_cast<size_t>(f)];
}

u32 BytesPerElement(TexelFormat f) {
    switch (f) {
        case TexelFormat::BC1:
        case TexelFormat::BC4: return 8;
        case TexelFormat::BC2:
        case TexelFormat::BC3:
        case TexelFormat::BC5:
        case TexelFormat::BC6H:
        case TexelFormat::BC7: return 16;
        case TexelFormat::RGBA8: return 4;
        default: return 0;
    }
}

bool IsBlockCompressed(TexelFormat f) { return f != TexelFormat::RGBA8 && f != TexelFormat::Unknown; }

u64 Thin1dLevelSize(u32 elems_w, u32 elems_h, u32 bpe) {
    const u64 tw = (elems_w + 7) / 8, th = (elems_h + 7) / 8;
    return tw * th * 64 * bpe;
}

void DetileThin1d(const u8* src, u32 elems_w, u32 elems_h, u32 bpe, u8* dst) {
    const u32 tiles_x = (elems_w + 7) / 8;
    const u32 tiles_y = (elems_h + 7) / 8;
    for (u32 ty = 0; ty < tiles_y; ++ty) {
        for (u32 tx = 0; tx < tiles_x; ++tx) {
            const u8* tile = src + (static_cast<u64>(ty) * tiles_x + tx) * 64 * bpe;
            for (u32 i = 0; i < 64; ++i) {
                u32 x, y;
                MicroTileXY(i, x, y);
                const u32 X = tx * 8 + x, Y = ty * 8 + y;
                if (X < elems_w && Y < elems_h) {
                    std::memcpy(dst + (static_cast<u64>(Y) * elems_w + X) * bpe, tile + static_cast<u64>(i) * bpe, bpe);
                }
            }
        }
    }
}

std::vector<TextureInfo> FindTextures(const KcapPackage& pkg) {
    std::vector<TextureInfo> out;
    for (const KcapLink& l : pkg.Links()) {
        if (l.kind != LinkKind::Address256 || l.location < 0x28 || l.target_virtual) continue;
        u32 dw[8];
        if (!pkg.Read(l.location, dw)) continue;
        TextureInfo t;
        t.type = static_cast<u8>((dw[3] >> 28) & 0xf);
        if (t.type != 8 && t.type != 9) continue;  // 1D/2D image resources only
        t.object = l.location - 0x28;
        t.data = l.target;
        t.data_format = static_cast<u8>((dw[1] >> 20) & 0x3f);
        t.num_format = static_cast<u8>((dw[1] >> 26) & 0xf);
        t.width = (dw[2] & 0x3fff) + 1;
        t.height = ((dw[2] >> 14) & 0x3fff) + 1;
        t.dst_sel = static_cast<u16>(dw[3] & 0xfff);
        t.base_level = static_cast<u8>((dw[3] >> 12) & 0xf);
        t.last_level = static_cast<u8>((dw[3] >> 16) & 0xf);
        t.tiling = static_cast<u8>((dw[3] >> 20) & 0x1f);
        t.format = FromDataFormat(t.data_format);
        pkg.Read(t.object + 0x48, t.id);
        out.push_back(t);
    }
    return out;
}

bool DecodeTexture(const KcapPackage& pkg, const TextureInfo& info, TextureData& out, std::string* error) {
    auto fail = [&](std::string msg) {
        if (error) *error = std::format("texture {:016x}: {}", info.id, msg);
        return false;
    };
    if (info.tiling != kTilingThin1d) return fail(std::format("tiling {} not supported", info.tiling));
    const u32 bpe = BytesPerElement(info.format);
    if (bpe == 0) return fail(std::format("data format {} not supported", info.data_format));
    if (info.last_level < info.base_level || info.width == 0 || info.height == 0) return fail("bad level range");
    const bool bc = IsBlockCompressed(info.format);
    out = TextureData{};
    out.id = info.id;
    out.format = info.format;
    out.width = info.width;
    out.height = info.height;
    u64 offset = info.data;
    for (u32 level = info.base_level; level <= info.last_level; ++level) {
        const u32 w = std::max(1u, info.width >> level), h = std::max(1u, info.height >> level);
        const u32 ew = Elements(w, bc), eh = Elements(h, bc);
        const u64 size = Thin1dLevelSize(ew, eh, bpe);
        if (!pkg.InRange(offset, size)) return fail(std::format("level {} outside the package data", level));
        std::vector<u8> linear(static_cast<size_t>(ew) * eh * bpe);
        DetileThin1d(pkg.Data().data() + offset, ew, eh, bpe, linear.data());
        out.mips.push_back(std::move(linear));
        offset += size;
    }
    return true;
}

std::optional<MaterialTextures> ReadMaterialTextures(const KcapPackage& pkg, u32 geometry_offset) {
    const auto material = pkg.PointerAt(geometry_offset + 0x10);
    if (!material) return std::nullopt;
    const auto list = pkg.PointerAt(*material + 0x30);
    u64 count = 0;
    if (!list || !pkg.Read(*material + 0x38, count) || count == 0 || count > 32) return std::nullopt;
    MaterialTextures mt;
    mt.material = *material;
    for (u64 i = 0; i < count; ++i) {
        const u32 e = *list + static_cast<u32>(i) * 0x30;
        u64 id = 0, slot = 0;
        if (!pkg.Read(e, id) || !pkg.Read(e + 0x18, slot)) return std::nullopt;
        if (pkg.PointerAt(e + 0x10) != material) return std::nullopt;  // must point back to its material
        mt.bindings.push_back({id, static_cast<u8>(slot)});
        // The field is a category, not a unique slot (e.g. two category-3 maps); keep the first.
        if (slot < mt.slots.size() && mt.slots[slot] == 0) mt.slots[slot] = id;
    }
    return mt;
}

}  // namespace sspc::res
