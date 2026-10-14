#include <cstring>
#include <unordered_map>

#include "core/file.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/texture.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;

TEST(texture_thin1d_detile_morton_order) {
    // 16 x 8 elements (two micro tiles), 1 byte per element; element value = its tiled index.
    std::vector<u8> tiled(128);
    for (u32 i = 0; i < 128; ++i) tiled[i] = static_cast<u8>(i);
    std::vector<u8> linear(128);
    DetileThin1d(tiled.data(), 16, 8, 1, linear.data());
    // Tile 0: (x, y) = (1, 0) is Morton index 1; (0, 1) is 2; (1, 1) is 3; (2, 0) is 4; (0, 2) is 8.
    CHECK(linear[0] == 0 && linear[1] == 1 && linear[16] == 2 && linear[17] == 3 && linear[2] == 4 && linear[32] == 8);
    CHECK(linear[7 * 16 + 7] == 63);  // last element of tile 0
    CHECK(linear[8] == 64);           // first element of tile 1 (tiles are row-major)
    CHECK_EQ(Thin1dLevelSize(16, 8, 1), 128ull);
    CHECK_EQ(Thin1dLevelSize(1, 1, 8), 512ull);  // one padded tile of BC1 blocks
    CHECK_EQ(Thin1dLevelSize(9, 1, 16), 2048ull);
    // A partial tile: 3 x 2 elements read from a full padded tile without overrun.
    std::vector<u8> small(64 * 4, 0xAB);
    std::vector<u8> out(3 * 2 * 4);
    DetileThin1d(small.data(), 3, 2, 4, out.data());
    CHECK(out[0] == 0xAB && out.back() == 0xAB);
}

TEST(real_hero_textures_and_material_bindings) {
    const std::wstring path = L"E:\\shdps4\\CUSA00004\\art\\cache\\hero.xpps";
    if (!file::Exists(path)) {
        test::Skip("hero.xpps not found");
        return;
    }
    KcapFile f;
    CHECK_EQ(f.Open(path), KcapError::Ok);
    const KcapPackage& p = f.Package();
    const auto textures = FindTextures(p);
    std::unordered_map<u64, TextureInfo> by_id;
    u32 decoded = 0, bc = 0;
    for (const auto& t : textures) {
        by_id[t.id] = t;
        TextureData d;
        if (DecodeTexture(p, t, d, nullptr)) {
            ++decoded;
            bc += IsBlockCompressed(d.format) ? 1 : 0;
            CHECK(d.mips.size() == static_cast<size_t>(t.last_level - t.base_level + 1));
        }
    }
    std::printf("    %zu texture descriptors, %u decoded (%u BCn)\n", textures.size(), decoded, bc);
    CHECK(textures.size() >= 80 && decoded == textures.size());
    // Part c2ad0 uses material 0xd22d0: slot 1 = the plaid shirt (BC1 512x512), slot 3 = its BC5 normal map.
    const auto mt = ReadMaterialTextures(p, 0xc2ad0);
    CHECK(mt && mt->material == 0xd22d0);
    if (!mt) return;
    CHECK_EQ(mt->slots[1], 0x51cba6a1f6d94abaull);
    CHECK_EQ(mt->slots[3], 0x51cba6a234dfa624ull);
    const auto it = by_id.find(mt->slots[1]);
    CHECK(it != by_id.end() && it->second.format == TexelFormat::BC1 && it->second.width == 512);
    // Every geometry part of the hero has a material with a colour map in this package.
    u32 with_albedo = 0;
    for (const auto& r : FindGeometry(p)) {
        const auto m = ReadMaterialTextures(p, r.offset);
        if (m && m->slots[1] && by_id.contains(m->slots[1])) ++with_albedo;
    }
    std::printf("    geometry with a colour map in this package: %u\n", with_albedo);
    CHECK(with_albedo >= 27);  // measured: 27 (another 10 colour maps live in other packages)
    CHECK(mt->bindings.size() == 5 && mt->bindings[4].category == 3);  // second category-3 entry kept
}
