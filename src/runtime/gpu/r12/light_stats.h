// SecondSonPC — R12 light census counters the GPU backend fills (diagnostics, SSPC_R12_LIGHT_CENSUS=1; off by
// default; docs/R12_LIGHTS.md). The engine side (engine/render/light_census.cpp) reports them with the engine's own
// light counts every 10 s, so every run says how many lights, shadow-atlas draws and glow queries it had.
#pragma once

#include <atomic>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::R12 {

struct LightStats {
    std::atomic<u64> atlas_draws{0};   ///< depth-only draws into a 1024-wide depth target (the spot shadow atlas)
    std::atomic<u64> cascade_draws{0}; ///< depth-only draws into other depth targets (sun cascades and the rest)
    std::atomic<u64> occ_queries{0};   ///< the guest's occlusion queries ended (one per glow test)
    std::atomic<u64> occ_draws{0};     ///< draws counted inside them
};

/// SSPC_R12_LIGHT_CENSUS=1 (read once).
bool LightCensusOn();
LightStats& Stats();
/// A dispatch's written buffer range of 256 KiB or more (which shaders make the large batch-end write-backs): kept by
/// (shader, range start), reported with the census every 60 s.
void NoteWriter(u64 shader, u64 begin, u64 bytes);
/// The top writers by bytes, one per line (for the census report).
std::string TopWriters(size_t top);
/// SSPC_R12_WRITER_DETAIL=<hex shader address> (0 when unset); NoteDetail logs each distinct line once (up to 48).
u64 DetailShader();
/// SSPC_R12_READERS_OF=<hex guest address> (0 when unset): the shaders binding it go to NoteDetail.
u64 ReadersOf();
void NoteDetail(const std::string& line);

} // namespace SecondSon::Gpu::R12
