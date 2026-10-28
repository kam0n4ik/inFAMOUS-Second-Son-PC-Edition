// SecondSonPC — diagnostics of the native GPU backend (P4.4). See vk_diagnostics.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/vulkan/vk_device_context.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

std::atomic<u64> g_diag_submission{0};

namespace {

constexpr u64 kRing = 65536;
std::atomic<Diagnostics*> g_active{nullptr};

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::mutex g_vlog_mutex;
FILE* g_vlog = nullptr;
u64 g_vlog_lines = 0;

std::string Now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return fmt::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

FILE* OpenText(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = nullptr;
    return _wfopen_s(&f, file.c_str(), L"w") == 0 ? f : nullptr;
}

std::string_view StageName(VkPipelineStageFlags stage) {
    switch (stage) {
    case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
        return "top of pipe";
    case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
        return "bottom of pipe";
    case VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT:
        return "draw indirect";
    case VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:
        return "vertex shader";
    case VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:
        return "fragment shader";
    case VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
        return "compute shader";
    case VK_PIPELINE_STAGE_TRANSFER_BIT:
        return "transfer";
    case VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:
        return "colour attachment output";
    default:
        return "other";
    }
}

std::string_view FaultAddressType(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        return "invalid read";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        return "invalid write";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        return "invalid execute";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "instruction pointer (fault)";
    default:
        return "?";
    }
}

} // namespace

void OpenValidationLog(const std::filesystem::path& file) {
    std::scoped_lock lock{g_vlog_mutex};
    if (g_vlog == nullptr) {
        g_vlog = OpenText(file);
    }
}

void WriteValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                            const VkDebugUtilsMessengerCallbackDataEXT* data) {
    std::scoped_lock lock{g_vlog_mutex};
    if (g_vlog == nullptr || data == nullptr || g_vlog_lines > 200000) {
        return;
    }
    ++g_vlog_lines;
    const char* sev = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARNING"
                                                                                     : "INFO";
    std::string objects;
    for (u32 i = 0; i < data->objectCount; ++i) {
        const auto& o = data->pObjects[i];
        objects += fmt::format(" [{} {:#x} \"{}\"]", static_cast<int>(o.objectType), o.objectHandle,
                               o.pObjectName != nullptr ? o.pObjectName : "");
    }
    fmt::print(g_vlog, "[{}] {} submission {} {} (id {:#x})\n  objects:{}\n  {}\n", Now(), sev,
               g_diag_submission.load(std::memory_order_relaxed),
               data->pMessageIdName != nullptr ? data->pMessageIdName : "-",
               static_cast<u32>(data->messageIdNumber), objects.empty() ? " -" : objects,
               data->pMessage != nullptr ? data->pMessage : "");
    std::fflush(g_vlog);
}

Diagnostics::Diagnostics(DeviceContext& ctx, bool enabled, const std::filesystem::path& logs_dir)
    : ctx_(ctx), enabled_(enabled), naming_(enabled || ctx.Caps().markers), dir_(logs_dir) {
    if (!enabled_) {
        return;
    }
    ring_.resize(kRing);
    alias_log_ = OpenText(dir_ / "gpu_alias.log");
    const auto& f = Fn();
    if (f.vkCmdWriteBufferMarkerAMD != nullptr) {
        const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = 256,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (f.vkCreateBuffer(ctx_.Device(), &bci, nullptr, &marker_buffer_) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            f.vkGetBufferMemoryRequirements(ctx_.Device(), marker_buffer_, &req);
            const s32 type = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .allocationSize = req.size,
                                           .memoryTypeIndex = static_cast<u32>(std::max(type, 0))};
            void* p = nullptr;
            if (type >= 0 && f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &marker_memory_) == VK_SUCCESS &&
                f.vkBindBufferMemory(ctx_.Device(), marker_buffer_, marker_memory_, 0) == VK_SUCCESS &&
                f.vkMapMemory(ctx_.Device(), marker_memory_, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                markers_ = static_cast<volatile u32*>(p);
                markers_[0] = 0;
                markers_[1] = 0;
                markers_[2] = 0;
                markers_[3] = 0;
                markers_[4] = 0; // [4], [5]: the presenter's (PresentBegin / PresentEnd)
                markers_[5] = 0;
            } else {
                SSPC_LOG(GPU, Warning, "native backend diagnostics: no buffer markers (host-visible memory)");
            }
        }
    }
    g_active.store(this, std::memory_order_release);
    SSPC_LOG(GPU, Info,
             "native backend diagnostics ON: command journal, checkpoints {}, buffer markers {}, device fault {}, "
             "object names {}",
             f.vkCmdSetCheckpointNV != nullptr ? "yes" : "no", markers_ != nullptr ? "yes" : "no",
             f.vkGetDeviceFaultInfoEXT != nullptr ? "yes" : "no", f.vkSetDebugUtilsObjectNameEXT != nullptr ? "yes" : "no");
}

Diagnostics::~Diagnostics() {
    Diagnostics* self = this;
    g_active.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    const auto& f = Fn();
    if (marker_memory_ != VK_NULL_HANDLE) {
        f.vkFreeMemory(ctx_.Device(), marker_memory_, nullptr);
    }
    if (marker_buffer_ != VK_NULL_HANDLE) {
        f.vkDestroyBuffer(ctx_.Device(), marker_buffer_, nullptr);
    }
    if (alias_log_ != nullptr) {
        std::fclose(alias_log_);
    }
}

u64 Diagnostics::Begin(VkCommandBuffer cb, u64 batch, std::string text, u8 lane) {
    if (!enabled_) {
        return 0;
    }
    const auto& f = Fn();
    u64 id;
    {
        std::scoped_lock lock{mutex_};
        id = next_id_++;
        ring_[id % kRing] = Record{id, batch, std::move(text)};
    }
    if (f.vkCmdSetCheckpointNV != nullptr) {
        f.vkCmdSetCheckpointNV(cb, reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
    }
    if (markers_ != nullptr) {
        f.vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, marker_buffer_, lane != 0 ? 8 : 0,
                                    static_cast<u32>(id));
    }
    return id;
}

void Diagnostics::End(VkCommandBuffer cb, u64 id, u8 lane) {
    if (!enabled_ || id == 0 || markers_ == nullptr) {
        return;
    }
    Fn().vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, marker_buffer_, lane != 0 ? 12 : 4,
                                   static_cast<u32>(id));
}

Diagnostics* Diagnostics::Active() {
    return g_active.load(std::memory_order_acquire);
}

void Diagnostics::PresentBegin(VkCommandBuffer cb, u64 flip) {
    if (markers_ != nullptr) {
        Fn().vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, marker_buffer_, 16, static_cast<u32>(flip));
    }
}

void Diagnostics::PresentEnd(VkCommandBuffer cb, u64 flip) {
    if (markers_ != nullptr) {
        Fn().vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, marker_buffer_, 20,
                                       static_cast<u32>(flip));
    }
}

void Diagnostics::PresentStep(u32 step, u64 flip, s32 result) {
    if (step < 3) {
        present_flip_[step].store(flip, std::memory_order_relaxed);
        present_ns_[step].store(SteadyNs(), std::memory_order_relaxed);
    }
    if (step == 2) {
        present_result_.store(result, std::memory_order_relaxed);
    }
}

void Diagnostics::Submitted(u64 batch, u64 first_cmd, u64 last_cmd, u8 lane) {
    if (!enabled_) {
        return;
    }
    std::scoped_lock lock{mutex_};
    batches_.push_back({batch, {first_cmd, last_cmd}});
    batch_lanes_.push_back(lane);
    if (batches_.size() > 256) {
        batches_.erase(batches_.begin(), batches_.begin() + 128);
        batch_lanes_.erase(batch_lanes_.begin(), batch_lanes_.begin() + 128);
    }
}

void Diagnostics::Name(VkObjectType type, u64 handle, const std::string& name) {
    const auto& f = Fn();
    if (!naming_ || handle == 0 || f.vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT ni{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                           .objectType = type,
                                           .objectHandle = handle,
                                           .pObjectName = name.c_str()};
    f.vkSetDebugUtilsObjectNameEXT(ctx_.Device(), &ni);
}

void Diagnostics::Alias(u64 key, const std::string& line) {
    if (!enabled_ || alias_log_ == nullptr) {
        return;
    }
    std::scoped_lock lock{alias_mutex_};
    if (std::ranges::find(alias_keys_, key) != alias_keys_.end()) {
        return;
    }
    alias_keys_.push_back(key);
    fmt::print(alias_log_, "[{}] submission {} {}\n", Now(), g_diag_submission.load(std::memory_order_relaxed), line);
    std::fflush(alias_log_);
}

std::string Diagnostics::RecordLines(u64 from, u64 to) {
    std::string out;
    for (u64 id = std::max<u64>(from, 1); id <= to && id < next_id_; ++id) {
        const Record& r = ring_[id % kRing];
        if (r.id == id) {
            out += fmt::format("  cmd {} (batch {}): {}\n", r.id, r.batch, r.text);
        }
    }
    return out;
}

std::filesystem::path Diagnostics::DeviceLost(const std::string& where, u64 failing_batch, const std::string& in_flight) {
    const auto path = dir_ / "gpu_device_lost.txt";
    if (lost_.exchange(true, std::memory_order_acq_rel)) {
        return path;
    }
    const auto& f = Fn();
    std::string out = fmt::format("SecondSonPC native GPU backend: VK_ERROR_DEVICE_LOST\n"
                                  "time {}\nwhere: {}\nfailing submission (timeline value): {}\n"
                                  "diagnostics: {}\n\nbatches in flight:\n{}\n",
                                  Now(), where, failing_batch, enabled_ ? "on (--gpu-native-diag)" : "off", in_flight);
    // the device fault (VK_EXT_device_fault)
    if (f.vkGetDeviceFaultInfoEXT != nullptr) {
        VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        const VkResult r0 = f.vkGetDeviceFaultInfoEXT(ctx_.Device(), &counts, nullptr);
        out += fmt::format("device fault: counts {} (address infos {}, vendor infos {}, vendor binary {} bytes)\n",
                           static_cast<int>(r0), counts.addressInfoCount, counts.vendorInfoCount,
                           counts.vendorBinarySize);
        if (r0 == VK_SUCCESS || r0 == VK_INCOMPLETE) {
            std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            counts.vendorBinarySize = 0; // the binary is not read
            VkDeviceFaultInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                                      .pAddressInfos = addrs.empty() ? nullptr : addrs.data(),
                                      .pVendorInfos = vendors.empty() ? nullptr : vendors.data()};
            const VkResult r1 = f.vkGetDeviceFaultInfoEXT(ctx_.Device(), &counts, &info);
            out += fmt::format("  result {}, description: {}\n", static_cast<int>(r1), info.description);
            for (const auto& a : addrs) {
                out += fmt::format("  address {:#x} (precision {:#x}): {}\n", a.reportedAddress, a.addressPrecision,
                                   FaultAddressType(a.addressType));
            }
            for (const auto& v : vendors) {
                out += fmt::format("  vendor: {} (code {:#x}, data {:#x})\n", v.description, v.vendorFaultCode,
                                   v.vendorFaultData);
            }
        }
    } else {
        out += "device fault: VK_EXT_device_fault not enabled\n";
    }
    // the NVIDIA checkpoints (FrameBreak AQ: the async compute lane's queue too)
    u64 last_cp = 0, lane_cp = 0;
    if (f.vkGetQueueCheckpointDataNV != nullptr) {
        for (const bool lane : {false, true}) {
            const VkQueue queue = lane ? ctx_.LaneQueue() : ctx_.Queue();
            if (queue == VK_NULL_HANDLE || (lane && queue == ctx_.Queue())) {
                continue;
            }
            u32 n = 0;
            f.vkGetQueueCheckpointDataNV(queue, &n, nullptr);
            std::vector<VkCheckpointDataNV> cps(n, VkCheckpointDataNV{.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
            f.vkGetQueueCheckpointDataNV(queue, &n, cps.data());
            out += fmt::format("checkpoints{} ({}):\n", lane ? " of the async compute lane" : "", n);
            for (const auto& c : cps) {
                const u64 id = static_cast<u64>(reinterpret_cast<uintptr_t>(c.pCheckpointMarker));
                u64& last = lane ? lane_cp : last_cp;
                last = std::max(last, id);
                out += fmt::format("  stage {:#x} ({}): command {}\n", static_cast<u32>(c.stage), StageName(c.stage),
                                   id);
            }
        }
    } else {
        out += "checkpoints: VK_NV_device_diagnostic_checkpoints not enabled\n";
    }
    // the buffer markers: the last command started (top of pipe) and completed (bottom of pipe); AQ: the lane's too
    u64 started = 0, completed = 0, lane_started = 0, lane_completed = 0;
    if (markers_ != nullptr) {
        started = markers_[0];
        completed = markers_[1];
        lane_started = markers_[2];
        lane_completed = markers_[3];
        out += fmt::format("buffer markers: last started command {} (low 32 bits), last completed command {}; the "
                           "async compute lane's {} / {}\n",
                           started, completed, lane_started, lane_completed);
    } else {
        out += "buffer markers: not available\n";
    }
    // the presenter: its submissions share the game's queue (a present waits there for its swapchain image)
    {
        const s64 now = SteadyNs();
        const auto ago = [&](u32 step) {
            const s64 t = present_ns_[step].load(std::memory_order_relaxed);
            return t != 0 ? fmt::format("{} ms ago", (now - t) / 1'000'000) : std::string("never");
        };
        out += fmt::format("presenter (host): image acquired for flip {} ({}), submitted flip {} ({}), presented flip {} "
                           "({}, result {})\n",
                           present_flip_[0].load(), ago(0), present_flip_[1].load(), ago(1), present_flip_[2].load(),
                           ago(2), present_result_.load());
        if (markers_ != nullptr) {
            out += fmt::format("presenter (GPU markers): last started flip {}, last completed flip {} (low 32 bits)\n",
                               markers_[4], markers_[5]);
        }
    }
    if (enabled_) {
        std::scoped_lock lock{mutex_};
        // the marker values are the ids' low 32 bits: the ids of this run stay below 2^32
        const u64 hi = std::max({started, completed, last_cp});
        const u64 lo = std::min(completed != 0 ? completed : hi, last_cp != 0 ? last_cp : hi);
        out += fmt::format("\nthe journal from the last completed command to past the last started one ({}..{}):\n",
                           lo > 8 ? lo - 8 : 1, hi + 16);
        out += RecordLines(lo > 8 ? lo - 8 : 1, hi + 16);
        if (lane_started != 0 || lane_cp != 0) {
            const u64 lhi = std::max({lane_started, lane_completed, lane_cp});
            const u64 llo = std::min(lane_completed != 0 ? lane_completed : lhi, lane_cp != 0 ? lane_cp : lhi);
            out += fmt::format("\nthe async compute lane's journal from its last completed command to past its last "
                               "started one ({}..{}):\n", llo > 8 ? llo - 8 : 1, lhi + 16);
            out += RecordLines(llo > 8 ? llo - 8 : 1, lhi + 16);
        }
        out += "\nthe last batches submitted (value: commands):\n";
        for (size_t i = batches_.size() > 48 ? batches_.size() - 48 : 0; i < batches_.size(); ++i) {
            out += fmt::format("  {}: {}..{}{}\n", batches_[i].first, batches_[i].second.first,
                               batches_[i].second.second, batch_lanes_[i] != 0 ? " (lane)" : "");
        }
        out += fmt::format("\nthe last 64 commands recorded ({}..{}):\n", next_id_ > 64 ? next_id_ - 64 : 1,
                           next_id_ - 1);
        out += RecordLines(next_id_ > 64 ? next_id_ - 64 : 1, next_id_ - 1);
    }
    if (FILE* file = OpenText(path)) {
        std::fwrite(out.data(), 1, out.size(), file);
        std::fclose(file);
    }
    SSPC_LOG(GPU, Error, "native backend: VK_ERROR_DEVICE_LOST ({}); GPU submissions stopped; report {}", where,
             path.string());
    return path;
}

} // namespace SecondSon::Gpu::Vk
