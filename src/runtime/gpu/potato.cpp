// SecondSonPC — potato mode (see potato.h).
#include "runtime/gpu/potato.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <windows.h> // GetEnvironmentVariableA

#include "base/log.h"
#include "engine/render/draw_runs.h"
#include "runtime/gpu/command_processor.h"

namespace SecondSon::Gpu::Potato {

namespace {

constexpr u32 kDbDepthView = 0xA002, kDbZInfo = 0xA010, kDbZWriteBase = 0xA014, kDbDepthSize = 0xA016;
constexpr u32 kCbTargetMask = 0xA08E, kCbColor0Info = 0xA31C;
constexpr u32 kViewport0 = 0xA10F; ///< XSCALE, XOFFSET, YSCALE, YOFFSET

std::atomic<u32> g_level{0};

// the command processor's thread only
u32 g_sun_runs = 0, g_sun_runs_last = 0; ///< runs of draws into the sun's shadow map: this frame's, the frame before's
bool g_in_sun = false;                   ///< the draw before went to it

/// SSPC_POTATO=0..3: the stand's level, over the menu's (-1: not set)
int EnvLevel() {
    static const int v = [] {
        char e[8] = {};
        return GetEnvironmentVariableA("SSPC_POTATO", e, sizeof(e)) > 0 ? std::clamp(std::atoi(e), 0, 4) : -1;
    }();
    return v;
}

bool CensusOn() {
    static const bool on = [] {
        char e[8] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_POTATO_CENSUS", e, sizeof(e)) == 1 && e[0] == '1';
    }();
    return on;
}

float F(const Cp::RegisterFile& r, u32 reg) {
    return std::bit_cast<float>(r.Get(reg));
}

// ---- SSPC_DIAG_POTATO_CENSUS: depth-only draws by target, slice and viewport

struct CensusEntry {
    u64 zbase = 0;
    u32 width = 0, height = 0, slice = 0;
    float xoff = 0, yoff = 0, xscale = 0, yscale = 0;
    u64 draws = 0, instances = 0, first = 0;
    u64 by_tag[4] = {};
    u64 by_indices[6] = {}; ///< < 64, < 256, < 1024, < 4096, < 16384, more (indices x instances)
    s32 pass = 0;
};

std::mutex g_census_mu;
std::unordered_map<u64, CensusEntry> g_census;
u64 g_census_draws = 0, g_census_order = 0;

void Census(const Cp::RegisterFile& regs, const Cp::DrawDescriptor& d, bool depth_only, u32 width, u32 height) {
    const std::scoped_lock lock{g_census_mu};
    ++g_census_draws;
    if (depth_only) {
        const u64 zbase = u64{regs.Get(kDbZWriteBase)} << 8;
        const u32 slice = regs.Get(kDbDepthView) & 0x7FF;
        const float xoff = F(regs, kViewport0 + 1), yoff = F(regs, kViewport0 + 3);
        const u64 key = (zbase ^ (u64{slice} << 52) ^ (u64{std::bit_cast<u32>(xoff)} * 0x9E3779B97F4A7C15ull) ^
                         (u64{std::bit_cast<u32>(yoff)} << 20));
        auto [it, fresh] = g_census.try_emplace(key);
        CensusEntry& e = it->second;
        if (fresh) {
            e = {.zbase = zbase, .width = width, .height = height, .slice = slice, .xoff = xoff, .yoff = yoff,
                 .xscale = F(regs, kViewport0), .yscale = F(regs, kViewport0 + 2), .first = g_census_order++};
        }
        ++e.draws;
        const u64 inst = std::max<u32>(d.instance_count, 1);
        e.instances += inst;
        ++e.by_tag[d.engine_run & 3];
        const u64 n = u64{d.index_count} * inst;
        ++e.by_indices[n < 64 ? 0 : n < 256 ? 1 : n < 1024 ? 2 : n < 4096 ? 3 : n < 16384 ? 4 : 5];
        e.pass = d.engine_pass;
    }
    if (g_census_draws % 3'000'000 != 0) {
        return;
    }
    std::vector<const CensusEntry*> v;
    u64 depth = 0;
    for (const auto& [k, e] : g_census) {
        v.push_back(&e);
        depth += e.draws;
    }
    std::ranges::sort(v, [](const CensusEntry* a, const CensusEntry* b) { return a->draws > b->draws; });
    SSPC_LOG(GPU, Info,
             "potato census: {} draws, {} depth-only in {} target/slice/viewport groups (top 40: draws share, first "
             "seen order, target, viewport offset/scale, by tag none/mesh/shadow/gbuffer, by indices x instances "
             "<64/<256/<1K/<4K/<16K/more, instances, last engine pass)",
             g_census_draws, depth, v.size());
    for (size_t i = 0; i < v.size() && i < 40; ++i) {
        const CensusEntry& e = *v[i];
        SSPC_LOG(GPU, Info,
                 "potato census:  {:5.1f} % order {:4} z {:#012x} {}x{} slice {} vp off {:.0f},{:.0f} scale "
                 "{:.0f},{:.0f}; tags {}/{}/{}/{}; idx {}/{}/{}/{}/{}/{}; inst {}; pass {}",
                 100.0 * static_cast<double>(e.draws) / static_cast<double>(g_census_draws), e.first, e.zbase, e.width,
                 e.height, e.slice, e.xoff, e.yoff, e.xscale, e.yscale, e.by_tag[0], e.by_tag[1], e.by_tag[2],
                 e.by_tag[3], e.by_indices[0], e.by_indices[1], e.by_indices[2], e.by_indices[3], e.by_indices[4],
                 e.by_indices[5], e.instances, e.pass);
    }
    g_census.clear();
    g_census_draws = 0;
    g_census_order = 0;
}

} // namespace

void SetLevel(std::string_view choice) {
    const u32 level = choice == "1" ? 1 : choice == "2" ? 2 : choice == "3" ? 3 : choice == "4" ? 4 : 0;
    const int env = EnvLevel();
    g_level.store(env >= 0 ? static_cast<u32>(env) : level, std::memory_order_relaxed);
    SSPC_LOG(GPU, Info, "potato mode: level {}{}", g_level.load(std::memory_order_relaxed),
             env >= 0 ? " (SSPC_POTATO)" : "");
}

u32 Level() {
    const int env = EnvLevel();
    return env >= 0 ? static_cast<u32>(env) : g_level.load(std::memory_order_relaxed);
}

bool Drop(const Cp::RegisterFile& regs, const Cp::DrawDescriptor& d) {
    const u32 level = Level();
    const bool census = CensusOn();
    if (level == 0 && !census) {
        return false;
    }
    // no colour: CB_TARGET_MASK 0 or CB_COLOR0_INFO.FORMAT invalid, and a depth target; DB_DEPTH_SIZE: PITCH_TILE_MAX
    // = width / 8 - 1, HEIGHT_TILE_MAX = height / 8 - 1
    const bool depth_only = (regs.Get(kCbTargetMask) == 0 || ((regs.Get(kCbColor0Info) >> 2) & 0x1F) == 0) &&
                            (regs.Get(kDbZInfo) & 3) != 0;
    const u32 size = regs.Get(kDbDepthSize);
    const u32 width = ((size & 0x7FF) + 1) * 8, height = (((size >> 11) & 0x3FFFFF) + 1) * 8;
    if (census) {
        Census(regs, d, depth_only, width, height);
    }
    // the sun's shadow map (2048x2048, depth only): its cascades are drawn into it one after another, each a run of
    // draws with other draws between them - the frame's n-th run is its n-th cascade (near to far)
    const bool sun = depth_only && width == 2048 && height == 2048;
    if (sun && !g_in_sun) {
        ++g_sun_runs;
    }
    g_in_sun = sun;
    if (level == 0) {
        return false;
    }
    if (!depth_only) {
        // level 4: the world's small objects themselves - the engine's mesh draws (tagged: G-buffer and the other
        // mesh passes; no full-screen pass is tagged) under 256 indices in all their instances. The main view has no
        // depth pre-pass (no 1920-wide depth-only target), so nothing else needs the dropped draw.
        const auto tag = static_cast<Engine::Render::DrawRuns::Source>(d.engine_run);
        return level >= 4 && !d.indirect &&
               (tag == Engine::Render::DrawRuns::Source::Mesh || tag == Engine::Render::DrawRuns::Source::GBuffer) &&
               u64{d.index_count} * std::max<u32>(d.instance_count, 1) < 256;
    }
    if (width == 1024) {
        return true; // level 1: the spot light shadow atlases
    }
    if (!sun || level < 2) {
        return false;
    }
    // level 2: the farthest cascade (the last run of the frame before: the cascades are the same every frame)
    if (g_sun_runs_last > 1 && g_sun_runs >= g_sun_runs_last) {
        return true;
    }
    // level 3: the shadows of small objects - fewer than 256 indices in all their instances (~85 triangles: debris,
    // small props, foliage cards); an indirect draw's count is not known here
    return level >= 3 && !d.indirect && u64{d.index_count} * std::max<u32>(d.instance_count, 1) < 256;
}

void NewFrame() {
    g_sun_runs_last = g_sun_runs;
    g_sun_runs = 0;
    g_in_sun = false;
}

} // namespace SecondSon::Gpu::Potato
