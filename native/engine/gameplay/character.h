// SecondSonPC native runtime - kinematic character movement.
//
// STATUS: technical placeholder for the vertical slice. Walk/run/jump/landing and collisions with
// boxes work and are deterministic, but the parameters are NOT the game's: Second Son's movement
// (parkour, dash, skill state machine at CHARACTER/SKILL objects) is not reconstructed yet.
#pragma once

#include <optional>
#include <span>

#include "core/math.h"

namespace sspc {

class TerrainGrid;
class CollisionMesh;

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
    float min_ground_up = 0.64f;  // cos of the steepest walkable slope (~50 degrees)
    bool ground_plane = true;   // infinite floor at z = 0 (test scene); off when a world is loaded
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

// Advances one fixed step. Ground = optional plane z = 0, the tops of `obstacles` and the walkable
// surfaces of `terrain`; obstacles also block horizontal movement (cylinder vs box, axis push-out).
// With `collision` the character is a capsule against that triangle mesh instead: the body capsule
// (radius, from step_height above the feet to the head) is pushed out of walls and ceilings and
// slides along them; the feet stand on the highest walkable surface found by downward rays within
// 0.7 radius (steps up to step_height, slopes up to min_ground_up). Movement is split into
// sub-steps of at most half a radius so fast motion cannot tunnel through thin walls.
void StepCharacter(CharacterState& s, const CharacterParams& p, const MoveInput& in, float dt,
                   std::span<const Aabb> obstacles, const TerrainGrid* terrain = nullptr,
                   const CollisionMesh* collision = nullptr);

// Highest walkable ground under the capsule footprint at or below `top` (nullopt = none within
// `max_drop` below `top`). Used for spawning and landing.
std::optional<float> FindGround(const CollisionMesh& col, const Vec3& feet, float radius, float top, float max_drop,
                                float min_up);

}  // namespace sspc
