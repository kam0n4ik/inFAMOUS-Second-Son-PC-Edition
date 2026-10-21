// SecondSonPC — the emitter thread (see vk_emit.h).
#include "runtime/gpu/vulkan/vk_emit.h"

#include <atomic>
#include <cstring>
#include <new>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <intrin.h>
#include <windows.h>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk::Emit {

namespace {

constexpr u64 kRingBytes = 16ull << 20; ///< power of two
constexpr u64 kHeader = 16;

using ReplayFn = void (*)(const u8* payload);

/// An entry: the replay function (nullptr: padding to the ring's end) and the entry's size with the header.
struct Header {
    ReplayFn fn;
    u64 bytes;
};
static_assert(sizeof(Header) == kHeader);

Api g_real; ///< the driver's functions (the table as loaded)
bool g_on = false;
u8* g_ring = nullptr;
thread_local bool t_producer = false;
thread_local u64 t_last = 0; ///< the end of this producer's last entry

alignas(64) std::atomic_flag g_lock;      ///< the producers' ring (two of them may record at once)
alignas(64) u64 g_wlocal = 0;             ///< under g_lock
alignas(64) std::atomic<u64> g_w{0};      ///< published to the emitter
alignas(64) std::atomic<u64> g_r{0};      ///< passed to the driver
alignas(64) std::atomic<u32> g_sleeping{0};

std::atomic<u64> g_deferred{0}, g_syncs{0}, g_sync_waits{0}, g_sync_ticks{0}, g_full_waits{0}, g_fallbacks{0};

void Bump(std::atomic<u64>& a, u64 v = 1) {
    a.fetch_add(v, std::memory_order_relaxed);
}

/// Waits until the emitter passed position `w` to the driver.
void WaitEmitted(u64 w) {
    if (g_r.load(std::memory_order_acquire) >= w) {
        return;
    }
    const u64 t0 = __rdtsc();
    u32 spins = 0;
    while (g_r.load(std::memory_order_acquire) < w) {
        if (++spins < 4096) {
            _mm_pause();
        } else {
            SwitchToThread();
        }
    }
    Bump(g_sync_waits);
    Bump(g_sync_ticks, __rdtsc() - t0);
}

/// A producer's entry of `payload` bytes for `fn` (16-aligned); Commit publishes it.
u8* Begin(ReplayFn fn, u64 payload) {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
        _mm_pause();
    }
    const u64 total = (kHeader + payload + 15) & ~u64{15};
    u64 pos = g_wlocal;
    const u64 off = pos & (kRingBytes - 1);
    const u64 pad = off + total > kRingBytes ? kRingBytes - off : 0;
    if (pos + pad + total - g_r.load(std::memory_order_acquire) > kRingBytes) {
        Bump(g_full_waits);
        while (pos + pad + total - g_r.load(std::memory_order_acquire) > kRingBytes) {
            _mm_pause();
        }
    }
    if (pad != 0) {
        *reinterpret_cast<Header*>(g_ring + off) = {nullptr, pad};
        pos += pad;
    }
    u8* h = g_ring + (pos & (kRingBytes - 1));
    *reinterpret_cast<Header*>(h) = {fn, total};
    g_wlocal = pos + total;
    return h + kHeader;
}

void Commit() {
    t_last = g_wlocal;
    g_w.store(t_last, std::memory_order_seq_cst);
    g_lock.clear(std::memory_order_release);
    if (g_sleeping.load(std::memory_order_seq_cst) != 0) {
        g_w.notify_one();
    }
    Bump(g_deferred);
}

/// A non-producer's call that may order with the producers' commands (a destroy): every published one passed on.
void FullSync() {
    WaitEmitted(g_w.load(std::memory_order_acquire));
}

void Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuEmit");
    u64 r = 0;
    for (;;) {
        u64 w = g_w.load(std::memory_order_acquire);
        if (w == r) {
            const u64 t0 = __rdtsc();
            while (w == r && __rdtsc() - t0 < 2'000'000) { // (~0.5 ms: hot while a frame is being recorded)
                _mm_pause();
                w = g_w.load(std::memory_order_acquire);
            }
            while (w == r) {
                g_sleeping.store(1, std::memory_order_seq_cst);
                w = g_w.load(std::memory_order_seq_cst);
                if (w == r) {
                    g_w.wait(r, std::memory_order_seq_cst);
                }
                g_sleeping.store(0, std::memory_order_relaxed);
                w = g_w.load(std::memory_order_acquire);
            }
        }
        while (r < w) {
            const u8* e = g_ring + (r & (kRingBytes - 1));
            const Header& h = *reinterpret_cast<const Header*>(e);
            if (h.fn != nullptr) {
                h.fn(e + kHeader);
            }
            r += h.bytes;
            g_r.store(r, std::memory_order_release);
        }
    }
}

// ---- thunks (a non-producer's command goes to the driver at once: its command buffers are its own)

/// Deferred: a command whose arguments are all values.
template <auto M, class Sig>
struct Scalar;
template <auto M, class... A>
struct Scalar<M, void(VKAPI_PTR*)(A...)> {
    using Args = std::tuple<A...>;
    static void Replay(const u8* p) {
        std::apply(g_real.*M, *reinterpret_cast<const Args*>(p));
    }
    static void VKAPI_CALL Call(A... a) {
        if (!t_producer) {
            (g_real.*M)(a...);
            return;
        }
        new (Begin(&Replay, sizeof(Args))) Args(a...);
        Commit();
    }
};

/// Ordered with the producer's commands: its entries passed on first.
template <auto M, class Sig>
struct Synced;
template <auto M, class R, class... A>
struct Synced<M, R(VKAPI_PTR*)(A...)> {
    static R VKAPI_CALL Call(A... a) {
        Sync();
        return (g_real.*M)(a...);
    }
};

/// A destroy / free / reset: an object the ring may still name - from any thread, the ring passed on first.
template <auto M, class Sig>
struct Ordered;
template <auto M, class R, class... A>
struct Ordered<M, R(VKAPI_PTR*)(A...)> {
    static R VKAPI_CALL Call(A... a) {
        t_producer ? Sync() : FullSync();
        return (g_real.*M)(a...);
    }
};

/// A payload writer / reader of the commands with arrays (8-aligned pieces).
struct Out {
    u8* p;
    template <class T>
    void Put(const T& v) {
        std::memcpy(p, &v, sizeof(T));
        p += (sizeof(T) + 7) & ~size_t{7};
    }
    template <class T>
    void Arr(const T* a, u64 n) {
        if (n != 0) {
            std::memcpy(p, a, sizeof(T) * n);
        }
        p += (sizeof(T) * n + 7) & ~u64{7};
    }
};
struct In {
    const u8* p;
    template <class T>
    T Get() {
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += (sizeof(T) + 7) & ~size_t{7};
        return v;
    }
    template <class T>
    const T* Arr(u64 n) {
        const T* a = reinterpret_cast<const T*>(p);
        p += (sizeof(T) * n + 7) & ~u64{7};
        return a;
    }
};
template <class T>
constexpr u64 Sz(u64 n = 1) {
    return (sizeof(T) * n + 7) & ~u64{7};
}

// vkCmdPushConstants
void ReplayPushConstants(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto layout = in.Get<VkPipelineLayout>();
    const auto stages = in.Get<VkShaderStageFlags>();
    const auto offset = in.Get<u32>();
    const auto size = in.Get<u32>();
    g_real.vkCmdPushConstants(cb, layout, stages, offset, size, in.Arr<u8>(size));
}
void VKAPI_CALL PushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset,
                              u32 size, const void* values) {
    if (!t_producer) {
        g_real.vkCmdPushConstants(cb, layout, stages, offset, size, values);
        return;
    }
    Out o{Begin(&ReplayPushConstants, Sz<VkCommandBuffer>() + Sz<VkPipelineLayout>() + Sz<VkShaderStageFlags>() +
                                          2 * Sz<u32>() + Sz<u8>(size))};
    o.Put(cb), o.Put(layout), o.Put(stages), o.Put(offset), o.Put(size), o.Arr(static_cast<const u8*>(values), size);
    Commit();
}

// vkCmdBindVertexBuffers
void ReplayBindVertexBuffers(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto first = in.Get<u32>();
    const auto n = in.Get<u32>();
    const VkBuffer* b = in.Arr<VkBuffer>(n);
    g_real.vkCmdBindVertexBuffers(cb, first, n, b, in.Arr<VkDeviceSize>(n));
}
void VKAPI_CALL BindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* buffers,
                                  const VkDeviceSize* offsets) {
    if (!t_producer) {
        g_real.vkCmdBindVertexBuffers(cb, first, n, buffers, offsets);
        return;
    }
    Out o{Begin(&ReplayBindVertexBuffers,
                Sz<VkCommandBuffer>() + 2 * Sz<u32>() + Sz<VkBuffer>(n) + Sz<VkDeviceSize>(n))};
    o.Put(cb), o.Put(first), o.Put(n), o.Arr(buffers, n), o.Arr(offsets, n);
    Commit();
}

// vkCmdBindDescriptorSets
void ReplayBindDescriptorSets(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto bp = in.Get<VkPipelineBindPoint>();
    const auto layout = in.Get<VkPipelineLayout>();
    const auto first = in.Get<u32>();
    const auto n = in.Get<u32>();
    const auto nd = in.Get<u32>();
    const VkDescriptorSet* sets = in.Arr<VkDescriptorSet>(n);
    g_real.vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, in.Arr<u32>(nd));
}
void VKAPI_CALL BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first,
                                   u32 n, const VkDescriptorSet* sets, u32 nd, const u32* dyn) {
    if (!t_producer) {
        g_real.vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn);
        return;
    }
    Out o{Begin(&ReplayBindDescriptorSets, Sz<VkCommandBuffer>() + Sz<VkPipelineBindPoint>() +
                                               Sz<VkPipelineLayout>() + 3 * Sz<u32>() + Sz<VkDescriptorSet>(n) +
                                               Sz<u32>(nd))};
    o.Put(cb), o.Put(bp), o.Put(layout), o.Put(first), o.Put(n), o.Put(nd), o.Arr(sets, n), o.Arr(dyn, nd);
    Commit();
}

// vkCmdSetViewportWithCount / vkCmdSetScissorWithCount
template <class T, auto M>
void ReplayCountArray(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto n = in.Get<u32>();
    (g_real.*M)(cb, n, in.Arr<T>(n));
}
template <class T, auto M>
void VKAPI_CALL CountArray(VkCommandBuffer cb, u32 n, const T* a) {
    if (!t_producer) {
        (g_real.*M)(cb, n, a);
        return;
    }
    Out o{Begin(&ReplayCountArray<T, M>, Sz<VkCommandBuffer>() + Sz<u32>() + Sz<T>(n))};
    o.Put(cb), o.Put(n), o.Arr(a, n);
    Commit();
}

// vkCmdSetColorWriteMaskEXT
void ReplayColorWriteMask(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto first = in.Get<u32>();
    const auto n = in.Get<u32>();
    g_real.vkCmdSetColorWriteMaskEXT(cb, first, n, in.Arr<VkColorComponentFlags>(n));
}
void VKAPI_CALL ColorWriteMask(VkCommandBuffer cb, u32 first, u32 n, const VkColorComponentFlags* masks) {
    if (!t_producer) {
        g_real.vkCmdSetColorWriteMaskEXT(cb, first, n, masks);
        return;
    }
    Out o{Begin(&ReplayColorWriteMask, Sz<VkCommandBuffer>() + 2 * Sz<u32>() + Sz<VkColorComponentFlags>(n))};
    o.Put(cb), o.Put(first), o.Put(n), o.Arr(masks, n);
    Commit();
}

// vkCmdSetBlendConstants
void ReplayBlendConstants(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    g_real.vkCmdSetBlendConstants(cb, in.Arr<float>(4));
}
void VKAPI_CALL BlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!t_producer) {
        g_real.vkCmdSetBlendConstants(cb, c);
        return;
    }
    Out o{Begin(&ReplayBlendConstants, Sz<VkCommandBuffer>() + Sz<float>(4))};
    o.Put(cb), o.Arr(c, 4);
    Commit();
}

// vkCmdSetVertexInputEXT (copied as they are: with a description's pNext, called at once)
void ReplayVertexInput(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto nb = in.Get<u32>();
    const auto na = in.Get<u32>();
    const auto* b = in.Arr<VkVertexInputBindingDescription2EXT>(nb);
    g_real.vkCmdSetVertexInputEXT(cb, nb, b, na, in.Arr<VkVertexInputAttributeDescription2EXT>(na));
}
void VKAPI_CALL VertexInput(VkCommandBuffer cb, u32 nb, const VkVertexInputBindingDescription2EXT* b, u32 na,
                            const VkVertexInputAttributeDescription2EXT* a) {
    if (!t_producer) {
        g_real.vkCmdSetVertexInputEXT(cb, nb, b, na, a);
        return;
    }
    bool plain = true;
    for (u32 i = 0; i < nb && plain; ++i) {
        plain = b[i].pNext == nullptr;
    }
    for (u32 i = 0; i < na && plain; ++i) {
        plain = a[i].pNext == nullptr;
    }
    if (!plain) {
        Bump(g_fallbacks);
        Sync();
        g_real.vkCmdSetVertexInputEXT(cb, nb, b, na, a);
        return;
    }
    Out o{Begin(&ReplayVertexInput, Sz<VkCommandBuffer>() + 2 * Sz<u32>() +
                                        Sz<VkVertexInputBindingDescription2EXT>(nb) +
                                        Sz<VkVertexInputAttributeDescription2EXT>(na))};
    o.Put(cb), o.Put(nb), o.Put(na), o.Arr(b, nb), o.Arr(a, na);
    Commit();
}

// vkCmdDrawMultiIndexedEXT (the infos repacked at their own size)
void ReplayMultiIndexed(const u8* p) {
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto n = in.Get<u32>();
    const auto instances = in.Get<u32>();
    const auto first = in.Get<u32>();
    const auto has_offset = in.Get<u32>();
    const auto offset = in.Get<s32>();
    g_real.vkCmdDrawMultiIndexedEXT(cb, n, in.Arr<VkMultiDrawIndexedInfoEXT>(n), instances, first,
                                    sizeof(VkMultiDrawIndexedInfoEXT), has_offset != 0 ? &offset : nullptr);
}
void VKAPI_CALL MultiIndexed(VkCommandBuffer cb, u32 n, const VkMultiDrawIndexedInfoEXT* infos, u32 instances,
                             u32 first, u32 stride, const s32* vertex_offset) {
    if (!t_producer) {
        g_real.vkCmdDrawMultiIndexedEXT(cb, n, infos, instances, first, stride, vertex_offset);
        return;
    }
    Out o{Begin(&ReplayMultiIndexed, Sz<VkCommandBuffer>() + 5 * Sz<u32>() + Sz<VkMultiDrawIndexedInfoEXT>(n))};
    o.Put(cb), o.Put(n), o.Put(instances), o.Put(first), o.Put(u32{vertex_offset != nullptr ? 1u : 0u}),
        o.Put(vertex_offset != nullptr ? *vertex_offset : 0);
    for (u32 i = 0; i < n; ++i) {
        std::memcpy(o.p + u64{i} * sizeof(VkMultiDrawIndexedInfoEXT),
                    reinterpret_cast<const u8*>(infos) + u64{i} * stride, sizeof(VkMultiDrawIndexedInfoEXT));
    }
    Commit();
}

// vkCmdPushDescriptorSetKHR: each write's infos copied after it
enum class InfoKind : u32 { Image, Buffer, View, Other };
InfoKind KindOf(VkDescriptorType t) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return InfoKind::Image;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return InfoKind::Buffer;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return InfoKind::View;
    default:
        return InfoKind::Other;
    }
}
struct WriteHead {
    VkDescriptorType type;
    u32 binding, element, count;
};
u64 InfoBytes(InfoKind k, u32 n) {
    return k == InfoKind::Image    ? Sz<VkDescriptorImageInfo>(n)
           : k == InfoKind::Buffer ? Sz<VkDescriptorBufferInfo>(n)
                                   : Sz<VkBufferView>(n);
}
void ReplayPushDescriptors(const u8* p) {
    thread_local std::vector<VkWriteDescriptorSet> writes;
    In in{p};
    const auto cb = in.Get<VkCommandBuffer>();
    const auto bp = in.Get<VkPipelineBindPoint>();
    const auto layout = in.Get<VkPipelineLayout>();
    const auto set = in.Get<u32>();
    const auto n = in.Get<u32>();
    writes.resize(n);
    for (u32 i = 0; i < n; ++i) {
        const WriteHead h = in.Get<WriteHead>();
        VkWriteDescriptorSet& w = writes[i];
        w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstBinding = h.binding,
             .dstArrayElement = h.element,
             .descriptorCount = h.count,
             .descriptorType = h.type};
        switch (KindOf(h.type)) {
        case InfoKind::Image:
            w.pImageInfo = in.Arr<VkDescriptorImageInfo>(h.count);
            break;
        case InfoKind::Buffer:
            w.pBufferInfo = in.Arr<VkDescriptorBufferInfo>(h.count);
            break;
        default:
            w.pTexelBufferView = in.Arr<VkBufferView>(h.count);
            break;
        }
    }
    g_real.vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, writes.data());
}
void VKAPI_CALL PushDescriptors(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 set, u32 n,
                                const VkWriteDescriptorSet* w) {
    if (!t_producer) {
        g_real.vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w);
        return;
    }
    u64 bytes = Sz<VkCommandBuffer>() + Sz<VkPipelineBindPoint>() + Sz<VkPipelineLayout>() + 2 * Sz<u32>();
    bool plain = true;
    for (u32 i = 0; i < n && plain; ++i) {
        const InfoKind k = KindOf(w[i].descriptorType);
        plain = w[i].pNext == nullptr && k != InfoKind::Other;
        bytes += Sz<WriteHead>() + InfoBytes(k, w[i].descriptorCount);
    }
    if (!plain) {
        Bump(g_fallbacks);
        Sync();
        g_real.vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w);
        return;
    }
    Out o{Begin(&ReplayPushDescriptors, bytes)};
    o.Put(cb), o.Put(bp), o.Put(layout), o.Put(set), o.Put(n);
    for (u32 i = 0; i < n; ++i) {
        o.Put(WriteHead{w[i].descriptorType, w[i].dstBinding, w[i].dstArrayElement, w[i].descriptorCount});
        switch (KindOf(w[i].descriptorType)) {
        case InfoKind::Image:
            o.Arr(w[i].pImageInfo, w[i].descriptorCount);
            break;
        case InfoKind::Buffer:
            o.Arr(w[i].pBufferInfo, w[i].descriptorCount);
            break;
        default:
            o.Arr(w[i].pTexelBufferView, w[i].descriptorCount);
            break;
        }
    }
    Commit();
}

} // namespace

void Sync() {
    if (!g_on || !t_producer) {
        return;
    }
    Bump(g_syncs);
    WaitEmitted(t_last);
}

void SetProducerThread() {
    t_producer = true;
}

bool Install(Api& f) {
    char e[8] = {};
    if (GetEnvironmentVariableA("SSPC_VK_EMIT_THREAD", e, sizeof(e)) != 1 || e[0] != '1') {
        return false;
    }
    g_ring = static_cast<u8*>(VirtualAlloc(nullptr, kRingBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_ring == nullptr) {
        SSPC_LOG(GPU, Warning, "emitter thread: no memory for its ring - not used");
        return false;
    }
    g_real = f;
    // the per-draw commands: deferred
#define SSPC_EMIT_SCALAR(name)                                                                                         \
    if (f.name != nullptr) {                                                                                           \
        f.name = &Scalar<&Api::name, PFN_##name>::Call;                                                                \
    }
    SSPC_EMIT_SCALAR(vkCmdDrawIndexed)
    SSPC_EMIT_SCALAR(vkCmdDraw)
    SSPC_EMIT_SCALAR(vkCmdBindPipeline)
    SSPC_EMIT_SCALAR(vkCmdBindIndexBuffer)
    SSPC_EMIT_SCALAR(vkCmdSetDepthTestEnable)
    SSPC_EMIT_SCALAR(vkCmdSetDepthWriteEnable)
    SSPC_EMIT_SCALAR(vkCmdSetDepthCompareOp)
    SSPC_EMIT_SCALAR(vkCmdSetDepthBiasEnable)
    SSPC_EMIT_SCALAR(vkCmdSetDepthBias)
    SSPC_EMIT_SCALAR(vkCmdSetDepthBoundsTestEnable)
    SSPC_EMIT_SCALAR(vkCmdSetDepthBounds)
    SSPC_EMIT_SCALAR(vkCmdSetStencilTestEnable)
    SSPC_EMIT_SCALAR(vkCmdSetStencilOp)
    SSPC_EMIT_SCALAR(vkCmdSetStencilReference)
    SSPC_EMIT_SCALAR(vkCmdSetStencilCompareMask)
    SSPC_EMIT_SCALAR(vkCmdSetStencilWriteMask)
    SSPC_EMIT_SCALAR(vkCmdSetCullMode)
    SSPC_EMIT_SCALAR(vkCmdSetFrontFace)
    SSPC_EMIT_SCALAR(vkCmdSetRasterizerDiscardEnable)
    SSPC_EMIT_SCALAR(vkCmdSetLineWidth)
    SSPC_EMIT_SCALAR(vkCmdSetPrimitiveRestartEnable)
#undef SSPC_EMIT_SCALAR
    f.vkCmdPushConstants = &PushConstants;
    f.vkCmdBindVertexBuffers = &BindVertexBuffers;
    f.vkCmdBindDescriptorSets = &BindDescriptorSets;
    f.vkCmdPushDescriptorSetKHR = &PushDescriptors;
    f.vkCmdSetViewportWithCount = &CountArray<VkViewport, &Api::vkCmdSetViewportWithCount>;
    f.vkCmdSetScissorWithCount = &CountArray<VkRect2D, &Api::vkCmdSetScissorWithCount>;
    f.vkCmdSetBlendConstants = &BlendConstants;
    if (f.vkCmdSetColorWriteMaskEXT != nullptr) {
        f.vkCmdSetColorWriteMaskEXT = &ColorWriteMask;
    }
    if (f.vkCmdSetVertexInputEXT != nullptr) {
        f.vkCmdSetVertexInputEXT = &VertexInput;
    }
    if (f.vkCmdDrawMultiIndexedEXT != nullptr) {
        f.vkCmdDrawMultiIndexedEXT = &MultiIndexed;
    }
    // every other command of a producer, and what orders with its command buffers: its entries passed on first
#define SSPC_EMIT_SYNC(name)                                                                                           \
    if (f.name != nullptr) {                                                                                           \
        f.name = &Synced<&Api::name, PFN_##name>::Call;                                                                \
    }
    SSPC_EMIT_SYNC(vkQueueWaitIdle)
    SSPC_EMIT_SYNC(vkQueueSubmit2)
    SSPC_EMIT_SYNC(vkQueueBindSparse)
    SSPC_EMIT_SYNC(vkAllocateCommandBuffers)
    SSPC_EMIT_SYNC(vkBeginCommandBuffer)
    SSPC_EMIT_SYNC(vkEndCommandBuffer)
    SSPC_EMIT_SYNC(vkCmdResetQueryPool)
    SSPC_EMIT_SYNC(vkCmdBeginQuery)
    SSPC_EMIT_SYNC(vkCmdEndQuery)
    SSPC_EMIT_SYNC(vkCmdCopyQueryPoolResults)
    SSPC_EMIT_SYNC(vkCmdWriteTimestamp2)
    SSPC_EMIT_SYNC(vkCmdClearAttachments)
    SSPC_EMIT_SYNC(vkCmdPipelineBarrier2)
    SSPC_EMIT_SYNC(vkCmdBeginRendering)
    SSPC_EMIT_SYNC(vkCmdEndRendering)
    SSPC_EMIT_SYNC(vkCmdBindVertexBuffers2)
    SSPC_EMIT_SYNC(vkCmdDrawIndirect)
    SSPC_EMIT_SYNC(vkCmdDrawIndexedIndirect)
    SSPC_EMIT_SYNC(vkCmdDispatch)
    SSPC_EMIT_SYNC(vkCmdDispatchIndirect)
    SSPC_EMIT_SYNC(vkCmdCopyBuffer)
    SSPC_EMIT_SYNC(vkCmdCopyBufferToImage)
    SSPC_EMIT_SYNC(vkCmdCopyImageToBuffer)
    SSPC_EMIT_SYNC(vkCmdCopyImage)
    SSPC_EMIT_SYNC(vkCmdBlitImage)
    SSPC_EMIT_SYNC(vkCmdClearColorImage)
    SSPC_EMIT_SYNC(vkCmdClearDepthStencilImage)
    SSPC_EMIT_SYNC(vkCmdFillBuffer)
    SSPC_EMIT_SYNC(vkCmdUpdateBuffer)
    SSPC_EMIT_SYNC(vkCmdResolveImage)
    SSPC_EMIT_SYNC(vkCmdBeginDebugUtilsLabelEXT)
    SSPC_EMIT_SYNC(vkCmdEndDebugUtilsLabelEXT)
    SSPC_EMIT_SYNC(vkCmdSetCheckpointNV)
    SSPC_EMIT_SYNC(vkCmdWriteBufferMarkerAMD)
#undef SSPC_EMIT_SYNC
    // the objects the ring may name, and the pools its command buffers come from: from any thread, after it
#define SSPC_EMIT_ORDERED(name)                                                                                        \
    if (f.name != nullptr) {                                                                                           \
        f.name = &Ordered<&Api::name, PFN_##name>::Call;                                                               \
    }
    SSPC_EMIT_ORDERED(vkDeviceWaitIdle)
    SSPC_EMIT_ORDERED(vkDestroyDevice)
    SSPC_EMIT_ORDERED(vkResetCommandPool)
    SSPC_EMIT_ORDERED(vkFreeCommandBuffers)
    SSPC_EMIT_ORDERED(vkResetCommandBuffer)
    SSPC_EMIT_ORDERED(vkDestroyCommandPool)
    SSPC_EMIT_ORDERED(vkDestroyQueryPool)
    SSPC_EMIT_ORDERED(vkDestroyBuffer)
    SSPC_EMIT_ORDERED(vkDestroyImage)
    SSPC_EMIT_ORDERED(vkFreeMemory)
    SSPC_EMIT_ORDERED(vkDestroyImageView)
    SSPC_EMIT_ORDERED(vkDestroySampler)
    SSPC_EMIT_ORDERED(vkDestroyShaderModule)
    SSPC_EMIT_ORDERED(vkDestroyDescriptorSetLayout)
    SSPC_EMIT_ORDERED(vkDestroyDescriptorPool)
    SSPC_EMIT_ORDERED(vkResetDescriptorPool)
    SSPC_EMIT_ORDERED(vkUpdateDescriptorSets)
    SSPC_EMIT_ORDERED(vkDestroyPipelineLayout)
    SSPC_EMIT_ORDERED(vkDestroyPipeline)
#undef SSPC_EMIT_ORDERED
    g_on = true;
    std::thread(Loop).detach();
    SSPC_LOG(GPU, Warning,
             "EXPERIMENT: SSPC_VK_EMIT_THREAD=1 - the per-draw Vulkan commands of the command processor's and the "
             "recording thread are passed to the driver by the emitter thread ({} MiB ring)",
             kRingBytes >> 20);
    return true;
}

std::string Report() {
    if (!g_on) {
        return {};
    }
    return fmt::format("emitter thread: {} commands deferred, {} syncs ({} waited, {:.0f} ms), ring full {} times, "
                       "called at once (extension data) {}",
                       g_deferred.load(), g_syncs.load(), g_sync_waits.load(),
                       static_cast<double>(g_sync_ticks.load()) / 3.0e6, g_full_waits.load(), g_fallbacks.load());
}

} // namespace SecondSon::Gpu::Vk::Emit
