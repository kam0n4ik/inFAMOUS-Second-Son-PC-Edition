#include "gameplay/collision.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sspc {
namespace {

constexpr u32 kLeafSize = 4;
constexpr u32 kMaxDepth = 60;

Vec3 Min3(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3 Max3(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
float Axis(const Vec3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Closest points of segments p1q1 and p2q2 (Ericson 5.1.9); returns the squared distance.
float ClosestSegmentSegment(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    float s = 0, t = 0;
    constexpr float eps = 1e-12f;
    if (a <= eps && e <= eps) {
        c1 = p1;
        c2 = p2;
        return Dot(c1 - c2, c1 - c2);
    }
    if (a <= eps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot(d1, r);
        if (e <= eps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot(d1, d2), denom = a * e - b * b;
            s = denom != 0 ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    return Dot(c1 - c2, c1 - c2);
}

}  // namespace

Vec3 ClosestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

float SegmentTriangleDistance(const Vec3& p0, const Vec3& p1, const Vec3& a, const Vec3& b, const Vec3& c,
                              Vec3& on_segment, Vec3& on_triangle) {
    // Segment crossing the triangle: distance 0 at the crossing point.
    const Vec3 n = Cross(b - a, c - a);
    const float d0 = Dot(n, p0 - a), d1 = Dot(n, p1 - a);
    if ((d0 <= 0 && d1 >= 0) || (d0 >= 0 && d1 <= 0)) {
        const float den = d0 - d1;
        if (std::abs(den) > 1e-20f) {
            const Vec3 x = p0 + (p1 - p0) * (d0 / den);
            const Vec3 e0 = Cross(b - a, x - a), e1 = Cross(c - b, x - b), e2 = Cross(a - c, x - c);
            if (Dot(e0, n) >= 0 && Dot(e1, n) >= 0 && Dot(e2, n) >= 0) {
                on_segment = on_triangle = x;
                return 0.0f;
            }
        }
    }
    float best = 1e30f;
    auto consider = [&](const Vec3& s, const Vec3& t) {
        const float d = Dot(s - t, s - t);
        if (d < best) {
            best = d;
            on_segment = s;
            on_triangle = t;
        }
    };
    consider(p0, ClosestPointOnTriangle(p0, a, b, c));
    consider(p1, ClosestPointOnTriangle(p1, a, b, c));
    const Vec3* ev[3][2] = {{&a, &b}, {&b, &c}, {&c, &a}};
    for (const auto& e : ev) {
        Vec3 s, t;
        ClosestSegmentSegment(p0, p1, *e[0], *e[1], s, t);
        consider(s, t);
    }
    return std::sqrt(best);
}

Vec3 CollisionMesh::Normal(u32 t) const {
    const Vec3* v = Triangle(t);
    return Normalize(Cross(v[1] - v[0], v[2] - v[0]));
}

void CollisionMesh::Build(std::vector<Vec3> vertices, std::vector<u8> layers) {
    verts_ = std::move(vertices);
    verts_.resize(verts_.size() / 3 * 3);
    const u32 n = static_cast<u32>(verts_.size() / 3);
    layers_ = layers.size() == n ? std::move(layers) : std::vector<u8>(n, kCollisionHavok);
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0u);
    nodes_.clear();
    depth_ = 0;
    if (n == 0) return;
    std::vector<Vec3> centroids(n);
    for (u32 t = 0; t < n; ++t) centroids[t] = (verts_[t * 3] + verts_[t * 3 + 1] + verts_[t * 3 + 2]) * (1.0f / 3.0f);
    nodes_.reserve(2 * n / kLeafSize + 2);
    BuildNode(0, n, 1, centroids);
}

u32 CollisionMesh::BuildNode(u32 begin, u32 end, u32 depth, std::vector<Vec3>& centroids) {
    const u32 index = static_cast<u32>(nodes_.size());
    nodes_.push_back({});
    depth_ = std::max(depth_, depth);
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f}, cmn = mn, cmx = mx;
    for (u32 i = begin; i < end; ++i) {
        const u32 t = order_[i];
        for (int k = 0; k < 3; ++k) {
            mn = Min3(mn, verts_[t * 3 + k]);
            mx = Max3(mx, verts_[t * 3 + k]);
        }
        cmn = Min3(cmn, centroids[t]);
        cmx = Max3(cmx, centroids[t]);
    }
    nodes_[index].mn = mn;
    nodes_[index].mx = mx;
    if (end - begin <= kLeafSize || depth >= kMaxDepth) {
        nodes_[index].first = begin;
        nodes_[index].count = end - begin;
        return index;
    }
    // Median split of the centroids along the longest axis of their bounds.
    const Vec3 ext = cmx - cmn;
    const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const u32 mid = (begin + end) / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                     [&](u32 a, u32 b) { return Axis(centroids[a], axis) < Axis(centroids[b], axis); });
    BuildNode(begin, mid, depth + 1, centroids);
    const u32 right = BuildNode(mid, end, depth + 1, centroids);
    nodes_[index].first = right;
    nodes_[index].count = 0;
    return index;
}

std::optional<RayHit> CollisionMesh::Raycast(const Vec3& origin, const Vec3& dir, float max_t, u8 mask) const {
    if (nodes_.empty()) return std::nullopt;
    const Vec3 inv{dir.x != 0 ? 1.0f / dir.x : 1e30f, dir.y != 0 ? 1.0f / dir.y : 1e30f, dir.z != 0 ? 1.0f / dir.z : 1e30f};
    std::optional<RayHit> best;
    float best_t = max_t;
    u32 stack[64];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& n = nodes_[stack[--sp]];
        const u32 ni = static_cast<u32>(&n - nodes_.data());
        // Slab test.
        float t0 = 0, t1 = best_t;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            const float o = Axis(origin, a), id = Axis(inv, a);
            float ta = (Axis(n.mn, a) - o) * id, tb = (Axis(n.mx, a) - o) * id;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            miss = t0 > t1;
        }
        if (miss) continue;
        if (n.count) {
            for (u32 i = 0; i < n.count; ++i) {
                const u32 t = order_[n.first + i];
                if (!(layers_[t] & mask)) continue;
                const Vec3* v = Triangle(t);
                // Moller-Trumbore, both sides.
                const Vec3 e1 = v[1] - v[0], e2 = v[2] - v[0];
                const Vec3 pv = Cross(dir, e2);
                const float det = Dot(e1, pv);
                if (std::abs(det) < 1e-12f) continue;
                const float idet = 1.0f / det;
                const Vec3 tv = origin - v[0];
                const float u = Dot(tv, pv) * idet;
                if (u < 0 || u > 1) continue;
                const Vec3 qv = Cross(tv, e1);
                const float w = Dot(dir, qv) * idet;
                if (w < 0 || u + w > 1) continue;
                const float th = Dot(e2, qv) * idet;
                if (th < 0 || th > best_t) continue;
                best_t = th;
                Vec3 nrm = Normalize(Cross(e1, e2));
                if (Dot(nrm, dir) > 0) nrm = -nrm;
                best = RayHit{th, origin + dir * th, nrm, t};
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.first;
            stack[sp++] = ni + 1;
        }
    }
    return best;
}

}  // namespace sspc
