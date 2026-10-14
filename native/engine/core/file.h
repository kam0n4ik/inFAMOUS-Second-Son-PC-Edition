// SecondSonPC native runtime - file I/O.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/types.h"

namespace sspc::file {

bool ReadAll(const std::wstring& path, std::vector<u8>& out);

// Writes to "<path>.tmp", flushes, then atomically replaces <path> (MoveFileEx with
// MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). A crash never leaves a half-written file.
bool WriteAtomic(const std::wstring& path, std::span<const u8> data);

bool Exists(const std::wstring& path);
bool CreateDirectories(const std::wstring& path);
std::wstring ExecutableDirectory();

// Read-only memory mapping of a whole file. Movable, not copyable.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    std::span<const u8> Bytes() const { return {data_, size_}; }
    bool IsOpen() const { return data_ != nullptr || size_ == 0 && file_ != nullptr; }

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const u8* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace sspc::file
