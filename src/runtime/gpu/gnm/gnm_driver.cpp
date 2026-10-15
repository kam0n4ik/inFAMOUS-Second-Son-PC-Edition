// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — SecondSonPC's own libSceGnmDriver (P4.4, EXPERIMENTAL). See gnm_driver.h. Ported from shadPS4's
// gnmdriver.cpp (GPL-2.0-or-later): every function is the bridge's function of the same name, with the same checks in
// the same order, the same results and the same dwords; the packet layouts are re-derived from the bridge's
// video_core/amdgpu/pm4_cmds.h (its PM4Type3Header, WriteHeader/WriteBody/WritePacket, WriteTrailingNop).
//
// Where the bridge asserts, stops at UNREACHABLE or would crash on the game's input, this driver logs an error and
// returns an error code instead:
//  * sceGnmSubmitAndFlip*: count 0 (the bridge reads dcbGpuAddrs[-1]) -> 0x80D11000; the flip marker missing (DCB
//    shorter than 64 dwords, or its last 64 dwords not the prepareFlip NOP) or a label marker with an unaligned label
//    -> 0x80D11080 (SUBMISSION_AND_FLIP_FAILED_INVALID_COMMAND_BUFFER); buffer index 0xFFFFFFFF -> 0x80D11000; no label
//    address for the VideoOut handle -> 0x80D11082 (asked before the flip is requested, so a refused label leaves no
//    flip behind);
//  * a submitted DCB without an address -> 0x80D11000; a constant engine buffer (CCB) with dwords -> 0x80D110FF: the
//    command processor has no constant engine (the game submits none; the bridge would run it);
//  * sceGnmAddEqEvent: an interrupt id above 0x40 or a second registration of the same queue and id -> 0x80D11000
//    (nothing registered); sceGnmDeleteEqEvent with an id above 0x40 -> 0x80D11000 (the event is removed first, as in
//    the bridge);
//  * sceGnmMapComputeQueue: a null read pointer (the bridge writes 0 through it) -> 0x80D17004; a 65th queue ->
//    0x80D170FF; sceGnmUnmapComputeQueue of a queue that is not mapped -> 0x80D170FF (the bridge returns 0 and corrupts
//    its slot list);
//  * sceGnmDrawIndexMultiInstanced, sceGnmDrawIndirectCountMulti (UNREACHABLE) -> 0x80D11000;
//  * a null command buffer where the bridge writes without a check (DispatchInitDefaultHardwareState,
//    DrawInitDefaultHardwareState350, DrawInitToDefaultContextState400, InsertWaitFlipDone, SetEmbedded*Shader) ->
//    the function's failure result (0 or -1); sceGnmGetEqEventType with a null event -> 0x80D11000.
// The bridge's stubs keep their results and warn once that they do nothing; "not available in retail firmware"
// functions keep the retail answer the bridge gives (logged once at Debug; at Warning when that answer is 0 = OK).
// Without a host (SecondSonPC's backend not running) every function that needs one logs an error (the first 16) and
// refuses: 0x80D11000, or 0x80D170FF for the compute queue functions. During the orderly exit (the backend stopped by
// its exit step while the game's threads still run) the refusal is the same, logged once at Info.
//
// Threads: g_submit_mutex orders submissions, DingDong and SubmitDone (the bridge's m_submit_lock); the submission lock
// is a flag under g_idle_mutex with a condition variable (the bridge's submission_lock / m_wait_idle / cv_lock). The
// host is read and used under g_submit_mutex, g_queue_mutex or g_label_mutex, so SetHost(nullptr) — after it released
// the submission lock — waits for every call still using the previous host. A backend calls SetHost(nullptr) before it
// stops its GPU thread and not from that thread (a submission may be waiting for the GPU thread inside the host).
#include <windows.h>

#include <intrin.h>

#include "runtime/gpu/gnm/gnm_driver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/frame_pacing.h"
#include "runtime/gpu/gnm/gnm_init_sequences.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/kernel/clock.h"
#include "runtime/kernel/event_queues.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Gpu::Gnm {

namespace {

namespace Op = Pm4::Op;
namespace Queues = Kernel::EventQueues;

// ------------------------------------------------------------------------------ results (the bridge's gnm_error.h)

constexpr s32 kOk = 0;
constexpr s32 kFail = -1; ///< ORBIS_FAIL: the command-building functions' answer to bad arguments
constexpr s32 kErrSubmitInvalidArgument = static_cast<s32>(0x80D11000);
constexpr s32 kErrFlipInvalidCommandBuffer = static_cast<s32>(0x80D11080);
constexpr s32 kErrFlipQueueFull = static_cast<s32>(0x80D11081);
constexpr s32 kErrFlipRequestFailed = static_cast<s32>(0x80D11082);
constexpr s32 kErrSubmitInternal = static_cast<s32>(0x80D110FF);
constexpr s32 kErrCaptureRazorNotLoaded = static_cast<s32>(0x80D15001);
constexpr s32 kErrCaptureFailedInternal = static_cast<s32>(0x80D1500F);
constexpr s32 kErrQueueInvalidPipeId = static_cast<s32>(0x80D17000);
constexpr s32 kErrQueueInvalidQueueId = static_cast<s32>(0x80D17001);
constexpr s32 kErrQueueInvalidRingSize = static_cast<s32>(0x80D17002);
constexpr s32 kErrQueueInvalidRingBase = static_cast<s32>(0x80D17003);
constexpr s32 kErrQueueInvalidReadPtr = static_cast<s32>(0x80D17004);
constexpr s32 kErrQueueInternal = static_cast<s32>(0x80D170FF);
constexpr s32 kErrFailure = static_cast<s32>(0x8EEE00FF);         ///< ORBIS_GNM_ERROR_FAILURE
constexpr s32 kErrKernelEbadf = static_cast<s32>(0x80020009);     ///< ORBIS_KERNEL_ERROR_EBADF
constexpr s32 kErrVideoOutQueueFull = static_cast<s32>(0x80290012); ///< VideoOut: the flip queue is full

// ------------------------------------------------------------------------------ limits and constants

constexpr u32 kNumComputePipes = 7;       ///< Liverpool has 8; the 8th is the system software's
constexpr u32 kNumQueuesPerPipe = 8;
constexpr u32 kMaxComputeQueues = 64;     ///< Gnm's vqid 1..64 (the bridge's slot vector of 64)
constexpr u32 kMaxSubmitBytes = 0x3FFFFC; ///< a DCB or CCB: at most 2^20 - 1 dwords
constexpr u32 kMaxInterruptId = 0x40;     ///< the bridge's interrupt table ends at GfxEop
constexpr u32 kSdk200 = 0x2000000;        ///< the game's SDK version 2.00 (sceKernelGetCompiledSdkVersion)
constexpr u32 kSdk400 = 0x4000000;
constexpr u32 kTessFactorRingBelowReservedEnd = 0x10000000;
constexpr u32 kOffChipTessellationBufferSize = 0x800000;

/// The flip marker: the last 64 dwords of the flip's DCB are a NOP the game's Gnm library leaves for the driver.
constexpr u32 kFlipPacketDwords = 64;
constexpr u32 kPrepareFlipHeader = 0xC03E1000u; ///< NOP with 0x3F payload dwords

/// The first payload dword of the driver's NOPs (the bridge's PM4CmdNop::PayloadType).
constexpr u32 kDebugMarkerPush = 0x68750001u;
constexpr u32 kDebugMarkerPop = 0x68750002u;
constexpr u32 kDebugSetMarker = 0x68750003u;
constexpr u32 kDebugColorMarkerPush = 0x6875000Eu;
constexpr u32 kPatchedFlip = 0x68750776u;
constexpr u32 kPrepareFlip = 0x68750777u;
constexpr u32 kPrepareFlipLabel = 0x68750778u;
constexpr u32 kPrepareFlipInterrupt = 0x68750780u;
constexpr u32 kPrepareFlipInterruptLabel = 0x68750781u;

/// Shader stages as the indirect draws name them (Cs, Ps, Vs, Gs, Es, Hs, Ls) and the user data register each one's
/// SGPR offsets are relative to (the bridge's indirect_sgpr_offsets).
constexpr u32 kStageVs = 2;
constexpr u32 kStageEs = 4;
constexpr u32 kStageLs = 6;
constexpr u32 kNumStages = 7;
constexpr std::array<u32, kNumStages> kIndirectSgprOffsets{0u, 0u, 0x4Cu, 0u, 0xCCu, 0u, 0x14Cu};
constexpr u32 kDrawIndirectArgsSize = 0x10;        ///< 4 dwords: vertices, instances, first vertex, first instance
constexpr u32 kDrawIndexedIndirectArgsSize = 0x14; ///< 5 dwords: indices, instances, first index, base vertex, first instance

// ------------------------------------------------------------------------------ PM4 (the bridge's pm4_cmds.h helpers)

/// A type-3 header: `count` = body dwords - 1 (14 bits).
constexpr u32 Type3(u8 opcode, u32 count, bool compute = false, bool predicate = false) {
    return (3u << 30) | ((count & 0x3FFFu) << 16) | (u32{opcode} << 8) | (u32{compute} << 1) | u32{predicate};
}

/// WriteHeader: the header of a packet with `body` dwords after it; returns the body.
u32* Header(u32* p, u8 opcode, u32 body, bool compute = false, bool predicate = false) {
    *p = Type3(opcode, body - 1, compute, predicate);
    return p + 1;
}

/// WritePacket: header and body; returns the next packet.
u32* Packet(u32* p, u8 opcode, std::initializer_list<u32> body, bool compute = false) {
    p = Header(p, opcode, static_cast<u32>(body.size()), compute);
    std::memcpy(p, body.begin(), body.size() * sizeof(u32));
    return p + body.size();
}

u32* SetSh(u32* p, std::initializer_list<u32> body) {
    return Packet(p, Op::SetShReg, body);
}

u32* SetShCompute(u32* p, std::initializer_list<u32> body) {
    return Packet(p, Op::SetShReg, body, true);
}

u32* SetContext(u32* p, std::initializer_list<u32> body) {
    return Packet(p, Op::SetContextReg, body);
}

/// A NOP whose payload is a register update in the PS4 driver's patchable form (0xC01E << 16 | register, values):
/// sceGnmUpdate*Shader leaves context registers to the patch step.
u32* Nop(u32* p, std::initializer_list<u32> body) {
    return Packet(p, Op::Nop, body);
}

/// WriteTrailingNop: a NOP over the next `payload` dwords, of which only the first is written; returns past them.
u32* TrailingNop(u32* p, u32 payload) {
    p[0] = Type3(Op::Nop, payload - 1);
    p[1] = 0;
    return p + payload + 1;
}

void Store64(u32* p, u64 value) {
    std::memcpy(p, &value, sizeof(value));
}

u32 SgprOffset(u32 offset, u32 stage) {
    return offset == 0 ? 0 : (offset & 0xFFFFu) + kIndirectSgprOffsets[stage];
}

bool IsVertexStage(u32 stage) {
    return stage == kStageVs || stage == kStageEs || stage == kStageLs;
}

// ------------------------------------------------------------------------------ state

std::atomic<Host*> g_host{nullptr};
std::atomic<s32> g_sdk_version{0};
std::atomic<u32> g_errors_without_host{0};
std::atomic<bool> g_refused_while_exiting{false};
std::atomic<u64> g_tess_factor_ring{0};

std::mutex g_submit_mutex;
bool g_send_init_packet = true; ///< under g_submit_mutex

std::mutex g_idle_mutex;
std::condition_variable g_idle_cv;
bool g_submission_locked = false; ///< under g_idle_mutex

// P5.4 early frame submission (SetEarlyFrameSource)
constexpr u32 kMaxEarlyDcbs = 512;            ///< SubmitFrame's list (its stack arrays hold 512)
std::atomic<EarlyFrameSource> g_early_source{nullptr};
std::atomic<bool> g_early_installed{false};   ///< a source was ever installed (the flip call looks for its frame)
/// The frame sceGnmSubmitDone handed on, until the game's sceGnmSubmitAndFlipCommandBuffers of it (under g_submit_mutex).
struct EarlySubmitted {
    bool pending = false;
    s32 result = 0;
    u32 count = 0;
    u32 vo_handle = 0, buffer_index = 0, flip_mode = 0;
    s64 flip_arg = 0;
    std::array<const u32*, kMaxEarlyDcbs> dcbs{};
    std::array<u32, kMaxEarlyDcbs> sizes{};
};
EarlySubmitted g_early; ///< under g_submit_mutex

/// Compute queue slots as the bridge's slot vector hands them out: vqid = slot + 1, the most recently freed first.
std::vector<u32> InitialFreeSlots() {
    std::vector<u32> slots(kMaxComputeQueues);
    for (u32 i = 0; i < kMaxComputeQueues; ++i) {
        slots[i] = kMaxComputeQueues - 1 - i; // back() = slot 0
    }
    return slots;
}

std::mutex g_queue_mutex;
std::vector<u32> g_free_slots = InitialFreeSlots(); ///< under g_queue_mutex
std::bitset<kMaxComputeQueues> g_mapped;            ///< under g_queue_mutex

std::mutex g_label_mutex; ///< sceGnmInsertWaitFlipDone's label lookup (outside the submission order)

/// sceGnmAddEqEvent: a queue (by identity) waits for an interrupt id.
struct Registration {
    Queues::QueueId queue;
    u32 id;
};
std::mutex g_irq_mutex;
std::vector<Registration> g_registrations; ///< under g_irq_mutex

// ------------------------------------------------------------------------------ helpers

/// The installed host, or nullptr after an error line (the first 16 of the process).
Host* CurrentHost(const char* function) {
    Host* const host = g_host.load(std::memory_order_acquire);
    if (host == nullptr) [[unlikely]] {
        // The orderly exit stops the backend (exit step "native GPU backend") while the game's threads still run: their
        // GPU calls after it are expected and refused with one line, not errors (P4.12: sceGnmDingDong of async
        // compute after the window was closed)
        if (Lifecycle::Current() != Lifecycle::State::Running) {
            if (!g_refused_while_exiting.exchange(true, std::memory_order_relaxed)) {
                SSPC_LOG(GPU, Info, "{}: refused - the process is exiting and the GPU backend has stopped (further "
                         "refusals during the exit are not logged)", function);
            }
            return host;
        }
        const u32 n = g_errors_without_host.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            SSPC_LOG(GPU, Error, "{}: SecondSonPC's GPU backend is not running (no Gnm host): refused{}", function,
                     n == 15 ? " (further refusals are not logged)" : "");
        }
    }
    return host;
}

/// The bridge's stub: the same result, nothing done; one warning per function.
s32 NotImplemented(std::atomic<bool>& warned, const char* function, s32 result) {
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        SSPC_LOG(GPU, Warning, "{}: not implemented (a stub in the bridge too): does nothing, returns {:#x}", function,
                 static_cast<u32>(result));
    }
    return result;
}

/// Not in the retail firmware (the bridge's note): the retail driver's answer, logged once.
template <typename R>
R RetailAnswer(std::atomic<bool>& logged, const char* function, R result) {
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        SSPC_LOG(GPU, Debug, "{}: not available in retail firmware: returns {:#x}", function, static_cast<u32>(result));
    }
    return result;
}

/// The bridge stops at UNREACHABLE: an error result here.
s32 Unsupported(std::atomic<bool>& logged, const char* function) {
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        SSPC_LOG(GPU, Error, "{}: not implemented (the bridge stops at UNREACHABLE): returns {:#x}", function,
                 static_cast<u32>(kErrSubmitInvalidArgument));
    }
    return kErrSubmitInvalidArgument;
}

/// A refused argument of a command-building function (the bridge's LOG_ERROR lines): -1.
template <typename... Args>
s32 Refuse(const char* function, fmt::format_string<Args...> what, Args&&... args) {
    SSPC_LOG(GPU, Error, "{}: {}", function, fmt::format(what, std::forward<Args>(args)...));
    return kFail;
}

void NullCommandBuffer(const char* function) {
    SSPC_LOG(GPU, Error, "{}: null command buffer (the bridge would write through it): refused", function);
}

void ReleaseSubmissionLock() {
    {
        std::scoped_lock lk{g_idle_mutex};
        g_submission_locked = false;
    }
    g_idle_cv.notify_all();
}

/// The bridge's WaitGpuIdle: until no SubmitDone holds the submission lock.
void WaitGpuIdle() {
    std::unique_lock lk{g_idle_mutex};
    if (!g_submission_locked) {
        return;
    }
    const u64 t0 = __rdtsc();
    g_idle_cv.wait(lk, [] { return !g_submission_locked; });
    const u64 t1 = __rdtsc();
    StallTrace::Span(StallTrace::Ev::GameIdleWait, t0, t1); // (P4.12)
    FrameLog::GameIdleWait(static_cast<u64>(static_cast<double>(t1 - t0) * 1e9 /
                                            static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1))));
}

/// The initialisation sequence for the game's SDK version (the bridge's choice, base PS4).
std::span<const u32> InitSequenceFor(s32 sdk_version) {
    const u32 sdk = static_cast<u32>(sdk_version);
    if (sdk < kSdk200) {
        return kInitSequence;
    }
    if (sdk < kSdk400) {
        return kInitSequence200;
    }
    return kInitSequence350;
}

/// The bridge's checks of sceGnmSubmit*: every DCB 1..2^20-1 dwords, every CCB at most that.
s32 CheckSizes(const char* function, u32 count, const u32* dcb_sizes, const u32* ccb_sizes) {
    for (u32 i = 0; i < count; ++i) {
        if (dcb_sizes[i] == 0) {
            SSPC_LOG(GPU, Error, "{}: submitting a null DCB {}", function, i);
            return kErrSubmitInvalidArgument;
        }
        if (dcb_sizes[i] > kMaxSubmitBytes) {
            SSPC_LOG(GPU, Error, "{}: dcbSizesInBytes[{}] ({}) is limited to (2^20)-1 dwords", function, i,
                     dcb_sizes[i]);
            return kErrSubmitInvalidArgument;
        }
        if (ccb_sizes != nullptr && ccb_sizes[i] > kMaxSubmitBytes) {
            SSPC_LOG(GPU, Error, "{}: ccbSizesInBytes[{}] ({}) is limited to (2^20)-1 dwords", function, i,
                     ccb_sizes[i]);
            return kErrSubmitInvalidArgument;
        }
    }
    return kOk;
}

/// What the bridge passes on unchecked and SecondSonPC's backend cannot take: a DCB without an address, a CCB with
/// dwords (the command processor has no constant engine; the game submits none).
s32 CheckSupported(const char* function, u32 count, const u32* const* dcbs, const u32* ccb_sizes) {
    for (u32 i = 0; i < count; ++i) {
        if (dcbs[i] == nullptr) {
            SSPC_LOG(GPU, Error, "{}: DCB {} has no address", function, i);
            return kErrSubmitInvalidArgument;
        }
        if (ccb_sizes != nullptr && (ccb_sizes[i] >> 2) != 0) {
            SSPC_LOG(GPU, Error, "{}: CCB {} ({} bytes): constant engine buffers are not supported", function, i,
                     ccb_sizes[i]);
            return kErrSubmitInternal;
        }
    }
    return kOk;
}

/// The bridge's PerformSubmit: the initialisation sequence once after each SubmitDone, then the DCBs in order. Under
/// g_submit_mutex, past WaitGpuIdle.
s32 PerformSubmit(Host& host, u32 count, const u32* const* dcbs, const u32* dcb_sizes) {
    if (g_send_init_packet) {
        const std::span<const u32> sequence = InitSequenceFor(g_sdk_version.load(std::memory_order_relaxed));
        if (!host.SubmitGraphics(reinterpret_cast<u64>(sequence.data()), static_cast<u32>(sequence.size()),
                                 sequence.data())) {
            SSPC_LOG(GPU, Error, "GnmDriver: the backend refused the initialisation sequence: submission dropped");
            return kErrSubmitInternal; // the sequence stays pending
        }
        g_send_init_packet = false;
    }
    for (u32 i = 0; i < count; ++i) {
        if (!host.SubmitGraphics(reinterpret_cast<u64>(dcbs[i]), dcb_sizes[i] >> 2, nullptr)) {
            SSPC_LOG(GPU, Error, "GnmDriver: the backend refused DCB {} of {} ({:#x}, {} bytes)", i, count,
                     reinterpret_cast<u64>(dcbs[i]), dcb_sizes[i]);
            return kErrSubmitInternal;
        }
    }
    return kOk;
}

/// The bridge's PatchFlipRequest: the prepareFlip NOP in the last 64 dwords of the flip's DCB becomes a WRITE_DATA of
/// the buffer's VideoOut label (1: the surface is busy until the flip) and the PatchedFlip marker NOP, plus the label
/// write or end-of-pipe event the game asked for; the flip is requested from VideoOut first.
s32 PatchFlipRequest(Host& host, u32* dcb, u32 dwords, u32 vo_handle, u32 buffer_index, u32 flip_mode, s64 flip_arg) {
    constexpr const char* kName = "sceGnmSubmitAndFlipCommandBuffers";
    if (dwords < kFlipPacketDwords || dcb[dwords - kFlipPacketDwords] != kPrepareFlipHeader) {
        SSPC_LOG(GPU, Error, "{}: can't find the prepareFlip packet in the last 64 of {} dwords", kName, dwords);
        return kErrFlipInvalidCommandBuffer;
    }
    u32* const marker = dcb + (dwords - kFlipPacketDwords);
    std::array<u32, 7> backup;
    std::memcpy(backup.data(), marker, sizeof(backup));
    if ((backup[2] & 3) != 0 && backup[1] == kPrepareFlipLabel) {
        SSPC_LOG(GPU, Error, "{}: invalid flip packet (label address {:#x} not 4-byte aligned)", kName, backup[2]);
        return kErrFlipInvalidCommandBuffer;
    }
    if (buffer_index == 0xFFFFFFFFu) {
        SSPC_LOG(GPU, Error, "{}: invalid VideoOut buffer index", kName);
        return kErrSubmitInvalidArgument;
    }
    u64 labels = 0;
    if (!host.BufferLabelAddress(static_cast<s32>(vo_handle), labels)) {
        SSPC_LOG(GPU, Error, "{}: no buffer labels for VideoOut handle {}", kName, static_cast<s32>(vo_handle));
        return kErrFlipRequestFailed;
    }
    const s32 flip = host.SubmitEopFlip(static_cast<s32>(vo_handle), buffer_index, flip_mode, flip_arg);
    if (flip != 0) {
        if (flip == kErrVideoOutQueueFull) {
            SSPC_LOG(GPU, Error, "{}: flip queue is full", kName);
            return kErrFlipQueueFull;
        }
        SSPC_LOG(GPU, Error, "{}: flip request failed ({:#x})", kName, static_cast<u32>(flip));
        return flip;
    }

    const u64 label = (labels + u64{buffer_index} * sizeof(u64)) & ~3ull;
    marker[0] = Type3(Op::WriteData, 3);
    marker[1] = 0x500u; // destination: memory, write confirm
    Store64(marker + 2, label);
    marker[4] = 1;
    switch (backup[1]) {
    case kPrepareFlip:
        marker[5] = Type3(Op::Nop, 0x39);
        marker[6] = kPatchedFlip;
        break;
    case kPrepareFlipLabel:
        marker[5] = Type3(Op::Nop, 0x34);
        marker[6] = kPatchedFlip;
        marker[0x3B] = Type3(Op::WriteData, 3); // the game's label write
        marker[0x3C] = 0x500u;
        marker[0x3D] = backup[2] & 0xFFFFFFFCu;
        marker[0x3E] = backup[3];
        marker[0x3F] = backup[4];
        break;
    case kPrepareFlipInterruptLabel:
    case kPrepareFlipInterrupt: {
        const bool label_write = backup[1] == kPrepareFlipInterruptLabel;
        marker[5] = Type3(Op::Nop, 0x33);
        marker[6] = kPatchedFlip;
        marker[0x3A] = Type3(Op::EventWriteEop, 4); // the game's end-of-pipe event (with its interrupt)
        marker[0x3B] = (backup[5] & 0x3F) + 0x500u + (backup[6] & 0x3F) * 0x1000u;
        marker[0x3C] = label_write ? backup[2] & 0xFFFFFFFCu : 0u;
        marker[0x3D] = label_write ? (backup[3] & 0xFFFFu) | 0x22000000u : 0x1000000u;
        marker[0x3E] = label_write ? backup[4] : 0u;
        marker[0x3F] = 0;
        break;
    }
    default: {
        static std::atomic<bool> logged;
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            SSPC_LOG(GPU, Error, "{}: unknown flip marker type {:#x}: only the buffer label write is patched in (as "
                     "the bridge does)", kName, backup[1]);
        }
        break;
    }
    }
    return kOk;
}

/// The guest's kernel event (struct kevent) as sceGnmGetEqEventType reads it.
struct GuestKernelEvent {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    u64 data;
    void* udata;
};
static_assert(offsetof(GuestKernelEvent, data) == 16 && sizeof(GuestKernelEvent) == 32);

/// The interrupts a queue registered with sceGnmAddEqEvent is woken by (the bridge's IsValidEventType).
bool IsEventType(u32 id) {
    return id <= static_cast<u32>(Interrupt::Compute6RelMem) || id == static_cast<u32>(Interrupt::GfxEop);
}

// ------------------------------------------------------------------------------ the guest functions

// --- event queues

SSPC_SYSV_ABI s32 AddEqEvent(s64 eq, u64 id, void* udata) {
    Queues::Owner* const owner = Queues::GetOwner();
    if (owner == nullptr) {
        SSPC_LOG(GPU, Error, "sceGnmAddEqEvent: no event queue owner");
        return kErrKernelEbadf;
    }
    const auto queue = owner->Find(eq);
    if (!queue) {
        return kErrKernelEbadf;
    }
    if (id > kMaxInterruptId) {
        SSPC_LOG(GPU, Error, "sceGnmAddEqEvent: interrupt id {:#x} out of range (the bridge asserts)", id);
        return kErrSubmitInvalidArgument;
    }
    std::scoped_lock lk{g_irq_mutex};
    const bool again = std::ranges::any_of(g_registrations, [&](const Registration& r) {
        return r.queue.serial == queue->serial && r.id == static_cast<u32>(id);
    });
    if (again) {
        SSPC_LOG(GPU, Error, "sceGnmAddEqEvent: queue {} already waits for interrupt {:#x} (the bridge asserts)", eq,
                 id);
        return kErrSubmitInvalidArgument;
    }
    if (!owner->AddEvent(*queue, id, Queues::Filter::GraphicsCore, udata)) {
        return kErrKernelEbadf; // deleted meanwhile
    }
    g_registrations.push_back({*queue, static_cast<u32>(id)});
    return kOk;
}

SSPC_SYSV_ABI s32 DeleteEqEvent(s64 eq, u64 id) {
    Queues::Owner* const owner = Queues::GetOwner();
    if (owner == nullptr) {
        SSPC_LOG(GPU, Error, "sceGnmDeleteEqEvent: no event queue owner");
        return kErrKernelEbadf;
    }
    const auto queue = owner->Find(eq);
    if (!queue) {
        return kErrKernelEbadf;
    }
    std::scoped_lock lk{g_irq_mutex};
    owner->RemoveEvent(*queue, id, Queues::Filter::GraphicsCore); // whether it was there or not, as the bridge
    if (id > kMaxInterruptId) {
        SSPC_LOG(GPU, Error, "sceGnmDeleteEqEvent: interrupt id {:#x} out of range (the bridge asserts)", id);
        return kErrSubmitInvalidArgument;
    }
    std::erase_if(g_registrations, [&](const Registration& r) {
        return r.queue.serial == queue->serial && r.id == static_cast<u32>(id);
    });
    return kOk;
}

SSPC_SYSV_ABI s32 GetEqEventType(const GuestKernelEvent* ev) {
    if (ev == nullptr) {
        SSPC_LOG(GPU, Error, "sceGnmGetEqEventType: null event");
        return kErrSubmitInvalidArgument;
    }
    return static_cast<s32>(ev->data); // the interrupt id the trigger stored (sceKernelGetEventData)
}

// --- submission

SSPC_SYSV_ABI s32 AreSubmitsAllowed() {
    std::scoped_lock lk{g_idle_mutex};
    return g_submission_locked ? 0 : 1;
}

SSPC_SYSV_ABI s32 SubmitCommandBuffersForWorkload(u32 /*workload*/, u32 count, const u32* dcbs[], u32* dcb_sizes,
                                                  const u32* /*ccbs*/[], u32* ccb_sizes) {
    constexpr const char* kName = "sceGnmSubmitCommandBuffers";
    if (dcbs == nullptr || dcb_sizes == nullptr) {
        SSPC_LOG(GPU, Error, "{}: dcbGpuAddrs and dcbSizesInBytes must not be NULL", kName);
        return kErrSubmitInvalidArgument;
    }
    if (const s32 r = CheckSizes(kName, count, dcb_sizes, ccb_sizes); r != kOk) {
        return r;
    }
    if (const s32 r = CheckSupported(kName, count, dcbs, ccb_sizes); r != kOk) {
        return r;
    }
    std::scoped_lock lk{g_submit_mutex};
    WaitGpuIdle();
    Host* const host = CurrentHost(kName);
    if (host == nullptr) {
        return kErrSubmitInvalidArgument;
    }
    return PerformSubmit(*host, count, dcbs, dcb_sizes);
}

SSPC_SYSV_ABI s32 SubmitCommandBuffers(u32 count, const u32* dcbs[], u32* dcb_sizes, const u32* ccbs[],
                                       u32* ccb_sizes) {
    return SubmitCommandBuffersForWorkload(count, count, dcbs, dcb_sizes, ccbs, ccb_sizes);
}

std::atomic<u64> g_last_submit_tsc{0};

} // namespace

u64 LastSubmitTsc() {
    return g_last_submit_tsc.load(std::memory_order_acquire);
}

namespace {

/// P5.4 (SetEarlyFrameSource): whether the caller of sceGnmSubmitDone is the engine's SubmitFrame with a frame to flip
/// that passes the checks of sceGnmSubmitAndFlipCommandBuffers. Reads only (the caller's stack frame): no side effect.
bool AcceptEarlyFrame(EarlyFrameSource source, const u64* return_slot, EarlyFrame& f) {
    constexpr const char* kName = "sceGnmSubmitDone (early frame)";
    if (!source(return_slot, f)) {
        return false;
    }
    if (f.count == 0 || f.count > kMaxEarlyDcbs || f.dcbs == nullptr || f.dcb_sizes == nullptr) {
        SSPC_LOG(GPU, Error, "{}: {} DCBs (1..{} expected): left to the game's own call", kName, f.count,
                 kMaxEarlyDcbs);
        return false;
    }
    return CheckSizes(kName, f.count, f.dcb_sizes, nullptr) == kOk &&
           CheckSupported(kName, f.count, f.dcbs, nullptr) == kOk;
}

/// P5.4: the accepted frame handed on as sceGnmSubmitAndFlipCommandBuffers hands it on (under g_submit_mutex, after
/// host.SubmitDone()) and kept for the game's own call. A refused flip request writes nothing into the DCB: then nothing
/// is kept and the game's own call asks again.
void SubmitEarlyFrame(Host& host, const EarlyFrame& f) {
    if (g_early.pending) {
        SSPC_LOG(GPU, Error, "sceGnmSubmitDone (early frame): the frame handed on before ({} DCBs, last {:#x}) got no "
                 "sceGnmSubmitAndFlipCommandBuffers", g_early.count,
                 reinterpret_cast<u64>(g_early.dcbs[g_early.count - 1]));
        g_early.pending = false;
    }
    const StallTrace::Scope trace(StallTrace::Ev::GameEarlySubmit, f.count, f.buffer_index);
    FrameLog::GameFlipEnter();
    if (PatchFlipRequest(host, f.dcbs[f.count - 1], f.dcb_sizes[f.count - 1] / 4, f.vo_handle, f.buffer_index,
                         f.flip_mode, f.flip_arg) != kOk) {
        return;
    }
    if (FramePacing::Enabled()) [[unlikely]] { // (the pair the game's own call marks, past the flip request)
        FramePacing::Mark(FramePacing::Event::SubmitEnter);
        FramePacing::Mark(FramePacing::Event::SubmitUnblocked);
    }
    const s32 submitted = PerformSubmit(host, f.count, f.dcbs, f.dcb_sizes);
    FrameLog::GameFlip();
    Inspector::FlipThread();
    g_last_submit_tsc.store(__rdtsc(), std::memory_order_release);
    if (FramePacing::Enabled()) [[unlikely]] {
        FramePacing::Mark(FramePacing::Event::Submitted);
    }
    g_early.pending = true;
    g_early.result = submitted;
    g_early.count = f.count;
    g_early.vo_handle = f.vo_handle;
    g_early.buffer_index = f.buffer_index;
    g_early.flip_mode = f.flip_mode;
    g_early.flip_arg = f.flip_arg;
    for (u32 i = 0; i < f.count; ++i) {
        g_early.dcbs[i] = f.dcbs[i];
        g_early.sizes[i] = f.dcb_sizes[i];
    }
}

/// P5.4: the result of the frame sceGnmSubmitDone handed on, for the game's sceGnmSubmitAndFlipCommandBuffers of it
/// (the same count and last DCB - the one its flip marker was patched in). nullopt for any other call: it is handed on
/// as before and the pending frame stays. The same frame with other arguments returns the result too (its DCBs and its
/// flip went already; a second patch would find no marker): logged, and the early submission is turned off.
std::optional<s32> TakeEarlyFrame(u32 count, u32* const* dcbs, const u32* dcb_sizes, u32 vo_handle, u32 buffer_index,
                                  u32 flip_mode, s64 flip_arg) {
    std::scoped_lock lk{g_submit_mutex};
    if (!g_early.pending || count != g_early.count || dcbs[count - 1] != g_early.dcbs[count - 1]) {
        return std::nullopt;
    }
    g_early.pending = false;
    bool same = vo_handle == g_early.vo_handle && buffer_index == g_early.buffer_index &&
                flip_mode == g_early.flip_mode && flip_arg == g_early.flip_arg;
    for (u32 i = 0; same && i < count; ++i) {
        same = dcbs[i] == g_early.dcbs[i] && dcb_sizes[i] == g_early.sizes[i];
    }
    if (!same) {
        g_early_source.store(nullptr, std::memory_order_release);
        SSPC_LOG(GPU, Error, "sceGnmSubmitAndFlipCommandBuffers: {} DCBs, buffer {} - the frame handed on at "
                 "sceGnmSubmitDone was taken with buffer {} (or other DCBs): that submission stands; early frame "
                 "submission off", count, buffer_index, g_early.buffer_index);
    }
    return g_early.result;
}

SSPC_SYSV_ABI s32 SubmitAndFlipCommandBuffersForWorkload(u32 /*workload*/, u32 count, u32* dcbs[], u32* dcb_sizes,
                                                         u32* ccbs[], u32* ccb_sizes, u32 vo_handle, u32 buffer_index,
                                                         u32 flip_mode, s64 flip_arg) {
    constexpr const char* kName = "sceGnmSubmitAndFlipCommandBuffers";
    if (count != 0) {
        if (dcbs == nullptr) {
            SSPC_LOG(GPU, Error, "{}: dcbGpuAddrs must not be NULL", kName);
            return kErrSubmitInvalidArgument;
        }
        if (dcb_sizes == nullptr) {
            SSPC_LOG(GPU, Error, "{}: dcbSizesInBytes must not be NULL", kName);
            return kErrSubmitInvalidArgument;
        }
        if (ccb_sizes != nullptr && ccbs == nullptr) {
            SSPC_LOG(GPU, Error, "{}: ccbGpuAddrs must not be NULL if ccbSizesInBytes is non-NULL", kName);
            return kErrSubmitInvalidArgument;
        }
    }
    if (const s32 r = CheckSizes(kName, count, dcb_sizes, ccb_sizes); r != kOk) {
        return r;
    }
    if (count == 0) {
        SSPC_LOG(GPU, Error, "{}: no command buffer (count 0) to carry the flip marker", kName);
        return kErrSubmitInvalidArgument;
    }
    if (const s32 r = CheckSupported(kName, count, dcbs, ccb_sizes); r != kOk) {
        return r;
    }
    if (g_early_installed.load(std::memory_order_acquire)) { // (P5.4: the frame may be in already)
        const u64 t0 = StallTrace::Now();
        if (const std::optional<s32> early =
                TakeEarlyFrame(count, dcbs, dcb_sizes, vo_handle, buffer_index, flip_mode, flip_arg)) {
            StallTrace::RegisterThread(StallTrace::Role::Game);
            StallTrace::Span(StallTrace::Ev::GameSubmitFlip, t0, StallTrace::Now());
            return *early;
        }
    }
    u32* const flip_dcb = dcbs[count - 1];
    const u32 flip_dcb_dwords = dcb_sizes[count - 1] / 4;

    if (FramePacing::Enabled()) [[unlikely]] {
        FramePacing::Mark(FramePacing::Event::SubmitEnter);
    }
    FrameLog::GameFlipEnter();
    StallTrace::RegisterThread(StallTrace::Role::Game); // (P4.12: its log made here once - not under the submit lock)
    const StallTrace::Scope trace_flip(StallTrace::Ev::GameSubmitFlip); // (P4.12: entry to return)
    const u64 lock_t0 = StallTrace::On() ? StallTrace::Now() : 0;
    std::scoped_lock lk{g_submit_mutex};
    if (lock_t0 != 0) {
        StallTrace::Span(StallTrace::Ev::GameSubmitLock, lock_t0, StallTrace::Now());
    }
    WaitGpuIdle();
    if (FramePacing::Enabled()) [[unlikely]] {
        FramePacing::Mark(FramePacing::Event::SubmitUnblocked);
    }
    Host* const host = CurrentHost(kName);
    if (host == nullptr) {
        return kErrSubmitInvalidArgument;
    }
    // SSPC_DIAG_SYNC_SUBMIT=3: before handing on this frame, the frames before it were processed and recorded (the game
    // stays at most one frame ahead of the backend's recording; P4.5 experiment)
    static const bool sync_before = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_SYNC_SUBMIT", v, sizeof(v)) == 1 && v[0] == '3';
    }();
    if (sync_before) {
        for (u32 i = 0; i < 300 && !host->IsRecorded(); ++i) {
            Sleep(1);
        }
    }
    const s32 patched = PatchFlipRequest(*host, flip_dcb, flip_dcb_dwords, vo_handle, buffer_index, flip_mode, flip_arg);
    if (patched != kOk) {
        return patched;
    }
    const s32 submitted = PerformSubmit(*host, count, dcbs, dcb_sizes);
    FrameLog::GameFlip(); // the flip was requested (PatchFlipRequest): its row ends here, submitted or not
    Inspector::FlipThread(); // (P4.8: this thread's CPU time per frame, when the inspector runs)
    g_last_submit_tsc.store(__rdtsc(), std::memory_order_release);
    if (FramePacing::Enabled()) [[unlikely]] {
        FramePacing::Mark(FramePacing::Event::Submitted);
    }
    // SSPC_DIAG_SYNC_SUBMIT=1: the game's thread waits (up to 300 ms) until the frame it submitted executed - the GPU
    // consumes the frame before the game writes more (an experiment on the backend's latency; P4.5)
    // (=2: only until the frame was processed and recorded - its commands may still execute)
    static const int sync_submit = [] {
        char v[4] = {};
        const int mode = GetEnvironmentVariableA("SSPC_DIAG_SYNC_SUBMIT", v, sizeof(v)) == 1 ? v[0] - '0' : 0;
        if (mode == 3) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_SYNC_SUBMIT=3 - each frame waits for the earlier "
                                   "frames' recording before it is handed on");
        }
        if (mode == 1 || mode == 2) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_SYNC_SUBMIT={} - the game waits for each frame ({})",
                     mode, mode == 1 ? "executed" : "recorded");
            return mode;
        }
        return 0;
    }();
    if (sync_submit != 0 && submitted == kOk) {
        for (u32 i = 0; i < 300 && !(sync_submit == 1 ? host->IsQuiet() : host->IsRecorded()); ++i) {
            Sleep(1);
        }
    }
    return submitted;
}

SSPC_SYSV_ABI s32 SubmitAndFlipCommandBuffers(u32 count, u32* dcbs[], u32* dcb_sizes, u32* ccbs[], u32* ccb_sizes,
                                              u32 vo_handle, u32 buffer_index, u32 flip_mode, s64 flip_arg) {
    return SubmitAndFlipCommandBuffersForWorkload(count, count, dcbs, dcb_sizes, ccbs, ccb_sizes, vo_handle,
                                                  buffer_index, flip_mode, flip_arg);
}

SSPC_SYSV_ABI s32 SubmitDone() {
    // (P5.4 SetEarlyFrameSource: the caller's frame is read through the slot of the return address into it)
    const u64* const return_slot = static_cast<const u64*>(_AddressOfReturnAddress());
    if (FramePacing::Enabled()) [[unlikely]] {
        FramePacing::Mark(FramePacing::Event::SubmitDone);
    }
    const u64 enter_tsc = __rdtsc();
    StallTrace::RegisterThread(StallTrace::Role::Game);
    const StallTrace::Scope trace_done(StallTrace::Ev::GameSubmitDone); // (P5.4: entry to return)
    std::scoped_lock lk{g_submit_mutex};
    WaitGpuIdle();
    Host* const host = CurrentHost("sceGnmSubmitDone");
    if (host == nullptr) {
        return kErrSubmitInvalidArgument;
    }
    // (P5.4: a frame handed on here takes no submission lock - the engine waits for the previous frame's label itself;
    // any other caller, or a frame the source declines, takes it as before)
    const EarlyFrameSource early = g_early_source.load(std::memory_order_acquire);
    EarlyFrame frame;
    const bool accepted = early != nullptr && AcceptEarlyFrame(early, return_slot, frame);
    if (!accepted && !host->IsGpuIdle()) {
        std::scoped_lock il{g_idle_mutex};
        g_submission_locked = true; // until Interrupt::GpuIdle, which follows host->SubmitDone()
    }
    host->SubmitDone();
    g_send_init_packet = true;
    FrameLog::GameSubmitDone(enter_tsc, __rdtsc());
    if (accepted) {
        SubmitEarlyFrame(*host, frame);
    }
    return kOk;
}

SSPC_SYSV_ABI void DingDong(u32 vqid, u32 next_offset_dw) {
    if (vqid == 0) {
        return;
    }
    StallTrace::RegisterThread(StallTrace::Role::Game);
    StallTrace::Instant(StallTrace::Ev::GameDingDong, vqid, next_offset_dw); // (P5.4: the game's compute submission)
    std::scoped_lock lk{g_submit_mutex};
    WaitGpuIdle();
    if (Host* const host = CurrentHost("sceGnmDingDong")) {
        host->DingDong(vqid, next_offset_dw); // false: the backend logged why; the command processor keeps the offset
    }
}

SSPC_SYSV_ABI void DingDongForWorkload(u32 vqid, u32 next_offset_dw, u64 /*workload_id*/) {
    DingDong(vqid, next_offset_dw);
}

// --- compute queues

SSPC_SYSV_ABI s32 MapComputeQueue(u32 pipe_id, u32 queue_id, u64 ring_base, u32 ring_size_dw, u32* read_ptr) {
    constexpr const char* kName = "sceGnmMapComputeQueue";
    if (pipe_id >= kNumComputePipes) {
        return kErrQueueInvalidPipeId;
    }
    if (queue_id >= kNumQueuesPerPipe) {
        return kErrQueueInvalidQueueId;
    }
    if (ring_base % 256 != 0) {
        return kErrQueueInvalidRingBase;
    }
    if (!std::has_single_bit(ring_size_dw)) {
        return kErrQueueInvalidRingSize;
    }
    if (reinterpret_cast<u64>(read_ptr) % 4 != 0) {
        return kErrQueueInvalidReadPtr;
    }
    if (read_ptr == nullptr) {
        SSPC_LOG(GPU, Error, "{}: null read pointer address (the bridge writes 0 through it)", kName);
        return kErrQueueInvalidReadPtr;
    }
    std::scoped_lock lk{g_queue_mutex};
    Host* const host = CurrentHost(kName);
    if (host == nullptr) {
        return kErrQueueInternal;
    }
    if (g_free_slots.empty()) {
        SSPC_LOG(GPU, Error, "{}: all {} compute queues are mapped (the bridge asserts)", kName, kMaxComputeQueues);
        return kErrQueueInternal;
    }
    const u32 slot = g_free_slots.back();
    const u32 vqid = slot + 1; // DingDong's queue ids are 1..64
    *read_ptr = 0;
    if (!host->MapComputeQueue(vqid, pipe_id, ring_base, ring_size_dw, reinterpret_cast<u64>(read_ptr))) {
        SSPC_LOG(GPU, Error, "{}: the backend refused vqueue {} (pipe {}, ring {:#x}, {} dwords)", kName, vqid,
                 pipe_id, ring_base, ring_size_dw);
        return kErrQueueInternal;
    }
    g_free_slots.pop_back();
    g_mapped.set(slot);
    SSPC_LOG(GPU, Info, "GnmDriver: compute pipe {} queue {} mapped to vqueue {} (ring {:#x}, {} dwords)", pipe_id,
             queue_id, vqid, ring_base, ring_size_dw);
    return static_cast<s32>(vqid);
}

SSPC_SYSV_ABI s32 MapComputeQueueWithPriority(u32 pipe_id, u32 queue_id, u64 ring_base, u32 ring_size_dw,
                                              u32* read_ptr, u32 /*pipe_priority*/) {
    return MapComputeQueue(pipe_id, queue_id, ring_base, ring_size_dw, read_ptr);
}

SSPC_SYSV_ABI s32 UnmapComputeQueue(u32 vqid) {
    std::scoped_lock lk{g_queue_mutex};
    if (vqid == 0 || vqid > kMaxComputeQueues || !g_mapped.test(vqid - 1)) {
        SSPC_LOG(GPU, Error, "sceGnmUnmapComputeQueue: vqueue {} is not mapped", vqid);
        return kErrQueueInternal;
    }
    Host* const host = CurrentHost("sceGnmUnmapComputeQueue");
    if (host == nullptr) {
        return kErrQueueInternal;
    }
    host->UnmapComputeQueue(vqid);
    g_mapped.reset(vqid - 1);
    g_free_slots.push_back(vqid - 1);
    return kOk;
}

// --- dispatch

SSPC_SYSV_ABI s32 DispatchDirect(u32* cmdbuf, u32 size, u32 threads_x, u32 threads_y, u32 threads_z, u32 flags) {
    if (cmdbuf == nullptr || size != 9 || static_cast<s32>(threads_x | threads_y | threads_z) < 0) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DispatchDirect, 4, true, (flags & 1) != 0);
    p[0] = threads_x;
    p[1] = threads_y;
    p[2] = threads_z;
    p[3] = (flags & 0x18) + 1; // dispatch initiator: compute shader enable, ordered append mode
    TrailingNop(p + 4, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DispatchIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 flags) {
    if (cmdbuf == nullptr || size != 7) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DispatchIndirect, 2, true, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = (flags & 0x18) + 1;
    TrailingNop(p + 2, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DispatchIndirectOnMec(u32* cmdbuf, u32 size, u64 args, u32 modifier) {
    if (cmdbuf == nullptr || size != 8 || args == 0 || (args & 3) != 0) {
        return kFail;
    }
    cmdbuf[0] = Type3(Op::DispatchIndirect, 2, true) | (modifier & 1); // the arguments' address (no SET_BASE on a MEC)
    Store64(cmdbuf + 1, args);
    cmdbuf[3] = (modifier & 0x18) | 1;
    cmdbuf[4] = Type3(Op::Nop, 2);
    cmdbuf[5] = 0;
    return kOk;
}

SSPC_SYSV_ABI u32 DispatchInitDefaultHardwareState(u32* cmdbuf, u32 size) {
    if (size < kHwInitPacketSize) {
        return 0;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer("sceGnmDispatchInitDefaultHardwareState");
        return 0;
    }
    u32* p = SetShCompute(cmdbuf, {0x216u, 0xFFFFFFFFu}); // COMPUTE_STATIC_THREAD_MGMT_SE0
    p = SetShCompute(p, {0x217u, 0xFFFFFFFFu});            // COMPUTE_STATIC_THREAD_MGMT_SE1
    p = SetShCompute(p, {0x215u, 0x170u});                 // COMPUTE_RESOURCE_LIMITS
    p = Packet(p, Op::AcquireMem, {0x28000000u, 0u, 0u, 0u, 0u, 0xAu});
    TrailingNop(p, 0xEF);
    return kHwInitPacketSize;
}

// --- draws

SSPC_SYSV_ABI s32 DrawIndex(u32* cmdbuf, u32 size, u32 index_count, u64 index_addr, u32 flags, u32 /*type*/) {
    // flags: only the predication bit may be set, and it is not put into the packet (as the bridge)
    if (cmdbuf == nullptr || size != 10 || index_addr == 0 || (index_addr & 1) != 0 || (flags & 0x1FFFFFFEu) != 0) {
        return kFail;
    }
    cmdbuf[0] = Type3(Op::DrawIndex2, 4);
    cmdbuf[1] = index_count; // max size
    cmdbuf[2] = static_cast<u32>(index_addr);
    cmdbuf[3] = static_cast<u32>(index_addr >> 32);
    cmdbuf[4] = index_count;
    cmdbuf[5] = 0; // draw initiator (Neo: the flags' top 3 bits)
    TrailingNop(cmdbuf + 6, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndexAuto(u32* cmdbuf, u32 size, u32 index_count, u32 flags) {
    if (cmdbuf == nullptr || size != 7 || (flags & 0x1FFFFFFEu) != 0) {
        return kFail;
    }
    TrailingNop(Packet(cmdbuf, Op::DrawIndexAuto, {index_count, 2u}), 3); // draw initiator: auto index
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndexIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage, u32 vertex_sgpr_offset,
                                    u32 instance_sgpr_offset, u32 flags) {
    if (cmdbuf == nullptr || size != 9 || shader_stage >= kNumStages || vertex_sgpr_offset >= 0x10 ||
        instance_sgpr_offset >= 0x10) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DrawIndexIndirect, 4, false, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = SgprOffset(vertex_sgpr_offset, shader_stage);
    p[2] = SgprOffset(instance_sgpr_offset, shader_stage);
    p[3] = 0; // draw initiator
    TrailingNop(p + 4, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndexIndirectCountMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count, u64 count_addr,
                                              u32 shader_stage, u32 vertex_sgpr_offset, u32 instance_sgpr_offset,
                                              u32 flags) {
    if (cmdbuf == nullptr || size != 16 || vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10 ||
        !IsVertexStage(shader_stage)) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::Nop, 2);
    p[0] = 0;
    p = Header(p + 2, Op::DrawIndexIndirectCountMulti, 9, false, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = SgprOffset(vertex_sgpr_offset, shader_stage);
    p[2] = SgprOffset(instance_sgpr_offset, shader_stage);
    p[3] = (count_addr != 0 ? 1u : 0u) << 30; // count from memory
    p[4] = max_count;
    Store64(p + 5, count_addr);
    p[7] = kDrawIndexedIndirectArgsSize; // stride
    p[8] = 0;                            // draw initiator
    TrailingNop(p + 9, 2);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndexIndirectMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count, u32 shader_stage,
                                         u32 vertex_sgpr_offset, u32 instance_sgpr_offset, u32 flags) {
    if (cmdbuf == nullptr || size != 11 || vertex_sgpr_offset >= 0x10 || instance_sgpr_offset >= 0x10 ||
        !IsVertexStage(shader_stage)) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DrawIndexIndirectMulti, 6, false, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = SgprOffset(vertex_sgpr_offset, shader_stage);
    p[2] = SgprOffset(instance_sgpr_offset, shader_stage);
    p[3] = max_count;
    p[4] = kDrawIndexedIndirectArgsSize;
    p[5] = 0;
    TrailingNop(p + 6, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndexMultiInstanced() {
    static std::atomic<bool> logged;
    return Unsupported(logged, "sceGnmDrawIndexMultiInstanced");
}

SSPC_SYSV_ABI s32 DrawIndexOffset(u32* cmdbuf, u32 size, u32 index_offset, u32 index_count, u32 flags) {
    if (cmdbuf == nullptr || size != 9) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DrawIndexOffset2, 4, false, (flags & 1) != 0);
    p[0] = index_count; // max size
    p[1] = index_offset;
    p[2] = index_count;
    p[3] = 0; // draw initiator
    TrailingNop(p + 4, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndirect(u32* cmdbuf, u32 size, u32 data_offset, u32 shader_stage, u32 vertex_sgpr_offset,
                               u32 instance_sgpr_offset, u32 flags) {
    if (cmdbuf == nullptr || size != 9 || shader_stage >= kNumStages || vertex_sgpr_offset >= 0x10 ||
        instance_sgpr_offset >= 0x10) {
        return kFail;
    }
    u32* p = Header(cmdbuf, Op::DrawIndirect, 4, false, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = SgprOffset(vertex_sgpr_offset, shader_stage);
    p[2] = SgprOffset(instance_sgpr_offset, shader_stage);
    p[3] = 2; // draw initiator: auto index
    TrailingNop(p + 4, 3);
    return kOk;
}

SSPC_SYSV_ABI s32 DrawIndirectCountMulti() {
    static std::atomic<bool> logged;
    return Unsupported(logged, "sceGnmDrawIndirectCountMulti");
}

SSPC_SYSV_ABI s32 DrawIndirectMulti(u32* cmdbuf, u32 size, u32 data_offset, u32 max_count, u32 shader_stage,
                                    u32 vertex_sgpr_offset, u32 instance_sgpr_offset, u32 flags) {
    if (cmdbuf == nullptr || size != 11 || shader_stage >= kNumStages || vertex_sgpr_offset >= 0x10 ||
        instance_sgpr_offset >= 0x10) {
        return kFail;
    }
    // The bridge's header counts 4 body dwords for the 6 it writes: kept, the same dwords.
    u32* p = Header(cmdbuf, Op::DrawIndirectMulti, 4, false, (flags & 1) != 0);
    p[0] = data_offset;
    p[1] = SgprOffset(vertex_sgpr_offset, shader_stage);
    p[2] = SgprOffset(instance_sgpr_offset, shader_stage);
    p[3] = max_count;
    p[4] = kDrawIndirectArgsSize;
    p[5] = 2; // draw initiator: auto index
    TrailingNop(p + 6, 3);
    return kOk;
}

SSPC_SYSV_ABI u32 DrawInitDefaultHardwareState350(u32* cmdbuf, u32 size) {
    if (size < kHwInitPacketSize) {
        return 0;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer("sceGnmDrawInitDefaultHardwareState350");
        return 0;
    }
    u32* const end = cmdbuf + kHwInitPacketSize;
    u32* p = std::copy(kClearStateSequence.begin(), kClearStateSequence.end(), cmdbuf);
    p = std::copy(kInitSequence350.begin() + 2, kInitSequence350.end(), p); // without the fake CLEAR_STATE preamble
    TrailingNop(p, static_cast<u32>(end - p - 1));
    return kHwInitPacketSize;
}

SSPC_SYSV_ABI u32 DrawInitToDefaultContextState400(u32* cmdbuf, u32 size) {
    constexpr u32 kCtxInitPacketSize = 0x100;
    if (size != kCtxInitPacketSize) {
        return 0;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer("sceGnmDrawInitToDefaultContextState400");
        return 0;
    }
    std::copy(kCtxInitSequence400.begin(), kCtxInitSequence400.end(), cmdbuf); // its last NOP covers the rest
    return kCtxInitPacketSize;
}

// --- markers

/// sceGnmInsertPushMarker / InsertSetMarker: a NOP with the marker text (NUL-terminated, zero-padded).
s32 InsertTextMarker(u32* cmdbuf, u32 size, const char* marker, u32 payload_type) {
    if (cmdbuf == nullptr || marker == nullptr) {
        return kFail;
    }
    const size_t length = std::strlen(marker);
    const u32 packet_size = static_cast<u32>(((length + 8) >> 2) + ((length + 0xC) >> 3) * 2);
    if (packet_size + 2 != size) {
        return kFail;
    }
    cmdbuf[0] = Type3(Op::Nop, packet_size);
    cmdbuf[1] = payload_type;
    u8* const text = reinterpret_cast<u8*>(cmdbuf + 2);
    std::memcpy(text, marker, length + 1);
    std::memset(text + length + 1, 0, size_t{packet_size} * 4 - (length + 1));
    return kOk;
}

SSPC_SYSV_ABI s32 InsertPushMarker(u32* cmdbuf, u32 size, const char* marker) {
    return InsertTextMarker(cmdbuf, size, marker, kDebugMarkerPush);
}

SSPC_SYSV_ABI s32 InsertSetMarker(u32* cmdbuf, u32 size, const char* marker) {
    return InsertTextMarker(cmdbuf, size, marker, kDebugSetMarker);
}

SSPC_SYSV_ABI s32 InsertPushColorMarker(u32* cmdbuf, u32 size, const char* marker, u32 color) {
    if (cmdbuf == nullptr || marker == nullptr) {
        return kFail;
    }
    const size_t length = std::strlen(marker);
    const u32 packet_size = static_cast<u32>(((length + 0xC) >> 2) + ((length + 0x10) >> 3) * 2);
    if (packet_size + 2 != size) {
        return kFail;
    }
    cmdbuf[0] = Type3(Op::Nop, packet_size);
    cmdbuf[1] = kDebugColorMarkerPush;
    u8* const text = reinterpret_cast<u8*>(cmdbuf + 2);
    std::memcpy(text, marker, length + 1);
    // 8 bytes after the text stay as they are (the bridge does not write them), then the colour, then zeros
    std::memcpy(text + length + 1 + 8, &color, sizeof(color));
    std::memset(text + length + 1 + 8 + sizeof(color), 0, size_t{packet_size} * 4 - (length + 1) - 8 - sizeof(color));
    return kOk;
}

SSPC_SYSV_ABI s32 InsertPopMarker(u32* cmdbuf, u32 size) {
    if (cmdbuf == nullptr || size != 6) {
        return kFail;
    }
    Nop(cmdbuf, {kDebugMarkerPop, 0u, 0u, 0u, 0u});
    return kOk;
}

SSPC_SYSV_ABI s32 InsertDingDongMarker(u32* cmdbuf, u32 size) {
    if (cmdbuf == nullptr || size != 4) {
        return kFail;
    }
    Nop(cmdbuf, {0u, 0u, 0u});
    return kOk;
}

SSPC_SYSV_ABI s32 InsertWaitFlipDone(u32* cmdbuf, u32 size, s32 vo_handle, u32 buffer_index) {
    constexpr const char* kName = "sceGnmInsertWaitFlipDone";
    if (size != 7) {
        return kFail;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer(kName);
        return kFail;
    }
    u64 labels = 0;
    {
        std::scoped_lock lk{g_label_mutex};
        Host* const host = CurrentHost(kName);
        if (host == nullptr) {
            return kErrSubmitInvalidArgument;
        }
        if (!host->BufferLabelAddress(vo_handle, labels)) {
            SSPC_LOG(GPU, Error, "{}: no buffer labels for VideoOut handle {} (the bridge asserts)", kName, vo_handle);
            return kErrSubmitInvalidArgument;
        }
    }
    // WAIT_REG_MEM: memory, equal, from the PFP — until the buffer's label is 0 (the flip released the buffer)
    cmdbuf[0] = Type3(Op::WaitRegMem, 5);
    cmdbuf[1] = 0x13u;
    Store64(cmdbuf + 2, (labels + u64{buffer_index} * sizeof(u64)) & ~3ull);
    cmdbuf[4] = 0;           // reference
    cmdbuf[5] = 0xFFFFFFFFu; // mask
    cmdbuf[6] = 10;          // poll interval
    return kOk;
}

// --- state

SSPC_SYSV_ABI s32 ResetVgtControl(u32* cmdbuf, u32 size) {
    if (cmdbuf == nullptr || size != 3) {
        return kFail;
    }
    SetContext(cmdbuf, {0x2AAu, 0xFFu}); // IA_MULTI_VGT_PARAM
    return kOk;
}

SSPC_SYSV_ABI s32 SetVgtControl(u32* cmdbuf, u32 size, u32 prim_group_size_minus_one, u32 partial_vs_wave_mode,
                                u32 wd_switch_only_on_eop_mode) {
    if (cmdbuf == nullptr || size != 3 || prim_group_size_minus_one >= 0x100 ||
        (wd_switch_only_on_eop_mode | partial_vs_wave_mode) >= 2) {
        return kFail;
    }
    SetContext(cmdbuf, {0x2AAu, ((partial_vs_wave_mode & 1) << 16) | (prim_group_size_minus_one & 0xFFFFu)});
    return kOk;
}

// --- shaders

// clang-format off
/// sceGnmSetEmbeddedPsShader 0 (the PS4 driver's at 0xfe000f00): writes 0 to MRT0. Code, binary header, PS registers.
alignas(256) constexpr std::array kEmbeddedPs0{
    0xbeeb03ffu, 0x00000003u, // s_mov_b32     vcc_hi, $0x00000003
    0x7e000280u,              // v_mov_b32     v0, 0
    0x5e000100u,              // v_cvt_pkrtz_f16_f32 v0, v0, v0
    0xbf800000u,              // s_nop
    0xf8001c0fu, 0x00000000u, // exp           mrt0, v0, v0 compr vm done
    0xbf810000u,              // s_endpgm
    0x5362724fu, 0x07726468u, 0x00002043u, 0u, 0xb0a45b2bu, 0x1d39766du, 0x72044b7bu, 0x0000000fu,
    0x0fe000f0u, 0u, 0xc0000u, 4u, 0u, 4u, 2u, 2u, 0u, 0u, 0x10u, 0xfu, 0xcu, 0u, 0u, 0u,
};
static_assert(kEmbeddedPs0.size() == 32);

/// sceGnmSetEmbeddedPsShader 1 (0xfe000f20): writes 0 to MRT0, two 32-bit channels.
alignas(256) constexpr std::array kEmbeddedPs1{
    0xbeeb03ffu, 0x00000003u, // s_mov_b32     vcc_hi, $0x00000003
    0x7e040280u,              // v_mov_b32     v2, 0
    0xf8001803u, 0x02020202u, // exp           mrt0, v2, v2, off, off vm done
    0xbf810000u,              // s_endpgm
    0x5362724fu, 0x07726468u, 0x00001841u, 0x04080002u, 0x98b9cb94u, 0u, 0x6f130734u, 0x0000000fu,
    0x0fe000f2u, 0u, 0x2000u, 0u, 0u, 2u, 2u, 2u, 0u, 0u, 0x10u, 3u, 0xcu,
};
static_assert(kEmbeddedPs1.size() == 27);

/// sceGnmSetEmbeddedVsShader 0 (0xfe000f10): a full-screen triangle with one UV set.
alignas(256) constexpr std::array kEmbeddedVs0{
    0xbeeb03ffu, 0x00000007u, // s_mov_b32     vcc_hi, $0x00000007
    0x36020081u,              // v_and_b32     v1, 1, v0
    0x34020281u,              // v_lshlrev_b32 v1, 1, v1
    0x360000c2u,              // v_and_b32     v0, -2, v0
    0x4a0202c1u,              // v_add_i32     v1, vcc, -1, v1
    0x4a0000c1u,              // v_add_i32     v0, vcc, -1, v0
    0x7e020b01u,              // v_cvt_f32_i32 v1, v1
    0x7e000b00u,              // v_cvt_f32_i32 v0, v0
    0x7e040280u,              // v_mov_b32     v2, 0
    0x7e0602f2u,              // v_mov_b32     v3, 1.0
    0xf80008cfu, 0x03020001u, // exp           pos0, v1, v0, v2, v3 done
    0xf800020fu, 0x03030303u, // exp           param0, v3, v3, v3, v3
    0xbf810000u,              // s_endpgm
    0x5362724fu, 0x07726468u, 0x00004047u, 0u, 0x47f8c29fu, 0x9b2da5cfu, 0xff7c5b7du, 0x00000017u,
    0x0fe000f1u, 0u, 0x000c0000u, 4u, 0u, 4u, 0u, 7u,
};
static_assert(kEmbeddedVs0.size() == 32);

/// The embedded shaders' register values after the program address (the bridge's ps0_regs/ps1_regs/vs_regs [2..]).
constexpr std::array<u32, 11> kEmbeddedPs0Regs{0xc0000u, 4u, 0u, 4u, 2u, 2u, 0u, 0u, 0x10u, 0xfu, 0xcu};
constexpr std::array<u32, 11> kEmbeddedPs1Regs{0x2000u, 0u, 0u, 2u, 2u, 2u, 0u, 0u, 0x10u, 3u, 0xcu};
constexpr std::array<u32, 6> kEmbeddedVs0Regs{0xc0000u, 4u, 0u, 4u, 0u, 7u};
// clang-format on

/// The PS registers of sceGnmSetPsShader350 and the embedded PS: `r` indexed as the game's ps_regs (r[2..11]).
u32* SetPsRegs(u32* p, u32 program_lo, u32 program_hi, const u32* r) {
    p = SetSh(p, {8u, program_lo, program_hi});  // SPI_SHADER_PGM_LO_PS/HI_PS
    p = SetSh(p, {10u, r[2], r[3]});             // SPI_SHADER_PGM_RSRC1_PS/RSRC2_PS
    p = SetContext(p, {0x1C4u, r[4], r[5]});     // SPI_SHADER_Z_FORMAT/COL_FORMAT
    p = SetContext(p, {0x1B3u, r[6], r[7]});     // SPI_PS_INPUT_ENA/ADDR
    p = SetContext(p, {0x1B6u, r[8]});           // SPI_PS_IN_CONTROL
    p = SetContext(p, {0x1B8u, r[9]});           // SPI_BARYC_CNTL
    p = SetContext(p, {0x203u, r[10]});          // DB_SHADER_CONTROL
    return SetContext(p, {0x8Fu, r[11]});        // CB_SHADER_MASK
}

/// The VS registers of sceGnmSetVsShader and the embedded VS: `r` indexed as the game's vs_regs (r[3..6]).
u32* SetVsRegs(u32* p, u32 program_lo, u32 program_hi, u32 rsrc1, const u32* r) {
    p = SetSh(p, {0x48u, program_lo, program_hi}); // SPI_SHADER_PGM_LO_VS/HI_VS
    p = SetSh(p, {0x4Au, rsrc1, r[3]});            // SPI_SHADER_PGM_RSRC1_VS/RSRC2_VS
    p = SetContext(p, {0x207u, r[6]});             // PA_CL_VS_OUT_CNTL
    p = SetContext(p, {0x1B1u, r[4]});             // SPI_VS_OUT_CONFIG
    return SetContext(p, {0x1C3u, r[5]});          // SPI_SHADER_POS_FORMAT
}

SSPC_SYSV_ABI s32 SetCsShaderWithModifier(u32* cmdbuf, u32 size, const u32* cs_regs, u32 modifier) {
    constexpr const char* kName = "sceGnmSetCsShaderWithModifier";
    if (cmdbuf == nullptr || size <= 0x18) {
        return kFail;
    }
    if (cs_regs == nullptr) {
        return Refuse(kName, "null pointer in shader registers");
    }
    if ((modifier & 0xFFFFFC3Fu) != 0) {
        return Refuse(kName, "invalid modifier mask {:#x}", modifier);
    }
    if (cs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    const u32 rsrc1 = modifier == 0 ? cs_regs[2] : (cs_regs[2] & 0xFFFFFC3Fu) | modifier;
    u32* p = SetSh(cmdbuf, {0x20Cu, cs_regs[0], 0u});           // COMPUTE_PGM_LO/HI
    p = SetSh(p, {0x212u, rsrc1, cs_regs[3]});                  // COMPUTE_PGM_RSRC1/RSRC2
    p = SetSh(p, {0x207u, cs_regs[4], cs_regs[5], cs_regs[6]}); // COMPUTE_NUM_THREAD_X/Y/Z
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetEmbeddedPsShader(u32* cmdbuf, u32 /*size*/, u32 shader_id, u32 /*shader_modifier*/) {
    constexpr const char* kName = "sceGnmSetEmbeddedPsShader";
    if (shader_id > 1) {
        SSPC_LOG(GPU, Error, "{}: unknown shader id {}", kName, shader_id);
        return kErrFailure;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer(kName);
        return kFail;
    }
    // The program lives in this executable, as the bridge's in shadPS4's (the PS4's at 0xfe000f00/0xfe000f20).
    const u64 code = reinterpret_cast<u64>(shader_id == 0 ? kEmbeddedPs0.data() : kEmbeddedPs1.data());
    const auto& tail = shader_id == 0 ? kEmbeddedPs0Regs : kEmbeddedPs1Regs;
    std::array<u32, 13> regs{};
    std::copy(tail.begin(), tail.end(), regs.begin() + 2);
    TrailingNop(SetPsRegs(cmdbuf, static_cast<u32>(code >> 8), static_cast<u32>(code >> 40), regs.data()), 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetEmbeddedVsShader(u32* cmdbuf, u32 /*size*/, u32 shader_id, u32 /*shader_modifier*/) {
    constexpr const char* kName = "sceGnmSetEmbeddedVsShader";
    if (shader_id != 0) {
        SSPC_LOG(GPU, Error, "{}: unknown shader id {}", kName, shader_id);
        return kErrFailure;
    }
    if (cmdbuf == nullptr) {
        NullCommandBuffer(kName);
        return kFail;
    }
    const u64 code = reinterpret_cast<u64>(kEmbeddedVs0.data()); // the PS4's at 0xfe000f10
    std::array<u32, 8> regs{};
    std::copy(kEmbeddedVs0Regs.begin(), kEmbeddedVs0Regs.end(), regs.begin() + 2);
    TrailingNop(SetVsRegs(cmdbuf, static_cast<u32>(code >> 8), static_cast<u32>(code >> 40), regs[2], regs.data()), 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetEsShader(u32* cmdbuf, u32 size, const u32* es_regs, u32 shader_modifier) {
    constexpr const char* kName = "sceGnmSetEsShader";
    if (cmdbuf == nullptr || size < 0x14) {
        return kFail;
    }
    if (es_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if ((shader_modifier & 0xFCFFFC3Fu) != 0) {
        return Refuse(kName, "invalid modifier mask {:#x}", shader_modifier);
    }
    if (es_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    const u32 rsrc1 = shader_modifier == 0 ? es_regs[2] : (es_regs[2] & 0xFCFFFC3Fu) | shader_modifier;
    u32* p = SetSh(cmdbuf, {0xC8u, es_regs[0], 0u}); // SPI_SHADER_PGM_LO_ES/HI_ES
    p = SetSh(p, {0xCAu, rsrc1, es_regs[3]});         // SPI_SHADER_PGM_RSRC1_ES/RSRC2_ES
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetGsShader(u32* cmdbuf, u32 size, const u32* gs_regs) {
    constexpr const char* kName = "sceGnmSetGsShader";
    if (cmdbuf == nullptr || size < 0x1D) {
        return kFail;
    }
    if (gs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if (gs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    u32* p = SetSh(cmdbuf, {0x88u, gs_regs[0], 0u}); // SPI_SHADER_PGM_LO_GS/HI_GS
    p = SetSh(p, {0x8Au, gs_regs[2], gs_regs[3]});    // SPI_SHADER_PGM_RSRC1_GS/RSRC2_GS
    p = SetContext(p, {0x2E5u, gs_regs[4]});          // VGT_STRMOUT_CONFIG
    p = SetContext(p, {0x29Bu, gs_regs[5]});          // VGT_GS_OUT_PRIM_TYPE
    p = SetContext(p, {0x2E4u, gs_regs[6]});          // VGT_GS_INSTANCE_CNT
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetHsShader(u32* cmdbuf, u32 size, const u32* hs_regs, u32 ls_hs_config) {
    constexpr const char* kName = "sceGnmSetHsShader";
    if (cmdbuf == nullptr || size < 0x1E) {
        return kFail;
    }
    if (hs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if (hs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    u32* p = SetSh(cmdbuf, {0x108u, hs_regs[0], 0u});   // SPI_SHADER_PGM_LO_HS/HI_HS
    p = SetSh(p, {0x10Au, hs_regs[2], hs_regs[3]});      // SPI_SHADER_PGM_RSRC1_HS/RSRC2_HS
    p = SetContext(p, {0x286u, hs_regs[5], hs_regs[6]}); // VGT_HOS_MAX_TESS_LEVEL/MIN_TESS_LEVEL
    p = SetContext(p, {0x2DBu, hs_regs[4]});             // VGT_TF_PARAM
    p = SetContext(p, {0x2D6u, ls_hs_config});           // VGT_LS_HS_CONFIG
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetLsShader(u32* cmdbuf, u32 size, const u32* ls_regs, u32 shader_modifier) {
    constexpr const char* kName = "sceGnmSetLsShader";
    if (cmdbuf == nullptr || size < 0x17) {
        return kFail;
    }
    if (ls_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    const u32 mask = (shader_modifier & 0xFFFFFC3Fu) == 0 ? 0xFFFFFC3Fu : 0xFCFFFC3Fu;
    if ((shader_modifier & mask) != 0) {
        return Refuse(kName, "invalid modifier mask {:#x}", shader_modifier);
    }
    if (ls_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    const u32 rsrc1 = shader_modifier == 0 ? ls_regs[2] : (ls_regs[2] & mask) | shader_modifier;
    u32* p = SetSh(cmdbuf, {0x148u, ls_regs[0], 0u}); // SPI_SHADER_PGM_LO_LS/HI_LS
    p = SetSh(p, {0x14Bu, ls_regs[3]});                // SPI_SHADER_PGM_RSRC2_LS
    p = SetSh(p, {0x14Au, rsrc1, ls_regs[3]});         // SPI_SHADER_PGM_RSRC1_LS/RSRC2_LS
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetPsShader350(u32* cmdbuf, u32 size, const u32* ps_regs) {
    if (cmdbuf == nullptr || size <= 0x27) {
        return kFail;
    }
    if (ps_regs == nullptr) { // no pixel shader
        u32* p = SetSh(cmdbuf, {8u, 0u, 0u});   // SPI_SHADER_PGM_LO_PS/HI_PS
        p = SetContext(p, {0x203u, 0u});         // DB_SHADER_CONTROL
        p = SetContext(p, {0x8Fu, 0xFu});        // CB_SHADER_MASK
        TrailingNop(p, 0x1D);
        return kOk;
    }
    if (ps_regs[1] != 0) {
        return Refuse("sceGnmSetPsShader350", "invalid shader address");
    }
    TrailingNop(SetPsRegs(cmdbuf, ps_regs[0], 0u, ps_regs), 11);
    return kOk;
}

SSPC_SYSV_ABI s32 SetVsShader(u32* cmdbuf, u32 size, const u32* vs_regs, u32 shader_modifier) {
    constexpr const char* kName = "sceGnmSetVsShader";
    if (cmdbuf == nullptr || size <= 0x1C) {
        return kFail;
    }
    if (vs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if ((shader_modifier & 0xFCFFFC3Fu) != 0) {
        return Refuse(kName, "invalid modifier mask {:#x}", shader_modifier);
    }
    if (vs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    const u32 rsrc1 = shader_modifier == 0 ? vs_regs[2] : (vs_regs[2] & 0xFCFFFC3Fu) | shader_modifier;
    TrailingNop(SetVsRegs(cmdbuf, vs_regs[0], 0u, rsrc1, vs_regs), 11);
    return kOk;
}

SSPC_SYSV_ABI s32 UpdateGsShader(u32* cmdbuf, u32 size, const u32* gs_regs) {
    constexpr const char* kName = "sceGnmUpdateGsShader";
    if (cmdbuf == nullptr || size < 0x1D) {
        return kFail;
    }
    if (gs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if (gs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    u32* p = SetSh(cmdbuf, {0x88u, gs_regs[0], 0u});
    p = SetSh(p, {0x8Au, gs_regs[2], gs_regs[3]});
    p = Nop(p, {0xC01E02E5u, gs_regs[4]}); // VGT_STRMOUT_CONFIG update
    p = Nop(p, {0xC01E029Bu, gs_regs[5]}); // VGT_GS_OUT_PRIM_TYPE update
    p = Nop(p, {0xC01E02E4u, gs_regs[6]}); // VGT_GS_INSTANCE_CNT update
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 UpdateHsShader(u32* cmdbuf, u32 size, const u32* hs_regs, u32 ls_hs_config) {
    constexpr const char* kName = "sceGnmUpdateHsShader";
    if (cmdbuf == nullptr || size <= 0x1C) {
        return kFail;
    }
    if (hs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if (hs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    u32* p = SetSh(cmdbuf, {0x108u, hs_regs[0], 0u});
    p = SetSh(p, {0x10Au, hs_regs[2], hs_regs[3]});
    p = Nop(p, {0xC01E0286u, hs_regs[5], hs_regs[6]}); // VGT_HOS_MAX/MIN_TESS_LEVEL update
    p = Nop(p, {0xC01E02DBu, hs_regs[4]});             // VGT_TF_PARAM update
    p = Nop(p, {0xC01E02D6u, ls_hs_config});           // VGT_LS_HS_CONFIG update
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 UpdatePsShader350(u32* cmdbuf, u32 size, const u32* ps_regs) {
    if (cmdbuf == nullptr || size <= 0x27) {
        return kFail;
    }
    if (ps_regs == nullptr) {
        u32* p = SetSh(cmdbuf, {8u, 0u, 0u});
        p = Nop(p, {0xC01E0203u, 0u});    // DB_SHADER_CONTROL update
        p = SetContext(p, {0x8Fu, 0xFu}); // CB_SHADER_MASK
        TrailingNop(p, 0x1D);
        return kOk;
    }
    if (ps_regs[1] != 0) {
        return Refuse("sceGnmUpdatePsShader350", "invalid shader address");
    }
    u32* p = SetSh(cmdbuf, {8u, ps_regs[0], 0u});
    p = SetSh(p, {10u, ps_regs[2], ps_regs[3]});
    p = Nop(p, {0xC01E01C4u, ps_regs[4], ps_regs[5]}); // SPI_SHADER_Z_FORMAT/COL_FORMAT update
    p = Nop(p, {0xC01E01B3u, ps_regs[6], ps_regs[7]}); // SPI_PS_INPUT_ENA/ADDR update
    p = Nop(p, {0xC01E01B6u, ps_regs[8]});             // SPI_PS_IN_CONTROL update
    p = Nop(p, {0xC01E01B8u, ps_regs[9]});             // SPI_BARYC_CNTL update
    p = Nop(p, {0xC01E0203u, ps_regs[10]});            // DB_SHADER_CONTROL update
    p = Nop(p, {0xC01E008Fu, ps_regs[11]});            // CB_SHADER_MASK update
    TrailingNop(p, 11);
    return kOk;
}

SSPC_SYSV_ABI s32 UpdateVsShader(u32* cmdbuf, u32 size, const u32* vs_regs, u32 shader_modifier) {
    constexpr const char* kName = "sceGnmUpdateVsShader";
    if (cmdbuf == nullptr || size <= 0x1C) {
        return kFail;
    }
    if (vs_regs == nullptr) {
        return Refuse(kName, "null pointer passed as argument");
    }
    if ((shader_modifier & 0xFCFFFC3Fu) != 0) {
        return Refuse(kName, "invalid modifier mask {:#x}", shader_modifier);
    }
    if (vs_regs[1] != 0) {
        return Refuse(kName, "invalid shader address");
    }
    const u32 rsrc1 = shader_modifier == 0 ? vs_regs[2] : (vs_regs[2] & 0xFCFFFC3Fu) | shader_modifier;
    u32* p = SetSh(cmdbuf, {0x48u, vs_regs[0], 0u});
    p = SetSh(p, {0x4Au, rsrc1, vs_regs[3]});
    p = Nop(p, {0xC01E0207u, vs_regs[6]}); // PA_CL_VS_OUT_CNTL update
    p = Nop(p, {0xC01E01B1u, vs_regs[4]}); // SPI_VS_OUT_CONFIG update
    p = Nop(p, {0xC01E01C3u, vs_regs[5]}); // SPI_SHADER_POS_FORMAT update
    TrailingNop(p, 11);
    return kOk;
}

// --- workloads, values

SSPC_SYSV_ABI s32 BeginWorkload(u32 workload_stream, u64* workload) {
    if (workload == nullptr) {
        return 3;
    }
    *workload = workload_stream < 0x10 ? 1 : 0;
    return workload_stream > 0xF ? 1 : 0;
}

SSPC_SYSV_ABI s32 CreateWorkloadStream(u64 param, u32* workload_stream) {
    if (param != 0 && workload_stream != nullptr) {
        *workload_stream = 1;
        return 0;
    }
    return 3;
}

SSPC_SYSV_ABI s32 EndWorkload(u64 workload) {
    if (workload != 0) {
        return (((workload >> 56) & 0xFF) > 0xF) ? 2 : 0;
    }
    return 2;
}

SSPC_SYSV_ABI s32 GetOffChipTessellationBufferSize() {
    return static_cast<s32>(kOffChipTessellationBufferSize);
}

SSPC_SYSV_ABI u64 GetTheTessellationFactorRingBufferBaseAddress() {
    u64 ring = g_tess_factor_ring.load(std::memory_order_relaxed);
    if (ring == 0) {
        GuestMemory* const memory = GetGuestMemory();
        if (memory == nullptr) {
            SSPC_LOG(GPU, Error, "sceGnmGetTheTessellationFactorRingBufferBaseAddress: the guest memory is not "
                                 "installed");
            return 0;
        }
        ring = memory->SystemReservedEnd() - kTessFactorRingBelowReservedEnd;
        g_tess_factor_ring.store(ring, std::memory_order_relaxed);
    }
    return ring;
}

SSPC_SYSV_ABI s32 LogicalCuMaskToPhysicalCuMask(s64 /*unused*/, s32 logical_cu_mask) {
    SSPC_LOG(GPU, Debug, "sceGnmLogicalCuMaskToPhysicalCuMask: logical CU mask {:#x} (kept)",
             static_cast<u32>(logical_cu_mask));
    return logical_cu_mask;
}

// --- the bridge's stubs (nothing done, the same result)

SSPC_SYSV_ABI s32 DebugHardwareStatus() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmDebugHardwareStatus", kOk);
}

SSPC_SYSV_ABI s32 DestroyWorkloadStream() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmDestroyWorkloadStream", kOk);
}

SSPC_SYSV_ABI s32 DisableMipStatsReport() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmDisableMipStatsReport", kOk);
}

SSPC_SYSV_ABI s32 DrawOpaqueAuto() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmDrawOpaqueAuto", kOk);
}

SSPC_SYSV_ABI void FlushGarlic() {
    static std::atomic<bool> once;
    NotImplemented(once, "sceGnmFlushGarlic", kOk); // host memory is coherent; nothing to flush
}

SSPC_SYSV_ABI s32 GetEqTimeStamp() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetEqTimeStamp", kOk);
}

SSPC_SYSV_ABI s32 GetNumTcaUnits() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetNumTcaUnits", kOk);
}

SSPC_SYSV_ABI s32 InsertSetColorMarker() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmInsertSetColorMarker", kOk);
}

SSPC_SYSV_ABI s32 RequestFlipAndSubmitDone() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmRequestFlipAndSubmitDone", kOk);
}

SSPC_SYSV_ABI s32 RequestFlipAndSubmitDoneForWorkload() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmRequestFlipAndSubmitDoneForWorkload", kOk);
}

SSPC_SYSV_ABI s32 RequestMipStatsReportAndReset() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmRequestMipStatsReportAndReset", kOk);
}

SSPC_SYSV_ABI s32 SetGsRingSizes() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmSetGsRingSizes", kOk);
}

SSPC_SYSV_ABI s32 SetWaveLimitMultipliers() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmSetWaveLimitMultipliers", kOk);
}

SSPC_SYSV_ABI s32 SetupMipStatsReport() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmSetupMipStatsReport", kOk);
}

// --- not available in retail firmware (the bridge's answers; those that answer 0 = OK warn: nothing is done)

SSPC_SYSV_ABI s32 GetGpuBlockStatus() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetGpuBlockStatus", kOk);
}

SSPC_SYSV_ABI s32 GetGpuInfoStatus() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetGpuInfoStatus", kOk);
}

SSPC_SYSV_ABI s32 GetLastWaitedAddress() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetLastWaitedAddress", kOk);
}

SSPC_SYSV_ABI s32 GetShaderProgramBaseAddress() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetShaderProgramBaseAddress", kOk);
}

SSPC_SYSV_ABI s32 GetShaderStatus() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmGetShaderStatus", kOk);
}

SSPC_SYSV_ABI s32 InsertThreadTraceMarker() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmInsertThreadTraceMarker", kOk);
}

SSPC_SYSV_ABI s32 LogicalCuIndexToPhysicalCuIndex() {
    static std::atomic<bool> once;
    return NotImplemented(once, "sceGnmLogicalCuIndexToPhysicalCuIndex", kOk);
}

SSPC_SYSV_ABI bool DriverCaptureInProgress() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceGnmDriverCaptureInProgress", false);
}

SSPC_SYSV_ABI bool DriverTraceInProgress() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceGnmDriverTraceInProgress", false);
}

SSPC_SYSV_ABI s32 DriverTriggerCapture() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceGnmDriverTriggerCapture", kErrCaptureRazorNotLoaded);
}

SSPC_SYSV_ABI u32 GetProtectionFaultTimeStamp() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceGnmGetProtectionFaultTimeStamp", 0u);
}

SSPC_SYSV_ABI bool IsUserPaEnabled() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceGnmIsUserPaEnabled", false);
}

SSPC_SYSV_ABI s32 RazorCaptureImmediate() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceRazorCaptureImmediate", kErrCaptureFailedInternal);
}

SSPC_SYSV_ABI bool RazorIsLoaded() {
    static std::atomic<bool> once;
    return RetailAnswer(once, "sceRazorIsLoaded", false);
}

// ------------------------------------------------------------------------------ the export table

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

/// One import key of the game: library and module libSceGnmDriver, group "gnm", experimental.
Runtime::Export Entry(const char* nid, const char* name, void* function) {
    return {nid, "libSceGnmDriver", "libSceGnmDriver", name, function, nullptr, "gnm", false, true};
}

// The game's import keys (src/loader/generated/import_table.inc), in its order.
const Runtime::Export kExports[] = {
    Entry("b0xyllnVY-I", "sceGnmAddEqEvent", Fn(&AddEqEvent)),
    Entry("b08AgtPlHPg", "sceGnmAreSubmitsAllowed", Fn(&AreSubmitsAllowed)),
    Entry("ihxrbsoSKWc", "sceGnmBeginWorkload", Fn(&BeginWorkload)),
    Entry("5udAm+6boVg", "sceGnmCreateWorkloadStream", Fn(&CreateWorkloadStream)),
    Entry("qpGITzPE+Zc", "sceGnmDebugHardwareStatus", Fn(&DebugHardwareStatus)),
    Entry("PVT+fuoS9gU", "sceGnmDeleteEqEvent", Fn(&DeleteEqEvent)),
    Entry("UtObDRQiGbs", "sceGnmDestroyWorkloadStream", Fn(&DestroyWorkloadStream)),
    Entry("bX5IbRvECXk", "sceGnmDingDong", Fn(&DingDong)),
    Entry("byXlqupd8cE", "sceGnmDingDongForWorkload", Fn(&DingDongForWorkload)),
    Entry("HHo1BAljZO8", "sceGnmDisableMipStatsReport", Fn(&DisableMipStatsReport)),
    Entry("0BzLGljcwBo", "sceGnmDispatchDirect", Fn(&DispatchDirect)),
    Entry("Z43vKp5k7r0", "sceGnmDispatchIndirect", Fn(&DispatchIndirect)),
    Entry("wED4ZXCFJT0", "sceGnmDispatchIndirectOnMec", Fn(&DispatchIndirectOnMec)),
    Entry("nF6bFRUBRAU", "sceGnmDispatchInitDefaultHardwareState", Fn(&DispatchInitDefaultHardwareState)),
    Entry("HlTPoZ-oY7Y", "sceGnmDrawIndex", Fn(&DrawIndex)),
    Entry("GGsn7jMTxw4", "sceGnmDrawIndexAuto", Fn(&DrawIndexAuto)),
    Entry("ED9-Fjr8Ta4", "sceGnmDrawIndexIndirect", Fn(&DrawIndexIndirect)),
    Entry("thbPcG7E7qk", "sceGnmDrawIndexIndirectCountMulti", Fn(&DrawIndexIndirectCountMulti)),
    Entry("5q95ravnueg", "sceGnmDrawIndexIndirectMulti", Fn(&DrawIndexIndirectMulti)),
    Entry("jHdPvIzlpKc", "sceGnmDrawIndexMultiInstanced", Fn(&DrawIndexMultiInstanced)),
    Entry("oYM+YzfCm2Y", "sceGnmDrawIndexOffset", Fn(&DrawIndexOffset)),
    Entry("4v+otIIdjqg", "sceGnmDrawIndirect", Fn(&DrawIndirect)),
    Entry("cUCo8OvArrw", "sceGnmDrawIndirectCountMulti", Fn(&DrawIndirectCountMulti)),
    Entry("f5QQLp9rzGk", "sceGnmDrawIndirectMulti", Fn(&DrawIndirectMulti)),
    Entry("yb2cRhagD1I", "sceGnmDrawInitDefaultHardwareState350", Fn(&DrawInitDefaultHardwareState350)),
    Entry("im2ZuItabu4", "sceGnmDrawInitToDefaultContextState400", Fn(&DrawInitToDefaultContextState400)),
    Entry("stDSYW2SBVM", "sceGnmDrawOpaqueAuto", Fn(&DrawOpaqueAuto)),
    Entry("TLV4mswiZ4A", "sceGnmDriverCaptureInProgress", Fn(&DriverCaptureInProgress)),
    Entry("R6z1xM3pW-w", "sceGnmDriverTraceInProgress", Fn(&DriverTraceInProgress)),
    Entry("d88anrgNoKY", "sceGnmDriverTriggerCapture", Fn(&DriverTriggerCapture)),
    Entry("Fa3x75OOLRA", "sceGnmEndWorkload", Fn(&EndWorkload)),
    Entry("iBt3Oe00Kvc", "sceGnmFlushGarlic", Fn(&FlushGarlic)),
    Entry("UoYY0DWMC0U", "sceGnmGetEqEventType", Fn(&GetEqEventType)),
    Entry("H7-fgvEutM0", "sceGnmGetEqTimeStamp", Fn(&GetEqTimeStamp)),
    Entry("oL4hGI1PMpw", "sceGnmGetGpuBlockStatus", Fn(&GetGpuBlockStatus)),
    Entry("tZCSL5ulnB4", "sceGnmGetGpuInfoStatus", Fn(&GetGpuInfoStatus)),
    Entry("iFirFzgYsvw", "sceGnmGetLastWaitedAddress", Fn(&GetLastWaitedAddress)),
    Entry("KnldROUkWJY", "sceGnmGetNumTcaUnits", Fn(&GetNumTcaUnits)),
    Entry("FFVZcCu3zWU", "sceGnmGetOffChipTessellationBufferSize", Fn(&GetOffChipTessellationBufferSize)),
    Entry("fzJdEihTFV4", "sceGnmGetProtectionFaultTimeStamp", Fn(&GetProtectionFaultTimeStamp)),
    Entry("nEyFbYUloIM", "sceGnmGetShaderProgramBaseAddress", Fn(&GetShaderProgramBaseAddress)),
    Entry("k7iGTvDQPLQ", "sceGnmGetShaderStatus", Fn(&GetShaderStatus)),
    Entry("ln33zjBrfjk", "sceGnmGetTheTessellationFactorRingBufferBaseAddress",
          Fn(&GetTheTessellationFactorRingBufferBaseAddress)),
    Entry("NfvOrNzy6sk", "sceGnmInsertDingDongMarker", Fn(&InsertDingDongMarker)),
    Entry("7qZVNgEu+SY", "sceGnmInsertPopMarker", Fn(&InsertPopMarker)),
    Entry("aPIZJTXC+cU", "sceGnmInsertPushColorMarker", Fn(&InsertPushColorMarker)),
    Entry("W1Etj-jlW7Y", "sceGnmInsertPushMarker", Fn(&InsertPushMarker)),
    Entry("aj3L-iaFmyk", "sceGnmInsertSetColorMarker", Fn(&InsertSetColorMarker)),
    Entry("jiItzS6+22g", "sceGnmInsertSetMarker", Fn(&InsertSetMarker)),
    Entry("URDgJcXhQOs", "sceGnmInsertThreadTraceMarker", Fn(&InsertThreadTraceMarker)),
    Entry("1qXLHIpROPE", "sceGnmInsertWaitFlipDone", Fn(&InsertWaitFlipDone)),
    Entry("jg33rEKLfVs", "sceGnmIsUserPaEnabled", Fn(&IsUserPaEnabled)),
    Entry("26PM5Mzl8zc", "sceGnmLogicalCuIndexToPhysicalCuIndex", Fn(&LogicalCuIndexToPhysicalCuIndex)),
    Entry("RU74kek-N0c", "sceGnmLogicalCuMaskToPhysicalCuMask", Fn(&LogicalCuMaskToPhysicalCuMask)),
    Entry("29oKvKXzEZo", "sceGnmMapComputeQueue", Fn(&MapComputeQueue)),
    Entry("A+uGq+3KFtQ", "sceGnmMapComputeQueueWithPriority", Fn(&MapComputeQueueWithPriority)),
    Entry("gObODli-OH8", "sceGnmRequestFlipAndSubmitDone", Fn(&RequestFlipAndSubmitDone)),
    Entry("6YRHhh5mHCs", "sceGnmRequestFlipAndSubmitDoneForWorkload", Fn(&RequestFlipAndSubmitDoneForWorkload)),
    Entry("f85orjx7qts", "sceGnmRequestMipStatsReportAndReset", Fn(&RequestMipStatsReportAndReset)),
    Entry("MYRtYhojKdA", "sceGnmResetVgtControl", Fn(&ResetVgtControl)),
    Entry("Kx-h-nWQJ8A", "sceGnmSetCsShaderWithModifier", Fn(&SetCsShaderWithModifier)),
    Entry("X9Omw9dwv5M", "sceGnmSetEmbeddedPsShader", Fn(&SetEmbeddedPsShader)),
    Entry("+AFvOEXrKJk", "sceGnmSetEmbeddedVsShader", Fn(&SetEmbeddedVsShader)),
    Entry("FUHG8sQ3R58", "sceGnmSetEsShader", Fn(&SetEsShader)),
    Entry("jtkqXpAOY6w", "sceGnmSetGsRingSizes", Fn(&SetGsRingSizes)),
    Entry("UJwNuMBcUAk", "sceGnmSetGsShader", Fn(&SetGsShader)),
    Entry("VJNjFtqiF5w", "sceGnmSetHsShader", Fn(&SetHsShader)),
    Entry("vckdzbQ46SI", "sceGnmSetLsShader", Fn(&SetLsShader)),
    Entry("5uFKckiJYRM", "sceGnmSetPsShader350", Fn(&SetPsShader350)),
    Entry("cFCp0NX8wf0", "sceGnmSetVgtControl", Fn(&SetVgtControl)),
    Entry("gAhCn6UiU4Y", "sceGnmSetVsShader", Fn(&SetVsShader)),
    Entry("XiyzNZ9J4nQ", "sceGnmSetWaveLimitMultipliers", Fn(&SetWaveLimitMultipliers)),
    Entry("+xuDhxlWRPg", "sceGnmSetupMipStatsReport", Fn(&SetupMipStatsReport)),
    Entry("xbxNatawohc", "sceGnmSubmitAndFlipCommandBuffers", Fn(&SubmitAndFlipCommandBuffers)),
    Entry("Ga6r7H6Y0RI", "sceGnmSubmitAndFlipCommandBuffersForWorkload", Fn(&SubmitAndFlipCommandBuffersForWorkload)),
    Entry("zwY0YV91TTI", "sceGnmSubmitCommandBuffers", Fn(&SubmitCommandBuffers)),
    Entry("jRcI8VcgTz4", "sceGnmSubmitCommandBuffersForWorkload", Fn(&SubmitCommandBuffersForWorkload)),
    Entry("yvZ73uQUqrk", "sceGnmSubmitDone", Fn(&SubmitDone)),
    Entry("ArSg-TGinhk", "sceGnmUnmapComputeQueue", Fn(&UnmapComputeQueue)),
    Entry("nLM2i2+65hA", "sceGnmUpdateGsShader", Fn(&UpdateGsShader)),
    Entry("GNlx+y7xPdE", "sceGnmUpdateHsShader", Fn(&UpdateHsShader)),
    Entry("mLVL7N7BVBg", "sceGnmUpdatePsShader350", Fn(&UpdatePsShader350)),
    Entry("V31V01UiScY", "sceGnmUpdateVsShader", Fn(&UpdateVsShader)),
    Entry("u9YKpRRHe-M", "sceRazorCaptureImmediate", Fn(&RazorCaptureImmediate)),
    Entry("f33OrruQYbM", "sceRazorIsLoaded", Fn(&RazorIsLoaded)),
};
static_assert(std::size(kExports) == 85);

} // namespace

// ------------------------------------------------------------------------------ the backend's side (gnm_host.h)

void SetHost(Host* host) {
    Host* const previous = g_host.exchange(host, std::memory_order_acq_rel);
    if (host == nullptr) {
        // A stopping backend raises no GpuIdle any more: a submission waiting for one returns (and finds no host).
        ReleaseSubmissionLock();
        // Every call that took the previous host under one of these locks is done with it once they are free.
        std::scoped_lock barrier{g_submit_mutex, g_queue_mutex, g_label_mutex};
        if (previous != nullptr) {
            SSPC_LOG(GPU, Info, "GnmDriver: the GPU backend's host is removed; GPU work is refused from now on");
        }
        return;
    }
    const std::span<const u32> sequence = InitSequenceFor(g_sdk_version.load(std::memory_order_relaxed));
    SSPC_LOG(GPU, Info, "GnmDriver: SecondSonPC's libSceGnmDriver serves the game (P4.4, EXPERIMENTAL); SDK {:#x}, "
             "initialisation sequence of {} dwords",
             static_cast<u32>(g_sdk_version.load(std::memory_order_relaxed)), sequence.size());
    if (Kernel::NeoMode()) {
        SSPC_LOG(GPU, Error, "GnmDriver: Neo mode (PS4 Pro) is on, but SecondSonPC's driver builds the base PS4's "
                 "packets and initialisation sequences only (the bridge's Neo variants are not ported)");
    }
}

Host* GetHost() {
    return g_host.load(std::memory_order_acquire);
}

void RaiseInterrupt(Interrupt irq) {
    const u32 id = static_cast<u32>(irq);
    if (id > kMaxInterruptId) {
        SSPC_LOG(GPU, Error, "GnmDriver: unexpected interrupt {:#x} (the bridge asserts)", id);
        return;
    }
    if (irq == Interrupt::GpuIdle) {
        ReleaseSubmissionLock();
    }
    if (!IsEventType(id)) {
        return; // GfxFlip and GpuIdle wake no event queue (sceGnmAddEqEvent with those ids registers in vain)
    }
    Queues::Owner* const owner = Queues::GetOwner();
    if (owner == nullptr) {
        return;
    }
    std::scoped_lock lk{g_irq_mutex};
    for (auto it = g_registrations.begin(); it != g_registrations.end();) {
        if (it->id == id && !owner->Trigger(it->queue, id, Queues::Filter::GraphicsCore, id)) {
            it = g_registrations.erase(it); // the queue is gone (deleted without sceGnmDeleteEqEvent)
        } else {
            ++it;
        }
    }
}

void SetSdkVersion(s32 version) {
    g_sdk_version.store(version, std::memory_order_relaxed);
}

void SetEarlyFrameSource(EarlyFrameSource source) {
    g_early_source.store(source, std::memory_order_release);
    if (source != nullptr) {
        g_early_installed.store(true, std::memory_order_release);
    }
}

std::span<const Runtime::Export> GnmExports() {
    return kExports;
}

namespace Test {

void Reset() {
    {
        std::scoped_lock lk{g_submit_mutex};
        g_send_init_packet = true;
        g_early.pending = false;
    }
    g_early_source.store(nullptr, std::memory_order_release);
    g_early_installed.store(false, std::memory_order_release);
    ReleaseSubmissionLock();
    {
        std::scoped_lock lk{g_queue_mutex};
        g_free_slots = InitialFreeSlots();
        g_mapped.reset();
    }
    {
        std::scoped_lock lk{g_irq_mutex};
        g_registrations.clear();
    }
    g_sdk_version.store(0, std::memory_order_relaxed);
}

bool SubmissionLocked() {
    std::scoped_lock lk{g_idle_mutex};
    return g_submission_locked;
}

} // namespace Test

} // namespace SecondSon::Gpu::Gnm
