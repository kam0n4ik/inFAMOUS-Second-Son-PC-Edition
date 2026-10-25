// SecondSonPC R11 - the game's main thread sleeps on the label it waits for (see label_wait.h).
#include "runtime/gpu/r11/label_wait.h"

#include <intrin.h>
#include <windows.h>

#include "runtime/kernel/clock.h"

#pragma comment(lib, "Synchronization.lib")

namespace SecondSon::Gpu::LabelWait {

namespace {
std::atomic<u64> g_sleeps{0}, g_spins_met{0}, g_timeouts{0};

u32 Load(u64 label) {
    return *reinterpret_cast<const volatile u32*>(label);
}
} // namespace

void OnWrite(u64 addr, u64 bytes) {
    std::atomic_thread_fence(std::memory_order_seq_cst); // (the label's store before the read of the waiter's slot)
    const u64 w = g_waiter.load(std::memory_order_relaxed);
    if (w != 0 && w >= addr && w < addr + bytes) {
        WakeByAddressAll(reinterpret_cast<void*>(w));
        g_wakes.fetch_add(1, std::memory_order_relaxed);
    }
}

void Sleep(u64 label, u32 target, u32 spin_us) {
    static const u64 ticks_per_us = [] {
        const u64 f = Kernel::Clock::TscFrequency();
        return f >= 1'000'000 ? f / 1'000'000 : 1;
    }();
    if (spin_us != 0) {
        const u64 t0 = __rdtsc(), span = u64{spin_us} * ticks_per_us;
        do {
            if (Load(label) >= target) {
                g_spins_met.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            _mm_pause();
        } while (__rdtsc() - t0 < span);
    }
    g_waiter.store(label, std::memory_order_seq_cst); // (published before the label is compared)
    const u32 seen = Load(label);
    if (seen < target) {
        g_sleeps.fetch_add(1, std::memory_order_relaxed);
        u32 compare = seen;
        // returns when the label differs from `seen`, a fence write woke it or 1 ms passed (the caller loops)
        if (!WaitOnAddress(reinterpret_cast<volatile void*>(label), &compare, sizeof(compare), 1) &&
            GetLastError() == ERROR_TIMEOUT && Load(label) == seen) {
            g_timeouts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_waiter.store(0, std::memory_order_relaxed);
}

Totals Read() {
    return {g_sleeps.load(std::memory_order_relaxed), g_spins_met.load(std::memory_order_relaxed),
            g_timeouts.load(std::memory_order_relaxed), g_wakes.load(std::memory_order_relaxed)};
}

} // namespace SecondSon::Gpu::LabelWait
