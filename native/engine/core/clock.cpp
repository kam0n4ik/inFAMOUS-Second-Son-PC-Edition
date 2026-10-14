#include "core/clock.h"

#include <windows.h>

namespace sspc {
namespace {

u64 Frequency() {
    static const u64 f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return static_cast<u64>(li.QuadPart);
    }();
    return f;
}

}  // namespace

Clock::Clock() : start_(Ticks()) {}

double Clock::Now() const { return TicksToSeconds(Ticks() - start_); }

u64 Clock::Ticks() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<u64>(li.QuadPart);
}

double Clock::TicksToSeconds(u64 ticks) {
    return static_cast<double>(ticks) / static_cast<double>(Frequency());
}

}  // namespace sspc
