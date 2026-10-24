// SecondSonPC — the engine's runs of mesh draws, told to the command processor (CITY-OPT Phase 5,
// SSPC_CITY_ENGINE_OPT=1; off by default; docs/CITY_ENGINE_OPTIMIZATION.md).
//
// Two draw functions of the engine record ~70 % of the city's draws: 0x1a1960 (a mesh instance: the CUE's VS vertex
// table, triangle list, 16-bit indices, commit, drawIndex) and 0x43d480 (the shadow casters' instanced draw: the same
// plus the instances' constant buffer and NUM_INSTANCES). Draw after draw of one material they change only the VS
// vertex table and the index range (0x1a1960) - docs/CITY_ENGINE_REVERSE.md.
//
// Entry probes (guest_probe.h) on both run the function unchanged; then they tag the draw it wrote: the driver's
// drawIndex leaves a 4-dword NOP after the draw packet whose body the hardware ignores and whose first dword it zeroes
// (native_render_ops.h, the P5.3 direct path) - the tag goes into that body, so the stream gets no dword more. The
// command processor reads the tag as a hint only: whether a tagged draw joins the run before it is decided from its
// registers and preparation (parallel_cp.h, SSPC_CITY_ENGINE_OPT).
#pragma once

#include <array>
#include <atomic>

#include "base/abi.h"
#include "native_engine/reconstructed/gbuffer_draw.h"

namespace SecondSon::Engine::Render::DrawRuns {

inline constexpr u32 kRunMagic = 0x52505353u; ///< "SSPR": the first body dword of a tagged draw's trailing NOP
inline constexpr u32 kTrailingNopHeader = 0xC0021000u; ///< that NOP: type 3, 3 body dwords (kDrawNopHeader)

/// SSPC_NATIVE_GBUFFER: the tag's source dword carries this bit when the draw's engine descriptors were captured into
/// the side ring; the fourth NOP body dword (nop[3]) then holds the ring sequence number (EngineDesc).
inline constexpr u32 kNativeDesc = 0x200;

enum class Source : u8 {
    None = 0,
    Mesh = 1,   ///< 0x1a1960
    Shadow = 2, ///< 0x43d480
    /// 0x1a1770: the mesh pass's instanced draw (G-buffer, pass 1 and others; CITY-OPT Phase 6) - tagged only with
    /// SSPC_CITY_ENGINE_GBUFFER_RUNS or SSPC_DIAG_ENGINE_SRT
    GBuffer = 3,
};

/// CITY-OPT Phase 6, in the tag's source dword (Source::GBuffer): the mesh pass set another material on this context
/// (0x448640, called by 0x1a2030 only when the material of its sorted objects changes) since its last 0x1a1770 call -
/// the draw's PS reads other descriptors; a hint only: the processor refuses the draw as a run member, never accepts one
inline constexpr u32 kMaterialChanged = 0x100;

/// The tag of the trailing NOP at `nop` (4 dwords readable): None when it carries none.
inline Source TagOf(const u32* nop) {
    return nop[0] == kTrailingNopHeader && nop[1] == kRunMagic ? static_cast<Source>(nop[2] & 0xFF) : Source::None;
}
/// SSPC_NATIVE_GBUFFER: the tag carries engine descriptors (kNativeDesc); nop[3] is then the ring sequence number.
inline bool NativeDescOf(const u32* nop) {
    return nop[0] == kTrailingNopHeader && nop[1] == kRunMagic && (nop[2] & kNativeDesc) != 0;
}
inline u32 NativeDescSeq(const u32* nop) {
    return nop[3];
}

/// SSPC_NATIVE_GBUFFER: what the engine gave one 0x1a1770 draw - its descriptors (Describe) and its VS / PS CUE state
/// at the commit (CaptureStage) - in the side ring (the reconstruction's own types, gbuffer_draw.h).
struct EngineDraw {
    NativeEngine::Reconstructed::GBufferDraw::DrawDescription desc;
    NativeEngine::Reconstructed::GBufferDraw::StageShadow vs, ps;
};
/// The ring slot of the draw whose trailing NOP holds `seq`, read in place (a seqlock): nullptr when the slot has since
/// been reused or is being written - the command processor then uses the full path. After reading it, EngineDescStill
/// says whether the slot still held `seq` throughout (false: what was read may be another draw's - discard it).
const EngineDraw* EngineDescPeek(u32 seq);
bool EngineDescStill(u32 seq);
/// Engine-descriptor capture is on (SSPC_NATIVE_GBUFFER set and the reconstruction's pure callees are ready).
bool EngineDescOn();
/// R10 (SSPC_R10_NATIVE_DRAWS=verify|guarded|native, off by default; docs/R10_CP.md): the shadow casters' (0x43d480) and
/// mesh (0x1a1960) draws' VS / PS CUE state at their commit, captured after the engine's call as for 0x1a1770 (the
/// same commit 0x14ad0 runs right before their drawIndex), into a ring of its own with smaller slots
/// (CompactStageShadow; SSPC_R10_NATIVE_DRAWS_SOURCES=shadow|mesh|shadow,mesh, both by default). The tag carries
/// kNativeDesc and nop[3] the sequence as for the G-buffer; the source in the tag tells which ring it is.
struct EngineStages {
    NativeEngine::Reconstructed::GBufferDraw::CompactStageShadow vs, ps;
};
const EngineStages* StagesPeek(u32 seq);
bool StagesStill(u32 seq);
/// The capture is on for draws of `source` (Source::Mesh / Source::Shadow).
bool StagesOn(Source source);
/// R10: the captures so far by source (Mesh, Shadow): made; the stages not captured (no layout / too many dwords);
/// the game threads' ticks spent capturing (CaptureStage x2 + the ring write).
struct StagesCounters {
    std::array<u64, 2> made{}, refused{}, ticks{};
};
StagesCounters StagesStats();
/// The tag carries kMaterialChanged.
inline bool MaterialChangedOf(const u32* nop) {
    return nop[0] == kTrailingNopHeader && nop[1] == kRunMagic && (nop[2] & kMaterialChanged) != 0;
}

struct Counters {
    std::atomic<u64> calls[4]{};    ///< by Source
    std::atomic<u64> tagged[4]{};
    std::atomic<u64> no_nop{0};     ///< the function wrote no draw with a trailing NOP (nothing tagged)
    std::atomic<u64> materials{0};  ///< 0x448640 calls (Phase 6, with the G-buffer's tags)
    std::atomic<u64> material_tagged{0}; ///< G-buffer draws tagged kMaterialChanged
};
const Counters& Stats();

/// The loader's window before the guest runs. SSPC_CITY_ENGINE_OPT=1|verify with SecondSonPC's GPU backend only (the
/// bridge's command processor does not know the tag; it ignores a NOP's body anyway). False (logged) when the switch is
/// set but nothing was installed.
bool InstallFromEnvironment(u64 eboot_base, bool native_backend);

/// The probes are installed (the processor looks for tags then).
bool On();
/// CITY-OPT Phase 6: 0x1a1770's draws are tagged too (Source::GBuffer).
bool GBufferTagged();
// (R7-Core C3: with the Native Render Layer's G-buffer path on, the probe on 0x1a1770 opens the layer's scope for the
// call - native_render_layer.h, OpenScope)

} // namespace SecondSon::Engine::Render::DrawRuns
