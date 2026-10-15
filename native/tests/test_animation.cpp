// Animation clips ('70AE'), rest pose ('60SE') and the native character animator.
// Real-data tests are skipped when the game files are not present (SSPC_GAME_DIR, default
// E:\shdps4\CUSA00004).
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#include "core/file.h"
#include "resources/anim_clip.h"
#include "resources/kcap.h"
#include "resources/skeleton.h"
#include "scene/animator.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;

namespace {

std::wstring GameDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"SSPC_GAME_DIR", buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"E:\\shdps4\\CUSA00004";
}

bool OpenPackage(const wchar_t* name, KcapFile& f) {
    const std::wstring path = GameDir() + L"\\art\\cache\\" + name;
    if (!file::Exists(path)) {
        test::Skip("game package not found (set SSPC_GAME_DIR)");
        return false;
    }
    return f.Open(path) == KcapError::Ok;
}

float AngleDeg(const Quat& a, const Quat& b) {
    const float d = std::min(1.0f, std::abs(Dot(a, b)));
    return 2.0f * std::acos(d) * 180.0f / kPi;
}

// Clips identified by motion analysis (docs/native/N11_FIRST_PLAYABLE_REPORT.md), data offsets in
// core_common.sprig.xpps.
constexpr u32 kIdleClip = 0x1aed3a0;
constexpr u32 kWalkClip = 0x26762a0;
constexpr u32 kRunClip = 0x2777220;

}  // namespace

TEST(anim_component_decoding) {
    // Fixed point (exponent bits 0): m-bit magnitude, sign makes it (m+1)-bit two's complement.
    CHECK(std::abs(DecodeComponent(0, 0, 8191, 0, 0, 13) - 1.0f) < 1e-6f);
    CHECK(std::abs(DecodeComponent(0, 0, 0, 1, 0, 12)) < 1e-9f);
    CHECK(std::abs(DecodeComponent(1, 0, 4095, 1, 0, 12) - (-1.0f / 4095.0f)) < 1e-7f);
    CHECK(std::abs(DecodeComponent(1, 0, 0, 1, 0, 12) - (-4096.0f / 4095.0f)) < 1e-6f);
    // Small float: exponent bias 128 - 2^(e-1): e = 5 bits, raw 15 -> 2^(15 + 1 - 16) = 1.0.
    CHECK(DecodeComponent(0, 15, 0, 1, 5, 13) == 1.0f);
    CHECK(DecodeComponent(1, 15, 0, 1, 5, 13) == -1.0f);
    CHECK(DecodeComponent(0, 16, 1u << 12, 1, 5, 13) == 3.0f);  // 2^1 * 1.5
    CHECK(DecodeComponent(0, 0, 0, 0, 0, 0) == 0.0f);
    // Format word layout: component c at bit 2 + 10c {m:5, e:4, s:1}, dropped component in bits 0..1.
    const TrackFormat f = TrackFormat::FromWord(0xabaadab7);
    CHECK(f.dropped == 3);
    CHECK(f.mantissa[0] == 13 && f.exponent[0] == 5 && f.sign[0] == 1);
    CHECK(f.mantissa[2] == 14 && f.exponent[2] == 5 && f.sign[2] == 1);
    CHECK(f.bits == 58);
}

TEST(anim_quaternion_math) {
    const Quat a = Normalize({0.1f, 0.2f, 0.3f, 0.9f});
    const Quat b = Normalize({-0.4f, 0.1f, 0.2f, 0.8f});
    CHECK(AngleDeg(Slerp(a, b, 0.0f), a) < 1e-2f);
    CHECK(AngleDeg(Slerp(a, b, 1.0f), b) < 1e-2f);
    const Quat m = Slerp(a, b, 0.5f);
    CHECK(std::abs(AngleDeg(a, m) - AngleDeg(m, b)) < 0.05f);
    // Rotation matrix agrees with the quaternion rotation of a vector.
    const Vec3 v{0.3f, -1.2f, 2.0f};
    const Vec3 r1 = Rotate(a, v);
    const Vec3 r2 = RotationTranslation(a, {}).TransformPoint(v);
    CHECK(Length(r1 - r2) < 1e-5f);
    CHECK(Length(Rotate(a * b, v) - Rotate(a, Rotate(b, v))) < 1e-5f);
}

TEST(anim_clip_rejects_truncated_and_corrupt_data) {
    std::vector<u8> junk(0x200, 0);
    CHECK(!AnimClip::Parse(junk, 0).has_value());  // no magic
    std::memcpy(junk.data(), "70AE", 4);
    std::string err;
    CHECK(!AnimClip::Parse(junk, 0, &err).has_value());  // zero keys / segments
    CHECK(!err.empty());
    CHECK(!AnimClip::Parse(junk, 0x1f0).has_value());  // header past the end
    KcapFile f;
    if (!OpenPackage(L"lh_delsin_interact_drug_a10#tandem#0.xpps", f)) return;
    const auto data = f.Package().Data();
    const auto clip = AnimClip::Parse(data, 0x190, &err);
    CHECK(clip.has_value());
    if (!clip) return;
    // Every truncation fails cleanly; random corruptions never read out of range.
    std::vector<u8> copy(data.begin() + 0x190, data.begin() + 0x190 + clip->SizeBytes());
    for (u32 cut : {0x40u, 0x200u, 0xd00u, 0x1000u, clip->SizeBytes() / 2}) {
        CHECK(!AnimClip::Parse(std::span<const u8>(copy.data(), cut), 0).has_value());
    }
    std::mt19937 rng(1234);
    std::vector<JointPose> pose(653);
    u32 parsed = 0;
    for (int it = 0; it < 400; ++it) {
        std::vector<u8> bad = copy;
        for (int k = 0; k < 4; ++k) bad[rng() % 0x400] ^= static_cast<u8>(1u << (rng() % 8));
        if (auto c = AnimClip::Parse(bad, 0)) {
            ++parsed;
            for (float ph : {0.0f, 0.37f, 1.0f}) c->Sample(ph, pose);
        }
    }
    std::printf("    400 corrupted copies: %u still parse (sampled without faults)\n", parsed);
}

TEST(real_hero_rest_pose_reproduces_bind) {
    KcapFile f;
    if (!OpenPackage(L"hero.xpps", f)) return;
    const auto sk = FindSkeleton(f.Package());
    CHECK(sk.has_value());
    if (!sk) return;
    CHECK_EQ(sk->rest.size(), sk->JointCount());
    CHECK_EQ(sk->rest_offset, 0xf80u);
    // Local rest pose composed through the parents == bind matrices (rotation and translation).
    std::vector<Mat4> model(sk->JointCount());
    float pos_err = 0, rot_err = 0;
    for (size_t j = 0; j < sk->JointCount(); ++j) {
        const Mat4 local =
            ScaledBasis(RotationTranslation(sk->rest[j].rotation, sk->rest[j].translation * 0.01f), sk->rest[j].scale);
        model[j] = sk->parents[j] < 0 ? local : model[static_cast<size_t>(sk->parents[j])] * local;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 3; ++r) {
                const float d = std::abs(model[j].m[c][r] - sk->bind[j].m[c][r]);
                (c == 3 ? pos_err : rot_err) = std::max(c == 3 ? pos_err : rot_err, d);
            }
    }
    std::printf("    rest pose vs bind: max position error %.2e m, max basis error %.2e\n", pos_err, rot_err);
    CHECK(pos_err < 1e-4f);
    CHECK(rot_err < 1e-3f);
}

TEST(real_clip_constants_match_rest_pose) {
    KcapFile hero, scene;
    if (!OpenPackage(L"hero.xpps", hero) || !OpenPackage(L"lh_delsin_interact_drug_a10#tandem#0.xpps", scene)) return;
    const auto sk = FindSkeleton(hero.Package());
    CHECK(sk && !sk->rest.empty());
    if (!sk || sk->rest.empty()) return;
    std::string err;
    const auto clip = AnimClip::Parse(scene.Package().Data(), 0x190, &err);
    CHECK(clip.has_value());
    if (!clip) return;
    CHECK_EQ(clip->SkeletonJoints(), 413);
    CHECK_EQ(clip->KeyCount(), 161u);
    CHECK_EQ(clip->SegmentCount(), 16u);
    CHECK_EQ(clip->ConstantTranslations(), 255u);
    CHECK_EQ(clip->RotationTracks(), 141u);
    // Every constant translation (variable-width format) must equal the rest translation of its
    // joint (bone length) within the quantisation step.
    float worst = 0;
    for (const auto& [j, t] : clip->ConstantTranslationValues()) {
        const Vec3 d = t - sk->rest[j].translation;
        worst = std::max(worst, std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)}));
    }
    std::printf("    %zu constant translations, worst deviation from the rest pose %.4f cm\n",
                clip->ConstantTranslationValues().size(), worst);
    CHECK(worst < 0.02f);
}

TEST(real_core_common_clips_decode) {
    KcapFile f;
    if (!OpenPackage(L"core_common.sprig.xpps", f)) return;
    const auto data = f.Package().Data();
    const auto blobs = FindClipBlobs(data);
    CHECK_EQ(blobs.size(), static_cast<size_t>(4444));
    u32 ok = 0, hero = 0, hero_ok = 0, bad_values = 0;
    std::string err, first_err;
    std::vector<JointPose> pose(1024);
    for (u32 o : blobs) {
        const auto c = AnimClip::Parse(data, o, &err);
        const bool is_hero = data[o + 0xe] == (413 & 0xff) && data[o + 0xf] == (413 >> 8);
        hero += is_hero;
        if (!c) {
            if (first_err.empty()) first_err = err;
            continue;
        }
        ++ok;
        hero_ok += is_hero;
        for (float ph : {0.0f, 0.5f, 1.0f}) {
            for (auto& p : pose) p = {};
            c->Sample(ph, pose);
            for (u32 j = 0; j < c->SkeletonJoints() && j < pose.size(); ++j) {
                const float l = Dot(pose[j].rotation, pose[j].rotation);
                const Vec3& t = pose[j].translation;
                if (!std::isfinite(l) || std::abs(l - 1) > 1e-3f || !std::isfinite(t.x) || std::abs(t.x) > 1e5f ||
                    std::abs(t.y) > 1e5f || std::abs(t.z) > 1e5f) {
                    ++bad_values;
                }
            }
        }
    }
    std::printf("    %u/%zu clips decode (%u/%u of Delsin's 413-joint rig); first rejection: %s\n", ok, blobs.size(),
                hero_ok, hero, first_err.c_str());
    CHECK(hero_ok == hero);
    CHECK(ok + 50 >= blobs.size());  // only fixed-format animated translations are rejected
    CHECK_EQ(bad_values, 0u);
}

TEST(real_locomotion_clips_loop_and_knees) {
    KcapFile hero, cc;
    if (!OpenPackage(L"hero.xpps", hero) || !OpenPackage(L"core_common.sprig.xpps", cc)) return;
    const auto sk = FindSkeleton(hero.Package());
    CHECK(sk && !sk->rest.empty());
    if (!sk) return;
    struct Expect {
        u32 offset;
        u32 keys;
        float knee_min, knee_max;
    };
    // Left knee (341) local rotation angle: idle stays nearly straight, walk bends moderately, run
    // bends past 100 degrees.
    for (const Expect e : {Expect{kIdleClip, 433, 0, 45}, Expect{kWalkClip, 33, 0, 100}, Expect{kRunClip, 21, 5, 140}}) {
        const auto c = AnimClip::Parse(cc.Package().Data(), e.offset);
        CHECK(c.has_value());
        if (!c) continue;
        CHECK_EQ(c->KeyCount(), e.keys);
        std::vector<JointPose> first = sk->rest, last = sk->rest, p = sk->rest;
        c->Sample(0.0f, first);
        c->Sample(1.0f, last);
        float loop_err = 0;  // mean over the clip's joints: the last frame repeats the first in a loop
        for (u32 j = 0; j < 413; ++j) loop_err += AngleDeg(first[j].rotation, last[j].rotation) / 413.0f;
        float kmin = 1e9f, kmax = -1e9f;
        for (u32 fr = 0; fr < c->KeyCount(); ++fr) {
            p = sk->rest;
            c->Sample(static_cast<float>(fr) / static_cast<float>(c->KeyCount() - 1), p);
            const float knee = AngleDeg(p[341].rotation, Quat{});
            kmin = std::min(kmin, knee);
            kmax = std::max(kmax, knee);
        }
        std::printf("    clip %#x: %u keys, loop error %.2f deg, left knee %.0f..%.0f deg\n", e.offset, c->KeyCount(), loop_err,
                    kmin, kmax);
        CHECK(loop_err < 3.0f);
        CHECK(kmin >= e.knee_min && kmax <= e.knee_max);
    }
}

TEST(animator_state_machine) {
    // Synthetic two-joint skeleton, no clips: the palette stays the identity (rest == bind) and
    // the states follow the input.
    Skeleton sk;
    sk.parents = {-1, 0};
    sk.rest = {JointPose{{}, {0, 0, 0}}, JointPose{{}, {0, 0, 100}}};
    sk.bind = {Mat4::Identity(), Mat4::Translation({0, 0, 1})};
    sk.inverse_bind = {Mat4::Identity(), Mat4::Translation({0, 0, -1})};
    CharacterAnimator a;
    AnimatorClips clips;
    clips.walk_speed = 1.0f;
    clips.run_speed = 5.0f;
    CHECK(a.Init(sk, clips));
    float dev = 0;
    for (const Mat4& m : a.Palette())
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) dev = std::max(dev, std::abs(m.m[c][r] - (c == r ? 1.0f : 0.0f)));
    CHECK(dev < 1e-6f);
    auto step = [&](float speed, bool grounded, bool jumped, bool landed, int n = 1) {
        for (int i = 0; i < n; ++i) a.Update(1.0f / 60.0f, {speed, grounded, jumped, landed});
    };
    step(0, true, false, false);
    CHECK(a.State() == AnimState::Idle);
    step(1.0f, true, false, false);
    CHECK(a.State() == AnimState::Walk);
    step(5.0f, true, false, false);
    CHECK(a.State() == AnimState::Run);
    step(5.0f, false, true, false);
    CHECK(a.State() == AnimState::JumpStart);
    step(5.0f, false, false, false, 120);  // no clip: phase never advances, stays in the jump
    step(5.0f, true, false, true);
    CHECK(a.State() == AnimState::Land);
    step(0.0f, true, false, false);
    CHECK(a.State() == AnimState::Land);  // land clip missing: phase stays 0 until speed or a new jump
    step(0.0f, true, true, false);
    CHECK(a.State() == AnimState::JumpStart);
}

TEST(real_animator_walk_cycle_moves_the_feet) {
    KcapFile hero, cc;
    if (!OpenPackage(L"hero.xpps", hero) || !OpenPackage(L"core_common.sprig.xpps", cc)) return;
    const auto sk = FindSkeleton(hero.Package());
    if (!sk || sk->rest.empty()) return;
    const auto walk = AnimClip::Parse(cc.Package().Data(), kWalkClip);
    CHECK(walk.has_value());
    if (!walk) return;
    CharacterAnimator a;
    AnimatorClips clips;
    clips.walk = &*walk;
    CHECK(a.Init(*sk, clips));
    a.Preview(&*walk);
    // Left ankle (343) world position over one cycle: swings forward and back by tens of cm.
    float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f;
    for (int i = 0; i <= 33; ++i) {
        a.Update(1.0f / 30.0f, {});
        const Mat4 world = a.Palette()[343] * sk->bind[343];
        xmin = std::min(xmin, world.m[3][0]);
        xmax = std::max(xmax, world.m[3][0]);
        zmin = std::min(zmin, world.m[3][2]);
    }
    std::printf("    left ankle x %.2f..%.2f m, lowest %.3f m, animator update %.3f ms\n", xmin, xmax, zmin, a.LastUpdateMs());
    CHECK(xmax - xmin > 0.3f && xmax - xmin < 1.5f);
    CHECK(zmin > 0.0f && zmin < 0.2f);
}
