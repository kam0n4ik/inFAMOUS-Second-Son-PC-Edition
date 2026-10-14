// SecondSonPC native runtime - fixed-timestep simulation clock.
//
// Real frame time is accumulated and converted into a whole number of fixed simulation steps, so
// gameplay results do not depend on the rendering frame rate (30, 60, 144 Hz ...). Long stalls
// are clamped (max_steps) instead of spiralling; the dropped time is counted.
#pragma once

#include "core/types.h"

namespace sspc {

class FixedStepClock {
public:
    explicit FixedStepClock(double step_seconds = 1.0 / 60.0, u32 max_steps_per_frame = 8);

    // Adds real elapsed time and returns how many fixed steps to simulate now.
    u32 Advance(double elapsed_seconds);
    // Fraction of a step left in the accumulator (0..1), for render interpolation.
    double Alpha() const { return accumulator_ / step_; }
    double Step() const { return step_; }
    u64 TotalSteps() const { return total_steps_; }
    double DroppedSeconds() const { return dropped_; }

private:
    double step_;
    u32 max_steps_;
    double accumulator_ = 0;
    u64 total_steps_ = 0;
    double dropped_ = 0;
};

}  // namespace sspc
