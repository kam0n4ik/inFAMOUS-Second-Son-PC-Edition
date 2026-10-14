#include <cmath>
#include <cstring>

#include "core/file.h"
#include "gameplay/character.h"
#include "gameplay/terrain.h"
#include "kcap_builder.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/world_chunk.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;
using sspc::test::KcapBuilder;

namespace {

u32 Dst(u32 dfmt, u32 nfmt) { return 4 | (5 << 3) | (6 << 6) | (7 << 9) | (nfmt << 12) | (dfmt << 15); }

// Chunk with a scene object at 0x100: 3 instances, 2 matrices, 2 geometry slots (slot 1 external).
KcapBuilder ChunkBuilder() {
    KcapBuilder b;
    b.cpu.resize(0x400, 0);
    b.gpu.resize(0x100, 0);
    const u32 obj = 0x100, inst = 0x200, mats = 0x220, slots = 0x280, g = 0x300, vs = 0x380, gpu = b.GpuBase();
    b.Put<u32>(b.cpu, obj + 0xa8, inst);
    b.Put<u64>(b.cpu, obj + 0xb0, 3);
    b.Put<u32>(b.cpu, obj + 0xb8, mats);
    b.Put<u64>(b.cpu, obj + 0xc0, 2);
    b.Put<u32>(b.cpu, obj + 0xc8, slots);
    b.Put<u64>(b.cpu, obj + 0xd0, 2);
    const u16 instances[3][4] = {{0, 0, (0 << 2) | 1, 7}, {0, 1, (0 << 2) | 1, 7}, {0, 1, (1 << 2) | 1, 7}};
    std::memcpy(b.cpu.data() + inst, instances, sizeof(instances));
    // Matrix 0: identity, translation (100, 200, 300) cm. Matrix 1: rotate 90 deg about Z, scale 2.
    const float m0[12] = {1, 0, 0, 100, 0, 1, 0, 200, 0, 0, 1, 300};
    const float m1[12] = {0, -2, 0, 0, 2, 0, 0, 0, 0, 0, 2, 50};
    std::memcpy(b.cpu.data() + mats, m0, sizeof(m0));
    std::memcpy(b.cpu.data() + mats + 48, m1, sizeof(m1));
    b.Put<u32>(b.cpu, slots, g);  // slot 0 -> geometry; slot 1 stays null (external)
    // One-triangle geometry: normalised (0,0,0) (1,0,0) (0,1,0), dequant offset 0, scale 100 cm.
    b.Put<u64>(b.cpu, g + 0x18, 1ull | (0x600bull << 16) | (6ull << 56));
    const float aabb_dq[10] = {0, 0, 0, 1, 1, 0, 0, 0, 0, 100};
    std::memcpy(b.cpu.data() + g + 0x20, aabb_dq, sizeof(aabb_dq));
    b.Put<u32>(b.cpu, g + 0x58, gpu + 0x80);
    b.Put<u32>(b.cpu, g + 0x60, vs);
    b.Put<u32>(b.cpu, g + 0x68, 3);
    b.Put<u8>(b.cpu, g + 0x6c, 1);
    const u32 v[4] = {gpu, 8u << 16, 3, Dst(12, 1)};
    std::memcpy(b.cpu.data() + vs, v, sizeof(v));
    const s16 pos[3][4] = {{0, 0, 0, 0}, {32767, 0, 0, 0}, {0, 32767, 0, 0}};
    std::memcpy(b.gpu.data(), pos, sizeof(pos));
    const u16 idx[3] = {0, 1, 2};
    std::memcpy(b.gpu.data() + 0x80, idx, sizeof(idx));
    b.links = {{0, obj + 0xa8}, {0, obj + 0xb8}, {0, obj + 0xc8}, {0, slots}, {3, g + 0x58}, {0, g + 0x60}, {2, vs}};
    return b;
}

}  // namespace

TEST(world_chunk_scene_detected_and_matrices_converted) {
    const auto bytes = ChunkBuilder().Build();
    KcapPackage p;
    CHECK_EQ(p.Parse(bytes), KcapError::Ok);
    const auto scenes = FindChunkScenes(p);
    CHECK_EQ(scenes.size(), static_cast<size_t>(1));
    if (scenes.empty()) return;
    const ChunkScene& s = scenes[0];
    CHECK_EQ(s.object, 0x100u);
    CHECK(s.instances.size() == 3 && s.matrices.size() == 2 && s.geometry.size() == 2);
    CHECK(s.geometry[0].value_or(0) == 0x300 && !s.geometry[1].has_value());
    CHECK(s.instances[2].geometry == 1 && s.instances[2].mode == 1 && s.instances[2].extra == 7);
    const Mat4 w0 = ChunkMatrixToWorld(s.matrices[0]);
    const Vec3 p0 = w0.TransformPoint({1, 0, 0});  // mesh metres -> world metres
    CHECK(std::abs(p0.x - 2.0f) < 1e-5f && std::abs(p0.y - 2.0f) < 1e-5f && std::abs(p0.z - 3.0f) < 1e-5f);
    const Mat4 w1 = ChunkMatrixToWorld(s.matrices[1]);
    const Vec3 p1 = w1.TransformPoint({1, 0, 0});  // rotated +90 deg about Z and scaled by 2
    CHECK(std::abs(p1.x) < 1e-5f && std::abs(p1.y - 2.0f) < 1e-5f && std::abs(p1.z - 0.5f) < 1e-5f);

    const auto rec = ReadGeometry(p, *s.geometry[0]);
    MeshData m;
    CHECK(rec && DecodeGeometry(p, *rec, m, nullptr));
    CHECK(m.positions.size() == 3 && std::abs(m.positions[1].x - 1.0f) < 1e-6f);  // 100 cm = 1 m
}

TEST(world_chunk_rejects_out_of_range_indices) {
    KcapBuilder b = ChunkBuilder();
    const u16 bad[4] = {0, 5, 0, 0};  // matrix index 5 of 2
    std::memcpy(b.cpu.data() + 0x200, bad, sizeof(bad));
    const auto bytes = b.Build();
    KcapPackage p;
    CHECK_EQ(p.Parse(bytes), KcapError::Ok);
    CHECK(FindChunkScenes(p).empty());
}

TEST(terrain_grid_heights_and_walking) {
    TerrainGrid t;
    // A 10 x 10 m floor at z = 1 and a 2 x 2 m platform at z = 1.3 (step) and a roof at z = 5.
    const std::vector<Vec3> quad = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    const std::vector<u32> idx = {0, 1, 2, 0, 2, 3};
    t.AddMesh(quad, idx, Mat4::Translation({0, 0, 1}));
    t.AddMesh({{4, 4, 0}, {6, 4, 0}, {6, 6, 0}, {4, 6, 0}}, idx, Mat4::Translation({0, 0, 1.3f}));
    t.AddMesh(quad, idx, Mat4::Translation({0, 0, 5}));
    // A vertical wall must not become walkable.
    t.AddMesh({{0, 0, 0}, {10, 0, 0}, {10, 0, 3}, {0, 0, 3}}, idx, Mat4::Identity());
    t.Build(2.0f);
    CHECK_EQ(t.TriangleCount(), static_cast<size_t>(6));
    CHECK(std::abs(t.HeightAt(2, 2, 3).value_or(-1) - 1.0f) < 1e-5f);    // below the roof
    CHECK(std::abs(t.HeightAt(2, 2, 10).value_or(-1) - 5.0f) < 1e-5f);   // roof is reachable from above
    CHECK(std::abs(t.LowestAt(5, 5).value_or(-1) - 1.0f) < 1e-5f);
    CHECK(!t.HeightAt(20, 20, 10).has_value());

    CharacterParams p;
    p.ground_plane = false;
    CharacterState s;
    s.position = {1, 5, 1};
    MoveInput fwd;
    fwd.y = 1;  // +X
    for (int i = 0; i < 120; ++i) StepCharacter(s, p, fwd, 1.0f / 60.0f, {}, &t);
    CHECK(s.grounded && s.position.x > 4.0f);
    for (int i = 0; i < 600 && s.position.x < 12.0f; ++i) StepCharacter(s, p, fwd, 1.0f / 60.0f, {}, &t);
    CHECK(!s.grounded || s.position.z < 1.0f);  // walked off the floor edge: falling
}

TEST(real_world_chunk_a1) {
    const std::wstring path = L"E:\\shdps4\\CUSA00004\\art\\cache\\a1.xpps";
    if (!file::Exists(path)) {
        test::Skip("a1.xpps not found");
        return;
    }
    KcapFile f;
    CHECK_EQ(f.Open(path), KcapError::Ok);
    const auto scenes = FindChunkScenes(f.Package());
    CHECK_EQ(scenes.size(), static_cast<size_t>(1));
    if (scenes.empty()) return;
    const ChunkScene& s = scenes[0];
    CHECK(s.instances.size() == 3788 && s.matrices.size() == 1432 && s.geometry.size() == 238);
    u32 drawable = 0, decoded = 0;
    for (const auto& i : s.instances) {
        if (i.kind == 0 && s.geometry[i.geometry]) ++drawable;
    }
    for (const auto& g : s.geometry) {
        if (!g) continue;
        const auto rec = ReadGeometry(f.Package(), *g);
        MeshData m;
        if (rec && DecodeGeometry(f.Package(), *rec, m, nullptr)) ++decoded;
    }
    std::printf("    a1: %u drawable instances, %u geometry slots decoded\n", drawable, decoded);
    CHECK_EQ(drawable, 1492u);  // measured with native/tools/research/kcap_chunk_scene.py
}
