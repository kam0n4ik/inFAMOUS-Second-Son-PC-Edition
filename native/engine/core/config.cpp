#include "core/config.h"

#include <cctype>
#include <charconv>
#include <format>

#include "core/file.h"

namespace sspc {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::string Config::Key(std::string_view section, std::string_view key) {
    return Lower(section) + "." + Lower(key);
}

bool Config::Parse(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);  // UTF-8 BOM
    }
    std::string section;
    bool ok = true;
    int line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const size_t eol = text.find('\n');
        std::string_view line = Trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3) {
                errors_.push_back(std::format("line {}: malformed section header", line_no));
                ok = false;
                continue;
            }
            section = std::string(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || Trim(line.substr(0, eq)).empty()) {
            errors_.push_back(std::format("line {}: expected key = value", line_no));
            ok = false;
            continue;
        }
        if (section.empty()) {
            errors_.push_back(std::format("line {}: key outside of a section", line_no));
            ok = false;
            continue;
        }
        std::string_view value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values_[Key(section, Trim(line.substr(0, eq)))] = std::string(value);
    }
    return ok;
}

bool Config::LoadFile(const std::wstring& path) {
    std::vector<u8> bytes;
    if (!file::ReadAll(path, bytes)) {
        errors_.push_back("cannot read config file");
        return false;
    }
    return Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::optional<std::string> Config::Get(std::string_view section, std::string_view key) const {
    const auto it = values_.find(Key(section, key));
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::string Config::GetString(std::string_view section, std::string_view key,
                              std::string_view def) const {
    auto v = Get(section, key);
    return v ? *v : std::string(def);
}

long long Config::GetInt(std::string_view section, std::string_view key, long long def) const {
    auto v = Get(section, key);
    if (!v) return def;
    long long out = 0;
    const char* b = v->data();
    const char* e = b + v->size();
    int base = 10;
    if (v->size() > 2 && (*v)[0] == '0' && ((*v)[1] == 'x' || (*v)[1] == 'X')) {
        b += 2;
        base = 16;
    }
    auto [p, ec] = std::from_chars(b, e, out, base);
    return (ec == std::errc() && p == e) ? out : def;
}

double Config::GetFloat(std::string_view section, std::string_view key, double def) const {
    auto v = Get(section, key);
    if (!v) return def;
    double out = 0;
    auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    return (ec == std::errc() && p == v->data() + v->size()) ? out : def;
}

bool Config::GetBool(std::string_view section, std::string_view key, bool def) const {
    auto v = Get(section, key);
    if (!v) return def;
    const std::string l = Lower(*v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") return true;
    if (l == "0" || l == "false" || l == "no" || l == "off") return false;
    return def;
}

void Config::Set(std::string_view section, std::string_view key, std::string_view value) {
    values_[Key(section, key)] = std::string(value);
}

}  // namespace sspc
