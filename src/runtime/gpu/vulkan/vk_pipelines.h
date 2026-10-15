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
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_precompile.h"

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
    /// feedback). false + `error`: nothing left behind in `out`.
    bool Build(const GS::PipelineDesc& desc, VkShaderModule vs, VkShaderModule ps, PipelineObjects& out, u32& flags,
               std::string& error);
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
    std::unordered_map<u64, SharedLayout> layouts_;
    bool share_layouts_ = false;
    std::unordered_map<u32, VkShaderModule> modules_;
    std::mutex aux_mutex_; ///< (P4.14: the precompiler's workers build auxiliary stages too)
    std::unordered_map<u64, VkShaderModule> aux_;
    std::unordered_map<u64, const PipelineObjects*> pipelines_; ///< (pipeline id or module, layout signature) -> pipeline
    std::deque<std::unique_ptr<PipelineObjects>> owned_;       ///< created on demand (the precompiler owns its own)
    std::unordered_multimap<u64, std::pair<GS::PipelineDesc, const PipelineObjects*>> by_desc_; ///< hash -> made
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    GS::ShaderDiskCache* disk_ = nullptr;
    std::unique_ptr<PipelinePrecompiler> pre_;
    std::atomic<u64> created_{0};
    PipelineStats stats_;
};

/// The layout signature of plans (binding numbers, kinds, counts): part of a pipeline's key.
u64 LayoutSignature(std::span<const GS::ResourceBindingPlan* const> plans);

} // namespace SecondSon::Gpu::Vk
