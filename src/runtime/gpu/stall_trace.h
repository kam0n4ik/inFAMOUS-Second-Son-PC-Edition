// SecondSonPC — the stall trace (Phase 4, P4.12; docs/PHASE4_P412_FULL_STALL_ANALYSIS.md): where the native backend's
// threads wait and for whom - the command processor's thread (thread 1), the parallel recording thread (thread 2), the
// VideoOut present thread, the game's submitting threads - and when the GPU executed each batch.
//
// Off unless SSPC_P412_STALL_TRACE is set; then every hook is one relaxed load and a branch. Values:
//   1 | light        per event type and thread: count, total, max, a log histogram (p50/p95/p99), totals per frame -
//                    no event is kept; the GPU's idle gaps between batches (GPU clock) per frame
//   deep[,<opts>]    light, and inside a time window every event with its start/end (TSC), the GPU's batches (submit
//                    time, GPU start/end converted to the TSC by VK_EXT_calibrated_timestamps when the device has it),
//                    timestamped stack samples of threads 1 and 2; written at the stop
//     window=<a>:<b>   seconds of the frame log's time (default 25:37)
//     events=<n>       events kept by thread 2 at most (default 6291456: 192 MiB; thread 1 a quarter, others 256 Ki)
//     sample=<hz>      stack samples of threads 1 and 2 per second each (default 1000 requested - the timer gives
//                      ~600; 0 = none); each sample records how long its thread was suspended (the stall it adds).
//                      The unwinder takes the loader's function-table locks while a thread is suspended - as
//                      CpuSampler: diagnostic use only (a thread suspended inside exception dispatch would hang it)
//     records=0        no span per executed record on thread 2 (the default keeps them)
// Files (at the backend's stop, in its logs directory): p412_light.json, p412_frames.csv (n, thread, event, count, ms -
// nonzero only); deep: p412_trace.bin (tools\p412\p412_analyze.py reads it).
//
// Nothing is allocated, locked or formatted on the hot path: each thread writes its own preallocated ring and counters
// (a thread's log is made when it first records); a ring that is full drops (counted) - the window keeps it bounded.
#pragma once

#include <atomic>
#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::StallTrace {

/// Whose thread recorded an event.
enum class Role : u8 { Cp, Record, Present, Game, Other, Count };

/// The events (spans unless named instant). The thread is the log's, not part of the type.
enum class Ev : u16 {
    // thread 1 (the command processor's loop)
    PreRound,       ///< the loop's work before a round: submissions taken, invalidations, the guest's CPU writes drained
    Poll,           ///< Executor::Poll (completed batches, labels written); a = the round's PM4 packets before it,
                    ///< b = the queues that stopped at the round's limit of 4096 packets, c = 1: the graphics queue did
    IdleEmpty,      ///< idle, nothing to process and no GPU work in flight (waits for the game's submissions)
    IdleGpu,        ///< idle, nothing to process, waits for the oldest in-flight batch (a = the last submitted value)
    IdleLabel,      ///< idle, a WAIT_REG_MEM blocked on a VideoOut buffer label (a flip still to be shown)
    IdleFlag,       ///< idle, a WAIT_REG_MEM blocked on other memory (flags the game's CPU writes)
    Drain,          ///< thread 1 waits until thread 2 executed every record (a = target record, c = DrainReason)
    QueueFull,      ///< thread 1 waits for a free slot (backpressure; a = target record)
    JournalFull,    ///< thread 1 waits for journal room (a = target record)
    FlipMarker,     ///< instant: the processor reached a flip marker (a = the frame's timeline value, b = frame index)
    GpuCompleted,   ///< instant: thread 1 saw the timeline reach a = value (EOP labels written now)
    // thread 2 (the recording thread)
    RecordWait,     ///< thread 2 waits for a record (a = the push TSC of the record it got, b = its index low 32 bits)
    Record,         ///< deep: thread 2 executes one record (b = index low 32 bits, c = kind)
    // either thread that records Vulkan work
    GpuSync,        ///< Executor::SyncFrom: waits for every submitted batch (a = value, c = source 0 packet/1 frontend/2 read)
    RingWait,       ///< the stream ring waits for an old batch (a = value)
    BindWait,       ///< sparse binds waited for on the host (validation only)
    QueueLock,      ///< waits for the Vulkan queue's lock (another thread submits or presents)
    Submit,         ///< vkQueueSubmit2 of a batch (a = timeline value), the lock wait excluded
    PipelineCreate, ///< a graphics/compute pipeline created (the driver's compile)
    ShaderTranslate,///< a GCN program translated to SPIR-V
    FlipAcquire,    ///< the flip waits for a free presenter frame
    CopyFallback,   ///< a queued timeline copy done by the CPU (thread 2)
    // the present thread (VideoOut)
    VblankWait,     ///< sleeps until the next (software) vblank
    NotReady,       ///< instant: a flip was due but its GPU work had not completed (a = its timeline value)
    Present,        ///< the presentation of one flip (a = flip id)
    PresentFence,   ///< the swapchain slot's previous submission waited for
    PresentAcquire, ///< vkAcquireNextImageKHR
    PresentSubmit,  ///< vkQueueSubmit2 + vkQueuePresentKHR (the lock wait excluded)
    // the game's threads (sceGnm*)
    GameSubmitLock, ///< a submission waits for the driver's submit lock
    GameIdleWait,   ///< a submission waits until no SubmitDone holds the submission lock
    GameSubmitFlip, ///< sceGnmSubmitAndFlipCommandBuffers from entry to return
    // P5.4 frame lifecycle (SSPC_P54_FRAME_TRACE=1, engine/render/frame_trace.h): the game's frame and its waits
    GameSubmitFrame, ///< the engine's SubmitFrame 0x1f1e30 from entry to return (a = the frame's fence after it)
    GameComputeWait, ///< the engine's wait 0x1695e0 for its async compute queue's label (a = label address,
                     ///< b = the value waited for, c = 1: it had to wait)
    GameSubmitDone,  ///< sceGnmSubmitDone from entry to return
    GameDingDong,    ///< instant: sceGnmDingDong (a = vqid, b = the new write offset in dwords)
    // the command processor's fences (EOP / EOS / RELEASE_MEM / ...), on the thread that writes them
    FenceHeld,       ///< instant: a fence waits for the GPU work before it (a = address, b = value, c = queue)
    FenceWrite,      ///< instant: a fence written to guest memory (a = address, b = value low 32 bits, c = queue)
    GameEarlySubmit, ///< P5.4 SSPC_P54_EARLY_SUBMIT: the frame handed on inside sceGnmSubmitDone (a = DCBs, b = buffer)
    CpQueueSlice,    ///< P5.4: thread 1 steps one queue in a round (a = queue, b = packets, c = how it stopped: 0 nothing
                     ///< left, 1 blocked, 2 the round's cap, 3 faulted)
    Count,
};
constexpr size_t kEvents = static_cast<size_t>(Ev::Count);
const char* EvName(Ev e);
const char* RoleName(Role r);

namespace detail {
extern std::atomic<u32> g_mode; ///< bits 0-1: 0 off, 1 light, 2 deep; kForward: waits forwarded (P4.14)
void Record(Ev e, u64 t0, u64 t1, u64 a, u32 b, u16 c);
} // namespace detail

/// The trace runs (light or deep).
inline bool On() {
    return detail::g_mode.load(std::memory_order_relaxed) != 0;
}
/// Deep: events and samples are kept (inside the window).
bool Deep();
/// Deep with a span per record of thread 2.
bool DeepRecords();

inline u64 Now() {
    return __rdtsc();
}
/// A span of this thread's (its log is made at its first event).
inline void Span(Ev e, u64 t0, u64 t1, u64 a = 0, u32 b = 0, u16 c = 0) {
    if (On()) {
        detail::Record(e, t0, t1, a, b, c);
    }
}
/// A span of `ns` nanoseconds that ends now (a step timed with another clock).
void SpanEndingNow(Ev e, u64 ns, u64 a = 0);
inline void Instant(Ev e, u64 a = 0, u32 b = 0, u16 c = 0) {
    if (On()) {
        const u64 t = Now();
        detail::Record(e, t, t, a, b, c);
    }
}
/// RAII span (nothing when off).
class Scope {
public:
    explicit Scope(Ev e, u64 a = 0, u32 b = 0, u16 c = 0) : e_(e), a_(a), b_(b), c_(c), t0_(On() ? Now() : 0) {}
    ~Scope() {
        if (t0_ != 0) {
            detail::Record(e_, t0_, Now(), a_, b_, c_);
        }
    }
    void SetA(u64 a) { a_ = a; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Ev e_;
    u64 a_;
    u32 b_;
    u16 c_;
    u64 t0_;
};

/// This thread's role (before its first event; a thread that never calls it is Game if its first event is a Game*
/// one, else Other). Threads 1 and 2 are sampled in deep mode.
void RegisterThread(Role role);

/// The GPU's batch with timeline value `value` completed: its submission (TSC before the lock, after the call) and its
/// GPU start/end (the device's timestamp ticks; 0 when not timed), the frame it was recorded in (flip markers then).
void GpuBatch(u64 value, u64 submit_t0, u64 submit_t1, u64 gpu_t0, u64 gpu_t1, u32 frame);

/// P4.14: the waits go to the shader stall trace too (shader_trace.h; its SSPC_SHADER_STALL_TRACE), whether or not this
/// trace runs. Before the backend's threads.
constexpr u32 kForward = 4;
void EnableForward();

/// Reads SSPC_P412_STALL_TRACE; false when not set. Before the backend's threads. `timestamp_period`: ns per GPU tick.
bool Start(const std::filesystem::path& logs_dir, double timestamp_period);
/// The device's calibrated timestamps (VK_EXT/KHR_calibrated_timestamps): one (GPU ticks, QPC) pair; false: failed.
using CalibrateFn = bool (*)(void* ctx, u64& gpu_ticks, u64& qpc, u64& max_deviation_ns);
void SetCalibration(CalibrateFn fn, void* ctx);
/// The polling thread, now and then: a calibration pair when due (deep, inside the window).
void MaybeCalibrate();
/// The command processor reached flip marker number `n` (0-based): its events from now on count to frame n + 1.
void CpFrame(u32 n);
/// The frame the command processor works on (flip markers reached so far).
u32 CurrentFrame();
/// Stops recording and writes the files. After the backend's threads stopped.
void Stop();

} // namespace SecondSon::Gpu::StallTrace
