#include "core/log.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>

namespace sspc::log {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
Level g_level = Level::Info;
bool g_stderr = false;
const auto g_start = std::chrono::steady_clock::now();

const char* LevelName(Level l) {
    switch (l) {
        case Level::Debug: return "D";
        case Level::Info: return "I";
        case Level::Warn: return "W";
        case Level::Error: return "E";
    }
    return "?";
}

void RawWrite(const char* data, size_t size) {
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, data, static_cast<DWORD>(size), &written, nullptr);
    }
}

}  // namespace

bool Init(const std::wstring& path, Level min_level, bool echo_stderr) {
    std::lock_guard lock(g_mutex);
    g_level = min_level;
    g_stderr = echo_stderr;
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_file != INVALID_HANDLE_VALUE;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void SetLevel(Level min_level) {
    std::lock_guard lock(g_mutex);
    g_level = min_level;
}

void Write(Level level, std::string_view category, std::string_view message) {
    if (level < g_level) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::string line = std::format("{:10.3f} [{}] [{}] {}\n", t, LevelName(level), category, message);
    std::lock_guard lock(g_mutex);
    RawWrite(line.data(), line.size());
    OutputDebugStringA(line.c_str());
    if (g_stderr || level >= Level::Warn) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
}

void EmergencyWrite(const char* text) {
    // No lock: the crash may have happened while the lock was held.
    RawWrite(text, std::strlen(text));
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    OutputDebugStringA(text);
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace sspc::log
