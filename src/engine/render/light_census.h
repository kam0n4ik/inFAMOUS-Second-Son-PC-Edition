// SecondSonPC — R12: how many lights the engine puts into the frame's light list, of which kind, with a shadow, and
// what the GPU did for them (diagnostics, SSPC_R12_LIGHT_CENSUS=1; off by default; docs/R12_LIGHTS.md).
//
// The frame's light list is the object at eboot 0xe17f90 (+0x154 the count, +0x158 the capacity 2000, +0x330 the 0xb0
// records the tiled lighting reads, +0x338 the 0x50 records). Read-only entry probes (guest_probe.h) on:
//   0x19a4d0  a data light (CLight) into a job's batch: 0x16c430 fills the record (+0x2c kind, +0x4c the shadow slot,
//             -1 none); the batch count grows only when the light is kept
//   0x19a960  a point light made by code (effects; caller 0x2e9cb0): +0x2c = 1
//   0x16dcc0  a job's batch copied into the frame's list (memcpy of n records)
//   0x16dd50  particle lights: n slots reserved, filled by a GPU DMA (0x3f3ed0)
// The GPU backend adds the spot shadow atlas's depth draws and the glows' occlusion queries (runtime/gpu/r12/
// light_stats.h). A log line every 10 s: per frame averages.
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::LightCensus {

/// The loader's window before the guest runs; false (logged) when the switch is set but a function does not hash.
bool InstallFromEnvironment(u64 eboot_base);

} // namespace SecondSon::Engine::Render::LightCensus
