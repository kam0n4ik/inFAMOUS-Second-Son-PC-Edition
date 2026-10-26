// SecondSonPC — R12 light census (see light_census.h).
#include "engine/render/light_census.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/r12/light_stats.h"

namespace SecondSon::Gpu::R12 {

bool LightCensusOn() {
    static const bool on = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_R12_LIGHT_CENSUS", v, sizeof(v)) == 1 && v[0] == '1';
    }();
    return on;
}

LightStats& Stats() {
    static LightStats s;
    return s;
}

namespace {
struct Writer {
    u64 n = 0, bytes = 0, size = 0;
};
std::mutex g_writers_mutex;
std::map<std::pair<u64, u64>, Writer> g_writers; ///< (shader, range start) -> dispatches, bytes, last size
} // namespace

void NoteWriter(u64 shader, u64 begin, u64 bytes) {
    const std::lock_guard lock(g_writers_mutex);
    auto it = g_writers.find({shader, begin});
    if (it == g_writers.end()) {
        if (g_writers.size() >= 4096) {
            return; // (bounded: the large writers are among the first seen)
        }
        it = g_writers.emplace(std::pair{shader, begin}, Writer{}).first;
    }
    Writer& w = it->second;
    ++w.n;
    w.bytes += bytes;
    w.size = bytes;
}

u64 DetailShader() {
    static const u64 shader = [] {
        char v[32] = {};
        return GetEnvironmentVariableA("SSPC_R12_WRITER_DETAIL", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    return shader;
}

u64 ReadersOf() {
    static const u64 address = [] {
        char v[32] = {};
        return GetEnvironmentVariableA("SSPC_R12_READERS_OF", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    return address;
}

void NoteDetail(const std::string& line) {
    static std::mutex m;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(m);
        if (seen.size() >= 96 || !seen.insert(line).second) {
            return;
        }
    }
    SSPC_LOG(Engine, Info, "R12 writer detail: {}", line);
}

std::string TopWriters(size_t top) {
    std::vector<std::pair<std::pair<u64, u64>, Writer>> v;
    {
        const std::lock_guard lock(g_writers_mutex);
        v.assign(g_writers.begin(), g_writers.end());
    }
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        out += fmt::format("\n    cs {:#x} writes {:#x}+{:#x}: {} dispatches, {} MiB", v[i].first.first,
                           v[i].first.second, v[i].second.size, v[i].second.n, v[i].second.bytes >> 20);
    }
    return out;
}

} // namespace SecondSon::Gpu::R12

namespace SecondSon::Engine::Render::LightCensus {

namespace {

// prologues checked in Ghidra (queries/prologue.py: whole position-independent instructions, nothing branches into
// them); hashes tools/native-reconstruction/function_hashes.py
constexpr ProbeSite kDataLightSite{{0x19a4d0, 0x48e, 0x99ef1c1099a38cd4ull}, 20};
constexpr ProbeSite kCodeLightSite{{0x19a960, 0x1cc, 0x99b170503e44fccbull}, 17};
constexpr ProbeSite kFlushSite{{0x16dcc0, 0x84, 0x4431e506ff677ad7ull}, 14};
constexpr ProbeSite kParticleSite{{0x16dd50, 0x48, 0xe2cc5fa4748b8767ull}, 15};

constexpr u32 kKinds = 17; ///< record +0x2c: 0..15, then "other"

using DataLightFn = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64);
using CodeLightFn = SSPC_SYSV_ABI void (*)(u64, u32, u32, u32, __m128, __m128, __m128, __m128, __m128);
using FlushFn = SSPC_SYSV_ABI s32 (*)(u64, s32, u64, u64);
using ParticleFn = SSPC_SYSV_ABI s32 (*)(u64, s32, u64, u64);

DataLightFn g_data = nullptr;
CodeLightFn g_code = nullptr;
FlushFn g_flush = nullptr;
ParticleFn g_particle = nullptr;

struct Counters {
    std::atomic<u64> data_calls{0}, data_kept{0}, data_shadowed{0}, data_animated{0};
    std::array<std::atomic<u64>, kKinds> data_kind{};
    std::atomic<u64> code_calls{0}, code_kept{0};
    std::atomic<u64> flushed{0}, flushes{0}, overflow{0};
    std::atomic<s64> peak{0}; ///< the highest list index a flush reached since the last report
    std::atomic<u64> particle{0}, particle_calls{0};
};
Counters g_n;

u32 BatchCount(u64 batch) {
    u32 n = 0;
    std::memcpy(&n, reinterpret_cast<const void*>(batch + 0xa0), sizeof(n));
    return n;
}

SSPC_SYSV_ABI void DataLightProbe(u64 batch, u64 b, u64 c, u64 d) {
    // (batch, ?, the light's owner, the light instance): 0x19a4d0 appends one record to the batch at +0x90 (count
    // +0xa0) when 0x16c430 keeps the light; +0x21 & 0x40 of the second argument: an animated record at +0xa8 too
    const u32 before = BatchCount(batch);
    g_data(batch, b, c, d);
    g_n.data_calls.fetch_add(1, std::memory_order_relaxed);
    const u32 after = BatchCount(batch);
    if (after == before || after == 0) {
        return;
    }
    g_n.data_kept.fetch_add(1, std::memory_order_relaxed);
    u64 records = 0;
    std::memcpy(&records, reinterpret_cast<const void*>(batch + 0x90), sizeof(records));
    const u64 rec = records + u64{after - 1} * 0xb0;
    u32 kind = 0;
    s32 shadow = -1;
    std::memcpy(&kind, reinterpret_cast<const void*>(rec + 0x2c), sizeof(kind));
    std::memcpy(&shadow, reinterpret_cast<const void*>(rec + 0x4c), sizeof(shadow));
    g_n.data_kind[kind < kKinds - 1 ? kind : kKinds - 1].fetch_add(1, std::memory_order_relaxed);
    if (shadow >= 0) {
        g_n.data_shadowed.fetch_add(1, std::memory_order_relaxed);
    }
    if ((*reinterpret_cast<const u8*>(b + 0x21) & 0x40) != 0) {
        g_n.data_animated.fetch_add(1, std::memory_order_relaxed);
    }
}

SSPC_SYSV_ABI void CodeLightProbe(u64 batch, u32 p2, u32 p3, u32 p4, __m128 x0, __m128 x1, __m128 x2, __m128 x3,
                                  __m128 x4) {
    // (batch, value at +0x48, diffuse, specular; xmm0 position, xmm1 colour, xmm2 range, xmm4 +0x28): the vector
    // arguments are passed on unchanged in xmm0..xmm4
    const u32 before = BatchCount(batch);
    g_code(batch, p2, p3, p4, x0, x1, x2, x3, x4);
    g_n.code_calls.fetch_add(1, std::memory_order_relaxed);
    if (const u32 after = BatchCount(batch); after != before && after != 0) {
        g_n.code_kept.fetch_add(1, std::memory_order_relaxed);
    }
}

SSPC_SYSV_ABI s32 FlushProbe(u64 list, s32 n, u64 records, u64 records2) {
    const s32 start = g_flush(list, n, records, records2);
    s32 cap = 0;
    std::memcpy(&cap, reinterpret_cast<const void*>(list + 0x158), sizeof(cap));
    g_n.flushes.fetch_add(1, std::memory_order_relaxed);
    g_n.flushed.fetch_add(n > 0 ? static_cast<u64>(n) : 0, std::memory_order_relaxed);
    if (start + n > cap) {
        g_n.overflow.fetch_add(static_cast<u64>(start + n - std::max(start, cap)), std::memory_order_relaxed);
    }
    s64 top = start + n, seen = g_n.peak.load(std::memory_order_relaxed);
    while (top > seen && !g_n.peak.compare_exchange_weak(seen, top, std::memory_order_relaxed)) {
    }
    return start;
}

SSPC_SYSV_ABI s32 ParticleProbe(u64 list, s32 n, u64 out1, u64 out2) {
    const s32 fit = g_particle(list, n, out1, out2);
    g_n.particle_calls.fetch_add(1, std::memory_order_relaxed);
    g_n.particle.fetch_add(fit > 0 ? static_cast<u64>(fit) : 0, std::memory_order_relaxed);
    return fit;
}

void Reporter() {
    struct Snap {
        u64 data_calls, data_kept, data_shadowed, data_animated, code_calls, code_kept, flushed, overflow, particle;
        std::array<u64, kKinds> kind;
        u64 atlas, cascade, occ_q, occ_d;
    };
    const auto take = [] {
        Snap s{};
        s.data_calls = g_n.data_calls.load(std::memory_order_relaxed);
        s.data_kept = g_n.data_kept.load(std::memory_order_relaxed);
        s.data_shadowed = g_n.data_shadowed.load(std::memory_order_relaxed);
        s.data_animated = g_n.data_animated.load(std::memory_order_relaxed);
        s.code_calls = g_n.code_calls.load(std::memory_order_relaxed);
        s.code_kept = g_n.code_kept.load(std::memory_order_relaxed);
        s.flushed = g_n.flushed.load(std::memory_order_relaxed);
        s.overflow = g_n.overflow.load(std::memory_order_relaxed);
        s.particle = g_n.particle.load(std::memory_order_relaxed);
        for (u32 k = 0; k < kKinds; ++k) {
            s.kind[k] = g_n.data_kind[k].load(std::memory_order_relaxed);
        }
        auto& gs = Gpu::R12::Stats();
        s.atlas = gs.atlas_draws.load(std::memory_order_relaxed);
        s.cascade = gs.cascade_draws.load(std::memory_order_relaxed);
        s.occ_q = gs.occ_queries.load(std::memory_order_relaxed);
        s.occ_d = gs.occ_draws.load(std::memory_order_relaxed);
        return s;
    };
    Snap last = take();
    u64 last_flip = Gpu::VideoOut::NextFlipId();
    for (u32 report = 1;; ++report) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const u64 flip = Gpu::VideoOut::NextFlipId();
        const Snap now = take();
        const double fr = static_cast<double>(std::max<u64>(1, flip - last_flip));
        const auto pf = [&](u64 a, u64 b) { return static_cast<double>(a - b) / fr; };
        std::string kinds;
        for (u32 k = 0; k < kKinds; ++k) {
            if (now.kind[k] != last.kind[k]) {
                kinds += fmt::format(" {}:{:.1f}", k == kKinds - 1 ? std::string("other") : std::to_string(k),
                                     pf(now.kind[k], last.kind[k]));
            }
        }
        const s64 peak = g_n.peak.exchange(0, std::memory_order_relaxed);
        SSPC_LOG(Engine, Info,
                 "R12 light census/10s ({:.0f} frames), per frame: data lights {:.1f} tested, {:.1f} kept (by kind{}), "
                 "{:.1f} with a shadow slot, {:.1f} animated; code point lights {:.1f} of {:.1f}; flushed to the list "
                 "{:.1f} (peak index {}, over capacity {:.1f}); particle lights {:.1f}; spot atlas depth draws {:.1f}, "
                 "other depth-only draws {:.1f}; glow occlusion queries {:.1f} ({:.1f} draws)",
                 fr, pf(now.data_calls, last.data_calls), pf(now.data_kept, last.data_kept), kinds,
                 pf(now.data_shadowed, last.data_shadowed), pf(now.data_animated, last.data_animated),
                 pf(now.code_kept, last.code_kept), pf(now.code_calls, last.code_calls), pf(now.flushed, last.flushed),
                 peak, pf(now.overflow, last.overflow), pf(now.particle, last.particle), pf(now.atlas, last.atlas),
                 pf(now.cascade, last.cascade), pf(now.occ_q, last.occ_q), pf(now.occ_d, last.occ_d));
        if (report % 6 == 0) {
            SSPC_LOG(Engine, Info, "R12 light census: dispatches' written buffer ranges >= 256 KiB since the start, by "
                                   "bytes:{}", Gpu::R12::TopWriters(16));
        }
        last = now;
        last_flip = flip;
    }
}

} // namespace

bool InstallFromEnvironment(u64 eboot_base) {
    if (!Gpu::R12::LightCensusOn()) {
        return true;
    }
    std::string error;
    for (const ProbeSite* s : {&kDataLightSite, &kCodeLightSite, &kFlushSite, &kParticleSite}) {
        if (!VerifyGuestFunction(eboot_base, s->function, error)) {
            SSPC_LOG(Engine, Error, "R12 light census not installed: {}", error);
            return false;
        }
    }
    const auto install = [&](const ProbeSite& site, const void* target) {
        void* original = InstallEntryProbe(eboot_base, site, target, error);
        if (original == nullptr) {
            SSPC_LOG(Engine, Critical, "R12 light census: {}", error);
            std::abort();
        }
        return original;
    };
    g_data = reinterpret_cast<DataLightFn>(install(kDataLightSite, reinterpret_cast<const void*>(&DataLightProbe)));
    g_code = reinterpret_cast<CodeLightFn>(install(kCodeLightSite, reinterpret_cast<const void*>(&CodeLightProbe)));
    g_flush = reinterpret_cast<FlushFn>(install(kFlushSite, reinterpret_cast<const void*>(&FlushProbe)));
    g_particle = reinterpret_cast<ParticleFn>(install(kParticleSite, reinterpret_cast<const void*>(&ParticleProbe)));
    SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_R12_LIGHT_CENSUS=1 - entry probes on the light list's producers 0x19a4d0, "
                              "0x19a960, 0x16dcc0, 0x16dd50 (read-only); the GPU counts atlas draws and glow queries");
    std::thread(Reporter).detach();
    return true;
}

} // namespace SecondSon::Engine::Render::LightCensus
