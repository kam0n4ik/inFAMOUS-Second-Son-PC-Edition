#include "gameplay/character.h"

#include <algorithm>
#include <cmath>

namespace sspc {
namespace {

float WrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Highest support under the cylinder footprint at height <= feet + step.
float GroundHeight(const Vec3& feet, float radius, float step, std::span<const Aabb> obstacles) {
    float g = 0.0f;
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

}  // namespace

void StepCharacter(CharacterState& s, const CharacterParams& p, const MoveInput& in, float dt,
                   std::span<const Aabb> obstacles) {
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

    s.position.x += s.velocity.x * dt;
    s.position.y += s.velocity.y * dt;
    ResolveHorizontal(s, p, obstacles);
    s.position.z += s.velocity.z * dt;

    const float ground = GroundHeight(s.position, p.radius * 0.7f, p.step_height, obstacles);
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
