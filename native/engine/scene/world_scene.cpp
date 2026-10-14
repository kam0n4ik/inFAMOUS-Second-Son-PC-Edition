#include "scene/world_scene.h"

#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

#include "core/clock.h"
#include "core/log.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/texture.h"
#include "resources/exports.h"
#include "resources/texture_library.h"
#include "resources/world_chunk.h"

namespace sspc {
namespace {

Vec4 SlotColor(u32 geometry_offset) {
    // Stable muted tint per geometry so separate assets stay distinguishable (no textures yet).
    u32 h = geometry_offset * 2654435761u;
    const float t0 = ((h >> 8) & 0xff) / 255.0f, t1 = ((h >> 16) & 0xff) / 255.0f;
    return {0.50f + 0.25f * t0, 0.52f + 0.18f * t1, 0.45f + 0.15f * t0 * t1, 0.0f};
}

}  // namespace

bool LoadWorldChunk(const std::wstring& package_path, const std::vector<std::wstring>& texture_packages,
                    Renderer& renderer, JobSystem& jobs, WorldScene& out, std::string& error) {
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

    // Null geometry slots are filled through KNLX by objects other packages export (shared packages).
    res::ExportIndex exports;
    for (const auto& p : texture_packages) exports.AddPackage(p);
    std::unordered_map<u32, std::array<u8, 16>> external_key;  // slot location -> export key
    for (const auto& x : pkg.ExternalLinks()) {
        if (x.addend == 0) external_key[x.location] = x.key;
    }
    using GeoKey = std::pair<const res::KcapPackage*, u32>;
    auto resolve = [&](const res::ChunkScene& sc, u16 slot) -> std::optional<GeoKey> {
        if (sc.geometry[slot]) return GeoKey{&pkg, *sc.geometry[slot]};
        const auto k = external_key.find(sc.geometry_array + slot * 8u);
        if (k == external_key.end()) return std::nullopt;
        const auto ref = exports.Find(k->second);
        if (!ref) return std::nullopt;
        return GeoKey{ref->package, ref->object};
    };

    // Unique geometry records referenced by drawable instances.
    std::map<GeoKey, size_t> slot_of;
    std::vector<std::pair<const res::KcapPackage*, res::GeometryRecord>> records;
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            ++out.instances_total;
            if (inst.kind != 0) continue;
            const auto g = resolve(sc, inst.geometry);
            if (!g) {
                ++out.instances_external;
                continue;
            }
            if (!sc.geometry[inst.geometry]) ++out.instances_resolved;
            if (slot_of.contains(*g)) continue;
            if (auto rec = res::ReadGeometry(*g->first, g->second)) {
                slot_of[*g] = records.size();
                records.emplace_back(g->first, std::move(*rec));
            }
        }
    }
    std::vector<res::MeshData> meshes(records.size());
    std::vector<char> ok(records.size(), 0);
    jobs.ParallelFor(static_cast<u32>(records.size()), [&](u32 i) {
        ok[i] = res::DecodeGeometry(*records[i].first, records[i].second, meshes[i], nullptr) ? 1 : 0;
    });
    std::vector<MeshHandle> handles(records.size(), kInvalidMesh);
    for (size_t i = 0; i < records.size(); ++i) {
        if (!ok[i]) continue;
        handles[i] = renderer.UploadMesh(meshes[i], "world");
        if (handles[i] != kInvalidMesh) ++out.geometry_uploaded;
    }

    // Colour maps (material slot 1), decoded in parallel and uploaded once per texture id.
    res::TextureLibrary lib;
    lib.AddPackage(pkg);
    for (const auto& p : texture_packages) {
        if (!lib.AddPackage(p)) log::Warn("world", "texture package {} not available", log::Narrow(p));
    }
    std::vector<u64> albedo(records.size(), 0);
    std::vector<u64> wanted;
    std::unordered_map<u64, size_t> wanted_index;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto mt = res::ReadMaterialTextures(*records[i].first, records[i].second.offset);
        if (!mt || !mt->slots[1] || !lib.Contains(mt->slots[1])) continue;
        albedo[i] = mt->slots[1];
        if (wanted_index.emplace(albedo[i], wanted.size()).second) wanted.push_back(albedo[i]);
    }
    std::vector<res::TextureData> tex_data(wanted.size());
    jobs.ParallelFor(static_cast<u32>(wanted.size()), [&](u32 i) { lib.Decode(wanted[i], tex_data[i], nullptr); });
    std::unordered_map<u64, TextureHandle> tex_handle;
    for (const auto& t : tex_data) {
        if (!t.mips.empty()) tex_handle[t.id] = renderer.UploadTexture(t, /*srgb=*/true);
    }
    out.textures_uploaded = static_cast<u32>(tex_handle.size());

    // Instances -> draw items + walkable triangles; spawn = densest 20 m cell of instances.
    std::unordered_map<u64, u32> density;
    auto cell_key = [](const Vec3& p) {
        return (static_cast<u64>(static_cast<u32>(static_cast<s32>(std::floor(p.x / 20)))) << 32) |
               static_cast<u32>(static_cast<s32>(std::floor(p.y / 20)));
    };
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            if (inst.kind != 0) continue;
            const auto gk = resolve(sc, inst.geometry);
            if (!gk) continue;
            const auto it = slot_of.find(*gk);
            if (it == slot_of.end() || handles[it->second] == kInvalidMesh) continue;
            const Mat4 world = res::ChunkMatrixToWorld(sc.matrices[inst.matrix]);
            const auto th = tex_handle.find(albedo[it->second]);
            if (th != tex_handle.end()) {
                out.draws.push_back({handles[it->second], world, {1, 1, 1, 0.5f}, th->second});  // alpha-tested
            } else {
                out.draws.push_back({handles[it->second], world, SlotColor(gk->second)});
            }
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
    log::Info("world", "{}: {} scene objects, {} instances ({} drawn, {} external), {} meshes, {} textures, {} walkable tris, "
              "spawn ({:.1f}, {:.1f}, {:.1f}) in {:.0f} ms",
              log::Narrow(package_path), scenes.size(), out.instances_total, out.instances_drawn,
              out.instances_external, out.geometry_uploaded, out.textures_uploaded, out.terrain.TriangleCount(), out.spawn.x, out.spawn.y,
              out.spawn.z, out.load_seconds * 1000);
    return true;
}

}  // namespace sspc
