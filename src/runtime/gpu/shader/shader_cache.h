// SecondSonPC — the shader frontend's caches: programs by content, their translated modules, fetch shaders
// (Phase 4, P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §5, §6).
//
// Programs: an address maps to the program last read there; a program is its identity (stage, length, 128-bit content
// hash) and owns a copy of its code. An address is trusted until it is revalidated: the first time it is used in an
// epoch (a frame; Validation::PerFrame) or at every use (PerBind), its footer and code are read again and compared
// with the copy — a program rewritten in place becomes another program (the old one stays, by identity, for the day
// it comes back). Invalidate() marks the addresses overlapping a written range for revalidation at their next use (the
// executor's memory listener will call it in P4.4; there is no page tracking of SecondSonPC's own in P4.3).
//
// Modules: a program's translations, one per specialization (SpecializationKey). A key needs the program's layout —
// the resources its reflection lists, which depend on the code and on the runtime parameters — so the first
// translation for a program and runtime is made without a key; its result gives the layout and its own key (from what
// it saw). A module is created Pending by the owner thread, filled by the translator (any thread) and published by a
// release store of its state; the owner reads it after an acquire load.
//
// One owner thread for everything here except Module::result/state (above).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"

namespace SecondSon::Gpu::Shader {

enum class Validation : u8 { PerFrame, PerBind };

/// P5.2 (SSPC_P52_CODE_TRUST): a program's or fetch shader's code read once and its pages write-tracked stays trusted
/// - no read and comparison every frame - until a write to those pages (or their unmapping) invalidates it. Off: the
/// per-frame validation alone; Verify: tracked but read every frame as before, counting what a trusted entry missed.
enum class CodeTrust : u8 { Off, On, Verify };
/// Write-tracks [addr, addr + size) (before the code there is read); false: it cannot be tracked (the entry is then
/// validated every frame as before).
using TrackCodeFn = bool (*)(void* ctx, u64 addr, u64 size);
enum class ModuleState : u8 { Pending, Ready, Failed };

struct Module {
    u32 id = 0;                 ///< from 1, in creation order
    SpecializationKey key;      ///< what the module is specialized on
    bool keyed = false;         ///< the key is known (the first module of a layout: once it is ready)
    std::atomic<ModuleState> state{ModuleState::Pending};
    TranslationResult result;   ///< written by the translator before `state` leaves Pending
    RuntimeParams runtime;      ///< what it was translated for
    BindingStart start;
    std::array<u32, 16> user_data{};
    bool queued = false;        ///< handed to the translator (a full queue leaves it for a later draw)
    // the owner's, once it has seen the module ready
    SrtProgram srt;             ///< the reflection's SRT walker, compiled
    bool prepared = false;
    std::string srt_error;
    u64 uses = 0;
    u64 queued_tick = 0;        ///< when it was queued (TSC), the wait until ready is counted
    // P4.14 (the persistent shader cache, shader_disk_cache.h)
    SpirvHash spirv_hash;       ///< the result's SPIR-V by content (set before `state` leaves Pending)
    u32 disk_record = 0;        ///< Pending: its translation is in the disk cache (record + 1), read instead of made
    bool from_disk = false;     ///< its result was read from the disk cache
};

/// The resources a program reads for one value of its runtime parameters: the first module translated for it.
struct Layout {
    RuntimeParams runtime;
    Module* module = nullptr;
    u32 retries = 0; ///< first translations retried after a failed read of guest memory
};

struct Program {
    u32 index = 0; ///< from 1
    ProgramIdentity id;
    u64 footer_hash = 0;
    u64 first_address = 0;
    std::vector<u32> code;
    std::vector<std::unique_ptr<Module>> modules;
    std::vector<Layout> layouts;
    u32 addresses = 0; ///< addresses that held it

    Layout* FindLayout(const RuntimeParams& runtime);
    /// The module for `key` (hash, then every word; a hash match with other words counts in `collisions`).
    Module* FindModule(const SpecializationKey& key, u64& collisions);
};

struct ProgramCacheStats {
    u64 lookups = 0;
    u64 trusted = 0;          ///< the address was validated in this epoch: no read
    u64 reads = 0;            ///< footer and code read and compared (first use or revalidation)
    u64 same = 0;             ///< ... the code was the program's
    u64 changed = 0;          ///< ... another program now (an address seen before)
    u64 new_programs = 0;
    u64 invalidations = 0;    ///< addresses marked by Invalidate
    u64 bytes_read = 0;
    u64 trusted_tracked = 0;  ///< P5.2: trusted by write tracking (no read; any epoch)
    u64 trust_mismatches = 0; ///< P5.2 verify: a tracked, unwritten entry's code had changed (tracking missed a write)
    std::array<u64, static_cast<size_t>(ProgramError::Count)> errors{};
};

class ProgramCache {
public:
    struct Lookup {
        Program* program = nullptr;
        ProgramError error = ProgramError::None;
        bool read = false;    ///< the code was read now
        bool changed = false; ///< the address holds another program than before
        bool created = false; ///< the program is new to the cache (P4.14: its modules are looked up on disk then)
        u64 footer_hash = 0;  ///< the footer at this address (one program by content may have several footers)
    };
    /// The program at `addr` running as `stage`.
    Lookup Get(GuestReader& memory, u64 addr, HwStage stage);
    void NewEpoch() { ++epoch_; }
    u64 Epoch() const { return epoch_; }
    /// [addr, addr + size) was written: programs there are read again at their next use.
    void Invalidate(u64 addr, u64 size);
    void SetValidation(Validation v) { validation_ = v; }
    Validation GetValidation() const { return validation_; }
    void SetCodeTrust(CodeTrust trust, TrackCodeFn track, void* ctx) {
        trust_ = track != nullptr ? trust : CodeTrust::Off;
        track_ = track;
        track_ctx_ = ctx;
    }

    const ProgramCacheStats& Stats() const { return stats_; }
    size_t Programs() const { return programs_.size(); }
    size_t Addresses() const { return addresses_.size(); }
    u64 CodeBytes() const { return code_bytes_; }
    std::vector<Program*> All() const;

    /// Self-test: a content hash replacing XXH3-128 (to force collisions); nullptr restores it.
    void SetTestHash(ContentHash (*hash)(std::span<const u32> code)) { test_hash_ = hash; }

private:
    struct AddressEntry {
        Program* program = nullptr;
        u64 epoch = 0;
        u64 footer_hash = 0;
        u32 length = 0;
        bool dirty = false;
        bool tracked = false; ///< P5.2: its code and footer were write-tracked before the read that validated them
    };
    CodeTrust trust_ = CodeTrust::Off;
    TrackCodeFn track_ = nullptr;
    void* track_ctx_ = nullptr;
    std::unordered_map<u64, AddressEntry> addresses_; ///< key: address << 3 | stage
    /// P4.9: the entries looked up last (direct-mapped by key) - the map's hashing and bucket walk on every stage of every
    /// draw were ~1.4% of the GPU thread (village); its entries are never erased, so the pointers stay valid
    std::array<std::pair<u64, AddressEntry*>, 256> addr_memo_{};
    u64 addr_lo_ = ~0ull, addr_hi_ = 0; ///< the bytes any entry covers (Invalidate: ranges outside need no walk)
    std::unordered_map<ProgramIdentity, std::unique_ptr<Program>, ProgramIdentityHash> programs_;
    ProgramImage scratch_;
    std::vector<u32> scan_;
    Validation validation_ = Validation::PerFrame;
    u64 epoch_ = 1;
    u64 code_bytes_ = 0;
    u32 next_index_ = 1;
    ProgramCacheStats stats_;
    ContentHash (*test_hash_)(std::span<const u32>) = nullptr;
};

/// Fetch shaders by address (and the user data pair it was found through), validated like programs.
class FetchCache {
public:
    struct Entry {
        bool ok = false;
        std::string error;
        FetchShader fetch;
        std::vector<u32> code;
        u64 epoch = 0;
        u64 addr = 0;         ///< P5.2: where it was read (Invalidate)
        bool tracked = false; ///< P5.2: its bytes were write-tracked before the read that validated them
        bool dirty = false;   ///< P5.2: written since (read again at the next use)
        u64 version = 0;      ///< P5.3: bumped whenever what it holds changes (the native frontend's key)
    };
    const Entry& Get(GuestReader& memory, u64 addr, u32 sgpr_base, u64 epoch, Validation validation);
    size_t Size() const { return entries_.size(); }
    u64 reads = 0, changed = 0;
    u64 trusted_tracked = 0, trust_mismatches = 0, invalidations = 0; ///< P5.2 (see ProgramCacheStats)
    void SetCodeTrust(CodeTrust trust, TrackCodeFn track, void* ctx) {
        trust_ = track != nullptr ? trust : CodeTrust::Off;
        track_ = track;
        track_ctx_ = ctx;
    }
    /// [addr, addr + size) was written: fetch shaders there are read again at their next use.
    void Invalidate(u64 addr, u64 size);

private:
    CodeTrust trust_ = CodeTrust::Off;
    TrackCodeFn track_ = nullptr;
    void* track_ctx_ = nullptr;
    u64 lo_ = ~0ull, hi_ = 0; ///< the bytes any entry covers
    const Entry& GetEntry(GuestReader& memory, u64 addr, u32 sgpr_base, u64 epoch, Validation validation, Entry& e);
    std::unordered_map<u64, Entry> entries_;
    std::array<std::pair<u64, Entry*>, 256> memo_{}; ///< P4.9: entries looked up last (never erased: stable pointers)
    std::vector<u32> scratch_;
};

inline constexpr u32 kMaxFetchDwords = 512;

/// P4.9: the direct-mapped memos in front of the frontend's and the executor's u64-keyed maps (SSPC_P49_LOOKUP_MEMO=0:
/// every lookup through the map, the earlier behaviour)
bool LookupMemoOn();
/// SSPC_P49_LOOKUP_MEMO=verify: every memo hit is looked up in its map too and the entries compared (the counts in
/// gpu_native.txt; the GPU thread's)
bool LookupMemoVerify();
struct LookupMemoChecks {
    // (atomic: P4.10 runs the frontend's and the executor's memos on two threads)
    std::atomic<u64> verified{0}, mismatches{0};
};
LookupMemoChecks& LookupMemoCounts();
inline size_t LookupMemoSlot(u64 key) {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 56); // 256 slots
}

} // namespace SecondSon::Gpu::Shader
