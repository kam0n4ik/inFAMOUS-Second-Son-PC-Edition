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
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu::Vk {
class BufferCache;
class Executor;
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
    void SetCopyFallback(bool (*copy)(void* ctx, u64 src, u64 dst, u64 bytes), void* ctx) {
        copy_fallback_ = copy;
        copy_ctx_ = ctx;
    }
    /// Thread 1: waits until thread 2 has executed every record pushed so far; the journal, deferred invalidations and
    /// memory changes not yet handed on are applied here (thread 2 parked).
    void Drain(DrainReason why);
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
    enum class Kind : u8 { Draw, Dispatch, Journal, Stop, CacheAction, Fence, Dma, WaitMet, MemoryAcquired, Copy };
    struct Record {
        Kind kind = Kind::Journal;
        bool memory_changed = false; ///< a guest memory change reported before it
        bool has_cs = false;
        bool blocked = false;  ///< MemoryAcquired
        bool hook_done = false, applied = false; ///< MemoryAcquired with P4.11's async_acquire (applied: writes found)
        bool prepared = false; ///< stage 2: the frontend's preparation below (a filtered draw has none)
        bool front_memo = false; ///< frontend thread: a guest memory change (memo) before this record
        u64 journal_end = 0;   ///< the journal applied up to here before it executes
        u64 reg_hash = 0;      ///< Options::verify: the processor's registers at the packet
        Cp::DrawDescriptor draw;
        Cp::DispatchDescriptor dispatch;
        Cp::ComputeRegisters cs;
        Cp::CacheActionOp cache;
        Cp::FenceOp fence;
        Cp::DmaOp dma;
        u32 queue = 0;                       ///< WaitMet, MemoryAcquired, Copy
        u64 seq = 0, src = 0, dst = 0, bytes = 0; ///< Copy; MemoryAcquired: dst = the poll address, seq = met (TSC)
        // stage 2: the frontend's preparation, copied at the packet (the frontend's own state is the next command's)
        GS::DrawPreparation prep;
        GS::StageView stage, ps; ///< VS or CS; PS
        /// P5.3: the Native Render Frontend's shared preparations of the draw's VS/PS (instead of the copies above)
        std::shared_ptr<const GS::StageView> stage_shared, ps_shared;
        const GS::GraphicsPipelineState* state = nullptr;
        const GS::StageView& Vs() const { return stage_shared != nullptr ? *stage_shared : stage; }
        const GS::StageView& Ps() const { return ps_shared != nullptr ? *ps_shared : ps; }
    };
    static constexpr u32 kSlots = 1024; ///< records (power of two)
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
    bool Idle() const { return head_.load(std::memory_order_acquire) == next_; }
    /// The journal, deferred invalidations and a memory change not handed on yet, applied by thread 1 (queue idle).
    void ApplyPending();
    void SnapshotTimeline(); ///< the executor's pending timeline writes (queue idle)
    void RunCopy(const Record& r);

    // thread 1
    Record& BeginRecord(Kind kind);
    void PushRecord();
    void JournalReserve(u32 words);
    u32* JournalAt(u64 pos) { return journal_.get() + (pos & (kJournalWords - 1)); }
    /// until head_ >= target; `why`: the DrainReason of a drain, kWhyFull / kWhyJournal for backpressure (P4.12 trace)
    void WaitHead(u64 target, bool full, u16 why);
    static constexpr u16 kWhyFull = 0xFFFF, kWhyJournal = 0xFFFE;
    /// Stage 2: the frontend for record `index`, `regs` as they are at its packet; its GPU writes noted; the memo told
    /// when it writes memory. (Thread 1 with the record being filled, or the frontend thread.)
    void PrepareDraw(Record& r, u64 index, const Cp::RegisterFile& regs);
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
    void Execute(Record& r, u64 index);
    /// Nothing more executes; the first failure's text is kept (thread 2, or thread 1 while the queue is drained).
    void Fail(std::string text);

    Vk::Executor& exec_;
    Vk::BufferCache* buffers_ = nullptr;
    Cp::CommandProcessor* cp_ = nullptr;
    Cp::RegisterFile replica_;
    const u64 spin_ticks_;
    const bool async_, async_copies_, verify_;
    bool async_acquire_ = false;
    std::atomic<u64> verified_{0}, verify_mismatches_{0}; ///< thread 2 (Options::verify)
    GS::ShaderFrontend* frontend_ = nullptr;
    const std::atomic<u64>* map_gen_ = nullptr;
    const Vk::Timeline* timeline_ = nullptr;
    FrontReader reader_{*this};
    u32 front_fine_counter_ = 0;
    bool (*copy_fallback_)(void*, u64, u64, u64) = nullptr;
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
