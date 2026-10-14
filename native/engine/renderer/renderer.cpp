#include "renderer/renderer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/file.h"
#include "core/log.h"
#include "platform/window.h"
#include "shaders/mesh_frag_spv.h"
#include "shaders/mesh_vert_spv.h"

namespace sspc {

using vk::api;

namespace {

constexpr u32 kVertexStride = 32;

struct PushConstants {
    float mvp[16];
    float world_row0[4];
    float world_row1[4];
    float world_row2[4];
    float color[4];
};
static_assert(sizeof(PushConstants) == 128);

#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        const VkResult r_ = (expr);                                                          \
        if (r_ != VK_SUCCESS) {                                                              \
            log::Error("vulkan", "{} failed: {}", #expr, vk::ResultString(r_));              \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                  VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    api.vkCmdPipelineBarrier2(cmd, &dep);
}

bool WriteBmp(const std::wstring& path, const u8* pixels, u32 w, u32 h, u32 row_pitch, bool bgra) {
    const u32 row = (w * 3 + 3) & ~3u;
    std::vector<u8> out(54 + static_cast<size_t>(row) * h, 0);
    auto put32 = [&](size_t o, u32 v) { std::memcpy(out.data() + o, &v, 4); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<u32>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1;
    out[28] = 24;
    put32(34, static_cast<u32>(row) * h);
    for (u32 y = 0; y < h; ++y) {
        const u8* src = pixels + static_cast<size_t>(y) * row_pitch;
        u8* dst = out.data() + 54 + static_cast<size_t>(h - 1 - y) * row;
        for (u32 x = 0; x < w; ++x) {
            const u8* p = src + x * 4;
            dst[x * 3 + 0] = bgra ? p[0] : p[2];
            dst[x * 3 + 1] = p[1];
            dst[x * 3 + 2] = bgra ? p[2] : p[0];
        }
    }
    return file::WriteAtomic(path, out);
}

// Smooth vertex normals from face normals (for meshes without a normal stream).
std::vector<Vec3> ComputeNormals(const res::MeshData& m) {
    std::vector<Vec3> n(m.positions.size(), Vec3{0, 0, 0});
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vec3& a = m.positions[m.indices[i]];
        const Vec3& b = m.positions[m.indices[i + 1]];
        const Vec3& c = m.positions[m.indices[i + 2]];
        const Vec3 f = Cross(b - a, c - a);
        n[m.indices[i]] += f;
        n[m.indices[i + 1]] += f;
        n[m.indices[i + 2]] += f;
    }
    for (auto& v : n) v = Normalize(v);
    return n;
}

}  // namespace

Renderer::~Renderer() { Shutdown(); }

VKAPI_ATTR VkBool32 VKAPI_CALL Renderer::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                       void* user) {
    auto* self = static_cast<Renderer*>(user);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++self->stats_.validation_messages;
        const bool err = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        log::Write(err ? log::Level::Error : log::Level::Warn, "vk-validation",
                   data && data->pMessage ? data->pMessage : "(null)");
    }
    return VK_FALSE;
}

bool Renderer::Init(Window& window, const RendererDesc& desc) {
    window_ = &window;
    desc_ = desc;
    if (!vk::LoadLoader()) return false;
    if (!CreateInstance(desc)) return false;
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = window.Instance();
    sci.hwnd = window.Handle();
    VK_CHECK(api.vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_));
    if (!PickDevice() || !CreateDevice()) return false;

    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queue_family_;
        VK_CHECK(api.vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(api.vkAllocateCommandBuffers(device_, &ai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(api.vkCreateFence(device_, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(api.vkCreateSemaphore(device_, &sem, nullptr, &f.acquired));
    }
    if (timestamps_supported_) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2 * kFramesInFlight;
        VK_CHECK(api.vkCreateQueryPool(device_, &qci, nullptr, &query_pool_));
    }
    if (!CreateSwapchain() || !CreatePipeline()) return false;
    initialized_ = true;
    log::Info("renderer", "ready: {} | {}x{} | format {} | vsync {} | validation {}", device_name_,
              extent_.width, extent_.height, static_cast<int>(swapchain_format_), desc.vsync,
              messenger_ != VK_NULL_HANDLE);
    return true;
}

bool Renderer::CreateInstance(const RendererDesc& desc) {
    u32 version = VK_API_VERSION_1_0;
    if (api.vkEnumerateInstanceVersion) api.vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        log::Error("vulkan", "Vulkan 1.3 loader required (have {}.{})", VK_API_VERSION_MAJOR(version),
                   VK_API_VERSION_MINOR(version));
        return false;
    }
    u32 n = 0;
    api.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    api.vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    std::vector<const char*> enabled = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    debug_utils_ = HasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils_) enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (desc.validation) {
        api.vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        api.vkEnumerateInstanceLayerProperties(&n, props.data());
        const bool have = std::any_of(props.begin(), props.end(), [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (have) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        } else {
            log::Warn("vulkan", "validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SecondSonPC";
    app.pEngineName = "SecondSonPC Native";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<u32>(enabled.size());
    ici.ppEnabledExtensionNames = enabled.data();
    ici.enabledLayerCount = static_cast<u32>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(api.vkCreateInstance(&ici, nullptr, &instance_));
    if (!vk::LoadInstance(instance_)) return false;
    if (!layers.empty() && debug_utils_ && api.vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = &Renderer::DebugCallback;
        mci.pUserData = this;
        VK_CHECK(api.vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_));
    }
    return true;
}

bool Renderer::PickDevice() {
    u32 n = 0;
    api.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    api.vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        api.vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        u32 en = 0;
        api.vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        api.vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, exts.data());
        if (!HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        api.vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2) continue;
        u32 qn = 0;
        api.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        api.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        for (u32 q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            api.vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                physical_ = pd;
                queue_family_ = q;
                device_name_ = props.deviceName;
                timestamp_period_ns_ = props.limits.timestampPeriod;
                timestamps_supported_ = qf[q].timestampValidBits > 0 && props.limits.timestampPeriod > 0;
            }
            break;
        }
    }
    if (!physical_) {
        log::Error("vulkan", "no GPU with Vulkan 1.3, dynamic rendering, synchronization2 and presentation");
        return false;
    }
    api.vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);
    return true;
}

bool Renderer::CreateDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VK_CHECK(api.vkCreateDevice(physical_, &dci, nullptr, &device_));
    if (!vk::LoadDevice(device_)) return false;
    api.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool Renderer::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
    u32 n = 0;
    api.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    api.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    api.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    api.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!desc_.vsync) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
        }
    }
    extent_ = caps.currentExtent;
    if (extent_.width == 0xffffffffu) {
        extent_ = {std::clamp(window_->Width(), caps.minImageExtent.width, caps.maxImageExtent.width),
                   std::clamp(window_->Height(), caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    if (extent_.width == 0 || extent_.height == 0) return true;  // minimised; retried later
    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    swapchain_transfer_src_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     (swapchain_transfer_src_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR next = VK_NULL_HANDLE;
    VK_CHECK(api.vkCreateSwapchainKHR(device_, &sci, nullptr, &next));
    DestroySwapchain();
    swapchain_ = next;
    swapchain_format_ = chosen.format;

    api.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapchain_images_.resize(n);
    api.vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());
    for (VkImage img : swapchain_images_) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchain_format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK_CHECK(api.vkCreateImageView(device_, &vci, nullptr, &view));
        swapchain_views_.push_back(view);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        VK_CHECK(api.vkCreateSemaphore(device_, &sem, nullptr, &s));
        render_done_.push_back(s);
    }
    return CreateDepth();
}

void Renderer::DestroySwapchain() {
    if (!device_) return;
    for (VkImageView v : swapchain_views_) api.vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : render_done_) api.vkDestroySemaphore(device_, s, nullptr);
    swapchain_views_.clear();
    render_done_.clear();
    swapchain_images_.clear();
    if (depth_view_) api.vkDestroyImageView(device_, depth_view_, nullptr);
    if (depth_image_) api.vkDestroyImage(device_, depth_image_, nullptr);
    if (depth_memory_) api.vkFreeMemory(device_, depth_memory_, nullptr);
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    if (swapchain_) api.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Renderer::CreateDepth() {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depth_format_;
    ici.extent = {extent_.width, extent_.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(api.vkCreateImage(device_, &ici, nullptr, &depth_image_));
    VkMemoryRequirements req;
    api.vkGetImageMemoryRequirements(device_, depth_image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(api.vkAllocateMemory(device_, &mai, nullptr, &depth_memory_));
    VK_CHECK(api.vkBindImageMemory(device_, depth_image_, depth_memory_, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depth_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depth_format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(api.vkCreateImageView(device_, &vci, nullptr, &depth_view_));
    SetName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(depth_image_), "depth");
    return true;
}

bool Renderer::CreatePipeline() {
    std::vector<u8> cache_data;
    if (!desc_.pipeline_cache_path.empty()) file::ReadAll(desc_.pipeline_cache_path, cache_data);
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pcci.initialDataSize = cache_data.size();
    pcci.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    if (api.vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pcci.initialDataSize = 0;  // incompatible/corrupt cache data: start empty
        pcci.pInitialData = nullptr;
        VK_CHECK(api.vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_));
    }

    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kMeshVertSpv);
    smci.pCode = kMeshVertSpv;
    VK_CHECK(api.vkCreateShaderModule(device_, &smci, nullptr, &vs));
    smci.codeSize = sizeof(kMeshFragSpv);
    smci.pCode = kMeshFragSpv;
    VK_CHECK(api.vkCreateShaderModule(device_, &smci, nullptr, &fs));

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(api.vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                  {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // The game's meshes are counter-clockwise when seen from outside (checked on all 41 hero parts:
    // face normals agree with vertex normals for 100 % of triangles, tests/test_real_assets.cpp).
    // The projection flips Y and Vulkan's framebuffer Y points down: the two mirrors cancel, so
    // outward faces stay COUNTER_CLOCKWISE (verified by screenshot: CLOCKWISE culled the ground).
    rs.cullMode = desc_.cull_back_faces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &swapchain_format_;
    rci.depthAttachmentFormat = depth_format_;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = pipeline_layout_;
    const VkResult r = api.vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &gp, nullptr, &pipeline_);
    api.vkDestroyShaderModule(device_, vs, nullptr);
    api.vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) {
        log::Error("vulkan", "vkCreateGraphicsPipelines failed: {}", vk::ResultString(r));
        return false;
    }
    SetName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline_), "mesh-lit");
    return true;
}

u32 Renderer::FindMemoryType(u32 type_bits, VkMemoryPropertyFlags props) const {
    for (u32 i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return 0;
}

bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buffer& out) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VK_CHECK(api.vkCreateBuffer(device_, &bci, nullptr, &out.buffer));
    VkMemoryRequirements req;
    api.vkGetBufferMemoryRequirements(device_, out.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    VK_CHECK(api.vkAllocateMemory(device_, &mai, nullptr, &out.memory));
    VK_CHECK(api.vkBindBufferMemory(device_, out.buffer, out.memory, 0));
    out.size = size;
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK_CHECK(api.vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    }
    return true;
}

void Renderer::DestroyBuffer(Buffer& b) {
    if (!device_) return;
    if (b.mapped) api.vkUnmapMemory(device_, b.memory);
    if (b.buffer) api.vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) api.vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer{};
}

void Renderer::SetName(VkObjectType type, u64 handle, const char* name) {
    if (!api.vkSetDebugUtilsObjectNameEXT || !name || !device_ || !debug_utils_) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    api.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

bool Renderer::SubmitImmediate(void (*record)(VkCommandBuffer, void*), void* user) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queue_family_;
    VkCommandPool pool;
    VK_CHECK(api.vkCreateCommandPool(device_, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    api.vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.vkBeginCommandBuffer(cmd, &bi);
    record(cmd, user);
    api.vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;
    const VkResult r = api.vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) api.vkQueueWaitIdle(queue_);
    api.vkDestroyCommandPool(device_, pool, nullptr);
    return r == VK_SUCCESS;
}

MeshHandle Renderer::UploadMesh(const res::MeshData& mesh, const char* debug_name) {
    if (!initialized_ || mesh.positions.empty() || mesh.indices.empty()) return kInvalidMesh;
    const std::vector<Vec3> generated = mesh.normals.size() == mesh.positions.size()
                                            ? std::vector<Vec3>{}
                                            : ComputeNormals(mesh);
    const std::vector<Vec3>& normals = generated.empty() ? mesh.normals : generated;
    std::vector<float> verts(mesh.positions.size() * 8);
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        float* v = &verts[i * 8];
        v[0] = mesh.positions[i].x;
        v[1] = mesh.positions[i].y;
        v[2] = mesh.positions[i].z;
        v[3] = normals[i].x;
        v[4] = normals[i].y;
        v[5] = normals[i].z;
        v[6] = mesh.uv0.empty() ? 0.0f : mesh.uv0[i].x;
        v[7] = mesh.uv0.empty() ? 0.0f : mesh.uv0[i].y;
    }
    const VkDeviceSize vbytes = verts.size() * sizeof(float);
    const VkDeviceSize ibytes = mesh.indices.size() * sizeof(u32);
    Buffer staging;
    GpuMesh gm;
    if (!CreateBuffer(vbytes + ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging) ||
        !CreateBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gm.vertices) ||
        !CreateBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gm.indices)) {
        DestroyBuffer(staging);
        DestroyBuffer(gm.vertices);
        DestroyBuffer(gm.indices);
        return kInvalidMesh;
    }
    std::memcpy(staging.mapped, verts.data(), vbytes);
    std::memcpy(static_cast<u8*>(staging.mapped) + vbytes, mesh.indices.data(), ibytes);
    struct Ctx {
        Buffer* staging;
        GpuMesh* gm;
        VkDeviceSize vbytes, ibytes;
    } ctx{&staging, &gm, vbytes, ibytes};
    const bool ok = SubmitImmediate(
        [](VkCommandBuffer cmd, void* user) {
            auto* c = static_cast<Ctx*>(user);
            VkBufferCopy v{0, 0, c->vbytes};
            api.vkCmdCopyBuffer(cmd, c->staging->buffer, c->gm->vertices.buffer, 1, &v);
            VkBufferCopy i{c->vbytes, 0, c->ibytes};
            api.vkCmdCopyBuffer(cmd, c->staging->buffer, c->gm->indices.buffer, 1, &i);
        },
        &ctx);
    DestroyBuffer(staging);
    if (!ok) {
        DestroyBuffer(gm.vertices);
        DestroyBuffer(gm.indices);
        return kInvalidMesh;
    }
    gm.index_count = static_cast<u32>(mesh.indices.size());
    SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(gm.vertices.buffer), debug_name);
    meshes_.push_back(gm);
    return static_cast<MeshHandle>(meshes_.size() - 1);
}

float Renderer::AspectRatio() const {
    return extent_.height ? static_cast<float>(extent_.width) / static_cast<float>(extent_.height) : 1.0f;
}

void Renderer::ReadTimestamps(Frame& f, u32 frame_index) {
    if (!query_pool_ || !f.timestamps_written) return;
    u64 ts[2] = {};
    if (api.vkGetQueryPoolResults(device_, query_pool_, frame_index * 2, 2, sizeof(ts), ts, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
        stats_.gpu_ms = static_cast<double>(ts[1] - ts[0]) * timestamp_period_ns_ * 1e-6;
    }
}

bool Renderer::RenderFrame(const FrameView& view, std::span<const DrawItem> items) {
    if (!initialized_) return false;
    if (window_->Minimized()) {
        Sleep(10);
        return true;
    }
    if (window_->ConsumeResized() || swapchain_ == VK_NULL_HANDLE) {
        api.vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) return false;
        ++stats_.swapchain_recreations;
        if (swapchain_ == VK_NULL_HANDLE) return true;
    }
    Frame& f = frames_[frame_index_];
    VK_CHECK(api.vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));
    ReadTimestamps(f, frame_index_);

    u32 image = 0;
    VkResult r = api.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        api.vkDeviceWaitIdle(device_);
        ++stats_.swapchain_recreations;
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        log::Error("vulkan", "vkAcquireNextImageKHR: {}", vk::ResultString(r));
        return false;
    }
    VK_CHECK(api.vkResetFences(device_, 1, &f.fence));
    VK_CHECK(api.vkResetCommandPool(device_, f.pool, 0));
    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(api.vkBeginCommandBuffer(cmd, &bi));
    if (query_pool_) {
        api.vkCmdResetQueryPool(cmd, query_pool_, frame_index_ * 2, 2);
        api.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, query_pool_, frame_index_ * 2);
    }
    if (debug_utils_ && api.vkCmdBeginDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = "scene";
        api.vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    }

    const VkImage color = swapchain_images_[image];
    ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    ImageBarrier(cmd, depth_image_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = swapchain_views_[image];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{view.clear_color.x, view.clear_color.y, view.clear_color.z, 1.0f}};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth_view_;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    api.vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    VkRect2D sc{{0, 0}, extent_};
    api.vkCmdSetViewport(cmd, 0, 1, &vp);
    api.vkCmdSetScissor(cmd, 0, 1, &sc);
    api.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const Mat4 view_proj = view.projection * view.view;
    stats_.draws = 0;
    stats_.triangles = 0;
    for (const DrawItem& item : items) {
        if (item.mesh >= meshes_.size()) continue;
        const GpuMesh& gm = meshes_[item.mesh];
        PushConstants pc;
        const Mat4 mvp = view_proj * item.world;
        std::memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        for (int r3 = 0; r3 < 3; ++r3) {
            float* row = r3 == 0 ? pc.world_row0 : r3 == 1 ? pc.world_row1 : pc.world_row2;
            row[0] = item.world.m[0][r3];
            row[1] = item.world.m[1][r3];
            row[2] = item.world.m[2][r3];
            row[3] = 0;
        }
        pc.color[0] = item.color.x;
        pc.color[1] = item.color.y;
        pc.color[2] = item.color.z;
        pc.color[3] = item.color.w;
        api.vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
        const VkDeviceSize zero = 0;
        api.vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vertices.buffer, &zero);
        api.vkCmdBindIndexBuffer(cmd, gm.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        api.vkCmdDrawIndexed(cmd, gm.index_count, 1, 0, 0, 0);
        ++stats_.draws;
        stats_.triangles += gm.index_count / 3;
    }
    api.vkCmdEndRendering(cmd);

    const bool capture = !screenshot_path_.empty() && swapchain_transfer_src_;
    if (capture) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
        if (screenshot_buffer_.size < bytes) {
            DestroyBuffer(screenshot_buffer_);
            CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         screenshot_buffer_);
        }
        ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        api.vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshot_buffer_.buffer, 1,
                                   &region);
        ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    if (debug_utils_ && api.vkCmdEndDebugUtilsLabelEXT) api.vkCmdEndDebugUtilsLabelEXT(cmd);
    if (query_pool_) {
        api.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, query_pool_, frame_index_ * 2 + 1);
        f.timestamps_written = true;
    }
    VK_CHECK(api.vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_done_[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(api.vkQueueSubmit2(queue_, 1, &si, f.fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    r = api.vkQueuePresentKHR(queue_, &pi);
    if (capture) {
        api.vkQueueWaitIdle(queue_);
        WriteScreenshot();
    }
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        api.vkDeviceWaitIdle(device_);
        ++stats_.swapchain_recreations;
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS) {
        log::Error("vulkan", "vkQueuePresentKHR: {}", vk::ResultString(r));
        return false;
    }
    return true;
}

void Renderer::WriteScreenshot() {
    const bool bgra = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM;
    screenshot_ok_ = WriteBmp(screenshot_path_, static_cast<const u8*>(screenshot_buffer_.mapped), extent_.width,
                              extent_.height, extent_.width * 4, bgra);
    log::Info("renderer", "screenshot {} -> {}", screenshot_ok_ ? "written" : "FAILED",
              log::Narrow(screenshot_path_));
    screenshot_path_.clear();
}

void Renderer::SavePipelineCache() {
    if (!pipeline_cache_ || desc_.pipeline_cache_path.empty()) return;
    size_t size = 0;
    if (api.vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<u8> data(size);
    if (api.vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) == VK_SUCCESS) {
        data.resize(size);
        file::WriteAtomic(desc_.pipeline_cache_path, data);
    }
}

void Renderer::Shutdown() {
    if (device_) {
        api.vkDeviceWaitIdle(device_);
        SavePipelineCache();
        for (GpuMesh& m : meshes_) {
            DestroyBuffer(m.vertices);
            DestroyBuffer(m.indices);
        }
        meshes_.clear();
        DestroyBuffer(screenshot_buffer_);
        DestroySwapchain();
        if (pipeline_) api.vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipeline_layout_) api.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (pipeline_cache_) api.vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        if (query_pool_) api.vkDestroyQueryPool(device_, query_pool_, nullptr);
        for (Frame& f : frames_) {
            if (f.pool) api.vkDestroyCommandPool(device_, f.pool, nullptr);
            if (f.fence) api.vkDestroyFence(device_, f.fence, nullptr);
            if (f.acquired) api.vkDestroySemaphore(device_, f.acquired, nullptr);
            f = Frame{};
        }
        api.vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        if (surface_) api.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (messenger_ && api.vkDestroyDebugUtilsMessengerEXT) {
            api.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        api.vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        messenger_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

}  // namespace sspc
