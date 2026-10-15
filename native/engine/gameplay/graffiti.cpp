#include "gameplay/graffiti.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/save_file.h"

namespace sspc {
namespace {

constexpr u32 kGraffitiSaveVersion = 1;

float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vec3 ForwardFromEuler(float pitch, float yaw) {
    // Right-handed rotations about the world axes (yaw about +Z, pitch about +Y): matches the data,
    // where CAMERA_EULER z = -0.0159 aims the a8 camera at the wall centre to within 1 cm.
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

void Rle(const std::vector<u8>& in, ByteWriter& w) {
    std::vector<u8> out;
    for (size_t i = 0; i < in.size();) {
        size_t run = 1;
        while (i + run < in.size() && in[i + run] == in[i] && run < 255) ++run;
        out.push_back(static_cast<u8>(run));
        out.push_back(in[i]);
        i += run;
    }
    w.PutBytes(out);
}

bool UnRle(ByteReader& r, size_t expected, std::vector<u8>& out) {
    std::vector<u8> packed;
    if (!r.GetBytes(packed, static_cast<u32>(expected * 2 + 16)) || packed.size() % 2) return false;
    out.clear();
    out.reserve(expected);
    for (size_t i = 0; i < packed.size(); i += 2) {
        if (packed[i] == 0 || out.size() + packed[i] > expected) return false;
        out.insert(out.end(), packed[i], packed[i + 1]);
    }
    return out.size() == expected;
}

}  // namespace

const char* ToString(GraffitiPhase p) {
    static const char* const kNames[] = {"Idle", "Entering", "Painting", "Revealing", "Exiting", "Done"};
    const auto i = static_cast<size_t>(p);
    return i < std::size(kNames) ? kNames[i] : "?";
}

bool GraffitiMinigame::Configure(const res::GraffitiDef& def, const WallFrame& wall, const GraffitiTuning& tuning) {
    if (def.stages.empty() || wall.half_extent.x <= 0 || wall.half_extent.y <= 0 || tuning.mask_width == 0 ||
        tuning.mask_height == 0 || tuning.mask_width > 4096 || tuning.mask_height > 4096) {
        return false;
    }
    def_ = def;
    wall_ = wall;
    tuning_ = tuning;
    const size_t n = static_cast<size_t>(tuning.mask_width) * tuning.mask_height;
    stages_.assign(def.stages.size(), StageState{});
    for (StageState& s : stages_) {
        s.cover.assign(n, 0.0f);
        s.mask.assign(n, 0);
    }
    phase_ = GraffitiPhase::Idle;
    stage_ = 0;
    timer_ = 0;
    cursor_ = {0.5f, 0.5f};
    return true;
}

void GraffitiMinigame::SetPhase(GraffitiPhase p) {
    phase_ = p;
    timer_ = 0;
}

bool GraffitiMinigame::Begin(const CameraPose& gameplay) {
    if (!Configured() || phase_ != GraffitiPhase::Idle) return false;
    from_ = gameplay;
    finishing_ = false;
    SetPhase(GraffitiPhase::Entering);
    return true;
}

void GraffitiMinigame::SetCursor(Vec2 uv) { cursor_ = {std::clamp(uv.x, 0.0f, 1.0f), std::clamp(uv.y, 0.0f, 1.0f)}; }

Vec3 GraffitiMinigame::WallNormal() const { return Rotate(wall_.rotation, Vec3{1, 0, 0}); }

Vec3 GraffitiMinigame::WallPoint(Vec2 uv, float offset) const {
    const Vec3 local{offset, (uv.x * 2.0f - 1.0f) * wall_.half_extent.x, (uv.y * 2.0f - 1.0f) * wall_.half_extent.y};
    return wall_.origin + Rotate(wall_.rotation, local);
}

size_t GraffitiMinigame::MarkerCount(int stage) const {
    return stage >= 0 && stage < StageCount() ? def_.stages[static_cast<size_t>(stage)].markers_cm.size() : 0;
}

Vec2 GraffitiMinigame::MarkerUv(int stage, size_t marker) const {
    const Vec3& m = def_.stages[static_cast<size_t>(stage)].markers_cm[marker];
    return {(m.y * 0.01f + wall_.half_extent.x) / (2 * wall_.half_extent.x),
            (m.z * 0.01f + wall_.half_extent.y) / (2 * wall_.half_extent.y)};
}

float GraffitiMinigame::Coverage(int stage, Vec2 uv, float radius) const {
    const StageState& s = stages_[static_cast<size_t>(stage)];
    const int w = static_cast<int>(tuning_.mask_width), h = static_cast<int>(tuning_.mask_height);
    const float ru = radius / (2 * wall_.half_extent.x), rv = radius / (2 * wall_.half_extent.y);
    const int x0 = std::max(0, static_cast<int>((uv.x - ru) * w)), x1 = std::min(w - 1, static_cast<int>((uv.x + ru) * w));
    const int y0 = std::max(0, static_cast<int>((uv.y - rv) * h)), y1 = std::min(h - 1, static_cast<int>((uv.y + rv) * h));
    double sum = 0;
    int n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float du = ((x + 0.5f) / w - uv.x) / ru, dv = ((y + 0.5f) / h - uv.y) / rv;
            if (du * du + dv * dv > 1.0f) continue;
            sum += s.cover[static_cast<size_t>(y) * w + x];
            ++n;
        }
    }
    return n ? static_cast<float>(sum / n) : 0.0f;
}

bool GraffitiMinigame::MarkerCovered(int stage, size_t marker) const {
    if (stage < 0 || stage >= StageCount() || marker >= MarkerCount(stage)) return false;
    return Coverage(stage, MarkerUv(stage, marker), tuning_.marker_radius) >= tuning_.marker_threshold;
}

float GraffitiMinigame::StageProgress() const {
    if (stage_ >= StageCount()) return 1.0f;
    const size_t n = MarkerCount(stage_);
    if (n == 0) return 0.0f;
    size_t covered = 0;
    for (size_t i = 0; i < n; ++i) covered += MarkerCovered(stage_, i) ? 1 : 0;
    return static_cast<float>(covered) / static_cast<float>(n);
}

bool GraffitiMinigame::StageDone() const {
    const size_t n = MarkerCount(stage_);
    if (n == 0) return false;  // a stage without markers cannot be judged (none in the shipped data)
    for (size_t i = 0; i < n; ++i) {
        if (!MarkerCovered(stage_, i)) return false;
    }
    return true;
}

void GraffitiMinigame::SprayAt(Vec2 uv, float dt) {
    if (stage_ >= StageCount() || dt <= 0) return;
    StageState& s = stages_[static_cast<size_t>(stage_)];
    const int w = static_cast<int>(tuning_.mask_width), h = static_cast<int>(tuning_.mask_height);
    const float ru = tuning_.spray_radius / (2 * wall_.half_extent.x), rv = tuning_.spray_radius / (2 * wall_.half_extent.y);
    const int x0 = std::max(0, static_cast<int>((uv.x - ru) * w)), x1 = std::min(w - 1, static_cast<int>((uv.x + ru) * w));
    const int y0 = std::max(0, static_cast<int>((uv.y - rv) * h)), y1 = std::min(h - 1, static_cast<int>((uv.y + rv) * h));
    const float amount = tuning_.spray_rate * dt;
    bool changed = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float du = ((x + 0.5f) / w - uv.x) / ru, dv = ((y + 0.5f) / h - uv.y) / rv;
            const float d2 = du * du + dv * dv;
            if (d2 >= 1.0f) continue;
            const float f = (1.0f - d2) * (1.0f - d2);
            const size_t i = static_cast<size_t>(y) * w + x;
            const float c = std::min(1.0f, s.cover[i] + amount * f);
            if (c == s.cover[i]) continue;
            s.cover[i] = c;
            const u8 q = static_cast<u8>(std::lround(c * 255.0f));
            if (q != s.mask[i]) {
                s.mask[i] = q;
                changed = true;
            }
        }
    }
    if (changed) ++s.revision;
}

void GraffitiMinigame::Update(float dt, const GraffitiInput& in, const CameraPose& gameplay, GameEventBus& bus) {
    if (!Configured()) return;
    timer_ += dt;
    switch (phase_) {
        case GraffitiPhase::Idle:
        case GraffitiPhase::Done:
            return;
        case GraffitiPhase::Entering:
            if (timer_ >= tuning_.enter_seconds) {
                SetPhase(GraffitiPhase::Painting);
                bus.Post(EventType::GraffitiStarted, def_.oid, stage_);
            }
            return;
        case GraffitiPhase::Painting: {
            if (in.cancel) {
                finishing_ = false;
                from_ = gameplay;
                SetPhase(GraffitiPhase::Exiting);
                bus.Post(EventType::GraffitiAborted, def_.oid, stage_);
                return;
            }
            // Mouse right = +u (texture u runs along the wall to the painter's right), up = +v.
            SetCursor({cursor_.x + in.mouse_dx * tuning_.mouse_uv_per_count + in.stick_x * tuning_.stick_uv_per_second * dt,
                       cursor_.y - in.mouse_dy * tuning_.mouse_uv_per_count * (wall_.half_extent.x / wall_.half_extent.y) +
                           in.stick_y * tuning_.stick_uv_per_second * dt * (wall_.half_extent.x / wall_.half_extent.y)});
            if (in.spray) SprayAt(cursor_, dt);
            if (StageDone()) {
                StageState& s = stages_[static_cast<size_t>(stage_)];
                s.reveal_from = s.mask;
                SetPhase(GraffitiPhase::Revealing);
            }
            return;
        }
        case GraffitiPhase::Revealing: {
            StageState& s = stages_[static_cast<size_t>(stage_)];
            const float t = Smooth(timer_ / tuning_.reveal_seconds);
            for (size_t i = 0; i < s.mask.size(); ++i) {
                const float v = s.reveal_from[i] + (255.0f - s.reveal_from[i]) * t;
                s.mask[i] = static_cast<u8>(std::lround(v));
                s.cover[i] = s.mask[i] / 255.0f;
            }
            ++s.revision;
            if (timer_ < tuning_.reveal_seconds) return;
            s.reveal_from.clear();
            bus.Post(EventType::GraffitiStageCompleted, def_.oid, stage_);
            ++stage_;
            if (stage_ < StageCount()) {
                SetPhase(GraffitiPhase::Painting);
            } else {
                finishing_ = true;
                from_ = gameplay;
                SetPhase(GraffitiPhase::Exiting);
            }
            return;
        }
        case GraffitiPhase::Exiting:
            if (timer_ < tuning_.exit_seconds) return;
            SetPhase(finishing_ ? GraffitiPhase::Done : GraffitiPhase::Idle);
            if (finishing_) bus.Post(EventType::GraffitiCompleted, def_.oid, StageCount());
            return;
    }
}

CameraPose GraffitiMinigame::GraffitiCamera() const {
    CameraPose p;
    p.eye = def_.camera_pos_cm * 0.01f;
    const float follow_yaw = -(cursor_.x - 0.5f) * tuning_.camera_follow;
    const float follow_pitch = -(cursor_.y - 0.5f) * tuning_.camera_follow * 0.6f;
    p.forward = ForwardFromEuler(def_.camera_euler.y + follow_pitch, def_.camera_euler.z + follow_yaw);
    const float fov_deg = def_.camera_fov.value_or(tuning_.default_fov_deg);
    p.fov_y = std::clamp(fov_deg, 10.0f, 120.0f) * kPi / 180.0f;
    return p;
}

float GraffitiMinigame::CameraBlend() const {
    switch (phase_) {
        case GraffitiPhase::Entering: return Smooth(timer_ / tuning_.enter_seconds);
        case GraffitiPhase::Painting:
        case GraffitiPhase::Revealing: return 1.0f;
        case GraffitiPhase::Exiting: return 1.0f - Smooth(timer_ / tuning_.exit_seconds);
        default: return 0.0f;
    }
}

CameraPose GraffitiMinigame::Camera(const CameraPose& gameplay) const {
    const float t = CameraBlend();
    if (t <= 0) return gameplay;
    const CameraPose g = GraffitiCamera();
    // Entering blends from the pose the session started at; exiting back to the live gameplay pose.
    const CameraPose& a = phase_ == GraffitiPhase::Entering ? from_ : gameplay;
    CameraPose p;
    p.eye = Lerp(a.eye, g.eye, t);
    p.forward = Normalize(Lerp(a.forward, g.forward, t));
    p.fov_y = a.fov_y + (g.fov_y - a.fov_y) * t;
    return p;
}

std::vector<u8> GraffitiMinigame::Serialize() const {
    ByteWriter w;
    w.Put(kGraffitiSaveVersion);
    w.Put(def_.oid);
    w.Put(static_cast<u32>(stages_.size()));
    w.Put(tuning_.mask_width);
    w.Put(tuning_.mask_height);
    w.Put(static_cast<s32>(stage_));
    w.Put(static_cast<u8>(phase_ == GraffitiPhase::Done ? 1 : 0));
    w.Put(cursor_.x);
    w.Put(cursor_.y);
    for (const StageState& s : stages_) {
        // A stage interrupted mid-reveal is saved as revealed only if it completed.
        Rle(s.mask, w);
    }
    return w.Take();
}

bool GraffitiMinigame::Deserialize(std::span<const u8> data, std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    ByteReader r(data);
    u32 version = 0, count = 0, mw = 0, mh = 0;
    u64 oid = 0;
    s32 stage = 0;
    u8 done = 0;
    Vec2 cursor;
    r.Get(version);
    r.Get(oid);
    r.Get(count);
    r.Get(mw);
    r.Get(mh);
    r.Get(stage);
    r.Get(done);
    r.Get(cursor.x);
    r.Get(cursor.y);
    if (!r.Ok() || version != kGraffitiSaveVersion) return fail("graffiti state version");
    if (oid != def_.oid || count != stages_.size() || mw != tuning_.mask_width || mh != tuning_.mask_height) {
        return fail("graffiti state belongs to another wall or mask size");
    }
    if (stage < 0 || stage > static_cast<s32>(count) || (done && stage != static_cast<s32>(count)) ||
        !std::isfinite(cursor.x) || !std::isfinite(cursor.y)) {
        return fail("graffiti state values out of range");
    }
    std::vector<std::vector<u8>> masks(count);
    for (u32 i = 0; i < count; ++i) {
        if (!UnRle(r, static_cast<size_t>(mw) * mh, masks[i])) return fail("graffiti mask corrupted");
    }
    if (!r.AtEnd()) return fail("graffiti state has trailing bytes");
    for (u32 i = 0; i < count; ++i) {
        StageState& s = stages_[i];
        s.mask = std::move(masks[i]);
        for (size_t k = 0; k < s.mask.size(); ++k) s.cover[k] = s.mask[k] / 255.0f;
        s.reveal_from.clear();
        ++s.revision;
    }
    stage_ = stage;
    SetCursor(cursor);
    finishing_ = false;
    if (!done && stage_ == StageCount()) {
        // Saved in the final exit transition, before GraffitiCompleted was posted: finish on the next update
        // (the exit timer has run out, so the camera is already at the gameplay pose).
        finishing_ = true;
        SetPhase(GraffitiPhase::Exiting);
        timer_ = tuning_.exit_seconds;
        return true;
    }
    SetPhase(done ? GraffitiPhase::Done : GraffitiPhase::Idle);
    return true;
}

}  // namespace sspc
