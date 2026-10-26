// SecondSonPC — R12 weather: the stand's tool for reproducible weather states (see weather.h, docs/R12_WEATHER.md).
#include "engine/render/weather.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "engine/guest_probe.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/media/videodec.h"

namespace SecondSon::Engine::Render::Weather {

namespace {

// --- the analysed functions (eboot.bin of CUSA00004; FNV-1a 64 of the body, tools/native-reconstruction) ------------
// TimeOfDayUpdate(manager 0x7f8e800): once a frame from the main loop 0x29c8d0 (before RenderOverridesUpdate 0x1ba7c0):
// switches to the requested time of day (+0x10) when its package (<world>_x_<tod>) is loaded. Prologue push rbp /
// mov rbp,rsp / push r15..rbx / sub rsp,0x98 (20 bytes, position-independent, no branch into it; ghidra prologue.py).
constexpr ProbeSite kTodUpdateSite{{0x59cf80, 0x499, 0x70a5af77385b8e5eull}, 20};
// WetnessDeferred(wetness 0x7d4c158, context, ...): the deferred wetness pass, run by 0x190c90 only when the blended
// wetness > 0 (ripples 0x5984e0, FX_WETNESS_DEFERRED). Counted only (prologue as above with sub rsp,0x838).
constexpr ProbeSite kWetnessPassSite{{0x597ec0, 0x613, 0x48f786e41047d504ull}, 20};
// The functions whose behaviour the tool relies on (what it reads and writes means what the analysis says).
constexpr GuestFunction kAssumed[] = {
    {0x1b8e60, 0x257, 0xe922eb702f44277cull}, // the blend's base: the time of day's fields, WETNESS_LEVEL -> +0x150
    {0x1ba7c0, 0x3fa, 0xf7df752bdd57a0ebull}, // RenderOverridesUpdate: base + overrides -> 0xe87b30 (wetness 0xe87c80)
    {0x598b50, 0x010, 0x688b6a1e01078d90ull}, // GetWetness: the blended 0xe87c80
    {0x598ea0, 0x029, 0x2194a3935e177580ull}, // PUDDLE_LEVEL: the current time of day's +0x350
    {0x59d560, 0x01d, 0x56932dfe76dde2b0ull}, // the transition-busy test: 0x7e00740 | 0x7dff388
    {0x190c90, 0x20c, 0x85899986e3144dcaull}, // the wetness pass's gate (GetWetness > 0)
    {0x59be90, 0x03b, 0x00f9771aa4f4909full}, // CLOCK_TIME: +0x3e0 = HHMM as a fraction of the day, manager +0x28
};

// The time-of-day manager (eboot data) and its objects.
constexpr u64 kTodManager = 0x7f8e800; // +0 current, +8 ensured, +0x10 requested id, +0x18 random id, +0x20 override
                                       // id, +0x28 clock (float), +0x30 buckets (u32), +0x38 bucket array, +0x40 count
constexpr u64 kBusyA = 0x7e00740;      // u64 (0x7e00720 + 0x20)
constexpr u64 kBusyB = 0x7dff388;      // u64 (0x7dff1e0 + 0x1a8)
constexpr u64 kWorldName = 0x7f2d280;  // u64 -> a counted string (+2: the chars), "%s_x_%s" in 0x59cf80
constexpr u64 kPackages = 0x7d37610;   // the package registry 0x3c4e40: buckets +0x198 (997), entries +0xc8 / +0xd0
constexpr u64 kWetness = 0xe87c80;     // float, the frame's blended wetness
constexpr u64 kOverrides = 0xe87aa0;   // u64[16] active visual overrides, count (s64) at 0xe87b20
constexpr u64 kOverrideCount = 0xe87b20;
constexpr u64 kGameDt = 0xe16298;      // float, the game's step (0: paused / menus / loading; main loop 0x29c8d0)
// CTimeOfDayParams fields (reflection table 0xd1bc60.., direct fields)
constexpr u64 kRecord = 0x28;      // -> the object's record: +0x18 id, +0x58 name (char*), +0x50 default, +0x51 random
constexpr u64 kSkyBrightness = 0x254, kHazeAlphaNear = 0x25c, kHazeAlphaFar = 0x260, kExposure = 0x2d0;
constexpr u64 kWetnessLevel = 0x348, kWallMultiplier = 0x34c, kPuddleLevel = 0x350, kStreakLevel = 0x354;
constexpr u64 kRippleRate = 0x358, kWindDisplacement = 0x378, kWindFrequency = 0x37c, kSsrOpacity = 0x3c8;
constexpr u64 kHalfResReflections = 0x3cc, kBlurWaterReflections = 0x3cd, kVisibility = 0x3d4, kWeatherKind = 0x3d8;
constexpr u64 kClock = 0x3e0, kDirectionalLight = 0x3e8;
// a visual override: mask +0xf0 (u64), weight +0xf8 (float), `wetness` +0x240 under mask bit 57 (setter 0x2e5310)
constexpr u64 kOvrMask = 0xf0, kOvrWeight = 0xf8, kOvrWetness = 0x240;
constexpr u64 kOvrWetnessBit = 1ull << 57;

constexpr double kSettleSeconds = 3.0;

enum class Mode { Log, Dry, Wet };

// (all six integer argument registers forwarded: the probes never assume fewer arguments than the guest passes)
using TodUpdateFn = SSPC_SYSV_ABI void (*)(u64 manager, u64, u64, u64, u64, u64);
using WetnessPassFn = SSPC_SYSV_ABI void (*)(u64 wetness, u64 context, u64, u64, u64, u64);
TodUpdateFn g_tod_update = nullptr;
WetnessPassFn g_wetness_pass = nullptr;
u64 g_base = 0;

// settings (SSPC_R12_WEATHER*)
Mode g_mode = Mode::Log;
std::string g_spec;
float g_set_wetness = 1.0f;
bool g_set_puddle = false;
float g_puddle = 0.0f;
double g_at = 0.0, g_until = -1.0, g_period = 5.0;

std::atomic<u64> g_wetness_calls{0};

// the tool's state (the main thread; the journal and the exit restore under g_mutex)
std::mutex g_mutex;
std::FILE* g_file = nullptr;
const auto g_t0 = std::chrono::steady_clock::now();
u64 g_frames = 0;
u64 g_last_cur = 0;
double g_safe_since = -1.0;
std::string g_unsafe_reason = "start";
double g_next_log = 0.0;
u64 g_period_frames = 0;
u64 g_period_unsafe = 0;
u64 g_period_cost_ns = 0, g_period_cost_max_ns = 0; // the tool's own time on the game's main thread (Frame)
bool g_done = false; // UNTIL passed: never again
double g_dt_zero_since = -1.0; // the game's step has been 0 since then
struct Applied {
    bool on = false;
    u64 obj = 0, id = 0;
    std::string name;
    float wetness = 0, puddle = 0;           // the original values
    float set_wetness = 0, set_puddle = 0;   // what was written
    u64 frame = 0;
    bool verified = false;
    u32 applies = 0, restores = 0;
} g_applied;

template <typename T>
T Rd(u64 at) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof(T));
    return v;
}

/// An eboot global (module-relative address).
template <typename T>
T Gd(u64 rva) {
    return Rd<T>(g_base + rva);
}

template <typename T>
void Wr(u64 at, T v) {
    std::memcpy(reinterpret_cast<void*>(at), &v, sizeof(T));
}

/// The committed, readable regions VirtualQuery reported (forgotten when the world changes, see Frame): VirtualQuery
/// is not cheap on the guest's address space - ~1 TB of reservations, commit on demand: the region size it reports
/// is found by walking the pages, milliseconds a call [E] - and the objects the tool reads sit in a few regions.
struct Region {
    u64 lo, hi;
};
std::vector<Region> g_regions;

bool Readable(u64 at, size_t size) {
    if (at < 0x10000) {
        return false;
    }
    for (const Region& r : g_regions) {
        if (at >= r.lo && at + size <= r.hi) {
            return true;
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const u64 lo = reinterpret_cast<u64>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    if (g_regions.size() < 64) {
        g_regions.push_back({lo, hi});
    }
    return at + size <= hi;
}

/// (VirtualQuery is not cheap on the guest's address space - ~1 TB of reservations, commit on demand: the region size
/// it reports is found by walking the pages - so a string is checked a page at a time, not a byte at a time)
std::string Str(u64 p, size_t max = 64) {
    if (!Readable(p, 1)) {
        return "?";
    }
    std::string s;
    u64 checked_to = (p | 0xfff) + 1; // the first page is readable (a committed region holds whole pages)
    for (size_t i = 0; i < max; ++i) {
        if (p + i >= checked_to) {
            if (!Readable(p + i, 1)) {
                break;
            }
            checked_to = ((p + i) | 0xfff) + 1;
        }
        const char c = Rd<char>(p + i);
        if (c == 0) {
            break;
        }
        s.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    return s;
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

std::string Env(const char* name) {
    char v[256] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

/// Hash of the engine's names (0x5ed7c0, the reflection tables): h * 101 + lower(c).
u64 NameHash(const std::string& s) {
    u64 h = 0;
    for (const char c : s) {
        h = h * 101 + static_cast<u8>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return h;
}

/// The current time of day's object and record as last checked readable (the per-frame path reads them without a
/// VirtualQuery while the manager's current object and its record pointer stay the same; Frame forgets them when the
/// current object changes).
u64 g_checked_obj = 0, g_checked_rec = 0;

/// A time-of-day object's record (+0x28), 0 when it is not readable.
u64 Record(u64 obj) {
    if (obj != 0 && obj == g_checked_obj && Rd<u64>(obj + kRecord) == g_checked_rec) {
        return g_checked_rec;
    }
    if (!Readable(obj, 0x408)) {
        return 0;
    }
    const u64 rec = Rd<u64>(obj + kRecord);
    return Readable(rec, 0x70) ? rec : 0;
}

/// Record() for the manager's current object, remembered for the next frames.
u64 CheckCurrent(u64 cur) {
    const u64 rec = Record(cur);
    if (rec != 0) {
        g_checked_obj = cur;
        g_checked_rec = rec;
    }
    return rec;
}

u64 TodId(u64 obj) {
    const u64 rec = Record(obj);
    return rec != 0 ? Rd<u64>(rec + 0x18) : ~0ull;
}

/// The names read (per object; forgotten with the current object, see Frame): a name costs VirtualQuery calls.
std::vector<std::pair<u64, std::string>> g_names;

std::string TodName(u64 obj) {
    const u64 rec = Record(obj);
    if (rec == 0) {
        return "-";
    }
    for (const auto& [o, name] : g_names) {
        if (o == obj) {
            return name;
        }
    }
    std::string name = Str(Rd<u64>(rec + 0x58));
    if (g_names.size() < 64) {
        g_names.emplace_back(obj, name);
    }
    return name;
}

/// The registered time-of-day objects (the manager's map, 0x59bc70 / 0x59cf80): {id, object}.
std::vector<std::pair<u64, u64>> Registered() {
    std::vector<std::pair<u64, u64>> out;
    const u32 buckets = Gd<u32>(kTodManager + 0x30);
    const u64 array = Gd<u64>(kTodManager + 0x38);
    if (buckets == 0 || buckets > 4096 || !Readable(array, buckets * 8ull)) {
        return out;
    }
    for (u32 b = 0; b < buckets; ++b) {
        u64 node = Rd<u64>(array + b * 8ull);
        for (u32 guard = 0; node != 0 && guard < 256 && Readable(node, 0x20); ++guard) {
            out.emplace_back(Rd<u64>(node + 8), Rd<u64>(node + 0x10));
            node = Rd<u64>(node + 0x18);
        }
    }
    return out;
}

/// The object registered under `id`, 0 when none (the same lookup as 0x59cf80, read only).
u64 RegisteredObject(u64 id) {
    for (const auto& [i, obj] : Registered()) {
        if (i == id) {
            return obj;
        }
    }
    return 0;
}

/// "<world>_x_<tod>": is the package registered and what its state bytes say (0x3c4e40 / 0x3c4db0, read only).
std::string PackageState(const std::string& tod) {
    const u64 world = Gd<u64>(kWorldName);
    if (!Readable(world, 4)) {
        return "world ?";
    }
    const std::string name = Str(world + 2) + "_x_" + tod;
    const u64 h = NameHash(name);
    const u32 key = static_cast<u32>((h >> 32) ^ h);
    u64 node = Gd<u64>(kPackages + 0x198 + (key % 997) * 8ull);
    for (u32 guard = 0; node != 0 && guard < 1024 && Readable(node, 0x18); ++guard) {
        if (Rd<u64>(node + 8) == h) {
            const s32 index = Rd<s16>(node + 0x10) >> 8;
            const u32 count = Gd<u32>(kPackages + 0xd0);
            const u64 entries = Gd<u64>(kPackages + 0xc8);
            u64 e = 0;
            if (index >= 0 && static_cast<u32>(index) < count && Readable(entries + index * 8ull, 8)) {
                e = Rd<u64>(entries + index * 8ull);
            }
            if (!Readable(e, 0x48)) {
                return fmt::format("{}: registered #{} (no entry)", name, index);
            }
            return fmt::format("{}: registered #{} state {:02x}{:02x}{:02x}{:02x}", name, index, Rd<u8>(e + 0x44),
                               Rd<u8>(e + 0x45), Rd<u8>(e + 0x46), Rd<u8>(e + 0x47));
        }
        node = Rd<u64>(node);
    }
    return name + ": not registered";
}

void Journal(const std::string& line, bool to_log) {
    const double fl = Gpu::FrameLog::Seconds();
    const std::string text = fmt::format("{:8.2f} {:8.2f} f{:<7} {}", Now(), fl, g_frames, line);
    if (g_file != nullptr) {
        std::fputs(text.c_str(), g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
    if (to_log) {
        SSPC_LOG(Engine, Info, "Weather: {}", line);
    }
}

std::string Fields(u64 obj) {
    if (Record(obj) == 0) {
        return "time of day -";
    }
    const float clock = Rd<float>(obj + kClock);
    const int minutes = static_cast<int>(std::lround(clock * 1440.0f));
    const s32 kind = Rd<s32>(obj + kWeatherKind);
    return fmt::format(
        "tod {} id {:#x} kind {} visibility {} clock {:.4f} ({:02}:{:02}) wetness {:.3f} wall {:.3f} puddle {:.3f} "
        "streak {:.3f} ripple {:.3f} wind {:.3f}/{:.3f} haze {:.3f}/{:.3f} sky {:.3f} exposure {:.3f} ssr {:.3f} "
        "halfres {} blurwater {} sun {}",
        TodName(obj), TodId(obj), kind == 1 ? "Rainy" : (kind == 0 ? "Default" : "?"),
        Rd<s32>(obj + kVisibility), clock, minutes / 60, minutes % 60, Rd<float>(obj + kWetnessLevel),
        Rd<float>(obj + kWallMultiplier), Rd<float>(obj + kPuddleLevel), Rd<float>(obj + kStreakLevel),
        Rd<float>(obj + kRippleRate), Rd<float>(obj + kWindDisplacement), Rd<float>(obj + kWindFrequency),
        Rd<float>(obj + kHazeAlphaNear), Rd<float>(obj + kHazeAlphaFar), Rd<float>(obj + kSkyBrightness),
        Rd<float>(obj + kExposure), Rd<float>(obj + kSsrOpacity), Rd<u8>(obj + kHalfResReflections),
        Rd<u8>(obj + kBlurWaterReflections), Rd<u64>(obj + kDirectionalLight) != 0 ? "yes" : "no");
}

/// The override objects already checked readable (forgotten with the current time of day, see Frame).
std::vector<u64> g_overrides_ok;

/// The active visual overrides that carry a wetness (mask bit 57): "n (w x weight, ...)".
std::string WetOverrides(u32* wet_count) {
    const s64 n = Gd<s64>(kOverrideCount);
    std::string s;
    u32 wet = 0;
    for (s64 i = 0; i < std::min<s64>(n, 16); ++i) {
        const u64 o = Gd<u64>(kOverrides + i * 8);
        if (std::find(g_overrides_ok.begin(), g_overrides_ok.end(), o) == g_overrides_ok.end()) {
            if (!Readable(o + kOvrMask, kOvrWeight + 4 - kOvrMask) || !Readable(o + kOvrWetness, 4)) {
                continue;
            }
            if (g_overrides_ok.size() < 64) {
                g_overrides_ok.push_back(o);
            }
        }
        if ((Rd<u64>(o + kOvrMask) & kOvrWetnessBit) != 0) {
            s += fmt::format("{}{:.3f}x{:.2f}", wet == 0 ? "" : ",", Rd<float>(o + kOvrWetness),
                             Rd<float>(o + kOvrWeight));
            ++wet;
        }
    }
    if (wet_count != nullptr) {
        *wet_count = wet;
    }
    return fmt::format("{} active, {} with wetness{}{}{}", std::max<s64>(n, 0), wet, wet != 0 ? " (" : "", s,
                       wet != 0 ? ")" : "");
}

void LogRegistry(const char* why) {
    const auto reg = Registered();
    std::string list;
    for (const auto& [id, obj] : reg) {
        const u64 rec = Record(obj);
        if (rec == 0) {
            list += fmt::format(" | {:#x} (object {:#x} not readable)", id, obj);
            continue;
        }
        const std::string name = TodName(obj);
        list += fmt::format(" | {} {:#x}{}{} kind {} wet {:.3f} puddle {:.3f} clock {:.3f}; {}", name, id,
                            Rd<u8>(rec + 0x50) != 0 ? " default" : "", Rd<u8>(rec + 0x51) != 0 ? " random" : "",
                            Rd<s32>(obj + kWeatherKind),
                            Rd<float>(obj + kWetnessLevel), Rd<float>(obj + kPuddleLevel), Rd<float>(obj + kClock),
                            PackageState(name));
    }
    const u64 world = Gd<u64>(kWorldName);
    Journal(fmt::format("registry ({}): world {}, {} time-of-day objects{}", why,
                        Readable(world, 4) ? Str(world + 2) : std::string("?"), reg.size(), list),
            true);
}

/// Why the game must not be touched now (nullptr: it may).
const char* Unsafe(u64 cur) {
    if (Media::FullScreenMovieShowing()) {
        return "movie";
    }
    // (a stopped step for a few frames is not yet a pause: held, see Frame)
    if (!(Gd<float>(kGameDt) > 0.0f) && g_dt_zero_since >= 0.0 && Now() - g_dt_zero_since >= 2.0) {
        return "game clock stopped (pause / menu / loading)";
    }
    if (cur == 0 || cur == Gd<u64>(kTodManager + 8) || CheckCurrent(cur) == 0) {
        return "no time of day (loading)";
    }
    if (Gd<u64>(kTodManager + 0x10) != TodId(cur)) {
        return "time of day change requested";
    }
    if ((Gd<u64>(kBusyA) | Gd<u64>(kBusyB)) != 0) {
        return "time of day transition busy";
    }
    return nullptr;
}

/// Puts back the values the tool replaced, when the object is still the registered one and holds what was written.
void Restore(const char* why) {
    if (!g_applied.on) {
        return;
    }
    g_applied.on = false;
    const u64 obj = g_applied.obj;
    // (the manager's current object, checked readable, needs no walk of the registry - a walk costs VirtualQuery
    // calls; an object the manager no longer holds is looked up in the registry, it may be gone with its world)
    const bool current = obj == Gd<u64>(kTodManager) && obj == g_checked_obj && Record(obj) != 0 &&
                         TodId(obj) == g_applied.id;
    if (!current && (RegisteredObject(g_applied.id) != obj || Record(obj) == 0)) {
        Journal(fmt::format("restore ({}): {} is no longer registered - nothing to put back", why, g_applied.name),
                true);
        return;
    }
    std::string what;
    if (Rd<float>(obj + kWetnessLevel) == g_applied.set_wetness) {
        Wr<float>(obj + kWetnessLevel, g_applied.wetness);
        what += fmt::format(" WETNESS_LEVEL {:.3f}", g_applied.wetness);
    } else {
        what += fmt::format(" WETNESS_LEVEL left at {:.3f} (changed by the game)", Rd<float>(obj + kWetnessLevel));
    }
    if (g_set_puddle) {
        if (Rd<float>(obj + kPuddleLevel) == g_applied.set_puddle) {
            Wr<float>(obj + kPuddleLevel, g_applied.puddle);
            what += fmt::format(" PUDDLE_LEVEL {:.3f}", g_applied.puddle);
        } else {
            what += fmt::format(" PUDDLE_LEVEL left at {:.3f} (changed by the game)", Rd<float>(obj + kPuddleLevel));
        }
    }
    ++g_applied.restores;
    Journal(fmt::format("restore ({}): {}{}; read back: {}", why, g_applied.name, what, Fields(obj)), true);
}

void Apply(u64 cur) {
    g_applied = Applied{.applies = g_applied.applies, .restores = g_applied.restores};
    g_applied.on = true;
    g_applied.obj = cur;
    g_applied.id = TodId(cur);
    g_applied.name = TodName(cur);
    g_applied.wetness = Rd<float>(cur + kWetnessLevel);
    g_applied.puddle = Rd<float>(cur + kPuddleLevel);
    g_applied.set_wetness = g_mode == Mode::Dry ? 0.0f : g_set_wetness;
    g_applied.set_puddle = g_set_puddle ? g_puddle : g_applied.puddle;
    g_applied.frame = g_frames;
    ++g_applied.applies;
    Journal(fmt::format("original: {}", Fields(cur)), false);
    Wr<float>(cur + kWetnessLevel, g_applied.set_wetness);
    if (g_set_puddle) {
        Wr<float>(cur + kPuddleLevel, g_applied.set_puddle);
    }
    Journal(fmt::format("apply #{} {}: {} WETNESS_LEVEL {:.3f} -> {:.3f}{}", g_applied.applies, g_spec, g_applied.name,
                        g_applied.wetness, g_applied.set_wetness,
                        g_set_puddle ? fmt::format(", PUDDLE_LEVEL {:.3f} -> {:.3f}", g_applied.puddle, g_puddle)
                                     : std::string()),
            true);
}

/// The frame after a write: the fields read back and the frame's blended wetness (the blend ran after the write).
void Verify(u64 cur) {
    g_applied.verified = true;
    u32 wet_overrides = 0;
    const std::string ovr = WetOverrides(&wet_overrides);
    const float field = Rd<float>(cur + kWetnessLevel), blended = Gd<float>(kWetness);
    const bool field_ok = field == g_applied.set_wetness &&
                          (!g_set_puddle || Rd<float>(cur + kPuddleLevel) == g_applied.set_puddle);
    const bool blend_ok = wet_overrides != 0 || std::fabs(blended - g_applied.set_wetness) < 1e-4f;
    Journal(fmt::format("verify: field {} ({:.3f}), blended wetness {:.3f} {} (overrides: {}){}", field_ok ? "ok" : "BAD",
                        field, blended,
                        wet_overrides != 0 ? "(lerped with overrides)" : (blend_ok ? "= the field: applied" : "DIFFERS"),
                        ovr, field_ok && blend_ok ? "" : " - NOT APPLIED"),
            true);
}

void Frame(u64 manager) {
    std::scoped_lock lock{g_mutex};
    ++g_frames;
    ++g_period_frames;
    const double now = Now();
    const bool stepping = Gd<float>(kGameDt) > 0.0f;
    if (stepping) {
        g_dt_zero_since = -1.0;
    } else if (g_dt_zero_since < 0.0) {
        g_dt_zero_since = now;
    }
    const u64 cur = Rd<u64>(manager);
    if (cur != g_last_cur) {
        Journal(fmt::format("current time of day: {} ({:#x} -> {:#x})", TodName(cur), g_last_cur, cur), true);
        if (g_applied.on && g_applied.obj != cur) {
            Restore("the game changed the time of day");
        }
        g_last_cur = cur;
        g_checked_obj = 0; // (checked again: the old objects may be gone with their world)
        g_names.clear();
        g_overrides_ok.clear();
        g_regions.clear();
        g_safe_since = -1.0;
        if (Record(cur) != 0 && cur != Rd<u64>(manager + 8)) {
            Journal(fmt::format("state: {}", Fields(cur)), true);
            LogRegistry("time of day loaded");
        }
    }
    const char* unsafe = Unsafe(cur);
    if (unsafe != nullptr) {
        ++g_period_unsafe;
        if (g_unsafe_reason != unsafe) {
            Journal(fmt::format("unsafe: {} - the tool keeps out", unsafe), true);
            g_unsafe_reason = unsafe;
        }
        g_safe_since = -1.0;
        if (g_applied.on) {
            Restore(unsafe);
        }
    } else {
        if (g_safe_since < 0.0) {
            g_safe_since = now;
        }
        if (!g_unsafe_reason.empty() && now - g_safe_since >= kSettleSeconds) {
            Journal(fmt::format("safe for {:.0f} s: {}", kSettleSeconds, Fields(cur)), true);
            g_unsafe_reason.clear();
        }
    }
    const bool settled = unsafe == nullptr && g_unsafe_reason.empty() && stepping;
    if (g_mode != Mode::Log && !g_done) {
        if (g_until >= 0.0 && now >= g_until) {
            Restore("SSPC_R12_WEATHER_UNTIL");
            g_done = true;
            Journal("done (SSPC_R12_WEATHER_UNTIL): the game's own values from here on", true);
        } else if (!g_applied.on && settled && now >= g_at) {
            Apply(cur);
        } else if (g_applied.on && !g_applied.verified && g_frames > g_applied.frame) {
            Verify(cur);
        } else if (g_applied.on && Rd<float>(cur + kWetnessLevel) != g_applied.set_wetness) {
            Journal(fmt::format("the game changed WETNESS_LEVEL to {:.3f}: the tool lets go",
                                Rd<float>(cur + kWetnessLevel)),
                    true);
            g_applied.on = false;
            g_done = true;
        }
    }
    if (now >= g_next_log) {
        g_next_log = now + g_period;
        u32 wet_overrides = 0;
        const std::string ovr = WetOverrides(&wet_overrides);
        Journal(fmt::format("{} | blended wetness {:.3f}, wetness pass {} calls / {} frames ({} unsafe), overrides {} | "
                            "tool ({:.1f} us/frame, max {:.1f}): {}{}",
                            Fields(cur), Gd<float>(kWetness), g_wetness_calls.exchange(0), g_period_frames,
                            g_period_unsafe, ovr,
                            g_period_frames != 0 ? g_period_cost_ns / 1000.0 / g_period_frames : 0.0,
                            g_period_cost_max_ns / 1000.0, g_spec,
                            g_applied.on ? fmt::format(" applied (#{}, since f{})", g_applied.applies, g_applied.frame)
                                         : std::string(g_done ? " done" : " not applied")),
                false);
        g_period_frames = 0;
        g_period_unsafe = 0;
        g_period_cost_ns = 0;
        g_period_cost_max_ns = 0;
    }
}

SSPC_SYSV_ABI void TodUpdateProbe(u64 manager, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    g_tod_update(manager, a1, a2, a3, a4, a5);
    const auto t0 = std::chrono::steady_clock::now();
    Frame(manager);
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_period_cost_ns += ns; // (the main thread only; read under g_mutex in Frame - a frame late, which is fine)
    g_period_cost_max_ns = std::max(g_period_cost_max_ns, ns);
}

SSPC_SYSV_ABI void WetnessPassProbe(u64 wetness, u64 context, u64 a2, u64 a3, u64 a4, u64 a5) {
    g_wetness_calls.fetch_add(1, std::memory_order_relaxed);
    g_wetness_pass(wetness, context, a2, a3, a4, a5);
}

void AtExit() {
    std::scoped_lock lock{g_mutex};
    Restore("exit");
    Journal(fmt::format("exit: {} applies, {} restores", g_applied.applies, g_applied.restores), true);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

bool ParseMode(const std::string& spec) {
    const std::string head = spec.substr(0, spec.find(':'));
    if (head == "log") {
        g_mode = Mode::Log;
    } else if (head == "dry") {
        g_mode = Mode::Dry;
    } else if (head == "wet") {
        g_mode = Mode::Wet;
        const size_t a = spec.find(':');
        if (a != std::string::npos) {
            g_set_wetness = std::strtof(spec.c_str() + a + 1, nullptr);
            const size_t b = spec.find(':', a + 1);
            if (b != std::string::npos) {
                g_set_puddle = true;
                g_puddle = std::strtof(spec.c_str() + b + 1, nullptr);
            }
        }
        if (!(g_set_wetness >= 0.0f && g_set_wetness <= 4.0f) || !(g_puddle >= 0.0f && g_puddle <= 4.0f)) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir) {
    g_spec = Env("SSPC_R12_WEATHER");
    if (g_spec.empty()) {
        return false;
    }
    if (!ParseMode(g_spec)) {
        SSPC_LOG(Engine, Error, "Weather: SSPC_R12_WEATHER={} not understood (log | dry | wet[:<w>[:<puddle>]]): off",
                 g_spec);
        return false;
    }
    if (const std::string s = Env("SSPC_R12_WEATHER_AT"); !s.empty()) {
        g_at = std::strtod(s.c_str(), nullptr);
    }
    if (const std::string s = Env("SSPC_R12_WEATHER_UNTIL"); !s.empty()) {
        g_until = std::strtod(s.c_str(), nullptr);
    }
    if (const std::string s = Env("SSPC_R12_WEATHER_LOG"); !s.empty()) {
        g_period = std::max(0.5, std::strtod(s.c_str(), nullptr));
    }
    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "Weather tool not installed: {}", error);
            return false;
        }
    }
    if (!VerifyGuestFunction(eboot_base, kTodUpdateSite.function, error) ||
        !VerifyGuestFunction(eboot_base, kWetnessPassSite.function, error)) {
        SSPC_LOG(Engine, Error, "Weather tool not installed: {}", error);
        return false;
    }
    g_base = eboot_base;
    // (the analysed addresses are module-relative: the eboot's globals and code from here on through g_base)
    void* tod = InstallEntryProbe(eboot_base, kTodUpdateSite, reinterpret_cast<const void*>(&TodUpdateProbe), error,
                                  reinterpret_cast<void**>(&g_tod_update));
    if (tod == nullptr) {
        SSPC_LOG(Engine, Error, "Weather tool not installed: {}", error);
        return false;
    }
    g_tod_update = reinterpret_cast<TodUpdateFn>(tod);
    void* pass = InstallEntryProbe(eboot_base, kWetnessPassSite, reinterpret_cast<const void*>(&WetnessPassProbe),
                                   error, reinterpret_cast<void**>(&g_wetness_pass));
    if (pass == nullptr) {
        SSPC_LOG(Engine, Error, "Weather tool: the wetness pass counter not installed ({}); the rest works", error);
    } else {
        g_wetness_pass = reinterpret_cast<WetnessPassFn>(pass);
    }
    std::string path = Env("SSPC_R12_WEATHER_FILE");
    if (path.empty()) {
        path = (logs_dir / "weather.log").string();
    }
    g_file = std::fopen(path.c_str(), "wb");
    if (g_file != nullptr) {
        std::fputs("# SecondSonPC R12 weather journal: process s, frame-log s (gpu_frames.csv, SSPC_DIAG_CAPTURE_FRAMES "
                   "t), the game's main-loop frame, event\n",
                   g_file);
    }
    Journal(fmt::format("DIAGNOSTICS: SSPC_R12_WEATHER={} (at {:.0f} s, until {}, journal every {:.1f} s) -> {}", g_spec,
                        g_at, g_until >= 0.0 ? fmt::format("{:.0f} s", g_until) : std::string("exit"), g_period,
                        g_file != nullptr ? path : std::string("(journal file not opened)")),
            false);
    SSPC_LOG(Engine, Warning,
             "DIAGNOSTICS: SSPC_R12_WEATHER={} - the weather tool's probes on the time-of-day update 0x59cf80{}; "
             "journal {}",
             g_spec, g_wetness_pass != nullptr ? " and the wetness pass 0x597ec0" : "", path);
    // (the orderly exit runs its steps and ends with ExitProcess: no atexit handler runs; the Reports stage comes
    // first, while the game still runs and before the save data backup)
    Lifecycle::AddExitStep(Lifecycle::Stage::Reports, "weather tool: restore", AtExit);
    return true;
}

} // namespace SecondSon::Engine::Render::Weather
