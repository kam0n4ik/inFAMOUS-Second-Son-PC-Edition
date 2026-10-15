// Collision: BVH queries, capsule character controller, the game's Havok collision of chunk a1.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/file.h"
#include "gameplay/character.h"
#include "gameplay/collision.h"
#include "gameplay/terrain.h"
#include "resources/havok.h"
#include "resources/kcap.h"
#include "resources/world_chunk.h"
#include "test.h"

using namespace sspc;

namespace {

void AddQuad(std::vector<Vec3>& t, Vec3 a, Vec3 b, Vec3 c, Vec3 d) { t.insert(t.end(), {a, b, c, a, c, d}); }

void AddBox(std::vector<Vec3>& t, Vec3 mn, Vec3 mx) {
    const Vec3 p[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                       {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    AddQuad(t, p[4], p[5], p[6], p[7]);  // top
    AddQuad(t, p[0], p[3], p[2], p[1]);  // bottom
    AddQuad(t, p[0], p[1], p[5], p[4]);
    AddQuad(t, p[1], p[2], p[6], p[5]);
    AddQuad(t, p[2], p[3], p[7], p[6]);
    AddQuad(t, p[3], p[0], p[4], p[7]);
}

CollisionMesh Floor(std::vector<Vec3> extra = {}) {
    std::vector<Vec3> t = std::move(extra);
    AddQuad(t, {-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0});
    CollisionMesh m;
    m.Build(std::move(t));
    return m;
}

void Run(CharacterState& s, const CollisionMesh& m, MoveInput in, int steps, const CharacterParams& p = {}) {
    for (int i = 0; i < steps; ++i) {
        StepCharacter(s, p, in, 1.0f / 60.0f, {}, nullptr, &m);
        in.jump = false;
    }
}

std::wstring GameDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"SSPC_GAME_DIR", buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"E:\\shdps4\\CUSA00004";
}

}  // namespace

TEST(collision_bvh_matches_brute_force) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-20, 20), small(-1.5f, 1.5f);
    std::vector<Vec3> tris;
    for (int i = 0; i < 3000; ++i) {
        const Vec3 c{u(rng), u(rng), u(rng)};
        tris.insert(tris.end(), {c + Vec3{small(rng), small(rng), small(rng)}, c + Vec3{small(rng), small(rng), small(rng)},
                                 c + Vec3{small(rng), small(rng), small(rng)}});
    }
    CollisionMesh m;
    m.Build(tris);
    CHECK_EQ(m.TriangleCount(), 3000u);
    CHECK(m.Depth() < 30);
    int mismatches = 0;
    for (int i = 0; i < 500; ++i) {
        const Vec3 o{u(rng), u(rng), u(rng)};
        const Vec3 d = Normalize(Vec3{small(rng), small(rng), small(rng)});
        const auto hit = m.Raycast(o, d, 100.0f);
        // Brute force over the same triangles.
        float best = 1e30f;
        for (size_t t = 0; t < tris.size(); t += 3) {
            const Vec3 e1 = tris[t + 1] - tris[t], e2 = tris[t + 2] - tris[t];
            const Vec3 pv = Cross(d, e2);
            const float det = Dot(e1, pv);
            if (std::abs(det) < 1e-12f) continue;
            const Vec3 tv = o - tris[t];
            const float uu = Dot(tv, pv) / det;
            const Vec3 qv = Cross(tv, e1);
            const float vv = Dot(d, qv) / det;
            const float th = Dot(e2, qv) / det;
            if (uu >= 0 && vv >= 0 && uu + vv <= 1 && th >= 0 && th <= 100 && th < best) best = th;
        }
        const bool brute_hit = best < 1e29f;
        if (brute_hit != hit.has_value() || (hit && std::abs(hit->t - best) > 1e-4f)) ++mismatches;
    }
    CHECK_EQ(mismatches, 0);
    // Box query returns exactly the triangles whose bounds overlap.
    u32 found = 0;
    m.ForEachInBox({-2, -2, -2}, {2, 2, 2}, [&](u32) { ++found; });
    u32 expect = 0;
    for (size_t t = 0; t < tris.size(); t += 3) {
        bool overlap = true;
        for (int a = 0; a < 3; ++a) {
            auto comp = [&](const Vec3& v) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); };
            const float lo = std::min({comp(tris[t]), comp(tris[t + 1]), comp(tris[t + 2])});
            const float hi = std::max({comp(tris[t]), comp(tris[t + 1]), comp(tris[t + 2])});
            overlap = overlap && hi >= -2 && lo <= 2;
        }
        expect += overlap;
    }
    CHECK_EQ(found, expect);
}

TEST(collision_segment_triangle_distance) {
    Vec3 s, t;
    const Vec3 a{0, 0, 0}, b{2, 0, 0}, c{0, 2, 0};
    CHECK(std::abs(SegmentTriangleDistance({0.5f, 0.5f, 1}, {0.5f, 0.5f, 3}, a, b, c, s, t) - 1.0f) < 1e-5f);
    CHECK(SegmentTriangleDistance({0.5f, 0.5f, -1}, {0.5f, 0.5f, 1}, a, b, c, s, t) == 0.0f);  // crosses
    CHECK(std::abs(SegmentTriangleDistance({3, 0, -1}, {3, 0, 1}, a, b, c, s, t) - 1.0f) < 1e-5f);  // edge-side
    CHECK(std::abs(t.x - 2.0f) < 1e-5f && std::abs(t.z) < 1e-5f);
}

TEST(capsule_stands_walks_into_walls_and_slides) {
    CharacterParams p;
    MoveInput none, fwd;
    fwd.y = 1;  // +X
    fwd.sprint = true;
    {
        const CollisionMesh m = Floor();
        CharacterState s;
        s.position = {0, 0, 0.5f};
        s.grounded = false;
        Run(s, m, none, 120, p);
        CHECK(s.grounded && std::abs(s.position.z) < 1e-4f);
    }
    {
        std::vector<Vec3> extra;
        AddBox(extra, {3, -5, 0}, {4, 5, 3});  // wall
        const CollisionMesh m = Floor(extra);
        CharacterState s;
        Run(s, m, fwd, 240, p);
        CHECK(s.position.x <= 3.0f - p.radius + 1e-3f && s.position.x > 2.5f);
        CHECK(s.grounded && std::abs(s.position.z) < 1e-4f);
        // Diagonal into the wall: slides along +Y instead of stopping.
        MoveInput diag;
        diag.y = 0.7071f;
        diag.x = -0.7071f;  // right of +X camera = -Y ... use left: x < 0 moves +Y
        diag.sprint = true;
        const float y0 = s.position.y;
        Run(s, m, diag, 60, p);
        CHECK(std::abs(s.position.y - y0) > 1.0f && s.position.x <= 3.0f - p.radius + 1e-3f);
    }
}

TEST(capsule_steps_slopes_jumps_and_does_not_tunnel) {
    CharacterParams p;
    MoveInput fwd;
    fwd.y = 1;
    {
        std::vector<Vec3> extra;
        AddBox(extra, {2, -3, 0}, {6, 3, 0.3f});   // step below step_height
        AddBox(extra, {8, -3, 0}, {12, 3, 1.0f});  // ledge: needs a jump
        const CollisionMesh m = Floor(extra);
        CharacterState s;
        Run(s, m, fwd, 90, p);  // 2.4 m/s * 1.5 s = 3.6 m: on the step
        CHECK(s.grounded && std::abs(s.position.z - 0.3f) < 1e-3f);
        Run(s, m, fwd, 120, p);  // blocked by the 1 m ledge
        CHECK(s.position.x <= 8.0f - p.radius + 1e-3f && s.position.z < 0.31f);
        MoveInput jump = fwd;
        jump.jump = true;
        bool landed = false;
        for (int i = 0; i < 120 && !(landed && s.position.z > 0.9f); ++i) {
            StepCharacter(s, p, jump, 1.0f / 60.0f, {}, nullptr, &m);
            jump.jump = false;
            landed = landed || s.landed_this_step;
        }
        CHECK(landed && s.grounded && std::abs(s.position.z - 1.0f) < 1e-3f);
    }
    {
        // 30 degree ramp is walkable, 60 degree ramp is not.
        std::vector<Vec3> extra;
        const float h30 = std::tan(30.0f * kPi / 180.0f) * 4, h60 = std::tan(60.0f * kPi / 180.0f) * 4;
        AddQuad(extra, {2, -3, 0}, {6, -3, h30}, {6, -1, h30}, {2, -1, 0});
        AddQuad(extra, {2, 1, 0}, {6, 1, h60}, {6, 3, h60}, {2, 3, 0});
        const CollisionMesh m = Floor(extra);
        CharacterState a;
        a.position = {0, -2, 0};
        Run(a, m, fwd, 120, p);
        CHECK(a.grounded && a.position.x > 3.0f && a.position.z > 0.5f);
        CharacterState b;
        b.position = {0, 2, 0};
        Run(b, m, fwd, 120, p);
        CHECK(b.position.z < 0.5f);
    }
    {
        // Falling at the speed limit onto a thin floor (one quad) never passes through it.
        std::vector<Vec3> t;
        AddQuad(t, {-5, -5, 0}, {5, -5, 0}, {5, 5, 0}, {-5, 5, 0});
        CollisionMesh m;
        m.Build(t);
        CharacterState s;
        s.position = {0, 0, 30};
        s.grounded = false;
        s.velocity = {0, 0, -40};
        Run(s, m, MoveInput{}, 240, p);
        CHECK(s.grounded && std::abs(s.position.z) < 1e-4f);
    }
}

TEST(real_a1_havok_collision) {
    const std::wstring path = GameDir() + L"\\art\\cache\\a1.xpps";
    if (!file::Exists(path)) {
        test::Skip("a1.xpps not found");
        return;
    }
    res::KcapFile f;
    CHECK_EQ(f.Open(path), res::KcapError::Ok);
    const auto data = f.Package().Data();
    const auto packs = res::FindHavokPackfiles(data);
    CHECK_EQ(packs.size(), static_cast<size_t>(1));
    if (packs.empty()) return;
    res::HavokPackfile pf;
    std::string err;
    CHECK(pf.Parse(data, packs[0], &err));
    CHECK_EQ(pf.Version(), std::string("hk_2013.2.0-r1"));
    std::vector<Vec3> tris;
    res::HavokCollisionStats st;
    const bool decoded = res::DecodeStaticCollision(pf, tris, st, &err);
    if (!decoded) std::printf("    decode error: %s\n", err.c_str());
    CHECK(decoded);
    std::printf("    a1: %u compound, %u instances, %u meshes, %u triangles\n", st.compound_shapes, st.instances,
                st.mesh_shapes, st.triangles);
    CHECK_EQ(st.triangles, 27361u);  // 3 deleted (0xDEAD) quads skipped
    CHECK_EQ(st.mesh_shapes, 19u);
    CollisionMesh m;
    m.Build(tris);
    const Vec3 cmin = m.BoundsMin(), cmax = m.BoundsMax();
    std::printf("    collision bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n", cmin.x, cmin.y, cmin.z, cmax.x, cmax.y, cmax.z);
    // Render instances of the chunk (metres): how many stand on / near the collision surface.
    const auto scenes = res::FindChunkScenes(f.Package());
    Vec3 rmin{1e9f, 1e9f, 1e9f}, rmax{-1e9f, -1e9f, -1e9f};
    u32 covered = 0, probed = 0;
    for (const auto& sc : scenes) {
        for (const auto& inst : sc.instances) {
            if (inst.kind != 0) continue;
            const Mat4 w = res::ChunkMatrixToWorld(sc.matrices[inst.matrix]);
            const Vec3 p{w.m[3][0], w.m[3][1], w.m[3][2]};
            rmin = {std::min(rmin.x, p.x), std::min(rmin.y, p.y), std::min(rmin.z, p.z)};
            rmax = {std::max(rmax.x, p.x), std::max(rmax.y, p.y), std::max(rmax.z, p.z)};
            if (probed < 400) {
                ++probed;
                const auto g = FindGround(m, p, 0.0f, p.z + 5.0f, 10.0f, -1.0f);
                if (g) ++covered;
            }
        }
    }
    std::printf("    render instances (%.1f %.1f %.1f)..(%.1f %.1f %.1f); %u/%u have collision within 5 m above/below\n",
                rmin.x, rmin.y, rmin.z, rmax.x, rmax.y, rmax.z, covered, probed);
    // The Havok shapes hold rocks, trunks and buildings (no ground surface): only some instance
    // origins (trees, rocks) have collision around them.
    CHECK(covered > 0);
}
