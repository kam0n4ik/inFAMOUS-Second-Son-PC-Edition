// SecondSonPC native runtime - high resolution clock (QueryPerformanceCounter).
#pragma once

#include "core/types.h"

namespace sspc {

class Clock {
public:
    Clock();
    // Seconds since construction.
    double Now() const;
    static u64 Ticks();
    static double TicksToSeconds(u64 ticks);

private:
    u64 start_;
};

}  // namespace sspc
