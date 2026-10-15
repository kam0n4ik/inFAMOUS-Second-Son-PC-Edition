#include "core/save_file.h"

#include <format>

#include "core/crc32.h"
#include "core/file.h"

namespace sspc {
namespace {

constexpr char kMagic[8] = {'S', 'S', 'P', 'C', 'S', 'A', 'V', 'E'};
constexpr size_t kHeaderSize = 8 + 4 * 4;
constexpr u32 kMaxSections = 64;
constexpr u32 kMaxPayload = 64u << 20;

}  // namespace

bool ByteReader::GetString(std::string& s, u32 max_len) {
    u32 n = 0;
    if (!Get(n) || n > max_len || data_.size() - pos_ < n) return ok_ = false;
    s.assign(reinterpret_cast<const char*>(data_.data() + pos_), n);
    pos_ += n;
    return true;
}

bool ByteReader::GetBytes(std::vector<u8>& b, u32 max_len) {
    u32 n = 0;
    if (!Get(n) || n > max_len || data_.size() - pos_ < n) return ok_ = false;
    b.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return true;
}

std::vector<u8> EncodeSave(const SaveSections& sections) {
    ByteWriter payload;
    for (const auto& [tag, bytes] : sections) {
        payload.Put(tag);
        payload.Put(static_cast<u32>(bytes.size()));
        payload.PutRaw(bytes);
    }
    const std::vector<u8>& p = payload.Data();
    ByteWriter out;
    for (char c : kMagic) out.Put(c);
    out.Put(kSaveFormatVersion);
    out.Put(static_cast<u32>(sections.size()));
    out.Put(static_cast<u32>(p.size()));
    out.Put(Crc32(p));
    std::vector<u8> file = out.Take();
    file.insert(file.end(), p.begin(), p.end());
    return file;
}

bool DecodeSave(std::span<const u8> file, SaveSections& out, std::string* error) {
    out.clear();
    auto fail = [&](std::string msg) {
        if (error) *error = std::move(msg);
        out.clear();
        return false;
    };
    if (file.size() < kHeaderSize) return fail("file too small");
    if (std::memcmp(file.data(), kMagic, 8) != 0) return fail("bad magic");
    ByteReader h(file.subspan(8, kHeaderSize - 8));
    u32 version = 0, count = 0, size = 0, crc = 0;
    h.Get(version);
    h.Get(count);
    h.Get(size);
    h.Get(crc);
    if (version != kSaveFormatVersion) return fail(std::format("unsupported save version {}", version));
    if (count > kMaxSections || size > kMaxPayload) return fail("section table too large");
    if (file.size() != kHeaderSize + size) return fail(std::format("size mismatch: header says {}, file has {}", size, file.size() - kHeaderSize));
    const auto payload = file.subspan(kHeaderSize, size);
    if (Crc32(payload) != crc) return fail("checksum mismatch (corrupted save)");
    ByteReader r(payload);
    for (u32 i = 0; i < count; ++i) {
        u32 tag = 0;
        std::vector<u8> bytes;
        if (!r.Get(tag) || !r.GetBytes(bytes, kMaxPayload)) return fail("truncated section");
        if (!out.emplace(tag, std::move(bytes)).second) return fail("duplicate section");
    }
    if (!r.AtEnd()) return fail("trailing bytes after the sections");
    return true;
}

bool WriteSave(const std::wstring& path, const SaveSections& sections) {
    const std::vector<u8> bytes = EncodeSave(sections);
    return file::WriteAtomic(path, bytes);
}

bool ReadSave(const std::wstring& path, SaveSections& out, std::string* error) {
    std::vector<u8> bytes;
    if (!file::ReadAll(path, bytes)) {
        if (error) *error = "cannot read file";
        return false;
    }
    return DecodeSave(bytes, out, error);
}

}  // namespace sspc
