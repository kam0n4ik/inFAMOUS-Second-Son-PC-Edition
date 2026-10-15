// N12 tests against the real game packages (skipped when the game files are not installed):
// original object/script/text data, the stencil art of the prologue and the prologue fragment run
// headless (no GPU). Data location: SSPC_GAME_DIR, default E:\shdps4\CUSA00004.
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "core/file.h"
#include "core/log.h"
#include "core/name_hash.h"
#include "gameplay/prologue.h"
#include "resources/game_objects.h"
#include "resources/game_text.h"
#include "resources/graffiti_data.h"
#include "resources/kcap.h"
#include "resources/mission_data.h"
#include "resources/type_registry.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;

namespace {

std::wstring GameDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"SSPC_GAME_DIR", buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"E:\\shdps4\\CUSA00004";
}
std::wstring Cache(const wchar_t* name) { return GameDir() + L"\\art\\cache\\" + name; }

bool Have(const std::wstring& path) {
    if (file::Exists(path)) return true;
    test::Skip("game package not found: " + log::Narrow(path) + " (set SSPC_GAME_DIR)");
    return false;
}

bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

PrologueSettings Settings() {
    PrologueSettings s;
    s.mission_file = log::Widen(SSPC_SOURCE_DIR) + L"\\config\\missions\\vor_prologue.mission";
    s.mission_package = Cache(L"msn_vor.sprig.xpps");
    s.graffiti_package = Cache(L"graffiti_a8_family.xpps");
    s.graffiti_chunk = Cache(L"a8.xpps");
    s.text_package = Cache(L"lang_english_text.xpps");
    return s;
}

// Runs the slice headless with the scripted input until `done` or `seconds` of simulation.
template <class Done>
void RunSlice(PrologueSlice& p, Vec3& feet, float& yaw, float seconds, Done done) {
    for (int i = 0; i < static_cast<int>(seconds * 60) && !done(); ++i) {
        PrologueFrameIn in;
        in.dt = 1.0f / 60;
        in.player_feet = feet;
        in.player_yaw = yaw;
        in.gameplay_camera = {feet + Vec3{-3, 0, 1.6f}, {1, 0, 0}, 1.047f};
        in.scripted = true;
        const PrologueFrameOut out = p.Update(in);
        if (auto h = p.TakeHeroPlacement()) {
            feet = h->feet;
            yaw = h->yaw;
        }
        if (out.face_yaw) yaw = *out.face_yaw;
    }
}

}  // namespace

TEST(n12_name_hash_and_type_registry) {
    // Engine name hash, confirmed on the eboot dictionary: class ids are "cid_<CamelName>".
    CHECK_EQ(NameHash("cid_GraffitiMinigame"), 0x40e9fe2abbcac8e2ull);
    CHECK_EQ(ClassIdHash("GraffitiMinigame"), 0x40e9fe2abbcac8e2ull);
    CHECK_EQ(NameHash("AbC"), NameHash("abc"));
    const auto t = FindType(0xc458073763851767ull);
    CHECK(t && t->name == "MISSION_DATA" && t->index == 769);
    CHECK(TypeHash("GRAFFITI_MINIGAME_DATA") == 0x6fabfe95da448b74ull);
    CHECK(TypeCount() > 300);
}

TEST(n12_mission_package_vor) {
    const std::wstring path = Cache(L"msn_vor.sprig.xpps");
    if (!Have(path)) return;
    KcapFile f;
    CHECK_EQ(f.Open(path), KcapError::Ok);
    MissionPackageInfo info;
    std::string err;
    CHECK(LoadMissionPackage(f.Package(), info, &err));
    CHECK_EQ(info.objects, 1232u);
    CHECK_EQ(info.objectives.size(), 24u);
    CHECK_EQ(info.hero_points.size(), 10u);
    CHECK_EQ(info.volumes.size(), 58u);
    const MissionPoint* start = info.HeroPoint(0x21278fb67dc6d9a0ull);
    CHECK(start != nullptr);
    if (start) CHECK(Near(start->position_cm.z, 2360.0f, 5.0f));
    CHECK(info.Volume(0xdf15efbc896f3779ull) != nullptr);
    // Texts set by the scripts include the prologue briefs.
    bool roof = false;
    for (u64 k : info.text_keys) roof = roof || k == NameHash("u_brief_vor_escape_roof");
    CHECK(roof);
}

TEST(n12_every_mission_package_has_one_mission) {
    const std::wstring dir = GameDir() + L"\\art\\cache";
    if (!Have(Cache(L"msn_vor.sprig.xpps"))) return;
    int packages = 0, ok = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"msn_", 0) != 0 || e.path().extension() != L".xpps") continue;
        ++packages;
        KcapFile f;
        if (f.Open(e.path().wstring()) != KcapError::Ok) continue;
        MissionPackageInfo info;
        std::string err;
        if (f.Package().Header().kind == 0xA && LoadMissionPackage(f.Package(), info, &err)) ++ok;
    }
    CHECK_EQ(packages, 65);
    CHECK_EQ(ok, packages);
}

TEST(n12_graffiti_definition_a8_family) {
    const std::wstring path = Cache(L"graffiti_a8_family.xpps");
    if (!Have(path)) return;
    KcapFile f;
    CHECK_EQ(f.Open(path), KcapError::Ok);
    ObjectCatalog cat;
    std::string err;
    CHECK(cat.Build(f.Package(), &err));
    GraffitiDef d;
    CHECK(LoadGraffitiDefinition(f.Package(), cat, d, &err));
    CHECK_EQ(d.oid, 0x4a399b4da8529383ull);
    CHECK(Near(d.phys_dimensions_cm.x, 7.2f, 0.01f) && Near(d.phys_dimensions_cm.y, 691.3f, 0.01f) &&
          Near(d.phys_dimensions_cm.z, 360.8f, 0.01f));
    CHECK(d.has_camera);
    CHECK(Near(d.camera_pos_cm.x, 10636.7f, 0.1f) && Near(d.camera_pos_cm.y, -20132.5f, 0.1f) &&
          Near(d.camera_pos_cm.z, 2536.9f, 0.1f));
    CHECK(Near(d.camera_euler.y, -0.1214f, 1e-3f) && Near(d.camera_euler.z, -0.0159f, 1e-3f));
    CHECK(!d.camera_fov && !d.paint_size);
    CHECK_EQ(d.geometry_nopaint, 0x6d113b57d733ull);
    CHECK_EQ(d.stages.size(), 2u);
    if (d.stages.size() == 2) {
        CHECK_EQ(d.stages[0].stencil_geometry, 0x1174b60cb3b8459aull);
        CHECK_EQ(d.stages[0].paint_geometry, 0x6d113b57d734ull);
        CHECK_EQ(d.stages[0].reveal_geometry, 0xf6737289d61e08d6ull);
        CHECK_EQ(d.stages[1].reveal_geometry, kNoGeometry);
        CHECK(d.stages[0].color.x == 1 && d.stages[0].color.y == 1 && d.stages[0].color.z == 1);
        CHECK(d.stages[1].color.x == 0 && d.stages[1].color.y == 0 && d.stages[1].color.z == 0);
        CHECK_EQ(d.stages[0].markers_cm.size(), 14u);
        CHECK_EQ(d.stages[1].markers_cm.size(), 11u);
        for (const auto& st : d.stages) {
            for (const Vec3& m : st.markers_cm) CHECK(std::abs(m.y) < 346.4f && std::abs(m.z) < 180.5f);
        }
    }
}

TEST(n12_every_graffiti_package_loads) {
    const std::wstring dir = GameDir() + L"\\art\\cache";
    if (!Have(Cache(L"graffiti_a8_family.xpps"))) return;
    int packages = 0, ok = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"graffiti_", 0) != 0) continue;
        ++packages;
        KcapFile f;
        if (f.Open(e.path().wstring()) != KcapError::Ok) continue;
        ObjectCatalog cat;
        GraffitiDef d;
        std::string err;
        if (cat.Build(f.Package(), &err) && LoadGraffitiDefinition(f.Package(), cat, d, &err) && !d.stages.empty() && d.has_camera &&
            std::isfinite(d.camera_pos_cm.x)) {
            ++ok;
        } else {
            std::printf("    %s: %s\n", log::Narrow(name).c_str(), err.c_str());
        }
    }
    std::printf("    %d/%d graffiti packages decoded\n", ok, packages);
    CHECK_EQ(packages, 59);
    CHECK_EQ(ok, packages);
}

TEST(n12_graffiti_placement_matches_original_camera) {
    if (!Have(Cache(L"a8.xpps")) || !Have(Cache(L"graffiti_a8_family.xpps"))) return;
    KcapFile chunk, g;
    CHECK_EQ(chunk.Open(Cache(L"a8.xpps")), KcapError::Ok);
    CHECK_EQ(g.Open(Cache(L"graffiti_a8_family.xpps")), KcapError::Ok);
    ObjectCatalog ccat, gcat;
    std::string err;
    CHECK(ccat.Build(chunk.Package(), &err) && gcat.Build(g.Package(), &err));
    GraffitiDef d;
    CHECK(LoadGraffitiDefinition(g.Package(), gcat, d, &err));
    const auto site = FindGraffitiSite(ccat, d.oid);
    CHECK(site.has_value());
    if (!site) return;
    CHECK(Near(site->position_cm.x, 1119.9f, 0.1f) && Near(site->position_cm.y, -141.2f, 0.1f) &&
          Near(site->position_cm.z, 2541.4f, 0.1f));
    const Vec3 normal = Rotate(site->rotation, Vec3{1, 0, 0});
    CHECK(Near(normal.x, -1.0f, 1e-4f));  // the wall faces -X
    // Chunk-frame origin (100, -200, 0) m: the camera (world) looks at the wall centre (chunk frame).
    const Vec3 centre = Vec3{100, -200, 0} + site->position_cm * 0.01f;
    const Vec3 eye = d.camera_pos_cm * 0.01f;
    const float yaw = d.camera_euler.z;
    const float t = (centre.x - eye.x) / std::cos(yaw);  // ray to the wall plane x = centre.x
    const float hit_y = eye.y + t * std::sin(yaw);
    CHECK(t > 4.0f && t < 6.0f);
    CHECK(Near(hit_y, centre.y, 0.02f));     // aimed at the centre to within 2 cm horizontally
    CHECK(Near(eye.z, centre.z, 0.06f));     // at the centre's height to within 6 cm
    // Delsin's start point stands on the billboard's lower edge.
    KcapFile m;
    CHECK_EQ(m.Open(Cache(L"msn_vor.sprig.xpps")), KcapError::Ok);
    MissionPackageInfo info;
    CHECK(LoadMissionPackage(m.Package(), info, &err));
    const MissionPoint* start = info.HeroPoint(0x21278fb67dc6d9a0ull);
    CHECK(start != nullptr);
    if (start) CHECK(Near(start->position_cm.z * 0.01f, centre.z - d.phys_dimensions_cm.z * 0.005f, 0.03f));
}

TEST(n12_game_text) {
    const std::wstring path = Cache(L"lang_english_text.xpps");
    if (!Have(path)) return;
    GameText t;
    std::string err;
    CHECK(t.Open(path, &err));
    CHECK(t.Count() > 12000);
    const auto roof = t.Find("u_brief_vor_escape_roof");
    CHECK(roof && roof->text == "Find a safe way down off the fish cannery roof");
    const auto line = t.Find("m_vor_ohnocop_02_hro");
    CHECK(line && line->text == "Okay, think Delsin, think.");
    const auto g3 = t.Find("m_vor_graffiti_03_hro");
    CHECK(g3 && g3->text == "You always remember your first painted lady.");
    CHECK(!t.Find("no_such_key_n12"));
}

TEST(n12_prologue_fragment_headless) {
    if (!Have(Cache(L"msn_vor.sprig.xpps")) || !Have(Cache(L"a8.xpps"))) return;
    PrologueSlice p;
    std::string err;
    CHECK(p.Load(Settings(), nullptr, err));
    if (!p.Loaded()) {
        std::printf("    %s\n", err.c_str());
        return;
    }
    p.StartMission();
    Vec3 feet;
    float yaw = 0;
    if (auto h = p.TakeHeroPlacement()) {
        feet = h->feet;
        yaw = h->yaw;
    }
    CHECK(Near(feet.x, 109.8f, 0.05f) && Near(feet.z, 23.6f, 0.05f));
    CHECK(p.Missions().Objective() && p.Missions().Objective()->name == "intro");
    auto objective = [&]() { return p.Missions().Objective() ? p.Missions().Objective()->name : std::string(); };
    RunSlice(p, feet, yaw, 15, [&] { return objective() == "tag_billboard"; });
    CHECK_EQ(objective(), std::string("tag_billboard"));
    // The scripted player uses the stencil art and sprays every marker of both stages.
    RunSlice(p, feet, yaw, 60, [&] { return objective() == "escape_roof"; });
    CHECK(p.Graffiti().Completed());
    CHECK_EQ(objective(), std::string("escape_roof"));
    CHECK_EQ(p.Missions().Xp(), 100);
    // Getting down off the roof (the native fallback volume below it) gives the next objective.
    Vec3 ground{p.Wall().origin.x - 8.0f, p.Wall().origin.y, 1.0f};
    RunSlice(p, ground, yaw, 2, [&] { return objective() == "escape_cannery"; });
    CHECK_EQ(objective(), std::string("escape_cannery"));
    CHECK_EQ(p.Missions().Xp(), 150);
}

TEST(n12_prologue_save_load_mid_graffiti) {
    if (!Have(Cache(L"msn_vor.sprig.xpps")) || !Have(Cache(L"a8.xpps"))) return;
    PrologueSlice a;
    std::string err;
    CHECK(a.Load(Settings(), nullptr, err));
    if (!a.Loaded()) return;
    a.StartMission();
    Vec3 feet;
    float yaw = 0;
    if (auto h = a.TakeHeroPlacement()) {
        feet = h->feet;
        yaw = h->yaw;
    }
    // Into stage 2 of the stencil art.
    RunSlice(a, feet, yaw, 60, [&] { return a.Graffiti().Stage() == 1 && a.Graffiti().Phase() == GraffitiPhase::Painting; });
    CHECK_EQ(a.Graffiti().Stage(), 1);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"sspc_n12_test.sav";
    CHECK(a.SaveGame(path, feet, yaw));
    PrologueSlice b;
    CHECK(b.Load(Settings(), nullptr, err));
    CHECK(b.LoadGame(path, err));
    CHECK_EQ(b.Graffiti().Stage(), 1);
    CHECK(b.Graffiti().Phase() == GraffitiPhase::Idle);  // the session restarts on interaction
    CHECK(b.Graffiti().Mask(0) == a.Graffiti().Mask(0));
    CHECK(b.Missions().Objective() && b.Missions().Objective()->name == "tag_billboard");
    auto hb = b.TakeHeroPlacement();
    CHECK(hb && Near(hb->feet.x, feet.x, 1e-4f));
    // Finish after loading: exactly one completion, the reward once.
    Vec3 fb = hb ? hb->feet : feet;
    float yb = hb ? hb->yaw : yaw;
    RunSlice(b, fb, yb, 60, [&] { return b.Missions().Objective() && b.Missions().Objective()->name == "escape_roof"; });
    CHECK(b.Graffiti().Completed());
    CHECK_EQ(b.Missions().Xp(), 100);
    // A corrupted save is rejected and changes nothing.
    std::vector<u8> bytes;
    CHECK(file::ReadAll(path, bytes));
    bytes[bytes.size() / 2] ^= 0x10;
    CHECK(file::WriteAtomic(path, bytes));
    CHECK(!b.LoadGame(path, err));
    CHECK(b.Graffiti().Completed());
    DeleteFileW(path.c_str());
}
