// SecondSonPC native runtime - versioned save files with integrity checks.
//
// File: "SSPCSAVE" | u32 format version | u32 section count | u32 payload size | u32 CRC-32 of the
// payload | payload = sections {u32 tag, u32 size, bytes}. Little-endian. Written atomically
// (file::WriteAtomic). A file with a wrong magic, version, size or checksum is rejected as a whole;
// the game state is only changed after every section decoded successfully.
#pragma once

#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace sspc {

inline constexpr u32 kSaveFormatVersion = 1;

constexpr u32 SaveTag(const char (&s)[5]) {
    return static_cast<u32>(s[0]) | static_cast<u32>(s[1]) << 8 | static_cast<u32>(s[2]) << 16 | static_cast<u32>(s[3]) << 24;
}

class ByteWriter {
public:
    template <class T>
    void Put(const T& v) {
        const size_t at = data_.size();
        data_.resize(at + sizeof(T));
        std::memcpy(data_.data() + at, &v, sizeof(T));
    }
    void PutString(std::string_view s) {
        Put(static_cast<u32>(s.size()));
        data_.insert(data_.end(), s.begin(), s.end());
    }
    void PutBytes(std::span<const u8> b) {
        Put(static_cast<u32>(b.size()));
        PutRaw(b);
    }
    void PutRaw(std::span<const u8> b) { data_.insert(data_.end(), b.begin(), b.end()); }
    const std::vector<u8>& Data() const { return data_; }
    std::vector<u8> Take() { return std::move(data_); }

private:
    std::vector<u8> data_;
};

// Bounds-checked reader: once a read fails every later read fails too (check Ok() at the end).
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data) : data_(data) {}
    template <class T>
    bool Get(T& v) {
        if (!ok_ || data_.size() - pos_ < sizeof(T)) return ok_ = false;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool GetString(std::string& s, u32 max_len = 4096);
    bool GetBytes(std::vector<u8>& b, u32 max_len);
    bool Ok() const { return ok_; }
    bool AtEnd() const { return pos_ == data_.size(); }
    void Fail() { ok_ = false; }

private:
    std::span<const u8> data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

using SaveSections = std::map<u32, std::vector<u8>>;

std::vector<u8> EncodeSave(const SaveSections& sections);
// Returns false (and leaves `out` empty) on any structural or checksum error.
bool DecodeSave(std::span<const u8> file, SaveSections& out, std::string* error);
bool WriteSave(const std::wstring& path, const SaveSections& sections);
bool ReadSave(const std::wstring& path, SaveSections& out, std::string* error);

}  // namespace sspc
