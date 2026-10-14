// SecondSonPC native runtime - configuration (INI subset).
//
//   ; comment            # comment
//   [Section]
//   Key = value          (keys and sections are case-insensitive; values keep their case)
//
// Parsing never throws; problems are collected as messages with line numbers.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sspc {

class Config {
public:
    // Parses text. Returns false if any line was malformed (valid lines are still kept).
    bool Parse(std::string_view text);
    bool LoadFile(const std::wstring& path);

    std::optional<std::string> Get(std::string_view section, std::string_view key) const;
    std::string GetString(std::string_view section, std::string_view key, std::string_view def) const;
    long long GetInt(std::string_view section, std::string_view key, long long def) const;
    double GetFloat(std::string_view section, std::string_view key, double def) const;
    bool GetBool(std::string_view section, std::string_view key, bool def) const;
    void Set(std::string_view section, std::string_view key, std::string_view value);

    const std::vector<std::string>& Errors() const { return errors_; }

private:
    static std::string Key(std::string_view section, std::string_view key);
    std::map<std::string, std::string> values_;
    std::vector<std::string> errors_;
};

}  // namespace sspc
