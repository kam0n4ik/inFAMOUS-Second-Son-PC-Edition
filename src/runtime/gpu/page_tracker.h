// SecondSonPC — write tracking of the guest pages SecondSonPC's GPU backend holds copies of (P4.4,
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §E). A texture uploaded from guest memory has its pages write-protected
// (GuestMemory::ProtectTracking, Read); the guest's first write there faults, the handler gives the 64 KiB block back
// (ReadWrite) and records it as written; the GPU thread drains the written blocks and invalidates its caches before it
// executes more GPU work — the guest wrote before it submitted, so the next draw sees the new texels (the intro's video
// frames, streamed textures). The bridge's page manager does the same for its texture cache.
//
// Threads: Arm/Disarm/Drain on the GPU thread; Forget from the memory listener (any thread); OnFault on the faulting
// thread inside exception dispatch (no allocation, no logging: a slim reader/writer lock and atomics only).
#pragma once

#include <atomic>
#include <bit>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu {

class GuestMemory;

class PageTracker {
public:
    static constexpr u64 kPageBits = 12; ///< host pages (protection granularity)
    static constexpr u64 kBlockBits = 16; ///< what a fault gives back and reports (64 KiB)

    /// Reserves the bitmaps (36 MiB) and installs the fault handler. Once; false with `error`.
    bool Start(GuestMemory& memory, std::string& error);

    /// Write-protects [addr, addr + size) (whole pages; those already armed stay). GPU thread.
    void Arm(u64 addr, u64 size);
    /// Gives [addr, addr + size) back (ReadWrite) and reports the armed blocks there as written: before host code writes
    /// it or a Vulkan import needs it writable. GPU thread.
    void Disarm(u64 addr, u64 size);
    /// The range can be write-protected (started, not imported through the guest's own pages).
    bool CanArm(u64 addr, u64 size) const;
    /// [addr, addr + size) is imported into Vulkan through the guest's own pages: given back now and never armed again
    /// (NVIDIA loses the device on any access to imported memory whose protection changed). GPU thread.
    void NeverArm(u64 addr, u64 size);
    /// The pages of [addr, addr + size) are being unmapped or remapped: forgotten and given back (ReadWrite; no effect
    /// on pages already unmapped). Any thread.
    void Forget(u64 addr, u64 size);
    /// The faulting thread: true when a write faulted on an armed page (now writable again) — resume.
    bool OnFault(u64 address, bool is_write);

    // FrameBreak FB-7 (SSPC_FB_LAZY_WB): a hidden block's video memory copy is newer than the host's and was not written
    // back - its armed pages get no access on the guest's view (never on a Vulkan import), so any CPU access faults;
    // the fault calls the sync function (the GPU writes the block back, the call returns once that completed), then the
    // block is shown again (armed pages read-only) and the access retries.
    using HiddenSync = void (*)(void* ctx, u64 block_addr);
    void SetHiddenSync(HiddenSync fn, void* ctx) {
        hidden_sync_ctx_ = ctx;
        hidden_sync_ = fn;
    }
    /// Hides the 64 KiB block at `block_addr` when every page of it is armed (true). GPU thread.
    bool Hide(u64 block_addr);
    /// Shows it again (armed pages read-only). Any thread.
    void Unhide(u64 block_addr);
    /// Some block of [addr, addr + size) is hidden. Any thread.
    bool Hidden(u64 addr, u64 size) const;
    u64 HiddenFaults() const { return hidden_faults_.load(std::memory_order_relaxed); }
    /// Read faults on a block shown (or hidden again) by another thread between the fault and its check: retried
    u64 HiddenRaced() const { return hidden_raced_.load(std::memory_order_relaxed); }

    /// Calls `fn(addr, size)` for every range written since the last drain (adjacent blocks coalesced). GPU thread.
    template <class F>
    void Drain(F&& fn) {
        if (!any_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        u64 run_begin = 0, run_end = 0;
        for (u64 s = 0; s < kSummaryWords; ++s) {
            u64 sbits = summary_[s].exchange(0, std::memory_order_acq_rel);
            while (sbits != 0) {
                const u64 w = s * 64 + static_cast<u64>(std::countr_zero(sbits));
                sbits &= sbits - 1;
                u64 bits = written_[w].exchange(0, std::memory_order_acq_rel);
                while (bits != 0) {
                    const u64 block = w * 64 + static_cast<u64>(std::countr_zero(bits));
                    bits &= bits - 1;
                    const u64 b = block << kBlockBits, e = b + (1ull << kBlockBits);
                    if (b == run_end && run_end != 0) {
                        run_end = e;
                    } else {
                        if (run_end != 0) {
                            fn(run_begin, run_end - run_begin);
                        }
                        run_begin = b;
                        run_end = e;
                    }
                }
            }
        }
        if (run_end != 0) {
            fn(run_begin, run_end - run_begin);
        }
    }

    /// The faulting addresses since the last call - each the first write into a block given back - appended to `out`;
    /// at most kFaultRing kept (older ones dropped: the blocks are still reported by Drain). GPU thread (P4.5: a write
    /// surely inside a small GPU-written image, where a whole block says nothing).
    void TakeFaultAddresses(std::vector<u64>& out);

    struct Stats {
        u64 armed_pages = 0;  ///< currently write-protected
        u64 arms = 0, disarms = 0, faults = 0, faults_raced = 0, forgotten = 0, never_armed_blocks = 0;
        u64 watch_reads = 0, watch_first = 0, watch_last = 0; ///< SSPC_DIAG_READ_WATCH: CPU reads seen, first/last address
    };
    Stats GetStats() const;

private:
    static constexpr u64 kAddressBits = 40;
    static constexpr u64 kPages = 1ull << (kAddressBits - kPageBits);
    static constexpr u64 kBlocks = 1ull << (kAddressBits - kBlockBits);
    static constexpr u64 kWrittenWords = kBlocks / 64;
    static constexpr u64 kSummaryWords = kWrittenWords / 64;

    bool Armed(u64 page) const {
        return (armed_[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1;
    }
    void MarkWritten(u64 block);
    /// Clears the armed pages of [first, last) and gives them back; reports their blocks written. Lock held.
    u64 Release(u64 first, u64 last, bool protect);

    GuestMemory* memory_ = nullptr;
    std::atomic<u64>* armed_ = nullptr;   ///< one bit per page
    std::atomic<u64>* written_ = nullptr; ///< one bit per block
    std::atomic<u64>* summary_ = nullptr; ///< one bit per written_ word
    std::atomic<u64>* never_ = nullptr;   ///< one bit per block imported through the guest's pages
    std::atomic<u64>* hidden_ = nullptr;  ///< one bit per block (FB-7)
    bool IsHidden(u64 block) const {
        return (hidden_[block / 64].load(std::memory_order_acquire) >> (block % 64)) & 1;
    }
    std::atomic<HiddenSync> hidden_sync_{nullptr};
    void* hidden_sync_ctx_ = nullptr;
    std::atomic<u64> hidden_faults_{0};
    std::atomic<u64> hidden_raced_{0};
    std::atomic<bool> ever_hidden_{false}; ///< some block was hidden (only then a read fault can be FB-7's)
    std::atomic<bool> any_{false};
    std::atomic<u64> armed_pages_{0}, arms_{0}, disarms_{0}, faults_{0}, faults_raced_{0}, forgotten_{0};
    std::atomic<u64> never_blocks_{0};
    // SSPC_DIAG_READ_WATCH=<hex address>:<hex size> (FrameBreak FB-7, diagnostic): armed pages there get no access at
    // all (the guest's view only, never an import); a read faults, is counted, and gets the page back read-only
    u64 watch_begin_ = 0, watch_end_ = 0;
    std::atomic<u64> watch_reads_{0}, watch_first_{0}, watch_last_{0};
    bool Watched(u64 page) const {
        return (page << kPageBits) >= watch_begin_ && (page << kPageBits) < watch_end_;
    }
    static constexpr u64 kFaultRing = 4096;
    u64 fault_addr_[kFaultRing] = {}; ///< under the lock
    u64 fault_head_ = 0, fault_tail_ = 0;
};

} // namespace SecondSon::Gpu
