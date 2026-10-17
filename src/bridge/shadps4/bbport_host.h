// SecondSonPC — experiment/bbport-gpu: bbport's GPU library as the game's GPU backend (docs/BBPORT_GPU_EXPERIMENT.md).
//
// bbgpu-sspc.dll (bbport's vendored shadPS4 video core, Gnm/VideoOut drivers and kernel event queues, built by MSYS2
// clang from E:\BBPort-GPU-Test, branch sspc-integration) is loaded from <exe>\bbport\. The loader functions it takes
// from bbport's own runtime come from here (gpu/sspc_host.h): guest memory on SecondSonPC's manager (--native memory),
// one clock, the guest errno. Its GnmDriver, VideoOut and event queue functions are bound to the game's imports in
// place of SecondSonPC's and the bridge's; SecondSonPC's GPU backend is not started. Access violations reach its page
// tracking first through the bridge's signal dispatch.
#pragma once

#include <string>
#include <vector>

#include "app/config.h"
#include "base/abi.h"
#include "runtime/exports.h"

namespace SecondSon::Bridge::Bbport {

/// Loads the library, hands it the host table, registers its event queues and initializes it (its window, Vulkan
/// device, presenter and GPU command processor); attaches it to the guest memory manager. false with `error` on any
/// failure (nothing of it is used then: the caller stops the run).
bool Start(const App::Config& cfg, const std::string& title, const std::string& serial, u32 sdk_version,
           u32 psf_attributes, std::string* error);

/// After the guest memory contract is installed: its memory hooks get the mappings so far and every change.
bool Attach(std::string* error);

/// The game imports the library serves (GnmDriver, VideoOut, the kernel's event queues), as exports placed before
/// SecondSonPC's (the first entry for an import key wins); GnmDriver/VideoOut imports it lacks get logging stubs (the
/// bridge's renderer must not start for them). Logged.
std::vector<Runtime::Export> Exports();

/// SSPC_FLIP_LOG=1 (both backends, for A/B): the flip functions' implementation wrapped by a timestamp of every call
/// (<logs>\flips.csv). Returns the wrapper for a flip import (nid), else `implementation` unchanged.
void* WrapFlip(const char* nid, void* implementation);
void SetFlipLogDir(const std::string& logs_dir);

} // namespace SecondSon::Bridge::Bbport
