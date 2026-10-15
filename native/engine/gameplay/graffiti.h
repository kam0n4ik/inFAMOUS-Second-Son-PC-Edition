// SecondSonPC native runtime - stencil-art (graffiti) minigame.
//
// What comes from the original data (docs/native/missions/GRAFFITI_RESEARCH.md): the wall size and
// layers (stencil / paint / reveal quads per stage), the stage colours, the spray targets (markers),
// the minigame camera (CAMERA_POS / CAMERA_EULER) and where the wall stands in the world. The paint is
// revealed by an accumulated spray mask, as in the game (its renderer splats a spray disc into a
// mask with compute, eboot 0x1ed990); the brush size, rates, completion rule and camera timings of
// the original are not decoded - the values below are native choices, marked as such.
//
// Native rules: the cursor moves on the wall (mouse or stick); spraying deposits a soft disc into the
// current stage's mask; a stage is done when every marker of the stage is covered, then its mask is
// filled to full (reveal) and the next stage starts; after the last stage the camera returns and
// GraffitiCompleted is posted. Cancelling leaves the minigame with the masks kept; starting again
// resumes the current stage.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "gameplay/events.h"
#include "resources/graffiti_data.h"

namespace sspc {

struct CameraPose {
    Vec3 eye;
    Vec3 forward{1, 0, 0};
    float fov_y = 1.047f;  // radians
};

// The wall in the world: origin at the wall centre, local +X = wall normal (towards the painter),
// +Y = width (texture u), +Z = height (texture v), metres.
struct WallFrame {
    Vec3 origin;
    Quat rotation;
    Vec2 half_extent{3.4f, 1.8f};
};

struct GraffitiTuning {  // native choices (the original values are not decoded)
    u32 mask_width = 512, mask_height = 256;
    float spray_radius = 0.24f;      // metres
    float spray_rate = 3.0f;         // coverage per second at the disc centre
    float marker_radius = 0.10f;     // metres
    float marker_threshold = 0.5f;   // mean coverage around a marker
    float enter_seconds = 0.9f, exit_seconds = 0.8f, reveal_seconds = 0.6f;
    float mouse_uv_per_count = 0.0009f;
    float stick_uv_per_second = 0.45f;
    float camera_follow = 0.06f;     // radians of camera turn across the wall
    float default_fov_deg = 60.0f;   // CAMERA_FIELD_OF_VIEW is not set by the shipped graffiti
};

struct GraffitiInput {
    float mouse_dx = 0, mouse_dy = 0;  // counts this frame
    float stick_x = 0, stick_y = 0;    // -1..1
    bool spray = false;
    bool cancel = false;
};

enum class GraffitiPhase : u8 { Idle, Entering, Painting, Revealing, Exiting, Done };
const char* ToString(GraffitiPhase p);

class GraffitiMinigame {
public:
    bool Configure(const res::GraffitiDef& def, const WallFrame& wall, const GraffitiTuning& tuning = {});
    bool Configured() const { return !def_.stages.empty(); }

    // Starts (or resumes after a cancel) from Idle. False once completed.
    bool Begin(const CameraPose& gameplay);
    void Update(float dt, const GraffitiInput& in, const CameraPose& gameplay, GameEventBus& bus);

    GraffitiPhase Phase() const { return phase_; }
    bool Active() const { return phase_ != GraffitiPhase::Idle && phase_ != GraffitiPhase::Done; }
    bool Completed() const { return phase_ == GraffitiPhase::Done; }
    int Stage() const { return stage_; }
    int StageCount() const { return static_cast<int>(def_.stages.size()); }
    float StageProgress() const;  // covered markers / markers of the current stage
    bool MarkerCovered(int stage, size_t marker) const;
    size_t MarkerCount(int stage) const;
    Vec2 MarkerUv(int stage, size_t marker) const;

    Vec2 Cursor() const { return cursor_; }
    void SetCursor(Vec2 uv);
    Vec3 WallPoint(Vec2 uv, float offset = 0) const;  // world point on (or `offset` in front of) the wall
    Vec3 WallNormal() const;
    // Deposits paint at `uv` for `dt` seconds (the input path calls this while spraying).
    void SprayAt(Vec2 uv, float dt);

    const std::vector<u8>& Mask(int stage) const { return stages_[static_cast<size_t>(stage)].mask; }
    u32 MaskRevision(int stage) const { return stages_[static_cast<size_t>(stage)].revision; }
    CameraPose GraffitiCamera() const;              // the minigame camera itself
    CameraPose Camera(const CameraPose& gameplay) const;  // blended for the current phase
    float CameraBlend() const;
    const res::GraffitiDef& Def() const { return def_; }
    const WallFrame& Wall() const { return wall_; }
    const GraffitiTuning& Tuning() const { return tuning_; }

    // Progress only (an interrupted session is saved as stopped: the camera returns on load).
    std::vector<u8> Serialize() const;
    bool Deserialize(std::span<const u8> data, std::string* error);

private:
    struct StageState {
        std::vector<float> cover;  // 0..1 per texel
        std::vector<u8> mask;      // cover quantised for the GPU
        std::vector<u8> reveal_from;
        u32 revision = 0;
    };
    float Coverage(int stage, Vec2 uv, float radius) const;
    bool StageDone() const;
    void SetPhase(GraffitiPhase p);

    res::GraffitiDef def_;
    WallFrame wall_;
    GraffitiTuning tuning_;
    std::vector<StageState> stages_;
    GraffitiPhase phase_ = GraffitiPhase::Idle;
    bool finishing_ = false;  // Exiting after the last stage (else after a cancel)
    int stage_ = 0;
    float timer_ = 0;
    Vec2 cursor_{0.5f, 0.5f};
    CameraPose from_;  // gameplay camera when the session started
};

}  // namespace sspc
