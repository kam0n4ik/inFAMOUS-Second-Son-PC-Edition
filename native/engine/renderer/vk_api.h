// SecondSonPC native runtime - Vulkan function table.
//
// vulkan-1.dll is loaded at run time (no import library), so the exe starts even without a
// Vulkan driver and can report a clear error. Headers come from the LunarG Vulkan SDK.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace sspc::vk {

#define SSPC_VK_GLOBAL_FUNCS(X)              \
    X(vkCreateInstance)                      \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)    \
    X(vkEnumerateInstanceVersion)

#define SSPC_VK_INSTANCE_FUNCS(X)                  \
    X(vkDestroyInstance)                           \
    X(vkEnumeratePhysicalDevices)                  \
    X(vkGetPhysicalDeviceProperties)               \
    X(vkGetPhysicalDeviceFeatures2)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties)         \
    X(vkGetPhysicalDeviceFormatProperties)         \
    X(vkCreateDevice)                              \
    X(vkGetDeviceProcAddr)                         \
    X(vkEnumerateDeviceExtensionProperties)        \
    X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)   \
    X(vkCreateWin32SurfaceKHR)

#define SSPC_VK_INSTANCE_OPTIONAL_FUNCS(X) \
    X(vkCreateDebugUtilsMessengerEXT)      \
    X(vkDestroyDebugUtilsMessengerEXT)     \
    X(vkCmdBeginDebugUtilsLabelEXT)        \
    X(vkCmdEndDebugUtilsLabelEXT)          \
    X(vkSetDebugUtilsObjectNameEXT)

#define SSPC_VK_DEVICE_FUNCS(X)         \
    X(vkDestroyDevice)                  \
    X(vkGetDeviceQueue)                 \
    X(vkDeviceWaitIdle)                 \
    X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR)            \
    X(vkGetSwapchainImagesKHR)          \
    X(vkAcquireNextImageKHR)            \
    X(vkQueuePresentKHR)                \
    X(vkQueueSubmit2)                   \
    X(vkQueueWaitIdle)                  \
    X(vkCreateImageView)                \
    X(vkDestroyImageView)               \
    X(vkCreateImage)                    \
    X(vkDestroyImage)                   \
    X(vkGetImageMemoryRequirements)     \
    X(vkBindImageMemory)                \
    X(vkCreateBuffer)                   \
    X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements)    \
    X(vkBindBufferMemory)               \
    X(vkAllocateMemory)                 \
    X(vkFreeMemory)                     \
    X(vkMapMemory)                      \
    X(vkUnmapMemory)                    \
    X(vkCreateCommandPool)              \
    X(vkDestroyCommandPool)             \
    X(vkResetCommandPool)               \
    X(vkAllocateCommandBuffers)         \
    X(vkBeginCommandBuffer)             \
    X(vkEndCommandBuffer)               \
    X(vkCreateFence)                    \
    X(vkDestroyFence)                   \
    X(vkWaitForFences)                  \
    X(vkResetFences)                    \
    X(vkCreateSemaphore)                \
    X(vkDestroySemaphore)               \
    X(vkCreateShaderModule)             \
    X(vkDestroyShaderModule)            \
    X(vkCreatePipelineLayout)           \
    X(vkDestroyPipelineLayout)          \
    X(vkCreateGraphicsPipelines)        \
    X(vkDestroyPipeline)                \
    X(vkCreatePipelineCache)            \
    X(vkDestroyPipelineCache)           \
    X(vkGetPipelineCacheData)           \
    X(vkCmdPipelineBarrier2)            \
    X(vkCmdBeginRendering)              \
    X(vkCmdEndRendering)                \
    X(vkCmdBindPipeline)                \
    X(vkCmdSetViewport)                 \
    X(vkCmdSetScissor)                  \
    X(vkCmdBindVertexBuffers)           \
    X(vkCmdBindIndexBuffer)             \
    X(vkCmdPushConstants)               \
    X(vkCmdDrawIndexed)                 \
    X(vkCmdCopyBuffer)                  \
    X(vkCmdCopyImageToBuffer)           \
    X(vkCreateQueryPool)                \
    X(vkDestroyQueryPool)               \
    X(vkCmdResetQueryPool)              \
    X(vkCmdWriteTimestamp2)             \
    X(vkGetQueryPoolResults)            \
    X(vkCreateSampler)                  \
    X(vkDestroySampler)                 \
    X(vkCreateDescriptorSetLayout)      \
    X(vkDestroyDescriptorSetLayout)     \
    X(vkCreateDescriptorPool)           \
    X(vkDestroyDescriptorPool)          \
    X(vkAllocateDescriptorSets)         \
    X(vkUpdateDescriptorSets)           \
    X(vkCmdBindDescriptorSets)          \
    X(vkCmdCopyBufferToImage)

struct Api {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define SSPC_VK_DECL(name) PFN_##name name = nullptr;
    SSPC_VK_GLOBAL_FUNCS(SSPC_VK_DECL)
    SSPC_VK_INSTANCE_FUNCS(SSPC_VK_DECL)
    SSPC_VK_INSTANCE_OPTIONAL_FUNCS(SSPC_VK_DECL)
    SSPC_VK_DEVICE_FUNCS(SSPC_VK_DECL)
#undef SSPC_VK_DECL
};

// Global table (one Vulkan instance/device per process in this engine).
extern Api api;

bool LoadLoader();  // loads vulkan-1.dll and the global entry points
bool LoadInstance(VkInstance instance);
bool LoadDevice(VkDevice device);
void UnloadLoader();
const char* ResultString(VkResult r);

}  // namespace sspc::vk
