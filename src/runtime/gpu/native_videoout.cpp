// SecondSonPC — SecondSonPC's own libSceVideoOut (P4.4). See native_videoout.h. Ported from shadPS4's
// core/libraries/videoout (video_out.cpp, driver.cpp; GPL-2.0-or-later).
#include <windows.h>

#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/r10/spread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <immintrin.h>

#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <vector>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/kernel/clock.h"
#include "runtime/kernel/event_queues.h"

namespace SecondSon::Gpu::VideoOut {

namespace {

constexpr s32 kErrInvalidValue = static_cast<s32>(0x80290001);
constexpr s32 kErrInvalidAddress = static_cast<s32>(0x80290002);
constexpr s32 kErrInvalidPitch = static_cast<s32>(0x80290004);
constexpr s32 kErrInvalidTilingMode = static_cast<s32>(0x80290007);
constexpr s32 kErrInvalidAspectRatio = static_cast<s32>(0x80290008);
constexpr s32 kErrResourceBusy = static_cast<s32>(0x80290009);
constexpr s32 kErrInvalidIndex = static_cast<s32>(0x8029000A);
constexpr s32 kErrInvalidHandle = static_cast<s32>(0x8029000B);
constexpr s32 kErrNoEmptySlot = static_cast<s32>(0x8029000F);
constexpr s32 kErrSlotOccupied = static_cast<s32>(0x80290010);
constexpr s32 kErrFlipQueueFull = static_cast<s32>(0x80290012);

constexpr u32 kMaxGroups = 4;
constexpr u64 kFlipEvent = 0x6, kVblankEvent = 0x7;

/// The game's structures (SCE layouts).
struct BufferAttribute {
    u32 pixel_format;
    s32 tiling_mode;
    s32 aspect_ratio;
    u32 width;
    u32 height;
    u32 pitch_in_pixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};
static_assert(sizeof(BufferAttribute) == 40);

struct FlipStatus {
    u64 count = 0;
    u64 process_time = 0;
    u64 tsc = 0;
    s64 flip_arg = -1;
    u64 submit_tsc = 0;
    u64 reserved0 = 0;
    s32 gc_queue_num = 0;
    s32 flip_pending_num = 0;
    s32 current_buffer = -1;
    u32 reserved1 = 0;
};
static_assert(sizeof(FlipStatus) == 64);

struct VblankStatus {
    u64 count = 0;
    u64 process_time = 0;
    u64 tsc = 0;
    u64 reserved = 0;
};

struct ColorSettings {
    float gamma;
    u32 reserved[3];
};

struct Mode {
    u32 size;
    u8 encoding;
    u8 range;
    u8 colorimetry;
    u8 depth;
    u64 refresh_rate;
    u64 resolution;
    u8 reserved[8];
};

struct Slot {
    s32 group = -1;
    u64 address = 0;
};

struct Group {
    bool occupied = false;
    BufferAttribute attrib{};
};

struct PendingEop {
    u32 index = 0;
    s64 flip_arg = 0;
};

struct Port {
    std::mutex mutex; ///< everything below but the labels and the vblank status
    bool open = false;
    s32 flip_rate = 0;
    s32 prev_index = -1;
    bool hdr = false;
    float gamma = 1.0f;
    std::array<Slot, kMaxBuffers> slots{};
    std::array<Group, kMaxGroups> groups{};
    FlipStatus flip{};
    std::deque<PendingEop> pending_eop;
    std::deque<FlipRequest> requests;
    std::vector<Kernel::EventQueues::QueueId> flip_events, vblank_events;
    u64 next_flip_id = 1;

    std::mutex vblank_mutex;
    VblankStatus vblank{};
};

Port g_port;
std::atomic<u64> g_next_flip_id{1}; ///< g_port.next_flip_id as NextFlipId reads it (written with it, under the mutex)
// the labels: the GPU writes them (the patched flip's WRITE_DATA), the game waits on them (WAIT_REG_MEM), the
// present thread clears the previous buffer's after each flip
alignas(64) std::array<std::atomic<u64>, kMaxBuffers> g_labels{};
static_assert(sizeof(std::atomic<u64>) == 8);

Sink* g_sink = nullptr;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};
void (*g_label_cb)(void*) = nullptr;
void* g_label_ctx = nullptr;

std::mutex g_stats_mutex;
Stats g_stats;

u64 Tsc() {
    return Kernel::Clock::ReadTsc();
}
u64 ProcessTime() {
    return Kernel::Clock::ProcessTimeUs();
}

void LabelChanged() {
    if (g_label_cb != nullptr) {
        g_label_cb(g_label_ctx);
    }
}

Port* GetPort(s32 handle) {
    return handle == 1 ? &g_port : nullptr;
}

void TriggerEvents(const std::vector<Kernel::EventQueues::QueueId>& queues, u64 ident, u64 data) {
    auto* owner = Kernel::EventQueues::GetOwner();
    if (owner == nullptr) {
        return;
    }
    for (const auto& q : queues) {
        owner->Trigger(q, ident, Kernel::EventQueues::Filter::VideoOut, data);
    }
}

/// One frame shown: the status, the events, the previous buffer's label released.
void CompleteFlip(const FlipRequest& r) {
    std::vector<Kernel::EventQueues::QueueId> events;
    s32 prev = -1;
    {
        std::scoped_lock lock{g_port.mutex};
        auto& f = g_port.flip;
        ++f.count;
        f.process_time = ProcessTime();
        f.tsc = Tsc();
        f.flip_arg = r.flip_arg;
        f.current_buffer = r.index;
        if (r.eop) {
            --f.gc_queue_num;
        }
        --f.flip_pending_num;
        events = g_port.flip_events;
        prev = g_port.prev_index;
        g_port.prev_index = r.index;
    }
    TriggerEvents(events, kFlipEvent, kFlipEvent | (static_cast<u64>(r.flip_arg) << 16));
    if (prev >= 0 && prev < static_cast<s32>(kMaxBuffers)) {
        g_labels[prev].store(0, std::memory_order_release);
        LabelChanged();
    }
}

// FrameBreak FB-2: the pacing, packed (bit 0 vblank, bits 8..31 the cap, bits 32..47 the spin); g_wake (auto-reset)
// wakes the present thread for a new request or a pacing change
std::atomic<u64> g_pacing{1};
HANDLE g_wake = nullptr;

u64 PackPacing(const Pacing& p) {
    return (p.vblank ? 1ull : 0ull) | (u64{std::min<u32>(p.cap_fps, 0xFFFFFF)} << 8) |
           (u64{std::min<u32>(p.spin_us, 0xFFFF)} << 32);
}
Pacing UnpackPacing(u64 v) {
    return {(v & 1) != 0, static_cast<u32>((v >> 8) & 0xFFFFFF), static_cast<u32>((v >> 32) & 0xFFFF)};
}

void Wake() {
    if (g_wake != nullptr) {
        SetEvent(g_wake);
    }
}

void Present(const FlipRequest& r) {
    if (g_sink != nullptr) {
        g_sink->Present(r);
    }
    if (r.eop) {
        FrameLog::Presented();
    }
    CompleteFlip(r);
    std::scoped_lock s{g_stats_mutex};
    ++g_stats.flips_presented;
    g_stats.blank += r.index < 0 ? 1 : 0;
}

/// One emulated vblank: its events and status (both pacings keep the PS4's 59.94 Hz count).
void Vblank(u64 vcount) {
    std::vector<Kernel::EventQueues::QueueId> vblank_events;
    {
        std::scoped_lock lock{g_port.mutex};
        vblank_events = g_port.vblank_events;
    }
    TriggerEvents(vblank_events, kVblankEvent, kVblankEvent | (vcount << 16));
    {
        std::scoped_lock lock{g_port.vblank_mutex};
        ++g_port.vblank.count;
        g_port.vblank.process_time = ProcessTime();
        g_port.vblank.tsc = Tsc();
    }
    std::scoped_lock s{g_stats_mutex};
    ++g_stats.vblanks;
}

/// Sleeps until the QPC value `until` or a wake (a request, a pacing change), whichever first.
void SleepUntil(HANDLE timer, double until, const LARGE_INTEGER& freq) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double wait = until - static_cast<double>(now.QuadPart);
    if (wait <= 0) {
        return;
    }
    const LONGLONG hundred_ns = static_cast<LONGLONG>(wait * 1e7 / static_cast<double>(freq.QuadPart));
    if (timer != nullptr && hundred_ns > 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -hundred_ns;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        const HANDLE h[2] = {timer, g_wake};
        WaitForMultipleObjects(g_wake != nullptr ? 2 : 1, h, FALSE, 100);
    } else if (hundred_ns >= 10000) {
        Sleep(static_cast<DWORD>(hundred_ns / 10000));
    }
}

/// R9-F4 (SSPC_R9_PACE=<percent>, 50..100; off by default; docs/R9_GAMEPLAY_PERFORMANCE.md §10): the fraction of the
/// average interval between the frames' GPU completions that a free flip keeps after the last one (0: off).
double PaceFraction() {
    static const double fraction = [] {
        char v[8] = {};
        const u64 n = GetEnvironmentVariableA("SSPC_R9_PACE", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) : 0ull;
        if (n == 0) {
            return 0.0;
        }
        const u64 percent = std::clamp<u64>(n, 50, 100);
        SSPC_LOG(GPU, Info, "VideoOut: R9-F4 SSPC_R9_PACE={} - a free flip at least {}% of the average interval between "
                            "the frames' GPU completions after the last one",
                 n, percent);
        return static_cast<double>(percent) / 100.0;
    }();
    return fraction;
}

/// The free pacing (FrameBreak FB-2): the oldest request is shown as soon as its GPU work completed and the cap (and
/// the game's own flip rate, its "lock to 30" option) allow; the vblank count still advances at 59.94 Hz. Returns when
/// the pacing changes back to the vblank one or the thread stops.
/// R9-F4: with REA-3 in a GPU-bound scene the frames complete in pairs (the next frame's async compute runs before
/// this one's flip: 17 / 43 ms); the pacer shows them at an even cadence. Its average follows the completions, not
/// the flips (its own delay does not feed it); an interval over 100 ms (a hitch, a load) restarts it.
void FreeLoop(HANDLE timer, const LARGE_INTEGER& freq, double period_ticks, double& next_tick) {
    LARGE_INTEGER now{};
    double last_flip = 0.0; // QPC of the last flip taken
    const double pace = PaceFraction();
    const double pace_restart = static_cast<double>(freq.QuadPart) / 10.0; // 100 ms
    double ready_at = 0.0, ready_avg = 0.0; // QPC of the last completion seen, the average interval (0: none yet)
    u64 ready_id = 0;                       // the flip whose completion it is
    bool paced = false;                     // the front request is held back by the pacer
    for (;;) {
        if (g_stop.load(std::memory_order_acquire)) {
            return;
        }
        const Pacing p = UnpackPacing(g_pacing.load(std::memory_order_acquire));
        if (p.vblank || (g_sink != nullptr && g_sink->NeedsVblank())) {
            return;
        }
        QueryPerformanceCounter(&now);
        double t = static_cast<double>(now.QuadPart);
        if (t >= next_tick) {
            u64 vcount = 0;
            {
                std::scoped_lock lock{g_port.vblank_mutex};
                vcount = g_port.vblank.count;
            }
            Vblank(vcount);
            next_tick += period_ticks;
            if (t > next_tick + 4 * period_ticks) {
                next_tick = t + period_ticks; // far behind (a debugger, a stall): resync
            }
        }
        FlipRequest front;
        bool have = false;
        s32 rate = 0;
        {
            std::scoped_lock lock{g_port.mutex};
            if (g_port.open && !g_port.requests.empty()) {
                front = g_port.requests.front();
                rate = g_port.flip_rate;
                have = true;
            }
        }
        double wake_at = next_tick;
        if (have) {
            // the cap, and the game's flip rate (1: 30 Hz, 2: 20 Hz - its own frame rate lock) as a minimum interval
            double interval = p.cap_fps != 0 ? static_cast<double>(freq.QuadPart) / p.cap_fps : 0.0;
            interval = std::max(interval, static_cast<double>(rate + 1) * period_ticks * (rate > 0 ? 1.0 : 0.0));
            double earliest = last_flip + interval;
            if (g_sink != nullptr && !g_sink->Ready(front.gpu_value)) {
                // the frame's GPU work: waited for here (bounded by the next vblank tick)
                const double left = next_tick - t;
                const u64 ns = left > 0 ? static_cast<u64>(left * 1e9 / static_cast<double>(freq.QuadPart)) : 0;
                if (!g_sink->WaitReady(front.gpu_value, std::max<u64>(ns, 100'000))) {
                    QueryPerformanceCounter(&now);
                    if (static_cast<double>(now.QuadPart) + period_ticks / 8 < next_tick) {
                        SleepUntil(timer, next_tick, freq); // a failure, not a timeout: no busy loop
                    }
                }
                continue;
            }
            if (pace > 0.0) {
                if (front.flip_id != ready_id) {
                    // (its completion: first seen ready here - at most a wake-up late)
                    const double iv = t - ready_at;
                    ready_avg = ready_at == 0.0 || iv > pace_restart ? 0.0
                                : ready_avg == 0.0                   ? iv
                                                                     : ready_avg + (iv - ready_avg) / 16.0;
                    ready_at = t;
                    ready_id = front.flip_id;
                }
                const double paced_at = last_flip + pace * ready_avg;
                if (ready_avg > 0.0 && paced_at > earliest) {
                    earliest = paced_at;
                    paced = paced || t < earliest;
                }
            }
            const double spin = static_cast<double>(p.spin_us) * static_cast<double>(freq.QuadPart) / 1e6;
            if (t + spin >= earliest) {
                if (t < earliest) {
                    const StallTrace::Scope trace_cap(StallTrace::Ev::VblankWait);
                    do {
                        _mm_pause();
                        QueryPerformanceCounter(&now);
                    } while (static_cast<double>(now.QuadPart) < earliest);
                    t = static_cast<double>(now.QuadPart);
                }
                {
                    std::scoped_lock lock{g_port.mutex};
                    if (g_port.requests.empty()) {
                        continue; // (closed meanwhile)
                    }
                    front = g_port.requests.front(); // only this thread takes requests: the same one
                    g_port.requests.pop_front();
                }
                last_flip = t;
                Present(front);
                std::scoped_lock s{g_stats_mutex};
                ++g_stats.free_flips;
                g_stats.paced += paced ? 1 : 0;
                paced = false;
                continue;
            }
            {
                std::scoped_lock s{g_stats_mutex};
                ++g_stats.capped_waits;
            }
            wake_at = std::min(wake_at, earliest - spin);
        }
        const StallTrace::Scope trace_idle(StallTrace::Ev::VblankWait); // (P4.12)
        SleepUntil(timer, wake_at, freq);
    }
}

void PresentThread() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:VideoOutPresent");
    R10::SpreadThisThread(R10::Heavy::Present); // (SSPC_R10_SPREAD)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    StallTrace::RegisterThread(StallTrace::Role::Present);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    // 59.94 Hz, as the PS4's output (and the bridge's vblank)
    const double period_ticks = static_cast<double>(freq.QuadPart) * 1001.0 / 60000.0;
    double next = static_cast<double>(now.QuadPart) + period_ticks;
    const auto present = [](const FlipRequest& r) { Present(r); };
    bool fg_logged = false;
    while (!g_stop.load(std::memory_order_acquire)) {
        if (!UnpackPacing(g_pacing.load(std::memory_order_acquire)).vblank) {
            if (g_sink == nullptr || !g_sink->NeedsVblank()) {
                FreeLoop(timer, freq, period_ticks, next);
                continue;
            }
            if (!fg_logged) {
                fg_logged = true;
                SSPC_LOG(GPU, Info, "VideoOut: frame generation on - flips at the vblank (the frame rate limit applies "
                                    "without it)");
            }
        }
        QueryPerformanceCounter(&now);
        const double wait = next - static_cast<double>(now.QuadPart);
        if (wait > 0) {
            const StallTrace::Scope trace_vblank(StallTrace::Ev::VblankWait); // (P4.12)
            const LONGLONG hundred_ns = static_cast<LONGLONG>(wait * 1e7 / static_cast<double>(freq.QuadPart));
            if (timer != nullptr && hundred_ns > 0) {
                LARGE_INTEGER due{};
                due.QuadPart = -hundred_ns;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else {
                Sleep(static_cast<DWORD>(hundred_ns / 10000));
            }
        }
        next += period_ticks;
        QueryPerformanceCounter(&now);
        if (static_cast<double>(now.QuadPart) > next + 4 * period_ticks) {
            next = static_cast<double>(now.QuadPart) + period_ticks; // far behind (a debugger, a stall): resync
        }

        u64 vcount = 0;
        {
            std::scoped_lock lock{g_port.vblank_mutex};
            vcount = g_port.vblank.count;
        }
        FlipRequest req;
        bool take = false;
        {
            std::scoped_lock lock{g_port.mutex};
            // SSPC_DIAG_FLIP_INTERVAL=<n>: a flip at least n vblanks after the last one (a steady frame cadence: the
            // game's timestep follows the flips; an experiment, P4.5)
            static const u64 min_interval = [] {
                char v[8] = {};
                const u64 n = GetEnvironmentVariableA("SSPC_DIAG_FLIP_INTERVAL", v, sizeof(v)) > 0
                                  ? std::strtoull(v, nullptr, 10)
                                  : 0ull;
                if (n != 0) {
                    SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_FLIP_INTERVAL={} - a flip every {}+ vblanks",
                             n, n);
                }
                return n;
            }();
            static u64 last_flip_vcount = 0;
            if (g_port.open && !g_port.requests.empty() && vcount % static_cast<u64>(g_port.flip_rate + 1) == 0 &&
                (min_interval == 0 || vcount >= last_flip_vcount + min_interval)) {
                if (g_sink == nullptr || g_sink->Ready(g_port.requests.front().gpu_value)) {
                    req = g_port.requests.front();
                    g_port.requests.pop_front();
                    take = true;
                    last_flip_vcount = vcount;
                } else {
                    StallTrace::Instant(StallTrace::Ev::NotReady, g_port.requests.front().gpu_value); // (P4.12)
                    std::scoped_lock s{g_stats_mutex};
                    ++g_stats.waited_vblanks;
                }
            }
        }
        if (take) {
            present(req);
        }
        Vblank(vcount);
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
}

bool ValidAttribute(const BufferAttribute* a, s32& error) {
    if (a->reserved0 != 0 || a->reserved1 != 0) {
        error = kErrInvalidValue;
        return false;
    }
    if (a->aspect_ratio != 0) {
        error = kErrInvalidAspectRatio;
        return false;
    }
    if (a->width > a->pitch_in_pixel) {
        error = kErrInvalidPitch;
        return false;
    }
    if (a->tiling_mode < 0 || a->tiling_mode > 1) {
        error = kErrInvalidTilingMode;
        return false;
    }
    return true;
}

// ---- the game's functions

SSPC_SYSV_ABI void SetBufferAttribute(BufferAttribute* attribute, u32 pixel_format, u32 tiling_mode, u32 aspect_ratio,
                                      u32 width, u32 height, u32 pitch_in_pixel) {
    if (attribute == nullptr) {
        return;
    }
    SSPC_LOG(GPU, Info, "VideoOut: buffer attribute: pixel format {:#x}, tiling {}, aspect {}, {}x{}, pitch {}",
             pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel);
    std::memset(attribute, 0, sizeof(BufferAttribute));
    attribute->pixel_format = pixel_format;
    attribute->tiling_mode = static_cast<s32>(tiling_mode);
    attribute->aspect_ratio = static_cast<s32>(aspect_ratio);
    attribute->width = width;
    attribute->height = height;
    attribute->pitch_in_pixel = pitch_in_pixel;
}

SSPC_SYSV_ABI s32 Open(s32 /*user_id*/, s32 bus_type, s32 index, const void* /*param*/) {
    if (bus_type != 0 || index != 0) {
        SSPC_LOG(GPU, Error, "VideoOut: open of bus {} index {} (only the main port 0 exists)", bus_type, index);
        return kErrInvalidValue;
    }
    std::scoped_lock lock{g_port.mutex};
    if (g_port.open) {
        return kErrResourceBusy;
    }
    g_port.open = true;
    SSPC_LOG(GPU, Info, "VideoOut: main port opened (handle 1)");
    return 1;
}

SSPC_SYSV_ABI s32 Close(s32 handle) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::vector<Kernel::EventQueues::QueueId> flip_events, vblank_events;
    {
        std::scoped_lock lock{port->mutex};
        port->open = false;
        port->flip_rate = 0;
        port->prev_index = -1;
        port->slots.fill(Slot{});
        port->groups.fill(Group{});
        port->flip = FlipStatus{};
        port->pending_eop.clear();
        port->requests.clear();
        flip_events.swap(port->flip_events);
        vblank_events.swap(port->vblank_events);
    }
    for (auto& l : g_labels) {
        l.store(0, std::memory_order_release);
    }
    if (auto* owner = Kernel::EventQueues::GetOwner()) {
        for (const auto& q : flip_events) {
            owner->RemoveEvent(q, kFlipEvent, Kernel::EventQueues::Filter::VideoOut);
        }
        for (const auto& q : vblank_events) {
            owner->RemoveEvent(q, kVblankEvent, Kernel::EventQueues::Filter::VideoOut);
        }
    }
    LabelChanged();
    return 0;
}

SSPC_SYSV_ABI s32 RegisterBuffers(s32 handle, s32 start_index, void* const* addresses, s32 buffer_num,
                                  const BufferAttribute* attribute) {
    if (addresses == nullptr || attribute == nullptr) {
        return kErrInvalidAddress;
    }
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::scoped_lock lock{port->mutex};
    if (!port->open) {
        return kErrInvalidHandle;
    }
    s32 group = 0;
    while (group < static_cast<s32>(kMaxGroups) && port->groups[group].occupied) {
        ++group;
    }
    if (group >= static_cast<s32>(kMaxGroups)) {
        return kErrNoEmptySlot;
    }
    if (start_index < 0 || buffer_num < 0 || start_index + buffer_num > static_cast<s32>(kMaxBuffers)) {
        return kErrInvalidValue;
    }
    for (s32 i = start_index; i < start_index + buffer_num; ++i) {
        if (port->slots[i].group != -1) {
            return kErrSlotOccupied;
        }
    }
    s32 error = 0;
    if (!ValidAttribute(attribute, error)) {
        return error;
    }
    port->groups[group].occupied = true;
    port->groups[group].attrib = *attribute;
    for (s32 i = 0; i < buffer_num; ++i) {
        const u64 address = reinterpret_cast<u64>(addresses[i]);
        port->slots[start_index + i] = {group, address};
        g_labels[start_index + i].store(0, std::memory_order_release);
        SSPC_LOG(GPU, Info, "VideoOut: buffer {} = {:#x} (group {}, format {:#x}, tiling {}, {}x{}, pitch {})",
                 start_index + i, address, group, attribute->pixel_format, attribute->tiling_mode, attribute->width,
                 attribute->height, attribute->pitch_in_pixel);
    }
    return group;
}

SSPC_SYSV_ABI s32 UnregisterBuffers(s32 handle, s32 attribute_index) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::scoped_lock lock{port->mutex};
    if (!port->open) {
        return kErrInvalidHandle;
    }
    if (attribute_index < 0 || attribute_index >= static_cast<s32>(kMaxGroups) ||
        !port->groups[attribute_index].occupied) {
        return kErrInvalidValue;
    }
    port->groups[attribute_index].occupied = false;
    for (auto& s : port->slots) {
        if (s.group == attribute_index) {
            s = Slot{};
        }
    }
    return 0;
}

SSPC_SYSV_ABI s32 SetFlipRate(s32 handle, s32 rate) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    if (rate < 0 || rate > 2) {
        return kErrInvalidValue;
    }
    SSPC_LOG(GPU, Info, "VideoOut: flip rate {} ({} Hz)", rate, 60 / (rate + 1));
    std::scoped_lock lock{port->mutex};
    port->flip_rate = rate;
    return 0;
}

SSPC_SYSV_ABI s32 IsFlipPending(s32 handle) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::scoped_lock lock{port->mutex};
    return port->flip.flip_pending_num;
}

SSPC_SYSV_ABI s32 GetFlipStatus(s32 handle, FlipStatus* status) {
    if (status == nullptr) {
        return kErrInvalidAddress;
    }
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::scoped_lock lock{port->mutex};
    *status = port->flip;
    return 0;
}

SSPC_SYSV_ABI s32 GetDeviceCapabilityInfo(s32 handle, u64* capability) {
    if (capability == nullptr) {
        return kErrInvalidAddress;
    }
    if (GetPort(handle) == nullptr) {
        return kErrInvalidHandle;
    }
    *capability = 0; // SDR output (no BT.2020 PQ)
    return 0;
}

SSPC_SYSV_ABI s32 ColorSettingsSetGamma(ColorSettings* settings, float gamma) {
    if (settings == nullptr) {
        return kErrInvalidAddress;
    }
    if (gamma < 0.1f || gamma > 2.0f) {
        return kErrInvalidValue;
    }
    settings->gamma = gamma;
    return 0;
}

SSPC_SYSV_ABI s32 AdjustColor(s32 handle, const ColorSettings* settings) {
    if (settings == nullptr) {
        return kErrInvalidAddress;
    }
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    std::scoped_lock lock{port->mutex};
    port->gamma = settings->gamma;
    SSPC_LOG(GPU, Info, "VideoOut: gamma {} requested (the presenter shows the buffer unchanged)", settings->gamma);
    return 0;
}

SSPC_SYSV_ABI void ModeSetAny(Mode* mode, u32 size) {
    if (mode == nullptr || size < 4) {
        return;
    }
    std::memset(mode, 0xff, size);
    mode->size = size;
}

SSPC_SYSV_ABI s32 ConfigureOutputMode(s32 handle, u32 reserved, const Mode* mode, const void* /*options*/,
                                      u32 /*size_mode*/, u32 /*size_options*/) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    if (reserved != 0 || mode == nullptr) {
        return kErrInvalidValue;
    }
    std::scoped_lock lock{port->mutex};
    switch (mode->colorimetry) {
    case 0xff: // any
        port->hdr = false;
        return 0;
    case 2: // BT.2020 PQ: SDR only here (the game's capability query said so)
        port->hdr = false;
        return 0;
    default:
        return kErrInvalidValue;
    }
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

constexpr const char* kGroup = "videoout";

const Runtime::Export kExports[] = {
    {"pv9CI5VC+R0", "libSceVideoOut", "libSceVideoOut", "sceVideoOutAdjustColor_", Fn(&AdjustColor), nullptr, kGroup,
     false, true},
    {"uquVH4-Du78", "libSceVideoOut", "libSceVideoOut", "sceVideoOutClose", Fn(&Close), nullptr, kGroup, false, true},
    {"DYhhWbJSeRg", "libSceVideoOut", "libSceVideoOut", "sceVideoOutColorSettingsSetGamma_",
     Fn(&ColorSettingsSetGamma), nullptr, kGroup, false, true},
    {"N1bEoJ4SRw4", "libSceVideoOut", "libSceVideoOut", "sceVideoOutConfigureOutputMode_", Fn(&ConfigureOutputMode),
     nullptr, kGroup, false, true},
    {"kGVLc3htQE8", "libSceVideoOut", "libSceVideoOut", "sceVideoOutGetDeviceCapabilityInfo_",
     Fn(&GetDeviceCapabilityInfo), nullptr, kGroup, false, true},
    {"SbU3dwp80lQ", "libSceVideoOut", "libSceVideoOut", "sceVideoOutGetFlipStatus", Fn(&GetFlipStatus), nullptr,
     kGroup, false, true},
    {"zgXifHT9ErY", "libSceVideoOut", "libSceVideoOut", "sceVideoOutIsFlipPending", Fn(&IsFlipPending), nullptr,
     kGroup, false, true},
    {"pjkDsgxli6c", "libSceVideoOut", "libSceVideoOut", "sceVideoOutModeSetAny_", Fn(&ModeSetAny), nullptr, kGroup,
     false, true},
    {"Up36PTk687E", "libSceVideoOut", "libSceVideoOut", "sceVideoOutOpen", Fn(&Open), nullptr, kGroup, false, true},
    {"w3BY+tAEiQY", "libSceVideoOut", "libSceVideoOut", "sceVideoOutRegisterBuffers", Fn(&RegisterBuffers), nullptr,
     kGroup, false, true},
    {"i6-sR91Wt-4", "libSceVideoOut", "libSceVideoOut", "sceVideoOutSetBufferAttribute", Fn(&SetBufferAttribute),
     nullptr, kGroup, false, true},
    {"CBiu4mCE1DA", "libSceVideoOut", "libSceVideoOut", "sceVideoOutSetFlipRate", Fn(&SetFlipRate), nullptr, kGroup,
     false, true},
    {"N5KDtkIjjJ4", "libSceVideoOut", "libSceVideoOut", "sceVideoOutUnregisterBuffers", Fn(&UnregisterBuffers),
     nullptr, kGroup, false, true},
};

} // namespace

void SetPacing(const Pacing& pacing) {
    const u64 v = PackPacing(pacing);
    if (g_pacing.exchange(v, std::memory_order_acq_rel) != v) {
        SSPC_LOG(GPU, Info, "VideoOut: pacing {}{}", pacing.vblank ? "console (a flip at a 59.94 Hz vblank)" : "free",
                 pacing.vblank       ? std::string()
                 : pacing.cap_fps    ? fmt::format(", at most {} FPS (spin {} us)", pacing.cap_fps, pacing.spin_us)
                                     : std::string(", unlimited"));
    }
    Wake();
}

Pacing GetPacing() {
    return UnpackPacing(g_pacing.load(std::memory_order_acquire));
}

bool ParsePacing(const std::string& text, Pacing& out) {
    if (text == "console") {
        out = Pacing{.vblank = true};
        return true;
    }
    if (text == "unlimited") {
        out = Pacing{.vblank = false, .cap_fps = 0};
        return true;
    }
    char* end = nullptr;
    const unsigned long n = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0' || n < 20 || n > 1000) {
        return false;
    }
    out = Pacing{.vblank = false, .cap_fps = static_cast<u32>(n)};
    return true;
}

void Start(Sink* sink) {
    if (g_started.exchange(true)) {
        return;
    }
    g_sink = sink;
    if (g_wake == nullptr) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(&PresentThread);
}

void Stop() {
    if (!g_started.exchange(false)) {
        return;
    }
    g_stop.store(true, std::memory_order_release);
    Wake();
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_sink = nullptr;
}

bool Started() {
    return g_started.load(std::memory_order_acquire);
}

std::atomic<u64> g_requested_flips{0}; ///< SubmitEopFlip's requests (RequestedFlips)

s32 SubmitEopFlip(s32 handle, u32 index, u32 flip_mode, s64 flip_arg) {
    Port* port = GetPort(handle);
    if (port == nullptr) {
        return kErrInvalidHandle;
    }
    if (index >= kMaxBuffers) {
        return kErrInvalidIndex;
    }
    std::scoped_lock lock{port->mutex};
    if (port->slots[index].group < 0) {
        SSPC_LOG(GPU, Error, "VideoOut: GPU flip of buffer {}, which is not registered", index);
        return kErrInvalidIndex;
    }
    if (port->flip.flip_pending_num > 16) {
        return kErrFlipQueueFull;
    }
    if (flip_mode != 1) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) {
            SSPC_LOG(GPU, Warning, "VideoOut: flip mode {} (vsync flips are done for every mode)", flip_mode);
        }
    }
    ++port->flip.gc_queue_num;
    ++port->flip.flip_pending_num;
    port->flip.submit_tsc = Tsc();
    port->pending_eop.push_back({index, flip_arg});
    g_requested_flips.fetch_add(1, std::memory_order_release);
    std::scoped_lock s{g_stats_mutex};
    ++g_stats.flips_requested;
    return 0;
}

bool NextFlipBuffer(BufferInfo& out, s32& index) {
    std::scoped_lock lock{g_port.mutex};
    if (g_port.pending_eop.empty()) {
        return false;
    }
    index = static_cast<s32>(g_port.pending_eop.front().index);
    const Slot& slot = g_port.slots[index];
    if (slot.group < 0) {
        return false;
    }
    const BufferAttribute& a = g_port.groups[slot.group].attrib;
    out = {slot.address, a.pixel_format, static_cast<u32>(a.tiling_mode), a.width, a.height, a.pitch_in_pixel};
    return true;
}

bool IsDisplayBuffer(u64 address) {
    std::scoped_lock lock{g_port.mutex};
    for (const Slot& slot : g_port.slots) {
        if (slot.group >= 0 && slot.address == address) {
            return true;
        }
    }
    return false;
}

u64 RequestedFlips() {
    return g_requested_flips.load(std::memory_order_acquire);
}

u64 NextFlipId() {
    // SSPC_CITY_HOT_LOCKS=1 (SSPC_CITY_PERFORMANCE; docs/CITY_PERFORMANCE.md): without the port's mutex - the recording
    // thread asks for every buffer range of every draw (the lock was ~1% of that thread in the city)
    static const bool lock_free = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_CITY_HOT_LOCKS", v, sizeof(v)) == 1 && v[0] == '1';
    }();
    if (lock_free) {
        return g_next_flip_id.load(std::memory_order_acquire);
    }
    std::scoped_lock lock{g_port.mutex};
    return g_port.next_flip_id;
}

bool OnFlipMarker(u64 gpu_value, s32 frame) {
    std::scoped_lock lock{g_port.mutex};
    if (g_port.pending_eop.empty()) {
        std::scoped_lock s{g_stats_mutex};
        if (g_stats.markers_without_flip++ == 0) {
            SSPC_LOG(GPU, Warning, "VideoOut: a flip marker without a requested GPU flip (ignored)");
        }
        return false;
    }
    const PendingEop p = g_port.pending_eop.front();
    g_port.pending_eop.pop_front();
    FlipRequest r;
    r.index = static_cast<s32>(p.index);
    r.flip_arg = p.flip_arg;
    r.eop = true;
    r.gpu_value = gpu_value;
    r.frame = frame;
    r.flip_id = g_port.next_flip_id++;
    g_next_flip_id.store(g_port.next_flip_id, std::memory_order_release);
    const Slot& slot = g_port.slots[p.index];
    if (slot.group >= 0) {
        const BufferAttribute& a = g_port.groups[slot.group].attrib;
        r.buffer = {slot.address, a.pixel_format, static_cast<u32>(a.tiling_mode), a.width, a.height,
                    a.pitch_in_pixel};
    }
    g_port.requests.push_back(r);
    Wake(); // (the free pacing shows it as soon as its work completes)
    return true;
}

bool BufferLabelAddress(s32 handle, u64& address) {
    if (GetPort(handle) == nullptr) {
        return false;
    }
    address = reinterpret_cast<u64>(g_labels.data());
    return true;
}

bool LabelRead(u64 addr, void* dst, u64 bytes) {
    const u64 base = reinterpret_cast<u64>(g_labels.data());
    const u64 size = sizeof(g_labels);
    if (addr < base || bytes > size || addr - base > size - bytes) {
        return false;
    }
    // aligned 4/8-byte accesses (labels): one atomic load per label touched
    u8 tmp[sizeof(g_labels)];
    for (u32 i = 0; i < kMaxBuffers; ++i) {
        const u64 v = g_labels[i].load(std::memory_order_acquire);
        std::memcpy(tmp + i * 8, &v, 8);
    }
    std::memcpy(dst, tmp + (addr - base), bytes);
    return true;
}

bool IsLabel(u64 addr, u64 bytes) {
    const u64 base = reinterpret_cast<u64>(g_labels.data());
    const u64 size = sizeof(g_labels);
    return addr >= base && bytes <= size && addr - base <= size - bytes;
}

bool LabelWrite(u64 addr, const void* src, u64 bytes) {
    const u64 base = reinterpret_cast<u64>(g_labels.data());
    const u64 size = sizeof(g_labels);
    if (addr < base || bytes > size || addr - base > size - bytes) {
        return false;
    }
    const u64 off = addr - base;
    const u32 first = static_cast<u32>(off / 8), last = static_cast<u32>((off + bytes - 1) / 8);
    for (u32 i = first; i <= last; ++i) {
        u64 v = g_labels[i].load(std::memory_order_acquire);
        u8 b[8];
        std::memcpy(b, &v, 8);
        const u64 lo = std::max<u64>(off, u64{i} * 8), hi = std::min<u64>(off + bytes, u64{i} * 8 + 8);
        std::memcpy(b + (lo - u64{i} * 8), static_cast<const u8*>(src) + (lo - off), hi - lo);
        std::memcpy(&v, b, 8);
        g_labels[i].store(v, std::memory_order_release);
    }
    return true;
}

void SetLabelCallback(void (*fn)(void*), void* ctx) {
    g_label_ctx = ctx;
    g_label_cb = fn;
}

Stats GetStats() {
    std::scoped_lock s{g_stats_mutex};
    return g_stats;
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::Gpu::VideoOut
