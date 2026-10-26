// SecondSonPC — the PC settings window as a model (see settings_menu.h).
#include "ui/settings_menu.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <span>

#include <fmt/format.h>

#include "input/binding_rules.h"
#include "settings/experiments.h"
#include "settings/registry.h"

namespace SecondSon::Ui {

namespace {

using Settings::Category;
using Settings::Desc;
using Settings::Type;
using Settings::Value;
namespace Cap = Settings::Capability;

// ---- the look: inFAMOUS Second Son's menus on a PC (a dark see-through panel, a thin white frame with the game's
// corner square and top tab, the game's yellow for what is chosen, slab-serif capitals for titles)
constexpr Color kPanel{15, 16, 19, 236};
constexpr Color kShadow{0, 0, 0, 40};
constexpr Color kFrame{255, 255, 255, 84};
constexpr Color kFrameMark{240, 240, 240, 255};
constexpr Color kNavBg{0, 0, 0, 64};
constexpr Color kLine{255, 255, 255, 30};
constexpr Color kRowFocus{255, 255, 255, 18};
constexpr Color kRowHover{255, 255, 255, 9};
constexpr Color kAccent{245, 184, 0, 255};
constexpr Color kAccentSoft{245, 184, 0, 36};
constexpr Color kAccentLine{245, 184, 0, 150};
constexpr Color kOnAccent{20, 18, 12, 255};
constexpr Color kText{236, 236, 238, 255};
constexpr Color kText2{158, 160, 168, 255};
constexpr Color kText3{100, 102, 110, 255};
constexpr Color kWarn{255, 112, 96, 255};
constexpr Color kRestart{255, 172, 64, 255};
constexpr Color kControl{255, 255, 255, 12};
constexpr Color kControlLine{255, 255, 255, 44};
constexpr Color kControlHover{255, 255, 255, 90};
constexpr Color kTrack{255, 255, 255, 40};
constexpr Color kKnob{232, 232, 236, 255};
constexpr Color kKeycap{255, 255, 255, 16};
constexpr Color kPopup{26, 27, 32, 252};
constexpr Color kShade{0, 0, 0, 150};

Color Alpha(Color c, float a) {
    c.a = static_cast<u8>(std::clamp(c.a * a, 0.0f, 255.0f));
    return c;
}

/// Capitals as the game writes its titles (ASCII and Russian; other text unchanged).
std::string Caps(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 32);
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < utf8.size()) {
            const auto d = static_cast<unsigned char>(utf8[i + 1]);
            if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) { // а..п
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d - 0x20);
            } else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) { // р..я
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
            } else if (c == 0xD1 && d == 0x91) { // ё
                out += static_cast<char>(0xD0);
                out += static_cast<char>(0x81);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(d);
            }
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

constexpr Page kPages[] = {Page::Graphics, Page::Display, Page::Controls, Page::Keys,
                           Page::Audio,    Page::Game,    Page::Advanced, Page::Experiments};
constexpr u64 kConfirmCommandNs = 5'000'000'000;

using A = Input::Action;
constexpr A kMoveActions[] = {A::MoveForward, A::MoveBack, A::MoveLeft, A::MoveRight, A::Walk};
constexpr A kButtonActions[] = {A::Cross,  A::Circle,  A::Square, A::Triangle, A::L1,       A::R1,
                                A::L2,     A::R2,      A::L3,     A::R3,       A::Options,  A::DpadUp,
                                A::DpadDown, A::DpadLeft, A::DpadRight};
constexpr A kTouchActions[] = {A::TouchPad, A::TouchTwoFingers, A::TouchDrag, A::MotionMode, A::Shake};
constexpr A kPcActions[] = {A::ReleaseMouse};
struct Group {
    std::string_view en, ru;
    std::span<const A> actions;
};
const Group kGroups[] = {
    {"Movement", "Движение", kMoveActions},
    {"Controller buttons", "Кнопки контроллера", kButtonActions},
    {"Touch pad and motion", "Сенсорная панель и наклон", kTouchActions},
    {"SecondSonPC", "SecondSonPC", kPcActions},
};

constexpr std::string_view kDisplayIds[] = {"display.fullscreen", "display.fullscreenMode", "display.width",
                                            "display.height", "display.monitor"};

bool IsDisplayId(std::string_view id) {
    return std::ranges::find(kDisplayIds, id) != std::end(kDisplayIds);
}

const Desc* BindingDesc(Input::Action a) {
    return Settings::Find(fmt::format("keyboard.{}", Input::ActionName(a)));
}

std::string Replace(std::string s, std::string_view from, std::string_view to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
    return s;
}

// ---- layout (units of a 900 x 700 window; pixels = units x scale)
constexpr float kHeaderH = 60, kFooterH = 52, kNavW = 210, kHelpH = 96;
constexpr float kRowH = 40, kGroupH = 34, kResetH = 50;
constexpr float kListX = 222, kListR = 876; // rows from x 222 to 876
constexpr float kListTop = kHeaderH + 50;   // under the page title
constexpr float kCtrlW = 270;               // a row's control, right-aligned at kListR - 12
constexpr float kListItemH = 30;
constexpr int kListVisible = 8;

enum class Hint : u8 { Navigate, Adjust, Accept, Back, Reset, Clear, Tabs };

/// A button symbol of the footer: a keycap, a PlayStation shape or an Xbox letter, drawn from primitives (no font
/// glyphs, no system icons). Returns its width.
float DrawGlyph(DrawList& out, TextMeasure& measure, Hint hint, const MenuEnv& env, float x, float cy, float s) {
    const auto keycap = [&](std::string_view label) {
        const float size = 12.0f * s;
        const float w = std::max(24.0f * s, measure.Width(label, size, FontWeight::Semibold) + 12.0f * s);
        const Rect r{x, cy - 11.0f * s, w, 22.0f * s};
        out.FillRect(r, kKeycap, 4.0f * s);
        out.StrokeRect(r, kControlLine, std::max(1.0f, s), 4.0f * s);
        out.Text(r, label, size, kText, Align::Center, FontWeight::Semibold);
        return w;
    };
    const float r = 11.0f * s;
    const float gx = x + r;
    switch (env.prompts) {
    case Input::PromptStyle::Keyboard: {
        constexpr std::string_view kKeys[] = {"↑↓", "←→", "Enter", "Esc", "Del", "Backspace", "Tab"};
        return keycap(kKeys[static_cast<size_t>(hint)]);
    }
    case Input::PromptStyle::PlayStation: {
        enum Shape { Cross, Circle, Square, Triangle };
        Shape shape;
        switch (hint) {
        case Hint::Navigate:
            return keycap("↑↓");
        case Hint::Adjust:
            return keycap("←→");
        case Hint::Tabs:
            return keycap("L1 R1");
        case Hint::Accept:
            shape = env.confirm_is_circle ? Circle : Cross;
            break;
        case Hint::Back:
            shape = env.confirm_is_circle ? Cross : Circle;
            break;
        case Hint::Reset:
            shape = Square;
            break;
        case Hint::Clear:
        default:
            shape = Triangle;
            break;
        }
        out.FillCircle(gx, cy, r, Color{40, 41, 46, 255});
        const float k = 5.0f * s, w = 1.8f * s;
        switch (shape) {
        case Cross:
            out.Line(gx - k, cy - k, gx + k, cy + k, Color{124, 170, 234}, w);
            out.Line(gx - k, cy + k, gx + k, cy - k, Color{124, 170, 234}, w);
            break;
        case Circle:
            out.StrokeCircle(gx, cy, k, Color{232, 98, 98}, w);
            break;
        case Square:
            out.StrokeRect({gx - k, cy - k, 2 * k, 2 * k}, Color{214, 126, 190}, w);
            break;
        case Triangle:
            out.StrokeTriangle(gx, cy - k, gx + k, cy + k * 0.8f, gx - k, cy + k * 0.8f, Color{92, 200, 160}, w);
            break;
        }
        return 2 * r;
    }
    case Input::PromptStyle::Xbox: {
        struct Letter {
            std::string_view text;
            Color color;
        };
        Letter letter;
        switch (hint) {
        case Hint::Navigate:
            return keycap("↑↓");
        case Hint::Adjust:
            return keycap("←→");
        case Hint::Tabs:
            return keycap("LB RB");
        case Hint::Accept:
            letter = {"A", Color{94, 186, 73}};
            break;
        case Hint::Back:
            letter = {"B", Color{222, 74, 63}};
            break;
        case Hint::Reset:
            letter = {"X", Color{63, 129, 222}};
            break;
        case Hint::Clear:
        default:
            letter = {"Y", Color{240, 190, 40}};
            break;
        }
        out.FillCircle(gx, cy, r, letter.color);
        out.Text({x, cy - r, 2 * r, 2 * r}, letter.text, 12.0f * s, Color{20, 22, 26}, Align::Center,
                 FontWeight::Semibold);
        return 2 * r;
    }
    }
    return 0.0f;
}

/// The navigation's icons: line drawings on a 20-unit grid around (cx, cy), one stroke width, no font glyphs.
void DrawPageIcon(DrawList& out, Page page, float cx, float cy, float s, Color c) {
    const float k = s; // 1 icon unit = 1 layout unit
    const float w = std::max(1.0f, 1.5f * s);
    const auto P = [&](float x, float y) { return std::pair{cx + x * k, cy + y * k}; };
    const auto L = [&](float x0, float y0, float x1, float y1) {
        const auto [a, b] = P(x0, y0);
        const auto [e, f] = P(x1, y1);
        out.Line(a, b, e, f, c, w);
    };
    const auto R = [&](float x, float y, float rw, float rh) { return Rect{cx + x * k, cy + y * k, rw * k, rh * k}; };
    switch (page) {
    case Page::Graphics: { // a picture: frame, mountains, sun
        out.StrokeRect(R(-9, -7, 18, 14), c, w, 2 * k);
        out.FillTriangle(cx - 6 * k, cy + 4.5f * k, cx - 1.5f * k, cy - 1 * k, cx + 2.5f * k, cy + 4.5f * k, c);
        out.FillTriangle(cx + 0.5f * k, cy + 4.5f * k, cx + 3.5f * k, cy + 1 * k, cx + 6.5f * k, cy + 4.5f * k, c);
        out.FillCircle(cx + 4 * k, cy - 2.5f * k, 1.6f * k, c);
        break;
    }
    case Page::Display: // a monitor on its stand
        out.StrokeRect(R(-9, -7.5f, 18, 12.5f), c, w, 1.5f * k);
        L(0, 5, 0, 8);
        L(-4.5f, 8.5f, 4.5f, 8.5f);
        break;
    case Page::Controls: // a controller: body, d-pad, two buttons
        out.StrokeRect(R(-9.5f, -5.5f, 19, 11.5f), c, w, 5 * k);
        L(-6.5f, 0.3f, -2.5f, 0.3f);
        L(-4.5f, -1.7f, -4.5f, 2.3f);
        out.FillCircle(cx + 3.5f * k, cy - 0.8f * k, 1.2f * k, c);
        out.FillCircle(cx + 6 * k, cy + 1.5f * k, 1.2f * k, c);
        break;
    case Page::Keys: { // a keyboard: keys and the space bar
        out.StrokeRect(R(-9.5f, -6, 19, 12), c, w, 1.5f * k);
        for (int i = 0; i < 5; ++i) {
            out.FillRect(R(-6.8f + 3.2f * static_cast<float>(i), -3.4f, 1.8f, 1.8f), c);
        }
        for (int i = 0; i < 4; ++i) {
            out.FillRect(R(-5.2f + 3.2f * static_cast<float>(i), -0.4f, 1.8f, 1.8f), c);
        }
        out.FillRect(R(-4, 2.6f, 8, 1.6f), c);
        break;
    }
    case Page::Audio: // a speaker and two waves
        out.FillRect(R(-8.5f, -2.8f, 3, 5.6f), c);
        out.FillTriangle(cx - 5.5f * k, cy - 2.8f * k, cx - 1 * k, cy - 7 * k, cx - 1 * k, cy + 7 * k, c);
        out.FillTriangle(cx - 5.5f * k, cy - 2.8f * k, cx - 1 * k, cy + 7 * k, cx - 5.5f * k, cy + 2.8f * k, c);
        L(2, -3, 3.6f, 0);
        L(3.6f, 0, 2, 3);
        L(5, -6, 7.6f, 0);
        L(7.6f, 0, 5, 6);
        break;
    case Page::Game: // a speech bubble (language, subtitles, interface)
        out.StrokeRect(R(-9, -7.5f, 18, 12), c, w, 2 * k);
        out.FillTriangle(cx - 5 * k, cy + 4 * k, cx - 1 * k, cy + 4 * k, cx - 5.5f * k, cy + 8.5f * k, c);
        L(-5.5f, -3.5f, 5.5f, -3.5f);
        L(-5.5f, 0, 2.5f, 0);
        break;
    case Page::Advanced: { // sliders
        const float knob[] = {-3.0f, 4.0f, -1.0f};
        for (int i = 0; i < 3; ++i) {
            const float y = -5.5f + 5.5f * static_cast<float>(i);
            L(-9, y, 9, y);
            out.FillCircle(cx + knob[i] * k, cy + y * k, 2.4f * k, c);
            out.FillCircle(cx + knob[i] * k, cy + y * k, 1.1f * k, kPanel);
        }
        break;
    }
    case Page::Experiments: // a flask: neck, body, liquid
        L(-2.5f, -8, -2.5f, -2.5f);
        L(2.5f, -8, 2.5f, -2.5f);
        L(-4, -8, 4, -8);
        L(-2.5f, -2.5f, -8, 7);
        L(2.5f, -2.5f, 8, 7);
        L(-8, 7, 8, 7);
        out.FillTriangle(cx - 6.2f * k, cy + 4 * k, cx + 6.2f * k, cy + 4 * k, cx + 7.8f * k, cy + 6.8f * k, c);
        out.FillTriangle(cx - 6.2f * k, cy + 4 * k, cx + 7.8f * k, cy + 6.8f * k, cx - 7.8f * k, cy + 6.8f * k, c);
        break;
    case Page::Count:
        break;
    }
}

/// A small "v" (a drop-down list), pointing up when `up`.
void Chevron(DrawList& out, float cx, float cy, float s, Color c, bool up = false) {
    const float a = 4.5f * s, b = up ? -2.5f * s : 2.5f * s;
    out.Line(cx - a, cy - b, cx, cy + b, c, std::max(1.0f, 1.6f * s));
    out.Line(cx, cy + b, cx + a, cy - b, c, std::max(1.0f, 1.6f * s));
}

} // namespace

std::string_view PageLabel(Page page, bool ru) {
    constexpr std::array<std::string_view, kPageCount> kEn{"Graphics", "Display", "Controls", "Key bindings",
                                                          "Audio",    "Game",    "Advanced", "Experiments"};
    constexpr std::array<std::string_view, kPageCount> kRu{"Графика", "Экран", "Управление", "Клавиши",
                                                          "Звук",    "Игра",  "Дополнительно", "Эксперименты"};
    const auto i = std::min(static_cast<size_t>(page), kPageCount - 1);
    return ru ? kRu[i] : kEn[i];
}

Page PageOf(Category category) {
    switch (category) {
    case Category::Display:
        return Page::Display;
    case Category::Graphics:
    case Category::Upscaling:
        return Page::Graphics;
    case Category::Audio:
        return Page::Audio;
    case Category::Controls:
    case Category::Gamepad:
        return Page::Controls;
    case Category::Keyboard:
        return Page::Keys;
    case Category::Interface:
    case Category::Game:
        return Page::Game;
    case Category::Shaders:
        return Page::Advanced;
    case Category::Experiments:
        return Page::Experiments;
    case Category::Count:
        break;
    }
    return Page::Graphics;
}

SettingsMenu::SettingsMenu(Settings::Service& service) : service_{service} {}

void SettingsMenu::Open() {
    open_ = true;
    close_request_ = false;
    capture_.reset();
    modal_ = Modal::None;
    dragging_row_ = -1;
    dragging_thumb_ = false;
    hover_row_ = hover_page_ = hover_item_ = hover_close_ = -1;
    status_.clear();
    // the page, its focused row and its scroll stay as they were in this session
    const std::string focused = FocusedId();
    RebuildRows();
    if (focused.empty() || !Focus(focused)) {
        focus_ = 0;
        MoveFocus(0);
    }
    scroll_to_focus_ = true;
}

void SettingsMenu::Close() {
    if (modal_ == Modal::ConfirmDisplay) {
        RevertDisplay(); // closed without keeping them: the safe side (the picture may be gone)
    }
    open_ = false;
    capture_.reset();
    modal_ = Modal::None; // an open drop-down list is closed without choosing
    dragging_row_ = -1;
    dragging_thumb_ = false;
}

bool SettingsMenu::TakeCloseRequest() {
    return std::exchange(close_request_, false);
}

void SettingsMenu::SetEnv(MenuEnv env) {
    env_ = std::move(env);
}

void SettingsMenu::SelectPage(Page page) {
    if (modal_ == Modal::List) {
        modal_ = Modal::None;
    }
    page_ = page;
    capture_.reset();
    hover_row_ = -1;
    dragging_row_ = -1;
    dragging_thumb_ = false;
    scroll_ = 0;
    slot_ = 0;
    status_.clear();
    RebuildRows();
    focus_ = 0;
    MoveFocus(0);
    scroll_to_focus_ = true;
}

bool SettingsMenu::Shown(const Desc& d) const {
    if (d.apply == Settings::Apply::Unavailable) {
        return false; // not implemented: listed in docs/PC_SETTINGS_UI.md, never shown as a setting
    }
    const u32 caps = service_.Capabilities();
    if ((d.needs & Cap::BridgePresenter) != 0 && (caps & Cap::BridgePresenter) == 0) {
        return false; // the shadPS4 bridge's presenter only (--gpu-backend bridge)
    }
    if (d.id == "upscaling.frameGenMultiplier" && (caps & Cap::DlssFrameGen) == 0) {
        return false; // x3 / x4 are DLSS Multi Frame Generation, which this build does not run
    }
    return true;
}

void SettingsMenu::AddHeader(std::string_view en, std::string_view ru) {
    rows_.push_back({.kind = Row::Kind::Header, .en = en, .ru = ru});
}

void SettingsMenu::AddInfo(std::string_view en, std::string_view ru, std::string tag) {
    rows_.push_back({.kind = Row::Kind::Info, .en = en, .ru = ru, .tag = std::move(tag)});
}

void SettingsMenu::AddCategory(Category category) {
    for (const auto& d : Settings::All()) {
        if (d.category == category && d.own_row && d.id != "display.showFps" && Shown(d)) {
            rows_.push_back({.kind = Row::Kind::Setting, .desc = &d});
        }
    }
}

void SettingsMenu::RebuildRows() {
    rows_.clear();
    const auto setting = [this](std::string_view id) {
        if (const Desc* d = Settings::Find(id); d != nullptr && Shown(*d)) {
            rows_.push_back({.kind = Row::Kind::Setting, .desc = d});
        }
    };
    switch (page_) {
    case Page::Graphics: {
        AddHeader("Upscaling", "Масштабирование");
        setting("upscaling.upscaler");
        setting("upscaling.quality");
        setting("upscaling.sharpness");
        AddInfo("Render resolution", "Разрешение рендеринга", "info.render");
        AddHeader("Picture", "Картинка");
        setting("graphics.renderScale");
        setting("graphics.fov");
        AddHeader("Frame generation", "Генерация кадров");
        setting("upscaling.frameGeneration");
        setting("upscaling.frameGenMultiplier");
        setting("upscaling.reflex");
        AddHeader("Fire and smoke", "Огонь и дым");
        setting("graphics.smokeQuality");
        setting("graphics.particleResolution");
        AddHeader("Effects", "Эффекты"); // (R11 postfx: the game's own post-processing)
        setting("graphics.motionBlur");
        setting("graphics.motionBlurStrength");
        setting("graphics.reflectionQuality"); // (R12 refl)
        setting("graphics.rainShadows");       // (R12 rain)
        const size_t before = rows_.size();
        AddHeader("The bridge's frame (shadPS4)", "Кадр моста (shadPS4)");
        setting("graphics.fsr");
        setting("graphics.sharpening");
        setting("graphics.sharpness");
        if (rows_.size() == before + 1) {
            rows_.pop_back(); // the native backend: nothing of the bridge's presenter
        }
        break;
    }
    case Page::Display: {
        // R11: the window, the frames, the game's picture; then any other Display setting in the registry's order
        AddHeader("Window", "Окно");
        rows_.push_back({.kind = Row::Kind::WindowMode, .en = "Window mode", .ru = "Режим окна", .tag = "window.mode"});
        rows_.push_back({.kind = Row::Kind::WindowSize, .en = "Window size", .ru = "Размер окна", .tag = "window.size"});
        setting("display.monitor");
        AddHeader("Frames", "Кадры");
        setting("display.presentMode");
        setting("display.frameRateLimit");
        AddHeader("The game's picture", "Картинка игры");
        constexpr std::string_view kPlaced[] = {"display.monitor", "display.presentMode", "display.frameRateLimit",
                                                "display.showFps"};
        for (const auto& d : Settings::All()) {
            if (d.category == Category::Display && d.own_row && std::ranges::find(kPlaced, d.id) == std::end(kPlaced) &&
                Shown(d)) {
                rows_.push_back({.kind = Row::Kind::Setting, .desc = &d});
            }
        }
        break;
    }
    case Page::Controls:
        AddHeader("Mouse", "Мышь");
        setting("input.mouseSensitivity");
        setting("input.invertY");
        setting("input.mouseDeadzone");
        setting("input.captureMouse");
        AddHeader("Motion mode (graffiti, G)", "Режим наклона (граффити, G)");
        setting("input.motionSensitivity");
        setting("input.sideways");
        AddHeader("Controller", "Геймпад");
        AddInfo("Active input device", "Активное устройство ввода", "info.device");
        AddCategory(Category::Gamepad);
        break;
    case Page::Keys:
        for (const auto& g : kGroups) {
            AddHeader(g.en, g.ru);
            for (const auto a : g.actions) {
                if (const Desc* d = BindingDesc(a)) {
                    rows_.push_back({.kind = Row::Kind::Setting, .desc = d});
                }
            }
        }
        AddInfo("SecondSonPC hotkeys", "Горячие клавиши SecondSonPC", "info.hotkeys");
        break;
    case Page::Audio:
        AddCategory(Category::Audio);
        break;
    case Page::Game:
        AddCategory(Category::Game);
        AddHeader("SecondSonPC interface", "Интерфейс SecondSonPC");
        AddCategory(Category::Interface);
        break;
    case Page::Advanced:
        AddHeader("On screen", "На экране");
        setting("display.showFps");
        AddHeader("Shaders and cache", "Шейдеры и кэш");
        AddCategory(Category::Shaders);
        AddInfo("Cache size", "Размер кэша", "info.cache.size");
        AddInfo("In the cache", "В кэше", "info.cache.content");
        AddInfo("This run", "В этом запуске", "info.cache.run");
        rows_.push_back({.kind = Row::Kind::Command, .en = "Clear the shader cache", .ru = "Очистить кэш шейдеров",
                         .tag = "cmd.clear_shaders"});
        rows_.push_back({.kind = Row::Kind::Command, .en = "Clear the Vulkan pipeline cache",
                         .ru = "Очистить кэш конвейеров Vulkan", .tag = "cmd.clear_pipelines"});
        AddHeader("About this run", "Сведения о запуске");
        AddInfo("Graphics backend", "Графический бэкенд", "info.backend");
        AddInfo("Settings file", "Файл настроек", "info.settings");
        break;
    case Page::Experiments: {
        // the launcher's switches, by group; each applies from the next start (Reset: as the launcher set it)
        std::string_view group;
        for (const auto& e : Settings::Experiments()) {
            if (e.group_en != group) {
                group = e.group_en;
                AddHeader(e.group_en, e.group_ru);
            }
            setting(Settings::ExperimentId(e.env));
        }
        break;
    }
    case Page::Count:
        break;
    }
    rows_.push_back({.kind = Row::Kind::ResetPage, .en = "Reset this page", .ru = "Сбросить раздел", .tag = "reset.page"});
    if (focus_ >= rows_.size()) {
        focus_ = rows_.size() - 1;
    }
}

std::vector<std::string> SettingsMenu::ListedIds() const {
    std::vector<std::string> ids;
    for (const auto& r : rows_) {
        if (r.kind == Row::Kind::Setting && r.desc != nullptr) {
            ids.emplace_back(r.desc->id);
        } else if (!r.tag.empty()) {
            ids.push_back(r.tag);
        }
    }
    return ids;
}

bool SettingsMenu::Focusable(size_t row) const {
    return row < rows_.size() && rows_[row].kind != Row::Kind::Header;
}

void SettingsMenu::MoveFocus(int delta) {
    if (rows_.empty()) {
        return;
    }
    const int dir = delta < 0 ? -1 : 1;
    int steps = std::abs(delta);
    int at = static_cast<int>(focus_);
    if (steps == 0) {
        // settle on a focusable row from here
        while (at < static_cast<int>(rows_.size()) && !Focusable(at)) {
            ++at;
        }
        focus_ = static_cast<size_t>(std::min(at, static_cast<int>(rows_.size()) - 1));
        return;
    }
    int last_ok = at;
    while (steps > 0) {
        at += dir;
        if (at < 0 || at >= static_cast<int>(rows_.size())) {
            break;
        }
        if (Focusable(at)) {
            last_ok = at;
            --steps;
        }
    }
    if (static_cast<size_t>(last_ok) != focus_) {
        focus_ = static_cast<size_t>(last_ok);
        slot_ = 0;
        status_.clear();
    }
    scroll_to_focus_ = true;
}

std::string SettingsMenu::TakeCommand() {
    return std::exchange(command_, {});
}

std::string SettingsMenu::FocusedId() const {
    if (focus_ >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[focus_];
    return r.kind == Row::Kind::Setting && r.desc != nullptr ? std::string(r.desc->id) : r.tag;
}

bool SettingsMenu::Focus(std::string_view id) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        if ((r.kind == Row::Kind::Setting && r.desc != nullptr && r.desc->id == id) || (!r.tag.empty() && r.tag == id)) {
            focus_ = i;
            slot_ = 0;
            scroll_to_focus_ = true;
            return true;
        }
    }
    return false;
}

void SettingsMenu::BeginDisplayChange() {
    if (modal_ == Modal::ConfirmDisplay) {
        return;
    }
    pending_display_.clear();
    for (const auto id : kDisplayIds) {
        if (const Desc* d = Settings::Find(id)) {
            pending_display_.emplace_back(d, service_.Get(*d));
        }
    }
}

void SettingsMenu::EndDisplayChange(bool changed) {
    if (!changed || modal_ == Modal::ConfirmDisplay) {
        return;
    }
    display_before_ = pending_display_;
    modal_ = Modal::ConfirmDisplay;
    modal_button_ = 0;
    modal_deadline_ns_ = env_.now_ns + kConfirmNs;
}

void SettingsMenu::RevertDisplay() {
    if (!display_before_.empty()) {
        service_.SetMany(display_before_);
    }
    display_before_.clear();
    modal_ = Modal::None;
    status_ = std::string(T("Display settings restored.", "Параметры экрана возвращены."));
    status_warn_ = false;
}

void SettingsMenu::SetValue(const Desc& desc, const Value& value) {
    const auto avail = service_.Available(desc);
    if (!avail.available) {
        status_ = std::string(T(avail.why_en, avail.why_ru));
        status_warn_ = true;
        return;
    }
    const bool display = IsDisplayId(desc.id);
    if (display) {
        BeginDisplayChange();
    }
    std::string error;
    const auto result = service_.Set(desc, value, &error);
    if (display) {
        EndDisplayChange(result == Settings::SetResult::Ok);
    }
    if (result == Settings::SetResult::Invalid || result == Settings::SetResult::Unavailable) {
        status_ = error;
        status_warn_ = true;
    } else if (result == Settings::SetResult::Ok && service_.EffectiveApply(desc) == Settings::Apply::Restart) {
        status_ = service_.NeedsRestart(desc)
                      ? std::string(T("Saved. Takes effect after a restart.",
                                      "Сохранено. Вступит в силу после перезапуска игры."))
                      : std::string(T("Back to the value this run uses.", "Возвращено значение текущего запуска."));
        status_warn_ = false;
    }
}

void SettingsMenu::SetWindowSize(u32 w, u32 h) {
    const Desc* dw = Settings::Find("display.width");
    const Desc* dh = Settings::Find("display.height");
    if (dw == nullptr || dh == nullptr || !service_.Available(*dw).available) {
        if (dw != nullptr) {
            const auto a = service_.Available(*dw);
            status_ = std::string(T(a.why_en, a.why_ru));
            status_warn_ = true;
        }
        return;
    }
    BeginDisplayChange();
    const std::pair<const Desc*, Value> changes[] = {{dw, Value{static_cast<s64>(w)}}, {dh, Value{static_cast<s64>(h)}}};
    std::string error;
    const auto result = service_.SetMany(changes, &error);
    EndDisplayChange(result == Settings::SetResult::Ok);
    if (result == Settings::SetResult::Invalid) {
        status_ = error;
        status_warn_ = true;
    }
}

std::vector<std::pair<u32, u32>> SettingsMenu::WindowSizes() const {
    auto sizes = env_.window_sizes;
    const auto cfg = service_.Current();
    const std::pair<u32, u32> now{cfg.window_width, cfg.window_height};
    if (std::ranges::find(sizes, now) == sizes.end()) {
        sizes.push_back(now); // a size from the file that is not a preset stays selectable
    }
    std::ranges::sort(sizes);
    return sizes;
}

void SettingsMenu::StepWindowSize(int dir) {
    const auto sizes = WindowSizes();
    const auto cfg = service_.Current();
    const auto it = std::ranges::find(sizes, std::pair<u32, u32>{cfg.window_width, cfg.window_height});
    const auto idx = static_cast<int>(it - sizes.begin()) + dir;
    if (idx < 0 || idx >= static_cast<int>(sizes.size())) {
        return;
    }
    SetWindowSize(sizes[static_cast<size_t>(idx)].first, sizes[static_cast<size_t>(idx)].second);
}

std::string SettingsMenu::WindowModeNow() const {
    const auto cfg = service_.Current();
    return !cfg.fullscreen ? "window" : cfg.exclusive_fullscreen ? "exclusive" : "borderless";
}

std::vector<SettingsMenu::Option> SettingsMenu::WindowModeOptions() const {
    std::vector<Option> out{
        {.value = "window", .label = std::string(T("Windowed", "Оконный"))},
        {.value = "borderless", .label = std::string(T("Borderless (full screen)", "Без рамки (на весь экран)"))},
    };
    if (WindowModeNow() == "exclusive") {
        // [Display] fullscreenMode = "exclusive" from a file: kept while chosen, not offered (no Vulkan exclusive mode:
        // SDL's full screen at the desktop's display mode, as borderless, minimized on Alt+Tab)
        out.push_back({.value = "exclusive",
                       .label = std::string(T("Exclusive (SecondSonPC.toml)", "Эксклюзивный (SecondSonPC.toml)")),
                       .ok = false,
                       .why = std::string(T("no exclusive mode in the renderer: the display's mode is not changed, "
                                            "the window covers it as borderless and minimizes on Alt+Tab",
                                            "эксклюзивного режима в рендерере нет: режим монитора не меняется, окно "
                                            "занимает его как «без рамки» и сворачивается при Alt+Tab"))});
    }
    return out;
}

void SettingsMenu::SetWindowMode(std::string_view mode) {
    const Desc* fs = Settings::Find("display.fullscreen");
    const Desc* fm = Settings::Find("display.fullscreenMode");
    if (fs == nullptr || fm == nullptr) {
        return;
    }
    if (const auto a = service_.Available(*fs); !a.available) {
        status_ = std::string(T(a.why_en, a.why_ru));
        status_warn_ = true;
        return;
    }
    // (a mode chosen here is borderless or windowed: an "exclusive" of the file is not kept)
    const std::pair<const Desc*, Value> changes[] = {{fs, Value{mode != "window"}},
                                                     {fm, Value{std::string("borderless")}}};
    BeginDisplayChange();
    std::string error;
    const auto result = service_.SetMany(changes, &error);
    EndDisplayChange(result == Settings::SetResult::Ok);
    if (result == Settings::SetResult::Invalid || result == Settings::SetResult::Unavailable) {
        status_ = error;
        status_warn_ = true;
    }
}

void SettingsMenu::StepWindowMode(int dir) {
    const auto options = WindowModeOptions();
    const std::string now = WindowModeNow();
    int idx = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        idx = options[i].value == now ? static_cast<int>(i) : idx;
    }
    for (int next = idx + dir; next >= 0 && next < static_cast<int>(options.size()); next += dir) {
        if (options[static_cast<size_t>(next)].ok) {
            SetWindowMode(options[static_cast<size_t>(next)].value);
            return;
        }
    }
}

std::vector<std::string> SettingsMenu::TextOptions(const Desc& desc) const {
    std::vector<std::string> options{""};
    const auto& extra = desc.id == "display.monitor" ? env_.displays : env_.audio_devices;
    options.insert(options.end(), extra.begin(), extra.end());
    const auto current = std::get<std::string>(service_.Get(desc));
    if (std::ranges::find(options, current) == options.end()) {
        options.push_back(current); // a device that is gone now stays selectable
    }
    return options;
}

std::vector<SettingsMenu::Option> SettingsMenu::Options(const Desc& desc) const {
    std::vector<Option> out;
    const u32 caps = service_.Capabilities();
    if (desc.type == Type::Text) {
        for (const auto& v : TextOptions(desc)) {
            std::string label = ValueText(desc, Value{v});
            if (desc.id == "display.monitor") {
                // R11: the displays with their desktop size and refresh rate ("" = the primary one)
                if (v.empty() && !env_.primary_label.empty()) {
                    label = fmt::format("{} — {}", label, env_.primary_label);
                }
                const auto it = std::ranges::find(env_.displays, v);
                if (!v.empty() && it != env_.displays.end()) {
                    const size_t i = static_cast<size_t>(it - env_.displays.begin());
                    if (i < env_.display_labels.size() && !env_.display_labels[i].empty()) {
                        label = env_.display_labels[i];
                    }
                } else if (!v.empty()) {
                    label = fmt::format("{} ({})", v, T("not connected", "не подключён"));
                }
            }
            out.push_back({.value = v, .label = std::move(label)});
        }
        return out;
    }
    if (desc.type != Type::Choice) {
        return out;
    }
    const auto why = [this](std::string_view en, std::string_view ru, const std::string& detail) {
        std::string w(T(en, ru));
        if (!detail.empty()) {
            w += fmt::format(" ({})", detail);
        }
        return w;
    };
    for (const auto& c : desc.choices) {
        Option o{.value = std::string(c.value), .label = std::string(env_.ru ? c.ru : c.en)};
        if (desc.id == "upscaling.upscaler") {
            if (c.value == "dlss" && (caps & Cap::Dlss) == 0) {
                o.ok = false;
                o.why = why("DLSS needs a GeForce RTX card and its driver", "DLSS требует видеокарту GeForce RTX и её драйвер",
                            env_.dlss_why);
            } else if (c.value == "fsr" && !env_.fsr_why.empty()) {
                o.ok = false;
                o.why = why("FSR 3.1 cannot run", "FSR 3.1 не может работать", env_.fsr_why);
            }
        } else if (desc.id == "upscaling.frameGeneration") {
            if (c.value == "dlss" && (caps & Cap::DlssFrameGen) == 0) {
                continue; // DLSS Frame Generation is not part of this build: not offered at all
            }
            if (c.value == "fsr" && (caps & Cap::FsrFrameGen) == 0) {
                o.ok = false;
                o.why = why("FSR frame generation is not available on this system",
                            "Генерация кадров FSR недоступна в этой системе", env_.fsr_fg_why);
            }
        } else if (desc.id == "upscaling.frameGenMultiplier" && c.value != "2" && (caps & Cap::DlssFrameGen) == 0) {
            continue;
        } else if (desc.id == "display.presentMode" && env_.present_known) {
            // R11: what the presenter's surface does with it (Off without IMMEDIATE: MAILBOX, as the presenter picks)
            if (c.value == "immediate" && !env_.present_immediate) {
                o.label += env_.present_mailbox ? std::string(T(" (here: Mailbox)", " (здесь: Mailbox)"))
                                                : std::string(T(" (here: FIFO)", " (здесь: FIFO)"));
            } else if (c.value == "mailbox" && !env_.present_mailbox) {
                o.label += env_.present_immediate ? std::string(T(" (here: Immediate)", " (здесь: Immediate)"))
                                                  : std::string(T(" (here: FIFO)", " (здесь: FIFO)"));
            }
        } else if (desc.id == "display.resolution" && env_.display_size.first != 0 &&
                   c.value == fmt::format("{}x{}", env_.display_size.first, env_.display_size.second)) {
            o.label += std::string(T(" — the display's", " — как у монитора"));
        }
        out.push_back(std::move(o));
    }
    if (desc.id == "display.resolution" && env_.display_size.first != 0) {
        // R11: the window's display's own size, when no preset has it (1366x768, 2560x1080 of a laptop...)
        const std::string own = fmt::format("{}x{}", env_.display_size.first, env_.display_size.second);
        std::string error;
        if (std::ranges::none_of(out, [&](const Option& o) { return o.value == own; }) &&
            Settings::Validate(desc, Value{own}, error)) {
            out.push_back({.value = own,
                           .label = fmt::format("{} × {}{}", env_.display_size.first, env_.display_size.second,
                                                T(" — the display's", " — как у монитора"))});
        }
    }
    if (desc.accepts != nullptr) {
        // a value outside the list from SecondSonPC.toml / the user file ("75", "1366x768"): selectable while chosen
        const Value v = service_.Get(desc);
        const auto* now = std::get_if<std::string>(&v);
        if (now != nullptr && std::ranges::none_of(out, [&](const Option& o) { return o.value == *now; })) {
            out.push_back({.value = *now, .label = fmt::format("{} ({})", *now, T("from the settings file",
                                                                                   "из файла настроек"))});
        }
    }
    return out;
}

std::vector<SettingsMenu::Option> SettingsMenu::RowOptions(size_t row) const {
    if (row >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[row];
    if (r.kind == Row::Kind::WindowSize) {
        std::vector<Option> out;
        for (const auto& [w, h] : WindowSizes()) {
            out.push_back({.value = fmt::format("{}x{}", w, h), .label = fmt::format("{} × {}", w, h)});
        }
        return out;
    }
    if (r.kind == Row::Kind::WindowMode) {
        return WindowModeOptions();
    }
    if (r.kind == Row::Kind::Setting && r.desc != nullptr) {
        return Options(*r.desc);
    }
    return {};
}

int SettingsMenu::CurrentOption(size_t row, const std::vector<Option>& options) const {
    std::string now;
    const auto& r = rows_[row];
    if (r.kind == Row::Kind::WindowSize) {
        const auto cfg = service_.Current();
        now = fmt::format("{}x{}", cfg.window_width, cfg.window_height);
    } else if (r.kind == Row::Kind::WindowMode) {
        now = WindowModeNow();
    } else if (r.desc != nullptr) {
        const Value v = service_.Get(*r.desc);
        if (const auto* s = std::get_if<std::string>(&v)) {
            now = *s;
        }
    }
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i].value == now) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SettingsMenu::OpenList() {
    const auto options = RowOptions(focus_);
    if (options.empty()) {
        return;
    }
    if (rows_[focus_].desc != nullptr) {
        const auto a = service_.Available(*rows_[focus_].desc);
        if (!a.available) {
            status_ = std::string(T(a.why_en, a.why_ru));
            status_warn_ = true;
            return;
        }
    } else if (rows_[focus_].kind == Row::Kind::WindowSize || rows_[focus_].kind == Row::Kind::WindowMode) {
        if (const Desc* dw = Settings::Find("display.width"); dw != nullptr && !service_.Available(*dw).available) {
            const auto a = service_.Available(*dw);
            status_ = std::string(T(a.why_en, a.why_ru));
            status_warn_ = true;
            return;
        }
    }
    modal_ = Modal::List;
    list_row_ = focus_;
    list_item_ = std::max(0, CurrentOption(focus_, options));
    list_first_ = std::clamp(list_item_ - kListVisible / 2, 0,
                             std::max(0, static_cast<int>(options.size()) - kListVisible));
    hover_item_ = -1;
}

void SettingsMenu::ChooseListItem(int index) {
    const auto options = RowOptions(list_row_);
    modal_ = Modal::None;
    if (index < 0 || index >= static_cast<int>(options.size())) {
        return;
    }
    const auto& o = options[static_cast<size_t>(index)];
    if (!o.ok) {
        status_ = o.why;
        status_warn_ = true;
        return;
    }
    const auto& r = rows_[list_row_];
    if (r.kind == Row::Kind::WindowSize) {
        u32 w = 0, h = 0; // "<w>x<h>"
        const char* end = o.value.data() + o.value.size();
        const auto rw = std::from_chars(o.value.data(), end, w);
        if (rw.ec == std::errc{} && rw.ptr != end && *rw.ptr == 'x' &&
            std::from_chars(rw.ptr + 1, end, h).ec == std::errc{} && w != 0 && h != 0) {
            SetWindowSize(w, h);
        }
    } else if (r.kind == Row::Kind::WindowMode) {
        SetWindowMode(o.value);
    } else if (r.desc != nullptr) {
        SetValue(*r.desc, Value{o.value});
    }
}

void SettingsMenu::Adjust(int dir) {
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    if (row.kind == Row::Kind::WindowSize) {
        StepWindowSize(dir);
        return;
    }
    if (row.kind == Row::Kind::WindowMode) {
        StepWindowMode(dir);
        return;
    }
    if (row.kind != Row::Kind::Setting || row.desc == nullptr) {
        return;
    }
    const Desc& d = *row.desc;
    if (d.type == Type::Binding) {
        slot_ = std::clamp(slot_ + dir, 0, 1);
        return;
    }
    if (const auto a = service_.Available(d); !a.available) {
        status_ = std::string(T(a.why_en, a.why_ru));
        status_warn_ = true;
        return;
    }
    const Value v = service_.Get(d);
    switch (d.type) {
    case Type::Bool:
        SetValue(d, Value{dir > 0});
        break;
    case Type::Int: {
        const double x = std::clamp(static_cast<double>(std::get<s64>(v)) + dir * d.step, d.ui_min, d.ui_max);
        SetValue(d, Value{static_cast<s64>(std::lround(x))});
        break;
    }
    case Type::Float: {
        double x = std::get<double>(v) + dir * d.step;
        x = std::round(std::round(x / d.step) * d.step * 1e6) / 1e6; // 0.35, not 0.35000000000000003
        SetValue(d, Value{std::clamp(x, d.ui_min, d.ui_max)});
        break;
    }
    case Type::Choice:
    case Type::Text: {
        // the next choice that can be chosen (a choice this system cannot run is skipped, with its reason shown when
        // nothing else is left)
        const auto options = Options(d);
        const int n = static_cast<int>(options.size());
        if (n == 0) {
            break;
        }
        const int cur = CurrentOption(focus_, options);
        int idx = cur < 0 ? 0 : cur;
        for (int i = 0; i < n; ++i) {
            idx = ((idx + dir) % n + n) % n;
            if (options[static_cast<size_t>(idx)].ok || idx == cur) {
                break;
            }
        }
        if (idx == cur) {
            for (const auto& o : options) {
                if (!o.ok) {
                    status_ = o.why;
                    status_warn_ = true;
                    break;
                }
            }
            break;
        }
        if (options[static_cast<size_t>(idx)].ok) {
            SetValue(d, Value{options[static_cast<size_t>(idx)].value});
        }
        break;
    }
    case Type::Binding:
        break;
    }
}

void SettingsMenu::Activate() {
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    switch (row.kind) {
    case Row::Kind::WindowSize:
    case Row::Kind::WindowMode:
        OpenList();
        return;
    case Row::Kind::ResetPage:
        modal_ = Modal::ConfirmReset;
        modal_button_ = 1; // "cancel" first: a reset is not a single key away
        return;
    case Row::Kind::Header:
    case Row::Kind::Info:
        return;
    case Row::Kind::Command:
        if (armed_ == row.tag && env_.now_ns - armed_ns_ < kConfirmCommandNs) {
            command_ = row.tag;
            armed_.clear();
            status_ = std::string(T("Done: the cache is cleared at the next start.",
                                    "Готово: кэш будет очищен при следующем запуске."));
            status_warn_ = false;
        } else {
            armed_ = row.tag;
            armed_ns_ = env_.now_ns;
            status_ = std::string(T("Press once more to confirm.", "Нажмите ещё раз для подтверждения."));
            status_warn_ = true;
        }
        return;
    case Row::Kind::Setting:
        break;
    }
    if (row.desc == nullptr) {
        return;
    }
    const Desc& d = *row.desc;
    if (d.type == Type::Binding) {
        capture_ = Capture{static_cast<Input::Action>(d.action), static_cast<size_t>(slot_)};
        status_.clear();
        return;
    }
    if (d.type == Type::Bool) {
        if (service_.Available(d).available) {
            SetValue(d, Value{!std::get<bool>(service_.Get(d))});
        } else {
            Adjust(1); // reports why
        }
        return;
    }
    if (d.type == Type::Choice || d.type == Type::Text) {
        OpenList();
    }
}

void SettingsMenu::ResetFocused() {
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    if (row.kind == Row::Kind::WindowSize) {
        const Desc* dw = Settings::Find("display.width");
        const Desc* dh = Settings::Find("display.height");
        if (dw != nullptr && dh != nullptr) {
            SetWindowSize(static_cast<u32>(std::get<s64>(service_.Default(*dw))),
                          static_cast<u32>(std::get<s64>(service_.Default(*dh))));
        }
        return;
    }
    if (row.kind == Row::Kind::WindowMode) {
        // both settings back to SecondSonPC.toml's (a file's "exclusive" included), confirmed as any display change
        const Desc* fs = Settings::Find("display.fullscreen");
        const Desc* fm = Settings::Find("display.fullscreenMode");
        if (fs != nullptr && fm != nullptr && service_.Available(*fs).available) {
            const std::pair<const Desc*, Value> changes[] = {{fs, service_.Default(*fs)}, {fm, service_.Default(*fm)}};
            BeginDisplayChange();
            EndDisplayChange(service_.SetMany(changes) == Settings::SetResult::Ok);
        }
        return;
    }
    if (row.kind == Row::Kind::Setting && row.desc != nullptr && !row.desc->table.empty()) {
        SetValue(*row.desc, service_.Default(*row.desc));
    }
}

void SettingsMenu::ResetPageSettings() {
    std::vector<std::pair<const Desc*, Value>> changes;
    const auto add = [&](const Desc* d) {
        if (d != nullptr && d->get != nullptr && !d->table.empty() && service_.Available(*d).available) {
            changes.emplace_back(d, service_.Default(*d));
        }
    };
    for (const auto& r : rows_) {
        if (r.kind == Row::Kind::Setting) {
            add(r.desc);
        } else if (r.kind == Row::Kind::WindowSize) {
            add(Settings::Find("display.width"));
            add(Settings::Find("display.height"));
        } else if (r.kind == Row::Kind::WindowMode) {
            add(Settings::Find("display.fullscreen"));
            add(Settings::Find("display.fullscreenMode"));
        }
    }
    if (changes.empty()) {
        return;
    }
    const bool display = page_ == Page::Display;
    if (display) {
        BeginDisplayChange();
    }
    std::string error;
    const auto result = service_.SetMany(changes, &error);
    if (display) {
        EndDisplayChange(result == Settings::SetResult::Ok);
    }
    if (result == Settings::SetResult::Invalid || result == Settings::SetResult::Unavailable) {
        status_ = error;
        status_warn_ = true;
    } else {
        status_ = fmt::format("{} «{}»", T("Reset:", "Сброшено:"), PageLabel(page_, env_.ru));
        status_warn_ = false;
    }
}

void SettingsMenu::ModalAccept(int button) {
    if (modal_ == Modal::ConfirmDisplay) {
        if (button == 0) {
            modal_ = Modal::None;
            display_before_.clear();
            status_ = std::string(T("Display settings kept.", "Параметры экрана сохранены."));
            status_warn_ = false;
        } else {
            RevertDisplay();
        }
    } else if (modal_ == Modal::ConfirmReset) {
        modal_ = Modal::None;
        if (button == 0) {
            ResetPageSettings();
        }
    }
}

void SettingsMenu::OnNav(Nav nav) {
    if (!open_) {
        return;
    }
    if (modal_ == Modal::List) {
        const int n = static_cast<int>(RowOptions(list_row_).size());
        switch (nav) {
        case Nav::Up:
            list_item_ = std::max(0, list_item_ - 1);
            break;
        case Nav::Down:
            list_item_ = std::min(n - 1, list_item_ + 1);
            break;
        case Nav::PageUp:
            list_item_ = std::max(0, list_item_ - kListVisible);
            break;
        case Nav::PageDown:
            list_item_ = std::min(n - 1, list_item_ + kListVisible);
            break;
        case Nav::Accept:
            ChooseListItem(list_item_);
            return;
        case Nav::Back:
        case Nav::PrevTab:
        case Nav::NextTab:
            modal_ = Modal::None; // closed without choosing
            return;
        default:
            return;
        }
        list_first_ = std::clamp(list_first_, std::max(0, list_item_ - kListVisible + 1), list_item_);
        return;
    }
    if (modal_ != Modal::None) {
        switch (nav) {
        case Nav::Left:
        case Nav::Up:
            modal_button_ = 0;
            break;
        case Nav::Right:
        case Nav::Down:
            modal_button_ = 1;
            break;
        case Nav::Accept:
            ModalAccept(modal_button_);
            break;
        case Nav::Back:
            ModalAccept(1);
            break;
        default:
            break;
        }
        return;
    }
    if (capture_) {
        if (nav == Nav::Back) {
            capture_.reset();
            status_ = std::string(T("Not changed.", "Без изменений."));
            status_warn_ = false;
        }
        return;
    }
    const int page_rows = std::max(1, static_cast<int>(view_h_ / kRowH) - 1);
    switch (nav) {
    case Nav::Up:
        MoveFocus(-1);
        break;
    case Nav::Down:
        MoveFocus(1);
        break;
    case Nav::PageUp:
        MoveFocus(-page_rows);
        break;
    case Nav::PageDown:
        MoveFocus(page_rows);
        break;
    case Nav::Left:
        Adjust(-1);
        break;
    case Nav::Right:
        Adjust(1);
        break;
    case Nav::Accept:
        Activate();
        break;
    case Nav::Back:
        close_request_ = true;
        break;
    case Nav::PrevTab:
    case Nav::NextTab: {
        const auto n = static_cast<int>(kPageCount);
        const int idx = static_cast<int>(page_);
        SelectPage(kPages[static_cast<size_t>(((idx + (nav == Nav::NextTab ? 1 : -1)) % n + n) % n)]);
        break;
    }
    case Nav::Reset:
        ResetFocused();
        break;
    case Nav::Clear:
        if (focus_ < rows_.size() && rows_[focus_].desc != nullptr && rows_[focus_].desc->type == Type::Binding) {
            const Desc& d = *rows_[focus_].desc;
            auto input = service_.Current().input;
            Input::ClearBinding(input, static_cast<Input::Action>(d.action), static_cast<size_t>(slot_));
            SetValue(d, Value{input.bindings[static_cast<size_t>(d.action)]});
        }
        break;
    }
}

void SettingsMenu::OnCaptureInput(Input::InputCode input) {
    if (!capture_) {
        return;
    }
    const auto [action, slot] = *capture_;
    capture_.reset();
    if (Input::IsReservedInput(input)) {
        status_ = fmt::format("{}: {}", BindingText(input),
                              T("a SecondSonPC hotkey (F10 FPS, F11 full screen) - not assigned",
                                "горячая клавиша SecondSonPC (F10 FPS, F11 полный экран) — не назначена"));
        status_warn_ = true;
        return;
    }
    auto settings = service_.Current().input;
    Input::AssignBinding(settings, action, slot, input);
    const Desc* d = BindingDesc(action);
    if (d == nullptr) {
        return;
    }
    SetValue(*d, Value{settings.bindings[static_cast<size_t>(action)]});
    std::string others;
    for (const auto other : Input::ActionsUsing(settings, input)) {
        if (other != action && !Input::MayShareInput(other, action)) {
            others += fmt::format("{}«{}»", others.empty() ? "" : ", ", Settings::ActionLabel(other, env_.ru));
        }
    }
    status_ = others.empty()
                  ? fmt::format("{} — {}", Settings::ActionLabel(action, env_.ru), BindingText(input))
                  : fmt::format("{} {} {}", BindingText(input), T("is also assigned to", "также назначена на"), others);
    status_warn_ = !others.empty();
}

void SettingsMenu::Tick(u64 now_ns) {
    env_.now_ns = now_ns;
    if (modal_ == Modal::ConfirmDisplay && now_ns >= modal_deadline_ns_) {
        RevertDisplay();
    }
    if (!armed_.empty() && now_ns - armed_ns_ >= kConfirmCommandNs) {
        armed_.clear();
        if (status_ == T("Press once more to confirm.", "Нажмите ещё раз для подтверждения.")) {
            status_.clear();
        }
    }
}

bool SettingsMenu::NeedsTicks() const {
    return open_ && (modal_ == Modal::ConfirmDisplay || !armed_.empty());
}

void SettingsMenu::OnMouseMove(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (dragging_thumb_) {
        const float range = track_.h - thumb_.h;
        const float max_scroll = std::max(0.0f, content_h_ - view_h_);
        if (range > 0) {
            scroll_ = std::clamp((y - drag_grab_ - track_.y) / range, 0.0f, 1.0f) * max_scroll;
            scroll_to_focus_ = false;
        }
        return;
    }
    if (dragging_row_ >= 0) {
        for (const auto& h : hits_) {
            if (h.kind == Hit::Kind::Row && h.index == dragging_row_ && h.part == Hit::Part::Track &&
                static_cast<size_t>(dragging_row_) < rows_.size() && rows_[static_cast<size_t>(dragging_row_)].desc) {
                const Desc& d = *rows_[static_cast<size_t>(dragging_row_)].desc;
                const double f = std::clamp((x - h.rect.x) / std::max(1.0f, h.rect.w), 0.0f, 1.0f);
                double v = d.ui_min + f * (d.ui_max - d.ui_min);
                v = std::round(std::round(v / d.step) * d.step * 1e6) / 1e6;
                v = std::clamp(v, d.ui_min, d.ui_max);
                if (d.type == Type::Int) {
                    SetValue(d, Value{static_cast<s64>(std::lround(v))});
                } else {
                    SetValue(d, Value{v});
                }
                break;
            }
        }
        return;
    }
    hover_row_ = hover_page_ = hover_item_ = hover_close_ = -1;
    for (const auto& h : hits_) {
        if (!h.rect.Contains(x, y)) {
            continue;
        }
        switch (h.kind) {
        case Hit::Kind::Row:
            hover_row_ = h.index;
            break;
        case Hit::Kind::Page:
            hover_page_ = h.index;
            break;
        case Hit::Kind::Close:
            hover_close_ = h.index;
            break;
        case Hit::Kind::ListItem:
        case Hit::Kind::ModalButton:
            hover_item_ = h.index;
            break;
        default:
            break;
        }
    }
    if (modal_ == Modal::List && hover_item_ >= 0) {
        list_item_ = hover_item_;
    } else if (modal_ != Modal::None && modal_ != Modal::List && hover_item_ >= 0) {
        modal_button_ = hover_item_;
    }
    // the row under the pointer is the chosen one (its description below the list follows it)
    if (modal_ == Modal::None && !capture_ && hover_row_ >= 0 && Focusable(static_cast<size_t>(hover_row_)) &&
        focus_ != static_cast<size_t>(hover_row_)) {
        focus_ = static_cast<size_t>(hover_row_);
        slot_ = 0;
        status_.clear();
    }
}

void SettingsMenu::OnMouseButton(float x, float y, bool down) {
    if (!down) {
        dragging_row_ = -1;
        dragging_thumb_ = false;
        return;
    }
    const Hit* hit = nullptr;
    for (const auto& h : hits_) {
        if (h.rect.Contains(x, y)) {
            hit = &h; // the last one drawn is on top
        }
    }
    if (modal_ == Modal::List) {
        if (hit != nullptr && hit->kind == Hit::Kind::ListItem) {
            ChooseListItem(hit->index);
        } else {
            modal_ = Modal::None; // a click outside the list closes it without choosing
        }
        return;
    }
    if (modal_ != Modal::None) {
        if (hit != nullptr && hit->kind == Hit::Kind::ModalButton) {
            ModalAccept(hit->index);
        }
        return;
    }
    if (capture_ || hit == nullptr) {
        return; // the host sends clicks as inputs to bind while capturing
    }
    switch (hit->kind) {
    case Hit::Kind::Close:
        close_request_ = true;
        return;
    case Hit::Kind::Page:
        SelectPage(kPages[static_cast<size_t>(hit->index)]);
        return;
    case Hit::Kind::ScrollThumb:
        dragging_thumb_ = true;
        drag_grab_ = y - thumb_.y;
        return;
    case Hit::Kind::ScrollTrack:
        scroll_ += (y < thumb_.y ? -1.0f : 1.0f) * std::max(kRowH, view_h_ - kRowH);
        scroll_to_focus_ = false;
        return;
    case Hit::Kind::ModalButton:
    case Hit::Kind::ListItem:
    case Hit::Kind::ListOutside:
        return;
    case Hit::Kind::Row:
        break;
    }
    if (!Focusable(static_cast<size_t>(hit->index))) {
        return;
    }
    if (focus_ != static_cast<size_t>(hit->index)) {
        focus_ = static_cast<size_t>(hit->index);
        slot_ = 0;
    }
    status_.clear();
    switch (hit->part) {
    case Hit::Part::Prev:
        Adjust(-1);
        break;
    case Hit::Part::Next:
        Adjust(1);
        break;
    case Hit::Part::Value:
    case Hit::Part::Button:
        Activate();
        break;
    case Hit::Part::Track:
        if (rows_[focus_].desc != nullptr && service_.Available(*rows_[focus_].desc).available) {
            dragging_row_ = hit->index;
            OnMouseMove(x, y);
        } else {
            Adjust(1); // reports why
        }
        break;
    case Hit::Part::Slot0:
    case Hit::Part::Slot1:
        slot_ = hit->part == Hit::Part::Slot0 ? 0 : 1;
        Activate();
        break;
    case Hit::Part::Body:
        break;
    }
}

void SettingsMenu::OnWheel(float notches) {
    if (notches == 0.0f || capture_) {
        return;
    }
    if (modal_ == Modal::List) {
        const int n = static_cast<int>(RowOptions(list_row_).size());
        list_first_ = std::clamp(list_first_ - static_cast<int>(std::lround(notches)), 0, std::max(0, n - kListVisible));
        return;
    }
    if (modal_ != Modal::None) {
        return;
    }
    scroll_ -= notches * 3.0f * kRowH;
    scroll_to_focus_ = false;
}

std::string SettingsMenu::BindingText(const Input::InputCode& input) const {
    std::string name = Input::InputName(input);
    if (!env_.ru) {
        return name;
    }
    static constexpr std::pair<std::string_view, std::string_view> kRu[] = {
        {"Mouse Left", "ЛКМ"},           {"Mouse Right", "ПКМ"},           {"Mouse Middle", "СКМ"},
        {"Mouse X1", "Мышь X1"},         {"Mouse X2", "Мышь X2"},          {"Wheel Up", "Колесо вверх"},
        {"Wheel Down", "Колесо вниз"},   {"Wheel Left", "Колесо влево"},   {"Wheel Right", "Колесо вправо"},
        {"Space", "Пробел"},             {"Left Shift", "Левый Shift"},    {"Right Shift", "Правый Shift"},
        {"Left Ctrl", "Левый Ctrl"},     {"Right Ctrl", "Правый Ctrl"},    {"Left Alt", "Левый Alt"},
        {"Right Alt", "Правый Alt"},     {"Return", "Enter"},              {"Escape", "Esc"},
        {"Up", "↑"},                     {"Down", "↓"},                    {"Left", "←"},
        {"Right", "→"},
    };
    for (const auto& [en, ru] : kRu) {
        if (name == en) {
            return std::string(ru);
        }
    }
    return name;
}

std::string SettingsMenu::ValueText(const Desc& desc, const Value& value) const {
    switch (desc.type) {
    case Type::Bool:
        return std::string(std::get<bool>(value) ? T("On", "Вкл.") : T("Off", "Выкл."));
    case Type::Int: {
        const auto x = std::get<s64>(value);
        if (desc.id == "shaders.threads" && x == 0) {
            return std::string(T("Auto", "Авто"));
        }
        return desc.unit.empty() ? std::to_string(x) : fmt::format("{} {}", x, desc.unit);
    }
    case Type::Float: {
        std::string s = fmt::format("{:.{}f}", std::get<double>(value), desc.step < 0.1 ? 2 : 1);
        if (env_.ru) {
            s = Replace(s, ".", ",");
        }
        return desc.unit == "x" ? s + "×" : s;
    }
    case Type::Choice: {
        const auto label = Settings::ChoiceLabel(desc, std::get<std::string>(value), env_.ru);
        if (label.empty() && desc.id == "display.resolution") {
            return Replace(std::get<std::string>(value), "x", " × "); // (a size outside the list: "1366 × 768")
        }
        return label.empty() ? std::get<std::string>(value) : std::string(label);
    }
    case Type::Text: {
        const auto& s = std::get<std::string>(value);
        if (!s.empty()) {
            return s;
        }
        return std::string(desc.id == "display.monitor" ? T("Primary display", "Основной монитор")
                                                        : T("System default", "По умолчанию"));
    }
    case Type::Binding:
        break;
    }
    return {};
}

std::string SettingsMenu::Inactive(size_t row) const {
    if (row >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[row];
    const auto cfg = service_.Current();
    if (r.kind == Row::Kind::WindowSize) {
        return cfg.fullscreen ? std::string(T("Used in windowed mode (full screen is on).",
                                              "Действует в оконном режиме (сейчас включён полноэкранный)."))
                              : std::string{};
    }
    if (r.desc == nullptr) {
        return {};
    }
    const Desc& d = *r.desc;
    if (!d.requires_on.empty()) {
        if (const Desc* dep = Settings::Find(d.requires_on)) {
            const Value dv = service_.Get(*dep);
            if (std::holds_alternative<bool>(dv) && !std::get<bool>(dv)) {
                return fmt::format("{} «{}».", T("Matters while this is on:", "Действует, когда включено"),
                                   env_.ru ? dep->ru : dep->en);
            }
        }
    }
    if ((d.id == "upscaling.quality" || d.id == "upscaling.sharpness" || d.id == "upscaling.frameGeneration") &&
        cfg.pc.upscaler == "off" && service_.Available(d).available) {
        return std::string(T("Works with DLSS or FSR chosen as the upscaling technology.",
                             "Работает, когда выбрана технология масштабирования DLSS или FSR."));
    }
    return {};
}

std::string SettingsMenu::ChosenWhy(const Desc& desc) const {
    if (desc.type != Type::Choice) {
        return {};
    }
    const Value v = service_.Get(desc);
    const auto* now = std::get_if<std::string>(&v);
    if (now == nullptr) {
        return {};
    }
    for (const auto& o : Options(desc)) {
        if (o.value == *now) {
            return o.ok ? std::string{} : o.why;
        }
    }
    return std::string(T("not offered by this build (it was set in the settings file)",
                         "не поддерживается этой сборкой (задано в файле настроек)"));
}

std::string SettingsMenu::HelpText(size_t row, Color& color) const {
    color = kText2;
    if (row >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[row];
    std::string help;
    const auto add = [&help](std::string_view s) {
        if (!s.empty()) {
            help += help.empty() ? "" : " ";
            help += s;
        }
    };
    if (capture_) {
        color = kText;
        return fmt::format("{} «{}». {}", T("Press a key or a mouse button for", "Нажмите клавишу или кнопку мыши для"),
                           Settings::ActionLabel(capture_->action, env_.ru), T("Esc cancels.", "Esc — отмена."));
    }
    if (r.desc != nullptr) {
        const Desc& d = *r.desc;
        if (const auto a = service_.Available(d); !a.available) {
            color = kRestart;
            return fmt::format("{} {}", T("Unavailable:", "Недоступно:"), T(a.why_en, a.why_ru));
        }
        add(T(d.help_en, d.help_ru));
        if (d.type == Type::Binding) {
            add(T("Enter: assign · Backspace: clear the slot · Del: default (the way back to Esc, which cancels an "
                  "assignment).",
                  "Enter — назначить · Backspace — очистить ячейку · Del — по умолчанию (так возвращается Esc: при "
                  "назначении он отменяет)."));
        }
        if (service_.NeedsRestart(d)) {
            color = kRestart;
            add(fmt::format("{} {}. {}", T("Now in use:", "Сейчас действует:"), ValueText(d, service_.Started(d)),
                            T("The new value takes effect after a restart.",
                              "Новое значение вступит в силу после перезапуска.")));
        } else if (service_.EffectiveApply(d) == Settings::Apply::Restart) {
            add(T("(Applied at the next start.)", "(Применяется при следующем запуске.)"));
        }
        if (d.id == "display.presentMode" && env_.present_known) {
            add(fmt::format("{} {} ({} {}).", T("Now:", "Сейчас:"), env_.present_now,
                            T("the display offers", "монитор поддерживает"), env_.present_supported));
        }
        if (d.id == "upscaling.upscaler" && !env_.upscale_reason.empty()) {
            color = kRestart;
            add(fmt::format("{} {}", T("Not running now:", "Сейчас не работает:"), env_.upscale_reason));
        }
        if (const std::string why = ChosenWhy(d); !why.empty()) {
            color = kWarn;
            add(fmt::format("{} {}.", T("The chosen value cannot run here:", "Выбранное значение здесь не работает:"),
                            why));
        }
    } else if (r.kind == Row::Kind::WindowMode) {
        add(T("Windowed: a window with a frame, its size below. Borderless: the window covers the whole display "
              "(Monitor), quick Alt+Tab. F11 switches between them while playing. A change is undone after 15 s "
              "unless kept.",
              "«Оконный» — окно с рамкой, его размер ниже. «Без рамки» — окно на весь монитор (строка «Монитор»), "
              "быстрый Alt+Tab. F11 переключает их во время игры. Изменение отменяется через 15 с, если его не "
              "подтвердить."));
        for (const auto& o : WindowModeOptions()) {
            if (o.value == WindowModeNow() && !o.ok) {
                color = kWarn;
                add(fmt::format("{} {}.", T("The chosen value cannot run here:", "Выбранное значение здесь не работает:"),
                                o.why));
            }
        }
    } else if (r.kind == Row::Kind::WindowSize) {
        add(T("The window's size when not in full screen. The game draws at its Resolution (automatic: this size, at "
              "least 1080 lines); the window scales the finished frame.",
              "Размер окна вне полноэкранного режима. Игра рисует в своём «Разрешении» (автоматически — в этом "
              "размере, не меньше 1080 строк); окно масштабирует готовый кадр."));
    } else if (r.kind == Row::Kind::ResetPage) {
        add(fmt::format("{} «{}» {}", T("Every setting of", "Все настройки раздела"), PageLabel(page_, env_.ru),
                        T("goes back to its default (SecondSonPC.toml). Asks first.",
                          "вернутся к значениям по умолчанию (SecondSonPC.toml). Сначала спросит подтверждение.")));
    } else if (r.tag == "cmd.clear_shaders") {
        add(T("The translated shaders, the list of known pipelines and the Vulkan pipeline cache are deleted at the "
              "next start: that start and the first visit of every place compile again.",
              "Переведённые шейдеры, список известных конвейеров и кэш конвейеров Vulkan удаляются при следующем "
              "запуске: этот запуск и первое посещение каждого места компилируют заново."));
    } else if (r.tag == "cmd.clear_pipelines") {
        add(T("Only the driver's compiled pipelines (vulkan.bin) are deleted at the next start; the translated shaders "
              "and the list of pipelines stay.",
              "При следующем запуске удаляются только скомпилированные драйвером конвейеры (vulkan.bin); переведённые "
              "шейдеры и список конвейеров остаются."));
    } else if (r.tag.starts_with("info.cache.")) {
        add(T("The native backend's persistent shader cache ([ShaderCache] dir).",
              "Постоянный кэш шейдеров собственного бэкенда ([ShaderCache] dir)."));
    } else if (r.tag == "info.backend") {
        add(T("Chosen at start (--gpu-backend). Settings of the other backend are not listed.",
              "Выбирается при запуске (--gpu-backend). Настройки другого бэкенда не показываются."));
    } else if (r.tag == "info.render") {
        add(T("The 3D scene's resolution now -> the game's resolution (its interface and the output). Render scale or "
              "an upscaler renders the scene smaller; the upscaler rebuilds the full picture.",
              "Разрешение 3D-сцены сейчас → разрешение игры (её интерфейс и выходной кадр). Масштаб рендеринга или "
              "апскейлер рендерят сцену меньше; апскейлер восстанавливает полную картинку."));
    } else if (r.tag == "info.settings") {
        add(T("Every change is saved at once to this file, over SecondSonPC.toml's defaults. Delete a line there to use "
              "the default again; keys this version does not know are kept.",
              "Каждое изменение сразу сохраняется в этот файл поверх умолчаний SecondSonPC.toml. Удалите строку в нём, "
              "чтобы вернуть значение по умолчанию; неизвестные этой версии ключи сохраняются."));
    } else if (r.tag == "info.device") {
        add(T("The device used last: it decides the button icons in the game's hints (Game - Button hints).",
              "Последнее использованное устройство: по нему выбираются значки кнопок в подсказках игры (Игра → "
              "Подсказки кнопок)."));
    } else if (r.tag == "info.hotkeys") {
        add(T("These keys are SecondSonPC's and cannot be assigned to the game's actions.",
              "Эти клавиши принадлежат SecondSonPC и не назначаются действиям игры."));
    }
    if (const std::string why = Inactive(row); !why.empty()) {
        add(why);
    }
    return help;
}

void SettingsMenu::Build(TextMeasure& measure, DrawList& out) {
    const float W = env_.width, H = env_.height;
    out.Reset(W, H);
    hits_.clear();
    if (!open_) {
        return;
    }
    const auto cfg = service_.Current();
    // ---- scale: the window's height against 1080 (or the system's DPI scale if larger) times [Interface] scale,
    // then whatever fits the window with a margin
    const float ui = std::clamp(static_cast<float>(cfg.pc.ui_scale) / 100.0f, 0.5f, 2.0f);
    float s = std::max(H / 1080.0f, std::max(1.0f, env_.dpi_scale) * (H >= 700.0f ? 1.0f : H / 700.0f)) * ui;
    s = std::min({s, (W - 24.0f) / kWindowW, (H - 24.0f) / kWindowH});
    s = std::max(s, 0.35f);
    scale_ = s;
    const float pw = std::floor(kWindowW * s), ph = std::floor(kWindowH * s);
    const Rect panel{std::floor((W - pw) / 2), std::floor((H - ph) / 2), pw, ph};
    const auto R = [&](float x, float y, float w, float h) { return Rect{panel.x + x * s, panel.y + y * s, w * s, h * s}; };
    const auto X = [&](float x) { return panel.x + x * s; };
    const auto Y = [&](float y) { return panel.y + y * s; };
    const float px1 = std::max(1.0f, s);
    const float m = 10.0f * s;
    out.SetSurface({panel.x - m, panel.y - m, pw + 2 * m, ph + 2 * m});
    out.SetDim(110);

    // ---- the window: a soft shadow, the panel, the game's frame marks
    for (int i = 3; i >= 1; --i) {
        const float g = static_cast<float>(i) * 2.5f * s;
        out.FillRect({panel.x - g, panel.y - g + 2 * s, pw + 2 * g, ph + 2 * g}, kShadow, 8 * s + g);
    }
    out.FillRect(panel, kPanel, 6 * s);
    out.FillRect({panel.x, Y(kHeaderH), kNavW * s, (kWindowH - kHeaderH - kFooterH) * s}, kNavBg);
    out.StrokeRect(panel, kFrame, px1, 6 * s);
    out.FillRect(R(-3, -3, 7, 7), kFrameMark);                 // the corner square of the game's panels
    out.FillRect(R(kWindowW - 150, -3, 126, 3), kFrameMark);   // and its tab over the top edge

    // ---- header: title, edition, restart notice, close
    const std::string title = Caps(T("Settings", "Настройки"));
    const float title_w = measure.Width(title, 26 * s, FontWeight::GameSlab);
    out.Text(R(24, 0, 400, kHeaderH), title, 26 * s, kAccent, Align::Left, FontWeight::GameSlab);
    {
        const std::string_view ed = "PC EDITION";
        const float ew = measure.Width(ed, 11 * s, FontWeight::Semibold) + 14 * s;
        const Rect badge{X(24) + title_w + 12 * s, Y(kHeaderH / 2 - 10), ew, 20 * s};
        out.StrokeRect(badge, kAccentLine, px1, 3 * s);
        out.Text(badge, ed, 11 * s, kAccent, Align::Center, FontWeight::Semibold);
    }
    const Rect close = R(kWindowW - 52, 14, 32, 32);
    if (hover_close_ == 0) {
        out.FillRect(close, kRowFocus, 4 * s);
    }
    {
        const float a = 6.5f * s, cx = close.x + close.w / 2, cy = close.y + close.h / 2;
        const Color c = hover_close_ == 0 ? kText : kText2;
        out.Line(cx - a, cy - a, cx + a, cy + a, c, std::max(1.0f, 1.8f * s));
        out.Line(cx - a, cy + a, cx + a, cy - a, c, std::max(1.0f, 1.8f * s));
    }
    hits_.push_back({.kind = Hit::Kind::Close, .rect = close, .index = 0});
    size_t restart_n = 0;
    for (const auto& d : Settings::All()) {
        restart_n += service_.NeedsRestart(d) ? 1 : 0;
    }
    if (restart_n > 0) {
        const std::string note = fmt::format("{}: {}", T("Restart needed", "Нужен перезапуск"), restart_n);
        const float nw = measure.Width(note, 12 * s, FontWeight::Semibold) + 30 * s;
        const Rect pill{close.x - 12 * s - nw, Y(kHeaderH / 2 - 12), nw, 24 * s};
        out.FillRect(pill, Alpha(kRestart, 0.12f), 12 * s);
        out.StrokeRect(pill, Alpha(kRestart, 0.7f), px1, 12 * s);
        // a circular arrow: a ring and its head
        const float rcx = pill.x + 13 * s, rcy = pill.y + pill.h / 2;
        out.StrokeCircle(rcx, rcy, 4.5f * s, kRestart, std::max(1.0f, 1.4f * s));
        out.FillTriangle(rcx + 2.5f * s, rcy - 6.5f * s, rcx + 6.5f * s, rcy - 4.2f * s, rcx + 2.5f * s, rcy - 1.5f * s,
                         kPanel);
        out.FillTriangle(rcx + 1.5f * s, rcy - 7 * s, rcx + 6 * s, rcy - 4.5f * s, rcx + 1.5f * s, rcy - 2 * s, kRestart);
        out.Text({pill.x + 22 * s, pill.y, pill.w - 26 * s, pill.h}, note, 12 * s, kRestart, Align::Left,
                 FontWeight::Semibold);
    }
    out.Line(panel.x, Y(kHeaderH), panel.Right(), Y(kHeaderH), kLine, px1);

    // ---- navigation
    for (size_t i = 0; i < std::size(kPages); ++i) {
        const Rect item = R(10, kHeaderH + 12 + static_cast<float>(i) * 44, kNavW - 20, 40);
        const bool selected = kPages[i] == page_;
        const bool hovered = static_cast<int>(i) == hover_page_;
        if (selected) {
            out.FillRect(item, kAccentSoft, 4 * s);
            out.FillRect({item.x, item.y + 8 * s, 3 * s, item.h - 16 * s}, kAccent, 1.5f * s);
        } else if (hovered) {
            out.FillRect(item, kRowHover, 4 * s);
        }
        const Color ic = selected ? kAccent : hovered ? kText : kText2;
        DrawPageIcon(out, kPages[i], item.x + 26 * s, item.y + item.h / 2, s, ic);
        out.Text({item.x + 48 * s, item.y, item.w - 54 * s, item.h}, PageLabel(kPages[i], env_.ru), 17 * s,
                 selected ? kText : hovered ? kText : kText2, Align::Left, FontWeight::GameItem);
        hits_.push_back({.kind = Hit::Kind::Page, .rect = item, .index = static_cast<int>(i)});
    }
    {
        const bool saving = !env_.settings_file.empty();
        out.Text(R(22, kWindowH - kFooterH - 64, kNavW - 40, 52),
                 saving ? T("Changes apply and are saved at once.", "Изменения применяются и сохраняются сразу.")
                        : T("Changes are not saved in this run.", "В этом запуске изменения не сохраняются."),
                 12 * s, saving ? kText3 : kWarn, Align::Left, FontWeight::Regular, true);
    }

    // ---- the page: title, list, description
    out.Text(R(kListX + 12, kHeaderH + 10, 560, 34), Caps(PageLabel(page_, env_.ru)), 20 * s, kText, Align::Left,
             FontWeight::GameSlab);
    const float list_bottom = kWindowH - kFooterH - kHelpH;
    view_h_ = list_bottom - kListTop - 6;
    const int n = static_cast<int>(rows_.size());
    std::vector<float> ry(rows_.size()), rh(rows_.size());
    float y = 0;
    for (int i = 0; i < n; ++i) {
        const auto& r = rows_[static_cast<size_t>(i)];
        const float h = r.kind == Row::Kind::Header ? kGroupH : r.kind == Row::Kind::ResetPage ? kResetH : kRowH;
        ry[static_cast<size_t>(i)] = y;
        rh[static_cast<size_t>(i)] = h;
        y += h;
    }
    content_h_ = y;
    const float max_scroll = std::max(0.0f, content_h_ - view_h_);
    if (scroll_to_focus_ && focus_ < rows_.size()) {
        float top = ry[focus_];
        if (focus_ > 0 && rows_[focus_ - 1].kind == Row::Kind::Header) {
            top = ry[focus_ - 1]; // the group's name stays visible with its first row
        }
        const float bottom = ry[focus_] + rh[focus_];
        if (top < scroll_) {
            scroll_ = top;
        } else if (bottom > scroll_ + view_h_) {
            scroll_ = bottom - view_h_;
        }
        scroll_to_focus_ = false;
    }
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll);

    const auto conflicts = Input::FindConflicts(cfg.input);
    const auto in_conflict = [&conflicts](Input::Action a, const Input::InputCode& in) {
        return std::ranges::any_of(conflicts, [&](const Input::BindingConflict& c) {
            return (c.first == a || c.second == a) && c.input == in;
        });
    };

    const Rect list_clip = R(kListX - 2, kListTop, kListR - kListX + 4, view_h_ + 2);
    Rect list_anchor{};
    out.PushClip(list_clip);
    for (int i = 0; i < n; ++i) {
        const auto& row = rows_[static_cast<size_t>(i)];
        const float top = kListTop + ry[static_cast<size_t>(i)] - scroll_;
        const float h = rh[static_cast<size_t>(i)];
        if (top + h < kListTop - 2 || top > kListTop + view_h_ + 2) {
            continue;
        }
        if (row.kind == Row::Kind::Header) {
            const std::string name = Caps(T(row.en, row.ru));
            const float tw = measure.Width(name, 11.5f * s, FontWeight::Semibold);
            out.Text(R(kListX + 12, top + 12, kListR - kListX - 24, 20), name, 11.5f * s, kText2, Align::Left,
                     FontWeight::Semibold);
            const float ly = Y(top + 22);
            if (X(kListX + 12) + tw + 10 * s < X(kListR - 12)) {
                out.Line(X(kListX + 12) + tw + 10 * s, ly, X(kListR - 12), ly, kLine, px1);
            }
            continue;
        }
        const Rect rr = R(kListX, top + (row.kind == Row::Kind::ResetPage ? 8 : 0), kListR - kListX,
                          row.kind == Row::Kind::ResetPage ? h - 8 : h);
        const float cy = rr.y + rr.h / 2;
        const bool focused = static_cast<size_t>(i) == focus_;
        if (focused) {
            out.FillRect(rr, kRowFocus, 4 * s);
            out.FillRect({rr.x, rr.y + 8 * s, 3 * s, rr.h - 16 * s}, kAccent, 1.5f * s);
        } else if (i == hover_row_) {
            out.FillRect(rr, kRowHover, 4 * s);
        }
        hits_.push_back({.kind = Hit::Kind::Row, .rect = rr, .index = i, .part = Hit::Part::Body});

        const Rect ctrl{X(kListR - 12 - kCtrlW), rr.y, kCtrlW * s, rr.h};
        float label_right = ctrl.x - 12 * s;
        const Desc* d = row.desc;
        const bool available = d == nullptr || service_.Available(*d).available;
        const bool inactive = !Inactive(static_cast<size_t>(i)).empty();
        const bool dimmed = !available || inactive;
        if (row.kind == Row::Kind::Info) {
            label_right = X(kListX + 12) + 220 * s; // an information row: the value takes the rest
        }

        // after-restart badge; the changed-from-default dot
        if (d != nullptr && service_.NeedsRestart(*d)) {
            const auto tag = T("after restart", "после перезапуска");
            const float tw = measure.Width(tag, 11 * s, FontWeight::Semibold) + 14 * s;
            const Rect tr{label_right - tw, cy - 10 * s, tw, 20 * s};
            out.StrokeRect(tr, Alpha(kRestart, 0.8f), px1, 10 * s);
            out.Text(tr, tag, 11 * s, kRestart, Align::Center, FontWeight::Semibold);
            label_right = tr.x - 8 * s;
        }
        if (d != nullptr && service_.IsOverridden(*d)) {
            out.FillCircle(X(kListX + 12), cy, 2.5f * s, Alpha(kAccent, 0.9f));
        }
        std::string label = d != nullptr ? std::string(env_.ru ? d->ru : d->en) : std::string(T(row.en, row.ru));
        if (row.kind == Row::Kind::ResetPage) {
            label = fmt::format("{} «{}»", label, PageLabel(page_, env_.ru));
        }
        out.Text({X(kListX + 20), rr.y, std::max(10.0f, label_right - X(kListX + 20)), rr.h}, label, 16 * s,
                 dimmed ? kText3 : kText, Align::Left, FontWeight::GameItem);

        // the control
        const auto button = [&](std::string_view text, bool primary) {
            const float bw = std::max(110 * s, measure.Width(text, 13 * s, FontWeight::Semibold) + 28 * s);
            const Rect b{ctrl.Right() - bw, cy - 15 * s, bw, 30 * s};
            if (primary) {
                out.FillRect(b, kAccent, 4 * s);
            } else {
                out.FillRect(b, kControl, 4 * s);
                out.StrokeRect(b, focused ? kAccentLine : kControlLine, px1, 4 * s);
            }
            out.Text(b, text, 13 * s, primary ? kOnAccent : kText, Align::Center, FontWeight::Semibold);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = b, .index = i, .part = Hit::Part::Button});
        };
        const auto dropdown = [&](const std::string& text, Color tc) {
            const Rect b{ctrl.x, cy - 15 * s, ctrl.w, 30 * s};
            const bool open = modal_ == Modal::List && list_row_ == static_cast<size_t>(i);
            out.FillRect(b, kControl, 4 * s);
            out.StrokeRect(b, open || focused ? kAccentLine : (i == hover_row_ ? kControlHover : kControlLine), px1,
                           4 * s);
            out.Text({b.x + 10 * s, b.y, b.w - 34 * s, b.h}, text, 14 * s, tc, Align::Left, FontWeight::Regular);
            Chevron(out, b.Right() - 15 * s, cy, s, dimmed ? kText3 : kText2, open);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = b, .index = i, .part = Hit::Part::Value});
            if (open) {
                list_anchor = b;
            }
        };
        switch (row.kind) {
        case Row::Kind::Header:
            break;
        case Row::Kind::Info: {
            std::string text;
            if (row.tag == "info.backend") {
                text = env_.backend_label;
            } else if (row.tag == "info.device") {
                text = env_.device_label;
            } else if (row.tag == "info.hotkeys") {
                text = std::string(T("F10 FPS counter · F11 full screen", "F10 — счётчик FPS · F11 — полный экран"));
            } else if (row.tag == "info.render") {
                text = env_.render_label.empty() ? std::string("1920 × 1080") : env_.render_label;
            } else if (row.tag == "info.cache.size") {
                text = env_.shader_cache_size;
            } else if (row.tag == "info.cache.content") {
                text = env_.shader_cache_content;
            } else if (row.tag == "info.cache.run") {
                text = env_.shader_cache_run;
            } else if (row.tag == "info.settings") {
                text = env_.settings_file.empty() ? std::string(T("not saved in this run", "не сохраняются в этом запуске"))
                                                  : env_.settings_file;
            }
            out.Text({label_right + 8 * s, rr.y, ctrl.Right() - label_right - 8 * s, rr.h}, text, 14 * s, kText2,
                     Align::Right);
            break;
        }
        case Row::Kind::Command: {
            const bool armed = armed_ == row.tag && env_.now_ns - armed_ns_ < kConfirmCommandNs;
            button(armed ? T("Confirm", "Подтвердить") : T("Clear", "Очистить"), armed);
            break;
        }
        case Row::Kind::ResetPage:
            button(T("Reset", "Сбросить"), false);
            break;
        case Row::Kind::WindowSize: {
            const bool ok = service_.Available(*Settings::Find("display.width")).available;
            dropdown(fmt::format("{} × {}", cfg.window_width, cfg.window_height), ok && !inactive ? kText : kText3);
            break;
        }
        case Row::Kind::WindowMode: {
            const bool ok = service_.Available(*Settings::Find("display.fullscreen")).available;
            const auto options = WindowModeOptions();
            const std::string now = WindowModeNow();
            std::string text = now;
            bool runs = true;
            for (const auto& o : options) {
                if (o.value == now) {
                    text = o.label;
                    runs = o.ok;
                }
            }
            dropdown(text, !ok ? kText3 : runs ? kText : kWarn);
            break;
        }
        case Row::Kind::Setting: {
            if (d == nullptr) {
                break;
            }
            if (!available) {
                const auto tag = T("Unavailable", "Недоступно");
                const float tw = measure.Width(tag, 12 * s, FontWeight::Semibold) + 20 * s;
                const Rect tr{ctrl.Right() - tw, cy - 11 * s, tw, 22 * s};
                out.FillRect(tr, kKeycap, 11 * s);
                out.Text(tr, tag, 12 * s, kText3, Align::Center, FontWeight::Semibold);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = ctrl, .index = i, .part = Hit::Part::Value});
                break;
            }
            const Value v = service_.Get(*d);
            switch (d->type) {
            case Type::Bool: {
                const bool on = std::get<bool>(v);
                const Rect pill{ctrl.Right() - 42 * s, cy - 11 * s, 42 * s, 22 * s};
                const float a = dimmed ? 0.45f : 1.0f;
                out.FillRect(pill, on ? Alpha(kAccent, a) : kTrack, 11 * s);
                out.FillCircle(on ? pill.Right() - 11 * s : pill.x + 11 * s, cy, 8 * s,
                               on ? Alpha(kOnAccent, std::max(a, 0.8f)) : Alpha(kKnob, a));
                out.Text({ctrl.x, rr.y, ctrl.w - 52 * s, rr.h}, ValueText(*d, v), 13 * s, dimmed ? kText3 : kText2,
                         Align::Right);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = ctrl, .index = i, .part = Hit::Part::Value});
                break;
            }
            case Type::Int:
            case Type::Float: {
                const double x = std::holds_alternative<s64>(v) ? static_cast<double>(std::get<s64>(v))
                                                                : std::get<double>(v);
                const double f = std::clamp((x - d->ui_min) / std::max(1e-9, d->ui_max - d->ui_min), 0.0, 1.0);
                const float tx = ctrl.x + 8 * s, tw = ctrl.w - 86 * s;
                const float kx = tx + static_cast<float>(tw * f);
                out.FillRect({tx, cy - 2 * s, tw, 4 * s}, kTrack, 2 * s);
                out.FillRect({tx, cy - 2 * s, kx - tx, 4 * s}, dimmed ? kText3 : kAccent, 2 * s);
                if (focused) {
                    out.FillCircle(kx, cy, 10 * s, kAccentSoft);
                }
                out.FillCircle(kx, cy, 7 * s, dimmed ? kText3 : kKnob);
                const Rect vb{ctrl.Right() - 66 * s, cy - 13 * s, 66 * s, 26 * s};
                out.FillRect(vb, kControl, 4 * s);
                out.Text(vb, ValueText(*d, v), 13 * s, dimmed ? kText3 : kText, Align::Center, FontWeight::Semibold);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = {tx - 8 * s, rr.y, tw + 16 * s, rr.h}, .index = i,
                                 .part = Hit::Part::Track});
                break;
            }
            case Type::Choice:
            case Type::Text: {
                // a value this system cannot run (chosen in the file, or a GPU change): shown, in the warning colour
                const Color tc = !ChosenWhy(*d).empty() ? kWarn : dimmed ? kText3 : kText;
                dropdown(ValueText(*d, v), tc);
                break;
            }
            case Type::Binding: {
                const auto& b = std::get<Settings::Bindings>(v);
                const auto action = static_cast<Input::Action>(d->action);
                const float slot_w = (ctrl.w - 8 * s) / 2;
                for (int k = 0; k < 2; ++k) {
                    const Rect sr{ctrl.x + static_cast<float>(k) * (slot_w + 8 * s), cy - 14 * s, slot_w, 28 * s};
                    const bool capturing = capture_ && capture_->action == action &&
                                           capture_->slot == static_cast<size_t>(k);
                    const bool has = static_cast<size_t>(k) < b.size();
                    const bool conflict = has && in_conflict(action, b[static_cast<size_t>(k)]);
                    const bool chosen = focused && slot_ == k;
                    out.FillRect(sr, capturing ? kAccentSoft : kKeycap, 4 * s);
                    const Color line = capturing ? kAccent : conflict ? kWarn : chosen ? kAccentLine : kControlLine;
                    out.StrokeRect(sr, line, capturing || conflict || chosen ? 1.6f * px1 : px1, 4 * s);
                    std::string text = capturing ? std::string(T("Press a key…", "Нажмите клавишу…"))
                                       : has      ? BindingText(b[static_cast<size_t>(k)])
                                                  : std::string("—");
                    if (has && k == 1 && b.size() > 2) {
                        text += fmt::format(" +{}", b.size() - 2);
                    }
                    out.Text(sr, text, 13.5f * s, capturing ? kAccent : has ? (conflict ? kWarn : kText) : kText3,
                             Align::Center, has || capturing ? FontWeight::Semibold : FontWeight::Regular);
                    hits_.push_back({.kind = Hit::Kind::Row, .rect = sr, .index = i,
                                     .part = k == 0 ? Hit::Part::Slot0 : Hit::Part::Slot1});
                }
                break;
            }
            }
            break;
        }
        }
    }
    out.PopClip();

    // ---- scroll bar
    thumb_ = track_ = {};
    if (max_scroll > 0) {
        track_ = R(kListR + 6, kListTop, 4, view_h_);
        const float th = std::max(28 * s, track_.h * view_h_ / content_h_);
        thumb_ = {track_.x, track_.y + (track_.h - th) * (scroll_ / max_scroll), track_.w, th};
        out.FillRect(track_, Color{255, 255, 255, 14}, 2 * s);
        out.FillRect(thumb_, dragging_thumb_ ? kText2 : Color{255, 255, 255, 70}, 2 * s);
        hits_.push_back({.kind = Hit::Kind::ScrollTrack, .rect = {track_.x - 5 * s, track_.y, track_.w + 10 * s, track_.h}});
        hits_.push_back({.kind = Hit::Kind::ScrollThumb, .rect = {thumb_.x - 5 * s, thumb_.y, thumb_.w + 10 * s, thumb_.h}});
    }
    // fades at the list's cut edges (more above / below)
    if (scroll_ > 0.5f) {
        out.FillRect(R(kListX, kListTop, kListR - kListX, 1), kLine);
    }
    if (scroll_ < max_scroll - 0.5f) {
        out.FillRect(R(kListX, kListTop + view_h_, kListR - kListX, 1), kLine);
    }

    // ---- the description of the focused row (and what just happened)
    {
        const float hy = list_bottom;
        out.Line(X(kListX + 4), Y(hy), X(kListR - 4), Y(hy), kLine, px1);
        float ty = hy + 10;
        std::string first;
        Color first_c = kText2;
        if (!status_.empty()) {
            first = status_;
            first_c = status_warn_ ? kWarn : kAccent;
        } else if (!env_.save_note.empty()) {
            first = env_.save_note;
            first_c = kWarn;
        } else if (!conflicts.empty() && page_ == Page::Keys) {
            first = fmt::format("{}: {}", T("Key conflicts", "Конфликтов назначений"), conflicts.size());
            first_c = kWarn;
        }
        if (!first.empty()) {
            out.Text(R(kListX + 12, ty, kListR - kListX - 24, 18), first, 13.5f * s, first_c, Align::Left,
                     FontWeight::Semibold);
            ty += 20;
        }
        Color hc = kText2;
        std::string help;
        if (modal_ == Modal::List) {
            const auto options = RowOptions(list_row_);
            if (list_item_ >= 0 && list_item_ < static_cast<int>(options.size()) &&
                !options[static_cast<size_t>(list_item_)].ok) {
                help = options[static_cast<size_t>(list_item_)].why;
                hc = kRestart;
            } else {
                help = HelpText(list_row_, hc);
            }
        } else {
            help = HelpText(focus_, hc);
        }
        out.PushClip(R(kListX, hy + 2, kListR - kListX, kHelpH - 4));
        out.Text(R(kListX + 12, ty, kListR - kListX - 24, hy + kHelpH - ty - 4), help, 13.5f * s, hc, Align::Left,
                 FontWeight::Regular, true);
        out.PopClip();
    }

    // ---- footer: hints, close button
    {
        const float fy = kWindowH - kFooterH;
        out.Line(panel.x, Y(fy), panel.Right(), Y(fy), kLine, px1);
        const std::string_view close_text = T("Close", "Закрыть");
        const float bw = std::max(104 * s, measure.Width(close_text, 13.5f * s, FontWeight::Semibold) + 32 * s);
        const Rect cb{X(kWindowW - 24) - bw, Y(fy + 10), bw, 32 * s};
        out.FillRect(cb, hover_close_ == 1 ? kRowFocus : kControl, 4 * s);
        out.StrokeRect(cb, hover_close_ == 1 ? kControlHover : kControlLine, px1, 4 * s);
        out.Text(cb, close_text, 13.5f * s, kText, Align::Center, FontWeight::Semibold);
        hits_.push_back({.kind = Hit::Kind::Close, .rect = cb, .index = 1});
        struct HintItem {
            Hint hint;
            std::string_view en, ru;
        };
        std::vector<HintItem> hints;
        if (modal_ == Modal::List) {
            hints = {{Hint::Navigate, "Select", "Выбор"}, {Hint::Accept, "Choose", "Выбрать"}, {Hint::Back, "Cancel", "Отмена"}};
        } else if (modal_ != Modal::None) {
            hints = {{Hint::Adjust, "Select", "Выбор"}, {Hint::Accept, "Confirm", "Подтвердить"}, {Hint::Back, "Cancel", "Отмена"}};
        } else if (capture_) {
            hints = {{Hint::Back, "Cancel", "Отмена"}};
        } else {
            hints = {{Hint::Navigate, "Select", "Выбор"},  {Hint::Adjust, "Change", "Изменить"},
                     {Hint::Accept, "Open", "Открыть"},     {Hint::Tabs, "Page", "Раздел"},
                     {Hint::Reset, "Default", "Сброс"},    {Hint::Back, "Close", "Закрыть"}};
        }
        float hx = X(24);
        const float hcy = Y(fy + kFooterH / 2);
        for (const auto& h : hints) {
            const auto text = T(h.en, h.ru);
            const float tw = measure.Width(text, 13 * s, FontWeight::Regular);
            // a hint that would run into the close button is left out (narrow windows, long words)
            DrawList probe;
            probe.Reset(W, H);
            const float gw = DrawGlyph(probe, measure, h.hint, env_, hx, hcy, s);
            if (hx + gw + 6 * s + tw > cb.x - 12 * s) {
                break;
            }
            DrawGlyph(out, measure, h.hint, env_, hx, hcy, s);
            hx += gw + 6 * s;
            out.Text({hx, Y(fy), tw + 4 * s, kFooterH * s}, text, 13 * s, kText2);
            hx += tw + 18 * s;
        }
    }

    // ---- an open drop-down list: over the rows, under or above its box
    if (modal_ == Modal::List) {
        hits_.push_back({.kind = Hit::Kind::ListOutside, .rect = out.Surface()});
        const auto options = RowOptions(list_row_);
        const int count = static_cast<int>(options.size());
        if (list_anchor.w <= 0) {
            list_anchor = R(kListR - 12 - kCtrlW, kListTop, kCtrlW, 30); // its row scrolled out of view
        }
        float lw = list_anchor.w;
        for (const auto& o : options) {
            lw = std::max(lw, measure.Width(o.label, 14 * s, FontWeight::Regular) + 44 * s);
        }
        lw = std::min(lw, X(kListR) - X(kNavW + 8));
        const int visible = std::min(count, kListVisible);
        const float lh = (static_cast<float>(visible) * kListItemH + 8) * s;
        float ly = list_anchor.Bottom() + 4 * s;
        if (ly + lh > Y(kWindowH - kFooterH) - 4 * s) {
            ly = std::max(Y(kHeaderH) + 4 * s, list_anchor.y - 4 * s - lh);
        }
        const Rect box{std::max(X(kNavW + 8), list_anchor.Right() - lw), ly, lw, lh};
        out.FillRect({box.x, box.y + 3 * s, box.w, box.h}, Color{0, 0, 0, 90}, 6 * s);
        out.FillRect(box, kPopup, 5 * s);
        out.StrokeRect(box, kAccentLine, px1, 5 * s);
        list_first_ = std::clamp(list_first_, 0, std::max(0, count - kListVisible));
        const int current = CurrentOption(list_row_, options);
        for (int k = list_first_; k < std::min(count, list_first_ + visible); ++k) {
            const auto& o = options[static_cast<size_t>(k)];
            const Rect ir{box.x + 4 * s, box.y + 4 * s + static_cast<float>(k - list_first_) * kListItemH * s,
                          box.w - 8 * s, kListItemH * s};
            if (k == list_item_) {
                out.FillRect(ir, o.ok ? kRowFocus : Color{255, 255, 255, 8}, 3 * s);
            }
            if (k == current) { // a check mark
                const float cx = ir.x + 13 * s, cy = ir.y + ir.h / 2;
                out.Line(cx - 4.5f * s, cy, cx - 1.5f * s, cy + 3.5f * s, kAccent, std::max(1.0f, 1.8f * s));
                out.Line(cx - 1.5f * s, cy + 3.5f * s, cx + 5 * s, cy - 4 * s, kAccent, std::max(1.0f, 1.8f * s));
            }
            out.Text({ir.x + 28 * s, ir.y, ir.w - 36 * s, ir.h}, o.label, 14 * s,
                     !o.ok ? kText3 : k == current ? kAccent : kText, Align::Left, FontWeight::Regular);
            if (!o.ok) {
                const auto tag = T("unavailable", "недоступно");
                const float tw = measure.Width(tag, 11 * s, FontWeight::Semibold);
                out.Text({ir.Right() - tw - 10 * s, ir.y, tw + 4 * s, ir.h}, tag, 11 * s, kText3, Align::Right,
                         FontWeight::Semibold);
            }
            hits_.push_back({.kind = Hit::Kind::ListItem, .rect = ir, .index = k});
        }
        if (count > kListVisible) { // the list's own scroll position
            const float th = box.h * static_cast<float>(visible) / static_cast<float>(count);
            const float tt = box.y + (box.h - th) * static_cast<float>(list_first_) /
                                         static_cast<float>(std::max(1, count - kListVisible));
            out.FillRect({box.Right() - 4 * s, tt + 2 * s, 2 * s, th - 4 * s}, Color{255, 255, 255, 70}, 1 * s);
        }
    }

    // ---- dialogs
    if (modal_ == Modal::ConfirmDisplay || modal_ == Modal::ConfirmReset) {
        out.FillRect(panel, kShade, 6 * s);
        const Rect box = R((kWindowW - 480) / 2, (kWindowH - 216) / 2, 480, 216);
        out.FillRect({box.x, box.y + 4 * s, box.w, box.h}, Color{0, 0, 0, 100}, 8 * s);
        out.FillRect(box, kPopup, 6 * s);
        out.StrokeRect(box, kFrame, px1, 6 * s);
        out.FillRect({box.x, box.y, box.w, 3 * s}, kAccent, 1.5f * s);
        std::string mtitle, text;
        std::string_view yes, no;
        float left = 0;
        if (modal_ == Modal::ConfirmDisplay) {
            const u64 left_ns = modal_deadline_ns_ > env_.now_ns ? modal_deadline_ns_ - env_.now_ns : 0;
            const auto left_s = (left_ns + 999'999'999) / 1'000'000'000;
            left = static_cast<float>(static_cast<double>(left_ns) / static_cast<double>(kConfirmNs));
            mtitle = std::string(T("Keep these display settings?", "Оставить новые параметры экрана?"));
            text = fmt::format("{} {} {}.", T("If the picture is wrong, do nothing: the previous ones come back in",
                                              "Если изображение неправильное, ничего не нажимайте: прежние параметры "
                                              "вернутся через"),
                               left_s, T("s", "с"));
            yes = T("Keep", "Оставить");
            no = T("Restore", "Вернуть");
        } else {
            mtitle = fmt::format("{} «{}»?", T("Reset", "Сбросить раздел"), PageLabel(page_, env_.ru));
            text = std::string(T("Every setting of this page goes back to its default (SecondSonPC.toml).",
                                 "Все настройки раздела вернутся к значениям по умолчанию (SecondSonPC.toml)."));
            yes = T("Reset", "Сбросить");
            no = T("Cancel", "Отмена");
        }
        out.Text({box.x + 24 * s, box.y + 18 * s, box.w - 48 * s, 30 * s}, mtitle, 18 * s, kText, Align::Left,
                 FontWeight::Semibold);
        out.Text({box.x + 24 * s, box.y + 56 * s, box.w - 48 * s, 64 * s}, text, 14 * s, kText2, Align::Left,
                 FontWeight::Regular, true);
        if (modal_ == Modal::ConfirmDisplay) {
            const Rect bar{box.x + 24 * s, box.y + 126 * s, box.w - 48 * s, 3 * s};
            out.FillRect(bar, kTrack, 1.5f * s);
            out.FillRect({bar.x, bar.y, bar.w * left, bar.h}, kAccent, 1.5f * s);
        }
        const float bw = 132 * s, bh = 34 * s, by = box.Bottom() - bh - 20 * s;
        for (int i = 0; i < 2; ++i) {
            const Rect b{box.Right() - 24 * s - bw * static_cast<float>(2 - i) - 10 * s * static_cast<float>(1 - i), by,
                         bw, bh};
            const bool sel = modal_button_ == i;
            if (sel) {
                out.FillRect(b, kAccent, 4 * s);
            } else {
                out.FillRect(b, kControl, 4 * s);
                out.StrokeRect(b, kControlLine, px1, 4 * s);
            }
            out.Text(b, i == 0 ? yes : no, 14 * s, sel ? kOnAccent : kText, Align::Center, FontWeight::Semibold);
            hits_.push_back({.kind = Hit::Kind::ModalButton, .rect = b, .index = i});
        }
    }
}

} // namespace SecondSon::Ui
