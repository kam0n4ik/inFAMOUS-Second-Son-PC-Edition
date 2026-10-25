// SecondSonPC — the game's own post-processing effects as PC settings (see post_effects.h).
#include "engine/render/post_effects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render::PostFx {

namespace {

// --- the probed functions (eboot.bin of CUSA00004; FNV-1a 64 of the body, tools/native-reconstruction) --------------
// RenderOverridesUpdate(overrides 0xe87aa0): the frame's post-processing parameters - the environment's base, the
// active overrides lerped over it (0x1b9220), the result copied to overrides + 0x90. Prologue push rbp / mov rbp,rsp /
// push r15..rbx / sub rsp,0x178 (20 bytes, position-independent, no branch into it; ghidra/queries/prologue.py).
constexpr ProbeSite kBlendSite{{0x1ba7c0, 0x3fa, 0xf7df752bdd57a0ebull}, 20};
// MotionBlurPass(scene targets 0xe682b0, context, camera): pass 30 after the depth blur 0x193300 (its only caller
// 0x193e30). Its whole body is under `camera+0x694 == 0 && !0xe86e68 && exposure > 0`; nothing of it outlives the
// call but the GPU commands it records and the render state it leaves (the same as when the game skips it).
constexpr ProbeSite kMotionBlurSite{{0x193810, 0x4e3, 0x5ec53c58982d6c41ull}, 20};
// Functions whose behaviour the probes assume (the readers of the values written after the blend).
constexpr GuestFunction kAssumed[] = {
    {0x193e30, 0x179, 0x002719ba689d09e2ull}, // pass 30: depth blur, motion blur, bright pass (the probe's caller)
    {0x195da0, 0x041, 0x5764a6403dee04b8ull}, // the film grain pass: FX_FILMGRAIN when film_grain > 0
    {0x455180, 0x650, 0xc91f190d1d4d9ebfull}, // the bokeh DOF: when !0x7dfc300 && DOF_RADIUS > 0
};

// The blended block (globals of the eboot's data segment).
constexpr u64 kExposure = 0xe87c18;     // float MOTION_BLUR_EXPOSURE
constexpr u64 kFilmGrain = 0xe87c88;    // float film_grain
constexpr u64 kDofRadius = 0xe87bf4;    // float DOF_RADIUS
constexpr u64 kDepthBlur = 0xe87c14;    // float DEPTH_BLUR_STRENGTH (0x193300 runs when != 0)
constexpr u64 kBloomScale = 0xe87c1c;   // float BLOOM_SCALE, BLOOM_SCALE_SKY (the bright pass 0x1a8830)
constexpr u64 kVignette = 0xe87c40;     // float VIGNETTE_SCALE_X, _Y, _WIDTH (the composite 0x1a67d0)
constexpr u64 kDofDisabled = 0x7dfc300; // u8: the engine's DOF-off flag (read only by the DOF functions)
constexpr u64 kMotionBlurOff = 0xe86e68; // u8: the engine's motion-blur-off flag (also gates the velocity pass)
constexpr u64 kCameraSkip = 0x694;       // s32 of MotionBlurPass's camera: != 0 skips it
// The motion blur's FX (MotionBlurPass) and the velocity pass's camera FX (0x1936d0), for the log.
constexpr u64 kVelocityFx = 0x7d4c230;
constexpr u64 kMotionBlurFx[] = {0x7d4c238, 0x7d4c240, 0x7d4c248, 0x7d4c258, 0x7d4c260, 0x7d4c1c0};

using BlendFn = SSPC_SYSV_ABI void (*)(u64 overrides);
using MotionBlurFn = SSPC_SYSV_ABI void (*)(u64 targets, u64 context, u64 camera);
BlendFn g_blend = nullptr;
MotionBlurFn g_motion_blur = nullptr;
u64 g_base = 0;

std::atomic<bool> g_mb_on{true};
std::atomic<u32> g_mb_strength{100};
std::atomic<bool> g_grain_on{true};
std::atomic<bool> g_dof_on{true};
bool g_dof_written = false; // the flag is ours (the game never writes it): cleared again when DOF comes back on
// SSPC_POSTFX_DIAG
bool g_diag_bloom = false, g_diag_vignette = false, g_diag_depth_blur = false;
bool g_diag_mb_flip = false, g_diag_dof_flip = false; // motion blur skipped on every other MotionBlurPass call (A/B of neighbouring frames)
bool g_diag_mb_cycle = false;  // MotionBlurPass calls cycle: skipped / the set strength / 100 % (neighbouring frames)
bool g_diag_dof_force = false; // the DOF radius set to 1.0 every frame
float g_raw_exposure = 0.0f;  // the blended exposure before the strength (mbcycle)
std::atomic<u64> g_mb_calls{0};
// which settings an environment variable fixed (the menu does not change those)
bool g_env_mb = false, g_env_strength = false, g_env_grain = false, g_env_dof = false;

struct Counters {
    std::atomic<u64> frames{0};
    std::atomic<u64> exposure_frames{0}; // frames with a blended exposure > 0
    std::atomic<u32> exposure_max{0};    // float bits
    std::atomic<u64> mb_run{0}, mb_skipped{0}, mb_idle{0}; // MotionBlurPass calls: blur recorded / ours / game's
    std::atomic<u64> grain_frames{0}, grain_zeroed{0};
    std::atomic<u64> dof_frames{0}, depth_blur_frames{0};
    std::atomic<u32> grain_max{0}, dof_max{0}, depth_blur_max{0}, bloom_last{0}, vignette_last{0};
};
Counters g_count;

template <typename T>
T* At(u64 rva) {
    return reinterpret_cast<T*>(g_base + rva);
}

std::string Env(const char* name) {
    char v[128] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

bool Readable(u64 at, size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (at == 0 || VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return at + size <= reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
}

void Max(std::atomic<u32>& slot, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, 4);
    u32 old = slot.load(std::memory_order_relaxed);
    float o = 0;
    std::memcpy(&o, &old, 4);
    while (v > o && !slot.compare_exchange_weak(old, bits, std::memory_order_relaxed)) {
        std::memcpy(&o, &old, 4);
    }
}

void Store(std::atomic<u32>& slot, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, 4);
    slot.store(bits, std::memory_order_relaxed);
}

float Load(std::atomic<u32>& slot, bool reset) {
    const u32 bits = reset ? slot.exchange(0, std::memory_order_relaxed) : slot.load(std::memory_order_relaxed);
    float v = 0;
    std::memcpy(&v, &bits, 4);
    return v;
}

SSPC_SYSV_ABI void BlendProbe(u64 overrides) {
    g_blend(overrides);
    g_count.frames.fetch_add(1, std::memory_order_relaxed);
    float* exposure = At<float>(kExposure);
    if (*exposure > 0.0f) {
        g_count.exposure_frames.fetch_add(1, std::memory_order_relaxed);
        Max(g_count.exposure_max, *exposure);
        g_raw_exposure = *exposure;
        *exposure = ScaleExposure(*exposure, g_mb_strength.load(std::memory_order_relaxed));
    }
    float* grain = At<float>(kFilmGrain);
    if (*grain > 0.0f) {
        g_count.grain_frames.fetch_add(1, std::memory_order_relaxed);
        Max(g_count.grain_max, *grain);
        if (!g_grain_on.load(std::memory_order_relaxed)) {
            *grain = 0.0f; // the film grain pass 0x195da0 draws nothing at 0 (its only reader)
            g_count.grain_zeroed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const float dof = *At<float>(kDofRadius);
    if (dof > 0.0f) {
        g_count.dof_frames.fetch_add(1, std::memory_order_relaxed);
        Max(g_count.dof_max, dof);
    }
    if (g_diag_dof_force) {
        *At<float>(kDofRadius) = 1.0f; // (a strong, visible bokeh: does the engine's flag turn it off)
    }
    if (g_diag_dof_flip) {
        *At<u8>(kDofDisabled) = (g_count.frames.load(std::memory_order_relaxed) & 1) != 0 ? 1 : 0;
        g_dof_written = true;
    } else if (!g_dof_on.load(std::memory_order_relaxed)) {
        *At<u8>(kDofDisabled) = 1;
        g_dof_written = true;
    } else if (g_dof_written) {
        *At<u8>(kDofDisabled) = 0;
        g_dof_written = false;
    }
    float* depth_blur = At<float>(kDepthBlur);
    if (*depth_blur != 0.0f) {
        g_count.depth_blur_frames.fetch_add(1, std::memory_order_relaxed);
        Max(g_count.depth_blur_max, *depth_blur);
        if (g_diag_depth_blur) {
            *depth_blur = 0.0f;
        }
    }
    float* bloom = At<float>(kBloomScale);
    Store(g_count.bloom_last, bloom[0]);
    if (g_diag_bloom) {
        bloom[0] = bloom[1] = 0.0f;
    }
    float* vignette = At<float>(kVignette);
    Store(g_count.vignette_last, vignette[0]);
    if (g_diag_vignette) {
        vignette[0] = vignette[1] = 0.0f;
    }
}

SSPC_SYSV_ABI void MotionBlurProbe(u64 targets, u64 context, u64 camera) {
    const bool would_run = camera != 0 && *reinterpret_cast<const s32*>(camera + kCameraSkip) == 0 &&
                           *At<u8>(kMotionBlurOff) == 0 && *At<float>(kExposure) > 0.0f;
    if (would_run && g_diag_mb_cycle && g_mb_on.load(std::memory_order_relaxed)) {
        const u64 k = g_mb_calls.fetch_add(1, std::memory_order_relaxed) % 3;
        if (k == 0) {
            g_count.mb_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_count.mb_run.fetch_add(1, std::memory_order_relaxed);
        float* exposure = At<float>(kExposure);
        const float set = *exposure;
        if (k == 2) {
            *exposure = g_raw_exposure; // the game's own length for this frame's blur only (pass 29 already ran)
        }
        g_motion_blur(targets, context, camera);
        *exposure = set;
        return;
    }
    if (!would_run) {
        g_count.mb_idle.fetch_add(1, std::memory_order_relaxed);
    } else if (!g_mb_on.load(std::memory_order_relaxed) ||
               (g_diag_mb_flip && (g_mb_calls.fetch_add(1, std::memory_order_relaxed) & 1) != 0)) {
        g_count.mb_skipped.fetch_add(1, std::memory_order_relaxed);
        return; // the game's own path at exposure 0: nothing recorded
    } else {
        g_count.mb_run.fetch_add(1, std::memory_order_relaxed);
    }
    g_motion_blur(targets, context, camera);
}

/// FX kind of an effect object: its vtable's slot 8 is `mov eax, kind; ret` (0x2dead0: CFxMotionBlur 0x3ba).
std::string FxKind(u64 global) {
    const u64 fx = *At<u64>(global);
    if (!Readable(fx, 0x120)) {
        return "-";
    }
    const u64 vtbl = *reinterpret_cast<const u64*>(fx);
    if (!Readable(vtbl, 0x48)) {
        return "?";
    }
    const u64 get_kind = *reinterpret_cast<const u64*>(vtbl + 0x40);
    const u8* code = reinterpret_cast<const u8*>(get_kind);
    if (!Readable(get_kind, 6) || code[0] != 0xB8 || code[5] != 0xC3) {
        return "?";
    }
    u32 kind = 0;
    std::memcpy(&kind, code + 1, 4);
    const s16 c118 = *reinterpret_cast<const s16*>(fx + 0x118), c11a = *reinterpret_cast<const s16*>(fx + 0x11a);
    return fmt::format("{:#x}(c118={},c11a={})", kind, c118, c11a);
}

void Reporter(u32 seconds) {
    bool fx_logged = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const u64 frames = g_count.frames.exchange(0, std::memory_order_relaxed);
        if (!fx_logged && frames != 0) {
            std::string kinds;
            for (const u64 g : kMotionBlurFx) {
                kinds += fmt::format(" {:#x}:{}", g, FxKind(g));
            }
            SSPC_LOG(Engine, Info, "PostFx: velocity FX {:#x}:{}; motion blur FX{}", kVelocityFx, FxKind(kVelocityFx),
                     kinds);
            fx_logged = true;
        }
        SSPC_LOG(Engine, Info,
                 "PostFx stats: {} frames; exposure>0 {} (max {:.4f}, x{}%); motion blur run {} / skipped {} / "
                 "idle {}; film grain>0 {} (max {:.3f}, zeroed {}); DOF radius>0 {} (max {:.3f}, {}); depth blur {} "
                 "(max {:.3f}); bloom scale {:.3f}, vignette x {:.3f}",
                 frames, g_count.exposure_frames.exchange(0), Load(g_count.exposure_max, true),
                 g_mb_strength.load(), g_count.mb_run.exchange(0), g_count.mb_skipped.exchange(0),
                 g_count.mb_idle.exchange(0), g_count.grain_frames.exchange(0), Load(g_count.grain_max, true),
                 g_count.grain_zeroed.exchange(0), g_count.dof_frames.exchange(0), Load(g_count.dof_max, true),
                 g_dof_on.load() ? "on" : "off", g_count.depth_blur_frames.exchange(0),
                 Load(g_count.depth_blur_max, true), Load(g_count.bloom_last, false),
                 Load(g_count.vignette_last, false));
    }
}

bool EnvBool(const char* name, bool& out) {
    const std::string v = Env(name);
    if (v == "0" || v == "1") {
        out = v == "1";
        return true;
    }
    return false;
}

} // namespace

float ScaleExposure(float exposure, u32 percent) {
    if (!(exposure > 0.0f) || percent >= 100) {
        return exposure;
    }
    // never 0: an exposure > 0 is what keeps the velocity pass running without SMAA T2x (the upscalers)
    const float scaled = exposure * static_cast<float>(std::max<u32>(percent, 10)) / 100.0f;
    return std::max(scaled, std::numeric_limits<float>::min());
}

bool Install(u64 eboot_base, const Options& options) {
    if (Env("SSPC_POSTFX_HOOK") == "0") {
        SSPC_LOG(Engine, Warning, "Post effects: SSPC_POSTFX_HOOK=0 - the game's own (the settings do nothing)");
        return false;
    }
    bool b = false;
    g_mb_on.store(options.motion_blur);
    g_mb_strength.store(std::clamp<u32>(options.motion_blur_strength, 10, 100));
    g_grain_on.store(options.film_grain);
    g_dof_on.store(options.depth_of_field);
    if ((g_env_mb = EnvBool("SSPC_POSTFX_MOTION_BLUR", b))) {
        g_mb_on.store(b);
    }
    if (const std::string s = Env("SSPC_POSTFX_MB_STRENGTH"); !s.empty()) {
        g_env_strength = true;
        g_mb_strength.store(std::clamp<u32>(static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10)), 10, 100));
    }
    if ((g_env_grain = EnvBool("SSPC_POSTFX_FILM_GRAIN", b))) {
        g_grain_on.store(b);
    }
    if ((g_env_dof = EnvBool("SSPC_POSTFX_DOF", b))) {
        g_dof_on.store(b);
    }
    if (const std::string d = Env("SSPC_POSTFX_DIAG"); !d.empty()) {
        g_diag_bloom = d.find("bloom") != std::string::npos;
        g_diag_vignette = d.find("vignette") != std::string::npos;
        g_diag_depth_blur = d.find("depthblur") != std::string::npos;
        g_diag_mb_flip = d.find("mbflip") != std::string::npos;
        g_diag_dof_flip = d.find("dofflip") != std::string::npos;
        g_diag_mb_cycle = d.find("mbcycle") != std::string::npos;
        g_diag_dof_force = d.find("dofforce") != std::string::npos;
        SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_POSTFX_DIAG={} - zeroed after the blend:{}{}{}{}{}{}{}", d,
                 g_diag_bloom ? " bloom scale" : "", g_diag_vignette ? " vignette scale" : "",
                 g_diag_depth_blur ? " depth blur strength" : "",
                 g_diag_mb_flip ? "; motion blur on every other frame" : "",
                 g_diag_dof_flip ? "; depth of field on every other frame" : "",
                 g_diag_mb_cycle ? "; motion blur cycles off / the strength / 100 %" : "",
                 g_diag_dof_force ? "; DOF radius 1.0" : "");
    }

    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "Post effects not installed (the game's own): {}", error);
            return false;
        }
    }
    if (!VerifyGuestFunction(eboot_base, kBlendSite.function, error) ||
        !VerifyGuestFunction(eboot_base, kMotionBlurSite.function, error)) {
        SSPC_LOG(Engine, Error, "Post effects not installed (the game's own): {}", error);
        return false;
    }
    g_base = eboot_base;
    void* blend = InstallEntryProbe(eboot_base, kBlendSite, reinterpret_cast<const void*>(&BlendProbe), error);
    if (blend == nullptr) {
        SSPC_LOG(Engine, Error, "Post effects not installed (the game's own): {}", error);
        return false;
    }
    g_blend = reinterpret_cast<BlendFn>(blend);
    void* mb = InstallEntryProbe(eboot_base, kMotionBlurSite, reinterpret_cast<const void*>(&MotionBlurProbe), error);
    if (mb == nullptr) {
        // (the blend probe stays: film grain, DOF and the strength still apply)
        SSPC_LOG(Engine, Error, "Motion blur switch not installed (the game's motion blur stays): {}", error);
        g_mb_on.store(true);
    } else {
        g_motion_blur = reinterpret_cast<MotionBlurFn>(mb);
    }
    const bool changed = !g_mb_on.load() || g_mb_strength.load() != 100 || !g_grain_on.load() || !g_dof_on.load();
    const std::string what = fmt::format(
        "Post effects: entry probes on RenderOverridesUpdate 0x1ba7c0 and MotionBlurPass 0x193810 - motion blur {}{} "
        "({}%{}), film grain {}{}, depth of field {}{}",
        g_mb_on.load() ? "on" : "off", g_env_mb ? " (SSPC_POSTFX_MOTION_BLUR)" : "", g_mb_strength.load(),
        g_env_strength ? ", SSPC_POSTFX_MB_STRENGTH" : "", g_grain_on.load() ? "on" : "off",
        g_env_grain ? " (SSPC_POSTFX_FILM_GRAIN)" : "", g_dof_on.load() ? "on" : "off",
        g_env_dof ? " (SSPC_POSTFX_DOF)" : "");
    if (changed) {
        SSPC_LOG(Engine, Warning, "{}", what);
    } else {
        SSPC_LOG(Engine, Info, "{}", what);
    }
    if (const std::string s = Env("SSPC_POSTFX_STATS"); !s.empty()) {
        const u32 seconds = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
        if (seconds != 0) {
            std::thread(Reporter, seconds).detach();
        }
    }
    return true;
}

void SetMotionBlur(bool on) {
    if (g_env_mb) {
        return;
    }
    g_mb_on.store(on, std::memory_order_relaxed); // (no effect when MotionBlurPass's probe is not installed)
    SSPC_LOG(Engine, Info, "Post effects: motion blur {}", on ? "on" : "off");
}

void SetMotionBlurStrength(u32 percent) {
    if (g_env_strength) {
        return;
    }
    g_mb_strength.store(std::clamp<u32>(percent, 10, 100), std::memory_order_relaxed);
    SSPC_LOG(Engine, Info, "Post effects: motion blur strength {}%", g_mb_strength.load());
}

} // namespace SecondSon::Engine::Render::PostFx
