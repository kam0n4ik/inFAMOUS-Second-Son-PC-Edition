// SecondSonPC — Smoke Quality: fewer smoke particles spawned by the game's own particle system (experimental; the
// original density by default; docs/SMOKE_QUALITY.md).
//
// The engine's emitters (CParticleEmitterInstance, psysnew.cpp) count the particles to spawn each frame in
// EmitterSpawn 0x3f3580: bursts and continuous rate (rate * frame time, with a fractional carry) are multiplied by the
// system's PARTICLE_COUNT_RATIO (a script property: reflection entry 0xd4a3e0, field +0x1a0 of the owner, setter
// 0x3f7150) and added to the emitter's pending count (+0x78); the next update 0x3efde0 takes the pending count as the
// number of new particles of the emitter's GPU emission dispatch. An entry probe (guest_probe.h) runs the original
// function and keeps only a fraction of what it added, for smoke emitters only:
//   smoke   = the emitter's name contains "smoke". The emitter data's id (+0x18) is its name hashed by the engine's
//             name hash (h = h * 101 + lower(c) mod 2^64, the same as the reflection table's), and a name of up to
//             ~12 characters is recovered from it exactly (smoke, smoke01, smoke_near, thick_smoke); longer ones are
//             matched against a dictionary of smoke names. Fire (flames_*, light_fire), embers, sparks, glow, steam,
//             dust and foam share the same sorted translucent draw (draw kind 1, VS 0x2edac4000) and are never
//             touched; an emitter whose name is not recovered is not touched either.
//   child   = an emitter that spawns from a parent emitter's particles is left alone when the parent is reduced (it
//             already follows the parent's live count), reduced like any other when the parent is not.
//   carry   = a per-emitter fraction keeps the long-run rate exact; an emitter with nothing alive always spawns at
//             least one particle, so a puff never disappears completely.
// Nothing else changes: emitter lifetime, triggering, physics, damage and the powers' logic stay the game's.
//
//   [Graphics] smokeQuality = "high" | "medium" | "low" | "verylow"   (SecondSonPC.toml / user settings / F1 menu)
//   SSPC_SMOKE_QUALITY=high|medium|low|verylow|<density 0..1>         overrides the setting (automated runs)
//   SSPC_SMOKE_STATS=<seconds>                                        a log line per draw kind every <seconds>
//   SSPC_SMOKE_KINDS=<k,k,...>                                        only these draw kinds (diagnostics; all)
//   SSPC_SMOKE_MATCH=<word,word,...>                                  name words reduced (diagnostics; smoke)
//   SSPC_SMOKE_SELECT=<f|u|b><hex offset><op><value>,...              data field conditions (diagnostics)
//   SSPC_SMOKE_DUMP=1                                                 emitter data -> logs\smoke_emitters.txt
//   SSPC_SMOKE_HOOK=0                                                 do not install the probe at all
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Particles {

/// Density kept for a quality name: high 1.0, medium 0.65, low 0.35, verylow 0.20; a number 0..1 is taken as is
/// (diagnostics). nullopt: not a valid value.
std::optional<float> SmokeDensity(std::string_view quality);

/// The loader's window before the guest runs: installs the probes (the eboot's functions must hash to the analysed
/// ones) unless SSPC_SMOKE_HOOK=0, with the density of `quality` (SSPC_SMOKE_QUALITY wins) and the translucent
/// particles' resolution `resolution` (SSPC_PARTICLE_RES wins). Returns false and logs why when the smoke probe is not
/// installed; the game then runs unchanged at the original density.
bool Install(u64 eboot_base, std::string_view quality, std::string_view resolution);

/// A change from the settings menu: takes effect from the next spawn of each emitter.
void SetSmokeQuality(std::string_view quality);

/// Particle Resolution (experimental; [Graphics] particleResolution, SSPC_PARTICLE_RES): the engine's own
/// reduced-resolution path for its sorted translucent particles (fire, smoke, dust - one draw), which the PS4 build
/// enters by itself when the particle pass is slow (TranslucentParticles 0x3ed7b0, level 0..2 at manager+0xca0,
/// pass 28 at screen x 1 / 0.5 / 0.25, upsampled into the HDR target). The particles and their count stay the same;
/// only the pixels they are drawn at change. "auto" (the game's own adaptation; the port's GPU timers keep it at
/// full), "full", "half", "quarter". -1/0/1/2; nullopt: not a valid value.
std::optional<s32> ParticleLevel(std::string_view resolution);

/// A change from the settings menu: takes effect from the next frame.
void SetParticleResolution(std::string_view resolution);

/// The density in use (1.0 when the probe is not installed).
float CurrentDensity();

/// One spawn decision (pure; the self-test checks it): `added` particles the game asked for this call, `density` kept
/// on average, `carry` the emitter's fraction (updated), `nothing_alive` the emitter has no live particle.
u32 ScaleSpawn(u32 added, float density, float& carry, bool nothing_alive);

/// The engine's 64-bit name hash (property names, emitter ids): h = h * 101 + lower(c) mod 2^64.
u64 EngineNameHash(std::string_view name);

/// Whether the emitter whose data id (+0x18) is `id` is smoke: its name, recovered from the hash, contains "smoke"
/// (or the id is a dictionary smoke name). `name`: the recovered name, "" when none.
bool SmokeEmitterName(u64 id, std::string* name = nullptr);

} // namespace SecondSon::Engine::Particles
