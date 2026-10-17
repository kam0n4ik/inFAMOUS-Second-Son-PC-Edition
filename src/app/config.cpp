// SecondSonPC — SecondSonPC.toml parser. Wrong types are errors, never silent defaults.
#include "app/config.h"

#include <algorithm>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <toml.hpp>

#include "runtime/language.h"
#include "settings/registry.h"
#include "settings/toml_value.h"

namespace SecondSon::App {

namespace {

std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

class Reader {
public:
    Reader(const toml::value& root, std::vector<std::string>& errors) : root_{root}, errors_{errors} {}

    const toml::value* Find(const char* table, const char* key) const {
        known_.emplace(table, key);
        if (!root_.is_table() || !root_.contains(table)) {
            return nullptr;
        }
        const auto& t = root_.at(table);
        if (!t.is_table() || !t.contains(key)) {
            return nullptr;
        }
        return &t.at(key);
    }

    void String(const char* table, const char* key, std::string& out) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_string()) {
                out = v->as_string();
            } else {
                Bad(table, key, "a string");
            }
        }
    }

    void Bool(const char* table, const char* key, bool& out) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_boolean()) {
                out = v->as_boolean();
            } else {
                Bad(table, key, "true/false");
            }
        }
    }

    void UInt(const char* table, const char* key, u32& out, u32 min, u32 max) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_integer() && v->as_integer() >= min && v->as_integer() <= max) {
                out = static_cast<u32>(v->as_integer());
            } else {
                Bad(table, key, fmt::format("an integer in [{}, {}]", min, max));
            }
        }
    }

    void Number(const char* table, const char* key, double& out, double min, double max) const {
        if (const auto* v = Find(table, key)) {
            double d = 0.0;
            if (v->is_floating()) {
                d = v->as_floating();
            } else if (v->is_integer()) {
                d = static_cast<double>(v->as_integer());
            } else {
                Bad(table, key, "a number");
                return;
            }
            if (d < min || d > max) {
                Bad(table, key, fmt::format("a number in [{}, {}]", min, max));
                return;
            }
            out = d;
        }
    }

    /// A string or an array of strings. Returns whether the key is present.
    bool Strings(const char* table, const char* key, std::vector<std::string>& out) const {
        const auto* v = Find(table, key);
        if (v == nullptr) {
            return false;
        }
        if (v->is_string()) {
            out.push_back(v->as_string());
            return true;
        }
        if (v->is_array()) {
            for (const auto& item : v->as_array()) {
                if (!item.is_string()) {
                    Bad(table, key, "a string or an array of strings");
                    return false;
                }
                out.push_back(item.as_string());
            }
            return true;
        }
        Bad(table, key, "a string or an array of strings");
        return false;
    }

    void Error(std::string message) const {
        errors_.push_back(std::move(message));
    }

    /// Every table/key of the file must have been asked for: a misspelt key is an error, not a
    /// silently ignored line.
    void ReportUnknown() const {
        if (!root_.is_table()) {
            return;
        }
        for (const auto& [table, value] : root_.as_table()) {
            const bool known_table = std::ranges::any_of(
                known_, [&](const auto& k) { return k.first == table; });
            if (!known_table) {
                errors_.push_back(fmt::format("unknown table [{}]", table));
                continue;
            }
            if (!value.is_table()) {
                errors_.push_back(fmt::format("[{}] must be a table", table));
                continue;
            }
            for (const auto& [key, unused] : value.as_table()) {
                if (!known_.contains({table, key})) {
                    errors_.push_back(fmt::format("[{}] unknown key {}", table, key));
                }
            }
        }
    }

private:
    void Bad(const char* table, const char* key, std::string_view expected) const {
        errors_.push_back(fmt::format("[{}] {} must be {}", table, key, expected));
    }

    const toml::value& root_;
    std::vector<std::string>& errors_;
    mutable std::set<std::pair<std::string, std::string>> known_;
};

} // namespace

std::optional<Config> LoadConfig(const std::filesystem::path& root, std::vector<std::string>& errors) {
    Config c;
    c.root = root;
    c.file = root / "SecondSonPC.toml";

    toml::value data;
    try {
        data = toml::parse(c.file);
    } catch (const std::exception& e) {
        errors.push_back(fmt::format("cannot parse {}: {}", c.file.string(), e.what()));
        return std::nullopt;
    }
    const Reader r{data, errors};

    std::string game = "E:\\shdps4\\CUSA00004";
    r.String("Game", "path", game);
    c.game_dir = Utf8Path(game);
    if (!std::filesystem::is_regular_file(c.game_dir / "eboot.bin")) {
        r.Error(fmt::format("[Game] path: {} does not contain eboot.bin", game));
    }

    c.language_locale = "en-US";
    r.String("Language", "system", c.language_locale);
    if (const auto* lang = Runtime::FindLanguageByLocale(c.language_locale)) {
        c.language_id = lang->id;
        c.language_locale = std::string(lang->locale);
    } else {
        r.Error(fmt::format("[Language] system = \"{}\" is not a PS4 system language "
                            "(use \"en-US\" or \"ru-RU\")",
                            c.language_locale));
    }

    r.String("System", "name", c.system_name);
    if (c.system_name.size() > 64) {
        r.Error("[System] name must be 0..64 bytes (PS4 system name limit)");
    }
    std::string enter = "cross";
    r.String("System", "enterButton", enter);
    if (enter == "cross" || enter == "circle") {
        c.enter_button_cross = enter == "cross";
    } else {
        r.Error("[System] enterButton must be \"cross\" or \"circle\"");
    }

    r.Bool("Console", "neoMode", c.neo_mode);
    r.Bool("Console", "devKit", c.dev_kit);
    r.UInt("Console", "extraDirectMemoryMiB", c.extra_dmem_mib, 0, 4096);
    r.UInt("Console", "extraFlexibleMemoryMiB", c.extra_fmem_mib, 0, 4096);

    std::string user_data = "userdata";
    std::string logs = "logs";
    r.String("Paths", "userData", user_data);
    r.String("Paths", "logs", logs);
    const auto resolve = [&](const std::string& p) {
        const auto path = Utf8Path(p);
        return path.is_absolute() ? path : root / path;
    };
    c.user_data_dir = resolve(user_data);
    c.logs_dir = resolve(logs);

    std::string shader_cache = "cache\\shaders";
    r.Bool("ShaderCache", "enabled", c.shader_cache);
    r.String("ShaderCache", "dir", shader_cache);
    c.shader_cache_dir = resolve(shader_cache);

    r.UInt("Display", "width", c.window_width, 640, 7680);
    r.UInt("Display", "height", c.window_height, 360, 4320);
    r.Bool("Display", "fullscreen", c.fullscreen);
    // experiment/bbport-gpu: [Graphics] backend = "native" (SecondSonPC's own, the default) | "bbport" (read at
    // start-up only: the backend owns the Vulkan device and the guest GPU for the whole run)
    {
        std::string backend;
        r.String("Graphics", "backend", backend);
        if (backend == "bbport") {
            c.gpu_backend = Config::GpuBackend::Bbport;
        } else if (!backend.empty() && backend != "native") {
            r.Error("[Graphics] backend must be \"native\" or \"bbport\"");
        }
    }
    r.UInt("Display", "vblankFrequency", c.vblank_hz, 30, 360);
    std::string present = "mailbox";
    r.String("Display", "presentMode", present);
    if (present == "mailbox" || present == "fifo" || present == "immediate") {
        c.present_mode = present == "mailbox" ? "Mailbox" : present == "fifo" ? "Fifo" : "Immediate";
    } else {
        r.Error("[Display] presentMode must be \"mailbox\", \"fifo\" or \"immediate\"");
    }
    std::string fullscreen_mode = "borderless";
    r.String("Display", "fullscreenMode", fullscreen_mode);
    if (fullscreen_mode == "borderless" || fullscreen_mode == "exclusive") {
        c.exclusive_fullscreen = fullscreen_mode == "exclusive";
    } else {
        r.Error("[Display] fullscreenMode must be \"borderless\" or \"exclusive\"");
    }
    r.Bool("Display", "showFps", c.show_fps);

    r.UInt("Audio", "volume", c.volume, 0, 100);
    r.String("Audio", "device", c.audio_device);

    r.Bool("Debug", "vulkanValidation", c.vulkan_validation);
    r.Bool("Debug", "vulkanCrashDiagnostic", c.vulkan_crash_diagnostic);
    r.Bool("Debug", "inputLog", c.input_log);
    r.Bool("Debug", "crashDump", c.crash_dump);

    auto& in = c.input;
    r.Number("Input", "mouseSensitivity", in.mouse_sensitivity, 0.01, 100.0);
    r.Bool("Input", "invertY", in.invert_y);
    r.Number("Input", "mouseDeadzone", in.mouse_deadzone, 0.0, 0.9);
    r.Bool("Input", "captureMouse", in.capture_mouse);
    r.Number("Input", "motionSensitivity", in.motion_sensitivity, 0.01, 100.0);
    std::string sideways = "right-up";
    r.String("Input", "sideways", sideways);
    if (sideways == "right-up" || sideways == "left-up") {
        in.sideways_right_up = sideways == "right-up";
    } else {
        r.Error("[Input] sideways must be \"right-up\" or \"left-up\"");
    }
    // [Keyboard]: action = "Key" or ["Key", "Mouse Left", ...]; a listed action replaces its
    // default binding, [] leaves it unbound.
    for (size_t a = 0; a < Input::kActionCount; ++a) {
        const std::string key(Input::ActionName(static_cast<Input::Action>(a)));
        std::vector<std::string> names;
        if (!r.Strings("Keyboard", key.c_str(), names)) {
            continue;
        }
        std::vector<Input::InputCode> codes;
        for (const auto& n : names) {
            Input::InputCode code;
            if (Input::ParseInput(n, code)) {
                codes.push_back(code);
            } else {
                r.Error(fmt::format("[Keyboard] {}: unknown key \"{}\" (SDL key names, e.g. \"Space\", "
                                    "\"W\", \"Left Shift\", \"Return\", or \"Mouse Left\")",
                                    key, n));
            }
        }
        in.bindings[a] = std::move(codes);
    }

    std::string level = "info";
    r.String("Log", "level", level);
    if (const auto parsed = Log::ParseLevel(level)) {
        c.log_level = *parsed;
    } else {
        r.Error("[Log] level must be trace|debug|info|warning|error|critical|off");
    }
    r.String("Log", "bridgeFilter", c.bridge_log_filter);

    r.Bool("Trace", "importCounter", c.import_counter);
    r.UInt("Trace", "dumpIntervalSeconds", c.dump_interval_s, 1, 3600);

    // PC settings added with the in-game settings menu: parsed through the settings registry, by the same rules as
    // the user settings file (docs/PC_SETTINGS_IMPLEMENTATION.md); here a wrong value is an error, as above.
    for (const auto& d : Settings::All()) {
        if (d.legacy_parse || d.table.empty() || d.set == nullptr) {
            continue;
        }
        const std::string table(d.table), key(d.key);
        if (const auto* v = r.Find(table.c_str(), key.c_str())) {
            Settings::Value value;
            std::string error;
            if (Settings::FromToml(d, *v, value, error)) {
                d.set(c, d, value);
            } else {
                r.Error(fmt::format("[{}] {}: {}", table, key, error));
            }
        }
    }

    r.ReportUnknown();
    if (!errors.empty()) {
        return std::nullopt;
    }
    return c;
}

} // namespace SecondSon::App
