// SecondSonPC R7 — the Native Render Layer: the contract between the native engines and the existing backend
// (owned by R7-Core; docs/NATIVE_ENGINE_R7_CORE.md §4).
//
//   Game engine  ->  Native Render Layer  ->  the existing backend (CommandProcessor -> Executor -> Vulkan)
//
// There is no second GPU backend and no second command processor. A native operation is the P5.3 native stream's
// (native_render_ops.h: StreamOp::Registers / State / Draw, a run token at the place in the context's DCB where the
// game's packets would be): the processor runs it through the entry a PM4 draw takes (Draw -> BuildDraw -> the
// frontend's stage preparation -> the executor), so pipelines, descriptors, memory, barriers and submission stay the
// existing ones, and the order relative to the game's packets (barriers, labels, other contexts) is the token's place.
// Whatever the layer cannot carry, the game's own path runs, unchanged.
//
// Who fills what (from the engine's own objects - no PM4 is read to build them):
//   R7-G / R7-S  NativeDrawCommand and its NativeDrawState for the geometry / shadow engines' draws;
//   R7-R         NativeResourceBindings - a stage's resources as the CUE holds them, versioned per stage;
//   R7-Core      NativeRenderContext and Submit (the stream, the fallback, the verification, the counters).
// A subsystem that needs another field adds it here only through R7-Core (a "[shared]" commit, §3 of the doc).
#pragma once

#include <atomic>

#include "base/abi.h"
#include "engine/render/native_render_ops.h"

namespace SecondSon::Engine::Render::R7 {

/// The subsystem that built a command (counters, routing).
enum class Producer : u8 { None = 0, GBuffer = 1, Shadows = 2, Resources = 3, Core = 4 };

/// The engine's draw function a command stands for (an eboot offset; 0: another caller of the draw wrappers).
enum class EngineFn : u32 {
    Other = 0,
    GBufferInstanced = 0x1a1770, ///< the mesh pass's instanced draw
    GBufferMesh = 0x1a1960,      ///< the mesh pass's single draw
    ShadowInstanced = 0x43d480,  ///< the shadow casters' instanced draw
};

/// One shader stage's resources as the CUE holds them (VS / PS: kStageVs / kStagePs of native_render_ops.h).
struct NativeStageResources {
    bool present = false;
    u64 shadow = 0;         ///< the stage's resource shadow (CUE + 0x90 + stage * 0x1800)
    u32 dwords = 0;         ///< what the commit copies of it (the layout table's first entry)
    u64 snapshot = 0;       ///< the commit's copy in the frame ring the user data point at (0: not committed)
    TableShape shape{};     ///< where the user-data pointers into the snapshot sit
    u32 direct_mask = 0;    ///< user-data SGPRs written directly (setUserDataRegion), one bit each
    u32 direct[16] = {};
    /// R7-R: changes whenever anything of the stage's resources changes (setters, material, commit); equal versions of
    /// one CUE and stage mean equal resources. 0: not tracked - nothing may be reused by it.
    u64 version = 0;
    /// R8 (A4's CUE audit): the CUE's reset epoch (0x12390 / 0x13d20 each frame turn the snapshot ring, so a snapshot
    /// address can come back with other bytes while the version holds) - a consumer keys on (version, epoch), never on
    /// the snapshot address, and compares the user data by value (0x16fb0 writes SGPRs past the shadow)
    u64 epoch = 0;
    /// the stage's shader program by value: code address (SPI_SHADER_PGM_LO << 8) as the commit sets it; 0: unknown
    u64 program = 0;
};

struct NativeResourceBindings {
    u64 cue = 0;
    NativeStageResources stage[kDrawStages];
};

/// What the draw's shaders and fixed function see, besides the resources.
struct NativeDrawState {
    s32 pass = -1; ///< 0..40; -2 pre, -3 post, -4 deformation, -1 another context (as DrawOp::pass)
    u32 sub = 0;
    bool shaders = false; ///< vs / ps known (the commit's ordinary shader path)
    bool has_ps = false;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    u32 prim_type = 0;     ///< VGT_PRIMITIVE_TYPE
    u32 index_size = 0;    ///< INDEX_TYPE (0: 16-bit, 1: 32-bit)
    u32 num_instances = 1; ///< NUM_INSTANCES
};

/// One draw as the engine meant it.
struct NativeDrawCommand {
    Producer producer = Producer::None;
    EngineFn fn = EngineFn::Other;
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;
    /// CITY-OPT run hint (DrawRuns::Source in the low byte, DrawRuns::kMaterialChanged): what the engine's draw functions
    /// tag their draws with today, so a native draw keeps the Phase 5/6 runs (0: none)
    u32 run_tag = 0;
    NativeDrawState state;
    NativeResourceBindings resources;
};

/// The engine context a command is recorded into.
struct NativeRenderContext {
    u64 r = 0;   ///< the render context (pool slot, pre, post or deformation)
    u64 cue = 0; ///< its CUE (r + 0x128)
    u64 dcb = 0; ///< its command buffer (r + 8)
    s32 pass = -1;
    u32 sub = 0;
};

enum class SubmitResult : u8 {
    Native,   ///< the command is in the native stream; the caller must not run the game's path
    Fallback, ///< not carried (no room, an unsupported form, verification only): the caller runs the game's path
    Off,      ///< the layer is off (SSPC_R7_NATIVE_RENDER unset): the caller runs the game's path
};

/// SSPC_R7_NATIVE_RENDER (read once; off by default; needs --gpu-backend sspc):
///   0 / unset       off - the game's path everywhere
///   gbuffer (1)     R7-Core C3: the G-buffer's instanced draw 0x1a1770 - its CUE commit and draw go to the native
///                   stream (no PM4 of their own), its CITY-OPT run tag with the draw (needs SSPC_CITY_ENGINE_OPT and
///                   SSPC_CITY_ENGINE_GBUFFER_RUNS: the probe on 0x1a1770 opens the scope; the stand's preset sets both)
///   gbuffer-verify  the same, each native draw preceded in its run by the engine's description of it (StreamOp::
///                   Verify): the processor compares the draw the native commit and draw built with it (P5.3B fields)
enum class Mode : u8 { Off, GBuffer, GBufferVerify };
Mode GetMode();
bool Enabled();

/// The draw in the context's native stream at the DCB's write pointer, or Fallback/Off - then nothing was written.
/// (R7-Core C3: the stream's writer is engine/render/native_draw.cpp, EmitNativeDraw.)
SubmitResult Submit(const NativeRenderContext& ctx, const NativeDrawCommand& cmd);

/// A key of the state two draws must share to reuse one prepared draw state (shader registers, prim / index type, the
/// tables' shapes and versions); 0 when something in it is not known (no reuse).
u64 StateKey(const NativeDrawCommand& cmd);

struct LayerCounters {
    std::atomic<u64> submitted[5]{};  ///< by Producer
    std::atomic<u64> native[5]{};
    std::atomic<u64> fallback[5]{};
};
LayerCounters& Counters();

// --- R7-Core C3: the scope of an engine draw function's call ---------------------------------------------------------
//
// With the G-buffer path on (SetGBufferPath, by native_draw.cpp at install), the probe on 0x1a1770 (draw_runs.cpp) opens
// a scope on its thread for the call: the context R and the CITY-OPT tag of its draw (DrawRuns::Source::GBuffer |
// kMaterialChanged, decided before the call - 0x448640 runs before it). Inside the scope the commit and draw wrappers'
// probes (native_draw.cpp) put the commit and the draw into the native stream with that tag instead of the game's PM4;
// a draw that falls back is tagged in its NOP by the probe as before.

struct DrawScope {
    u64 r = 0;
    u32 tag = 0;
    u32 native = 0; ///< its draws that went to the native stream
    EngineFn fn = EngineFn::Other; ///< the engine function whose call it is
};
void SetGBufferPath(bool on);
bool GBufferPath();
/// A scope for a call of `fn` on `r`; returns the one it replaces (CloseScope restores it).
DrawScope OpenScope(u64 r, u32 tag, EngineFn fn = EngineFn::GBufferInstanced);
/// The scope closed, `outer` restored; its native draws.
u32 CloseScope(const DrawScope& outer);
/// The thread is inside a scope on `r`: its tag (and the function whose call it is).
bool InScope(u64 r, u32& tag);
bool InScope(u64 r, u32& tag, EngineFn& fn);
/// A draw of the open scope went to the native stream.
void ScopeNative();

struct ScopeCounters {
    std::atomic<u64> scopes{0};       ///< 0x1a1770 calls with the path on
    std::atomic<u64> native_calls{0}; ///< ... that drew through the native stream
    std::atomic<u64> native_draws{0};
};
ScopeCounters& ScopeStats();

// --- R8-A3 (docs/NATIVE_ENGINE_R8_RENDER_LAYER.md): the layer as a working path ----------------------------------------
//
// NR5. SSPC_R8_RENDER_PATHS=mesh,shadow (with SSPC_R7_NATIVE_RENDER=gbuffer*; off by default): the scope and the native
// commit / draw of 0x1a1770 also for the mesh pass's single draw 0x1a1960 (mesh) and the shadow casters' instanced draw
// 0x43d480 (shadow), each its own switch; their probes (draw_runs.cpp, Phase 5) open the scope with the Phase 5 tag.
enum PathBits : u32 { kPathMesh = 1, kPathShadow = 2 };
u32 R8Paths();
/// The layer carries `fn`'s draws now (0x1a1770 with the layer on; the others with their R8 switch too).
bool Carries(EngineFn fn);

// NR2. The Native Resource State's view of the layer's own commits. The native commit (native_draw.cpp NativeCommit)
// makes the snapshot and the user data the game's commit 0x14ad0 would make; the state (native_engine/r7/resources)
// learns a commit by watching 0x14ad0's calls - for the native one the commit reports the same events itself through
// these hooks, so a stage's binding and version stay the GPU's. All on the committing thread, between begin and end, in
// 0x14ad0's order. Fill: the CUE's resources for a draw (R7::Resources::Fill), copied by value.
// The state registers them (R7::Resources::Prepare); header-only storage, so neither side links the other.
struct ResourceHooks {
    void (*begin)(u64 cue) = nullptr;
    void (*snapshot)(u32 cue_stage, u64 snapshot, u32 dwords) = nullptr; ///< allocated and copied from the shadow
    void (*pointer)(u32 cue_stage, u32 sgpr, u64 value) = nullptr;       ///< setPointerInUserData's
    void (*value)(u32 cue_stage, u32 sgpr, u32 value) = nullptr;         ///< setUserDataRegion(.., 1)'s
    void (*end)(u64 cue) = nullptr;
    bool (*fill)(u64 cue, NativeResourceBindings& out) = nullptr;
};
inline std::atomic<const ResourceHooks*> g_resource_hooks{nullptr};
inline void SetResourceHooks(const ResourceHooks* hooks) {
    g_resource_hooks.store(hooks, std::memory_order_release);
}
inline const ResourceHooks* GetResourceHooks() {
    return g_resource_hooks.load(std::memory_order_acquire);
}
/// SSPC_R8_RENDER_RESOURCES=1 (off by default): the layer's native commits are reported through the hooks and its
/// commands carry the CUE's resources (NativeDrawCommand::resources, by value) - needs SSPC_R7_RES_STATE (the hooks).
bool ResourcesOn();

} // namespace SecondSon::Engine::Render::R7
