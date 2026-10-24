// SecondSonPC — the backend's Vulkan pipelines (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §D): shader
// modules from the frontend's translations (P4.3), the descriptor set and pipeline layouts from the draw's binding
// plans (one set, push descriptors when the bindings fit, the translator's push constants), graphics pipelines from
// GraphicsPipelineState as the bridge's renderer builds them (the same SPIR-V needs the same fixed function: dynamic
// rendering formats, blending with its GCN adjustments, rect/quad lists through the translator's tessellation
// stages, everything else dynamic), compute pipelines. Created on first use, kept until the backend stops.
//
// P4.14 (docs/PHASE4_SHADER_PRECOMPILER.md): every pipeline is first described without a handle or a process-local id
// (GS::PipelineDesc: the stages by SPIR-V content, the state, the bindings) and built from that description only
// (Build: also on the precompiler's workers). A description met before in this run, or built by the start-up
// precompiler, is taken instead of created again; one created now goes through the driver's VkPipelineCache and into
// the persistent manifest.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/r10/work_pool.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_precompile.h"
#include "runtime/gpu/u64_hash.h"

namespace SecondSon::Gpu::Vk {

namespace GS = Gpu::Shader;

class DeviceContext;

/// The translator's push constants (the bridge's GS::PushData, byte for byte).
struct PushData {
    float xoffset = 0.0f, yoffset = 0.0f, xscale = 1.0f, yscale = 1.0f;
    std::array<u8, 56> buf_offsets{};
};
static_assert(sizeof(PushData) == 72);

struct PipelineObjects {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    bool push_descriptors = true;
    u32 bindings = 0;
    bool compute = false;
    bool shared_layout = false; ///< the layouts belong to the factory's table (P4.7), not to this pipeline
};

struct PipelineStats {
    u64 graphics = 0, compute = 0, modules = 0, failures = 0, lookups = 0, aux_stages = 0;
    std::string first_failure;
    // the time it cost (docs/SHADER_PRECOMPILATION.md): vkCreate*Pipelines, vkCreateShaderModule + auxiliary stages
    u64 create_ns = 0, module_ns = 0;
    u64 slowest_ns = 0;  ///< the slowest single pipeline
    std::string slowest; ///< ... which one
    std::array<u64, 6> create_hist{}; ///< pipelines by creation time: <1, <5, <20, <50, <100, >=100 ms
    // P4.14
    u64 desc_reuse = 0;      ///< another pipeline id with a description made already in this run
    u64 prebuilt = 0;        ///< taken from the start-up precompiler
    u64 prebuilt_waited = 0; ///< ... still being built by a worker: waited for
    u64 prebuilt_wait_ns = 0;
    u64 feedback = 0, cache_hits = 0; ///< created on demand with the driver's creation feedback / application cache hits
    /// SSPC_ASYNC_PIPELINES: graphics pipelines handed to the background workers, draws skipped while one was being
    /// built, built / failed there
    u64 async_queued = 0, async_skips = 0, async_built = 0, async_failed = 0;
    /// R9-N SSPC_PIPE_PREFETCH: compute pipelines a worker built before their dispatch was recorded (taken), of them
    /// still being built when it was (waited for); not started yet - built by the recording thread itself (claimed);
    /// failed on the worker (built again by the recording thread)
    u64 prefetch_taken = 0, prefetch_waited = 0, prefetch_claimed = 0, prefetch_failed = 0;
    u64 prefetch_wait_ns = 0;  ///< the recording thread's waits for the workers (what a frame still stalls)
    u64 prefetch_build_ns = 0; ///< the workers' creation time of the pipelines taken
    u64 prefetch_slowest_ns = 0;
};

class PipelineFactory {
public:
    PipelineFactory(DeviceContext& ctx, const GS::DeviceProfile& profile);
    ~PipelineFactory(); ///< stops the precompiler first

    /// `pipeline_id`: the frontend's PipelineStateCache id; `state` its state; the stages' modules and plans as the
    /// frontend prepared them for this draw (vs required, ps may be null). nullptr with `error`.
    const PipelineObjects* Graphics(u32 pipeline_id, const GS::GraphicsPipelineState& state,
                                    const GS::Module* vs, const GS::ResourceBindingPlan& vs_plan,
                                    const GS::Module* ps, const GS::ResourceBindingPlan* ps_plan,
                                    std::string& error);
    const PipelineObjects* Compute(const GS::Module& cs, const GS::ResourceBindingPlan& plan, std::string& error);

    /// R9-N (SSPC_PIPE_PREFETCH=1|<threads>, docs/R9_NEON_DASH.md): the compute pipeline of a dispatch the frontend
    /// just prepared, when nothing has built it yet, started on worker threads now - the frontend runs ahead of the
    /// recording thread, so the new pipelines of a burst (an effect met the first time) are built side by side instead
    /// of one after another. Compute takes the result, waiting for the worker if it is still building: the dispatch is
    /// never skipped. The frontend's thread (or the processor's without one), never the recording thread.
    void PrefetchCompute(const GS::Module& cs, const GS::ResourceBindingPlan& plan);
    bool Prefetching() const { return prefetch_; }
    /// Pipelines handed to the prefetch workers so far (any thread).
    u64 PrefetchQueued() const { return pf_queued_.load(std::memory_order_relaxed); }
    /// R10: the pool the background jobs run on (nullptr: the factory's own threads), its refused submissions.
    const R10::WorkPool* Pool() const { return pool_; }
    u64 PoolRefused() const { return pool_refused_.load(std::memory_order_relaxed); }

    const PipelineStats& Stats() const { return stats_; }

    // ---- P4.14
    /// The driver's pipeline cache for every creation (VK_NULL_HANDLE: none) and the persistent manifest the pipelines
    /// created on demand are added to (nullptr: none). Before the first draw.
    void SetPersistence(VkPipelineCache cache, GS::ShaderDiskCache* disk);
    /// Starts building `descs` on worker threads (the recording thread takes them from it). Before the first draw.
    void StartPrecompile(std::vector<GS::PipelineDesc> descs, const PrecompileOptions& options);
    PipelinePrecompiler* Precompiler() { return pre_.get(); }
    /// Cancels and joins the precompiler's workers (the pipelines they built stay until the factory goes).
    void StopPrecompile();
    /// Pipelines created on demand so far (any thread: the periodic save of the pipeline cache looks at it).
    u64 CreatedOnDemand() const { return created_.load(std::memory_order_relaxed); }
    /// The descriptions of every pipeline this run used (created, precompiled and taken, made again). After the
    /// recording thread stopped.
    std::vector<u64> UsedDescriptions() const;

    /// What a draw's / dispatch's pipeline is, without handles or ids.
    static GS::PipelineDesc DescribeGraphics(const GS::GraphicsPipelineState& state, const GS::Module* vs,
                                             const GS::ResourceBindingPlan& vs_plan, const GS::Module* ps,
                                             const GS::ResourceBindingPlan* ps_plan);
    static GS::PipelineDesc DescribeCompute(const GS::Module& cs, const GS::ResourceBindingPlan& plan);
    /// A pipeline from its description and its stages' modules (ps: VK_NULL_HANDLE without one). Any thread: it
    /// touches nothing of the factory but the auxiliary stages' table (locked). `flags`: ShaderTrace::kPipe* (driver
    /// feedback). false + `error`: nothing left behind in `out`. `create_flags`: VkPipelineCreateFlags of the create info
    /// (R9-N: VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT for a quick first build). `on_demand`: one the game needs now
    /// (not the precompiler's) - without the driver's pipeline cache under SSPC_DIAG_PIPE_COLD_DEMAND=1.
    bool Build(const GS::PipelineDesc& desc, VkShaderModule vs, VkShaderModule ps, PipelineObjects& out, u32& flags,
               std::string& error, VkPipelineCreateFlags create_flags = 0, bool on_demand = false);
    /// Destroys a pipeline's objects (a failed precompiled one, the precompiler's at the end).
    void Destroy(PipelineObjects& obj);
    VkDevice Device() const;

private:
    VkShaderModule Module(const GS::Module& module, std::string& error);
    VkShaderModule Aux(u32 which, const GS::RuntimeParams* fragment, std::string& error);
    bool MakeLayout(std::span<const u64> bindings, bool compute, PipelineObjects& out, std::string& error);
    /// The pipeline of a description that missed the id table: made before in this run, precompiled, or created now.
    const PipelineObjects* Obtain(u64 key, const GS::PipelineDesc& desc, const GS::Module* vs, const GS::Module* ps,
                                  u32 pipeline_id, std::string& error);

    DeviceContext& ctx_;
    const GS::DeviceProfile& profile_;
    // P4.7: one descriptor set layout and pipeline layout per layout signature (graphics/compute), shared by every
    // pipeline with it - push descriptors and push constants stay valid across their pipelines (the executor records
    // only what changed); each pipeline has its own otherwise. Off unless SSPC_DIAG_SHARED_LAYOUTS=on: the A/B (3 pairs)
    // found no time saved (docs\PHASE4_FOLIAGE_OPTIMIZATION.md).
    struct SharedLayout {
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        bool push_descriptors = true;
        u32 bindings = 0;
        std::vector<u64> desc; ///< (binding, type, count) of each binding: compared, the hash only finds it
    };
    std::unordered_map<u64, SharedLayout, U64Hash> layouts_;
    bool share_layouts_ = false;
    std::unordered_map<u32, VkShaderModule> modules_;
    std::mutex aux_mutex_; ///< (P4.14: the precompiler's workers build auxiliary stages too)
    std::unordered_map<u64, VkShaderModule> aux_;
    std::unordered_map<u64, const PipelineObjects*, U64Hash> pipelines_; ///< (pipeline id or module, layout signature) -> pipeline
    std::deque<std::unique_ptr<PipelineObjects>> owned_;       ///< created on demand (the precompiler owns its own)
    std::unordered_multimap<u64, std::pair<GS::PipelineDesc, const PipelineObjects*>> by_desc_; ///< hash -> made
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    GS::ShaderDiskCache* disk_ = nullptr;
    std::unique_ptr<PipelinePrecompiler> pre_;
    std::atomic<u64> created_{0};
    PipelineStats stats_;

    // SSPC_ASYNC_PIPELINES=1: a graphics pipeline nothing has built yet is built by background workers; its draws are
    // skipped until it is ready (the recording thread never waits for the driver's compiler). Compute stays synchronous.
    struct AsyncJob {
        u64 key = 0, hash = 0;
        GS::PipelineDesc desc;
        VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
        u32 pipeline_id = 0;
        std::atomic<u8> state{0}; ///< 0 queued, 1 building, 2 ready, 3 failed
        std::unique_ptr<PipelineObjects> obj;
        u32 flags = 0;
        u64 ns = 0;
        std::string error;
    };
    void AsyncWorker();
    /// One queued job taken (`wait`: until one comes or the stop - the own workers; else nullptr when none - a pool task)
    AsyncJob* PopAsync(bool wait);
    void RunAsync(AsyncJob* j);
    bool async_ = false, async_compute_ = false;
    std::unordered_map<u64, std::unique_ptr<AsyncJob>, U64Hash> async_jobs_; ///< by key (the recording thread's)
    std::mutex async_mutex_;
    std::condition_variable async_cv_;
    std::deque<AsyncJob*> async_queue_;
    std::vector<std::thread> async_workers_;
    bool async_stop_ = false;

    // R9-N SSPC_PIPE_PREFETCH (PrefetchCompute)
    struct PrefetchJob {
        GS::PipelineDesc desc;
        u64 hash = 0;
        std::vector<u32> spirv; ///< the compute stage's (a copy: the worker makes its own module)
        u8 state = 0;           ///< 0 queued, 1 building, 2 ready, 3 failed (under pf_mutex_)
        std::unique_ptr<PipelineObjects> obj;
        u32 flags = 0;
        u64 ns = 0;
        std::string error;
    };
    void PrefetchWorker();
    PrefetchJob* PopPrefetch(bool wait); ///< (as PopAsync)
    void RunPrefetch(PrefetchJob* j);
    /// The recording thread: the prefetched pipeline of `desc`, installed under `key` (waits for a worker still
    /// building it). nullptr: none, failed, or not started yet (taken out of the queue: the caller builds it now).
    const PipelineObjects* TakePrefetched(u64 key, u64 hash, const GS::PipelineDesc& desc, const GS::Module* cs);
    /// The recording thread has (or is about to build) a pipeline of this description: not prefetched again.
    void NoteKnown(u64 hash);
    bool prefetch_ = false;
    /// SSPC_DIAG_PIPE_COLD_DEMAND=1 (R9-N A/B): pipelines created on demand bypass the driver's pipeline cache file (with
    /// __GL_SHADER_DISK_CACHE_PATH=<an empty directory>: compiled as the first time a player meets them)
    bool cold_demand_ = false;
    std::unordered_set<u64, U64Hash> pf_seen_; ///< (cs module, layout) keys the frontend looked at (its thread only)
    std::mutex pf_mutex_;
    std::condition_variable pf_cv_, pf_done_cv_;
    std::unordered_multimap<u64, std::unique_ptr<PrefetchJob>, U64Hash> pf_jobs_; ///< by description hash, until taken
    std::unordered_set<u64, U64Hash> pf_known_; ///< descriptions the recording thread has or builds itself
    std::deque<PrefetchJob*> pf_queue_;
    std::vector<std::thread> pf_workers_;
    bool pf_stop_ = false;
    std::atomic<u64> pf_queued_{0};

    // R10 (SSPC_R10_SCHED=1, r10/work_pool.h): the async graphics and prefetch jobs run as tasks of the shared work
    // pool instead of the threads above - drainer tasks (at most one a worker per kind) take the oldest job of their
    // queue, one a task, and go on as a new task while it is not empty; a job is never dropped (the first version
    // refused jobs when the pool's queue was full: on a cold cache the draws stayed skipped - docs §8). The destructor
    // waits for the tasks submitted so far (those that find the factory dying take nothing).
    /// A drainer task of the pool for the kind's queue (prefetch / async graphics) when fewer than the workers run.
    void Kick(bool prefetch);
    bool SubmitDrainer(bool prefetch);
    bool QueuedFor(bool prefetch);
    std::atomic<u32> pool_drainers_[2] = {}; ///< drainer tasks of each kind submitted and not ended
    R10::WorkPool* pool_ = nullptr;
    std::atomic<u32> pool_inflight_{0};
    std::atomic<bool> pool_dead_{false};
    std::atomic<u64> pool_refused_{0};
};

/// The layout signature of plans (binding numbers, kinds, counts): part of a pipeline's key.
u64 LayoutSignature(std::span<const GS::ResourceBindingPlan* const> plans);

} // namespace SecondSon::Gpu::Vk
