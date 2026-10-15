// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — the backend's Vulkan pipelines (P4.4). See vk_pipelines.h. The graphics pipeline's fixed function is
// ported from shadPS4's renderer_vulkan/vk_graphics_pipeline.cpp (GPL-2.0-or-later): the translated SPIR-V is the
// bridge's, so the pipeline around it must be built the same way.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_pipelines.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/shader_trace.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_formats.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

constexpr VkShaderStageFlags kAllGraphics = VK_SHADER_STAGE_ALL_GRAPHICS;

u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// A pipeline the driver created in `ns`: totals, histogram, the slowest (`name()` only then).
template <typename Name>
void CountCreation(PipelineStats& s, u64 ns, Name&& name) {
    s.create_ns += ns;
    StallTrace::SpanEndingNow(StallTrace::Ev::PipelineCreate, ns); // (P4.12)
    static constexpr u64 kEdges[] = {1'000'000, 5'000'000, 20'000'000, 50'000'000, 100'000'000};
    size_t b = 0;
    while (b < std::size(kEdges) && ns >= kEdges[b]) {
        ++b;
    }
    ++s.create_hist[b];
    if (ns > s.slowest_ns) {
        s.slowest_ns = ns;
        s.slowest = name();
    }
}

/// The descriptor set layout's bindings of plans, as PipelineDesc keeps them: (binding << 32) | (type << 16) | count.
void PlanBindings(std::span<const GS::ResourceBindingPlan* const> plans, std::vector<u64>& out) {
    out.clear();
    const auto put = [&out](u32 binding, VkDescriptorType type, u32 count) {
        out.push_back((u64{binding} << 32) | (u64{static_cast<u32>(type)} << 16) | count);
    };
    for (const auto* p : plans) {
        if (p == nullptr) {
            continue;
        }
        for (const auto& b : p->buffers) {
            put(b.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1);
        }
        for (const auto& i : p->images) {
            put(i.binding, i.storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                std::max<u32>(i.count, 1));
        }
        for (const auto& s : p->samplers) {
            put(s.binding, VK_DESCRIPTOR_TYPE_SAMPLER, 1);
        }
    }
}

/// A ready module's SPIR-V by content (set when it was translated or read; computed for one that came another way).
GS::SpirvHash SpirvOf(const GS::Module& m) {
    return m.spirv_hash.Empty() ? GS::HashSpirv(m.result.spirv) : m.spirv_hash;
}

/// The driver's creation feedback as ShaderTrace::kPipe* flags.
u32 FeedbackFlags(const VkPipelineCreationFeedback& fb) {
    if ((fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0) {
        return 0;
    }
    return ShaderTrace::kPipeFeedback |
           ((fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0 ? ShaderTrace::kPipeCacheHit
                                                                                                : 0u);
}

} // namespace

u64 LayoutSignature(std::span<const GS::ResourceBindingPlan* const> plans) {
    u64 h = 0x51;
    for (const auto* p : plans) {
        if (p == nullptr) {
            continue;
        }
        for (const auto& b : p->buffers) {
            h = Mix(h, b.binding);
        }
        h = Mix(h, 0xB0);
        for (const auto& i : p->images) {
            h = Mix(h, (u64{i.binding} << 32) | (u64{i.count} << 1) | (i.storage ? 1 : 0));
        }
        h = Mix(h, 0x1A);
        for (const auto& s : p->samplers) {
            h = Mix(h, s.binding);
        }
        h = Mix(h, 0x5A);
    }
    return h;
}

PipelineFactory::PipelineFactory(DeviceContext& ctx, const GS::DeviceProfile& profile)
    : ctx_(ctx), profile_(profile) {
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_DIAG_SHARED_LAYOUTS", v, sizeof(v)) == 2 && std::string_view(v) == "on") {
        share_layouts_ = true;
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_DIAG_SHARED_LAYOUTS=on - one pipeline layout per binding signature");
    }
}

PipelineFactory::~PipelineFactory() {
    StopPrecompile();
    pre_.reset(); // (destroys the pipelines it built through Destroy)
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    for (auto& p : owned_) {
        Destroy(*p);
    }
    for (auto& [k, l] : layouts_) {
        f.vkDestroyPipelineLayout(dev, l.layout, nullptr);
        f.vkDestroyDescriptorSetLayout(dev, l.set_layout, nullptr);
    }
    for (auto& [k, m] : modules_) {
        f.vkDestroyShaderModule(dev, m, nullptr);
    }
    for (auto& [k, m] : aux_) {
        f.vkDestroyShaderModule(dev, m, nullptr);
    }
}

void PipelineFactory::Destroy(PipelineObjects& p) {
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    if (p.pipeline != VK_NULL_HANDLE) {
        f.vkDestroyPipeline(dev, p.pipeline, nullptr);
        p.pipeline = VK_NULL_HANDLE;
    }
    if (p.shared_layout) {
        return; // (the table's)
    }
    if (p.layout != VK_NULL_HANDLE) {
        f.vkDestroyPipelineLayout(dev, p.layout, nullptr);
        p.layout = VK_NULL_HANDLE;
    }
    if (p.set_layout != VK_NULL_HANDLE) {
        f.vkDestroyDescriptorSetLayout(dev, p.set_layout, nullptr);
        p.set_layout = VK_NULL_HANDLE;
    }
}

std::vector<u64> PipelineFactory::UsedDescriptions() const {
    std::vector<u64> out;
    out.reserve(by_desc_.size());
    for (const auto& [hash, made] : by_desc_) {
        out.push_back(hash);
    }
    return out;
}

VkDevice PipelineFactory::Device() const {
    return ctx_.Device();
}

void PipelineFactory::SetPersistence(VkPipelineCache cache, GS::ShaderDiskCache* disk) {
    cache_ = cache;
    disk_ = disk;
}

void PipelineFactory::StartPrecompile(std::vector<GS::PipelineDesc> descs, const PrecompileOptions& options) {
    if (disk_ == nullptr || descs.empty()) {
        return;
    }
    if (share_layouts_) {
        SSPC_LOG(GPU, Warning, "shader precompiler: off with SSPC_DIAG_SHARED_LAYOUTS=on (its layouts are not shared)");
        return;
    }
    pre_ = std::make_unique<PipelinePrecompiler>(*this, *disk_, std::move(descs), options);
    pre_->Start();
}

void PipelineFactory::StopPrecompile() {
    if (pre_) {
        pre_->Cancel();
        pre_->Join();
    }
}

VkShaderModule PipelineFactory::Module(const GS::Module& module, std::string& error) {
    if (const auto it = modules_.find(module.id); it != modules_.end()) {
        return it->second;
    }
    const auto& spirv = module.result.spirv;
    if (spirv.empty()) {
        error = fmt::format("module {} has no SPIR-V", module.id);
        return VK_NULL_HANDLE;
    }
    const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = spirv.size() * 4,
                                      .pCode = spirv.data()};
    VkShaderModule m = VK_NULL_HANDLE;
    const u64 t0 = NowNs();
    const u64 tsc0 = __rdtsc();
    const VkResult r = Fn().vkCreateShaderModule(ctx_.Device(), &ci, nullptr, &m);
    stats_.module_ns += NowNs() - t0;
    ShaderTrace::Event(ShaderTrace::Kind::ShaderModule, tsc0, __rdtsc(), module.id, spirv.size() * 4);
    if (r != VK_SUCCESS) {
        error = fmt::format("vkCreateShaderModule refused module {}", module.id);
        return VK_NULL_HANDLE;
    }
    modules_.emplace(module.id, m);
    ++stats_.modules;
    return m;
}

VkShaderModule PipelineFactory::Aux(u32 which, const GS::RuntimeParams* fragment, std::string& error) {
    u64 key = which;
    if (fragment != nullptr) {
        const auto* bytes = reinterpret_cast<const u8*>(fragment);
        for (size_t i = 0; i < sizeof(GS::RuntimeParams); i += 8) {
            u64 v = 0;
            std::memcpy(&v, bytes + i, std::min<size_t>(8, sizeof(GS::RuntimeParams) - i));
            key = Mix(key, v);
        }
    }
    std::scoped_lock lock{aux_mutex_};
    if (const auto it = aux_.find(key); it != aux_.end()) {
        return it->second;
    }
    GS::ShaderTranslator* t = GS::GetShaderTranslator();
    if (t == nullptr) {
        error = "no shader translator for the auxiliary stages";
        return VK_NULL_HANDLE;
    }
    const auto spirv = t->EmitAuxStage(static_cast<GS::AuxStage>(which), fragment, profile_, error);
    if (spirv.empty()) {
        return VK_NULL_HANDLE;
    }
    const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = spirv.size() * 4,
                                      .pCode = spirv.data()};
    VkShaderModule m = VK_NULL_HANDLE;
    const VkResult r = Fn().vkCreateShaderModule(ctx_.Device(), &ci, nullptr, &m);
    if (r != VK_SUCCESS) {
        error = "the auxiliary stage's module was refused";
        return VK_NULL_HANDLE;
    }
    aux_.emplace(key, m);
    return m;
}

bool PipelineFactory::MakeLayout(std::span<const u64> encoded, bool compute, PipelineObjects& out,
                                 std::string& error) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(encoded.size());
    const VkShaderStageFlags stages = compute ? VK_SHADER_STAGE_COMPUTE_BIT : kAllGraphics;
    u32 count = 0;
    for (const u64 e : encoded) {
        const u32 n = static_cast<u32>(e & 0xFFFF);
        bindings.push_back({static_cast<u32>(e >> 32), static_cast<VkDescriptorType>((e >> 16) & 0xFFFF), n, stages,
                            nullptr});
        count += n;
    }
    out.bindings = count;
    out.push_descriptors = count < ctx_.Caps().max_push_descriptors;
    // P4.7: the layouts of this binding set, if made before (one per set of bindings, shared by its pipelines)
    std::vector<u64> desc;
    SharedLayout* shared = nullptr;
    if (share_layouts_) {
        desc.reserve(bindings.size() + 1);
        u64 h = compute ? 0xC0 : 0x6F;
        desc.push_back(compute ? 1 : 0);
        for (const auto& b : bindings) {
            desc.push_back((u64{b.binding} << 32) | (u64{static_cast<u32>(b.descriptorType)} << 16) | b.descriptorCount);
            h = Mix(h, desc.back());
        }
        auto [it, inserted] = layouts_.try_emplace(h);
        if (!inserted && it->second.desc == desc) {
            out.set_layout = it->second.set_layout;
            out.layout = it->second.layout;
            out.push_descriptors = it->second.push_descriptors;
            out.shared_layout = true;
            return true;
        }
        if (inserted) {
            shared = &it->second; // filled below once both layouts exist
        } // (another set with this hash: this pipeline keeps layouts of its own)
    }
    const VkDescriptorSetLayoutCreateInfo dci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = out.push_descriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0u,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data()};
    if (Fn().vkCreateDescriptorSetLayout(ctx_.Device(), &dci, nullptr, &out.set_layout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    const VkPushConstantRange push{.stageFlags = stages, .offset = 0, .size = sizeof(PushData)};
    const VkPipelineLayoutCreateInfo pci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &out.set_layout,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    if (Fn().vkCreatePipelineLayout(ctx_.Device(), &pci, nullptr, &out.layout) != VK_SUCCESS) {
        Fn().vkDestroyDescriptorSetLayout(ctx_.Device(), out.set_layout, nullptr);
        out.set_layout = VK_NULL_HANDLE;
        error = "vkCreatePipelineLayout failed";
        return false;
    }
    if (shared != nullptr) {
        *shared = {out.set_layout, out.layout, out.push_descriptors, out.bindings, std::move(desc)};
        out.shared_layout = true;
    }
    return true;
}

GS::PipelineDesc PipelineFactory::DescribeGraphics(const GS::GraphicsPipelineState& state, const GS::Module* vs,
                                                   const GS::ResourceBindingPlan& vs_plan, const GS::Module* ps,
                                                   const GS::ResourceBindingPlan* ps_plan) {
    GS::PipelineDesc d;
    d.compute = false;
    d.state = state;
    d.state.vs_module = 0;
    d.state.ps_module = 0;
    d.vs = vs != nullptr ? SpirvOf(*vs) : GS::SpirvHash{};
    d.has_ps = ps != nullptr;
    if (ps != nullptr) {
        d.ps = SpirvOf(*ps);
        d.fs_runtime = ps->runtime;
    }
    const GS::ResourceBindingPlan* plans[2] = {ps_plan, &vs_plan};
    PlanBindings(plans, d.bindings);
    return d;
}

GS::PipelineDesc PipelineFactory::DescribeCompute(const GS::Module& cs, const GS::ResourceBindingPlan& plan) {
    GS::PipelineDesc d;
    d.compute = true;
    d.vs = SpirvOf(cs);
    const GS::ResourceBindingPlan* plans[1] = {&plan};
    PlanBindings(plans, d.bindings);
    return d;
}

bool PipelineFactory::Build(const GS::PipelineDesc& desc, VkShaderModule vs_mod, VkShaderModule ps_mod,
                            PipelineObjects& out, u32& flags, std::string& error) {
    flags = desc.compute ? ShaderTrace::kPipeCompute : 0u;
    out.compute = desc.compute;
    if (!MakeLayout(desc.bindings, desc.compute, out, error)) {
        return false;
    }
    const auto& f = Fn();
    const auto fail = [&](std::string why) {
        error = std::move(why);
        Destroy(out);
        return false;
    };
    VkPipelineCreationFeedback feedback{};
    std::array<VkPipelineCreationFeedback, 4> stage_feedback{};
    if (desc.compute) {
        const VkPipelineCreationFeedbackCreateInfo fci{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                       .pPipelineCreationFeedback = &feedback,
                                                       .pipelineStageCreationFeedbackCount = 1,
                                                       .pPipelineStageCreationFeedbacks = stage_feedback.data()};
        const VkComputePipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                             .pNext = &fci,
                                             .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                       .module = vs_mod,
                                                       .pName = "main"},
                                             .layout = out.layout};
        const VkResult r = f.vkCreateComputePipelines(ctx_.Device(), cache_, 1, &ci, nullptr, &out.pipeline);
        if (r != VK_SUCCESS) {
            out.pipeline = VK_NULL_HANDLE;
            return fail(fmt::format("vkCreateComputePipelines failed ({})", static_cast<int>(r)));
        }
        flags |= FeedbackFlags(feedback);
        return true;
    }
    const GS::GraphicsPipelineState& st = desc.state;
    const DeviceCaps& caps = ctx_.Caps();

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_VERTEX_BIT,
                      .module = vs_mod,
                      .pName = "main"});
    const bool rect = st.prim_type == 17, quad = st.prim_type == 19;
    const GS::RuntimeParams* fs_runtime = desc.has_ps ? &desc.fs_runtime : nullptr;
    if (rect || quad) {
        const VkShaderModule tcs = Aux(static_cast<u32>(quad ? GS::AuxStage::QuadListTcs : GS::AuxStage::RectListTcs),
                                       fs_runtime, error);
        const VkShaderModule tes = Aux(static_cast<u32>(GS::AuxStage::PassthroughTes), fs_runtime, error);
        if (tcs == VK_NULL_HANDLE || tes == VK_NULL_HANDLE) {
            return fail("rect/quad list tessellation stages: " + error);
        }
        stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                          .module = tcs,
                          .pName = "main"});
        stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                          .module = tes,
                          .pName = "main"});
    }
    if (desc.has_ps) {
        stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                          .module = ps_mod,
                          .pName = "main"});
    }

    const auto topology = PrimitiveType(st.prim_type);
    if (!topology.Valid()) {
        return fail(fmt::format("primitive type {} has no topology", st.prim_type));
    }
    const VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = topology.value};
    const VkPipelineTessellationStateCreateInfo tess{.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                     .patchControlPoints = rect ? 3u : (quad ? 4u : 3u)};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
        .depthClipEnable = st.depth_clip != 0 ? VK_TRUE : VK_FALSE};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .pNext = caps.depth_clip_enable ? &clip : nullptr,
        .provokingVertexMode =
            st.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT};
    const auto poly = PolygonMode(st.poly_mode);
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = caps.provoking_vertex ? static_cast<const void*>(&provoking)
                                       : (caps.depth_clip_enable ? static_cast<const void*>(&clip) : nullptr),
        .depthClampEnable = (st.depth_clamp != 0 && (st.depth_clip == 0 || caps.depth_clip_enable)) ? VK_TRUE : VK_FALSE,
        .polygonMode = poly.Valid() ? poly.value : VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f};
    u32 max_samples_log2 = st.depth_samples_log2;
    for (const auto& t : st.targets) {
        max_samples_log2 = std::max(max_samples_log2, t.samples_log2);
    }
    const auto samples = NumSamples(1u << max_samples_log2, caps.color_sample_counts & caps.depth_sample_counts);
    const bool sample_shading =
        fs_runtime != nullptr && ((fs_runtime->ps_input_addr & 1) != 0 || ((fs_runtime->ps_input_addr >> 4) & 1) != 0);
    const VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                  .rasterizationSamples =
                                                      samples.Valid() ? samples.value : VK_SAMPLE_COUNT_1_BIT,
                                                  .sampleShadingEnable = sample_shading ? VK_TRUE : VK_FALSE};
    const VkPipelineViewportDepthClipControlCreateInfoEXT clip_control{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
        .negativeOneToOne = st.clip_space == 0 ? VK_TRUE : VK_FALSE}; // DX_CLIP_SPACE_DEF 0: [-W, W]
    const VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                     .pNext = caps.depth_clip_control ? &clip_control : nullptr};
    std::vector<VkDynamicState> dyn = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,   VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,       VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,     VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,   VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_OP,            VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_LINE_WIDTH,            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
    };
    if (caps.color_write_mask_dynamic) {
        dyn.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    }
    if (!caps.vertex_input_dynamic) {
        return fail("the device has no dynamic vertex input (VK_EXT_vertex_input_dynamic_state): not supported yet");
    }
    dyn.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = static_cast<u32>(dyn.size()),
                                                   .pDynamicStates = dyn.data()};

    // targets and blending (the bridge's GraphicsPipeline, with its GCN adjustments)
    const u32 num_color = static_cast<u32>(std::bit_width(st.mrt_mask & 0xFFu));
    std::array<VkFormat, 8> color_formats{};
    std::array<VkPipelineColorBlendAttachmentState, 8> blends{};
    for (u32 i = 0; i < num_color; ++i) {
        const auto& t = st.targets[i];
        color_formats[i] = (st.mrt_mask >> i) & 1 ? ColorTargetFormat(t.info) : VK_FORMAT_UNDEFINED;
        const u32 c = t.blend;
        const u32 color_src = c & 0x1F, color_func = (c >> 5) & 7, color_dst = (c >> 8) & 0x1F;
        const u32 alpha_src = (c >> 16) & 0x1F, alpha_func = (c >> 21) & 7, alpha_dst = (c >> 24) & 0x1F;
        const bool separate = (c >> 29) & 1, enable = (c >> 30) & 1;
        VkBlendFactor src_color = BlendFactor(color_src).value, dst_color = BlendFactor(color_dst).value;
        VkBlendOp color_op = BlendOp(color_func).value;
        VkBlendFactor src_alpha = separate ? BlendFactor(alpha_src).value : src_color;
        VkBlendFactor dst_alpha = separate ? BlendFactor(alpha_dst).value : dst_color;
        VkBlendOp alpha_op = separate ? BlendOp(alpha_func).value : color_op;
        if (fs_runtime != nullptr && fs_runtime->targets[i].min_max_self != 0) {
            src_color = VK_BLEND_FACTOR_ONE; // the shader squares its output (PsColorBuffer blend_self_scale)
            dst_color = VK_BLEND_FACTOR_ONE;
        }
        // CB_SHADER_MASK without alpha: the hardware blends with alpha 1
        const bool alpha_masked = ((st.cb_shader_mask >> (i * 4)) & 8) == 0;
        if (alpha_masked && (src_color == VK_BLEND_FACTOR_SRC_ALPHA || src_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)) {
            src_color = src_color == VK_BLEND_FACTOR_SRC_ALPHA ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        }
        if (alpha_masked && (dst_color == VK_BLEND_FACTOR_SRC_ALPHA || dst_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)) {
            dst_color = dst_color == VK_BLEND_FACTOR_SRC_ALPHA ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        }
        const u32 nfmt = static_cast<u32>(ColorTargetNumberFormat(t.info));
        NormalizeMinMaxBlend(color_op, src_color, dst_color, nfmt);
        NormalizeMinMaxBlend(alpha_op, src_alpha, dst_alpha, nfmt);
        blends[i] = {.blendEnable = enable ? VK_TRUE : VK_FALSE,
                     .srcColorBlendFactor = src_color,
                     .dstColorBlendFactor = dst_color,
                     .colorBlendOp = color_op,
                     .srcAlphaBlendFactor = src_alpha,
                     .dstAlphaBlendFactor = dst_alpha,
                     .alphaBlendOp = alpha_op,
                     .colorWriteMask = caps.color_write_mask_dynamic ? 0xFu : (t.write_mask & 0xFu)};
    }
    const auto logic = LogicOp(st.logic_op);
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .logicOpEnable = st.logic_op != 0xCC ? VK_TRUE : VK_FALSE,
                                                    .logicOp = logic.value,
                                                    .attachmentCount = num_color,
                                                    .pAttachments = blends.data(),
                                                    .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f}};
    const VkFormat depth_format =
        (st.z_format != 0 || st.stencil_format != 0) ? DepthFormat(st.z_format, st.stencil_format) : VK_FORMAT_UNDEFINED;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = num_color,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = st.z_format != 0 ? depth_format : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = st.stencil_format != 0 ? depth_format : VK_FORMAT_UNDEFINED};
    // P4.14: the driver says whether the application's pipeline cache had it (the stall trace and the statistics)
    const VkPipelineCreationFeedbackCreateInfo fci{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                   .pNext = &rendering,
                                                   .pPipelineCreationFeedback = &feedback,
                                                   .pipelineStageCreationFeedbackCount = static_cast<u32>(stages.size()),
                                                   .pPipelineStageCreationFeedbacks = stage_feedback.data()};
    const VkPipelineDepthStencilStateCreateInfo ds{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                   .maxDepthBounds = 1.0f};
    const VkPipelineVertexInputStateCreateInfo vi{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkGraphicsPipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                          .pNext = &fci,
                                          .stageCount = static_cast<u32>(stages.size()),
                                          .pStages = stages.data(),
                                          .pVertexInputState = &vi,
                                          .pInputAssemblyState = &ia,
                                          .pTessellationState = (rect || quad) ? &tess : nullptr,
                                          .pViewportState = &viewport,
                                          .pRasterizationState = &raster,
                                          .pMultisampleState = &ms,
                                          .pDepthStencilState = &ds,
                                          .pColorBlendState = &blend,
                                          .pDynamicState = &dynamic,
                                          .layout = out.layout};
    const VkResult r = f.vkCreateGraphicsPipelines(ctx_.Device(), cache_, 1, &ci, nullptr, &out.pipeline);
    if (r != VK_SUCCESS) {
        out.pipeline = VK_NULL_HANDLE;
        return fail(fmt::format("vkCreateGraphicsPipelines failed ({})", static_cast<int>(r)));
    }
    flags |= FeedbackFlags(feedback);
    return true;
}

const PipelineObjects* PipelineFactory::Obtain(u64 key, const GS::PipelineDesc& desc, const GS::Module* vs,
                                               const GS::Module* ps, u32 pipeline_id, std::string& error) {
    const u64 hash = desc.Hash();
    const auto label = [&] {
        return desc.compute ? fmt::format("cs module {}", vs->id) : fmt::format("gfx p{}", pipeline_id);
    };
    // the same pipeline made already in this run (another pipeline id, or another module with the same SPIR-V)
    for (auto [a, b] = by_desc_.equal_range(hash); a != b; ++a) {
        if (a->second.first == desc) {
            ++stats_.desc_reuse;
            pipelines_.emplace(key, a->second.second);
            return a->second.second;
        }
    }
    // built before the game started (P4.14)
    if (pre_) {
        bool waited = false;
        u64 wait_ns = 0;
        const u64 t0 = __rdtsc();
        if (PipelineObjects* p = pre_->Take(desc, hash, waited, wait_ns)) {
            ++stats_.prebuilt;
            if (waited) {
                ++stats_.prebuilt_waited;
                stats_.prebuilt_wait_ns += wait_ns;
            }
            ShaderTrace::Event(ShaderTrace::Kind::PipelinePrebuilt, t0, __rdtsc(), hash, 0,
                               (desc.compute ? ShaderTrace::kPipeCompute : 0u) | (waited ? ShaderTrace::kPipeWaited : 0u));
            if (ctx_.Caps().debug_utils) {
                ctx_.Name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(p->pipeline), label().c_str());
            }
            pipelines_.emplace(key, p);
            by_desc_.emplace(hash, std::pair{desc, p});
            return p;
        }
    }
    // created now
    const auto fail = [&](std::string why) -> const PipelineObjects* {
        error = std::move(why);
        ++stats_.failures;
        if (stats_.first_failure.empty()) {
            stats_.first_failure = error;
            SSPC_LOG(GPU, Error, "native backend: {}: {}", label(), error);
        }
        return nullptr;
    };
    const VkShaderModule vs_mod = Module(*vs, error);
    if (vs_mod == VK_NULL_HANDLE) {
        return fail(error);
    }
    VkShaderModule ps_mod = VK_NULL_HANDLE;
    if (!desc.compute && desc.has_ps) {
        ps_mod = Module(*ps, error);
        if (ps_mod == VK_NULL_HANDLE) {
            return fail(error);
        }
    }
    auto obj = std::make_unique<PipelineObjects>();
    u32 flags = 0;
    const u64 t0 = NowNs();
    const u64 tsc0 = __rdtsc();
    const bool ok = Build(desc, vs_mod, ps_mod, *obj, flags, error);
    const u64 ns = NowNs() - t0;
    CountCreation(stats_, ns, [&] {
        return desc.compute ? fmt::format("compute (cs module {})", vs->id)
                            : fmt::format("graphics p{} (vs module {}, ps module {})", pipeline_id, vs->id,
                                          ps != nullptr ? ps->id : 0);
    });
    ShaderTrace::Event(ShaderTrace::Kind::PipelineCreate, tsc0, __rdtsc(), hash, 0,
                       flags | (ok ? 0u : ShaderTrace::kPipeFailed));
    if (!ok) {
        return fail(error);
    }
    ++(desc.compute ? stats_.compute : stats_.graphics);
    if ((flags & ShaderTrace::kPipeFeedback) != 0) {
        ++stats_.feedback;
        stats_.cache_hits += (flags & ShaderTrace::kPipeCacheHit) != 0 ? 1 : 0;
    }
    if (ctx_.Caps().debug_utils) { // the id the executor's labels and SSPC_DIAG_* name (P4.6-G6)
        ctx_.Name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(obj->pipeline), label().c_str());
    }
    const PipelineObjects* raw = obj.get();
    owned_.push_back(std::move(obj));
    pipelines_.emplace(key, raw);
    by_desc_.emplace(hash, std::pair{desc, raw});
    created_.fetch_add(1, std::memory_order_relaxed);
    if (disk_ != nullptr) {
        disk_->AddPipeline(desc, hash, ns); // (P4.14: the manifest the next run precompiles)
    }
    return raw;
}

const PipelineObjects* PipelineFactory::Graphics(u32 pipeline_id, const GS::GraphicsPipelineState& st,
                                                 const GS::Module* vs, const GS::ResourceBindingPlan& vs_plan,
                                                 const GS::Module* ps, const GS::ResourceBindingPlan* ps_plan,
                                                 std::string& error) {
    ++stats_.lookups;
    const GS::ResourceBindingPlan* plans[2] = {ps_plan, &vs_plan};
    const u64 key = Mix(Mix(0x6A, pipeline_id), LayoutSignature(plans));
    if (const auto it = pipelines_.find(key); it != pipelines_.end()) {
        return it->second;
    }
    if (vs == nullptr) {
        error = "no vertex stage";
        ++stats_.failures;
        if (stats_.first_failure.empty()) {
            stats_.first_failure = error;
            SSPC_LOG(GPU, Error, "native backend: graphics pipeline {}: {}", pipeline_id, error);
        }
        return nullptr;
    }
    return Obtain(key, DescribeGraphics(st, vs, vs_plan, ps, ps_plan), vs, ps, pipeline_id, error);
}

const PipelineObjects* PipelineFactory::Compute(const GS::Module& cs, const GS::ResourceBindingPlan& plan,
                                                std::string& error) {
    ++stats_.lookups;
    const GS::ResourceBindingPlan* plans[1] = {&plan};
    const u64 key = Mix(Mix(0xC5, cs.id), LayoutSignature(plans));
    if (const auto it = pipelines_.find(key); it != pipelines_.end()) {
        return it->second;
    }
    return Obtain(key, DescribeCompute(cs, plan), &cs, nullptr, 0, error);
}

} // namespace SecondSon::Gpu::Vk
