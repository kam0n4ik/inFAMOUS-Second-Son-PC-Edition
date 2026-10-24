// SecondSonPC — reconstructions of the mesh pass's two draw functions, eboot 0x1a1770 and 0x1a1960 (R6-G;
// docs/NATIVE_RENDER_R6_GBUFFER.md). No symbols in the eboot; our names. Analysis: docs/NATIVE_RECOMP_R5_E_GBUFFER.md.
//
// 0x1a1770 "GBuffer::InstancedDraw" (rdi material M, rsi context R, rdx instance records, ecx n, r8 mesh) [A]:
//   M+0x1c (u16) low byte != 0: v = lo << 16 | (hi | mesh+0x1d) << 24 | 0xff00; v != u32 R+0xde64:
//       bytes R+0xde64.. = {0, 0xff, lo, hi | mesh+0x1d}; 0x6229a0(R+8, v)       DB_STENCILREFMASK (SET_CONTEXT_REG)
//   0x120280(R)                                                                  draw constants (when dirty)
//   M+0x135 != 0xff: V# = 0x45f590(eboot+0x7dff738, ~(mesh+0x18 >> 27) & 1)   (RAX:RDX; for a table entry 0 it
//       returns the stack guard and the caller's RDX - not reproducible: such a call runs the original)
//       M+0x13c != 0xff: 0x16b70(R+0x128, 2, M+0x13c, 1, &V#)
//   V# = 0; 0x61e260(&V#, records, 0xd4, n); 0x16b70(R+0x128, 0x27530(0), M+0x127, 1, &V#)   instance records' V#
//   0x16aa0(R+0x128, 2, 0, mesh+0x6c (u8), mesh+0x60)                              vertex streams
//   0x6250b0(R+8, n); 0x623740(R+8, 4); 0x624f80(R+8, 0, 2)                       NUM_INSTANCES, primitive, index type
//   u8 eboot+0x1a0cbd5 != 0: 0x628ae0(R+8, mesh+0x58 << 16)
//   count = mesh+0x68, indices = mesh+0x58 (read before the commit); 0x14ad0(R+0x128); 0x623c60(R+8, count, indices, 0)
//
// 0x1a1960 "GBuffer::MeshDraw" (rdi material M, rsi context R, rdx instance I (0x80-byte object record)) [A]:
//   mesh = **(I+0x18); skin = I+0x20; n = mesh+0x6c (u8)
//   skin && (u8 skin & 7): the n stream V#s copied to the stack (memmove n*16 into 0x200 bytes) and replaced by the
//       skin's V#s skin+0x28 / +0x38 / +0x48 at s8 indices k0, k1, k2 = *(*(skin+8)+0x58) [0..2] (k1, k2 < 0: kept);
//       table = the copy; else table = mesh+0x60
//   0x16aa0(R+0x128, 2, 0, n, table)
//   M+0xd8 != 0xff: 0x16b70(R+0x128, 2, M+0xd8, 1, skin ? skin+0x58 : *(GOT 0xd9dec8)+0x1f6c)
//   (*(M+0xf0))+0xf & 2: 0x16aa0(R+0x128, 2, (s8) (*(*(M+0x10)+0x58))+0x22, 1,
//                                skin && skin+0x20 ? skin+0xc0 : *(GOT 0xd9dec8)+0x1f6c)
//   0x623740(R+8, 4); 0x624f80(R+8, 0, 2); [0x628ae0 as above]; 0x14ad0(R+0x128); 0x623c60(R+8, mesh+0x68, mesh+0x58, 0)
//
// Writes of their own: 0x1a1770 the four stencil bytes R+0xde64..0xde67; 0x1a1960 only its stack. Everything else is
// calls. The stack guard (GOT 0xd9de00) is read and checked as in any function with a stack buffer.
//
// Implementations: the hand-written one below (Native), or the recompiled recomp_1a1770 / recomp_1a1960
// (SSPC_R6G_IMPL=recomp with SSPC_NATIVE_ENGINE_RECOMP). A call the hand-written one does not follow (more than 32
// streams, a skin index outside the copy, an empty vertex-format table entry) runs the original.
//
// Verification (gbuffer_trace.h): the original as a clone whose recorded callees go to the trace thunks, the
// implementation with the same thunks, both on a shadow context (the stencil bytes copied in); the traces and the
// stencil bytes are compared, then the original runs (verify) - or the implementation (guarded).
//
// Measurement (SSPC_R6G_STATS=1, docs/NATIVE_RENDER_R6_GBUFFER.md §G2): the hand-written path also counts per call
// what it does and what the CUE commits (which stages are dirty, how large their tables are, whether a committed
// table equals the previous one) and times the call, the commit and the draw; a reporter logs it every 10 s.
#pragma once

#include <array>
#include <string>

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kGBufferInstancedDrawSpec; ///< 0x1a1770, key gbuffer_instanced_draw
extern const FunctionSpec kGBufferMeshDrawSpec;      ///< 0x1a1960, key gbuffer_mesh_draw

namespace GBufferDraw {

using InstancedFn = SSPC_SYSV_ABI void (*)(u64 material, u64 r, u64 records, u64 n, u64 mesh);
using MeshFn = SSPC_SYSV_ABI void (*)(u64 material, u64 r, u64 instance);
using RedirectsFn = int (*)(const u64* from, const u64* to, u32 n); ///< recomp_set_redirects

inline constexpr u64 kCue = 0x128;          ///< R + 0x128: the context's CUE
inline constexpr u64 kGfx = 0x8;            ///< R + 8: the context's Gnm DCB
inline constexpr u64 kStencilState = 0xde64; ///< R: the last stencil value written (u32)
inline constexpr u64 kShadowBytes = 0xde80; ///< a shadow context (what the functions address in R, rounded up)
inline constexpr u32 kMaxStreams = 32;      ///< 0x1a1960's stack copy (0x200 bytes)

/// The callees in the order of the trace's callee table.
enum Callee : u32 {
    kStencil = 0,   ///< 0x6229a0 (gfx, value)
    kDrawConstants, ///< 0x120280 (R)
    kSetBuffers,    ///< 0x16b70 (cue, stage, slot, count, src)
    kSetVertex,     ///< 0x16aa0 (cue, stage, slot, count, src)
    kNumInstances,  ///< 0x6250b0 (gfx, n)
    kPrimitive,     ///< 0x623740 (gfx, type)
    kIndexType,     ///< 0x624f80 (gfx, a, b)
    kDebugMarker,   ///< 0x628ae0 (gfx, value)
    kCommit,        ///< 0x14ad0 (cue)
    kDrawIndex,     ///< 0x623c60 (gfx, count, indices, flags)
    kStackFail,     ///< 0x984360 __stack_chk_fail
    kMemmove,       ///< 0x984430 memmove (not recorded: performed)
    kVsharp,        ///< 0x45f590 (table, which) -> a V# in RAX:RDX (pure: recorded, returns a fixed sequence)
    kCallees
};
extern const GTrace::CalleeSpec kCalleeSpecs[kCallees];

/// The hand-written reconstructions (System V; counted by the dispatcher).
SSPC_SYSV_ABI void InstancedNative(u64 material, u64 r, u64 records, u64 n, u64 mesh);
SSPC_SYSV_ABI void MeshNative(u64 material, u64 r, u64 instance);

// --- the Native Engine G-buffer path (SSPC_NATIVE_GBUFFER): the engine's descriptors of a draw ------------------
//
// Describe() reads the descriptors a 0x1a1770 draw binds straight from the engine's source structures - the instance
// records' V# built by 0x61e260 from (records, n), the vertex-stream V#s from mesh+0x60, the optional vertex-format V#
// from 0x45f590, the per-draw constant buffer the context holds at R+0xde20+8 - and the CUE API slot and user-data
// layout offset each goes to. It runs the verified Instanced path with recording setters: the pure callees (0x61e260,
// 0x45f590, 0x27530) run for real, the CUE setters and the draw are recorded, not performed. No SRT walk, no reading
// of the committed register snapshot: this is the engine's own data, the point of the native path. The material's PS
// textures/samplers/constants (0x448640) are the stable, per-material part and are not captured here (material_bind.h;
// the command processor's cache holds them per material - docs/NATIVE_GBUFFER_DRAW_0x1a1770.md).
struct DescResource {
    u8 stage = 0;     ///< the CUE stage (VS = 2, PS = 1, CS = 0; from 0x27530)
    u8 kind = 0;      ///< 0 a buffer (SetBuffers), 1 a vertex stream (SetVertexBuffers), 2 the draw constant buffer
    u8 slot = 0;      ///< the CUE API slot it was set at
    u8 direct = 0;    ///< the layout entry had bit 15 (a direct SET_SH_REG user-data region, not a shadow offset)
    u16 offset = 0xffff; ///< the stage's user-data dword offset the layout maps the slot to (entry & 0x3fff); 0xffff: the slot is unused by the shader
    u16 pad = 0;
    std::array<u32, 4> desc{}; ///< the descriptor (a V#: 4 dwords of 16 bytes)
};
inline constexpr u32 kMaxDescResources = 40; ///< 1 vformat + 1 instance + up to kMaxStreams vertex + 2 constants

struct DrawDescription {
    bool followed = false; ///< Describe reproduced the draw (false: a case it does not follow - the full path is used)
    u32 n = 0;             ///< instance count (0x1a1770's `n`)
    u32 index_count = 0;   ///< mesh + 0x68
    u64 index_base = 0;    ///< mesh + 0x58
    u32 count = 0;         ///< DescResource entries filled
    std::array<DescResource, kMaxDescResources> res{};
};

/// Fill `out` with the engine descriptors of a 0x1a1770 draw (its source structures, through the Instanced formulas),
/// with recording setters and no draw. Needs Prepare() or EnsurePureCallees(); false when they are not ready. Call
/// after the original draw ran (the context's stencil and constant-buffer state are then already the draw's).
bool Describe(u64 material, u64 r, u64 records, u64 n, u64 mesh, DrawDescription& out);

/// A stage's constant-update-engine state at the draw's commit. 0x14ad0 (the CUE commit, docs §9) copies a dirty
/// stage's shadow - cue+0x90+stage*0x1800, its layout's first L[0] dwords (L = [cue+0xc180+stage*8]) - whole to new
/// frame memory and points the stage's user-data registers into that copy: register L[7] at +0, L[8]/L[9] at
/// L16[0x16]/L16[0x1a], L[10]/L[11] at L16[0x1c]/L16[0x1e], L[5] at L16[0x18], L[6] at L16[0x20] (dwords; 0xff: no
/// register). So what the shader's SRT walk reads through those registers is this shadow, which Capture takes after
/// the engine's draw (the commit ran; nothing writes the shadow between the commit and the draw). Registers given
/// other memory - L[12] (cue+0x78), L[4] (cue+0xc120+stage*8) - and the dwords L[13]/L[14]/L[15] are not in it.
inline constexpr u32 kMaxShadowDwords = 512;
inline constexpr u32 kShadowPointers = 7;
/// (N: the copy's capacity in dwords - StageShadow's 512 for the G-buffer ring, CompactStageShadow's for R10's)
template <u32 N>
struct StageShadowOf {
    static constexpr u32 kCapacity = N;
    u64 layout = 0;   ///< L: the stage's layout (the CUE's per-shader user-data layout)
    u16 dwords = 0;   ///< L[0]: the dwords the commit copies (0: an empty layout - its registers are direct entries)
    bool captured = false; ///< false: no layout, or more than N dwords
    std::array<u8, kShadowPointers> reg{};   ///< the user-data register given the copy + off[i] dwords (0xff: none)
    std::array<u16, kShadowPointers> off{};
    std::array<u32, N> shadow{}; ///< [0, dwords): the copy the stage's registers point into
};
using StageShadow = StageShadowOf<kMaxShadowDwords>;
/// R10 (SSPC_R10_NATIVE_DRAWS, engine/render/draw_runs.h): the shadow casters' and mesh draws' CUE state, in a ring of
/// smaller slots (a layout of more dwords is not captured: those draws take the full path).
inline constexpr u32 kCompactShadowDwords = 224; // (R10: the most seen in the city 216)
using CompactStageShadow = StageShadowOf<kCompactShadowDwords>;
/// Capture `stage`'s (VS 2, PS 1) CUE state of context `r` into `out`; false when it has no layout or too many dwords.
bool CaptureStage(u64 r, u32 stage, StageShadow& out);
bool CaptureStage(u64 r, u32 stage, CompactStageShadow& out);
/// CaptureStage so far: [0] no layout (refused), [1] an empty layout (captured: nothing copied), [2] more than
/// kMaxShadowDwords (refused); [3] the most dwords a layout had. (The compact captures: CompactCaptureRefusals.)
std::array<u64, 4> CaptureRefusals();
std::array<u64, 4> CompactCaptureRefusals();

/// Set the module base and the pure callees Describe needs (0x45f590, 0x61e260, 0x27530) if Prepare() has not already;
/// a lighter setup than Prepare for the Native Engine G-buffer path, which uses no recording clones.
void EnsurePureCallees(u64 module_base);

/// For tests and the install: callee addresses at module_base, the recording clones, the recompiled implementations
/// when selected. False with `error` when a clone cannot be made.
bool Prepare(u64 module_base, std::string& error);
InstancedFn RecordingInstanced();
MeshFn RecordingMesh();
/// The recompiled implementations (nullptr: the hand-written ones).
void UseRecompiled(InstancedFn instanced, MeshFn mesh, RedirectsFn redirects);

/// One comparison of the original (recording clone) with the implementation on the calling thread: "" when equal,
/// else the first difference; `skipped` when the call cannot be compared (a case the implementation does not follow,
/// a trace overflow).
std::string CheckInstanced(u64 material, u64 r, u64 records, u64 n, u64 mesh, bool& skipped);
std::string CheckMesh(u64 material, u64 r, u64 instance, bool& skipped);

/// Generated cases, as the install's self-check (also the offline tests): "" or the first failure.
std::string SelfCheckInstanced(u32 cases, u64 seed);
std::string SelfCheckMesh(u32 cases, u64 seed);

/// The GOT slots the functions read, while a check runs in the loader's window or on an unrelocated image (before
/// relocation they are 0 - cue_commit.cpp): the stack guard (-> a guard value), the defaults' global (-> a zeroed
/// block), the vertex-format table's two entries (-> 1). Restored on destruction. Only while nothing else runs the
/// functions (the loader's window, the tests).
class GotForChecks {
public:
    explicit GotForChecks(u64 module);
    ~GotForChecks();
    GotForChecks(const GotForChecks&) = delete;
    GotForChecks& operator=(const GotForChecks&) = delete;
    bool ok() const {
        return ok_;
    }
    u64 saved(u32 k) const {
        return k < kSlots ? saved_[k] : 0;
    }

private:
    static constexpr u32 kSlots = 4;
    u64* at_[kSlots] = {};
    u64 saved_[kSlots] = {};
    bool ok_ = false;
};

/// Tests only: 1 - the hand-written 0x1a1770 passes a wrong index type; 2 - the hand-written 0x1a1960 skips the
/// primitive type; 0 - none.
void InjectFault(u32 fault);

/// SSPC_R6G_STATS: per-call measurement in the hand-written path.
void EnableStats(bool on);
bool StatsEnabled();
/// The measurement so far, as log text (summed over threads; per frame when `frames` > 0).
std::string StatsText(double frames, bool reset);

} // namespace GBufferDraw

/// The current frame (VideoOut flip id) in the game; 0 in the tests (gbuffer_frames*.cpp).
u64 R6GFrameId();

} // namespace SecondSon::NativeEngine::Reconstructed
