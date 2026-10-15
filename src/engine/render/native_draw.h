// SecondSonPC — Native Render Interface, engine side (P5.3B; docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md).
//
// SSPC_P53_NATIVE_DRAW=shadow (off by default): entry probes on the engine's draw wrappers (eboot 0x623c60 drawIndex,
// 0x623a40 drawIndexAuto; every mesh/fullscreen draw of the render passes reaches one of them right after the CUE
// commit 0x14ad0). At each call the draw is described from the engine's own objects (BuildDrawOp) - the context's pass,
// the CUE's VS/PS shader objects, the stage resource shadows and user-data layout tables, the wrapper's arguments -
// and written into the context's command buffer as a kVerify marker just before the game's draw packet. The game's
// draw runs unchanged; SecondSonPC's command processor compares its PM4-built draw with the marker
// (native_render_ops.h). Counters in the log every SSPC_P53_NATIVE_DRAW_STATS seconds (default 10).
#pragma once

#include <string>

#include "base/abi.h"
#include "engine/render/native_render_ops.h"

namespace SecondSon::Engine::Render::NativeDraw {

/// The loader's window before the guest runs. False (and logged) when the switch is set but nothing was installed.
/// `native_backend`: SecondSonPC's own GPU backend runs the game (--gpu-backend sspc); only its command processor
/// knows the markers, so apply mode is refused without it (the bridge would drop the native draws) and shadow runs.
bool InstallFromEnvironment(u64 eboot_base, bool native_backend);

/// P5.3 render modes (--render-mode / SSPC_RENDER_MODE; one SecondSonPC.exe, one game):
///   legacy               the game's PM4 through SecondSonPC's command processor, as before (the default)
///   hybrid               the verified native parts: passes 1 (G-buffer) and 9 (lighting) - commit, draw and render
///                        state natively; every engine draw described (kDescribe), its stages through the Native
///                        Render Frontend's cache; all else the game's PM4 (the fallback)
///   native_experimental  every pass 0..40 natively where the engine's path allows it (commit/state fallbacks to the
///                        game's functions as counted), the Native Render Frontend for all
/// A mode sets the SSPC_P53_* switches it consists of unless they are set already (an explicit switch wins). Needs the
/// native backend; with the bridge the mode is refused (logged) and legacy runs. Before the GPU backend starts.
/// Returns the mode that runs.
std::string ApplyRenderMode(std::string mode, bool native_backend);

/// The draw that the context `r` (render context R, its DCB at R+8) is about to issue, from the engine's objects.
/// `module_base` locates the frame object (pass keys); false when `r` is not a context SecondSonPC knows.
bool BuildDrawOp(u64 module_base, u64 r, bool indexed, u32 index_count, u64 index_base, DrawOp& out);

// --- P5.3C: the CUE commit done natively ----------------------------------------------------------------------------
//
// SSPC_P53_NATIVE_DRAW=apply:<pass>[,<pass>...]: in the listed passes the commit 0x14ad0 and the draw wrappers do not
// run; the native commit gives the processor the commit's register writes (OpKind::Registers) and the draw goes as a
// kApply|kVerify DrawOp, executed by the processor without a draw packet. The other passes stay in shadow mode.

/// The game functions the native commit calls (the game's own snapshot allocator and PS input mapping).
using AllocFn = SSPC_SYSV_ABI u64 (*)(u64 cue, u32 dwords);                                       ///< eboot 0x123d0
using PsInputsFn = SSPC_SYSV_ABI void (*)(u32* out, u64 vs_semantics, u32 vs_count, u64 ps_semantics, s32 n); ///< 0x638540
struct CommitEnv {
    AllocFn alloc = nullptr;
    PsInputsFn ps_inputs = nullptr;
};

enum class CommitCheck : u8 {
    Native,    ///< VS/PS only: the native commit reproduces it
    StageMode, ///< ES/GS or LS/HS mode (CUE+0xC1E0 & 0x21): the game's commit runs
    AltShader, ///< a shader set another way (CUE+0xC1D9/0xC1DA)
    Missing,   ///< a dirty stage without its shader object or layout table
};
/// No side effects.
CommitCheck CheckNativeCommit(u64 cue);
/// What 0x14ad0 does in the VS/PS case, without a command buffer: the dirty stages' snapshots are allocated and copied
/// with the game's allocator, the register writes the game's packets would make are returned in the same order (at
/// most kMaxRegWrites), the CUE's dirty flags are cleared as the commit clears them. Call after CheckNativeCommit.
u32 NativeCommit(u64 cue, const CommitEnv& env, RegWrite* out);

} // namespace SecondSon::Engine::Render::NativeDraw
