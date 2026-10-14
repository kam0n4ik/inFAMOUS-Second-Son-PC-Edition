#include "scene/primitives.h"

namespace sspc {
namespace {

void AddQuad(res::MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& n) {
    const u32 base = static_cast<u32>(m.positions.size());
    for (const Vec3& p : {a, b, c, d}) {
        m.positions.push_back(p);
        m.normals.push_back(n);
    }
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

}  // namespace

void MakeGroundGrid(int tiles, float tile_size, res::MeshData& even, res::MeshData& odd) {
    even = res::MeshData{};
    odd = res::MeshData{};
    const float half = tiles * tile_size * 0.5f;
    for (int y = 0; y < tiles; ++y) {
        for (int x = 0; x < tiles; ++x) {
            const float x0 = -half + x * tile_size, y0 = -half + y * tile_size;
            res::MeshData& m = ((x + y) & 1) ? odd : even;
            AddQuad(m, {x0, y0, 0}, {x0 + tile_size, y0, 0}, {x0 + tile_size, y0 + tile_size, 0},
                    {x0, y0 + tile_size, 0}, {0, 0, 1});
        }
    }
    even.bounds_min = odd.bounds_min = {-half, -half, 0};
    even.bounds_max = odd.bounds_max = {half, half, 0};
}

res::MeshData MakeBox(const Vec3& lo, const Vec3& hi) {
    res::MeshData m;
    AddQuad(m, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, {0, 0, 1});
    AddQuad(m, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {0, 0, -1});
    AddQuad(m, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, {0, -1, 0});
    AddQuad(m, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {0, 1, 0});
    AddQuad(m, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, {1, 0, 0});
    AddQuad(m, {lo.x, hi.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {-1, 0, 0});
    m.bounds_min = lo;
    m.bounds_max = hi;
    return m;
}

}  // namespace sspc
