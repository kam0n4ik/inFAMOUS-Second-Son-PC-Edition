// SecondSonPC — the parallel command processor (P4.10). See parallel_cp.h.
#include <windows.h>

#include "runtime/gpu/parallel_cp.h"

#include <intrin.h>

#include <algorithm>
#include <optional>
#include <bit>
#include <cstring>
#include <exception>

#include <fmt/format.h>

#include "base/log.h"
#include "engine/render/draw_runs.h"
#include "runtime/gpu/cp_r6_opt.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/r8/cp_native_diag.h"
#include "runtime/gpu/r8/cp_profile.h"
#include "runtime/gpu/r10/spread.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_emit.h"
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
    R8_CP_PROF(CpProfile::Sec::CopyStage);
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

/// FB-6 verify: the copies of one view (vectors of trivially copyable bindings: their bytes).
template <typename T>
bool SameBytes(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
bool SameView(const GS::StageView& a, const GS::StageView& b) {
    return a.status == b.status && a.address == b.address && a.program == b.program &&
           a.footer_hash == b.footer_hash && a.module == b.module &&
           std::memcmp(&a.runtime, &b.runtime, sizeof(a.runtime)) == 0 && a.start == b.start && a.end == b.end &&
           a.program_error == b.program_error && a.plan.stage == b.plan.stage && SameBytes(a.plan.flat, b.plan.flat) &&
           SameBytes(a.plan.buffers, b.plan.buffers) && SameBytes(a.plan.images, b.plan.images) &&
           SameBytes(a.plan.samplers, b.plan.samplers) && SameBytes(a.plan.vertices, b.plan.vertices) &&
           a.plan.status_counts == b.plan.status_counts && a.plan.error == b.plan.error;
}

/// SSPC_CITY_ENGINE_OPT=verify: two preparations field by field (bindings built twice differ in padding bytes).
bool SameStageView(const GS::StageView& x, const GS::StageView& y) {
    const auto bufs = [](const GS::BufferBinding& a, const GS::BufferBinding& b) {
        return a.binding == b.binding && a.kind == b.kind && a.desc.raw == b.desc.raw && a.address == b.address &&
               a.size == b.size && a.written == b.written && a.formatted == b.formatted && a.status == b.status;
    };
    const auto imgs = [](const GS::ImageBinding& a, const GS::ImageBinding& b) {
        return a.binding == b.binding && a.count == b.count && a.desc.raw == b.desc.raw && a.view_type == b.view_type &&
               a.storage == b.storage && a.depth == b.depth && a.array == b.array && a.atomic == b.atomic &&
               a.fmask == b.fmask && a.status == b.status;
    };
    const auto smps = [](const GS::SamplerBinding& a, const GS::SamplerBinding& b) {
        return a.binding == b.binding && a.desc.raw == b.desc.raw && a.status == b.status;
    };
    const auto vtxs = [](const GS::VertexBinding& a, const GS::VertexBinding& b) {
        return a.attribute == b.attribute && a.fetch == b.fetch && a.read == b.read && a.desc.raw == b.desc.raw &&
               a.address == b.address && a.size == b.size && a.status == b.status;
    };
    return x.status == y.status && x.address == y.address && x.program == y.program && x.footer_hash == y.footer_hash &&
           x.module == y.module && std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) == 0 && x.start == y.start &&
           x.end == y.end && x.program_error == y.program_error && x.plan.flat == y.plan.flat &&
           std::ranges::equal(x.plan.buffers, y.plan.buffers, bufs) &&
           std::ranges::equal(x.plan.images, y.plan.images, imgs) &&
           std::ranges::equal(x.plan.samplers, y.plan.samplers, smps) &&
           std::ranges::equal(x.plan.vertices, y.plan.vertices, vtxs) &&
           x.plan.status_counts == y.plan.status_counts && x.plan.error == y.plan.error;
}

} // namespace

const GS::StageView& ParallelCp::ReuseView(GS::StageView& rec, bool shared, bool same, GS::StageView& last) {
    if (shared || stage_reuse_ == 0) {
        return rec;
    }
    const auto bump = [](std::atomic<u64>& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
    if (same) {
        bump(reuse_same_);
        if (stage_reuse_ == 2 && !SameView(rec, last)) {
            bump(reuse_mismatches_);
        }
        return last;
    }
    bump(reuse_copied_);
    std::swap(rec, last); // (the record keeps the old buffers: thread 1 copies into their capacity later)
    return last;
}

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
      async_writes_(options.async && options.async_copies && options.async_writes),
      async_gds_only_(options.async_gds_only),
      frontend_(options.timeline != nullptr ? options.frontend : nullptr), map_gen_(options.map_gen),
      timeline_(options.timeline), slots_(std::make_unique<Record[]>(kSlots)),
      journal_(std::make_unique<u32[]>(kJournalWords)) {
    if (frontend_ != nullptr) {
        gw_bits_ = std::make_unique<u64[]>(kGwWords);
        rec_value_ = std::make_unique<std::atomic<u64>[]>(kRecRing);
    }
    fthread_mode_ = options.frontend_thread && frontend_ != nullptr;
    if (char v[16] = {}; frontend_ != nullptr && GetEnvironmentVariableA("SSPC_CITY_BATCH", v, sizeof(v)) > 0) {
        const std::string_view sv(v);
        batch_on_ = sv == "1" || sv.starts_with("ab:");
        batch_ab_ = sv.starts_with("ab:") ? std::max<u32>(1, static_cast<u32>(std::strtoul(v + 3, nullptr, 10))) : 0;
        if (batch_on_) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_BATCH={}: draws differing only in their geometry are recorded as one draw", sv);
        }
    }
    // (front mode - the frontend thread: GEOFAST and the runs are that thread's, against its replica)
    if (char v[16] = {}; frontend_ != nullptr && GetEnvironmentVariableA("SSPC_CITY_GEOFAST", v, sizeof(v)) > 0) {
        const std::string_view sv(v);
        geofast_ = sv == "1" ? 1 : sv == "verify" ? 2 : 0;
        if (geofast_ == 2) {
            geofast_check_ = std::make_unique<Record>();
        }
        if (geofast_ != 0) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_GEOFAST={}: a draw differing from the one before it only in its geometry takes "
                                "that one's preparation with its own vertex bindings", sv);
        }
    }
    if (char v[8] = {}; geofast_ != 0 && GetEnvironmentVariableA("SSPC_CITY_DRAW_CACHE", v, sizeof(v)) > 0 && v[0] == '1') {
        drawcache_ = true;
        SSPC_LOG(GPU, Info, "SSPC_CITY_DRAW_CACHE=1: ... and a draw differing in other user data of stages without an SRT "
                            "walk, with their resources decoded again (the same module)");
    }
    if (char v[16] = {}; geofast_ != 0 && GetEnvironmentVariableA("SSPC_CITY_ENGINE_RESOURCES", v, sizeof(v)) > 0 &&
                         v[0] != '0') {
        // CITY-OPT Phase 6: the template for stages with an SRT walk too (verify: SSPC_CITY_GEOFAST's)
        resources_ = std::string_view(v) == "verify" ? 2 : 1;
        if (resources_ == 2 && geofast_ != 2) {
            geofast_ = 2;
            geofast_check_ = std::make_unique<Record>();
        }
        SSPC_LOG(GPU, Info, "SSPC_CITY_ENGINE_RESOURCES={}: a draw differing from the one before it in registers the "
                            "stages' SRT walks did not read takes that draw's walks again (CITY-OPT Phase 6){}", v,
                 resources_ == 2 ? " - verify: each also prepared in full and compared" : "");
    }
    if (char v[4] = {}; frontend_ != nullptr && !fthread_mode_ &&
                        GetEnvironmentVariableA("SSPC_DIAG_ENGINE_SRT", v, sizeof(v)) == 1 && v[0] == '1') {
        srt_diag_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTICS: SSPC_DIAG_ENGINE_SRT=1 - draw preparations by engine function "
                               "(gpu_native.txt)");
    }
    if (char v[32] = {}; frontend_ != nullptr && !fthread_mode_ &&
                         GetEnvironmentVariableA("SSPC_DIAG_PREP_CENSUS", v, sizeof(v)) > 0) {
        char* end = nullptr;
        census_first_ = std::strtoull(v, &end, 10);
        const u64 n = *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 300;
        census_end_ = census_first_ + std::max<u64>(n, 1);
        SSPC_LOG(GPU, Info, "SSPC_DIAG_PREP_CENSUS={}: the draws of flips {}..{} by program (gpu_native.txt)", v,
                 census_first_, census_end_ - 1);
    }
    if (char v[16] = {}; frontend_ != nullptr && GetEnvironmentVariableA("SSPC_CITY_ENGINE_OPT", v, sizeof(v)) > 0 &&
                         v[0] != '0' &&
                         std::string_view(v) != "ahead") { // (=ahead: the processor's RunAhead only - A/B)
        runs_ = std::string_view(v) == "verify" ? 2 : 1;
        if (std::string_view(v).starts_with("ab:")) { // (image A/B: runs in odd blocks of n flips only)
            runs_ab_ = std::max<u32>(1, static_cast<u32>(std::strtoul(v + 3, nullptr, 10)));
        }
        SSPC_LOG(GPU, Info, "SSPC_CITY_ENGINE_OPT={}: the engine's tagged mesh draws that differ only in their geometry "
                            "are one record (the first prepared, the others their geometry only){}", v,
                 runs_ == 2 ? " - verify: each also prepared in full and compared" : "");
        if (char g[16] = {}; GetEnvironmentVariableA("SSPC_CITY_ENGINE_GBUFFER_RUNS", g, sizeof(g)) > 0 && g[0] != '0') {
            gruns_ = std::string_view(g) == "verify" ? 2 : 1;
            if (std::string_view(g).starts_with("ab:")) { // (image A/B: in odd blocks of n flips only)
                gruns_ab_ = std::max<u32>(1, static_cast<u32>(std::strtoul(g + 3, nullptr, 10)));
            }
            SSPC_LOG(GPU, Info, "SSPC_CITY_ENGINE_GBUFFER_RUNS={}: runs of the instanced mesh draws (0x1a1770) too, their "
                                "members with their own PS constant buffers (CITY-OPT Phase 6)", g);
            if (char m[16] = {}; GetEnvironmentVariableA("SSPC_CITY_GB_RUN_MATERIAL", m, sizeof(m)) > 0 && m[0] != '0') {
                gmat_ = std::string_view(m) == "verify" ? 2 : 1;
                SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_CITY_GB_RUN_MATERIAL={} - G-buffer members of another material "
                                       "join their run with their own PS textures and samplers{}",
                         m, gmat_ == 2 ? " (verify: each also prepared in full and compared)" : "");
            }
        }
        if (char p[16] = {}; GetEnvironmentVariableA("SSPC_R10_RUN_PS", p, sizeof(p)) > 0 && p[0] != '0' &&
                             std::string_view(p) != "off") {
            run_ps_ = std::string_view(p) == "verify" ? 2 : 1;
            char m[8] = {};
            run_ps_mat_ = GetEnvironmentVariableA("SSPC_R10_RUN_PS_MATERIAL", m, sizeof(m)) > 0 && m[0] == '1' ? 1 : 0;
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R10_RUN_PS={} - members of mesh 0x1a1960 / shadow 0x43d480 runs "
                                   "with other PS user data join with their PS plan decoded again{}{}",
                     p, run_ps_mat_ != 0 ? ", their own T#s / S#s too (SSPC_R10_RUN_PS_MATERIAL=1)" : "",
                     run_ps_ == 2 ? " - verify: each also prepared in full and compared" : "");
        }
    }
    if (fthread_mode_) {
        // SSPC_FP_RUN_HOLD_US (default 50): a held run with no record to join is published after this long (thread 1
        // waiting for thread 2 publishes it at once)
        char v[16] = {};
        const u64 us = GetEnvironmentVariableA("SSPC_FP_RUN_HOLD_US", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) : 50;
        f_hold_ticks_ = Kernel::Clock::TscFrequency() / 1'000'000 * us;
        SSPC_LOG(GPU, Info, "front mode: thread 1 parses the packets only; the frontend thread prepares the draws{}{} "
                            "(a held run published after {} us without a record)",
                 geofast_ != 0 ? ", takes the geometry-only ones from the draw before" : "",
                 runs_ != 0 ? " and builds the runs" : "", us);
    }
    u64 ring = fthread_mode_ ? kSlots : 1024;
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_CP_RING", v, sizeof(v)) > 0) {
        ring = std::bit_ceil(std::clamp<u64>(std::strtoull(v, nullptr, 10), 64, kSlots));
    }
    slot_mask_ = ring - 1;
    depth_limit_ = ring;
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_CP_QUEUE_DEPTH", v, sizeof(v)) > 0) {
        depth_limit_ = std::clamp<u64>(std::strtoull(v, nullptr, 10), 16, ring);
    }
    SSPC_LOG(GPU, Info, "parallel CP: a ring of {} records (SSPC_CP_RING), at most {} queued ahead of thread 2 "
                        "(SSPC_CP_QUEUE_DEPTH), {} bytes a record",
             ring, depth_limit_, sizeof(Record));
    // (front mode too: the hook's frontend invalidations reach the frontend thread as events of the next record -
    // Backend::FrontendInvalidate - in stream order)
    async_acquire_ = options.async_acquire && async_ && frontend_ != nullptr;
    journal_changes_only_ = options.journal_changes_only;
    // (front mode: the frontend thread begins the frontend's frame when it passes the flip record - FrontStep)
    async_flip_ = options.async_flip && frontend_ != nullptr;
    if (char v[16] = {}; frontend_ != nullptr && !fthread_mode_ &&
                         GetEnvironmentVariableA("SSPC_FB_STAGE_REUSE", v, sizeof(v)) > 0) {
        stage_reuse_ = std::string_view(v) == "verify" ? 2 : v[0] == '1' ? 1 : 0;
        if (stage_reuse_ != 0) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_FB_STAGE_REUSE={} - unchanged draw stages not copied", v);
        }
    }
    if (frontend_ != nullptr && !fthread_mode_ && R8::PrepDiag::On()) {
        r8_diag_ = std::make_unique<R8::PrepDiag>();
    }
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
    cp.MutableRegisters().SetSinkChangesOnly(journal_changes_only_);
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
        cp_->MutableRegisters().SetSinkChangesOnly(false);
        exec_.SetRegisters(&cp_->Registers());
    }
}

// ---- thread 1

ParallelCp::Record& ParallelCp::BeginRecord(Kind kind) {
    if (run_open_ && R6::Opt().diag) {
        R6::Bump(R6::Count().run_closed_by[static_cast<u32>(kind) & (R6::kKinds - 1)]); // (R6-P diagnostics)
    }
    CloseRun(); // (a held run's first draw goes before any other record)
    if (next_ - head_.load(std::memory_order_acquire) >= depth_limit_) {
        WaitHead(next_ - depth_limit_ + 1, true, kWhyFull); // backpressure: the oldest slot executed
    }
    Record& r = slots_[next_ & slot_mask_];
    r.kind = kind;
    r.front_memo = pending_front_memo_; // (frontend thread: told before it prepares this record)
    pending_front_memo_ = false;
    r.run_barrier = pending_run_barrier_;
    pending_run_barrier_ = false;
    r.joined = false;
    r.memory_changed = pending_memory_changed_;
    if (pending_memory_changed_) {
        pending_memory_changed_ = false;
        ++t1_.memo_changes;
    }
    return r;
}

void ParallelCp::PushRecord(bool keep_journal_end) {
    Record& r = slots_[next_ & slot_mask_];
    if (!keep_journal_end) {
        r.journal_end = jtail_; // every register write of the stream before the record
    }
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
    R8_CP_PROF(full ? (why == kWhyJournal ? CpProfile::Sec::WaitJournal : CpProfile::Sec::WaitFull) : CpProfile::Drain(why));
    const u64 h0 = h;
    const u64 t0 = __rdtsc();
    // (R8: a full queue while the frontend thread holds a run - filed apart below)
    const bool held = full && fthread_mode_ && f_hold_lead_.load(std::memory_order_relaxed) != 0;
    if (fthread_mode_) {
        f_flush_.store(1, std::memory_order_seq_cst); // (front mode: a run the frontend thread holds is published now)
    }
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
    if (fthread_mode_) {
        f_flush_.store(0, std::memory_order_relaxed);
    }
    const u64 t1 = __rdtsc();
    const u64 ticks = t1 - t0;
    if (full) {
        t1_.t1_full_ticks += ticks;
        ++t1_.t1_full_waits;
        if (held) {
            t1_full_held_.store(t1_full_held_.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
            t1_full_held_n_.store(t1_full_held_n_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    } else {
        t1_.t1_drain_ticks += ticks;
        if (why < t1_.drain_ticks_by.size()) {
            t1_.drain_ticks_by[why] += ticks;
        }
    }
    // P4.12: what thread 1 waited for - thread 2 executing records up to `target` (a: the target; b: records then
    // queued; c: the drain's reason)
    StallTrace::Span(!full ? StallTrace::Ev::Drain
                     : why == kWhyJournal ? StallTrace::Ev::JournalFull
                                          : StallTrace::Ev::QueueFull,
                     t0, t1, target, static_cast<u32>(target - std::min(target, h0)), why);
}

void ParallelCp::ApplyPending() {
    R8_CP_PROF(CpProfile::Sec::ApplyPending);
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
    CloseRun();
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

bool ParallelCp::RoundEndQueued() {
    if (!started_ || !async_ || Idle()) {
        return false;
    }
    BeginRecord(Kind::RoundEnd);
    PushRecord();
    if (frontend_ != nullptr && !fthread_mode_) {
        // (only records already executed: safe with thread 2 running; with the frontend thread the list is that
        // thread's - it retires them itself, FrontStep - and a second writer here corrupted it: CITY-OPT)
        RetireGpuWrites();
    }
    return true;
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
    CloseRun();
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
    CloseRun(); // (before: the first record after the write - after a held run's first draw)
    pending_run_barrier_ = fthread_mode_; // (front mode: the frontend thread's run ends before that record)
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
    R8_CP_PROF_LIGHT(CpProfile::Light::JournalWrite);
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

bool ParallelCp::RedecodeStage(const GS::StageView& v, std::span<const u32, 16> user_data,
                               std::span<const std::array<u32, 4>> vertex_reads, GS::ResourceBindingPlan& out) {
    R8_CP_PROF(CpProfile::Sec::Redecode);
    // a stage without an SRT walk: its plan is the registers' decoding alone (ResolveResources with an empty walk);
    // CITY-OPT Phase 6 (resources_): one with a walk whose output stays - the registers it read are the same
    if (v.program == nullptr || v.module == nullptr || v.plan.flat.size() < 16 || !v.plan.error.empty() ||
        !v.module->keyed) {
        return false;
    }
    const bool walked = v.plan.flat.size() != 16;
    const auto refuse = [&](u32 why) {
        Bump(res_refused_[why]);
        return false;
    };
    // a walk that read a register that differs (the CUE's table copied to new frame memory at each commit): walked
    // again for these registers (the same walk program, memory and decoding as the full preparation)
    bool rewalk = false;
    if (walked) {
        if (resources_ == 0 && gruns_ == 0) {
            return false;
        }
        // (the flattened data's first 16 dwords are the registers of the draw it was prepared for)
        u32 diff = 0;
        for (u32 i = 0; i < 16; ++i) {
            diff |= v.plan.flat[i] != user_data[i] ? 1u << i : 0u;
        }
        rewalk = (diff & v.plan.srt.user_data_read) != 0;
        if (!rewalk && (v.plan.srt.user_data_written || v.plan.srt.faults != 0)) {
            return refuse(2);
        }
    }
    const GS::Layout* layout = v.program->FindLayout(v.runtime);
    if (layout == nullptr || layout->module == nullptr) {
        return false;
    }
    const GS::Module& first = *layout->module; // (PrepareStage decodes with the first module of the layout)
    if (first.state.load(std::memory_order_acquire) != GS::ModuleState::Ready || !first.prepared ||
        !first.srt_error.empty() || first.srt.Empty() == walked) {
        return walked ? refuse(2) : false;
    }
    const GS::ShaderReflection& r = first.result.reflection;
    const GS::ShaderReflection& mr = v.module->result.reflection;
    if (mr.buffers.size() != r.buffers.size() || mr.images.size() != r.images.size() ||
        mr.samplers.size() != r.samplers.size() || mr.flatbuf_dwords != r.flatbuf_dwords ||
        (walked ? v.plan.flat.size() != std::max<size_t>(r.flatbuf_dwords, 16) : r.flatbuf_dwords > 16)) {
        return walked ? refuse(3) : false; // (the module's own reflection would decide the plan)
    }
    const GS::FetchShader* fetch = r.fetch.present ? &mr.fetch : nullptr; // (the fetch shader at the same address)
    if (fetch != nullptr && (!mr.fetch.present || vertex_reads.size() != mr.fetch.attributes.size())) {
        return false;
    }
    if (rewalk) {
        // (ResolveResources as PrepareStage calls it: the vertex V#s read from the draw's tables again)
        if (!GS::ResolveResources(r, first.srt, v.start, fetch, user_data, v.address, reader_, out) ||
            out.srt.faults != 0 || out.srt.user_data_written || !out.error.empty()) {
            return refuse(6);
        }
    } else {
        if (walked) {
            out.flat.assign(v.plan.flat.begin(), v.plan.flat.end()); // (the walk's output, unchanged)
            std::copy(user_data.begin(), user_data.end(), out.flat.begin());
        } else {
            out.flat.assign(user_data.begin(), user_data.end());
        }
        GS::DecodeResources(r, v.start, fetch, user_data, v.address, GS::VertexSource{nullptr, vertex_reads}, out);
        // (the walk the plan comes from: its registers read stay the next draw's check; no reads of its own)
        out.srt = walked ? v.plan.srt : GS::SrtProgram::RunStats{};
        out.srt.reads = out.srt.line_reads = 0;
        out.error.clear();
    }
    GS::BuildSpecializationKey(v.runtime, v.start, r, fetch, out, dc_key_);
    if (!(dc_key_ == v.module->key) || !(GS::EndOf(v.start, out) == v.end)) {
        dc_key_refused_.store(dc_key_refused_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return walked ? refuse(!(dc_key_ == v.module->key) ? 4 : 5) : false; // another module, or other bindings
    }
    if (rewalk) {
        Bump(v.plan.stage == GS::HwStage::Ps ? res_rewalk_ps_ : res_rewalk_vs_);
    } else if (walked) {
        Bump(v.plan.stage == GS::HwStage::Ps ? res_walk_ps_ : res_walk_vs_);
    }
    return true;
}

bool ParallelCp::PrepareGeometryOnly(const Record& r, u64 index, Record& out) {
    R8_CP_PROF(CpProfile::Sec::GeoFast);
    const auto no = [&](u8 why) {
        geofast_why_ = why;
        return false;
    };
    if (index == 0 || geofast_prev_ != index - 1 || r.memory_changed ||
        frontend_->MemoryGeneration() != geofast_gen_) {
        return no(1);
    }
    const Record& p = slots_[(index - 1) & slot_mask_];
    if (p.kind != Kind::Draw || !p.prepared || !p.prep.ready || p.state == nullptr || p.stage_shared != nullptr ||
        p.ps_shared != nullptr) {
        return no(2);
    }
    const Cp::DrawDescriptor& a = p.draw;
    const Cp::DrawDescriptor& b = r.draw;
    using G = Cp::Group;
    constexpr Cp::GroupMask kGeometryGroups = (Cp::GroupMask{1} << static_cast<u32>(G::VsUserData)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::IndexState)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::DrawParams));
    constexpr u8 kVsPs = (1u << static_cast<u32>(Cp::Stage::Vs)) | (1u << static_cast<u32>(Cp::Stage::Ps));
    const size_t vs = static_cast<size_t>(Cp::Stage::Vs), ps = static_cast<size_t>(Cp::Stage::Ps);
    const bool templ = drawcache_ || resources_ != 0; // (other user data: the stages' plans decoded again)
    const Cp::GroupMask allowed =
        kGeometryGroups | (templ ? Cp::GroupMask{1} << static_cast<u32>(G::PsUserData) : Cp::GroupMask{0});
    if ((b.changed & ~allowed) != 0) {
        return no(3);
    }
    if (a.active_stages != kVsPs || b.active_stages != kVsPs || b.indirect || a.indirect) {
        return no(4);
    }
    if (a.shaders[vs].address != b.shaders[vs].address || a.shaders[vs].rsrc1 != b.shaders[vs].rsrc1 ||
        a.shaders[vs].rsrc2 != b.shaders[vs].rsrc2 || a.shaders[ps].address != b.shaders[ps].address ||
        a.shaders[ps].rsrc1 != b.shaders[ps].rsrc1 || a.shaders[ps].rsrc2 != b.shaders[ps].rsrc2) {
        return no(5);
    }
    const bool ps_changed = a.user_data[ps] != b.user_data[ps];
    if (ps_changed && !templ) {
        return no(6);
    }
    const GS::StageView& pv = p.stage;
    // (copies of an SRT walk: they may follow a pointer that changed - Phase 6 checks the registers the walk read)
    if (pv.status != GS::StageStatus::Ready || pv.module == nullptr ||
        (pv.plan.flat.size() != 16 && (resources_ == 0 || pv.plan.flat.size() < 16)) ||
        (pv.plan.vertices.empty() && !templ)) {
        return no(7);
    }
    const GS::FetchShader& fetch = pv.module->result.reflection.fetch;
    if ((!fetch.present || fetch.sgpr_base + 1 >= 16) && (!templ || !pv.plan.vertices.empty())) {
        return no(8);
    }
    u32 fetch_only = 0;
    if (fetch.present && fetch.sgpr_base + 1 < 16) {
        for (const GS::FetchAttribute& at : fetch.attributes) {
            fetch_only |= at.sgpr_base + 1 < 16 ? 3u << at.sgpr_base : 0u;
        }
        fetch_only &= ~(3u << fetch.sgpr_base); // (the fetch shader's own address must be the same)
    }
    bool vs_other = false; // VS user data past the vertex tables changed: its resources decoded again
    u32 vs_diff = 0;
    for (u32 i = 0; i < 16; ++i) {
        if (a.user_data[vs][i] != b.user_data[vs][i]) {
            vs_diff |= 1u << i;
        }
        if (a.user_data[vs][i] != b.user_data[vs][i] && ((fetch_only >> i) & 1) == 0) {
            if (!templ || (fetch.present && (i == fetch.sgpr_base || i == fetch.sgpr_base + 1))) {
                return no(9);
            }
            vs_other = true;
        }
    }
    if (pv.plan.flat.size() != 16 && !vs_other &&
        ((vs_diff & pv.plan.srt.user_data_read) != 0 || pv.plan.srt.user_data_written || pv.plan.srt.faults != 0)) {
        vs_other = true; // (Phase 6: the VS walk read a vertex table register, or is not reusable - RedecodeStage)
    }
    // nothing the previous draw prepared writes memory (its notes would be due again)
    for (const GS::StageView* s : {&pv, &p.ps}) {
        for (const GS::BufferBinding& bb : s->plan.buffers) {
            if (bb.written || (bb.kind != GS::BufferKind::Guest && bb.kind != GS::BufferKind::Flatbuf &&
                               bb.kind != GS::BufferKind::ClipPlanes)) {
                return no(10);
            }
        }
        for (const GS::ImageBinding& im : s->plan.images) {
            if (im.storage) {
                return no(10);
            }
        }
    }
    // the vertex bindings from this draw's table (shader_resources.cpp DecodeResources: the same reads and results)
    std::vector<GS::VertexBinding>& nv = geofast_vertices_;
    nv.assign(pv.plan.vertices.begin(), pv.plan.vertices.end());
    for (GS::VertexBinding& v : nv) {
        const GS::FetchAttribute& at = v.fetch;
        if (v.status != GS::ResourceStatus::Ok || at.sgpr_base + 1 >= 16) {
            return no(11);
        }
        const u64 table = (b.user_data[vs][at.sgpr_base] | (u64{b.user_data[vs][at.sgpr_base + 1]} << 32)) &
                          0xFFFFFFFFFFFFull;
        const u64 where = table + u64{at.dword_offset} * 4;
        GS::BufferDesc d;
        if (where >= (1ull << 40) || !reader_.Read(where, d.raw.data(), 16)) {
            return no(11);
        }
        v.read = d.raw;
        if (at.data_format != 0) {
            d.raw[3] = (d.raw[3] & ~((0xFu << 15) | (7u << 12))) | (at.data_format << 15) | (at.num_format << 12);
        }
        const u64 addr = d.Base() + at.inst_offset, size = d.Size();
        if (!d.IsBuffer() || d.NumRecords() == 0 || addr >= (1ull << 40) || size > (1ull << 40) - addr ||
            (d.raw[3] & 0x7FFFFu) != (v.desc.raw[3] & 0x7FFFFu)) {
            return no(11); // (not Ok, or another specialization of the module)
        }
        v.desc = d;
        v.address = addr;
        v.size = size;
    }
    // SSPC_CITY_DRAW_CACHE: the stages whose other user data changed - their resources from this draw's registers
    if (vs_other) {
        dc_reads_.clear();
        for (const GS::VertexBinding& v : nv) {
            dc_reads_.push_back(v.read);
        }
        if (!RedecodeStage(pv, b.user_data[vs], dc_reads_, dc_vs_)) {
            return no(12);
        }
    }
    if (ps_changed && !RedecodeStage(p.ps, b.user_data[ps], {}, dc_ps_)) {
        return no(13);
    }
    geofast_why_ = 0;
    out.prep = p.prep;
    out.state = p.state;
    CopyStage(out.stage, pv);
    if (vs_other) {
        out.stage.plan.flat.assign(dc_vs_.flat.begin(), dc_vs_.flat.end());
        out.stage.plan.buffers.assign(dc_vs_.buffers.begin(), dc_vs_.buffers.end());
        out.stage.plan.images.assign(dc_vs_.images.begin(), dc_vs_.images.end());
        out.stage.plan.samplers.assign(dc_vs_.samplers.begin(), dc_vs_.samplers.end());
        out.stage.plan.vertices.assign(dc_vs_.vertices.begin(), dc_vs_.vertices.end());
        out.stage.plan.status_counts = dc_vs_.status_counts;
        out.stage.plan.srt = dc_vs_.srt; // (Phase 6: a walk again - its registers read)
        dc_vs_hits_.store(dc_vs_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    } else {
        out.stage.plan.vertices.assign(nv.begin(), nv.end());
        std::copy(b.user_data[vs].begin(), b.user_data[vs].end(), out.stage.plan.flat.begin());
        if (pv.plan.flat.size() != 16) {
            Bump(res_walk_vs_); // (Phase 6: its walk's output kept, only the vertex tables new)
        }
    }
    CopyStage(out.ps, p.ps);
    if (ps_changed) {
        out.ps.plan.flat.assign(dc_ps_.flat.begin(), dc_ps_.flat.end());
        out.ps.plan.buffers.assign(dc_ps_.buffers.begin(), dc_ps_.buffers.end());
        out.ps.plan.images.assign(dc_ps_.images.begin(), dc_ps_.images.end());
        out.ps.plan.samplers.assign(dc_ps_.samplers.begin(), dc_ps_.samplers.end());
        out.ps.plan.status_counts = dc_ps_.status_counts;
        out.ps.plan.srt = dc_ps_.srt;
        dc_ps_hits_.store(dc_ps_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    return true;
}

void ParallelCp::PrepareDraw(Record& r, u64 index, const Cp::RegisterFile& regs) {
    R8_CP_PROF(CpProfile::Sec::PrepDraw);
    r.prepared = false;
    r.census_key = 0;
    r.vs_same = r.ps_same = false;
    r.material = {};
    r.stage_shared.reset();
    r.ps_shared.reset();
    r.state = nullptr;
    if (Vk::Executor::DrawFiltered(regs, r.draw)) {
        return; // no preparation: thread 2 sees the same registers and skips it as filtered
    }
    const u64 t0 = __rdtsc();
    geofast_why_ = 15; // (not asked)
    if (geofast_ == 1 && PrepareGeometryOnly(r, index, r)) {
        r.prepared = true;
        geofast_prev_ = index;
        geofast_hits_.store(geofast_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        Bump(fc_.ticks, __rdtsc() - t0);
        if (srt_diag_) {
            SrtDiag& sd = srt_by_[r.draw.engine_run & 3];
            Bump(sd.fast);
            Bump(sd.t_fast, __rdtsc() - t0);
        }
        if (census_end_ != 0) {
            CensusPrepared(r, __rdtsc() - t0, true);
        }
        return;
    }
    // SSPC_NATIVE_GBUFFER (native_gbuffer_prep.h): a 0x1a1770 draw with the engine's record - guarded / native prepare it
    // from the engine's state (no SRT walk); verify and guarded's samples prepare it in full below and compare
    namespace DR = Engine::Render::DrawRuns;
    // R10 (SSPC_R10_NATIVE_DRAWS): the mesh and shadow casters' draws the same way, from their ring (EngineStages)
    NativeGBuffer* nat = nullptr;
    std::optional<NativeGBuffer::EngineState> engine;
    bool (*still)(u32) = nullptr;
    CpProfile::Sec nat_sec = CpProfile::Sec::FrontNative;
    if (ngb_.On() && r.draw.engine_run == static_cast<u8>(DR::Source::GBuffer)) {
        nat = &ngb_;
        still = &DR::EngineDescStill;
        if (const DR::EngineDraw* e = r.draw.engine_native_present ? DR::EngineDescPeek(r.draw.engine_native) : nullptr) {
            engine.emplace(NativeGBuffer::EngineState{e->vs, e->ps});
        }
    } else if ((r.draw.engine_run == static_cast<u8>(DR::Source::Mesh) ||
                r.draw.engine_run == static_cast<u8>(DR::Source::Shadow)) &&
               ndr_[r.draw.engine_run - 1].On()) {
        nat = &ndr_[r.draw.engine_run - 1];
        still = &DR::StagesStill;
        nat_sec = r.draw.engine_run == static_cast<u8>(DR::Source::Mesh) ? CpProfile::Sec::FrontNativeMesh
                                                                          : CpProfile::Sec::FrontNativeShadow;
        if (const DR::EngineStages* e = r.draw.engine_native_present ? DR::StagesPeek(r.draw.engine_native) : nullptr) {
            engine.emplace(NativeGBuffer::EngineState{e->vs, e->ps});
        }
    }
    bool ngb_check = false;
    u64 ngb_targets = 0;
    if (nat != nullptr) {
        // (the colour/depth target registers, which the stages' runtime parameters and the pipeline read: by content)
        ngb_targets = (regs.GroupHash(Cp::Group::ColorTarget) * 0x9E3779B97F4A7C15ull) ^
                      regs.GroupHash(Cp::Group::DepthTarget);
        nat->NoteRecord(engine.has_value());
        ngb_check = engine.has_value() && (nat->mode() == NativeGBuffer::Mode::Verify || nat->SampleCheck());
        if (engine.has_value() && nat->Takes() && !ngb_check) {
            R8_CP_PROF_ENTER(nat_sec);
            const bool ok = nat->Prepare(r.draw, *engine, frontend_->MutablePrograms(), reader_, ngb_targets, r.prep,
                                         r.state, r.stage, r.ps, r.material) &&
                            still(r.draw.engine_native); // (the slot not reused while it was read)
            R8_CP_PROF_LEAVE();
            if (!ok || nat != &ngb_) {
                r.material = {}; // (R10's draws: the executor's G-buffer binding memo, GbBind, is not theirs)
            }
            if (ok) {
                r.prepared = true;
                // the frontend skipped this draw: its next preparation sees the register groups changed meanwhile
                ngb_pending_ |= r.draw.changed;
                copied_gen_vs_ = copied_gen_ps_ = kNoGen; // (FB-6: the frontend's views are not this draw's)
                geofast_prev_ = index;
                geofast_gen_ = frontend_->MemoryGeneration();
                Bump(fc_.ticks, __rdtsc() - t0);
                return;
            }
        }
    }
    const bool check = geofast_ == 2 && PrepareGeometryOnly(r, index, *geofast_check_);
    const u8 why = geofast_why_;
    u64 t_copy = 0;
    {
        const GS::FineSample fine(front_fine_counter_);
        if (r8_diag_ != nullptr) {
            r8_diag_->Before(*frontend_);
        }
        const u64 tf = r8_diag_ != nullptr ? __rdtsc() : 0;
        R8_CP_PROF_ENTER(CpProfile::Front(r.draw.engine_run));
        // (the groups changed at draws the native G-buffer path prepared without the frontend count as changed now)
        const Cp::GroupMask changed = r.draw.changed;
        r.draw.changed |= ngb_pending_;
        ngb_pending_ = 0;
        const GS::DrawPreparation& prep = frontend_->PrepareDraw(r.draw, regs, reader_);
        r.draw.changed = changed;
        R8_CP_PROF_LEAVE();
        if (r8_diag_ != nullptr) {
            r8_diag_->After(r.draw, *frontend_, reader_, __rdtsc() - tf); // (R8-A2: before the views are copied)
        }
        R8_CP_PROF_PREPARED(r.draw, frontend_->Stage(GS::HwStage::Vs), frontend_->Stage(GS::HwStage::Ps));
        const u64 tc = srt_diag_ ? __rdtsc() : 0;
        r.prep = prep;
        // P5.3: a preparation the native cache shares is kept by reference (immutable), not copied
        r.stage_shared = frontend_->SharedStage(GS::HwStage::Vs);
        r.ps_shared = frontend_->SharedStage(GS::HwStage::Ps);
        // R8-A2 K1a: the PS reuse's shared preparation is copied like an own view (run leaders, GEOFAST's template and
        // FB-6 take a record's own views only); the copy's registers are then the draw's own (CopyStage does not carry
        // native_shared: the executor would keep those of the draw that made it)
        const bool r8_ps = frontend_->R8PsShared();
        if (r8_ps) {
            r.ps_shared.reset();
        }
        // FB-6: a view the frontend did not change since the last copy is not copied again (stage_reuse_)
        const auto copy = [&](GS::HwStage hw, GS::StageView& dst, bool shared, u64& copied, bool& same) {
            same = false;
            if (shared) {
                copied = kNoGen; // (thread 2 does not see this one: the next own view is copied)
                return;
            }
            const u64 gen = frontend_->StageGeneration(hw);
            same = stage_reuse_ != 0 && gen == copied;
            if (!same || stage_reuse_ == 2) {
                CopyStage(dst, frontend_->Stage(hw));
                R8_CP_PROF_COPY(gen == copied);
            }
            copied = gen;
        };
        copy(GS::HwStage::Vs, r.stage, r.stage_shared != nullptr, copied_gen_vs_, r.vs_same);
        copy(GS::HwStage::Ps, r.ps, r.ps_shared != nullptr, copied_gen_ps_, r.ps_same);
        if (r8_ps) {
            if (r.ps_same) { // (cannot be: the frontend's generation moved; kept safe)
                CopyStage(r.ps, frontend_->Stage(GS::HwStage::Ps));
                r.ps_same = false;
            }
            const auto& ud = r.draw.user_data[static_cast<size_t>(Cp::Stage::Ps)];
            std::copy_n(ud.begin(), std::min<size_t>(ud.size(), r.ps.plan.flat.size()), r.ps.plan.flat.begin());
        }
        r.state = prep.ready ? frontend_->Pipelines().Graphics(prep.pipeline) : nullptr;
        t_copy = srt_diag_ ? __rdtsc() - tc : 0;
    }
    r.prepared = true;
    // SSPC_NATIVE_GBUFFER: the full preparation is its key's template; verify / guarded's samples compare the engine
    // build with it (the full one is what the draw uses)
    if (engine.has_value() && r.stage_shared == nullptr && r.ps_shared == nullptr && still(r.draw.engine_native)) {
        R8_CP_PROF(nat_sec);
        const GS::StageView& dv = r.vs_same ? frontend_->Stage(GS::HwStage::Vs) : r.Vs();
        const GS::StageView& dp = r.ps_same ? frontend_->Stage(GS::HwStage::Ps) : r.Ps();
        nat->AfterFull(r.draw, *engine, frontend_->MutablePrograms(), reader_, ngb_targets, r.prep, r.state, dv, dp,
                       ngb_check);
    }
    if (srt_diag_) {
        // (a view not copied is the frontend's current one)
        const GS::StageView& dv = r.vs_same ? frontend_->Stage(GS::HwStage::Vs) : r.Vs();
        const GS::StageView& dp = r.ps_same ? frontend_->Stage(GS::HwStage::Ps) : r.Ps();
        SrtDiag& sd = srt_by_[r.draw.engine_run & 3];
        Bump(sd.full);
        Bump(sd.t_full, __rdtsc() - t0);
        Bump(sd.t_copy, t_copy);
        Bump(sd.reads, dv.plan.srt.reads + dp.plan.srt.reads);
        Bump(sd.walk_vs, dv.plan.flat.size() > 16 ? 1 : 0);
        Bump(sd.walk_ps, dp.plan.flat.size() > 16 ? 1 : 0);
        Bump(sd.flat_vs, dv.plan.flat.size());
        Bump(sd.flat_ps, dp.plan.flat.size());
        Bump(sd.images, dv.plan.images.size() + dp.plan.images.size());
        Bump(sd.buffers, dv.plan.buffers.size() + dp.plan.buffers.size());
        Bump(sd.why[why & 15]);
    }
    if (R6::Opt().diag) { // (R6-P: how often a full walk repeats the program's last output - docs §G)
        const GS::StageView& dv = r.vs_same ? frontend_->Stage(GS::HwStage::Vs) : r.Vs();
        const GS::StageView& dp = r.ps_same ? frontend_->Stage(GS::HwStage::Ps) : r.Ps();
        R6::NoteWalk(0, dv.address, dv.plan.flat.data(), dv.plan.flat.data(), dv.plan.flat.size());
        R6::NoteWalk(1, dp.address, dp.plan.flat.data(), dp.plan.flat.data(), dp.plan.flat.size());
    }
    bool writes = false;
    // (a view not copied is the frontend's current one)
    NoteStageWrites(r.vs_same ? frontend_->Stage(GS::HwStage::Vs) : r.Vs(), index, writes);
    NoteStageWrites(r.ps_same ? frontend_->Stage(GS::HwStage::Ps) : r.Ps(), index, writes);
    if (writes) {
        frontend_->GuestMemoryChanged(); // (Executor::MarkWrites after the draw: no memoized preparation past it)
    }
    if (check) {
        // SSPC_CITY_GEOFAST=verify: the fast preparation against the full one, field by field (the bindings' padding
        // bytes differ between two builds of the same values)
        const auto same_view = [](const GS::StageView& x, const GS::StageView& y) {
            const auto bufs = [](const GS::BufferBinding& a, const GS::BufferBinding& b) {
                return a.binding == b.binding && a.kind == b.kind && a.desc.raw == b.desc.raw && a.address == b.address &&
                       a.size == b.size && a.written == b.written && a.formatted == b.formatted && a.status == b.status;
            };
            const auto imgs = [](const GS::ImageBinding& a, const GS::ImageBinding& b) {
                return a.binding == b.binding && a.count == b.count && a.desc.raw == b.desc.raw &&
                       a.view_type == b.view_type && a.storage == b.storage && a.depth == b.depth && a.array == b.array &&
                       a.atomic == b.atomic && a.fmask == b.fmask && a.status == b.status;
            };
            const auto smps = [](const GS::SamplerBinding& a, const GS::SamplerBinding& b) {
                return a.binding == b.binding && a.desc.raw == b.desc.raw && a.status == b.status;
            };
            const auto vtxs = [](const GS::VertexBinding& a, const GS::VertexBinding& b) {
                return a.attribute == b.attribute && a.fetch == b.fetch && a.read == b.read && a.desc.raw == b.desc.raw &&
                       a.address == b.address && a.size == b.size && a.status == b.status;
            };
            return x.status == y.status && x.address == y.address && x.program == y.program &&
                   x.footer_hash == y.footer_hash && x.module == y.module &&
                   std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) == 0 && x.start == y.start &&
                   x.end == y.end && x.program_error == y.program_error && x.plan.flat == y.plan.flat &&
                   std::ranges::equal(x.plan.buffers, y.plan.buffers, bufs) &&
                   std::ranges::equal(x.plan.images, y.plan.images, imgs) &&
                   std::ranges::equal(x.plan.samplers, y.plan.samplers, smps) &&
                   std::ranges::equal(x.plan.vertices, y.plan.vertices, vtxs) &&
                   x.plan.status_counts == y.plan.status_counts && x.plan.error == y.plan.error;
        };
        const bool same = r.prep.ready && r.prep.pipeline == geofast_check_->prep.pipeline &&
                          r.state == geofast_check_->state && same_view(r.Vs(), geofast_check_->stage) &&
                          same_view(r.Ps(), geofast_check_->ps);
        geofast_verified_.store(geofast_verified_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (!same) {
            const u64 n = geofast_mismatches_.load(std::memory_order_relaxed) + 1;
            geofast_mismatches_.store(n, std::memory_order_relaxed);
            if (n <= 12) {
                const auto what = [](const GS::StageView& x, const GS::StageView& y) {
                    std::string t;
                    t += x.status != y.status ? " status" : "";
                    t += x.address != y.address || x.program != y.program ? " program" : "";
                    t += x.footer_hash != y.footer_hash ? " footer" : "";
                    t += x.module != y.module ? " module" : "";
                    t += std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) != 0 ? " runtime" : "";
                    t += !(x.start == y.start) || !(x.end == y.end) ? " start/end" : "";
                    t += !SameBytes(x.plan.flat, y.plan.flat) ? " flat" : "";
                    if (!SameBytes(x.plan.buffers, y.plan.buffers)) {
                        t += fmt::format(" buffers({}/{}", x.plan.buffers.size(), y.plan.buffers.size());
                        for (size_t i = 0; i < std::min(x.plan.buffers.size(), y.plan.buffers.size()); ++i) {
                            const auto& a = x.plan.buffers[i];
                            const auto& b = y.plan.buffers[i];
                            t += fmt::format(" {}:k{}{}{}{}{}", i, static_cast<int>(a.kind),
                                             a.address != b.address ? fmt::format("a{:x}/{:x}", a.address, b.address) : "",
                                             a.size != b.size ? "s" : "", a.desc.raw != b.desc.raw ? "d" : "",
                                             a.status != b.status ? "t" : "");
                        }
                        t += ")";
                    }
                    t += !SameBytes(x.plan.images, y.plan.images) ? " images" : "";
                    t += !SameBytes(x.plan.samplers, y.plan.samplers) ? " samplers" : "";
                    t += x.plan.status_counts != y.plan.status_counts ? " counts" : "";
                    if (!SameBytes(x.plan.vertices, y.plan.vertices)) {
                        t += fmt::format(" vertices({}/{}", x.plan.vertices.size(), y.plan.vertices.size());
                        for (size_t i = 0; i < std::min(x.plan.vertices.size(), y.plan.vertices.size()); ++i) {
                            const auto& a = x.plan.vertices[i];
                            const auto& b = y.plan.vertices[i];
                            t += fmt::format(" {}:{}{}{}{}{}", i, a.address != b.address ? "a" : "", a.size != b.size ? "s" : "",
                                             a.desc.raw != b.desc.raw ? "d" : "", a.read != b.read ? "r" : "",
                                             a.status != b.status ? "t" : "");
                        }
                        t += ")";
                    }
                    t += x.plan.error != y.plan.error ? " error" : "";
                    return t;
                };
                SSPC_LOG(GPU, Warning, "SSPC_CITY_GEOFAST=verify: record {} (VS {:#x}) differs: pipeline {}/{} state {} | "
                                       "VS:{} | PS:{}", index,
                         r.draw.shaders[static_cast<size_t>(Cp::Stage::Vs)].address, r.prep.pipeline,
                         geofast_check_->prep.pipeline, r.state == geofast_check_->state, what(r.Vs(), geofast_check_->stage),
                         what(r.Ps(), geofast_check_->ps));
            }
        }
    }
    geofast_prev_ = index;
    geofast_gen_ = frontend_->MemoryGeneration();
    Bump(fc_.ticks, __rdtsc() - t0);
    if (census_end_ != 0) {
        CensusPrepared(r, __rdtsc() - t0, false);
    }
}

void ParallelCp::CensusPrepared(Record& r, u64 ticks, bool fast) {
    r.census_key = 0;
    const u64 flip = VideoOut::NextFlipId();
    const Cp::DrawDescriptor last = census_last_;
    census_last_ = r.draw;
    if (flip < census_first_ || flip >= census_end_) {
        return;
    }
    const size_t vs = static_cast<size_t>(Cp::Stage::Vs), ps = static_cast<size_t>(Cp::Stage::Ps);
    const u64 va = r.draw.shaders[vs].address, pa = r.draw.shaders[ps].address;
    const auto note = [](std::vector<std::pair<u64, u64>>& v, u64 mask) {
        for (auto& [m, n] : v) {
            if (m == mask) {
                ++n;
                return;
            }
        }
        if (v.size() < 8) {
            v.push_back({mask, 1});
        }
    };
    u32 vs_ud = 0, ps_ud = 0;
    for (u32 i = 0; i < 16; ++i) {
        vs_ud |= last.user_data[vs][i] != r.draw.user_data[vs][i] ? 1u << i : 0u;
        ps_ud |= last.user_data[ps][i] != r.draw.user_data[ps][i] ? 1u << i : 0u;
    }
    const std::scoped_lock lock(census_mu_);
    size_t i = 0;
    while (i < census_.size() && (census_[i].vs != va || census_[i].ps != pa)) {
        ++i;
    }
    if (i == census_.size()) {
        if (census_.size() >= 4096) {
            return;
        }
        census_.push_back({.vs = va, .ps = pa});
    }
    CensusRow& c = census_[i];
    r.census_key = i + 1;
    ++c.draws;
    c.t1 += ticks;
    c.why[fast ? 0 : geofast_why_ & 15] += 1;
    c.srt_reads += r.Vs().plan.srt.reads + r.Ps().plan.srt.reads;
    c.vs_flat += r.Vs().plan.flat.size();
    c.ps_flat += r.Ps().plan.flat.size();
    c.vs_buffers += r.Vs().plan.buffers.size();
    c.ps_buffers += r.Ps().plan.buffers.size();
    c.ps_images += r.Ps().plan.images.size();
    c.vertices += r.Vs().plan.vertices.size();
    note(c.groups, r.draw.changed);
    note(c.vs_ud, vs_ud);
    note(c.ps_ud, ps_ud);
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
        R8_CP_PROF(CpProfile::Sec::FrontDispatch);
        const GS::FineSample fine(front_fine_counter_);
        r.prep = frontend_->PrepareDispatch(r.dispatch, std::span<const u32, Cp::kComputeCount>(r.cs.values), reader_);
        const u64 tc = GS::FineTicks();
        CopyStage(r.stage, frontend_->Stage(GS::HwStage::Cs));
        if (tc != 0) {
            frontend_->NoteDispatchCopy(GS::TicksToNs(__rdtsc() - tc)); // (CC-1: the copy's share)
        }
    }
    r.prepared = true;
    if (r.prep.ready) {
        exec_.PrefetchCompute(r.stage); // (R9-N SSPC_PIPE_PREFETCH: a new pipeline built while thread 2 catches up)
    }
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
        if (!ask && (p_.MayBeGpuWritten(addr, bytes) || p_.exec_.KeptBack(addr, bytes))) { // (FB-7: kept back)
            Bump(p_.fc_.read_hits);
            p_.FrontWaitExecuted(k);
            ask = true;
        }
        if (ask) {
            p_.DropUnknownGpuWrites();
            Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0);
        }
    } else if (p_.Idle()) {
        R8_CP_PROF(CpProfile::Sec::FrontSync);
        p_.ApplyPending();
        Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0); // (exact: TimelineReader's check)
    } else if (p_.MayBeGpuWritten(addr, bytes) || p_.exec_.KeptBack(addr, bytes)) { // (FB-7: kept back)
        R8_CP_PROF(CpProfile::Sec::FrontSync);
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
    R8_CP_PROF(CpProfile::Sec::Journal);
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
    R8_CP_PROF(CpProfile::Sec::Journal);
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

bool ParallelCp::FrontTryJoin(Record& r, u64 index) {
    (void)index;
    R8_CP_PROF(CpProfile::Sec::JoinRun);
    if (r.kind != Kind::Draw || r.memory_changed || r.front_memo || r.run_barrier || !RunTagged(r.draw) ||
        !JoinRun(r.draw, freplica_)) {
        return false;
    }
    // its draw is recorded with the run's first one; thread 2 replays its registers (after the run's draws, as the
    // journal behind a held record is in thread 1's runs)
    r.kind = Kind::Journal;
    r.prepared = false;
    r.joined = true;
    Bump(f_run_joined_);
    return true;
}

void ParallelCp::FrontCloseRun(u64 upto) {
    if (!f_run_open_) {
        return;
    }
    f_run_open_ = false;
    f_hold_lead_.store(0, std::memory_order_relaxed);
    const Record& r = slots_[f_run_lead_ & slot_mask_];
    if (!r.members.empty()) {
        geofast_prev_ = ~0ull; // (the draw before the next one is the run's last, not this record's)
        run_count_.store(run_count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        run_members_.store(run_members_.load(std::memory_order_relaxed) + r.members.size(), std::memory_order_relaxed);
    }
    // published at once: the record being prepared may wait for thread 2 to get to it (FrontReader::Read)
    if (upto > fhead_.load(std::memory_order_relaxed)) {
        fhead_.store(upto, std::memory_order_seq_cst);
        if (t2_waiting_.load(std::memory_order_seq_cst) != 0) {
            fhead_.notify_one();
        }
    }
}

void ParallelCp::FrontStep(Record& r, u64 index) {
    // (front mode: while a run is held, its first draw is the record thread 2 stops before - a read of memory a GPU
    // write may still be pending for waits for thread 2 to get there, not to this record)
    f_current_ = f_run_open_ ? f_run_lead_ : index;
    ApplyFrontEvents(index); // what thread 1 told the frontend before this record
    if (r.front_memo) {
        frontend_->GuestMemoryChanged(); // (a change thread 1 met before this record: stream order)
    }
    FrontApplyJournal(r.journal_end); // the registers as they were at the record's packet
    if (f_run_open_) {
        if (FrontTryJoin(r, index)) {
            return;
        }
        FrontCloseRun(index);
        f_current_ = index;
    }
    switch (r.kind) {
    case Kind::Draw:
        PrepareDraw(r, index, freplica_);
        if (runs_ != 0 && RunTagged(r.draw) && (runs_ab_ == 0 || (VideoOut::NextFlipId() / runs_ab_) % 2 == 1) &&
            RunLeaderOk(r)) {
            // front mode: held - the tagged draws after it may join (fhead_ stays at it until the run is complete)
            f_run_open_ = true;
            f_run_lead_ = index;
            f_hold_lead_.store(index + 1, std::memory_order_relaxed);
            run_gen_ = frontend_->MemoryGeneration();
            run_leaders_.store(run_leaders_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        break;
    case Kind::Dispatch:
        PrepareDispatch(r, index);
        break;
    case Kind::Copy:
    case Kind::Write:
    case Kind::Zpass:
        AddGpuWrite(r.dst, r.dst + r.bytes, index, kLookup); // (until thread 2 recorded it: the frontend's reads wait)
        break;
    case Kind::Gds:
        if (r.gds_kind == Cp::DmaKind::GdsToMemory) {
            AddGpuWrite(r.src, r.src + r.bytes, index, kLookup); // (SSPC_CP_ASYNC_WRITES: as a queued copy)
        }
        break;
    case Kind::Flip:
        frontend_->NewFrame(); // (SSPC_CP_ASYNC_FLIP: the frontend's frame begins at the marker, in stream order)
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
    R10::SpreadThisThread(R10::Heavy::Frontend); // (SSPC_R10_SPREAD)
    StallTrace::RegisterThread(StallTrace::Role::Frontend); // (R10 timeline)
    R8_CP_PROF_THREAD(Front);
    const bool trace_prep = StallTrace::DeepRecords();
    u64 k = fhead_.load(std::memory_order_relaxed);
    for (;;) {
        u64 t = tail_.load(std::memory_order_acquire);
        R8_CP_PROF_FRAME_POLL(); // (R8 profiler: the processor's frame moved on - the frontend's row so far is that frame's)
        if (t == k && f_run_open_) {
            R8_CP_PROF(CpProfile::Sec::T2Idle); // (a held run waiting for its next member)
            // front mode: a held run never waits blocked - published when thread 1 waits for thread 2 (a drain,
            // backpressure: no record comes before thread 2 got further) or after f_hold_ticks_ without a record
            const u64 w0 = __rdtsc();
            while (t == k && f_flush_.load(std::memory_order_acquire) == 0 && __rdtsc() - w0 < f_hold_ticks_) {
                _mm_pause();
                t = tail_.load(std::memory_order_acquire);
            }
            if (t == k) {
                Bump(f_flush_.load(std::memory_order_acquire) != 0 ? f_run_closed_flush_ : f_run_closed_idle_);
                FrontCloseRun(k);
            }
            Bump(fc_.idle, __rdtsc() - w0);
        }
        if (t == k) {
            R8_CP_PROF(CpProfile::Sec::T2Idle); // (the frontend thread waiting for records)
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
            const u64 w1 = __rdtsc();
            Bump(fc_.idle, w1 - w0);
            if (w1 - w0 >= 20'000) { // (R10 timeline: waits of ~5 us and more)
                StallTrace::Span(StallTrace::Ev::FrontWait, w0, w1, 0, static_cast<u32>(k));
            }
        }
        const u64 b0 = __rdtsc();
        Record& r = slots_[k & slot_mask_];
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
        if (stop || failed_.load(std::memory_order_relaxed)) {
            FrontCloseRun(k); // (nothing joins any more: the held first draw is published, this record below)
        }
        Bump(fc_.busy, __rdtsc() - b0);
        if (trace_prep && !stop) {
            StallTrace::Span(StallTrace::Ev::FrontPrep, b0, __rdtsc(), 0, static_cast<u32>(k), static_cast<u16>(r.kind));
        }
        ++k;
        // (the record's preparation published to thread 2 - front mode: up to a held run's first draw)
        const u64 publish = f_run_open_ ? f_run_lead_ : k;
        if (publish != fhead_.load(std::memory_order_relaxed)) {
            fhead_.store(publish, std::memory_order_seq_cst);
            if (t2_waiting_.load(std::memory_order_seq_cst) != 0) {
                fhead_.notify_one();
            }
        }
        if (stop) {
            break;
        }
    }
}

// ---- thread 2

void ParallelCp::Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuRecord");
    R10::SpreadThisThread(R10::Heavy::Record); // (SSPC_R10_SPREAD)
    StallTrace::RegisterThread(StallTrace::Role::Record);
    R8_CP_PROF_THREAD(Record);
    Vk::Emit::SetProducerThread();
    Vk::Emit::EnableBatching(); // (SSPC_VK_EMIT_BATCH=1: its commands handed to the emitter once a record)
    const bool trace_records = StallTrace::DeepRecords();
    u64 h = head_.load(std::memory_order_relaxed);
    const auto add = [](std::atomic<u64>& a, u64 v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); };
    // (with the frontend thread, the records it prepared: fhead_)
    std::atomic<u64>& source = fthread_mode_ ? fhead_ : tail_;
    // FB-7 (SSPC_FB_LAZY_WB): another thread's request for kept-back blocks is served after a record, before it is
    // published as executed (thread 1 uses the executor only once every record is: never both at once); with the
    // queue empty, thread 1 serves them (Backend::Loop)
    const bool lazy = exec_.Lazy();
    if (lazy) {
        exec_.SetRecordThread();
    }
    for (;;) {
        u64 t = source.load(std::memory_order_acquire);
        if (t == h) {
            // (R8: a wait at the first draw of a run the frontend thread holds - it is still joining the members - filed
            // apart; read at the wait's start)
            const bool held = fthread_mode_ && f_hold_lead_.load(std::memory_order_relaxed) == h + 1;
            R8_CP_PROF(held ? CpProfile::Sec::T2IdleHeld : CpProfile::Sec::T2Idle);
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
            if (held) {
                add(t2_idle_held_, w1 - w0);
                add(t2_idle_held_n_, 1);
            }
            // P4.12: waited for record h - thread 1 pushed it at push_tsc_[h] (deep), the wake-up is the rest
            StallTrace::Span(StallTrace::Ev::RecordWait, w0, w1, push_tsc_ != nullptr ? push_tsc_[h & (kRecRing - 1)] : 0,
                             static_cast<u32>(h));
        }
        R8_CP_PROF_FRAME_POLL();
        const u64 b0 = __rdtsc();
        Record& r = slots_[h & slot_mask_];
        const bool stop = r.kind == Kind::Stop;
        const u16 kind = static_cast<u16>(r.kind); // (before Execute: a record is not touched by thread 1 until head_ moves)
        const u64 queue = r.kind == Kind::Dispatch ? r.dispatch.queue : 0; // (P5.4: the guest queue in the trace)
        u64 extra = 0;
        if (!stop) {
            extra = Execute(r, h, t);
            if (lazy) {
                exec_.ServeKeptBack();
            }
            Vk::Emit::Publish(); // (before the record is published as executed: thread 1 may use the executor then)
            if (rec_value_ != nullptr) {
                for (u64 k = 0; k <= extra; ++k) {
                    rec_value_[(h + k) & (kRecRing - 1)].store(exec_.RecordedValue(), std::memory_order_relaxed);
                }
            }
        }
        const u64 b1 = __rdtsc();
        add(t2_busy_, b1 - b0);
        if (census_end_ != 0 && !stop && r.kind == Kind::Draw && r.census_key != 0) {
            const std::scoped_lock lock(census_mu_);
            census_[r.census_key - 1].t2 += b1 - b0;
            census_[r.census_key - 1].t2_draws += 1 + extra;
            census_[r.census_key - 1].batch_end[batch_end_ & 15] += 1;
        }
        if (trace_records) {
            StallTrace::Span(StallTrace::Ev::Record, b0, b1, queue, static_cast<u32>(h), kind);
        }
        add(t2_records_, 1 + extra);
        h += 1 + extra;
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

void ParallelCp::RunWrite(const Record& r) {
    if (exec_.WriteOnTimeline(r.queue, r.seq, r.dst, r.payload.data(), r.bytes)) {
        return;
    }
    // refused when it ran (an import): what the processor does after a refusal, here in stream order
    exec_.Synchronize(r.queue);
    if (write_fallback_ == nullptr || !write_fallback_(write_ctx_, r.dst, r.payload.data(), r.bytes)) {
        SSPC_LOG(GPU, Error, "parallel command processor: a queued timeline write of {} bytes at {:#x} could be neither "
                             "recorded nor written by the CPU (its work completes)", r.bytes, r.dst);
    } else {
        exec_.GuestMemoryChanged();
    }
    write_fallbacks_.store(write_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

void ParallelCp::RunGds(const Record& r) {
    if (exec_.GdsOnTimeline(r.queue, r.seq, r.gds_kind, r.src, r.gds_offset, static_cast<u32>(r.bytes), r.pattern)) {
        return;
    }
    exec_.Synchronize(r.queue);
    if (gds_fallback_ == nullptr ||
        !gds_fallback_(write_ctx_, r.gds_kind, r.src, r.gds_offset, static_cast<u32>(r.bytes), r.pattern)) {
        SSPC_LOG(GPU, Error, "parallel command processor: a queued GDS DMA ({} bytes, GDS +{:#x}, memory {:#x}) could be "
                             "neither recorded nor done by the CPU (its work completes)", r.bytes, r.gds_offset, r.src);
    } else {
        exec_.GuestMemoryChanged();
    }
    write_fallbacks_.store(write_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

void ParallelCp::RunZpass(const Record& r) {
    const u32 db_mask = r.gds_offset, stride = r.pattern;
    if (exec_.ZpassOnTimeline(r.queue, r.seq, r.dst, db_mask, stride)) {
        return;
    }
    // refused when it ran (an import): what the processor does after a refusal - every DB record gets a count that
    // only grows (the query reads as visible), here in stream order
    exec_.Synchronize(r.queue);
    zpass_fallback_count_ += 0x10000;
    const u64 value = zpass_fallback_count_ | (1ull << 63);
    bool ok = write_fallback_ != nullptr;
    for (u32 db = 0; db < 16 && ok; ++db) {
        if ((db_mask >> db & 1) != 0) {
            ok = write_fallback_(write_ctx_, r.dst + u64{db} * stride, &value, sizeof(value));
        }
    }
    if (!ok) {
        SSPC_LOG(GPU, Error, "parallel command processor: a queued occlusion query's counts at {:#x} could be neither "
                             "recorded nor written by the CPU (its work completes)", r.dst);
    } else {
        exec_.GuestMemoryChanged();
    }
    write_fallbacks_.store(write_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    exec_.CompleteRefusedWork(r.queue, r.seq);
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

u64 ParallelCp::Execute(Record& r, u64 index, u64 avail) {
    u64 extra = 0;
    batch_end_ = 0;
    if (failed_.load(std::memory_order_relaxed)) {
        return 0; // consumed, not executed: thread 1's drains still complete; the backend stops at its next check
    }
    try {
        // (a draw's sections by the engine function that wrote it: BindStage and DrawRunMembers read the source)
        CpProfile::t_source = r.kind == Kind::Draw ? static_cast<u8>(r.draw.engine_run & 3) : u8{0};
        R8_CP_PROF(r.kind == Kind::Draw ? CpProfile::BySource(CpProfile::Sec::T2Draw, CpProfile::Sec::T2DrawMesh,
                                                              CpProfile::t_source)
                                        : CpProfile::RecordSection(static_cast<u32>(r.kind)));
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
                    pc = {&r.prep, &ReuseView(r.stage, r.stage_shared != nullptr, r.vs_same, last_vs_),
                          &ReuseView(r.ps, r.ps_shared != nullptr, r.ps_same, last_ps_), r.state, r.material};
                    if (r.stage_shared != nullptr) {
                        pc.stage = &r.Vs();
                    }
                    if (r.ps_shared != nullptr) {
                        pc.ps = &r.Ps();
                    }
                }
                if (r.prepared && batch_on_ && stage_reuse_ == 0 && r.members.empty() &&
                    (batch_ab_ == 0 || (VideoOut::NextFlipId() / batch_ab_) % 2 == 1) &&
                    !has_deferred_.load(std::memory_order_acquire)) {
                    // the published draws right after it that differ only in their geometry (each from the one
                    // before it: its changed register groups are since that one)
                    constexpr size_t kMaxBatch = 256;
                    batch_items_.clear();
                    batch_items_.push_back({&r.draw, pc});
                    batch_end_ = 0;
                    while (batch_items_.size() < kMaxBatch && index + batch_items_.size() < avail) {
                        Record& f = slots_[(index + batch_items_.size()) & slot_mask_];
                        if (f.kind != Kind::Draw || !f.prepared || f.memory_changed || !f.members.empty()) {
                            break;
                        }
                        const Vk::PreparedCommand fp{&f.prep, &f.Vs(), &f.Ps(), f.state};
                        if (!exec_.Batchable(*batch_items_.back().draw, batch_items_.back().prepared, f.draw, fp)) {
                            batch_end_ = exec_.LastBatchReject();
                            break;
                        }
                        batch_items_.push_back({&f.draw, fp});
                    }
                    extra = batch_items_.size() - 1;
                }
                if (extra != 0) {
                    const bool one = exec_.DrawPreparedBatch(batch_items_);
                    for (u64 k = 1; k <= extra; ++k) {
                        // each draw's registers (only theirs changed: VS user data, index, draw parameters); not
                        // recorded as one: each on its own after its registers, as Execute does it
                        ApplyJournal(slots_[(index + k) & slot_mask_].journal_end);
                        if (!one) {
                            exec_.DrawPrepared(*batch_items_[k].draw, batch_items_[k].prepared);
                        }
                    }
                } else {
                    exec_.DrawPrepared(r.draw, pc);
                    if (!r.members.empty()) {
                        // SSPC_CITY_ENGINE_OPT: the run's other draws on its state (their registers: the journal
                        // after this record's, applied with the next record)
                        exec_.DrawRunMembers(r.draw, pc, r.members, r.member_flat);
                        if (fthread_mode_) {
                            // front mode: the members' own records (published with this one, right after it) - their
                            // registers now, one step for thread 2 instead of a record each
                            while (index + 1 + extra < avail && slots_[(index + 1 + extra) & slot_mask_].joined) {
                                ++extra;
                            }
                            if (extra != 0) {
                                ApplyJournal(slots_[(index + extra) & slot_mask_].journal_end);
                            }
                        }
                        if (r.members.capacity() > 32) {
                            if (R6::Opt().run_pool) {
                                R6::GiveMembers(r.members); // (R6-P: to thread 1's next long run)
                            } else {
                                std::vector<Cp::RunMember>().swap(r.members); // (a long run's memory not kept per slot)
                            }
                        }
                    }
                }
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
            if (flush_at_fence_ && exec_.WantsFlush()) {
                exec_.FlushFromLoop(); // (CP-9: a held fence's work to the GPU now, its completion polled)
                exec_.Poll();
            }
            break;
        case Kind::Dma:
            exec_.Dma(r.dma);
            break;
        case Kind::WaitMet:
            exec_.WaitMetByHeldFence(r.queue, r.seq);
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
        case Kind::Flip:
            flip_totals_ = &r.flip_totals; // (the flip hook's frame log row: thread 1's totals at the marker)
            exec_.FlipQueued(r.flip);
            flip_totals_ = nullptr;
            break;
        case Kind::Write:
            RunWrite(r);
            break;
        case Kind::Gds:
            RunGds(r);
            break;
        case Kind::Zpass:
            RunZpass(r);
            break;
        case Kind::RoundEnd:
            // CP-9: what the loop does after a drained round end - a held fence's batch submitted, completed batches
            // collected (their completions kept for thread 1)
            if (exec_.WantsFlush()) {
                exec_.FlushFromLoop();
            }
            exec_.Poll();
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
    return extra;
}

void ParallelCp::Fail(std::string text) {
    if (!failed_.load(std::memory_order_relaxed)) {
        failure_ = std::move(text);
        failed_.store(true, std::memory_order_release);
    }
}

// ---- Cp::GpuWork (thread 1)

void ParallelCp::Draw(const Cp::DrawDescriptor& draw) {
    const bool tagged = RunTagged(draw);
    if (run_open_) {
        R8_CP_PROF(CpProfile::Sec::JoinRun);
        const u64 tj = srt_diag_ ? __rdtsc() : 0;
        const bool joined = tagged && JoinRun(draw, cp_->Registers());
        if (srt_diag_ && tagged) {
            // (Phase 6 diagnostics: a member's cost, and the cost of an attempt that did not join)
            SrtDiag& sd = srt_by_[slots_[next_ & slot_mask_].draw.engine_run & 3];
            Bump(joined ? sd.t_join : sd.t_refused, __rdtsc() - tj);
            Bump(joined ? sd.joins : sd.refusals);
        }
        if (joined) {
            ++t1_.draws;
            return;
        }
        if (R6::Opt().diag) { // (R6-P diagnostics: an untagged draw / a tagged one refused)
            R6::Bump(R6::Count().run_closed_by[tagged ? R6::kKindTaggedRefused : 0]);
        }
        CloseRun();
    }
    R8_CP_PROF(CpProfile::Sec::ParDraw);
    Record& r = BeginRecord(Kind::Draw);
    r.draw = draw;
    r.members.clear();
    r.member_flat.clear();
    if (verify_) {
        r.reg_hash = RegistersHash(cp_->Registers());
    }
    if (frontend_ != nullptr && !fthread_mode_) {
        PrepareDraw(r, next_, cp_->Registers()); // (as they are at the draw's packet)
    }
    ++t1_.draws;
    if (runs_ != 0 && !fthread_mode_ && tagged && (runs_ab_ == 0 || (VideoOut::NextFlipId() / runs_ab_) % 2 == 1) &&
        RunLeaderOk(r)) {
        // SSPC_CITY_ENGINE_OPT: held - the tagged draws after it may join; its registers are the journal so far
        r.journal_end = jtail_;
        run_open_ = true;
        run_gen_ = frontend_->MemoryGeneration();
        run_leaders_.store(run_leaders_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    PushRecord();
}

void ParallelCp::CloseRun() {
    if (!run_open_) {
        return;
    }
    run_open_ = false;
    Record& r = slots_[next_ & slot_mask_];
    if (!r.members.empty()) {
        geofast_prev_ = ~0ull; // (the draw before the next one is the run's last, not this record's)
        run_count_.store(run_count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        run_members_.store(run_members_.load(std::memory_order_relaxed) + r.members.size(), std::memory_order_relaxed);
    }
    PushRecord(true);
}

bool ParallelCp::RunTagged(const Cp::DrawDescriptor& d) const {
    // (Phase 6: 0x1a1770's tag - SSPC_DIAG_ENGINE_SRT - leads or joins a run only with SSPC_CITY_ENGINE_GBUFFER_RUNS)
    return d.engine_run != 0 &&
           (d.engine_run != static_cast<u8>(Engine::Render::DrawRuns::Source::GBuffer) || GRunsNow());
}

bool ParallelCp::GRunsNow() const {
    return gruns_ != 0 && (gruns_ab_ == 0 || (VideoOut::NextFlipId() / gruns_ab_) % 2 == 1);
}

bool ParallelCp::RunLeaderOk(const Record& r) const {
    const Cp::DrawDescriptor& d = r.draw;
    constexpr u8 kVsPs = (1u << static_cast<u32>(Cp::Stage::Vs)) | (1u << static_cast<u32>(Cp::Stage::Ps));
    if (r.kind != Kind::Draw || !r.prepared || !r.prep.ready || r.state == nullptr || r.stage_shared != nullptr ||
        r.ps_shared != nullptr || r.vs_same || r.ps_same || !d.indexed || d.indirect || d.active_stages != kVsPs) {
        return false;
    }
    const GS::StageView& v = r.stage;
    // (Phase 6, a G-buffer run: a VS with an SRT walk whose output its members take again - JoinRun checks the
    // registers it read)
    const bool walk_ok = GRunsNow() && d.engine_run == static_cast<u8>(Engine::Render::DrawRuns::Source::GBuffer) &&
                         v.plan.flat.size() > 16 && !v.plan.srt.user_data_written && v.plan.srt.faults == 0;
    if (v.status != GS::StageStatus::Ready || v.module == nullptr || r.ps.module == nullptr ||
        r.ps.status != GS::StageStatus::Ready || (v.plan.flat.size() != 16 && !walk_ok) || v.plan.vertices.empty() ||
        v.plan.vertices.size() > Cp::RunMember::kMaxStreams) {
        return false;
    }
    const GS::FetchShader& fetch = v.module->result.reflection.fetch;
    if (!fetch.present || fetch.sgpr_base + 1 >= 16) {
        return false;
    }
    for (const GS::VertexBinding& vb : v.plan.vertices) {
        if (vb.status != GS::ResourceStatus::Ok || vb.fetch.sgpr_base + 1 >= 16 || vb.fetch.instance_data != 0 ||
            vb.desc.Stride() == 0) {
            return false;
        }
    }
    // nothing it prepared writes memory (each draw of the run would write it again)
    for (const GS::StageView* s : {&v, &r.ps}) {
        for (const GS::BufferBinding& bb : s->plan.buffers) {
            if (bb.written) {
                return false;
            }
        }
        for (const GS::ImageBinding& im : s->plan.images) {
            if (im.storage) {
                return false;
            }
        }
    }
    return true;
}

bool ParallelCp::JoinRun(const Cp::DrawDescriptor& b, const Cp::RegisterFile& regs) {
    Record& lead = RunLead();
    const size_t flat_mark = lead.member_flat.size();
    const auto no = [&](u32 why) {
        if (RunHeld()) {
            lead.member_flat.resize(flat_mark); // (a refused member's flattened data dropped - the run still held)
        }
        auto& c = run_refused_[why];
        c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return false;
    };
    if (lead.members.size() + 1 >= kMaxRun) {
        return no(1);
    }
    if (frontend_->MemoryGeneration() != run_gen_ || (!fthread_mode_ && pending_memory_changed_)) {
        return no(2);
    }
    const Cp::DrawDescriptor& a = lead.draw;
    using G = Cp::Group;
    constexpr Cp::GroupMask kGeometryGroups = (Cp::GroupMask{1} << static_cast<u32>(G::VsUserData)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::IndexState)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::DrawParams));
    // Phase 6: a G-buffer run - its members may differ in PS user data too (each one's PS plan decoded again below)
    const bool g = GRunsNow() && a.engine_run == static_cast<u8>(Engine::Render::DrawRuns::Source::GBuffer);
    // R10 (SSPC_R10_RUN_PS): a mesh / shadow run's members may differ in PS user data too - their PS plan decoded again
    // below, as a G-buffer member's (not the stencil reference: that ends the run as before)
    const bool psm = g || (run_ps_ != 0 &&
                           (a.engine_run == static_cast<u8>(Engine::Render::DrawRuns::Source::Mesh) ||
                            a.engine_run == static_cast<u8>(Engine::Render::DrawRuns::Source::Shadow)));
    const u8 mat = g ? gmat_ : run_ps_mat_; // (members with T#s / S#s of their own)
    if (g && b.engine_material) {
        // (the engine set another material for it: its PS reads other descriptors - not walked again to see that;
        // SSPC_CITY_GB_RUN_MATERIAL: walked again below, its own images and samplers)
        Bump(run_material_);
        if (gruns_ != 2 && gmat_ == 0) {
            return no(16);
        }
    }
    // the registers since the draw before (the run's last): only its geometry's groups
    // (a G-buffer run: the PS user data, and the stencil reference the engine sets per mesh - Vulkan dynamic state)
    constexpr Cp::GroupMask kGBufferGroups =
        (Cp::GroupMask{1} << static_cast<u32>(G::PsUserData)) | (Cp::GroupMask{1} << static_cast<u32>(G::StencilRef));
    constexpr Cp::GroupMask kPsGroups = Cp::GroupMask{1} << static_cast<u32>(G::PsUserData);
    if (const Cp::GroupMask other = b.changed & ~(kGeometryGroups | (g ? kGBufferGroups : psm ? kPsGroups : 0));
        other != 0) {
        if (srt_diag_) {
            SrtDiag& sd = srt_by_[a.engine_run & 3];
            for (u32 k = 0; k < sd.groups.size(); ++k) {
                Bump(sd.groups[k], (other >> k) & 1);
            }
        }
        // (which groups ended the run, by its engine source - the report)
        for (Cp::GroupMask o = other; o != 0; o &= o - 1) {
            Bump(gb_groups_[a.engine_run & 3][std::countr_zero(o) & 63]);
        }
        return no(3);
    }
    if (b.queue != a.queue || !b.indexed || b.indirect || b.active_stages != a.active_stages ||
        b.primitive_type != a.primitive_type || b.index_type != a.index_type || b.stages_enable != a.stages_enable ||
        b.cb_extent != a.cb_extent || b.db_extent != a.db_extent) {
        return no(4);
    }
    for (size_t s = 0; s < a.shaders.size(); ++s) {
        if (a.shaders[s].address != b.shaders[s].address || a.shaders[s].rsrc1 != b.shaders[s].rsrc1 ||
            a.shaders[s].rsrc2 != b.shaders[s].rsrc2) {
            return no(5);
        }
    }
    const size_t vs = static_cast<size_t>(Cp::Stage::Vs), ps = static_cast<size_t>(Cp::Stage::Ps);
    for (size_t s = 0; s < a.user_data.size(); ++s) {
        if (s != vs && (s != ps || !psm) && ((a.active_stages >> s) & 1) != 0 && a.user_data[s] != b.user_data[s]) {
            return no(6);
        }
    }
    // the VS user data: only the vertex tables' pairs (and the fetch shader's vertex/instance offsets) differ, and the
    // stage itself reads none of them (the run's draws are recorded with the first one's flattened user data)
    const GS::StageView& pv = lead.stage;
    const GS::ShaderReflection& refl = pv.module->result.reflection;
    const GS::FetchShader& fetch = refl.fetch;
    u32 tables = 0, offsets = 0;
    for (const GS::FetchAttribute& at : fetch.attributes) {
        tables |= at.sgpr_base + 1 < 16 ? 3u << at.sgpr_base : 0u;
    }
    tables &= ~(3u << fetch.sgpr_base); // (the fetch shader's own address must be the same)
    if (fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
        offsets |= 1u << fetch.vertex_offset_sgpr;
    }
    if (fetch.instance_offset_sgpr >= 0 && fetch.instance_offset_sgpr < 16) {
        offsets |= 1u << fetch.instance_offset_sgpr;
    }
    u32 diff = 0;
    for (u32 i = 0; i < 16; ++i) {
        diff |= a.user_data[vs][i] != b.user_data[vs][i] ? 1u << i : 0u;
    }
    // other VS user data that differ (the shadow casters' instance constants' V#s): the draw's plan decoded again below -
    // only its own constant buffers may differ; its user data reach the stage as its own copy
    // (Phase 6: a VS walk that read a register that differs - its plan from a walk for these registers, below)
    const bool rewalk = pv.plan.flat.size() != 16 && (diff & pv.plan.srt.user_data_read) != 0;
    if (rewalk && !g) {
        return no(15);
    }
    const bool own = (diff & ~(tables | offsets)) != 0 || (diff & refl.user_data_used & ~tables) != 0 || rewalk;
    // its vertex streams from its own tables (shader_resources.cpp DecodeResources: the same reads and results)
    Cp::RunMember m;
    m.streams = static_cast<u32>(pv.plan.vertices.size());
    for (u32 i = 0; i < m.streams; ++i) {
        const GS::VertexBinding& v = pv.plan.vertices[i];
        const GS::FetchAttribute& at = v.fetch;
        const u64 table = (b.user_data[vs][at.sgpr_base] | (u64{b.user_data[vs][at.sgpr_base + 1]} << 32)) &
                          0xFFFFFFFFFFFFull;
        const u64 where = table + u64{at.dword_offset} * 4;
        GS::BufferDesc d;
        if (where >= (1ull << 40) || !reader_.Read(where, d.raw.data(), 16)) {
            return no(8);
        }
        m.v[i].read = d.raw;
        if (at.data_format != 0) {
            d.raw[3] = (d.raw[3] & ~((0xFu << 15) | (7u << 12))) | (at.data_format << 15) | (at.num_format << 12);
        }
        const u64 addr = d.Base() + at.inst_offset, size = d.Size();
        // (the same format, stride and specialization of the module: the first draw's vertex input state)
        if (!d.IsBuffer() || d.NumRecords() == 0 || addr == 0 || addr >= (1ull << 40) || size == 0 ||
            size > (1ull << 40) - addr || d.raw[3] != v.desc.raw[3] || (d.raw[1] >> 16) != (v.desc.raw[1] >> 16)) {
            return no(9);
        }
        m.v[i].desc = d.raw;
        m.v[i].address = addr;
        m.v[i].size = size;
    }
    if (!RunHeld() || frontend_->MemoryGeneration() != run_gen_) {
        return no(10); // (a read drained the queue: the run was published meanwhile)
    }
    if (own) {
        // a stage without an SRT walk: its plan is its registers' decoding (the same module, the same binding numbers)
        std::array<std::array<u32, 4>, Cp::RunMember::kMaxStreams> reads{};
        for (u32 i = 0; i < m.streams; ++i) {
            reads[i] = m.v[i].read;
        }
        if (!RedecodeStage(pv, b.user_data[vs], std::span(reads.data(), m.streams), run_plan_)) {
            return no(12);
        }
        if (!RunHeld() || frontend_->MemoryGeneration() != run_gen_) {
            return no(10); // (a walk's read drained the queue)
        }
        const GS::ResourceBindingPlan& x = pv.plan;
        const GS::ResourceBindingPlan& y = run_plan_;
        bool same = x.images.size() == y.images.size() && x.samplers.size() == y.samplers.size() &&
                    x.buffers.size() == y.buffers.size() && x.vertices.size() == y.vertices.size();
        for (size_t i = 0; same && i < x.images.size(); ++i) {
            const GS::ImageBinding& p = x.images[i];
            const GS::ImageBinding& q = y.images[i];
            same = p.binding == q.binding && p.count == q.count && p.desc.raw == q.desc.raw && p.view_type == q.view_type &&
                   p.storage == q.storage && p.depth == q.depth && p.array == q.array && p.fmask == q.fmask &&
                   p.status == q.status;
        }
        for (size_t i = 0; same && i < x.samplers.size(); ++i) {
            same = x.samplers[i].binding == y.samplers[i].binding && x.samplers[i].desc.raw == y.samplers[i].desc.raw &&
                   x.samplers[i].status == y.samplers[i].status;
        }
        for (u32 i = 0; same && i < m.streams; ++i) {
            same = y.vertices[i].address == m.v[i].address && y.vertices[i].size == m.v[i].size &&
                   y.vertices[i].desc.raw == m.v[i].desc && y.vertices[i].status == GS::ResourceStatus::Ok;
        }
        if (!same) {
            return no(13);
        }
        for (size_t i = 0; i < x.buffers.size(); ++i) {
            const GS::BufferBinding& p = x.buffers[i];
            const GS::BufferBinding& q = y.buffers[i];
            if (p.binding == q.binding && p.kind == q.kind && p.desc.raw == q.desc.raw && p.address == q.address &&
                p.size == q.size && p.written == q.written && p.formatted == q.formatted && p.status == q.status) {
                continue;
            }
            // (a constant block elsewhere, of another size: the same format and stride, read only, at most 16 KiB)
            if (p.binding != q.binding || p.kind != GS::BufferKind::Guest || q.kind != GS::BufferKind::Guest ||
                p.written || q.written || p.formatted != q.formatted || p.status != GS::ResourceStatus::Ok ||
                q.status != GS::ResourceStatus::Ok || (p.desc.raw[1] >> 16) != (q.desc.raw[1] >> 16) ||
                p.desc.raw[3] != q.desc.raw[3] || q.address == 0 || q.size == 0 || q.size > (16u << 10) ||
                m.buffers >= Cp::RunMember::kMaxBuffers) {
                return no(14);
            }
            m.b[m.buffers++] = {static_cast<u32>(i), q.address, q.size, q.desc.raw};
        }
        m.own_user_data = true;
        if (x.flat.size() != y.flat.size()) {
            return no(13);
        }
        if (!std::equal(x.flat.begin() + 16, x.flat.end(), y.flat.begin() + 16)) {
            // (Phase 6: its walk's output differs - its own V#s: its whole flattened user data in the run's store)
            m.vs_flat_at = static_cast<u32>(lead.member_flat.size());
            lead.member_flat.insert(lead.member_flat.end(), y.flat.begin(), y.flat.end());
        }
    }
    u32 own_img = 0, own_smp = 0; // (SSPC_CITY_GB_RUN_MATERIAL: its own T#s / S#s, without those it has again)
    bool ps_redecoded = false;
    if (psm && a.user_data[ps] != b.user_data[ps]) {
        // Phase 6: its PS plan from its registers (RedecodeStage: the walk's output again, the same module and binding
        // numbers) - the same images and samplers; only read-only constant blocks of the same format elsewhere
        const GS::StageView& pp = lead.ps;
        // SSPC_CITY_GB_RUN_MATERIAL: the plan from its engine record's CUE state - the walk's plan without walking the
        // material's tables again (NativeGBuffer::MemberPlan; the walk's cost, R8: most of the member's on the
        // frontend); no map: the walk
        u8 planned = NativeGBuffer::kPlanNone;
        if (gmat_ != 0 && ngb_.On() && b.engine_native_present &&
            b.engine_run == static_cast<u8>(Engine::Render::DrawRuns::Source::GBuffer)) { // (R10's tags: their own ring)
            namespace DR = Engine::Render::DrawRuns;
            if (const DR::EngineDraw* e = DR::EngineDescPeek(b.engine_native); e != nullptr) {
                planned = ngb_.MemberPlan(pp, e->ps, b.user_data[ps], run_ps_plan_);
                if (planned != NativeGBuffer::kPlanNone && !DR::EngineDescStill(b.engine_native)) {
                    planned = NativeGBuffer::kPlanNone; // (the slot reused while it was read: walked)
                }
            }
            if (gmat_ == 2 && planned != NativeGBuffer::kPlanNone) {
                // verify: the walk's plan too (what the member would have had)
                const bool walked = RedecodeStage(pp, b.user_data[ps], {}, run_ps_check_);
                ngb_.CheckMemberPlan(planned, walked, run_ps_plan_, run_ps_check_);
            }
        }
        if (planned == NativeGBuffer::kPlanOther) {
            return no(12); // (another module or other bindings: the walk's plan is the same)
        }
        if (planned == NativeGBuffer::kPlanNone && !RedecodeStage(pp, b.user_data[ps], {}, run_ps_plan_)) {
            return no(12);
        }
        if (!RunHeld() || frontend_->MemoryGeneration() != run_gen_) {
            return no(10); // (a walk's read drained the queue)
        }
        const GS::ResourceBindingPlan& x = pp.plan;
        const GS::ResourceBindingPlan& y = run_ps_plan_;
        bool same = x.images.size() == y.images.size() && x.samplers.size() == y.samplers.size() &&
                    x.buffers.size() == y.buffers.size() && x.vertices.empty() && y.vertices.empty();
        // SSPC_CITY_GB_RUN_MATERIAL: an image or sampler of the same kind with another T# / S# is the member's own (one
        // sampled view of a real image; a null one binds the null descriptor whatever its T#, a sampler with a bad S#
        // the default one - the same as the first draw's)
        u32 img_mask = 0, smp_mask = 0;
        for (size_t i = 0; same && i < x.images.size(); ++i) {
            const GS::ImageBinding& p = x.images[i];
            const GS::ImageBinding& q = y.images[i];
            same = p.binding == q.binding && p.count == q.count && p.view_type == q.view_type &&
                   p.storage == q.storage && p.depth == q.depth && p.array == q.array && p.fmask == q.fmask &&
                   p.status == q.status;
            if (same && p.desc.raw != q.desc.raw && (mat == 0 || q.status == GS::ResourceStatus::Ok)) {
                if (mat == 0) {
                    same = false;
                } else if (i >= Cp::RunMember::kMaxSharpIndex) {
                    return no(18);
                } else if (q.count > 1 || q.storage || q.fmask || q.atomic || p.atomic) {
                    return no(17);
                } else {
                    img_mask |= 1u << i;
                }
            }
        }
        for (size_t i = 0; same && i < x.samplers.size(); ++i) {
            const GS::SamplerBinding& p = x.samplers[i];
            const GS::SamplerBinding& q = y.samplers[i];
            same = p.binding == q.binding && p.status == q.status;
            if (same && p.desc.raw != q.desc.raw && (mat == 0 || q.status == GS::ResourceStatus::Ok)) {
                if (mat == 0) {
                    same = false;
                } else if (i >= Cp::RunMember::kMaxSharpIndex) {
                    return no(18);
                } else {
                    smp_mask |= 1u << i;
                }
            }
        }
        if (!same) {
            return no(13);
        }
        m.img_mask = img_mask;
        m.smp_mask = smp_mask;
        own_img = img_mask;
        own_smp = smp_mask;
        ps_redecoded = true;
        for (size_t i = 0; i < x.buffers.size(); ++i) {
            const GS::BufferBinding& p = x.buffers[i];
            const GS::BufferBinding& q = y.buffers[i];
            if (p.binding == q.binding && p.kind == q.kind && p.desc.raw == q.desc.raw && p.address == q.address &&
                p.size == q.size && p.written == q.written && p.formatted == q.formatted && p.status == q.status) {
                continue;
            }
            if (p.binding != q.binding || p.kind != GS::BufferKind::Guest || q.kind != GS::BufferKind::Guest ||
                p.written || q.written || p.formatted != q.formatted || p.status != GS::ResourceStatus::Ok ||
                q.status != GS::ResourceStatus::Ok || (p.desc.raw[1] >> 16) != (q.desc.raw[1] >> 16) ||
                p.desc.raw[3] != q.desc.raw[3] || q.address == 0 || q.size == 0 || q.size > (16u << 10) ||
                m.ps_buffers >= Cp::RunMember::kMaxBuffers) {
                return no(14);
            }
            m.pb[m.ps_buffers++] = {static_cast<u32>(i), q.address, q.size, q.desc.raw};
        }
        u32 pdiff = 0;
        for (u32 i = 0; i < 16; ++i) {
            pdiff |= a.user_data[ps][i] != b.user_data[ps][i] ? 1u << i : 0u;
        }
        m.own_ps = true;
        m.own_ps_user_data = (pdiff & pp.module->result.reflection.user_data_used) != 0;
        m.ps_user_data = b.user_data[ps];
        if (x.flat.size() != y.flat.size()) {
            return no(13);
        }
        if (!std::equal(x.flat.begin() + 16, x.flat.end(), y.flat.begin() + 16)) {
            // (its walk's output differs: its whole flattened user data in the run's store)
            m.own_ps_user_data = true;
            m.ps_flat_at = static_cast<u32>(lead.member_flat.size());
            lead.member_flat.insert(lead.member_flat.end(), y.flat.begin(), y.flat.end());
        }
        if (srt_diag_) {
            Bump(srt_by_[a.engine_run & 3].own_ps);
        }
    }
    if (!lead.members.empty()) {
        // thread 2 records a member on the descriptors the one before it left (Executor::DrawRunMembers pushes only a
        // member's own over them): what that one had of its own and this one has not, this one has again - the run's
        // (the first draw's: this one's are those), with its own registers
        const Cp::RunMember& prev = lead.members.back();
        u32 again = 0;
        const auto restore = [&](const Cp::RunMember::Buffer* before, u32 n_before, Cp::RunMember::Buffer* own,
                                 u32& n_own, const GS::ResourceBindingPlan& run) {
            for (u32 k = 0; k < n_before; ++k) {
                const u32 index = before[k].index;
                if (std::any_of(own, own + n_own, [&](const Cp::RunMember::Buffer& o) { return o.index == index; })) {
                    continue;
                }
                if (n_own >= Cp::RunMember::kMaxBuffers || index >= run.buffers.size()) {
                    return false;
                }
                const GS::BufferBinding& q = run.buffers[index];
                own[n_own++] = {index, q.address, q.size, q.desc.raw};
                ++again;
            }
            return true;
        };
        if (!restore(prev.b.data(), prev.buffers, m.b.data(), m.buffers, pv.plan)) {
            return no(19);
        }
        if (prev.own_user_data && !m.own_user_data) {
            m.own_user_data = true; // (the run's past the registers: vs_flat_at stays kNoFlat)
            ++again;
        }
        if (prev.own_ps && !m.own_ps) {
            m.own_ps = true; // (its PS user data are the first draw's)
            m.ps_user_data = b.user_data[ps];
        }
        if (prev.own_ps) {
            if (!restore(prev.pb.data(), prev.ps_buffers, m.pb.data(), m.ps_buffers, lead.ps.plan)) {
                return no(19);
            }
            if (prev.own_ps_user_data && !m.own_ps_user_data) {
                m.own_ps_user_data = true; // (ps_flat_at stays kNoFlat)
                ++again;
            }
            again += static_cast<u32>(std::popcount(prev.img_mask & ~m.img_mask) +
                                      std::popcount(prev.smp_mask & ~m.smp_mask));
            m.img_mask |= prev.img_mask;
            m.smp_mask |= prev.smp_mask;
        }
        if (again != 0) {
            Bump(run_restored_, again);
        }
    }
    if ((m.img_mask | m.smp_mask) != 0) {
        // its T#s / S#s (RunMember::sharps_at): its own from its PS plan decoded again, those it has again the run's
        // (the same in that plan where it has none of its own)
        const GS::ResourceBindingPlan& sp = ps_redecoded ? run_ps_plan_ : lead.ps.plan;
        m.sharps_at = static_cast<u32>(lead.member_flat.size());
        for (u32 mk = m.img_mask; mk != 0; mk &= mk - 1) {
            const auto& raw = sp.images[std::countr_zero(mk)].desc.raw;
            lead.member_flat.insert(lead.member_flat.end(), raw.begin(), raw.end());
        }
        for (u32 mk = m.smp_mask; mk != 0; mk &= mk - 1) {
            const auto& raw = sp.samplers[std::countr_zero(mk)].desc.raw;
            lead.member_flat.insert(lead.member_flat.end(), raw.begin(), raw.end());
        }
    }
    if (g) {
        // (its stencil reference at its packet: thread 2 sets it for its draw - `regs` are the registers at the packet)
        constexpr u32 kDbStencilRefMask = 0xA10C, kDbStencilRefMaskBf = 0xA10D;
        m.own_stencil = true;
        m.stencil_ref = regs.Get(kDbStencilRefMask);
        m.stencil_ref_bf = regs.Get(kDbStencilRefMaskBf);
    }
    m.seq = b.seq;
    m.packet_addr = b.packet_addr;
    m.index_base = b.index_base;
    m.index_offset = b.index_offset;
    m.index_count = b.index_count;
    m.instance_count = b.instance_count;
    m.max_index_size = b.max_index_size;
    m.vgt_index_offset = b.vgt_index_offset;
    m.changed = b.changed;
    m.vs_user_data = b.user_data[vs];
    if (runs_ == 2 || gruns_ == 2 || (gmat_ == 2 && g) || (run_ps_ == 2 && psm && !g)) {
        // verify: the full preparation of this draw against the first's with its geometry (it must be the same)
        // (R8: the groups changed at draws the native G-buffer path prepared without the frontend - the run's first
        // one too - count as changed, as in PrepareDraw: else the frontend kept older registers - a PS translated with
        // another SPI_PS_IN_CONTROL, a bridge assert)
        Cp::DrawDescriptor bv = b;
        bv.changed |= ngb_pending_;
        ngb_pending_ = 0;
        const GS::DrawPreparation& prep = frontend_->PrepareDraw(bv, regs, reader_);
        GS::StageView expect = pv;
        std::copy(m.vs_user_data.begin(), m.vs_user_data.end(), expect.plan.flat.begin());
        if (m.vs_flat_at != Cp::RunMember::kNoFlat) {
            std::copy_n(lead.member_flat.begin() + m.vs_flat_at, expect.plan.flat.size(), expect.plan.flat.begin());
        }
        GS::StageView expect_ps = lead.ps;
        if (m.own_ps) {
            std::copy(m.ps_user_data.begin(), m.ps_user_data.end(), expect_ps.plan.flat.begin());
            if (m.ps_flat_at != Cp::RunMember::kNoFlat) {
                std::copy_n(lead.member_flat.begin() + m.ps_flat_at, expect_ps.plan.flat.size(),
                            expect_ps.plan.flat.begin());
            }
            for (u32 k = 0; k < m.ps_buffers; ++k) {
                GS::BufferBinding& bb = expect_ps.plan.buffers[m.pb[k].index];
                bb.address = m.pb[k].address;
                bb.size = m.pb[k].size;
                bb.desc.raw = m.pb[k].desc;
            }
            if (m.sharps_at != Cp::RunMember::kNoFlat) { // (SSPC_CITY_GB_RUN_MATERIAL: its T#s / S#s)
                auto at = lead.member_flat.begin() + m.sharps_at;
                for (u32 mk = m.img_mask; mk != 0; mk &= mk - 1, at += 8) {
                    std::copy_n(at, 8, expect_ps.plan.images[std::countr_zero(mk)].desc.raw.begin());
                }
                for (u32 mk = m.smp_mask; mk != 0; mk &= mk - 1, at += 4) {
                    std::copy_n(at, 4, expect_ps.plan.samplers[std::countr_zero(mk)].desc.raw.begin());
                }
            }
        }
        for (u32 i = 0; i < m.streams; ++i) {
            GS::VertexBinding& v = expect.plan.vertices[i];
            v.read = m.v[i].read;
            v.desc.raw = m.v[i].desc;
            v.address = m.v[i].address;
            v.size = m.v[i].size;
        }
        for (u32 k = 0; k < m.buffers; ++k) {
            GS::BufferBinding& bb = expect.plan.buffers[m.b[k].index];
            bb.address = m.b[k].address;
            bb.size = m.b[k].size;
            bb.desc.raw = m.b[k].desc;
        }
        const bool same = prep.ready && prep.pipeline == lead.prep.pipeline &&
                          frontend_->Pipelines().Graphics(prep.pipeline) == lead.state &&
                          frontend_->SharedStage(GS::HwStage::Vs) == nullptr &&
                          frontend_->SharedStage(GS::HwStage::Ps) == nullptr &&
                          SameStageView(frontend_->Stage(GS::HwStage::Vs), expect) &&
                          SameStageView(frontend_->Stage(GS::HwStage::Ps), expect_ps);
        run_verified_.store(run_verified_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (!same) {
            const u64 n = run_mismatches_.load(std::memory_order_relaxed) + 1;
            run_mismatches_.store(n, std::memory_order_relaxed);
            if (n <= 12) {
                SSPC_LOG(GPU, Warning, "SSPC_CITY_ENGINE_OPT=verify: run member at {:#x} (VS {:#x}) differs from its full "
                                       "preparation (ready {}, pipeline {}/{})", b.packet_addr, a.shaders[vs].address,
                         prep.ready, prep.pipeline, lead.prep.pipeline);
            }
            return no(11); // (recorded on its own)
        }
        if (frontend_->MemoryGeneration() != run_gen_ || !RunHeld()) {
            return no(10);
        }
    }
    if (lead.members.size() == lead.members.capacity() && R6::Opt().run_pool) { // (R6-P: SSPC_R6_CP_RUN_POOL=1)
        if (lead.members.capacity() == 0) {
            lead.members.reserve(32); // (the slot's vector went to the pool: one allocation instead of a growth chain)
        } else {
            R6::TakeMembers(lead.members);
        }
    }
    lead.members.push_back(m);
    if (srt_diag_) {
        Bump(srt_by_[a.engine_run & 3].members);
    }
    if (g && b.engine_material) {
        Bump(run_material_joined_);
    }
    if (ps_redecoded && !g) {
        Bump(run_ps_joined_[a.engine_run & 3]);
    }
    if ((own_img | own_smp) != 0) {
        Bump(gmat_joined_);
        Bump(gmat_images_, static_cast<u64>(std::popcount(own_img)));
        Bump(gmat_samplers_, static_cast<u64>(std::popcount(own_smp)));
    }
    return true;
}

void ParallelCp::Dispatch(const Cp::DispatchDescriptor& dispatch) {
    R8_CP_PROF(CpProfile::Sec::ParDispatch);
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
    R8_CP_PROF(CpProfile::Sec::CbCache);
    SSPC_P410_QUEUED_CALL(Kind::CacheAction, kQCache, DrainReason::CacheAction, exec_.CacheAction(op), r.cache = op);
}

void ParallelCp::Fence(const Cp::FenceOp& op) {
    R8_CP_PROF(CpProfile::Sec::CbFence);
    SSPC_P410_QUEUED_CALL(Kind::Fence, kQFence, DrainReason::Fence, exec_.Fence(op), r.fence = op);
}

void ParallelCp::Dma(const Cp::DmaOp& op) {
    R8_CP_PROF(CpProfile::Sec::CbDma);
    // R6-P (SSPC_R6_CP_PREFETCH=skip): Gnm's L2 prefetch writes no memory - nothing for thread 2, no memory change
    if (const R6::Options& r6 = R6::Opt(); r6.diag || r6.prefetch_skip) {
        R6::Counters& c = R6::Count();
        if (r6.diag) {
            R6::Bump(c.dma_by[static_cast<u32>(op.kind) & (R6::kDmaKinds - 1)]);
            R6::Bump(c.dma_run_open, run_open_ ? 1 : 0);
        }
        if (r6.prefetch_skip && op.kind == Cp::DmaKind::ReadTagsPrefetch && !op.executed && !op.timeline) {
            R6::Bump(c.prefetch_skipped);
            R6::Bump(c.prefetch_skipped_run_open, run_open_ ? 1 : 0);
            return;
        }
    }
    SSPC_P410_QUEUED_CALL(Kind::Dma, kQDma, DrainReason::Dma, exec_.Dma(op), r.dma = op);
}

void ParallelCp::WaitMetByHeldFence(u32 queue, u64 after_seq) {
    R8_CP_PROF(CpProfile::Sec::CbWaitMet);
    SSPC_P410_QUEUED_CALL(Kind::WaitMet, kQWaitMet, DrainReason::WaitMet, exec_.WaitMetByHeldFence(queue, after_seq),
                          r.queue = queue; r.seq = after_seq);
}

void ParallelCp::MemoryAcquired(u32 queue, bool blocked, u64 poll_address) {
    R8_CP_PROF(CpProfile::Sec::CbAcquire);
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
    R8_CP_PROF(CpProfile::Sec::CbCopy);
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
    R8_CP_PROF(CpProfile::Sec::CbFlip);
    if (async_flip_ && started_ && !exec_.EvictionDue() && !StallTrace::On() && !Inspector::On()) {
        // Native CP: executed by thread 2 behind the frame's draws; the frontend's frame begins here (thread 1's order)
        Record& r = BeginRecord(Kind::Flip);
        r.flip = marker;
        r.flip_totals = {};
        if (flip_snapshot_ != nullptr) {
            flip_snapshot_(flip_snapshot_ctx_, r.flip_totals);
        }
        ++t1_.queued_flips;
        PushRecord();
        if (!fthread_mode_) {
            frontend_->NewFrame();
        }
        return;
    }
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
    R8_CP_PROF(CpProfile::Sec::CbSync);
    Drain(DrainReason::Synchronize);
    exec_.Synchronize(queue);
    SnapshotTimeline();
}

bool ParallelCp::WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) {
    R8_CP_PROF(CpProfile::Sec::CbWrite);
    if (async_writes_ && !async_gds_only_ && started_ && !Idle()) {
        // CP-9: accepted by its shape, recorded in stream order (RunWrite: the CPU writes there after a refusal)
        if (!Vk::Executor::TimelineWriteShapeOk(dst, bytes)) {
            return false; // (the processor writes it, draining first - as the executor's refusal)
        }
        Record& r = BeginRecord(Kind::Write);
        r.queue = queue;
        r.seq = seq;
        r.dst = dst;
        r.bytes = bytes;
        r.payload.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + bytes);
        ++writes_queued_;
        queued_tw_.emplace_back(dst, dst + bytes);
        qtw_lo_ = std::min(qtw_lo_, dst);
        qtw_hi_ = std::max(qtw_hi_, dst + bytes);
        if (frontend_ != nullptr && !fthread_mode_) {
            AddGpuWrite(dst, dst + bytes, next_, kLookup);
        }
        PushRecord();
        FrontMemoChanged();
        return true;
    }
    Drain(DrainReason::TimelineWrite);
    const bool ok = exec_.WriteOnTimeline(queue, seq, dst, data, bytes);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr) {
        AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
    }
    return ok;
}

bool ParallelCp::ZpassOnTimeline(u32 queue, u64 seq, u64 dst, u32 db_mask, u32 stride) {
    R8_CP_PROF(CpProfile::Sec::CbZpass);
    if (async_writes_ && !async_gds_only_ && started_ && !Idle()) {
        // CP-9 (SSPC_CP_ASYNC_WRITES=1): after the draws queued before it (the query's own draw among them) in stream
        // order, as a queued timeline write (RunZpass: the CPU writes the counts there after a refusal)
        if (!Vk::Executor::ZpassShapeOk(dst, db_mask, stride)) {
            return false; // (the processor writes its counts, draining first - as the executor's refusal)
        }
        const u64 bytes = u64(std::popcount(db_mask)) * 16; // (the end's counts: within the begin's DB records)
        Record& r = BeginRecord(Kind::Zpass);
        r.queue = queue;
        r.seq = seq;
        r.dst = dst;
        r.bytes = bytes;
        r.gds_offset = db_mask;
        r.pattern = stride;
        ++writes_queued_;
        queued_tw_.emplace_back(dst, dst + bytes);
        qtw_lo_ = std::min(qtw_lo_, dst);
        qtw_hi_ = std::max(qtw_hi_, dst + bytes);
        if (frontend_ != nullptr && !fthread_mode_) {
            AddGpuWrite(dst, dst + bytes, next_, kLookup);
        }
        PushRecord();
        FrontMemoChanged();
        return true;
    }
    // after the draws queued before it (the query's own draw among them), as the executor records it
    Drain(DrainReason::TimelineWrite);
    const bool ok = exec_.ZpassOnTimeline(queue, seq, dst, db_mask, stride);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr) {
        AddGpuWrite(dst, dst + 16ull * stride, next_, exec_.RecordedValue());
    }
    return ok;
}

bool ParallelCp::GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                               u32 pattern) {
    R8_CP_PROF(CpProfile::Sec::CbGds);
    if (async_writes_ && started_ && !Idle()) {
        if (!exec_.GdsShapeOk(kind, memory, gds_offset, bytes)) {
            return false;
        }
        Record& r = BeginRecord(Kind::Gds);
        r.queue = queue;
        r.seq = seq;
        r.gds_kind = kind;
        r.src = memory;
        r.gds_offset = gds_offset;
        r.bytes = bytes;
        r.pattern = pattern;
        ++writes_queued_;
        if (kind == Cp::DmaKind::GdsToMemory) {
            queued_tw_.emplace_back(memory, memory + bytes);
            qtw_lo_ = std::min(qtw_lo_, memory);
            qtw_hi_ = std::max(qtw_hi_, memory + bytes);
            if (frontend_ != nullptr && !fthread_mode_) {
                AddGpuWrite(memory, memory + bytes, next_, kLookup);
            }
        }
        PushRecord();
        FrontMemoChanged();
        return true;
    }
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
        out += fmt::format("  front mode runs: draws joined on the frontend thread {}, runs published for thread 1's "
                           "wait {} / after {:.0f} us without a record {}\n",
                           f_run_joined_.load(std::memory_order_relaxed),
                           f_run_closed_flush_.load(std::memory_order_relaxed),
                           static_cast<double>(f_hold_ticks_) * 1e6 / static_cast<double>(Kernel::Clock::TscFrequency()),
                           f_run_closed_idle_.load(std::memory_order_relaxed));
        out += fmt::format("  R8 waits for a held run: thread 2 at its first draw {:.0f} ms ({} times); thread 1's full "
                           "queue while a run was held {:.0f} ms ({} times)\n",
                           ms(t2_idle_held_.load(std::memory_order_relaxed)),
                           t2_idle_held_n_.load(std::memory_order_relaxed),
                           ms(t1_full_held_.load(std::memory_order_relaxed)),
                           t1_full_held_n_.load(std::memory_order_relaxed));
    }
    if (geofast_ != 0) {
        out += fmt::format("  CITY-OPT geometry-only draw preparations (SSPC_CITY_GEOFAST): taken {}, verified {} "
                           "(mismatches {})\n",
                           geofast_hits_.load(std::memory_order_relaxed), geofast_verified_.load(std::memory_order_relaxed),
                           geofast_mismatches_.load(std::memory_order_relaxed));
    }
    const auto ld = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    if (runs_ != 0) {
        const auto& es = Engine::Render::DrawRuns::Stats();
        std::string why;
        for (size_t i = 1; i < run_refused_.size(); ++i) {
            why += fmt::format(" {}:{}", i, ld(run_refused_[i]));
        }
        out += fmt::format("  CITY-OPT Phase 5 engine runs (SSPC_CITY_ENGINE_OPT{}): engine draws tagged - mesh {} of {}, "
                           "shadow {} of {} (no trailing NOP {}); runs {} with {} draws joined (first draws held {}); "
                           "refused (1 full, 2 memory, 3 groups, 4 draw, 5 shaders, 6 other user data, 7 VS user data, "
                           "8 table read, 9 V#, 10 published meanwhile, 11 verify, 12 VS/PS decoded again refused, 13 other "
                           "bindings, 14 a buffer not its own constants, 15 VS walk read a register that differs, 16 another "
                           "material, 17 another kind of image/sampler, 18 an image/sampler index past 32, 19 the earlier "
                           "member's own bindings again past 2 buffers):{}; verified {} (mismatches {}); bindings had "
                           "again (the member before had them as its own) {}; instanced mesh draws (0x1a1770) tagged {} of "
                           "{}{}; materials set (0x448640) {}, G-buffer draws tagged with another material {} (as members "
                           "{}, joined anyway {})\n",
                           runs_ == 2 ? "=verify" : "", ld(es.tagged[1]), ld(es.calls[1]), ld(es.tagged[2]),
                           ld(es.calls[2]), ld(es.no_nop), ld(run_count_), ld(run_members_), ld(run_leaders_), why,
                           ld(run_verified_), ld(run_mismatches_), ld(run_restored_), ld(es.tagged[3]), ld(es.calls[3]),
                           gruns_ != 0 ? fmt::format(" - their runs (SSPC_CITY_ENGINE_GBUFFER_RUNS{})",
                                                     gruns_ == 2 ? "=verify" : "")
                                       : std::string{},
                           ld(es.materials), ld(es.material_tagged), ld(run_material_), ld(run_material_joined_));
        constexpr const char* kRunSrc[] = {"untagged", "mesh 0x1a1960", "shadow 0x43d480", "G-buffer 0x1a1770"};
        for (size_t src = 1; src < gb_groups_.size(); ++src) {
            std::string groups;
            for (size_t k = 0; k < gb_groups_[src].size(); ++k) {
                if (ld(gb_groups_[src][k]) != 0) {
                    groups += fmt::format(" {}:{}", k, ld(gb_groups_[src][k]));
                }
            }
            out += fmt::format("  R8/R10 {} members refused for registers (3), by group (Cp::Group):{}\n", kRunSrc[src],
                               groups.empty() ? std::string(" none") : groups);
        }
        if (run_ps_ != 0) {
            out += fmt::format("  R10 mesh / shadow members with a PS of their own (SSPC_R10_RUN_PS{}{}): mesh {}, shadow {}\n",
                               run_ps_ == 2 ? "=verify" : "", run_ps_mat_ != 0 ? ", own T#s/S#s" : "",
                               ld(run_ps_joined_[1]), ld(run_ps_joined_[2]));
        }
        if (gmat_ != 0) {
            out += fmt::format("  R8 G-buffer members with another material (SSPC_CITY_GB_RUN_MATERIAL{}): joined with "
                               "images/samplers of their own {} (images {}, samplers {})\n",
                               gmat_ == 2 ? "=verify" : "", ld(gmat_joined_), ld(gmat_images_), ld(gmat_samplers_));
        }
    }
    if (ngb_.On()) {
        out += "  " + ngb_.Report();
    }
    for (u32 i = 0; i < 2; ++i) {
        if (ndr_[i].On()) {
            const auto st = Engine::Render::DrawRuns::StagesStats();
            out += fmt::format("  R10 engine captures ({}): made {}, not captured {}, game threads {:.1f} ms in all\n",
                               i == 0 ? "mesh 0x1a1960" : "shadow 0x43d480", st.made[i], st.refused[i],
                               static_cast<double>(GS::TicksToNs(st.ticks[i])) / 1e6);
            out += "  " + ndr_[i].Report();
        }
    }
    if (resources_ != 0 || gruns_ != 0) {
        std::string why;
        for (size_t i = 1; i < res_refused_.size(); ++i) {
            why += fmt::format(" {}:{}", i, ld(res_refused_[i]));
        }
        out += fmt::format("  CITY-OPT Phase 6 SRT walks taken again (SSPC_CITY_ENGINE_RESOURCES{}): VS {}, PS {}; "
                           "walked again for the new registers: VS {}, PS {}; refused (2 not reusable, 3 layout, 4 "
                           "another key, 5 other bindings, 6 the walk failed):{}\n",
                           resources_ == 2 ? "=verify" : resources_ == 0 ? "=0" : "", ld(res_walk_vs_), ld(res_walk_ps_),
                           ld(res_rewalk_vs_), ld(res_rewalk_ps_), why);
    }
    if (srt_diag_) {
        static constexpr const char* kSrc[] = {"untagged", "mesh 0x1a1960", "shadow 0x43d480", "G-buffer 0x1a1770"};
        const double tpu = Kernel::Clock::TscFrequency() / 1e6;
        out += "  CITY-OPT Phase 6 draw preparations by engine function (SSPC_DIAG_ENGINE_SRT; us per draw; template "
               "refusals: 1 not after a prepared draw/memory, 3 groups, 5 programs, 6 PS user data, 7 VS walk, 9 VS user "
               "data, 10 writes, 11 vertex reads, 12/13 VS/PS decoded again refused, 14 VS walk read a register, 15 "
               "not asked):\n";
        for (size_t i = 0; i < srt_by_.size(); ++i) {
            const SrtDiag& sd = srt_by_[i];
            const u64 full = ld(sd.full), fast = ld(sd.fast);
            const double nf = static_cast<double>(std::max<u64>(full, 1));
            std::string w;
            for (size_t k = 0; k < sd.why.size(); ++k) {
                if (ld(sd.why[k]) != 0) {
                    w += fmt::format(" {}:{}", k, ld(sd.why[k]));
                }
            }
            w += "; run refused for groups";
            for (size_t k = 0; k < sd.groups.size(); ++k) {
                if (ld(sd.groups[k]) != 0) {
                    w += fmt::format(" g{}:{}", k, ld(sd.groups[k]));
                }
            }
            out += fmt::format("    {}: full {} ({:.2f} us, of it the stage copies {:.2f} us; SRT reads {:.1f}, walks VS "
                               "{:.0f}% PS {:.0f}%, flat {:.1f}/{:.1f} dwords, images {:.1f}, buffers {:.1f}), from the draw "
                               "before {} ({:.2f} us), run members {} (own PS {}; joining {:.2f} us, refused attempts {} "
                               "{:.2f} us); why full:{}\n",
                               kSrc[i], full, ld(sd.t_full) / tpu / nf, ld(sd.t_copy) / tpu / nf, ld(sd.reads) / nf,
                               100.0 * ld(sd.walk_vs) / nf, 100.0 * ld(sd.walk_ps) / nf, ld(sd.flat_vs) / nf,
                               ld(sd.flat_ps) / nf, ld(sd.images) / nf, ld(sd.buffers) / nf, fast,
                               ld(sd.t_fast) / tpu / static_cast<double>(std::max<u64>(fast, 1)), ld(sd.members),
                               ld(sd.own_ps), ld(sd.t_join) / tpu / static_cast<double>(std::max<u64>(ld(sd.joins), 1)),
                               ld(sd.refusals),
                               ld(sd.t_refused) / tpu / static_cast<double>(std::max<u64>(ld(sd.refusals), 1)), w);
        }
    }
    if (drawcache_) {
        out += fmt::format("  CITY-OPT draw templates (SSPC_CITY_DRAW_CACHE): of those, VS decoded again {}, PS {}; "
                           "refused for another module or bindings {}\n",
                           dc_vs_hits_.load(std::memory_order_relaxed), dc_ps_hits_.load(std::memory_order_relaxed),
                           dc_key_refused_.load(std::memory_order_relaxed));
    }
    if (census_end_ != 0) {
        const std::scoped_lock lock(census_mu_);
        std::vector<const CensusRow*> rows;
        u64 d = 0, t1 = 0, t2 = 0;
        for (const CensusRow& c : census_) {
            rows.push_back(&c);
            d += c.draws;
            t1 += c.t1;
            t2 += c.t2;
        }
        std::ranges::sort(rows, [](const CensusRow* x, const CensusRow* y) { return x->t1 + x->t2 > y->t1 + y->t2; });
        const u64 flips = census_end_ - census_first_;
        out += fmt::format("  CITY-OPT draw census (SSPC_DIAG_PREP_CENSUS, flips {}..{}): {} draws ({:.0f}/flip), "
                           "thread 1 {:.1f} ms/flip, thread 2 {:.1f} ms/flip; why GEOFAST did not take a draw: 1 not "
                           "after a prepared draw/memory, 2 that one shared, 3 other groups, 4 stages, 5 programs, 6 PS "
                           "user data, 7 VS walk/no vertices, 8 no fetch, 9 VS user data, 10 writes, 11 vertex reads, "
                           "12/13 VS/PS template refused, 15 off\n",
                           census_first_, census_end_ - 1, d, static_cast<double>(d) / flips, ms(t1) / flips,
                           ms(t2) / flips);
        const auto masks = [](const std::vector<std::pair<u64, u64>>& v) {
            std::string t;
            for (const auto& [m, n] : v) {
                t += fmt::format(" {:x}:{}", m, n);
            }
            return t;
        };
        for (size_t i = 0; i < std::min<size_t>(rows.size(), 40); ++i) {
            const CensusRow& c = *rows[i];
            const double n = static_cast<double>(c.draws);
            std::string why;
            for (size_t w = 0; w < c.why.size(); ++w) {
                why += c.why[w] != 0 ? fmt::format(" {}:{}", w, c.why[w]) : "";
            }
            why += " | batch end";
            for (size_t w = 0; w < c.batch_end.size(); ++w) {
                why += c.batch_end[w] != 0 ? fmt::format(" {}:{}", w, c.batch_end[w]) : "";
            }
            out += fmt::format("    VS {:#x} PS {:#x}: {:.0f}/flip, t1 {:.2f} ms/flip ({:.2f} us), t2 {:.2f} ms/flip "
                               "({:.2f} us/record, {} records) | flat {:.0f}/{:.0f} srt reads {:.1f} buffers "
                               "{:.1f}/{:.1f} PS images {:.1f} vertices {:.1f} | fast/why{} | groups{} | VS ud{} | PS ud{}\n",
                               c.vs, c.ps, n / flips, ms(c.t1) / flips, ms(c.t1) * 1000.0 / n, ms(c.t2) / flips,
                               c.t2_draws != 0 ? ms(c.t2) * 1000.0 / static_cast<double>(c.t2_draws) : 0.0, c.t2_draws,
                               c.vs_flat / n, c.ps_flat / n, c.srt_reads / n, c.vs_buffers / n, c.ps_buffers / n,
                               c.ps_images / n, c.vertices / n, why, masks(c.groups), masks(c.vs_ud), masks(c.ps_ud));
        }
    }
    if (stage_reuse_ != 0) {
        out += fmt::format("  FB-6 unchanged draw stages not copied (SSPC_FB_STAGE_REUSE={}): taken from the last copy "
                           "{}, copied {}; verify mismatches {}\n",
                           stage_reuse_ == 2 ? "verify" : "1", reuse_same_.load(std::memory_order_relaxed),
                           reuse_copied_.load(std::memory_order_relaxed),
                           reuse_mismatches_.load(std::memory_order_relaxed));
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
    if (async_flip_) {
        out += fmt::format("  Native CP: flip markers queued to the recording thread (SSPC_CP_ASYNC_FLIP=1) {}\n",
                           t.queued_flips);
    }
    out += "  drains that waited, by reason:";
    for (size_t i = 0; i < t.drains_by.size(); ++i) {
        if (t.drains_by[i] != 0) {
            out += fmt::format(" {} {} ({:.0f} ms);", DrainReasonName(static_cast<DrainReason>(i)), t.drains_by[i],
                               ms(t.drain_ticks_by[i]));
        }
    }
    out += "\n";
    out += R6::Report(); // (R6-P: docs/NATIVE_RENDER_R6_CP_OPTIMIZATION.md; empty when off)
    if (r8_diag_ != nullptr) {
        out += r8_diag_->Report(); // (R8-A2: docs/NATIVE_ENGINE_R8_CP_OPTIMIZATION.md)
    }
    if (Failed()) {
        out += "  FAILED: " + failure_ + "\n";
    }
    return out;
}

} // namespace SecondSon::Gpu::Native
