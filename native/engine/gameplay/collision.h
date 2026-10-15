// SecondSonPC native runtime - static collision geometry with a bounding volume hierarchy.
//
// Triangles come from the game's Havok collision (resources/havok.h) when the chunk has it, or
// from walkable render triangles as a documented fallback. Queries: closest ray hit and
// "triangles overlapping a box" (capsule contacts), both through a binary AABB tree, so the
// character never tests all triangles of a chunk.
#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace sspc {

inline constexpr u8 kCollisionHavok = 1;     // the game's own collision shapes
inline constexpr u8 kCollisionFallback = 2;  // render geometry stand-in (approximation)
inline constexpr u8 kCollisionAll = 0xff;

struct RayHit {
    float t = 0;
    Vec3 point, normal;  // normal faces the ray origin
    u32 triangle = 0;
};

class CollisionMesh {
public:
    // `vertices`: 3 per triangle (metres). `layers`: one per triangle (or empty = kCollisionHavok).
    void Build(std::vector<Vec3> vertices, std::vector<u8> layers = {});
    bool Empty() const { return verts_.empty(); }
    size_t TriangleCount() const { return verts_.size() / 3; }
    size_t NodeCount() const { return nodes_.size(); }
    u32 Depth() const { return depth_; }
    Vec3 BoundsMin() const { return nodes_.empty() ? Vec3{} : nodes_[0].mn; }
    Vec3 BoundsMax() const { return nodes_.empty() ? Vec3{} : nodes_[0].mx; }

    std::optional<RayHit> Raycast(const Vec3& origin, const Vec3& dir, float max_t, u8 mask = kCollisionAll) const;
    // Calls f(triangle) for every triangle whose bounds overlap [mn, mx].
    template <class F>
    void ForEachInBox(const Vec3& mn, const Vec3& mx, F&& f, u8 mask = kCollisionAll) const;

    const Vec3* Triangle(u32 t) const { return &verts_[static_cast<size_t>(t) * 3]; }
    Vec3 Normal(u32 t) const;
    u8 Layer(u32 t) const { return layers_[t]; }

private:
    struct Node {
        Vec3 mn, mx;
        u32 first = 0;  // leaf: first entry in order_; inner: right child (left = this + 1)
        u32 count = 0;  // > 0: leaf
    };
    u32 BuildNode(u32 begin, u32 end, u32 depth, std::vector<Vec3>& centroids);

    std::vector<Vec3> verts_;
    std::vector<u8> layers_;
    std::vector<u32> order_;  // triangle indices in leaf order
    std::vector<Node> nodes_;
    u32 depth_ = 0;
};

template <class F>
void CollisionMesh::ForEachInBox(const Vec3& mn, const Vec3& mx, F&& f, u8 mask) const {
    if (nodes_.empty()) return;
    u32 stack[64];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const u32 ni = stack[--sp];
        const Node& n = nodes_[ni];
        if (n.mx.x < mn.x || n.mn.x > mx.x || n.mx.y < mn.y || n.mn.y > mx.y || n.mx.z < mn.z || n.mn.z > mx.z) continue;
        if (n.count) {
            for (u32 i = 0; i < n.count; ++i) {
                const u32 t = order_[n.first + i];
                if (!(layers_[t] & mask)) continue;
                const Vec3* v = Triangle(t);
                const float tx0 = std::min({v[0].x, v[1].x, v[2].x}), tx1 = std::max({v[0].x, v[1].x, v[2].x});
                const float ty0 = std::min({v[0].y, v[1].y, v[2].y}), ty1 = std::max({v[0].y, v[1].y, v[2].y});
                const float tz0 = std::min({v[0].z, v[1].z, v[2].z}), tz1 = std::max({v[0].z, v[1].z, v[2].z});
                if (tx1 < mn.x || tx0 > mx.x || ty1 < mn.y || ty0 > mx.y || tz1 < mn.z || tz0 > mx.z) continue;
                f(t);
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.first;
            stack[sp++] = ni + 1;
        }
    }
}

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 ClosestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);
// Distance between segment p0p1 and triangle abc with the closest points (0 if they intersect).
float SegmentTriangleDistance(const Vec3& p0, const Vec3& p1, const Vec3& a, const Vec3& b, const Vec3& c,
                              Vec3& on_segment, Vec3& on_triangle);

}  // namespace sspc
