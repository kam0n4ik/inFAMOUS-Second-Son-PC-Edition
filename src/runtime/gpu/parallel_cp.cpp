// SecondSonPC — the parallel command processor (P4.10). See parallel_cp.h.
#include <windows.h>

#include "runtime/gpu/parallel_cp.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <exception>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_resources.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Native {

namespace {

enum QueuedKind : size_t { kQCache, kQFence, kQDma, kQWaitMet, kQAcquired, kQCopy };

constexpr u64 kLookup = ~0ull; ///< a GPU write's value thread 2 publishes once its record ran
constexpr u64 kPage = 4096;    ///< BufferCache's validity pages: its pending GPU writes are per page

/// Every register of the file, through the groups' hashes it keeps up to date (Options::verify).
u64 RegistersHash(const Cp::RegisterFile& regs) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 g = 0; g < static_cast<u32>(Cp::Group::Count); ++g) {
        h = (h ^ regs.GroupHash(static_cast<Cp::Group>(g))) * 0x100000001B3ull;
    }
    return h;
}

/// `lo`/`hi`: the ranges' bounds (P4.11: the processor's reads and writes outside them are answered at once).
bool Overlaps(const std::vector<std::pair<u64, u64>>& ranges, u64 lo, u64 hi, u64 addr, u64 bytes) {
    if (ranges.empty() || addr >= hi || addr + bytes <= lo) {
        return false;
    }
    return std::ranges::any_of(ranges, [&](const auto& r) { return addr < r.second && r.first < addr + bytes; });
}

/// What the executor reads of a stage the frontend prepared (not its specialization key).
void CopyStage(GS::StageView& d, const GS::StageView& s) {
    d.status = s.status;
    d.address = s.address;
    d.program = s.program;
    d.footer_hash = s.footer_hash;
    d.module = s.module;
    d.runtime = s.runtime;
    d.start = s.start;
    d.end = s.end;
    d.program_error = s.program_error;
    d.plan.stage = s.plan.stage;
    d.plan.flat.assign(s.plan.flat.begin(), s.plan.flat.end());
    d.plan.buffers.assign(s.plan.buffers.begin(), s.plan.buffers.end());
    d.plan.images.assign(s.plan.images.begin(), s.plan.images.end());
    d.plan.samplers.assign(s.plan.samplers.begin(), s.plan.samplers.end());
    d.plan.vertices.assign(s.plan.vertices.begin(), s.plan.vertices.end());
    d.plan.status_counts = s.plan.status_counts;
    d.plan.srt = s.plan.srt;
    if (!s.plan.error.empty() || !d.plan.error.empty()) {
        d.plan.error = s.plan.error;
    }
}

} // namespace

std::string_view DrainReasonName(DrainReason r) {
    static constexpr std::string_view kNames[] = {
        "round end",     "cache action", "fence",       "dma report",  "flip",         "timeline write",
        "timeline copy", "timeline gds", "memory wait", "wait met",    "synchronize",  "memory read",
        "memory write",  "frontend read", "stop"};
    static_assert(std::size(kNames) == static_cast<size_t>(DrainReason::Count));
    const auto i = static_cast<size_t>(r);
    return i < std::size(kNames) ? kNames[i] : "?";
}

u32 ForcedCopyFallbackEvery() {
    static const u32 every = [] {
        char v[16] = {};
        const u32 n = GetEnvironmentVariableA("SSPC_P410_DIAG_COPY_FALLBACK", v, sizeof(v)) > 0
                          ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                          : 0u;
        if (n != 0) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_P410_DIAG_COPY_FALLBACK={} - every {}-th queued timeline copy is "
                                   "copied by the CPU on the recording thread (its refusal path)",
                     n, n);
        }
        return n;
    }();
    return every;
}

ParallelCp::ParallelCp(Vk::Executor& executor, Vk::BufferCache* buffers, const Options& options)
    : exec_(executor), buffers_(buffers), spin_ticks_(Kernel::Clock::TscFrequency() / 1'000'000 * options.spin_us),
      async_(options.async), async_copies_(options.async && options.async_copies), verify_(options.verify),
      frontend_(options.timeline != nullptr ? options.frontend : nullptr), map_gen_(options.map_gen),
      timeline_(options.timeline), slots_(std::make_unique<Record[]>(kSlots)),
      journal_(std::make_unique<u32[]>(kJournalWords)) {
    if (frontend_ != nullptr) {
        gw_bits_ = std::make_unique<u64[]>(kGwWords);
        rec_value_ = std::make_unique<std::atomic<u64>[]>(kRecRing);
    }
    fthread_mode_ = options.frontend_thread && frontend_ != nullptr;
    async_acquire_ = options.async_acquire && async_ && frontend_ != nullptr && !fthread_mode_;
}

ParallelCp::~ParallelCp() {
    Stop();
}

void ParallelCp::Attach(Cp::CommandProcessor& cp) {
    cp_ = &cp;
    replica_ = cp.Registers(); // (the same registers, versions and hashes from here on, the journal keeps them so)
    replica_.SetSink(nullptr);
    replica_.SetMirror(nullptr);
    if (fthread_mode_) {
        freplica_ = cp.Registers(); // (the frontend thread's own copy, kept by the same journal)
        freplica_.SetSink(nullptr);
        freplica_.SetMirror(nullptr);
    }
    cp.MutableRegisters().SetSink(this);
    exec_.SetRegisters(&replica_);
}

void ParallelCp::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    if (StallTrace::Deep()) {
        push_tsc_ = std::make_unique<u64[]>(kRecRing);
    }
    if (fthread_mode_) {
        fthread_ = std::thread([this] { FrontLoop(); });
    }
    thread_ = std::thread([this] { Loop(); });
}

void ParallelCp::Stop() {
    if (!started_) {
        return;
    }
    Drain(DrainReason::Stop);
    BeginRecord(Kind::Stop);
    PushRecord();
    if (fthread_.joinable()) {
        fthread_.join(); // (it passes the stop record on to thread 2)
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    started_ = false;
    exec_.SetMemoBumps(true);
    if (cp_ != nullptr) {
        cp_->MutableRegisters().SetSink(nullptr);
        exec_.SetRegisters(&cp_->Registers());
    }
}

// ---- thread 1

ParallelCp::Record& ParallelCp::BeginRecord(Kind kind) {
    if (next_ - head_.load(std::memory_order_acquire) >= kSlots) {
        WaitHead(next_ - kSlots + 1, true, kWhyFull); // backpressure: the oldest slot executed
    }
    Record& r = slots_[next_ & (kSlots - 1)];
    r.kind = kind;
    r.front_memo = pending_front_memo_; // (frontend thread: told before it prepares this record)
    pending_front_memo_ = false;
    r.memory_changed = pending_memory_changed_;
    if (pending_memory_changed_) {
        pending_memory_changed_ = false;
        ++t1_.memo_changes;
    }
    return r;
}

void ParallelCp::PushRecord() {
    Record& r = slots_[next_ & (kSlots - 1)];
    r.journal_end = jtail_; // every register write of the stream before the record
    const u64 depth = next_ - head_.load(std::memory_order_relaxed);
    t1_.depth_sum += depth;
    t1_.depth_max = std::max(t1_.depth_max, depth);
    ++t1_.records;
    if (push_tsc_ != nullptr) {
        push_tsc_[next_ & (kRecRing - 1)] = __rdtsc(); // (published with tail_ below)
    }
    ++next_;
    tail_.store(next_, std::memory_order_seq_cst);
    // (tail_'s waiter: thread 2, or with the frontend thread that thread - thread 2 then waits on fhead_)
    if ((fthread_mode_ ? f_waiting_ : t2_waiting_).load(std::memory_order_seq_cst) != 0) {
        tail_.notify_one();
    }
    if (frontend_ != nullptr && !fthread_mode_ && (next_ & 63) == 0) {
        RetireGpuWrites(); // (the GPU writes completed so far leave the list; the bits follow at the next hit)
    }
}

void ParallelCp::WaitHead(u64 target, bool full, u16 why) {
    u64 h = head_.load(std::memory_order_acquire);
    if (h >= target) {
        return;
    }
    const u64 h0 = h;
    const u64 t0 = __rdtsc();
    while (h < target && __rdtsc() - t0 < spin_ticks_) {
        _mm_pause();
        h = head_.load(std::memory_order_acquire);
    }
    while (h < target) {
        t1_wait_target_.store(target, std::memory_order_seq_cst);
        h = head_.load(std::memory_order_seq_cst);
        if (h < target) {
            ++t1_.t1_sleeps;
            head_.wait(h, std::memory_order_seq_cst);
        }
        t1_wait_target_.store(0, std::memory_order_relaxed);
        h = head_.load(std::memory_order_acquire);
    }
    const u64 t1 = __rdtsc();
    const u64 ticks = t1 - t0;
    if (full) {
        t1_.t1_full_ticks += ticks;
        ++t1_.t1_full_waits;
    } else {
        t1_.t1_drain_ticks += ticks;
    }
    // P4.12: what thread 1 waited for - thread 2 executing records up to `target` (a: the target; b: records then
    // queued; c: the drain's reason)
    StallTrace::Span(!full ? StallTrace::Ev::Drain
                     : why == kWhyJournal ? StallTrace::Ev::JournalFull
                                          : StallTrace::Ev::QueueFull,
                     t0, t1, target, static_cast<u32>(target - std::min(target, h0)), why);
}

void ParallelCp::ApplyPending() {
    ApplyJournal(jtail_);
    if (has_deferred_.load(std::memory_order_acquire)) {
        ApplyDeferred(~0ull);
    }
    if (pending_memory_changed_) {
        pending_memory_changed_ = false;
        ++t1_.memo_changes;
        exec_.GuestMemoryChanged();
    }
    if (frontend_ != nullptr) {
        exec_.SetMemoBumps(true); // (calls made here tell the frontend themselves, in stream order)
        if (fthread_mode_) {
            // the frontend thread is parked (every record prepared and executed): what it was not told yet
            ApplyFrontEvents(~0ull);
            if (pending_front_memo_) {
                pending_front_memo_ = false;
                frontend_->GuestMemoryChanged();
            }
        } else {
            // GPU writes whose completion value fell out of the ring: the executor decides now (rare)
            while (!gw_.empty() && gw_.front().value == kLookup && gw_.front().record + kRecRing <= next_ &&
                   !exec_.FrontendReadPending(gw_.front().begin, gw_.front().end - gw_.front().begin)) {
                gw_.pop_front();
                gw_stale_ = true;
            }
        }
    }
    queued_tw_.clear(); // (every queued timeline copy executed: they are the executor's own now)
    qtw_lo_ = ~0ull;
    qtw_hi_ = 0;
    SnapshotTimeline();
}

void ParallelCp::SnapshotTimeline() {
    const u64 v = exec_.TimelineWritesVersion();
    if (v != tw_version_seen_) {
        exec_.TimelineWriteRanges(exec_tw_);
        tw_version_seen_ = v;
        etw_lo_ = ~0ull;
        etw_hi_ = 0;
        for (const auto& [b, e] : exec_tw_) {
            etw_lo_ = std::min(etw_lo_, b);
            etw_hi_ = std::max(etw_hi_, e);
        }
    }
}

void ParallelCp::Drain(DrainReason why) {
    if (!started_) {
        return;
    }
    if (Idle()) {
        ++t1_.drains_empty;
    } else {
        ++t1_.drains;
        ++t1_.drains_by[static_cast<size_t>(why)];
        WaitHead(next_, false, static_cast<u16>(why));
    }
    // thread 2 is parked (it waits for the next record): what is not handed on yet is applied here
    ApplyPending();
    if (frontend_ != nullptr) {
        RetireGpuWrites(); // (frontend thread: parked too - the queue is empty, so it prepared everything)
    }
}

bool ParallelCp::TimelinePending(u64 addr, u64 bytes) {
    if (!started_ || Idle()) {
        if (started_) {
            ApplyPending();
        }
        return exec_.TimelineWritePending(addr, bytes); // (exact: nothing executes on thread 2 now)
    }
    if (Overlaps(queued_tw_, qtw_lo_, qtw_hi_, addr, bytes) || Overlaps(exec_tw_, etw_lo_, etw_hi_, addr, bytes)) {
        ++t1_.timeline_read_drains;
        return true; // the caller drains and asks the executor
    }
    return false;
}

bool ParallelCp::CopyQueuedOver(u64 addr, u64 bytes) const {
    return started_ && !Idle() && Overlaps(queued_tw_, qtw_lo_, qtw_hi_, addr, bytes);
}

void ParallelCp::MemoryChanged() {
    if (fthread_mode_) {
        FrontMemoChanged(); // (with the next record, or at the next idle point)
        return;
    }
    if (frontend_ != nullptr) {
        frontend_->GuestMemoryChanged(); // (the frontend is here: at once, where the write is in the stream)
        return;
    }
    pending_memory_changed_ = true;
}

void ParallelCp::DeferInvalidateIfCached(u64 addr, u64 bytes) {
    std::scoped_lock lock{deferred_mutex_};
    // (a compute ring's read pointer is written after each packet: the same range again is not queued twice)
    if (deferred_.empty() || deferred_.back().before != next_ || deferred_.back().addr != addr ||
        deferred_.back().bytes != bytes) {
        deferred_.push_back({next_, addr, bytes});
        ++t1_.deferred_invalidations;
    }
    has_deferred_.store(true, std::memory_order_release);
}

void ParallelCp::JournalReserve(u32 words) {
    const u64 idx = jtail_ & (kJournalWords - 1);
    const bool wrap = idx + words > kJournalWords;
    const u64 need = words + (wrap ? kJournalWords - idx : 0);
    // (the frontend thread reads the journal too: its words are free once both replicas passed them)
    const u64 read = fthread_mode_ ? std::min(jhead_.load(std::memory_order_acquire), fjhead_.load(std::memory_order_acquire))
                                   : jhead_.load(std::memory_order_acquire);
    if (jtail_ + need - read > kJournalWords) {
        // thread 2 replays the journal with records only: one that carries it so far, then wait until it ran
        BeginRecord(Kind::Journal);
        PushRecord();
        ++t1_.journal_records;
        WaitHead(next_, true, kWhyJournal);
    }
    if (wrap) {
        journal_[idx] = u32{kOpPad} << 28; // the entry starts at the ring's start
        jtail_ += kJournalWords - idx;
    }
}

void ParallelCp::OnWrite(Cp::Space space, u32 offset, const u32* values, u32 count) {
    const u32 words = 2 + count;
    JournalReserve(words);
    u32* w = JournalAt(jtail_);
    w[0] = (u32{kOpWrite} << 28) | (static_cast<u32>(space) << 24) | count;
    w[1] = offset;
    std::memcpy(w + 2, values, size_t{count} * sizeof(u32));
    jtail_ += words;
    t1_.journal_words += words;
}

void ParallelCp::OnReset() {
    JournalReserve(1);
    *JournalAt(jtail_) = u32{kOpReset} << 28;
    jtail_ += 1;
    t1_.journal_words += 1;
}

void ParallelCp::OnLoad(std::span<const u32> values) {
    const u32 words = 1 + static_cast<u32>(values.size());
    JournalReserve(words);
    u32* w = JournalAt(jtail_);
    w[0] = (u32{kOpLoad} << 28) | static_cast<u32>(values.size());
    std::memcpy(w + 1, values.data(), values.size_bytes());
    jtail_ += words;
    t1_.journal_words += words;
}

// ---- stage 2: the frontend on thread 1

void ParallelCp::AddGpuWrite(u64 begin, u64 end, u64 record, u64 value) {
    if (end <= begin) {
        return;
    }
    gw_.push_back({begin, end, record, value});
    Bump(fc_.gpu_writes);
    if (frontend_ != nullptr) {
        frontend_->GpuWrote(begin, end - begin); // (P5.2: trusted shader code under a GPU write is read again)
    }
    if (gw_.size() > fc_.gw_max.load(std::memory_order_relaxed)) {
        fc_.gw_max.store(gw_.size(), std::memory_order_relaxed);
    }
    const u64 b0 = begin >> kGwBlockBits, b1 = std::min((end - 1) >> kGwBlockBits, kGwWords * 64 - 1);
    for (u64 b = b0; b <= b1; ++b) {
        u64& word = gw_bits_[b >> 6];
        if (word == 0) {
            gw_dirty_.push_back(static_cast<u32>(b >> 6));
        }
        word |= u64{1} << (b & 63);
    }
}

void ParallelCp::RetireGpuWrites() {
    if (gw_.empty()) {
        return;
    }
    const u64 head = head_.load(std::memory_order_acquire);
    u64 completed = 0;
    bool queried = false;
    while (!gw_.empty()) {
        const GpuWrite& w = gw_.front();
        u64 v = w.value;
        if (v == kLookup) {
            if (w.record >= head || w.record + kRecRing <= head) {
                break; // not executed yet / its value is gone (the executor decides at an idle point)
            }
            v = rec_value_[w.record & (kRecRing - 1)].load(std::memory_order_relaxed);
        }
        if (!queried) {
            completed = timeline_->QueryCompleted();
            queried = true;
        }
        if (v > completed) {
            break;
        }
        gw_.pop_front();
        gw_stale_ = true;
    }
}

void ParallelCp::RebuildGpuWriteBits() {
    for (const u32 w : gw_dirty_) {
        gw_bits_[w] = 0;
    }
    gw_dirty_.clear();
    for (const GpuWrite& w : gw_) {
        const u64 b0 = w.begin >> kGwBlockBits, b1 = std::min((w.end - 1) >> kGwBlockBits, kGwWords * 64 - 1);
        for (u64 b = b0; b <= b1; ++b) {
            u64& word = gw_bits_[b >> 6];
            if (word == 0) {
                gw_dirty_.push_back(static_cast<u32>(b >> 6));
            }
            word |= u64{1} << (b & 63);
        }
    }
    gw_stale_ = false;
    Bump(fc_.gw_rebuilds);
}

bool ParallelCp::MayBeGpuWritten(u64 addr, u64 bytes) {
    if (gw_.empty() || bytes == 0 || addr >= (1ull << 40)) {
        return false;
    }
    const u64 b0 = addr >> kGwBlockBits, b1 = std::min((addr + bytes - 1) >> kGwBlockBits, kGwWords * 64 - 1);
    const auto any_bit = [&] {
        for (u64 b = b0; b <= b1; ++b) {
            if ((gw_bits_[b >> 6] >> (b & 63)) & 1) {
                return true;
            }
        }
        return false;
    };
    if (!any_bit()) {
        return false;
    }
    Bump(fc_.gw_block_hits);
    RetireGpuWrites();
    if (gw_stale_) {
        RebuildGpuWriteBits();
        if (!any_bit()) {
            return false;
        }
    }
    return std::ranges::any_of(gw_, [&](const GpuWrite& w) { return addr < w.end && w.begin < addr + bytes; });
}

void ParallelCp::NoteStageWrites(const GS::StageView& s, u64 record, bool& writes) {
    for (const GS::BufferBinding& b : s.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest) {
            if (b.written && b.status == GS::ResourceStatus::Ok && b.size != 0 && b.address != 0) {
                // (the buffer cache keeps its GPU writes per 4 KiB page: so does this, its superset)
                AddGpuWrite(b.address & ~(kPage - 1), (b.address + b.size + kPage - 1) & ~(kPage - 1), record, kLookup);
                writes = true;
            }
        } else if (b.written || (b.kind != GS::BufferKind::Flatbuf && b.kind != GS::BufferKind::ClipPlanes)) {
            writes = true; // GDS, the emulated LDS, BDA: what the executor counts as a write (of unknown extent)
        }
    }
    for (const GS::ImageBinding& im : s.plan.images) {
        writes = writes || im.storage;
    }
}

void ParallelCp::PrepareDraw(Record& r, u64 index, const Cp::RegisterFile& regs) {
    r.prepared = false;
    r.stage_shared.reset();
    r.ps_shared.reset();
    r.state = nullptr;
    if (Vk::Executor::DrawFiltered(regs, r.draw)) {
        return; // no preparation: thread 2 sees the same registers and skips it as filtered
    }
    const u64 t0 = __rdtsc();
    {
        const GS::FineSample fine(front_fine_counter_);
        const GS::DrawPreparation& prep = frontend_->PrepareDraw(r.draw, regs, reader_);
        r.prep = prep;
        // P5.3: a preparation the native cache shares is kept by reference (immutable), not copied
        r.stage_shared = frontend_->SharedStage(GS::HwStage::Vs);
        r.ps_shared = frontend_->SharedStage(GS::HwStage::Ps);
        if (r.stage_shared == nullptr) {
            CopyStage(r.stage, frontend_->Stage(GS::HwStage::Vs));
        }
        if (r.ps_shared == nullptr) {
            CopyStage(r.ps, frontend_->Stage(GS::HwStage::Ps));
        }
        r.state = prep.ready ? frontend_->Pipelines().Graphics(prep.pipeline) : nullptr;
    }
    r.prepared = true;
    bool writes = false;
    NoteStageWrites(r.Vs(), index, writes);
    NoteStageWrites(r.Ps(), index, writes);
    if (writes) {
        frontend_->GuestMemoryChanged(); // (Executor::MarkWrites after the draw: no memoized preparation past it)
    }
    Bump(fc_.ticks, __rdtsc() - t0);
}

void ParallelCp::PrepareDispatch(Record& r, u64 index) {
    r.prepared = false;
    r.stage_shared.reset(); // (a compute stage is never shared: Execute takes r.stage)
    r.ps_shared.reset();
    r.state = nullptr;
    if (!r.has_cs) {
        return;
    }
    const u64 t0 = __rdtsc();
    {
        const GS::FineSample fine(front_fine_counter_);
        r.prep = frontend_->PrepareDispatch(r.dispatch, std::span<const u32, Cp::kComputeCount>(r.cs.values), reader_);
        CopyStage(r.stage, frontend_->Stage(GS::HwStage::Cs));
    }
    r.prepared = true;
    bool writes = false;
    NoteStageWrites(r.stage, index, writes);
    if (writes) {
        frontend_->GuestMemoryChanged(); // (Executor::NoteAccesses after the dispatch)
    }
    Bump(fc_.ticks, __rdtsc() - t0);
}

bool ParallelCp::FrontReader::Read(u64 addr, void* dst, u64 bytes) {
    Bump(p_.fc_.reads);
    if (p_.fthread_mode_) {
        // The frontend thread prepares record k: thread 2 runs only the records before it. With every one of them
        // executed, thread 2 is parked on k and thread 1 does not use the executor (k is queued, the queue not empty):
        // the executor decides exactly, as at thread 1's idle point. A possible pending GPU write otherwise: wait for
        // thread 2 to get there first (not a drain of thread 1's later records).
        const u64 k = p_.f_current_;
        bool ask = p_.head_.load(std::memory_order_acquire) == k;
        if (!ask && p_.MayBeGpuWritten(addr, bytes)) {
            Bump(p_.fc_.read_hits);
            p_.FrontWaitExecuted(k);
            ask = true;
        }
        if (ask) {
            p_.DropUnknownGpuWrites();
            Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0);
        }
    } else if (p_.Idle()) {
        p_.ApplyPending();
        Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0); // (exact: TimelineReader's check)
    } else if (p_.MayBeGpuWritten(addr, bytes)) {
        Bump(p_.fc_.read_hits);
        p_.Drain(DrainReason::FrontendRead);
        Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0);
    }
    // (TimelineReader's read: a run of the physical backing read before - its host view, what CopySparse copies)
    if (p_.map_gen_ != nullptr && GuestMemory::IsValidGpuMapping(addr, bytes)) {
        const u64 gen = p_.map_gen_->load(std::memory_order_acquire);
        for (const Run& r : runs_) {
            if (r.gen == gen && r.host != nullptr && addr >= r.begin && addr + bytes <= r.end) {
                std::memcpy(dst, r.host + (addr - r.begin), bytes);
                return true;
            }
        }
        const u64 block = (addr >> 20) + 1;
        const bool known_none =
            std::ranges::any_of(no_run_, [&](const auto& n) { return n.first == block && n.second == gen; });
        if (GuestMemory* gm = GetGuestMemory(); gm != nullptr && !known_none) {
            u8* host = nullptr;
            u64 rb = 0, rs = 0;
            if (gm->BackingRun(addr, &host, &rb, &rs) && host != nullptr && addr >= rb && addr + bytes <= rb + rs) {
                runs_[next_run_++ % runs_.size()] = {rb, rb + rs, host, gen};
                std::memcpy(dst, host + (addr - rb), bytes);
                return true;
            }
            no_run_[next_no_run_++ % no_run_.size()] = {block, gen};
        }
    }
    return GS::GameMemory().Read(addr, dst, bytes);
}

// ---- either thread (never both at once: thread 1 only while the queue is drained)

void ParallelCp::ApplyJournal(u64 end) {
    u64 pos = jhead_.load(std::memory_order_relaxed);
    while (pos < end) {
        const u32* w = JournalAt(pos);
        const u32 op = w[0] >> 28;
        if (op == kOpPad) {
            pos = (pos + kJournalWords) & ~u64{kJournalWords - 1};
            continue;
        }
        if (op == kOpWrite) {
            const u32 count = w[0] & 0xFFFFFF;
            replica_.Write(static_cast<Cp::Space>((w[0] >> 24) & 0xF), w[1], w + 2, count);
            pos += 2 + count;
        } else if (op == kOpReset) {
            replica_.Reset();
            pos += 1;
        } else if (op == kOpLoad && (w[0] & 0xFFFFFF) == Cp::kNumRegs) {
            replica_.Load(std::span<const u32, Cp::kNumRegs>(w + 1, Cp::kNumRegs));
            pos += 1 + Cp::kNumRegs;
        } else {
            // never written: a broken journal would make every later draw read wrong registers - stop executing
            Fail(fmt::format("parallel command processor: corrupt register journal at {} (word {:#x})", pos, w[0]));
            pos = end;
        }
    }
    jhead_.store(pos, std::memory_order_release);
}

void ParallelCp::ApplyDeferred(u64 before) {
    std::scoped_lock lock{deferred_mutex_};
    size_t n = 0;
    while (n < deferred_.size() && deferred_[n].before <= before) {
        if (buffers_ != nullptr) {
            buffers_->InvalidateIfCached(deferred_[n].addr, deferred_[n].bytes);
        }
        ++n;
    }
    deferred_.erase(deferred_.begin(), deferred_.begin() + static_cast<std::ptrdiff_t>(n));
    has_deferred_.store(!deferred_.empty(), std::memory_order_release);
}

// ---- P5.3D: the frontend thread (between thread 1 and thread 2)

void ParallelCp::FrontApplyJournal(u64 end) {
    u64 pos = fjhead_.load(std::memory_order_relaxed);
    while (pos < end) {
        const u32* w = JournalAt(pos);
        const u32 op = w[0] >> 28;
        if (op == kOpPad) {
            pos = (pos + kJournalWords) & ~u64{kJournalWords - 1};
            continue;
        }
        if (op == kOpWrite) {
            const u32 count = w[0] & 0xFFFFFF;
            freplica_.Write(static_cast<Cp::Space>((w[0] >> 24) & 0xF), w[1], w + 2, count);
            pos += 2 + count;
        } else if (op == kOpReset) {
            freplica_.Reset();
            pos += 1;
        } else if (op == kOpLoad && (w[0] & 0xFFFFFF) == Cp::kNumRegs) {
            freplica_.Load(std::span<const u32, Cp::kNumRegs>(w + 1, Cp::kNumRegs));
            pos += 1 + Cp::kNumRegs;
        } else {
            Fail(fmt::format("parallel command processor: corrupt register journal at {} (word {:#x}, frontend thread)",
                             pos, w[0]));
            pos = end;
        }
    }
    fjhead_.store(pos, std::memory_order_release);
}

void ParallelCp::FrontWaitExecuted(u64 index) {
    u64 h = head_.load(std::memory_order_acquire);
    if (h >= index) {
        return;
    }
    const u64 t0 = __rdtsc();
    while (h < index && __rdtsc() - t0 < spin_ticks_) {
        _mm_pause();
        h = head_.load(std::memory_order_acquire);
    }
    while (h < index) {
        f_wait_target_.store(index, std::memory_order_seq_cst);
        h = head_.load(std::memory_order_seq_cst);
        if (h < index) {
            head_.wait(h, std::memory_order_seq_cst);
        }
        f_wait_target_.store(0, std::memory_order_relaxed);
        h = head_.load(std::memory_order_acquire);
    }
    Bump(fc_.waits);
    Bump(fc_.wait_ticks, __rdtsc() - t0);
}

void ParallelCp::DropUnknownGpuWrites() {
    // (thread 2 parked before f_current_: the executor may be asked)
    while (!gw_.empty() && gw_.front().value == kLookup && gw_.front().record + kRecRing <= f_current_ &&
           !exec_.FrontendReadPending(gw_.front().begin, gw_.front().end - gw_.front().begin)) {
        gw_.pop_front();
        gw_stale_ = true;
    }
}

void ParallelCp::FrontEvent(u64 addr, u64 size, bool gpu) {
    if (!fthread_mode_ || !started_ || Idle()) {
        // the frontend thread (if any) parked: at once, after what is still queued for it (stream order)
        ApplyFrontEvents(~0ull);
        if (gpu) {
            frontend_->GpuWrote(addr, size);
        } else {
            frontend_->Invalidate(addr, size);
        }
        return;
    }
    std::scoped_lock lock{fe_mutex_};
    fe_.push_back({next_, addr, size, gpu});
    has_fe_.store(true, std::memory_order_release);
}

void ParallelCp::ApplyFrontEvents(u64 upto) {
    if (!has_fe_.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{fe_mutex_};
    size_t n = 0;
    while (n < fe_.size() && fe_[n].before <= upto) {
        const FrontEventEntry& e = fe_[n];
        if (e.gpu) {
            frontend_->GpuWrote(e.addr, e.size);
        } else {
            frontend_->Invalidate(e.addr, e.size);
        }
        ++n;
    }
    fe_.erase(fe_.begin(), fe_.begin() + static_cast<std::ptrdiff_t>(n));
    has_fe_.store(!fe_.empty(), std::memory_order_release);
}

void ParallelCp::FrontStep(Record& r, u64 index) {
    f_current_ = index;
    ApplyFrontEvents(index); // what thread 1 told the frontend before this record
    if (r.front_memo) {
        frontend_->GuestMemoryChanged(); // (a change thread 1 met before this record: stream order)
    }
    FrontApplyJournal(r.journal_end); // the registers as they were at the record's packet
    switch (r.kind) {
    case Kind::Draw:
        PrepareDraw(r, index, freplica_);
        break;
    case Kind::Dispatch:
        PrepareDispatch(r, index);
        break;
    case Kind::Copy:
        AddGpuWrite(r.dst, r.dst + r.bytes, index, kLookup); // (until thread 2 recorded it: the frontend's reads wait)
        break;
    default:
        break;
    }
    if ((index & 63) == 0) {
        RetireGpuWrites();
    }
    fc_.gw_now.store(gw_.size(), std::memory_order_relaxed);
}

void ParallelCp::FrontLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuFrontend");
    u64 k = fhead_.load(std::memory_order_relaxed);
    for (;;) {
        u64 t = tail_.load(std::memory_order_acquire);
        if (t == k) {
            const u64 w0 = __rdtsc();
            while (t == k && __rdtsc() - w0 < spin_ticks_) {
                _mm_pause();
                t = tail_.load(std::memory_order_acquire);
            }
            while (t == k) {
                f_waiting_.store(1, std::memory_order_seq_cst);
                t = tail_.load(std::memory_order_seq_cst);
                if (t == k) {
                    tail_.wait(k, std::memory_order_seq_cst);
                }
                f_waiting_.store(0, std::memory_order_relaxed);
                t = tail_.load(std::memory_order_acquire);
            }
            Bump(fc_.idle, __rdtsc() - w0);
        }
        const u64 b0 = __rdtsc();
        Record& r = slots_[k & (kSlots - 1)];
        const bool stop = r.kind == Kind::Stop;
        if (!stop && !failed_.load(std::memory_order_relaxed)) {
            try {
                FrontStep(r, k);
            } catch (const std::exception& e) {
                r.prepared = false;
                Fail(fmt::format("parallel command processor: the frontend thread threw: {}", e.what()));
            } catch (...) {
                r.prepared = false;
                Fail("parallel command processor: the frontend thread threw");
            }
        }
        Bump(fc_.busy, __rdtsc() - b0);
        ++k;
        fhead_.store(k, std::memory_order_seq_cst); // (the record's preparation published to thread 2)
        if (t2_waiting_.load(std::memory_order_seq_cst) != 0) {
            fhead_.notify_one();
        }
        if (stop) {
            break;
        }
    }
}

// ---- thread 2

void ParallelCp::Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuRecord");
    StallTrace::RegisterThread(StallTrace::Role::Record);
    const bool trace_records = StallTrace::DeepRecords();
    u64 h = head_.load(std::memory_order_relaxed);
    const auto add = [](std::atomic<u64>& a, u64 v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); };
    // (with the frontend thread, the records it prepared: fhead_)
    std::atomic<u64>& source = fthread_mode_ ? fhead_ : tail_;
    for (;;) {
        u64 t = source.load(std::memory_order_acquire);
        if (t == h) {
            const u64 w0 = __rdtsc();
            while (t == h && __rdtsc() - w0 < spin_ticks_) {
                _mm_pause();
                t = source.load(std::memory_order_acquire);
            }
            while (t == h) {
                t2_waiting_.store(1, std::memory_order_seq_cst);
                t = source.load(std::memory_order_seq_cst);
                if (t == h) {
                    add(t2_sleeps_, 1);
                    source.wait(h, std::memory_order_seq_cst);
                }
                t2_waiting_.store(0, std::memory_order_relaxed);
                t = source.load(std::memory_order_acquire);
            }
            const u64 w1 = __rdtsc();
            add(t2_idle_, w1 - w0);
            // P4.12: waited for record h - thread 1 pushed it at push_tsc_[h] (deep), the wake-up is the rest
            StallTrace::Span(StallTrace::Ev::RecordWait, w0, w1, push_tsc_ != nullptr ? push_tsc_[h & (kRecRing - 1)] : 0,
                             static_cast<u32>(h));
        }
        const u64 b0 = __rdtsc();
        Record& r = slots_[h & (kSlots - 1)];
        const bool stop = r.kind == Kind::Stop;
        const u16 kind = static_cast<u16>(r.kind); // (before Execute: a record is not touched by thread 1 until head_ moves)
        const u64 queue = r.kind == Kind::Dispatch ? r.dispatch.queue : 0; // (P5.4: the guest queue in the trace)
        if (!stop) {
            Execute(r, h);
            if (rec_value_ != nullptr) {
                rec_value_[h & (kRecRing - 1)].store(exec_.RecordedValue(), std::memory_order_relaxed);
            }
        }
        const u64 b1 = __rdtsc();
        add(t2_busy_, b1 - b0);
        if (trace_records) {
            StallTrace::Span(StallTrace::Ev::Record, b0, b1, queue, static_cast<u32>(h), kind);
        }
        add(t2_records_, 1);
        ++h;
        head_.store(h, std::memory_order_seq_cst);
        const u64 wt = t1_wait_target_.load(std::memory_order_seq_cst);
        const u64 ft = f_wait_target_.load(std::memory_order_seq_cst);
        if ((wt != 0 && h >= wt) || (ft != 0 && h >= ft)) {
            head_.notify_all(); // (thread 1 and the frontend thread may both wait on it)
        }
        if (stop) {
            break;
        }
    }
}

void ParallelCp::RunCopy(const Record& r) {
    const u32 every = ForcedCopyFallbackEvery();
    const bool forced = every != 0 && ++forced_fallback_n_ % every == 0;
    if (!forced && exec_.CopyOnTimeline(r.queue, r.seq, r.src, r.dst, r.bytes)) {
        return;
    }
    // the executor refused what thread 1 accepted by its shape: the processor's own fallback, here in stream order -
    // the earlier work completes, the CPU copies (caches invalidated), the copy's work number completes with the next
    // batch (its fences wait for it as for a recorded copy)
    const StallTrace::Scope trace_copy(StallTrace::Ev::CopyFallback, r.bytes);
    exec_.Synchronize(r.queue);
    if (copy_fallback_ == nullptr || !copy_fallback_(copy_ctx_, r.src, r.dst, r.bytes)) {
        // P4.11: what the processor does with a DMA it cannot copy (a failed submission: an error, the game goes on) -
        // here the copy is reported and its work number completed (its fences are not held forever); the rest of the
        // stream was processed already. Before: the whole backend stopped (exit code 5), a stricter end than the one
        // thread's.
        const u64 n = copy_failures_.load(std::memory_order_relaxed) + 1;
        copy_failures_.store(n, std::memory_order_relaxed);
        if (n <= 8) {
            SSPC_LOG(GPU, Error, "parallel command processor: a queued timeline copy {:#x} -> {:#x} ({} bytes) could be "
                                 "neither recorded nor copied by the CPU (not copied; its work completes)",
                     r.src, r.dst, r.bytes);
        }
    } else {
        exec_.GuestMemoryChanged();
        copy_fallbacks_.store(copy_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

void ParallelCp::Execute(Record& r, u64 index) {
    if (failed_.load(std::memory_order_relaxed)) {
        return; // consumed, not executed: thread 1's drains still complete; the backend stops at its next check
    }
    try {
        // (stage 2: thread 1 tells the frontend about guest memory changes in stream order; nothing from here)
        exec_.SetMemoBumps(frontend_ == nullptr);
        ApplyJournal(r.journal_end);
        if (verify_ && (r.kind == Kind::Draw || r.kind == Kind::Dispatch)) {
            verified_.store(verified_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (RegistersHash(replica_) != r.reg_hash) {
                verify_mismatches_.store(verify_mismatches_.load(std::memory_order_relaxed) + 1,
                                         std::memory_order_relaxed);
            }
        }
        if (has_deferred_.load(std::memory_order_acquire)) {
            ApplyDeferred(index); // the processor's writes made before this record was pushed
        }
        if (r.memory_changed) {
            exec_.GuestMemoryChanged();
        }
        switch (r.kind) {
        case Kind::Draw:
            if (frontend_ != nullptr) {
                Vk::PreparedCommand pc;
                if (r.prepared) {
                    pc = {&r.prep, &r.Vs(), &r.Ps(), r.state};
                }
                exec_.DrawPrepared(r.draw, pc);
            } else {
                exec_.Draw(r.draw);
            }
            break;
        case Kind::Dispatch:
            if (frontend_ != nullptr) {
                Vk::PreparedCommand pc;
                if (r.prepared) {
                    pc = {&r.prep, &r.stage, nullptr, nullptr};
                }
                exec_.DispatchPrepared(r.dispatch, r.has_cs ? &r.cs : nullptr, pc);
            } else {
                exec_.DispatchWith(r.dispatch, r.has_cs ? &r.cs : nullptr);
            }
            break;
        case Kind::CacheAction:
            exec_.CacheAction(r.cache);
            break;
        case Kind::Fence:
            exec_.Fence(r.fence);
            break;
        case Kind::Dma:
            exec_.Dma(r.dma);
            break;
        case Kind::WaitMet:
            exec_.WaitMetByHeldFence(r.queue);
            break;
        case Kind::MemoryAcquired:
            if (r.hook_done) {
                exec_.MemoryAcquiredApplied(r.queue, r.blocked, r.dst, r.seq, r.applied);
            } else {
                exec_.MemoryAcquired(r.queue, r.blocked, r.dst);
            }
            break;
        case Kind::Copy:
            RunCopy(r);
            break;
        case Kind::Journal:
        case Kind::Stop:
            break;
        }
    } catch (const std::exception& e) {
        Fail(fmt::format("parallel command processor: the recording thread threw: {}", e.what()));
    } catch (...) {
        Fail("parallel command processor: the recording thread threw");
    }
}

void ParallelCp::Fail(std::string text) {
    if (!failed_.load(std::memory_order_relaxed)) {
        failure_ = std::move(text);
        failed_.store(true, std::memory_order_release);
    }
}

// ---- Cp::GpuWork (thread 1)

void ParallelCp::Draw(const Cp::DrawDescriptor& draw) {
    Record& r = BeginRecord(Kind::Draw);
    r.draw = draw;
    if (verify_) {
        r.reg_hash = RegistersHash(cp_->Registers());
    }
    if (frontend_ != nullptr && !fthread_mode_) {
        PrepareDraw(r, next_, cp_->Registers()); // (as they are at the draw's packet)
    }
    ++t1_.draws;
    PushRecord();
}

void ParallelCp::Dispatch(const Cp::DispatchDescriptor& dispatch) {
    Record& r = BeginRecord(Kind::Dispatch);
    r.dispatch = dispatch;
    const Cp::ComputeRegisters* cs = cp_ != nullptr ? cp_->Compute(dispatch.queue) : nullptr;
    r.has_cs = cs != nullptr;
    if (cs != nullptr) {
        r.cs = *cs; // as they are at the packet: later packets of the queue change them
    }
    if (verify_) {
        r.reg_hash = RegistersHash(cp_->Registers());
    }
    if (frontend_ != nullptr && !fthread_mode_) {
        PrepareDispatch(r, next_);
    }
    ++t1_.dispatches;
    PushRecord();
}

// The processor's calls that need no answer: queued behind the draws before them (P4.10.4), or - the queue empty, thread
// 2 parked - made at once here; without `async_` each drains (the first prototype). Queued with the frontend on thread 1
// (stage 2): it is told about the call's guest memory change here, in stream order (thread 2 does not tell it).
#define SSPC_P410_QUEUED_CALL(KIND, SLOT, REASON, DIRECT, FILL)                                                        \
    do {                                                                                                               \
        if (!async_) {                                                                                                 \
            Drain(REASON);                                                                                             \
            DIRECT;                                                                                                    \
            SnapshotTimeline();                                                                                        \
        } else if (Idle()) {                                                                                           \
            ApplyPending();                                                                                            \
            ++t1_.direct_by[SLOT];                                                                                     \
            DIRECT;                                                                                                    \
            SnapshotTimeline();                                                                                        \
        } else {                                                                                                       \
            Record& r = BeginRecord(KIND);                                                                             \
            FILL;                                                                                                      \
            ++t1_.queued_by[SLOT];                                                                                     \
            PushRecord();                                                                                              \
            FrontMemoChanged();                                                                                        \
        }                                                                                                              \
    } while (false)

void ParallelCp::CacheAction(const Cp::CacheActionOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::CacheAction, kQCache, DrainReason::CacheAction, exec_.CacheAction(op), r.cache = op);
}

void ParallelCp::Fence(const Cp::FenceOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::Fence, kQFence, DrainReason::Fence, exec_.Fence(op), r.fence = op);
}

void ParallelCp::Dma(const Cp::DmaOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::Dma, kQDma, DrainReason::Dma, exec_.Dma(op), r.dma = op);
}

void ParallelCp::WaitMetByHeldFence(u32 queue) {
    SSPC_P410_QUEUED_CALL(Kind::WaitMet, kQWaitMet, DrainReason::WaitMet, exec_.WaitMetByHeldFence(queue),
                          r.queue = queue);
}

void ParallelCp::MemoryAcquired(u32 queue, bool blocked, u64 poll_address) {
    if (frontend_ != nullptr) {
        if (async_acquire_ && !Idle()) {
            // P4.11 (Options::async_acquire): the CPU writes applied here, the rest in stream order on thread 2
            const u64 met = blocked ? __rdtsc() : 0;
            const bool applied = exec_.RunAcquireHook();
            Record& r = BeginRecord(Kind::MemoryAcquired);
            r.queue = queue;
            r.blocked = blocked;
            r.dst = poll_address;
            r.seq = met;
            r.hook_done = true;
            r.applied = applied;
            ++t1_.queued_by[kQAcquired];
            ++t1_.async_acquires;
            PushRecord();
            FrontMemoChanged(); // (GuestSync's memo change)
            return;
        }
        // the CPU writes it applies invalidate the frontend's programs too: on the frontend's thread, the queue drained
        Drain(DrainReason::MemoryAcquired);
        ++t1_.direct_by[kQAcquired];
        exec_.MemoryAcquired(queue, blocked, poll_address);
        SnapshotTimeline();
        return;
    }
    SSPC_P410_QUEUED_CALL(Kind::MemoryAcquired, kQAcquired, DrainReason::MemoryAcquired,
                          exec_.MemoryAcquired(queue, blocked, poll_address),
                          (r.queue = queue, r.blocked = blocked, r.dst = poll_address, r.hook_done = false));
}

bool ParallelCp::CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) {
    if (!async_copies_) {
        Drain(DrainReason::TimelineCopy);
        const bool ok = exec_.CopyOnTimeline(queue, seq, src, dst, bytes);
        SnapshotTimeline();
        if (ok && frontend_ != nullptr) {
            AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
        }
        return ok;
    }
    // the executor's own refusals by shape (Executor::CopyOnTimeline): the processor copies (it drains first)
    if (bytes == 0 || src >= (1ull << 40) || dst >= (1ull << 40) || (src < dst + bytes && dst < src + bytes)) {
        ++t1_.copies_refused;
        return false;
    }
    if (Idle()) {
        ApplyPending();
        ++t1_.direct_by[kQCopy];
        const bool ok = exec_.CopyOnTimeline(queue, seq, src, dst, bytes);
        SnapshotTimeline();
        if (ok && frontend_ != nullptr) {
            AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
        }
        return ok;
    }
    // accepted: the executor records it in stream order (or, refusing, the CPU copies there - RunCopy); until it ran,
    // the processor's reads of the destination drain first (TimelinePending), the frontend's too (stage 2)
    Record& r = BeginRecord(Kind::Copy);
    r.queue = queue;
    r.seq = seq;
    r.src = src;
    r.dst = dst;
    r.bytes = bytes;
    ++t1_.queued_by[kQCopy];
    queued_tw_.emplace_back(dst, dst + bytes);
    qtw_lo_ = std::min(qtw_lo_, dst);
    qtw_hi_ = std::max(qtw_hi_, dst + bytes);
    if (frontend_ != nullptr && !fthread_mode_) {
        AddGpuWrite(dst, dst + bytes, next_, kLookup); // (the frontend thread notes it when it passes the record)
    }
    PushRecord();
    FrontMemoChanged(); // (the copy's NoteAccesses)
    return true;
}

void ParallelCp::Flip(const Cp::FlipMarker& marker) {
    Drain(DrainReason::Flip);
    const u64 v0 = exec_.TimelineWritesVersion();
    exec_.Flip(marker); // (the frontend's new frame, evictions: here, on the frontend's thread)
    if (frontend_ != nullptr && exec_.TimelineWritesVersion() != v0) {
        // images written back at the flip (EvictGpuImages): the executor's timeline writes now are pending until then
        std::vector<std::pair<u64, u64>> tw;
        exec_.TimelineWriteRanges(tw);
        const u64 value = exec_.RecordedValue();
        for (const auto& [b, e] : tw) {
            AddGpuWrite(b, e, next_, value);
        }
    }
    SnapshotTimeline();
}

void ParallelCp::Interrupt(u32 queue, Cp::FenceKind kind) {
    exec_.Interrupt(queue, kind); // (the guest's interrupt: no executor state)
}

void ParallelCp::Synchronize(u32 queue) {
    Drain(DrainReason::Synchronize);
    exec_.Synchronize(queue);
    SnapshotTimeline();
}

bool ParallelCp::WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) {
    Drain(DrainReason::TimelineWrite);
    const bool ok = exec_.WriteOnTimeline(queue, seq, dst, data, bytes);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr) {
        AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
    }
    return ok;
}

bool ParallelCp::GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                               u32 pattern) {
    Drain(DrainReason::TimelineGds);
    const bool ok = exec_.GdsOnTimeline(queue, seq, kind, memory, gds_offset, bytes, pattern);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr && kind == Cp::DmaKind::GdsToMemory) {
        AddGpuWrite(memory, memory + bytes, next_, exec_.RecordedValue());
    }
    return ok;
}

// ---- reports

std::string ParallelCp::FailureText() const {
    return failed_.load(std::memory_order_acquire) ? failure_ : std::string{};
}

u64 ParallelCp::Depth() const {
    return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
}

u64 ParallelCp::RecordingThreadCpuNs() const {
    if (!started_ || !thread_.joinable()) {
        return 0;
    }
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(const_cast<std::thread&>(thread_).native_handle(), &c, &e, &k, &u)) {
        return 0;
    }
    const auto ft = [](const FILETIME& f) { return (u64{f.dwHighDateTime} << 32 | f.dwLowDateTime) * 100; };
    return ft(k) + ft(u);
}

ParallelTotals ParallelCp::Totals() const {
    ParallelTotals t = t1_;
    t.t2_busy_ticks = t2_busy_.load(std::memory_order_relaxed);
    t.t2_idle_ticks = t2_idle_.load(std::memory_order_relaxed);
    t.t2_sleeps = t2_sleeps_.load(std::memory_order_relaxed);
    t.t2_records = t2_records_.load(std::memory_order_relaxed);
    t.copy_fallbacks = copy_fallbacks_.load(std::memory_order_relaxed);
    t.copy_failures = copy_failures_.load(std::memory_order_relaxed);
    const auto get = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    t.front_ticks = get(fc_.ticks);
    t.front_reads = get(fc_.reads);
    t.front_read_hits = get(fc_.read_hits);
    t.front_syncs = get(fc_.syncs);
    t.gpu_writes = get(fc_.gpu_writes);
    t.gw_max = get(fc_.gw_max);
    t.gw_block_hits = get(fc_.gw_block_hits);
    t.gw_rebuilds = get(fc_.gw_rebuilds);
    t.gw_now = fthread_mode_ ? get(fc_.gw_now) : gw_.size(); // (thread 1's own list without the frontend thread)
    t.tf_busy_ticks = get(fc_.busy);
    t.tf_idle_ticks = get(fc_.idle);
    t.tf_wait_ticks = get(fc_.wait_ticks);
    t.tf_waits = get(fc_.waits);
    return t;
}

std::string ParallelCp::Report() const {
    const ParallelTotals t = Totals();
    const double freq = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    const auto ms = [&](u64 ticks) { return static_cast<double>(ticks) * 1000.0 / freq; };
    std::string out = fmt::format(
        "parallel command processor (P4.10, SSPC_P410_PARALLEL_CP=1; other calls {}, timeline copies {}, frontend on "
        "{}): records {} (draws {}, dispatches {}, journal {}), executed {}; queue depth at a push: mean {:.1f}, max {}; "
        "register journal {} words\n"
        "  thread 1 waited: drains {:.0f} ms ({} drains that waited, {} of an empty queue), a full queue {:.0f} ms ({} "
        "times); blocking waits {}\n"
        "  thread 2: executing {:.0f} ms, waiting for records {:.0f} ms (blocking waits {}); processor writes' "
        "invalidations handed on {}, memory changes handed on {}\n"
        "  queued / made at once (queue empty): cache actions {}/{}, fences {}/{}, DMA reports {}/{}, waits met {}/{}, "
        "memory acquired {}/{}, timeline copies {}/{} (refused by shape {}, copied by the CPU after a refusal {}); "
        "processor reads drained for a pending timeline copy {}\n",
        async_ ? "queued" : "drained", async_copies_ ? "queued" : "drained",
        fthread_mode_ ? "its own thread (SSPC_P53_FRONTEND_THREAD=1)"
        : frontend_ != nullptr ? "the processor's thread" : "the recording thread",
        t.records, t.draws, t.dispatches,
        t.journal_records, t.t2_records,
        t.records != 0 ? static_cast<double>(t.depth_sum) / static_cast<double>(t.records) : 0.0, t.depth_max,
        t.journal_words, ms(t.t1_drain_ticks), t.drains, t.drains_empty, ms(t.t1_full_ticks), t.t1_full_waits,
        t.t1_sleeps, ms(t.t2_busy_ticks), ms(t.t2_idle_ticks), t.t2_sleeps, t.deferred_invalidations, t.memo_changes,
        t.queued_by[kQCache], t.direct_by[kQCache], t.queued_by[kQFence], t.direct_by[kQFence], t.queued_by[kQDma],
        t.direct_by[kQDma], t.queued_by[kQWaitMet], t.direct_by[kQWaitMet], t.queued_by[kQAcquired],
        t.direct_by[kQAcquired], t.queued_by[kQCopy], t.direct_by[kQCopy], t.copies_refused, t.copy_fallbacks,
        t.timeline_read_drains);
    if (frontend_ != nullptr) {
        out += fmt::format("  frontend on {}: {:.0f} ms; its reads {} (a GPU write possibly pending: {} - {}; the "
                           "executor waited for the GPU {}); GPU write ranges noted {} (at most {} at once, {} now), "
                           "block hits {}, bitmap rebuilds {}\n",
                           fthread_mode_ ? "its own thread" : "the processor's thread", ms(t.front_ticks), t.front_reads,
                           t.front_read_hits, fthread_mode_ ? "waited for thread 2" : "drained", t.front_syncs,
                           t.gpu_writes, t.gw_max, t.gw_now, t.gw_block_hits, t.gw_rebuilds);
    }
    if (fthread_mode_) {
        out += fmt::format("  frontend thread (P5.3D): preparing {:.0f} ms, waiting for records {:.0f} ms, waiting for "
                           "thread 2 {:.0f} ms ({} times)\n",
                           ms(t.tf_busy_ticks), ms(t.tf_idle_ticks), ms(t.tf_wait_ticks), t.tf_waits);
    }
    if (verify_) {
        out += fmt::format("  register replica verification (SSPC_P410_VERIFY=1): draws/dispatches compared {}, "
                           "different {}\n",
                           verified_.load(std::memory_order_relaxed), verify_mismatches_.load(std::memory_order_relaxed));
    }
    if (async_acquire_ || t.copy_failures != 0) {
        out += fmt::format("  P4.11: met memory waits queued with their CPU writes applied on the processor's thread "
                           "(SSPC_P411_ASYNC_ACQUIRE=1) {}; queued copies neither recorded nor copied {}\n",
                           t.async_acquires, t.copy_failures);
    }
    out += "  drains that waited, by reason:";
    for (size_t i = 0; i < t.drains_by.size(); ++i) {
        if (t.drains_by[i] != 0) {
            out += fmt::format(" {} {};", DrainReasonName(static_cast<DrainReason>(i)), t.drains_by[i]);
        }
    }
    out += "\n";
    if (Failed()) {
        out += "  FAILED: " + failure_ + "\n";
    }
    return out;
}

} // namespace SecondSon::Gpu::Native
