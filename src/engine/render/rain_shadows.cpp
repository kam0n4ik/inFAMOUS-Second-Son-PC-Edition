// SecondSonPC — the game's rain shadow maps (see rain_shadows.h).
#include "engine/render/rain_shadows.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render::RainShadows {

namespace {

// --- the hooked functions (FNV-1a 64 of the body, tools/native-reconstruction/ghidra/queries/candidates.py) ---------
// IsActive(object) -> bool, 0x68 bytes, pure: replaced by IsActiveHook (the same test; its first 14 bytes hold a
// jnc, so it cannot take an entry probe; no branch of the program targets them).
// Only installed for SSPC_R12_RAIN_DIAG=release.
constexpr GuestFunction kIsActive{0x43b740, 0x068, 0xb5bd65b9bc5e5424ull};
// Update(object, view): its only callers are the three calls in 0x41b490. Prologue push rbp / mov rbp,rsp /
// push r15..rbx / sub rsp,0x1c8 (20 bytes, position-independent, no reference into it).
constexpr ProbeSite kUpdateSite{{0x43b7b0, 0x99c, 0x013a31573e4f5b53ull}, 20};
// The functions whose behaviour the hooks rely on.
constexpr GuestFunction kAssumed[] = {
    {0x598b50, 0x010, 0x688b6a1e01078d90ull}, // GetWetness: returns the blended 0xe87c80 (ignores its argument)
    {0x190330, 0x1e5, 0x056c61470c877ae0ull}, // pass 1's rain draws: inactive -> clear + the default texture
    {0x597ec0, 0x613, 0x48f786e41047d504ull}, // the wetness pass: binds the Rain object's last cascade (0x43f7c0)
    {0x43f7c0, 0x4d5, 0x90ae27b36ae67b98ull}, // the shadow binding: no cascade -> 0x7d4c348, depth compare always
    {0x4394a0, 0x110, 0x434b122e81e49c04ull}, // the shadow atlas's release (manager 0x7d45028, data, render data)
    {0x0aa250, 0x00c, 0x57ca660fc1cc05bfull}, // the allocation's destroy at reference count 0 (virtual, +0x18)
    {0x197960, 0x004, 0x16f990e4c0a50ff0ull}, // the render data's geometry mask: mov [rdi+8], esi; ret
    {0x198d50, 0x1d1, 0xd6c572d52d1fb72bull}, // the draw list's init (0x439650): mask = render data +8 & 0x31ff
};

// The shadow objects (eboot data) and their fields.
constexpr u64 kSunObject = 0x7d45120;      // kind 0
constexpr u64 kRainObject = 0x7d47570;     // kind 1
constexpr u64 kRainDynObject = 0x7d499c0;  // kind 2
constexpr u64 kKind = 0x2440;              // s32: 0 Directional, 1 Rain, 2 Rain Dynamic
constexpr u64 kCascadeCount = 0x2380;      // s32
constexpr u64 kCascadeStride = 0x8e0;
constexpr u64 kCascadeData = 0x8d0;        // u64: the atlas allocation (reference counted, +8 s32)
constexpr u64 kCascadeRender = 0x8d8;      // u64: the render data (+0xd u8: drawn, +8 u32: geometry mask)
constexpr u64 kRenderMask = 0x8;           // u32, written by Update every frame (0x43c04c -> 0x197960)
constexpr u64 kCascadeSlot = 0xfc;         // s32: -1 when released
constexpr u64 kShadowAtlas = 0x7d45028;    // the shadow atlas manager (0x4394a0's first argument)
constexpr u64 kRelease = 0x4394a0, kDestroy = 0x0aa250;
// IsActive's inputs
constexpr u64 kWetness = 0xe87c80;         // float, the frame's blended wetness (GetWetness)
constexpr u64 kSunLight = 0x1d1dfb8;       // u64, the directional light (kind 0)
constexpr u64 kSunIntensity = 0xe87b44;    // float (0xe87aa0 + 0xa4)
constexpr u64 kSunOff = 0x7d45118;         // u8
constexpr u64 kRainDistance = 0xdf9d20;    // float CSM1_RAIN_SHADOW_DISTANCE

using UpdateFn = SSPC_SYSV_ABI void (*)(u64 object, u64 view);
using ReleaseFn = SSPC_SYSV_ABI void (*)(u64 atlas, u64 data, u64 render);
using DestroyFn = SSPC_SYSV_ABI void (*)(u64 data);
UpdateFn g_update = nullptr;
u64 g_base = 0;

std::atomic<bool> g_requested_off{false}; // [Graphics] rainShadows = "off"
std::atomic<bool> g_frame_off{false};     // latched at the sun's update (the frame's first)
bool g_env_mode = false;                  // (SSPC_RAIN_SHADOWS set: the setting's changes are not applied)
bool g_installed = false;
bool g_diag_release = false;              // SSPC_R12_RAIN_DIAG=release: "off" = the game's wetness-0 path (rejected)
float g_diag_distance = 0.0f;             // SSPC_R12_RAIN_DIST (0: the game's own)
bool g_stats = false;                     // SSPC_R12_RAIN_STATS: the counters below are kept
u32 g_diag_cycle = 0;                     // SSPC_R12_RAIN_CYCLE=<s>: off in every other <s> seconds (the live switch)
const auto g_t0 = std::chrono::steady_clock::now();

struct Counters {
    std::atomic<u64> updates[3]{}, skipped[3]{}, released{0}, emptied{0};
    std::atomic<u64> active_yes[3]{}, active_no[3]{};
};
Counters g_count;

template <typename T>
T* At(u64 rva) {
    return reinterpret_cast<T*>(g_base + rva);
}

std::string Env(const char* name) {
    char v[128] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

bool IsRainKind(s32 kind) {
    return static_cast<u32>(kind - 1) < 2;
}

u32 KindIndex(s32 kind) {
    return kind >= 0 && kind <= 2 ? static_cast<u32>(kind) : 0;
}

/// IsActive 0x43b740, reconstructed (pinned by its hash): at "off" the rain kinds answer as at wetness 0.
SSPC_SYSV_ABI bool IsActiveHook(u64 object) {
    const s32 kind = *reinterpret_cast<const s32*>(object + kKind);
    bool active = false;
    if (IsRainKind(kind)) {
        active = !g_frame_off.load(std::memory_order_relaxed) && *At<float>(kWetness) > 0.0f; // (seta: NaN false)
    } else if (kind == 0 && *At<u64>(kSunLight) != 0) {
        const float intensity = *At<float>(kSunIntensity);
        active = !(intensity == 0.0f || std::isnan(intensity)) && *At<u8>(kSunOff) == 0; // (jz: equal or unordered)
    }
    if (g_stats) {
        const u32 k = KindIndex(kind);
        (active ? g_count.active_yes[k] : g_count.active_no[k]).fetch_add(1, std::memory_order_relaxed);
    }
    return active;
}

/// Update's own path at wetness <= 0 for a rain kind (0x43b90e..0x43b984): every cascade with render data is
/// released and nothing else changes.
void ReleaseCascades(u64 object) {
    const s32 count = *reinterpret_cast<const s32*>(object + kCascadeCount);
    for (s32 i = 0; i < count; ++i) {
        const u64 c = object + static_cast<u64>(i) * kCascadeStride;
        const u64 render = *reinterpret_cast<const u64*>(c + kCascadeRender);
        if (render == 0) {
            continue;
        }
        reinterpret_cast<ReleaseFn>(g_base + kRelease)(g_base + kShadowAtlas,
                                                       *reinterpret_cast<const u64*>(c + kCascadeData), render);
        *reinterpret_cast<s32*>(c + kCascadeSlot) = -1;
        *reinterpret_cast<u64*>(c + kCascadeRender) = 0;
        if (const u64 data = *reinterpret_cast<const u64*>(c + kCascadeData); data != 0) {
            if (--*reinterpret_cast<s32*>(data + 8) == 0) {
                reinterpret_cast<DestroyFn>(g_base + kDestroy)(data);
            }
        }
        *reinterpret_cast<u64*>(c + kCascadeData) = 0;
        g_count.released.fetch_add(1, std::memory_order_relaxed);
    }
}

SSPC_SYSV_ABI void UpdateProbe(u64 object, u64 view) {
    const s32 kind = *reinterpret_cast<const s32*>(object + kKind);
    if (kind == 0) {
        bool off = g_requested_off.load(std::memory_order_relaxed);
        if (g_diag_cycle != 0) {
            const auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_t0);
            off = (static_cast<u64>(s.count()) / g_diag_cycle & 1) != 0;
        }
        g_frame_off.store(off, std::memory_order_relaxed);
    }
    const u32 k = KindIndex(kind);
    if (g_stats) {
        g_count.updates[k].fetch_add(1, std::memory_order_relaxed);
    }
    if (!IsRainKind(kind)) {
        g_update(object, view);
        return;
    }
    const bool off = g_frame_off.load(std::memory_order_relaxed);
    if (off && g_diag_release) {
        if (g_stats) {
            g_count.skipped[k].fetch_add(1, std::memory_order_relaxed);
        }
        ReleaseCascades(object); // the game's own wetness-0 path for this object
        return;
    }
    if (g_diag_distance > 0.0f && *At<float>(kRainDistance) != g_diag_distance) {
        *At<float>(kRainDistance) = g_diag_distance;
    }
    g_update(object, view);
    if (!off) {
        return;
    }
    // "off": the map stays (allocated, bound, cleared by the game every frame) but its draw list collects nothing -
    // the geometry mask Update has just set (0x10 / 0x1000) is 0 until Update sets it again next frame.
    const s32 count = *reinterpret_cast<const s32*>(object + kCascadeCount);
    for (s32 i = 0; i < count; ++i) {
        const u64 render = *reinterpret_cast<const u64*>(object + static_cast<u64>(i) * kCascadeStride + kCascadeRender);
        if (render != 0) {
            *reinterpret_cast<u32*>(render + kRenderMask) = 0;
            if (g_stats) {
                g_count.emptied.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

/// The cascades of an object that hold render data, and of those the drawn ones.
std::string Cascades(u64 rva) {
    const u64 object = g_base + rva;
    const s32 count = *reinterpret_cast<const s32*>(object + kCascadeCount);
    std::string s = fmt::format("{}:", count);
    for (s32 i = 0; i < count && i < 8; ++i) {
        const u64 render = *reinterpret_cast<const u64*>(object + static_cast<u64>(i) * kCascadeStride + kCascadeRender);
        s += render == 0 ? "-" : (*reinterpret_cast<const u8*>(render + 0xd) != 0 ? "D" : "r");
    }
    return s;
}

void Reporter(u32 seconds) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const auto x = [](std::atomic<u64>& a) { return a.exchange(0); };
        SSPC_LOG(Engine, Info,
                 "Rain shadows stats: {} - updates sun/rain/dyn {}/{}/{}, emptied {}, skipped rain/dyn {}/{}, released {}; "
                 "IsActive yes/no sun {}/{} rain {}/{} dyn {}/{}; cascades sun {} rain {} dyn {}; wetness {:.3f}, "
                 "CSM1_RAIN_SHADOW_DISTANCE {:.0f}",
                 g_frame_off.load() ? (g_diag_release ? "off (release)" : "off") : "original",
                 x(g_count.updates[0]), x(g_count.updates[1]), x(g_count.updates[2]), x(g_count.emptied),
                 x(g_count.skipped[1]), x(g_count.skipped[2]), x(g_count.released),
                 x(g_count.active_yes[0]), x(g_count.active_no[0]), x(g_count.active_yes[1]),
                 x(g_count.active_no[1]), x(g_count.active_yes[2]), x(g_count.active_no[2]), Cascades(kSunObject),
                 Cascades(kRainObject), Cascades(kRainDynObject), *At<float>(kWetness), *At<float>(kRainDistance));
    }
}

bool IsOff(std::string_view mode) {
    return mode == "off";
}

} // namespace

void SetMode(std::string_view mode) {
    if (g_env_mode) {
        SSPC_LOG(Engine, Info, "Rain shadows: {} not applied - SSPC_RAIN_SHADOWS is set", mode);
        return;
    }
    if (!g_installed) {
        SSPC_LOG(Engine, Warning, "Rain shadows: {} not applied - the hooks are not installed", mode);
        return;
    }
    g_requested_off.store(IsOff(mode));
    SSPC_LOG(Engine, Info, "Rain shadows: {} (from the game's next frame)", IsOff(mode) ? "off" : "original");
}

bool Install(u64 eboot_base, std::string_view mode) {
    const std::string env_mode = Env("SSPC_RAIN_SHADOWS");
    const std::string stats = Env("SSPC_R12_RAIN_STATS");
    const std::string dist = Env("SSPC_R12_RAIN_DIST");
    g_env_mode = !env_mode.empty();
    const bool off = IsOff(g_env_mode ? std::string_view(env_mode) : mode);
    if (Env("SSPC_R12_RAIN_HOOK") == "0") {
        SSPC_LOG(Engine, Warning, "Rain shadows: SSPC_R12_RAIN_HOOK=0 - no hook (the game's own; the setting does "
                                  "nothing)");
        return false;
    }
    g_diag_release = Env("SSPC_R12_RAIN_DIAG") == "release";
    std::string error;
    bool verified = (!g_diag_release || VerifyGuestFunction(eboot_base, kIsActive, error)) &&
                    VerifyGuestFunction(eboot_base, kUpdateSite.function, error);
    for (const GuestFunction& f : kAssumed) {
        // (another module's entry probe on it - the weather tool's on 0x597ec0 - verified the same bytes)
        const ProbeRecord* probe = FindEntryProbe(eboot_base + f.rva);
        verified = verified && ((probe != nullptr && probe->function.fnv1a64 == f.fnv1a64 &&
                                 probe->function.size == f.size) ||
                                VerifyGuestFunction(eboot_base, f, error));
    }
    if (!verified) {
        SSPC_LOG(Engine, Error, "Rain shadows: hooks not installed (the game's own; the setting does nothing): {}",
                 error);
        return false;
    }
    g_base = eboot_base;
    g_stats = std::strtoul(stats.c_str(), nullptr, 10) != 0;
    g_requested_off.store(off);
    g_frame_off.store(off);
    void* t = InstallEntryProbe(eboot_base, kUpdateSite, reinterpret_cast<const void*>(&UpdateProbe), error,
                                reinterpret_cast<void**>(&g_update));
    if (t == nullptr) {
        SSPC_LOG(Engine, Error, "Rain shadows: Update 0x43b7b0 probe not installed (the game's own): {}", error);
        return false;
    }
    g_update = reinterpret_cast<UpdateFn>(t);
    if (g_diag_release &&
        !InstallJump(eboot_base + kIsActive.rva, reinterpret_cast<const void*>(&IsActiveHook), error)) {
        // (the probe alone at "off" would release the cascades while IsActive still says yes: keep it neutral)
        g_requested_off.store(false);
        g_frame_off.store(false);
        SSPC_LOG(Engine, Error, "Rain shadows: IsActive 0x43b740 not replaced (the game's own): {}", error);
        return false;
    }
    g_installed = true;
    const std::string what =
        fmt::format("Rain shadows: Update 0x43b7b0 probed{} - {}{}",
                    g_diag_release ? ", IsActive 0x43b740 replaced (DIAGNOSTICS: SSPC_R12_RAIN_DIAG=release - \"off\" "
                                     "releases the maps; breaks the picture, see docs/R12_RAIN_SHADOWS.md)"
                                   : "",
                    off ? "off (empty map)" : "original", g_env_mode ? " (SSPC_RAIN_SHADOWS)" : "");
    if (off) {
        SSPC_LOG(Engine, Warning, "{}", what);
    } else {
        SSPC_LOG(Engine, Info, "{}", what);
    }
    if (!dist.empty()) {
        g_diag_distance = std::strtof(dist.c_str(), nullptr);
        if (g_diag_distance > 0.0f) {
            SSPC_LOG(Engine, Warning,
                     "DIAGNOSTICS: SSPC_R12_RAIN_DIST={} - CSM1_RAIN_SHADOW_DISTANCE written before each rain "
                     "update (the game's: {:.0f}; changes the picture, A/B only)",
                     g_diag_distance, *At<float>(kRainDistance));
        }
    }
    if (const std::string cycle = Env("SSPC_R12_RAIN_CYCLE"); !cycle.empty()) {
        g_diag_cycle = static_cast<u32>(std::strtoul(cycle.c_str(), nullptr, 10));
        if (g_diag_cycle != 0) {
            SSPC_LOG(Engine, Warning,
                     "DIAGNOSTICS: SSPC_R12_RAIN_CYCLE={} - rain shadows off in every other {} s (the live switch; "
                     "changes the picture, A/B only)",
                     g_diag_cycle, g_diag_cycle);
        }
    }
    if (const u32 seconds = static_cast<u32>(std::strtoul(stats.c_str(), nullptr, 10)); seconds != 0) {
        std::thread(Reporter, seconds).detach();
    }
    return true;
}

} // namespace SecondSon::Engine::Render::RainShadows
