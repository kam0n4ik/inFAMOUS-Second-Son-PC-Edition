// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

//
// SecondSonPC — the backend's executor (P4.4). See vk_executor.h. The per-draw state (viewports, scissors, depth,
// stencil, bias, binding conventions) follows shadPS4's renderer_vulkan/vk_rasterizer.cpp (GPL-2.0-or-later): the
// translated SPIR-V is the bridge's, so its bindings and push constants are filled the same way.
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_executor_upscale.h"
#include "runtime/gpu/r12/light_stats.h"

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
#include "runtime/gpu/critical_compute.h"
#include "runtime/gpu/diag_request.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/gnm/gnm_host.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/menu_focus.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/r8/cp_profile.h"
#include "runtime/gpu/vulkan/vk_reflex.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_capture.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"
#include "runtime/gpu/vulkan/vk_emit.h"
#include "runtime/gpu/vulkan/vk_formats.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/vulkan/vk_r12_psort_spv.h"
#include "runtime/gpu/vulkan/vk_resources.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/media/videodec.h"

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

/// TexMemo's binding flags and key (BindStage; a run member's own textures - DrawRunMembers)
u32 TexFlags(const GS::ImageBinding& im) {
    return (im.depth ? 1u : 0u) | (im.array ? 2u : 0u) | (im.storage ? 4u : 0u) | (im.view_type << 3);
}
u64 TexMemoKey(const std::array<u32, 8>& raw, u32 flags) {
    u64 key = flags;
    for (const u32 w : raw) {
        key = (key ^ w) * 0x100000001B3ull;
    }
    return key;
}

/// The layout a draw reads a texture in (RecordDraw's barriers move it there outside rendering)
VkImageLayout TextureLayout(const CachedImage* img) {
    const bool storage =
        (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written && img->current == VK_IMAGE_LAYOUT_GENERAL;
    return storage ? VK_IMAGE_LAYOUT_GENERAL
           : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/// SSPC_CITY_GB_RUN_MATERIAL=verify: what a graphics binding refers to, a hash of its guest description (never 0)
u64 HashWords(u64 h, const u32* w, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ w[i]) * 0x100000001B3ull;
    }
    return h | 1;
}
/// ... the registers a stage reads through its flattened user data (a VS: not its vertex tables' or offsets' - the run's
/// draws are recorded with the first one's there, CITY-OPT Phase 5)
u32 FlatRegisters(const GS::StageView& st) {
    const GS::ShaderReflection& refl = st.module->result.reflection;
    const GS::FetchShader& fetch = refl.fetch;
    u32 skip = 0;
    for (const GS::FetchAttribute& at : fetch.attributes) {
        skip |= at.sgpr_base + 1 < 16 ? 3u << at.sgpr_base : 0u;
    }
    if (fetch.sgpr_base < 16) {
        skip &= ~(3u << fetch.sgpr_base); // (the fetch shader's own address: the same in every draw of a run)
    }
    skip |= fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16 ? 1u << fetch.vertex_offset_sgpr : 0u;
    skip |= fetch.instance_offset_sgpr >= 0 && fetch.instance_offset_sgpr < 16 ? 1u << fetch.instance_offset_sgpr : 0u;
    return refl.user_data_used & ~skip;
}
u64 FlatHash(const u32* flat, size_t n, u32 registers) {
    u64 h = 0xF1A7;
    for (u32 i = 0; i < 16 && i < n; ++i) {
        h = (h ^ ((registers >> i) & 1 ? flat[i] : 0u)) * 0x100000001B3ull;
    }
    return n > 16 ? HashWords(h, flat + 16, n - 16) : h | 1;
}
u64 BufferHash(const GS::BufferBinding& b) {
    const u32 w[] = {static_cast<u32>(b.kind), static_cast<u32>(b.address), static_cast<u32>(b.address >> 32),
                     static_cast<u32>(b.size), static_cast<u32>(b.size >> 32), b.desc.raw[0], b.desc.raw[1],
                     b.desc.raw[2], b.desc.raw[3], (b.written ? 1u : 0u) | (b.formatted ? 2u : 0u),
                     static_cast<u32>(b.status)};
    return HashWords(0xB0F, w, std::size(w));
}
u64 ImageHash(const GS::ImageBinding& im) {
    const u32 w[] = {TexFlags(im), im.count, (im.fmask ? 1u : 0u) | (im.atomic ? 2u : 0u), static_cast<u32>(im.status)};
    const u64 h = HashWords(0x1A6, w, std::size(w));
    return im.status != GS::ResourceStatus::Ok ? h : HashWords(h, im.desc.raw.data(), 8); // (a null one: no T#)
}
u64 SamplerHash(const GS::SamplerBinding& sm) {
    const GS::SamplerDesc d = sm.status != GS::ResourceStatus::Ok ? GS::SamplerDesc{} : sm.desc;
    return HashWords(0x5A3, d.raw.data(), 4);
}
/// ... every binding of a stage's plan into `out` (by binding; a stage without a module is not bound - RecordDraw's
/// has_ps: a draw without a pixel shader)
void StageHashes(const GS::StageView& st, std::vector<u64>& out) {
    if (st.module == nullptr) {
        return;
    }
    const auto put = [&](u32 binding, u64 h) {
        if (binding >= out.size()) {
            out.resize(binding + 1, 0);
        }
        out[binding] = h;
    };
    const u32 registers = FlatRegisters(st);
    for (const GS::BufferBinding& b : st.plan.buffers) {
        put(b.binding, b.kind == GS::BufferKind::Flatbuf ? FlatHash(st.plan.flat.data(), st.plan.flat.size(), registers)
                                                         : BufferHash(b));
    }
    for (const GS::ImageBinding& im : st.plan.images) {
        put(im.binding, ImageHash(im));
    }
    for (const GS::SamplerBinding& sm : st.plan.samplers) {
        put(sm.binding, SamplerHash(sm));
    }
}

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
std::atomic<u64> g_watch_target{0}; ///< SSPC_UPSCALE_SCAN_JITTER: the watch moved to what the scan found
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
    const u64 target = g_watch_target.load(std::memory_order_acquire);
    const bool counted = target == 0 || (a + 0x40 >= target && a < target + 0x40); // (a retargeted watch: its bytes only)
    const u32 n = counted ? g_watch_n.fetch_add(1, std::memory_order_acq_rel) : ~0u;
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
    // (FB-7: a kept-back write-back bound its own compute pipeline and pushed its constants inside the batch)
    buffers_.SetAfterDispatch([](void* self) { static_cast<Executor*>(self)->push_layout_ = VK_NULL_HANDLE; }, this);
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
            // the batch end split: its barrier and copies / the changed-words shader (key 5)
            buffers_.SetWriteBackMark([this] { ProfMark(6, "batch end: barrier and write-back copies"); });
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
    // R11 t2 (docs/R11_T2.md, off by default): SSPC_R11_EARLY_SUBMIT=<min commands>[:<backlog>] - the open main batch
    // submitted between two records once it holds `min` commands and the GPU has at most `backlog` main batches not
    // completed; SSPC_R11_ASYNC_SUBMIT=1 (vk_emit.h) - the batches' vkQueueSubmit2 made by the emitter's submitter
    if (char v[32] = {}; GetEnvironmentVariableA("SSPC_R11_EARLY_SUBMIT", v, sizeof(v)) > 0) {
        char* end = nullptr;
        const unsigned long m = std::strtoul(v, &end, 10);
        if (m >= 8) {
            r11_early_min_ = static_cast<u32>(std::min<unsigned long>(m, options_.max_draws_per_batch));
            r11_early_backlog_ = end != nullptr && *end == ':' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 10)) : 0;
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R11_EARLY_SUBMIT={} - a main batch is submitted before it is full "
                                   "once it holds {} commands and the GPU has at most {} main batches not completed",
                     v, r11_early_min_, r11_early_backlog_);
        }
    }
    r11_async_ = Emit::AsyncSubmit();
    // FrameBreak AQ: the async compute lane (off by default)
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_FB_ASYNC_COMPUTE", v, sizeof(v)) == 1 && v[0] == '1') {
        if (ctx_.LaneQueue() == VK_NULL_HANDLE || !timeline_.EnableLane()) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_FB_ASYNC_COMPUTE=1 - no queue / semaphore for the lane: off");
        } else if (prof_ || clock_at_place_) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_FB_ASYNC_COMPUTE=1 - off with SSPC_GPU_PROFILE / SSPC_DIAG_CLOCK=place");
        } else {
            aq_ = true;
            aq_family_ = ctx_.LaneFamily() != ctx_.QueueFamily();
            if (char am[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_AQ_MARKERS", am, sizeof(am)) == 1 && am[0] == '1' &&
                                 ctx_.Caps().buffer_markers && Fn().vkCmdWriteBufferMarkerAMD != nullptr) {
                std::string merr;
                if (CreateBuffer(ctx_, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, aq_marker_buf_, merr) &&
                    aq_marker_buf_.mapped != nullptr) {
                    std::memset(aq_marker_buf_.mapped, 0, 256);
                    aq_marker_ring_.resize(1u << 16);
                    aq_markers_ = true;
                    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_AQ_MARKERS=1 - buffer markers around every dispatch, by "
                                           "queue, for the device-lost report");
                    char wv[32] = {};
                    std::string perr;
                    if (GetEnvironmentVariableA("SSPC_DIAG_AQ_WATCH", wv, sizeof(wv)) > 0 &&
                        CreateBuffer(ctx_, u64{16} << 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, aq_probe_buf_, perr,
                                     true) &&
                        aq_probe_buf_.mapped != nullptr) {
                        aq_watch_cs_ = std::strtoull(wv, nullptr, 16);
                        aq_writers_.resize(4096);
                        char pv[4] = {};
                        aq_watch_probe_ = !(GetEnvironmentVariableA("SSPC_DIAG_AQ_WATCH_PROBE", pv, sizeof(pv)) == 1 &&
                                            pv[0] == '0');
                        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_AQ_WATCH={:#x} - that shader's first buffer, CPU "
                                               "and GPU bytes, and its writers", aq_watch_cs_);
                    }
                }
            }
            // SSPC_FB_ASYNC_NOTE_UPLOADS=0 (A/B only): the uploads' pages past the binding's range not noted
            if (char nu[4] = {}; GetEnvironmentVariableA("SSPC_FB_ASYNC_NOTE_UPLOADS", nu, sizeof(nu)) == 1 && nu[0] == '0') {
                aq_note_uploads_ = false;
                SSPC_LOG(GPU, Warning, "A/B: SSPC_FB_ASYNC_NOTE_UPLOADS=0 - the block uploads' other pages are not noted "
                                       "for the lane's conflicts (the device-lost behaviour)");
            }
            if (char nr[4] = {}; GetEnvironmentVariableA("SSPC_FB_ASYNC_NOTE_READS", nr, sizeof(nr)) == 1 && nr[0] == '1') {
                aq_note_reads_ = true;
                SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_FB_ASYNC_NOTE_READS=1 - the main queue's graphics reads noted for "
                                       "the lane's conflicts");
            }
            if (char mf[4] = {}; GetEnvironmentVariableA("SSPC_R10_AQ_MAIN_FREE", mf, sizeof(mf)) == 1 && mf[0] == '1') {
                r10_main_free_ = true;
                timeline_.SetMainFree(true);
                SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R10_AQ_MAIN_FREE=1 - a main batch waits only for the lane batches "
                                       "it conflicts with or a guest wait was met by, not for every lane batch before it");
            }
            if (char ai[4] = {}; GetEnvironmentVariableA("SSPC_R11_AQ_IMAGES", ai, sizeof(ai)) == 1 && ai[0] == '1') {
                if (aq_family_) {
                    r11_aq_images_ = true; // (R11 neon, vk_executor.h)
                    SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R11_AQ_IMAGES=1 - compute dispatches that only sample clean "
                                           "uploaded textures already in their layout may run on the async compute lane");
                } else {
                    SSPC_LOG(GPU, Warning, "SSPC_R11_AQ_IMAGES=1 needs the lane on the compute family: off");
                }
            }
            if (char oe[4] = {}; GetEnvironmentVariableA("SSPC_R11_AQ_ORDER_EPOCH", oe, sizeof(oe)) == 1 && oe[0] == '1') {
                if (r10_main_free_) {
                    r11_order_epoch_ = true; // (R11 neon, vk_executor.h)
                    SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R11_AQ_ORDER_EPOCH=1 - a main batch does not wait for a lane "
                                           "batch of its guest queue in the same guest epoch (dispatches only)");
                } else {
                    SSPC_LOG(GPU, Warning, "SSPC_R11_AQ_ORDER_EPOCH=1 needs SSPC_R10_AQ_MAIN_FREE=1: off");
                }
            }
            buffers_.DisableLazy(); // (FB-7's kept-back write-backs stay on the main queue: not with the lane)
            char sp[16] = {};
            if (GetEnvironmentVariableA("SSPC_FB_ASYNC_SPLIT", sp, sizeof(sp)) > 0) {
                const std::string_view m = sp;
                aq_split_ = m == "off" ? 0 : m == "always" ? 2 : 1;
            }
            SSPC_LOG(GPU, Warning,
                     "EXPERIMENT: SSPC_FB_ASYNC_COMPUTE=1 - compute-only batches on a second queue beside the main "
                     "queue's (waiting for a main batch only where they conflict); batches split between graphics and "
                     "compute: {}; the lane on the {} family",
                     aq_split_ == 0 ? "never" : aq_split_ == 1 ? "while the game waits" : "always",
                     aq_family_ ? "compute" : "main");
            R11DiagInit(); // (R11 neon: SSPC_R11_DIAG_AQ, vk_executor_r11.cpp)
        }
    }
}

Executor::~Executor() {
    if (trace_.on && !trace_.written && !trace_.entries.empty()) {
        CmdTraceWrite(); // the run ended inside the traced flips
    }
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    for (auto& r : focus_reads_) {
        if (r.buffer.buffer != VK_NULL_HANDLE) {
            DestroyBuffer(ctx_, r.buffer);
        }
    }
    if (queries_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, queries_, nullptr);
    }
    if (occ_pool_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, occ_pool_, nullptr);
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
    DestroyBuffer(ctx_, lds_lane_);
    DestroyBuffer(ctx_, aq_marker_buf_);
    DestroyBuffer(ctx_, aq_probe_buf_);
    DestroyBuffer(ctx_, diag_scratch_);
    for (OwnedBuffer& a : geo_arena_) {
        DestroyBuffer(ctx_, a);
    }
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
    // (AQ on the compute family: a compute command opens a lane batch - its pool is the compute family's)
    const u8 want_lane = aq_ && aq_family_ && aq_cmd_compute_ ? 1 : 0;
    auto reuse = free_.end();
    for (auto it = free_.rbegin(); it != free_.rend(); ++it) {
        if (it->pool_lane == want_lane) {
            reuse = std::prev(it.base());
            break;
        }
    }
    if (reuse != free_.end()) {
        cur_ = std::move(*reuse);
        free_.erase(reuse);
        // (R11 SSPC_R11_ASYNC_SUBMIT: a completed batch was submitted, so the emitter passed all of its commands long
        // ago - its pools reset at once, not after the ring's newer entries as the Ordered thunks would)
        const auto& reset = r11_async_ ? Emit::Real() : f;
        reset.vkResetCommandPool(ctx_.Device(), cur_.pool, 0);
        if (cur_.descriptors != VK_NULL_HANDLE) {
            reset.vkResetDescriptorPool(ctx_.Device(), cur_.descriptors, 0);
        }
    } else {
        cur_ = Batch{};
        cur_.pool_lane = want_lane;
        const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                          .queueFamilyIndex = want_lane != 0 ? ctx_.LaneFamily() : ctx_.QueueFamily()};
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
    cur_.lane = cur_.pool_lane;
    cur_.aq_wait_main_min = std::exchange(aq_next_wait_main_, 0);
    aq_mark_draw_key_ = ~0ull;
    cur_.aq_main = cur_.aq_wait_all = cur_.aq_images = cur_.aq_gds = cur_.aq_overflow = false;
    cur_.aq_lds = 0;
    cur_.aq_ranges.clear();
    cur_.r11_gds.clear(); // (R11 diagnostics)
    cur_.r11_ovf = 0;
    cur_.r11_epoch.clear();
    cur_.r11_nondisp = 0;
    cur_.done = false;
    cur_.aq_unreported.reset();
    cur_.value = timeline_.Next();
    if (r11_aq_images_) {
        images_.SetRecording(cur_.value, cur_.pool_lane != 0); // (R11: the images' barriers' batch)
    }
    cur_.trace_frame = StallTrace::CurrentFrame(); // (P4.12)
    cur_.open_t = StallTrace::On() ? StallTrace::Now() : 0; // (R10: AqSubmit's start)
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
    r11_early_next_ = r11_early_min_; // (R11 SSPC_R11_EARLY_SUBMIT)
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
    if (!r11_note_dispatch_) {
        cur_.r11_nondisp |= 1u << queue;
    }
    r11_note_dispatch_ = false;
    if (r11_diag_ != nullptr || r11_order_epoch_) {
        // (R11 diagnostics / SSPC_R11_AQ_ORDER_EPOCH: the queue's guest epoch at the batch's last command of it)
        auto it = std::find_if(cur_.r11_epoch.begin(), cur_.r11_epoch.end(), [&](const auto& e) { return e.first == queue; });
        if (it == cur_.r11_epoch.end()) {
            cur_.r11_epoch.emplace_back(queue, guest_epoch_[queue]);
        } else {
            it->second = guest_epoch_[queue];
        }
    }
    if (cur_.work[queue] == 0) {
        cur_.queues.push_back(queue);
    }
    cur_.work[queue] = std::max(cur_.work[queue], seq);
    aq_q_last_[queue] = {cur_.value, cur_.pool_lane};
    if (prof_queues_ && queue != prof_queue_) {
        ProfQueue(queue); // (before the command's barriers and uploads: they count to its queue)
    }
}

void Executor::AqBoundary(u32 queue, bool main_only) {
    aq_cmd_compute_ = queue != Cp::kGraphicsQueue && !main_only;
    if (!aq_ || !open_) {
        return;
    }
    if (aq_family_ && cur_.pool_lane != 0 && !aq_cmd_compute_) {
        ++stats_.aq_splits; // (a lane batch's pool cannot record it: the batch ends here, whatever the split mode)
        Flush();
        return;
    }
    if (aq_split_ == 0 || commands_ == 0) {
        return;
    }
    const bool main_kind = cur_.work[Cp::kGraphicsQueue] != 0 || cur_.aq_main || (aq_family_ && cur_.pool_lane == 0);
    if (main_kind == !aq_cmd_compute_) {
        return; // the same kind
    }
    if (aq_split_ == 1 && !CriticalCompute::Waiting()) {
        return;
    }
    ++stats_.aq_splits;
    Flush();
}

void Executor::AqNote(u64 addr, u64 size) {
    auto& v = cur_.aq_ranges;
    const u64 end = addr + size;
    if (!v.empty() && v.back().first <= end && addr <= v.back().second) {
        v.back().first = std::min(v.back().first, addr);
        v.back().second = std::max(v.back().second, end);
        return;
    }
    if (v.size() >= 4096) {
        cur_.aq_overflow = true;
        cur_.r11_ovf |= 1; // (R11 diagnostics)
        return;
    }
    v.emplace_back(addr, end);
}

bool Executor::AqConflict(const Batch& main, const Batch& lane) {
    if (main.aq_overflow || (main.aq_gds && lane.aq_gds) || (main.aq_lds & lane.aq_lds) != 0) {
        return true;
    }
    for (const auto& [lb, le] : lane.aq_ranges) {
        for (const auto& [mb, me] : main.aq_ranges) {
            if (lb < me && mb < le) {
                return true;
            }
        }
    }
    return false;
}

bool Executor::R11LaneImagesOk(const GS::StageView& cs, u64& wait_main) {
    // R11 SSPC_R11_AQ_IMAGES (vk_executor.h): each image the texture memo resolves to the image BindStage takes without
    // Find, sampled in the layout it is in, no write since its barrier - binding it records nothing
    constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                       VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    images_.ApplyPendingInvalidations(); // (as BindStage before its memo's checks: nothing changes between)
    wait_main = 0;
    const auto refuse = [&](u32 why, const CachedImage* m) {
        ++r11_img_why_[why];
        if (r11_img_logged_ < 12) {
            ++r11_img_logged_;
            SSPC_LOG(GPU, Info, "R11 AQ images: cs {:#x} not on the lane ({}): img{} {:#x} fmt {} uploaded {} dirty {} "
                                "gpu_written {} layout {} access {:#x} barrier v{} lane {}", cs.address, why,
                     m != nullptr ? m->id : 0, m != nullptr ? m->layout.guest_address : 0,
                     m != nullptr ? m->layout.pixel_format : 0, m != nullptr && m->uploaded, m != nullptr && m->dirty,
                     m != nullptr && m->gpu_written, m != nullptr ? static_cast<int>(m->current) : -1,
                     m != nullptr ? m->access : 0, m != nullptr ? m->barrier_value : 0, m != nullptr && m->barrier_lane);
        }
        return false;
    };
    for (const auto& im : cs.plan.images) {
        if (im.status != GS::ResourceStatus::Ok || im.storage || im.fmask || im.atomic || std::max<u32>(im.count, 1) != 1) {
            return refuse(0, nullptr);
        }
        const u32 flags = TexFlags(im);
        const auto it = tex_memo_.find(TexMemoKey(im.desc.raw, flags));
        if (it == tex_memo_.end()) {
            return refuse(1, nullptr);
        }
        const TexMemo& memo = it->second;
        const CachedImage* m = memo.img;
        if (m == nullptr || memo.epoch != images_.Epoch() || memo.raw != im.desc.raw || memo.flags != flags) {
            return refuse(2, m);
        }
        // (BindStage's memo hit; a GPU-written one - rendered by the main queue - too: its last barrier, before which
        // the rendering is, is a main batch's the lane batch waits for)
        if (!((m->gpu_written && !m->memory_newer) || (m->uploaded && !m->dirty && !m->gpu_written))) {
            return refuse(3, m);
        }
        // the layout RecordDispatch's transition takes it to (TextureLayout's rule) - already there, read since
        const bool storage = (m->usage & VK_IMAGE_USAGE_STORAGE_BIT) && m->gpu_written;
        const VkImageLayout want = storage                    ? VK_IMAGE_LAYOUT_GENERAL
                                   : m->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (m->current != want || (m->access & kWrites) != 0) {
            return refuse(4, m);
        }
        if (m->barrier_lane || m->barrier_value == 0) {
            return refuse(5, m);
        }
        wait_main = std::max(wait_main, m->barrier_value);
    }
    return !cs.plan.images.empty();
}

void Executor::EndRendering() {
    if (rendering_) {
        Fn().vkCmdEndRendering(cur_.cb);
        rendering_ = false;
        render_ = RenderKey{};
    }
}

void Executor::SegSync() {
    const u64 g = images_.Generation();
    if (g != seg_gen_ || cur_.value != seg_value_) {
        ++tex_seg_;
        ++stats_.tex_seg_segments;
        seg_gen_ = g;
        seg_value_ = cur_.value;
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

// R12 lights: the tiled lighting compute shader's GPU address (the same in every run since R9, 2026-10-18 [E]; its
// footer hash is logged when SSPC_R12_LIGHT_LIST_KEEP=1 registers its output)
constexpr u64 kR12TiledLightingCs = 0x2edafa400;

void Executor::NoteOrderRange(const void* plan_ptr, u64 shader, u32 queue, bool dispatch) {
    if (const u64 watched = R12::ReadersOf(); watched != 0 && R12::LightCensusOn()) {
        // R12 diagnostics (SSPC_R12_READERS_OF=<hex address>): each shader binding a buffer over that address, once
        for (const auto& b : static_cast<const GS::ResourceBindingPlan*>(plan_ptr)->buffers) {
            if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.address <= watched &&
                watched < b.address + b.size) {
                if (b.size >= (256ull << 20)) {
                    // (a heap-size binding over it - the host import: counted, the first one logged)
                    static std::atomic<u64> heap_binds{0};
                    if (heap_binds.fetch_add(1, std::memory_order_relaxed) == 0) {
                        R12::NoteDetail(fmt::format("first heap-size binding over {:#x}: {} {:#x} [{:#x}, +{:#x})",
                                                    watched, dispatch ? "dispatch" : "draw", shader, b.address, b.size));
                    }
                    continue;
                }
                R12::NoteDetail(fmt::format("{} {:#x} q{} {} [{:#x}, +{:#x}) over {:#x}", dispatch ? "dispatch" : "draw",
                                            shader, queue, b.written ? "WRITES" : "reads", b.address, b.size, watched));
            }
        }
    }
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
    // (R11: the recording thread's time in a flush, every cause - the batch end, the emitter's catch-up, the submission)
    struct FlushTime {
        u64& acc;
        u64 t0;
        ~FlushTime() { acc += GS::TicksToNs(GS::Ticks() - t0); }
    } const flush_time{stats_.r11_flush_ns, GS::Ticks()};
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
    // FrameBreak AQ: the lane for a batch of compute queues only; it waits for the main batches in flight it conflicts
    // with (or all of them); a main batch waits for the lane's last batch
    u64 aq_wait_main = 0, aq_wait_lane = 0;
    u16 aq_why = 0; // (R10: the stall trace's AqSubmit flags)
    if (!aq_family_) {
        cur_.lane = 0;
    }
    if (aq_) {
        if (aq_family_ ? cur_.lane != 0
                       : cur_.work[Cp::kGraphicsQueue] == 0 && !cur_.aq_main && !cur_.queues.empty()) {
            cur_.lane = 1;
            const u64 main_done = timeline_.QueryMain();
            aq_wait_main = cur_.aq_wait_main_min > main_done ? cur_.aq_wait_main_min : 0;
            // SSPC_FB_ASYNC_SERIAL=1 (diagnostic): every lane batch after every main batch before it
            static const bool serial = [] {
                char v[4] = {};
                return GetEnvironmentVariableA("SSPC_FB_ASYNC_SERIAL", v, sizeof(v)) == 1 && v[0] == '1';
            }();
            if (serial) {
                aq_wait_main = aq_last_main_ > main_done ? aq_last_main_ : 0;
            }
            if (aq_wait_main != 0) {
                ++stats_.aq_wait_all; // (a wait met by a held fence of a main batch in flight)
                aq_why = StallTrace::kAqWhyAll | StallTrace::kAqWhyRange; // (R10: all|range = this path)
            } else if (cur_.aq_wait_all || cur_.aq_images || cur_.aq_overflow) {
                aq_wait_main = aq_last_main_ > main_done ? aq_last_main_ : 0;
                ++(cur_.aq_wait_all ? stats_.aq_wait_all : cur_.aq_images ? stats_.aq_wait_images : stats_.aq_wait_conflict);
                aq_why = cur_.aq_wait_all ? StallTrace::kAqWhyAll
                         : cur_.aq_images ? StallTrace::kAqWhyImages
                                          : StallTrace::kAqWhyShared;
            } else {
                bool shared = false;
                for (const Batch& b : in_flight_) {
                    if (b.lane == 0 && !b.done && b.value > main_done && AqConflict(b, cur_)) {
                        aq_wait_main = std::max(aq_wait_main, b.value);
                        shared = shared || b.aq_overflow || (b.aq_gds && cur_.aq_gds) || (b.aq_lds & cur_.aq_lds) != 0;
                    }
                }
                ++(aq_wait_main == 0 ? stats_.aq_free : shared ? stats_.aq_wait_shared : stats_.aq_wait_conflict);
                aq_why = aq_wait_main == 0 ? 0 : shared ? StallTrace::kAqWhyShared : StallTrace::kAqWhyRange;
            }
            if (r11_aq_images_ && cur_.aq_wait_main_min > main_done) {
                // R11: a wait for one main batch (a held fence's, its textures' last barrier) covers the ones before it
                // only - the later ones it conflicts with too
                for (const Batch& b : in_flight_) {
                    if (b.lane == 0 && !b.done && b.value > aq_wait_main && AqConflict(b, cur_)) {
                        aq_wait_main = b.value;
                    }
                }
            }
            aq_why |= StallTrace::kAqLane | (aq_wait_main != 0 ? StallTrace::kAqWaitMain : 0);
            if (r11_diag_ != nullptr) {
                R11DiagLane(main_done);
            }
        } else {
            stats_.aq_kept_main += cur_.work[Cp::kGraphicsQueue] == 0 && !cur_.queues.empty() ? 1 : 0;
            const u64 lane_done = timeline_.QueryLane();
            // (R11 SSPC_R11_AQ_IMAGES: a barrier in it over an image a lane batch in flight sampled)
            const u64 r11_img_wait = r11_aq_images_ ? images_.TakeLaneWait() : 0;
            if (!r10_main_free_) {
                aq_wait_lane = aq_last_lane_ > lane_done ? aq_last_lane_ : 0;
                aq_why = aq_wait_lane != 0 ? StallTrace::kAqWaitLane : 0;
            } else if (aq_last_lane_ > lane_done) {
                // R10: the lane batches in flight it conflicts with, those with images / unknown accesses, the guest's
                // waits met by a lane fence (the AqSubmit flags: wait lane + range / images / all)
                ++stats_.r10_main_free_batches;
                u64 conflict = 0, images = 0;
                for (const Batch& b : in_flight_) {
                    if (b.lane == 0 || b.done || b.value <= lane_done) {
                        continue;
                    }
                    if (b.aq_images || b.aq_overflow || b.aq_wait_all) {
                        images = std::max(images, b.value);
                    } else if (b.value > conflict && AqConflict(cur_, b)) {
                        conflict = b.value;
                    }
                }
                if (r11_img_wait > lane_done) {
                    images = std::max(images, r11_img_wait);
                    ++r11_img_stats_[2];
                }
                u64 sync = 0, order = 0;
                for (const u32 q : cur_.queues) {
                    sync = std::max(sync, r10_q_sync_[q]);
                    if (r11_order_epoch_ && r10_q_lane_[q] > lane_done && (cur_.r11_nondisp & (1u << q)) == 0) {
                        const auto e = std::find_if(cur_.r11_epoch.begin(), cur_.r11_epoch.end(),
                                                    [&](const auto& x) { return x.first == q; });
                        if (e != cur_.r11_epoch.end() && e->second == r11_q_epoch_[q]) {
                            order = std::max(order, r11_q_prev_[q]); // (R11: the same guest epoch - unordered)
                            ++r11_order_skipped_;
                            continue;
                        }
                        ++r11_order_kept_;
                    }
                    order = std::max(order, r10_q_lane_[q]);
                }
                sync = sync > lane_done ? sync : 0;
                order = order > lane_done ? order : 0;
                aq_wait_lane = std::max({conflict, images, sync, order});
                if (r11_diag_ != nullptr && order != 0) {
                    R11DiagMain(lane_done);
                }
                if (aq_wait_lane == 0) {
                    ++stats_.r10_main_free;
                } else {
                    ++(aq_wait_lane == sync     ? stats_.r10_main_wait_sync
                       : aq_wait_lane == order  ? stats_.r10_main_wait_order
                       : aq_wait_lane == images ? stats_.r10_main_wait_images
                                                : stats_.r10_main_wait_conflict);
                    aq_why = StallTrace::kAqWaitLane | (aq_wait_lane == sync || aq_wait_lane == order ? StallTrace::kAqWhyAll
                                                        : aq_wait_lane == images ? StallTrace::kAqWhyImages
                                                                                 : StallTrace::kAqWhyRange);
                }
            }
        }
    }
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = cur_.lane != 0 ? timeline_.LaneHandle() : timeline_.Handle(),
                                       .value = cur_.value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    g_diag_submission.store(cur_.value, std::memory_order_relaxed);
    // the batch in flight from here (both ways of submitting it)
    const auto submitted = [this] {
        if (diag_ != nullptr) {
            diag_->Submitted(cur_.value, batch_first_cmd_, batch_last_cmd_, cur_.lane);
        }
        submitted_value_.store(cur_.value, std::memory_order_release);
        if (aq_) {
            (cur_.lane != 0 ? aq_last_lane_ : aq_last_main_) = cur_.value;
            if (cur_.lane != 0) {
                for (const u32 q : cur_.queues) {
                    if (r11_order_epoch_) {
                        // (R11: the guest epoch at its last command of the queue; the newest lane batch of an earlier one)
                        const auto e = std::find_if(cur_.r11_epoch.begin(), cur_.r11_epoch.end(),
                                                    [&](const auto& x) { return x.first == q; });
                        const u32 ep = e != cur_.r11_epoch.end() ? e->second : guest_epoch_[q];
                        if ((cur_.r11_nondisp & (1u << q)) != 0 || r10_q_lane_[q] == 0 || ep != r11_q_epoch_[q]) {
                            r11_q_prev_[q] = (cur_.r11_nondisp & (1u << q)) != 0 ? cur_.value : r10_q_lane_[q];
                        }
                        r11_q_epoch_[q] = ep;
                    }
                    r10_q_lane_[q] = cur_.value; // (R10 SSPC_R10_AQ_MAIN_FREE)
                }
            }
            ++(cur_.lane != 0 ? stats_.aq_lane_batches : stats_.aq_main_batches);
            if (r11_diag_ != nullptr && cur_.lane == 0 && cur_.aq_overflow) {
                ++r11_diag_->overflow_batches[(cur_.r11_ovf & 1) != 0 ? 0 : 1];
            }
            if (r11_aq_images_) {
                images_.SetRecording(aq_last_main_, false); // (R11: barriers outside a batch taken as the main queue's)
            }
        }
        in_flight_.push_back(std::move(cur_));
        cur_ = Batch{};
        open_ = false;
        commands_ = 0;
    };
    if (r11_async_ && Emit::CanSubmitLater()) {
        // R11 SSPC_R11_ASYNC_SUBMIT: the submission handed to the submitter thread - made once the emitter passed this
        // batch's commands (its End included), in the order of the jobs; its result checked there (R11RunSubmit). The
        // batch is in flight now: nothing completes it but its semaphore value, which a refused submission never signals.
        static_assert(sizeof(R11SubmitJob) <= 1024 && std::is_trivially_copyable_v<R11SubmitJob>);
        R11SubmitJob job;
        job.self = this;
        job.queue = cur_.lane != 0 ? ctx_.LaneQueue() : ctx_.Queue();
        job.value = cur_.value;
        job.lane = cur_.lane;
        job.flip_id = cur_.lane != 0 ? 0 : VideoOut::NextFlipId(); // (Reflex: the frame being recorded)
        job.open_t = cur_.open_t;
        job.commands = static_cast<u32>(commands_);
        // the video memory the batch's commands need bound first (their own timeline): bound now, before the job
        VkSemaphore bind_sem = VK_NULL_HANDLE;
        u64 bind_value = 0;
        if (buffers_.HasPendingBinds()) {
            std::scoped_lock lock{ctx_.QueueMutex()};
            buffers_.SubmitBinds(bind_sem, bind_value);
        }
        const bool bind_waited = bind_sem != VK_NULL_HANDLE;
        if (aq_ && bind_sem == VK_NULL_HANDLE && buffers_.LastBindValue() != 0) {
            bind_sem = buffers_.BindTimeline(); // (as below: the lane is not ordered behind them by the queue)
            bind_value = buffers_.LastBindValue();
        }
        const auto wait_on = [&](VkSemaphore s, u64 v) {
            job.waits[job.n_wait++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = s,
                                       .value = v,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        };
        if (bind_sem != VK_NULL_HANDLE) {
            wait_on(bind_sem, bind_value);
        }
        if (aq_wait_main != 0) {
            wait_on(timeline_.Handle(), aq_wait_main);
        }
        if (aq_wait_lane != 0) {
            wait_on(timeline_.LaneHandle(), aq_wait_lane);
        }
        job.n_cb = n_cb;
        for (u32 i = 0; i < n_cb; ++i) {
            job.cbis[i] = cbis[i];
        }
        job.signal = signal;
        u16 qmask = 0;
        for (const u32 q : cur_.queues) {
            qmask |= q < 8 ? static_cast<u16>(1u << q) : u16{0x80};
        }
        job.aq_wait = static_cast<u32>(aq_wait_main != 0 ? aq_wait_main : aq_wait_lane);
        job.aq_flags = static_cast<u16>(aq_why | (bind_waited ? StallTrace::kAqBinds : 0) | (qmask << 8));
        if (cur_.lane != 0) {
            timeline_.SubmittedOnLane(cur_.value); // (before its submission: a waiter finds it on the lane)
        }
        auto& st = r11_submit_t_[cur_.value & (r11_submit_t_.size() - 1)];
        st[0].store(0, std::memory_order_relaxed);
        st[1].store(0, std::memory_order_relaxed);
        Emit::SubmitLater(&Executor::R11RunSubmit, nullptr, &job, sizeof(job));
        ++stats_.r11_async;
        submitted();
        return;
    }
    if (r11_async_) {
        Emit::DrainSubmits(); // (this thread submits itself: after every batch handed on - the timeline's order)
    }
    VkResult r;
    const u64 submit_t0 = GS::Ticks();
    u64 locked_t = 0;
    bool contended = false;
    bool bind_waited = false;
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
        bind_waited = bind_sem != VK_NULL_HANDLE;
        if (aq_ && bind_sem == VK_NULL_HANDLE && buffers_.LastBindValue() != 0) {
            // AQ: blocks an earlier batch's binds made resident (on the main queue) may be used here - the lane is not
            // ordered behind them by the queue (measured: device lost with an MMU fault in a shader otherwise)
            bind_sem = buffers_.BindTimeline();
            bind_value = buffers_.LastBindValue();
        }
        VkSemaphoreSubmitInfo waits[3];
        u32 n_wait = 0;
        if (bind_sem != VK_NULL_HANDLE) {
            waits[n_wait++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = bind_sem,
                               .value = bind_value,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        if (aq_wait_main != 0) {
            waits[n_wait++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = timeline_.Handle(),
                               .value = aq_wait_main,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        if (aq_wait_lane != 0) {
            waits[n_wait++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = timeline_.LaneHandle(),
                               .value = aq_wait_lane,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        VkLatencySubmissionPresentIdNV latency{}; // (NVIDIA Reflex: the GPU work of the frame being recorded)
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .pNext = cur_.lane != 0 ? nullptr : Reflex::SubmitChain(VideoOut::NextFlipId(), latency),
                               .waitSemaphoreInfoCount = n_wait,
                               .pWaitSemaphoreInfos = waits,
                               .commandBufferInfoCount = n_cb,
                               .pCommandBufferInfos = cbis,
                               .signalSemaphoreInfoCount = 1,
                               .pSignalSemaphoreInfos = &signal};
        if (cur_.lane != 0) {
            timeline_.SubmittedOnLane(cur_.value); // (before its submission: a waiter finds it on the lane)
        }
        r = ctx_.Failed() ? ctx_.Failure()
                          : f.vkQueueSubmit2(cur_.lane != 0 ? ctx_.LaneQueue() : ctx_.Queue(), 1, &si, VK_NULL_HANDLE);
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
        // R10: the batch from its first command to here, its queue and what it waits for on the other one
        u16 qmask = 0;
        for (const u32 q : cur_.queues) {
            qmask |= q < 8 ? static_cast<u16>(1u << q) : u16{0x80};
        }
        StallTrace::Span(StallTrace::Ev::AqSubmit, cur_.open_t != 0 ? cur_.open_t : locked_t, submit_t1, cur_.value,
                         static_cast<u32>(aq_wait_main != 0 ? aq_wait_main : aq_wait_lane),
                         static_cast<u16>(aq_why | (bind_waited ? StallTrace::kAqBinds : 0) | (qmask << 8)));
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
    submitted();
}

void Executor::R11RunSubmit(void*, const void* data) {
    // (the submitter thread: Emit's SubmitterLoop, once the emitter passed the batch's commands)
    const R11SubmitJob& j = *static_cast<const R11SubmitJob*>(data);
    Executor& e = *j.self;
    VkResult r = static_cast<VkResult>(Emit::RecordingError()); // (a begin/end the driver refused: not submitted)
    const u64 t0 = GS::Ticks();
    u64 locked_t = t0;
    bool contended = false;
    if (r == VK_SUCCESS) {
        std::unique_lock lock{e.ctx_.QueueMutex(), std::try_to_lock};
        if (!lock.owns_lock()) {
            contended = true; // (the present thread holds the queue)
            lock.lock();
        }
        locked_t = GS::Ticks();
        VkLatencySubmissionPresentIdNV latency{}; // (NVIDIA Reflex: the frame recorded when it was handed on)
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .pNext = j.lane != 0 ? nullptr : Reflex::SubmitChain(j.flip_id, latency),
                               .waitSemaphoreInfoCount = j.n_wait,
                               .pWaitSemaphoreInfos = j.waits,
                               .commandBufferInfoCount = j.n_cb,
                               .pCommandBufferInfos = j.cbis,
                               .signalSemaphoreInfoCount = 1,
                               .pSignalSemaphoreInfos = &j.signal};
        r = e.ctx_.Failed() ? e.ctx_.Failure() : Emit::Real().vkQueueSubmit2(j.queue, 1, &si, VK_NULL_HANDLE);
    }
    const u64 t1 = GS::Ticks();
    e.r11_async_submit_ns_.fetch_add(GS::TicksToNs(t1 - t0), std::memory_order_relaxed);
    auto& st = e.r11_submit_t_[j.value & (e.r11_submit_t_.size() - 1)];
    st[0].store(t0, std::memory_order_relaxed);
    st[1].store(t1, std::memory_order_release);
    if (StallTrace::On()) {
        if (contended) {
            StallTrace::Span(StallTrace::Ev::QueueLock, t0, locked_t, j.value);
        }
        StallTrace::Span(StallTrace::Ev::Submit, locked_t, t1, j.value, j.commands);
        StallTrace::Span(StallTrace::Ev::AqSubmit, j.open_t != 0 ? j.open_t : locked_t, t1, j.value, j.aq_wait,
                         j.aq_flags);
    }
    if (r != VK_SUCCESS) {
        s32 expected = 0;
        if (e.r11_async_fail_.compare_exchange_strong(expected, static_cast<s32>(r))) {
            e.r11_async_fail_value_.store(j.value, std::memory_order_release);
            SSPC_LOG(GPU, Error, "native backend: vkQueueSubmit2 failed on the submitter thread (VkResult {}, value {}); "
                                 "no completion will be published (SSPC_R11_ASYNC_SUBMIT)",
                     static_cast<int>(r), j.value);
        }
        e.ctx_.Fail("vkQueueSubmit2 (R11 submitter)", r);
    }
}

void Executor::R11EarlySubmitCheck() {
    r11_early_next_ = commands_ + 16; // (the GPU's value read every 16 commands at most)
    // the main queue's batches only: a batch with graphics work (a compute-only one may go to the lane at its end)
    if (cur_.lane != 0 || cur_.work[Cp::kGraphicsQueue] == 0) {
        return;
    }
    const u64 t0 = GS::Ticks();
    ++stats_.r11_early_checks;
    const u64 done = aq_ ? timeline_.QueryMain() : timeline_.QueryCompleted();
    u32 backlog = 0;
    for (const Batch& b : in_flight_) {
        backlog += b.lane == 0 && !b.done && b.value > done ? 1 : 0;
    }
    stats_.r11_early_check_ns += GS::TicksToNs(GS::Ticks() - t0);
    if (backlog > r11_early_backlog_) {
        return;
    }
    ++stats_.r11_early;
    stats_.r11_early_commands += commands_;
    run_ok_ = false; // (as at a full batch's end)
    Flush();
}

void Executor::Poll() {
    if (r11_async_ && !r11_async_fail_reported_ && r11_async_fail_.load(std::memory_order_acquire) != 0) {
        // R11 SSPC_R11_ASYNC_SUBMIT: the submitter thread's refused submission reported here (the executor's thread)
        r11_async_fail_reported_ = true;
        if (r11_async_fail_.load(std::memory_order_relaxed) == VK_ERROR_DEVICE_LOST) {
            OnDeviceLost("vkQueueSubmit2 (R11 submitter)", r11_async_fail_value_.load(std::memory_order_acquire));
        }
    }
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
    if (aq_) {
        // AQ: a batch completes when its queue's semaphore reached it (a lane batch possibly before main batches before
        // it); a guest queue's completion is reported only once every earlier batch with its work completed
        const u64 main_v = timeline_.QueryMain(), lane_v = timeline_.QueryLane();
        std::bitset<Cp::kNumQueues> blocked;
        for (Batch& b : in_flight_) {
            if (!b.done && (b.value <= completed || (b.lane != 0 ? lane_v : main_v) >= b.value)) {
                Retire(b);
                b.done = true;
                for (const u32 q : b.queues) {
                    b.aq_unreported.set(q);
                }
            }
            for (const u32 q : b.queues) {
                if (!b.done || (b.aq_unreported.test(q) && blocked.test(q))) {
                    blocked.set(q);
                } else if (b.aq_unreported.test(q)) {
                    b.aq_unreported.reset(q);
                    stats_.aq_out_of_order += b.value > completed ? 1 : 0;
                    Complete(q, b.work[q]);
                }
            }
        }
        while (!in_flight_.empty() && in_flight_.front().done && in_flight_.front().aq_unreported.none()) {
            free_.push_back(std::move(in_flight_.front()));
            in_flight_.pop_front();
        }
    }
    while (!aq_ && !in_flight_.empty() && in_flight_.front().value <= completed) {
        Batch b = std::move(in_flight_.front());
        in_flight_.pop_front();
        Retire(b);
        for (const u32 q : b.queues) {
            Complete(q, b.work[q]);
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
    PollCollect(completed);
}

bool Executor::PollIfCompleted() {
    if (Failed() || in_flight_.empty()) {
        return false;
    }
    // (AQ: a lane batch may complete before the oldest - Poll decides; the caller's interval bounds the cost)
    if (!aq_ && timeline_.QueryCompleted() < in_flight_.front().value) {
        return false;
    }
    Poll();
    return true;
}

void Executor::Retire(Batch& b) {
    {
        if (cp_ != nullptr && !defer_completions_ && !b.clock_fences.empty() && b.query_slot != ~0u) {
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
        ++stats_.completed_batches;
        if (b.query_slot != ~0u) {
            u64 ts[2] = {};
            if (Fn().vkGetQueryPoolResults(ctx_.Device(), queries_, b.query_slot * 2, 2, sizeof(ts), ts, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[1] >= ts[0]) {
                stats_.gpu_busy_ns += static_cast<u64>(static_cast<double>(ts[1] - ts[0]) * ctx_.Caps().timestamp_period);
                ++stats_.gpu_batches_timed;
                if (aq_) {
                    // AQ: the lane's GPU time, and how much of it ran beside the main queue's batches (each pair counted
                    // once: when the later of the two completes)
                    const double period = ctx_.Caps().timestamp_period;
                    if (b.lane != 0) {
                        stats_.aq_lane_gpu_ns += static_cast<u64>(static_cast<double>(ts[1] - ts[0]) * period);
                    }
                    for (const AqInterval& o : aq_iv_[b.lane != 0 ? 0 : 1]) {
                        const u64 from = std::max(o.begin, ts[0]), to = std::min(o.end, ts[1]);
                        if (to > from) {
                            stats_.aq_overlap_ns += static_cast<u64>(static_cast<double>(to - from) * period);
                        }
                    }
                    auto& mine = aq_iv_[b.lane != 0 ? 1 : 0];
                    mine.push_back({ts[0], ts[1]});
                    if (mine.size() > 32) {
                        mine.pop_front();
                    }
                }
                if (r11_async_ && b.submit_t1 == 0) { // (R11: submitted by the submitter thread)
                    const auto& st = r11_submit_t_[b.value & (r11_submit_t_.size() - 1)];
                    b.submit_t1 = st[1].load(std::memory_order_acquire);
                    b.submit_t0 = st[0].load(std::memory_order_relaxed);
                }
                StallTrace::GpuBatch(b.value, b.submit_t0, b.submit_t1, ts[0], ts[1], b.trace_frame,
                                     b.lane != 0 ? 1u : 0u); // (P4.12; R10: the lane)
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
    }
}

void Executor::Complete(u32 queue, u64 seq) {
    if (defer_completions_) {
        std::scoped_lock lock{completions_mu_};
        completions_.emplace_back(queue, seq);
        completions_ready_.store(true, std::memory_order_release);
    } else if (cp_ != nullptr) {
        cp_->CompleteWork(queue, seq);
    }
}

void Executor::PollCollect(u64 completed) {
    if (!aq_watch_pending_.empty()) {
        AqWatchCheck(completed);
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
    CollectMenuFocus(completed);
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

void Executor::AqMarkersReport(std::string& out) {
    if (!aq_markers_) {
        return;
    }
    const volatile u32* m = reinterpret_cast<const volatile u32*>(aq_marker_buf_.mapped);
    for (u32 lane = 0; lane < 2; ++lane) {
        const u32 started = m[lane * 2], completed = m[lane * 2 + 1];
        out += fmt::format("\nAQ dispatch markers, {} queue: last started {}, last completed {} (recorded up to {})\n",
                           lane != 0 ? "the async compute lane's" : "the main", started, completed, aq_marker_next_ - 1);
        // the dispatches of this queue from a few before the last completed one to a few past the last started one
        u32 shown = 0;
        const u32 from = completed > 64 ? completed - 64 : 1;
        for (u32 id = from; id < aq_marker_next_ && shown < 96; ++id) {
            const AqMarker& e = aq_marker_ring_[id & (aq_marker_ring_.size() - 1)];
            if (e.lane != lane || (id > started + 8 && id > completed + 8)) {
                continue;
            }
            ++shown;
            const char* state = id <= completed ? "done" : id <= started ? "STARTED" : "-";
            if (e.queue == 0xFFFF) {
                out += fmt::format("  {} {}: batch {} draws: pipeline {} vs {:#x} ps {:#x}\n", id, state, e.batch,
                                   e.groups[0], e.cs, e.ps);
                continue;
            }
            out += fmt::format("  {} {}: batch {} q{} cs {:#x} groups {}x{}x{}{}", id, state, e.batch, e.queue, e.cs,
                               e.groups[0], e.groups[1], e.groups[2], e.indirect ? " indirect" : "");
            if (e.waddr != 0 && aq_probe_buf_.mapped != nullptr) {
                u32 g[4];
                std::memcpy(g, aq_probe_buf_.mapped + u64{id & (aq_marker_ring_.size() - 1)} * 16, 16);
                out += fmt::format(" | watch b0 {:#x}+{} path {} flags {} cpu [{:#x} {:#x} {:#x} {:#x}] gpu [{:#x} {:#x} "
                                   "{:#x} {:#x}]",
                                   e.waddr, e.wsize, e.wpath, e.wflags, e.wcpu[0], e.wcpu[1], e.wcpu[2], e.wcpu[3], g[0],
                                   g[1], g[2], g[3]);
            }
            out += '\n';
        }
    }
    if (aq_watch_cs_ != 0) {
        out += fmt::format("\nAQ watch {:#x}: {} probes checked, {} differ from the CPU's bytes, {} with a count past the "
                           "threads, {} huge; {} watched ranges; the last writers of them ({} recorded):\n",
                           aq_watch_cs_, aq_watch_stats_[0], aq_watch_stats_[1], aq_watch_stats_[2], aq_watch_stats_[3],
                           aq_watch_ranges_.size(), aq_writer_next_);
        for (u32 k = aq_writer_next_ > 64 ? aq_writer_next_ - 64 : 0; k < aq_writer_next_; ++k) {
            const AqWriter& w = aq_writers_[k & (aq_writers_.size() - 1)];
            out += fmt::format("  batch {}{} kind {} what {:#x} at {:#x}+{}\n", w.batch, w.lane != 0 ? " (lane)" : "",
                               w.kind, w.what, w.addr, w.size);
        }
    }
}

void Executor::AqWatchWrite(u64 addr, u64 size, u8 kind) {
    // SSPC_DIAG_AQ_WATCH: a command writing (1), uploading (2) or WRITE_DATA (3) over a watched buffer's range
    const u64 end = addr + size;
    if (end <= aq_watch_lo_ || aq_watch_hi_ <= addr) {
        return;
    }
    for (const auto& [b, e] : aq_watch_ranges_) {
        if (addr < e && b < end) {
            aq_writers_[aq_writer_next_++ & (aq_writers_.size() - 1)] = {cur_.value, addr, size, aq_what_,
                                                                         cur_.pool_lane, kind};
            // each kind of writer once in the log (the command, how it writes, the queue)
            const u64 key = aq_what_ * 31 + kind * 2 + (cur_.pool_lane != 0 ? 1 : 0);
            if (aq_writer_kinds_.size() < 64 && aq_writer_kinds_.insert(key).second) {
                SSPC_LOG(GPU, Warning,
                         "DIAGNOSTIC AQ watch: writer of a watched range: batch {}{} kind {} what {:#x} at {:#x}+{} "
                         "(watched {:#x}+{})",
                         cur_.value, cur_.pool_lane != 0 ? " (lane)" : "", kind, aq_what_, addr, size, b, e - b);
            }
            return;
        }
    }
}

void Executor::AqWatchDispatch(u32 id) {
    // SSPC_DIAG_AQ_WATCH: the watched dispatch's first buffer - the CPU's bytes now, the GPU's right before the dispatch
    // (copied into the probe's slot after a barrier; 0xCD until the copy ran)
    const u32 slot = id & (static_cast<u32>(aq_marker_ring_.size()) - 1);
    AqMarker& e = aq_marker_ring_[slot];
    const AqWatchB0& b = aq_watch_b0_;
    e.waddr = b.addr;
    e.wsize = static_cast<u32>(std::min<u64>(b.size, 0xFFFFFFFFull));
    e.wpath = b.path;
    u8 cpu[16] = {};
    if (GuestMemory* gm = GetGuestMemory(); gm != nullptr) {
        gm->CopySparse(b.addr, cpu, std::min<u64>(b.size, 16));
    }
    std::memcpy(e.wcpu, cpu, 16);
    e.wflags = static_cast<u8>((buffers_.HostWritePending(b.addr, 16) ? 1 : 0) | (TimelineWritePending(b.addr, 16) ? 2 : 0) |
                               (buffers_.GpuWritePending(b.addr, 16, timeline_.CompletedCached()) ? 4 : 0));
    std::memset(aq_probe_buf_.mapped + u64{slot} * 16, 0xCD, 16);
    const u64 n = aq_watch_probe_ ? std::min<u64>(std::min<u64>(16, b.size), b.avail) : 0;
    if (n != 0) {
        const auto& f = Fn();
        const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cur_.cb, &dep);
        const VkBufferCopy region{.srcOffset = b.offset, .dstOffset = u64{slot} * 16, .size = n};
        f.vkCmdCopyBuffer(cur_.cb, b.buffer, aq_probe_buf_.buffer, 1, &region);
    }
    const std::pair<u64, u64> r{b.addr, b.addr + std::max<u64>(b.size, 16)};
    if (aq_watch_ranges_.size() < 256 && std::ranges::find(aq_watch_ranges_, r) == aq_watch_ranges_.end()) {
        aq_watch_lo_ = aq_watch_ranges_.empty() ? r.first : std::min(aq_watch_lo_, r.first);
        aq_watch_hi_ = std::max(aq_watch_hi_, r.second);
        aq_watch_ranges_.push_back(r);
    }
    aq_watch_pending_.push_back(id);
}

void Executor::AqWatchCheck(u64 completed) {
    // SSPC_DIAG_AQ_WATCH: the probes of completed batches against the CPU's bytes and the dispatch's threads (logged:
    // the first 16, then those with a count past the threads or a huge one)
    while (!aq_watch_pending_.empty()) {
        const u32 id = aq_watch_pending_.front();
        const u32 slot = id & (static_cast<u32>(aq_marker_ring_.size()) - 1);
        const AqMarker& e = aq_marker_ring_[slot];
        if (e.waddr != 0 && e.batch > completed) {
            break;
        }
        aq_watch_pending_.pop_front();
        if (e.waddr == 0) {
            continue; // (overwritten)
        }
        u32 g[4];
        std::memcpy(g, aq_probe_buf_.mapped + u64{slot} * 16, 16);
        const u64 threads = u64{e.groups[0]} * e.groups[1] * e.groups[2] * 64;
        const bool differ = std::memcmp(g, e.wcpu, 16) != 0, over = g[3] > threads,
                   huge = g[3] > 65536 || g[2] > (1u << 24);
        ++aq_watch_stats_[0];
        aq_watch_stats_[1] += differ ? 1 : 0;
        aq_watch_stats_[2] += over ? 1 : 0;
        aq_watch_stats_[3] += huge ? 1 : 0;
        if ((aq_watch_stats_[0] <= 16 || over || huge) && aq_watch_logged_ < 64) {
            ++aq_watch_logged_;
            SSPC_LOG(GPU, Warning,
                     "DIAGNOSTIC AQ watch: marker {} batch {}{} cs {:#x} groups {}x{}x{}: b0 {:#x}+{} path {} flags {} cpu "
                     "[{:#x} {:#x} {:#x} {:#x}] gpu [{:#x} {:#x} {:#x} {:#x}]{}",
                     id, e.batch, e.lane != 0 ? " (lane)" : "", e.cs, e.groups[0], e.groups[1], e.groups[2], e.waddr,
                     e.wsize, e.wpath, e.wflags, e.wcpu[0], e.wcpu[1], e.wcpu[2], e.wcpu[3], g[0], g[1], g[2], g[3],
                     over ? " COUNT PAST THE THREADS" : "");
        }
        if ((aq_watch_stats_[0] & 4095) == 0) {
            SSPC_LOG(GPU, Info, "DIAGNOSTIC AQ watch: {} checked, {} differ from the CPU's, {} past the threads, {} huge",
                     aq_watch_stats_[0], aq_watch_stats_[1], aq_watch_stats_[2], aq_watch_stats_[3]);
        }
    }
}

void Executor::OnDeviceLost(const char* where, u64 value) {
    if (device_lost_) {
        return;
    }
    device_lost_ = true;
    ctx_.Fail(where, VK_ERROR_DEVICE_LOST);
    std::string in_flight;
    for (const auto& b : in_flight_) {
        in_flight += fmt::format("  value {}{}{}: queues", b.value, b.lane != 0 ? " (lane)" : "", b.done ? " done" : "");
        for (const u32 q : b.queues) {
            in_flight += fmt::format(" {} (work {})", q, b.work[q]);
        }
        in_flight += "\n";
    }
    in_flight += fmt::format("  timeline: completed (last seen) {}, last submitted {}\n", timeline_.CompletedCached(),
                             timeline_.LastSubmitted());
    AqMarkersReport(in_flight);
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
        // GPU-6 (docs/GPU_OPTIMIZATION.md): the shadows are for the host memory mirror (its stores ~150x slower); a
        // binding in the buffer cache's video memory is written as fast as a shadow - copying it in and out (and the
        // two global barriers) is work for nothing: left in place (A/B: GPU -0.33..-0.37 ms a frame in all three
        // scenes). SSPC_GPU_SHADOW_VRAM=keep shadows them as before.
        if (info.buffer == buffers_.Buffer()) {
            ++stats_.shadow_vram;
            static const bool skip_vram = [] {
                char v[8] = {};
                const bool keep = GetEnvironmentVariableA("SSPC_GPU_SHADOW_VRAM", v, sizeof(v)) == 4 &&
                                  std::string_view(v) == "keep";
                if (keep) {
                    SSPC_LOG(GPU, Warning, "SSPC_GPU_SHADOW_VRAM=keep - written bindings in video memory are shadowed "
                                           "too (before GPU-6)");
                }
                return !keep;
            }();
            if (skip_vram) {
                continue;
            }
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
    const u64 id = diag_->Begin(cur_.cb, cur_.value, std::move(text), cur_.pool_lane);
    if (batch_first_cmd_ == 0) {
        batch_first_cmd_ = id;
    }
    batch_last_cmd_ = id;
    return id;
}

void Executor::DiagEnd(u64 id) {
    if (id != 0) {
        diag_->End(cur_.cb, id, cur_.pool_lane);
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
            if (aq_ && (aq_cmd_compute_ || aq_note_reads_)) {
                AqNote(addr, size);
            }
            aq_range_path_ = 1;
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
                        aq_range_path_ = 2;
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
                aq_range_path_ = 3;
                if (stream_who_on_ < 0) {
                    char v[4] = {};
                    stream_who_on_ = GetEnvironmentVariableA("SSPC_DIAG_STREAM_WHO", v, sizeof(v)) == 1 && v[0] == '1';
                }
                if (stream_who_on_ == 1) {
                    auto& e = stream_who_[stream_pipe_];
                    ++e[0];
                    e[1] += size;
                }
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
    const u64 up0 = buffers_.Stats().uploads;
    const bool prepared = buffers_.Prepare(addr, size, write, cur_.cb, cur_.value, r);
    stats_.t_prepare += GS::FineTicks() - tp;
    ++stats_.n_prepare;
    if (!prepared) {
        return false;
    }
    if (aq_ && (write || aq_cmd_compute_ || aq_note_reads_ || buffers_.Stats().uploads != up0)) {
        AqNote(addr, size); // (AQ: a write, an upload, or a compute command's access)
    }
    if (aq_ && buffers_.Stats().uploads != up0 && aq_note_uploads_) {
        // AQ: every page the upload copied - the block's other pages P4.9-B brings along too. A batch of the other
        // queue may write or read them meanwhile (measured: a main batch's block upload put the host's older bytes
        // over an emitter header the lane had written; the lane's next dispatch read a count of 0x3f800000 and never
        // finished - device lost)
        for (const auto& [b, e] : buffers_.LastUploads()) {
            AqNote(b, e - b);
        }
    }
    aq_range_path_ = write ? 6 : buffers_.Stats().uploads != up0 ? 5 : 4;
    if (!aq_watch_ranges_.empty() && (write || buffers_.Stats().uploads != up0)) {
        AqWatchWrite(addr, size, write ? 1 : 2);
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
    AqBoundary(queue);
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
        e_.FlushKeptBack(addr, bytes, false, 1); // (FB-7)
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
    AqBoundary(queue);
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
    if (aq_) {
        AqNote(dst, bytes);
    }
    if (!aq_watch_ranges_.empty()) {
        aq_what_ = 1;
        AqWatchWrite(dst, bytes, 3);
    }
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
    AqBoundary(queue);
    if (!Begin()) {
        return false;
    }
    write_run_ = false;
    aq_what_ = 2;
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

void Executor::OcclusionResetSlots(const std::vector<u32>& slots) {
    // outside rendering; vkCmdResetQueryPool orders itself after the earlier query commands on those queries
    for (size_t i = 0; i < slots.size();) {
        size_t j = i + 1;
        while (j < slots.size() && slots[j] == slots[j - 1] + 1) {
            ++j;
        }
        Fn().vkCmdResetQueryPool(cur_.cb, occ_pool_, slots[i], static_cast<u32>(j - i));
        i = j;
    }
}

bool Executor::ZpassShapeOk(u64 dst, u32 db_mask, u32 stride) {
    // SSPC_OCCLUSION=off: the earlier behaviour (every query reads as visible), for A/B
    static const bool off = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SSPC_OCCLUSION", v, sizeof(v)) > 0 && std::string_view(v) == "off";
    }();
    // 16-byte DB records {begin, end} for DBs 0..n-1 (the game: 8 or 16), in guest memory
    const u32 dbs = static_cast<u32>(std::popcount(db_mask));
    return !off && stride == 16 && dbs != 0 && dbs <= 16 && db_mask == (1u << dbs) - 1u && (dst & 7) == 0 &&
           dst < (1ull << 40);
}

bool Executor::ZpassOnTimeline(u32 queue, u64 seq, u64 dst, u32 db_mask, u32 stride) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb);
    const u32 dbs = static_cast<u32>(std::popcount(db_mask));
    if (!ZpassShapeOk(dst, db_mask, stride)) {
        ++occ_refused_;
        return false;
    }
    const bool end = occ_.open && dst == occ_.begin + 8;
    if (!end) {
        // the begin event: every DB record {begin 0, end 0}, both valid (bit 63); the end's low dword gets a count
        std::array<u32, 64> block{};
        for (u32 i = 0; i < dbs; ++i) {
            block[i * 4 + 1] = 0x80000000u;
            block[i * 4 + 3] = 0x80000000u;
        }
        if (!WriteOnTimeline(queue, seq, dst, block.data(), u64{dbs} * 16)) { // (it notes the work number)
            ++occ_refused_;
            return false;
        }
        EndRendering();
        if (occ_pool_ == VK_NULL_HANDLE) {
            const VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                           .queryType = VK_QUERY_TYPE_OCCLUSION,
                                           .queryCount = kOccSlots};
            if (Fn().vkCreateQueryPool(ctx_.Device(), &qi, nullptr, &occ_pool_) != VK_SUCCESS) {
                occ_pool_ = VK_NULL_HANDLE;
                SSPC_LOG(GPU, Error, "occlusion queries: no query pool - the guest's queries read as nothing drawn");
            } else {
                Fn().vkCmdResetQueryPool(cur_.cb, occ_pool_, 0, kOccSlots);
                SSPC_LOG(GPU, Info, "occlusion queries: counted on the host GPU ({})",
                         ctx_.Caps().occlusion_precise ? "precise sample counts"
                                                       : "NOT precise - the device has no exact counts");
            }
        }
        if (occ_.open) {
            ++occ_abandoned_; // a begin without its end: its draws' queries are not read, only made reusable
            OcclusionResetSlots(occ_.slots);
        }
        occ_.open = occ_pool_ != VK_NULL_HANDLE;
        occ_.begin = dst;
        occ_.max_slots = dbs;
        occ_.slots.clear();
        return true;
    }
    // the end event: the counts of the query's draws, one per DB record (the guest sums the DBs)
    occ_.open = false;
    ++occ_queries_;
    if (R12::LightCensusOn()) {
        R12::Stats().occ_queries.fetch_add(1, std::memory_order_relaxed);
        R12::Stats().occ_draws.fetch_add(occ_.slots.size(), std::memory_order_relaxed);
    }
    if ((occ_queries_ & 0xFFFF) == 1) {
        SSPC_LOG(GPU, Info, "occlusion queries: {} done, {} draws counted, {} refused, {} begun again before their end, "
                            "{} draws over a query's DB records",
                 occ_queries_, occ_draws_, occ_refused_, occ_abandoned_, occ_overflow_);
    }
    if (!Begin()) {
        return false;
    }
    if (occ_.slots.empty()) {
        Note(queue, seq); // nothing drawn: the begin event's zero counts are the result
        ++commands_;
        return true;
    }
    write_run_ = false;
    EndRendering();
    const auto ranges = [&] {
        acc_reads_.clear();
        acc_writes_.clear();
        for (size_t k = 0; k < occ_.slots.size(); ++k) {
            acc_writes_.push_back({dst + k * 16, dst + k * 16 + 4});
        }
    };
    ranges();
    if (NeedsBarrier(acc_reads_, acc_writes_, false)) {
        GlobalBarrier(5);
    }
    std::array<std::pair<VkBuffer, u64>, 16> where{};
    for (size_t k = 0; k < occ_.slots.size(); ++k) {
        u64 avail = 0;
        if (!GuestRange(dst + k * 16, 4, true, 4, where[k].first, where[k].second, avail) || avail < 4) {
            ++occ_refused_;
            OcclusionResetSlots(occ_.slots);
            return false; // the processor writes its counts (after Synchronize)
        }
    }
    Note(queue, seq);
    ++commands_;
    EndRendering(); // (the ranges' uploads ordered themselves)
    if (LabelChanges(kLabelOther | 4)) {
        Label(kLabelOther | 4, "T occlusion counts on the timeline");
    }
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    InspMark(Inspector::kGpuTransfer);
    for (size_t k = 0; k < occ_.slots.size(); ++k) {
        Fn().vkCmdCopyQueryPoolResults(cur_.cb, occ_pool_, occ_.slots[k], 1, where[k].first, where[k].second, 4,
                                       VK_QUERY_RESULT_WAIT_BIT);
    }
    if (prof_) {
        ProfMark(7, "occlusion counts copied to the guest's DB records (R12 key)");
    }
    OcclusionResetSlots(occ_.slots);
    ranges();
    NoteAccesses(acc_reads_, acc_writes_, false);
    const u64 bytes = occ_.slots.size() * 16;
    timeline_writes_.push_back({dst, dst + bytes, cur_.value});
    ++tw_version_;
    tw_lo_ = std::min(tw_lo_, dst);
    tw_hi_ = std::max(tw_hi_, dst + bytes);
    images_.Invalidate(dst, bytes);
    ++stats_.timeline_copies;
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
    AqBoundary(queue);
    if (!Begin()) {
        return false;
    }
    cur_.aq_gds = true;
    if (r11_diag_ != nullptr) {
        R11DiagGds((1ull << 62) | (u64{queue & 0xFFFF} << 48) | (u64{static_cast<u32>(kind) & 0xFFFF} << 32) | gds_offset);
    }
    write_run_ = false;
    aq_what_ = 3 | (u64{static_cast<u32>(kind)} << 8);
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

void Executor::WaitMetByHeldFence(u32 queue_and_fence, u64 after_seq) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    // (FrameBreak AQ: the fence's queue + 1 in the upper half, 0 unknown)
    const u32 queue = queue_and_fence & 0xFFFF;
    const u32 fence_q = (queue_and_fence >> 16) - 1;
    GuestSync(queue);
    // the guest's GPU waits for its earlier work here (EVENT_WRITE_EOS + WAIT_REG_MEM): everything recorded after it
    // waits for everything before it - the next command's global barrier (the batch-end barrier covers later batches)
    ++stats_.waits_by_held;
    MarkWrites(2);
    if (aq_) {
        // R10 diagnostics (SSPC_R10_DIAG_HELD=1): what each wait made the later work wait for (the first 200, then
        // every 32nd up to 400 more)
        static const bool diag = [] {
            char v[4] = {};
            return GetEnvironmentVariableA("SSPC_R10_DIAG_HELD", v, sizeof(v)) == 1 && v[0] == '1';
        }();
        if (diag) {
            static u32 seen = 0, logged = 0;
            const u32 k = seen++;
            if ((k < 200 || (k % 32) == 0) && logged++ < 600) {
                const AqLast l = fence_q < Cp::kNumQueues ? AqFenceBatch(fence_q, after_seq) : AqLast{};
                const AqLast last = fence_q < Cp::kNumQueues ? aq_q_last_[fence_q] : AqLast{};
                SSPC_LOG(GPU, Info, "R10 AQ wait-met #{} tsc {}: queue {} fence queue {} after {}; fence batch v{} lane {} "
                         "(queue's last v{} lane {}); open {} v{} lane {} queues {}; last main v{} lane v{}", k, __rdtsc(),
                         queue, static_cast<int>(fence_q), after_seq, l.value, l.lane, last.value, last.lane, open_,
                         cur_.value, cur_.lane, cur_.queues.size(), aq_last_main_, aq_last_lane_);
            }
        }
        // AQ: the work after the wait goes after the batch the fence's queue recorded in last - nothing to wait for on
        // the same queue or in the open batch; a main batch when the work after it may go to the lane
        if (fence_q >= Cp::kNumQueues || !aq_family_) {
            if (r11_diag_ != nullptr) {
                ++r11_diag_->unknown_fence_waits;
            }
            if (open_) {
                cur_.aq_wait_all = true;
            } else {
                aq_next_wait_main_ = std::max(aq_next_wait_main_, aq_last_main_);
            }
            if (r10_main_free_ && queue < Cp::kNumQueues) {
                // R10: unknown - the queue's later work in a main batch after every lane batch so far (the open one
                // included)
                r10_q_sync_[queue] = std::max(r10_q_sync_[queue], open_ && cur_.pool_lane != 0 ? cur_.value : aq_last_lane_);
            }
        } else if (const AqLast l = AqFenceBatch(fence_q, after_seq);
                   r10_main_free_ && l.lane != 0 && l.value != 0 && queue < Cp::kNumQueues) {
            // R10: the fence's work is a lane batch's (REA-3's gate: graphics waiting for the frame's async compute) -
            // a main batch with the waiting queue's work from here on waits for it
            r10_q_sync_[queue] = std::max(r10_q_sync_[queue], l.value);
        } else if (l.lane == 0 && l.value != 0) {
            if (open_ && l.value == cur_.value) {
                // the fence is in the open main batch: ordered while the work stays in it, but a split right after it
                // moves that work to the lane - the next batch waits for this one
                aq_next_wait_main_ = std::max(aq_next_wait_main_, l.value);
            } else if (open_) {
                cur_.aq_wait_main_min = std::max(cur_.aq_wait_main_min, l.value);
                aq_next_wait_main_ = std::max(aq_next_wait_main_, l.value); // (and after a split)
            } else {
                aq_next_wait_main_ = std::max(aq_next_wait_main_, l.value);
            }
        }
    }
}

Executor::AqLast Executor::AqFenceBatch(u32 fence_q, u64 after_seq) {
    // R10 (SSPC_R10_AQ_HELD_EXACT=1: after_seq != 0): the first batch with the fence queue's work up to the fence (the
    // main queue completes in order: it and everything before it); that work complete: nothing to wait for
    if (after_seq == 0) {
        return aq_q_last_[fence_q];
    }
    ++stats_.r10_held_exact;
    for (const Batch& b : in_flight_) {
        if (b.work[fence_q] >= after_seq) {
            if (b.done) {
                ++stats_.r10_held_complete;
                return {};
            }
            stats_.r10_held_earlier += b.value != aq_q_last_[fence_q].value ? 1 : 0;
            return {b.value, b.lane};
        }
    }
    if (open_ && cur_.work[fence_q] >= after_seq) {
        return {cur_.value, cur_.lane};
    }
    ++stats_.r10_held_complete; // (retired: completed)
    return {};
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
    if (aq_) {
        return WaitOldestOrWake(0, timeout_ns);
    }
    const bool done = timeline_.Wait(in_flight_.front().value, timeout_ns);
    if (!done && timeline_.Lost()) {
        OnDeviceLost("vkWaitSemaphores (oldest batch)", in_flight_.front().value);
    }
    return done;
}

bool Executor::WaitOldestOrWake(u64 wake_value, u64 timeout_ns) {
    if (in_flight_.empty() || Failed()) {
        return false;
    }
    if (aq_) {
        // AQ: the oldest batch not complete on either queue
        u64 m = 0, l = 0;
        for (const Batch& b : in_flight_) {
            if (!b.done) {
                u64& v = b.lane != 0 ? l : m;
                v = v == 0 ? b.value : v;
                if (m != 0 && l != 0) {
                    break;
                }
            }
        }
        if (m == 0 && l == 0) {
            return true;
        }
        const bool done = timeline_.WaitAny(m, l, wake_value, timeout_ns);
        if (!done && timeline_.Lost()) {
            OnDeviceLost("vkWaitSemaphores (oldest batch of either queue)", m != 0 ? m : l);
        }
        return done;
    }
    const bool done = timeline_.WaitOrWake(in_flight_.front().value, wake_value, timeout_ns);
    if (!done && timeline_.Lost()) {
        OnDeviceLost("vkWaitSemaphores (oldest batch or a wake-up)", in_flight_.front().value);
    }
    return done;
}

void Executor::Synchronize(u32) {
    const CallbackTimer cb_timer(stats_.t_cb, stats_.n_cb); // (P4.10)
    FlushKeptBack(0, 1ull << 40, false, 1); // (FB-7: the CPU side may read anything after it)
    SyncFrom(0);
}

bool Executor::FlushKeptBack(u64 addr, u64 size, bool eager, u32 why) {
    if (!buffers_.AnyDeferred() || !Begin()) {
        return false;
    }
    if (!buffers_.FlushDeferred(addr, size, cur_.cb, cur_.value, eager, why)) {
        return false;
    }
    cur_.aq_main = true;
    ++commands_; // (the batch is submitted by the sync that follows)
    return true;
}

void Executor::SyncKeptBack(u64 addr, u64 size, bool eager) {
    if (FlushKeptBack(addr, size, eager, 2)) {
        SyncFrom(2);
    } else if (KeptBack(addr, size)) {
        SyncFrom(2); // (written back by a batch not completed yet)
    }
    buffers_.Collect(timeline_.Completed()); // (shown again once completed)
}

bool Executor::OnRecordThread() const {
    return GetCurrentThreadId() == record_tid_.load(std::memory_order_relaxed);
}

void Executor::RequestKeptBack(u64 addr, u64 size) {
    if (OnRecordThread()) {
        ++stats_.lazy_inline; // (the recording thread itself: no one else to wait for)
        SyncKeptBack(addr, size, true);
        return;
    }
    LazyReq* r = nullptr;
    while (r == nullptr && !Failed()) {
        for (LazyReq& q : lazy_req_) {
            u32 free = 0;
            if (q.state.compare_exchange_strong(free, 1, std::memory_order_acq_rel)) {
                r = &q;
                break;
            }
        }
        if (r == nullptr) {
            Sleep(0);
        }
    }
    if (r == nullptr) {
        return;
    }
    r->addr = addr;
    r->size = size;
    r->state.store(2, std::memory_order_release);
    lazy_reqs_.fetch_add(1, std::memory_order_acq_rel);
    for (u32 spins = 0; r->state.load(std::memory_order_acquire) != 3 && !Failed(); ++spins) {
        if (const auto wake = wake_.load(std::memory_order_acquire); wake != nullptr && spins % 64 == 0) {
            wake(wake_ctx_);
        }
        Sleep(spins < 64 ? 0 : 1);
    }
    const u64 v = r->value.load(std::memory_order_acquire);
    r->state.store(0, std::memory_order_release);
    while (v != 0 && !Failed() && !timeline_.Wait(v, 1'000'000'000ull)) {
        if (timeline_.Lost()) {
            break;
        }
    }
}

void Executor::SetRecordThread() {
    record_tid_.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

void Executor::ServeKeptBack() {
    if (lazy_reqs_.load(std::memory_order_acquire) == 0) {
        return;
    }
    LazyReq* served[16];
    u32 n = 0;
    for (LazyReq& q : lazy_req_) {
        if (q.state.load(std::memory_order_acquire) == 2) {
            FlushKeptBack(q.addr, q.size, true, 3);
            served[n++] = &q;
        }
    }
    if (n == 0) {
        return;
    }
    u64 v = 0;
    if (open_ && commands_ > 0) {
        v = cur_.value;
        Flush();
    } else if (!in_flight_.empty()) {
        v = in_flight_.back().value; // (written back by a batch already submitted)
    }
    ++stats_.lazy_served;
    for (u32 i = 0; i < n; ++i) {
        served[i]->value.store(v, std::memory_order_release);
        served[i]->state.store(3, std::memory_order_release);
        lazy_reqs_.fetch_sub(1, std::memory_order_acq_rel);
    }
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
        FlushKeptBack(addr, bytes, false, 1); // (FB-7)
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
                         bool& writes_memory, const std::vector<CachedImage*>& targets, const u32* user_data,
                         u64 material) {
    R8_CP_PROF(compute ? CpProfile::Sec::T2Bind
                       : CpProfile::BySource(CpProfile::Sec::T2Bind, CpProfile::Sec::T2BindMesh, CpProfile::t_source));
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
            if (const auto ov = std::ranges::find(inst_bind_, index, &std::pair<u32, VkDescriptorBufferInfo>::first);
                ov != inst_bind_.end()) {
                info = ov->second; // SSPC_CITY_INSTANCING: every draw's block of the batch, at the stride (aligned)
                if (index < push.buf_offsets.size()) {
                    push.buf_offsets[index] = 0;
                }
                break;
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
            if (aq_watch_capture_ && compute && i == 0) {
                aq_watch_b0_ = {vb, voff, b.address, b.size, vavail, aq_range_path_};
            }
            if (b.written) {
                writes_memory = true;
                images_.InvalidateFromBufferWrite(b.address, b.size);
            }
            if (cb_dump_flip_ != 0 && !compute && !b.written && b.size <= (64u << 10)) {
                // SSPC_UPSCALE_CB_DUMP (diagnostics): the first scene draw's constant buffers, frame after frame
                std::vector<u8> bytes(b.size);
                if (auto* mem = GetGuestMemory(); mem != nullptr) {
                    mem->CopySparse(b.address, bytes.data(), b.size);
                    const auto dir = options_.capture_path.parent_path().parent_path() / "upscale" / "cb";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    std::ofstream(dir / fmt::format("cb_{}_{}_{}_{:x}.bin", cb_dump_flip_, s.module == nullptr ? 0 : 1,
                                                    b.binding, b.address),
                                  std::ios::binary)
                        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
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
            cur_.aq_gds = true;
            if (r11_diag_ != nullptr) {
                R11DiagGds(aq_what_);
            }
            info = {gds_.buffer, 0, gds_.size};
            writes_memory |= b.written;
            break;
        case GS::BufferKind::SharedMemory: {
            // the translator's emulated LDS: each workgroup indexes its own part (the bridge: SharedMemSize() *
            // NumWorkgroups(), zeroed before the dispatch)
            const u64 per_group = std::max<u64>(refl.shared_memory_size, lds_group_bytes_);
            const u64 bytes = (std::max<u64>(per_group * (compute ? lds_groups_ : 1), 16) + 255) & ~255ull;
            const auto& f = Fn();
            // (AQ: a batch of compute queues only so far - possibly the lane's - takes the lane's buffer)
            const bool lane_lds = aq_ && (aq_family_ ? cur_.pool_lane != 0
                                                     : aq_cmd_compute_ && cur_.work[Cp::kGraphicsQueue] == 0 && !cur_.aq_main);
            OwnedBuffer& lds_buf = lane_lds ? lds_lane_ : lds_;
            cur_.aq_lds |= lane_lds ? 2 : 1;
            if (lds_buf.size < bytes) {
                if (lds_buf.buffer != VK_NULL_HANDLE) {
                    lds_retired_.push_back({cur_.value, lds_buf});
                    lds_buf = {};
                }
                u64 grown = 64ull << 20;
                while (grown < bytes) {
                    grown *= 2;
                }
                std::string err;
                if (!CreateBuffer(ctx_, grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  false, lds_buf, err)) {
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
            f.vkCmdFillBuffer(cur_.cb, lds_buf.buffer, 0, bytes, 0);
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            f.vkCmdPipelineBarrier2(cur_.cb, &dep);
            info = {lds_buf.buffer, 0, bytes};
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
    const bool gb_parts = !compute && CpProfile::PartsSource(); // (R8: a 0x1a1770 draw's parts; R10: SSPC_R10_T2_PARTS)
    if (gb_parts) {
        CpProfile::Enter(CpProfile::Sec::T2GbBindImages);
    }
    const VkPipelineStageFlags2 shader_stage = compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : kGraphicsShaders;
    images_.ApplyPendingInvalidations(); // before the memo's dirty checks below
    const bool seg = seg_draw_ && !compute; // (SSPC_TEX_SEGMENT: a draw's textures checked once a segment)
    if (seg) {
        SegSync();
    }
    const u64 border_base = u64{regs.Get(kTaBcBase)} << 8;
    const float lod = !compute && jitter_draw_ ? lod_bias_ : 0.0f;
    const auto image_flags = [](const GS::ImageBinding& im) { return TexFlags(im); };
    const auto sampler_desc = [](const GS::SamplerBinding& sm) {
        return sm.status != GS::ResourceStatus::Ok ? GS::SamplerDesc{} : sm.desc;
    };
    // SSPC_NATIVE_GBUFFER: a native 0x1a1770 stage's images and samplers as kept for its material (GbBind)
    GbBind* gb = nullptr;
    const u64 gb_epoch = images_.Epoch();
    u32 lod_bits = 0;
    std::memcpy(&lod_bits, &lod, 4);
    if (gb_mode_ == GbMode::Unread) {
        char v[16] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_NATIVE_GBUFFER_BIND", v, sizeof(v));
        const std::string_view sv(v, n < sizeof(v) ? n : 0);
        // (off by default: R8, city - images 533 -> 536 ns a stage, samplers 81 -> 60; the cost per texture is its
        // image's state - validity, Touch, layout - not the lookup; docs/NATIVE_GBUFFER_DRAW_0x1a1770.md 9.8)
        gb_mode_ = sv == "on" ? GbMode::On : sv == "verify" ? GbMode::Verify : GbMode::Off;
        if (gb_mode_ != GbMode::Off) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_NATIVE_GBUFFER_BIND={} - the native G-buffer stages' material "
                                   "bindings kept{}",
                     sv, gb_mode_ == GbMode::Verify ? ", each hit made again in full and compared" : "");
        }
    }
    bool gb_checking = false; // (verify: a hit's bindings, made again below)
    size_t gb_i0 = 0, gb_w0 = 0, gb_b0 = 0, gb_n0 = 0;
    if (material != 0 && gb_mode_ != GbMode::Off && !compute && !up_display_draw_ &&
        plan.images.size() <= kGbMaxImages && plan.samplers.size() <= kGbMaxSamplers) {
        if (gb_bind_.empty()) {
            gb_bind_.resize(kGbBinds);
        }
        gb = &gb_bind_[(material * 0x9E3779B97F4A7C15ull) >> 53];
        bool hit = gb->id == material && gb->module == s.module && gb->epoch == gb_epoch && gb->lod == lod_bits &&
                   gb->images == plan.images.size() && gb->samplers == plan.samplers.size();
        for (size_t i = 0; hit && i < plan.images.size(); ++i) {
            const auto& im = plan.images[i];
            if (im.count > 1 || im.fmask || im.storage) {
                hit = false; // (one sampled view each - a null one too: its descriptor type and count are the binding's)
            } else if (im.status != GS::ResourceStatus::Ok) {
                hit = gb->flags[i] == kGbNull;
                continue;
            }
            const CachedImage* m = gb->img[i];
            hit = hit && gb->flags[i] == image_flags(im) && gb->raw[i] == im.desc.raw &&
                  ((m->gpu_written && !m->memory_newer) || (m->uploaded && !m->dirty && !m->gpu_written));
        }
        for (size_t i = 0; hit && i < plan.samplers.size(); ++i) {
            hit = gb->sraw[i] == sampler_desc(plan.samplers[i]).raw;
        }
        if (hit) {
            ++stats_.gb_bind_hits;
            gb_i0 = image_infos.size();
            gb_w0 = writes.size();
            gb_b0 = bound.size();
            gb_n0 = info_images_.size();
            for (size_t i = 0; i < plan.images.size(); ++i) {
                const auto& im = plan.images[i];
                const size_t first = image_infos.size();
                CachedImage* m = gb->img[i];
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
                if (gb->flags[i] != kGbNull) {
                    images_.Touch(*m, cur_.value); // what Find records
                    const bool feedback = std::ranges::find(targets, m) != targets.end();
                    ii = {VK_NULL_HANDLE, gb->view[i],
                          feedback                     ? VK_IMAGE_LAYOUT_GENERAL
                          : m->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    bound.push_back(m);
                } else {
                    m = nullptr;
                }
                image_infos.push_back(ii);
                info_images_.push_back(m);
                writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstBinding = im.binding,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                  .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
            }
            if (gb_parts) {
                CpProfile::Leave();
                CpProfile::Enter(CpProfile::Sec::T2GbBindSamplers);
            }
            for (size_t i = 0; i < plan.samplers.size(); ++i) {
                VkSampler vs = gb->smp[i];
                if (vs == VK_NULL_HANDLE) {
                    const bool depth = i < refl.samplers.size() && refl.samplers[i].is_depth;
                    vs = images_.Sampler(sampler_desc(plan.samplers[i]), depth, border_base, lod);
                    if (vs == VK_NULL_HANDLE) {
                        vs = images_.Sampler(GS::SamplerDesc{}, depth, 0);
                    }
                }
                const size_t first = image_infos.size();
                image_infos.push_back({vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
                info_images_.push_back(nullptr);
                writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstBinding = plan.samplers[i].binding,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                  .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
            }
            if (gb_mode_ != GbMode::Verify) {
                if (gb_parts) {
                    CpProfile::Leave();
                }
                return true;
            }
            // verify: set aside, the full lookups below make them again (compared at the end)
            gb_check_infos_.assign(image_infos.begin() + gb_i0, image_infos.end());
            gb_check_writes_.assign(writes.begin() + gb_w0, writes.end());
            gb_check_bound_.assign(bound.begin() + gb_b0, bound.end());
            gb_check_info_images_.assign(info_images_.begin() + gb_n0, info_images_.end());
            image_infos.resize(gb_i0);
            writes.resize(gb_w0);
            bound.resize(gb_b0);
            info_images_.resize(gb_n0);
            gb_checking = true;
            if (gb_parts) {
                CpProfile::Leave();
                CpProfile::Enter(CpProfile::Sec::T2GbBindImages);
            }
        } else {
            ++stats_.gb_bind_misses;
        }
        // kept below when every image, null or not, is one sampled view (no FMask, not storage)
        gb_new_.images = static_cast<u8>(plan.images.size());
        gb_new_.samplers = static_cast<u8>(plan.samplers.size());
        if (gb_checking) {
            gb = nullptr; // (verify: the entry stays as it was)
        }
        for (const auto& im : plan.images) {
            if (gb == nullptr || im.count > 1 || im.fmask || im.storage) {
                gb = nullptr;
                break;
            }
        }
    }
    u32 image_index = 0;
    for (const auto& im : plan.images) {
        const u32 gi = image_index++;
        const u32 count = std::max<u32>(im.count, 1);
        const size_t first = image_infos.size();
        bool null_image = im.status != GS::ResourceStatus::Ok;
        CachedImage* img = nullptr;
        if (movie_check_ && !null_image) {
            // a 1920x1080 movie's picture: the game's copies of the decoder's NV12 planes (8-bit luma, 8_8 chroma at
            // half size, 16-aligned heights)
            const u32 tw = im.desc.Width(), th = im.desc.Height(), tf = im.desc.DataFormat();
            if ((tf == 1 && tw == 1920 && th >= 1080 && th <= 1088) || (tf == 3 && tw == 960 && th >= 540 && th <= 544)) {
                movie_hit_ = true;
                movie_v_end_ = static_cast<float>(tw * 9 / 16) / static_cast<float>(th);
            }
        }
        // the memo: one view (a dynamic mip index binds every level: not memoized), the same T# and binding flags
        const bool memo_able = !null_image && count == 1 && !im.fmask;
        const u32 flags = image_flags(im);
        if (gb != nullptr) {
            gb_new_.flags[gi] = null_image ? kGbNull : flags;
            gb_new_.raw[gi] = im.desc.raw;
            gb_new_.img[gi] = nullptr;
            gb_new_.view[gi] = VK_NULL_HANDLE;
        }
        if (im.storage) {
            ++tex_seg_; // (SSPC_TEX_SEGMENT: an image a shader writes - no stamp of before is taken again)
        }
        u64 memo_key = 0;
        TexMemo* memo = nullptr;
        if (memo_able) {
            memo_key = TexMemoKey(im.desc.raw, flags);
            auto& slot = tex_slot_[TexSlot(memo_key)];
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
            if (seg && m != nullptr && memo->seg == tex_seg_ && !im.storage && memo->epoch == images_.Epoch() &&
                memo->raw == im.desc.raw && memo->flags == flags && std::ranges::find(targets, m) == targets.end()) {
                // checked in this segment: valid, touched, in its layout, read there since its barrier - none of that
                // can have changed (the image cache's generation is the same), the image is not looked at again
                ++stats_.tex_seg_hits;
                if (seg_mode_ != SegMode::Verify) {
                    image_infos.push_back({VK_NULL_HANDLE, memo->view, memo->seg_layout});
                    info_images_.push_back(nullptr); // (no layout fix-up: it is in seg_layout)
                    if (gb != nullptr) {
                        gb_new_.img[gi] = m;
                        gb_new_.view[gi] = memo->view;
                    }
                    writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstBinding = im.binding,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
                    continue;
                }
                // verify: the image as the full path below finds it (before its Touch), then that path taken
                const bool valid = (m->gpu_written && !m->memory_newer) || (m->uploaded && !m->dirty && !m->gpu_written);
                const bool same = valid && m->current == memo->seg_layout && m->current == TextureLayout(m) &&
                                  ImageCache::ReadSince(*m, kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT) &&
                                  m->last_seq == cur_.value && m->last_frame == images_.Frame();
                ++stats_.tex_seg_verified;
                if (!same && stats_.tex_seg_mismatches++ < 16) {
                    SSPC_LOG(GPU, Warning,
                             "SSPC_TEX_SEGMENT=verify: img{} {:#x} stamped in segment {} differs: valid {} layout {} "
                             "(stamped {}, texture {}) stage {:#x} access {:#x} seq {} (batch {}) frame {} ({})",
                             m->id, m->layout.guest_address, tex_seg_, valid, static_cast<int>(m->current),
                             static_cast<int>(memo->seg_layout), static_cast<int>(TextureLayout(m)), m->stage, m->access,
                             m->last_seq, cur_.value, m->last_frame, images_.Frame());
                }
                seg_check_.push_back({static_cast<u32>(image_infos.size()), memo->seg_layout});
            }
            if (m != nullptr && memo->epoch == images_.Epoch() && memo->raw == im.desc.raw && memo->flags == flags &&
                ((m->gpu_written && !m->memory_newer) || (m->uploaded && !m->dirty && !m->gpu_written))) {
                ++stats_.tex_memo_hits;
                images_.Touch(*m, cur_.value); // what Find records
                if (seg && !im.storage) {
                    seg_pending_.push_back({memo, m}); // (stamped once the draw's barriers are recorded)
                }
                if (im.storage) {
                    m->gpu_written = true;
                }
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, memo->view, VK_IMAGE_LAYOUT_GENERAL};
                const bool feedback = std::ranges::find(targets, m) != targets.end();
                ii.imageLayout = (im.storage || feedback) ? VK_IMAGE_LAYOUT_GENERAL
                                 : m->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (up_display_draw_ && !im.storage) { // (upscaling: the display draw's textures)
                    const ImageViewDesc vd = TSharpView(im.desc, im.depth, im.array, im.storage);
                    const ImageLayout tl = LayoutFromTSharp(im.desc, im.depth);
                    const auto comps = ViewComponents(vd.mapping);
                    up_sampled_.push_back({m, ii.imageView, vd.format,
                                           comps.Valid() ? comps.value : VkComponentMapping{}, tl.size.width,
                                           tl.size.height});
                }
                image_infos.push_back(ii);
                info_images_.push_back(m);
                bound.push_back(m);
                if (gb != nullptr) {
                    gb_new_.img[gi] = m;
                    gb_new_.view[gi] = memo->view;
                }
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
                if (gb_parts) {
                    CpProfile::Leave();
                }
                return false;
            }
        }
        for (u32 j = 0; j < count; ++j) {
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            if (img != nullptr) {
                ImageViewDesc vd = TSharpView(im.desc, im.depth, im.array, im.storage);
                if (!vd.supported) {
                    Skipped(Skip::Image, std::string(vd.reason));
                    if (gb_parts) {
                        CpProfile::Leave();
                    }
                    return false;
                }
                if (count > 1) {
                    vd.base_level = static_cast<u16>(vd.base_level + j);
                    vd.levels = 1;
                }
                ii.imageView = images_.View(*img, vd, im.storage);
                if (up_display_draw_ && !im.storage && count == 1) { // (upscaling: the display draw's textures)
                    const auto comps = ViewComponents(vd.mapping);
                    const ImageLayout tl = LayoutFromTSharp(im.desc, im.depth);
                    up_sampled_.push_back({img, ii.imageView, vd.format,
                                           comps.Valid() ? comps.value : VkComponentMapping{}, tl.size.width,
                                           tl.size.height});
                }
                if (memo != nullptr && ii.imageView != VK_NULL_HANDLE) {
                    *memo = {im.desc.raw, flags, images_.Epoch(), img, ii.imageView}; // (no stamp)
                    if (seg && !im.storage) {
                        seg_pending_.push_back({memo, img});
                    }
                }
                if (gb != nullptr && count == 1) {
                    gb_new_.img[gi] = img;
                    gb_new_.view[gi] = ii.imageView;
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
    if (gb_parts) {
        CpProfile::Leave();
        CpProfile::Enter(CpProfile::Sec::T2GbBindSamplers);
    }
    for (size_t i = 0; i < plan.samplers.size(); ++i) {
        const auto& sm = plan.samplers[i];
        const bool depth = i < refl.samplers.size() && refl.samplers[i].is_depth;
        const GS::SamplerDesc desc = sampler_desc(sm);
        VkSampler vs = images_.Sampler(desc, depth, border_base, lod);
        if (vs == VK_NULL_HANDLE) {
            vs = images_.Sampler(GS::SamplerDesc{}, depth, 0); // the bridge's fallback: the default S#
        }
        if (gb != nullptr) {
            gb_new_.sraw[i] = desc.raw;
            gb_new_.smp[i] = desc.BorderColorType() == 3 ? VK_NULL_HANDLE : vs; // (a custom colour: read each time)
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
    // (kept only when every image has its view and the image cache's epoch held: what the next draw checks)
    if (gb_checking) {
        const auto same_write = [](const VkWriteDescriptorSet& a, const VkWriteDescriptorSet& b) {
            return a.dstBinding == b.dstBinding && a.descriptorCount == b.descriptorCount &&
                   a.descriptorType == b.descriptorType && a.pImageInfo == b.pImageInfo;
        };
        const auto same_info = [](const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
            return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
        };
        const bool same =
            std::ranges::equal(gb_check_infos_, std::span(image_infos).subspan(gb_i0), same_info) &&
            std::ranges::equal(gb_check_writes_, std::span(writes).subspan(gb_w0), same_write) &&
            std::ranges::equal(gb_check_bound_, std::span(bound).subspan(gb_b0)) &&
            std::ranges::equal(gb_check_info_images_, std::span(info_images_).subspan(gb_n0));
        ++stats_.gb_bind_verified;
        if (!same && stats_.gb_bind_mismatches++ < 16) {
            SSPC_LOG(GPU, Warning, "SSPC_NATIVE_GBUFFER_BIND=verify: program {:#x} material {:#x}: the kept bindings "
                                   "differ from the full lookups ({} / {} image infos, {} / {} writes, {} / {} bound)",
                     s.address, material, gb_check_infos_.size(), image_infos.size() - gb_i0, gb_check_writes_.size(),
                     writes.size() - gb_w0, gb_check_bound_.size(), bound.size() - gb_b0);
        }
    }
    bool gb_complete = gb != nullptr && images_.Epoch() == gb_epoch;
    for (u32 i = 0; gb_complete && i < gb_new_.images; ++i) {
        gb_complete = gb_new_.flags[i] == kGbNull || (gb_new_.img[i] != nullptr && gb_new_.view[i] != VK_NULL_HANDLE);
    }
    if (gb_complete) {
        gb_new_.id = material;
        gb_new_.module = s.module;
        gb_new_.epoch = gb_epoch;
        gb_new_.lod = lod_bits;
        *gb = gb_new_;
        ++stats_.gb_bind_kept;
    }
    if (gb_parts) {
        CpProfile::Leave();
    }
    return true;
}

void Executor::SetDynamicState(const Cp::RegisterFile& r, const GS::GraphicsPipelineState& st, bool indexed) {
    if (dyn_memo_on_ < 0) {
        // SSPC_CITY_DYN_MEMO=1: the groups of every register read below (one outside the groups: the memo stays off);
        // =ab:<n> (image A/B, docs/CITY_PERFORMANCE.md): on in every second block of n flips only
        char v[16] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_CITY_DYN_MEMO", v, sizeof(v));
        const std::string_view sv(v, n < sizeof(v) ? n : 0);
        dyn_memo_on_ = sv == "1" ? 1 : 0;
        if (sv.starts_with("ab:")) {
            dyn_memo_ab_ = std::max<u32>(1, static_cast<u32>(std::strtoul(v + 3, nullptr, 10)));
            dyn_memo_on_ = 1;
        }
        std::vector<u32> read = {kPaClClipCntl,       kPaClVteCntl,        kWindowOffset,     kWindowScissorTl,
                                 kWindowScissorBr,    kGenericScissorTl,   kGenericScissorBr, kScreenScissorTl,
                                 kScreenScissorBr,    kPaScModeCntl0,      kDbDepthControl,   kDbRenderControl,
                                 kDbZInfo,            kDbStencilInfo,      kDbDepthBoundsMin, kDbDepthBoundsMax,
                                 kPaSuScModeCntl,     kDbStencilControl,   kDbStencilRefMask, kDbStencilRefMaskBf,
                                 kVgtMultiPrimIbResetEn};
        for (u32 i = 0; i < 16 * 6; ++i) {
            read.push_back(kViewport0 + i);
        }
        for (u32 i = 0; i < 16 * 2; ++i) {
            read.push_back(kViewportScissor0 + i);
        }
        for (u32 i = 0; i < 5; ++i) {
            read.push_back(kPolyOffsetClamp + i);
        }
        for (u32 i = 0; i < 4; ++i) {
            read.push_back(kCbBlendRed + i);
        }
        for (const u32 reg : read) {
            const Cp::Group g = Cp::GroupOf(reg);
            if (g == Cp::Group::Count) {
                dyn_memo_on_ = 0;
                SSPC_LOG(GPU, Error, "SSPC_CITY_DYN_MEMO: register {:#x} is in no group - off", reg);
                break;
            }
            if (std::ranges::find(dyn_groups_, g) == dyn_groups_.end()) {
                dyn_groups_.push_back(g);
            }
        }
        if (dyn_groups_.size() > dyn_memo_.versions.size()) {
            dyn_memo_on_ = 0;
        }
        if (dyn_memo_on_ == 1) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_DYN_MEMO=1: the dynamic state is computed again only when one of {} register "
                                "groups, the pipeline state's fields or the jitter changed",
                     dyn_groups_.size());
        }
    }
    DynMemo memo;
    if (dyn_memo_on_ == 1 && !stencil_override_ && !movie_fit_ &&
        (dyn_memo_ab_ == 0 || (VideoOut::NextFlipId() / dyn_memo_ab_) % 2 == 1)) {
        memo.valid = true;
        memo.regs = &r;
        for (size_t i = 0; i < dyn_groups_.size(); ++i) {
            memo.versions[i] = r.Version(dyn_groups_[i]);
        }
        memo.prim_type = st.prim_type;
        memo.mrt_mask = st.mrt_mask;
        for (u32 i = 0, n = static_cast<u32>(std::bit_width(st.mrt_mask & 0xFFu)); i < n; ++i) {
            memo.write_masks[i] = st.targets[i].write_mask & 0xF;
        }
        memo.indexed = indexed;
        memo.jitter = jitter_draw_;
        if (jitter_draw_) {
            memo.jx = jitter_x_;
            memo.jy = jitter_y_;
        }
        if (dyn_.valid && memo == dyn_memo_) {
            ++stats_.dyn_memo_hits;
            return;
        }
    }
    SetDynamicStateWhole(r, st, indexed);
    dyn_memo_ = memo; // (off: never valid)
}

void Executor::SetRunStencilRef(const Cp::RegisterFile& r, u32 front_ref, u32 back_ref) {
    const u32 dc = r.Get(kDbDepthControl);
    if (!dyn_.valid || (dc & 1) == 0 || (r.Get(kDbStencilInfo) & 1) == 0) {
        return; // (no stencil test: the reference and masks do not matter, SetDynamicStateWhole sets none)
    }
    const auto& f = Fn();
    const u32 sc = r.Get(kDbStencilControl);
    const u32 rc = r.Get(kDbRenderControl);
    const bool backface = (dc >> 7) & 1;
    if (!backface) {
        back_ref = front_ref;
    }
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
    const auto set = [&](VkStencilFaceFlags face, u32 ref, bool op_val, DynShadow::Face& have) {
        const u32 reference = op_val ? (ref >> 24) : (ref & 0xFF);
        const u32 write_mask = clear ? 0 : (ref >> 16) & 0xFF;
        const u32 compare_mask = (ref >> 8) & 0xFF;
        if (reference != have.reference) {
            f.vkCmdSetStencilReference(cur_.cb, face, reference);
            have.reference = reference;
            ++dyn_set_;
        }
        if (write_mask != have.write_mask) {
            f.vkCmdSetStencilWriteMask(cur_.cb, face, write_mask);
            have.write_mask = write_mask;
            ++dyn_set_;
        }
        if (compare_mask != have.compare_mask) {
            f.vkCmdSetStencilCompareMask(cur_.cb, face, compare_mask);
            have.compare_mask = compare_mask;
            ++dyn_set_;
        }
    };
    set(VK_STENCIL_FACE_FRONT_BIT, front_ref, fop, dyn_.front);
    set(VK_STENCIL_FACE_BACK_BIT, back_ref, bop, dyn_.back);
    dyn_memo_.valid = false; // (the state set is not the replica's any more)
}

void Executor::SetDynamicStateWhole(const Cp::RegisterFile& r, const GS::GraphicsPipelineState& st, bool indexed) {
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
            if (jitter_draw_) { // upscaling: the scene's sub-pixel jitter (render pixels)
                v.x += jitter_x_;
                v.y += jitter_y_;
            }
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
    if (movie_fit_) { // (the movie draw, see movie_fit_) the viewport: the 16:9 box centred in the target
        VkViewport& v = vps[0];
        const float cx = v.x + v.width / 2, cy = v.y + v.height / 2;
        if (std::abs(v.width) / std::abs(v.height) > 16.0f / 9.0f) {
            v.width = std::copysign(std::abs(v.height) * 16.0f / 9.0f, v.width);
        } else {
            v.height = std::copysign(std::abs(v.width) * 9.0f / 16.0f, v.height);
        }
        v.x = cx - v.width / 2;
        v.y = cy - v.height / 2;
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
        // (Phase 6: a G-buffer run member recorded in full has its own reference on the run's registers)
        const u32 fr = stencil_override_ ? stencil_override_ref_ : r.Get(kDbStencilRefMask);
        const u32 br = stencil_override_ ? stencil_override_bf_ : r.Get(kDbStencilRefMaskBf);
        const u32 front_ref = fr, back_ref = backface ? br : front_ref;
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
    if (mode == 2 || mode == 5 || d.primitive_type == 0) { // EliminateFastClear, FmaskDecompress, None
        return true;
    }
    // DIAGNOSTIC (measurement only, changes the picture): SSPC_DIAG_SKIP_DEPTH_SIZE=<width> - depth-only draws into a
    // depth target of that width are dropped before any preparation (what such a pass costs the command processor)
    static const u32 skip_width = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_SKIP_DEPTH_SIZE", v, sizeof(v)) > 0
                   ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                   : 0u;
    }();
    // no colour: CB_TARGET_MASK 0 or CB_COLOR0_INFO.FORMAT invalid; DB_DEPTH_SIZE.PITCH_TILE_MAX = width / 8 - 1
    const bool hit = skip_width != 0 && (regs.Get(kCbTargetMask) == 0 || ((regs.Get(0xA31C) >> 2) & 0x1F) == 0) &&
                     (regs.Get(kDbZInfo) & 3) != 0 && ((regs.Get(0xA016) & 0x7FF) + 1) * 8 == skip_width;
    if (R12::LightCensusOn() && (regs.Get(kCbTargetMask) == 0 || ((regs.Get(0xA31C) >> 2) & 0x1F) == 0) &&
        (regs.Get(kDbZInfo) & 3) != 0) { // (R12 light census: the spot shadow atlas is the 1024-wide depth target)
        (((regs.Get(0xA016) & 0x7FF) + 1) * 8 == 1024 ? R12::Stats().atlas_draws : R12::Stats().cascade_draws)
            .fetch_add(1, std::memory_order_relaxed);
    }
    if (hit) {
        static std::atomic<u64> n{0};
        if (const u64 k = n.fetch_add(1, std::memory_order_relaxed); (k & (k - 1)) == 0 && k >= 1024) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_SKIP_DEPTH_SIZE={} - {} depth-only draws dropped", skip_width,
                     k);
        }
    }
    return hit;
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

std::string Executor::StreamWhoReport() const {
    if (stream_who_.empty()) {
        return {};
    }
    std::vector<std::pair<u32, std::array<u64, 2>>> v(stream_who_.begin(), stream_who_.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second[1] > b.second[1]; });
    std::string t = "streamed ranges by pipeline (SSPC_DIAG_STREAM_WHO; p0 = no draw): ";
    for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i) {
        t += fmt::format("p{} {} ranges {} MiB; ", v[i].first == 0 ? 0 : v[i].first - 1, v[i].second[0],
                         v[i].second[1] >> 20);
    }
    return t + "\n";
}

bool Executor::BatchModuleOk(const GS::Module* m, bool vertex) {
    const auto [it, fresh] = batch_modules_.try_emplace(m, false);
    if (!fresh) {
        return it->second;
    }
    // a batch draws its geometry at other vertex indices and primitive numbers: a stage that loads the vertex index or
    // the base vertex (VS), or the primitive ID (PS), is never batched (a built-in only declared is fine)
    const std::vector<u32>& w = m->result.spirv;
    std::unordered_set<u32> ids;
    for (size_t i = 5; i < w.size();) {
        const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
        if (n == 0 || i + n > w.size()) {
            return false; // (malformed: never batched)
        }
        if (op == 71 && n >= 4 && w[i + 2] == 11) { // OpDecorate <id> BuiltIn <builtin>
            const u32 b = w[i + 3];
            if (vertex ? (b == 42 || b == 5 || b == 4424) : (b == 7)) { // VertexIndex, VertexId, BaseVertex; PrimitiveId
                ids.insert(w[i + 1]);
            }
        }
        i += n;
    }
    // a load of one is dead when the translator only parks it in a register variable that is written again before it
    // is ever read (the VS's v0: the fetch shader's index, replaced by vertex inputs). Proven only for one function
    // without branches (instruction order = execution order); anything else: never batched.
    std::vector<size_t> at; // instruction starts
    u32 functions = 0, branches = 0;
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) {
        at.push_back(i);
        const u32 op = w[i] & 0xFFFF;
        functions += op == 54 ? 1 : 0;                                     // OpFunction
        branches += op == 250 || op == 251 || op == 246 || op == 247 ? 1 : 0; // conditional, switch, loop/selection merge
    }
    const auto uses = [&](u32 id, size_t from, size_t& first) {
        u32 n = 0;
        for (size_t k = from; k < at.size(); ++k) {
            const size_t i = at[k];
            for (size_t j = i + 1; j < i + (w[i] >> 16); ++j) {
                if (w[j] == id) {
                    first = n == 0 ? k : first;
                    ++n;
                    break;
                }
            }
        }
        return n;
    };
    bool ok = true;
    for (size_t k = 0; k < at.size() && ok && !ids.empty(); ++k) {
        const size_t i = at[k];
        if ((w[i] & 0xFFFF) != 61 || (w[i] >> 16) < 4 || !ids.contains(w[i + 3])) { // OpLoad <type> <result> <pointer>
            continue;
        }
        ok = functions == 1 && branches == 0;
        const u32 value = w[i + 2];
        size_t use = 0;
        if (!ok || uses(value, k + 1, use) != 1 || (w[at[use]] & 0xFFFF) != 62 || w[at[use] + 2] != value) {
            ok = false; // (not one OpStore <variable> <value>)
            break;
        }
        const u32 var = w[at[use] + 1];
        size_t next = 0;
        ok = uses(var, use + 1, next) != 0 && (w[at[next]] & 0xFFFF) == 62 && w[at[next] + 1] == var; // written again
    }
    it->second = ok;
    return ok;
}

namespace {

/// SSPC_CITY_INSTANCING: the largest constant block a draw of a multi-draw has (its copy is taken per draw)
constexpr u64 kInstMaxBytes = 4096;

/// The user data registers a descriptor is read from (bit i: register i; past the registers: none).
u32 SharpSlots(const GS::SharpLocation& loc) {
    u32 m = 0;
    for (u32 i = 0; i < 4; ++i) {
        u32 o = 16;
        if (loc.summary == GS::SharpLocation::Summary::SingleLoad) {
            o = loc.offsets[0] + i;
        } else if (loc.summary == GS::SharpLocation::Summary::MultiLoad && ((loc.load_mask >> i) & 1) != 0) {
            o = loc.offsets[i];
        }
        m |= o < 16 ? 1u << o : 0u;
    }
    return m;
}

/// The stride class of a constant block of `bytes` (a few patched variants per stage, not one per size).
u32 InstStride(u64 bytes) {
    return bytes <= 256 ? 256u : bytes <= 1024 ? 1024u : 4096u;
}

} // namespace

bool Executor::InstancingOn() {
    if (inst_on_ < 0) {
        char v[16] = {};
        const std::string_view sv(v, GetEnvironmentVariableA("SSPC_CITY_INSTANCING", v, sizeof(v)) < sizeof(v)
                                         ? std::strlen(v)
                                         : 0);
        inst_on_ = sv == "1" || sv.starts_with("ab:") ? 1 : 0;
        inst_ab_ = sv.starts_with("ab:") ? std::max<u32>(1, static_cast<u32>(std::strtoul(v + 3, nullptr, 10))) : 0;
        if (inst_on_ == 1 && (!ctx_.Caps().multi_draw || Fn().vkCmdDrawMultiIndexedEXT == nullptr)) {
            SSPC_LOG(GPU, Warning, "SSPC_CITY_INSTANCING=1: the device has no VK_EXT_multi_draw with shaderDrawParameters "
                                   "- off");
            inst_on_ = 0;
        } else if (inst_on_ == 1) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_INSTANCING={}: batches of draws differing in VS constants too, as one "
                                "multi-draw (each draw's constants at DrawIndex * stride)", sv);
        }
    }
    // =ab:<n>: in odd blocks of n flips only (image A/B in one run)
    return inst_on_ == 1 && (inst_ab_ == 0 || (VideoOut::NextFlipId() / inst_ab_) % 2 == 1);
}

bool Executor::InstanceBufferOk(const GS::StageView& vs, size_t buffer) {
    const u64 key = (u64{vs.module->id} << 8) | buffer;
    if (const auto it = inst_buffer_ok_.find(key); it != inst_buffer_ok_.end()) {
        return it->second;
    }
    bool& ok = inst_buffer_ok_[key];
    ok = false;
    const auto no = [&](std::string_view why) {
        if (inst_buffer_ok_.size() <= 400) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_INSTANCING: VS {:#x} (module {}) buffer {} not instanced: {}", vs.address,
                     vs.module->id, buffer, why);
        }
        return false;
    };
    const GS::ShaderReflection& r = vs.module->result.reflection;
    const std::vector<u32>& w = vs.module->result.spirv;
    if (buffer >= r.buffers.size() || r.buffers[buffer].kind != GS::BufferKind::Guest || w.size() < 5 ||
        vs.plan.buffers.size() != r.buffers.size()) {
        return no("not a guest buffer of the module's reflection");
    }
    // the patch itself (the stage reads the buffer through the translator's buf_offsets extracts only)
    std::vector<u32> patched;
    std::string why;
    const InstancedBuffer ib{static_cast<u32>(vs.start.buffer + buffer), 256};
    if (!PatchDrawIndexOffsets(w, std::span(&ib, 1), patched, why)) {
        ++stats_.inst_patch_refused;
        return no(why);
    }
    // the V#'s words in the flattened user data (the batch binds the first draw's): read only as the stride
    // (bitfieldExtract(word 1, 16, 14), the same in every draw), never at an index not known here
    const u32 slots = SharpSlots(r.buffers[buffer].sharp);
    const GS::SharpLocation& loc = r.buffers[buffer].sharp;
    const u32 stride_word = loc.summary == GS::SharpLocation::Summary::SingleLoad ? loc.offsets[0] + 1u
                            : ((loc.load_mask >> 1) & 1) != 0                    ? loc.offsets[1]
                                                                                 : 16u;
    u32 flat_binding = ~0u;
    for (const GS::BufferBinding& b : vs.plan.buffers) {
        flat_binding = b.kind == GS::BufferKind::Flatbuf ? b.binding : flat_binding;
    }
    if (flat_binding == ~0u) {
        ok = true; // (no flattened user data: the V#'s words are not read at all)
        return true;
    }
    std::vector<size_t> at;
    for (size_t i = 5; i < w.size();) {
        const u32 n = w[i] >> 16;
        if (n == 0 || i + n > w.size()) {
            return false;
        }
        at.push_back(i);
        i += n;
    }
    // uses of values are looked for in the function only (the globals' literals - capabilities, constants' values -
    // are not ids)
    std::vector<size_t> code;
    for (const size_t i : at) {
        if ((w[i] & 0xFFFF) == 54 || !code.empty()) { // from OpFunction on
            code.push_back(i);
        }
    }
    std::unordered_map<u32, u32> constants; // OpConstant (32-bit) -> value
    std::unordered_set<u32> flat_vars;
    for (const size_t i : at) {
        const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
        if (op == 43 && n == 4) {
            constants[w[i + 2]] = w[i + 3];
        } else if (op == 71 && n == 4 && w[i + 2] == 33 && w[i + 3] == flat_binding) { // OpDecorate Binding
            flat_vars.insert(w[i + 1]);
        }
    }
    if (flat_vars.size() != 1) {
        return no("no single variable of the flattened user data");
    }
    const u32 flat_var = *flat_vars.begin();
    std::unordered_map<u32, u32> word_ptrs; // a pointer to flat[c] for c among the V#'s words -> c
    for (const size_t i : code) {
        const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
        for (size_t j = i + 1; j < i + n; ++j) {
            if (w[j] != flat_var || (op == 59 && j == i + 2) || op == 71 || op == 72 || op == 5 || op == 6) {
                continue; // (its own OpVariable and decorations)
            }
            if (op == 15) {
                continue; // (the entry point's interface)
            }
            // only OpAccessChain %ptr %flat %0 %const (the translator's FlatDword); anything else: not proven
            if ((op != 65 && op != 66) || j != i + 3 || n != 6) {
                return no(fmt::format("the flattened user data used by op {}", op));
            }
            const auto c = constants.find(w[i + 5]);
            if (c == constants.end()) {
                return no("the flattened user data at an index not known"); // (it could be one of the V#'s words)
            }
            if (c->second < 16 && ((slots >> c->second) & 1) != 0) {
                word_ptrs[w[i + 2]] = c->second;
            }
        }
    }
    // each such word: loaded, the value only extracted at bits 16..29 of word 1 - or parked in a register variable
    // nothing reads (the translator copies every user SGPR at the entry; a variable only stored to is dead)
    std::unordered_set<u32> dead_vars;
    {
        std::unordered_map<u32, bool> fvars; // Function variable -> read somewhere
        for (const size_t i : code) {
            if ((w[i] & 0xFFFF) == 59 && (w[i] >> 16) >= 4 && w[i + 3] == 7) { // OpVariable Function
                fvars[w[i + 2]] = false;
            }
        }
        for (const size_t i : code) {
            const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
            for (size_t j = i + 1; j < i + n; ++j) {
                const auto v = fvars.find(w[j]);
                if (v == fvars.end() || (op == 59 && j == i + 2) || (op == 62 && j == i + 1) || op == 5) {
                    continue; // (its definition, a store into it, its name)
                }
                v->second = true;
            }
        }
        for (const auto& [v, read] : fvars) {
            if (!read) {
                dead_vars.insert(v);
            }
        }
    }
    std::unordered_set<u32> loads, dead_only;
    for (const size_t i : code) {
        const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
        for (size_t j = i + 1; j < i + n; ++j) {
            const auto p = word_ptrs.find(w[j]);
            if (p == word_ptrs.end() || (j == i + 2 && (op == 65 || op == 66))) {
                continue; // (the pointer's own definition)
            }
            if (op != 61 || j != i + 3) {
                return no(fmt::format("the V#'s word {} used by op {}", p->second, op));
            }
            loads.insert(w[i + 2]);
            if (p->second != stride_word) {
                dead_only.insert(w[i + 2]); // (another word of the V#: only into a dead variable)
            }
        }
    }
    for (const size_t i : code) {
        const u32 op = w[i] & 0xFFFF, n = w[i] >> 16;
        for (size_t j = i + 1; j < i + n; ++j) {
            if (!loads.contains(w[j]) || (op == 61 && j == i + 2)) {
                continue;
            }
            if (op == 62 && j == i + 2 && dead_vars.contains(w[i + 1])) {
                continue; // OpStore %dead %value
            }
            const auto off = constants.find(n == 6 ? w[i + 4] : 0);
            const auto cnt = constants.find(n == 6 ? w[i + 5] : 0);
            if (dead_only.contains(w[j]) || op != 203 || j != i + 3 || off == constants.end() || cnt == constants.end() ||
                off->second != 16 || cnt->second != 14) {
                return no(fmt::format("a word of the V# used by op {}", op));
            }
        }
    }
    ok = true;
    return true;
}

Executor::InstVariant* Executor::InstancedVariant(const GS::Module& vs) {
    u64 key = vs.id;
    for (const InstancedBuffer& b : inst_bufs_) {
        key = (key ^ (u64{b.index} << 40) ^ (u64{b.stride} << 16)) * 0x100000001B3ull;
    }
    auto [it, fresh] = inst_variants_.try_emplace(key);
    InstVariant& v = it->second;
    if (fresh) {
        std::vector<u32> patched;
        std::string why;
        if (!PatchDrawIndexOffsets(vs.result.spirv, inst_bufs_, patched, why)) {
            ++stats_.inst_patch_refused;
            SSPC_LOG(GPU, Warning, "SSPC_CITY_INSTANCING: module {} not patched: {}", vs.id, why);
            return &v; // (module stays nullptr: never again)
        }
        auto m = std::make_unique<GS::Module>();
        m->id = inst_next_module_++; // (ids of the translated modules count from 1: no clash)
        m->key = vs.key;
        m->keyed = true;
        m->result = vs.result;
        m->result.spirv = std::move(patched);
        m->runtime = vs.runtime;
        m->start = vs.start;
        m->prepared = true;
        m->spirv_hash = GS::HashSpirv(m->result.spirv);
        m->state.store(GS::ModuleState::Ready, std::memory_order_release);
        if (char d[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_INSTANCING_DUMP", d, sizeof(d)) == 1 && d[0] == '1') {
            // (diagnostics: the original and the patched stage, for spirv-val / spirv-dis)
            const auto dir = options_.capture_path.parent_path();
            for (const auto& [name, code] : {std::pair<const char*, const std::vector<u32>*>{"orig", &vs.result.spirv},
                                     std::pair<const char*, const std::vector<u32>*>{"inst", &m->result.spirv}}) {
                std::ofstream(dir / fmt::format("instancing_{}_{}_{}.spv", vs.id, m->id, name), std::ios::binary)
                    .write(reinterpret_cast<const char*>(code->data()), static_cast<std::streamsize>(code->size() * 4));
            }
        }
        v.module = std::move(m);
    }
    return &v;
}

const GS::Module* Executor::PsortVariant(const GS::Module& cs, u64 address) {
    if (psort_on_ < 0) {
        char v[4] = {};
        psort_on_ = GetEnvironmentVariableA("SSPC_R12_PSORT", v, sizeof(v)) == 1 && v[0] == '1' ? 1 : 0;
        SSPC_LOG(GPU, Info, "SSPC_R12_PSORT: {}", psort_on_ != 0 ? "on (cs 0x2edaa0c00's ranking loop without the exec-mask emulation)" : "off");
        if (psort_on_ == 0) {
            return nullptr;
        }
    }
    const std::vector<u32>& spirv = cs.result.spirv;
    if (spirv.size() != kR12PsortOrigWords) {
        return nullptr; // (most programs: one compare)
    }
    auto [it, fresh] = psort_variants_.try_emplace(cs.id);
    if (fresh) {
        u64 h = 1469598103934665603ull; // FNV-1a over the SPIR-V's bytes (as tools/r12/psort_patch.py)
        for (const u32 w : spirv) {
            for (u32 b = 0; b < 4; ++b) {
                h = (h ^ ((w >> (b * 8)) & 0xFFu)) * 1099511628211ull;
            }
        }
        if (h != kR12PsortOrigFnv) {
            SSPC_LOG(GPU, Info, "SSPC_R12_PSORT: cs {:#x} module {}: another SPIR-V ({:016x}), used as it is",
                     address, cs.id, h);
            return nullptr;
        }
        auto m = std::make_unique<GS::Module>();
        m->id = psort_next_module_++; // (>= 0x40000000: transient, not in the precompile manifest)
        m->key = cs.key;
        m->keyed = cs.keyed;
        m->result = cs.result;
        m->result.spirv.assign(std::begin(kR12PsortSpirv), std::end(kR12PsortSpirv));
        m->runtime = cs.runtime;
        m->start = cs.start;
        m->prepared = true;
        m->spirv_hash = GS::HashSpirv(m->result.spirv);
        m->state.store(GS::ModuleState::Ready, std::memory_order_release);
        SSPC_LOG(GPU, Info, "SSPC_R12_PSORT: cs {:#x} module {} -> variant {} ({} -> {} SPIR-V words)", address,
                 cs.id, m->id, spirv.size(), m->result.spirv.size());
        it->second = std::move(m);
    }
    return it->second.get();
}

bool Executor::GatherInstanced(u64 align) {
    inst_bind_.clear();
    const u64 completed = timeline_.CompletedCached();
    const u32 n = static_cast<u32>(batch_.size());
    for (const InstancedBuffer& ib : inst_bufs_) {
        const u64 bytes = u64{ib.stride} * n;
        StreamRing::Slice sl{};
        if (ring_.Allocate(bytes, align, cur_.value, sl) != StreamRing::Status::Ok) {
            return false;
        }
        for (u32 k = 0; k < n; ++k) {
            const GS::StageView& v = *batch_[k].prepared.stage;
            const GS::BufferBinding& b = v.plan.buffers[ib.index - v.start.buffer];
            u8* dst = sl.data + u64{k} * ib.stride;
            if (b.size > ib.stride || buffers_.HostWritePending(b.address, b.size) ||
                TimelineWritePending(b.address, b.size) || buffers_.GpuWritePending(b.address, b.size, completed) ||
                buffers_.KeptBack(b.address, b.size)) {
                return false; // (the bytes on the host are not the current ones)
            }
            const u8* src = guest_.BackingPointer(b.address, b.size);
            if (src == nullptr) {
                return false;
            }
            std::memcpy(dst, src, b.size);
            std::memset(dst + b.size, 0, ib.stride - b.size); // (past the block: zeros, as a robust read past it)
        }
        inst_bind_.push_back({ib.index, VkDescriptorBufferInfo{sl.buffer, sl.offset, bytes}});
    }
    return true;
}

bool Executor::Batchable(const Cp::DrawDescriptor& a, const PreparedCommand& pa, const Cp::DrawDescriptor& b,
                         const PreparedCommand& pb) {
    using G = Cp::Group;
    const auto Reject = [&](size_t why) {
        last_batch_reject_ = static_cast<u8>(why);
        ++stats_.batch_rejects[std::min(why, stats_.batch_rejects.size() - 1)];
        return false;
    };
    // the registers: between the two draws only the VS user data, the index buffer and the draw's own parameters
    constexpr Cp::GroupMask kGeometryGroups = (Cp::GroupMask{1} << static_cast<u32>(G::VsUserData)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::IndexState)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::DrawParams));
    if ((b.changed & ~kGeometryGroups) != 0) {
        return Reject(2);
    }
    // SSPC_CITY_INSTANCING: draws of several instances too - as one multi-draw each draw keeps its instances together
    // (one draw as one would draw instance by instance: the primitives in another order)
    const bool multi = InstancingOn();
    const auto plain = [multi](const Cp::DrawDescriptor& d) {
        // a direct, indexed, single-instance triangle list of VS + PS (lists: one after another they stay lists)
        return d.indexed && !d.indirect && (d.instance_count <= 1 || multi) && d.primitive_type == 4 && d.index_count != 0 &&
               d.index_count % 3 == 0 && d.index_count <= d.max_index_size && (d.index_type & 3) <= 1 &&
               d.active_stages == ((1u << static_cast<u32>(Cp::Stage::Vs)) | (1u << static_cast<u32>(Cp::Stage::Ps)));
    };
    if (!plain(a) || !plain(b)) {
        const Cp::DrawDescriptor& x = plain(a) ? b : a;
        ++stats_.batch_plain[!x.indexed                               ? 0
                             : x.indirect                             ? 1
                             : x.instance_count > 1                   ? 2
                             : x.primitive_type != 4                  ? 3
                             : x.index_count == 0 || x.index_count % 3 ? 4
                             : x.index_count > x.max_index_size       ? 5
                             : (x.index_type & 3) > 1                 ? 6
                                                                      : 7];
        return Reject(1);
    }
    if (a.queue != b.queue || a.index_type != b.index_type || a.cb_extent != b.cb_extent || a.db_extent != b.db_extent ||
        (!multi && std::max<u32>(a.instance_count, 1) != std::max<u32>(b.instance_count, 1))) {
        return Reject(15);
    }
    if (!std::equal(a.shaders.begin(), a.shaders.end(), b.shaders.begin(),
                    [](const Cp::ShaderRef& x, const Cp::ShaderRef& y) {
                        return x.address == y.address && x.rsrc1 == y.rsrc1 && x.rsrc2 == y.rsrc2;
                    }) ||
        a.user_data[static_cast<size_t>(Cp::Stage::Ps)] != b.user_data[static_cast<size_t>(Cp::Stage::Ps)]) {
        return Reject(3);
    }
    if (pa.prep == nullptr || pb.prep == nullptr || !pa.prep->ready || !pb.prep->ready ||
        pa.prep->pipeline != pb.prep->pipeline || pa.state == nullptr || pa.state != pb.state || pa.stage == nullptr ||
        pb.stage == nullptr || pa.ps == nullptr || pb.ps == nullptr) {
        return Reject(4);
    }
    const GS::StageView& va = *pa.stage;
    const GS::StageView& vb = *pb.stage;
    const GS::StageView& qa = *pa.ps;
    const GS::StageView& qb = *pb.ps;
    if (va.module == nullptr || va.module != vb.module || qa.module == nullptr || qa.module != qb.module ||
        !(va.start == vb.start) || !(va.end == vb.end) || !(qa.start == qb.start) || !(qa.end == qb.end)) {
        return Reject(5);
    }
    const auto same_buffers = [](const GS::ResourceBindingPlan& x, const GS::ResourceBindingPlan& y) {
        return x.buffers.size() == y.buffers.size() &&
               std::equal(x.buffers.begin(), x.buffers.end(), y.buffers.begin(), [](const auto& p, const auto& q) {
                   return p.binding == q.binding && p.kind == q.kind && p.desc.raw == q.desc.raw &&
                          p.address == q.address && p.size == q.size && !p.written && !q.written &&
                          p.formatted == q.formatted && p.status == q.status;
               });
    };
    const auto same_images = [](const GS::ResourceBindingPlan& x, const GS::ResourceBindingPlan& y) {
        return x.images.size() == y.images.size() &&
               std::equal(x.images.begin(), x.images.end(), y.images.begin(), [](const auto& p, const auto& q) {
                   return p.binding == q.binding && p.count == q.count && p.desc.raw == q.desc.raw &&
                          p.view_type == q.view_type && !p.storage && !q.storage && p.depth == q.depth &&
                          p.array == q.array && p.fmask == q.fmask && p.status == q.status;
               }) &&
               x.samplers.size() == y.samplers.size() &&
               std::equal(x.samplers.begin(), x.samplers.end(), y.samplers.begin(), [](const auto& p, const auto& q) {
                   return p.binding == q.binding && p.desc.raw == q.desc.raw && p.status == q.status;
               });
    };
    // the flattened user data: the walk's copies the same, the registers the shader reads the same
    u32 inst_slots = 0; // SSPC_CITY_INSTANCING: the user data of the V#s of the VS buffers that differ
    const auto same_flat = [&inst_slots](const GS::StageView& x, const GS::StageView& y) {
        const std::vector<u32>& f = x.plan.flat;
        const std::vector<u32>& g = y.plan.flat;
        if (f.size() != g.size() || f.size() < 16 ||
            !std::equal(f.begin() + 16, f.end(), g.begin() + 16)) {
            return false;
        }
        // (the vertex buffer table's and the fetch shader's pairs: the fetch shader alone reads them - the translated
        // stage takes its vertices as vertex inputs, from each draw's own ranges)
        const GS::FetchShader& fs = x.module->result.reflection.fetch;
        u32 fetch_only = 0;
        if (fs.present) {
            fetch_only |= 3u << fs.sgpr_base;
            for (const GS::FetchAttribute& at : fs.attributes) {
                fetch_only |= 3u << at.sgpr_base;
            }
        }
        const u16 used = static_cast<u16>(x.module->result.reflection.user_data_used & ~fetch_only & ~inst_slots);
        for (u32 i = 0; i < 16; ++i) {
            if (((used >> i) & 1) != 0 && f[i] != g[i]) {
                return false;
            }
        }
        return true;
    };
    if (!same_buffers(va.plan, vb.plan)) {
        // SSPC_CITY_INSTANCING: VS constant buffers that differ only in where their bytes are (and how many) - each
        // draw reads its own copy at DrawIndex * stride (vk_instancing.h)
        if (!InstancingOn() || va.plan.buffers.size() != vb.plan.buffers.size()) {
            return Reject(6);
        }
        const GS::ShaderReflection& mr = va.module->result.reflection;
        u32 differ = 0;
        for (size_t i = 0; i < va.plan.buffers.size(); ++i) {
            const GS::BufferBinding& x = va.plan.buffers[i];
            const GS::BufferBinding& y = vb.plan.buffers[i];
            if (x.binding == y.binding && x.kind == y.kind && x.desc.raw == y.desc.raw && x.address == y.address &&
                x.size == y.size && !x.written && !y.written && x.formatted == y.formatted && x.status == y.status) {
                continue;
            }
            if (x.binding != y.binding || x.kind != GS::BufferKind::Guest || y.kind != GS::BufferKind::Guest ||
                x.written || y.written || x.formatted != y.formatted || x.status != GS::ResourceStatus::Ok ||
                y.status != GS::ResourceStatus::Ok || (x.desc.raw[1] >> 16) != (y.desc.raw[1] >> 16) ||
                x.desc.raw[3] != y.desc.raw[3] || x.size > kInstMaxBytes || y.size > kInstMaxBytes ||
                (x.address & 3) != 0 || (y.address & 3) != 0 || i >= mr.buffers.size() || ++differ > 4 ||
                !InstanceBufferOk(va, i)) {
                return Reject(14);
            }
            inst_slots |= SharpSlots(mr.buffers[i].sharp);
        }
    }
    if (!same_images(va.plan, vb.plan)) {
        return Reject(7);
    }
    if (!same_flat(va, vb)) {
        return Reject(12);
    }
    if (!same_buffers(qa.plan, qb.plan) || !same_images(qa.plan, qb.plan) || qa.plan.flat != qb.plan.flat) {
        return Reject(13);
    }
    // the vertex inputs: the same attributes, formats and strides (their own ranges)
    if (va.plan.vertices.empty() || va.plan.vertices.size() != vb.plan.vertices.size()) {
        return Reject(8);
    }
    for (size_t i = 0; i < va.plan.vertices.size(); ++i) {
        const GS::VertexBinding& x = va.plan.vertices[i];
        const GS::VertexBinding& y = vb.plan.vertices[i];
        if (x.attribute != y.attribute || !(x.fetch == y.fetch) || x.fetch.instance_data != 0 ||
            x.desc.raw[1] >> 16 != y.desc.raw[1] >> 16 || x.desc.raw[3] != y.desc.raw[3] || x.desc.Stride() == 0 ||
            x.status != GS::ResourceStatus::Ok || y.status != GS::ResourceStatus::Ok || y.address == 0 || y.size == 0) {
            return Reject(9);
        }
    }
    return BatchModuleOk(va.module, true) ? (BatchModuleOk(qa.module, false) ? true : Reject(11)) : Reject(10);
}

bool Executor::BatchGeometry(const GS::StageView& vs, std::array<VkBuffer, 16>& vbufs,
                             std::array<VkDeviceSize, 16>& voffs, VkBuffer& ibuf, u64& ioff, u32& count) {
    const size_t streams = vs.plan.vertices.size();
    if (streams == 0 || streams > vbufs.size()) {
        return false;
    }
    const GS::FetchShader& fetch = vs.module->result.reflection.fetch;
    // the vertex offset a draw adds to its indices (as RecordDraw: VGT_INDX_OFFSET or the fetch shader's)
    const auto vertex_offset = [&](const Cp::DrawDescriptor& d) {
        u32 vo = d.vgt_index_offset;
        const auto& ud = d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
        if (fetch.present && vo == 0 && fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
            vo = ud[fetch.vertex_offset_sgpr];
        }
        return vo;
    };
    // the batch's composition (the persistent copy's key): strides, every draw's vertex ranges, index range and
    // vertex offset - the bytes behind them are versioned by the buffer cache
    std::vector<u64>& key = geo_key_;
    key.clear();
    key.push_back(streams);
    for (size_t s = 0; s < streams; ++s) {
        key.push_back(vs.plan.vertices[s].desc.Stride());
    }
    u64 indices = 0, slots = 0;
    for (const BatchDraw& b : batch_) {
        const Cp::DrawDescriptor& d = *b.draw;
        const GS::StageView& v = *b.prepared.stage;
        u64 most = 0;
        for (size_t s = 0; s < streams; ++s) {
            const GS::VertexBinding& vb = v.plan.vertices[s];
            key.push_back(vb.address);
            key.push_back(vb.size);
            most = std::max<u64>(most, (vb.size + vb.desc.Stride() - 1) / vb.desc.Stride());
        }
        key.push_back(d.index_base + u64{d.index_offset} * ((d.index_type & 3) == 1 ? 4 : 2));
        key.push_back((u64{d.index_count} << 2) | (d.index_type & 3));
        key.push_back(vertex_offset(d));
        slots += most;
        indices += d.index_count;
    }
    if (slots > 0xFFFFFFF0ull || indices > (1u << 24)) {
        return false;
    }
    u64 h = 0xCBF29CE484222325ull;
    for (const u64 w : key) {
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    const u64 completed = timeline_.CompletedCached();
    // SSPC_CITY_GEOMETRY_CACHE=1: the batch's geometry built once into video memory and drawn again while every source
    // range is unchanged (the versions of its blocks in the buffer cache) - no copy, no index rebase
    if (geo_on_ < 0) {
        char v[4] = {};
        geo_on_ = GetEnvironmentVariableA("SSPC_CITY_GEOMETRY_CACHE", v, sizeof(v)) == 1 && v[0] == '1';
        if (char mb[16] = {}; GetEnvironmentVariableA("SSPC_CITY_GEOMETRY_CACHE_MB", mb, sizeof(mb)) > 0) {
            geo_arena_bytes_ = std::clamp<u64>(std::strtoull(mb, nullptr, 10), 4, 512) << 20;
        }
        if (geo_on_ == 1) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_GEOMETRY_CACHE=1: batch geometry kept in video memory (2 x {} MiB)",
                     geo_arena_bytes_ >> 20);
        }
    }
    if (geo_on_ == 1) {
        if (const auto it = geo_cache_.find(h); it != geo_cache_.end()) {
            GeoEntry& e = it->second;
            bool same = e.key == key;
            for (size_t i = 0; i < e.sources.size() && same; ++i) {
                same = buffers_.RangeVersion(e.sources[i].first, e.sources[i].second) == e.versions[i];
            }
            if (same) {
                for (size_t s = 0; s < streams; ++s) {
                    vbufs[s] = geo_arena_[e.arena].buffer;
                    voffs[s] = e.vb_off[s];
                }
                ibuf = geo_arena_[e.arena].buffer;
                ioff = e.ib_off;
                count = e.count;
                geo_arena_seq_[e.arena] = std::max(geo_arena_seq_[e.arena], cur_.value);
                ++stats_.geo_hits;
                return true;
            }
            ++stats_.geo_changed;
            geo_cache_.erase(it);
        }
    }
    // the bytes: the host's copy, when it is current (no write of the GPU or of the timeline pending, none kept back)
    const auto host = [&](u64 addr, u64 size) -> const u8* {
        if (size == 0 || size > 16 * 1024 || buffers_.HostWritePending(addr, size) || TimelineWritePending(addr, size) ||
            buffers_.GpuWritePending(addr, size, completed) || buffers_.KeptBack(addr, size)) {
            return nullptr;
        }
        return guest_.BackingPointer(addr, size);
    };
    std::array<u64, 16> stream_bytes{};
    u64 total = 0;
    for (size_t s = 0; s < streams; ++s) {
        stream_bytes[s] = (slots * vs.plan.vertices[s].desc.Stride() + 15) & ~15ull;
        total += stream_bytes[s];
    }
    total += indices * 4;
    // kept in an arena when every source range is current in video memory (write-tracked: versioned), else this
    // batch's stream ring
    std::vector<std::pair<u64, u64>>& sources = geo_sources_;
    std::vector<u32>& versions = geo_versions_;
    sources.clear();
    versions.clear();
    bool persistent = geo_on_ == 1 && total <= geo_arena_bytes_ / 4;
    for (size_t k = 0; k < batch_.size() && persistent; ++k) {
        const Cp::DrawDescriptor& d = *batch_[k].draw;
        for (const GS::VertexBinding& vb : batch_[k].prepared.stage->plan.vertices) {
            sources.emplace_back(vb.address, vb.size);
        }
        const u64 isz = (d.index_type & 3) == 1 ? 4 : 2;
        sources.emplace_back(d.index_base + u64{d.index_offset} * isz, u64{d.index_count} * isz);
    }
    for (size_t i = 0; i < sources.size() && persistent; ++i) {
        const u32 v = buffers_.RangeVersion(sources[i].first, sources[i].second);
        persistent = v != 0;
        versions.push_back(v);
    }
    u8* dst = nullptr;
    VkBuffer dst_buffer = VK_NULL_HANDLE;
    u64 dst_offset = 0;
    u32 arena = 0;
    if (persistent && geo_arena_[0].buffer == VK_NULL_HANDLE) {
        std::string err;
        for (OwnedBuffer& a : geo_arena_) {
            if (!CreateBuffer(ctx_, geo_arena_bytes_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              true, a, err, false, HostPlacement::Bar)) {
                SSPC_LOG(GPU, Error, "SSPC_CITY_GEOMETRY_CACHE: an arena of {} MiB: {} - off", geo_arena_bytes_ >> 20, err);
                geo_on_ = 0;
                persistent = false;
                break;
            }
        }
    }
    if (persistent) {
        u32 a = geo_cur_;
        if (geo_used_[a] + total > geo_arena_bytes_ && geo_arena_seq_[a ^ 1] <= completed) {
            // the other arena takes over once the GPU finished every draw that read it: its entries go (LRU by halves)
            a ^= 1;
            std::erase_if(geo_cache_, [&](const auto& e) { return e.second.arena == a; });
            ++stats_.geo_recycles;
            geo_used_[a] = 0;
            geo_cur_ = a;
        }
        if (geo_used_[a] + total <= geo_arena_bytes_) {
            arena = a;
            dst_buffer = geo_arena_[a].buffer;
            dst_offset = geo_used_[a];
            dst = geo_arena_[a].mapped + dst_offset;
            geo_used_[a] += (total + 255) & ~255ull;
        }
    }
    StreamRing::Slice ring{};
    if (dst == nullptr) {
        persistent = false;
        if (ring_.Allocate(total, 16, cur_.value, ring) != StreamRing::Status::Ok) {
            return false;
        }
        dst = ring.data;
        dst_buffer = ring.buffer;
        dst_offset = ring.offset;
    }
    // the vertices of each draw one after another in each stream, the indices moved to them (32-bit)
    std::array<u64, 16> soff{};
    u64 at = 0;
    for (size_t s = 0; s < streams; ++s) {
        soff[s] = at;
        at += stream_bytes[s];
    }
    u32* out = reinterpret_cast<u32*>(dst + at);
    u64 base = 0;
    for (const BatchDraw& b : batch_) {
        const Cp::DrawDescriptor& d = *b.draw;
        const GS::StageView& v = *b.prepared.stage;
        u64 most = 0, least = ~0ull;
        for (size_t s = 0; s < streams; ++s) {
            const GS::VertexBinding& vb = v.plan.vertices[s];
            const u8* src = host(vb.address, vb.size);
            if (src == nullptr) {
                return false; // (bytes taken in an arena stay unused until it is recycled)
            }
            const u32 stride = vb.desc.Stride();
            std::memcpy(dst + soff[s] + base * stride, src, vb.size);
            most = std::max<u64>(most, (vb.size + stride - 1) / stride);
            least = std::min<u64>(least, vb.size / stride);
        }
        const u32 vo = vertex_offset(d);
        const u64 isize = (d.index_type & 3) == 1 ? 4 : 2;
        const u8* src = host(d.index_base + u64{d.index_offset} * isize, u64{d.index_count} * isize);
        if (src == nullptr) {
            return false;
        }
        for (u32 k = 0; k < d.index_count; ++k) {
            u32 ix = 0;
            std::memcpy(&ix, src + k * isize, isize);
            const u64 vtx = u64{ix} + vo;
            if (vtx >= least) {
                return false; // past its own vertices (robustness reads zeros there; in a batch, another draw's)
            }
            *out++ = static_cast<u32>(base + vtx);
        }
        base += most;
    }
    for (size_t s = 0; s < streams; ++s) {
        vbufs[s] = dst_buffer;
        voffs[s] = dst_offset + soff[s];
    }
    ibuf = dst_buffer;
    ioff = dst_offset + at;
    count = static_cast<u32>(indices);
    stats_.geo_built_bytes += total;
    ++stats_.geo_builds;
    if (persistent) {
        GeoEntry& e = geo_cache_[h];
        e.key = key;
        e.sources = sources;
        e.versions = versions;
        e.arena = arena;
        for (size_t s = 0; s < streams; ++s) {
            e.vb_off[s] = voffs[s];
        }
        e.ib_off = ioff;
        e.count = count;
        geo_arena_seq_[arena] = std::max(geo_arena_seq_[arena], cur_.value);
        ++stats_.geo_stored;
    }
    return true;
}

bool Executor::ZeroCopyBatch(const GS::StageView& vs, std::array<VkBuffer, 16>& vbufs,
                             std::array<VkDeviceSize, 16>& voffs, VkBuffer& ibuf, u64& ioff) {
    // every draw's vertex and index ranges in video memory (the buffer cache's own copy, write-tracked) in one buffer
    // per stream: bound once at the lowest offset, each draw's own place given by its vertexOffset / firstIndex (its
    // indices as they are) - nothing copied. Vertices as the draw alone reads them: its binding + (index + its vertex
    // offset) * stride.
    const size_t streams = vs.plan.vertices.size();
    if (streams == 0 || streams > vbufs.size()) {
        return false;
    }
    const GS::FetchShader& fetch = vs.module->result.reflection.fetch;
    const u64 isize = (batch_[0].draw->index_type & 3) == 1 ? 4 : 2;
    zc_voff_.assign(batch_.size() * streams, 0);
    zc_ioff_.assign(batch_.size(), 0);
    VkBuffer ib = VK_NULL_HANDLE;
    std::array<VkBuffer, 16> vb{};
    std::array<u64, 16> vlow{};
    vlow.fill(~0ull);
    u64 ilow = ~0ull;
    for (size_t k = 0; k < batch_.size(); ++k) {
        const Cp::DrawDescriptor& d = *batch_[k].draw;
        const GS::StageView& v = *batch_[k].prepared.stage;
        for (size_t s = 0; s < streams; ++s) {
            const GS::VertexBinding& x = v.plan.vertices[s];
            BufferCache::Range r;
            if (buffers_.Classify(x.address, x.size, r) != BufferCache::Residency::All ||
                (vb[s] != VK_NULL_HANDLE && vb[s] != r.buffer)) {
                return false;
            }
            if (aq_note_reads_) {
                AqNote(x.address, x.size);
            }
            vb[s] = r.buffer;
            zc_voff_[k * streams + s] = r.offset;
            vlow[s] = std::min(vlow[s], r.offset);
        }
        BufferCache::Range r;
        if (buffers_.Classify(d.index_base + u64{d.index_offset} * isize, u64{d.index_count} * isize, r) !=
                BufferCache::Residency::All ||
            (ib != VK_NULL_HANDLE && ib != r.buffer) || r.offset % isize != 0) {
            return false;
        }
        if (aq_note_reads_) {
            AqNote(d.index_base + u64{d.index_offset} * isize, u64{d.index_count} * isize);
        }
        ib = r.buffer;
        zc_ioff_[k] = r.offset;
        ilow = std::min(ilow, r.offset);
    }
    // each draw's place: the same number of vertices from the lowest binding in every stream
    inst_draws_.clear();
    for (size_t k = 0; k < batch_.size(); ++k) {
        const Cp::DrawDescriptor& d = *batch_[k].draw;
        const auto& ud = d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
        u32 vo = d.vgt_index_offset;
        if (fetch.present && vo == 0 && fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
            vo = ud[fetch.vertex_offset_sgpr];
        }
        u64 q = ~0ull;
        for (size_t s = 0; s < streams; ++s) {
            const u32 stride = batch_[k].prepared.stage->plan.vertices[s].desc.Stride();
            const u64 delta = zc_voff_[k * streams + s] - vlow[s];
            if (stride == 0 || delta % stride != 0 || (q != ~0ull && q != delta / stride)) {
                return false;
            }
            q = delta / stride;
        }
        const u64 first = (zc_ioff_[k] - ilow) / isize;
        const u64 voff = q + vo;
        if (first > 0xFFFFFFFFull || voff > 0x7FFFFFFFull) {
            return false;
        }
        inst_draws_.push_back({static_cast<u32>(first), d.index_count, static_cast<s32>(voff)});
    }
    for (size_t s = 0; s < streams; ++s) {
        vbufs[s] = vb[s];
        voffs[s] = vlow[s];
    }
    ibuf = ib;
    ioff = ilow;
    ++stats_.zc_batches;
    stats_.zc_draws += batch_.size();
    return true;
}

bool Executor::DrawPreparedBatch(std::span<const BatchDraw> draws) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_);
    batch_ = draws;
    batch_done_ = false;
    // SSPC_CITY_INSTANCING: the VS buffers that differ in the batch (Batchable let only constant blocks differ), each
    // at the stride class of its largest block
    inst_bufs_.clear();
    const GS::StageView& head = *draws[0].prepared.stage;
    for (size_t i = 0; i < head.plan.buffers.size(); ++i) {
        const GS::BufferBinding& h = head.plan.buffers[i];
        u64 most = h.size;
        bool differ = false;
        for (const BatchDraw& b : draws.subspan(1)) {
            const GS::BufferBinding& x = b.prepared.stage->plan.buffers[i];
            differ = differ || x.address != h.address || x.size != h.size || x.desc.raw != h.desc.raw;
            most = std::max(most, x.size);
        }
        if (differ) {
            inst_bufs_.push_back({static_cast<u32>(head.start.buffer + i), InstStride(most)});
        }
    }
    RecordDraw(*draws[0].draw, draws[0].prepared, t0);
    batch_ = {};
    inst_bind_.clear();
    if (!inst_bufs_.empty() && !batch_done_) {
        ++stats_.inst_fallbacks;
    }
    if (!batch_done_) {
        ++stats_.batch_fallbacks; // (the first draw recorded on its own)
        return false;
    }
    ++stats_.draw_batches;
    stats_.batched_draws += draws.size();
    if (!inst_bufs_.empty()) {
        ++stats_.inst_batches;
        stats_.inst_draws += draws.size();
    }
    for (const BatchDraw& b : draws.subspan(1)) {
        // what RecordDraw counts for each draw: its work recorded in this batch
        ++stats_.draws;
        ++stats_.draws_executed;
        ++stats_.indexed;
        ++trace_.ordinal;
        Note(b.draw->queue, b.draw->seq);
        ++commands_;
    }
    return true;
}

namespace {
/// A full-screen movie's vertex constants (0x90 bytes the game writes each frame, docs/RESOLUTION_RESEARCH.md): the
/// quad's corners (x, y, 0, 1) at +-1, their texture coordinates (u, v, 0, 0), a colour. The game fits the picture to
/// the target's width: on a target that is not 16:9 v runs past the picture's lines (narrower) or over a part of them
/// (wider). They are set to the whole picture, as on 16:9 (0 .. v_end); false: not that layout, nothing written.
bool FitMovieCoords(u64 address, u64 size, float v_end) {
    static constexpr float kCorners[16] = {1, 1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, -1, -1, 0, 1};
    if (size < 0x90) {
        return false;
    }
    float* c = reinterpret_cast<float*>(address);
    if (std::memcmp(c, kCorners, sizeof(kCorners)) != 0 || c[16] != 1 || c[20] != 0 || c[24] != 1 || c[28] != 0 ||
        c[17] != c[21] || c[25] != c[29] || c[17] >= c[25]) {
        return false;
    }
    if (c[17] != 0 || c[25] != v_end) {
        c[17] = c[21] = 0;
        c[25] = c[29] = v_end;
        Filesystem::NotifyHostWrite(address, 0x90);
    }
    return true;
}

/// R8 profile (SSPC_R8_CP_PROFILE): a 0x1a1770 draw's record by its stage (CpProfile::Sec::T2Gb*) - one section open at
/// a time, the last one left at the record's end (any return)
class GBufferRecordStages {
public:
    GBufferRecordStages() : on_(CpProfile::PartsSource()) { // (3: DrawRuns::Source::GBuffer; SSPC_R10_T2_PARTS)
        if (on_) {
            CpProfile::Enter(CpProfile::Sec::T2GbHead);
        }
    }
    ~GBufferRecordStages() {
        if (on_) {
            CpProfile::Leave();
        }
    }
    void Next(CpProfile::Sec s) {
        if (on_) {
            CpProfile::Leave();
            CpProfile::Enter(s);
        }
    }

private:
    bool on_;
};
} // namespace

void Executor::RecordDraw(const Cp::DrawDescriptor& d, const PreparedCommand& pc, u64 t0) {
    GBufferRecordStages gb;
    stream_pipe_ = 0;
    run_ok_ = false;
    ++stats_.draws;
    ++trace_.ordinal;
    const auto& f = Fn();
    const Cp::RegisterFile& regs = *regs_;
    const u32 mode = (regs.Get(kCbColorControl) >> 4) & 7;
    const bool filtered = DrawFiltered(regs, d);
    const GS::DrawPreparation* prepared = filtered ? nullptr : pc.prep;
    const u64 splits_before = stats_.aq_splits; // (R11 diagnostics)
    AqBoundary(d.queue);
    if (r11_diag_ != nullptr) {
        r11_diag_->draw_splits += stats_.aq_splits - splits_before;
    }
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
    const PipelineObjects* pipe = nullptr;
    // SSPC_CITY_INSTANCING: a batch whose draws differ in constant blocks - the patched stage's pipeline (a single
    // draw with it reads its own block: DrawIndex 0); not built yet - recorded draw by draw with the stage's own
    bool instanced = !batch_.empty() && !inst_bufs_.empty() && st != nullptr;
    if (instanced) {
        InstVariant* iv = InstancedVariant(*vs.module);
        instanced = iv->module != nullptr;
        if (instanced) {
            auto [pid, fresh_id] = iv->pipeline_ids.try_emplace(prep.pipeline, inst_next_pipeline_);
            inst_next_pipeline_ += fresh_id ? 1 : 0;
            std::string ierr;
            pipe = pipelines_.Graphics(pid->second, *st, iv->module.get(), vs.plan, has_ps ? ps.module : nullptr,
                                       has_ps ? &ps.plan : nullptr, ierr);
            instanced = pipe != nullptr;
            stats_.inst_no_pipeline += instanced ? 0 : 1;
        }
    }
    if (pipe == nullptr) {
        pipe = st == nullptr ? nullptr
                             : pipelines_.Graphics(prep.pipeline, *st, vs.module, vs.plan, has_ps ? ps.module : nullptr,
                                                   has_ps ? &ps.plan : nullptr, err);
    }
    if (pipe == nullptr) {
        Skipped(Skip::Pipeline, err.empty() ? "no pipeline state" : err);
        return;
    }
    stage(stats_.t_pipe);
    stream_pipe_ = prep.pipeline + 1;
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
    if (aq_markers_) {
        // R9 AQ diagnostic: a marker at each pipeline change of the draws - the previous group's commands completed
        // (bottom of pipe), this group's first draw started (top of pipe)
        const u64 akey = (u64{prep.pipeline} << 40) ^ vs.address ^ (has_ps ? ps.address << 1 : 0);
        if (akey != aq_mark_draw_key_) {
            aq_mark_draw_key_ = akey;
            const u32 id = aq_marker_next_++;
            AqMarker& e = aq_marker_ring_[id & (aq_marker_ring_.size() - 1)];
            e = {cur_.value, vs.address, 0xFFFF, {static_cast<u32>(prep.pipeline), 0, 0}, 0, false, has_ps ? ps.address : 0};
            f.vkCmdWriteBufferMarkerAMD(cur_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, aq_marker_buf_.buffer, 4, id - 1);
            f.vkCmdWriteBufferMarkerAMD(cur_.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, aq_marker_buf_.buffer, 0, id);
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
    gb.Next(CpProfile::Sec::T2GbTargets);
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
    UpscaleOnTargets(d, color_img, depth_img, dc); // (upscaling: the velocity pass, the display draw, jitter)
    cb_dump_flip_ = 0;
    if (up_ && jitter_draw_) {
        static const u64 cb_from = [] {
            char v[24] = {};
            return GetEnvironmentVariableA("SSPC_UPSCALE_CB_DUMP", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) : 0ull;
        }();
        static u64 dumped_flip = 0;
        const u64 flip = VideoOut::NextFlipId();
        if (cb_from != 0 && flip >= cb_from && flip < cb_from + 4 && dumped_flip != flip) {
            dumped_flip = flip; // (the frame's first jittered draw)
            cb_dump_flip_ = flip;
        }
    }
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
    gb.Next(CpProfile::Sec::T2GbBind);
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
    if (seg_mode_ == SegMode::Unread) {
        char v[16] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_TEX_SEGMENT", v, sizeof(v));
        const std::string_view sv(v, n < sizeof(v) ? n : 0);
        seg_mode_ = sv == "on" || sv == "1" ? SegMode::On : sv == "verify" ? SegMode::Verify : SegMode::Off;
        if (seg_mode_ != SegMode::Off) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_TEX_SEGMENT={} - a draw's textures checked once a segment{}", sv,
                     seg_mode_ == SegMode::Verify ? ", each segment hit checked in full and the full path taken" : "");
        }
    }
    // (not where a diagnostic looks at the draw's bound images, nor the upscaler's display draw)
    seg_draw_ = seg_mode_ != SegMode::Off && !up_display_draw_ && !trace_.Active() && !histo_.active && !prof_ &&
                !texdump_.active && insp_catalog_ == nullptr && (diag_ == nullptr || !diag_->Enabled());
    seg_pending_.clear();
    seg_check_.clear();
    const u64 tb0 = GS::FineTicks();
    aq_what_ = (1ull << 63) | vs.address;
    movie_check_ = Media::FullScreenMovieShowing();
    movie_hit_ = false;
    if (has_ps && !BindStage(&ps, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets,
                             d.user_data[static_cast<size_t>(Cp::Stage::Ps)].data(), pc.material[1])) {
        seg_draw_ = false;
        return;
    }
    if (instanced && !GatherInstanced(std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4))) {
        ++stats_.inst_not_current;
        instanced = false; // (the first draw's own block below; the batch is recorded draw by draw)
        inst_bind_.clear();
    }
    // A full-screen movie on a target that is not 16:9: the game's quad covers the whole target and its picture is
    // fitted to the width - cut above and below on a wider target, its edge lines smeared above and below on a
    // narrower one. Shown whole and 16:9 instead: the coordinates as on 16:9 (before the vertex stage reads them), the
    // viewport the centred 16:9 box (SetDynamicState), the rest of the target cleared to black. The subtitles and
    // menus over it are other draws (docs/RESOLUTION_IMPLEMENTATION.md).
    movie_fit_ = false;
    if (movie_hit_ && color_img[0] != nullptr) {
        const float vw = 2.0f * std::abs(F(regs, kViewport0)), vh = 2.0f * std::abs(F(regs, kViewport0 + 2));
        const auto& ts = color_img[0]->layout.size;
        if ((regs.Get(kPaClVteCntl) & 0xF) == 0xF && ((regs.Get(kPaClClipCntl) >> 16) & 1) == 0 && vh > 0 &&
            std::abs(vw - static_cast<float>(ts.width)) < 1.0f && std::abs(vh - static_cast<float>(ts.height)) < 1.0f &&
            std::abs(vw / vh - 16.0f / 9.0f) > 0.01f) {
            for (const auto& b : vs.plan.buffers) {
                if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.address != 0 &&
                    FitMovieCoords(b.address, b.size, movie_v_end_)) {
                    movie_fit_ = true;
                    break;
                }
            }
            if (movie_fitted_ < 2 || (movie_fitted_ + 1) % 1800 == 0 || (!movie_fit_ && movie_fitted_ < 4)) {
                SSPC_LOG(GPU, Info, "movie draw {}: {} on its {}x{} target", movie_fitted_ + 1,
                         movie_fit_ ? "shown whole, 16:9" : "left as the game draws it (not its constants' layout)",
                         ts.width, ts.height);
            }
            ++movie_fitted_;
        }
    }
    const bool vs_bound = BindStage(&vs, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets,
                                    d.user_data[static_cast<size_t>(Cp::Stage::Vs)].data(), pc.material[0]);
    seg_draw_ = false;
    inst_runs_.clear(); // (the copies' descriptors: a run of the multi-draw after the first binds them further on)
    for (const auto& [index, info] : inst_bind_) {
        inst_runs_.push_back({vs.plan.buffers[index - vs.start.buffer].binding, info});
    }
    inst_bind_.clear();
    if (!vs_bound) {
        return;
    }
    stats_.t_bind += GS::FineTicks() - tb0;
    UpscaleDisplayDraw(color_img, img_infos); // (upscaling: the scene sampled by the display draw replaced)
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
    gb.Next(CpProfile::Sec::T2GbVertex);
    ts = GS::FineTicks();
    // SSPC_CITY_BATCH: the batch's geometry (DrawPreparedBatch) in place of this draw's - not with a memory write, an
    // occlusion query, the upscaler's display draw or a diagnostic that looks at single draws
    std::array<VkBuffer, 16> batch_vb{};
    std::array<VkDeviceSize, 16> batch_vo{};
    VkBuffer batch_ib = VK_NULL_HANDLE;
    u64 batch_io = 0;
    u32 batch_count = 0;
    // SSPC_CITY_INSTANCING: a batch with a draw of several instances, or of differing ones, only as a multi-draw (one
    // draw would draw it instance by instance); runs of the same count need push descriptors for the copies
    bool inst_counts = false;
    for (const BatchDraw& b : batch_) {
        inst_counts = inst_counts || std::max<u32>(b.draw->instance_count, 1) != 1;
    }
    bool multi = (instanced || inst_counts) && (inst_runs_.empty() || pipe->push_descriptors);
    const bool batch_ok = !batch_.empty() && (inst_bufs_.empty() || instanced) && (!inst_counts || multi) &&
                          !writes_memory && !occ_.open &&
                         !up_display_draw_ && !trace_.Active() &&
                         !histo_.active && insp_catalog_ == nullptr && !prof_ && skip_draws_.empty() &&
                         !texdump_.active && (diag_ == nullptr || !diag_->Enabled()) &&
                         (regs.Get(kVgtMultiPrimIbResetEn) & 1) == 0;
    // SSPC_CITY_INSTANCING: a batch whose ranges are all in video memory - a multi-draw of them where they are (no copy);
    // else the batch's geometry copied one after another (a multi-draw's draws then at their place in the copy)
    const bool zero_copy = batch_ok && InstancingOn() && (inst_runs_.empty() || pipe->push_descriptors) &&
                           ZeroCopyBatch(vs, batch_vb, batch_vo, batch_ib, batch_io);
    multi = multi || zero_copy;
    const bool batched = batch_ok && (zero_copy || BatchGeometry(vs, batch_vb, batch_vo, batch_ib, batch_io, batch_count));
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
        if (batched) {
            vbufs.push_back(batch_vb[slot]);
            voffs.push_back(batch_vo[slot]);
        } else if (v.status == GS::ResourceStatus::Ok && v.address != 0 && v.size != 0) {
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
    if (batched) {
        ibuf = batch_ib;
        ioff = batch_io;
    } else if (d.indexed) {
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
    gb.Next(CpProfile::Sec::T2GbLayout);
    {
        // SSPC_DIAG_GEOM=<VS address hex>:<first flip>:<flips> (CITY-OPT Phase 3, diagnostics): every range of that VS's
        // draws - address, size, content hash, residency, why it is not cached - to logs\geom\geom.tsv
        struct GeomDiag {
            u64 vs = 0, from = 0, to = 0;
            std::ofstream out;
        };
        static GeomDiag* gd = [this]() -> GeomDiag* {
            char v[64] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_GEOM", v, sizeof(v)) == 0) {
                return nullptr;
            }
            auto* g = new GeomDiag;
            char* p = v;
            g->vs = std::strtoull(p, &p, 16);
            g->from = *p == ':' ? std::strtoull(p + 1, &p, 10) : 3000;
            g->to = g->from + (*p == ':' ? std::strtoull(p + 1, &p, 10) : 300);
            const auto dir = options_.capture_path.parent_path().parent_path() / "geom";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            g->out.open(dir / "geom.tsv", std::ios::trunc);
            g->out << "flip\tn\tstream\taddr\tsize\tstride\thash\tresidency\twhy\tcount\tvo\trt0\tdepth\n";
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_GEOM - VS {:#x}, flips {}..{}", g->vs, g->from, g->to);
            return g;
        }();
        if (gd != nullptr && vs.address == gd->vs) {
            const u64 flip = VideoOut::NextFlipId();
            static u64 last_flip = 0, n = 0;
            n = flip == last_flip ? n + 1 : 0;
            last_flip = flip;
            if (flip >= gd->from && flip < gd->to) {
                const auto line = [&](int s, u64 addr, u64 size, u32 stride) {
                    BufferCache::Range r;
                    const auto res = buffers_.Classify(addr, size, r);
                    const u8* host = guest_.BackingPointer(addr, size);
                    gd->out << flip << '\t' << n << '\t' << s << '\t' << std::hex << addr << std::dec << '\t' << size
                            << '\t' << stride << '\t' << std::hex << (host != nullptr ? RaceHash(host, size) : 0)
                            << std::dec << '\t' << static_cast<int>(res) << '\t' << buffers_.PromoteWhy(addr, size)
                            << '\t' << d.index_count << '\t' << d.vgt_index_offset << '\t' << std::hex
                            << (color_img[0] != nullptr ? color_img[0]->layout.guest_address : 0) << '\t'
                            << (depth_img != nullptr ? depth_img->layout.guest_address : 0) << std::dec << '\n';
                };
                for (size_t s = 0; s < vs.plan.vertices.size(); ++s) {
                    const auto& v = vs.plan.vertices[s];
                    line(static_cast<int>(s), v.address, v.size, v.desc.Stride());
                }
                if (d.indexed) {
                    const u64 isz = idx32 ? 4 : 2;
                    line(-1, d.index_base + u64{d.index_offset} * isz, u64{d.index_count} * isz, static_cast<u32>(isz));
                }
            } else if (flip == gd->to) {
                gd->out.flush();
            }
        }
    }
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
    const auto texture_layout = [](const CachedImage* img) { return TextureLayout(img); };
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
    // SSPC_TEX_SEGMENT: verify - a segment hit's layout as the full path left it; the draw's textures stamped, after
    // its barriers (the generation they changed is the new segment's)
    for (const SegCheck& c : seg_check_) {
        if (c.info < img_infos.size() && img_infos[c.info].imageLayout != c.layout && stats_.tex_seg_mismatches++ < 16) {
            SSPC_LOG(GPU, Warning, "SSPC_TEX_SEGMENT=verify: a segment hit's layout {} differs from the full path's {}",
                     static_cast<int>(c.layout), static_cast<int>(img_infos[c.info].imageLayout));
        }
    }
    if (!seg_pending_.empty()) {
        SegSync();
        const u64 epoch = images_.Epoch(), frame = images_.Frame();
        for (const auto& [memo, img] : seg_pending_) {
            if (memo->img == img && memo->epoch == epoch && std::ranges::find(targets, img) == targets.end() &&
                ((img->gpu_written && !img->memory_newer) || (img->uploaded && !img->dirty && !img->gpu_written)) &&
                img->current == TextureLayout(img) &&
                ImageCache::ReadSince(*img, kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT) &&
                img->last_seq == cur_.value && img->last_frame == frame) {
                memo->seg = tex_seg_;
                memo->seg_layout = img->current;
                ++stats_.tex_seg_stamped;
            }
        }
        seg_pending_.clear();
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
    gb.Next(CpProfile::Sec::T2GbEmit);
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
    if (movie_fit_) { // (the movie draw kept 16:9: the target outside its box black)
        const VkViewport& vp = dyn_.vps[0];
        const s32 bx0 = static_cast<s32>(std::lround(std::min(vp.x, vp.x + vp.width)));
        const s32 bx1 = static_cast<s32>(std::lround(std::max(vp.x, vp.x + vp.width)));
        const s32 by0 = static_cast<s32>(std::lround(std::min(vp.y, vp.y + vp.height)));
        const s32 by1 = static_cast<s32>(std::lround(std::max(vp.y, vp.y + vp.height)));
        const s32 tw = static_cast<s32>(key.width), th = static_cast<s32>(key.height);
        std::array<VkClearRect, 2> bars{};
        u32 nb = 0;
        const auto bar = [&](s32 x0, s32 y0, s32 x1, s32 y1) {
            if (x1 > x0 && y1 > y0) {
                bars[nb++] = {{{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}}, 0, key.layers};
            }
        };
        if (bx0 > 0 || bx1 < tw) {
            bar(0, 0, std::clamp(bx0, 0, tw), th);
            bar(std::clamp(bx1, 0, tw), 0, tw, th);
        } else {
            bar(0, 0, tw, std::clamp(by0, 0, th));
            bar(0, std::clamp(by1, 0, th), tw, th);
        }
        if (nb != 0) {
            VkClearAttachment black{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
            black.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            f.vkCmdClearAttachments(cur_.cb, 1, &black, nb, bars.data());
        }
    }
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
    // a draw inside a guest occlusion query (the game's glow proxies): its own host query, read at the query's end
    u32 occ_slot = ~0u;
    if (occ_.open && !diag_skip && rendering_) {
        if (occ_.slots.size() < occ_.max_slots) {
            occ_slot = occ_next_;
            occ_next_ = (occ_next_ + 1) % kOccSlots;
            occ_.slots.push_back(occ_slot);
            ++occ_draws_;
            f.vkCmdBeginQuery(cur_.cb, occ_pool_, occ_slot,
                              ctx_.Caps().occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        } else {
            ++occ_overflow_;
        }
    }
    if (diag_skip) {
        ++stats_.diag_skipped_draws;
    } else {
    if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            const VkIndexType it = idx32 || (batched && !zero_copy) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
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
    } else if (batched && multi) {
        // one draw per draw of the batch (its own indices, already moved to its vertices), in their order: DrawIndex
        // picks its constants. A multi-draw has one instance count: a run of draws with the same count each - a run
        // after the first takes the constants' copies from its first draw on (DrawIndex starts at 0 again)
        if (!zero_copy) { // (the copy: each draw's indices one after another, already moved to its vertices)
            inst_draws_.clear();
            u32 first = 0;
            for (const BatchDraw& b : batch_) {
                inst_draws_.push_back({first, b.draw->index_count, 0});
                first += b.draw->index_count;
            }
        }
        for (size_t k0 = 0; k0 < batch_.size();) {
            const u32 n_inst = std::max<u32>(batch_[k0].draw->instance_count, 1);
            size_t k1 = k0 + 1;
            while (k1 < batch_.size() && std::max<u32>(batch_[k1].draw->instance_count, 1) == n_inst) {
                ++k1;
            }
            if (k0 != 0 && !inst_runs_.empty()) {
                std::array<VkDescriptorBufferInfo, 4> infos{};
                std::array<VkWriteDescriptorSet, 4> ws{};
                for (size_t r = 0; r < inst_runs_.size() && r < 4; ++r) {
                    const auto& [binding, info] = inst_runs_[r];
                    const u64 skip = u64{inst_bufs_[r].stride} * k0;
                    infos[r] = {info.buffer, info.offset + skip, info.range - skip};
                    ws[r] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstBinding = binding,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[r]};
                    if (binding < desc_shadow_.size()) {
                        desc_shadow_[binding].valid = false; // (the next draw pushes its own)
                    }
                }
                f.vkCmdPushDescriptorSetKHR(cur_.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->layout, 0,
                                            static_cast<u32>(std::min<size_t>(inst_runs_.size(), 4)), ws.data());
            }
            f.vkCmdDrawMultiIndexedEXT(cur_.cb, static_cast<u32>(k1 - k0), inst_draws_.data() + k0, n_inst,
                                       instance_offset, sizeof(VkMultiDrawIndexedInfoEXT), nullptr);
            ++stats_.inst_calls;
            k0 = k1;
        }
        batch_done_ = true;
    } else if (batched) {
        f.vkCmdDrawIndexed(cur_.cb, batch_count, instances, 0, 0, instance_offset); // (the offsets are in the indices)
        batch_done_ = true;
    } else if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            f.vkCmdDrawIndexed(cur_.cb, std::min(d.index_count, d.max_index_size), instances, 0,
                               static_cast<s32>(vertex_offset), instance_offset);
            // SSPC_CITY_ENGINE_OPT: what a run's next draw takes on (DrawRunMembers) - nothing a single draw adds
            run_ok_ = occ_slot == ~0u && rendering_ && !writes_memory && !up_display_draw_ && !trace_.Active() &&
                      !histo_.active && insp_catalog_ == nullptr && !prof_ && !texdump_.active &&
                      (diag_ == nullptr || !diag_->Enabled()) && vbufs.size() <= Cp::RunMember::kMaxStreams;
            run_layout_ = pipe->layout;
            // (push descriptors are updated binding by binding within one layout: a member pushes only its own)
            run_push_desc_ = pipe->push_descriptors && push_layout_ == pipe->layout &&
                             (!bind_shadow_ || desc_layout_ == pipe->layout);
            run_feedback_ = feedback_any;
        }
    } else {
        f.vkCmdDraw(cur_.cb, d.index_count, instances, vertex_offset, instance_offset);
    }
    }
    if (occ_slot != ~0u) {
        f.vkCmdEndQuery(cur_.cb, occ_pool_, occ_slot);
    }
    DiagEnd(cid);
    DiagCaptureDraw(prep.pipeline, color_img, true);
    stage(stats_.t_emit);
    if (prof_) {
        // R12 wfx: the key also tells the targets apart (16 bits of a hash of the rt0 and depth guest addresses in
        // bits 48..63; the low bits stay the pipeline's) - the same pipeline in two passes shows as two rows, each with
        // its targets' addresses, which name the engine's pass (frame trace, RENDER_ENGINE_PASSES.md)
        const u64 rt0_addr = color_img[0] != nullptr ? color_img[0]->layout.guest_address : 0;
        const u64 depth_addr = depth_img != nullptr ? depth_img->layout.guest_address : 0;
        const u64 tgt_hash = ((rt0_addr * 0x9E3779B97F4A7C15ull) ^ (depth_addr * 0xC2B2AE3D27D4EB4Full)) >> 48;
        ProfMark((tgt_hash << 48) | (u64{prep.pipeline} << 2) | 0,
                 fmt::format("draw pipeline {} vs {:#x} ps {:#x} prim {} target {}x{} rt0 fmt {} depth {} rt0 {:#x} "
                             "ds {:#x}",
                             prep.pipeline, vs.address, has_ps ? ps.address : 0, d.primitive_type, key.width,
                             key.height, color_img[0] != nullptr ? color_img[0]->layout.pixel_format : 0,
                             depth_img != nullptr, rt0_addr, depth_addr));
        ProfDescribe((tgt_hash << 48) | (u64{prep.pipeline} << 2) | 0, "draw: vertex stage", &vs, bound);
        if (has_ps) {
            ProfDescribe(((tgt_hash << 48) | (u64{prep.pipeline} << 2)) ^ 0x8000000000000000ull, "draw: pixel stage", &ps, bound);
        }
    }
    if (texdump_.active) { // (F8: the textures this draw reads, once each)
        for (const auto* sv : {&vs, &ps}) {
            if (sv == &ps && !has_ps) {
                continue;
            }
            for (const auto& im : sv->plan.images) {
                const u64 base = (u64{im.desc.raw[0]} | (u64{im.desc.raw[1] & 0xFF} << 32)) << 8;
                if (base == 0 || texdump_.lines.contains(base)) {
                    continue;
                }
                const CachedImage* img = nullptr;
                for (const CachedImage* b : bound) {
                    img = b->layout.guest_address == base ? b : img;
                }
                texdump_.lines[base] = fmt::format(
                    "{:#x} T# {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} | pipeline {} vs {:#x} ps {:#x} | "
                    "{} size {} fmt {} {}x{} levels {} layers {} tile {} pitch {} gpu_written {}",
                    base, im.desc.raw[0], im.desc.raw[1], im.desc.raw[2], im.desc.raw[3], im.desc.raw[4],
                    im.desc.raw[5], im.desc.raw[6], im.desc.raw[7], prep.pipeline, vs.address, has_ps ? ps.address : 0,
                    img != nullptr ? "cached" : "-", img != nullptr ? img->layout.guest_size : 0,
                    img != nullptr ? img->layout.pixel_format : 0, img != nullptr ? img->layout.size.width : 0,
                    img != nullptr ? img->layout.size.height : 0, img != nullptr ? img->layout.levels : 0,
                    img != nullptr ? img->layout.layers : 0, img != nullptr ? static_cast<u32>(img->layout.tile_mode) : 0,
                    img != nullptr ? img->layout.pitch : 0, img != nullptr && img->gpu_written);
            }
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
        // (a value that is a shader address - the pipeline numbers move between runs - names the draws' VS)
        if (std::ranges::find(detail, u64{prep.pipeline}) != detail.end() ||
            std::ranges::find(detail, vs.address) != detail.end()) {
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
        ++tex_seg_; // (SSPC_TEX_SEGMENT: what it wrote may be a texture's memory - its invalidation applied later)
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
        run_ok_ = false;
        Flush();
    }
}

bool Executor::RunRange(u64 addr, u64 size, u64 align, VkBuffer& buffer, u64& offset, u64& avail) {
    // a read range as GuestRange takes it, but only where that records no command: resident in video memory, or a
    // small range nothing of which is there, copied by the CPU into the stream ring
    BufferCache::Range r;
    const BufferCache::Residency res = buffers_.Classify(addr, size, r);
    ++stats_.run_range[static_cast<size_t>(res)];
    if (res == BufferCache::Residency::All) {
        buffer = r.buffer;
        offset = r.offset;
        avail = r.size;
        if (aq_note_reads_) {
            AqNote(addr, size);
        }
        return true;
    }
    if (res != BufferCache::Residency::None || size > (16u << 10) || buffers_.HostWritePending(addr, size) ||
        TimelineWritePending(addr, size)) {
        return false;
    }
    const u8* host = guest_.BackingPointer(addr, size);
    StreamRing::Slice sl{};
    if (host == nullptr || ring_.Allocate(size, align, cur_.value, sl) != StreamRing::Status::Ok) {
        return false;
    }
    std::memcpy(sl.data, host, size);
    buffer = sl.buffer;
    offset = sl.offset;
    avail = size;
    ++stats_.streamed;
    stats_.streamed_bytes += size;
    return true;
}

void Executor::DrawRunMembers(const Cp::DrawDescriptor& lead, const PreparedCommand& pc,
                              std::span<const Cp::RunMember> members, std::span<const u32> member_flat) {
    R8_CP_PROF(CpProfile::BySource(CpProfile::Sec::T2Run, CpProfile::Sec::T2RunMesh, CpProfile::t_source));
    const auto& f = Fn();
    const GS::StageView& vs = *pc.stage;
    const GS::FetchShader& fetch = vs.module->result.reflection.fetch;
    const bool idx32 = (lead.index_type & 3) == 1;
    const u32 isize = idx32 ? 4 : 2;
    const VkIndexType it = idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    std::vector<VkBuffer>& vbufs = scratch_.vbufs;
    std::vector<VkDeviceSize>& voffs = scratch_.voffs;
    if (run_check_ < 0) {
        // (any of the runs' verify modes: the frontend's check of a member's description, this one of its recording)
        run_check_ = 0;
        for (const char* name :
             {"SSPC_CITY_GB_RUN_MATERIAL", "SSPC_CITY_ENGINE_OPT", "SSPC_CITY_ENGINE_GBUFFER_RUNS"}) {
            char v[16] = {};
            const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
            run_check_ |= std::string_view(v, n < sizeof(v) ? n : 0) == "verify" ? 1 : 0;
        }
    }
    const bool check = run_check_ == 1;
    if (check) {
        run_desc_.clear(); // (the run's first draw's: RecordDraw pushed them all)
        StageHashes(vs, run_desc_);
        if (pc.ps != nullptr) {
            StageHashes(*pc.ps, run_desc_);
        }
    }
    // a member's own views: the run's VS with its geometry, its registers and its own constant blocks, the run's PS
    // with its registers, its own constant blocks (CITY-OPT Phase 6) and its own T#s / S#s (SSPC_CITY_GB_RUN_MATERIAL)
    const auto own_views = [&](const Cp::RunMember& m) {
        if (run_view_ == nullptr) {
            run_view_ = std::make_unique<GS::StageView>();
        }
        *run_view_ = vs;
        std::copy(m.vs_user_data.begin(), m.vs_user_data.end(), run_view_->plan.flat.begin());
        if (m.vs_flat_at != Cp::RunMember::kNoFlat &&
            u64{m.vs_flat_at} + run_view_->plan.flat.size() <= member_flat.size()) {
            std::copy_n(member_flat.begin() + m.vs_flat_at, run_view_->plan.flat.size(), run_view_->plan.flat.begin());
        }
        for (u32 i = 0; i < m.streams; ++i) {
            GS::VertexBinding& v = run_view_->plan.vertices[i];
            v.read = m.v[i].read;
            v.desc.raw = m.v[i].desc;
            v.address = m.v[i].address;
            v.size = m.v[i].size;
        }
        for (u32 k = 0; k < m.buffers; ++k) {
            GS::BufferBinding& bb = run_view_->plan.buffers[m.b[k].index];
            bb.address = m.b[k].address;
            bb.size = m.b[k].size;
            bb.desc.raw = m.b[k].desc;
        }
        PreparedCommand p = pc;
        p.stage = run_view_.get();
        if (m.own_ps && pc.ps != nullptr) {
            if (run_ps_view_ == nullptr) {
                run_ps_view_ = std::make_unique<GS::StageView>();
            }
            *run_ps_view_ = *pc.ps;
            GS::ResourceBindingPlan& plan = run_ps_view_->plan;
            std::copy(m.ps_user_data.begin(), m.ps_user_data.end(), plan.flat.begin());
            if (m.ps_flat_at != Cp::RunMember::kNoFlat && u64{m.ps_flat_at} + plan.flat.size() <= member_flat.size()) {
                std::copy_n(member_flat.begin() + m.ps_flat_at, plan.flat.size(), plan.flat.begin());
            }
            for (u32 k = 0; k < m.ps_buffers; ++k) {
                GS::BufferBinding& bb = plan.buffers[m.pb[k].index];
                bb.address = m.pb[k].address;
                bb.size = m.pb[k].size;
                bb.desc.raw = m.pb[k].desc;
            }
            if (m.sharps_at != Cp::RunMember::kNoFlat &&
                u64{m.sharps_at} + 8 * std::popcount(m.img_mask) + 4 * std::popcount(m.smp_mask) <= member_flat.size() &&
                static_cast<size_t>(std::bit_width(m.img_mask)) <= plan.images.size() &&
                static_cast<size_t>(std::bit_width(m.smp_mask)) <= plan.samplers.size()) {
                const u32* at = member_flat.data() + m.sharps_at;
                for (u32 mk = m.img_mask; mk != 0; mk &= mk - 1, at += 8) {
                    std::copy_n(at, 8, plan.images[std::countr_zero(mk)].desc.raw.begin());
                }
                for (u32 mk = m.smp_mask; mk != 0; mk &= mk - 1, at += 4) {
                    std::copy_n(at, 4, plan.samplers[std::countr_zero(mk)].desc.raw.begin());
                }
                p.material[1] = 0; // (GbBind: the run's material is not its)
            }
            p.ps = run_ps_view_.get();
        }
        return p;
    };
    const float lod = jitter_draw_ ? lod_bias_ : 0.0f; // (BindStage's for the run's first draw)
    bool invalidations = false; // (ApplyPendingInvalidations before the members' own textures' memo checks)
    // a member's own descriptors (written in full before each use)
    constexpr size_t kMaxBufferWrites = 2 * (Cp::RunMember::kMaxBuffers + 1); // (VS and PS: buffers + flattened data)
    constexpr size_t kMaxImageWrites = 2 * Cp::RunMember::kMaxSharpIndex;   // (its own images and samplers)
    std::array<VkDescriptorBufferInfo, kMaxBufferWrites> infos;
    std::array<VkDescriptorImageInfo, kMaxImageWrites> iinfos;
    std::array<VkWriteDescriptorSet, kMaxBufferWrites + kMaxImageWrites> ws;
    for (const Cp::RunMember& m : members) {
        const u64 t0 = GS::Ticks();
        const u32 count = std::min(m.index_count, m.max_index_size);
        // on the state the run's last draw left (RecordDraw keeps its vertex buffers in scratch_), its ranges where the
        // buffer cache keeps them in video memory
        bool lean = run_ok_ && rendering_ && open_ && commands_ < options_.max_draws_per_batch && count != 0 &&
                    m.streams == vbufs.size();
        VkBuffer ib = VK_NULL_HANDLE;
        u64 io = 0;
        if (lean) {
            for (u32 i = 0; i < m.streams && lean; ++i) {
                BufferCache::Range r;
                lean = buffers_.Classify(m.v[i].address, m.v[i].size, r) == BufferCache::Residency::All;
                vbufs[i] = r.buffer;
                voffs[i] = r.offset;
            }
            BufferCache::Range r;
            lean = lean &&
                   buffers_.Classify(m.index_base + u64{m.index_offset} * isize, u64{count} * isize, r) ==
                       BufferCache::Residency::All &&
                   r.offset % isize == 0;
            ib = r.buffer;
            io = r.offset;
            if (lean && aq_note_reads_) {
                for (u32 i = 0; i < m.streams; ++i) {
                    AqNote(m.v[i].address, m.v[i].size);
                }
                AqNote(m.index_base + u64{m.index_offset} * isize, u64{count} * isize);
            }
            stats_.run_not_resident += lean ? 0 : 1;
            stats_.run_why[0] += lean ? 0 : 1;
        }
        // its own constant blocks and user data (the shadow casters' instances): descriptors of their own, pushed over
        // the run's (the shadow of what this command buffer has pushed decides what is pushed)
        u32 nw = 0, nb = 0, ni = 0;
        PushData push = push_last_;
        if (check) {
            run_check_own_.clear();
        }
        if (lean && (m.buffers != 0 || m.own_user_data || m.own_ps)) {
            lean = run_push_desc_ && push_layout_ == run_layout_ && (!bind_shadow_ || desc_layout_ == run_layout_);
            stats_.run_why[1] += lean ? 0 : 1;
            const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
            const auto add = [&](u32 binding, VkBuffer buf, u64 offset, u64 range) {
                infos[nb] = {buf, offset, range};
                ws[nw++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .dstBinding = binding,
                            .descriptorCount = 1,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .pBufferInfo = &infos[nb++]};
            };
            // a stage's own constant blocks (the run's plan index, range, V#) at its bindings
            const auto own_buffers = [&](const GS::StageView& st, std::span<const Cp::RunMember::Buffer> own) {
                for (u32 k = 0; k < own.size() && lean; ++k) {
                    VkBuffer buf = VK_NULL_HANDLE;
                    u64 off = 0, avail = 0;
                    lean = RunRange(own[k].address, own[k].size, align, buf, off, avail);
                    stats_.run_why[2] += lean ? 0 : 1;
                    if (lean) {
                        const u64 aligned = off & ~(align - 1), adjust = off - aligned;
                        const u32 index = st.start.buffer + own[k].index;
                        if (index < push.buf_offsets.size()) {
                            push.buf_offsets[index] = static_cast<u8>(adjust);
                        }
                        add(st.plan.buffers[own[k].index].binding, buf, aligned,
                            std::min<u64>(own[k].size + adjust, avail + adjust));
                        if (check) {
                            GS::BufferBinding bb = st.plan.buffers[own[k].index];
                            bb.address = own[k].address;
                            bb.size = own[k].size;
                            bb.desc.raw = own[k].desc;
                            run_check_own_.push_back({bb.binding, BufferHash(bb)});
                        }
                    }
                }
            };
            // a stage's own flattened user data: the run's (past the registers: an SRT walk's output, the same for
            // every member - CITY-OPT Phase 6) with its own registers at the start
            // (`at`: its own whole flattened data in the run's store - a walk of its own, Phase 6)
            const auto own_flat = [&](const GS::StageView& st, const std::array<u32, 16>& user_data, u32 at) {
                const auto fb = std::ranges::find(st.plan.buffers, GS::BufferKind::Flatbuf, &GS::BufferBinding::kind);
                StreamRing::Slice sl{};
                const u64 bytes = st.plan.flat.size() * 4;
                lean = fb != st.plan.buffers.end() && bytes >= user_data.size() * 4 &&
                       (at == Cp::RunMember::kNoFlat || u64{at} + st.plan.flat.size() <= member_flat.size()) &&
                       ring_.Allocate(bytes, align, cur_.value, sl) == StreamRing::Status::Ok;
                stats_.run_why[3] += lean ? 0 : 1;
                if (lean) {
                    if (at != Cp::RunMember::kNoFlat) {
                        std::memcpy(sl.data, member_flat.data() + at, bytes);
                    } else {
                        std::memcpy(sl.data, st.plan.flat.data(), bytes);
                        std::memcpy(sl.data, user_data.data(), user_data.size() * 4);
                    }
                    add(fb->binding, sl.buffer, sl.offset, bytes);
                    if (check) { // (the bytes pushed, read back)
                        run_check_own_.push_back({fb->binding, FlatHash(reinterpret_cast<const u32*>(sl.data),
                                                                        st.plan.flat.size(), FlatRegisters(st))});
                    }
                }
            };
            // its own textures and samplers (SSPC_CITY_GB_RUN_MATERIAL): an image's view as BindStage's memo keeps it -
            // valid as BindStage takes it, already in the layout the run's draw reads it in (no barrier inside
            // rendering), not one of the targets -, an S#'s sampler as BindStage makes it
            const auto own_sharps = [&](const GS::StageView& st) {
                const u32 n_img = static_cast<u32>(std::popcount(m.img_mask));
                const u32 n_smp = static_cast<u32>(std::popcount(m.smp_mask));
                lean = m.sharps_at != Cp::RunMember::kNoFlat && regs_ != nullptr && st.module != nullptr &&
                       u64{m.sharps_at} + 8 * n_img + 4 * n_smp <= member_flat.size() &&
                       static_cast<size_t>(std::bit_width(m.img_mask)) <= st.plan.images.size() &&
                       static_cast<size_t>(std::bit_width(m.smp_mask)) <= st.plan.samplers.size();
                u32 why = ~0u;
                if (lean && (run_feedback_ || pending_writes_)) {
                    why = run_feedback_ ? 2 : 3;
                    lean = false;
                }
                if (lean && n_img != 0 && !invalidations) {
                    images_.ApplyPendingInvalidations(); // (as BindStage before its memo's checks)
                    invalidations = true;
                }
                // SSPC_TEX_SEGMENT: an image stamped in this segment taken from its stamp (on), checked (verify)
                const bool seg = lean && n_img != 0 && seg_mode_ != SegMode::Off;
                if (seg) {
                    SegSync();
                }
                const u32* at = lean ? member_flat.data() + m.sharps_at : nullptr;
                const u64 epoch = images_.Epoch();
                for (u32 mk = m.img_mask; mk != 0 && lean; mk &= mk - 1, at += 8) {
                    const GS::ImageBinding& im = st.plan.images[std::countr_zero(mk)];
                    std::array<u32, 8> raw;
                    std::copy_n(at, 8, raw.begin());
                    const u32 flags = TexFlags(im);
                    const u64 key = TexMemoKey(raw, flags);
                    TexMemo* memo = nullptr;
                    if (const auto& slot = tex_slot_[TexSlot(key)]; slot.second != nullptr && slot.first == key) {
                        memo = slot.second;
                    } else if (const auto mi = tex_memo_.find(key); mi != tex_memo_.end()) {
                        memo = &mi->second;
                    }
                    CachedImage* img = memo != nullptr ? memo->img : nullptr;
                    const bool seg_hit = seg && img != nullptr && memo->seg == tex_seg_ && !im.storage &&
                                         memo->epoch == epoch && memo->raw == raw && memo->flags == flags &&
                                         std::ranges::find(scratch_.targets, img) == scratch_.targets.end();
                    if (seg_hit && seg_mode_ == SegMode::On) {
                        ++stats_.tex_seg_member_hits;
                        iinfos[ni] = {VK_NULL_HANDLE, memo->view, memo->seg_layout};
                        ws[nw++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstBinding = im.binding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                    .pImageInfo = &iinfos[ni++]};
                        if (check) {
                            GS::ImageBinding ib = im;
                            ib.desc.raw = raw;
                            run_check_own_.push_back({im.binding, ImageHash(ib)});
                        }
                        continue;
                    }
                    if (seg_hit) { // (verify: what the full checks below take - the stamp's layout, the image touched)
                        ++stats_.tex_seg_member_hits;
                        ++stats_.tex_seg_verified;
                        const bool same =
                            ((img->gpu_written && !img->memory_newer) || (img->uploaded && !img->dirty && !img->gpu_written)) &&
                            img->current == TextureLayout(img) && img->current == memo->seg_layout &&
                            img->last_seq == cur_.value && img->last_frame == images_.Frame();
                        if (!same && stats_.tex_seg_mismatches++ < 16) {
                            SSPC_LOG(GPU, Warning,
                                     "SSPC_TEX_SEGMENT=verify: a run member's img{} {:#x} stamped in segment {} differs: "
                                     "layout {} (stamped {}) seq {} (batch {})",
                                     img->id, img->layout.guest_address, tex_seg_, static_cast<int>(img->current),
                                     static_cast<int>(memo->seg_layout), img->last_seq, cur_.value);
                        }
                    }
                    if (img == nullptr || memo->epoch != epoch || memo->raw != raw || memo->flags != flags ||
                        !((img->gpu_written && !img->memory_newer) || (img->uploaded && !img->dirty && !img->gpu_written))) {
                        why = 0; // (BindStage would find it, maybe upload it)
                    } else if (img->current != TextureLayout(img)) {
                        why = 1; // (a barrier first)
                    } else if (std::ranges::find(scratch_.targets, img) != scratch_.targets.end()) {
                        why = 2; // (the attachments in the general layout)
                    } else {
                        images_.Touch(*img, cur_.value); // what Find records
                        if (seg && !im.storage &&
                            ImageCache::ReadSince(*img, kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT)) {
                            memo->seg = tex_seg_; // (inside rendering: no barrier since - checked as a draw's are)
                            memo->seg_layout = img->current;
                            ++stats_.tex_seg_stamped;
                        }
                        iinfos[ni] = {VK_NULL_HANDLE, memo->view, img->current};
                        ws[nw++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstBinding = im.binding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                    .pImageInfo = &iinfos[ni++]};
                        if (check) {
                            GS::ImageBinding ib = im;
                            ib.desc.raw = raw;
                            run_check_own_.push_back({im.binding, ImageHash(ib)});
                        }
                        continue;
                    }
                    lean = false;
                }
                const GS::ShaderReflection* refl = lean ? &st.module->result.reflection : nullptr;
                const u64 border_base = lean ? u64{regs_->Get(kTaBcBase)} << 8 : 0;
                for (u32 mk = m.smp_mask; mk != 0 && lean; mk &= mk - 1, at += 4) {
                    const u32 i = static_cast<u32>(std::countr_zero(mk));
                    GS::SamplerDesc desc;
                    std::copy_n(at, 4, desc.raw.begin());
                    const bool depth = i < refl->samplers.size() && refl->samplers[i].is_depth;
                    VkSampler s = images_.Sampler(desc, depth, border_base, lod);
                    if (s == VK_NULL_HANDLE) {
                        s = images_.Sampler(GS::SamplerDesc{}, depth, 0); // the bridge's fallback: the default S#
                    }
                    iinfos[ni] = {s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
                    ws[nw++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstBinding = st.plan.samplers[i].binding,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                .pImageInfo = &iinfos[ni++]};
                    if (check) {
                        GS::SamplerBinding sb = st.plan.samplers[i];
                        sb.desc = desc;
                        run_check_own_.push_back({sb.binding, SamplerHash(sb)});
                    }
                }
                stats_.run_why[4] += lean ? 0 : 1;
                if (!lean && why < stats_.run_sharp_why.size()) {
                    ++stats_.run_sharp_why[why];
                }
                if (lean) {
                    stats_.run_sharp_images += n_img;
                    stats_.run_sharp_samplers += n_smp;
                }
            };
            own_buffers(vs, std::span(m.b.data(), m.buffers));
            if (lean && m.own_user_data) {
                own_flat(vs, m.vs_user_data, m.vs_flat_at);
            }
            if (lean && m.own_ps) {
                lean = pc.ps != nullptr;
                if (lean) {
                    own_buffers(*pc.ps, std::span(m.pb.data(), m.ps_buffers));
                }
                if (lean && m.own_ps_user_data) {
                    own_flat(*pc.ps, m.ps_user_data, m.ps_flat_at);
                }
                if (lean && (m.img_mask | m.smp_mask) != 0) {
                    own_sharps(*pc.ps);
                }
            }
            stats_.run_not_resident += lean ? 0 : 1;
        }
        if (!lean) {
            // recorded as the draw it is: the run's preparation with its geometry, its registers (RecordDraw)
            Cp::DrawDescriptor d = lead;
            d.seq = m.seq;
            d.packet_addr = m.packet_addr;
            d.index_base = m.index_base;
            d.index_offset = m.index_offset;
            d.index_count = m.index_count;
            d.instance_count = m.instance_count;
            d.max_index_size = m.max_index_size;
            d.vgt_index_offset = m.vgt_index_offset;
            d.changed = m.changed;
            d.user_data[static_cast<size_t>(Cp::Stage::Vs)] = m.vs_user_data;
            if (m.own_ps && pc.ps != nullptr) {
                d.user_data[static_cast<size_t>(Cp::Stage::Ps)] = m.ps_user_data; // (CITY-OPT Phase 6)
            }
            const PreparedCommand p = own_views(m);
            if (check) {
                run_desc_.clear(); // (RecordDraw pushes them all)
                StageHashes(*p.stage, run_desc_);
                if (p.ps != nullptr) {
                    StageHashes(*p.ps, run_desc_);
                }
            }
            ++stats_.run_own;
            stencil_override_ = m.own_stencil;
            stencil_override_ref_ = m.stencil_ref;
            stencil_override_bf_ = m.stencil_ref_bf;
            RecordDraw(d, p, t0);
            stencil_override_ = false;
            continue;
        }
        if (check) {
            // =verify: the bindings it is recorded with - those the run's draws before it left, its own over them -
            // against those it has recorded on its own (its views)
            for (const auto& [binding, h] : run_check_own_) {
                if (binding >= run_desc_.size()) {
                    run_desc_.resize(binding + 1, 0);
                }
                run_desc_[binding] = h;
            }
            const PreparedCommand p = own_views(m);
            run_expect_.clear();
            StageHashes(*p.stage, run_expect_);
            if (p.ps != nullptr) {
                StageHashes(*p.ps, run_expect_);
            }
            const size_t n = std::max(run_desc_.size(), run_expect_.size());
            run_desc_.resize(n, 0);
            run_expect_.resize(n, 0);
            ++stats_.run_checked;
            u32 differ = 0, first = 0;
            for (u32 k = 0; k < n; ++k) {
                if (run_desc_[k] != run_expect_[k]) {
                    first = differ == 0 ? k : first;
                    ++differ;
                }
            }
            if (differ != 0) {
                if (++stats_.run_check_mismatches <= 12) {
                    SSPC_LOG(GPU, Warning, "SSPC_CITY_GB_RUN_MATERIAL=verify: run member at {:#x} (of the draw at {:#x}) "
                                           "recorded with {} bindings that differ from its own (first: binding {})",
                             m.packet_addr, lead.packet_addr, differ, first);
                }
                run_desc_.swap(run_expect_);
            }
        }
        if (bind_shadow_ && vb_valid_ && vbufs == vb_last_ && voffs == vb_off_last_) {
            ++stats_.vb_binds_skipped;
        } else {
            f.vkCmdBindVertexBuffers(cur_.cb, 0, static_cast<u32>(vbufs.size()), vbufs.data(), voffs.data());
            stats_.vertex_bindings += vbufs.size();
            vb_last_ = vbufs;
            vb_off_last_ = voffs;
            vb_valid_ = true;
        }
        if (bind_shadow_ && ib == ib_last_ && io == ib_off_last_ && it == ib_type_last_) {
            ++stats_.ib_binds_skipped;
        } else {
            f.vkCmdBindIndexBuffer(cur_.cb, ib, io, it);
            ib_last_ = ib;
            ib_off_last_ = io;
            ib_type_last_ = it;
        }
        if (nw != 0) {
            f.vkCmdPushDescriptorSetKHR(cur_.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, run_layout_, 0, nw, ws.data());
            for (u32 k = 0; k < nw; ++k) {
                if (ws[k].dstBinding >= desc_shadow_.size()) {
                    desc_shadow_.resize(ws[k].dstBinding + 1);
                }
                DescShadow& sh = desc_shadow_[ws[k].dstBinding];
                sh.valid = true;
                sh.type = ws[k].descriptorType;
                if (ws[k].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    sh.buffer = *ws[k].pBufferInfo;
                } else {
                    sh.image = *ws[k].pImageInfo;
                }
            }
            stats_.desc_pushed += nw;
        }
        if (std::memcmp(&push, &push_last_, sizeof(PushData)) != 0) {
            f.vkCmdPushConstants(cur_.cb, run_layout_, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(PushData), &push);
            push_last_ = push;
        }
        if (m.own_stencil && regs_ != nullptr) {
            SetRunStencilRef(*regs_, m.stencil_ref, m.stencil_ref_bf); // (Phase 6: the mesh's stencil reference)
        }
        // the fetch shader's vertex/instance offsets from its own registers (as RecordDraw)
        u32 vertex_offset = m.vgt_index_offset;
        u32 instance_offset = 0;
        if (vertex_offset == 0 && fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
            vertex_offset = m.vs_user_data[fetch.vertex_offset_sgpr];
        }
        if (fetch.instance_offset_sgpr >= 0 && fetch.instance_offset_sgpr < 16) {
            instance_offset = m.vs_user_data[fetch.instance_offset_sgpr];
        }
        f.vkCmdDrawIndexed(cur_.cb, count, std::max<u32>(m.instance_count, 1), 0, static_cast<s32>(vertex_offset),
                           instance_offset);
        // what RecordDraw counts for a draw
        ++stats_.draws;
        ++stats_.draws_executed;
        ++stats_.indexed;
        ++stats_.run_lean;
        ++trace_.ordinal;
        Note(lead.queue, m.seq);
        ++commands_;
        stats_.record_ns += GS::TicksToNs(GS::Ticks() - t0);
    }
    if (commands_ >= options_.max_draws_per_batch) {
        ++stats_.flushes_full;
        flush_full_ = true;
        run_ok_ = false;
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

void Executor::PrefetchCompute(const GS::StageView& cs) {
    if (cs.module != nullptr && pipelines_.Prefetching()) {
        pipelines_.PrefetchCompute(*cs.module, cs.plan);
    }
}

void Executor::DispatchPrepared(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs,
                                const PreparedCommand& pc) {
    const u64 t0 = GS::Ticks();
    const GS::FineSample fine_sample(fine_counter_);
    RecordDispatch(d, cs, pc, t0);
}

void Executor::RecordDispatch(const Cp::DispatchDescriptor& d, const Cp::ComputeRegisters* cs,
                              const PreparedCommand& pc, u64 t0) {
    stream_pipe_ = 0;
    ++stats_.dispatches;
    ++trace_.ordinal;
    const auto& f = Fn();
    const GS::DrawPreparation* prepared = cs == nullptr ? nullptr : pc.prep;
    // (AQ: a dispatch binding images stays on the main queue - their layouts are one state, the transitions graphics')
    bool main_only = pc.stage != nullptr && !pc.stage->plan.images.empty();
    // (R11 SSPC_R11_AQ_IMAGES: a compute queue's dispatch that only samples clean textures in their layout may not)
    u64 r11_img_main = 0;
    bool r11_img = false;
    if (main_only && r11_aq_images_ && d.queue != Cp::kGraphicsQueue) {
        r11_img = R11LaneImagesOk(*pc.stage, r11_img_main);
        main_only = !r11_img;
        ++r11_img_stats_[r11_img ? 0 : 1];
    }
    const u64 splits_before = stats_.aq_splits; // (R11 diagnostics)
    AqBoundary(d.queue, main_only);
    if (!Begin()) {
        return;
    }
    r11_img = r11_img && cur_.pool_lane != 0; // (on the main queue - not split - it is an ordinary one there)
    if (r11_img && r11_img_main != 0) {
        cur_.aq_wait_main_min = std::max(cur_.aq_wait_main_min, r11_img_main); // (their last barriers' main batch)
    }
    r11_note_dispatch_ = true;
    Note(d.queue, d.seq);
    if (r11_diag_ != nullptr && pc.stage != nullptr) {
        R11DiagDispatch(*pc.stage, d.queue, d.indirect, main_only, splits_before);
    }
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
    const GS::Module* cs_module = csv.module;
    if (psort_on_ != 0) { // SSPC_R12_PSORT: the particle ranking CS's variant (the same bindings and plan)
        if (const GS::Module* v = PsortVariant(*csv.module, csv.address); v != nullptr) {
            cs_module = v;
        }
    }
    const PipelineObjects* pipe = pipelines_.Compute(*cs_module, csv.plan, err);
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
    aq_what_ = csv.address;
    aq_watch_capture_ = aq_watch_cs_ != 0 && csv.address == aq_watch_cs_;
    aq_watch_b0_ = {};
    if (!BindStage(&csv, true, push, writes, buf_infos, img_infos, bound, writes_memory, none)) {
        if (r11_img) {
            cur_.aq_images = true; // (R11: whatever BindStage did to the images before it failed)
        }
        return;
    }
    stats_.t_bind += GS::FineTicks() - tb0;
    if (aq_ && !bound.empty() && !r11_img) {
        cur_.aq_images = true;
    }
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
            if (b.kind != GS::BufferKind::SharedMemory && aq_) {
                cur_.aq_overflow = true; // (AQ: an access of unknown extent)
                cur_.r11_ovf |= 2;
            }
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
    if (r11_img) {
        // R11 SSPC_R11_AQ_IMAGES: binding them recorded no barrier on the lane (an invalidation applied meanwhile made
        // one dirty - uploaded here -, a new image) - a later main barrier over them waits for this batch; otherwise the
        // lane's images rule (conservative: after every main batch before it, the main batches after it after it)
        bool clean = !bound.empty();
        for (const CachedImage* img : bound) {
            clean = clean && !img->barrier_lane;
        }
        if (clean) {
            for (CachedImage* img : bound) {
                img->lane_use = cur_.value;
            }
            ++r11_img_stats_[3];
        } else {
            cur_.aq_images = true;
            if (r11_img_stats_[4]++ < 8) {
                SSPC_LOG(GPU, Info, "R11 AQ images: cs {:#x} fell back - a barrier on the lane over its {} images",
                         csv.address, bound.size());
            }
        }
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
    const u32 aq_mark = aq_markers_ ? aq_marker_next_++ : 0;
    if (aq_mark != 0) {
        aq_marker_ring_[aq_mark & (aq_marker_ring_.size() - 1)] = {
            cur_.value, csv.address, d.queue, {d.groups[0], d.groups[1], d.groups[2]}, cur_.pool_lane, d.indirect};
        if (aq_watch_b0_.buffer != VK_NULL_HANDLE) {
            AqWatchDispatch(aq_mark);
        }
        f.vkCmdWriteBufferMarkerAMD(cur_.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, aq_marker_buf_.buffer,
                                    cur_.pool_lane != 0 ? 8 : 0, aq_mark);
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
    if (aq_mark != 0) {
        f.vkCmdWriteBufferMarkerAMD(cur_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, aq_marker_buf_.buffer,
                                    cur_.pool_lane != 0 ? 12 : 4, aq_mark);
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
    if (R12::LightCensusOn()) {
        // (R12 diagnostics: which dispatches make the large batch-end write-backs; SSPC_R12_WRITER_DETAIL=<hex cs>:
        // that shader's guest buffers, each distinct set once)
        for (const auto& [wb, we] : acc_writes_) {
            if (we - wb >= (256u << 10)) {
                R12::NoteWriter(csv.address, wb, we - wb);
            }
        }
        if (csv.address == R12::DetailShader()) {
            std::string line;
            for (const auto& b : csv.plan.buffers) {
                if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.size != 0) {
                    line += fmt::format(" {}{:#x}+{:#x}", b.written ? "W" : "R", b.address, b.size);
                }
            }
            R12::NoteDetail(fmt::format("cs {:#x} q{} groups {}x{}x{}{}:{}", csv.address, d.queue, d.groups[0],
                                        d.groups[1], d.groups[2], d.indirect ? " indirect" : "", line));
        }
    }
    if (buffers_.Lazy() == 3 && csv.address == kR12TiledLightingCs) {
        // R12 (SSPC_R12_LIGHT_LIST_KEEP=1): the tiled lighting's written buffer - the per-tile light index list (1-64
        // MiB) - is GPU-only: kept back from the host (docs/R12_LIGHTS.md)
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            SSPC_LOG(GPU, Info, "R12: tiled lighting CS {:#x} (footer hash {:#018x}) groups {}x{}x{}", csv.address,
                     csv.footer_hash, d.groups[0], d.groups[1], d.groups[2]);
        }
        for (const auto& b : csv.plan.buffers) {
            if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok && b.written &&
                b.size >= (1ull << 20) && b.size <= (64ull << 20)) {
                buffers_.AddGpuOnlyRange(b.address, b.size);
            }
        }
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

void Executor::MenuFocusProbe(const CachedImage& img, VkFormat shown, u32 w, u32 h) {
    if (!MenuFocus::Wanted() || w < 640 || h < 360) {
        return;
    }
    const u32 texel = std::max<u32>(img.layout.num_bits / 8, 1);
    if (texel != 4 && texel != 8) {
        return;
    }
    FocusRead* slot = nullptr;
    for (auto& r : focus_reads_) {
        if (r.value == 0) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        return; // the GPU is behind: this flip is not read (the next one is)
    }
    // the strip in this image's pixels: the game lays its menus out for 1920x1080, scaled by the height and centred
    // across a picture that is not 16:9 (docs/RESOLUTION_RESEARCH.md)
    const float sx = static_cast<float>(h) / 1080.0f, sy = sx;
    const float ox = (static_cast<float>(w) / sx - 1920.0f) / 2;
    const u32 x0 = static_cast<u32>(std::max(0.0f, (MenuFocus::kStripX + ox) * sx));
    const u32 y0 = static_cast<u32>(MenuFocus::kStripY * sy);
    if (x0 >= w) {
        return;
    }
    const u32 sw = std::max<u32>(1, std::min<u32>(static_cast<u32>(MenuFocus::kStripW * sx), w - x0));
    const u32 sh = std::max<u32>(1, std::min<u32>(static_cast<u32>(MenuFocus::kStripH * sy), h - y0));
    const u32 bx = static_cast<u32>(std::max(0.0f, (MenuFocus::kBarX + ox) * sx));
    const u32 bw = std::max<u32>(1, std::min<u32>(static_cast<u32>(MenuFocus::kBarW * sx), w > bx ? w - bx : 1));
    if (bx >= w) {
        return;
    }
    const u64 list_bytes = u64{sw} * sh * texel;
    const u64 bytes = list_bytes + u64{bw} * sh * texel;
    if (slot->buffer.buffer == VK_NULL_HANDLE || slot->buffer.size < bytes) {
        if (slot->buffer.buffer != VK_NULL_HANDLE) {
            DestroyBuffer(ctx_, slot->buffer);
        }
        std::string error;
        if (!CreateBuffer(ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, slot->buffer, error, true,
                          HostPlacement::Cached)) {
            SSPC_LOG(GPU, Warning, "menu focus readback: no buffer ({})", error);
            return;
        }
    }
    const auto& f = Fn();
    const VkBufferImageCopy regions[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageOffset = {static_cast<s32>(x0), static_cast<s32>(y0), 0},
         .imageExtent = {sw, sh, 1}},
        {.bufferOffset = list_bytes,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageOffset = {static_cast<s32>(bx), static_cast<s32>(y0), 0},
         .imageExtent = {bw, sh, 1}}};
    // the image is in TRANSFER_SRC_OPTIMAL for the flip copy (both only read it)
    f.vkCmdCopyImageToBuffer(cur_.cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->buffer.buffer, 2, regions);
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cur_.cb, &dep);
    slot->value = cur_.value;
    slot->format = shown;
    slot->w = sw;
    slot->bw = bw;
    slot->h = sh;
    slot->texel = texel;
    slot->y0 = static_cast<float>(y0) / sy;
    slot->scale = 1.0f / sy;
}

void Executor::CollectMenuFocus(u64 completed) {
    // the oldest completed copy first: readings are published in flip order
    for (;;) {
        FocusRead* next = nullptr;
        for (auto& r : focus_reads_) {
            if (r.value != 0 && r.value <= completed && (next == nullptr || r.value < next->value)) {
                next = &r;
            }
        }
        if (next == nullptr) {
            return;
        }
        std::vector<u8> rgba, bar;
        // host-coherent memory (CreateBuffer: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED): no invalidation needed
        const u8* bar_src = next->buffer.mapped + u64{next->w} * next->h * next->texel;
        if (ImageReadback::ToRgba8(next->format, next->buffer.mapped, next->w, next->h, rgba) &&
            ImageReadback::ToRgba8(next->format, bar_src, next->bw, next->h, bar)) {
            MenuFocus::Publish(rgba.data(), next->w, bar.data(), next->bw, next->h, next->y0, next->scale);
        }
        next->value = 0;
    }
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
        const u64 target = g_watch_target.load(std::memory_order_acquire);
        const u64 page = (target != 0 ? target : watch) & ~0xFFFull;
        DWORD old = 0;
        if (target != 0 && page != g_watch_page.load(std::memory_order_acquire)) {
            if (const u64 prev = g_watch_page.load(std::memory_order_acquire); prev != 0) {
                VirtualProtect(reinterpret_cast<void*>(prev), 4096, PAGE_READWRITE, &old); // (the old page released)
            }
        }
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

void Executor::TexDumpFlip() {
    // the frame that collected ends: its list and every texture's bytes (as the game keeps them) into a folder
    if (texdump_.active) {
        texdump_.active = false;
        const std::filesystem::path dir =
            options_.capture_path.parent_path().parent_path() / "frame_textures" / std::to_string(texdump_.number);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream index(dir / "index.txt");
        u32 saved = 0;
        for (const auto& [base, line] : texdump_.lines) {
            index << line << '\n';
            const auto at = line.find(" size ");
            const u64 size = at == std::string::npos ? 0 : std::strtoull(line.c_str() + at + 6, nullptr, 10);
            if (size == 0 || size > (16u << 20) || line.find("gpu_written true") != std::string::npos) {
                continue;
            }
            std::vector<u8> bytes(size);
            if (GS::GameMemory().Read(base, bytes.data(), size)) {
                std::ofstream(dir / fmt::format("{:x}.bin", base), std::ios::binary)
                    .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size));
                ++saved;
            }
        }
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: F8 - {} textures of a frame listed, {} saved -> {}", texdump_.lines.size(),
                 saved, dir.string());
        texdump_.lines.clear();
    }
    const u32 req = Diag::g_texture_dump_request.load(std::memory_order_relaxed);
    if (req != texdump_.seen_request) {
        texdump_.seen_request = req;
        texdump_.active = true;
        ++texdump_.number;
    }
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
    if (wants.empty()) {
        return;
    }
    AqBoundary(Cp::kGraphicsQueue);
    if (!Begin()) {
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
    AqBoundary(Cp::kGraphicsQueue);
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
    if (back.empty()) {
        return;
    }
    AqBoundary(Cp::kGraphicsQueue);
    if (!Begin()) {
        return;
    }
    cur_.aq_main = true;
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
    if (!flip_queued_) {
        EvictGpuImages(); // (a queued flip leaves it to the next drained one: EvictionDue)
    }
    eviction_due_.store(images_.OverBudget(), std::memory_order_release);
    CaptureImageAt();
    TexDumpFlip();
    DrawHistogram();
    RaceCheck();
    CmdTraceFlip();
    UpscaleOnFlip(); // (upscaling: the next frame's jitter, its inputs to find again)
    ++stats_.flips;
    if (prof_ && ++prof_frames_ >= 300) {
        ProfReport();
    }
    write_run_ = false;
    VideoOut::BufferInfo buf;
    s32 index = -1;
    s32 frame = -1;
    const bool has = VideoOut::NextFlipBuffer(buf, index);
    if (has && presenter_ != nullptr) {
        AqBoundary(Cp::kGraphicsQueue);
    }
    if (has && presenter_ != nullptr && Begin()) {
        cur_.aq_main = true;
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
                    UpscaleFrameGenInputs(frame, img); // (frame generation: depth, motion, the HUD-less frame)
                    CaptureSeries(*img, shown, w, h);
                    MenuFocusProbe(*img, shown, w, h);
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
    const bool flipped = VideoOut::OnFlipMarker(value, frame);
    if (flipped) {
        Reflex::RenderSubmitEnd(VideoOut::NextFlipId() - 1); // (NVIDIA Reflex: the frame's commands are submitted)
    }
    if (flipped && flip_hook_ != nullptr) {
        flip_hook_(flip_ctx_, value);
        if (insp_catalog_ != nullptr) {
            insp_frame_ends_.push_back({insp_frame_, value}); // (Poll hands its GPU time on once it completed)
        }
        ++insp_frame_; // (FrameLog's flip marker index: the hook's calls)
    }
    if (!flip_queued_) {
        frontend_.NewFrame(); // (queued: the processor's thread began the frontend's frame at the marker)
    }
}

void Executor::FlipQueued(const Cp::FlipMarker& marker) {
    flip_queued_ = true;
    Flip(marker);
    flip_queued_ = false;
}

void RetargetWriteWatch(u64 address) {
    g_watch_target.store(address, std::memory_order_release);
}

} // namespace SecondSon::Gpu::Vk
