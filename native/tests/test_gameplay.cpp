#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gameplay/character.h"
#include "gameplay/game_loop.h"
#include "test.h"

using namespace sspc;

namespace {

MoveInput ScriptedInput(u64 step) {
    MoveInput in;
    const double t = step / 60.0;
    in.y = t < 2.0 ? 1.0f : 0.3f;
    in.x = (t > 0.5 && t < 1.2) ? -0.7f : 0.0f;
    in.sprint = t > 1.0;
    in.jump = step == 90;
    in.camera_yaw = static_cast<float>(0.3 * t);
    return in;
}

// Runs `seconds` of game time with frames of `frame_dt`, applying input per simulation step.
CharacterState RunWithFrameTime(double frame_dt, double seconds, const std::vector<Aabb>& obstacles) {
    FixedStepClock clock(1.0 / 60.0, 8);
    CharacterParams p;
    CharacterState s;
    u64 step = 0;
    const int frames = static_cast<int>(std::lround(seconds / frame_dt));
    for (int f = 0; f < frames; ++f) {
        const u32 n = clock.Advance(frame_dt);
        for (u32 i = 0; i < n; ++i) StepCharacter(s, p, ScriptedInput(step++), 1.0f / 60.0f, obstacles);
    }
    return s;
}

}  // namespace

TEST(fixed_step_clock_is_frame_rate_independent) {
    for (double fps : {30.0, 60.0, 144.0, 240.0}) {
        FixedStepClock c(1.0 / 60.0);
        u64 steps = 0;
        for (int i = 0; i < static_cast<int>(fps) * 3; ++i) steps += c.Advance(1.0 / fps);
        CHECK_EQ(steps, 180ull);
    }
    FixedStepClock c(1.0 / 60.0, 8);
    CHECK_EQ(c.Advance(10.0), 8u);  // stall: clamped instead of spiralling
    CHECK(c.DroppedSeconds() > 9.0);
    CHECK_EQ(c.Advance(-1.0), 0u);
    CHECK_EQ(c.Advance(std::nan("")), 0u);
}

TEST(character_simulation_is_deterministic_across_frame_rates) {
    const std::vector<Aabb> obstacles = {{{3, -1, 0}, {4.5f, 1, 0.5f}}};
    const CharacterState a = RunWithFrameTime(1.0 / 30.0, 4.0, obstacles);
    const CharacterState b = RunWithFrameTime(1.0 / 144.0, 4.0, obstacles);
    const CharacterState c = RunWithFrameTime(1.0 / 60.0, 4.0, obstacles);
    CHECK(std::memcmp(&a.position, &b.position, sizeof(Vec3)) == 0);
    CHECK(std::memcmp(&a.position, &c.position, sizeof(Vec3)) == 0);
    CHECK(a.yaw == b.yaw && a.grounded == b.grounded);
    CHECK(Length(a.position) > 5.0f);  // it actually moved
}

TEST(character_jump_and_landing) {
    CharacterParams p;
    CharacterState s;
    MoveInput jump;
    jump.jump = true;
    StepCharacter(s, p, jump, 1.0f / 60.0f, {});
    CHECK(!s.grounded && s.velocity.z > 0);
    float max_z = 0;
    int steps = 0;
    bool landed = false;
    for (; steps < 600 && !landed; ++steps) {
        StepCharacter(s, p, MoveInput{}, 1.0f / 60.0f, {});
        max_z = std::max(max_z, s.position.z);
        landed = s.landed_this_step;
    }
    CHECK(landed && s.grounded && s.position.z == 0.0f);
    const float expected_apex = p.jump_speed * p.jump_speed / (2 * p.gravity);
    CHECK(std::abs(max_z - expected_apex) < 0.1f);
    const float expected_time = 2 * p.jump_speed / p.gravity;
    CHECK(std::abs(steps / 60.0f - expected_time) < 0.1f);
}

TEST(character_collides_with_walls_and_steps_onto_low_boxes) {
    CharacterParams p;
    MoveInput fwd;
    fwd.y = 1.0f;  // camera yaw 0 -> forward = +X
    {
        const std::vector<Aabb> wall = {{{3, -2, 0}, {4, 2, 2}}};
        CharacterState s;
        for (int i = 0; i < 300; ++i) StepCharacter(s, p, fwd, 1.0f / 60.0f, wall);
        CHECK(s.position.x <= 3.0f - p.radius + 1e-3f);
        CHECK(s.position.x > 2.0f);
    }
    {
        const std::vector<Aabb> step = {{{2, -2, 0}, {6, 2, 0.3f}}};
        CharacterState s;
        for (int i = 0; i < 120; ++i) StepCharacter(s, p, fwd, 1.0f / 60.0f, step);
        CHECK(s.position.x > 2.5f && std::abs(s.position.z - 0.3f) < 1e-4f && s.grounded);
    }
    {
        // Jump onto a 1 m box from a run-up and stay on top.
        const std::vector<Aabb> box = {{{2, -2, 0}, {6, 2, 1.0f}}};
        CharacterState s;
        MoveInput in = fwd;
        for (int i = 0; i < 200; ++i) {
            in.jump = i == 35;  // take-off ~0.4 m before the box edge (x = 2 - radius)
            StepCharacter(s, p, in, 1.0f / 60.0f, box);
            if (s.landed_this_step) break;  // first landing (the box is only 4 m long)
        }
        for (int i = 0; i < 30; ++i) StepCharacter(s, p, MoveInput{}, 1.0f / 60.0f, box);  // stand still on top
        if (!(s.grounded && std::abs(s.position.z - 1.0f) < 1e-4f)) {
            std::printf("    state: x %.3f z %.3f grounded %d\n", s.position.x, s.position.z, s.grounded);
        }
        // Supported by the box: the footprint (0.7 * radius) overlaps it even if the centre is at the edge.
        CHECK(s.grounded && std::abs(s.position.z - 1.0f) < 1e-4f && s.position.x > 2.0f - 0.7f * p.radius);
    }
}
