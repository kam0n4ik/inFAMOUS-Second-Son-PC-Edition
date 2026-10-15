// SecondSonPC — the backend's Vulkan device (P4.4). See vk_device_context.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_device_context.h"

#include "runtime/gpu/vulkan/vk_diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/shader/shader_types.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

Api g_api;

std::atomic<u64> g_val_errors{0}, g_val_warnings{0};
std::mutex g_val_mutex;
std::string g_val_first;

bool Has(const std::vector<VkExtensionProperties>& exts, std::string_view name) {
    return std::ranges::any_of(exts, [&](const VkExtensionProperties& e) { return name == e.extensionName; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* text = data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no text)";
    WriteValidationMessage(severity, data); // logs\vk_validation.log: VUID, objects, submission
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        const u64 n = g_val_errors.fetch_add(1, std::memory_order_relaxed);
        if (n == 0) {
            std::scoped_lock lock{g_val_mutex};
            g_val_first = text;
        }
        if (n < 64) {
            SSPC_LOG(GPU, Error, "[vk validation] {}", text);
        }
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        const u64 n = g_val_warnings.fetch_add(1, std::memory_order_relaxed);
        if (n < 32) {
            SSPC_LOG(GPU, Warning, "[vk validation] {}", text);
        }
    }
    return VK_FALSE;
}

/// Extensions enabled when the device has them (the translated SPIR-V and the backend use them then).
constexpr const char* kWantedExtensions[] = {
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME,
    VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
    VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
    VK_KHR_SHADER_CLOCK_EXTENSION_NAME,
    VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
    VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
    VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    "VK_AMD_shader_image_load_store_lod",
    "VK_AMD_gcn_shader",
    "VK_AMD_shader_trinary_minmax",
    "VK_AMD_shader_explicit_vertex_parameter",
};

} // namespace

Api& Fn() {
    return g_api;
}

std::unique_ptr<DeviceContext> DeviceContext::Create(const ContextOptions& options, std::string& error) {
    std::unique_ptr<DeviceContext> ctx(new DeviceContext());
    Api& f = g_api;
    f = Api{};

    HMODULE module = LoadLibraryW(L"vulkan-1.dll");
    if (module == nullptr) {
        error = "vulkan-1.dll is not installed (no Vulkan loader)";
        return nullptr;
    }
    ctx->module_ = module;
    f.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
    if (f.vkGetInstanceProcAddr == nullptr) {
        error = "vulkan-1.dll has no vkGetInstanceProcAddr";
        return nullptr;
    }
#define SSPC_LOAD_G(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    SSPC_VKA_GLOBAL(SSPC_LOAD_G)
#undef SSPC_LOAD_G
    if (f.vkCreateInstance == nullptr || f.vkEnumerateInstanceExtensionProperties == nullptr) {
        error = "the Vulkan loader lacks its global functions";
        return nullptr;
    }
    u32 loader_api = VK_API_VERSION_1_0;
    if (f.vkEnumerateInstanceVersion != nullptr) {
        f.vkEnumerateInstanceVersion(&loader_api);
    }
    if (loader_api < VK_API_VERSION_1_3) {
        error = fmt::format("the Vulkan loader is {}.{} (1.3 is needed)", VK_API_VERSION_MAJOR(loader_api),
                            VK_API_VERSION_MINOR(loader_api));
        return nullptr;
    }

    // --- instance
    u32 n = 0;
    f.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iexts(n);
    f.vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
    std::vector<const char*> ienable;
    if (options.presentation) {
        for (const char* e : {"VK_KHR_surface", "VK_KHR_win32_surface",
                              "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1"}) {
            if (!Has(iexts, e)) {
                error = fmt::format("the instance extension {} is missing", e);
                return nullptr;
            }
            ienable.push_back(e);
        }
    }
    std::vector<const char*> layers;
    bool validation = false;
    if (options.validation) {
        u32 nl = 0;
        f.vkEnumerateInstanceLayerProperties(&nl, nullptr);
        std::vector<VkLayerProperties> lp(nl);
        f.vkEnumerateInstanceLayerProperties(&nl, lp.data());
        validation = std::ranges::any_of(lp, [](const VkLayerProperties& l) {
            return std::string_view(l.layerName) == "VK_LAYER_KHRONOS_validation";
        });
        if (validation) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            if (!options.validation_log.empty()) {
                OpenValidationLog(options.validation_log);
            }
        } else {
            SSPC_LOG(GPU, Warning,
                     "--gpu-native-validate: VK_LAYER_KHRONOS_validation is not installed (validation SKIPPED)");
        }
    }
    // debug utils: the validation messenger, the object names of --gpu-native-diag, the profiler labels of
    // SSPC_GPU_MARKERS=1 (P4.6-G6; under Nsight Graphics its layer provides the extension)
    const bool markers = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_GPU_MARKERS", v, sizeof(v)) == 1 && v[0] == '1';
    }();
    const bool debug_utils =
        (validation || options.diagnostics || markers) && Has(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (markers && !debug_utils) {
        SSPC_LOG(GPU, Warning, "SSPC_GPU_MARKERS=1: VK_EXT_debug_utils is not available - no labels or names");
    }
    if (debug_utils) {
        ienable.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "SecondSonPC",
                                .applicationVersion = 1,
                                .pEngineName = "SecondSonPC native GPU backend",
                                .engineVersion = 1,
                                .apiVersion = VK_API_VERSION_1_3};
    const VkValidationFeatureEnableEXT val_enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    const VkValidationFeaturesEXT val_features{.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                               .enabledValidationFeatureCount = 1,
                                               .pEnabledValidationFeatures = val_enables};
    const VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   .pNext = validation ? &val_features : nullptr,
                                   .pApplicationInfo = &app,
                                   .enabledLayerCount = static_cast<u32>(layers.size()),
                                   .ppEnabledLayerNames = layers.data(),
                                   .enabledExtensionCount = static_cast<u32>(ienable.size()),
                                   .ppEnabledExtensionNames = ienable.data()};
    if (const VkResult r = f.vkCreateInstance(&ici, nullptr, &ctx->instance_); r != VK_SUCCESS) {
        ctx->instance_ = VK_NULL_HANDLE;
        error = fmt::format("vkCreateInstance failed ({})", static_cast<int>(r));
        return nullptr;
    }
#define SSPC_LOAD_I(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetInstanceProcAddr(ctx->instance_, #name));
    SSPC_VKA_INSTANCE(SSPC_LOAD_I)
    SSPC_VKA_INSTANCE_OPT(SSPC_LOAD_I)
#undef SSPC_LOAD_I
    if (f.vkCreateDevice == nullptr || f.vkGetDeviceProcAddr == nullptr || f.vkEnumeratePhysicalDevices == nullptr) {
        error = "the instance lacks core functions";
        return nullptr;
    }
    if (validation && debug_utils && f.vkCreateDebugUtilsMessengerEXT != nullptr) {
        const VkDebugUtilsMessengerCreateInfoEXT mci{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = &OnValidation};
        f.vkCreateDebugUtilsMessengerEXT(ctx->instance_, &mci, nullptr, &ctx->messenger_);
    }
    ctx->caps_.validation = validation;
    ctx->caps_.debug_utils = debug_utils;
    ctx->caps_.markers = markers && debug_utils;
    if (ctx->caps_.markers) {
        SSPC_LOG(GPU, Warning, "SSPC_GPU_MARKERS=1: debug labels and object names for a GPU profiler (diagnostic)");
    }

    // --- the physical device
    f.vkEnumeratePhysicalDevices(ctx->instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    f.vkEnumeratePhysicalDevices(ctx->instance_, &n, pds.data());
    if (n == 0) {
        error = "no Vulkan physical device";
        return nullptr;
    }
    s32 pick = options.device_index;
    if (pick >= static_cast<s32>(n)) {
        error = fmt::format("--gpu-device {}: there are {} devices", pick, n);
        return nullptr;
    }
    if (pick < 0) {
        u64 best_bytes = 0;
        s32 best_rank = -1;
        for (u32 i = 0; i < n; ++i) {
            VkPhysicalDeviceProperties p{};
            f.vkGetPhysicalDeviceProperties(pds[i], &p);
            if (p.apiVersion < VK_API_VERSION_1_3) {
                continue;
            }
            VkPhysicalDeviceMemoryProperties mp{};
            f.vkGetPhysicalDeviceMemoryProperties(pds[i], &mp);
            u64 local = 0;
            for (u32 h = 0; h < mp.memoryHeapCount; ++h) {
                if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    local = std::max<u64>(local, mp.memoryHeaps[h].size);
                }
            }
            const s32 rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                      : 0;
            if (rank > best_rank || (rank == best_rank && local > best_bytes)) {
                best_rank = rank;
                best_bytes = local;
                pick = static_cast<s32>(i);
            }
        }
        if (pick < 0) {
            error = "no Vulkan 1.3 device";
            return nullptr;
        }
    }
    ctx->physical_ = pds[pick];
    VkPhysicalDevice pd = ctx->physical_;

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDevicePushDescriptorPropertiesKHR push_props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, .pNext = &host_props};
    VkPhysicalDeviceVulkan11Properties p11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                           .pNext = &push_props};
    VkPhysicalDeviceProperties2 p2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
    f.vkGetPhysicalDeviceProperties2(pd, &p2);
    ctx->name_ = p2.properties.deviceName;
    ctx->driver_version_ = p2.properties.driverVersion;
    ctx->vendor_id_ = p2.properties.vendorID;
    ctx->device_id_ = p2.properties.deviceID;
    std::copy(std::begin(p2.properties.pipelineCacheUUID), std::end(p2.properties.pipelineCacheUUID),
              ctx->cache_uuid_.begin());
    if (p2.properties.apiVersion < VK_API_VERSION_1_3) {
        error = fmt::format("{} supports Vulkan {}.{} (1.3 is needed)", ctx->name_,
                            VK_API_VERSION_MAJOR(p2.properties.apiVersion),
                            VK_API_VERSION_MINOR(p2.properties.apiVersion));
        return nullptr;
    }
    const VkPhysicalDeviceLimits& lim = p2.properties.limits;
    DeviceCaps& caps = ctx->caps_;
    caps.subgroup_size = p11.subgroupSize;
    caps.max_push_descriptors = push_props.maxPushDescriptors;
    caps.min_storage_buffer_alignment = lim.minStorageBufferOffsetAlignment;
    caps.min_uniform_buffer_alignment = lim.minUniformBufferOffsetAlignment;
    caps.max_viewport_width = lim.maxViewportDimensions[0];
    caps.max_viewport_height = lim.maxViewportDimensions[1];
    caps.max_framebuffer_width = lim.maxFramebufferWidth;
    caps.max_framebuffer_height = lim.maxFramebufferHeight;
    caps.color_sample_counts = lim.framebufferColorSampleCounts;
    caps.depth_sample_counts = lim.framebufferDepthSampleCounts;
    caps.timestamp_period = lim.timestampPeriod;
    caps.min_imported_host_alignment = host_props.minImportedHostPointerAlignment;
    f.vkGetPhysicalDeviceMemoryProperties(pd, &ctx->memory_);

    // --- the queue: graphics + compute (+ present, checked by the presenter on its surface)
    f.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    f.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf.data());
    for (u32 i = 0; i < n; ++i) {
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            ctx->family_ = i;
            break;
        }
    }
    if (ctx->family_ == ~0u) {
        error = fmt::format("{} has no graphics+compute queue family", ctx->name_);
        return nullptr;
    }

    // --- extensions
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dexts(n);
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, dexts.data());
    std::vector<const char*> denable;
    for (const char* e : {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME}) {
        if (!Has(dexts, e)) {
            error = fmt::format("{} lacks {}", ctx->name_, e);
            return nullptr;
        }
        denable.push_back(e);
    }
    const char* divisor = Has(dexts, "VK_KHR_vertex_attribute_divisor")   ? "VK_KHR_vertex_attribute_divisor"
                          : Has(dexts, "VK_EXT_vertex_attribute_divisor") ? "VK_EXT_vertex_attribute_divisor"
                                                                          : nullptr;
    if (divisor == nullptr) {
        error = fmt::format("{} lacks a vertex attribute divisor extension", ctx->name_);
        return nullptr;
    }
    denable.push_back(divisor);
    if (options.presentation) {
        if (!Has(dexts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            error = fmt::format("{} lacks {}", ctx->name_, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            return nullptr;
        }
        denable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (!Has(dexts, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
            error = "native presentation requires VK_EXT_swapchain_maintenance1 for safe present resource retirement";
            return nullptr;
        }
        denable.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }
    for (const char* e : kWantedExtensions) {
        if (Has(dexts, e)) {
            denable.push_back(e);
        }
    }
    if (Has(dexts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
        denable.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    // P4.12: the stall trace's deep mode converts the batches' GPU timestamps to the CPU's clock (only then: the device
    // is otherwise created as before)
    if (char st[256] = {}; GetEnvironmentVariableA("SSPC_P412_STALL_TRACE", st, sizeof(st)) >= 4 &&
                         std::string_view(st).starts_with("deep") &&
                         Has(dexts, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
        denable.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
    if (options.diagnostics) {
        // SSPC_DIAG_NV_CONFIG=off: without NVIDIA's diagnostics config (resource tracking may change the driver's
        // behaviour; a diagnostic experiment)
        char nvcfg[8] = {};
        const bool nv_config = !(GetEnvironmentVariableA("SSPC_DIAG_NV_CONFIG", nvcfg, sizeof(nvcfg)) == 3 &&
                                 std::string_view(nvcfg) == "off");
        for (const char* e : {VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, VK_AMD_BUFFER_MARKER_EXTENSION_NAME}) {
            if (Has(dexts, e)) {
                denable.push_back(e);
            }
        }
        if (nv_config && Has(dexts, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)) {
            denable.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        }
        SSPC_LOG(GPU, Info, "native backend diagnostics: NVIDIA diagnostics config {}", nv_config ? "on" : "OFF (experiment)");
    }
    const auto enabled = [&](std::string_view name) {
        return std::ranges::any_of(denable, [&](const char* e) { return name == e; });
    };
    caps.external_memory_host = enabled(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    caps.vertex_input_dynamic = enabled(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    caps.depth_clip_control = enabled(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    caps.depth_clip_enable = enabled(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    caps.provoking_vertex = enabled(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    caps.custom_border_color = enabled(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    caps.memory_budget = enabled(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    caps.attachment_feedback_loop = enabled(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    caps.image_view_min_lod = enabled(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    caps.maintenance5 = enabled(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    caps.calibrated_timestamps = enabled(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    // --- features: every one the device has among those the translated code and the backend use (queried, then
    // passed back; the chain only holds structures of enabled extensions)
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swap_maintenance{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT rob{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR div{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dce{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT lr{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT afl{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceImageViewMinLodFeaturesEXT minlod{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceShaderClockFeaturesKHR clock{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wgl{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT af2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceFaultFeaturesEXT fault{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV diagcfg{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV};
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
    VkPhysicalDeviceVulkan11Features v11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
    VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
    void** tail = &v13.pNext;
    const auto link = [&tail](auto& s) {
        *tail = &s;
        tail = &s.pNext;
    };
    if (options.presentation) {
        link(swap_maintenance);
    }
    link(rob);
    link(div);
    if (enabled(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) {
        link(eds3);
    }
    if (caps.vertex_input_dynamic) {
        link(vids);
    }
    if (caps.depth_clip_control) {
        link(dcc);
    }
    if (caps.depth_clip_enable) {
        link(dce);
    }
    if (caps.provoking_vertex) {
        link(pv);
    }
    if (caps.custom_border_color) {
        link(cbc);
    }
    if (enabled(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
        link(lr);
    }
    if (caps.attachment_feedback_loop) {
        link(afl);
    }
    if (caps.image_view_min_lod) {
        link(minlod);
    }
    if (enabled(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)) {
        link(bary);
    }
    if (enabled(VK_KHR_SHADER_CLOCK_EXTENSION_NAME)) {
        link(clock);
    }
    if (enabled(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME)) {
        link(wgl);
    }
    if (enabled(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
        link(fault);
    }
    if (enabled(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)) {
        link(diagcfg);
    }
    if (enabled(VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME)) {
        link(af2);
    }
    if (caps.maintenance5) {
        link(m5);
    }
    f.vkGetPhysicalDeviceFeatures2(pd, &f2);
    caps.swapchain_maintenance1 = options.presentation && swap_maintenance.swapchainMaintenance1 == VK_TRUE;
    if (options.presentation && !caps.swapchain_maintenance1) {
        error = "native presentation requires the swapchainMaintenance1 feature";
        return nullptr;
    }
    const auto need = [&](VkBool32 have, const char* what) {
        if (!have && error.empty()) {
            error = fmt::format("{} lacks {}", ctx->name_, what);
        }
    };
    need(f2.features.robustBufferAccess, "robustBufferAccess");
    need(f2.features.tessellationShader, "tessellationShader (rect and quad lists)");
    need(v13.dynamicRendering, "dynamicRendering");
    need(v13.synchronization2, "synchronization2");
    need(v12.timelineSemaphore, "timelineSemaphore");
    need(rob.nullDescriptor, "nullDescriptor");
    need(div.vertexAttributeInstanceRateDivisor, "vertexAttributeInstanceRateDivisor");
    if (!error.empty()) {
        return nullptr;
    }
    // what the backend does not use and only costs
    // sparse buffers: the buffer cache's mirror of the guest's GPU address space (kept when the device has them)
    caps.sparse_buffer = f2.features.sparseBinding == VK_TRUE && f2.features.sparseResidencyBuffer == VK_TRUE &&
                         (qf[ctx->family_].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
    if (!caps.sparse_buffer) {
        f2.features.sparseBinding = VK_FALSE;
        f2.features.sparseResidencyBuffer = VK_FALSE;
    }
    f2.features.sparseResidencyImage2D = VK_FALSE;
    f2.features.sparseResidencyImage3D = VK_FALSE;
    f2.features.sparseResidency2Samples = VK_FALSE;
    f2.features.sparseResidency4Samples = VK_FALSE;
    f2.features.sparseResidency8Samples = VK_FALSE;
    f2.features.sparseResidency16Samples = VK_FALSE;
    f2.features.sparseResidencyAliased = VK_FALSE;
    f2.features.pipelineStatisticsQuery = VK_FALSE;
    f2.features.inheritedQueries = VK_FALSE;
    v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    v12.bufferDeviceAddressMultiDevice = VK_FALSE;
    // of extended dynamic state 3, only the colour write mask (as the bridge's device)
    {
        void* next = eds3.pNext;
        const VkBool32 cwm = eds3.extendedDynamicState3ColorWriteMask;
        eds3 = VkPhysicalDeviceExtendedDynamicState3FeaturesEXT{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        eds3.pNext = next;
        eds3.extendedDynamicState3ColorWriteMask = cwm;
    }
    caps.color_write_mask_dynamic =
        enabled(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && eds3.extendedDynamicState3ColorWriteMask == VK_TRUE;
    caps.list_restart = lr.primitiveTopologyListRestart == VK_TRUE;
    caps.patch_list_restart = lr.primitiveTopologyPatchListRestart == VK_TRUE;
    caps.vertex_input_dynamic = caps.vertex_input_dynamic && vids.vertexInputDynamicState == VK_TRUE;

    fault.deviceFaultVendorBinary = VK_FALSE; // the vendor binary is not read
    caps.device_fault = enabled(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && fault.deviceFault == VK_TRUE;
    caps.checkpoints = enabled(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    caps.buffer_markers = enabled(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    caps.diagnostics_config = enabled(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME) && diagcfg.diagnosticsConfig == VK_TRUE;
    // NVIDIA's own diagnostics of a device loss (shader debug info, resource tracking, automatic checkpoints)
    const VkDeviceDiagnosticsConfigCreateInfoNV diag_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV,
        .pNext = &f2,
        .flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |
                 VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                 VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV};
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = ctx->family_,
                                      .queueCount = 1,
                                      .pQueuePriorities = &priority};
    const VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .pNext = caps.diagnostics_config ? static_cast<const void*>(&diag_info)
                                                                  : static_cast<const void*>(&f2),
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qci,
                                 .enabledExtensionCount = static_cast<u32>(denable.size()),
                                 .ppEnabledExtensionNames = denable.data()};
    if (const VkResult r = f.vkCreateDevice(pd, &dci, nullptr, &ctx->device_); r != VK_SUCCESS) {
        ctx->device_ = VK_NULL_HANDLE;
        error = fmt::format("vkCreateDevice on {} failed ({})", ctx->name_, static_cast<int>(r));
        return nullptr;
    }
#define SSPC_LOAD_D(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetDeviceProcAddr(ctx->device_, #name));
    SSPC_VKA_DEVICE(SSPC_LOAD_D)
    SSPC_VKA_DEVICE_OPT(SSPC_LOAD_D)
#undef SSPC_LOAD_D
#define SSPC_CHECK_D(name)                                                                                             \
    if (f.name == nullptr && error.empty()) {                                                                          \
        error = "the device lacks " #name;                                                                             \
    }
    SSPC_VKA_DEVICE(SSPC_CHECK_D)
#undef SSPC_CHECK_D
    if (!error.empty()) {
        return nullptr;
    }
    if (caps.external_memory_host && f.vkGetMemoryHostPointerPropertiesEXT == nullptr) {
        caps.external_memory_host = false;
    }
    if (caps.vertex_input_dynamic && f.vkCmdSetVertexInputEXT == nullptr) {
        caps.vertex_input_dynamic = false;
    }
    if (caps.color_write_mask_dynamic && f.vkCmdSetColorWriteMaskEXT == nullptr) {
        caps.color_write_mask_dynamic = false;
    }
    if (caps.calibrated_timestamps && f.vkGetCalibratedTimestampsEXT == nullptr) {
        caps.calibrated_timestamps = false;
    }
    f.vkGetDeviceQueue(ctx->device_, ctx->family_, 0, &ctx->queue_);

    std::string list;
    for (const char* e : denable) {
        list += (list.empty() ? "" : " ") + std::string(e);
    }
    SSPC_LOG(GPU, Info,
             "native backend: Vulkan device {} (driver {:#x}), queue family {}; external host memory {} (alignment {}), "
             "vertex input dynamic {}, colour write mask dynamic {}, push descriptors {}, validation {}; extensions: {}",
             ctx->name_, ctx->driver_version_, ctx->family_, caps.external_memory_host,
             caps.min_imported_host_alignment, caps.vertex_input_dynamic, caps.color_write_mask_dynamic,
             caps.max_push_descriptors, caps.validation ? "on" : "off", list);
    return ctx;
}

DeviceContext::~DeviceContext() {
    Api& f = g_api;
    if (device_ != VK_NULL_HANDLE) {
        f.vkDeviceWaitIdle(device_);
        f.vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE && f.vkDestroyDebugUtilsMessengerEXT != nullptr) {
        f.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        f.vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    f = Api{};
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

void DeviceContext::Fail(const char* where, VkResult result) {
    if (result == VK_SUCCESS) {
        return;
    }
    VkResult expected = VK_SUCCESS;
    if (failure_.compare_exchange_strong(expected, result, std::memory_order_acq_rel)) {
        SSPC_LOG(GPU, Error, "native backend failure: {} returned VkResult {}; submissions stopped",
                 where, static_cast<int>(result));
    }
}

s32 DeviceContext::FindMemoryType(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
    for (u32 i = 0; i < memory_.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory_.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want && (flags & avoid) == 0) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

void DeviceContext::QueryProfile(Gpu::Shader::DeviceProfile& out) const {
    // The same rules as the P4.3 foundation (vk_foundation.cpp QueryProfile) on the same physical device, so the
    // modules translated for it are the ones the self-test and the offline verification checked.
    Api& f = g_api;
    u32 ne = 0;
    f.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    f.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ne, exts.data());
    VkPhysicalDeviceVulkan12Properties p12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                           .pNext = &p12};
    VkPhysicalDeviceProperties2 p2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
    f.vkGetPhysicalDeviceProperties2(physical_, &p2);
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    f.vkGetPhysicalDeviceFeatures2(physical_, &f2);
    const VkPhysicalDeviceLimits& lim = p2.properties.limits;
    const bool nvidia = p12.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    out = {};
    out.max_viewport_width = lim.maxViewportDimensions[0];
    out.max_viewport_height = lim.maxViewportDimensions[1];
    out.max_shared_memory_size = lim.maxComputeSharedMemorySize;
    out.supported_spirv = 0x00010600;
    out.subgroup_size = p11.subgroupSize;
    out.sparse_page_shift = 16;
    out.support_int8 = v12.shaderInt8 != 0;
    out.support_int16 = f2.features.shaderInt16 != 0;
    out.support_int64 = f2.features.shaderInt64 != 0;
    out.support_float16 = v12.shaderFloat16 != 0;
    out.support_float64 = f2.features.shaderFloat64 != 0;
    out.supports_denorm_behavior_independence =
        p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    out.supports_rounding_mode_independence = p12.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    out.support_fp16_denorm_preserve = p12.shaderDenormPreserveFloat16 != 0;
    out.support_fp16_denorm_flush = p12.shaderDenormFlushToZeroFloat16 != 0;
    out.support_fp16_round_to_zero = p12.shaderRoundingModeRTZFloat16 != 0;
    out.support_fp32_denorm_preserve = p12.shaderDenormPreserveFloat32 != 0;
    out.support_fp32_denorm_flush = p12.shaderDenormFlushToZeroFloat32 != 0;
    out.support_fp32_round_to_zero = p12.shaderRoundingModeRTZFloat32 != 0;
    out.support_fp64_denorm_preserve = p12.shaderDenormPreserveFloat64 != 0;
    out.support_fp64_denorm_flush = p12.shaderDenormFlushToZeroFloat64 != 0;
    out.support_fp64_round_to_zero = p12.shaderRoundingModeRTZFloat64 != 0;
    out.support_fp16_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat16 != 0;
    out.support_fp32_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
    out.support_fp64_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat64 != 0;
    out.supports_image_load_store_lod = Has(exts, "VK_AMD_shader_image_load_store_lod");
    out.supports_native_cube_calc = Has(exts, "VK_AMD_gcn_shader");
    out.supports_trinary_minmax = Has(exts, "VK_AMD_shader_trinary_minmax");
    out.supports_buffer_int64_atomics = v12.shaderBufferInt64Atomics != 0;
    out.supports_shared_int64_atomics = v12.shaderSharedInt64Atomics != 0;
    out.supports_workgroup_explicit_memory_layout = Has(exts, "VK_KHR_workgroup_memory_explicit_layout");
    out.supports_amd_shader_explicit_vertex_parameter = Has(exts, "VK_AMD_shader_explicit_vertex_parameter");
    out.supports_fragment_shader_barycentric = Has(exts, "VK_KHR_fragment_shader_barycentric");
    out.supports_shader_subgroup_clock = Has(exts, "VK_KHR_shader_clock");
    out.needs_manual_interpolation = out.supports_fragment_shader_barycentric && nvidia;
    // SSPC_DIAG_NO_MANUAL_INTERP=1: fixed-function interpolation instead of the per-vertex inputs (an experiment, P4.5)
    if (char v[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_NO_MANUAL_INTERP", v, sizeof(v)) == 1 && v[0] == '1') {
        out.needs_manual_interpolation = false;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_NO_MANUAL_INTERP=1 - no manual interpolation");
    }
    out.needs_lds_barriers = nvidia;
    out.needs_buffer_offsets = lim.minStorageBufferOffsetAlignment > 4;
    out.needs_clip_distance_emulation = nvidia;
    out.supports_shader_stencil_export = Has(exts, "VK_EXT_shader_stencil_export");
    out.depth_clip_control = Has(exts, "VK_EXT_depth_clip_control");
    out.source = fmt::format("{} (driver {:#x})", name_, driver_version_);
}

DeviceContext::Budget DeviceContext::QueryBudget() const {
    Budget b;
    if (!caps_.memory_budget || g_api.vkGetPhysicalDeviceMemoryProperties2 == nullptr) {
        return b;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget};
    g_api.vkGetPhysicalDeviceMemoryProperties2(physical_, &mp);
    for (u32 h = 0; h < mp.memoryProperties.memoryHeapCount; ++h) {
        if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            b.device_local_budget += budget.heapBudget[h];
            b.device_local_usage += budget.heapUsage[h];
        } else {
            b.host_budget += budget.heapBudget[h];
            b.host_usage += budget.heapUsage[h];
        }
    }
    return b;
}

void DeviceContext::Name(VkObjectType type, u64 handle, const char* name) const {
    if (!caps_.debug_utils || g_api.vkSetDebugUtilsObjectNameEXT == nullptr || handle == 0) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                             .objectType = type,
                                             .objectHandle = handle,
                                             .pObjectName = name};
    g_api.vkSetDebugUtilsObjectNameEXT(device_, &info);
}

u64 DeviceContext::ValidationErrors() const {
    return g_val_errors.load(std::memory_order_relaxed);
}

u64 DeviceContext::ValidationWarnings() const {
    return g_val_warnings.load(std::memory_order_relaxed);
}

std::string DeviceContext::FirstValidationError() const {
    std::scoped_lock lock{g_val_mutex};
    return g_val_first;
}

} // namespace SecondSon::Gpu::Vk
