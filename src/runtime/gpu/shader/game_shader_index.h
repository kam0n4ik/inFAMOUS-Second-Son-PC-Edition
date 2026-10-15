// SecondSonPC — the game's GCN programs found in its files (Phase 4, P4.14 offline discovery;
// docs/OFFLINE_SHADER_PRECOMPILATION.md): every program of every .xpps pack (Sucker Punch's KCAP containers) by
// content, the packs it is in and where, read without changing anything. The game draws only programs it loads from
// these packs, so the index is what can ever be met; the persistent shader cache (shader_disk_cache.h) is what was
// met. Their difference is the part of the game no run has prepared yet.
//
// A program in a pack is its code followed by the compiler's 28-byte footer ("OrbShdr"); the code starts with
// s_mov_b32 vcc_hi, <footer offset / 8 - 1> (the token the frontend finds programs by). Its identity is the frontend's:
// XXH3-128 of the code and its length (ProgramIdentity without the stage: the stage is where the game binds it; the
// footer's type is kept as found). The stage registers, the descriptors and the pipeline state a draw gives a program
// are not next to the code (the packs keep them in tables of their own format): the index alone cannot translate a
// program exactly - see the document.
//
// File game_programs.idx (in the cache's <dir>\<game>\native): "SSPCGSI1", version, counts, then the packs' relative
// paths, the programs {hash, length, footer type, footer hash, first location}, the memberships {program, pack,
// offset}; XXH3-64 of everything after the header. Made by `SecondSonPC.exe --scan-game-shaders` (minutes: every pack
// is read once).
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"

namespace SecondSon::Gpu::Shader {

struct GameProgram {
    ContentHash hash;
    u32 length = 0;      ///< code bytes
    u8 footer_type = 0;  ///< the compiler's shader type field (as found)
    u64 footer_hash = 0; ///< the compiler's hash
    u32 first_member = 0, members = 0; ///< its locations in GameShaderIndex::Members()
};

struct GameProgramLocation {
    u32 program = 0;
    u32 pack = 0;
    u64 offset = 0; ///< of the code in the pack
};

class GameShaderIndex {
public:
    struct ScanStats {
        u64 files = 0, bytes = 0, footers = 0, unresolved = 0; ///< unresolved: a footer without its program's start
        double ms = 0.0;
    };
    /// Reads every .xpps under `game_dir` (nothing is written there). `progress(files done, files)` now and then.
    bool Scan(const std::filesystem::path& game_dir, ScanStats& stats, std::string& error,
              void (*progress)(void* ctx, u64 done, u64 total) = nullptr, void* ctx = nullptr);
    bool Save(const std::filesystem::path& file, std::string& error) const;
    bool Load(const std::filesystem::path& file, std::string& error);

    /// The program with this content (nullptr: not in the game's packs).
    const GameProgram* Find(const ContentHash& hash, u32 length) const;
    std::span<const GameProgram> Programs() const { return programs_; }
    std::span<const GameProgramLocation> Members() const { return members_; }
    const std::vector<std::string>& Packs() const { return packs_; }
    bool Empty() const { return programs_.empty(); }

private:
    struct Key {
        u64 lo, hi;
        u32 length;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept { return static_cast<size_t>(k.lo ^ (k.hi * 31) ^ k.length); }
    };
    void Rebuild();

    std::vector<std::string> packs_;
    std::vector<GameProgram> programs_;
    std::vector<GameProgramLocation> members_; ///< by program
    std::unordered_map<Key, u32, KeyHash> by_content_;
};

/// The persistent caches' directory of a game: <[ShaderCache] dir>\<game>\native.
inline std::filesystem::path NativeCacheRoot(const std::filesystem::path& cache_dir, const std::string& game) {
    return cache_dir / game / "native";
}

} // namespace SecondSon::Gpu::Shader
