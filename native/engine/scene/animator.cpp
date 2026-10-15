#include "scene/animator.h"

#include <algorithm>
#include <cmath>

#include "core/clock.h"

namespace sspc {

const char* ToString(AnimState s) {
    switch (s) {
        case AnimState::Idle: return "idle";
        case AnimState::Walk: return "walk";
        case AnimState::Run: return "run";
        case AnimState::JumpStart: return "jump";
        case AnimState::Fall: return "fall";
        case AnimState::Land: return "land";
        case AnimState::Preview: return "preview";
        default: return "?";
    }
}

bool CharacterAnimator::Init(const res::Skeleton& skeleton, const AnimatorClips& clips, float blend_seconds) {
    skeleton_ = nullptr;
    if (skeleton.rest.size() != skeleton.JointCount() || skeleton.JointCount() == 0) return false;
    skeleton_ = &skeleton;
    clips_ = clips;
    blend_seconds_ = std::max(blend_seconds, 1e-3f);
    pose_ = skeleton.rest;
    scratch_ = skeleton.rest;
    current_ = {};
    previous_ = {};
    blend_ = 1;
    state_ = AnimState::Idle;
    current_.clip = clips_.idle;
    BuildPalette();
    return true;
}

void CharacterAnimator::Enter(AnimState s, const res::AnimClip* clip, bool loop, float start_phase) {
    if (current_.clip) {
        previous_ = current_;
        blend_ = 0;
    }
    current_ = {clip, start_phase, 1.0f, loop};
    state_ = s;
}

void CharacterAnimator::Preview(const res::AnimClip* clip, float rate) {
    current_ = {clip, 0, rate, true};
    previous_ = {};
    blend_ = 1;
    state_ = AnimState::Preview;
}

void CharacterAnimator::Advance(Layer& l, float dt) {
    if (!l.clip || l.clip->KeyCount() < 2) return;
    l.phase += dt * l.rate * l.clip->FrameRate() / static_cast<float>(l.clip->KeyCount() - 1);
    if (l.loop) l.phase -= std::floor(l.phase);
    else l.phase = std::min(l.phase, 1.0f);
}

void CharacterAnimator::SampleLayer(const Layer& l, std::vector<res::JointPose>& out) const {
    out = skeleton_->rest;
    if (l.clip) l.clip->Sample(l.phase, out);
}

void CharacterAnimator::Update(float dt, const AnimatorInput& in) {
    if (!skeleton_) return;
    const Clock clock;
    const float speed = in.horizontal_speed;
    // Speed thresholds between the clips' own speeds.
    const float idle_below = 0.25f * clips_.walk_speed;
    const float run_above = 0.5f * (clips_.walk_speed + clips_.run_speed);
    auto locomotion = [&]() {
        if (speed < idle_below) return AnimState::Idle;
        return speed < run_above ? AnimState::Walk : AnimState::Run;
    };
    auto clip_of = [&](AnimState s) {
        switch (s) {
            case AnimState::Idle: return clips_.idle;
            case AnimState::Walk: return clips_.walk;
            case AnimState::Run: return clips_.run;
            case AnimState::JumpStart: return clips_.jump_start;
            case AnimState::Fall: return clips_.fall;
            case AnimState::Land: return clips_.land;
            default: return current_.clip;
        }
    };
    switch (state_) {
        case AnimState::Idle:
        case AnimState::Walk:
        case AnimState::Run: {
            if (in.jumped) {
                Enter(AnimState::JumpStart, clip_of(AnimState::JumpStart), false);
            } else if (!in.grounded) {
                Enter(AnimState::Fall, clip_of(AnimState::Fall), true);
            } else if (const AnimState want = locomotion(); want != state_) {
                // Walk <-> run keep the stride phase; idle restarts.
                const bool stride = want != AnimState::Idle && state_ != AnimState::Idle;
                Enter(want, clip_of(want), true, stride ? current_.phase : 0.0f);
            }
            break;
        }
        case AnimState::JumpStart:
            if (in.landed || in.grounded) Enter(AnimState::Land, clip_of(AnimState::Land), false);
            else if (current_.phase >= 1.0f) Enter(AnimState::Fall, clip_of(AnimState::Fall), true);
            break;
        case AnimState::Fall:
            if (in.landed || in.grounded) Enter(AnimState::Land, clip_of(AnimState::Land), false);
            break;
        case AnimState::Land:
            if (in.jumped) {
                Enter(AnimState::JumpStart, clip_of(AnimState::JumpStart), false);
            } else if (!in.grounded) {
                Enter(AnimState::Fall, clip_of(AnimState::Fall), true);
            } else if (current_.phase >= 1.0f || (speed >= run_above && current_.phase > 0.5f)) {
                const AnimState want = locomotion();
                Enter(want, clip_of(want), true);
            }
            break;
        default:
            break;
    }
    // Locomotion plays at the speed it was authored for.
    if (state_ == AnimState::Walk) current_.rate = std::clamp(speed / clips_.walk_speed, 0.6f, 1.6f);
    if (state_ == AnimState::Run) current_.rate = std::clamp(speed / clips_.run_speed, 0.6f, 1.4f);

    Advance(current_, dt);
    if (blend_ < 1) {
        Advance(previous_, dt);
        blend_ = std::min(1.0f, blend_ + dt / blend_seconds_);
    }
    SampleLayer(current_, pose_);
    if (blend_ < 1 && previous_.clip) {
        SampleLayer(previous_, scratch_);
        // Smoothstep weight; poses mixed per joint.
        const float w = blend_ * blend_ * (3 - 2 * blend_);
        for (size_t j = 0; j < pose_.size(); ++j) {
            pose_[j].rotation = Slerp(scratch_[j].rotation, pose_[j].rotation, w);
            pose_[j].translation = Lerp(scratch_[j].translation, pose_[j].translation, w);
            pose_[j].scale = Lerp(scratch_[j].scale, pose_[j].scale, w);
        }
    }
    BuildPalette();
    last_update_ms_ = clock.Now() * 1000.0;
}

void CharacterAnimator::BuildPalette() {
    const size_t n = skeleton_->JointCount();
    model_.resize(n);
    palette_.resize(n);
    for (size_t j = 0; j < n; ++j) {
        // T * R * S; translations cm -> m. Scale only on the leaf helper joint of the analysed rigs.
        const Mat4 local = ScaledBasis(RotationTranslation(pose_[j].rotation, pose_[j].translation * 0.01f), pose_[j].scale);
        const s16 p = skeleton_->parents[j];
        model_[j] = p < 0 ? local : model_[static_cast<size_t>(p)] * local;
        palette_[j] = model_[j] * skeleton_->inverse_bind[j];
    }
    for (size_t j = 0; j < drivers_.size() && j < n; ++j) {
        const s16 d = drivers_[j];
        if (d >= 0 && static_cast<size_t>(d) < n) palette_[j] = palette_[static_cast<size_t>(d)];
    }
}

}  // namespace sspc
