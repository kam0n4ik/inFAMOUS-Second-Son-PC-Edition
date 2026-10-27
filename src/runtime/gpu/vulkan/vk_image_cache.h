// SecondSonPC — the backend's images (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §C): guest images as
// Vulkan images in video memory, keyed by their layout (image_layout.h), uploaded from the guest's memory (linear:
// copied from the imported guest buffer; tiled: detiled by the tiling kernels into scratch, then copied), render and
// depth targets (rendered by the GPU, never uploaded over), views, samplers, layout/access tracking for barriers,
// invalidation when the guest writes the memory (the memory listener) — a texture is uploaded again at its next use;
// a GPU-written image stays (its memory in guest RAM is stale until written back, which this sub-stage does not do).
//
// One thread (the backend's GPU thread) uses it; Invalidate may come from any thread (queued under a mutex).
// Nothing is substituted: an image that cannot be made (unsupported layout/format, no memory) is reported and the
// caller binds a null descriptor and counts the draw as incomplete.
#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_formats.h"
#include "runtime/gpu/u64_hash.h"

namespace SecondSon::Gpu {
class PageTracker;
}

namespace SecondSon::Gpu::Vk {

class Diagnostics;
class BufferCache;

/// [Graphics] anisotropy: 0 = the game's own (each S#'s ratio), else 2 / 4 / 8 / 16 for the S#s the game samples
/// anisotropically (AnisoLinear minification, a ratio above 1x, normalized, not a depth comparison). Any thread; the
/// samplers made from then on (ImageCache::Sampler keys the level, so the game's own ones stay cached).
void SetAnisotropyOverride(u32 level);
u32 AnisotropyOverride();

class DeviceContext;
class GuestBuffers;

struct CachedImage {
    u32 id = 0;
    ImageLayout layout;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags create_flags = 0;
    u32 alias_hits = 0; ///< textures over another layout that the equal-address rule gave this image
    u64 bytes = 0;
    // state for barriers (whole image)
    VkImageLayout current = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2 access = 0;
    bool uploaded = false;    ///< its guest memory was copied in (textures)
    bool gpu_written = false; ///< rendered/stored by the GPU: guest memory is no longer its source
    bool dirty = false;       ///< the guest wrote its memory since the upload
    /// A GPU-written image whose memory was written since by someone else - the CPU (tracker), DMA, a shader's buffer
    /// write at its base: its texels are memory's again; it is uploaded before its next use (P4.5).
    bool memory_newer = false;
    /// Another GPU-written image wrote this one's memory since: retired once unused for a while (P4.5).
    bool superseded = false;
    /// The CPU wrote a 64 KiB block it shares with other memory (or it has no whole block): maybe into it, maybe beside
    /// it. Its texels are kept (a neighbour's writes are common), but it is no source to expand from (P4.5).
    bool maybe_newer = false;
    /// Its pages are write-protected since it was last written by the GPU (the CPU's writes there are seen): only then
    /// may a write-back go over its memory (P4.5 review M1b)
    bool write_tracked = false;
    u64 last_seq = 0;
    u64 last_frame = 0; ///< ImageCache::NewFrame count at its last use
    // R11 neon (SSPC_R11_AQ_IMAGES, docs/R11_NEON.md): the last async compute lane batch that sampled it, and the batch
    // its last barrier was recorded in (0: none since the switch is on) and whether that one was a lane batch
    u64 lane_use = 0;
    u64 barrier_value = 0;
    bool barrier_lane = false;
    std::unordered_map<u64, VkImageView, U64Hash> views;
};

struct ImageCacheStats {
    u64 images = 0, bytes = 0, created = 0, uploads = 0, detiles = 0, linear_uploads = 0, upload_bytes = 0;
    u64 hits = 0, invalidations = 0, reuploads = 0, unsupported = 0, import_failures = 0, alias_hits = 0;
    u64 views = 0, samplers = 0, barriers = 0;
    u64 overlap_hits = 0;   ///< the bridge's equal-address rule: another image's texels through a view
    u64 target_misses = 0;  ///< a texture over a GPU-written image none of the rules accepted (uploaded instead)
    u64 overlap_misses = 0; ///< a new texture whose memory overlaps a GPU-written image (any overlap; uploaded)
    u64 evicted = 0, evicted_bytes = 0; ///< uploaded images freed over the budget (re-uploaded on their next use)
    // P4.5
    u64 memory_newer = 0;            ///< GPU-written images whose memory was written by someone else since
    u64 memory_newer_by[4] = {};     ///< by source: exact write, a shader's buffer write at its base, CPU (tracker), unmap
    u64 target_uploads = 0;          ///< targets uploaded before rendering (their memory was newer)
    u64 expanded = 0, block_copies = 0; ///< ExpandImage: more levels/layers; a block-compressed view of uncompressed texels
    u64 retired = 0, retired_bytes = 0; ///< images taken out (unmapped, superseded, expanded) and destroyed when unused
    u64 superseded = 0;
    u64 gpu_tracked = 0, gpu_untracked = 0; ///< GPU-written images whose pages could / could not be write-protected
    u64 stencil_views = 0; ///< textures of a depth target's stencil plane: the depth image's stencil aspect
    /// GPU-written images over the budget: dropped (memory newer), written back and freed, kept (no write-back path)
    u64 gpu_dropped = 0, gpu_written_back = 0, gpu_written_back_bytes = 0, gpu_kept_no_path = 0;
    std::string first_unsupported;
};

/// A GPU-written image over the budget (P4.5; the bridge's GarbageCollectImages): what becomes of it.
enum class GpuEviction : u8 {
    Keep,      ///< used lately, in flight, maybe newer in memory, or no write-back path (see no_write_path)
    Drop,      ///< memory holds newer texels (the CPU/DMA or another image wrote it since): freed, nothing written
    WriteBack, ///< its texels copied (tiled: through the tiler) to its guest memory, then freed (re-uploaded at its
               ///< next use)
};
struct GpuEvictionCandidate {
    bool in_flight = false;    ///< its last use has not completed
    bool memory_newer = false; ///< CachedImage::memory_newer
    bool superseded = false;   ///< CachedImage::superseded
    bool maybe_newer = false;  ///< CachedImage::maybe_newer (a write-back could put old texels over the CPU's bytes)
    /// No copy back to its memory: MSAA, a volume, block-compressed, depth with stencil or not D32/D16, a tiling
    /// the tiler has no variant for (the bridge keeps every tiled one: "can't handle non-linear image downloads")
    bool no_write_path = false;
    bool untracked = false;    ///< the CPU's writes to its memory are not seen (CachedImage::write_tracked false)
    /// Another live GPU-written image over its memory was used later: that one holds the memory's latest texels
    bool newer_overlap = false;
    /// Another live GPU-written image over its memory, used earlier: a write-back would mark it memory-newer
    bool older_overlap = false;
    u64 age_frames = 0;        ///< presented frames since its last use
};
inline constexpr u64 kGpuEvictStaleFrames = 120; ///< a GPU-written image whose memory is newer: dropped this old
inline constexpr u64 kGpuEvictCleanFrames = 300; ///< a clean one (its texels exist only in video memory): written back
GpuEviction ClassifyGpuEviction(const GpuEvictionCandidate& c);

/// Who wrote guest memory behind the images (P4.5): a DMA/WRITE_DATA or an exactly reported guest write; a shader's
/// buffer write; the write tracker's report (whole 64 KiB blocks); an unmap/remap.
enum class InvalKind : u8 { Exact, BufferWrite, Coarse, Unmap };
/// What a write of [addr, end) does to an image over [begin, begin + size) (ImageCache::ApplyInvalidations' rule).
struct InvalidationEffect {
    bool retire = false;       ///< unmapped: taken out
    bool dirty = false;        ///< an uploaded texture: uploaded again at its next use
    bool memory_newer = false; ///< a GPU-written image: memory holds its texels now (surely written into it)
    bool maybe_newer = false;  ///< a GPU-written image sharing a reported block: maybe written, maybe its neighbour
};
InvalidationEffect ClassifyInvalidation(InvalKind kind, u64 begin, u64 size, bool gpu_written, bool uploaded, u64 addr,
                                        u64 end);
/// An image with layout `have` at the same address serves a request for `want` through a view (the same texels: size,
/// tiling, samples; a format of the same class; at least the levels/layers asked for) - the equal-address rule.
bool ImageServes(const ImageLayout& have, const ImageLayout& want);
/// `have` (an image the GPU wrote) can be copied into a new image for `want` (the bridge's ExpandImage: more levels or
/// layers of the same texels, or a block-compressed view of its uncompressed texels).
bool CanExpandFrom(const CachedImage& have, const ImageLayout& want);
/// `have` (an image the GPU wrote, at the same address) serves `want` and holds more layers or levels than `same`, the
/// image of `want`'s own layout (null: none) - the bridge's subresource rule: a slice of an array rendered as a target
/// of its own goes into the array, not into an image of its own that would split the array's texels.
bool ServesBetter(const CachedImage& have, const CachedImage* same, const ImageLayout& want);

class ImageCache {
public:
    ImageCache(DeviceContext& ctx, GuestBuffers& guest);
    ~ImageCache(); ///< destroys everything (the device must be idle)

    enum class Use : u8 { Texture, Storage, ColorTarget, DepthTarget, TransferSrc };

    /// The image for `layout` (created when new). Textures are uploaded (again, when dirty) by commands recorded into
    /// `cb` for submission `seq`; targets are not. nullptr with `error` set: unsupported or out of memory.
    CachedImage* Find(const ImageLayout& layout, Use use, VkCommandBuffer cb, u64 seq, std::string& error);
    /// A view of `image` for `view` (created once).
    VkImageView View(CachedImage& image, const ImageViewDesc& view, bool storage);
    /// Records a barrier bringing the whole image to `layout` for (`stage`, `access`); none when already there and both
    /// accesses are reads. true when a barrier was recorded.
    bool Transition(VkCommandBuffer cb, CachedImage& image, VkImageLayout layout, VkPipelineStageFlags2 stage,
                    VkAccessFlags2 access);

    /// A sampler for an S# (cached by its bits). VK_NULL_HANDLE: unsupported fields (counted).
    /// `extra_lod_bias`: added to the S#'s (upscaling: the scene's textures sampled for the output resolution).
    VkSampler Sampler(const Gpu::Shader::SamplerDesc& sharp, bool is_depth, u64 border_color_base,
                      float extra_lod_bias = 0.0f);

    /// [addr, addr + size) was written by someone other than the GPU's rendering (a DMA/WRITE_DATA, a file read, the
    /// guest's own memory writes reported exactly): textures there are uploaded again at their next use, GPU-written
    /// images overlapping it too (memory holds their texels now). Any thread.
    void Invalidate(u64 addr, u64 size);
    /// A shader's buffer write of [addr, addr + size): uploaded textures there again; a GPU-written image only when it
    /// starts at `addr` (the bridge's InvalidateMemoryFromGPU: a written pool overlapping a target is not a write of
    /// it). Any thread.
    void InvalidateFromBufferWrite(u64 addr, u64 size);
    /// The write tracker's report (whole 64 KiB blocks the CPU wrote some page of): textures there again; a GPU-written
    /// image only when a reported block lies inside it (a block shared with its neighbour says nothing). Any thread.
    void InvalidateCoarse(u64 addr, u64 size);
    /// [addr, addr + size) was unmapped or mapped again: the images there are taken out (destroyed once their last use
    /// completed). Any thread.
    void Unmap(u64 addr, u64 size);
    /// The executor reused an image it resolved before (target cache, texture memo): what Find records.
    void Touch(CachedImage& image, u64 seq) {
        image.last_seq = seq;
        image.last_frame = frame_;
    }
    /// A presented frame ended (the age of images in frames).
    void NewFrame() { ++frame_; }
    /// The invalidations queued so far applied now (Find applies them too; a caller that skips Find for a texture it
    /// resolved before calls this first, then checks the image's dirty flag).
    void ApplyPendingInvalidations() { ApplyInvalidations(); }
    /// Video memory the images may keep: over it, uploaded images (never the GPU-written ones - their texels exist only
    /// in video memory) whose last use completed are freed, least recently used first, down to 90% of it.
    void SetBudget(u64 bytes) { budget_ = bytes; }
    u64 Budget() const { return budget_; }
    /// A lower limit for the uploaded images only (the OS's video memory budget, SSPC_VRAM_ADAPT): Evict frees them
    /// down to it, but OverBudget (the GPU-written images' write-back, the executor's eviction drains) keeps budget_.
    void SetSoftBudget(u64 bytes) { soft_budget_ = bytes; }
    u64 SoftBudget() const { return std::min(budget_, soft_budget_); }
    /// The GPU-written images' bytes: what a lower budget cannot free by evicting uploaded images.
    u64 GpuWrittenBytes() const {
        u64 n = 0;
        for (const auto& img : images_) {
            n += img->gpu_written ? img->bytes : 0;
        }
        return n;
    }
    /// Called before an upload records its commands (the executor ends rendering and makes earlier writes visible).
    void SetBeforeUpload(void (*hook)(void* ctx), void* ctx) {
        before_upload_ = hook;
        before_upload_ctx_ = ctx;
    }
    /// Uploads read video memory where the buffer cache holds the range (the GPU may have written it newer).
    void SetBufferCache(BufferCache* buffers) { buffers_ = buffers; }
    /// Object names, the aliasing log, upload markers (--gpu-native-diag).
    void SetDiagnostics(Diagnostics* diag) { diag_ = diag; }
    /// Write tracking: a texture's guest pages are armed when it is uploaded (P4.4).
    void SetTracker(PageTracker* tracker) { tracker_ = tracker; }
    /// Releases what completed submissions no longer need (temporary detile buffers).
    void Collect(u64 completed);
    /// Still over the budget once the uploaded images went (Collect): GPU-written images by ClassifyGpuEviction,
    /// least recently used first, at most `max` per call. Drop: retired here. WriteBack: returned - the caller records
    /// the copy of each to its guest memory (WriteBackAndRetire; P4.5).
    std::vector<CachedImage*> EvictGpuWritten(u64 completed, u32 max);
    /// Records into `cb` (outside rendering) the copy of `img`'s texels to its guest memory at (`dst`, `dst_offset`)
    /// (guest_size bytes; tiled: through a linear temporary and the tiler), then retires it (destroyed once `seq`
    /// completed). false with `error`: nothing recorded, the image stays.
    bool WriteBackAndRetire(CachedImage* img, VkCommandBuffer cb, u64 seq, VkBuffer dst, u64 dst_offset,
                            std::string& error);
    bool OverBudget() const { return stats_.bytes > budget_ || force_gpu_evict_; }

    /// The target whose CMASK/FMASK/HTILE metadata starts at `address` (registered when bound as a target), nullptr: none.
    CachedImage* MetaOwner(u64 address) const {
        const auto it = meta_owner_.find(address);
        return it != meta_owner_.end() ? it->second : nullptr;
    }
    /// The image currently at `address` written by the GPU (VideoOut buffers are render targets), nullptr: none.
    CachedImage* FindTargetAt(u64 address);

    ImageCacheStats Stats() const { return stats_; }

private:
    struct Temp {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u64 seq = 0;
    };
    CachedImage* Create(const ImageLayout& layout, std::string& error);
    bool Upload(CachedImage& image, VkCommandBuffer cb, u64 seq, std::string& error);
    bool DetilePipeline(const TilingParams& p, VkPipeline& out, std::string& error);
    void ApplyInvalidations();
    void Queue(u64 addr, u64 size, InvalKind kind);
    /// Takes `img` out of the lookups (its views stay valid until destroyed with it once `last_seq` completed).
    void Retire(CachedImage* img);
    /// Records into `cb` the copy of `src`'s levels/layers both have into `dst` (`dst` in TRANSFER_DST afterwards).
    void CopyImage(CachedImage& src, CachedImage& dst, VkCommandBuffer cb);
    /// The GPU-written image at `layout`'s address a texture with more levels/layers, or a block-compressed view of its
    /// uncompressed texels, can be made from by a copy (nullptr: none).
    CachedImage* ExpandSource(const ImageLayout& layout, std::multimap<u64, CachedImage*>::iterator lo,
                              std::multimap<u64, CachedImage*>::iterator hi);
    /// Write tracking of a GPU-written image (its pages protected: the CPU's writes there are seen).
    void TrackGpuWritten(CachedImage& img);
    /// No way to copy `l`'s texels back to its memory (ClassifyGpuEviction's no_write_path).
    static bool NoWritePath(const ImageLayout& l, VkImageAspectFlags aspect, VkFormat format);
    /// A device-local temporary of `bytes` (destroyed once `seq` completed). VK_NULL_HANDLE: no memory.
    VkBuffer TempBuffer(u64 bytes, u64 seq);
    std::vector<std::unique_ptr<CachedImage>> retired_; ///< taken out, destroyed once their last use completed
    u64 frame_ = 0, last_retire_scan_ = ~0ull;

    DeviceContext& ctx_;
    PageTracker* tracker_ = nullptr;
    BufferCache* buffers_ = nullptr;
    std::unordered_map<u64, CachedImage*> meta_owner_; ///< CMASK/FMASK/HTILE address -> its target
    /// A depth target's stencil plane address -> the depth image (the GCN stencil is a separate surface; the Vulkan
    /// image holds both aspects: a texture of that address is the image's stencil aspect; P4.5)
    std::unordered_map<u64, CachedImage*> stencil_owner_;
    u64 overlap_misses_logged_ = 0;
    u64 dup_logged_ = 0; ///< second images of an address logged (P4.5 diagnostics)
    Diagnostics* diag_ = nullptr;
    bool force_gpu_evict_ = false; ///< SSPC_DIAG_GPU_EVICT_ALL: the write-back stress test
    u64 force_age_scale_ = 1;      ///< ... =<frames>: ages counted that much faster
    bool alias_rule_ = true; ///< SSPC_DIAG_ALIAS_RULE=off: a diagnostic control run without the equal-address rule
    void (*before_upload_)(void*) = nullptr;
    void* before_upload_ctx_ = nullptr;
    GuestBuffers& guest_;
    std::vector<std::unique_ptr<CachedImage>> images_;
    std::multimap<u64, CachedImage*> by_address_;
    u64 max_image_size_ = 0; ///< the largest image's guest size: how far before a range an overlapping image may start
    u64 budget_ = ~0ull;
    u64 soft_budget_ = ~0ull;
    u64 soft_evict_frame_ = ~0ull; ///< the frame of the soft limit's last eviction step
    void Evict(u64 completed);
    u64 epoch_ = 0;

    u64 gen_ = 0; ///< Generation's own part: barriers, uploads, applied invalidations, Find's changes of an image's flags

public:
    /// Bumped whenever the set of images changes (a new image; an eviction must bump it too): what Find returned for
    /// the same layout before is still what it returns while the epoch is the same (the executor's target cache).
    u64 Epoch() const { return epoch_; }
    /// Changes whenever any image's state may have: its layout and access (a barrier), texels (an upload), flags (an
    /// invalidation applied, a target/storage use, an upload), the set of images (the epoch), the frame (Touch's age).
    /// While it is the same, an image checked before is as it was (SSPC_TEX_SEGMENT: the executor's checks of a texture
    /// once a segment). A sum of counters that only grow: it changes when any of them does.
    u64 Generation() const { return gen_ + epoch_ + frame_; }
    u64 Frame() const { return frame_; }
    /// A Transition of `img` to its current layout for (`stage`, `access`) would record nothing and change nothing: it
    /// is read there without a write since its barrier, by those stages already.
    static bool ReadSince(const CachedImage& img, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    /// R11 (SSPC_R11_AQ_IMAGES): the batch commands are recorded into now (its value, on the lane or not) - kept in the
    /// images' barrier_value; a barrier on a main batch over an image a lane batch sampled makes the main batch wait for
    /// it (TakeLaneWait: the highest such lane value since the last call).
    void SetRecording(u64 value, bool lane) {
        rec_value_ = value;
        rec_lane_ = lane;
    }
    u64 TakeLaneWait() { return std::exchange(lane_wait_, 0); }

private:
    u64 rec_value_ = 0, lane_wait_ = 0;
    bool rec_lane_ = false;
    u32 next_id_ = 1;
    std::unordered_map<u64, VkSampler, U64Hash> samplers_;
    std::deque<Temp> temps_;
    // detiler
    VkDescriptorSetLayout detile_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout detile_layout_ = VK_NULL_HANDLE;
    std::unordered_map<u64, VkPipeline> detile_pipelines_;
    // invalidations from other threads
    std::mutex inval_mutex_;
    std::atomic<bool> has_inval_{false};
    struct PendingInval {
        u64 addr, size;
        InvalKind kind;
    };
    std::vector<PendingInval> pending_inval_;
    ImageCacheStats stats_;
};

} // namespace SecondSon::Gpu::Vk
