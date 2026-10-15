#include "gameplay/prologue.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/config.h"
#include "core/file.h"
#include "core/log.h"
#include "core/save_file.h"
#include "platform/input.h"
#include "resources/geometry.h"
#include "resources/kcap.h"
#include "resources/texture.h"
#include "resources/texture_library.h"

namespace sspc {
namespace {

constexpr u32 kTagMission = SaveTag("MISN");
constexpr u32 kTagGraffiti = SaveTag("GRAF");
constexpr u32 kTagInteract = SaveTag("INTR");
constexpr u32 kTagPlayer = SaveTag("PLYR");
constexpr size_t kLogLines = 8;

std::wstring Resolve(const std::wstring& dir, const std::string& rel) {
    const std::wstring r = log::Widen(rel);
    if (r.size() > 1 && (r[1] == L':' || r[0] == L'\\' || r[0] == L'/')) return r;
    return dir + L"\\" + r;
}

bool ParseVec3(const std::string& s, Vec3& out) {
    float v[3];
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t end = s.find(',', pos);
        const std::string part = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        char* e = nullptr;
        v[i] = std::strtof(part.c_str(), &e);
        if (e == part.c_str() || !std::isfinite(v[i])) return false;
        if (end == std::string::npos && i < 2) return false;
        pos = end + 1;
    }
    out = {v[0], v[1], v[2]};
    return true;
}

// The game's texts use private-use glyphs for controller buttons; show PC bindings instead.
std::string PcButtons(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        if (c == 0xef && i + 2 < s.size() && static_cast<u8>(s[i + 1]) == 0x82) {
            switch (static_cast<u8>(s[i + 2])) {
                case 0x83: out += "[Space/A]"; break;
                case 0x85: out += "[RT]"; break;
                case 0x87: out += "[LMB]"; break;
                case 0x8a: out += "[WASD/LS]"; break;
                case 0x8e: out += "[Enter/A]"; break;
                default: out += "[button]"; break;
            }
            i += 3;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

// Box in a local frame -> 12 world triangles, counter-clockwise seen from outside.
void AddBox(std::vector<Vec3>& tris, const Mat4& m, const Vec3& lo, const Vec3& hi) {
    const Vec3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                       {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    static constexpr int kFaces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    for (const auto& f : kFaces) {
        const Vec3 a = m.TransformPoint(c[f[0]]), b = m.TransformPoint(c[f[1]]), cc = m.TransformPoint(c[f[2]]),
                   d = m.TransformPoint(c[f[3]]);
        tris.insert(tris.end(), {a, b, cc, cc, d, a});
    }
}

std::string Speaker(std::string_view key) {
    if (key.ends_with("_hro")) return "DELSIN: ";
    if (key.ends_with("_reg")) return "REGGIE: ";
    return {};
}

}  // namespace

bool ReadPrologueSettings(const Config& cfg, const std::wstring& data_dir, const std::wstring& exe_dir, PrologueSettings& out) {
    if (!cfg.GetBool("Prologue", "Enabled", false)) return false;
    out.mission_file = Resolve(exe_dir, cfg.GetString("Prologue", "MissionFile", "missions\\vor_prologue.mission"));
    out.mission_package = Resolve(data_dir, cfg.GetString("Prologue", "MissionPackage", "art\\cache\\msn_vor.sprig.xpps"));
    out.graffiti_package = Resolve(data_dir, cfg.GetString("Prologue", "GraffitiPackage", "art\\cache\\graffiti_a8_family.xpps"));
    out.graffiti_chunk = Resolve(data_dir, cfg.GetString("Prologue", "GraffitiChunk", "art\\cache\\a8.xpps"));
    out.text_package = Resolve(data_dir, cfg.GetString("Prologue", "TextPackage", "art\\cache\\lang_english_text.xpps"));
    out.save_file = Resolve(exe_dir, cfg.GetString("Prologue", "SaveFile", "saves\\prologue.sav"));
    Vec3 origin;
    if (ParseVec3(cfg.GetString("Prologue", "ChunkOrigin", "100, -200, 0"), origin)) out.chunk_origin = origin;
    GraffitiTuning& t = out.tuning;
    t.spray_radius = static_cast<float>(cfg.GetFloat("Prologue", "SprayRadius", t.spray_radius));
    t.spray_rate = static_cast<float>(cfg.GetFloat("Prologue", "SprayRate", t.spray_rate));
    t.mouse_uv_per_count = static_cast<float>(cfg.GetFloat("Prologue", "CursorMouseSpeed", t.mouse_uv_per_count));
    t.stick_uv_per_second = static_cast<float>(cfg.GetFloat("Prologue", "CursorStickSpeed", t.stick_uv_per_second));
    return true;
}

PrologueSlice::~PrologueSlice() {
    missions_.Detach();
    if (bus_subscription_) bus_.Unsubscribe(bus_subscription_);
}

bool PrologueSlice::LoadGraffiti(const PrologueSettings& s, Renderer* renderer, std::string& error) {
    res::KcapFile gfile, chunk;
    if (const auto e = gfile.Open(s.graffiti_package); e != res::KcapError::Ok) {
        error = std::format("graffiti package {}: {}", log::Narrow(s.graffiti_package), res::ToString(e));
        return false;
    }
    const res::KcapPackage& gp = gfile.Package();
    res::ObjectCatalog gcat;
    if (!gcat.Build(gp, &error) || !res::LoadGraffitiDefinition(gp, gcat, graffiti_def_, &error)) return false;
    if (const auto e = chunk.Open(s.graffiti_chunk); e != res::KcapError::Ok) {
        error = std::format("chunk {}: {}", log::Narrow(s.graffiti_chunk), res::ToString(e));
        return false;
    }
    res::ObjectCatalog ccat;
    if (!ccat.Build(chunk.Package(), &error)) return false;
    const auto site = res::FindGraffitiSite(ccat, graffiti_def_.oid);
    if (!site) {
        error = std::format("no PROXY_SITE for graffiti {:#x} in {}", graffiti_def_.oid, log::Narrow(s.graffiti_chunk));
        return false;
    }
    // Layers of every stage: geometry and colour maps of the graffiti package.
    res::TextureLibrary lib;
    lib.AddPackage(gp);
    std::vector<res::MeshData> meshes;
    stages_.assign(graffiti_def_.stages.size(), StageGpu{});
    Vec2 half{graffiti_def_.phys_dimensions_cm.y * 0.005f, graffiti_def_.phys_dimensions_cm.z * 0.005f};
    for (size_t i = 0; i < graffiti_def_.stages.size(); ++i) {
        const res::GraffitiStageDef& st = graffiti_def_.stages[i];
        StageGpu& g = stages_[i];
        g.paint_mode = st.reveal_geometry != res::kNoGeometry ? DecalMode::TintedShape : DecalMode::Image;
        for (int which = 0; which < 2; ++which) {
            const u64 id = which == 0 ? st.stencil_geometry : st.paint_geometry;
            const auto off = res::FindGeometryById(gp, id);
            const auto rec = off ? res::ReadGeometry(gp, *off) : std::nullopt;
            res::MeshData md;
            std::string derr;
            if (!rec || !res::DecodeGeometry(gp, *rec, md, &derr)) {
                error = std::format("graffiti stage {} geometry {:#x}: {}", i, id, derr);
                return false;
            }
            if (which == 1 && i == 0) {
                // Wall size from the paint layer itself (matches PHYS_DIMENSIONS to 0.2 %).
                half = {(md.bounds_max.y - md.bounds_min.y) * 0.5f, (md.bounds_max.z - md.bounds_min.z) * 0.5f};
            }
            if (which == 1) {
                for (Vec3& p : md.positions) p.x += 0.012f;  // paint sits on the wall: lift it off the surface
            }
            Layer& layer = which == 0 ? g.stencil : g.paint;
            if (renderer) {
                layer.mesh = renderer->UploadMesh(md, which == 0 ? "graffiti-stencil" : "graffiti-paint");
                if (const auto mt = res::ReadMaterialTextures(gp, *off); mt && mt->slots[1] && lib.Contains(mt->slots[1])) {
                    res::TextureData td;
                    if (lib.Decode(mt->slots[1], td, nullptr)) layer.texture = renderer->UploadTexture(td, /*srgb=*/true);
                }
            }
        }
        if (renderer) g.mask = renderer->CreateDynamicTexture(s.tuning.mask_width, s.tuning.mask_height, "graffiti-mask");
    }
    wall_.origin = s.chunk_origin + site->position_cm * 0.01f;
    wall_.rotation = site->rotation;
    wall_.half_extent = half;
    wall_matrix_ = RotationTranslation(wall_.rotation, wall_.origin);
    if (!graffiti_.Configure(graffiti_def_, wall_, s.tuning)) {
        error = "graffiti minigame configuration failed";
        return false;
    }
    if (renderer) {  // marker / cursor quad facing local +X, 1 m
        res::MeshData q;
        q.positions = {{0, -0.5f, -0.5f}, {0, 0.5f, -0.5f}, {0, 0.5f, 0.5f}, {0, -0.5f, 0.5f}};
        q.normals.assign(4, Vec3{1, 0, 0});
        q.uv0 = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        q.indices = {0, 1, 2, 2, 3, 0};
        marker_mesh_ = renderer->UploadMesh(q, "graffiti-marker");
    }
    log::Info("prologue", "graffiti {:#x}: {} stages ({} + {} markers), wall {:.2f} x {:.2f} m at ({:.2f}, {:.2f}, {:.2f})",
              graffiti_def_.oid, graffiti_def_.stages.size(), graffiti_def_.stages[0].markers_cm.size(),
              graffiti_def_.stages.size() > 1 ? graffiti_def_.stages[1].markers_cm.size() : 0, half.x * 2, half.y * 2,
              wall_.origin.x, wall_.origin.y, wall_.origin.z);
    return true;
}

bool PrologueSlice::Load(const PrologueSettings& s, Renderer* renderer, std::string& error) {
    settings_ = s;
    mission::MissionDef def;
    std::vector<std::string> errs;
    if (!mission::LoadMissionFile(s.mission_file, def, errs)) {
        error = std::format("mission file {}: {}", log::Narrow(s.mission_file), errs.empty() ? "?" : errs.front());
        return false;
    }
    res::KcapFile mfile;
    if (const auto e = mfile.Open(s.mission_package); e != res::KcapError::Ok) {
        error = std::format("mission package {}: {}", log::Narrow(s.mission_package), res::ToString(e));
        return false;
    }
    if (!res::LoadMissionPackage(mfile.Package(), mission_info_, &error)) return false;
    if (!LoadGraffiti(s, renderer, error)) return false;
    std::string terr;
    have_text_ = text_.Open(s.text_package, &terr);
    if (!have_text_) log::Warn("prologue", "texts not loaded ({}): keys are shown instead", terr);

    // The stencil-art use point (USE_BUTTON of a8, use radius 200 cm in its data). Its own placement
    // is relative to a parent that is not decoded: it is put at the wall, at chest height.
    Interactable use;
    use.id = kGraffitiInteraction;
    use.name = "graffiti_a8_family";
    use.position = graffiti_.WallPoint({0.5f, 0.25f}, 0.2f);
    use.radius = 2.0f;
    use.prompt_key = NameHash("u_button_graffiti");
    interactions_.AddInteractable(use);
    // Catwalk height: the HERO_MATCH_POINT nearest to the wall (Delsin's start, at the billboard's lower edge).
    catwalk_top_ = wall_.origin.z - wall_.half_extent.y;
    float best = 1e30f;
    for (const res::MissionPoint& p : mission_info_.hero_points) {
        const Vec3 m = p.position_cm * 0.01f;
        const float d = (m.x - wall_.origin.x) * (m.x - wall_.origin.x) + (m.y - wall_.origin.y) * (m.y - wall_.origin.y);
        if (d < best) {
            best = d;
            catwalk_top_ = m.z;
        }
    }
    if (renderer) {
        const std::vector<Vec3> tris = ApproximateCollision();
        res::MeshData md;
        md.positions = tris;
        md.indices.resize(tris.size());
        for (u32 k = 0; k < md.indices.size(); ++k) md.indices[k] = k;
        approx_mesh_ = renderer->UploadMesh(md, "approx-catwalk");
    }
    // NATIVE FALLBACK: the original "got off the roof" volumes lie on the cannery floor (z ~10 m), which
    // is part of the undecoded building; this box below the roof lets the next objective complete.
    TriggerVolume fallback;
    fallback.id = kRoofExitFallback;
    fallback.name = "vor_roof_exit_fallback";
    fallback.center = {wall_.origin.x - 10.0f, wall_.origin.y, 3.5f};
    fallback.half_extents = {35.0f, 35.0f, 7.5f};
    interactions_.AddVolume(fallback);
    // Trigger volumes of the mission (VOLUME_BUTTON_DATA): oriented boxes, scale read as half extents
    // in metres (the unit shape is not decoded - approximation).
    for (const res::MissionPoint& v : mission_info_.volumes) {
        TriggerVolume tv;
        tv.id = v.oid;
        tv.center = v.position_cm * 0.01f;
        tv.rotation = v.rotation;
        tv.half_extents = {std::max(0.3f, std::abs(v.scale.x)), std::max(0.3f, std::abs(v.scale.y)), std::max(0.3f, std::abs(v.scale.z))};
        interactions_.AddVolume(tv);
    }

    mission_id_ = def.id;
    if (!missions_.Add(std::move(def), &errs)) {
        error = errs.empty() ? "mission definition rejected" : errs.front();
        return false;
    }
    missions_.Attach(&bus_, this);
    // Interaction -> minigame (started in Update, where the live gameplay camera is known).
    bus_subscription_ = bus_.Subscribe(EventType::InteractionPressed, [this](const GameEvent& e) {
        if (e.subject == kGraffitiInteraction && !graffiti_.Active() && !graffiti_.Completed()) begin_graffiti_ = true;
    });
    loaded_ = true;
    log::Info("prologue", "mission {} ({} objectives) from {}, {} hero points, {} trigger volumes, {} texts",
              missions_.Find(mission_id_)->name, missions_.Find(mission_id_)->objectives.size(), log::Narrow(s.mission_package),
              mission_info_.hero_points.size(), mission_info_.volumes.size(), have_text_ ? text_.Count() : 0);
    return true;
}

void PrologueSlice::StartMission() {
    if (!loaded_) return;
    missions_.Start(mission_id_);
    bus_.Dispatch();
}

std::optional<HeroPlacement> PrologueSlice::TakeHeroPlacement() {
    auto p = pending_hero_;
    pending_hero_.reset();
    return p;
}

std::string PrologueSlice::Text(u64 key_hash) const {
    if (!have_text_) return {};
    const auto t = text_.Find(key_hash);
    return t ? PcButtons(t->text) : std::string{};
}

void PrologueSlice::Note(std::string text) {
    log::Info("mission", "{}", text);
    log_.push_back(std::format("{:7.2f}  {}", time_, text));
    while (log_.size() > kLogLines) log_.pop_front();
    note_time_ = 4.0f;
}

// --- IMissionHost --------------------------------------------------------------------------------

void PrologueSlice::ObjectiveChanged(const mission::MissionDef&, const mission::ObjectiveDef* objective) {
    objective_ = objective;
    if (objective) Note(std::format("objective: {}", objective->name));
}

void PrologueSlice::ShowHint(const mission::Action& a) { hint_ = Timed{a.id, a.text, a.seconds}; }

void PrologueSlice::ShowSubtitle(const mission::Action& a) { subtitle_ = Timed{a.id, a.text, a.seconds}; }

void PrologueSlice::SetInteractionEnabled(u64 id, bool enabled) {
    if (!interactions_.SetEnabled(id, enabled)) Note(std::format("no interaction {:#x}", id));
}

void PrologueSlice::PlaceHero(u64 point_id) {
    const res::MissionPoint* p = mission_info_.HeroPoint(point_id);
    if (!p) {
        Note(std::format("no HERO_MATCH_POINT {:#x}", point_id));
        return;
    }
    // Facing: the point's rotation about Z (x axis of the quaternion's basis).
    const Vec3 fwd = Rotate(p->rotation, Vec3{1, 0, 0});
    pending_hero_ = HeroPlacement{p->position_cm * 0.01f, std::atan2(fwd.y, fwd.x)};
}

void PrologueSlice::MissionLog(std::string_view text) { Note(std::string(text)); }

// --- Frame ---------------------------------------------------------------------------------------

void PrologueSlice::ScriptedInput(const PrologueFrameIn& in, bool& interact, GraffitiInput& g) {
    scripted_clock_ += in.dt;
    const mission::ObjectiveDef* o = missions_.Objective();
    if (!graffiti_.Active() && !graffiti_.Completed() && o && o->name == "tag_billboard" && missions_.ObjectiveTime() > 1.5f &&
        interactions_.Focused()) {
        interact = true;
        return;
    }
    if (graffiti_.Phase() != GraffitiPhase::Painting) return;
    // Spray the markers of the current stage one by one: glide to the next uncovered one, then
    // spray around it in a small circle.
    const int st = graffiti_.Stage();
    for (size_t m = 0; m < graffiti_.MarkerCount(st); ++m) {
        if (graffiti_.MarkerCovered(st, m)) continue;
        const Vec2 target = graffiti_.MarkerUv(st, m);
        const Vec2 cur = graffiti_.Cursor();
        const float dx = target.x - cur.x, dy = target.y - cur.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float step = 1.2f * in.dt;
        if (d > 0.02f) {
            graffiti_.SetCursor({cur.x + dx / d * std::min(step, d), cur.y + dy / d * std::min(step, d)});
            g.spray = d < 0.06f;
        } else {
            const float a = scripted_clock_ * 9.0f;
            graffiti_.SetCursor({target.x + 0.012f * std::cos(a), target.y + 0.02f * std::sin(a)});
            g.spray = true;
        }
        return;
    }
}

PrologueFrameOut PrologueSlice::Update(const PrologueFrameIn& in) {
    PrologueFrameOut out;
    if (!loaded_) return out;
    time_ += in.dt;
    bus_.SetTime(time_);
    const bool was_active = graffiti_.Active();
    bool interact = in.input && !in.scripted && in.input->Pressed(Action::Interact);
    GraffitiInput g;
    if (in.scripted) {
        ScriptedInput(in, interact, g);
    } else if (in.input && was_active) {
        if (in.look_enabled) {
            g.mouse_dx = in.look_dx;
            g.mouse_dy = in.look_dy;
        }
        const auto& pad = in.input->Gamepad();
        if (pad.connected) {
            g.stick_x = pad.left_x != 0 ? pad.left_x : pad.right_x;
            g.stick_y = pad.left_y != 0 ? pad.left_y : pad.right_y;
        }
        g.spray = in.input->Down(Action::Spray);
        g.cancel = in.input->Pressed(Action::Cancel);
    }
    interactions_.Update(in.player_feet, in.player_yaw, interact && !was_active, bus_);
    bus_.Dispatch();  // an interaction may request the minigame now
    if (begin_graffiti_) {
        begin_graffiti_ = false;
        graffiti_.Begin(in.gameplay_camera);
    }
    graffiti_.Update(in.dt, g, in.gameplay_camera, bus_);
    missions_.Update(in.dt);
    bus_.Dispatch();

    if (hint_ && (hint_->time_left -= in.dt) <= 0) hint_.reset();
    if (subtitle_ && (subtitle_->time_left -= in.dt) <= 0) subtitle_.reset();
    note_time_ = std::max(0.0f, note_time_ - in.dt);
    focused_prompt_ = interactions_.Focused() != nullptr && !graffiti_.Active();

    if (graffiti_.Active()) {
        out.lock_player = true;
        out.camera_override = true;
        out.camera = graffiti_.Camera(in.gameplay_camera);
        const Vec3 face = -graffiti_.WallNormal();
        out.face_yaw = std::atan2(face.y, face.x);
    }
    return out;
}

void PrologueSlice::UploadMasks(Renderer& renderer) {
    for (size_t i = 0; i < stages_.size(); ++i) {
        const u32 rev = graffiti_.MaskRevision(static_cast<int>(i));
        if (rev == stages_[i].uploaded_revision || stages_[i].mask == kWhiteTexture) continue;
        if (renderer.UpdateDynamicTexture(stages_[i].mask, graffiti_.Mask(static_cast<int>(i)))) stages_[i].uploaded_revision = rev;
    }
}

std::vector<Vec3> PrologueSlice::ApproximateCollision() const {
    std::vector<Vec3> tris;
    const float hx = wall_.half_extent.x, hy = wall_.half_extent.y;
    const float top = catwalk_top_ - wall_.origin.z;  // wall-local height (the wall only turns about Z)
    AddBox(tris, wall_matrix_, {0.0f, -hx - 0.4f, top - 0.25f}, {1.6f, hx + 0.4f, top});        // catwalk
    AddBox(tris, wall_matrix_, {-0.18f, -hx - 0.1f, -hy - 0.1f}, {-0.03f, hx + 0.1f, hy + 0.1f});  // billboard board
    return tris;
}

void PrologueSlice::AppendDraws(std::vector<DrawItem>& items) const {
    if (!loaded_ || stages_.empty()) return;
    if (approx_mesh_ != kInvalidMesh) items.push_back({approx_mesh_, Mat4::Identity(), {0.62f, 0.62f, 0.60f, 0}});
    const GraffitiPhase phase = graffiti_.Phase();
    const int stage = graffiti_.Stage();
    // Paint layers (back to front), each revealed by its own spray mask.
    for (size_t i = 0; i < stages_.size(); ++i) {
        const StageGpu& g = stages_[i];
        if (g.paint.mesh == kInvalidMesh) continue;
        const res::GraffitiStageDef& def = graffiti_def_.stages[i];
        DrawItem d;
        d.mesh = g.paint.mesh;
        d.world = wall_matrix_;
        d.texture = g.paint.texture;
        d.mask = g.mask;
        d.pass = DrawPass::Decal;
        d.mode = g.paint_mode;
        d.color = {def.color.x, def.color.y, def.color.z, 1};
        items.push_back(d);
    }
    const bool painting = phase == GraffitiPhase::Entering || phase == GraffitiPhase::Painting || phase == GraffitiPhase::Revealing;
    if (!painting || stage >= static_cast<int>(stages_.size())) return;
    // The cardboard stencil of the current stage, taken away while its paint is revealed.
    const StageGpu& g = stages_[static_cast<size_t>(stage)];
    if (g.stencil.mesh != kInvalidMesh) {
        DrawItem d;
        d.mesh = g.stencil.mesh;
        d.world = wall_matrix_;
        d.texture = g.stencil.texture;
        d.pass = DrawPass::Decal;
        d.mode = DecalMode::Board;
        d.color = {1, 1, 1, 1};
        d.opacity = phase == GraffitiPhase::Revealing ? 0.35f : 1.0f;  // native: the board lifts while the paint shows
        items.push_back(d);
    }
    if (phase != GraffitiPhase::Painting || marker_mesh_ == kInvalidMesh) return;
    // Spray targets of the stage (markers from the data) and the cursor.
    for (size_t m = 0; m < graffiti_.MarkerCount(stage); ++m) {
        const Vec3 p = graffiti_.WallPoint(graffiti_.MarkerUv(stage, m), 0.045f);
        DrawItem d;
        d.mesh = marker_mesh_;
        d.world = RotationTranslation(wall_.rotation, p) * Mat4::Scale(0.07f);
        d.pass = DrawPass::Decal;
        d.mode = DecalMode::Solid;
        const bool covered = graffiti_.MarkerCovered(stage, m);
        d.color = covered ? Vec4{0.25f, 0.9f, 0.35f, 1} : Vec4{1.0f, 0.75f, 0.1f, 1};
        d.opacity = covered ? 0.6f : 0.95f;
        items.push_back(d);
    }
    const Vec3 c = graffiti_.WallPoint(graffiti_.Cursor(), 0.05f);
    DrawItem ring;
    ring.mesh = marker_mesh_;
    ring.world = RotationTranslation(wall_.rotation, c) * Mat4::Scale(graffiti_.Tuning().spray_radius * 2.0f);
    ring.pass = DrawPass::Decal;
    ring.mode = DecalMode::Solid;
    ring.color = {1, 1, 1, 1};
    ring.opacity = 0.18f;
    items.push_back(ring);
    DrawItem dot = ring;
    dot.world = RotationTranslation(wall_.rotation, c + graffiti_.WallNormal() * 0.002f) * Mat4::Scale(0.035f);
    dot.opacity = 0.9f;
    items.push_back(dot);
}

void PrologueSlice::DrawHud(TextOverlay& o, bool debug) const {
    if (!loaded_ || !o.Ready()) return;
    const float W = o.ScreenWidth(), H = o.ScreenHeight();
    const float lh = o.LineHeight();
    auto text_or_key = [&](u64 key, std::string_view name) {
        std::string t = Text(key);
        return t.empty() ? std::string(name) : t;
    };
    // Objective (brief + mini note).
    if (objective_ && (objective_->brief_key || objective_->note_key)) {
        const std::string brief = objective_->brief_key ? text_or_key(objective_->brief_key, objective_->brief) : std::string{};
        const std::string note = objective_->note_key ? text_or_key(objective_->note_key, objective_->note) : std::string{};
        const float w = std::max(o.Measure(brief), o.Measure(note, 0.85f)) + 28;
        o.Rect(24, 24, w, (note.empty() ? 1.4f : 2.4f) * lh, Rgba(10, 12, 16, 170));
        o.Rect(24, 24, 4, (note.empty() ? 1.4f : 2.4f) * lh, Rgba(240, 190, 40, 255));
        o.Text(40, 24 + lh * 0.2f, brief, Rgba(255, 255, 255));
        if (!note.empty()) o.Text(40, 24 + lh * 1.2f, note, Rgba(200, 205, 210), 0.85f);
    }
    // Interaction prompt / minigame controls.
    if (focused_prompt_) {
        const Interactable* f = interactions_.Focused();
        const std::string label = std::format("[E / X]  {}", f ? text_or_key(f->prompt_key, "u_button_graffiti") : "Use");
        const float w = o.Measure(label) + 32;
        o.Rect((W - w) * 0.5f, H * 0.62f, w, lh * 1.5f, Rgba(10, 12, 16, 190));
        o.Text((W - w) * 0.5f + 16, H * 0.62f + lh * 0.25f, label, Rgba(255, 230, 120));
    }
    if (graffiti_.Active()) {
        const int st = graffiti_.Stage();
        const int n = graffiti_.StageCount();
        size_t covered = 0, total = 0;
        if (st < n) {
            total = graffiti_.MarkerCount(st);
            for (size_t m = 0; m < total; ++m) covered += graffiti_.MarkerCovered(st, m) ? 1 : 0;
        }
        const std::string l1 = Text(NameHash("u_title_graffiti")).empty() ? "Stencil Art" : Text(NameHash("u_title_graffiti"));
        const std::string l2 = std::format("Stage {}/{}   targets {}/{}   {}", std::min(st + 1, n), n, covered, total,
                                           ToString(graffiti_.Phase()));
        const std::string l3 = std::format("{}    [Mouse / stick] aim    [Q / B] leave",
                                           text_or_key(NameHash("u_brief_vor_graffiti_prompt"), "Spray-paint"));
        const float w = std::max({o.Measure(l1), o.Measure(l2), o.Measure(l3)}) + 32;
        const float y = H - lh * 4.4f - 24;
        o.Rect(24, y, w, lh * 3.6f, Rgba(10, 12, 16, 180));
        o.Text(40, y + lh * 0.2f, l1, Rgba(255, 230, 120));
        o.Text(40, y + lh * 1.2f, l2, Rgba(255, 255, 255));
        o.Text(40, y + lh * 2.2f, l3, Rgba(200, 205, 210));
        // Progress bar of the current stage.
        const float frac = total ? static_cast<float>(covered) / static_cast<float>(total) : 0.0f;
        o.Rect(40, y + lh * 3.25f, w - 32, 6, Rgba(60, 60, 60, 200));
        o.Rect(40, y + lh * 3.25f, (w - 32) * frac, 6, Rgba(90, 220, 110, 255));
    }
    // Hint (bottom left) and subtitle (bottom centre).
    if (hint_) {
        const std::string t = text_or_key(hint_->key, hint_->key_name);
        const float w = o.Measure(t) + 32;
        const float y = H - lh * 7.0f - 24;
        o.Rect(24, y, w, lh * 1.5f, Rgba(20, 60, 110, 190));
        o.Text(40, y + lh * 0.25f, t, Rgba(255, 255, 255));
    }
    if (subtitle_) {
        std::string t = text_or_key(subtitle_->key, subtitle_->key_name);
        const std::string sp = Speaker(subtitle_->key_name);
        // Wrap long narration at ~70% of the screen width.
        std::vector<std::string> lines;
        std::string cur;
        size_t pos = 0;
        while (pos < t.size()) {
            size_t sp_at = t.find(' ', pos);
            if (sp_at == std::string::npos) sp_at = t.size();
            const std::string word = t.substr(pos, sp_at - pos);
            const std::string cand = cur.empty() ? word : cur + " " + word;
            if (o.Measure(cand) > W * 0.7f && !cur.empty()) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = cand;
            }
            pos = sp_at + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
        if (!lines.empty()) lines.front() = sp + lines.front();
        float y = H * 0.80f - lh * static_cast<float>(lines.size());
        for (const auto& l : lines) {
            const float w = o.Measure(l);
            o.Rect((W - w) * 0.5f - 12, y - 2, w + 24, lh + 4, Rgba(0, 0, 0, 150));
            o.Text((W - w) * 0.5f, y, l, Rgba(255, 255, 255));
            y += lh + 4;
        }
    }
    // Last mission note (cutscenes skipped, objective changes).
    if (note_time_ > 0 && !log_.empty() && !debug) {
        const std::string& l = log_.back();
        o.Text(W - o.Measure(l, 0.75f) - 24, 24, l, Rgba(190, 195, 200, 220), 0.75f);
    }
    if (!debug) return;
    // Debug view: mission runtime, minigame, recent events.
    std::vector<std::string> lines = missions_.DebugLines();
    lines.push_back(std::format("graffiti {:#x}: {} stage {}/{} progress {:.0f}%  cursor ({:.2f}, {:.2f})", graffiti_def_.oid,
                                ToString(graffiti_.Phase()), std::min(graffiti_.Stage() + 1, graffiti_.StageCount()),
                                graffiti_.StageCount(), graffiti_.StageProgress() * 100, graffiti_.Cursor().x, graffiti_.Cursor().y));
    const Interactable* focus = interactions_.Focused();
    lines.push_back(std::format("focus: {}", focus ? focus->name : std::string("-")));
    lines.push_back("events:");
    const auto& hist = bus_.History();
    const size_t start = hist.size() > 12 ? hist.size() - 12 : 0;
    for (size_t i = start; i < hist.size(); ++i) {
        const GameEvent& e = hist[i];
        lines.push_back(std::format("  {:7.2f}  {:<22} {:#018x} {}", e.time, ToString(e.type), e.subject, e.param));
    }
    lines.push_back("log:");
    for (const auto& l : log_) lines.push_back("  " + l);
    float w = 0;
    for (const auto& l : lines) w = std::max(w, o.Measure(l, 0.72f));
    const float lh2 = o.LineHeight(0.72f);
    const float x = W - w - 40, y0 = 60;
    o.Rect(x - 12, y0 - 8, w + 28, lh2 * static_cast<float>(lines.size()) + 16, Rgba(5, 8, 12, 200));
    float y = y0;
    for (const auto& l : lines) {
        o.Text(x, y, l, l.starts_with("  ") ? Rgba(170, 220, 255) : Rgba(255, 255, 255), 0.72f);
        y += lh2;
    }
}

bool PrologueSlice::SaveGame(const std::wstring& path, const Vec3& feet, float yaw) const {
    SaveSections s;
    s[kTagMission] = missions_.Serialize();
    s[kTagGraffiti] = graffiti_.Serialize();
    s[kTagInteract] = interactions_.Serialize();
    ByteWriter p;
    p.Put(u32{1});
    p.Put(feet.x);
    p.Put(feet.y);
    p.Put(feet.z);
    p.Put(yaw);
    s[kTagPlayer] = p.Take();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) file::CreateDirectories(path.substr(0, slash));
    return WriteSave(path, s);
}

bool PrologueSlice::LoadGame(const std::wstring& path, std::string& error) {
    SaveSections s;
    if (!ReadSave(path, s, &error)) return false;
    for (u32 tag : {kTagMission, kTagGraffiti, kTagInteract, kTagPlayer}) {
        if (!s.contains(tag)) {
            error = "save file is missing a section";
            return false;
        }
    }
    ByteReader p(s[kTagPlayer]);
    u32 version = 0;
    HeroPlacement hero;
    p.Get(version);
    p.Get(hero.feet.x);
    p.Get(hero.feet.y);
    p.Get(hero.feet.z);
    p.Get(hero.yaw);
    if (!p.Ok() || version != 1 || !std::isfinite(hero.feet.x) || !std::isfinite(hero.feet.y) || !std::isfinite(hero.feet.z)) {
        error = "player section corrupted";
        return false;
    }
    // Validate everything before applying anything: decode into copies first.
    GraffitiMinigame g = graffiti_;
    InteractionSystem i = interactions_;
    if (!g.Deserialize(s[kTagGraffiti], &error) || !i.Deserialize(s[kTagInteract], &error)) return false;
    if (!missions_.Deserialize(s[kTagMission], &error)) return false;
    graffiti_ = std::move(g);
    interactions_ = std::move(i);
    interactions_.Update(hero.feet, hero.yaw, false, bus_, /*silent=*/true);
    hint_.reset();
    subtitle_.reset();
    pending_hero_ = hero;
    for (StageGpu& st : stages_) st.uploaded_revision = ~0u;
    Note("game loaded");
    return true;
}

}  // namespace sspc
