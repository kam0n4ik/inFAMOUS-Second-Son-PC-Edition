// SecondSonPC — the memory side of SecondSonPC's Vulkan backend (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md
// §C): the GPU timeline, the bounded stream ring, the guest's memory as GPU buffers, GDS.
//
// Guest buffers are the guest's own memory imported into Vulkan (VK_EXT_external_memory_host): the GPU reads and
// writes the game's pages themselves (host-visible, coherent), so a CPU write before a submission is seen by it and a
// GPU write is seen by the CPU once the submission completed — no copy, no second image of guest memory, no page
// tracking for buffers. Windows of imported memory are made on demand (16 MiB aligned, 32 MiB long when the guest
// mapping allows, else exactly the range) and dropped when the guest unmaps them (the memory listener), destroyed only
// after the GPU finished the work that used them. Pages that cannot be imported (reserved but not committed, read-only)
// are reported, never silently replaced.
//
// The stream ring holds what a draw needs that is not guest memory (flattened user data, clip planes, LDS, upload
// copies): 64 MiB, host-visible, reused behind the timeline (a region is free again once the submission that used it
// completed — never vkDeviceWaitIdle per chunk).
#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu {
class PageTracker;
}

namespace SecondSon::Gpu::Vk {

class DeviceContext;

/// One timeline semaphore: value N signalled by the N-th submission. Next() is the recording thread's; the completed
/// value and the last error are read and advanced by any thread (the recording thread, the backend's GPU thread, the
/// present thread's Ready - P4.11: atomics, the completed value only grows).
class Timeline {
public:
    explicit Timeline(DeviceContext& ctx);
    ~Timeline();
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    VkSemaphore Handle() const { return semaphore_; }
    u64 Next() { return ++last_; }            ///< the value the next submission signals
    u64 LastSubmitted() const { return last_; }
    u64 Completed();                           ///< the GPU's value now
    /// Waits until `value` (or `timeout_ns`); true when reached.
    bool Wait(u64 value, u64 timeout_ns);
    /// CP-8: as Wait, but Wake(v >= wake_value) from another thread ends the wait too (a second timeline semaphore,
    /// both waited on with VK_SEMAPHORE_WAIT_ANY_BIT); true when `value` was reached.
    bool WaitOrWake(u64 value, u64 wake_value, u64 timeout_ns);
    /// CP-8: signals the wake semaphore to `wake_value` (the callers serialize it; the values only grow).
    void Wake(u64 wake_value);
    bool Ok() const { return semaphore_ != VK_NULL_HANDLE; }
    /// A query or a wait returned VK_ERROR_DEVICE_LOST.
    bool Lost() const { return last_error_.load(std::memory_order_acquire) == VK_ERROR_DEVICE_LOST; }
    u64 CompletedCached() const { return completed_.load(std::memory_order_acquire); } ///< the last value seen (no query)
    /// P4.10: the GPU's value now without touching the cached one (any thread; 0 on an error).
    u64 QueryCompleted() const;

    // FrameBreak AQ (SSPC_FB_ASYNC_COMPUTE): batches on two queues. The values stay one dense sequence (Next); a batch
    // on the main queue signals the main semaphore with its value, a batch on the async compute lane the lane's
    // semaphore. Every main-queue batch waits for the lane's last batch before it, so the main semaphore at `v` means
    // every batch up to `v` completed; the lane's values above it complete on their own. Completed / QueryCompleted /
    // Wait keep their meaning: every batch up to the value completed.
    /// The lane's semaphore (false: it could not be created).
    bool EnableLane();
    bool LaneOn() const { return lane_ != VK_NULL_HANDLE; }
    VkSemaphore LaneHandle() const { return lane_; }
    /// The recording thread, before the lane's batch `value` is submitted (values in increasing order).
    void SubmittedOnLane(u64 value);
    /// The two semaphores' values now (0 on an error / no lane).
    u64 QueryMain() const;
    u64 QueryLane() const;
    /// Waits until the main semaphore reaches `main_value` or the lane's `lane_value` or the wake semaphore
    /// `wake_value` (0: not that one); true when one of the GPU's was reached.
    bool WaitAny(u64 main_value, u64 lane_value, u64 wake_value, u64 timeout_ns);
    /// R10 pacing (SSPC_R10_AQ_MAIN_FREE=1, docs/R10_PACING.md): a main batch no longer waits for every lane batch
    /// before it, so the main semaphore at `v` means every MAIN batch up to `v` completed; the completed value (every
    /// batch up to it) stops below the lane's first value not completed.
    void SetMainFree(bool on) { main_free_ = on; }
    /// R10: the main-queue batch `value` and the main batches before it completed (lane batches before it possibly
    /// not); false for a lane value (the caller asks Completed then).
    bool MainReached(u64 value) const;

private:
    DeviceContext& ctx_;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    VkSemaphore wake_ = VK_NULL_HANDLE; ///< CP-8: WaitOrWake's second semaphore (host-signalled only)
    VkSemaphore lane_ = VK_NULL_HANDLE; ///< AQ: the async compute lane's batches
    /// AQ: the lane's values not known complete (ascending; popped by Completed only)
    mutable std::mutex lane_mu_;
    std::deque<u64> lane_pending_;
    /// AQ: every batch up to the value completed, from the two semaphores (lane_mu_ held).
    u64 DenseLocked(u64 main, u64 lane) const;
    bool OnLaneLocked(u64 v) const { return std::binary_search(lane_pending_.begin(), lane_pending_.end(), v); }
    /// The completed value raised to `v` (never lowered: two threads may see different values in either order).
    void Advance(u64 v) {
        u64 c = completed_.load(std::memory_order_relaxed);
        while (v > c && !completed_.compare_exchange_weak(c, v, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
    }

    u64 last_ = 0;
    bool main_free_ = false; ///< R10: SetMainFree
    std::atomic<u64> completed_{0};
    std::atomic<VkResult> last_error_{VK_SUCCESS};
};

/// A buffer with its own memory (device-local or host-visible), destroyed with it.
struct OwnedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
    u64 size = 0;
    u32 memory_type = 0;
};
/// Where a host-visible buffer lives: chosen by size (the BAR for small ones), cached system memory, or the BAR (video
/// memory the CPU writes through PCIe; the GPU reads it at video memory speed) whatever the size.
enum class HostPlacement : u8 { Auto, Cached, Bar, Device /* video memory the CPU never maps */ };
bool CreateBuffer(DeviceContext& ctx, u64 size, VkBufferUsageFlags usage, bool host_visible, OwnedBuffer& out,
                  std::string& error, bool host_cached = false, HostPlacement placement = HostPlacement::Auto);
void DestroyBuffer(DeviceContext& ctx, OwnedBuffer& buffer);

class StreamRing {
public:
    /// `placement`: Cached - cached system memory (the CPU's small copies at memory speed; the GPU reads over PCIe);
    /// Bar - video memory the CPU writes through the BAR (the GPU reads at video memory speed, as the bridge's stream
    /// buffer); Auto - by size.
    StreamRing(DeviceContext& ctx, Timeline& timeline, u64 size, HostPlacement placement);
    u32 MemoryType() const { return buf_.memory_type; }
    ~StreamRing();
    bool Ok() const { return buf_.buffer != VK_NULL_HANDLE; }

    /// The hybrid ring (P4.6 night sprint): the CPU writes the cached system memory (`Slice::data`, memory speed), the
    /// GPU reads a device-local mirror of the same size at the same offsets (`Slice::buffer`, video memory speed, no
    /// PCIe read per shader access). The bytes handed out since the last TakeDirty() must be copied into the mirror
    /// before the work that reads them: the executor copies them at the head of each submission (every slice is
    /// written by the CPU while that submission is recorded and never again, so one copy before its first command
    /// is exact). Regions are reused only behind the timeline, as the ring's own. false: no mirror (out of memory).
    bool CreateMirror(std::string& error);
    bool Mirrored() const { return mirror_.buffer != VK_NULL_HANDLE; }
    VkBuffer HostBuffer() const { return buf_.buffer; }
    VkBuffer MirrorBuffer() const { return mirror_.buffer; }
    /// The ring regions handed out since the last call, as host -> mirror copies (at most two: the ring wrapped).
    u32 TakeDirty(VkBufferCopy (&out)[2]);
    u64 MirroredBytes() const { return mirrored_bytes_; }

    struct Slice {
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0;
        u8* data = nullptr;
    };
    enum class Status : u8 {
        Ok,
        NeedFlush, ///< only the submission being recorded holds the ring: submit it, then allocate again
        TooLarge,  ///< more than half the ring
    };
    /// `bytes` aligned to `align` for the submission being recorded (`seq`, the timeline value it will signal). When
    /// the ring is full of regions of submitted work, waits for the oldest (counted).
    Status Allocate(u64 bytes, u64 align, u64 seq, Slice& out);
    /// Regions of completed submissions become free.
    void Collect(u64 completed);

    u64 Size() const { return buf_.size; }
    u64 PeakUsed() const { return peak_; }
    u64 Waits() const { return waits_; }
    u64 BytesAllocated() const { return allocated_; }

private:
    struct Fence {
        u64 seq = 0;
        u64 end = 0; ///< the ring position after the submission's last region
    };
    DeviceContext& ctx_;
    Timeline& timeline_;
    OwnedBuffer buf_;
    OwnedBuffer mirror_;   ///< hybrid: the device-local copy the GPU reads
    u64 dirty_begin_ = 0;  ///< hybrid: the first position not yet copied into the mirror
    u64 mirrored_bytes_ = 0;
    // positions grow without bound (bytes ever handed out, with padding); the ring offset is position % size
    u64 head_ = 0; ///< next free position
    u64 tail_ = 0; ///< oldest position still in use
    u64 used_ = 0;
    u64 peak_ = 0, waits_ = 0, allocated_ = 0;
    std::deque<Fence> fences_;
};

/// The guest's memory as Vulkan buffers. Thread-safe (the GPU thread looks up; the memory listener drops windows).
class GuestBuffers {
public:
    GuestBuffers(DeviceContext& ctx);
    ~GuestBuffers(); ///< destroys every window (call once the device is idle)

    struct View {
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0; ///< of the guest address in the buffer
        u64 base = 0;   ///< the window's guest address
        u64 size = 0;   ///< the window's size
        bool backing = false; ///< imported through the physical backing's view (not the guest's pages)
    };
    enum class Failure : u8 { None, Range, NotImportable, NoExtension };

    /// A buffer covering guest [addr, addr + size) for work of submission `seq`. false: `failure` says why (the
    /// caller reports the resource; nothing is substituted).
    bool Get(u64 addr, u64 size, u64 seq, View& out, Failure& failure);
    /// The guest is about to unmap or replace [addr, addr + size): windows overlapping it leave the lookup now and are
    /// destroyed once the work that used them completed.
    void Drop(u64 addr, u64 size);
    /// Destroys dropped windows whose last use completed.
    void Collect(u64 completed);
    /// The physical backing's host view of [addr, addr + size) when an existing window imported through it covers the
    /// range (mapped for the process's life: a CPU read there never faults), nullptr otherwise.
    const u8* BackingPointer(u64 addr, u64 size);
    /// Write tracking: a window is imported only over writable pages, so armed pages there are given back first.
    void SetTracker(PageTracker* tracker) { tracker_ = tracker; }

    struct Stats {
        u64 windows = 0, bytes = 0, imports = 0, import_failures = 0, drops = 0, destroyed = 0, lookups = 0;
        u64 exact_windows = 0;
        u64 backing_windows = 0, guest_windows = 0; ///< imported through the backing's view / the guest's pages
        u64 merged_runs = 0; ///< REA-4: backing runs joined to the one before (host-contiguous)
        u64 superseded = 0;  ///< R9-F3: windows replaced by a longer heap window of their base
        std::string first_failure;
    };
    Stats GetStats();

private:
    struct Window {
        u64 base = 0, size = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u64 last_seq = 0;
        bool backing = false;
        u8* host = nullptr; ///< the host view the window was imported from
    };
    /// Imports guest [base, base + size) through `host`: the backing's view of it, or the guest's pages themselves.
    bool Import(u64 base, u64 size, u8* host, Window& out, std::string& error);
    void Destroy(Window& w);

    DeviceContext& ctx_;
    PageTracker* tracker_ = nullptr;
    std::mutex mutex_;
    std::multimap<u64, Window> windows_; ///< by base (a larger and a smaller window may share one - review MEDIUM 5)
    Window* last_get_ = nullptr;          ///< the window Get found last (under mutex_; Drop clears it)
    const Window* last_backing_ = nullptr; ///< the backing window BackingPointer found last (likewise)
    std::vector<Window> retired_;
    u64 max_window_ = 0;
    Stats stats_;
};

} // namespace SecondSon::Gpu::Vk
