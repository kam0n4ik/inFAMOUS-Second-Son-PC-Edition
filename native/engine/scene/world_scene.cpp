#include "scene/world_scene.h"

#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

#include "core/clock.h"
#include "core/log.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/world_chunk.h"

namespace sspc {
namespace {

Vec4 SlotColor(u32 geometry_offset) {
    // Stable muted tint per geometry so separate assets stay distinguishable (no textures yet).
    u32 h = geometry_offset * 2654435761u;
    const float t0 = ((h >> 8) & 0xff) / 255.0f, t1 = ((h >> 16) & 0xff) / 255.0f;
    return {0.50f + 0.25f * t0, 0.52f + 0.18f * t1, 0.45f + 0.15f * t0 * t1, 1.0f};
}

}  // namespace

bool LoadWorldChunk(const std::wstring& package_path, Renderer& renderer, JobSystem& jobs, WorldScene& out,
                    std::string& error) {
    const Clock clock;
    out = WorldScene{};
    res::KcapFile file;
    if (const auto e = file.Open(package_path); e != res::KcapError::Ok) {
        error = std::format("{}: {}", log::Narrow(package_path), res::ToString(e));
        return false;
    }
    const res::KcapPackage& pkg = file.Package();
    const auto scenes = res::FindChunkScenes(pkg);
    if (scenes.empty()) {
        error = "no chunk scene object in package";
        return false;
    }

    // Unique geometry records referenced by drawable instances.
    std::map<u32, size_t> slot_of;
    std::vector<res::GeometryRecord> records;
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            ++out.instances_total;
            if (inst.kind != 0) continue;
            const auto& g = sc.geometry[inst.geometry];
            if (!g) {
                ++out.instances_external;
                continue;
            }
            if (slot_of.contains(*g)) continue;
            if (auto rec = res::ReadGeometry(pkg, *g)) {
                slot_of[*g] = records.size();
                records.push_back(std::move(*rec));
            }
        }
    }
    std::vector<res::MeshData> meshes(records.size());
    std::vector<char> ok(records.size(), 0);
    jobs.ParallelFor(static_cast<u32>(records.size()),
                     [&](u32 i) { ok[i] = res::DecodeGeometry(pkg, records[i], meshes[i], nullptr) ? 1 : 0; });
    std::vector<MeshHandle> handles(records.size(), kInvalidMesh);
    for (size_t i = 0; i < records.size(); ++i) {
        if (!ok[i]) continue;
        handles[i] = renderer.UploadMesh(meshes[i], "world");
        if (handles[i] != kInvalidMesh) ++out.geometry_uploaded;
    }

    // Instances -> draw items + walkable triangles; spawn = densest 20 m cell of instances.
    std::unordered_map<u64, u32> density;
    auto cell_key = [](const Vec3& p) {
        return (static_cast<u64>(static_cast<u32>(static_cast<s32>(std::floor(p.x / 20)))) << 32) |
               static_cast<u32>(static_cast<s32>(std::floor(p.y / 20)));
    };
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            if (inst.kind != 0 || !sc.geometry[inst.geometry]) continue;
            const auto it = slot_of.find(*sc.geometry[inst.geometry]);
            if (it == slot_of.end() || handles[it->second] == kInvalidMesh) continue;
            const Mat4 world = res::ChunkMatrixToWorld(sc.matrices[inst.matrix]);
            out.draws.push_back({handles[it->second], world, SlotColor(*sc.geometry[inst.geometry])});
            out.terrain.AddMesh(meshes[it->second].positions, meshes[it->second].indices, world);
            ++out.instances_drawn;
            ++density[cell_key({world.m[3][0], world.m[3][1], 0})];
        }
    }
    out.terrain.Build(2.0f);
    u64 best_key = 0;
    u32 best = 0;
    for (const auto& [k, n] : density) {
        if (n > best) {
            best = n;
            best_key = k;
        }
    }
    const float sx = (static_cast<s32>(best_key >> 32) + 0.5f) * 20.0f;
    const float sy = (static_cast<s32>(best_key & 0xffffffffu) + 0.5f) * 20.0f;
    const auto ground = out.terrain.LowestAt(sx, sy);
    out.spawn = {sx, sy, ground ? *ground + 0.05f : 0.0f};
    out.load_seconds = clock.Now();
    log::Info("world", "{}: {} scene objects, {} instances ({} drawn, {} external), {} meshes, {} walkable tris, "
              "spawn ({:.1f}, {:.1f}, {:.1f}) in {:.0f} ms",
              log::Narrow(package_path), scenes.size(), out.instances_total, out.instances_drawn,
              out.instances_external, out.geometry_uploaded, out.terrain.TriangleCount(), out.spawn.x, out.spawn.y,
              out.spawn.z, out.load_seconds * 1000);
    return true;
}

}  // namespace sspc
