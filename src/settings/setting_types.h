// SecondSonPC — the typed description of one PC setting (docs/PC_SETTINGS_IMPLEMENTATION.md §3): where it lives in
// SecondSonPC.toml, its type and range, how a change takes effect, which runtime capability it needs, its labels.
// The settings menu, the user settings file and the self-tests all work from these descriptions. Own code.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/abi.h"
#include "input/settings.h"

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Settings {

enum class Category : u8 {
    Display, Graphics, Audio, Controls, Keyboard, Gamepad, Interface, Game, Shaders, Upscaling, Experiments, Count
};
constexpr size_t kCategoryCount = static_cast<size_t>(Category::Count);

enum class Type : u8 {
    Bool,
    Int,
    Float,
    Choice,  // one of `choices` (stored as its value string)
    Text,    // free text (a display or audio device name; the menu offers what the system has)
    Binding, // the inputs of one keyboard/mouse action
};

/// How a change takes effect.
enum class Apply : u8 {
    Live,        // at once, while playing
    Restart,     // stored now, used from the next start ("restart required" in the menu)
    Unavailable, // not implemented: shown greyed with the reason, never stored, never changes anything
};

/// What this process can change while running, decided at start (GPU backend, window, audio). A setting whose
/// `needs` is not all here is unavailable for this run, with `why_*` (or a generic reason) shown.
namespace Capability {
constexpr u32 Window = 1u << 0;         // an SDL window (absent in self-tests): mode, size, display
constexpr u32 BridgePresenter = 1u << 1; // the bridge's Vulkan presenter shows the frames: FSR/RCAS, FPS counter
constexpr u32 LiveVolume = 1u << 2;     // the bridge's audio output (SDL): the volume per buffer, the device by name
constexpr u32 RealController = 1u << 3; // libScePad is SecondSonPC's (stick tuning, rumble scale)
// P4.6 contract (docs/PC_SETTINGS_IMPLEMENTATION.md §9): bits the native backend will report when it can.
constexpr u32 RenderScale = 1u << 8;
constexpr u32 FrameLimiter = 1u << 9;
constexpr u32 FpsCounter = 1u << 10; // a presenter that draws the FPS counter and switches it while running (either)
// Upscaling (docs/UPSCALING_FRAME_GENERATION.md): what this GPU and its driver can do, found when the backend started
constexpr u32 Dlss = 1u << 11;         // DLSS Super Resolution (GeForce RTX, NGX)
constexpr u32 FsrFrameGen = 1u << 12;  // FSR 3.1 frame generation (amd_fidelityfx_vk.dll)
constexpr u32 DlssFrameGen = 1u << 13; // DLSS Frame Generation (Streamline, GeForce RTX 40/50)
constexpr u32 Reflex = 1u << 14;       // NVIDIA Reflex (VK_NV_low_latency2: GeForce, a recent driver)
// R11 display: the native presenter replaces its swapchain for a new present mode while the game runs
constexpr u32 PresentModeSwitch = 1u << 15;
} // namespace Capability

struct Choice {
    std::string_view value; // as written in SecondSonPC.toml
    std::string_view en;
    std::string_view ru;
};

using Bindings = std::vector<Input::InputCode>;
using Value = std::variant<bool, s64, double, std::string, Bindings>;

struct Desc;
using Getter = Value (*)(const App::Config& config, const Desc& desc);
using Setter = void (*)(App::Config& config, const Desc& desc, const Value& value);

struct Desc {
    std::string_view id;    // "display.fullscreen"; also the menu's identity of the row
    std::string_view table; // SecondSonPC.toml table ("" for an Unavailable setting: nothing stored)
    std::string_view key;
    Category category;
    Type type;
    Apply apply;
    u32 needs = 0;    // Capability bits
    double min = 0.0, max = 0.0;       // Int/Float: accepted range (SecondSonPC.toml and the user file)
    double ui_min = 0.0, ui_max = 0.0; // Int/Float: the menu's slider range (within min..max)
    double step = 0.0;                 // the menu's step
    std::string_view unit;             // "%", "x"... after the value in the menu
    std::span<const Choice> choices;
    std::string_view en, ru;           // label
    std::string_view help_en, help_ru; // what it does, shown under the list
    std::string_view why_en, why_ru;   // Unavailable / missing capability: why
    Getter get = nullptr;
    Setter set = nullptr;
    s32 action = -1;            // Binding: the Input::Action
    bool legacy_parse = false;  // SecondSonPC.toml: parsed by LoadConfig's own code (keys older than this file)
    std::string_view requires_on; // another Bool setting's id that must be true for this one to matter
    bool own_row = true;          // false: the menu edits it through a combined row (window width + height)
    // Apply::Live only in a run with all these Capability bits; in another run the setting applies from the next
    // start (Service::EffectiveApply). 0: `apply` as it is.
    u32 live_needs = 0;
    // Choice: a value outside `choices` this accepts is valid too (SecondSonPC.toml / the user file: a frame rate
    // limit of 75, a resolution of 1366x768); the menu lists it while it is the current value. nullptr: choices only.
    bool (*accepts)(std::string_view value) = nullptr;
};

} // namespace SecondSon::Settings
