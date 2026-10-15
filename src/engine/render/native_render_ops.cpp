// SecondSonPC — native render operations (see native_render_ops.h).
#include "engine/render/native_render_ops.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>

#include "engine/guest_hooks.h"

namespace SecondSon::Engine::Render {

namespace {

constexpr u32 kNopHeader = 0xC0001000u; ///< type 3, NOP; count field = body dwords - 1
constexpr u32 kPayload = kDrawMarkerDwords - 1;

enum Bits : u32 { kBitIndexed = 1, kBitShaders = 2, kBitPs = 4, kBitVsTable = 8, kBitPsTable = 16, kBitCanary = 32 };

} // namespace

u32 EncodeDrawMarker(const DrawOp& op, u16 flags, u32* out) {
    u32* p = out;
    *p++ = kNopHeader | ((kPayload - 1) << 16);
    *p++ = kMarkerMagic;
    *p++ = static_cast<u32>(OpKind::Draw) | (u32{flags} << 16);
    *p++ = static_cast<u32>(op.pass);
    *p++ = op.sub;
    *p++ = (op.indexed ? kBitIndexed : 0) | (op.shaders ? kBitShaders : 0) | (op.has_ps ? kBitPs : 0) |
           (op.table[kStageVs].present ? kBitVsTable : 0) | (op.table[kStagePs].present ? kBitPsTable : 0) | (op.canary ? kBitCanary : 0);
    *p++ = op.index_count;
    *p++ = static_cast<u32>(op.index_base);
    *p++ = static_cast<u32>(op.index_base >> 32);
    std::memcpy(p, op.vs, sizeof(op.vs));
    p += kVsRegs;
    std::memcpy(p, op.ps, sizeof(op.ps));
    p += kPsRegs;
    for (const StageTable& t : op.table) {
        *p++ = t.dwords;
        *p++ = static_cast<u32>(t.hash);
        *p++ = static_cast<u32>(t.hash >> 32);
        *p++ = t.refs;
        for (u32 i = 0; i < 7; ++i) {
            *p++ = i < t.refs ? (u32{t.ref[i].sgpr} | (u32{t.ref[i].offset} << 8)) : 0;
        }
    }
    return static_cast<u32>(p - out);
}

bool IsMarker(std::span<const u32> packet) {
    return packet.size() >= 3 && (packet[0] >> 30) == 3 && ((packet[0] >> 8) & 0xFF) == 0x10 &&
           packet[1] == kMarkerMagic;
}

bool DecodeDrawMarker(std::span<const u32> packet, DrawOp& op, u16& flags) {
    if (!IsMarker(packet) || packet.size() != kDrawMarkerDwords ||
        (packet[2] & 0xFFFF) != static_cast<u32>(OpKind::Draw)) {
        return false;
    }
    const u32* p = packet.data() + 2;
    flags = static_cast<u16>(*p++ >> 16);
    op.pass = static_cast<s32>(*p++);
    op.sub = *p++;
    const u32 bits = *p++;
    op.indexed = (bits & kBitIndexed) != 0;
    op.shaders = (bits & kBitShaders) != 0;
    op.has_ps = (bits & kBitPs) != 0;
    op.canary = (bits & kBitCanary) != 0;
    op.index_count = *p++;
    op.index_base = u64{p[0]} | (u64{p[1]} << 32);
    p += 2;
    std::memcpy(op.vs, p, sizeof(op.vs));
    p += kVsRegs;
    std::memcpy(op.ps, p, sizeof(op.ps));
    p += kPsRegs;
    for (u32 s = 0; s < kDrawStages; ++s) {
        StageTable& t = op.table[s];
        t.present = (bits & (s == kStageVs ? kBitVsTable : kBitPsTable)) != 0;
        t.dwords = *p++;
        t.hash = u64{p[0]} | (u64{p[1]} << 32);
        p += 2;
        t.refs = *p++;
        if (t.refs > 7 || t.dwords > 0x600) {
            return false;
        }
        for (u32 i = 0; i < 7; ++i) {
            const u32 v = *p++;
            t.ref[i] = TableRef{static_cast<u8>(v & 0xFF), static_cast<u16>(v >> 8)};
            if (i < t.refs && t.ref[i].sgpr > 14) {
                return false;
            }
        }
    }
    return true;
}

u32 EncodeRegistersMarker(std::span<const RegWrite> writes, u16 flags, u32* out, OpKind kind) {
    const u32 cap = kind == OpKind::State ? kMaxStateWrites : kMaxRegWrites;
    const u32 n = static_cast<u32>(std::min<size_t>(writes.size(), cap));
    const u32 total = RegistersMarkerDwords(n);
    u32* p = out;
    *p++ = kNopHeader | ((total - 2) << 16);
    *p++ = kMarkerMagic;
    *p++ = static_cast<u32>(kind) | (u32{flags} << 16);
    *p++ = n;
    for (u32 i = 0; i < n; ++i) {
        *p++ = writes[i].reg;
        *p++ = writes[i].value;
    }
    return total;
}

bool DecodeRegistersMarker(std::span<const u32> packet, RegWrite* out, u32& count, u16& flags, OpKind kind,
                           u32 capacity) {
    if (!IsMarker(packet) || packet.size() < 4 || MarkerKind(packet) != kind) {
        return false;
    }
    flags = static_cast<u16>(packet[2] >> 16);
    count = packet[3];
    if (count > capacity || packet.size() != RegistersMarkerDwords(count)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out[i] = RegWrite{packet[4 + 2 * i], packet[5 + 2 * i]};
    }
    return true;
}

namespace {

/// The table the user data point at: every reference must agree on the snapshot's start; its content must hash to
/// the engine's shadow.
bool TableMatches(const StageTable& t, const u32* user_data, HashGuestFn hash_guest) {
    u64 base = 0;
    for (u32 i = 0; i < t.refs; ++i) {
        const u32 sgpr = t.ref[i].sgpr;
        const u64 pointer = u64{user_data[sgpr]} | (u64{user_data[sgpr + 1] & 0xFFFF} << 32);
        const u64 start = pointer - u64{t.ref[i].offset} * 4;
        if (i == 0) {
            base = start;
        } else if (start != base) {
            return false;
        }
    }
    if (t.refs == 0 || t.dwords == 0) {
        return true; // nothing reaches the shader through a table
    }
    u64 hash = 0;
    return hash_guest(base, u64{t.dwords} * 4, hash) && hash == t.hash;
}

} // namespace

u32 VerifyDraw(const DrawOp& op, const DrawView& view, HashGuestFn hash_guest) {
    u32 m = 0;
    if (op.indexed != view.indexed) {
        m |= kMismatchKind;
    }
    if (op.index_count != view.index_count) {
        m |= kMismatchIndexCount;
    }
    if (op.indexed && op.index_base != view.index_base) {
        m |= kMismatchIndexBase;
    }
    if (op.shaders) {
        if (std::memcmp(op.vs, view.vs, sizeof(op.vs)) != 0) {
            m |= kMismatchVsRegs;
        }
        if (op.has_ps) {
            if (std::memcmp(op.ps, view.ps, sizeof(op.ps)) != 0) {
                m |= kMismatchPsRegs;
            }
        } else if (view.ps[0] != 0 || view.ps[1] != 0 || view.ps[10] != 0 || view.ps[11] != 0xF) {
            m |= kMismatchPsRegs; // no pixel shader: PGM 0, DB_SHADER_CONTROL 0, CB_SHADER_MASK 0xF
        }
    }
    for (u32 s = 0; s < kDrawStages; ++s) {
        const StageTable& t = op.table[s];
        if (t.present && view.user_data[s] != nullptr && !TableMatches(t, view.user_data[s], hash_guest)) {
            m |= s == kStageVs ? kMismatchVsTable : kMismatchPsTable;
        }
    }
    return m;
}

bool HashGuestMemory(u64 address, u64 bytes, u64& hash) {
    // One readable region remembered per thread: the snapshots of a frame sit in one arena.
    thread_local u64 known_start = 0;
    thread_local u64 known_end = 0;
    if (address == 0 || bytes > (1u << 20) || address + bytes < address) {
        return false;
    }
    u64 at = address;
    while (at < address + bytes) {
        if (at >= known_start && at < known_end) {
            at = known_end;
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            return false;
        }
        known_start = reinterpret_cast<u64>(mbi.BaseAddress);
        known_end = known_start + mbi.RegionSize;
        at = known_end;
    }
    hash = HashBytes(reinterpret_cast<const void*>(address), bytes);
    return true;
}

u32 PassSlot(s32 pass) {
    if (pass >= 0 && pass <= 40) {
        return static_cast<u32>(pass);
    }
    switch (pass) {
    case -2:
        return 41;
    case -3:
        return 42;
    case -4:
        return 43;
    default:
        return 44;
    }
}

DrawCounters& Counters() {
    static DrawCounters counters;
    return counters;
}

NativeControl& Control() {
    static NativeControl control;
    return control;
}

// --- P5.3 direct path ---------------------------------------------------------------------------------------------

namespace {

static_assert(sizeof(TableShape) == 24, "TableShape is compared and hashed as its bytes");

/// The interned shapes: entries written once under the lock, then found without it (a slot holds an entry's id).
struct ShapeTable {
    static constexpr u32 kSlots = kMaxShapes * 2; ///< at most half full: a probe always meets an empty slot
    std::mutex insert;
    TableShape entries[kMaxShapes];
    std::atomic<u32> count{1}; ///< id 0: no table
    std::atomic<u32> slots[kSlots] = {};
};

ShapeTable& Shapes() {
    static ShapeTable* table = new ShapeTable(); // (never freed: the processor may still read it at exit)
    return *table;
}

u32 ShapeSlot(const TableShape& s) {
    u64 w[3];
    std::memcpy(w, &s, sizeof(w));
    u64 h = w[0] * 0x9E3779B97F4A7C15ull;
    h = (h ^ w[1]) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ w[2]) * 0x94D049BB133111EBull;
    return static_cast<u32>(h ^ (h >> 31)) & (ShapeTable::kSlots - 1);
}

std::atomic<bool> g_direct{false};

constexpr u64 kBlockSealed = 1ull << 31; ///< block state: runs written (bits 0..30), sealed, runs done (bits 32..)
constexpr u32 kRunTaken = 1u << 31;      ///< run state: its words | taken by the processor

struct Stream {
    std::unique_ptr<u32[]> words{new u32[size_t{kStreamBlocks} * kStreamBlockWords]};
    std::atomic<u64> state[kStreamBlocks] = {};
    std::atomic<u32> generation[kStreamBlocks] = {};
    std::mutex free_mutex;
    std::vector<u16> free;
    Stream() {
        free.reserve(kStreamBlocks);
        for (u32 b = kStreamBlocks; b-- > 0;) {
            free.push_back(static_cast<u16>(b));
        }
    }
    u32* Block(u32 b) {
        return words.get() + size_t{b} * kStreamBlockWords;
    }
};

Stream& TheStream() {
    static Stream* stream = new Stream(); // (never freed, as the shapes)
    return *stream;
}

s32 AllocBlock(u32& generation) {
    Stream& s = TheStream();
    u16 b = 0;
    {
        std::scoped_lock lock(s.free_mutex);
        if (s.free.empty()) {
            return -1;
        }
        b = s.free.back();
        s.free.pop_back();
    }
    s.state[b].store(0, std::memory_order_relaxed);
    generation = s.generation[b].fetch_add(1, std::memory_order_acq_rel) + 1;
    Direct().blocks_in_use.fetch_add(1, std::memory_order_relaxed);
    return b;
}

void FreeBlock(u32 b) {
    Stream& s = TheStream();
    Direct().blocks_in_use.fetch_sub(1, std::memory_order_relaxed);
    std::scoped_lock lock(s.free_mutex);
    s.free.push_back(static_cast<u16>(b));
}

/// The engine left the block: free once the processor is done with every run in it.
void SealBlock(u32 b) {
    const u64 old = TheStream().state[b].fetch_or(kBlockSealed, std::memory_order_acq_rel);
    if ((old >> 32) == (old & (kBlockSealed - 1))) {
        FreeBlock(b);
    }
}

} // namespace

bool InternShape(const TableShape& shape, u32& id) {
    ShapeTable& t = Shapes();
    constexpr u32 mask = ShapeTable::kSlots - 1;
    const u32 first = ShapeSlot(shape);
    for (u32 i = first;; i = (i + 1) & mask) {
        const u32 v = t.slots[i].load(std::memory_order_acquire);
        if (v == 0) {
            break;
        }
        if (std::memcmp(&t.entries[v], &shape, sizeof(shape)) == 0) {
            id = v;
            return true;
        }
    }
    std::scoped_lock lock(t.insert);
    u32 i = first;
    for (;; i = (i + 1) & mask) { // again: another thread may have added it meanwhile
        const u32 v = t.slots[i].load(std::memory_order_relaxed);
        if (v == 0) {
            break;
        }
        if (std::memcmp(&t.entries[v], &shape, sizeof(shape)) == 0) {
            id = v;
            return true;
        }
    }
    const u32 n = t.count.load(std::memory_order_relaxed);
    if (n >= kMaxShapes) {
        Direct().shapes_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    t.entries[n] = shape;
    t.count.store(n + 1, std::memory_order_release);
    t.slots[i].store(n, std::memory_order_release);
    id = n;
    return true;
}

const TableShape* ShapeOf(u32 id) {
    ShapeTable& t = Shapes();
    return id == 0 || id >= t.count.load(std::memory_order_acquire) ? nullptr : &t.entries[id];
}

void SetDirect(bool on) {
    g_direct.store(on, std::memory_order_release);
}

bool DirectOn() {
    return g_direct.load(std::memory_order_relaxed);
}

u32* StreamReserve(StreamCursor& c, u64 cmd_ptr, u32 max_words) {
    Stream& s = TheStream();
    c.append = c.token_end != 0 && cmd_ptr == c.token_end && c.block >= 0 && c.used + 2 + max_words <= kStreamBlockWords;
    if (c.append) {
        return s.Block(static_cast<u32>(c.block)) + c.used;
    }
    if (c.block < 0 || c.used + 2 + max_words > kStreamBlockWords) {
        c.token_end = 0; // (the open run stays in the block left)
        if (c.block >= 0) {
            SealBlock(static_cast<u32>(c.block));
            c.block = -1;
        }
        const s32 b = AllocBlock(c.generation);
        if (b < 0) {
            Direct().stream_full.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        c.block = b;
        c.used = 0;
    }
    return s.Block(static_cast<u32>(c.block)) + c.used + 2;
}

bool StreamCommit(StreamCursor& c, u32 words, u32* token) {
    Stream& s = TheStream();
    u32* base = s.Block(static_cast<u32>(c.block));
    if (c.append) {
        u32 expect = c.run_words;
        if (std::atomic_ref<u32>(base[c.run_at])
                .compare_exchange_strong(expect, c.run_words + words, std::memory_order_release,
                                         std::memory_order_relaxed)) {
            c.used += words;
            c.run_words += words;
            return true;
        }
        // the processor took the run already (its DCB submitted while still recorded): this operation starts one here
        std::memmove(base + c.used + 2, base + c.used, size_t{words} * 4);
    }
    c.run_at = c.used;
    base[c.run_at + 1] = c.generation;
    s.state[c.block].fetch_add(1, std::memory_order_release); // a run written (before any token names it)
    std::atomic_ref<u32>(base[c.run_at]).store(words, std::memory_order_release);
    c.used = c.run_at + 2 + words;
    c.run_words = words;
    c.token_end = 0;
    token[0] = kStreamTokenHeader;
    token[1] = kStreamMagic;
    token[2] = static_cast<u32>(c.block) | (c.run_at << 16);
    token[3] = c.generation;
    return false;
}

bool StreamTake(std::span<const u32> token, StreamRun& run) {
    if (token.size() != kStreamTokenDwords || token[1] != kStreamMagic) {
        return false;
    }
    const u32 block = token[2] & 0xFFFF;
    const u32 at = token[2] >> 16;
    const u32 generation = token[3];
    if (block >= kStreamBlocks || at + 2 > kStreamBlockWords) {
        return false;
    }
    Stream& s = TheStream();
    if (s.generation[block].load(std::memory_order_acquire) != generation) {
        return false; // the block was given out again: a stale token (left untouched)
    }
    u32* w = s.Block(block) + at;
    const u32 st = std::atomic_ref<u32>(w[0]).fetch_or(kRunTaken, std::memory_order_acq_rel);
    if ((st & kRunTaken) != 0 || w[1] != generation || at + 2 + st > kStreamBlockWords) {
        return false;
    }
    run = StreamRun{w + 2, st, block};
    return true;
}

void StreamDone(const StreamRun& run) {
    const u64 old = TheStream().state[run.block].fetch_add(1ull << 32, std::memory_order_acq_rel);
    if ((old & kBlockSealed) != 0 && (old >> 32) + 1 == (old & (kBlockSealed - 1))) {
        FreeBlock(run.block);
    }
}

DirectCounters& Direct() {
    static DirectCounters counters;
    return counters;
}

} // namespace SecondSon::Engine::Render
