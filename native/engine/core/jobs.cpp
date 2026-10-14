#include "core/jobs.h"

#include <atomic>

namespace sspc {

JobSystem::JobSystem(u32 worker_count) {
    if (worker_count == 0) {
        const u32 hw = std::thread::hardware_concurrency();
        worker_count = hw > 1 ? hw - 1 : 1;
    }
    workers_.reserve(worker_count);
    for (u32 i = 0; i < worker_count; ++i) workers_.emplace_back([this] { WorkerMain(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void JobSystem::Submit(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(job));
    }
    work_cv_.notify_one();
}

bool JobSystem::RunOne(std::unique_lock<std::mutex>& lock) {
    if (queue_.empty()) return false;
    auto job = std::move(queue_.front());
    queue_.pop_front();
    ++active_;
    lock.unlock();
    job();
    lock.lock();
    --active_;
    if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
    return true;
}

void JobSystem::WorkerMain() {
    std::unique_lock lock(mutex_);
    for (;;) {
        work_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        RunOne(lock);
    }
}

void JobSystem::WaitIdle() {
    std::unique_lock lock(mutex_);
    while (RunOne(lock)) {
    }
    idle_cv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void JobSystem::ParallelFor(u32 count, const std::function<void(u32 index)>& body) {
    if (count == 0) return;
    std::atomic<u32> next{0};
    std::atomic<u32> done{0};
    const u32 tasks = std::min<u32>(count, WorkerCount() + 1);
    std::mutex m;
    std::condition_variable cv;
    auto worker = [&] {
        for (u32 i = next.fetch_add(1); i < count; i = next.fetch_add(1)) body(i);
        if (done.fetch_add(1) + 1 == tasks) {
            std::lock_guard lock(m);
            cv.notify_all();
        }
    };
    for (u32 t = 1; t < tasks; ++t) Submit(worker);
    worker();
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return done.load() == tasks; });
}

}  // namespace sspc
