// Tests against the real game package (hero.xpps). Skipped when the game files are not present.
// Data location: environment variable SSPC_GAME_DIR, default E:\shdps4\CUSA00004.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/file.h"
#include "resources/gcn_buffer.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/spcmesh.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;

namespace {

std::wstring GameDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"SSPC_GAME_DIR", buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"E:\\shdps4\\CUSA00004";
}

bool OpenHero(KcapFile& f) {
    const std::wstring path = GameDir() + L"\\art\\cache\\hero.xpps";
    if (!file::Exists(path)) {
        test::Skip("hero.xpps not found (set SSPC_GAME_DIR)");
        return false;
    }
    const KcapError e = f.Open(path);
    CHECK_EQ(e, KcapError::Ok);
    return e == KcapError::Ok;
}

}  // namespace

TEST(real_hero_package_structure) {
    KcapFile f;
    if (!OpenHero(f)) return;
    const KcapPackage& p = f.Package();
    CHECK_EQ(p.Header().version, 13);
    CHECK_EQ(p.Segments().size(), static_cast<size_t>(6));
    CHECK(p.FindChunk(kTagKnli) && p.FindChunk(kTagDic) && p.FindChunk(kTagPyto) && p.FindChunk(kTagEnd));
    CHECK_EQ(p.Links().size(), static_cast<size_t>(8720));  // measured with the Python reader
    CHECK_EQ(p.InvalidLinkCount(), 0u);
    const auto root = ReadRootGeometryList(p);
    CHECK(root && root->count == 41);
}

TEST(real_hero_geometry_decodes_and_normals_match_faces) {
    KcapFile f;
    if (!OpenHero(f)) return;
    const KcapPackage& p = f.Package();
    const auto root = ReadRootGeometryList(p);
    if (!root) return;
    u32 decoded = 0, with_normals = 0;
    double dot_sum = 0, positive = 0, faces = 0;
    for (u32 i = 0; i < root->count; ++i) {
        const auto ptr = p.PointerAt(root->list + i * 8);
        CHECK(ptr.has_value());
        if (!ptr) continue;
        const auto rec = ReadGeometry(p, *ptr);
        CHECK(rec.has_value());
        if (!rec) continue;
        MeshData m;
        std::string err;
        const bool ok = DecodeGeometry(p, *rec, m, &err);
        if (!ok) std::printf("    %s\n", err.c_str());
        CHECK(ok);
        if (!ok) continue;
        ++decoded;
        if (m.normals.empty()) continue;
        ++with_normals;
        // Vertex normals must agree with the triangle normals. This checks the 2_10_10_10
        // component order (a wrong order gives |dot| ~ random) and reveals the winding.
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const Vec3& a = m.positions[m.indices[t]];
            const Vec3 n = Cross(m.positions[m.indices[t + 1]] - a, m.positions[m.indices[t + 2]] - a);
            if (Length(n) < 1e-9f) continue;
            const Vec3 fn = Normalize(n);
            const Vec3 vn = Normalize(m.normals[m.indices[t]] + m.normals[m.indices[t + 1]] + m.normals[m.indices[t + 2]]);
            const float d = Dot(fn, vn);
            dot_sum += std::abs(d);
            positive += d > 0 ? 1 : 0;
            faces += 1;
        }
    }
    CHECK_EQ(decoded, 41u);
    CHECK(with_normals >= 30);
    const double mean_abs_dot = dot_sum / std::max(faces, 1.0);
    const double ccw_share = positive / std::max(faces, 1.0);
    std::printf("    %u parts decoded, %u with normals; mean |face . vertex normal| = %.3f; CCW-front share = %.3f\n",
                decoded, with_normals, mean_abs_dot, ccw_share);
    CHECK(mean_abs_dot > 0.8);
}

TEST(real_hero_spcmesh_roundtrip) {
    KcapFile f;
    if (!OpenHero(f)) return;
    const KcapPackage& p = f.Package();
    std::vector<MeshData> meshes;
    for (const auto& r : FindGeometry(p)) {
        MeshData m;
        if (DecodeGeometry(p, r, m, nullptr)) meshes.push_back(std::move(m));
    }
    CHECK(meshes.size() >= 41);
    const auto bytes = WriteSpcMesh(meshes, p.Header().hash);
    std::vector<MeshData> back;
    std::string err;
    CHECK(ReadSpcMesh(bytes, back, &err));
    bool same = back.size() == meshes.size();
    for (size_t i = 0; same && i < meshes.size(); ++i) {
        same = back[i].indices == meshes[i].indices && back[i].positions.size() == meshes[i].positions.size() &&
               std::memcmp(back[i].positions.data(), meshes[i].positions.data(), meshes[i].positions.size() * sizeof(Vec3)) == 0;
    }
    CHECK(same);
    std::printf("    %zu meshes, .spcmesh %zu bytes\n", meshes.size(), bytes.size());
}
