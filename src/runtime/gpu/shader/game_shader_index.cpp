// SecondSonPC — the game's GCN programs found in its files (P4.14). See game_shader_index.h.
#include <windows.h>

#include "runtime/gpu/shader/game_shader_index.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include <fmt/format.h>
#include <xxhash.h>

namespace SecondSon::Gpu::Shader {

namespace {

constexpr char kMagic[8] = {'S', 'S', 'P', 'C', 'G', 'S', 'I', '1'};
constexpr u32 kVersion = 1;

struct Header {
    char magic[8];
    u32 version;
    u32 packs, programs, members;
    u64 payload_bytes;
    u64 payload_hash;
};
static_assert(sizeof(Header) == 40);

bool IsPack(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<u8>(c))); });
    return ext == ".xpps";
}

} // namespace

bool GameShaderIndex::Scan(const std::filesystem::path& game_dir, ScanStats& stats, std::string& error,
                           void (*progress)(void*, u64, u64), void* ctx) {
    const auto t0 = std::chrono::steady_clock::now();
    stats = {};
    packs_.clear();
    programs_.clear();
    members_.clear();
    by_content_.clear();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             game_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && IsPack(it->path())) {
            files.push_back(it->path());
        }
    }
    if (files.empty()) {
        error = fmt::format("no .xpps pack under {}", game_dir.string());
        return false;
    }
    std::ranges::sort(files);
    std::vector<std::vector<GameProgramLocation>> where;
    std::vector<u8> buf;
    std::vector<u32> code;
    for (size_t fi = 0; fi < files.size(); ++fi) {
        std::ifstream in(files[fi], std::ios::binary | std::ios::ate);
        if (!in) {
            continue;
        }
        const auto size = static_cast<size_t>(in.tellg());
        buf.resize(size);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
        if (!in) {
            continue;
        }
        ++stats.files;
        stats.bytes += size;
        const u32 pack = static_cast<u32>(packs_.size());
        packs_.push_back(std::filesystem::relative(files[fi], game_dir, ec).generic_string());
        const u8* d = buf.data();
        for (size_t p = 0; p + kFooterBytes <= size;) {
            const void* hit = std::memchr(d + p, 'O', size - kFooterBytes + 1 - p);
            if (hit == nullptr) {
                break;
            }
            p = static_cast<size_t>(static_cast<const u8*>(hit) - d);
            ProgramFooter f;
            if (!ParseFooter(d + p, f)) {
                ++p;
                continue;
            }
            ++stats.footers;
            // the program's start: its first instruction points at the footer (s_mov_b32 vcc_hi, offset / 8 - 1)
            size_t start = SIZE_MAX;
            if (f.length != 0 && f.length % 4 == 0 && f.length <= kMaxProgramBytes) {
                for (size_t off = f.length; off < size_t{f.length} + 256 && off <= p; off += 4) {
                    u32 w[2];
                    std::memcpy(w, d + p - off, 8);
                    if (w[0] == kTokenMovVcchi && (u64{w[1]} + 1) * 8 == off) {
                        start = p - off;
                        break;
                    }
                }
            }
            if (start == SIZE_MAX) {
                ++stats.unresolved;
                p += 7;
                continue;
            }
            code.resize(f.length / 4);
            std::memcpy(code.data(), d + start, f.length);
            const ContentHash h = HashCode(code);
            const auto [it, inserted] = by_content_.try_emplace(Key{h.lo, h.hi, f.length}, static_cast<u32>(programs_.size()));
            if (inserted) {
                programs_.push_back({h, f.length, f.type, f.shader_hash, 0, 0});
                where.emplace_back();
            }
            where[it->second].push_back({it->second, pack, start});
            p += kFooterBytes;
        }
        if (progress != nullptr) {
            progress(ctx, fi + 1, files.size());
        }
    }
    for (size_t i = 0; i < programs_.size(); ++i) {
        programs_[i].first_member = static_cast<u32>(members_.size());
        programs_[i].members = static_cast<u32>(where[i].size());
        members_.insert(members_.end(), where[i].begin(), where[i].end());
    }
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool GameShaderIndex::Save(const std::filesystem::path& file, std::string& error) const {
    std::vector<u8> payload;
    const auto put = [&payload](const void* p, size_t n) {
        const auto* c = static_cast<const u8*>(p);
        payload.insert(payload.end(), c, c + n);
    };
    for (const std::string& s : packs_) {
        const u32 n = static_cast<u32>(s.size());
        put(&n, 4);
        put(s.data(), n);
    }
    for (const GameProgram& p : programs_) {
        put(&p.hash.lo, 8);
        put(&p.hash.hi, 8);
        put(&p.length, 4);
        const u32 type = p.footer_type;
        put(&type, 4);
        put(&p.footer_hash, 8);
        put(&p.first_member, 4);
        put(&p.members, 4);
    }
    for (const GameProgramLocation& m : members_) {
        put(&m.program, 4);
        put(&m.pack, 4);
        put(&m.offset, 8);
    }
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.packs = static_cast<u32>(packs_.size());
    h.programs = static_cast<u32>(programs_.size());
    h.members = static_cast<u32>(members_.size());
    h.payload_bytes = payload.size();
    h.payload_hash = XXH3_64bits(payload.data(), payload.size());
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!out) {
            error = fmt::format("cannot write {}", tmp.string());
            return false;
        }
    }
    if (!MoveFileExW(tmp.wstring().c_str(), file.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = fmt::format("cannot replace {} ({})", file.string(), GetLastError());
        return false;
    }
    return true;
}

bool GameShaderIndex::Load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "no index (SecondSonPC.exe --scan-game-shaders makes it)";
        return false;
    }
    Header h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0 || h.version != kVersion ||
        h.payload_bytes > (1ull << 30)) {
        error = "not an index of this version";
        return false;
    }
    std::vector<u8> payload(h.payload_bytes);
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!in || XXH3_64bits(payload.data(), payload.size()) != h.payload_hash) {
        error = "the index is damaged";
        return false;
    }
    size_t pos = 0;
    bool ok = true;
    const auto get = [&](void* p, size_t n) {
        if (!ok || pos + n > payload.size()) {
            ok = false;
            return;
        }
        std::memcpy(p, payload.data() + pos, n);
        pos += n;
    };
    packs_.assign(h.packs, {});
    for (std::string& s : packs_) {
        u32 n = 0;
        get(&n, 4);
        if (!ok || n > payload.size() - pos) {
            ok = false;
            break;
        }
        s.assign(reinterpret_cast<const char*>(payload.data() + pos), n);
        pos += n;
    }
    programs_.assign(h.programs, {});
    for (GameProgram& p : programs_) {
        u32 type = 0;
        get(&p.hash.lo, 8);
        get(&p.hash.hi, 8);
        get(&p.length, 4);
        get(&type, 4);
        p.footer_type = static_cast<u8>(type);
        get(&p.footer_hash, 8);
        get(&p.first_member, 4);
        get(&p.members, 4);
    }
    members_.assign(h.members, {});
    for (GameProgramLocation& m : members_) {
        get(&m.program, 4);
        get(&m.pack, 4);
        get(&m.offset, 8);
    }
    if (!ok || pos != payload.size()) {
        error = "the index is malformed";
        packs_.clear();
        programs_.clear();
        members_.clear();
        return false;
    }
    Rebuild();
    return true;
}

void GameShaderIndex::Rebuild() {
    by_content_.clear();
    by_content_.reserve(programs_.size());
    for (u32 i = 0; i < programs_.size(); ++i) {
        by_content_.emplace(Key{programs_[i].hash.lo, programs_[i].hash.hi, programs_[i].length}, i);
    }
}

const GameProgram* GameShaderIndex::Find(const ContentHash& hash, u32 length) const {
    const auto it = by_content_.find(Key{hash.lo, hash.hi, length});
    return it != by_content_.end() ? &programs_[it->second] : nullptr;
}

} // namespace SecondSon::Gpu::Shader
