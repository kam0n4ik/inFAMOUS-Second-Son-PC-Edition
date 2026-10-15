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

class Presenter {
public:
    /// `sdl_window`: the game's SDL_Window. nullptr and `error` on failure.
    static std::unique_ptr<Presenter> Create(DeviceContext& ctx, Timeline& timeline, void* sdl_window,
                                             std::string& error);
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

    /// Present thread: shows `frame` (or black for -1). The frame is free again once the presentation's commands
    /// completed.
    void Present(s32 frame, u64 flip_id);

    /// Captures the frame shown for flip `flip_id` to `png` (written by the present thread right after the present).
    void RequestCapture(u64 flip_id, const std::filesystem::path& png);

    struct Stats {
        u64 presented = 0, black = 0, swapchain_recreated = 0, minimized_skips = 0, acquire_failures = 0;
        u64 captures = 0, copies = 0;
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
