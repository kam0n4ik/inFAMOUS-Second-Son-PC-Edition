#include <cmath>

#include "core/file.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/skeleton.h"
#include "scene/test_pose.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;

TEST(skeleton_inverse_affine) {
    Mat4 m = Mat4::Translation({1, 2, 3}) * Mat4::RotationZ(0.7f) * Mat4::Scale(2.0f);
    const Mat4 i = InverseAffine(m) * m;
    float err = 0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) err = std::max(err, std::abs(i.m[c][r] - (c == r ? 1.0f : 0.0f)));
    CHECK(err < 1e-5f);
}

TEST(real_hero_skeleton_and_skin) {
    const std::wstring path = L"E:\\shdps4\\CUSA00004\\art\\cache\\hero.xpps";
    if (!file::Exists(path)) {
        test::Skip("hero.xpps not found");
        return;
    }
    KcapFile f;
    CHECK_EQ(f.Open(path), KcapError::Ok);
    const auto sk = FindSkeleton(f.Package());
    CHECK(sk.has_value());
    if (!sk) return;
    CHECK_EQ(sk->JointCount(), static_cast<size_t>(653));
    CHECK_EQ(sk->bind_offset, 0xb6b0u);
    // Pelvis at ~1.01 m, stored inverse bind really is the inverse of the bind pose.
    CHECK(std::abs(sk->bind[1].m[3][2] - 1.0124f) < 1e-3f);
    float worst = 0;
    for (size_t j = 0; j < sk->JointCount(); ++j) {
        const Mat4 i = sk->inverse_bind[j] * sk->bind[j];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) worst = std::max(worst, std::abs(i.m[c][r] - (c == r ? 1.0f : 0.0f)));
    }
    std::printf("    653 joints, max |inverse_bind * bind - I| = %.2e\n", worst);
    CHECK(worst < 1e-3f);
    // Locals recompose to the bind pose.
    std::vector<Mat4> model;
    ComposeModelSpace(*sk, BindLocals(*sk), model);
    float dev = 0;
    for (size_t j = 0; j < model.size(); ++j)
        for (int k = 0; k < 3; ++k) dev = std::max(dev, std::abs(model[j].m[3][k] - sk->bind[j].m[3][k]));
    CHECK(dev < 1e-4f);
    // Rest pose of the test pose with zero motion still lowers the arms but leaves the hips alone.
    TestPose pose;
    pose.Init(*sk, TestPoseJoints{});
    CHECK(pose.Valid());
    const auto& pal = pose.Evaluate(0, 0);
    CHECK(std::abs(pal[1].m[3][0]) < 1e-5f && std::abs(pal[1].m[0][0] - 1.0f) < 1e-5f);  // pelvis untouched
    // Skin streams decode: joint 0 gets the implicit weight, weights sum to 255.
    const auto rec = ReadGeometry(f.Package(), 0xc0800);
    MeshData m;
    CHECK(rec && DecodeGeometry(f.Package(), *rec, m, nullptr));
    CHECK(m.joints.size() == m.positions.size() && m.weights.size() == m.positions.size());
    bool sums = true, ranges = true;
    for (size_t v = 0; v < m.weights.size(); ++v) {
        sums &= m.weights[v][0] + m.weights[v][1] + m.weights[v][2] + m.weights[v][3] == 255;
        for (u16 jj : m.joints[v]) ranges &= jj < 653;
    }
    CHECK(sums && ranges);
}
