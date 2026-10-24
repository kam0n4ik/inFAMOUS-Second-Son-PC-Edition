// SecondSonPC — PC settings added by the in-game settings menu (docs/PC_SETTINGS_IMPLEMENTATION.md): the
// SecondSonPC.toml keys of [Display] monitor, [Graphics], [Gamepad] and [Interface]. Own code; App::Config keeps
// them in one member (Config::pc) so the older sections stay as they are.
#pragma once

#include <map>
#include <string>

#include "base/abi.h"

namespace SecondSon::Settings {

struct PcSettings {
    // [Display]
    std::string monitor; // display name the window opens on; "" = the primary display
    // FrameBreak FB-2 (native backend): "console" (a flip at the PS4's 59.94 Hz vblank) | "30" | "60" | "90" | "120" |
    // "144" | "165" | "240" | "unlimited" (a flip as soon as its frame is done). Default 60: no vblank wait, and the
    // frame rate the game was made for (FB-3: its clock measured at 30 / 56 FPS / unlimited runs at wall time)
    std::string frame_rate_limit = "60";
    // the game's own resolution and picture shape (docs/RESOLUTION_IMPLEMENTATION.md); used from the next start
    std::string resolution = "auto";   // "auto" (the window, or the display in full screen) | "WxH": the game's display
    std::string aspect_ratio = "auto"; // "auto" (the output's, 4:3..32:9) | "16:9" | "16:10" | "21:9" | "32:9" | "4:3"
    // the HUD (while playing): its distance from the picture's edges, as the PS4's Display Area (the game takes
    // 85..100 %), and on a wide picture the part of it the HUD uses
    u32 hud_safe_area = 100;       // 85..100 %
    std::string hud_area = "full"; // "full" (to the picture's edges) | "21:9" | "16:9" (the centre of that shape)

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
    // [Graphics] the 3D scene against the game's display, per side (native backend; while playing up to the size the
    // game started with): "auto" (at most 1080 lines, as the PS4) | "50" | "60" | "67" | "75" | "85" | "100"
    std::string render_scale = "auto";
    u32 fov = 100; // 80..130 %: the player camera's vertical field of view (cut-scenes keep theirs)
    // [Graphics] upscaling and frame generation (native backend; docs/UPSCALING_FRAME_GENERATION.md)
    std::string upscaler = "off";            // "off" | "dlss" | "fsr"
    std::string upscale_quality = "quality"; // "native" | "quality" | "balanced" | "performance" | "ultra"
    u32 upscale_sharpness = 50;              // 0..100 %: RCAS after DLSS, FSR's own RCAS
    std::string frame_generation = "off";    // "off" | "fsr" | "dlss"
    u32 frame_gen_multiplier = 2;            // 2 | 3 | 4 (3 and 4: DLSS Multi Frame Generation, RTX 50)
    bool reflex = true;                      // NVIDIA Reflex low latency with DLSS Frame Generation

    // [Gamepad]: a real controller (DualShock 4 and others through SDL)
    u32 vibration = 100;             // % of the motor strength the game asks for; 0 = off
    double camera_sensitivity = 1.0; // right stick deflection multiplier, on top of the game's own option
    bool invert_camera_x = false;    // mirrors the right stick (the game has its own inversion options too)
    bool invert_camera_y = false;
    std::string menu_button = "off"; // a shortcut to the PC settings besides the game's Settings screen: "guide" | "l3r3" | "off"

    // [Interface]: SecondSonPC's own menu (the game's menus and prompts are the game's)
    std::string ui_language = "auto"; // "auto" (as [Language] system) | "ru" | "en"
    u32 ui_scale = 100;               // % on top of the window height / 1080
    std::string prompts = "auto";     // button hints in the menu: "auto" | "playstation" | "xbox" | "keyboard"

    // [ShaderCache] (P4.14): the native backend's persistent shader cache (enabled/dir are older keys of the table)
    bool shader_precompile = true;          // the pipelines known from earlier runs prepared before the game starts
    bool shader_precompile_progress = true; // the progress panel while they are (Esc skips the wait)
    bool shader_predict = true;             // every shader of the game's packs compiled before it starts (a console
                                            // window with the progress; once per templates/predictor/driver)
    u32 shader_cache_max_mib = 2048;        // the shader cache file's size limit
    u32 shader_precompile_threads = 0;      // 0: automatic (half the logical processors, at most 6)

    // [Experiments] (settings/experiments.h): the runtime's SSPC_* switches, variable -> its value ("" = not set);
    // the launcher's environment at start, then the user's choices (from the next start)
    std::map<std::string, std::string, std::less<>> experiments;
};

} // namespace SecondSon::Settings
