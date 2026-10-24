// SecondSonPC — the mouse turns the game's camera directly (see mouse_camera.h).
#include "engine/mouse_camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "input/keyboard_mouse.h"
#include "runtime/pad/pad.h"

namespace SecondSon::Engine::MouseCamera {

namespace {

// The pad class (vtable 0xcddf70): slots +0x110 (0x24cb80) and +0x118 (0x24cc10) give a stick as x*|x|, y*|y| with
// the options' inversion flags (two sets: 0x6d5/0x6d6 and 0x6d7/0x6d8) - void (pad*, int stick, float* x, float* y).
constexpr u64 kVtableSlotA = 0xcde080, kOriginalA = 0x24cb80;
constexpr u64 kVtableSlotB = 0xcde088, kOriginalB = 0x24cc10;

using Getter = SSPC_SYSV_ABI void (*)(void* pad, int stick, float* x, float* y);

u64 g_base = 0;
Getter g_original_a = nullptr, g_original_b = nullptr;

struct Caller {
    u64 rva = 0; // return address in the eboot
    u8 slot = 0; // 0: +0x110, 1: +0x118
    int stick = 0;
    u64 calls = 0, moving = 0;
    float max_x = 0.0f, max_y = 0.0f;
    u64 pad = 0;
};
constexpr int kMaxCallers = 64;
std::mutex g_lock;
Caller g_callers[kMaxCallers];
int g_caller_count = 0;

void Note(u8 slot, void* pad, int stick, const float* x, const float* y, u64 ret) {
    const u64 rva = ret - g_base;
    std::scoped_lock lock{g_lock};
    Caller* c = nullptr;
    for (int i = 0; i < g_caller_count; ++i) {
        if (g_callers[i].rva == rva && g_callers[i].slot == slot && g_callers[i].stick == stick) {
            c = &g_callers[i];
            break;
        }
    }
    if (c == nullptr) {
        if (g_caller_count == kMaxCallers) {
            return;
        }
        c = &g_callers[g_caller_count++];
        c->rva = rva;
        c->slot = slot;
        c->stick = stick;
    }
    ++c->calls;
    c->pad = reinterpret_cast<u64>(pad);
    if (*x != 0.0f || *y != 0.0f) {
        ++c->moving;
        c->max_x = std::max(c->max_x, std::fabs(*x));
        c->max_y = std::max(c->max_y, std::fabs(*y));
    }
}

SSPC_SYSV_ABI void ProbeA(void* pad, int stick, float* x, float* y) {
    g_original_a(pad, stick, x, y);
    Note(0, pad, stick, x, y, reinterpret_cast<u64>(__builtin_return_address(0)));
}

SSPC_SYSV_ABI void ProbeB(void* pad, int stick, float* x, float* y) {
    g_original_b(pad, stick, x, y);
    Note(1, pad, stick, x, y, reinterpret_cast<u64>(__builtin_return_address(0)));
}

void Report() {
    std::string out;
    {
        std::scoped_lock lock{g_lock};
        for (int i = 0; i < g_caller_count; ++i) {
            const Caller& c = g_callers[i];
            out += fmt::format("\n  ret eboot+{:#x} slot +{:#x} stick {}: {} calls, {} moving, max |x| {:.3f} |y| {:.3f}, "
                               "pad {:#x}",
                               c.rva, c.slot == 0 ? 0x110 : 0x118, c.stick, c.calls, c.moving, c.max_x, c.max_y, c.pad);
        }
    }
    SSPC_LOG(Engine, Info, "MOUSE_PROBE stick getter callers:{}", out.empty() ? " none yet" : out);
}

/// Swaps the pointer at `at` from `expected` to `value` in one 8-byte write (a thread may be calling through it).
bool SwapSlot(u64 at, u64 expected, u64 value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 8, PAGE_READWRITE, &old)) {
        return false;
    }
    const bool swapped = static_cast<u64>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(at),
                                                                       static_cast<LONG64>(value),
                                                                       static_cast<LONG64>(expected))) == expected;
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(at), 8, old, &unused);
    return swapped;
}

/// The vtable's pointers are relocated by the loader after Install runs (they read 0x24cb80 / 0x24cc10 until then):
/// waits for the relocated values, then wraps them.
void InstallProbe(u64 base) {
    g_base = base;
    std::thread([base] {
        const auto* a = reinterpret_cast<const volatile u64*>(base + kVtableSlotA);
        const auto* b = reinterpret_cast<const volatile u64*>(base + kVtableSlotB);
        for (int ms = 0; *a != base + kOriginalA || *b != base + kOriginalB; ++ms) {
            const bool unrelocated = (*a == kOriginalA || *a == base + kOriginalA) &&
                                     (*b == kOriginalB || *b == base + kOriginalB);
            if (!unrelocated || ms > 120'000) {
                SSPC_LOG(Engine, Warning, "MOUSE_PROBE not installed: the pad vtable holds {:#x} / {:#x}", *a, *b);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_original_a = reinterpret_cast<Getter>(base + kOriginalA);
        g_original_b = reinterpret_cast<Getter>(base + kOriginalB);
        if (!SwapSlot(base + kVtableSlotA, base + kOriginalA, reinterpret_cast<u64>(&ProbeA)) ||
            !SwapSlot(base + kVtableSlotB, base + kOriginalB, reinterpret_cast<u64>(&ProbeB))) {
            SSPC_LOG(Engine, Error, "MOUSE_PROBE not installed: the slots changed or VirtualProtect failed ({})",
                     GetLastError());
            return;
        }
        SSPC_LOG(Engine, Warning, "DIAGNOSTIC (SSPC_MOUSE_PROBE=1): the pad's right-stick getters are wrapped");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            Report();
        }
    }).detach();
}

// SSPC_MOUSE_WATCH=1 (diagnostics): hardware watchpoints (DR0..DR3, read or write, 4 bytes) on the game's filtered
// right stick (pad object +0x14 x, +0x18 y, +0x1C magnitude) and its raw sticks (+0xD4) in every thread; each access
// is counted by the address after the instruction.
struct WatchHit {
    std::atomic<u64> rip{0};
    std::atomic<u64> count{0};
    u64 callers[4] = {}; // the return addresses of the first hit by the frame pointer chain (rbp), innermost first
};
constexpr u32 kWatchSlots = 512;
WatchHit g_watch_hits[kWatchSlots];
std::atomic<u64> g_watch_dropped{0};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (ep->ContextRecord->Dr6 & 0xF) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 rip = ep->ContextRecord->Rip;
    ep->ContextRecord->Dr6 = 0;
    for (u32 i = 0, h = static_cast<u32>((rip * 0x9E3779B97F4A7C15ull) >> 55); i < kWatchSlots; ++i) {
        WatchHit& w = g_watch_hits[(h + i) % kWatchSlots];
        u64 cur = w.rip.load(std::memory_order_acquire);
        if (cur == 0 && w.rip.compare_exchange_strong(cur, rip, std::memory_order_acq_rel)) {
            cur = rip;
        }
        if (cur == rip) {
            if (w.count.fetch_add(1, std::memory_order_relaxed) == 0) {
                u64 frame = ep->ContextRecord->Rbp;
                for (u64& c : w.callers) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (frame == 0 || (frame & 7) != 0 ||
                        VirtualQuery(reinterpret_cast<void*>(frame), &mbi, sizeof(mbi)) == 0 ||
                        mbi.State != MEM_COMMIT ||
                        frame + 16 > reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize) {
                        break;
                    }
                    c = reinterpret_cast<const u64*>(frame)[1];
                    frame = reinterpret_cast<const u64*>(frame)[0];
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    g_watch_dropped.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/// Sets the watchpoints in every thread of the process but this one that does not have them yet.
u32 ArmWatchpoints(u64 a, u64 b, u64 mag, u64 raw, std::vector<DWORD>& armed) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    u32 n = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId() ||
            std::ranges::find(armed, te.th32ThreadID) != armed.end()) {
            continue;
        }
        const HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                    te.th32ThreadID);
        if (t == nullptr) {
            continue;
        }
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = a;
                c.Dr1 = b;
                c.Dr2 = mag;
                c.Dr3 = raw;
                // L0..L3; RW = 11 (read or write), LEN = 11 (4 bytes) for each
                c.Dr7 = (c.Dr7 & ~0xFFFF00FFull) | 0x1 | 0x4 | 0x10 | 0x40 | (0xFFFFull << 16);
                if (SetThreadContext(t, &c)) {
                    armed.push_back(te.th32ThreadID);
                    ++n;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return n;
}

void InstallWatch(u64 base) {
    g_base = base;
    std::thread([base] {
        u64 pad = 0;
        while ((pad = Pad::GamePadObject()) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        AddVectoredExceptionHandler(1, &WatchHandler);
        std::vector<DWORD> armed;
        // SSPC_MOUSE_WATCH_AT="0x14,0x18,0x1c,0xd4": the 4 pad object offsets watched (the default: the filtered right
        // stick x / y / magnitude and the raw stick bytes)
        u64 at[4] = {0x14, 0x18, 0x1C, 0xD4};
        char list[128] = {};
        if (GetEnvironmentVariableA("SSPC_MOUSE_WATCH_AT", list, sizeof(list)) != 0) {
            char* p = list;
            for (u64& o : at) {
                o = std::strtoull(p, &p, 16);
                p += *p == ',' ? 1 : 0;
            }
        }
        SSPC_LOG(Engine, Warning, "DIAGNOSTIC (SSPC_MOUSE_WATCH=1): watchpoints on the pad object {:#x} at +{:#x} "
                                  "+{:#x} +{:#x} +{:#x}",
                 pad, at[0], at[1], at[2], at[3]);
        for (u32 tick = 0;; ++tick) {
            const u32 n = ArmWatchpoints(pad + at[0], pad + at[1], pad + at[2], pad + at[3], armed);
            if (n != 0) {
                SSPC_LOG(Engine, Info, "MOUSE_WATCH: {} more threads watched ({} in all)", n, armed.size());
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tick % 5 != 4) {
                continue;
            }
            std::string out;
            for (const WatchHit& w : g_watch_hits) {
                const u64 rip = w.rip.load(std::memory_order_acquire);
                if (rip != 0) {
                    out += rip >= base && rip < base + 0x4000000
                               ? fmt::format("\n  after eboot+{:#x}: {}", rip - base, w.count.load())
                               : fmt::format("\n  after {:#x} (not the eboot): {}", rip, w.count.load());
                    for (const u64 c : w.callers) {
                        if (c != 0) {
                            out += c >= base && c < base + 0x4000000 ? fmt::format(" <- eboot+{:#x}", c - base)
                                                                     : fmt::format(" <- {:#x}", c);
                        }
                    }
                }
            }
            SSPC_LOG(Engine, Info, "MOUSE_WATCH accesses (dropped {}):{}", g_watch_dropped.load(),
                     out.empty() ? " none yet" : out);
        }
    }).detach();
}

// The direct camera. The game's follow camera (update 0x14ddd0) reads the right stick through the pad's vtable +0x70
// (0x24d740: its own dead zone and gate from the filter's raw bytes), shapes it ([params+0x248..0x264]: a dead zone
// and a power curve) and hands it to its integrator 0x14d6f0 (camera, params, params, ?, float* angles out, float*
// velocities out, yaw input, pitch input): a yaw / pitch velocity with acceleration limits (0x14a810 / 0x14c270), then
// angle += (old + new velocity) / 2 * dt from the camera's angles at +0x330 (yaw, pitch; rad). An entry probe on the
// integrator adds the mouse to those angles before it runs: an angle per count, the stick at rest.
constexpr ProbeSite kIntegrateSite{{0x14d6f0, 0x593, 0x724cebb9e1f76f99ull}, 17};
constexpr u64 kCameraAngles = 0x330;  // float yaw, pitch
constexpr u64 kCameraLocks = 0x7c0;   // u32: bit 0 the yaw input is off, bit 1 the pitch input (cut-scenes, moves)
// float min, max in both params: the stick's limits (0x14a810 at 0x14c111: the narrower of the two). The second
// params' +0x210 ([-0.15, +0.20] rad on foot) narrows them only for the automatic camera (0x14c270, blended in by
// camera +0x498) - the stick goes past it (SSPC_MOUSE_CAMERA=observe on the stand, 10-24: a full stick down for 3 s
// reached -1.45 rad, up past +0.6)
constexpr u64 kPitchLimits = 0x1c0;
constexpr u64 kPitchLimitsAuto = 0x210;
constexpr u64 kOptions = 0xefacf8;    // the game's options: +0x6d5 / +0x6d6 the camera's x / y inversion (bytes)
// mouse right = the stick right = yaw input > 0 = the yaw falls; mouse up = the stick up = pitch input > 0 = the
// pitch falls (SSPC_MOUSE_CAMERA=observe on the stand, 10-24: a full stick for 0.3 s turned 0.27 rad, the stick of a
// slow mouse - 60 counts in 1 s - 0.002 rad)
constexpr double kYawPerCount = -0.001; // rad (1000 counts ~ 57 deg at sensitivity 1)
constexpr double kPitchPerCount = 0.001;

using IntegrateFn = SSPC_SYSV_ABI void (*)(u64 camera, u64 params, u64 params2, u64 arg3, float* angles,
                                           float* velocities, float yaw_input, float pitch_input);
IntegrateFn g_integrate = nullptr;
bool g_direct = false;

struct CameraStats {
    u64 frames = 0, moved = 0;
    double input_yaw = 0.0, input_pitch = 0.0;  // the stick inputs (observe)
    double turned_yaw = 0.0, turned_pitch = 0.0; // what the integrator turned on frames with a stick input
    double mouse_yaw = 0.0, mouse_pitch = 0.0;   // what the mouse turned (direct)
    float pitch = 0.0f, pitch_min = 0.0f, pitch_max = 0.0f; // the stick's limits
    float auto_min = 0.0f, auto_max = 0.0f;                // the automatic camera's
};
std::mutex g_stats_lock;
CameraStats g_stats;

float WrapAngle(float a) {
    return static_cast<float>(std::remainder(static_cast<double>(a), 2.0 * 3.14159265358979323846));
}

SSPC_SYSV_ABI void IntegrateProbe(u64 camera, u64 params, u64 params2, u64 arg3, float* angles, float* velocities,
                                  float yaw_input, float pitch_input) {
    auto* current = reinterpret_cast<float*>(camera + kCameraAngles);
    const u32 locks = *reinterpret_cast<const u32*>(camera + kCameraLocks);
    const auto* p1 = reinterpret_cast<const float*>(params + kPitchLimits);
    const auto* p2 = reinterpret_cast<const float*>(params2 + kPitchLimits);
    const auto* p3 = reinterpret_cast<const float*>(params2 + kPitchLimitsAuto);
    // (as the stick: the narrower of the two; crossed limits meet at the lower one)
    const float lo = std::max(p1[0], p2[0]), hi = std::max(lo, std::min(p1[1], p2[1]));
    double mouse_yaw = 0.0, mouse_pitch = 0.0;
    if (g_direct) {
        double dx = 0.0, dy = 0.0;
        Input::Player1().TakeCameraCounts(dx, dy);
        const auto* options = reinterpret_cast<const u8*>(g_base + kOptions);
        dx = options[0x6d5] != 0 ? -dx : dx;
        dy = options[0x6d6] != 0 ? -dy : dy;
        if ((locks & 1) == 0 && dx != 0.0) {
            mouse_yaw = dx * kYawPerCount;
            current[0] = WrapAngle(static_cast<float>(current[0] + mouse_yaw));
        }
        if ((locks & 2) == 0 && dy != 0.0) {
            const float before = current[1];
            current[1] = std::clamp(static_cast<float>(before + dy * kPitchPerCount), std::min(lo, before),
                                    std::max(hi, before));
            mouse_pitch = current[1] - before;
        }
    }
    const float yaw0 = current[0], pitch0 = current[1];
    g_integrate(camera, params, params2, arg3, angles, velocities, yaw_input, pitch_input);
    std::scoped_lock lock{g_stats_lock};
    CameraStats& s = g_stats;
    ++s.frames;
    if (yaw_input != 0.0f || pitch_input != 0.0f) {
        ++s.moved;
        s.input_yaw += yaw_input;
        s.input_pitch += pitch_input;
        s.turned_yaw += WrapAngle(angles[0] - yaw0);
        s.turned_pitch += angles[1] - pitch0;
    }
    s.mouse_yaw += mouse_yaw;
    s.mouse_pitch += mouse_pitch;
    s.pitch = angles[1];
    s.pitch_min = lo;
    s.pitch_max = hi;
    s.auto_min = p3[0];
    s.auto_max = p3[1];
}

void ReportCamera() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        CameraStats s;
        {
            std::scoped_lock lock{g_stats_lock};
            s = g_stats;
            g_stats = {};
        }
        if (s.frames == 0 || (s.moved == 0 && s.mouse_yaw == 0.0 && s.mouse_pitch == 0.0)) {
            continue;
        }
        SSPC_LOG(Engine, Info,
                 "mouse camera 1 s: {} frames; stick on {}: input yaw {:+.3f} pitch {:+.3f} -> turned yaw {:+.4f} "
                 "pitch {:+.4f} rad; mouse turned yaw {:+.4f} pitch {:+.4f} rad; pitch {:+.3f} in [{:+.3f}, {:+.3f}] "
                 "(automatic camera [{:+.3f}, {:+.3f}])",
                 s.frames, s.moved, s.input_yaw, s.input_pitch, s.turned_yaw, s.turned_pitch, s.mouse_yaw,
                 s.mouse_pitch, s.pitch, s.pitch_min, s.pitch_max, s.auto_min, s.auto_max);
    }
}

/// SSPC_MOUSE_CAMERA: unset or 1 the mouse turns the camera directly, 0 it moves the right stick (the PS4's way),
/// observe the stick does and the integrator's turns are logged (diagnostics).
void InstallDirectCamera(u64 base) {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_MOUSE_CAMERA", v, sizeof(v));
    const std::string mode = n > 0 && n < sizeof(v) ? v : "1";
    if (mode == "0") {
        SSPC_LOG(Engine, Info, "Mouse camera: the mouse moves the right stick (SSPC_MOUSE_CAMERA=0)");
        return;
    }
    std::string error;
    void* original = InstallEntryProbe(base, kIntegrateSite, reinterpret_cast<const void*>(&IntegrateProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Mouse camera not installed (the mouse moves the right stick): {}", error);
        return;
    }
    g_base = base;
    g_integrate = reinterpret_cast<IntegrateFn>(original);
    g_direct = mode != "observe";
    std::thread(&ReportCamera).detach();
    SSPC_LOG(Engine, Info, "Mouse camera: entry probe on the camera integrator 0x14d6f0 ({})",
             g_direct ? "the mouse turns the camera directly" : "observing the stick (SSPC_MOUSE_CAMERA=observe)");
}

} // namespace

void Install(u64 eboot_base) {
    InstallDirectCamera(eboot_base);
    char v[4] = {};
    if (GetEnvironmentVariableA("SSPC_MOUSE_PROBE", v, sizeof(v)) == 1 && v[0] == '1') {
        InstallProbe(eboot_base);
    }
    if (GetEnvironmentVariableA("SSPC_MOUSE_WATCH", v, sizeof(v)) == 1 && v[0] == '1') {
        InstallWatch(eboot_base);
    }
}

} // namespace SecondSon::Engine::MouseCamera
