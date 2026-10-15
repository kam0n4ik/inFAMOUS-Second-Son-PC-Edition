// SecondSonPC — SecondSonPC's own GPU command processor (Phase 4, P4.2; docs/PHASE4_P42_COMMAND_PROCESSOR.md).
//
// Takes the game's submissions (graphics command buffers; compute ring spans), reads the PM4 packets itself through
// CommandMemory (bounded reads into its own buffers: it never dereferences a guest address), follows indirect
// buffers, keeps the register file (cp_registers.h) and each queue's compute registers, and turns draws and dispatches
// into descriptors (DrawDescriptor, DispatchDescriptor) handed to GpuWork — the GPU that will execute them (the Vulkan
// backend from P4.3/P4.4; a test or the shadow validation now). It renders nothing itself.
//
// Two modes, chosen at construction:
//   * Shadow   — beside another command processor that executes the same stream (the bridge's Liverpool): the state
//                and the descriptions only. No guest memory write, no wait, no fence, no interrupt, no DMA, no ring
//                read pointer update: whatever the packet would do outside the processor is reported (GpuWork), never
//                done a second time.
//   * Isolated — the only command processor of its memory: the packets' CPU-side effects are executed. Writes of
//                WRITE_DATA and DMA_DATA happen when the packet is processed (the hardware's micro engine does them in
//                stream order); end-of-pipe/end-of-shader/release writes wait for the GPU work before them (a fence
//                after a draw is written when GpuWork reports that draw complete, CompleteWork) and are written at once
//                only when nothing is outstanding; WAIT_REG_MEM blocks the queue until its condition holds. Nothing is
//                reported done that was not done.
//
// One thread owns a CommandProcessor (no locking inside); submissions from other threads are queued by the owner.
// Unknown packets, forms the processor does not implement, truncated packets, unreadable or too deep indirect buffers
// and runaway streams stop the queue's submission with a CpError naming the packet (never a read out of bounds,
// never a skipped packet passed off as executed).
#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "engine/render/native_render_ops.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/pm4.h"

namespace SecondSon::Gpu::Cp {

enum class Mode : u8 { Shadow, Isolated };

/// The guest memory as the command processor reads and writes it (the 40-bit GPU address space).
class CommandMemory {
public:
    virtual ~CommandMemory() = default;
    /// [addr, addr + bytes) into `dst`; false if any part is not readable (nothing guaranteed in `dst` then).
    virtual bool Read(u64 addr, void* dst, u64 bytes) = 0;
    /// Isolated mode only; false if any part is not writable (nothing written then).
    virtual bool Write(u64 addr, const void* src, u64 bytes) = 0;
    /// Memory no GPU work reads or writes (VideoOut's buffer labels): a WRITE_DATA there lands when the processor
    /// reaches it, as the hardware's micro engine writes it - not after the earlier GPU work (P4.6).
    virtual bool Unordered(u64 /*addr*/, u64 /*bytes*/) { return false; }
};

inline constexpr u32 kGraphicsQueue = 0;
inline constexpr u32 kMaxComputeQueues = 64; ///< Gnm's compute queue ids 1..64
inline constexpr u32 kNumQueues = 1 + kMaxComputeQueues;
inline constexpr u32 kGdsBytes = 64 * 1024;

enum class Stage : u8 { Ps, Vs, Gs, Es, Hs, Ls, Count };

struct ShaderRef {
    u64 address = 0; ///< (PGM_HI:PGM_LO) << 8; the program's content is not identified here (P4.3)
    u32 rsrc1 = 0;
    u32 rsrc2 = 0;
};

enum class DrawKind : u8 { Index2, IndexOffset2, IndexAuto, IndexIndirect, Indirect };
std::string_view DrawKindName(DrawKind kind);

/// P5.3 Native Render Frontend: a stage's resource table as the engine describes it (its draw operation's
/// StageTable, engine/render/native_render_ops.h) - the user-data SGPR pairs that point into the snapshot of the
/// constant update engine's stage shadow, the dwords past its start each points at, and the snapshot's extent. Only
/// where the pointers sit and how large the table is: its content is read from guest memory by the frontend itself.
struct EngineTable {
    u32 dwords = 0; ///< 0: no table (the stage reads nothing through one)
    u8 refs = 0;
    std::array<u8, 7> sgpr{};
    std::array<u16, 7> offset{};
};
inline constexpr s32 kNoEnginePass = -100;

/// A draw as the GPU will execute it: the draw packet's parameters and the state it sees. Built from the register file;
/// the parts that cost more than a copy (the pipeline key) are rebuilt only when their groups changed.
struct DrawDescriptor {
    u64 seq = 0;         ///< the queue's work sequence number (1, 2, ...)
    u64 packet_addr = 0; ///< guest address of the draw packet
    u32 queue = 0;
    DrawKind kind = DrawKind::Index2;
    bool indexed = false;
    bool indirect = false;    ///< the GPU reads the draw arguments from args_addr when it executes the draw
    u32 index_count = 0;      ///< VGT_NUM_INDICES (direct draws)
    u32 instance_count = 0;   ///< VGT_NUM_INSTANCES (0 draws one instance on the hardware)
    u64 index_base = 0;       ///< VGT_DMA_BASE_HI:VGT_DMA_BASE, bytes
    u32 index_type = 0;       ///< VGT_DMA_INDEX_TYPE
    u32 max_index_size = 0;   ///< VGT_DMA_MAX_SIZE
    u32 index_offset = 0;     ///< DRAW_INDEX_OFFSET_2's first index (0 otherwise)
    u32 vgt_index_offset = 0; ///< VGT_INDX_OFFSET (added to each index)
    u32 draw_initiator = 0;
    u32 primitive_type = 0; ///< VGT_PRIMITIVE_TYPE
    u32 stages_enable = 0;  ///< VGT_SHADER_STAGES_EN
    u64 args_addr = 0;      ///< indirect: SET_BASE's draw-indirect base + the packet's data offset
    u16 base_vtx_loc = 0;   ///< indirect: SH register offsets the GPU writes the base vertex / start instance to
    u16 start_inst_loc = 0;
    u8 active_stages = 0; ///< bits of Stage: VS and PS always; ES/GS, LS/HS when VGT_SHADER_STAGES_EN enables them
    std::array<ShaderRef, static_cast<size_t>(Stage::Count)> shaders{};
    std::array<std::array<u32, 16>, static_cast<size_t>(Stage::Count)> user_data{}; ///< active stages only
    std::array<u16, static_cast<size_t>(Stage::Count)> inline_sharps{}; ///< user data slots set right after a
                                                                         ///< "V#/T#/S# in user data" NOP marker
    u64 pipeline_key = 0;     ///< hash of every register of the pipeline groups (kPipelineGroups)
    u32 pipeline_version = 0; ///< bumps whenever a pipeline group changed since the previous draw
    u32 dynamic_version = 0;  ///< ... a dynamic-state group (viewport, scissor, blend constants, stencil ref, bias)
    u32 targets_version = 0;  ///< ... the colour/depth targets
    GroupMask changed = 0;    ///< groups changed since the previous draw
    std::array<u32, 8> cb_extent{}; ///< the driver's size hints after a colour target's setup (0: none)
    u32 db_extent = 0;
    /// P5.3: the engine described this draw (a draw marker of SecondSonPC before it, or the marker that replaced its
    /// packet): its render pass (0..40, -2 pre, -3 post, -4 deformation) and its VS/PS resource tables.
    /// kNoEnginePass: a draw of the game's PM4 alone.
    s32 engine_pass = kNoEnginePass;
    EngineTable engine_vs, engine_ps;
};

struct DispatchDescriptor {
    u64 seq = 0;
    u64 packet_addr = 0;
    u32 queue = 0;
    bool indirect = false; ///< the GPU reads the group counts from indirect_addr when it executes the dispatch
    bool patched = false;  ///< the counts were written into the packet by an earlier DMA_DATA of the stream
    u64 indirect_addr = 0;
    std::array<u32, 3> groups{};     ///< COMPUTE_DIM_X/Y/Z (direct)
    u32 initiator = 0;               ///< COMPUTE_DISPATCH_INITIATOR
    std::array<u32, 3> threads{};    ///< COMPUTE_NUM_THREAD_X/Y/Z
    ShaderRef shader;                ///< COMPUTE_PGM_LO/HI, RSRC1/RSRC2
    std::array<u32, 16> user_data{}; ///< COMPUTE_USER_DATA_0..15
    u32 program_version = 0;         ///< the queue's compute program registers' version
    u32 user_data_version = 0;
};

/// A fence: EVENT_WRITE_EOP, EVENT_WRITE_EOS, RELEASE_MEM.
enum class FenceKind : u8 { EndOfPipe, EndOfShader, Release };
struct FenceOp {
    FenceKind kind = FenceKind::EndOfPipe;
    u32 queue = 0;
    u64 packet_addr = 0;
    u32 event_type = 0;
    u32 data_sel = 0; ///< EOP/release: 0 none, 1 32-bit, 2 64-bit, 3 GPU clock, 4 GPU counter, 5 GDS (release)
    u32 int_sel = 0;
    u32 eos_command = 0; ///< EOS: 1 GDS store, 2 signal fence
    u64 addr = 0;
    u64 data = 0;       ///< the value written (Isolated: once written; with a clock: the sample)
    u32 gds_offset = 0; ///< bytes
    u32 gds_dwords = 0;
    u64 after_seq = 0;     ///< the queue's work it waits for (0: none outstanding)
    bool deferred = false; ///< Isolated: held until CompleteWork(queue, after_seq)
    bool written = false;  ///< Isolated: the memory write was done
    /// A held clock fence (data_sel 3/4): how long before its write the GPU passed the fence's place in the stream
    /// (the executor's timestamp there; P4.6 night, SSPC-020) - the clock written is the sample minus this. ~0: none.
    u64 clock_age_ns = ~0ull;
};

/// ACQUIRE_MEM: caches over a range made coherent before what follows (a barrier for the GPU); EVENT_WRITE of a
/// flush/partial-flush event (event != 0, no range).
struct CacheActionOp {
    u32 queue = 0;
    u64 packet_addr = 0;
    u32 event = 0; ///< EVENT_WRITE: VGT_EVENT_INITIATOR (type | index << 8); 0 for ACQUIRE_MEM
    u32 coher_cntl = 0;
    u64 base = 0; ///< bytes
    u64 size = 0; ///< bytes (the packet's size << 8)
    u64 after_seq = 0;
};

/// The flip marker (a NOP the driver's SubmitAndFlip leaves at the end of the frame's last command buffer).
struct FlipMarker {
    u32 queue = 0;
    u64 packet_addr = 0;
    u64 after_seq = 0; ///< the frame's work: everything handed to GpuWork before the marker
};

enum class DmaKind : u8 { MemoryToMemory, DataToMemory, MemoryToGds, DataToGds, GdsToMemory, ReadTagsPrefetch };
std::string_view DmaKindName(DmaKind kind);
struct DmaOp {
    DmaKind kind = DmaKind::MemoryToMemory;
    u32 queue = 0;
    u64 packet_addr = 0;
    u64 src = 0; ///< address, GDS byte offset or the fill data
    u64 dst = 0;
    u32 bytes = 0;
    bool command_patch = false; ///< the destination is a packet of the stream not yet processed
    bool executed = false;      ///< Isolated: done
    bool timeline = false;      ///< Isolated: recorded on the GPU's timeline by the executor (not copied by the CPU)
};

/// The GPU side: what the command processor hands on, in stream order (the owner's thread).
class GpuWork {
public:
    virtual ~GpuWork() = default;
    virtual void Draw(const DrawDescriptor& draw) = 0;
    virtual void Dispatch(const DispatchDescriptor& dispatch) = 0;
    virtual void CacheAction(const CacheActionOp&) {}
    virtual void Flip(const FlipMarker&) {}
    /// P5.3D A/B image comparison (SSPC_P53_CAPTURE_PASSES): the targets render pass `pass` wrote, as they are where
    /// its draws end in the stream (the next pass's first draw marker); how many of its draws ran natively and how many
    /// from the game's PM4. Diagnostics; a sink may ignore it.
    virtual void CaptureTargets(const u64* /*addresses*/, u32 /*count*/, s32 /*pass*/, u32 /*native_draws*/,
                                u32 /*pm4_draws*/) {}
    virtual void Fence(const FenceOp&) {} ///< Shadow: what would be written; Isolated: when held, and when written
    virtual void Dma(const DmaOp&) {}     ///< Shadow: what would be copied; Isolated: what was copied
    virtual void Interrupt(u32 /*queue*/, FenceKind) {}
    /// Isolated: called before the processor makes a CPU-side memory effect of a packet (DMA_DATA other than the
    /// prefetch, WRITE_DATA). An executor that defers GPU work (records it, submits later) completes everything handed
    /// on so far here, so the effect lands after the earlier work in stream order, as on the hardware (P4.4). It must
    /// not call back into the processor.
    virtual void Synchronize(u32 /*queue*/) {}
    /// Isolated: a packet's memory write with known bytes (WRITE_DATA, DMA_DATA fill) recorded on the GPU's timeline,
    /// after the GPU work before it, instead of Synchronize + a CPU write (P4.4). `seq` is the queue's work number the
    /// write takes when accepted (fences after it wait for it). true: recorded — the processor writes nothing itself.
    virtual bool WriteOnTimeline(u32 /*queue*/, u64 /*seq*/, u64 /*dst*/, const void* /*data*/, u64 /*bytes*/) {
        return false;
    }
    /// Isolated: a DMA_DATA memory-to-memory copy recorded on the GPU's timeline after the GPU work before it, instead
    /// of Synchronize + a CPU copy (P4.4). true: recorded - the processor copies nothing itself.
    virtual bool CopyOnTimeline(u32 /*queue*/, u64 /*seq*/, u64 /*src*/, u64 /*dst*/, u64 /*bytes*/) { return false; }
    /// Isolated: a DMA_DATA with GDS on one side recorded on the GPU's timeline (GDS is the executor's buffer, which the
    /// shaders bind and Options::gds maps): DataToGds fills `pattern`, MemoryToGds copies from `memory`, GdsToMemory to
    /// it; `gds_offset` in bytes. true: recorded - the processor touches neither side (P4.6).
    virtual bool GdsOnTimeline(u32 /*queue*/, u64 /*seq*/, DmaKind /*kind*/, u64 /*memory*/, u32 /*gds_offset*/,
                               u32 /*bytes*/, u32 /*pattern*/) {
        return false;
    }
    /// Isolated: a WAIT_REG_MEM on memory was met (`blocked`: it had blocked before). The guest's CPU writes that came
    /// before the value it waited for (data published with a flag) belong to the work after it: an executor holding
    /// copies of guest memory applies the CPU writes seen so far before recording more (P4.5).
    virtual void MemoryAcquired(u32 /*queue*/, bool /*blocked*/, u64 /*poll_address*/) {}
    /// Isolated with Options::waits_by_held_fences: a WAIT_REG_MEM was met in the GPU's order - a fence held for work
    /// already handed on will write a value meeting it (EVENT_WRITE_EOS then a wait for it: the GPU waiting for its own
    /// shaders). The executor orders everything after it after all earlier work (a full barrier); the fence's memory is
    /// still written only when that work completed (P4.6).
    virtual void WaitMetByHeldFence(u32 /*queue*/) {}
};

/// Before/after every packet (validation; nullptr: none).
class PacketObserver {
public:
    virtual ~PacketObserver() = default;
    virtual void BeforePacket(u32 queue, u64 addr, std::span<const u32> packet) = 0;
    virtual void AfterPacket(u32 queue, u64 addr, std::span<const u32> packet) = 0;
};

enum class CpErrorKind : u8 {
    None,
    Truncated,          ///< the packet is longer than what is left of its buffer
    ReservedType,       ///< type-1 header
    Type0,              ///< type-0 packet (register writes by address: not used by the PS4 driver)
    UnknownOpcode,      ///< an opcode SecondSonPC has no name for
    UnsupportedOpcode,  ///< known, not implemented by this command processor
    UnsupportedForm,    ///< a field value the command processor does not implement (named in the detail)
    ShortPacket,        ///< shorter than its opcode's fixed fields
    RegisterRange,      ///< SET_*_REG outside the register windows
    IndirectUnreadable, ///< an indirect buffer or submission the memory cannot read
    IndirectTooDeep,
    BudgetExceeded, ///< more dwords than the submission's budget (a looping chain)
    MemoryUnreadable,
    MemoryWriteFailed,
    GdsRange,
    FenceQueueFull,
    QueueNotMapped,
    QueueFull,
    Count,
};
std::string_view CpErrorName(CpErrorKind kind);

struct CpError {
    CpErrorKind kind = CpErrorKind::None;
    u32 queue = 0;
    u32 depth = 0;   ///< 0: the submission, 1..: inside indirect buffers
    u64 address = 0; ///< the packet's guest address
    u8 opcode = 0;
    u32 dwords = 0;
    std::string detail;
    std::string Text() const;
};

enum class StepStatus : u8 {
    Packet,  ///< one packet processed
    Idle,    ///< nothing to process
    Blocked, ///< Isolated: a WAIT_REG_MEM whose condition does not hold (the packet stays next)
    Faulted, ///< the packet failed (LastError); the submission was dropped
};

struct Counters {
    std::array<u64, 256> graphics_packets{}; ///< per opcode
    std::array<u64, 256> compute_packets{};
    u64 type2 = 0;
    u64 draws = 0, dispatches = 0, dispatches_disabled = 0, flips = 0;
    u64 fences = 0, fences_deferred = 0, fences_written = 0, fences_dropped = 0;
    u64 waits = 0, waits_blocked = 0, cache_actions = 0;
    /// Isolated: how long the WAIT_REG_MEM that blocked stayed blocked (<50 us, 100, 200, 500 us, 1, 1.5, 2, 5, 10 ms,
    /// longer; P4.6: the GPU thread's polling granularity)
    std::array<u64, 10> block_hist{};
    u64 waits_met_held = 0; ///< Isolated: WAIT_REG_MEM met in the GPU's order by a held fence (waits_by_held_fences)
    u64 timeline_writes = 0; ///< Isolated: WRITE_DATA / DMA fills the executor recorded on the GPU's timeline
    u64 timeline_copies = 0; ///< Isolated: DMA_DATA memory-to-memory copies the executor recorded on the timeline
    u64 timeline_gds = 0;    ///< Isolated: DMA_DATA to/from GDS the executor recorded on the timeline (P4.6)
    u64 sync_write_data = 0;                 ///< Isolated: WRITE_DATA waits for the GPU (not on the timeline)
    std::array<u64, 8> sync_dma{};           ///< Isolated: DMA_DATA waits for the GPU, by DmaKind
    std::array<u64, 8> sync_dma_ns{};        ///< ... their time (P4.6)
    u64 sync_write_data_ns = 0;
    u64 unordered_writes = 0; ///< Isolated: WRITE_DATA to memory no GPU work reads (VideoOut labels): written at once
    std::array<u64, 4> sync_dma_m2m_size{};  ///< memory-to-memory sizes: <=256 B, <=4 KiB, <=64 KiB, larger
    u64 dma = 0, dma_bytes = 0, dma_command_patches = 0, dma_prefetch = 0;
    u64 writes = 0, write_bytes = 0;
    u64 indirect_buffers = 0, submissions = 0, ring_carries = 0;
    u64 extent_hints = 0, extent_hints_missing = 0;
    u64 reg_index_nonzero = 0; ///< SET_CONTEXT/UCONFIG_REG with the CI "index" field set (ignored, counted)
    u64 gds_fences = 0;        ///< EOS GDS stores and RELEASE_MEM GDS data (offset unit unverified, docs §3)
    u64 pipeline_key_rebuilds = 0;
    u64 buffer_growths = 0; ///< the processor's own allocations: a buffer outgrew its capacity
    std::array<u64, static_cast<size_t>(CpErrorKind::Count)> errors{};
};

struct Options {
    Mode mode = Mode::Isolated;
    CommandMemory* memory = nullptr; ///< required
    GpuWork* work = nullptr;         ///< required
    u32 max_ib_depth = 4;            ///< indirect buffers nested below a submission
    u64 max_dwords_per_submission = 64ull << 20; ///< with its indirect buffers: a looping chain ends here
    bool resume_after_fault = false; ///< take the queue's next submission after a fault (Shadow); else stay faulted
    /// GPU clocks for EOP/RELEASE_MEM data (3: 64-bit GPU clock, 4: GPU counter); nullptr: the host's steady clock (ns)
    u64 (*clock)(void* ctx, u32 data_sel) = nullptr;
    void* clock_ctx = nullptr;
    /// GDS (kGdsBytes) the processor's DMA and fences use; nullptr: its own. An executor whose shaders bind GDS passes
    /// its host-visible GDS buffer, so the CP side and the shaders see one GDS.
    u8* gds = nullptr;
    /// Isolated: a WAIT_REG_MEM on memory that a held fence (any queue: the executor records every queue in one
    /// order) will meet passes at once, GpuWork::WaitMetByHeldFence ordering the work after it on the GPU - instead of
    /// the processor stopping until the GPU completed everything before the fence (P4.6).
    bool waits_by_held_fences = false;
    /// Isolated: DMA_DATA to/from GDS offered to GpuWork::GdsOnTimeline (else Synchronize and the CPU copies; P4.6).
    bool gds_on_timeline = false;
};

class CommandProcessor {
public:
    explicit CommandProcessor(const Options& options);
    ~CommandProcessor();
    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    Mode GetMode() const {
        return options_.mode;
    }

    /// A graphics submission: `dwords` from guest `addr`. `data`: the dwords already copied by the caller (kept valid
    /// until the queue has processed the submission), or nullptr to read them through CommandMemory when it starts.
    bool SubmitGraphics(u64 addr, u32 dwords, const u32* data = nullptr);
    /// A compute queue (Gnm's vqid 1..64): its ring and the guest address of its read pointer (Isolated writes it).
    bool MapComputeQueue(u32 vqid, u64 ring_addr, u32 ring_dwords, u64 read_ptr_addr);
    void UnmapComputeQueue(u32 vqid); ///< its pending work dropped
    /// A span of the compute queue's ring (the caller computed it); a packet may continue in the next span.
    bool SubmitCompute(u32 vqid, u64 addr, u32 dwords, const u32* data = nullptr);
    /// Isolated: the ring from the queue's read offset up to `next_offset_dw` (sceGnmDingDong); wraps at the ring end.
    bool DingDong(u32 vqid, u32 next_offset_dw);

    /// One packet of `queue` (0 graphics, 1..64 compute).
    StepStatus Step(u32 queue);
    /// Packets of `queue` until it is idle, blocked or faulted, or `max_packets`.
    StepStatus Run(u32 queue, u64 max_packets = ~0ull);
    /// The next packet of `queue` without processing it (submission started, finished buffers left): its guest
    /// address and dwords (valid until the next call). false: nothing to process (or the queue is faulted).
    bool Peek(u32 queue, u64& addr, std::span<const u32>& packet);

    /// Shadow: bring `queue` to the submission starting at `addr` (the other command processor starts it now). true:
    /// the next packet is its first (earlier queued submissions and the rest of the current one dropped); false: no
    /// such submission — the current one's rest is dropped unless it has not started.
    bool Resync(u32 queue, u64 addr);

    /// Isolated: the GPU finished `queue`'s work up to `seq`: held fences up to it are written, in order.
    void CompleteWork(u32 queue, u64 seq);
    /// The held clock fence of `queue` at `packet_addr` (after work `after_seq`): the GPU passed its place `age_ns` before
    /// now (called before the CompleteWork that writes it).
    void SetFenceClockAge(u32 queue, u64 packet_addr, u64 after_seq, u64 age_ns);
    u64 SubmittedWork(u32 queue) const;
    u64 CompletedWork(u32 queue) const;

    /// Drops everything in flight (pending submissions, buffers, held fences — counted as dropped, never written),
    /// registers to the power-on state, errors cleared. The processor is usable again afterwards.
    void Reset();

    const RegisterFile& Registers() const {
        return regs_;
    }
    RegisterFile& MutableRegisters() {
        return regs_;
    }
    const ComputeRegisters* Compute(u32 queue) const;
    /// The queue's compute registers; a compute queue (1..64) not seen yet is created (unmapped).
    ComputeRegisters* MutableCompute(u32 queue);
    const std::array<u32, 8>& CbExtents() const {
        return cb_extent_;
    }
    u32 DbExtent() const {
        return db_extent_;
    }
    void SetExtents(const std::array<u32, 8>& cb, u32 db) {
        cb_extent_ = cb;
        db_extent_ = db;
    }
    u64 DrawIndirectBase() const {
        return set_base_[1];
    }
    void SetDrawIndirectBase(u64 addr) {
        set_base_[1] = addr;
    }
    std::span<const u8, kGdsBytes> Gds() const {
        return std::span<const u8, kGdsBytes>(gds_ptr_, kGdsBytes);
    }
    bool QueueFaulted(u32 queue) const;
    bool QueueBlocked(u32 queue) const;
    /// The memory address (or 1 << 63 | register) the queue's blocked WAIT_REG_MEM polls; 0 when it is not blocked.
    u64 BlockedPoll(u32 queue) const;
    u32 PendingSubmissions(u32 queue) const;
    /// The current submission of a ring queue began by completing a packet carried from the previous span.
    bool StartedMidPacket(u32 queue) const;
    const CpError& LastError() const {
        return last_error_;
    }
    const Counters& Stats() const {
        return stats_;
    }
    void ResetStats() {
        stats_ = {};
    }
    void SetObserver(PacketObserver* observer) {
        observer_ = observer;
    }

private:
    struct Queue;
    struct Frame;

    enum class NextResult : u8 { Ok, None, Fault };

    Queue* Q(u32 queue) const;
    /// Isolated (diagnostics): a fence held for the GPU (any queue) will write `poll` with a value meeting the wait.
    bool HeldFenceSatisfies(u64 poll, u32 function, u32 mask, u32 ref) const;
    /// Isolated: a held fence writes GDS data (RELEASE_MEM data_sel 5, EVENT_WRITE_EOS GDS store) when it completes.
    bool HeldGdsFence() const;
    /// Isolated (diagnostics): the WAIT_REG_MEM addresses the queues blocked on, the time they blocked.
    struct WaitStat {
        u64 count = 0, ns = 0, held = 0;
        u32 queues = 0, function = 0, ref = 0, mask = 0, last = 0;
    };
    std::unordered_map<u64, WaitStat> wait_stats_;
    struct FenceStat {
        u64 count = 0, last = 0;
        u32 queues = 0, kind = 0, data_sel = 0;
    };
    std::unordered_map<u64, FenceStat> fence_stats_; ///< diagnostics: where fences write, how often, the last value

public:
    std::string WaitReport() const;
    std::string FenceReport() const;

private:
    NextResult Next(Queue& q, u64& addr, std::span<const u32>& packet, Pm4::Packet& pk);
    void UpdateReadPointer(Queue& q);
    StepStatus Fail(Queue& q, CpErrorKind kind, u64 addr, u8 opcode, u32 dwords, std::string detail);
    StepStatus Execute(Queue& q, Frame& f, u64 addr, std::span<const u32> p);
    StepStatus SetRegs(Queue& q, Frame& f, u64 addr, std::span<const u32> p, Space space);
    /// `packet`: p is the queue's draw packet itself (its trailing NOP may carry the engine's description, P5.3 direct
    /// path); false for a draw built from a native operation.
    StepStatus Draw(Queue& q, u64 addr, std::span<const u32> p, DrawKind kind, bool packet = true);
    StepStatus Dispatch(Queue& q, u64 addr, std::span<const u32> p, bool indirect);
    StepStatus DmaData(Queue& q, u64 addr, std::span<const u32> p);
    StepStatus Fence(Queue& q, FenceOp& op);
    bool WriteFence(Queue& q, FenceOp& op);
    StepStatus WaitRegMem(Queue& q, u64 addr, std::span<const u32> p);
    StepStatus IndirectBuffer(Queue& q, u64 addr, std::span<const u32> p);
    void ExtentHint(const Frame& f, u32 reg, u32 count);
    void BuildDraw(DrawDescriptor& d);
    /// P5.3B: a SecondSonPC operation in the stream (engine/render/native_render_ops.h) / the draw it describes.
    StepStatus NativeMarker(Queue& q, u64 addr, std::span<const u32> p);
    /// P5.3 direct path: a run token of the native stream - its operations run here, in their order.
    StepStatus NativeRun(Queue& q, u64 addr, std::span<const u32> p);
    /// ... a description from a draw's trailing NOP ({DescribeWord, PS shape id}) for the draw being built.
    void DescribedDraw(Queue& q, u32 word, u32 ps_shape);
    void ApplyNativeRegisters(const u32* pairs, u32 count);
    bool ApplyNativeState(const u32* pairs, u32 count);
    StepStatus NativeDrawOp(Queue& q, u64 addr, s32 pass, bool indexed, u32 count, u64 base, u32 vs_shape, u32 ps_shape);
    void VerifyNativeDraw(const DrawDescriptor& d);
    /// The processor's time in native operations, every 8th call timed (DirectCounters::native_tsc); the executor's
    /// part of their draws paused out (Draw).
    void NativeTimeBegin();
    void NativeTimeEnd();
    u32 PatchCommands(Queue& q, u64 dst, const u8* bytes_known, u32 bytes, u64 src_addr);
    /// [dst, dst + bytes) holds packets of this queue not processed yet.
    bool OverlapsPendingCommands(const Queue& q, u64 dst, u32 bytes) const;
    bool Fetch(std::vector<u32>& buffer, u64 addr, u32 dwords);
    u64 ClockSample(u32 data_sel);

    Options options_;
    RegisterFile regs_;
    std::array<std::unique_ptr<Queue>, kNumQueues> queues_;
    std::array<u64, 4> set_base_{}; ///< SET_BASE: 0 display list patch table, 1 draw indirect args, 2 GDS, 3 CE
    std::array<u32, 8> cb_extent_{};
    u32 db_extent_ = 0;
    std::array<u8, kGdsBytes> gds_{};
    u8* gds_ptr_ = gds_.data(); ///< Options::gds or gds_
    std::vector<u8> scratch_;   ///< DMA staging
    // draw state built incrementally
    u64 pipeline_key_ = 0;
    u32 pipeline_version_ = 0, dynamic_version_ = 0, targets_version_ = 0;
    u32 inline_marker_ = 0; ///< a "V#/T#/S# in user data" NOP marker waiting for its SET_SH_REG
    std::array<u16, static_cast<size_t>(Stage::Count)> inline_sharps_{};
    std::unique_ptr<Engine::Render::DrawOp> native_expect_; ///< kVerify marker waiting for its draw (P5.3B)
    u32 native_expect_queue_ = 0;
    /// P5.3: the engine's description of the next draw of a queue (any draw marker), handed on with it
    struct EngineNext {
        bool valid = false;
        u32 queue = 0;
        s32 pass = kNoEnginePass;
        EngineTable vs, ps;
    };
    EngineNext engine_next_;
    u32 native_logged_ = 0;
    u32 native_calls_ = 0;
    u64 native_t0_ = 0;  ///< a timed native operation in progress: its start (or resume) TSC
    u64 native_acc_ = 0; ///< ... its time so far
    /// P5.3D A/B image comparison: SSPC_P53_CAPTURE_PASSES=<pass>[,...] with SSPC_P53_CAPTURE_FLIPS=<first>+<n> (the
    /// processor's flip count): the targets those passes' draws wrote, captured where the next pass's draws begin.
    struct PassCapture {
        u64 passes = 0;
        u64 first = 0, count = 0;
        bool parsed = false;
        s32 pass = -100;          ///< the pass of the last draw marker
        std::vector<u64> targets; ///< written by the current pass's draws so far
        u32 native = 0, pm4 = 0;  ///< the current pass's draws: kApply markers, kVerify-only markers
    };
    PassCapture pass_capture_;
    void PassCaptureMarker(s32 pass, bool applied);
    PacketObserver* observer_ = nullptr;
    CpError last_error_;
    Counters stats_;
};

} // namespace SecondSon::Gpu::Cp
