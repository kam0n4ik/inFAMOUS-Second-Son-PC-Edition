// SecondSonPC — the backend's executor: the GPU side of SecondSonPC's command processor (Phase 4, P4.4;
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §D, §E). Implements Cp::GpuWork on the backend's GPU thread: every draw
// and dispatch the processor hands on is prepared by the shader frontend (P4.3), recorded into the open batch (a command
// buffer that signals the next value of the timeline), submitted at fences, flips, synchronization points or when full;
// once the GPU completed a batch, the processor learns it (CompleteWork) and writes the held end-of-pipe labels — never
// before. Work the backend cannot execute (an unsupported stage, format, resource) is counted by reason and still
// completed in order (its fences must come), never replaced by something else.
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_resources.h"

namespace SecondSon::Gpu::Shader {
class ShaderFrontend;
struct StageView;
struct DrawPreparation;
struct GraphicsPipelineState;
}

namespace SecondSon::Gpu::Vk {

class Diagnostics;

class DeviceContext;
class Timeline;
class StreamRing;
class GuestBuffers;
class ImageCache;
class Presenter;
class ImageReadback;
struct CachedImage;
struct OwnedBuffer;

enum class Skip : u8 {
    NotReady,        ///< the frontend has no module/pipeline for it (pending, failed, no program)
    Unsupported,     ///< a stage SecondSonPC does not prepare (GS/HS/...)
    Pipeline,        ///< pipeline creation failed
    Buffer,          ///< a buffer's guest memory cannot be read by the GPU
    SpecialBuffer,   ///< BDA page table / fault buffer (shader DMA): not supported yet
    Image,           ///< an image could not be made
    VertexFormat,    ///< a vertex attribute has no host format
    Target,          ///< a colour/depth target could not be made
    FilteredMode,    ///< fast clear elimination, FMASK decompression, primitive None: no draw on the hardware either
    MetaClear,       ///< a compute clear of CMASK/HTILE: recorded as a pending clear of its target
    Ring,            ///< the stream ring could not hold the draw's data
    Count,
};
std::string_view SkipName(Skip s);

struct ExecutorStats {
    u64 draws = 0, draws_executed = 0, dispatches = 0, dispatches_executed = 0;
    std::array<u64, static_cast<size_t>(Skip::Count)> skipped{};
    u64 batches = 0, flushes_fence = 0, flushes_flip = 0, flushes_sync = 0, flushes_full = 0, flushes_idle = 0;
    u64 syncs = 0, sync_wait_us = 0, flips = 0, flips_blank = 0, render_passes = 0, global_barriers = 0;
    u64 completed_batches = 0, cache_actions = 0, dma = 0, fast_clears = 0, depth_clears = 0;
    u64 depth_clears_partial = 0; ///< DB_RENDER_CONTROL clears of part of the target (the draw's viewport/scissor)
    u64 indexed = 0, indirect = 0, vertex_bindings = 0, descriptor_sets = 0;
    u64 record_ns = 0; ///< CPU time recording draws/dispatches (TSC)
    u64 timeline_writes = 0, timeline_read_syncs = 0, uploads_ended_rendering = 0;
    u64 meta_writes = 0, meta_writes_xor = 0, meta_writes_reading = 0; ///< compute writes of known targets' metadata
    u64 gpu_busy_ns = 0, gpu_batches_timed = 0; ///< the GPU's time executing batches (timestamps)
    u64 submit_ns = 0;                          ///< CPU time in vkQueueSubmit2
    u64 lds_dispatches = 0, lds_max_bytes = 0;  ///< dispatches with the emulated LDS buffer
    u64 diag_skipped_draws = 0, diag_skipped_dispatches = 0; ///< SSPC_DIAG_SKIP_DRAWS / _DISPATCHES (diagnostic)
    u64 merge_split_dispatches = 0; ///< the particle sort's dispatches recorded one width each (SplitMergeLevels)
    u64 streamed = 0, streamed_bytes = 0; ///< small read-only ranges the CPU rewrote, copied into the stream ring
    u64 ring_mirror_copies = 0;           ///< hybrid ring: host -> video memory copies at the head of submissions
    u64 stream_promotions = 0;            ///< small read-only ranges cached instead of streamed (BufferCache::Promotable)
    u64 binds_skipped = 0, push_constants_skipped = 0; ///< draws repeating the bound pipeline / pushed constants
    /// SSPC_DIAG_STREAM_STATS: streamed ranges first seen / again in the frame unchanged / changed / unchanged since an
    /// earlier frame / changed since (count, bytes)
    std::array<u64, 5> stream_kinds{}, stream_kind_bytes{};
    u64 t_front = 0, t_bind = 0, t_range = 0, t_image = 0; ///< TSC ticks: frontend, resource binding (of it buffers, images)
    /// TSC ticks of a draw's other stages: pipeline lookup, targets, vertex/index/indirect ranges, barriers and layouts,
    /// the commands (rendering, state, descriptors, the draw); the dispatches' whole recording
    u64 t_pipe = 0, t_targets = 0, t_vertex = 0, t_layout = 0, t_emit = 0, t_dispatch = 0;
    u64 t_classify = 0, t_hostptr = 0, t_alloc = 0, t_copy = 0, t_prepare = 0; ///< TSC ticks inside GuestRange
    u64 n_prepare = 0;
    u64 tw_fail_shape = 0, tw_fail_import = 0; ///< WriteOnTimeline refusals: size/alignment/address, no import
    u64 timeline_copies = 0, timeline_copy_bytes = 0, tc_refused = 0;
    u64 timeline_gds = 0, gds_refused = 0; ///< DMA_DATA to/from GDS on the timeline (P4.6)
    u64 shadows = 0, shadow_bytes = 0, shadow_overlaps = 0, shadow_no_room = 0, shadow_too_large = 0;
    u64 target_cache_hits = 0, target_cache_misses = 0;
    u64 tex_memo_hits = 0, tex_memo_misses = 0;
    u64 gpu_write_read_syncs = 0; ///< frontend reads of memory a shader writes in a batch not completed
    /// P4.7 frontend reads: from a backing run kept / a run found for it / through the guest memory manager
    u64 read_run_hits = 0, read_run_misses = 0, read_run_none = 0;
    u64 read_run_verified = 0, read_run_mismatches = 0; ///< SSPC_DIAG_READ_RUNS=verify
    /// P4.7 binding shadow: graphics descriptors wanted / pushed, pushes left out (nothing differed), vertex and index
    /// buffer bindings left out (the bound ones)
    u64 desc_wanted = 0, desc_pushed = 0, desc_push_skipped = 0, vb_binds_skipped = 0, ib_binds_skipped = 0;
    /// P4.7: streamed ranges / flattened user data that took the batch's earlier slice of the same bytes
    u64 stream_reused = 0, stream_reused_bytes = 0;
    /// SSPC_DIAG_STREAM_REUSE=verify: reuses copied again anyway / those whose bytes differed from the earlier slice
    u64 stream_verified = 0, stream_mismatches = 0;
    /// met WAIT_REG_MEM on memory (of them: had blocked; the CPU had written tracked memory since the last drain - before
    /// P4.5 the work after the wait read the older copies)
    u64 acquires = 0, acquires_blocked = 0, acquires_applied = 0;
    u64 waits_by_held = 0; ///< WAIT_REG_MEM met in the GPU's order by a held fence (a barrier before the next command)
    std::array<u64, 9> gb_sites{}; ///< global barriers by GlobalBarrier site
    u64 barriers_elided = 0; ///< dispatches/timeline copies after writes that touched none of their ranges: no barrier
    u64 barriers_war = 0;    ///< barriers for a write over a range read since the last one (no write pending)
    /// barriers kept by NeedsBarrier, why: off, an unknown write pending, the command's own unknown accesses, too many
    /// ranges, read-after-write, write-after-write, write-after-read
    std::array<u64, 7> barrier_why{};
    u64 barrier_waw_gds = 0; ///< of the write-after-write ones: both write GDS
    std::array<u64, 64> guest_events{}; ///< the guest's EVENT_WRITE by event type (63: ACQUIRE_MEM)
    /// dispatches barriered for a range conflict: the guest flushed on that queue since its previous dispatch / did not
    u64 conflict_guest_flushed = 0, conflict_guest_silent = 0;
    u64 barriers_guest_order = 0; ///< conflicts between dispatches the guest does not order (SSPC_P46_GUEST_ORDER)
    u64 clock_label_binds = 0;    ///< SSPC-020 diagnostics: shader bindings over a GPU clock label
    u64 clock_fences_placed = 0, clock_fences_unplaced = 0; ///< held clock fences with/without a timestamp at their place
    std::array<u64, 7> unknown_by{}; ///< writes of unknown extent by source (Executor::MarkWrites)
    std::array<u64, 3> wb_bytes_by{}; ///< write-back bytes by batch end: fence/flip/sync/idle, full+fence, full only
    std::array<u64, 6> shadow_sizes{}; ///< written dispatch bindings: <=64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB, larger
    std::array<u64, 3> sync_eff{}, sync_eff_us{}; ///< waits that did wait: processor packets, frontend reads, processor reads
    /// P4.10: TSC ticks in the processor's callbacks other than draws and dispatches (fences, cache actions, DMA, timeline
    /// writes/copies, waits met, flips, Synchronize) - the processor's own time is its rounds minus these and recording
    u64 t_cb = 0, n_cb = 0;
    u64 prepared_mismatches = 0; ///< P4.10: a draw/dispatch without the preparation its registers ask for (never)
    std::string first_skip[static_cast<size_t>(Skip::Count)];
};

struct ExecutorOptions {
    u64 capture_flip = 0;                 ///< --gpu-capture-native-frame N: the presented frame of flip N to PNG
    std::filesystem::path capture_path;   ///< where (native_frame.png)
    /// A batch is submitted after this many draws: the GPU works on a frame while the rest is recorded (e3-e5: 4000 -
    /// the frame's last batch ran on the GPU after recording, 19 ms idle a frame; 500 - 12 ms, 11.5 -> 12.6 FPS; 150 -
    /// no better, submissions cost more)
    u32 max_draws_per_batch = 500;
};

/// P4.10: what the shader frontend prepared for one draw or dispatch, handed to the executor's recording (made by the
/// frontend on the same thread, or a copy the parallel command processor made on its thread at the command's packet).
/// prep nullptr: none (a filtered draw; a dispatch without compute registers).
struct PreparedCommand {
    const GS::DrawPreparation* prep = nullptr;
    const GS::StageView* stage = nullptr; ///< VS of a draw, CS of a dispatch
    const GS::StageView* ps = nullptr;    ///< a draw's PS
    const GS::GraphicsPipelineState* state = nullptr; ///< a ready draw's pipeline state
};

class Executor final : public Cp::GpuWork {
public:
    Executor(DeviceContext& ctx, Timeline& timeline, StreamRing& ring, GuestBuffers& guest, BufferCache& buffers,
             ImageCache& images, PipelineFactory& pipelines, GS::ShaderFrontend& frontend, Presenter* presenter,
             OwnedBuffer& gds, const ExecutorOptions& options);
    ~Executor() override;

    /// The processor whose work this executes (its registers are read at each draw; CompleteWork is called on it).
    void Attach(Cp::CommandProcessor* cp) {
        cp_ = cp;
        regs_ = cp != nullptr ? &cp->Registers() : nullptr;
    }
    /// P4.10: the register file draws read instead of the processor's (the parallel command processor's replica, which
    /// holds the registers as they were at the draw's packet while the processor already reads later ones).
    void SetRegisters(const Cp::RegisterFile* regs) { regs_ = regs; }
    /// The command journal, markers and the device loss report (the backend always sets it; --gpu-native-diag turns
    /// the journal on).
    void SetDiagnostics(Diagnostics* diag) { diag_ = diag; }
    /// VK_ERROR_DEVICE_LOST was seen: nothing more is recorded or submitted (the backend stops).
    bool DeviceLost() const { return device_lost_; }
    bool Failed() const;
    /// Compute queue -> its Gnm pipe (interrupts of RELEASE_MEM name the pipe).
    void SetQueuePipe(u32 queue, u32 pipe);

    // Cp::GpuWork (the GPU thread, inside CommandProcessor::Run)
    void Draw(const Cp::DrawDescriptor& draw) override;
    void Dispatch(const Cp::DispatchDescriptor& dispatch) override;
    /// P4.10: a dispatch with its queue's compute registers as they were at its packet (`cs` nullptr: none).
    void DispatchWith(const Cp::DispatchDescriptor& dispatch, const Cp::ComputeRegisters* cs);
    /// P4.10: a draw / dispatch the frontend already prepared (on the processor's thread): only recorded here.
    void DrawPrepared(const Cp::DrawDescriptor& draw, const PreparedCommand& prepared);
    void DispatchPrepared(const Cp::DispatchDescriptor& dispatch, const Cp::ComputeRegisters* cs,
                          const PreparedCommand& prepared);
    /// A draw the hardware does not draw (fast clear elimination, FMASK decompression, primitive None).
    static bool DrawFiltered(const Cp::RegisterFile& regs, const Cp::DrawDescriptor& draw);
    /// P4.10: the frontend's memo is told about guest memory changes from here (true) or not (false: the parallel
    /// command processor's thread, which runs the frontend, tells it in stream order itself).
    void SetMemoBumps(bool on) { memo_bumps_ = on; }
    /// P4.10: a frontend read of [addr, addr + bytes) made elsewhere (the queue drained): what TimelineReader does
    /// first - a write still on the GPU's timeline or a shader's write the host does not have lands (true: it waited).
    bool SyncForFrontendRead(u64 addr, u64 bytes);
    /// The same check without the wait: such a write is pending over [addr, addr + bytes).
    bool FrontendReadPending(u64 addr, u64 bytes);
    /// P4.10: the timeline value everything recorded so far completes at (the open batch's, else the last submitted).
    u64 RecordedValue() const;
    void CacheAction(const Cp::CacheActionOp& op) override;
    void Flip(const Cp::FlipMarker& marker) override;
    /// P5.3D A/B: images at those addresses now -> SSPC_DIAG_CAPTURE_DIR (CaptureImageAt's output, pass < 0 there).
    void CaptureTargets(const u64* addresses, u32 count, s32 pass, u32 native_draws, u32 pm4_draws) override;
    void Fence(const Cp::FenceOp& op) override;
    void Dma(const Cp::DmaOp& op) override;
    void Interrupt(u32 queue, Cp::FenceKind kind) override;
    void Synchronize(u32 queue) override;
    bool WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) override;
    bool CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) override;
    bool GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                       u32 pattern) override;
    void MemoryAcquired(u32 queue, bool blocked, u64 poll_address) override;
    void WaitMetByHeldFence(u32 queue) override;
    /// What MemoryAcquired runs: the backend applies the guest's CPU writes seen so far to the caches (true: some).
    void SetAcquireHook(bool (*hook)(void*), void* ctx) {
        acquire_hook_ = hook;
        acquire_ctx_ = ctx;
    }
    /// P4.11 (SSPC_P411_ASYNC_ACQUIRE=1, the parallel processor with its frontend): MemoryAcquired split in two - the
    /// hook as MemoryAcquired would run it (SSPC_DIAG_ACQUIRE_DRAIN=off: not at all), on the processor's thread while
    /// the recording thread executes earlier records (it touches no executor state: the backend's hook takes the
    /// tracker, the caches' locked invalidation queues and the frontend); then, in stream order on the recording thread,
    /// the rest (the guest's ordering point, the counts; `applied`: the hook found writes; `unblock_tsc`: when the wait
    /// was met, for a blocked one).
    bool RunAcquireHook() const;
    void MemoryAcquiredApplied(u32 queue, bool blocked, u64 poll_address, u64 unblock_tsc, bool applied);
    /// TSC of the last met WAIT_REG_MEM on memory that had blocked (the frame log, P4.6).
    u64 LastUnblockTsc() const { return last_unblock_tsc_; }
    /// P4.11 soak telemetry: command buffer batches the executor owns (free and in flight).
    u64 BatchObjects() const { return free_.size() + in_flight_.size() + (open_ ? 1 : 0); }
    /// Called at each flip marker queued for presentation (after its batch was submitted), with its timeline value.
    void SetFlipHook(void (*hook)(void*, u64), void* ctx) {
        flip_hook_ = hook;
        flip_ctx_ = ctx;
    }
    /// Dense video memory for the written buffers of dispatches (see PrepareShadows); null: they write in place.
    void SetWriteShadowRing(StreamRing* ring) { shadow_ring_ = ring; }
    /// Synchronize for a CPU read of guest memory by the command processor (its own memory reads).
    void SyncFromMemoryRead() { SyncFrom(2); }
    /// The command processor wrote guest memory, or the guest submitted (its CPU may have written anything before):
    /// the frontend's memoized stage preparations are not taken again (P4.7).
    void GuestMemoryChanged();
    /// P4.7: the backend's generation of guest maps (bumped by every map/unmap, any thread): the frontend's reads keep
    /// backing runs while it is unchanged. nullptr: every read through the guest memory manager.
    void SetMapGeneration(const std::atomic<u64>* gen);

    /// A write recorded on the GPU's timeline that has not completed overlaps [addr, addr + bytes): a CPU read of that
    /// memory has to Synchronize first (the command processor's reads, the frontend's descriptor reads).
    bool TimelineWritePending(u64 addr, u64 bytes) const;
    /// P4.10.4: those writes' ranges, and a number that moves whenever they change (the parallel command processor's
    /// snapshot for the processor's reads while draws are recorded on the other thread).
    void TimelineWriteRanges(std::vector<std::pair<u64, u64>>& out) const;
    u64 TimelineWritesVersion() const { return tw_version_; }
    u64 TimelineWritesCount() const { return timeline_writes_.size(); } ///< (P4.11 soak telemetry)
    /// P4.10.4: work number `seq` of `queue` was taken by a write/copy the processor believed recorded and the CPU did
    /// instead (after Synchronize): it completes with the next batch (its fences come).
    void CompleteRefusedWork(u32 queue, u64 seq);

    /// Submits the open batch (nothing when empty).
    void Flush();
    /// Completed batches -> CommandProcessor::CompleteWork (in order), resources released. The GPU thread, outside
    /// CommandProcessor::Run.
    void Poll();
    /// A fence was held since the last Flush (the processor waits for the GPU): the loop flushes.
    bool WantsFlush() const { return want_flush_; }
    bool HasOpenWork() const { return open_ && commands_ > 0; }
    bool Busy() const { return !in_flight_.empty(); }
    /// Waits up to `timeout_ns` for the oldest batch in flight; true when one completed.
    bool WaitOldest(u64 timeout_ns);

    const ExecutorStats& Stats() const { return stats_; }

    /// P4.8 Performance Inspector (SSPC_INSPECTOR): the draws and dispatches classified by effect category and counted
    /// (recording CPU time included); GPU timestamps at category changes in every `gpu_every`-th frame (0: none), read
    /// once the batch completed; each frame's GPU time handed to Inspector::GpuFrame. nullptr: all of it off.
    void SetInspector(const Inspector::Catalog* catalog, u32 gpu_every);
    const Inspector::CategoryTotals& InspectorCategories() const { return insp_cat_; }
    /// The run's draws by pipeline and dispatches by program (CSV, Inspector::WriteFile("pipelines.csv")).
    std::string InspectorPipelines() const;

private:
    friend struct StabilityTestAccess;
    struct Batch {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBuffer pre = VK_NULL_HANDLE; ///< recorded at submission, runs first: the stream ring's mirror copies
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        u64 value = 0;
        std::array<u64, Cp::kNumQueues> work{};
        u32 queues_mask_lo = 0; ///< queues with work (bit 0 graphics, 1.. compute; up to 31)
        u32 query_slot = ~0u;   ///< its two timestamps (GPU time of the batch)
        u32 prof_base = ~0u;    ///< SSPC_GPU_PROFILE: its first query in the profiler's pool
        u32 prof_count = 0;
        std::vector<u64> prof_keys; ///< the command key of each timestamp after the first
        std::vector<u32> queues;
        /// held clock fences (data_sel 3/4) recorded in it: the timestamp query at each one's place (SSPC-020)
        struct ClockFence {
            u32 queue = 0;
            u64 packet_addr = 0, after_seq = 0;
            u32 query = 0;
        };
        std::vector<ClockFence> clock_fences;
        u32 clock_base = ~0u; ///< its block of clock_pool_ queries
        // P4.8 Performance Inspector: a timestamp at each change of effect category (insp_slots: the slot of the
        // interval each one starts), the frame (flip hook count) it was recorded in
        u32 insp_base = ~0u, insp_count = 0;
        std::vector<u8> insp_slots;
        u8 insp_last = 0xFF;
        bool insp_overflow = false;
        u32 insp_frame = 0;
        // P4.12 stall trace: the submission (TSC before the queue's lock, after vkQueueSubmit2), the frame recorded in
        u64 submit_t0 = 0, submit_t1 = 0;
        u32 trace_frame = 0;
    };
    struct RenderKey {
        std::array<VkImageView, 8> color{};
        u32 color_count = 0;
        VkImageView depth = VK_NULL_HANDLE;
        bool has_depth = false, has_stencil = false;
        u32 width = 0, height = 0, layers = 1;
        bool operator==(const RenderKey&) const = default;
    };
    /// A draw's render targets are built from these alone (the registers' group versions, the values of the groups
    /// that change per draw for other reasons, the pipeline's MRT mask, the driver's size hints, the images' epoch).
    struct TargetInputs {
        std::array<u32, 4> versions{}; ///< ColorFormat, ColorTarget, DepthFormat, DepthTarget
        u32 mode = 0, target_mask = 0, depth_control = 0, mrt_mask = 0;
        std::array<u32, 8> cb_extent{};
        u32 db_extent = 0;
        u64 epoch = 0;
        bool operator==(const TargetInputs&) const = default;
    };
    struct TargetCache {
        bool valid = false;
        TargetInputs in;
        std::vector<CachedImage*> targets;
        RenderKey key;
        std::array<CachedImage*, 8> color_img{};
        CachedImage* depth_img = nullptr;
        std::array<u64, 8> cmask{}; ///< each colour target's CMASK address (a pending fast clear is checked per draw)
        u64 htile = 0;
    };
    TargetCache target_cache_;
    struct Scratch {
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> buf_infos;
        std::vector<VkDescriptorImageInfo> img_infos;
        std::vector<CachedImage*> bound, targets;
        std::vector<VkVertexInputBindingDescription2EXT> vbind;
        std::vector<VkVertexInputAttributeDescription2EXT> vattr;
        std::vector<VkBuffer> vbufs;
        std::vector<VkDeviceSize> voffs;
    };
    Scratch scratch_; ///< one command's lists (Draw/Dispatch do not nest)
    /// The dynamic state set in the batch's command buffer so far: a draw sets only what changed (every graphics
    /// pipeline has the same dynamic states, so binding one keeps them; a new command buffer starts undefined).
    /// ~25 vkCmdSet* a draw were 4.7% of the GPU thread's samples (f2).
    struct DynShadow {
        bool valid = false;
        u32 vp_count = 0;
        std::array<VkViewport, 16> vps{};
        std::array<VkRect2D, 16> scs{};
        VkBool32 depth_test = 0, depth_write = 0, bounds_test = 0, bias_enable = 0, stencil_test = 0, restart = 0,
                 discard = 0;
        VkCompareOp depth_op = VK_COMPARE_OP_NEVER;
        float bounds[2] = {}, bias[3] = {}, blend[4] = {}, line_width = 0.0f;
        bool bias_set = false;
        struct Face {
            VkStencilOp fail = VK_STENCIL_OP_KEEP, pass = VK_STENCIL_OP_KEEP, depth_fail = VK_STENCIL_OP_KEEP;
            VkCompareOp compare = VK_COMPARE_OP_NEVER;
            u32 reference = 0, write_mask = 0, compare_mask = 0;
            bool operator==(const Face&) const = default;
        };
        Face front, back;
        VkCullModeFlags cull = 0;
        VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        u32 color_masks_count = 0;
        std::array<VkColorComponentFlags, 8> color_masks{};
        std::vector<VkVertexInputBindingDescription2EXT> vbind;
        std::vector<VkVertexInputAttributeDescription2EXT> vattr;
    };
    DynShadow dyn_;
    /// A texture's T# (with the binding's depth/array/storage) resolved before: its image and view, while the image
    /// cache's epoch is the same and the image needs no upload - LayoutFromTSharp, Find and View for the same textures
    /// draw after draw were ~4.5% of the GPU thread's samples (i1).
    struct TexMemo {
        std::array<u32, 8> raw{};
        u32 flags = 0;
        u64 epoch = ~0ull;
        CachedImage* img = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::unordered_map<u64, TexMemo> tex_memo_;
    /// P4.9: tex_memo_'s entries looked up last, direct-mapped by key (never erased: the pointers stay valid) - the map's
    /// hashing on every texture of every draw was ~1.1% of the GPU thread (village); SSPC_P49_LOOKUP_MEMO=0: off
    std::array<std::pair<u64, TexMemo*>, 256> tex_slot_{};
    struct StreamSeen {
        u64 hash = 0, flip = 0;
    };
    std::unordered_map<u64, StreamSeen> stream_seen_; ///< SSPC_DIAG_STREAM_STATS
    // what this command buffer has bound/pushed last (reset by Begin): repeats are not recorded again
    VkPipeline gfx_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout push_layout_ = VK_NULL_HANDLE; ///< the layout of the last push constants (either bind point)
    PushData push_last_{};
    // P4.7: what this command buffer has pushed for graphics descriptors and bound as vertex/index buffers - a draw
    // records only the descriptors that differ (push descriptors are updated incrementally and stay valid until a push
    // with another layout) and no buffer binding it repeats. Off unless SSPC_DIAG_BIND_SHADOW=on: it pushes ~64% fewer
    // descriptors on the beach, but the A/B (3 pairs) found no time saved (docs\PHASE4_FOLIAGE_OPTIMIZATION.md).
    struct DescShadow {
        bool valid = false;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        VkDescriptorBufferInfo buffer{};
        VkDescriptorImageInfo image{};
    };
    bool bind_shadow_ = false;
    // P4.7: small ranges streamed in this batch, by range: the same range again with no guest memory change since
    // takes the slice it got (57% of the streamed copies on the beach were the same bytes again in the frame - one mesh
    // drawn into each shadow cascade and the main pass). Same batch only: a slice lives until its batch completes.
    // SSPC_DIAG_STREAM_REUSE=off: every range copied again.
    struct StreamMemo {
        u64 addr = 0, size = 0, batch = 0, gen = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0;
        u64 hash = 0; ///< SSPC_DIAG_STREAM_REUSE=verify: of the bytes copied
    };
    static constexpr u32 kStreamMemo = 4096;
    bool stream_reuse_ = true;
    bool stream_verify_ = false; ///< SSPC_DIAG_STREAM_REUSE=verify: every reuse copied again, the bytes compared
    bool read_verify_ = false;   ///< SSPC_DIAG_READ_RUNS=verify: every run read through the manager too, compared
    std::vector<StreamMemo> stream_memo_;
    VkPipelineLayout desc_layout_ = VK_NULL_HANDLE; ///< the layout of the graphics push descriptors in desc_shadow_
    std::vector<DescShadow> desc_shadow_;            ///< by binding
    std::vector<VkWriteDescriptorSet> desc_changed_; ///< (capacity reused)
    std::vector<VkBuffer> vb_last_;
    std::vector<VkDeviceSize> vb_off_last_;
    bool vb_valid_ = false;
    VkBuffer ib_last_ = VK_NULL_HANDLE;
    u64 ib_off_last_ = 0;
    VkIndexType ib_type_last_ = VK_INDEX_TYPE_MAX_ENUM;
    bool dyn_vertex_valid_ = false;
    u64 dyn_skipped_ = 0, dyn_set_ = 0;

    bool Begin();
    void RecordDraw(const Cp::DrawDescriptor& d, const PreparedCommand& pc, u64 t0);
    void RecordDispatch(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs, const PreparedCommand& pc,
                        u64 t0);
    bool memo_bumps_ = true;
    void Note(u32 queue, u64 seq);
    void EndRendering();
    /// `site`: who asked (counted): 0 other, 1/2 shadows in, 3 shadows out, 4 timeline write, 5 timeline copy, 6 upload,
    /// 7 before a draw, 8 before a dispatch
    void GlobalBarrier(u32 site = 0);
    void Skipped(Skip why, const std::string& detail);
    /// `user_data`: the draw's 16 user data registers of the stage (P5.3: a shared preparation's flattened user data
    /// gets them in place of the registers of the draw that made it); nullptr for a dispatch.
    bool BindStage(const void* stage_view, bool compute, PushData& push, std::vector<VkWriteDescriptorSet>& writes,
                   std::vector<VkDescriptorBufferInfo>& buffers, std::vector<VkDescriptorImageInfo>& images,
                   std::vector<CachedImage*>& bound, bool& writes_memory, const std::vector<CachedImage*>& targets,
                   const u32* user_data = nullptr);
    bool PushDescriptors(const PipelineObjects& p, VkPipelineBindPoint bind, std::vector<VkWriteDescriptorSet>& writes);
    void SetDynamicState(const Cp::RegisterFile& regs, const GS::GraphicsPipelineState& st, bool indexed);
    void RecordFrameCopy();
    /// SSPC_DIAG_CAPTURE_FRAMES: the displayed image of this flip read back (recorded after its copy; P4.5).
    void CaptureSeries(const CachedImage& img, VkFormat shown, u32 w, u32 h);
    std::unique_ptr<ImageReadback> readback_;
    /// SSPC_DIAG_CAPTURE_IMAGE: a GPU-written image read back at a flip (P4.5)
    void CaptureImageAt();
    /// SSPC_DIAG_DRAW_HISTO: the frame's draws by pipeline / dispatches by program, written at the flip (P4.5)
    void DrawHistogram();
    struct HistoEntry {
        u64 count = 0, indirect = 0, instances = 0, verts = 0;
        std::string legend;
    };
    struct Histo {
        bool active = false;
        std::unordered_map<u64, HistoEntry> draws, dispatches;
        std::unordered_set<u64> legend_done;
        std::vector<std::string> detail;
        std::ofstream out, args;
        u64 last_flip_tsc = 0, last_submit_tsc = 0;
    };
    struct ArgsReadback {
        u64 value, flip, addr;
        OwnedBuffer buffer;
    };
    std::deque<ArgsReadback> args_rb_;
    /// SSPC_DIAG_PROBE=<vs|ps hex address or hash>:<first flip>:<flips>:<bindings>[:<max bytes>] (smoke research): at the
    /// matching draw, the listed guest buffer bindings of its VS ('+'-separated indices in the order of the command
    /// trace's R0 ranges; 'a': the indirect arguments) read back as the draw reads them ->
    /// logs\smoke_research\probe\f<flip>_<binding>_<address>.bin
    struct ProbeReadback {
        u64 value, size;
        std::string name;
        OwnedBuffer buffer;
    };
    std::deque<ProbeReadback> probe_rb_;
    void Probe(const Cp::DrawDescriptor& d, const GS::StageView& vs, const GS::StageView* ps);
    /// SSPC_DIAG_PROBE_CS (same form): the bindings of a matching dispatch before and after it.
    void ProbeDispatch(const Cp::DispatchDescriptor& d, const GS::StageView& cs, bool after);
    /// SSPC-020: a dispatch of the particle sort (merge path, docs\SMOKE_SHADER_MAP.md section 4) that merges several
    /// widths in one go (params w, max with 2w <= max: a width reads what other threads of the same dispatch wrote at the
    /// one before, without a barrier - in step on the PS4, a race here) recorded as one dispatch per width with a barrier
    /// between them; the same levels in the same order. true: recorded (instead of the dispatch). SSPC_DIAG_SORT_SPLIT=off:
    /// as the game records it.
    bool SplitMergeLevels(const Cp::DispatchDescriptor& d, const GS::StageView& cs, const PipelineObjects& pipe,
                          PushData& push, std::vector<VkWriteDescriptorSet>& writes,
                          std::vector<VkDescriptorBufferInfo>& infos);
    u32 split_logged_ = 0;
    void ProbeCopies(const std::string& prefix, const GS::StageView& stage, const std::vector<s32>& bindings,
                     u64 max_bytes, u64 args_addr, u64 args_size);
    Histo histo_;
    /// SSPC_DIAG_RACE: streamed ranges re-read at the flip (P4.5)
    void RaceCheck();
    static u64 RaceHash(const u8* p, u64 n);
    struct RaceCopy {
        u64 addr, size, hash;
    };
    struct RaceRange {
        u64 count = 0, size = 0;
    };
    struct Race {
        bool on = false;
        std::vector<RaceCopy> copies;
        std::unordered_map<u64, RaceRange> by_range;
        std::unordered_map<u64, u64> post; ///< a changed range's value at the flip
        u64 total = 0, changed = 0, frames = 0, frames_changed = 0, next_equal = 0, next_differ = 0;
        u64 watch_logged = 0, watch_copy_tsc = 0;
        std::vector<std::pair<u64, u64>> acquires; ///< (TSC, poll address) of this frame's met memory waits
        u32 watch_copy_hits = 0;
    };
    Race race_;
    void BeforeUpload();
    void OnDeviceLost(const char* where, u64 value);
    void SyncFrom(u32 source);
    /// A dispatch's written guest buffers moved to dense shadows: their contents copied in (recorded now, outside
    /// rendering, after the earlier writes); `infos` point at the shadows. FinishShadows copies them back after it.
    void PrepareShadows(const void* plan, std::vector<VkDescriptorBufferInfo>& infos, u64 workgroups);
    void FinishShadows();
    struct Shadow {
        VkBuffer home, shadow;
        u64 home_offset, shadow_offset, size;
    };
    std::vector<Shadow> shadows_;
    std::vector<u32> buf_adjust_; ///< BindStage: each guest buffer's offset from its aligned descriptor offset
    StreamRing* shadow_ring_ = nullptr;
    void DiagDumpShader(u64 address, const std::vector<u32>* code, const std::vector<u32>& spirv,
                        const GS::RuntimeParams& runtime, u32 pipeline);
    std::unordered_set<u64> dumped_shaders_; ///< SSPC_DIAG_DUMP_SHADER: the variants written
    void DiagCaptureBefore(u32 pipeline, const std::array<CachedImage*, 8>& color_img);
    void DiagCaptureDraw(u32 pipeline, const std::array<CachedImage*, 8>& color_img, bool after);
    /// Smoke research (docs/PHASE4_SMOKE_PIPELINE.md): SSPC_DIAG_CMD_TRACE=<first flip | t<seconds>>:<flips> - every
    /// draw and dispatch of those flips, and every one not executed, in recording order with the programs' stable
    /// identities (PGM address, footer hash, content hash), targets, states, textures and guest ranges, kept in memory
    /// and written once after the last flip -> logs\smoke_research\cmd_trace.txt. SSPC_DIAG_SKIP_DRAWS /
    /// SSPC_DIAG_SKIP_DISPATCHES=<spec>[,<spec>...] (vs:|ps:|cs:<hex address or hash>, p:<pipeline>, rt:<hex target>,
    /// n:<first>-<last> ordinal in the frame): those commands not executed - only the draw/dispatch command itself, after
    /// its render pass, barriers, clears, state and uploads were recorded (diagnostic only; never a configuration).
    struct CmdTraceEntry {
        u64 frame = 0, seq = 0, packet = 0, batch = 0, barriers = 0;
        u64 address[2]{}, footer[2]{}, content[2]{}; ///< VS/PS (CS in [0])
        u64 args = 0, depth = 0;
        std::array<u64, 8> rt{};
        std::array<u32, 8> rt_fmt{};
        u32 ordinal = 0, pipeline = 0, prim = 0, count = 0, instances = 0, blend0 = 0, depth_control = 0;
        u32 groups[3]{};
        u32 ranges_begin = 0, ranges_count = 0, images_begin = 0, images_count = 0, skip_text = 0;
        u8 queue = 0;
        char kind = 'D';   ///< D draw, X dispatch, S not executed (skip_text: why)
        u8 skip = 0;       ///< S: the Skip; D/X: 1 when SSPC_DIAG_SKIP_* left the command out
        bool indexed = false, indirect = false;
    };
    struct CmdTraceRange {
        u64 address, size;
        u8 stage;          ///< 0 VS/CS, 1 PS, 2 vertex buffer, 3 index buffer, 4 indirect arguments
        bool written;
    };
    struct CmdTraceState {
        bool on = false, by_time = false, written = false;
        u64 first = 0, flips = 0, frame = 0;
        u32 ordinal = 0;
        std::vector<CmdTraceEntry> entries;
        std::vector<CmdTraceRange> ranges;
        std::vector<u64> images; ///< textures (guest address << 8 | pixel format low bits)
        std::vector<std::string> skip_texts;
        bool Active() const { return on && !by_time && frame >= first && frame < first + flips; }
    };
    struct CmdFilter {
        char what;         ///< v, p (PS), c, P (pipeline), r (target), n (ordinal range)
        u64 value, last;
    };
    CmdTraceState trace_;
    std::vector<CmdFilter> skip_draws_, skip_dispatches_;
    void CmdTraceInit();
    void CmdTraceFlip();
    void CmdTraceWrite();
    static bool StageMatches(const GS::StageView& s, u64 value);
    /// The command's entry (kind D or X) with its identities; the caller adds ranges/images.
    CmdTraceEntry& CmdTraceAdd(char kind, u32 queue, u64 seq, u64 packet);
    void CmdTraceRanges(const GS::ResourceBindingPlan& plan, u8 stage);
    /// Over the image budget: GPU-written images written back to their memory and freed (P4.5, R7).
    void EvictGpuImages();
    OwnedBuffer diag_scratch_; ///< SSPC_DIAG_DUP_DISPATCH: a dense copy of the dispatch's buffers
    u32 dense_logged_ = 0;
    static constexpr u64 DiagKey(u64 n) { return (0xD1A6ull << 32) | (n << 2) | 2; }
    /// The buffer a command accesses guest [addr, addr + size) through: video memory (the buffer cache), a stream copy
    /// for a small read-only range the CPU rewrote, or the host import. `avail`: bytes usable from `offset`.
    bool GuestRange(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail);
    bool GuestRangeImpl(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail);
    u64 DiagBegin(std::string text);
    void DiagEnd(u64 id);

    /// Guest memory as the frontend reads it at a draw: a write still on the GPU's timeline lands first.
    class TimelineReader final : public GS::GuestReader {
    public:
        explicit TimelineReader(Executor& e) : e_(e) {}
        bool Read(u64 addr, void* dst, u64 bytes) override;

    private:
        Executor& e_;
        // P4.7: the physical backing's host view of the runs read last - the frontend's SRT walks read ~11 small
        // pieces a stage, each through the guest memory manager (a virtual call and a page table walk). A guest map or
        // unmap anywhere drops them (the backend's map generation); addresses without a backing run are remembered by
        // their 1 MiB block so they do not ask again. SSPC_DIAG_READ_RUNS=off: every read through the manager.
        struct Run {
            u64 begin = 0, end = 0;
            const u8* host = nullptr;
            u64 gen = 0;
        };
        std::array<Run, 4> runs_{};
        u32 next_run_ = 0;
        std::array<std::pair<u64, u64>, 4> no_run_{}; ///< (1 MiB block + 1, generation) without a backing run
        u32 next_no_run_ = 0;
        std::vector<u8> verify_; ///< SSPC_DIAG_READ_RUNS=verify

        void Verify(u64 addr, const void* dst, u64 bytes); ///< SSPC_DIAG_READ_RUNS=verify
    };
    struct TimelineWrite {
        u64 begin = 0, end = 0, value = 0;
    };

    DeviceContext& ctx_;
    Timeline& timeline_;
    StreamRing& ring_;
    GuestBuffers& guest_;
    BufferCache& buffers_;
    ImageCache& images_;
    PipelineFactory& pipelines_;
    GS::ShaderFrontend& frontend_;
    Presenter* presenter_;
    OwnedBuffer& gds_;
    ExecutorOptions options_;
    Cp::CommandProcessor* cp_ = nullptr;
    const Cp::RegisterFile* regs_ = nullptr; ///< what draws read (the processor's, or the P4.10 replica)
    std::array<u32, Cp::kNumQueues> queue_pipe_{};

    Batch cur_;
    bool open_ = false;
    u32 commands_ = 0;
    std::deque<Batch> in_flight_;
    std::vector<Batch> abandoned_; ///< never submitted successfully; retained until device teardown
    std::vector<Batch> free_;
    bool rendering_ = false;
    RenderKey render_{};
    bool pending_writes_ = false; ///< storage writes since the last global barrier
    /// What the commands since the last global barrier accessed, where known (P4.6 night sprint): the guest memory
    /// ranges dispatches and timeline copies read and wrote. A dispatch or a copy that touches none of the ranges
    /// written since (and writes none read since) needs no barrier of its own; anything else - a write of unknown
    /// extent (draws, images, GDS, LDS, timeline writes, a met wait) - makes `unknown` true and the next command
    /// barriers as before. SSPC_DIAG_BARRIER_RANGES=off: always a barrier (the P4.6 behaviour).
    struct PendingRanges {
        std::vector<std::pair<u64, u64>> writes, reads;
        /// per range: the dispatch's guest order tag (queue + 1 << 32 | the queue's guest sync epoch), 0 for others
        std::vector<u64> wtags, rtags;
        bool unknown = false;
        void Clear() {
            writes.clear();
            reads.clear();
            wtags.clear();
            rtags.clear();
            unknown = false;
        }
    };
    /// P4.6 night (SSPC_P46_GUEST_ORDER): the guest's own synchronization points per queue (an EVENT_WRITE/ACQUIRE_MEM,
    /// a fence, a met wait). Two dispatches of one queue with none between them are not ordered on the PS4 either (the
    /// CP launches the next one while the previous runs - the guest separates dependent dispatches with a CS partial
    /// flush): a range conflict between such dispatches needs no barrier here. Measured in the village: 99% of the
    /// dispatches barriered for a conflict had no guest synchronization since the queue's previous dispatch.
    std::array<u32, Cp::kNumQueues> guest_epoch_{};
    void GuestSync(u32 queue) {
        if (queue < guest_epoch_.size()) {
            ++guest_epoch_[queue];
        }
        GuestMemoryChanged(); // (P4.7: a guest ordering point - the CPU may write past it)
    }
    PendingRanges pend_;
    std::array<u64, Cp::kNumQueues> guest_flush_{}; ///< diagnostics: event types the guest wrote since the queue's last dispatch
    std::vector<u64> clock_labels_; ///< SSPC-020 diagnostics: addresses EOP/RELEASE_MEM write a GPU clock to
    u32 clock_label_logged_ = 0;
    void NoteClockLabelReads(const void* plan, u64 shader);
    void NoteOrderRange(const void* plan, u64 shader, u32 queue, bool dispatch);
    static constexpr u64 kGdsRangeBase = 1ull << 48; ///< GDS in the range checks: above every guest address
    /// A write of unknown extent since the last barrier: the next command barriers.
    /// `site` (counted): 0 a draw, 1 a dispatch, 2 a met wait, 3 WRITE_DATA, 4 GDS DMA, 5 an eviction, 6 other
    void MarkWrites(u32 site) {
        ++stats_.unknown_by[site < stats_.unknown_by.size() ? site : 6];
        pending_writes_ = true;
        pend_.unknown = true;
        GuestMemoryChanged(); // (P4.7: no memoized stage preparation past a write)
    }
    /// The barrier a dispatch or a timeline copy needs before it: false when its ranges touch nothing pending.
    bool NeedsBarrier(const std::vector<std::pair<u64, u64>>& reads, const std::vector<std::pair<u64, u64>>& writes,
                      bool unknown, u64 tag = 0);
    /// Its accesses noted for the next commands' decisions (after it was recorded).
    void NoteAccesses(const std::vector<std::pair<u64, u64>>& reads, const std::vector<std::pair<u64, u64>>& writes,
                      bool unknown, u64 tag = 0);
    std::vector<std::pair<u64, u64>> acc_reads_, acc_writes_; ///< the command's own ranges (capacity reused)
    bool want_flush_ = false;
    bool flush_full_ = false;  ///< the flush in progress ends a full batch (max_draws_per_batch)
    bool batch_fence_ = false; ///< the open batch holds a fence, a flip or a timeline write
    // CMASK/HTILE addresses a compute clear marked: the target clears at its next rendering
    std::unordered_map<u64, bool> pending_meta_clear_;
    ExecutorStats stats_;
    std::vector<TimelineWrite> timeline_writes_; ///< not completed yet (pruned in Poll)
    u64 tw_lo_ = ~0ull, tw_hi_ = 0;              ///< their bounds
    u64 tw_version_ = 0;                         ///< bumped at every change of timeline_writes_ (P4.10.4)
    bool write_run_ = false;                     ///< the commands recorded last were timeline writes
    std::vector<std::pair<u64, u64>> run_ranges_; ///< the ranges that run wrote (a write over one needs a barrier)
    TimelineReader reader_{*this};
    const std::atomic<u64>* map_gen_ = nullptr; ///< P4.7 (SetMapGeneration)
    Diagnostics* diag_ = nullptr;
    VkQueryPool queries_ = VK_NULL_HANDLE; ///< two timestamps per batch slot
    u32 next_query_slot_ = 0;
    // SSPC-020 / P4.6 night: the GPU's time at each held clock fence's place (SSPC_DIAG_CLOCK=completion: the time the
    // completion was seen, the earlier behaviour)
    static constexpr u32 kClockBatches = 64, kClockPerBatch = 32;
    VkQueryPool clock_pool_ = VK_NULL_HANDLE;
    u32 clock_next_ = 0;
    bool clock_at_place_ = false;
    bool clock_label_diag_ = false; ///< SSPC_DIAG_CLOCK=labels|place: clock_labels_ and the bindings over them
    // SSPC_GPU_PROFILE=1: GPU time per command key (pipeline / compute shader / upload / frame copy)
    struct ProfAcc {
        u64 ns = 0, count = 0;
        std::string label;
    };
    void ProfMark(u64 key, const std::string& label);
    void ProfCollect(Batch& b);
    void ProfReport();
    /// SSPC_GPU_PROFILE: the bindings of a command key, logged once (what the hot commands read and write).
    void ProfDescribe(u64 key, const char* what, const void* stage_view, const std::vector<CachedImage*>& bound);
    std::unordered_set<u64> prof_described_;
    bool prof_ = false;
    /// SSPC_GPU_PROFILE=q: a timestamp only where the recorded commands' guest queue changes and at a batch's end -
    /// the GPU time of each queue's runs (their barriers, uploads and shadows included) without one timestamp per
    /// command serializing them
    bool prof_queues_ = false;
    u32 prof_queue_ = 0; ///< (=q) the guest queue of the commands since the last timestamp
    static constexpr u64 kProfQueueTag = 0xD1A7; ///< (=q) a queue run's profile key: tag << 32 | queue
    /// (=q) the commands since the last timestamp end here (the time is prof_queue_'s); `next` the queue from now on.
    void ProfQueue(u32 next);

    VkQueryPool prof_pool_ = VK_NULL_HANDLE;
    u32 prof_next_batch_ = 0;
    std::unordered_map<u64, ProfAcc> prof_acc_;
    u64 prof_frames_ = 0, prof_total_ns_ = 0;
    // the translator's emulated LDS (shared memory beyond the device's): one workgroup's worth per workgroup of the
    // dispatch (the bridge: SharedMemSize() * NumWorkgroups()), in video memory, zeroed on the GPU
    OwnedBuffer lds_;
    std::vector<std::pair<u64, OwnedBuffer>> lds_retired_; ///< outgrown, until the batches using them complete
    u64 lds_groups_ = 1;
    u32 lds_group_bytes_ = 0;
    u32 fine_counter_ = 0; ///< SSPC_GPU_TIMINGS=sample:N - the commands since the last timed one (P4.9)
    u64 lds_logged_ = 0;
    std::vector<CachedImage*> info_images_; ///< the image of each image descriptor info of the draw (nullptr: none)
    bool device_lost_ = false;
    bool (*acquire_hook_)(void*) = nullptr;
    void* acquire_ctx_ = nullptr;
    void (*flip_hook_)(void*, u64) = nullptr;
    void* flip_ctx_ = nullptr;
    u64 last_unblock_tsc_ = 0;
    u64 batch_first_cmd_ = 0, batch_last_cmd_ = 0;

    // P4.8 Performance Inspector (SetInspector)
    const Inspector::Catalog* insp_catalog_ = nullptr;
    u32 insp_every_ = 0;
    VkQueryPool insp_pool_ = VK_NULL_HANDLE;
    u32 insp_next_batch_ = 0;
    Inspector::CategoryTotals insp_cat_{};
    u32 insp_frame_ = 0; ///< flip hooks called (FrameLog's flip marker index)
    struct InspFrameEnd {
        u32 n = 0;
        u64 value = 0; ///< the timeline value its flip's batch completes at
    };
    std::deque<InspFrameEnd> insp_frame_ends_;
    std::array<Inspector::GpuFrameTimes, 8> insp_acc_{}; ///< by frame % 8: completed batches' times so far
    u64 insp_last_end_ = 0;                              ///< the latest batch end timestamp (the union of intervals)
    struct InspDrawMemo {
        u64 vs = 0, ps = 0;
        const Inspector::Class* entry = nullptr; ///< the catalog's (nullptr: the rules decide per draw)
        bool valid = false, has_ps = false;
    };
    std::array<InspDrawMemo, 1024> insp_draw_memo_{};
    struct InspDispatchMemo {
        u64 cs = 0;
        Inspector::Class cls;
        bool valid = false;
    };
    std::array<InspDispatchMemo, 256> insp_dispatch_memo_{};
    struct InspPipeline {
        u64 draws = 0, instances = 0, vertices = 0, cpu_ticks = 0, indirect = 0;
        u64 vs = 0, ps = 0, vs_addr = 0, ps_addr = 0;
        Inspector::Class cls;
        u32 prim = 0, colors = 0, width = 0, height = 0;
        bool depth = false;
    };
    std::vector<InspPipeline> insp_pipelines_; ///< by pipeline id (this run's numbering)
    struct InspProgram {
        u64 dispatches = 0, groups = 0, cpu_ticks = 0, indirect = 0, single_group = 0;
        u64 address = 0;
        Inspector::Class cls;
    };
    std::unordered_map<u64, InspProgram> insp_programs_; ///< by compute shader content hash
    /// A timestamp where the GPU's work turns to `slot` (a Category or kGpu*): its interval begins.
    void InspMark(size_t slot);
    Inspector::Class InspClassifyDraw(const GS::StageView& vs, const GS::StageView& ps, bool has_ps,
                                      const Inspector::DrawShape& shape);
    Inspector::Class InspClassifyDispatch(const GS::StageView& cs);

    // SSPC_GPU_MARKERS=1 (P4.6-G6, diagnostic): a debug label around each run of commands of one graphics pipeline with
    // the same target registers / one compute shader / one kind of transfer (a GPU profiler's capture gives time and
    // counters per label); the draw labels' targets once each in logs\gpu_markers.txt. Off: one branch per command.
    bool markers_ = false;
    bool label_open_ = false;
    u64 label_key_ = 0;
    std::string label_text_;
    std::unordered_set<u64> legend_done_;
    std::ofstream legend_;
    bool LabelChanges(u64 key) const { return markers_ && open_ && (!label_open_ || key != label_key_); }
    void Label(u64 key, std::string text); ///< ends the open label, begins this one
    void EndLabel();
    static constexpr u64 kLabelOther = 2ull << 62; ///< | kind: transfers and frame copies (draws 0, dispatches 1 << 62)
};

} // namespace SecondSon::Gpu::Vk
