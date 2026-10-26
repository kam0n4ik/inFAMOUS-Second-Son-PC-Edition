// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project (sequence from src/emulator.cpp)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY bridge (removed in Phase 5).
// Start-up sequence adapted from shadPS4 Core::Emulator::Run()
// (third_party/shadPS4 @ 0fe263a4760dfbfa973366890061749b4af0de97). Differences:
//  * configuration comes from SecondSonPC.toml (ApplyBridgeSettings). On the sspc path shadPS4's
//    config.json, custom_configs\<id>.json, users.json and keys.json in runtime\sspc\user are not
//    read; the shadps4 path loads them as Phase 0 did and then gets the same SecondSonPC values;
//  * /savedata0 lives under SecondSonPC [Paths] userData (shadPS4 "home dir"), not in any shadPS4 install;
//  * no game patches/cheats, Discord RPC, launcher IPC or play-time tracking.
// Two loader paths (--loader):
//  * sspc     Phase 1: SecondSonPC's Loader (src/loader) loads eboot.bin + the game's PRX, resolves
//             imports through the static binding table (NATIVE / BRIDGE by C++ name / COMMON_STUB);
//             shadPS4's Core::Linker, Core::Loader, SymbolsResolver/InitHLELibs and aerolib are not
//             used. Memory, threads, CPU patches, GPU, input and audio stay BRIDGE.
//  * shadps4  Phase 0 path, unchanged: shadPS4's Core::Linker with the HLE symbol table rebuilt
//             (SecondSonPC Phase 0 exports first). Kept as the in-binary oracle until Phase 1 ends.

#include <WinSock2.h>
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include "app/config.h"
#include "base/crash.h"
#include "base/lifecycle.h"
#include "base/log.h"
#include "bridge/shadps4/bootstrap.h"
#include "bridge/shadps4/bridge_exports.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/memory_host.h"
#include "bridge/shadps4/native_memory.h"
#include "bridge/shadps4/perf_opts.h"
#include "bridge/shadps4/settings_host.h"
#include "bridge/shadps4/sysrt_host.h"
#include "runtime/gpu/gcn/gcn_translator.h"
#include "runtime/savedata/savedata.h"
#include "runtime/gpu/upscale/reflex.h"
#include "ui/menu_controller.h"
#include "bridge/shadps4/track_diag.h"
#include "engine/game_clock_probe.h"
#include "runtime/gpu/r8/r8_verify_frame.h"
#include "runtime/gpu/r9/gprof.h"
#include "runtime/gpu/r10/latency.h"
#include "engine/guest_hooks.h"
#include "engine/game_fixes.h"
#include "engine/mouse_camera.h"
#include "engine/job_system.h"
#include "engine/particles/smoke_quality.h"
#include "engine/render/post_effects.h"
#include "engine/render/weather.h"
#include "engine/render/rain_shadows.h"
#include "engine/render/reflections.h"
#include "engine/render/frame_trace.h"
#include "engine/render/draw_census.h"
#include "engine/render/light_census.h"
#include "engine/render/draw_runs.h"
#include "native_engine/native_function_registry.h"
#include "engine/render/native_draw.h"
#include "engine/render/render_resolution.h"
#include "app/product.h"
#include "engine/ui/game_menu.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "settings/settings_service.h"
#include "loader/bindings.h"
#include "loader/image_file.h"
#include "loader/linker.h"
#include "loader/snapshot.h"
#include "runtime/cpu/exceptions.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/exports.h"
#include "bridge/shadps4/gpu_memory_host.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/cp_mode.h"
#include "runtime/gpu/cp_shadow.h"
#include "runtime/gpu/frame_pacing.h"
#include "runtime/gpu/native_backend.h"
#include "runtime/gpu/static/static_predictor.h"
#include "runtime/gpu/pm4_stats.h"
#include "runtime/gpu/shader/shader_capture.h"
#include "bridge/shadps4/shader_bridge_hooks.h"
#include "runtime/kernel/clock.h"
#include "runtime/kernel/thread.h"
#include "runtime/memory/virtual_memory.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/filesystem/vfs.h"
#include "runtime/psf/param_sfo.h"
#include "runtime/sysmodule/sysmodule.h"
#include "runtime/system/system_service.h"
#include "trace/import_counter.h"

#include "common/elf_info.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/ntapi.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/libs.h"
#include "core/libraries/save_data/save_backup.h"
#include "core/linker.h"
#include "core/debug_state.h"
#include "core/guest_memory.h"
#include "core/signals.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "input/controller.h"
#include "input/keyboard_mouse.h"
#include "sdl_window.h"
#include "video_core/cache_storage.h"
#include "video_core/renderdoc.h"

extern Frontend::WindowSDL* g_window; // defined in shadPS4 src/emulator.cpp

namespace Libraries::Kernel {
extern char const* g_environment[64];
}

// shadPS4 library entry points started explicitly on the sspc path (StartBridgeLibraries).
namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::VideoOut {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::Pad {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::Np::NpManager {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::SystemService {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::UserService {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::AppContent {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::PlayGo {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}

namespace Core { // defined in shadPS4 src/emulator.cpp
s32 ReadCompiledSdkVersion(const std::string& guest_or_host_path);
std::map<s32, std::string> ExtractTrophies(std::string_view npbind_guest,
                                           std::string_view trophy_dir_guest);
} // namespace Core

namespace Common {
// ElfInfo's fields are private (only Core::Emulator may fill them); this class is made a friend by
// patches/shadps4/0002-elfinfo-friend-secondson-bootstrap.patch. Phase 0-4 only.
class SecondSonBootstrap {
public:
    struct GameInfo {
        std::string serial;
        std::string title;
        std::string app_version;
        u32 firmware = 0;
        u32 sdk = 0;
        PSFAttributes attributes{};
        std::vector<u8> splash;
        std::filesystem::path folder;
    };

    static void Fill(GameInfo info) {
        auto& e = ElfInfo::Instance();
        e.initialized = true;
        e.game_serial = std::move(info.serial);
        e.title = std::move(info.title);
        e.app_ver = std::move(info.app_version);
        e.firmware_ver = info.firmware & 0xFFF00000;
        e.raw_firmware_ver = info.firmware;
        e.sdk_ver = info.sdk;
        e.psf_attributes = info.attributes;
        e.splash_data = std::move(info.splash);
        e.game_folder = std::move(info.folder);
    }

    static void SetTrophyIndexMap(std::map<s32, std::string> map) {
        ElfInfo::Instance().trophy_index_map = std::move(map);
    }
};
} // namespace Common

namespace SecondSon::Bridge {

namespace {

using LoaderBackend = App::Config::LoaderBackend;

::Input::GameControllers* g_controllers = nullptr; // shadPS4's (not SecondSon::Input)

// P3.6: the end of the process, step by step (base/lifecycle.h). The steps run once, on the exiting thread,
// while the other threads still run; the process then exits (ExitProcess ends the bridge's threads).
void ExitStepCallCounts() {
    Trace::FinalDump();
}

void ExitStepRuntimeSummaries() {
    if (NativeFsPatcher()) {
        const auto s = Cpu::GetStats();
        SSPC_LOG(Cpu, Info,
                 "FS patcher at exit: {} modules, {} sites patched at load time, {} on first use ({} repeats), {} threads "
                 "sent back to a site being patched ({} reported as privileged instruction), {} unsupported{}, {} "
                 "trampoline bytes",
                 s.modules, s.static_sites, s.runtime_sites, s.runtime_repeats, s.breakpoints, s.misreported,
                 s.unsupported,
                 s.first_unsupported != 0 ? fmt::format(" (first met at {:#x})", s.first_unsupported) : std::string{},
                 s.trampoline_bytes);
        if (Cpu::Sse4aEmulated()) {
            SSPC_LOG(Cpu, Info,
                     "SSE4a at exit: {} sites to trampolines, {} movnt* rewritten, {} executed from their exception "
                     "({} executions)",
                     s.sse4a_trampolines, s.sse4a_inplace, s.sse4a_short, s.sse4a_emulated);
        }
    }
    Cpu::Exceptions::LogSummary("at exit"); // P3.4 (nothing without --native exceptions)
    // P3.5.3: the process clock's drift against QueryPerformanceCounter over the session; the bridge's own TSC
    // frequency for comparison (only once its clock exists: not in the early test paths).
    Kernel::Clock::LogSummary("at exit", Kernel::Clock::Enabled() || Libraries::Kernel::Dev::GetClock() != nullptr
                                             ? Libraries::Kernel::sceKernelGetTscFrequency()
                                             : 0);
    // P3.2: what the native managers hold at the end, and the process (leak checks of a long session).
    if (Kernel::Threads::Enabled()) {
        Kernel::Threads::LogSummary();
    }
    if (const auto native = GetNativeGame(); native.vm != nullptr) {
        const auto m = native.vm->GetStats();
        SSPC_LOG(Memory, Info,
                 "native memory at exit: {} areas, maps {}, unmaps {}, protects {}, refused {}; host: views {}, private "
                 "{}, unmaps {}, remaps {}, protection restores {}, commits {}, protect failures {}, coalesce "
                 "failures {}",
                 m.vma_count, m.maps, m.unmaps, m.protects, m.failures, m.host.views, m.host.privates, m.host.unmaps,
                 m.host.remaps, m.host.protect_restores, m.host.commits, m.host.protect_failures,
                 m.host.coalesce_failures);
    }
}

void ExitStepProcessSummary() {
    {
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        u32 threads = 0;
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                threads += te.th32OwnerProcessID == GetCurrentProcessId() ? 1 : 0;
            }
            CloseHandle(snap);
        }
        SSPC_LOG(Bridge, Info, "process at exit: {} handles, {} threads, private commit {} MiB (peak {} MiB)", handles,
                 threads, pmc.PrivateUsage >> 20, pmc.PeakPagefileUsage >> 20);
    }
    // Before the steps that wait for the bridge's threads: what is written so far survives their watchdog.
    Log::Flush();
    Common::Log::Flush();
}

void ExitStepSaveDataBackup() {
    Libraries::SaveData::Backup::StopThread(); // waits for a backup being written
}

void ExitStepShaderCache() {
    Storage::DataBase::Instance().Close(); // writes what is queued; its statistics go to the bridge's log
}

void ExitStepControllerLights() {
    if (g_controllers != nullptr) {
        g_controllers->ResetLightbarColors();
        Sleep(10); // SDL sends it
    }
}

void ExitStepLogs() {
    SSPC_LOG(Bridge, Info, "session end");
    Log::Flush();
    Common::Log::Flush(); // the cache's last lines (its statistics, tools\perf_report.py reads them)
}

void InstallLifecycle() {
    using Lifecycle::Stage;
    Lifecycle::AddExitStep(Stage::Reports, "import call counts", &ExitStepCallCounts);
    Lifecycle::AddExitStep(Stage::Reports, "runtime summaries", &ExitStepRuntimeSummaries);
    Lifecycle::AddExitStep(Stage::Reports, "process summary", &ExitStepProcessSummary);
    Lifecycle::AddExitStep(Stage::Services, "save data backup thread (bridge)", &ExitStepSaveDataBackup);
    Lifecycle::AddExitStep(Stage::Storage, "shader cache (bridge)", &ExitStepShaderCache);
    Lifecycle::AddExitStep(Stage::Devices, "controller lights (bridge)", &ExitStepControllerLights);
    Lifecycle::AddExitStep(Stage::Logs, "logs", &ExitStepLogs);
    Lifecycle::Install(30000);
    InstallBridgeShutdownHook(); // the bridge starts no shutdown of its own (patch 0026)
}

[[noreturn]] void Fatal(std::string_view message) {
    SSPC_LOG(Bridge, Critical, "{}", message);
    Log::Flush();
    MessageBoxA(nullptr, std::string(message).c_str(), "SecondSonPC", MB_ICONERROR | MB_OK);
    Lifecycle::Exit(1, "a start-up failure");
}

std::string NidName(const std::string& nid) {
    const auto* entry = Core::AeroLib::FindByNid(nid.c_str());
    return entry != nullptr ? entry->name : "?";
}

bool IsUnder(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto rel = std::filesystem::weakly_canonical(path).lexically_relative(
        std::filesystem::weakly_canonical(root));
    return !rel.empty() && *rel.begin() != "..";
}

bool Listed(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Every EmulatorSettings value the bridge reads on the game's path that SecondSonPC decides
/// (docs/MIGRATION.md, "EmulatorSettings"). On the sspc path nothing else is loaded, so the other
/// values are shadPS4's built-in defaults: bridge internals SecondSonPC keeps as they are (GPU
/// accuracy knobs, log details) or features this game does not use (camera, IME, USB, trophy
/// pop-up layout, launcher). The same values are applied on the shadps4 path after its
/// config.json, so both loaders run with identical settings.
void ApplyBridgeSettings(const App::Config& cfg) {
    auto& s = EmulatorSettings;
    // [Console]: the emulated PS4 (sceKernelIsNeoMode, the direct/flexible memory budget).
    s.SetNeo(cfg.neo_mode);
    s.SetDevKit(cfg.dev_kit);
    s.SetExtraDmemInMBytes(static_cast<int>(cfg.extra_dmem_mib));
    s.SetExtraFmemInMBytes(static_cast<int>(cfg.extra_fmem_mib));
    // shadPS4-internal users of the language (trophy XML, save locale); the game asks ours.
    s.SetConsoleLanguage(cfg.language_id);
    // [Paths]: /savedata0 and trophies of the BRIDGE SaveData/NpTrophy live under userData\home.
    s.SetHomeDir(cfg.user_data_dir / "home");
    // [Display]
    s.SetFullScreen(cfg.fullscreen);
    // shadPS4's spelling: anything but "Fullscreen" means borderless (its default is "Windowed").
    s.SetFullScreenMode(cfg.exclusive_fullscreen ? "Fullscreen" : "Windowed");
    s.SetWindowWidth(cfg.window_width);
    s.SetWindowHeight(cfg.window_height);
    s.SetVblankFrequency(cfg.vblank_hz);
    s.SetPresentMode(cfg.present_mode);
    s.SetShowFpsCounter(cfg.show_fps);
    ApplyPcBridgeSettings(cfg); // [Graphics]: FSR/RCAS of the bridge's presenter (read when it is created)
    // P4.4 diagnostics: SSPC_DIAG_BRIDGE_READBACKS=1 (relaxed) / 2 (precise) - the bridge's GPU readbacks (shadPS4's
    // default: none; the native backend writes every GPU write back) for a comparison of the two backends' images
    if (char rb[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_BRIDGE_READBACKS", rb, sizeof(rb)) > 0 &&
                         (rb[0] == '1' || rb[0] == '2')) {
        s.SetReadbacksMode(rb[0] == '1' ? GpuReadbacksMode::Relaxed : GpuReadbacksMode::Precise);
        SSPC_LOG(Bridge, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BRIDGE_READBACKS={} - the bridge reads GPU writes back",
                 rb[0]);
    }
    // smoke research: SSPC_DIAG_BRIDGE_DUMP_SHADERS=1 - the recompiler's dumps of every shader it translates (the IR after
    // its passes, among them the wave64 lowering) -> the bridge's shader directory\dumps
    if (char dv[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_BRIDGE_DUMP_SHADERS", dv, sizeof(dv)) == 1 && dv[0] == '1') {
        s.SetDumpShaders(true);
        SSPC_LOG(Bridge, Warning, "DIAGNOSTIC: SSPC_DIAG_BRIDGE_DUMP_SHADERS=1 - every translated shader dumped to {}",
                 (Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "dumps").string());
    }
    // [Audio]
    s.SetAudioBackend(AudioBackend::SDL);
    s.SetVolumeSlider(static_cast<int>(cfg.volume));
    s.SetSDLMainOutputDevice(cfg.audio_device.empty() ? "Default Device" : cfg.audio_device);
    // Offline console: no network, no shadNet (NP offline is Phase 3).
    s.SetConnectedToNetwork(false);
    s.SetShadNetEnabled(false);
    // [ShaderCache] (patch 0013): <dir>\<game>\<generation>-<device> keeps the translated SPIR-V,
    // the pipeline keys and the driver's VkPipelineCache between runs (one file per blob).
    bool shader_cache = cfg.shader_cache;
    if (shader_cache) {
        std::error_code ec; // SetUserPath ignores a directory that does not exist
        std::filesystem::create_directories(cfg.shader_cache_dir, ec);
        if (!std::filesystem::is_directory(cfg.shader_cache_dir, ec)) {
            SSPC_LOG(Bridge, Error, "[ShaderCache] dir {} cannot be created: the cache is off",
                     cfg.shader_cache_dir.string());
            shader_cache = false;
        } else {
            Common::FS::SetUserPath(Common::FS::PathType::CacheDir, cfg.shader_cache_dir);
        }
    }
    s.SetPipelineCacheEnabled(shader_cache);
    s.SetPipelineCacheArchived(false);
    // [Debug]
    s.SetVkValidationEnabled(cfg.vulkan_validation);
    s.SetVkCrashDiagnosticEnabled(cfg.vulkan_crash_diagnostic);
    // [Log]
    s.SetLogFilter(cfg.bridge_log_filter);
    SSPC_LOG(Bridge, Info,
             "bridge settings from SecondSonPC.toml: neo {}, devkit {}, extra dmem/fmem {}/{} MiB, "
             "window {}x{}{}, vblank {} Hz, present {}, volume {}, audio device \"{}\", "
             "vk validation {}, crash diagnostic {}, shader cache {}",
             cfg.neo_mode, cfg.dev_kit, cfg.extra_dmem_mib, cfg.extra_fmem_mib, cfg.window_width,
             cfg.window_height,
             cfg.fullscreen ? (cfg.exclusive_fullscreen ? " fullscreen" : " borderless") : "",
             cfg.vblank_hz, cfg.present_mode, cfg.volume, cfg.audio_device, cfg.vulkan_validation,
             cfg.vulkan_crash_diagnostic,
             cfg.shader_cache ? cfg.shader_cache_dir.string() : std::string("off"));
}

/// Host file of a guest path in /app0 (the module loader's view; the game's files go through the
/// FileSystem). Empty if the path is not in /app0.
std::filesystem::path HostPathOf(const App::Config& cfg, std::string_view guest) {
    constexpr std::string_view kApp0 = "/app0/";
    if (!guest.starts_with(kApp0)) {
        return {};
    }
    auto relative = std::string(guest.substr(kApp0.size()));
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return cfg.game_dir / relative;
}

/// Which SecondSonPC exports a loader path binds (diagnostic filters included).
std::vector<Runtime::Export> SelectExports(const App::Config& cfg, bool phase0_path) {
    std::vector<Runtime::Export> out;
    for (const auto& e : Runtime::Exports()) {
        const bool observer = e.original != nullptr;
        const std::string_view group = e.group;
        const bool loader_core = group == "loader" || group == "sysmodule";
        if (phase0_path && !e.phase0) {
            continue; // Phase 1 code: the Phase 0 path keeps shadPS4's implementation
        }
        if (e.experimental && !Listed(cfg.native_groups, group)) {
            continue; // BRIDGE unless enabled with --native <group>
        }
        if (!loader_core && ((observer && !cfg.bind_observers) || (!observer && !cfg.bind_natives) ||
                             Listed(cfg.bridged_groups, group) || Listed(cfg.no_bind, e.name))) {
            SSPC_LOG(Bridge, Warning, "DIAGNOSTIC: {} not bound (shadPS4 implementation used)", e.name);
            continue;
        }
        out.push_back(e);
    }
    return out;
}

/// DIAGNOSTIC (--reloc-snapshot): writes every relocation slot at the first ParamGetInt call.
void InstallSnapshotHook(const App::Config& cfg,
                         std::function<std::vector<Loader::SnapshotModule>()> modules,
                         const Loader::Bindings* bindings) {
    if (cfg.reloc_snapshot.empty()) {
        return;
    }
    SystemService::SetFirstParamGetIntHook([file = cfg.reloc_snapshot, modules, bindings] {
        const auto list = modules();
        std::string error;
        if (Loader::WriteRelocationSnapshot(file, list, bindings, error)) {
            SSPC_LOG(Loader, Info, "relocation snapshot ({} modules) -> {}", list.size(), file.string());
        } else {
            SSPC_LOG(Loader, Error, "relocation snapshot failed: {}", error);
        }
    });
}

// ------------------------------------------------------------------------------ Phase 0 path

void BuildPhase0BindingTable(Core::Linker& linker, const App::Config& cfg) {
    using namespace Core::Loader;

    SymbolsResolver shad; // temporary shadPS4 HLE table (BRIDGE)
    Libraries::InitHLELibs(&shad);
    // Linker::Execute() registers the libSceLibcInternal HLE itself when the LLE sprx is absent;
    // registering it here first puts these functions (used by the game's libSceFios2.prx) behind
    // the counter too. Execute()'s later duplicate registration loses to these (first match wins).
    const auto libc_internal_sprx = EmulatorSettings.GetSysModulesDir() / "libSceLibcInternal.sprx";
    if (!std::filesystem::exists(libc_internal_sprx)) {
        Libraries::LibcInternal::RegisterLib(&shad);
    }

    auto& table = linker.GetHLESymbols();
    if (cfg.import_counter && !Trace::Init(shad.GetSize() + 64)) {
        SSPC_LOG(Trace, Error, "import counter disabled (allocation failed)");
    }

    // 1) SecondSonPC exports first: SymbolsResolver::FindSymbol returns the first match.
    const auto exports = SelectExports(cfg, true);
    for (const auto& e : exports) {
        SymbolResolver key{};
        key.name = e.nid;
        key.nidName = e.name;
        key.library = e.library;
        key.library_version = 1;
        key.module = e.module;
        key.type = SymbolType::Function;

        auto owner = Trace::Owner::Native;
        if (e.original != nullptr) {
            const auto* rec = shad.FindSymbol(key);
            if (rec == nullptr || rec->virtual_address == 0) {
                SSPC_LOG(Bridge, Error, "observer {}: nothing to observe, not bound", e.name);
                continue;
            }
            *e.original = reinterpret_cast<void*>(rec->virtual_address);
            owner = Trace::Owner::Observed;
        }
        void* fn = e.function;
        if (cfg.import_counter) {
            if (void* thunk = Trace::Wrap(e.nid, e.library, e.module, e.name, owner, fn)) {
                fn = thunk;
            }
        }
        table.AddSymbol(key, reinterpret_cast<u64>(fn));
        SSPC_LOG(Bridge, Info, "bind {:<8} {} ({}/{}) -> SecondSonPC",
                 owner == Trace::Owner::Native ? "NATIVE" : "OBSERVED", e.name, e.library, e.module);
    }

    // 2) shadPS4 HLE: temporary BRIDGE, every function behind the call counter.
    size_t functions = 0;
    size_t objects = 0;
    for (const auto& rec : shad.GetSymbols()) {
        u64 address = rec.virtual_address;
        if (rec.symbol.type == SymbolType::Function) {
            ++functions;
            if (cfg.import_counter && address != 0) {
                if (void* thunk = Trace::Wrap(rec.symbol.name, rec.symbol.library, rec.symbol.module,
                                              NidName(rec.symbol.name), Trace::Owner::Bridge,
                                              reinterpret_cast<void*>(address))) {
                    address = reinterpret_cast<u64>(thunk);
                }
            }
        } else {
            ++objects;
        }
        table.AddSymbol(rec.symbol, address);
    }
    if (cfg.import_counter) {
        Trace::Seal();
    }
    SSPC_LOG(Bridge, Info,
             "binding table: {} SecondSonPC exports, {} shadPS4 HLE functions (BRIDGE) + {} objects, "
             "{} counted thunks",
             exports.size(), functions, objects, Trace::WrappedCount());
}

void RunPhase0Loader(const App::Config& cfg, Core::Linker& linker, const std::string& guest_eboot_path) {
    BuildPhase0BindingTable(linker, cfg);
    if (cfg.import_counter) {
        Trace::StartPeriodicDump(cfg.logs_dir / "calls.json", cfg.dump_interval_s);
    }
    if (!cfg.reloc_snapshot.empty()) {
        // Names for the snapshot only (classification); resolution is shadPS4's.
        static Loader::Bindings names;
        names.Build(SelectExports(cfg, true), BridgeExports(),
                    Loader::BindingOptions{.import_counter = false, .classify_only = true});
        InstallSnapshotHook(cfg,
                            [&cfg, &linker] {
                                std::vector<Loader::SnapshotModule> list;
                                for (s32 i = 0; auto* m = linker.GetModule(i); ++i) {
                                    list.push_back({m->name, HostPathOf(cfg, m->file.generic_string()),
                                                    m->GetBaseAddress(), m->aligned_base_size});
                                }
                                return list;
                            },
                            &names);
    }

    if (linker.LoadModule(guest_eboot_path) == -1) {
        Fatal(fmt::format("failed to load {}", guest_eboot_path));
    }
    SSPC_LOG(Loader, Info, "loaded {} (shadPS4 Core::Linker), starting guest", guest_eboot_path);
    // Core::Linker::Execute's main-thread lambda keeps a reference to `args`: it must outlive us.
    static const std::vector<std::string> args{guest_eboot_path};
    linker.Execute(args);
}

// ------------------------------------------------------------------------------- sspc path

/// Life cycle of the shadPS4 libraries that still serve the game (BRIDGE). shadPS4 has no separate
/// start-up entry point: its RegisterLib functions register symbols *and* start the library,
/// writing file-static state only they can reach. The sspc path resolves imports through its own
/// static table, so it calls RegisterLib only for the start-up, into a throw-away resolver, and only
/// for the libraries below — those whose RegisterLib starts something (docs/MIGRATION.md, "BRIDGE
/// library start-up") and that still have at least one BRIDGE import. shadPS4's InitHLELibs (about
/// 70 libraries, LLE sys_modules lookup, libraries SecondSonPC implements itself) is not used.
/// Order = InitHLELibs order. A library leaves this list when its last import becomes NATIVE.
struct BridgeLibrary {
    std::string_view module; // module column of the import table
    void (*start)(Core::Loader::SymbolsResolver*);
    std::string_view start_up; // what RegisterLib starts besides registering symbols
};

constexpr std::array kBridgeLibraries{
    BridgeLibrary{"libkernel", Libraries::Kernel::RegisterLib,
                  "service thread, process clock, AIO queues, semaphore table, Net kernel hooks"},
    BridgeLibrary{"libSceGnmDriver", Libraries::GnmDriver::RegisterLib,
                  "Liverpool command processor, Vulkan presenter"},
    BridgeLibrary{"libSceVideoOut", Libraries::VideoOut::RegisterLib, "VideoOut driver"},
    BridgeLibrary{"libScePad", Libraries::Pad::RegisterLib,
                  "SDL game controllers (they also log users in)"},
    BridgeLibrary{"libSceNpManager", Libraries::Np::NpManager::RegisterLib,
                  "NP state callback, SDK version"},
};

/// --selftest runs NATIVE and BRIDGE implementations side by side; the shadPS4 side of these reads
/// globals set by RegisterLib (SDK version, ...), so it is started for the comparison only.
constexpr std::array kSelftestLibraries{
    BridgeLibrary{"libSceSystemService", Libraries::SystemService::RegisterLib, "SDK version"},
    BridgeLibrary{"libSceUserService", Libraries::UserService::RegisterLib, "shadNet flag"},
    BridgeLibrary{"libSceAppContentUtil", Libraries::AppContent::RegisterLib, "SDK version"},
    BridgeLibrary{"libScePlayGo", Libraries::PlayGo::RegisterLib, "-"},
};

bool HasBridgeImport(const Loader::Bindings& bindings, std::string_view module) {
    return std::ranges::any_of(bindings.All(), [module](const Loader::Binding& b) {
        return b.owner == Loader::Owner::Bridge && b.info->module == module;
    });
}

void StartBridgeLibraries(const Loader::Bindings& bindings, bool selftest) {
    Core::Loader::SymbolsResolver discarded;
    u32 started = 0;
    const auto start = [&](const BridgeLibrary& lib, const char* why) {
        lib.start(&discarded);
        ++started;
        SSPC_LOG(Bridge, Info, "BRIDGE library started: {} ({}; {})", lib.module, lib.start_up, why);
    };
    for (const auto& lib : kBridgeLibraries) {
        if (HasBridgeImport(bindings, lib.module)) {
            start(lib, "has BRIDGE imports");
        } else {
            SSPC_LOG(Bridge, Info, "BRIDGE library not started: {} (no BRIDGE imports)", lib.module);
        }
    }
    if (selftest) {
        for (const auto& lib : kSelftestLibraries) {
            start(lib, "--selftest comparison");
        }
    }
    SSPC_LOG(Bridge, Info, "{} shadPS4 libraries started ({} shadPS4 symbols discarded, not used "
                           "for resolution); InitHLELibs not used",
             started, discarded.GetSize());
    if (!HasBridgeImport(bindings, "libScePad")) {
        // libScePad is NATIVE: real controllers still come from shadPS4's SDL controller layer,
        // which its (not started) RegisterLib would have opened.
        StartControllers();
        SSPC_LOG(Bridge, Info, "real controllers opened on the bridge's SDL controller layer "
                               "(libScePad is NATIVE)");
    }
}

/// The game's view of the file system (Vfs). /savedata0 is added by the SaveData observer when
/// the game mounts it (SaveData is BRIDGE until Phase 3).
/// P4.14: the start-up shader precompiler's progress over the game window (the settings menu's overlay); true: skip.
bool ShowPrecompileProgress(void*, const Gpu::Native::Options::PrecompileStatus& s) {
    return Ui::ShowStartupProgress(s.done, s.total, s.failed, s.elapsed_s, s.finished);
}

void SetupFilesystem(const App::Config& cfg, const std::string& serial) {
    static std::unique_ptr<Filesystem::FsHost> fs_host = CreateFsHost();
    Filesystem::SetHost(fs_host.get());
    auto& vfs = Filesystem::GetVfs();
    std::error_code ec;
    vfs.Mount("/app0", cfg.game_dir, true);
    vfs.Mount("/hostapp", cfg.game_dir, true);
    const auto temp = cfg.user_data_dir / "temp0" / serial;
    std::filesystem::remove_all(temp, ec); // temp0 is cleared on each boot
    std::filesystem::create_directories(temp, ec);
    vfs.Mount("/temp0", temp, false);
    vfs.Mount("/temp", temp, false);
    const auto download = cfg.user_data_dir / "download0" / serial;
    std::filesystem::create_directories(download, ec);
    vfs.Mount("/download0", download, false);
    const auto data = cfg.user_data_dir / "data";
    std::filesystem::create_directories(data, ec);
    vfs.Mount("/data", data, false);
}

void DiagBenchAt(const App::Config& cfg, const std::string& game, std::string_view point); // (below)

void RunSspcLoader(const App::Config& cfg, const std::string& guest_eboot_path,
                   const std::string& serial, u32 psf_attributes) {
    for (const auto& g : cfg.bridged_groups) {
        if (g == "loader" || g == "sysmodule") {
            Fatal(fmt::format("--bridge {} is not possible with --loader sspc: the shadPS4 "
                              "implementation needs shadPS4's Core::Linker",
                              g));
        }
    }
    for (const auto& g : cfg.native_groups) {
        if (g == "memory" || g == "fs-patch" || g == "cpu" || g == "exceptions") {
            continue; // not exports: the memory manager (P2.3.4), the CPU patcher (P3.3), the exception
                      // dispatcher (P3.4), selected in RunGame
        }
        if (std::ranges::none_of(Runtime::Exports(),
                                 [&g](const Runtime::Export& e) { return e.experimental && g == e.group; })) {
            Fatal(fmt::format("--native {}: SecondSonPC has no experimental group of that name", g));
        }
    }
    // The table decides which shadPS4 libraries are still needed, so it is built first (BRIDGE
    // entries are addresses of shadPS4 functions; nothing has to be started to take them).
    static Loader::Bindings bindings;
    Loader::BindingOptions options;
    options.import_counter = cfg.import_counter && !cfg.selftest;
    options.bridged_groups = cfg.bridged_groups;
    if (!bindings.Build(SelectExports(cfg, false), BridgeExports(), options)) {
        Fatal("binding table is inconsistent, see SecondSonPC.log");
    }
    // The direct memory functions and the bridge's accounting of mapped direct memory switch
    // together, before anything is allocated (the self-test switches them itself).
    if (Listed(cfg.native_groups, "kernel-dmem") && !cfg.selftest && !InstallNativeDirectMemory(cfg)) {
        Fatal("--native kernel-dmem: the bridge's direct memory is already in use, see SecondSonPC.log");
    }
    // P4.1.3: SecondSonPC's GPU memory contract over the selected manager; for the game, the bridge's renderer (its
    // GnmDriver starts below) is recorded as the one owner of the guest GPU — no other backend may take a part of it.
    InstallGpuGuestMemory();
    // P4.3: SecondSonPC's translator interface over the bridge's recompiler (used only by SecondSonPC's frontend:
    // --gpu-shaders, --gpu-shader-verify, the self-test)
    InstallShaderTranslatorAdapter();
    // SecondSonPC's GCN translator for cs,vs,ps (SSPC_OWN_SHADERS=bridge: off); the self-test keeps the bridge's
    // adapter it tests unless SSPC_OWN_SHADERS names stages
    Gpu::Gcn::InstallOwnTranslatorIfEnabled(!cfg.selftest);
    InstallHostShaders(); // P4.4: SecondSonPC's backend's tiling kernels (data only)
    const bool native_gpu = cfg.gpu_backend == App::Config::GpuBackend::Sspc;
    if (!cfg.selftest) {
        // P5.3: the render mode's switches before the GPU backend reads them (legacy: none)
        Engine::Render::NativeDraw::ApplyRenderMode(cfg.render_mode, native_gpu);
        // Native CP: the command processor's mode, also before the backend reads its switches (cp_mode.h)
        Gpu::ApplyCpMode(cfg.cp_mode, native_gpu);
    }
    if (!cfg.selftest) {
        // one owner of the guest GPU, chosen once: the bridge's renderer, or SecondSonPC's backend (it claims every
        // resource itself when it starts, below); never both, never a switch while the game runs
        std::string gpu_error;
        bool gpu_ok = Gpu::SelectBackend(native_gpu ? Gpu::BackendKind::Sspc : Gpu::BackendKind::Bridge, &gpu_error);
        for (u8 r = 0; gpu_ok && !native_gpu && r < static_cast<u8>(Gpu::Resource::Count); ++r) {
            gpu_ok = Gpu::Claim(static_cast<Gpu::Resource>(r), Gpu::BackendKind::Bridge, &gpu_error);
        }
        if (!gpu_ok) {
            Fatal("the guest GPU's owner: " + gpu_error);
        }
    }
    StartBridgeLibraries(bindings, cfg.selftest);
    InstallKernelHost(cfg, psf_attributes);
    InstallSysrtHost(cfg); // R10 sysrt: the bridge's dialogs and sockets beside SecondSonPC's CommonDialog / libSceNet
    if (!HasBridgeImport(bindings, "libScePad")) {
        // Automated runs (--press) inject input; the system cursor must not be grabbed by them.
        InstallNativeInput(cfg, g_window->GetSDLWindow(), !cfg.test_presses.empty());
        // PC settings menu (F1) and the bridge's live settings; off in automated runs and self-tests
        InstallSettingsHost(cfg, g_window->GetSDLWindow(), !cfg.test_presses.empty() || cfg.selftest, native_gpu);
    } else {
        SSPC_LOG(Bridge, Warning, "libScePad is BRIDGE: shadPS4's keyboard layout and controllers");
    }
    SetupFilesystem(cfg, serial);
    if (native_gpu && !cfg.selftest && cfg.gpu_shader_verify.empty()) {
        // P4.4: SecondSonPC's own GPU backend (its GnmDriver and VideoOut are NATIVE: the bridge's renderer, presenter
        // and VideoOut were not started above)
        if (HasBridgeImport(bindings, "libSceGnmDriver") || HasBridgeImport(bindings, "libSceVideoOut")) {
            Fatal("--gpu-backend sspc: GnmDriver/VideoOut still have BRIDGE imports (they switch together)");
        }
        Gpu::Native::Options no;
        no.sdl_window = g_window->GetSDLWindow();
        no.device_index = cfg.gpu_device;
        no.validation = cfg.gpu_native_validate;
        no.present_mode = cfg.present_mode;
        no.frame_limit = cfg.pc.frame_rate_limit;
        no.diagnostics = cfg.gpu_native_diag;
        no.sdk_version = Core::ReadCompiledSdkVersion(guest_eboot_path);
        no.capture_flip = cfg.gpu_capture_flip;
        no.logs_dir = cfg.logs_dir;
        // P4.14: the persistent shader cache and the start-up precompiler ([ShaderCache]; --shader-cache <dir> /
        // --no-shader-cache for test runs)
        no.shader_cache = cfg.shader_cache;
        no.shader_cache_dir = cfg.shader_cache_dir;
        if (!serial.empty()) {
            no.game = serial;
        }
        no.game_dir = cfg.game_dir;
        no.precompile = cfg.pc.shader_precompile;
        if (char v[4] = {}; GetEnvironmentVariableA("SSPC_SHADER_PRECOMPILE", v, sizeof(v)) == 1) {
            no.precompile = v[0] == '1'; // (a diagnostic switch for test runs: 0 off, 1 on)
        }
        no.cache_max_mib = cfg.pc.shader_cache_max_mib;
        no.precompile_threads = cfg.pc.shader_precompile_threads;
        if (cfg.pc.shader_precompile_progress && cfg.test_presses.empty()) {
            no.precompile_progress = &ShowPrecompileProgress;
        }
        DiagBenchAt(cfg, serial, "backend");
        std::string gpu_error;
        if (!Gpu::Native::Start(no, gpu_error)) {
            Fatal("--gpu-backend sspc: SecondSonPC's GPU backend did not start: " + gpu_error);
        }
        Lifecycle::AddExitStep(Lifecycle::Stage::Devices, "native GPU backend", [] { Gpu::Native::Stop(); });
        Gpu::Native::SetFpsCounter(cfg.show_fps); // [Display] showFps (F10 and the settings switch it later)
        {
            // the settings menu: what the upscaling rows can do with this GPU (known now the device exists)
            namespace Cap = Settings::Capability;
            u32 caps = Settings::Instance().Capabilities() | Cap::RenderScale | Cap::FrameLimiter | Cap::FpsCounter |
                       Cap::PresentModeSwitch; // (R11: the vertical sync switches while running)
            if (Gpu::Upscale::GetStatus().dlss_available) {
                caps |= Cap::Dlss;
            }
            if (Gpu::Upscale::GetStatus().fsr_fg_available) {
                caps |= Cap::FsrFrameGen;
            }
            if (Gpu::Upscale::GetStatus().dlss_fg_available) {
                caps |= Cap::DlssFrameGen;
            }
            if (Gpu::Reflex::Available()) {
                caps |= Cap::Reflex;
            }
            Settings::Instance().SetCapabilities(caps);
        }
    }
    bindings.LogSummary(); // also for --selftest: the NATIVE/BRIDGE split without running the game
    if (!cfg.gpu_shader_verify.empty()) {
        // P4.3: a capture of the bridge's translations through SecondSonPC's frontend parts and translator interface
        std::quick_exit(Gpu::Shader::RunShaderVerify(cfg.gpu_shader_verify, cfg.logs_dir));
    }
    if (cfg.selftest) {
        // The self-test compares with the bridge's manager; with --native memory there is none: the native
        // manager's own start-up test runs instead.
        std::quick_exit(Listed(cfg.native_groups, "memory") ? RunNativeMemorySelfTest(cfg) : RunSelfTest(cfg));
    }

    static std::unique_ptr<Loader::Host> host = CreateLoaderHost();
    static Loader::Linker linker(*host, bindings,
                                 [&cfg](std::string_view guest) { return HostPathOf(cfg, guest); });
    if (cfg.import_counter) {
        Trace::StartPeriodicDump(cfg.logs_dir / "calls.json", cfg.dump_interval_s);
    }
    InstallSnapshotHook(cfg,
                        [] {
                            std::vector<Loader::SnapshotModule> list;
                            for (s32 i = 0; auto* m = linker.GetModule(i); ++i) {
                                list.push_back({m->name, m->host_path, m->Base(), m->aligned_base_size});
                            }
                            return list;
                        },
                        &bindings);

    const s32 eboot_index = linker.LoadModule(guest_eboot_path, false);
    if (eboot_index < 0) {
        Fatal(fmt::format("failed to load {}", guest_eboot_path));
    }
    // Native Engine Reconstruction (docs/native-reconstruction): reconstructed engine functions replace the game's
    // own before any guest code runs; each one is off unless its switch is set (SSPC_NATIVE_JOBS=1).
    Engine::SetGuestImage(linker.GetModule(eboot_index)->Base(), linker.GetModule(eboot_index)->aligned_base_size);
    Engine::GameMenu::StartMemorySnapshotsFromEnvironment(linker.GetModule(eboot_index)->Base(),
                                                         linker.GetModule(eboot_index)->aligned_base_size);
    // PC Edition: the game's own settings screen (Esc) leads to the PC settings (docs/PC_EDITION_UI.md)
    Engine::GameMenu::Install(linker.GetModule(eboot_index)->Base());
    // races in the game's own code the PC's timing exposes (docs/GAME_FIXES.md)
    Engine::GameFixes::Install(linker.GetModule(eboot_index)->Base());
    Engine::MouseCamera::Install(linker.GetModule(eboot_index)->Base());
    Engine::Jobs::InstallFromEnvironment(linker.GetModule(eboot_index)->Base());
    Engine::ClockProbe::StartFromEnvironment(linker.GetModule(eboot_index)->Base(), cfg.logs_dir); // (FrameBreak FB-3)
    Gpu::R8::Verify::Frame::InstallFromEnvironment(linker.GetModule(eboot_index)->Base(), cfg.logs_dir); // (R8-V3)
    Engine::Render::Trace::InstallFromEnvironment(linker.GetModule(eboot_index)->Base());
    Engine::Render::DrawCensus::InstallFromEnvironment(linker.GetModule(eboot_index)->Base()); // (CITY-OPT diagnostics)
    Engine::Render::LightCensus::InstallFromEnvironment(linker.GetModule(eboot_index)->Base()); // (R12 diagnostics)
    Engine::Render::Trace::InstallLifecycleFromEnvironment(linker.GetModule(eboot_index)->Base());
    Engine::Render::Trace::InstallCriticalComputeFromEnvironment(linker.GetModule(eboot_index)->Base());
    if (native_gpu) { // (SecondSonPC's Gnm driver hands the frame on: the bridge's has no such source)
        Engine::Render::Trace::InstallEarlySubmitFromEnvironment(linker.GetModule(eboot_index)->Base());
        // (REA-3: the gate goes through SecondSonPC's Gnm driver too)
        Engine::Render::Trace::InstallDeferComputeWaitFromEnvironment(linker.GetModule(eboot_index)->Base());
        // (R11: the labels' wake-ups come from SecondSonPC's command processor)
        Engine::Render::Trace::InstallLabelWaitFromEnvironment(linker.GetModule(eboot_index)->Base());
    } else if (char es[4] = {}; GetEnvironmentVariableA("SSPC_P54_EARLY_SUBMIT", es, sizeof(es)) == 1 && es[0] == '1') {
        SSPC_LOG(Loader, Warning, "SSPC_P54_EARLY_SUBMIT=1 ignored: only SecondSonPC's GPU backend (--gpu-backend "
                 "sspc) hands frames on early");
    }
    Engine::Render::NativeDraw::InstallFromEnvironment(linker.GetModule(eboot_index)->Base(), native_gpu);
    // CITY-OPT Phase 5 (SSPC_CITY_ENGINE_OPT, off by default): the engine's mesh/shadow draws tagged for the processor's runs
    Engine::Render::DrawRuns::InstallFromEnvironment(linker.GetModule(eboot_index)->Base(), native_gpu);
    // Smoke Quality and Particle Resolution (both backends): the game's own unless [Graphics] smokeQuality /
    // particleResolution (SSPC_SMOKE_QUALITY / SSPC_PARTICLE_RES).
    Engine::Particles::Install(linker.GetModule(eboot_index)->Base(), cfg.pc.smoke_quality,
                               cfg.pc.particle_resolution);
    // R12 weather (diagnostics, SSPC_R12_WEATHER only): the stand's reproducible weather states (docs/R12_WEATHER.md)
    Engine::Render::Weather::InstallFromEnvironment(linker.GetModule(eboot_index)->Base(), cfg.logs_dir);
    // R11 post effects (both backends): the game's own unless [Graphics] motionBlur / motionBlurStrength (SSPC_POSTFX_*;
    // film grain and depth of field only SSPC_POSTFX_FILM_GRAIN / SSPC_POSTFX_DOF; docs/R11_POSTFX.md)
    Engine::Render::PostFx::Install(linker.GetModule(eboot_index)->Base(),
                                    {.motion_blur = cfg.pc.motion_blur,
                                     .motion_blur_strength = cfg.pc.motion_blur_strength});
    // R12 reflections (both backends; docs/R12_REFLECTIONS.md): the game's own unless [Graphics] reflectionQuality
    // (SSPC_REFLECTION_QUALITY); diagnostics SSPC_R12_REFL_STATS / _DIAG
    Engine::Render::Reflections::Install(linker.GetModule(eboot_index)->Base(), cfg.pc.reflection_quality);
    // R12 rain shadows (both backends; docs/R12_RAIN_SHADOWS.md): the game's own unless [Graphics] rainShadows
    // (SSPC_RAIN_SHADOWS); diagnostics SSPC_R12_RAIN_STATS / _DIST / _HOOK
    Engine::Render::RainShadows::Install(linker.GetModule(eboot_index)->Base(), cfg.pc.rain_shadows);
    // Upscaling (docs/UPSCALING_FRAME_GENERATION.md): with DLSS / FSR the game's scene renders at the quality's render
    // size (the engine's own render size, written before it starts); the display and the UI keep the plan's size.
    {
        Gpu::Upscale::ApplyEnvironmentOverrides();
        // the game starts at the native size (its targets' memory is allocated for it at start; a smaller size is
        // switched to at its first frame and back whenever the menu changes - SSPC_RENDER_RES=WxH: a fixed start size,
        // diagnostics)
        // the display, screen and scene sizes from main.cpp's plan (docs/RESOLUTION_IMPLEMENTATION.md; the PS4's
        // 1920x1080 on the bridge's GPU backend)
        Engine::Render::InstallRenderResolution(linker.GetModule(eboot_index)->Base());
        if (native_gpu) {
            Engine::Render::SetRenderScale(Engine::Render::ParseRenderScale(cfg.pc.render_scale));
            Engine::Render::SetHudLayout(cfg.pc.hud_safe_area, cfg.pc.hud_area);
            Engine::Render::SetFovPercent(cfg.pc.fov);
            Engine::Render::InstallFovControl(linker.GetModule(eboot_index)->Base());
            Engine::Render::InstallTemporalAaControl(linker.GetModule(eboot_index)->Base());
            Engine::Render::InstallRenderSwitch(linker.GetModule(eboot_index)->Base());
            Bridge::ApplyUpscaling(true);
        } else if (Gpu::Upscale::GetSettings().backend != Gpu::Upscale::Backend::Off) {
            SSPC_LOG(Loader, Warning, "Upscaling ({}) needs SecondSonPC's GPU backend (--gpu-backend sspc): off",
                     Gpu::Upscale::BackendName(Gpu::Upscale::GetSettings().backend));
            Gpu::Upscale::Settings us = Gpu::Upscale::GetSettings();
            us.backend = Gpu::Upscale::Backend::Off;
            Gpu::Upscale::SetSettings(us);
        }
    }
    // Native Engine Reconstruction (docs/NATIVE_ENGINE_RECONSTRUCTION.md; SSPC_NATIVE_ENGINE, off by default):
    // reconstructed engine functions replace the game's own. Installed last, so a function another switch already
    // hooked no longer hashes and is left alone.
    NativeEngine::InstallFromEnvironment(linker.GetModule(eboot_index)->Base(), cfg.logs_dir);
    SSPC_LOG(Loader, Info, "loaded {} (SecondSonPC Loader), starting guest", guest_eboot_path);
    linker.Execute({guest_eboot_path}, [] { Runtime::Sysmodule::PreloadForLibkernel(); }, {});
}

/// --frame-log (diagnostics, tools/perf_runs.ps1): one row per presented frame -> <logs>\frametimes.csv
/// ("t_s,frame_ms": seconds since start, time since the previous frame). The bridge's flip counter
/// (DebugState, raised after vkQueuePresent) is polled every millisecond by a high-resolution
/// waitable timer, so the system timer resolution stays as it is; frames between two polls share
/// the interval. Rows are flushed in batches: the last few frames before quick_exit may be lost.
void StartFrameLog(const std::filesystem::path& file) {
    std::thread([file] {
        Common::SetCurrentThreadName("SecondSonPC:FrameLog");
        std::ofstream out(file, std::ios::trunc);
        if (!out) {
            SSPC_LOG(App, Error, "--frame-log: cannot write {}", file.string());
            return;
        }
        // --track-stats (P2.5): the GPU write tracking of each frame (deltas; frames between two
        // polls share them evenly).
        const bool track = TrackDiag::Enabled();
        const double tpus = TrackDiag::TicksPerMicrosecond();
        TrackDiag::Totals tlast = TrackDiag::Take();
        out << (track ? "t_s,frame_ms,faults,fault_us,chain_us,protect_track,protect_untrack,pages_track,"
                        "pages_untrack,protect_us,uploads,upload_kib,upload_us,readbacks,readback_kib,"
                        "readback_us,flushes,tex_images\n"
                      : "t_s,frame_ms\n");
        const HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        SSPC_LOG(App, Info, "--frame-log: presented frames -> {} ({})", file.string(),
                 timer != nullptr ? "1 ms high-resolution timer" : "Sleep(1) fallback");
        LARGE_INTEGER freq{}, start{}, last{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        u32 frames = DebugState.GetFrameNum();
        u64 rows = 0;
        for (;;) {
            if (timer != nullptr) {
                LARGE_INTEGER due{};
                due.QuadPart = -10000; // 1 ms, relative
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
            const u32 now_frames = DebugState.GetFrameNum();
            if (now_frames == frames) {
                continue;
            }
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (last.QuadPart != 0) {
                const double t = double(now.QuadPart - start.QuadPart) / freq.QuadPart;
                const double ms = double(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart / (now_frames - frames);
                std::string extra;
                if (track) {
                    const TrackDiag::Totals tn = TrackDiag::Take();
                    const double n = now_frames - frames;
                    const auto per = [n](u64 a, u64 b) { return double(a - b) / n; };
                    const auto us = [n, tpus](u64 a, u64 b) { return double(a - b) / tpus / n; };
                    extra = fmt::format(
                        ",{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}",
                        per(tn.faults, tlast.faults), us(tn.fault_ticks, tlast.fault_ticks),
                        us(tn.chain_ticks, tlast.chain_ticks), per(tn.protect_track, tlast.protect_track),
                        per(tn.protect_untrack, tlast.protect_untrack), per(tn.pages_track, tlast.pages_track),
                        per(tn.pages_untrack, tlast.pages_untrack), us(tn.protect_ticks, tlast.protect_ticks),
                        per(tn.uploads, tlast.uploads), per(tn.upload_bytes, tlast.upload_bytes) / 1024.0,
                        us(tn.upload_ticks, tlast.upload_ticks), per(tn.readbacks, tlast.readbacks),
                        per(tn.readback_bytes, tlast.readback_bytes) / 1024.0,
                        us(tn.readback_ticks, tlast.readback_ticks), per(tn.flushes, tlast.flushes),
                        per(tn.tex_images, tlast.tex_images));
                    tlast = tn;
                }
                for (u32 i = frames; i != now_frames; ++i) {
                    out << fmt::format("{:.3f},{:.3f}{}\n", t, ms, extra);
                }
                if ((rows += now_frames - frames) % 32 < now_frames - frames) {
                    out.flush();
                }
            } else if (track) {
                tlast = TrackDiag::Take(); // the first frame has no row: start the deltas here
            }
            last = now;
            frames = now_frames;
        }
    }).detach();
}

/// --press: input for automated runs (tools/smoke_runs.ps1). Comma-separated "what@seconds[:hold]",
/// timed from the window opening; the events go into the window's SDL event queue (the window
/// does not need focus) — the events a real keyboard and mouse produce:
///   <SDL key name>    key down, `hold` s later (default 0.15) key up, by scancode (space, w, g,
///                     return, ...). Layouts: --loader sspc = SecondSonPC's (space = cross),
///                     --loader shadps4 = the bridge's (n = cross).
///   mouse_left | mouse_right | mouse_middle   the same for a mouse button
///   move:<dx>:<dy>    relative mouse movement spread evenly over `hold` s (10 ms steps)
///   shot              one frame of the game -> <logs>\shots\<seconds>s.png (bridge screenshot)
///   mark:<label>      R9 gameplay profiler (SSPC_R9_GPROF): a marked moment (the phase of a scripted fight/flight)
void StartTestInput(const std::string& spec, const std::filesystem::path& logs_dir) {
    enum class Kind { KeyDown, KeyUp, ButtonDown, ButtonUp, Move, Shot, Mark };
    struct Event {
        double at;
        Kind kind;
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        u8 button = 0;
        float dx = 0.0f, dy = 0.0f;
        std::string label;
    };
    std::vector<Event> timeline;
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        const std::string token = spec.substr(pos, end - pos);
        pos = end + 1;
        const size_t at_pos = token.find('@');
        if (at_pos == std::string::npos) {
            Fatal(fmt::format("--press: '{}' is not what@seconds[:hold]", token));
        }
        const std::string what = token.substr(0, at_pos);
        char* rest = nullptr;
        const double at = std::strtod(token.c_str() + at_pos + 1, &rest);
        const bool has_hold = rest != nullptr && *rest == ':';
        const double hold = has_hold ? std::strtod(rest + 1, nullptr) : 0.15;
        if (what == "shot") {
            timeline.push_back({at, Kind::Shot, {}, 0, 0, 0, fmt::format("{:g}s", at)});
        } else if (what.starts_with("mark:")) {
            timeline.push_back({at, Kind::Mark, {}, 0, 0, 0, what.substr(5)});
        } else if (what.starts_with("move:")) {
            float dx = 0.0f, dy = 0.0f;
            if (std::sscanf(what.c_str(), "move:%f:%f", &dx, &dy) != 2) {
                Fatal(fmt::format("--press: '{}' is not move:<dx>:<dy>", what));
            }
            const double span = has_hold ? hold : 0.0;
            const int steps = std::max(1, static_cast<int>(std::lround(span / 0.01)));
            for (int i = 0; i < steps; ++i) {
                timeline.push_back({at + span * i / steps, Kind::Move, {}, 0, dx / steps, dy / steps,
                                    i == 0 ? what : ""});
            }
        } else if (what == "mouse_left" || what == "mouse_right" || what == "mouse_middle") {
            const u8 b = what == "mouse_left" ? SDL_BUTTON_LEFT
                                              : (what == "mouse_right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE);
            timeline.push_back({at, Kind::ButtonDown, {}, b, 0, 0, what});
            timeline.push_back({at + hold, Kind::ButtonUp, {}, b, 0, 0, ""});
        } else {
            const SDL_Scancode sc = SDL_GetScancodeFromName(what.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                Fatal(fmt::format("--press: '{}' is not an SDL key name", what));
            }
            timeline.push_back({at, Kind::KeyDown, sc, 0, 0, 0, what});
            timeline.push_back({at + hold, Kind::KeyUp, sc, 0, 0, 0, ""});
        }
    }
    std::ranges::stable_sort(timeline, {}, &Event::at);
    const auto shots = logs_dir / "shots";
    // R8 deterministic mode (SSPC_R8_VERIFY_DET_ANCHOR, runtime/gpu/r8/r8_verify_frame.h): the events from the anchor
    // on are due at world frames, not at wall times
    const double anchor = Gpu::R8::Verify::Frame::Anchor();
    Gpu::R8::Verify::Frame::ScriptStart(
        static_cast<u32>(std::ranges::count_if(timeline, [&](const Event& ev) { return anchor >= 0 && ev.at >= anchor; })));
    std::thread([timeline, shots, anchor] {
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t idx = 0; idx < timeline.size(); ++idx) {
            const auto& ev = timeline[idx];
            const bool by_frame = anchor >= 0 && ev.at >= anchor;
            // (P4.14: the time the start waited for the shader precompiler moves the script with the game - read
            // until it is due: the wait ends after the script started)
            while (true) {
                const auto due = t0 + std::chrono::duration<double>(by_frame ? anchor : ev.at) +
                                 std::chrono::nanoseconds(Gpu::Native::StartupPauseNs());
                const auto now = std::chrono::steady_clock::now();
                if (now >= due) {
                    break;
                }
                const auto left = std::chrono::duration_cast<std::chrono::microseconds>(due - now);
                std::this_thread::sleep_for(std::min(left, std::chrono::microseconds(50'000)));
            }
            if (by_frame) {
                Gpu::R8::Verify::Frame::WaitWorldFrame(ev.at);
            }
            SDL_Event e{};
            switch (ev.kind) {
            case Kind::KeyDown:
            case Kind::KeyUp:
                e.type = ev.kind == Kind::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.timestamp = SDL_GetTicksNS();
                e.key.which = Input::KeyboardMouse::kScriptedInputId;
                e.key.scancode = ev.scancode;
                e.key.key = SDL_GetKeyFromScancode(ev.scancode, SDL_KMOD_NONE, false);
                e.key.down = ev.kind == Kind::KeyDown;
                if (ev.kind == Kind::KeyDown) {
                    Gpu::R10::Latency::Input(1, 0.0f, 0.0f); // (SSPC_R10_LATENCY: the moment it is pushed)
                }
                SDL_PushEvent(&e);
                break;
            case Kind::ButtonDown:
            case Kind::ButtonUp:
                e.type = ev.kind == Kind::ButtonDown ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                     : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.timestamp = SDL_GetTicksNS();
                e.button.which = Input::KeyboardMouse::kScriptedInputId;
                e.button.button = ev.button;
                e.button.down = ev.kind == Kind::ButtonDown;
                e.button.clicks = 1;
                if (ev.kind == Kind::ButtonDown) {
                    Gpu::R10::Latency::Input(2, 0.0f, 0.0f);
                }
                SDL_PushEvent(&e);
                break;
            case Kind::Move:
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.timestamp = SDL_GetTicksNS();
                e.motion.which = Input::KeyboardMouse::kScriptedInputId;
                e.motion.xrel = ev.dx;
                e.motion.yrel = ev.dy;
                Gpu::R10::Latency::Input(3, ev.dx, ev.dy);
                SDL_PushEvent(&e);
                break;
            case Kind::Mark:
                Gpu::Gprof::Mark(ev.label);
                break;
            case Kind::Shot: {
                // The bridge saves the next presented frame into its screenshots folder; the new
                // file is copied next to the logs under the requested time once it is complete.
                // The bridge's file is left alone: right after writing it the bridge reads it
                // back for its on-screen notification, and its texture manager crashes on an
                // empty image (no check of stbi's result) — removing it here caused exactly that.
                const auto dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
                std::error_code ec;
                std::vector<std::filesystem::path> before;
                for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
                    before.push_back(f.path());
                }
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
                std::thread([before, dir, shots, label = ev.label] {
                    for (int i = 0; i < 100; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        std::error_code ec2;
                        for (const auto& f : std::filesystem::directory_iterator(dir, ec2)) {
                            if (f.path().extension() != ".png" ||
                                std::ranges::find(before, f.path()) != before.end()) {
                                continue;
                            }
                            // Complete = non-empty and the same size half a second later.
                            u64 size = 0;
                            for (int k = 0; k < 20; ++k) {
                                const u64 now_size = std::filesystem::file_size(f.path(), ec2);
                                if (!ec2 && now_size != 0 && now_size == size) {
                                    break;
                                }
                                size = ec2 ? 0 : now_size;
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            }
                            std::filesystem::create_directories(shots, ec2);
                            const auto target = shots / (label + ".png");
                            std::filesystem::copy_file(f.path(), target,
                                                       std::filesystem::copy_options::overwrite_existing, ec2);
                            SSPC_LOG(App, Info, "test input: frame at {} -> {} ({} bytes)", label,
                                     target.string(), size);
                            return;
                        }
                    }
                    SSPC_LOG(App, Warning, "test input: frame at {} was not saved", label);
                }).detach();
                break;
            }
            }
            if (by_frame) {
                // the window's queue took the event (the main thread pumps it into the input state) before the
                // world frame it is due in goes on
                for (int k = 0; k < 500 && SDL_HasEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL); ++k) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                Gpu::R8::Verify::Frame::Delivered(idx + 1 < timeline.size() ? timeline[idx + 1].at : -1.0);
            }
            if (!ev.label.empty() && ev.kind != Kind::Shot) {
                SSPC_LOG(App, Info, "test input: '{}' at {:.2f} s{}", ev.label, ev.at,
                         by_frame ? fmt::format(" (world frame {})", Gpu::R8::Verify::Frame::WorldFrame())
                                  : std::string());
            }
        }
    }).detach();
}

/// Title metadata from /app0/sce_sys.
struct GameMetadata {
    std::string id;
    std::string title = "Unknown title";
    std::string app_version = "Unknown version";
    u32 fw_version = 0x4700000;
    u32 attributes = 0;
    std::vector<u8> splash; // pic1.png
    std::vector<u8> icon;   // icon0.png
};

std::vector<u8> ReadHostFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// sspc path: SecondSonPC's PSF reader on the host files behind /app0 (no shadPS4 mount table).
GameMetadata ReadGameMetadata(const std::filesystem::path& game_dir) {
    const auto sce_sys = game_dir / "sce_sys";
    Psf::ParamSfo sfo;
    std::string error;
    if (!sfo.Load(sce_sys / "param.sfo", error)) {
        Fatal(fmt::format("cannot read /app0/sce_sys/param.sfo: {}", error));
    }
    GameMetadata m;
    const auto content_id = sfo.GetString("CONTENT_ID");
    if (content_id.has_value() && content_id->size() > 7) {
        m.id = content_id->substr(7, 9);
    } else if (const auto title_id = sfo.GetString("TITLE_ID")) {
        m.id = *title_id;
    }
    m.title = sfo.GetString("TITLE").value_or(m.title);
    m.fw_version = sfo.GetInteger("SYSTEM_VER").value_or(m.fw_version);
    m.app_version = sfo.GetString("APP_VER").value_or(m.app_version);
    m.attributes = sfo.GetInteger("ATTRIBUTE").value_or(0);
    m.splash = ReadHostFile(sce_sys / "pic1.png");
    m.icon = ReadHostFile(sce_sys / "icon0.png");
    SSPC_LOG(Loader, Info, "param.sfo: {} entries (SecondSonPC reader)", sfo.Entries().size());

    // BRIDGE SaveData reads INSTALL_DIR_SAVEDATA from shadPS4's PSF singleton (savedata.cpp); it
    // is filled from the same host file, without the mount table, until SaveData is NATIVE.
    if (!Common::Singleton<PSF>::Instance()->Open(sce_sys / "param.sfo")) {
        Fatal("shadPS4's PSF cannot parse /app0/sce_sys/param.sfo (needed by BRIDGE SaveData)");
    }
    return m;
}

/// shadps4 path (oracle), unchanged: shadPS4's mount table and PSF.
GameMetadata ReadGameMetadataShad(Core::FileSys::MntPoints& mnt) {
    GameMetadata m;
    if (auto psf_handle = mnt.Open("/app0/sce_sys/param.sfo", /*writable=*/false)) {
        auto* param_sfo = Common::Singleton<PSF>::Instance();
        if (!param_sfo->Open(psf_handle)) {
            Fatal("cannot parse /app0/sce_sys/param.sfo");
        }
        const auto content_id = param_sfo->GetString("CONTENT_ID");
        const auto title_id = param_sfo->GetString("TITLE_ID");
        if (content_id.has_value() && !content_id->empty()) {
            m.id = std::string(*content_id, 7, 9);
        } else if (title_id.has_value()) {
            m.id = *title_id;
        }
        m.title = param_sfo->GetString("TITLE").value_or(m.title);
        m.fw_version = param_sfo->GetInteger("SYSTEM_VER").value_or(m.fw_version);
        m.app_version = param_sfo->GetString("APP_VER").value_or(m.app_version);
        if (const auto raw_attributes = param_sfo->GetInteger("ATTRIBUTE")) {
            m.attributes = *raw_attributes;
        }
    } else {
        Fatal("/app0/sce_sys/param.sfo not found");
    }
    if (auto splash = mnt.ReadFile("/app0/sce_sys/pic1.png")) {
        m.splash = std::move(*splash);
    }
    if (auto icon = mnt.ReadFile("/app0/sce_sys/icon0.png")) {
        m.icon = std::move(*icon);
    }
    return m;
}

} // namespace

/// --predict-shaders <templates> (docs/REA_ENGINE_REVERSE.md §8): the material draws' modules predicted from the packs
/// and translated into the native backend's shader cache, before any guest memory or game code exists.
int RunPredictShaders(const App::Config& cfg, const std::string& game) {
    InstallShaderTranslatorAdapter();
    // the modules prepared by the own translator (SSPC_OWN_SHADERS=bridge: the bridge's); a program it declines is not
    // predicted - the bridge's recompiler would stop this process on an ASSERT the predicted state trips (the game
    // translates such a program itself, with its real state)
    Gpu::Gcn::InstallOwnTranslatorIfEnabled(true, false);
    Gpu::Native::PredictShadersOptions po;
    po.device_index = cfg.gpu_device;
    po.game_dir = cfg.game_dir;
    po.templates = cfg.predict_shaders;
    po.shader_cache_dir = cfg.shader_cache_dir;
    po.game = game;
    po.cache_max_mib = cfg.pc.shader_cache_max_mib;
    po.threads = cfg.pc.shader_precompile_threads;
    po.logs_dir = cfg.logs_dir;
    po.if_needed = cfg.predict_if_needed;
    bool up_to_date = false;
    po.up_to_date = &up_to_date;
    // the console shows where it is: a line per phase, rewritten in place five times a second
    Gpu::Static::PredictProgress progress;
    po.progress = &progress;
    SetConsoleTitleW(L"SecondSonPC — подготовка шейдеров");
    std::printf("SecondSonPC: подготовка шейдеров игры (все шейдеры из файлов игры компилируются до её запуска)\n");
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> stop{false};
    std::thread printer([&] {
        static constexpr const char* kPhase[] = {"запуск",
                                                 "чтение файлов игры",
                                                 "пиксельные шейдеры",
                                                 "вершинные шейдеры",
                                                 "вычислительные шейдеры",
                                                 "шейдеры конвейеров",
                                                 "компиляция конвейеров драйвером",
                                                 "готово"};
        int shown = -1;
        for (u32 tick = 0; !stop.load(); ++tick) {
            const int phase = progress.phase.load();
            const u64 done = progress.done.load(), total = progress.total.load();
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (phase != shown && shown > 0) {
                std::printf("\n"); // the finished phase's line stays
            }
            shown = phase;
            // the phase's name padded to 34 characters (UTF-8: a Cyrillic letter is two bytes)
            std::string name = kPhase[std::clamp(phase, 0, 7)];
            size_t chars = 0;
            for (const char ch : name) {
                chars += (static_cast<u8>(ch) & 0xC0) != 0x80 ? 1 : 0;
            }
            name.append(chars < 34 ? 34 - chars : 0, ' ');
            if (total == 0) {
                static constexpr char kSpin[] = {'|', '/', '-', '\\'};
                std::printf("\r  %s %c   %6.0f с          ", name.c_str(), kSpin[tick % 4], s);
            } else {
                const double f = static_cast<double>(std::min(done, total)) / static_cast<double>(total);
                char bar[31];
                const int fill = static_cast<int>(f * 30.0);
                for (int i = 0; i < 30; ++i) {
                    bar[i] = i < fill ? '#' : '.';
                }
                bar[30] = '\0';
                std::printf("\r  %s [%s] %5.1f%%  %llu / %llu   %6.0f с   ", name.c_str(), bar, f * 100.0,
                            static_cast<unsigned long long>(done), static_cast<unsigned long long>(total), s);
            }
            std::fflush(stdout);
            Sleep(200);
        }
    });
    std::string summary;
    const bool ok = Gpu::Native::PredictShaders(po, summary);
    stop.store(true);
    printer.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (up_to_date) {
        std::printf("\r%-110s\rSecondSonPC: все шейдеры уже скомпилированы - запуск игры\n", "");
    } else {
        std::printf("\n\nSecondSonPC: %s\n\n", summary.c_str());
        std::printf(ok ? "SecondSonPC: шейдеры скомпилированы за %.0f с - запуск игры\n"
                       : "SecondSonPC: подготовка шейдеров не удалась (%.0f с) - игра запустится как раньше\n",
                    s);
    }
    std::fflush(stdout);
    if (cfg.predict_if_needed) {
        Sleep(up_to_date ? 700 : 3000); // (the window of the start: readable before it closes)
    }
    return ok ? 0 : 1;
}

namespace {
/// SSPC_DIAG_BENCH_AT=memory|backend (R9-N, docs/R9_NEON_DASH.md): the pipeline bench (SSPC_PIPE_BENCH=...,
/// native_backend.cpp PipelineBench) run in the game's own process at that point of its start - after the guest's
/// memory is reserved, or just before the GPU backend starts - then the process ends: which step of the start makes the
/// driver miss its pipeline cache file (the bench of --predict-shaders, earlier in RunGame, hits it)
void DiagBenchAt(const App::Config& cfg, const std::string& game, std::string_view point) {
    char v[16] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_BENCH_AT", v, sizeof(v)) == 0 || point != v) {
        return;
    }
    if (point == "memory") { // (RunSspcLoader installs them later)
        InstallShaderTranslatorAdapter();
        Gpu::Gcn::InstallOwnTranslatorIfEnabled(true);
    }
    Gpu::Native::PredictShadersOptions po;
    po.device_index = cfg.gpu_device;
    po.game_dir = cfg.game_dir;
    po.templates = cfg.predict_shaders;
    po.shader_cache_dir = cfg.shader_cache_dir;
    po.game = game;
    po.cache_max_mib = cfg.pc.shader_cache_max_mib;
    po.threads = cfg.pc.shader_precompile_threads;
    po.logs_dir = cfg.logs_dir;
    std::string summary;
    Gpu::Native::PredictShaders(po, summary);
    SSPC_LOG(Bridge, Warning, "DIAGNOSTIC: SSPC_DIAG_BENCH_AT={} - {}", point, summary);
    std::fflush(nullptr);
    std::quick_exit(0);
}
} // namespace

void RunGame(const App::Config& cfg) {
    const bool own_loader = cfg.loader == LoaderBackend::Sspc;
    Common::SetCurrentThreadName("SecondSonPC:Main");

    // Equivalent of Core::Emulator::Emulator().
    Common::NtApi::Initialize();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InstallLifecycle(); // P3.6: one owner of the process's end, before any bridge code may assert
    Trace::SetCallOrderCapacity(cfg.call_order);

    // The shadPS4 core keeps its own log file; SecondSonPC.log is written by SecondSonPC only.
    std::error_code ec;
    std::filesystem::create_directories(cfg.logs_dir, ec);
    Common::FS::SetUserPath(Common::FS::PathType::LogDir, cfg.logs_dir);
    Common::Log::Setup("bridge_shadps4.log");

    const auto& shad_user = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    if (!IsUnder(shad_user, cfg.root)) {
        Fatal(fmt::format("shadPS4 core user dir {} is outside {} — refusing to run",
                          shad_user.string(), cfg.root.string()));
    }
    SSPC_LOG(Bridge, Info, "shadPS4 core {} (temporary bridge), user dir {}", Common::g_scm_rev,
             shad_user.string());

    if (own_loader) {
        // The bridge's users (controller slots, save/trophy folders by user id) live in memory
        // only: the four default users 1000..1003, the ids SecondSonPC's UserService uses.
        UserManagement.GetUsers() = UserManagement.CreateDefaultUsers();
        SSPC_LOG(Bridge, Info,
                 "bridge configuration from SecondSonPC.toml only; config.json, custom_configs, "
                 "users.json and keys.json in {} are not read",
                 shad_user.string());
    } else {
        // Phase 0 path (oracle): shadPS4's own files, then SecondSonPC.toml on top (below).
        KeyManager::GetInstance()->LoadFromFile();
        EmulatorSettings.Load();
        UserSettings.Load();
    }

    // /app0 in shadPS4's mount table: on the shadps4 path it is the game's file system; on the sspc
    // path only BRIDGE code reads through it (SaveData: /app0/sce_sys/save_data.png; the trophy
    // table below). The game's own file access on the sspc path goes through SecondSonPC's Vfs.
    const auto& game_folder = cfg.game_dir;
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    mnt->Mount(game_folder, "/app0", true);
    mnt->Mount(game_folder, "/hostapp", true);

    const GameMetadata meta = own_loader ? ReadGameMetadata(game_folder) : ReadGameMetadataShad(*mnt);
    const std::string& id = meta.id;
    const std::string& title = meta.title;
    const std::string& app_version = meta.app_version;
    const u32 fw_version = meta.fw_version;
    Common::PSFAttributes psf_attributes{};
    psf_attributes.raw = meta.attributes;

    if (!own_loader) {
        // Per-game overrides of the Phase 0 path (custom_configs\<id>.json, normally absent).
        EmulatorSettings.Load(id);
    }
    std::filesystem::create_directories(cfg.user_data_dir / "home", ec);
    ApplyBridgeSettings(cfg);
    Common::Log::Switch("bridge_shadps4.log", false); // applies the filter
    // P3.3: SecondSonPC's TCB slot (the bridge's TLS code is pointed at it) and the FS patcher, before
    // any thread, trampoline or module exists.
    if (std::string error; own_loader && !InstallGuestCpu(cfg, &error)) {
        Fatal(error);
    }
    // P3.5: the guest errno (and the process clock) SecondSonPC's with --native errno / clock, before any bridge
    // library starts (libkernel's RegisterLib starts the bridge's clock) and before any guest thread.
    if (std::string error; own_loader && !InstallKernelRuntime(cfg, &error)) {
        Fatal(error);
    }
    if (cfg.patch_race_test) {
        // Before the game window exists: a window that pumps no messages for half a minute looks hung.
        std::quick_exit(RunPatchRaceTest(cfg));
    }
    if (!cfg.predict_shaders.empty()) {
        std::quick_exit(RunPredictShaders(cfg, id));
    }

    SSPC_LOG(Bridge, Info, "game {} \"{}\" v{} fw {:#x}", id, title, app_version, fw_version);
    {
        // SecondSonPC's SaveData (group "savedata"): the same folders as the bridge's below.
        ::SecondSon::SaveData::Settings s;
        s.home = cfg.user_data_dir / "home";
        s.title_dir = id;
        ::SecondSon::Psf::ParamSfo game_sfo;
        std::string sfo_error;
        if (game_sfo.Load(game_folder / "sce_sys" / "param.sfo", sfo_error)) {
            s.title_dir = game_sfo.GetString("INSTALL_DIR_SAVEDATA").value_or(id);
        }
        s.firmware = fw_version & 0xFFF00000u;
        s.language_id = cfg.language_id;
        s.icon = game_folder / "sce_sys" / "save_data.png";
        ::SecondSon::SaveData::Configure(s);
    }
    SSPC_LOG(SaveData, Info, "/savedata0 root: {}\\<user>\\savedata\\{}\\<dir>",
             (cfg.user_data_dir / "home").string(), id);

    const std::string guest_eboot_path = "/app0/eboot.bin";
    Common::SecondSonBootstrap::GameInfo info{
        .serial = id,
        .title = title,
        .app_version = app_version,
        .firmware = fw_version,
        .sdk = own_loader ? static_cast<u32>(Loader::ReadSdkVersion(game_folder / "eboot.bin"))
                          : static_cast<u32>(Core::ReadCompiledSdkVersion(guest_eboot_path)),
        .attributes = psf_attributes,
        .splash = meta.splash,
        .folder = game_folder,
    };
    SSPC_LOG(Loader, Info, "eboot SDK version {:#x}", info.sdk);
    Common::SecondSonBootstrap::Fill(std::move(info));

    Common::Singleton<Core::FileSys::HandleTable>::Instance()->CreateStdHandles();

    // Upscaling: DLSS needs the driver's CUDA part, which needs address space under 1 TiB - given to it before the guest
    // memory takes all of it (docs/UPSCALING_FRAME_GENERATION.md)
    if (cfg.gpu_backend == App::Config::GpuBackend::Sspc) {
        Gpu::Upscale::ApplyConfig(cfg.pc.upscaler, cfg.pc.upscale_quality, cfg.pc.upscale_sharpness,
                                  cfg.pc.frame_generation, cfg.pc.frame_gen_multiplier, cfg.pc.reflex);
        Gpu::Upscale::ApplyEnvironmentOverrides();
        Gpu::Upscale::ReserveCudaWindow(); // (DLSS may be chosen in the menu while the game runs)
    }
    // The guest memory's manager, chosen before anything reaches it (patch 0012): the bridge's by default,
    // SecondSonPC's own with --native memory (P2.3.4), which takes the address space right here.
    if (own_loader && Listed(cfg.native_groups, "memory") &&
        !InstallNativeMemory(cfg, Common::ElfInfo::Instance().CompiledSdkVer(), meta.attributes)) {
        Fatal("--native memory: SecondSonPC's memory manager could not take the guest memory, see SecondSonPC.log");
    }
    // Reserves the guest address space. Must precede Trace::Init (thunks are host allocations).
    Core::GetGuestVirtualMemory().Reserve();
    DiagBenchAt(cfg, id, "memory");
    g_controllers = Common::Singleton<::Input::GameControllers>::Instance();
    // The Phase 0 path's loader. The sspc path never creates shadPS4's Core::Linker.
    Core::Linker* shad_linker = own_loader ? nullptr : Common::Singleton<Core::Linker>::Instance();
    VideoCore::LoadRenderDoc();

    Common::SecondSonBootstrap::SetTrophyIndexMap(
        Core::ExtractTrophies("/app0/sce_sys/npbind.dat", "/app0/sce_sys/trophy"));

    // PC Edition: the product's name (app/product.h); the loader, title id and version go to the log
    const std::string window_title(App::kWindowTitle);
    SSPC_LOG(Bridge, Info, "game window \"{}\" ({} loader, {} - {} <{}>)", window_title,
             own_loader ? "sspc" : "shadps4", id, title, app_version);
    // The window's first controller check logs in the user of slot 1.
    if (own_loader && std::ranges::find(cfg.bridged_groups, "user") == cfg.bridged_groups.end()) {
        InstallUserHost();
    }
    auto window = std::make_unique<Frontend::WindowSDL>(
        static_cast<s32>(EmulatorSettings.GetWindowWidth()),
        static_cast<s32>(EmulatorSettings.GetWindowHeight()), g_controllers, window_title);
    g_window = window.get();
    window->SetIcon(meta.icon);
    {
        // the title as SDL and Windows hold it (UTF-8 -> UTF-16: the em dash survives)
        const char* sdl_title = SDL_GetWindowTitle(window->GetSDLWindow());
        const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window->GetSDLWindow()),
                                                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        wchar_t shown[128] = {};
        const int n = hwnd != nullptr ? GetWindowTextW(hwnd, shown, static_cast<int>(std::size(shown))) : 0;
        wchar_t expected[128] = {};
        const int en = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, App::kWindowTitle.data(),
                                           static_cast<int>(App::kWindowTitle.size()), expected,
                                           static_cast<int>(std::size(expected)));
        if (sdl_title != nullptr && std::string_view(sdl_title) == App::kWindowTitle &&
            en > 0 && std::wstring_view(shown, static_cast<size_t>(n)) ==
                          std::wstring_view(expected, static_cast<size_t>(en))) {
            SSPC_LOG(Bridge, Info, "game window title verified (SDL and Win32, {} UTF-16 units)", n);
        } else {
            SSPC_LOG(Bridge, Error, "game window title is not the PC Edition's (SDL \"{}\", Win32 {} units)",
                     sdl_title != nullptr ? sdl_title : "", n);
        }
    }
    if (!cfg.test_presses.empty()) {
        StartTestInput(cfg.test_presses, cfg.logs_dir);
    }
    SSPC_LOG(App, Info,
             "P2.5 bridge performance changes: barrier-memo {}, image-memo {}, stream-host {}, "
             "stream-reads {}",
             PerfOpts::barrier_memo ? "on" : "off", PerfOpts::image_memo ? "on" : "off",
             PerfOpts::stream_host ? "on" : "off", PerfOpts::stream_reads ? "on" : "off");
    if (cfg.track_stats) {
        TrackDiag::Enable(); // before the GPU (its page manager) and before the frame log
    }
    if (cfg.gpu_stats) {
        Gpu::Pm4Stats::Enable({.dir = cfg.logs_dir}); // before the GPU command processor takes a packet
    }
    if (cfg.frame_pacing) {
        Gpu::FramePacing::Enable(cfg.logs_dir); // P4.2, SSPC-015: before the first submit and the first vblank
    }
    if (cfg.gpu_shadow && EmulatorSettings.IsCopyGpuBuffers()) {
        // the bridge would execute copies of the command buffers at other addresses: the shadow's lockstep (same
        // packet, same address) cannot hold — refused rather than reporting every packet as a divergence
        Fatal("--gpu-shadow / --gpu-trace need the bridge's copyGPUBuffers off (it is on in the bridge's settings)");
    }
    if (cfg.gpu_shadow) {
        // P4.2: SecondSonPC's command processor beside the bridge's, before its first submission (reads command
        // memory through the GPU memory contract installed above; claims nothing)
        Gpu::CpShadow::Enable({.dir = cfg.logs_dir,
                               .trace_start_s = cfg.gpu_trace_start_s,
                               .trace_frames = cfg.gpu_trace_frames,
                               .shaders = cfg.gpu_shaders,
                               .shaders_per_bind = cfg.gpu_shaders_per_bind,
                               .shaders_translate = cfg.gpu_shaders_translate});
        // P4.3: the bridge's pipeline cache reports its pipeline at each draw (patch 0031)
        ShaderHooks::g_view.store(cfg.gpu_shaders, std::memory_order_relaxed);
    }
    if (cfg.gpu_shader_capture) {
        // P4.3: every module the bridge translates from now on, with its inputs (patch 0031). With the persistent
        // shader cache warm the bridge translates nothing: capture with --no-shader-cache.
        const auto path = cfg.logs_dir / "gpu_shaders" /
                          fmt::format("capture_{}.sspcshd", std::chrono::duration_cast<std::chrono::seconds>(
                                                                std::chrono::system_clock::now().time_since_epoch())
                                                                .count());
        if (!Gpu::Shader::StartCapture(path)) {
            Fatal("--gpu-shader-capture: cannot write " + path.string());
        }
        ShaderHooks::g_capture.store(true, std::memory_order_relaxed);
        Lifecycle::AddExitStep(Lifecycle::Stage::Reports, "shader capture", [] {
            ShaderHooks::g_capture.store(false, std::memory_order_relaxed);
            Gpu::Shader::StopCapture();
        });
        SSPC_LOG(Shader, Info, "--gpu-shader-capture: the bridge's translations -> {}", path.string());
    }
    if (cfg.frame_log) {
        StartFrameLog(cfg.logs_dir / "frametimes.csv");
    }
    StartMemoryMapDump(cfg.logs_dir / "memory_map.txt", cfg.memory_map_s);

    using Common::FS::GetUserPath;
    using Common::FS::PathType;
    if (!own_loader) {
        // The game's /data, /temp0, /download0 and the system fonts in shadPS4's mount table. On the
        // sspc path the game reaches them through SecondSonPC's Vfs (SetupFilesystem) and no BRIDGE
        // library the game imports uses them (all libkernel file functions are NATIVE, libSceFont
        // is not imported), so they are not mounted there.
        mnt->Mount(GetUserPath(PathType::GameDataDir), "/data");
        const auto temp_dir = GetUserPath(PathType::TempDataDir) / id;
        std::filesystem::remove_all(temp_dir, ec); // temp0 is cleared on each boot
        std::filesystem::create_directory(temp_dir, ec);
        mnt->Mount(temp_dir, "/temp0");
        mnt->Mount(temp_dir, "/temp");
        const auto download_dir = GetUserPath(PathType::DownloadDir) / id;
        std::filesystem::create_directory(download_dir, ec);
        mnt->Mount(download_dir, "/download0");

        const auto fonts_dir = EmulatorSettings.GetFontsDir();
        std::filesystem::create_directory(fonts_dir, ec);
        std::string guest_font_dir = "/";
        guest_font_dir.append(Libraries::Kernel::sceKernelGetFsSandboxRandomWord()).append("/common/font");
        std::filesystem::create_directory(fonts_dir / "font", ec);
        std::filesystem::create_directory(fonts_dir / "font2", ec);
        mnt->Mount(fonts_dir / "font", guest_font_dir);
        mnt->Mount(fonts_dir / "font2", guest_font_dir + "2");
    }
    const auto captures_dir = GetUserPath(PathType::CapturesDir);
    std::filesystem::create_directory(captures_dir, ec);
    VideoCore::SetOutputDir(captures_dir, id);

    Libraries::Kernel::g_environment[0] = nullptr;

    // The bridge's vectored exception handler (GPU write tracking, JIT patches, guest signals) is
    // registered when its dispatcher is first used; create it now, before any game code, and put
    // the crash recorder's data-fault half behind it: the faults the bridge resolves (~1000 a
    // second in gameplay) then never reach it (P0 FPS regression).
    (void)Core::Signals::Instance();
    Base::InstallLateFaultRecorder();

    if (own_loader) {
        RunSspcLoader(cfg, guest_eboot_path, id, meta.attributes);
    } else {
        RunPhase0Loader(cfg, *shad_linker, guest_eboot_path);
    }

    window->InitTimers();
    while (window->IsOpen()) {
        window->WaitEvent();
    }
    SSPC_LOG(App, Info, "window closed");
    Lifecycle::Exit(0, "the window was closed");
}

void RunStockForDiagnostics(const App::Config& cfg) {
    SSPC_LOG(Bridge, Warning, "DIAGNOSTIC: --stock-bootstrap, running shadPS4 Core::Emulator::Run "
                              "(no SecondSonPC exports, no call counter)");
    std::error_code ec;
    std::filesystem::create_directories(cfg.logs_dir, ec);
    Common::FS::SetUserPath(Common::FS::PathType::LogDir, cfg.logs_dir);
    Common::Log::Setup("bridge_shadps4.log");
    KeyManager::GetInstance()->LoadFromFile();
    EmulatorSettings.Load();
    UserSettings.Load();
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = "SecondSonPC.exe";
    emulator->Run(cfg.game_dir / "eboot.bin");
    std::quick_exit(0);
}

} // namespace SecondSon::Bridge
