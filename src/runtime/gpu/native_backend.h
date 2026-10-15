// SecondSonPC — SecondSonPC's own GPU backend in the game process (Phase 4, P4.4; --gpu-backend sspc;
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §B). The one owner of the guest GPU in this mode: it claims every
// Resource (backend.h), registers the one memory listener, installs the GnmDriver host and starts VideoOut's present
// thread. Its GPU thread owns the command processor (P4.2, Mode::Isolated), the shader frontend (P4.3) and the Vulkan
// executor; submissions from the game's threads are queued to it. Chosen once at start-up, never switched while the
// game runs; the bridge's renderer, presenter and VideoOut are not started in this mode (their libraries have no BRIDGE
// imports left).
#pragma once

#include <filesystem>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::Native {

struct Options {
    void* sdl_window = nullptr;          ///< the game window (SDL_Window*)
    s32 device_index = -1;
    bool validation = false;             ///< --gpu-native-validate
    bool diagnostics = false;            ///< --gpu-native-diag (command journal, checkpoints, markers, names)
    s32 sdk_version = 0;                 ///< the game's compiled SDK version (GnmDriver's init sequence)
    u64 capture_flip = 0;                ///< --gpu-capture-native-frame N
    std::filesystem::path logs_dir;      ///< reports, captures
    // P4.14: the persistent shader cache and the start-up precompiler (docs/PHASE4_SHADER_PRECOMPILER.md)
    bool shader_cache = false;             ///< [ShaderCache] enabled (--no-shader-cache: off)
    std::filesystem::path shader_cache_dir; ///< [ShaderCache] dir: <dir>\<game>\native\<generation>
    std::string game = "CUSA00004";
    bool precompile = true;                ///< [ShaderCache] precompile: the manifest's pipelines before the game
    u32 cache_max_mib = 2048;              ///< [ShaderCache] maxSizeMiB
    u32 precompile_threads = 0;            ///< [ShaderCache] precompileThreads (0: auto)
    double precompile_max_s = 300.0;       ///< the game starts after this long at most (the rest in the background)
    /// The precompiler's progress, on the starting thread (the game window's: it pumps its messages and draws it),
    /// every ~30 ms and once more with `finished` set. true: the user skipped the wait (the game starts; the rest is
    /// built in the background). nullptr: nobody shows it (automated runs) - the start waits.
    struct PrecompileStatus {
        u32 total = 0, done = 0, failed = 0;
        double elapsed_s = 0.0;
        bool finished = false;
    };
    bool (*precompile_progress)(void* ctx, const PrecompileStatus& status) = nullptr;
    void* precompile_progress_ctx = nullptr;
};

/// P4.14: how long the start waited for the precompiler (scripted input shifts its timeline by it; 0 without).
u64 StartupPauseNs();

/// P4.14: the persistent shader cache of this run (the settings menu's statistics). Any thread.
struct ShaderCacheInfo {
    bool enabled = false;
    std::string dir;
    u64 file_bytes = 0, vulkan_bytes = 0;
    u64 programs = 0, modules = 0, spirv = 0, pipelines = 0; ///< in the cache (read at start + added)
    u64 module_loads = 0, translations = 0;                 ///< this run: modules read / translated
    u32 precompile_total = 0, precompile_built = 0, precompile_taken = 0;
    double precompile_ms = 0.0;
    u64 created_on_demand = 0;
    u64 game_programs = 0;   ///< the game's programs (the offline index, --scan-game-shaders; 0 without it)
    u64 cached_in_game = 0;  ///< ... the cache has
};
ShaderCacheInfo CacheInfo();

/// Brings the backend up (device, caches, threads, ownership). false with `error`: nothing claimed, nothing running.
bool Start(const Options& options, std::string& error);
/// Stops the threads, waits for the device, releases the resources (the process's exit step). Idempotent.
void Stop();
bool Running();
/// A summary of what the backend did (also written to <logs>\gpu_native.txt at Stop).
std::string Report();

} // namespace SecondSon::Gpu::Native
