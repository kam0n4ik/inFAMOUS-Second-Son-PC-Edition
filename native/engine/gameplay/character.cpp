#include "gameplay/character.h"

#include <algorithm>
#include <cmath>

#include "gameplay/collision.h"
#include "gameplay/terrain.h"

namespace sspc {
namespace {

float WrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

constexpr float kNoGround = -1e30f;

// Highest support under the cylinder footprint at height <= feet + step.
float GroundHeight(const Vec3& feet, float radius, float step, std::span<const Aabb> obstacles,
                   const TerrainGrid* terrain, bool plane) {
    float g = plane ? 0.0f : kNoGround;
    if (terrain) {
        if (auto h = terrain->HeightAt(feet.x, feet.y, feet.z + step)) g = std::max(g, *h);
    }
    for (const Aabb& b : obstacles) {
        if (feet.x + radius <= b.min.x || feet.x - radius >= b.max.x || feet.y + radius <= b.min.y ||
            feet.y - radius >= b.max.y) {
            continue;
        }
        if (b.max.z <= feet.z + step && b.max.z > g) g = b.max.z;
    }
    return g;
}

void ResolveHorizontal(CharacterState& s, const CharacterParams& p, std::span<const Aabb> obstacles) {
    for (const Aabb& b : obstacles) {
        // Only boxes that overlap the body vertically (above the step height) block.
        if (s.position.z + p.step_height >= b.max.z || s.position.z + p.height <= b.min.z) continue;
        const float cx = std::clamp(s.position.x, b.min.x, b.max.x);
        const float cy = std::clamp(s.position.y, b.min.y, b.max.y);
        const float dx = s.position.x - cx, dy = s.position.y - cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= p.radius * p.radius) continue;
        if (d2 > 1e-12f) {
            const float d = std::sqrt(d2);
            const float push = p.radius - d;
            s.position.x += dx / d * push;
            s.position.y += dy / d * push;
            const float vn = (s.velocity.x * dx + s.velocity.y * dy) / d;
            if (vn < 0) {
                s.velocity.x -= dx / d * vn;
                s.velocity.y -= dy / d * vn;
            }
        } else {
            // Centre inside the box: push out along the shallowest axis.
            const float px[4] = {s.position.x - b.min.x, b.max.x - s.position.x, s.position.y - b.min.y,
                                 b.max.y - s.position.y};
            const int axis = static_cast<int>(std::min_element(px, px + 4) - px);
            if (axis == 0) s.position.x = b.min.x - p.radius;
            if (axis == 1) s.position.x = b.max.x + p.radius;
            if (axis == 2) s.position.y = b.min.y - p.radius;
            if (axis == 3) s.position.y = b.max.y + p.radius;
        }
    }
}

// Pushes the body capsule (from step_height + radius to height - radius above the feet) out of the
// collision triangles and removes the velocity that points into them.
void ResolveCapsule(CharacterState& s, const CharacterParams& p, const CollisionMesh& col, const Vec3& came_from) {
    const float r = p.radius;
    for (int iter = 0; iter < 4; ++iter) {
        bool any = false;
        const float z0 = p.step_height + r, z1 = std::max(z0, p.height - r);
        Vec3 a = s.position + Vec3{0, 0, z0}, b = s.position + Vec3{0, 0, z1};
        const Vec3 mn{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, a.z - r};
        const Vec3 mx{std::max(a.x, b.x) + r, std::max(a.y, b.y) + r, b.z + r};
        col.ForEachInBox(mn, mx, [&](u32 t) {
            const Vec3* v = col.Triangle(t);
            Vec3 on_seg, on_tri;
            const float d = SegmentTriangleDistance(a, b, v[0], v[1], v[2], on_seg, on_tri);
            if (d >= r) return;
            Vec3 n;
            if (d > 1e-5f) {
                n = (on_seg - on_tri) * (1.0f / d);
            } else {  // segment through the triangle: push back to the side it came from
                n = col.Normal(t);
                if (Dot(n, came_from + Vec3{0, 0, z0} - on_tri) < 0) n = -n;
            }
            Vec3 push = n * (r - d + 1e-4f);
            if (s.grounded) push.z = 0;  // the feet are held by the ground probe
            s.position += push;
            a += push;
            b += push;
            const float vn = Dot(s.velocity, n);
            if (vn < 0) s.velocity -= n * vn;  // slide along the surface
            any = true;
        });
        if (!any) break;
    }
}

void StepCapsule(CharacterState& s, const CharacterParams& p, float dt, const CollisionMesh& col) {
    const Vec3 move = s.velocity * dt;
    const float len = Length(move);
    const int n = std::clamp(static_cast<int>(std::ceil(len / (0.5f * p.radius))), 1, 16);
    const Vec3 step = move * (1.0f / static_cast<float>(n));
    for (int i = 0; i < n; ++i) {
        const Vec3 from = s.position;
        s.position += step;
        ResolveCapsule(s, p, col, from);
    }
    // Ground: highest walkable surface under the footprint, up to step_height above the feet;
    // while grounded the probe also reaches one step down so the feet follow slopes and stairs.
    const float top = s.position.z + p.step_height;
    const float reach = p.step_height + (s.grounded ? p.step_height : std::max(0.05f, -s.velocity.z * dt));
    const auto ground = FindGround(col, s.position, p.radius * 0.7f, top, reach, p.min_ground_up);
    if (s.grounded) {
        if (ground && s.velocity.z <= 0) {
            s.position.z = *ground;
            s.velocity.z = 0;
        } else {
            s.grounded = false;  // walked off a ledge
        }
    } else if (ground && s.velocity.z <= 0 && s.position.z <= *ground + 1e-3f) {
        s.position.z = *ground;
        s.velocity.z = 0;
        s.grounded = true;
        s.landed_this_step = true;
        s.air_time = 0;
    }
}

}  // namespace

std::optional<float> FindGround(const CollisionMesh& col, const Vec3& feet, float radius, float top, float max_drop,
                                float min_up) {
    std::optional<float> best;
    const Vec3 offsets[5] = {{0, 0, 0}, {radius, 0, 0}, {-radius, 0, 0}, {0, radius, 0}, {0, -radius, 0}};
    for (const Vec3& o : offsets) {
        const Vec3 origin{feet.x + o.x, feet.y + o.y, top};
        const auto hit = col.Raycast(origin, {0, 0, -1}, max_drop);
        if (!hit || hit->normal.z < min_up) continue;
        if (!best || hit->point.z > *best) best = hit->point.z;
    }
    return best;
}

void StepCharacter(CharacterState& s, const CharacterParams& p, const MoveInput& in, float dt,
                   std::span<const Aabb> obstacles, const TerrainGrid* terrain, const CollisionMesh* collision) {
    s.landed_this_step = false;
    // Desired horizontal velocity, camera relative.
    const float cy = std::cos(in.camera_yaw), sy = std::sin(in.camera_yaw);
    const Vec3 forward{cy, sy, 0}, right{sy, -cy, 0};
    Vec3 wish = forward * in.y + right * in.x;
    float mag = Length(wish);
    if (mag > 1.0f) {
        wish = wish * (1.0f / mag);
        mag = 1.0f;
    }
    const float speed = (in.sprint ? p.run_speed : p.walk_speed) * mag;
    const Vec3 target = mag > 1e-4f ? Normalize(wish) * speed : Vec3{0, 0, 0};
    const float accel = (s.grounded ? p.ground_accel : p.air_accel) * dt;
    Vec3 dv{target.x - s.velocity.x, target.y - s.velocity.y, 0};
    const float dvl = Length(dv);
    if (dvl > accel) dv = dv * (accel / dvl);
    s.velocity.x += dv.x;
    s.velocity.y += dv.y;

    if (mag > 1e-4f) {
        const float want = std::atan2(wish.y, wish.x);
        const float diff = WrapAngle(want - s.yaw);
        s.yaw = WrapAngle(s.yaw + diff * std::min(1.0f, p.turn_rate * dt));
    }

    if (s.grounded && in.jump) {
        s.velocity.z = p.jump_speed;
        s.grounded = false;
    }
    if (!s.grounded) {
        s.velocity.z = std::max(s.velocity.z - p.gravity * dt, -p.max_fall_speed);
        s.air_time += dt;
    }
    if (collision && !collision->Empty()) {
        StepCapsule(s, p, dt, *collision);
        return;
    }

    s.position.x += s.velocity.x * dt;
    s.position.y += s.velocity.y * dt;
    ResolveHorizontal(s, p, obstacles);
    s.position.z += s.velocity.z * dt;

    const float ground = GroundHeight(s.position, p.radius * 0.7f, p.step_height, obstacles, terrain, p.ground_plane);
    if (s.position.z <= ground) {
        if (!s.grounded) s.landed_this_step = true;
        s.position.z = ground;
        s.velocity.z = 0;
        s.grounded = true;
        s.air_time = 0;
    } else if (s.grounded && s.position.z - ground > p.step_height) {
        s.grounded = false;  // walked off a ledge
    } else if (s.grounded) {
        s.position.z = ground;  // follow small steps down/up
    }
}

}  // namespace sspc
