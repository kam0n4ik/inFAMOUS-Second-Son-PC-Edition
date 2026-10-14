#include <cmath>
#include <cstddef>
#include <cstring>

#include "core/crc32.h"
#include "kcap_builder.h"
#include "resources/gcn_buffer.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/spcmesh.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;
using sspc::test::KcapBuilder;

namespace {

u32 Dst(u32 dfmt, u32 nfmt) { return 4 | (5 << 3) | (6 << 6) | (7 << 9) | (nfmt << 12) | (dfmt << 15); }

struct GeoOpts {
    u32 index2 = 2;
    float aabb_max_x = 1.0f;
    u32 pos_dfmt = 12;
    u32 index_count = 3;
};

// One triangle: positions (0,0,0) (1,0,0) (0,1,0), normal +Z, uv (1, 0.5); layout as in hero.xpps.
std::vector<u8> GeometryPackage(const GeoOpts& o = {}) {
    KcapBuilder b;
    b.cpu.resize(0x200, 0);
    b.gpu.resize(0x100, 0);
    const u32 g = 0x100, vs = 0x180, gpu = b.GpuBase();
    b.Put<u32>(b.cpu, 0x58, 0x80);
    b.Put<u32>(b.cpu, 0x60, 1);
    b.Put<u32>(b.cpu, 0x80, g);
    for (u32 i = 0; i < 16; ++i) b.cpu[g + i] = static_cast<u8>(0xA0 + i);
    b.Put<u64>(b.cpu, g + 0x18, 1ull | (0x600bull << 16) | (0ull << 48) | (6ull << 56));
    const float aabb[6] = {0, 0, 0, o.aabb_max_x, 1, 0};
    std::memcpy(b.cpu.data() + g + 0x20, aabb, sizeof(aabb));
    b.Put<u32>(b.cpu, g + 0x58, gpu + 0x80);
    b.Put<u32>(b.cpu, g + 0x60, vs);
    b.Put<u32>(b.cpu, g + 0x68, o.index_count);
    b.Put<u8>(b.cpu, g + 0x6c, 3);
    const u32 vsharps[3][4] = {
        {gpu + 0x00, 20u << 16, 3, Dst(o.pos_dfmt, 1)},  // position 16_16_16_16 snorm
        {gpu + 0x40, 8u << 16, 3, Dst(9, 1)},            // normal 2_10_10_10 snorm
        {gpu + 0x44, 8u << 16, 3, Dst(5, 7)},            // uv 16_16 float
    };
    std::memcpy(b.cpu.data() + vs, vsharps, sizeof(vsharps));
    const s16 pos[3][4] = {{0, 0, 0, 0}, {32767, 0, 0, 0}, {0, 32767, 0, 0}};
    for (u32 v = 0; v < 3; ++v) std::memcpy(b.gpu.data() + v * 20, pos[v], 8);
    for (u32 v = 0; v < 3; ++v) {
        b.Put<u32>(b.gpu, 0x40 + v * 8, 511u << 20);  // (0, 0, 1)
        b.Put<u16>(b.gpu, 0x44 + v * 8, 0x3C00);      // 1.0
        b.Put<u16>(b.gpu, 0x46 + v * 8, 0x3800);      // 0.5
    }
    const u16 idx[3] = {0, 1, static_cast<u16>(o.index2)};
    std::memcpy(b.gpu.data() + 0x80, idx, sizeof(idx));
    b.links = {{0, 0x58}, {0, 0x80}, {3, g + 0x58}, {0, g + 0x60}, {2, vs}, {2, vs + 16}, {2, vs + 32}};
    return b.Build();
}

}  // namespace

TEST(gcn_element_decoding) {
    CHECK(HalfToFloat(0x3C00) == 1.0f && HalfToFloat(0xC000) == -2.0f && HalfToFloat(0x0000) == 0.0f);
    CHECK(std::abs(HalfToFloat(0x0001) - 5.96046448e-8f) < 1e-12f);  // smallest subnormal
    float v[4];
    const s16 s[4] = {-32768, 32767, 0, -16384};
    CHECK(DecodeElement(reinterpret_cast<const u8*>(s), DataFormat::D16_16_16_16, NumFormat::Snorm, v));
    CHECK(v[0] == -1.0f && v[1] == 1.0f && v[2] == 0.0f && std::abs(v[3] + 0.5f) < 1e-4f);
    const u32 packed = 511u | (0x201u << 10) | (0u << 20) | (1u << 30);  // x=+1, y=-1 (0x201 = -511)
    CHECK(DecodeElement(reinterpret_cast<const u8*>(&packed), DataFormat::D2_10_10_10, NumFormat::Snorm, v));
    CHECK(v[0] == 1.0f && v[1] == -1.0f && v[2] == 0.0f && v[3] == 1.0f);
    const u8 rgba[4] = {255, 0, 128, 255};
    CHECK(DecodeElement(rgba, DataFormat::D8_8_8_8, NumFormat::Unorm, v));
    CHECK(v[0] == 1.0f && v[1] == 0.0f && std::abs(v[2] - 128 / 255.0f) < 1e-6f);
    CHECK(!DecodeElement(rgba, DataFormat::D8_8_8_8, NumFormat::Float, v));  // no 8-bit float
    float sel[4];
    const float in[4] = {1, 2, 3, 4};
    ApplyDstSel(in, {6, 5, 4, 1}, sel);  // z y x 1
    CHECK(sel[0] == 3 && sel[1] == 2 && sel[2] == 1 && sel[3] == 1);
    const u32 dw[4] = {0x1234, (20u << 16) | 0x5, 77, Dst(12, 6)};
    const VSharp vs = DecodeVSharp(dw);
    CHECK(vs.base == 0x500001234ull && vs.stride == 20 && vs.num_records == 77);
    CHECK(vs.dfmt == DataFormat::D16_16_16_16 && vs.nfmt == NumFormat::SnormNz);
}

TEST(geometry_record_found_and_decoded) {
    const auto bytes = GeometryPackage();
    KcapPackage p;
    CHECK_EQ(p.Parse(bytes), KcapError::Ok);
    const auto root = ReadRootGeometryList(p);
    CHECK(root && root->list == 0x80 && root->count == 1);
    const auto recs = FindGeometry(p);
    CHECK_EQ(recs.size(), static_cast<size_t>(1));
    if (recs.empty()) return;
    const GeometryRecord& r = recs[0];
    CHECK_EQ(r.offset, 0x100u);
    CHECK_EQ(r.view_mask, 0x600b);
    CHECK(r.lod_min == 0 && r.lod_max == 6 && r.triangle_count == 1);
    CHECK_EQ(r.streams.size(), static_cast<size_t>(3));
    MeshData m;
    std::string err;
    CHECK(DecodeGeometry(p, r, m, &err));
    CHECK_EQ(m.positions.size(), static_cast<size_t>(3));
    CHECK(m.positions[1].x == 1.0f && m.positions[2].y == 1.0f && m.positions[0].z == 0.0f);
    CHECK(m.normals.size() == 3 && m.normals[0].z == 1.0f);
    CHECK(m.uv0.size() == 3 && m.uv0[0].x == 1.0f && m.uv0[0].y == 0.5f);
    CHECK((m.indices == std::vector<u32>{0, 1, 2}));
    CHECK(m.id[0] == 0xA0 && m.id[15] == 0xAF);
}

TEST(geometry_rejects_inconsistent_data) {
    {
        GeoOpts o;
        o.index2 = 5;  // index beyond the vertex count
        const auto bytes = GeometryPackage(o);
        KcapPackage p;
        CHECK_EQ(p.Parse(bytes), KcapError::Ok);
        const auto recs = FindGeometry(p);
        MeshData m;
        std::string err;
        CHECK(recs.size() == 1 && !DecodeGeometry(p, recs[0], m, &err));
        CHECK(err.find("index") != std::string::npos);
    }
    {
        GeoOpts o;
        o.aabb_max_x = 0.5f;  // positions outside the record's bounding box
        const auto bytes = GeometryPackage(o);
        KcapPackage p;
        p.Parse(bytes);
        const auto recs = FindGeometry(p);
        MeshData m;
        std::string err;
        CHECK(recs.size() == 1 && !DecodeGeometry(p, recs[0], m, &err));
        CHECK(err.find("outside") != std::string::npos);
    }
    {
        GeoOpts o;
        o.pos_dfmt = 0;  // invalid buffer format: record must not be accepted at all
        const auto bytes = GeometryPackage(o);
        KcapPackage p;
        p.Parse(bytes);
        CHECK(FindGeometry(p).empty());
    }
    {
        GeoOpts o;
        o.index_count = 4;  // not a triangle list
        const auto bytes = GeometryPackage(o);
        KcapPackage p;
        p.Parse(bytes);
        CHECK(FindGeometry(p).empty());
    }
}

TEST(spcmesh_roundtrip_and_corruption) {
    const auto bytes = GeometryPackage();
    KcapPackage p;
    p.Parse(bytes);
    MeshData m;
    DecodeGeometry(p, FindGeometry(p)[0], m, nullptr);
    std::vector<MeshData> in = {m, m};
    in[1].normals.clear();
    in[1].uv0.clear();
    const auto file = WriteSpcMesh(in, p.Header().hash);
    std::vector<MeshData> out;
    std::string err;
    CHECK(ReadSpcMesh(file, out, &err));
    CHECK(out.size() == 2 && out[0].indices == m.indices && out[0].normals.size() == 3 && out[1].normals.empty());
    CHECK(out[0].positions[1].x == 1.0f && out[0].uv0[2].y == 0.5f && out[0].id == m.id);

    auto bad = file;
    bad[bad.size() - 1] ^= 1;
    CHECK(!ReadSpcMesh(bad, out, &err) && err.find("CRC") != std::string::npos);
    bad = file;
    bad.resize(bad.size() - 4);
    CHECK(!ReadSpcMesh(bad, out, &err));
    bad = file;
    bad[4] = 2;  // version
    CHECK(!ReadSpcMesh(bad, out, &err) && err.find("version") != std::string::npos);
    CHECK(!ReadSpcMesh(std::span<const u8>(file.data(), 10), out, &err));

    // Index out of range with a valid CRC: structural validation must still catch it.
    bad = file;
    SpcMeshEntry e;
    std::memcpy(&e, bad.data() + sizeof(SpcMeshHeader), sizeof(e));
    const u32 big = 99;
    std::memcpy(bad.data() + e.index_offset, &big, 4);
    const u32 crc = Crc32(std::span(bad).subspan(sizeof(SpcMeshHeader)));
    std::memcpy(bad.data() + offsetof(SpcMeshHeader, crc32), &crc, 4);
    CHECK(!ReadSpcMesh(bad, out, &err) && err.find("index") != std::string::npos);
}
