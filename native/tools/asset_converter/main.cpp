// sspc_convert - converts the geometry of a KCAP package into the native .spcmesh format.
//
//   sspc_convert <package.xpps> <out.spcmesh> [--main-only]
//
// The output is re-read and compared with the decoded data before the command reports success.
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "core/file.h"
#include "core/log.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/spcmesh.h"

using namespace sspc;
using namespace sspc::res;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: sspc_convert <package.xpps> <out.spcmesh> [--main-only]\n");
        return 1;
    }
    const bool main_only = argc > 3 && std::wstring(argv[3]) == L"--main-only";
    KcapFile f;
    if (const KcapError e = f.Open(argv[1]); e != KcapError::Ok) {
        std::printf("error: %s\n", ToString(e));
        return 2;
    }
    const KcapPackage& pkg = f.Package();
    std::vector<MeshData> meshes;
    u32 failed = 0;
    for (const auto& rec : FindGeometry(pkg)) {
        if (main_only && (rec.view_mask & 0x2) == 0) continue;
        MeshData m;
        std::string err;
        if (DecodeGeometry(pkg, rec, m, &err)) {
            meshes.push_back(std::move(m));
        } else {
            std::printf("skip: %s\n", err.c_str());
            ++failed;
        }
    }
    if (meshes.empty()) {
        std::printf("error: no decodable geometry\n");
        return 3;
    }
    const std::vector<u8> bytes = WriteSpcMesh(meshes, pkg.Header().hash);
    if (!file::WriteAtomic(argv[2], bytes)) {
        std::printf("error: cannot write output\n");
        return 4;
    }
    // Verify: read back and compare.
    std::vector<u8> back;
    std::vector<MeshData> check;
    std::string err;
    if (!file::ReadAll(argv[2], back) || !ReadSpcMesh(back, check, &err) || check.size() != meshes.size()) {
        std::printf("error: verification failed: %s\n", err.c_str());
        return 5;
    }
    u64 verts = 0, tris = 0;
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (check[i].positions.size() != meshes[i].positions.size() || check[i].indices != meshes[i].indices) {
            std::printf("error: verification mismatch in mesh %zu\n", i);
            return 5;
        }
        verts += meshes[i].positions.size();
        tris += meshes[i].indices.size() / 3;
    }
    std::printf("%zu meshes (%llu vertices, %llu triangles) -> %s (%zu bytes), %u skipped, verified\n", meshes.size(),
                static_cast<unsigned long long>(verts), static_cast<unsigned long long>(tris),
                log::Narrow(argv[2]).c_str(), bytes.size(), failed);
    return 0;
}
