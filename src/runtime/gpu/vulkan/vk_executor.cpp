// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

//
// SecondSonPC — the backend's executor (P4.4). See vk_executor.h. The per-draw state (viewports, scissors, depth,
// stencil, bias, binding conventions) follows shadPS4's renderer_vulkan/vk_rasterizer.cpp (GPL-2.0-or-later): the
// translated SPIR-V is the bridge's, so its bindings and push constants are filled the same way.
#include "runtime/gpu/vulkan/vk_executor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

#include "base/log.h"
#include "engine/render/native_render_ops.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/gnm/gnm_host.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_capture.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"
#include "runtime/gpu/vulkan/vk_formats.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

// context registers (indices verified against the bridge's regs.cpp layout assertions)
constexpr u32 kDbRenderControl = 0xA000;
constexpr u32 kDbHtileBase = 0xA005;
constexpr u32 kDbDepthBoundsMin = 0xA008, kDbDepthBoundsMax = 0xA009;
constexpr u32 kDbStencilClear = 0xA00A, kDbDepthClear = 0xA00B;
constexpr u32 kScreenScissorTl = 0xA00C, kScreenScissorBr = 0xA00D;
constexpr u32 kDbZInfo = 0xA010, kDbStencilInfo = 0xA011;
constexpr u32 kTaBcBase = 0xA020;
constexpr u32 kWindowOffset = 0xA080, kWindowScissorTl = 0xA081, kWindowScissorBr = 0xA082;
constexpr u32 kCbTargetMask = 0xA08E;
constexpr u32 kGenericScissorTl = 0xA090, kGenericScissorBr = 0xA091;
constexpr u32 kViewportScissor0 = 0xA094; ///< TL, BR per viewport (stride 2)
constexpr u32 kCbBlendRed = 0xA105;
constexpr u32 kDbStencilControl = 0xA10B, kDbStencilRefMask = 0xA10C, kDbStencilRefMaskBf = 0xA10D;
constexpr u32 kViewport0 = 0xA10F; ///< XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET per viewport (stride 6)
constexpr u32 kClipUserData = 0xA16F;
constexpr u32 kDbDepthControl = 0xA200;
constexpr u32 kCbColorControl = 0xA202;
constexpr u32 kPaClClipCntl = 0xA204;
constexpr u32 kPaSuScModeCntl = 0xA205;
constexpr u32 kPaClVteCntl = 0xA206;
constexpr u32 kPaScModeCntl0 = 0xA292;
constexpr u32 kVgtMultiPrimIbResetEn = 0xA2A5;
constexpr u32 kVgtInstanceStepRate0 = 0xA2A8, kVgtInstanceStepRate1 = 0xA2A9;
constexpr u32 kPolyOffsetClamp = 0xA2DF; ///< + FRONT_SCALE, FRONT_OFFSET, BACK_SCALE, BACK_OFFSET

float F(const Cp::RegisterFile& r, u32 reg) {
    return std::bit_cast<float>(r.Get(reg));
}
s32 Lo16(u32 v) {
    return static_cast<s16>(static_cast<u16>(v & 0xFFFF));
}
s32 Hi16(u32 v) {
    return static_cast<s16>(static_cast<u16>(v >> 16));
}

/// P4.10: a callback of the processor other than a draw or dispatch, timed into ExecutorStats::t_cb.
class CallbackTimer {
public:
    CallbackTimer(u64& ticks, u64& count) : ticks_(ticks), t0_(__rdtsc()) { ++count; }
    ~CallbackTimer() { ticks_ += __rdtsc() - t0_; }
    CallbackTimer(const CallbackTimer&) = delete;
    CallbackTimer& operator=(const CallbackTimer&) = delete;

private:
    u64& ticks_;
    u64 t0_;
};

constexpr u32 kQuerySlots = 1024; ///< batches in flight stay far below
constexpr u32 kProfBatches = 64, kProfPerBatch = 2048; ///< the profiler's pool: batch slots x timestamps
/// P4.8 Performance Inspector: batch slots x timestamps at category changes (a batch has ~500 commands; the
/// draws of one pass share a category, so far fewer changes)
constexpr u32 kInspBatches = 128, kInspPerBatch = 512;

constexpr VkPipelineStageFlags2 kGraphicsShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

/// The format the displayed image is read with: the VideoOut buffer's pixel format (the bridge's ImageInfo for a
/// VideoOut buffer), when the image's texels have its size (a bit copy reinterprets them).
VkFormat DisplayFormat(u32 pixel_format, VkFormat image_format) {
    const bool four_bytes = image_format == VK_FORMAT_R8G8B8A8_UNORM || image_format == VK_FORMAT_R8G8B8A8_SRGB ||
                            image_format == VK_FORMAT_B8G8R8A8_UNORM || image_format == VK_FORMAT_B8G8R8A8_SRGB ||
                            image_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                            image_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (!four_bytes) {
        return image_format;
    }
    switch (pixel_format) {
    case 0x80000000: // A8R8G8B8 sRGB
        return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x80002200: // A8B8G8R8 sRGB
        return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x88060000: // A2R10G10B10
    case 0x88000000: // A2R10G10B10 sRGB
    case 0x88740000: // A2R10G10B10 BT.2020 PQ
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    default:
        return image_format;
    }
}

// SSPC_DIAG_WATCH=<hex address>: the CPU writes of that guest page, caught by write protection (P4.5 race diagnosis)
std::atomic<u64> g_watch_page{0};
// SSPC_DIAG_WATCH_READS=1 (P4.6-G7, SSPC-020): the page no-access - the CPU's reads are caught too (who reads the GPU
// clock fences)
DWORD g_watch_prot = PAGE_READONLY;
struct WatchHit {
    u64 tsc, rip, addr;
    u32 tid;
    u32 write; ///< the access: 1 write, 0 read
};
std::array<WatchHit, 512> g_watch_hits;
std::atomic<u32> g_watch_n{0};

thread_local bool t_watch_rearm = false;

LONG CALLBACK WatchFilter(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_rearm) {
        // the write went through: the page protected again for the next one
        t_watch_rearm = false;
        e->ContextRecord->EFlags &= ~0x100u;
        DWORD old = 0;
        if (const u64 page = g_watch_page.load(std::memory_order_acquire); page != 0) {
            VirtualProtect(reinterpret_cast<void*>(page), 4096, g_watch_prot, &old);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        (r->ExceptionInformation[0] != 1 && !(g_watch_prot == PAGE_NOACCESS && r->ExceptionInformation[0] == 0))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 page = g_watch_page.load(std::memory_order_acquire);
    const u64 a = r->ExceptionInformation[1];
    if (page == 0 || (a & ~0xFFFull) != page) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u32 n = g_watch_n.fetch_add(1, std::memory_order_acq_rel);
    if (n < g_watch_hits.size()) {
        g_watch_hits[n] = {__rdtsc(), e->ContextRecord->Rip, a, GetCurrentThreadId(),
                           static_cast<u32>(r->ExceptionInformation[0])};
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
    t_watch_rearm = true;
    e->ContextRecord->EFlags |= 0x100u; // one instruction, then protected again (EXCEPTION_SINGLE_STEP above)
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

std::string_view SkipName(Skip s) {
    static constexpr std::string_view kNames[] = {"not ready",     "unsupported stage", "pipeline",      "buffer",
                                                  "shader DMA",    "image",             "vertex format", "target",
                                                  "filtered mode", "meta clear",        "stream ring"};
    return static_cast<size_t>(s) < std::size(kNames) ? kNames[static_cast<size_t>(s)] : "?";
}

Executor::Executor(DeviceContext& ctx, Timeline& timeline, StreamRing& ring, GuestBuffers& guest, BufferCache& buffers,
                   ImageCache& images, PipelineFactory& pipelines, GS::ShaderFrontend& frontend, Presenter* presenter,
                   OwnedBuffer& gds, const ExecutorOptions& options)
    : ctx_(ctx), timeline_(timeline), ring_(ring), guest_(guest), buffers_(buffers), images_(images), pipelines_(pipelines),
      frontend_(frontend), presenter_(presenter), gds_(gds), options_(options) {
    images_.SetBeforeUpload([](void* self) { static_cast<Executor*>(self)->BeforeUpload(); }, this);
    buffers_.SetBeforeUpload([](void* self) { static_cast<Executor*>(self)->BeforeUpload(); }, this);
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_BIND_SHADOW", v, sizeof(v)) == 2 && std::string_view(v) == "on") {
        bind_shadow_ = true;
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_DIAG_BIND_SHADOW=on - descriptor pushes and buffer bindings a draw "
                               "repeats are left out");
    }
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_STREAM_REUSE", v, sizeof(v)) > 0) {
        if (std::string_view(v) == "off") {
            stream_reuse_ = false;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_STREAM_REUSE=off - every small range streamed again");
        } else if (std::string_view(v) == "verify") {
            stream_verify_ = true;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_STREAM_REUSE=verify - every reuse copied again and "
                                   "compared");
        }
    }
    if (!GS::LookupMemoOn()) {
        SSPC_LOG(GPU, Warning, "SSPC_P49_LOOKUP_MEMO=0 - programs, fetch shaders and textures are looked up in their maps "
                               "every time (the P4.8 behaviour)");
    }
    char prof[4] = {};
    prof_ = GetEnvironmentVariableA("SSPC_GPU_PROFILE", prof, sizeof(prof)) == 1 && (prof[0] == '1' || prof[0] == 'q') &&
            ctx_.Caps().timestamp_period > 0.0f;
    prof_queues_ = prof_ && prof[0] == 'q';
    if (prof_) {
        const VkQueryPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = kProfBatches * kProfPerBatch};
        if (Fn().vkCreateQueryPool(ctx_.Device(), &pi, nullptr, &prof_pool_) != VK_SUCCESS) {
            prof_ = prof_queues_ = false;
        } else if (prof_queues_) {
            SSPC_LOG(GPU, Warning, "SSPC_GPU_PROFILE=q: GPU time per guest queue (a timestamp where the commands' queue "
                                   "changes and at a batch's end)");
        } else {
            SSPC_LOG(GPU, Warning, "SSPC_GPU_PROFILE=1: GPU time per command (a timestamp after every command; slower)");
        }
    }
    markers_ = ctx_.Caps().markers && Fn().vkCmdBeginDebugUtilsLabelEXT != nullptr &&
               Fn().vkCmdEndDebugUtilsLabelEXT != nullptr;
    if (markers_) {
        legend_.open(options_.capture_path.parent_path().parent_path() / "gpu_markers.txt", std::ios::trunc);
        legend_ << "# SSPC_GPU_MARKERS legend: label (D p<pipeline> vs<addr> ps<addr> @<PM4 packet>) | its targets\n";
    }
    if (ctx_.Caps().timestamp_period > 0.0f) {
        const VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = 2 * kQuerySlots};
        if (Fn().vkCreateQueryPool(ctx_.Device(), &qi, nullptr, &queries_) != VK_SUCCESS) {
            queries_ = VK_NULL_HANDLE;
        }
        // SSPC_DIAG_CLOCK=place (SSPC-020 experiment, not the default): measured on the beach (logs\p46\f3_*) it did not
        // reduce the smoke's flicker (107 vs 45 block jumps per 100 frames) and the command processor took 4-6 ms more
        // a frame with the same draw/dispatch counts - the game reads these clocks (P4.6) and does more with them
        char cv[16] = {};
        const bool place = GetEnvironmentVariableA("SSPC_DIAG_CLOCK", cv, sizeof(cv)) > 0 && std::string_view(cv) == "place";
        clock_label_diag_ = place || std::string_view(cv) == "labels"; // (every draw's bindings against the labels)
        const VkQueryPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = kClockBatches * kClockPerBatch};
        if (place && queries_ != VK_NULL_HANDLE &&
            Fn().vkCreateQueryPool(ctx_.Device(), &ci, nullptr, &clock_pool_) == VK_SUCCESS) {
            clock_at_place_ = true;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_CLOCK=place - GPU clock fences carry the GPU's time "
                                   "at their place (a timestamp there)");
        }
    }
    CmdTraceInit();
}

Executor::~Executor() {
    if (trace_.on && !trace_.written && !trace_.entries.empty()) {
        CmdTraceWrite(); // the run ended inside the traced flips
    }
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    if (queries_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, queries_, nullptr);
    }
    if (insp_pool_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, insp_pool_, nullptr);
    }
    if (prof_pool_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, prof_pool_, nullptr);
    }
    if (clock_pool_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, clock_pool_, nullptr);
    }
    DestroyBuffer(ctx_, lds_);
    DestroyBuffer(ctx_, diag_scratch_);
    for (auto& [value, b] : lds_retired_) {
        DestroyBuffer(ctx_, b);
    }
    const auto destroy = [&](Batch& b) {
        if (b.descriptors != VK_NULL_HANDLE) {
            f.vkDestroyDescriptorPool(dev, b.descriptors, nullptr);
        }
        if (b.pool != VK_NULL_HANDLE) {
            f.vkDestroyCommandPool(dev, b.pool, nullptr);
        }
    };
    if (open_) {
        f.vkEndCommandBuffer(cur_.cb);
        destroy(cur_);
    }
    for (auto& b : in_flight_) {
        destroy(b);
    }
    for (auto& b : free_) {
        destroy(b);
    }
    for (auto& b : abandoned_) {
        destroy(b);
    }
}

void Executor::SetQueuePipe(u32 queue, u32 pipe) {
    if (queue < Cp::kNumQueues) {
        queue_pipe_[queue] = pipe;
    }
}

void Executor::Skipped(Skip why, const std::string& detail) {
    if (trace_.Active()) {
        CmdTraceEntry& e = CmdTraceAdd('S', 0, 0, 0);
        e.skip = static_cast<u8>(why);
        e.skip_text = static_cast<u32>(trace_.skip_texts.size());
        trace_.skip_texts.push_back(detail);
    }
    const auto i = static_cast<size_t>(why);
    if (stats_.skipped[i]++ == 0) {
        stats_.first_skip[i] = detail;
        SSPC_LOG(GPU, Warning, "native backend: first draw/dispatch not executed ({}): {}", SkipName(why), detail);
    }
}

bool Executor::Failed() const { return ctx_.Failed(); }

bool Executor::Begin() {
    if (Failed()) {
        return false; // nothing more reaches the GPU
    }
    if (open_) {
        return true;
    }
    const auto& f = Fn();
    if (!free_.empty()) {
        cur_ = std::move(free_.back());
        free_.pop_back();
        f.vkResetCommandPool(ctx_.Device(), cur_.pool, 0);
        if (cur_.descriptors != VK_NULL_HANDLE) {
            f.vkResetDescriptorPool(ctx_.Device(), cur_.descriptors, 0);
        }
    } else {
        cur_ = Batch{};
        const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                          .queueFamilyIndex = ctx_.QueueFamily()};
        if (f.vkCreateCommandPool(ctx_.Device(), &pci, nullptr, &cur_.pool) != VK_SUCCESS) {
            return false;
        }
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = cur_.pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 2};
        VkCommandBuffer cbs[2] = {};
        f.vkAllocateCommandBuffers(ctx_.Device(), &ai, cbs);
        cur_.cb = cbs[0];
        cur_.pre = cbs[1];
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                              {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                              {VK_DESCRIPTOR_TYPE_SAMPLER, 4096}};
        const VkDescriptorPoolCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = 2048,
                                             .poolSizeCount = 4,
                                             .pPoolSizes = sizes};
        f.vkCreateDescriptorPool(ctx_.Device(), &dci, nullptr, &cur_.descriptors);
    }
    cur_.work.fill(0);
    cur_.queues.clear();
    cur_.value = timeline_.Next();
    cur_.trace_frame = StallTrace::CurrentFrame(); // (P4.12)
    cur_.submit_t0 = cur_.submit_t1 = 0;
    g_diag_submission.store(cur_.value, std::memory_order_relaxed);
    batch_first_cmd_ = batch_last_cmd_ = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        diag_->Name(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<u64>(cur_.cb), fmt::format("batch {}", cur_.value));
    }
    const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    f.vkBeginCommandBuffer(cur_.cb, &bi);
    if (queries_ != VK_NULL_HANDLE) {
        cur_.query_slot = next_query_slot_++ % kQuerySlots;
        if (!ring_.Mirrored()) { // (hybrid ring: the batch's first timestamp is written by its head, before the copies)
            f.vkCmdResetQueryPool(cur_.cb, queries_, cur_.query_slot * 2, 2);
            f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, cur_.query_slot * 2);
        }
    }
    cur_.prof_base = ~0u;
    cur_.prof_count = 0;
    cur_.prof_keys.clear();
    cur_.clock_fences.clear();
    cur_.clock_base = ~0u;
    if (clock_at_place_ && cur_.query_slot != ~0u) {
        cur_.clock_base = (clock_next_++ % kClockBatches) * kClockPerBatch;
        f.vkCmdResetQueryPool(cur_.cb, clock_pool_, cur_.clock_base, kClockPerBatch);
    }
    if (prof_) {
        cur_.prof_base = (prof_next_batch_++ % kProfBatches) * kProfPerBatch;
        f.vkCmdResetQueryPool(cur_.cb, prof_pool_, cur_.prof_base, kProfPerBatch);
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool_, cur_.prof_base);
        cur_.prof_count = 1;
    }
    // P4.8: the inspector's category timestamps (every insp_every_-th frame; never more batch slots than the pool has
    // in flight)
    cur_.insp_base = ~0u;
    cur_.insp_count = 0;
    cur_.insp_slots.clear();
    cur_.insp_last = 0xFF;
    cur_.insp_overflow = false;
    cur_.insp_frame = insp_frame_;
    if (insp_pool_ != VK_NULL_HANDLE && insp_every_ != 0 && insp_frame_ % insp_every_ == 0 &&
        in_flight_.size() + 2 < kInspBatches) {
        cur_.insp_base = (insp_next_batch_++ % kInspBatches) * kInspPerBatch;
        f.vkCmdResetQueryPool(cur_.cb, insp_pool_, cur_.insp_base, kInspPerBatch);
        open_ = true; // (InspMark records into the open batch)
        InspMark(Inspector::kGpuBatchStart);
    }
    open_ = true;
    commands_ = 0;
    rendering_ = false;
    label_open_ = false; // labels never span command buffers (Flush ends the open one)
    // a new command buffer: no dynamic state set yet - also what a draw sets only conditionally (the depth bias values
    // with bias on, the colour write masks with colour targets): not "already set" from the last buffer (validation f3)
    dyn_.valid = false;
    dyn_.bias_set = false;
    dyn_.color_masks_count = ~0u;
    dyn_vertex_valid_ = false;
    gfx_pipeline_ = VK_NULL_HANDLE; // a new command buffer: nothing bound or pushed yet
    push_layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
    vb_valid_ = false;
    ib_last_ = VK_NULL_HANDLE;
    ib_type_last_ = VK_INDEX_TYPE_MAX_ENUM;
    ++stats_.batches;
    return true;
}

void Executor::Note(u32 queue, u64 seq) {
    if (queue >= Cp::kNumQueues) {
        return;
    }
    if (cur_.work[queue] == 0) {
        cur_.queues.push_back(queue);
    }
    cur_.work[queue] = std::max(cur_.work[queue], seq);
    if (prof_queues_ && queue != prof_queue_) {
        ProfQueue(queue); // (before the command's barriers and uploads: they count to its queue)
    }
}

void Executor::EndRendering() {
    if (rendering_) {
        Fn().vkCmdEndRendering(cur_.cb);
        rendering_ = false;
        render_ = RenderKey{};
    }
}

void Executor::Label(u64 key, std::string text) {
    EndLabel();
    const VkDebugUtilsLabelEXT l{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = text.c_str()};
    Fn().vkCmdBeginDebugUtilsLabelEXT(cur_.cb, &l);
    label_open_ = true;
    label_key_ = key;
    label_text_ = std::move(text);
}

void Executor::EndLabel() {
    if (label_open_) {
        Fn().vkCmdEndDebugUtilsLabelEXT(cur_.cb);
        label_open_ = false;
    }
}

void Executor::GlobalBarrier(u32 site) {
    ++stats_.gb_sites[site < stats_.gb_sites.size() ? site : 0];
    EndRendering();
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    Fn().vkCmdPipelineBarrier2(cur_.cb, &dep);
    pending_writes_ = false;
    pend_.Clear();
    ++stats_.global_barriers;
}

void Executor::NoteOrderRange(const void* plan_ptr, u64 shader, u32 queue, bool dispatch) {
    // SSPC_DIAG_ORDER_RANGE=<hex address>[:<hex size>] (SSPC-020): every draw/dispatch binding that memory, in recording
    // order with the flip and queue -> logs\p45\order_range.txt (flips 1400..1460)
    static const std::pair<u64, u64> range = [] {
        char v[64] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_ORDER_RANGE", v, sizeof(v)) == 0) {
            return std::pair<u64, u64>{0, 0};
        }
        char* p = v;
        const u64 a = std::strtoull(p, &p, 16);
        const u64 s = *p == ':' ? std::strtoull(p + 1, nullptr, 16) : 0x1000;
        return std::pair<u64, u64>{a, s};
    }();
    if (range.second == 0) {
        return;
    }
    const u64 flip = VideoOut::NextFlipId();
    if (flip < 1400 || flip > 1460) {
        return;
    }
    static std::ofstream out(options_.capture_path.parent_path().parent_path() / "p45" / "order_range.txt",
                             std::ios::trunc);
    const auto& plan = *static_cast<const GS::ResourceBindingPlan*>(plan_ptr);
    for (const auto& b : plan.buffers) {
        if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.address < range.first + range.second &&
            range.first < b.address + b.size) {
            out << fmt::format("flip {} batch {} q{} {} {:#x} {} [{:#x}, +{:#x})\n", flip, cur_.value, queue,
                               dispatch ? "dispatch" : "draw", shader, b.written ? "WRITES" : "reads", b.address, b.size);
        }
    }
}

void Executor::NoteClockLabelReads(const void* plan_ptr, u64 shader) {
    // SSPC-020 diagnostics: a shader binding a buffer over an address the guest's EOP/RELEASE_MEM writes a GPU clock to
    if (clock_labels_.empty()) {
        return;
    }
    const auto& plan = *static_cast<const GS::ResourceBindingPlan*>(plan_ptr);
    for (const auto& b : plan.buffers) {
        if (b.kind != GS::BufferKind::Guest || b.status != GS::ResourceStatus::Ok || b.size == 0) {
            continue;
        }
        for (const u64 l : clock_labels_) {
            if (l >= b.address && l < b.address + b.size) {
                ++stats_.clock_label_binds;
                if (clock_label_logged_ < 16) {
                    ++clock_label_logged_;
                    SSPC_LOG(GPU, Info, "SSPC-020 diag: shader {:#x} binds [{:#x}, +{:#x}) {} over the GPU clock label {:#x}",
                             shader, b.address, b.size, b.written ? "(written)" : "(read)", l);
                }
            }
        }
    }
}

bool Executor::NeedsBarrier(const std::vector<std::pair<u64, u64>>& reads,
                            const std::vector<std::pair<u64, u64>>& writes, bool unknown, u64 tag) {
    static const bool off = [] {
        char v[8] = {};
        const bool o = GetEnvironmentVariableA("SSPC_DIAG_BARRIER_RANGES", v, sizeof(v)) == 3 && std::string_view(v) == "off";
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BARRIER_RANGES=off - a global barrier before every "
                                   "dispatch and timeline copy after any write");
        }
        return o;
    }();
    // a conflict between two dispatches of one queue with no guest synchronization between them (equal tags) does not
    // count: the guest does not order them (counted)
    bool guest_unordered = false;
    const auto overlaps = [&](const std::vector<std::pair<u64, u64>>& a, const std::vector<std::pair<u64, u64>>& b,
                              const std::vector<u64>& btags) {
        for (const auto& [ab, ae] : a) {
            for (size_t i = 0; i < b.size(); ++i) {
                if (ab < b[i].second && b[i].first < ae) {
                    if (tag != 0 && i < btags.size() && btags[i] == tag) {
                        guest_unordered = true;
                        continue;
                    }
                    return true;
                }
            }
        }
        return false;
    };
    if (!pending_writes_) {
        // nothing written since the last barrier: no read-after-write; a write over what an earlier command since then
        // still reads (write-after-read) is ordered too
        if (!off && !writes.empty() && overlaps(writes, pend_.reads, pend_.rtags)) {
            ++stats_.barriers_war;
            return true;
        }
        return false;
    }
    // why (counted): 0 off, 1 an unknown write pending, 2 this command's unknown accesses, 3 too many ranges, 4 RAW,
    // 5 WAW, 6 WAR
    const u32 why = off                                                   ? 0
                    : pend_.unknown                                       ? 1
                    : unknown                                             ? 2
                    : pend_.writes.size() + pend_.reads.size() > 256      ? 3
                    : overlaps(reads, pend_.writes, pend_.wtags)          ? 4
                    : overlaps(writes, pend_.writes, pend_.wtags)         ? 5
                    : overlaps(writes, pend_.reads, pend_.rtags)          ? 6
                                                                          : 7;
    if (why != 7) {
        ++stats_.barrier_why[why];
        const auto gds = [](const std::pair<u64, u64>& r) { return r.first >= kGdsRangeBase; };
        if (why == 5 && std::ranges::any_of(writes, gds) && std::ranges::any_of(pend_.writes, gds)) {
            ++stats_.barrier_waw_gds;
        }
        return true;
    }
    ++stats_.barriers_elided;
    stats_.barriers_guest_order += guest_unordered ? 1 : 0;
    return false;
}

void Executor::NoteAccesses(const std::vector<std::pair<u64, u64>>& reads,
                            const std::vector<std::pair<u64, u64>>& writes, bool unknown, u64 tag) {
    // (a range already noted with the same tag is not noted again: a chain of dispatches over one pool stays short)
    const auto note = [&](std::vector<std::pair<u64, u64>>& list, std::vector<u64>& tags, const std::pair<u64, u64>& r) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == r && tags[i] == tag) {
                return;
            }
        }
        list.push_back(r);
        tags.push_back(tag);
    };
    for (const auto& r : reads) {
        note(pend_.reads, pend_.rtags, r);
    }
    for (const auto& r : writes) {
        note(pend_.writes, pend_.wtags, r);
    }
    if (unknown) {
        pend_.unknown = true;
    }
    if (unknown || !writes.empty()) {
        pending_writes_ = true;
        GuestMemoryChanged(); // (P4.7: no memoized stage preparation past a write)
    }
}

void Executor::Flush() {
    want_flush_ = false;
    write_run_ = false;
    run_ranges_.clear();
    if (!open_) {
        return;
    }
    const auto& f = Fn();
    if (Failed()) { // never publish completion or recycle an unsubmitted batch
        f.vkEndCommandBuffer(cur_.cb);
        abandoned_.push_back(std::move(cur_));
        cur_ = Batch{};
        open_ = false;
        commands_ = 0;
        rendering_ = false;
        return;
    }
    EndRendering();
    if (LabelChanges(kLabelOther | 5)) {
        Label(kLabelOther | 5, "W batch end: write-backs, host barrier");
    }
    // the GPU's writes of guest buffers (video memory) copied back to the host before the batch's fences
    if (prof_queues_) {
        ProfQueue(prof_queue_);
    } else if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    InspMark(Inspector::kGpuTransfer);
    const u64 wb0 = buffers_.Stats().writeback_bytes;
    // SSPC_DIAG_MEASURE=skip_wb: MEASUREMENT ONLY (the host never sees the GPU's buffer writes) - the write-backs'
    // GPU cost without the profiler (never a configuration)
    static const bool skip_wb = [] {
        char v[32] = {};
        const bool s = GetEnvironmentVariableA("SSPC_DIAG_MEASURE", v, sizeof(v)) > 0 && std::string_view(v) == "skip_wb";
        if (s) {
            SSPC_LOG(GPU, Warning, "MEASUREMENT ONLY: SSPC_DIAG_MEASURE=skip_wb - no write-backs (the host's copy is "
                                   "wrong)");
        }
        return s;
    }();
    if (skip_wb) {
        buffers_.DropWriteBack();
    }
    buffers_.RecordWriteBack(cur_.cb, cur_.value);
    // (by the batch's end: 0 a fence/flip/sync/idle flush, 1 full with a fence or a timeline write, 2 full without)
    stats_.wb_bytes_by[!flush_full_ ? 0 : batch_fence_ ? 1 : 2] += buffers_.Stats().writeback_bytes - wb0;
    flush_full_ = false;
    batch_fence_ = false;
    if (prof_) {
        ProfMark(5, "batch end: write-backs of the GPU's writes to the host");
    }
    // the batch's writes made available to the host (guest memory the CPU reads after the fence)
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT |
                                               VK_ACCESS_2_MEMORY_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cur_.cb, &dep);
    pending_writes_ = false;
    pend_.Clear();
    EndLabel();
    if (cur_.insp_base != ~0u && cur_.insp_count < kInspPerBatch) {
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, insp_pool_, cur_.insp_base + cur_.insp_count);
        ++cur_.insp_count; // (the end: no interval starts at it)
    }
    if (cur_.query_slot != ~0u) {
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, cur_.query_slot * 2 + 1);
    }
    f.vkEndCommandBuffer(cur_.cb);
    // hybrid stream ring: the bytes the CPU put into the ring for this batch, copied into the video memory mirror its
    // commands read - before the first of them (one or two copies; the previous batch ended with a full barrier, so
    // the regions' earlier readers are done)
    VkCommandBufferSubmitInfo cbis[2] = {};
    u32 n_cb = 0;
    if (ring_.Mirrored()) {
        VkBufferCopy copies[2];
        const u32 n = ring_.TakeDirty(copies);
        if (n > 0 || cur_.query_slot != ~0u) {
            const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            f.vkBeginCommandBuffer(cur_.pre, &bi);
            if (cur_.query_slot != ~0u) {
                f.vkCmdResetQueryPool(cur_.pre, queries_, cur_.query_slot * 2, 2);
                f.vkCmdWriteTimestamp2(cur_.pre, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, cur_.query_slot * 2);
            }
            if (n > 0) {
                f.vkCmdCopyBuffer(cur_.pre, ring_.HostBuffer(), ring_.MirrorBuffer(), n, copies);
                const VkMemoryBarrier2 cmb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
                const VkDependencyInfo cdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                            .pMemoryBarriers = &cmb};
                f.vkCmdPipelineBarrier2(cur_.pre, &cdep);
                stats_.ring_mirror_copies += n;
            }
            f.vkEndCommandBuffer(cur_.pre);
            cbis[n_cb++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cur_.pre};
        }
    }
    cbis[n_cb++] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cur_.cb};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = timeline_.Handle(),
                                       .value = cur_.value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    g_diag_submission.store(cur_.value, std::memory_order_relaxed);
    VkResult r;
    const u64 submit_t0 = GS::Ticks();
    u64 locked_t = 0;
    bool contended = false;
    {
        std::unique_lock lock{ctx_.QueueMutex(), std::try_to_lock};
        if (!lock.owns_lock()) {
            contended = true; // (P4.12: the present thread or another submitter holds the queue)
            lock.lock();
        }
        locked_t = GS::Ticks();
        // the video memory the batch's commands need bound first (the binds signal their own timeline)
        VkSemaphore bind_sem = VK_NULL_HANDLE;
        u64 bind_value = 0;
        buffers_.SubmitBinds(bind_sem, bind_value);
        const VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                         .semaphore = bind_sem,
                                         .value = bind_value,
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .waitSemaphoreInfoCount = bind_sem != VK_NULL_HANDLE ? 1u : 0u,
                               .pWaitSemaphoreInfos = &wait,
                               .commandBufferInfoCount = n_cb,
                               .pCommandBufferInfos = cbis,
                               .signalSemaphoreInfoCount = 1,
                               .pSignalSemaphoreInfos = &signal};
        r = ctx_.Failed() ? ctx_.Failure() : f.vkQueueSubmit2(ctx_.Queue(), 1, &si, VK_NULL_HANDLE);
    }
    const u64 submit_t1 = GS::Ticks();
    stats_.submit_ns += GS::TicksToNs(submit_t1 - submit_t0);
    cur_.submit_t0 = submit_t0;
    cur_.submit_t1 = submit_t1;
    if (StallTrace::On()) {
        if (contended) {
            StallTrace::Span(StallTrace::Ev::QueueLock, submit_t0, locked_t, cur_.value);
        }
        StallTrace::Span(StallTrace::Ev::Submit, locked_t, submit_t1, cur_.value, static_cast<u32>(commands_));
    }
    if (r != VK_SUCCESS) {
        const u64 value = cur_.value;
        SSPC_LOG(GPU, Error, "native backend: vkQueueSubmit2 failed (VkResult {}, value {}, in-flight {}); "
                            "batch quarantined, no completion will be published",
                 static_cast<int>(r), value, in_flight_.size());
        ctx_.Fail("vkQueueSubmit2", r);
        abandoned_.push_back(std::move(cur_));
        cur_ = Batch{};
        open_ = false;
        commands_ = 0;
        if (r == VK_ERROR_DEVICE_LOST) {
            OnDeviceLost("vkQueueSubmit2", value);
        }
        return;
    }
    if (diag_ != nullptr) {
        diag_->Submitted(cur_.value, batch_first_cmd_, batch_last_cmd_);
    }
    in_flight_.push_back(std::move(cur_));
    cur_ = Batch{};
    open_ = false;
    commands_ = 0;
}

void Executor::Poll() {
    if (Failed()) {
        return;
    }
    const u64 completed = timeline_.Completed();
    if (timeline_.Lost()) {
        OnDeviceLost("vkGetSemaphoreCounterValue", completed + 1);
        return;
    }
    if (Failed()) {
        return;
    }
    FrameLog::GpuCompleted(completed);
    if (StallTrace::On() && !in_flight_.empty() && in_flight_.front().value <= completed) {
        StallTrace::Instant(StallTrace::Ev::GpuCompleted, completed); // (P4.12: the labels of these batches land now)
    }
    while (!in_flight_.empty() && in_flight_.front().value <= completed) {
        Batch b = std::move(in_flight_.front());
        in_flight_.pop_front();
        if (cp_ != nullptr && !b.clock_fences.empty() && b.query_slot != ~0u) {
            // each clock fence's age: from its timestamp to the batch's last one (taken as now - the completion was
            // just seen)
            u64 end_ts[2] = {};
            if (Fn().vkGetQueryPoolResults(ctx_.Device(), queries_, b.query_slot * 2, 2, sizeof(end_ts), end_ts, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                for (const auto& cf : b.clock_fences) {
                    u64 ts = 0;
                    if (Fn().vkGetQueryPoolResults(ctx_.Device(), clock_pool_, cf.query, 1, sizeof(ts), &ts, 8,
                                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                        end_ts[1] >= ts) {
                        const u64 age = static_cast<u64>(static_cast<double>(end_ts[1] - ts) *
                                                         ctx_.Caps().timestamp_period);
                        cp_->SetFenceClockAge(cf.queue, cf.packet_addr, cf.after_seq, age);
                    }
                }
            }
        }
        if (cp_ != nullptr) {
            for (const u32 q : b.queues) {
                cp_->CompleteWork(q, b.work[q]);
            }
        }
        ++stats_.completed_batches;
        if (b.query_slot != ~0u) {
            u64 ts[2] = {};
            if (Fn().vkGetQueryPoolResults(ctx_.Device(), queries_, b.query_slot * 2, 2, sizeof(ts), ts, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[1] >= ts[0]) {
                stats_.gpu_busy_ns += static_cast<u64>(static_cast<double>(ts[1] - ts[0]) * ctx_.Caps().timestamp_period);
                ++stats_.gpu_batches_timed;
                StallTrace::GpuBatch(b.value, b.submit_t0, b.submit_t1, ts[0], ts[1], b.trace_frame); // (P4.12)
                if (insp_catalog_ != nullptr) {
                    // the frame's GPU time: the union of its batches' intervals (one queue; a batch's start may come
                    // before the previous one's end - its top-of-pipe timestamp)
                    Inspector::GpuFrameTimes& acc = insp_acc_[b.insp_frame % insp_acc_.size()];
                    const u64 from = std::max(ts[0], insp_last_end_);
                    if (ts[1] > from) {
                        acc.busy_ns += static_cast<u64>(static_cast<double>(ts[1] - from) * ctx_.Caps().timestamp_period);
                    }
                    insp_last_end_ = std::max(insp_last_end_, ts[1]);
                    ++acc.batches;
                }
            }
            b.query_slot = ~0u;
        }
        if (b.insp_base != ~0u) {
            Inspector::GpuFrameTimes& acc = insp_acc_[b.insp_frame % insp_acc_.size()];
            u64 ts[kInspPerBatch];
            if (b.insp_count >= 2 &&
                Fn().vkGetQueryPoolResults(ctx_.Device(), insp_pool_, b.insp_base, b.insp_count, b.insp_count * 8, ts, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const double period = ctx_.Caps().timestamp_period;
                for (u32 i = 0; i + 1 < b.insp_count && i < b.insp_slots.size(); ++i) {
                    if (ts[i + 1] > ts[i]) {
                        acc.slot_ns[b.insp_slots[i]] += static_cast<u64>(static_cast<double>(ts[i + 1] - ts[i]) * period);
                    }
                }
                acc.timed = true;
            }
            acc.overflow += b.insp_overflow ? 1 : 0;
            b.insp_base = ~0u;
        }
        if (b.prof_base != ~0u) {
            ProfCollect(b);
        }
        free_.push_back(std::move(b));
    }
    while (!insp_frame_ends_.empty() && insp_frame_ends_.front().value <= completed) {
        const u32 n = insp_frame_ends_.front().n;
        insp_frame_ends_.pop_front();
        Inspector::GpuFrameTimes& acc = insp_acc_[n % insp_acc_.size()];
        Inspector::GpuFrame(n, acc);
        acc = {};
    }
    ring_.Collect(completed);
    if (shadow_ring_ != nullptr) {
        shadow_ring_->Collect(completed);
    }
    guest_.Collect(completed);
    images_.Collect(completed);
    if (readback_) {
        readback_->Collect(completed);
    }
    while (!args_rb_.empty() && args_rb_.front().value <= completed) {
        auto& a = args_rb_.front();
        u32 v[5] = {};
        std::memcpy(v, a.buffer.mapped, sizeof(v));
        histo_.args << fmt::format("flip {} args {:#x}: {} {} {} {} {}\n", a.flip, a.addr, v[0], v[1], v[2], v[3], v[4]);
        histo_.args.flush();
        DestroyBuffer(ctx_, a.buffer);
        args_rb_.pop_front();
    }
    while (!probe_rb_.empty() && probe_rb_.front().value <= completed) {
        auto& p = probe_rb_.front();
        const auto dir = options_.capture_path.parent_path().parent_path() / "smoke_research" / "probe";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir / (p.name + ".bin"), std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(p.buffer.mapped), static_cast<std::streamsize>(p.size));
        DestroyBuffer(ctx_, p.buffer);
        probe_rb_.pop_front();
    }
    buffers_.Collect(completed);
    std::erase_if(lds_retired_, [&](auto& e) {
        if (e.first > completed) {
            return false;
        }
        DestroyBuffer(ctx_, e.second);
        return true;
    });
    if (!timeline_writes_.empty()) {
        std::erase_if(timeline_writes_, [&](const TimelineWrite& w) { return w.value <= completed; });
        ++tw_version_;
        tw_lo_ = ~0ull;
        tw_hi_ = 0;
        for (const auto& w : timeline_writes_) {
            tw_lo_ = std::min(tw_lo_, w.begin);
            tw_hi_ = std::max(tw_hi_, w.end);
        }
    }
}

std::string DescribeImage(const CachedImage& img) {
    const ImageLayout& l = img.layout;
    return fmt::format("img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} s {} tile {} bits {}{}{}{}", img.id,
                       l.guest_address, l.guest_size, l.pixel_format, l.size.width, l.size.height, l.size.depth,
                       l.levels, l.layers, l.num_samples, static_cast<u32>(l.tile_mode), l.num_bits,
                       l.props.is_depth ? " depth" : "", img.gpu_written ? " gpu-written" : "",
                       img.alias_hits != 0 ? " ALIASED" : "");
}

void Executor::OnDeviceLost(const char* where, u64 value) {
    if (device_lost_) {
        return;
    }
    device_lost_ = true;
    ctx_.Fail(where, VK_ERROR_DEVICE_LOST);
    std::string in_flight;
    for (const auto& b : in_flight_) {
        in_flight += fmt::format("  value {}: queues", b.value);
        for (const u32 q : b.queues) {
            in_flight += fmt::format(" {} (work {})", q, b.work[q]);
        }
        in_flight += "\n";
    }
    in_flight += fmt::format("  timeline: completed (last seen) {}, last submitted {}\n", timeline_.CompletedCached(),
                             timeline_.LastSubmitted());
    if (diag_ != nullptr) {
        diag_->DeviceLost(where, value, in_flight);
    } else {
        SSPC_LOG(GPU, Error, "native backend: VK_ERROR_DEVICE_LOST ({}, submission {})", where, value);
    }
}

void Executor::SetInspector(const Inspector::Catalog* catalog, u32 gpu_every) {
    insp_catalog_ = catalog;
    insp_every_ = 0;
    if (catalog == nullptr || gpu_every == 0 || queries_ == VK_NULL_HANDLE) {
        return;
    }
    const VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                   .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                   .queryCount = kInspBatches * kInspPerBatch};
    if (Fn().vkCreateQueryPool(ctx_.Device(), &qi, nullptr, &insp_pool_) == VK_SUCCESS) {
        insp_every_ = gpu_every;
    } else {
        insp_pool_ = VK_NULL_HANDLE;
        SSPC_LOG(GPU, Warning, "Performance Inspector: no query pool for the category timestamps - no GPU times");
    }
}

void Executor::InspMark(size_t slot) {
    if (cur_.insp_base == ~0u || !open_ || slot == cur_.insp_last) {
        return;
    }
    if (cur_.insp_count + 1 >= kInspPerBatch) { // (one kept for the batch's end)
        cur_.insp_overflow = true;
        return;
    }
    Fn().vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, insp_pool_, cur_.insp_base + cur_.insp_count);
    ++cur_.insp_count;
    cur_.insp_slots.push_back(static_cast<u8>(slot));
    cur_.insp_last = static_cast<u8>(slot);
}

Inspector::Class Executor::InspClassifyDraw(const GS::StageView& vs, const GS::StageView& ps, bool has_ps,
                                            const Inspector::DrawShape& shape) {
    const u64 vc = vs.program != nullptr ? vs.program->id.hash.lo : 0;
    const u64 pc = has_ps && ps.program != nullptr ? ps.program->id.hash.lo : 0;
    InspDrawMemo& m = insp_draw_memo_[((vc * 0x9E3779B97F4A7C15ull) ^ (pc * 0xC2B2AE3D27D4EB4Full)) >> 54];
    if (!m.valid || m.vs != vc || m.ps != pc || m.has_ps != has_ps) {
        m = {vc, pc, insp_catalog_->DrawEntry(vc, pc, has_ps), true, has_ps};
    }
    return m.entry != nullptr ? *m.entry : Inspector::RuleForDraw(shape);
}

Inspector::Class Executor::InspClassifyDispatch(const GS::StageView& cs) {
    const u64 cc = cs.program != nullptr ? cs.program->id.hash.lo : 0;
    InspDispatchMemo& m = insp_dispatch_memo_[(cc * 0x9E3779B97F4A7C15ull) >> 56];
    if (!m.valid || m.cs != cc) {
        m = {cc, insp_catalog_->Dispatch(cc), true};
    }
    return m.cls;
}

std::string Executor::InspectorPipelines() const {
    const double ns_per_tick = static_cast<double>(GS::TicksToNs(1'000'000)) / 1e6;
    std::string out = "kind,id,category,basis,count,instances,vertices,groups,indirect,single_group,cpu_ms,vs_addr,vs,"
                      "ps_addr,ps,cs_addr,cs,prim,colors,depth,width,height\n";
    for (size_t i = 0; i < insp_pipelines_.size(); ++i) {
        const InspPipeline& p = insp_pipelines_[i];
        if (p.draws == 0) {
            continue;
        }
        out += fmt::format("draw,{},{},{},{},{},{},,{},,{:.3f},{:x},{:x},{:x},{:x},,,{},{},{},{},{}\n", i,
                           Inspector::CategoryName(p.cls.category), Inspector::BasisName(p.cls.basis), p.draws,
                           p.instances, p.vertices, p.indirect, static_cast<double>(p.cpu_ticks) * ns_per_tick / 1e6,
                           p.vs_addr, p.vs, p.ps_addr, p.ps, p.prim, p.colors, p.depth ? 1 : 0, p.width, p.height);
    }
    for (const auto& [content, p] : insp_programs_) {
        out += fmt::format("dispatch,,{},{},{},,,{},{},{},{:.3f},,,,,{:x},{:x},,,,,\n",
                           Inspector::CategoryName(p.cls.category), Inspector::BasisName(p.cls.basis), p.dispatches,
                           p.groups, p.indirect, p.single_group, static_cast<double>(p.cpu_ticks) * ns_per_tick / 1e6,
                           p.address, content);
    }
    return out;
}

void Executor::ProfMark(u64 key, const std::string& label) {
    if (!prof_ || !open_ || cur_.prof_base == ~0u || cur_.prof_count >= kProfPerBatch) {
        return;
    }
    if (prof_queues_ && (key >> 32) != kProfQueueTag && key != 5) {
        return; // (=q: the queue marks and the batch end's write-backs only)
    }
    Fn().vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool_, cur_.prof_base + cur_.prof_count);
    ++cur_.prof_count;
    cur_.prof_keys.push_back(key);
    if (auto& acc = prof_acc_[key]; acc.label.empty()) {
        acc.label = label;
    }
}

void Executor::ProfQueue(u32 next) {
    const u32 q = prof_queue_;
    prof_queue_ = next;
    ProfMark((kProfQueueTag << 32) | q,
             fmt::format("queue {} runs: its commands with their barriers, uploads, shadows", q));
}

void Executor::ProfCollect(Batch& b) {
    std::vector<u64> ts(b.prof_count);
    if (b.prof_count > 1 &&
        Fn().vkGetQueryPoolResults(ctx_.Device(), prof_pool_, b.prof_base, b.prof_count, ts.size() * 8, ts.data(), 8,
                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const double period = ctx_.Caps().timestamp_period;
        for (u32 i = 1; i < b.prof_count; ++i) {
            const u64 dt = ts[i] >= ts[i - 1] ? static_cast<u64>(static_cast<double>(ts[i] - ts[i - 1]) * period) : 0;
            auto& acc = prof_acc_[b.prof_keys[i - 1]];
            acc.ns += dt;
            ++acc.count;
            prof_total_ns_ += dt;
        }
    }
    b.prof_base = ~0u;
    b.prof_count = 0;
    b.prof_keys.clear();
}

void Executor::PrepareShadows(const void* plan_ptr, std::vector<VkDescriptorBufferInfo>& infos, u64 workgroups) {
    // Measured (logs\p44\diag c17-c20): a shader's stores into the sparse mirror are ~150x slower than into a dense
    // buffer on NVIDIA (the skinning dispatch 1196 us vs 7.7 us with only its outputs dense), its loads are not; a copy
    // into the mirror costs 12 us for its 420 KiB. So a dispatch writes dense shadows, copied back after it.
    static const bool off = [] {
        char v[8] = {};
        const bool o = GetEnvironmentVariableA("SSPC_DIAG_WRITE_SHADOW", v, sizeof(v)) == 3 && std::string_view(v) == "off";
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_WRITE_SHADOW=off - dispatches write the mirror in place");
        }
        return o;
    }();
    // a small dispatch writes little: its stores into the mirror cost less than the shadows' two barriers and copies
    // (j1: ~3 global barriers a dispatch, two of them the shadows', ~350 dispatches a frame, most of them 1x1x1)
    static const u64 kMinGroups = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_SHADOW_MIN_GROUPS", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10)
                                                                                        : 8ull;
    }();
    shadows_.clear();
    if (shadow_ring_ == nullptr || off || workgroups < kMinGroups) {
        return;
    }
    const auto& plan = *static_cast<const GS::ResourceBindingPlan*>(plan_ptr);
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    // the whole binding is copied in and out: a large one a shader writes a little of costs more than it saves.
    // Measured on the reference route (d1-d3): no limit 33.9 ms GPU a frame (149 MiB copied each way a frame), 1 MiB
    // 35.6 ms (the 1x1x1 dispatches' 1 MiB bindings copied), 256 KiB 29.6 ms (2 MiB a frame) - written bindings are
    // <=64 KiB, ~140 KiB (the skinning), ~1 MiB or ~16 MiB, nothing between.
    static const u64 kShadowMax = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_SHADOW_MAX_KIB", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) << 10
                                                                                     : (256ull << 10);
    }();
    const size_t n = std::min(plan.buffers.size(), infos.size());
    for (size_t k = 0; k < n; ++k) {
        const auto& b = plan.buffers[k];
        VkDescriptorBufferInfo& info = infos[k];
        if (b.kind != GS::BufferKind::Guest || !b.written || b.size == 0 || info.buffer == VK_NULL_HANDLE ||
            info.range == VK_WHOLE_SIZE || info.range == 0) {
            continue;
        }
        ++stats_.shadow_sizes[info.range <= (64u << 10)    ? 0
                              : info.range <= (256u << 10) ? 1
                              : info.range <= (1u << 20)   ? 2
                              : info.range <= (4u << 20)   ? 3
                              : info.range <= (16u << 20)  ? 4
                                                           : 5];
        if (info.range > kShadowMax) {
            ++stats_.shadow_too_large;
            continue;
        }
        // another binding of the same memory would see the other copy: those stay in place
        bool overlap = false;
        for (size_t j = 0; j < plan.buffers.size() && !overlap; ++j) {
            const auto& o = plan.buffers[j];
            overlap = j != k && o.kind == GS::BufferKind::Guest && o.size != 0 && o.address < b.address + b.size &&
                      b.address < o.address + o.size;
        }
        if (overlap) {
            ++stats_.shadow_overlaps;
            continue;
        }
        StreamRing::Slice sl{};
        if (shadow_ring_->Allocate(info.range, align, cur_.value, sl) != StreamRing::Status::Ok) {
            ++stats_.shadow_no_room;
            continue;
        }
        // only the binding's own bytes are copied: the descriptor starts up to `align` - 1 bytes before them (aligned
        // down), bytes that may be another binding's - adjacent outputs (the skinning's three) copied back whole would
        // put their stale copy over the other's result (synchronization validation d4/d6: WRITE_AFTER_WRITE)
        const u64 adjust = k < buf_adjust_.size() ? buf_adjust_[k] : 0;
        if (adjust >= info.range) {
            continue;
        }
        shadows_.push_back({info.buffer, sl.buffer, info.offset + adjust, sl.offset + adjust,
                            std::min<u64>(b.size, info.range - adjust)});
        info.buffer = sl.buffer;
        info.offset = sl.offset; // aligned as the original: the shader's offset within it is the same
    }
    if (shadows_.empty()) {
        return;
    }
    // the current contents in (the shader may read what it does not overwrite); the caller ordered the earlier writes
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier(1);
    }
    for (const Shadow& s : shadows_) {
        const VkBufferCopy c{.srcOffset = s.home_offset, .dstOffset = s.shadow_offset, .size = s.size};
        Fn().vkCmdCopyBuffer(cur_.cb, s.home, s.shadow, 1, &c);
        ++stats_.shadows;
        stats_.shadow_bytes += s.size;
    }
    GlobalBarrier(2);
}

void Executor::FinishShadows() {
    if (shadows_.empty()) {
        return;
    }
    GlobalBarrier(3); // the dispatch's writes before the copies
    for (const Shadow& s : shadows_) {
        const VkBufferCopy c{.srcOffset = s.shadow_offset, .dstOffset = s.home_offset, .size = s.size};
        Fn().vkCmdCopyBuffer(cur_.cb, s.shadow, s.home, 1, &c);
    }
    shadows_.clear();
    pending_writes_ = true; // a later access of the memory waits for the copies
}

void Executor::DiagDumpShader(u64 address, const std::vector<u32>* code, const std::vector<u32>& spirv,
                              const GS::RuntimeParams& runtime, u32 pipeline) {
    // SSPC_DIAG_DUMP_SHADER=<hex address>[,<hex address>...]: each distinct translation (SPIR-V) of those programs, with
    // its GCN code and runtime parameters -> logs\p44\shader_<address>_<n>.{gcn,spv,rt,txt} (P4.5: the same variant
    // compared with the bridge's)
    static const std::vector<u64> wanted = [] {
        std::vector<u64> out;
        char v[512] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_DUMP_SHADER", v, sizeof(v)) > 0) {
            for (char* p = v; *p != 0;) {
                char* end = nullptr;
                const u64 a = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                out.push_back(a);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return out;
    }();
    if (wanted.empty() || std::find(wanted.begin(), wanted.end(), address) == wanted.end() || dumped_shaders_.size() >= 64) {
        return;
    }
    u64 h = 1469598103934665603ull; // FNV-1a over the SPIR-V and the runtime parameters
    const auto mix = [&h](const void* data, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            h = (h ^ static_cast<const u8*>(data)[i]) * 1099511628211ull;
        }
    };
    mix(spirv.data(), spirv.size() * 4);
    mix(&runtime, sizeof(runtime));
    if (!dumped_shaders_.insert(h).second) {
        return;
    }
    const auto write = [](const std::filesystem::path& path, const void* data, size_t bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        return static_cast<bool>(out);
    };
    // next to the frame capture (logs\p44)
    const std::filesystem::path base =
        options_.capture_path.parent_path() / fmt::format("shader_{:x}_{:016x}", address, h);
    bool ok = true;
    if (code != nullptr) {
        ok &= write(std::filesystem::path(base).replace_extension(".gcn"), code->data(), code->size() * 4);
    }
    ok &= write(std::filesystem::path(base).replace_extension(".spv"), spirv.data(), spirv.size() * 4);
    ok &= write(std::filesystem::path(base).replace_extension(".rt"), &runtime, sizeof(runtime));
    std::string t = fmt::format("address={:#x} pipeline={} hw={} rsrc1={:#x} rsrc2={:#x} ps_input_ena={:#x} "
                                "ps_input_addr={:#x} num_interp={} col_format={:#x} z_format={} db_shader_control={:#x} "
                                "aa_samples_log2={} dual_source={}\n",
                                address, pipeline, static_cast<u32>(runtime.hw), runtime.rsrc1, runtime.rsrc2,
                                runtime.ps_input_ena, runtime.ps_input_addr, runtime.num_interp, runtime.col_format,
                                runtime.z_format, runtime.db_shader_control, runtime.aa_samples_log2, runtime.dual_source);
    for (u32 i = 0; i < runtime.targets.size(); ++i) {
        const auto& c = runtime.targets[i];
        if (c.bound != 0) {
            t += fmt::format("target {}: info={:#x} min_max_self={}\n", i, c.info, c.min_max_self);
        }
    }
    for (u32 i = 0; i < runtime.num_interp && i < runtime.ps_input_cntl.size(); ++i) {
        t += fmt::format("ps_input_cntl[{}]={:#x}\n", i, runtime.ps_input_cntl[i]);
    }
    ok &= write(std::filesystem::path(base).replace_extension(".txt"), t.data(), t.size());
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: shader {:#x} variant {:016x} (pipeline {}) dumped to {}.* ({} SPIR-V words): {}",
             address, h, pipeline, base.string(), spirv.size(), ok ? "written" : "NOT written");
}

void Executor::ProfDescribe(u64 key, const char* what, const void* stage_view, const std::vector<CachedImage*>& bound) {
    if (!prof_described_.insert(key).second) {
        return;
    }
    const auto& s = *static_cast<const GS::StageView*>(stage_view);
    std::string out = fmt::format("profile key {:#x} ({}) bindings:", key, what);
    for (const auto& b : s.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest) {
            // where the GPU reads it: video memory (the cache's mirror), the stream ring or the host import (PCIe)
            BufferCache::Range r{};
            const BufferCache::Residency res = buffers_.Classify(b.address, b.size, r);
            const char* where = res == BufferCache::Residency::All ? "vram"
                                : !b.written && b.size <= 16 * 1024 ? "ring"
                                                                     : (res == BufferCache::Residency::Partial ? "partial"
                                                                                                                : "HOST");
            out += fmt::format(" | buf {:#x}+{:#x}{} [{}]", b.address, b.size, b.written ? " WRITTEN" : "", where);
        } else {
            out += fmt::format(" | special kind {}", static_cast<int>(b.kind));
        }
    }
    for (const CachedImage* img : bound) {
        out += fmt::format(" | img{} {:#x} fmt {} {}x{} lv {} ly {}{}{}", img->id, img->layout.guest_address,
                           img->layout.pixel_format, img->layout.size.width, img->layout.size.height, img->layout.levels,
                           img->layout.layers, img->gpu_written ? " gpu-written" : "",
                           (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? " storage-usage" : "");
    }
    for (const auto& v : s.plan.vertices) {
        out += fmt::format(" | vertex {:#x}+{:#x} stride {}", v.address, v.size, v.desc.Stride());
    }
    SSPC_LOG(GPU, Info, "{}", out);
}

void Executor::ProfReport() {
    if (prof_frames_ == 0 || prof_total_ns_ == 0) {
        return;
    }
    std::vector<std::pair<u64, const ProfAcc*>> v;
    for (const auto& [k, a] : prof_acc_) {
        if (a.ns != 0) {
            v.push_back({k, &a});
        }
    }
    std::ranges::sort(v, [](const auto& x, const auto& y) { return x.second->ns > y.second->ns; });
    std::string out = fmt::format("GPU profile over {} frames: {:.2f} ms GPU per frame in commands; the top:\n", prof_frames_,
                                  static_cast<double>(prof_total_ns_) / 1e6 / static_cast<double>(prof_frames_));
    {
        // by kind (P4.6-G3): draws (key low bits 0), dispatches (1, of them single-workgroup ones), the rest (small keys)
        double ns[3] = {}, ns_1x1 = 0;
        u64 n[3] = {}, n_1x1 = 0;
        for (const auto& [k, a] : prof_acc_) {
            const bool other = (k < 8 && (k & 3) != 1) || (k >> 32) == 0xD1A6 || // ProfMark's 2..5, the DIAG keys,
                               (k >> 32) == kProfQueueTag;                        // the queue runs (=q)
            const u32 kind = other ? 2 : (k & 3) == 0 ? 0 : (k & 3) == 1 ? 1 : 2;
            ns[kind] += static_cast<double>(a.ns);
            n[kind] += a.count;
            if (kind == 1 && a.label.ends_with("groups 1x1x1")) {
                ns_1x1 += static_cast<double>(a.ns);
                n_1x1 += a.count;
            }
        }
        const double fr = static_cast<double>(prof_frames_);
        out += fmt::format("  by kind (ms/frame, commands/frame): draws {:.2f} ({:.0f}), dispatches {:.2f} ({:.0f}; "
                           "keys first seen as 1x1x1: {:.2f} ({:.0f})), other {:.2f} ({:.0f})\n",
                           ns[0] / 1e6 / fr, n[0] / fr, ns[1] / 1e6 / fr, n[1] / fr, ns_1x1 / 1e6 / fr, n_1x1 / fr,
                           ns[2] / 1e6 / fr, n[2] / fr);
    }
    for (size_t i = 0; i < v.size() && i < 30; ++i) {
        const ProfAcc& a = *v[i].second;
        out += fmt::format("  {:5.2f} ms/frame {:4.1f}% x{:6} ({:7.1f} us each)  {}\n",
                           static_cast<double>(a.ns) / 1e6 / static_cast<double>(prof_frames_),
                           100.0 * static_cast<double>(a.ns) / static_cast<double>(prof_total_ns_), a.count,
                           static_cast<double>(a.ns) / 1e3 / static_cast<double>(std::max<u64>(a.count, 1)), a.label);
    }
    // the diagnostic experiments' intervals whatever their rank (SSPC_DIAG_DUP_DISPATCH)
    for (u64 k = 6; k <= 13; ++k) {
        if (const auto it = prof_acc_.find(DiagKey(k)); it != prof_acc_.end()) {
            const ProfAcc& a = it->second;
            out += fmt::format("  (diag) {:5.3f} ms/frame x{:6} ({:7.1f} us each)  {}\n",
                               static_cast<double>(a.ns) / 1e6 / static_cast<double>(prof_frames_), a.count,
                               static_cast<double>(a.ns) / 1e3 / static_cast<double>(std::max<u64>(a.count, 1)), a.label);
        }
    }
    SSPC_LOG(GPU, Info, "{}", out);
    {
        // every key of this report (P4.6-G3: totals by queue / workgroup count offline), appended per report
        static std::ofstream full(options_.capture_path.parent_path().parent_path() / "gpu_profile_full.txt",
                                  std::ios::trunc);
        full << fmt::format("report frames {} total_ms_per_frame {:.3f}\n", prof_frames_,
                            static_cast<double>(prof_total_ns_) / 1e6 / static_cast<double>(prof_frames_));
        for (const auto& [k, a] : v) {
            full << fmt::format("{:#x}\t{:.4f}\t{}\t{}\n", k,
                                static_cast<double>(a->ns) / 1e6 / static_cast<double>(prof_frames_), a->count, a->label);
        }
        full.flush();
    }
    prof_acc_.clear();
    prof_frames_ = 0;
    prof_total_ns_ = 0;
}

u64 Executor::DiagBegin(std::string text) {
    if (diag_ == nullptr || !diag_->Enabled()) {
        return 0;
    }
    const u64 id = diag_->Begin(cur_.cb, cur_.value, std::move(text));
    if (batch_first_cmd_ == 0) {
        batch_first_cmd_ = id;
    }
    batch_last_cmd_ = id;
    return id;
}

void Executor::DiagEnd(u64 id) {
    if (id != 0) {
        diag_->End(cur_.cb, id);
    }
}

bool Executor::GuestRange(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail) {
    const u64 t0 = GS::FineTicks();
    const bool ok = GuestRangeImpl(addr, size, write, align, buffer, offset, avail);
    stats_.t_range += GS::FineTicks() - t0;
    return ok;
}

bool Executor::GuestRangeImpl(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail) {
    constexpr u64 kStreamMax = 16 * 1024; // the bridge's STREAM_THRESHOLD (16 KiB)
    if (!write && size <= kStreamMax) {
        BufferCache::Range r;
        u64 tt = GS::FineTicks();
        const BufferCache::Residency res = buffers_.Classify(addr, size, r);
        stats_.t_classify += GS::FineTicks() - tt;
        if (res == BufferCache::Residency::All) {
            buffer = r.buffer;
            offset = r.offset;
            avail = r.size;
            return true;
        }
        // nothing of it in video memory (the CPU wrote it, or it is new): the host's copy is current; a small read
        // is copied by the CPU into the stream ring (video memory) - no upload, no barrier, no break of rendering (the
        // bridge's stream buffer). The copy reads the physical backing's view (never faults).
        // P4.6 night: a range whose blocks the CPU has not written lately is cached instead (no copy each use; the
        // tracker invalidates it at the CPU's next write and its block streams for a while). SSPC_DIAG_STREAM_PROMOTE=off:
        // always streamed (the P4.6 behaviour).
        static const bool promote = [] {
            char v[8] = {};
            const bool off = GetEnvironmentVariableA("SSPC_DIAG_STREAM_PROMOTE", v, sizeof(v)) == 3 &&
                             std::string_view(v) == "off";
            if (off) {
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_STREAM_PROMOTE=off - small read-only ranges are "
                                       "always streamed");
            }
            return !off;
        }();
        const bool promoted = res == BufferCache::Residency::None && promote &&
                              buffers_.Promotable(addr, size, VideoOut::NextFlipId());
        if (promoted) {
            ++stats_.stream_promotions;
        }
        if (res == BufferCache::Residency::None && !promoted && !buffers_.HostWritePending(addr, size) &&
            !TimelineWritePending(addr, size)) {
            // P4.7: these bytes streamed earlier in this batch and nothing written since - that slice again
            StreamMemo* memo = nullptr;
            const u64 gen = frontend_.MemoryGeneration();
            u64 verify = 0; // SSPC_DIAG_STREAM_REUSE=verify: the hash of the slice a reuse would have taken
            if (stream_reuse_) {
                if (stream_memo_.empty()) {
                    stream_memo_.resize(kStreamMemo);
                }
                memo = &stream_memo_[((addr >> 2) * 0x9E3779B97F4A7C15ull ^ size) >> (64 - 12)];
                if (memo->addr == addr && memo->size == size && memo->batch == cur_.value && memo->gen == gen &&
                    (memo->offset & (std::max<u64>(align, 4) - 1)) == 0) {
                    if (!stream_verify_) {
                        buffer = memo->buffer;
                        offset = memo->offset;
                        avail = size;
                        ++stats_.stream_reused;
                        stats_.stream_reused_bytes += size;
                        return true;
                    }
                    verify = memo->hash | 1;
                }
            }
            StreamRing::Slice sl{};
            tt = GS::FineTicks();
            const u8* host = guest_.BackingPointer(addr, size);
            if (host == nullptr) { // no window there yet: imported once (32 MiB), then the copies read its host view
                GuestBuffers::View gv{};
                GuestBuffers::Failure gfl{};
                if (guest_.Get(addr, size, cur_.value, gv, gfl)) {
                    host = guest_.BackingPointer(addr, size);
                }
            }
            stats_.t_hostptr += GS::FineTicks() - tt;
            tt = GS::FineTicks();
            const bool allocated = ring_.Allocate(size, std::max<u64>(align, 4), cur_.value, sl) == StreamRing::Status::Ok;
            stats_.t_alloc += GS::FineTicks() - tt;
            tt = GS::FineTicks();
            const bool copied =
                allocated &&
                (host != nullptr ? (std::memcpy(sl.data, host, size), true) : GS::GameMemory().Read(addr, sl.data, size));
            stats_.t_copy += GS::FineTicks() - tt;
            if (copied) {
                buffer = sl.buffer;
                offset = sl.offset;
                avail = size;
                ++stats_.streamed;
                stats_.streamed_bytes += size;
                if (memo != nullptr) {
                    const u64 h = stream_verify_ ? RaceHash(sl.data, size) : 0;
                    if (verify != 0) {
                        ++stats_.stream_verified;
                        stats_.stream_mismatches += (h | 1) != verify ? 1 : 0;
                    }
                    *memo = {addr, size, cur_.value, gen, sl.buffer, sl.offset, h};
                }
                // SSPC_DIAG_WATCH_DUMP=<hex address>: the bytes copied for a range starting there, per copy ->
                // logs\p45\watch_dump.bin (flip id, size, bytes)
                static const u64 dump_addr = [] {
                    char v[32] = {};
                    return GetEnvironmentVariableA("SSPC_DIAG_WATCH_DUMP", v, sizeof(v)) > 0
                               ? std::strtoull(v, nullptr, 16)
                               : 0ull;
                }();
                if (dump_addr != 0 && addr == dump_addr) {
                    static std::ofstream dump(options_.capture_path.parent_path().parent_path() / "p45" / "watch_dump.bin",
                                              std::ios::binary | std::ios::trunc);
                    const u64 hdr[2] = {VideoOut::NextFlipId(), size};
                    dump.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                    dump.write(reinterpret_cast<const char*>(sl.data), static_cast<std::streamsize>(size));
                    dump.flush();
                }
                if (const u64 wp = g_watch_page.load(std::memory_order_relaxed);
                    wp != 0 && addr < wp + 4096 && wp < addr + size) {
                    race_.watch_copy_tsc = __rdtsc(); // this frame's copy of the watched page
                    race_.watch_copy_hits = g_watch_n.load(std::memory_order_acquire);
                }
                // SSPC_DIAG_STREAM_STATS=1: is a streamed range a repeat in the frame / unchanged since the last frame
                // (counts in gpu_native.txt)
                static const bool stream_stats = [] {
                    char v[4] = {};
                    return GetEnvironmentVariableA("SSPC_DIAG_STREAM_STATS", v, sizeof(v)) == 1 && v[0] == '1';
                }();
                if (stream_stats) {
                    const u64 h = RaceHash(sl.data, size);
                    const u64 flip = VideoOut::NextFlipId();
                    auto& e = stream_seen_[(addr << 16) ^ size];
                    const u32 kind = e.flip == 0 ? 0 : e.flip == flip ? (e.hash == h ? 1 : 2) : (e.hash == h ? 3 : 4);
                    ++stats_.stream_kinds[kind];
                    stats_.stream_kind_bytes[kind] += size;
                    e = {h, flip};
                }
                if (race_.on) {
                    const u64 h = RaceHash(sl.data, size);
                    race_.copies.push_back({addr, size, h});
                    // a range that changed after last frame's copy: is this frame's copy what the flip saw then
                    // (the next frame's data written early) or yet another value (written late in each frame)?
                    if (const auto it = race_.post.find(addr); it != race_.post.end()) {
                        ++(it->second == h ? race_.next_equal : race_.next_differ);
                        race_.post.erase(it);
                    }
                }
                return true;
            }
        }
    }
    BufferCache::Range r;
    const u64 tp = GS::FineTicks();
    const bool prepared = buffers_.Prepare(addr, size, write, cur_.cb, cur_.value, r);
    stats_.t_prepare += GS::FineTicks() - tp;
    ++stats_.n_prepare;
    if (!prepared) {
        return false;
    }
    buffer = r.buffer;
    offset = r.offset;
    avail = r.size;
    return true;
}

void Executor::TimelineWriteRanges(std::vector<std::pair<u64, u64>>& out) const {
    out.clear();
    for (const TimelineWrite& w : timeline_writes_) {
        out.emplace_back(w.begin, w.end);
    }
}

void Executor::CompleteRefusedWork(u32 queue, u64 seq) {
    if (!Begin()) {
        return;
    }
    Note(queue, seq);
    ++commands_; // (a batch with it is submitted at the round's end like any other: its completion completes `seq`)
}

bool Executor::TimelineWritePending(u64 addr, u64 bytes) const {
    if (timeline_writes_.empty() || addr >= tw_hi_ || addr + bytes <= tw_lo_) {
        return false;
    }
    return std::ranges::any_of(timeline_writes_,
                               [&](const TimelineWrite& w) { return addr < w.end && w.begin < addr + bytes; });
}

bool Executor::TimelineReader::Read(u64 addr, void* dst, u64 bytes) {
    if (e_.TimelineWritePending(addr, bytes)) {
        ++e_.stats_.timeline_read_syncs;
        e_.SyncFrom(1);
    } else if (e_.buffers_.GpuWritePending(addr, bytes, e_.timeline_.CompletedCached()) ||
               e_.buffers_.HostWritePending(addr, bytes)) {
        // a shader's write the host does not have yet (its batch has not completed): the frontend reads after it
        ++e_.stats_.gpu_write_read_syncs;
        e_.SyncFrom(1);
    }
    // P4.7: a run of the physical backing read before (its host view: the same bytes CopySparse copies)
    if (e_.map_gen_ != nullptr && GuestMemory::IsValidGpuMapping(addr, bytes)) {
        const u64 gen = e_.map_gen_->load(std::memory_order_acquire);
        for (const Run& r : runs_) {
            if (r.gen == gen && r.host != nullptr && addr >= r.begin && addr + bytes <= r.end) {
                std::memcpy(dst, r.host + (addr - r.begin), bytes);
                ++e_.stats_.read_run_hits;
                if (e_.read_verify_) {
                    Verify(addr, dst, bytes);
                }
                return true;
            }
        }
        const u64 block = (addr >> 20) + 1;
        const bool known_none = std::ranges::any_of(no_run_, [&](const auto& n) { return n.first == block && n.second == gen; });
        if (GuestMemory* gm = GetGuestMemory(); gm != nullptr && !known_none) {
            u8* host = nullptr;
            u64 rb = 0, rs = 0;
            if (gm->BackingRun(addr, &host, &rb, &rs) && host != nullptr && addr >= rb && addr + bytes <= rb + rs) {
                runs_[next_run_++ % runs_.size()] = {rb, rb + rs, host, gen};
                std::memcpy(dst, host + (addr - rb), bytes);
                ++e_.stats_.read_run_misses;
                if (e_.read_verify_) {
                    Verify(addr, dst, bytes);
                }
                return true;
            }
            no_run_[next_no_run_++ % no_run_.size()] = {block, gen};
        }
        ++e_.stats_.read_run_none;
    }
    return GS::GameMemory().Read(addr, dst, bytes);
}

void Executor::TimelineReader::Verify(u64 addr, const void* dst, u64 bytes) {
    verify_.resize(bytes);
    const bool read = GS::GameMemory().Read(addr, verify_.data(), bytes);
    ++e_.stats_.read_run_verified;
    e_.stats_.read_run_mismatches += !read || std::memcmp(verify_.data(), dst, bytes) != 0 ? 1 : 0;
}

bool Executor::WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    batch_fence_ = true;
    // vkCmdUpdateBuffer: 4-byte aligned, up to 64 KiB; guest memory only (VideoOut's labels are host memory)
    if (bytes == 0 || bytes > 65536 || (dst & 3) != 0 || (bytes & 3) != 0 || dst >= (1ull << 40)) {
        ++stats_.tw_fail_shape;
        return false;
    }
    if (!Begin()) {
        return false;
    }
    GuestBuffers::View v{};
    GuestBuffers::Failure fl{};
    if (!guest_.Get(dst, bytes, cur_.value, v, fl)) {
        ++stats_.tw_fail_import;
        return false;
    }
    Note(queue, seq);
    ++commands_;
    if (LabelChanges(kLabelOther | 1)) {
        Label(kLabelOther | 1, "T WRITE_DATA on the timeline");
    }
    // after the earlier work's accesses of this memory, before the later work's: one barrier for a run of writes to
    // different ranges (the next draw's barrier follows through pending_writes_)
    const bool overlap = std::ranges::any_of(run_ranges_, [&](const std::pair<u64, u64>& r) {
        return dst < r.second && r.first < dst + bytes;
    });
    if (!write_run_ || overlap) {
        GlobalBarrier(4);
        run_ranges_.clear();
    }
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? DiagBegin(fmt::format("timeline write q{} seq {} {:#x}+{}", queue, seq, dst, bytes))
                        : 0;
    // this batch's GPU writes there reach the host first and leave the end's write-back (it would undo this write)
    buffers_.FlushWrittenForHostWrite(dst, bytes, cur_.cb, cur_.value);
    Fn().vkCmdUpdateBuffer(cur_.cb, v.buffer, v.offset, bytes, data);
    DiagEnd(cid);
    buffers_.Invalidate(dst, bytes); // the host's copy is newer: uploaded again (after this write) at the next use
    MarkWrites(3);
    write_run_ = true;
    run_ranges_.emplace_back(dst, dst + bytes);
    timeline_writes_.push_back({dst, dst + bytes, cur_.value});
    ++tw_version_;
    tw_lo_ = std::min(tw_lo_, dst);
    tw_hi_ = std::max(tw_hi_, dst + bytes);
    images_.Invalidate(dst, bytes); // textures there are uploaded again (after the write, in stream order)
    ++stats_.timeline_writes;
    return true;
}

bool Executor::CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    // guest memory only (VideoOut's labels are host memory); overlapping ranges are a memmove the CPU does
    if (bytes == 0 || src >= (1ull << 40) || dst >= (1ull << 40) || (src < dst + bytes && dst < src + bytes)) {
        ++stats_.tc_refused;
        return false;
    }
    if (memo_bumps_) {
        frontend_.GpuWrote(dst, bytes); // (P5.2: the frontend runs on this thread; trusted shader code there is re-read)
    }
    if (!Begin()) {
        return false;
    }
    write_run_ = false;
    if (LabelChanges(kLabelOther | 2)) {
        Label(kLabelOther | 2, "T DMA copy on the timeline");
    }
    // a copy: outside rendering, after the earlier writes (of its source too) - unless it touches none of them
    EndRendering();
    acc_reads_.assign(1, {src, src + bytes});
    acc_writes_.assign(1, {dst, dst + bytes});
    if (NeedsBarrier(acc_reads_, acc_writes_, false)) {
        GlobalBarrier(5);
    }
    VkBuffer sbuf = VK_NULL_HANDLE, dbuf = VK_NULL_HANDLE;
    u64 soff = 0, doff = 0, savail = 0, davail = 0;
    if (!GuestRange(src, bytes, false, 4, sbuf, soff, savail) || !GuestRange(dst, bytes, true, 4, dbuf, doff, davail)) {
        ++stats_.tc_refused;
        return false; // not noted: the processor copies (after Synchronize) and the sequence number stays its own
    }
    // noted only when recorded: a refused copy noted here would complete the queue's next work number with this batch
    // (review MEDIUM 3)
    Note(queue, seq);
    ++commands_;
    // the ranges' uploads (if any) ended rendering and ordered themselves; the copy after them
    EndRendering();
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? DiagBegin(fmt::format("timeline copy q{} seq {} {:#x} -> {:#x} ({} bytes)", queue, seq, src, dst,
                                                bytes))
                        : 0;
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    InspMark(Inspector::kGpuTransfer);
    const VkBufferCopy region{.srcOffset = soff, .dstOffset = doff, .size = bytes};
    Fn().vkCmdCopyBuffer(cur_.cb, sbuf, dbuf, 1, &region);
    DiagEnd(cid);
    if (prof_) {
        ProfMark(4, "DMA copy on the timeline");
    }
    // a later read of the destination waits for the copy (the ranges again: GuestRange reuses the lists)
    acc_reads_.assign(1, {src, src + bytes});
    acc_writes_.assign(1, {dst, dst + bytes});
    NoteAccesses(acc_reads_, acc_writes_, false);
    // the CPU side (the processor's reads, the frontend's descriptor reads) waits for it
    timeline_writes_.push_back({dst, dst + bytes, cur_.value});
    ++tw_version_;
    tw_lo_ = std::min(tw_lo_, dst);
    tw_hi_ = std::max(tw_hi_, dst + bytes);
    images_.Invalidate(dst, bytes); // textures there are uploaded again (they read the video memory copy, after it)
    ++stats_.timeline_copies;
    stats_.timeline_copy_bytes += bytes;
    return true;
}

bool Executor::GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                             u32 pattern) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    // GDS is gds_ (a storage buffer the shaders bind): its DMA recorded in stream order instead of the processor
    // waiting for the GPU and touching the mapped buffer (P4.6). Guest memory only on the other side.
    if (bytes == 0 || (gds_offset & 3) != 0 || (bytes & 3) != 0 || gds_.buffer == VK_NULL_HANDLE ||
        u64{gds_offset} + bytes > gds_.size || (kind != Cp::DmaKind::DataToGds && memory >= (1ull << 40)) ||
        (kind != Cp::DmaKind::DataToGds && kind != Cp::DmaKind::MemoryToGds && kind != Cp::DmaKind::GdsToMemory)) {
        ++stats_.gds_refused;
        return false;
    }
    if (!Begin()) {
        return false;
    }
    write_run_ = false;
    if (LabelChanges(kLabelOther | 3)) {
        Label(kLabelOther | 3, "T GDS DMA on the timeline");
    }
    // after every earlier access (the shaders' GDS atomics are storage writes; a fill/copy over GDS must also wait for
    // their reads): one full barrier
    GlobalBarrier(5);
    VkBuffer mbuf = VK_NULL_HANDLE;
    u64 moff = 0, mavail = 0;
    if (kind != Cp::DmaKind::DataToGds &&
        (!GuestRange(memory, bytes, kind == Cp::DmaKind::GdsToMemory, 4, mbuf, moff, mavail) || mavail < bytes)) {
        ++stats_.gds_refused;
        return false; // not noted: the processor copies after Synchronize
    }
    Note(queue, seq);
    ++commands_;
    EndRendering(); // GuestRange may have uploaded (it ordered itself)
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? DiagBegin(fmt::format("timeline GDS q{} seq {} kind {} memory {:#x} GDS +{:#x} ({} bytes)",
                                                queue, seq, static_cast<u32>(kind), memory, gds_offset, bytes))
                        : 0;
    const auto& f = Fn();
    switch (kind) {
    case Cp::DmaKind::DataToGds:
        f.vkCmdFillBuffer(cur_.cb, gds_.buffer, gds_offset, bytes, pattern);
        break;
    case Cp::DmaKind::MemoryToGds: {
        const VkBufferCopy region{.srcOffset = moff, .dstOffset = gds_offset, .size = bytes};
        f.vkCmdCopyBuffer(cur_.cb, mbuf, gds_.buffer, 1, &region);
        break;
    }
    default: {
        const VkBufferCopy region{.srcOffset = gds_offset, .dstOffset = moff, .size = bytes};
        f.vkCmdCopyBuffer(cur_.cb, gds_.buffer, mbuf, 1, &region);
        // the CPU side (the processor's reads, the frontend's descriptor reads) waits for it
        timeline_writes_.push_back({memory, memory + bytes, cur_.value});
        ++tw_version_;
        tw_lo_ = std::min(tw_lo_, memory);
        tw_hi_ = std::max(tw_hi_, memory + bytes);
        images_.Invalidate(memory, bytes);
        break;
    }
    }
    DiagEnd(cid);
    MarkWrites(4); // a later access of either side waits for it
    ++stats_.timeline_gds;
    return true;
}

namespace {
/// SSPC_DIAG_ACQUIRE_DRAIN=off: the P4.4 behaviour (CPU writes applied only between the backend's loop rounds) - a
/// control run for the A/B, not a configuration
bool AcquireDrainOff() {
    static const bool off = [] {
        char v[8] = {};
        const bool o = GetEnvironmentVariableA("SSPC_DIAG_ACQUIRE_DRAIN", v, sizeof(v)) == 3 && std::string_view(v) == "off";
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_ACQUIRE_DRAIN=off - CPU writes before a met "
                                   "WAIT_REG_MEM are not applied before the work after it");
        }
        return o;
    }();
    return off;
}
} // namespace

void Executor::MemoryAcquired(u32 queue, bool blocked, u64 poll_address) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    GuestSync(queue); // (a met WAIT_REG_MEM: the guest's own ordering point)
    ++stats_.acquires;
    if (blocked) {
        last_unblock_tsc_ = __rdtsc();
    }
    if (g_watch_page.load(std::memory_order_relaxed) != 0 && race_.acquires.size() < 64) {
        race_.acquires.emplace_back(__rdtsc(), poll_address);
    }
    stats_.acquires_blocked += blocked ? 1 : 0;
    if (RunAcquireHook()) {
        ++stats_.acquires_applied;
    }
}

bool Executor::RunAcquireHook() const {
    return !AcquireDrainOff() && acquire_hook_ != nullptr && acquire_hook_(acquire_ctx_);
}

void Executor::MemoryAcquiredApplied(u32 queue, bool blocked, u64 poll_address, u64 unblock_tsc, bool applied) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb);
    GuestSync(queue);
    ++stats_.acquires;
    if (blocked) {
        last_unblock_tsc_ = unblock_tsc;
    }
    if (g_watch_page.load(std::memory_order_relaxed) != 0 && race_.acquires.size() < 64) {
        race_.acquires.emplace_back(unblock_tsc != 0 ? unblock_tsc : __rdtsc(), poll_address);
    }
    stats_.acquires_blocked += blocked ? 1 : 0;
    stats_.acquires_applied += applied ? 1 : 0;
}

void Executor::WaitMetByHeldFence(u32 queue) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    GuestSync(queue);
    // the guest's GPU waits for its earlier work here (EVENT_WRITE_EOS + WAIT_REG_MEM): everything recorded after it
    // waits for everything before it - the next command's global barrier (the batch-end barrier covers later batches)
    ++stats_.waits_by_held;
    MarkWrites(2);
}

void Executor::BeforeUpload() {
    // an upload records copies (and a detiling dispatch): outside rendering, after the writes before it
    if (rendering_) {
        ++stats_.uploads_ended_rendering;
    }
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier(6);
    }
}

bool Executor::WaitOldest(u64 timeout_ns) {
    if (in_flight_.empty() || Failed()) {
        return false;
    }
    const bool done = timeline_.Wait(in_flight_.front().value, timeout_ns);
    if (!done && timeline_.Lost()) {
        OnDeviceLost("vkWaitSemaphores (oldest batch)", in_flight_.front().value);
    }
    return done;
}

void Executor::Synchronize(u32) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    SyncFrom(0);
}

void Executor::SyncFrom(u32 source) {
    // the CPU side of a packet must land after the GPU work before it: complete everything handed on so far
    ++stats_.syncs;
    GuestMemoryChanged(); // (P4.7: write-backs land in the host's memory)
    if (Failed()) {
        return;
    }
    if (!open_ && in_flight_.empty()) {
        return;
    }
    ++stats_.sync_eff[source % 3];
    const u64 t0 = GS::Ticks();
    if (open_ && commands_ > 0) {
        ++stats_.flushes_sync;
        Flush();
    }
    // P4.12: the wait for every batch submitted so far (its own flush above is in the Submit span)
    const StallTrace::Scope trace_sync(StallTrace::Ev::GpuSync, in_flight_.empty() ? 0 : in_flight_.back().value, 0,
                                       static_cast<u16>(source));
    // until done (a CPU access follows): a timeout without a lost device waits on (review MEDIUM 6)
    for (u32 round = 0; !Failed() && !in_flight_.empty() && !timeline_.Wait(in_flight_.back().value, 5'000'000'000ull); ++round) {
        if (timeline_.Lost()) {
            OnDeviceLost("vkWaitSemaphores (Synchronize)", in_flight_.back().value);
            break;
        }
        SSPC_LOG(GPU, Warning, "native backend: the GPU has not completed submission {} after {} s", in_flight_.back().value,
                 (round + 1) * 5);
    }
    const u64 us = GS::TicksToNs(GS::Ticks() - t0) / 1000;
    stats_.sync_wait_us += us;
    stats_.sync_eff_us[source % 3] += us;
}

void Executor::Fence(const Cp::FenceOp& op) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    if (op.deferred && !op.written && (op.data_sel == 3 || op.data_sel == 4) && op.kind != Cp::FenceKind::EndOfShader) {
        // a clock the processor writes once this work completed: the GPU's time here, measured by a timestamp
        if (open_ && cur_.clock_base != ~0u && cur_.clock_fences.size() < kClockPerBatch) {
            const u32 qi = cur_.clock_base + static_cast<u32>(cur_.clock_fences.size());
            Fn().vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, clock_pool_, qi);
            cur_.clock_fences.push_back({op.queue, op.packet_addr, op.after_seq, qi});
            ++stats_.clock_fences_placed;
        } else {
            ++stats_.clock_fences_unplaced;
        }
    }
    if (clock_label_diag_ && (op.data_sel == 3 || op.data_sel == 4) && op.addr != 0 && clock_labels_.size() < 64 &&
        std::ranges::find(clock_labels_, op.addr) == clock_labels_.end()) {
        clock_labels_.push_back(op.addr); // (diagnostics, SSPC-020: who reads the GPU clock labels)
    }
    batch_fence_ = true;
    GuestSync(op.queue);
    if (op.queue < guest_flush_.size()) {
        guest_flush_[op.queue] |= 1ull << 62; // (diagnostics: an end-of-shader/pipe event: counted as a flush too)
    }
    if (op.deferred && !op.written) {
        want_flush_ = true; // the processor holds it until the GPU completes the work before it
    }
}

void Executor::Interrupt(u32 queue, Cp::FenceKind) {
    if (queue == Cp::kGraphicsQueue) {
        Gnm::RaiseInterrupt(Gnm::Interrupt::GfxEop);
    } else {
        Gnm::RaiseInterrupt(static_cast<Gnm::Interrupt>(std::min<u32>(queue_pipe_[queue], 6)));
    }
}

void Executor::GuestMemoryChanged() {
    if (memo_bumps_) {
        frontend_.GuestMemoryChanged();
    }
}

bool Executor::SyncForFrontendRead(u64 addr, u64 bytes) {
    // (TimelineReader's checks: a write still on the GPU's timeline, or a shader's write the host does not have yet)
    if (TimelineWritePending(addr, bytes)) {
        ++stats_.timeline_read_syncs;
        SyncFrom(1);
        return true;
    }
    if (buffers_.GpuWritePending(addr, bytes, timeline_.CompletedCached()) || buffers_.HostWritePending(addr, bytes)) {
        ++stats_.gpu_write_read_syncs;
        SyncFrom(1);
        return true;
    }
    return false;
}

bool Executor::FrontendReadPending(u64 addr, u64 bytes) {
    return TimelineWritePending(addr, bytes) ||
           buffers_.GpuWritePending(addr, bytes, timeline_.CompletedCached()) || buffers_.HostWritePending(addr, bytes);
}

u64 Executor::RecordedValue() const {
    return open_ ? cur_.value : timeline_.LastSubmitted();
}

void Executor::SetMapGeneration(const std::atomic<u64>* gen) {
    char v[8] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_READ_RUNS", v, sizeof(v)) == 3 && std::string_view(v) == "off") {
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_READ_RUNS=off - every frontend read through the guest "
                               "memory manager");
        gen = nullptr;
    } else if (std::string_view(v) == "verify") {
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_READ_RUNS=verify - every read from a backing run is "
                               "read through the memory manager too and compared");
        read_verify_ = true;
    }
    map_gen_ = gen;
}

void Executor::Dma(const Cp::DmaOp& op) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    ++stats_.dma;
    GuestMemoryChanged(); // (P4.7)
    if (op.timeline) {
        return; // recorded by CopyOnTimeline: the copy keeps the caches coherent itself
    }
    if (op.executed && (op.kind == Cp::DmaKind::MemoryToMemory || op.kind == Cp::DmaKind::DataToMemory ||
                        op.kind == Cp::DmaKind::GdsToMemory)) {
        images_.Invalidate(op.dst, op.bytes); // the CPU side wrote guest memory images may have copies of
        buffers_.Invalidate(op.dst, op.bytes);
    }
}

void Executor::CacheAction(const Cp::CacheActionOp& op) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    GuestSync(op.queue);
    // (diagnostics: the guest's own flush between two of its dispatches - per queue, by event type; 63: ACQUIRE_MEM)
    const u32 type = op.event == 0 ? 63u : (op.event & 0x3F);
    ++stats_.guest_events[type];
    if (op.queue < guest_flush_.size()) {
        guest_flush_[op.queue] |= 1ull << type;
    }
    // the guest's cache flushes/invalidations (EVENT_WRITE, ACQUIRE_MEM): no barrier of their own, as the bridge (it
    // ignores them too). The hazards they order are ordered here per resource: a buffer write -> a barrier before the
    // next command (pending_writes_), an image -> its tracked access, a CPU side effect -> Synchronize, the host's
    // reads -> the batch's last barrier. Synchronization validation checks this on the reference route.
    ++stats_.cache_actions;
    write_run_ = false;
}

bool Executor::PushDescriptors(const PipelineObjects& p, VkPipelineBindPoint bind, std::vector<VkWriteDescriptorSet>& w) {
    const auto& f = Fn();
    if (w.empty()) {
        return true;
    }
    if (p.push_descriptors) {
        // (a whole push repeats in 0.04% of the draws - the flattened user data and streamed ranges take a new ring slice
        // each draw - but most of its descriptors do: P4.7 pushes only those that differ from what this command buffer
        // pushed with this layout; push descriptors are updated incrementally, a push with another layout makes them all
        // undefined - then the whole set again)
        if (bind != VK_PIPELINE_BIND_POINT_GRAPHICS || !bind_shadow_) {
            f.vkCmdPushDescriptorSetKHR(cur_.cb, bind, p.layout, 0, static_cast<u32>(w.size()), w.data());
            return true;
        }
        stats_.desc_wanted += w.size();
        const bool same_layout = p.layout == desc_layout_;
        if (!same_layout) {
            for (DescShadow& d : desc_shadow_) {
                d.valid = false;
            }
            desc_layout_ = p.layout;
        }
        desc_changed_.clear();
        for (const VkWriteDescriptorSet& x : w) {
            if (x.dstBinding >= desc_shadow_.size()) {
                desc_shadow_.resize(x.dstBinding + 1);
            }
            DescShadow& d = desc_shadow_[x.dstBinding];
            const bool buffer = x.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            const bool same = x.descriptorCount == 1 && d.valid && d.type == x.descriptorType &&
                              (buffer ? (d.buffer.buffer == x.pBufferInfo->buffer &&
                                         d.buffer.offset == x.pBufferInfo->offset && d.buffer.range == x.pBufferInfo->range)
                                      : (d.image.sampler == x.pImageInfo->sampler &&
                                         d.image.imageView == x.pImageInfo->imageView &&
                                         d.image.imageLayout == x.pImageInfo->imageLayout));
            if (same) {
                continue;
            }
            desc_changed_.push_back(x);
            d.valid = x.descriptorCount == 1; // (an array of views: pushed whole each time)
            d.type = x.descriptorType;
            if (buffer) {
                d.buffer = *x.pBufferInfo;
            } else {
                d.image = *x.pImageInfo;
            }
        }
        if (desc_changed_.empty()) {
            ++stats_.desc_push_skipped;
            return true;
        }
        stats_.desc_pushed += desc_changed_.size();
        f.vkCmdPushDescriptorSetKHR(cur_.cb, bind, p.layout, 0, static_cast<u32>(desc_changed_.size()),
                                    desc_changed_.data());
        return true;
    }
    const VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = cur_.descriptors,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &p.set_layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (f.vkAllocateDescriptorSets(ctx_.Device(), &ai, &set) != VK_SUCCESS) {
        return false;
    }
    for (auto& x : w) {
        x.dstSet = set;
    }
    f.vkUpdateDescriptorSets(ctx_.Device(), static_cast<u32>(w.size()), w.data(), 0, nullptr);
    f.vkCmdBindDescriptorSets(cur_.cb, bind, p.layout, 0, 1, &set, 0, nullptr);
    if (bind == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        desc_layout_ = VK_NULL_HANDLE; // (P4.7: set 0 bound - the descriptors pushed before are no longer there)
    }
    ++stats_.descriptor_sets;
    return true;
}

bool Executor::BindStage(const void* stage_view, bool compute, PushData& push,
                         std::vector<VkWriteDescriptorSet>& writes, std::vector<VkDescriptorBufferInfo>& buffers,
                         std::vector<VkDescriptorImageInfo>& image_infos, std::vector<CachedImage*>& bound,
                         bool& writes_memory, const std::vector<CachedImage*>& targets, const u32* user_data) {
    const auto& s = *static_cast<const GS::StageView*>(stage_view);
    const GS::ResourceBindingPlan& plan = s.plan;
    const GS::ShaderReflection& refl = s.module->result.reflection;
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    const Cp::RegisterFile& regs = *regs_;
    buf_adjust_.assign(plan.buffers.size(), 0);
    // buffers
    for (size_t i = 0; i < plan.buffers.size(); ++i) {
        const auto& b = plan.buffers[i];
        const u32 index = s.start.buffer + static_cast<u32>(i);
        VkDescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        switch (b.kind) {
        case GS::BufferKind::Guest: {
            if (b.status != GS::ResourceStatus::Ok || b.size == 0 || b.address == 0) {
                break; // a null descriptor: robustness2 reads zeros (the bridge binds null for base 0 / size 0)
            }
            VkBuffer vb = VK_NULL_HANDLE;
            u64 voff = 0, vavail = 0;
            if (!GuestRange(b.address, b.size, b.written, align, vb, voff, vavail)) {
                Skipped(Skip::Buffer, fmt::format("buffer {} at {:#x} ({} bytes)", b.binding, b.address, b.size));
                return false;
            }
            const u64 aligned = voff & ~(align - 1);
            const u64 adjust = voff - aligned;
            if (index < push.buf_offsets.size()) {
                push.buf_offsets[index] = static_cast<u8>(adjust);
            }
            buf_adjust_[i] = static_cast<u32>(adjust);
            info = {vb, aligned, std::min<u64>(b.size + adjust, vavail + adjust)};
            if (b.written) {
                writes_memory = true;
                images_.InvalidateFromBufferWrite(b.address, b.size);
            }
            break;
        }
        case GS::BufferKind::Flatbuf: {
            const u64 bytes = plan.flat.size() * 4;
            StreamRing::Slice sl{};
            if (bytes == 0 || ring_.Allocate(bytes, align, cur_.value, sl) != StreamRing::Status::Ok) {
                Skipped(Skip::Ring, "flattened user data");
                return false;
            }
            std::memcpy(sl.data, plan.flat.data(), bytes);
            if (s.native_shared && user_data != nullptr) {
                // P5.3: the registers part of the flattened user data is this draw's (the snapshot pointers differ
                // from those of the draw that made the shared preparation; everything past them is the same)
                std::memcpy(sl.data, user_data, std::min<u64>(bytes, 16 * 4));
            }
            info = {sl.buffer, sl.offset, bytes};
            break;
        }
        case GS::BufferKind::ClipPlanes: {
            if ((regs.Get(kPaClClipCntl) & 0x3F) == 0) {
                break; // compiled without planes: never read
            }
            StreamRing::Slice sl{};
            if (ring_.Allocate(24 * 4, align, cur_.value, sl) != StreamRing::Status::Ok) {
                Skipped(Skip::Ring, "clip planes");
                return false;
            }
            for (u32 k = 0; k < 24; ++k) {
                const u32 v = regs.Get(kClipUserData + k);
                std::memcpy(sl.data + k * 4, &v, 4);
            }
            info = {sl.buffer, sl.offset, 24 * 4};
            break;
        }
        case GS::BufferKind::GdsBuffer:
            info = {gds_.buffer, 0, gds_.size};
            writes_memory |= b.written;
            break;
        case GS::BufferKind::SharedMemory: {
            // the translator's emulated LDS: each workgroup indexes its own part (the bridge: SharedMemSize() *
            // NumWorkgroups(), zeroed before the dispatch)
            const u64 per_group = std::max<u64>(refl.shared_memory_size, lds_group_bytes_);
            const u64 bytes = (std::max<u64>(per_group * (compute ? lds_groups_ : 1), 16) + 255) & ~255ull;
            const auto& f = Fn();
            if (lds_.size < bytes) {
                if (lds_.buffer != VK_NULL_HANDLE) {
                    lds_retired_.push_back({cur_.value, lds_});
                    lds_ = {};
                }
                u64 grown = 64ull << 20;
                while (grown < bytes) {
                    grown *= 2;
                }
                std::string err;
                if (!CreateBuffer(ctx_, grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  false, lds_, err)) {
                    Skipped(Skip::Ring, fmt::format("emulated LDS buffer of {} bytes: {}", bytes, err));
                    return false;
                }
            }
            // earlier users of the buffer -> the zero fill -> this dispatch
            EndRendering();
            VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
            const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                       .pMemoryBarriers = &mb};
            f.vkCmdPipelineBarrier2(cur_.cb, &dep);
            f.vkCmdFillBuffer(cur_.cb, lds_.buffer, 0, bytes, 0);
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            f.vkCmdPipelineBarrier2(cur_.cb, &dep);
            info = {lds_.buffer, 0, bytes};
            writes_memory = true;
            ++stats_.lds_dispatches;
            stats_.lds_max_bytes = std::max(stats_.lds_max_bytes, bytes);
            if (lds_logged_++ < 8) {
                SSPC_LOG(GPU, Info,
                         "native backend: emulated LDS: {} bytes ({} per workgroup x {} workgroups; translator {} bytes)",
                         bytes, per_group, compute ? lds_groups_ : 1, refl.shared_memory_size);
            }
            break;
        }
        default:
            Skipped(Skip::SpecialBuffer, "BDA page table / fault buffer (shader DMA)");
            return false;
        }
        buffers.push_back(info);
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = b.binding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = nullptr});
    }
    // images
    const VkPipelineStageFlags2 shader_stage = compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : kGraphicsShaders;
    images_.ApplyPendingInvalidations(); // before the memo's dirty checks below
    for (const auto& im : plan.images) {
        const u32 count = std::max<u32>(im.count, 1);
        const size_t first = image_infos.size();
        bool null_image = im.status != GS::ResourceStatus::Ok;
        CachedImage* img = nullptr;
        // the memo: one view (a dynamic mip index binds every level: not memoized), the same T# and binding flags
        const bool memo_able = !null_image && count == 1 && !im.fmask;
        const u32 flags = (im.depth ? 1u : 0u) | (im.array ? 2u : 0u) | (im.storage ? 4u : 0u) | (im.view_type << 3);
        u64 memo_key = 0;
        TexMemo* memo = nullptr;
        if (memo_able) {
            memo_key = flags;
            for (const u32 w : im.desc.raw) {
                memo_key = (memo_key ^ w) * 0x100000001B3ull;
            }
            auto& slot = tex_slot_[GS::LookupMemoSlot(memo_key)];
            if (slot.second != nullptr && slot.first == memo_key) {
                memo = slot.second;
                if (GS::LookupMemoVerify()) {
                    const auto it = tex_memo_.find(memo_key);
                    ++GS::LookupMemoCounts().verified;
                    GS::LookupMemoCounts().mismatches += it == tex_memo_.end() || &it->second != memo ? 1 : 0;
                }
            } else {
                memo = &tex_memo_[memo_key];
                if (GS::LookupMemoOn()) {
                    slot = {memo_key, memo};
                }
            }
            CachedImage* m = memo->img;
            if (m != nullptr && memo->epoch == images_.Epoch() && memo->raw == im.desc.raw && memo->flags == flags &&
                ((m->gpu_written && !m->memory_newer) || (m->uploaded && !m->dirty && !m->gpu_written))) {
                ++stats_.tex_memo_hits;
                images_.Touch(*m, cur_.value); // what Find records
                if (im.storage) {
                    m->gpu_written = true;
                }
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, memo->view, VK_IMAGE_LAYOUT_GENERAL};
                const bool feedback = std::ranges::find(targets, m) != targets.end();
                ii.imageLayout = (im.storage || feedback) ? VK_IMAGE_LAYOUT_GENERAL
                                 : m->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                image_infos.push_back(ii);
                info_images_.push_back(m);
                bound.push_back(m);
                if (im.storage) {
                    writes_memory = true;
                }
                writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstBinding = im.binding,
                                  .descriptorCount = 1,
                                  .descriptorType = im.storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                               : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                  .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
                continue;
            }
            ++stats_.tex_memo_misses;
        }
        if (!null_image) {
            const ImageLayout layout = LayoutFromTSharp(im.desc, im.depth);
            std::string err;
            const u64 ti0 = GS::FineTicks();
            img = images_.Find(layout, im.storage ? ImageCache::Use::Storage : ImageCache::Use::Texture, cur_.cb,
                               cur_.value, err);
            stats_.t_image += GS::FineTicks() - ti0;
            if (img == nullptr) {
                Skipped(Skip::Image, err);
                return false;
            }
        }
        for (u32 j = 0; j < count; ++j) {
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            if (img != nullptr) {
                ImageViewDesc vd = TSharpView(im.desc, im.depth, im.array, im.storage);
                if (!vd.supported) {
                    Skipped(Skip::Image, std::string(vd.reason));
                    return false;
                }
                if (count > 1) {
                    vd.base_level = static_cast<u16>(vd.base_level + j);
                    vd.levels = 1;
                }
                ii.imageView = images_.View(*img, vd, im.storage);
                if (memo != nullptr && ii.imageView != VK_NULL_HANDLE) {
                    *memo = {im.desc.raw, flags, images_.Epoch(), img, ii.imageView};
                }
                const bool feedback = std::ranges::find(targets, img) != targets.end();
                ii.imageLayout = (im.storage || feedback) ? VK_IMAGE_LAYOUT_GENERAL
                                 : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            image_infos.push_back(ii);
            info_images_.push_back(img);
        }
        if (img != nullptr) {
            bound.push_back(img);
            if (im.storage) {
                writes_memory = true;
            }
        }
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = im.binding,
                          .descriptorCount = count,
                          .descriptorType = im.storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                       : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)}); // fixed up below
        (void)null_image;
        (void)shader_stage;
    }
    // samplers
    const u64 border_base = u64{regs.Get(kTaBcBase)} << 8;
    for (size_t i = 0; i < plan.samplers.size(); ++i) {
        const auto& sm = plan.samplers[i];
        const bool depth = i < refl.samplers.size() && refl.samplers[i].is_depth;
        GS::SamplerDesc desc = sm.desc;
        if (sm.status != GS::ResourceStatus::Ok) {
            desc = {};
        }
        VkSampler vs = images_.Sampler(desc, depth, border_base);
        if (vs == VK_NULL_HANDLE) {
            vs = images_.Sampler(GS::SamplerDesc{}, depth, 0); // the bridge's fallback: the default S#
        }
        const size_t first = image_infos.size();
        image_infos.push_back({vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
        info_images_.push_back(nullptr);
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = sm.binding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                          .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
    }
    return true;
}

void Executor::SetDynamicState(const Cp::RegisterFile& r, const GS::GraphicsPipelineState& st, bool indexed) {
    const auto& f = Fn();
    const VkCommandBuffer cb = cur_.cb;
    const u32 clip = r.Get(kPaClClipCntl);
    const u32 vte = r.Get(kPaClVteCntl);
    // scissor: screen ∩ window ∩ generic (+ window offset)
    const u32 woff = r.Get(kWindowOffset);
    const bool enable_offset = (r.Get(kWindowScissorTl) >> 31) == 0;
    const s32 ox = enable_offset ? Lo16(woff) : 0, oy = enable_offset ? Hi16(woff) : 0;
    const u32 wtl = r.Get(kWindowScissorTl), wbr = r.Get(kWindowScissorBr);
    const u32 gtl = r.Get(kGenericScissorTl), gbr = r.Get(kGenericScissorBr);
    const u32 stl = r.Get(kScreenScissorTl), sbr = r.Get(kScreenScissorBr);
    s32 tlx = std::max({Lo16(stl), static_cast<s32>(static_cast<s16>((wtl & 0x7FFF) + ox)),
                        static_cast<s32>(static_cast<s16>((gtl & 0x7FFF) + ox))});
    s32 tly = std::max({Hi16(stl), static_cast<s32>(static_cast<s16>(((wtl >> 16) & 0x7FFF) + oy)),
                        static_cast<s32>(static_cast<s16>(((gtl >> 16) & 0x7FFF) + oy))});
    s32 brx = std::min({Lo16(sbr), static_cast<s32>(static_cast<s16>((wbr & 0x7FFF) + ox)),
                        static_cast<s32>(static_cast<s16>((gbr & 0x7FFF) + ox))});
    s32 bry = std::min({Hi16(sbr), static_cast<s32>(static_cast<s16>(((wbr >> 16) & 0x7FFF) + oy)),
                        static_cast<s32>(static_cast<s16>(((gbr >> 16) & 0x7FFF) + oy))});
    const bool vport_scissor = (r.Get(kPaScModeCntl0) >> 1) & 1;
    std::array<VkViewport, 16> vps{};
    std::array<VkRect2D, 16> scs{};
    u32 n = 0;
    for (u32 i = 0; i < 16; ++i) {
        const u32 base = kViewport0 + i * 6;
        const float xscale_raw = F(r, base + 0);
        if (xscale_raw == 0.0f) {
            continue;
        }
        const float zscale = (vte >> 4) & 1 ? F(r, base + 4) : 1.0f;
        const float zoffset = (vte >> 5) & 1 ? F(r, base + 5) : 0.0f;
        VkViewport v{};
        if (((clip >> 19) & 1) == 0) { // [-W, W]
            v.minDepth = zoffset - zscale;
            v.maxDepth = zoffset + zscale;
        } else {
            v.minDepth = zoffset;
            v.maxDepth = zoffset + zscale;
        }
        v.minDepth = std::clamp(v.minDepth, 0.0f, 1.0f);
        v.maxDepth = std::clamp(v.maxDepth, 0.0f, 1.0f);
        if ((clip >> 16) & 1) { // clipping disabled: the shader maps screen space into a 16K window
            v.x = 0.0f;
            v.y = 0.0f;
            v.width = static_cast<float>(std::min<u32>(ctx_.Caps().max_viewport_width, 16384));
            v.height = static_cast<float>(std::min<u32>(ctx_.Caps().max_viewport_height, 16384));
        } else {
            const float xscale = (vte & 1) ? xscale_raw : 1.0f;
            const float xoffset = (vte >> 1) & 1 ? F(r, base + 1) : 0.0f;
            const float yscale = (vte >> 2) & 1 ? F(r, base + 2) : 1.0f;
            const float yoffset = (vte >> 3) & 1 ? F(r, base + 3) : 0.0f;
            v.x = xoffset - xscale;
            v.y = yoffset - yscale;
            v.width = xscale * 2.0f;
            v.height = yscale * 2.0f;
        }
        s32 ax = tlx, ay = tly, bx = brx, by = bry;
        if (vport_scissor) {
            const u32 vtl = r.Get(kViewportScissor0 + i * 2), vbr = r.Get(kViewportScissor0 + i * 2 + 1);
            ax = std::max(ax, static_cast<s32>(vtl & 0x7FFF));
            ay = std::max(ay, static_cast<s32>((vtl >> 16) & 0x7FFF));
            bx = std::min(std::clamp(bx, 0, 16384), static_cast<s32>(vbr & 0x7FFF));
            by = std::min(std::clamp(by, 0, 16384), static_cast<s32>((vbr >> 16) & 0x7FFF));
        }
        ax = std::max(ax, 0);
        ay = std::max(ay, 0);
        vps[n] = v;
        scs[n] = {{ax, ay}, {static_cast<u32>(std::max(bx - ax, 0)), static_cast<u32>(std::max(by - ay, 0))}};
        ++n;
    }
    if (n == 0) {
        vps[0] = {-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        scs[0] = {{0, 0}, {1, 1}};
        n = 1;
    }
    DynShadow& sh = dyn_;
    const bool all = !sh.valid;
    sh.valid = true;
    const auto changed = [&](bool differs) {
        if (all || differs) {
            ++dyn_set_;
            return true;
        }
        ++dyn_skipped_;
        return false;
    };
    if (changed(n != sh.vp_count || std::memcmp(vps.data(), sh.vps.data(), n * sizeof(VkViewport)) != 0 ||
                std::memcmp(scs.data(), sh.scs.data(), n * sizeof(VkRect2D)) != 0)) {
        f.vkCmdSetViewportWithCount(cb, n, vps.data());
        f.vkCmdSetScissorWithCount(cb, n, scs.data());
        sh.vp_count = n;
        std::memcpy(sh.vps.data(), vps.data(), n * sizeof(VkViewport));
        std::memcpy(sh.scs.data(), scs.data(), n * sizeof(VkRect2D));
    }

    // depth / stencil
    const u32 dc = r.Get(kDbDepthControl);
    const u32 rc = r.Get(kDbRenderControl);
    const bool depth_valid = (r.Get(kDbZInfo) & 3) != 0, stencil_valid = (r.Get(kDbStencilInfo) & 1) != 0;
    const VkBool32 depth_test = ((dc >> 1) & 1) && depth_valid;
    const VkBool32 depth_write = depth_test && ((dc >> 2) & 1) && (rc & 1) == 0;
    const VkCompareOp depth_op = CompareOp((dc >> 4) & 7).value;
    if (changed(depth_test != sh.depth_test)) {
        f.vkCmdSetDepthTestEnable(cb, depth_test);
        sh.depth_test = depth_test;
    }
    if (changed(depth_write != sh.depth_write)) {
        f.vkCmdSetDepthWriteEnable(cb, depth_write);
        sh.depth_write = depth_write;
    }
    if (changed(depth_op != sh.depth_op)) {
        f.vkCmdSetDepthCompareOp(cb, depth_op);
        sh.depth_op = depth_op;
    }
    const VkBool32 bounds = (dc >> 3) & 1;
    if (changed(bounds != sh.bounds_test)) {
        f.vkCmdSetDepthBoundsTestEnable(cb, bounds);
        sh.bounds_test = bounds;
    }
    const float bmin = std::clamp(F(r, kDbDepthBoundsMin), 0.0f, 1.0f), bmax = std::clamp(F(r, kDbDepthBoundsMax), 0.0f, 1.0f);
    if (changed(bmin != sh.bounds[0] || bmax != sh.bounds[1])) {
        f.vkCmdSetDepthBounds(cb, bmin, bmax);
        sh.bounds[0] = bmin;
        sh.bounds[1] = bmax;
    }
    const u32 pc = r.Get(kPaSuScModeCntl);
    const VkBool32 bias = ((pc >> 11) & 7) != 0;
    if (changed(bias != sh.bias_enable)) {
        f.vkCmdSetDepthBiasEnable(cb, bias);
        sh.bias_enable = bias;
    }
    if (bias) {
        const bool front = (pc >> 11) & 1;
        const float offset = F(r, kPolyOffsetClamp + (front ? 2 : 4));
        const float scale = F(r, kPolyOffsetClamp + (front ? 1 : 3));
        const float b3[3] = {offset, F(r, kPolyOffsetClamp), scale / 16.0f};
        if (changed(!sh.bias_set || std::memcmp(b3, sh.bias, sizeof(b3)) != 0)) {
            f.vkCmdSetDepthBias(cb, b3[0], b3[1], b3[2]);
            std::memcpy(sh.bias, b3, sizeof(b3));
            sh.bias_set = true;
        }
    }
    const VkBool32 stencil = (dc & 1) && stencil_valid;
    if (changed(stencil != sh.stencil_test)) {
        f.vkCmdSetStencilTestEnable(cb, stencil);
        sh.stencil_test = stencil;
    }
    // the stencil faces as this draw wants them, set where they differ
    DynShadow::Face want_front{}, want_back{};
    if (stencil) {
        const u32 sc = r.Get(kDbStencilControl);
        const bool backface = (dc >> 7) & 1;
        const auto ops = [&](u32 shift) {
            return std::array<VkStencilOp, 3>{StencilOp((sc >> shift) & 0xF).value, StencilOp((sc >> (shift + 4)) & 0xF).value,
                                              StencilOp((sc >> (shift + 8)) & 0xF).value};
        };
        const auto fo = ops(0);
        const auto bo = backface ? ops(12) : fo;
        const VkCompareOp ffunc = CompareOp((dc >> 8) & 7).value;
        const VkCompareOp bfunc = backface ? CompareOp((dc >> 20) & 7).value : ffunc;
        want_front = {fo[0], fo[1], fo[2], ffunc};
        want_back = {bo[0], bo[1], bo[2], bfunc};
        const u32 front_ref = r.Get(kDbStencilRefMask), back_ref = backface ? r.Get(kDbStencilRefMaskBf) : front_ref;
        const auto uses_op_val = [&](u32 shift) {
            for (u32 k = 0; k < 3; ++k) {
                if (((sc >> (shift + 4 * k)) & 0xF) == 4) { // ReplaceOp: the op value
                    return true;
                }
            }
            return false;
        };
        const bool fop = uses_op_val(0), bop = backface ? uses_op_val(12) : fop;
        const bool clear = (rc >> 1) & 1;
        want_front.reference = fop ? (front_ref >> 24) : (front_ref & 0xFF);
        want_back.reference = bop ? (back_ref >> 24) : (back_ref & 0xFF);
        want_front.write_mask = clear ? 0 : (front_ref >> 16) & 0xFF;
        want_back.write_mask = clear ? 0 : (back_ref >> 16) & 0xFF;
        want_front.compare_mask = (front_ref >> 8) & 0xFF;
        want_back.compare_mask = (back_ref >> 8) & 0xFF;
    } else {
        want_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0, 0};
        want_back = want_front;
    }
    const auto set_face = [&](VkStencilFaceFlags face, const DynShadow::Face& w, DynShadow::Face& have) {
        if (changed(w.fail != have.fail || w.pass != have.pass || w.depth_fail != have.depth_fail ||
                    w.compare != have.compare)) {
            f.vkCmdSetStencilOp(cb, face, w.fail, w.pass, w.depth_fail, w.compare);
        }
        if (changed(w.reference != have.reference)) {
            f.vkCmdSetStencilReference(cb, face, w.reference);
        }
        if (changed(w.write_mask != have.write_mask)) {
            f.vkCmdSetStencilWriteMask(cb, face, w.write_mask);
        }
        if (changed(w.compare_mask != have.compare_mask)) {
            f.vkCmdSetStencilCompareMask(cb, face, w.compare_mask);
        }
        have = w;
    };
    set_face(VK_STENCIL_FACE_FRONT_BIT, want_front, sh.front);
    set_face(VK_STENCIL_FACE_BACK_BIT, want_back, sh.back);
    // primitive state
    const bool rect_quad = st.prim_type == 17 || st.prim_type == 19;
    const auto topo = PrimitiveType(st.prim_type).value;
    const bool list = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                      topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                      topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
                      topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    const bool restart = (r.Get(kVgtMultiPrimIbResetEn) & 1) && indexed &&
                         (ctx_.Caps().list_restart || !list) && (ctx_.Caps().patch_list_restart || !rect_quad);
    if (changed(static_cast<VkBool32>(restart) != sh.restart)) {
        f.vkCmdSetPrimitiveRestartEnable(cb, restart);
        sh.restart = restart;
    }
    const VkBool32 discard = (clip >> 22) & 1;
    if (changed(discard != sh.discard)) {
        f.vkCmdSetRasterizerDiscardEnable(cb, discard);
        sh.discard = discard;
    }
    const auto culled = IsPrimitiveCulled(st.prim_type);
    const VkCullModeFlags cull = culled.value ? CullMode(pc & 3).value : VK_CULL_MODE_NONE;
    if (changed(cull != sh.cull)) {
        f.vkCmdSetCullMode(cb, cull);
        sh.cull = cull;
    }
    const VkFrontFace front_face = FrontFace((pc >> 2) & 1).value;
    if (changed(front_face != sh.front_face)) {
        f.vkCmdSetFrontFace(cb, front_face);
        sh.front_face = front_face;
    }
    if (changed(sh.line_width != 1.0f)) {
        f.vkCmdSetLineWidth(cb, 1.0f);
        sh.line_width = 1.0f;
    }
    const float bc[4] = {F(r, kCbBlendRed), F(r, kCbBlendRed + 1), F(r, kCbBlendRed + 2), F(r, kCbBlendRed + 3)};
    if (changed(std::memcmp(bc, sh.blend, sizeof(bc)) != 0)) {
        f.vkCmdSetBlendConstants(cb, bc);
        std::memcpy(sh.blend, bc, sizeof(bc));
    }
    if (ctx_.Caps().color_write_mask_dynamic) {
        const u32 num_color = static_cast<u32>(std::bit_width(st.mrt_mask & 0xFFu));
        if (num_color > 0) {
            std::array<VkColorComponentFlags, 8> masks{};
            for (u32 i = 0; i < num_color; ++i) {
                masks[i] = st.targets[i].write_mask & 0xF;
            }
            if (changed(num_color != sh.color_masks_count || masks != sh.color_masks)) {
                f.vkCmdSetColorWriteMaskEXT(cb, 0, num_color, masks.data());
                sh.color_masks_count = num_color;
                sh.color_masks = masks;
            }
        }
    }
}

bool Executor::DrawFiltered(const Cp::RegisterFile& regs, const Cp::DrawDescriptor& d) {
    const u32 mode = (regs.Get(kCbColorControl) >> 4) & 7;
    return mode == 2 || mode == 5 || d.primitive_type == 0; // EliminateFastClear, FmaskDecompress, None
}

void Executor::Draw(const Cp::DrawDescriptor& d) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_); // (SSPC_GPU_TIMINGS=sample:N: this draw's stage timers)
    // the frontend reads the guest's descriptors now (a write still on the GPU's timeline lands first: before this draw
    // is noted, so that flush completes only the earlier work)
    PreparedCommand pc;
    if (!DrawFiltered(*regs_, d)) {
        const u64 tf0 = GS::FineTicks();
        pc.prep = &frontend_.PrepareDraw(d, *regs_, reader_);
        pc.stage = &frontend_.Stage(GS::HwStage::Vs);
        pc.ps = &frontend_.Stage(GS::HwStage::Ps);
        pc.state = pc.prep->ready ? frontend_.Pipelines().Graphics(pc.prep->pipeline) : nullptr;
        stats_.t_front += GS::FineTicks() - tf0;
    }
    RecordDraw(d, pc, t0);
}

void Executor::DrawPrepared(const Cp::DrawDescriptor& d, const PreparedCommand& pc) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_);
    RecordDraw(d, pc, t0);
}

void Executor::RecordDraw(const Cp::DrawDescriptor& d, const PreparedCommand& pc, u64 t0) {
    ++stats_.draws;
    ++trace_.ordinal;
    const auto& f = Fn();
    const Cp::RegisterFile& regs = *regs_;
    const u32 mode = (regs.Get(kCbColorControl) >> 4) & 7;
    const bool filtered = DrawFiltered(regs, d);
    const GS::DrawPreparation* prepared = filtered ? nullptr : pc.prep;
    if (!Begin()) {
        return;
    }
    Note(d.queue, d.seq);
    ++commands_;
    write_run_ = false;
    if (filtered) {
        Skipped(Skip::FilteredMode, fmt::format("CB_COLOR_CONTROL mode {}, primitive {}", mode, d.primitive_type));
        return;
    }
    if (prepared == nullptr || pc.stage == nullptr || pc.ps == nullptr) {
        // (not filtered at the draw's packet but here: the registers differ - never with the P4.10 replica)
        ++stats_.prepared_mismatches;
        Skipped(Skip::NotReady, fmt::format("draw at {:#x}: no preparation", d.packet_addr));
        return;
    }
    const GS::DrawPreparation& prep = *prepared;
    if (!prep.ready) {
        Skipped(prep.unsupported ? Skip::Unsupported : Skip::NotReady,
                fmt::format("draw at {:#x}: VS {}, PS {}", d.packet_addr, GS::StageStatusName(pc.stage->status),
                            GS::StageStatusName(pc.ps->status)));
        return;
    }
    u64 ts = GS::FineTicks();
    const auto stage = [&](u64& acc) {
        const u64 now = GS::FineTicks();
        acc += now - ts;
        ts = now;
    };
    const GS::GraphicsPipelineState* st = pc.state;
    const GS::StageView& vs = *pc.stage;
    const GS::StageView& ps = *pc.ps;
    const bool has_ps = st != nullptr && st->ps_module != 0 && ps.module != nullptr;
    std::string err;
    const PipelineObjects* pipe = st == nullptr ? nullptr
                                                : pipelines_.Graphics(prep.pipeline, *st, vs.module, vs.plan,
                                                                      has_ps ? ps.module : nullptr,
                                                                      has_ps ? &ps.plan : nullptr, err);
    if (pipe == nullptr) {
        Skipped(Skip::Pipeline, err.empty() ? "no pipeline state" : err);
        return;
    }
    stage(stats_.t_pipe);
    if (markers_) {
        // a new label at a new pipeline or a change of the target registers (before this draw's uploads and barriers:
        // they count to it)
        const u64 tv = ((u64{regs.Version(Cp::Group::ColorFormat)} * 31 + regs.Version(Cp::Group::ColorTarget)) * 31 +
                        regs.Version(Cp::Group::DepthFormat)) * 31 + regs.Version(Cp::Group::DepthTarget);
        const u64 lkey = ((u64{prep.pipeline} & 0x3FFFFFFF) << 32) | (tv & 0xFFFFFFFF);
        if (LabelChanges(lkey)) {
            Label(lkey, fmt::format("D p{} vs{:x} ps{:x} @{:x}", prep.pipeline, vs.address, has_ps ? ps.address : 0,
                                    d.packet_addr));
        }
    }
    if (has_ps) { // SSPC_DIAG_DUMP_SHADER: a pixel shader too (P4.5)
        DiagDumpShader(ps.address, ps.program != nullptr ? &ps.program->code : nullptr, ps.module->result.spirv,
                       ps.runtime, prep.pipeline);
    }
    DiagDumpShader(vs.address, vs.program != nullptr ? &vs.program->code : nullptr, vs.module->result.spirv, vs.runtime,
                   prep.pipeline);
    {
        // SSPC_DIAG_SKIP_PIPELINES=<id>,<id>...: those pipelines' draws not executed (which draws make what is seen; P4.5)
        static const std::vector<u64> skip = [] {
            std::vector<u64> v;
            char s[256] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_SKIP_PIPELINES", s, sizeof(s)) > 0) {
                for (char* p = s; *p != 0;) {
                    v.push_back(std::strtoull(p, &p, 10));
                    while (*p == ',' || *p == ' ') {
                        ++p;
                    }
                }
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_SKIP_PIPELINES - {} pipelines' draws skipped",
                         v.size());
            }
            return v;
        }();
        if (!skip.empty() && std::ranges::find(skip, u64{prep.pipeline}) != skip.end()) {
            Skipped(Skip::FilteredMode, fmt::format("SSPC_DIAG_SKIP_PIPELINES: pipeline {}", prep.pipeline));
            return;
        }
    }

    // targets
    std::vector<CachedImage*>& targets = scratch_.targets;
    targets.clear();
    RenderKey key;
    std::array<CachedImage*, 8> color_img{};
    const u32 target_mask = regs.Get(kCbTargetMask);
    std::array<bool, 8> color_clear{};
    CachedImage* depth_img = nullptr;
    const u32 dc = regs.Get(kDbDepthControl);
    bool depth_clear = false;
    // the last draw's targets when nothing they are built from changed (e1: 9.9 ms a frame, ~1.8 us a draw, rebuilding
    // the same targets draw after draw)
    TargetInputs tin{};
    tin.versions = {regs.Version(Cp::Group::ColorFormat), regs.Version(Cp::Group::ColorTarget),
                    regs.Version(Cp::Group::DepthFormat), regs.Version(Cp::Group::DepthTarget)};
    tin.mode = mode;
    tin.target_mask = target_mask;
    tin.depth_control = dc & 7; // stencil, depth test, depth write (the depth view)
    tin.mrt_mask = st->mrt_mask;
    tin.cb_extent = d.cb_extent;
    tin.db_extent = d.db_extent;
    images_.ApplyPendingInvalidations(); // a target whose memory was written since is found (uploaded) again
    tin.epoch = images_.Epoch();
    if (target_cache_.valid && target_cache_.in == tin &&
        std::ranges::none_of(target_cache_.targets, [](const CachedImage* t) { return t->memory_newer; })) {
        ++stats_.target_cache_hits;
        targets = target_cache_.targets;
        key = target_cache_.key;
        color_img = target_cache_.color_img;
        depth_img = target_cache_.depth_img;
        for (CachedImage* img : targets) {
            images_.Touch(*img, cur_.value); // what Find records for a target
        }
        if (!pending_meta_clear_.empty()) {
            // a pending clear is only looked at here: it is taken when the render pass that clears begins (a draw
            // that returns before that leaves it for the next one; P4.5, the review's LOW)
            const auto pending = [&](u64 meta) {
                const auto it = meta != 0 ? pending_meta_clear_.find(meta) : pending_meta_clear_.end();
                return it != pending_meta_clear_.end() && it->second;
            };
            for (u32 i = 0; i < 8; ++i) {
                color_clear[i] = color_img[i] != nullptr && pending(target_cache_.cmask[i]);
            }
            depth_clear = depth_img != nullptr && pending(target_cache_.htile);
        }
    } else {
    ++stats_.target_cache_misses;
    target_cache_.valid = false;
    target_cache_.cmask.fill(0);
    target_cache_.htile = 0;
    key.width = ctx_.Caps().max_framebuffer_width;
    key.height = ctx_.Caps().max_framebuffer_height;
    key.layers = 0xFFFF;
    const u32 num_color = static_cast<u32>(std::bit_width(st->mrt_mask & 0xFFu));
    for (u32 i = 0; i < num_color; ++i) {
        const u32 base = Cp::Reg::CbColor0Base + i * Cp::Reg::CbColorStride;
        const u32 info = regs.Get(base + 4);
        if (mode == 0 || ((st->mrt_mask >> i) & 1) == 0 || ((info >> 2) & 0x1F) == 0 || regs.Get(base) == 0 ||
            ((target_mask >> (i * 4)) & 0xF) == 0) {
            continue;
        }
        const ImageLayout layout = LayoutFromColorTarget(regs, i, d.cb_extent[i]);
        CachedImage* img = images_.Find(layout, ImageCache::Use::ColorTarget, cur_.cb, cur_.value, err);
        if (img == nullptr) {
            Skipped(Skip::Target, err);
            return;
        }
        const ImageViewDesc vd = ColorTargetView(regs, i);
        key.color[i] = images_.View(*img, vd, false);
        if (key.color[i] == VK_NULL_HANDLE) {
            Skipped(Skip::Target, "colour target view");
            return;
        }
        key.width = std::min(key.width, std::max(img->layout.size.width, 1u));
        key.height = std::min(key.height, std::max(img->layout.size.height, 1u));
        key.layers = std::min<u32>(key.layers, std::max<u32>(vd.layers, 1));
        color_img[i] = img;
        targets.push_back(img);
        const u64 cmask = u64{regs.Get(base + 7)} << 8;
        target_cache_.cmask[i] = cmask;
        if (cmask != 0) {
            if (auto it = pending_meta_clear_.find(cmask); it != pending_meta_clear_.end() && it->second) {
                color_clear[i] = true; // taken when the clearing render pass begins
            }
        }
    }
    key.color_count = num_color;
    const bool depth_valid = (regs.Get(kDbZInfo) & 3) != 0, stencil_valid = (regs.Get(kDbStencilInfo) & 1) != 0;
    if ((((dc >> 1) & 1) && depth_valid) || ((dc & 1) && stencil_valid)) {
        const ImageLayout layout = LayoutFromDepthTarget(regs, d.db_extent);
        depth_img = images_.Find(layout, ImageCache::Use::DepthTarget, cur_.cb, cur_.value, err);
        if (depth_img == nullptr) {
            Skipped(Skip::Target, err);
            return;
        }
        key.depth = images_.View(*depth_img, DepthTargetView(regs), false);
        key.has_depth = depth_valid;
        key.has_stencil = stencil_valid && depth_img->layout.props.has_stencil;
        key.width = std::min(key.width, std::max(depth_img->layout.size.width, 1u));
        key.height = std::min(key.height, std::max(depth_img->layout.size.height, 1u));
        targets.push_back(depth_img);
        const u64 htile = u64{regs.Get(kDbHtileBase)} << 8;
        target_cache_.htile = htile;
        if (htile != 0) {
            if (auto it = pending_meta_clear_.find(htile); it != pending_meta_clear_.end() && it->second) {
                depth_clear = true; // taken when the clearing render pass begins
            }
        }
    }
    if (key.layers == 0xFFFF) {
        key.layers = 1;
    }
    if (targets.empty()) {
        key.width = std::max<u32>(1, std::min<u32>(key.width, 16384));
        key.height = std::max<u32>(1, std::min<u32>(key.height, 16384));
    }
    target_cache_.valid = true;
    target_cache_.in = tin;
    target_cache_.in.epoch = images_.Epoch(); // a target just created bumped it: the next draw's inputs carry the new one
    target_cache_.targets = targets;
    target_cache_.key = key;
    target_cache_.color_img = color_img;
    target_cache_.depth_img = depth_img;
    }
    stage(stats_.t_targets);
    Inspector::Class insp_cls{};
    if (insp_catalog_ != nullptr) {
        Inspector::DrawShape sh;
        for (const CachedImage* c : color_img) {
            sh.colors += c != nullptr ? 1 : 0;
        }
        sh.depth = depth_img != nullptr;
        sh.width = key.width;
        sh.height = key.height;
        sh.primitive = d.primitive_type;
        sh.count = d.index_count;
        sh.instances = std::max<u32>(d.instance_count, 1);
        sh.indexed = d.indexed;
        sh.indirect = d.indirect;
        insp_cls = InspClassifyDraw(vs, ps, has_ps, sh);
        InspMark(static_cast<size_t>(insp_cls.category)); // (its uploads, barriers and render pass changes count to it)
    }
    if (histo_.active) {
        // SSPC_DIAG_TARGET_WATCH=<hex address>: every draw rendering into that target (any MRT slot) with its export
        // state (P4.5: a channel of a target lost)
        static const u64 watch_rt = [] {
            char v[32] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_TARGET_WATCH", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16)
                                                                                       : 0ull;
        }();
        for (u32 i = 0; i < 8 && watch_rt != 0; ++i) {
            if (color_img[i] != nullptr && color_img[i]->layout.guest_address == watch_rt) {
                const auto& t = st->targets[i];
                histo_.detail.push_back(fmt::format(
                    "    target watch: draw {} pipeline {} vs {:#x} ps {:#x} slot {} img{} fmt {} | CB_COLOR_INFO {:#x} "
                    "export {} write mask {:#x} blend {:#x} cb_shader_mask {:#x} mrt_mask {:#x} view fmt {}",
                    stats_.draws, prep.pipeline, vs.address, has_ps ? ps.address : 0, i, color_img[i]->id,
                    color_img[i]->layout.pixel_format, t.info, t.export_format, t.write_mask, t.blend,
                    st->cb_shader_mask, st->mrt_mask, static_cast<int>(ColorTargetView(regs, i).format)));
            }
        }
    }
    // resources
    PushData push{};
    const u32 vte = regs.Get(kPaClVteCntl);
    push.xscale = (vte & 1) ? F(regs, kViewport0) : 1.0f;
    push.xoffset = (vte >> 1) & 1 ? F(regs, kViewport0 + 1) : 0.0f;
    push.yscale = (vte >> 2) & 1 ? F(regs, kViewport0 + 2) : 1.0f;
    push.yoffset = (vte >> 3) & 1 ? F(regs, kViewport0 + 3) : 0.0f;
    // the command's descriptor lists: reused (their capacity kept) - per-command vectors were 14% of the GPU thread's
    // samples in the heap (f1)
    std::vector<VkWriteDescriptorSet>& writes = scratch_.writes;
    std::vector<VkDescriptorBufferInfo>& buf_infos = scratch_.buf_infos;
    std::vector<VkDescriptorImageInfo>& img_infos = scratch_.img_infos;
    std::vector<CachedImage*>& bound = scratch_.bound;
    writes.clear();
    buf_infos.clear();
    img_infos.clear();
    bound.clear();
    bool writes_memory = false;
    info_images_.clear();
    const u64 tb0 = GS::FineTicks();
    if (has_ps && !BindStage(&ps, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets,
                             d.user_data[static_cast<size_t>(Cp::Stage::Ps)].data())) {
        return;
    }
    if (!BindStage(&vs, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets,
                   d.user_data[static_cast<size_t>(Cp::Stage::Vs)].data())) {
        return;
    }
    stats_.t_bind += GS::FineTicks() - tb0;
    if (!clock_labels_.empty()) {
        NoteClockLabelReads(&vs.plan, vs.address);
        if (has_ps) {
            NoteClockLabelReads(&ps.plan, ps.address);
        }
    }
    NoteOrderRange(&vs.plan, vs.address, d.queue, false);
    if (has_ps) {
        NoteOrderRange(&ps.plan, ps.address, d.queue, false);
    }
    ts = GS::FineTicks();
    // vertex inputs
    std::vector<VkVertexInputBindingDescription2EXT>& vbind = scratch_.vbind;
    std::vector<VkVertexInputAttributeDescription2EXT>& vattr = scratch_.vattr;
    std::vector<VkBuffer>& vbufs = scratch_.vbufs;
    std::vector<VkDeviceSize>& voffs = scratch_.voffs;
    vbind.clear();
    vattr.clear();
    vbufs.clear();
    voffs.clear();
    for (const auto& v : vs.plan.vertices) {
        const u32 slot = static_cast<u32>(vattr.size());
        // the format the fetch shader loads with: a typed load's (MTBUF) own format overrides the V#'s, as the
        // translated shader declares the input with it (the bridge's VertexAttribute::GetSharp; VUID-...-Input-08734)
        // and remapped as the bridge's AmdGpu::Buffer::GetNumberFmt() (RemapNumberFormat: SnormNz/Sscaled -> Sint,
        // Uscaled -> Uint, ...; the shader converts those), the number class the shader's input was declared with
        const u32 dfmt = v.fetch.data_format != 0 ? v.fetch.data_format : static_cast<u32>(v.desc.DataFormat());
        const u32 raw_nfmt = v.fetch.data_format != 0 ? v.fetch.num_format : static_cast<u32>(v.desc.NumFormat());
        const u32 nfmt = static_cast<u32>(
            RemapNumberFormat(static_cast<NumberFormat>(raw_nfmt), static_cast<DataFormat>(dfmt)));
        const VkFormat fmt = SurfaceFormat(dfmt, nfmt);
        if (fmt == VK_FORMAT_UNDEFINED) {
            Skipped(Skip::VertexFormat, fmt::format("vertex attribute {} format {}/{}", v.attribute, dfmt, nfmt));
            return;
        }
        const u32 inst = v.fetch.instance_data;
        const u32 divisor = inst == 1 ? regs.Get(kVgtInstanceStepRate0) : (inst == 2 ? regs.Get(kVgtInstanceStepRate1) : 1);
        vattr.push_back({.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                         .location = slot,
                         .binding = slot,
                         .format = fmt,
                         .offset = 0});
        vbind.push_back({.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                         .binding = slot,
                         .stride = v.desc.Stride(),
                         .inputRate = inst == 0 ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE,
                         .divisor = inst == 0 ? 1u : std::max<u32>(divisor, 1)});
        if (v.status == GS::ResourceStatus::Ok && v.address != 0 && v.size != 0) {
            VkBuffer vb = VK_NULL_HANDLE;
            u64 voff = 0, vavail = 0;
            if (!GuestRange(v.address, v.size, false, 4, vb, voff, vavail)) {
                Skipped(Skip::Buffer, fmt::format("vertex buffer at {:#x}", v.address));
                return;
            }
            vbufs.push_back(vb);
            voffs.push_back(voff);
        } else {
            vbufs.push_back(VK_NULL_HANDLE);
            voffs.push_back(0);
        }
    }
    // index buffer
    VkBuffer ibuf = VK_NULL_HANDLE;
    u64 ioff = 0;
    const bool idx32 = (d.index_type & 3) == 1;
    if (d.indexed) {
        const u32 isize = idx32 ? 4 : 2;
        const u64 addr = d.index_base + u64{d.index_offset} * isize;
        const u64 count = d.indirect ? d.max_index_size : std::min(d.index_count, d.max_index_size);
        if (count != 0) {
            u64 iavail = 0;
            if (!GuestRange(addr, count * isize, false, isize, ibuf, ioff, iavail)) {
                Skipped(Skip::Buffer, fmt::format("index buffer at {:#x}", addr));
                return;
            }
        }
    }
    VkBuffer argbuf = VK_NULL_HANDLE;
    u64 argoff = 0;
    if (d.indirect) {
        u64 aavail = 0;
        if (!GuestRange(d.args_addr, d.indexed ? 20 : 16, false, 4, argbuf, argoff, aavail)) {
            Skipped(Skip::Buffer, fmt::format("indirect arguments at {:#x}", d.args_addr));
            return;
        }
    }

    stage(stats_.t_vertex);
    if (histo_.active && d.indirect && argbuf != VK_NULL_HANDLE) {
        static const u64 detail_p = [] {
            char v[16] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_DRAW_HISTO_P", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10)
                                                                                    : ~0ull;
        }();
        if (prep.pipeline == detail_p) {
            // SSPC_DIAG_DRAW_HISTO_P: the indirect arguments this draw takes, read back (P4.5)
            EndRendering();
            if (pending_writes_) {
                GlobalBarrier();
            }
            OwnedBuffer rb;
            std::string rerr;
            if (CreateBuffer(ctx_, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, rb, rerr, true, HostPlacement::Cached)) {
                const VkBufferCopy c{.srcOffset = argoff, .dstOffset = 0, .size = d.indexed ? 20u : 16u};
                Fn().vkCmdCopyBuffer(cur_.cb, argbuf, rb.buffer, 1, &c);
                const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                          .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                          .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
                const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                           .pMemoryBarriers = &mb};
                Fn().vkCmdPipelineBarrier2(cur_.cb, &dep);
                args_rb_.push_back({cur_.value, VideoOut::NextFlipId(), d.args_addr, rb});
            }
        }
    }
    Probe(d, vs, has_ps ? &ps : nullptr);
    // once per pipeline and target images (the register versions in the label's key change almost every draw:
    // g2_village_markers wrote 141 MB keyed by them)
    if (markers_ && label_open_ &&
        legend_done_
            .insert((u64{prep.pipeline} << 40) ^ (u64{color_img[0] != nullptr ? color_img[0]->id : 0} << 20) ^
                    (color_img[1] != nullptr ? u64{color_img[1]->id} << 10 : 0) ^ (depth_img != nullptr ? depth_img->id : 0))
            .second) {
        std::string line = label_text_ + " |";
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                line += fmt::format(" c{} {};", i, DescribeImage(*color_img[i]));
            }
        }
        if (depth_img != nullptr) {
            line += fmt::format(" depth {};", DescribeImage(*depth_img));
        }
        legend_ << line << " | " << key.width << "x" << key.height << " layers " << key.layers << "\n";
    }
    DiagCaptureBefore(prep.pipeline, color_img);
    DiagCaptureDraw(prep.pipeline, color_img, false);
    // barriers and layouts (outside rendering)
    if (pending_writes_) {
        GlobalBarrier(7);
    }
    const bool feedback_any = std::ranges::any_of(bound, [&](CachedImage* b) {
        return std::ranges::find(targets, b) != targets.end();
    });
    bool need_transition = false;
    const auto want = [&](CachedImage* img, VkImageLayout layout) { return img->current != layout; };
    for (u32 i = 0; i < 8; ++i) {
        if (color_img[i] != nullptr &&
            want(color_img[i], feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)) {
            need_transition = true;
        }
    }
    // a draw that samples its own depth attachment: one layout valid for both uses (VUID-vkCmdDraw-imageLayout-00344)
    const bool depth_feedback = depth_img != nullptr && std::ranges::find(bound, depth_img) != bound.end();
    const VkImageLayout depth_layout =
        depth_feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (depth_img != nullptr && want(depth_img, depth_layout)) {
        need_transition = true;
    }
    const auto texture_layout = [](const CachedImage* img) {
        const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written &&
                             img->current == VK_IMAGE_LAYOUT_GENERAL;
        return storage ? VK_IMAGE_LAYOUT_GENERAL
               : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    for (CachedImage* img : bound) {
        // a texture in another layout (rendered, uploaded, stored) needs its barrier outside rendering; one already
        // read in its layout does not (storage writes since are ordered by pending_writes_' global barrier)
        if (std::ranges::find(targets, img) == targets.end() && img->current != texture_layout(img)) {
            need_transition = true;
            break;
        }
    }
    // a pending fast clear needs a render pass of its own (loadOp CLEAR): one already open with the same targets would
    // never clear them (P4.5)
    const bool any_clear = depth_clear || std::ranges::any_of(color_clear, [](bool c) { return c; });
    if (any_clear || need_transition || !rendering_ || !(key == render_)) {
        EndRendering();
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                images_.Transition(cur_.cb, *color_img[i],
                                   feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            }
        }
        if (depth_img != nullptr) {
            images_.Transition(cur_.cb, *depth_img, depth_layout,
                               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        for (u32 k = 0; k < bound.size(); ++k) {
            CachedImage* img = bound[k];
            if (std::ranges::find(targets, img) != targets.end()) {
                continue;
            }
            const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written && img->current == VK_IMAGE_LAYOUT_GENERAL;
            images_.Transition(cur_.cb, *img,
                               storage ? VK_IMAGE_LAYOUT_GENERAL
                               : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT);
        }
    }
    // the descriptors' image layouts are the images' current ones (VUID-vkCmdDraw-None-09600)
    for (size_t i = 0; i < img_infos.size() && i < info_images_.size(); ++i) {
        if (info_images_[i] != nullptr && img_infos[i].imageView != VK_NULL_HANDLE) {
            img_infos[i].imageLayout = info_images_[i]->current;
        }
    }
    for (auto& w : writes) {
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            continue;
        }
        w.pImageInfo = img_infos.data() + reinterpret_cast<size_t>(w.pImageInfo);
    }
    {
        size_t bi = 0;
        for (auto& w : writes) {
            if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                w.pBufferInfo = &buf_infos[bi++];
            }
        }
    }
    stage(stats_.t_layout);
    if (!rendering_) {
        std::array<VkRenderingAttachmentInfo, 8> colors{};
        for (u32 i = 0; i < key.color_count; ++i) {
            colors[i] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                         .imageView = key.color[i],
                         .imageLayout = feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (color_clear[i] && key.color[i] != VK_NULL_HANDLE) {
                colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colors[i].clearValue = ColorBufferClearValue(regs, i).value;
                ++stats_.fast_clears;
            }
        }
        VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = key.depth,
                                        .imageLayout = depth_layout,
                                        .loadOp = depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        depth.clearValue.depthStencil = {std::clamp(F(regs, kDbDepthClear), 0.0f, 1.0f), regs.Get(kDbStencilClear) & 0xFF};
        const VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {{0, 0}, {key.width, key.height}},
                                 .layerCount = key.layers,
                                 .colorAttachmentCount = key.color_count,
                                 .pColorAttachments = colors.data(),
                                 .pDepthAttachment = key.has_depth ? &depth : nullptr,
                                 .pStencilAttachment = key.has_stencil ? &depth : nullptr};
        f.vkCmdBeginRendering(cur_.cb, &ri);
        rendering_ = true;
        render_ = key;
        ++stats_.render_passes;
        if (depth_clear) {
            ++stats_.depth_clears;
        }
        // the clears this render pass made are done
        for (u32 i = 0; i < 8; ++i) {
            if (color_clear[i] && key.color[i] != VK_NULL_HANDLE && target_cache_.cmask[i] != 0) {
                pending_meta_clear_[target_cache_.cmask[i]] = false;
            }
        }
        if (depth_clear && target_cache_.htile != 0) {
            pending_meta_clear_[target_cache_.htile] = false;
        }
    }
    // the pipeline, descriptors and push constants recorded only when they differ from what this command buffer has
    // bound/pushed last (P4.6 night: ~83% of the draws keep the pipeline)
    if (pipe->pipeline != gfx_pipeline_) {
        f.vkCmdBindPipeline(cur_.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
        gfx_pipeline_ = pipe->pipeline;
    } else {
        ++stats_.binds_skipped;
    }
    if (!PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_GRAPHICS, writes)) {
        Skipped(Skip::Pipeline, "descriptor set allocation");
        return;
    }
    if (pipe->layout != push_layout_ || std::memcmp(&push, &push_last_, sizeof(PushData)) != 0) {
        f.vkCmdPushConstants(cur_.cb, pipe->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(PushData), &push);
        push_layout_ = pipe->layout;
        push_last_ = push;
    } else {
        ++stats_.push_constants_skipped;
    }
    SetDynamicState(regs, *st, d.indexed);
    // the hardware's depth/stencil clear (DB_RENDER_CONTROL clear bits): the pixels the draw covers take the clear
    // value - the draw is the game's clear rectangle, so its viewport within its scissor. Not the whole target: a
    // shadow atlas clears each cascade's part before drawing it, and a clear of everything wiped the cascades drawn
    // before (surfaces lit by the sun came out shadowed; P4.5)
    const u32 rc = regs.Get(kDbRenderControl);
    if (depth_img != nullptr && (rc & 3) != 0 && !depth_clear) {
        VkClearAttachment ca{.aspectMask = 0};
        if ((rc & 1) && key.has_depth && ((dc >> 1) & 1) && ((dc >> 2) & 1)) {
            ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((rc & 2) && key.has_stencil) {
            ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        const VkViewport& vp = dyn_.vps[0];
        const VkRect2D& sc = dyn_.scs[0];
        const float vx0 = std::min(vp.x, vp.x + vp.width), vx1 = std::max(vp.x, vp.x + vp.width);
        const float vy0 = std::min(vp.y, vp.y + vp.height), vy1 = std::max(vp.y, vp.y + vp.height);
        const s64 x0 = std::max<s64>({0, sc.offset.x, static_cast<s64>(std::floor(vx0))});
        const s64 y0 = std::max<s64>({0, sc.offset.y, static_cast<s64>(std::floor(vy0))});
        const s64 x1 = std::min<s64>({key.width, s64{sc.offset.x} + sc.extent.width, static_cast<s64>(std::ceil(vx1))});
        const s64 y1 = std::min<s64>({key.height, s64{sc.offset.y} + sc.extent.height, static_cast<s64>(std::ceil(vy1))});
        if (ca.aspectMask != 0 && x1 > x0 && y1 > y0) {
            ca.clearValue.depthStencil = {std::clamp(F(regs, kDbDepthClear), 0.0f, 1.0f),
                                          regs.Get(kDbStencilClear) & 0xFF};
            const VkClearRect rect{{{static_cast<s32>(x0), static_cast<s32>(y0)},
                                    {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}},
                                   0,
                                   key.layers};
            f.vkCmdClearAttachments(cur_.cb, 1, &ca, 1, &rect);
            ++stats_.depth_clears;
            stats_.depth_clears_partial += (x1 - x0) < key.width || (y1 - y0) < key.height ? 1 : 0;
            if (histo_.active) { // SSPC_DIAG_DRAW_HISTO: the frame's clears in order
                histo_.detail.push_back(fmt::format(
                    "    depth clear: draw {} pipeline {} packet {:#x} depth {:#x} aspects {:#x} rect {},{} {}x{} value {} "
                    "| rc {:#x} dc {:#x} prim {} count {}",
                    stats_.draws, prep.pipeline, d.packet_addr, depth_img->layout.guest_address, ca.aspectMask, x0, y0,
                    x1 - x0, y1 - y0, ca.clearValue.depthStencil.depth, rc, dc, d.primitive_type, d.index_count));
            }
        }
    }
    const auto same_input = [](const auto& a, const auto& b) {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
    };
    if (!dyn_vertex_valid_ || !same_input(vbind, dyn_.vbind) || !same_input(vattr, dyn_.vattr)) {
        f.vkCmdSetVertexInputEXT(cur_.cb, static_cast<u32>(vbind.size()), vbind.data(), static_cast<u32>(vattr.size()),
                                 vattr.data());
        dyn_.vbind = vbind;
        dyn_.vattr = vattr;
        dyn_vertex_valid_ = true;
        ++dyn_set_;
    } else {
        ++dyn_skipped_;
    }
    if (!vbufs.empty()) {
        // (P4.7: not again when this command buffer has these bound - the foliage's instanced draws repeat their mesh)
        if (bind_shadow_ && vb_valid_ && same_input(vbufs, vb_last_) && same_input(voffs, vb_off_last_)) {
            ++stats_.vb_binds_skipped;
        } else {
            f.vkCmdBindVertexBuffers(cur_.cb, 0, static_cast<u32>(vbufs.size()), vbufs.data(), voffs.data());
            stats_.vertex_bindings += vbufs.size();
            vb_last_ = vbufs;
            vb_off_last_ = voffs;
            vb_valid_ = true;
        }
    }
    // the fetch shader's vertex/instance offsets (the bridge's GetDrawOffsets)
    const GS::FetchShader& fetch = vs.module->result.reflection.fetch;
    const auto& vs_ud = d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
    u32 vertex_offset = d.vgt_index_offset;
    u32 instance_offset = 0;
    if (fetch.present) {
        if (vertex_offset == 0 && fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
            vertex_offset = vs_ud[fetch.vertex_offset_sgpr];
        }
        if (fetch.instance_offset_sgpr >= 0 && fetch.instance_offset_sgpr < 16) {
            instance_offset = vs_ud[fetch.instance_offset_sgpr];
        }
    }
    const u32 instances = std::max<u32>(d.instance_count, 1);
    u64 cid = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        std::string text = fmt::format("draw q{} seq {} packet {:#x} pipeline {} vs {:#x} ps {:#x} prim {}{}{} count {} "
                                       "instances {} render {}x{}x{}",
                                       d.queue, d.seq, d.packet_addr, prep.pipeline, vs.address,
                                       has_ps ? ps.address : 0, d.primitive_type, d.indexed ? " indexed" : "",
                                       d.indirect ? " indirect" : "", d.index_count, d.instance_count, key.width,
                                       key.height, key.layers);
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                text += fmt::format(" | rt{} {}", i, DescribeImage(*color_img[i]));
            }
        }
        if (depth_img != nullptr) {
            text += fmt::format(" | depth {}", DescribeImage(*depth_img));
        }
        for (const CachedImage* img : bound) {
            text += fmt::format(" | tex {}", DescribeImage(*img));
        }
        cid = DiagBegin(std::move(text));
    }
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    // smoke research: SSPC_DIAG_SKIP_DRAWS leaves out only the draw command (its render pass, barriers, clears, state
    // and uploads above are recorded as for any draw); SSPC_DIAG_CMD_TRACE keeps the command's identities
    bool diag_skip = false;
    for (const CmdFilter& c : skip_draws_) {
        diag_skip = diag_skip || (c.what == 'v' && StageMatches(vs, c.value)) ||
                    (c.what == 'p' && has_ps && StageMatches(ps, c.value)) || (c.what == 'P' && prep.pipeline == c.value) ||
                    (c.what == 'n' && trace_.ordinal >= c.value && trace_.ordinal <= c.last) ||
                    (c.what == 'r' && std::ranges::any_of(color_img, [&](const CachedImage* i) {
                         return i != nullptr && i->layout.guest_address == c.value;
                     }));
    }
    if (trace_.Active()) {
        CmdTraceEntry& e = CmdTraceAdd('D', d.queue, d.seq, d.packet_addr);
        e.skip = diag_skip ? 1 : 0;
        e.pipeline = prep.pipeline;
        e.address[0] = vs.address;
        e.footer[0] = vs.footer_hash;
        e.content[0] = vs.program != nullptr ? vs.program->id.hash.lo : 0;
        if (has_ps) {
            e.address[1] = ps.address;
            e.footer[1] = ps.footer_hash;
            e.content[1] = ps.program != nullptr ? ps.program->id.hash.lo : 0;
        }
        e.prim = d.primitive_type;
        e.count = d.index_count;
        e.instances = d.instance_count;
        e.indexed = d.indexed;
        e.indirect = d.indirect;
        e.args = d.indirect ? d.args_addr : 0;
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                e.rt[i] = color_img[i]->layout.guest_address;
                e.rt_fmt[i] = color_img[i]->layout.pixel_format;
            }
        }
        e.depth = depth_img != nullptr ? depth_img->layout.guest_address : 0;
        e.blend0 = regs.Get(0xA1E0);
        e.depth_control = dc;
        CmdTraceRanges(vs.plan, 0);
        if (has_ps) {
            CmdTraceRanges(ps.plan, 1);
        }
        for (const auto& v : vs.plan.vertices) {
            if (v.address != 0 && v.size != 0) {
                trace_.ranges.push_back({v.address, v.size, 2, false});
            }
        }
        if (d.indexed) {
            const u32 isize = idx32 ? 4 : 2;
            trace_.ranges.push_back({d.index_base + u64{d.index_offset} * isize,
                                     (d.indirect ? d.max_index_size : std::min(d.index_count, d.max_index_size)) * u64{isize},
                                     3, false});
        }
        if (d.indirect) {
            trace_.ranges.push_back({d.args_addr, d.indexed ? 20u : 16u, 4, false});
        }
        e.ranges_count = static_cast<u32>(trace_.ranges.size()) - e.ranges_begin;
        for (const CachedImage* img : bound) {
            trace_.images.push_back(img->layout.guest_address | (u64{img->layout.pixel_format & 0x7FFFFF} << 40) |
                                    (img->gpu_written && (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? 1ull << 63 : 0));
        }
        e.images_count = static_cast<u32>(trace_.images.size()) - e.images_begin;
    }
    if (diag_skip) {
        ++stats_.diag_skipped_draws;
    } else {
    if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            const VkIndexType it = idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            if (bind_shadow_ && ibuf == ib_last_ && ioff == ib_off_last_ && it == ib_type_last_) {
                ++stats_.ib_binds_skipped; // (P4.7: the bound one)
            } else {
                f.vkCmdBindIndexBuffer(cur_.cb, ibuf, ioff, it);
                ib_last_ = ibuf;
                ib_off_last_ = ioff;
                ib_type_last_ = it;
            }
        }
        ++stats_.indexed;
    }
    if (d.indirect) {
        ++stats_.indirect;
        if (d.indexed) {
            if (ibuf != VK_NULL_HANDLE) {
                f.vkCmdDrawIndexedIndirect(cur_.cb, argbuf, argoff, 1, 20);
            }
        } else {
            f.vkCmdDrawIndirect(cur_.cb, argbuf, argoff, 1, 16);
        }
    } else if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            f.vkCmdDrawIndexed(cur_.cb, std::min(d.index_count, d.max_index_size), instances, 0,
                               static_cast<s32>(vertex_offset), instance_offset);
        }
    } else {
        f.vkCmdDraw(cur_.cb, d.index_count, instances, vertex_offset, instance_offset);
    }
    }
    DiagEnd(cid);
    DiagCaptureDraw(prep.pipeline, color_img, true);
    stage(stats_.t_emit);
    if (prof_) {
        ProfMark((u64{prep.pipeline} << 2) | 0,
                 fmt::format("draw pipeline {} vs {:#x} ps {:#x} prim {} target {}x{} rt0 fmt {} depth {}", prep.pipeline,
                             vs.address, has_ps ? ps.address : 0, d.primitive_type, key.width, key.height,
                             color_img[0] != nullptr ? color_img[0]->layout.pixel_format : 0, depth_img != nullptr));
        ProfDescribe((u64{prep.pipeline} << 2) | 0, "draw: vertex stage", &vs, bound);
        if (has_ps) {
            ProfDescribe((u64{prep.pipeline} << 2) | 0x8000000000000000ull, "draw: pixel stage", &ps, bound);
        }
    }
    ++stats_.draws_executed;
    if (histo_.active) {
        auto& e = histo_.draws[prep.pipeline];
        ++e.count;
        e.indirect += d.indirect ? 1 : 0;
        e.instances += d.indirect ? 0 : instances;
        e.verts += d.indirect ? 0 : d.index_count;
        // SSPC_DIAG_DRAW_HISTO_P=<pipeline>: that pipeline's draws in detail (textures, vertex buffers)
        static const std::vector<u64> detail = [] {
            std::vector<u64> v;
            char s[256] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_DRAW_HISTO_P", s, sizeof(s)) > 0) {
                for (char* p = s; *p != 0;) {
                    v.push_back(std::strtoull(p, &p, 10));
                    while (*p == ',' || *p == ' ') {
                        ++p;
                    }
                }
            }
            return v;
        }();
        if (std::ranges::find(detail, u64{prep.pipeline}) != detail.end()) {
            std::string line = fmt::format("    draw verts {} inst {} indirect {} args {:#x}", d.index_count, instances,
                                           d.indirect, d.indirect ? d.args_addr : 0);
            for (const auto* sv : {&vs, &ps}) {
                for (const auto& b : sv->plan.buffers) {
                    if (b.kind == GS::BufferKind::Guest) {
                        line += fmt::format(" | buf {:#x}+{:#x}{}", b.address, b.size, b.written ? " W" : "");
                    }
                }
            }
            line = fmt::format("    p{} ", prep.pipeline) + line.substr(4);
            for (const CachedImage* img : bound) {
                line += fmt::format(" | img{} {:#x} fmt {} {}x{} lv {} ly {} type {}{}{}{}", img->id,
                                    img->layout.guest_address, img->layout.pixel_format, img->layout.size.width,
                                    img->layout.size.height, img->layout.levels, img->layout.layers,
                                    static_cast<u32>(img->layout.type), img->layout.props.is_depth ? " depth" : "",
                                    img->gpu_written ? " gw" : "", img->alias_hits != 0 ? " alias" : "");
            }
            for (const auto* sv : {&vs, &ps}) {
                for (const auto& im : sv->plan.images) {
                    line += fmt::format(" | T# {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} depth {} array {} "
                                        "storage {}",
                                        im.desc.raw[0], im.desc.raw[1], im.desc.raw[2], im.desc.raw[3], im.desc.raw[4],
                                        im.desc.raw[5], im.desc.raw[6], im.desc.raw[7], im.depth, im.array, im.storage);
                }
                for (const auto& sm : sv->plan.samplers) {
                    line += fmt::format(" | S# {:08x} {:08x} {:08x} {:08x}", sm.desc.raw[0], sm.desc.raw[1],
                                        sm.desc.raw[2], sm.desc.raw[3]);
                }
            }
            for (const auto& v : vs.plan.vertices) {
                const u32 dfmt = v.fetch.data_format != 0 ? v.fetch.data_format : static_cast<u32>(v.desc.DataFormat());
                const u32 raw_nfmt = v.fetch.data_format != 0 ? v.fetch.num_format : static_cast<u32>(v.desc.NumFormat());
                const u32 nfmt = static_cast<u32>(
                    RemapNumberFormat(static_cast<NumberFormat>(raw_nfmt), static_cast<DataFormat>(dfmt)));
                line += fmt::format(" | vb {:#x}+{:#x} attr {} dfmt {} nfmt {}->{} vk {} stride {} V#dfmt {} V#nfmt {} "
                                    "fetch dfmt {} nfmt {}",
                                    v.address, v.size, v.attribute, dfmt, raw_nfmt, nfmt,
                                    static_cast<int>(SurfaceFormat(dfmt, nfmt)), v.desc.Stride(),
                                    static_cast<u32>(v.desc.DataFormat()), static_cast<u32>(v.desc.NumFormat()),
                                    v.fetch.data_format, v.fetch.num_format);
            }
            histo_.detail.push_back(std::move(line));
        }
        if (e.legend.empty()) {
            e.legend = fmt::format("vs {:#x} ps {:#x} prim {} rt0 fmt {} {}x{} depth {} blend0 {:#x} rt0 {:#x} db {:#x}",
                                   vs.address, has_ps ? ps.address : 0, d.primitive_type,
                                   color_img[0] != nullptr ? color_img[0]->layout.pixel_format : 0, key.width, key.height,
                                   depth_img != nullptr, regs.Get(0xA1E0),
                                   color_img[0] != nullptr ? color_img[0]->layout.guest_address : 0,
                                   depth_img != nullptr ? depth_img->layout.guest_address : 0);
        }
    }
    if (writes_memory) {
        MarkWrites(0);
    }
    const u64 draw_ticks = GS::Ticks() - t0;
    stats_.record_ns += GS::TicksToNs(draw_ticks);
    if (insp_catalog_ != nullptr) {
        auto& c = insp_cat_[static_cast<size_t>(insp_cls.category)];
        const u64 inst = d.indirect ? 0 : instances, verts = d.indirect ? 0 : d.index_count;
        ++c.draws;
        c.instances += inst;
        c.vertices += verts * std::max<u64>(inst, 1);
        c.cpu_ticks += draw_ticks;
        if (prep.pipeline < (1u << 20)) {
            if (insp_pipelines_.size() <= prep.pipeline) {
                insp_pipelines_.resize(prep.pipeline + 1);
            }
            InspPipeline& p = insp_pipelines_[prep.pipeline];
            if (p.draws == 0) {
                p.vs = vs.program != nullptr ? vs.program->id.hash.lo : 0;
                p.ps = has_ps && ps.program != nullptr ? ps.program->id.hash.lo : 0;
                p.vs_addr = vs.address;
                p.ps_addr = has_ps ? ps.address : 0;
                p.cls = insp_cls;
                p.prim = d.primitive_type;
                p.width = key.width;
                p.height = key.height;
                p.depth = depth_img != nullptr;
                for (const CachedImage* ci : color_img) {
                    p.colors += ci != nullptr ? 1 : 0;
                }
            }
            ++p.draws;
            p.instances += inst;
            p.vertices += verts * std::max<u64>(inst, 1);
            p.indirect += d.indirect ? 1 : 0;
            p.cpu_ticks += draw_ticks;
        }
    }
    if (commands_ >= options_.max_draws_per_batch) {
        ++stats_.flushes_full;
        flush_full_ = true;
        Flush();
    }
}

void Executor::Dispatch(const Cp::DispatchDescriptor& d) {
    DispatchWith(d, cp_->Compute(d.queue));
}

void Executor::DispatchWith(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_); // (SSPC_GPU_TIMINGS=sample:N: this dispatch's stage timers)
    PreparedCommand pc;
    if (cs != nullptr) {
        const u64 tf0 = GS::FineTicks();
        pc.prep = &frontend_.PrepareDispatch(d, std::span<const u32, Cp::kComputeCount>(cs->values), reader_);
        pc.stage = &frontend_.Stage(GS::HwStage::Cs);
        stats_.t_front += GS::FineTicks() - tf0;
    }
    RecordDispatch(d, cs, pc, t0);
}

void Executor::DispatchPrepared(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs,
                                const PreparedCommand& pc) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_);
    RecordDispatch(d, cs, pc, t0);
}

void Executor::RecordDispatch(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs,
                              const PreparedCommand& pc, u64 t0) {
    ++stats_.dispatches;
    ++trace_.ordinal;
    const auto& f = Fn();
    const GS::DrawPreparation* prepared = cs == nullptr ? nullptr : pc.prep;
    if (!Begin()) {
        return;
    }
    Note(d.queue, d.seq);
    ++commands_;
    write_run_ = false;
    if (cs == nullptr) {
        Skipped(Skip::NotReady, "no compute registers for the queue");
        return;
    }
    if (prepared == nullptr || pc.stage == nullptr) {
        ++stats_.prepared_mismatches;
        Skipped(Skip::NotReady, fmt::format("dispatch at {:#x}: no preparation", d.packet_addr));
        return;
    }
    const GS::DrawPreparation& prep = *prepared;
    if (!prep.ready) {
        Skipped(prep.unsupported ? Skip::Unsupported : Skip::NotReady,
                fmt::format("dispatch at {:#x}: CS {}", d.packet_addr, GS::StageStatusName(pc.stage->status)));
        return;
    }
    const GS::StageView& csv = *pc.stage;
    if (LabelChanges((1ull << 62) | csv.address)) {
        Label((1ull << 62) | csv.address,
              fmt::format("X cs{:x} {}x{}x{}{} @{:x}", csv.address, d.groups[0], d.groups[1], d.groups[2],
                          d.indirect ? " indirect" : "", d.packet_addr));
    }
    Inspector::Class insp_cls{};
    if (insp_catalog_ != nullptr) {
        insp_cls = InspClassifyDispatch(csv);
        InspMark(static_cast<size_t>(insp_cls.category)); // (its uploads and barriers count to it)
    }
    lds_groups_ = std::max<u64>(1, u64{cs->values[Cp::Reg::ComputeDimX]} * cs->values[Cp::Reg::ComputeDimX + 1] *
                                       cs->values[Cp::Reg::ComputeDimX + 2]);
    lds_group_bytes_ = ((d.shader.rsrc2 >> 15) & 0x1FFu) * 512u; // COMPUTE_PGM_RSRC2.LDS_SIZE, 128-dword units
    // a compute clear of a target's CMASK/HTILE (no address arithmetic): the target clears at its next rendering
    const GS::ShaderReflection& refl = csv.module->result.reflection;
    {
        // diagnostics: compute writes/reads of the metadata of targets the image cache knows (the bridge's IsMeta)
        bool reads_meta = false, writes_meta = false;
        u64 meta_addr = 0;
        for (const auto& b : csv.plan.buffers) {
            if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok &&
                images_.MetaOwner(b.address) != nullptr) {
                if (b.written) {
                    writes_meta = true;
                    meta_addr = b.address;
                } else {
                    reads_meta = true;
                }
            }
        }
        if (writes_meta) {
            ++stats_.meta_writes;
            stats_.meta_writes_xor += refl.has_bitwise_xor ? 1 : 0;
            stats_.meta_writes_reading += reads_meta ? 1 : 0;
            if (stats_.meta_writes <= 24) {
                const CachedImage* owner = images_.MetaOwner(meta_addr);
                SSPC_LOG(GPU, Info,
                         "native backend: dispatch {:#x} (cs {:#x}, groups {}x{}x{}) writes metadata {:#x} of img{} "
                         "{:#x} fmt {} {}x{} depth {} (xor {}, reads metadata {})",
                         d.packet_addr, csv.address, d.groups[0], d.groups[1], d.groups[2], meta_addr, owner->id,
                         owner->layout.guest_address, owner->layout.pixel_format, owner->layout.size.width,
                         owner->layout.size.height, owner->layout.props.is_depth, refl.has_bitwise_xor, reads_meta);
            }
        }
    }
    if (!refl.has_bitwise_xor) {
        for (const auto& b : csv.plan.buffers) {
            if (b.kind == GS::BufferKind::Guest && b.written && b.status == GS::ResourceStatus::Ok) {
                // registered when targets bound: the CMASK/HTILE addresses seen so far
                const Cp::RegisterFile& regs = *regs_;
                bool meta = false;
                for (u32 i = 0; i < 8 && !meta; ++i) {
                    meta = (u64{regs.Get(Cp::Reg::CbColor0Base + i * Cp::Reg::CbColorStride + 7)} << 8) == b.address;
                }
                meta = meta || (u64{regs.Get(kDbHtileBase)} << 8) == b.address;
                if (meta && b.address != 0) {
                    pending_meta_clear_[b.address] = true;
                    Skipped(Skip::MetaClear, fmt::format("compute write of metadata at {:#x}", b.address));
                    return;
                }
            }
        }
    }
    std::string err;
    const PipelineObjects* pipe = pipelines_.Compute(*csv.module, csv.plan, err);
    DiagDumpShader(csv.address, csv.program != nullptr ? &csv.program->code : nullptr, csv.module->result.spirv,
                   csv.runtime, 0);
    if (pipe == nullptr) {
        Skipped(Skip::Pipeline, err);
        return;
    }
    PushData push{};
    // the command's descriptor lists: reused (their capacity kept) - per-command vectors were 14% of the GPU thread's
    // samples in the heap (f1)
    std::vector<VkWriteDescriptorSet>& writes = scratch_.writes;
    std::vector<VkDescriptorBufferInfo>& buf_infos = scratch_.buf_infos;
    std::vector<VkDescriptorImageInfo>& img_infos = scratch_.img_infos;
    std::vector<CachedImage*>& bound = scratch_.bound;
    writes.clear();
    buf_infos.clear();
    img_infos.clear();
    bound.clear();
    bool writes_memory = false;
    info_images_.clear();
    const std::vector<CachedImage*> none;
    const u64 tb0 = GS::FineTicks();
    if (!BindStage(&csv, true, push, writes, buf_infos, img_infos, bound, writes_memory, none)) {
        return;
    }
    stats_.t_bind += GS::FineTicks() - tb0;
    VkBuffer argbuf = VK_NULL_HANDLE;
    u64 argoff = 0;
    if (d.indirect) {
        u64 aavail = 0;
        if (!GuestRange(d.indirect_addr, 12, false, 4, argbuf, argoff, aavail)) {
            Skipped(Skip::Buffer, fmt::format("indirect dispatch arguments at {:#x}", d.indirect_addr));
            return;
        }
    }
    EndRendering();
    // the dispatch's guest memory accesses (its bindings): a barrier only when they touch what was written since the
    // last one, or write what was read since (unknown: images it stores to, GDS, LDS, special buffers)
    bool acc_unknown = false;
    acc_reads_.clear();
    acc_writes_.clear();
    for (const auto& b : csv.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest) {
            if (b.status == GS::ResourceStatus::Ok && b.size != 0 && b.address != 0) {
                (b.written ? acc_writes_ : acc_reads_).emplace_back(b.address, b.address + b.size);
            }
        } else if (b.kind == GS::BufferKind::GdsBuffer) {
            // GDS (gds_): a range of its own, above the guest's 40-bit addresses
            (b.written ? acc_writes_ : acc_reads_).emplace_back(kGdsRangeBase, kGdsRangeBase + gds_.size);
        } else if (b.kind != GS::BufferKind::Flatbuf && b.kind != GS::BufferKind::ClipPlanes) {
            acc_unknown = true; // the emulated LDS (shared by every dispatch), BDA, ...
        }
    }
    // storage images need nothing here: ImageCache::Transition orders each image's accesses with its own barrier (a
    // write before or after it -> an image barrier from the image's last stage/access)
    if (d.indirect) {
        acc_reads_.emplace_back(d.indirect_addr, d.indirect_addr + 12);
    }
    NoteClockLabelReads(&csv.plan, csv.address);
    NoteOrderRange(&csv.plan, csv.address, d.queue, true);
    // dispatches of one queue the guest does not order (no sync point between them) need no barrier for their range
    // conflicts (P4.6 night; SSPC_P46_GUEST_ORDER=0: a barrier for every conflict - the earlier behaviour)
    static const bool guest_order = [] {
        char v[4] = {};
        const bool off = GetEnvironmentVariableA("SSPC_P46_GUEST_ORDER", v, sizeof(v)) == 1 && v[0] == '0';
        if (off) {
            SSPC_LOG(GPU, Warning, "native backend: SSPC_P46_GUEST_ORDER=0 - a barrier for every range conflict between "
                                   "dispatches");
        } else {
            SSPC_LOG(GPU, Info, "native backend: dispatches of one queue without a guest synchronization between them "
                                "are not ordered by a barrier (SSPC_P46_GUEST_ORDER=0: they are)");
        }
        return !off;
    }();
    const u64 tag = guest_order && d.queue < guest_epoch_.size()
                        ? ((u64{d.queue} + 1) << 32) | guest_epoch_[d.queue]
                        : 0;
    const u64 why_before = stats_.barrier_why[4] + stats_.barrier_why[5] + stats_.barrier_why[6];
    if (NeedsBarrier(acc_reads_, acc_writes_, acc_unknown, tag)) {
        GlobalBarrier(8);
        // diagnostics: a range conflict with the commands since the last barrier - did the guest flush (CS partial
        // flush, ACQUIRE_MEM, ...) on this queue since its previous dispatch?
        if (stats_.barrier_why[4] + stats_.barrier_why[5] + stats_.barrier_why[6] != why_before &&
            d.queue < guest_flush_.size()) {
            ++(guest_flush_[d.queue] != 0 ? stats_.conflict_guest_flushed : stats_.conflict_guest_silent);
        }
    }
    if (d.queue < guest_flush_.size()) {
        guest_flush_[d.queue] = 0;
    }
    for (CachedImage* img : bound) {
        const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written;
        images_.Transition(cur_.cb, *img,
                           storage ? VK_IMAGE_LAYOUT_GENERAL
                           : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           storage ? (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)
                                   : VK_ACCESS_2_SHADER_READ_BIT);
    }
    for (auto& w : writes) {
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            w.pImageInfo = img_infos.data() + reinterpret_cast<size_t>(w.pImageInfo);
        }
    }
    size_t bi = 0;
    for (auto& w : writes) {
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            w.pBufferInfo = &buf_infos[bi++];
        }
    }
    // the descriptors' image layouts are the images' current ones (after the transitions above)
    for (size_t i = 0; i < img_infos.size() && i < info_images_.size(); ++i) {
        if (info_images_[i] != nullptr && img_infos[i].imageView != VK_NULL_HANDLE) {
            img_infos[i].imageLayout = info_images_[i]->current;
        }
    }
    // an indirect dispatch's size is on the GPU: taken as large
    PrepareShadows(&csv.plan, buf_infos,
                   d.indirect ? ~0ull : u64{d.groups[0]} * std::max<u32>(d.groups[1], 1) * std::max<u32>(d.groups[2], 1));
    f.vkCmdBindPipeline(cur_.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipeline);
    if (!PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_COMPUTE, writes)) {
        FinishShadows();
        MarkWrites(6);
        Skipped(Skip::Pipeline, "descriptor set allocation");
        return;
    }
    f.vkCmdPushConstants(cur_.cb, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData), &push);
    push_layout_ = pipe->layout; // (the next draw's layout differs: it pushes its own again)
    push_last_ = push;
    u64 cid = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        std::string text = fmt::format("dispatch q{} seq {} packet {:#x} cs {:#x} groups {}x{}x{}{}", d.queue, d.seq,
                                       d.packet_addr, csv.address, d.groups[0], d.groups[1], d.groups[2],
                                       d.indirect ? " indirect" : "");
        for (const CachedImage* img : bound) {
            text += fmt::format(" | image {}", DescribeImage(*img));
        }
        cid = DiagBegin(std::move(text));
    }
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    // SSPC_DIAG_MEASURE=skip_q1small: MEASUREMENT ONLY (wrong output) - the compute queues' dispatches of < 8 groups
    // are not executed: what they cost the GPU without the profiler's serialization (never a configuration)
    static const bool skip_small = [] {
        char v[32] = {};
        const bool s = GetEnvironmentVariableA("SSPC_DIAG_MEASURE", v, sizeof(v)) > 0 &&
                       std::string_view(v) == "skip_q1small";
        if (s) {
            SSPC_LOG(GPU, Warning, "MEASUREMENT ONLY: SSPC_DIAG_MEASURE=skip_q1small - small compute-queue dispatches are "
                                   "not executed (the output is wrong)");
        }
        return s;
    }();
    ProbeDispatch(d, csv, false);
    // smoke research: SSPC_DIAG_SKIP_DISPATCHES leaves out only the dispatch command (its barrier, shadows and noted
    // accesses stay); SSPC_DIAG_CMD_TRACE keeps the command's identities
    bool diag_skip = false;
    for (const CmdFilter& c : skip_dispatches_) {
        diag_skip = diag_skip || (c.what == 'c' && StageMatches(csv, c.value)) ||
                    (c.what == 'n' && trace_.ordinal >= c.value && trace_.ordinal <= c.last);
    }
    if (trace_.Active()) {
        CmdTraceEntry& e = CmdTraceAdd('X', d.queue, d.seq, d.packet_addr);
        e.skip = diag_skip ? 1 : 0;
        e.pipeline = prep.pipeline;
        e.address[0] = csv.address;
        e.footer[0] = csv.footer_hash;
        e.content[0] = csv.program != nullptr ? csv.program->id.hash.lo : 0;
        e.groups[0] = d.groups[0];
        e.groups[1] = d.groups[1];
        e.groups[2] = d.groups[2];
        e.indirect = d.indirect;
        e.args = d.indirect ? d.indirect_addr : 0;
        CmdTraceRanges(csv.plan, 0);
        if (d.indirect) {
            trace_.ranges.push_back({d.indirect_addr, 12, 4, false});
        }
        e.ranges_count = static_cast<u32>(trace_.ranges.size()) - e.ranges_begin;
        for (const CachedImage* img : bound) {
            trace_.images.push_back(img->layout.guest_address | (u64{img->layout.pixel_format & 0x7FFFFF} << 40) |
                                    (img->gpu_written && (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? 1ull << 63 : 0));
        }
        e.images_count = static_cast<u32>(trace_.images.size()) - e.images_begin;
    }
    if (diag_skip) {
        ++stats_.diag_skipped_dispatches;
    } else if (skip_small && d.queue != 0 && !d.indirect && u64{d.groups[0]} * d.groups[1] * d.groups[2] < 8) {
        // nothing recorded
    } else if (d.indirect) {
        f.vkCmdDispatchIndirect(cur_.cb, argbuf, argoff);
    } else if (!SplitMergeLevels(d, csv, *pipe, push, writes, buf_infos)) {
        f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
    }
    DiagEnd(cid);
    if (prof_) {
        ProfMark((csv.address << 2) | 1, fmt::format("dispatch q{} cs {:#x} groups {}x{}x{}{}", d.queue, csv.address,
                                                     d.groups[0], d.groups[1], d.groups[2], d.indirect ? " indirect" : ""));
        ProfDescribe((csv.address << 2) | 1, "dispatch", &csv, bound);
        // SSPC_DIAG_DUP_DISPATCH=<hex cs address> (with SSPC_GPU_PROFILE): the dispatch once more right after itself
        // (same inputs, same outputs) and an empty interval - whether its time is its own execution or a stall
        static const u64 dup = [] {
            char v[32] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_DUP_DISPATCH", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16)
                                                                                      : 0ull;
        }();
        if (dup != 0 && csv.address == dup && !d.indirect) {
            GlobalBarrier();
            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
            ProfMark(DiagKey(6), "DIAG: an empty interval (timestamp to timestamp)");
            f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
            ProfMark(DiagKey(7), "DIAG: the same dispatch again, right after itself");
            f.vkCmdDispatch(cur_.cb, 1, 1, 1);
            ProfMark(DiagKey(8), "DIAG: the same program, one workgroup");
            // its buffers copied into one dense device-local buffer (not the sparse mirror, not the ring): the same
            // dispatch reading and writing there (the guest's data untouched)
            std::string serr;
            if (diag_scratch_.buffer == VK_NULL_HANDLE) {
                CreateBuffer(ctx_, 16ull << 20,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, diag_scratch_, serr);
            }
            if (diag_scratch_.buffer == VK_NULL_HANDLE && dense_logged_++ == 0) {
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC: no dense scratch buffer: {}", serr);
            }
            // 9: every buffer dense; 10: only the read ones (the written stay in the mirror); 11: only the written
            for (u32 variant = 9; variant <= 11 && diag_scratch_.buffer != VK_NULL_HANDLE; ++variant) {
                GlobalBarrier(); // the copies after the dispatches above
                std::vector<VkDescriptorBufferInfo> dup_infos = buf_infos;
                u64 at = 0;
                bool fits = true;
                for (size_t k = 0; k < dup_infos.size(); ++k) {
                    auto& bi = dup_infos[k];
                    if (bi.buffer == VK_NULL_HANDLE || bi.range == VK_WHOLE_SIZE || bi.range == 0) {
                        continue;
                    }
                    const bool written = k < csv.plan.buffers.size() && csv.plan.buffers[k].written;
                    if ((variant == 10 && written) || (variant == 11 && !written)) {
                        continue;
                    }
                    at = (at + 255) & ~255ull;
                    if (at + bi.range > diag_scratch_.size) {
                        fits = false;
                        break;
                    }
                    const VkBufferCopy c{.srcOffset = bi.offset, .dstOffset = at, .size = bi.range};
                    f.vkCmdCopyBuffer(cur_.cb, bi.buffer, diag_scratch_.buffer, 1, &c);
                    bi.buffer = diag_scratch_.buffer;
                    bi.offset = at;
                    at += bi.range;
                }
                if (dense_logged_++ == 0) {
                    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: dense copy of the dispatch's {} buffers: {} bytes, fits {}",
                             dup_infos.size(), at, fits);
                }
                if (fits) {
                    GlobalBarrier();
                    std::vector<VkWriteDescriptorSet> dup_writes = writes;
                    for (auto& w : dup_writes) {
                        if (w.pBufferInfo != nullptr) {
                            w.pBufferInfo = dup_infos.data() + (w.pBufferInfo - buf_infos.data());
                        }
                    }
                    if (PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_COMPUTE, dup_writes)) {
                        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                        f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
                        ProfMark(DiagKey(variant), variant == 9    ? "DIAG: the same dispatch, all its buffers in a dense copy"
                                          : variant == 10 ? "DIAG: the same dispatch, read buffers dense, written in the mirror"
                                                          : "DIAG: the same dispatch, written buffers dense, read in the mirror");
                        if (variant == 11) {
                            // the dense outputs copied back over the mirror's (the same values): a copy's writes
                            GlobalBarrier();
                            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                            u32 copies = 0;
                            for (size_t k = 0; k < dup_infos.size() && k < csv.plan.buffers.size(); ++k) {
                                if (csv.plan.buffers[k].written && dup_infos[k].buffer == diag_scratch_.buffer) {
                                    const VkBufferCopy c{.srcOffset = dup_infos[k].offset,
                                                         .dstOffset = buf_infos[k].offset,
                                                         .size = dup_infos[k].range};
                                    f.vkCmdCopyBuffer(cur_.cb, diag_scratch_.buffer, buf_infos[k].buffer, 1, &c);
                                    ++copies;
                                }
                            }
                            ProfMark(DiagKey(12), "DIAG: the written buffers copied from the dense copy into the mirror");
                            // the same bytes through a shader-free fill-free path: dense -> dense
                            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                            for (size_t k = 0; k < dup_infos.size() && k < csv.plan.buffers.size(); ++k) {
                                if (csv.plan.buffers[k].written && dup_infos[k].buffer == diag_scratch_.buffer) {
                                    const VkBufferCopy c{.srcOffset = dup_infos[k].offset,
                                                         .dstOffset = (8ull << 20) + dup_infos[k].offset,
                                                         .size = dup_infos[k].range};
                                    f.vkCmdCopyBuffer(cur_.cb, diag_scratch_.buffer, diag_scratch_.buffer, 1, &c);
                                }
                            }
                            ProfMark(DiagKey(13), "DIAG: the same copies dense -> dense");
                            (void)copies;
                        }
                    }
                }
            }
        }
    }
    FinishShadows();
    ProbeDispatch(d, csv, true);
    ++stats_.dispatches_executed;
    if (histo_.active) {
        auto& e = histo_.dispatches[csv.address];
        ++e.count;
        e.indirect += d.indirect ? 1 : 0;
        e.instances += d.indirect ? 0 : u64{d.groups[0]} * std::max<u32>(d.groups[1], 1) * std::max<u32>(d.groups[2], 1);
        // SSPC_DIAG_TARGET_WATCH: a dispatch binding the watched image (storage or sampled), with its other images
        static const u64 watch_rt = [] {
            char v[32] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_TARGET_WATCH", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16)
                                                                                       : 0ull;
        }();
        if (watch_rt != 0 && std::ranges::any_of(bound, [](const CachedImage* i) { return i->layout.guest_address == watch_rt; })) {
            std::string line = fmt::format("    dispatch watch: cs {:#x} groups {}x{}x{}", csv.address, d.groups[0],
                                           d.groups[1], d.groups[2]);
            for (const auto& im : csv.plan.images) {
                line += fmt::format(" | T# {:08x} {:08x} storage {}", im.desc.raw[0], im.desc.raw[1], im.storage);
            }
            for (const CachedImage* img : bound) {
                line += fmt::format(" | img{} {:#x} fmt {} {}x{}{}{}", img->id, img->layout.guest_address,
                                    img->layout.pixel_format, img->layout.size.width, img->layout.size.height,
                                    img->layout.props.is_depth ? " depth" : "", img->gpu_written ? " gw" : "");
            }
            histo_.detail.push_back(std::move(line));
        }
    }
    // (a storage image write alone is ordered by the image's own barriers: not an unknown write)
    if (acc_unknown) {
        ++stats_.unknown_by[1];
    }
    NoteAccesses(acc_reads_, acc_writes_, acc_unknown, tag);
    const u64 dispatch_ticks = GS::Ticks() - t0;
    if (GS::g_fine_timing || GS::g_fine_now) {
        stats_.t_dispatch += dispatch_ticks; // a stage timer like the others (sampled: every N-th dispatch's, P4.9)
    }
    stats_.record_ns += GS::TicksToNs(dispatch_ticks);
    if (insp_catalog_ != nullptr) {
        auto& c = insp_cat_[static_cast<size_t>(insp_cls.category)];
        const u64 groups = d.indirect ? 0
                                      : u64{d.groups[0]} * std::max<u32>(d.groups[1], 1) * std::max<u32>(d.groups[2], 1);
        ++c.dispatches;
        c.groups += groups;
        c.cpu_ticks += dispatch_ticks;
        InspProgram& p = insp_programs_[csv.program != nullptr ? csv.program->id.hash.lo : csv.address];
        if (p.dispatches == 0) {
            p.address = csv.address;
            p.cls = insp_cls;
        }
        ++p.dispatches;
        p.groups += groups;
        p.single_group += groups == 1 ? 1 : 0;
        p.indirect += d.indirect ? 1 : 0;
        p.cpu_ticks += dispatch_ticks;
    }
}

void Executor::CaptureSeries(const CachedImage& img, VkFormat shown, u32 w, u32 h) {
    // SSPC_DIAG_CAPTURE_FRAMES=<first flip>:<count>[:<step>[:<downsample>]]: a series of presented frames ->
    // logs\p45\frames\flip_N.png (consecutive frames: flicker, frame-to-frame differences; P4.5). `t<seconds>` as the
    // first: the first flip at or after that time of the frame log (the smoke benchmark: a place on the route, not a
    // flip number that depends on the frame rate)
    static bool by_time = false;
    static std::array<u64, 4> series = [] {
        std::array<u64, 4> v{0, 0, 1, 1};
        char s[64] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_CAPTURE_FRAMES", s, sizeof(s)) > 0) {
            char* p = s;
            if (*p == 't') {
                by_time = true;
                ++p;
            }
            for (u64& x : v) {
                x = std::strtoull(p, &p, 10);
                if (*p != ':') {
                    break;
                }
                ++p;
            }
            v[2] = std::max<u64>(v[2], 1);
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CAPTURE_FRAMES - {} {} + {} flips (step {}) captured to PNG (slower)",
                     by_time ? "from second" : "from flip", v[0], v[1] * v[2], v[2]);
        }
        return v;
    }();
    if (series[1] == 0) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    if (by_time) {
        if (FrameLog::Seconds() < static_cast<double>(series[0])) {
            return;
        }
        by_time = false;
        series[0] = id;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CAPTURE_FRAMES - the series starts at flip {}", id);
    }
    if (id < series[0] || id >= series[0] + series[1] * series[2] || (id - series[0]) % series[2] != 0) {
        return;
    }
    if (!readback_) {
        readback_ = std::make_unique<ImageReadback>(ctx_);
    }
    // the displayed image's texels as the scanout reads them (`shown`), recorded after the frame's work: what it showed
    readback_->Record(cur_.cb, cur_.value, img.image, shown, VK_IMAGE_ASPECT_COLOR_BIT, 0, w, h,
                      std::max<u32>(img.layout.num_bits / 8, 1),
                      options_.capture_path.parent_path().parent_path() / "p45" / "frames" / fmt::format("flip_{:06}", id),
                      false, static_cast<u32>(std::clamp<u64>(series[3], 1, 16)));
}

u64 Executor::RaceHash(const u8* p, u64 n) {
    u64 h = 0xCBF29CE484222325ull;
    for (u64 i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

void Executor::RaceCheck() {
    // SSPC_DIAG_RACE=1: the small ranges this frame copied from guest memory when its draws were recorded, compared at
    // the flip with guest memory now - a range the game rewrote while the backend still recorded the frame (P4.5)
    static const bool on = [] {
        char v[4] = {};
        const bool o = GetEnvironmentVariableA("SSPC_DIAG_RACE", v, sizeof(v)) == 1 && v[0] == '1';
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_RACE=1 - streamed ranges re-read at each flip (slower)");
        }
        return o;
    }();
    race_.on = on;
    static const u64 watch = [] {
        char v[32] = {};
        const u64 w = GetEnvironmentVariableA("SSPC_DIAG_WATCH", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16) : 0ull;
        if (w != 0) {
            char rd[4] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_WATCH_READS", rd, sizeof(rd)) == 1 && rd[0] == '1') {
                g_watch_prot = PAGE_NOACCESS;
            }
            AddVectoredExceptionHandler(1, &WatchFilter);
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_WATCH={:#x} - CPU {} of its page logged per flip", w,
                     g_watch_prot == PAGE_NOACCESS ? "reads and writes" : "writes");
        }
        return w;
    }();
    if (watch != 0) {
        // the writes since the last flip (the page was protected then), the value now, protection again
        const u32 n = std::min<u32>(g_watch_n.exchange(0, std::memory_order_acq_rel), g_watch_hits.size());
        u64 value[2] = {};
        GS::GameMemory().Read(watch, value, 16);
        static const u64 watch_from = [] {
            char v[32] = {};
            GetEnvironmentVariableA("SSPC_DIAG_WATCH_FROM", v, sizeof(v));
            return std::strtoull(v, nullptr, 10);
        }();
        if (race_.watch_logged < 400 && VideoOut::NextFlipId() >= watch_from) {
            ++race_.watch_logged;
            const u64 sub = Gnm::LastSubmitTsc(), now = __rdtsc();
            const auto rel = [&](u64 t) {
                return t == 0 ? 0.0
                       : t >= sub ? static_cast<double>(GS::TicksToNs(t - sub)) / 1e6
                                  : -static_cast<double>(GS::TicksToNs(sub - t)) / 1e6;
            };
            std::string out = fmt::format("watch flip {} value {:#x} {:#x}: {} CPU writes; ms since the last submit: "
                                          "now {:.1f}, copy {:.1f} (after {} writes)",
                                          VideoOut::NextFlipId(), value[0], value[1], n, rel(now),
                                          rel(race_.watch_copy_tsc), race_.watch_copy_hits);
            if (g_watch_prot == PAGE_NOACCESS) {
                // SSPC-020: the four 64-bit timer pairs from the watched address (start, end) as the game computes
                // them (low 32 bits, 800 MHz GPU counter -> ms; the game's timer 0x196ef0 in eboot)
                u64 slots[8] = {};
                GS::GameMemory().Read(watch & ~0xFull, slots, sizeof(slots));
                out += " | pairs ms:";
                for (u32 p = 0; p < 4; ++p) {
                    const u32 d = static_cast<u32>(slots[2 * p + 1]) - static_cast<u32>(slots[2 * p]);
                    out += fmt::format(" {:.3f}", static_cast<double>(d) / 800000.0);
                }
            }
            race_.watch_copy_tsc = 0;
            // the writes' times, coalesced into bursts (writes within 0.2 ms), with their code addresses
            for (u32 i = 0; i < n;) {
                u32 j = i + 1;
                while (j < n && rel(g_watch_hits[j].tsc) - rel(g_watch_hits[j - 1].tsc) < 0.2) {
                    ++j;
                }
                out += fmt::format(" | {} accesses at {:.1f}..{:.1f} rip {:#x} [{:#x}..]", j - i,
                                   rel(g_watch_hits[i].tsc), rel(g_watch_hits[j - 1].tsc), g_watch_hits[i].rip,
                                   g_watch_hits[i].addr);
                if (j - i <= 32) { // a short burst: every address (r read, w write) and its code address
                    out += " at";
                    for (u32 k = i; k < j; ++k) {
                        out += fmt::format(" {}{:x}@{:x}", g_watch_hits[k].write != 0 ? 'w' : 'r',
                                           g_watch_hits[k].addr & 0xFFF, g_watch_hits[k].rip);
                    }
                }
                i = j;
            }
            for (const auto& [t, a] : race_.acquires) {
                out += fmt::format(" | wait met {:#x} at {:.1f}", a, rel(t));
            }
            race_.acquires.clear();
            for (u32 i = 0; i < n && i < 1; ++i) {
                wchar_t* name = nullptr;
                std::string tname;
                if (HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_watch_hits[i].tid)) {
                    if (SUCCEEDED(GetThreadDescription(th, &name)) && name != nullptr) {
                        for (const wchar_t* c = name; *c != 0; ++c) {
                            tname.push_back(static_cast<char>(*c));
                        }
                        LocalFree(name);
                    }
                    CloseHandle(th);
                }
                out += fmt::format(" | tid {} {} rip {:#x} at {:#x}", g_watch_hits[i].tid, tname, g_watch_hits[i].rip,
                                   g_watch_hits[i].addr);
            }
            SSPC_LOG(GPU, Info, "{}", out);
        }
        const u64 page = watch & ~0xFFFull;
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(page), 4096, g_watch_prot, &old)) {
            g_watch_page.store(page, std::memory_order_release);
        }
    }
    if (!on || race_.copies.empty()) {
        return;
    }
    u64 changed = 0;
    std::vector<u8> buf;
    for (const auto& c : race_.copies) {
        buf.resize(c.size);
        const u8* host = guest_.BackingPointer(c.addr, c.size);
        if (host != nullptr) {
            std::memcpy(buf.data(), host, c.size);
        } else if (!GS::GameMemory().Read(c.addr, buf.data(), c.size)) {
            continue;
        }
        if (const u64 now = RaceHash(buf.data(), c.size); now != c.hash) {
            race_.post[c.addr] = now;
            ++changed;
            auto& r = race_.by_range[c.addr];
            ++r.count;
            r.size = c.size;
        }
    }
    race_.total += race_.copies.size();
    race_.changed += changed;
    ++race_.frames;
    if (changed != 0) {
        ++race_.frames_changed;
    }
    race_.copies.clear();
    if (race_.frames % 300 == 0) {
        std::vector<std::pair<u64, RaceRange>> v(race_.by_range.begin(), race_.by_range.end());
        std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
        std::string out = fmt::format("race check over {} frames: {} of {} streamed ranges changed before the flip "
                                      "({} frames with changes; the next frame copied the flip's value {} times, another "
                                      "value {} times); top ranges:",
                                      race_.frames, race_.changed, race_.total, race_.frames_changed, race_.next_equal,
                                      race_.next_differ);
        for (size_t i = 0; i < v.size() && i < 16; ++i) {
            out += fmt::format(" {:#x}+{:#x} x{};", v[i].first, v[i].second.size, v[i].second.count);
        }
        SSPC_LOG(GPU, Info, "{}", out);
    }
}

void Executor::DrawHistogram() {
    // SSPC_DIAG_DRAW_HISTO=<first flip>:<count>: per frame, the draws by pipeline and the dispatches by program ->
    // logs\p45\draw_histo.txt (what is drawn frame to frame: content that toggles - lights, particles; P4.5)
    static const std::array<u64, 2> range = [] {
        std::array<u64, 2> v{0, 0};
        char s[64] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_DRAW_HISTO", s, sizeof(s)) > 0) {
            char* p = s;
            v[0] = std::strtoull(p, &p, 10);
            if (*p == ':') {
                v[1] = std::strtoull(p + 1, nullptr, 10);
            }
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_DRAW_HISTO - draws per pipeline of flips {} .. {}", v[0],
                     v[0] + v[1]);
        }
        return v;
    }();
    if (range[1] == 0) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    if (histo_.active) {
        if (!histo_.out.is_open()) {
            histo_.out.open(options_.capture_path.parent_path().parent_path() / "p45" / "draw_histo.txt",
                            std::ios::trunc);
            histo_.args.open(options_.capture_path.parent_path().parent_path() / "p45" / "draw_args.txt",
                             std::ios::trunc);
        }
        u64 draws = 0, disp = 0;
        for (const auto& [k, e] : histo_.draws) {
            draws += e.count;
        }
        for (const auto& [k, e] : histo_.dispatches) {
            disp += e.count;
        }
        // the frame's timing: since the previous flip marker (the backend's frame) and between the game's submissions
        const u64 now = __rdtsc(), sub = Gnm::LastSubmitTsc();
        const double flip_ms = histo_.last_flip_tsc != 0 ? static_cast<double>(GS::TicksToNs(now - histo_.last_flip_tsc)) / 1e6 : 0.0;
        const double sub_ms = histo_.last_submit_tsc != 0 && sub > histo_.last_submit_tsc
                                  ? static_cast<double>(GS::TicksToNs(sub - histo_.last_submit_tsc)) / 1e6
                                  : 0.0;
        histo_.last_flip_tsc = now;
        histo_.last_submit_tsc = sub;
        histo_.out << fmt::format("flip {} draws {} dispatches {} flip_ms {:.1f} submit_ms {:.1f}\n", id, draws, disp,
                                  flip_ms, sub_ms);
        std::vector<std::pair<u64, const HistoEntry*>> v;
        for (const auto& [k, e] : histo_.draws) {
            v.push_back({k, &e});
        }
        std::ranges::sort(v, [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [k, e] : v) {
            histo_.out << fmt::format("  p {} n {} ind {} inst {} verts {}{}\n", k, e->count, e->indirect, e->instances,
                                      e->verts, histo_.legend_done.insert(k).second ? " | " + e->legend : "");
        }
        v.clear();
        for (const auto& [k, e] : histo_.dispatches) {
            v.push_back({k, &e});
        }
        std::ranges::sort(v, [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [k, e] : v) {
            histo_.out << fmt::format("  cs {:#x} n {} ind {} groups {}\n", k, e->count, e->indirect, e->instances);
        }
        for (const auto& line : histo_.detail) {
            histo_.out << line << '\n';
        }
        histo_.detail.clear();
        histo_.out.flush();
        for (auto& [k, e] : histo_.draws) {
            e.count = e.indirect = e.instances = e.verts = 0;
        }
        histo_.dispatches.clear();
        std::erase_if(histo_.draws, [](const auto& kv) { return kv.second.legend.empty(); });
    }
    histo_.active = id + 1 >= range[0] && id + 1 < range[0] + range[1];
}

void Executor::CaptureImageAt() {
    // SSPC_DIAG_CAPTURE_IMAGE=<hex address>:<flip>[+<flips>][,...]: the GPU-written image at that address as it is at
    // that flip's end (+<flips>: at each of that many flips from it) -> logs\p45\image_<address>_<flip>.raw/.txt (+ .png
    // for display formats); P4.5. SSPC_DIAG_CAPTURE_DIR=<dir>: there instead of logs\p45, with captures.csv - each
    // capture with the native render operations the processor had applied so far (P5.3D A/B: which path drew a flip).
    struct Want {
        u64 addr, flip;
    };
    static const std::vector<Want> wants = [] {
        std::vector<Want> v;
        char s[1024] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_CAPTURE_IMAGE", s, sizeof(s)) > 0) {
            for (char* p = s; *p != 0;) {
                Want w{std::strtoull(p, &p, 16), 0};
                u64 flips = 1;
                if (*p == ':') {
                    w.flip = std::strtoull(p + 1, &p, 10);
                }
                if (*p == '+') {
                    flips = std::clamp<u64>(std::strtoull(p + 1, &p, 10), 1, 4096);
                }
                for (u64 k = 0; k < flips; ++k) {
                    v.push_back(Want{w.addr, w.flip + k});
                }
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CAPTURE_IMAGE - {} image captures", v.size());
        }
        return v;
    }();
    if (wants.empty() || !Begin()) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    for (const Want& w : wants) {
        if (w.flip == id) {
            CaptureTargets(&w.addr, 1, -1, 0, 0);
        }
    }
}

void Executor::CaptureTargets(const u64* addresses, u32 count, s32 pass, u32 native_draws, u32 pm4_draws) {
    static const std::filesystem::path dir = [this] {
        char s[512] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_DIAG_CAPTURE_DIR", s, sizeof(s));
        return n > 0 && n < sizeof(s) ? std::filesystem::path(s)
                                       : options_.capture_path.parent_path().parent_path() / "p45";
    }();
    if (!Begin()) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    for (u32 k = 0; k < count; ++k) {
        const struct {
            u64 addr;
        } w{addresses[k]};
        CachedImage* img = images_.FindTargetAt(w.addr);
        if (img == nullptr) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: no GPU-written image at {:#x} at flip {}", w.addr, id);
            continue;
        }
        images_.Touch(*img, cur_.value);
        EndRendering();
        if (pending_writes_) {
            GlobalBarrier();
        }
        images_.Transition(cur_.cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        if (!readback_) {
            readback_ = std::make_unique<ImageReadback>(ctx_);
        }
        const bool depth = img->layout.props.is_depth;
        const VkFormat fmt = depth ? VK_FORMAT_D32_SFLOAT : img->format;
        const u32 texel = depth ? 4u : std::max<u32>(img->layout.num_bits / 8, 1);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (u32 layer = 0; layer < std::min<u32>(std::max<u32>(img->layout.layers, 1), 8); ++layer) {
            const std::string stem = pass < 0 ? fmt::format("image_{:x}_{}_l{}", w.addr, id, layer)
                                              : fmt::format("p{}_{:x}_{}_l{}", pass, w.addr, id, layer);
            readback_->Record(cur_.cb, cur_.value, img->image, fmt, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : img->aspect,
                              layer, img->layout.size.width, img->layout.size.height, texel, dir / stem, true);
        }
        {
            const Engine::Render::DrawCounters& c = Engine::Render::Counters();
            const bool fresh = !std::filesystem::exists(dir / "captures.csv");
            std::ofstream csv(dir / "captures.csv", std::ios::app);
            if (fresh) {
                csv << "flip,address,format,width,height,native_draws,native_commits,native_state,verified_draws,"
                       "state_verified,pass,pass_native_draws,pass_pm4_draws\n";
            }
            csv << fmt::format("{},{:x},{},{},{},{},{},{},{},{},{},{},{}\n", id, w.addr, static_cast<u32>(fmt),
                               img->layout.size.width, img->layout.size.height, c.applied_draws.load(),
                               c.applied_commits.load(), c.state_applied.load(), c.verified.load(),
                               c.state_verified.load(), pass, native_draws, pm4_draws);
        }
        SSPC_LOG(GPU, Info, "DIAGNOSTIC: image capture img{} {:#x} fmt {} {}x{} layers {} at flip {}", img->id, w.addr,
                 img->layout.pixel_format, img->layout.size.width, img->layout.size.height, img->layout.layers, id);
    }
}

void Executor::DiagCaptureDraw(u32 pipeline, const std::array<CachedImage*, 8>& color_img, bool after) {
    // SSPC_DIAG_CAPTURE_DRAW=<hex address>:<first flip>:<count>:<pipeline>: in each of those flips, the render target at
    // that address before and after the first draw of that pipeline, downsampled 4x -> logs\p45\draw\<before|after>_
    // <flip> (P4.5: what one draw adds, frame after frame - the smoke's own flicker against its background's)
    struct Range {
        u64 addr = 0, first = 0, count = 0;
        u32 pipeline = 0;
    };
    static const Range r = [] {
        Range v;
        char s[128] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_CAPTURE_DRAW", s, sizeof(s)) > 0) {
            char* p = s;
            v.addr = std::strtoull(p, &p, 16);
            v.first = *p == ':' ? std::strtoull(p + 1, &p, 10) : 0;
            v.count = *p == ':' ? std::strtoull(p + 1, &p, 10) : 0;
            v.pipeline = *p == ':' ? static_cast<u32>(std::strtoul(p + 1, &p, 10)) : 0;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CAPTURE_DRAW - {:#x} around pipeline {} in flips {} .. {}",
                     v.addr, v.pipeline, v.first, v.first + v.count);
        }
        return v;
    }();
    static u64 done[2] = {~0ull, ~0ull};
    if (r.count == 0 || pipeline != r.pipeline) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    if (id < r.first || id >= r.first + r.count || done[after ? 1 : 0] == id) {
        return;
    }
    CachedImage* img = nullptr;
    for (CachedImage* c : color_img) {
        if (c != nullptr && c->layout.guest_address == r.addr) {
            img = c;
        }
    }
    if (img == nullptr) {
        return;
    }
    done[after ? 1 : 0] = id;
    EndRendering();
    if (pending_writes_ || after) {
        GlobalBarrier();
    }
    images_.Transition(cur_.cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!readback_) {
        readback_ = std::make_unique<ImageReadback>(ctx_);
    }
    const auto dir = options_.capture_path.parent_path().parent_path() / "p45" / "draw";
    std::filesystem::create_directories(dir);
    readback_->Record(cur_.cb, cur_.value, img->image, img->format, img->aspect, 0, img->layout.size.width,
                      img->layout.size.height, std::max<u32>(img->layout.num_bits / 8, 1),
                      dir / fmt::format("{}_{}", after ? "after" : "before", id), false, 4);
}

void Executor::DiagCaptureBefore(u32 pipeline, const std::array<CachedImage*, 8>& color_img) {
    // SSPC_DIAG_CAPTURE_BEFORE=<hex address>:<flip>:<pipeline>[,...]: the render target at that address as it is before
    // the first draw of that pipeline (rendering into it) in that flip -> logs\p45\before_<address>_<flip>_p<pipeline>
    // (P4.5: which draw changes a channel)
    struct Want {
        u64 addr, flip;
        u32 pipeline;
    };
    static std::vector<Want> wants = [] {
        std::vector<Want> v;
        char s[512] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_CAPTURE_BEFORE", s, sizeof(s)) > 0) {
            for (char* p = s; *p != 0;) {
                Want w{std::strtoull(p, &p, 16), 0, 0};
                if (*p == ':') {
                    w.flip = std::strtoull(p + 1, &p, 10);
                }
                if (*p == ':') {
                    w.pipeline = static_cast<u32>(std::strtoul(p + 1, &p, 10));
                }
                v.push_back(w);
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CAPTURE_BEFORE - {} captures before draws", v.size());
        }
        return v;
    }();
    if (wants.empty()) {
        return;
    }
    const u64 id = VideoOut::NextFlipId();
    for (auto it = wants.begin(); it != wants.end();) {
        CachedImage* img = nullptr;
        for (CachedImage* c : color_img) {
            if (c != nullptr && c->layout.guest_address == it->addr) {
                img = c;
            }
        }
        if (it->flip != id || it->pipeline != pipeline || img == nullptr) {
            ++it;
            continue;
        }
        EndRendering();
        if (pending_writes_) {
            GlobalBarrier();
        }
        images_.Transition(cur_.cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        if (!readback_) {
            readback_ = std::make_unique<ImageReadback>(ctx_);
        }
        readback_->Record(cur_.cb, cur_.value, img->image, img->format, img->aspect, 0, img->layout.size.width,
                          img->layout.size.height, std::max<u32>(img->layout.num_bits / 8, 1),
                          options_.capture_path.parent_path().parent_path() / "p45" /
                              fmt::format("before_{:x}_{}_p{}", it->addr, id, pipeline),
                          true);
        SSPC_LOG(GPU, Info, "DIAGNOSTIC: capture of img{} {:#x} before draw {} (pipeline {}) at flip {}", img->id,
                 it->addr, stats_.draws, pipeline, id);
        it = wants.erase(it);
    }
}

namespace {

/// SSPC_DIAG_PROBE / SSPC_DIAG_PROBE_CS: <hex address or hash>:<first flip>:<flips>:<bindings>[:<max bytes>]
struct ProbeSpec {
    u64 shader = 0, first = 0, flips = 0, max_bytes = 1ull << 20;
    std::vector<s32> bindings; ///< -1: the indirect arguments
};

ProbeSpec ParseProbeSpec(const char* name) {
    ProbeSpec v;
    char s[256] = {};
    if (GetEnvironmentVariableA(name, s, sizeof(s)) == 0) {
        return v;
    }
    char* p = s;
    v.shader = std::strtoull(p, &p, 16);
    v.first = *p == ':' ? std::strtoull(p + 1, &p, 10) : 0;
    v.flips = *p == ':' ? std::strtoull(p + 1, &p, 10) : 0;
    if (*p == ':') {
        ++p;
        while (*p != 0 && *p != ':') {
            if (*p == 'a') {
                v.bindings.push_back(-1);
                ++p;
            } else {
                v.bindings.push_back(static_cast<s32>(std::strtol(p, &p, 10)));
            }
            if (*p == '+') {
                ++p;
            }
        }
    }
    if (*p == ':') {
        v.max_bytes = std::strtoull(p + 1, &p, 10);
    }
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: {} - shader {:#x}, flips {} .. {}, {} bindings read back (up to {} bytes each)",
             name, v.shader, v.first, v.first + v.flips, v.bindings.size(), v.max_bytes);
    return v;
}

} // namespace

void Executor::ProbeCopies(const std::string& prefix, const GS::StageView& stage, const std::vector<s32>& bindings,
                           u64 max_bytes, u64 args_addr, u64 args_size) {
    std::vector<std::pair<u64, u64>> guest; // the stage's guest buffers, as the command trace lists them (R0)
    for (const auto& b : stage.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.address != 0 && b.size != 0) {
            guest.emplace_back(b.address, b.size);
        }
    }
    EndRendering();
    GlobalBarrier(); // always: after a dispatch its writes are not noted yet
    for (const s32 k : bindings) {
        u64 addr = 0, size = 0, off = 0, avail = 0;
        if (k < 0) {
            if (args_addr == 0) {
                continue;
            }
            addr = args_addr;
            size = args_size;
        } else {
            if (static_cast<size_t>(k) >= guest.size()) {
                continue;
            }
            addr = guest[k].first;
            size = std::min(guest[k].second, max_bytes);
        }
        VkBuffer src = VK_NULL_HANDLE;
        if (!GuestRange(addr, size, false, 4, src, off, avail)) {
            continue;
        }
        size = std::min(size, avail);
        OwnedBuffer rb;
        std::string err;
        if (!CreateBuffer(ctx_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, rb, err, true, HostPlacement::Cached)) {
            continue;
        }
        const VkBufferCopy c{.srcOffset = off, .dstOffset = 0, .size = size};
        Fn().vkCmdCopyBuffer(cur_.cb, src, rb.buffer, 1, &c);
        probe_rb_.push_back({cur_.value, size,
                             fmt::format("{}_{}_{:x}", prefix, k < 0 ? std::string("a") : std::to_string(k), addr),
                             std::move(rb)});
    }
    // the copies' reads before any later write of those buffers; their writes visible to the host
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    Fn().vkCmdPipelineBarrier2(cur_.cb, &dep);
}

void Executor::Probe(const Cp::DrawDescriptor& d, const GS::StageView& vs, const GS::StageView* ps) {
    static const ProbeSpec spec = ParseProbeSpec("SSPC_DIAG_PROBE");
    if (spec.flips == 0 || (!StageMatches(vs, spec.shader) && (ps == nullptr || !StageMatches(*ps, spec.shader)))) {
        return;
    }
    const u64 flip = VideoOut::NextFlipId();
    if (flip >= spec.first && flip < spec.first + spec.flips) {
        ProbeCopies(fmt::format("f{}", flip), vs, spec.bindings, spec.max_bytes, d.indirect ? d.args_addr : 0,
                    d.indexed ? 20 : 16);
    }
}

void Executor::ProbeDispatch(const Cp::DispatchDescriptor& d, const GS::StageView& cs, bool after) {
    // SSPC_DIAG_PROBE_CS: the bindings before and after every matching dispatch of those flips -> f<flip>_<ordinal in the
    // frame>_<pre|post>_<binding>_<address>.bin (which pass of a chain changes what)
    static const ProbeSpec spec = ParseProbeSpec("SSPC_DIAG_PROBE_CS");
    if (spec.flips == 0 || !StageMatches(cs, spec.shader)) {
        return;
    }
    const u64 flip = VideoOut::NextFlipId();
    if (flip >= spec.first && flip < spec.first + spec.flips) {
        ProbeCopies(fmt::format("f{}_{}_{}", flip, trace_.ordinal, after ? "post" : "pre"), cs, spec.bindings,
                    spec.max_bytes, d.indirect ? d.indirect_addr : 0, 12);
    }
}

bool Executor::SplitMergeLevels(const Cp::DispatchDescriptor& d, const GS::StageView& cs, const PipelineObjects& pipe,
                                PushData& push, std::vector<VkWriteDescriptorSet>& writes,
                                std::vector<VkDescriptorBufferInfo>& infos) {
    // the particle sort ahead of the smoke draws (docs\SMOKE_SHADER_MAP.md section 4), by its content hash: its first
    // pass merges widths 2..64 in one dispatch. Recorded so (logs\smoke_research s3-s5) its sorted list holds 549-956
    // repeated particles a frame (the smoke flickers, SSPC-020); one width per dispatch (s6) none.
    constexpr u64 kSortProgram = 0x80279384190567beull;
    // SSPC_DIAG_SORT_SPLIT=off: recorded as the game has it. =keep:<w> (diagnostic): the widths below w in one dispatch
    struct Mode {
        bool off = false;
        u64 keep = 0;
    };
    static const Mode mode = [] {
        Mode m;
        char v[32] = {};
        if (GetEnvironmentVariableA("SSPC_DIAG_SORT_SPLIT", v, sizeof(v)) > 0) {
            const std::string_view s(v);
            m.off = s == "off";
            if (s.substr(0, 5) == "keep:") {
                m.keep = std::strtoull(v + 5, nullptr, 10);
            }
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_SORT_SPLIT={} - the particle sort's widths {}", s,
                     m.off ? "merged in one dispatch as the game records them"
                           : fmt::format("below {} in one dispatch, the others one each", m.keep));
        }
        return m;
    }();
    if (mode.off || cs.program == nullptr || cs.program->id.hash.lo != kSortProgram) {
        return false;
    }
    // the program's buffers (its IR): #0 the count, #1 the draw arguments, #2 the pass (w, max), #3 -> #4 the merge
    // (and after them the translator's own: the flattened user data)
    const auto& b = cs.plan.buffers;
    const auto guest = [&](size_t i) { return i < b.size() && b[i].kind == GS::BufferKind::Guest; };
    const bool shape = b.size() >= 5 && infos.size() >= 5 && guest(0) && guest(1) && guest(2) && guest(3) && guest(4) &&
                       !b[2].written && b[2].size >= 8 && b[2].size <= 16 && b[3].written && b[4].written && b[3].size == b[4].size;
    if (!shape) {
        if (split_logged_++ < 4) {
            std::string kinds;
            for (const auto& x : b) {
                kinds += fmt::format(" {}:{:#x}+{:#x}{}", static_cast<u32>(x.kind), x.address, x.size, x.written ? "W" : "");
            }
            SSPC_LOG(GPU, Warning, "particle sort cs {:#x}: not the expected buffers, recorded as it is:{}", cs.address,
                     kinds);
        }
        return false;
    }
    u32 p[4] = {};
    if (!reader_.Read(b[2].address, p, std::min<u64>(b[2].size, 16))) {
        return false;
    }
    const u64 w0 = p[0], max = p[1];
    if (w0 == 0 || w0 * 2 > max) {
        return false; // one width: the dispatch as it is
    }
    std::vector<u64> widths;
    for (u64 w = w0; widths.empty() || w <= max; w *= 2) {
        widths.push_back(w);
    }
    // the dispatches: [first level, last level]. Width w merges two runs of w/2 into one of w (after the first pass the
    // list is in runs of 64: s6 85 order breaks for 5455 particles) - what its threads read the threads of the same w
    // elements wrote at w/2: within one 32-wide subgroup up to w 32, the workgroup's two subgroups at 64 - on the PS4
    // one wave64, in step
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t k = 0; k < widths.size(); ++k) {
        if (k != 0 && widths[k] < mode.keep) {
            runs.back().second = k;
        } else {
            runs.push_back({k, k});
        }
    }
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    std::vector<StreamRing::Slice> slices(runs.size());
    for (size_t r = 0; r < runs.size(); ++r) {
        if (ring_.Allocate(16, align, cur_.value, slices[r]) != StreamRing::Status::Ok) {
            return false;
        }
        const u32 q[4] = {static_cast<u32>(widths[runs[r].first]), static_cast<u32>(widths[runs[r].second]), p[2], p[3]};
        std::memcpy(slices[r].data, q, sizeof(q));
    }
    const auto& f = Fn();
    const u32 base = cs.start.buffer;
    const VkDescriptorBufferInfo i2 = infos[2], i3 = infos[3], i4 = infos[4];
    const PushData saved = push;
    for (size_t r = 0; r < runs.size(); ++r) {
        if (r != 0) {
            GlobalBarrier(8); // the previous width's stores before this width's loads
        }
        infos[2] = {slices[r].buffer, slices[r].offset, 16};
        // the shader's loop merges #3 -> #4 at its first width, #4 -> #3 at the next: a run starting at an odd level
        // has them swapped, so every level reads and writes the lists it does in the one dispatch
        const bool swap = (runs[r].first & 1) != 0;
        infos[3] = swap ? i4 : i3;
        infos[4] = swap ? i3 : i4;
        if (base + 4 < push.buf_offsets.size()) {
            push.buf_offsets[base + 2] = 0;
            push.buf_offsets[base + 3] = swap ? saved.buf_offsets[base + 4] : saved.buf_offsets[base + 3];
            push.buf_offsets[base + 4] = swap ? saved.buf_offsets[base + 3] : saved.buf_offsets[base + 4];
        }
        PushDescriptors(pipe, VK_PIPELINE_BIND_POINT_COMPUTE, writes);
        f.vkCmdPushConstants(cur_.cb, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData), &push);
        f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
    }
    push_last_ = push; // what the command buffer has pushed last
    infos[2] = i2;
    infos[3] = i3;
    infos[4] = i4;
    push = saved;
    stats_.merge_split_dispatches += runs.size();
    if (split_logged_++ < 4) {
        SSPC_LOG(GPU, Info, "particle sort cs {:#x} (widths {}..{}) recorded as {} dispatches", cs.address, w0,
                 widths.back(), runs.size());
    }
    return true;
}

void Executor::CmdTraceInit() {
    char s[512] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_CMD_TRACE", s, sizeof(s)) > 0) {
        char* p = s;
        if (*p == 't') {
            trace_.by_time = true;
            ++p;
        }
        trace_.first = std::strtoull(p, &p, 10);
        trace_.flips = *p == ':' ? std::strtoull(p + 1, &p, 10) : 1;
        trace_.on = trace_.flips != 0;
        trace_.entries.reserve(8192);
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CMD_TRACE - every command of {} flips from {} {} kept and written "
                               "to smoke_research\\cmd_trace.txt",
                 trace_.flips, trace_.by_time ? "second" : "flip", trace_.first);
    }
    const auto filters = [](const char* name, std::vector<CmdFilter>& out) {
        char v[512] = {};
        if (GetEnvironmentVariableA(name, v, sizeof(v)) == 0) {
            return;
        }
        for (char* p = v; *p != 0;) {
            CmdFilter c{0, 0, 0};
            const std::string_view rest(p);
            const auto take = [&](std::string_view prefix, char what) {
                if (c.what == 0 && rest.starts_with(prefix)) {
                    c.what = what;
                    p += prefix.size();
                }
            };
            take("vs:", 'v');
            take("ps:", 'p');
            take("cs:", 'c');
            take("rt:", 'r');
            take("p:", 'P');
            take("n:", 'n');
            if (c.what == 0) {
                SSPC_LOG(GPU, Error, "{}: '{}' is not vs:|ps:|cs:|rt:<hex>, p:<pipeline> or n:<first>-<last>", name, rest);
                return;
            }
            c.value = std::strtoull(p, &p, c.what == 'P' || c.what == 'n' ? 10 : 16);
            c.last = c.value;
            if (c.what == 'n' && *p == '-') {
                c.last = std::strtoull(p + 1, &p, 10);
            }
            out.push_back(c);
            while (*p == ',' || *p == ' ') {
                ++p;
            }
        }
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: {}={} - {} filters; the matching commands are not executed", name,
                 v, out.size());
    };
    filters("SSPC_DIAG_SKIP_DRAWS", skip_draws_);
    filters("SSPC_DIAG_SKIP_DISPATCHES", skip_dispatches_);
}

bool Executor::StageMatches(const GS::StageView& s, u64 value) {
    return value != 0 && (s.address == value || s.footer_hash == value ||
                          (s.program != nullptr && s.program->id.hash.lo == value));
}

Executor::CmdTraceEntry& Executor::CmdTraceAdd(char kind, u32 queue, u64 seq, u64 packet) {
    CmdTraceEntry& e = trace_.entries.emplace_back();
    e.kind = kind;
    e.frame = trace_.frame;
    e.ordinal = trace_.ordinal;
    e.queue = static_cast<u8>(queue);
    e.seq = seq;
    e.packet = packet;
    e.batch = cur_.value;
    e.barriers = stats_.global_barriers;
    e.ranges_begin = static_cast<u32>(trace_.ranges.size());
    e.images_begin = static_cast<u32>(trace_.images.size());
    return e;
}

void Executor::CmdTraceRanges(const GS::ResourceBindingPlan& plan, u8 stage) {
    for (const auto& b : plan.buffers) {
        if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.address != 0 && b.size != 0) {
            trace_.ranges.push_back({b.address, b.size, stage, b.written});
        } else if (b.kind == GS::BufferKind::GdsBuffer) {
            trace_.ranges.push_back({0, gds_.size, stage, b.written}); // address 0: GDS
        }
    }
}

void Executor::CmdTraceFlip() {
    trace_.ordinal = 0;
    if (!trace_.on) {
        return;
    }
    const u64 id = VideoOut::NextFlipId(); // this flip ends the frame recorded so far
    trace_.frame = id + 1;
    if (trace_.by_time && FrameLog::Seconds() >= static_cast<double>(trace_.first)) {
        trace_.by_time = false;
        trace_.first = id + 1;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CMD_TRACE - flips {} .. {}", trace_.first,
                 trace_.first + trace_.flips - 1);
    }
    if (!trace_.by_time && !trace_.written && id + 1 >= trace_.first + trace_.flips && !trace_.entries.empty()) {
        CmdTraceWrite();
    }
}

void Executor::CmdTraceWrite() {
    trace_.written = true;
    const auto dir = options_.capture_path.parent_path().parent_path() / "smoke_research";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "cmd_trace.txt", std::ios::trunc);
    out << "# SSPC_DIAG_CMD_TRACE: commands in recording order. f<flip> #<ordinal in the frame> D|X|S q<guest queue> seq "
           "<queue's work seq> @<PM4 packet> b<batch (timeline value)> gb<global barriers so far>\n"
           "# D: p<pipeline> vs/ps <PGM address>/<footer hash>/<content hash lo> prim count inst [idx] [ind <args>] "
           "rt<i> <addr>:<VkFormat> depth blend0 dc [SKIPPED]\n"
           "# X: cs <address>/<footer>/<content> groups [ind <args>]; S: not executed (why: detail)\n"
           "# | R<stage: 0 VS/CS 1 PS 2 vertex 3 index 4 args>[W] <addr>+<size> (addr 0: GDS) | T <texture addr>:<fmt>[ S] "
           "(S: storage, GPU-written)\n";
    for (const CmdTraceEntry& e : trace_.entries) {
        std::string line = fmt::format("f{} #{} {} q{} seq {} @{:x} b{} gb{}", e.frame, e.ordinal, e.kind, e.queue, e.seq,
                                       e.packet, e.batch, e.barriers);
        if (e.kind == 'S') {
            line += fmt::format(" ({}: {})", SkipName(static_cast<Skip>(e.skip)),
                                e.skip_text < trace_.skip_texts.size() ? trace_.skip_texts[e.skip_text] : "");
        } else if (e.kind == 'D') {
            line += fmt::format(" p{} vs {:x}/{:x}/{:x} ps {:x}/{:x}/{:x} prim {} count {} inst {}{}", e.pipeline,
                                e.address[0], e.footer[0], e.content[0], e.address[1], e.footer[1], e.content[1], e.prim,
                                e.count, e.instances, e.indexed ? " idx" : "");
            if (e.indirect) {
                line += fmt::format(" ind {:x}", e.args);
            }
            for (u32 i = 0; i < 8; ++i) {
                if (e.rt[i] != 0) {
                    line += fmt::format(" rt{} {:x}:{}", i, e.rt[i], e.rt_fmt[i]);
                }
            }
            line += fmt::format(" depth {:x} blend0 {:x} dc {:x}", e.depth, e.blend0, e.depth_control);
        } else {
            line += fmt::format(" cs {:x}/{:x}/{:x} groups {}x{}x{}", e.address[0], e.footer[0], e.content[0], e.groups[0],
                                e.groups[1], e.groups[2]);
            if (e.indirect) {
                line += fmt::format(" ind {:x}", e.args);
            }
        }
        if (e.skip != 0 && e.kind != 'S') {
            line += " SKIPPED";
        }
        for (u32 i = 0; i < e.ranges_count; ++i) {
            const CmdTraceRange& r = trace_.ranges[e.ranges_begin + i];
            line += fmt::format(" | R{}{} {:x}+{:x}", r.stage, r.written ? "W" : "", r.address, r.size);
        }
        for (u32 i = 0; i < e.images_count; ++i) {
            const u64 t = trace_.images[e.images_begin + i];
            line += fmt::format(" | T {:x}:{}{}", t & 0xFF'FFFF'FFFFull, (t >> 40) & 0x7FFFFF, (t >> 63) ? " S" : "");
        }
        out << line << '\n';
    }
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_CMD_TRACE - {} commands written", trace_.entries.size());
    trace_.entries = {};
    trace_.ranges = {};
    trace_.images = {};
    trace_.skip_texts = {};
}

void Executor::EvictGpuImages() {
    // over the image budget after the uploaded images went (P4.5, R7): the GPU-written ones the cache gives back are
    // copied to their guest memory in this batch - through GuestRange's write path, as a DMA copy on the timeline (the
    // buffer cache's copy of the range stays coherent) - and retired; textures there are uploaded again after it
    if (!images_.OverBudget()) {
        return;
    }
    const std::vector<CachedImage*> back = images_.EvictGpuWritten(timeline_.Completed(), 16);
    if (back.empty() || !Begin()) {
        return;
    }
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    for (CachedImage* img : back) {
        const u64 addr = img->layout.guest_address, bytes = img->layout.guest_size;
        VkBuffer buf = VK_NULL_HANDLE;
        u64 off = 0, avail = 0;
        if (!GuestRange(addr, bytes, true, 4, buf, off, avail) || avail < bytes ||
            (img->layout.IsTiled() && off % std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 1) != 0)) {
            continue; // kept (no write path to its memory now, or one the tiler cannot bind; a later call tries again)
        }
        EndRendering(); // GuestRange may have uploaded
        std::string err;
        if (!images_.WriteBackAndRetire(img, cur_.cb, cur_.value, buf, off, err)) {
            SSPC_LOG(GPU, Warning, "native backend: GPU-written image at {:#x} kept: {}", addr, err);
            continue;
        }
        MarkWrites(5); // a later read of the memory waits for the copy
        // the CPU side (the processor's reads, the frontend's descriptor reads) waits for it
        timeline_writes_.push_back({addr, addr + bytes, cur_.value});
        ++tw_version_;
        tw_lo_ = std::min(tw_lo_, addr);
        tw_hi_ = std::max(tw_hi_, addr + bytes);
        images_.Invalidate(addr, bytes); // textures there are uploaded again, after the copy
    }
}

void Executor::Flip(const Cp::FlipMarker&) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    batch_fence_ = true;
    images_.NewFrame();
    EvictGpuImages();
    CaptureImageAt();
    DrawHistogram();
    RaceCheck();
    CmdTraceFlip();
    ++stats_.flips;
    if (prof_ && ++prof_frames_ >= 300) {
        ProfReport();
    }
    write_run_ = false;
    VideoOut::BufferInfo buf;
    s32 index = -1;
    s32 frame = -1;
    const bool has = VideoOut::NextFlipBuffer(buf, index);
    if (has && presenter_ != nullptr && Begin()) {
        EndRendering();
        if (LabelChanges(kLabelOther | 4)) {
            Label(kLabelOther | 4, "F flip: frame copy to the presenter");
        }
        CachedImage* img = images_.FindTargetAt(buf.address);
        if (img != nullptr) {
            images_.Touch(*img, cur_.value); // the flip copy reads it in this batch (review M2)
        }
        std::string err;
        if (img == nullptr) {
            // not rendered by the GPU (a CPU-drawn buffer): from guest memory
            const ImageLayout layout = LayoutFromVideoOut(buf.address, buf.width, buf.height, buf.pitch,
                                                          buf.pixel_format, buf.tiling_mode);
            img = images_.Find(layout, ImageCache::Use::Texture, cur_.cb, cur_.value, err);
        }
        if (img != nullptr) {
            {
                const StallTrace::Scope trace_acquire(StallTrace::Ev::FlipAcquire); // (P4.12)
                frame = presenter_->AcquireFrame(250);
            }
            if (frame >= 0) {
                if (pending_writes_) {
                    GlobalBarrier();
                }
                images_.Transition(cur_.cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                const u32 w = std::min(std::max(buf.width, 1u), img->layout.size.width);
                const u32 h = std::min(std::max(buf.height, 1u), img->layout.size.height);
                // the scanout reads the bytes in the VideoOut buffer's pixel format (the bridge views the image with
                // it): A8R8G8B8 is B,G,R,A in memory, whatever format the colour target was created with
                const VkFormat shown = DisplayFormat(buf.pixel_format, img->format);
                InspMark(Inspector::kGpuFrameCopy);
                const u64 cid = DiagBegin(fmt::format("flip copy {} as fmt {} {}x{} -> frame {}", DescribeImage(*img),
                                                      static_cast<int>(shown), w, h, frame));
                if (!presenter_->RecordCopy(cur_.cb, frame, img->image, shown, w, h)) {
                    presenter_->ReleaseFrame(frame);
                    frame = -1;
                } else {
                    CaptureSeries(*img, shown, w, h);
                }
                DiagEnd(cid);
                ProfMark(2, "flip copy (frame to the presenter)");
            }
        } else if (stats_.flips_blank == 0) {
            SSPC_LOG(GPU, Warning, "native backend: the displayed buffer {:#x} has no image: {}", buf.address, err);
        }
        ++commands_;
    }
    if (frame < 0) {
        ++stats_.flips_blank;
    }
    if (options_.capture_flip != 0 && presenter_ != nullptr && VideoOut::NextFlipId() == options_.capture_flip) {
        presenter_->RequestCapture(options_.capture_flip, options_.capture_path);
    }
    const u64 value = open_ ? cur_.value : timeline_.LastSubmitted();
    ++stats_.flushes_flip;
    Flush();
    if (VideoOut::OnFlipMarker(value, frame) && flip_hook_ != nullptr) {
        flip_hook_(flip_ctx_, value);
        if (insp_catalog_ != nullptr) {
            insp_frame_ends_.push_back({insp_frame_, value}); // (Poll hands its GPU time on once it completed)
        }
        ++insp_frame_; // (FrameLog's flip marker index: the hook's calls)
    }
    frontend_.NewFrame();
}

} // namespace SecondSon::Gpu::Vk
