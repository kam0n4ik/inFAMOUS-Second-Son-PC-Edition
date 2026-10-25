// SecondSonPC — dynamic trace of the engine's render frame (P5.3A, diagnostics, SSPC_P53_RENDER_TRACE=1; off by
// default; docs/native-reconstruction/RENDER_ENGINE_PASSES.md).
//
// Entry probes (guest_probe.h) on functions the reconstruction rests on; the game's code runs unchanged, the probes
// only read its data:
//   SubmitFrame 0x1f1e30   every frame: whether each pool context's command buffer starts anew (the per-frame reset
//                          of the 254 contexts that static analysis does not show); every Nth frame: the frame's
//                          contexts in submission order, their command buffers walked (dcb_scan.h), the resources the
//                          engine bound on each, grouped into passes and linked (pass_graph.h) -> one JSON file
//   Reset 0x11f050         calls by caller and by kind of context (pool slot or not)
//   DcbReserve 0x11eed0    the DCB's out-of-space callback: chunks taken per frame
//   CUE setters 0x16d10 / 0x16de0 / 0x16b70 / 0x16c40   textures / RW textures / buffers / RW buffers bound per
//                          context, recorded only while a sampled frame is being recorded
//
//   SSPC_P53_RENDER_TRACE=1           install the probes (the eboot's functions must hash to the analysed ones)
//   SSPC_P53_RENDER_TRACE_EVERY=<n>   frames between two sampled frames (default 300)
//   SSPC_P53_RENDER_TRACE_DUMPS=<n>   sampled frames written (default 8)
//   SSPC_P53_RENDER_TRACE_DIR=<dir>   where (default logs\p53_render_trace)
//   SSPC_P53_RENDER_TRACE_STATS=<s>   a log line every <s> seconds (default 10; 0: none)
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::Trace {

/// The loader's window before the guest runs. Returns false (and logs why) if the switch is set but the probes could
/// not be installed; nothing is changed then.
bool InstallFromEnvironment(u64 eboot_base);

/// P5.4 frame lifecycle (SSPC_P54_FRAME_TRACE=1; docs/native-reconstruction/P54_FRAME_LIFECYCLE.md): SubmitFrame's
/// span and the engine's async compute waits (0x1695e0, replaced by the same loop) go to the stall trace
/// (SSPC_P412_STALL_TRACE, runtime/gpu/stall_trace.h) as GameSubmitFrame / GameComputeWait. After
/// InstallFromEnvironment (it shares the SubmitFrame probe); the same window before the guest runs.
bool InstallLifecycleFromEnvironment(u64 eboot_base);
/// FrameBreak CC-2: SSPC_CC_CRITICAL=1 - the async compute wait 0x1695e0 (the lifecycle trace's replacement, installed
/// here when the trace is off) counts the game's blocked waits in Gpu::CriticalCompute; after InstallLifecycle.
bool InstallCriticalComputeFromEnvironment(u64 eboot_base);

/// P5.4 early frame submission (SSPC_P54_EARLY_SUBMIT=1, an experiment, off by default; the same document): SubmitFrame
/// calls sceGnmSubmitDone (its step 4, mode 0) with its list of DCBs complete, then waits for the previous frame's label
/// (step 5) before it passes that list to sceGnmSubmitAndFlipCommandBuffers (step 6). With the switch the driver
/// (Gpu::Gnm::SetEarlyFrameSource) reads the list from SubmitFrame's stack frame at step 4 and hands the frame on there:
/// the command processor records frame k while the GPU finishes frame k-1. The game's code is unchanged and still waits
/// at step 5 before it returns and reuses frame k-1's buffers. The same window before the guest runs.
bool InstallEarlySubmitFromEnvironment(u64 eboot_base);

/// REA-3 (SSPC_REA_DEFER_WAIT_B=1, an experiment, off by default; docs/REA_ENGINE_REVERSE.md): the frame's second async
/// compute wait (0x3ee470's tail call to 0x1695e0, return 0x29cd5b in the main loop) waits for the particles' light DMA
/// (0x3f3ed0: GPU-made light records copied into the frame's light list), which the frame's graphics reads. The game
/// thread does not block there: the next graphics submission starts with WAIT_REG_MEM(label >= target)
/// (Gpu::Gnm::SetGraphicsGate). The first wait (0x3edef0: the CPU reads emitter state) stays. Its safe reset (unless
/// SSPC_R9_REA3_SAFE_RESET=0, A/B only): at SubmitFrame's return - before the frame's tail 0x196c30 resets the compute
/// queue's command buffer, which the GPU reads in place - the game waits until the queue finished its last submission
/// (docs/R9_REA3_FIX.md). After InstallCriticalComputeFromEnvironment and InstallEarlySubmitFromEnvironment.
bool InstallDeferComputeWaitFromEnvironment(u64 eboot_base);

/// R11 (SSPC_R11_LABEL_WAIT=1, off by default; docs/R11_MAIN_THREAD.md): the async compute wait 0x1695e0 (the replacement
/// above, installed here when nothing else did) sleeps on its label (Gpu::LabelWait) instead of the game's rdtsc spin
/// delay(50): Game:Main leaves the CPU while it waits. After InstallDeferComputeWaitFromEnvironment.
bool InstallLabelWaitFromEnvironment(u64 eboot_base);

} // namespace SecondSon::Engine::Render::Trace
