// SecondSonPC native runtime - job system (fixed worker pool).
//
// Minimal by design: the first consumer is parallel asset decoding at load time. Jobs are plain
// std::function; ParallelFor splits [0, count) into chunks and blocks the caller until done, with
// the caller thread also executing chunks.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/types.h"

namespace sspc {

class JobSystem {
public:
    explicit JobSystem(u32 worker_count = 0);  // 0 = hardware threads - 1 (at least 1)
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void Submit(std::function<void()> job);
    void ParallelFor(u32 count, const std::function<void(u32 index)>& body);
    void WaitIdle();
    u32 WorkerCount() const { return static_cast<u32>(workers_.size()); }

private:
    void WorkerMain();
    bool RunOne(std::unique_lock<std::mutex>& lock);

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    u32 active_ = 0;
    bool stop_ = false;
};

}  // namespace sspc
