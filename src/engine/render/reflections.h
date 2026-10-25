// SecondSonPC — the game's reflections (R12 refl; docs/R12_REFLECTIONS.md).
//
// What the engine has (eboot.bin of CUSA00004; [A] Ghidra, [E] measured in the stand's runs):
//   screen-space reflections  SsrPass 0x45a690(scene targets 0xe682b0, context, camera), recorded in pass 12's setup
//                (0x191100, after the shadow contexts): when the debug flag 0x7dff160 is clear and the environment's
//                SSR_OPACITY (environment object *0x7f8e800 + 0x3c8; reflection table 0xd1df40) is > 0 - otherwise it
//                records nothing at all. Its draws: a scene copy (0x45a110), a mask, the ray march (0x459940, the
//                previous frame's result 0x1aed0 as history), a copy of it into the history, the reflected colour, a
//                separable bilateral blur (0x45a430, twice) and a blended composite into the HDR scene.
//   planar reflections  0x3bfb90 (pass 18, 0x191e30) and the water's 0x5974f0 (pass 16, 0x191760): one context per
//                reflection plane (a clip plane each), a copy into the half- or quarter-size target (the environment's
//                HALF_RES_REFLECTIONS +0x3cc) and a blur (BLUR_WATER_REFLECTIONS +0x3cd for the water).
//   cube maps    the ambient specular probes (AMBIENT_SPEC_PROBE, ambient_spec_probes/%s(%s).dds) and FX_AMBIENT_SPECULAR;
//                AMBIENT_SPECULAR_CUBEMAP_MULTIPLIER (environment +0x250).
//   wetness      WETNESS_LEVEL / PUDDLE_LEVEL ... (environment +0x348..0x368) and FX_WETNESS_DEFERRED.
//
//   [Graphics] reflectionQuality (SSPC_REFLECTION_QUALITY=original|low overrides it): "low" skips SsrPass - the game's
//                                   own path at SSR_OPACITY 0 (nothing recorded); live. The entry probe on SsrPass is
//                                   always installed (a pass-through at "original").
//   SSPC_R12_REFL_STATS=<seconds>   a log line every <seconds>: the passes' calls, the environment's reflection values
//                                   (diagnostics; the FX objects' classes once)
//   SSPC_R12_REFL_DIAG=ssroff,ssrflip,ssrcycle,planaroff,wateroff  DIAGNOSTICS (change the picture, A/B only):
//                                   SsrPass / the planar / the water reflections not recorded (ssrflip: on every other
//                                   frame; ssrcycle: in every other second - the live switch's first frames)
//   SSPC_R12_REFL_HOOK=0            no probe at all (the game's own everything; the setting does nothing)
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Render::Reflections {

/// The loader's window before the guest runs: installs the SsrPass probe (and, with a stats / diagnostic variable, the
/// planar and water ones) when the eboot's functions hash to the analysed ones. quality: [Graphics] reflectionQuality.
/// Returns false and logs why when the SsrPass probe is not installed (the game's own reflections; the setting does
/// nothing).
bool Install(u64 eboot_base, std::string_view quality);

/// [Graphics] reflectionQuality, live: from the game's next frame ("original" | "low"; anything else = "original").
void SetQuality(std::string_view quality);

} // namespace SecondSon::Engine::Render::Reflections
