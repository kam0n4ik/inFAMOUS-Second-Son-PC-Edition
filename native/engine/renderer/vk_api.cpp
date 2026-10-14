#include "renderer/vk_api.h"

#include <windows.h>

#include "core/log.h"

namespace sspc::vk {

Api api;

namespace {
HMODULE g_module = nullptr;
}

bool LoadLoader() {
    if (g_module) return true;
    g_module = LoadLibraryW(L"vulkan-1.dll");
    if (!g_module) {
        log::Error("vulkan", "vulkan-1.dll not found: no Vulkan driver installed?");
        return false;
    }
    api.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_module, "vkGetInstanceProcAddr"));
    if (!api.vkGetInstanceProcAddr) return false;
#define SSPC_VK_LOAD_GLOBAL(name) \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetInstanceProcAddr(nullptr, #name));
    SSPC_VK_GLOBAL_FUNCS(SSPC_VK_LOAD_GLOBAL)
#undef SSPC_VK_LOAD_GLOBAL
    return api.vkCreateInstance != nullptr;
}

bool LoadInstance(VkInstance instance) {
    bool ok = true;
#define SSPC_VK_LOAD_INSTANCE(name)                                                         \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetInstanceProcAddr(instance, #name));    \
    if (!api.name) {                                                                        \
        log::Error("vulkan", "missing instance function {}", #name);                        \
        ok = false;                                                                         \
    }
    SSPC_VK_INSTANCE_FUNCS(SSPC_VK_LOAD_INSTANCE)
#undef SSPC_VK_LOAD_INSTANCE
#define SSPC_VK_LOAD_OPTIONAL(name) \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetInstanceProcAddr(instance, #name));
    SSPC_VK_INSTANCE_OPTIONAL_FUNCS(SSPC_VK_LOAD_OPTIONAL)
#undef SSPC_VK_LOAD_OPTIONAL
    return ok;
}

bool LoadDevice(VkDevice device) {
    bool ok = true;
#define SSPC_VK_LOAD_DEVICE(name)                                                   \
    api.name = reinterpret_cast<PFN_##name>(api.vkGetDeviceProcAddr(device, #name)); \
    if (!api.name) {                                                                \
        log::Error("vulkan", "missing device function {}", #name);                  \
        ok = false;                                                                 \
    }
    SSPC_VK_DEVICE_FUNCS(SSPC_VK_LOAD_DEVICE)
#undef SSPC_VK_LOAD_DEVICE
    return ok;
}

void UnloadLoader() {
    if (g_module) FreeLibrary(g_module);
    g_module = nullptr;
    api = Api{};
}

const char* ResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult(other)";
    }
}

}  // namespace sspc::vk
