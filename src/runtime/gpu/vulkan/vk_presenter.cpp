// SecondSonPC — the backend's presenter (P4.4). See vk_presenter.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_presenter.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <vulkan/vulkan_win32.h>

#include "base/log.h"
#include "runtime/gpu/png_writer.h"
#include "runtime/gpu/r10/latency.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/upscale/fsr_framegen.h"
#include "runtime/gpu/vulkan/vk_reflex.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

/// The raw-bits twin of a format: a frame keeps the buffer's bits (sRGB-encoded values stay encoded).
VkFormat Unorm(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default:
        return f;
    }
}

VkPresentModeKHR ParsePresentMode(const std::string& present_mode) {
    return present_mode == "Fifo"        ? VK_PRESENT_MODE_FIFO_KHR
           : present_mode == "Immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                         : VK_PRESENT_MODE_MAILBOX_KHR;
}

// R11 display: what the menu shows (GetPresentModeInfo): bit 31 known, bits 0..3 the surface's modes, 8..15 the
// swapchain's mode, 16..23 the wanted one
std::atomic<u32> g_present_info{0};

float HalfToFloat(u16 h) {
    const u32 s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) {
        v = std::ldexp(static_cast<float>(m), -24);
    } else if (e == 31) {
        v = m ? 0.0f : 65504.0f;
    } else {
        v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    }
    return s ? -v : v;
}

u8 ToByte(float v) {
    return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// The FPS counter (P4.6): a 5x7 bitmap font for its few characters, drawn into a small host buffer and copied onto the
// swapchain image after the frame's blit (no pipeline, no shader).
constexpr u32 kOverlayW = 320, kOverlayH = 30, kGlyphScale = 3;

/// Rows of a 5x7 glyph (bit 4 = leftmost column); nullptr: blank.
const u8* Glyph(char c) {
    static constexpr u8 kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
    static constexpr u8 kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static constexpr u8 kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static constexpr u8 kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr u8 kM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static constexpr u8 kDot[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
    static constexpr u8 kSlash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    if (c >= '0' && c <= '9') {
        return kDigits[c - '0'];
    }
    switch (c) {
    case 'F':
        return kF;
    case 'P':
        return kP;
    case 'S':
        return kS;
    case 'M':
        return kM;
    case '.':
        return kDot;
    case '/':
        return kSlash;
    default:
        return nullptr;
    }
}

/// `text` in white on a dark box, B8G8R8A8/R8G8B8A8 bytes (the same for grey values), kOverlayW x kOverlayH.
void DrawOverlay(u8* px, std::string_view text) {
    for (u32 i = 0; i < kOverlayW * kOverlayH; ++i) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 16;
        px[i * 4 + 3] = 255;
    }
    u32 x0 = 6;
    const u32 y0 = (kOverlayH - 7 * kGlyphScale) / 2;
    for (const char c : text) {
        if (x0 + 5 * kGlyphScale > kOverlayW) {
            break;
        }
        if (const u8* g = Glyph(c); g != nullptr) {
            for (u32 row = 0; row < 7; ++row) {
                for (u32 col = 0; col < 5; ++col) {
                    if (((g[row] >> (4 - col)) & 1) == 0) {
                        continue;
                    }
                    for (u32 dy = 0; dy < kGlyphScale; ++dy) {
                        for (u32 dx = 0; dx < kGlyphScale; ++dx) {
                            u8* p = px + ((y0 + row * kGlyphScale + dy) * kOverlayW + x0 + col * kGlyphScale + dx) * 4;
                            p[0] = p[1] = p[2] = 240;
                        }
                    }
                }
            }
        }
        x0 += 6 * kGlyphScale;
    }
}

std::atomic<bool> g_fps_counter_shown{true};

} // namespace

void SetFpsCounterShown(bool shown) {
    g_fps_counter_shown.store(shown, std::memory_order_relaxed);
}

const char* PresentModeName(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    default:
        return "other";
    }
}

PresentModeInfo GetPresentModeInfo() {
    const u32 v = g_present_info.load(std::memory_order_acquire);
    PresentModeInfo info;
    info.known = (v >> 31) != 0;
    info.supported = v & 0xF;
    info.current = static_cast<VkPresentModeKHR>((v >> 8) & 0xFF);
    info.wanted = static_cast<VkPresentModeKHR>((v >> 16) & 0xFF);
    return info;
}

/// An image of the presenter's own (frame generation inputs), recreated for a size or format.
struct OwnImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

void DestroyOwn(DeviceContext& ctx, OwnImage& i) {
    if (i.image != VK_NULL_HANDLE) {
        Fn().vkDestroyImage(ctx.Device(), i.image, nullptr);
    }
    if (i.memory != VK_NULL_HANDLE) {
        Fn().vkFreeMemory(ctx.Device(), i.memory, nullptr);
    }
    i = OwnImage{};
}

bool MakeOwn(DeviceContext& ctx, OwnImage& i, VkFormat format, u32 w, u32 h, VkImageUsageFlags usage) {
    if (i.image != VK_NULL_HANDLE && i.format == format && i.width == w && i.height == h) {
        return true;
    }
    DestroyOwn(ctx, i);
    const auto& f = Fn();
    const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = format,
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (f.vkCreateImage(ctx.Device(), &ici, nullptr, &i.image) != VK_SUCCESS) {
        i.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(ctx.Device(), i.image, &req);
    const s32 type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = static_cast<u32>(std::max(type, 0))};
    if (type < 0 || f.vkAllocateMemory(ctx.Device(), &mai, nullptr, &i.memory) != VK_SUCCESS) {
        i.memory = VK_NULL_HANDLE;
        DestroyOwn(ctx, i);
        return false;
    }
    f.vkBindImageMemory(ctx.Device(), i.image, i.memory, 0);
    i.format = format;
    i.width = w;
    i.height = h;
    return true;
}

void OwnBarrier(VkCommandBuffer cb, OwnImage& i, VkImageLayout to, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access) {
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = dst_stage,
                                  .dstAccessMask = dst_access,
                                  .oldLayout = i.layout,
                                  .newLayout = to,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = i.image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
    Fn().vkCmdPipelineBarrier2(cb, &dep);
    i.layout = to;
}

struct Presenter::Impl {
    DeviceContext& ctx;
    Timeline& timeline;
    SDL_Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR wanted_mode = VK_PRESENT_MODE_MAILBOX_KHR; ///< [Display] presentMode
    bool modes_logged = false;
    VkPresentModeKHR logged_mode = VK_PRESENT_MODE_FIFO_KHR, logged_wanted = VK_PRESENT_MODE_FIFO_KHR;
    std::atomic<int> mode_request{-1}; ///< SetPresentMode: the wanted VkPresentModeKHR, -1 nothing asked
    std::vector<VkImage> images;
    std::vector<VkSemaphore> done; ///< per swapchain image: its commands finished (the present waits on it)

    struct Slot {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence acquire_fence = VK_NULL_HANDLE, present_fence = VK_NULL_HANDLE;
        bool submitted = false, acquire_pending = false, present_pending = false;
        s32 frame = -1; ///< the frame its commands read
        OwnedBuffer overlay; ///< the FPS counter's pixels (free once the slot's fence signalled)
        // R10 latency probe (SSPC_R10_LATENCY=1): the texel grid of the frame this slot presented, read when the
        // slot's fence is met next
        OwnedBuffer probe;
        u64 probe_flip = 0, probe_tsc = 0;
        u32 probe_bpp = 0; ///< 0: nothing copied
        bool probe_pending = false;
    };
    // the FPS counter (P4.6; SSPC_FPS_OVERLAY=0 hides it): presented frames over the last half second
    bool overlay_on = true;
    std::deque<std::chrono::steady_clock::time_point> shown;
    std::string overlay_text;
    std::chrono::steady_clock::time_point overlay_t{};
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<Slot, 2> slots{};
    u32 slot = 0;
    struct PendingCapture { OwnedBuffer buffer; VkCommandBuffer cb; VkFence fence; };
    std::vector<PendingCapture> pending_captures;

    struct Frame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 width = 0, height = 0;
        std::atomic<u32> state{0}; ///< 0 free, 1 the GPU thread's, 2 waiting to be shown, 3 being shown
        VkFence fence = VK_NULL_HANDLE; ///< the presentation slot's fence while being shown
    };
    std::array<Frame, kFrames> frames{};
    std::mutex frames_mutex;

    // FSR 3.1 frame generation (null: off): per frame its inputs, the HUD-less frame at the backbuffer's size
    std::unique_ptr<Upscale::FsrFrameGen> fg;
    std::atomic<bool> fg_active{false}; ///< fg != null (read by the recording thread)
    std::atomic<int> fg_wanted{-1};     ///< SetFrameGen: 0 off, 1 on, -1 nothing asked (the present thread switches)
    struct FgFrame {
        OwnImage depth, motion, hudless;
        bool valid = false;
        Upscale::FsrFrameGen::Inputs in;
    };
    std::array<FgFrame, kFrames> fg_frames{};
    OwnImage fg_hudless_bb;
    std::chrono::steady_clock::time_point fg_last_present{}, fg_window_t{};
    u32 fg_real = 0, fg_shown = 0;
    float fg_real_fps = 0.0f, fg_shown_fps = 0.0f;

    std::mutex capture_mutex;
    std::deque<std::pair<u64, std::filesystem::path>> captures; ///< requested flips (ascending) and their PNGs

    std::mutex stats_mutex;
    Stats stats;

    Impl(DeviceContext& c, Timeline& t) : ctx(c), timeline(t) {}

    bool WaitFence(VkFence fence, bool& pending, u64 timeout) {
        if (!pending) {
            return true;
        }
        const VkResult r = Fn().vkWaitForFences(ctx.Device(), 1, &fence, VK_TRUE, timeout);
        if (r == VK_SUCCESS) {
            pending = false;
            return true;
        }
        {
            std::scoped_lock lock{stats_mutex};
            stats.last_error = fmt::format("presentation retirement wait returned {}", static_cast<int>(r));
            stats.device_lost |= r == VK_ERROR_DEVICE_LOST;
        }
        if (r != VK_TIMEOUT) {
            ctx.Fail("presentation retirement wait", r);
        }
        return false; // pending remains owned; never reset/destroy on timeout
    }

    bool WaitSlot(Slot& s, u64 timeout = 2'000'000'000ull) {
        // AcquireFrame also observes these fences. Retire every reference before a slot's
        // fence can be reset, under the same host-side lock.
        std::scoped_lock lock{frames_mutex};
        if (!WaitFence(s.fence, s.submitted, timeout) ||
            !WaitFence(s.acquire_fence, s.acquire_pending, timeout) ||
            !WaitFence(s.present_fence, s.present_pending, timeout)) {
            return false;
        }
        for (auto& fr : frames) {
            if (fr.state.load(std::memory_order_acquire) == 3 && fr.fence == s.fence) {
                fr.fence = VK_NULL_HANDLE;
                fr.state.store(0, std::memory_order_release);
            }
        }
        if (s.probe_pending) { // (R10: the slot's last present completed - its grid is in the buffer)
            s.probe_pending = false;
            R10::Latency::Present(s.probe_flip, s.probe_tsc,
                                  s.probe_bpp != 0 ? static_cast<const u8*>(s.probe.mapped) : nullptr,
                                  s.probe_bpp != 0 ? s.probe_bpp : 4);
        }
        return true;
    }

    bool CreateSwapchain(std::string& error) {
        const auto& f = Fn();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        VkSurfaceCapabilitiesKHR caps{};
        if (f.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.Physical(), surface, &caps) != VK_SUCCESS) {
            error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
            return false;
        }
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            ext = {static_cast<u32>(std::max(w, 1)), static_cast<u32>(std::max(h, 1))};
        }
        if (ext.width == 0 || ext.height == 0) {
            extent = ext;
            return true; // minimized: no swapchain until restored
        }
        u32 nf = 0;
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.Physical(), surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(nf);
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.Physical(), surface, &nf, formats.data());
        VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                 : formats[0];
        for (const auto& sf : formats) {
            if (sf.format == VK_FORMAT_B8G8R8A8_UNORM && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                fmt = sf;
            }
        }
        u32 nm = 0;
        f.vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.Physical(), surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm);
        f.vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.Physical(), surface, &nm, modes.data());
        // [Display] presentMode (FrameBreak FB-2): the wanted mode if the surface has it, else mailbox, immediate,
        // FIFO (FIFO always exists). FIFO blocks the VideoOut thread at the display's refresh (vertical sync).
        const auto has = [&](VkPresentModeKHR m) { return std::ranges::find(modes, m) != modes.end(); };
        mode = VK_PRESENT_MODE_FIFO_KHR;
        if (has(wanted_mode)) {
            mode = wanted_mode;
        } else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        {
            u32 supported = 0;
            for (const VkPresentModeKHR m : modes) {
                supported |= static_cast<u32>(m) < 4 ? 1u << static_cast<u32>(m) : 0u;
            }
            g_present_info.store(1u << 31 | supported | (static_cast<u32>(mode) & 0xFF) << 8 |
                                     (static_cast<u32>(wanted_mode) & 0xFF) << 16,
                                 std::memory_order_release);
        }
        if (!modes_logged || mode != logged_mode || wanted_mode != logged_wanted) {
            // (at the first swapchain, and whenever the vertical sync setting changes it - not at every resize)
            std::string list;
            for (const VkPresentModeKHR m : modes) {
                list += fmt::format("{}{}", list.empty() ? "" : ", ", PresentModeName(m));
            }
            SSPC_LOG(GPU, Info, "native presenter: present modes {}; wanted {}, using {}{}; {} images min, {} max{}",
                     list, PresentModeName(wanted_mode), PresentModeName(mode),
                     mode == wanted_mode ? "" : " (the surface lacks the wanted one)", caps.minImageCount,
                     caps.maxImageCount, modes_logged ? " (changed while running)" : "");
            modes_logged = true;
            logged_mode = mode;
            logged_wanted = wanted_mode;
        }
        u32 count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount != 0) {
            count = std::min(count, caps.maxImageCount);
        }
        const VkSwapchainKHR old = fg ? VK_NULL_HANDLE : swapchain;
        if (fg && swapchain != VK_NULL_HANDLE) {
            // (the FFX swapchain is replaced, not chained: the old one first, after its presents)
            Fn().vkDeviceWaitIdle(ctx.Device());
            DestroySwapchainObjects();
            fg->DestroySwapchain(swapchain);
            swapchain = VK_NULL_HANDLE;
        }
        // NVIDIA Reflex works on this swapchain (not on frame generation's: its own presents)
        const VkSwapchainLatencyCreateInfoNV latency{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
                                                     .latencyModeEnable = VK_TRUE};
        const bool reflex = !fg && ctx.Caps().low_latency;
        const VkSwapchainCreateInfoKHR sci{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                           .pNext = reflex ? &latency : nullptr,
                                           .surface = surface,
                                           .minImageCount = count,
                                           .imageFormat = fmt.format,
                                           .imageColorSpace = fmt.colorSpace,
                                           .imageExtent = ext,
                                           .imageArrayLayers = 1,
                                           .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .preTransform = caps.currentTransform,
                                           .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                           .presentMode = mode,
                                           .clipped = VK_TRUE,
                                           .oldSwapchain = old};
        VkSwapchainKHR created = VK_NULL_HANDLE;
        if (const VkResult r = fg ? fg->CreateSwapchain(sci, created)
                                  : f.vkCreateSwapchainKHR(ctx.Device(), &sci, nullptr, &created);
            r != VK_SUCCESS) {
            error = fmt::format("vkCreateSwapchainKHR failed ({})", static_cast<int>(r));
            // oldSwapchain may already be retired even when creation fails.
            ctx.Fail("vkCreateSwapchainKHR", r);
            return false;
        }
        DestroySwapchainObjects();
        if (reflex) {
            Reflex::Attach(created); // (the old one is no longer used by it)
        } else {
            Reflex::Detach(old);
        }
        if (old != VK_NULL_HANDLE) {
            f.vkDestroySwapchainKHR(ctx.Device(), old, nullptr);
        }
        swapchain = created;
        swap_format = fmt.format;
        extent = ext;
        u32 ni = 0;
        if (fg) {
            fg->GetImages(swapchain, images);
            ni = static_cast<u32>(images.size());
        } else {
            f.vkGetSwapchainImagesKHR(ctx.Device(), swapchain, &ni, nullptr);
            images.resize(ni);
            f.vkGetSwapchainImagesKHR(ctx.Device(), swapchain, &ni, images.data());
        }
        done.resize(ni);
        for (auto& s : done) {
            const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const VkResult r = f.vkCreateSemaphore(ctx.Device(), &ci, nullptr, &s);
            if (r != VK_SUCCESS) {
                error = fmt::format("swapchain semaphore creation failed ({})", static_cast<int>(r));
                ctx.Fail("create swapchain semaphore", r);
                return false;
            }
        }
        std::scoped_lock lock{stats_mutex};
        stats.window_width = ext.width;
        stats.window_height = ext.height;
        return true;
    }

    void DestroySwapchainObjects() {
        for (auto& s : done) {
            if (s != VK_NULL_HANDLE) {
                Fn().vkDestroySemaphore(ctx.Device(), s, nullptr);
            }
        }
        done.clear();
        images.clear();
    }

    bool Recreate() {
        if (ctx.Failed()) {
            return false;
        }
        // Submission completion alone does not retire presentation-engine references.
        for (auto& s : slots) {
            if (!WaitSlot(s)) {
                return false;
            }
        }
        std::string error;
        if (!CreateSwapchain(error)) {
            std::scoped_lock lock{stats_mutex};
            stats.last_error = error;
            SSPC_LOG(GPU, Error, "native presenter: swapchain: {}", error);
            return false;
        }
        std::scoped_lock lock{stats_mutex};
        ++stats.swapchain_recreated;
        return true;
    }

    /// FSR 3.1 frame generation's context (it owns the swapchain): its own present and acquire queues.
    bool CreateFrameGen(std::string& why) {
        Upscale::FsrFrameGen::Queues q;
        q.game = {ctx.Queue(), ctx.QueueFamily()};
        q.present = {ctx.ExtraQueue(0), ctx.QueueFamily()};
        q.acquire = {ctx.ExtraQueue(1), ctx.QueueFamily()};
        q.compute = {ctx.ComputeQueue(), ctx.ComputeFamily()};
        if (q.present.queue == VK_NULL_HANDLE || q.acquire.queue == VK_NULL_HANDLE) {
            why = "the graphics queue family has no spare queues for its presents";
            return false;
        }
        fg = Upscale::FsrFrameGen::Create(ctx.Physical(), ctx.Device(), q, Fn().vkGetDeviceProcAddr, why);
        return fg != nullptr;
    }

    /// The settings menu turned frame generation on or off: the swapchain is replaced by one of the other kind (the
    /// present thread, between two presents; every present slot retired first).
    void SwitchFrameGen(bool on) {
        if (on == (fg != nullptr) || ctx.Failed()) {
            return;
        }
        for (auto& s : slots) {
            if (!WaitSlot(s)) {
                return;
            }
        }
        Fn().vkDeviceWaitIdle(ctx.Device());
        DestroySwapchainObjects();
        Reflex::Detach(swapchain);
        if (swapchain != VK_NULL_HANDLE) {
            if (fg) {
                fg->DestroySwapchain(swapchain);
            } else {
                Fn().vkDestroySwapchainKHR(ctx.Device(), swapchain, nullptr);
            }
            swapchain = VK_NULL_HANDLE;
        }
        std::string why;
        if (on) {
            if (!CreateFrameGen(why)) {
                SSPC_LOG(GPU, Warning, "native presenter: FSR frame generation unavailable: {}", why);
            }
        } else {
            fg_active.store(false, std::memory_order_release);
            fg.reset();
        }
        for (auto& g : fg_frames) {
            g.valid = false;
        }
        std::string error;
        if (!CreateSwapchain(error)) {
            SSPC_LOG(GPU, Error, "native presenter: swapchain after switching frame generation: {}", error);
        }
        fg_active.store(fg != nullptr, std::memory_order_release);
        Upscale::UpdateStatus([&](Upscale::Status& st) {
            st.fsr_fg_available = !on || fg != nullptr;
            st.fsr_fg_why = why;
        });
        SSPC_LOG(GPU, Info, "native presenter: FSR frame generation {} (swapchain {}x{})",
                 fg ? "on" : (on ? "unavailable: " + why : std::string("off")), extent.width, extent.height);
    }

    void DestroyFrame(Frame& fr) {
        if (fr.image != VK_NULL_HANDLE) {
            Fn().vkDestroyImage(ctx.Device(), fr.image, nullptr);
        }
        if (fr.memory != VK_NULL_HANDLE) {
            Fn().vkFreeMemory(ctx.Device(), fr.memory, nullptr);
        }
        fr.image = VK_NULL_HANDLE;
        fr.memory = VK_NULL_HANDLE;
        fr.width = fr.height = 0;
        fr.format = VK_FORMAT_UNDEFINED;
    }

    bool MakeFrame(Frame& fr, VkFormat format, u32 w, u32 h) {
        if (fr.image != VK_NULL_HANDLE && fr.format == format && fr.width == w && fr.height == h) {
            return true;
        }
        DestroyFrame(fr);
        const auto& f = Fn();
        const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = format,
                                    .extent = {w, h, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (f.vkCreateImage(ctx.Device(), &ici, nullptr, &fr.image) != VK_SUCCESS) {
            fr.image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req{};
        f.vkGetImageMemoryRequirements(ctx.Device(), fr.image, &req);
        const s32 type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = static_cast<u32>(std::max(type, 0))};
        if (type < 0 || f.vkAllocateMemory(ctx.Device(), &mai, nullptr, &fr.memory) != VK_SUCCESS) {
            fr.memory = VK_NULL_HANDLE;
            DestroyFrame(fr);
            return false;
        }
        f.vkBindImageMemory(ctx.Device(), fr.image, fr.memory, 0);
        fr.format = format;
        fr.width = w;
        fr.height = h;
        return true;
    }

    void Capture(const Frame& fr, const std::filesystem::path& png) {
        const auto& f = Fn();
        u32 bpp = 4;
        if (fr.format == VK_FORMAT_R16G16B16A16_SFLOAT || fr.format == VK_FORMAT_R16G16B16A16_UNORM) {
            bpp = 8;
        }
        const u64 bytes = u64{fr.width} * fr.height * bpp;
        OwnedBuffer buf;
        std::string error;
        if (!CreateBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, error)) {
            SSPC_LOG(GPU, Error, "native presenter: capture buffer: {}", error);
            return;
        }
        VkCommandBuffer cb = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        VkResult result = f.vkAllocateCommandBuffers(ctx.Device(), &ai, &cb);
        if (result != VK_SUCCESS) {
            ctx.Fail("allocate capture commands", result);
            DestroyBuffer(ctx, buf);
            return;
        }
        const auto discard = [&] {
            f.vkFreeCommandBuffers(ctx.Device(), pool, 1, &cb);
            DestroyBuffer(ctx, buf);
        };
        const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        result = f.vkBeginCommandBuffer(cb, &bi);
        if (result != VK_SUCCESS) {
            ctx.Fail("begin capture commands", result);
            discard();
            return;
        }
        const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {fr.width, fr.height, 1}};
        Fn().vkCmdCopyImageToBuffer(cb, fr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &region);
        const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
        result = f.vkEndCommandBuffer(cb);
        if (result != VK_SUCCESS) {
            ctx.Fail("end capture commands", result);
            discard();
            return;
        }
        VkFence fence = VK_NULL_HANDLE;
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = f.vkCreateFence(ctx.Device(), &fci, nullptr, &fence);
        if (result != VK_SUCCESS) {
            ctx.Fail("create capture fence", result);
            discard();
            return;
        }
        const VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &cbi};
        VkResult submitted;
        {
            std::scoped_lock lock{ctx.QueueMutex()};
            submitted = ctx.Failed() ? ctx.Failure() : f.vkQueueSubmit2(ctx.Queue(), 1, &si, fence);
        }
        if (submitted != VK_SUCCESS) {
            ctx.Fail("capture vkQueueSubmit2", submitted);
            f.vkDestroyFence(ctx.Device(), fence, nullptr);
            f.vkFreeCommandBuffers(ctx.Device(), pool, 1, &cb);
            DestroyBuffer(ctx, buf);
            return;
        }
        pending_captures.push_back({buf, cb, fence});
        bool pending = true;
        if (!WaitFence(fence, pending, 5'000'000'000ull)) {
            // Buffer and command objects remain owned, and the frame cannot be recycled.
            ctx.Fail("capture wait", VK_TIMEOUT);
            return;
        }
        pending_captures.pop_back();
        f.vkDestroyFence(ctx.Device(), fence, nullptr);
        f.vkFreeCommandBuffers(ctx.Device(), pool, 1, &cb);
        std::vector<u8> rgba(u64{fr.width} * fr.height * 4);
        const u8* src = buf.mapped;
        for (u64 i = 0, n = u64{fr.width} * fr.height; i < n; ++i) {
            u8* d = rgba.data() + i * 4;
            switch (fr.format) {
            case VK_FORMAT_B8G8R8A8_UNORM:
                d[0] = src[i * 4 + 2], d[1] = src[i * 4 + 1], d[2] = src[i * 4 + 0], d[3] = src[i * 4 + 3];
                break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                u32 v;
                std::memcpy(&v, src + i * 4, 4);
                const u32 c0 = v & 0x3FF, c1 = (v >> 10) & 0x3FF, c2 = (v >> 20) & 0x3FF;
                const bool bgr = fr.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32; // R in the high bits
                d[0] = static_cast<u8>((bgr ? c2 : c0) >> 2);
                d[1] = static_cast<u8>(c1 >> 2);
                d[2] = static_cast<u8>((bgr ? c0 : c2) >> 2);
                d[3] = 255;
                break;
            }
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                u16 h[4];
                std::memcpy(h, src + i * 8, 8);
                for (int c = 0; c < 3; ++c) {
                    d[c] = ToByte(HalfToFloat(h[c]));
                }
                d[3] = 255;
                break;
            }
            default: // R8G8B8A8 and the like: as stored
                std::memcpy(d, src + i * bpp, 4);
                break;
            }
            d[3] = 255; // the displayed image is opaque
        }
        DestroyBuffer(ctx, buf);
        const bool ok = WritePng(png, fr.width, fr.height, rgba);
        SSPC_LOG(GPU, Info, "native presenter: frame {}x{} (format {}) captured to {}: {}", fr.width, fr.height,
                 static_cast<int>(fr.format), png.string(), ok ? "written" : "FAILED");
        std::scoped_lock lock{stats_mutex};
        stats.captures += ok ? 1 : 0;
    }

};

Presenter::Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Presenter> Presenter::Create(DeviceContext& ctx, Timeline& timeline, void* sdl_window,
                                             const std::string& present_mode, std::string& error) {
    auto impl = std::make_unique<Impl>(ctx, timeline);
    impl->wanted_mode = ParsePresentMode(present_mode);
    if (!ctx.Caps().swapchain_maintenance1) {
        error = "present fences are required (swapchainMaintenance1)";
        return nullptr;
    }
    impl->window = static_cast<SDL_Window*>(sdl_window);
    if (impl->window == nullptr) {
        error = "no game window";
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(impl->window);
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        error = "the game window has no Win32 handle";
        return nullptr;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        Fn().vkGetInstanceProcAddr(ctx.Instance(), "vkCreateWin32SurfaceKHR"));
    if (create_surface == nullptr) {
        error = "vkCreateWin32SurfaceKHR is not available";
        return nullptr;
    }
    const VkWin32SurfaceCreateInfoKHR sci{.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                          .hinstance = GetModuleHandleW(nullptr),
                                          .hwnd = hwnd};
    if (create_surface(ctx.Instance(), &sci, nullptr, &impl->surface) != VK_SUCCESS) {
        error = "vkCreateWin32SurfaceKHR failed";
        return nullptr;
    }
    VkBool32 supported = VK_FALSE;
    Fn().vkGetPhysicalDeviceSurfaceSupportKHR(ctx.Physical(), ctx.QueueFamily(), impl->surface, &supported);
    if (!supported) {
        error = "the device's queue cannot present to the game window";
        Fn().vkDestroySurfaceKHR(ctx.Instance(), impl->surface, nullptr);
        return nullptr;
    }
    const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = ctx.QueueFamily()};
    VkResult created = Fn().vkCreateCommandPool(ctx.Device(), &pci, nullptr, &impl->pool);
    if (created != VK_SUCCESS) {
        error = fmt::format("present command pool creation failed ({})", static_cast<int>(created));
        Presenter cleanup(std::move(impl));
        return nullptr;
    }
    for (auto& s : impl->slots) {
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = impl->pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        created = Fn().vkAllocateCommandBuffers(ctx.Device(), &ai, &s.cb);
        if (created != VK_SUCCESS) {
            error = fmt::format("present command allocation failed ({})", static_cast<int>(created));
            Presenter cleanup(std::move(impl));
            return nullptr;
        }
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        created = Fn().vkCreateFence(ctx.Device(), &fci, nullptr, &s.fence);
        const VkSemaphoreCreateInfo semi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (created == VK_SUCCESS) {
            created = Fn().vkCreateSemaphore(ctx.Device(), &semi, nullptr, &s.acquired);
        }
        if (created != VK_SUCCESS) {
            error = fmt::format("present slot creation failed ({})", static_cast<int>(created));
            Presenter cleanup(std::move(impl));
            return nullptr;
        }
        const VkFenceCreateInfo pending_fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkResult r = Fn().vkCreateFence(ctx.Device(), &pending_fci, nullptr, &s.acquire_fence);
        if (r == VK_SUCCESS) {
            r = Fn().vkCreateFence(ctx.Device(), &pending_fci, nullptr, &s.present_fence);
        }
        if (r != VK_SUCCESS) {
            error = fmt::format("present/acquire fence creation failed ({})", static_cast<int>(r));
            Presenter cleanup(std::move(impl));
            return nullptr;
        }
    }
    // FSR 3.1 frame generation (chosen at start; it owns the swapchain): its own present and acquire queues
    if (Upscale::GetSettings().frame_gen == Upscale::FrameGen::Fsr) {
        std::string why;
        if (!impl->CreateFrameGen(why)) {
            SSPC_LOG(GPU, Warning, "native presenter: FSR frame generation unavailable: {}", why);
        }
        Upscale::UpdateStatus([&](Upscale::Status& st) {
            st.fsr_fg_available = impl->fg != nullptr;
            st.fsr_fg_why = why;
        });
    } else {
        // (not chosen: it would be available where the DLL and a second queue are)
        Upscale::UpdateStatus([&](Upscale::Status& st) { st.fsr_fg_available = true; });
    }
    Reflex::Init(ctx.Device(), ctx.Caps().low_latency);
    if (!impl->CreateSwapchain(error)) {
        Presenter tmp(std::move(impl)); // destroys what was made
        return nullptr;
    }
    impl->fg_active.store(impl->fg != nullptr, std::memory_order_release);
    SSPC_LOG(GPU, Info, "native presenter: swapchain {}x{} on the game window, format {}, present mode {}",
             impl->extent.width, impl->extent.height, static_cast<int>(impl->swap_format), static_cast<int>(impl->mode));
    return std::unique_ptr<Presenter>(new Presenter(std::move(impl)));
}

Presenter::~Presenter() {
    if (!impl_) {
        return;
    }
    Impl& m = *impl_;
    const auto& f = Fn();
    if (m.ctx.Device() == VK_NULL_HANDLE) {
        return;
    }
    // No normal-frame WaitIdle. At teardown every actually enqueued present has its
    // own completion fence; unsent/failed submissions never create an impossible wait.
    for (auto& s : m.slots) {
        if (!m.WaitSlot(s, 5'000'000'000ull) && m.ctx.Failure() != VK_ERROR_DEVICE_LOST) {
            SSPC_LOG(GPU, Critical, "native presenter: retirement failed at shutdown; terminating without "
                                   "destroying possibly pending Vulkan objects");
            TerminateProcess(GetCurrentProcess(), 5);
        }
    }
    for (auto& capture : m.pending_captures) {
        bool pending = true;
        if (!m.WaitFence(capture.fence, pending, 5'000'000'000ull) &&
            m.ctx.Failure() != VK_ERROR_DEVICE_LOST) {
            SSPC_LOG(GPU, Critical, "native presenter: capture did not retire; terminating safely");
            TerminateProcess(GetCurrentProcess(), 5);
        }
        f.vkDestroyFence(m.ctx.Device(), capture.fence, nullptr);
        f.vkFreeCommandBuffers(m.ctx.Device(), m.pool, 1, &capture.cb);
        DestroyBuffer(m.ctx, capture.buffer);
    }
    for (auto& fr : m.frames) {
        m.DestroyFrame(fr);
    }
    m.DestroySwapchainObjects();
    for (auto& g : m.fg_frames) {
        DestroyOwn(m.ctx, g.depth);
        DestroyOwn(m.ctx, g.motion);
        DestroyOwn(m.ctx, g.hudless);
    }
    DestroyOwn(m.ctx, m.fg_hudless_bb);
    Reflex::Detach(m.swapchain);
    if (m.swapchain != VK_NULL_HANDLE) {
        if (m.fg) {
            f.vkDeviceWaitIdle(m.ctx.Device());
            m.fg->DestroySwapchain(m.swapchain);
        } else {
            f.vkDestroySwapchainKHR(m.ctx.Device(), m.swapchain, nullptr);
        }
    }
    m.fg.reset();
    for (auto& s : m.slots) {
        DestroyBuffer(m.ctx, s.overlay);
        DestroyBuffer(m.ctx, s.probe);
        if (s.acquire_fence != VK_NULL_HANDLE) {
            f.vkDestroyFence(m.ctx.Device(), s.acquire_fence, nullptr);
        }
        if (s.present_fence != VK_NULL_HANDLE) {
            f.vkDestroyFence(m.ctx.Device(), s.present_fence, nullptr);
        }
        if (s.fence != VK_NULL_HANDLE) {
            f.vkDestroyFence(m.ctx.Device(), s.fence, nullptr);
        }
        if (s.acquired != VK_NULL_HANDLE) {
            f.vkDestroySemaphore(m.ctx.Device(), s.acquired, nullptr);
        }
    }
    if (m.pool != VK_NULL_HANDLE) {
        f.vkDestroyCommandPool(m.ctx.Device(), m.pool, nullptr);
    }
    if (m.surface != VK_NULL_HANDLE) {
        f.vkDestroySurfaceKHR(m.ctx.Instance(), m.surface, nullptr);
    }
}

s32 Presenter::AcquireFrame(u32 timeout_ms) {
    Impl& m = *impl_;
    if (m.ctx.Failed()) {
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        {
            std::scoped_lock lock{m.frames_mutex};
            for (u32 i = 0; i < kFrames; ++i) {
                auto& fr = m.frames[i];
                u32 st = fr.state.load(std::memory_order_acquire);
                if (st == 3 && fr.fence != VK_NULL_HANDLE) {
                    const VkResult r = Fn().vkWaitForFences(m.ctx.Device(), 1, &fr.fence, VK_TRUE, 0);
                    if (r == VK_SUCCESS) {
                        fr.fence = VK_NULL_HANDLE;
                        fr.state.store(0, std::memory_order_release);
                        st = 0;
                    } else if (r != VK_TIMEOUT) {
                        m.ctx.Fail("acquire frame fence", r);
                        return -1;
                    }
                }
                if (st == 0) {
                    fr.state.store(1, std::memory_order_release);
                    return static_cast<s32>(i);
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void Presenter::ReleaseFrame(s32 frame) {
    if (frame >= 0 && frame < static_cast<s32>(kFrames)) {
        impl_->frames[frame].state.store(0, std::memory_order_release);
    }
}

bool Presenter::RecordCopy(VkCommandBuffer cb, s32 frame, VkImage src, VkFormat format, u32 width, u32 height) {
    Impl& m = *impl_;
    if (frame < 0 || frame >= static_cast<s32>(kFrames) || src == VK_NULL_HANDLE || width == 0 || height == 0) {
        return false;
    }
    auto& fr = m.frames[frame];
    if (!m.MakeFrame(fr, Unorm(format), width, height)) {
        SSPC_LOG(GPU, Error, "native presenter: a {}x{} frame of format {} could not be created", width, height,
                 static_cast<int>(format));
        return false;
    }
    const auto& f = Fn();
    const VkImageMemoryBarrier2 to_dst{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .srcAccessMask = 0,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = fr.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &to_dst};
    f.vkCmdPipelineBarrier2(cb, &dep);
    const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .extent = {width, height, 1}};
    f.vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &copy);
    const VkImageMemoryBarrier2 to_src{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = fr.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    dep.pImageMemoryBarriers = &to_src;
    f.vkCmdPipelineBarrier2(cb, &dep);
    fr.state.store(2, std::memory_order_release);
    std::scoped_lock lock{m.stats_mutex};
    ++m.stats.copies;
    m.stats.frame_width = width;
    m.stats.frame_height = height;
    return true;
}

void Presenter::Present(s32 frame, u64 flip_id) {
    const StallTrace::Scope trace_present(StallTrace::Ev::Present, flip_id); // (P4.12)
    Impl& m = *impl_;
    const auto& f = Fn();
    Impl::Frame* fr = (frame >= 0 && frame < static_cast<s32>(kFrames)) ? &m.frames[frame] : nullptr;
    const auto release = [&] {
        if (fr != nullptr) {
            fr->state.store(0, std::memory_order_release);
        }
    };
    if (m.ctx.Failed()) {
        return;
    }
    if (const int want = m.fg_wanted.exchange(-1, std::memory_order_acq_rel); want >= 0) {
        m.SwitchFrameGen(want == 1);
    }
    if (const int want = m.mode_request.exchange(-1, std::memory_order_acq_rel);
        want >= 0 && static_cast<VkPresentModeKHR>(want) != m.wanted_mode) {
        // R11 display: the vertical sync changed in the settings menu - a swapchain of the new mode, between two
        // presents, through Recreate (every present slot retired first; oldSwapchain chained, or frame generation's
        // replaced). Minimized (no swapchain): the next one is made with it.
        const VkPresentModeKHR before = m.mode;
        m.wanted_mode = static_cast<VkPresentModeKHR>(want);
        const bool ok = m.swapchain == VK_NULL_HANDLE || m.Recreate();
        SSPC_LOG(GPU, Info, "native presenter: vertical sync setting -> {}: swapchain {} ({} -> {}){}",
                 PresentModeName(m.wanted_mode), m.swapchain == VK_NULL_HANDLE ? "made at the next present"
                                                                               : (ok ? "recreated" : "NOT recreated"),
                 PresentModeName(before), PresentModeName(m.mode), m.fg ? ", frame generation's swapchain" : "");
    }
    if (m.fg) {
        // FrameBreak: FFX's present waits for room in its queue - inside the queue's lock below, where the backend's
        // flush waits too (a deadlock when the game's frames came faster than FFX shows them). Waiting for its earlier
        // presents here, outside the lock, paces the game's frames by what frame generation shows (not by the
        // emulated 59.94 Hz vblank).
        const StallTrace::Scope trace_fg(StallTrace::Ev::PresentFence, flip_id);
        m.fg->WaitForPresents();
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m.window, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(m.window) & SDL_WINDOW_MINIMIZED)) {
        release();
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.minimized_skips;
        return;
    }
    if (m.swapchain == VK_NULL_HANDLE || static_cast<u32>(w) != m.extent.width ||
        static_cast<u32>(h) != m.extent.height) {
        if (!m.Recreate() || m.swapchain == VK_NULL_HANDLE) {
            release();
            return;
        }
    }
    Impl::Slot& s = m.slots[m.slot];
    m.slot = (m.slot + 1) % m.slots.size();
    {
        const StallTrace::Scope trace_fence(StallTrace::Ev::PresentFence);
        if (!m.WaitSlot(s)) {
            release();
            return;
        }
    }
    u32 index = 0;
    VkResult r;
    {
        const StallTrace::Scope trace_acquire(StallTrace::Ev::PresentAcquire);
        r = f.vkResetFences(m.ctx.Device(), 1, &s.acquire_fence);
        if (r != VK_SUCCESS) {
            m.ctx.Fail("reset acquire fence", r);
            return;
        }
        if (m.fg) {
            r = m.fg->Acquire(m.swapchain, 100'000'000ull, s.acquired, VK_NULL_HANDLE, &index);
        } else {
            const auto reflex_lock = Reflex::LockSwapchain(); // (the game's thread uses the swapchain too)
            r = f.vkAcquireNextImageKHR(m.ctx.Device(), m.swapchain, 100'000'000ull, s.acquired, s.acquire_fence,
                                        &index);
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m.Recreate();
        release();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.acquire_failures;
        if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            m.stats.device_lost |= r == VK_ERROR_DEVICE_LOST;
            m.stats.last_error = fmt::format("acquire failed ({})", static_cast<int>(r));
            m.ctx.Fail("vkAcquireNextImageKHR", r);
        }
        release();
        return;
    }
    s.acquire_pending = !m.fg;
    // SSPC_PRESENT_ACQUIRE_FENCE=1 (default off): the image waited for here, on the present thread. The present's
    // submission below shares the one queue with the game's batches: its wait on `acquired` holds that queue until the
    // presentation engine gives the image back (FIFO: up to a refresh) - the batches submitted after it wait too. With
    // the acquire fence met the semaphore is signalled when the submission reaches the queue. A timeout keeps the
    // fence pending (WaitSlot waits for it later) and the semaphore wait still orders the copy: only advisory.
    // (Castlesea's port, vk_swapchain.cpp: city at 75 Hz, frames over 25 ms 17.9 -> 2.7 a minute, p99 21.9 -> 16.8 ms)
    static const bool acquire_fence_wait = [] {
        char v[8] = {};
        const bool on = GetEnvironmentVariableA("SSPC_PRESENT_ACQUIRE_FENCE", v, sizeof(v)) == 1 && v[0] == '1';
        if (on) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_PRESENT_ACQUIRE_FENCE=1 - the swapchain image waited for on the "
                                   "present thread before the present's submission");
        }
        return on;
    }();
    if (acquire_fence_wait && s.acquire_pending) {
        const StallTrace::Scope trace_wait(StallTrace::Ev::PresentAcquire, flip_id);
        const auto w0 = std::chrono::steady_clock::now();
        const bool met = m.WaitFence(s.acquire_fence, s.acquire_pending, 1'000'000'000ull);
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count());
        std::scoped_lock lock{m.stats_mutex};
        if (met) {
            ++m.stats.acquire_fence_waits;
            m.stats.acquire_fence_ns += ns;
            m.stats.acquire_fence_max_ns = std::max(m.stats.acquire_fence_max_ns, ns);
            m.stats.acquire_fence_over5 += ns > 5'000'000 ? 1 : 0;
        }
    }
    r = f.vkResetFences(m.ctx.Device(), 1, &s.fence);
    if (r == VK_SUCCESS) {
        r = f.vkResetFences(m.ctx.Device(), 1, &s.present_fence);
    }
    if (r == VK_SUCCESS) {
        r = f.vkResetCommandBuffer(s.cb, 0);
    }
    if (r != VK_SUCCESS) {
        m.ctx.Fail("reset presentation slot", r);
        return;
    }
    const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    r = f.vkBeginCommandBuffer(s.cb, &bi);
    if (r != VK_SUCCESS) {
        m.ctx.Fail("begin presentation commands", r);
        return;
    }
    const VkImage swap = m.images[index];
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            .srcAccessMask = 0,
                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = swap,
                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    // the frame was written by an earlier submission of this queue (its completion seen on the host): a memory
    // dependency on everything before in submission order makes those writes visible to the blit
    const VkMemoryBarrier2 earlier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                         .pMemoryBarriers = &earlier, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    f.vkCmdPipelineBarrier2(s.cb, &dep);
    dep.memoryBarrierCount = 0;
    dep.pMemoryBarriers = nullptr;
    const VkClearColorValue black{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(s.cb, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    const bool show = fr != nullptr && fr->image != VK_NULL_HANDLE && fr->state.load() == 2;
    if (show) {
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        f.vkCmdPipelineBarrier2(s.cb, &dep); // clear -> blit on the same image
        // the frame's aspect ratio kept, centred
        const double sx = static_cast<double>(m.extent.width) / fr->width;
        const double sy = static_cast<double>(m.extent.height) / fr->height;
        const double sc = std::min(sx, sy);
        const s32 dw = static_cast<s32>(fr->width * sc), dh = static_cast<s32>(fr->height * sc);
        const s32 dx = (static_cast<s32>(m.extent.width) - dw) / 2, dy = (static_cast<s32>(m.extent.height) - dh) / 2;
        const VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .srcOffsets = {{0, 0, 0}, {static_cast<s32>(fr->width), static_cast<s32>(fr->height), 1}},
                               .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        f.vkCmdBlitImage(s.cb, fr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    // R10 latency probe (SSPC_R10_LATENCY=1, r10/latency.h): a 16 x 9 grid of the frame's texels into the slot's host
    // buffer (the frame stays in TRANSFER_SRC as for the blit: a read after a read, no barrier before)
    s.probe_bpp = 0;
    if (R10::Latency::On() && show) {
        const u32 bpp = fr->format == VK_FORMAT_R16G16B16A16_SFLOAT || fr->format == VK_FORMAT_R16G16B16A16_UNORM ? 8u
                        : (fr->format == VK_FORMAT_B8G8R8A8_UNORM || fr->format == VK_FORMAT_B8G8R8A8_SRGB ||
                           fr->format == VK_FORMAT_R8G8B8A8_UNORM || fr->format == VK_FORMAT_R8G8B8A8_SRGB ||
                           fr->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                           fr->format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
                            ? 4u
                            : 0u;
        if (bpp != 0 && s.probe.buffer == VK_NULL_HANDLE) {
            std::string err;
            if (!CreateBuffer(m.ctx, u64{R10::Latency::kGrid} * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, s.probe, err,
                              true)) {
                SSPC_LOG(GPU, Warning, "R10 latency probe: no readback buffer ({})", err);
            }
        }
        if (bpp != 0 && s.probe.mapped != nullptr && fr->width >= R10::Latency::kGridW &&
            fr->height >= R10::Latency::kGridH) {
            std::array<VkBufferImageCopy, R10::Latency::kGrid> regions{};
            for (u32 y = 0; y < R10::Latency::kGridH; ++y) {
                for (u32 x = 0; x < R10::Latency::kGridW; ++x) {
                    const u32 i = y * R10::Latency::kGridW + x;
                    regions[i] = {.bufferOffset = u64{i} * bpp,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageOffset = {static_cast<s32>(fr->width * (2 * x + 1) / (2 * R10::Latency::kGridW)),
                                                  static_cast<s32>(fr->height * (2 * y + 1) / (2 * R10::Latency::kGridH)),
                                                  0},
                                  .imageExtent = {1, 1, 1}};
                }
            }
            f.vkCmdCopyImageToBuffer(s.cb, fr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.probe.buffer,
                                     static_cast<u32>(regions.size()), regions.data());
            const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
            const VkDependencyInfo hdep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                        .pMemoryBarriers = &to_host};
            f.vkCmdPipelineBarrier2(s.cb, &hdep);
            s.probe_bpp = bpp;
        }
    }
    // the FPS counter over the frame, top left (P4.6): the shown frames of the last second, the text renewed twice a
    // second; SSPC_FPS_OVERLAY=0 hides it
    static const bool overlay_env = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("SSPC_FPS_OVERLAY", v, sizeof(v)) == 1 && v[0] == '0');
    }();
    const bool byte_format = m.swap_format == VK_FORMAT_B8G8R8A8_UNORM || m.swap_format == VK_FORMAT_B8G8R8A8_SRGB ||
                             m.swap_format == VK_FORMAT_R8G8B8A8_UNORM || m.swap_format == VK_FORMAT_R8G8B8A8_SRGB;
    if (overlay_env && g_fps_counter_shown.load(std::memory_order_relaxed) && m.overlay_on && byte_format &&
        m.extent.width >= kOverlayW + 8 &&
        m.extent.height >= kOverlayH + 8) {
        const auto now = std::chrono::steady_clock::now();
        if (show) {
            m.shown.push_back(now);
        }
        while (!m.shown.empty() && now - m.shown.front() > std::chrono::seconds(1)) {
            m.shown.pop_front();
        }
        if (m.overlay_text.empty() || now - m.overlay_t >= std::chrono::milliseconds(500)) {
            m.overlay_t = now;
            double fps = 0.0;
            if (m.shown.size() >= 2) {
                const double span = std::chrono::duration<double>(m.shown.back() - m.shown.front()).count();
                fps = span > 0.0 ? static_cast<double>(m.shown.size() - 1) / span : 0.0;
            }
            m.overlay_text = fps > 0.0 ? fmt::format("{:.1f} FPS  {:.1f} MS", fps, 1000.0 / fps) : "0.0 FPS";
        }
        if (s.overlay.buffer == VK_NULL_HANDLE) {
            std::string err;
            if (!CreateBuffer(m.ctx, u64{kOverlayW} * kOverlayH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, s.overlay,
                              err)) {
                m.overlay_on = false;
                SSPC_LOG(GPU, Warning, "native backend: no FPS counter ({})", err);
            }
        }
        if (s.overlay.mapped != nullptr) {
            DrawOverlay(s.overlay.mapped, m.overlay_text); // the slot's last use completed (its fence, above)
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            f.vkCmdPipelineBarrier2(s.cb, &dep); // clear/blit -> the counter on the same image
            const VkBufferImageCopy region{.bufferOffset = 0,
                                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                           .imageOffset = {8, 8, 0},
                                           .imageExtent = {kOverlayW, kOverlayH, 1}};
            f.vkCmdCopyBufferToImage(s.cb, s.overlay.buffer, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
    }
    if (m.fg) {
        // frame generation: this frame's inputs (none: a frame of its own - menus, movies, loading, a reset), the
        // frame without the HUD scaled to the backbuffer like the frame itself
        const auto now = std::chrono::steady_clock::now();
        const float frame_ms = m.fg_last_present.time_since_epoch().count() != 0
                                   ? std::chrono::duration<float, std::milli>(now - m.fg_last_present).count()
                                   : 33.3f;
        m.fg_last_present = now;
        Upscale::FsrFrameGen::Inputs* in = nullptr;
        if (show && frame >= 0 && m.fg_frames[frame].valid) {
            Impl::FgFrame& g = m.fg_frames[frame];
            in = &g.in;
            // (TRANSFER_SRC: AMD's frame generation copies out of it - VUID-vkCmdCopyImage-aspect-06662)
            if (MakeOwn(m.ctx, m.fg_hudless_bb, m.swap_format, m.extent.width, m.extent.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                OwnBarrier(s.cb, m.fg_hudless_bb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkClearColorValue clear{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
                const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                f.vkCmdClearColorImage(s.cb, m.fg_hudless_bb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
                OwnBarrier(s.cb, m.fg_hudless_bb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const double sx = static_cast<double>(m.extent.width) / g.hudless.width;
                const double sy = static_cast<double>(m.extent.height) / g.hudless.height;
                const double sc = std::min(sx, sy);
                const s32 dw = static_cast<s32>(g.hudless.width * sc), dh = static_cast<s32>(g.hudless.height * sc);
                const s32 dx = (static_cast<s32>(m.extent.width) - dw) / 2;
                const s32 dy = (static_cast<s32>(m.extent.height) - dh) / 2;
                const VkImageBlit hb{
                    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .srcOffsets = {{0, 0, 0}, {static_cast<s32>(g.hudless.width), static_cast<s32>(g.hudless.height), 1}},
                    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
                f.vkCmdBlitImage(s.cb, g.hudless.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.fg_hudless_bb.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hb, VK_FILTER_LINEAR);
                OwnBarrier(s.cb, m.fg_hudless_bb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                in->hudless = m.fg_hudless_bb.image;
                in->hudless_format = m.swap_format;
            } else {
                in->hudless = VK_NULL_HANDLE;
            }
        }
        m.fg->SetFrame(s.cb, in, m.extent, frame_ms);
    }
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    f.vkCmdPipelineBarrier2(s.cb, &dep);
    r = f.vkEndCommandBuffer(s.cb);
    if (r != VK_SUCCESS) {
        m.ctx.Fail("end presentation commands", r);
        return;
    }

    const VkSemaphoreSubmitInfo waits[1] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = s.acquired,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
    };
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m.done[index],
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s.cb};
    const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                           .waitSemaphoreInfoCount = 1,
                           .pWaitSemaphoreInfos = waits,
                           .commandBufferInfoCount = 1,
                           .pCommandBufferInfos = &cbi,
                           .signalSemaphoreInfoCount = 1,
                           .pSignalSemaphoreInfos = &signal};
    VkResult pr = VK_SUCCESS;
    {
        const u64 lock_t0 = StallTrace::On() ? StallTrace::Now() : 0;
        std::unique_lock lock{m.ctx.QueueMutex(), std::try_to_lock};
        if (!lock.owns_lock()) {
            lock.lock(); // (P4.12: a batch's submission holds the queue)
            if (lock_t0 != 0) {
                StallTrace::Span(StallTrace::Ev::QueueLock, lock_t0, StallTrace::Now(), flip_id);
            }
        }
        const StallTrace::Scope trace_submit(StallTrace::Ev::PresentSubmit, flip_id);
        r = m.ctx.Failed() ? m.ctx.Failure() : f.vkQueueSubmit2(m.ctx.Queue(), 1, &si, s.fence);
        if (r == VK_SUCCESS) {
            s.submitted = true;
            // NVIDIA Reflex: the present carries the frame's id (its flip id), as its markers do
            const bool reflex = !m.fg && Reflex::Attached();
            const VkPresentIdKHR present_id{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
                                            .swapchainCount = 1,
                                            .pPresentIds = &flip_id};
            const VkSwapchainPresentFenceInfoEXT pf{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                .pNext = reflex ? &present_id : nullptr,
                .swapchainCount = 1, .pFences = &s.present_fence};
            const VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                      .pNext = &pf,
                                      .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &m.done[index],
                                      .swapchainCount = 1,
                                      .pSwapchains = &m.swapchain,
                                      .pImageIndices = &index};
            if (m.fg) {
                VkPresentInfoKHR fpi = pi;
                fpi.pNext = nullptr;
                pr = m.fg->Present(m.ctx.Queue(), fpi);
            } else {
                const auto reflex_lock = Reflex::LockSwapchain(); // (after the queue's mutex, as the executor)
                if (reflex) {
                    Reflex::PresentMarker(true, flip_id);
                }
                pr = f.vkQueuePresentKHR(m.ctx.Queue(), &pi);
                if (reflex) {
                    Reflex::PresentMarker(false, flip_id);
                }
            }
            StallTrace::Instant(StallTrace::Ev::PresentDone, flip_id); // (R10 timeline)
            if (R10::Latency::On()) {
                s.probe_flip = show ? flip_id : 0;
                s.probe_tsc = __rdtsc();
                s.probe_pending = true;
            }
            // WSI errors below still enqueue semaphore waits and the present fence.
            s.present_pending = !m.fg && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR ||
                                pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR ||
                                pr == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
        }
    }
    if (r != VK_SUCCESS) {
        std::scoped_lock lock{m.stats_mutex};
        m.stats.last_error = fmt::format("present submission failed ({})", static_cast<int>(r));
        m.stats.device_lost |= r == VK_ERROR_DEVICE_LOST;
        m.ctx.Fail("presentation vkQueueSubmit2", r);
        release();
        return;
    }
    // Keep the source frame owned until a requested capture has finished too.
    {
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.presented;
        m.stats.black += show ? 0 : 1;
    }
    if (m.fg) {
        // the game's frames presented, and what reached the display for them (generated frames included)
        ++m.fg_real;
        m.fg_shown += m.fg->TakeShown();
        const auto now = std::chrono::steady_clock::now();
        if (m.fg_window_t.time_since_epoch().count() == 0) {
            m.fg_window_t = now;
        }
        const float sec = std::chrono::duration<float>(now - m.fg_window_t).count();
        if (sec >= 5.0f) {
            m.fg_real_fps = static_cast<float>(m.fg_real) / sec;
            m.fg_shown_fps = static_cast<float>(m.fg_shown) / sec;
            SSPC_LOG(GPU, Info, "[FSR-FG] game frames {:.1f}/s, displayed {:.1f}/s (generated {:.1f}/s){}",
                     m.fg_real_fps, m.fg_shown_fps, m.fg_shown_fps - m.fg_real_fps,
                     m.fg->Generating() ? "" : " - generation off");
            m.fg_real = m.fg_shown = 0;
            m.fg_window_t = now;
        }
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        m.Recreate();
    } else if (pr != VK_SUCCESS) {
        std::scoped_lock lock{m.stats_mutex};
        m.stats.device_lost |= pr == VK_ERROR_DEVICE_LOST;
        m.stats.last_error = fmt::format("vkQueuePresentKHR failed ({})", static_cast<int>(pr));
        m.ctx.Fail("vkQueuePresentKHR", pr);
    }
    std::filesystem::path capture;
    {
        std::scoped_lock lock{m.capture_mutex};
        // the first shown flip at or after each request (a request whose flip was not shown takes the next one)
        while (show && !m.captures.empty() && m.captures.front().first <= flip_id) {
            capture = std::move(m.captures.front().second);
            m.captures.pop_front();
        }
    }
    if (!capture.empty() && fr != nullptr && !m.ctx.Failed()) {
        m.Capture(*fr, capture);
    }
    if (fr != nullptr && !m.ctx.Failed()) {
        std::scoped_lock lock{m.frames_mutex};
        fr->fence = s.fence;
        fr->state.store(show ? 3 : 0, std::memory_order_release);
    }
}

bool Presenter::FrameGenActive() const {
    return impl_->fg_active.load(std::memory_order_acquire);
}

void Presenter::SetFrameGen(bool on) {
    impl_->fg_wanted.store(on ? 1 : 0, std::memory_order_release);
}

void Presenter::SetPresentMode(const std::string& present_mode) {
    impl_->mode_request.store(static_cast<int>(ParsePresentMode(present_mode)), std::memory_order_release);
}

bool Presenter::RecordFrameGenInputs(VkCommandBuffer cb, s32 frame, const FrameGenSource& src) {
    Impl& m = *impl_;
    if (!m.fg_active.load(std::memory_order_acquire) || frame < 0 || frame >= static_cast<s32>(kFrames)) {
        return false;
    }
    Impl::FgFrame& g = m.fg_frames[frame];
    g.valid = false;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!MakeOwn(m.ctx, g.depth, VK_FORMAT_R32_SFLOAT, src.depth_w, src.depth_h, usage) ||
        !MakeOwn(m.ctx, g.motion, VK_FORMAT_R16G16_SFLOAT, src.motion_w, src.motion_h, usage) ||
        !MakeOwn(m.ctx, g.hudless, src.hudless_format, src.hudless_w, src.hudless_h,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    const auto& f = Fn();
    const auto src_barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to) {
        const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      .oldLayout = from,
                                      .newLayout = to,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = image,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                                   .pImageMemoryBarriers = &b};
        f.vkCmdPipelineBarrier2(cb, &dep);
    };
    src_barrier(src.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    src_barrier(src.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    OwnBarrier(cb, g.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    OwnBarrier(cb, g.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    OwnBarrier(cb, g.hudless, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const auto copy = [&](VkImage from, OwnImage& to) {
        const VkImageCopy c{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .extent = {to.width, to.height, 1}};
        f.vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &c);
    };
    copy(src.depth, g.depth);
    copy(src.motion, g.motion);
    copy(src.hudless, g.hudless);
    src_barrier(src.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    src_barrier(src.motion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    OwnBarrier(cb, g.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_SHADER_READ_BIT);
    OwnBarrier(cb, g.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_SHADER_READ_BIT);
    OwnBarrier(cb, g.hudless, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
    g.in = {};
    g.in.depth = g.depth.image;
    g.in.motion = g.motion.image;
    g.in.depth_w = g.depth.width;
    g.in.depth_h = g.depth.height;
    g.in.motion_w = g.motion.width;
    g.in.motion_h = g.motion.height;
    g.in.render_w = src.render_w;
    g.in.render_h = src.render_h;
    g.in.jitter_x = src.jitter_x;
    g.in.jitter_y = src.jitter_y;
    g.in.mv_scale_x = src.mv_scale_x;
    g.in.mv_scale_y = src.mv_scale_y;
    g.in.camera_near = src.camera_near;
    g.in.camera_far = src.camera_far;
    g.in.fov_y = src.fov_y;
    g.in.depth_inverted = src.depth_inverted;
    g.in.reset = src.reset;
    g.valid = true;
    return true;
}

void Presenter::RequestCapture(u64 flip_id, const std::filesystem::path& png) {
    std::scoped_lock lock{impl_->capture_mutex};
    impl_->captures.emplace_back(flip_id, png);
}

bool Presenter::TestRecreate() { return impl_->Recreate(); }

Presenter::Stats Presenter::GetStats() {
    std::scoped_lock lock{impl_->stats_mutex};
    return impl_->stats;
}

} // namespace SecondSon::Gpu::Vk
