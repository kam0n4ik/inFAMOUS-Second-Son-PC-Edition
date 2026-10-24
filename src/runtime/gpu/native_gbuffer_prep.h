// SecondSonPC - the Native Engine G-buffer draw path (SSPC_NATIVE_GBUFFER; off by default;
// docs/NATIVE_GBUFFER_DRAW_0x1a1770.md). A 0x1a1770 draw prepared from the engine's own state, not from PM4/SRT.
//
// The mesh pass draws the city's G-buffer with 0x1a1770 (engine/render/draw_runs.h). At each such draw the engine
// side (draw_runs.cpp) records into a side ring, indexed by the draw's trailing NOP:
//   * its descriptors from their source structures (gbuffer_draw.h Describe: the instance records' V#, the vertex
//     streams, the per-object constants - the GAME_VERIFIED Instanced formulas), and
//   * its VS / PS constant-update-engine state at the commit (gbuffer_draw.h CaptureStage): the CUE shadow 0x14ad0
//     copies to frame memory, and the user-data registers it points into that copy.
// What a stage's SRT walk reads through those registers *is* that shadow. This file is the command processor's
// consumer:
//   * a stage map, once per (module, CUE layout): which shadow dword each flattened user-data dword is - the module's
//     own walk run once over tagged registers (TagReader), never over guest memory;
//   * a draw's stages built from the engine's state through the map (no SRT walk: the flattened user data, the
//     vertex V#s), decoded as the full preparation decodes (DecodeResources), the specialization key checked;
//   * the heads (program, module, runtime parameters, pipeline) from a template: an earlier full preparation of
//     the same programs, pipeline registers (the processor's pipeline key), targets and CUE layouts. A template
//     outlives its frame: at its first use in a new one its programs are looked up again in the program cache (as
//     the full preparation looks them up: read again, or trusted by write tracking) and its fetch shader's code is
//     read and compared; a variant whose program or fetch shader is another now is dropped.
// verify: every draw prepared in full (the walk, what the draw uses) and built from the engine too, the two plans
// compared binding by binding (raw V#/T#/S#, addresses, sizes, bindings, statuses) and the flattened data dword by
// dword. guarded / native: the engine build is the draw's preparation; anything it does not describe (no record, a
// map the walk does not fit, no template, another key) takes the full path. guarded also checks every 64th engine
// build against the walk and turns the engine path off at the first difference.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "engine/render/draw_runs.h"
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu {

namespace GDR = NativeEngine::Reconstructed::GBufferDraw;
namespace GS = Gpu::Shader;

class NativeGBuffer {
public:
    enum class Mode : u8 { Off, Verify, Guarded, Native };

    /// A stage's CUE state at its draw's commit, whichever ring holds it (the G-buffer's StageShadow, R10's compact one).
    struct ShadowRef {
        u64 layout = 0;
        u16 dwords = 0;
        bool captured = false;
        std::array<u8, GDR::kShadowPointers> reg{};
        std::array<u16, GDR::kShadowPointers> off{};
        const u32* shadow = nullptr; ///< [0, dwords)
        template <u32 N>
        ShadowRef(const GDR::StageShadowOf<N>& s) // NOLINT (implicit: a view)
            : layout(s.layout), dwords(s.dwords), captured(s.captured), reg(s.reg), off(s.off), shadow(s.shadow.data()) {}
    };
    /// A draw's two stages as the engine committed them.
    struct EngineState {
        ShadowRef vs, ps;
    };

    /// SSPC_NATIVE_GBUFFER's path (0x1a1770).
    NativeGBuffer();
    /// R10 (SSPC_R10_NATIVE_DRAWS): the same path for another engine draw function's draws - `env` its mode's
    /// variable, `what` its name in the log and the report.
    NativeGBuffer(const char* env, const char* what);

    Mode mode() const { return mode_; }
    bool On() const { return mode_ != Mode::Off; }
    /// guarded / native: the engine build is used (and not turned off by a guarded difference).
    bool Takes() const { return (mode_ == Mode::Guarded || mode_ == Mode::Native) && !tripped_; }

    /// A tagged G-buffer draw reached the preparation; `has`: its engine record was found (false: no record, or the
    /// ring slot reused since - the full path).
    void NoteRecord(bool has) {
        Bump(draws_);
        Bump(has ? with_record_ : no_record_);
    }

    /// guarded / native: prepare `d` from the engine record `e` and a template (see above) into `prep`, `state`, `vs`,
    /// `ps`. false: the full path (the reason is counted). `programs`, `memory`: the frontend's program cache and its
    /// guest reader (a template's validation in a new frame); `targets`: a hash of the draw's colour/depth target
    /// registers (the register file's group hashes). `material`: the stages' (VS, PS) material ids - module, T#s,
    /// S#s - for the executor's binding memo (Vk::Executor::GbBind; 0: a stage it does not keep).
    bool Prepare(const Cp::DrawDescriptor& d, const EngineState& e,
                 GS::ProgramCache& programs, GS::GuestReader& memory, u64 targets, GS::DrawPreparation& prep,
                 const GS::GraphicsPipelineState*& state, GS::StageView& vs, GS::StageView& ps,
                 std::array<u64, 2>& material);

    /// After `d`'s full preparation (`prep`, `state`, its VS/PS views): keep it as its key's template; `check` (verify:
    /// every draw, guarded: its samples) builds the draw from `e` as Prepare would and compares it with the walk's.
    void AfterFull(const Cp::DrawDescriptor& d, const EngineState& e,
                   GS::ProgramCache& programs, GS::GuestReader& memory, u64 targets, const GS::DrawPreparation& prep,
                   const GS::GraphicsPipelineState* state, const GS::StageView& vs, const GS::StageView& ps,
                   bool check);

    /// guarded: this engine build is one to check against a full preparation (every 64th).
    bool SampleCheck() { return mode_ == Mode::Guarded && (++sample_ & 63) == 0; }

    /// SSPC_CITY_GB_RUN_MATERIAL: a G-buffer run member's plan of `head`'s stage (the run's first draw's view) for its
    /// registers `ud`, from its engine record's CUE state `sh` through the stage map - what RedecodeStage decodes after
    /// walking the guest's tables again, without the walk (the native path's plans: the same as the walk's, verify).
    /// kPlanOk: `out` is the plan, of `head`'s module and bindings; kPlanOther: another module or other bindings (the
    /// member is refused - the walk would decode the same); kPlanNone: no map / not ready (the caller walks).
    static constexpr u8 kPlanOk = 0, kPlanOther = 1, kPlanNone = 2;
    u8 MemberPlan(const GS::StageView& head, const ShadowRef& sh, std::span<const u32, 16> ud,
                  GS::ResourceBindingPlan& out);
    /// SSPC_CITY_GB_RUN_MATERIAL=verify: MemberPlan's answer `result` / plan `mine` against the walk's (RedecodeStage:
    /// `walked` - it gave a plan of the same module and bindings - and `walk`), counted by part (Report).
    void CheckMemberPlan(u8 result, bool walked, const GS::ResourceBindingPlan& mine, const GS::ResourceBindingPlan& walk);

    std::string Report() const;

private:
    static constexpr u16 kZero = 0xffff; ///< a flattened dword the walk does not write (0 in both)

    /// Where a stage's flattened user data comes from in its CUE copy (one module's walk over one CUE layout).
    struct StageMap {
        const GS::Module* first = nullptr; ///< the layout's first module (whose walk PrepareStage runs)
        u64 layout = 0;                    ///< the CUE layout (GDR::StageShadow::layout)
        bool ok = false;
        u8 why = 0;       ///< not ok: 1 no walk / not ready, 2 the walk failed, 3 it read memory not of the copy,
                          ///< 4 it wrote the registers, 5 a dword of unknown origin, 6 past the copy, 7 a vertex V#
        u32 ud_read = 0;  ///< the registers the walk reads (RunStats::user_data_read)
        u32 flat_size = 0;
        std::vector<u16> flat;   ///< [16, flat_size): the copy's dword each is (kZero: not written)
        std::vector<u16> vertex; ///< per fetch attribute: the copy's dword its V# starts at
        /// `flat` as runs (the walk copies tables: long runs of consecutive dwords) - what BuildStage copies
        struct Run {
            u16 at = 0, from = 0, n = 0; ///< flat[at, at + n) = the copy's [from, from + n) (from kZero: zeros)
        };
        std::vector<Run> runs;
    };

    struct Key {
        u64 vs = 0, ps = 0;    ///< program addresses
        u64 pipeline_key = 0;  ///< the processor's hash of every pipeline-group register
        u64 targets = 0;       ///< the colour/depth target registers' hash (runtime parameters and pipeline read them)
        u64 vs_layout = 0, ps_layout = 0;
        bool operator==(const Key&) const = default;
    };
    /// One full preparation of a key: its modules (specialized on the plans' descriptors: a material's texture types
    /// select another module of the same programs), fetch shader and pipeline.
    struct Variant {
        GS::DrawPreparation prep;
        const GS::GraphicsPipelineState* state = nullptr;
        GS::StageView vs, ps; ///< the heads (their plans are each draw's)
        std::vector<u32> fetch_code; ///< its fetch shader's code up to its s_setpc_b64 (a new frame compares it)
    };
    /// What the variant search reads of a variant - kept apart from the variants (a template's first cache lines: the
    /// heads are read only of the variant a draw takes).
    struct Hot {
        u64 fetch = 0;        ///< the VS fetch shader's address (user data at the fetch registers)
        const GS::Module* vs = nullptr;
        const GS::Module* ps = nullptr;
        u64 vs_key = 0, ps_key = 0; ///< their specialization keys' hashes (0: not keyed when kept - compared in full)
        u32 fetch_reg = 0xff; ///< (0xff: no fetch shader)
    };
    static constexpr u32 kVariants = 16; ///< (a material's texture formats select modules: city keys have more than 4)
    struct Template {
        Key key;
        u64 epoch = 0;       ///< the program cache epoch its variants were validated in
        u32 n = 0, next = 0; ///< variants filled; the one a new variant replaces when all are
        /// variant 0's stage maps and VS first module (its fetch register): found at the first use after variant 0
        /// changed (nullptr: not yet)
        const StageMap* vm = nullptr;
        const StageMap* pm = nullptr;
        const GS::Module* vfirst = nullptr;
        std::array<Hot, kVariants> hot;
        std::array<Variant, kVariants> v;
    };
    /// A fetch shader's code as read in one program cache epoch (Revalidate: each address read once a frame).
    struct FetchSeen {
        u64 epoch = ~0ull;
        bool ok = false;
        std::vector<u32> code;
    };
    static constexpr size_t kMaxTemplates = 8192; ///< (more keys: all dropped, made again)

    static void Bump(std::atomic<u64>& c, u64 n = 1) {
        c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); // (one writer thread)
    }
    static Key KeyOf(const Cp::DrawDescriptor& d, const EngineState& e, u64 targets);
    static u64 HashOf(const Key& k);
    /// The VS fetch shader's address in `ud` (`first`: the VS layout's first module) and its register.
    static u64 FetchOf(const GS::Module* first, std::span<const u32, 16> ud, u32& reg);
    /// Variant 0 changed: its maps are found again at the next use.
    static void Forget(Template& t) {
        t.vm = t.pm = nullptr;
        t.vfirst = nullptr;
    }
    static void Keep(Template& t, u32 i, u64 fetch, u32 fetch_reg); ///< t.hot[i] from t.v[i]'s heads
    /// The fetch shader at `addr` (its address pair at `reg`) up to its s_setpc_b64, as the fetch cache reads it.
    static bool ReadFetch(GS::GuestReader& memory, u64 addr, u32 reg, std::vector<u32>& code);
    /// A template first used in a new frame: its variants' programs and fetch shaders validated again (see above),
    /// those changed dropped. false: none is left.
    bool Revalidate(Template& t, GS::ProgramCache& programs, GS::GuestReader& memory);

    /// The map of `v`'s stage for CUE state `sh` (made on first use). nullptr: the view has no first module yet.
    const StageMap* MapOf(const GS::StageView& v, const ShadowRef& sh);
    void MakeMap(StageMap& m, const GS::Module& first, const ShadowRef& sh);
    /// `out` = `head`'s stage for this draw, its plan from the CUE state `sh` through `m`, `key` its specialization
    /// (the module it needs). false: the head's module is not ready.
    bool BuildStage(const GS::StageView& head, const StageMap& m, const ShadowRef& sh,
                    std::span<const u32, 16> ud, GS::StageView& out, GS::SpecializationKey& key);
    /// A prepared stage's material id (Prepare); 0: an image the executor's memo does not keep (several views, FMask,
    /// storage).
    static u64 MaterialOf(const GS::StageView& v);
    /// verify: `built` against the walk's `full`, binding by binding; counts by part, logs the first ones. true: same.
    bool Compare(const GS::StageView& full, const GS::StageView& built, u32 stage);

    void Init(const char* env, const char* what, const char* detail);

    Mode mode_ = Mode::Off;
    const char* env_ = "SSPC_NATIVE_GBUFFER";
    const char* what_ = "native G-buffer 0x1a1770";
    bool compact_ = false; ///< R10's instance: its stages are in the compact ring (the report's capture counts)
    bool tripped_ = false; ///< guarded: a checked build differed - the engine path is off from then on
    u64 sample_ = 0;
    std::unordered_map<u64, std::unique_ptr<StageMap>> maps_;
    std::unordered_map<u64, Template> templates_;
    std::vector<std::array<u32, 4>> vreads_;
    std::vector<u32> fetch_code_;
    std::unordered_map<u64, FetchSeen> fetch_seen_; ///< by address
    GS::SpecializationKey key_vs_, key_ps_, key_member_;
    GS::StageView check_vs_, check_ps_; ///< verify: the engine builds compared with the walk's

    std::atomic<u64> draws_{0}, with_record_{0}, no_record_{0};
    std::atomic<u64> taken_{0};            ///< prepared from the engine (guarded / native)
    std::array<std::atomic<u64>, 12> refused_{}; ///< by reason (Report)
    std::array<std::atomic<u64>, 8> map_why_{};  ///< maps made, by StageMap::why (0: ok)
    std::atomic<u64> templates_made_{0}, variants_made_{0}, templates_dropped_{0};
    std::atomic<u64> revalidated_{0}, variants_changed_{0}, fetch_unread_{0}, fetch_reads_{0};
    // verify / guarded checks: stages compared, the same, different by part (flat, buffers, images, samplers,
    // vertices, heads)
    std::atomic<u64> compared_{0}, same_{0};
    std::array<std::atomic<u64>, 6> diff_{};
    std::atomic<u64> built_failed_{0}; ///< verify: a draw whose engine build was refused (why: refused_)
    std::array<std::atomic<u64>, 3> member_{}; ///< MemberPlan by result (kPlanOk, kPlanOther, kPlanNone)
    /// CheckMemberPlan: checked, the same; differing in flat, buffers, images, samplers, status counts; answers that
    /// differ (a plan the walk refused, a refusal of a plan the walk gave)
    std::array<std::atomic<u64>, 9> member_check_{};
    std::atomic<u64> logged_{0};
};

} // namespace SecondSon::Gpu
