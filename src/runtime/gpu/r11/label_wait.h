// SecondSonPC R11 - the game's main thread sleeps on the label it waits for instead of spinning (SSPC_R11_LABEL_WAIT=1,
// off by default; docs/R11_MAIN_THREAD.md).
//
// The engine waits for its async compute queue in 0x1695e0: `while (labels[q+0x1b8c] < q[+0x1b88]) delay(50)` with the
// game's delay(us) 0x53b2b0 an rdtsc spin without PAUSE - Game:Main is on the CPU all the time it waits (~80 % of its
// frame in a fight, docs/R9_HOT_FUNCTIONS.md §2), beside SecondSonPC's pipeline threads on the same cores. The labels are
// written by the command processor's fence writes (CommandProcessor::WriteFence, thread 1). With the flag the engine's
// replacement of 0x1695e0 (Engine::Render::Trace::ComputeWaitHook) spins a short while with PAUSE, then publishes the
// label's address here and sleeps on it (WaitOnAddress, 1 ms at most); the fence write that lands on a published address
// wakes it (WakeByAddressAll). Dekker order: the waiter publishes, then WaitOnAddress compares the label; the writer
// writes, then reads the published address - with a full fence on both sides one of them sees the other.
#pragma once

#include <atomic>

#include "base/abi.h"

namespace SecondSon::Gpu::LabelWait {

/// SSPC_R11_LABEL_WAIT=1 (read once by the engine's installer; the writer side reads this).
inline std::atomic<bool> g_on{false};
/// The label address a game thread sleeps on (0: none). One slot: only Game:Main waits for the compute queue.
inline std::atomic<u64> g_waiter{0};

/// Fence writes that woke a sleeping waiter (diagnostics).
inline std::atomic<u64> g_wakes{0};

/// The writer: `bytes` at `addr` just landed in guest memory.
void OnWrite(u64 addr, u64 bytes);

inline void Written(u64 addr, u64 bytes) {
    if (g_on.load(std::memory_order_relaxed)) {
        OnWrite(addr, bytes);
    }
}

/// The waiter: returns once the u32 at `label` is >= target, it changed, a wake came or about a millisecond passed (the
/// caller re-checks). Spins `spin_us` first.
void Sleep(u64 label, u32 target, u32 spin_us);

struct Totals {
    u64 sleeps, spins_met, timeouts, wakes;
};
Totals Read();

} // namespace SecondSon::Gpu::LabelWait
