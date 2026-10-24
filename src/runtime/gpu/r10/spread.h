// SecondSonPC R10 — the busiest threads spread over the PC's physical cores (docs/R10_CPU_GPU_SCHEDULER.md,
// "Распределение по ядрам"). EXPERIMENT, off by default: SSPC_R10_SPREAD=1.
//
// Measured (ETW context switches, city fight, Ryzen 5 5500 6C/12T): six threads run most of the time - the game's main
// thread and SecondSonPC's command processor, recording, frontend, emitter threads (75..98 % of one CPU each, much of it
// spin-waiting) and the present thread; Windows puts two of them on the two SMT siblings of one core for 24..30 % of
// their run time. SSPC_R10_SPREAD=1 gives each of them a different physical core as its ideal processor
// (SetThreadIdealProcessorEx: a preference of the scheduler, no pinning - the thread still runs anywhere when that CPU
// is busy). The guest's own CPU masks stay not applied (runtime/kernel/thread.cpp).
#pragma once

#include "base/abi.h"

namespace SecondSon::Gpu::R10 {

enum class Heavy : u32 { GameMain = 0, Cp = 1, Record = 2, Frontend = 3, Emit = 4, Present = 5 };

/// The calling thread is `h`: with SSPC_R10_SPREAD=1 its ideal processor becomes the first logical CPU of physical core
/// (index of h) modulo the cores. Otherwise nothing.
void SpreadThisThread(Heavy h);

} // namespace SecondSon::Gpu::R10
