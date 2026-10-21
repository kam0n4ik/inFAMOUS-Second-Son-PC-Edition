// SecondSonPC — the engine's runs of mesh draws, told to the command processor (CITY-OPT Phase 5). See draw_runs.h.
#include "engine/render/draw_runs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/native_render_layer.h"
#include "engine/render/render_layout.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::Engine::Render::DrawRuns {

namespace {

namespace L = Layout;

// the same sites as the draw census (draw_census.cpp): prologues checked in Ghidra, bodies pinned by hash
constexpr ProbeSite kSites[3] = {
    {{0x1a1960, 0x225, 0xea1442f922a0a0ecull}, 20}, // mesh draw (param_1, context R, mesh instance)
    {{0x43d480, 0x106, 0xaffc4c64777b1e37ull}, 17}, // shadow casters' draw (param_1, R, records, count, mesh)
    // CITY-OPT Phase 6: the mesh pass's instanced draw (param_1, R, the instances' 0xd4-byte records, count, mesh) -
    // the G-buffer's (docs/CITY_ENGINE_RESOURCES.md); the draw census' site
    {{0x1a1770, 0x1e4, 0x5bf111e570dcaaedull}, 17},
};

// CITY-OPT Phase 6: the mesh pass's material setup (param_1, context R, material) - the PS's descriptors of the draws
// after it (docs/CITY_ENGINE_RESOURCES.md); prologue checked in Ghidra (17 bytes, no RIP operand, no branch into it)
constexpr ProbeSite kMaterialSite = {{0x448640, 0x15f, 0x22c6fc74651c7374ull}, 17};

using Fn = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64);
std::array<Fn, 3> g_orig{};
Fn g_orig_material = nullptr;
Counters g_stats;
bool g_on = false;
bool g_gbuffer = false; ///< kSites[2] installed
// the context whose material 0x448640 set last on this thread (the engine sets a context's material and draws it on
// one thread, 0x1a2030); cleared by the context's next 0x1a1770 call
thread_local u64 t_material_context = 0;

// SSPC_DIAG_ENGINE_RUNS=1 (with SSPC_CITY_ENGINE_OPT): per frame, the meshes the two functions draw and the contexts
// (pass, sub - the shadow cascades are contexts of their own) each mesh is drawn in; a log line every 10 s
struct DiagFrame {
    u64 flip = 0;
    std::unordered_map<u64, u32> contexts;                 ///< (pass << 32 | sub) -> its number (the first 32 kept)
    std::array<std::unordered_map<u64, u32>, 3> meshes;    ///< mesh -> the contexts it was drawn in (bits)
    std::array<u64, 3> calls{}, instances{};
};
struct DiagTotals {
    u64 frames = 0;
    std::array<u64, 3> calls{}, instances{}, meshes{}, multi{}, draws_of_multi{};
    u64 contexts = 0;
};
bool g_diag = false;
std::mutex g_diag_mu;
DiagFrame g_frame;
DiagTotals g_totals;

void DiagClose() {
    // (under g_diag_mu) the frame that ended into the totals
    if (g_frame.flip == 0) {
        return;
    }
    ++g_totals.frames;
    g_totals.contexts += g_frame.contexts.size();
    for (u32 i = 0; i < 3; ++i) {
        g_totals.calls[i] += g_frame.calls[i];
        g_totals.instances[i] += g_frame.instances[i];
        g_totals.meshes[i] += g_frame.meshes[i].size();
        for (const auto& [mesh, bits] : g_frame.meshes[i]) {
            const u32 n = static_cast<u32>(std::popcount(bits));
            if (n > 1) {
                ++g_totals.multi[i];
                g_totals.draws_of_multi[i] += n;
            }
        }
        g_frame.meshes[i].clear();
        g_frame.calls[i] = g_frame.instances[i] = 0;
    }
    g_frame.contexts.clear();
}

void DiagNote(u32 fn, u64 r, u64 mesh, u64 instances) {
    s32 pass = -1;
    u32 sub = 0;
    std::memcpy(&pass, reinterpret_cast<const void*>(r + L::kSlotPass), sizeof(pass));
    std::memcpy(&sub, reinterpret_cast<const void*>(r + L::kSlotSub), sizeof(sub));
    const u64 flip = Gpu::VideoOut::NextFlipId();
    const std::scoped_lock lock(g_diag_mu);
    if (flip != g_frame.flip) {
        DiagClose();
        g_frame.flip = flip;
    }
    const u64 key = (u64{static_cast<u32>(pass)} << 32) | sub;
    const auto [ctx, fresh] = g_frame.contexts.try_emplace(key, static_cast<u32>(g_frame.contexts.size()));
    ++g_frame.calls[fn];
    g_frame.instances[fn] += std::max<u64>(instances, 1);
    g_frame.meshes[fn][mesh] |= ctx->second < 32 ? 1u << ctx->second : 0u;
}

void DiagReporter() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        DiagTotals t;
        {
            const std::scoped_lock lock(g_diag_mu);
            t = g_totals;
            g_totals = {};
        }
        const double n = static_cast<double>(std::max<u64>(t.frames, 1));
        SSPC_LOG(Engine, Info, "engine runs/10s ({} frames), per frame: contexts {:.0f}; mesh draws 0x1a1960 {:.0f} of "
                               "{:.0f} distinct meshes, {:.0f} of them in more than one context ({:.0f} draws); shadow "
                               "casters' draws 0x43d480 {:.0f} with {:.0f} instances of {:.0f} distinct meshes, {:.0f} of "
                               "them in more than one context ({:.0f} draws); instanced mesh draws 0x1a1770 {:.0f} with "
                               "{:.0f} instances of {:.0f} distinct meshes",
                 t.frames, t.contexts / n, t.calls[0] / n, t.meshes[0] / n, t.multi[0] / n, t.draws_of_multi[0] / n,
                 t.calls[1] / n, t.instances[1] / n, t.meshes[1] / n, t.multi[1] / n, t.draws_of_multi[1] / n,
                 t.calls[2] / n, t.instances[2] / n, t.meshes[2] / n);
    }
}

// --- R8-A3: SSPC_R8_RENDER_BINDINGS=1 - what the CUE's binding versions could save the processor ------------------------
//
// (E:\research\r8\render\INTERFACE.md §4; needs SSPC_R7_RES_STATE: R7::ResourceHooks::fill.) After each probed call the
// CUE's VS / PS bindings: a stage whose snapshot is new since the context's previous probed draw makes the processor's
// stage memo (P4.7) miss today (other user data: the new snapshot's address); its binding version and program equal to
// the stage's previous ones (hit1) or to one of its last four (hit4) is a draw whose stage plan the version could keep.
// An upper bound for a consumer in the processor (it still checks memory and the other user data), per source.
constexpr u32 kBindHistory = 4;
struct BindStage {
    u64 snapshot = 0;
    u64 version[kBindHistory] = {};
    u64 program[kBindHistory] = {};
    u32 next = 0;
};
struct BindCue {
    std::atomic<u64> key{0};
    BindStage stage[kDrawStages];
};
constexpr u32 kBindCues = 1024;
BindCue* g_bind = nullptr;
struct BindCounters {
    std::atomic<u64> calls{0}, filled{0};
    std::atomic<u64> present[kDrawStages] = {}, versioned[kDrawStages] = {}, fresh[kDrawStages] = {};
    std::atomic<u64> hit1[kDrawStages] = {}, hit4[kDrawStages] = {};
};
BindCounters g_bind_stats[4]; ///< by Source (1 mesh, 2 shadow, 3 G-buffer)

BindCue* BindFind(u64 cue) {
    u32 h = static_cast<u32>(((cue >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & (kBindCues - 1);
    for (u32 n = 0; n < kBindCues; ++n, h = (h + 1) & (kBindCues - 1)) {
        u64 k = g_bind[h].key.load(std::memory_order_acquire);
        if (k == cue) {
            return &g_bind[h];
        }
        if (k == 0 && (g_bind[h].key.compare_exchange_strong(k, cue, std::memory_order_acq_rel) || k == cue)) {
            return &g_bind[h];
        }
    }
    return nullptr;
}

void BindNote(u32 src, u64 r) {
    const R7::ResourceHooks* hooks = R7::GetResourceHooks();
    BindCounters& s = g_bind_stats[src];
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const u64 cue = r + L::kContextCue;
    R7::NativeResourceBindings b;
    BindCue* bc = nullptr;
    if (hooks == nullptr || !hooks->fill(cue, b) || (bc = BindFind(cue)) == nullptr) {
        return;
    }
    s.filled.fetch_add(1, std::memory_order_relaxed);
    const auto at32 = [](u64 a) {
        u32 v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
        return v;
    };
    for (u32 d = 0; d < kDrawStages; ++d) {
        const R7::NativeStageResources& st = b.stage[d];
        if (!st.present) {
            continue;
        }
        // the consumer's key (INTERFACE.md v3): version within the CUE's reset epoch, the program by value (the VS with
        // its RSRC modifier, native_draw.cpp kCueVsModifier)
        const u64 program_key = (st.program | (d == 0 ? u64{at32(cue + 0xC168)} << 40 : 0)) ^
                                (st.epoch * 0x9E3779B97F4A7C15ull);
        BindStage& h = bc->stage[d];
        s.present[d].fetch_add(1, std::memory_order_relaxed);
        if (st.version != 0) {
            s.versioned[d].fetch_add(1, std::memory_order_relaxed);
        }
        const bool fresh = st.snapshot != h.snapshot;
        h.snapshot = st.snapshot;
        if (!fresh) {
            continue;
        }
        s.fresh[d].fetch_add(1, std::memory_order_relaxed);
        if (st.version == 0) {
            continue;
        }
        const u32 newest = (h.next + kBindHistory - 1) % kBindHistory;
        bool found = false;
        for (u32 i = 0; i < kBindHistory && !found; ++i) {
            found = h.version[i] == st.version && h.program[i] == program_key;
        }
        if (h.version[newest] == st.version && h.program[newest] == program_key) {
            s.hit1[d].fetch_add(1, std::memory_order_relaxed);
        } else {
            if (found) {
                s.hit4[d].fetch_add(1, std::memory_order_relaxed);
            }
            h.version[h.next] = st.version;
            h.program[h.next] = program_key;
            h.next = (h.next + 1) % kBindHistory;
        }
    }
}

void BindReporter() {
    u64 flips0 = Gpu::VideoOut::NextFlipId();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const u64 flips1 = Gpu::VideoOut::NextFlipId();
        const double frames = static_cast<double>(std::max<u64>(flips1 - flips0, 1));
        flips0 = flips1;
        std::string line;
        static constexpr const char* kName[4] = {"", "mesh 0x1a1960", "shadow 0x43d480", "G-buffer 0x1a1770"};
        for (u32 src = 1; src < 4; ++src) {
            BindCounters& s = g_bind_stats[src];
            const auto take = [frames](std::atomic<u64>& v) { return static_cast<double>(v.exchange(0)) / frames; };
            const double calls = take(s.calls), filled = take(s.filled);
            line += fmt::format(" | {}: calls {:.0f}, filled {:.0f}", kName[src], calls, filled);
            static constexpr const char* kStage[kDrawStages] = {"VS", "PS"};
            for (u32 d = 0; d < kDrawStages; ++d) {
                const double present = take(s.present[d]), versioned = take(s.versioned[d]), fresh = take(s.fresh[d]);
                const double hit1 = take(s.hit1[d]), hit4 = take(s.hit4[d]);
                line += fmt::format("; {} present {:.0f} versioned {:.0f} new snapshot {:.0f} same version+program "
                                    "{:.0f} (last) + {:.0f} (last 4)",
                                    kStage[d], present, versioned, fresh, hit1, hit4);
            }
        }
        SSPC_LOG(Engine, Info, "R8 render bindings per frame ({:.0f} flips){}", frames, line);
    }
}

template <u32 I>
SSPC_SYSV_ABI void Probe(u64 a, u64 r, u64 c, u64 d, u64 e, u64 f) {
    // the context's DCB (G = R + 8): the chunk's start and the write pointer before and after the engine's draw
    u32* const* begin = reinterpret_cast<u32* const*>(r + L::kContextGfx + L::kCbBegin);
    u32* const* cmd = reinterpret_cast<u32* const*>(r + L::kContextGfx + L::kCbCmdPtr);
    const u32* const b0 = *begin;
    const u32* const p0 = *cmd;
    constexpr u32 src = I + 1;
    u32 tag = src;
    R7::DrawScope outer{};
    bool scoped = false;
    if constexpr (I == 2) {
        if (R7::GBufferPath()) {
            // R7-Core C3: the tag before the call (0x448640 ran before it), for the native draws inside it
            if (t_material_context == r) {
                t_material_context = 0;
                tag |= kMaterialChanged;
                g_stats.material_tagged.fetch_add(1, std::memory_order_relaxed);
            }
            outer = R7::OpenScope(r, tag);
            scoped = true;
        }
    } else {
        // R8 NR5: the mesh pass's single draw / the shadow casters' draw through the layer too (SSPC_R8_RENDER_PATHS)
        constexpr R7::EngineFn fn = I == 0 ? R7::EngineFn::GBufferMesh : R7::EngineFn::ShadowInstanced;
        if (R7::GBufferPath() && R7::Carries(fn)) {
            outer = R7::OpenScope(r, tag, fn);
            scoped = true;
        }
    }
    g_orig[I](a, r, c, d, e, f);
    const u32 native = scoped ? R7::CloseScope(outer) : 0;
    if (g_bind != nullptr) {
        BindNote(src, r);
    }
    if (g_diag) {
        // 0x1a1960: the instance (c) -> its mesh **(c + 0x18); 0x43d480, 0x1a1770: the mesh (e), d instances
        DiagNote(I, r, I == 0 ? **reinterpret_cast<const u64* const*>(c + 0x18) : e, I == 0 ? 1 : static_cast<u32>(d));
    }
    g_stats.calls[src].fetch_add(1, std::memory_order_relaxed);
    u32* const p1 = *cmd;
    // the trailing NOP of the draw this call wrote: the last 4 dwords, written by this call (in this chunk after p0,
    // or in a chunk the call started), its first body dword still the driver's 0
    const bool fresh = *begin != b0 ? p1 - *begin >= 4 : p1 - p0 >= 4;
    if constexpr (I == 2) {
        if (!scoped && t_material_context == r) {
            t_material_context = 0;
            tag |= kMaterialChanged;
            g_stats.material_tagged.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (fresh && p1[-4] == kTrailingNopHeader && p1[-3] == 0) {
        p1[-3] = kRunMagic;
        p1[-2] = tag;
        g_stats.tagged[src].fetch_add(1, std::memory_order_relaxed);
    } else if (native == 0) { // (a native draw left a run token, not a NOP: its tag went with it - R7::ScopeStats)
        g_stats.no_nop.fetch_add(1, std::memory_order_relaxed);
    }
}

SSPC_SYSV_ABI void ProbeMaterial(u64 a, u64 r, u64 c, u64 d, u64 e, u64 f) {
    g_orig_material(a, r, c, d, e, f);
    t_material_context = r;
    g_stats.materials.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

const Counters& Stats() {
    return g_stats;
}

bool On() {
    return g_on;
}

bool GBufferTagged() {
    return g_gbuffer;
}

bool InstallFromEnvironment(u64 eboot_base, bool native_backend) {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_CITY_ENGINE_OPT", v, sizeof(v));
    if (n == 0 || n >= sizeof(v) || v[0] == '0') {
        return true;
    }
    if (!native_backend) {
        SSPC_LOG(Engine, Warning, "SSPC_CITY_ENGINE_OPT ignored: only SecondSonPC's GPU backend (--gpu-backend sspc) reads "
                                  "the engine's draw runs");
        return false;
    }
    char census[4] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_DRAW_CENSUS", census, sizeof(census)) == 1 && census[0] == '1') {
        SSPC_LOG(Engine, Error, "SSPC_CITY_ENGINE_OPT not installed: SSPC_DIAG_DRAW_CENSUS probes the same functions");
        return false;
    }
    // CITY-OPT Phase 6: 0x1a1770 too, for the G-buffer's runs or the stage diagnostics only (Phase 5 as it was without)
    const auto env_on = [](const char* name) {
        char e[16] = {};
        const DWORD k = GetEnvironmentVariableA(name, e, sizeof(e));
        return k > 0 && k < sizeof(e) && e[0] != '0';
    };
    const u32 sites = env_on("SSPC_CITY_ENGINE_GBUFFER_RUNS") || env_on("SSPC_DIAG_ENGINE_SRT") ? 3 : 2;
    std::string error;
    for (u32 i = 0; i < sites; ++i) {
        if (!VerifyGuestFunction(eboot_base, kSites[i].function, error)) {
            SSPC_LOG(Engine, Error, "SSPC_CITY_ENGINE_OPT not installed (another game version?): {}", error);
            return false;
        }
    }
    const std::array<const void*, 3> targets = {reinterpret_cast<const void*>(&Probe<0>),
                                                reinterpret_cast<const void*>(&Probe<1>),
                                                reinterpret_cast<const void*>(&Probe<2>)};
    for (u32 i = 0; i < sites; ++i) {
        // (the slot: a Native Engine reconstruction of the function goes in below the probe - guest_probe.h)
        void* original =
            InstallEntryProbe(eboot_base, kSites[i], targets[i], error, reinterpret_cast<void**>(&g_orig[i]));
        if (original == nullptr) {
            SSPC_LOG(Engine, Critical, "SSPC_CITY_ENGINE_OPT: {}", error);
            std::abort();
        }
        g_orig[i] = reinterpret_cast<Fn>(original);
    }
    if (sites == 3) {
        // (the G-buffer draws' material changes: a member the processor refuses before walking its PS again)
        void* original = VerifyGuestFunction(eboot_base, kMaterialSite.function, error)
                             ? InstallEntryProbe(eboot_base, kMaterialSite, reinterpret_cast<const void*>(&ProbeMaterial),
                                                 error, reinterpret_cast<void**>(&g_orig_material))
                             : nullptr;
        if (original == nullptr) {
            SSPC_LOG(Engine, Warning, "CITY-OPT Phase 6: the material probe (0x448640) not installed: {}", error);
        }
        g_orig_material = reinterpret_cast<Fn>(original);
    }
    g_on = true;
    g_gbuffer = sites == 3;
    if (R7::GBufferPath() && !g_gbuffer) {
        SSPC_LOG(Engine, Error, "SSPC_R7_NATIVE_RENDER: the G-buffer path needs 0x1a1770's probe "
                                "(SSPC_CITY_ENGINE_GBUFFER_RUNS=1) - its draws stay the game's");
        R7::SetGBufferPath(false);
    }
    if (env_on("SSPC_R8_RENDER_BINDINGS")) {
        g_bind = new BindCue[kBindCues];
        std::thread(BindReporter).detach();
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_R8_RENDER_BINDINGS=1 - per probed draw the CUE's VS / PS binding "
                                  "versions against the context's previous ones (what a processor stage memo keyed by "
                                  "them could keep; needs SSPC_R7_RES_STATE)");
    }
    if (char dv[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_ENGINE_RUNS", dv, sizeof(dv)) == 1 && dv[0] == '1') {
        g_diag = true;
        std::thread(DiagReporter).detach();
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_DIAG_ENGINE_RUNS=1 - the engine's meshes per frame and context");
    }
    SSPC_LOG(Engine, Warning, "EXPERIMENT: SSPC_CITY_ENGINE_OPT={} - the engine's mesh draws (0x1a1960) and shadow casters' "
                              "draws (0x43d480){} tagged for the command processor's runs", v,
             g_gbuffer ? " and instanced mesh draws (0x1a1770, CITY-OPT Phase 6)" : "");
    return true;
}

} // namespace SecondSon::Engine::Render::DrawRuns
