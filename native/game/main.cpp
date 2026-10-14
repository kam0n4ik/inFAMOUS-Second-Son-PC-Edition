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
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
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
#include "scene/camera.h"
#include "scene/character_model.h"
#include "scene/primitives.h"
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
    }
    LocalFree(argv);
    return a;
}

std::wstring Join(const std::wstring& dir, const std::wstring& rel) {
    if (rel.size() > 1 && (rel[1] == L':' || rel[0] == L'\\' || rel[0] == L'/')) return rel;
    return dir + L"\\" + rel;
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

Vec4 PartColor(size_t i) {
    // Neutral clay with a slight per-part variation so separate parts stay distinguishable.
    const float t = static_cast<float>((i * 37) % 11) / 10.0f;
    return {0.72f + 0.08f * t, 0.70f + 0.04f * t, 0.66f, 1.0f};
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
    std::vector<bool> part_visible;
    const bool have_model = LoadCharacterModel(package, cache, jobs, model, load_error);
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
        for (size_t i = 0; i < model.meshes.size(); ++i) {
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
        have_world = LoadWorldChunk(Join(data_dir, log::Widen(chunk)), renderer, jobs, world, world_error);
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
    FreeCamera free_cam;
    bool free_mode = false;
    FixedStepClock sim(1.0 / static_cast<double>(cfg.GetInt("Game", "SimulationHz", 60)));
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
        bool sprint = input.Down(Action::Sprint);
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
        for (u32 s = 0; s < steps; ++s) {
            prev_player = player;
            MoveInput mi;
            if (!free_mode) {
                mi.x = mx;
                mi.y = my;
                mi.sprint = sprint;
                mi.jump = pending_jump;
            }
            mi.camera_yaw = orbit.ForwardYaw();
            StepCharacter(player, params, mi, static_cast<float>(sim.Step()), obstacles,
                          have_world ? &world.terrain : nullptr);
            pending_jump = false;
            if (player.position.z < kill_z) {  // fell through a hole in the walkable surfaces: respawn
                player = CharacterState{};
                player.position = have_world ? world.spawn : Vec3{0, 0, 0};
                prev_player = player;
                log::Info("app", "respawned (fell below {:.1f} m)", kill_z);
            }
        }
        const float alpha = static_cast<float>(sim.Alpha());
        const Vec3 render_pos = Lerp(prev_player.position, player.position, alpha);
        orbit.target = render_pos + Vec3{0, 0, 1.45f};

        std::vector<DrawItem> items;
        if (have_world) {
            items.insert(items.end(), world.draws.begin(), world.draws.end());
        } else {
            items.push_back({h_even, Mat4::Identity(), {0.46f, 0.47f, 0.45f, 1}});
            items.push_back({h_odd, Mat4::Identity(), {0.38f, 0.39f, 0.37f, 1}});
            for (MeshHandle h : obstacle_meshes) items.push_back({h, Mat4::Identity(), {0.55f, 0.40f, 0.26f, 1}});
        }
        const Mat4 hero_world = Mat4::Translation(render_pos) * Mat4::RotationZ(player.yaw) * Mat4::Scale(model_scale);
        for (size_t i = 0; i < part_handles.size(); ++i) {
            if (!part_visible[i] || part_handles[i] == kInvalidMesh) continue;
            Vec4 c = PartColor(i);
            if (i == selected_part && window.MouseCaptured() == false) c = {0.95f, 0.55f, 0.20f, 1};
            items.push_back({part_handles[i], hero_world, c});
        }
        FrameView fv;
        fv.view = free_mode ? free_cam.View() : orbit.View();
        fv.projection = Mat4::Perspective(60.0f * kPi / 180.0f, renderer.AspectRatio(), 0.1f, 2000.0f);
        if (args.frames >= 0 && frame + 1 == static_cast<u64>(args.frames) && !args.screenshot.empty()) {
            renderer.RequestScreenshot(args.screenshot);
        }
        if (!renderer.RenderFrame(fv, items)) {
            log::Error("app", "rendering failed; exiting");
            break;
        }
        ++frame;
        ++title_frames;
        frame_ms_acc += dt * 1000.0;
        if (now - title_time >= 0.5) {
            const auto& st = renderer.Stats();
            window.SetTitle(std::format(L"SecondSonPC native | {:.0f} FPS | CPU frame {:.2f} ms | GPU {:.2f} ms | {} draws, {} tris | part {}/{} {}",
                                        title_frames / (now - title_time), frame_ms_acc / title_frames, st.gpu_ms,
                                        st.draws, st.triangles, selected_part + 1, part_handles.size(),
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
    if (!args.modules.empty()) WriteLoadedModules(args.modules);
    renderer.Shutdown();
    log::Shutdown();
    return (args.validation && st.validation_messages > 0) ? 3 : 0;
}
