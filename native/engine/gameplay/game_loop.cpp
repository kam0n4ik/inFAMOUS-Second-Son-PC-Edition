#include "gameplay/game_loop.h"

#include <algorithm>
#include <cmath>

namespace sspc {

FixedStepClock::FixedStepClock(double step_seconds, u32 max_steps_per_frame)
    : step_(step_seconds > 0 ? step_seconds : 1.0 / 60.0), max_steps_(std::max<u32>(1, max_steps_per_frame)) {}

u32 FixedStepClock::Advance(double elapsed_seconds) {
    if (elapsed_seconds < 0 || elapsed_seconds != elapsed_seconds) elapsed_seconds = 0;  // NaN/negative guard
    accumulator_ += elapsed_seconds;
    u32 steps = 0;
    // Small epsilon so that e.g. 2 x (1/120 s) reliably produces one 1/60 s step.
    const double eps = step_ * 1e-9;
    while (accumulator_ + eps >= step_ && steps < max_steps_) {
        accumulator_ -= step_;
        ++steps;
    }
    if (accumulator_ < 0) accumulator_ = 0;
    if (steps == max_steps_ && accumulator_ + eps >= step_) {
        // Stall longer than max_steps: drop the whole backlog instead of carrying a remainder that
        // floating-point rounding can leave just below one step.
        dropped_ += accumulator_;
        accumulator_ = 0;
    }
    total_steps_ += steps;
    return steps;
}

}  // namespace sspc
