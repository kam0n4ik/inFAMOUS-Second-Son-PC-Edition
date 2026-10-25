// SecondSonPC — the backend's presenter (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §F): its own
// swapchain on the game's SDL window, frames of its own, the VideoOut sink.
//
// Ownership: frames (images) belong to the presenter. On the GPU thread, at the flip marker, the executor records the
// copy of the displayed buffer's image (its render target) into a free frame, in stream order after the frame's work;
// the present thread later shows that frame when the GPU work completed. The present thread touches nothing of the
// executor's (images, layouts, command buffers): the copy is the hand-over. Window resize, minimize and out-of-date
// swapchains recreate the swapchain; a lost device is reported, never treated as success.
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

namespace SecondSon::Gpu::Vk {

class DeviceContext;
class Timeline;

/// The FPS counter over the presented picture shown or hidden ([Display] showFps, F10, the settings window; any
/// thread, read by the present thread at its next present). SSPC_FPS_OVERLAY=0 still hides it.
void SetFpsCounterShown(bool shown);

/// R11 display: the present modes of the presenter's surface and the one its swapchain uses (any thread; the settings
/// menu). Bits: 1 << VkPresentModeKHR for IMMEDIATE (0) .. FIFO_RELAXED (3); `known` once a swapchain was made.
struct PresentModeInfo {
    bool known = false;
    u32 supported = 0;
    VkPresentModeKHR current = VK_PRESENT_MODE_FIFO_KHR; ///< the swapchain's
    VkPresentModeKHR wanted = VK_PRESENT_MODE_FIFO_KHR;  ///< [Display] presentMode as asked
};
PresentModeInfo GetPresentModeInfo();
/// "IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"
const char* PresentModeName(VkPresentModeKHR mode);

class Presenter {
public:
    /// `sdl_window`: the game's SDL_Window; `present_mode` the wanted one ("Mailbox" | "Fifo" | "Immediate", [Display]
    /// presentMode: the next supported one if the surface lacks it). nullptr and `error` on failure.
    static std::unique_ptr<Presenter> Create(DeviceContext& ctx, Timeline& timeline, void* sdl_window,
                                             const std::string& present_mode, std::string& error);
    ~Presenter(); ///< waits for its submissions, destroys frames and the swapchain

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    static constexpr u32 kFrames = 3;

    /// GPU thread: a free frame for the next flip (waits up to `timeout_ms` for one being presented); -1 if none.
    s32 AcquireFrame(u32 timeout_ms);
    /// GPU thread: records into `cb` the copy of `src` (currently in TRANSFER_SRC_OPTIMAL, `width` x `height`, format
    /// `format`) into `frame` (recreated for the size/format when needed; done before recording). false: the frame
    /// could not be (re)created — the flip shows blank.
    bool RecordCopy(VkCommandBuffer cb, s32 frame, VkImage src, VkFormat format, u32 width, u32 height);
    /// GPU thread: the flip will not use `frame` (no buffer to copy).
    void ReleaseFrame(s32 frame);

    /// Frame generation (FSR 3.1; docs/UPSCALING_FRAME_GENERATION.md): the inputs of the game frame copied into `frame`
    /// - its depth and motion (render size; in SHADER_READ_ONLY_OPTIMAL, left so) and the frame without the HUD (in
    /// TRANSFER_SRC_OPTIMAL, left so), copied into the frame's own images by commands recorded into `cb`.
    struct FrameGenSource {
        VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE, hudless = VK_NULL_HANDLE;
        VkFormat hudless_format = VK_FORMAT_UNDEFINED;
        u32 depth_w = 0, depth_h = 0, motion_w = 0, motion_h = 0, hudless_w = 0, hudless_h = 0;
        u32 render_w = 0, render_h = 0;
        float jitter_x = 0, jitter_y = 0, mv_scale_x = 1, mv_scale_y = 1;
        float camera_near = 0.1f, camera_far = 10000.0f, fov_y = 1.0f;
        bool depth_inverted = true, reset = false;
    };
    /// GPU thread, after RecordCopy for the same frame. false: frame generation is not on.
    bool RecordFrameGenInputs(VkCommandBuffer cb, s32 frame, const FrameGenSource& src);
    /// Frame generation runs (the FSR swapchain exists): the executor gathers its inputs.
    bool FrameGenActive() const;
    /// FSR frame generation on or off while the game runs (the settings menu): the present thread replaces the
    /// swapchain before its next present.
    void SetFrameGen(bool on);
    /// R11 display: [Display] presentMode while the game runs ("Fifo" | "Immediate" | "Mailbox"; the vertical sync of
    /// the settings menu). The present thread recreates the swapchain before its next present (the same retirement
    /// path as a resize; frame generation's swapchain is replaced by its own). Any thread.
    void SetPresentMode(const std::string& present_mode);

    /// Present thread: shows `frame` (or black for -1). The frame is free again once the presentation's commands
    /// completed.
    void Present(s32 frame, u64 flip_id);

    /// Captures the frame shown for flip `flip_id` to `png` (written by the present thread right after the present).
    void RequestCapture(u64 flip_id, const std::filesystem::path& png);

    struct Stats {
        u64 presented = 0, black = 0, swapchain_recreated = 0, minimized_skips = 0, acquire_failures = 0;
        u64 captures = 0, copies = 0;
        /// SSPC_PRESENT_ACQUIRE_FENCE=1: the swapchain image waited for on the present thread (its acquire fence) before
        /// the present's submission - the waits, their time (ns), the longest, those over 5 ms
        u64 acquire_fence_waits = 0, acquire_fence_ns = 0, acquire_fence_max_ns = 0, acquire_fence_over5 = 0;
        u32 window_width = 0, window_height = 0, frame_width = 0, frame_height = 0;
        bool device_lost = false;
        std::string last_error;
    };
    Stats GetStats();
    /// Diagnostic test seam: uses the same retirement path as resize/out-of-date.
    bool TestRecreate();

    struct Impl;

private:
    explicit Presenter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace SecondSon::Gpu::Vk
