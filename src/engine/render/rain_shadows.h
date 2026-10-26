// SecondSonPC — the game's rain shadow maps (R12 rain; docs/R12_RAIN_SHADOWS.md).
//
// What the engine has (eboot.bin of CUSA00004; [A] Ghidra, [E] measured in the stand's runs):
//   three shadow objects (static init 0x4446a0, 0x2450 bytes each, cascades of 0x8e0 bytes at +0, cascade count
//   +0x2380, kind +0x2440): 0x7d45120 = Directional (kind 0, the sun), 0x7d47570 = Rain (kind 1, static geometry,
//   mask 0x10), 0x7d499c0 = Rain Dynamic (kind 2, moving objects, mask 0x1000). The rain kinds are a top-down depth
//   map of what covers the ground from the rain: 2 cascades (0xdf9d28), of which only cascade 1 is drawn
//   ("CSM1_RAIN_SHADOW_*" in the render settings table 0xd1d7a0..0xd1da40), straight down (0x600d90 with
//   0x8000000080000000), out to CSM1_RAIN_SHADOW_DISTANCE (float 0xdf9d20, 10000; setter 0x2e3380 only stores it).
//   They exist only while the frame's blended wetness (0xe87c80, GetWetness 0x598b50) is > 0, in every function:
//     IsActive 0x43b740(object)   kinds 1/2: wetness > 0; kind 0: the sun's own test
//     Update   0x43b7b0(object, view)  (0x41b490, once a frame, sun -> rain -> rain dynamic): at wetness <= 0 the
//              rain kinds release their cascades (0x4394a0 + the render data's reference, +0x8d0/+0x8d8 = 0) and
//              nothing else - the game's own "no rain shadow" state.
//   Readers of the map (the Rain object's last cascade):
//     the deferred wetness pass 0x597ec0 binds it through 0x43f7c0 - with no cascade it binds the default texture
//              (0x7d4c348) with the sampler's depth compare set to "always" (|0x7000): uncovered everywhere;
//     the particles' frame constants 0x3ecb20 (from 0x190330's 0xe7e578/0xe7e598 per frame parity): with no cascade
//              the default texture and zero parameters.
//   Draws: 0x190330 (pass 1's setup 0x1905e0: Rain; when inactive it clears 0xe7d660[parity] instead), 0x190520 and
//   pass 28's setup 0x1923f0 (Rain Dynamic). [E] ~995 depth-only draws a frame in the stand's rain scenes, ~0.15 ms of
//   GPU but ~1.6 ms of the command processor (R12 wfx).
//
//   Draw lists: Update sets each cascade's render data geometry mask (+8; 0x43c04c -> 0x197960) every frame, and the
//   draw list (0x439650 -> 0x198d50) collects only objects of that mask (& 0x31ff).
//
//   [Graphics] rainShadows (SSPC_RAIN_SHADOWS=original|off overrides it): "off" = an empty map - after the original
//              Update the two rain objects' geometry mask is 0, so nothing is drawn into their maps; the maps stay
//              allocated, cleared and bound as at "original" (nothing reads the default texture). Live: latched at
//              the sun's update (the frame's first), Update restores the mask itself next frame. "original": as the
//              game. (The game's own wetness-0 path - releasing the maps - while the wetness stays > 0 turns the whole
//              frame grey on this emulator: SSPC_R12_RAIN_DIAG=release, diagnostics only.)
//   SSPC_R12_RAIN_STATS=<seconds>  a log line every <seconds>: updates, releases, IsActive answers by kind, the
//                                  cascades with render data, wetness, distance (diagnostics)
//   SSPC_R12_RAIN_DIST=<units>     DIAGNOSTICS: CSM1_RAIN_SHADOW_DISTANCE written before each rain update (A/B only)
//   SSPC_R12_RAIN_CYCLE=<seconds>  DIAGNOSTICS: off in every other <seconds> (the live switch's path, over and over)
//   SSPC_R12_RAIN_DIAG=release     DIAGNOSTICS: "off" releases the maps (IsActive 0x43b740 replaced, Update's
//                                  wetness-0 path): breaks the picture, kept to show why it is not the setting
//   SSPC_R12_RAIN_HOOK=0           no hook at all (the game's own everything; the setting does nothing)
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Render::RainShadows {

/// The loader's window before the guest runs: probes Update 0x43b7b0 (and for SSPC_R12_RAIN_DIAG=release replaces
/// IsActive 0x43b740) when the eboot's functions hash to the analysed ones. mode: [Graphics]
/// rainShadows. Returns false and logs why when nothing is installed (the game's own; the setting does nothing).
bool Install(u64 eboot_base, std::string_view mode);

/// [Graphics] rainShadows, live: from the game's next frame ("original" | "off"; anything else = "original").
void SetMode(std::string_view mode);

} // namespace SecondSon::Engine::Render::RainShadows
