// SecondSonPC — write tracking of guest pages for the native GPU backend (P4.4). See page_tracker.h.
#include <windows.h>

#include "runtime/gpu/page_tracker.h"

#include "runtime/gpu/gpu_memory.h"

namespace SecondSon::Gpu {

namespace {

PageTracker* g_tracker = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT; // one tracker per process

bool Fault(u64 address, bool is_write) {
    PageTracker* t = g_tracker;
    return t != nullptr && t->OnFault(address, is_write);
}

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

} // namespace

bool PageTracker::Start(GuestMemory& memory, std::string& error) {
    if (g_tracker != nullptr) {
        error = "the page tracker is already started";
        return false;
    }
    const u64 bytes = (kPages + kBlocks + kWrittenWords + kBlocks + kBlocks) / 8;
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr) {
        error = "the page tracker's bitmaps could not be allocated";
        return false;
    }
    armed_ = static_cast<std::atomic<u64>*>(p);
    written_ = armed_ + kPages / 64;
    summary_ = written_ + kWrittenWords;
    never_ = summary_ + kSummaryWords;
    hidden_ = never_ + kBlocks / 64;
    memory_ = &memory;
    if (char v[64] = {}; GetEnvironmentVariableA("SSPC_DIAG_READ_WATCH", v, sizeof(v)) > 0) {
        char* p = v;
        watch_begin_ = std::strtoull(p, &p, 16);
        watch_end_ = *p == ':' ? watch_begin_ + std::strtoull(p + 1, nullptr, 16) : watch_begin_;
    }
    g_tracker = this;
    InstallTrackingFaultHandler(&Fault);
    return true;
}

void PageTracker::MarkWritten(u64 block) {
    const u64 w = block / 64;
    written_[w].fetch_or(1ull << (block % 64), std::memory_order_acq_rel);
    summary_[w / 64].fetch_or(1ull << (w % 64), std::memory_order_acq_rel);
    any_.store(true, std::memory_order_release);
}

u64 PageTracker::Release(u64 first, u64 last, bool protect) {
    u64 released = 0;
    u64 run = ~0ull;
    for (u64 page = first; page <= last; ++page) {
        const bool armed = page < last && Armed(page);
        if (armed) {
            if (run == ~0ull) {
                run = page;
            }
            armed_[page / 64].fetch_and(~(1ull << (page % 64)), std::memory_order_acq_rel);
            MarkWritten(page >> (kBlockBits - kPageBits));
            ++released;
        } else if (run != ~0ull) {
            if (protect) {
                memory_->ProtectTracking(run << kPageBits, (page - run) << kPageBits, TrackingProtection::ReadWrite);
            }
            run = ~0ull;
        }
    }
    armed_pages_.fetch_sub(released, std::memory_order_relaxed);
    // (FB-7: a hidden block given back is shown - Forget of unmapped memory, Disarm: the buffer cache sees it no longer
    // hidden and drops what it kept back)
    constexpr u64 kShift = kBlockBits - kPageBits;
    if (first < last) {
        for (u64 block = first >> kShift; block <= (last - 1) >> kShift; ++block) {
            hidden_[block / 64].fetch_and(~(1ull << (block % 64)), std::memory_order_acq_rel);
        }
    }
    return released;
}

bool PageTracker::Hide(u64 block_addr) {
    if (memory_ == nullptr || block_addr >= (1ull << kAddressBits)) {
        return false;
    }
    const u64 block = block_addr >> kBlockBits;
    const u64 first = block << (kBlockBits - kPageBits), n = 1ull << (kBlockBits - kPageBits);
    Lock lock;
    if (IsHidden(block)) {
        return true;
    }
    for (u64 p = first; p < first + n; ++p) {
        if (!Armed(p)) {
            return false; // (a page the CPU wrote since: its write-back stays eager)
        }
    }
    hidden_[block / 64].fetch_or(1ull << (block % 64), std::memory_order_acq_rel);
    ever_hidden_.store(true, std::memory_order_relaxed);
    memory_->ProtectTracking(block << kBlockBits, 1ull << kBlockBits, TrackingProtection::None);
    return true;
}

void PageTracker::Unhide(u64 block_addr) {
    if (memory_ == nullptr || block_addr >= (1ull << kAddressBits)) {
        return;
    }
    const u64 block = block_addr >> kBlockBits;
    const u64 first = block << (kBlockBits - kPageBits), n = 1ull << (kBlockBits - kPageBits);
    Lock lock;
    if (!IsHidden(block)) {
        return;
    }
    hidden_[block / 64].fetch_and(~(1ull << (block % 64)), std::memory_order_acq_rel);
    // armed pages read-only again, run by run (a page given back meanwhile stays)
    u64 run = ~0ull;
    for (u64 p = first; p <= first + n; ++p) {
        const bool armed = p < first + n && Armed(p);
        if (armed && run == ~0ull) {
            run = p;
        } else if (!armed && run != ~0ull) {
            memory_->ProtectTracking(run << kPageBits, (p - run) << kPageBits, TrackingProtection::Read);
            run = ~0ull;
        }
    }
}

bool PageTracker::Hidden(u64 addr, u64 size) const {
    if (hidden_ == nullptr || size == 0 || addr >= (1ull << kAddressBits)) {
        return false;
    }
    const u64 first = addr >> kBlockBits, last = (std::min(addr + size, 1ull << kAddressBits) - 1) >> kBlockBits;
    for (u64 w = first / 64; w <= last / 64; ++w) {
        u64 bits = hidden_[w].load(std::memory_order_acquire);
        if (bits == 0) {
            continue;
        }
        const u64 lo = w == first / 64 ? first % 64 : 0, hi = w == last / 64 ? last % 64 : 63;
        bits >>= lo;
        bits &= hi - lo == 63 ? ~0ull : ((1ull << (hi - lo + 1)) - 1);
        if (bits != 0) {
            return true;
        }
    }
    return false;
}

void PageTracker::Arm(u64 addr, u64 size) {
    if (memory_ == nullptr || size == 0 || !GuestMemory::IsValidGpuMapping(addr, size)) {
        return;
    }
    const u64 first = addr >> kPageBits;
    const u64 last = (addr + size + (1ull << kPageBits) - 1) >> kPageBits;
    Lock lock;
    u64 armed = 0;
    u64 run = ~0ull;
    for (u64 page = first; page <= last; ++page) {
        const u64 block = page >> (kBlockBits - kPageBits);
        const bool need = page < last && !Armed(page) &&
                          ((never_[block / 64].load(std::memory_order_acquire) >> (block % 64)) & 1) == 0;
        // (a run ends where a watched range or a hidden block starts or ends: those pages get no access)
        const auto no_access = [&](u64 p) { return Watched(p) || IsHidden(p >> (kBlockBits - kPageBits)); };
        const bool split = run != ~0ull && need && no_access(page) != no_access(run);
        if (run != ~0ull && (!need || split)) {
            memory_->ProtectTracking(run << kPageBits, (page - run) << kPageBits,
                                     no_access(run) ? TrackingProtection::None : TrackingProtection::Read);
            run = ~0ull;
        }
        if (need) {
            if (run == ~0ull) {
                run = page;
            }
            // the bit before the protection: a fault can only follow the protection
            armed_[page / 64].fetch_or(1ull << (page % 64), std::memory_order_acq_rel);
            ++armed;
        }
    }
    if (armed != 0) {
        armed_pages_.fetch_add(armed, std::memory_order_relaxed);
        arms_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PageTracker::Disarm(u64 addr, u64 size) {
    if (memory_ == nullptr || size == 0 || !GuestMemory::IsValidGpuMapping(addr, size)) {
        return;
    }
    const u64 first = addr >> kPageBits;
    const u64 last = (addr + size + (1ull << kPageBits) - 1) >> kPageBits;
    Lock lock;
    if (Release(first, last, true) != 0) {
        disarms_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool PageTracker::CanArm(u64 addr, u64 size) const {
    if (memory_ == nullptr || size == 0 || !GuestMemory::IsValidGpuMapping(addr, size)) {
        return false;
    }
    // the bitmap a word (64 blocks) at a time
    const u64 first = addr >> kBlockBits, last = (addr + size - 1) >> kBlockBits;
    for (u64 w = first / 64; w <= last / 64; ++w) {
        u64 bits = never_[w].load(std::memory_order_acquire);
        if (bits == 0) {
            continue;
        }
        const u64 lo = w == first / 64 ? first % 64 : 0, hi = w == last / 64 ? last % 64 : 63;
        bits >>= lo;
        bits &= hi - lo == 63 ? ~0ull : ((1ull << (hi - lo + 1)) - 1);
        if (bits != 0) {
            return false;
        }
    }
    return true;
}

void PageTracker::NeverArm(u64 addr, u64 size) {
    if (memory_ == nullptr || size == 0 || !GuestMemory::IsValidGpuMapping(addr, size)) {
        return;
    }
    const u64 first = addr >> kPageBits;
    const u64 last = (addr + size + (1ull << kPageBits) - 1) >> kPageBits;
    constexpr u64 kShift = kBlockBits - kPageBits;
    Lock lock;
    for (u64 block = first >> kShift; block <= (last - 1) >> kShift; ++block) {
        const u64 bit = 1ull << (block % 64);
        if ((never_[block / 64].fetch_or(bit, std::memory_order_acq_rel) & bit) == 0) {
            never_blocks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Release(first, last, true);
}

void PageTracker::Forget(u64 addr, u64 size) {
    if (memory_ == nullptr || size == 0) {
        return;
    }
    const u64 end = std::min<u64>(addr + size, 1ull << kAddressBits);
    if (addr >= end) {
        return;
    }
    const u64 first = addr >> kPageBits;
    const u64 last = (end + (1ull << kPageBits) - 1) >> kPageBits;
    Lock lock;
    // given back too: a page left write-protected without its bit would fault with no owner (the range may stay
    // mapped - a notification for a mapping that keeps its pages); no effect on pages being unmapped
    forgotten_.fetch_add(Release(first, last, true), std::memory_order_relaxed);
}

bool PageTracker::OnFault(u64 address, bool is_write) {
    if (memory_ == nullptr || address >= (1ull << kAddressBits)) {
        return false;
    }
    const u64 page = address >> kPageBits;
    if (hidden_ != nullptr && IsHidden(page >> (kBlockBits - kPageBits))) {
        // FB-7: the GPU's copy is written back first (no lock held: the GPU thread arms pages meanwhile), then shown
        hidden_faults_.fetch_add(1, std::memory_order_relaxed);
        const u64 block_addr = (page >> (kBlockBits - kPageBits)) << kBlockBits;
        if (const HiddenSync fn = hidden_sync_.load(std::memory_order_acquire)) {
            fn(hidden_sync_ctx_, block_addr);
        }
        Unhide(block_addr);
        return true; // (the access retries: a write faults again on the read-only page - the ordinary path)
    }
    if (!is_write) {
        // SSPC_DIAG_READ_WATCH: a read of a watched armed page - counted, the page read-only again (still armed)
        if (watch_end_ == 0 || !Watched(page)) {
            if (!ever_hidden_.load(std::memory_order_relaxed)) {
                return false;
            }
            // FB-7 (R12): an access that faulted on a hidden block another thread showed meanwhile - its Unhide clears
            // the bit before it gives the pages back, both under the lock: under it the block is hidden again (the
            // access retries and faults into the path above) or readable now (it retries and reads). Without this
            // the guest died here ("nobody resolved", neon: 2 of 11 runs with SSPC_FB_LAZY_WB=1).
            Lock lock;
            if (IsHidden(page >> (kBlockBits - kPageBits))) {
                hidden_raced_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
                (mbi.Protect & PAGE_GUARD) == 0 &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0) {
                hidden_raced_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
        Lock lock;
        if (Armed(page)) {
            if (watch_reads_.fetch_add(1, std::memory_order_relaxed) == 0) {
                watch_first_.store(address, std::memory_order_relaxed);
            }
            watch_last_.store(address, std::memory_order_relaxed);
            memory_->ProtectTracking(page << kPageBits, 1ull << kPageBits, TrackingProtection::Read);
            return true;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
               (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE)) != 0; // (given back meanwhile: retry)
    }
    const u64 per_block = 1ull << (kBlockBits - kPageBits);
    const u64 first = page & ~(per_block - 1);
    // Under the lock: a Release on another thread (another fault in the same block, a Disarm) clears the bits before it
    // gives the pages back; checking only after it finished, the page is either still armed or writable again.
    Lock lock;
    if (Armed(page) && Release(first, first + per_block, true) != 0) {
        faults_.fetch_add(1, std::memory_order_relaxed);
        fault_addr_[fault_head_++ % kFaultRing] = address; // no allocation: a fixed ring under the lock
        if (fault_head_ - fault_tail_ > kFaultRing) {
            fault_tail_ = fault_head_ - kFaultRing;
        }
        return true;
    }
    // given back by another thread between the fault and this check: the write retries
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
        (mbi.Protect & PAGE_GUARD) == 0 && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0) {
        faults_raced_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void PageTracker::TakeFaultAddresses(std::vector<u64>& out) {
    Lock lock;
    for (; fault_tail_ < fault_head_; ++fault_tail_) {
        out.push_back(fault_addr_[fault_tail_ % kFaultRing]);
    }
}

PageTracker::Stats PageTracker::GetStats() const {
    Stats s;
    s.armed_pages = armed_pages_.load(std::memory_order_relaxed);
    s.arms = arms_.load(std::memory_order_relaxed);
    s.disarms = disarms_.load(std::memory_order_relaxed);
    s.faults = faults_.load(std::memory_order_relaxed);
    s.faults_raced = faults_raced_.load(std::memory_order_relaxed);
    s.forgotten = forgotten_.load(std::memory_order_relaxed);
    s.never_armed_blocks = never_blocks_.load(std::memory_order_relaxed);
    s.watch_reads = watch_reads_.load(std::memory_order_relaxed);
    s.watch_first = watch_first_.load(std::memory_order_relaxed);
    s.watch_last = watch_last_.load(std::memory_order_relaxed);
    return s;
}

} // namespace SecondSon::Gpu
