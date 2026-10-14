// SecondSonPC native runtime - procedural TEST pose for the hero skeleton.
//
// NOT the game's animation: the original clips are not decoded yet. This drives a few joints with
// sine curves (arms lowered from the T-pose, a walk cycle scaled by speed) only to exercise the
// skinning path end to end. Joint indices are those of hero.xpps (identified from bind positions:
// shoulders 239/293, elbows 246/300, hips 340/369, knees 341/370).
#pragma once

#include <vector>

#include "core/math.h"
#include "resources/skeleton.h"

namespace sspc {

struct TestPoseJoints {
    int shoulder_l = 239, shoulder_r = 293, elbow_l = 246, elbow_r = 300;
    int hip_l = 340, hip_r = 369, knee_l = 341, knee_r = 370;
};

class TestPose {
public:
    void Init(const res::Skeleton& skeleton, const TestPoseJoints& joints);
    // phase: walk cycle phase in radians; amount: 0 idle .. 1 full stride.
    // Returns the skinning palette (model space): model_current[j] * inverse_bind[j].
    const std::vector<Mat4>& Evaluate(float phase, float amount);
    bool Valid() const { return valid_; }

private:
    void RotateAboutModelAxis(int joint, const Vec3& axis, float radians);

    const res::Skeleton* skeleton_ = nullptr;
    TestPoseJoints j_;
    std::vector<Mat4> bind_locals_, locals_, model_, palette_;
    bool valid_ = false;
};

}  // namespace sspc
