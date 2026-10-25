// SecondSonPC — the backend's images (P4.4). See vk_image_cache.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_image_cache.h"

#include "runtime/gpu/page_tracker.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/host_shaders.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

bool SameImage(const ImageLayout& a, const ImageLayout& b) {
    return a.pixel_format == b.pixel_format && a.type == b.type && a.size == b.size && a.levels == b.levels &&
           a.layers == b.layers && a.num_samples == b.num_samples && a.tile_mode == b.tile_mode &&
           a.pitch == b.pitch && a.guest_size == b.guest_size;
}

constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                        VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;

} // namespace

ImageCache::ImageCache(DeviceContext& ctx, GuestBuffers& guest) : ctx_(ctx), guest_(guest) {
    const auto& f = Fn();
    char rule[8] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_ALIAS_RULE", rule, sizeof(rule)) == 3 && std::string_view(rule) == "off") {
        alias_rule_ = false;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_ALIAS_RULE=off - the image cache's equal-address "
                               "aliasing rule is disabled for this run (a control run, not a configuration)");
    }
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_GPU_EVICT_ALL", v, sizeof(v)) > 0) {
        // =1: the standard ages; =<frames> (> 1): a clean image counts as old after that many frames
        const u64 n = std::strtoull(v, nullptr, 10);
        force_gpu_evict_ = n != 0;
        force_age_scale_ = n > 1 ? std::max<u64>(kGpuEvictCleanFrames / n, 1) : 1;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_GPU_EVICT_ALL={} - every GPU-written image old enough is "
                               "evicted (written back) whatever the budget (a stress test of the write-back, P4.5)", n);
    }
    const VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                              .bindingCount = 3,
                                              .pBindings = b};
    f.vkCreateDescriptorSetLayout(ctx_.Device(), &dci, nullptr, &detile_set_layout_);
    const VkPipelineLayoutCreateInfo pci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &detile_set_layout_};
    f.vkCreatePipelineLayout(ctx_.Device(), &pci, nullptr, &detile_layout_);
}

ImageCache::~ImageCache() {
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    for (auto& img : retired_) {
        images_.push_back(std::move(img));
    }
    for (auto& img : images_) {
        for (auto& [k, v] : img->views) {
            f.vkDestroyImageView(dev, v, nullptr);
        }
        f.vkDestroyImage(dev, img->image, nullptr);
        f.vkFreeMemory(dev, img->memory, nullptr);
    }
    for (auto& [k, s] : samplers_) {
        f.vkDestroySampler(dev, s, nullptr);
    }
    for (auto& t : temps_) {
        f.vkDestroyBuffer(dev, t.buffer, nullptr);
        f.vkFreeMemory(dev, t.memory, nullptr);
    }
    for (auto& [k, p] : detile_pipelines_) {
        f.vkDestroyPipeline(dev, p, nullptr);
    }
    if (detile_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyPipelineLayout(dev, detile_layout_, nullptr);
    }
    if (detile_set_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyDescriptorSetLayout(dev, detile_set_layout_, nullptr);
    }
}

void ImageCache::Evict(u64 completed) {
    if (stats_.bytes <= budget_) {
        return;
    }
    // candidates: images uploaded from guest memory (re-uploadable), not used by a batch still in flight
    std::vector<CachedImage*> cand;
    for (const auto& img : images_) {
        if (!img->gpu_written && img->last_seq <= completed) {
            cand.push_back(img.get());
        }
    }
    std::ranges::sort(cand, [](const CachedImage* a, const CachedImage* b) { return a->last_seq < b->last_seq; });
    const u64 target = budget_ / 10 * 9;
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    std::vector<CachedImage*> gone;
    for (CachedImage* img : cand) {
        if (stats_.bytes <= target) {
            break;
        }
        for (auto& [k, v] : img->views) {
            f.vkDestroyImageView(dev, v, nullptr);
        }
        f.vkDestroyImage(dev, img->image, nullptr);
        f.vkFreeMemory(dev, img->memory, nullptr);
        const auto [lo, hi] = by_address_.equal_range(img->layout.guest_address);
        for (auto it = lo; it != hi; ++it) {
            if (it->second == img) {
                by_address_.erase(it);
                break;
            }
        }
        std::erase_if(meta_owner_, [&](const auto& e) { return e.second == img; });
        std::erase_if(stencil_owner_, [&](const auto& e) { return e.second == img; });
        stats_.bytes -= img->bytes;
        --stats_.images;
        ++stats_.evicted;
        stats_.evicted_bytes += img->bytes;
        gone.push_back(img);
    }
    if (gone.empty()) {
        return;
    }
    std::erase_if(images_, [&](const std::unique_ptr<CachedImage>& p) {
        return std::ranges::find(gone, p.get()) != gone.end();
    });
    ++epoch_; // what Find returned (the executor's target cache and texture memo) may be gone
}

void ImageCache::Queue(u64 addr, u64 size, InvalKind kind) {
    std::scoped_lock lock{inval_mutex_};
    pending_inval_.push_back({addr, size, kind});
    has_inval_.store(true, std::memory_order_release);
}

void ImageCache::Invalidate(u64 addr, u64 size) {
    Queue(addr, size, InvalKind::Exact);
}

void ImageCache::InvalidateFromBufferWrite(u64 addr, u64 size) {
    Queue(addr, size, InvalKind::BufferWrite);
}

void ImageCache::InvalidateCoarse(u64 addr, u64 size) {
    Queue(addr, size, InvalKind::Coarse);
}

void ImageCache::Unmap(u64 addr, u64 size) {
    Queue(addr, size, InvalKind::Unmap);
}

void ImageCache::Retire(CachedImage* img) {
    const auto [lo, hi] = by_address_.equal_range(img->layout.guest_address);
    for (auto it = lo; it != hi; ++it) {
        if (it->second == img) {
            by_address_.erase(it);
            break;
        }
    }
    std::erase_if(meta_owner_, [&](const auto& e) { return e.second == img; });
    std::erase_if(stencil_owner_, [&](const auto& e) { return e.second == img; });
    const auto it = std::ranges::find_if(images_, [&](const std::unique_ptr<CachedImage>& p) { return p.get() == img; });
    if (it == images_.end()) {
        return;
    }
    stats_.bytes -= img->bytes;
    --stats_.images;
    ++stats_.retired;
    stats_.retired_bytes += img->bytes;
    retired_.push_back(std::move(*it));
    images_.erase(it);
    ++epoch_; // what Find returned for it (the executor's target cache and texture memo) is gone
}

void ImageCache::TrackGpuWritten(CachedImage& img) {
    // its pages protected: the CPU's writes there reach ApplyInvalidations as coarse reports (interior blocks count)
    const ImageLayout& l = img.layout;
    if (tracker_ != nullptr && l.guest_size != 0 && tracker_->CanArm(l.guest_address, l.guest_size)) {
        tracker_->Arm(l.guest_address, l.guest_size);
        img.write_tracked = true;
        ++stats_.gpu_tracked;
    } else {
        img.write_tracked = false;
        ++stats_.gpu_untracked; // the CPU's writes there are not seen (memory imported through the guest's pages)
    }
}

void ImageCache::ApplyInvalidations() {
    if (!has_inval_.load(std::memory_order_acquire)) {
        return; // the common case: no lock taken
    }
    std::vector<PendingInval> ranges;
    {
        std::scoped_lock lock{inval_mutex_};
        has_inval_.store(false, std::memory_order_relaxed);
        if (pending_inval_.empty()) {
            return;
        }
        ranges.swap(pending_inval_);
    }
    ++gen_; // (images' flags may change)
    // only the images that can overlap each range (by address; it walked every image for every range: 7.8% of the GPU
    // thread's samples, f1)
    std::vector<CachedImage*> unmapped;
    for (const auto& [addr, size, kind] : ranges) {
        const u64 end = addr + size;
        auto it = by_address_.lower_bound(addr > max_image_size_ ? addr - max_image_size_ : 0);
        for (; it != by_address_.end() && it->first < end; ++it) {
            CachedImage* img = it->second;
            const u64 b = img->layout.guest_address, e = b + img->layout.guest_size;
            if (!(b < end && addr < e)) {
                continue;
            }
            const InvalidationEffect fx =
                ClassifyInvalidation(kind, b, img->layout.guest_size, img->gpu_written, img->uploaded, addr, end);
            if (fx.retire) {
                unmapped.push_back(img);
                continue;
            }
            if (fx.dirty) {
                img->dirty = true;
                ++stats_.invalidations;
            }
            img->maybe_newer |= fx.maybe_newer;
            const bool hit = fx.memory_newer;
            if (hit && !img->memory_newer) {
                img->memory_newer = true;
                ++stats_.memory_newer;
                ++stats_.memory_newer_by[static_cast<u32>(kind)];
                if (stats_.memory_newer <= 16) {
                    SSPC_LOG(GPU, Info,
                             "native backend: GPU-written img{} {:#x}+{:#x} fmt {} {}x{}: its memory was written ({} "
                             "{:#x}+{:#x}) - uploaded before its next use",
                             img->id, b, img->layout.guest_size, img->layout.pixel_format, img->layout.size.width,
                             img->layout.size.height,
                             kind == InvalKind::Exact ? "exact" : kind == InvalKind::BufferWrite ? "buffer write" : "CPU",
                             addr, size);
                }
            }
        }
    }
    std::ranges::sort(unmapped);
    unmapped.erase(std::unique(unmapped.begin(), unmapped.end()), unmapped.end());
    for (CachedImage* img : unmapped) {
        ++stats_.memory_newer_by[static_cast<u32>(InvalKind::Unmap)];
        Retire(img);
    }
}

CachedImage* ImageCache::Create(const ImageLayout& layout, std::string& error) {
    const auto& f = Fn();
    const VkFormat format = static_cast<VkFormat>(layout.pixel_format);
    const auto type = ConvertImageType(layout.type);
    if (!layout.supported || format == VK_FORMAT_UNDEFINED || !type.Valid()) {
        error = fmt::format("image at {:#x}: {}", layout.guest_address,
                            !layout.supported ? std::string(layout.reason) : "no host format or type");
        return nullptr;
    }
    auto img = std::make_unique<CachedImage>();
    img->layout = layout;
    img->format = format;
    img->aspect = ImageAspect(layout);
    VkImageUsageFlags usage = ImageUsage(layout, false, false);
    const VkImageCreateFlags flags = ImageCreateFlags(layout, false, true);
    const auto samples = NumSamples(layout.num_samples, layout.props.is_depth ? ctx_.Caps().depth_sample_counts
                                                                              : ctx_.Caps().color_sample_counts);
    // the device may not take every usage for the format: drop storage, then the attachment
    VkImageFormatProperties props{};
    VkResult r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value,
                                                            VK_IMAGE_TILING_OPTIMAL, usage, flags, &props);
    if (r != VK_SUCCESS) {
        usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value, VK_IMAGE_TILING_OPTIMAL,
                                                       usage, flags, &props);
    }
    if (r != VK_SUCCESS) {
        usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value, VK_IMAGE_TILING_OPTIMAL,
                                                       usage, flags, &props);
    }
    if (r != VK_SUCCESS) {
        error = fmt::format("image at {:#x}: format {} is not supported by the device", layout.guest_address,
                            static_cast<int>(format));
        return nullptr;
    }
    img->usage = usage;
    img->create_flags = flags;
    const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = flags,
                                .imageType = type.value,
                                .format = format,
                                .extent = {std::max(layout.size.width, 1u), std::max(layout.size.height, 1u),
                                           std::max(layout.size.depth, 1u)},
                                .mipLevels = std::max<u32>(layout.levels, 1),
                                .arrayLayers = std::max<u32>(layout.layers, 1),
                                .samples = samples.Valid() ? samples.value : VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (f.vkCreateImage(ctx_.Device(), &ici, nullptr, &img->image) != VK_SUCCESS) {
        error = fmt::format("vkCreateImage failed for {}x{} format {}", ici.extent.width, ici.extent.height,
                            static_cast<int>(format));
        return nullptr;
    }
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(ctx_.Device(), img->image, &req);
    const s32 mt = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = static_cast<u32>(std::max(mt, 0))};
    if (mt < 0 || f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &img->memory) != VK_SUCCESS) {
        f.vkDestroyImage(ctx_.Device(), img->image, nullptr);
        error = fmt::format("no video memory for a {} MiB image", req.size >> 20);
        return nullptr;
    }
    f.vkBindImageMemory(ctx_.Device(), img->image, img->memory, 0);
    img->bytes = req.size;
    img->id = next_id_++;
    ++stats_.created;
    ++stats_.images;
    stats_.bytes += req.size;
    CachedImage* raw = img.get();
    by_address_.emplace(layout.guest_address, raw);
    max_image_size_ = std::max<u64>(max_image_size_, layout.guest_size);
    ++epoch_;
    images_.push_back(std::move(img));
    return raw;
}

bool ImageCache::DetilePipeline(const TilingParams& p, VkPipeline& out, std::string& error) {
    u64 key = Mix(p.num_bits, p.macro ? 1 : 0);
    for (u32 i = 0; i < p.spec_count; ++i) {
        key = Mix(key, p.spec[i]);
    }
    if (const auto it = detile_pipelines_.find(key); it != detile_pipelines_.end()) {
        out = it->second;
        return true;
    }
    const std::span<const u32> code = HostShaders::Tiling(p.num_bits, p.macro);
    if (code.empty()) {
        error = fmt::format("no tiling kernel for {} bits ({})", p.num_bits, p.macro ? "macro" : "micro");
        return false;
    }
    const auto& f = Fn();
    const VkShaderModuleCreateInfo mci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = code.size_bytes(),
                                       .pCode = code.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (f.vkCreateShaderModule(ctx_.Device(), &mci, nullptr, &module) != VK_SUCCESS) {
        error = "the tiling kernel's module was refused";
        return false;
    }
    std::array<VkSpecializationMapEntry, 12> entries{};
    for (u32 i = 0; i < 12; ++i) {
        entries[i] = {i, i * 4, 4};
    }
    const VkSpecializationInfo spec{.mapEntryCount = p.spec_count,
                                    .pMapEntries = entries.data(),
                                    .dataSize = p.spec_count * 4u,
                                    .pData = p.spec.data()};
    const VkComputePipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = module,
                                                   .pName = "main",
                                                   .pSpecializationInfo = &spec},
                                         .layout = detile_layout_};
    const VkResult r = f.vkCreateComputePipelines(ctx_.Device(), VK_NULL_HANDLE, 1, &ci, nullptr, &out);
    f.vkDestroyShaderModule(ctx_.Device(), module, nullptr);
    if (r != VK_SUCCESS) {
        error = fmt::format("the tiling pipeline could not be created ({})", static_cast<int>(r));
        return false;
    }
    detile_pipelines_.emplace(key, out);
    return true;
}

bool ImageCache::Upload(CachedImage& img, VkCommandBuffer cb, u64 seq, std::string& error) {
    const auto& f = Fn();
    const ImageLayout& l = img.layout;
    ++gen_; // (its texels and flags)
    GuestBuffers::View src{};
    GuestBuffers::Failure failure{};
    BufferCache::Range resident{};
    if (l.guest_size != 0 && buffers_ != nullptr && buffers_->Resident(l.guest_address, l.guest_size, resident)) {
        src.buffer = resident.buffer; // the GPU's copy (it may have written the texels: newer than the host's)
        src.offset = resident.offset;
    } else if (l.guest_size == 0 || !guest_.Get(l.guest_address, l.guest_size, seq, src, failure)) {
        ++stats_.import_failures;
        error = fmt::format("image at {:#x} ({} bytes): its guest memory cannot be read by the GPU", l.guest_address,
                            l.guest_size);
        return false;
    }
    if (tracker_ != nullptr) {
        tracker_->Arm(l.guest_address, l.guest_size); // a guest write from now on uploads it again
    }
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? diag_->Begin(cb, seq,
                                       fmt::format("upload img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} tile {}{}",
                                                   img.id, l.guest_address, l.guest_size, l.pixel_format, l.size.width,
                                                   l.size.height, l.size.depth, l.levels, l.layers,
                                                   static_cast<u32>(l.tile_mode), img.uploaded ? " (again)" : ""))
                        : 0;
    VkBuffer copy_src = src.buffer;
    u64 copy_base = src.offset;
    if (l.IsTiled()) {
        const TilingParams p = BuildTilingParams(l, false);
        if (!p.supported) {
            error = fmt::format("image at {:#x}: no detiler ({})", l.guest_address, p.reason);
            return false;
        }
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!DetilePipeline(p, pipe, error)) {
            return false;
        }
        // a device-local temporary: the linear texels, then the tiling parameters
        const u64 uoff = (static_cast<u64>(l.guest_size) + 255) & ~255ull;
        Temp t;
        t.seq = seq;
        const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = uoff + 256,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (f.vkCreateBuffer(ctx_.Device(), &bci, nullptr, &t.buffer) != VK_SUCCESS) {
            error = "detile buffer: vkCreateBuffer failed";
            return false;
        }
        VkMemoryRequirements req{};
        f.vkGetBufferMemoryRequirements(ctx_.Device(), t.buffer, &req);
        const s32 mt = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = static_cast<u32>(std::max(mt, 0))};
        if (mt < 0 || f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &t.memory) != VK_SUCCESS) {
            f.vkDestroyBuffer(ctx_.Device(), t.buffer, nullptr);
            error = "detile buffer: no video memory";
            return false;
        }
        f.vkBindBufferMemory(ctx_.Device(), t.buffer, t.memory, 0);
        temps_.push_back(t);
        f.vkCmdUpdateBuffer(cb, t.buffer, uoff, sizeof(TilingInfo), &p.info);
        VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                             .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        const VkDescriptorBufferInfo in{src.buffer, src.offset, l.guest_size};
        const VkDescriptorBufferInfo outb{t.buffer, 0, l.guest_size};
        const VkDescriptorBufferInfo uni{t.buffer, uoff, sizeof(TilingInfo)};
        const VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &in},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &outb},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &uni},
        };
        f.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, detile_layout_, 0, 3, w);
        f.vkCmdDispatch(cb, std::max(p.dispatch_x, 1u), 1, 1);
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        f.vkCmdPipelineBarrier2(cb, &dep);
        copy_src = t.buffer;
        copy_base = 0;
        ++stats_.detiles;
    } else {
        ++stats_.linear_uploads;
    }
    Transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> copies;
    const bool is3d = l.type == ImageType::Color3D;
    for (u32 m = 0; m < l.levels && m < kMaxMips; ++m) {
        const MipInfo& mip = l.mips[m];
        VkImageAspectFlags aspect = img.aspect;
        if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT; // the stencil plane is not uploaded (targets are not uploaded at all)
        }
        copies.push_back(VkBufferImageCopy{
            .bufferOffset = copy_base + mip.offset,
            .bufferRowLength = mip.pitch,
            .bufferImageHeight = mip.height,
            .imageSubresource = {aspect, m, 0, is3d ? 1u : std::max<u32>(l.layers, 1)},
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::max(l.size.width >> m, 1u), std::max(l.size.height >> m, 1u),
                            is3d ? std::max(l.size.depth >> m, 1u) : 1u}});
    }
    f.vkCmdCopyBufferToImage(cb, copy_src, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<u32>(copies.size()), copies.data());
    if (cid != 0) {
        diag_->End(cb, cid);
    }
    img.uploaded = true;
    img.dirty = false;
    ++stats_.uploads;
    stats_.upload_bytes += l.guest_size;
    return true;
}

CachedImage* ImageCache::ExpandSource(const ImageLayout& want, std::multimap<u64, CachedImage*>::iterator lo,
                                      std::multimap<u64, CachedImage*>::iterator hi) {
    // the bridge's ExpandImage (texture_cache.cpp ResolveOverlap): a texture at the address of an image the GPU wrote,
    // asking for more of it (levels, layers) or for a block-compressed view of its uncompressed texels (the same bits
    // a texel and a block). No Vulkan view covers that: a new image, guest memory uploaded (the parts the GPU never
    // wrote), the GPU-written texels copied over it (vkCmdCopyImage: size-compatible formats, the extent in the
    // source's texels). An image whose memory was written since (memory_newer: the address reused) is not a source.
    for (auto it = lo; it != hi; ++it) {
        if (CanExpandFrom(*it->second, want)) {
            return it->second;
        }
    }
    return nullptr;
}

bool CanExpandFrom(const CachedImage& have_img, const ImageLayout& want) {
    const ImageLayout& have = have_img.layout;
    if (!have_img.gpu_written || have_img.memory_newer || have_img.maybe_newer || have.num_samples != 1 ||
        want.num_samples != 1 || have.num_bits != want.num_bits || have.props.is_depth != want.props.is_depth ||
        (have.type == ImageType::Color3D) != (want.type == ImageType::Color3D)) {
        return false;
    }
    if (want.props.is_block && !have.props.is_block) {
        // texels of the uncompressed image = blocks of the compressed one
        return have.BlockDim() == want.BlockDim() && have.layers <= want.layers && have.size.depth == want.size.depth;
    }
    if (want.props.is_block != have.props.is_block || have.size != want.size || have.tile_mode != want.tile_mode) {
        return false;
    }
    return have.levels < want.levels || have.layers < want.layers;
}

void ImageCache::CopyImage(CachedImage& src, CachedImage& dst, VkCommandBuffer cb) {
    Transition(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
    Transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const ImageLayout& s = src.layout;
    const ImageLayout& d = dst.layout;
    const bool is3d = s.type == ImageType::Color3D;
    const u32 levels = std::min<u32>(std::max<u32>(s.levels, 1), std::max<u32>(d.levels, 1));
    const u32 layers = is3d ? 1u : std::min<u32>(std::max<u32>(s.layers, 1), std::max<u32>(d.layers, 1));
    const auto strip = [](VkImageAspectFlags a) {
        return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                                                                              : a;
    };
    std::vector<VkImageCopy> regions;
    for (u32 m = 0; m < levels; ++m) {
        // the extent in the source's texels (an uncompressed source of a compressed destination: one texel a block)
        const VkExtent3D ext{std::max(s.size.width >> m, 1u), std::max(s.size.height >> m, 1u),
                             is3d ? std::max(s.size.depth >> m, 1u) : 1u};
        if (d.props.is_block && !s.props.is_block) {
            // the destination's level must hold that many whole blocks
            // and stay inside it: a level that is not whole blocks (tail mips 2x2, 1x1, 6 wide) would take the copy past
            // its edge (vkCmdCopyImage's extent rule; review M3) - those keep memory's texels
            const u32 dw = std::max(d.size.width >> m, 1u), dh = std::max(d.size.height >> m, 1u);
            if (dw % 4 != 0 || dh % 4 != 0 || ext.width * 4 > dw || ext.height * 4 > dh) {
                break;
            }
        }
        regions.push_back(VkImageCopy{.srcSubresource = {strip(src.aspect), m, 0, layers},
                                      .srcOffset = {0, 0, 0},
                                      .dstSubresource = {strip(dst.aspect), m, 0, layers},
                                      .dstOffset = {0, 0, 0},
                                      .extent = ext});
    }
    if (!regions.empty()) {
        Fn().vkCmdCopyImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(regions.size()), regions.data());
    }
}

CachedImage* ImageCache::Find(const ImageLayout& layout, Use use, VkCommandBuffer cb, u64 seq, std::string& error) {
    ApplyInvalidations();
    if (use == Use::Texture && !stencil_owner_.empty() && layout.num_bits == 8 && !layout.props.is_block) {
        // a texture of a depth target's stencil plane (GCN: a surface of its own at DB_STENCIL_*_BASE; here the
        // stencil aspect of the depth image the GPU rendered): the bridge's associated stencil image, copied from its
        // depth image; a view of the aspect here. Guest memory never holds those texels - an upload read garbage, and
        // the sun shadow mask (which reads the scene's stencil) came out shadowed where the bridge's is lit (P4.5).
        if (const auto it = stencil_owner_.find(layout.guest_address); it != stencil_owner_.end()) {
            CachedImage* depth = it->second;
            if (depth->gpu_written && !depth->memory_newer && depth->layout.size.width == layout.size.width &&
                depth->layout.size.height == layout.size.height && (depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                depth->last_seq = seq;
                depth->last_frame = frame_;
                ++stats_.stencil_views;
                return depth;
            }
        }
    }
    CachedImage* match = nullptr;
    const auto [lo, hi] = by_address_.equal_range(layout.guest_address);
    for (auto it = lo; it != hi; ++it) {
        if (SameImage(it->second->layout, layout)) {
            match = it->second;
            break;
        }
    }
    if (match == nullptr) {
        for (auto it = lo; it != hi; ++it) {
            if (ImageServes(it->second->layout, layout)) {
                match = it->second;
                ++stats_.alias_hits;
                break;
            }
        }
    }
    if (match == nullptr) {
        // the bridge's equal-address overlap (TextureCache::ResolveOverlap): the same element size and grid, and a
        // format the image can be viewed with (a depth image through its depth aspect) -> that image. Above all a
        // texture of what the GPU rendered there (G-buffer, depth, post-processing targets): those texels exist only
        // in video memory, guest memory never has them.
        // Only a sampled texture over an image the GPU wrote (a storage view needs the image's storage usage, a
        // sampled view the same sample count; CPU-written textures at a reused address are uploaded as themselves).
        const VkFormat want = static_cast<VkFormat>(layout.pixel_format);
        // P4.5: a storage image too (a compute pass writing a channel of a G-buffer target the draws rendered: one
        // image of that memory, as the bridge's ResolveOverlap - two images split the texels, the shadow mask read a
        // target whose red channel only the compute pass wrote, into an image of its own)
        for (auto it = lo; it != hi && match == nullptr && (use == Use::Texture || use == Use::Storage) && alias_rule_;
             ++it) {
            const ImageLayout& have = it->second->layout;
            // a compressed view of an uncompressed image is not a view in Vulkan (the bridge copies: ExpandImage)
            if (!it->second->gpu_written || have.BlockDim() != layout.BlockDim() || have.num_bits != layout.num_bits ||
                have.num_samples != layout.num_samples || have.props.is_block != layout.props.is_block ||
                (have.type == ImageType::Color3D) != (layout.type == ImageType::Color3D) ||
                have.levels < layout.levels || have.layers < layout.layers) {
                // fewer levels/layers: no view covers the request (the bridge copies the smaller image into a new one;
                // the backend has no such copy yet, the texture is its own image)
                continue;
            }
            if (use == Use::Storage &&
                ((it->second->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 || have.props.is_depth || layout.props.is_depth)) {
                continue; // a storage view needs the image's storage usage (colour images only)
            }
            if (layout.props.is_depth && !have.props.is_depth) {
                continue; // depth sampling of a colour image: a copy the backend does not make yet
            }
            const VkFormat hf = static_cast<VkFormat>(have.pixel_format);
            const bool viewable = IsVulkanFormatCompatible(hf, want) || (have.props.is_depth && IsFormatDepthCompatible(want)) ||
                                  (have.props.is_depth && layout.props.is_depth);
            if (viewable) {
                match = it->second;
                ++stats_.overlap_hits;
                ++match->alias_hits;
                if (diag_ != nullptr) {
                    diag_->Alias(Mix(Mix(layout.guest_address, layout.pixel_format), match->id),
                                 fmt::format("ALIAS texture {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} bits {} block {} "
                                             "samples {} levels {} layers {} tile {} depth {} -> img{} {:#x}+{:#x} fmt {} "
                                             "type {} {}x{}x{} pitch {} bits {} block {} samples {} levels {} layers {} "
                                             "tile {} depth {} stencil {} gpu-written {} usage {:#x} flags {:#x} "
                                             "layout {}",
                                             layout.guest_address, layout.guest_size, layout.pixel_format,
                                             static_cast<u32>(layout.type), layout.size.width, layout.size.height,
                                             layout.size.depth, layout.pitch, layout.num_bits, layout.props.is_block,
                                             layout.num_samples, layout.levels, layout.layers,
                                             static_cast<u32>(layout.tile_mode), layout.props.is_depth, match->id,
                                             have.guest_address, have.guest_size, have.pixel_format,
                                             static_cast<u32>(have.type), have.size.width, have.size.height,
                                             have.size.depth, have.pitch, have.num_bits, have.props.is_block,
                                             have.num_samples, have.levels, have.layers,
                                             static_cast<u32>(have.tile_mode), have.props.is_depth,
                                             have.props.has_stencil, match->gpu_written, match->usage,
                                             match->create_flags, static_cast<int>(match->current)));
                }
            }
        }
        if (match == nullptr && use == Use::Texture) {
            for (auto it = lo; it != hi; ++it) {
                if (!it->second->gpu_written) {
                    continue;
                }
                const ImageLayout& have = it->second->layout;
                if (stats_.target_misses++ < 16) {
                    SSPC_LOG(GPU, Warning,
                             "native backend: texture at {:#x} over a GPU-written image not used: want fmt {} {}x{}x{} "
                             "pitch {} bits {} samples {} levels {} layers {} tile {} size {:#x} depth {} | have fmt {} "
                             "{}x{}x{} pitch {} bits {} samples {} levels {} layers {} tile {} size {:#x} depth {}",
                             layout.guest_address, layout.pixel_format, layout.size.width, layout.size.height,
                             layout.size.depth, layout.pitch, layout.num_bits, layout.num_samples, layout.levels,
                             layout.layers, static_cast<u32>(layout.tile_mode), layout.guest_size, layout.props.is_depth,
                             have.pixel_format, have.size.width, have.size.height, have.size.depth, have.pitch,
                             have.num_bits, have.num_samples, have.levels, have.layers, static_cast<u32>(have.tile_mode),
                             have.guest_size, have.props.is_depth);
                }
                break;
            }
        }
    }
    // P4.5: a texture that needs more of a GPU-written image than a view gives (ExpandImage)
    CachedImage* expand_src =
        match == nullptr && use == Use::Texture && alias_rule_ ? ExpandSource(layout, lo, hi) : nullptr;
    if (match == nullptr && use == Use::Texture && expand_src == nullptr) {
        // diagnostics: a new texture over memory the GPU rendered into (any overlap): those texels exist only in video
        // memory, the upload reads guest memory the GPU never wrote
        const u64 a = layout.guest_address, e = a + layout.guest_size;
        auto it = by_address_.lower_bound(a > (256ull << 20) ? a - (256ull << 20) : 0);
        for (; it != by_address_.end() && it->first < e; ++it) {
            const CachedImage* h = it->second;
            const u64 ha = h->layout.guest_address, he = ha + h->layout.guest_size;
            if (!h->gpu_written || he <= a) {
                continue;
            }
            ++stats_.overlap_misses;
            if (overlap_misses_logged_++ < 48) {
                const char* rel = ha == a                  ? "same start"
                                  : (ha < a && he >= e)   ? "texture inside target"
                                  : (a < ha && e >= he)   ? "target inside texture"
                                                          : "partial";
                SSPC_LOG(GPU, Warning,
                         "native backend: texture {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} lv {} ly {} tile {} bits {} "
                         "block {} depth {} OVERLAPS gpu-written img{} {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} lv {} "
                         "ly {} tile {} bits {} block {} depth {} ({}, offset {:#x})",
                         a, layout.guest_size, layout.pixel_format, static_cast<u32>(layout.type), layout.size.width,
                         layout.size.height, layout.size.depth, layout.pitch, layout.levels, layout.layers,
                         static_cast<u32>(layout.tile_mode), layout.num_bits, layout.props.is_block,
                         layout.props.is_depth, h->id, ha, h->layout.guest_size, h->layout.pixel_format,
                         static_cast<u32>(h->layout.type), h->layout.size.width, h->layout.size.height,
                         h->layout.size.depth, h->layout.pitch, h->layout.levels, h->layout.layers,
                         static_cast<u32>(h->layout.tile_mode), h->layout.num_bits, h->layout.props.is_block,
                         h->layout.props.is_depth, rel, a >= ha ? a - ha : ha - a);
            }
            break;
        }
    }
    if (match == nullptr) {
        // diagnostics (P4.5): a second image of an address another image already has - each split of a memory's texels
        // between two Vulkan images is logged once with both layouts (bounded)
        for (auto it = lo; it != hi && dup_logged_ < 64; ++it) {
            const ImageLayout& h = it->second->layout;
            ++dup_logged_;
            SSPC_LOG(GPU, Info,
                     "native backend: another image at {:#x} for {}: want fmt {} {}x{}x{} type {} pitch {} lv {} ly {} s {} "
                     "tile {} size {:#x} bits {} | have img{}{} fmt {} {}x{}x{} type {} pitch {} lv {} ly {} s {} tile {} "
                     "size {:#x} bits {} usage {:#x}",
                     layout.guest_address,
                     use == Use::Texture   ? "texture"
                     : use == Use::Storage ? "storage"
                     : use == Use::ColorTarget ? "colour target"
                     : use == Use::DepthTarget ? "depth target"
                                               : "transfer",
                     layout.pixel_format, layout.size.width, layout.size.height, layout.size.depth,
                     static_cast<u32>(layout.type), layout.pitch, layout.levels, layout.layers, layout.num_samples,
                     static_cast<u32>(layout.tile_mode), layout.guest_size, layout.num_bits, it->second->id,
                     it->second->gpu_written ? " gw" : "", h.pixel_format, h.size.width, h.size.height, h.size.depth,
                     static_cast<u32>(h.type), h.pitch, h.levels, h.layers, h.num_samples, static_cast<u32>(h.tile_mode),
                     h.guest_size, h.num_bits, it->second->usage);
        }
        match = Create(layout, error);
        if (match != nullptr && diag_ != nullptr && diag_->Naming()) {
            diag_->Name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(match->image),
                        fmt::format("img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} s {} tile {}", match->id,
                                    layout.guest_address, layout.guest_size, layout.pixel_format, layout.size.width,
                                    layout.size.height, layout.size.depth, layout.levels, layout.layers,
                                    layout.num_samples, static_cast<u32>(layout.tile_mode)));
        }
        if (match == nullptr) {
            ++stats_.unsupported;
            if (stats_.first_unsupported.empty()) {
                stats_.first_unsupported = error;
                SSPC_LOG(GPU, Error, "native backend: {}", error);
            }
            return nullptr;
        }
    } else {
        ++stats_.hits;
    }
    match->last_seq = seq;
    match->last_frame = frame_;
    const bool target = use == Use::ColorTarget || use == Use::DepthTarget;
    if (expand_src != nullptr) {
        // guest memory first (what the GPU never wrote there), then the GPU-written texels over it (ExpandImage)
        if (before_upload_ != nullptr) {
            before_upload_(before_upload_ctx_);
        }
        if (!Upload(*match, cb, seq, error)) {
            return nullptr;
        }
        CopyImage(*expand_src, *match, cb);
        const bool block = layout.props.is_block && !expand_src->layout.props.is_block;
        ++(block ? stats_.block_copies : stats_.expanded);
        if (stats_.expanded + stats_.block_copies <= 16) {
            SSPC_LOG(GPU, Info,
                     "native backend: img{} {:#x} fmt {} {}x{} lv {} ly {} made from GPU-written img{} fmt {} {}x{} lv "
                     "{} ly {} ({})",
                     match->id, layout.guest_address, layout.pixel_format, layout.size.width, layout.size.height,
                     layout.levels, layout.layers, expand_src->id, expand_src->layout.pixel_format,
                     expand_src->layout.size.width, expand_src->layout.size.height, expand_src->layout.levels,
                     expand_src->layout.layers, block ? "block-compressed view" : "expanded");
        }
        match->gpu_written = true;
        expand_src->last_seq = std::max(expand_src->last_seq, seq); // the copy reads it: destroyed after this batch
        Retire(expand_src);
        TrackGpuWritten(*match);
    } else if (!target && !match->gpu_written && (!match->uploaded || match->dirty)) {
        stats_.reuploads += match->uploaded ? 1 : 0;
        if (before_upload_ != nullptr) {
            before_upload_(before_upload_ctx_);
        }
        if (!Upload(*match, cb, seq, error)) {
            return nullptr;
        }
    } else if (match->gpu_written && match->memory_newer) {
        // memory was written after the GPU wrote this image (the CPU, a DMA, a shader's buffer write at its base): its
        // texels are memory's - uploaded before this use (a target too: a draw covering part of it keeps the rest).
        // A multisampled image cannot take a buffer copy: it keeps its texels (as the bridge's RefreshImage).
        match->memory_newer = false;
        ++gen_;
        if (layout.num_samples <= 1 && match->layout.num_samples <= 1) {
            if (before_upload_ != nullptr) {
                before_upload_(before_upload_ctx_);
            }
            if (!Upload(*match, cb, seq, error)) {
                return nullptr;
            }
            stats_.target_uploads += target ? 1 : 0;
            if (!target && use != Use::Storage) {
                match->gpu_written = false; // a texture of memory's texels now (uploaded, tracked)
            }
        }
    }
    if ((target || use == Use::Storage) && match->maybe_newer) {
        // what the GPU writes now is its latest; the blocks a fault released are armed again (the CPU's later writes
        // there are seen - a write-back must not go over them; review M1b)
        match->maybe_newer = false;
        if (match->gpu_written) {
            TrackGpuWritten(*match);
        }
    }
    if ((target || use == Use::Storage) && !match->gpu_written) {
        match->gpu_written = true;
        match->superseded = false;
        ++gen_;
        TrackGpuWritten(*match);
        // other GPU-written images over this memory hold what was there before: retired once unused for a while
        const u64 a = layout.guest_address, e = a + layout.guest_size;
        auto it = by_address_.lower_bound(a > max_image_size_ ? a - max_image_size_ : 0);
        for (; it != by_address_.end() && it->first < e; ++it) {
            CachedImage* o = it->second;
            if (o != match && o->gpu_written && !o->superseded && o->layout.guest_address < e &&
                a < o->layout.guest_address + o->layout.guest_size) {
                o->superseded = true;
                ++stats_.superseded;
            }
        }
    } else if (target || use == Use::Storage) {
        match->superseded = false;
    }
    if (target) {
        for (const u64 meta : {layout.meta.cmask_addr, layout.meta.fmask_addr, layout.meta.htile_addr}) {
            if (meta != 0) {
                meta_owner_[meta] = match;
            }
        }
        if (use == Use::DepthTarget && layout.props.has_stencil && layout.stencil_addr != 0) {
            stencil_owner_[layout.stencil_addr] = match;
        }
    }
    return match;
}

CachedImage* ImageCache::FindTargetAt(u64 address) {
    CachedImage* best = nullptr;
    const auto [lo, hi] = by_address_.equal_range(address);
    for (auto it = lo; it != hi; ++it) {
        if (it->second->gpu_written && (best == nullptr || it->second->last_seq > best->last_seq)) {
            best = it->second;
        }
    }
    return best;
}

VkImageView ImageCache::View(CachedImage& img, const ImageViewDesc& v, bool storage) {
    const ViewFormat vf = ViewFormatOf(v.format, img.format, img.aspect);
    VkImageViewType type = ConvertImageViewType(v.type).value;
    const bool image3d = img.layout.type == ImageType::Color3D;
    if (image3d) {
        type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (type == VK_IMAGE_VIEW_TYPE_3D) {
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    const u32 levels_avail = std::max<u32>(img.layout.levels, 1), layers_avail = image3d ? 1u : std::max<u32>(img.layout.layers, 1);
    const u32 base_level = std::min<u32>(v.base_level, levels_avail - 1);
    const u32 levels = std::clamp<u32>(v.levels, 1, levels_avail - base_level);
    const u32 base_layer = image3d ? 0u : std::min<u32>(v.base_layer, layers_avail - 1);
    u32 layers = image3d ? 1u : std::clamp<u32>(v.layers, 1, layers_avail - base_layer);
    if (type == VK_IMAGE_VIEW_TYPE_2D && layers > 1) {
        layers = 1;
    }
    constexpr VkComponentMapping kIdentity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const auto mapped = ViewComponents(v.mapping);
    const VkComponentMapping comps = (storage || !mapped.Valid()) ? kIdentity : mapped.value;
    if (diag_ != nullptr && img.alias_hits != 0) {
        diag_->Alias(Mix(Mix(Mix(img.id, static_cast<u64>(v.format)), static_cast<u64>(v.type)),
                         (u64{v.base_level} << 48) | (u64{v.levels} << 32) | (u64{v.base_layer} << 16) | v.layers),
                     fmt::format("VIEW of aliased img{} (image fmt {} type {} levels {} layers {} usage {:#x} flags "
                                 "{:#x}): asked fmt {} type {} levels {}+{} layers {}+{} storage {} -> view fmt {} "
                                 "aspect {:#x} type {} levels {}+{} layers {}+{}",
                                 img.id, static_cast<int>(img.format), static_cast<u32>(img.layout.type),
                                 img.layout.levels, img.layout.layers, img.usage, img.create_flags,
                                 static_cast<int>(v.format), static_cast<u32>(v.type), v.base_level, v.levels,
                                 v.base_layer, v.layers, storage, static_cast<int>(vf.format), vf.aspect,
                                 static_cast<int>(type), base_level, levels, base_layer, layers));
    }
    u64 key = Mix(static_cast<u64>(vf.format), type);
    key = Mix(key, (u64{base_level} << 48) | (u64{levels} << 32) | (u64{base_layer} << 16) | layers);
    key = Mix(key, (u64{static_cast<u32>(comps.r)} << 24) | (u64{static_cast<u32>(comps.g)} << 16) |
                       (u64{static_cast<u32>(comps.b)} << 8) | static_cast<u32>(comps.a));
    key = Mix(key, (storage ? 1u : 0u) | (u64{vf.aspect} << 8));
    if (const auto it = img.views.find(key); it != img.views.end()) {
        return it->second;
    }
    const VkImageViewUsageCreateInfo usage{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                           .usage = storage ? img.usage : (img.usage & ~VK_IMAGE_USAGE_STORAGE_BIT)};
    const VkImageViewCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .pNext = &usage,
                                   .image = img.image,
                                   .viewType = type,
                                   .format = vf.format,
                                   .components = comps,
                                   .subresourceRange = {vf.aspect, base_level, levels, base_layer, layers}};
    VkImageView view = VK_NULL_HANDLE;
    if (Fn().vkCreateImageView(ctx_.Device(), &ci, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    img.views.emplace(key, view);
    ++stats_.views;
    return view;
}

bool ImageCache::Transition(VkCommandBuffer cb, CachedImage& img, VkImageLayout layout, VkPipelineStageFlags2 stage,
                            VkAccessFlags2 access) {
    const bool prev_write = (img.access & kWriteAccess) != 0, next_write = (access & kWriteAccess) != 0;
    if (img.current == layout && !prev_write && !next_write) {
        img.stage |= stage;
        img.access |= access;
        return false;
    }
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = img.stage == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : img.stage,
                                  .srcAccessMask = img.access & kWriteAccess,
                                  .dstStageMask = stage,
                                  .dstAccessMask = access,
                                  .oldLayout = img.current,
                                  .newLayout = layout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = img.image,
                                  .subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                       VK_REMAINING_ARRAY_LAYERS}};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
    Fn().vkCmdPipelineBarrier2(cb, &dep);
    if (rec_value_ != 0) { // (R11 SSPC_R11_AQ_IMAGES: the lane batches that sampled it run before this barrier)
        img.barrier_value = rec_value_;
        img.barrier_lane = rec_lane_;
        lane_wait_ = !rec_lane_ ? std::max(lane_wait_, img.lane_use) : lane_wait_;
    }
    img.current = layout;
    img.stage = stage;
    img.access = access;
    ++stats_.barriers;
    ++gen_;
    return true;
}

bool ImageCache::ReadSince(const CachedImage& img, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    return (img.access & kWriteAccess) == 0 && (access & kWriteAccess) == 0 && (img.stage & stage) == stage &&
           (img.access & access) == access;
}

VkSampler ImageCache::Sampler(const Gpu::Shader::SamplerDesc& s, bool is_depth, u64 border_color_base, float extra_lod_bias) {
    SamplerCaps caps;
    caps.anisotropy = true;
    caps.max_anisotropy = 16.0f;
    caps.custom_border_color = ctx_.Caps().custom_border_color;
    caps.max_lod_bias = 15.0f;
    const SamplerParams p = BuildSamplerParams(s, is_depth, caps);
    if (!p.supported) {
        return VK_NULL_HANDLE;
    }
    std::array<float, 4> border{};
    if (p.border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) {
        if (auto* mem = GetGuestMemory(); mem != nullptr && border_color_base != 0) {
            mem->CopySparse(border_color_base + u64{p.border_color_index} * 16, reinterpret_cast<u8*>(border.data()), 16);
        }
    }
    u64 key = Mix(Mix(s.raw[0] | (u64{s.raw[1]} << 32), s.raw[2] | (u64{s.raw[3]} << 32)), is_depth ? 1 : 0);
    u32 bits[4];
    std::memcpy(bits, border.data(), 16);
    key = Mix(Mix(key, bits[0] | (u64{bits[1]} << 32)), bits[2] | (u64{bits[3]} << 32));
    if (extra_lod_bias != 0.0f) { // (upscaling: the scene's textures at the output's detail)
        u32 b = 0;
        std::memcpy(&b, &extra_lod_bias, 4);
        key = Mix(key, 0xB1A5000000000000ull | b);
    }
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
        return it->second;
    }
    VkSamplerCustomBorderColorCreateInfoEXT custom{.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                   .format = VK_FORMAT_R32G32B32A32_SFLOAT};
    std::memcpy(custom.customBorderColor.float32, border.data(), 16);
    const VkSamplerCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                 .pNext = p.border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ? &custom : nullptr,
                                 .magFilter = p.mag_filter,
                                 .minFilter = p.min_filter,
                                 .mipmapMode = p.mipmap_mode,
                                 .addressModeU = p.address_u,
                                 .addressModeV = p.address_v,
                                 .addressModeW = p.address_w,
                                 .mipLodBias = std::clamp(p.mip_lod_bias + extra_lod_bias, -15.0f, 15.0f),
                                 .anisotropyEnable = p.anisotropy_enable ? VK_TRUE : VK_FALSE,
                                 .maxAnisotropy = p.max_anisotropy,
                                 .compareEnable = p.compare_enable ? VK_TRUE : VK_FALSE,
                                 .compareOp = p.compare_op,
                                 .minLod = p.min_lod,
                                 .maxLod = p.max_lod,
                                 .borderColor = p.border_color,
                                 .unnormalizedCoordinates = VK_FALSE};
    VkSampler sampler = VK_NULL_HANDLE;
    if (Fn().vkCreateSampler(ctx_.Device(), &ci, nullptr, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    samplers_.emplace(key, sampler);
    ++stats_.samplers;
    return sampler;
}

void ImageCache::Collect(u64 completed) {
    while (!temps_.empty() && temps_.front().seq <= completed) {
        Fn().vkDestroyBuffer(ctx_.Device(), temps_.front().buffer, nullptr);
        Fn().vkFreeMemory(ctx_.Device(), temps_.front().memory, nullptr);
        temps_.pop_front();
    }
    // taken-out images whose last use completed: destroyed (P4.5)
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    std::erase_if(retired_, [&](const std::unique_ptr<CachedImage>& img) {
        if (img->last_seq > completed) {
            return false;
        }
        for (auto& [k, v] : img->views) {
            f.vkDestroyImageView(dev, v, nullptr);
        }
        f.vkDestroyImage(dev, img->image, nullptr);
        f.vkFreeMemory(dev, img->memory, nullptr);
        return true;
    });
    // superseded GPU-written images unused for kRetireFrames frames: their memory belongs to another image now
    constexpr u64 kRetireFrames = 120;
    if (stats_.superseded != 0 && (frame_ & 15) == 0 && frame_ != last_retire_scan_) {
        last_retire_scan_ = frame_;
        std::vector<CachedImage*> old;
        for (const auto& img : images_) {
            if (img->superseded && img->last_seq <= completed && img->last_frame + kRetireFrames < frame_) {
                old.push_back(img.get());
            }
        }
        for (CachedImage* img : old) {
            Retire(img);
        }
    }
    Evict(completed);
}

bool ImageServes(const ImageLayout& have, const ImageLayout& want) {
    return have.size == want.size && have.num_samples == want.num_samples && have.tile_mode == want.tile_mode &&
           have.levels >= want.levels && have.layers >= want.layers &&
           IsVulkanFormatCompatible(static_cast<VkFormat>(have.pixel_format), static_cast<VkFormat>(want.pixel_format));
}

InvalidationEffect ClassifyInvalidation(InvalKind kind, u64 b, u64 size, bool gpu_written, bool uploaded, u64 addr,
                                        u64 end) {
    InvalidationEffect fx;
    const u64 e = b + size;
    if (!(b < end && addr < e)) {
        return fx; // not over it
    }
    if (kind == InvalKind::Unmap) {
        fx.retire = true;
        return fx;
    }
    if (!gpu_written) {
        fx.dirty = uploaded;
        return fx;
    }
    // a GPU-written image: memory holds its texels again only if this write was surely into it (P4.5)
    constexpr u64 kBlock = 64 * 1024; // the tracker's report granularity
    switch (kind) {
    case InvalKind::Exact:
        fx.memory_newer = true;
        break;
    case InvalKind::BufferWrite:
        fx.memory_newer = b == addr; // the bridge's InvalidateMemoryFromGPU: a pool overlapping it is not a write of it
        break;
    case InvalKind::Coarse: {
        const u64 ib = (b + kBlock - 1) & ~(kBlock - 1), ie = e & ~(kBlock - 1); // blocks wholly inside
        fx.memory_newer = ib < ie && addr < ie && ib < end;
        fx.maybe_newer = !fx.memory_newer; // a block it shares: the write may be its neighbour's
        break;
    }
    default:
        break;
    }
    return fx;
}

GpuEviction ClassifyGpuEviction(const GpuEvictionCandidate& c) {
    if (c.in_flight) {
        return GpuEviction::Keep;
    }
    if (c.memory_newer || c.superseded || c.newer_overlap) {
        // memory (or another image) holds the newer texels: nothing of this one is lost (the bridge frees a dirty
        // image without a download)
        return c.age_frames >= kGpuEvictStaleFrames ? GpuEviction::Drop : GpuEviction::Keep;
    }
    // its texels exist only in video memory: written back where a copy can, else kept - never freed with its texels
    // lost; nor written over memory the CPU may have written since (maybe_newer)
    if (c.age_frames < kGpuEvictCleanFrames || c.maybe_newer || c.no_write_path || c.untracked || c.older_overlap) {
        return GpuEviction::Keep;
    }
    return GpuEviction::WriteBack;
}

std::vector<CachedImage*> ImageCache::EvictGpuWritten(u64 completed, u32 max) {
    std::vector<CachedImage*> out;
    if (stats_.bytes <= budget_ && !force_gpu_evict_) {
        return out;
    }
    std::vector<std::pair<CachedImage*, GpuEviction>> cand;
    u64 no_path = 0;
    for (const auto& img : images_) {
        if (!img->gpu_written) {
            continue;
        }
        const ImageLayout& l = img->layout;
        const u64 age = (frame_ - std::min(frame_, img->last_frame)) * force_age_scale_;
        bool newer_overlap = false, older_overlap = false;
        if (age >= kGpuEvictStaleFrames) {
            // other live GPU-written images over its memory (review M1a: a write-back must not put these texels over a
            // newer image's, nor mark it memory-newer)
            const u64 a0 = l.guest_address, e0 = a0 + l.guest_size;
            auto it = by_address_.lower_bound(a0 > max_image_size_ ? a0 - max_image_size_ : 0);
            for (; it != by_address_.end() && it->first < e0; ++it) {
                const CachedImage* o = it->second;
                if (o != img.get() && o->gpu_written && o->layout.guest_address < e0 &&
                    a0 < o->layout.guest_address + o->layout.guest_size) {
                    (o->last_seq > img->last_seq ? newer_overlap : older_overlap) = true;
                }
            }
        }
        const GpuEviction a = ClassifyGpuEviction({.in_flight = img->last_seq > completed,
                                                   .memory_newer = img->memory_newer,
                                                   .superseded = img->superseded,
                                                   .maybe_newer = img->maybe_newer,
                                                   .no_write_path = NoWritePath(l, img->aspect, img->format),
                                                   .untracked = !img->write_tracked,
                                                   .newer_overlap = newer_overlap,
                                                   .older_overlap = older_overlap,
                                                   .age_frames = age});
        if (a != GpuEviction::Keep) {
            cand.emplace_back(img.get(), a);
        } else if (img->last_seq <= completed && age >= kGpuEvictCleanFrames) {
            ++no_path; // old enough, but no safe way to free it
        }
    }
    stats_.gpu_kept_no_path = no_path;
    std::ranges::sort(cand, [](const auto& a, const auto& b) { return a.first->last_seq < b.first->last_seq; });
    const u64 target = force_gpu_evict_ ? 0 : budget_ / 10 * 9;
    u64 bytes = stats_.bytes;
    for (const auto& [img, a] : cand) {
        if (bytes <= target || out.size() >= max) {
            break;
        }
        bytes -= img->bytes;
        if (a == GpuEviction::Drop) {
            ++stats_.gpu_dropped;
            Retire(img);
        } else {
            out.push_back(img);
        }
    }
    return out;
}

bool ImageCache::NoWritePath(const ImageLayout& l, VkImageAspectFlags aspect, VkFormat format) {
    if (l.num_samples > 1 || l.props.is_volume || l.props.is_block || l.guest_size == 0) {
        return true;
    }
    if (l.props.is_depth &&
        ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 || (format != VK_FORMAT_D32_SFLOAT && format != VK_FORMAT_D16_UNORM))) {
        return true; // the stencil plane is a surface of its own; other depth formats' bytes differ from the guest's
    }
    return l.IsTiled() && !BuildTilingParams(l, true).supported;
}

VkBuffer ImageCache::TempBuffer(u64 bytes, u64 seq) {
    const auto& f = Fn();
    Temp t;
    t.seq = seq;
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = bytes,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (f.vkCreateBuffer(ctx_.Device(), &bci, nullptr, &t.buffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements req{};
    f.vkGetBufferMemoryRequirements(ctx_.Device(), t.buffer, &req);
    const s32 mt = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = static_cast<u32>(std::max(mt, 0))};
    if (mt < 0 || f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &t.memory) != VK_SUCCESS) {
        f.vkDestroyBuffer(ctx_.Device(), t.buffer, nullptr);
        return VK_NULL_HANDLE;
    }
    f.vkBindBufferMemory(ctx_.Device(), t.buffer, t.memory, 0);
    temps_.push_back(t);
    return t.buffer;
}

bool ImageCache::WriteBackAndRetire(CachedImage* img, VkCommandBuffer cb, u64 seq, VkBuffer dst, u64 dst_offset,
                                    std::string& error) {
    // Upload in reverse: the image's levels/layers copied out in the guest's linear arrangement (each level at its
    // offset, its pitch and height), tiled ones then tiled into the guest's memory by the tiler (IS_TILER)
    const auto& f = Fn();
    const ImageLayout& l = img->layout;
    TilingParams p;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkBuffer linear = dst;
    u64 linear_base = dst_offset;
    u64 uoff = 0;
    if (l.IsTiled()) {
        p = BuildTilingParams(l, true);
        if (!p.supported || !DetilePipeline(p, pipe, error)) {
            if (error.empty()) {
                error = fmt::format("no tiler ({})", p.reason);
            }
            return false;
        }
        uoff = (static_cast<u64>(l.guest_size) + 255) & ~255ull;
        linear = TempBuffer(uoff + 512, seq);
        if (linear == VK_NULL_HANDLE) {
            error = "write-back temporary: no video memory";
            return false;
        }
        linear_base = 0;
    }
    VkPipeline detile = VK_NULL_HANDLE;
    TilingParams dp;
    if (l.IsTiled()) {
        dp = BuildTilingParams(l, false);
        if (!dp.supported || !DetilePipeline(dp, detile, error)) {
            if (error.empty()) {
                error = fmt::format("no detiler ({})", dp.reason);
            }
            return false;
        }
    }
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? diag_->Begin(cb, seq,
                                       fmt::format("write-back img{}@{:#x}+{:#x} fmt {} {}x{} lv {} ly {} tile {}", img->id,
                                                   l.guest_address, l.guest_size, l.pixel_format, l.size.width,
                                                   l.size.height, l.levels, l.layers, static_cast<u32>(l.tile_mode)))
                        : 0;
    const auto push = [&](VkBuffer tiled, u64 tiled_offset, VkBuffer lin, u64 info_offset) {
        const VkDescriptorBufferInfo t{tiled, tiled_offset, l.guest_size};
        const VkDescriptorBufferInfo li{lin, 0, l.guest_size};
        const VkDescriptorBufferInfo u{lin, info_offset, sizeof(TilingInfo)};
        const VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &t},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &li},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &u},
        };
        f.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, detile_layout_, 0, 3, w);
    };
    const auto barrier = [&](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                             VkAccessFlags2 dst) {
        const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = src_stage,
                                  .srcAccessMask = src,
                                  .dstStageMask = dst_stage,
                                  .dstAccessMask = dst};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
    };
    if (l.IsTiled()) {
        // the guest's bytes detiled into the temporary first: what the image does not cover (row padding, gaps between
        // levels) goes back as it was, not as the temporary's undefined bytes (review L6)
        const u64 doff = uoff + 256;
        f.vkCmdUpdateBuffer(cb, linear, doff, sizeof(TilingInfo), &dp.info);
        barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, detile);
        push(dst, dst_offset, linear, doff);
        f.vkCmdDispatch(cb, std::max(dp.dispatch_x, 1u), 1, 1);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    Transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
    std::vector<VkBufferImageCopy> copies;
    const VkImageAspectFlags aspect = l.props.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : img->aspect;
    for (u32 m = 0; m < l.levels && m < kMaxMips; ++m) {
        const MipInfo& mip = l.mips[m];
        copies.push_back(VkBufferImageCopy{
            .bufferOffset = linear_base + mip.offset,
            .bufferRowLength = mip.pitch,
            .bufferImageHeight = mip.height,
            .imageSubresource = {aspect, m, 0, std::max<u32>(l.layers, 1)},
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::max(l.size.width >> m, 1u), std::max(l.size.height >> m, 1u), 1u}});
    }
    f.vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear,
                             static_cast<u32>(copies.size()), copies.data());
    if (l.IsTiled()) {
        f.vkCmdUpdateBuffer(cb, linear, uoff, sizeof(TilingInfo), &p.info);
        barrier(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT);
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        push(dst, dst_offset, linear, uoff);
        f.vkCmdDispatch(cb, std::max(p.dispatch_x, 1u), 1, 1);
    }
    if (cid != 0) {
        diag_->End(cb, cid);
    }
    img->last_seq = std::max(img->last_seq, seq); // destroyed after the copy completed
    ++stats_.gpu_written_back;
    stats_.gpu_written_back_bytes += l.guest_size;
    Retire(img);
    return true;
}

} // namespace SecondSon::Gpu::Vk
