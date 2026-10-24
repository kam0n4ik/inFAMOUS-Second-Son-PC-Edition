// SecondSonPC R10 — the native work pool. See work_pool.h.
#include "runtime/gpu/r10/work_pool.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/log.h"
#include "runtime/gpu/stall_trace.h"

namespace SecondSon::Gpu::R10 {

struct Task {
    TaskFn fn;
    Prio prio = Prio::Normal;
    u32 cls = 0;
    std::atomic<u32> waiting{0}; ///< predecessors not completed yet (+1 while Then registers it)
    std::mutex m;                ///< done / next
    bool done = false;
    std::vector<TaskRef> next;   ///< the tasks waiting for this one
};

struct WorkPool::Worker {
    std::mutex m;
    std::deque<TaskRef> q[static_cast<int>(Prio::Count)];
    Scratch scratch;
    std::thread thread;
};

void* Scratch::Alloc(size_t bytes, size_t align) {
    const size_t at = (used_ + align - 1) & ~(align - 1);
    if (at + bytes > cap_) {
        return nullptr; // (the task's own allocation then)
    }
    used_ = at + bytes;
    return mem_.get() + at;
}

namespace {
constexpr size_t kScratchBytes = 256 << 10;
u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Max(std::atomic<u64>& a, u64 v) {
    u64 cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
thread_local int t_worker = -1; ///< this thread's index in the pool (-1: not a worker)
} // namespace

WorkPool* WorkPool::Get() {
    static WorkPool* const pool = []() -> WorkPool* {
        char v[8] = {};
        if (!(GetEnvironmentVariableA("SSPC_R10_SCHED", v, sizeof(v)) == 1 && v[0] == '1')) {
            return nullptr;
        }
        // default: the logical processors less 4 (the game's main thread and the backend's 3 busiest), 4..8 - as many
        // as the threads it replaces (3 async + up to 6 prefetch); a cold cache needs the driver's compile throughput
        u32 n = std::clamp(std::thread::hardware_concurrency(), 8u, 12u) - 4, pending = 256;
        if (char w[8] = {}; GetEnvironmentVariableA("SSPC_R10_SCHED_WORKERS", w, sizeof(w)) > 0) {
            n = std::clamp<u32>(static_cast<u32>(std::strtoul(w, nullptr, 10)), 1u, 12u);
        }
        if (char p[12] = {}; GetEnvironmentVariableA("SSPC_R10_SCHED_PENDING", p, sizeof(p)) > 0) {
            pending = std::clamp<u32>(static_cast<u32>(std::strtoul(p, nullptr, 10)), 8u, 1u << 16);
        }
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R10_SCHED=1 - the backend's background work (pipeline builds) runs on "
                               "one pool of {} worker threads (below normal priority, work stealing, at most {} tasks "
                               "waiting per priority)", n, pending);
        return new WorkPool(n, pending); // (never destroyed: the process ends with it - Stop() joins)
    }();
    return pool;
}

WorkPool::WorkPool(u32 workers, u32 max_pending) : workers_(workers), max_pending_(max_pending) {
    w_ = std::make_unique<Worker[]>(workers_);
    for (u32 i = 0; i < workers_; ++i) {
        w_[i].scratch.mem_ = std::make_unique<u8[]>(kScratchBytes);
        w_[i].scratch.cap_ = kScratchBytes;
    }
    for (u32 i = 0; i < workers_; ++i) {
        w_[i].thread = std::thread([this, i] { Run(i); });
    }
}

WorkPool::~WorkPool() {
    Stop();
}

void WorkPool::Stop() {
    if (stop_.exchange(true, std::memory_order_seq_cst)) {
        return;
    }
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    epoch_.notify_all();
    for (u32 i = 0; i < workers_; ++i) {
        if (w_[i].thread.joinable()) {
            w_[i].thread.join();
        }
    }
}

TaskRef WorkPool::Submit(Prio prio, u32 cls, TaskFn fn) {
    return Then({}, prio, cls, std::move(fn));
}

TaskRef WorkPool::Then(std::initializer_list<TaskRef> after, Prio prio, u32 cls, TaskFn fn) {
    const int p = static_cast<int>(prio);
    if (stop_.load(std::memory_order_acquire) || pending_[p].load(std::memory_order_relaxed) >= max_pending_) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    auto t = std::make_shared<Task>();
    t->fn = std::move(fn);
    t->prio = prio;
    t->cls = cls;
    t->waiting.store(1, std::memory_order_relaxed); // (the registration's own count)
    for (const TaskRef& a : after) {
        if (!a) {
            continue;
        }
        std::scoped_lock lock{a->m};
        if (!a->done) {
            t->waiting.fetch_add(1, std::memory_order_relaxed);
            a->next.push_back(t);
        }
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);
    if (t->waiting.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        Push(t, t_worker);
    }
    return t;
}

bool WorkPool::Done(const TaskRef& t) {
    if (!t) {
        return true;
    }
    std::scoped_lock lock{t->m};
    return t->done;
}

void WorkPool::Push(const TaskRef& t, int self) {
    const int p = static_cast<int>(t->prio);
    const u32 i = self >= 0 && static_cast<u32>(self) < workers_ ? static_cast<u32>(self)
                                                                   : rr_.fetch_add(1, std::memory_order_relaxed) % workers_;
    // (counted before it can be taken: a Pop's decrement never comes first, the count never wraps below 0)
    Max(max_pending_seen_, pending_[p].fetch_add(1, std::memory_order_relaxed) + 1);
    {
        std::scoped_lock lock{w_[i].m};
        w_[i].q[p].push_back(t);
    }
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_seq_cst) != 0) {
        epoch_.notify_one();
    }
}

TaskRef WorkPool::Pop(u32 index, bool& stolen) {
    stolen = false;
    for (int p = 0; p < static_cast<int>(Prio::Count); ++p) {
        {
            Worker& own = w_[index];
            std::scoped_lock lock{own.m};
            if (!own.q[p].empty()) {
                TaskRef t = std::move(own.q[p].front());
                own.q[p].pop_front();
                pending_[p].fetch_sub(1, std::memory_order_relaxed);
                return t;
            }
        }
        for (u32 k = 1; k < workers_; ++k) {
            Worker& other = w_[(index + k) % workers_];
            std::scoped_lock lock{other.m};
            if (!other.q[p].empty()) {
                TaskRef t = std::move(other.q[p].back());
                other.q[p].pop_back();
                pending_[p].fetch_sub(1, std::memory_order_relaxed);
                stolen = true;
                return t;
            }
        }
    }
    return nullptr;
}

void WorkPool::Finish(const TaskRef& t) {
    std::vector<TaskRef> next;
    {
        std::scoped_lock lock{t->m};
        t->done = true;
        next.swap(t->next);
    }
    t->fn = nullptr; // (what it captured released now, not when the last handle goes)
    for (const TaskRef& n : next) {
        if (n->waiting.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Push(n, t_worker);
        }
    }
}

void WorkPool::Run(u32 index) {
    SetThreadDescription(GetCurrentThread(), (L"SecondSonPC:R10Worker" + std::to_wstring(index)).c_str());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    t_worker = static_cast<int>(index);
    bool trace_role = false; // (registered once the trace is on: the pool may start before it)
    Worker& me = w_[index];
    for (;;) {
        bool stolen = false;
        TaskRef t = Pop(index, stolen);
        if (!t) {
            const u64 e = epoch_.load(std::memory_order_seq_cst);
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            t = Pop(index, stolen);
            if (!t) {
                if (stop_.load(std::memory_order_seq_cst)) {
                    sleepers_.fetch_sub(1, std::memory_order_seq_cst);
                    return; // (stopping and nothing left: every queue was seen empty after the stop)
                }
                sleeps_.fetch_add(1, std::memory_order_relaxed);
                epoch_.wait(e, std::memory_order_seq_cst);
            }
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
            if (!t) {
                continue;
            }
        }
        if (stolen) {
            stolen_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!trace_role && StallTrace::On()) {
            StallTrace::RegisterThread(StallTrace::Role::Worker);
            trace_role = true;
        }
        me.scratch.Reset();
        const u64 t0 = NowNs();
        {
            const StallTrace::Scope span(StallTrace::Ev::SchedTask, t->cls, 0, static_cast<u16>(t->prio));
            if (t->fn) {
                t->fn(me.scratch);
            }
        }
        busy_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
        ran_.fetch_add(1, std::memory_order_relaxed);
        Finish(t);
    }
}

WorkPool::Stats WorkPool::GetStats() const {
    Stats s;
    s.submitted = submitted_.load(std::memory_order_relaxed);
    s.refused = refused_.load(std::memory_order_relaxed);
    s.ran = ran_.load(std::memory_order_relaxed);
    s.stolen = stolen_.load(std::memory_order_relaxed);
    s.sleeps = sleeps_.load(std::memory_order_relaxed);
    s.busy_ns = busy_ns_.load(std::memory_order_relaxed);
    s.max_pending = max_pending_seen_.load(std::memory_order_relaxed);
    return s;
}

} // namespace SecondSon::Gpu::R10
