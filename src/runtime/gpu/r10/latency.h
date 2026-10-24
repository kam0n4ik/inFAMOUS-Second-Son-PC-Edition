// SecondSonPC R10 — the input-to-present latency probe (docs/R10_CPU_GPU_SCHEDULER.md §7; SSPC_R10_LATENCY=1, off).
//
// What it measures: from the moment scripted input (--press) is pushed into the window's event queue to the present of
// the first frame whose picture shows it. The input side is the moment of SDL_PushEvent (TSC); the picture side is a
// grid of 16 x 9 texels of every presented game frame, copied by the present's own command buffer into a host buffer
// of its slot and read when that slot comes round again (its fence met). The probe writes, per presented frame, the
// mean absolute change of the grid against the frame presented before (0..255 per channel) and the TSC right after
// vkQueuePresentKHR returned; tools/r10/latency.py finds, for each camera turn of the route, the first present after
// it whose change stands out of the frames before it. A camera turn while standing changes almost every texel, so the
// detection does not depend on what the scene shows.
//
// Cost: one vkCmdCopyImageToBuffer of 144 texels and one memory barrier per present, a 1.2 KiB host read two presents
// later; nothing when off. File (at the backend's stop): <logs>/r10_latency.csv.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::R10::Latency {

constexpr u32 kGridW = 16, kGridH = 9, kGrid = kGridW * kGridH;

/// SSPC_R10_LATENCY=1 (read once).
bool On();

/// Scripted input pushed now: kind 1 key, 2 mouse button, 3 mouse movement (dx, dy its relative motion).
void Input(u32 kind, float dx, float dy);

/// A presented frame's grid read back: `flip_id` (0: a frame without a picture), `present_tsc` right after
/// vkQueuePresentKHR, `texels` kGrid texels of `bpp` bytes (4 or 8) each.
void Present(u64 flip_id, u64 present_tsc, const u8* texels, u32 bpp);

/// Writes <dir>/r10_latency.csv (after the presenter stopped).
void Stop(const std::filesystem::path& dir);

} // namespace SecondSon::Gpu::R10::Latency
