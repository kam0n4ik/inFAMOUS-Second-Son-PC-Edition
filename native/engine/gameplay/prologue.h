// SecondSonPC native runtime - the first playable fragment of the original prologue (mission VOR).
//
// Wires original data to the native systems: the mission package msn_vor (hero placement points,
// trigger volumes), the stencil art graffiti_a8_family (wall layers, stages, markers, camera) placed by
// its PROXY_SITE in chunk a8, the localized texts, and a native mission definition
// (config/missions/vor_prologue.mission). Interaction "graffiti_a8_family" starts the minigame; the
// minigame reports its events on the bus; the mission runtime reacts to them. See
// docs/native/missions/N12_CAMPAIGN_RECONSTRUCTION.md.
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/name_hash.h"
#include "gameplay/events.h"
#include "gameplay/graffiti.h"
#include "gameplay/interaction.h"
#include "gameplay/mission/mission_runtime.h"
#include "renderer/renderer.h"
#include "renderer/text_overlay.h"
#include "resources/game_text.h"
#include "resources/graffiti_data.h"
#include "resources/mission_data.h"

namespace sspc {

class Config;
class Input;

struct PrologueSettings {
    std::wstring mission_file;      // native definition
    std::wstring mission_package;   // msn_vor.sprig.xpps
    std::wstring graffiti_package;  // graffiti_a8_family.xpps
    std::wstring graffiti_chunk;    // a8.xpps (holds the PROXY_SITE that places the minigame)
    std::wstring text_package;      // lang_english_text.xpps
    std::wstring save_file;
    // Origin of the chunk's object frame in the world (metres). For a8: (100, -200, 0), derived from
    // the minigame camera (world) against the PROXY_SITE (chunk frame); checked by tests.
    Vec3 chunk_origin{100, -200, 0};
    GraffitiTuning tuning;
};
// Reads [Prologue] (paths relative to the data / executable directory). False if disabled.
bool ReadPrologueSettings(const Config& cfg, const std::wstring& data_dir, const std::wstring& exe_dir, PrologueSettings& out);

struct HeroPlacement {
    Vec3 feet;
    float yaw = 0;
};

struct PrologueFrameIn {
    float dt = 0;
    const Input* input = nullptr;
    bool look_enabled = true;  // mouse captured (mouse deltas mean "look/aim")
    float look_dx = 0, look_dy = 0;
    Vec3 player_feet;
    float player_yaw = 0;
    CameraPose gameplay_camera;
    bool scripted = false;  // automated run: the slice drives interaction and spraying itself
};

struct PrologueFrameOut {
    bool lock_player = false;
    bool camera_override = false;
    CameraPose camera;
    std::optional<float> face_yaw;
};

class PrologueSlice final : public mission::IMissionHost {
public:
    PrologueSlice() = default;
    PrologueSlice(const PrologueSlice&) = delete;
    PrologueSlice& operator=(const PrologueSlice&) = delete;
    ~PrologueSlice() override;

    // `renderer` may be null (tests): data and logic only, no GPU resources.
    bool Load(const PrologueSettings& settings, Renderer* renderer, std::string& error);
    void StartMission();
    bool Loaded() const { return loaded_; }

    std::optional<HeroPlacement> TakeHeroPlacement();
    PrologueFrameOut Update(const PrologueFrameIn& in);
    void UploadMasks(Renderer& renderer);
    void AppendDraws(std::vector<DrawItem>& items) const;
    void DrawHud(TextOverlay& overlay, bool debug) const;

    // APPROXIMATION for geometry that is not decoded yet: the cannery building with the billboard is
    // a model of OBJECT_DATA objects in a8 (not chunk scene instances), so the billboard catwalk the
    // hero stands on (HERO_MATCH_POINT height, wall width) is supplied as collision triangles here.
    std::vector<Vec3> ApproximateCollision() const;
    static constexpr u64 kRoofExitFallback = NameHash("vor_roof_exit_fallback");

    bool SaveGame(const std::wstring& path, const Vec3& feet, float yaw) const;
    bool LoadGame(const std::wstring& path, std::string& error);

    const mission::MissionManager& Missions() const { return missions_; }
    mission::MissionManager& Missions() { return missions_; }
    const GraffitiMinigame& Graffiti() const { return graffiti_; }
    GraffitiMinigame& Graffiti() { return graffiti_; }
    InteractionSystem& Interactions() { return interactions_; }
    GameEventBus& Bus() { return bus_; }
    const res::GraffitiDef& GraffitiDefinition() const { return graffiti_def_; }
    const res::MissionPackageInfo& MissionPackage() const { return mission_info_; }
    const WallFrame& Wall() const { return wall_; }
    std::string Text(u64 key_hash) const;  // localized text with PC button names ("" if missing)
    std::vector<std::string> Log() const { return {log_.begin(), log_.end()}; }

    // IMissionHost
    void ObjectiveChanged(const mission::MissionDef& mission, const mission::ObjectiveDef* objective) override;
    void ShowHint(const mission::Action& a) override;
    void ShowSubtitle(const mission::Action& a) override;
    void SetInteractionEnabled(u64 id, bool enabled) override;
    void PlaceHero(u64 point_id) override;
    void MissionLog(std::string_view text) override;

    static constexpr u64 kGraffitiInteraction = NameHash("graffiti_a8_family");

private:
    struct Layer {
        MeshHandle mesh = kInvalidMesh;
        TextureHandle texture = kWhiteTexture;
    };
    struct StageGpu {
        Layer stencil, paint;
        TextureHandle mask = kWhiteTexture;
        u32 uploaded_revision = ~0u;
        DecalMode paint_mode = DecalMode::TintedShape;
    };
    struct Timed {
        u64 key = 0;
        std::string key_name;
        float time_left = 0;
    };
    bool LoadGraffiti(const PrologueSettings& s, Renderer* renderer, std::string& error);
    void ScriptedInput(const PrologueFrameIn& in, bool& interact, GraffitiInput& g);
    void Note(std::string text);

    bool loaded_ = false;
    PrologueSettings settings_;
    GameEventBus bus_;
    mission::MissionManager missions_;
    u64 mission_id_ = 0;
    InteractionSystem interactions_;
    GraffitiMinigame graffiti_;
    res::GraffitiDef graffiti_def_;
    res::MissionPackageInfo mission_info_;
    res::GameText text_;
    bool have_text_ = false;
    WallFrame wall_;
    Mat4 wall_matrix_ = Mat4::Identity();
    std::vector<StageGpu> stages_;
    MeshHandle marker_mesh_ = kInvalidMesh;
    MeshHandle approx_mesh_ = kInvalidMesh;  // catwalk + billboard backing (approximation, see above)
    float catwalk_top_ = 0;                  // world z of the catwalk (HERO_MATCH_POINT height)
    std::optional<HeroPlacement> pending_hero_;
    double time_ = 0;
    u32 bus_subscription_ = 0;

    // HUD state
    const mission::ObjectiveDef* objective_ = nullptr;
    std::optional<Timed> hint_, subtitle_;
    std::deque<std::string> log_;
    float note_time_ = 0;
    bool focused_prompt_ = false;
    bool begin_graffiti_ = false;
    float scripted_clock_ = 0;
    Vec2 scripted_jitter_{0, 0};
};

}  // namespace sspc
