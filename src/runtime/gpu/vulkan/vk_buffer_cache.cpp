// SecondSonPC — the guest's buffers in video memory (P4.4). See vk_buffer_cache.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_buffer_cache.h"

#include <algorithm>
#include <cstring>
#include <bit>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/page_tracker.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_resources.h"
#include "runtime/gpu/vulkan/wb_diff_spv.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

constexpr u64 kAddressSpace = 1ull << 40;
constexpr u64 kMaxCopy = 8ull << 20; ///< one copy's source must fit an import window (16 MiB aligned, 32 MiB long)

/// The pages of [first_page, last_page] inside block `block` (16 pages per block) as a mask.
u16 PageMask(u64 block, u64 first_page, u64 last_page) {
    const u64 b0 = block * 16, b1 = b0 + 15;
    const u64 p0 = std::max(first_page, b0) - b0, p1 = std::min(last_page, b1) - b0;
    return static_cast<u16>(((1u << (p1 - p0 + 1)) - 1u) << p0);
}

} // namespace

BufferCache::BufferCache(DeviceContext& ctx, GuestBuffers& guest, PageTracker* tracker, u64 budget_bytes,
                         bool host_wait_binds)
    : ctx_(ctx), guest_(guest), tracker_(tracker), budget_(budget_bytes), host_wait_binds_(host_wait_binds) {
    const auto& f = Fn();
    char off[8] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_BUFFER_CACHE", off, sizeof(off)) == 3 && std::string_view(off) == "off") {
        stats_.first_error = "SSPC_DIAG_BUFFER_CACHE=off (a diagnostic control run)";
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BUFFER_CACHE=off - the buffer cache is disabled for this run");
        return;
    }
    if (char hr[4] = {}; GetEnvironmentVariableA("SSPC_REA_HEAP_READS", hr, sizeof(hr)) == 1 && hr[0] == '1') {
        heap_reads_ = true;
        SSPC_LOG(GPU, Warning, "REA-2: SSPC_REA_HEAP_READS=1 - the game's vertex-data heaps (read-only bindings >= 256 MiB) "
                               "do not write back GPU-written data in their range");
    }
    if (char po[4] = {}; GetEnvironmentVariableA("SSPC_REA_POISON", po, sizeof(po)) == 1 && po[0] == '1') {
        poison_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC (correctness test): SSPC_REA_POISON=1 - the host copy of every kept-back block "
                               "is filled with 0xFFFFFFFF until a reader writes it back");
    }
    if (char lb[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_LARGE_BINDS", lb, sizeof(lb)) == 1 && lb[0] == '1') {
        diag_large_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_LARGE_BINDS=1 - bindings left on the host import are counted");
    }
    char hw[8] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_BIND_HOST_WAIT", hw, sizeof(hw)) == 1 && hw[0] == '1') {
        host_wait_binds_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BIND_HOST_WAIT=1 - sparse binds complete on the host, "
                               "batches do not wait for them on the queue");
    }
    // P4.9-B block upload: on by default since 2026-10-14 (docs\SMOKE_QUALITY.md: the village's compute queue
    // 9.2 -> 3.0 ms GPU, Validation + Sync Validation 0); SSPC_P49_BLOCK_UPLOAD=0 - one upload per binding's pages
    if (char bu[4] = {}; GetEnvironmentVariableA("SSPC_P49_BLOCK_UPLOAD", bu, sizeof(bu)) == 1 && bu[0] == '0') {
        block_upload_ = false;
        SSPC_LOG(GPU, Warning, "SSPC_P49_BLOCK_UPLOAD=0 - block upload off: only a binding's own missing pages are "
                               "uploaded (the P4.8 behaviour)");
    } else {
        block_upload_ = true;
        SSPC_LOG(GPU, Info, "native backend: block upload on - a block's first missing page brings its other used, "
                            "invalid pages along (one upload a block; SSPC_P49_BLOCK_UPLOAD=0: off)");
    }
    if (!ctx_.Caps().sparse_buffer) {
        stats_.first_error = "no sparse buffers (sparseBinding/sparseResidencyBuffer, a sparse binding queue)";
        SSPC_LOG(GPU, Warning, "native backend: buffer cache off: {}", stats_.first_error);
        return;
    }
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                 .size = kAddressSpace,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    // the whole 40-bit space when the driver gives it, else the low part (higher addresses: the host import)
    VkResult cr = VK_ERROR_UNKNOWN;
    for (u64 size = kAddressSpace; size >= (64ull << 30); size /= 2) {
        bci.size = size;
        VkBufferCreateInfo shared = bci;
        ctx_.ShareWithLane(shared); // (FrameBreak AQ)
        cr = f.vkCreateBuffer(ctx_.Device(), &shared, nullptr, &buffer_);
        if (cr == VK_SUCCESS) {
            space_ = size;
            leaves_.resize(size >> (kBlockBits + kLeafBits));
            break;
        }
        SSPC_LOG(GPU, Info, "native backend: buffer cache: a sparse buffer of {} GiB was refused ({})", size >> 30,
                 static_cast<int>(cr));
    }
    if (cr != VK_SUCCESS) {
        buffer_ = VK_NULL_HANDLE;
        stats_.first_error = fmt::format("vkCreateBuffer (sparse) failed ({})", static_cast<int>(cr));
        SSPC_LOG(GPU, Warning, "native backend: buffer cache off: {}", stats_.first_error);
        return;
    }
    VkMemoryRequirements req{};
    f.vkGetBufferMemoryRequirements(ctx_.Device(), buffer_, &req);
    block_alignment_ = req.alignment;
    const s32 type = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type < 0 || (1ull << kBlockBits) % block_alignment_ != 0) {
        stats_.first_error = fmt::format("sparse block alignment {} / memory type {}", block_alignment_, type);
        f.vkDestroyBuffer(ctx_.Device(), buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        SSPC_LOG(GPU, Warning, "native backend: buffer cache off: {}", stats_.first_error);
        return;
    }
    memory_type_ = static_cast<u32>(type);
    const VkSemaphoreTypeCreateInfo ti{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                       .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                       .initialValue = 0};
    const VkSemaphoreCreateInfo si{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    f.vkCreateSemaphore(ctx_.Device(), &si, nullptr, &bind_timeline_);
    InitDiff();
    SSPC_LOG(GPU, Info,
             "native backend: buffer cache on: the guest's GPU address space mirrored in video memory (sparse {} GiB, "
             "{} KiB blocks, budget {} MiB)",
             space_ >> 30, (1ull << kBlockBits) >> 10, budget_ >> 20);
}

BufferCache::~BufferCache() {
    const auto& f = Fn();
    for (WbShadow& sh : wb_shadows_) {
        f.vkDestroyBuffer(ctx_.Device(), sh.buffer, nullptr);
        f.vkFreeMemory(ctx_.Device(), sh.memory, nullptr);
    }
    if (diff_pipeline_ != VK_NULL_HANDLE) {
        f.vkDestroyPipeline(ctx_.Device(), diff_pipeline_, nullptr);
    }
    if (diff_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyPipelineLayout(ctx_.Device(), diff_layout_, nullptr);
    }
    if (diff_set_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyDescriptorSetLayout(ctx_.Device(), diff_set_layout_, nullptr);
    }
    if (buffer_ != VK_NULL_HANDLE) {
        f.vkDestroyBuffer(ctx_.Device(), buffer_, nullptr);
    }
    for (VkDeviceMemory m : chunks_) {
        f.vkFreeMemory(ctx_.Device(), m, nullptr);
    }
    if (bind_timeline_ != VK_NULL_HANDLE) {
        f.vkDestroySemaphore(ctx_.Device(), bind_timeline_, nullptr);
    }
}

void BufferCache::Invalidate(u64 addr, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lock{inval_mutex_};
    pending_inval_.emplace_back(addr, size);
    has_inval_.store(true, std::memory_order_release);
}

void BufferCache::InvalidateIfCached(u64 addr, u64 size) {
    // the GPU thread's own write (the command processor's): nothing to queue when no page of it is in video memory
    // (the block table is the GPU thread's) - the compute rings' read pointers are written after every packet, and
    // each queued invalidation cost a lock and a later scan (3% of the GPU thread in the village)
    if (size == 0 || !Ok()) {
        return;
    }
    const u64 end = std::min(addr + size, space_);
    if (addr >= end) {
        return;
    }
    const u64 first_page = addr >> kPageBits, last_page = (end - 1) >> kPageBits;
    for (u64 b = first_page / kPagesPerBlock; b <= last_page / kPagesPerBlock; ++b) {
        if (const Block* blk = FindBlock(b); blk != nullptr && (blk->valid & PageMask(b, first_page, last_page)) != 0) {
            Invalidate(addr, size);
            return;
        }
    }
}

void BufferCache::InvalidateCoarse(u64 addr, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lock{inval_mutex_};
    pending_coarse_.emplace_back(addr, size);
    has_inval_.store(true, std::memory_order_release);
}

void BufferCache::ApplyInvalidations() {
    if (!has_inval_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<u64, u64>> ranges, coarse;
    {
        std::scoped_lock lock{inval_mutex_};
        has_inval_.store(false, std::memory_order_relaxed);
        if (pending_inval_.empty() && pending_coarse_.empty()) {
            return;
        }
        ranges.swap(pending_inval_);
        coarse.swap(pending_coarse_);
    }
    const size_t exact = ranges.size();
    ++valid_gen_; // (pages may stop being valid: Prepare's memo is stale)
    // (a write-back shadow under an exact report needs nothing: those pages are invalid now, and a GPU write there - the
    // only thing compared with the shadow - uploads them first, which copies the same host bytes into the shadow)
    ranges.insert(ranges.end(), coarse.begin(), coarse.end());
    for (size_t r = 0; r < ranges.size(); ++r) {
        const u64 addr = ranges[r].first, size = ranges[r].second;
        const bool is_coarse = r >= exact;
        const u64 end = std::min(addr + size, space_);
        if (addr >= end) {
            continue;
        }
        const u64 first_page = addr >> kPageBits, last_page = (end - 1) >> kPageBits;
        const u64 first_block = first_page / kPagesPerBlock, last_block = last_page / kPagesPerBlock;
        // leaf by leaf: a range wider than what is resident (a whole pool written) skips the leaves never touched
        for (u64 leaf = first_block >> kLeafBits; leaf <= (last_block >> kLeafBits) && leaf < leaves_.size(); ++leaf) {
            Leaf* lp = leaves_[leaf].get();
            if (lp == nullptr) {
                continue;
            }
            const u64 b0 = std::max(first_block, leaf << kLeafBits);
            const u64 b1 = std::min(last_block, ((leaf + 1) << kLeafBits) - 1);
            for (u64 b = b0; b <= b1; ++b) {
                Block& blk = lp->blocks[b & kLeafMask];
                if (blk.valid == 0) {
                    continue;
                }
                if (blk.deferred) {
                    // FB-7: every CPU-side writer of a hidden block has it written back first; still hidden here: a
                    // writer that did not (counted - its bytes and the GPU's kept-back ones may not both survive);
                    // shown again (unmapped, given back): what was kept back is gone with the mapping
                    const bool hidden = tracker_ != nullptr && tracker_->Hidden(b << kBlockBits, 1);
                    if (hidden) {
                        ++stats_.lazy_unsynced_inval;
                        continue; // (kept: the GPU's copy stays the current one until a reader writes it back)
                    }
                    blk.deferred = false;
                    --deferred_count_;
                    deferred_set_.erase(b);
                    ++stats_.lazy_dropped;
                }
                // a coarse report keeps the pages the GPU wrote in a batch not completed (their data is newer)
                const u16 mask = PageMask(b, first_page, last_page);
                if (is_coarse && (blk.valid & mask) != 0) {
                    // the CPU writes here: small ranges in this block are streamed for a while (doubling back-off)
                    blk.cpu_writes = static_cast<u8>(std::min<u32>(blk.cpu_writes + 1u, 8u));
                    blk.quiet_from = frame_ + (30ull << blk.cpu_writes);
                }
                const u16 keep = is_coarse && blk.pending_seq > completed_ ? static_cast<u16>(blk.gpu_pending & mask)
                                                                           : u16{0};
                blk.valid &= static_cast<u16>(~(mask & ~keep));
                ++blk.version;
                if (keep != 0) {
                    deferred_inval_.push_back({b, keep, blk.pending_seq});
                }
                ++stats_.invalidations;
            }
        }
    }
}

bool BufferCache::EnsureBound(u64 first_block, u64 last_block) {
    const auto& f = Fn();
    const u64 bytes = 1ull << kBlockBits;
    u64 needed = 0;
    for (u64 b = first_block; b <= last_block; ++b) {
        const Block* blk = FindBlock(b);
        needed += (blk == nullptr || !blk->bound) ? bytes : 0;
    }
    if (stats_.resident_bytes + needed > budget_) {
        ++stats_.over_budget;
        return false;
    }
    for (u64 b = first_block; b <= last_block; ++b) {
        Block& blk = GetBlock(b);
        if (blk.bound) {
            continue;
        }
        if (chunk_used_ + bytes > kChunkBytes) {
            const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .allocationSize = kChunkBytes,
                                           .memoryTypeIndex = memory_type_};
            VkDeviceMemory m = VK_NULL_HANDLE;
            if (f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &m) != VK_SUCCESS) {
                ++stats_.over_budget;
                if (stats_.first_error.empty()) {
                    stats_.first_error = "video memory for the buffer cache exhausted";
                }
                return false;
            }
            chunks_.push_back(m);
            chunk_used_ = 0;
        }
        pending_binds_.push_back(VkSparseMemoryBind{.resourceOffset = b << kBlockBits,
                                                    .size = bytes,
                                                    .memory = chunks_.back(),
                                                    .memoryOffset = chunk_used_});
        chunk_used_ += bytes;
        stats_.resident_bytes += bytes;
        ++stats_.binds;
        blk.bound = true;
    }
    return true;
}

bool BufferCache::Upload(u64 addr, u64 size, VkCommandBuffer cb, u64 seq) {
    const auto& f = Fn();
    for (u64 done = 0; done < size;) {
        const u64 a = addr + done;
        const u64 piece = std::min(kMaxCopy - (a & (kMaxCopy - 1)), size - done);
        GuestBuffers::View v{};
        GuestBuffers::Failure fl{};
        if (!guest_.Get(a, piece, seq, v, fl)) {
            return false;
        }
        const VkBufferCopy copy{.srcOffset = v.offset, .dstOffset = a, .size = piece};
        f.vkCmdCopyBuffer(cb, v.buffer, buffer_, 1, &copy);
        // a write-back shadow over these pages takes the same host bytes (it holds what the host has where the video
        // memory copy is valid; the caller's barriers order both copies)
        for (const WbShadow& sh : wb_shadows_) {
            const u64 lo = std::max(a, sh.addr), hi = std::min(a + piece, sh.addr + sh.size);
            if (sh.valid && lo < hi) {
                const VkBufferCopy sc{.srcOffset = v.offset + (lo - a), .dstOffset = lo - sh.addr, .size = hi - lo};
                f.vkCmdCopyBuffer(cb, v.buffer, sh.buffer, 1, &sc);
            }
        }
        stats_.upload_bytes += piece;
        done += piece;
    }
    ++stats_.uploads;
    return true;
}

void BufferCache::CopyIntoShadows(u64 a, u64 size, VkCommandBuffer cb) {
    // a write-back by copy: the shadows over [a, a + size) take the video memory bytes too (else an older shadow word
    // that equals the video memory copy again later - a counter reset - would hide the change from the comparison)
    for (const WbShadow& sh : wb_shadows_) {
        const u64 lo = std::max(a, sh.addr), hi = std::min(a + size, sh.addr + sh.size);
        if (sh.valid && lo < hi) {
            const VkBufferCopy c{.srcOffset = lo, .dstOffset = lo - sh.addr, .size = hi - lo};
            Fn().vkCmdCopyBuffer(cb, buffer_, sh.buffer, 1, &c);
        }
    }
}

void BufferCache::InitDiff() {
    // SSPC_DIAG_WB_DIFF=off: every write-back copies whole ranges (the P4.6 behaviour)
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_WB_DIFF", v, sizeof(v)) == 3 && std::string_view(v) == "off") {
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_WB_DIFF=off - write-backs copy whole ranges");
        return;
    }
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    if (ctx_.Caps().max_push_descriptors < 3) {
        return;
    }
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < 3; ++i) {
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    const VkDescriptorSetLayoutCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                              .bindingCount = 3,
                                              .pBindings = b};
    const VkPushConstantRange pr{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16};
    VkShaderModule sm = VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo smi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = sizeof(kWbDiffSpirv),
                                       .pCode = kWbDiffSpirv};
    if (f.vkCreateDescriptorSetLayout(dev, &dci, nullptr, &diff_set_layout_) != VK_SUCCESS) {
        diff_set_layout_ = VK_NULL_HANDLE;
        return;
    }
    const VkPipelineLayoutCreateInfo pci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &diff_set_layout_,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pr};
    if (f.vkCreatePipelineLayout(dev, &pci, nullptr, &diff_layout_) != VK_SUCCESS ||
        f.vkCreateShaderModule(dev, &smi, nullptr, &sm) != VK_SUCCESS) {
        return;
    }
    const VkComputePipelineCreateInfo cpi{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = sm,
                  .pName = "main"},
        .layout = diff_layout_};
    if (f.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &diff_pipeline_) != VK_SUCCESS) {
        diff_pipeline_ = VK_NULL_HANDLE;
    }
    f.vkDestroyShaderModule(dev, sm, nullptr);
    wb_shadows_.reserve(kMaxShadows);
    if (char v[16] = {}; diff_pipeline_ != VK_NULL_HANDLE && tracker_ != nullptr &&
                         GetEnvironmentVariableA("SSPC_FB_LAZY_WB", v, sizeof(v)) > 0) {
        lazy_ = std::string_view(v) == "1" ? 1 : std::string_view(v) == "verify" ? 2 : 0;
        if (lazy_ != 0) {
            SSPC_LOG(GPU, Warning, "FrameBreak: SSPC_FB_LAZY_WB={} - large GPU-written ranges' blocks {} until a reader "
                                   "needs them",
                     v, lazy_ == 1 ? "kept back from the host" : "counted as if kept back (written back as before)");
        }
    }
    // R12 lights: FB-7's kept-back write-back for the tiled lighting's light index list only (mode 3; FB-7 itself wins)
    if (char v[4] = {}; lazy_ == 0 && diff_pipeline_ != VK_NULL_HANDLE && tracker_ != nullptr &&
                        GetEnvironmentVariableA("SSPC_R12_LIGHT_LIST_KEEP", v, sizeof(v)) == 1 && v[0] == '1') {
        lazy_ = 3;
        SSPC_LOG(GPU, Warning, "R12: SSPC_R12_LIGHT_LIST_KEEP=1 - the tiled lighting's per-tile light index list stays "
                               "in video memory (kept back from the host until a CPU access or a non-heap host reader)");
    }
    if (diff_pipeline_ != VK_NULL_HANDLE) {
        SSPC_LOG(GPU, Info, "native backend: buffer cache: write-backs of ranges >= {} KiB write only the words the GPU "
                            "changed (a video memory shadow per range)",
                 kDiffMin >> 10);
    }
}

BufferCache::WbShadow* BufferCache::ShadowFor(u64 b, u64 e, VkCommandBuffer cb, u64 seq, bool& inited) {
    inited = false;
    WbShadow* match = nullptr;
    for (WbShadow& sh : wb_shadows_) {
        if (match == nullptr && sh.addr <= b && e <= sh.addr + sh.size) {
            match = &sh; // a shadow holding the whole range
        }
    }
    for (WbShadow& sh : wb_shadows_) {
        if (&sh != match && sh.valid && sh.addr < e && b < sh.addr + sh.size) {
            // another shadow over these bytes would not see this write-back: made again before its next use (an
            // older shadow word equal to the video memory copy would hide a newer CPU write from the comparison)
            sh.valid = false;
        }
    }
    if (match == nullptr) {
        if (wb_shadows_.size() >= kMaxShadows || shadow_bytes_ + (e - b) > kShadowBudget) {
            return nullptr;
        }
        const auto& f = Fn();
        WbShadow sh{.addr = b, .size = e - b};
        const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = sh.size,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkBufferCreateInfo shared = bci;
        ctx_.ShareWithLane(shared); // (FrameBreak AQ)
        if (f.vkCreateBuffer(ctx_.Device(), &shared, nullptr, &sh.buffer) != VK_SUCCESS) {
            return nullptr;
        }
        VkMemoryRequirements req{};
        f.vkGetBufferMemoryRequirements(ctx_.Device(), sh.buffer, &req);
        const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = memory_type_};
        if ((req.memoryTypeBits & (1u << memory_type_)) == 0 ||
            f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &sh.memory) != VK_SUCCESS) {
            f.vkDestroyBuffer(ctx_.Device(), sh.buffer, nullptr);
            return nullptr;
        }
        f.vkBindBufferMemory(ctx_.Device(), sh.buffer, sh.memory, 0);
        shadow_bytes_ += sh.size;
        wb_shadows_.push_back(sh);
        match = &wb_shadows_.back();
    }
    if (!match->valid) {
        // the host's bytes now (before this batch's write-back: what the host had since the last sync), all of it
        const auto& f = Fn();
        // REA-5 (Sync Validation in the village with merged runs): ordered after the copies this write-back recorded
        // already (into the host and into the shadows) and before the ones after it - an execution and memory
        // dependency between transfers on both sides (inits are rare: tens a run)
        const VkMemoryBarrier2 tb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        const VkDependencyInfo tdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                    .pMemoryBarriers = &tb};
        f.vkCmdPipelineBarrier2(cb, &tdep);
        const u64 sb = match->addr, se = match->addr + match->size;
        for (u64 a = sb; a < se;) {
            const u64 piece = std::min(kMaxCopy - (a & (kMaxCopy - 1)), se - a);
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            if (!guest_.Get(a, piece, seq, v, fl)) {
                return nullptr;
            }
            const VkBufferCopy c{.srcOffset = v.offset, .dstOffset = a - sb, .size = piece};
            f.vkCmdCopyBuffer(cb, v.buffer, match->buffer, 1, &c);
            a += piece;
        }
        f.vkCmdPipelineBarrier2(cb, &tdep);
        ++stats_.wb_shadow_inits;
        inited = true;
    }
    return match;
}

bool BufferCache::Resident(u64 addr, u64 size, Range& out) {
    if (!Ok() || size == 0 || size > kMaxCached || addr + size > space_) {
        return false;
    }
    ApplyInvalidations();
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    for (u64 b = first_page / kPagesPerBlock; b <= last_page / kPagesPerBlock; ++b) {
        const Block* blk = FindBlock(b);
        const u16 mask = PageMask(b, first_page, last_page);
        if (blk == nullptr || !blk->bound || (blk->valid & mask) != mask) {
            return false;
        }
    }
    out = {buffer_, addr, space_ - addr, true};
    return true;
}

BufferCache::Residency BufferCache::Classify(u64 addr, u64 size, Range& out) {
    if (!Ok() || size == 0 || size > kMaxCached || addr + size > space_) {
        return Residency::None;
    }
    ApplyInvalidations();
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    bool all = true, any = false;
    for (u64 b = first_page / kPagesPerBlock; b <= last_page / kPagesPerBlock; ++b) {
        const Block* blk = FindBlock(b);
        const u16 mask = PageMask(b, first_page, last_page);
        const u16 v = blk != nullptr && blk->bound ? static_cast<u16>(blk->valid & mask) : u16{0};
        all &= v == mask;
        any |= v != 0;
    }
    if (all) {
        out = {buffer_, addr, space_ - addr, true};
        return Residency::All;
    }
    return any ? Residency::Partial : Residency::None;
}

bool BufferCache::Promotable(u64 addr, u64 size, u64 frame) {
    frame_ = std::max(frame_, frame);
    if (!Ok() || tracker_ == nullptr || size == 0 || size > kMaxCached || addr + size > space_ ||
        stats_.resident_bytes + (2ull << kBlockBits) > budget_ - budget_ / 8) {
        return false;
    }
    const u64 first_block = addr >> kBlockBits, last_block = (addr + size - 1) >> kBlockBits;
    for (u64 b = first_block; b <= last_block; ++b) {
        if (const Block* blk = FindBlock(b); blk != nullptr && frame_ < blk->quiet_from) {
            return false;
        }
    }
    return tracker_->CanArm(addr, size);
}

u32 BufferCache::RangeVersion(u64 addr, u64 size) {
    if (!Ok() || size == 0 || addr + size > space_) {
        return 0;
    }
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    u32 v = 0;
    for (u64 b = first_page / kPagesPerBlock; b <= last_page / kPagesPerBlock; ++b) {
        const Block* blk = FindBlock(b);
        const u16 mask = PageMask(b, first_page, last_page);
        if (blk == nullptr || !blk->bound || (blk->valid & mask) != mask || blk->deferred) {
            return 0; // not wholly current in video memory (then not write-tracked either)
        }
        v = v * 0x9E3779B1u + blk->version + 1;
    }
    return v == 0 ? 1 : v;
}

u32 BufferCache::PromoteWhy(u64 addr, u64 size) {
    if (!Ok() || tracker_ == nullptr || size == 0 || size > kMaxCached || addr + size > space_) {
        return 1;
    }
    if (stats_.resident_bytes + (2ull << kBlockBits) > budget_ - budget_ / 8) {
        return 2;
    }
    for (u64 b = addr >> kBlockBits; b <= (addr + size - 1) >> kBlockBits; ++b) {
        if (const Block* blk = FindBlock(b); blk != nullptr && frame_ < blk->quiet_from) {
            return 3;
        }
    }
    return tracker_->CanArm(addr, size) ? 0 : 4;
}

bool BufferCache::AnyValid(u64 addr, u64 size) {
    if (!Ok() || size == 0 || addr + size > space_) {
        return false;
    }
    ApplyInvalidations();
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    const u64 first_block = first_page / kPagesPerBlock, last_block = last_page / kPagesPerBlock;
    for (u64 b = first_block; b <= last_block;) {
        const u64 leaf = b >> kLeafBits;
        if (leaf >= leaves_.size()) {
            break;
        }
        if (!leaves_[leaf]) {
            b = (leaf + 1) << kLeafBits; // never touched: none of its blocks is valid
            continue;
        }
        if ((leaves_[leaf]->blocks[b & kLeafMask].valid & PageMask(b, first_page, last_page)) != 0) {
            return true;
        }
        ++b;
    }
    return false;
}

bool BufferCache::Prepare(u64 addr, u64 size, bool write, VkCommandBuffer cb, u64 seq, Range& out) {
    const auto fallback = [&] {
        ++stats_.uncached;
        GuestBuffers::View v{};
        GuestBuffers::Failure fl{};
        u64 take = size;
        // REA-7: a vertex-data heap's V# (read-only, >= 256 MiB) may reach past the memory the game committed (it grows
        // as meshes stream in): the largest importable prefix, found once per (addr, size); reads past it get the
        // robust access's zeros instead of the whole draw being skipped
        if (!write && size >= kHeapMin) {
            if (const auto it = heap_clamp_.find({addr, size}); it != heap_clamp_.end()) {
                take = it->second;
            }
        }
        if (take == 0 || !guest_.Get(addr, take, seq, v, fl)) {
            if (write || size < kHeapMin || take != size) {
                return false;
            }
            u64 s = size;
            if (GuestMemory* gm = GetGuestMemory()) {
                s = std::min(s, gm->ClampRangeSize(addr, size));
            }
            bool found = false;
            for (int tries = 0; tries < 24 && s >= kHeapMin; ++tries) {
                if (s != size && guest_.Get(addr, s, seq, v, fl)) {
                    found = true;
                    break;
                }
                s = (s - std::max<u64>(s / 16, 1ull << 20)) & ~0xFFFFull;
            }
            heap_clamp_[{addr, size}] = found ? s : 0;
            if (!found) {
                return false;
            }
            ++stats_.heap_clamped;
            SSPC_LOG(GPU, Warning, "native backend: heap binding [{:#x}, +{:#x}) bound as its importable [{:#x}, +{:#x}) "
                                   "(REA-7)", addr, size, addr, s);
        }
        out = {v.buffer, v.offset, v.size - v.offset, false};
        return true;
    };
    if (!Ok() || size == 0 || addr + size > space_ || tracker_ == nullptr) {
        return fallback();
    }
    ApplyInvalidations();
    // the same range prepared in this batch since anything could invalidate it: nothing to do again (P4.6 night: a
    // 12 MiB particle pool bound by ~1500 dispatches a frame walked its ~190 blocks each time)
    for (const PrepMemo& m : prep_memo_) {
        if (m.seq == seq && m.gen == valid_gen_ && m.addr == addr && m.size == size && (m.write || !write)) {
            ++stats_.prep_memo_hits;
            out = {buffer_, addr, space_ - addr, true};
            return true;
        }
    }
    u64 t0 = Gpu::Shader::FineTicks();
    // (a large range is checked first: its tracking test would walk the whole range for nothing)
    if (size > kMaxCached || stats_.resident_bytes >= budget_ || !tracker_->CanArm(addr, size)) {
        // a range larger than the cache takes (a whole pool bound for a part of it) - or the budget is spent: the host
        // import. Kept coherent with the video memory copy: what the GPU wrote there in this batch is copied back to
        // the host before this command, and a write here makes the video memory copy older.
        ++stats_.large_ranges;
        if (diag_large_) {
            const u8 why = size > kMaxCached ? 0 : stats_.resident_bytes >= budget_ ? 1 : 2;
            if (++large_binds_[{addr, size, why, write}] == 1 && size >= kHeapMin) {
                DiagHeapLayout(addr, size);
            }
        }
        if (heap_reads_ && !write && size >= kHeapMin) {
            ++heap_unflushed_; // REA-2: a vertex-data heap - GPU-written data in its range is not what it reads
        } else {
            // (R12 mode 3: a heap reading leaves the light index list kept back - REA-2's assumption for that range)
            FlushWritten(addr, size, cb, seq, lazy_ == 3 && !write && size >= kHeapMin);
        }
        if (write) {
            Invalidate(addr, size);
            NoteHostWrite(addr, size, seq);
        }
        const bool ok = fallback();
        stats_.t_large += Gpu::Shader::FineTicks() - t0;
        return ok;
    }
    const u64 first_block = addr >> kBlockBits, last_block = (addr + size - 1) >> kBlockBits;
    t0 = Gpu::Shader::FineTicks();
    const bool bound = EnsureBound(first_block, last_block);
    stats_.t_bound += Gpu::Shader::FineTicks() - t0;
    if (!bound) {
        if (diag_large_) {
            ++large_binds_[{addr, size, u8{3}, write}];
        }
        FlushWritten(addr, size, cb, seq);
        if (write) {
            Invalidate(addr, size);
            NoteHostWrite(addr, size, seq);
        }
        return fallback();
    }
    // the pages not valid in video memory, as runs
    t0 = Gpu::Shader::FineTicks();
    uploads_.clear();
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    for (u64 b = first_block; b <= last_block; ++b) {
        Block& blk = GetBlock(b);
        const u16 mask = PageMask(b, first_page, last_page);
        u16 missing = static_cast<u16>(mask & ~blk.valid);
        // P4.9: a CPU write gives the whole block back; the bindings of the frame then asked for its pages one part at a
        // time - each an upload, a protection (a system call) and two or three full barriers (village: ~330 uploads a
        // frame for ~30 blocks). The block's other pages a binding used before and that are invalid now come along
        // with the first: invalid pages hold nothing newer than the host (the same pages a later binding would upload,
        // recorded earlier - the barriers below order them after the batch's earlier writes the same way).
        if (const u16 extra = static_cast<u16>(blk.used & ~blk.valid & ~mask); missing != 0 && extra != 0 && block_upload_) {
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            if (guest_.Get(b << kBlockBits, 1ull << kBlockBits, seq, v, fl)) {
                missing |= extra;
                stats_.block_upload_pages += static_cast<u64>(std::popcount(extra));
            } else {
                ++stats_.block_upload_refused; // not importable whole: only the binding's pages
            }
        }
        blk.used |= mask;
        blk.valid |= mask | missing;
        while (missing != 0) {
            const u32 lo = static_cast<u32>(std::countr_zero(missing));
            u32 hi = lo;
            while (hi + 1 < 16 && ((missing >> (hi + 1)) & 1)) {
                ++hi;
            }
            missing &= static_cast<u16>(~(((1u << (hi - lo + 1)) - 1u) << lo));
            const u64 pa = ((b * kPagesPerBlock) + lo) << kPageBits, pe = ((b * kPagesPerBlock) + hi + 1) << kPageBits;
            if (!uploads_.empty() && uploads_.back().second == pa) {
                uploads_.back().second = pe;
            } else {
                uploads_.emplace_back(pa, pe);
            }
        }
    }
    stats_.t_scan += Gpu::Shader::FineTicks() - t0;
    if (!uploads_.empty()) {
        const auto& f = Fn();
        ++stats_.n_upload_calls;
        t0 = Gpu::Shader::FineTicks();
        if (before_upload_ != nullptr) {
            before_upload_(before_upload_ctx_); // outside rendering, after the batch's earlier writes
        }
        stats_.t_hook += Gpu::Shader::FineTicks() - t0;
        // earlier accesses of these pages (reads of their old copy) -> the copies -> later accesses
        VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
        for (const auto& [b, e] : uploads_) {
            // write-protect first: a CPU write from now on invalidates the pages again (the copy may still read it)
            t0 = Gpu::Shader::FineTicks();
            tracker_->Arm(b, e - b);
            stats_.t_arm += Gpu::Shader::FineTicks() - t0;
            t0 = Gpu::Shader::FineTicks();
            const bool uploaded = Upload(b, e - b, cb, seq);
            stats_.t_copy += Gpu::Shader::FineTicks() - t0;
            if (!uploaded) {
                // not current: the pages stay invalid (they were marked valid above, before the copy - review MEDIUM 7)
                const u64 fp = b >> kPageBits, lp = (e - 1) >> kPageBits;
                for (u64 blk_i = fp / kPagesPerBlock; blk_i <= lp / kPagesPerBlock; ++blk_i) {
                    if (Block* blk = FindBlock(blk_i)) {
                        blk->valid &= static_cast<u16>(~PageMask(blk_i, fp, lp));
                        ++blk->version;
                    }
                }
                if (stats_.first_error.empty()) {
                    stats_.first_error = fmt::format("upload of [{:#x}, {:#x}): no host import", b, e);
                }
            }
        }
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        f.vkCmdPipelineBarrier2(cb, &dep);
    }
    if (write) {
        for (u64 b = first_block; b <= last_block; ++b) {
            Block& blk = GetBlock(b);
            blk.gpu_pending |= PageMask(b, first_page, last_page);
            ++blk.version;
            blk.pending_seq = seq;
        }
        if (!written_.empty() && written_.back().second >= addr && written_.back().first <= addr + size) {
            written_.back().first = std::min(written_.back().first, addr);
            written_.back().second = std::max(written_.back().second, addr + size);
        } else {
            written_.emplace_back(addr, addr + size);
        }
    }
    out = {buffer_, addr, space_ - addr, true};
    prep_memo_[prep_next_++ % prep_memo_.size()] = {addr, size, seq, valid_gen_, write};
    return true;
}

void BufferCache::DiagHeapLayout(u64 addr, u64 size) {
    // REA-4: how much of a heap binding is GPU-mapped (64 KiB blocks), whether the tracker could arm it, and its chain of
    // physical backing runs (host-contiguous or not) - whether it could live in video memory
    GuestMemory* gm = GetGuestMemory();
    const u64 end = addr + size;
    u64 mapped = 0, armable = 0, never = 0;
    for (u64 b = addr & ~0xFFFFull; b < end; b += 0x10000) {
        if (GuestMemory::IsValidGpuMapping(b, 0x10000)) {
            mapped += 0x10000;
            (tracker_ != nullptr && tracker_->CanArm(b, 0x10000) ? armable : never) += 0x10000;
        }
    }
    SSPC_LOG(GPU, Info, "REA-4 heap [{:#x}, +{:#x}): GPU-mapped {} MiB (armable {} MiB, never armable {} MiB)", addr, size,
             mapped >> 20, armable >> 20, never >> 20);
    u64 cur = addr;
    u8* prev_host = nullptr;
    u64 prev_end = 0;
    for (int i = 0; i < 64 && cur < end && gm != nullptr; ++i) {
        u8* host = nullptr;
        u64 rb = 0, rs = 0;
        if (!gm->BackingRun(cur, &host, &rb, &rs) || rs == 0) {
            SSPC_LOG(GPU, Info, "REA-4   no backing run at {:#x}", cur);
            cur = (cur + 0x10000) & ~0xFFFFull;
            prev_host = nullptr;
            continue;
        }
        const bool contiguous = prev_host != nullptr && rb == prev_end && host == prev_host;
        SSPC_LOG(GPU, Info, "REA-4   run [{:#x}, +{:#x}) host {} {}", rb, rs, static_cast<const void*>(host),
                 contiguous ? "(host-contiguous with the previous run)" : "");
        prev_host = host + rs;
        prev_end = rb + rs;
        cur = rb + rs;
    }
}

std::string BufferCache::LargeBindReport() const {
    if (!diag_large_) {
        return {};
    }
    std::vector<std::pair<u64, std::tuple<u64, u64, u8, bool>>> v;
    u64 total = 0;
    for (const auto& [k, n] : large_binds_) {
        v.emplace_back(n, k);
        total += n;
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    static constexpr const char* kWhy[] = {"larger than 16 MiB", "over budget", "cannot arm (import window)", "not bound"};
    std::string out = fmt::format("REA-1 large binds: {} distinct, {} bindings\n", v.size(), total);
    const auto top = [&](const std::map<u64, u64>& m, const char* what, u64 unit) {
        std::vector<std::pair<u64, u64>> s(m.begin(), m.end());
        std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        u64 sum = 0;
        for (const auto& e : s) {
            sum += e.second;
        }
        out += fmt::format("REA-1 {} by 1 MiB of address (total {}):\n", what, sum / unit);
        for (size_t i = 0; i < s.size() && i < 24; ++i) {
            out += fmt::format("  {:#x} {}\n", s[i].first << 20, s[i].second / unit);
        }
    };
    top(diag_wb_, "changed-words write-back MiB", 1ull << 20);
    top(diag_kept_, "blocks kept back", 1);
    // the most frequent, then every range over the particle pools' area (0x26c000000..0x271000000) whatever its count
    for (size_t i = 0; i < v.size(); ++i) {
        const auto& [addr, size, why, write] = v[i].second;
        const bool particles = addr < 0x271000000ull && addr + size > 0x26c000000ull;
        if (i < 40 || particles) {
            out += fmt::format("  {:>9} {:#x}+{:#x} {} {}{}\n", v[i].first, addr, size, write ? "W" : "R", kWhy[why],
                               particles ? " [particle area]" : "");
        }
    }
    return out;
}

void BufferCache::AddGpuOnlyRange(u64 addr, u64 size) {
    const std::pair<u64, u64> r{addr, addr + size};
    if (lazy_ != 3 || std::ranges::find(gpu_only_, r) != gpu_only_.end() || gpu_only_.size() >= 4) {
        return;
    }
    gpu_only_.push_back(r);
    SSPC_LOG(GPU, Info, "R12: light index list [{:#x}, +{:#x}) registered as GPU-only (kept back from the host)", addr,
             size);
}

bool BufferCache::InGpuOnlyRange(u64 block) const {
    const u64 b = block << kBlockBits, e = b + (1ull << kBlockBits);
    return std::ranges::any_of(gpu_only_, [&](const std::pair<u64, u64>& r) { return r.first <= b && e <= r.second; });
}

void BufferCache::FlushWritten(u64 addr, u64 size, VkCommandBuffer cb, u64 seq, bool skip_gpu_only) {
    // this batch's GPU writes overlapping [addr, addr + size): copied back to the host now (a host import of the range
    // is accessed next) and dropped from the batch end's write-back - the host import may be written next (a shader
    // through it, WRITE_DATA): the end's copy of the older video memory bytes would undo that (review HIGH 1)
    const u64 end = addr + size;
    bool any = false;
    // FB-7: what earlier batches kept back there reaches the host first (the host import is read or written next)
    FlushDeferred(addr, size, cb, seq, false, 0, skip_gpu_only);
    for (const auto& [b, e] : written_) {
        if (b >= end || e <= addr) {
            continue;
        }
        const auto& f = Fn();
        if (!any) {
            if (before_upload_ != nullptr) {
                before_upload_(before_upload_ctx_);
            }
            const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                      .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
            const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                       .pMemoryBarriers = &mb};
            f.vkCmdPipelineBarrier2(cb, &dep);
            any = true;
        }
        const u64 cb0 = std::max(b, addr), ce = std::min(e, end);
        for (u64 a = cb0; a < ce;) {
            const u64 piece = std::min(kMaxCopy - (a & (kMaxCopy - 1)), ce - a);
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            if (guest_.Get(a, piece, seq, v, fl)) {
                const VkBufferCopy copy{.srcOffset = a, .dstOffset = v.offset, .size = piece};
                f.vkCmdCopyBuffer(cb, buffer_, v.buffer, 1, &copy);
                stats_.writeback_bytes += piece;
                CopyIntoShadows(a, piece, cb);
            }
            a += piece;
        }
        ++stats_.early_writebacks;
    }
    if (any) {
        std::vector<std::pair<u64, u64>> rest;
        rest.reserve(written_.size() + 1);
        for (const auto& [b, e] : written_) {
            if (b >= end || e <= addr) {
                rest.emplace_back(b, e);
                continue;
            }
            if (b < addr) {
                rest.emplace_back(b, addr);
            }
            if (e > end) {
                rest.emplace_back(end, e);
            }
        }
        written_.swap(rest);
        ++valid_gen_; // (written ranges left the batch's list)
        const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        Fn().vkCmdPipelineBarrier2(cb, &dep);
    }
}

void BufferCache::NoteHostWrite(u64 addr, u64 size, u64 seq) {
    host_writes_.push_back({addr, addr + size, seq});
    hw_lo_ = std::min(hw_lo_, addr);
    hw_hi_ = std::max(hw_hi_, addr + size);
}

bool BufferCache::GpuWritePending(u64 addr, u64 size, u64 completed) {
    if (!Ok() || size == 0 || addr + size > space_) {
        return false;
    }
    const u64 first_page = addr >> kPageBits, last_page = (addr + size - 1) >> kPageBits;
    for (u64 b = first_page / kPagesPerBlock; b <= last_page / kPagesPerBlock; ++b) {
        Block* blk = FindBlock(b);
        if (blk != nullptr && blk->deferred) {
            return true; // (FB-7: kept back - the host has it only once a reader wrote it back)
        }
        if (blk == nullptr || blk->gpu_pending == 0) {
            continue;
        }
        if (blk->pending_seq <= completed) {
            blk->gpu_pending = 0; // written back: the host has it
            continue;
        }
        if ((blk->gpu_pending & PageMask(b, first_page, last_page)) != 0) {
            return true;
        }
    }
    return false;
}

bool BufferCache::HostWritePending(u64 addr, u64 size) const {
    if (host_writes_.empty() || addr >= hw_hi_ || addr + size <= hw_lo_) {
        return false;
    }
    return std::ranges::any_of(host_writes_, [&](const HostWrite& w) { return addr < w.end && w.begin < addr + size; });
}

void BufferCache::Collect(u64 completed) {
    if (!wb_snaps_.empty()) {
        CheckWriteBacks(completed);
    }
    completed_ = std::max(completed_, completed);
    if (!unhide_pending_.empty()) {
        // FB-7: blocks written back by a reader - shown to the CPU once that write-back completed
        std::erase_if(unhide_pending_, [&](const std::pair<u64, u64>& u) {
            if (u.second > completed) {
                return false;
            }
            if (tracker_ != nullptr) {
                const Block* bp = FindBlock(u.first >> kBlockBits);
                if (bp == nullptr || !bp->deferred) { // (not kept back again since)
                    tracker_->Unhide(u.first);
                }
            }
            return true;
        });
    }
    if (!deferred_inval_.empty()) {
        // a coarse report's kept pages: invalid once the GPU's batch completed (written back) - unless the GPU wrote
        // them again since (then after that batch)
        std::erase_if(deferred_inval_, [&](DeferredInval& d) {
            Block* blk = FindBlock(d.block);
            if (blk == nullptr) {
                return true;
            }
            if (blk->pending_seq > completed_ && (blk->gpu_pending & d.mask) != 0) {
                d.seq = blk->pending_seq;
                return false;
            }
            if (d.seq > completed_) {
                return false;
            }
            blk->valid &= static_cast<u16>(~d.mask);
            ++blk->version;
            ++valid_gen_;
            return true;
        });
    }
    if (host_writes_.empty()) {
        return;
    }
    std::erase_if(host_writes_, [&](const HostWrite& w) { return w.seq <= completed; });
    hw_lo_ = ~0ull;
    hw_hi_ = 0;
    for (const auto& w : host_writes_) {
        hw_lo_ = std::min(hw_lo_, w.begin);
        hw_hi_ = std::max(hw_hi_, w.end);
    }
}

void BufferCache::RecordWriteBack(VkCommandBuffer cb, u64 seq) {
    if (written_.empty()) {
        return;
    }
    const auto& f = Fn();
    std::ranges::sort(written_);
    std::vector<std::pair<u64, u64>> merged;
    for (const auto& r : written_) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    written_.clear();
    // the batch's writes before the copies and the write-back shader (it reads the video memory copy and the shadows)
    VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cb, &dep);
    // SSPC_DIAG_WB_DIRTY=1: the host's bytes of each range before its write-back, compared with them once the batch
    // completed (Collect): how much of what is copied back did the GPU change (gpu_native.txt)
    static const bool dirty_diag = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_WB_DIRTY", v, sizeof(v)) == 1 && v[0] == '1';
    }();
    // the regions by destination window: one copy command per window (P4.6 night: ~2100 ranges a frame in the
    // village were 2100 copy commands)
    wb_regions_.clear();
    // large ranges: only the words the GPU changed reach the host (P4.6 night, SSPC_DIAG_WB_DIRTY in the village: of
    // ~48 MiB a frame copied back, 87% in ~3 ranges of ~11 MiB a frame, 5% of the words changed)
    ApplyInvalidations(); // (a shadow under a write the tracker did not see is made again first)
    struct Diff {
        u64 b, e;
        WbShadow* sh;
    };
    std::vector<Diff> diffs;
    bool any_init = false;
    for (const auto& [b, e] : merged) {
        const u64 size = e - b;
        const u32 bucket = size <= (4u << 10) ? 0 : size <= (64u << 10) ? 1 : size <= (1u << 20) ? 2 : size <= (4u << 20) ? 3 : 4;
        ++stats_.wb_hist_count[bucket];
        stats_.wb_hist_bytes[bucket] += size;
        if (dirty_diag && size <= (16ull << 20)) {
            WbSnap snap{seq, b, std::vector<u8>(size)};
            if (GuestMemory* gm = GetGuestMemory()) {
                gm->CopySparse(b, snap.before.data(), size);
                wb_snaps_.push_back(std::move(snap));
            }
        }
        if (diff_pipeline_ != VK_NULL_HANDLE && size >= kDiffMin && (b & 3) == 0 && (size & 3) == 0) {
            bool inited = false;
            if (WbShadow* sh = ShadowFor(b, e, cb, seq, inited)) {
                any_init |= inited;
                diffs.push_back({b, e, sh});
                ++stats_.writebacks;
                continue;
            }
        }
        for (u64 a = b; a < e;) {
            const u64 piece = std::min(kMaxCopy - (a & (kMaxCopy - 1)), e - a);
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            if (guest_.Get(a, piece, seq, v, fl)) {
                wb_regions_.push_back({v.buffer, VkBufferCopy{.srcOffset = a, .dstOffset = v.offset, .size = piece}});
                stats_.writeback_bytes += piece;
                CopyIntoShadows(a, piece, cb); // (a shadow over it takes the same bytes the host gets)
            } else if (stats_.first_error.empty()) {
                stats_.first_error = fmt::format("write-back of [{:#x}, {:#x}): no host import", a, a + piece);
            }
            a += piece;
        }
        ++stats_.writebacks;
    }
    if (any_init) {
        // a shadow made from the host just now read host bytes the plain copies below may write (REA-5: the merged heap
        // window made it one buffer - Sync Validation's WRITE_AFTER_READ): those copies after that read
        const VkMemoryBarrier2 sb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        const VkDependencyInfo sdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                    .pMemoryBarriers = &sb};
        f.vkCmdPipelineBarrier2(cb, &sdep);
    }
    std::ranges::stable_sort(wb_regions_, [](const auto& x, const auto& y) { return x.first < y.first; });
    wb_copies_.clear();
    for (size_t i = 0; i < wb_regions_.size(); ++i) {
        wb_copies_.push_back(wb_regions_[i].second);
        if (i + 1 == wb_regions_.size() || wb_regions_[i + 1].first != wb_regions_[i].first) {
            f.vkCmdCopyBuffer(cb, buffer_, wb_regions_[i].first, static_cast<u32>(wb_copies_.size()), wb_copies_.data());
            ++stats_.wb_commands;
            wb_copies_.clear();
        }
    }
    if (wb_mark_) {
        wb_mark_();
    }
    if (diffs.empty()) {
        return;
    }
    if (any_init) { // the shadows' host bytes before the shader compares with them
        const VkMemoryBarrier2 ib{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        const VkDependencyInfo idep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                    .pMemoryBarriers = &ib};
        f.vkCmdPipelineBarrier2(cb, &idep);
    }
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, diff_pipeline_);
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    // SSPC_DIAG_MEASURE=wb_nohost: MEASUREMENT ONLY - the changed words go to the shadow again instead of the host (the
    // shader's comparison without its PCIe stores; the host's copy is wrong)
    static const bool nohost = [] {
        char v[32] = {};
        const bool s = GetEnvironmentVariableA("SSPC_DIAG_MEASURE", v, sizeof(v)) > 0 && std::string_view(v) == "wb_nohost";
        if (s) {
            SSPC_LOG(GPU, Warning, "MEASUREMENT ONLY: SSPC_DIAG_MEASURE=wb_nohost - changed words not written to the host");
        }
        return s;
    }();
    (void)align;
    std::vector<std::pair<u64, u64>> segs;
    std::vector<u64> poison_blocks; // (REA-2 SSPC_REA_POISON)
    for (const Diff& d : diffs) {
        // FB-7: the blocks wholly inside the range kept back (hidden) - the rest written back now
        segs.clear();
        u64 cur = d.b;
        if (lazy_ != 0 && tracker_ != nullptr) {
            constexpr u64 kBlock = 1ull << kBlockBits;
            for (u64 blk = (d.b + kBlock - 1) >> kBlockBits; blk < (d.e >> kBlockBits); ++blk) {
                Block* bp = FindBlock(blk);
                bool keep = bp != nullptr && bp->deferred;
                if (!keep && bp != nullptr && !bp->eager && bp->valid == 0xFFFF && (lazy_ != 3 || InGpuOnlyRange(blk))) {
                    if (lazy_ == 2) {
                        ++stats_.lazy_would_defer;
                    } else if (tracker_->Hide(blk << kBlockBits)) {
                        bp->deferred = true;
                        ++deferred_count_;
                        deferred_set_.insert(blk);
                        ++stats_.lazy_deferred;
                        if (diag_large_) {
                            ++diag_kept_[blk >> 4]; // (1 MiB buckets)
                        }
                        if (poison_) {
                            poison_blocks.push_back(blk);
                        }
                        keep = true;
                    }
                }
                if (keep) {
                    if (cur < (blk << kBlockBits)) {
                        segs.emplace_back(cur, blk << kBlockBits);
                    }
                    cur = (blk + 1) << kBlockBits;
                }
            }
        }
        if (cur < d.e) {
            segs.emplace_back(cur, d.e);
        }
        bool in_step = true;
        for (const auto& [sb, se] : segs) {
            for (u64 a = sb; a < se;) {
                const u64 piece = std::min(kMaxCopy - (a & (kMaxCopy - 1)), se - a);
                GuestBuffers::View v{};
                GuestBuffers::Failure fl{};
                if (!guest_.Get(a, piece, seq, v, fl) || (v.offset & 3) != 0) {
                    // (no host view: the shadow no longer matches the host)
                    in_step = false;
                    if (stats_.first_error.empty()) {
                        stats_.first_error = fmt::format("write-back of [{:#x}, {:#x}): no host import", a, a + piece);
                    }
                    a += piece;
                    continue;
                }
                if (nohost) {
                    DiffPiece(cb, a, piece, *d.sh, d.sh->buffer, a - d.sh->addr);
                } else {
                    DiffPiece(cb, a, piece, *d.sh, v.buffer, v.offset);
                }
                a += piece;
            }
        }
        d.sh->valid = in_step; // the shadow now holds what the host has (where the GPU wrote: the GPU's words)
    }
    // REA-2 correctness test (SSPC_REA_POISON=1): the host copy of the blocks kept back now and their shadow's bytes
    // are 0xFFFFFFFF - a reader of the host copy that is not written back first sees the poison; the write-back (the
    // changed words against the shadow) still restores every word that differs
    if (!poison_blocks.empty()) {
        const VkMemoryBarrier2 pb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        const VkDependencyInfo pdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                    .pMemoryBarriers = &pb};
        f.vkCmdPipelineBarrier2(cb, &pdep);
        constexpr u64 kBlock = 1ull << kBlockBits;
        for (const u64 blk : poison_blocks) {
            const u64 a = blk << kBlockBits;
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            if (guest_.Get(a, kBlock, seq, v, fl) && (v.offset & 3) == 0) {
                f.vkCmdFillBuffer(cb, v.buffer, v.offset, kBlock, 0xFFFFFFFFu);
            }
            for (const WbShadow& sh : wb_shadows_) {
                if (sh.buffer != VK_NULL_HANDLE && sh.addr <= a && a + kBlock <= sh.addr + sh.size) {
                    f.vkCmdFillBuffer(cb, sh.buffer, a - sh.addr, kBlock, 0xFFFFFFFFu);
                }
            }
            ++poisoned_blocks_;
        }
        f.vkCmdPipelineBarrier2(cb, &pdep);
        poison_blocks.clear();
    }
}

void BufferCache::DiffPiece(VkCommandBuffer cb, u64 a, u64 piece, const WbShadow& sh, VkBuffer host, u64 host_offset) {
    const auto& f = Fn();
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    const u64 in_sh = a - sh.addr; // (the shadow may hold more than this range)
    const u64 c_off = a & ~(align - 1), s_off = in_sh & ~(align - 1), h_off = host_offset & ~(align - 1);
    const VkDescriptorBufferInfo bi[3] = {{buffer_, c_off, a + piece - c_off},
                                          {sh.buffer, s_off, in_sh + piece - s_off},
                                          {host, h_off, host_offset + piece - h_off}};
    VkWriteDescriptorSet w[3];
    for (u32 i = 0; i < 3; ++i) {
        w[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bi[i]};
    }
    f.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, diff_layout_, 0, 3, w);
    const u32 pc[4] = {static_cast<u32>(piece / 4), static_cast<u32>((a - c_off) / 4), static_cast<u32>((in_sh - s_off) / 4),
                       static_cast<u32>((host_offset - h_off) / 4)};
    f.vkCmdPushConstants(cb, diff_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    f.vkCmdDispatch(cb, static_cast<u32>((piece / 4 + 255) / 256), 1, 1);
    stats_.wb_diff_bytes += piece;
    ++stats_.wb_diff_dispatches;
    if (diag_large_) {
        diag_wb_[a >> 20] += piece;
    }
}

bool BufferCache::KeptBack(u64 addr, u64 size) const {
    return (lazy_ == 1 || lazy_ == 3) && tracker_ != nullptr && tracker_->Hidden(addr, size);
}

bool BufferCache::FlushDeferred(u64 addr, u64 size, VkCommandBuffer cb, u64 seq, bool eager, u32 why,
                                bool skip_gpu_only) {
    if (deferred_count_ == 0 || size == 0) {
        return false;
    }
    if (skip_gpu_only && lazy_ == 3) {
        // (R12 mode 3 keeps back only blocks of the registered GPU-only ranges: a heap reading leaves them all)
        ++gpu_only_heap_skips_;
        return false;
    }
    const u64 end = std::min(addr + size, space_);
    if (addr >= end) {
        return false;
    }
    if (eager && tracker_ != nullptr) {
        // a CPU access of blocks written back for a GPU reader but still hidden: never kept back again, shown
        for (u64 b = addr >> kBlockBits; b <= (end - 1) >> kBlockBits && b - (addr >> kBlockBits) < 64; ++b) {
            Block* bp = FindBlock(b);
            if (bp != nullptr && !bp->deferred && !bp->eager && tracker_->Hidden(b << kBlockBits, 1)) {
                bp->eager = true;
                unhide_pending_.emplace_back(b << kBlockBits, seq);
            }
        }
    }
    // (the kept-back blocks by address: a binding of a whole 1.4 GiB pool would walk 23 thousand blocks)
    std::vector<u64> blocks;
    for (auto it = deferred_set_.lower_bound(addr >> kBlockBits);
         it != deferred_set_.end() && *it <= ((end - 1) >> kBlockBits); ++it) {
        blocks.push_back(*it);
    }
    if (blocks.empty()) {
        return false;
    }
    const auto& f = Fn();
    if (before_upload_ != nullptr) {
        before_upload_(before_upload_ctx_); // (outside rendering)
    }
    // after the earlier writes of the video memory copy (and the shadows), before the later accesses of the host's
    VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cb, &dep);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, diff_pipeline_);
    constexpr u64 kBlock = 1ull << kBlockBits;
    const auto done = [&](u64 b) {
        Block& blk = *FindBlock(b);
        blk.deferred = false;
        --deferred_count_;
        deferred_set_.erase(b);
        if (eager) {
            // a CPU access wanted it: shown once written back, never kept back again
            blk.eager = true;
            unhide_pending_.emplace_back(b << kBlockBits, seq);
        }
        // (else - a GPU reader of the host import: the block stays hidden, kept back again at the next batch end
        // without a protection change; a CPU access meanwhile faults and finds nothing left to write back)
        ++stats_.lazy_flush_blocks;
    };
    const auto shadow_of = [&](u64 a, u64 n) -> const WbShadow* {
        for (const WbShadow& s : wb_shadows_) {
            if (s.valid && s.addr <= a && a + n <= s.addr + s.size) {
                return &s;
            }
        }
        return nullptr;
    };
    // runs of adjacent blocks inside one import window and one shadow: one dispatch each (else block by block)
    for (size_t i = 0; i < blocks.size();) {
        size_t j = i + 1;
        const u64 ra = blocks[i] << kBlockBits, window_end = (ra | (kMaxCopy - 1)) + 1;
        while (j < blocks.size() && blocks[j] == blocks[j - 1] + 1 && ((blocks[j] + 1) << kBlockBits) <= window_end) {
            ++j;
        }
        if (j - i > 1) {
            const u64 n = static_cast<u64>(j - i) << kBlockBits;
            GuestBuffers::View v{};
            GuestBuffers::Failure fl{};
            const WbShadow* sh = shadow_of(ra, n);
            if (sh != nullptr && guest_.Get(ra, n, seq, v, fl) && (v.offset & 3) == 0) {
                DiffPiece(cb, ra, n, *sh, v.buffer, v.offset);
                ++stats_.lazy_flush_runs;
                for (size_t k = i; k < j; ++k) {
                    done(blocks[k]);
                }
                i = j;
                continue;
            }
        }
        const u64 b = blocks[i++];
        const u64 ba = b << kBlockBits;
        GuestBuffers::View v{};
        GuestBuffers::Failure fl{};
        if (guest_.Get(ba, kBlock, seq, v, fl) && (v.offset & 3) == 0) {
            if (const WbShadow* sh = shadow_of(ba, kBlock)) {
                DiffPiece(cb, ba, kBlock, *sh, v.buffer, v.offset);
            } else {
                // (its shadow was made again or dropped since: the whole block - the CPU had no access to it meanwhile)
                const VkBufferCopy c{.srcOffset = ba, .dstOffset = v.offset, .size = kBlock};
                f.vkCmdCopyBuffer(cb, buffer_, v.buffer, 1, &c);
                CopyIntoShadows(ba, kBlock, cb);
                ++stats_.lazy_flush_plain;
            }
        } else if (stats_.first_error.empty()) {
            stats_.first_error = fmt::format("kept-back write-back of {:#x}: no host import", ba);
        }
        done(b);
    }
    ++stats_.lazy_flushes;
    ++stats_.lazy_flush_by[std::min<u32>(why, 3)];
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
    f.vkCmdPipelineBarrier2(cb, &dep);
    if (after_dispatch_ != nullptr) {
        after_dispatch_(after_dispatch_ctx_); // (its pipeline, descriptors and push constants are bound now)
    }
    return true;
}

void BufferCache::CheckWriteBacks(u64 completed) {
    // SSPC_DIAG_WB_DIRTY: the ranges written back by completed batches against the host's bytes before them
    while (!wb_snaps_.empty() && wb_snaps_.front().seq <= completed) {
        WbSnap& s = wb_snaps_.front();
        std::vector<u8> now(s.before.size());
        if (GuestMemory* gm = GetGuestMemory()) {
            gm->CopySparse(s.addr, now.data(), now.size());
            stats_.wb_checked_bytes += now.size();
            for (u64 off = 0; off < now.size(); off += 256) {
                const u64 n = std::min<u64>(256, now.size() - off);
                if (std::memcmp(now.data() + off, s.before.data() + off, n) != 0) {
                    ++stats_.wb_dirty_chunks;
                    for (u64 w = off; w < off + n; w += 4) {
                        const u64 m = std::min<u64>(4, off + n - w);
                        stats_.wb_dirty_words += std::memcmp(now.data() + w, s.before.data() + w, m) != 0 ? 1 : 0;
                    }
                }
            }
        }
        wb_snaps_.pop_front();
    }
}

bool BufferCache::SubmitBinds(VkSemaphore& wait, u64& wait_value) {
    wait = VK_NULL_HANDLE;
    wait_value = 0;
    if (pending_binds_.empty()) {
        return true;
    }
    const auto& f = Fn();
    const VkSparseBufferMemoryBindInfo bb{.buffer = buffer_,
                                          .bindCount = static_cast<u32>(pending_binds_.size()),
                                          .pBinds = pending_binds_.data()};
    const u64 value = bind_value_ + 1;
    const VkTimelineSemaphoreSubmitInfo tsi{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .signalSemaphoreValueCount = 1,
                                            .pSignalSemaphoreValues = &value};
    const VkBindSparseInfo bi{.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                              .pNext = &tsi,
                              .bufferBindCount = 1,
                              .pBufferBinds = &bb,
                              .signalSemaphoreCount = 1,
                              .pSignalSemaphores = &bind_timeline_};
    const VkResult r = f.vkQueueBindSparse(ctx_.Queue(), 1, &bi, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        // the blocks are not bound: back to unbound (a later range binds them again or takes the host import) - the
        // batch's commands that use them read zeros / drop writes (review MEDIUM 4; an error is reported below)
        for (const VkSparseMemoryBind& bind : pending_binds_) {
            if (Block* blk = FindBlock(bind.resourceOffset >> kBlockBits)) {
                blk->bound = false;
                blk->valid = 0;
                ++blk->version;
                stats_.resident_bytes -= std::min<u64>(stats_.resident_bytes, bind.size);
            }
        }
    }
    pending_binds_.clear();
    ++stats_.bind_calls;
    if (r != VK_SUCCESS) {
        if (stats_.first_error.empty()) {
            stats_.first_error = fmt::format("vkQueueBindSparse failed ({})", static_cast<int>(r));
        }
        SSPC_LOG(GPU, Error, "native backend: vkQueueBindSparse failed ({})", static_cast<int>(r));
        return false;
    }
    bind_value_ = value;
    if (host_wait_binds_) {
        // the batch is submitted after the binds completed: ordered without a queue wait
        const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                     .semaphoreCount = 1,
                                     .pSemaphores = &bind_timeline_,
                                     .pValues = &value};
        f.vkWaitSemaphores(ctx_.Device(), &wi, 5'000'000'000ull);
        return true;
    }
    wait = bind_timeline_;
    wait_value = value;
    return true;
}

} // namespace SecondSon::Gpu::Vk
