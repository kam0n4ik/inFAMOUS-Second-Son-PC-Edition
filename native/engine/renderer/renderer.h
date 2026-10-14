// SecondSonPC native runtime - scene renderer (Vulkan 1.3).
//
// Input is engine data: meshes uploaded from MeshData and a list of draw items with world
// transforms. There is no console command stream anywhere in this path:
//   game world -> scene -> Renderer::RenderFrame -> Vulkan
#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "renderer/vk_api.h"
#include "resources/geometry.h"
#include "resources/texture.h"

namespace sspc {

class Window;

struct RendererDesc {
    bool validation = false;  // VK_LAYER_KHRONOS_validation + debug messenger (if installed)
    bool vsync = true;
    bool cull_back_faces = true;
    std::wstring pipeline_cache_path;  // empty = no persistent cache
};

using MeshHandle = u32;
inline constexpr MeshHandle kInvalidMesh = 0xffffffffu;
using TextureHandle = u32;
inline constexpr TextureHandle kWhiteTexture = 0;  // created at Init; also the fallback

struct DrawItem {
    MeshHandle mesh = kInvalidMesh;
    Mat4 world = Mat4::Identity();
    Vec4 color{0.8f, 0.8f, 0.8f, 0.0f};  // rgb multiplies the texture; a > 0 = alpha-test cutoff
    TextureHandle texture = kWhiteTexture;
};

struct FrameView {
    Mat4 view = Mat4::Identity();
    Mat4 projection = Mat4::Identity();
    Vec3 clear_color{0.42f, 0.55f, 0.70f};
};

struct RendererStats {
    u32 draws = 0;
    u64 triangles = 0;
    double gpu_ms = 0;  // previous completed frame, from timestamps (0 if unsupported)
    u32 validation_messages = 0;
    u32 swapchain_recreations = 0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(Window& window, const RendererDesc& desc);
    void Shutdown();

    // Uploads positions/normals/uv/indices to device-local buffers. Missing normals are generated.
    MeshHandle UploadMesh(const res::MeshData& mesh, const char* debug_name = nullptr);
    size_t MeshCount() const { return meshes_.size(); }
    // Uploads all mip levels (BCn stay compressed on the GPU). `srgb` selects the sRGB view of
    // colour formats. Returns kWhiteTexture on failure.
    TextureHandle UploadTexture(const res::TextureData& texture, bool srgb);
    size_t TextureCount() const { return textures_.size(); }

    // Records, submits and presents one frame. Returns false on a fatal device error.
    bool RenderFrame(const FrameView& view, std::span<const DrawItem> items);

    // The next rendered frame is copied back and written as a 24-bit BMP.
    void RequestScreenshot(const std::wstring& path) { screenshot_path_ = path; }
    bool LastScreenshotOk() const { return screenshot_ok_; }

    const RendererStats& Stats() const { return stats_; }
    float AspectRatio() const;
    std::string DeviceName() const { return device_name_; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };
    struct GpuMesh {
        Buffer vertices;
        Buffer indices;
        u32 index_count = 0;
    };
    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        bool timestamps_written = false;
    };

    bool CreateInstance(const RendererDesc& desc);
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateDepth();
    bool CreatePipeline();
    bool CreateTextureResources();
    TextureHandle CreateTexture(VkFormat format, u32 width, u32 height, const std::vector<std::vector<u8>>& mips,
                                const char* debug_name);
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buffer& out);
    void DestroyBuffer(Buffer& b);
    u32 FindMemoryType(u32 type_bits, VkMemoryPropertyFlags props) const;
    void SetName(VkObjectType type, u64 handle, const char* name);
    bool SubmitImmediate(void (*record)(VkCommandBuffer, void*), void* user);
    void ReadTimestamps(Frame& f, u32 frame_index);
    void WriteScreenshot();
    void SavePipelineCache();
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                        void* user);

    Window* window_ = nullptr;
    RendererDesc desc_;
    bool initialized_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    float timestamp_period_ns_ = 0;
    bool timestamps_supported_ = false;
    bool debug_utils_ = false;
    std::string device_name_;
    u32 queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    bool swapchain_transfer_src_ = false;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;

    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<GpuTexture> textures_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkQueryPool query_pool_ = VK_NULL_HANDLE;

    static constexpr u32 kFramesInFlight = 2;
    Frame frames_[kFramesInFlight];
    u32 frame_index_ = 0;
    std::vector<GpuMesh> meshes_;

    std::wstring screenshot_path_;
    Buffer screenshot_buffer_;
    bool screenshot_pending_copy_ = false;
    bool screenshot_ok_ = false;
    RendererStats stats_;
};

}  // namespace sspc
