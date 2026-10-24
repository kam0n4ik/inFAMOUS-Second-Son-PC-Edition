// SecondSonPC — the guest's buffers in video memory (P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §E).
//
// Measured (logs\p44\diag\p3/p6): the GPU reading the guest's buffers straight from imported host memory (PCIe) cost
// most of the frame (the tiled lighting dispatch alone 30 ms; 0.8 ms once its inputs were in video memory). This cache
// mirrors the guest's GPU address space in one sparse buffer (offset = guest address; RTX 2060: 1 TiB sparse address
// space = 2^40), with device-local memory bound in 64 KiB blocks where the GPU touches it:
//  - a GPU read of a range uploads its pages that are not valid in video memory (a GPU copy from the host import),
//    then write-protects them (PageTracker): the guest's next CPU write invalidates them;
//  - a GPU write goes to video memory; the batch's written ranges are copied back to the host at the batch's end (before
//    its fences are written), so the CPU, the command processor and texture uploads see them after completion;
//  - writes the GPU does not see through the tracker (the command processor's DMA/WRITE_DATA, file reads, unmaps)
//    invalidate explicitly (Invalidate, any thread; applied on the GPU thread).
// Ranges that cannot be tracked (imported through the guest's own pages: no backing) are not cached: the host import.
// One thread records (the GPU thread).
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu {
class PageTracker;
}

namespace SecondSon::Gpu::Vk {

class DeviceContext;
class GuestBuffers;
class Timeline;

struct BufferCacheStats {
    u64 resident_bytes = 0, binds = 0, bind_calls = 0;
    u64 uploads = 0, upload_bytes = 0, writebacks = 0, writeback_bytes = 0;
    u64 invalidations = 0, uncached = 0, over_budget = 0, large_ranges = 0, early_writebacks = 0;
    u64 t_large = 0, t_bound = 0, t_scan = 0, t_hook = 0, t_arm = 0, t_copy = 0; ///< TSC ticks inside Prepare
    u64 n_upload_calls = 0;
    /// write-back ranges by size (<=4 KiB, <=64 KiB, <=1 MiB, <=4 MiB, larger): count, bytes; copy commands recorded
    std::array<u64, 5> wb_hist_count{}, wb_hist_bytes{};
    u64 wb_commands = 0;
    u64 prep_memo_hits = 0; ///< Prepare calls answered by the batch's memo
    /// P4.9: pages uploaded with another binding's first missing page of their block (used before, invalid now); blocks
    /// not extended because the block is not importable whole
    u64 block_upload_pages = 0, block_upload_refused = 0;
    u64 wb_diff_bytes = 0, wb_diff_dispatches = 0, wb_shadow_inits = 0; ///< write-backs by the changed-words shader
    u64 wb_checked_bytes = 0, wb_dirty_chunks = 0, wb_dirty_words = 0; ///< SSPC_DIAG_WB_DIRTY (256-byte chunks, words)
    /// FB-7 (SSPC_FB_LAZY_WB): blocks kept back (hidden), blocks that would have been (=verify), write-backs of kept-back
    /// blocks by a reader (calls, blocks; of them without a shadow: plain copies), exact invalidations of a hidden block
    /// (none expected: every CPU-side writer syncs first)
    u64 lazy_deferred = 0, lazy_would_defer = 0, lazy_flushes = 0, lazy_flush_blocks = 0, lazy_flush_plain = 0,
        lazy_flush_runs = 0;
    u64 lazy_unsynced_inval = 0, lazy_dropped = 0;
    u64 heap_clamped = 0; ///< REA-7: heap bindings bound as their importable prefix
    std::array<u64, 4> lazy_flush_by{}; ///< FlushDeferred's `why`
    std::string first_error;
};

class BufferCache {
public:
    /// `host_wait_binds`: the sparse binds complete on the host before the batch is submitted, instead of the batch
    /// waiting for their timeline on the queue (with --gpu-native-validate: synchronization validation does not track
    /// vkQueueBindSparse's semaphore signals - a batch waiting for one stays unresolved for it and every later present
    /// is reported as WRITE_AFTER_PRESENT; logs\p44\diag c6/c7). Both orderings are valid.
    BufferCache(DeviceContext& ctx, GuestBuffers& guest, PageTracker* tracker, u64 budget_bytes, bool host_wait_binds);
    ~BufferCache(); ///< frees everything (the device must be idle)

    bool Ok() const { return buffer_ != VK_NULL_HANDLE; }

    struct Range {
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0;   ///< of the guest address in `buffer`
        u64 size = 0;     ///< bytes from offset the binding may span
        bool cached = false;
    };
    /// Called before upload commands are recorded (the executor ends rendering and orders earlier writes).
    void SetBeforeUpload(void (*hook)(void* ctx), void* ctx) {
        before_upload_ = hook;
        before_upload_ctx_ = ctx;
    }
    /// Called after FlushDeferred recorded its compute dispatches inside a batch (the executor forgets what it bound).
    void SetAfterDispatch(void (*hook)(void* ctx), void* ctx) {
        after_dispatch_ = hook;
        after_dispatch_ctx_ = ctx;
    }
    /// The GPU accesses guest [addr, addr + size) in commands of submission `seq` recorded into `cb`: the video memory
    /// copy, with its invalid pages uploaded first (recorded into `cb`), and a write recorded for the write-back.
    /// false: not even the host import (the caller reports the resource).
    bool Prepare(u64 addr, u64 size, bool write, VkCommandBuffer cb, u64 seq, Range& out);
    /// The page runs the last Prepare that uploaded copied from the host (until the next Prepare): P4.9-B brings a
    /// block's other used, invalid pages along - past the range asked for.
    const std::vector<std::pair<u64, u64>>& LastUploads() const { return uploads_; }
    /// [addr, addr + size) is valid in video memory (a texture upload reads it there: newer than the host's).
    bool Resident(u64 addr, u64 size, Range& out);
    /// Some page of [addr, addr + size) is valid in video memory (then the host's copy may be older).
    bool AnyValid(u64 addr, u64 size);
    enum class Residency : u8 { None, Partial, All };
    /// How much of a range (up to the cached size) is valid in video memory: one pass. All: `out` set.
    Residency Classify(u64 addr, u64 size, Range& out);
    /// P4.6 night: a small read-only range nothing of which is in video memory (the executor streams those - copies
    /// them into the stream ring each use) may be cached instead: it can be tracked, the budget has room and no CPU
    /// write hit its blocks recently (a block whose cached pages the CPU wrote waits 60 frames, then 120, ... before
    /// it is cached again). Measured on the beach: ~93% of the streamed bytes were the same bytes as the range's last
    /// copy (in the frame or an earlier one).
    bool Promotable(u64 addr, u64 size, u64 frame);
    /// SSPC_DIAG_GEOM: why Promotable refuses the range (0 it does not, 1 size/space, 2 budget, 3 the CPU wrote its
    /// blocks lately, 4 the tracker cannot arm it)
    u32 PromoteWhy(u64 addr, u64 size);
    /// SSPC_CITY_GEOMETRY_CACHE: 0 unless every page of the range is current in video memory (so write-tracked);
    /// otherwise a value that changes whenever a block of it was invalidated or GPU-written since (Block::version)
    u32 RangeVersion(u64 addr, u64 size);
    /// The guest wrote or unmapped [addr, addr + size) behind the GPU's back: the pages are uploaded again. Any thread.
    void Invalidate(u64 addr, u64 size);
    /// Invalidate when some page of the range is valid in video memory (the GPU thread only: it reads the block table
    /// unlocked; the invalidations already queued are applied before any upload, so a valid page here is current).
    void InvalidateIfCached(u64 addr, u64 size);
    /// The write tracker's report: the CPU wrote some page of each 64 KiB block of [addr, addr + size) (a fault gives
    /// the whole block back). Pages the GPU wrote in a batch not completed stay valid: their data is the newer one
    /// and reaches the host at that batch's end - uploading the host's older copy over them would lose it (review
    /// HIGH 2). Any thread.
    void InvalidateCoarse(u64 addr, u64 size);
    /// Before a host-side GPU write of [addr, addr + size) in this batch (WRITE_DATA on the timeline): this batch's
    /// GPU writes there copied to the host first and dropped from the end's write-back (it would put them back over
    /// the newer bytes - review HIGH 1).
    void FlushWrittenForHostWrite(u64 addr, u64 size, VkCommandBuffer cb, u64 seq) { FlushWritten(addr, size, cb, seq); }
    /// The GPU writes [addr, addr + size) of the host's copy in submission `seq` (a range not cached): until `seq`
    /// completes, the host's bytes there are older than the GPU's (no CPU copy may read them).
    void NoteHostWrite(u64 addr, u64 size, u64 seq);
    bool HostWritePending(u64 addr, u64 size) const;
    /// The GPU writes some page of [addr, addr + size) in a submission after `completed` (the video memory copy, written
    /// back to the host when that submission completes): a CPU read of the host's copy must wait.
    bool GpuWritePending(u64 addr, u64 size, u64 completed);
    void Collect(u64 completed);
    /// The end of a batch: the copies of its GPU writes back to the host (`cb` outside rendering).
    void RecordWriteBack(VkCommandBuffer cb, u64 seq);
    /// SSPC_GPU_PROFILE: called in RecordWriteBack after the copies, before the changed-words shader (a timestamp)
    void SetWriteBackMark(std::function<void()> mark) { wb_mark_ = std::move(mark); }
    /// FrameBreak FB-7 (SSPC_FB_LAZY_WB=1): the changed-words write-back of a large range keeps back the 64 KiB blocks
    /// wholly inside it whose pages are all valid and write-protected and that no reader asked for yet - hidden (the
    /// tracker: no CPU access on the guest's view), their video memory copy the only current one. A reader writes them
    /// back first: FlushDeferred (the GPU's readers of the host import: FlushWritten; the processor's and frontend's
    /// reads: GpuWritePending is true there; a CPU access: the tracker's fault). =verify: the bookkeeping only (counted,
    /// written back as before). 0 off.
    int Lazy() const { return lazy_; }
    void DisableLazy() { lazy_ = 0; }
    /// Some block of [addr, addr + size) may be kept back (hidden: until its write-back completed). Any thread.
    bool KeptBack(u64 addr, u64 size) const;
    bool AnyDeferred() const { return deferred_count_ != 0; }
    /// The kept-back blocks overlapping [addr, addr + size) written back in `cb` (diff against their shadow); `eager`:
    /// never kept back again (a CPU read). true: some. GPU thread.
    /// `why` (counted): 0 a GPU reader of the host import, 1 the executor's CPU-side reads and syncs, 2 the processor,
    /// 3 another thread's request (a CPU access).
    bool FlushDeferred(u64 addr, u64 size, VkCommandBuffer cb, u64 seq, bool eager, u32 why);
    /// SSPC_DIAG_MEASURE=skip_wb only: the batch's written ranges forgotten (no write-back).
    void DropWriteBack() {
        written_.clear();
        ++valid_gen_;
    }
    /// SSPC_DIAG_WB_DIRTY=1: the completed write-backs compared with the host's bytes before them (Collect calls it).
    void CheckWriteBacks(u64 completed);
    /// Before the batch is submitted: binds the memory its commands need (vkQueueBindSparse, the queue's lock held by
    /// the caller); `wait_value` of `wait` is what the batch waits for (0: nothing to wait for).
    bool SubmitBinds(VkSemaphore& wait, u64& wait_value);
    bool HasPendingBinds() const { return !pending_binds_.empty(); } ///< (R11: SubmitBinds would call the driver)
    /// FrameBreak AQ: the binds' semaphore and the value of the last binds submitted (0: none, or waited on the host) -
    /// a batch on another queue than the one after them waits for it (it may use the blocks they bound).
    VkSemaphore BindTimeline() const { return host_wait_binds_ ? VK_NULL_HANDLE : bind_timeline_; }
    u64 LastBindValue() const { return host_wait_binds_ ? 0 : bind_value_; }

    const BufferCacheStats& Stats() const { return stats_; }
    VkBuffer Buffer() const { return buffer_; }
    /// REA-1 diagnostic (SSPC_DIAG_LARGE_BINDS=1): the bindings Prepare left on the host import, by range and reason
    /// (the most frequent first); empty when off.
    std::string LargeBindReport() const;

    /// REA-2 (SSPC_REA_HEAP_READS=1): the game's vertex-data heaps (one V# over 0.5-2.5 GiB, stride 2, read-only, bound
    /// by most draws; docs/REA_ENGINE_REVERSE.md) are bound on the host import without writing back what the GPU wrote
    /// in their address range: the GPU-only data allocated inside them (the tiled lighting's 6.25 MiB light index list)
    /// stays in video memory. Host-import readings by such heaps that skipped the write-back are counted.
    bool HeapReads() const { return heap_reads_; }
    u64 HeapReadsUnflushed() const { return heap_unflushed_; }
    u64 PoisonedBlocks() const { return poisoned_blocks_; }
    static constexpr u64 kHeapMin = 256ull << 20;

private:
    void DiagHeapLayout(u64 addr, u64 size); ///< REA-4 diagnostic (SSPC_DIAG_LARGE_BINDS=1), once per heap binding
    bool heap_reads_ = false, poison_ = false;
    u64 heap_unflushed_ = 0, poisoned_blocks_ = 0;
    bool diag_large_ = false;
    /// (addr, size, reason: 0 larger than kMaxCached, 1 over budget, 2 the tracker cannot arm it, 3 not bound; write)
    std::map<std::tuple<u64, u64, u8, bool>, u64> large_binds_;
    std::map<u64, u64> diag_wb_, diag_kept_; ///< changed-words bytes / kept-back blocks by 1 MiB of address
    std::map<std::pair<u64, u64>, u64> heap_clamp_; ///< REA-7: (addr, size) of a heap binding -> its importable size (0: none)
    static constexpr u64 kBlockBits = 16; ///< 64 KiB residency blocks
    static constexpr u64 kPageBits = 12;  ///< 4 KiB validity pages (the tracker's)
    static constexpr u64 kPagesPerBlock = 1ull << (kBlockBits - kPageBits);
    static constexpr u64 kChunkBytes = 64ull << 20; ///< device memory allocations bound block by block
    /// Larger bindings (often a whole pool of which a draw reads a part) stay on the host import: caching them would
    /// upload and keep the whole pool. Every hot range measured on the reference route is below.
    static constexpr u64 kMaxCached = 16ull << 20;
    struct Block {
        u16 valid = 0; ///< pages whose video memory copy is current
        bool bound = false;
        u8 cpu_writes = 0; ///< CPU writes the tracker reported over cached pages (capped): the promotion back-off
        u16 gpu_pending = 0; ///< pages the GPU writes in submission `pending_seq` (the host's copy is older until then)
        u16 used = 0;        ///< pages a binding asked for (P4.9: uploaded together once invalid)
        u64 pending_seq = 0;
        u64 quiet_from = 0; ///< the frame from which a small read-only range here is cached again instead of streamed
        bool deferred = false; ///< FB-7: kept back (hidden): the host's copy is older until a reader writes it back
        bool eager = false;    ///< FB-7: a CPU read wanted it: never kept back again
        u32 version = 0;       ///< CITY-OPT: bumped whenever pages stop being valid or the GPU writes it
    };
    int lazy_ = 0; ///< FB-7: 0 off, 1 on, 2 verify
    u64 deferred_count_ = 0;
    std::set<u64> deferred_set_; ///< the kept-back blocks (FlushDeferred finds those of a range)
    std::vector<std::pair<u64, u64>> unhide_pending_; ///< (block address, submission): shown once it completed
    u64 frame_ = 0; ///< the frame Promotable saw last
    /// The block table: leaves of 4096 blocks (256 MiB of addresses) made when first touched - a lookup is two array
    /// indexings (it was a hash map: binding checks and scans 5.6 ms a frame for ~6200 ranges, e1).
    static constexpr u64 kLeafBits = 12;
    static constexpr u64 kLeafMask = (1ull << kLeafBits) - 1;
    struct Leaf {
        std::array<Block, 1ull << kLeafBits> blocks{};
    };
    std::vector<std::unique_ptr<Leaf>> leaves_;
    Block* FindBlock(u64 b) {
        const u64 l = b >> kLeafBits;
        return l < leaves_.size() && leaves_[l] ? &leaves_[l]->blocks[b & kLeafMask] : nullptr;
    }
    Block& GetBlock(u64 b) {
        auto& leaf = leaves_[b >> kLeafBits];
        if (!leaf) {
            leaf = std::make_unique<Leaf>();
        }
        return leaf->blocks[b & kLeafMask];
    }
    void ApplyInvalidations();
    bool EnsureBound(u64 first_block, u64 last_block);
    bool Upload(u64 addr, u64 size, VkCommandBuffer cb, u64 seq);
    void FlushWritten(u64 addr, u64 size, VkCommandBuffer cb, u64 seq);

    DeviceContext& ctx_;
    GuestBuffers& guest_;
    PageTracker* tracker_;
    u64 budget_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    u64 space_ = 0; ///< the mirrored guest addresses: [0, space_)
    u64 block_alignment_ = 1ull << kBlockBits;
    u32 memory_type_ = 0;
    std::vector<VkDeviceMemory> chunks_;
    u64 chunk_used_ = kChunkBytes; ///< bytes of the last chunk given out
    std::vector<VkSparseMemoryBind> pending_binds_;
    VkSemaphore bind_timeline_ = VK_NULL_HANDLE;
    u64 bind_value_ = 0;
    std::vector<std::pair<u64, u64>> written_; ///< this batch's GPU-written ranges [begin, end)
    struct HostWrite {
        u64 begin, end, seq;
    };
    std::vector<HostWrite> host_writes_; ///< not completed
    u64 hw_lo_ = ~0ull, hw_hi_ = 0;
    std::vector<std::pair<u64, u64>> uploads_;  ///< scratch
    std::mutex inval_mutex_;
    std::atomic<bool> has_inval_{false};
    std::vector<std::pair<u64, u64>> pending_inval_;
    std::vector<std::pair<u64, u64>> pending_coarse_; ///< InvalidateCoarse's (under inval_mutex_)
    u64 completed_ = 0;                                ///< the last completed submission Collect saw
    /// Pages a coarse report kept valid (the GPU wrote them in submission `seq`): invalid once it completes (the host
    /// then has them; their tracking protection is gone, so the next use uploads and protects them again).
    struct DeferredInval {
        u64 block;
        u16 mask;
        u64 seq;
    };
    std::vector<DeferredInval> deferred_inval_;
    /// Prepare's memo of cached ranges prepared in the batch (`seq`) while nothing invalidated pages or dropped written
    /// ranges (`gen` == valid_gen_)
    struct PrepMemo {
        u64 addr = 0, size = 0, seq = 0, gen = 0;
        bool write = false;
    };
    std::array<PrepMemo, 8> prep_memo_{};
    u32 prep_next_ = 0;
    u64 valid_gen_ = 1;
    std::vector<std::pair<VkBuffer, VkBufferCopy>> wb_regions_; ///< RecordWriteBack's scratch
    std::function<void()> wb_mark_;
    std::vector<VkBufferCopy> wb_copies_;
    struct WbSnap {
        u64 seq = 0, addr = 0;
        std::vector<u8> before;
    };
    std::deque<WbSnap> wb_snaps_; ///< SSPC_DIAG_WB_DIRTY
    /// The write-back of a large range (>= kDiffMin) by the shader wb_diff.comp: a device-local shadow per range holds
    /// what the host has (made from the host once; every upload of pages under it copies the same host bytes into it;
    /// a write the tracker does not see over it - an exact invalidation - makes it again); at the batch's end the
    /// shader writes to the host (and the shadow) only the words where the video memory copy differs from it.
    struct WbShadow {
        u64 addr = 0, size = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool valid = false;
    };
    static constexpr u64 kDiffMin = 1ull << 20;
    static constexpr size_t kMaxShadows = 32;
    static constexpr u64 kShadowBudget = 256ull << 20;
    std::vector<WbShadow> wb_shadows_; ///< (capacity reserved: pointers into it stay valid)
    u64 shadow_bytes_ = 0;
    VkDescriptorSetLayout diff_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout diff_layout_ = VK_NULL_HANDLE;
    VkPipeline diff_pipeline_ = VK_NULL_HANDLE;
    void InitDiff();
    /// One changed-words dispatch: [a, a + piece) of the video memory copy against `sh` into the host view at
    /// (`host`, `host_offset`) (the pipeline bound by the caller).
    void DiffPiece(VkCommandBuffer cb, u64 a, u64 piece, const WbShadow& sh, VkBuffer host, u64 host_offset);
    void CopyIntoShadows(u64 a, u64 size, VkCommandBuffer cb);
    /// The shadow of exactly [b, e) (made, or its host bytes copied in when not in step: `inited`); nullptr: none.
    WbShadow* ShadowFor(u64 b, u64 e, VkCommandBuffer cb, u64 seq, bool& inited);
    bool host_wait_binds_ = false; ///< the binds complete on the host (validation runs; SSPC_DIAG_BIND_HOST_WAIT=1)
    /// P4.9-B (on by default since 2026-10-14; SSPC_P49_BLOCK_UPLOAD=0: off): a block's first missing page brings the
    /// block's other used, invalid pages along - village GPU busy -7 ms, 26.4 -> 31.3 FPS with the parallel command
    /// processor (docs\PHASE4_P49_*.md, docs\SMOKE_QUALITY.md)
    bool block_upload_ = true;
    void (*before_upload_)(void*) = nullptr;
    void* before_upload_ctx_ = nullptr;
    void (*after_dispatch_)(void*) = nullptr;
    void* after_dispatch_ctx_ = nullptr;
    BufferCacheStats stats_;
};

} // namespace SecondSon::Gpu::Vk
