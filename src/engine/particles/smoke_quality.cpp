// SecondSonPC — Smoke Quality (see smoke_quality.h).
#include "engine/particles/smoke_quality.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Particles {

namespace {

// --- the probed function (eboot.bin of CUSA00004; tools/native-reconstruction/function_hashes.py) -------------------
// EmitterSpawn(CParticleEmitterInstance*) -> bool: prologue push rbp/mov rbp,rsp/push r15..rbx/sub rsp,0xe8 (20 bytes,
// position-independent, no branch into it; ghidra/queries/prologue.py).
constexpr ProbeSite kSpawnSite{{0x3f3580, 0x7a7, 0x91c01916f8965f17ull}, 20};
// Functions whose behaviour the probe assumes.
constexpr GuestFunction kAssumed[] = {
    {0x3f3280, 0x2f9, 0xafefbaab910557e4ull}, // the only caller: EmitterSpawn once per emitter per frame
    {0x3efde0, 0xb01, 0x4274811e7ad63eb6ull}, // EmitterUpdate: pending (+0x78) -> new particles of the GPU dispatch
};

// CParticleEmitterInstance
constexpr u64 kEmitterData = 0x28;   // SParticleEmitterNewData*
constexpr u64 kEmitterSystem = 0x30; // CParticleSystemInstance*
constexpr u64 kPending = 0x78;       // s32: particles to spawn at the next update
constexpr u64 kInterval = 0x80;      // f32: spawn spacing within the frame (rate mode)
constexpr u64 kSubIndex = 0x90;      // s32: which copy of the system's emitters
constexpr u64 kNextEmitter = 0xb0;   // next emitter of the same system
constexpr u64 kGpuBlock = 0xe8;      // the emission dispatch's constants: +8 s32 live particles
// SParticleEmitterNewData
constexpr u64 kDataId = 0x18;          // u64: the emitter's id (a child names its parent by it)
constexpr u64 kDataFlags = 0x60;       // bit 2: instances share one pending count (+0x164)
constexpr u64 kDataDrawKind = 0x78;    // s32: 1 = the sorted translucent list (smoke draw), others per-emitter draws
constexpr u64 kDataParent = 0x108;     // u64: parent emitter id, ~0 = none
constexpr u64 kDataSharedPending = 0x164;
// CParticleSystemInstance
constexpr u64 kSystemFirstEmitter = 0x58;

using SpawnFn = SSPC_SYSV_ABI u64 (*)(u8* emitter);

SpawnFn g_spawn = nullptr;

// --- Particle Resolution: the engine's own reduced-resolution translucent particles ----------------------------------
// TranslucentParticles 0x3ed7b0(manager, view, skip) draws "particle_translucent" (the sorted list: fire, smoke, dust;
// VS 0x2edac4000) and first adapts the manager's level (+0xca0, 0..2) to the particle pass's GPU time: up when the
// pass takes more than 10 % of a 30 fps frame and the frame is GPU-bound, down when there is room (thresholds
// 0xaaa900; hysteresis counter 0x7d3cea8). Level 0 draws the technique in pass 25 (full-resolution HDR); level 1/2
// in pass 28, whose setup 0x1923f0 sizes its target at screen x {1, 0.5, 0.25}[level] (0xaaa918) and whose end
// 0x1926e0 upsamples it into the HDR target. The port's GPU timers do not give the PS4's values, so the level stays 0;
// a forced level is written before each call with the hysteresis counter at 0 (the function then skips adapting).
constexpr ProbeSite kTranslucentSite{{0x3ed7b0, 0x2ef, 0x1de2d1e52e4cf14eull}, 17};
constexpr GuestFunction kTranslucentAssumed[] = {
    {0x1923f0, 0x156, 0xba69aab1c0ded83cull}, // pass 28 setup: target = screen x scale[level]
    {0x3ed790, 0x14, 0x801e919fc4d9c52cull},  // scale[level] = 0xaaa918[manager+0xca0]
    {0x447df0, 0xf, 0xb68b4cc0e34a5a3cull},   // technique pass = 0x19 (level 0) / 0x1c (level >= 1)
};
constexpr u64 kParticleManager = 0x7d3eb10; // eboot data
constexpr u64 kManagerLevel = 0xca0;
constexpr u64 kLevelHysteresis = 0x7d3cea8;
using TranslucentFn = SSPC_SYSV_ABI void (*)(u8* manager, u8* view, u32 skip_draw);
TranslucentFn g_translucent = nullptr;
u64 g_eboot = 0;
std::atomic<s32> g_particle_level{-1}; // -1: the game's own adaptation
std::atomic<bool> g_level_restore{false}; // back to auto after a forced level: the starting level written once
std::atomic<float> g_density{1.0f};
// the probes installed (the settings thread reads them); a setting changed before (the menu opens earlier) is kept
std::atomic<bool> g_spawn_installed{false}, g_translucent_installed{false};
std::atomic<bool> g_quality_live{false}, g_resolution_live{false};
u32 g_smoke_kinds = ~0u; // draw kinds considered (SSPC_SMOKE_KINDS restricts; the name decides)
u32 g_stats_seconds = 0;

template <typename T>
T At(const u8* address) {
    T value;
    std::memcpy(&value, address, sizeof(T));
    return value;
}
template <typename T>
void Store(u8* address, T value) {
    std::memcpy(address, &value, sizeof(T));
}
u8* Ptr(const u8* address) {
    return At<u8*>(address);
}

std::atomic<u64> g_translucent_calls{0};

SSPC_SYSV_ABI void TranslucentProbe(u8* manager, u8* view, u32 skip_draw) {
    const s32 level = g_particle_level.load(std::memory_order_relaxed);
    if (reinterpret_cast<u64>(manager) == g_eboot + kParticleManager &&
        (level >= 0 || g_level_restore.exchange(false, std::memory_order_relaxed))) {
        // a forced level; back to auto: the level the game starts with (0) - its adaptation gates on GPU timers the
        // port does not give it, so a forced half/quarter would otherwise stay
        Store<s32>(manager + kManagerLevel, std::max(level, 0));
        Store<s32>(reinterpret_cast<u8*>(g_eboot + kLevelHysteresis), 0);
    }
    g_translucent(manager, view, skip_draw);
    // diagnostics (SSPC_SMOKE_STATS): what the call left - the level, the switched technique's pass (+0x110) and the
    // state field that gates the switch (technique+0x10 -> +0x28: not 0 or 6), every 60th call
    if (g_stats_seconds != 0 && (g_translucent_calls.fetch_add(1, std::memory_order_relaxed) % 60) == 0) {
        const u8* tech = Ptr(manager + 0x90);
        const u8* state = tech != nullptr ? Ptr(tech + 0x10) : nullptr;
        SSPC_LOG(Engine, Info,
                 "Particle Resolution: call skip={} level {} hysteresis {} | technique {} pass {:#x} state mode {} | "
                 "ui particles {} | flag e16bb9 {}",
                 skip_draw & 0xff, At<s32>(manager + kManagerLevel),
                 At<s32>(reinterpret_cast<const u8*>(g_eboot + kLevelHysteresis)), fmt::ptr(tech),
                 tech != nullptr ? At<u32>(tech + 0x110) : 0u, state != nullptr ? At<s32>(state + 0x28) : -1,
                 At<s32>(manager + 0x78), At<u8>(reinterpret_cast<const u8*>(g_eboot + 0xe16bb9)));
    }
}

bool SmokeKind(s32 kind) {
    return kind >= 0 && kind < 32 && (g_smoke_kinds & (1u << kind)) != 0;
}

// SSPC_SMOKE_SELECT (diagnostics): conditions on the emitter data, all of which a smoke emitter must also meet -
// "<f|u|b><hex offset><op><value>" joined by ','; f float, u u32, b u8; op = ! < > & (& = any bit of value set).
struct Condition {
    char type;
    u32 offset;
    char op;
    double value;
};
std::array<Condition, 8> g_select{};
u32 g_select_count = 0;

bool Selected(const u8* d) {
    for (u32 i = 0; i < g_select_count; ++i) {
        const Condition& c = g_select[i];
        const double x = c.type == 'f'   ? static_cast<double>(At<float>(d + c.offset))
                         : c.type == 'u' ? static_cast<double>(At<u32>(d + c.offset))
                                         : static_cast<double>(At<u8>(d + c.offset));
        const bool ok = c.op == '='   ? x == c.value
                        : c.op == '!' ? x != c.value
                        : c.op == '<' ? x < c.value
                        : c.op == '>' ? x > c.value
                                      : (static_cast<u64>(x) & static_cast<u64>(c.value)) != 0;
        if (!ok) {
            return false;
        }
    }
    return true;
}

// --- what an emitter is: its name ------------------------------------------------------------------------------------
// The emitter data's id (+0x18) is its name hashed with the engine's name hash (the reflection table's 12/12 pairs):
// h = h * 101 + lower(c) mod 2^64. A name of up to ~9 characters is h itself in base 101 (a character code is a
// digit; 101..122 'e'..'z' only as r + 101 for a remainder r < 22, so the digits are unique); a longer one wrapped
// 2^64 k times, tried for k < kWraps (names up to ~12 characters). A name that is not recovered is checked against
// hashes of longer smoke names built from common words. Only a recovered name that contains "smoke" (or an exact
// dictionary match) makes an emitter smoke: fire, embers, steam, dust, foam stay the game's.
constexpr u32 kWraps = 1u << 15;

u64 NameHash(std::string_view s) {
    u64 h = 0;
    for (const char ch : s) {
        const u8 c = static_cast<u8>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        h = h * 101 + c;
    }
    return h;
}

bool NameChar(u32 c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/// The name whose hash is h after `wraps` overflows of 2^64, or false.
bool DecodeName(u64 h, u32 wraps, std::string& out) {
    u32 limb[3] = {static_cast<u32>(h), static_cast<u32>(h >> 32), wraps}; // the value, little-endian 32-bit limbs
    char rev[32];
    u32 n = 0;
    while ((limb[0] | limb[1] | limb[2]) != 0) {
        u64 rem = 0;
        for (int i = 2; i >= 0; --i) {
            const u64 cur = (rem << 32) | limb[i];
            limb[i] = static_cast<u32>(cur / 101);
            rem = cur % 101;
        }
        u32 c = static_cast<u32>(rem);
        if (c < 22) {
            // 'e'..'z': the quotient is one less
            c += 101;
            if ((limb[0] | limb[1] | limb[2]) == 0) {
                return false;
            }
            for (int i = 0; i < 3; ++i) { // limb -= 1 with borrow
                if (limb[i]-- != 0) {
                    break;
                }
            }
        }
        if (!NameChar(c) || n == sizeof(rev)) {
            return false;
        }
        rev[n++] = static_cast<char>(c);
    }
    out.assign(std::make_reverse_iterator(rev + n), std::make_reverse_iterator(rev));
    return n != 0;
}

std::vector<u64> g_smoke_dictionary; // sorted hashes of longer smoke names
std::vector<std::string> g_smoke_words = {"smoke"}; // SSPC_SMOKE_MATCH: what a recovered name must contain

void BuildSmokeDictionary() {
    static constexpr std::string_view kWords[] = {
        "fire", "camp", "campfire", "bonfire", "torch", "house", "roof", "chimney", "car", "vehicle", "truck",
        "building", "window", "debris", "rubble", "ground", "large", "small", "big", "near", "far", "thick", "thin",
        "dark", "black", "grey", "white", "light", "heavy", "soft", "column", "plume", "puff", "puffs", "wisp",
        "trail", "burst", "ring", "cloud", "rising", "lingering", "base", "top", "low", "high", "wide", "tall",
        "ambient", "area", "volume", "distant", "explosion", "impact", "hit", "dash", "bolt", "missile", "drop",
        "orbital", "power", "smother", "grenade", "vent", "launch", "hero", "player", "delsin", "absorb", "drain"};
    static constexpr std::string_view kSuffixes[] = {"", "1", "2", "3", "01", "02", "03", "_1", "_2", "_01", "_02",
                                                     "_a", "_b", "a", "b"};
    std::vector<std::string> bases = {"smoke"};
    for (const auto w : kWords) {
        bases.push_back(fmt::format("{}_smoke", w));
        bases.push_back(fmt::format("{}smoke", w));
        bases.push_back(fmt::format("smoke_{}", w));
        for (const auto w2 : kWords) {
            bases.push_back(fmt::format("{}_{}_smoke", w, w2));
            bases.push_back(fmt::format("{}_smoke_{}", w, w2));
            bases.push_back(fmt::format("smoke_{}_{}", w, w2));
        }
    }
    g_smoke_dictionary.clear();
    for (const auto& b : bases) {
        for (const auto s : kSuffixes) {
            g_smoke_dictionary.push_back(NameHash(b + std::string(s)));
        }
    }
    std::ranges::sort(g_smoke_dictionary);
}

bool SmokeName(const std::string& name) {
    return std::ranges::any_of(g_smoke_words, [&](const std::string& w) { return name.find(w) != std::string::npos; });
}

/// The name (or "") and whether the emitter with this id is smoke. Pure arithmetic, ~a few hundred us at most.
std::once_flag g_dictionary_built;

void EnsureSmokeDictionary() {
    std::call_once(g_dictionary_built, BuildSmokeDictionary); // (~230k names: built in Install, not mid-game)
}

std::pair<std::string, bool> Classify(u64 id) {
    EnsureSmokeDictionary();
    std::string name, first;
    for (u32 k = 0; k < kWraps; ++k) {
        if (DecodeName(id, k, name)) {
            if (SmokeName(name)) {
                return {name, true};
            }
            if (first.empty()) {
                first = name; // a short name decodes at k = 0 (the true one); later ones may be chance
            }
        }
    }
    if (std::ranges::binary_search(g_smoke_dictionary, id)) {
        return {"(dictionary) smoke", true};
    }
    return {first, false};
}

struct DataClass {
    u64 id = 0;
    bool smoke = false;
};
std::mutex g_class_lock;
std::unordered_map<u64, DataClass> g_classes; // emitter data address -> class (id checked: an address may be reused)
std::unordered_map<u64, bool> g_class_by_id;  // emitter id -> smoke (one id, many emitter datas: decoded once)
std::vector<std::pair<u64, std::string>> g_new_names; // classified since the last stats report (id, name + class)

bool SmokeEmitter(const u8* d) {
    if (!SmokeKind(At<s32>(d + kDataDrawKind)) || (g_select_count != 0 && !Selected(d))) {
        return false;
    }
    const u64 id = At<u64>(d + kDataId);
    {
        std::lock_guard guard(g_class_lock);
        const auto it = g_classes.find(reinterpret_cast<u64>(d));
        if (it != g_classes.end() && it->second.id == id) {
            return it->second.smoke;
        }
        if (const auto by_id = g_class_by_id.find(id); by_id != g_class_by_id.end()) {
            if (g_classes.size() > 65536) {
                g_classes.clear();
            }
            g_classes[reinterpret_cast<u64>(d)] = {id, by_id->second};
            return by_id->second;
        }
    }
    const auto [name, smoke] = Classify(id); // outside the lock: other emitters go on
    std::lock_guard guard(g_class_lock);
    if (g_classes.size() > 65536) {
        g_classes.clear();
    }
    if (g_class_by_id.size() > 65536) {
        g_class_by_id.clear();
    }
    g_classes[reinterpret_cast<u64>(d)] = {id, smoke};
    g_class_by_id[id] = smoke;
    if (g_stats_seconds != 0 && g_new_names.size() < 4096) {
        g_new_names.push_back({id, fmt::format("{}{}", name.empty() ? "?" : name, smoke ? " [smoke]" : "")});
    }
    return smoke;
}

/// The parent emitter instance of `e` (same system, same copy), as the game finds it in EmitterSpawn.
u8* FindParent(const u8* e, u64 parent_id) {
    const u8* system = Ptr(e + kEmitterSystem);
    if (system == nullptr) {
        return nullptr;
    }
    const s32 sub = At<s32>(e + kSubIndex);
    u8* p = Ptr(system + kSystemFirstEmitter);
    for (u32 guard = 0; p != nullptr && guard < 4096; ++guard) {
        const u8* pd = Ptr(p + kEmitterData);
        if (pd != nullptr && At<u64>(pd + kDataId) == parent_id && At<s32>(p + kSubIndex) == sub) {
            return p;
        }
        p = Ptr(p + kNextEmitter);
    }
    return nullptr;
}

/// True when the particles of `e` are fewer than the game's: it is reduced itself, or it spawns from a parent that is.
bool Reduced(const u8* e, u32 depth);

/// True when `e` is reduced itself: a smoke emitter whose parent (if any) is not reduced.
bool ReducedItself(const u8* e, const u8* d, u32 depth) {
    if (!SmokeEmitter(d)) {
        return false;
    }
    const u64 parent_id = At<u64>(d + kDataParent);
    if (parent_id == ~u64{0} || depth >= 4) {
        return true;
    }
    const u8* parent = FindParent(e, parent_id);
    return parent == nullptr || !Reduced(parent, depth + 1);
}

bool Reduced(const u8* e, u32 depth) {
    const u8* d = Ptr(e + kEmitterData);
    if (d == nullptr) {
        return false;
    }
    if (ReducedItself(e, d, depth)) {
        return true;
    }
    const u64 parent_id = At<u64>(d + kDataParent);
    if (parent_id == ~u64{0} || depth >= 4) {
        return false;
    }
    const u8* parent = FindParent(e, parent_id);
    return parent != nullptr && Reduced(parent, depth + 1);
}

// --- per-emitter state -----------------------------------------------------------------------------------------------
struct Track {
    float carry = 0.5f; // a new emitter rounds to nearest
    u32 calls = 0;      // in the current stats window
};
std::mutex g_tracks_lock;
std::unordered_map<u64, Track> g_tracks; // emitter instance address -> state (stale addresses only lose a fraction)
constexpr size_t kMaxTracks = 32768;

// --- statistics (SSPC_SMOKE_STATS) -----------------------------------------------------------------------------------
constexpr u32 kKinds = 9; // draw kinds 0..7, 8 = any other value
struct KindStats {
    u64 calls = 0;
    u64 reduced_calls = 0;
    u64 added = 0;
    u64 kept = 0;
    u64 live = 0;
    std::unordered_set<u64> emitters;
    std::unordered_set<u64> datas;
};
std::array<KindStats, kKinds> g_kinds; // under g_tracks_lock
struct SmokeTotals {
    u64 added = 0, kept = 0, live = 0;
};
SmokeTotals g_smoke; // smoke emitters (SmokeEmitter: kind and SSPC_SMOKE_SELECT), under g_tracks_lock

// SSPC_SMOKE_DUMP=1 (diagnostics): per emitter data, its spawn and live particles over the run and its first 0x1c0
// bytes -> logs\smoke_emitters.txt, rewritten at every stats report (the fields that tell fire from smoke).
constexpr u32 kDumpBytes = 0x1c0;       // SSPC_SMOKE_SELECT offsets stay inside the fixed part
constexpr u32 kDumpDataBytes = 0x1400;  // the data with the blocks it carries right after it (+0x90..+0x1190)
constexpr u32 kDumpCurveBytes = 0x400;  // what +0x180 points to (GPU memory)
struct DataDump {
    u64 calls = 0;
    u64 added = 0;
    u64 live = 0;
    u64 reduced = 0;
    std::vector<u32> words;  // kDumpDataBytes, zero where unreadable
    std::vector<u32> curves; // kDumpCurveBytes at *(data + 0x180)
};

bool ReadableRange(const void* p, size_t bytes) {
    const u8* at = static_cast<const u8*>(p);
    const u8* end = at + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(at, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        at = static_cast<const u8*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

std::vector<u32> Snapshot(const u8* p, u32 bytes) {
    std::vector<u32> v(bytes / 4, 0);
    if (p != nullptr && ReadableRange(p, bytes)) {
        std::memcpy(v.data(), p, bytes);
    }
    return v;
}
bool g_dump = false;
std::unordered_map<u64, DataDump> g_dumps; // under g_tracks_lock

Track& TrackOf(const u8* e) {
    if (g_tracks.size() > kMaxTracks) {
        g_tracks.clear();
    }
    return g_tracks[reinterpret_cast<u64>(e)];
}

SSPC_SYSV_ABI u64 SpawnProbe(u8* e) {
    const float density = g_density.load(std::memory_order_relaxed);
    const bool stats = g_stats_seconds != 0;
    if (density >= 1.0f && !stats) {
        return g_spawn(e);
    }
    u8* d = Ptr(e + kEmitterData);
    const s32 before = At<s32>(e + kPending);
    const bool shared = d != nullptr && (At<u8>(d + kDataFlags) & 4) != 0;
    const s32 shared_before = shared ? At<s32>(d + kDataSharedPending) : 0;

    const u64 result = g_spawn(e);

    if (d == nullptr) {
        return result;
    }
    const s32 added = At<s32>(e + kPending) - before;
    const s32 kind = At<s32>(d + kDataDrawKind);
    const u8* gpu = Ptr(e + kGpuBlock);
    const s32 live = gpu != nullptr ? At<s32>(gpu + 8) : 0;
    // classified (once per emitter data) outside the stats lock, and only when it matters
    const bool smoke = ((density < 1.0f && added > 0) || stats) && SmokeEmitter(d);
    const bool reduce = density < 1.0f && added > 0 && smoke && ReducedItself(e, d, 0);

    u32 kept = added > 0 ? static_cast<u32>(added) : 0;
    if (reduce || stats) {
        std::lock_guard guard(g_tracks_lock);
        Track& t = TrackOf(e);
        if (reduce) {
            kept = ScaleSpawn(static_cast<u32>(added), density, t.carry, live <= 0);
        }
        if (stats) {
            ++t.calls;
            KindStats& k = g_kinds[kind >= 0 && kind < 8 ? kind : 8];
            ++k.calls;
            k.reduced_calls += reduce ? 1 : 0;
            k.added += added > 0 ? static_cast<u64>(added) : 0;
            k.kept += kept;
            k.live += live > 0 ? static_cast<u64>(live) : 0;
            k.emitters.insert(reinterpret_cast<u64>(e));
            k.datas.insert(reinterpret_cast<u64>(d));
            if (smoke) {
                g_smoke.added += added > 0 ? static_cast<u64>(added) : 0;
                g_smoke.kept += kept;
                g_smoke.live += live > 0 ? static_cast<u64>(live) : 0;
            }
            if (g_dump) {
                DataDump& dd = g_dumps[reinterpret_cast<u64>(d)];
                if (dd.calls++ == 0) {
                    dd.words = Snapshot(d, kDumpDataBytes);
                    dd.curves = Snapshot(Ptr(d + 0x180), kDumpCurveBytes);
                }
                dd.added += added > 0 ? static_cast<u64>(added) : 0;
                dd.live += live > 0 ? static_cast<u64>(live) : 0;
                dd.reduced += reduce ? 1 : 0;
            }
        }
    }
    if (reduce && kept < static_cast<u32>(added)) {
        const s32 dropped = added - static_cast<s32>(kept);
        Store<s32>(e + kPending, before + static_cast<s32>(kept));
        if (shared) {
            // The game added the same count to the shared pending count; drop the same part of it.
            const s32 now = At<s32>(d + kDataSharedPending);
            if (now - shared_before == added) {
                Store<s32>(d + kDataSharedPending, now - dropped);
            }
        }
        if (kept > 0) {
            // Rate mode spaces the new particles over the frame by +0x80: keep the kept ones over the same time.
            const float interval = At<float>(e + kInterval);
            if (interval > 0.0f && std::isfinite(interval)) {
                Store<float>(e + kInterval, interval * static_cast<float>(added) / static_cast<float>(kept));
            }
        }
    }
    return result;
}

void Reporter(u32 seconds) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::array<KindStats, kKinds> k;
        SmokeTotals smoke;
        u32 frames = 0;
        {
            std::lock_guard guard(g_tracks_lock);
            for (auto& [addr, t] : g_tracks) {
                frames = std::max(frames, t.calls);
                t.calls = 0;
            }
            k = std::move(g_kinds);
            g_kinds = {};
            smoke = g_smoke;
            g_smoke = {};
            if (g_dump) {
                std::vector<std::pair<u64, const DataDump*>> v;
                for (const auto& [addr, dd] : g_dumps) {
                    v.push_back({addr, &dd});
                }
                std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second->added > b.second->added; });
                FILE* f = nullptr;
                if (fopen_s(&f, "logs\\smoke_emitters.txt", "w") == 0 && f != nullptr) {
                    std::fprintf(f, "# data calls spawned live_sum reduced_calls | u32 words 0x0..0x%x | u32 words at "
                                    "*(data+0x180) 0x0..0x%x\n",
                                 kDumpDataBytes, kDumpCurveBytes);
                    for (const auto& [addr, dd] : v) {
                        std::fprintf(f, "%llx %llu %llu %llu %llu |", static_cast<unsigned long long>(addr),
                                     static_cast<unsigned long long>(dd->calls),
                                     static_cast<unsigned long long>(dd->added),
                                     static_cast<unsigned long long>(dd->live),
                                     static_cast<unsigned long long>(dd->reduced));
                        for (const u32 w : dd->words) {
                            std::fprintf(f, " %08x", w);
                        }
                        std::fputs(" |", f);
                        for (const u32 w : dd->curves) {
                            std::fprintf(f, " %08x", w);
                        }
                        std::fputc('\n', f);
                    }
                    std::fclose(f);
                }
            }
        }
        std::string line = fmt::format("Smoke Quality stats ({} s, ~{} frames, density {:.2f}):", seconds, frames,
                                       g_density.load(std::memory_order_relaxed));
        const u64 smoke_added = smoke.added, smoke_kept = smoke.kept, smoke_live = smoke.live;
        for (u32 i = 0; i < kKinds; ++i) {
            const KindStats& s = k[i];
            if (s.calls == 0) {
                continue;
            }
            const bool smoke_kind = i < 8 && SmokeKind(static_cast<s32>(i));
            line += fmt::format(" | kind {}{}: emitters {} (data {}), reduced calls {}, spawn/s {:.0f} -> {:.0f}, "
                                "live/frame {:.0f}",
                                i == 8 ? std::string("other") : std::to_string(i), smoke_kind ? "*" : "", s.emitters.size(),
                                s.datas.size(), s.reduced_calls, static_cast<double>(s.added) / seconds,
                                static_cast<double>(s.kept) / seconds,
                                frames ? static_cast<double>(s.live) / frames : 0.0);
        }
        line += fmt::format(" || smoke: spawn/s {:.0f} -> {:.0f} ({:.0f} %), live/frame {:.0f}",
                            static_cast<double>(smoke_added) / seconds, static_cast<double>(smoke_kept) / seconds,
                            smoke_added ? 100.0 * static_cast<double>(smoke_kept) / static_cast<double>(smoke_added)
                                        : 100.0,
                            frames ? static_cast<double>(smoke_live) / frames : 0.0);
        SSPC_LOG(Engine, Info, "{}", line);
        std::vector<std::pair<u64, std::string>> names;
        {
            std::lock_guard guard(g_class_lock);
            names.swap(g_new_names);
        }
        for (size_t i = 0; i < names.size(); i += 16) {
            std::string list;
            for (size_t j = i; j < names.size() && j < i + 16; ++j) {
                list += fmt::format("{}{:016x} {}", j == i ? "" : ", ", names[j].first, names[j].second);
            }
            SSPC_LOG(Engine, Info, "Smoke Quality emitters: {}", list);
        }
    }
}

std::string Env(const char* name) {
    char v[256] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

} // namespace

std::optional<float> SmokeDensity(std::string_view quality) {
    if (quality == "high") {
        return 1.0f;
    }
    if (quality == "medium") {
        return 0.65f;
    }
    if (quality == "low") {
        return 0.35f;
    }
    if (quality == "verylow") {
        return 0.20f;
    }
    float x = 0.0f;
    const auto [end, ec] = std::from_chars(quality.data(), quality.data() + quality.size(), x);
    if (ec == std::errc{} && end == quality.data() + quality.size() && x >= 0.0f && x <= 1.0f) {
        return x;
    }
    return std::nullopt;
}

u32 ScaleSpawn(u32 added, float density, float& carry, bool nothing_alive) {
    if (density >= 1.0f || added == 0) {
        return added;
    }
    density = std::max(density, 0.0f);
    const float x = static_cast<float>(added) * density + carry;
    u32 kept = x > 0.0f ? std::min(static_cast<u32>(std::floor(x)), added) : 0;
    carry = x - static_cast<float>(kept);
    if (kept == 0 && nothing_alive && density > 0.0f) {
        kept = 1; // never let an emitter with nothing alive stay invisible
        carry = x - 1.0f;
    }
    carry = std::clamp(carry, -1.0f, 1.0f);
    return kept;
}

std::optional<s32> ParticleLevel(std::string_view resolution) {
    if (resolution == "auto") {
        return -1;
    }
    if (resolution == "full") {
        return 0;
    }
    if (resolution == "half") {
        return 1;
    }
    if (resolution == "quarter") {
        return 2;
    }
    return std::nullopt;
}

namespace {

void InstallParticleResolution(u64 eboot_base, std::string_view resolution) {
    const std::string env = Env("SSPC_PARTICLE_RES");
    const std::string_view chosen = env.empty() ? resolution : std::string_view(env);
    std::optional<s32> level = ParticleLevel(chosen);
    if (!level) {
        SSPC_LOG(Engine, Warning, "Particle Resolution: '{}' is not auto|full|half|quarter - the game's own", chosen);
        level = -1;
    }
    std::string error;
    for (const GuestFunction& f : kTranslucentAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "Particle Resolution not installed (the game's own): {}", error);
            return;
        }
    }
    void* original =
        InstallEntryProbe(eboot_base, kTranslucentSite, reinterpret_cast<const void*>(&TranslucentProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Particle Resolution not installed (the game's own): {}", error);
        return;
    }
    g_eboot = eboot_base;
    g_translucent = reinterpret_cast<TranslucentFn>(original);
    if (!env.empty() || !g_resolution_live.load(std::memory_order_relaxed)) {
        g_particle_level.store(*level, std::memory_order_relaxed); // (else: the menu's choice, made meanwhile)
    }
    level = g_particle_level.load(std::memory_order_relaxed);
    g_translucent_installed.store(true, std::memory_order_release);
    const std::string what = fmt::format(
        "Particle Resolution: entry probe on TranslucentParticles 0x3ed7b0, '{}'{} - {}", chosen,
        env.empty() ? "" : " (SSPC_PARTICLE_RES)",
        *level < 0 ? "the game's own level (adapts to its GPU timers)"
                   : fmt::format("level {} forced (translucent particles at {} resolution)", *level,
                                 *level == 0 ? "full" : *level == 1 ? "half" : "quarter"));
    if (*level > 0) {
        SSPC_LOG(Engine, Warning, "{}", what);
    } else {
        SSPC_LOG(Engine, Info, "{}", what);
    }
}

} // namespace

void SetParticleResolution(std::string_view resolution) {
    const std::optional<s32> level = ParticleLevel(resolution);
    if (!level) {
        return;
    }
    const s32 old = g_particle_level.exchange(*level, std::memory_order_relaxed);
    if (*level < 0 && old >= 0) {
        g_level_restore.store(true, std::memory_order_relaxed);
    }
    g_resolution_live.store(true, std::memory_order_relaxed);
    SSPC_LOG(Engine, Info, "Particle Resolution: '{}' (level {}){}", resolution, *level,
             g_translucent_installed.load(std::memory_order_acquire) ? "" : " - applies when installed");
}

bool Install(u64 eboot_base, std::string_view quality, std::string_view resolution) {
    if (Env("SSPC_SMOKE_HOOK") == "0") {
        SSPC_LOG(Engine, Info, "Smoke Quality / Particle Resolution: SSPC_SMOKE_HOOK=0 - not installed");
        return false;
    }
    InstallParticleResolution(eboot_base, resolution);
    const std::string env_quality = Env("SSPC_SMOKE_QUALITY");
    const std::string_view chosen = env_quality.empty() ? quality : std::string_view(env_quality);
    std::optional<float> density = SmokeDensity(chosen);
    if (!density) {
        SSPC_LOG(Engine, Warning, "Smoke Quality: '{}' is not high|medium|low|verylow|0..1 - the original density",
                 chosen);
        density = 1.0f;
    }
    if (const std::string kinds = Env("SSPC_SMOKE_KINDS"); !kinds.empty()) {
        u32 mask = 0;
        for (size_t at = 0; at < kinds.size();) {
            const size_t comma = std::min(kinds.find(',', at), kinds.size());
            u32 k = 0;
            const auto [end, ec] = std::from_chars(kinds.data() + at, kinds.data() + comma, k);
            if (ec == std::errc{} && k < 32) {
                mask |= 1u << k;
            }
            at = comma + 1;
        }
        g_smoke_kinds = mask;
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_SMOKE_KINDS={} - only these emitter draw kinds are considered "
                                  "(mask {:#x})",
                 kinds, mask);
    }
    if (const std::string s = Env("SSPC_SMOKE_STATS"); !s.empty()) {
        g_stats_seconds = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
    }
    if (const std::string m = Env("SSPC_SMOKE_MATCH"); !m.empty()) {
        g_smoke_words.clear();
        for (size_t at = 0; at < m.size();) {
            const size_t comma = std::min(m.find(',', at), m.size());
            if (comma > at) {
                g_smoke_words.push_back(m.substr(at, comma - at));
            }
            at = comma + 1;
        }
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_SMOKE_MATCH={} - emitters whose name contains one of these are "
                                  "reduced (instead of 'smoke')",
                 m);
    }
    if (Env("SSPC_SMOKE_DUMP") == "1") {
        g_dump = true;
        g_stats_seconds = g_stats_seconds != 0 ? g_stats_seconds : 10;
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_SMOKE_DUMP=1 - emitter data dumped to logs\\smoke_emitters.txt");
    }
    if (const std::string s = Env("SSPC_SMOKE_SELECT"); !s.empty()) {
        for (size_t at = 0; at < s.size() && g_select_count < g_select.size();) {
            const size_t comma = std::min(s.find(',', at), s.size());
            const std::string c = s.substr(at, comma - at);
            const size_t op = c.find_first_of("=!<>&");
            if (c.size() > 2 && op != std::string::npos && op > 1 && std::string("fub").find(c[0]) != std::string::npos) {
                const u32 offset = static_cast<u32>(std::strtoul(c.substr(1, op - 1).c_str(), nullptr, 16));
                if (offset <= kDumpBytes - 4) {
                    g_select[g_select_count++] = {c[0], offset, c[op], std::strtod(c.c_str() + op + 1, nullptr)};
                }
            }
            at = comma + 1;
        }
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_SMOKE_SELECT={} - {} condition(s) on the emitter data", s,
                 g_select_count);
    }

    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "Smoke Quality not installed (the original density): {}", error);
            return false;
        }
    }
    EnsureSmokeDictionary(); // (before the game runs: a live switch from high must not build it mid-game)
    void* original = InstallEntryProbe(eboot_base, kSpawnSite, reinterpret_cast<const void*>(&SpawnProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Smoke Quality not installed (the original density): {}", error);
        return false;
    }
    g_spawn = reinterpret_cast<SpawnFn>(original);
    if (!env_quality.empty() || !g_quality_live.load(std::memory_order_relaxed)) {
        g_density.store(*density, std::memory_order_relaxed); // (else: the menu's choice, made meanwhile)
    }
    density = g_density.load(std::memory_order_relaxed);
    g_spawn_installed.store(true, std::memory_order_release);
    const std::string what =
        fmt::format("Smoke Quality: entry probe on EmitterSpawn 0x3f3580, smoke density {:.2f} ('{}'{}){}", *density,
                    chosen, env_quality.empty() ? "" : ", SSPC_SMOKE_QUALITY",
                    *density == 0.0f ? " - DIAGNOSTICS: smoke spawning off" : "");
    if (*density < 1.0f) {
        SSPC_LOG(Engine, Warning, "{}", what);
    } else {
        SSPC_LOG(Engine, Info, "{}", what);
    }
    if (g_stats_seconds != 0) {
        std::thread(Reporter, g_stats_seconds).detach();
    }
    return true;
}

void SetSmokeQuality(std::string_view quality) {
    const std::optional<float> density = SmokeDensity(quality);
    if (!density) {
        return;
    }
    g_density.store(*density, std::memory_order_relaxed);
    g_quality_live.store(true, std::memory_order_relaxed);
    SSPC_LOG(Engine, Info, "Smoke Quality: '{}' - smoke density {:.2f}{}", quality, *density,
             g_spawn_installed.load(std::memory_order_acquire) ? "" : " - applies when installed");
}

float CurrentDensity() {
    return g_spawn_installed.load(std::memory_order_acquire) ? g_density.load(std::memory_order_relaxed) : 1.0f;
}

u64 EngineNameHash(std::string_view name) {
    return NameHash(name);
}

bool SmokeEmitterName(u64 id, std::string* name) {
    auto [n, smoke] = Classify(id);
    if (name != nullptr) {
        *name = std::move(n);
    }
    return smoke;
}

} // namespace SecondSon::Engine::Particles
