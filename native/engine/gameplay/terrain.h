// SecondSonPC native runtime - walkable-surface grid for ground queries.
//
// Built from the original environment's render triangles (faces whose normal points up enough).
// This is NOT the game's collision data (Havok shapes are not decoded yet); it gives the
// character something real to stand and walk on.
#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace sspc {

class TerrainGrid {
public:
    // Adds triangles transformed by `world`; faces with normal.z < min_up are not walkable.
    void AddMesh(const std::vector<Vec3>& positions, const std::vector<u32>& indices, const Mat4& world,
                 float min_up = 0.6f);
    void Build(float cell_size = 2.0f);

    // Highest walkable surface under (x, y) at or below z_max.
    std::optional<float> HeightAt(float x, float y, float z_max) const;
    // Lowest walkable surface under (x, y).
    std::optional<float> LowestAt(float x, float y) const;

    size_t TriangleCount() const { return tris_.size() / 3; }
    bool Empty() const { return tris_.empty(); }
    Vec3 BoundsMin() const { return bmin_; }
    Vec3 BoundsMax() const { return bmax_; }

private:
    static u64 Key(s32 cx, s32 cy) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cy); }
    template <class F>
    void ForEachSurface(float x, float y, F&& f) const;

    std::vector<Vec3> tris_;
    std::unordered_map<u64, std::vector<u32>> cells_;
    float cell_ = 2.0f;
    Vec3 bmin_{1e30f, 1e30f, 1e30f}, bmax_{-1e30f, -1e30f, -1e30f};
};

}  // namespace sspc
