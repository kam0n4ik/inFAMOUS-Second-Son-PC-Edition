// SecondSonPC — R12 weather: the stand's tool for reproducible weather states through the engine's own data
// (docs/R12_WEATHER.md). Diagnostics only: nothing is installed unless SSPC_R12_WEATHER is set.
//
// What the engine has [A] (eboot of CUSA00004, docs/R12_WEATHER.md §1):
//   The weather and the time of day are one object, CTimeOfDayParams (TIME_OF_DAY_PARAMS, reflection table 0xd1bc60..
//   0xd1e2a0). The city (world_seaport) has seven of them (blustery, candy, dismal, early_morning, hazy_sunset, night,
//   prototype), each with its own lighting packages (imposter-/sh-/spec-world_seaport(<tod>).xpps). The current one is
//   held by the time-of-day manager 0x7f8e800 (+0 current, +8 the "ensured" default, +0x10 the requested id, a map of
//   the registered ones at +0x30), which the main loop 0x29c8d0 updates once a frame (0x59cf80) before the render
//   parameters are blended (0x1ba7c0). Its fields: WETNESS_LEVEL +0x348, WETNESS_WALL_MULTIPLIER +0x34c, PUDDLE_LEVEL
//   +0x350, WETNESS_STREAK_LEVEL +0x354, WETNESS_RIPPLE_RATE +0x358, BASE_WIND_* +0x378/+0x37c, SSR_OPACITY +0x3c8,
//   HALF_RES_REFLECTIONS +0x3cc, BLUR_WATER_REFLECTIONS +0x3cd, VISIBILITY +0x3d4 (Clear/Foggy/Night),
//   WEATHER_KIND +0x3d8 (Default/Rainy), the clock (CLOCK_TIME, HHMM -> a fraction of the day) +0x3e0, HAZE_* ...
//   The frame's wetness = WETNESS_LEVEL (base, 0x1b8e60) lerped with the visual overrides' `wetness` (mask bit 57,
//   0x1b9220) -> 0xe87c80 (GetWetness 0x598b50). At wetness > 0 the deferred wetness pass 0x597ec0 runs (0x190c90:
//   ripples 0x5984e0, FX_WETNESS_DEFERRED) and the rain shadow maps are drawn (0x43c350 / 0x43fcb0, shadow kinds
//   "Rain" / "Rain Dynamic").
//
// What the tool does (on the game's main thread: an entry probe on the time-of-day update 0x59cf80, after it ran):
//   SSPC_R12_WEATHER=log                the journal only: nothing is written
//   SSPC_R12_WEATHER=dry                the current time of day's WETNESS_LEVEL = 0 (surfaces dry; what else the time of
//                                       day has, rain particles included, stays)
//   SSPC_R12_WEATHER=wet[:<w>[:<p>]]    WETNESS_LEVEL = w (default 1), PUDDLE_LEVEL = p (default: unchanged)
//   SSPC_R12_WEATHER_AT=<s>             apply no earlier than <s> seconds after start (default 0: when safe)
//   SSPC_R12_WEATHER_UNTIL=<s>          restore the original values at <s> seconds (default: at exit)
//   SSPC_R12_WEATHER_LOG=<s>            a journal line every <s> seconds (default 5)
//   SSPC_R12_WEATHER_FILE=<path>        the journal (default <logs>\weather.log); the main points also go to the log
// Each change: the original values recorded, the new ones written, read back (the field and the blended wetness one
// frame later, the wetness pass's calls in the next journal period), restored at UNTIL / when the state turns unsafe
// / at exit. Never while a movie is shown, the game's clock is stopped (pause, menus, loading), the time of day is
// being changed or loaded (the request differs from the current one, the manager's busy flags 0x7e00740 / 0x7dff388,
// the ensured default in use) or within 3 s of any of those; the game's own data stays as it was then. It never saves.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Engine::Render::Weather {

/// The loader's window before the guest runs: nothing unless SSPC_R12_WEATHER is set; otherwise verifies the analysed
/// functions (hash-pinned) and installs the probes. Returns false and logs why when it does not.
bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir);

} // namespace SecondSon::Engine::Render::Weather
