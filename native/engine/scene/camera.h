// SecondSonPC native runtime - third-person orbit camera and free-fly camera.
#pragma once

#include <algorithm>
#include <cmath>

#include "core/math.h"

namespace sspc {

struct OrbitCamera {
    Vec3 target{0, 0, 1.4f};
    float yaw = kPi;        // camera position angle around the target (radians, 0 = +X)
    float pitch = 0.25f;    // radians above the horizontal
    float distance = 4.0f;
    float sensitivity = 0.0025f;  // radians per mouse count

    void AddLook(float dx, float dy) {
        yaw -= dx * sensitivity;
        pitch = std::clamp(pitch + dy * sensitivity, -0.6f, 1.35f);
    }
    Vec3 Eye() const {
        const float cp = std::cos(pitch);
        return target + Vec3{std::cos(yaw) * cp * distance, std::sin(yaw) * cp * distance, std::sin(pitch) * distance};
    }
    // Direction the camera looks along the ground, for camera-relative movement.
    float ForwardYaw() const { return yaw + kPi; }
    Mat4 View() const { return Mat4::LookAt(Eye(), target, {0, 0, 1}); }
};

struct FreeCamera {
    Vec3 position{-4, 0, 2};
    float yaw = 0, pitch = -0.2f;
    float speed = 6.0f;
    float sensitivity = 0.0025f;

    void AddLook(float dx, float dy) {
        yaw -= dx * sensitivity;
        pitch = std::clamp(pitch - dy * sensitivity, -1.5f, 1.5f);
    }
    Vec3 Forward() const {
        return {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
    }
    void Move(float right, float forward, float up, float dt, bool fast) {
        const Vec3 f = Forward();
        const Vec3 r = Normalize(Cross(f, {0, 0, 1}));
        position += (f * forward + r * right + Vec3{0, 0, 1} * up) * (speed * (fast ? 4.0f : 1.0f) * dt);
    }
    Mat4 View() const { return Mat4::LookAt(position, position + Forward(), {0, 0, 1}); }
};

}  // namespace sspc
