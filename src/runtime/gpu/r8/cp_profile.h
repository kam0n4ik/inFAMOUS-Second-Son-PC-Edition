// SecondSonPC R8-A1 - the command processor's profiler (SSPC_R8_CP_PROFILE=1|2, off by default;
// docs/NATIVE_ENGINE_R8_CP_PROFILE.md, docs/NATIVE_ENGINE_R8_CORE.md §5.1).
//
// What it measures: where the processor's thread (SecondSonPC:GpuNative - PM4, registers, the shader frontend, the
// queue's records, its waits) and the recording thread (SecondSonPC:GpuRecord) spend each frame, by sections of an
// enumeration (Sec), not by strings.
//
//   exact sections   R8_CP_PROF(sec) - an RAII marker; every call is timed. The time is exclusive: a marker inside
//                    another one takes its time out of the outer one (a stack per thread, one __rdtsc() per transition),
//                    so a thread's sections sum to its wall time between two frames. Section 0 (Other) is what no
//                    marker covers. The frontend's own steps are not marked (shader/* keeps the shader cache's
//                    generation): its time is measured around the processor's calls into it.
//   sampled packets  R8_CP_PROF_PACKET() / R8_CP_PROF_DECODED(op, packet) in CommandProcessor::Step - every N-th packet
//                    (SSPC_R8_CP_PROFILE_SAMPLE, 32): its decoding (Next) and its execution's own time (what the exact
//                    sections inside it did not take), by opcode class, scaled by N. Too frequent for exact markers
//                    (~60 k packets a frame): this splits the exact section Packets.
//   light sections   R8_CP_PROF_LIGHT(l) - timed inside sampled packets only (inclusive, scaled by N): BuildDraw, the
//                    register file's writes, the register journal.
//
// Per frame: the processor's thread hands its counts to a row at each flip marker (R8_CP_PROF_FRAME); the recording
// thread when it next sees that frame number (R8_CP_PROF_FRAME_POLL, before each record) - a thread's own counts, no
// lock, no shared write on the hot path. The report (gpu_native.txt, "R8 CP profile") takes the frames of a time window
// (SSPC_R8_CP_PROFILE_WINDOW=<from>:<to> s of the frame log, 70:148 by default - the city stand's) and the whole run;
// SSPC_R8_CP_PROFILE_CSV=1 also writes logs\r8_cp_profile.csv (one row per frame).
//
// SSPC_R8_CP_PROFILE=2 adds the repetition census (task 4 of docs/NATIVE_ENGINE_R8_CP_PROFILE.md): each stage the
// frontend prepared compared with the last preparation of the same program (result and registers hashed), the
// frontend's time of the draws by class (both stages repeated / PS only / VS only / none), the stage copies whose view
// had not changed.
//
// Off: every marker is one load of a static bool and a predicted branch; nothing is written.
#pragma once

#include <intrin.h>

#include <filesystem>
#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::Cp {
struct DrawDescriptor;
}
namespace SecondSon::Gpu::Shader {
struct StageView;
}

namespace SecondSon::Gpu::CpProfile {

/// SSPC_R8_CP_PROFILE (read once, at static initialization): 0 off, 1 timings, 2 timings + the repetition census.
extern const u8 g_mode;
inline bool On() {
    return g_mode != 0;
}

enum class Role : u8 { None = 0, Cp = 1, Record = 2, Front = 3 }; ///< Front: the frontend thread (front mode)

/// The sections. Thread 1's first, thread 2's after T2Idle; a section's class (compute / sync / wait / idle) is in the
/// report's table (cp_profile.cpp kInfo).
enum class Sec : u8 {
    Other = 0,       ///< no marker (the backend loop's own code)
    LoopPre,         ///< the game's submissions taken (Backend::Apply)
    FrontInvalidate, ///< ShaderFrontend::Invalidate for CPU writes (Backend::FrontendInvalidate)
    CpuWrites,       ///< the guest's CPU writes applied to the caches (Backend::DrainCpuWrites)
    Packets,         ///< the queues' slices: PM4 decoding, the packet dispatcher, registers, small packets (sampled split)
    RoundEnd,        ///< a round's end without a drain (CP-9): completions applied
    Poll,            ///< the executor's poll after a drained round, completions applied
    Flush,           ///< the open batch submitted from the loop
    IdleBlocked,     ///< idle: a WAIT_REG_MEM blocked
    IdleGpu,         ///< idle: waiting for the GPU
    IdleEmpty,       ///< idle: nothing submitted
    JoinRun,         ///< a tagged draw joining the held run (ParallelCp::JoinRun; a refused one's CloseRun)
    ParDraw,         ///< a draw's record (BeginRecord, the descriptor's copy, RunLeaderOk, PushRecord)
    PrepDraw,        ///< ParallelCp::PrepareDraw's own part (filter, notes of writes, bookkeeping)
    GeoFast,         ///< ParallelCp::PrepareGeometryOnly (SSPC_CITY_GEOFAST; refusals included)
    Redecode,        ///< ParallelCp::RedecodeStage (Phase 6 / templates: a stage decoded again, or walked again)
    FrontUntagged,   ///< ShaderFrontend::PrepareDraw of a draw without an engine tag
    FrontMesh,       ///< ... tagged mesh 0x1a1960 (Phase 5)
    FrontShadow,     ///< ... tagged shadow 0x43d480 (Phase 5)
    FrontGBuffer,    ///< ... tagged G-buffer 0x1a1770 (Phase 6)
    FrontNative,     ///< the native G-buffer preparation of a 0x1a1770 draw (SSPC_NATIVE_GBUFFER: lookup, check, patch)
    CopyStage,       ///< a prepared stage view copied into its record
    ParDispatch,     ///< a dispatch's record
    FrontDispatch,   ///< ShaderFrontend::PrepareDispatch and the stage's copy
    FrontSync,       ///< a frontend read at an idle queue (ApplyPending) / after a drain: the executor's GPU-write check
    CbDma,           ///< ParallelCp::Dma (a DMA / WRITE_DATA report)
    CbCache,         ///< ParallelCp::CacheAction
    CbFence,         ///< ParallelCp::Fence
    CbAcquire,       ///< ParallelCp::MemoryAcquired (a met memory wait)
    CbWaitMet,       ///< ParallelCp::WaitMetByHeldFence
    CbCopy,          ///< ParallelCp::CopyOnTimeline
    CbWrite,         ///< ParallelCp::WriteOnTimeline (WRITE_DATA / DMA fill)
    CbZpass,         ///< ParallelCp::ZpassOnTimeline (an occlusion query's record)
    CbGds,           ///< ParallelCp::GdsOnTimeline
    CbSync,          ///< ParallelCp::Synchronize (the executor waits for the GPU; its drain is a section of its own)
    CbFlip,          ///< ParallelCp::Flip (the executor's flip on thread 1)
    MemRead,         ///< the processor's memory read (native_backend Memory::Read)
    MemSync,         ///< ... that waits for the GPU (Executor::SyncFromMemoryRead)
    PollRead,        ///< a WAIT_REG_MEM's memory read
    ApplyPending,    ///< ParallelCp::ApplyPending (the queue idle: journal, deferred invalidations, timeline snapshot)
    Journal,         ///< ParallelCp::ApplyJournal (the register journal replayed into the replica)
    WaitFull,        ///< thread 1 waits: the queue full
    WaitJournal,     ///< thread 1 waits: the journal full
    Drain0,          ///< thread 1 waits for thread 2 in a drain, by DrainReason (Drain0 + reason, 16 of them)
    DrainLast = Drain0 + 15,
    T2Idle,          ///< thread 2: waiting for records
    T2Draw,          ///< thread 2: a draw record (Executor::DrawPrepared / a batch / the run's first draw)
    T2Dispatch,      ///< thread 2: a dispatch record
    T2RoundEnd,      ///< thread 2: a round end (flush + poll)
    T2Flip,          ///< thread 2: a queued flip
    T2Other,         ///< thread 2: the other records (cache actions, fences, DMA, waits, copies, writes)
    T2Run,           ///< thread 2: a run's members (Executor::DrawRunMembers)
    T2Bind,          ///< thread 2: Executor::BindStage (descriptors of a stage)
    // the same three by the engine function that wrote the draw (Cp::DrawDescriptor::engine_run; T2Draw, T2Run and
    // T2Bind are the untagged draws' then)
    T2DrawMesh,      ///< thread 2: a draw record of a 0x1a1960 draw
    T2DrawShadow,    ///< ... of a 0x43d480 draw
    T2DrawGBuffer,   ///< ... of a 0x1a1770 draw
    T2RunMesh,       ///< thread 2: a run's members, the run of a 0x1a1960 draw
    T2RunShadow,     ///< ... of a 0x43d480 draw
    T2RunGBuffer,    ///< ... of a 0x1a1770 draw
    T2BindMesh,      ///< thread 2: BindStage for a 0x1a1960 draw
    T2BindShadow,    ///< ... for a 0x43d480 draw
    T2BindGBuffer,   ///< ... for a 0x1a1770 draw
    // a 0x1a1770 draw's record by its stage (Executor::RecordDraw; SSPC_NATIVE_GBUFFER's thread 2 work - BindStage
    // is T2BindGBuffer inside them)
    T2GbHead,        ///< thread 2, a 0x1a1770 draw's record: its checks and pipeline lookup
    T2GbTargets,     ///< ... its targets (the target cache, or images found)
    T2GbBind,        ///< ... around its stages' BindStage (push data, instancing, order notes)
    T2GbVertex,      ///< ... its vertex inputs, index buffer (batch geometry)
    T2GbLayout,      ///< ... barriers, image layouts, descriptor fix-ups
    T2GbEmit,        ///< ... the commands: rendering, pipeline, descriptors, push constants, dynamic state, the draw
    T2GbBindImages,  ///< BindStage of a 0x1a1770 draw: its images (T2BindGBuffer keeps the buffers)
    T2GbBindSamplers, ///< ... its samplers
    // the native G-buffer preparation (FrontNative) by its part
    FrontNativeFind,   ///< the template, its validation in a new frame, the stage maps
    FrontNativeDecode, ///< a stage's plan: the flattened data from the CUE state, DecodeResources
    FrontNativeKey,    ///< its specialization key, the variant of these modules, EndOf
    JoinPsPlan,        ///< JoinRun: a G-buffer member's PS plan from its CUE state (NativeGBuffer::MemberPlan)
    T2IdleHeld,        ///< thread 2 waiting at a held run's first draw (front mode: the frontend thread joining members)
    // R10 (SSPC_R10_NATIVE_DRAWS): the engine-state preparation of a 0x1a1960 / 0x43d480 draw (and, verify, its check)
    FrontNativeMesh,
    FrontNativeShadow,
    Count,
};
inline constexpr u32 kSections = static_cast<u32>(Sec::Count);
/// Drain0 + reason (Native::DrainReason, < 16).
inline constexpr Sec Drain(u32 reason) {
    return static_cast<Sec>(static_cast<u32>(Sec::Drain0) + (reason & 15));
}
/// FrontUntagged + the draw's engine tag (Cp::DrawDescriptor::engine_run & 3).
inline constexpr Sec Front(u32 engine_run) {
    return static_cast<Sec>(static_cast<u32>(Sec::FrontUntagged) + (engine_run & 3));
}
/// Thread 2: the engine tag of the draw record it executes (Cp::DrawDescriptor::engine_run & 3; 0 for any other
/// record), set by ParallelCp::Execute - BindStage and DrawRunMembers file their time under it.
inline thread_local constinit u8 t_source = 0;
/// SSPC_R10_T2_PARTS (read once, at static initialization): the engine source whose draw records thread 2's part
/// sections (T2Gb*: the record's stages, BindStage's images and samplers) split - 3 G-buffer 0x1a1770 (the default,
/// R8's), 2 shadow 0x43d480 ("shadow"), 1 mesh 0x1a1960 ("mesh"), 0 untagged draws ("untagged").
extern const u8 g_parts_source;
/// Thread 2: whether the draw being recorded is of g_parts_source (its record and BindStage split by parts).
inline bool PartsSource() {
    return g_mode != 0 && (t_source & 3) == g_parts_source;
}
/// `untagged` for an untagged draw, else the section of the source among the three after `mesh`.
inline constexpr Sec BySource(Sec untagged, Sec mesh, u32 source) {
    return (source & 3) == 0 ? untagged : static_cast<Sec>(static_cast<u32>(mesh) + (source & 3) - 1);
}
/// Thread 2's section for a record of ParallelCp::Kind `kind` (Draw 0, Dispatch 1, ..., Flip 10, RoundEnd 11).
inline constexpr Sec RecordSection(u32 kind) {
    return kind == 0 ? Sec::T2Draw : kind == 1 ? Sec::T2Dispatch : kind == 10 ? Sec::T2Flip
           : kind == 11 ? Sec::T2RoundEnd : Sec::T2Other;
}

enum class Light : u8 { BuildDraw, SetRegs, JournalWrite, Count };
inline constexpr u32 kLights = static_cast<u32>(Light::Count);
/// The sampled packets' classes (0: the decoding, Next).
inline constexpr u32 kPacketClasses = 17;
/// After the classes: 8 kinds of NOP, 8 ranges of SET_SH_REG (their sampled packets' ticks and counts), then the same
/// 8 ranges' register dwords (packet_n only).
inline constexpr u32 kNopKinds = 8, kShRanges = 8;
inline constexpr u32 kNopSlot = kPacketClasses, kShSlot = kNopSlot + kNopKinds, kShDwordSlot = kShSlot + kShRanges;
inline constexpr u32 kPacketSlots = kShDwordSlot + kShRanges;

inline constexpr u32 kDepth = 32;

/// A thread's counts (thread_local, zero-initialized: no initialization guard on the hot path).
struct ThreadAcc {
    bool on;          ///< the thread registered with the profiler on
    u8 role;
    bool sampling;    ///< inside a sampled packet
    u8 depth;         ///< stack[depth] is the current section (stack[0] = Other)
    u32 sample_count; ///< packets since the last sampled one
    u32 frame;        ///< the processor's frame these counts belong to
    u32 flushes;      ///< rows handed over (a sampled packet across one is dropped)
    u64 last;        ///< TSC of the last transition
    u64 last_incl;    ///< the inclusive ticks of the last section left (the repetition census reads it)
    u64 overflow;     ///< markers deeper than kDepth (not timed)
    u8 stack[kDepth];
    u64 entered[kDepth];
    u64 self[kSections];
    u64 incl[kSections];
    u32 calls[kSections];
    u64 light_ticks[kLights];
    u32 light_n[kLights];
    u64 packet_ticks[kPacketSlots];
    u32 packet_n[kPacketSlots];
};
extern thread_local constinit ThreadAcc t_acc;

inline void Enter(Sec s) {
    ThreadAcc& t = t_acc;
    if (!t.on) {
        return;
    }
    const u64 now = __rdtsc();
    t.self[t.stack[t.depth]] += now - t.last;
    t.last = now;
    if (t.depth + 1u >= kDepth) {
        ++t.overflow;
        return;
    }
    ++t.depth;
    t.stack[t.depth] = static_cast<u8>(s);
    t.entered[t.depth] = now;
    ++t.calls[static_cast<u32>(s)];
}
inline void Leave() {
    ThreadAcc& t = t_acc;
    if (!t.on || t.depth == 0) {
        return;
    }
    const u64 now = __rdtsc();
    const u8 s = t.stack[t.depth];
    t.self[s] += now - t.last;
    t.last_incl = now - t.entered[t.depth];
    t.incl[s] += t.last_incl;
    t.last = now;
    --t.depth;
}

class Scope {
public:
    explicit Scope(Sec s) : on_(g_mode != 0) {
        if (on_) {
            Enter(s);
        }
    }
    ~Scope() {
        if (on_) {
            Leave();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const bool on_;
};

/// A light section: timed only inside a sampled packet (its inclusive ticks).
class LightScope {
public:
    explicit LightScope(Light l) {
        if (g_mode != 0 && t_acc.sampling) {
            l_ = static_cast<u8>(l);
            t0_ = __rdtsc();
        }
    }
    ~LightScope() {
        if (t0_ != 0) {
            t_acc.light_ticks[l_] += __rdtsc() - t0_;
            ++t_acc.light_n[l_];
        }
    }
    LightScope(const LightScope&) = delete;
    LightScope& operator=(const LightScope&) = delete;

private:
    u64 t0_ = 0;
    u8 l_ = 0;
};

/// A packet of CommandProcessor::Step: every N-th one (of the thread's packets inside the Packets section) timed.
class PacketSample {
public:
    PacketSample() {
        if (g_mode != 0) {
            Begin();
        }
    }
    void Decoded(u8 opcode, std::span<const u32> packet) {
        if (active_) {
            OnDecoded(opcode, packet);
        }
    }
    ~PacketSample() {
        if (active_) {
            End();
        }
    }
    PacketSample(const PacketSample&) = delete;
    PacketSample& operator=(const PacketSample&) = delete;

private:
    void Begin();
    void OnDecoded(u8 opcode, std::span<const u32> packet);
    void End();
    u64 a_ = 0;          ///< the Packets section's ticks at the packet's start (charged + pending)
    u64 b_ = 0;          ///< ... after its decoding
    u32 flushes_ = 0;    ///< ThreadAcc::flushes at the start
    bool active_ = false;
    bool decoded_ = false;
    u8 cls_ = 0;
    u8 sub_ = 0;  ///< the NOP kind's / SET_SH_REG range's slot (0: none)
    u16 dw_ = 0;  ///< SET_SH_REG: its register dwords
};

/// The calling thread is `role`'s (the start of Backend::Loop / ParallelCp::Loop): its counts are kept from now on.
void RegisterThread(Role role);
/// The processor's thread reached a flip marker: its counts so far are the frame's row.
void FrameEnd();
/// The recording thread, before a record: the processor's frame moved on - its counts so far are that frame's row.
void FramePollSlow();
inline void FramePoll() {
    if (g_mode != 0 && t_acc.on) {
        FramePollSlow();
    }
}

/// SSPC_R8_CP_PROFILE=2: a draw the frontend prepared (its VS / PS views as the frontend left them); the frontend's
/// time is the last section left (t_acc.last_incl).
void NotePreparedSlow(const Cp::DrawDescriptor& draw, const Gpu::Shader::StageView& vs, const Gpu::Shader::StageView& ps);
inline void NotePrepared(const Cp::DrawDescriptor& draw, const Gpu::Shader::StageView& vs, const Gpu::Shader::StageView& ps) {
    if (g_mode == 2) {
        NotePreparedSlow(draw, vs, ps);
    }
}
/// SSPC_R8_CP_PROFILE=2: a stage view copied into a record (its time: the last section left - CopyStage); `unchanged`:
/// the frontend's StageGeneration had not moved since the last copy (FB-6's case).
void NoteCopySlow(bool unchanged);
inline void NoteCopy(bool unchanged) {
    if (g_mode == 2) {
        NoteCopySlow(unchanged);
    }
}

/// The "R8 CP profile" part of gpu_native.txt (empty when off).
std::string Report();
/// SSPC_R8_CP_PROFILE_CSV=1: <dir>/r8_cp_profile.csv, one row per frame (nothing when off).
void WriteCsv(const std::filesystem::path& dir);

} // namespace SecondSon::Gpu::CpProfile

#define SSPC_R8_PROF_CAT2(a, b) a##b
#define SSPC_R8_PROF_CAT(a, b) SSPC_R8_PROF_CAT2(a, b)
/// An exact section to the end of the enclosing block.
#define R8_CP_PROF(sec) \
    const ::SecondSon::Gpu::CpProfile::Scope SSPC_R8_PROF_CAT(r8_prof_scope_, __LINE__)(sec)
/// An exact section around a statement (no early exit in between): Enter ... Leave.
#define R8_CP_PROF_ENTER(sec)                         \
    do {                                              \
        if (::SecondSon::Gpu::CpProfile::On()) {      \
            ::SecondSon::Gpu::CpProfile::Enter(sec);  \
        }                                             \
    } while (false)
#define R8_CP_PROF_LEAVE()                            \
    do {                                              \
        if (::SecondSon::Gpu::CpProfile::On()) {      \
            ::SecondSon::Gpu::CpProfile::Leave();     \
        }                                             \
    } while (false)
/// A light section to the end of the enclosing block.
#define R8_CP_PROF_LIGHT(l) \
    const ::SecondSon::Gpu::CpProfile::LightScope SSPC_R8_PROF_CAT(r8_prof_light_, __LINE__)(l)
/// CommandProcessor::Step: the packet's sample (before Next) and its opcode (after Next).
#define R8_CP_PROF_PACKET() ::SecondSon::Gpu::CpProfile::PacketSample r8_prof_packet_
#define R8_CP_PROF_DECODED(op, packet) r8_prof_packet_.Decoded(op, packet)
#define R8_CP_PROF_THREAD(role)                                                                     \
    do {                                                                                            \
        if (::SecondSon::Gpu::CpProfile::On()) {                                                    \
            ::SecondSon::Gpu::CpProfile::RegisterThread(::SecondSon::Gpu::CpProfile::Role::role);   \
        }                                                                                           \
    } while (false)
#define R8_CP_PROF_FRAME()                            \
    do {                                              \
        if (::SecondSon::Gpu::CpProfile::On()) {      \
            ::SecondSon::Gpu::CpProfile::FrameEnd();  \
        }                                             \
    } while (false)
#define R8_CP_PROF_FRAME_POLL() ::SecondSon::Gpu::CpProfile::FramePoll()
/// SSPC_R8_CP_PROFILE=2: a draw the frontend prepared (the views' expressions are evaluated only then).
#define R8_CP_PROF_PREPARED(draw, vs, ps)                                     \
    do {                                                                      \
        if (::SecondSon::Gpu::CpProfile::g_mode == 2) {                       \
            ::SecondSon::Gpu::CpProfile::NotePreparedSlow(draw, vs, ps);      \
        }                                                                     \
    } while (false)
/// SSPC_R8_CP_PROFILE=2: a stage view copied (right after CopyStage).
#define R8_CP_PROF_COPY(unchanged) ::SecondSon::Gpu::CpProfile::NoteCopy(unchanged)
