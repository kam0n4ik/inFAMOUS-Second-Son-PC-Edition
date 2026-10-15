// SecondSonPC — native render operations: the engine's draw as SecondSonPC reconstructs it, carried to the command
// processor IN the game's command buffer (P5.3B/C; docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md).
//
// An operation travels as a PM4 NOP whose payload starts with kMarkerMagic, written into the context's DCB at the place
// the game's own draw packet goes. The command buffers keep their order, are submitted, chained and replayed as before,
// and the processor meets the operation exactly where the game's draw is - so the order of state changes, draws,
// barriers and fences stays the game's by construction. What the processor does with it:
//   kVerify (shadow, P5.3B)  remembers the operation; the next draw packet of the queue is compared with it (the draw
//                            the PM4 path built: its parameters, the shader registers it sees, the resource tables its
//                            user data point at). Counted as a match or a mismatch per field; nothing is executed twice.
//   kApply  (P5.3C)          the operation replaces the game's PM4 for its draw (see NATIVE_RENDER_INTERFACE.md §4).
//
// The engine side (native_draw.cpp) builds DrawOp from the engine's own objects: the constant update engine's shader
// pointers and stage resource shadows, the user-data layout tables, the draw wrapper's arguments. No PM4 is read there.
#pragma once

#include <atomic>
#include <span>

#include "base/abi.h"

namespace SecondSon::Engine::Render {

inline constexpr u32 kMarkerMagic = 0x43505353u; ///< "SSPC" (little endian) - no driver NOP payload starts with it

enum class OpKind : u16 {
    Draw = 1,      ///< DrawOp: verified (kVerify) and/or executed (kApply) by the processor
    Registers = 2, ///< kApply: register writes of the engine's CUE commit done natively (RegWrite list)
    State = 3,     ///< P5.3D: render state of SetRenderTargets / TransitionTo / primitive and index type (RegWrite
                   ///< list with the kState* pseudo registers): kApply writes it, kVerify compares it with the
                   ///< state the game's PM4 left (the marker follows the game's packets)
};

enum OpFlags : u16 {
    kVerify = 1,
    kApply = 2,
    /// P5.3 (draw markers): the engine's description of the game's draw packet that follows - nothing verified or
    /// replaced; the processor hands the stage tables on with the draw (DrawDescriptor::engine_*) for the Native
    /// Render Frontend. kApply and kVerify markers describe their draws as well.
    kDescribe = 4,
};

/// Registers sceGnmSetVsShader / sceGnmSetPsShader350 write from the shader object's register block (gnm_driver.cpp
/// SetVsRegs/SetPsRegs), in the block's order: the values a draw sees for its VS and PS.
inline constexpr u32 kVsRegs = 7;  ///< PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL
inline constexpr u32 kPsRegs = 12; ///< PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_SHADER_Z_FORMAT, SPI_SHADER_COL_FORMAT,
                                   ///< SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, SPI_PS_IN_CONTROL, SPI_BARYC_CNTL,
                                   ///< DB_SHADER_CONTROL, CB_SHADER_MASK
inline constexpr u32 kVsRegIndex[kVsRegs] = {0x2C48, 0x2C49, 0x2C4A, 0x2C4B, 0xA1B1, 0xA1C3, 0xA207};
inline constexpr u32 kPsRegIndex[kPsRegs] = {0x2C08, 0x2C09, 0x2C0A, 0x2C0B, 0xA1C4, 0xA1C5,
                                             0xA1B3, 0xA1B4, 0xA1B6, 0xA1B8, 0xA203, 0xA08F};

/// A user-data SGPR pair that holds a pointer into the stage's resource snapshot (the copy of the CUE's stage shadow the
/// commit 0x14ad0 makes), `offset` dwords past its start.
struct TableRef {
    u8 sgpr = 0;
    u16 offset = 0;
};

/// A stage's resource table as the engine holds it: the shadow's first `dwords` dwords and their FNV-1a 64.
struct StageTable {
    bool present = false;
    u32 dwords = 0;
    u64 hash = 0;
    u32 refs = 0;
    TableRef ref[7];
};

enum DrawStage : u32 { kStageVs = 0, kStagePs = 1, kDrawStages = 2 };

struct DrawOp {
    s32 pass = -1; ///< 0..40; -2 pre, -3 post, -4 deformation, -1 other context
    u32 sub = 0;
    bool canary = false;   ///< spoiled on purpose (index count + 1): the processor must report it
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;    ///< indexed: the index buffer's address
    bool shaders = false;  ///< vs/ps below are known (the commit's ordinary shader path)
    bool has_ps = false;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    StageTable table[kDrawStages];
};

/// Dwords EncodeDrawMarker writes (header included).
inline constexpr u32 kDrawMarkerDwords = 1 + 27 + 2 * 11;

/// Writes the NOP: header, {magic, kind | flags << 16, the operation}. Returns kDrawMarkerDwords.
u32 EncodeDrawMarker(const DrawOp& op, u16 flags, u32* out);
/// True when `packet` (a NOP, header included) carries a SecondSonPC operation.
bool IsMarker(std::span<const u32> packet);
/// The marker's draw operation; false if it is not a well-formed one (the caller counts it).
bool DecodeDrawMarker(std::span<const u32> packet, DrawOp& out, u16& flags);

/// One register write of a native commit: hardware register index (0x2C0C.. user data, 0xA191.. PS input, ...).
struct RegWrite {
    u32 reg = 0;
    u32 value = 0;
};
inline constexpr u32 kMaxRegWrites = 128;
inline constexpr u32 kMaxStateWrites = 192; ///< SetRenderTargets with 8 colour targets, depth, viewport: < 170
constexpr u32 RegistersMarkerDwords(u32 writes) {
    return 1 + 3 + 2 * writes; ///< header, magic, kind | flags, count, pairs
}
/// Writes the NOP for `writes` (at most kMaxRegWrites; kMaxStateWrites for OpKind::State). Returns
/// RegistersMarkerDwords(n).
u32 EncodeRegistersMarker(std::span<const RegWrite> writes, u16 flags, u32* out, OpKind kind = OpKind::Registers);
/// The marker's writes into `out` (room for `capacity`); false if it is not a well-formed one of `kind`.
bool DecodeRegistersMarker(std::span<const u32> packet, RegWrite* out, u32& count, u16& flags,
                           OpKind kind = OpKind::Registers, u32 capacity = kMaxRegWrites);

/// State pseudo registers: what the driver's one-dword NOP after a colour / depth target setup tells the processor
/// (the target's real extent, CommandProcessor::ExtentHint), given by the operation instead.
inline constexpr u32 kStateCbExtent0 = 0xFFFF0000u; ///< + colour target 0..7
inline constexpr u32 kStateDbExtent = 0xFFFF0008u;
inline constexpr bool IsStatePseudo(u32 reg) {
    return reg >= kStateCbExtent0 && reg <= kStateDbExtent;
}
/// The operation kind of a marker (IsMarker true).
inline OpKind MarkerKind(std::span<const u32> packet) {
    return static_cast<OpKind>(packet[2] & 0xFFFF);
}

/// What the command processor built for the draw that follows the marker.
struct DrawView {
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    const u32* user_data[kDrawStages] = {}; ///< 16 dwords each (USER_DATA_VS_0.., USER_DATA_PS_0..)
};

enum DrawMismatch : u32 {
    kMismatchKind = 1u << 0,       ///< indexed vs auto
    kMismatchIndexCount = 1u << 1,
    kMismatchIndexBase = 1u << 2,
    kMismatchVsRegs = 1u << 3,
    kMismatchPsRegs = 1u << 4,
    kMismatchVsTable = 1u << 5,    ///< the VS user data do not point at a copy of the engine's VS table
    kMismatchPsTable = 1u << 6,
    kMismatchFields = 7,
};

/// FNV-1a 64 of `bytes` guest bytes at `address`; false if they cannot be read.
using HashGuestFn = bool (*)(u64 address, u64 bytes, u64& hash);

/// The fields of `view` that differ from `op` (DrawMismatch bits; 0 = the PM4 path drew exactly the engine's draw).
u32 VerifyDraw(const DrawOp& op, const DrawView& view, HashGuestFn hash_guest);

/// Guest memory as the processor's process sees it (the eboot's memory is the process's); readable ranges cached.
bool HashGuestMemory(u64 address, u64 bytes, u64& hash);

/// What the command processor did with the markers (all queues, since start).
inline constexpr u32 kPassSlots = 48; ///< 0..40 passes, 41 pre, 42 post, 43 deformation, 44 other
u32 PassSlot(s32 pass);
struct DrawCounters {
    std::atomic<u64> markers{0};
    std::atomic<u64> malformed{0};
    std::atomic<u64> orphaned{0};  ///< a marker not followed by a draw before the next marker / end of buffer
    std::atomic<u64> verified{0};
    std::atomic<u64> matched{0};
    std::atomic<u64> mismatched{0};
    std::atomic<u64> field[kMismatchFields] = {};
    std::atomic<u64> shaders_unknown{0};
    std::atomic<u64> tables_checked{0}; ///< stage tables compared by content (references and dwords present)
    std::atomic<u64> canaries{0};       ///< markers the engine side spoiled on purpose (SSPC_P53_NATIVE_DRAW_CANARY)
    std::atomic<u64> canaries_caught{0};
    std::atomic<u64> applied_draws{0};     ///< kApply draws executed from their operation (no draw packet)
    std::atomic<u64> applied_registers{0}; ///< register writes of native commits (no SET_*_REG packet)
    std::atomic<u64> applied_commits{0};
    std::atomic<u64> described{0};          ///< P5.3: kDescribe-only markers (the game's draw packet followed)
    std::atomic<u64> described_orphaned{0}; ///< ... a description that met no draw before the next marker
    std::atomic<u64> state_applied{0};       ///< P5.3D: kApply state operations written (no PM4 of the game)
    std::atomic<u64> state_applied_writes{0};
    std::atomic<u64> state_verified{0};      ///< kVerify state operations compared with the PM4 path's state
    std::atomic<u64> state_matched{0};
    std::atomic<u64> state_mismatched{0};
    std::atomic<u64> state_values_checked{0};
    std::atomic<u32> state_first_bad_reg{0}; ///< the first mismatching register (0: none) and its two values
    std::atomic<u32> state_first_bad_want{0};
    std::atomic<u32> state_first_bad_have{0};
    std::atomic<u64> pass_verified[kPassSlots] = {};
    std::atomic<u64> pass_mismatched[kPassSlots] = {};
};
DrawCounters& Counters();

/// P5.3D A/B comparison (SSPC_P53_NATIVE_AB=<frames>): the processor counts the flips it meets; the engine runs its
/// native operations (apply mode) only in every other block of that many flips - the game's PM4 path in the first
/// block, the native path in the second, and so on. 0 (default): native whenever apply mode is on.
struct NativeControl {
    std::atomic<u64> flips{0};
    u32 ab_frames = 0;
};
NativeControl& Control();
inline bool NativeActive() {
    const NativeControl& c = Control();
    return c.ab_frames == 0 || ((c.flips.load(std::memory_order_relaxed) / c.ab_frames) & 1) != 0;
}

// --- P5.3 direct path (SSPC_P53_DIRECT=1): the operations without PM4 markers ----------------------------------------
//
// The markers above carry every operation inside the game's DCB as a NOP the processor decodes: one packet more per
// described draw (kDrawMarkerDwords), one per commit, state change and native draw. The direct path keeps what the DCB
// is for - the one ordered stream: where an operation sits in it is where it runs relative to the game's packets, the
// barriers and labels among them and the other contexts' buffers (in the game's submission order) - but serializes no
// operation into it:
//   * a draw's description rides in the NOP the driver already writes after the game's draw packet (sceGnmDrawIndex /
//     DrawIndexAuto: the draw, then a 4-dword NOP whose body the hardware ignores and whose first dword the driver
//     zeroes): kDescribeMagic, the pass and the ids of the VS/PS table shapes (InternShape, once per distinct layout).
//     No packet and no dword more; the processor reads it when it takes the draw packet;
//   * the operations of a native pass (commit registers, render state, draws) are written ready to run into the native
//     stream (blocks of SecondSonPC's memory, one run of operations after another per context); the DCB gets one
//     4-dword token per run - operations with no game packet between them share one - and the processor runs the run
//     where it meets the token. No GPU flush and no CPU wait: the token's place is the order.
// Verification (kVerify, SSPC_P53_NATIVE_VERIFY=1) keeps its markers: they are a check, not the hot path.

/// A stage table's shape as the engine's layout gives it (StageTable without the content hash): where the user-data
/// pointers sit and how large the table is.
struct TableShape {
    u16 dwords = 0;
    u8 refs = 0;
    u8 sgpr[7] = {};
    u16 offset[7] = {};
};
inline constexpr u32 kMaxShapes = 1u << 16;
/// The id of `shape` (the same shape, the same id: 1..kMaxShapes - 1). False when the table is full (counted): the
/// draw goes undescribed. A shape without a table (refs or dwords 0) is id 0 and needs no call.
bool InternShape(const TableShape& shape, u32& id);
/// The shape of `id`; nullptr for 0 and for an id never given.
const TableShape* ShapeOf(u32 id);

inline constexpr u32 kDescribeMagic = 0x44505353u; ///< "SSPD": the first body dword of the draw's trailing NOP
inline constexpr u32 kDrawNopHeader = 0xC0021000u; ///< that NOP: type 3, 3 body dwords
/// The NOP's body: {kDescribeMagic, (pass + 64) & 0xFF | VS shape id << 8, PS shape id}.
inline constexpr u32 DescribeWord(s32 pass, u32 vs_shape) {
    return (static_cast<u32>(pass + 64) & 0xFF) | (vs_shape << 8);
}
inline constexpr s32 DescribedPass(u32 word) {
    return static_cast<s32>(word & 0xFF) - 64;
}

/// Set (before the guest runs) when the engine side runs the direct path: the processor looks for descriptions in
/// draws' trailing NOPs only then.
void SetDirect(bool on);
bool DirectOn();

// The native stream. A run in a block: {state: its operations' words | kRunTaken, the block's generation}, then the
// operations, each {kind | payload words << 8, payload}. Registers / State: {reg, value} pairs (State with the kState*
// pseudo registers); Draw: pass, bits (1: indexed), index count, index base lo, hi, VS shape id, PS shape id.
enum class StreamOp : u8 { Registers = 1, State = 2, Draw = 3 };
inline constexpr u32 OpHeader(StreamOp kind, u32 payload_words) {
    return static_cast<u32>(kind) | (payload_words << 8);
}
inline constexpr u32 kStreamDrawWords = 7;
inline constexpr u32 kStreamMagic = 0x53505353u;    ///< "SSPS"
inline constexpr u32 kStreamTokenDwords = 4;        ///< {NOP header (3 body dwords), magic, block | run << 16, generation}
inline constexpr u32 kStreamTokenHeader = 0xC0021000u;
inline constexpr u32 kStreamBlockWords = 4096;      ///< 16 KiB: a run never crosses a block
inline constexpr u32 kStreamBlocks = 2048;          ///< 32 MiB in all
inline constexpr u32 kStreamMaxOpWords = 1 + 2 * kMaxStateWrites;

/// The engine side's place in the stream, one per context (its writer is the thread that records the context).
struct StreamCursor {
    s32 block = -1;
    u32 generation = 0;
    u32 used = 0;      ///< words of the block written
    u32 run_at = 0;    ///< the open run's first word
    u32 run_words = 0; ///< its operations' words
    u64 token_end = 0; ///< the DCB address right after the open run's token; 0: no run to append to
    bool append = false; ///< (StreamReserve -> StreamCommit)
};
/// Room for one operation of at most `max_words` (header included). `cmd_ptr`: the DCB's write pointer now (room for
/// a token there already made). Appends to the open run when the DCB has not moved since its token, else starts a
/// run (in a new block when this one is full). Null: no block free - the caller runs the game's path, nothing written.
u32* StreamReserve(StreamCursor& c, u64 cmd_ptr, u32 max_words);
/// The operation (`words` written at StreamReserve's pointer) published. True: it joined the open run (the DCB gets
/// nothing). False: it starts a run - `token` holds the kStreamTokenDwords the DCB must carry at cmd_ptr; tell the
/// cursor where they end (StreamTokenWritten).
bool StreamCommit(StreamCursor& c, u32 words, u32* token);
inline void StreamTokenWritten(StreamCursor& c, u64 token_end) {
    c.token_end = token_end;
}

/// Processor side: the run a token names, taken (no operation joins it afterwards). False: not a well-formed token, or
/// a stale one (its block reused, the run taken before) - counted by the caller.
struct StreamRun {
    const u32* words = nullptr;
    u32 count = 0;
    u32 block = 0;
};
bool StreamTake(std::span<const u32> token, StreamRun& run);
/// After the processor ran it: its block may be reused once every run in it is done and the engine left it.
void StreamDone(const StreamRun& run);

struct DirectCounters {
    std::atomic<u64> described{0};       ///< processor: draws whose trailing NOP carried a description
    std::atomic<u64> described_bad{0};   ///< ... with a shape id never given
    std::atomic<u64> runs{0};            ///< processor: run tokens met
    std::atomic<u64> runs_stale{0};      ///< ... naming no run (a reused block, a run taken twice) or malformed
    std::atomic<u64> ops{0};             ///< operations run
    std::atomic<u64> draws{0};
    std::atomic<u64> register_writes{0};
    std::atomic<u64> state_writes{0};
    std::atomic<u64> native_tsc{0};      ///< processor time in native operations (TSC, every 8th call timed, x8; the
                                         ///< executor's part of their draws not in it) - markers and the direct path
    std::atomic<u64> native_calls{0};
    std::atomic<u64> shapes_full{0};     ///< engine: InternShape refused (the table full)
    std::atomic<u64> stream_full{0};     ///< engine: no block free - the game's path ran
    std::atomic<u64> blocks_in_use{0};
};
DirectCounters& Direct();

} // namespace SecondSon::Engine::Render
