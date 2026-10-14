#include "scene/test_pose.h"

#include <cmath>

namespace sspc {
namespace {

Mat4 AxisAngle(const Vec3& axis_in, float a) {
    const Vec3 ax = Normalize(axis_in);
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat4 r = Mat4::Identity();
    r.m[0][0] = c + ax.x * ax.x * t;
    r.m[0][1] = ax.y * ax.x * t + ax.z * s;
    r.m[0][2] = ax.z * ax.x * t - ax.y * s;
    r.m[1][0] = ax.x * ax.y * t - ax.z * s;
    r.m[1][1] = c + ax.y * ax.y * t;
    r.m[1][2] = ax.z * ax.y * t + ax.x * s;
    r.m[2][0] = ax.x * ax.z * t + ax.y * s;
    r.m[2][1] = ax.y * ax.z * t - ax.x * s;
    r.m[2][2] = c + ax.z * ax.z * t;
    return r;
}

}  // namespace

void TestPose::Init(const res::Skeleton& skeleton, const TestPoseJoints& joints) {
    skeleton_ = &skeleton;
    j_ = joints;
    const int n = static_cast<int>(skeleton.JointCount());
    valid_ = n > 0;
    for (int v : {j_.shoulder_l, j_.shoulder_r, j_.elbow_l, j_.elbow_r, j_.hip_l, j_.hip_r, j_.knee_l, j_.knee_r}) {
        valid_ = valid_ && v >= 0 && v < n;
    }
    bind_locals_ = res::BindLocals(skeleton);
}

void TestPose::RotateAboutModelAxis(int joint, const Vec3& axis, float radians) {
    // Rotation about a bind-pose model-space axis through the joint, expressed in the joint's
    // local frame: R_local = Bind^-1 * R_model * Bind (translation cancels for a pivot at the joint).
    const Mat4& b = skeleton_->bind[static_cast<size_t>(joint)];
    Mat4 b_rot = b;
    b_rot.m[3][0] = b_rot.m[3][1] = b_rot.m[3][2] = 0;
    const Mat4 local_rot = res::InverseAffine(b_rot) * AxisAngle(axis, radians) * b_rot;
    locals_[static_cast<size_t>(joint)] = locals_[static_cast<size_t>(joint)] * local_rot;
}

const std::vector<Mat4>& TestPose::Evaluate(float phase, float amount) {
    locals_ = bind_locals_;
    if (valid_) {
        const float s = std::sin(phase) * amount;
        // Arms down from the T-pose (rotation about the forward axis X), plus a small swing.
        RotateAboutModelAxis(j_.shoulder_l, {1, 0, 0}, -1.25f);
        RotateAboutModelAxis(j_.shoulder_r, {1, 0, 0}, 1.25f);
        RotateAboutModelAxis(j_.shoulder_l, {0, 1, 0}, 0.35f * s);
        RotateAboutModelAxis(j_.shoulder_r, {0, 1, 0}, -0.35f * s);
        RotateAboutModelAxis(j_.elbow_l, {0, 0, 1}, 0.25f + 0.2f * amount);
        RotateAboutModelAxis(j_.elbow_r, {0, 0, 1}, -0.25f - 0.2f * amount);
        // Legs: thigh swing about the sideways axis Y; knees flex only backwards.
        RotateAboutModelAxis(j_.hip_l, {0, 1, 0}, -0.45f * s);
        RotateAboutModelAxis(j_.hip_r, {0, 1, 0}, 0.45f * s);
        RotateAboutModelAxis(j_.knee_l, {0, 1, 0}, 0.8f * amount * std::max(0.0f, -std::cos(phase)));
        RotateAboutModelAxis(j_.knee_r, {0, 1, 0}, 0.8f * amount * std::max(0.0f, std::cos(phase)));
    }
    res::ComposeModelSpace(*skeleton_, locals_, model_);
    palette_.resize(model_.size());
    for (size_t j = 0; j < model_.size(); ++j) palette_[j] = model_[j] * skeleton_->inverse_bind[j];
    return palette_;
}

}  // namespace sspc
