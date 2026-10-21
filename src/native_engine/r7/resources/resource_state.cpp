// SecondSonPC R7-R — Native Resource State (see resource_state.h).
#include "native_engine/r7/resources/resource_state.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "native_engine/r6/cue_dedup.h"
#include "native_engine/reconstructed/cue_commit.h"
#include "native_engine/reconstructed/cue_set_buffers.h"

namespace SecondSon::NativeEngine::R7::Resources {

namespace {

namespace CSB = Reconstructed::CueSetBuffers;
using Engine::Render::TableShape;
using Engine::Render::R7::NativeResourceBindings;
using Engine::Render::R7::NativeStageResources;

constexpr u64 kAllocRva = 0x123d0;       ///< CUE::AllocSnapshot (cue, dwords) -> pointer
constexpr u64 kMemcpyRva = 0x9843a0;     ///< memcpy (dst, src, n)
constexpr u64 kSetPointerRva = 0x626400; ///< setPointerInUserData (dcb, stage, sgpr, pointer)
constexpr u64 kSetRegionRva = 0x626520;  ///< setUserDataRegion (dcb, stage, sgpr, values, count)

constexpr u64 kCueAllocCursor = 0x20; ///< u64: the snapshot allocator's next free dword (0x123d0 moves it, 0x12390 resets it)
constexpr u64 kCueShadow = 0x90;
constexpr u64 kCueShadowStride = 0x1800;
constexpr u64 kCueLayout = 0xc180;
constexpr u64 kCueDirty = 0xc1d0;    ///< + stage: resources changed (the next commit snapshots the stage)
constexpr u64 kCueModeFlags = 0xc1e0; ///< u32: 0x20 ES/GS, 0x1 LS/HS (the commit writes ring descriptors into shadows)
constexpr u32 kStages = 8;
constexpr u32 kWindowDwords = 0x600;
constexpr u32 kBitWords = kWindowDwords / 32;
constexpr u32 kUserData = 16;
constexpr u32 kCueStagePs = 1, kCueStageVs = 2;

// the CUE resets [A: candidates.py, prologue checked: no branch, no RIP-relative operand in the moved bytes]
constexpr Engine::ProbeSite kResetShadersSite{{0x13d20, 122, 0x38b128d610934b7bull}, 14}; ///< CUE::ResetShaders
constexpr Engine::ProbeSite kAllocResetSite{{0x12390, 52, 0x120db2c4e5281b1full}, 15};    ///< allocator reset

using GuestFn = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);

struct StageState {
    u64 cv = 0;           ///< content version
    u64 bind_version = 0; ///< binding version (0: never committed)
    u64 bind_cv = 0;
    u64 bind_layout = 0;
    u64 snapshot = 0;
    u32 dwords = 0;
    bool current = false; ///< the snapshot is this CUE's since its last reset
    u32 direct_mask = 0;  ///< user-data SGPRs this CUE wrote for the stage since its last reset (not snapshot pointers)
    u32 direct[kUserData] = {};
    u32 bind_direct_mask = 0;
    u32 bind_direct[kUserData] = {};
    u32 delta[kBitWords] = {};      ///< shadow dwords changed since the last snapshot
    u32 last_delta[kBitWords] = {}; ///< ... between the last two snapshots
    bool last_delta_valid = false;
    // verify
    u8* copy = nullptr; ///< the last snapshot's bytes
    u32 copy_dwords = 0;
    u64 copy_version = 0;
    u64 copy_cv = 0;
    u64 copy_layout = 0;
    u32 copy_direct_mask = 0;
    u32 copy_direct[kUserData] = {};
};

struct CueState {
    std::atomic<u64> key{0};
    StageState stage[kStages];
    // R8 NR2 (A4's CUE audit): what Fill vouches for besides the versions
    u64 epoch = 0;          ///< resets seen (0x12390 / 0x13d20, Forget): the snapshot ring may have turned since
    u64 alloc_end = 0;      ///< the snapshot allocator's cursor (CUE + 0x20) at the last observed commit's end
    bool alloc_known = false;
};

constexpr u32 kCues = 1024; // 254 pool slots + pre / post + 5 deformation contexts, and room
CueState* g_cues = nullptr;
bool g_active = false;
bool g_probes = false;          ///< the reset probes are in (once per process)
bool g_assume_tracking = false; ///< tests
StateMode g_mode = StateMode::Off;
u64 g_base = 0;
void (*g_redirect)(u64, u64) = nullptr;
GuestFn g_alloc = nullptr, g_set_pointer = nullptr, g_set_region = nullptr;
GuestFn g_reset_shaders = nullptr, g_alloc_reset = nullptr; ///< the probes' originals
std::atomic<u32> g_threads{0};

// --- per-thread counters (no shared cache line on the setters' path) ------------------------------------------------

constexpr u32 kMaxBlocks = 512;
std::atomic<Stats*> g_blocks[kMaxBlocks] = {};
std::atomic<u32> g_block_count{0};
Stats g_shared_block; ///< threads beyond kMaxBlocks (racy increments: diagnostics only)
thread_local Stats* t_stats = nullptr;

Stats& C() {
    if (t_stats == nullptr) {
        const u32 i = g_block_count.fetch_add(1, std::memory_order_relaxed);
        if (i < kMaxBlocks) {
            t_stats = new Stats{};
            g_blocks[i].store(t_stats, std::memory_order_release);
        } else {
            t_stats = &g_shared_block;
        }
    }
    return *t_stats;
}

thread_local u64 t_version_high = 0, t_version_low = 0;

/// A new version id, unique across threads.
u64 NextVersion() {
    if (t_version_high == 0) {
        t_version_high = u64{g_threads.fetch_add(1, std::memory_order_relaxed) + 1} << 40;
    }
    return t_version_high | ++t_version_low;
}

template <typename T>
inline T At(u64 address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T));
    return v;
}

CueState* Find(u64 cue, bool create) {
    if (g_cues == nullptr || cue == 0) {
        return nullptr;
    }
    u32 h = static_cast<u32>(((cue >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & (kCues - 1);
    for (u32 n = 0; n < kCues; ++n, h = (h + 1) & (kCues - 1)) {
        CueState& c = g_cues[h];
        u64 k = c.key.load(std::memory_order_acquire);
        if (k == cue) {
            return &c;
        }
        if (k == 0) {
            if (!create) {
                return nullptr;
            }
            if (c.key.compare_exchange_strong(k, cue, std::memory_order_acq_rel)) {
                for (StageState& s : c.stage) {
                    s.cv = NextVersion(); // content before tracking: one unknown version
                }
                return &c;
            }
            if (k == cue) {
                return &c;
            }
        }
    }
    return nullptr;
}

inline void MarkDelta(StageState& s, u32 first, u32 n) {
    for (u32 d = first; d < first + n && d < kWindowDwords; ++d) {
        s.delta[d >> 5] |= 1u << (d & 31);
    }
}

inline u32 CountBits(const u32 (&bits)[kBitWords], u32 dwords) {
    u32 n = 0;
    for (u32 w = 0; w < kBitWords && w * 32 < dwords; ++w) {
        u32 v = bits[w];
        if ((w + 1) * 32 > dwords) {
            v &= (1u << (dwords - w * 32)) - 1;
        }
        n += static_cast<u32>(std::popcount(v));
    }
    return n;
}

/// The table shape the layout gives (native_draw.cpp LayoutShape: the commit's pointer bytes and offsets).
bool Shape(u64 layout, bool vertex, TableShape& s) {
    if (layout == 0) {
        return false;
    }
    s = TableShape{};
    s.dwords = static_cast<u16>(std::min<u32>(At<u16>(layout), kWindowDwords));
    struct Ref {
        u32 sgpr_byte;
        u32 offset_field;
    };
    static constexpr Ref kRefs[] = {{7, 0}, {8, 0x16}, {9, 0x1A}, {0xA, 0x1C}, {0xB, 0x1E}, {5, 0x18}, {6, 0x20}};
    for (u32 i = 0; i < (vertex ? 7u : 5u); ++i) {
        const u8 sgpr = At<u8>(layout + kRefs[i].sgpr_byte);
        if (sgpr != 0xFF && sgpr <= 14 && s.refs < 7) {
            s.sgpr[s.refs] = sgpr;
            s.offset[s.refs++] = kRefs[i].offset_field == 0 ? u16{0} : At<u16>(layout + kRefs[i].offset_field);
        }
    }
    return true;
}

bool TrackingValid() {
    if (g_assume_tracking) {
        return true;
    }
    using EngineMode = NativeEngine::Mode; // (Mode here is this namespace's function)
    for (u32 k = 0; k < CSB::kAllKinds; ++k) {
        const Slot* s = CSB::Spec(static_cast<CSB::Kind>(k)).slot;
        if (s->original == nullptr ||
            static_cast<EngineMode>(s->mode.load(std::memory_order_relaxed)) == EngineMode::Original) {
            return false;
        }
    }
    const Slot* c = Reconstructed::kCueCommitSpec.slot;
    const EngineMode m = static_cast<EngineMode>(c->mode.load(std::memory_order_relaxed));
    return c->original != nullptr && (m == EngineMode::Native || m == EngineMode::Guarded);
}

// --- the commit's observed calls ------------------------------------------------------------------------------------

struct Capture {
    u64 cue = 0;
    u64 pending = 0, pending_bytes = 0;
    u64 snap[kStages] = {};
    u32 snap_dwords[kStages] = {};
    u8 refs[kStages] = {};
    u8 ref_sgpr[kStages][8] = {};
    u16 ref_offset[kStages][8] = {};
    u32 ref_overflow = 0;
    u32 written_mask[kStages] = {}; ///< user-data SGPRs written by the commit (direct values)
    u32 written[kStages][kUserData] = {};
    u32 pointer_mask[kStages] = {}; ///< ... holding a pointer into the stage's snapshot
};
thread_local Capture t_cap;

SSPC_SYSV_ABI u64 ObserveAlloc(u64 cue, u64 dwords, u64 a2, u64 a3, u64 a4, u64 a5) {
    const u64 p = g_alloc(cue, dwords, a2, a3, a4, a5);
    t_cap.pending = p;
    t_cap.pending_bytes = u64{static_cast<u32>(dwords)} * 4;
    return p;
}

SSPC_SYSV_ABI u64 ObserveCopy(u64 dst, u64 src, u64 n, u64, u64, u64) {
    const u64 shadow = t_cap.cue + kCueShadow;
    if (dst != 0 && dst == t_cap.pending && n == t_cap.pending_bytes && src >= shadow &&
        (src - shadow) % kCueShadowStride == 0 && (src - shadow) / kCueShadowStride < kStages) {
        const u32 st = static_cast<u32>((src - shadow) / kCueShadowStride);
        t_cap.snap[st] = dst;
        t_cap.snap_dwords[st] = static_cast<u32>(n / 4);
    } else {
        C().other_copies++;
    }
    t_cap.pending = 0;
    std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
    return dst;
}

/// setPointerInUserData's pointer into the capture (the game's commit through its observer, the layer's directly).
void CapturePointer(u32 st, u32 sg, u64 pointer) {
    if (st < kStages && sg + 1 < kUserData) {
        const u64 snap = t_cap.snap[st];
        if (snap != 0 && pointer >= snap && pointer < snap + u64{t_cap.snap_dwords[st]} * 4) {
            if (t_cap.refs[st] < 8) {
                t_cap.ref_sgpr[st][t_cap.refs[st]] = static_cast<u8>(sg);
                t_cap.ref_offset[st][t_cap.refs[st]++] = static_cast<u16>((pointer - snap) / 4);
            } else {
                t_cap.ref_overflow++;
            }
            t_cap.pointer_mask[st] |= 3u << sg;
            t_cap.written_mask[st] &= ~(3u << sg);
        } else { // another pointer (the global table CUE+0x78, the VS's CUE+0xC130): a user-data value
            t_cap.written_mask[st] |= 3u << sg;
            t_cap.pointer_mask[st] &= ~(3u << sg);
            t_cap.written[st][sg] = static_cast<u32>(pointer);
            t_cap.written[st][sg + 1] = static_cast<u32>(pointer >> 32);
        }
    }
}

/// One user-data value of setUserDataRegion into the capture.
void CaptureValue(u32 st, u32 sg, u32 value) {
    if (st < kStages && sg < kUserData) {
        t_cap.written_mask[st] |= 1u << sg;
        t_cap.pointer_mask[st] &= ~(1u << sg);
        t_cap.written[st][sg] = value;
    }
}

SSPC_SYSV_ABI u64 ObserveSetPointer(u64 dcb, u64 stage, u64 sgpr, u64 pointer, u64 a4, u64 a5) {
    CapturePointer(static_cast<u32>(stage), static_cast<u32>(sgpr), pointer);
    return g_set_pointer(dcb, stage, sgpr, pointer, a4, a5);
}

SSPC_SYSV_ABI u64 ObserveSetRegion(u64 dcb, u64 stage, u64 sgpr, u64 values, u64 count, u64 a5) {
    const u32 st = static_cast<u32>(stage), sg = static_cast<u32>(sgpr), n = static_cast<u32>(count);
    if (st < kStages) {
        for (u32 j = 0; j < n && sg + j < kUserData; ++j) {
            CaptureValue(st, sg + j, At<u32>(values + 4ull * j));
        }
    }
    return g_set_region(dcb, stage, sgpr, values, count, a5);
}

void ClearDirect(StageState& s) {
    s.direct_mask = 0;
    std::memset(s.direct, 0, sizeof(s.direct));
}

SSPC_SYSV_ABI u64 ResetShadersProbe(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    OnReset(a0);
    return g_reset_shaders(a0, a1, a2, a3, a4, a5);
}

SSPC_SYSV_ABI u64 AllocResetProbe(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    OnReset(a0); // also called by 0x13270 for another object: OnReset ignores pointers that are no tracked CUE
    return g_alloc_reset(a0, a1, a2, a3, a4, a5);
}

/// R8-A3 NR2: R7::ResourceHooks for the Native Render Layer's own commits (defined with the commit, below).
void RegisterHooks(bool on);

void Reporter() {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        SSPC_LOG(Engine, Info, "r7 resources/10s: {}", Report(Totals(), s));
    }
}

StateMode ReadMode() {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R7_RES_STATE", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return StateMode::Off;
    }
    const std::string s(v, n);
    return s == "verify" ? StateMode::Verify : (s == "1" || s == "on") ? StateMode::On : StateMode::Off;
}

} // namespace

StateMode Mode() {
    static const StateMode m = ReadMode();
    return g_mode != StateMode::Off ? g_mode : m;
}

bool Active() {
    return g_active;
}

void SetMode(StateMode m) {
    g_mode = m;
}

void AssumeTracking(bool on) {
    g_assume_tracking = on;
}

void Forget(u64 cue) {
    if (CueState* cs = Find(cue, false)) {
        for (StageState& s : cs->stage) {
            u8* copy = s.copy;
            s = StageState{};
            s.copy = copy;
            s.cv = NextVersion();
        }
        cs->epoch++;
        cs->alloc_known = false;
    }
}

void Deactivate() {
    RegisterHooks(false);
    CSB::SetObserver(nullptr);
    g_active = false;
    g_mode = StateMode::Off;
    g_assume_tracking = false;
}

bool Prepare(u64 module_base, void (*redirect)(u64 from, u64 to), std::string& error) {
    if (g_active) {
        return true;
    }
    if (redirect == nullptr) {
        error = "SSPC_R7_RES_STATE needs recomp_set_redirect";
        return false;
    }
    if (R6::CueDedup::Enabled()) {
        error = "SSPC_R7_RES_STATE and SSPC_R6_CUE_DEDUP both redirect the commit's snapshot copy; R6's is on";
        return false;
    }
    g_base = module_base;
    g_redirect = redirect;
    g_alloc = reinterpret_cast<GuestFn>(module_base + kAllocRva);
    g_set_pointer = reinterpret_cast<GuestFn>(module_base + kSetPointerRva);
    g_set_region = reinterpret_cast<GuestFn>(module_base + kSetRegionRva);
    if (g_probes) {
        CSB::SetObserver(&BeforeSetter);
        g_active = true;
        RegisterHooks(true);
        return true;
    }
    void* a = Engine::InstallEntryProbe(module_base, kResetShadersSite,
                                        reinterpret_cast<const void*>(&ResetShadersProbe), error,
                                        reinterpret_cast<void**>(&g_reset_shaders));
    if (a == nullptr) {
        error = "reset probe 0x13d20: " + error;
        return false;
    }
    g_reset_shaders = reinterpret_cast<GuestFn>(a);
    void* b = Engine::InstallEntryProbe(module_base, kAllocResetSite, reinterpret_cast<const void*>(&AllocResetProbe),
                                        error, reinterpret_cast<void**>(&g_alloc_reset));
    if (b == nullptr) {
        error = "reset probe 0x12390: " + error;
        return false; // (0x13d20's probe stays: it only observes)
    }
    g_alloc_reset = reinterpret_cast<GuestFn>(b);
    g_probes = true;
    g_cues = new CueState[kCues];
    if (g_mode == StateMode::Off) {
        g_mode = Mode() == StateMode::Off ? StateMode::On : Mode();
    }
    CSB::SetObserver(&BeforeSetter);
    g_active = true;
    RegisterHooks(true); // (R8 NR2: the layer's native commits and draws)
    if (!g_assume_tracking) { // (the game; tests read Totals themselves)
        std::thread(Reporter).detach();
    }
    return true;
}

void BeforeSetter(u32 kind, u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    Stats& c = C();
    c.setter_calls++;
    c.kind_calls[kind]++;
    CueState* cs = Find(cue, true);
    const s32 st = static_cast<s32>(stage);
    if (cs == nullptr || st < 0 || st >= static_cast<s32>(kStages)) {
        c.setter_untracked++;
        return;
    }
    StageState& s = cs->stage[st];
    const u32 bytes = CSB::kDescriptorBytes[kind];
    const u32 dwords = bytes / 4;
    bool changed = false, outside = false;
    if (count != 0) {
        const u64 table = At<u64>(cue + kCueLayout + static_cast<u64>(st) * 8);
        const u64 shadow = cue + kCueShadow + static_cast<u64>(st) * kCueShadowStride;
        for (u32 i = 0; i < count; ++i, src += bytes) {
            const u32 entry = At<u16>(table + CSB::kTableOffset[kind] + static_cast<u64>(slot + i) * 2);
            if (entry == 0xffff) {
                continue;
            }
            const u32 index = entry & 0x3fff;
            c.setter_slots++;
            if ((entry & 0x8000) != 0) { // straight into user-data SGPRs (setUserDataRegion)
                c.setter_direct++;
                for (u32 j = 0; j < dwords; ++j) {
                    const u32 sg = index + j, v = At<u32>(src + 4ull * j);
                    if (sg >= kUserData) {
                        changed = true;
                        continue;
                    }
                    if ((s.direct_mask >> sg & 1) == 0 || s.direct[sg] != v) {
                        changed = true;
                        s.direct_mask |= 1u << sg;
                        s.direct[sg] = v;
                    }
                }
                continue;
            }
            if (index + dwords > kWindowDwords) {
                outside = true; // another stage's shadow or the CUE's own fields
                continue;
            }
            if (std::memcmp(reinterpret_cast<const void*>(shadow + index * 4ull), reinterpret_cast<const void*>(src),
                            bytes) == 0) {
                c.setter_slots_same++;
            } else {
                changed = true;
                MarkDelta(s, index, dwords);
            }
        }
    }
    if (outside) {
        c.setter_outside++;
        for (StageState& o : cs->stage) {
            o.cv = NextVersion();
            MarkDelta(o, 0, kWindowDwords);
        }
        c.setter_changing++;
    } else if (changed) {
        s.cv = NextVersion();
        c.setter_changing++;
    } else {
        c.setter_redundant++;
        c.kind_redundant[kind]++;
    }
}

void OnReset(u64 cue) {
    CueState* cs = g_active ? Find(cue, false) : nullptr;
    if (cs == nullptr) {
        return;
    }
    C().resets++;
    for (StageState& s : cs->stage) {
        s.current = false;
        ClearDirect(s);
    }
    cs->epoch++;
    cs->alloc_known = false;
}

namespace {

/// A commit's start (Commit, ExternalBegin): the counters, verify's shadow check, the capture reset.
CueState* CommitBegin(u64 cue) {
    Stats& c = C();
    c.commits++;
    CueState* cs = Find(cue, true);
    const bool verify = g_mode == StateMode::Verify;
    if (cs != nullptr && verify) {
        // the shadow against the last snapshot while the content version held (an unobserved shadow writer shows here)
        for (u32 st = 0; st < kStages; ++st) {
            StageState& s = cs->stage[st];
            if (s.copy != nullptr && s.copy_cv == s.cv && s.copy_dwords != 0) {
                c.verify_shadow_checked++;
                if (std::memcmp(s.copy, reinterpret_cast<const void*>(cue + kCueShadow + st * kCueShadowStride),
                                u64{s.copy_dwords} * 4) != 0) {
                    c.verify_shadow_violations++;
                }
            }
        }
    }
    t_cap = Capture{};
    t_cap.cue = cue;
    return cs;
}

/// A commit's end: the captured snapshots and user data into the stages' bindings (`rings`: the CUE's ES/GS or LS/HS
/// mode as the commit found it).
void CommitEnd(CueState* cs, u64 cue, bool rings) {
    if (cs == nullptr) {
        return;
    }
    Stats& c = C();
    const bool verify = g_mode == StateMode::Verify;
    for (u32 st = 0; st < kStages; ++st) {
        StageState& s = cs->stage[st];
        if (rings && (st == 3 || st == 4)) {
            s.cv = NextVersion(); // the ES/GS mode writes ring descriptors into these shadows
            MarkDelta(s, 0, kWindowDwords);
        }
        // the commit's single values and non-snapshot pointers join the stage's user data
        const u32 wm = t_cap.written_mask[st];
        s.direct_mask = (s.direct_mask & ~t_cap.pointer_mask[st]) | wm;
        for (u32 sg = 0; sg < kUserData; ++sg) {
            if ((wm >> sg & 1) != 0) {
                s.direct[sg] = t_cap.written[st][sg];
            }
        }
        const u64 snap = t_cap.snap[st];
        if (snap == 0) {
            continue;
        }
        const u32 n = t_cap.snap_dwords[st];
        const u64 layout = At<u64>(cue + kCueLayout + u64{st} * 8);
        c.snapshots++;
        // the pointers into the snapshot against the layout's shape
        TableShape shape;
        if ((st == kCueStageVs || st == kCueStagePs) && Shape(layout, st == kCueStageVs, shape)) {
            c.shape_checked++;
            bool same = shape.refs == t_cap.refs[st];
            for (u32 i = 0; same && i < shape.refs; ++i) {
                bool found = false;
                for (u32 j = 0; j < t_cap.refs[st] && !found; ++j) {
                    found = t_cap.ref_sgpr[st][j] == shape.sgpr[i] && t_cap.ref_offset[st][j] == shape.offset[i];
                }
                same = found;
            }
            if (!same) {
                c.shape_mismatch++;
            }
        }
        const bool kept = s.bind_version != 0 && s.bind_cv == s.cv && s.bind_layout == layout && s.dwords == n &&
                          s.bind_direct_mask == s.direct_mask &&
                          std::memcmp(s.bind_direct, s.direct, sizeof(s.direct)) == 0;
        c.stage_snapshots[st]++;
        if (kept) {
            c.binding_kept++;
            c.stage_kept[st]++;
        } else {
            c.binding_new++;
            s.bind_version = NextVersion();
            s.bind_cv = s.cv;
            s.bind_layout = layout;
            s.bind_direct_mask = s.direct_mask;
            std::memcpy(s.bind_direct, s.direct, sizeof(s.direct));
        }
        // what changed in the table since the stage's previous snapshot
        if (s.snapshot != 0 && s.dwords == n) {
            c.delta_known++;
            c.changed_dwords += CountBits(s.delta, n);
            c.table_dwords += n;
            std::memcpy(s.last_delta, s.delta, sizeof(s.delta));
            s.last_delta_valid = true;
        } else {
            s.last_delta_valid = false;
        }
        std::memset(s.delta, 0, sizeof(s.delta));
        s.snapshot = snap;
        s.dwords = n;
        s.current = true;
        if (verify) {
            if (s.copy == nullptr) {
                s.copy = new u8[kCueShadowStride];
            }
            const bool same_bytes = s.copy_dwords == n &&
                                    std::memcmp(s.copy, reinterpret_cast<const void*>(snap), u64{n} * 4) == 0;
            const bool same_user = s.copy_direct_mask == s.direct_mask &&
                                   std::memcmp(s.copy_direct, s.direct, sizeof(s.direct)) == 0;
            if (s.copy_version == s.bind_version) {
                c.verify_same_version++;
                if (!same_bytes || !same_user || s.copy_layout != layout) {
                    c.verify_violations++;
                }
            } else if (same_bytes && same_user && s.copy_layout == layout) {
                c.verify_new_same_bytes++;
            }
            std::memcpy(s.copy, reinterpret_cast<const void*>(snap), u64{n} * 4);
            s.copy_dwords = n;
            s.copy_version = s.bind_version;
            s.copy_cv = s.cv;
            s.copy_layout = layout;
            s.copy_direct_mask = s.direct_mask;
            std::memcpy(s.copy_direct, s.direct, sizeof(s.direct));
        }
    }
    if (t_cap.ref_overflow != 0) {
        c.shape_mismatch += t_cap.ref_overflow;
    }
    cs->alloc_end = At<u64>(cue + kCueAllocCursor);
    cs->alloc_known = true;
    if (verify) { // the draw that follows: what Fill hands it equals the CUE's shadow now
        NativeResourceBindings b;
        Fill(cue, b);
        for (const NativeStageResources& r : b.stage) {
            if (r.present) {
                c.verify_fill_checked++;
                if (std::memcmp(reinterpret_cast<const void*>(r.snapshot), reinterpret_cast<const void*>(r.shadow),
                                u64{r.dwords} * 4) != 0) {
                    c.verify_fill_mismatch++;
                }
            }
        }
    }
}

// R8-A3 NR2: the Native Render Layer's own commit (native_draw.cpp NativeCommit), told through R7::ResourceHooks
thread_local bool t_external = false;
thread_local CueState* t_external_cue = nullptr;
thread_local bool t_external_rings = false;

constexpr Engine::Render::R7::ResourceHooks kHooks{&ExternalBegin, &ExternalSnapshot, &ExternalPointer,
                                                   &ExternalValue, &ExternalEnd,      &Fill};

void RegisterHooks(bool on) {
    Engine::Render::R7::SetResourceHooks(on ? &kHooks : nullptr);
}

} // namespace

void Commit(SSPC_SYSV_ABI void (*impl)(u64), u64 cue) {
    if (!g_active) {
        impl(cue);
        return;
    }
    const bool rings = (At<u32>(cue + kCueModeFlags) & 0x21) != 0;
    CueState* cs = CommitBegin(cue);
    g_redirect(g_base + kAllocRva, reinterpret_cast<u64>(&ObserveAlloc));
    g_redirect(g_base + kMemcpyRva, reinterpret_cast<u64>(&ObserveCopy));
    g_redirect(g_base + kSetPointerRva, reinterpret_cast<u64>(&ObserveSetPointer));
    g_redirect(g_base + kSetRegionRva, reinterpret_cast<u64>(&ObserveSetRegion));
    impl(cue);
    g_redirect(0, 0);
    CommitEnd(cs, cue, rings);
}

void ExternalBegin(u64 cue) {
    t_external = g_active;
    if (!t_external) {
        return;
    }
    C().external_commits++;
    t_external_rings = (At<u32>(cue + kCueModeFlags) & 0x21) != 0;
    t_external_cue = CommitBegin(cue);
}

void ExternalSnapshot(u32 cue_stage, u64 snapshot, u32 dwords) {
    if (t_external && cue_stage < kStages && snapshot != 0) {
        t_cap.snap[cue_stage] = snapshot;
        t_cap.snap_dwords[cue_stage] = dwords;
    }
}

void ExternalPointer(u32 cue_stage, u32 sgpr, u64 value) {
    if (t_external) {
        CapturePointer(cue_stage, sgpr, value);
    }
}

void ExternalValue(u32 cue_stage, u32 sgpr, u32 value) {
    if (t_external) {
        CaptureValue(cue_stage, sgpr, value);
    }
}

void ExternalEnd(u64 cue) {
    if (!t_external) {
        return;
    }
    t_external = false;
    CommitEnd(t_external_cue, cue, t_external_rings);
    t_external_cue = nullptr;
}

bool Fill(u64 cue, NativeResourceBindings& out) {
    out = NativeResourceBindings{};
    out.cue = cue;
    Stats& c = C();
    c.fills++;
    CueState* cs = g_active ? Find(cue, false) : nullptr;
    if (cs == nullptr) {
        return false;
    }
    if (!cs->alloc_known || At<u64>(cue + kCueAllocCursor) != cs->alloc_end) {
        // the snapshot allocator moved since the last observed commit's end: a commit not observed (0x140d0 changes
        // user data without changing a shadow) may have pointed the GPU elsewhere - nothing is vouched for
        c.fills_moved++;
        return true;
    }
    const bool valid = TrackingValid();
    static constexpr u32 kCueStage[Engine::Render::kDrawStages] = {kCueStageVs, kCueStagePs};
    // the stage's shader program by value: the code address of the register block the commit sets (sceGnmSetVsShader /
    // SetPsShader350 at shader object + 8, PGM_HI written 0); 0 when the commit sets the stage another way
    static constexpr u64 kCueShader[Engine::Render::kDrawStages] = {0xC0B0, 0xC0A8};
    static constexpr u64 kCueAltPath[Engine::Render::kDrawStages] = {0xC1DA, 0xC1D9};
    for (u32 d = 0; d < Engine::Render::kDrawStages; ++d) {
        const u32 st = kCueStage[d];
        const StageState& s = cs->stage[st];
        NativeStageResources& r = out.stage[d];
        const u64 layout = At<u64>(cue + kCueLayout + u64{st} * 8);
        if (!s.current || s.snapshot == 0 || layout == 0 || layout != s.bind_layout || At<u8>(cue + kCueDirty + st) != 0) {
            continue;
        }
        if (s.cv != s.bind_cv) {
            // the content changed since the binding and no dirty byte is left: another commit took it (0x140d0 of the
            // dispatch-draw path, or a commit run as the game's code) - the binding is not the GPU's
            c.fills_stale++;
            continue;
        }
        r.present = true;
        r.shadow = cue + kCueShadow + u64{st} * kCueShadowStride;
        r.dwords = s.dwords;
        r.snapshot = s.snapshot;
        Shape(layout, st == kCueStageVs, r.shape);
        r.direct_mask = s.direct_mask;
        std::memcpy(r.direct, s.direct, sizeof(r.direct));
        r.version = valid ? s.bind_version : 0;
        r.epoch = cs->epoch;
        const u64 shader = At<u64>(cue + kCueShader[d]);
        r.program = shader != 0 && At<u8>(cue + kCueAltPath[d]) == 0 ? u64{At<u32>(shader + 8)} << 8 : 0;
        c.fills_present++;
        if (r.version != 0) {
            c.fills_versioned++;
        }
    }
    return true;
}

bool ChangedDwords(u64 cue, u32 cue_stage, u32 (&bits)[kWindowDwords / 32]) {
    CueState* cs = g_active && cue_stage < kStages ? Find(cue, false) : nullptr;
    if (cs == nullptr || !cs->stage[cue_stage].last_delta_valid) {
        return false;
    }
    std::memcpy(bits, cs->stage[cue_stage].last_delta, sizeof(bits));
    return true;
}

Stats Totals() {
    Stats t;
    const auto add = [&t](const Stats& s) {
        const u64* a = reinterpret_cast<const u64*>(&s);
        u64* b = reinterpret_cast<u64*>(&t);
        for (size_t i = 0; i < sizeof(Stats) / sizeof(u64); ++i) {
            b[i] += a[i];
        }
    };
    const u32 n = std::min(g_block_count.load(std::memory_order_relaxed), kMaxBlocks);
    for (u32 i = 0; i < n; ++i) {
        if (const Stats* s = g_blocks[i].load(std::memory_order_acquire)) {
            add(*s);
        }
    }
    add(g_shared_block);
    return t;
}

std::string Report(const Stats& s, double seconds) {
    const auto pct = [](u64 a, u64 b) { return b != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    std::string kinds;
    static constexpr const char* kName[7] = {"vb", "buf", "cb", "tex", "smp", "rwbuf", "rwtex"};
    for (u32 k = 0; k < 7; ++k) {
        kinds += fmt::format(" {} {}/{}", kName[k], s.kind_redundant[k], s.kind_calls[k]);
    }
    std::string stages;
    for (u32 st = 0; st < kStages; ++st) {
        if (s.stage_snapshots[st] != 0) {
            stages += fmt::format(" {} {}/{}", st, s.stage_kept[st], s.stage_snapshots[st]);
        }
    }
    return fmt::format(
        "{:.0f} s, mode {}, tracking {}; setters {} (changing {}, redundant {} = {:.1f} %; slots {}, same {}, direct {}, "
        "outside {}, untracked {}; redundant/calls by kind:{}); commits {}, snapshots {} (binding kept {} = {:.1f} %, "
        "new {}; kept/snapshots by stage:{}), changed dwords {} = {:.2f} % of {} ({} snapshots with a delta), shape checked {} mismatched {}, other "
        "copies {}, resets {}; fills {} present {} versioned {} stale {}; verify: same-version {} violations {}, new "
        "version with equal bytes {}, shadow checked {} violations {}, fill checked {} mismatches {}; external commits {}, "
        "fills refused (allocator moved) {}",
        seconds, Mode() == StateMode::Verify ? "verify" : "on", TrackingValid() ? "valid" : "NOT VALID", s.setter_calls,
        s.setter_changing, s.setter_redundant, pct(s.setter_redundant, s.setter_calls), s.setter_slots,
        s.setter_slots_same, s.setter_direct, s.setter_outside, s.setter_untracked, kinds, s.commits, s.snapshots,
        s.binding_kept, pct(s.binding_kept, s.snapshots), s.binding_new, stages, s.changed_dwords,
        pct(s.changed_dwords, s.table_dwords),
        s.table_dwords, s.delta_known, s.shape_checked, s.shape_mismatch, s.other_copies, s.resets, s.fills,
        s.fills_present, s.fills_versioned, s.fills_stale, s.verify_same_version, s.verify_violations,
        s.verify_new_same_bytes, s.verify_shadow_checked, s.verify_shadow_violations, s.verify_fill_checked,
        s.verify_fill_mismatch, s.external_commits, s.fills_moved);
}

} // namespace SecondSon::NativeEngine::R7::Resources
