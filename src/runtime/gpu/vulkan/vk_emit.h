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

namespace SecondSon::Gpu::Vk {
struct Api;
}

namespace SecondSon::Gpu::Vk::Emit {

/// After the device's functions are loaded: with SSPC_VK_EMIT_THREAD=1, the thunks replace the table's entries and the
/// emitter thread starts (true). Otherwise nothing changes.
bool Install(Api& f);

/// The calling thread records into the backend's command buffers: its per-draw commands go through the ring.
void SetProducerThread();

/// A producer: returns once every command it put in the ring was passed to the driver (before anything that records
/// into its command buffers by other means - the upscalers' own Vulkan calls). Elsewhere: nothing.
void Sync();

std::string Report();

} // namespace SecondSon::Gpu::Vk::Emit
