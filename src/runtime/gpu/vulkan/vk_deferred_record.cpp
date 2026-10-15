// SecondSonPC — deferred Vulkan command recording on a recorder thread (vk_deferred_record.h).
// Ported from uncharted_pc gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.{h,cpp} (RecordChunk, Record,
// KickRecording, SyncRecording, RecorderThread; GPL-2.0-or-later, shadPS4 Emulator Project / bbport).
#include <windows.h>

#include "runtime/gpu/vulkan/vk_deferred_record.h"

#include <immintrin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Gpu::Vk::Deferred {

namespace {

using Clock = std::chrono::steady_clock;

/// A block of deferred commands: closures placed in fixed storage, run in order (uncharted_pc RecordChunk).
class Chunk {
public:
    static constexpr size_t kCapacity = 128 * 1024;

    template <typename Func>
    bool Push(Func&& func) {
        using Command = Typed<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= 1024, "a deferred command's closure is too large");
        const size_t offset = (used_ + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > kCapacity) {
            return false;
        }
        auto* command = new (storage_ + offset) Command(std::forward<Func>(func));
        if (last_ != nullptr) {
            last_->next = command;
        } else {
            first_ = command;
        }
        last_ = command;
        used_ = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Storage for a command's arrays (the same chunk as the command, recycled after it ran); null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used_ + align - 1) & ~(align - 1);
        if (offset + bytes > kCapacity) {
            return nullptr;
        }
        used_ = offset + bytes;
        PrefetchAhead();
        return storage_ + offset;
    }

    void Execute() {
        for (Base* command = first_; command != nullptr;) {
            Base* const next = command->next;
            command->Run();
            command->~Base();
            command = next;
        }
        first_ = last_ = nullptr;
        used_ = 0;
    }

    bool Empty() const { return first_ == nullptr; }
    size_t Size() const { return used_; }
    size_t Room() const { return kCapacity - used_; }

private:
    // (RecordPrefetch: the recorder thread last read these lines; ownership requested ahead overlaps the transfers)
    void PrefetchAhead() const {
        __builtin_prefetch(storage_ + used_ + 384, 1, 3);
        __builtin_prefetch(storage_ + used_ + 448, 1, 3);
    }

    struct Base {
        virtual ~Base() = default;
        virtual void Run() = 0;
        Base* next = nullptr;
    };
    template <typename Func>
    struct Typed final : Base {
        explicit Typed(Func&& f) : func(std::move(f)) {}
        explicit Typed(const Func& f) : func(f) {}
        void Run() override { func(); }
        Func func;
    };

    alignas(64) std::byte storage_[kCapacity + 512]; // (+512: the prefetch ahead stays inside the allocation)
    size_t used_ = 0;
    Base* first_ = nullptr;
    Base* last_ = nullptr;
};

enum SyncReason : u32 { kSubmit, kCommandBuffer, kDestroy, kFallback, kExplicit, kReasons };
constexpr const char* kReasonNames[kReasons] = {"queue", "command buffer/pool", "destroy/free/reset", "direct command",
                                                "explicit"};

struct State {
    const Api* real = nullptr;
    Api table{};
    bool enabled = false;
    // producer side: the thread recording (one at a time - the parallel processor's drains order them)
    std::unique_ptr<Chunk> cur;
    std::vector<std::unique_ptr<Chunk>> full;
    // shared
    std::mutex m;
    std::condition_variable cv, idle_cv;
    std::deque<std::unique_ptr<Chunk>> queue;
    std::vector<std::unique_ptr<Chunk>> free;
    bool busy = false, sleeping = false, stop = false;
    std::atomic<size_t> queued{0};
    std::thread thread;
    // counters
    std::atomic<u64> commands{0}, data_bytes{0}, kicks{0}, chunks{0}, host_copies{0}, host_copy_bytes{0};
    std::atomic<u64> syncs[kReasons]{}, sync_waited{0}, sync_ns{0}, busy_ns{0}, fallbacks{0};
};
State g;
thread_local Api* t_table = nullptr;

std::unique_ptr<Chunk> AcquireChunk() {
    std::scoped_lock lk{g.m};
    if (g.free.empty()) {
        return std::make_unique<Chunk>();
    }
    auto chunk = std::move(g.free.back());
    g.free.pop_back();
    return chunk;
}

void Kick(bool force) {
    // (batches of tens of KiB keep the hand-off cheap relative to the work it carries)
    if (!force && g.full.empty() && g.cur->Size() < 32 * 1024) {
        return;
    }
    if (g.full.empty() && g.cur->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{g.m};
        for (auto& chunk : g.full) {
            g.queue.push_back(std::move(chunk));
        }
        if (!g.cur->Empty()) {
            g.queue.push_back(std::move(g.cur));
        }
        wake = g.sleeping;
        g.queued.store(g.queue.size(), std::memory_order_release);
    }
    g.full.clear();
    g.kicks.fetch_add(1, std::memory_order_relaxed);
    if (wake) { // (a busy recorder takes new chunks by itself: a wake-up is a system call)
        g.cv.notify_one();
    }
    if (!g.cur) {
        g.cur = AcquireChunk();
    }
}

void SyncFor(SyncReason reason) {
    if (!g.enabled) {
        return;
    }
    g.syncs[reason].fetch_add(1, std::memory_order_relaxed);
    Kick(true);
    std::unique_lock lk{g.m};
    if (g.queue.empty() && !g.busy) {
        return;
    }
    const auto t0 = Clock::now();
    g.idle_cv.wait(lk, [] { return g.queue.empty() && !g.busy; });
    g.sync_waited.fetch_add(1, std::memory_order_relaxed);
    g.sync_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(),
                        std::memory_order_relaxed);
}

void RecorderLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:VkRecorder");
    while (true) {
        // a short spin before sleeping: the next chunk usually follows within microseconds, and a sleeping recorder
        // costs the producer a wake-up per kick (uncharted_pc: 200 us with >= 12 hardware threads, else 20 us)
        const auto spin = std::chrono::microseconds(std::thread::hardware_concurrency() >= 12 ? 200 : 20);
        const auto until = Clock::now() + spin;
        for (u32 spins = 1; g.queued.load(std::memory_order_acquire) == 0; ++spins) {
            _mm_pause();
            if ((spins & 255) == 0 && Clock::now() >= until) {
                break;
            }
        }
        std::unique_ptr<Chunk> chunk;
        {
            std::unique_lock lk{g.m};
            g.sleeping = true;
            g.cv.wait(lk, [] { return !g.queue.empty() || g.stop; });
            g.sleeping = false;
            if (g.queue.empty()) {
                return; // stop
            }
            chunk = std::move(g.queue.front());
            g.queue.pop_front();
            g.queued.store(g.queue.size(), std::memory_order_release);
            g.busy = true;
        }
        const auto t0 = Clock::now();
        chunk->Execute();
        g.busy_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(),
                            std::memory_order_relaxed);
        g.chunks.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lk{g.m};
            g.free.push_back(std::move(chunk));
            g.busy = false;
            if (g.queue.empty()) {
                g.idle_cv.notify_all();
            }
        }
    }
}

template <typename Func>
void Push(Func&& func) {
    g.commands.fetch_add(1, std::memory_order_relaxed);
    if (!g.cur->Push(func)) {
        g.full.push_back(std::move(g.cur));
        g.cur = AcquireChunk();
        const bool pushed = g.cur->Push(func);
        (void)pushed;
    }
    Kick(false);
}

/// Room for `bytes` of arrays plus the command using them in the current chunk (both must share it); false when the
/// arrays are too large for a chunk (the command is then recorded directly).
bool Reserve(size_t bytes) {
    if (bytes + 2048 > Chunk::kCapacity) {
        return false;
    }
    if (g.cur->Room() < bytes + 2048) {
        g.full.push_back(std::move(g.cur));
        g.cur = AcquireChunk();
    }
    return true;
}

template <typename T>
T* Copy(const T* src, u32 count) {
    if (src == nullptr || count == 0) {
        return nullptr;
    }
    auto* dst = static_cast<T*>(g.cur->Allocate(sizeof(T) * count, alignof(T)));
    std::memcpy(dst, src, sizeof(T) * count);
    g.data_bytes.fetch_add(sizeof(T) * count, std::memory_order_relaxed);
    return dst;
}

template <typename T>
size_t Bytes(u32 count) {
    return (sizeof(T) + alignof(T)) * count;
}

// ---- generic thunks: functions that wait for the recorder, and commands with value arguments only ----

template <auto M, typename PFN>
struct SyncThunk;
template <auto M, typename R, typename... A>
struct SyncThunk<M, R(VKAPI_PTR*)(A...)> {
    template <SyncReason Reason>
    static R VKAPI_CALL Call(A... a) {
        SyncFor(Reason);
        return (g.real->*M)(a...);
    }
};

template <auto M, typename PFN>
struct DeferThunk;
template <auto M, typename... A>
struct DeferThunk<M, void(VKAPI_PTR*)(A...)> {
    static void VKAPI_CALL Call(A... a) {
        Push([=] { (g.real->*M)(a...); });
    }
};

/// A command recorded directly on the producer after the recorder caught up (arguments this port does not copy).
#define SSPC_DIRECT(name, ...)                                                                                         \
    do {                                                                                                               \
        g.fallbacks.fetch_add(1, std::memory_order_relaxed);                                                           \
        SyncFor(kFallback);                                                                                            \
        g.real->name(__VA_ARGS__);                                                                                     \
        return;                                                                                                        \
    } while (0)

// ---- commands with arrays: the arrays copied into the chunk ----

void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    bool chained = d->pNext != nullptr;
    for (u32 i = 0; i < d->memoryBarrierCount; ++i) chained |= d->pMemoryBarriers[i].pNext != nullptr;
    for (u32 i = 0; i < d->bufferMemoryBarrierCount; ++i) chained |= d->pBufferMemoryBarriers[i].pNext != nullptr;
    for (u32 i = 0; i < d->imageMemoryBarrierCount; ++i) chained |= d->pImageMemoryBarriers[i].pNext != nullptr;
    if (chained || !Reserve(Bytes<VkMemoryBarrier2>(d->memoryBarrierCount) +
                            Bytes<VkBufferMemoryBarrier2>(d->bufferMemoryBarrierCount) +
                            Bytes<VkImageMemoryBarrier2>(d->imageMemoryBarrierCount))) {
        SSPC_DIRECT(vkCmdPipelineBarrier2, cb, d);
    }
    VkDependencyInfo c = *d;
    c.pMemoryBarriers = Copy(d->pMemoryBarriers, d->memoryBarrierCount);
    c.pBufferMemoryBarriers = Copy(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount);
    c.pImageMemoryBarriers = Copy(d->pImageMemoryBarriers, d->imageMemoryBarrierCount);
    Push([cb, c] { g.real->vkCmdPipelineBarrier2(cb, &c); });
}

void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* r) {
    bool chained = r->pNext != nullptr || (r->pDepthAttachment && r->pDepthAttachment->pNext) ||
                   (r->pStencilAttachment && r->pStencilAttachment->pNext);
    for (u32 i = 0; i < r->colorAttachmentCount; ++i) chained |= r->pColorAttachments[i].pNext != nullptr;
    if (chained || !Reserve(Bytes<VkRenderingAttachmentInfo>(r->colorAttachmentCount + 2))) {
        SSPC_DIRECT(vkCmdBeginRendering, cb, r);
    }
    VkRenderingInfo c = *r;
    c.pColorAttachments = Copy(r->pColorAttachments, r->colorAttachmentCount);
    // (depth and stencil may be the same attachment: copied once, the same pointer kept)
    VkRenderingAttachmentInfo* depth = Copy(r->pDepthAttachment, r->pDepthAttachment ? 1u : 0u);
    c.pDepthAttachment = depth;
    c.pStencilAttachment = r->pStencilAttachment == nullptr                   ? nullptr
                           : r->pStencilAttachment == r->pDepthAttachment ? depth
                                                                          : Copy(r->pStencilAttachment, 1u);
    Push([cb, c] { g.real->vkCmdBeginRendering(cb, &c); });
}

void VKAPI_CALL CmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 set,
                                        u32 count, const VkWriteDescriptorSet* w) {
    size_t bytes = Bytes<VkWriteDescriptorSet>(count);
    bool chained = false;
    for (u32 i = 0; i < count; ++i) {
        chained |= w[i].pNext != nullptr;
        bytes += w[i].pImageInfo ? Bytes<VkDescriptorImageInfo>(w[i].descriptorCount) : 0;
        bytes += w[i].pBufferInfo ? Bytes<VkDescriptorBufferInfo>(w[i].descriptorCount) : 0;
        bytes += w[i].pTexelBufferView ? Bytes<VkBufferView>(w[i].descriptorCount) : 0;
    }
    if (chained || !Reserve(bytes)) {
        SSPC_DIRECT(vkCmdPushDescriptorSetKHR, cb, bp, layout, set, count, w);
    }
    VkWriteDescriptorSet* c = Copy(w, count);
    for (u32 i = 0; i < count; ++i) {
        c[i].pImageInfo = Copy(w[i].pImageInfo, w[i].descriptorCount);
        c[i].pBufferInfo = Copy(w[i].pBufferInfo, w[i].descriptorCount);
        c[i].pTexelBufferView = Copy(w[i].pTexelBufferView, w[i].descriptorCount);
    }
    Push([=] { g.real->vkCmdPushDescriptorSetKHR(cb, bp, layout, set, count, c); });
}

void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first,
                                      u32 count, const VkDescriptorSet* sets, u32 dyn_count, const u32* dyn) {
    if (!Reserve(Bytes<VkDescriptorSet>(count) + Bytes<u32>(dyn_count))) {
        SSPC_DIRECT(vkCmdBindDescriptorSets, cb, bp, layout, first, count, sets, dyn_count, dyn);
    }
    const VkDescriptorSet* s = Copy(sets, count);
    const u32* d = Copy(dyn, dyn_count);
    Push([=] { g.real->vkCmdBindDescriptorSets(cb, bp, layout, first, count, s, dyn_count, d); });
}

void VKAPI_CALL CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset,
                                 u32 size, const void* values) {
    if (!Reserve(size + 16)) {
        SSPC_DIRECT(vkCmdPushConstants, cb, layout, stages, offset, size, values);
    }
    const void* v = Copy(static_cast<const std::byte*>(values), size);
    Push([=] { g.real->vkCmdPushConstants(cb, layout, stages, offset, size, v); });
}

void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size,
                                const void* data) {
    if (!Reserve(static_cast<size_t>(size) + 16)) {
        SSPC_DIRECT(vkCmdUpdateBuffer, cb, dst, offset, size, data);
    }
    const void* v = Copy(static_cast<const std::byte*>(data), static_cast<u32>(size));
    Push([=] { g.real->vkCmdUpdateBuffer(cb, dst, offset, size, v); });
}

void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 count, const VkBuffer* bufs,
                                     const VkDeviceSize* offs) {
    if (!Reserve(Bytes<VkBuffer>(count) + Bytes<VkDeviceSize>(count))) {
        SSPC_DIRECT(vkCmdBindVertexBuffers, cb, first, count, bufs, offs);
    }
    const VkBuffer* b = Copy(bufs, count);
    const VkDeviceSize* o = Copy(offs, count);
    Push([=] { g.real->vkCmdBindVertexBuffers(cb, first, count, b, o); });
}

void VKAPI_CALL CmdBindVertexBuffers2(VkCommandBuffer cb, u32 first, u32 count, const VkBuffer* bufs,
                                      const VkDeviceSize* offs, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (!Reserve(Bytes<VkBuffer>(count) + 3 * Bytes<VkDeviceSize>(count))) {
        SSPC_DIRECT(vkCmdBindVertexBuffers2, cb, first, count, bufs, offs, sizes, strides);
    }
    const VkBuffer* b = Copy(bufs, count);
    const VkDeviceSize* o = Copy(offs, count);
    const VkDeviceSize* s = Copy(sizes, count);
    const VkDeviceSize* st = Copy(strides, count);
    Push([=] { g.real->vkCmdBindVertexBuffers2(cb, first, count, b, o, s, st); });
}

void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer cb, u32 count, const VkViewport* v) {
    if (!Reserve(Bytes<VkViewport>(count))) {
        SSPC_DIRECT(vkCmdSetViewportWithCount, cb, count, v);
    }
    const VkViewport* c = Copy(v, count);
    Push([=] { g.real->vkCmdSetViewportWithCount(cb, count, c); });
}

void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer cb, u32 count, const VkRect2D* s) {
    if (!Reserve(Bytes<VkRect2D>(count))) {
        SSPC_DIRECT(vkCmdSetScissorWithCount, cb, count, s);
    }
    const VkRect2D* c = Copy(s, count);
    Push([=] { g.real->vkCmdSetScissorWithCount(cb, count, c); });
}

void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cb, const float constants[4]) {
    std::array<float, 4> c{constants[0], constants[1], constants[2], constants[3]};
    Push([cb, c] { g.real->vkCmdSetBlendConstants(cb, c.data()); });
}

void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cb, u32 count, const VkClearAttachment* a, u32 rects,
                                    const VkClearRect* r) {
    if (!Reserve(Bytes<VkClearAttachment>(count) + Bytes<VkClearRect>(rects))) {
        SSPC_DIRECT(vkCmdClearAttachments, cb, count, a, rects, r);
    }
    const VkClearAttachment* ca = Copy(a, count);
    const VkClearRect* cr = Copy(r, rects);
    Push([=] { g.real->vkCmdClearAttachments(cb, count, ca, rects, cr); });
}

void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 count, const VkBufferCopy* r) {
    if (!Reserve(Bytes<VkBufferCopy>(count))) {
        SSPC_DIRECT(vkCmdCopyBuffer, cb, src, dst, count, r);
    }
    const VkBufferCopy* c = Copy(r, count);
    Push([=] { g.real->vkCmdCopyBuffer(cb, src, dst, count, c); });
}

void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, u32 count,
                                     const VkBufferImageCopy* r) {
    if (!Reserve(Bytes<VkBufferImageCopy>(count))) {
        SSPC_DIRECT(vkCmdCopyBufferToImage, cb, src, dst, layout, count, r);
    }
    const VkBufferImageCopy* c = Copy(r, count);
    Push([=] { g.real->vkCmdCopyBufferToImage(cb, src, dst, layout, count, c); });
}

void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst, u32 count,
                                     const VkBufferImageCopy* r) {
    if (!Reserve(Bytes<VkBufferImageCopy>(count))) {
        SSPC_DIRECT(vkCmdCopyImageToBuffer, cb, src, layout, dst, count, r);
    }
    const VkBufferImageCopy* c = Copy(r, count);
    Push([=] { g.real->vkCmdCopyImageToBuffer(cb, src, layout, dst, count, c); });
}

void VKAPI_CALL CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                             u32 count, const VkImageCopy* r) {
    if (!Reserve(Bytes<VkImageCopy>(count))) {
        SSPC_DIRECT(vkCmdCopyImage, cb, src, sl, dst, dl, count, r);
    }
    const VkImageCopy* c = Copy(r, count);
    Push([=] { g.real->vkCmdCopyImage(cb, src, sl, dst, dl, count, c); });
}

void VKAPI_CALL CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                             u32 count, const VkImageBlit* r, VkFilter filter) {
    if (!Reserve(Bytes<VkImageBlit>(count))) {
        SSPC_DIRECT(vkCmdBlitImage, cb, src, sl, dst, dl, count, r, filter);
    }
    const VkImageBlit* c = Copy(r, count);
    Push([=] { g.real->vkCmdBlitImage(cb, src, sl, dst, dl, count, c, filter); });
}

void VKAPI_CALL CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                u32 count, const VkImageResolve* r) {
    if (!Reserve(Bytes<VkImageResolve>(count))) {
        SSPC_DIRECT(vkCmdResolveImage, cb, src, sl, dst, dl, count, r);
    }
    const VkImageResolve* c = Copy(r, count);
    Push([=] { g.real->vkCmdResolveImage(cb, src, sl, dst, dl, count, c); });
}

void VKAPI_CALL CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout, const VkClearColorValue* color,
                                   u32 count, const VkImageSubresourceRange* r) {
    if (!Reserve(Bytes<VkImageSubresourceRange>(count))) {
        SSPC_DIRECT(vkCmdClearColorImage, cb, img, layout, color, count, r);
    }
    const VkClearColorValue cv = *color;
    const VkImageSubresourceRange* c = Copy(r, count);
    Push([=] { g.real->vkCmdClearColorImage(cb, img, layout, &cv, count, c); });
}

void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout,
                                          const VkClearDepthStencilValue* value, u32 count,
                                          const VkImageSubresourceRange* r) {
    if (!Reserve(Bytes<VkImageSubresourceRange>(count))) {
        SSPC_DIRECT(vkCmdClearDepthStencilImage, cb, img, layout, value, count, r);
    }
    const VkClearDepthStencilValue v = *value;
    const VkImageSubresourceRange* c = Copy(r, count);
    Push([=] { g.real->vkCmdClearDepthStencilImage(cb, img, layout, &v, count, c); });
}

void VKAPI_CALL CmdSetColorWriteMaskEXT(VkCommandBuffer cb, u32 first, u32 count, const VkColorComponentFlags* m) {
    if (!Reserve(Bytes<VkColorComponentFlags>(count))) {
        SSPC_DIRECT(vkCmdSetColorWriteMaskEXT, cb, first, count, m);
    }
    const VkColorComponentFlags* c = Copy(m, count);
    Push([=] { g.real->vkCmdSetColorWriteMaskEXT(cb, first, count, c); });
}

void VKAPI_CALL CmdSetVertexInputEXT(VkCommandBuffer cb, u32 nb, const VkVertexInputBindingDescription2EXT* b, u32 na,
                                     const VkVertexInputAttributeDescription2EXT* a) {
    bool chained = false;
    for (u32 i = 0; i < nb; ++i) chained |= b[i].pNext != nullptr;
    for (u32 i = 0; i < na; ++i) chained |= a[i].pNext != nullptr;
    if (chained || !Reserve(Bytes<VkVertexInputBindingDescription2EXT>(nb) +
                            Bytes<VkVertexInputAttributeDescription2EXT>(na))) {
        SSPC_DIRECT(vkCmdSetVertexInputEXT, cb, nb, b, na, a);
    }
    const auto* cbind = Copy(b, nb);
    const auto* cattr = Copy(a, na);
    Push([=] { g.real->vkCmdSetVertexInputEXT(cb, nb, cbind, na, cattr); });
}

void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* l) {
    const size_t len = l->pLabelName ? std::strlen(l->pLabelName) + 1 : 0;
    if (l->pNext != nullptr || !Reserve(len + 16)) {
        SSPC_DIRECT(vkCmdBeginDebugUtilsLabelEXT, cb, l);
    }
    VkDebugUtilsLabelEXT c = *l;
    c.pLabelName = len ? Copy(l->pLabelName, static_cast<u32>(len)) : nullptr;
    Push([cb, c] { g.real->vkCmdBeginDebugUtilsLabelEXT(cb, &c); });
}

#undef SSPC_DIRECT

} // namespace

Api* ThreadTable() {
    return t_table;
}

bool Enabled() {
    return g.enabled;
}

void Configure() {
    if (g.real != nullptr) {
        return; // (once per process)
    }
    char v[8] = {};
    if (GetEnvironmentVariableA("SSPC_PORT_VK_RECORDER", v, sizeof(v)) == 0 || v[0] != '1') {
        return;
    }
    g.real = &Fn(); // (the calling thread is not attached: the process's table)
    Api& t = g.table;
    t = *g.real;
    // functions that wait until every deferred command is recorded
#define SSPC_SYNC(name, reason)                                                                                        \
    if (t.name != nullptr) {                                                                                           \
        t.name = &SyncThunk<&Api::name, PFN_##name>::template Call<reason>;                                           \
    }
    SSPC_SYNC(vkQueueSubmit2, kSubmit)
    SSPC_SYNC(vkQueueWaitIdle, kSubmit)
    SSPC_SYNC(vkDeviceWaitIdle, kSubmit)
    SSPC_SYNC(vkQueuePresentKHR, kSubmit)
    SSPC_SYNC(vkBeginCommandBuffer, kCommandBuffer)
    SSPC_SYNC(vkEndCommandBuffer, kCommandBuffer)
    SSPC_SYNC(vkResetCommandBuffer, kCommandBuffer)
    SSPC_SYNC(vkResetCommandPool, kCommandBuffer)
    SSPC_SYNC(vkAllocateCommandBuffers, kCommandBuffer)
    SSPC_SYNC(vkFreeCommandBuffers, kCommandBuffer)
    SSPC_SYNC(vkDestroyCommandPool, kCommandBuffer)
    SSPC_SYNC(vkDestroyDevice, kDestroy)
    SSPC_SYNC(vkDestroyPipelineCache, kDestroy)
    SSPC_SYNC(vkDestroyFence, kDestroy)
    SSPC_SYNC(vkDestroySemaphore, kDestroy)
    SSPC_SYNC(vkDestroyQueryPool, kDestroy)
    SSPC_SYNC(vkDestroyBuffer, kDestroy)
    SSPC_SYNC(vkDestroyImage, kDestroy)
    SSPC_SYNC(vkFreeMemory, kDestroy)
    SSPC_SYNC(vkDestroyImageView, kDestroy)
    SSPC_SYNC(vkDestroySampler, kDestroy)
    SSPC_SYNC(vkDestroyShaderModule, kDestroy)
    SSPC_SYNC(vkDestroyDescriptorSetLayout, kDestroy)
    SSPC_SYNC(vkDestroyDescriptorPool, kDestroy)
    SSPC_SYNC(vkResetDescriptorPool, kDestroy)
    SSPC_SYNC(vkDestroyPipelineLayout, kDestroy)
    SSPC_SYNC(vkDestroyPipeline, kDestroy)
    SSPC_SYNC(vkDestroySwapchainKHR, kDestroy)
#undef SSPC_SYNC
    // commands with value arguments only
#define SSPC_DEFER(name)                                                                                               \
    if (t.name != nullptr) {                                                                                           \
        t.name = &DeferThunk<&Api::name, PFN_##name>::Call;                                                            \
    }
    SSPC_DEFER(vkCmdResetQueryPool)
    SSPC_DEFER(vkCmdWriteTimestamp2)
    SSPC_DEFER(vkCmdEndRendering)
    SSPC_DEFER(vkCmdBindPipeline)
    SSPC_DEFER(vkCmdBindIndexBuffer)
    SSPC_DEFER(vkCmdDraw)
    SSPC_DEFER(vkCmdDrawIndexed)
    SSPC_DEFER(vkCmdDrawIndirect)
    SSPC_DEFER(vkCmdDrawIndexedIndirect)
    SSPC_DEFER(vkCmdDispatch)
    SSPC_DEFER(vkCmdDispatchIndirect)
    SSPC_DEFER(vkCmdSetDepthTestEnable)
    SSPC_DEFER(vkCmdSetDepthWriteEnable)
    SSPC_DEFER(vkCmdSetDepthCompareOp)
    SSPC_DEFER(vkCmdSetDepthBiasEnable)
    SSPC_DEFER(vkCmdSetDepthBias)
    SSPC_DEFER(vkCmdSetDepthBoundsTestEnable)
    SSPC_DEFER(vkCmdSetDepthBounds)
    SSPC_DEFER(vkCmdSetStencilTestEnable)
    SSPC_DEFER(vkCmdSetStencilOp)
    SSPC_DEFER(vkCmdSetStencilReference)
    SSPC_DEFER(vkCmdSetStencilCompareMask)
    SSPC_DEFER(vkCmdSetStencilWriteMask)
    SSPC_DEFER(vkCmdSetCullMode)
    SSPC_DEFER(vkCmdSetFrontFace)
    SSPC_DEFER(vkCmdSetRasterizerDiscardEnable)
    SSPC_DEFER(vkCmdSetLineWidth)
    SSPC_DEFER(vkCmdSetPrimitiveRestartEnable)
    SSPC_DEFER(vkCmdFillBuffer)
    SSPC_DEFER(vkCmdEndDebugUtilsLabelEXT)
    SSPC_DEFER(vkCmdSetCheckpointNV)
    SSPC_DEFER(vkCmdWriteBufferMarkerAMD)
#undef SSPC_DEFER
    // commands with arrays
#define SSPC_ARRAYS(name, fn)                                                                                          \
    if (t.name != nullptr) {                                                                                           \
        t.name = &fn;                                                                                                  \
    }
    SSPC_ARRAYS(vkCmdPipelineBarrier2, CmdPipelineBarrier2)
    SSPC_ARRAYS(vkCmdBeginRendering, CmdBeginRendering)
    SSPC_ARRAYS(vkCmdPushDescriptorSetKHR, CmdPushDescriptorSetKHR)
    SSPC_ARRAYS(vkCmdBindDescriptorSets, CmdBindDescriptorSets)
    SSPC_ARRAYS(vkCmdPushConstants, CmdPushConstants)
    SSPC_ARRAYS(vkCmdUpdateBuffer, CmdUpdateBuffer)
    SSPC_ARRAYS(vkCmdBindVertexBuffers, CmdBindVertexBuffers)
    SSPC_ARRAYS(vkCmdBindVertexBuffers2, CmdBindVertexBuffers2)
    SSPC_ARRAYS(vkCmdSetViewportWithCount, CmdSetViewportWithCount)
    SSPC_ARRAYS(vkCmdSetScissorWithCount, CmdSetScissorWithCount)
    SSPC_ARRAYS(vkCmdSetBlendConstants, CmdSetBlendConstants)
    SSPC_ARRAYS(vkCmdClearAttachments, CmdClearAttachments)
    SSPC_ARRAYS(vkCmdCopyBuffer, CmdCopyBuffer)
    SSPC_ARRAYS(vkCmdCopyBufferToImage, CmdCopyBufferToImage)
    SSPC_ARRAYS(vkCmdCopyImageToBuffer, CmdCopyImageToBuffer)
    SSPC_ARRAYS(vkCmdCopyImage, CmdCopyImage)
    SSPC_ARRAYS(vkCmdBlitImage, CmdBlitImage)
    SSPC_ARRAYS(vkCmdResolveImage, CmdResolveImage)
    SSPC_ARRAYS(vkCmdClearColorImage, CmdClearColorImage)
    SSPC_ARRAYS(vkCmdClearDepthStencilImage, CmdClearDepthStencilImage)
    SSPC_ARRAYS(vkCmdSetColorWriteMaskEXT, CmdSetColorWriteMaskEXT)
    SSPC_ARRAYS(vkCmdSetVertexInputEXT, CmdSetVertexInputEXT)
    SSPC_ARRAYS(vkCmdBeginDebugUtilsLabelEXT, CmdBeginDebugUtilsLabelEXT)
#undef SSPC_ARRAYS
    g.cur = AcquireChunk();
    g.stop = false;
    g.thread = std::thread(RecorderLoop);
    g.enabled = true;
    SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_PORT_VK_RECORDER=1 - Vulkan commands recorded on a recorder thread "
                           "(port of uncharted_pc ThreadedRecording)");
}

void AttachThisThread() {
    if (g.enabled) {
        t_table = &g.table;
    }
}

void DetachThisThread() {
    if (t_table != nullptr) {
        SyncFor(kExplicit);
        t_table = nullptr;
    }
}

void Sync() {
    SyncFor(kExplicit);
}

void HostCopy(void* dst, const void* src, size_t bytes) {
    if (t_table == nullptr) {
        std::memcpy(dst, src, bytes);
        return;
    }
    g.host_copies.fetch_add(1, std::memory_order_relaxed);
    g.host_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    Push([dst, src, bytes] { std::memcpy(dst, src, bytes); });
}

void Shutdown() {
    if (!g.enabled) {
        return;
    }
    SyncFor(kExplicit);
    {
        std::scoped_lock lk{g.m};
        g.stop = true;
    }
    g.cv.notify_all();
    if (g.thread.joinable()) {
        g.thread.join();
    }
    g.enabled = false;
    SSPC_LOG(GPU, Info, "{}", Report());
}

std::string Report() {
    std::string syncs;
    for (u32 r = 0; r < kReasons; ++r) {
        syncs += fmt::format("{}{} {}", r ? ", " : "", kReasonNames[r], g.syncs[r].load());
    }
    return fmt::format("deferred recording (SSPC_PORT_VK_RECORDER): commands {}, array bytes {} MiB, kicks {}, chunks "
                       "{}, recorder busy {} ms; syncs: {} (waited {}, {} ms); direct commands {}; host copies {} ({} MiB)",
                       g.commands.load(), g.data_bytes.load() >> 20, g.kicks.load(), g.chunks.load(),
                       g.busy_ns.load() / 1'000'000, syncs, g.sync_waited.load(), g.sync_ns.load() / 1'000'000,
                       g.fallbacks.load(), g.host_copies.load(), g.host_copy_bytes.load() >> 20);
}

} // namespace SecondSon::Gpu::Vk::Deferred
