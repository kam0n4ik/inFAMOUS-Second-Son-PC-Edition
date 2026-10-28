// SecondSonPC — the Vulkan device of SecondSonPC's own backend inside the game process (Phase 4, P4.4;
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §B). The P4.1 foundation (vk_foundation.h) stays the probe's and the
// self-test's; this is the device the game's work runs on with --gpu-backend sspc.
//
// What it enables is what the translated SPIR-V needs (the same modules as the bridge's renderer: the same features and
// extensions, enabled when the device has them) plus what the backend uses: dynamic rendering, synchronization2,
// timeline semaphores, push descriptors, vertex input dynamic state, external host memory (the guest's memory imported
// for buffers, docs §C). One queue (graphics + compute + present) — the backend serializes the guest's queues onto it
// (correctness first, docs §E). Created once, destroyed after the device is idle; nothing else owns these objects.
#pragma once

#include <array>
#include <atomic>
#include <filesystem>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Shader {
struct DeviceProfile;
}

namespace SecondSon::Gpu::Vk {

struct ContextOptions {
    s32 device_index = -1;     ///< -1: the first discrete device that has everything, else the policy's choice
    bool validation = false;   ///< VK_LAYER_KHRONOS_validation (+ synchronization validation) when installed
    bool presentation = true;  ///< the surface extensions and the swapchain
    /// --gpu-native-diag: NVIDIA checkpoints and diagnostics config, buffer markers, object names (VK_EXT_device_fault
    /// is enabled whenever the device has it: it costs nothing until a device loss)
    bool diagnostics = false;
    std::filesystem::path validation_log; ///< the validation layer's messages (with --gpu-native-validate)
};

/// What the device has beyond the core the backend requires.
struct DeviceCaps {
    bool external_memory_host = false;
    u64 min_imported_host_alignment = 0;
    bool color_write_mask_dynamic = false;  ///< VK_EXT_extended_dynamic_state3's colour write mask
    bool vertex_input_dynamic = false;      ///< VK_EXT_vertex_input_dynamic_state
    bool depth_clip_control = false;
    bool depth_clip_enable = false;
    bool provoking_vertex = false;
    bool custom_border_color = false;
    bool memory_budget = false;
    bool list_restart = false, patch_list_restart = false;
    bool attachment_feedback_loop = false;
    bool image_view_min_lod = false;
    bool maintenance5 = false;
    /// VK_EXT_multi_draw with shaderDrawParameters (enabled only with SSPC_CITY_INSTANCING=1)
    bool multi_draw = false;
    bool swapchain_maintenance1 = false; ///< present fences: required for safe native presentation teardown
    bool low_latency = false; ///< VK_NV_low_latency2 + the presentId feature (NVIDIA Reflex)
    bool debug_utils = false;
    /// SSPC_GPU_MARKERS=1 (P4.6-G6, diagnostic): debug labels around the recorded commands and object names, for a
    /// GPU profiler's capture (NVIDIA Nsight Graphics GPU Trace) to be read by pass/pipeline; needs VK_EXT_debug_utils
    bool markers = false;
    bool device_fault = false;      ///< VK_EXT_device_fault
    bool sparse_buffer = false;     ///< sparseBinding + sparseResidencyBuffer on the queue (the buffer cache)
    bool occlusion_precise = false; ///< occlusionQueryPrecise: the guest's occlusion queries counted exactly
    bool checkpoints = false;       ///< VK_NV_device_diagnostic_checkpoints (diagnostics)
    bool buffer_markers = false;    ///< VK_AMD_buffer_marker (diagnostics)
    bool diagnostics_config = false; ///< VK_NV_device_diagnostics_config (diagnostics)
    bool calibrated_timestamps = false; ///< VK_EXT_calibrated_timestamps (P4.12 stall trace, deep mode only)
    bool validation = false; ///< the validation layer is on
    u32 subgroup_size = 0;
    u32 subgroup_size_min = 0, subgroup_size_max = 0;
    /// the stages whose pipelines ask for subgroup_size (requiredSubgroupSize): a SPIR-V 1.6 module may run at any
    /// subgroup size between the minimum and the maximum the driver picks (AMD RDNA: 32 or 64), and the translated
    /// shaders' lane arithmetic is for subgroup_size (issue #2); 0 when the size cannot vary (NVIDIA: 32 only)
    VkShaderStageFlags subgroup_size_stages = 0;
    u32 max_push_descriptors = 0;
    u64 min_storage_buffer_alignment = 256;
    u64 min_uniform_buffer_alignment = 256;
    u32 max_viewport_width = 0, max_viewport_height = 0;
    u32 max_framebuffer_width = 0, max_framebuffer_height = 0;
    u32 color_sample_counts = 1, depth_sample_counts = 1;
    float timestamp_period = 1.0f;
};

class DeviceContext {
public:
    /// The instance, the device and its queue. nullptr and `error` on any failure (nothing left alive then).
    static std::unique_ptr<DeviceContext> Create(const ContextOptions& options, std::string& error);
    ~DeviceContext(); ///< waits for the device, then destroys it, the instance, unloads the loader

    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    VkInstance Instance() const { return instance_; }
    VkPhysicalDevice Physical() const { return physical_; }
    VkDevice Device() const { return device_; }
    VkQueue Queue() const { return queue_; }
    /// Frame generation (FSR 3.1: docs/UPSCALING_FRAME_GENERATION.md): more queues of the main family (present,
    /// acquire) and an async compute queue, created only when frame generation was chosen; VK_NULL_HANDLE without.
    VkQueue ExtraQueue(u32 index) const { return index < extra_queues_.size() ? extra_queues_[index] : VK_NULL_HANDLE; }
    VkQueue ComputeQueue() const { return compute_queue_; }
    /// FrameBreak AQ (SSPC_FB_ASYNC_COMPUTE=1): a queue of the main family for the executor's async compute lane;
    /// VK_NULL_HANDLE without (the switch off, or the family has no queue left).
    VkQueue LaneQueue() const { return lane_queue_; }
    u32 LaneFamily() const { return lane_family_; }
    /// AQ on the compute family: a buffer the lane's batches may use is created shared by both families (concurrent
    /// sharing: no ownership transfers). Nothing changes otherwise.
    void ShareWithLane(VkBufferCreateInfo& bci) const {
        if (lane_queue_ != VK_NULL_HANDLE && lane_family_ != family_) {
            bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bci.queueFamilyIndexCount = 2;
            bci.pQueueFamilyIndices = lane_families_;
        }
    }
    u32 ComputeFamily() const { return compute_family_; }
    /// First fatal runtime error, shared by recording, presentation and timeline waiters.
    void Fail(const char* where, VkResult result);
    VkResult Failure() const { return failure_.load(std::memory_order_acquire); }
    bool Failed() const { return Failure() != VK_SUCCESS; }
    u32 QueueFamily() const { return family_; }
    const DeviceCaps& Caps() const { return caps_; }
    const VkPhysicalDeviceMemoryProperties& Memory() const { return memory_; }
    const std::string& Name() const { return name_; }
    u32 DriverVersion() const { return driver_version_; }
    /// What a VkPipelineCache's data is valid for (P4.14: the cache file is checked against them before it is given back)
    u32 VendorId() const { return vendor_id_; }
    u32 DeviceId() const { return device_id_; }
    const std::array<u8, VK_UUID_SIZE>& PipelineCacheUuid() const { return cache_uuid_; }

    /// A memory type in `bits` with every flag of `want` and none of `avoid`; -1: none.
    s32 FindMemoryType(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;

    /// What the device supports that a shader translation depends on (the bridge's profile rules, as P4.3's).
    void QueryProfile(Gpu::Shader::DeviceProfile& out) const;

    struct Budget {
        u64 device_local_budget = 0, device_local_usage = 0; ///< VK_EXT_memory_budget's heap (0 without it)
        u64 host_budget = 0, host_usage = 0;
    };
    Budget QueryBudget() const;
    /// The memory heaps for the log: size, flags, VK_EXT_memory_budget's budget and usage, the types on each
    std::string DescribeMemory() const;

    /// The one queue is used by the GPU thread and the presenter: every vkQueueSubmit2 / vkQueuePresentKHR under it.
    std::mutex& QueueMutex() { return queue_mutex_; }

    /// Debug name of an object (only with VK_EXT_debug_utils, i.e. validation runs); cheap no-op otherwise.
    void Name(VkObjectType type, u64 handle, const char* name) const;

    /// Validation messages counted (validation runs): errors, warnings; the first error's text.
    u64 ValidationErrors() const;
    u64 ValidationWarnings() const;
    std::string FirstValidationError() const;

    struct Impl;

private:
    DeviceContext() = default;
    void* module_ = nullptr; ///< HMODULE of vulkan-1.dll
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<VkQueue> extra_queues_;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue lane_queue_ = VK_NULL_HANDLE;
    u32 lane_family_ = ~0u;
    u32 lane_families_[2] = {};
    u32 compute_family_ = ~0u;
    u32 family_ = ~0u;
    DeviceCaps caps_;
    VkPhysicalDeviceMemoryProperties memory_{};
    std::string name_;
    u32 driver_version_ = 0;
    u32 vendor_id_ = 0, device_id_ = 0;
    std::array<u8, VK_UUID_SIZE> cache_uuid_{};
    std::mutex queue_mutex_;
    std::atomic<VkResult> failure_{VK_SUCCESS};
};

} // namespace SecondSon::Gpu::Vk
