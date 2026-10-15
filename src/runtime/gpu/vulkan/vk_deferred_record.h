// SecondSonPC — deferred Vulkan command recording on a recorder thread (SSPC_PORT_VK_RECORDER=1, off by default).
//
// Port of the ThreadedRecording / RecordPrefetch technique of Supermedo's uncharted_pc (inherited there from
// deadinside28's bbport; GPL-2.0-or-later like shadPS4): gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h
// (RecordChunk, Scheduler::Record) and vk_scheduler.cpp (KickRecording, SyncRecording, RecorderThread, the sync in
// SubmitExecution). There a closure per command is placed in 128 KiB chunks (no allocation per command), chunks go to
// the recorder thread in batches of >= 32 KiB, the recorder spins 200 us before it sleeps, and a submission first waits
// until every recorded command is in the command buffer.
//
// Here the backend records through the function table (vk_api.h, Fn()); on the threads that record the executor's
// command buffers (the GPU thread, the parallel processor's recording thread) Fn() returns a table whose vkCmd* entries
// copy their arguments (arrays included) into the current chunk; the recorder thread calls the real functions in the
// same order. Functions that touch a command buffer or its pool, a queue, or destroy/free an object first wait until
// every recorded command was recorded (Sync), so the command buffer, the pool's external synchronization and the
// lifetime of every handle are what they were with direct recording. GPU-side order is unchanged: the commands land in
// the same command buffer, in the same order, before the same submission.
#pragma once

#include <string>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk::Deferred {

/// Reads SSPC_PORT_VK_RECORDER; with "1" builds the deferring table from the filled process table and starts the
/// recorder thread. Called once after the device's functions are resolved.
void Configure();

/// Whether deferred recording runs.
bool Enabled();

/// The calling thread records the executor's command buffers through the deferring table (no-op when off).
void AttachThisThread();

/// The calling thread records directly again; everything it deferred is recorded first.
void DetachThisThread();

/// Waits until every deferred command is in its command buffer.
void Sync();

/// Syncs and stops the recorder thread (the backend's teardown).
void Shutdown();

/// A guest -> staging copy in order with the commands (uncharted_pc RecordHostCopy / PoolSmallCopies): on an attached
/// thread it runs on the recorder thread before the next submission (the submission waits for the recorder); else now.
/// `src` must stay readable until then (a backing view of guest memory) and `dst` unread by the CPU.
void HostCopy(void* dst, const void* src, size_t bytes);

/// Counters for the log (one line).
std::string Report();

/// Attach for the scope of a recording thread's loop (Detach on every way out).
struct ThreadScope {
    ThreadScope() { AttachThisThread(); }
    ~ThreadScope() { DetachThisThread(); }
    ThreadScope(const ThreadScope&) = delete;
    ThreadScope& operator=(const ThreadScope&) = delete;
};

/// The table of the calling thread (Fn()): the deferring one on an attached thread, else nullptr.
Api* ThreadTable();

} // namespace SecondSon::Gpu::Vk::Deferred
