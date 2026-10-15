// SecondSonPC — PC settings added by the in-game settings menu (docs/PC_SETTINGS_IMPLEMENTATION.md): the
// SecondSonPC.toml keys of [Display] monitor, [Graphics], [Gamepad] and [Interface]. Own code; App::Config keeps
// them in one member (Config::pc) so the older sections stay as they are.
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::Settings {

struct PcSettings {
    // [Display]
    std::string monitor; // display name the window opens on; "" = the primary display

    // [Graphics]: the bridge's presenter (FSR 1.0 + RCAS of the final frame); the native backend: P4.6
    bool fsr = false;       // upscales the game's 1920x1080 frame to a larger window
    bool sharpening = true; // RCAS after FSR
    u32 sharpness = 88;     // 0..100 %; RCAS attenuation = 2 * (1 - sharpness / 100) (88 % = the bridge's 0.25)
    // [Graphics] smoke: particles spawned by the game's smoke emitters (src/engine/particles/smoke_quality.h); both
    // backends. "high" (the game's own) | "medium" 65 % | "low" 35 % | "verylow" 20 %
    std::string smoke_quality = "high";
    // [Graphics] the engine's own reduced-resolution path for its translucent particles (fire, smoke, dust): "auto"
    // (the game's own adaptation) | "full" | "half" | "quarter"
    std::string particle_resolution = "auto";

    // [Gamepad]: a real controller (DualShock 4 and others through SDL)
    u32 vibration = 100;             // % of the motor strength the game asks for; 0 = off
    double camera_sensitivity = 1.0; // right stick deflection multiplier, on top of the game's own option
    bool invert_camera_x = false;    // mirrors the right stick (the game has its own inversion options too)
    bool invert_camera_y = false;
    std::string menu_button = "guide"; // opens the settings menu: "guide" (PS button) | "l3r3" (held) | "off"

    // [Interface]: SecondSonPC's own menu (the game's menus and prompts are the game's)
    std::string ui_language = "auto"; // "auto" (as [Language] system) | "ru" | "en"
    u32 ui_scale = 100;               // % on top of the window height / 1080
    std::string prompts = "auto";     // button hints in the menu: "auto" | "playstation" | "xbox" | "keyboard"

    // [ShaderCache] (P4.14): the native backend's persistent shader cache (enabled/dir are older keys of the table)
    bool shader_precompile = true;          // the pipelines known from earlier runs prepared before the game starts
    bool shader_precompile_progress = true; // the progress panel while they are (Esc skips the wait)
    u32 shader_cache_max_mib = 2048;        // the shader cache file's size limit
    u32 shader_precompile_threads = 0;      // 0: automatic (half the logical processors, at most 6)
};

} // namespace SecondSon::Settings
