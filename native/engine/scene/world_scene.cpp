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
                    Renderer& renderer, JobSystem& jobs, WorldCache& cache, WorldScene& out, std::string& error) {
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
    if (!cache.shared_loaded) {
        for (const auto& p : texture_packages) {
            cache.exports.AddPackage(p);
            if (!cache.textures.AddPackage(p)) log::Warn("world", "texture package {} not available", log::Narrow(p));
        }
        cache.shared_loaded = true;
    }
    const res::ExportIndex& exports = cache.exports;
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
    // GPU meshes: geometry of a shared package is uploaded once for all chunks.
    std::vector<MeshHandle> handles(records.size(), kInvalidMesh);
    for (size_t i = 0; i < records.size(); ++i) {
        if (!ok[i]) continue;
        const bool own = records[i].first == &pkg;
        const std::string key = (own ? log::Narrow(package_path)
                                     : std::format("shared@{:#x}", reinterpret_cast<uintptr_t>(records[i].first))) +
                                std::format(":{:#x}", records[i].second.offset);
        if (const auto it = cache.meshes.find(key); it != cache.meshes.end()) {
            handles[i] = it->second;
            ++cache.meshes_reused;
            continue;
        }
        handles[i] = renderer.UploadMesh(meshes[i], "world");
        if (handles[i] != kInvalidMesh) {
            ++out.geometry_uploaded;
            cache.meshes[key] = handles[i];
        }
    }

    // Colour maps (material slot 1): the chunk's own package first, then the shared packages;
    // decoded in parallel and uploaded once per texture id for all chunks.
    res::TextureLibrary own_lib;
    own_lib.AddPackage(pkg);
    auto has_texture = [&](u64 id) { return own_lib.Contains(id) || cache.textures.Contains(id); };
    std::vector<u64> albedo(records.size(), 0);
    std::vector<u64> wanted;
    std::unordered_map<u64, size_t> wanted_index;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto mt = res::ReadMaterialTextures(*records[i].first, records[i].second.offset);
        if (!mt || !mt->slots[1] || !has_texture(mt->slots[1])) continue;
        albedo[i] = mt->slots[1];
        if (cache.texture_handles.contains(albedo[i])) continue;
        if (wanted_index.emplace(albedo[i], wanted.size()).second) wanted.push_back(albedo[i]);
    }
    std::vector<res::TextureData> tex_data(wanted.size());
    jobs.ParallelFor(static_cast<u32>(wanted.size()), [&](u32 i) {
        if (own_lib.Contains(wanted[i])) own_lib.Decode(wanted[i], tex_data[i], nullptr);
        else cache.textures.Decode(wanted[i], tex_data[i], nullptr);
    });
    for (const auto& t : tex_data) {
        if (!t.mips.empty()) {
            cache.texture_handles[t.id] = renderer.UploadTexture(t, /*srgb=*/true);
            ++out.textures_uploaded;
        }
    }
    const auto& tex_handle = cache.texture_handles;

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

    // Collision: the chunk's Havok packfiles (static compound shapes of compressed meshes).
    std::vector<Vec3> col_tris;
    for (const u64 at : res::FindHavokPackfiles(pkg.Data())) {
        res::HavokPackfile pf;
        std::string herr;
        if (!pf.Parse(pkg.Data(), at, &herr) || !res::DecodeStaticCollision(pf, col_tris, out.havok, &herr)) {
            log::Warn("world", "Havok packfile at {:#x}: {}", at, herr);
        }
    }
    out.collision_from_havok = !col_tris.empty();
    const size_t havok_tris = col_tris.size() / 3;
    // The shipped Havok shapes of the village chunks hold rocks, trunks and buildings but no ground
    // surface. Ground (APPROXIMATION): walkable render triangles; with no Havok data at all, every
    // drawn render triangle collides.
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            if (inst.kind != 0) continue;
            const auto gk = resolve(sc, inst.geometry);
            if (!gk) continue;
            const auto it = slot_of.find(*gk);
            if (it == slot_of.end() || !ok[it->second]) continue;
            const Mat4 world = res::ChunkMatrixToWorld(sc.matrices[inst.matrix]);
            const auto& md = meshes[it->second];
            for (size_t i = 0; i + 2 < md.indices.size(); i += 3) {
                const Vec3 a = world.TransformPoint(md.positions[md.indices[i]]);
                const Vec3 b = world.TransformPoint(md.positions[md.indices[i + 1]]);
                const Vec3 c = world.TransformPoint(md.positions[md.indices[i + 2]]);
                const Vec3 n = Cross(b - a, c - a);
                const float len = Length(n);
                if (out.collision_from_havok && (len < 1e-8f || std::abs(n.z) / len < 0.6f)) continue;
                col_tris.insert(col_tris.end(), {a, b, c});
            }
        }
    }
    {
        res::MeshData dbg;  // debug view
        dbg.positions = col_tris;
        dbg.indices.resize(col_tris.size());
        for (u32 i = 0; i < dbg.indices.size(); ++i) dbg.indices[i] = i;
        if (!dbg.positions.empty()) out.collision_mesh = renderer.UploadMesh(dbg, "collision");
    }
    const size_t col_count = col_tris.size() / 3;
    std::vector<u8> layers(col_count, kCollisionFallback);
    std::fill(layers.begin(), layers.begin() + static_cast<std::ptrdiff_t>(havok_tris), kCollisionHavok);
    out.collision.Build(std::move(col_tris), std::move(layers));
    log::Info("world", "collision: {} Havok triangles ({} compound shapes, {} instances, {} meshes, {} convex props not "
              "decoded) + {} render triangles ({}), BVH {} nodes depth {}",
              havok_tris, out.havok.compound_shapes, out.havok.instances, out.havok.mesh_shapes,
              out.havok.convex_shapes_skipped, col_count - havok_tris,
              out.collision_from_havok ? "walkable ground, approximation" : "all, approximation", out.collision.NodeCount(),
              out.collision.Depth());
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
    if (!out.collision.Empty()) {
        // Stand on the collision surface: the top one at the spawn column, else the nearest drawn
        // instance that has collision under it.
        const float top = out.collision.BoundsMax().z + 1.0f, drop = top - out.collision.BoundsMin().z + 1.0f;
        auto ground_at = [&](float x, float y) { return out.collision.Raycast({x, y, top}, {0, 0, -1}, drop); };
        if (auto hit = ground_at(sx, sy); hit && hit->normal.z > 0.6f) {
            out.spawn = hit->point + Vec3{0, 0, 0.05f};
        } else {
            float best = 1e30f;
            for (const auto& d : out.draws) {
                const Vec3 p{d.world.m[3][0], d.world.m[3][1], d.world.m[3][2]};
                const float dist = (p.x - sx) * (p.x - sx) + (p.y - sy) * (p.y - sy);
                if (dist >= best) continue;
                if (auto h = ground_at(p.x + 1.0f, p.y + 1.0f); h && h->normal.z > 0.6f) {
                    best = dist;
                    out.spawn = h->point + Vec3{0, 0, 0.05f};
                }
            }
        }
    }
    out.load_seconds = clock.Now();
    log::Info("world", "{}: {} scene objects, {} instances ({} drawn, {} external), {} meshes, {} textures, {} walkable tris, "
              "spawn ({:.1f}, {:.1f}, {:.1f}) in {:.0f} ms",
              log::Narrow(package_path), scenes.size(), out.instances_total, out.instances_drawn,
              out.instances_external, out.geometry_uploaded, out.textures_uploaded, out.terrain.TriangleCount(), out.spawn.x, out.spawn.y,
              out.spawn.z, out.load_seconds * 1000);
    return true;
}

}  // namespace sspc
