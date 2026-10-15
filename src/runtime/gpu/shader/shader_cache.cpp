// SecondSonPC — the shader frontend's caches (P4.3). See shader_cache.h.
#include <windows.h>

#include "runtime/gpu/shader/shader_cache.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace SecondSon::Gpu::Shader {

Layout* Program::FindLayout(const RuntimeParams& runtime) {
    for (Layout& l : layouts) {
        if (std::memcmp(&l.runtime, &runtime, sizeof(RuntimeParams)) == 0) {
            return &l;
        }
    }
    return nullptr;
}

Module* Program::FindModule(const SpecializationKey& key, u64& collisions) {
    for (auto& m : modules) {
        if (!m->keyed || m->key.hash != key.hash) {
            continue;
        }
        if (m->key.words == key.words) {
            return m.get();
        }
        ++collisions; // the same 64-bit hash, another specialization: never taken for it
    }
    return nullptr;
}

bool LookupMemoOn() {
    static const bool on = [] {
        char v[4] = {};
        return !(GetEnvironmentVariableA("SSPC_P49_LOOKUP_MEMO", v, sizeof(v)) == 1 && v[0] == '0');
    }();
    return on;
}

bool LookupMemoVerify() {
    static const bool verify = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("SSPC_P49_LOOKUP_MEMO", v, sizeof(v)) == 6 && std::string_view(v) == "verify";
    }();
    return verify;
}

LookupMemoChecks& LookupMemoCounts() {
    static LookupMemoChecks counts;
    return counts;
}

ProgramCache::Lookup ProgramCache::Get(GuestReader& memory, u64 addr, HwStage stage) {
    ++stats_.lookups;
    Lookup out;
    const u64 key = (addr << 3) | static_cast<u64>(stage);
    AddressEntry* entry = nullptr;
    auto& slot = addr_memo_[LookupMemoSlot(key)];
    if (slot.second != nullptr && slot.first == key) {
        entry = slot.second;
        if (LookupMemoVerify()) {
            const auto it = addresses_.find(key);
            ++LookupMemoCounts().verified;
            LookupMemoCounts().mismatches += it == addresses_.end() || &it->second != entry ? 1 : 0;
        }
    } else if (const auto it = addresses_.find(key); it != addresses_.end()) {
        entry = &it->second;
        if (LookupMemoOn()) {
            slot = {key, entry};
        }
    }
    // P5.2: its code pages write-tracked before the read that validated it, and not written since
    const bool tracked_clean = entry != nullptr && entry->tracked && !entry->dirty;
    if (tracked_clean && trust_ == CodeTrust::On) {
        ++stats_.trusted_tracked;
        out.program = entry->program;
        out.footer_hash = entry->footer_hash;
        return out;
    }
    if (entry != nullptr && !entry->dirty && validation_ == Validation::PerFrame && entry->epoch == epoch_) {
        ++stats_.trusted;
        out.program = entry->program;
        out.footer_hash = entry->footer_hash;
        return out;
    }
    // read and compare (the first use of the address, or its revalidation); P5.2: tracked first, so a write after
    // the read is caught (a new address is tracked from its next validation, once its length is known)
    const bool armed = trust_ != CodeTrust::Off && entry != nullptr &&
                       track_(track_ctx_, addr, u64{entry->length} + kFooterBytes + kFooterSearchBytes);
    ++stats_.reads;
    out.read = true;
    const ProgramError e = ReadProgram(memory, addr, stage, scratch_, scan_);
    if (e != ProgramError::None) {
        ++stats_.errors[static_cast<size_t>(e)];
        out.error = e;
        if (entry != nullptr) {
            entry->dirty = true; // not trusted until it reads again
        }
        return out;
    }
    stats_.bytes_read += scratch_.code.size() * 4 + kFooterBytes;
    if (entry != nullptr) {
        Program* p = entry->program;
        if (p->code.size() == scratch_.code.size() &&
            std::memcmp(p->code.data(), scratch_.code.data(), p->code.size() * 4) == 0) {
            ++stats_.same;
            entry->epoch = epoch_;
            entry->dirty = false;
            entry->tracked = armed;
            entry->footer_hash = scratch_.footer.shader_hash;
            out.program = p;
            out.footer_hash = scratch_.footer.shader_hash;
            return out;
        }
        ++stats_.changed;
        if (tracked_clean) {
            ++stats_.trust_mismatches; // (verify) the code changed without a tracked write
        }
        out.changed = true;
    }
    const ContentHash hash = test_hash_ != nullptr ? test_hash_(scratch_.code) : scratch_.hash;
    const ProgramIdentity id{hash, static_cast<u32>(scratch_.code.size() * 4), stage};
    auto pit = programs_.find(id);
    Program* program = nullptr;
    if (pit != programs_.end() && pit->second->code == scratch_.code) {
        program = pit->second.get();
    } else if (pit != programs_.end()) {
        // a 128-bit hash collision (never seen): refuse rather than merge two programs
        ++stats_.errors[static_cast<size_t>(ProgramError::Collision)];
        out.error = ProgramError::Collision;
        return out;
    } else {
        auto p = std::make_unique<Program>();
        p->index = next_index_++;
        p->id = id;
        p->footer_hash = scratch_.footer.shader_hash;
        p->first_address = addr;
        p->code = scratch_.code;
        code_bytes_ += p->code.size() * 4;
        program = p.get();
        programs_.emplace(id, std::move(p));
        ++stats_.new_programs;
        out.created = true;
    }
    ++program->addresses;
    addresses_[key] = AddressEntry{program, epoch_, scratch_.footer.shader_hash, id.length, false, armed};
    addr_lo_ = std::min(addr_lo_, addr);
    addr_hi_ = std::max(addr_hi_, addr + id.length + kFooterBytes + kFooterSearchBytes);
    out.program = program;
    out.footer_hash = scratch_.footer.shader_hash;
    return out;
}

void ProgramCache::Invalidate(u64 addr, u64 size) {
    const u64 end = addr + size;
    if (end <= addr_lo_ || addr >= addr_hi_) {
        return; // no program's code or footer there (the walk over every entry was ~1% of the GPU thread)
    }
    for (auto& [key, entry] : addresses_) {
        const u64 start = key >> 3;
        // the program's code and its footer
        if (start < end && addr < start + entry.length + kFooterBytes + kFooterSearchBytes) {
            if (!entry.dirty) {
                ++stats_.invalidations;
            }
            entry.dirty = true;
        }
    }
}

std::vector<Program*> ProgramCache::All() const {
    std::vector<Program*> out;
    out.reserve(programs_.size());
    for (const auto& [id, p] : programs_) {
        out.push_back(p.get());
    }
    std::sort(out.begin(), out.end(), [](const Program* a, const Program* b) { return a->index < b->index; });
    return out;
}

const FetchCache::Entry& FetchCache::Get(GuestReader& memory, u64 addr, u32 sgpr_base, u64 epoch,
                                         Validation validation) {
    const u64 key = (addr << 8) ^ sgpr_base;
    auto& slot = memo_[LookupMemoSlot(key)];
    if (slot.second == nullptr || slot.first != key) {
        Entry* found = &entries_[key];
        if (!LookupMemoOn()) {
            return GetEntry(memory, addr, sgpr_base, epoch, validation, *found);
        }
        slot = {key, found};
    } else if (LookupMemoVerify()) {
        const auto it = entries_.find(key);
        ++LookupMemoCounts().verified;
        LookupMemoCounts().mismatches += it == entries_.end() || &it->second != slot.second ? 1 : 0;
    }
    return GetEntry(memory, addr, sgpr_base, epoch, validation, *slot.second);
}

const FetchCache::Entry& FetchCache::GetEntry(GuestReader& memory, u64 addr, u32 sgpr_base, u64 epoch,
                                              Validation validation, Entry& e) {
    // P5.2: tracked before the read that validated it, not written since (see ProgramCache::Get)
    const bool tracked_clean = e.epoch != 0 && e.tracked && !e.dirty;
    if (tracked_clean && trust_ == CodeTrust::On) {
        ++trusted_tracked;
        return e;
    }
    if (e.epoch == epoch && validation == Validation::PerFrame && e.epoch != 0 && !e.dirty) {
        return e;
    }
    const bool armed = trust_ != CodeTrust::Off && e.epoch != 0 && track_(track_ctx_, addr, kMaxFetchDwords * 4ull);
    ++reads;
    // read up to the fetch shader's s_setpc_b64 (64 dwords at a time, at most kMaxFetchDwords)
    scratch_.clear();
    std::string error;
    FetchShader parsed;
    bool ok = false;
    for (u32 have = 0; have < kMaxFetchDwords;) {
        const u32 chunk = std::min<u32>(64, kMaxFetchDwords - have);
        scratch_.resize(have + chunk);
        if (addr == 0 || addr + (have + chunk) * 4ull > (1ull << 40) ||
            !memory.Read(addr + have * 4ull, scratch_.data() + have, chunk * 4ull)) {
            error = fmt::format("the fetch shader at {:#x} is unreadable", addr);
            break;
        }
        have += chunk;
        if (ParseFetchShaderCode(scratch_, sgpr_base, parsed, error)) {
            ok = true;
            break;
        }
        if (error.rfind("no s_setpc_b64", 0) != 0 && error.rfind("a buffer load cut", 0) != 0) {
            break; // malformed, not merely cut by the read
        }
    }
    if (ok) {
        scratch_.resize(parsed.size / 4);
        if (e.ok && e.code != scratch_) {
            ++changed;
            if (tracked_clean) {
                ++trust_mismatches; // (verify) the code changed without a tracked write
            }
        }
        if (!e.ok || e.code != scratch_) {
            ++e.version;
        }
        e.code = scratch_;
        e.fetch = std::move(parsed);
        e.error.clear();
    } else {
        e.version += e.ok ? 1 : 0;
        e.fetch = {};
        e.code.clear();
        e.error = std::move(error);
    }
    e.ok = ok;
    e.epoch = epoch;
    e.addr = addr;
    e.tracked = armed;
    e.dirty = false;
    lo_ = std::min(lo_, addr);
    hi_ = std::max(hi_, addr + kMaxFetchDwords * 4ull);
    return e;
}

void FetchCache::Invalidate(u64 addr, u64 size) {
    const u64 end = addr + size;
    if (end <= lo_ || addr >= hi_) {
        return;
    }
    for (auto& [key, e] : entries_) {
        if (e.epoch != 0 && e.addr < end && addr < e.addr + kMaxFetchDwords * 4ull) {
            invalidations += e.dirty ? 0 : 1;
            e.dirty = true;
        }
    }
}

} // namespace SecondSon::Gpu::Shader
