// SecondSonPC R10 — the input-to-present latency probe. See latency.h.
#include "runtime/gpu/r10/latency.h"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::R10::Latency {

namespace {

struct InputRow {
    u64 tsc;
    u32 kind;
    float dx, dy;
};
struct PresentRow {
    u64 tsc, flip;
    float change; ///< mean absolute change of the grid's channels against the frame presented before (-1: none before)
};

std::mutex g_mutex;
std::vector<InputRow> g_inputs;
std::vector<PresentRow> g_presents;
std::array<u8, kGrid * 8> g_prev{};
u32 g_prev_bpp = 0;
bool g_stopped = false;

} // namespace

bool On() {
    static const bool on = [] {
        char v[4] = {};
        const bool o = GetEnvironmentVariableA("SSPC_R10_LATENCY", v, sizeof(v)) == 1 && v[0] == '1';
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_R10_LATENCY=1 - scripted input and a {}x{} texel grid of every "
                                   "presented frame are recorded (input-to-present latency, logs/r10_latency.csv)",
                     kGridW, kGridH);
            g_inputs.reserve(1 << 14);
            g_presents.reserve(1 << 16);
        }
        return o;
    }();
    return on;
}

void Input(u32 kind, float dx, float dy) {
    if (!On()) {
        return;
    }
    const u64 t = __rdtsc();
    StallTrace::Instant(StallTrace::Ev::InputEvent, kind, static_cast<u32>(std::abs(dx) + std::abs(dy)));
    std::scoped_lock lock{g_mutex};
    if (!g_stopped) {
        g_inputs.push_back({t, kind, dx, dy});
    }
}

void Present(u64 flip_id, u64 present_tsc, const u8* texels, u32 bpp) {
    if (!On() || (bpp != 4 && bpp != 8)) {
        return;
    }
    float change = -1.0f;
    std::scoped_lock lock{g_mutex};
    if (g_stopped) {
        return;
    }
    if (texels != nullptr) {
        if (g_prev_bpp == bpp) {
            // per channel: bytes of an 8-bit format, the high bytes of a 16-bit one (little endian: odd bytes)
            u64 sum = 0;
            u32 n = 0;
            for (u32 i = 0; i < kGrid * bpp; i += bpp == 4 ? 1 : 2) {
                const u32 k = bpp == 4 ? i : i + 1;
                if (bpp == 4 && (i & 3) == 3) {
                    continue; // (alpha)
                }
                if (bpp == 8 && (i & 7) == 6) {
                    continue;
                }
                sum += static_cast<u32>(std::abs(static_cast<int>(texels[k]) - static_cast<int>(g_prev[k])));
                ++n;
            }
            change = n != 0 ? static_cast<float>(sum) / static_cast<float>(n) : -1.0f;
        }
        std::memcpy(g_prev.data(), texels, size_t{kGrid} * bpp);
        g_prev_bpp = bpp;
    }
    g_presents.push_back({present_tsc, flip_id, change});
}

void Stop(const std::filesystem::path& dir) {
    if (!On()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (g_stopped) {
        return;
    }
    g_stopped = true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream o(dir / "r10_latency.csv", std::ios::trunc);
    o << fmt::format("# tsc_hz {}\n", Kernel::Clock::TscFrequency());
    o << "type,tsc,flip_or_kind,change,dx,dy\n";
    for (const InputRow& r : g_inputs) {
        o << fmt::format("input,{},{},,{:.1f},{:.1f}\n", r.tsc, r.kind, r.dx, r.dy);
    }
    for (const PresentRow& r : g_presents) {
        o << fmt::format("present,{},{},{:.3f},,\n", r.tsc, r.flip, r.change);
    }
    SSPC_LOG(GPU, Info, "R10 latency probe: {} inputs, {} presents -> {}", g_inputs.size(), g_presents.size(),
             (dir / "r10_latency.csv").string());
}

} // namespace SecondSon::Gpu::R10::Latency
