// SecondSonPC native runtime - character animation from the game's own clips.
//
// Plays original animation clips (resources/anim_clip.h) on the character skeleton and produces
// the skinning palette. A small native state machine picks the clips:
//   Idle <-> Walk <-> Run          by horizontal speed (thresholds between the clips' own speeds)
//   grounded -> JumpStart -> Fall  on a jump / leaving the ground
//   Fall -> Land -> locomotion     on landing
// Transitions cross-fade the two poses (slerp / lerp per joint). Locomotion clips are played at
// rate = speed / clip speed so the planted foot does not slide; the clip speeds are measured from
// the clips' own foot plants (docs/native/N11_FIRST_PLAYABLE_REPORT.md), not from game code.
//
// STATUS: the state machine and its thresholds are native; the game's own animation graph
// (ANIMATION_RUN / STRAFE_LAUNCH / STRAFE_LANDING objects) is not reconstructed yet.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "core/math.h"
#include "resources/anim_clip.h"
#include "resources/skeleton.h"

namespace sspc {

enum class AnimState : u8 { Idle, Walk, Run, JumpStart, Fall, Land, Preview, Count };
const char* ToString(AnimState s);

struct AnimatorClips {
    const res::AnimClip* idle = nullptr;
    const res::AnimClip* walk = nullptr;
    const res::AnimClip* run = nullptr;
    const res::AnimClip* jump_start = nullptr;
    const res::AnimClip* fall = nullptr;
    const res::AnimClip* land = nullptr;
    float walk_speed = 1.0f;  // m/s at playback rate 1 (measured from the clip's foot plant)
    float run_speed = 5.0f;
};

struct AnimatorInput {
    float horizontal_speed = 0;  // m/s
    bool grounded = true;
    bool jumped = false;  // jump started this update
    bool landed = false;  // touched the ground this update
};

class CharacterAnimator {
public:
    // `skeleton` must outlive the animator and have a rest pose (Skeleton::rest).
    bool Init(const res::Skeleton& skeleton, const AnimatorClips& clips, float blend_seconds = 0.2f);
    bool Valid() const { return skeleton_ != nullptr; }

    void Update(float dt, const AnimatorInput& in);
    // Plays one clip in a loop (clip viewer / tests); `rate` 1 = 30 frames per second.
    void Preview(const res::AnimClip* clip, float rate = 1.0f);
    // Jumps to a phase of the current clip (screenshots of a given frame).
    void SetPhase(float phase) { current_.phase = phase; }
    // Joints whose skinning matrix copies another joint's (LoadedModel::joint_driver, -1 = none).
    void SetJointDrivers(std::vector<s16> drivers) { drivers_ = std::move(drivers); }

    const std::vector<Mat4>& Palette() const { return palette_; }
    const std::vector<res::JointPose>& LocalPose() const { return pose_; }
    AnimState State() const { return state_; }
    float Phase() const { return current_.phase; }
    double LastUpdateMs() const { return last_update_ms_; }

private:
    struct Layer {
        const res::AnimClip* clip = nullptr;
        float phase = 0;  // 0..1
        float rate = 1;   // playback rate (1 = clip frame rate)
        bool loop = true;
    };
    void Enter(AnimState s, const res::AnimClip* clip, bool loop, float start_phase = 0);
    static void Advance(Layer& l, float dt);
    void SampleLayer(const Layer& l, std::vector<res::JointPose>& out) const;
    void BuildPalette();

    const res::Skeleton* skeleton_ = nullptr;
    AnimatorClips clips_;
    float blend_seconds_ = 0.2f;
    AnimState state_ = AnimState::Idle;
    Layer current_, previous_;
    float blend_ = 1.0f;  // 0 = previous pose, 1 = current pose
    std::vector<res::JointPose> pose_, scratch_;
    std::vector<Mat4> model_, palette_;
    std::vector<s16> drivers_;
    double last_update_ms_ = 0;
};

}  // namespace sspc
