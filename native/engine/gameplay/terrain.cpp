#include "gameplay/terrain.h"

#include <algorithm>
#include <cmath>

namespace sspc {

void TerrainGrid::AddMesh(const std::vector<Vec3>& positions, const std::vector<u32>& indices, const Mat4& world,
                          float min_up) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const Vec3 a = world.TransformPoint(positions[indices[i]]);
        const Vec3 b = world.TransformPoint(positions[indices[i + 1]]);
        const Vec3 c = world.TransformPoint(positions[indices[i + 2]]);
        const Vec3 n = Cross(b - a, c - a);
        const float len = Length(n);
        if (len < 1e-8f || std::abs(n.z) / len < min_up) continue;  // either winding counts as walkable
        for (const Vec3& p : {a, b, c}) {
            tris_.push_back(p);
            bmin_ = {std::min(bmin_.x, p.x), std::min(bmin_.y, p.y), std::min(bmin_.z, p.z)};
            bmax_ = {std::max(bmax_.x, p.x), std::max(bmax_.y, p.y), std::max(bmax_.z, p.z)};
        }
    }
}

void TerrainGrid::Build(float cell_size) {
    cell_ = cell_size;
    cells_.clear();
    for (u32 t = 0; t < tris_.size() / 3; ++t) {
        const Vec3* v = &tris_[t * 3];
        const float x0 = std::min({v[0].x, v[1].x, v[2].x}), x1 = std::max({v[0].x, v[1].x, v[2].x});
        const float y0 = std::min({v[0].y, v[1].y, v[2].y}), y1 = std::max({v[0].y, v[1].y, v[2].y});
        const s32 cx0 = static_cast<s32>(std::floor(x0 / cell_)), cx1 = static_cast<s32>(std::floor(x1 / cell_));
        const s32 cy0 = static_cast<s32>(std::floor(y0 / cell_)), cy1 = static_cast<s32>(std::floor(y1 / cell_));
        if (static_cast<s64>(cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 4096) continue;  // degenerate giant triangle
        for (s32 cx = cx0; cx <= cx1; ++cx)
            for (s32 cy = cy0; cy <= cy1; ++cy) cells_[Key(cx, cy)].push_back(t);
    }
}

template <class F>
void TerrainGrid::ForEachSurface(float x, float y, F&& f) const {
    const auto it = cells_.find(Key(static_cast<s32>(std::floor(x / cell_)), static_cast<s32>(std::floor(y / cell_))));
    if (it == cells_.end()) return;
    for (u32 t : it->second) {
        const Vec3& a = tris_[t * 3];
        const Vec3& b = tris_[t * 3 + 1];
        const Vec3& c = tris_[t * 3 + 2];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(den) < 1e-12f) continue;
        const float w0 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / den;
        const float w1 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / den;
        const float w2 = 1.0f - w0 - w1;
        constexpr float eps = -1e-5f;
        if (w0 < eps || w1 < eps || w2 < eps) continue;
        f(w0 * a.z + w1 * b.z + w2 * c.z);
    }
}

std::optional<float> TerrainGrid::HeightAt(float x, float y, float z_max) const {
    std::optional<float> best;
    ForEachSurface(x, y, [&](float z) {
        if (z <= z_max && (!best || z > *best)) best = z;
    });
    return best;
}

std::optional<float> TerrainGrid::LowestAt(float x, float y) const {
    std::optional<float> best;
    ForEachSurface(x, y, [&](float z) {
        if (!best || z < *best) best = z;
    });
    return best;
}

}  // namespace sspc
