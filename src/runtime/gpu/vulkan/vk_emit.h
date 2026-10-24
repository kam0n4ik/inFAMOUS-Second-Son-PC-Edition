// SecondSonPC — the emitter thread (experiment, SSPC_VK_EMIT_THREAD=1): a second Vulkan recording thread.
//
// The backend's threads that record (the command processor's and the recording thread) keep deciding everything - the
// bindings, the caches, the state - but the frequent per-draw commands (draws, pipeline binds, pushed descriptors and
// constants, vertex/index buffers, dynamic state) are not passed to the driver by them: each is copied into a ring and
// the emitter thread makes the driver call, in the same order. Every other command of theirs, and whatever orders
// with the command buffers (begin/end/reset, submits, destroys, descriptor updates), first waits until the ring is
// empty (Sync) and is then called by them as before - so a command pool is never used by two threads at once and the
// recorded stream is exactly the one without the emitter. Threads that are not producers (the presenter, the pipeline
// compilers) call the driver directly. Installed over the process's function table (Fn()) after the device's
// functions are loaded.
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::Vk {
struct Api;
}

namespace SecondSon::Gpu::Vk::Emit {

/// After the device's functions are loaded: with SSPC_VK_EMIT_THREAD=1, the thunks replace the table's entries and the
/// emitter thread starts (true). Otherwise nothing changes.
bool Install(Api& f);

/// The calling thread records into the backend's command buffers: its per-draw commands go through the ring.
void SetProducerThread();

/// A producer, SSPC_VK_EMIT_BATCH=1: its commands are handed to the emitter at Publish (and at any Sync, and when the
/// ring is full), not each one as it is put in the ring. true: on for this thread.
bool EnableBatching();
/// A batching producer: what it put in the ring since its last publish handed to the emitter (nothing: no cost).
void Publish();

/// A producer: returns once every command it put in the ring was passed to the driver (before anything that records
/// into its command buffers by other means - the upscalers' own Vulkan calls). Elsewhere: nothing.
void Sync();

// ---- R11 t2 (docs/R11_T2.md): submissions off the recording thread (SSPC_R11_ASYNC_SUBMIT=1, off by default; needs
// SSPC_VK_EMIT_THREAD=1). A producer's vkBeginCommandBuffer / vkEndCommandBuffer go through the ring like its other
// commands (no Sync), and its batch's vkQueueSubmit2 is made by the submitter thread once the emitter passed every
// entry put in the ring before it, in the order the producers handed them on. Nothing else changes order: a Sync still
// waits for the ring only, never for the submitter (so a holder of the queue's mutex that Syncs cannot wait for it).

/// SSPC_R11_ASYNC_SUBMIT=1 is on (the emitter runs).
bool AsyncSubmit();
/// The driver's own functions (the table as loaded): what may be called without the ring (an object the ring no longer
/// names - a completed batch's pools).
const Api& Real();
/// A producer, AsyncSubmit: `fn(ctx, data)` (`bytes` <= 1024, copied) is called by the submitter thread once the
/// emitter passed every entry in the ring now; the ring published. false: not on / not a producer (call it yourself).
bool SubmitLater(void (*fn)(void* ctx, const void* data), void* ctx, const void* data, u32 bytes);
/// SubmitLater would take a job from this thread (AsyncSubmit and a producer).
bool CanSubmitLater();
/// Every job handed to SubmitLater so far has run (or `timeout_ns` passed: false). Never with the queue's mutex held.
bool DrainSubmits(u64 timeout_ns = 2'000'000'000ull);
/// AsyncSubmit: the first VkResult a deferred vkBeginCommandBuffer / vkEndCommandBuffer returned (0: none).
int RecordingError();

std::string Report();

} // namespace SecondSon::Gpu::Vk::Emit
