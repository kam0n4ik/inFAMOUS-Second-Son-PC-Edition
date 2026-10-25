// SecondSonPC — TEMPORARY bridge side of the PC settings (see settings_host.h). Uses only the bridge's own run-time
// controls (what its IPC/devtools change while playing); no renderer, shader or presenter code changes.
#include "bridge/shadps4/settings_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

#include <windows.h>

#include "base/log.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "engine/particles/smoke_quality.h"
#include "engine/render/post_effects.h"
#include "engine/render/render_resolution.h"
#include "runtime/gpu/native_backend.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "ui/menu_controller.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter; // shadPS4 src/core/libraries/gnmdriver/gnmdriver.cpp

namespace SecondSon::Bridge {

namespace {

/// [Graphics] sharpness 0..100 % -> RCAS attenuation in stops (0 = sharpest; the bridge's default 0.25 = 88 %).
float Attenuation(u32 sharpness) {
    return 2.0f * (1.0f - static_cast<float>(std::min(sharpness, 100u)) / 100.0f);
}

} // namespace

void ApplyUpscaling(bool at_start) {
    namespace Up = Gpu::Upscale;
    Up::Settings us = Up::GetSettings();
    // the upscaler must be able to run, else the scene would only be stretched
    std::string unavailable;
    if (us.backend == Up::Backend::Dlss && !Up::GetStatus().dlss_available) {
        unavailable = "DLSS unavailable: " + Up::GetStatus().dlss_why;
    } else if (us.backend == Up::Backend::Fsr) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!std::filesystem::is_regular_file(std::filesystem::path(exe).parent_path() / L"amd_fidelityfx_vk.dll")) {
            unavailable = "FSR unavailable: amd_fidelityfx_vk.dll is not next to SecondSonPC.exe";
        }
    }
    if (!unavailable.empty()) {
        SSPC_LOG(Loader, Warning, "Upscaling: {} - the scene renders at the native size", unavailable);
        Up::UpdateStatus([&](Up::Status& st) { st.reason = unavailable; });
        us.backend = Up::Backend::Off; // (the runtime's: the menu keeps the choice)
        Up::SetSettings(us);
    }
    const bool upscaling = us.backend != Up::Backend::Off;
    const Engine::Render::RenderSize rs = upscaling
                                              ? Engine::Render::RenderSizeForScale(Up::ScaleOf(us.quality))
                                              : Engine::Render::SceneSizeForRenderScale();
    // the game's SMAA T2x (its own camera jitter, a two-frame blend) off while an upscaler jitters the scene;
    // SSPC_UPSCALE_GAME_TAA=1 keeps it (diagnostics)
    static const bool keep_taa = [] {
        char gt[4] = {};
        return GetEnvironmentVariableA("SSPC_UPSCALE_GAME_TAA", gt, sizeof(gt)) == 1 && gt[0] == '1';
    }();
    Engine::Render::SetTemporalAaOff(upscaling && !keep_taa);
    if (!Engine::Render::RequestRenderSize(rs)) {
        SSPC_LOG(Loader, Error, "Upscaling: the render size cannot change (the switch is not installed)");
    }
    Up::SetRenderSize(rs.width, rs.height);
    if (!at_start) {
        Gpu::Native::SetFrameGen(us.frame_gen == Up::FrameGen::Fsr);
    }
    SSPC_LOG(Loader, Info, "Upscaling: {} {}, scene {}x{} -> display {}x{}, frame generation {}{}",
             Up::BackendName(us.backend), Up::QualityName(us.quality), rs.width, rs.height,
             Engine::Render::DisplaySize().width, Engine::Render::DisplaySize().height,
             us.frame_gen == Up::FrameGen::Fsr ? "FSR" : "off", at_start ? "" : " (applied while running)");
    // SSPC_DIAG_UPSCALE_SWITCH=seconds:upscaler:quality:framegen[;...]: what the menu does, at those seconds after
    // start (automated runs of the live switch)
    char sw[512] = {};
    if (at_start && GetEnvironmentVariableA("SSPC_DIAG_UPSCALE_SWITCH", sw, sizeof(sw)) > 0) {
        SSPC_LOG(Loader, Warning, "DIAGNOSTIC: SSPC_DIAG_UPSCALE_SWITCH='{}'", sw);
        std::thread([plan = std::string(sw)] {
            const auto t0 = std::chrono::steady_clock::now();
            size_t at = 0;
            while (at < plan.size()) {
                const size_t end = std::min(plan.find(';', at), plan.size());
                const std::string step = plan.substr(at, end - at);
                at = end + 1;
                unsigned sec = 0;
                char up[16] = {}, q[16] = {}, fg[16] = {};
                if (std::sscanf(step.c_str(), "%u:%15[^:]:%15[^:]:%15s", &sec, up, q, fg) != 4) {
                    continue;
                }
                std::this_thread::sleep_until(t0 + std::chrono::seconds(sec));
                const Up::Settings cur = Up::GetSettings();
                Up::ApplyConfig(up, q, static_cast<u32>(std::lround(cur.sharpness * 100.0f)), fg, cur.fg_multiplier,
                                cur.reflex);
                SSPC_LOG(Loader, Warning, "DIAGNOSTIC: upscaling switch at {} s: {} {} frame generation {}", sec, up, q,
                         fg);
                ApplyUpscaling(false);
            }
        }).detach();
    }
}

void ApplyPcBridgeSettings(const App::Config& cfg) {
    auto& s = EmulatorSettings;
    s.SetFsrEnabled(cfg.pc.fsr);
    s.SetRcasEnabled(cfg.pc.sharpening);
    s.SetRcasAttenuation(static_cast<int>(std::lround(Attenuation(cfg.pc.sharpness) * 1000.0f)));
}

void InstallSettingsHost(const App::Config& cfg, SDL_Window* window, bool test_mode, bool native_gpu) {
    namespace Cap = Settings::Capability;
    u32 caps = Cap::RealController; // SecondSonPC's libScePad
    if (std::ranges::find(cfg.native_groups, "audio") == cfg.native_groups.end()) {
        caps |= Cap::LiveVolume; // the bridge's audio output (SecondSonPC's own, --native audio, has no volume yet)
    }
    if (window != nullptr) {
        caps |= Cap::Window;
    }
    if (!native_gpu) {
        caps |= Cap::BridgePresenter | Cap::FpsCounter; // (the native backend adds FpsCounter once it runs)
    }
    const bool bridge_presenter = !native_gpu;
    Settings::Instance().AddApplier([bridge_presenter](const App::Config& c,
                                                       std::span<const Settings::Desc* const> changed) {
        bool fsr = false;
        bool upscaling = false;
        for (const auto* d : changed) {
            if (d->id == "audio.volume") {
                EmulatorSettings.SetVolumeSlider(static_cast<int>(c.volume)); // the audio output reads it per buffer
            } else if (d->id == "display.showFps" && bridge_presenter) {
                EmulatorSettings.SetShowFpsCounter(c.show_fps);
                Overlay::SetSimpleFps(c.show_fps);
            } else if (d->id == "display.showFps") {
                Gpu::Native::SetFpsCounter(c.show_fps); // the native presenter's counter, at its next present
            } else if (d->id == "display.frameRateLimit" && !bridge_presenter) {
                Gpu::Native::SetFrameLimit(c.pc.frame_rate_limit); // the present thread's pacing, at once
            } else if (d->id == "display.presentMode" && !bridge_presenter) {
                // R11: the vertical sync - the presenter recreates its swapchain before its next present (the
                // bridge's presenter has no such switch: Restart there, Desc::live_needs)
                Gpu::Native::SetPresentMode(c.present_mode);
            } else if (d->id == "graphics.smokeQuality") {
                Engine::Particles::SetSmokeQuality(c.pc.smoke_quality); // the game's emitters, either backend
            } else if (d->id == "graphics.particleResolution") {
                Engine::Particles::SetParticleResolution(c.pc.particle_resolution);
            } else if (d->id == "graphics.motionBlur") {
                Engine::Render::PostFx::SetMotionBlur(c.pc.motion_blur); // the game's post effects, either backend
            } else if (d->id == "graphics.motionBlurStrength") {
                Engine::Render::PostFx::SetMotionBlurStrength(c.pc.motion_blur_strength);
            } else if (d->id == "graphics.renderScale" && !bridge_presenter) {
                Engine::Render::SetRenderScale(Engine::Render::ParseRenderScale(c.pc.render_scale));
                upscaling = true; // (the scene size switches at the game's next frame, as the upscalers')
            } else if ((d->id == "display.hudSafeArea" || d->id == "display.hudArea") && !bridge_presenter) {
                Engine::Render::SetHudLayout(c.pc.hud_safe_area, c.pc.hud_area); // written at every frame start
            } else if (d->id == "graphics.fov" && !bridge_presenter) {
                Engine::Render::SetFovPercent(c.pc.fov); // at the player camera's next update
            } else if (d->id == "upscaling.sharpness" || d->id == "upscaling.frameGenMultiplier" ||
                       d->id == "upscaling.reflex") {
                // read by the GPU's recording / present threads every frame
                Gpu::Upscale::Settings s = Gpu::Upscale::GetSettings();
                s.sharpness = static_cast<float>(std::min(c.pc.upscale_sharpness, 100u)) / 100.0f;
                s.fg_multiplier = std::clamp<u32>(c.pc.frame_gen_multiplier, 2, 4);
                s.reflex = c.pc.reflex;
                Gpu::Upscale::SetSettings(s);
            } else if (d->id == "upscaling.upscaler" || d->id == "upscaling.quality" ||
                       d->id == "upscaling.frameGeneration") {
                // while the game runs: the render size switches at the game's next frame (the engine's own PS4 Pro
                // path), the upscaler's feature is made for it, frame generation replaces the swapchain
                Gpu::Upscale::ApplyConfig(c.pc.upscaler, c.pc.upscale_quality, c.pc.upscale_sharpness,
                                          c.pc.frame_generation, c.pc.frame_gen_multiplier, c.pc.reflex);
                upscaling = true;
            } else if (d->category == Settings::Category::Graphics) {
                fsr = true;
            }
        }
        if (upscaling && !bridge_presenter) {
            ApplyUpscaling(false);
        }
        if (fsr && bridge_presenter) {
            ApplyPcBridgeSettings(c);
            if (presenter) {
                // what shadPS4's own IPC and devtools change while playing (read by the presenter every frame)
                auto& f = presenter->GetFsrSettingsRef();
                f.enable = c.pc.fsr;
                f.use_rcas = c.pc.sharpening;
                f.rcas_attenuation = Attenuation(c.pc.sharpness);
            }
        }
    });
    // R11 display (diagnostic): SSPC_DIAG_DISPLAY_SWITCH=seconds:vsync=fifo|immediate|mailbox[;seconds:limit=60;...] -
    // what the settings menu does with the vertical sync / the frame rate limit, at those seconds after the start (the
    // precompiler's wait added), through the settings service and its appliers (automated runs, type B measurements)
    char ds[512] = {};
    if (native_gpu && GetEnvironmentVariableA("SSPC_DIAG_DISPLAY_SWITCH", ds, sizeof(ds)) > 0) {
        SSPC_LOG(Loader, Warning, "DIAGNOSTIC: SSPC_DIAG_DISPLAY_SWITCH='{}'", ds);
        if (test_mode) {
            // (an automated run installs no menu, which gives the service this host's capabilities: the window one
            // is what the vertical sync needs - given here, for this diagnostic only)
            Settings::Instance().SetCapabilities(Settings::Instance().Capabilities() | caps);
            SSPC_LOG(Loader, Warning, "DIAGNOSTIC: automated run: settings capabilities 0x{:x} for the display switch",
                     Settings::Instance().Capabilities());
        }
        std::thread([plan = std::string(ds)] {
            const auto t0 = std::chrono::steady_clock::now();
            size_t at = 0;
            while (at < plan.size()) {
                const size_t end = std::min(plan.find(';', at), plan.size());
                const std::string step = plan.substr(at, end - at);
                at = end + 1;
                unsigned sec = 0;
                char key[16] = {}, value[32] = {};
                if (std::sscanf(step.c_str(), "%u:%15[^=]=%31s", &sec, key, value) != 3) {
                    continue;
                }
                const std::string_view k(key);
                const Settings::Desc* d = Settings::Find(k == "vsync" ? "display.presentMode"
                                                         : k == "limit" ? "display.frameRateLimit"
                                                                        : "");
                if (d == nullptr) {
                    continue;
                }
                std::this_thread::sleep_until(t0 + std::chrono::seconds(sec) +
                                              std::chrono::nanoseconds(Gpu::Native::StartupPauseNs()));
                std::string error;
                const auto r = Settings::Instance().Set(*d, Settings::Value{std::string(value)}, &error);
                SSPC_LOG(Loader, Warning, "DIAGNOSTIC: display switch at {} s: {} = {}: {}{}", sec, d->id, value,
                         r == Settings::SetResult::Ok          ? "applied"
                         : r == Settings::SetResult::Unchanged ? "unchanged"
                                                               : "refused",
                         error.empty() ? "" : " (" + error + ")");
            }
        }).detach();
    }
    Ui::Menu().Install({.window = window,
                        .test_mode = test_mode,
                        .capabilities = caps,
                        .backend_label = {},
                        .confirm_is_circle = !cfg.enter_button_cross});
}

} // namespace SecondSon::Bridge
