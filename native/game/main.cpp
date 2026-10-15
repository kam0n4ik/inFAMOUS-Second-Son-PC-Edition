// SecondSonPC native runtime - entry point (SecondSonPC.exe).
//
// Vertical slice: original Delsin model geometry decoded from the game's own package by the
// native resource loader, drawn by the native Vulkan renderer, controlled by a native
// fixed-timestep game loop. No PS4 code is loaded or executed and no shadPS4 component exists in
// this program.
//
// Command line:
//   --config <file.ini>     configuration (default: SecondSonPC.ini next to the exe)
//   --frames <n>            exit after n rendered frames (automated runs)
//   --screenshot <file.bmp> capture the last frame of an automated run
//   --validation            enable the Vulkan validation layer
//   --package <file.xpps>   character package override
//   --modules <file.txt>    write the list of loaded DLLs at exit (dependency audit)
//   --autoplay              scripted input for automated runs (walk, turn, jump)
//   --chunk <path.xpps>     world chunk override (relative to [Game] DataDir)
//   --anim-clip <hex>       clip viewer: loop the clip at this data offset of [Animation] ClipPackage
//   --anim-phase <0..1>     clip viewer: hold this phase (screenshots of one frame)
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/clock.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/file.h"
#include "core/jobs.h"
#include "core/log.h"
#include "gameplay/character.h"
#include "gameplay/game_loop.h"
#include "platform/input.h"
#include "platform/window.h"
#include "renderer/renderer.h"
#include "resources/anim_clip.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "scene/character_model.h"
#include "scene/primitives.h"
#include "scene/test_pose.h"
#include "scene/world_scene.h"

using namespace sspc;

namespace {

struct Args {
    std::wstring config;
    long long frames = -1;
    std::wstring screenshot;
    bool validation = false;
    std::wstring package;
    std::wstring modules;
    bool autoplay = false;
    std::string chunk;
    long long anim_clip = -1;
    float anim_phase = -1;
    float camera_yaw_deg = 1e9f;  // unset
};

Args ParseArgs() {
    Args a;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring s = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (s == L"--config") a.config = next();
        else if (s == L"--frames") a.frames = _wtoll(next().c_str());
        else if (s == L"--screenshot") a.screenshot = next();
        else if (s == L"--validation") a.validation = true;
        else if (s == L"--package") a.package = next();
        else if (s == L"--modules") a.modules = next();
        else if (s == L"--autoplay") a.autoplay = true;
        else if (s == L"--chunk") a.chunk = log::Narrow(next());
        else if (s == L"--anim-clip") a.anim_clip = std::wcstoll(next().c_str(), nullptr, 16);
        else if (s == L"--anim-phase") a.anim_phase = static_cast<float>(_wtof(next().c_str()));
        else if (s == L"--camera-yaw") a.camera_yaw_deg = static_cast<float>(_wtof(next().c_str()));
    }
    LocalFree(argv);
    return a;
}

std::wstring Join(const std::wstring& dir, const std::wstring& rel) {
    if (rel.size() > 1 && (rel[1] == L':' || rel[0] == L'\\' || rel[0] == L'/')) return rel;
    return dir + L"\\" + rel;
}

// Comma-separated package list from the config, resolved against the data directory.
std::vector<std::wstring> PackageList(const std::string& list, const std::wstring& data_dir) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(pos, end - pos);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(Join(data_dir, log::Widen(item)));
        pos = end + 1;
    }
    return out;
}

void WriteLoadedModules(const std::wstring& path) {
    HMODULE mods[1024];
    DWORD needed = 0;
    std::string text;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
            wchar_t name[MAX_PATH];
            if (GetModuleFileNameW(mods[i], name, MAX_PATH)) text += log::Narrow(name) + "\n";
        }
    }
    file::WriteAtomic(path, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

// Original animation clips named in [Animation] (data offsets in ClipPackage). The game stores no
// clip names: the roles were identified by motion analysis (docs/native/N11_FIRST_PLAYABLE_REPORT.md).
struct ClipSet {
    std::deque<res::AnimClip> storage;  // stable addresses
    AnimatorClips clips;
    const res::AnimClip* preview = nullptr;
    u32 loaded = 0;
};

ClipSet LoadClips(const Config& cfg, const std::wstring& data_dir, const res::Skeleton& skeleton, long long preview_offset) {
    ClipSet set;
    const std::string pkg = cfg.GetString("Animation", "ClipPackage", "");
    if (pkg.empty()) return set;
    const Clock clock;
    res::KcapFile file;
    const std::wstring path = data_dir + L"\\" + log::Widen(pkg);
    if (const auto e = file.Open(path); e != res::KcapError::Ok) {
        log::Warn("anim", "clip package {}: {}", log::Narrow(path), res::ToString(e));
        return set;
    }
    auto load = [&](u64 offset, const char* what) -> const res::AnimClip* {
        std::string err;
        auto c = res::AnimClip::Parse(file.Package().Data(), static_cast<u32>(offset), &err);
        if (!c) {
            log::Warn("anim", "{} clip {:#x}: {}", what, offset, err);
            return nullptr;
        }
        if (c->SkeletonJoints() > skeleton.JointCount()) {
            log::Warn("anim", "{} clip {:#x} targets {} joints, the skeleton has {}", what, offset, c->SkeletonJoints(),
                      skeleton.JointCount());
            return nullptr;
        }
        set.storage.push_back(std::move(*c));
        ++set.loaded;
        const res::AnimClip* p = &set.storage.back();
        log::Info("anim", "{} = clip {:#x}: {} keys ({:.2f} s), {} segments, {} rotation / {} translation tracks", what,
                  offset, p->KeyCount(), p->DurationSeconds(), p->SegmentCount(), p->RotationTracks(),
                  p->TranslationTracks());
        return p;
    };
    auto offset_of = [&](const char* key) -> std::optional<u64> {
        const std::string v = cfg.GetString("Animation", key, "");
        if (v.empty()) return std::nullopt;
        return std::strtoull(v.c_str(), nullptr, 16);
    };
    const res::AnimClip** slots[] = {&set.clips.idle, &set.clips.walk, &set.clips.run,
                                     &set.clips.jump_start, &set.clips.fall, &set.clips.land};
    const char* keys[] = {"Idle", "Walk", "Run", "JumpStart", "Fall", "Land"};
    for (size_t i = 0; i < 6; ++i) {
        if (const auto o = offset_of(keys[i])) *slots[i] = load(*o, keys[i]);
    }
    set.clips.walk_speed = static_cast<float>(cfg.GetFloat("Animation", "WalkSpeed", set.clips.walk_speed));
    set.clips.run_speed = static_cast<float>(cfg.GetFloat("Animation", "RunSpeed", set.clips.run_speed));
    if (preview_offset >= 0) set.preview = load(static_cast<u64>(preview_offset), "preview");
    log::Info("anim", "{} clips from {} in {:.0f} ms", set.loaded, log::Narrow(path), clock.Now() * 1000);
    return set;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const Args args = ParseArgs();
    const std::wstring exe_dir = file::ExecutableDirectory();
    file::CreateDirectories(exe_dir + L"\\logs");
    log::Init(exe_dir + L"\\logs\\native.log", log::Level::Info, false);
    crash::Install(exe_dir + L"\\logs");

    Config cfg;
    const std::wstring config_path = args.config.empty() ? exe_dir + L"\\SecondSonPC.ini" : args.config;
    if (!cfg.LoadFile(config_path)) log::Warn("app", "config {} not loaded; using defaults", log::Narrow(config_path));
    for (const auto& e : cfg.Errors()) log::Warn("config", "{}", e);
    if (cfg.GetString("Log", "Level", "info") == "debug") log::SetLevel(log::Level::Debug);
    log::Info("app", "SecondSonPC native runtime starting (exe dir {})", log::Narrow(exe_dir));

    Input input;
    input.ApplyConfig(cfg);
    Window window;
    WindowDesc wd;
    wd.width = static_cast<u32>(cfg.GetInt("Render", "Width", 1600));
    wd.height = static_cast<u32>(cfg.GetInt("Render", "Height", 900));
    wd.fullscreen = cfg.GetBool("Render", "Borderless", false);
    if (!window.Create(wd, &input)) return 1;

    Renderer renderer;
    RendererDesc rd;
    rd.validation = args.validation || cfg.GetBool("Render", "Validation", false);
    rd.vsync = cfg.GetBool("Render", "VSync", true);
    rd.cull_back_faces = cfg.GetBool("Render", "CullBackFaces", true);
    file::CreateDirectories(exe_dir + L"\\cache");
    rd.pipeline_cache_path = exe_dir + L"\\cache\\pipelines.bin";
    if (!renderer.Init(window, rd)) {
        MessageBoxW(nullptr, L"Vulkan 1.3 initialisation failed. See logs\\native.log.", L"SecondSonPC", MB_ICONERROR);
        return 2;
    }

    // --- Content -----------------------------------------------------------------------------
    JobSystem jobs;
    const std::wstring data_dir = log::Widen(cfg.GetString("Game", "DataDir", ""));
    const std::wstring package = args.package.empty()
                                     ? Join(data_dir, log::Widen(cfg.GetString("Game", "CharacterPackage", "art\\cache\\hero.xpps")))
                                     : args.package;
    const std::wstring cache = cfg.GetBool("Game", "UseConvertedCache", true) ? exe_dir + L"\\cache\\character.spcmesh" : L"";
    LoadedModel model;
    std::string load_error;
    std::vector<MeshHandle> part_handles;
    std::vector<TextureHandle> part_textures;
    std::vector<bool> part_visible;
    const auto char_textures = PackageList(cfg.GetString("Character", "TexturePackages", ""), data_dir);
    const bool have_model = LoadCharacterModel(package, cache, char_textures, jobs, model, load_error);
    if (have_model) {
        // Parts hidden by configuration (source offsets in the package, hex). The game picks
        // clothing/accessory variants from instance flags that are not reverse-engineered yet.
        std::vector<u32> hide;
        const std::string hide_list = cfg.GetString("Character", "HideParts", "");
        for (size_t pos = 0; pos < hide_list.size();) {
            size_t end = hide_list.find(',', pos);
            if (end == std::string::npos) end = hide_list.size();
            hide.push_back(static_cast<u32>(std::strtoul(hide_list.substr(pos, end - pos).c_str(), nullptr, 16)));
            pos = end + 1;
        }
        std::unordered_map<u64, TextureHandle> tex_by_id;
        for (const auto& t : model.textures) tex_by_id[t.id] = renderer.UploadTexture(t, /*srgb=*/true);
        for (size_t i = 0; i < model.meshes.size(); ++i) {
            const auto it = tex_by_id.find(model.part_albedo[i]);
            part_textures.push_back(it != tex_by_id.end() ? it->second : kWhiteTexture);
            const std::string name = std::format("part{}@{:#x}", i, model.parts[i].source_offset);
            part_handles.push_back(renderer.UploadMesh(model.meshes[i], name.c_str()));
            const bool hidden = std::find(hide.begin(), hide.end(), model.parts[i].source_offset) != hide.end();
            part_visible.push_back(model.parts[i].default_visible && !hidden);
        }
        size_t shown = 0;
        u64 tris = 0;
        for (size_t i = 0; i < model.parts.size(); ++i) {
            if (part_visible[i]) {
                ++shown;
                tris += model.parts[i].triangle_count;
            }
        }
        log::Info("app", "character: {} parts ({} shown, {} triangles), source {}", model.parts.size(), shown, tris,
                  model.from_cache ? "spcmesh cache" : "KCAP package");
        model.meshes.clear();  // GPU copies are uploaded; free the CPU side
        model.meshes.shrink_to_fit();
    } else {
        log::Error("app", "character model not loaded: {}", load_error);
        log::Error("app", "set [Game] DataDir / CharacterPackage in SecondSonPC.ini (game files are not shipped)");
    }

    // World: a chunk of the original environment, or the test ground with crates.
    WorldScene world;
    bool have_world = false;
    const std::string chunk = args.chunk.empty() ? cfg.GetString("World", "ChunkPackage", "") : args.chunk;
    if (!chunk.empty()) {
        std::string world_error;
        const auto world_textures = PackageList(cfg.GetString("World", "TexturePackages", ""), data_dir);
        have_world = LoadWorldChunk(Join(data_dir, log::Widen(chunk)), world_textures, renderer, jobs, world, world_error);
        if (!have_world) log::Error("app", "world chunk not loaded: {} (using the test ground)", world_error);
    }
    MeshHandle h_even = kInvalidMesh, h_odd = kInvalidMesh;
    std::vector<Aabb> obstacles;
    std::vector<MeshHandle> obstacle_meshes;
    if (!have_world) {
        res::MeshData ground_even, ground_odd;
        MakeGroundGrid(40, 1.0f, ground_even, ground_odd);
        h_even = renderer.UploadMesh(ground_even, "ground-even");
        h_odd = renderer.UploadMesh(ground_odd, "ground-odd");
        obstacles = {
            {{3, -1, 0}, {4.5f, 1, 0.5f}}, {{5, 2, 0}, {6, 3, 1.0f}},
            {{-4, 3, 0}, {-2, 5, 0.3f}},   {{-6, -5, 0}, {-5, -2, 2.0f}},
        };
        for (const Aabb& b : obstacles) obstacle_meshes.push_back(renderer.UploadMesh(MakeBox(b.min, b.max), "crate"));
    }

    // Meshes are decoded in metres with the model origin at the feet (game centimetres / 100).
    const float model_scale = static_cast<float>(cfg.GetFloat("Character", "ModelScale", 1.0));

    // --- Loop --------------------------------------------------------------------------------
    CharacterParams params;
    params.walk_speed = static_cast<float>(cfg.GetFloat("Character", "WalkSpeed", params.walk_speed));
    params.run_speed = static_cast<float>(cfg.GetFloat("Character", "RunSpeed", params.run_speed));
    params.jump_speed = static_cast<float>(cfg.GetFloat("Character", "JumpSpeed", params.jump_speed));
    params.gravity = static_cast<float>(cfg.GetFloat("Character", "Gravity", params.gravity));
    params.ground_plane = !have_world;
    CharacterState player;
    if (have_world) player.position = world.spawn;
    CharacterState prev_player = player;
    const float kill_z = have_world ? world.terrain.BoundsMin().z - 30.0f : -100.0f;
    OrbitCamera orbit;
    orbit.sensitivity = static_cast<float>(cfg.GetFloat("Input", "MouseSensitivity", orbit.sensitivity));
    if (args.camera_yaw_deg < 1e8f) orbit.yaw = args.camera_yaw_deg * kPi / 180.0f;
    FreeCamera free_cam;
    bool free_mode = false;
    FixedStepClock sim(1.0 / static_cast<double>(cfg.GetInt("Game", "SimulationHz", 60)));
    // Original animation clips of the game on the character skeleton ([Animation]); without them the
    // procedural TEST pose (arms down + walk cycle, not the game's animation) exercises skinning.
    const bool can_animate = have_model && model.skeleton && !model.skeleton->rest.empty();
    ClipSet clip_set = can_animate ? LoadClips(cfg, data_dir, *model.skeleton, args.anim_clip) : ClipSet{};
    CharacterAnimator animator;
    if (clip_set.loaded > 0 &&
        animator.Init(*model.skeleton, clip_set.clips, static_cast<float>(cfg.GetFloat("Animation", "BlendSeconds", 0.2)))) {
        if (clip_set.preview) animator.Preview(clip_set.preview);
        animator.SetJointDrivers(model.joint_driver);
        // Locomotion speeds come from the clips' own foot plants so the feet do not slide.
        params.walk_speed = clip_set.clips.walk_speed;
        params.run_speed = clip_set.clips.run_speed;
        log::Info("app", "character animation: original clips (walk {:.2f} m/s, run {:.2f} m/s)", params.walk_speed,
                  params.run_speed);
    }
    TestPose test_pose;
    if (!animator.Valid() && have_model && model.skeleton && cfg.GetBool("Character", "TestAnimation", true)) {
        test_pose.Init(*model.skeleton, TestPoseJoints{});
        log::Warn("app", "no animation clips loaded: procedural TEST pose");
    }
    float walk_phase = 0;
    // Performance record (after a short warm-up).
    std::vector<float> frame_ms_log, cpu_ms_log, gpu_ms_log, anim_ms_log;
    frame_ms_log.reserve(1 << 16);
    size_t selected_part = 0;
    bool pending_jump = false;
    window.SetMouseCaptured(args.frames < 0 && cfg.GetBool("Input", "CaptureMouse", true));

    const Clock clock;
    double last = clock.Now();
    double title_time = last;
    u64 frame = 0;
    u32 title_frames = 0;
    double frame_ms_acc = 0;
    while (window.PumpMessages()) {
        const double now = clock.Now();
        const double dt = std::min(now - last, 0.25);
        last = now;
        input.BeginFrame();
        if (input.Pressed(Action::Quit)) {
            if (window.MouseCaptured()) window.SetMouseCaptured(false);
            else break;
        }
        if (input.Pressed(Action::ToggleMouseCapture) || (!window.MouseCaptured() && input.MouseButton(0) && args.frames < 0)) {
            window.SetMouseCaptured(!window.MouseCaptured() || input.MouseButton(0));
        }
        if (input.Pressed(Action::ToggleFreeCamera)) {
            free_mode = !free_mode;
            if (free_mode) {
                free_cam.position = orbit.Eye();
                free_cam.yaw = orbit.ForwardYaw();
            }
        }
        if (!part_handles.empty()) {
            if (input.Pressed(Action::NextPart)) selected_part = (selected_part + 1) % part_handles.size();
            if (input.Pressed(Action::PrevPart)) selected_part = (selected_part + part_handles.size() - 1) % part_handles.size();
            if (input.Pressed(Action::TogglePart)) part_visible[selected_part] = !part_visible[selected_part];
        }
        float look_x = 0, look_y = 0;
        input.LookDelta(look_x, look_y, static_cast<float>(dt));
        if (!window.MouseCaptured() && !input.Gamepad().connected) look_x = look_y = 0;
        float mx = 0, my = 0;
        input.MoveAxis(mx, my);
        // With the game's clips a full stick runs (as in the game) and Walk holds the walk;
        // the test pose keeps the old walk / Sprint-to-run mapping.
        bool sprint = animator.Valid() ? !input.Down(Action::Walk) : input.Down(Action::Sprint);
        if (input.Pressed(Action::Jump)) pending_jump = true;
        if (args.autoplay) {  // deterministic scripted input for automated runs
            const double t = sim.TotalSteps() * sim.Step();
            my = t < 4.0 ? 1.0f : 0.0f;
            mx = (t > 1.5 && t < 2.5) ? 0.6f : 0.0f;
            sprint = t > 2.0;
            if (std::abs(t - 3.0) < 0.5 * sim.Step()) pending_jump = true;
            look_x = static_cast<float>(dt) * 120.0f;
        }
        if (free_mode) {
            free_cam.AddLook(look_x, look_y);
            free_cam.Move(mx, my, input.Down(Action::Jump) ? 1.0f : 0.0f, static_cast<float>(dt), sprint);
        } else {
            orbit.AddLook(look_x, look_y);
        }

        const u32 steps = sim.Advance(dt);
        bool jumped = false, landed = false;
        for (u32 s = 0; s < steps; ++s) {
            prev_player = player;
            MoveInput mi;
            if (!free_mode && !clip_set.preview) {
                mi.x = mx;
                mi.y = my;
                mi.sprint = sprint;
                mi.jump = pending_jump;
            }
            mi.camera_yaw = orbit.ForwardYaw();
            const bool was_grounded = player.grounded;
            StepCharacter(player, params, mi, static_cast<float>(sim.Step()), obstacles,
                          have_world ? &world.terrain : nullptr);
            jumped |= mi.jump && was_grounded && !player.grounded;
            landed |= player.landed_this_step;
            pending_jump = false;
            if (player.position.z < kill_z) {  // fell through a hole in the walkable surfaces: respawn
                player = CharacterState{};
                player.position = have_world ? world.spawn : Vec3{0, 0, 0};
                prev_player = player;
                log::Info("app", "respawned (fell below {:.1f} m)", kill_z);
            }
        }
        const float alpha = static_cast<float>(sim.Alpha());
        const float hspeed = std::sqrt(player.velocity.x * player.velocity.x + player.velocity.y * player.velocity.y);
        const std::vector<Mat4>* palette = nullptr;  // current skinning matrices (rigid parts ride on them)
        if (animator.Valid()) {
            const AnimatorInput ai{hspeed, player.grounded, jumped, landed};
            if (args.anim_phase >= 0) {  // clip viewer: hold one frame
                animator.SetPhase(args.anim_phase);
                animator.Update(0.0f, ai);
            } else {
                animator.Update(static_cast<float>(dt), ai);
            }
            renderer.SetSkinPalette(animator.Palette());
            palette = &animator.Palette();
        } else if (test_pose.Valid()) {
            walk_phase += hspeed * static_cast<float>(dt) * (2.0f * kPi / 1.6f);  // ~1.6 m per stride cycle
            palette = &test_pose.Evaluate(walk_phase, std::min(1.0f, hspeed / 3.0f));
            renderer.SetSkinPalette(*palette);
        }
        const Vec3 render_pos = Lerp(prev_player.position, player.position, alpha);
        orbit.target = render_pos + Vec3{0, 0, 1.45f};

        std::vector<DrawItem> items;
        if (have_world) {
            items.insert(items.end(), world.draws.begin(), world.draws.end());
        } else {
            items.push_back({h_even, Mat4::Identity(), {0.46f, 0.47f, 0.45f, 0}});
            items.push_back({h_odd, Mat4::Identity(), {0.38f, 0.39f, 0.37f, 0}});
            for (MeshHandle h : obstacle_meshes) items.push_back({h, Mat4::Identity(), {0.55f, 0.40f, 0.26f, 0}});
        }
        const Mat4 hero_world = Mat4::Translation(render_pos) * Mat4::RotationZ(player.yaw) * Mat4::Scale(model_scale);
        for (size_t i = 0; i < part_handles.size(); ++i) {
            if (!part_visible[i] || part_handles[i] == kInvalidMesh) continue;
            const bool textured = part_textures[i] != kWhiteTexture;
            // Parts whose colour map is in a package not loaded yet are drawn in neutral clay.
            Vec4 c = textured ? Vec4{1, 1, 1, 0} : Vec4{0.48f, 0.47f, 0.45f, 0};
            if (i == selected_part && window.MouseCaptured() == false) c = {0.95f, 0.55f, 0.20f, 0};
            Mat4 part_world = hero_world;
            if (palette && i < model.part_attach.size() && model.part_attach[i] >= 0 &&
                static_cast<size_t>(model.part_attach[i]) < palette->size()) {
                part_world = hero_world * (*palette)[static_cast<size_t>(model.part_attach[i])];
            }
            items.push_back({part_handles[i], part_world, c, part_textures[i]});
        }
        FrameView fv;
        fv.view = free_mode ? free_cam.View() : orbit.View();
        fv.projection = Mat4::Perspective(60.0f * kPi / 180.0f, renderer.AspectRatio(), 0.1f, 2000.0f);
        if (args.frames >= 0 && frame + 1 == static_cast<u64>(args.frames) && !args.screenshot.empty()) {
            renderer.RequestScreenshot(args.screenshot);
        }
        const double cpu_ms = (clock.Now() - now) * 1000.0;  // input, simulation, animation, draw list
        if (!renderer.RenderFrame(fv, items)) {
            log::Error("app", "rendering failed; exiting");
            break;
        }
        ++frame;
        ++title_frames;
        frame_ms_acc += dt * 1000.0;
        if (frame > 30) {
            frame_ms_log.push_back(static_cast<float>(dt * 1000.0));
            cpu_ms_log.push_back(static_cast<float>(cpu_ms));
            gpu_ms_log.push_back(static_cast<float>(renderer.Stats().gpu_ms));
            anim_ms_log.push_back(static_cast<float>(animator.LastUpdateMs()));
        }
        if (now - title_time >= 0.5) {
            const auto& st = renderer.Stats();
            window.SetTitle(std::format(L"SecondSonPC native | {:.0f} FPS | frame {:.2f} ms (CPU {:.2f}, anim {:.3f}) | GPU {:.2f} ms | {} draws, {} tris | {} | part {}/{} {}",
                                        title_frames / (now - title_time), frame_ms_acc / title_frames, cpu_ms,
                                        animator.LastUpdateMs(), st.gpu_ms, st.draws, st.triangles,
                                        log::Widen(animator.Valid() ? ToString(animator.State()) : "test pose"),
                                        selected_part + 1, part_handles.size(),
                                        part_visible.empty() ? L"" : (part_visible[selected_part] ? L"shown" : L"hidden")));
            title_time = now;
            title_frames = 0;
            frame_ms_acc = 0;
        }
        if (args.frames >= 0 && frame >= static_cast<u64>(args.frames)) break;
    }

    const auto& st = renderer.Stats();
    log::Info("app", "exit after {} frames, {} sim steps ({:.3f} s dropped), player at ({:.2f}, {:.2f}, {:.2f}) grounded {}",
              frame, sim.TotalSteps(), sim.DroppedSeconds(), player.position.x, player.position.y, player.position.z,
              player.grounded);
    log::Info("app", "renderer: validation messages {}, swapchain recreations {}, last GPU frame {:.3f} ms",
              st.validation_messages, st.swapchain_recreations, st.gpu_ms);
    if (!frame_ms_log.empty()) {
        auto mean = [](const std::vector<float>& v) {
            double s = 0;
            for (float x : v) s += x;
            return s / static_cast<double>(v.size());
        };
        std::vector<float> sorted = frame_ms_log;
        std::sort(sorted.begin(), sorted.end());
        const float p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
        const double avg_ms = mean(frame_ms_log);
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        log::Info("perf",
                  "{} frames: {:.1f} FPS avg, 1% low {:.1f} FPS | frame {:.2f} ms, CPU {:.3f} ms, GPU {:.3f} ms, animation "
                  "{:.3f} ms | {} draws, {} tris | RAM private {:.0f} MB, peak working set {:.0f} MB",
                  frame_ms_log.size(), 1000.0 / avg_ms, 1000.0 / p99, avg_ms, mean(cpu_ms_log), mean(gpu_ms_log),
                  mean(anim_ms_log), st.draws, st.triangles, pmc.PrivateUsage / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
    }
    if (!args.modules.empty()) WriteLoadedModules(args.modules);
    renderer.Shutdown();
    log::Shutdown();
    return (args.validation && st.validation_messages > 0) ? 3 : 0;
}
