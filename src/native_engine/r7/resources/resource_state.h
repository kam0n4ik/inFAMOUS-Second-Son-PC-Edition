// SecondSonPC R7-R — Native Resource State: the CUE's per-stage resources as the Native Render Layer takes them
// (NativeResourceBindings, engine/render/native_render_layer.h), versioned per stage, without reading PM4
// (SSPC_R7_RES_STATE, off by default; docs/NATIVE_ENGINE_R7_RESOURCES.md §4).
//
// Where the state comes from [A: eboot; E: offline and in game]:
//   - the CUE setters (0x16aa0 VB, 0x16b70 buffers, 0x169d0 CB, 0x16d10 T#, 0x16eb0 S#, 0x16c40 RW V#, 0x16de0 RW T#;
//     cue_set_buffers.h) - their dispatcher entries call BeforeSetter before the setter runs, in every mode but
//     Original: which shadow dwords change (byte compare against the shadow) and which user-data SGPRs a direct entry
//     (bit 15) writes. A call that changes nothing keeps the stage's content version;
//   - the CUE commit 0x14ad0 (cue_commit, recomp_14ad0 under native / guarded) - Commit runs it with the snapshot
//     allocator 0x123d0, the memcpy into the snapshot, setPointerInUserData 0x626400 and setUserDataRegion 0x626520
//     redirected (recomp_set_redirect) to observers that call the real ones: the stage's snapshot, the pointers into
//     it and the single values it writes;
//   - the CUE resets 0x13d20 (R::Reset's CUE::ResetShaders) and 0x12390 (CUE::Reset's allocator reset; 0x13da0 is not
//     probeable: a call in its prologue) - entry probes: the stage bindings of that CUE become stale until the next
//     commit, and the user-data values the CUE wrote are forgotten (another context's buffer may run in between).
//
// Versions. Per (CUE, stage) two counters, both unique ids (a thread id in the high bits, no atomics):
//   content  - changes when a setter changes a shadow dword or a direct SGPR value, or writes outside the stage's
//              window (then every stage of the CUE changes);
//   binding  - set at a commit's snapshot of the stage: kept when the content version, the layout table and the
//              user-data values are the ones of the previous binding, a new id otherwise. Equal binding versions of one
//              CUE and stage mean byte-equal snapshots (the GPU reads the snapshot) and equal user data.
// Proof: offline (r7_resources_tests.cpp: random setter / commit / reset sequences on the eboot's own code, every
// binding checked against the bytes) and in game with SSPC_R7_RES_STATE=verify (every snapshot compared with the
// previous one of the same binding version; the shadow compared with the snapshot while the content version holds).
// What a version does NOT cover: memory the descriptors point at (textures, constant data, the global table at
// CUE+0x78) - those change through guest memory and are not tracked here.
#pragma once

#include <string>

#include "base/abi.h"
#include "engine/render/native_render_layer.h"

namespace SecondSon::NativeEngine::R7::Resources {

enum class StateMode : u8 { Off = 0, On = 1, Verify = 2 };
/// SSPC_R7_RES_STATE = 1 | verify (read once; off by default).
StateMode Mode();
inline bool Enabled() {
    return Mode() != StateMode::Off;
}
/// Tests: the mode without the environment (before Prepare); the setters and the commit taken as observed (the harness
/// calls BeforeSetter and Commit itself, nothing installed); a CUE's state forgotten; everything stopped (the observer
/// cleared, Active false - the probes stay and do nothing).
void SetMode(StateMode m);
void AssumeTracking(bool on);
void Forget(u64 cue);
void Deactivate();

/// The cue_commit install calls it (module base, recomp_set_redirect): installs the reset probes once and turns the
/// setters' observation on. False (with `error`) when the state cannot run; the commit then installs without it.
bool Prepare(u64 module_base, void (*redirect)(u64 from, u64 to), std::string& error);
/// True once Prepare succeeded.
bool Active();

/// From a setter's dispatcher entry, before the setter runs: `kind` is CueSetBuffers::Kind.
void BeforeSetter(u32 kind, u64 cue, u32 stage, u32 slot, u32 count, u64 src);

/// One commit of `cue` by `impl` (the recompiled 0x14ad0) with its snapshot and user-data calls observed.
void Commit(SSPC_SYSV_ABI void (*impl)(u64), u64 cue);

/// R8-A3 NR2: a commit SecondSonPC's Native Render Layer made itself instead of 0x14ad0 (native_draw.cpp NativeCommit,
/// SSPC_R8_RENDER_RESOURCES): the same events Commit observes, told directly - begin, the stage snapshots (allocated and
/// copied from the shadow), the pointers and single values written into user data in 0x14ad0's order, end. On the
/// committing thread; Prepare registers them as Engine::Render::R7::ResourceHooks (with Fill), Deactivate clears them.
void ExternalBegin(u64 cue);
void ExternalSnapshot(u32 cue_stage, u64 snapshot, u32 dwords);
void ExternalPointer(u32 cue_stage, u32 sgpr, u64 value);
void ExternalValue(u32 cue_stage, u32 sgpr, u32 value);
void ExternalEnd(u64 cue);

/// A CUE reset (0x13d20 / 0x12390 probes; tests call it directly).
void OnReset(u64 cue);

/// The draw's VS / PS resources as the CUE holds them now (after its commit). A stage is present when its binding is
/// current (committed since the last reset, layout unchanged, no change pending - the CUE's dirty byte clear); its
/// version is 0 when tracking cannot vouch for it (a setter or the commit not observed, the table full). False when
/// the CUE is not tracked at all.
bool Fill(u64 cue, Engine::Render::R7::NativeResourceBindings& out);

/// The stage's table dwords changed between its last two snapshots (R5: what a consumer would re-decode), as a bit
/// set of 0x600 bits; false when unknown.
bool ChangedDwords(u64 cue, u32 cue_stage, u32 (&bits)[0x600 / 32]);

struct Stats {
    u64 setter_calls = 0;
    u64 setter_changing = 0;   ///< calls that changed a shadow dword or a direct value
    u64 setter_redundant = 0;  ///< calls that wrote only what was there
    u64 setter_slots = 0;      ///< descriptors written
    u64 setter_slots_same = 0; ///< ... equal to the shadow's
    u64 setter_direct = 0;     ///< direct (bit 15) descriptors
    u64 setter_outside = 0;    ///< descriptors beyond the stage's window (all stages of the CUE bumped)
    u64 setter_untracked = 0;  ///< no CUE entry (table full)
    u64 kind_calls[7] = {};
    u64 kind_redundant[7] = {};
    u64 commits = 0;
    u64 snapshots = 0;         ///< stage snapshots taken (VS / PS / others)
    u64 binding_kept = 0;      ///< ... whose binding version stayed (content, layout and user data unchanged)
    u64 binding_new = 0;
    u64 stage_snapshots[8] = {}; ///< snapshots by CUE stage (Gnm: 1 PS, 2 VS, ...)
    u64 stage_kept[8] = {};
    u64 changed_dwords = 0;    ///< sum over snapshots of the dwords changed since the stage's previous one
    u64 table_dwords = 0;      ///< sum over snapshots of the table's dwords
    u64 delta_known = 0;       ///< snapshots with a previous one of the same layout (changed_dwords counted)
    u64 shape_checked = 0;     ///< pointers into the snapshot compared with the layout's shape
    u64 shape_mismatch = 0;
    u64 other_copies = 0;      ///< memcpy calls of the commit that were not a shadow snapshot
    u64 resets = 0;
    u64 fills = 0, fills_present = 0, fills_versioned = 0;
    u64 fills_stale = 0; ///< a stage changed since its binding with no dirty byte left (a commit not observed)
    // verify
    u64 verify_same_version = 0;     ///< snapshots compared with the previous one of the same binding version
    u64 verify_violations = 0;       ///< ... bytes, layout or user data different (must stay 0)
    u64 verify_new_same_bytes = 0;   ///< new binding version, yet byte-equal snapshot and same user data (reuse missed)
    u64 verify_shadow_checked = 0;   ///< commit entries: shadow vs snapshot while the content version held
    u64 verify_shadow_violations = 0; ///< ... different (a shadow writer not observed; must stay 0)
    u64 verify_fill_checked = 0;      ///< after each commit: Fill's present VS / PS bindings, snapshot vs shadow
    u64 verify_fill_mismatch = 0;     ///< ... different (must stay 0)
    u64 external_commits = 0;         ///< R8 NR2: of the commits, the Native Render Layer's own (ExternalBegin)
    /// R8 NR2: fills refused because the snapshot allocator (CUE + 0x20) moved since the last observed commit's end
    /// (an unobserved commit such as 0x140d0, or a CS commit 0x13ec0 / 0x132b0 in between: conservative)
    u64 fills_moved = 0;
};
Stats Totals();
std::string Report(const Stats& s, double seconds);

} // namespace SecondSon::NativeEngine::R7::Resources
