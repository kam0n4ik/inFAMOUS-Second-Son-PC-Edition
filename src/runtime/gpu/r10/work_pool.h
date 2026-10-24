// SecondSonPC R10 — the native work pool (docs/R10_CPU_GPU_SCHEDULER.md §4; SSPC_R10_SCHED=1, off by default).
//
// One bounded pool of worker threads for the backend's background work, in place of a fixed set of threads per job
// kind (SSPC_ASYNC_PIPELINES' 3 graphics pipeline threads, SSPC_PIPE_PREFETCH's 2..6 compute pipeline threads) that
// otherwise all compile at once when a burst of new pipelines comes (an effect met for the first time) and compete with
// the game's threads, the command processor and the recording thread for the 6 cores:
//   * N workers (SSPC_R10_SCHED_WORKERS, default the logical processors less 4 within 4..8; 1..12), below normal priority - they never take a core from the
//     frame's pipeline threads; an idle worker sleeps (no spinning)
//   * each worker owns a deque per priority; a submission from outside goes to the next worker round-robin, a task
//     submitted by a worker to its own deque (cache-warm). A worker takes from the front of its own deques, highest
//     priority first, then steals from the back of the other workers' (the oldest first: the burst's start)
//   * priorities: High (work a frame will wait for: a compute pipeline its dispatch is recorded with), Normal (work a
//     frame skips until ready: a graphics pipeline under SSPC_ASYNC_PIPELINES), Low (speculative)
//   * backpressure: at most SSPC_R10_SCHED_PENDING (256) tasks waiting per priority; Submit returns false beyond it and
//     the caller keeps its own fallback - never an unbounded queue. (The pipeline factory submits drainer tasks - at most
//     one a worker per kind - so its jobs are never refused; dropping a refused graphics job left draws skipped on a
//     cold cache in the first version.)
//   * dependencies: a task may name tasks it waits for (Then); it is queued when the last of them completed
//   * each worker has a scratch arena (Scratch: 256 KiB, reset before each task) for the task's temporaries
// Locks: one small mutex per worker deque set (taken for a push/pop/steal, never while a task runs) and the sleepers'
// atomic; a task never runs under a lock of the pool.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

#include "base/abi.h"

namespace SecondSon::Gpu::R10 {

enum class Prio : u8 { High = 0, Normal = 1, Low = 2, Count = 3 };

/// A worker's scratch memory: a bump arena reset before each task (allocations do not outlive the task).
class Scratch {
public:
    void* Alloc(size_t bytes, size_t align = 16);
    void Reset() { used_ = 0; }
    size_t Capacity() const { return cap_; }

private:
    friend class WorkPool;
    std::unique_ptr<u8[]> mem_;
    size_t cap_ = 0, used_ = 0;
};

using TaskFn = std::function<void(Scratch&)>;

/// A submitted task's handle (dependencies, completion).
struct Task;
using TaskRef = std::shared_ptr<Task>;

class WorkPool {
public:
    /// The pool when SSPC_R10_SCHED=1 (made at the first call, any thread), nullptr otherwise.
    static WorkPool* Get();

    /// Queues `fn` (class `cls`: a number of the caller's, for the trace and the statistics). nullptr: refused
    /// (the pool is stopping, or `prio`'s queue is full - backpressure).
    TaskRef Submit(Prio prio, u32 cls, TaskFn fn);
    /// The same, queued once every task of `after` completed (a refused or finished one counts as completed).
    TaskRef Then(std::initializer_list<TaskRef> after, Prio prio, u32 cls, TaskFn fn);
    /// Whether `t` completed (ran or was cancelled).
    static bool Done(const TaskRef& t);

    u32 Workers() const { return workers_; }

    struct Stats {
        u64 submitted = 0, refused = 0, ran = 0, stolen = 0, sleeps = 0;
        u64 busy_ns = 0;      ///< the workers' time in tasks
        u64 max_pending = 0;  ///< the most tasks waiting at once
    };
    Stats GetStats() const;

    /// Stops the workers (queued tasks are run first). The process's end.
    void Stop();

    WorkPool(u32 workers, u32 max_pending);
    ~WorkPool();
    WorkPool(const WorkPool&) = delete;
    WorkPool& operator=(const WorkPool&) = delete;

private:
    struct Worker;
    void Run(u32 index);
    void Push(const TaskRef& t, int self);
    TaskRef Pop(u32 index, bool& stolen);
    void Finish(const TaskRef& t);

    u32 workers_;
    u32 max_pending_;
    std::unique_ptr<Worker[]> w_;
    std::atomic<u32> rr_{0};
    std::atomic<u64> epoch_{0}; ///< bumped by every push (the sleepers wait on it)
    std::atomic<u32> sleepers_{0};
    std::atomic<u32> pending_[static_cast<int>(Prio::Count)]{};
    std::atomic<bool> stop_{false};
    std::atomic<u64> submitted_{0}, refused_{0}, ran_{0}, stolen_{0}, sleeps_{0}, busy_ns_{0}, max_pending_seen_{0};
};

} // namespace SecondSon::Gpu::R10
