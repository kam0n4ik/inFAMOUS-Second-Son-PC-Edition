// SecondSonPC — the parallel command processor (Phase 4, P4.10; docs/PHASE4_P410_PARALLEL_COMMAND_PROCESSING.md).
//
// On by default since P5.3D (SSPC_P410_PARALLEL_CP=0: off): the native backend's command processor and its executor
// on two threads.
//
//   thread 1 (the backend's GPU thread)  PM4 packets, the register file, the processor's memory reads/writes, fences,
//                                        waits, everything between the loop's rounds (polls, flushes, idle waits);
//                                        with SSPC_P410_FRONTEND=1 also the shader frontend of each draw/dispatch
//        │  an ordered, bounded queue of records: draws and dispatches (the descriptor; the queue's compute registers
//        │  at the packet; the frontend's preparation when thread 1 made it) and the processor's calls that need no
//        │  answer (cache actions, fences, DMA reports, met waits, timeline copies); register writes in a journal
//        ▼
//   thread 2 (the recording thread)      the executor for each record, in order (and the shader frontend unless thread
//                                        1 made it), against a replica of the register file (the journal replayed up
//                                        to the record: the registers exactly as they were at the record's packet)
//
// The calls that need an answer from the executor in its current state, or a CPU-side effect after the GPU work before
// it (Synchronize, timeline writes/GDS DMA, a flip, a memory acquire when the frontend is on thread 1), drain the queue -
// thread 1 waits until thread 2 has executed every earlier record - and are then made on thread 1 directly, exactly as
// without the queue. So does the backend loop between rounds. With the queue empty, any call is made directly.
//
// What thread 1 does while thread 2 records: the processor's own state; its memory writes (a compute ring's read
// pointer after each packet, a fence with no work outstanding) whose buffer cache invalidation and memo change are
// handed to thread 2 in order (DeferInvalidateIfCached, MemoryChanged); its memory reads (a write on the GPU's timeline
// queued or pending there: drained first); the frontend's reads (a GPU write queued or not known complete there:
// drained, then the executor's own check - TimelineReader's - decides).
//
// Waiting: a bounded spin (SSPC_P410_SPIN_US, default 20 us) on the queue's counters, then a blocking wait on them
// (std::atomic::wait - WaitOnAddress); a waiter flag keeps the other side from waking anybody who does not sleep.
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu::Vk {
class BufferCache;
class Executor;
struct BatchDraw;
class Timeline;
}

namespace SecondSon::Gpu::Native {

namespace GS = Gpu::Shader;

/// Why thread 1 drained the queue (counted).
enum class DrainReason : u8 {
    RoundEnd,       ///< the backend loop's round ended (polls, flushes, idle waits follow)
    CacheAction,    ///< EVENT_WRITE / ACQUIRE_MEM
    Fence,          ///< EOP / EOS / RELEASE_MEM (held or written)
    Dma,            ///< a DMA_DATA / WRITE_DATA report
    Flip,           ///< the flip marker
    TimelineWrite,  ///< WRITE_DATA / DMA fill offered to the GPU's timeline
    TimelineCopy,   ///< DMA_DATA memory-to-memory offered to the timeline
    TimelineGds,    ///< DMA_DATA with GDS offered to the timeline
    MemoryAcquired, ///< a WAIT_REG_MEM on memory met
    WaitMet,        ///< a WAIT_REG_MEM met by a held fence
    Synchronize,    ///< the processor waits for the GPU before a CPU-side effect
    MemoryRead,     ///< the processor reads memory a timeline write is pending for
    MemoryWrite,    ///< the processor writes memory without physical backing (the tracker's pages)
    FrontendRead,   ///< the frontend (thread 1) reads memory a GPU write may still be pending for
    Stop,
    Count,
};
std::string_view DrainReasonName(DrainReason r);

/// Running totals (TSC ticks where named; thread 2's fields are written by thread 2 only, read by thread 1 at any time).
struct ParallelTotals {
    u64 records = 0, draws = 0, dispatches = 0, journal_records = 0;
    u64 drains = 0;          ///< drains that had to wait (the queue was not empty)
    u64 drains_empty = 0;    ///< drains of an empty queue (nothing to wait for)
    std::array<u64, static_cast<size_t>(DrainReason::Count)> drains_by{}; ///< waiting ones, by reason
    std::array<u64, static_cast<size_t>(DrainReason::Count)> drain_ticks_by{}; ///< Native CP (CP-0): their wait, TSC
    u64 t1_drain_ticks = 0;  ///< thread 1 waiting for drains
    u64 t1_full_ticks = 0, t1_full_waits = 0; ///< thread 1 waiting for a free slot / journal room (backpressure)
    u64 t1_sleeps = 0;       ///< thread 1's blocking waits (after the spin)
    u64 depth_sum = 0;       ///< records already queued at each push
    u64 depth_max = 0;
    u64 journal_words = 0;
    u64 t2_busy_ticks = 0;   ///< thread 2 executing records (journal replay included)
    u64 t2_idle_ticks = 0;   ///< thread 2 waiting for records
    u64 t2_sleeps = 0;
    u64 t2_records = 0;
    u64 deferred_invalidations = 0; ///< processor writes' buffer cache invalidations handed to thread 2
    u64 memo_changes = 0;           ///< guest memory changes handed to thread 2 with a record
    /// P4.10.4: the processor's other calls queued as records (thread 2 executes them in order) / made at once on thread
    /// 1 because the queue was empty (no wait), by kind: cache action, fence, DMA report, wait met, memory acquired,
    /// timeline copy
    std::array<u64, 6> queued_by{}, direct_by{};
    u64 copies_refused = 0;     ///< timeline copies thread 1 refused by their shape (the processor copies, as before)
    u64 copy_fallbacks = 0;     ///< queued timeline copies the executor refused: copied by the CPU on thread 2
    u64 timeline_read_drains = 0; ///< processor reads of memory a queued/pending timeline copy may write: drained
    // P4.10 stage 2 (SSPC_P410_FRONTEND=1): the frontend on thread 1
    u64 front_ticks = 0;          ///< thread 1 in the frontend (stage copies included)
    u64 front_reads = 0, front_read_hits = 0; ///< its memory reads / of them: a pending GPU write possible (drained)
    u64 front_syncs = 0;          ///< of those: the executor found a pending write (it waited for the GPU)
    u64 gpu_writes = 0, gw_rebuilds = 0, gw_block_hits = 0; ///< GPU write ranges noted; bitmap rebuilds; block hits
    u64 gw_max = 0;               ///< most ranges pending at once
    u64 gw_now = 0;               ///< ranges pending now (P4.11 soak telemetry)
    u64 async_acquires = 0;       ///< P4.11: met memory waits queued with their CPU writes applied on thread 1
    u64 queued_flips = 0;         ///< Native CP: flip markers queued to thread 2 (Options::async_flip)
    u64 copy_failures = 0;        ///< P4.11: queued copies neither recorded nor copied by the CPU (reported, completed)
    // P5.3D (SSPC_P53_FRONTEND_THREAD=1): the frontend on its own thread between thread 1 and thread 2
    u64 tf_busy_ticks = 0;    ///< the frontend thread preparing records (journal replay included)
    u64 tf_idle_ticks = 0;    ///< ... waiting for records from thread 1
    u64 tf_wait_ticks = 0;    ///< ... waiting for thread 2 (a read of memory a GPU write may still be pending for)
    u64 tf_waits = 0;
};

/// P4.10.4 diagnostics: every N-th queued timeline copy is refused on thread 2 as if the executor had refused it (its
/// CPU fallback tested in the game); 0: none.
u32 ForcedCopyFallbackEvery();

class ParallelCp final : public Cp::GpuWork, public Cp::RegisterSink {
public:
    struct Options {
        u32 spin_us = 20;         ///< how long a waiting side spins before it blocks (0: blocks at once)
        bool async = true;        ///< the calls that need no answer queued (false: each drains - the first prototype)
        bool async_copies = true; ///< timeline copies queued too (their answer from their shape)
        /// FrameBreak CP-9 (SSPC_CP_ASYNC_WRITES=1): WRITE_DATA / DMA fills and GDS DMA on the timeline queued too (their
        /// answer from their shape; a refusal when they run: the CPU does them there, after the earlier work)
        bool async_writes = false;
        bool async_gds_only = false; ///< (SSPC_CP_ASYNC_WRITES=gds: GDS DMA only)
        /// Stage 2: the shader frontend runs on thread 1 (nullptr: on thread 2, inside the executor); with it the
        /// guest maps' generation (its reads keep backing runs) and the timeline (the GPU writes it completed).
        GS::ShaderFrontend* frontend = nullptr;
        const std::atomic<u64>* map_gen = nullptr;
        const Vk::Timeline* timeline = nullptr;
        /// SSPC_P410_VERIFY=1: every draw/dispatch record carries the processor's register file hash at its packet
        /// (all groups' hashes) and thread 2 compares its replica's (the counts in the report)
        bool verify = false;
        /// P4.11 (SSPC_P411_ASYNC_ACQUIRE=1; with the frontend on thread 1 only): a met WAIT_REG_MEM on memory does
        /// not drain - the guest's CPU writes caught so far are applied on thread 1 (Executor::RunAcquireHook: the
        /// frontend's invalidations here, in stream order; the caches' invalidations into their locked queues, taken by
        /// thread 2 at its next command), the rest of MemoryAcquired is a record. Thread 2 may take those invalidations
        /// before records queued earlier: they then read CPU writes the guest made before it published them - work the
        /// guest's GPU runs unsynchronized with those writes too (a CPU write after a wait for that work completing is
        /// made only after thread 2 executed it: its label is written after that).
        bool async_acquire = false;
        /// P5.3D (SSPC_P53_FRONTEND_THREAD=1; with `frontend`): the frontend on a third thread - thread 1 only
        /// processes packets, the frontend thread prepares the records in order against its own replica of the
        /// register file, thread 2 executes the prepared ones. Guest memory changes thread 1 meets reach the frontend
        /// as flags of the next record (stream order); a read of memory a GPU write may still be pending for waits
        /// until thread 2 executed every earlier record (not a drain of thread 1). async_acquire is off with it.
        bool frontend_thread = false;
        /// Native CP (SSPC_CP_JOURNAL_CHANGES=1): register writes that change nothing are not journaled (the replicas
        /// already hold those values; RegisterFile::SetSinkChangesOnly).
        bool journal_changes_only = false;
        /// Native CP (SSPC_CP_ASYNC_FLIP=1; with `frontend` on thread 1): a flip marker is queued to thread 2 like a
        /// draw (Executor::FlipQueued: the frame's copy to the presenter, its submission, the flip) - not executed by
        /// thread 1 with the queue drained. Thread 1 begins the frontend's frame at the marker and snapshots its own
        /// per-frame totals for the frame log (SetFlipSnapshot). A flip that must evict GPU-written images
        /// (Executor::EvictionDue), and every flip while the stall trace or the inspector runs, is drained as before.
        bool async_flip = false;
    };
    /// `buffers`: the executor's buffer cache (the processor's writes invalidate it on thread 2).
    ParallelCp(Vk::Executor& executor, Vk::BufferCache* buffers, const Options& options);
    ~ParallelCp() override;
    ParallelCp(const ParallelCp&) = delete;
    ParallelCp& operator=(const ParallelCp&) = delete;

    /// The processor whose register file is journaled and whose compute registers are copied into dispatch records;
    /// the executor reads the replica from now on. Thread 1, before Start.
    void Attach(Cp::CommandProcessor& cp);
    /// Starts thread 2.
    void Start();
    /// Drains, stops thread 2 and joins it. Thread 1 (or after it stopped).
    void Stop();

    /// What thread 2 does for a queued timeline copy the executor refused (the processor's own fallback: the CPU copies
    /// after the GPU completed the earlier work): the copy and the caches' invalidation. true: copied.
    /// CP-9: the CPU's write / GDS DMA for a queued one the executor refused (thread 2, the earlier work complete).
    void SetWriteFallback(bool (*write)(void* ctx, u64 dst, const void* data, u64 bytes),
                          bool (*gds)(void* ctx, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes, u32 pattern),
                          void* ctx) {
        write_fallback_ = write;
        gds_fallback_ = gds;
        write_ctx_ = ctx;
    }
    void SetCopyFallback(bool (*copy)(void* ctx, u64 src, u64 dst, u64 bytes), void* ctx) {
        copy_fallback_ = copy;
        copy_ctx_ = ctx;
    }
    /// Options::async_flip: thread 1's part of a frame's totals, taken when the flip marker is queued.
    void SetFlipSnapshot(void (*snapshot)(void* ctx, FrameLog::GpuThreadTotals& out), void* ctx) {
        flip_snapshot_ = snapshot;
        flip_snapshot_ctx_ = ctx;
    }
    /// Thread 2, inside a queued flip's execution (the executor's flip hook): thread 1's totals taken at its marker;
    /// nullptr otherwise (the flip was executed by thread 1 with the queue drained).
    const FrameLog::GpuThreadTotals* QueuedFlipTotals() const { return flip_totals_; }
    /// Thread 1: waits until thread 2 has executed every record pushed so far; the journal, deferred invalidations and
    /// memory changes not yet handed on are applied here (thread 2 parked).
    void Drain(DrainReason why);
    /// FrameBreak CP-9 (SSPC_CP_ROUND_NODRAIN): the backend loop's round end without a drain while records are still
    /// queued - thread 2 flushes a held fence's batch and polls the GPU in stream order after them (the executor keeps
    /// the completions for thread 1). false: the queue is idle (the caller drains - nothing to wait for - and polls).
    bool RoundEndQueued();
    /// CP-9 (SSPC_CP_NODRAIN_FENCE=1): thread 2 submits a held fence's batch right after the fence's record and polls
    /// (the label's work reaches the GPU without waiting for the round's end).
    void SetFlushAtFence(bool on) { flush_at_fence_ = on; }
    u64 WritesQueued() const { return writes_queued_; }
    u64 WriteFallbacks() const { return write_fallbacks_.load(std::memory_order_relaxed); }
    /// Thread 1: a write on the GPU's timeline may be pending over [addr, addr + bytes) - one queued and not executed
    /// yet, or one the executor held at the last point the queue was empty (the processor's reads drain then).
    bool TimelinePending(u64 addr, u64 bytes);
    /// P4.11 (review): thread 1 is about to write [addr, addr + bytes) itself - a timeline copy queued and not executed
    /// yet writes there (its CPU fallback would land after this write: the caller drains first). Cheap: the compute
    /// rings' read pointers are written after every packet.
    bool CopyQueuedOver(u64 addr, u64 bytes) const;
    /// Thread 1: guest memory changed (a write of the processor): the frontend does not take a memoized stage
    /// preparation past it (handed on with the next record; at once when the queue is drained or the frontend is here).
    void MemoryChanged();
    /// Thread 1: the processor wrote [addr, addr + bytes): BufferCache::InvalidateIfCached on thread 2 before the first
    /// record pushed after the write (or at the next drain) - where the write is in the stream, not before older records.
    void DeferInvalidateIfCached(u64 addr, u64 bytes);
    /// The frontend runs on thread 1 (stage 2).
    bool FrontendAhead() const { return frontend_ != nullptr; }
    /// P5.3D: the frontend runs on its own thread.
    bool FrontendThread() const { return fthread_mode_; }
    /// Thread 1, frontend thread mode: what the processor's thread tells the frontend (ShaderFrontend::Invalidate /
    /// GpuWrote - not thread-safe) reaches it where it is in the stream: before the next record it prepares, or at once
    /// when the queue is empty (the frontend thread parked).
    void FrontInvalidate(u64 addr, u64 size) { FrontEvent(addr, size, false); }
    void FrontGpuWrote(u64 addr, u64 size) { FrontEvent(addr, size, true); }
    /// thread 2 caught an exception (nothing more is executed; the backend stops).
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    std::string FailureText() const;

    /// The register file draws read (thread 2's replica).
    const Cp::RegisterFile& Replica() const { return replica_; }
    ParallelTotals Totals() const;
    /// Records queued now (approximate, any thread).
    u64 Depth() const;
    /// P4.11 telemetry: thread 2's CPU time so far (user + kernel, ns; 0 when not running). Thread 1.
    u64 RecordingThreadCpuNs() const;
    std::string Report() const;

    // Cp::GpuWork (thread 1, inside CommandProcessor::Step)
    void Draw(const Cp::DrawDescriptor& draw) override;
    void Dispatch(const Cp::DispatchDescriptor& dispatch) override;
    void CacheAction(const Cp::CacheActionOp& op) override;
    void Flip(const Cp::FlipMarker& marker) override;
    void Fence(const Cp::FenceOp& op) override;
    void Dma(const Cp::DmaOp& op) override;
    void Interrupt(u32 queue, Cp::FenceKind kind) override;
    void Synchronize(u32 queue) override;
    bool WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) override;
    bool ZpassOnTimeline(u32 queue, u64 seq, u64 dst, u32 db_mask, u32 stride) override;
    bool CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) override;
    bool GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                       u32 pattern) override;
    void MemoryAcquired(u32 queue, bool blocked, u64 poll_address) override;
    void WaitMetByHeldFence(u32 queue) override;

    // Cp::RegisterSink (thread 1: the processor's register file)
    void OnWrite(Cp::Space space, u32 offset, const u32* values, u32 count) override;
    void OnReset() override;
    void OnLoad(std::span<const u32> values) override;

private:
    enum class Kind : u8 { Draw, Dispatch, Journal, Stop, CacheAction, Fence, Dma, WaitMet, MemoryAcquired, Copy, Flip,
                           RoundEnd, Write, Gds };
    struct Record {
        Kind kind = Kind::Journal;
        bool memory_changed = false; ///< a guest memory change reported before it
        bool has_cs = false;
        bool blocked = false;  ///< MemoryAcquired
        bool hook_done = false, applied = false; ///< MemoryAcquired with P4.11's async_acquire (applied: writes found)
        bool prepared = false; ///< stage 2: the frontend's preparation below (a filtered draw has none)
        bool front_memo = false; ///< frontend thread: a guest memory change (memo) before this record
        /// frontend thread: a processor write's deferred invalidation is due before this record (it never joins a run
        /// held before it: the run's draws are recorded at its first draw, before the invalidation)
        bool run_barrier = false;
        /// front mode: a draw the frontend thread joined to the run before it (a journal record now) - thread 2 takes
        /// it with the run's first draw (its registers after the run's draws)
        bool joined = false;
        /// FrameBreak FB-6: the draw's VS / PS view was not copied - the same as the last copied one (thread 2 keeps it)
        bool vs_same = false, ps_same = false;
        u64 journal_end = 0;   ///< the journal applied up to here before it executes
        u64 reg_hash = 0;      ///< Options::verify: the processor's registers at the packet
        Cp::DrawDescriptor draw;
        Cp::DispatchDescriptor dispatch;
        Cp::ComputeRegisters cs;
        Cp::CacheActionOp cache;
        Cp::FenceOp fence;
        Cp::DmaOp dma;
        Cp::FlipMarker flip;                 ///< Flip (Options::async_flip)
        FrameLog::GpuThreadTotals flip_totals; ///< ... thread 1's totals at the marker
        u32 queue = 0;                       ///< WaitMet, MemoryAcquired, Copy
        std::vector<u8> payload;             ///< CP-9 Write: the data (capacity reused)
        Cp::DmaKind gds_kind{};              ///< CP-9 Gds (memory in src)
        u32 gds_offset = 0, pattern = 0;
        u64 seq = 0, src = 0, dst = 0, bytes = 0; ///< Copy; MemoryAcquired: dst = the poll address, seq = met (TSC)
        // stage 2: the frontend's preparation, copied at the packet (the frontend's own state is the next command's)
        GS::DrawPreparation prep;
        GS::StageView stage, ps; ///< VS or CS; PS
        /// P5.3: the Native Render Frontend's shared preparations of the draw's VS/PS (instead of the copies above)
        std::shared_ptr<const GS::StageView> stage_shared, ps_shared;
        const GS::GraphicsPipelineState* state = nullptr;
        u64 census_key = 0; ///< SSPC_DIAG_PREP_CENSUS: the draw's row (0: not counted)
        /// SSPC_CITY_ENGINE_OPT: the draws that joined this one's run (recorded right after it, each with only its own
        /// geometry; their registers are in the journal after this record's journal_end)
        std::vector<Cp::RunMember> members;
        /// Phase 6: the members' own whole flattened user data (RunMember::vs_flat_at / ps_flat_at; capacity reused)
        std::vector<u32> member_flat;
        const GS::StageView& Vs() const { return stage_shared != nullptr ? *stage_shared : stage; }
        const GS::StageView& Ps() const { return ps_shared != nullptr ? *ps_shared : ps; }
    };
    /// records (power of two; front mode: a run's every draw is a record too)
    static constexpr u32 kSlots = 8192;
    static constexpr u32 kJournalWords = 1u << 22; ///< 16 MiB of register writes (power of two)
    enum JournalOp : u32 { kOpWrite = 1, kOpReset = 2, kOpLoad = 3, kOpPad = 15 };

    /// Stage 2: the frontend's guest memory on thread 1 - a GPU write that may still be pending drains the queue and
    /// lets the executor decide (TimelineReader's check); the bytes from the backing's host view (as TimelineReader).
    class FrontReader final : public GS::GuestReader {
    public:
        explicit FrontReader(ParallelCp& p) : p_(p) {}
        bool Read(u64 addr, void* dst, u64 bytes) override;

    private:
        ParallelCp& p_;
        struct Run {
            u64 begin = 0, end = 0;
            const u8* host = nullptr;
            u64 gen = 0;
        };
        std::array<Run, 4> runs_{};
        u32 next_run_ = 0;
        std::array<std::pair<u64, u64>, 4> no_run_{}; ///< (1 MiB block + 1, generation) without a backing run
        u32 next_no_run_ = 0;
    };

    /// The queue is empty and thread 2 parked: thread 1 may use the executor itself (after ApplyPending).
    bool Idle() const { return !run_open_ && head_.load(std::memory_order_acquire) == next_; }
    /// The journal, deferred invalidations and a memory change not handed on yet, applied by thread 1 (queue idle).
    void ApplyPending();
    void SnapshotTimeline(); ///< the executor's pending timeline writes (queue idle)
    void RunCopy(const Record& r);

    // thread 1
    Record& BeginRecord(Kind kind);
    /// `keep_journal_end`: the record's journal_end was set when it was filled (a run's first draw, held while the
    /// draws after it join: their register writes are journaled behind it)
    void PushRecord(bool keep_journal_end = false);
    void JournalReserve(u32 words);
    u32* JournalAt(u64 pos) { return journal_.get() + (pos & (kJournalWords - 1)); }
    /// until head_ >= target; `why`: the DrainReason of a drain, kWhyFull / kWhyJournal for backpressure (P4.12 trace)
    void WaitHead(u64 target, bool full, u16 why);
    static constexpr u16 kWhyFull = 0xFFFF, kWhyJournal = 0xFFFE;
    /// Stage 2: the frontend for record `index`, `regs` as they are at its packet; its GPU writes noted; the memo told
    /// when it writes memory. (Thread 1 with the record being filled, or the frontend thread.)
    void PrepareDraw(Record& r, u64 index, const Cp::RegisterFile& regs);
    /// SSPC_CITY_GEOFAST (docs/CITY_PERFORMANCE.md): the draw right after a prepared draw that differs from it only in
    /// its geometry - registers of the VS user data / index / draw parameter groups, VS user data only in the fetch
    /// shader's table pairs, no guest memory change between them - takes that draw's preparation with its own vertex
    /// bindings (the V#s read again from its table, as the frontend reads them). `out`: the views written (r's, or a
    /// scratch record for verify). false: not such a draw (nothing usable written).
    bool PrepareGeometryOnly(const Record& r, u64 index, Record& out);
    void PrepareDispatch(Record& r, u64 index);
    // P5.3D frontend thread
    void FrontLoop();
    void FrontStep(Record& r, u64 index);
    /// The frontend thread: until thread 2 executed every record before `index` (it then waits for this one).
    void FrontWaitExecuted(u64 index);
    /// The frontend thread, thread 2 parked before the record being prepared: GPU writes whose completion value fell
    /// out of the ring, asked of the executor (thread 1's ApplyPending does it without this thread).
    void DropUnknownGpuWrites();
    void FrontApplyJournal(u64 end);
    void FrontEvent(u64 addr, u64 size, bool gpu);
    /// The queued frontend events of records up to `upto` (the frontend thread; thread 1 with the queue empty).
    void ApplyFrontEvents(u64 upto);
    void NoteStageWrites(const GS::StageView& s, u64 record, bool& writes);
    /// Stage 2, the queue not idle: a GPU write of a record not executed, or not known complete, may overlap.
    bool MayBeGpuWritten(u64 addr, u64 bytes);
    void AddGpuWrite(u64 begin, u64 end, u64 record, u64 value);
    void RetireGpuWrites();
    void RebuildGpuWriteBits();
    /// Stage 2: the processor's calls queued for thread 2 tell the frontend here, in stream order (thread 2 does not).
    void FrontMemoChanged() {
        if (fthread_mode_) {
            pending_front_memo_ = true; // (the frontend thread is told with the next record: stream order)
        } else if (frontend_ != nullptr) {
            frontend_->GuestMemoryChanged();
        }
    }
    // either thread (thread 2's, or thread 1's while the queue is drained)
    void ApplyJournal(u64 end);
    void ApplyDeferred(u64 before); ///< the deferred invalidations of writes before record `before`
    // thread 2
    void Loop();
    /// Executes record `index`; returns how many records after it it executed with it (SSPC_CITY_BATCH: draws recorded
    /// as one with it - of the records before `avail`, the ones published)
    u64 Execute(Record& r, u64 index, u64 avail);
    /// Nothing more executes; the first failure's text is kept (thread 2, or thread 1 while the queue is drained).
    void Fail(std::string text);

    Vk::Executor& exec_;
    Vk::BufferCache* buffers_ = nullptr;
    Cp::CommandProcessor* cp_ = nullptr;
    Cp::RegisterFile replica_;
    const u64 spin_ticks_;
    const bool async_, async_copies_, verify_;
    const bool async_writes_, async_gds_only_;
    bool async_acquire_ = false;
    bool journal_changes_only_ = false;
    bool async_flip_ = false;
    void (*flip_snapshot_)(void*, FrameLog::GpuThreadTotals&) = nullptr;
    void* flip_snapshot_ctx_ = nullptr;
    const FrameLog::GpuThreadTotals* flip_totals_ = nullptr; ///< thread 2: the queued flip being executed
    std::atomic<u64> verified_{0}, verify_mismatches_{0}; ///< thread 2 (Options::verify)
    // FrameBreak FB-6 (SSPC_FB_STAGE_REUSE=1|verify): a draw's VS/PS view copied into its record only when the
    // frontend's StageGeneration moved; thread 2 swaps each copied view into last_vs_/last_ps_ and uses those (records
    // execute in order, so a record marked "same" finds the view of the last copied one there). verify: copied anyway,
    // a "same" record's copy compared with last_*.
    u8 stage_reuse_ = 0; ///< 0 off, 1 on, 2 verify
    /// SSPC_CITY_BATCH=1 (docs/CITY_PERFORMANCE.md): draws that follow a draw with only their geometry their own are
    /// recorded with it as one draw (Executor::Batchable); =ab:<n>: in odd blocks of n flips only (image A/B)
    u8 geofast_ = 0;            ///< SSPC_CITY_GEOFAST: 0 off, 1 on, 2 verify (prepared in full anyway and compared)
    u64 geofast_prev_ = ~0ull;  ///< the record of the last draw prepared (thread 1)
    u64 geofast_gen_ = 0;       ///< the frontend's memory generation after it
    std::unique_ptr<Record> geofast_check_; ///< verify: the fast preparation
    std::vector<GS::VertexBinding> geofast_vertices_;
    std::atomic<u64> geofast_hits_{0}, geofast_verified_{0}, geofast_mismatches_{0};
    u8 geofast_why_ = 0; ///< PrepareGeometryOnly's last refusal (the census' numbers)
    /// SSPC_CITY_DRAW_CACHE=1 (CITY-OPT Phase 4, with SSPC_CITY_GEOFAST): the draw before is the template - a draw that
    /// differs from it in user data past the vertex tables too (a constant buffer's V#, a texture's T#), VS or PS, keeps
    /// its preparation when the stage has no SRT walk: the stage's resources are decoded again from the new registers
    /// (DecodeResources, as the full preparation does) and the specialization key must be the module's
    bool drawcache_ = false;
    std::vector<std::array<u32, 4>> dc_reads_;
    GS::ResourceBindingPlan dc_vs_, dc_ps_;
    GS::SpecializationKey dc_key_;
    std::atomic<u64> dc_vs_hits_{0}, dc_ps_hits_{0}, dc_key_refused_{0};
    /// The plan of stage `v` (prepared for the draw before) for `user_data`: false when the stage has an SRT walk (that
    /// Phase 6 cannot take again), or the registers give another specialization or other binding numbers.
    bool RedecodeStage(const GS::StageView& v, std::span<const u32, 16> user_data,
                       std::span<const std::array<u32, 4>> vertex_reads, GS::ResourceBindingPlan& out);
    /// CITY-OPT Phase 6 (SSPC_CITY_ENGINE_RESOURCES=1|verify, with SSPC_CITY_GEOFAST; docs/CITY_ENGINE_RESOURCES.md):
    /// the engine's instanced draws of one material (0x1a1770 G-buffer, 0x43d480 shadows) change only the registers of
    /// the per-object constant buffer's V# and the vertex table; the material's tables the stages' SRT walks follow
    /// stay. The template of SSPC_CITY_DRAW_CACHE is taken for stages with a walk too: the draw before's walk output
    /// (the flattened user data past the registers) again when none of the registers it read changed, it wrote none and
    /// every read succeeded (no guest memory change between the two draws, as for the template itself) - the stage's
    /// descriptors decoded from it with the new registers, the key the module's, the bindings' end the same. verify:
    /// SSPC_CITY_GEOFAST's (each such draw also prepared in full and compared).
    u8 resources_ = 0; ///< 0 off, 1 on, 2 verify
    std::atomic<u64> res_walk_vs_{0}, res_walk_ps_{0}; ///< stages whose walk output was taken again
    /// stages whose walk read a register that differs (the CUE's table at a new address at each commit): walked again
    /// with the new registers - only the walk and the decoding, not the program, layout, fetch shader and module search
    std::atomic<u64> res_rewalk_vs_{0}, res_rewalk_ps_{0};
    /// RedecodeStage's refusals of a stage with a walk: 1 (unused), 2 it wrote the registers / a read failed / no walk
    /// program, 3 the layout's reflection differs, 4 another key, 5 other bindings, 6 the walk again failed
    std::array<std::atomic<u64>, 8> res_refused_{};
    // SSPC_DIAG_ENGINE_SRT=1 (CITY-OPT Phase 6): thread 1's draw preparations by the engine function that drew them
    // (DrawDescriptor::engine_run: 0 none, 1 mesh 0x1a1960, 2 shadow 0x43d480, 3 G-buffer 0x1a1770 - tagged with this
    // diagnostic or SSPC_CITY_ENGINE_GBUFFER_RUNS): prepared in full / from the draw before, their time, the SRT walks
    // and reads of the full ones, the stages' flattened data, the copies into the record, run members
    struct SrtDiag {
        std::atomic<u64> full{0}, fast{0}, t_full{0}, t_fast{0}, t_copy{0}, reads{0}, walk_vs{0}, walk_ps{0};
        std::atomic<u64> flat_vs{0}, flat_ps{0}, images{0}, buffers{0}, members{0}, own_ps{0};
        std::atomic<u64> t_join{0}, joins{0}, t_refused{0}, refusals{0}; ///< JoinRun: joined / refused, their time
        std::array<std::atomic<u64>, 16> why{}; ///< the template's refusals (geofast_why_)
        std::array<std::atomic<u64>, 64> groups{}; ///< JoinRun's refusal 3: the other register groups that changed
    };
    bool srt_diag_ = false;
    std::array<SrtDiag, 4> srt_by_{};
    // CITY-OPT Phase 6 (SSPC_CITY_ENGINE_GBUFFER_RUNS=1|verify): runs of 0x1a1770's draws too - their stages may have SRT
    // walks (taken again as above) and the members may differ in PS user data (the per-object constant buffer the CUE
    // binds for both stages): each member's PS plan decoded again, only read-only constant blocks of the same format may
    // differ (recorded with its own PS buffers and flattened user data)
    u8 gruns_ = 0; ///< 0 off, 1 on, 2 verify (each member prepared in full too, as SSPC_CITY_ENGINE_OPT=verify)
    u32 gruns_ab_ = 0; ///< =ab:<n>: in odd blocks of n flips only (image A/B)
    bool GRunsNow() const; ///< on, and (=ab:<n>) in an odd block
    GS::ResourceBindingPlan run_ps_plan_;
    // SSPC_DIAG_PREP_CENSUS=<first flip>:<flips> (CITY-OPT Phase 4): the draws of those flips by their VS/PS programs -
    // both threads' time, GEOFAST's answers, what changed since the draw before (Report)
    struct CensusRow {
        u64 vs = 0, ps = 0;
        u64 draws = 0, t1 = 0, t2 = 0, t2_draws = 0, srt_reads = 0, vs_flat = 0, ps_flat = 0;
        u64 vs_buffers = 0, ps_buffers = 0, ps_images = 0, vertices = 0;
        std::array<u64, 16> why{};
        std::array<u64, 16> batch_end{}; ///< thread 2: why the draw ended its batch (Executor::Batchable's check; 0 none)
        std::vector<std::pair<u64, u64>> groups, vs_ud, ps_ud; ///< (mask, draws), the first 8 masks
    };
    u64 census_first_ = 0, census_end_ = 0;
    mutable std::mutex census_mu_;
    std::vector<CensusRow> census_;
    Cp::DrawDescriptor census_last_; ///< thread 1: the draw before
    void CensusPrepared(Record& r, u64 ticks, bool fast);
    // CITY-OPT Phase 5 (SSPC_CITY_ENGINE_OPT=1|verify; docs/CITY_ENGINE_OPTIMIZATION.md): the engine's tagged mesh draws
    // (engine/render/draw_runs.h) one after another that differ only in their geometry - the VS vertex table, the index
    // range, instances - are one record: the first draw prepared in full and held (not published) while the next ones
    // join it with their own geometry only (no record, preparation, stage copy of their own), recorded by thread 2 as
    // the first draw's state + per draw its vertex/index bindings and draw command (Executor::DrawRunMembers). Any
    // other record, drain, memory change or call of the processor publishes the held one first (stream order).
    u8 runs_ = 0;           ///< 0 off, 1 on, 2 verify (each member also prepared in full and compared)
    u32 runs_ab_ = 0;       ///< =ab:<n>: runs only in odd blocks of n flips (image A/B)
    bool run_open_ = false; ///< thread 1: slots_[next_] is a run's first draw, held
    u64 run_gen_ = 0;       ///< the frontend's memory generation at it
    static constexpr size_t kMaxRun = 256;
    /// Thread 1: publishes the held run (nothing when none).
    void CloseRun();
    /// The record can lead a run (prepared, VS+PS, a VS without an SRT walk taking its vertices from fetch tables, no
    /// memory writes); the draw `d` can join the held run (the reason it cannot, counted, otherwise).
    bool RunLeaderOk(const Record& r) const;
    /// The draw is tagged by an engine draw function whose runs are on (Phase 6: 0x1a1770 with GBUFFER_RUNS only)
    bool RunTagged(const Cp::DrawDescriptor& d) const;
    /// `regs`: the registers at the joining draw's packet (the processor's, or the frontend thread's replica)
    bool JoinRun(const Cp::DrawDescriptor& d, const Cp::RegisterFile& regs);
    /// The held run's first draw and whether one is held - thread 1's, or with the frontend thread that thread's
    Record& RunLead() { return slots_[(fthread_mode_ ? f_run_lead_ : next_) & (kSlots - 1)]; }
    bool RunHeld() const { return fthread_mode_ ? f_run_open_ : run_open_; }
    // Fork port (front mode: SSPC_P53_FRONTEND_THREAD=1 with SSPC_CITY_ENGINE_OPT): thread 1 only parses the packets and
    // publishes every draw; the frontend thread prepares them (GEOFAST included) and builds the runs - a held run's
    // first draw is not published (fhead_ stays at it) while the draws after it join; a joined draw's record becomes a
    // journal record (thread 2 replays its registers, its draw is recorded with the run's first).
    bool f_run_open_ = false; ///< the frontend thread holds a run
    u64 f_run_lead_ = 0;      ///< ... its first draw's record
    bool pending_run_barrier_ = false; ///< thread 1: Record::run_barrier of the next record
    /// 1 while thread 1 waits for thread 2 (a drain, backpressure: written by thread 1 only) - a held run is published
    alignas(64) std::atomic<u32> f_flush_{0};
    u64 f_hold_ticks_ = 0; ///< how long the frontend thread keeps a run held with no record to join (then publishes)
    std::atomic<u64> f_run_joined_{0}, f_run_closed_idle_{0}, f_run_closed_flush_{0};
    /// The frontend thread: record `index` (its registers applied) joins the held run - true: it is a journal record now
    bool FrontTryJoin(Record& r, u64 index);
    /// The frontend thread: the held run is complete - every record before `upto` published to thread 2
    void FrontCloseRun(u64 upto);
    GS::ResourceBindingPlan run_plan_; ///< a joining draw's VS plan decoded again (its own constant buffers)
    std::atomic<u64> run_count_{0}, run_members_{0}, run_leaders_{0}, run_verified_{0}, run_mismatches_{0};
    std::array<std::atomic<u64>, 17> run_refused_{};
    /// Phase 6: G-buffer members tagged with another material (DrawRuns::kMaterialChanged) - refused before their PS is
    /// walked again (16); verify: not refused, those that still joined counted (the hint's misses)
    std::atomic<u64> run_material_{0}, run_material_joined_{0};
    bool batch_on_ = false;
    u32 batch_ab_ = 0;
    std::vector<Vk::BatchDraw> batch_items_;
    u8 batch_end_ = 0; ///< thread 2: the Batchable check that ended the last batch (0: none, or not checked)
    /// Thread 2: the view a prepared draw's stage executes with (`rec` itself when off or shared).
    const GS::StageView& ReuseView(GS::StageView& rec, bool shared, bool same, GS::StageView& last);
    static constexpr u64 kNoGen = ~0ull;
    u64 copied_gen_vs_ = kNoGen, copied_gen_ps_ = kNoGen; ///< thread 1: the generations last copied
    GS::StageView last_vs_, last_ps_;                     ///< thread 2
    std::atomic<u64> reuse_same_{0}, reuse_copied_{0}, reuse_mismatches_{0};
    GS::ShaderFrontend* frontend_ = nullptr;
    const std::atomic<u64>* map_gen_ = nullptr;
    const Vk::Timeline* timeline_ = nullptr;
    FrontReader reader_{*this};
    u32 front_fine_counter_ = 0;
    bool (*copy_fallback_)(void*, u64, u64, u64) = nullptr;
    bool (*write_fallback_)(void*, u64, const void*, u64) = nullptr;
    bool (*gds_fallback_)(void*, Cp::DmaKind, u64, u32, u32, u32) = nullptr;
    void* write_ctx_ = nullptr;
    void RunWrite(const Record& r);
    void RunGds(const Record& r);
    u64 writes_queued_ = 0;
    bool flush_at_fence_ = false;
    std::atomic<u64> write_fallbacks_{0};
    void* copy_ctx_ = nullptr;
    // thread 1: timeline copies queued since the queue was last idle; the executor's pending timeline writes then
    std::vector<std::pair<u64, u64>> queued_tw_, exec_tw_;
    u64 qtw_lo_ = ~0ull, qtw_hi_ = 0, etw_lo_ = ~0ull, etw_hi_ = 0; ///< (P4.11: their bounds - most checks miss both)
    u64 tw_version_seen_ = ~0ull;
    // stage 2, thread 1: GPU writes (shader writes of records, timeline writes) the frontend's reads may have to wait
    // for - until the record's work completed on the GPU (`value`: its timeline value; 0: thread 2 publishes it in
    // rec_value_ once the record ran); a bit per 64 KiB block of the 40-bit space they touch (rebuilt as they retire)
    struct GpuWrite {
        u64 begin = 0, end = 0, record = 0, value = 0;
    };
    std::deque<GpuWrite> gw_;
    std::unique_ptr<u64[]> gw_bits_;
    std::vector<u32> gw_dirty_; ///< the words of gw_bits_ set
    bool gw_stale_ = false;     ///< entries retired since the bits were built
    static constexpr u32 kGwBlockBits = 16;
    static constexpr u64 kGwWords = (1ull << (40 - kGwBlockBits)) / 64;
    static constexpr u32 kRecRing = 1u << 16;
    std::unique_ptr<std::atomic<u64>[]> rec_value_; ///< thread 2: the timeline value record i's work completes at
    /// P4.12 stall trace (on at Start only): when thread 1 pushed record i (TSC; thread 2 reads it after the tail it
    /// waited for - what its wait was for)
    std::unique_ptr<u64[]> push_tsc_;
    u64 forced_fallback_n_ = 0; ///< thread 2 (ForcedCopyFallbackEvery)
    std::atomic<u64> copy_fallbacks_{0}, copy_failures_{0};

    std::unique_ptr<Record[]> slots_;
    std::unique_ptr<u32[]> journal_;
    alignas(64) std::atomic<u64> tail_{0}; ///< records pushed (thread 1 writes)
    alignas(64) std::atomic<u64> head_{0}; ///< records executed (thread 2 writes)
    alignas(64) std::atomic<u64> t1_wait_target_{0}; ///< thread 1 sleeps until head_ reaches it (0: not sleeping)
    alignas(64) std::atomic<u32> t2_waiting_{0};      ///< thread 2 sleeps until tail_ moves
    alignas(64) std::atomic<u64> jhead_{0}; ///< journal applied up to (the applying side writes)
    u64 jtail_ = 0;                         ///< journal written up to (thread 1)
    u64 next_ = 0;                          ///< thread 1's next record (tail_ once pushed)
    bool pending_memory_changed_ = false;   ///< thread 1

    struct Deferred {
        u64 before = 0; ///< the first record pushed after the write
        u64 addr = 0, bytes = 0;
    };
    std::mutex deferred_mutex_;
    std::vector<Deferred> deferred_; ///< InvalidateIfCached ranges in stream order (under deferred_mutex_)
    std::atomic<bool> has_deferred_{false};

    // P5.3D frontend thread (fthread_mode_): it reads tail_, publishes fhead_ (thread 2 executes up to it)
    bool fthread_mode_ = false;
    std::thread fthread_;
    Cp::RegisterFile freplica_; ///< the frontend thread's registers (its journal position fjhead_)
    alignas(64) std::atomic<u64> fhead_{0};         ///< records prepared (the frontend thread writes)
    alignas(64) std::atomic<u32> f_waiting_{0};      ///< the frontend thread sleeps until tail_ moves
    alignas(64) std::atomic<u64> f_wait_target_{0};  ///< ... until head_ reaches it (0: not sleeping on it)
    alignas(64) std::atomic<u64> fjhead_{0};         ///< its journal position (thread 1's reservation reads it)
    u64 f_current_ = 0;              ///< the frontend thread: the record being prepared
    bool pending_front_memo_ = false; ///< thread 1: a memo change for the frontend thread, with the next record
    struct FrontEventEntry {
        u64 before = 0; ///< applied before the frontend thread prepares this record
        u64 addr = 0, size = 0;
        bool gpu = false; ///< GpuWrote (else Invalidate)
    };
    std::mutex fe_mutex_;
    std::vector<FrontEventEntry> fe_; ///< (under fe_mutex_)
    std::atomic<bool> has_fe_{false};
    /// The frontend's counters: written by the one thread running the frontend (thread 1, or the frontend thread),
    /// read by thread 1's reports - atomics written as a single writer's (load + store, no locked add).
    struct FrontCounters {
        std::atomic<u64> ticks{0}, reads{0}, read_hits{0}, syncs{0}, gpu_writes{0}, gw_max{0}, gw_block_hits{0},
            gw_rebuilds{0}, gw_now{0}, busy{0}, idle{0}, wait_ticks{0}, waits{0};
    };
    FrontCounters fc_;
    static void Bump(std::atomic<u64>& a, u64 v = 1) {
        a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }

    std::thread thread_;
    bool started_ = false;
    std::atomic<bool> failed_{false};
    std::string failure_; ///< written by thread 2 before failed_ is set

    // totals: thread 1's plain, thread 2's atomics (single writer)
    ParallelTotals t1_;
    std::atomic<u64> t2_busy_{0}, t2_idle_{0}, t2_sleeps_{0}, t2_records_{0};
};

} // namespace SecondSon::Gpu::Native
