// SecondSonPC — the game's reflections (see reflections.h).
#include "engine/render/reflections.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render::Reflections {

namespace {

// --- the probed functions (FNV-1a 64 of the body, tools/native-reconstruction/ghidra/queries/candidates.py) ---------
// SsrPass(targets 0xe682b0, context, camera): its only caller is pass 12's setup 0x191100. Prologue push rbp / mov
// rbp,rsp / push r15..rbx / sub rsp,0x98 (20 bytes, position-independent, no reference into it).
constexpr ProbeSite kSsrSite{{0x45a690, 0x5d3, 0x0638f1110f883d4cull}, 20};
// PlanarReflections(views 0x7d37598, context, camera): pass 18's setup 0x191e30 (a tail jump). push rbp .. sub rsp,0x78.
constexpr ProbeSite kPlanarSite{{0x3bfb90, 0x3ec, 0x8ee682ff0f9c41fcull}, 17};
// WaterReflections(water 0x7f7a750, context, camera): pass 16's setup 0x191760. push rbp .. sub rsp,0x48.
constexpr ProbeSite kWaterSite{{0x5974f0, 0x438, 0x702dfc8dc8163e00ull}, 17};

// The environment object (lazily made by 0x59d700) and its reflection fields (reflection table 0xd1c000..0xd1e180).
constexpr u64 kEnvironment = 0x7f8e800;
constexpr u64 kAmbientSpecCubemap = 0x250; // float AMBIENT_SPECULAR_CUBEMAP_MULTIPLIER
constexpr u64 kWetnessLevel = 0x348;       // float WETNESS_LEVEL
constexpr u64 kPuddleLevel = 0x350;        // float PUDDLE_LEVEL
constexpr u64 kSsrOpacity = 0x3c8;         // float SSR_OPACITY (SsrPass runs when > 0)
constexpr u64 kHalfRes = 0x3cc;            // u8 HALF_RES_REFLECTIONS
constexpr u64 kBlurWater = 0x3cd;          // u8 BLUR_WATER_REFLECTIONS
constexpr u64 kWeatherKind = 0x3d8;        // s32 WEATHER_KIND
constexpr u64 kSsrDebugOff = 0x7dff160;    // u8: SsrPass does nothing when set (never written by the game's code)
// The FX objects' globals (one pointer each) and the class names of their kinds (the class table of the FX classes:
// kind 0x3ba = CFxMotionBlur at 0xdace00, one qword per kind).
constexpr u64 kFxGlobalsLo = 0x7d4bea0, kFxGlobalsHi = 0x7d4c4c0;
constexpr u64 kFxClassNames = 0xdace00;
constexpr u32 kFxKindAtNames = 0x3ba, kFxKindLo = 0x39a, kFxKindHi = 0x401;
// The scene targets (0xe682b0, made by 0x18e4c0 through 0x300a90(target, level, ...)) the reflections use, and the
// size of each level (u32 width, height pairs).
constexpr u64 kTargets = 0xe682b0, kLevelSizes = 0x1d0cc4c;
constexpr u64 kSceneTargets[] = {0x78d4, 0x87ec, 0x8f78, 0x9704, 0xdaf0, 0x1890c, 0x19098, 0x1aed0};

using SsrFn = SSPC_SYSV_ABI void (*)(u64 targets, u64 context, u64 camera);
using ViewsFn = SSPC_SYSV_ABI void (*)(u64 views, u64 context, u64 camera);
SsrFn g_ssr = nullptr;
ViewsFn g_planar = nullptr;
ViewsFn g_water = nullptr;
u64 g_base = 0;

// [Graphics] reflectionQuality: true = "low" (SsrPass not recorded)
std::atomic<bool> g_ssr_skip{false};
bool g_env_quality = false; // (SSPC_REFLECTION_QUALITY set: the setting's changes are not applied)
bool g_installed = false;

bool g_diag_ssr_off = false, g_diag_ssr_flip = false, g_diag_planar_off = false, g_diag_water_off = false;
bool g_diag_ssr_cycle = false; // (off in every other second: the live switch's first frames)
const auto g_t0 = std::chrono::steady_clock::now();

struct Counters {
    std::atomic<u64> ssr_calls{0}, ssr_would_run{0}, ssr_skipped{0};
    std::atomic<u64> planar_calls{0}, planar_views{0}, planar_skipped{0};
    std::atomic<u64> water_calls{0}, water_skipped{0};
};
Counters g_count;
std::atomic<u64> g_ssr_flip{0};

template <typename T>
T* At(u64 rva) {
    return reinterpret_cast<T*>(g_base + rva);
}

std::string Env(const char* name) {
    char v[128] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

bool Readable(u64 at, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (at == 0 || VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return at + size <= reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
}

/// The environment object, 0 before the game made it.
u64 Environment() {
    return *At<u64>(kEnvironment);
}

bool SsrWouldRun() {
    const u64 env = Environment();
    return *At<u8>(kSsrDebugOff) == 0 && env != 0 && *reinterpret_cast<const float*>(env + kSsrOpacity) > 0.0f;
}

SSPC_SYSV_ABI void SsrProbe(u64 targets, u64 context, u64 camera) {
    g_count.ssr_calls.fetch_add(1, std::memory_order_relaxed);
    if (SsrWouldRun()) {
        g_count.ssr_would_run.fetch_add(1, std::memory_order_relaxed);
        if (g_ssr_skip.load(std::memory_order_relaxed) || g_diag_ssr_off ||
            (g_diag_ssr_flip && (g_ssr_flip.fetch_add(1, std::memory_order_relaxed) & 1) != 0) ||
            (g_diag_ssr_cycle &&
             (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_t0).count() & 1) !=
                 0)) {
            g_count.ssr_skipped.fetch_add(1, std::memory_order_relaxed);
            return; // the game's own path at SSR_OPACITY 0: nothing recorded
        }
    }
    g_ssr(targets, context, camera);
}

SSPC_SYSV_ABI void PlanarProbe(u64 views, u64 context, u64 camera) {
    g_count.planar_calls.fetch_add(1, std::memory_order_relaxed);
    // views: +0 the array of {view, ?, ?} (0x18 bytes), +8 s32 count, +0x18 u8 on; a view draws when +0x4a8 is set
    const u64 array = *reinterpret_cast<const u64*>(views);
    const s32 n = *reinterpret_cast<const s32*>(views + 8);
    u64 active = 0;
    for (s32 i = 0; i < n && i < 64 && Readable(array + static_cast<u64>(i) * 0x18, 8); ++i) {
        const u64 view = *reinterpret_cast<const u64*>(array + static_cast<u64>(i) * 0x18);
        if (Readable(view + 0x4a8, 1) && *reinterpret_cast<const u8*>(view + 0x4a8) != 0) {
            ++active;
        }
    }
    if (*reinterpret_cast<const u8*>(views + 0x18) != 0) {
        g_count.planar_views.fetch_add(active, std::memory_order_relaxed);
    }
    if (g_diag_planar_off) {
        g_count.planar_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_planar(views, context, camera);
}

SSPC_SYSV_ABI void WaterProbe(u64 water, u64 context, u64 camera) {
    g_count.water_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_diag_water_off) {
        g_count.water_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_water(water, context, camera);
}

/// "kind ClassName" of the FX object a global points to, empty when it is not one.
std::string FxClass(u64 global) {
    const u64 fx = *At<u64>(global);
    if (!Readable(fx, 0x130)) {
        return {};
    }
    const u64 vtbl = *reinterpret_cast<const u64*>(fx);
    if (!Readable(vtbl, 0x48)) {
        return {};
    }
    const u64 get_kind = *reinterpret_cast<const u64*>(vtbl + 0x40);
    const u8* code = reinterpret_cast<const u8*>(get_kind);
    if (!Readable(get_kind, 6) || code[0] != 0xB8 || code[5] != 0xC3) {
        return {};
    }
    u32 kind = 0;
    std::memcpy(&kind, code + 1, 4);
    if (kind < kFxKindLo || kind > kFxKindHi) {
        return {};
    }
    u64 name = *At<u64>(kFxClassNames + (static_cast<u64>(kind) - kFxKindAtNames) * 8);
    if (name < g_base) {
        name += g_base; // (not relocated)
    }
    std::string s;
    for (u32 i = 0; i < 48 && Readable(name + i, 1); ++i) {
        const char c = *reinterpret_cast<const char*>(name + i);
        if (c == 0) {
            break;
        }
        s += c;
    }
    return fmt::format("{:#x} {}", kind, s);
}

void Reporter(u32 seconds) {
    bool fx_logged = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const u64 env = Environment();
        if (!fx_logged && env != 0 && g_count.ssr_calls.load() != 0) {
            std::string list;
            for (u64 g = kFxGlobalsLo; g < kFxGlobalsHi; g += 8) {
                if (const std::string c = FxClass(g); !c.empty()) {
                    list += fmt::format(" {:#x}={}", g, c);
                }
            }
            SSPC_LOG(Engine, Info, "Reflections: FX objects:{}", list);
            // the scene targets' size levels (0x300a90: target+8 = level, size = the table 0x1d0cc4c[level])
            std::string sizes, targets;
            for (u64 l = 0; l < 10; ++l) {
                sizes += fmt::format(" {}:{}x{}", l, *At<u32>(kLevelSizes + l * 8), *At<u32>(kLevelSizes + l * 8 + 4));
            }
            for (const u64 t : kSceneTargets) {
                targets += fmt::format(" +{:#x}:L{}", t, *At<s32>(kTargets + t + 8));
            }
            SSPC_LOG(Engine, Info, "Reflections: target levels{}; scene targets 0xe682b0{}", sizes, targets);
            fx_logged = true;
        }
        float opacity = 0, wet = 0, puddle = 0, cube = 0;
        u32 half = 0, blur = 0;
        s32 weather = -1;
        if (env != 0) {
            opacity = *reinterpret_cast<const float*>(env + kSsrOpacity);
            wet = *reinterpret_cast<const float*>(env + kWetnessLevel);
            puddle = *reinterpret_cast<const float*>(env + kPuddleLevel);
            cube = *reinterpret_cast<const float*>(env + kAmbientSpecCubemap);
            half = *reinterpret_cast<const u8*>(env + kHalfRes);
            blur = *reinterpret_cast<const u8*>(env + kBlurWater);
            weather = *reinterpret_cast<const s32*>(env + kWeatherKind);
        }
        SSPC_LOG(Engine, Info,
                 "Reflections stats: SSR pass {} calls, {} would run, {} skipped; planar {} calls, {} views, {} "
                 "skipped; water {} calls, {} skipped; SSR_OPACITY {:.3f}, HALF_RES_REFLECTIONS {}, "
                 "BLUR_WATER_REFLECTIONS {}, WETNESS_LEVEL {:.3f}, PUDDLE_LEVEL {:.3f}, cube map x {:.3f}, WEATHER_KIND "
                 "{}, debug off {}",
                 g_count.ssr_calls.exchange(0), g_count.ssr_would_run.exchange(0), g_count.ssr_skipped.exchange(0),
                 g_count.planar_calls.exchange(0), g_count.planar_views.exchange(0),
                 g_count.planar_skipped.exchange(0), g_count.water_calls.exchange(0),
                 g_count.water_skipped.exchange(0), opacity, half, blur, wet, puddle, cube, weather,
                 static_cast<u32>(*At<u8>(kSsrDebugOff)));
    }
}

template <typename Fn>
bool Probe(u64 eboot_base, const ProbeSite& site, const void* target, Fn& original, const char* what) {
    std::string error;
    void* t = InstallEntryProbe(eboot_base, site, target, error, reinterpret_cast<void**>(&original));
    if (t == nullptr) {
        SSPC_LOG(Engine, Error, "Reflections: {} probe not installed (the game's own): {}", what, error);
        return false;
    }
    original = reinterpret_cast<Fn>(t);
    return true;
}

bool IsLow(std::string_view quality) {
    return quality == "low";
}

} // namespace

void SetQuality(std::string_view quality) {
    if (g_env_quality) {
        SSPC_LOG(Engine, Info, "Reflections: quality {} not applied - SSPC_REFLECTION_QUALITY is set", quality);
        return;
    }
    if (!g_installed) {
        SSPC_LOG(Engine, Warning, "Reflections: quality {} not applied - the SsrPass probe is not installed", quality);
        return;
    }
    g_ssr_skip.store(IsLow(quality));
    SSPC_LOG(Engine, Info, "Reflections: quality {} (screen-space reflections {})", IsLow(quality) ? "low" : "original",
             IsLow(quality) ? "off" : "on");
}

bool Install(u64 eboot_base, std::string_view quality) {
    const std::string stats = Env("SSPC_R12_REFL_STATS");
    const std::string diag = Env("SSPC_R12_REFL_DIAG");
    const std::string env_quality = Env("SSPC_REFLECTION_QUALITY");
    g_env_quality = !env_quality.empty();
    const bool low = IsLow(g_env_quality ? std::string_view(env_quality) : quality);
    if (Env("SSPC_R12_REFL_HOOK") == "0") {
        SSPC_LOG(Engine, Warning, "Reflections: SSPC_R12_REFL_HOOK=0 - no probe (the game's own; the setting does "
                                  "nothing)");
        return false;
    }
    g_diag_ssr_off = diag.find("ssroff") != std::string::npos;
    g_diag_ssr_flip = diag.find("ssrflip") != std::string::npos;
    g_diag_planar_off = diag.find("planaroff") != std::string::npos;
    g_diag_water_off = diag.find("wateroff") != std::string::npos;
    g_diag_ssr_cycle = diag.find("ssrcycle") != std::string::npos;
    // the planar and water probes only count / switch off for diagnostics
    const bool diagnostics = !stats.empty() || !diag.empty();
    std::string error;
    for (const ProbeSite* s : {&kSsrSite, &kPlanarSite, &kWaterSite}) {
        if ((s == &kSsrSite || diagnostics) && !VerifyGuestFunction(eboot_base, s->function, error)) {
            SSPC_LOG(Engine, Error, "Reflections: probes not installed (the game's own; the setting does nothing): {}",
                     error);
            return false;
        }
    }
    g_base = eboot_base;
    g_ssr_skip.store(low);
    if (!Probe(eboot_base, kSsrSite, reinterpret_cast<const void*>(&SsrProbe), g_ssr, "SsrPass 0x45a690")) {
        g_ssr_skip.store(false);
        return false;
    }
    g_installed = true;
    const std::string what =
        fmt::format("Reflections: entry probe on SsrPass 0x45a690 - quality {}{} (screen-space reflections {})",
                    low ? "low" : "original", g_env_quality ? " (SSPC_REFLECTION_QUALITY)" : "", low ? "off" : "on");
    if (low) {
        SSPC_LOG(Engine, Warning, "{}", what);
    } else {
        SSPC_LOG(Engine, Info, "{}", what);
    }
    if (!diagnostics) {
        return true;
    }
    bool ok = Probe(eboot_base, kPlanarSite, reinterpret_cast<const void*>(&PlanarProbe), g_planar, "planar 0x3bfb90");
    ok &= Probe(eboot_base, kWaterSite, reinterpret_cast<const void*>(&WaterProbe), g_water, "water 0x5974f0");
    if (!diag.empty()) {
        SSPC_LOG(Engine, Warning,
                 "DIAGNOSTICS: SSPC_R12_REFL_DIAG={} - not recorded (changes the picture, A/B only):{}{}{}{}{}", diag,
                 g_diag_ssr_off ? " screen-space reflections" : "",
                 g_diag_ssr_flip ? " screen-space reflections on every other frame" : "",
                 g_diag_ssr_cycle ? " screen-space reflections in every other second" : "",
                 g_diag_planar_off ? " planar reflections" : "", g_diag_water_off ? " water reflections" : "");
    }
    SSPC_LOG(Engine, Info, "Reflections: diagnostic entry probes on planar 0x3bfb90, water 0x5974f0 ({})",
             ok ? "both" : "some missing");
    if (const u32 seconds = static_cast<u32>(std::strtoul(stats.c_str(), nullptr, 10)); seconds != 0) {
        std::thread(Reporter, seconds).detach();
    }
    return true;
}

} // namespace SecondSon::Engine::Render::Reflections
