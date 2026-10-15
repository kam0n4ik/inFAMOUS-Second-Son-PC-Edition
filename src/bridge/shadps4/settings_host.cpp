// SecondSonPC — TEMPORARY bridge side of the PC settings (see settings_host.h). Uses only the bridge's own run-time
// controls (what its IPC/devtools change while playing); no renderer, shader or presenter code changes.
#include "bridge/shadps4/settings_host.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "base/log.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "engine/particles/smoke_quality.h"
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

void ApplyPcBridgeSettings(const App::Config& cfg) {
    auto& s = EmulatorSettings;
    s.SetFsrEnabled(cfg.pc.fsr);
    s.SetRcasEnabled(cfg.pc.sharpening);
    s.SetRcasAttenuation(static_cast<int>(std::lround(Attenuation(cfg.pc.sharpness) * 1000.0f)));
}

void InstallSettingsHost(const App::Config& cfg, SDL_Window* window, bool test_mode, bool native_gpu) {
    namespace Cap = Settings::Capability;
    u32 caps = Cap::LiveVolume | Cap::RealController; // the bridge's audio output and SecondSonPC's libScePad
    if (window != nullptr) {
        caps |= Cap::Window;
    }
    if (!native_gpu) {
        caps |= Cap::BridgePresenter;
    }
    const bool bridge_presenter = !native_gpu;
    Settings::Instance().AddApplier([bridge_presenter](const App::Config& c,
                                                       std::span<const Settings::Desc* const> changed) {
        bool fsr = false;
        for (const auto* d : changed) {
            if (d->id == "audio.volume") {
                EmulatorSettings.SetVolumeSlider(static_cast<int>(c.volume)); // the audio output reads it per buffer
            } else if (d->id == "display.showFps" && bridge_presenter) {
                EmulatorSettings.SetShowFpsCounter(c.show_fps);
                Overlay::SetSimpleFps(c.show_fps);
            } else if (d->id == "graphics.smokeQuality") {
                Engine::Particles::SetSmokeQuality(c.pc.smoke_quality); // the game's emitters, either backend
            } else if (d->id == "graphics.particleResolution") {
                Engine::Particles::SetParticleResolution(c.pc.particle_resolution);
            } else if (d->category == Settings::Category::Graphics) {
                fsr = true;
            }
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
    Ui::Menu().Install({.window = window,
                        .test_mode = test_mode,
                        .capabilities = caps,
                        .backend_label = {},
                        .confirm_is_circle = !cfg.enter_button_cross});
}

} // namespace SecondSon::Bridge
