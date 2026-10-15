// SecondSonPC — Native Render Interface, engine side (see native_draw.h).
#include "engine/render/native_draw.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <intrin.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/native_state.h"
#include "engine/render/render_layout.h"

namespace SecondSon::Engine::Render::NativeDraw {

namespace {

namespace L = Layout;

// eboot.bin of CUSA00004 (tools/native-reconstruction/function_hashes.py); prologues checked with queries/prologue.py.
constexpr ProbeSite kDrawIndexSite{{0x623c60, 0x7a, 0x90df1869fca20135ull}, 14};
constexpr ProbeSite kDrawAutoSite{{0x623a40, 0x6f, 0x1e6017d7a3f4de6cull}, 16};
constexpr ProbeSite kCommitSite{{0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, 20};
constexpr GuestFunction kAssumed[] = {
    {0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, // CUE commit: shaders from +0xC0A8/+0xC0B0, snapshot = shadow, user data
    {0x1f2c60, 0x1f, 0xef1991cc4587b6dfull},  // AcquireContext: the pool's layout (pass keys)
    // The native commit (apply mode) does what these do, or calls them:
    {0x123d0, 0x94, 0x7bf8f59b6455f910ull},   // CUE linear allocator (snapshots), called
    {0x638540, 0x229, 0xe7ccd229525c9081ull}, // PS input mapping (SPI_PS_INPUT_CNTL values), called
    {0x626400, 0x89, 0xc135b5f904cb94d7ull},  // setPointerInUserData: SET_SH_REG USER_DATA+sgpr {lo, hi}
    {0x626520, 0x143, 0x1e04ce784703751bull}, // setUserDataRegion: NOP 0x6875000D + SET_SH_REG USER_DATA+sgpr
    {0x624cb0, 0x15f, 0x79dea00a63f200beull}, // SET_CONTEXT_REG SPI_PS_INPUT_CNTL_0 x n
};

// P5.3D native render state (native_state.h): the context's state methods, probed at entry.
constexpr ProbeSite kSetTargetsSite{{0x11f910, 0x3ec, 0x65b79a72c0f31c59ull}, 17};
constexpr ProbeSite kTransitionSite{{0x120020, 0x206, 0xe0bb63e1c9509805ull}, 17};
constexpr ProbeSite kPrimTypeSite{{0x623740, 0x68, 0xad8002618fc3e975ull}, 17};
constexpr ProbeSite kIndexSizeSite{{0x624f80, 0x7e, 0x49f8a233037eaadcull}, 16};
constexpr GuestFunction kStateAssumed[] = {
    {0x11ffe0, 0x33, 0xe2332a445ab4b43full},  // SetStateBlock: the block layout R+0xDE48..+0xDE84 (no PM4)
    {0x637bf0, 0x110, 0xface254e00952095ull}, // RenderTarget base, called
    {0x637f30, 0xf3, 0x4e98f932a83ffc48ull},  // RenderTarget FMASK base, called
    {0x637e30, 0xfe, 0x99edeec68bcc4fd2ull},  // RenderTarget set FMASK base, called
    {0x621450, 0xdb, 0x574543ccea059a4cull},  // DepthRenderTarget Z base, called
    {0x6219e0, 0xe1, 0x7f2e84c819b5dcafull},  // DepthRenderTarget HTILE base, called
};
constexpr u64 kBarrierFlag = 0xe15e10;  ///< u8: 0x11f910 starts with ACQUIRE_MEM (0x625e80) when set
constexpr u64 kDebugRtFlag = 0xe171fc;  ///< u8: 0x11f910 ends with a debug colour target 7 / depth when set
constexpr u64 kDbReg2deTable = 0xaefb40; ///< u32[]: 0x6272e0's values

// CUE fields the commit 0x14ad0 reads (GFX_CONTEXT_LAYOUT.md §2, RENDER_ENGINE_ARCHITECTURE.md §4).
constexpr u64 kCuePsShader = 0xC0A8;  ///< PS shader object; its register block at +8
constexpr u64 kCueVsShader = 0xC0B0;  ///< VS shader object
constexpr u64 kCueVsModifier = 0xC168;
constexpr u64 kCuePsAltPath = 0xC1D9; ///< nonzero: the commit sets the PS another way (0x6251c0)
constexpr u64 kCueVsAltPath = 0xC1DA; ///< nonzero: ... the VS (0x625300)
constexpr u32 kCueStagePs = 1;        ///< CUE stage index = Gnm::ShaderStage
constexpr u32 kCueStageVs = 2;

using DrawIndexFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 count, u64 indices, u32 flags);
using DrawAutoFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 count, u32 flags);
using ReserveFn = SSPC_SYSV_ABI bool (*)(u8* cb, u32 dwords, u64 user);

using CommitFn = SSPC_SYSV_ABI void (*)(u8* cue);

u64 g_base = 0;
u64 g_canary = 0;        ///< SSPC_P53_NATIVE_DRAW_CANARY=<n>: every n-th marker is spoiled on purpose (0: none)
u64 g_apply_passes = 0;  ///< apply:<passes>: bit per pass whose commits and draws run natively
bool g_verify = true;    ///< SSPC_P53_NATIVE_VERIFY=0 (measurements): no shadow markers, no kVerify on native draws
bool g_describe = false; ///< P5.3 SSPC_P53_NATIVE_DESCRIBE=1: without verification, the other passes' draws get a
                         ///< kDescribe marker (their tables for the Native Render Frontend; the game's packet follows)
DrawIndexFn g_draw_index = nullptr;
DrawAutoFn g_draw_auto = nullptr;
CommitFn g_commit = nullptr;
CommitEnv g_env;

// P5.3D: SSPC_P53_NATIVE_STATE=verify|apply.
using SetTargetsFn = SSPC_SYSV_ABI void (*)(u8* r, u64 targets, const u32* rect);
using TransitionFn = SSPC_SYSV_ABI void (*)(u8* r, u64 block);
using PrimTypeFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 prim);
using IndexSizeFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 size, u32 policy);
bool g_state_on = false;     ///< the probes are installed
bool g_state_apply = false;  ///< apply: in the apply passes the state methods run natively
bool g_state_verify = true;  ///< the other known contexts: the game's method, then a kVerify state marker
SetTargetsFn g_set_targets = nullptr;
TransitionFn g_transition = nullptr;
PrimTypeFn g_prim_type = nullptr;
IndexSizeFn g_index_size = nullptr;
NativeState::Env g_state_env;
thread_local bool t_in_state = false; ///< inside a verified state method (its nested calls are part of it)

enum StateFn : u32 { kFnTargets, kFnTransition, kFnPrim, kFnIndex, kFnLeaf, kStateFns };
void* g_leaf_game[NativeState::kLeafCount] = {};
std::atomic<u64> g_leaf_native[NativeState::kLeafCount] = {};
enum StateFallback : u32 { kFbFlags, kFbOverflow, kFbNoSpace, kStateFallbacks };
struct StateCounters {
    std::atomic<u64> native[kStateFns] = {};
    std::atomic<u64> fallback[kStateFallbacks] = {};
    std::atomic<u64> verified[kStateFns] = {};
    std::atomic<u64> block_mismatch[kStateFns] = {};  ///< R+0xDE48.. after the game's method != the native result
    std::atomic<u64> size_mismatch[kStateFns] = {};   ///< PM4 dwords the game wrote != the native count
    std::atomic<u64> size_unchecked{0};               ///< the game's packets crossed into a new chunk
    std::atomic<u32> first_size_fn{0xFFFFFFFF}, first_size_want{0}, first_size_have{0};
};
StateCounters g_state;

struct EngineCounters {
    std::atomic<u64> draws{0};
    std::atomic<u64> marked{0};
    std::atomic<u64> foreign{0}; ///< the command buffer is not a render context SecondSonPC knows
    std::atomic<u64> no_space{0};
    std::atomic<u64> native_draws{0};
    std::atomic<u64> draw_fallbacks{0}; ///< apply pass, but predication/flags: the game's wrapper ran
    std::atomic<u64> native_commits{0};
    std::atomic<u64> commit_fallbacks[4] = {}; ///< CommitCheck reasons: the game's commit ran
    // P5.3 direct path
    std::atomic<u64> direct_described{0};   ///< descriptions written into a draw's trailing NOP
    std::atomic<u64> direct_undescribed{0}; ///< ... the game's draw left no NOP where expected (not described)
    std::atomic<u64> direct_runs{0};        ///< run tokens written into a DCB
    std::atomic<u64> direct_ops{0};         ///< operations put into the native stream
};
EngineCounters g_engine;

bool g_direct = false; ///< SSPC_P53_DIRECT=1: native operations and descriptions without markers (native_render_ops.h)
constexpr u32 kStreamContexts = L::kPoolSlots + 2 + L::kDeformContextCount;
StreamCursor g_cursor[kStreamContexts]; ///< a context's place in the native stream (its recording thread's)

/// Native in this pass now (the A/B schedule may hold it back for a block of frames: NativeActive).
bool Applies(s32 pass) {
    return pass >= 0 && pass < 64 && ((g_apply_passes >> pass) & 1) != 0 && NativeActive();
}

template <typename T>
T At(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

/// The pass key of a context, or false if `r` is none of the frame's contexts.
bool ContextPass(u64 base, u64 r, s32& pass, u32& sub) {
    const u64 frame = base + L::kFrame;
    const u64 pool = frame + L::kFramePool;
    if (r >= pool && r < pool + u64{L::kPoolSlots} * L::kPoolSlotStride && (r - pool) % L::kPoolSlotStride == 0) {
        pass = At<s32>(r + L::kSlotPass);
        sub = At<u32>(r + L::kSlotSub);
        return true;
    }
    sub = 0;
    if (r == frame + L::kFramePreContext) {
        pass = -2;
        return true;
    }
    if (r == frame + L::kFramePostContext) {
        pass = -3;
        return true;
    }
    const u64 deform = base + L::kDeformContexts;
    if (r >= deform && r < deform + u64{L::kDeformContextCount} * L::kContextSize &&
        (r - deform) % L::kContextSize == 0) {
        pass = -4;
        return true;
    }
    return false;
}

/// The stage's table shape from its user-data layout table; false when the stage has none.
bool LayoutShape(u64 cue, u32 stage, bool vertex, TableShape& s) {
    const u64 layout = At<u64>(cue + L::kCueLayout + u64{stage} * 8);
    if (layout == 0) {
        return false;
    }
    s.dwords = static_cast<u16>(std::min<u32>(At<u16>(layout), 0x600));
    // Commit 0x14ad0: SGPR byte -> pointer to snapshot + u16 offset (dwords). 0xFF: not used by the shader.
    struct Ref {
        u32 sgpr_byte;
        u32 offset_field; ///< 0: the snapshot's start
    };
    static constexpr Ref kRefs[] = {{7, 0}, {8, 0x16}, {9, 0x1A}, {0xA, 0x1C}, {0xB, 0x1E}};
    static constexpr Ref kVertexRefs[] = {{5, 0x18}, {6, 0x20}};
    const auto add = [&](const Ref& r) {
        const u8 sgpr = At<u8>(layout + r.sgpr_byte);
        if (sgpr != 0xFF && sgpr <= 14 && s.refs < 7) {
            s.sgpr[s.refs] = sgpr;
            s.offset[s.refs++] = r.offset_field == 0 ? u16{0} : At<u16>(layout + r.offset_field);
        }
    };
    for (const Ref& r : kRefs) {
        add(r);
    }
    if (vertex) {
        for (const Ref& r : kVertexRefs) {
            add(r);
        }
    }
    return true;
}

void DescribeTable(u64 cue, u32 stage, bool vertex, StageTable& t) {
    TableShape s;
    if (!LayoutShape(cue, stage, vertex, s)) {
        return;
    }
    t.present = true;
    t.dwords = s.dwords;
    t.hash = HashBytes(reinterpret_cast<const void*>(cue + L::kCueShadow + u64{stage} * L::kCueShadowStride),
                       u64{t.dwords} * 4);
    t.refs = s.refs;
    for (u32 i = 0; i < s.refs; ++i) {
        t.ref[i] = TableRef{s.sgpr[i], s.offset[i]};
    }
}

/// Room for `dwords` at the buffer's write pointer, asked the way the game's own wrappers ask (the buffer's callback:
/// 0x11eed0 for a render context). The write pointer, or null.
u32* Room(u8* dcb, u32 dwords) {
    const u64 cb = reinterpret_cast<u64>(dcb);
    if ((At<u64>(cb + L::kCbEnd) - At<u64>(cb + L::kCbCmdPtr)) / 4 < dwords) {
        const auto reserve = At<ReserveFn>(cb + 0x18);
        if (reserve == nullptr || !reserve(dcb, dwords, At<u64>(cb + 0x20))) {
            return nullptr;
        }
    }
    return reinterpret_cast<u32*>(At<u64>(cb + L::kCbCmdPtr));
}

void Advance(u8* dcb, u32* at, u32 dwords) {
    const u64 next = reinterpret_cast<u64>(at + dwords);
    std::memcpy(dcb + L::kCbCmdPtr, &next, sizeof(next));
}

// --- P5.3 direct path -----------------------------------------------------------------------------------------------

/// The context's index in g_cursor: pool slots, then the pre and post contexts, then the deformation contexts; -1 if
/// `r` is none of the frame's contexts.
s32 ContextIndex(u64 base, u64 r) {
    const u64 frame = base + L::kFrame;
    const u64 pool = frame + L::kFramePool;
    if (r >= pool && r < pool + u64{L::kPoolSlots} * L::kPoolSlotStride && (r - pool) % L::kPoolSlotStride == 0) {
        return static_cast<s32>((r - pool) / L::kPoolSlotStride);
    }
    if (r == frame + L::kFramePreContext) {
        return L::kPoolSlots;
    }
    if (r == frame + L::kFramePostContext) {
        return L::kPoolSlots + 1;
    }
    const u64 deform = base + L::kDeformContexts;
    if (r >= deform && r < deform + u64{L::kDeformContextCount} * L::kContextSize &&
        (r - deform) % L::kContextSize == 0) {
        return static_cast<s32>(L::kPoolSlots + 2 + (r - deform) / L::kContextSize);
    }
    return -1;
}

/// The ids of the draw's VS and PS table shapes (0: the stage reads no table); false when the shape table is full.
bool DrawShapes(u64 cue, u32& vs, u32& ps) {
    const auto one = [cue](u32 stage, bool vertex, u32& id) {
        TableShape s;
        id = 0;
        return !LayoutShape(cue, stage, vertex, s) || s.refs == 0 || s.dwords == 0 || InternShape(s, id);
    };
    return one(kCueStageVs, true, vs) && one(kCueStagePs, false, ps);
}

/// One operation into the context's native stream: `fill(op)` writes it (header first, at most `max_words`) and returns
/// its words (0: nothing to run); a run token goes into the DCB when the operation starts a run. False: nothing done
/// (no room for a token in the DCB, no stream block free) - the caller runs the game's path; `fill` ran only on true.
template <typename Fill>
bool EmitDirect(u8* dcb, u32 max_words, Fill&& fill) {
    const s32 ci = ContextIndex(g_base, reinterpret_cast<u64>(dcb) - L::kContextGfx);
    if (ci < 0) {
        return false;
    }
    u32* at = Room(dcb, kStreamTokenDwords); // (made before the operation: once it ran, it must be emitted)
    if (at == nullptr) {
        g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    StreamCursor& c = g_cursor[ci];
    u32* op = StreamReserve(c, reinterpret_cast<u64>(at), max_words);
    if (op == nullptr) {
        return false;
    }
    const u32 words = fill(op);
    if (words == 0) {
        return true;
    }
    u32 token[kStreamTokenDwords];
    if (!StreamCommit(c, words, token)) {
        std::memcpy(at, token, sizeof(token));
        Advance(dcb, at, kStreamTokenDwords);
        StreamTokenWritten(c, reinterpret_cast<u64>(at + kStreamTokenDwords));
        g_engine.direct_runs.fetch_add(1, std::memory_order_relaxed);
    }
    g_engine.direct_ops.fetch_add(1, std::memory_order_relaxed);
    return true;
}

constexpr u32 kDrawIndex2Header = 0xC0042700u;   ///< sceGnmDrawIndex: DRAW_INDEX_2, 5 body dwords (gnm_driver.cpp)
constexpr u32 kDrawIndexAutoHeader = 0xC0012D00u; ///< sceGnmDrawIndexAuto: DRAW_INDEX_AUTO, 2 body dwords

bool Mark(u8* dcb, bool indexed, u32 count, u64 indices, u32 flags, u32 packet_dwords);

/// The direct path's draw wrapper: in an apply pass the draw is a stream operation (after the commit's, in one run
/// when nothing of the game's came between); otherwise the game's wrapper draws and the description goes into the free
/// dwords of the NOP the driver wrote after the draw packet. Verification keeps its markers (Mark).
template <typename Call>
void DirectDraw(u8* dcb, bool indexed, u32 count, u64 indices, u32 flags, u32 packet_dwords, Call&& game) {
    const u64 cb = reinterpret_cast<u64>(dcb);
    const u64 r = cb - L::kContextGfx;
    s32 pass = -1;
    u32 sub = 0;
    if (!ContextPass(g_base, r, pass, sub) || At<u64>(r + L::kContextCue + L::kCueDcb) != cb) {
        g_engine.draws.fetch_add(1, std::memory_order_relaxed);
        g_engine.foreign.fetch_add(1, std::memory_order_relaxed);
        game();
        return;
    }
    const bool apply_pass = Applies(pass);
    if (!apply_pass && g_verify) {
        if (!Mark(dcb, indexed, count, indices, flags, packet_dwords)) {
            game();
        }
        return;
    }
    g_engine.draws.fetch_add(1, std::memory_order_relaxed);
    // the predication bit of the buffer (+0x28) and the wrapper's flags reach the packet; a native draw has neither
    const bool apply = apply_pass && flags == 0 && (At<u32>(cb + 0x28) & 1) == 0;
    const bool describe = g_describe && NativeActive();
    if (!apply && !describe) {
        if (apply_pass) {
            g_engine.draw_fallbacks.fetch_add(1, std::memory_order_relaxed);
        }
        game();
        return;
    }
    u32 vs = 0, ps = 0;
    const bool shapes = DrawShapes(r + L::kContextCue, vs, ps);
    if (apply && shapes) {
        const u64 base = indexed ? indices : 0;
        const bool emitted = EmitDirect(dcb, 1 + kStreamDrawWords, [&](u32* op) {
            op[0] = OpHeader(StreamOp::Draw, kStreamDrawWords);
            op[1] = static_cast<u32>(pass);
            op[2] = indexed ? 1u : 0u;
            op[3] = count;
            op[4] = static_cast<u32>(base);
            op[5] = static_cast<u32>(base >> 32);
            op[6] = vs;
            op[7] = ps;
            return 1 + kStreamDrawWords;
        });
        if (emitted) {
            g_engine.marked.fetch_add(1, std::memory_order_relaxed);
            g_engine.native_draws.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (apply_pass) {
        g_engine.draw_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    game();
    if (!describe || !shapes) {
        return;
    }
    // the draw packet and its trailing NOP end at the write pointer (in the buffer's current chunk)
    const u64 end = At<u64>(cb + L::kCbCmdPtr);
    const u64 packet = end - u64{packet_dwords} * 4;
    if (end < u64{packet_dwords} * 4 || packet < At<u64>(cb + L::kCbBegin)) {
        g_engine.direct_undescribed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* p = reinterpret_cast<u32*>(packet);
    u32* nop = p + packet_dwords - 4;
    if (p[0] != (indexed ? kDrawIndex2Header : kDrawIndexAutoHeader) || nop[0] != kDrawNopHeader || nop[1] != 0) {
        g_engine.direct_undescribed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    nop[2] = DescribeWord(pass, vs);
    nop[3] = ps;
    nop[1] = kDescribeMagic;
    g_engine.direct_described.fetch_add(1, std::memory_order_relaxed);
}

/// Shadow: a kVerify marker before the game's draw packet. Apply (a listed pass): the draw itself as a kApply|kVerify
/// marker; returns true when the game's wrapper must not run.
bool Mark(u8* dcb, bool indexed, u32 count, u64 indices, u32 flags, u32 packet_dwords) {
    g_engine.draws.fetch_add(1, std::memory_order_relaxed);
    DrawOp op;
    if (!BuildDrawOp(g_base, reinterpret_cast<u64>(dcb) - L::kContextGfx, indexed, count, indices, op)) {
        g_engine.foreign.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The predication bit of the buffer (+0x28) and the wrapper's flags reach the packet; a native draw has neither.
    const bool apply = Applies(op.pass) && flags == 0 && (At<u32>(reinterpret_cast<u64>(dcb) + 0x28) & 1) == 0;
    if (Applies(op.pass) && !apply) {
        g_engine.draw_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    // P5.3: the other passes' draws described for the Native Render Frontend (in the A/B comparison's native blocks)
    const bool describe = !apply && g_describe && NativeActive();
    if (!apply && !g_verify && !describe) {
        return false; // SSPC_P53_NATIVE_VERIFY=0: no shadow marker, the game's draw as it is
    }
    if (!apply && g_verify && g_canary != 0 && g_engine.draws.load(std::memory_order_relaxed) % g_canary == 0) {
        op.canary = true; // the processor must see the count differ
        op.index_count += 1;
    }
    u32* at = Room(dcb, kDrawMarkerDwords + (apply ? 0 : packet_dwords));
    if (at == nullptr) {
        g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
        return false; // nothing written: the game's wrapper draws (it reports its own failure if it has no room)
    }
    const u16 marker = apply ? u16(g_verify ? kApply | kVerify : kApply)
                             : u16((g_verify ? kVerify : 0) | (describe ? kDescribe : 0));
    Advance(dcb, at, EncodeDrawMarker(op, marker, at));
    g_engine.marked.fetch_add(1, std::memory_order_relaxed);
    if (apply) {
        g_engine.native_draws.fetch_add(1, std::memory_order_relaxed);
    }
    return apply;
}

SSPC_SYSV_ABI void DrawIndexProbe(u8* dcb, u32 count, u64 indices, u32 flags) {
    if (g_direct) {
        DirectDraw(dcb, true, count, indices, flags, 10, [&] { g_draw_index(dcb, count, indices, flags); });
    } else if (!Mark(dcb, true, count, indices, flags, 10)) {
        g_draw_index(dcb, count, indices, flags);
    }
}

SSPC_SYSV_ABI void DrawAutoProbe(u8* dcb, u32 count, u32 flags) {
    if (g_direct) {
        DirectDraw(dcb, false, count, 0, flags, 7, [&] { g_draw_auto(dcb, count, flags); });
    } else if (!Mark(dcb, false, count, 0, flags, 7)) {
        g_draw_auto(dcb, count, flags);
    }
}

/// Apply mode: the CUE commit of a listed pass's context runs natively (CheckNativeCommit), otherwise the game's.
SSPC_SYSV_ABI void CommitProbe(u8* cue_ptr) {
    const u64 cue = reinterpret_cast<u64>(cue_ptr);
    const u64 r = cue - L::kContextCue;
    s32 pass = -1;
    u32 sub = 0;
    if (!ContextPass(g_base, r, pass, sub) || !Applies(pass) || At<u64>(cue + L::kCueDcb) != r + L::kContextGfx) {
        g_commit(cue_ptr);
        return;
    }
    if (const CommitCheck check = CheckNativeCommit(cue); check != CommitCheck::Native) {
        g_engine.commit_fallbacks[static_cast<u32>(check)].fetch_add(1, std::memory_order_relaxed);
        g_commit(cue_ptr);
        return;
    }
    u8* dcb = reinterpret_cast<u8*>(r + L::kContextGfx);
    if (g_direct) {
        // the commit's register writes as a stream operation (the draw after it joins the run)
        const bool emitted = EmitDirect(dcb, 1 + 2 * kMaxRegWrites, [&](u32* op) {
            RegWrite writes[kMaxRegWrites];
            const u32 n = NativeCommit(cue, g_env, writes);
            op[0] = OpHeader(StreamOp::Registers, 2 * n);
            std::memcpy(op + 1, writes, sizeof(RegWrite) * n);
            return n == 0 ? 0u : 1 + 2 * n;
        });
        if (!emitted) {
            g_commit(cue_ptr);
            return;
        }
        g_engine.native_commits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u32* at = Room(dcb, RegistersMarkerDwords(kMaxRegWrites));
    if (at == nullptr) {
        g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
        g_commit(cue_ptr);
        return;
    }
    RegWrite writes[kMaxRegWrites];
    const u32 n = NativeCommit(cue, g_env, writes);
    if (n != 0) {
        Advance(dcb, at, EncodeRegistersMarker({writes, n}, kApply, at));
    }
    g_engine.native_commits.fetch_add(1, std::memory_order_relaxed);
}

// --- P5.3D: native render state -------------------------------------------------------------------------------------

enum class StatePath { Game, Verify, Apply };

StatePath DecideState(u64 r) {
    s32 pass = -1;
    u32 sub = 0;
    if (t_in_state || !ContextPass(g_base, r, pass, sub)) {
        return StatePath::Game;
    }
    if (g_state_apply && Applies(pass)) {
        return StatePath::Apply;
    }
    return g_state_verify ? StatePath::Verify : StatePath::Game;
}

/// 0x11f910's barrier / debug-target branches (globals the game sets): not reproduced, the game's method runs.
bool StateFlagsSet() {
    return At<u8>(g_base + kBarrierFlag) != 0 || At<u8>(g_base + kDebugRtFlag) != 0;
}

/// The operation's writes as a state marker in the DCB. False (nothing written) when it cannot be.
bool EmitState(u8* dcb, const NativeState::Writer& w, u16 flags) {
    if (w.overflow) {
        g_state.fallback[kFbOverflow].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (w.n == 0) {
        return true;
    }
    if (g_direct && flags == kApply) { // (a kVerify state marker is a check: it stays a marker)
        const bool emitted = EmitDirect(dcb, 1 + 2 * w.n, [&](u32* op) {
            op[0] = OpHeader(StreamOp::State, 2 * w.n);
            std::memcpy(op + 1, w.out, sizeof(RegWrite) * w.n);
            return 1 + 2 * w.n;
        });
        if (!emitted) {
            g_state.fallback[kFbNoSpace].fetch_add(1, std::memory_order_relaxed);
        }
        return emitted;
    }
    u32* at = Room(dcb, RegistersMarkerDwords(w.n));
    if (at == nullptr) {
        g_state.fallback[kFbNoSpace].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Advance(dcb, at, EncodeRegistersMarker({w.out, w.n}, flags, at, OpKind::State));
    return true;
}

/// Verify: the game's method runs (`call`); what it left is compared with the native result - the PM4 dwords it wrote
/// (engine side), R's state block (when `block` is given), and, through a kVerify marker after its packets, the
/// register file and target extents the processor holds (processor side).
template <typename Call>
void VerifyState(StateFn fn, u64 r, u8* dcb, const NativeState::StateBlock* block, const NativeState::Writer& w,
                 Call&& call) {
    const u64 cb = reinterpret_cast<u64>(dcb);
    const u64 end0 = At<u64>(cb + L::kCbEnd), ptr0 = At<u64>(cb + L::kCbCmdPtr);
    t_in_state = true;
    call();
    t_in_state = false;
    const u64 end1 = At<u64>(cb + L::kCbEnd), ptr1 = At<u64>(cb + L::kCbCmdPtr);
    g_state.verified[fn].fetch_add(1, std::memory_order_relaxed);
    if (end1 != end0 || ptr1 < ptr0) {
        g_state.size_unchecked.fetch_add(1, std::memory_order_relaxed);
    } else if (const u64 wrote = (ptr1 - ptr0) / 4; wrote != w.pm4_dwords) {
        g_state.size_mismatch[fn].fetch_add(1, std::memory_order_relaxed);
        u32 none = 0xFFFFFFFF;
        if (g_state.first_size_fn.compare_exchange_strong(none, fn)) {
            g_state.first_size_want.store(w.pm4_dwords);
            g_state.first_size_have.store(static_cast<u32>(wrote));
        }
    }
    if (block != nullptr) {
        NativeState::StateBlock after;
        NativeState::LoadBlock(r, after);
        if (std::memcmp(&after, block, NativeState::kStateBlockBytes) != 0) {
            g_state.block_mismatch[fn].fetch_add(1, std::memory_order_relaxed);
        }
    }
    EmitState(dcb, w, kVerify);
}

SSPC_SYSV_ABI void SetTargetsProbe(u8* rp, u64 targets, const u32* rect) {
    const u64 r = reinterpret_cast<u64>(rp);
    const StatePath path = DecideState(r);
    if (path == StatePath::Game) {
        g_set_targets(rp, targets, rect);
        return;
    }
    if (StateFlagsSet()) {
        g_state.fallback[kFbFlags].fetch_add(1, std::memory_order_relaxed);
        g_set_targets(rp, targets, rect);
        return;
    }
    u8* dcb = rp + L::kContextGfx;
    NativeState::StateBlock s;
    NativeState::LoadBlock(r, s);
    RegWrite writes[kMaxStateWrites];
    NativeState::Writer w{writes, kMaxStateWrites};
    NativeState::SetRenderTargets(s, targets, rect, g_state_env, w);
    if (path == StatePath::Verify) {
        VerifyState(kFnTargets, r, dcb, &s, w, [&] { g_set_targets(rp, targets, rect); });
        return;
    }
    if (!EmitState(dcb, w, kApply)) {
        g_set_targets(rp, targets, rect);
        return;
    }
    NativeState::StoreBlock(r, s);
    g_state.native[kFnTargets].fetch_add(1, std::memory_order_relaxed);
}

SSPC_SYSV_ABI void TransitionProbe(u8* rp, u64 block) {
    const u64 r = reinterpret_cast<u64>(rp);
    const StatePath path = DecideState(r);
    if (path == StatePath::Game) {
        g_transition(rp, block);
        return;
    }
    if (StateFlagsSet()) {
        g_state.fallback[kFbFlags].fetch_add(1, std::memory_order_relaxed);
        g_transition(rp, block);
        return;
    }
    u8* dcb = rp + L::kContextGfx;
    NativeState::StateBlock s;
    NativeState::LoadBlock(r, s);
    RegWrite writes[kMaxStateWrites];
    NativeState::Writer w{writes, kMaxStateWrites};
    NativeState::TransitionTo(s, block, g_state_env, w);
    if (path == StatePath::Verify) {
        VerifyState(kFnTransition, r, dcb, &s, w, [&] { g_transition(rp, block); });
        return;
    }
    if (!EmitState(dcb, w, kApply)) {
        g_transition(rp, block);
        return;
    }
    NativeState::StoreBlock(r, s);
    g_state.native[kFnTransition].fetch_add(1, std::memory_order_relaxed);
}

SSPC_SYSV_ABI void PrimTypeProbe(u8* dcb, u32 prim) {
    const u64 r = reinterpret_cast<u64>(dcb) - L::kContextGfx;
    const StatePath path = DecideState(r);
    if (path == StatePath::Game) {
        g_prim_type(dcb, prim);
        return;
    }
    RegWrite writes[1];
    NativeState::Writer w{writes, 1};
    NativeState::SetPrimitiveType(prim, w);
    if (path == StatePath::Verify) {
        VerifyState(kFnPrim, r, dcb, nullptr, w, [&] { g_prim_type(dcb, prim); });
    } else if (EmitState(dcb, w, kApply)) {
        g_state.native[kFnPrim].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_prim_type(dcb, prim);
    }
}

SSPC_SYSV_ABI void IndexSizeProbe(u8* dcb, u32 size, u32 policy) {
    const u64 r = reinterpret_cast<u64>(dcb) - L::kContextGfx;
    const StatePath path = DecideState(r);
    if (path == StatePath::Game) {
        g_index_size(dcb, size, policy);
        return;
    }
    RegWrite writes[1];
    NativeState::Writer w{writes, 1};
    NativeState::SetIndexSize(size, policy, w);
    if (path == StatePath::Verify) {
        VerifyState(kFnIndex, r, dcb, nullptr, w, [&] { g_index_size(dcb, size, policy); });
    } else if (EmitState(dcb, w, kApply)) {
        g_state.native[kFnIndex].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_index_size(dcb, size, policy);
    }
}

/// A one-packet Gnm state setter (NativeState::kLeaves[i]) on a render context's DCB.
template <typename Call>
void RunLeaf(u32 i, u8* dcb, u32 a, u32 b, Call&& game) {
    const u64 r = reinterpret_cast<u64>(dcb) - L::kContextGfx;
    const StatePath path = DecideState(r);
    if (path == StatePath::Game) {
        game();
        return;
    }
    RegWrite writes[2];
    NativeState::Writer w{writes, 2};
    NativeState::LeafWrite(NativeState::kLeaves[i], a, b, w);
    if (w.overflow) {
        g_state.fallback[kFbOverflow].fetch_add(1, std::memory_order_relaxed);
        game();
        return;
    }
    if (path == StatePath::Verify) {
        VerifyState(kFnLeaf, r, dcb, nullptr, w, game);
    } else if (EmitState(dcb, w, kApply)) {
        g_state.native[kFnLeaf].fetch_add(1, std::memory_order_relaxed);
        g_leaf_native[i].fetch_add(1, std::memory_order_relaxed);
    } else {
        game();
    }
}

using Leaf1Fn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 a);
using Leaf2Fn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 a, u32 b);
using LeafFFn = SSPC_SYSV_ABI void (*)(float x, float y, u8* dcb);

template <u32 I>
SSPC_SYSV_ABI void Leaf1Probe(u8* dcb, u32 a) {
    RunLeaf(I, dcb, a, 0, [&] { reinterpret_cast<Leaf1Fn>(g_leaf_game[I])(dcb, a); });
}
template <u32 I>
SSPC_SYSV_ABI void Leaf2Probe(u8* dcb, u32 a, u32 b) {
    RunLeaf(I, dcb, a, b, [&] { reinterpret_cast<Leaf2Fn>(g_leaf_game[I])(dcb, a, b); });
}
template <u32 I>
SSPC_SYSV_ABI void LeafFProbe(float x, float y, u8* dcb) {
    u32 a = 0, b = 0;
    std::memcpy(&a, &x, 4);
    std::memcpy(&b, &y, 4);
    RunLeaf(I, dcb, a, b, [&] { reinterpret_cast<LeafFFn>(g_leaf_game[I])(x, y, dcb); });
}

template <u32 I>
constexpr const void* LeafProbe() {
    constexpr NativeState::Leaf k = NativeState::kLeaves[I].kind;
    if constexpr (k == NativeState::Leaf::FloatPair) {
        return reinterpret_cast<const void*>(&LeafFProbe<I>);
    } else if constexpr (k == NativeState::Leaf::Blend || k == NativeState::Leaf::ColorControl ||
                         k == NativeState::Leaf::Generic) {
        return reinterpret_cast<const void*>(&Leaf2Probe<I>);
    } else {
        return reinterpret_cast<const void*>(&Leaf1Probe<I>);
    }
}
template <u32... I>
std::array<const void*, sizeof...(I)> LeafProbes(std::integer_sequence<u32, I...>) {
    return {LeafProbe<I>()...};
}

void ReportState() {
    const DrawCounters& c = Counters();
    const auto four = [](const std::atomic<u64>* a) {
        return fmt::format("{}/{}/{}/{}/{}", a[0].load(), a[1].load(), a[2].load(), a[3].load(), a[4].load());
    };
    std::string leaves;
    for (u32 i = 0; i < NativeState::kLeafCount; ++i) {
        if (const u64 n = g_leaf_native[i].load(); n != 0) {
            leaves += fmt::format(" {:#x}:{}", NativeState::kLeaves[i].rva, n);
        }
    }
    if (!leaves.empty()) {
        SSPC_LOG(Engine, Info, "native state setters (native calls):{}", leaves);
    }
    SSPC_LOG(Engine, Info,
             "native state (targets/transition/prim/index/setters): native {}, fallbacks (flags {}, overflow {}, no "
             "space {}) "
             "| verify: calls {}, state block mismatched {}, PM4 size mismatched {} (unchecked {}{}) | processor "
             "applied {} ({} writes), verified {}, matched {}, mismatched {}{}",
             four(g_state.native), g_state.fallback[kFbFlags].load(), g_state.fallback[kFbOverflow].load(),
             g_state.fallback[kFbNoSpace].load(), four(g_state.verified), four(g_state.block_mismatch),
             four(g_state.size_mismatch), g_state.size_unchecked.load(),
             g_state.first_size_fn.load() == 0xFFFFFFFF
                 ? std::string()
                 : fmt::format("; first: fn {} native {} game {}", g_state.first_size_fn.load(),
                               g_state.first_size_want.load(), g_state.first_size_have.load()),
             c.state_applied.load(), c.state_applied_writes.load(), c.state_verified.load(), c.state_matched.load(),
             c.state_mismatched.load(),
             c.state_first_bad_reg.load() == 0
                 ? std::string()
                 : fmt::format(" (first: reg {:#x} native {:#x} PM4 {:#x})", c.state_first_bad_reg.load(),
                               c.state_first_bad_want.load(), c.state_first_bad_have.load()));
}

/// P5.3: what the transport of the native operations costs, per frame of the last interval - the processor's markers
/// (or the direct path's run tokens and NOP descriptions) and its time in native operations (every 8th call timed).
void ReportTransport(double seconds, double tsc_per_ms) {
    struct Last {
        u64 flips = 0, markers = 0, runs = 0, described = 0, tsc = 0, calls = 0;
    };
    static Last last;
    const DrawCounters& c = Counters();
    DirectCounters& d = Direct();
    const Last now{Control().flips.load(), c.markers.load(), d.runs.load(), d.described.load(), d.native_tsc.load(),
                   d.native_calls.load()};
    const double frames = static_cast<double>(now.flips - last.flips);
    if (frames > 0 && tsc_per_ms > 0) {
        SSPC_LOG(Engine, Info,
                 "native transport per frame ({:.0f} flips in {:.0f} s): processor markers {:.0f}, run tokens {:.0f}, "
                 "descriptions in draw NOPs {:.0f}, native-operation calls {:.0f}, time {:.3f} ms",
                 frames, seconds, static_cast<double>(now.markers - last.markers) / frames,
                 static_cast<double>(now.runs - last.runs) / frames,
                 static_cast<double>(now.described - last.described) / frames,
                 static_cast<double>(now.calls - last.calls) / frames,
                 static_cast<double>(now.tsc - last.tsc) / tsc_per_ms / frames);
    }
    last = now;
    if (g_direct) {
        SSPC_LOG(Engine, Info,
                 "native direct path: engine described {}, undescribed {}, run tokens {}, stream operations {}, stream "
                 "full {}, shapes full {} | processor described {} (unknown shape {}), runs {} (stale {}), operations "
                 "{}, draws {}, register writes {}, state writes {}, blocks in use {}",
                 g_engine.direct_described.load(), g_engine.direct_undescribed.load(), g_engine.direct_runs.load(),
                 g_engine.direct_ops.load(), d.stream_full.load(), d.shapes_full.load(), d.described.load(),
                 d.described_bad.load(), d.runs.load(), d.runs_stale.load(), d.ops.load(), d.draws.load(),
                 d.register_writes.load(), d.state_writes.load(), d.blocks_in_use.load());
    }
}

void Reporter(u32 seconds) {
    static constexpr const char* kFields[kMismatchFields] = {"kind",    "index count", "index base", "VS regs",
                                                             "PS regs", "VS table",    "PS table"};
    auto t0 = std::chrono::steady_clock::now();
    u64 tsc0 = __rdtsc();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const auto t1 = std::chrono::steady_clock::now();
        const u64 tsc1 = __rdtsc();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ReportTransport(ms / 1000.0, ms > 0 ? static_cast<double>(tsc1 - tsc0) / ms : 0.0);
        t0 = t1;
        tsc0 = tsc1;
        DrawCounters& c = Counters();
        std::string fields;
        for (u32 i = 0; i < kMismatchFields; ++i) {
            if (const u64 n = c.field[i].load(); n != 0) {
                fields += fmt::format(" {} {}", kFields[i], n);
            }
        }
        std::string passes;
        for (u32 p = 0; p < kPassSlots; ++p) {
            if (const u64 n = c.pass_mismatched[p].load(); n != 0) {
                passes += fmt::format(" p{}:{}/{}", p, n, c.pass_verified[p].load());
            }
        }
        if (g_state_on) {
            ReportState();
        }
        if (g_apply_passes != 0) {
            SSPC_LOG(Engine, Info,
                     "native draw (apply): engine native draws {}, draw fallbacks {}, native commits {}, commit "
                     "fallbacks (stage mode {}, alt shader {}, missing {}) | processor applied draws {}, commits {}, "
                     "register writes {}",
                     g_engine.native_draws.load(), g_engine.draw_fallbacks.load(), g_engine.native_commits.load(),
                     g_engine.commit_fallbacks[1].load(), g_engine.commit_fallbacks[2].load(),
                     g_engine.commit_fallbacks[3].load(), c.applied_draws.load(), c.applied_commits.load(),
                     c.applied_registers.load());
        }
        SSPC_LOG(Engine, Info,
                 "native draw (shadow): engine draws {}, marked {}, foreign {}, no space {} | processor markers {}, "
                 "malformed {}, orphaned {}, verified {}, matched {}, mismatched {}{}{} shaders unknown {}, tables "
                 "checked {}, canaries caught {}/{}",
                 g_engine.draws.load(), g_engine.marked.load(), g_engine.foreign.load(), g_engine.no_space.load(),
                 c.markers.load(), c.malformed.load(), c.orphaned.load(), c.verified.load(), c.matched.load(),
                 c.mismatched.load(), fields.empty() ? "" : " (fields:" + fields + ")",
                 passes.empty() ? "" : " (mismatched/verified by pass slot:" + passes + ")", c.shaders_unknown.load(),
                 c.tables_checked.load(), c.canaries_caught.load(), c.canaries.load());
    }
}

std::string Env(const char* name) {
    char v[256] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

} // namespace

bool BuildDrawOp(u64 base, u64 r, bool indexed, u32 index_count, u64 index_base, DrawOp& op) {
    if (!ContextPass(base, r, op.pass, op.sub)) {
        return false;
    }
    const u64 cue = r + L::kContextCue;
    if (At<u64>(cue + L::kCueDcb) != r + L::kContextGfx) {
        return false;
    }
    op.indexed = indexed;
    op.index_count = index_count;
    op.index_base = indexed ? index_base : 0;

    const u64 vs = At<u64>(cue + kCueVsShader);
    const u64 ps = At<u64>(cue + kCuePsShader);
    op.shaders = vs != 0 && At<u8>(cue + kCueVsAltPath) == 0 && At<u8>(cue + kCuePsAltPath) == 0;
    if (op.shaders) {
        // sceGnmSetVsShader(cb, vs + 8, modifier) / sceGnmSetPsShader350(cb, ps + 8): the block's values as written.
        const auto* v = reinterpret_cast<const u32*>(vs + 8);
        const u32 modifier = At<u32>(cue + kCueVsModifier);
        const u32 rsrc1 = modifier == 0 ? v[2] : (v[2] & 0xFCFFFC3Fu) | modifier;
        const u32 vs_values[kVsRegs] = {v[0], 0u, rsrc1, v[3], v[4], v[5], v[6]};
        std::memcpy(op.vs, vs_values, sizeof(op.vs));
        op.has_ps = ps != 0;
        if (op.has_ps) {
            const auto* p = reinterpret_cast<const u32*>(ps + 8);
            const u32 ps_values[kPsRegs] = {p[0], 0u, p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]};
            std::memcpy(op.ps, ps_values, sizeof(op.ps));
        }
    }
    DescribeTable(cue, kCueStageVs, true, op.table[kStageVs]);
    DescribeTable(cue, kCueStagePs, false, op.table[kStagePs]);
    return true;
}

namespace {

// CUE fields of the commit's VS/PS part (0x14ad0, after its ES/GS and LS/HS parts).
constexpr u64 kCueModeFlags = 0xC1E0;    ///< u32: 0x20 ES/GS mode, 0x1 LS/HS mode
constexpr u64 kCueChangedBits = 0xC1E4;  ///< u16: |= 4 VS set, |= 2 PS set
constexpr u64 kCuePsDirty = 0xC1C9;      ///< PS shader to set
constexpr u64 kCueVsDirty = 0xC1CA;      ///< VS shader to set
constexpr u64 kCuePsTableDirty = 0xC1D1; ///< PS resources changed: new snapshot + user data
constexpr u64 kCueVsTableDirty = 0xC1D2;
constexpr u64 kCuePsInputCache = 0xC1C0; ///< u32*: precomputed SPI_PS_INPUT_CNTL values, or 0
constexpr u64 kCueGlobalTable = 0x78;    ///< u64 pointer every stage may get (layout byte 0xC)
constexpr u32 kUserDataPs = 0x2C0C;      ///< SPI_SHADER_USER_DATA_PS_0 (table 0xaf0070)
constexpr u32 kUserDataVs = 0x2C4C;
constexpr u32 kSpiPsInputCntl0 = 0xA191;

struct Writer {
    RegWrite* out;
    u32 n = 0;
    void Reg(u32 reg, u32 value) {
        if (n < kMaxRegWrites) {
            out[n++] = RegWrite{reg, value};
        }
    }
    void Pointer(u32 user_data, u8 sgpr, u64 value) { // setPointerInUserData 0x626400
        Reg(user_data + sgpr, static_cast<u32>(value));
        Reg(user_data + sgpr + 1, static_cast<u32>(value >> 32));
    }
};

/// One stage's part of the commit: snapshot of the shadow, then the layout's user-data slots (0x14ad0, in its order).
void CommitStage(u64 cue, u32 stage, u32 user_data, const CommitEnv& env, Writer& w) {
    const u64 layout = At<u64>(cue + L::kCueLayout + u64{stage} * 8);
    const u32 dwords = At<u16>(layout);
    const u64 snapshot = env.alloc(cue, dwords);
    std::memcpy(reinterpret_cast<void*>(snapshot),
                reinterpret_cast<const void*>(cue + L::kCueShadow + u64{stage} * L::kCueShadowStride), u64{dwords} * 4);
    const auto byte = [&](u32 at) { return At<u8>(layout + at); };
    const auto table = [&](u32 sgpr_at, u32 offset_at) {
        if (const u8 sgpr = byte(sgpr_at); sgpr != 0xFF) {
            w.Pointer(user_data, sgpr, snapshot + (offset_at == 0 ? 0 : u64{At<u16>(layout + offset_at)} * 4));
        }
    };
    table(7, 0);
    table(8, 0x16);
    table(9, 0x1A);
    table(0xA, 0x1C);
    table(0xB, 0x1E);
    if (const u8 sgpr = byte(0xC); sgpr != 0xFF) {
        w.Pointer(user_data, sgpr, At<u64>(cue + kCueGlobalTable));
    }
    const bool vertex = stage == kCueStageVs;
    if (const u8 sgpr = byte(0xD); sgpr != 0xFF) { // setUserDataRegion(.., 1): one dword
        w.Reg(user_data + sgpr, At<u32>(cue + (vertex ? 0xC0E8 : 0xC0E4)));
    }
    if (const u8 sgpr = byte(0xE); sgpr != 0xFF) {
        w.Reg(user_data + sgpr, At<u32>(cue + (vertex ? 0xC108 : 0xC104)));
    }
    if (!vertex) {
        return;
    }
    if (const u8 sgpr = byte(4); sgpr != 0xFF) {
        w.Pointer(user_data, sgpr, At<u64>(cue + 0xC130));
    }
    table(5, 0x18);
    table(6, 0x20);
    if (const u8 sgpr = byte(0xF); sgpr != 0xFF) {
        const u32 v = At<u32>(cue + 0xC6E8);
        w.Reg(user_data + sgpr, (v >> 16) * (v & 0xFFFF));
    }
}

} // namespace

CommitCheck CheckNativeCommit(u64 cue) {
    if ((At<u32>(cue + kCueModeFlags) & 0x21) != 0) {
        return CommitCheck::StageMode;
    }
    if (At<u8>(cue + kCuePsAltPath) != 0 || At<u8>(cue + kCueVsAltPath) != 0) {
        return CommitCheck::AltShader;
    }
    if ((At<u8>(cue + kCueVsDirty) != 0 && At<u64>(cue + kCueVsShader) == 0) ||
        (At<u8>(cue + kCueVsTableDirty) != 0 && At<u64>(cue + L::kCueLayout + u64{kCueStageVs} * 8) == 0) ||
        (At<u8>(cue + kCuePsTableDirty) != 0 && At<u64>(cue + L::kCueLayout + u64{kCueStagePs} * 8) == 0)) {
        return CommitCheck::Missing;
    }
    return CommitCheck::Native;
}

u32 NativeCommit(u64 cue, const CommitEnv& env, RegWrite* out) {
    Writer w{out};
    const bool vs_dirty = At<u8>(cue + kCueVsDirty) != 0;
    const bool ps_dirty = At<u8>(cue + kCuePsDirty) != 0;
    const u64 vs = At<u64>(cue + kCueVsShader);
    const u64 ps = At<u64>(cue + kCuePsShader);
    u16 changed = At<u16>(cue + kCueChangedBits);
    if (vs_dirty) { // sceGnmSetVsShader(dcb, vs + 8, modifier) -> gnm_driver SetVsRegs
        changed |= 4;
        const auto* v = reinterpret_cast<const u32*>(vs + 8);
        const u32 modifier = At<u32>(cue + kCueVsModifier);
        w.Reg(0x2C48, v[0]);
        w.Reg(0x2C49, 0);
        w.Reg(0x2C4A, modifier == 0 ? v[2] : (v[2] & 0xFCFFFC3Fu) | modifier);
        w.Reg(0x2C4B, v[3]);
        w.Reg(0xA207, v[6]);
        w.Reg(0xA1B1, v[4]);
        w.Reg(0xA1C3, v[5]);
    }
    if (ps_dirty) { // sceGnmSetPsShader350(dcb, ps ? ps + 8 : null) -> SetPsRegs / the no-PS form
        changed |= 2;
        if (ps == 0) {
            w.Reg(0x2C08, 0);
            w.Reg(0x2C09, 0);
            w.Reg(0xA203, 0);
            w.Reg(0xA08F, 0xF);
        } else {
            const auto* p = reinterpret_cast<const u32*>(ps + 8);
            w.Reg(0x2C08, p[0]);
            w.Reg(0x2C09, 0);
            w.Reg(0x2C0A, p[2]);
            w.Reg(0x2C0B, p[3]);
            w.Reg(0xA1C4, p[4]);
            w.Reg(0xA1C5, p[5]);
            w.Reg(0xA1B3, p[6]);
            w.Reg(0xA1B4, p[7]);
            w.Reg(0xA1B6, p[8]);
            w.Reg(0xA1B8, p[9]);
            w.Reg(0xA203, p[10]);
            w.Reg(0xA08F, p[11]);
        }
    }
    std::memcpy(reinterpret_cast<void*>(cue + kCueChangedBits), &changed, sizeof(changed));
    if (At<u8>(cue + kCueVsTableDirty) != 0) {
        CommitStage(cue, kCueStageVs, kUserDataVs, env, w);
    }
    if (At<u8>(cue + kCuePsTableDirty) != 0) {
        CommitStage(cue, kCueStagePs, kUserDataPs, env, w);
    }
    if ((vs_dirty || ps_dirty) && vs != 0 && ps != 0) {
        const u32 n = At<u8>(ps + 0x38); // (char)ps[0xe]: PS inputs
        if (n != 0) {
            u32 local[32] = {};
            const u32* values = reinterpret_cast<const u32*>(At<u64>(cue + kCuePsInputCache));
            if (values == nullptr) {
                const u32 vs9 = At<u32>(vs + 9 * 4);
                const u64 vs_semantics = vs + 4 * (u64{At<u8>(vs + 3)} + (vs9 & 0xFF) + 10);
                const u64 ps_semantics = ps + 4 * (u64{At<u8>(ps + 3)} + 0xF);
                env.ps_inputs(local, vs_semantics, (vs9 >> 8) & 0xFF, ps_semantics, static_cast<s32>(n));
                values = local;
            }
            for (u32 i = 0; i < std::min(n, 32u); ++i) {
                w.Reg(kSpiPsInputCntl0 + i, values[i]);
            }
        }
    }
    for (const u64 flag : {kCuePsTableDirty, kCuePsDirty, kCuePsAltPath, kCueVsTableDirty, kCueVsDirty, kCueVsAltPath}) {
        *reinterpret_cast<u8*>(cue + flag) = 0;
    }
    return w.n;
}

std::string ApplyRenderMode(std::string mode, bool native_backend) {
    if (mode.empty()) {
        mode = Env("SSPC_RENDER_MODE");
    }
    for (char& c : mode) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (mode == "native") {
        mode = "native_experimental";
    }
    if (mode.empty() || mode == "legacy") {
        return "legacy";
    }
    if (mode != "hybrid" && mode != "native_experimental") {
        SSPC_LOG(Engine, Error, "render mode '{}': want legacy, hybrid or native_experimental - legacy runs", mode);
        return "legacy";
    }
    if (!native_backend) {
        SSPC_LOG(Engine, Error,
                 "render mode {}: needs --gpu-backend sspc (the bridge's renderer has no native path) - legacy runs",
                 mode);
        return "legacy";
    }
    const bool hybrid = mode == "hybrid";
    const std::pair<const char*, const char*> preset[] = {
        {"SSPC_P53_NATIVE_DRAW", hybrid ? "apply:1,9" : "apply:all"},
        {"SSPC_P53_NATIVE_STATE", "apply"},
        {"SSPC_P53_NATIVE_VERIFY", "0"},
        {"SSPC_P53_NATIVE_DESCRIBE", "1"},
        {"SSPC_P53_NATIVE_FRONTEND", "all"},
    };
    std::string set, kept;
    for (const auto& [name, value] : preset) {
        if (Env(name).empty()) {
            SetEnvironmentVariableA(name, value);
            set += fmt::format(" {}={}", name, value);
        } else {
            kept += fmt::format(" {}={}", name, Env(name));
        }
    }
    SSPC_LOG(Engine, Warning, "EXPERIMENT: render mode {} -{}{}", mode, set.empty() ? "" : " set:" + set,
             kept.empty() ? "" : "; given explicitly:" + kept);
    return mode;
}

bool InstallFromEnvironment(u64 eboot_base, bool native_backend) {
    const std::string mode = Env("SSPC_P53_NATIVE_DRAW");
    if (mode.empty() || mode == "0" || mode == "off") {
        return true;
    }
    if (mode.rfind("apply:", 0) == 0) {
        if (mode == "apply:all") {
            g_apply_passes = (u64{1} << 41) - 1; // P5.3 NATIVE_EXPERIMENTAL: every pass 0..40
        }
        for (size_t at = 6; mode != "apply:all" && at < mode.size();) {
            char* end = nullptr;
            const unsigned long pass = std::strtoul(mode.c_str() + at, &end, 10);
            if (end == mode.c_str() + at || pass > 40) {
                SSPC_LOG(Engine, Error,
                         "SSPC_P53_NATIVE_DRAW={}: want apply:<pass 0..40>[,<pass>...] or apply:all; nothing installed",
                         mode);
                return false;
            }
            g_apply_passes |= u64{1} << pass;
            at = static_cast<size_t>(end - mode.c_str()) + (*end == ',' ? 1 : 0);
        }
        if (!native_backend) {
            SSPC_LOG(Engine, Error,
                     "SSPC_P53_NATIVE_DRAW={}: apply needs --gpu-backend sspc (the bridge does not execute native "
                     "operations); shadow mode instead",
                     mode);
            g_apply_passes = 0;
        }
    } else if (mode != "shadow") {
        SSPC_LOG(Engine, Error, "SSPC_P53_NATIVE_DRAW={}: want shadow or apply:<passes>; nothing installed", mode);
        return false;
    }
    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "native draw not installed: {}", error);
            return false;
        }
    }
    for (const ProbeSite* s : {&kDrawIndexSite, &kDrawAutoSite}) {
        if (!VerifyGuestFunction(eboot_base, s->function, error)) {
            SSPC_LOG(Engine, Error, "native draw not installed: {}", error);
            return false;
        }
    }
    g_base = eboot_base;
    void* index = InstallEntryProbe(eboot_base, kDrawIndexSite, reinterpret_cast<const void*>(&DrawIndexProbe), error);
    void* autod = index ? InstallEntryProbe(eboot_base, kDrawAutoSite, reinterpret_cast<const void*>(&DrawAutoProbe),
                                            error)
                        : nullptr;
    if (index == nullptr || autod == nullptr) {
        SSPC_LOG(Engine, Critical, "native draw: {}", error);
        std::abort(); // verified bytes, but the host refused the write: stop rather than run half-probed
    }
    g_draw_index = reinterpret_cast<DrawIndexFn>(index);
    g_draw_auto = reinterpret_cast<DrawAutoFn>(autod);
    if (g_apply_passes != 0) {
        g_env.alloc = reinterpret_cast<AllocFn>(eboot_base + 0x123d0);
        g_env.ps_inputs = reinterpret_cast<PsInputsFn>(eboot_base + 0x638540);
        void* commit = InstallEntryProbe(eboot_base, kCommitSite, reinterpret_cast<const void*>(&CommitProbe), error);
        if (commit == nullptr) {
            SSPC_LOG(Engine, Critical, "native draw: {}", error);
            std::abort();
        }
        g_commit = reinterpret_cast<CommitFn>(commit);
        std::string passes;
        for (u32 p = 0; p <= 40; ++p) {
            if (((g_apply_passes >> p) & 1) != 0) {
                passes += fmt::format(" {}", p);
            }
        }
        SSPC_LOG(Engine, Warning,
                 "EXPERIMENT: SSPC_P53_NATIVE_DRAW={} - passes{}: CUE commit 0x14ad0 and draws run natively (register "
                 "writes and draws handed to the command processor, no PM4 of their own); other passes in shadow mode",
                 mode, passes);
    }
    if (const std::string state = Env("SSPC_P53_NATIVE_STATE"); !state.empty() && state != "0" && state != "off") {
        if (state != "verify" && state != "apply") {
            SSPC_LOG(Engine, Error, "SSPC_P53_NATIVE_STATE={}: want verify or apply; native state not installed", state);
        } else {
            g_state_apply = state == "apply" && g_apply_passes != 0;
            if (state == "apply" && !g_state_apply) {
                SSPC_LOG(Engine, Error,
                         "SSPC_P53_NATIVE_STATE=apply needs SSPC_P53_NATIVE_DRAW=apply:<passes> (and the native "
                         "backend); verify instead");
            }
            bool ok = true;
            for (const GuestFunction& f : kStateAssumed) {
                ok = ok && VerifyGuestFunction(eboot_base, f, error);
            }
            for (const ProbeSite* s : {&kSetTargetsSite, &kTransitionSite, &kPrimTypeSite, &kIndexSizeSite}) {
                ok = ok && VerifyGuestFunction(eboot_base, s->function, error);
            }
            for (const NativeState::LeafSpec& s : NativeState::kLeaves) {
                ok = ok && VerifyGuestFunction(eboot_base, GuestFunction{s.rva, s.size, s.hash}, error);
            }
            if (!ok) {
                SSPC_LOG(Engine, Error, "native state not installed: {}", error);
            } else {
                g_state_env.rt_base = reinterpret_cast<NativeState::TargetFn>(eboot_base + 0x637bf0);
                g_state_env.rt_fmask_base = reinterpret_cast<NativeState::TargetFn>(eboot_base + 0x637f30);
                g_state_env.rt_set_fmask = reinterpret_cast<NativeState::SetFmaskFn>(eboot_base + 0x637e30);
                g_state_env.dt_z_base = reinterpret_cast<NativeState::TargetFn>(eboot_base + 0x621450);
                g_state_env.dt_htile = reinterpret_cast<NativeState::TargetFn>(eboot_base + 0x6219e0);
                g_state_env.db_reg_2de = At<u32>(eboot_base + kDbReg2deTable + 3 * 4);
                void* t = InstallEntryProbe(eboot_base, kSetTargetsSite, reinterpret_cast<const void*>(&SetTargetsProbe),
                                            error);
                void* x = t ? InstallEntryProbe(eboot_base, kTransitionSite,
                                                reinterpret_cast<const void*>(&TransitionProbe), error)
                            : nullptr;
                void* p = x ? InstallEntryProbe(eboot_base, kPrimTypeSite, reinterpret_cast<const void*>(&PrimTypeProbe),
                                                error)
                            : nullptr;
                void* i = p ? InstallEntryProbe(eboot_base, kIndexSizeSite,
                                                reinterpret_cast<const void*>(&IndexSizeProbe), error)
                            : nullptr;
                const auto leaf_probes =
                    LeafProbes(std::make_integer_sequence<u32, NativeState::kLeafCount>{});
                for (u32 k = 0; i != nullptr && k < NativeState::kLeafCount; ++k) {
                    const NativeState::LeafSpec& s = NativeState::kLeaves[k];
                    const ProbeSite site{{s.rva, s.size, s.hash}, s.prologue};
                    g_leaf_game[k] = InstallEntryProbe(eboot_base, site, leaf_probes[k], error);
                    if (g_leaf_game[k] == nullptr) {
                        i = nullptr;
                    }
                }
                if (i == nullptr) {
                    SSPC_LOG(Engine, Critical, "native state: {}", error);
                    std::abort(); // verified bytes, but the host refused the write: stop rather than run half-probed
                }
                g_set_targets = reinterpret_cast<SetTargetsFn>(t);
                g_transition = reinterpret_cast<TransitionFn>(x);
                g_prim_type = reinterpret_cast<PrimTypeFn>(p);
                g_index_size = reinterpret_cast<IndexSizeFn>(i);
                g_state_on = true;
                SSPC_LOG(Engine, Warning,
                         "EXPERIMENT: SSPC_P53_NATIVE_STATE={} - SetRenderTargets 0x11f910, TransitionTo 0x120020, "
                         "primitive type 0x623740, index size 0x624f80 and {} one-packet state setters (blend, "
                         "depth/stencil, rasterizer, depth bias, instances): {}",
                         state, NativeState::kLeafCount,
                         g_state_apply ? "native in the apply passes (state markers, no PM4 of their own), verified "
                                         "against the game's PM4 elsewhere"
                                       : "the game's PM4, verified against the native result");
            }
        }
    }
    if (Env("SSPC_P53_NATIVE_VERIFY") == "0") {
        g_verify = false;
        g_state_verify = false;
        SSPC_LOG(Engine, Warning,
                 "SSPC_P53_NATIVE_VERIFY=0: native operations without their checks (no shadow markers in the other "
                 "passes, no comparison of native draws or state) - for measurements; correctness runs keep it on");
    }
    if (Env("SSPC_P53_NATIVE_DESCRIBE") == "1") {
        g_describe = true;
        SSPC_LOG(Engine, Warning, "SSPC_P53_NATIVE_DESCRIBE=1: the other passes' draws carry the engine's description "
                                  "(kDescribe markers) for the Native Render Frontend; the game's draw packet follows");
    }
    if (const std::string direct = Env("SSPC_P53_DIRECT"); direct == "1") {
        if (!native_backend) {
            SSPC_LOG(Engine, Error, "SSPC_P53_DIRECT=1 needs --gpu-backend sspc (its processor runs the native stream); "
                                    "the markers instead");
        } else {
            g_direct = true;
            SetDirect(true);
            SSPC_LOG(Engine, Warning,
                     "EXPERIMENT: SSPC_P53_DIRECT=1 - no markers on the hot path: the apply passes' commits, state and "
                     "draws go to the native stream (one 4-dword run token in the DCB per run of operations), the other "
                     "draws' descriptions into the NOP after their draw packet{}",
                     g_verify ? "; verification markers kept (SSPC_P53_NATIVE_VERIFY)" : "");
        }
    }
    if (const std::string ab = Env("SSPC_P53_NATIVE_AB"); !ab.empty() && g_apply_passes != 0) {
        Control().ab_frames = static_cast<u32>(std::strtoul(ab.c_str(), nullptr, 10));
        SSPC_LOG(Engine, Warning,
                 "EXPERIMENT: SSPC_P53_NATIVE_AB={} - blocks of {} flips alternate: the game's PM4 path (verified), then "
                 "the native path",
                 ab, Control().ab_frames);
    }
    if (const std::string s = Env("SSPC_P53_NATIVE_DRAW_CANARY"); !s.empty()) {
        g_canary = std::strtoull(s.c_str(), nullptr, 10);
    }
    u32 stats = 10;
    if (const std::string s = Env("SSPC_P53_NATIVE_DRAW_STATS"); !s.empty()) {
        stats = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
    }
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_P53_NATIVE_DRAW=shadow - the engine's draws (0x623c60, 0x623a40) are described from its "
             "objects and verified by the command processor against the PM4 path (kVerify markers in the DCB)");
    if (stats != 0) {
        std::thread(Reporter, stats).detach();
    }
    return true;
}

} // namespace SecondSon::Engine::Render::NativeDraw
