#include "core/file.h"

#include <windows.h>

#include <algorithm>
#include <iterator>

namespace sspc::file {

bool ReadAll(const std::wstring& path, std::vector<u8>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool WriteAtomic(const std::wstring& path, std::span<const u8> data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 30));
        DWORD written = 0;
        ok = WriteFile(h, data.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool CreateDirectories(const std::wstring& path) {
    if (path.empty() || Exists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0 && !CreateDirectories(path.substr(0, slash))) {
        return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ExecutableDirectory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

MappedFile::~MappedFile() { Close(); }

MappedFile::MappedFile(MappedFile&& o) noexcept
    : file_(o.file_), mapping_(o.mapping_), data_(o.data_), size_(o.size_) {
    o.file_ = o.mapping_ = nullptr;
    o.data_ = nullptr;
    o.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        Close();
        file_ = o.file_;
        mapping_ = o.mapping_;
        data_ = o.data_;
        size_ = o.size_;
        o.file_ = o.mapping_ = nullptr;
        o.data_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

bool MappedFile::Open(const std::wstring& path) {
    Close();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    file_ = h;
    size_ = static_cast<size_t>(size.QuadPart);
    if (size_ == 0) return true;
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) {
        Close();
        return false;
    }
    mapping_ = m;
    data_ = static_cast<const u8*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
    if (!data_) {
        Close();
        return false;
    }
    return true;
}

void MappedFile::Close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
    size_ = 0;
}

}  // namespace sspc::file
