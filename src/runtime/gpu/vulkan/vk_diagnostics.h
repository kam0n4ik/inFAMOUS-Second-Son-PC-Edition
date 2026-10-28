// SecondSonPC — diagnostics of the native GPU backend (P4.4, --gpu-native-diag; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md
// §K). What a device loss or a validation message needs to be traced to one command:
//  - every recorded GPU command (draw, dispatch, upload, frame copy, timeline write) gets an id and a line in a journal
//    (a ring of the last 65536), with an NVIDIA checkpoint (VK_NV_device_diagnostic_checkpoints) and buffer markers
//    (VK_AMD_buffer_marker: "started" at the top of the pipe, "completed" at the bottom) around it;
//  - the validation layer's messages go to logs\vk_validation.log with their VUID, the objects (with the names given
//    here) and the submission being recorded or submitted;
//  - at the first VK_ERROR_DEVICE_LOST one report, logs\gpu_device_lost.txt: the markers, the checkpoints, the device
//    fault (VK_EXT_device_fault), the journal around the last started/completed command, the batches in flight;
//  - the image cache's aliasing decisions (a texture over another image) to logs\gpu_alias.log, once per pair/view.
// Off (no checkpoints, no markers, no names, no journal) unless enabled; the device loss report is written either way.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk {

class DeviceContext;

/// The submission being recorded or submitted (the batch's timeline value): validation messages name it.
extern std::atomic<u64> g_diag_submission;

/// Opens logs\vk_validation.log for the validation callback (before the instance is created).
void OpenValidationLog(const std::filesystem::path& file);
/// The validation callback's output (thread-safe; called from the layer).
void WriteValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, const VkDebugUtilsMessengerCallbackDataEXT* data);

class Diagnostics {
public:
    Diagnostics(DeviceContext& ctx, bool enabled, const std::filesystem::path& logs_dir);
    ~Diagnostics();

    bool Enabled() const { return enabled_; }
    /// Objects are named: --gpu-native-diag or SSPC_GPU_MARKERS=1 (a profiler's capture reads the names, P4.6-G6).
    bool Naming() const { return naming_; }

    /// A command recorded into `cb` for submission `batch`: its id (0 when off), with a checkpoint and a "started" marker.
    u64 Begin(VkCommandBuffer cb, u64 batch, std::string text, u8 lane = 0);
    /// The command `id` recorded: a "completed" marker.
    void End(VkCommandBuffer cb, u64 id, u8 lane = 0);
    /// A batch submitted (its value, the commands in it).
    void Submitted(u64 batch, u64 first_cmd, u64 last_cmd, u8 lane = 0);

    /// Names a Vulkan object for the validation messages (nothing when off).
    void Name(VkObjectType type, u64 handle, const std::string& name);

    /// The image cache's aliasing decisions (once per key; nothing when off).
    void Alias(u64 key, const std::string& line);

    /// The diagnostics of the running backend when on (--gpu-native-diag), else nullptr.
    static Diagnostics* Active();
    /// The presenter's submission of flip `flip` (its thread, the game's queue): "started" (top of pipe) and "completed"
    /// (bottom of pipe) markers of its own - whether the queue is held by a present (its wait on the swapchain image)
    void PresentBegin(VkCommandBuffer cb, u64 flip);
    void PresentEnd(VkCommandBuffer cb, u64 flip);
    /// The presenter's steps on the host: 0 the swapchain image acquired, 1 submitted, 2 handed to vkQueuePresentKHR
    /// (its result)
    void PresentStep(u32 step, u64 flip, s32 result = 0);

    /// The first device loss: one report (the later calls do nothing). Returns the report's path.
    std::filesystem::path DeviceLost(const std::string& where, u64 failing_batch, const std::string& in_flight);
    bool Lost() const { return lost_.load(std::memory_order_acquire); }

private:
    struct Record {
        u64 id = 0, batch = 0;
        std::string text;
    };
    std::string RecordLines(u64 from, u64 to);

    DeviceContext& ctx_;
    bool enabled_ = false;
    bool naming_ = false;
    bool serialize_ = false; ///< SSPC_DIAG_SERIALIZE=1 (diagnostics): a full barrier before every recorded command
    std::filesystem::path dir_;
    std::mutex mutex_;
    std::vector<Record> ring_; ///< by id % size
    u64 next_id_ = 1;
    std::vector<std::pair<u64, std::pair<u64, u64>>> batches_; ///< last batches: value -> [first, last] command
    std::vector<u8> batch_lanes_; ///< FrameBreak AQ: batches_'s queues (0 main, 1 the async compute lane)
    VkBuffer marker_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory marker_memory_ = VK_NULL_HANDLE;
    volatile u32* markers_ = nullptr; ///< [0] started (low 32 bits of the id), [1] completed; [2], [3] the AQ lane's
    std::atomic<bool> lost_{false};
    std::atomic<u64> present_flip_[3]{}; ///< PresentStep's flips by step
    std::atomic<s64> present_ns_[3]{};   ///< and when (steady clock)
    std::atomic<s32> present_result_{0}; ///< the last vkQueuePresentKHR's result
    std::mutex alias_mutex_;
    std::vector<u64> alias_keys_;
    FILE* alias_log_ = nullptr;
};

} // namespace SecondSon::Gpu::Vk
