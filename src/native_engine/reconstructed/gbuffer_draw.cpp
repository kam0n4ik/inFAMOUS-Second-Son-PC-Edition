// SecondSonPC — reconstructions of the mesh pass's draw functions 0x1a1770 / 0x1a1960 (see gbuffer_draw.h).
#include "native_engine/reconstructed/gbuffer_draw.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "native_engine/guest_abi_bridge.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace GD = GBufferDraw;
namespace GT = GTrace;

namespace {

constexpr Engine::GuestFunction kInstancedFunction{0x1a1770, 0x1e4, 0x5bf111e570dcaaedull};
constexpr Engine::GuestFunction kMeshFunction{0x1a1960, 0x225, 0xea1442f922a0a0ecull};
/// Run for real in a recording run (they only compute into the caller's stack): pinned.
constexpr Engine::GuestFunction kAssumedInstanced[] = {
    {0x45f590, 0x60, 0x2290fd2d5824be48ull}, // V# of a vertex-format table entry (-> 0x61e030)
    {0x61e030, 0x8f, 0x828d655230c37d5bull}, // Gnm V# (buffer) builder
    {0x61e260, 0xce, 0x82958d25b30182e7ull}, // Gnm initAsRegularBuffer (-> 0x61f430)
    {0x61f430, 0x33, 0x75d0a6829a95f352ull}, // Gnm DataFormat
    {0x27530, 0xe, 0xf08ab1c7f617f7e7ull},   // the pass's stage number -> the CUE's
};

constexpr u64 kVertexFormatTable = 0x7dff738; ///< 0x45f590's first argument (eboot data)
constexpr u64 kDebugFlag = 0xf1bcd0 + 0xdf0f05; ///< u8: 0x628ae0 is called when set
constexpr u64 kStackGuardSlot = 0xd9de00;   ///< GOT: __stack_chk_guard
constexpr u64 kDefaultsSlot = 0xd9dec8;     ///< GOT: a global whose + 0x1f6c is the default V# block
constexpr u64 kDefaultsOffset = 0x1f6c;

constexpr u64 kRva[GD::kCallees] = {0x6229a0, 0x120280, 0x16b70,  0x16aa0,  0x6250b0, 0x623740, 0x624f80,
                                    0x628ae0, 0x14ad0,  0x623c60, 0x984360, 0x984430, 0x45f590};
constexpr u64 kInitBufferRva = 0x61e260, kStageRva = 0x27530;

/// The callees of each function (a clone's redirects must all be used).
constexpr GD::Callee kInstancedCallees[] = {GD::kStencil,   GD::kDrawConstants, GD::kSetBuffers, GD::kSetVertex,
                                            GD::kNumInstances, GD::kPrimitive, GD::kIndexType, GD::kDebugMarker,
                                            GD::kCommit,    GD::kDrawIndex,     GD::kStackFail,  GD::kVsharp};
constexpr GD::Callee kMeshCallees[] = {GD::kMemmove,   GD::kSetVertex, GD::kSetBuffers, GD::kPrimitive, GD::kIndexType,
                                       GD::kDebugMarker, GD::kCommit,  GD::kDrawIndex,  GD::kStackFail};

struct V128 {
    u64 lo, hi;
};
using StencilFn = SSPC_SYSV_ABI u64 (*)(u64 gfx, u64 value);
using ContextFn = SSPC_SYSV_ABI u64 (*)(u64 r);
using SetterFn = SSPC_SYSV_ABI u64 (*)(u64 cue, u64 stage, u64 slot, u64 count, u64 src);
using Gfx1Fn = SSPC_SYSV_ABI u64 (*)(u64 gfx, u64 a);
using Gfx2Fn = SSPC_SYSV_ABI u64 (*)(u64 gfx, u64 a, u64 b);
using Gfx3Fn = SSPC_SYSV_ABI u64 (*)(u64 gfx, u64 a, u64 b, u64 c);
using CommitFn = SSPC_SYSV_ABI u64 (*)(u64 cue);
using VsharpFn = SSPC_SYSV_ABI V128 (*)(u64 table, u64 which);
using InitBufferFn = SSPC_SYSV_ABI u64 (*)(u64 vsharp, u64 base, u64 stride, u64 count);
using StageFn = SSPC_SYSV_ABI u64 (*)(u64 stage);

/// Where the hand-written code calls: the game's functions, or the trace thunks in a recording run.
struct Fns {
    StencilFn stencil;
    ContextFn draw_constants;
    SetterFn set_buffers;
    SetterFn set_vertex;
    Gfx1Fn num_instances;
    Gfx1Fn primitive;
    Gfx2Fn index_type;
    Gfx1Fn debug_marker;
    CommitFn commit;
    Gfx3Fn draw_index;
    // run for real in recordings too
    VsharpFn vsharp;
    InitBufferFn init_buffer;
    StageFn stage;
};

std::atomic<u64> g_module{0};
Fns g_real{};      ///< the game's callees (Prepare)
Fns g_recorders{}; ///< the trace thunks + the real pure callees (Prepare)
thread_local const Fns* t_fns = nullptr;
u32 g_fault = 0; ///< tests: GD::InjectFault

GD::InstancedFn g_rec_instanced = nullptr;
GD::MeshFn g_rec_mesh = nullptr;
GD::InstancedFn g_impl_instanced = &GD::InstancedNative;
GD::MeshFn g_impl_mesh = &GD::MeshNative;
GD::RedirectsFn g_redirects = nullptr;

inline const Fns& Table() {
    return t_fns != nullptr ? *t_fns : g_real;
}

// --- measurement (SSPC_R6G_STATS) -------------------------------------------------------------------------------

bool g_stats = false;

enum Stat : u32 {
    // per function: [0] instanced, [1] mesh
    kCalls,
    kInstances,
    kTicks,
    kTicksCommit,
    kTicksDraw,
    kDcbDwords,
    kDcbChunkChanged,
    kStencilWrites,
    kDrawConstantsCommits,
    kVsharpPath,
    kSkinPath,
    kDefaultsPath,
    kStreamPath,
    kDebugMarkers,
    kUnfollowed,
    // the commit
    kVsTableDirty,
    kPsTableDirty,
    kVsShaderDirty,
    kPsShaderDirty,
    kOtherStages,
    kVsDwords,
    kPsDwords,
    kVsSame,
    kPsSame,
    kVsSlotsChanged1,
    kVsSlotsChanged2,
    kVsSlotsChanged3,
    kVsSlotsChangedMore,
    kPsSlotsChanged1,
    kPsSlotsChangedMore,
    kPsSameLayoutDiffer,
    kStats
};
constexpr const char* kStatNames[kStats] = {
    "calls", "instances", "ticks", "ticks_commit", "ticks_draw", "dcb_dwords", "dcb_chunk_changed", "stencil_writes",
    "drawconst_commits", "vsharp_path", "skin_path", "defaults_path", "stream_path", "debug_markers", "unfollowed",
    "vs_table_dirty", "ps_table_dirty", "vs_shader_dirty", "ps_shader_dirty", "other_stages", "vs_dwords", "ps_dwords",
    "vs_same", "ps_same", "vs_slots_changed_1", "vs_slots_changed_2", "vs_slots_changed_3", "vs_slots_changed_more",
    "ps_slots_changed_1", "ps_slots_changed_more", "ps_differ_same_layout"};

struct StatBlock {
    std::atomic<u64> v[2][kStats]{};
    // the previous committed tables of this thread (one CUE per thread in practice)
    u64 cue = 0;
    u64 layout[2] = {}; ///< [0] VS, [1] PS
    u32 dwords[2] = {};
    alignas(64) u8 table[2][0x1800];
};
std::mutex g_stat_mu;
std::vector<StatBlock*> g_stat_blocks;
thread_local StatBlock* t_stat = nullptr;

StatBlock& Stats() {
    if (t_stat == nullptr) {
        t_stat = new StatBlock;
        const std::scoped_lock lock(g_stat_mu);
        g_stat_blocks.push_back(t_stat);
    }
    return *t_stat;
}

inline void Add(StatBlock& s, u32 f, Stat k, u64 n = 1) {
    std::atomic<u64>& c = s.v[f][k];
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline u64 Tsc() {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

/// What the commit about to run will do with the VS (stage 2) and PS (stage 1) tables [A: 0x14ad0, P-code]: dirty
/// flags cue + 0xc1d0 + stage, shader flags cue + 0xc1c9 (PS) / + 0xc1ca (VS), layout cue + 0xc180 + 8 stage (u16
/// dwords at + 0), shadow cue + 0x90 + 0x1800 stage. Compared with this thread's previous committed table.
void MeasureCommit(StatBlock& s, u32 f, u64 cue) {
    const u8 vs_dirty = GuestLoad<u8>(cue + 0xc1d2), ps_dirty = GuestLoad<u8>(cue + 0xc1d1);
    Add(s, f, kVsShaderDirty, GuestLoad<u8>(cue + 0xc1ca) != 0);
    Add(s, f, kPsShaderDirty, GuestLoad<u8>(cue + 0xc1c9) != 0);
    Add(s, f, kOtherStages, (GuestLoad<u32>(cue + 0xc1e0) & 0x21) != 0);
    if (s.cue != cue) {
        s.cue = cue;
        s.layout[0] = s.layout[1] = 0;
    }
    for (u32 k = 0; k < 2; ++k) {
        const bool dirty = (k == 0 ? vs_dirty : ps_dirty) != 0;
        if (!dirty) {
            continue;
        }
        Add(s, f, k == 0 ? kVsTableDirty : kPsTableDirty);
        const u64 stage = k == 0 ? 2 : 1;
        const u64 layout = GuestLoad<u64>(cue + 0xc180 + 8 * stage);
        const u32 dwords = layout != 0 ? GuestLoad<u16>(layout) : 0;
        Add(s, f, k == 0 ? kVsDwords : kPsDwords, dwords);
        const u8* table = reinterpret_cast<const u8*>(cue + 0x90 + 0x1800 * stage);
        const u32 bytes = std::min<u32>(dwords * 4, sizeof(s.table[k]));
        if (layout == s.layout[k] && dwords == s.dwords[k]) {
            if (std::memcmp(table, s.table[k], bytes) == 0) {
                Add(s, f, k == 0 ? kVsSame : kPsSame);
            } else {
                u32 changed = 0;
                for (u32 o = 0; o < bytes; o += 16) {
                    changed += std::memcmp(table + o, s.table[k] + o, std::min<u32>(16, bytes - o)) != 0;
                }
                if (k == 0) {
                    Add(s, f, changed == 1 ? kVsSlotsChanged1
                              : changed == 2 ? kVsSlotsChanged2
                              : changed == 3 ? kVsSlotsChanged3
                                             : kVsSlotsChangedMore);
                } else {
                    Add(s, f, changed == 1 ? kPsSlotsChanged1 : kPsSlotsChangedMore);
                    Add(s, f, kPsSameLayoutDiffer);
                }
            }
        }
        s.layout[k] = layout;
        s.dwords[k] = dwords;
        std::memcpy(s.table[k], table, bytes);
    }
}

/// The DCB write pointer and chunk (R + 8: the Gnm context; + 0 chunk start, + 0x10 write pointer).
struct DcbMark {
    u64 begin, cmd;
};
inline DcbMark Dcb(u64 r) {
    return {GuestLoad<u64>(r + GD::kGfx), GuestLoad<u64>(r + GD::kGfx + 0x10)};
}

} // namespace

namespace {

// --- the trace's callees ------------------------------------------------------------------------------------------

u64 ReturnCue(GT::Trace&, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    return in.gpr[0]; // the CUE setters return their CUE
}
u64 StackFail(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn&, const u64*) {
    t.stack_check_failed = true;
    return 0;
}
u64 Vsharp(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    const u64 lo = GT::Next(t);
    in.gpr[2] = GT::Next(t); // RDX
    return lo;
}
u64 Memmove(GT::Trace&, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    std::memmove(reinterpret_cast<void*>(in.gpr[0]), reinterpret_cast<const void*>(in.gpr[1]), in.gpr[2]);
    return in.gpr[0];
}

constexpr GT::Snap kNoSnap{};
constexpr GT::Snap kDescriptors{4, 3, 16, 16 * 64}; ///< r8 -> rcx V#s of 16 bytes

} // namespace

const GT::CalleeSpec GD::kCalleeSpecs[GD::kCallees] = {
    {0x6229a0, "0x6229a0 stencil", 2, 0x2, 0, 0, {kNoSnap, kNoSnap}},
    {0x120280, "0x120280 draw constants", 1, 0, 0, 0, {kNoSnap, kNoSnap}},
    {0x16b70, "0x16b70 CUE set buffers", 5, 0xe, 0, 0, {kDescriptors, kNoSnap}, &ReturnCue},
    {0x16aa0, "0x16aa0 CUE set vertex buffers", 5, 0xe, 0, 0, {kDescriptors, kNoSnap}, &ReturnCue},
    {0x6250b0, "0x6250b0 NUM_INSTANCES", 2, 0x2, 0, 0, {kNoSnap, kNoSnap}},
    {0x623740, "0x623740 primitive type", 2, 0x2, 0, 0, {kNoSnap, kNoSnap}},
    {0x624f80, "0x624f80 index type", 3, 0x6, 0, 0, {kNoSnap, kNoSnap}},
    {0x628ae0, "0x628ae0 debug marker", 2, 0x2, 0, 0, {kNoSnap, kNoSnap}},
    {0x14ad0, "0x14ad0 CUE commit", 1, 0, 0, 0, {kNoSnap, kNoSnap}},
    {0x623c60, "0x623c60 drawIndex", 4, 0xa, 0, 0, {kNoSnap, kNoSnap}},
    {0x984360, "0x984360 __stack_chk_fail", 0, 0, 0, 0, {kNoSnap, kNoSnap}, &StackFail},
    {0x984430, "0x984430 memmove", 3, 0, 0, 0, {kNoSnap, kNoSnap}, &Memmove, false},
    {0x45f590, "0x45f590 vertex-format V#", 2, 0x2, 0, 0, {kNoSnap, kNoSnap}, &Vsharp},
};

namespace {

// --- the hand-written implementations ---------------------------------------------------------------------------

/// 0x1a1770. False (nothing done) for a call it does not follow: an empty vertex-format table entry (0x45f590 then
/// returns the stack guard and its caller's RDX as the V#).
template <bool kStats>
bool Instanced(const Fns& f, u64 m, u64 r, u64 records, u64 n, u64 mesh) {
    StatBlock* s = kStats ? &Stats() : nullptr;
    const u64 module = g_module.load(std::memory_order_relaxed);
    if (GuestLoad<u8>(m + 0x135) != 0xff &&
        GuestLoad<u64>(module + kVertexFormatTable + 0x40 + 8 * (~(GuestLoad<u32>(mesh + 0x18) >> 27) & 1u)) == 0) {
        if constexpr (kStats) {
            Add(*s, 0, kUnfollowed);
        }
        return false;
    }
    const u64 t0 = kStats ? Tsc() : 0;
    const DcbMark d0 = kStats ? Dcb(r) : DcbMark{};
    const u16 st = GuestLoad<u16>(m + 0x1c);
    const u8 lo = static_cast<u8>(st);
    if (lo != 0) {
        const u8 hi = static_cast<u8>((st >> 8) | GuestLoad<u8>(mesh + 0x1d));
        const u32 v = (u32{lo} << 16) + (u32{hi} << 24) + 0xff00u;
        if (v != GuestLoad<u32>(r + GD::kStencilState)) {
            u8* p = reinterpret_cast<u8*>(r + GD::kStencilState);
            p[0] = 0;
            p[1] = 0xff;
            p[2] = lo;
            p[3] = hi;
            f.stencil(r + GD::kGfx, v);
            if constexpr (kStats) {
                Add(*s, 0, kStencilWrites);
            }
        }
    }
    if constexpr (kStats) {
        Add(*s, 0, kDrawConstantsCommits,
            GuestLoad<s32>(r + 0xde18) != 0 && (GuestLoad<u64>(r + 0xde20) == 0 || GuestLoad<u8>(r + 0xde1c) != 0));
    }
    f.draw_constants(r);
    alignas(16) V128 vs{};
    if (GuestLoad<u8>(m + 0x135) != 0xff) {
        const u32 flags = GuestLoad<u32>(mesh + 0x18);
        vs = f.vsharp(module + kVertexFormatTable, ~(flags >> 27) & 1u);
        const u8 slot = GuestLoad<u8>(m + 0x13c);
        if (slot != 0xff) {
            f.set_buffers(r + GD::kCue, 2, slot, 1, reinterpret_cast<u64>(&vs));
        }
        if constexpr (kStats) {
            Add(*s, 0, kVsharpPath);
        }
    }
    vs = {};
    f.init_buffer(reinterpret_cast<u64>(&vs), records, 0xd4, static_cast<u32>(n));
    const u64 stage = static_cast<u32>(f.stage(0));
    f.set_buffers(r + GD::kCue, stage, GuestLoad<u8>(m + 0x127), 1, reinterpret_cast<u64>(&vs));
    f.set_vertex(r + GD::kCue, 2, 0, GuestLoad<u8>(mesh + 0x6c), GuestLoad<u64>(mesh + 0x60));
    f.num_instances(r + GD::kGfx, static_cast<u32>(n));
    f.primitive(r + GD::kGfx, 4);
    f.index_type(r + GD::kGfx, 0, g_fault == 1 ? 1 : 2);
    if (GuestLoad<u8>(module + kDebugFlag) != 0) {
        f.debug_marker(r + GD::kGfx, static_cast<u32>(GuestLoad<u32>(mesh + 0x58) << 16));
        if constexpr (kStats) {
            Add(*s, 0, kDebugMarkers);
        }
    }
    const u32 count = GuestLoad<u32>(mesh + 0x68);
    const u64 indices = GuestLoad<u64>(mesh + 0x58);
    if constexpr (kStats) {
        MeasureCommit(*s, 0, r + GD::kCue);
        const u64 t1 = Tsc();
        f.commit(r + GD::kCue);
        const u64 t2 = Tsc();
        f.draw_index(r + GD::kGfx, count, indices, 0);
        const u64 t3 = Tsc();
        const DcbMark d1 = Dcb(r);
        Add(*s, 0, kCalls);
        Add(*s, 0, kInstances, static_cast<u32>(n));
        Add(*s, 0, kTicksCommit, t2 - t1);
        Add(*s, 0, kTicksDraw, t3 - t2);
        Add(*s, 0, kTicks, t3 - t0);
        if (d1.begin == d0.begin) {
            Add(*s, 0, kDcbDwords, (d1.cmd - d0.cmd) / 4);
        } else {
            Add(*s, 0, kDcbChunkChanged);
        }
    } else {
        f.commit(r + GD::kCue);
        f.draw_index(r + GD::kGfx, count, indices, 0);
    }
    return true;
}

/// 0x1a1960. False (nothing done) for a call it does not follow: more than kMaxStreams streams to copy, a skin index
/// outside the copy (the original would write outside its stack buffer).
template <bool kStats>
bool Mesh(const Fns& f, u64 m, u64 r, u64 inst) {
    StatBlock* s = kStats ? &Stats() : nullptr;
    const u64 t0 = kStats ? Tsc() : 0;
    const DcbMark d0 = kStats ? Dcb(r) : DcbMark{};
    const u64 module = g_module.load(std::memory_order_relaxed);
    const u64 mesh = GuestLoad<u64>(GuestLoad<u64>(inst + 0x18));
    const u64 skin = GuestLoad<u64>(inst + 0x20);
    const u32 n = GuestLoad<u8>(mesh + 0x6c);
    u64 table = GuestLoad<u64>(mesh + 0x60);
    alignas(16) u8 copy[GD::kMaxStreams * 16];
    if (skin != 0 && (GuestLoad<u8>(skin) & 7) != 0) {
        const u64 idx = GuestLoad<u64>(GuestLoad<u64>(skin + 8) + 0x58);
        const s8 k0 = GuestLoad<s8>(idx), k1 = GuestLoad<s8>(idx + 1), k2 = GuestLoad<s8>(idx + 2);
        if (n > GD::kMaxStreams || k0 < 0 || k0 >= static_cast<s8>(GD::kMaxStreams) ||
            k1 >= static_cast<s8>(GD::kMaxStreams) || k2 >= static_cast<s8>(GD::kMaxStreams)) {
            if constexpr (kStats) {
                Add(*s, 1, kUnfollowed);
            }
            return false;
        }
        std::memcpy(copy, reinterpret_cast<const void*>(table), u64{n} * 16);
        std::memcpy(copy + 16 * k0, reinterpret_cast<const void*>(skin + 0x28), 16);
        if (k1 >= 0) {
            std::memcpy(copy + 16 * k1, reinterpret_cast<const void*>(skin + 0x38), 16);
        }
        if (k2 >= 0) {
            std::memcpy(copy + 16 * k2, reinterpret_cast<const void*>(skin + 0x48), 16);
        }
        table = reinterpret_cast<u64>(copy);
        if constexpr (kStats) {
            Add(*s, 1, kSkinPath);
        }
    }
    f.set_vertex(r + GD::kCue, 2, 0, n, table);
    const u64 defaults = GuestLoad<u64>(module + kDefaultsSlot) + kDefaultsOffset;
    const u8 slot = GuestLoad<u8>(m + 0xd8);
    if (slot != 0xff) {
        f.set_buffers(r + GD::kCue, 2, slot, 1, skin != 0 ? skin + 0x58 : defaults);
        if constexpr (kStats) {
            Add(*s, 1, kDefaultsPath, skin == 0);
        }
    }
    if ((GuestLoad<u8>(GuestLoad<u64>(m + 0xf0) + 0xf) & 2) != 0) {
        const s8 stream = GuestLoad<s8>(GuestLoad<u64>(GuestLoad<u64>(m + 0x10) + 0x58) + 0x22);
        const u64 src = skin != 0 && GuestLoad<u64>(skin + 0x20) != 0 ? skin + 0xc0 : defaults;
        f.set_vertex(r + GD::kCue, 2, static_cast<u32>(static_cast<s32>(stream)), 1, src);
        if constexpr (kStats) {
            Add(*s, 1, kStreamPath);
        }
    }
    if (g_fault != 2) {
        f.primitive(r + GD::kGfx, 4);
    }
    f.index_type(r + GD::kGfx, 0, 2);
    if (GuestLoad<u8>(module + kDebugFlag) != 0) {
        f.debug_marker(r + GD::kGfx, static_cast<u32>(GuestLoad<u32>(mesh + 0x58) << 16));
        if constexpr (kStats) {
            Add(*s, 1, kDebugMarkers);
        }
    }
    const u32 count = GuestLoad<u32>(mesh + 0x68);
    const u64 indices = GuestLoad<u64>(mesh + 0x58);
    if constexpr (kStats) {
        MeasureCommit(*s, 1, r + GD::kCue);
        const u64 t1 = Tsc();
        f.commit(r + GD::kCue);
        const u64 t2 = Tsc();
        f.draw_index(r + GD::kGfx, count, indices, 0);
        const u64 t3 = Tsc();
        const DcbMark d1 = Dcb(r);
        Add(*s, 1, kCalls);
        Add(*s, 1, kInstances);
        Add(*s, 1, kTicksCommit, t2 - t1);
        Add(*s, 1, kTicksDraw, t3 - t2);
        Add(*s, 1, kTicks, t3 - t0);
        if (d1.begin == d0.begin) {
            Add(*s, 1, kDcbDwords, (d1.cmd - d0.cmd) / 4);
        } else {
            Add(*s, 1, kDcbChunkChanged);
        }
    } else {
        f.commit(r + GD::kCue);
        f.draw_index(r + GD::kGfx, count, indices, 0);
    }
    return true;
}

// --- verification -------------------------------------------------------------------------------------------------

constexpr u32 kMaxCalls = 64;
constexpr u32 kSnapBytes = 0x4000;

struct Run {
    GT::Trace t;
    GT::Call calls[kMaxCalls];
    alignas(64) u8 snap[kSnapBytes];
    alignas(64) u8 r[GD::kShadowBytes];
};
struct Shadow {
    Run run[2];
};
thread_local Shadow* t_shadow = nullptr;

Shadow& Shadows() {
    if (t_shadow == nullptr) { // per verifying thread, never freed (off the guest stack)
        t_shadow = new Shadow;
        for (Run& x : t_shadow->run) {
            std::memset(x.r, 0, sizeof(x.r));
        }
    }
    return *t_shadow;
}

/// Prepares run k on the shadow context (the stencil state copied from the live context `r`).
GT::Trace& BeginRun(Shadow& s, u32 k, u64 r, u64 stack_top) {
    Run& x = s.run[k];
    x.t.callees = GD::kCalleeSpecs;
    x.t.ncallees = GD::kCallees;
    x.t.calls = x.calls;
    x.t.max_calls = kMaxCalls;
    x.t.snap = x.snap;
    x.t.snap_cap = kSnapBytes;
    x.t.nregions = 0;
    x.t.AddRegion(reinterpret_cast<u64>(x.r), GD::kShadowBytes);
    std::memcpy(x.r + GD::kStencilState, reinterpret_cast<const void*>(r + GD::kStencilState), 4);
    x.t.Begin(0x5347425546464552ull, stack_top);
    return x.t;
}

/// Runs `body` with the calling thread's trace active and the implementation's calls aimed at the thunks.
template <typename F>
void Recorded(GT::Trace& t, bool original, bool hand_written, const Engine::GuestFunction& fn, F&& body) {
    GT::Trace* const saved = GT::Active();
    GT::SetActive(&t);
    if (original) {
        body();
    } else if (hand_written) {
        const Fns* saved_fns = t_fns;
        t_fns = &g_recorders;
        body();
        t_fns = saved_fns;
    } else if (g_redirects != nullptr) {
        const u64 module = g_module.load(std::memory_order_relaxed);
        u64 from[GD::kCallees], to[GD::kCallees];
        for (u32 i = 0; i < GD::kCallees; ++i) {
            from[i] = module + kRva[i];
            to[i] = GT::ThunkAddress(i);
        }
        g_redirects(from, to, GD::kCallees);
        body();
        g_redirects(nullptr, nullptr, 0);
    } else {
        t.overflow = true; // a foreign implementation without redirection: not comparable
    }
    (void)fn;
    GT::SetActive(saved);
}

/// The stack the runs' frames use, zeroed (their uninitialised locals equal in both runs; under a page: no probe).
__declspec(noinline) void WipeStackBelow() {
    volatile u8 area[0xf00];
    for (u32 i = 0; i < sizeof(area); i += 8) {
        *reinterpret_cast<volatile u64*>(&area[i]) = 0;
    }
}

std::string CompareRuns(Shadow& s, bool exact, bool& skipped) {
    skipped = s.run[0].t.overflow || s.run[1].t.overflow;
    if (skipped) {
        return {};
    }
    std::string d = GT::Compare(s.run[0].t, s.run[1].t, exact);
    if (d.empty() && std::memcmp(s.run[0].r + GD::kStencilState, s.run[1].r + GD::kStencilState, 4) != 0) {
        d = "the stencil state R+0xde64 differs";
    }
    return d;
}

} // namespace

namespace {
GD::InstancedFn g_instanced_original = nullptr; ///< the clones (slot.original): calls the hand-written code
GD::MeshFn g_mesh_original = nullptr;           ///< does not follow
} // namespace

SSPC_SYSV_ABI void GD::InstancedNative(u64 material, u64 r, u64 records, u64 n, u64 mesh) {
    const bool done = g_stats ? Instanced<true>(Table(), material, r, records, n, mesh)
                              : Instanced<false>(Table(), material, r, records, n, mesh);
    if (!done && t_fns == nullptr && g_instanced_original != nullptr) {
        g_instanced_original(material, r, records, n, mesh);
    }
}

SSPC_SYSV_ABI void GD::MeshNative(u64 material, u64 r, u64 instance) {
    const bool done = g_stats ? Mesh<true>(Table(), material, r, instance) : Mesh<false>(Table(), material, r, instance);
    if (!done && t_fns == nullptr && g_mesh_original != nullptr) {
        g_mesh_original(material, r, instance);
    }
}

// --- the Native Engine G-buffer path: Describe (SSPC_NATIVE_GBUFFER) --------------------------------------------
//
// Recording setters that read the engine descriptors into a DrawDescription instead of binding them. The pure callees
// (init_buffer 0x61e260, vsharp 0x45f590, stage 0x27530) are the game's; everything that would touch the context or
// the command buffer is a no-op. Only VS-side resources are described here; the material's PS resources are the
// stable per-material part (material_bind.h).
namespace {

// the CUE's user-data layout table ([cue + 0xc180 + stage * 8]); the section each resource kind is listed in
constexpr u64 kCueLayout = 0xc180;
constexpr u64 kSecResource = 0x22;  ///< buffers and textures
constexpr u64 kSecConstBuf = 0xe2;  ///< constant buffers
constexpr u64 kSecVertex = 0x10a;   ///< vertex buffers

thread_local GD::DrawDescription* t_desc = nullptr;

/// The stage's user-data dword offset the layout maps `slot` in `section` to; 0xffff when the shader does not use it.
u16 DescribeOffset(u64 cue, u64 stage, u64 slot, u64 section, u8& direct) {
    direct = 0;
    const u64 layout = GuestLoad<u64>(cue + kCueLayout + 8 * stage);
    if (layout == 0) {
        return 0xffff;
    }
    const u16 e = GuestLoad<u16>(layout + section + slot * 2);
    if (e == 0xffff) {
        return 0xffff;
    }
    direct = (e & 0x8000) != 0 ? 1 : 0;
    return static_cast<u16>(e & 0x3fff);
}

void DescribeAdd(u64 cue, u32 stage, u32 slot, u32 kind, u64 section, u64 src16) {
    GD::DrawDescription* d = t_desc;
    if (d == nullptr || d->count >= GD::kMaxDescResources) {
        return;
    }
    GD::DescResource& e = d->res[d->count++];
    e.stage = static_cast<u8>(stage);
    e.kind = static_cast<u8>(kind);
    e.slot = static_cast<u8>(slot);
    e.offset = DescribeOffset(cue, stage, slot, section, e.direct);
    for (u32 i = 0; i < 4; ++i) {
        e.desc[i] = GuestLoad<u32>(src16 + i * 4);
    }
}

SSPC_SYSV_ABI u64 RecNoop1(u64, u64) { return 0; }
SSPC_SYSV_ABI u64 RecNoop2(u64, u64, u64) { return 0; }
SSPC_SYSV_ABI u64 RecNoop3(u64, u64, u64, u64) { return 0; }
SSPC_SYSV_ABI u64 RecCommit(u64) { return 0; }

/// 0x120280's effect, read from the context: the per-draw constant buffer's V# at R+0xde20+8 (both stages, slot 12).
SSPC_SYSV_ABI u64 RecDrawConstants(u64 r) {
    const u64 at = GuestLoad<u64>(r + 0xde20);
    if (at == 0) {
        return 0; // no per-draw constant buffer this draw (the slot keeps its previous binding)
    }
    const u64 cue = r + GD::kCue;
    DescribeAdd(cue, 2, 12, 2, kSecConstBuf, r + 0xde20 + 8); // VS
    DescribeAdd(cue, 1, 12, 2, kSecConstBuf, r + 0xde20 + 8); // PS
    return 0;
}

SSPC_SYSV_ABI u64 RecSetBuffers(u64 cue, u64 stage, u64 slot, u64 count, u64 src) {
    for (u64 i = 0; i < count; ++i) {
        DescribeAdd(cue, static_cast<u32>(stage), static_cast<u32>(slot + i), 0, kSecResource, src + i * 16);
    }
    return cue;
}

SSPC_SYSV_ABI u64 RecSetVertex(u64 cue, u64 stage, u64 slot, u64 count, u64 src) {
    for (u64 i = 0; i < count; ++i) {
        DescribeAdd(cue, static_cast<u32>(stage), static_cast<u32>(slot + i), 1, kSecVertex, src + i * 16);
    }
    return cue;
}

Fns DescribeFns() {
    Fns f{};
    f.stencil = &RecNoop1;
    f.draw_constants = &RecDrawConstants;
    f.set_buffers = &RecSetBuffers;
    f.set_vertex = &RecSetVertex;
    f.num_instances = &RecNoop1;
    f.primitive = &RecNoop1;
    f.index_type = &RecNoop2;
    f.debug_marker = &RecNoop1;
    f.commit = &RecCommit;
    f.draw_index = &RecNoop3;
    f.vsharp = g_real.vsharp;         // pure: the real callees
    f.init_buffer = g_real.init_buffer;
    f.stage = g_real.stage;
    return f;
}

} // namespace

void GD::EnsurePureCallees(u64 module_base) {
    if (module_base == 0) {
        return;
    }
    u64 expected = 0;
    g_module.compare_exchange_strong(expected, module_base, std::memory_order_relaxed);
    if (g_real.vsharp == nullptr) {
        g_real.vsharp = reinterpret_cast<VsharpFn>(module_base + 0x45f590);
    }
    if (g_real.init_buffer == nullptr) {
        g_real.init_buffer = reinterpret_cast<InitBufferFn>(module_base + kInitBufferRva);
    }
    if (g_real.stage == nullptr) {
        g_real.stage = reinterpret_cast<StageFn>(module_base + kStageRva);
    }
}

namespace {
std::array<std::atomic<u64>, 4> g_capture_refused{}; // (CaptureRefusals)
std::array<std::atomic<u64>, 4> g_compact_refused{}; // (CompactCaptureRefusals: R10's shadow / mesh draws)

std::array<u64, 4> Loaded(const std::array<std::atomic<u64>, 4>& c) {
    return {c[0].load(std::memory_order_relaxed), c[1].load(std::memory_order_relaxed),
            c[2].load(std::memory_order_relaxed), c[3].load(std::memory_order_relaxed)};
}

template <u32 N>
bool CaptureStageOf(u64 r, u32 stage, GD::StageShadowOf<N>& out, std::array<std::atomic<u64>, 4>& refused) {
    using namespace GD;
    const u64 cue = r + kCue;
    out.layout = GuestLoad<u64>(cue + kCueLayout + 8 * u64{stage});
    out.dwords = 0;
    out.captured = false;
    if (out.layout == 0) {
        refused[0].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u16 dwords = GuestLoad<u16>(out.layout);
    if (dwords > refused[3].load(std::memory_order_relaxed)) {
        refused[3].store(dwords, std::memory_order_relaxed); // (a maximum for the report: a lost race only lowers it)
    }
    if (dwords > N) {
        refused[2].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (dwords == 0) {
        refused[1].fetch_add(1, std::memory_order_relaxed); // (the commit copies nothing; a walk that reads the copy
                                                             // fails its stage map: past the copy)
    }
    // 0x14ad0: the registers it points into the stage's copy and the dword offset each gets (L byte, L u16)
    constexpr std::array<std::pair<u8, u8>, kShadowPointers> kPointers = {
        {{7, 0}, {8, 0x16}, {9, 0x1a}, {10, 0x1c}, {11, 0x1e}, {5, 0x18}, {6, 0x20}}};
    for (u32 i = 0; i < kShadowPointers; ++i) {
        out.reg[i] = GuestLoad<u8>(out.layout + kPointers[i].first);
        out.off[i] = kPointers[i].second == 0 ? 0 : GuestLoad<u16>(out.layout + kPointers[i].second);
    }
    std::memcpy(out.shadow.data(), reinterpret_cast<const void*>(cue + 0x90 + u64{stage} * 0x1800), dwords * 4u);
    out.dwords = dwords;
    out.captured = true;
    return true;
}
} // namespace

std::array<u64, 4> GD::CaptureRefusals() {
    return Loaded(g_capture_refused);
}

std::array<u64, 4> GD::CompactCaptureRefusals() {
    return Loaded(g_compact_refused);
}

bool GD::CaptureStage(u64 r, u32 stage, StageShadow& out) {
    return CaptureStageOf(r, stage, out, g_capture_refused);
}

bool GD::CaptureStage(u64 r, u32 stage, CompactStageShadow& out) {
    return CaptureStageOf(r, stage, out, g_compact_refused);
}

bool GD::Describe(u64 material, u64 r, u64 records, u64 n, u64 mesh, DrawDescription& out) {
    out = {};
    if (g_real.vsharp == nullptr || g_real.init_buffer == nullptr || g_real.stage == nullptr ||
        g_module.load(std::memory_order_relaxed) == 0) {
        return false; // Prepare() not run: the pure callees / module base are unknown
    }
    const Fns f = DescribeFns();
    t_desc = &out;
    const bool followed = Instanced<false>(f, material, r, records, n, mesh);
    t_desc = nullptr;
    out.followed = followed;
    out.n = static_cast<u32>(n);
    out.index_count = GuestLoad<u32>(mesh + 0x68);
    out.index_base = GuestLoad<u64>(mesh + 0x58);
    return true;
}

std::string GD::CheckInstanced(u64 material, u64 r, u64 records, u64 n, u64 mesh, bool& skipped) {
    volatile u8 top = 0; // every frame of the runs is below
    Shadow& s = Shadows();
    const bool hand = g_impl_instanced == &InstancedNative;
    bool followed = true;
    for (u32 k = 0; k < 2; ++k) {
        GT::Trace& t = BeginRun(s, k, r, reinterpret_cast<u64>(&top));
        const u64 rr = reinterpret_cast<u64>(s.run[k].r);
        WipeStackBelow();
        if (k == 0) {
            Recorded(t, true, false, kInstancedFunction, [&] { g_rec_instanced(material, rr, records, n, mesh); });
        } else if (hand) {
            Recorded(t, false, true, kInstancedFunction,
                     [&] { followed = Instanced<false>(g_recorders, material, rr, records, n, mesh); });
        } else {
            Recorded(t, false, false, kInstancedFunction, [&] { g_impl_instanced(material, rr, records, n, mesh); });
        }
    }
    WipeStackBelow();
    if (!followed) {
        skipped = true;
        return {};
    }
    return CompareRuns(s, !hand, skipped);
}

std::string GD::CheckMesh(u64 material, u64 r, u64 instance, bool& skipped) {
    volatile u8 top = 0;
    Shadow& s = Shadows();
    const bool hand = g_impl_mesh == &MeshNative;
    bool followed = true;
    for (u32 k = 0; k < 2; ++k) {
        GT::Trace& t = BeginRun(s, k, r, reinterpret_cast<u64>(&top));
        const u64 rr = reinterpret_cast<u64>(s.run[k].r);
        WipeStackBelow();
        if (k == 0) {
            Recorded(t, true, false, kMeshFunction, [&] { g_rec_mesh(material, rr, instance); });
        } else if (hand) {
            Recorded(t, false, true, kMeshFunction, [&] { followed = Mesh<false>(g_recorders, material, rr, instance); });
        } else {
            Recorded(t, false, false, kMeshFunction, [&] { g_impl_mesh(material, rr, instance); });
        }
    }
    WipeStackBelow();
    if (!followed) {
        skipped = true;
        return {};
    }
    return CompareRuns(s, !hand, skipped);
}

namespace {

// --- the registry's entries -------------------------------------------------------------------------------------

/// A mismatch case: the arguments and the first difference's text.
struct Case {
    u64 args[5];
    char what[512];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, const u64 (&args)[5], const std::string& what) {
    Case c{};
    std::memcpy(c.args, args, sizeof(args));
    std::memcpy(c.what, what.data(), std::min(what.size(), sizeof(c.what) - 1));
    RecordCase(index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
}

template <typename Impl>
bool CheckCounted(const std::string& what, bool skipped, const u64 (&args)[5]) {
    Slot& slot = Impl::slot;
    if (skipped) {
        slot.skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    Count(slot.index, kCallsChecked);
    slot.verified.fetch_add(1, std::memory_order_relaxed);
    if (!what.empty()) {
        slot.mismatches.fetch_add(1, std::memory_order_relaxed);
        RecordMismatch(slot.index, args, what);
        return false;
    }
    return true;
}

struct InstancedImpl {
    static Slot slot;
    static GD::InstancedFn Original() {
        return reinterpret_cast<GD::InstancedFn>(slot.original);
    }
    static SSPC_SYSV_ABI void Native(u64 m, u64 r, u64 rec, u64 n, u64 mesh) {
        if (g_impl_instanced != &GD::InstancedNative) {
            g_impl_instanced(m, r, rec, n, mesh);
            return;
        }
        const bool done = g_stats ? Instanced<true>(g_real, m, r, rec, n, mesh) : Instanced<false>(g_real, m, r, rec, n, mesh);
        if (!done) { // a call the hand-written code does not follow: the original
            Original()(m, r, rec, n, mesh);
        }
    }
    static bool Check(u64 m, u64 r, u64 rec, u64 n, u64 mesh) {
        bool skipped = false;
        const std::string what = GD::CheckInstanced(m, r, rec, n, mesh, skipped);
        return CheckCounted<InstancedImpl>(what, skipped, {m, r, rec, n, mesh});
    }
    static SSPC_SYSV_ABI void Verify(u64 m, u64 r, u64 rec, u64 n, u64 mesh) {
        Check(m, r, rec, n, mesh);
        Count(slot.index, kCallsOriginal);
        Original()(m, r, rec, n, mesh);
    }
    static SSPC_SYSV_ABI void Guarded(u64 m, u64 r, u64 rec, u64 n, u64 mesh) {
        if (SampleThisCall(slot.index, slot.sample_period) && !Check(m, r, rec, n, mesh)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(m, r, rec, n, mesh);
            return;
        }
        Count(slot.index, kCallsNative);
        Native(m, r, rec, n, mesh);
    }
    static SSPC_SYSV_ABI void Bench(u64 m, u64 r, u64 rec, u64 n, u64 mesh) {
        Count(slot.index, kCallsOriginal);
        Original()(m, r, rec, n, mesh);
    }
};
Slot InstancedImpl::slot;

struct MeshImpl {
    static Slot slot;
    static GD::MeshFn Original() {
        return reinterpret_cast<GD::MeshFn>(slot.original);
    }
    static SSPC_SYSV_ABI void Native(u64 m, u64 r, u64 inst) {
        if (g_impl_mesh != &GD::MeshNative) {
            g_impl_mesh(m, r, inst);
            return;
        }
        const bool done = g_stats ? Mesh<true>(g_real, m, r, inst) : Mesh<false>(g_real, m, r, inst);
        if (!done) { // a call the hand-written code does not follow: the original
            Original()(m, r, inst);
        }
    }
    static bool Check(u64 m, u64 r, u64 inst) {
        bool skipped = false;
        const std::string what = GD::CheckMesh(m, r, inst, skipped);
        return CheckCounted<MeshImpl>(what, skipped, {m, r, inst, 0, 0});
    }
    static SSPC_SYSV_ABI void Verify(u64 m, u64 r, u64 inst) {
        Check(m, r, inst);
        Count(slot.index, kCallsOriginal);
        Original()(m, r, inst);
    }
    static SSPC_SYSV_ABI void Guarded(u64 m, u64 r, u64 inst) {
        if (SampleThisCall(slot.index, slot.sample_period) && !Check(m, r, inst)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(m, r, inst);
            return;
        }
        Count(slot.index, kCallsNative);
        Native(m, r, inst);
    }
    static SSPC_SYSV_ABI void Bench(u64 m, u64 r, u64 inst) {
        Count(slot.index, kCallsOriginal);
        Original()(m, r, inst);
    }
};
Slot MeshImpl::slot;



bool EnvOn(const char* name, char* value = nullptr, u32 size = 0) {
    char v[260] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return false;
    }
    if (value != nullptr) {
        std::snprintf(value, size, "%s", v);
    }
    return v[0] != '0';
}

void StatsReporter() {
    u64 last = R6GFrameId();
    for (;;) {
        Sleep(10000);
        const u64 now = R6GFrameId();
        const double frames = static_cast<double>(now - last);
        last = now;
        SSPC_LOG(Engine, Info, "r6g stats/10s ({:.0f} frames), per frame:{}", frames,
                 GD::StatsText(frames, true));
    }
}

/// Once for both specs: the implementation (SSPC_R6G_IMPL=recomp: recomp_1a1770 / recomp_1a1960 of the module
/// SSPC_NATIVE_ENGINE_RECOMP names), the measurement (SSPC_R6G_STATS=1).
bool PrepareInstall(u64 module_base, std::string& error) {
    static std::mutex mu;
    static bool done = false;
    static bool ok = false;
    static std::string saved;
    const std::scoped_lock lock(mu);
    if (done) {
        error = saved;
        return ok;
    }
    done = true;
    ok = GD::Prepare(module_base, saved);
    char impl[64] = {};
    if (ok && EnvOn("SSPC_R6G_IMPL", impl, sizeof(impl)) && std::strcmp(impl, "recomp") == 0) {
        char path[260] = {};
        HMODULE dll = nullptr;
        if (EnvOn("SSPC_NATIVE_ENGINE_RECOMP", path, sizeof(path))) {
            dll = GetModuleHandleA(path);
            if (dll == nullptr) {
                dll = LoadLibraryA(path);
            }
        }
        const auto base = dll != nullptr ? reinterpret_cast<u64*>(GetProcAddress(dll, "recomp_image_base")) : nullptr;
        const auto inst = dll != nullptr ? reinterpret_cast<GD::InstancedFn>(GetProcAddress(dll, "recomp_1a1770")) : nullptr;
        const auto mesh = dll != nullptr ? reinterpret_cast<GD::MeshFn>(GetProcAddress(dll, "recomp_1a1960")) : nullptr;
        const auto redirects =
            dll != nullptr ? reinterpret_cast<GD::RedirectsFn>(GetProcAddress(dll, "recomp_set_redirects")) : nullptr;
        if (base == nullptr || inst == nullptr || mesh == nullptr || redirects == nullptr) {
            saved = "SSPC_R6G_IMPL=recomp: SSPC_NATIVE_ENGINE_RECOMP has no recomp_1a1770 / recomp_1a1960 / "
                    "recomp_set_redirects / recomp_image_base";
            ok = false;
        } else {
            *base = module_base;
            GD::UseRecompiled(inst, mesh, redirects);
            SSPC_LOG(Engine, Warning, "R6-G: the geometry draws' implementation: the recompiled 0x1a1770 / 0x1a1960 ({})",
                     path);
        }
    }
    if (ok && EnvOn("SSPC_R6G_STATS")) {
        GD::EnableStats(true);
        std::thread(StatsReporter).detach();
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_R6G_STATS=1 - the geometry draws measured per call (hand-written "
                                  "path in native/guarded modes)");
    }
    error = saved;
    return ok;
}

bool SelfCheckInstancedInstall(void* original, std::string& error) {
    GD::GotForChecks guard(g_module.load(std::memory_order_relaxed));
    if (!guard.ok()) {
        error = "the stack guard's GOT slot is not writable memory: no self-check";
        return false;
    }
    g_instanced_original = reinterpret_cast<GD::InstancedFn>(original);
    error = GD::SelfCheckInstanced(2000, 0x5247314931373730ull);
    return error.empty();
}

bool SelfCheckMeshInstall(void* original, std::string& error) {
    GD::GotForChecks guard(g_module.load(std::memory_order_relaxed));
    if (!guard.ok()) {
        error = "the stack guard's GOT slot is not writable memory: no self-check";
        return false;
    }
    g_mesh_original = reinterpret_cast<GD::MeshFn>(original);
    SSPC_LOG(Engine, Info, "R6-G self-check: the GOT slots in the loader's window: stack guard {:#x}, defaults' global "
                           "{:#x}, vertex-format entries {:#x} {:#x} (replaced while it runs)",
             guard.saved(0), guard.saved(1), guard.saved(2), guard.saved(3));
    error = GD::SelfCheckMesh(2000, 0x5247314931393630ull);
    return error.empty();
}

using DI = Dispatch<void(u64, u64, u64, u64, u64)>;
using DM = Dispatch<void(u64, u64, u64)>;

} // namespace

GD::GotForChecks::GotForChecks(u64 module) {
    static const u64 kGuard = 0x5ec0de5ec0de5ec0ull;
    alignas(16) static u8 defaults[kDefaultsOffset + 0x100] = {};
    // the vertex-format table's two entries 0x45f590 reads (zero before the game's init: such calls are not
    // followed; a recording run never reads them - the V# comes from the trace)
    const u64 slots[kSlots] = {kStackGuardSlot, kDefaultsSlot, kVertexFormatTable + 0x40, kVertexFormatTable + 0x48};
    const u64 values[kSlots] = {reinterpret_cast<u64>(&kGuard), reinterpret_cast<u64>(defaults), 1, 1};
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    for (u32 k = 0; k < kSlots; ++k) {
        at_[k] = reinterpret_cast<u64*>(module + slots[k]);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(at_[k], &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & kWritable) == 0) {
            return;
        }
    }
    // (always replaced: before relocation a slot may hold a link-time value rather than 0 - the defaults' global did
    // in the game's loader window, 21.10.2026)
    for (u32 k = 0; k < kSlots; ++k) {
        saved_[k] = *at_[k];
        *at_[k] = values[k];
    }
    ok_ = true;
}

GD::GotForChecks::~GotForChecks() {
    if (ok_) {
        for (u32 k = 0; k < kSlots; ++k) {
            *at_[k] = saved_[k];
        }
    }
}

void GD::InjectFault(u32 fault) {
    g_fault = fault;
}

bool GD::Prepare(u64 module_base, std::string& error) {
    g_module.store(module_base, std::memory_order_relaxed);
    const auto at = [&](u64 rva) { return module_base + rva; };
    g_real = Fns{reinterpret_cast<StencilFn>(at(kRva[kStencil])),
                 reinterpret_cast<ContextFn>(at(kRva[kDrawConstants])),
                 reinterpret_cast<SetterFn>(at(kRva[kSetBuffers])),
                 reinterpret_cast<SetterFn>(at(kRva[kSetVertex])),
                 reinterpret_cast<Gfx1Fn>(at(kRva[kNumInstances])),
                 reinterpret_cast<Gfx1Fn>(at(kRva[kPrimitive])),
                 reinterpret_cast<Gfx2Fn>(at(kRva[kIndexType])),
                 reinterpret_cast<Gfx1Fn>(at(kRva[kDebugMarker])),
                 reinterpret_cast<CommitFn>(at(kRva[kCommit])),
                 reinterpret_cast<Gfx3Fn>(at(kRva[kDrawIndex])),
                 reinterpret_cast<VsharpFn>(at(kRva[kVsharp])),
                 reinterpret_cast<InitBufferFn>(at(kInitBufferRva)),
                 reinterpret_cast<StageFn>(at(kStageRva))};
    const auto thunk = [](Callee c) { return GT::ThunkAddress(c); };
    g_recorders = Fns{reinterpret_cast<StencilFn>(thunk(kStencil)),
                      reinterpret_cast<ContextFn>(thunk(kDrawConstants)),
                      reinterpret_cast<SetterFn>(thunk(kSetBuffers)),
                      reinterpret_cast<SetterFn>(thunk(kSetVertex)),
                      reinterpret_cast<Gfx1Fn>(thunk(kNumInstances)),
                      reinterpret_cast<Gfx1Fn>(thunk(kPrimitive)),
                      reinterpret_cast<Gfx2Fn>(thunk(kIndexType)),
                      reinterpret_cast<Gfx1Fn>(thunk(kDebugMarker)),
                      reinterpret_cast<CommitFn>(thunk(kCommit)),
                      reinterpret_cast<Gfx3Fn>(thunk(kDrawIndex)),
                      reinterpret_cast<VsharpFn>(thunk(kVsharp)),
                      g_real.init_buffer,
                      g_real.stage};
    for (u32 pass = 0; pass < 2; ++pass) {
        const Engine::GuestFunction& fn = pass == 0 ? kInstancedFunction : kMeshFunction;
        std::vector<CodeRedirect> redirects;
        const auto add = [&](Callee c) { redirects.push_back({at(kRva[c]), thunk(c)}); };
        if (pass == 0) {
            for (Callee c : kInstancedCallees) {
                add(c);
            }
        } else {
            for (Callee c : kMeshCallees) {
                add(c);
            }
        }
        std::vector<u8> bytes;
        const CloneResult clone =
            CloneGuestFunction(at(fn.rva), OriginalBytes(module_base, fn, bytes), fn.size, error, redirects);
        if (clone.code == nullptr) {
            error = fmt::format("recording clone of {:#x}: {}", fn.rva, error);
            return false;
        }
        if (pass == 0) {
            g_rec_instanced = reinterpret_cast<InstancedFn>(clone.code);
        } else {
            g_rec_mesh = reinterpret_cast<MeshFn>(clone.code);
        }
    }
    return true;
}

GD::InstancedFn GD::RecordingInstanced() {
    return g_rec_instanced;
}

GD::MeshFn GD::RecordingMesh() {
    return g_rec_mesh;
}

void GD::UseRecompiled(InstancedFn instanced, MeshFn mesh, RedirectsFn redirects) {
    g_impl_instanced = instanced != nullptr ? instanced : &InstancedNative;
    g_impl_mesh = mesh != nullptr ? mesh : &MeshNative;
    g_redirects = redirects;
}

namespace {

/// Generated memory for the self-checks: one block, every pointer the functions follow aimed inside it.
struct Arena {
    std::vector<u8> mem;
    std::mt19937_64 rng;
    explicit Arena(u64 seed) : mem(0x10000), rng(seed) {}
    u64 base() {
        return reinterpret_cast<u64>(mem.data());
    }
    void Fill() {
        for (u64 i = 0; i < mem.size(); i += 8) {
            const u64 v = rng();
            std::memcpy(mem.data() + i, &v, 8);
        }
    }
    void Put64(u64 off, u64 v) {
        std::memcpy(mem.data() + off, &v, 8);
    }
    void Put8(u64 off, u8 v) {
        mem[off] = v;
    }
};

} // namespace

std::string GD::SelfCheckInstanced(u32 cases, u64 seed) {
    if (g_rec_instanced == nullptr) {
        return "no recording clone (prepare did not run)";
    }
    Arena a(seed);
    // layout: material 0x0..0x200, mesh 0x400..0x480, streams 0x1000.., records 0x4000..
    for (u32 n = 0; n < cases; ++n) {
        a.Fill();
        const u64 b = a.base();
        const u64 m = b, mesh = b + 0x400, records = b + 0x4000;
        if (a.rng() % 4 == 0) {
            a.Put8(0x1c, 0); // no stencil
        }
        if (a.rng() % 3 == 0) {
            a.Put8(0x135, 0xff);
        }
        if (a.rng() % 3 == 0) {
            a.Put8(0x13c, 0xff);
        }
        a.Put8(0x400 + 0x6c, static_cast<u8>(a.rng() % 33));
        a.Put64(0x400 + 0x60, b + 0x1000);
        const u64 count = a.rng() % 3 == 0 ? a.rng() : a.rng() % 64;
        std::vector<u8> r_live(GD::kShadowBytes);
        // the stencil state: random, or what this case computes (no write)
        const u16 st = static_cast<u16>(a.mem[0x1c] | (a.mem[0x1d] << 8));
        const u8 hi = static_cast<u8>((st >> 8) | a.mem[0x400 + 0x1d]);
        u32 v = (u32{static_cast<u8>(st)} << 16) + (u32{hi} << 24) + 0xff00u;
        if (a.rng() % 2 == 0) {
            v = static_cast<u32>(a.rng());
        }
        std::memcpy(r_live.data() + GD::kStencilState, &v, 4);
        bool skipped = false;
        const std::string d =
            CheckInstanced(m, reinterpret_cast<u64>(r_live.data()), records, count, mesh, skipped);
        if (skipped) {
            return fmt::format("case {}: not comparable (trace overflow)", n);
        }
        if (!d.empty()) {
            return fmt::format("case {}: {}", n, d);
        }
    }
    return {};
}

std::string GD::SelfCheckMesh(u32 cases, u64 seed) {
    if (g_rec_mesh == nullptr) {
        return "no recording clone (prepare did not run)";
    }
    Arena a(seed);
    // layout: material 0x0..0x200 (+0xf0 -> 0x600, +0x10 -> 0x700 -> +0x58 -> 0x800), instance 0x200..0x280,
    // mesh instance 0x300 (-> mesh 0x400), skin 0x900..0xa00 (+8 -> 0xb00 -> +0x58 -> 0xc00 indices), streams 0x1000..
    u32 compared = 0;
    for (u32 n = 0; n < cases; ++n) {
        a.Fill();
        const u64 b = a.base();
        a.Put64(0xf0, b + 0x600);
        a.Put64(0x10, b + 0x700);
        a.Put64(0x700 + 0x58, b + 0x800);
        a.Put64(0x200 + 0x18, b + 0x300);
        a.Put64(0x300, b + 0x400);
        // (the original writes outside its 0x200-byte stack copy for more than 32 streams or a first index past 31:
        // never generated - the eboot code runs here)
        const u32 streams = a.rng() % 8 == 0 ? static_cast<u32>(a.rng() % 33) : static_cast<u32>(a.rng() % 17);
        a.Put8(0x400 + 0x6c, static_cast<u8>(streams));
        a.Put64(0x400 + 0x60, b + 0x1000);
        const bool skin = a.rng() % 3 != 0;
        a.Put64(0x200 + 0x20, skin ? b + 0x900 : 0);
        if (a.rng() % 4 == 0) {
            a.Put8(0x900, static_cast<u8>(a.mem[0x900] & ~7u)); // a skin without deformed streams
        }
        a.Put64(0x900 + 8, b + 0xb00);
        a.Put64(0xb00 + 0x58, b + 0xc00);
        const u32 lim = std::max<u32>(streams, 1);
        a.Put8(0xc00, static_cast<u8>(a.rng() % 16 == 0 ? a.rng() % GD::kMaxStreams : a.rng() % lim));
        a.Put8(0xc01, static_cast<u8>(a.rng() % 3 == 0 ? 0x80 | a.rng() : a.rng() % lim));
        a.Put8(0xc02, static_cast<u8>(a.rng() % 3 == 0 ? 0x80 | a.rng() : a.rng() % lim));
        if (a.rng() % 3 == 0) {
            a.Put64(0x900 + 0x20, 0);
        }
        if (a.rng() % 3 == 0) {
            a.Put8(0xd8, 0xff);
        }
        std::vector<u8> r_live(GD::kShadowBytes);
        bool skipped = false;
        const std::string d = CheckMesh(b, reinterpret_cast<u64>(r_live.data()), b + 0x200, skipped);
        if (!d.empty()) {
            return fmt::format("case {}: {}", n, d);
        }
        compared += skipped ? 0 : 1;
    }
    if (compared < cases / 2) {
        return fmt::format("only {} of {} cases comparable", compared, cases);
    }
    return {};
}

void GD::EnableStats(bool on) {
    g_stats = on;
}

bool GD::StatsEnabled() {
    return g_stats;
}

std::string GD::StatsText(double frames, bool reset) {
    u64 sum[2][kStats] = {};
    {
        const std::scoped_lock lock(g_stat_mu);
        for (StatBlock* b : g_stat_blocks) {
            for (u32 f = 0; f < 2; ++f) {
                for (u32 k = 0; k < kStats; ++k) {
                    sum[f][k] += reset ? b->v[f][k].exchange(0, std::memory_order_relaxed)
                                       : b->v[f][k].load(std::memory_order_relaxed);
                }
            }
        }
    }
    std::string out;
    const double div = frames > 0 ? frames : 1.0;
    for (u32 f = 0; f < 2; ++f) {
        out += f == 0 ? "\n  0x1a1770 instanced draw:" : "\n  0x1a1960 mesh draw:";
        for (u32 k = 0; k < kStats; ++k) {
            out += fmt::format(" {}={:.1f}", kStatNames[k], static_cast<double>(sum[f][k]) / div);
        }
    }
    return out;
}

const FunctionSpec kGBufferInstancedDrawSpec{
    "gbuffer_instanced_draw",
    "GBuffer::InstancedDraw",
    kInstancedFunction,
    kAssumedInstanced,
    static_cast<u32>(std::size(kAssumedInstanced)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&DI::Entry<InstancedImpl>),
    &InstancedImpl::slot,
    &SelfCheckInstancedInstall,
    &PrepareInstall,
    true, // explicit_only until GAME_VERIFIED (docs/NATIVE_RENDER_R6_GBUFFER.md)
};

const FunctionSpec kGBufferMeshDrawSpec{
    "gbuffer_mesh_draw",
    "GBuffer::MeshDraw",
    kMeshFunction,
    nullptr,
    0,
    nullptr,
    0,
    reinterpret_cast<const void*>(&DM::Entry<MeshImpl>),
    &MeshImpl::slot,
    &SelfCheckMeshInstall,
    &PrepareInstall,
    true,
};

} // namespace SecondSon::NativeEngine::Reconstructed
