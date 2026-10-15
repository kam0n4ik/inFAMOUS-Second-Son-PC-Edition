// SecondSonPC — the backend's timeline, stream ring and guest buffers (P4.4). See vk_resources.h.
#include "runtime/gpu/vulkan/vk_resources.h"

#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/page_tracker.h"

#include <algorithm>
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
}

Timeline::~Timeline() {
    if (semaphore_ != VK_NULL_HANDLE) {
        Fn().vkDestroySemaphore(ctx_.Device(), semaphore_, nullptr);
    }
}

u64 Timeline::Completed() {
    if (ctx_.Failed()) {
        return CompletedCached();
    }
    u64 v = 0;
    const VkResult r = Fn().vkGetSemaphoreCounterValue(ctx_.Device(), semaphore_, &v);
    if (r == VK_SUCCESS) {
        Advance(v);
    } else {
        last_error_.store(r, std::memory_order_release);
        ctx_.Fail("vkGetSemaphoreCounterValue", r);
    }
    return completed_.load(std::memory_order_acquire);
}

u64 Timeline::QueryCompleted() const {
    if (ctx_.Failed()) {
        return 0;
    }
    u64 v = 0;
    return Fn().vkGetSemaphoreCounterValue(ctx_.Device(), semaphore_, &v) == VK_SUCCESS ? v : 0;
}

bool Timeline::Wait(u64 value, u64 timeout_ns) {
    if (ctx_.Failed()) {
        return false;
    }
    if (completed_.load(std::memory_order_acquire) >= value) {
        return true;
    }
    const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                 .semaphoreCount = 1,
                                 .pSemaphores = &semaphore_,
                                 .pValues = &value};
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

// ------------------------------------------------------------------------------------------------ OwnedBuffer

bool CreateBuffer(DeviceContext& ctx, u64 size, VkBufferUsageFlags usage, bool host_visible, OwnedBuffer& out,
                  std::string& error, bool host_cached, HostPlacement placement) {
    host_cached |= placement == HostPlacement::Cached;
    out = {};
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = size,
                                 .usage = usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (Fn().vkCreateBuffer(ctx.Device(), &bci, nullptr, &out.buffer) != VK_SUCCESS) {
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
    if (f.vkCreateBuffer(ctx_.Device(), &bci, nullptr, &out.buffer) != VK_SUCCESS) {
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
        bool exact;
    };
    Try tries[3]{};
    u32 ntries = 0;
    u64 lo = 0, hi = 1ull << 40;
    u8* run_host = nullptr;
    u64 run_begin = 0, run_size = 0;
    GuestMemory* gm = GetGuestMemory();
    const bool backed = gm != nullptr && gm->BackingRun(addr, &run_host, &run_begin, &run_size) &&
                        run_begin <= addr && end <= run_begin + run_size && (run_begin % align) == 0 &&
                        (reinterpret_cast<u64>(run_host) % align) == 0;
    if (backed) {
        lo = run_begin;
        hi = AlignDown(run_begin + run_size, align);
    }
    const auto add = [&](u64 b, u64 e, bool exact) {
        b = std::max(b, lo);
        e = std::min(e, hi);
        const bool repeat = ntries != 0 && tries[ntries - 1].base == b && tries[ntries - 1].size == e - b;
        if (b <= addr && end <= e && ntries < 3 && !repeat) {
            tries[ntries++] = {b, e - b, exact};
        }
    };
    const u64 wbase = AlignDown(addr, kWindowAlign);
    add(wbase, wbase + kWindowSize, false);
    add(wbase, wbase + kWindowAlign, false);
    add(AlignDown(addr, align), AlignUp(end, align), true);
    std::string error;
    for (u32 i = 0; i < ntries; ++i) {
        Window w;
        u8* host = reinterpret_cast<u8*>(tries[i].base);
        if (backed) {
            host = run_host + (tries[i].base - run_begin);
        } else if (tracker_ != nullptr) {
            tracker_->NeverArm(tries[i].base, tries[i].size);
        }
        if (Import(tries[i].base, tries[i].size, host, w, error)) {
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
