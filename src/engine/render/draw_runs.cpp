// SecondSonPC — the engine's runs of mesh draws, told to the command processor (CITY-OPT Phase 5). See draw_runs.h.
#include "engine/render/draw_runs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <intrin.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/native_render_layer.h"
#include "engine/render/render_layout.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::Engine::Render::DrawRuns {

namespace {

namespace L = Layout;
namespace GDR = NativeEngine::Reconstructed::GBufferDraw;

// SSPC_NATIVE_GBUFFER: the side ring of engine descriptions (draw_runs.h). One producer writes a slot at a time (the
// sequence number it gets from g_desc_head); a reader (the command processor) takes it by the sequence in the draw's
// trailing NOP, and a seqlock check tells it when the slot has since been reused. Sized well above the draws in
// flight between a draw's capture and its recording, so reuse does not race the reader in practice.
struct DescSlot {
    std::atomic<u64> seq{~0ull}; ///< the sequence number whose description this holds (~0: none / being written)
    EngineDraw draw;
};
constexpr u32 kDescRingBits = 13; // (8192 slots of ~5 KiB: several frames of G-buffer draws in flight)
constexpr u32 kDescRingSize = 1u << kDescRingBits;
constexpr u32 kDescRingMask = kDescRingSize - 1;
std::unique_ptr<DescSlot[]> g_desc_ring;
std::atomic<u64> g_desc_head{0};
bool g_desc_on = false; ///< capture installed (SSPC_NATIVE_GBUFFER and the pure callees ready)

// R10 (SSPC_R10_NATIVE_DRAWS): the shadow casters' and mesh draws' CUE state, the same seqlock scheme. These draws come
// from several game threads (the shadow cascades' jobs), ~9 000 a frame in the city: 32768 slots of ~1 KiB hold more
// than three frames of them.
struct StagesSlot {
    std::atomic<u64> seq{~0ull};
    EngineStages draw;
};
constexpr u32 kStagesRingBits = 15;
constexpr u32 kStagesRingMask = (1u << kStagesRingBits) - 1;
std::unique_ptr<StagesSlot[]> g_stages_ring;
std::atomic<u64> g_stages_head{0};
std::array<bool, 2> g_stages_on{}; ///< by source - 1 (Mesh, Shadow)
struct alignas(64) StagesCount {
    std::atomic<u64> made{0}, refused{0}, ticks{0};
};
std::array<StagesCount, 2> g_stages_count{};

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
    }
    g_orig[I](a, r, c, d, e, f);
    const u32 native = scoped ? R7::CloseScope(outer) : 0;
    if (g_diag) {
        // 0x1a1960: the instance (c) -> its mesh **(c + 0x18); 0x43d480, 0x1a1770: the mesh (e), d instances
        DiagNote(I, r, I == 0 ? **reinterpret_cast<const u64* const*>(c + 0x18) : e, I == 0 ? 1 : static_cast<u32>(d));
    }
    g_stats.calls[src].fetch_add(1, std::memory_order_relaxed);
    // SSPC_NATIVE_GBUFFER: capture the engine descriptors of this G-buffer draw into the side ring (its source
    // structures, no SRT walk), the sequence into the trailing NOP for the command processor. (a = M/pass, r = R,
    // c = records, d = n, e = mesh.)
    u32 desc_seq = 0;
    bool have_desc = false;
    if constexpr (I == 2) {
        if (g_desc_on && !scoped) { // (not the R7 native-render path, which replaces the draw and its tag)
            const u64 h = g_desc_head.fetch_add(1, std::memory_order_relaxed);
            DescSlot& s = g_desc_ring[h & kDescRingMask];
            s.seq.store(~0ull, std::memory_order_release); // being written: readers see it as stale
            std::atomic_thread_fence(std::memory_order_release);
            if (GDR::Describe(a, r, c, d, e, s.draw.desc) && s.draw.desc.followed) {
                // the stages' CUE state at the draw's commit (what their SRT walks would read: gbuffer_draw.h)
                GDR::CaptureStage(r, 2, s.draw.vs);
                GDR::CaptureStage(r, 1, s.draw.ps);
                s.seq.store(h, std::memory_order_release); // published
                desc_seq = static_cast<u32>(h);
                have_desc = true;
                tag |= kNativeDesc;
            }
        }
    } else {
        // R10 (SSPC_R10_NATIVE_DRAWS): 0x1a1960 / 0x43d480 - their commit 0x14ad0 ran right before their drawIndex, so the
        // CUE shadows now are what the stages' registers point into (as for 0x1a1770, gbuffer_draw.h CaptureStage)
        if (g_stages_on[I]) {
            const u64 t0 = __rdtsc();
            const u64 h = g_stages_head.fetch_add(1, std::memory_order_relaxed);
            StagesSlot& s = g_stages_ring[h & kStagesRingMask];
            s.seq.store(~0ull, std::memory_order_release); // being written: readers see it as stale
            std::atomic_thread_fence(std::memory_order_release);
            const bool vs = GDR::CaptureStage(r, 2, s.draw.vs);
            const bool ps = GDR::CaptureStage(r, 1, s.draw.ps);
            if (vs && ps) {
                s.seq.store(h, std::memory_order_release); // published
                desc_seq = static_cast<u32>(h);
                have_desc = true;
                tag |= kNativeDesc;
                g_stages_count[I].made.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_stages_count[I].refused.fetch_add(1, std::memory_order_relaxed);
            }
            g_stages_count[I].ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        }
    }
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
        if (have_desc) {
            p1[-1] = desc_seq; // nop[3]: the ring sequence (NativeDescSeq)
        }
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

bool EngineDescOn() {
    return g_desc_on;
}

const EngineDraw* EngineDescPeek(u32 seq) {
    if (!g_desc_on) {
        return nullptr;
    }
    const DescSlot& s = g_desc_ring[seq & kDescRingMask];
    const u64 seq1 = s.seq.load(std::memory_order_acquire);
    if (static_cast<u32>(seq1) != seq || seq1 == ~0ull) {
        return nullptr; // the slot holds another draw now (or is being written)
    }
    return &s.draw;
}

bool EngineDescStill(u32 seq) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 seq2 = g_desc_ring[seq & kDescRingMask].seq.load(std::memory_order_relaxed);
    return static_cast<u32>(seq2) == seq && seq2 != ~0ull; // not overwritten while it was read
}

const EngineStages* StagesPeek(u32 seq) {
    if (g_stages_ring == nullptr) {
        return nullptr;
    }
    const StagesSlot& s = g_stages_ring[seq & kStagesRingMask];
    const u64 seq1 = s.seq.load(std::memory_order_acquire);
    if (static_cast<u32>(seq1) != seq || seq1 == ~0ull) {
        return nullptr; // the slot holds another draw now (or is being written)
    }
    return &s.draw;
}

bool StagesStill(u32 seq) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 seq2 = g_stages_ring[seq & kStagesRingMask].seq.load(std::memory_order_relaxed);
    return static_cast<u32>(seq2) == seq && seq2 != ~0ull; // not overwritten while it was read
}

bool StagesOn(Source source) {
    return (source == Source::Mesh && g_stages_on[0]) || (source == Source::Shadow && g_stages_on[1]);
}

StagesCounters StagesStats() {
    StagesCounters c;
    for (u32 i = 0; i < 2; ++i) {
        c.made[i] = g_stages_count[i].made.load(std::memory_order_relaxed);
        c.refused[i] = g_stages_count[i].refused.load(std::memory_order_relaxed);
        c.ticks[i] = g_stages_count[i].ticks.load(std::memory_order_relaxed);
    }
    return c;
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
    if (char dv[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_ENGINE_RUNS", dv, sizeof(dv)) == 1 && dv[0] == '1') {
        g_diag = true;
        std::thread(DiagReporter).detach();
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_DIAG_ENGINE_RUNS=1 - the engine's meshes per frame and context");
    }
    // SSPC_NATIVE_GBUFFER: capture the engine descriptors of 0x1a1770's draws into the side ring (needs the G-buffer
    // tag, i.e. SSPC_CITY_ENGINE_GBUFFER_RUNS). The command processor reads the ring for the draws it prepares.
    if (char ng[16] = {}; GetEnvironmentVariableA("SSPC_NATIVE_GBUFFER", ng, sizeof(ng)) > 0 && ng[0] != '0' &&
                          std::string_view(ng) != "off") {
        if (!g_gbuffer) {
            SSPC_LOG(Engine, Warning, "SSPC_NATIVE_GBUFFER: 0x1a1770's draws are not tagged "
                                      "(SSPC_CITY_ENGINE_GBUFFER_RUNS=1) - the engine descriptors are not captured");
        } else {
            GDR::EnsurePureCallees(eboot_base);
            g_desc_ring = std::make_unique<DescSlot[]>(kDescRingSize);
            g_desc_on = true;
            SSPC_LOG(Engine, Warning, "EXPERIMENT: SSPC_NATIVE_GBUFFER={} - 0x1a1770's engine descriptors (instance V#, "
                                      "vertex streams, draw constants) and its VS/PS CUE state at the commit captured "
                                      "for the command processor's native path", ng);
        }
    }
    // R10 (SSPC_R10_NATIVE_DRAWS): the shadow casters' and mesh draws' CUE state for the command processor (draw_runs.h)
    if (char nd[16] = {}; GetEnvironmentVariableA("SSPC_R10_NATIVE_DRAWS", nd, sizeof(nd)) > 0 && nd[0] != '0' &&
                          std::string_view(nd) != "off") {
        char srcs[32] = {};
        const DWORD k = GetEnvironmentVariableA("SSPC_R10_NATIVE_DRAWS_SOURCES", srcs, sizeof(srcs));
        const std::string_view sv = k > 0 && k < sizeof(srcs) ? std::string_view(srcs, k) : std::string_view("shadow,mesh");
        g_stages_on[0] = sv.find("mesh") != std::string_view::npos;
        g_stages_on[1] = sv.find("shadow") != std::string_view::npos;
        if (g_stages_on[0] || g_stages_on[1]) {
            g_stages_ring = std::make_unique<StagesSlot[]>(size_t{1} << kStagesRingBits);
        }
        SSPC_LOG(Engine, Warning, "EXPERIMENT: SSPC_R10_NATIVE_DRAWS={} - the VS/PS CUE state at the commit of the {}{}{} "
                                  "captured for the command processor's engine-state preparation (no SRT walk)", nd,
                 g_stages_on[1] ? "shadow casters' draws 0x43d480" : "", g_stages_on[0] && g_stages_on[1] ? " and the " : "",
                 g_stages_on[0] ? "mesh draws 0x1a1960" : "");
    }
    SSPC_LOG(Engine, Warning, "EXPERIMENT: SSPC_CITY_ENGINE_OPT={} - the engine's mesh draws (0x1a1960) and shadow casters' "
                              "draws (0x43d480){} tagged for the command processor's runs", v,
             g_gbuffer ? " and instanced mesh draws (0x1a1770, CITY-OPT Phase 6)" : "");
    return true;
}

} // namespace SecondSon::Engine::Render::DrawRuns
