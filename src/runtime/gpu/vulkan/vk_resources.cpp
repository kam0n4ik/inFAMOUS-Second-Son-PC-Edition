// SecondSonPC — the backend's timeline, stream ring and guest buffers (P4.4). See vk_resources.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_resources.h"

#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/page_tracker.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_device_context.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

constexpr u64 AlignUp(u64 v, u64 a) {
    return (v + a - 1) / a * a;
}
constexpr u64 AlignDown(u64 v, u64 a) {
    return v / a * a;
}

constexpr u64 kWindowAlign = 16ull << 20;
constexpr u64 kWindowSize = 32ull << 20;
constexpr u64 kHeapWindowMin = 256ull << 20; ///< R9-F3: a range this large is a heap binding (BufferCache::kHeapMin)
constexpr u64 kHeapHeadroom = 64ull << 20;   ///< R9-F3: imported past a heap binding's end where the pages allow

/// The bytes from p on (at most `want`) that are committed and writable: what an import of host memory accepts.
u64 CommittedWritable(const u8* p, u64 want) {
    u64 got = 0;
    while (got < want) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p + got, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
            break;
        }
        got = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize - reinterpret_cast<u64>(p);
    }
    return std::min(got, want);
}
constexpr VkBufferUsageFlags kGuestUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

} // namespace

// ------------------------------------------------------------------------------------------------ Timeline

Timeline::Timeline(DeviceContext& ctx) : ctx_(ctx) {
    const VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                         .initialValue = 0};
    const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    if (Fn().vkCreateSemaphore(ctx_.Device(), &ci, nullptr, &semaphore_) != VK_SUCCESS) {
        semaphore_ = VK_NULL_HANDLE;
    }
    if (Fn().vkCreateSemaphore(ctx_.Device(), &ci, nullptr, &wake_) != VK_SUCCESS) {
        wake_ = VK_NULL_HANDLE; // (WaitOrWake waits for the GPU alone)
    }
}

Timeline::~Timeline() {
    if (semaphore_ != VK_NULL_HANDLE) {
        Fn().vkDestroySemaphore(ctx_.Device(), semaphore_, nullptr);
    }
    if (lane_ != VK_NULL_HANDLE) {
        Fn().vkDestroySemaphore(ctx_.Device(), lane_, nullptr);
    }
    if (wake_ != VK_NULL_HANDLE) {
        Fn().vkDestroySemaphore(ctx_.Device(), wake_, nullptr);
    }
}

u64 Timeline::Completed() {
    if (ctx_.Failed()) {
        return CompletedCached();
    }
    u64 v = 0;
    const VkResult r = Fn().vkGetSemaphoreCounterValue(ctx_.Device(), semaphore_, &v);
    u64 lv = 0;
    const VkResult lr = lane_ != VK_NULL_HANDLE ? Fn().vkGetSemaphoreCounterValue(ctx_.Device(), lane_, &lv) : VK_SUCCESS;
    if (r == VK_SUCCESS && lr == VK_SUCCESS) {
        if (lane_ != VK_NULL_HANDLE) {
            const std::lock_guard lock{lane_mu_};
            v = DenseLocked(v, lv);
            Advance(v);
            while (!lane_pending_.empty() && lane_pending_.front() <= v) {
                lane_pending_.pop_front(); // (complete)
            }
        } else {
            Advance(v);
        }
    } else {
        const VkResult e = r != VK_SUCCESS ? r : lr;
        last_error_.store(e, std::memory_order_release);
        ctx_.Fail("vkGetSemaphoreCounterValue", e);
    }
    return completed_.load(std::memory_order_acquire);
}

u64 Timeline::QueryCompleted() const {
    if (ctx_.Failed()) {
        return 0;
    }
    u64 v = 0;
    if (Fn().vkGetSemaphoreCounterValue(ctx_.Device(), semaphore_, &v) != VK_SUCCESS) {
        return 0;
    }
    if (lane_ == VK_NULL_HANDLE) {
        return v;
    }
    u64 lv = 0;
    if (Fn().vkGetSemaphoreCounterValue(ctx_.Device(), lane_, &lv) != VK_SUCCESS) {
        return 0;
    }
    const std::lock_guard lock{lane_mu_};
    return DenseLocked(v, lv);
}

bool Timeline::EnableLane() {
    if (lane_ != VK_NULL_HANDLE) {
        return true;
    }
    const VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                         .initialValue = 0};
    const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    if (Fn().vkCreateSemaphore(ctx_.Device(), &ci, nullptr, &lane_) != VK_SUCCESS) {
        lane_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void Timeline::SubmittedOnLane(u64 value) {
    const std::lock_guard lock{lane_mu_};
    lane_pending_.push_back(value);
}

bool Timeline::MainReached(u64 value) const {
    if (!main_free_ || lane_ == VK_NULL_HANDLE) {
        return false;
    }
    {
        const std::lock_guard lock{lane_mu_};
        if (OnLaneLocked(value)) {
            return false;
        }
    }
    return QueryMain() >= value;
}

u64 Timeline::QueryMain() const {
    u64 v = 0;
    return Fn().vkGetSemaphoreCounterValue(ctx_.Device(), semaphore_, &v) == VK_SUCCESS ? v : 0;
}

u64 Timeline::QueryLane() const {
    u64 v = 0;
    return lane_ != VK_NULL_HANDLE && Fn().vkGetSemaphoreCounterValue(ctx_.Device(), lane_, &v) == VK_SUCCESS ? v : 0;
}

u64 Timeline::DenseLocked(u64 main, u64 lane) const {
    // the main semaphore's value: every batch up to it completed (a main batch waited for the lane's earlier ones);
    // above it, the run of lane values the lane completed that follow one another
    u64 v = std::max(main, completed_.load(std::memory_order_acquire));
    if (main_free_) {
        // R10 (SetMainFree): a main batch need not have waited for the lane's earlier ones - below the lane's first
        // value not completed (the lane completes in its order: the values up to `lane` did; never below what was
        // seen: those values were complete then)
        const auto first = std::upper_bound(lane_pending_.begin(), lane_pending_.end(), lane);
        if (first != lane_pending_.end() && *first <= v) {
            v = std::max(*first - 1, completed_.load(std::memory_order_acquire));
        }
    }
    auto it = std::upper_bound(lane_pending_.begin(), lane_pending_.end(), v);
    while (it != lane_pending_.end() && *it == v + 1 && *it <= lane) {
        v = *it++;
    }
    return v;
}

bool Timeline::WaitAny(u64 main_value, u64 lane_value, u64 wake_value, u64 timeout_ns) {
    if (ctx_.Failed()) {
        return false;
    }
    VkSemaphore sems[3];
    u64 values[3];
    u32 n = 0;
    if (main_value != 0) {
        sems[n] = semaphore_;
        values[n++] = main_value;
    }
    if (lane_value != 0 && lane_ != VK_NULL_HANDLE) {
        sems[n] = lane_;
        values[n++] = lane_value;
    }
    const u32 gpu = n;
    if (wake_value != 0 && wake_ != VK_NULL_HANDLE) {
        sems[n] = wake_;
        values[n++] = wake_value;
    }
    if (gpu == 0) {
        return false;
    }
    const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                 .flags = VK_SEMAPHORE_WAIT_ANY_BIT,
                                 .semaphoreCount = n,
                                 .pSemaphores = sems,
                                 .pValues = values};
    const VkResult r = Fn().vkWaitSemaphores(ctx_.Device(), &wi, timeout_ns);
    if (r == VK_SUCCESS) {
        return (main_value != 0 && QueryMain() >= main_value) || (lane_value != 0 && QueryLane() >= lane_value);
    }
    if (r != VK_TIMEOUT) {
        last_error_.store(r, std::memory_order_release);
        ctx_.Fail("vkWaitSemaphores", r);
    }
    return false;
}

bool Timeline::Wait(u64 value, u64 timeout_ns) {
    if (ctx_.Failed()) {
        return false;
    }
    if (completed_.load(std::memory_order_acquire) >= value) {
        return true;
    }
    VkSemaphore sems[2] = {semaphore_, lane_};
    u64 values[2] = {value, 0};
    u32 n = 1;
    if (lane_ != VK_NULL_HANDLE) {
        // AQ: the main semaphore at the last main value up to `value`, the lane's at its last value up to it (a value
        // not submitted yet is taken as the main queue's: as before)
        const std::lock_guard lock{lane_mu_};
        const u64 done = completed_.load(std::memory_order_acquire);
        u64 m = value;
        while (m > done && OnLaneLocked(m)) {
            --m;
        }
        const auto it = std::upper_bound(lane_pending_.begin(), lane_pending_.end(), value);
        const u64 l = it != lane_pending_.begin() && *std::prev(it) > done ? *std::prev(it) : 0;
        n = 0;
        if (m > done) {
            values[n] = m;
            sems[n++] = semaphore_;
        }
        if (l != 0) {
            values[n] = l;
            sems[n++] = lane_;
        }
        if (n == 0) {
            Advance(value);
            return true;
        }
    }
    const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                 .semaphoreCount = n,
                                 .pSemaphores = sems,
                                 .pValues = values};
    const VkResult r = Fn().vkWaitSemaphores(ctx_.Device(), &wi, timeout_ns);
    if (r == VK_SUCCESS) {
        Advance(value);
        return true;
    }
    if (r != VK_TIMEOUT) {
        last_error_.store(r, std::memory_order_release);
        ctx_.Fail("vkWaitSemaphores", r);
    }
    return false;
}

bool Timeline::WaitOrWake(u64 value, u64 wake_value, u64 timeout_ns) {
    if (wake_ == VK_NULL_HANDLE || lane_ != VK_NULL_HANDLE) {
        return Wait(value, timeout_ns); // (AQ: the executor waits with WaitAny)
    }
    if (ctx_.Failed()) {
        return false;
    }
    if (completed_.load(std::memory_order_acquire) >= value) {
        return true;
    }
    const VkSemaphore sems[2] = {semaphore_, wake_};
    const u64 values[2] = {value, wake_value};
    const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                 .flags = VK_SEMAPHORE_WAIT_ANY_BIT,
                                 .semaphoreCount = 2,
                                 .pSemaphores = sems,
                                 .pValues = values};
    const VkResult r = Fn().vkWaitSemaphores(ctx_.Device(), &wi, timeout_ns);
    if (r == VK_SUCCESS) {
        return Completed() >= value; // (either one: the GPU's value asked)
    }
    if (r != VK_TIMEOUT) {
        last_error_.store(r, std::memory_order_release);
        ctx_.Fail("vkWaitSemaphores", r);
    }
    return false;
}

void Timeline::Wake(u64 wake_value) {
    if (wake_ == VK_NULL_HANDLE || ctx_.Failed()) {
        return;
    }
    const VkSemaphoreSignalInfo si{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = wake_,
                                   .value = wake_value};
    Fn().vkSignalSemaphore(ctx_.Device(), &si); // (a failure: the waiter's timeout ends its wait as before)
}

// ------------------------------------------------------------------------------------------------ OwnedBuffer

bool CreateBuffer(DeviceContext& ctx, u64 size, VkBufferUsageFlags usage, bool host_visible, OwnedBuffer& out,
                  std::string& error, bool host_cached, HostPlacement placement) {
    host_cached |= placement == HostPlacement::Cached;
    out = {};
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = size,
                                 .usage = usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBufferCreateInfo shared = bci;
    ctx.ShareWithLane(shared); // (FrameBreak AQ)
    if (Fn().vkCreateBuffer(ctx.Device(), &shared, nullptr, &out.buffer) != VK_SUCCESS) {
        error = fmt::format("vkCreateBuffer of {} bytes failed", size);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req{};
    Fn().vkGetBufferMemoryRequirements(ctx.Device(), out.buffer, &req);
    s32 type = -1;
    if (host_visible && host_cached) {
        type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (type >= 0) {
    } else if (host_visible) {
        // device-local + host-visible (the BAR) first for small buffers, then plain host memory
        if (size <= (64ull << 20) || placement == HostPlacement::Bar) {
            type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (type < 0) {
            type = ctx.FindMemoryType(req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    } else {
        type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (type < 0) {
        error = "no memory type for the buffer";
        DestroyBuffer(ctx, out);
        return false;
    }
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = static_cast<u32>(type)};
    if (const VkResult r = Fn().vkAllocateMemory(ctx.Device(), &mai, nullptr, &out.memory); r != VK_SUCCESS) {
        error = fmt::format("vkAllocateMemory of {} bytes failed ({})", req.size, static_cast<int>(r));
        out.memory = VK_NULL_HANDLE;
        DestroyBuffer(ctx, out);
        return false;
    }
    Fn().vkBindBufferMemory(ctx.Device(), out.buffer, out.memory, 0);
    out.memory_type = static_cast<u32>(type);
    if (host_visible) {
        void* p = nullptr;
        if (Fn().vkMapMemory(ctx.Device(), out.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            error = "vkMapMemory failed";
            DestroyBuffer(ctx, out);
            return false;
        }
        out.mapped = static_cast<u8*>(p);
    }
    out.size = size;
    return true;
}

void DestroyBuffer(DeviceContext& ctx, OwnedBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        Fn().vkDestroyBuffer(ctx.Device(), buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        Fn().vkFreeMemory(ctx.Device(), buffer.memory, nullptr); // unmaps
    }
    buffer = {};
}

// ------------------------------------------------------------------------------------------------ StreamRing

StreamRing::StreamRing(DeviceContext& ctx, Timeline& timeline, u64 size, HostPlacement placement)
    : ctx_(ctx), timeline_(timeline) {
    std::string error;
    if (!CreateBuffer(ctx_, size,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      placement != HostPlacement::Device, buf_, error, false, placement)) {
        SSPC_LOG(GPU, Error, "native backend: the stream ring ({} MiB): {}", size >> 20, error);
    }
}

StreamRing::~StreamRing() {
    DestroyBuffer(ctx_, mirror_);
    DestroyBuffer(ctx_, buf_);
}

bool StreamRing::CreateMirror(std::string& error) {
    if (buf_.mapped == nullptr) {
        error = "the ring is not host-visible";
        return false;
    }
    return CreateBuffer(ctx_, buf_.size,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                        false, mirror_, error);
}

u32 StreamRing::TakeDirty(VkBufferCopy (&out)[2]) {
    const u64 begin = dirty_begin_, end = head_;
    dirty_begin_ = head_;
    if (mirror_.buffer == VK_NULL_HANDLE || end <= begin) {
        return 0;
    }
    const u64 size = buf_.size;
    mirrored_bytes_ += end - begin;
    if (end - begin >= size) { // (never: one submission holds less than the ring)
        out[0] = {0, 0, size};
        return 1;
    }
    const u64 b = begin % size, e = end % size;
    if (e == 0 || b < e) { // no wrap inside (or it ends exactly at the ring's end)
        out[0] = {b, b, (e == 0 ? size : e) - b};
        return 1;
    }
    out[0] = {b, b, size - b}; // up to the end (the skipped tail before a wrap is free: copying it is harmless)
    out[1] = {0, 0, e};
    return 2;
}

StreamRing::Status StreamRing::Allocate(u64 bytes, u64 align, u64 seq, Slice& out) {
    const u64 size = buf_.size;
    if (bytes > size / 2 || bytes == 0) {
        return bytes == 0 ? Status::Ok : Status::TooLarge;
    }
    align = std::max<u64>(align, 4);
    for (;;) {
        u64 start = AlignUp(head_, align);
        if ((start % size) + bytes > size) {
            start = AlignUp(start, size); // does not fit before the end: from the ring's start
        }
        if (start + bytes - tail_ <= size) {
            head_ = start + bytes;
            if (!fences_.empty() && fences_.back().seq == seq) {
                fences_.back().end = head_;
            } else {
                fences_.push_back({seq, head_});
            }
            used_ = head_ - tail_;
            peak_ = std::max(peak_, used_);
            allocated_ += bytes;
            out.buffer = mirror_.buffer != VK_NULL_HANDLE ? mirror_.buffer : buf_.buffer;
            out.offset = start % size;
            out.data = buf_.mapped != nullptr ? buf_.mapped + out.offset : nullptr;
            return Status::Ok;
        }
        // full: the oldest submission's regions must be released
        if (fences_.empty() || fences_.front().seq >= seq) {
            return Status::NeedFlush; // only the submission being recorded holds the ring: it must be submitted
        }
        ++waits_;
        const u64 wait_for = fences_.front().seq;
        if (wait_for > timeline_.LastSubmitted()) {
            return Status::NeedFlush;
        }
        const StallTrace::Scope trace_wait(StallTrace::Ev::RingWait, wait_for); // (P4.12)
        if (!timeline_.Wait(wait_for, 10'000'000'000ull)) {
            return Status::NeedFlush;
        }
        Collect(timeline_.Completed());
    }
}

void StreamRing::Collect(u64 completed) {
    while (!fences_.empty() && fences_.front().seq <= completed) {
        tail_ = fences_.front().end;
        fences_.pop_front();
    }
    if (fences_.empty()) {
        tail_ = head_;
    }
    used_ = head_ - tail_;
}

// ------------------------------------------------------------------------------------------------ GuestBuffers

GuestBuffers::GuestBuffers(DeviceContext& ctx) : ctx_(ctx) {}

GuestBuffers::~GuestBuffers() {
    std::scoped_lock lock{mutex_};
    for (auto& [base, w] : windows_) {
        Destroy(w);
    }
    windows_.clear();
    for (auto& w : retired_) {
        Destroy(w);
    }
    retired_.clear();
}

bool GuestBuffers::Import(u64 base, u64 size, u8* host, Window& out, std::string& error) {
    const auto& f = Fn();
    void* p = host;
    VkMemoryHostPointerPropertiesEXT props{.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = f.vkGetMemoryHostPointerPropertiesEXT(ctx_.Device(),
                                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &props);
    if (r != VK_SUCCESS || props.memoryTypeBits == 0) {
        error = fmt::format("host pointer {:#x}: no memory type ({})", base, static_cast<int>(r));
        return false;
    }
    const VkExternalMemoryBufferCreateInfo ext{.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                               .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .pNext = &ext,
                                 .size = size,
                                 .usage = kGuestUsage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBufferCreateInfo shared = bci;
    ctx_.ShareWithLane(shared); // (FrameBreak AQ)
    if (f.vkCreateBuffer(ctx_.Device(), &shared, nullptr, &out.buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer (external) failed";
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req{};
    f.vkGetBufferMemoryRequirements(ctx_.Device(), out.buffer, &req);
    const s32 type = ctx_.FindMemoryType(req.memoryTypeBits & props.memoryTypeBits, 0);
    if (type < 0) {
        error = "no memory type both the buffer and the host pointer accept";
        f.vkDestroyBuffer(ctx_.Device(), out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    const VkImportMemoryHostPointerInfoEXT imp{.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               .pHostPointer = p};
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .pNext = &imp,
                                   .allocationSize = size,
                                   .memoryTypeIndex = static_cast<u32>(type)};
    r = f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        // VK_ERROR_OUT_OF_DEVICE_MEMORY / INVALID_EXTERNAL_HANDLE: pages not committed or read-only (P4.4.0 probe)
        error = fmt::format("import of [{:#x}, {:#x}) failed ({}): pages not committed or not writable", base,
                            base + size, static_cast<int>(r));
        f.vkDestroyBuffer(ctx_.Device(), out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        out.memory = VK_NULL_HANDLE;
        return false;
    }
    f.vkBindBufferMemory(ctx_.Device(), out.buffer, out.memory, 0);
    out.base = base;
    out.size = size;
    return true;
}

void GuestBuffers::Destroy(Window& w) {
    if (w.buffer != VK_NULL_HANDLE) {
        Fn().vkDestroyBuffer(ctx_.Device(), w.buffer, nullptr);
    }
    if (w.memory != VK_NULL_HANDLE) {
        Fn().vkFreeMemory(ctx_.Device(), w.memory, nullptr);
    }
    w.buffer = VK_NULL_HANDLE;
    w.memory = VK_NULL_HANDLE;
}

bool GuestBuffers::Get(u64 addr, u64 size, u64 seq, View& out, Failure& failure) {
    failure = Failure::None;
    if (!ctx_.Caps().external_memory_host) {
        failure = Failure::NoExtension;
        return false;
    }
    if (size == 0) {
        size = 4;
    }
    if (addr >= (1ull << 40) || size > (1ull << 40) - addr) {
        failure = Failure::Range;
        return false;
    }
    const u64 end = addr + size;
    std::scoped_lock lock{mutex_};
    ++stats_.lookups;
    // the window found last (most lookups fall into it: one 32 MiB window covers a frame's small ranges)
    if (Window* w = last_get_; w != nullptr && w->base <= addr && end <= w->base + w->size) {
        w->last_seq = std::max(w->last_seq, seq);
        out = {w->buffer, addr - w->base, w->base, w->size, w->backing};
        return true;
    }
    // windows starting at or below addr, nearest first, up to the longest window back
    auto it = windows_.upper_bound(addr);
    while (it != windows_.begin()) {
        --it;
        Window& w = it->second;
        if (w.base + max_window_ < addr) {
            break;
        }
        if (w.base <= addr && end <= w.base + w.size) {
            w.last_seq = std::max(w.last_seq, seq);
            out = {w.buffer, addr - w.base, w.base, w.size, w.backing};
            last_get_ = &w;
            return true;
        }
    }
    // a new window: the 32 MiB one around addr, the 16 MiB one, then exactly the range. Through the physical backing's
    // view when the range is linear there: the guest's pages stay free for write tracking (NVIDIA loses the device on
    // any access to imported host memory whose protection changed after the import). Else (no backing) the guest's
    // own pages, which are then never write-protected.
    const u64 align = std::max<u64>(ctx_.Caps().min_imported_host_alignment, 4096);
    struct Try {
        u64 base, size;
        bool exact, headroom;
    };
    Try tries[3]{};
    u32 ntries = 0;
    u64 lo = 0, hi = 1ull << 40;
    u8* run_host = nullptr;
    u64 run_begin = 0, run_size = 0;
    GuestMemory* gm = GetGuestMemory();
    const bool have_run = gm != nullptr && gm->BackingRun(addr, &run_host, &run_begin, &run_size);
    // REA-4 (SSPC_REA_MERGE_RUNS=1): the physical backing is one host allocation; runs that follow each other in both the
    // guest's and the host's addresses are one view (the game's 542 MiB vertex-data heap crosses such a seam: imported
    // through the guest's own pages it made its whole window untrackable - every constant buffer in it read over PCIe)
    static const bool merge_runs = [] {
        const char* v = std::getenv("SSPC_REA_MERGE_RUNS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    if (have_run && merge_runs) {
        while (run_begin + run_size < end) {
            u8* next_host = nullptr;
            u64 next_begin = 0, next_size = 0;
            if (!gm->BackingRun(run_begin + run_size, &next_host, &next_begin, &next_size) || next_size == 0 ||
                next_begin != run_begin + run_size || next_host != run_host + run_size) {
                break;
            }
            run_size += next_size;
            ++stats_.merged_runs;
        }
    }
    const bool backed = have_run && run_begin <= addr && end <= run_begin + run_size && (run_begin % align) == 0 &&
                        (reinterpret_cast<u64>(run_host) % align) == 0;
    if (backed) {
        lo = run_begin;
        hi = AlignDown(run_begin + run_size, align);
    }
    const auto add = [&](u64 b, u64 e, bool exact, bool headroom) {
        b = std::max(b, lo);
        e = std::min(e, hi);
        const bool repeat = ntries != 0 && tries[ntries - 1].base == b && tries[ntries - 1].size == e - b;
        if (b <= addr && end <= e && ntries < 3 && !repeat) {
            tries[ntries++] = {b, e - b, exact, headroom};
        }
    };
    const u64 wbase = AlignDown(addr, kWindowAlign);
    add(wbase, wbase + kWindowSize, false, false);
    add(wbase, wbase + kWindowAlign, false, false);
    // R9-F3 (SSPC_R9_HEAP_WINDOW=1): the engine's 2.5 GiB vertex-data heap binding grows a page at a time as meshes
    // stream in; each longer binding missed every window and imported the whole heap again (~100 ms of the recording
    // thread), the old windows staying imported until the driver refused more (OUT_OF_DEVICE_MEMORY at ~14 GiB, then
    // REA-7 bound an older prefix). A heap range's window reaches up to kHeapHeadroom past its end, over the run's
    // committed writable pages only (what the guest has there: an import changes no page), and replaces the windows of
    // its base that it covers (destroyed once their work completed, as dropped ones).
    static const bool heap_window = [] {
        const char* v = std::getenv("SSPC_R9_HEAP_WINDOW");
        const bool on = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (on) {
            SSPC_LOG(GPU, Warning, "R9-F3: SSPC_R9_HEAP_WINDOW=1 - heap bindings (>= 256 MiB) imported with up to {} MiB "
                                   "past their end, replacing the shorter windows of their base", kHeapHeadroom >> 20);
        }
        return on;
    }();
    const bool heap = heap_window && backed && size >= kHeapWindowMin;
    if (heap && AlignUp(end, align) < hi) {
        const u64 e = AlignUp(end, align);
        const u64 extra = AlignDown(CommittedWritable(run_host + (e - run_begin), std::min(hi - e, kHeapHeadroom)), align);
        if (extra != 0) {
            add(AlignDown(addr, align), e + extra, true, true);
        }
    }
    add(AlignDown(addr, align), AlignUp(end, align), true, false);
    std::string error;
    // SSPC_R9_IMPORTS=1: every import of 64 MiB or more and every failed one, with the driver's time (R9: the fight's
    // 200 ms frames were the recording thread in vkAllocateMemory of ~2.5 GB host-pointer windows)
    static const bool log_imports = [] {
        const char* v = std::getenv("SSPC_R9_IMPORTS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    for (u32 i = 0; i < ntries; ++i) {
        Window w;
        u8* host = reinterpret_cast<u8*>(tries[i].base);
        if (backed) {
            host = run_host + (tries[i].base - run_begin);
        } else if (tracker_ != nullptr) {
            tracker_->NeverArm(tries[i].base, tries[i].size);
            static const bool diag = [] {
                const char* lb = std::getenv("SSPC_DIAG_LARGE_BINDS");
                return lb != nullptr && lb[0] == '1' && lb[1] == '\0';
            }();
            if (diag) { // REA-1: which range made a window of the guest's own pages (never tracked again)
                SSPC_LOG(GPU, Info, "REA-1 import without backing: window [{:#x}, +{:#x}) for [{:#x}, {:#x}); backing run "
                                    "[{:#x}, +{:#x}) host {}", tries[i].base, tries[i].size, addr, end, run_begin, run_size,
                         static_cast<const void*>(run_host));
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool imported = Import(tries[i].base, tries[i].size, host, w, error);
        if (log_imports && (!imported || tries[i].size >= (64ull << 20))) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            // after the window's end: the backing run (the addresses still host-contiguous) and its committed pages
            const u8* after = host + tries[i].size;
            u64 committed_after = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(after, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
                committed_after = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize - reinterpret_cast<u64>(after);
            }
            SSPC_LOG(GPU, Info, "R9 import {} [{:#x}, +{:#x}) for [{:#x}, +{:#x}){}{}{}: {:.1f} ms; windows {} ({} MiB); "
                                "after it: run +{:#x}, committed +{:#x}",
                     imported ? "of" : "FAILED", tries[i].base, tries[i].size, addr, size, backed ? " backed" : "",
                     tries[i].exact ? " exact" : "", tries[i].headroom ? " +headroom" : "", ms, stats_.windows,
                     stats_.bytes >> 20, backed ? hi - std::min(hi, tries[i].base + tries[i].size) : 0, committed_after);
        }
        if (imported && heap) {
            // R9-F3: the windows of this base that the new one covers leave the lookup (the cached ones may be among them)
            last_get_ = nullptr;
            last_backing_ = nullptr;
            for (auto it = windows_.lower_bound(w.base); it != windows_.end() && it->first == w.base;) {
                if (it->second.size <= w.size) {
                    ++stats_.superseded;
                    --stats_.windows;
                    stats_.bytes -= it->second.size;
                    retired_.push_back(it->second);
                    it = windows_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (imported) {
            w.backing = backed;
            w.host = host;
            w.last_seq = seq;
            ++stats_.imports;
            ++stats_.windows;
            stats_.bytes += w.size;
            stats_.exact_windows += tries[i].exact ? 1 : 0;
            max_window_ = std::max(max_window_, w.size);
            out = {w.buffer, addr - w.base, w.base, w.size, w.backing};
            ++(backed ? stats_.backing_windows : stats_.guest_windows);
            windows_.emplace(w.base, w); // an equal base (a smaller window there) stays too: lookups take either
            return true;
        }
    }
    ++stats_.import_failures;
    if (stats_.first_failure.empty()) {
        stats_.first_failure = error;
        SSPC_LOG(GPU, Error, "native backend: guest buffer [{:#x}, {:#x}) cannot be imported: {}", addr, end, error);
    }
    failure = Failure::NotImportable;
    return false;
}

const u8* GuestBuffers::BackingPointer(u64 addr, u64 size) {
    const u64 end = addr + size;
    std::scoped_lock lock{mutex_};
    if (const Window* w = last_backing_; w != nullptr && w->base <= addr && end <= w->base + w->size) {
        return w->host + (addr - w->base);
    }
    auto it = windows_.upper_bound(addr);
    while (it != windows_.begin()) {
        --it;
        const Window& w = it->second;
        if (w.base + max_window_ < addr) {
            break;
        }
        if (w.backing && w.base <= addr && end <= w.base + w.size) {
            last_backing_ = &w;
            return w.host + (addr - w.base);
        }
    }
    return nullptr;
}

void GuestBuffers::Drop(u64 addr, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 end = addr + size;
    std::scoped_lock lock{mutex_};
    last_get_ = nullptr; // (they may be erased below)
    last_backing_ = nullptr;
    auto it = windows_.lower_bound(addr > max_window_ ? addr - max_window_ : 0);
    while (it != windows_.end() && it->first < end) {
        Window& w = it->second;
        if (w.base + w.size > addr) {
            ++stats_.drops;
            --stats_.windows;
            stats_.bytes -= w.size;
            retired_.push_back(w);
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

void GuestBuffers::Collect(u64 completed) {
    std::scoped_lock lock{mutex_};
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].last_seq <= completed) {
            Destroy(retired_[i]);
            ++stats_.destroyed;
            retired_[i] = retired_.back();
            retired_.pop_back();
        } else {
            ++i;
        }
    }
}

GuestBuffers::Stats GuestBuffers::GetStats() {
    std::scoped_lock lock{mutex_};
    return stats_;
}

} // namespace SecondSon::Gpu::Vk
