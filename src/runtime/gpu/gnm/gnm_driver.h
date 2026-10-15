// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — SecondSonPC's own libSceGnmDriver (Phase 4, P4.4, EXPERIMENTAL: export group "gnm", bound together with
// SecondSonPC's VideoOut only with --gpu-backend sspc; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md). Ported from
// shadPS4's gnmdriver.cpp (GPL-2.0-or-later), the bridge's driver, which stays the oracle (src/bridge/shadps4/
// selftest_gnm.cpp compares every command-building function with it).
//
// The game's 85 libSceGnmDriver imports. Command-building functions write the same PM4 dwords as the bridge's into the
// game's own command buffers (guest addresses are host addresses) and need nothing else. Submissions, compute queues
// and flips go through Gnm::Host (gnm_host.h), installed by SecondSonPC's backend; before that (or after it stopped)
// those functions log an error and refuse. Event queue registrations (sceGnmAddEqEvent) go to the queues' owner
// (Kernel::EventQueues) and are woken by RaiseInterrupt. Base PS4 (non-Neo) only: with Neo mode on the driver logs an
// error when a host is installed and still builds the base PS4's packets.
//
// Kept from the bridge: argument checks, results and error codes; the submission lock (sceGnmSubmitDone takes it while
// the GPU is busy, Interrupt::GpuIdle releases it, every submission and DingDong waits for it); the initialisation
// sequence once before the first submission after each SubmitDone; the flip marker patch of sceGnmSubmitAndFlip*;
// compute queue ids. Where the bridge asserts (or would crash on the game's input) this driver logs an error and
// returns an error code instead (gnm_driver.cpp lists each case); where the bridge is a stub the result is the same
// and a warning says once that the function does nothing.
#pragma once

#include <span>

#include "base/abi.h"
#include "runtime/gpu/gnm/gnm_host.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Gpu::Gnm {

/// The game's libSceGnmDriver imports (group "gnm", experimental), keyed exactly like the game imports them.
std::span<const Runtime::Export> GnmExports();

/// P5.4 early frame submission (SSPC_P54_EARLY_SUBMIT=1, an experiment, off by default;
/// docs/native-reconstruction/P54_FRAME_LIFECYCLE.md): the arguments the engine's SubmitFrame will pass to
/// sceGnmSubmitAndFlipCommandBuffers once the previous frame's end-of-pipe label is written, read when that SubmitFrame
/// calls sceGnmSubmitDone with the list complete. The arrays are the caller's, valid during the call only.
struct EarlyFrame {
    u32 count = 0;
    u32* const* dcbs = nullptr;
    const u32* dcb_sizes = nullptr;
    u32 vo_handle = 0;
    u32 buffer_index = 0;
    u32 flip_mode = 0;
    s64 flip_arg = 0;
};

/// Called inside sceGnmSubmitDone (under the driver's submission mutex) with the address of its return address: true
/// and `out` filled when the caller is that step of SubmitFrame with a frame to flip.
using EarlyFrameSource = bool (*)(const u64* return_slot, EarlyFrame& out);

/// Installs the source (nullptr: off) before the guest runs. With a source, sceGnmSubmitDone hands that frame on (flip
/// request and DCBs, as sceGnmSubmitAndFlipCommandBuffers would) and the game's own call with the same arguments returns
/// its result without submitting again; any other call is handled as before. The submission lock is not taken: the
/// engine itself waits for the previous frame's label before it reuses that frame's buffers.
void SetEarlyFrameSource(EarlyFrameSource source);

namespace Test {
/// The driver as at start-up: no submission lock, the initialisation sequence pending, no compute queue mapped, no
/// event registration, SDK version 0. Self-tests only: no host installed and no guest thread inside the driver.
void Reset();
/// sceGnmSubmitDone took the submission lock and no Interrupt::GpuIdle has released it yet.
bool SubmissionLocked();
} // namespace Test

} // namespace SecondSon::Gpu::Gnm
