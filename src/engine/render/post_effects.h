// SecondSonPC — the game's own post-processing effects as PC settings (R11 postfx; docs/R11_POSTFX.md).
//
// The engine blends its post-processing parameters once a frame (RenderOverridesUpdate 0x1ba7c0, main loop 0x29c8d0):
// the level's environment (reflection table 0xd1c800: BLOOM_*, DOF_*, DEPTH_BLUR_*, film_grain, VIGNETTE_* ...) is the
// base, the active visual overrides (reflection table 0xd45000: the same names plus MOTION_BLUR_EXPOSURE, scripted:
// cameras, powers, cut-scenes) are lerped over it by their weight, and the result lands in one global block
// (0xe87b30, = the overrides list 0xe87aa0 + 0x90) that the passes read:
//   motion blur  MOTION_BLUR_EXPOSURE (override +0x1d8, mask bit 38) -> 0xe87c18. MotionBlurPass 0x193810 (pass 30,
//                after the depth blur) runs when it is > 0 (and the debug flag 0xe86e68 is clear): a scene copy, the
//                velocity tiles, a separable blur and a composite into the HDR scene (FX 0x7d4c238..0x7d4c260); the
//                exposure scales the blur length (0.0166 / dt * exposure, 0x1a9600 / 0x1a9c00). The same exposure > 0
//                also keeps the velocity pass (pass 29, 0x193d00) running when SMAA T2x is off - the upscalers' motion.
//   film grain   film_grain (environment +0x3a4 / override +0x248) -> 0xe87c88; the film grain pass 0x195da0 draws
//                FX_FILMGRAIN (resources 0x7d4bea8 + 0x3e0) when it is > 0. Its only reader.
//   depth of field  DOF_RADIUS -> 0xe87bf4; the bokeh DOF 0x455180 and its CoC terms (0x452e30, 0x4550b0, 0x455080)
//                run when it is > 0 and the engine's debug flag 0x7dfc300 is clear (never written by the game).
//
// What the settings change (the game's own path in each case; nothing else is touched):
//   [Graphics] motionBlur = true|false   off: MotionBlurPass is not run (entry probe returns before it - the game's
//                                        own path at exposure 0); the exposure itself stays, so the velocity pass,
//                                        SMAA T2x and the upscalers see what they always saw.
//   [Graphics] motionBlurStrength = 10..100 (%)  the blended exposure scaled after the blend (blur length; never to 0).
//   film grain (SSPC_POSTFX_FILM_GRAIN=0 only, not a setting: no place played so far sets film_grain > 0)
//                                        off: the blended film_grain set to 0 after the blend.
//   depth of field (SSPC_POSTFX_DOF=0 only, not a setting: switching the flag, even with the radius forced to 1.0,
//                                        changed no captured frame) off: the engine's DOF-disable flag 0x7dfc300 set.
//
//   SSPC_POSTFX_MOTION_BLUR=0|1, SSPC_POSTFX_MB_STRENGTH=<%>, SSPC_POSTFX_FILM_GRAIN=0|1, SSPC_POSTFX_DOF=0|1
//                                        override the settings (automated runs)
//   SSPC_POSTFX_STATS=<seconds>          a log line with the blended values and the passes run every <seconds>
//   SSPC_POSTFX_DIAG=bloom,vignette,depthblur,mbflip,dofflip,mbcycle,dofforce  DIAGNOSTICS: those parameters zeroed after the
//                                        blend; mbflip / dofflip: motion blur / depth of field off on every other
//                                        frame; mbcycle: frames cycle off / the strength / 100 % (neighbouring A/B);
//                                        dofforce: the DOF radius set to 1.0 (is the bokeh there at all)
//   SSPC_POSTFX_HOOK=0                   no probe at all (the game's own everything)
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::PostFx {

struct Options {
    bool motion_blur = true;
    u32 motion_blur_strength = 100; // %
    bool film_grain = true;
    bool depth_of_field = true;
};

/// The loader's window before the guest runs: installs the probes (the eboot's functions must hash to the analysed
/// ones) unless SSPC_POSTFX_HOOK=0. Returns false and logs why when they are not; the game then runs unchanged.
bool Install(u64 eboot_base, const Options& options);

/// Changes from the settings menu: take effect at the game's next frame.
void SetMotionBlur(bool on);
void SetMotionBlurStrength(u32 percent);

/// The exposure after the strength (pure; the self-test checks it): `exposure` the blended one, `percent` 0..100.
float ScaleExposure(float exposure, u32 percent);

} // namespace SecondSon::Engine::Render::PostFx
