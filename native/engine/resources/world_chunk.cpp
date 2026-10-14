#include "resources/world_chunk.h"

#include <cmath>

#include "resources/geometry.h"

namespace sspc::res {
namespace {

constexpr u32 kMaxInstances = 1u << 20;

struct Array {
    u32 offset = 0;
    u32 count = 0;
};

std::optional<Array> ReadArray(const KcapPackage& pkg, u32 field, u32 elem_size) {
    const auto p = pkg.PointerAt(field);
    u64 count = 0;
    if (!p || !pkg.Read(field + 8, count) || count == 0 || count > kMaxInstances) return std::nullopt;
    if (!pkg.InRange(*p, count * elem_size)) return std::nullopt;
    return Array{*p, static_cast<u32>(count)};
}

}  // namespace

std::vector<ChunkScene> FindChunkScenes(const KcapPackage& pkg) {
    std::vector<ChunkScene> out;
    for (const KcapLink& l : pkg.Links()) {
        if ((l.kind != LinkKind::Pointer && l.kind != LinkKind::Pointer64) || l.location < 0xa8) continue;
        const u32 obj = l.location - 0xa8;
        const auto inst = ReadArray(pkg, obj + 0xa8, 8);
        const auto mats = ReadArray(pkg, obj + 0xb8, 48);
        const auto geos = ReadArray(pkg, obj + 0xc8, 8);
        if (!inst || !mats || !geos) continue;
        ChunkScene scene;
        scene.object = obj;
        bool ok = true;
        scene.instances.resize(inst->count);
        for (u32 i = 0; i < inst->count && ok; ++i) {
            u16 f[4];
            pkg.Read(inst->offset + static_cast<u64>(i) * 8, f);
            ChunkInstance& ci = scene.instances[i];
            ci.kind = f[0];
            ci.matrix = f[1];
            ci.geometry = static_cast<u16>(f[2] >> 2);
            ci.mode = static_cast<u8>(f[2] & 3);
            ci.extra = f[3];
            ok = ci.matrix < mats->count && ci.geometry < geos->count;
        }
        if (!ok) continue;
        scene.matrices.resize(mats->count);
        for (u32 m = 0; m < mats->count && ok; ++m) {
            pkg.Read(mats->offset + static_cast<u64>(m) * 48, scene.matrices[m]);
            for (float v : scene.matrices[m]) ok = ok && std::isfinite(v);
        }
        if (!ok) continue;
        // Every non-null slot must be a valid geometry record (rejects look-alike {ptr, count}
        // triples that happen to fit, e.g. the same object read 16 bytes off).
        scene.geometry.resize(geos->count);
        u32 resolved = 0;
        for (u32 g = 0; g < geos->count && ok; ++g) {
            scene.geometry[g] = pkg.PointerAt(geos->offset + g * 8);
            if (scene.geometry[g]) {
                ok = ReadGeometry(pkg, *scene.geometry[g]).has_value();
                ++resolved;
            }
        }
        if (!ok || resolved == 0) continue;
        out.push_back(std::move(scene));
    }
    return out;
}

Mat4 ChunkMatrixToWorld(const std::array<float, 12>& m) {
    Mat4 r = Mat4::Identity();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) r.m[col][row] = m[row * 4 + col];
        r.m[3][row] = m[row * 4 + 3] * 0.01f;  // centimetres -> metres
    }
    return r;
}

}  // namespace sspc::res
