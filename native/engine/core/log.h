// SecondSonPC native runtime - logging.
//
// Thread-safe line logger: every line goes to the log file (if opened), to the debugger and
// optionally to stderr. Messages are formatted with std::format.
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace sspc::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Opens (truncates) the log file. Safe to call once at start-up; logging before Init only goes
// to the debugger/stderr.
bool Init(const std::wstring& path, Level min_level, bool echo_stderr);
void Shutdown();
void SetLevel(Level min_level);

void Write(Level level, std::string_view category, std::string_view message);
// Writes without taking the logger lock or allocating (crash handler path).
void EmergencyWrite(const char* text);

template <class... Args>
void Debug(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Debug, cat, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Info(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Info, cat, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Warn(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Warn, cat, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Error(std::string_view cat, std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Error, cat, std::format(fmt, std::forward<Args>(args)...));
}

// Wide-string helpers for paths in messages.
std::string Narrow(std::wstring_view w);
std::wstring Widen(std::string_view s);

}  // namespace sspc::log
