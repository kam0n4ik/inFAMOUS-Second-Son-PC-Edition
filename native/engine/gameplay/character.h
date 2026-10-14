// SecondSonPC native runtime - kinematic character movement.
//
// STATUS: technical placeholder for the vertical slice. Walk/run/jump/landing and collisions with
// boxes work and are deterministic, but the parameters are NOT the game's: Second Son's movement
// (parkour, dash, skill state machine at CHARACTER/SKILL objects) is not reconstructed yet.
#pragma once

#include <span>

#include "core/math.h"

namespace sspc {

struct CharacterParams {
    float walk_speed = 2.4f;    // m/s
    float run_speed = 6.0f;     // m/s
    float ground_accel = 20.0f; // m/s^2
    float air_accel = 5.0f;
    float gravity = 22.0f;      // m/s^2 (downwards)
    float jump_speed = 7.2f;    // m/s
    float radius = 0.35f;       // collision cylinder
    float height = 1.8f;
    float step_height = 0.35f;  // ledge the character walks up without jumping
    float turn_rate = 10.0f;    // 1/s, yaw smoothing towards the move direction
    float max_fall_speed = 40.0f;
};

struct MoveInput {
    float x = 0, y = 0;     // stick/keys: x right, y forward, |v| <= 1
    float camera_yaw = 0;   // radians, movement is relative to the camera
    bool jump = false;      // pressed this step
    bool sprint = false;
};

struct Aabb {
    Vec3 min, max;
};

struct CharacterState {
    Vec3 position{0, 0, 0};  // feet
    Vec3 velocity{0, 0, 0};
    float yaw = 0;           // facing, radians (0 = +X)
    bool grounded = true;
    bool landed_this_step = false;
    float air_time = 0;
};

// Advances one fixed step. Ground = plane z = 0 plus the tops of `obstacles`; obstacles also block
// horizontal movement (cylinder vs box, resolved by axis push-out).
void StepCharacter(CharacterState& s, const CharacterParams& p, const MoveInput& in, float dt,
                   std::span<const Aabb> obstacles);

}  // namespace sspc
