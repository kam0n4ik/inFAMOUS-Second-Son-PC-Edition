// SecondSonPC — entry point.
//   SecondSonPC.exe [--lang <locale>] [--loader sspc|shadps4] [options]
//     --lang <locale>          overrides [Language] system for this run
//     --loader sspc|shadps4    sspc: SecondSonPC's own Loader (Phase 1);
//                              shadps4: Phase 0 path on shadPS4's Core::Linker (in-binary oracle)
//     --system-name <name>     overrides [System] name
//     --user-data <dir>        overrides [Paths] userData (test runs on a copy of the saves)
//   diagnostics:
//     --no-counter             disables the import call counter
//     --window WxH             the window's size for this run (overrides [Display] width/height)
//     --call-order <N>         records the first N import calls -> logs\call_order.txt
//     --reloc-snapshot <file>  writes every relocation slot at the first ParamGetInt call
//     --selftest               NATIVE vs BRIDGE subsystem self-test -> logs\selftest.txt, no game
//     --fault-test             first-chance crash recorder test -> logs\faulttest.txt, no game
//     --patch-race-test        SSPC-002: threads run guest fs: accesses while the bridge patches them
//                              -> logs\patchrace.txt, no game
//     --no-fault-recorder      no first-chance crash recorder (A/B performance diagnostics)
//     --frame-log              presented frame times -> logs\frametimes.csv (tools\perf_runs.ps1)
//     --track-stats            GPU write tracking statistics (access violations, protection changes,
//                              uploads) every 10 s in SecondSonPC.log; per frame with --frame-log
//     --gpu-stats              PM4 command statistics (P4.1.2): every packet of the game's command buffers
//                              decoded and timed -> logs\gpu_stats.txt, gpu_frames.csv, gpu_stats_windows.csv;
//                              a diagnostic capture, not for FPS comparisons
//     --gpu-probe              SecondSonPC's own Vulkan foundation (P4.1.4) on its own: loader, instance, devices,
//                              the chosen device and queues, a resource round trip -> logs\gpu_probe.txt, no game
//     --gpu-probe-window <s>   the same and a test window cleared to a diagnostic colour for <s> seconds (not a
//                              game frame); --gpu-device <index>: the probe's device instead of its own choice
//     --gpu-shadow             SecondSonPC's command processor (P4.2) beside the bridge's: every packet processed by
//                              both, registers compared before each draw/dispatch -> logs\gpu_shadow.txt; nothing of it
//                              reaches the game (no writes, no waits); a diagnostic mode, not for FPS comparisons
//     --gpu-trace <s>[:<n>]    with the shadow: record <n> frames (4) from the first flip after <s> seconds
//                              -> logs\gpu_trace\*.sspctrace
//     --gpu-replay <trace>     the command processor on a recorded trace, compared with the bridge's recorded state
//                              -> logs\gpu_replay.txt, no game; --gpu-replay-bench <n>: and n timed passes
//     --gpu-shaders            SecondSonPC's shader frontend (P4.3) in the shadow at every draw/dispatch: programs by
//                              content, resources, specializations, pipelines, translations on a worker thread;
//                              compared with the bridge's pipeline at the same packet -> logs\gpu_shadow.txt
//                              (--gpu-shader-validation per-frame|per-bind, --gpu-shaders-no-translate)
//     --gpu-shader-capture     every module the bridge translates, with its inputs -> logs\gpu_shaders\*.sspcshd
//     --gpu-shader-verify <f>  a capture replayed through SecondSonPC's frontend and translator interface, compared
//                              with the bridge's SPIR-V and runtime information -> logs\gpu_shader_verify.txt, no game
//     --frame-pacing           the flip timeline (submit, GPU thread, flip, vblank) -> logs\frame_pacing.csv
//     --gpu-backend bridge|sspc   the game's GPU backend (bridge, the default; sspc is refused for the game in
//                              P4.1: it executes no GPU command of the game yet)
//     --render-mode legacy|hybrid|native_experimental   P5.3: the native render path of --gpu-backend sspc
//                              (engine/render/native_draw.h ApplyRenderMode; SSPC_RENDER_MODE the same)
//     --cp-mode legacy|parallel|native   the command processor of --gpu-backend sspc: one thread / two threads (the
//                              default) / the SecondSon Native CP (runtime/gpu/cp_mode.h; SSPC_CP_MODE the same)
//     --perf-opt <name>        P2.5 bridge performance change on / --no-perf-opt <name> off
//                              (barrier-memo: on by default; docs\PHASE2_P25_PERFORMANCE.md)
//     --vram-log <seconds>     video memory budget/usage samples (SSPC-001)
//     --press <key@sec,...>    automated runs: key presses (SDL key names, e.g. n = cross in the
//                              bridge's default keyboard layout) pushed into the window's event
//                              queue <sec> seconds after the window opens; no window focus needed
//     --bridge <group>         sends a NATIVE group back to BRIDGE (fs, system, ...)
//     --legacy-bridge-kernel   the bridge's kernel/threads/memory/exceptions/CPU patcher/errno/clock/exit
//                              instead of SecondSonPC's (the default with --loader sspc since 2026-10-18)
//     --native <group>         enables an EXPERIMENTAL group, BRIDGE by default (kernel-dmem: direct
//                              memory, P2.3.1; memory: SecondSonPC's memory manager instead of the
//                              bridge's, P2.3.4; threads: SecondSonPC's guest thread manager, P2.4;
//                              cpu (= fs-patch): SecondSonPC's CPU patcher, P3.3/P3.4; exceptions:
//                              SecondSonPC's exception dispatcher, P3.4; errno: SecondSonPC's guest errno,
//                              the bridge's functions use it too, P3.5; kernel-memory: libkernel's memory
//                              functions on SecondSonPC's memory manager (needs memory), P3.5; clock:
//                              SecondSonPC's process clock for the game and the bridge, P3.5; exit: the
//                              guest's _exit through SecondSonPC's orderly exit, P3.6; --loader sspc only)
//     --memory-commit upfront|on-demand   with --native memory: the physical memory's commit
//                              charge at start (upfront, as the bridge) or when handed out
//     --memory-map <seconds>   the guest's virtual and direct memory maps -> logs\memory_map.txt
//     --shader-cache <dir>     the persistent shader cache in <dir> for this run ([ShaderCache] dir)
//     --no-shader-cache        no persistent shader cache for this run
//     --clear-shader-cache     deletes the cache generations under [ShaderCache] dir and exits
//     --scan-game-shaders      indexes every GCN program of the game's .xpps packs (read only) into
//                              [ShaderCache] dir\<game>\native\game_programs.idx and exits (P4.14)
//     --predict-shaders <t>    the material draws' VS/PS modules predicted from the packs (templates <t>) and
//                              translated into the native shader cache, no game (docs\REA_ENGINE_REVERSE.md §8)
//     --if-needed              with --predict-shaders: nothing when the cache was prepared so already (its stamp)
//     --no-shader-prepare      no shader preparation window before the game ([ShaderCache] predictAll)
//     --exports all|native|observers|none, --no-bind <name>   which exports are bound
//     --stock-bootstrap        shadPS4's own startup path inside this binary
//   PC settings (docs/PC_SETTINGS_IMPLEMENTATION.md): the in-game menu (F1) saves changes to
//   %LOCALAPPDATA%\SecondSonPC\settings.toml, which overrides SecondSonPC.toml;
//     --settings <file>        that file instead (also for automated runs)
//     --no-user-settings       SecondSonPC.toml alone, changes not saved (the default of --press, --selftest
//                              and the other no-game modes: their runs stay reproducible)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "app/config.h"
#include "runtime/gpu/gcn/gcn_inventory.h"
#include "app/paths.h"
#include "base/crash.h"
#include "base/lifecycle.h"
#include "base/log.h"
#include "base/vram_monitor.h"
#include "bridge/shadps4/bootstrap.h"
#include "bridge/shadps4/gcn_decoder_check.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/perf_opts.h"
#include "engine/render/display_plan.h"
#include "engine/render/render_resolution.h"
#include "runtime/gpu/cp_replay.h"
#include "runtime/gpu/shader/game_shader_index.h"
#include "runtime/gpu/vulkan/vk_foundation.h"
#include "runtime/gpu/vulkan/vk_stability_tests.h"
#include "runtime/language.h"
#include "runtime/system/system_service.h"
#include "settings/experiments.h"
#include "settings/settings_service.h"
#include "settings/user_store.h"

namespace {

int Fail(const std::string& message) {
    std::fprintf(stderr, "SecondSonPC: %s\n", message.c_str());
    MessageBoxA(nullptr, message.c_str(), "SecondSonPC", MB_ICONERROR | MB_OK);
    return 1;
}

std::wstring QuoteArg(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

/// Every shader of the game's packs compiled before it starts (docs/REA_ENGINE_REVERSE.md §8): this program again with
/// --predict-shaders --if-needed in a console window of its own that shows the progress; the game waits for it (a
/// second when the cache was prepared already). A failure only costs the preparation: the game starts as before.
void PrepareShaders(const SecondSon::App::Config& config) {
    using namespace SecondSon;
    // data\static_templates.txt beside SecondSonPC.toml (game\data in the sources)
    const auto templates = App::RootDir() / "data" / "static_templates.txt";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(templates, ec)) {
        SSPC_LOG(App, Warning, "shader preparation: no {} - skipped", templates.string());
        return;
    }
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    std::wstring cmd = QuoteArg(exe) + L" --gpu-backend sspc --predict-shaders " + QuoteArg(templates.wstring()) +
                       L" --if-needed --shader-cache " + QuoteArg(config.shader_cache_dir.wstring());
    if (config.gpu_device >= 0) {
        cmd += L" --gpu-device " + std::to_wstring(config.gpu_device);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const auto t0 = std::chrono::steady_clock::now();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        App::RootDir().wstring().c_str(), &si, &pi)) {
        SSPC_LOG(App, Warning, "shader preparation: the process did not start (error {})", GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SSPC_LOG(App, Info, "shader preparation: exit code {} after {:.1f} s", code,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

/// The desktop mode (pixels) of the display named `name` as SDL names it on Windows (the monitor's friendly name from
/// the display configuration), or of the primary display; {0, 0} when Windows does not say.
SecondSon::Engine::Render::Size DesktopSize(const std::string& name) {
    std::wstring device;
    UINT32 paths = 0, modes = 0;
    if (!name.empty() && GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
        std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) ==
            ERROR_SUCCESS) {
            for (UINT32 i = 0; i < paths && device.empty(); ++i) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
                target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path[i].targetInfo.adapterId,
                                 path[i].targetInfo.id};
                if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
                    continue;
                }
                char utf8[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, target.monitorFriendlyDeviceName, -1, utf8, sizeof(utf8), nullptr,
                                    nullptr);
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path[i].sourceInfo.adapterId,
                                 path[i].sourceInfo.id};
                if (name == utf8 && DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
                    device = source.viewGdiDeviceName;
                }
            }
        }
    }
    if (device.empty()) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) {
            device = info.szDevice;
        }
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (device.empty() || !EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        return {};
    }
    return {dm.dmPelsWidth, dm.dmPelsHeight};
}

/// The game's display, screen and scene sizes (docs/RESOLUTION_IMPLEMENTATION.md) from [Display] resolution /
/// aspectRatio and [Graphics] renderScale, for the window's size or, in full screen, the display's desktop mode; the
/// game's render targets get the direct memory they need for them.
void PlanGameDisplay(SecondSon::App::Config& config) {
    using namespace SecondSon;
    namespace R = Engine::Render;
    R::Size output{config.window_width, config.window_height};
    if (config.fullscreen) {
        if (const R::Size desktop = DesktopSize(config.pc.monitor); desktop.width != 0) {
            output = desktop;
        }
    }
    R::AspectMode aspect = R::AspectMode::Auto;
    if (!R::ParseAspectMode(config.pc.aspect_ratio, aspect)) {
        SSPC_LOG(App, Warning, "[Display] aspectRatio '{}' is not auto / 16:9 / 16:10 / 21:9 / 32:9 / 4:3: auto",
                 config.pc.aspect_ratio);
    }
    R::Size fixed{};
    if (config.pc.resolution != "auto" && !R::ParseSize(config.pc.resolution, fixed)) {
        SSPC_LOG(App, Warning, "[Display] resolution '{}' is not auto or WxH (320x240..7680x4320): auto",
                 config.pc.resolution);
        fixed = {};
    }
    const R::DisplayPlan plan = R::PlanDisplay(output, aspect, fixed, R::ParseRenderScale(config.pc.render_scale));
    if (plan.Native() && plan.extra_direct_mib == 0) {
        SSPC_LOG(App, Info, "Resolution: {} (the PS4's sizes)", R::Describe(plan));
        return;
    }
    if (config.gpu_backend != App::Config::GpuBackend::Sspc || config.neo_mode) {
        SSPC_LOG(App, Warning, "Resolution: {} needs SecondSonPC's GPU backend (--gpu-backend sspc) on a PS4 (not "
                 "Pro): the game keeps 1920x1080", R::Describe(plan));
        return;
    }
    R::SetDisplayPlan(plan);
    config.extra_dmem_mib = std::min<u32>(config.extra_dmem_mib + plan.extra_direct_mib, 4096);
    SSPC_LOG(App, Info, "Resolution: {} (resolution {}, aspect {}, render scale {}{})", R::Describe(plan),
             config.pc.resolution, config.pc.aspect_ratio, config.pc.render_scale,
             config.fullscreen ? ", full screen" : ", window");
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace SecondSon;
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && std::string_view(argv[1]) == "--exceptions-child") {
        return Bridge::RunExceptionsChild(argc, argv); // P3.4 self-test: a fatal exception in a child process
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--lifecycle-child") {
        return Bridge::RunLifecycleChild(argc, argv); // P3.6 self-test: one way out of a process
    }

    std::vector<std::string> errors;
    auto config = App::LoadConfig(App::RootDir(), errors);
    if (!config) {
        std::string message = "SecondSonPC.toml (" + (App::RootDir() / "SecondSonPC.toml").string() + "):";
        for (const auto& e : errors) {
            message += "\n  " + e;
        }
        return Fail(message);
    }
    // PC settings: SecondSonPC.toml is the defaults; the user settings file (the in-game menu's) overrides them,
    // then the command line overrides both for this run. Automated and no-game runs use SecondSonPC.toml alone.
    // The experiments' defaults are the launcher's environment (settings/experiments.h).
    Settings::ReadExperiments(*config);
    const App::Config toml_defaults = *config;
    std::filesystem::path user_settings_file = Settings::DefaultUserFile();
    bool user_settings = true, explicit_settings = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--settings" && i + 1 < argc) {
            user_settings_file = std::filesystem::absolute(argv[++i]);
            explicit_settings = true;
        } else if (a == "--no-user-settings" || a == "--press" || a == "--selftest" || a == "--fault-test" ||
                   a == "--patch-race-test" || a == "--gpu-stability-test" || a == "--gpu-probe" || a == "--gpu-probe-window" ||
                   a == "--gpu-replay" || a == "--gpu-shader-verify" || a == "--clear-shader-cache" ||
                   a == "--scan-game-shaders" || a == "--predict-shaders" || a == "--gcn-inventory" || a == "--gcn-verify-decoder" || a == "--gcn-translate-test") {
            user_settings = false;
        }
    }
    user_settings = (user_settings || explicit_settings) && !user_settings_file.empty();
    Settings::Overrides user_overrides;
    Settings::KeptKeys user_kept; // keys this version does not take: written back unchanged
    Settings::LoadReport user_report;
    if (user_settings) {
        user_report = Settings::LoadUserFile(user_settings_file, user_overrides, &user_kept);
        Settings::ApplyOverrides(*config, user_overrides);
    }
    // the experiments the user changed, into the environment before any subsystem reads it (logged below)
    const std::vector<std::string> experiments_set =
        user_settings ? Settings::ExportExperiments(*config, user_overrides) : std::vector<std::string>{};
    bool stock_bootstrap = false;
    bool gpu_stability_test = false;
    bool legacy_bridge_kernel = false; // --legacy-bridge-kernel: the bridge's kernel groups, not SecondSonPC's
    bool memory_commit_given = false;
    u32 vram_log_seconds = 0; // --vram-log <seconds>: video memory budget/usage samples (SSPC-001)
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--settings" && has_value) {
            ++i; // read above, before the command line (it is the defaults the command line overrides)
        } else if (arg == "--no-user-settings") {
            // read above
        } else if (arg == "--no-counter") {
            config->import_counter = false;
        } else if (arg == "--window" && has_value) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || w < 320 || h < 240 || w > 7680 || h > 4320) {
                return Fail("--window must be WxH (320x240..7680x4320)");
            }
            config->window_width = w;
            config->window_height = h;
        } else if (arg == "--stock-bootstrap") {
            stock_bootstrap = true; // diagnostics only
        } else if (arg == "--no-bind" && has_value) {
            config->no_bind.emplace_back(argv[++i]); // diagnostics only
        } else if (arg == "--exports" && has_value) {
            const std::string_view mode = argv[++i]; // diagnostics only
            if (mode != "all" && mode != "native" && mode != "observers" && mode != "none") {
                return Fail("--exports must be all|native|observers|none");
            }
            config->bind_natives = mode == "all" || mode == "native";
            config->bind_observers = mode == "all" || mode == "observers";
        } else if (arg == "--lang" && has_value) {
            const auto* lang = Runtime::FindLanguageByLocale(argv[++i]);
            if (lang == nullptr) {
                return Fail(std::string("--lang: unknown locale ") + argv[i]);
            }
            config->language_id = lang->id;
            config->language_locale = std::string(lang->locale);
        } else if (arg == "--loader" && has_value) {
            const std::string_view backend = argv[++i];
            if (backend == "sspc") {
                config->loader = App::Config::LoaderBackend::Sspc;
            } else if (backend == "shadps4") {
                config->loader = App::Config::LoaderBackend::Shadps4;
            } else {
                return Fail("--loader must be sspc|shadps4");
            }
        } else if (arg == "--bridge" && has_value) {
            config->bridged_groups.emplace_back(argv[++i]);
        } else if (arg == "--legacy-bridge-kernel") {
            legacy_bridge_kernel = true;
        } else if (arg == "--native" && has_value) {
            config->native_groups.emplace_back(argv[++i]); // experimental groups only
        } else if (arg == "--memory-commit" && has_value) {
            const std::string_view mode = argv[++i];
            if (mode != "upfront" && mode != "on-demand") {
                return Fail("--memory-commit must be upfront|on-demand");
            }
            config->memory_commit_on_demand = mode == "on-demand";
            memory_commit_given = true;
        } else if (arg == "--cpu-emulate" && has_value) {
            if (std::string_view(argv[++i]) != "sse4a") {
                return Fail("--cpu-emulate: only sse4a");
            }
            config->cpu_emulate_sse4a = true; // diagnostics (P3.4)
        } else if (arg == "--memory-map" && has_value) {
            config->memory_map_s = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--call-order" && has_value) {
            config->call_order = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--reloc-snapshot" && has_value) {
            config->reloc_snapshot = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--system-name" && has_value) {
            config->system_name = argv[++i];
        } else if (arg == "--user-data" && has_value) {
            config->user_data_dir = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--fault-test") {
            config->fault_test = true;
        } else if (arg == "--no-fault-recorder") {
            config->fault_recorder = false; // diagnostics only
        } else if (arg == "--frame-log") {
            config->frame_log = true; // diagnostics only
        } else if (arg == "--track-stats") {
            config->track_stats = true; // diagnostics only (P2.5)
        } else if (arg == "--gpu-stats") {
            config->gpu_stats = true; // diagnostics only (P4.1.2): not for FPS comparisons
        } else if (arg == "--gpu-backend" && has_value) {
            const std::string_view backend = argv[++i];
            if (backend == "bridge") {
                config->gpu_backend = App::Config::GpuBackend::Bridge;
            } else if (backend == "sspc") {
                config->gpu_backend = App::Config::GpuBackend::Sspc;
            } else {
                return Fail("--gpu-backend must be bridge|sspc");
            }
        } else if (arg == "--render-mode" && has_value) {
            const std::string_view mode = argv[++i];
            if (mode != "legacy" && mode != "hybrid" && mode != "native_experimental" && mode != "native") {
                return Fail("--render-mode must be legacy|hybrid|native_experimental");
            }
            config->render_mode = std::string(mode); // (P5.3; needs --gpu-backend sspc, else legacy runs)
        } else if (arg == "--cp-mode" && has_value) {
            const std::string_view mode = argv[++i];
            if (mode != "legacy" && mode != "parallel" && mode != "native") {
                return Fail("--cp-mode must be legacy|parallel|native");
            }
            config->cp_mode = std::string(mode); // (needs --gpu-backend sspc; chosen once, before the backend starts)
        } else if (arg == "--gpu-native-validate") {
            config->gpu_native_validate = true;
        } else if (arg == "--gpu-native-diag") {
            config->gpu_native_diag = true;
        } else if (arg == "--gpu-capture-native-frame" && has_value) {
            config->gpu_capture_flip = std::strtoull(argv[++i], nullptr, 10);
            if (config->gpu_capture_flip == 0) {
                return Fail("--gpu-capture-native-frame <flip>: a flip number from 1");
            }
        } else if (arg == "--gpu-stability-test") {
            gpu_stability_test = true;
        } else if (arg == "--gpu-probe") {
            config->gpu_probe = true;
        } else if (arg == "--gpu-probe-window" && has_value) {
            char* end = nullptr;
            const unsigned long s = std::strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || s == 0 || s > 600) {
                return Fail("--gpu-probe-window <seconds>: 1..600");
            }
            config->gpu_probe = true;
            config->gpu_probe_window_s = static_cast<u32>(s);
        } else if (arg == "--gpu-device" && has_value) {
            char* end = nullptr;
            const long d = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || d < 0 || d > 64) {
                return Fail("--gpu-device <index>: a physical device index (0..64; --gpu-probe lists them)");
            }
            config->gpu_device = static_cast<s32>(d);
        } else if (arg == "--gpu-shadow") {
            config->gpu_shadow = true; // diagnostics only (P4.2): SecondSonPC's command processor beside the bridge's
        } else if (arg == "--gpu-trace" && has_value) {
            // <start seconds>[:<frames>]: the shadow records frames for --gpu-replay from the first flip after it
            const std::string v = argv[++i];
            char* end = nullptr;
            const long s = std::strtol(v.c_str(), &end, 10);
            long frames = 4;
            if (end != v.c_str() && *end == ':') {
                char* end2 = nullptr;
                frames = std::strtol(end + 1, &end2, 10);
                end = end2;
            }
            if (end == v.c_str() || *end != '\0' || s < 0 || s > 3600 || frames < 1 || frames > 120) {
                return Fail("--gpu-trace <start seconds 0..3600>[:<frames 1..120>]");
            }
            config->gpu_shadow = true;
            config->gpu_trace_start_s = static_cast<s32>(s);
            config->gpu_trace_frames = static_cast<u32>(frames);
        } else if (arg == "--gpu-replay" && has_value) {
            config->gpu_replay = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--gpu-replay-bench" && has_value) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || n == 0 || n > 1000) {
                return Fail("--gpu-replay-bench <passes 1..1000>");
            }
            config->gpu_replay_bench = static_cast<u32>(n);
        } else if (arg == "--gpu-shaders") {
            // diagnostics only (P4.3): SecondSonPC's shader frontend in the shadow, compared with the bridge's pipelines
            config->gpu_shaders = true;
            config->gpu_shadow = true;
        } else if (arg == "--gpu-shader-validation" && has_value) {
            const std::string_view v = argv[++i];
            if (v != "per-frame" && v != "per-bind") {
                return Fail("--gpu-shader-validation per-frame|per-bind");
            }
            config->gpu_shaders_per_bind = v == "per-bind";
        } else if (arg == "--gpu-shaders-no-translate") {
            config->gpu_shaders_translate = false;
        } else if (arg == "--gpu-shader-capture") {
            config->gpu_shader_capture = true; // diagnostics only (P4.3): the bridge's translations for --gpu-shader-verify
        } else if (arg == "--gpu-shader-verify" && has_value) {
            config->gpu_shader_verify = std::filesystem::absolute(argv[++i]);
            config->selftest = true; // the bridge's libraries as for the self-test; no game
        } else if (arg == "--frame-pacing") {
            config->frame_pacing = true; // diagnostics only (P4.2, SSPC-015): the flip timeline
        } else if (arg == "--patch-race-test") {
            config->patch_race_test = true; // SSPC-002 regression test -> logs\patchrace.txt
        } else if ((arg == "--perf-opt" || arg == "--no-perf-opt") && has_value) {
            // P2.5: one bridge performance change on or off (bridge/shadps4/perf_opts.h).
            const std::string_view name = argv[++i];
            const bool on = arg == "--perf-opt";
            if (name == "barrier-memo") {
                SecondSon::PerfOpts::barrier_memo = on;
            } else if (name == "image-memo") {
                SecondSon::PerfOpts::image_memo = on;
            } else if (name == "stream-host") {
                SecondSon::PerfOpts::stream_host = on;
            } else if (name == "stream-reads") {
                SecondSon::PerfOpts::stream_reads = on;
            } else {
                return Fail(std::string(arg) + ": unknown change " + std::string(name) +
                            " (barrier-memo, image-memo, stream-host, stream-reads)");
            }
        } else if (arg == "--selftest") {
            config->selftest = true;
            config->import_counter = false;
        } else if (arg == "--vram-log" && has_value) {
            vram_log_seconds = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--press" && has_value) {
            config->test_presses = argv[++i];
        } else if (arg == "--shader-cache" && has_value) {
            config->shader_cache = true; // test runs: a cache of their own (cold/warm series)
            config->shader_cache_dir = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--no-shader-cache") {
            config->shader_cache = false;
        } else if (arg == "--clear-shader-cache") {
            config->clear_shader_cache = true;
        } else if (arg == "--gcn-verify-decoder") {
            config->gcn_verify_decoder = true;
        } else if (arg == "--gcn-translate-test") {
            config->gcn_translate_test = true;
        } else if (arg == "--gcn-inventory") {
            config->gcn_inventory = true;
        } else if (arg == "--scan-game-shaders") {
            config->scan_game_shaders = true;
        } else if (arg == "--if-needed") {
            config->predict_if_needed = true;
        } else if (arg == "--no-shader-prepare") {
            config->prepare_shaders = false;
        } else if (arg == "--predict-shaders" && has_value) {
            // a relative path: from where the user started the program, else from the install root (the process
            // runs in <root>\runtime\sspc by now)
            const std::filesystem::path t = argv[++i];
            std::error_code ec;
            config->predict_shaders = t.is_absolute()                                  ? t
                                      : std::filesystem::exists(App::LaunchDir() / t, ec) ? App::LaunchDir() / t
                                                                                         : App::RootDir() / t;
        } else {
            return Fail(std::string("unknown argument: ") + std::string(arg));
        }
    }

    // Since 2026-10-18 the game runs on SecondSonPC's own kernel, threads, memory, exceptions, CPU patcher, errno,
    // clock and exit (Phase 2/3 of docs/AUDIT.md, measured equal to the bridge's in game): on by default with the
    // own loader; `--bridge <group>` gives one back to the bridge, --legacy-bridge-kernel all of them. Not for the
    // self-tests, which compare one group at a time.
    if (config->loader == App::Config::LoaderBackend::Sspc && !config->selftest && !legacy_bridge_kernel) {
        const auto listed = [](const std::vector<std::string>& v, std::string_view g) {
            return std::find(v.begin(), v.end(), g) != v.end();
        };
        const bool dmem = listed(config->native_groups, "kernel-dmem");
        for (const char* g : {"cpu", "exceptions", "threads", "memory", "errno", "clock", "kernel-memory", "exit"}) {
            const std::string_view group = g;
            if (listed(config->native_groups, group) || listed(config->bridged_groups, group) ||
                (group == "cpu" && (listed(config->native_groups, "fs-patch") ||
                                    listed(config->bridged_groups, "fs-patch"))) ||
                (group == "exceptions" && config->fault_test) ||
                ((group == "memory" || group == "kernel-memory") && dmem) ||
                (group == "kernel-memory" && listed(config->bridged_groups, "memory"))) {
                continue;
            }
            config->native_groups.emplace_back(g);
        }
        if (!memory_commit_given && listed(config->native_groups, "memory")) {
            config->memory_commit_on_demand = true;
        }
    }
    // R10 sysrt (docs/R10_SYSRT.md): SSPC_R10_SYSRT=native - the game's remaining CommonDialog, libSceNet and
    // libSceLibcInternal calls on SecondSonPC's own functions (groups sysrt-dialog, sysrt-net, sysrt-libc; each also
    // with --native <group>, back with --bridge <group>); bridge or unset - the bridge's, as before.
    if (const char* v = std::getenv("SSPC_R10_SYSRT");
        v != nullptr && config->loader == App::Config::LoaderBackend::Sspc && !config->selftest) {
        const std::string_view mode = v;
        if (mode == "native") {
            for (const char* g : {"sysrt-dialog", "sysrt-net", "sysrt-libc"}) {
                if (std::find(config->native_groups.begin(), config->native_groups.end(), g) ==
                        config->native_groups.end() &&
                    std::find(config->bridged_groups.begin(), config->bridged_groups.end(), g) ==
                        config->bridged_groups.end()) {
                    config->native_groups.emplace_back(g);
                }
            }
        } else if (mode != "bridge") {
            return Fail("SSPC_R10_SYSRT must be native or bridge");
        }
    }
    if (config->selftest && config->loader != App::Config::LoaderBackend::Sspc) {
        return Fail("--selftest compares SecondSonPC's subsystems with shadPS4's; it needs --loader sspc");
    }
    if (!config->native_groups.empty() && config->loader != App::Config::LoaderBackend::Sspc) {
        return Fail("--native enables SecondSonPC code on its own loader; it needs --loader sspc");
    }
    for (const auto& g : config->native_groups) {
        if (std::find(config->bridged_groups.begin(), config->bridged_groups.end(), g) !=
            config->bridged_groups.end()) {
            return Fail("--native " + g + " and --bridge " + g + " contradict each other");
        }
    }
    const auto native = [&config](std::string_view g) {
        return std::find(config->native_groups.begin(), config->native_groups.end(), g) != config->native_groups.end();
    };
    if (native("memory") && native("kernel-dmem")) {
        return Fail("--native memory and --native kernel-dmem: SecondSonPC's memory manager keeps the direct memory "
                    "itself; kernel-dmem accounts it beside the bridge's manager");
    }
    if (native("kernel-memory") && !native("memory")) {
        return Fail("--native kernel-memory is libkernel's memory API on SecondSonPC's memory manager: it needs "
                    "--native memory (on the bridge's manager the bridge's functions are its API)");
    }
    if (native("threads")) {
        // The bridge's synchronisation objects take the waiting thread from its own Pthread
        // (g_curthread), which SecondSonPC's guest threads do not have.
        for (const auto* g : {"kernel-mutex", "kernel-condvar", "kernel-rwlock", "kernel-sema", "kernel-eventflag"}) {
            if (std::find(config->bridged_groups.begin(), config->bridged_groups.end(), g) !=
                config->bridged_groups.end()) {
                return Fail(std::string("--native threads and --bridge ") + g +
                            ": the bridge's synchronisation objects need the bridge's own threads");
            }
        }
    }
    if (config->memory_commit_on_demand && !native("memory")) {
        return Fail("--memory-commit on-demand is a mode of SecondSonPC's memory manager: it needs --native memory");
    }
    if (config->cpu_emulate_sse4a && !native("cpu") && !native("fs-patch")) {
        return Fail("--cpu-emulate sse4a is a mode of SecondSonPC's CPU patcher: it needs --native cpu");
    }
    // The bridge's GnmDriver/VideoOut deliver into the bridge's own queue table, and the self-tests check that
    // table: then the game's queues stay the bridge's too (group kernel-equeue).
    if ((config->gpu_backend != App::Config::GpuBackend::Sspc || config->selftest) &&
        std::find(config->bridged_groups.begin(), config->bridged_groups.end(), "kernel-equeue") ==
            config->bridged_groups.end()) {
        config->bridged_groups.emplace_back("kernel-equeue");
    }
    if (config->gpu_backend == App::Config::GpuBackend::Sspc) {
        // P4.4: SecondSonPC's GnmDriver and VideoOut switch together with the backend (one owner of the GPU); the
        // bridge's shadow diagnostics need the bridge's renderer. No silent switch back to the bridge, ever.
        if (config->gpu_shadow || config->gpu_shaders) {
            return Fail("--gpu-shadow / --gpu-shaders compare with the bridge's renderer: not with --gpu-backend sspc");
        }
        for (const char* g : {"gnm", "videoout"}) {
            if (!native(g)) {
                config->native_groups.emplace_back(g);
            }
        }
    } else if (native("gnm") || native("videoout")) {
        return Fail("--native gnm/videoout: SecondSonPC's GnmDriver and VideoOut belong to its own GPU backend: use "
                    "--gpu-backend sspc (they switch together)");
    }
    if (native("exceptions") && config->fault_test) {
        return Fail("--fault-test checks the first-chance recorder's own handlers, which --native exceptions replaces; "
                    "run it without (the dispatcher has its own self-test: --selftest --native exceptions)");
    }
    if (!Log::Init(config->logs_dir / "SecondSonPC.log", config->log_level)) {
        return Fail("cannot open " + (config->logs_dir / "SecondSonPC.log").string());
    }
    // PC settings: what the user file gave, then the owner of the settings for the menu (capabilities: bootstrap).
    if (user_settings) {
        using Status = Settings::LoadReport::Status;
        switch (user_report.status) {
        case Status::Missing:
            SSPC_LOG(App, Info, "user settings {}: none yet (SecondSonPC.toml alone)", user_settings_file.string());
            break;
        case Status::Loaded:
            SSPC_LOG(App, Info, "user settings {}: {} override(s)", user_settings_file.string(), user_overrides.size());
            break;
        case Status::Corrupt:
            SSPC_LOG(App, Warning, "user settings {} do not parse ({}): moved to {}, SecondSonPC.toml alone",
                     user_settings_file.string(), user_report.error, user_report.moved_to.string());
            break;
        case Status::Unreadable:
            SSPC_LOG(App, Warning, "user settings {}: {} - SecondSonPC.toml alone", user_settings_file.string(),
                     user_report.error);
            break;
        }
        for (const auto& w : user_report.warnings) {
            SSPC_LOG(App, Warning, "user settings: {}", w);
        }
        for (const auto& e : experiments_set) {
            SSPC_LOG(App, Warning, "EXPERIMENT from the settings menu (Experiments): {}", e);
        }
    } else {
        SSPC_LOG(App, Info, "user settings: not used in this run (SecondSonPC.toml alone, changes not saved)");
    }
    // A user file that exists but cannot be read is never overwritten by this run's changes (they stay unsaved).
    const bool save_settings = user_settings && user_report.status != Settings::LoadReport::Status::Unreadable;
    Settings::Instance().Init(toml_defaults, user_overrides, *config,
                              save_settings ? user_settings_file : std::filesystem::path{}, 0, user_kept);
    Lifecycle::AddExitStep(Lifecycle::Stage::Storage, "user settings", [] {
        if (!Settings::Instance().Flush(3000)) {
            SSPC_LOG(App, Warning, "user settings: the last change was not written ({})",
                     Settings::Instance().LastSaveError().empty() ? std::string("3 s passed")
                                                                  : Settings::Instance().LastSaveError());
        }
    });
    // P3.4: SecondSonPC's exception dispatcher records execute faults itself; the first-chance recorder's
    // vectored handlers are then not added (the unhandled exception filter is).
    Base::InstallCrashLogger(config->logs_dir / "crash", config->crash_dump,
                             (config->fault_recorder || config->fault_test) && !native("exceptions"));
    if (config->fault_test) {
        return Base::RunFaultSelfTest(config->logs_dir); // before any bridge code: no other handlers
    }
    if (gpu_stability_test) {
        return Gpu::Vk::RunStabilityTests(config->logs_dir);
    }
    if (config->gpu_probe) {
        // P4.1.4: SecondSonPC's Vulkan foundation on its own (levels A/B); no game, no bridge renderer.
        return Gpu::Vk::RunProbe(config->logs_dir, config->gpu_device, config->gpu_probe_window_s, false);
    }
    if (!config->gpu_replay.empty()) {
        // P4.2: SecondSonPC's command processor on a recorded trace; no game, no bridge.
        return Gpu::Cp::RunReplay(config->gpu_replay, config->logs_dir, config->gpu_replay_bench);
    }
    if (config->gpu_replay_bench != 0) {
        return Fail("--gpu-replay-bench times a replay: it needs --gpu-replay <trace>");
    }
    if (config->clear_shader_cache) {
        // <dir>\<game>\<generation>: only directories with the cache's own marker are removed.
        u64 removed = 0;
        const auto dirs_in = [](const std::filesystem::path& dir) {
            std::vector<std::filesystem::path> out; // no exceptions: an unreadable entry ends the list
            std::error_code ec;
            for (auto it = std::filesystem::directory_iterator(dir, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                out.push_back(it->path());
            }
            return out;
        };
        for (const auto& game : dirs_in(config->shader_cache_dir)) {
            for (const auto& gen : dirs_in(game)) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(gen / "generation", ec) &&
                    std::filesystem::remove_all(gen, ec) > 0 && !ec) {
                    ++removed;
                }
            }
            // P4.14: the native backend's generations (<dir>\<game>\native\<generation>, each with its shaders.pack)
            for (const auto& gen : dirs_in(game / "native")) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(gen / "shaders.pack", ec) &&
                    std::filesystem::remove_all(gen, ec) > 0 && !ec) {
                    ++removed;
                }
            }
        }
        SSPC_LOG(App, Info, "--clear-shader-cache: {} cache generation(s) removed from {}", removed,
                 config->shader_cache_dir.string());
        std::printf("SecondSonPC: %llu shader cache generation(s) removed from %s\n",
                    static_cast<unsigned long long>(removed), config->shader_cache_dir.string().c_str());
        return 0;
    }
    if (config->gcn_verify_decoder) {
        return Bridge::RunGcnDecoderCheck(*config);
    }
    if (config->gcn_translate_test) {
        std::string error;
        const auto report = App::RootDir() / "logs" / "gcn_translate_test.txt";
        if (!Gpu::Gcn::RunTranslateTest(config->game_dir, config->shader_cache_dir, report, error)) {
            return Fail("--gcn-translate-test: " + error);
        }
        std::printf("SecondSonPC: GCN translate test -> %s\n", report.string().c_str());
        return 0;
    }
    if (config->gcn_inventory) {
        // Phase 4, own translator: what SecondSonPC's GCN decoder makes of every program of the game (read only)
        std::string error;
        const auto report = App::RootDir() / "logs" / "gcn_inventory.txt";
        if (!Gpu::Gcn::RunInventory(config->game_dir, config->shader_cache_dir, report, error)) {
            return Fail("--gcn-inventory: " + error);
        }
        std::printf("SecondSonPC: GCN inventory -> %s\n", report.string().c_str());
        return 0;
    }
    if (config->scan_game_shaders) {
        // P4.14 (docs\OFFLINE_SHADER_PRECOMPILATION.md): every GCN program of the game's packs by content, read only
        Gpu::Shader::GameShaderIndex index;
        Gpu::Shader::GameShaderIndex::ScanStats st;
        std::string error;
        const std::string game = config->game_dir.filename().string();
        const auto out = Gpu::Shader::NativeCacheRoot(config->shader_cache_dir, game) / "game_programs.idx";
        std::printf("SecondSonPC: scanning %s for shader programs (read only)...\n", config->game_dir.string().c_str());
        const auto progress = [](void*, u64 done, u64 total) {
            if (done % 100 == 0 || done == total) {
                std::printf("  %llu / %llu packs\n", static_cast<unsigned long long>(done),
                            static_cast<unsigned long long>(total));
            }
        };
        if (!index.Scan(config->game_dir, st, error, progress, nullptr) || !index.Save(out, error)) {
            return Fail("--scan-game-shaders: " + error);
        }
        std::map<u32, u64> by_type;
        std::vector<u64> per_pack(index.Packs().size());
        u64 shared = 0;
        for (const auto& p : index.Programs()) {
            ++by_type[p.footer_type];
            shared += p.members > 1 ? 1 : 0;
        }
        for (const auto& m : index.Members()) {
            ++per_pack[m.pack];
        }
        std::string text = fmt::format(
            "game shader scan (P4.14): {} packs, {:.1f} GiB read in {:.1f} s; {} footers, {} without their program's "
            "start; {} distinct programs ({} in more than one pack) -> {}\nby footer type:",
            st.files, static_cast<double>(st.bytes) / (1ull << 30), st.ms / 1000.0, st.footers, st.unresolved,
            index.Programs().size(), shared, out.string());
        for (const auto& [type, n] : by_type) {
            text += fmt::format(" type {}: {};", type, n);
        }
        text += "\nprograms (with repeats) per pack, the 40 largest:\n";
        std::vector<std::pair<u64, size_t>> order;
        for (size_t i = 0; i < per_pack.size(); ++i) {
            if (per_pack[i] != 0) {
                order.emplace_back(per_pack[i], i);
            }
        }
        std::ranges::sort(order, std::greater<>{});
        for (size_t i = 0; i < order.size() && i < 40; ++i) {
            text += fmt::format("  {:6} {}\n", order[i].first, index.Packs()[order[i].second]);
        }
        text += fmt::format("packs with programs: {} of {}\n", order.size(), index.Packs().size());
        std::error_code ec;
        std::filesystem::create_directories(config->logs_dir, ec);
        std::ofstream(config->logs_dir / "game_shader_scan.txt", std::ios::trunc) << text;
        SSPC_LOG(App, Info, "{}", text);
        std::printf("%s", text.c_str());
        return 0;
    }
    // the game itself (not a test, a probe or an offline mode, not an automated run): its shaders first
    if (config->prepare_shaders && config->pc.shader_predict && config->predict_shaders.empty() &&
        config->gpu_backend == App::Config::GpuBackend::Sspc && config->shader_cache && !config->selftest &&
        !config->fault_test && !config->gpu_probe && config->gpu_replay.empty() && config->gpu_shader_verify.empty() &&
        !config->patch_race_test && !gpu_stability_test && config->test_presses.empty()) {
        PrepareShaders(*config);
    }
    Base::StartVramLog(vram_log_seconds);
    const bool own_loader = config->loader == App::Config::LoaderBackend::Sspc;
    SSPC_LOG(App, Info, "SecondSonPC Phase 1 — loader: {}",
             own_loader ? "SecondSonPC (sspc)" : "Phase 0 path on the shadPS4 core (shadps4, oracle)");
    SSPC_LOG(App, Info, "root {}, config {}", config->root.string(), config->file.string());
    SSPC_LOG(App, Info, "game {}", config->game_dir.string());
    SSPC_LOG(App, Info, "user data {}, logs {}", config->user_data_dir.string(), config->logs_dir.string());

    const auto* language = Runtime::FindLanguageById(config->language_id);
    SSPC_LOG(Language, Info, "system language {} = PS4 id {} ({}), game tag \"{}\"", language->locale,
             language->id, language->display, language->game_tag);
    if (!Runtime::ReportLanguageFiles(*language, config->game_dir)) {
        SSPC_LOG(Language, Error, "language files for {} are missing in the game folder", language->locale);
    }

    PlanGameDisplay(*config); // (before the bridge reads the memory budget)

    SystemService::Configure(SystemService::Settings{
        .language_id = config->language_id,
        .system_name = config->system_name,
        .enter_button_cross = config->enter_button_cross,
    });
    SSPC_LOG(SystemService, Info, "SystemName \"{}\", enter button {}", config->system_name,
             config->enter_button_cross ? "cross" : "circle");
    SSPC_LOG(Trace, Info, "import counter {}, call order {}, bind natives {}, bind observers {}",
             config->import_counter ? "on" : "off", config->call_order, config->bind_natives,
             config->bind_observers);
    for (const auto& g : config->bridged_groups) {
        SSPC_LOG(App, Warning, "DIAGNOSTIC: --bridge {}", g);
    }
    for (const auto& g : config->native_groups) {
        SSPC_LOG(App, Warning, "EXPERIMENTAL: --native {}", g);
    }

    if (stock_bootstrap) {
        Bridge::RunStockForDiagnostics(*config);
    }
    Bridge::RunGame(*config);
}
