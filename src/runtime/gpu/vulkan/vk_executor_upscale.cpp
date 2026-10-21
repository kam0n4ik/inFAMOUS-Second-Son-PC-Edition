// SecondSonPC — temporal upscaling inside the native backend's frame (docs/UPSCALING_FRAME_GENERATION.md).
//
// The game renders its scene at the render size the loader wrote into its engine (Engine::Render::render_resolution:
// the PS4 Pro path, where the scene is smaller than the 1920x1080 display buffer) and scales it to the display buffer
// in its last draw, under which it then draws its UI at full resolution. Measured on that frame (render trace with
// 1280x720, logs\upscale\exp1):
//   velocity pass (CFxVelocityBuffer, pass 29)  RG16F render-size target, depth-tested against the scene depth (D32F).
//                                              Full motion (camera and objects): current minus previous position in
//                                              UV of the render size, y down (reprojection of the previous frame: error
//                                              3.1-3.5 against 16-22 without or with the other sign).
//   tone mapping / SMAA / motion blur (pass 32) the finished scene, RGBA8 at render size
//   UI (pass 34)                                draws into that scene with the scene depth target, which it overwrites
//   post                                         the display buffer: the scene sampled and scaled, then the UI on top
// So: depth and motion are copied when the velocity pass ends (before the UI pass changes the depth), and at the first
// draw into a display buffer that samples a render-size scene the upscaler runs on the scene, its output (display
// size) sampled by that draw in place of the scene - the game's own scaling replaced, its UI drawn over the result at
// full resolution. Scene geometry (draws with the scene depth before the velocity pass, not full-screen passes) gets a
// sub-pixel viewport offset (Halton 2,3) that the upscaler is told about; the velocity pass itself is not jittered (the
// game's motion carries no jitter).
#include "runtime/gpu/vulkan/vk_executor_upscale.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/vulkan/vk_capture.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_emit.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "engine/guest_hooks.h"

namespace SecondSon::Gpu::Vk {

namespace {

u64 NowTicks() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<u64>(t.QuadPart);
}

double TicksToMs(u64 ticks) {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) * 1000.0 / freq;
}

std::string Env(const char* name) {
    char v[128] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

bool IsRgba8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_B8G8R8A8_SRGB;
}

void Barrier(VkCommandBuffer cb, UpscaleImage& img, VkImageLayout to, VkPipelineStageFlags2 src_stage,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = src_stage,
                                  .srcAccessMask = src_access,
                                  .dstStageMask = dst_stage,
                                  .dstAccessMask = dst_access,
                                  .oldLayout = img.layout,
                                  .newLayout = to,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = img.image,
                                  .subresourceRange = {aspect, 0, 1, 0, 1}};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
    Fn().vkCmdPipelineBarrier2(cb, &dep);
    img.layout = to;
}

void DestroyImage(DeviceContext& ctx, UpscaleImage& img) {
    const auto& f = Fn();
    const VkDevice dev = ctx.Device();
    for (auto& v : img.views) {
        f.vkDestroyImageView(dev, v.view, nullptr);
    }
    img.views.clear();
    if (img.view != VK_NULL_HANDLE) {
        f.vkDestroyImageView(dev, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        f.vkDestroyImage(dev, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        f.vkFreeMemory(dev, img.memory, nullptr);
    }
    img = UpscaleImage{};
}

/// (Re)creates `img` as `width` x `height` `format` when it differs. `mutable_formats`: an RGBA8 image whose other
/// RGBA8 views are made on demand (sampled only).
bool EnsureImage(DeviceContext& ctx, UpscaleImage& img, VkFormat format, u32 width, u32 height,
                 VkImageUsageFlags usage, bool mutable_formats, std::string& error) {
    if (img.image != VK_NULL_HANDLE && img.format == format && img.width == width && img.height == height) {
        return true;
    }
    const auto& f = Fn();
    if (img.image != VK_NULL_HANDLE) {
        f.vkDeviceWaitIdle(ctx.Device()); // (a size or format change: rare - a new render size or output)
        DestroyImage(ctx, img);
    }
    const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .flags = mutable_formats
                                            ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT}
                                            : VkImageCreateFlags{0},
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = format,
                               .extent = {width, height, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (f.vkCreateImage(ctx.Device(), &ci, nullptr, &img.image) != VK_SUCCESS) {
        error = fmt::format("vkCreateImage {}x{} format {}", width, height, static_cast<int>(format));
        img.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(ctx.Device(), img.image, &req);
    const s32 type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = static_cast<u32>(std::max(type, 0))};
    if (type < 0 || f.vkAllocateMemory(ctx.Device(), &ai, nullptr, &img.memory) != VK_SUCCESS ||
        f.vkBindImageMemory(ctx.Device(), img.image, img.memory, 0) != VK_SUCCESS) {
        error = "no video memory for an upscaler image";
        DestroyImage(ctx, img);
        return false;
    }
    const VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = img.image,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = format,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (f.vkCreateImageView(ctx.Device(), &vi, nullptr, &img.view) != VK_SUCCESS) {
        error = "vkCreateImageView";
        DestroyImage(ctx, img);
        return false;
    }
    img.format = format;
    img.width = width;
    img.height = height;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

/// A sampled view of a mutable RGBA8 image in an RGBA8 format with a swizzle (the scene texture's view's).
VkImageView ViewAs(DeviceContext& ctx, UpscaleImage& img, VkFormat format, const VkComponentMapping& comps) {
    const auto same = [&](const VkComponentMapping& a) {
        return a.r == comps.r && a.g == comps.g && a.b == comps.b && a.a == comps.a;
    };
    for (const auto& v : img.views) {
        if (v.format == format && same(v.components)) {
            return v.view;
        }
    }
    const VkImageViewUsageCreateInfo usage{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                           .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    const VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .pNext = &usage,
                                   .image = img.image,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = format,
                                   .components = comps,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (Fn().vkCreateImageView(ctx.Device(), &vi, nullptr, &v) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    img.views.push_back({format, comps, v});
    return v;
}

constexpr u32 kRcasSpv[] = {
#include "runtime/gpu/upscale/rcas_spv.inc"
};

/// The RCAS compute pipeline (push descriptors: the input sampled, the output stored; push constant: sharpness).
bool EnsureRcas(DeviceContext& ctx, VkDescriptorSetLayout& set, VkPipelineLayout& layout, VkPipeline& pipeline) {
    if (pipeline != VK_NULL_HANDLE) {
        return true;
    }
    const auto& f = Fn();
    const VkDevice dev = ctx.Device();
    const VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    const VkDescriptorSetLayoutCreateInfo dl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                             .bindingCount = 2,
                                             .pBindings = b};
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkShaderModule module = VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(kRcasSpv),
                                      .pCode = kRcasSpv};
    if (f.vkCreateDescriptorSetLayout(dev, &dl, nullptr, &set) != VK_SUCCESS) {
        return false;
    }
    const VkPipelineLayoutCreateInfo pl{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1,
                                        .pSetLayouts = &set,
                                        .pushConstantRangeCount = 1,
                                        .pPushConstantRanges = &pr};
    if (f.vkCreatePipelineLayout(dev, &pl, nullptr, &layout) != VK_SUCCESS ||
        f.vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) {
        return false;
    }
    const VkComputePipelineCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = layout};
    const VkResult r = f.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    f.vkDestroyShaderModule(dev, module, nullptr);
    return r == VK_SUCCESS;
}

Upscale::Image Ref(const UpscaleImage& img) {
    return Upscale::Image{img.image, img.view, img.format, img.width, img.height, VK_IMAGE_ASPECT_COLOR_BIT};
}

} // namespace

Executor::UpscaleState::~UpscaleState() {
    dlss.reset(); // (NGX's feature and shutdown first: the device is idle)
    fsr.reset();
    DestroyImage(ctx, color);
    DestroyImage(ctx, depth);
    DestroyImage(ctx, motion);
    DestroyImage(ctx, output);
    DestroyImage(ctx, sharpened);
    DestroyBuffer(ctx, depth_staging);
    if (rcas_pipeline != VK_NULL_HANDLE) {
        Fn().vkDestroyPipeline(ctx.Device(), rcas_pipeline, nullptr);
    }
    if (rcas_layout != VK_NULL_HANDLE) {
        Fn().vkDestroyPipelineLayout(ctx.Device(), rcas_layout, nullptr);
    }
    if (rcas_set != VK_NULL_HANDLE) {
        Fn().vkDestroyDescriptorSetLayout(ctx.Device(), rcas_set, nullptr);
    }
    if (queries != VK_NULL_HANDLE) {
        Fn().vkDestroyQueryPool(ctx.Device(), queries, nullptr);
    }
}

void Executor::InvalidateBoundState() {
    dyn_.valid = false;
    dyn_.bias_set = false;
    dyn_.color_masks_count = ~0u;
    dyn_vertex_valid_ = false;
    gfx_pipeline_ = VK_NULL_HANDLE;
    push_layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
    vb_valid_ = false;
    ib_last_ = VK_NULL_HANDLE;
    ib_type_last_ = VK_INDEX_TYPE_MAX_ENUM;
}

void Executor::UpscaleOnTargets(const Cp::DrawDescriptor& d, const std::array<CachedImage*, 8>& color,
                                CachedImage* depth, u32 depth_control) {
    jitter_draw_ = false;
    up_display_draw_ = false;
    // SSPC_CITY_HOT_LOCKS=1 (SSPC_CITY_PERFORMANCE; docs/CITY_PERFORMANCE.md): upscaling off is seen without copying
    // the settings under their lock at each draw
    static const bool hot_locks = Env("SSPC_CITY_HOT_LOCKS") == "1";
    if (hot_locks && !Upscale::BackendOn()) {
        return;
    }
    const Upscale::Settings s = Upscale::GetSettings();
    if (s.backend == Upscale::Backend::Off) {
        return;
    }
    if (!up_) {
        up_ = std::make_unique<UpscaleState>(ctx_);
        UpscaleState& u = *up_;
        if (const std::string v = Env("SSPC_UPSCALE_JITTER_SIGN"); !v.empty()) {
            float x = 1, y = 1;
            if (std::sscanf(v.c_str(), "%f,%f", &x, &y) == 2) {
                u.jitter_sign_x = x;
                u.jitter_sign_y = y;
            }
        }
        if (const std::string v = Env("SSPC_UPSCALE_MV_SIGN"); !v.empty()) {
            u.mv_sign = static_cast<float>(std::atof(v.c_str()));
        }
        // the game's depth is reversed (measured: the sky 0, the scene 0.0003..0.033 nearer the camera);
        // SSPC_UPSCALE_DEPTH_INVERTED=0 tells the upscaler otherwise (diagnostics)
        u.depth_inverted = Env("SSPC_UPSCALE_DEPTH_INVERTED") != "0";
        if (const std::string v = Env("SSPC_UPSCALE_DUMP"); !v.empty()) {
            u.dump_flip = std::strtoull(v.c_str(), nullptr, 10);
        }
    }
    UpscaleState& u = *up_;
    CachedImage* c0 = color[0];
    // the velocity pass: its draws write an RG16F target of the scene's size and test the scene depth
    if (c0 != nullptr && c0->format == VK_FORMAT_R16G16_SFLOAT && depth != nullptr &&
        c0->layout.size.width == depth->layout.size.width && c0->layout.size.height == depth->layout.size.height) {
        u.mv_img = c0;
        u.vel_depth = depth;
        u.velocity_open = true;
        return;
    }
    if (u.velocity_open) {
        // the velocity pass ended: depth and motion copied now (the UI pass draws into the scene depth after it)
        u.velocity_open = false;
        u.scene_depth_addr = u.vel_depth->layout.guest_address;
        std::string err;
        const u32 w = u.vel_depth->layout.size.width, h = u.vel_depth->layout.size.height;
        u.frame_rw = w;
        u.frame_rh = h;
        const bool ok =
            EnsureImage(ctx_, u.depth, VK_FORMAT_R32_SFLOAT, w, h,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        false, err) &&
            EnsureImage(ctx_, u.motion, VK_FORMAT_R16G16_SFLOAT, u.mv_img->layout.size.width,
                        u.mv_img->layout.size.height,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        false, err);
        if (ok && u.depth_staging.size < u64{w} * h * 4) {
            DestroyBuffer(ctx_, u.depth_staging);
            if (!CreateBuffer(ctx_, u64{w} * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              false, u.depth_staging, err, false, HostPlacement::Device)) {
                u.depth_staging = {};
            }
        }
        if (!ok || u.depth_staging.buffer == VK_NULL_HANDLE) {
            Upscale::UpdateStatus([&](Upscale::Status& st) { st.reason = "no memory for the inputs: " + err; });
            return;
        }
        const auto& f = Fn();
        EndRendering();
        if (pending_writes_) {
            GlobalBarrier();
        }
        images_.Transition(cur_.cb, *u.vel_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        images_.Transition(cur_.cb, *u.mv_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        // depth: its depth aspect through a buffer into R32F (a depth image cannot be copied into a colour one)
        const VkBufferImageCopy region{.bufferOffset = 0,
                                       .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                       .imageExtent = {w, h, 1}};
        f.vkCmdCopyImageToBuffer(cur_.cb, u.vel_depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 u.depth_staging.buffer, 1, &region);
        const VkBufferMemoryBarrier2 bb{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = u.depth_staging.buffer,
                                        .size = VK_WHOLE_SIZE};
        const VkDependencyInfo bdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1,
                                    .pBufferMemoryBarriers = &bb};
        Barrier(cur_.cb, u.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        Barrier(cur_.cb, u.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        f.vkCmdPipelineBarrier2(cur_.cb, &bdep);
        const VkBufferImageCopy back{.bufferOffset = 0,
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {w, h, 1}};
        f.vkCmdCopyBufferToImage(cur_.cb, u.depth_staging.buffer, u.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &back);
        const VkImageCopy mc{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .extent = {u.motion.width, u.motion.height, 1}};
        f.vkCmdCopyImage(cur_.cb, u.mv_img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.motion.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mc);
        Barrier(cur_.cb, u.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        Barrier(cur_.cb, u.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        u.snapshot_valid = true;
    }
    // a draw into a display buffer (the post pass)
    if (c0 != nullptr && c0->layout.size.width >= 1280 && IsRgba8(c0->format) &&
        VideoOut::IsDisplayBuffer(c0->layout.guest_address)) {
        if (u.done_this_frame && !u.hudless_taken && presenter_ != nullptr && presenter_->FrameGenActive()) {
            // frame generation: the frame as the upscaled scene left it, before this first UI draw
            TakeHudless(*c0);
        }
        up_display_draw_ = !u.done_this_frame;
        up_sampled_.clear();
        return;
    }
    // scene geometry: tested against the scene depth before the velocity pass, not a full-screen pass (a quad or a
    // triangle: lighting, composites, clears)
    if (depth != nullptr && !u.snapshot_valid && depth->layout.guest_address == u.scene_depth_addr) {
        ++u.scene_candidates;
    }
    if (s.jitter && depth != nullptr && !u.snapshot_valid && depth->layout.guest_address == u.scene_depth_addr &&
        ((depth_control >> 1) & 1) != 0 && (d.indirect || d.index_count > 6 || d.instance_count > 1)) {
        ++u.jittered_draws;
        jitter_draw_ = true;
        jitter_x_ = u.frame_jx;
        jitter_y_ = u.frame_jy;
        const u32 rw = depth->layout.size.width; // (the scene depth: this frame's render size)
        // textures of the scene at the output's detail: the upscaler rebuilds it from the jittered samples
        lod_bias_ = u.last_out_w > rw && rw != 0 ? std::log2(static_cast<float>(rw) / static_cast<float>(u.last_out_w))
                                                 : 0.0f;
        u.jitter_this_frame = true;
    }
}

void Executor::UpscaleDisplayDraw(const std::array<CachedImage*, 8>& color,
                                  std::vector<VkDescriptorImageInfo>& images) {
    if (!up_display_draw_ || !up_) {
        return;
    }
    up_display_draw_ = false;
    UpscaleState& u = *up_;
    const Upscale::Settings s = Upscale::GetSettings();
    // this frame's render size: its velocity pass's (frames recorded before a size change keep theirs)
    u32 rw = u.frame_rw, rh = u.frame_rh;
    if (rw == 0) {
        Upscale::GetRenderSize(rw, rh);
    }
    // the scene this draw samples: a GPU-written RGBA8 texture of the render size
    const UpscaleSampled* scene = nullptr;
    for (const UpscaleSampled& t : up_sampled_) {
        if (t.img != nullptr && t.img->gpu_written && IsRgba8(t.img->format) && t.width == rw && t.height == rh &&
            t.img != color[0]) {
            scene = &t;
            break;
        }
    }
    if (scene == nullptr) {
        up_sampled_.clear();
        return; // a display draw of something else (menus, movies, the UI's own layers)
    }
    const UpscaleSampled sc = *scene;
    up_sampled_.clear();
    const auto fail = [&](std::string why) {
        if (why != u.last_fail) { // (once per distinct reason, not every frame)
            SSPC_LOG(GPU, Warning, "[Upscale] not this frame: {}", why);
            u.last_fail = why;
        }
        Upscale::UpdateStatus([&](Upscale::Status& st) {
            st.active = Upscale::Backend::Off;
            st.reason = std::move(why);
        });
    };
    if (!u.snapshot_valid) {
        fail("no velocity pass in this frame (the game's own scaling)");
        u.reset = true;
        return;
    }
    // the backend: created on first use (DLSS's NGX was prepared with the device)
    const Upscale::DeviceInfo di{ctx_.Instance(), ctx_.Physical(), ctx_.Device(), Fn().vkGetInstanceProcAddr,
                                 Fn().vkGetDeviceProcAddr, ctx_.VendorId()};
    if (s.backend == Upscale::Backend::Dlss && !u.dlss && !u.tried_dlss) {
        u.tried_dlss = true;
        u.dlss = Upscale::Dlss::Create(di, u.dlss_why);
        if (u.dlss) {
            SSPC_LOG(GPU, Info, "[Upscale] DLSS ready");
        } else {
            SSPC_LOG(GPU, Warning, "[Upscale] DLSS unavailable: {}", u.dlss_why);
        }
    }
    if (s.backend == Upscale::Backend::Fsr && !u.fsr && !u.tried_fsr) {
        u.tried_fsr = true;
        u.fsr = Upscale::Fsr::Create(di, u.fsr_why);
        if (u.fsr) {
            SSPC_LOG(GPU, Info, "[Upscale] FSR 3.1 ready");
        } else {
            SSPC_LOG(GPU, Warning, "[Upscale] FSR 3.1 unavailable: {}", u.fsr_why);
        }
    }
    const bool use_dlss = s.backend == Upscale::Backend::Dlss && u.dlss != nullptr;
    const bool use_fsr = s.backend == Upscale::Backend::Fsr && u.fsr != nullptr;
    if (!use_dlss && !use_fsr) {
        fail(s.backend == Upscale::Backend::Dlss ? "DLSS unavailable: " + u.dlss_why
                                                  : "FSR unavailable: " + u.fsr_why);
        return;
    }
    // the output: the display buffer's size (as VideoOut registered it)
    u32 out_w = 1920, out_h = 1080;
    {
        VideoOut::BufferInfo b{};
        s32 idx = -1;
        if (VideoOut::NextFlipBuffer(b, idx) && b.width != 0 && b.height != 0) {
            out_w = b.width;
            out_h = b.height;
        }
    }
    std::string err;
    if (!EnsureImage(ctx_, u.color, VK_FORMAT_R8G8B8A8_UNORM, rw, rh,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     false, err) ||
        !EnsureImage(ctx_, u.output, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     true, err)) {
        fail("no memory: " + err);
        return;
    }
    const VkImageView out_view = ViewAs(ctx_, u.output, sc.format, sc.components);
    if (out_view == VK_NULL_HANDLE) {
        fail(fmt::format("no view of the output as format {}", static_cast<int>(sc.format)));
        return;
    }
    const auto& f = Fn();
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    VkCommandBuffer cb = cur_.cb;
    // (re)create the feature / context: sizes, quality or backend changed
    const bool changed = u.last_w != rw || u.last_h != rh || u.last_out_w != out_w || u.last_out_h != out_h ||
                         u.last_backend != s.backend || u.last_quality != s.quality;
    if (use_dlss && u.dlss->NeedsRecreate(rw, rh, out_w, out_h, s.quality, false, u.depth_inverted)) {
        if (u.last_backend != Upscale::Backend::Off) {
            f.vkDeviceWaitIdle(ctx_.Device());
            u.dlss->ReleaseFeature();
        }
        if (!u.dlss->CreateFeature(cb, rw, rh, out_w, out_h, s.quality, false, u.depth_inverted, err)) {
            fail(err);
            u.dlss.reset();
            u.dlss_why = err;
            return;
        }
        SSPC_LOG(GPU, Info, "[Upscale] DLSS {}x{} -> {}x{} ({})", rw, rh, out_w, out_h, Upscale::QualityName(s.quality));
    }
    if (use_fsr && u.fsr->NeedsRecreate(rw, rh, out_w, out_h, false, u.depth_inverted)) {
        f.vkDeviceWaitIdle(ctx_.Device());
        if (!u.fsr->CreateContext(rw, rh, out_w, out_h, false, u.depth_inverted, err)) {
            fail(err);
            u.fsr.reset();
            u.fsr_why = err;
            return;
        }
    }
    if (changed) {
        u.reset = true;
    }
    u.last_w = rw;
    u.last_h = rh;
    u.last_out_w = out_w;
    u.last_out_h = out_h;
    u.last_backend = s.backend;
    u.last_quality = s.quality;
    // the scene's colour (the texture's part: the T#'s size of the possibly taller target)
    images_.Transition(cb, *sc.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    Barrier(cb, u.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageCopy cc{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {rw, rh, 1}};
    f.vkCmdCopyImage(cb, sc.img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.color.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
    Barrier(cb, u.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    // the output: written in full (its last contents are the upscaler's history's business, not ours)
    Barrier(cb, u.output, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    // timing
    if (u.queries == VK_NULL_HANDLE) {
        const VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = UpscaleState::kQuerySlots * 2};
        f.vkCreateQueryPool(ctx_.Device(), &qi, nullptr, &u.queries);
    }
    const u32 slot = static_cast<u32>(u.evals % UpscaleState::kQuerySlots);
    if (u.queries != VK_NULL_HANDLE) {
        if (u.evals >= UpscaleState::kQuerySlots / 2) { // the slot written kQuerySlots/2 evaluations ago
            const u32 old = static_cast<u32>((u.evals - UpscaleState::kQuerySlots / 2) % UpscaleState::kQuerySlots);
            u64 ts[2] = {};
            if (f.vkGetQueryPoolResults(ctx_.Device(), u.queries, old * 2, 2, sizeof(ts), ts, sizeof(u64),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[1] > ts[0]) {
                const float ms = static_cast<float>(static_cast<double>(ts[1] - ts[0]) *
                                                    ctx_.Caps().timestamp_period * 1e-6);
                Upscale::UpdateStatus([&](Upscale::Status& st) { st.gpu_ms = ms; });
                u.gpu_ms_sum += ms;
                if (++u.gpu_ms_n == 600) { // (a log line every 600 measured evaluations: the upscaler's GPU cost)
                    SSPC_LOG(GPU, Info, "[Upscale] {} {}: GPU {:.3f} ms per frame (mean of 600, the evaluation{})", Upscale::BackendName(s.backend), Upscale::QualityName(s.quality),
                             u.gpu_ms_sum / 600.0, use_dlss && s.sharpness > 0.0f ? "; RCAS not included" : "");
                    u.gpu_ms_sum = 0.0;
                    u.gpu_ms_n = 0;
                }
            }
        }
        f.vkCmdResetQueryPool(cb, u.queries, slot * 2, 2);
        f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, u.queries, slot * 2);
    }
    const u64 now = NowTicks();
    const double since = u.last_eval_ticks != 0 ? TicksToMs(now - u.last_eval_ticks) : 33.3;
    u.last_eval_ticks = now;
    if (since > 250.0) {
        u.reset = true; // a pause, a loading screen, a menu: the history is stale
    }
    Upscale::Inputs in;
    in.color = Ref(u.color);
    in.depth = Ref(u.depth);
    in.motion = Ref(u.motion);
    in.output = Ref(u.output);
    in.render_width = rw;
    in.render_height = rh;
    // the jitter the scene was drawn with (none in a frame whose draws were not jittered: the first, or jitter off)
    in.jitter_x = u.jitter_this_frame ? u.frame_jx * u.jitter_sign_x : 0.0f;
    in.jitter_y = u.jitter_this_frame ? u.frame_jy * u.jitter_sign_y : 0.0f;
    in.mv_scale_x = u.mv_sign * static_cast<float>(rw);
    in.mv_scale_y = u.mv_sign * static_cast<float>(rh);
    in.reset = u.reset;
    in.hdr = false; // the scene after the game's tone mapping (sRGB-encoded RGBA8)
    in.depth_inverted = u.depth_inverted;
    in.frame_ms = static_cast<float>(std::clamp(since, 1.0, 100.0));
    in.sharpness = std::clamp(s.sharpness, 0.0f, 1.0f);
    Emit::Sync(); // the SDKs record with their own Vulkan functions: the emitter's commands first
    const bool ok = use_dlss ? u.dlss->Evaluate(cb, in) : u.fsr->Evaluate(cb, in);
    InvalidateBoundState(); // the SDK recorded its own pipelines, descriptors and push constants
    if (u.queries != VK_NULL_HANDLE) {
        f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, u.queries, slot * 2 + 1);
    }
    ++u.evals;
    if (!ok) {
        fail(use_dlss ? "DLSS evaluation failed" : "FSR dispatch failed");
        u.reset = true;
        return;
    }
    u.reset = false;
    // SSPC_UPSCALE_DUMP=<flip>: what the upscaler got and gave at that flip (verification of the inputs)
    if (u.dump_flip != ~0ull && VideoOut::NextFlipId() >= u.dump_flip) { // (the first evaluation from that flip on)
        const u64 flip_dumped = u.dump_flip;
        u.dump_flip = ~0ull;
        if (!readback_) {
            readback_ = std::make_unique<ImageReadback>(ctx_);
        }
        const std::filesystem::path dir = options_.capture_path.parent_path().parent_path() / "upscale";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto dump = [&](UpscaleImage& img, const char* name, u32 texel) {
            const VkImageLayout back = img.layout;
            Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
            readback_->Record(cb, cur_.value, img.image, img.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, img.width,
                              img.height, texel, dir / fmt::format("{}_{}", name, flip_dumped), true);
            Barrier(cb, img, back, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        };
        dump(u.color, "color", 4);
        dump(u.depth, "depth", 4);
        dump(u.motion, "motion", 4);
        dump(u.output, "output", 4);
        SSPC_LOG(GPU, Warning, "[Upscale] SSPC_UPSCALE_DUMP: flip {} inputs/output -> {} (jitter {:.3f},{:.3f}; "
                               "{} of {} draws on the scene depth jittered, lod bias {:.3f})",
                 flip_dumped, dir.string(), in.jitter_x, in.jitter_y, u.jittered_draws, u.scene_candidates, lod_bias_);
    }
    Barrier(cb, u.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
    VkImageView shown_view = out_view;
    // DLSS has no sharpening: RCAS over its output with the player's sharpness (FSR applied its own)
    if (use_dlss && in.sharpness > 0.0f &&
        EnsureRcas(ctx_, u.rcas_set, u.rcas_layout, u.rcas_pipeline) &&
        EnsureImage(ctx_, u.sharpened, VK_FORMAT_R8G8B8A8_UNORM, out_w, out_h,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true,
                    err)) {
        Barrier(cb, u.sharpened, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        const VkDescriptorImageInfo src{VK_NULL_HANDLE, u.output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo dst{VK_NULL_HANDLE, u.sharpened.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &src},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dst}};
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, u.rcas_pipeline);
        f.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, u.rcas_layout, 0, 2, w);
        const float sharp = in.sharpness;
        f.vkCmdPushConstants(cb, u.rcas_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &sharp);
        f.vkCmdDispatch(cb, (out_w + 7) / 8, (out_h + 7) / 8, 1);
        InvalidateBoundState();
        Barrier(cb, u.sharpened, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        if (const VkImageView v = ViewAs(ctx_, u.sharpened, sc.format, sc.components); v != VK_NULL_HANDLE) {
            shown_view = v;
        }
    }
    // the draw samples the upscaled scene in the scene's place
    u32 replaced = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].imageView == sc.view) {
            images[i].imageView = shown_view;
            images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (i < info_images_.size()) {
                info_images_[i] = nullptr; // (its layout is ours, not a cached image's)
            }
            ++replaced;
        }
    }
    u.done_this_frame = true;
    u.last_fail.clear();
    Upscale::UpdateStatus([&](Upscale::Status& st) {
        st.active = s.backend;
        st.reason.clear();
        st.render_w = rw;
        st.render_h = rh;
        st.out_w = out_w;
        st.out_h = out_h;
        ++st.frames;
    });
    if (!u.logged_first) {
        u.logged_first = true;
        SSPC_LOG(GPU, Warning,
                 "[Upscale] {} {} {}x{} -> {}x{}: scene {:#x} (format {}), depth {:#x}, motion {:#x}; {} view(s) "
                 "replaced in the display draw",
                 Upscale::BackendName(s.backend), Upscale::QualityName(s.quality), rw, rh, out_w, out_h,
                 sc.img->layout.guest_address, static_cast<int>(sc.format), u.scene_depth_addr,
                 u.mv_img != nullptr ? u.mv_img->layout.guest_address : 0, replaced);
    }
}

void RetargetWriteWatch(u64 address); // (vk_executor.cpp: SSPC_DIAG_WATCH moved to another address)

namespace {
/// SSPC_UPSCALE_SCAN_JITTER=<flip> (diagnostics): the guest memory scanned for the game's own camera jitter (SMAA T2x:
/// +-0.5/width, +-0.5/height in clip space) at that flip and the next -> the log.
void ScanJitter(u64 flip, u32 rw, u32 rh) {
    static const u64 at = [] {
        char v[24] = {};
        return GetEnvironmentVariableA("SSPC_UPSCALE_SCAN_JITTER", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10)
                                                                                     : 0ull;
    }();
    if (at == 0 || flip < at || flip > at + 1 || rw == 0 || rh == 0) {
        return;
    }
    const float jx = 0.5f / static_cast<float>(rw), jy = 0.5f / static_cast<float>(rh);
    const auto close_to = [](float v, float want) { return std::fabs(std::fabs(v) - want) < want * 1e-3f; };
    u32 found = 0;
    MEMORY_BASIC_INFORMATION mi{};
    for (u64 a = 0x200000000ull; a < 0x1000000000ull && found < 64;) {
        if (VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) == 0) {
            break;
        }
        const u64 end = reinterpret_cast<u64>(mi.BaseAddress) + mi.RegionSize;
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) != 0 &&
            (mi.Protect & PAGE_GUARD) == 0) {
            const auto* f = reinterpret_cast<const float*>(mi.BaseAddress);
            const u64 n = mi.RegionSize / 4;
            for (u64 i = 0; i + 1 < n && found < 64; ++i) {
                if (close_to(f[i], jx) && (close_to(f[i + 1], jy) || (i + 4 < n && close_to(f[i + 4], jy)))) {
                    SSPC_LOG(GPU, Warning, "DIAGNOSTIC SSPC_UPSCALE_SCAN_JITTER flip {}: {:#x}: {:.9g} {:.9g} {:.9g} {:.9g} "
                                           "{:.9g}", flip, reinterpret_cast<u64>(&f[i]), f[i], f[i + 1],
                             i + 2 < n ? f[i + 2] : 0.0f, i + 3 < n ? f[i + 3] : 0.0f, i + 4 < n ? f[i + 4] : 0.0f);
                    ++found;
                    // a CPU-side camera projection (x, -y offsets, near term, w = 1), not a GPU arena copy: watched
                    static bool retargeted = false;
                    if (!retargeted && reinterpret_cast<u64>(&f[i]) < 0x240000000ull && i + 3 < n && f[i + 3] == 1.0f) {
                        retargeted = true;
                        RetargetWriteWatch(reinterpret_cast<u64>(&f[i]));
                    }
                }
            }
        }
        a = end;
    }
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC SSPC_UPSCALE_SCAN_JITTER flip {}: {} candidates for +-{:.9g}, +-{:.9g}", flip,
             found, jx, jy);
}
} // namespace

void Executor::TakeHudless(CachedImage& display) {
    UpscaleState& u = *up_;
    std::string err;
    if (!EnsureImage(ctx_, u.hudless, display.format, display.layout.size.width, display.layout.size.height,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, err)) {
        return;
    }
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    images_.Transition(cur_.cb, display, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    Barrier(cur_.cb, u.hudless, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageCopy c{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .extent = {u.hudless.width, u.hudless.height, 1}};
    Fn().vkCmdCopyImage(cur_.cb, display.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.hudless.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cur_.cb, u.hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    u.hudless_taken = true;
}

void Executor::UpscaleFrameGenInputs(s32 frame, CachedImage* display) {
    if (!up_ || presenter_ == nullptr || !presenter_->FrameGenActive() || frame < 0) {
        return;
    }
    UpscaleState& u = *up_;
    if (!u.flip_done || u.depth.image == VK_NULL_HANDLE || u.motion.image == VK_NULL_HANDLE) {
        return; // a frame without the scene's inputs: presented alone
    }
    if (!u.hudless_taken && display != nullptr) {
        TakeHudless(*display); // (no UI drawn this frame: the frame itself)
    }
    const bool taken = u.hudless_taken;
    u.hudless_taken = false; // (the next frame takes its own)
    if (!taken) {
        return;
    }
    const u32 rw = u.depth.width, rh = u.depth.height; // (this frame's render size)
    Presenter::FrameGenSource src;
    src.depth = u.depth.image;
    src.motion = u.motion.image;
    src.hudless = u.hudless.image;
    src.hudless_format = u.hudless.format;
    src.depth_w = u.depth.width;
    src.depth_h = u.depth.height;
    src.motion_w = u.motion.width;
    src.motion_h = u.motion.height;
    src.hudless_w = std::min<u32>(u.hudless.width, u.last_out_w != 0 ? u.last_out_w : u.hudless.width);
    src.hudless_h = std::min<u32>(u.hudless.height, u.last_out_h != 0 ? u.last_out_h : u.hudless.height);
    src.render_w = rw;
    src.render_h = rh;
    src.jitter_x = u.flip_jitter ? u.flip_jx * u.jitter_sign_x : 0.0f;
    src.jitter_y = u.flip_jitter ? u.flip_jy * u.jitter_sign_y : 0.0f;
    src.mv_scale_x = u.mv_sign * static_cast<float>(rw);
    src.mv_scale_y = u.mv_sign * static_cast<float>(rh);
    // the game's camera (Camera::BuildProjection 0x581c90: near +0x584, far +0x588, tan(fov/2) +0x5a4)
    if (const u64 base = Engine::GuestImageBase(); base != 0) {
        const u64 camera = *reinterpret_cast<const u64*>(base + 0x7f2d2b8);
        if (camera != 0) {
            src.camera_near = *reinterpret_cast<const float*>(camera + 0x584);
            src.camera_far = *reinterpret_cast<const float*>(camera + 0x588);
            src.fov_y = 2.0f * std::atan(*reinterpret_cast<const float*>(camera + 0x5a4));
        }
    }
    src.depth_inverted = u.depth_inverted;
    src.reset = u.flip_reset;
    presenter_->RecordFrameGenInputs(cur_.cb, frame, src);
}

void Executor::UpscaleOnFlip() {
    if (!up_) {
        return;
    }
    // (Flip copies the frame after this: frame generation reads what the frame had)
    up_->flip_done = up_->done_this_frame;
    up_->flip_jitter = up_->jitter_this_frame;
    up_->flip_jx = up_->frame_jx;
    up_->flip_jy = up_->frame_jy;
    up_->flip_reset = up_->reset;
    {
        u32 rw = 0, rh = 0;
        Upscale::GetRenderSize(rw, rh);
        ScanJitter(VideoOut::NextFlipId(), rw, rh);
    }
    UpscaleState& u = *up_;
    u.snapshot_valid = false;
    u.velocity_open = false;
    u.done_this_frame = false;
    u.jitter_this_frame = false;
    u.jittered_draws = u.scene_candidates = 0;
    const Upscale::Settings s = Upscale::GetSettings();
    if (s.backend == Upscale::Backend::Off || !s.jitter) {
        u.frame_jx = u.frame_jy = 0.0f;
        return;
    }
    u32 rw = u.frame_rw, rh = u.frame_rh; // (the last frame's size: a change shows one frame later)
    if (rw == 0) {
        Upscale::GetRenderSize(rw, rh);
    }
    ++u.jitter_index;
    Upscale::Jitter(u.jitter_index, rw, std::max<u32>(u.last_out_w, rw), u.frame_jx, u.frame_jy);
}

} // namespace SecondSon::Gpu::Vk
