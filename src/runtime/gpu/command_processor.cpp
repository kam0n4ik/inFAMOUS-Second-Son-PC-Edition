// SecondSonPC — SecondSonPC's GPU command processor (P4.2). See command_processor.h.
#include "runtime/gpu/command_processor.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <intrin.h>  // __rdtsc (the native operations' time)
#include <windows.h> // GetEnvironmentVariableA (SSPC_P53_CAPTURE_*)

#include "base/log.h"
#include "engine/render/draw_runs.h"
#include "runtime/gpu/cp_r6_opt.h"
#include "runtime/gpu/r8/cp_profile.h"
#include "runtime/gpu/r11/label_wait.h"
#include "runtime/gpu/stall_trace.h"

// Descriptors and fence records are filled naming only the fields a packet sets.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Cp {

namespace {

constexpr u32 kMaxPending = 512;     ///< submissions waiting per queue
constexpr u32 kMaxFences = 128;       ///< fences held per queue (Isolated)
constexpr u32 kMaxFrames = 8;         ///< the submission, a carried packet, indirect buffers
constexpr u32 kMaxPatches = 64;       ///< DMA/WRITE_DATA writes into packets not yet processed, per submission
constexpr u32 kCarryDwords = 0x4001;  ///< the longest type-3 packet: header + 0x4000 body dwords
constexpr u32 kNopHint = 0xC0001000u; ///< NOP with one payload dword: the driver's target size hint after a setup
constexpr u32 kReadTags = 0x3022C;    ///< CP_DMA_READ_TAGS (register 0xC08B as a byte address): an L2 prefetch

constexpr u32 FlatOf(u32 reg) {
    for (u32 s = 0; s < static_cast<u32>(Space::Count); ++s) {
        if (reg >= kSpaceBase[s] && reg - kSpaceBase[s] < kSpaceCount[s]) {
            return kSpaceFirst[s] + (reg - kSpaceBase[s]);
        }
    }
    return 0;
}

constexpr u32 kStageBase[] = {Reg::SpiShaderPgmLoPs, Reg::SpiShaderPgmLoVs, Reg::SpiShaderPgmLoGs,
                              Reg::SpiShaderPgmLoEs, Reg::SpiShaderPgmLoHs, Reg::SpiShaderPgmLoLs};
constexpr u32 kShBase = kSpaceBase[static_cast<u32>(Space::Sh)];
constexpr u32 kComputeShFirst = kComputeBase - kShBase; ///< SH offset 0x200
constexpr u32 kComputeShEnd = kComputeShFirst + kComputeCount;

constexpr u32 kIdxNumIndices = FlatOf(Reg::VgtNumIndices);
constexpr u32 kIdxNumInstances = FlatOf(Reg::VgtNumInstances);
constexpr u32 kIdxDmaBaseHi = FlatOf(Reg::VgtDmaBaseHi);
constexpr u32 kIdxDmaBase = FlatOf(Reg::VgtDmaBase);
constexpr u32 kIdxIndexType = FlatOf(Reg::VgtDmaIndexType);
constexpr u32 kIdxMaxSize = FlatOf(Reg::VgtDmaMaxSize);
constexpr u32 kIdxDmaSize = FlatOf(Reg::VgtDmaSize);
constexpr u32 kIdxIndxOffset = FlatOf(Reg::VgtIndxOffset);
constexpr u32 kIdxInitiator = FlatOf(Reg::VgtDrawInitiator);
constexpr u32 kIdxPrimType = FlatOf(Reg::VgtPrimitiveType);
constexpr u32 kIdxStages = FlatOf(Reg::VgtShaderStagesEn);

constexpr GroupMask kTargetGroups =
    Bit(Group::ColorTarget) | Bit(Group::ColorFormat) | Bit(Group::DepthTarget) | Bit(Group::DepthFormat);

u64 Mix(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

/// SET_*_REG of one register through its space (the draw packets' own register writes).
bool SetOne(RegisterFile& regs, u32 reg, u32 value) {
    return regs.WriteReg(reg, value);
}

} // namespace

struct CommandProcessor::Frame {
    u64 addr = 0;              ///< guest address of data[0]
    const u32* data = nullptr; ///< the frame's dwords: the caller's copy or the processor's buffer
    u32 size = 0;              ///< dwords
    u32 pos = 0;               ///< the next packet
    bool ring = false;         ///< a compute ring span: a packet cut by its end continues in the next span
    bool owned = false;        ///< data is the processor's buffer of this depth
    u32 ring_offset = 0;       ///< the span's start in the ring, dwords
};

struct CommandProcessor::Queue {
    struct Pending {
        u64 addr = 0;
        u32 dwords = 0;
        const u32* data = nullptr;
        bool ring = false;
        u32 ring_offset = 0;
        u64 seq = 0; ///< CommandProcessor::submit_seq_ when it came
    };
    struct Patch {
        u64 dst = 0;
        u32 bytes = 0;
        u64 src = 0;          ///< the DMA's source address (0: GDS)
        bool applied = false; ///< the processor's copy holds the written bytes
    };

    u32 id = 0;
    bool compute = false;
    bool mapped = false; ///< a compute ring known to the processor (MapComputeQueue)
    u64 ring_addr = 0;
    u32 ring_dw = 0;
    u64 rptr_addr = 0;
    /// CP-8: the read pointer last written (a write of the same value again is left out - the ring's position
    /// stays put while the queue executes an indirect buffer); rptr_known: written since the map
    u32 rptr_last = 0;
    bool rptr_known = false;
    u32 cursor_dw = 0; ///< DingDong: the next span's start
    std::array<Pending, kMaxPending> pending{};
    u32 head = 0;
    u32 count = 0;
    std::array<Frame, kMaxFrames> frames{};
    u32 depth = 0;
    std::array<std::vector<u32>, kMaxFrames> buffers;
    std::vector<u32> spare; ///< a chained indirect buffer is read here first (the packet that chains stays readable)
    u64 budget = 0;
    std::vector<u32> carry; ///< a packet cut by the end of a ring span
    u32 carry_have = 0;
    u32 carry_need = 0;
    u64 carry_addr = 0;
    ComputeRegisters compute_regs;
    u64 work_seq = 0;
    u64 work_done = 0;
    std::array<FenceOp, kMaxFences> fences{};
    u32 fence_head = 0;
    u32 fence_count = 0;
    std::array<Patch, kMaxPatches> patches{};
    u32 patch_count = 0;
    bool patch_overflow = false;     ///< a write into unprocessed packets found no free patch entry
    bool started_mid_packet = false; ///< the current submission began by completing a packet carried from the last
    bool faulted = false;
    bool blocked = false;
    u64 block_t0 = 0, block_poll = 0; ///< diagnostics: when and on what the queue blocked
    bool block_held = false;          ///< a held fence would meet the wait when it blocked
    u64 active_seq = 0;               ///< the seq of the submission at depth 1
    u8 order_state = 0; ///< R9 REA-3 order, the next submission: 1 counted as held, 2 as released
};

std::string_view DrawKindName(DrawKind kind) {
    switch (kind) {
    case DrawKind::Index2:
        return "DRAW_INDEX_2";
    case DrawKind::IndexOffset2:
        return "DRAW_INDEX_OFFSET_2";
    case DrawKind::IndexAuto:
        return "DRAW_INDEX_AUTO";
    case DrawKind::IndexIndirect:
        return "DRAW_INDEX_INDIRECT";
    case DrawKind::Indirect:
        return "DRAW_INDIRECT";
    }
    return "?";
}

std::string_view DmaKindName(DmaKind kind) {
    switch (kind) {
    case DmaKind::MemoryToMemory:
        return "memory->memory";
    case DmaKind::DataToMemory:
        return "data->memory";
    case DmaKind::MemoryToGds:
        return "memory->GDS";
    case DmaKind::DataToGds:
        return "data->GDS";
    case DmaKind::GdsToMemory:
        return "GDS->memory";
    case DmaKind::ReadTagsPrefetch:
        return "prefetch (CP_DMA_READ_TAGS)";
    }
    return "?";
}

std::string_view CpErrorName(CpErrorKind kind) {
    static constexpr std::string_view kNames[] = {"none",
                                                  "truncated packet",
                                                  "reserved packet type 1",
                                                  "type-0 packet",
                                                  "unknown opcode",
                                                  "unsupported opcode",
                                                  "unsupported form",
                                                  "packet shorter than its fields",
                                                  "register outside the register windows",
                                                  "unreadable command buffer",
                                                  "indirect buffers nested too deep",
                                                  "submission budget exceeded",
                                                  "unreadable memory",
                                                  "memory write failed",
                                                  "GDS range",
                                                  "too many fences held",
                                                  "compute queue not mapped",
                                                  "too many submissions queued"};
    static_assert(std::size(kNames) == static_cast<size_t>(CpErrorKind::Count));
    return kind < CpErrorKind::Count ? kNames[static_cast<size_t>(kind)] : "?";
}

std::string CpError::Text() const {
    return fmt::format("{} on queue {} (depth {}) at {:#x}: opcode {:#04x} {} ({} dwords){}{}", CpErrorName(kind),
                       queue, depth, address, opcode, Pm4::OpcodeName(opcode), dwords, detail.empty() ? "" : ": ",
                       detail);
}

CommandProcessor::CommandProcessor(const Options& options) : options_(options) {
    options_.max_ib_depth = std::min(options_.max_ib_depth, kMaxFrames - 2); // frames: submission, carry, IBs
    if (options_.gds != nullptr) {
        gds_ptr_ = options_.gds;
    }
    queues_[kGraphicsQueue] = std::make_unique<Queue>();
    queues_[kGraphicsQueue]->id = kGraphicsQueue;
    if (char v[4] = {}; GetEnvironmentVariableA("SSPC_CP_RPTR_EVERY_PACKET", v, sizeof(v)) == 1 && v[0] == '1') {
        rptr_every_packet_ = true;
    }
    // (the loader installs the engine's probes with the same switch; without them no draw carries a tag)
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_CITY_ENGINE_OPT", v, sizeof(v)) > 0 && v[0] != '0') {
        engine_runs_ = true;
        // (RunAhead: measured without a gain - only with SSPC_CITY_ENGINE_AHEAD=1 or =ahead; docs/CITY_ENGINE_OPTIMIZATION.md)
        char a[4] = {};
        run_ahead_ = (GetEnvironmentVariableA("SSPC_CITY_ENGINE_AHEAD", a, sizeof(a)) == 1 && a[0] == '1') ||
                     std::string_view(v) == "ahead";
    }
}

CommandProcessor::~CommandProcessor() = default;

CommandProcessor::Queue* CommandProcessor::Q(u32 queue) const {
    return queue < kNumQueues ? queues_[queue].get() : nullptr;
}

const ComputeRegisters* CommandProcessor::Compute(u32 queue) const {
    const Queue* q = Q(queue);
    return q ? &q->compute_regs : nullptr;
}

ComputeRegisters* CommandProcessor::MutableCompute(u32 queue) {
    if (queue != kGraphicsQueue && queue < kNumQueues && !queues_[queue]) {
        auto q = std::make_unique<Queue>(); // a compute queue known before its first submission (unmapped)
        q->id = queue;
        q->compute = true;
        q->carry.resize(kCarryDwords);
        queues_[queue] = std::move(q);
    }
    Queue* q = Q(queue);
    return q ? &q->compute_regs : nullptr;
}

bool CommandProcessor::QueueFaulted(u32 queue) const {
    const Queue* q = Q(queue);
    return q && q->faulted;
}

bool CommandProcessor::QueueBlocked(u32 queue) const {
    const Queue* q = Q(queue);
    return q && q->blocked;
}

u64 CommandProcessor::BlockedPoll(u32 queue) const {
    const Queue* q = Q(queue);
    return q && q->blocked ? q->block_poll : 0;
}

bool CommandProcessor::StartedMidPacket(u32 queue) const {
    const Queue* q = Q(queue);
    return q && q->started_mid_packet;
}

u32 CommandProcessor::PendingSubmissions(u32 queue) const {
    const Queue* q = Q(queue);
    return q ? q->count : 0;
}

u64 CommandProcessor::SubmittedWork(u32 queue) const {
    const Queue* q = Q(queue);
    return q ? q->work_seq : 0;
}

u64 CommandProcessor::CompletedWork(u32 queue) const {
    const Queue* q = Q(queue);
    return q ? q->work_done : 0;
}

// --- submissions ---

bool CommandProcessor::SubmitGraphics(u64 addr, u32 dwords, const u32* data) {
    Queue& q = *queues_[kGraphicsQueue];
    if (q.count == kMaxPending) {
        Fail(q, CpErrorKind::QueueFull, addr, 0, dwords, "graphics submission refused");
        return false;
    }
    q.pending[(q.head + q.count++) % kMaxPending] = {addr, dwords, data, false, 0, ++submit_seq_};
    return true;
}

bool CommandProcessor::MapComputeQueue(u32 vqid, u64 ring_addr, u32 ring_dwords, u64 read_ptr_addr) {
    if (vqid == 0 || vqid >= kNumQueues || ring_dwords == 0 || !std::has_single_bit(ring_dwords)) {
        return false;
    }
    auto& slot = queues_[vqid];
    if (slot) {
        // mapped again: nothing of the previous ring survives (pending spans, frames, a carried packet, fences)
        stats_.fences_dropped += slot->fence_count;
        auto buffers = std::move(slot->buffers);
        auto spare = std::move(slot->spare);
        slot = std::make_unique<Queue>();
        slot->buffers = std::move(buffers);
        slot->spare = std::move(spare);
    } else {
        slot = std::make_unique<Queue>();
    }
    Queue& q = *slot;
    q.id = vqid;
    q.compute = true;
    q.mapped = true;
    q.ring_addr = ring_addr;
    q.ring_dw = ring_dwords;
    q.rptr_addr = read_ptr_addr;
    q.cursor_dw = 0;
    q.carry.resize(kCarryDwords);
    if (options_.mode == Mode::Isolated && read_ptr_addr != 0) {
        const u32 zero = 0;
        q.rptr_known = options_.memory->Write(read_ptr_addr, &zero, sizeof(zero)); // as sceGnmMapComputeQueue: 0
    }
    return true;
}

void CommandProcessor::UnmapComputeQueue(u32 vqid) {
    if (vqid == 0 || vqid >= kNumQueues || !queues_[vqid]) {
        return;
    }
    stats_.fences_dropped += queues_[vqid]->fence_count;
    queues_[vqid].reset();
}

bool CommandProcessor::SubmitCompute(u32 vqid, u64 addr, u32 dwords, const u32* data) {
    if (vqid == 0 || vqid >= kNumQueues) {
        return false;
    }
    auto& slot = queues_[vqid];
    if (!slot) {
        // a span of a ring the processor was not told about (Shadow: the bridge mapped it): no read pointer
        slot = std::make_unique<Queue>();
        slot->id = vqid;
        slot->compute = true;
        slot->carry.resize(kCarryDwords);
    }
    Queue& q = *slot;
    if (q.count == kMaxPending) {
        Fail(q, CpErrorKind::QueueFull, addr, 0, dwords, "compute submission refused");
        return false;
    }
    const u32 offset = q.mapped ? static_cast<u32>((addr - q.ring_addr) / 4) : 0;
    q.pending[(q.head + q.count++) % kMaxPending] = {addr, dwords, data, true, offset, ++submit_seq_};
    return true;
}

bool CommandProcessor::DingDong(u32 vqid, u32 next_offset_dw) {
    Queue* q = Q(vqid);
    if (q == nullptr || !q->mapped || next_offset_dw > q->ring_dw) {
        if (q != nullptr) {
            Fail(*q, CpErrorKind::QueueNotMapped, 0, 0, next_offset_dw, "DingDong past the ring or on no ring");
        }
        return false;
    }
    const u32 cur = q->cursor_dw;
    const u32 next = next_offset_dw % q->ring_dw;
    if (next_offset_dw == cur) {
        return true;
    }
    bool ok = true;
    if (next_offset_dw > cur) {
        ok = SubmitCompute(vqid, q->ring_addr + u64{cur} * 4, next_offset_dw - cur);
    } else {
        ok = SubmitCompute(vqid, q->ring_addr + u64{cur} * 4, q->ring_dw - cur);
        if (ok && next != 0) {
            ok = SubmitCompute(vqid, q->ring_addr, next);
        }
    }
    q->cursor_dw = next;
    return ok;
}

// --- positioning ---

bool CommandProcessor::Fetch(std::vector<u32>& buffer, u64 addr, u32 dwords) {
    if (buffer.capacity() < dwords) {
        ++stats_.buffer_growths;
    }
    if (buffer.size() < dwords) {
        buffer.resize(dwords);
    }
    return options_.memory->Read(addr, buffer.data(), u64{dwords} * 4);
}

CommandProcessor::NextResult CommandProcessor::Next(Queue& q, u64& addr, std::span<const u32>& packet,
                                                    Pm4::Packet& pk) {
    for (;;) {
        while (q.depth > 0 && q.frames[q.depth - 1].pos >= q.frames[q.depth - 1].size) {
            --q.depth;
        }
        if (q.depth == 0) {
            if (q.faulted && !options_.resume_after_fault) {
                return NextResult::Fault;
            }
            if (q.count == 0) {
                return NextResult::None;
            }
            if (q.compute && order_compute_ && HeldByOrder(q)) {
                return NextResult::None;
            }
            const Queue::Pending s = q.pending[q.head];
            q.head = (q.head + 1) % kMaxPending;
            --q.count;
            q.active_seq = s.seq;
            q.order_state = 0;
            q.faulted = false;
            q.budget = 0;
            q.patch_count = 0;
            q.patch_overflow = false;
            q.started_mid_packet = false;
            ++stats_.submissions;
            Frame& f = q.frames[0];
            f = Frame{.addr = s.addr, .data = s.data, .size = s.dwords, .ring = s.ring, .ring_offset = s.ring_offset};
            if (f.data == nullptr) {
                Native::R6::g_read_site = Native::R6::kReadSubmission;
                const bool fetched = Fetch(q.buffers[0], s.addr, s.dwords);
                Native::R6::g_read_site = Native::R6::kReadOther;
                if (!fetched) {
                    Fail(q, CpErrorKind::IndirectUnreadable, s.addr, 0, s.dwords, "submission");
                    continue;
                }
                f.data = q.buffers[0].data();
                f.owned = true;
            }
            q.depth = 1;
            if (f.ring && q.carry_need > 0) {
                // the rest of the packet cut by the previous span's end
                const u32 take = std::min(q.carry_need - q.carry_have, s.dwords);
                q.started_mid_packet = true;
                std::copy_n(f.data, take, q.carry.data() + q.carry_have);
                q.carry_have += take;
                f.pos = take;
                if (q.carry_have == q.carry_need) {
                    q.frames[1] = Frame{.addr = q.carry_addr, .data = q.carry.data(), .size = q.carry_need};
                    q.depth = 2;
                    q.carry_need = q.carry_have = 0;
                }
                UpdateReadPointer(q);
            }
            continue;
        }
        Frame& f = q.frames[q.depth - 1];
        const std::span<const u32> rest(f.data + f.pos, f.size - f.pos);
        Pm4::DecodeError e{};
        addr = f.addr + u64{f.pos} * 4;
        if (!Pm4::Decode(rest, pk, e)) {
            if (e == Pm4::DecodeError::Truncated && f.ring && q.depth == 1) {
                // continues in the next span of the ring (the hardware reads the ring as one stream)
                q.carry_need = pk.dwords;
                q.carry_have = static_cast<u32>(rest.size());
                q.carry_addr = addr;
                std::copy(rest.begin(), rest.end(), q.carry.begin());
                f.pos = f.size;
                ++stats_.ring_carries;
                UpdateReadPointer(q);
                continue;
            }
            Fail(q, e == Pm4::DecodeError::ReservedType ? CpErrorKind::ReservedType : CpErrorKind::Truncated, addr,
                 static_cast<u8>(rest.empty() ? 0 : rest[0] >> 8), static_cast<u32>(rest.size()),
                 fmt::format("{} dwords declared, {} left in the buffer", pk.dwords, rest.size()));
            return NextResult::Fault;
        }
        packet = rest.first(pk.dwords);
        return NextResult::Ok;
    }
}

bool CommandProcessor::HeldByOrder(Queue& q) {
    // The executor records every queue in one stream: a compute submission the game made after a frame's graphics,
    // started before that graphics is processed, lands inside it and delays its completion by its own GPU time - the
    // next frame's async compute (the particles) in one frame of two, the other one without (REA-3's 17/40 ms frames).
    const Queue& g = *queues_[kGraphicsQueue];
    u64 first = 0; // the seq of the earliest graphics submission not processed yet (0: none)
    for (u32 i = 0; i < g.depth; ++i) {
        if (g.frames[i].pos < g.frames[i].size) {
            first = g.active_seq;
            break;
        }
    }
    if (first == 0 && g.count != 0) {
        first = g.pending[g.head].seq;
    }
    if (first == 0 || first > q.pending[q.head].seq) {
        return false;
    }
    if (g.blocked && !(g.block_poll >= order_free_lo_ && g.block_poll < order_free_hi_)) {
        // (the graphics queue waits for memory - or a register - the compute work may be what writes: no hold)
        if (q.order_state != 2) {
            q.order_state = 2;
            ++stats_.order_released;
        }
        return false;
    }
    if (q.order_state == 0) {
        q.order_state = 1;
        ++stats_.order_held;
    }
    return true;
}

void CommandProcessor::UpdateReadPointer(Queue& q) {
    if (options_.mode != Mode::Isolated || !q.mapped || q.rptr_addr == 0 || q.depth == 0 || !q.frames[0].ring) {
        return;
    }
    const u32 rptr = (q.frames[0].ring_offset + q.frames[0].pos) % q.ring_dw;
    if (q.rptr_known && rptr == q.rptr_last && !rptr_every_packet_) {
        ++stats_.rptr_same;
        return; // (CP-8: the memory holds this value already)
    }
    q.rptr_known = options_.memory->Write(q.rptr_addr, &rptr, sizeof(rptr));
    q.rptr_last = rptr;
}

bool CommandProcessor::Peek(u32 queue, u64& addr, std::span<const u32>& packet) {
    Queue* q = Q(queue);
    Pm4::Packet pk;
    return q != nullptr && Next(*q, addr, packet, pk) == NextResult::Ok;
}

StepStatus CommandProcessor::Fail(Queue& q, CpErrorKind kind, u64 addr, u8 opcode, u32 dwords, std::string detail) {
    last_error_ = CpError{.kind = kind,
                          .queue = q.id,
                          .depth = q.depth > 0 ? q.depth - 1 : 0,
                          .address = addr,
                          .opcode = opcode,
                          .dwords = dwords,
                          .detail = std::move(detail)};
    ++stats_.errors[static_cast<size_t>(kind)];
    if (kind == CpErrorKind::QueueFull || kind == CpErrorKind::QueueNotMapped) {
        return StepStatus::Faulted; // a refused submission: the queue itself goes on
    }
    q.depth = 0; // the submission is dropped
    q.blocked = false;
    q.faulted = true;
    q.carry_need = q.carry_have = 0;
    inline_marker_ = 0; // a marker of the dropped stream must not flag the next one's user data
    return StepStatus::Faulted;
}

StepStatus CommandProcessor::Step(u32 queue) {
    Queue* qp = Q(queue);
    if (qp == nullptr) {
        return StepStatus::Idle;
    }
    Queue& q = *qp;
    u64 addr = 0;
    std::span<const u32> p;
    Pm4::Packet pk;
    R8_CP_PROF_PACKET();
    switch (Next(q, addr, p, pk)) {
    case NextResult::None:
        return StepStatus::Idle;
    case NextResult::Fault:
        return StepStatus::Faulted;
    case NextResult::Ok:
        break;
    }
    R8_CP_PROF_DECODED(pk.opcode, p);
    Frame& f = q.frames[q.depth - 1];
    q.budget += pk.dwords;
    if (q.budget > options_.max_dwords_per_submission) {
        return Fail(q, CpErrorKind::BudgetExceeded, addr, pk.opcode, pk.dwords,
                    fmt::format("more than {} dwords in one submission", options_.max_dwords_per_submission));
    }
    if (pk.type == 2) {
        ++f.pos;
        ++stats_.type2;
        UpdateReadPointer(q);
        return StepStatus::Packet;
    }
    if (pk.type == 0) {
        return Fail(q, CpErrorKind::Type0, addr, 0, pk.dwords, fmt::format("base register {:#x}", pk.base));
    }
    if (pk.predicate) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, pk.opcode, pk.dwords, "predicated packet");
    }
    // Shadow: a packet an earlier DMA from memory wrote into — its bytes are what the GPU copied, unknown here; only
    // a dispatch's group counts can be taken from the DMA's source (Dispatch). Anything else is refused, not guessed.
    if (options_.mode == Mode::Shadow) {
        const u64 end = addr + u64{pk.dwords} * 4;
        for (u32 i = 0; i < q.patch_count; ++i) {
            const auto& pt = q.patches[i];
            if (pt.applied || pt.dst >= end || addr >= pt.dst + pt.bytes) {
                continue;
            }
            const bool counts = pk.opcode == Pm4::Op::DispatchDirect && pt.dst <= addr + 4 &&
                                addr + 16 <= pt.dst + pt.bytes;
            if (!counts) {
                return Fail(q, CpErrorKind::UnsupportedForm, addr, pk.opcode, pk.dwords,
                            "a packet an earlier DMA from memory wrote into (its bytes are unknown to the shadow)");
            }
        }
    }
    const u32 saved = f.pos;
    f.pos += pk.dwords;
    if (observer_ != nullptr && !q.blocked) { // once per packet, not per retry of a blocked wait
        observer_->BeforePacket(q.id, addr, p);
    }
    const u64 t0 = options_.packet_ticks ? __rdtsc() : 0;
    const StepStatus s = Execute(q, f, addr, p);
    if (s == StepStatus::Blocked) {
        f.pos = saved;
        q.budget -= pk.dwords;
        return s;
    }
    if (s != StepStatus::Packet) {
        return s;
    }
    ++(q.compute ? stats_.compute_packets : stats_.graphics_packets)[pk.opcode];
    if (observer_ != nullptr) {
        observer_->AfterPacket(q.id, addr, p); // p stays readable: a chained buffer was read into another vector
    }
    if (t0 != 0) {
        const u64 t1 = __rdtsc();
        (q.compute ? stats_.compute_ticks : stats_.graphics_ticks)[pk.opcode] += t1 - t0;
        UpdateReadPointer(q);
        stats_.rptr_ticks += __rdtsc() - t1;
        return StepStatus::Packet;
    }
    UpdateReadPointer(q);
    return StepStatus::Packet;
}

StepStatus CommandProcessor::Run(u32 queue, u64 max_packets) {
    StepStatus s = StepStatus::Idle;
    for (u64 n = 0; n < max_packets; ++n) {
        s = Step(queue);
        if (s != StepStatus::Packet) {
            return s;
        }
    }
    return s;
}

// --- packets ---

StepStatus CommandProcessor::Execute(Queue& q, Frame& f, u64 addr, std::span<const u32> p) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const auto body = p.subspan(1);
    const u32 n = static_cast<u32>(p.size());
    const auto short_packet = [&](u32 need) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, n, fmt::format("{} body dwords, {} needed", body.size(), need));
    };
    const auto graphics_only = [&] {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n, "a graphics packet on a compute queue");
    };
    const auto unsupported = [&](std::string detail) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n, std::move(detail));
    };
    namespace O = Pm4::Op;
    switch (op) {
    case O::Nop: {
        if (Engine::Render::IsMarker(p)) {
            NativeTimeBegin();
            const StepStatus s = NativeMarker(q, addr, p);
            NativeTimeEnd();
            return s;
        }
        if (p.size() == Engine::Render::kStreamTokenDwords && p[1] == Engine::Render::kStreamMagic) {
            NativeTimeBegin();
            const StepStatus s = NativeRun(q, addr, p);
            NativeTimeEnd();
            return s;
        }
        const Pm4::NopMarker m = Pm4::ClassifyNop(p);
        if (m == Pm4::NopMarker::PatchedFlip) {
            ++stats_.flips;
            Engine::Render::Control().flips.fetch_add(1, std::memory_order_relaxed); // (the A/B schedule's clock)
            options_.work->Flip(FlipMarker{.queue = q.id, .packet_addr = addr, .after_seq = q.work_seq});
            R8_CP_PROF_FRAME(); // (R8: the profiler's frame row)
        } else if (m == Pm4::NopMarker::SetVsharpInUdata || m == Pm4::NopMarker::SetTsharpInUdata ||
                   m == Pm4::NopMarker::SetSsharpInUdata) {
            inline_marker_ = body[0];
        }
        return StepStatus::Packet;
    }
    case O::SetShReg:
        return SetRegs(q, f, addr, p, Space::Sh);
    case O::SetContextReg:
        return q.compute ? graphics_only() : SetRegs(q, f, addr, p, Space::Context);
    case O::SetUconfigReg:
        return q.compute ? graphics_only() : SetRegs(q, f, addr, p, Space::Uconfig);
    case O::SetConfigReg:
        return q.compute ? graphics_only() : SetRegs(q, f, addr, p, Space::Config);
    case O::IndexType:
    case O::NumInstances:
    case O::IndexBufferSize: {
        if (q.compute) {
            return graphics_only();
        }
        if (body.empty()) {
            return short_packet(1);
        }
        const u32 reg = op == O::IndexType      ? Reg::VgtDmaIndexType
                        : op == O::NumInstances ? Reg::VgtNumInstances
                                                : Reg::VgtDmaSize;
        SetOne(regs_, reg, body[0]);
        return StepStatus::Packet;
    }
    case O::IndexBase: {
        if (q.compute) {
            return graphics_only();
        }
        if (body.size() < 2) {
            return short_packet(2);
        }
        SetOne(regs_, Reg::VgtDmaBase, body[0]);
        SetOne(regs_, Reg::VgtDmaBaseHi, body[1] & 0xFF); // BASE_ADDR[39:32]
        return StepStatus::Packet;
    }
    case O::SetBase: {
        if (body.size() < 3) {
            return short_packet(3);
        }
        const u32 index = body[0] & 0xF;
        if (index > 3) {
            return unsupported(fmt::format("base index {}", index));
        }
        set_base_[index] = body[1] | (u64{body[2] & 0xFFFF} << 32);
        return StepStatus::Packet;
    }
    case O::DrawIndex2:
        return Draw(q, addr, p, DrawKind::Index2);
    case O::DrawIndexOffset2:
        return Draw(q, addr, p, DrawKind::IndexOffset2);
    case O::DrawIndexAuto:
        return Draw(q, addr, p, DrawKind::IndexAuto);
    case O::DrawIndexIndirect:
        return Draw(q, addr, p, DrawKind::IndexIndirect);
    case O::DrawIndirect:
        return Draw(q, addr, p, DrawKind::Indirect);
    case O::DispatchDirect:
        return Dispatch(q, addr, p, false);
    case O::DispatchIndirect:
        return Dispatch(q, addr, p, true);
    case O::DmaData:
        return DmaData(q, addr, p);
    case O::AcquireMem: {
        if (body.size() < 6) {
            return short_packet(6);
        }
        const CacheActionOp a{.queue = q.id,
                              .packet_addr = addr,
                              .coher_cntl = body[0],
                              .base = (body[3] | (u64{body[4] & 0xFF} << 32)) << 8,
                              .size = (body[1] | (u64{body[2] & 0xFF} << 32)) << 8,
                              .after_seq = q.work_seq};
        ++stats_.cache_actions;
        options_.work->CacheAction(a);
        return StepStatus::Packet;
    }
    case O::EventWrite: {
        if (body.empty()) {
            return short_packet(1);
        }
        const u32 index = (body[0] >> 8) & 0xF;
        const u32 type = body[0] & 0x3F;
        if (index == 1 && type == 0x38) {
            // PIXEL_PIPE_STAT_CONTROL (the game's mission smi_torture_a10: 0x7fc00 = counter 0 (occlusion), stride
            // 128 bits, DBs 0..7): the address dwords hold the control fields, nothing is written to memory
            if (body.size() >= 2 && ((body.size() < 3 ? 0u : body[2]) & 0x70000000u) == 0) {
                static constexpr u32 kStride[4] = {4, 8, 16, 32};
                pipe_stat_stride_ = kStride[(body[1] >> 9) & 3];
                pipe_stat_mask_ = (body[1] >> 11) & 0xFFFF;
            }
            ++stats_.cache_actions;
            return StepStatus::Packet;
        }
        if (index == 1 && (type == 0x15 || type == 0x39)) {
            // ZPASS_DONE / PIXEL_PIPE_STAT_DUMP: each enabled DB writes its 64-bit pass count (bit 63: valid) at
            // addr + DB * stride. Not counted on the host GPU: the count written only grows, so every query's
            // end - begin is positive (the object reads as visible: conservative, nothing is culled wrongly)
            if (body.size() < 3) {
                return short_packet(3);
            }
            const u64 dst = (body[1] & ~7u) | (u64{body[2] & 0xFFFF} << 32);
            const u32 zmask = type == 0x39 ? pipe_stat_mask_ : 0xFFu;
            const u32 zstride = type == 0x39 ? pipe_stat_stride_ : 16u;
            if (options_.mode == Mode::Isolated &&
                options_.work->ZpassOnTimeline(q.id, q.work_seq + 1, dst, zmask, zstride)) {
                ++q.work_seq;
                ++stats_.timeline_writes;
                if (zpass_writes_++ == 0) {
                    SSPC_LOG(GPU, Info, "command processor: occlusion queries ({} at {:#x}, DBs {:#x}, stride {}): "
                                        "counted on the host GPU",
                             type == 0x39 ? "PIXEL_PIPE_STAT_DUMP" : "ZPASS_DONE", dst, zmask, zstride);
                }
                return StepStatus::Packet;
            }
            zpass_count_ += 0x10000;
            const u64 value = zpass_count_ | (1ull << 63);
            const u32 mask = zmask;
            const u32 stride = zstride;
            for (u32 db = 0; db < 16; ++db) {
                if ((mask >> db & 1) == 0) {
                    continue;
                }
                const u64 at = dst + u64{db} * stride;
                if (options_.mode == Mode::Isolated) {
                    if (options_.work->WriteOnTimeline(q.id, q.work_seq + 1, at, &value, sizeof(value))) {
                        ++q.work_seq;
                        ++stats_.timeline_writes;
                    } else {
                        options_.work->Synchronize(q.id);
                        if (!options_.memory->Write(at, &value, sizeof(value))) {
                            return Fail(q, CpErrorKind::MemoryWriteFailed, addr, op, n,
                                        fmt::format("occlusion count at {:#x}", at));
                        }
                    }
                }
                PatchCommands(q, at, reinterpret_cast<const u8*>(&value), sizeof(value), 0);
            }
            if (zpass_writes_++ == 0) {
                SSPC_LOG(GPU, Info, "command processor: occlusion queries ({} at {:#x}, DBs {:#x}, stride {}): counts "
                                    "not measured on the host GPU - every query reads as visible",
                         type == 0x39 ? "PIXEL_PIPE_STAT_DUMP" : "ZPASS_DONE", dst, mask, stride);
            }
            return StepStatus::Packet;
        }
        if (index != 0 && index != 4 && index != 7) {
            // ZPASS_DONE, statistics samples: they write counters to memory (not implemented)
            return unsupported(fmt::format("event index {} (type {})", index, body[0] & 0x3F));
        }
        ++stats_.cache_actions;
        options_.work->CacheAction(
            CacheActionOp{.queue = q.id, .packet_addr = addr, .event = body[0] & 0xFFF, .after_seq = q.work_seq});
        return StepStatus::Packet;
    }
    case O::WaitRegMem:
        return WaitRegMem(q, addr, p);
    case O::EventWriteEop: {
        if (q.compute) {
            return graphics_only();
        }
        if (body.size() < 5) {
            return short_packet(5);
        }
        FenceOp fence{.kind = FenceKind::EndOfPipe,
                      .queue = q.id,
                      .packet_addr = addr,
                      .event_type = body[0] & 0x3F,
                      .data_sel = (body[2] >> 29) & 7,
                      .int_sel = (body[2] >> 24) & 3,
                      .addr = body[1] | (u64{body[2] & 0xFFFF} << 32),
                      .data = body[3] | (u64{body[4]} << 32)};
        if (fence.data_sel > 4 || fence.int_sel == 3 || (fence.int_sel == 1 && fence.data_sel != 0)) {
            return unsupported(fmt::format("data_sel {} int_sel {}", fence.data_sel, fence.int_sel));
        }
        return Fence(q, fence);
    }
    case O::EventWriteEos: {
        if (q.compute) {
            return graphics_only();
        }
        if (body.size() < 4) {
            return short_packet(4);
        }
        FenceOp fence{.kind = FenceKind::EndOfShader,
                      .queue = q.id,
                      .packet_addr = addr,
                      .event_type = body[0] & 0x3F,
                      .eos_command = (body[2] >> 29) & 7,
                      .addr = body[1] | (u64{body[2] & 0xFFFF} << 32),
                      .data = body[3]};
        if (fence.eos_command == 1) {
            fence.gds_offset = body[3] & 0xFFFF; // bytes, as the bridge reads it (unverified: docs §3)
            fence.gds_dwords = body[3] >> 16;
            ++stats_.gds_fences;
        } else if (fence.eos_command != 2) {
            return unsupported(fmt::format("EOS command {}", fence.eos_command));
        }
        return Fence(q, fence);
    }
    case O::ReleaseMem: {
        if (body.size() < 6) {
            return short_packet(6);
        }
        FenceOp fence{.kind = FenceKind::Release,
                      .queue = q.id,
                      .packet_addr = addr,
                      .event_type = body[0] & 0x3F,
                      .data_sel = (body[1] >> 29) & 7,
                      .int_sel = (body[1] >> 24) & 7,
                      .addr = body[2] | (u64{body[3]} << 32),
                      .data = body[4] | (u64{body[5]} << 32)};
        if (fence.data_sel == 5) {
            fence.gds_offset = body[4] & 0xFFFF; // bytes, as the bridge reads it (unverified: docs §3)
            fence.gds_dwords = body[4] >> 16;
            ++stats_.gds_fences;
        } else if (fence.data_sel > 4) {
            return unsupported(fmt::format("data_sel {}", fence.data_sel));
        }
        if (fence.int_sel > 3 || (fence.int_sel == 1 && fence.data_sel != 0)) { // as EOP: 1 = interrupt only
            return unsupported(fmt::format("int_sel {} data_sel {}", fence.int_sel, fence.data_sel));
        }
        return Fence(q, fence);
    }
    case O::WriteData: {
        if (body.size() < 3) {
            return short_packet(3);
        }
        const u32 dst_sel = (body[0] >> 8) & 0xF;
        if ((dst_sel != 2 && dst_sel != 5) || ((body[0] >> 16) & 1) != 0) {
            return unsupported(fmt::format("dst_sel {}{}", dst_sel, ((body[0] >> 16) & 1) ? ", one address" : ""));
        }
        const u64 dst = body[1] | (u64{body[2]} << 32);
        const auto data = body.subspan(3);
        ++stats_.writes;
        stats_.write_bytes += data.size_bytes();
        DmaOp w{.kind = DmaKind::DataToMemory,
                .queue = q.id,
                .packet_addr = addr,
                .dst = dst,
                .bytes = static_cast<u32>(data.size_bytes())};
        if (options_.mode == Mode::Isolated && !data.empty()) {
            if (options_.work->WriteOnTimeline(q.id, q.work_seq + 1, dst, data.data(), data.size_bytes())) {
                ++q.work_seq; // on the GPU's timeline: fences after it wait for it
                ++stats_.timeline_writes;
            } else {
                if (options_.memory->Unordered(dst, data.size_bytes())) {
                    ++stats_.unordered_writes; // no GPU work reads it: the earlier work need not complete first
                } else {
                    ++stats_.sync_write_data;
                    const auto t0 = std::chrono::steady_clock::now();
                    options_.work->Synchronize(q.id);
                    stats_.sync_write_data_ns += static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                            .count());
                }
                if (!options_.memory->Write(dst, data.data(), data.size_bytes())) {
                    return Fail(q, CpErrorKind::MemoryWriteFailed, addr, op, n,
                                fmt::format("{} bytes at {:#x}", data.size_bytes(), dst));
                }
            }
            w.executed = true;
        }
        // the data is in the packet: the processor's copy of later packets it lands in takes it in both modes
        w.command_patch = PatchCommands(q, dst, reinterpret_cast<const u8*>(data.data()), w.bytes, 0) > 0;
        options_.work->Dma(w);
        return StepStatus::Packet;
    }
    case O::IndirectBuffer:
        return IndirectBuffer(q, addr, p);
    case O::ClearState:
        if (q.compute) {
            return graphics_only();
        }
        regs_.ClearContext();
        return StepStatus::Packet;
    case O::ContextControl:
        // Register shadowing for context switches: the processor keeps every register itself, nothing to load.
        if (body.size() < 2) {
            return short_packet(2);
        }
        return StepStatus::Packet;
    case O::PfpSyncMe:
        return StepStatus::Packet; // the prefetch parser waits for the micro engine: one in-order engine here
    default:
        break;
    }
    return Fail(q, Pm4::OpcodeName(op).empty() ? CpErrorKind::UnknownOpcode : CpErrorKind::UnsupportedOpcode, addr, op,
                n, {});
}

StepStatus CommandProcessor::SetRegs(Queue& q, Frame& f, u64 addr, std::span<const u32> p, Space space) {
    R8_CP_PROF_LIGHT(CpProfile::Light::SetRegs);
    const u8 op = static_cast<u8>(p[0] >> 8);
    if (p.size() < 2) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, static_cast<u32>(p.size()), "no register offset");
    }
    const u32 offset = p[1] & 0xFFFF;
    if ((p[1] >> 16) != 0) {
        ++stats_.reg_index_nonzero; // CI's index field (SET_*_REG_INDEX forms): reserved here, ignored
    }
    const u32 count = static_cast<u32>(p.size() - 2);
    const u32* values = p.data() + 2;
    const auto out_of_range = [&] {
        return Fail(q, CpErrorKind::RegisterRange, addr, op, static_cast<u32>(p.size()),
                    fmt::format("{} registers from {} offset {:#x}", count, SpaceName(space), offset));
    };
    if (space != Space::Sh) {
        if (!regs_.Write(space, offset, values, count)) {
            return out_of_range();
        }
        if (space == Space::Context) {
            ExtentHint(f, kSpaceBase[static_cast<u32>(Space::Context)] + offset, count);
        }
        return StepStatus::Packet;
    }
    // SH: the compute registers belong to the queue, the rest to the graphics register file. The whole range is
    // checked first: a refused packet writes nothing.
    if (offset > kSpaceCount[static_cast<u32>(Space::Sh)] || count > kSpaceCount[static_cast<u32>(Space::Sh)] - offset) {
        return out_of_range();
    }
    if (q.compute && (offset < kComputeShFirst || offset + count > kComputeShEnd)) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, static_cast<u32>(p.size()),
                    fmt::format("graphics SH registers set from a compute queue (offset {:#x}, {} registers)", offset,
                                count));
    }
    const u32 marker = inline_marker_;
    inline_marker_ = 0;
    for (u32 i = 0; i < count;) {
        const u32 o = offset + i;
        if (o >= kComputeShFirst && o < kComputeShEnd) {
            const u32 k = std::min(count - i, kComputeShEnd - o);
            q.compute_regs.Write(o - kComputeShFirst, values + i, k);
            i += k;
            continue;
        }
        if (q.compute) {
            return Fail(q, CpErrorKind::UnsupportedForm, addr, op, static_cast<u32>(p.size()),
                        fmt::format("graphics SH register {:#x} set from a compute queue", kShBase + o));
        }
        const u32 k = o < kComputeShFirst ? std::min(count - i, kComputeShFirst - o) : count - i;
        if (!regs_.Write(Space::Sh, o, values + i, k)) {
            return out_of_range();
        }
        // user data slots: an inline V#/T#/S# right after its marker, or a plain value replacing one
        for (u32 s = 0; s < static_cast<u32>(Stage::Count); ++s) {
            const u32 ud = kStageBase[s] + 4 - kShBase;
            const u32 lo = std::max(o, ud), hi = std::min(o + k, ud + 16);
            for (u32 r = lo; r < hi; ++r) {
                const u16 bit = static_cast<u16>(1u << (r - ud));
                inline_sharps_[s] = marker != 0 ? static_cast<u16>(inline_sharps_[s] | bit)
                                                : static_cast<u16>(inline_sharps_[s] & ~bit);
            }
        }
        i += k;
    }
    return StepStatus::Packet;
}

void CommandProcessor::ExtentHint(const Frame& f, u32 reg, u32 count) {
    // The PS4 driver follows a colour/depth target's setup with a one-dword NOP carrying the target's real size
    // (the registers only give the tile-aligned pitch). The same sequences as the bridge's Liverpool reads them,
    // bounded by the buffer: a missing hint is counted, never read past the buffer's end.
    const auto hint = [&f, this](u32 after, u32& out) {
        if (u64{f.pos} + after + 1 < f.size && f.data[f.pos + after] == kNopHint) {
            out = f.data[f.pos + after + 1];
            ++stats_.extent_hints;
        } else {
            out = 0;
            ++stats_.extent_hints_missing;
        }
    };
    if (reg >= Reg::CbColor0Base && reg < Reg::CbColor0Base + 8 * Reg::CbColorStride) {
        const u32 t = (reg - Reg::CbColor0Base) / Reg::CbColorStride;
        const u32 field = (reg - Reg::CbColor0Base) % Reg::CbColorStride;
        if (field == 0) { // CB_COLORn_BASE
            if (count == 0x0e || count == 0x0d || count == 0x0b) {
                hint(0, cb_extent_[t]);
            } else {
                cb_extent_[t] = 0;
            }
        } else if (field == 7 && count == 4) { // CB_COLORn_CMASK
            hint(0, cb_extent_[t]);
        }
    } else if (reg == Reg::DbZInfo) {
        if (count == 8) {
            hint(12, db_extent_); // 20 dwords after the registers' start: past one more setup packet
        } else {
            db_extent_ = 0;
        }
    }
}

StepStatus CommandProcessor::Draw(Queue& q, u64 addr, std::span<const u32> p, DrawKind kind, bool packet) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const u32 native_run = std::exchange(native_run_, 0u); // (NativeDrawOp's, for this draw only)
    if (q.compute) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, static_cast<u32>(p.size()),
                    "a draw on a compute queue");
    }
    const auto b = p.subspan(1);
    static constexpr u32 kNeed[] = {5, 4, 2, 4, 4};
    const u32 need = kNeed[static_cast<u32>(kind)];
    if (b.size() < need) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, static_cast<u32>(p.size()),
                    fmt::format("{} body dwords, {} needed", b.size(), need));
    }
    // P5.3 direct path: the engine's description in the free dwords of the NOP right after the game's draw packet
    if (packet && !(engine_next_.valid && engine_next_.queue == q.id) && Engine::Render::DirectOn()) {
        const Frame& f = q.frames[q.depth - 1];
        if (u64{f.pos} + 4 <= f.size && f.data[f.pos] == Engine::Render::kDrawNopHeader &&
            f.data[f.pos + 1] == Engine::Render::kDescribeMagic) {
            NativeTimeBegin();
            DescribedDraw(q, f.data[f.pos + 2], f.data[f.pos + 3]);
            NativeTimeEnd();
        }
    }
    DrawDescriptor d;
    // CITY-OPT Phase 5: the engine's tag in the NOP right after the draw packet (a hint: parallel_cp.cpp decides)
    if (packet && engine_runs_) {
        const Frame& f = q.frames[q.depth - 1];
        if (u64{f.pos} + 4 <= f.size) {
            d.engine_run = static_cast<u8>(Engine::Render::DrawRuns::TagOf(f.data + f.pos));
            d.engine_material = Engine::Render::DrawRuns::MaterialChangedOf(f.data + f.pos);
            // SSPC_NATIVE_GBUFFER: the engine descriptors' ring sequence, carried in the NOP's fourth body dword
            if (Engine::Render::DrawRuns::NativeDescOf(f.data + f.pos)) {
                d.engine_native_present = true;
                d.engine_native = Engine::Render::DrawRuns::NativeDescSeq(f.data + f.pos);
            }
        }
    } else if (!packet && native_run != 0 && engine_runs_) {
        // R7: a native draw's tag came with its stream operation (the Native Render Layer keeps the Phase 5/6 runs)
        d.engine_run = static_cast<u8>(native_run & 0xFF);
        d.engine_material = (native_run & Engine::Render::DrawRuns::kMaterialChanged) != 0;
    }
    switch (kind) {
    case DrawKind::Index2:
        SetOne(regs_, Reg::VgtDmaMaxSize, b[0]);
        SetOne(regs_, Reg::VgtDmaBase, b[1]);
        SetOne(regs_, Reg::VgtDmaBaseHi, b[2] & 0xFF);
        SetOne(regs_, Reg::VgtNumIndices, b[3]);
        SetOne(regs_, Reg::VgtDrawInitiator, b[4]);
        d.indexed = true;
        break;
    case DrawKind::IndexOffset2:
        SetOne(regs_, Reg::VgtDmaMaxSize, b[0]);
        SetOne(regs_, Reg::VgtNumIndices, b[2]);
        SetOne(regs_, Reg::VgtDrawInitiator, b[3]);
        d.index_offset = b[1];
        d.indexed = true;
        break;
    case DrawKind::IndexAuto:
        SetOne(regs_, Reg::VgtNumIndices, b[0]);
        SetOne(regs_, Reg::VgtDrawInitiator, b[1]);
        break;
    case DrawKind::IndexIndirect:
        // the index buffer's size (INDEX_BUFFER_SIZE) bounds the indices the GPU fetches
        SetOne(regs_, Reg::VgtDmaMaxSize, regs_.At(kIdxDmaSize));
        SetOne(regs_, Reg::VgtDrawInitiator, b[3]);
        d.indexed = true;
        d.indirect = true;
        break;
    case DrawKind::Indirect:
        SetOne(regs_, Reg::VgtDrawInitiator, b[3]);
        d.indirect = true;
        break;
    }
    if (d.indirect) {
        d.args_addr = set_base_[1] + b[0];
        d.base_vtx_loc = static_cast<u16>(b[1] & 0xFFFF);
        d.start_inst_loc = static_cast<u16>(b[2] & 0xFFFF);
    }
    d.kind = kind;
    d.queue = q.id;
    d.packet_addr = addr;
    BuildDraw(d);
    if (native_expect_ != nullptr && native_expect_queue_ == q.id) {
        VerifyNativeDraw(d);
    }
    if (engine_next_.valid && engine_next_.queue == q.id) {
        d.engine_pass = engine_next_.pass;
        d.engine_vs = engine_next_.vs;
        d.engine_ps = engine_next_.ps;
        engine_next_.valid = false;
    }
    d.seq = ++q.work_seq;
    ++stats_.draws;
    if (native_t0_ != 0) { // (a timed native operation: the executor's part is not the transport's)
        native_acc_ += __rdtsc() - native_t0_;
    }
    options_.work->Draw(d);
    if (native_t0_ != 0) {
        native_t0_ = __rdtsc();
    }
    if (d.engine_run != 0 && run_ahead_ && packet && !in_run_ahead_) {
        RunAhead(q);
    }
    return StepStatus::Packet;
}

void CommandProcessor::RunAhead(Queue& q) {
    if (observer_ != nullptr || options_.mode != Mode::Isolated || options_.packet_ticks || q.depth == 0) {
        return;
    }
    Frame& f = q.frames[q.depth - 1];
    if (f.ring) {
        return; // (a ring's read pointer follows each packet: Step's)
    }
    namespace O = Pm4::Op;
    in_run_ahead_ = true;
    for (;;) {
        // f.pos: the trailing NOP of the tagged draw just made. The packets up to the next draw packet, followed by its
        // tagged NOP: only those a tagged draw sets (none of them blocks, chains or writes memory)
        u32 pos = f.pos, end = 0, dwords = 0;
        while (pos < f.size) {
            const u32 h = f.data[pos];
            if ((h >> 30) != 3 || (h & 1) != 0) {
                break; // (not type 3, or predicated: Step's)
            }
            const u32 n = ((h >> 16) & 0x3FFF) + 2;
            const u8 op = static_cast<u8>(h >> 8);
            if (u64{pos} + n > f.size) {
                break;
            }
            if (op == O::DrawIndex2) {
                if (u64{pos} + n + 4 <= f.size &&
                    Engine::Render::DrawRuns::TagOf(f.data + pos + n) != Engine::Render::DrawRuns::Source::None) {
                    end = pos + n;
                    dwords += n;
                }
                break;
            }
            // (a NOP: the driver's own and the CUE's V#/T#/S# markers - not a flip's, nor SecondSonPC's P5.3 ones)
            const std::span<const u32> nop(f.data + pos, n);
            const Pm4::NopMarker mk = op == O::Nop ? Pm4::ClassifyNop(nop) : Pm4::NopMarker::Other;
            const bool plain_nop = op == O::Nop && mk != Pm4::NopMarker::PatchedFlip &&
                                   mk != Pm4::NopMarker::PrepareFlip && mk != Pm4::NopMarker::PrepareFlipLabel &&
                                   mk != Pm4::NopMarker::PrepareFlipInterrupt &&
                                   mk != Pm4::NopMarker::PrepareFlipInterruptLabel &&
                                   !(n == Engine::Render::kStreamTokenDwords && nop[1] == Engine::Render::kStreamMagic) &&
                                   !Engine::Render::IsMarker(nop);
            const bool uconfig_prim = op == O::SetUconfigReg && n == 3 &&
                                      (f.data[pos + 1] & 0xFFFF) + kSpaceBase[static_cast<u32>(Space::Uconfig)] ==
                                          Reg::VgtPrimitiveType;
            if (!(plain_nop || uconfig_prim || op == O::SetShReg || op == O::IndexType || op == O::NumInstances)) {
                break;
            }
            pos += n;
            dwords += n;
        }
        if (end == 0 || q.budget + dwords > options_.max_dwords_per_submission) {
            ++stats_.ahead_stops;
            break;
        }
        // as Step: each packet's dwords counted, the processor's position past it, Execute, its count
        while (f.pos < end) {
            const u32 n = ((f.data[f.pos] >> 16) & 0x3FFF) + 2;
            const u64 addr = f.addr + u64{f.pos} * 4;
            const std::span<const u32> p(f.data + f.pos, n);
            q.budget += n;
            f.pos += n;
            const StepStatus s = Execute(q, f, addr, p); // (the draw packet's Draw runs the work's draw)
            if (s != StepStatus::Packet) {
                in_run_ahead_ = false;
                return; // (a refused packet: the submission was dropped by Fail)
            }
            ++stats_.graphics_packets[static_cast<u8>(p[0] >> 8)];
            ++stats_.ahead_packets;
        }
        ++stats_.ahead_draws;
    }
    in_run_ahead_ = false;
}

void CommandProcessor::BuildDraw(DrawDescriptor& d) {
    R8_CP_PROF_LIGHT(CpProfile::Light::BuildDraw);
    const GroupMask changed = regs_.Dirty();
    regs_.ClearDirty(changed);
    d.changed = changed;
    if ((changed & kPipelineGroups) != 0) {
        // the groups' hashes are kept by the register file at every change: no pass over their registers here
        u64 key = 0x9e3779b97f4a7c15ull;
        for (GroupMask m = kPipelineGroups; m != 0; m &= m - 1) {
            key = Mix(key ^ regs_.GroupHash(static_cast<Group>(std::countr_zero(m))));
        }
        pipeline_key_ = key;
        ++pipeline_version_;
        ++stats_.pipeline_key_rebuilds;
    }
    if ((changed & kDynamicGroups) != 0) {
        ++dynamic_version_;
    }
    if ((changed & kTargetGroups) != 0) {
        ++targets_version_;
    }
    d.pipeline_key = pipeline_key_;
    d.pipeline_version = pipeline_version_;
    d.dynamic_version = dynamic_version_;
    d.targets_version = targets_version_;

    const RegisterFile& r = regs_;
    d.index_count = r.At(kIdxNumIndices);
    d.instance_count = r.At(kIdxNumInstances);
    d.index_base = (u64{r.At(kIdxDmaBaseHi) & 0xFF} << 32) | (r.At(kIdxDmaBase) & ~1u);
    d.index_type = r.At(kIdxIndexType);
    d.max_index_size = r.At(kIdxMaxSize);
    d.vgt_index_offset = r.At(kIdxIndxOffset);
    d.draw_initiator = r.At(kIdxInitiator);
    d.primitive_type = r.At(kIdxPrimType);
    d.stages_enable = r.At(kIdxStages);
    // VGT_SHADER_STAGES_EN: LS_EN [1:0], HS_EN [2], ES_EN [4:3], GS_EN [5]
    u32 active = (1u << static_cast<u32>(Stage::Ps)) | (1u << static_cast<u32>(Stage::Vs));
    if ((d.stages_enable & 0x3) != 0) {
        active |= 1u << static_cast<u32>(Stage::Ls);
    }
    if ((d.stages_enable & 0x4) != 0) {
        active |= 1u << static_cast<u32>(Stage::Hs);
    }
    if ((d.stages_enable & 0x18) != 0) {
        active |= 1u << static_cast<u32>(Stage::Es);
    }
    if ((d.stages_enable & 0x20) != 0) {
        active |= 1u << static_cast<u32>(Stage::Gs);
    }
    d.active_stages = static_cast<u8>(active);
    const u32* values = r.Values().data();
    for (u32 s = 0; s < static_cast<u32>(Stage::Count); ++s) {
        const u32 i = FlatOf(kStageBase[s]);
        d.shaders[s] = ShaderRef{.address = ((u64{values[i + 1] & 0xFF} << 32) | values[i]) << 8,
                                 .rsrc1 = values[i + 2],
                                 .rsrc2 = values[i + 3]};
        if ((active & (1u << s)) != 0) {
            std::copy_n(values + i + 4, 16, d.user_data[s].begin());
        }
    }
    d.inline_sharps = inline_sharps_;
    d.cb_extent = cb_extent_;
    d.db_extent = db_extent_;
}

StepStatus CommandProcessor::NativeMarker(Queue& q, u64 addr, std::span<const u32> p) {
    namespace R = Engine::Render;
    R::DrawCounters& c = R::Counters();
    c.markers.fetch_add(1, std::memory_order_relaxed);
    u16 flags = 0;
    if (R::MarkerKind(p) == R::OpKind::State) {
        // P5.3D native render state. kApply: the writes the game's state packets would make, and the target extents
        // their NOP hints give (ExtentHint). kVerify: the game's packets ran just before; what they left must be it.
        R::RegWrite writes[R::kMaxStateWrites];
        u32 count = 0;
        if (q.compute || !R::DecodeRegistersMarker(p, writes, count, flags, R::OpKind::State, R::kMaxStateWrites) ||
            (flags & (R::kApply | R::kVerify)) == 0) {
            c.malformed.fetch_add(1, std::memory_order_relaxed);
            return StepStatus::Packet;
        }
        const auto extent = [this](u32 reg) -> u32& {
            return reg == R::kStateDbExtent ? db_extent_ : cb_extent_[reg - R::kStateCbExtent0];
        };
        if ((flags & R::kApply) != 0) {
            for (u32 i = 0; i < count; ++i) {
                if (R::IsStatePseudo(writes[i].reg)) {
                    extent(writes[i].reg) = writes[i].value;
                } else if (!regs_.WriteReg(writes[i].reg, writes[i].value)) {
                    c.malformed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            c.state_applied.fetch_add(1, std::memory_order_relaxed);
            c.state_applied_writes.fetch_add(count, std::memory_order_relaxed);
            return StepStatus::Packet;
        }
        bool match = true;
        for (u32 i = 0; i < count; ++i) {
            const u32 reg = writes[i].reg;
            const u32 have = R::IsStatePseudo(reg) ? extent(reg) : regs_.Get(reg);
            if (have != writes[i].value) {
                match = false;
                u32 none = 0;
                if (c.state_first_bad_reg.compare_exchange_strong(none, reg)) {
                    c.state_first_bad_want.store(writes[i].value);
                    c.state_first_bad_have.store(have);
                }
            }
        }
        c.state_values_checked.fetch_add(count, std::memory_order_relaxed);
        c.state_verified.fetch_add(1, std::memory_order_relaxed);
        (match ? c.state_matched : c.state_mismatched).fetch_add(1, std::memory_order_relaxed);
        return StepStatus::Packet;
    }
    if (R::MarkerKind(p) == R::OpKind::Registers) {
        // A native CUE commit (P5.3C): its register writes go straight to the register file, as SET_SH_REG /
        // SET_CONTEXT_REG would have put them (user-data slots written this way hold no inline V#/T#/S#).
        R::RegWrite writes[R::kMaxRegWrites];
        u32 count = 0;
        if (q.compute || !R::DecodeRegistersMarker(p, writes, count, flags) || (flags & R::kApply) == 0) {
            c.malformed.fetch_add(1, std::memory_order_relaxed);
            return StepStatus::Packet;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 reg = writes[i].reg;
            if (!regs_.WriteReg(reg, writes[i].value)) {
                c.malformed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (u32 s = 0; s < static_cast<u32>(Stage::Count); ++s) {
                const u32 ud = kStageBase[s] + 4;
                if (reg >= ud && reg < ud + 16) {
                    inline_sharps_[s] = static_cast<u16>(inline_sharps_[s] & ~(1u << (reg - ud)));
                }
            }
        }
        c.applied_commits.fetch_add(1, std::memory_order_relaxed);
        c.applied_registers.fetch_add(count, std::memory_order_relaxed);
        return StepStatus::Packet;
    }
    R::DrawOp op;
    if (!R::DecodeDrawMarker(p, op, flags) || (flags & (R::kVerify | R::kApply | R::kDescribe)) == 0) {
        c.malformed.fetch_add(1, std::memory_order_relaxed);
        return StepStatus::Packet;
    }
    PassCaptureMarker(op.pass, (flags & R::kApply) != 0);
    if (native_expect_ != nullptr) {
        c.orphaned.fetch_add(1, std::memory_order_relaxed); // the previous marker met no draw
        native_expect_.reset();
    }
    // P5.3: the description goes with the queue's next draw (this marker's own draw in apply mode, else the game's
    // packet right after it); a marker that meets no draw leaves nothing behind
    if (engine_next_.valid) {
        c.described_orphaned.fetch_add(1, std::memory_order_relaxed);
    }
    // (a kVerify-only marker leaves the game's path as it is: the A/B comparison's PM4 blocks are the legacy path)
    engine_next_.valid = !op.canary && (flags & (R::kApply | R::kDescribe)) != 0;
    engine_next_.queue = q.id;
    engine_next_.pass = op.pass;
    const auto table = [](const R::StageTable& t, EngineTable& out) {
        out = {};
        if (!t.present || t.refs == 0 || t.dwords == 0) {
            return;
        }
        out.dwords = t.dwords;
        out.refs = static_cast<u8>(t.refs);
        for (u32 i = 0; i < t.refs; ++i) {
            out.sgpr[i] = t.ref[i].sgpr;
            out.offset[i] = t.ref[i].offset;
        }
    };
    table(op.table[R::kStageVs], engine_next_.vs);
    table(op.table[R::kStagePs], engine_next_.ps);
    if ((flags & (R::kVerify | R::kApply)) == 0) {
        c.described.fetch_add(1, std::memory_order_relaxed);
        return StepStatus::Packet; // kDescribe: the game's draw packet follows
    }
    const bool apply = (flags & R::kApply) != 0;
    const bool indexed = op.indexed;
    const u32 count = op.index_count;
    const u64 base = op.index_base;
    if ((flags & R::kVerify) != 0) {
        native_expect_ = std::make_unique<R::DrawOp>(op); // (only a verified draw keeps its operation)
        native_expect_queue_ = q.id;
    }
    if (!apply) {
        return StepStatus::Packet;
    }
    // P5.3C: the engine issued no draw packet; the draw is executed from the operation through the same draw entry
    // (registers, BuildDraw, the executor) the packet would have reached - what sceGnmDrawIndex/DrawIndexAuto write.
    c.applied_draws.fetch_add(1, std::memory_order_relaxed);
    const auto header = [](u8 opcode, u32 body_dwords) {
        return 0xC0000000u | ((body_dwords - 1) << 16) | (u32{opcode} << 8);
    };
    if (indexed) {
        const u32 body[6] = {header(Pm4::Op::DrawIndex2, 5), count, static_cast<u32>(base),
                             static_cast<u32>(base >> 32), count, 0u};
        return Draw(q, addr, body, DrawKind::Index2, false);
    }
    const u32 body[3] = {header(Pm4::Op::DrawIndexAuto, 2), count, 2u};
    return Draw(q, addr, body, DrawKind::IndexAuto, false);
}

// --- P5.3 direct path (engine/render/native_render_ops.h) ---

namespace {

/// The engine's interned table shape as the frontend takes it (an unknown or empty one: no table).
void TableFromShape(const Engine::Render::TableShape* s, EngineTable& out) {
    out = {};
    if (s == nullptr || s->refs == 0 || s->dwords == 0 || s->refs > 7) {
        return;
    }
    out.dwords = s->dwords;
    out.refs = s->refs;
    for (u32 i = 0; i < s->refs; ++i) {
        out.sgpr[i] = s->sgpr[i];
        out.offset[i] = s->offset[i];
    }
}

} // namespace

void CommandProcessor::NativeTimeBegin() {
    if ((++native_calls_ & 7) == 0) {
        native_acc_ = 0;
        native_t0_ = __rdtsc();
    }
}

void CommandProcessor::NativeTimeEnd() {
    if (native_t0_ == 0) {
        return;
    }
    native_acc_ += __rdtsc() - native_t0_;
    native_t0_ = 0;
    Engine::Render::DirectCounters& d = Engine::Render::Direct();
    d.native_tsc.fetch_add(native_acc_ * 8, std::memory_order_relaxed);
    d.native_calls.fetch_add(8, std::memory_order_relaxed);
}

void CommandProcessor::DescribedDraw(Queue& q, u32 word, u32 ps_shape) {
    namespace R = Engine::Render;
    R::DirectCounters& dc = R::Direct();
    const u32 vs_shape = word >> 8;
    const R::TableShape* vs = R::ShapeOf(vs_shape);
    const R::TableShape* ps = R::ShapeOf(ps_shape);
    if ((vs_shape != 0 && vs == nullptr) || (ps_shape != 0 && ps == nullptr)) {
        dc.described_bad.fetch_add(1, std::memory_order_relaxed);
        return; // (the draw goes undescribed: the frontend's own path)
    }
    const s32 pass = R::DescribedPass(word);
    PassCaptureMarker(pass, false);
    engine_next_.valid = true;
    engine_next_.queue = q.id;
    engine_next_.pass = pass;
    TableFromShape(vs, engine_next_.vs);
    TableFromShape(ps, engine_next_.ps);
    dc.described.fetch_add(1, std::memory_order_relaxed);
}

void CommandProcessor::ApplyNativeRegisters(const u32* pairs, u32 count) {
    // as the native commit's marker (NativeMarker, OpKind::Registers)
    for (u32 i = 0; i < count; ++i) {
        const u32 reg = pairs[2 * i];
        if (!regs_.WriteReg(reg, pairs[2 * i + 1])) {
            Engine::Render::Counters().malformed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (u32 s = 0; s < static_cast<u32>(Stage::Count); ++s) {
            const u32 ud = kStageBase[s] + 4;
            if (reg >= ud && reg < ud + 16) {
                inline_sharps_[s] = static_cast<u16>(inline_sharps_[s] & ~(1u << (reg - ud)));
            }
        }
    }
}

bool CommandProcessor::ApplyNativeState(const u32* pairs, u32 count) {
    // as a kApply state marker (NativeMarker, OpKind::State)
    namespace R = Engine::Render;
    bool ok = true;
    for (u32 i = 0; i < count; ++i) {
        const u32 reg = pairs[2 * i], value = pairs[2 * i + 1];
        if (R::IsStatePseudo(reg)) {
            (reg == R::kStateDbExtent ? db_extent_ : cb_extent_[reg - R::kStateCbExtent0]) = value;
        } else if (!regs_.WriteReg(reg, value)) {
            R::Counters().malformed.fetch_add(1, std::memory_order_relaxed);
            ok = false;
        }
    }
    return ok;
}

StepStatus CommandProcessor::NativeDrawOp(Queue& q, u64 addr, s32 pass, u32 bits, u32 count, u64 base,
                                          u32 vs_shape, u32 ps_shape) {
    namespace R = Engine::Render;
    const bool indexed = (bits & R::kStreamDrawIndexed) != 0;
    native_run_ = (bits & R::kStreamDrawRunMask) >> R::kStreamDrawRunShift; // (taken by Draw)
    PassCaptureMarker(pass, true);
    engine_next_.valid = true;
    engine_next_.queue = q.id;
    engine_next_.pass = pass;
    TableFromShape(R::ShapeOf(vs_shape), engine_next_.vs);
    TableFromShape(R::ShapeOf(ps_shape), engine_next_.ps);
    R::Counters().applied_draws.fetch_add(1, std::memory_order_relaxed);
    // what sceGnmDrawIndex / DrawIndexAuto write (as a kApply draw marker)
    const auto header = [](u8 opcode, u32 body_dwords) {
        return 0xC0000000u | ((body_dwords - 1) << 16) | (u32{opcode} << 8);
    };
    if (indexed) {
        const u32 body[6] = {header(Pm4::Op::DrawIndex2, 5), count, static_cast<u32>(base),
                             static_cast<u32>(base >> 32), count, 0u};
        return Draw(q, addr, body, DrawKind::Index2, false);
    }
    const u32 body[3] = {header(Pm4::Op::DrawIndexAuto, 2), count, 2u};
    return Draw(q, addr, body, DrawKind::IndexAuto, false);
}

StepStatus CommandProcessor::NativeRun(Queue& q, u64 addr, std::span<const u32> p) {
    namespace R = Engine::Render;
    R::DirectCounters& dc = R::Direct();
    R::DrawCounters& c = R::Counters();
    dc.runs.fetch_add(1, std::memory_order_relaxed);
    R::StreamRun run;
    if (q.compute || !R::StreamTake(p, run)) {
        dc.runs_stale.fetch_add(1, std::memory_order_relaxed);
        return StepStatus::Packet;
    }
    if (native_expect_ != nullptr) {
        c.orphaned.fetch_add(1, std::memory_order_relaxed); // a verification marker met no draw
        native_expect_.reset();
    }
    StepStatus status = StepStatus::Packet;
    u32 ops = 0, draws = 0, commits = 0, reg_writes = 0, states = 0, state_writes = 0;
    for (u32 i = 0; i < run.count && status == StepStatus::Packet;) {
        const u32 h = run.words[i];
        const u32 n = h >> 8;
        const u32* body = run.words + i + 1;
        if (n > run.count - i - 1) {
            c.malformed.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        i += 1 + n;
        ++ops;
        switch (static_cast<R::StreamOp>(h & 0xFF)) {
        case R::StreamOp::Registers:
            ApplyNativeRegisters(body, n / 2);
            ++commits;
            reg_writes += n / 2;
            break;
        case R::StreamOp::State:
            ApplyNativeState(body, n / 2);
            ++states;
            state_writes += n / 2;
            break;
        case R::StreamOp::Draw:
            if (n != R::kStreamDrawWords) {
                c.malformed.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            status = NativeDrawOp(q, addr, static_cast<s32>(body[0]), body[1], body[2],
                                  u64{body[3]} | (u64{body[4]} << 32), body[5], body[6]);
            ++draws;
            break;
        case R::StreamOp::Verify: {
            // R7: the engine's description of the run's next draw, compared with it as a kVerify marker's
            R::DrawOp op;
            u16 flags = 0;
            if (!R::DecodeDrawMarker(std::span<const u32>(body, n), op, flags) || (flags & R::kVerify) == 0) {
                c.malformed.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (native_expect_ != nullptr) {
                c.orphaned.fetch_add(1, std::memory_order_relaxed);
            }
            native_expect_ = std::make_unique<R::DrawOp>(op);
            native_expect_queue_ = q.id;
            break;
        }
        default:
            c.malformed.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    R::StreamDone(run);
    dc.ops.fetch_add(ops, std::memory_order_relaxed);
    dc.draws.fetch_add(draws, std::memory_order_relaxed);
    dc.register_writes.fetch_add(reg_writes, std::memory_order_relaxed);
    dc.state_writes.fetch_add(state_writes, std::memory_order_relaxed);
    c.applied_commits.fetch_add(commits, std::memory_order_relaxed);
    c.applied_registers.fetch_add(reg_writes, std::memory_order_relaxed);
    c.state_applied.fetch_add(states, std::memory_order_relaxed);
    c.state_applied_writes.fetch_add(state_writes, std::memory_order_relaxed);
    return status;
}

void CommandProcessor::PassCaptureMarker(s32 pass, bool applied) {
    PassCapture& c = pass_capture_;
    if (!c.parsed) {
        c.parsed = true;
        char s[128] = {};
        if (GetEnvironmentVariableA("SSPC_P53_CAPTURE_PASSES", s, sizeof(s)) > 0) {
            for (char* p = s; *p != 0;) {
                const unsigned long v = std::strtoul(p, &p, 10);
                if (v < 64) {
                    c.passes |= u64{1} << v;
                }
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
        }
        if (GetEnvironmentVariableA("SSPC_P53_CAPTURE_FLIPS", s, sizeof(s)) > 0) {
            char* p = s;
            c.first = std::strtoull(p, &p, 10);
            c.count = *p == '+' ? std::strtoull(p + 1, &p, 10) : 1;
        }
        if (c.passes != 0) {
            SSPC_LOG(GPU, Warning,
                     "DIAGNOSTIC: SSPC_P53_CAPTURE_PASSES - the targets of passes {:#x} (bits) captured where their "
                     "draws end, in flips {} .. {} (the processor's count)",
                     c.passes, c.first, c.first + c.count);
        }
    }
    if (c.passes == 0) {
        return;
    }
    const auto wanted = [&c](s32 p) { return p >= 0 && p < 64 && ((c.passes >> p) & 1) != 0; };
    if (pass != c.pass) {
        if (!c.targets.empty() && wanted(c.pass)) {
            options_.work->CaptureTargets(c.targets.data(), static_cast<u32>(c.targets.size()), c.pass, c.native,
                                          c.pm4);
        }
        c.targets.clear();
        c.native = c.pm4 = 0;
        c.pass = pass;
    }
    const u64 flips = Engine::Render::Control().flips.load(std::memory_order_relaxed);
    if (!wanted(pass) || flips < c.first || flips >= c.first + c.count) {
        return;
    }
    ++(applied ? c.native : c.pm4);
    // What the draw after this marker writes: the bound colour targets (CB_COLORn_INFO.FORMAT set) and the depth target.
    const auto add = [&c](u64 address) {
        if (address != 0 && std::find(c.targets.begin(), c.targets.end(), address) == c.targets.end()) {
            c.targets.push_back(address);
        }
    };
    for (u32 n = 0; n < 8; ++n) {
        const u32 base = Reg::CbColor0Base + n * Reg::CbColorStride;
        if (((regs_.Get(base + 4) >> 2) & 0x1F) != 0) { // CB_COLORn_INFO
            add(u64{regs_.Get(base)} << 8);
        }
    }
    if ((regs_.Get(Reg::DbZInfo) & 3) != 0) {
        add(u64{regs_.Get(Reg::DbZInfo + 4)} << 8); // DB_Z_WRITE_BASE
    }
}

void CommandProcessor::VerifyNativeDraw(const DrawDescriptor& d) {
    namespace R = Engine::Render;
    const std::unique_ptr<R::DrawOp> op = std::move(native_expect_);
    R::DrawView view;
    view.indexed = d.indexed;
    view.index_count = d.index_count;
    view.index_base = d.index_base;
    for (u32 i = 0; i < R::kVsRegs; ++i) {
        view.vs[i] = regs_.Get(R::kVsRegIndex[i]);
    }
    for (u32 i = 0; i < R::kPsRegs; ++i) {
        view.ps[i] = regs_.Get(R::kPsRegIndex[i]);
    }
    view.user_data[R::kStageVs] = d.user_data[static_cast<size_t>(Stage::Vs)].data();
    view.user_data[R::kStagePs] = d.user_data[static_cast<size_t>(Stage::Ps)].data();
    const u32 mismatch = R::VerifyDraw(*op, view, &R::HashGuestMemory);

    R::DrawCounters& c = R::Counters();
    if (op->canary) {
        // Spoiled on purpose by the engine side: proves the comparison sees what it should; not a real mismatch.
        c.canaries.fetch_add(1, std::memory_order_relaxed);
        if ((mismatch & R::kMismatchIndexCount) != 0) {
            c.canaries_caught.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    for (const R::StageTable& t : op->table) {
        if (t.present && t.refs != 0 && t.dwords != 0) {
            c.tables_checked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const u32 slot = R::PassSlot(op->pass);
    c.verified.fetch_add(1, std::memory_order_relaxed);
    c.pass_verified[slot].fetch_add(1, std::memory_order_relaxed);
    if (!op->shaders) {
        c.shaders_unknown.fetch_add(1, std::memory_order_relaxed);
    }
    if (mismatch == 0) {
        c.matched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    c.mismatched.fetch_add(1, std::memory_order_relaxed);
    c.pass_mismatched[slot].fetch_add(1, std::memory_order_relaxed);
    for (u32 i = 0; i < R::kMismatchFields; ++i) {
        if ((mismatch & (1u << i)) != 0) {
            c.field[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (native_logged_ < 20) {
        ++native_logged_;
        std::string regs;
        for (u32 i = 0; i < R::kVsRegs; ++i) {
            if (op->shaders && op->vs[i] != view.vs[i]) {
                regs += fmt::format(" VS[{:#x}] {:#x}!={:#x}", R::kVsRegIndex[i], op->vs[i], view.vs[i]);
            }
        }
        for (u32 i = 0; i < R::kPsRegs; ++i) {
            if (op->shaders && op->has_ps && op->ps[i] != view.ps[i]) {
                regs += fmt::format(" PS[{:#x}] {:#x}!={:#x}", R::kPsRegIndex[i], op->ps[i], view.ps[i]);
            }
        }
        SSPC_LOG(GPU, Warning,
                 "native draw mismatch {:#x} at {:#x} (pass {} sub {}): engine {} count {} base {:#x}, PM4 {} count {} "
                 "base {:#x}; tables VS {}x{} PS {}x{}{}",
                 mismatch, d.packet_addr, op->pass, op->sub, op->indexed ? "indexed" : "auto", op->index_count,
                 op->index_base, d.indexed ? "indexed" : "auto", d.index_count, d.index_base,
                 op->table[R::kStageVs].refs, op->table[R::kStageVs].dwords, op->table[R::kStagePs].refs,
                 op->table[R::kStagePs].dwords, regs);
    }
}

StepStatus CommandProcessor::Dispatch(Queue& q, u64 addr, std::span<const u32> p, bool indirect) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const u32 n = static_cast<u32>(p.size());
    const auto b = p.subspan(1);
    ComputeRegisters& cr = q.compute_regs;
    DispatchDescriptor d;
    u32 initiator = 0;
    if (!indirect) {
        if (b.size() < 4) {
            return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "4 body dwords needed");
        }
        initiator = b[3];
        // the group counts written into this packet by an earlier write of the submission (Gnm's indirect dispatch
        // on a compute queue: a DMA from the counts' buffer into the packet)
        const u64 counts = addr + 4;
        for (u32 i = 0; i < q.patch_count; ++i) {
            const auto& pt = q.patches[i];
            const bool covers = pt.dst <= counts && counts + 12 <= pt.dst + pt.bytes;
            const bool overlaps = pt.dst < counts + 12 && counts < pt.dst + pt.bytes;
            if (covers) {
                d.patched = true;
                if (!pt.applied) { // Shadow: the GPU copies the counts when it gets there
                    if (pt.src == 0) {
                        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n,
                                    "group counts written from GDS into the packet");
                    }
                    d.indirect = true;
                    d.indirect_addr = pt.src + (counts - pt.dst);
                } else {
                    d.indirect = false; // the processor's copy holds them (a later write wins)
                }
            } else if (overlaps) {
                return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n,
                            "a write into part of this dispatch's group counts");
            }
        }
        if (!d.indirect) {
            cr.Write(Reg::ComputeDimX, b.data(), 3); // COMPUTE_DIM_X/Y/Z
        }
    } else {
        if (q.compute) { // MEC form: the address in the packet
            if (b.size() < 3) {
                return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "3 body dwords needed");
            }
            d.indirect_addr = b[0] | (u64{b[1] & 0xFFFF} << 32);
            initiator = b[2];
        } else { // graphics form: an offset from SET_BASE's base
            if (b.size() < 2) {
                return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "2 body dwords needed");
            }
            d.indirect_addr = set_base_[1] + b[0];
            initiator = b[1];
        }
        d.indirect = true;
    }
    cr.Write(Reg::ComputeDispatchInitiator, &initiator, 1);
    if ((initiator & 1) == 0) { // COMPUTE_SHADER_EN off: nothing is launched
        ++stats_.dispatches_disabled;
        return StepStatus::Packet;
    }
    const auto& v = cr.values;
    if (!d.indirect) {
        d.groups = {v[Reg::ComputeDimX], v[Reg::ComputeDimX + 1], v[Reg::ComputeDimX + 2]};
    }
    d.initiator = initiator;
    d.threads = {v[0x07] & 0xFFFF, v[0x08] & 0xFFFF, v[0x09] & 0xFFFF};
    d.shader = ShaderRef{.address = ((u64{v[Reg::ComputePgmLo + 1] & 0xFF} << 32) | v[Reg::ComputePgmLo]) << 8,
                         .rsrc1 = v[Reg::ComputePgmRsrc1],
                         .rsrc2 = v[Reg::ComputePgmRsrc1 + 1]};
    std::copy_n(v.begin() + Reg::ComputeUserData0, 16, d.user_data.begin());
    d.program_version = cr.versions[static_cast<u32>(ComputeGroup::Program)];
    d.user_data_version = cr.versions[static_cast<u32>(ComputeGroup::UserData)];
    cr.dirty = 0;
    d.queue = q.id;
    d.packet_addr = addr;
    d.seq = ++q.work_seq;
    ++stats_.dispatches;
    options_.work->Dispatch(d);
    return StepStatus::Packet;
}

bool CommandProcessor::OverlapsPendingCommands(const Queue& q, u64 dst, u32 bytes) const {
    for (u32 i = 0; i < q.depth; ++i) {
        const Frame& f = q.frames[i];
        const u64 lo = f.addr + u64{f.pos} * 4, hi = f.addr + u64{f.size} * 4;
        if (std::max(dst, lo) < std::min(dst + bytes, hi)) {
            return true;
        }
    }
    return false;
}

u32 CommandProcessor::PatchCommands(Queue& q, u64 dst, const u8* bytes_known, u32 bytes, u64 src_addr) {
    // A write into packets of this queue not processed yet: the processor reads them later, so it must see it. With
    // the bytes known (WRITE_DATA, a fill, Isolated's DMA) the processor's copy takes them; otherwise (Shadow's DMA from
    // memory) the patch names the source the GPU will copy from.
    u32 hits = 0;
    for (u32 i = 0; i < q.depth; ++i) {
        Frame& f = q.frames[i];
        const u64 lo = f.addr + u64{f.pos} * 4;
        const u64 hi = f.addr + u64{f.size} * 4;
        const u64 a = std::max(dst, lo), e = std::min(dst + bytes, hi);
        if (a >= e) {
            continue;
        }
        ++hits;
        ++stats_.dma_command_patches;
        if (q.patch_count < kMaxPatches) {
            q.patches[q.patch_count++] = {dst, bytes, src_addr, bytes_known != nullptr};
        } else {
            q.patch_overflow = true; // the caller refuses the packet when the bytes are not known (Shadow)
        }
        if (bytes_known == nullptr) {
            continue;
        }
        if (!f.owned) { // the caller's copy is read-only: take one of our own
            auto& buf = q.buffers[i];
            if (buf.capacity() < f.size) {
                ++stats_.buffer_growths;
            }
            buf.assign(f.data, f.data + f.size);
            f.data = buf.data();
            f.owned = true;
        }
        std::memcpy(reinterpret_cast<u8*>(const_cast<u32*>(f.data)) + (a - f.addr), bytes_known + (a - dst), e - a);
    }
    return hits;
}

StepStatus CommandProcessor::DmaData(Queue& q, u64 addr, std::span<const u32> p) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const u32 n = static_cast<u32>(p.size());
    const auto b = p.subspan(1);
    if (b.size() < 6) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "6 body dwords needed");
    }
    const u32 src_sel = (b[0] >> 29) & 3, dst_sel = (b[0] >> 20) & 3;
    const u32 bytes = b[5] & 0x1FFFFF;
    const bool sas = (b[5] >> 26) & 1, das = (b[5] >> 27) & 1, saic = (b[5] >> 28) & 1, daic = (b[5] >> 29) & 1;
    const u64 src = b[1] | (u64{b[2]} << 32);
    const u64 dst = b[3] | (u64{b[4]} << 32);
    DmaOp d{.queue = q.id, .packet_addr = addr, .src = src, .dst = dst, .bytes = bytes};
    ++stats_.dma;
    if (dst_sel != 1 && dst == kReadTags) {
        // Gnm's prefetch: the source read into the L2 through a CP register; no memory changes
        d.kind = DmaKind::ReadTagsPrefetch;
        ++stats_.dma_prefetch;
        options_.work->Dma(d);
        return StepStatus::Packet;
    }
    if (sas || das || saic || daic) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n,
                    fmt::format("address spaces/no-increment sas {} das {} saic {} daic {}", sas, das, saic, daic));
    }
    const bool src_mem = src_sel == 0 || src_sel == 3, dst_mem = dst_sel == 0 || dst_sel == 3;
    if (src_mem && dst_mem) {
        d.kind = DmaKind::MemoryToMemory;
    } else if (src_sel == 2 && dst_mem) {
        d.kind = DmaKind::DataToMemory;
        d.src = b[1];
    } else if (src_mem && dst_sel == 1) {
        d.kind = DmaKind::MemoryToGds;
    } else if (src_sel == 2 && dst_sel == 1) {
        d.kind = DmaKind::DataToGds;
        d.src = b[1];
    } else if (src_sel == 1 && dst_mem) {
        d.kind = DmaKind::GdsToMemory;
    } else {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n, fmt::format("src_sel {} dst_sel {}", src_sel, dst_sel));
    }
    const bool gds = d.kind == DmaKind::MemoryToGds || d.kind == DmaKind::DataToGds || d.kind == DmaKind::GdsToMemory;
    const u64 gds_off = d.kind == DmaKind::GdsToMemory ? (src & 0xFFFFFFFF) : (dst & 0xFFFFFFFF);
    if (gds && (gds_off > kGdsBytes || bytes > kGdsBytes - gds_off)) {
        return Fail(q, CpErrorKind::GdsRange, addr, op, n, fmt::format("{} bytes at GDS {:#x}", bytes, gds_off));
    }
    stats_.dma_bytes += bytes;
    const bool to_memory =
        d.kind == DmaKind::MemoryToMemory || d.kind == DmaKind::DataToMemory || d.kind == DmaKind::GdsToMemory;
    const auto staging = [&] {
        if (scratch_.capacity() < bytes) {
            ++stats_.buffer_growths;
        }
        if (scratch_.size() < bytes) {
            scratch_.resize(bytes);
        }
        return scratch_.data();
    };
    const auto fill = [&](u8* out) {
        const u32 pattern = b[1];
        for (u32 i = 0; i < bytes; ++i) {
            out[i] = static_cast<u8>(pattern >> ((i % 4) * 8));
        }
    };
    if (options_.mode != Mode::Isolated) {
        if (to_memory) {
            const u8* known = nullptr;
            if (d.kind == DmaKind::DataToMemory) { // the fill's bytes are known: the processor's copy takes them
                u8* tmp = staging();
                fill(tmp);
                known = tmp;
            }
            d.command_patch =
                PatchCommands(q, dst, known, bytes, d.kind == DmaKind::MemoryToMemory ? src : 0) > 0;
            if (q.patch_overflow && known == nullptr) {
                return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n,
                            fmt::format("more than {} writes into packets not yet processed", kMaxPatches));
            }
        }
        options_.work->Dma(d); // not executed: the command processor beside it copies
        return StepStatus::Packet;
    }
    u8* tmp = staging();
    if (d.kind == DmaKind::MemoryToMemory && !OverlapsPendingCommands(q, dst, bytes) &&
        options_.work->CopyOnTimeline(q.id, q.work_seq + 1, src, dst, bytes)) {
        // on the GPU's timeline: fences after it wait for it; the processor reads the destination only after it
        ++q.work_seq;
        ++stats_.timeline_copies;
        d.executed = true;
        d.timeline = true;
        options_.work->Dma(d);
        return StepStatus::Packet;
    }
    // (not while a fence that writes GDS data to memory is held: it reads the mapped GDS when its work completed, and
    // this GDS write may already have run on the GPU by then)
    if (gds && options_.gds_on_timeline && !(d.kind == DmaKind::GdsToMemory && OverlapsPendingCommands(q, dst, bytes)) &&
        !HeldGdsFence() &&
        options_.work->GdsOnTimeline(q.id, q.work_seq + 1, d.kind, d.kind == DmaKind::GdsToMemory ? dst : src,
                                     static_cast<u32>(gds_off), bytes, b[1])) {
        // on the GPU's timeline after the earlier work (the shaders' GDS accesses too): no wait for it (P4.6)
        ++q.work_seq;
        ++stats_.timeline_gds;
        d.executed = true;
        d.timeline = true;
        options_.work->Dma(d);
        return StepStatus::Packet;
    }
    if (d.kind == DmaKind::DataToMemory) {
        // a fill: known bytes, written on the GPU's timeline when the executor can (no wait for the earlier work)
        fill(tmp);
        if (options_.work->WriteOnTimeline(q.id, q.work_seq + 1, dst, tmp, bytes)) {
            ++q.work_seq;
            ++stats_.timeline_writes;
            d.command_patch = PatchCommands(q, dst, tmp, bytes, 0) > 0;
            d.executed = true;
            options_.work->Dma(d);
            return StepStatus::Packet;
        }
    }
    // Isolated: the CP's DMA engine copies now (stream order; the stream orders it against GPU work with fences). An
    // executor deferring GPU work completes the earlier work first.
    ++stats_.sync_dma[static_cast<size_t>(d.kind) & 7];
    if (d.kind == DmaKind::MemoryToMemory) {
        ++stats_.sync_dma_m2m_size[bytes <= 256 ? 0 : bytes <= 4096 ? 1 : bytes <= 65536 ? 2 : 3];
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        options_.work->Synchronize(q.id);
        stats_.sync_dma_ns[static_cast<size_t>(d.kind) & 7] += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    switch (d.kind) {
    case DmaKind::MemoryToMemory:
    case DmaKind::MemoryToGds:
        Native::R6::g_read_site = Native::R6::kReadDma;
        if (!options_.memory->Read(src, d.kind == DmaKind::MemoryToGds ? gds_ptr_ + gds_off : tmp, bytes)) {
            Native::R6::g_read_site = Native::R6::kReadOther;
            return Fail(q, CpErrorKind::MemoryUnreadable, addr, op, n, fmt::format("{} bytes at {:#x}", bytes, src));
        }
        Native::R6::g_read_site = Native::R6::kReadOther;
        break;
    case DmaKind::DataToMemory:
        fill(tmp);
        break;
    case DmaKind::DataToGds:
        fill(gds_ptr_ + gds_off);
        break;
    case DmaKind::GdsToMemory:
        std::memcpy(tmp, gds_ptr_ + gds_off, bytes);
        break;
    case DmaKind::ReadTagsPrefetch:
        break;
    }
    if (to_memory) {
        if (!options_.memory->Write(dst, tmp, bytes)) {
            return Fail(q, CpErrorKind::MemoryWriteFailed, addr, op, n, fmt::format("{} bytes at {:#x}", bytes, dst));
        }
        d.command_patch = PatchCommands(q, dst, tmp, bytes, d.kind == DmaKind::MemoryToMemory ? src : 0) > 0;
    }
    d.executed = true;
    options_.work->Dma(d);
    return StepStatus::Packet;
}

u64 CommandProcessor::ClockSample(u32 data_sel) {
    if (options_.clock != nullptr) {
        return options_.clock(options_.clock_ctx, data_sel);
    }
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool CommandProcessor::WriteFence(Queue& q, FenceOp& op) {
    u8 buf[8];
    const u8* src = buf;
    u64 bytes = 0;
    bool from_gds = false;
    if (op.kind == FenceKind::EndOfShader) {
        if (op.eos_command == 2) {
            const u32 v = static_cast<u32>(op.data);
            std::memcpy(buf, &v, 4);
            bytes = 4;
        } else {
            from_gds = true;
        }
    } else {
        switch (op.data_sel) {
        case 1: {
            const u32 v = static_cast<u32>(op.data);
            std::memcpy(buf, &v, 4);
            bytes = 4;
            break;
        }
        case 3:
        case 4:
            op.data = ClockSample(op.data_sel);
            if (op.clock_age_ns != ~0ull) {
                // the GPU's time at the fence's place: the sample minus how long ago it passed it (data_sel 4 counts
                // at the 800 MHz GPU core clock, 3 in ns)
                const u64 age = op.data_sel == 4 ? op.clock_age_ns * 4 / 5 : op.clock_age_ns;
                op.data = op.data > age ? op.data - age : 0;
            }
            [[fallthrough]];
        case 2:
            std::memcpy(buf, &op.data, 8);
            bytes = 8;
            break;
        case 5:
            from_gds = true;
            break;
        default:
            break;
        }
    }
    if (from_gds) {
        bytes = u64{op.gds_dwords} * 4;
        if (op.gds_offset > kGdsBytes || bytes > kGdsBytes - op.gds_offset) {
            Fail(q, CpErrorKind::GdsRange, op.packet_addr, 0, 0,
                 fmt::format("fence of {} bytes from GDS {:#x}", bytes, op.gds_offset));
            return false;
        }
        src = gds_ptr_ + op.gds_offset;
    }
    if (bytes != 0 && !options_.memory->Write(op.addr, src, bytes)) {
        Fail(q, CpErrorKind::MemoryWriteFailed, op.packet_addr, 0, 0,
             fmt::format("fence of {} bytes at {:#x}", bytes, op.addr));
        return false;
    }
    op.written = true;
    ++stats_.fences_written;
    LabelWait::Written(op.addr, bytes); // (R11 SSPC_R11_LABEL_WAIT: the game's main thread sleeps on this label)
    // (P5.4: the wake-up point of whoever polls this label - the game's frame and compute waits)
    StallTrace::Instant(StallTrace::Ev::FenceWrite, op.addr, static_cast<u32>(op.data), static_cast<u16>(q.id));
    if (fence_stats_.size() < 4096 || fence_stats_.contains(op.addr)) { // bounded (review LOW 10)
        FenceStat& fs = fence_stats_[op.addr];
        ++fs.count;
        fs.last = op.data;
        fs.queues |= 1u << std::min<u32>(q.id, 31);
        fs.kind = static_cast<u32>(op.kind);
        fs.data_sel = op.data_sel;
    }
    const bool irq = op.kind == FenceKind::EndOfPipe ? (op.int_sel == 1 || op.int_sel == 2)
                                                     : (op.kind == FenceKind::Release && op.int_sel >= 1);
    if (irq) {
        options_.work->Interrupt(q.id, op.kind);
    }
    return true;
}

StepStatus CommandProcessor::Fence(Queue& q, FenceOp& op) {
    ++stats_.fences;
    op.after_seq = q.work_seq > q.work_done ? q.work_seq : 0;
    if (options_.mode == Mode::Shadow) {
        options_.work->Fence(op); // not written: the command processor beside it writes
        return StepStatus::Packet;
    }
    if (op.after_seq != 0 || q.fence_count != 0) { // GPU work before it, or an earlier fence still held
        if (q.fence_count == kMaxFences) {
            return Fail(q, CpErrorKind::FenceQueueFull, op.packet_addr, 0, 0,
                        fmt::format("{} fences held waiting for the GPU", kMaxFences));
        }
        op.after_seq = q.work_seq;
        op.deferred = true;
        q.fences[(q.fence_head + q.fence_count++) % kMaxFences] = op;
        ++stats_.fences_deferred;
        StallTrace::Instant(StallTrace::Ev::FenceHeld, op.addr, static_cast<u32>(op.data), static_cast<u16>(q.id));
        options_.work->Fence(op);
        return StepStatus::Packet;
    }
    if (!WriteFence(q, op)) {
        return StepStatus::Faulted;
    }
    options_.work->Fence(op);
    return StepStatus::Packet;
}

void CommandProcessor::SetFenceClockAge(u32 queue, u64 packet_addr, u64 after_seq, u64 age_ns) {
    Queue* qp = Q(queue);
    if (qp == nullptr) {
        return;
    }
    for (u32 i = 0; i < qp->fence_count; ++i) {
        FenceOp& f = qp->fences[(qp->fence_head + i) % kMaxFences];
        if (f.packet_addr == packet_addr && f.after_seq == after_seq && !f.written) {
            f.clock_age_ns = age_ns;
            return;
        }
    }
}

void CommandProcessor::CompleteWork(u32 queue, u64 seq) {
    Queue* qp = Q(queue);
    if (qp == nullptr) {
        return;
    }
    Queue& q = *qp;
    q.work_done = std::max(q.work_done, std::min(seq, q.work_seq));
    while (q.fence_count > 0 && q.fences[q.fence_head].after_seq <= q.work_done) {
        FenceOp op = q.fences[q.fence_head];
        q.fence_head = (q.fence_head + 1) % kMaxFences;
        --q.fence_count;
        if (WriteFence(q, op)) {
            options_.work->Fence(op);
        }
    }
}

namespace {
bool WaitHolds(u32 function, u32 v, u32 ref) {
    switch (function) {
    case 0:
        return true;
    case 1:
        return v < ref;
    case 2:
        return v <= ref;
    case 3:
        return v == ref;
    case 4:
        return v != ref;
    case 5:
        return v >= ref;
    case 6:
        return v > ref;
    default:
        return false;
    }
}
} // namespace

/// R10 pacing (docs/R10_PACING.md): SSPC_R10_AQ_HELD_EXACT=1 - a WAIT_REG_MEM met by held fences is met by the earliest
/// of them from which on every held write there meets it, and the work after the wait is ordered behind the batch with
/// that fence's work (FrameBreak AQ: else behind the last batch with the fence queue's work - the whole graphics frame
/// the processor got through, when it is ahead of the GPU).
bool CommandProcessor::R10HeldExact() {
    static const bool on = [] {
        char v[4] = {};
        const bool o = GetEnvironmentVariableA("SSPC_R10_AQ_HELD_EXACT", v, sizeof(v)) == 1 && v[0] == '1';
        if (o) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R10_AQ_HELD_EXACT=1 - a wait met by held fences goes after the batch "
                                   "of the earliest fence meeting it, not after the fence queue's last batch");
        }
        return o;
    }();
    return on;
}

/// R10 diagnostics: SSPC_R10_DIAG_HELD=1 - the first waits met by held fences, each with every held fence there.
bool CommandProcessor::R10HeldDiag() {
    static const bool on = [] {
        char v[4] = {};
        const bool o = GetEnvironmentVariableA("SSPC_R10_DIAG_HELD", v, sizeof(v)) == 1 && v[0] == '1';
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_R10_DIAG_HELD=1 - waits met by held fences logged (the first 300)");
        }
        return o;
    }();
    return on;
}

void CommandProcessor::R10LogHeldWait(const Queue& q, u64 poll, u32 function, u32 mask, u32 ref, u32 by_queue,
                                      u64 chosen_seq) const {
    // (only the waits met by another queue's fence - the first 300 of them, and every 64th after those up to 300 more:
    // the measured window is a minute in)
    if (by_queue == q.id) {
        return;
    }
    static std::atomic<u32> seen{0};
    static std::atomic<u32> logged{0};
    const u32 k = seen.fetch_add(1, std::memory_order_relaxed);
    if (k >= 300 && (k % 64) != 0) {
        return;
    }
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 600) {
        return;
    }
    std::string held;
    const Queue* fq = Q(by_queue);
    u32 later = 0, met_index = ~0u;
    if (fq != nullptr) {
        for (u32 i = 0; i < fq->fence_count; ++i) {
            const FenceOp& f = fq->fences[(fq->fence_head + i) % kMaxFences];
            if (f.addr == poll || (f.data_sel == 2 && f.addr + 4 == poll)) {
                held += fmt::format(" [{} sel {} data {:#x} after {}{}]", f.kind == FenceKind::EndOfShader ? "eos" : "rel",
                                    f.data_sel, f.addr == poll ? static_cast<u32>(f.data) : static_cast<u32>(f.data >> 32),
                                    f.after_seq, f.after_seq == chosen_seq ? " *" : "");
                if (f.after_seq == chosen_seq) {
                    met_index = i;
                }
            }
        }
        for (u32 i = met_index == ~0u ? fq->fence_count : met_index + 1; i < fq->fence_count; ++i) {
            ++later; // (the queue's held fences after the meeting one, at any address)
        }
    }
    u32 now = 0;
    (void)options_.memory->Read(poll, &now, sizeof(now));
    SSPC_LOG(GPU, Info, "R10 held wait #{} tsc {}: queue {} waits [{:#x}] func {} mask {:#x} ref {:#x} (memory {:#x}); "
             "met by queue {} (its work {}, done {}; held fences {}, {} after the meeting one), its held fences there:{}",
             k, __rdtsc(), q.id, poll, function, mask, ref, now,
             by_queue, fq ? fq->work_seq : 0, fq ? fq->work_done : 0, fq ? fq->fence_count : 0, later, held);
}

bool CommandProcessor::HeldFenceSatisfies(u64 poll, u32 function, u32 mask, u32 ref, u32* by_queue, u64* after_seq,
                                          bool earliest) const {
    for (u32 id = 0; id < kNumQueues; ++id) {
        const Queue* qp = Q(id);
        if (qp == nullptr || qp->fence_count == 0) {
            continue;
        }
        // the queue's last held fence to that address is the value its memory keeps once they all completed
        // (R10 `earliest`: going back from it, the earliest of the held writes there that all meet the wait - from its
        // completion on, the memory meets it whatever completes after)
        const FenceOp* met = nullptr;
        for (u32 i = qp->fence_count; i-- > 0;) {
            const FenceOp& f = qp->fences[(qp->fence_head + i) % kMaxFences];
            u32 v = 0;
            if (f.kind == FenceKind::EndOfShader) {
                // EVENT_WRITE_EOS "signal fence": 32 bits of known data (P4.6); a GDS store there: not known
                if (f.addr != poll) {
                    continue;
                }
                if (f.eos_command != 2) {
                    break;
                }
                v = static_cast<u32>(f.data);
            } else if (f.data_sel != 1 && f.data_sel != 2) {
                if (f.data_sel != 0 && f.addr <= poll && poll < f.addr + 8) {
                    break; // a clock, a counter or GDS written there: not known
                }
                continue; // no data
            } else if (f.addr == poll) {
                v = static_cast<u32>(f.data);
            } else if (f.data_sel == 2 && f.addr + 4 == poll) {
                v = static_cast<u32>(f.data >> 32);
            } else {
                continue;
            }
            if (WaitHolds(function, v & mask, ref)) {
                met = &f;
                if (earliest) {
                    continue; // (an earlier write there may meet it too)
                }
            }
            break; // the queue's last write there does not meet it (`earliest`: the one before the run that does)
        }
        if (met != nullptr) {
            if (by_queue != nullptr) {
                *by_queue = id;
            }
            if (after_seq != nullptr) {
                *after_seq = met->after_seq;
            }
            return true;
        }
    }
    return false;
}

bool CommandProcessor::HeldGdsFence() const {
    for (u32 id = 0; id < kNumQueues; ++id) {
        const Queue* qp = Q(id);
        if (qp == nullptr) {
            continue;
        }
        for (u32 i = 0; i < qp->fence_count; ++i) {
            const FenceOp& f = qp->fences[(qp->fence_head + i) % kMaxFences];
            if ((f.kind == FenceKind::EndOfShader && f.eos_command != 2) ||
                (f.kind != FenceKind::EndOfShader && f.data_sel == 5)) {
                return true;
            }
        }
    }
    return false;
}

StepStatus CommandProcessor::WaitRegMem(Queue& q, u64 addr, std::span<const u32> p) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const u32 n = static_cast<u32>(p.size());
    const auto b = p.subspan(1);
    if (b.size() < 6) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "6 body dwords needed");
    }
    const u32 function = b[0] & 7;
    const bool memory = (b[0] >> 4) & 1;
    if (function == 7) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n, "function 7 (reserved)");
    }
    if (!q.blocked) {
        ++stats_.waits;
    }
    if (options_.mode == Mode::Shadow) {
        return StepStatus::Packet; // the command processor beside it waits
    }
    u32 value = 0;
    // R6-P (SSPC_R6_CP_POLL_TRIVIAL): a compare that holds for every value - the memory is not read
    const u8 r6_trivial = memory && Native::R6::Opt().poll_trivial != 0 && Native::R6::TrivialPoll(function, b[3], b[4])
                              ? Native::R6::Opt().poll_trivial
                              : 0;
    if (r6_trivial == 1) {
        Native::R6::Bump(Native::R6::Count().trivial_polls);
    } else if (memory) {
        const u64 poll = (u64{b[2]} << 32) | (b[1] & ~3u);
        Native::R6::g_read_site = q.blocked ? Native::R6::kReadWaitAgain : Native::R6::kReadWait;
        Native::R6::g_poll_function = function; // (R6-P diagnostics)
        Native::R6::g_poll_ref = b[3];
        Native::R6::g_poll_mask = b[4];
        Native::R6::g_read_over_timeline = false;
        const bool read = options_.memory->Read(poll, &value, sizeof(value));
        Native::R6::g_read_site = Native::R6::kReadOther;
        if (!read) {
            return Fail(q, CpErrorKind::MemoryUnreadable, addr, op, n, fmt::format("poll address {:#x}", poll));
        }
        if (r6_trivial == 2) {
            Native::R6::Counters& c = Native::R6::Count();
            Native::R6::Bump(c.trivial_polls);
            Native::R6::Bump(c.trivial_over_timeline, Native::R6::g_read_over_timeline ? 1 : 0);
        }
    } else {
        const u32 reg = b[1] & 0xFFFF;
        u32 index = 0;
        if (!FlatIndex(reg, index)) {
            return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n,
                        fmt::format("register {:#x} outside the register windows", reg));
        }
        value = regs_.At(index);
    }
    const u32 v = value & b[4], ref = b[3];
    bool ok = false;
    switch (function) {
    case 0:
        ok = true;
        break;
    case 1:
        ok = v < ref;
        break;
    case 2:
        ok = v <= ref;
        break;
    case 3:
        ok = v == ref;
        break;
    case 4:
        ok = v != ref;
        break;
    case 5:
        ok = v >= ref;
        break;
    case 6:
        ok = v > ref;
        break;
    default:
        break;
    }
    if (r6_trivial == 2 && !ok) {
        Native::R6::Bump(Native::R6::Count().trivial_not_met); // (R6-P verify: TrivialPoll was wrong)
    }
    const u64 key = memory ? ((u64{b[2]} << 32) | (b[1] & ~3u)) : (u64{1} << 63) | (b[1] & 0xFFFF);
    const auto now_ns = [] {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    u32 by_queue = ~0u;
    u64 held_seq = 0;
    if (!ok && memory && options_.waits_by_held_fences && options_.work != nullptr &&
        HeldFenceSatisfies(key, function, b[4], ref, &by_queue, &held_seq, R10HeldExact())) {
        // met in the GPU's order: the work before the fence is already handed on (P4.6); (FrameBreak AQ: the fence's
        // queue + 1 in the upper half - the executor orders the work after it behind that queue's batch; R10: behind
        // the batch with the meeting fence's own work)
        ok = true;
        ++stats_.waits_met_held;
        if (R10HeldDiag()) {
            R10LogHeldWait(q, key, function, b[4], ref, by_queue, held_seq);
        }
        options_.work->WaitMetByHeldFence(q.id | ((by_queue + 1) << 16), R10HeldExact() ? held_seq : 0);
    }
    if (!ok) {
        if (!q.blocked) {
            ++stats_.waits_blocked;
            q.block_t0 = now_ns();
            q.block_poll = key;
            q.block_held = memory && HeldFenceSatisfies(key, function, b[4], ref);
        }
        q.blocked = true;
        return StepStatus::Blocked;
    }
    if (q.blocked) {
        constexpr u64 kEdgesUs[] = {50, 100, 200, 500, 1000, 1500, 2000, 5000, 10000};
        const u64 us = (now_ns() - q.block_t0) / 1000;
        size_t bin = 0;
        while (bin < std::size(kEdgesUs) && us >= kEdgesUs[bin]) {
            ++bin;
        }
        ++stats_.block_hist[bin];
    }
    if (q.blocked && (wait_stats_.size() < 4096 || wait_stats_.contains(q.block_poll))) {
        WaitStat& w = wait_stats_[q.block_poll];
        ++w.count;
        w.ns += now_ns() - q.block_t0;
        w.held += q.block_held ? 1 : 0;
        w.queues |= 1u << std::min<u32>(q.id, 31);
        w.function = function;
        w.ref = ref;
        w.mask = b[4];
        w.last = value;
    }
    if (memory && options_.work != nullptr) {
        // the CPU writes before the awaited value (published data) apply to the work after the wait (P4.5)
        options_.work->MemoryAcquired(q.id, q.blocked, (u64{b[2]} << 32) | (b[1] & ~3u));
    }
    q.blocked = false;
    return StepStatus::Packet;
}

std::string CommandProcessor::FenceReport() const {
    std::vector<std::pair<u64, FenceStat>> v(fence_stats_.begin(), fence_stats_.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    std::string out = "fences written by address:\n";
    for (size_t i = 0; i < v.size() && i < 24; ++i) {
        const auto& [a, f] = v[i];
        out += fmt::format("  {:#x}: {} writes, last {:#x}, kind {}, data_sel {}, queues {:#x}\n", a, f.count, f.last,
                           f.kind, f.data_sel, f.queues);
    }
    return out;
}

std::string CommandProcessor::WaitReport() const {
    std::vector<std::pair<u64, WaitStat>> v(wait_stats_.begin(), wait_stats_.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::string out = "WAIT_REG_MEM blocks by address (time blocked; held: a held GPU fence would have met it):\n";
    for (size_t i = 0; i < v.size() && i < 24; ++i) {
        const auto& [k, w] = v[i];
        out += fmt::format("  {} {:#x}: {} blocks, {} ms, held {}, queues {:#x}, function {} ref {:#x} mask {:#x}, "
                           "last value {:#x}\n",
                           (k >> 63) ? "register" : "memory", k & ~(u64{1} << 63), w.count, w.ns / 1'000'000, w.held,
                           w.queues, w.function, w.ref, w.mask, w.last);
    }
    const auto& h = stats_.block_hist;
    out += fmt::format("WAIT_REG_MEM block durations: <50us {} | <100us {} | <200us {} | <500us {} | <1ms {} | <1.5ms {} | "
                       "<2ms {} | <5ms {} | <10ms {} | longer {}\n",
                       h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9]);
    return out;
}

StepStatus CommandProcessor::IndirectBuffer(Queue& q, u64 addr, std::span<const u32> p) {
    const u8 op = static_cast<u8>(p[0] >> 8);
    const u32 n = static_cast<u32>(p.size());
    const auto b = p.subspan(1);
    if (b.size() < 3) {
        return Fail(q, CpErrorKind::ShortPacket, addr, op, n, "3 body dwords needed");
    }
    if ((b[0] & 3) != 0) {
        return Fail(q, CpErrorKind::UnsupportedForm, addr, op, n, "unaligned base");
    }
    const u64 ib = b[0] | (u64{b[1] & 0xFFFF} << 32);
    const u32 size = b[2] & 0xFFFFF;
    const bool chain = (b[2] >> 20) & 1;
    ++stats_.indirect_buffers;
    if (size == 0) {
        if (chain) { // a chain ends the current buffer even when it chains to nothing
            q.frames[q.depth - 1].pos = q.frames[q.depth - 1].size;
        }
        return StepStatus::Packet;
    }
    const u32 slot = chain ? q.depth - 1 : q.depth;
    // frames: [0] the submission, [1] a carried packet (ring queues) or the first indirect buffer, ...
    const u32 limit = options_.max_ib_depth + (q.frames[0].ring ? 1u : 0u);
    if (slot > limit || slot >= kMaxFrames) {
        return Fail(q, CpErrorKind::IndirectTooDeep, addr, op, n, fmt::format("{} levels", slot));
    }
    if (q.budget + size > options_.max_dwords_per_submission) {
        return Fail(q, CpErrorKind::BudgetExceeded, addr, op, n, fmt::format("indirect buffer of {} dwords", size));
    }
    // Read into a buffer nobody points at: for a chain the current frame's buffer still holds this packet.
    std::vector<u32>& target = chain ? q.spare : q.buffers[slot];
    Native::R6::g_read_site = Native::R6::kReadIndirect;
    const bool fetched = Fetch(target, ib, size);
    Native::R6::g_read_site = Native::R6::kReadOther;
    if (!fetched) {
        return Fail(q, CpErrorKind::IndirectUnreadable, addr, op, n, fmt::format("{} dwords at {:#x}", size, ib));
    }
    if (chain) {
        std::swap(q.spare, q.buffers[slot]);
    }
    q.frames[slot] = Frame{.addr = ib, .data = q.buffers[slot].data(), .size = size, .owned = true};
    q.depth = slot + 1;
    return StepStatus::Packet;
}

bool CommandProcessor::Resync(u32 queue, u64 addr) {
    Queue* qp = Q(queue);
    if (qp == nullptr) {
        return false;
    }
    Queue& q = *qp;
    inline_marker_ = 0;
    const bool unstarted = q.depth == 1 && q.frames[0].pos == 0;
    if (unstarted && q.frames[0].addr == addr) {
        return true;
    }
    for (u32 i = 0; i < q.count; ++i) {
        if (q.pending[(q.head + i) % kMaxPending].addr == addr) {
            q.depth = 0;
            q.carry_need = q.carry_have = 0;
            q.head = (q.head + i) % kMaxPending;
            q.count -= i;
            stats_.submissions += i; // dropped unprocessed, counted as taken
            return true;
        }
    }
    if (!unstarted) {
        q.depth = 0;
        q.carry_need = q.carry_have = 0;
    }
    return false;
}

void CommandProcessor::Reset() {
    for (u32 i = 0; i < kNumQueues; ++i) {
        auto& slot = queues_[i];
        if (!slot) {
            continue;
        }
        stats_.fences_dropped += slot->fence_count;
        // a fresh queue (unmapped) keeping the buffers' capacity: a reset does not cost allocations later
        auto buffers = std::move(slot->buffers);
        auto spare = std::move(slot->spare);
        auto carry = std::move(slot->carry);
        const bool compute = slot->compute;
        slot = std::make_unique<Queue>();
        slot->id = i;
        slot->compute = compute;
        slot->buffers = std::move(buffers);
        slot->spare = std::move(spare);
        slot->carry = std::move(carry);
    }
    regs_.Reset();
    set_base_ = {};
    cb_extent_ = {};
    db_extent_ = 0;
    std::memset(gds_ptr_, 0, kGdsBytes);
    pipeline_key_ = 0;
    inline_marker_ = 0;
    inline_sharps_ = {};
    last_error_ = {};
}

} // namespace SecondSon::Gpu::Cp
