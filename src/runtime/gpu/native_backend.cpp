// SecondSonPC — SecondSonPC's own GPU backend (P4.4). See native_backend.h.
#include <windows.h>

#include "runtime/gpu/native_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/gnm/gnm_driver.h"
#include "runtime/gpu/gnm/gnm_host.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/cpu_sampler.h"
#include "runtime/gpu/page_tracker.h"
#include "runtime/gpu/parallel_cp.h"
#include "runtime/gpu/shader_trace.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/kernel/clock.h"
#include "runtime/gpu/shader/game_shader_index.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/gpu/vulkan/vk_precompile.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/vulkan/vk_resources.h"

namespace SecondSon::Gpu::Native {

namespace {

namespace GS = Gpu::Shader;

/// The command processor's view of memory: guest memory (40 bits) and VideoOut's labels (host memory).
class Memory final : public Cp::CommandMemory {
public:
    PageTracker* tracker = nullptr;
    Vk::BufferCache* buffers = nullptr; ///< its video memory copy of what the processor writes is invalidated
    Vk::Executor* executor = nullptr; ///< its writes on the GPU's timeline land before a read of them
    /// P5.2: the processor's writes reach the shader frontend's code trust (set when the frontend runs on the
    /// processor's thread)
    GS::ShaderFrontend* frontend = nullptr;
    /// P4.10 (SSPC_P410_PARALLEL_CP=1): the recording thread executes draws behind the processor - what this memory
    /// does to the executor goes through it (drained first, or handed on in stream order)
    ParallelCp* parallel = nullptr;
    void CachedWrite(u64 addr, u64 bytes) {
        if (buffers == nullptr) {
            return;
        }
        if (parallel != nullptr) {
            parallel->DeferInvalidateIfCached(addr, bytes); // (BufferCache::InvalidateIfCached: the recording thread's)
        } else {
            buffers->InvalidateIfCached(addr, bytes);
        }
    }
    bool Read(u64 addr, void* dst, u64 bytes) override {
        if (VideoOut::LabelRead(addr, dst, bytes)) {
            return true;
        }
        if (parallel != nullptr) {
            // (the executor's list is the recording thread's while records are queued: the queued copies and what it
            // held when the queue was last empty decide; a possible overlap drains, then the executor decides)
            if (parallel->TimelinePending(addr, bytes)) {
                parallel->Drain(DrainReason::MemoryRead);
                if (executor->TimelineWritePending(addr, bytes)) {
                    executor->SyncFromMemoryRead();
                }
            }
        } else if (executor != nullptr && executor->TimelineWritePending(addr, bytes)) {
            executor->SyncFromMemoryRead();
        }
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        gm->CopySparse(addr, static_cast<u8*>(dst), bytes);
        return true;
    }
    bool unordered_labels = true; ///< P4.6 (SSPC_P46_SYNC=0: off)
    bool Unordered(u64 addr, u64 bytes) override {
        return unordered_labels && VideoOut::IsLabel(addr, bytes);
    }
    bool Write(u64 addr, const void* src, u64 bytes) override {
        if (parallel != nullptr && parallel->FrontendThread()) {
            parallel->FrontGpuWrote(addr, bytes); // (P5.3D: to the frontend's thread, in stream order)
        } else if (frontend != nullptr) {
            frontend->GpuWrote(addr, bytes); // (P5.2)
        }
        if (parallel != nullptr) {
            parallel->MemoryChanged(); // (with the next record: where the write is in the stream)
        } else if (executor != nullptr) {
            executor->GuestMemoryChanged(); // (P4.7: no memoized stage preparation past the processor's write)
        }
        if (VideoOut::LabelWrite(addr, src, bytes)) {
            return true;
        }
        if (parallel != nullptr) {
            // P4.11 (review): a queued timeline copy to this memory - its CPU fallback on the recording thread would
            // land after this later write - executes first (the GPU-recorded copy as with one thread)
            if (parallel->CopyQueuedOver(addr, bytes)) {
                parallel->Drain(DrainReason::MemoryWrite);
            }
            // and the memory changes again once written: the recording thread's streamed copies (memoized by the
            // generation) taken between the change above and the write hold the older bytes
            const bool ok = WriteGuest(addr, src, bytes);
            parallel->MemoryChanged();
            return ok;
        }
        return WriteGuest(addr, src, bytes);
    }
    bool WriteGuest(u64 addr, const void* src, u64 bytes) {
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        // P4.6 night sprint: the backing's host view of runs written before (the compute rings' read pointers are
        // written after every packet; TryWriteBacking copies the VMAs with their maps each call - 7% of the GPU
        // thread on the beach). A run is cached only after TryWriteBacking wrote there (the same memory it writes);
        // a map or unmap anywhere drops them all (the generation).
        const u64 gen = map_gen.load(std::memory_order_acquire);
        for (const Run& r : runs) {
            if (r.host != nullptr && r.gen == gen && addr >= r.begin && addr + bytes <= r.end) {
                std::memcpy(r.host + (addr - r.begin), src, bytes);
                CachedWrite(addr, bytes);
                return true;
            }
        }
        if (gm->TryWriteBacking(reinterpret_cast<void*>(addr), src, bytes)) {
            CachedWrite(addr, bytes); // written past the tracker: the video memory copy is older
            u8* host = nullptr;
            u64 rb = 0, rs = 0;
            if (gm->BackingRun(addr, &host, &rb, &rs) && host != nullptr && addr >= rb && addr + bytes <= rb + rs) {
                runs[next_run++ % runs.size()] = {rb, rb + rs, host, gen};
            }
            return true;
        }
        // memory without physical backing (flexible, anonymous): written in place when the pages are writable (a
        // texture's write-protected pages given back first: its copy is stale after this write)
        if (tracker != nullptr) {
            if (parallel != nullptr) {
                parallel->Drain(DrainReason::MemoryWrite); // (the tracker's arming is the recording thread's)
            }
            tracker->Disarm(addr, bytes);
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 ||
            reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize < addr + bytes) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(addr), src, bytes);
        CachedWrite(addr, bytes);
        return true;
    }
    struct Run {
        u64 begin = 0, end = 0;
        u8* host = nullptr;
        u64 gen = 0;
    };
    std::array<Run, 4> runs{};
    u32 next_run = 0;
    std::atomic<u64> map_gen{1}; ///< bumped by every guest map/unmap (the listener's threads)
};

struct Submission {
    enum class Kind : u8 { Graphics, MapQueue, UnmapQueue, DingDong, Done } kind = Kind::Graphics;
    u64 addr = 0;
    u32 dwords = 0;
    const u32* data = nullptr;
    u32 vqid = 0, pipe = 0, ring_dwords = 0;
    u64 read_ptr = 0;
};

class Backend final : public Gnm::Host, public MemoryListener, public VideoOut::Sink {
public:
    // ---- Gnm::Host (the game's threads)
    bool SubmitGraphics(u64 addr, u32 dwords, const u32* data) override {
        FrameLog::GameSubmit();
        Push({.kind = Submission::Kind::Graphics, .addr = addr, .dwords = dwords, .data = data});
        return true;
    }
    bool MapComputeQueue(u32 vqid, u32 pipe, u64 ring_addr, u32 ring_dwords, u64 read_ptr_addr) override {
        Push({.kind = Submission::Kind::MapQueue, .addr = ring_addr, .vqid = vqid, .pipe = pipe,
              .ring_dwords = ring_dwords, .read_ptr = read_ptr_addr});
        return true;
    }
    void UnmapComputeQueue(u32 vqid) override {
        Push({.kind = Submission::Kind::UnmapQueue, .vqid = vqid});
    }
    bool DingDong(u32 vqid, u32 next_offset_dw) override {
        Push({.kind = Submission::Kind::DingDong, .dwords = next_offset_dw, .vqid = vqid});
        return true;
    }
    void SubmitDone() override {
        Push({.kind = Submission::Kind::Done});
    }
    bool IsGpuIdle() override {
        return idle_.load(std::memory_order_acquire);
    }
    bool IsQuiet() override {
        return quiet_.load(std::memory_order_acquire);
    }
    bool IsRecorded() override {
        return recorded_.load(std::memory_order_acquire);
    }
    s32 SubmitEopFlip(s32 handle, u32 index, u32 mode, s64 arg) override {
        return VideoOut::SubmitEopFlip(handle, index, mode, arg);
    }
    bool BufferLabelAddress(s32 handle, u64& address) override {
        return VideoOut::BufferLabelAddress(handle, address);
    }

    // ---- MemoryListener (the guest memory manager's threads)
    void OnUnmap(u64 addr, u64 size) override {
        Inspector::GuestMap(true);
        memory_.map_gen.fetch_add(1, std::memory_order_acq_rel);
        tracker_.Forget(addr, size);
        if (guest_) {
            guest_->Drop(addr, size);
        }
        if (images_) {
            images_->Unmap(addr, size); // the images there are taken out (P4.5)
        }
        Invalidate(addr, size);
    }
    void OnRegister(u64, u64) override {}
    void OnMap(u64 addr, u64 size) override {
        Inspector::GuestMap(false);
        memory_.map_gen.fetch_add(1, std::memory_order_acq_rel);
        tracker_.Forget(addr, size);
        if (images_) {
            images_->Unmap(addr, size); // new memory there: what the images held is gone (P4.5)
        }
        Invalidate(addr, size);
    }
    void OnGuestWrite(u64 addr, u64 size) override {
        Invalidate(addr, size);
    }

    // ---- VideoOut::Sink (the present thread)
    bool Ready(u64 value) override {
        return value == 0 || (timeline_ && timeline_->Completed() >= value);
    }
    void Present(const VideoOut::FlipRequest& r) override {
        if (presenter_) {
            presenter_->Present(r.frame, r.flip_id);
        }
    }

    bool Start(const Options& o, std::string& error);
    void Stop();
    std::string Report();

private:
    void Push(Submission s) {
        {
            std::scoped_lock lock{mutex_};
            queue_.push_back(s);
            idle_.store(false, std::memory_order_release);
            quiet_.store(false, std::memory_order_release);
            recorded_.store(false, std::memory_order_release);
        }
        cv_.notify_one();
    }
    void Invalidate(u64 addr, u64 size) {
        if (images_) {
            images_->Invalidate(addr, size);
        }
        if (buffers_) {
            buffers_->Invalidate(addr, size);
        }
        std::scoped_lock lock{inval_mutex_};
        pending_inval_.emplace_back(addr, size);
    }
    void Loop();
    void Apply(const Submission& s);
    /// The guest's CPU writes the tracker caught so far applied to the caches (textures and buffers uploaded again at
    /// their next use): before the work the guest submitted after them, and at each met WAIT_REG_MEM on memory (data a
    /// CPU thread published with a flag; P4.5). true: there were some. GPU thread.
    bool DrainCpuWrites();
    /// The guest's CPU wrote [addr, addr + size): the frontend's programs there are read again - on the frontend's
    /// own thread in stream order when it has one (P5.3D), else at once (it runs on this thread).
    void FrontendInvalidate(u64 addr, u64 size) {
        if (parallel_ && parallel_->FrontendThread()) {
            parallel_->FrontInvalidate(addr, size);
        } else {
            frontend_->Invalidate(addr, size);
        }
    }
    /// The executor reached a flip marker (its batch submitted, the flip queued): the frame log's row (P4.6).
    void OnFlip(u64 gpu_value);
    /// P4.10.4: a queued timeline copy the executor refused, done as the processor would (after Synchronize): the CPU
    /// copies guest memory and the caches drop the destination. The recording thread. true: copied.
    bool CpuCopy(u64 src, u64 dst, u64 bytes);
    /// P4.14: the persistent shader cache, the driver's pipeline cache and the start-up precompiler (Start).
    void StartShaderCache(const Options& o);
    void RunPrecompile(const Options& o);
    /// The disk cache's thread every 10 s: the pipeline cache saved when pipelines were added and none for 10 s.
    void MaybeSavePipelineCache();
    std::string CacheReport();

public:
    ShaderCacheInfo CacheInfo();

private:
    std::vector<u8> copy_scratch_; ///< CpuCopy's (the recording thread's)

    Options options_;
    std::unique_ptr<Vk::DeviceContext> ctx_;
    std::unique_ptr<Vk::Diagnostics> diag_; ///< destroyed after everything recording into the device
    std::unique_ptr<Vk::Timeline> timeline_;
    std::unique_ptr<Vk::StreamRing> ring_;
    std::unique_ptr<Vk::StreamRing> shadow_ring_; ///< dense video memory dispatches write (Executor::PrepareShadows)
    std::unique_ptr<Vk::GuestBuffers> guest_;
    std::unique_ptr<Vk::BufferCache> buffers_; ///< the guest's buffers in video memory
    std::unique_ptr<Vk::ImageCache> images_;
    // P4.14 (before the factory: destroyed after it and its precompiler)
    std::unique_ptr<GS::ShaderDiskCache> disk_;
    std::unique_ptr<Vk::PipelineCacheFile> pcache_;
    GS::GameShaderIndex game_index_; ///< the game's programs (P4.14 offline index; empty without it)
    u64 cached_in_game_ = 0;
    std::atomic<u64> pc_saved_count_{0}, pc_prev_count_{0};
    std::chrono::steady_clock::time_point pc_saved_at_{};
    std::unique_ptr<Vk::PipelineFactory> pipelines_;
    std::unique_ptr<Vk::Presenter> presenter_;
    std::unique_ptr<GS::ShaderFrontend> frontend_;
    std::unique_ptr<Vk::Executor> executor_;
    std::unique_ptr<Cp::CommandProcessor> cp_;
    std::unique_ptr<ParallelCp> parallel_; ///< P4.10 SSPC_P410_PARALLEL_CP=1 (destroyed before the processor)
    Vk::OwnedBuffer gds_;
    GS::DeviceProfile profile_;
    Memory memory_;
    PageTracker tracker_;
    std::vector<u64> fault_addrs_; ///< DrainCpuWrites' scratch

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Submission> queue_;
    std::atomic<bool> idle_{true};
    std::atomic<bool> quiet_{true};    ///< IsQuiet (diagnostics)
    std::atomic<bool> recorded_{true}; ///< IsRecorded (diagnostics)
    std::atomic<bool> stop_{false};
    std::atomic<VkResult> failure_result_{VK_SUCCESS};
    std::thread thread_;
    CpuSampler game_sampler_; ///< SSPC_DIAG_SAMPLE_GAME=<name part>: the game's threads (gpu_cpu_profile_game.txt)
    CpuSampler sampler_; ///< SSPC_DIAG_SAMPLE=1: where the GPU thread's CPU time goes (logs\gpu_cpu_profile.txt)
    CpuSampler raw_sampler_; ///< P5.2 SSPC_DIAG_SAMPLE_RAW=1: every thread, raw (logs\cpu_samples_raw.txt)
    bool submit_done_ = false;
    std::array<bool, Cp::kNumQueues> mapped_{};
    std::mutex inval_mutex_;
    std::vector<std::pair<u64, u64>> pending_inval_;
    u64 submissions_ = 0, cp_errors_ = 0;
    std::string first_cp_error_;
    // the GPU thread's running totals for the frame log (P4.6): busy (rounds, polls, flushes); idle by what it waited for
    u64 tot_busy_ns_ = 0, tot_blocked_label_ns_ = 0, tot_blocked_other_ns_ = 0, tot_empty_ns_ = 0;
    // P4.8 (the Performance Inspector): the loop's time split further - before a round (submissions, invalidations,
    // the CPU's writes drained), rounds, polls, flushes; of the idle time, the part the GPU had work
    u64 tot_pre_ns_ = 0, tot_round_ns_ = 0, tot_poll_ns_ = 0, tot_flush_ns_ = 0, tot_idle_gpu_ns_ = 0;
    std::chrono::steady_clock::time_point round_t0_{}; ///< the current round's start (OnFlip runs inside it)
    u64 label_base_ = 0;                               ///< VideoOut's buffer labels (a wait on them waits for a flip)
};

bool Backend::Start(const Options& o, std::string& error) {
    options_ = o;
    Vk::ContextOptions co;
    co.device_index = o.device_index;
    co.validation = o.validation;
    co.presentation = true;
    co.diagnostics = o.diagnostics;
    co.validation_log = o.logs_dir / "vk_validation.log";

    ctx_ = Vk::DeviceContext::Create(co, error);
    if (!ctx_) {
        return false;
    }
    if (!ctx_->Caps().external_memory_host) {
        error = "the device has no VK_EXT_external_memory_host (guest buffers are imported)";
        return false;
    }
    ctx_->QueryProfile(profile_);
    timeline_ = std::make_unique<Vk::Timeline>(*ctx_);
    // the stream ring: small read-only ranges and user data each command (~35 MiB a frame measured): room for 4 frames
    // in the BAR heap, so recording never waits for the GPU to free it
    // By default hybrid: the CPU copies into cached system memory, one copy per submission puts the bytes into a
    // device-local mirror the GPU reads (P4.6: shaders reading the ring over PCIe cost the village ~14 ms of GPU a
    // frame; the BAR instead slowed the CPU's ~33 MiB of small writes a frame). SSPC_DIAG_RING=system - no mirror (the
    // GPU reads system memory); =bar - the ring in the BAR.
    Vk::HostPlacement ring_place = Vk::HostPlacement::Cached;
    u64 ring_size = 160ull << 20;
    bool ring_mirror = true;
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_DIAG_RING", v, sizeof(v)) > 0) {
        if (std::string_view(v) == "bar") {
            ring_place = Vk::HostPlacement::Bar;
            ring_size = 128ull << 20;
            ring_mirror = false;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_RING=bar - the stream ring ({} MiB) in device-local "
                                   "host-visible memory (BAR) instead of cached system memory",
                     ring_size >> 20);
        } else if (std::string_view(v) == "system") {
            ring_mirror = false;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_RING=system - no video memory mirror of the stream "
                                   "ring (the GPU reads cached system memory)");
        }
    }
    ring_ = std::make_unique<Vk::StreamRing>(*ctx_, *timeline_, ring_size, ring_place);
    if (!timeline_->Ok() || !ring_->Ok()) {
        error = "the timeline or the stream ring could not be created";
        return false;
    }
    if (ring_mirror) {
        if (std::string merr; !ring_->CreateMirror(merr)) {
            SSPC_LOG(GPU, Warning, "native backend: no video memory mirror of the stream ring ({}): the GPU reads it "
                                   "from system memory",
                     merr);
        } else {
            SSPC_LOG(GPU, Info, "native backend: the stream ring is hybrid: a {} MiB device-local mirror, one copy per "
                                "submission",
                     ring_size >> 20);
        }
    }
    // written buffers of dispatches: ~10 MiB a frame measured (the skinning's 21 x 420 KiB first)
    shadow_ring_ = std::make_unique<Vk::StreamRing>(*ctx_, *timeline_, 128ull << 20, Vk::HostPlacement::Device);
    if (!shadow_ring_->Ok()) {
        SSPC_LOG(GPU, Warning, "native backend: no write shadow ring: dispatches write the buffer cache in place");
        shadow_ring_.reset();
    }
    {
        const auto& mem = ctx_->Memory();
        const u32 t = ring_->MemoryType();
        const VkMemoryType& mt = mem.memoryTypes[t];
        SSPC_LOG(GPU, Info, "native backend: the stream ring: {} MiB, memory type {} (flags {:#x}), heap {} ({} MiB, "
                 "flags {:#x})",
                 ring_size >> 20, t, static_cast<u32>(mt.propertyFlags), mt.heapIndex,
                 mem.memoryHeaps[mt.heapIndex].size >> 20, static_cast<u32>(mem.memoryHeaps[mt.heapIndex].flags));
    }
    if (!Vk::CreateBuffer(*ctx_, Cp::kGdsBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          true, gds_, error)) {
        return false;
    }
    std::memset(gds_.mapped, 0, Cp::kGdsBytes);
    guest_ = std::make_unique<Vk::GuestBuffers>(*ctx_);
    buffers_ = std::make_unique<Vk::BufferCache>(*ctx_, *guest_, &tracker_, 2560ull << 20, o.validation);
    images_ = std::make_unique<Vk::ImageCache>(*ctx_, *guest_);
    {
        // the images' video memory: 40% of the device-local heap, at most 3 GiB (the route keeps ~0.85 GiB);
        // SSPC_DIAG_IMAGE_BUDGET_MIB tests the eviction with less
        u64 heap = 0;
        const auto& mem = ctx_->Memory();
        for (u32 h = 0; h < mem.memoryHeapCount; ++h) {
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                heap = std::max<u64>(heap, mem.memoryHeaps[h].size);
            }
        }
        u64 budget = std::min<u64>(heap / 10 * 4, 3ull << 30);
        if (char v[16] = {}; GetEnvironmentVariableA("SSPC_DIAG_IMAGE_BUDGET_MIB", v, sizeof(v)) > 0) {
            budget = std::strtoull(v, nullptr, 10) << 20;
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_IMAGE_BUDGET_MIB - images kept within {} MiB",
                     budget >> 20);
        }
        images_->SetBudget(budget);
    }
    pipelines_ = std::make_unique<Vk::PipelineFactory>(*ctx_, profile_);
    presenter_ = Vk::Presenter::Create(*ctx_, *timeline_, o.sdl_window, error);
    if (!presenter_) {
        return false;
    }
    GS::FrontendOptions fo;
    fo.async = false; // modules ready before the draw that needs them (no draw waits for a worker)
    fo.translate = true;
    // P4.7: a stage with its last ready preparation's inputs takes it again (the executor and the processor report
    // every guest memory change); SSPC_DIAG_STAGE_MEMO=off - every stage prepared in full
    {
        char v[8] = {};
        fo.stage_memo = !(GetEnvironmentVariableA("SSPC_DIAG_STAGE_MEMO", v, sizeof(v)) == 3 && std::string_view(v) == "off");
        if (!fo.stage_memo) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_STAGE_MEMO=off - no memoized stage preparations");
        }
    }
    fo.profile = &profile_;
    // P5.2: SSPC_P52_CODE_TRUST=1 - programs and fetch shaders trusted while their write-tracked code pages stay
    // unwritten (not read and compared again every frame); verify - tracked and still read, counting missed writes
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_P52_CODE_TRUST", v, sizeof(v)) > 0) {
        const std::string_view s(v);
        fo.code_trust = s == "1" ? GS::CodeTrust::On : s == "verify" ? GS::CodeTrust::Verify : GS::CodeTrust::Off;
        if (fo.code_trust != GS::CodeTrust::Off) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_P52_CODE_TRUST={} - shader code trusted while its write-tracked "
                                   "pages stay unwritten", s);
        }
    }
    // P5.3 Native Render Frontend: SSPC_P53_NATIVE_FRONTEND=[verify:]<passes> - the stages of draws the engine
    // described (draw markers; SSPC_P53_NATIVE_DRAW) in these passes take a cached preparation with exactly their
    // inputs. <passes>: "apply" (or 1/on: the passes of SSPC_P53_NATIVE_DRAW=apply:..), "all", or a list 0..40.
    if (char v[160] = {}; GetEnvironmentVariableA("SSPC_P53_NATIVE_FRONTEND", v, sizeof(v)) > 0) {
        std::string_view s(v);
        if (s.rfind("verify", 0) == 0) {
            fo.native_stages = GS::NativeStages::Verify;
            s.remove_prefix(s.size() > 6 && s[6] == ':' ? 7 : 6);
        } else if (s != "0" && s != "off") {
            fo.native_stages = GS::NativeStages::On;
        }
        const auto passes_of = [](std::string_view list) {
            u64 mask = 0;
            for (size_t at = 0; at < list.size();) {
                size_t end = list.find(',', at);
                end = end == std::string_view::npos ? list.size() : end;
                const std::string item(list.substr(at, end - at));
                if (item == "all") {
                    mask |= (u64{1} << 44) - 1; // 0..40, pre, post, deformation (Engine::Render::PassSlot)
                } else if (!item.empty() && std::isdigit(static_cast<unsigned char>(item[0])) != 0) {
                    const unsigned long p = std::strtoul(item.c_str(), nullptr, 10);
                    mask |= p <= 40 ? u64{1} << p : 0;
                }
                at = end + 1;
            }
            return mask;
        };
        if (s.empty() || s == "1" || s == "on" || s == "apply") {
            char d[160] = {};
            const std::string_view draw(d, GetEnvironmentVariableA("SSPC_P53_NATIVE_DRAW", d, sizeof(d)));
            fo.native_passes = draw.rfind("apply:", 0) == 0 ? passes_of(draw.substr(6)) : 0;
        } else {
            fo.native_passes = passes_of(s);
        }
        if (fo.native_stages != GS::NativeStages::Off) {
            SSPC_LOG(GPU, Warning,
                     "EXPERIMENT: SSPC_P53_NATIVE_FRONTEND={} - the Native Render Frontend: stages of the engine's "
                     "draws in passes {:#x} (bits) take a cached preparation with exactly their inputs{}",
                     v, fo.native_passes,
                     fo.native_stages == GS::NativeStages::Verify ? " (verify: prepared in full anyway and compared)"
                                                                  : "");
            if (fo.native_passes == 0) {
                SSPC_LOG(GPU, Error, "SSPC_P53_NATIVE_FRONTEND={}: no pass (with 'apply': SSPC_P53_NATIVE_DRAW has "
                                     "no apply:<passes>) - off", v);
            }
        }
    }
    frontend_ = std::make_unique<GS::ShaderFrontend>(fo);
    // P4.14: SSPC_SHADER_STALL_TRACE (its times are moved to the frame log's origin once that starts, below)
    ShaderTrace::Start(o.logs_dir);
    StartShaderCache(o);
    Vk::ExecutorOptions eo;
    eo.capture_flip = o.capture_flip;
    eo.capture_path = o.logs_dir / "p44" / "native_frame.png";
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_DIAG_BATCH_DRAWS", v, sizeof(v)) > 0) {
        eo.max_draws_per_batch = std::max<u32>(16, static_cast<u32>(std::strtoul(v, nullptr, 10)));
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BATCH_DRAWS={} - a batch is submitted after that many "
                               "commands", eo.max_draws_per_batch);
    }
    images_->SetBufferCache(buffers_.get());
    executor_ = std::make_unique<Vk::Executor>(*ctx_, *timeline_, *ring_, *guest_, *buffers_, *images_, *pipelines_,
                                               *frontend_, presenter_.get(), gds_, eo);
    // P4.10: draws and dispatches recorded on a second thread behind the command processor (parallel_cp.h). On by
    // default since P5.3D (the owner's decision; single-thread CP ~32 FPS vs ~41 parallel on the beach);
    // SSPC_P410_PARALLEL_CP=0: one thread (the rollback). SSPC_P410_SPIN_US: how long a waiting side spins first.
    if (char v[8] = {}; !(GetEnvironmentVariableA("SSPC_P410_PARALLEL_CP", v, sizeof(v)) == 1 && v[0] == '0')) {
        u32 spin_us = 20;
        if (char s[16] = {}; GetEnvironmentVariableA("SSPC_P410_SPIN_US", s, sizeof(s)) > 0) {
            spin_us = static_cast<u32>(std::strtoul(s, nullptr, 10));
        }
        // SSPC_P410_ASYNC=0: the processor's other calls drain the queue (the first prototype); SSPC_P410_ASYNC_COPIES=0:
        // timeline copies do (the rest queued)
        const auto env_off = [](const char* name) {
            char e[8] = {};
            return GetEnvironmentVariableA(name, e, sizeof(e)) == 1 && e[0] == '0';
        };
        const bool async = !env_off("SSPC_P410_ASYNC"), async_copies = !env_off("SSPC_P410_ASYNC_COPIES");
        // the shader frontend on the processor's thread too (stage 2: the recording thread only records);
        // SSPC_P410_FRONTEND=0 - on the recording thread (stage 1b)
        const bool frontend_ahead = !env_off("SSPC_P410_FRONTEND");
        ParallelCp::Options po;
        po.verify = [] {
            char e[8] = {};
            return GetEnvironmentVariableA("SSPC_P410_VERIFY", e, sizeof(e)) == 1 && e[0] == '1';
        }();
        po.spin_us = spin_us;
        po.async = async;
        po.async_copies = async_copies;
        // P4.11: SSPC_P411_ASYNC_ACQUIRE=1 - a met WAIT_REG_MEM on memory does not drain the queue (its CPU writes
        // applied on the processor's thread; parallel_cp.h, Options::async_acquire); off by default
        po.async_acquire = [] {
            char e[8] = {};
            return GetEnvironmentVariableA("SSPC_P411_ASYNC_ACQUIRE", e, sizeof(e)) == 1 && e[0] == '1';
        }();
        if (frontend_ahead) {
            po.frontend = frontend_.get();
            po.map_gen = &memory_.map_gen;
            po.timeline = timeline_.get();
            // P5.3D: SSPC_P53_FRONTEND_THREAD=1 - the frontend on a third thread between the processor and the
            // recording thread (parallel_cp.h, Options::frontend_thread)
            char ft[4] = {};
            po.frontend_thread = GetEnvironmentVariableA("SSPC_P53_FRONTEND_THREAD", ft, sizeof(ft)) == 1 && ft[0] == '1';
        }
        parallel_ = std::make_unique<ParallelCp>(*executor_, buffers_.get(), po);
        parallel_->SetCopyFallback(
            [](void* self, u64 src, u64 dst, u64 bytes) { return static_cast<Backend*>(self)->CpuCopy(src, dst, bytes); },
            this);
        SSPC_LOG(GPU, Warning, "parallel command processor (default; SSPC_P410_PARALLEL_CP=0 turns it off) - draws and "
                               "dispatches are recorded on a second thread behind the command processor (spin {} us "
                               "before a blocking wait; other calls {}, "
                               "timeline copies {}; the shader frontend on the {} thread; met memory waits {})",
                 spin_us, async ? "queued" : "drained", async && async_copies ? "queued" : "drained",
                 po.frontend_thread ? "frontend's own (SSPC_P53_FRONTEND_THREAD=1)"
                 : frontend_ahead   ? "processor's"
                                    : "recording",
                 po.async_acquire && async && frontend_ahead ? "queued (SSPC_P411_ASYNC_ACQUIRE=1)" : "drained");
    }
    Cp::Options cpo;
    cpo.mode = Cp::Mode::Isolated;
    cpo.memory = &memory_;
    cpo.work = parallel_ ? static_cast<Cp::GpuWork*>(parallel_.get()) : executor_.get();
    cpo.gds = gds_.mapped;
    // P4.6: the command processor does not stop for the GPU where the guest's GPU would not - a WAIT_REG_MEM a held
    // fence will meet passes in the GPU's order (EVENT_WRITE_EOS + a wait for it: ~27 GPU drains a frame before), DMA
    // with GDS is recorded on the timeline, WRITE_DATA to VideoOut's labels lands at once (docs\PHASE4_P46_PERFORMANCE.md
    // §6). SSPC_P46_SYNC=0: the P4.5 behaviour (a control for A/B runs, not a configuration).
    const bool p46_sync = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("SSPC_P46_SYNC", v, sizeof(v)) == 1 && v[0] == '0');
    }();
    if (!p46_sync) {
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC CONTROL: SSPC_P46_SYNC=0 - the command processor waits for the GPU as in P4.5");
    }
    cpo.waits_by_held_fences = p46_sync;
    cpo.gds_on_timeline = p46_sync;
    memory_.unordered_labels = p46_sync;
    // EOP/RELEASE_MEM clocks in the guest's time base (P4.5): data_sel 4 (the GPU counter the game scales with
    // sceGnmGetGpuCoreClockFrequency, 800 MHz) is the TSC the game reads with rdtsc, in GPU core clocks - as the
    // bridge's GetGpuPerfCounter; data_sel 3 (the 64-bit GPU clock) nanoseconds as the bridge's GetGpuClock64. Before,
    // both were the host's steady clock in ns: another base and rate than every other time the game sees.
    // SSPC_DIAG_GPU_COUNTER=ns: the old clocks (a control run, not a configuration).
    static const bool counter_ns = [] {
        char v[8] = {};
        const bool ns = GetEnvironmentVariableA("SSPC_DIAG_GPU_COUNTER", v, sizeof(v)) == 2 && std::string_view(v) == "ns";
        if (ns) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_GPU_COUNTER=ns - GPU counter fences in host ns");
        }
        return ns;
    }();
    if (!counter_ns) {
        cpo.clock = [](void*, u32 data_sel) -> u64 {
            if (data_sel == 4) {
                constexpr u64 kGpuCoreClock = 800'000'000; // sceGnmGetGpuCoreClockFrequency (base PS4)
                const u64 tsc = Kernel::Clock::ReadTsc(), freq = std::max<u64>(Kernel::Clock::TscFrequency(), 1);
                return tsc / freq * kGpuCoreClock + tsc % freq * kGpuCoreClock / freq;
            }
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::high_resolution_clock::now().time_since_epoch())
                                        .count());
        };
    }
    cp_ = std::make_unique<Cp::CommandProcessor>(cpo);
    executor_->Attach(cp_.get());
    if (parallel_) {
        parallel_->Attach(*cp_); // (the executor reads its replica of the registers from now on)
    }
    executor_->SetWriteShadowRing(shadow_ring_.get());
    diag_ = std::make_unique<Vk::Diagnostics>(*ctx_, o.diagnostics, o.logs_dir);
    executor_->SetDiagnostics(diag_.get());
    executor_->SetAcquireHook([](void* self) { return static_cast<Backend*>(self)->DrainCpuWrites(); }, this);
    images_->SetDiagnostics(diag_.get());
    // one owner of the guest GPU: every resource, or none
    for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
        if (!Claim(static_cast<Resource>(r), BackendKind::Sspc, &error)) {
            for (u8 k = 0; k < r; ++k) {
                Release(static_cast<Resource>(k), BackendKind::Sspc);
            }
            return false;
        }
    }
    GuestMemory* gm = GetGuestMemory();
    if (gm == nullptr || !gm->SetListener(this, BackendKind::Sspc, &error)) {
        if (gm == nullptr) {
            error = "no guest memory contract installed";
        }
        for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
            Release(static_cast<Resource>(r), BackendKind::Sspc);
        }
        return false;
    }
    if (!tracker_.Start(*gm, error)) {
        gm->SetListener(nullptr, BackendKind::Sspc, nullptr);
        for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
            Release(static_cast<Resource>(r), BackendKind::Sspc);
        }
        return false;
    }
    images_->SetTracker(&tracker_);
    guest_->SetTracker(&tracker_);
    frontend_->SetCodeTracker(
        [](void* ctx, u64 addr, u64 size) {
            auto* t = static_cast<PageTracker*>(ctx);
            if (!t->CanArm(addr, size)) {
                return false;
            }
            t->Arm(addr, size);
            return true;
        },
        &tracker_);
    memory_.tracker = &tracker_;
    // (P5.2: the parallel processor runs the frontend on its thread unless SSPC_P410_FRONTEND=0)
    if (char v[4] = {}; !parallel_ || !(GetEnvironmentVariableA("SSPC_P410_FRONTEND", v, sizeof(v)) == 1 && v[0] == '0')) {
        memory_.frontend = frontend_.get();
    }
    memory_.buffers = buffers_.get();
    memory_.executor = executor_.get();
    memory_.parallel = parallel_.get();
    executor_->SetMapGeneration(&memory_.map_gen); // P4.7: the frontend's reads keep backing runs between maps
    Gnm::SetSdkVersion(o.sdk_version);
    VideoOut::BufferLabelAddress(1, label_base_);
    FrameLog::Start();
    ShaderTrace::SetOrigin(FrameLog::StartTsc());
    // P4.12: SSPC_P412_STALL_TRACE=light|deep[,...] - the stall trace (off otherwise: nothing of it runs); deep mode
    // converts the batches' GPU timestamps with the device's calibrated timestamps (GPU ticks <-> QPC)
    if (ShaderTrace::On()) {
        StallTrace::EnableForward(); // (P4.14: the waits for the shader stall trace's breakdown of long frames)
    }
    if (StallTrace::Start(o.logs_dir, ctx_->Caps().timestamp_period) && ctx_->Caps().calibrated_timestamps) {
        StallTrace::SetCalibration(
            [](void* c, u64& gpu, u64& qpc, u64& deviation) {
                const auto* ctx = static_cast<const Vk::DeviceContext*>(c);
                const VkCalibratedTimestampInfoEXT info[2] = {
                    {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
                     .pNext = nullptr,
                     .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
                    {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
                     .pNext = nullptr,
                     .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT}};
                u64 ts[2] = {};
                u64 dev = 0;
                if (Vk::Fn().vkGetCalibratedTimestampsEXT(ctx->Device(), 2, info, ts, &dev) != VK_SUCCESS) {
                    return false;
                }
                gpu = ts[0];
                qpc = ts[1];
                deviation = dev;
                return true;
            },
            ctx_.get());
    }
    // P4.8: SSPC_INSPECTOR=light - the Performance Inspector (off otherwise: nothing of it runs)
    if (Inspector::Start({.logs_dir = o.logs_dir,
                          .device = ctx_->Name(),
                          .timestamp_period = ctx_->Caps().timestamp_period,
                          .fine_timing = GS::g_fine_timing || GS::g_fine_every != 0})) {
        executor_->SetInspector(Inspector::GetCatalog(), Inspector::GpuEvery());
    }
    executor_->SetFlipHook([](void* self, u64 value) { static_cast<Backend*>(self)->OnFlip(value); }, this);
    Gnm::SetHost(this);
    VideoOut::SetLabelCallback([](void* ctx) { static_cast<Backend*>(ctx)->cv_.notify_one(); }, this);
    VideoOut::Start(this);
    stop_.store(false);
    if (parallel_) {
        parallel_->Start();
    }
    thread_ = std::thread([this] { Loop(); });
    if (Inspector::On()) {
        Inspector::SetGpuThread(reinterpret_cast<HANDLE>(thread_.native_handle()));
    }
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_SAMPLE", v, sizeof(v)) > 0 && v[0] == '1') {
        char delay[16] = {};
        const u32 after = GetEnvironmentVariableA("SSPC_DIAG_SAMPLE_AFTER", delay, sizeof(delay)) > 0
                              ? static_cast<u32>(std::strtoul(delay, nullptr, 10))
                              : 30;
        char game[64] = {};
        if (const DWORD gn = GetEnvironmentVariableA("SSPC_DIAG_SAMPLE_GAME", game, sizeof(game)); gn > 0) {
            game_sampler_.StartByName(std::wstring(game, game + gn), 1000, after);
        }
        if (sampler_.Start(reinterpret_cast<HANDLE>(thread_.native_handle()), 1000, after)) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_SAMPLE=1 - the GPU thread is sampled every 1 ms from {} s "
                                   "(logs\\gpu_cpu_profile.txt at the end)", after);
        }
    }
    // P5.2: every thread of the process sampled raw, for attribution to the game's functions offline
    // (tools/native-reconstruction/cpu_profile.py); SSPC_DIAG_SAMPLE_RAW_US = period (default 2000), _AFTER = start.
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_SAMPLE_RAW", v, sizeof(v)) > 0 && v[0] == '1') {
        char s[16] = {};
        const u32 period = GetEnvironmentVariableA("SSPC_DIAG_SAMPLE_RAW_US", s, sizeof(s)) > 0
                               ? static_cast<u32>(std::strtoul(s, nullptr, 10))
                               : 2000;
        char d[16] = {};
        const u32 after = GetEnvironmentVariableA("SSPC_DIAG_SAMPLE_AFTER", d, sizeof(d)) > 0
                              ? static_cast<u32>(std::strtoul(d, nullptr, 10))
                              : 30;
        if (raw_sampler_.StartRaw(period, after)) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_SAMPLE_RAW=1 - every thread is sampled every {} us from {} s "
                                   "(logs\\cpu_samples_raw.txt at the end)", period, after);
        }
    }
    SSPC_LOG(GPU, Info, "native backend started on {} (the game's GPU work runs on SecondSonPC's Vulkan backend)",
             ctx_->Name());
    return true;
}

void Backend::Apply(const Submission& s) {
    if (parallel_ && parallel_->FrontendThread()) {
        parallel_->MemoryChanged(); // (P5.3D: the frontend's thread is told with the next record)
    } else {
        executor_->GuestMemoryChanged(); // (P4.7: the guest's CPU may have written anything before it submitted)
    }
    switch (s.kind) {
    case Submission::Kind::Graphics:
        ++submissions_;
        cp_->SubmitGraphics(s.addr, s.dwords, s.data);
        break;
    case Submission::Kind::MapQueue:
        if (s.vqid < Cp::kNumQueues && cp_->MapComputeQueue(s.vqid, s.addr, s.ring_dwords, s.read_ptr)) {
            mapped_[s.vqid] = true;
            executor_->SetQueuePipe(s.vqid, s.pipe);
        }
        break;
    case Submission::Kind::UnmapQueue:
        if (s.vqid < Cp::kNumQueues) {
            cp_->UnmapComputeQueue(s.vqid);
            mapped_[s.vqid] = false;
        }
        break;
    case Submission::Kind::DingDong:
        ++submissions_;
        cp_->DingDong(s.vqid, s.dwords);
        break;
    case Submission::Kind::Done:
        submit_done_ = true;
        break;
    }
}

bool Backend::DrainCpuWrites() {
    bool any = false;
    // the exact first write of each block given back: surely a write at that address (a small GPU-written image has no
    // whole block a coarse report could name; P4.5)
    fault_addrs_.clear();
    tracker_.TakeFaultAddresses(fault_addrs_);
    for (const u64 a : fault_addrs_) {
        images_->Invalidate(a, 1);
    }
    tracker_.Drain([&](u64 a, u64 sz) {
        any = true;
        images_->InvalidateCoarse(a, sz); // whole 64 KiB blocks: a GPU-written image counts only its interior ones
        buffers_->InvalidateCoarse(a, sz); // whole 64 KiB blocks: the GPU's pending pages there stay
        FrontendInvalidate(a, sz);
    });
    return any;
}

bool Backend::CpuCopy(u64 src, u64 dst, u64 bytes) {
    GuestMemory* gm = GetGuestMemory();
    if (gm == nullptr || !GuestMemory::IsValidGpuMapping(src, bytes) || !GuestMemory::IsValidGpuMapping(dst, bytes)) {
        return false;
    }
    copy_scratch_.resize(bytes);
    gm->CopySparse(src, copy_scratch_.data(), bytes);
    if (!gm->TryWriteBacking(reinterpret_cast<void*>(dst), copy_scratch_.data(), bytes)) {
        // memory without physical backing: in place when writable (Memory::Write's path; the tracker's pages first)
        tracker_.Disarm(dst, bytes);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(dst), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 ||
            reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize < dst + bytes) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(dst), copy_scratch_.data(), bytes);
    }
    images_->Invalidate(dst, bytes); // (what Executor::Dma does for a DMA the CPU executed)
    buffers_->Invalidate(dst, bytes);
    return true;
}

void Backend::Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuNative");
    StallTrace::RegisterThread(StallTrace::Role::Cp);
    std::deque<Submission> local;
    auto rate_t0 = std::chrono::steady_clock::now();
    u64 rate_flips = 0, rate_draws = 0, rate_record = 0, rate_wait = 0, rate_tw = 0, rate_gpu = 0, rate_submit = 0;
    u64 rate_front = 0, rate_bind = 0, rate_range = 0, rate_image = 0, rate_streamed = 0, rate_streamed_bytes = 0;
    u64 rate_pipe = 0, rate_targets = 0, rate_vertex = 0, rate_layout = 0, rate_emit = 0, rate_dispatch = 0;
    u64 rate_mirrored = 0;
    u64 rate_ring_waits = 0, rate_classify = 0, rate_hostptr = 0, rate_alloc = 0, rate_copy = 0, rate_prepare = 0;
    u64 rate_nprep = 0, rate_tl = 0, rate_tb = 0, rate_ts = 0, rate_th = 0, rate_ta = 0, rate_tc = 0, rate_nu = 0;
    u64 prof_cp = 0, prof_poll = 0, prof_flush = 0, prof_idle = 0; // ns since the last rate line
    u64 prof_idle_blocked = 0, prof_idle_gpu = 0; // of the idle time: a WAIT_REG_MEM blocked; waiting for the GPU
    ParallelTotals rate_pt{}; // (P4.11 soak telemetry: the parallel processor's totals at the last rate line)
    u64 rate_t1_cpu = 0, rate_t2_cpu = 0, rate_proc_cpu = 0; // (... and the threads' CPU time, ns)
    using Clock = std::chrono::steady_clock;
    const auto ns_since = [](Clock::time_point t) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
    };
    while (!stop_.load(std::memory_order_acquire)) {
        const auto iter_t0 = Clock::now();
        const u64 pre_tsc = StallTrace::On() ? StallTrace::Now() : 0; // (P4.12)
        {
            std::unique_lock lock{mutex_};
            local.swap(queue_);
        }
        for (const auto& s : local) {
            Apply(s);
        }
        const size_t taken = local.size();
        local.clear();
        {
            std::vector<std::pair<u64, u64>> inval;
            {
                std::scoped_lock lock{inval_mutex_};
                inval.swap(pending_inval_);
            }
            for (const auto& [a, sz] : inval) {
                FrontendInvalidate(a, sz);
            }
        }
        // the guest's writes to tracked pages, before the work it submitted after them
        DrainCpuWrites();
        if (pre_tsc != 0) {
            StallTrace::Span(StallTrace::Ev::PreRound, pre_tsc, StallTrace::Now(), taken);
        }
        bool progress = false, blocked = false, pending = false, blocked_label = false;
        u32 round_packets = 0, round_capped = 0;
        bool graphics_capped = false;
        const auto cp_t0 = Clock::now();
        round_t0_ = cp_t0;
        tot_pre_ns_ += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(cp_t0 - iter_t0).count());
        // SSPC_DIAG_QUEUE_ORDER=compute-first: the compute queues stepped before the graphics queue in each round (an
        // experiment on async compute's place relative to the graphics frame's end-of-pipe; P4.5)
        static const bool compute_first = [] {
            char v[16] = {};
            const bool c = GetEnvironmentVariableA("SSPC_DIAG_QUEUE_ORDER", v, sizeof(v)) > 0 &&
                           std::string_view(v) == "compute-first";
            if (c) {
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_QUEUE_ORDER=compute-first");
            }
            return c;
        }();
        for (u32 k = 0; k < Cp::kNumQueues; ++k) {
            const u32 q = compute_first ? (k + 1) % Cp::kNumQueues : k;
            if (q != Cp::kGraphicsQueue && !mapped_[q]) {
                continue;
            }
            u32 i = 0;
            const u64 slice_t0 = StallTrace::On() ? StallTrace::Now() : 0; // (P5.4: the queue's slice of the round)
            u16 slice_stop = 0;
            for (; i < 4096; ++i) {
                const Cp::StepStatus st = cp_->Step(q);
                if (st == Cp::StepStatus::Packet) {
                    progress = true;
                    continue;
                }
                if (st == Cp::StepStatus::Blocked) {
                    blocked = true;
                    slice_stop = 1;
                    const u64 poll = cp_->BlockedPoll(q);
                    blocked_label |= poll >= label_base_ && poll < label_base_ + VideoOut::kMaxBuffers * 8;
                } else if (st == Cp::StepStatus::Faulted) {
                    slice_stop = 3;
                    if (cp_errors_++ == 0) {
                        first_cp_error_ = cp_->LastError().Text();
                        SSPC_LOG(GPU, Error, "native backend: the command processor stopped a submission: {}",
                                 first_cp_error_);
                    }
                }
                break;
            }
            if (slice_t0 != 0 && (i != 0 || slice_stop == 1)) {
                StallTrace::Span(StallTrace::Ev::CpQueueSlice, slice_t0, StallTrace::Now(), q, i,
                                 i == 4096 ? u16{2} : slice_stop);
            }
            round_packets += i; // (P4.12: the round's packets; a queue that stopped at the limit)
            round_capped += i == 4096 ? 1 : 0;
            graphics_capped |= i == 4096 && q == Cp::kGraphicsQueue;
            pending |= cp_->PendingSubmissions(q) != 0;
        }
        if (parallel_) {
            // P4.10: everything the round handed to the recording thread executed - polls, flushes and idle waits as
            // with one thread (part of the round's time: the processor's work is done only now)
            parallel_->Drain(DrainReason::RoundEnd);
            if (parallel_->Failed()) {
                SSPC_LOG(GPU, Error, "native backend: {}; the process exits with code 5", parallel_->FailureText());
                std::thread([] { Lifecycle::Exit(5, "native GPU backend: the parallel recording thread failed"); }).detach();
                return;
            }
        }
        const u64 round_ns = ns_since(cp_t0);
        prof_cp += round_ns;
        tot_busy_ns_ += round_ns;
        tot_round_ns_ += round_ns;
        const auto poll_t0 = Clock::now();
        {
            const StallTrace::Scope poll_span(StallTrace::Ev::Poll, round_packets, round_capped,
                                              graphics_capped ? 1 : 0);
            executor_->Poll(); // completed batches: the processor writes its held labels now
        }
        StallTrace::MaybeCalibrate();
        const u64 poll_ns = ns_since(poll_t0);
        prof_poll += poll_ns;
        tot_busy_ns_ += poll_ns;
        tot_poll_ns_ += poll_ns;
        if (ctx_->Failed()) {
            const VkResult result = ctx_->Failure();
            failure_result_.store(result, std::memory_order_release);
            SSPC_LOG(GPU, Error, "native backend: stopped after VkResult {}; the process exits with code 5",
                     static_cast<int>(result));
            std::thread([] {
                Lifecycle::Exit(5, "native GPU backend failure (see GPU log)");
            }).detach();
            return;
        }
        if (const auto now = std::chrono::steady_clock::now(); now - rate_t0 >= std::chrono::seconds(10)) {
            const auto& s = executor_->Stats();
            const double secs = std::chrono::duration<double>(now - rate_t0).count();
            const double frames = std::max<double>(1.0, static_cast<double>(s.flips - rate_flips));
            const auto per_frame = [&](double ns) { return ns / 1e6 / frames; };
            // the stage timers' sums (TSC ticks); sampled (SSPC_GPU_TIMINGS=sample:N): of every N-th command, times N
            const auto fine_frame = [&](u64 ticks) {
                return per_frame(static_cast<double>(GS::TicksToNs(ticks)) * static_cast<double>(GS::FineScale()));
            };
            SSPC_LOG(GPU, Info,
                     "native backend: {:.1f} flips/s, {:.0f} draws/s over the last {:.1f} s: recording {} ms, waiting "
                     "for the GPU {} ms, writes on the timeline {} | per frame (ms): command processor {:.1f} (of it "
                     "recording {:.1f}, waiting for the GPU {:.1f}), submit {:.1f}, poll {:.1f}, flush {:.1f}, idle {:.1f}; "
                     "GPU busy {:.1f}",
                     static_cast<double>(s.flips - rate_flips) / secs, static_cast<double>(s.draws - rate_draws) / secs,
                     secs, (s.record_ns - rate_record) / 1'000'000, (s.sync_wait_us - rate_wait) / 1000,
                     s.timeline_writes - rate_tw, per_frame(static_cast<double>(prof_cp)),
                     per_frame(static_cast<double>(s.record_ns - rate_record)),
                     per_frame(static_cast<double>(s.sync_wait_us - rate_wait) * 1000.0),
                     per_frame(static_cast<double>(s.submit_ns - rate_submit)), per_frame(static_cast<double>(prof_poll)),
                     per_frame(static_cast<double>(prof_flush)), per_frame(static_cast<double>(prof_idle)),
                     per_frame(static_cast<double>(s.gpu_busy_ns - rate_gpu)));
            SSPC_LOG(GPU, Info,
                     "native backend: recording per frame (ms): frontend {:.1f}, binding {:.1f} (buffers {:.1f}, images "
                     "{:.1f}); streamed {:.0f} small ranges ({:.1f} MiB) per frame; stream ring waits {}, mirrored "
                     "{:.1f} MiB per frame",
                     fine_frame(s.t_front - rate_front),
                     fine_frame(s.t_bind - rate_bind),
                     fine_frame(s.t_range - rate_range),
                     fine_frame(s.t_image - rate_image),
                     static_cast<double>(s.streamed - rate_streamed) / frames,
                     static_cast<double>(s.streamed_bytes - rate_streamed_bytes) / 1048576.0 / frames,
                     ring_->Waits() - rate_ring_waits,
                     static_cast<double>(ring_->MirroredBytes() - rate_mirrored) / 1048576.0 / frames);
            rate_ring_waits = ring_->Waits();
            rate_mirrored = ring_->MirroredBytes();
            if (GS::g_fine_timing || GS::g_fine_every != 0) { // (SSPC_GPU_TIMINGS=1 or sample[:N] only)
                SSPC_LOG(GPU, Info,
                         "native backend: draw stages per frame (ms): pipeline {:.1f}, targets {:.1f}, vertex/index {:.1f}, "
                         "barriers/layouts {:.1f}, commands {:.1f}; dispatches whole {:.1f}",
                         fine_frame(s.t_pipe - rate_pipe),
                         fine_frame(s.t_targets - rate_targets),
                         fine_frame(s.t_vertex - rate_vertex),
                         fine_frame(s.t_layout - rate_layout),
                         fine_frame(s.t_emit - rate_emit),
                         fine_frame(s.t_dispatch - rate_dispatch));
                rate_pipe = s.t_pipe;
                rate_targets = s.t_targets;
                rate_vertex = s.t_vertex;
                rate_layout = s.t_layout;
                rate_emit = s.t_emit;
                rate_dispatch = s.t_dispatch;
                SSPC_LOG(GPU, Info,
                         "native backend: buffers per frame (ms): classify {:.1f}, host pointer {:.1f}, ring {:.1f}, copy {:.1f}, "
                         "cache prepare {:.1f} ({:.0f} calls)",
                         fine_frame(s.t_classify - rate_classify),
                         fine_frame(s.t_hostptr - rate_hostptr),
                         fine_frame(s.t_alloc - rate_alloc),
                         fine_frame(s.t_copy - rate_copy),
                         fine_frame(s.t_prepare - rate_prepare),
                         static_cast<double>(s.n_prepare - rate_nprep) / frames);
                rate_classify = s.t_classify;
                rate_hostptr = s.t_hostptr;
                rate_alloc = s.t_alloc;
                rate_copy = s.t_copy;
                rate_prepare = s.t_prepare;
                rate_nprep = s.n_prepare;
                const auto& bc = buffers_->Stats();
                SSPC_LOG(GPU, Info,
                         "native backend: cache prepare per frame (ms): large ranges {:.1f}, bind {:.1f}, scan {:.1f}, upload "
                         "hook {:.1f}, write-protect {:.1f}, copies {:.1f} ({:.0f} uploading calls)",
                         fine_frame(bc.t_large - rate_tl),
                         fine_frame(bc.t_bound - rate_tb),
                         fine_frame(bc.t_scan - rate_ts),
                         fine_frame(bc.t_hook - rate_th),
                         fine_frame(bc.t_arm - rate_ta),
                         fine_frame(bc.t_copy - rate_tc),
                         static_cast<double>(bc.n_upload_calls - rate_nu) / frames);
            }
            const auto& bc = buffers_->Stats();
            rate_tl = bc.t_large;
            rate_tb = bc.t_bound;
            rate_ts = bc.t_scan;
            rate_th = bc.t_hook;
            rate_ta = bc.t_arm;
            rate_tc = bc.t_copy;
            rate_nu = bc.n_upload_calls;
            rate_streamed_bytes = s.streamed_bytes;
            rate_front = s.t_front;
            rate_bind = s.t_bind;
            rate_range = s.t_range;
            rate_image = s.t_image;
            rate_streamed = s.streamed;
            rate_gpu = s.gpu_busy_ns;
            rate_submit = s.submit_ns;
            SSPC_LOG(GPU, Info,
                     "native backend: idle per frame (ms): with a WAIT_REG_MEM blocked {:.1f}, while the GPU had work "
                     "{:.1f} (WAIT_REG_MEM blocks so far {})",
                     per_frame(static_cast<double>(prof_idle_blocked)), per_frame(static_cast<double>(prof_idle_gpu)),
                     cp_->Stats().waits_blocked);
            {
                // P4.11 soak telemetry: what could grow over a long session - handles, Vulkan objects, the caches, the
                // parallel processor's queue (the process's memory and VRAM: --vram-log's samples)
                DWORD handles = 0;
                GetProcessHandleCount(GetCurrentProcess(), &handles);
                // the threads' CPU over the interval (% of one core): this GPU thread, the recording thread, the process
                const auto ft = [](const FILETIME& f) { return (u64{f.dwHighDateTime} << 32 | f.dwLowDateTime) * 100; };
                FILETIME c{}, e{}, k{}, u{};
                const u64 t1_cpu = GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? ft(k) + ft(u) : 0;
                const u64 proc_cpu = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft(k) + ft(u) : 0;
                const u64 t2_cpu = parallel_ ? parallel_->RecordingThreadCpuNs() : 0;
                const double wall_ns = secs * 1e9;
                const std::string cpu = fmt::format(
                    "; cpu (% of a core): GPU thread {:.0f}, recording thread {:.0f}, process {:.0f}",
                    static_cast<double>(t1_cpu - rate_t1_cpu) * 100.0 / wall_ns,
                    static_cast<double>(t2_cpu - rate_t2_cpu) * 100.0 / wall_ns,
                    static_cast<double>(proc_cpu - rate_proc_cpu) * 100.0 / wall_ns);
                rate_t1_cpu = t1_cpu;
                rate_t2_cpu = t2_cpu;
                rate_proc_cpu = proc_cpu;
                const auto is = images_->Stats();
                const auto gs = guest_->GetStats();
                const auto& ps = pipelines_->Stats();
                std::string par;
                if (parallel_) {
                    const ParallelTotals pt = parallel_->Totals();
                    const u64 recs = pt.records - rate_pt.records;
                    par = fmt::format("; queue: depth mean {:.1f} (max ever {}), drains that waited {:.1f}/frame, thread 1 "
                                      "waited {:.2f} ms/frame, thread 2 executing {:.2f} ms/frame, GPU write ranges {} "
                                      "(max {}), copies by the CPU {} (failed {}), async acquires {:.1f}/frame",
                                      recs != 0 ? static_cast<double>(pt.depth_sum - rate_pt.depth_sum) /
                                                      static_cast<double>(recs)
                                                : 0.0,
                                      pt.depth_max, static_cast<double>(pt.drains - rate_pt.drains) / frames,
                                      per_frame(static_cast<double>(GS::TicksToNs(pt.t1_drain_ticks + pt.t1_full_ticks -
                                                                                  rate_pt.t1_drain_ticks -
                                                                                  rate_pt.t1_full_ticks))),
                                      per_frame(static_cast<double>(GS::TicksToNs(pt.t2_busy_ticks - rate_pt.t2_busy_ticks))),
                                      pt.gw_now, pt.gw_max, pt.copy_fallbacks, pt.copy_failures,
                                      static_cast<double>(pt.async_acquires - rate_pt.async_acquires) / frames);
                    rate_pt = pt;
                }
                SSPC_LOG(GPU, Info,
                         "soak: handles {}; batches {}, images {} ({} MiB, created {}), buffers resident {} MiB, guest "
                         "windows {} ({} MiB), pipelines {} graphics + {} compute, modules {}, frontend {} MiB, timeline "
                         "writes pending {}{}",
                         handles, executor_->BatchObjects(), is.images, is.bytes >> 20, is.created,
                         buffers_->Stats().resident_bytes >> 20, gs.windows, gs.bytes >> 20, ps.graphics, ps.compute,
                         frontend_->Modules(), frontend_->MemoryBytes() >> 20, executor_->TimelineWritesCount(), par + cpu);
            }
            prof_idle_blocked = prof_idle_gpu = 0;
            prof_cp = prof_poll = prof_flush = prof_idle = 0;
            rate_record = s.record_ns;
            rate_wait = s.sync_wait_us;
            rate_tw = s.timeline_writes;
            rate_t0 = now;
            rate_flips = s.flips;
            rate_draws = s.draws;
        }
        if (executor_->WantsFlush() || (!progress && executor_->HasOpenWork())) {
            const auto flush_t0 = Clock::now();
            executor_->Flush();
            const u64 flush_ns = ns_since(flush_t0);
            prof_flush += flush_ns;
            tot_busy_ns_ += flush_ns;
            tot_flush_ns_ += flush_ns;
        }
        if (progress) {
            continue;
        }
        const bool queue_empty = [&] {
            std::scoped_lock lock{mutex_};
            return queue_.empty();
        }();
        if (queue_empty && !pending && !blocked) {
            recorded_.store(true, std::memory_order_release);
        }
        if (queue_empty && !pending && !blocked && !executor_->Busy() && !executor_->HasOpenWork()) {
            quiet_.store(true, std::memory_order_release);
        }
        if (submit_done_ && queue_empty && !pending && !blocked && !executor_->Busy() &&
            !executor_->HasOpenWork()) {
            submit_done_ = false;
            idle_.store(true, std::memory_order_release);
            Gnm::RaiseInterrupt(Gnm::Interrupt::GpuIdle);
        }
        const auto idle_t0 = Clock::now();
        const u64 idle_tsc = StallTrace::On() ? StallTrace::Now() : 0; // (P4.12)
        const bool busy = executor_->Busy();
        // SSPC_DIAG_BLOCK_SPIN_US=<n>: a WAIT_REG_MEM blocked on memory the game's CPU writes (no notification comes)
        // is polled again after n us instead of a 1 ms sleep (P4.6 experiment)
        static const u64 spin_ns = [] {
            char v[16] = {};
            const u64 us = GetEnvironmentVariableA("SSPC_DIAG_BLOCK_SPIN_US", v, sizeof(v)) > 0
                               ? std::strtoull(v, nullptr, 10)
                               : 0ull;
            if (us != 0) {
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BLOCK_SPIN_US={} - a blocked memory wait is "
                                       "polled every {} us", us, us);
            }
            return us * 1000;
        }();
        if (blocked && !blocked_label && spin_ns != 0) {
            const auto until = idle_t0 + std::chrono::nanoseconds(spin_ns);
            while (Clock::now() < until) {
                YieldProcessor();
            }
        } else if (busy) {
            executor_->WaitOldest(1'000'000); // 1 ms: completion unblocks labels
        } else {
            std::unique_lock lock{mutex_};
            cv_.wait_for(lock, std::chrono::milliseconds(1), [&] { return !queue_.empty() || stop_.load(); });
        }
        const u64 idle_ns = ns_since(idle_t0);
        if (idle_tsc != 0) {
            StallTrace::Span(blocked   ? (blocked_label ? StallTrace::Ev::IdleLabel : StallTrace::Ev::IdleFlag)
                             : busy    ? StallTrace::Ev::IdleGpu
                                       : StallTrace::Ev::IdleEmpty,
                             idle_tsc, StallTrace::Now(), timeline_->LastSubmitted(), busy ? 1u : 0u);
        }
        prof_idle += idle_ns;
        prof_idle_blocked += blocked ? idle_ns : 0;
        prof_idle_gpu += busy ? idle_ns : 0;
        (blocked ? (blocked_label ? tot_blocked_label_ns_ : tot_blocked_other_ns_) : tot_empty_ns_) += idle_ns;
        tot_idle_gpu_ns_ += busy ? idle_ns : 0;
    }
}

void Backend::OnFlip(u64 gpu_value) {
    const auto& s = executor_->Stats();
    FrameLog::GpuThreadTotals t;
    t.cp_ns = tot_busy_ns_ + static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - round_t0_)
                                                  .count());
    t.record_ns = s.record_ns;
    t.sync_ns = s.sync_wait_us * 1000;
    t.blocked_label_ns = tot_blocked_label_ns_;
    t.blocked_other_ns = tot_blocked_other_ns_;
    t.empty_ns = tot_empty_ns_;
    t.gpu_busy_ns = s.gpu_busy_ns;
    t.draws = s.draws;
    t.dispatches = s.dispatches;
    t.batches = s.batches;
    t.blocks = cp_->Stats().waits_blocked;
    const auto tr = frontend_->Translations();
    t.shaders = tr.count;
    t.shader_ns = tr.ns;
    const auto& p = pipelines_->Stats();
    t.pipelines = p.graphics + p.compute;
    t.pipeline_ns = p.create_ns;
    t.module_ns = p.module_ns;
    t.shader_disk = frontend_->DiskLoads();
    t.callbacks_ns = GS::TicksToNs(s.t_cb);
    if (parallel_) { // (the flip marker is executed with the queue drained: thread 2's totals are settled)
        const ParallelTotals pt = parallel_->Totals();
        t.par_wait_ns = GS::TicksToNs(pt.t1_drain_ticks + pt.t1_full_ticks);
        t.par_busy_ns = GS::TicksToNs(pt.t2_busy_ticks);
        t.par_idle_ns = GS::TicksToNs(pt.t2_idle_ticks);
        t.par_drains = pt.drains;
        t.par_records = pt.records;
        t.par_depth_sum = pt.depth_sum;
    }
    FrameLog::CpFlip(gpu_value, t, executor_->LastUnblockTsc());
    if (StallTrace::On()) { // (P4.12: the trace's events count to the next frame from here)
        const u32 n = FrameLog::GetProgress().cp - 1;
        StallTrace::Instant(StallTrace::Ev::FlipMarker, gpu_value, n);
        StallTrace::CpFrame(n);
    }
    if (Inspector::On()) {
        Inspector::CpTotals it;
        const u64 partial = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - round_t0_).count());
        it.pre_ns = tot_pre_ns_;
        it.round_ns = tot_round_ns_ + partial; // (the flip marker is inside a round: its time so far)
        it.poll_ns = tot_poll_ns_;
        it.flush_ns = tot_flush_ns_;
        it.idle_label_ns = tot_blocked_label_ns_;
        it.idle_other_ns = tot_blocked_other_ns_;
        it.idle_empty_ns = tot_empty_ns_;
        it.idle_gpu_ns = tot_idle_gpu_ns_;
        for (size_t i = 0; i < 3; ++i) {
            it.sync_ns[i] = s.sync_eff_us[i] * 1000;
            it.syncs[i] = s.sync_eff[i];
        }
        it.submit_ns = s.submit_ns;
        const auto& bs = buffers_->Stats();
        const auto is = images_->Stats();
        it.wb_bytes = bs.writeback_bytes;
        it.buf_upload_bytes = bs.upload_bytes;
        it.buf_resident_bytes = bs.resident_bytes;
        it.img_uploads = is.uploads;
        it.img_upload_bytes = is.upload_bytes;
        it.img_created = is.created;
        it.img_resident_bytes = is.bytes;
        it.streamed_bytes = s.streamed_bytes;
        it.mirrored_bytes = ring_->MirroredBytes();
        it.barriers = s.global_barriers;
        it.render_passes = s.render_passes;
        it.stage_ticks = {s.t_front, s.t_bind, s.t_range, s.t_image, s.t_pipe,
                          s.t_targets, s.t_vertex, s.t_layout, s.t_emit, s.t_dispatch};
        for (u64& t : it.stage_ticks) {
            t *= GS::FineScale(); // (sampled timers: every N-th command's sums, P4.9)
        }
        it.categories = executor_->InspectorCategories();
        Inspector::CpFrame(FrameLog::GetProgress().cp - 1, it); // (CpFlip counted this frame)
    }
}

void Backend::Stop() {
    Inspector::StopSampling(); // while the sampled thread still exists
    sampler_.Stop(); // while the sampled thread still exists
    game_sampler_.Stop();
    raw_sampler_.Stop();
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (parallel_) {
        parallel_->Stop(); // (its queue was drained by the loop's last round)
    }
    if (pipelines_) {
        pipelines_->StopPrecompile(); // (P4.14: its workers; the pipelines they built stay until the factory goes)
    }
    VideoOut::Stop();
    Gnm::SetHost(nullptr);
    if (frontend_) {
        frontend_->Stop();
    }
    if (ctx_) {
        std::scoped_lock lock{ctx_->QueueMutex()};
        Vk::Fn().vkDeviceWaitIdle(ctx_->Device());
    }
    if (disk_) {
        disk_->SetPeriodic(nullptr, nullptr, 0);
    }
    if (pcache_ && pcache_->Save()) { // (P4.14: the driver's compiled pipelines for the next run)
        SSPC_LOG(GPU, Info, "pipeline cache: {:.1f} MiB saved", static_cast<double>(pcache_->SavedBytes()) / (1 << 20));
    }
    if (disk_) {
        if (pipelines_) {
            const auto used = pipelines_->UsedDescriptions();
            disk_->AddUsage(used); // (P4.14: the next start prepares these first)
        }
        disk_->Close(); // (what is queued written first)
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.logs_dir, ec);
    std::ofstream(options_.logs_dir / "gpu_native.txt", std::ios::trunc) << Report();
    FrameLog::Write(options_.logs_dir / "gpu_frames.csv");
    StallTrace::Stop(); // (P4.12; the backend's threads and the present thread stopped)
    ShaderTrace::Stop(); // (P4.14)
    if (Inspector::On() && executor_) {
        Inspector::WriteFile("pipelines.csv", executor_->InspectorPipelines());
        Inspector::WriteFile("gpu_native.txt", Report());
    }
    Inspector::Stop();
    if (sampler_.Running()) {
        std::ofstream(options_.logs_dir / "gpu_cpu_profile.txt", std::ios::trunc) << sampler_.Report(60);
    }
    if (raw_sampler_.Running()) {
        raw_sampler_.WriteRaw((options_.logs_dir / "cpu_samples_raw.txt").wstring());
    }
    if (game_sampler_.Running()) {
        std::ofstream(options_.logs_dir / "gpu_cpu_profile_game.txt", std::ios::trunc) << game_sampler_.Report(60);
    }
}

// ---- P4.14: the persistent shader cache, the driver's pipeline cache, the start-up precompiler

std::atomic<u64> g_startup_pause_ns{0};

/// The settings menu's requests (files the menu leaves in [ShaderCache] dir, applied at the next start, before anything
/// is open): clear_shaders.request - every generation's directory under `root`; clear_pipelines.request - their
/// vulkan.bin only. A directory another process holds (its lock file) is left.
void ApplyClearRequests(const std::filesystem::path& requests, const std::filesystem::path& root) {
    std::error_code ec;
    const bool shaders = std::filesystem::exists(requests / "clear_shaders.request", ec);
    const bool pipelines = std::filesystem::exists(requests / "clear_pipelines.request", ec);
    if (!shaders && !pipelines) {
        return;
    }
    u32 removed = 0, kept = 0;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory()) {
            continue;
        }
        if (std::filesystem::exists(e.path() / "lock", ec) && !std::filesystem::remove(e.path() / "lock", ec)) {
            ++kept; // (in use by another process)
            continue;
        }
        if (shaders) {
            removed += std::filesystem::remove_all(e.path(), ec) != static_cast<std::uintmax_t>(-1) ? 1 : 0;
        } else {
            std::filesystem::remove(e.path() / "vulkan.bin", ec);
            std::filesystem::remove(e.path() / "vulkan.bin.tmp", ec);
            ++removed;
        }
    }
    std::filesystem::remove(requests / "clear_shaders.request", ec);
    std::filesystem::remove(requests / "clear_pipelines.request", ec);
    SSPC_LOG(GPU, Warning, "shader cache: cleared as asked in the settings menu ({}): {} director{} {}{}",
             shaders ? "shaders and pipelines" : "the Vulkan pipeline cache", removed, removed == 1 ? "y" : "ies",
             shaders ? "removed" : "without vulkan.bin", kept != 0 ? fmt::format(", {} in use left", kept) : "");
}

/// Other generations' directories (another build or GPU profile): the three used last are kept (A/B builds), older
/// ones removed. Only directories with a cache file; never one in use.
void PruneGenerations(const std::filesystem::path& root, const std::string& current) {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> others;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory() || e.path().filename() == current) {
            continue;
        }
        const auto pack = e.path() / "shaders.pack";
        if (!std::filesystem::exists(pack, ec) || std::filesystem::exists(e.path() / "lock", ec)) {
            continue;
        }
        others.emplace_back(std::filesystem::last_write_time(pack, ec), e.path());
    }
    std::ranges::sort(others, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 3; i < others.size(); ++i) {
        std::filesystem::remove_all(others[i].second, ec);
        SSPC_LOG(GPU, Info, "shader cache: an old generation removed ({})", others[i].second.string());
    }
}

void Backend::StartShaderCache(const Options& o) {
    if (!o.shader_cache || o.shader_cache_dir.empty()) {
        SSPC_LOG(GPU, Info, "native backend: no persistent shader cache ([ShaderCache] enabled = false or "
                            "--no-shader-cache): shaders translated and pipelines created as they are met");
        return;
    }
    const auto root = o.shader_cache_dir / o.game / "native";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    ApplyClearRequests(o.shader_cache_dir, root);
    const std::string gen = GS::ShaderCacheGeneration(profile_);
    PruneGenerations(root, gen);
    auto disk = std::make_unique<GS::ShaderDiskCache>();
    std::string error;
    if (!disk->Open({.dir = root / gen, .generation = gen, .max_bytes = u64{o.cache_max_mib} << 20}, error)) {
        SSPC_LOG(GPU, Warning, "native backend: the shader cache could not be opened ({}): without it", error);
        return;
    }
    const auto ds = disk->Stats();
    SSPC_LOG(GPU, Info,
             "shader cache {} ({}): {:.1f} MiB, {} records - programs {}, modules {}, SPIR-V {}, pipelines {}; read in "
             "{:.0f} ms{}{}{}",
             (root / gen).string(), disk->Writable() ? "read/write" : "read only",
             static_cast<double>(ds.file_bytes) / (1 << 20), ds.records, ds.programs, ds.modules, ds.spirv, ds.pipelines,
             ds.open_ms, ds.recreated ? fmt::format(", started anew ({})", ds.recreate_reason) : "",
             ds.torn_bytes != 0 ? fmt::format(", {} bytes of a torn tail cut off", ds.torn_bytes) : "",
             ds.skipped_records != 0 ? fmt::format(", {} records not used", ds.skipped_records) : "");
    pcache_ = std::make_unique<Vk::PipelineCacheFile>(*ctx_, *disk);
    if (!pcache_->Load()) {
        SSPC_LOG(GPU, Warning, "pipeline cache (driver): {}", pcache_->LoadNote());
        pcache_.reset();
    } else {
        SSPC_LOG(GPU, Info, "pipeline cache (driver): {}", pcache_->LoadNote());
    }
    // the offline index of the game's programs (--scan-game-shaders): the cache's coverage of the game
    if (std::string ierr; game_index_.Load(root / "game_programs.idx", ierr)) {
        for (const GS::ProgramIdentity& id : disk->ProgramIdentities()) {
            cached_in_game_ += game_index_.Find(id.hash, id.length) != nullptr ? 1 : 0;
        }
        SSPC_LOG(GPU, Info, "game shader index: {} programs in {} packs; the cache has {} of them ({:.1f} %)",
                 game_index_.Programs().size(), game_index_.Packs().size(), cached_in_game_,
                 100.0 * static_cast<double>(cached_in_game_) /
                     static_cast<double>(std::max<size_t>(game_index_.Programs().size(), 1)));
    } else {
        SSPC_LOG(GPU, Info, "game shader index: {}", ierr);
    }
    disk_ = std::move(disk);
    pipelines_->SetPersistence(pcache_ ? pcache_->Handle() : VK_NULL_HANDLE, disk_.get());
    frontend_->SetDiskCache(disk_.get());
    pc_saved_at_ = std::chrono::steady_clock::now();
    if (pcache_ && disk_->Writable()) {
        disk_->SetPeriodic([](void* self) { static_cast<Backend*>(self)->MaybeSavePipelineCache(); }, this, 10000);
    }
    if (o.precompile) {
        RunPrecompile(o);
    } else {
        SSPC_LOG(GPU, Info, "shader precompiler: off ([ShaderCache] precompile = false)");
    }
}

void Backend::RunPrecompile(const Options& o) {
    // the pipelines the last runs used first (a skipped wait, or the time limit, leaves the likeliest ones ready)
    std::vector<GS::PipelineDesc> descs = disk_->PipelinesByUse();
    if (descs.empty()) {
        SSPC_LOG(GPU, Info, "shader precompiler: the manifest is empty (a first run): nothing to prepare");
        return;
    }
    Vk::PrecompileOptions po;
    po.threads = o.precompile_threads;
    pipelines_->StartPrecompile(std::move(descs), po);
    Vk::PipelinePrecompiler* pre = pipelines_->Precompiler();
    if (pre == nullptr) {
        return;
    }
    // the game starts once they are ready (or the user skips the wait, or the time limit): the start waits here,
    // the host shows the progress
    const auto t0 = std::chrono::steady_clock::now();
    bool skipped = false, timed_out = false;
    Options::PrecompileStatus st;
    while (true) {
        const Vk::PrecompileProgress p = pre->Progress();
        st = {p.total, p.Done(), p.failed, p.elapsed_ms / 1000.0, p.finished};
        if (p.finished) {
            break;
        }
        if (o.precompile_progress != nullptr && o.precompile_progress(o.precompile_progress_ctx, st)) {
            skipped = true;
            break;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > o.precompile_max_s) {
            timed_out = true;
            break;
        }
        Sleep(o.precompile_progress != nullptr ? 30 : 20);
    }
    st.finished = true;
    if (o.precompile_progress != nullptr) {
        o.precompile_progress(o.precompile_progress_ctx, st); // (the host hides it)
    }
    pre->SetBackground();
    g_startup_pause_ns.store(static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    const Vk::PrecompileProgress p = pre->Progress();
    SSPC_LOG(GPU, Info, "shader precompiler: {} of {} pipelines ready in {:.0f} ms ({} failed, {} without their SPIR-V){}",
             p.built, p.total, p.elapsed_ms, p.failed, p.missing_spirv,
             skipped     ? std::string(" - skipped by the user: the rest is built in the background")
             : timed_out ? fmt::format(" - not finished after {:.0f} s: the rest is built in the background",
                                       o.precompile_max_s)
                         : std::string());
}

void Backend::MaybeSavePipelineCache() {
    // saved when pipelines were added since the last save and none in the last 10 s (the driver's cache is copied while
    // it is quiet: a save during a burst of creations could hold its lock), at most once a minute
    const Vk::PipelinePrecompiler* pre = pipelines_->Precompiler();
    const u64 count = pipelines_->CreatedOnDemand() + (pre != nullptr ? pre->Progress().built : 0);
    const u64 prev = pc_prev_count_.exchange(count);
    if (count == pc_saved_count_.load() || count != prev ||
        std::chrono::steady_clock::now() - pc_saved_at_ < std::chrono::seconds(60)) {
        return;
    }
    if (pcache_->Save()) {
        pc_saved_count_.store(count);
        pc_saved_at_ = std::chrono::steady_clock::now();
    }
}

std::string Backend::CacheReport() {
    if (!disk_) {
        return "persistent shader cache (P4.14): off\n";
    }
    const auto ds = disk_->Stats();
    const auto& fs = frontend_->Stats();
    const auto& ps = pipelines_->Stats();
    std::string out = fmt::format(
        "persistent shader cache (P4.14): {} - {:.1f} MiB; at start {} programs, {} modules, {} SPIR-V, {} pipelines "
        "(read in {:.0f} ms{}); this run: modules read {} (unusable {}, translation time saved as recorded {:.0f} ms), "
        "translated {}; written {} records {:.1f} MiB (modules {}, pipelines {}, SPIR-V {}), not added (size limit) {}, "
        "write errors {}\n",
        disk_->Dir().string(), static_cast<double>(ds.file_bytes) / (1 << 20), ds.programs, ds.modules, ds.spirv,
        ds.pipelines, ds.open_ms, ds.torn_bytes != 0 ? fmt::format(", torn tail {} bytes cut", ds.torn_bytes) : "",
        fs.disk_loads, fs.disk_load_failures, static_cast<double>(fs.disk_saved_us) / 1000.0,
        frontend_->Translations().count, ds.written_records, static_cast<double>(ds.written_bytes) / (1 << 20),
        ds.written_modules, ds.written_pipelines, ds.written_spirv, ds.dropped_full, ds.write_errors);
    out += fmt::format("pipelines on demand: created {} (driver cache feedback {}: hits {}, misses {}), the same "
                       "description again {}, taken precompiled {} (waited for {} of them, {:.1f} ms)\n",
                       ps.graphics + ps.compute, ps.feedback, ps.cache_hits, ps.feedback - ps.cache_hits, ps.desc_reuse,
                       ps.prebuilt, ps.prebuilt_waited, static_cast<double>(ps.prebuilt_wait_ns) / 1e6);
    if (const Vk::PipelinePrecompiler* pre = pipelines_->Precompiler()) {
        const auto p = pre->Progress();
        out += fmt::format("start-up precompiler: {} pipelines in the manifest, built {} ({:.0f} ms of the workers' time, "
                           "{:.0f} ms wall), failed {} (SPIR-V missing {}), claimed by the game first {}, taken {}, "
                           "never taken {}\n",
                           p.total, p.built, static_cast<double>(p.build_ns) / 1e6, p.elapsed_ms, p.failed,
                           p.missing_spirv, p.claimed, p.taken, pre->NeverTaken());
    }
    if (pcache_) {
        out += fmt::format("pipeline cache (driver): at start {}; saved {} times, last {:.1f} MiB\n", pcache_->LoadNote(),
                           pcache_->Saves(), static_cast<double>(pcache_->SavedBytes()) / (1 << 20));
    }
    if (!game_index_.Empty()) {
        // the packs this run's programs are in (observed, not guessed from the packs' names)
        std::unordered_map<u32, u64> by_pack;
        u64 met = 0, found = 0;
        for (const GS::Program* p : frontend_->Programs().All()) {
            ++met;
            if (const GS::GameProgram* g = game_index_.Find(p->id.hash, p->id.length)) {
                ++found;
                for (u32 i = 0; i < g->members; ++i) {
                    ++by_pack[game_index_.Members()[g->first_member + i].pack];
                }
            }
        }
        std::vector<std::pair<u64, u32>> order;
        for (const auto& [pack, n] : by_pack) {
            order.emplace_back(n, pack);
        }
        std::ranges::sort(order, std::greater<>{});
        out += fmt::format("game programs (offline index): {} in {} packs; the cache had {} at start ({:.1f} %); this run "
                           "met {} programs, {} of them in the packs, in {} packs; the packs with most:",
                           game_index_.Programs().size(), game_index_.Packs().size(), cached_in_game_,
                           100.0 * static_cast<double>(cached_in_game_) /
                               static_cast<double>(std::max<size_t>(game_index_.Programs().size(), 1)),
                           met, found, order.size());
        for (size_t i = 0; i < order.size() && i < 12; ++i) {
            out += fmt::format(" {} {};", game_index_.Packs()[order[i].second], order[i].first);
        }
        out += "\n";
    }
    return out;
}

ShaderCacheInfo Backend::CacheInfo() {
    ShaderCacheInfo info;
    if (!disk_) {
        return info;
    }
    const auto ds = disk_->Stats();
    info.enabled = true;
    info.dir = disk_->Dir().string();
    info.file_bytes = ds.file_bytes;
    std::error_code ec;
    const auto vb = std::filesystem::file_size(disk_->Dir() / "vulkan.bin", ec);
    info.vulkan_bytes = ec ? 0 : vb;
    info.programs = ds.programs + ds.written_programs;
    info.modules = ds.modules + ds.written_modules;
    info.spirv = ds.spirv + ds.written_spirv;
    info.pipelines = ds.pipelines + ds.written_pipelines;
    info.module_loads = frontend_->DiskLoads();
    info.translations = frontend_->Translations().count;
    if (const Vk::PipelinePrecompiler* pre = pipelines_->Precompiler()) {
        const auto p = pre->Progress();
        info.precompile_total = p.total;
        info.precompile_built = p.built;
        info.precompile_taken = p.taken;
        info.precompile_ms = p.elapsed_ms;
    }
    info.created_on_demand = pipelines_->CreatedOnDemand();
    info.game_programs = game_index_.Programs().size();
    info.cached_in_game = cached_in_game_;
    return info;
}

std::string Backend::Report() {
    std::string out = failure_result_.load() != VK_SUCCESS
                          ? fmt::format("STATUS: FAILED - VkResult {}\n", static_cast<int>(failure_result_.load()))
                      : parallel_ && parallel_->Failed()
                          ? "STATUS: FAILED - " + parallel_->FailureText() + "\n"
                          : "STATUS: running/stopped normally\n";
    out += fmt::format("SecondSonPC native GPU backend (P4.4)\ndevice: {}\nsubmissions: {}, "
                                  "command processor errors: {} {}\n",
                                  ctx_ ? ctx_->Name() : "-", submissions_, cp_errors_, first_cp_error_);
    if (parallel_) {
        out += parallel_->Report();
    }
    if (executor_) {
        const auto& s = executor_->Stats();
        out += fmt::format("the processor's other callbacks (P4.10): {} calls, {:.0f} ms; recording {:.0f} ms; draws/"
                           "dispatches without the preparation their registers ask for {}\n",
                           s.n_cb, static_cast<double>(GS::TicksToNs(s.t_cb)) / 1e6,
                           static_cast<double>(s.record_ns) / 1e6, s.prepared_mismatches);
    }
    if (executor_) {
        const auto& s = executor_->Stats();
        out += fmt::format("draws {} (executed {}), dispatches {} (executed {}), batches {}, completed {}, flips {} "
                           "(blank {}), syncs {} ({} us waiting), render passes {}, global barriers {}, fast clears {}, "
                           "depth clears {} (of part of the target {}), metadata writes {} (xor {}, reading metadata {}), "
                           "cache actions {}\n",
                           s.draws, s.draws_executed, s.dispatches, s.dispatches_executed, s.batches,
                           s.completed_batches, s.flips, s.flips_blank, s.syncs, s.sync_wait_us, s.render_passes,
                           s.global_barriers, s.fast_clears, s.depth_clears, s.depth_clears_partial, s.meta_writes,
                           s.meta_writes_xor, s.meta_writes_reading, s.cache_actions);
        for (size_t i = 0; i < s.skipped.size(); ++i) {
            if (s.skipped[i] != 0) {
                out += fmt::format("  not executed ({}): {} — first: {}\n", Vk::SkipName(static_cast<Vk::Skip>(i)),
                                   s.skipped[i], s.first_skip[i]);
            }
        }
        if (s.diag_skipped_draws + s.diag_skipped_dispatches != 0) {
            out += fmt::format("  DIAGNOSTIC: left out by SSPC_DIAG_SKIP_DRAWS {} draws, SSPC_DIAG_SKIP_DISPATCHES {} "
                               "dispatches\n",
                               s.diag_skipped_draws, s.diag_skipped_dispatches);
        }
        if (s.merge_split_dispatches != 0) {
            out += fmt::format("  particle sort (SSPC-020): {} dispatches recorded one merge width each\n",
                               s.merge_split_dispatches);
        }
    }
    if (images_) {
        const auto i = images_->Stats();
        out += fmt::format("images {} ({} MiB), uploads {} (detiled {}, linear {}, {} MiB), overlap hits {}, target misses {}, overlap misses {}, unsupported {} {}\n",
                           i.images, i.bytes >> 20, i.uploads, i.detiles, i.linear_uploads, i.upload_bytes >> 20,
                           i.overlap_hits, i.target_misses, i.overlap_misses, i.unsupported, i.first_unsupported);
        out += fmt::format("images evicted over the budget: {} ({} MiB); GPU-written: dropped (memory newer) {}, written "
                           "back and freed {} ({} MiB), old but kept (no write-back path) {}\n",
                           i.evicted, i.evicted_bytes >> 20, i.gpu_dropped, i.gpu_written_back,
                           i.gpu_written_back_bytes >> 20, i.gpu_kept_no_path);
        out += fmt::format("GPU-written images whose memory was written since: {} (exact {}, buffer write at the base {}, "
                           "CPU {}, unmapped {}), targets uploaded first {}; expanded {}, block-compressed copies {}; "
                           "superseded {}; retired {} ({} MiB); write-tracked {}, not trackable {}; stencil views {}\n",
                           i.memory_newer, i.memory_newer_by[0], i.memory_newer_by[1], i.memory_newer_by[2],
                           i.memory_newer_by[3], i.target_uploads, i.expanded, i.block_copies, i.superseded, i.retired,
                           i.retired_bytes >> 20, i.gpu_tracked, i.gpu_untracked, i.stencil_views);
    }
    if (guest_) {
        const auto g = guest_->GetStats();
        out += fmt::format("guest buffer windows {} ({} MiB; through the backing {}, the guest's pages {}), imports {}, failures {} {}\n", g.windows, g.bytes >> 20, g.backing_windows, g.guest_windows,
                           g.imports, g.import_failures, g.first_failure);
    }
    if (cp_ && executor_) {
        const auto& c = cp_->Stats();
        const auto& s = executor_->Stats();
        out += fmt::format("target cache: hits {}, misses {}; texture memo: hits {}, misses {}\n",
                           s.target_cache_hits, s.target_cache_misses, s.tex_memo_hits, s.tex_memo_misses);
        out += cp_->WaitReport();
        out += cp_->FenceReport();
        const VideoOut::Stats vo = VideoOut::GetStats();
        out += fmt::format("videoout: flips requested {}, presented {}, vblanks {}, vblanks a flip waited for its GPU work "
                           "{}\n",
                           vo.flips_requested, vo.flips_presented, vo.vblanks, vo.waited_vblanks);
        out += fmt::format("frontend reads that waited for a shader's write: {}\n", s.gpu_write_read_syncs);
        out += fmt::format("frontend reads (P4.7): from a kept backing run {}, a run found {}, through the memory "
                           "manager {}\n",
                           s.read_run_hits, s.read_run_misses, s.read_run_none);
        if (s.read_run_verified != 0) {
            out += fmt::format("backing run read verification (SSPC_DIAG_READ_RUNS=verify): reads compared {}, "
                               "different {}\n",
                               s.read_run_verified, s.read_run_mismatches);
        }
        out += fmt::format("graphics bindings (P4.7 shadow): descriptors wanted {}, pushed {}, pushes left out {}; "
                           "vertex buffer bindings left out {}, index buffer {}\n",
                           s.desc_wanted, s.desc_pushed, s.desc_push_skipped, s.vb_binds_skipped, s.ib_binds_skipped);
        out += fmt::format("streamed copies reused in their batch (P4.7): ranges {} ({} MiB)\n", s.stream_reused,
                           s.stream_reused_bytes >> 20);
        if (s.stream_verified != 0) {
            out += fmt::format("streamed copy reuse verification (SSPC_DIAG_STREAM_REUSE=verify): reuses copied again "
                               "{}, different bytes {}\n",
                               s.stream_verified, s.stream_mismatches);
        }
        out += fmt::format("memory waits met: {} (had blocked {}), CPU writes applied at them {}; met in the GPU's "
                           "order by a held fence {}\n",
                           s.acquires, s.acquires_blocked, s.acquires_applied, c.waits_met_held);
        out += fmt::format("global barriers by site: other {}, shadows in {}+{}, shadows out {}, timeline write {}, timeline "
                           "copy {}, upload {}, before a draw {}, before a dispatch {}\n",
                           s.gb_sites[0], s.gb_sites[1], s.gb_sites[2], s.gb_sites[3], s.gb_sites[4], s.gb_sites[5],
                           s.gb_sites[6], s.gb_sites[7], s.gb_sites[8]);
        out += fmt::format("barriers by ranges: elided {} (dispatches/timeline copies touching nothing written since), "
                           "write-after-read {}; stream ring mirrored {} MiB in {} copies\n",
                           s.barriers_elided, s.barriers_war, ring_->MirroredBytes() >> 20, s.ring_mirror_copies);
        out += fmt::format("small read-only ranges cached instead of streamed: {}\n", s.stream_promotions);
        out += fmt::format("draws repeating the bound state (not recorded again): pipeline {}, push constants {}\n",
                           s.binds_skipped, s.push_constants_skipped);
        out += fmt::format("write-backs by batch end: fence/flip/sync/idle {} MiB, full with a fence {} MiB, full "
                           "without {} MiB (full batches {})\n",
                           s.wb_bytes_by[0] >> 20, s.wb_bytes_by[1] >> 20, s.wb_bytes_by[2] >> 20, s.flushes_full);
        out += fmt::format("barriers kept by ranges: off {}, unknown write pending {}, own unknown access {}, too many "
                           "ranges {}, read-after-write {}, write-after-write {}, write-after-read {}\n",
                           s.barrier_why[0], s.barrier_why[1], s.barrier_why[2], s.barrier_why[3], s.barrier_why[4],
                           s.barrier_why[5], s.barrier_why[6]);
        out += fmt::format("write-after-write barriers where both write GDS: {}\n", s.barrier_waw_gds);
        out += fmt::format("SSPC-020 diagnostics (SSPC_DIAG_CLOCK=labels): shader bindings over a GPU clock label "
                           "(EOP/RELEASE_MEM data_sel 3/4): {}\n",
                           s.clock_label_binds);
        out += fmt::format("held GPU clock fences with a timestamp at their place {}, without {}\n",
                           s.clock_fences_placed, s.clock_fences_unplaced);
        out += fmt::format("dispatches barriered for a range conflict: the guest flushed on the queue since its last "
                           "dispatch {}, did not {}; guest events:",
                           s.conflict_guest_flushed, s.conflict_guest_silent);
        out += fmt::format(" (barriers left out for dispatches the guest does not order: {})", s.barriers_guest_order);
        for (u32 t = 0; t < 64; ++t) {
            if (s.guest_events[t] != 0) {
                out += fmt::format(" {:#x}={}", t, s.guest_events[t]);
            }
        }
        out += "\n";
        out += fmt::format("writes of unknown extent: draws {}, dispatches {}, met waits {}, WRITE_DATA {}, GDS DMA {}, "
                           "evictions {}, other {}\n",
                           s.unknown_by[0], s.unknown_by[1], s.unknown_by[2], s.unknown_by[3], s.unknown_by[4],
                           s.unknown_by[5], s.unknown_by[6]);
        if (s.stream_kinds[0] != 0) {
            out += fmt::format("streamed ranges (SSPC_DIAG_STREAM_STATS): new {} ({} MiB), again in the frame unchanged {} "
                               "({} MiB), again changed {} ({} MiB), unchanged since an earlier frame {} ({} MiB), "
                               "changed since {} ({} MiB)\n",
                               s.stream_kinds[0], s.stream_kind_bytes[0] >> 20, s.stream_kinds[1],
                               s.stream_kind_bytes[1] >> 20, s.stream_kinds[2], s.stream_kind_bytes[2] >> 20,
                               s.stream_kinds[3], s.stream_kind_bytes[3] >> 20, s.stream_kinds[4],
                               s.stream_kind_bytes[4] >> 20);
        }
        out += fmt::format("write shadows {} ({} MiB; left in place: overlapping {}, no room {}, too large {}); written "
                           "dispatch bindings <=64 KiB {}, <=256 KiB {}, <=1 MiB {}, <=4 MiB {}, <=16 MiB {}, larger {}\n",
                           s.shadows, s.shadow_bytes >> 20, s.shadow_overlaps, s.shadow_no_room, s.shadow_too_large,
                           s.shadow_sizes[0], s.shadow_sizes[1], s.shadow_sizes[2], s.shadow_sizes[3],
                           s.shadow_sizes[4], s.shadow_sizes[5]);
        out += fmt::format("timeline copies {} ({} MiB, refused {})\n", s.timeline_copies, s.timeline_copy_bytes >> 20,
                           s.tc_refused);
        out += fmt::format("waits that waited: processor packets {} ({} ms), frontend reads {} ({} ms), processor memory "
                           "reads {} ({} ms)\n",
                           s.sync_eff[0], s.sync_eff_us[0] / 1000, s.sync_eff[1], s.sync_eff_us[1] / 1000, s.sync_eff[2],
                           s.sync_eff_us[2] / 1000);
        out += fmt::format("command processor waits for the GPU: WRITE_DATA {} (timeline refusals: shape {}, import {}), "
                           "DMA by kind [m2m {}, data2mem {}, m2gds {}, data2gds {}, gds2mem {}]; m2m sizes "
                           "<=256 B {}, <=4 KiB {}, <=64 KiB {}, larger {}\n",
                           c.sync_write_data, s.tw_fail_shape, s.tw_fail_import, c.sync_dma[0], c.sync_dma[1],
                           c.sync_dma[2], c.sync_dma[3], c.sync_dma[4], c.sync_dma_m2m_size[0], c.sync_dma_m2m_size[1],
                           c.sync_dma_m2m_size[2], c.sync_dma_m2m_size[3]);
        out += fmt::format("  their time (ms): WRITE_DATA {}, DMA [m2m {}, data2mem {}, m2gds {}, data2gds {}, gds2mem {}]; "
                           "WRITE_DATA to VideoOut labels without a wait {}; GDS DMA on the timeline {} (refused {}); "
                           "fences with GDS data {}\n",
                           c.sync_write_data_ns / 1'000'000, c.sync_dma_ns[0] / 1'000'000, c.sync_dma_ns[1] / 1'000'000,
                           c.sync_dma_ns[2] / 1'000'000, c.sync_dma_ns[3] / 1'000'000, c.sync_dma_ns[4] / 1'000'000,
                           c.unordered_writes, c.timeline_gds, s.gds_refused, c.gds_fences);
    }
    if (buffers_) {
        const auto& b = buffers_->Stats();
        out += fmt::format("buffer cache: {} resident {} MiB ({} binds in {} calls), uploads {} ({} MiB), write-backs {} "
                           "({} MiB), invalidations {}, uncached {} (large ranges {}, early write-backs {}), over budget {} {}\n",
                           buffers_->Ok() ? "on," : "OFF,", b.resident_bytes >> 20, b.binds, b.bind_calls, b.uploads,
                           b.upload_bytes >> 20, b.writebacks, b.writeback_bytes >> 20, b.invalidations, b.uncached, b.large_ranges, b.early_writebacks,
                           b.over_budget, b.first_error);
        out += fmt::format("write-back ranges <=4 KiB {} ({} MiB), <=64 KiB {} ({} MiB), <=1 MiB {} ({} MiB), <=4 MiB {} "
                           "({} MiB), larger {} ({} MiB); copy commands {}\n",
                           b.wb_hist_count[0], b.wb_hist_bytes[0] >> 20, b.wb_hist_count[1], b.wb_hist_bytes[1] >> 20,
                           b.wb_hist_count[2], b.wb_hist_bytes[2] >> 20, b.wb_hist_count[3], b.wb_hist_bytes[3] >> 20,
                           b.wb_hist_count[4], b.wb_hist_bytes[4] >> 20, b.wb_commands);
        out += fmt::format("write-backs of changed words only (large ranges): {} MiB compared in {} dispatches, shadows "
                           "made from the host {}; prepares answered by the batch's memo {}\n",
                           b.wb_diff_bytes >> 20, b.wb_diff_dispatches, b.wb_shadow_inits, b.prep_memo_hits);
        out += fmt::format("uploads by bindings (P4.9): prepares that uploaded {}, pages brought along with their block's "
                           "first missing page {} (blocks not importable whole {})\n",
                           b.n_upload_calls, b.block_upload_pages, b.block_upload_refused);
        out += fmt::format("lookup memos (P4.9): {}; verified hits {}, mismatches {}\n",
                           GS::LookupMemoOn() ? (GS::LookupMemoVerify() ? "on, verify" : "on") : "off",
                           GS::LookupMemoCounts().verified.load(), GS::LookupMemoCounts().mismatches.load());
        if (b.wb_checked_bytes != 0) {
            out += fmt::format("write-backs checked (SSPC_DIAG_WB_DIRTY): {} MiB, changed 256-byte chunks {} ({} MiB), "
                               "changed words {} ({} KiB)\n",
                               b.wb_checked_bytes >> 20, b.wb_dirty_chunks, (b.wb_dirty_chunks * 256) >> 20,
                               b.wb_dirty_words, (b.wb_dirty_words * 4) >> 10);
        }
    }
    if (pipelines_) {
        const auto& p = pipelines_->Stats();
        out += fmt::format("pipelines: graphics {}, compute {}, modules {}, failures {} {}\n", p.graphics, p.compute,
                           p.modules, p.failures, p.first_failure);
        const auto& h = p.create_hist;
        out += fmt::format("pipeline creation (driver): {:.1f} ms in all; by time <1 ms {}, 1-5 {}, 5-20 {}, 20-50 {}, "
                           "50-100 {}, >=100 {}; slowest {:.1f} ms ({}); shader modules + auxiliary stages {:.1f} ms\n",
                           p.create_ns / 1e6, h[0], h[1], h[2], h[3], h[4], h[5], p.slowest_ns / 1e6, p.slowest,
                           p.module_ns / 1e6);
    }
    if (frontend_) {
        const auto tr = frontend_->Translations();
        out += fmt::format("shader translations (GCN -> SPIR-V): {}, {:.1f} ms in all, slowest {:.1f} ms\n", tr.count,
                           tr.ns / 1e6, tr.max_ns / 1e6);
        out += CacheReport();
    }
    if (frontend_) {
        const auto& fs = frontend_->Stats();
        out += fmt::format("stage preparations (P4.7 memo): taken again {}, prepared: memory changed since {}, other user "
                           "data {}, other inputs {}; draws {}, dispatches {}, SRT reads {}\n",
                           fs.memo_hits, fs.memo_miss_memory, fs.memo_miss_user_data, fs.memo_miss_other, fs.draws,
                           fs.dispatches, fs.srt_reads);
        if (fs.memo_verified != 0) {
            out += fmt::format("stage memo verification (SSPC_DIAG_STAGE_MEMO=verify): hits prepared in full {}, "
                               "different {}\n",
                               fs.memo_verified, fs.memo_mismatches);
        }
        out += frontend_->NativeReport(); // (P5.3; empty when off)
        out += frontend_->MemoReport();
    }
    {
        const auto t = tracker_.GetStats();
        out += fmt::format("write tracking: armed pages {}, arms {}, disarms {}, write faults {} (raced {}), never-armed blocks {}, forgotten "
                           "pages {}\n",
                           t.armed_pages, t.arms, t.disarms, t.faults, t.faults_raced, t.never_armed_blocks, t.forgotten);
    }
    if (presenter_) {
        const auto p = presenter_->GetStats();
        out += fmt::format("presented {} (black {}), copies {}, captures {}, swapchain recreated {}{}\n", p.presented,
                           p.black, p.copies, p.captures, p.swapchain_recreated, p.device_lost ? ", DEVICE LOST" : "");
    }
    if (frontend_) { // (P5.2: the shader frontend's own counters - programs read again per frame, fetch shaders, ...)
        out += "--- shader frontend ---\n" + frontend_->Report();
    }
    return out;
}

Backend* g_backend = nullptr;

} // namespace

bool Start(const Options& options, std::string& error) {
    if (g_backend != nullptr) {
        error = "the native backend is already running";
        return false;
    }
    static Backend backend; // lives for the process: the memory listener slot must never dangle
    if (!backend.Start(options, error)) {
        return false;
    }
    g_backend = &backend;
    return true;
}

void Stop() {
    if (g_backend != nullptr) {
        g_backend->Stop();
    }
}

u64 StartupPauseNs() {
    return g_startup_pause_ns.load();
}

ShaderCacheInfo CacheInfo() {
    return g_backend != nullptr ? g_backend->CacheInfo() : ShaderCacheInfo{};
}

bool Running() {
    return g_backend != nullptr;
}

std::string Report() {
    return g_backend != nullptr ? g_backend->Report() : std::string("the native backend is not running\n");
}

} // namespace SecondSon::Gpu::Native
