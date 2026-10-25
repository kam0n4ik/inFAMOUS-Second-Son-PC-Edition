// SecondSonPC — the registry of PC settings (see registry.h). The table below is the one place that says which
// settings exist, where they are stored and how they apply; the menu, the user file and the tests read it.
#include "settings/registry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "engine/render/display_plan.h"
#include "input/binding_rules.h"
#include "runtime/language.h"
#include "settings/experiments.h"

namespace SecondSon::Settings {

namespace {

using App::Config;

// ---- choices

constexpr Choice kFullscreenModes[] = {
    {"borderless", "Borderless", "Без рамки"},
    {"exclusive", "Exclusive", "Эксклюзивный"},
};
// [Display] presentMode (the stored values stay those of SecondSonPC.toml: "fifo" | "immediate" | "mailbox")
constexpr Choice kPresentModes[] = {
    {"fifo", "On", "Вкл."},
    {"immediate", "Off", "Выкл."},
    {"mailbox", "Off, without tearing (Mailbox)", "Выкл., без разрывов (Mailbox)"},
};
constexpr Choice kFrameRateLimits[] = {
    {"console", "60 (PS4 vblank)", "60 (кадр по vblank PS4)"},
    {"30", "30", "30"},
    {"40", "40", "40"},
    {"50", "50", "50"},
    {"60", "60", "60"},
    {"90", "90", "90"},
    {"120", "120", "120"},
    {"144", "144", "144"},
    {"165", "165", "165"},
    {"240", "240", "240"},
    {"unlimited", "Unlimited", "Без ограничения"},
};
constexpr Choice kResolutions[] = {
    {"auto", "Automatic (the window / the display)", "Автоматически (окно / монитор)"},
    {"1280x720", "1280 × 720 (16:9)", "1280 × 720 (16:9)"},
    {"1600x900", "1600 × 900 (16:9)", "1600 × 900 (16:9)"},
    {"1920x1080", "1920 × 1080 (16:9)", "1920 × 1080 (16:9)"},
    {"2560x1440", "2560 × 1440 (16:9)", "2560 × 1440 (16:9)"},
    {"3840x2160", "3840 × 2160 (16:9)", "3840 × 2160 (16:9)"},
    {"1280x800", "1280 × 800 (16:10)", "1280 × 800 (16:10)"},
    {"1680x1050", "1680 × 1050 (16:10)", "1680 × 1050 (16:10)"},
    {"1920x1200", "1920 × 1200 (16:10)", "1920 × 1200 (16:10)"},
    {"2560x1600", "2560 × 1600 (16:10)", "2560 × 1600 (16:10)"},
    {"2560x1080", "2560 × 1080 (21:9)", "2560 × 1080 (21:9)"},
    {"3440x1440", "3440 × 1440 (21:9)", "3440 × 1440 (21:9)"},
    {"3840x1600", "3840 × 1600 (21:9)", "3840 × 1600 (21:9)"},
    {"3840x1080", "3840 × 1080 (32:9)", "3840 × 1080 (32:9)"},
    {"5120x1440", "5120 × 1440 (32:9)", "5120 × 1440 (32:9)"},
    {"800x600", "800 × 600 (4:3)", "800 × 600 (4:3)"},
    {"1024x768", "1024 × 768 (4:3)", "1024 × 768 (4:3)"},
    {"1440x1080", "1440 × 1080 (4:3)", "1440 × 1080 (4:3)"},
    {"1600x1200", "1600 × 1200 (4:3)", "1600 × 1200 (4:3)"},
};
constexpr Choice kAspectRatios[] = {
    {"auto", "Automatic (as the resolution)", "Автоматически (по разрешению)"},
    {"16:9", "16:9", "16:9"},
    {"16:10", "16:10", "16:10"},
    {"21:9", "21:9", "21:9"},
    {"32:9", "32:9", "32:9"},
    {"4:3", "4:3", "4:3"},
};
constexpr Choice kHudAreas[] = {
    {"full", "Whole picture", "Вся картинка"},
    {"21:9", "Centre 21:9", "Центр 21:9"},
    {"16:9", "Centre 16:9", "Центр 16:9"},
};
constexpr Choice kRenderScales[] = {
    {"auto", "Automatic (up to 1080 lines)", "Автоматически (до 1080 строк)"},
    {"50", "50 %", "50 %"},
    {"60", "60 %", "60 %"},
    {"67", "67 %", "67 %"},
    {"75", "75 %", "75 %"},
    {"85", "85 %", "85 %"},
    {"100", "100 % (native)", "100 % (родное)"},
};
constexpr Choice kSideways[] = {
    {"right-up", "Right side up", "Правой стороной вверх"},
    {"left-up", "Left side up", "Левой стороной вверх"},
};
constexpr Choice kMenuButtons[] = {
    {"guide", "PS / Guide button", "Кнопка PS / Guide"},
    {"l3r3", "L3 + R3 (hold 1 s)", "L3 + R3 (удерживать 1 с)"},
    {"off", "Off", "Выключено"},
};
constexpr Choice kUiLanguages[] = {
    {"auto", "As the game", "Как в игре"},
    {"ru", "Русский", "Русский"},
    {"en", "English", "English"},
};
constexpr Choice kPrompts[] = {
    {"auto", "Automatic", "Автоматически"},
    {"playstation", "PlayStation", "PlayStation"},
    {"xbox", "Xbox", "Xbox"},
    {"keyboard", "Keyboard", "Клавиатура"},
};
constexpr Choice kGameLanguages[] = {
    {"ru-RU", "Russian", "Русский"},
    {"en-US", "English", "English"},
};
constexpr Choice kEnterButtons[] = {
    {"cross", "Cross", "Крест"},
    {"circle", "Circle", "Круг"},
};
constexpr Choice kSmokeQualities[] = {
    {"high", "High (original)", "Высокое (оригинал)"},
    {"medium", "Medium (65 %)", "Среднее (65 %)"},
    {"low", "Low (35 %)", "Низкое (35 %)"},
    {"verylow", "Very low (20 %)", "Очень низкое (20 %)"},
};
constexpr Choice kParticleResolutions[] = {
    {"auto", "Auto (as the game)", "Авто (как в игре)"},
    {"full", "Full", "Полное"},
    {"half", "Half", "Половина"},
    {"quarter", "Quarter", "Четверть"},
};
constexpr Choice kUpscalers[] = {
    {"off", "Off (Render scale)", "Выключено (масштаб рендеринга)"},
    {"dlss", "NVIDIA DLSS", "NVIDIA DLSS"},
    {"fsr", "AMD FSR 3.1", "AMD FSR 3.1"},
};
constexpr Choice kUpscaleQualities[] = {
    {"native", "Native AA (DLAA / FSR native)", "Нативное сглаживание (DLAA / FSR native)"},
    {"quality", "Quality (67 %)", "Качество (67 %)"},
    {"balanced", "Balanced (59 %)", "Баланс (59 %)"},
    {"performance", "Performance (50 %)", "Производительность (50 %)"},
    {"ultra", "Ultra Performance (33 %)", "Ультрапроизводительность (33 %)"},
};
constexpr Choice kFrameGenerations[] = {
    {"off", "Off", "Выключено"},
    {"fsr", "AMD FSR Frame Generation", "AMD FSR Frame Generation"},
    {"dlss", "NVIDIA DLSS Frame Generation", "NVIDIA DLSS Frame Generation"},
};
constexpr Choice kFrameGenMultipliers[] = {
    {"2", "x2", "×2"},
    {"3", "x3 (DLSS, RTX 50)", "×3 (DLSS, RTX 50)"},
    {"4", "x4 (DLSS, RTX 50)", "×4 (DLSS, RTX 50)"},
};

/// [Display] frameRateLimit beyond the list: any whole number of frames per second the limiter takes
/// (VideoOut::ParsePacing: 20..1000).
bool AcceptsFrameRate(std::string_view s) {
    u32 n = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    return !s.empty() && r.ec == std::errc{} && r.ptr == s.data() + s.size() && n >= 20 && n <= 1000;
}

/// [Display] resolution beyond the list: any "WxH" the display plan takes (a display's own size, 1366x768).
bool AcceptsResolution(std::string_view s) {
    Engine::Render::Size size;
    return Engine::Render::ParseSize(s, size);
}

// ---- labels of the keyboard/mouse actions (index = Input::Action)

struct ActionText {
    std::string_view en, ru;
    std::string_view help_en, help_ru;
};
constexpr std::array<ActionText, Input::kActionCount> kActionText{{
    {"Cross (jump)", "Крест (прыжок)", "", ""},
    {"Circle", "Круг", "", ""},
    {"Square", "Квадрат", "", ""},
    {"Triangle", "Треугольник", "", ""},
    {"L1", "L1", "", ""},
    {"R1", "R1", "", ""},
    {"L2", "L2", "", ""},
    {"R2", "R2", "", ""},
    {"L3 (left stick press)", "L3 (нажатие левого стика)", "", ""},
    {"R3 (right stick press)", "R3 (нажатие правого стика)", "", ""},
    {"Options (pause)", "Options (пауза)", "", ""},
    {"Touch pad click", "Нажатие сенсорной панели",
     "A finger at the centre of the touch pad and its click while held (absorbing, upgrades menu).",
     "Палец в центре сенсорной панели и её нажатие, пока клавиша удерживается (поглощение, меню улучшений)."},
    {"D-pad up", "Крестовина вверх", "", ""},
    {"D-pad down", "Крестовина вниз", "", ""},
    {"D-pad left", "Крестовина влево", "", ""},
    {"D-pad right", "Крестовина вправо", "", ""},
    {"Move forward", "Вперёд", "", ""},
    {"Move back", "Назад", "", ""},
    {"Move left", "Влево", "", ""},
    {"Move right", "Вправо", "", ""},
    {"Walk (hold)", "Шаг (удерживать)", "The left stick at half deflection while held.",
     "Левый стик отклоняется наполовину, пока клавиша удерживается."},
    {"Release the cursor", "Отпустить курсор",
     "Gives the mouse cursor back and takes it again. Shares Esc with Options by default (pause + cursor).",
     "Отпускает курсор мыши и снова захватывает его. По умолчанию делит Esc с Options (пауза + курсор)."},
    {"Hold the controller sideways (G)", "Контроллер «боком» (G)",
     "Toggles the motion mode for graffiti: the mouse turns the controller instead of the camera. A movement key "
     "also ends it.",
     "Включает режим движения для граффити: мышь поворачивает контроллер, а не камеру. Клавиша движения тоже "
     "выключает режим."},
    {"Shake the controller", "Встряхнуть контроллер", "Shakes the controller while held (the spray can).",
     "Трясёт контроллер, пока клавиша удерживается (баллончик краски)."},
    {"Drag on the touch pad", "Вести пальцем по панели",
     "While held, the mouse drags a finger on the touch pad (swipes).",
     "Пока клавиша удерживается, мышь ведёт палец по сенсорной панели (свайпы)."},
    {"Two-finger touch pad click", "Нажатие панели двумя пальцами",
     "Two fingers on the touch pad and its click while held: the game's two-finger actions (absorbing with two "
     "fingers, pushing a car). Not yet checked in the game.",
     "Два пальца на сенсорной панели и её нажатие, пока клавиша удерживается: действия игры «двумя пальцами» "
     "(поглощение двумя пальцами, толкание машины). В игре ещё не проверено."},
}};

// ---- accessors

Value GetBinding(const Config& c, const Desc& d) {
    return c.input.bindings[static_cast<size_t>(d.action)];
}
void SetBinding(Config& c, const Desc& d, const Value& v) {
    c.input.bindings[static_cast<size_t>(d.action)] = std::get<Bindings>(v);
}

// The table is built once (ids of the binding settings are generated from the action names).
struct Registry {
    std::vector<Desc> all;
    std::array<std::string, Input::kActionCount> binding_ids;
    std::vector<std::string> experiment_ids; // "experiments.<variable>", filled before their settings (no moves)
};

// ---- experiments (settings/experiments.h): the setting's key is the variable's name

Value GetExperiment(const Config& c, const Desc& d) {
    const Experiment* e = FindExperiment(d.key);
    const auto it = c.pc.experiments.find(d.key);
    const std::string_view v = it != c.pc.experiments.end() ? std::string_view(it->second) : std::string_view{};
    if (d.type == Type::Bool) {
        return e != nullptr && v == e->on;
    }
    return std::string(v.empty() && e != nullptr ? e->unset_shows : v);
}
void SetExperiment(Config& c, const Desc& d, const Value& x) {
    const Experiment* e = FindExperiment(d.key);
    if (e == nullptr) {
        return;
    }
    c.pc.experiments.insert_or_assign(std::string(d.key), d.type == Type::Bool
                                                              ? std::string(std::get<bool>(x) ? e->on : e->off)
                                                              : std::get<std::string>(x));
}

constexpr std::string_view kNeedsBridgePresenterEn =
    "Drawn by the bridge's (shadPS4) presenter; SecondSonPC's own backend (--gpu-backend sspc) gets it in P4.6.";
constexpr std::string_view kNeedsBridgePresenterRu =
    "Это делает презентер моста shadPS4; для собственного бэкенда (--gpu-backend sspc) — интеграция P4.6.";

/// Fills `r` in place: the binding settings' ids point into r.binding_ids, which must not move afterwards.
void Build(Registry& r) {
    auto& v = r.all;

    // ------------------------------------------------------------------ Display
    v.push_back(Desc{
        .id = "display.fullscreen", .table = "Display", .key = "fullscreen", .category = Category::Display,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::Window,
        .en = "Full screen", .ru = "Полноэкранный режим",
        .help_en = "The window covers the whole display. F11 toggles it while playing.",
        .help_ru = "Окно на весь экран. F11 переключает режим во время игры.",
        .get = [](const Config& c, const Desc&) -> Value { return c.fullscreen; },
        .set = [](Config& c, const Desc&, const Value& x) { c.fullscreen = std::get<bool>(x); },
        .legacy_parse = true, .own_row = false, // (R11: the menu's "Window mode" row, with fullscreenMode)
    });
    v.push_back(Desc{
        .id = "display.fullscreenMode", .table = "Display", .key = "fullscreenMode", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::Window, .choices = kFullscreenModes,
        .en = "Full screen type", .ru = "Тип полноэкранного режима",
        .help_en = "Borderless: a window over the desktop, quick Alt+Tab. Exclusive (SecondSonPC.toml only): SDL's full "
                   "screen at the desktop's display mode - no Vulkan exclusive mode; it minimizes on Alt+Tab.",
        .help_ru = "«Без рамки» — окно поверх рабочего стола, быстрый Alt+Tab. «Эксклюзивный» (только из "
                   "SecondSonPC.toml) — полноэкранный режим SDL в режиме рабочего стола, без эксклюзивного режима "
                   "Vulkan; при Alt+Tab окно сворачивается.",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.exclusive_fullscreen ? "exclusive" : "borderless");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.exclusive_fullscreen = std::get<std::string>(x) == "exclusive";
        },
        .legacy_parse = true, .requires_on = "display.fullscreen", .own_row = false, // (the "Window mode" row)
    });
    v.push_back(Desc{
        .id = "display.width", .table = "Display", .key = "width", .category = Category::Display, .type = Type::Int,
        .apply = Apply::Live, .needs = Capability::Window, .min = 640, .max = 7680, .ui_min = 640,
        .ui_max = 7680, .step = 1, .unit = "px", .en = "Window width", .ru = "Ширина окна",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.window_width); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.window_width = static_cast<u32>(std::get<s64>(x));
        },
        .legacy_parse = true, .own_row = false,
    });
    v.push_back(Desc{
        .id = "display.height", .table = "Display", .key = "height", .category = Category::Display,
        .type = Type::Int, .apply = Apply::Live, .needs = Capability::Window, .min = 360, .max = 4320,
        .ui_min = 360, .ui_max = 4320, .step = 1, .unit = "px", .en = "Window height", .ru = "Высота окна",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.window_height); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.window_height = static_cast<u32>(std::get<s64>(x));
        },
        .legacy_parse = true, .own_row = false,
    });
    v.push_back(Desc{
        .id = "display.monitor", .table = "Display", .key = "monitor", .category = Category::Display,
        .type = Type::Text, .apply = Apply::Live, .needs = Capability::Window, .en = "Display",
        .ru = "Монитор",
        .help_en = "The display the game window is on (the list: the system's displays, their desktop size and refresh "
                   "rate). Applies at once; the full screen covers that display.",
        .help_ru = "Монитор, на котором находится окно игры (в списке — мониторы системы, их разрешение рабочего стола и "
                   "частота). Применяется сразу; полноэкранный режим занимает этот монитор.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.monitor; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.monitor = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "display.presentMode", .table = "Display", .key = "presentMode", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::Window,
        .choices = kPresentModes, .en = "Vertical sync", .ru = "Вертикальная синхронизация",
        .help_en = "On (FIFO): no tearing, at most the display's refresh rate. Off (IMMEDIATE): frames at once, tearing "
                   "possible; Mailbox where the display lacks it. Off without tearing (MAILBOX): the newest frame at "
                   "each refresh. The FPS limit works in every mode. Applies at once (shadPS4 bridge: after a restart).",
        .help_ru = "«Вкл.» (FIFO) — без разрывов, не выше частоты монитора. «Выкл.» (IMMEDIATE) — кадр сразу, "
                   "возможны разрывы; если монитор не поддерживает — Mailbox. «Выкл., без разрывов» (MAILBOX) — "
                   "новейший кадр на каждом обновлении. Ограничение FPS работает в любом режиме. Применяется сразу "
                   "(мост shadPS4 — после перезапуска).",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.present_mode == "Fifo" ? "fifo" : c.present_mode == "Immediate" ? "immediate"
                                                                                                    : "mailbox");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            const auto& s = std::get<std::string>(x);
            c.present_mode = s == "fifo" ? "Fifo" : s == "immediate" ? "Immediate" : "Mailbox";
        },
        .legacy_parse = true, .live_needs = Capability::PresentModeSwitch, // (the bridge: from the next start)
    });
    v.push_back(Desc{
        .id = "display.showFps", .table = "Display", .key = "showFps", .category = Category::Display,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::FpsCounter, .en = "FPS counter",
        .ru = "Счётчик FPS",
        .help_en = "Frames per second and the frame time in the top left corner of the picture. F10 switches it "
                   "while playing; the choice is kept.",
        .help_ru = "Кадры в секунду и время кадра в левом верхнем углу картинки. F10 переключает его во время игры; "
                   "выбор сохраняется.",
        .why_en = "This run's presenter has no FPS counter.", .why_ru = "У презентера этого запуска нет счётчика FPS.",
        .get = [](const Config& c, const Desc&) -> Value { return c.show_fps; },
        .set = [](Config& c, const Desc&, const Value& x) { c.show_fps = std::get<bool>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "display.frameRateLimit", .table = "Display", .key = "frameRateLimit", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::FrameLimiter, .choices = kFrameRateLimits,
        .en = "Frame rate limit", .ru = "Ограничение FPS",
        .help_en = "60 (PS4 vblank): as the console - a frame is shown only at a 60 Hz vblank, so a frame that takes "
                   "17 ms waits until 33 ms. A number or Unlimited: a frame is shown as soon as it is ready, at most "
                   "that many a second (the game's clock is real time: the world does not run faster). Works with "
                   "Vertical sync on or off; with it on, the display's refresh rate limits too. The game's own 30 fps "
                   "lock (Options - Game) still applies. Applies at once.",
        .help_ru = "60 (кадр по vblank PS4) — как на консоли: кадр показывается только на vblank 60 Гц, поэтому кадр "
                   "за 17 мс ждёт до 33 мс. Число или «Без ограничения» — кадр показывается сразу, как готов, не чаще "
                   "заданного числа в секунду (часы игры — реальное время: мир не ускоряется). Работает при любой "
                   "вертикальной синхронизации; при включённой частота монитора ограничивает дополнительно. "
                   "Ограничение 30 к/с самой игры (Параметры → Игра) действует. Применяется сразу.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc).",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.frame_rate_limit; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.frame_rate_limit = std::get<std::string>(x); },
        .accepts = &AcceptsFrameRate, // (a number from SecondSonPC.toml / the user file: "75")
    });

    v.push_back(Desc{
        .id = "display.resolution", .table = "Display", .key = "resolution", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Restart, .needs = Capability::RenderScale, .choices = kResolutions,
        .en = "Resolution", .ru = "Разрешение",
        .help_en = "The game's own resolution: its picture and HUD are drawn at this size and the window shows it "
                   "(with bars when the shapes differ). Automatic: the window's size, or the display's in full screen. "
                   "Below 1080 lines the game still draws 1080 lines (as the PS4) and the picture is shown smaller. "
                   "The 3D scene's own resolution is Render scale. Takes effect after a restart: the game sizes its "
                   "buffers when it starts.",
        .help_ru = "Собственное разрешение игры: картинка и интерфейс рисуются в этом размере, окно показывает его "
                   "(с полосами, если форма другая). «Автоматически» — размер окна, а в полноэкранном режиме — "
                   "монитора. Меньше 1080 строк игра всё равно рисует 1080 (как PS4), а картинка показывается "
                   "уменьшенной. Разрешение самой 3D-сцены — «Масштаб рендеринга». Вступает в силу после "
                   "перезапуска: игра создаёт буферы при старте.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc); the bridge's renders 1920x1080.",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc); мост рендерит 1920×1080.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.resolution; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.resolution = std::get<std::string>(x); },
        .accepts = &AcceptsResolution, // (the display's own size; any "WxH" from the files)
    });
    v.push_back(Desc{
        .id = "display.aspectRatio", .table = "Display", .key = "aspectRatio", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Restart, .needs = Capability::RenderScale, .choices = kAspectRatios,
        .en = "Aspect ratio", .ru = "Соотношение сторон",
        .help_en = "The shape of the game's picture. A wider one shows more at the sides with the same height of view "
                   "(Hor+), a narrower one less; nothing is stretched, the rest of the window gets bars. Automatic: "
                   "the resolution's shape (4:3 .. 32:9; a taller window shows 16:9 with bars above and below). Takes "
                   "effect after a restart.",
        .help_ru = "Форма картинки игры. Шире — по бокам видно больше при той же высоте обзора (Hor+), уже — меньше; "
                   "ничего не растягивается, остальное окно занимают полосы. «Автоматически» — форма разрешения "
                   "(от 4:3 до 32:9; в высоком окне — 16:9 с полосами сверху и снизу). Вступает в силу после "
                   "перезапуска.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc); the bridge's renders 16:9.",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc); мост рендерит 16:9.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.aspect_ratio; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.aspect_ratio = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "display.hudSafeArea", .table = "Display", .key = "hudSafeArea", .category = Category::Display,
        .type = Type::Int, .apply = Apply::Live, .needs = Capability::RenderScale, .min = 85, .max = 100,
        .ui_min = 85, .ui_max = 100, .step = 1, .unit = "%", .en = "HUD safe area", .ru = "Безопасная зона HUD",
        .help_en = "How far the HUD (health, the map, prompts) keeps from the picture's edges, as the PS4's Display "
                   "Area: 100 % at the edges, 85 % furthest in. Applies at once.",
        .help_ru = "Насколько интерфейс (здоровье, карта, подсказки) отступает от краёв картинки, как «Область "
                   "отображения» PS4: 100 % — у самых краёв, 85 % — дальше всего. Применяется сразу.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc).",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.hud_safe_area); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.hud_safe_area = static_cast<u32>(std::get<s64>(x));
        },
    });
    v.push_back(Desc{
        .id = "display.hudArea", .table = "Display", .key = "hudArea", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::RenderScale, .choices = kHudAreas,
        .en = "HUD area", .ru = "Область HUD",
        .help_en = "On a wide picture (21:9, 32:9): the HUD at the picture's edges, or in its centre 21:9 / 16:9 part, "
                   "closer to where you look. Applies at once.",
        .help_ru = "На широкой картинке (21:9, 32:9): интерфейс у краёв картинки или в её центральной части 21:9 / "
                   "16:9, ближе к центру взгляда. Применяется сразу.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc).",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.hud_area; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.hud_area = std::get<std::string>(x); },
    });

    // ------------------------------------------------------------------ Graphics
    v.push_back(Desc{
        .id = "graphics.fsr", .table = "Graphics", .key = "fsr", .category = Category::Graphics, .type = Type::Bool,
        .apply = Apply::Live, .needs = Capability::BridgePresenter, .en = "FSR 1.0 upscaling",
        .ru = "Масштабирование FSR 1.0",
        .help_en = "Scales the game's 1920x1080 frame to the window with AMD FidelityFX Super Resolution 1.0 instead "
                   "of a plain stretch. Useful with a window larger than 1080p; the game's own resolution does not "
                   "change.",
        .help_ru = "Растягивает кадр игры 1920×1080 до размера окна алгоритмом AMD FidelityFX Super Resolution 1.0 "
                   "вместо простого растяжения. Полезно, когда окно больше 1080p; внутреннее разрешение игры не "
                   "меняется.",
        .why_en = kNeedsBridgePresenterEn, .why_ru = kNeedsBridgePresenterRu,
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.fsr; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.fsr = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "graphics.sharpening", .table = "Graphics", .key = "sharpening", .category = Category::Graphics,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::BridgePresenter,
        .en = "Sharpening (RCAS)", .ru = "Повышение резкости (RCAS)",
        .help_en = "AMD's robust contrast-adaptive sharpening after FSR.",
        .help_ru = "Контрастно-адаптивное повышение резкости AMD после FSR.", .why_en = kNeedsBridgePresenterEn,
        .why_ru = kNeedsBridgePresenterRu,
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.sharpening; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.sharpening = std::get<bool>(x); },
        .requires_on = "graphics.fsr",
    });
    v.push_back(Desc{
        .id = "graphics.sharpness", .table = "Graphics", .key = "sharpness", .category = Category::Graphics,
        .type = Type::Int, .apply = Apply::Live, .needs = Capability::BridgePresenter, .min = 0, .max = 100,
        .ui_min = 0, .ui_max = 100, .step = 5, .unit = "%", .en = "Sharpening strength", .ru = "Сила резкости",
        .why_en = kNeedsBridgePresenterEn, .why_ru = kNeedsBridgePresenterRu,
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.sharpness); },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.sharpness = static_cast<u32>(std::get<s64>(x)); },
        .requires_on = "graphics.sharpening",
    });
    v.push_back(Desc{
        .id = "graphics.smokeQuality", .table = "Graphics", .key = "smokeQuality", .category = Category::Graphics,
        .type = Type::Choice, .apply = Apply::Live, .choices = kSmokeQualities, .en = "Smoke quality (experimental)",
        .ru = "Качество дыма (эксперимент)",
        .help_en = "How many particles the game's smoke emitters spawn (fires, chimneys, the smoke powers' clouds): "
                   "fewer particles mean thinner smoke and less GPU work on its translucent layers. Fire, sparks and "
                   "the powers' logic are not changed. High is the game's own density.",
        .help_ru = "Сколько частиц создают эмиттеры дыма игры (костры, трубы, облака дымовых способностей): меньше "
                   "частиц — реже дым и меньше работы GPU на его прозрачных слоях. Огонь, искры и логика способностей "
                   "не меняются. «Высокое» — плотность самой игры.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.smoke_quality; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.smoke_quality = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "graphics.particleResolution", .table = "Graphics", .key = "particleResolution",
        .category = Category::Graphics, .type = Type::Choice, .apply = Apply::Live, .choices = kParticleResolutions,
        .en = "Fire and smoke resolution (experimental)", .ru = "Разрешение огня и дыма (эксперимент)",
        .help_en = "The game's own reduced-resolution mode for its translucent particles (fire, smoke, dust), which the "
                   "PS4 version switches to by itself when they are slow: the same particles drawn at half or a "
                   "quarter of the resolution and upscaled. Much less GPU work in scenes full of fire; edges of "
                   "flames get softer. Auto is the game's own choice.",
        .help_ru = "Собственный режим игры для прозрачных частиц (огонь, дым, пыль) в пониженном разрешении, в который "
                   "версия для PS4 переходит сама, когда они тормозят: те же частицы рисуются в половинном или "
                   "четвертном разрешении и растягиваются. Намного меньше работы GPU в сценах с огнём; края пламени "
                   "мягче. «Авто» — выбор самой игры.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.particle_resolution; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.particle_resolution = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "graphics.renderScale", .table = "Graphics", .key = "renderScale", .category = Category::Graphics,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::RenderScale, .choices = kRenderScales,
        .en = "Render scale", .ru = "Масштаб рендеринга",
        .help_en = "The 3D scene's resolution against the game's resolution, per side; the game scales the scene up "
                   "and draws the HUD sharp on top. Automatic: at most 1080 lines, as the PS4 (a larger display costs "
                   "no frame rate). 100 %: the scene at the full resolution (up to about 6 million pixels). With an "
                   "upscaler (DLSS / FSR) its quality sets the scene instead. Applies at once.",
        .help_ru = "Разрешение 3D-сцены относительно разрешения игры, по стороне; игра растягивает сцену, а интерфейс "
                   "рисует поверх чётким. «Автоматически» — не больше 1080 строк, как на PS4 (большой монитор не "
                   "отнимает FPS). 100 % — сцена в полном разрешении (до ~6 млн пикселей). С апскейлером (DLSS / FSR) "
                   "сцену задаёт его качество. Применяется сразу.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc).",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.render_scale; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.render_scale = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "graphics.fov", .table = "Graphics", .key = "fov", .category = Category::Graphics, .type = Type::Int,
        .apply = Apply::Live, .needs = Capability::RenderScale, .min = 80, .max = 130, .ui_min = 80, .ui_max = 130,
        .step = 5, .unit = "%", .en = "Field of view", .ru = "Поле зрения",
        .help_en = "The player camera's field of view against the game's own (its aiming and sprint changes stay). "
                   "Cut-scenes keep theirs. The picture's shape already widens the view on wide displays (Hor+). "
                   "Applies at once.",
        .help_ru = "Поле зрения камеры игрока относительно собственного (её изменения при прицеливании и беге "
                   "сохраняются). В кат-сценах — своё. Широкий монитор и так расширяет обзор по бокам (Hor+). "
                   "Применяется сразу.",
        .why_en = "Needs SecondSonPC's own GPU backend (--gpu-backend sspc).",
        .why_ru = "Нужен собственный графический бэкенд SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.fov); },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.fov = static_cast<u32>(std::get<s64>(x)); },
    });
    // ------------------------------------------------------------------ Graphics: post effects (R11 postfx)
    v.push_back(Desc{
        .id = "graphics.motionBlur", .table = "Graphics", .key = "motionBlur", .category = Category::Graphics,
        .type = Type::Bool, .apply = Apply::Live, .en = "Motion blur", .ru = "Размытие в движении",
        .help_en = "The game's own blur of fast movement (the camera and objects). Off: a sharp picture; the motion "
                   "data of the game's anti-aliasing and of DLSS / FSR does not change. Applies at once.",
        .help_ru = "Собственное размытие игры при быстром движении (камеры и объектов). «Выкл.» — чёткая картинка; "
                   "данные движения для сглаживания игры и для DLSS / FSR не меняются. Применяется сразу.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.motion_blur; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.motion_blur = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "graphics.motionBlurStrength", .table = "Graphics", .key = "motionBlurStrength",
        .category = Category::Graphics, .type = Type::Int, .apply = Apply::Live, .min = 10, .max = 100, .ui_min = 10,
        .ui_max = 100, .step = 10, .unit = "%", .en = "Motion blur strength", .ru = "Сила размытия",
        .help_en = "The length of the game's motion blur: 100 % is the game's own. Applies at once.",
        .help_ru = "Длина размытия в движении: 100 % — как в игре. Применяется сразу.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.motion_blur_strength); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.motion_blur_strength = static_cast<u32>(std::get<s64>(x));
        },
        .requires_on = "graphics.motionBlur",
    });
    // (film grain: the pass and its parameter exist, but no place played so far sets it - not offered; depth of
    // field: see kMissing; R11_POSTFX.md)
    // ------------------------------------------------------------------ Upscaling & frame generation
    v.push_back(Desc{
        .id = "upscaling.upscaler", .table = "Graphics", .key = "upscaler", .category = Category::Upscaling,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::RenderScale, .choices = kUpscalers,
        .en = "Upscaling technology", .ru = "Технология масштабирования",
        .help_en = "The game renders its 3D scene at the quality's lower resolution and the upscaler rebuilds the "
                   "picture at the game's resolution from it (motion vectors, depth and a sub-pixel jitter of the scene); the HUD is "
                   "drawn by the game at full resolution on top. DLSS needs a GeForce RTX card; FSR 3.1 runs on any "
                   "GPU. Off: the scene renders at Render scale with the game's own anti-aliasing.",
        .help_ru = "Игра рендерит 3D-сцену в пониженном разрешении выбранного качества, а апскейлер восстанавливает "
                   "из неё картинку в разрешении игры (векторы движения, глубина и субпиксельный сдвиг сцены); интерфейс игра "
                   "рисует поверх в полном разрешении. DLSS нужна видеокарта GeForce RTX; FSR 3.1 работает на любой. "
                   "«Выключено» — сцена рендерится по «Масштабу рендеринга» со сглаживанием самой игры.",
        .why_en = "Only SecondSonPC's own GPU backend (--gpu-backend sspc) has the upscalers.",
        .why_ru = "Апскейлеры есть только в собственном графическом бэкенде SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.upscaler; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.upscaler = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "upscaling.quality", .table = "Graphics", .key = "upscaleQuality", .category = Category::Upscaling,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::RenderScale, .choices = kUpscaleQualities,
        .en = "Quality", .ru = "Качество",
        .help_en = "The render resolution of the scene (output: the game's resolution): Quality 1.5x, Balanced 1.7x, Performance 2x, "
                   "Ultra Performance 3x fewer pixels per side. Lower: faster where the GPU limits the frame rate, "
                   "softer picture. Native: anti-aliasing only, at full resolution.",
        .help_ru = "Разрешение рендеринга сцены (вывод — разрешение игры): «Качество» в 1,5 раза, «Баланс» в 1,7, "
                   "«Производительность» в 2, «Ультрапроизводительность» в 3 раза меньше пикселей по стороне. Ниже — "
                   "быстрее там, где кадр упирается в GPU, и мягче картинка. «Нативное» — только сглаживание в полном "
                   "разрешении.",
        .why_en = "Only SecondSonPC's own GPU backend (--gpu-backend sspc) has the upscalers.",
        .why_ru = "Апскейлеры есть только в собственном графическом бэкенде SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.upscale_quality; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.upscale_quality = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "upscaling.sharpness", .table = "Graphics", .key = "upscaleSharpness", .category = Category::Upscaling,
        .type = Type::Int, .apply = Apply::Live, .needs = Capability::RenderScale, .min = 0, .max = 100, .ui_min = 0,
        .ui_max = 100, .step = 5, .unit = "%", .en = "Image sharpness", .ru = "Резкость изображения",
        .help_en = "Contrast-adaptive sharpening (AMD RCAS) of the upscaled picture: SecondSonPC's after DLSS, FSR's own "
                   "with FSR. 0: none.",
        .help_ru = "Контрастно-адаптивное повышение резкости (AMD RCAS) масштабированной картинки: собственное "
                   "SecondSonPC после DLSS, встроенное в FSR — с FSR. 0 — без него.",
        .why_en = "Only SecondSonPC's own GPU backend (--gpu-backend sspc) has the upscalers.",
        .why_ru = "Апскейлеры есть только в собственном графическом бэкенде SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.upscale_sharpness); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.upscale_sharpness = static_cast<u32>(std::get<s64>(x));
        },
    });
    v.push_back(Desc{
        .id = "upscaling.frameGeneration", .table = "Graphics", .key = "frameGeneration",
        .category = Category::Upscaling, .type = Type::Choice, .apply = Apply::Live,
        .needs = Capability::RenderScale, .choices = kFrameGenerations, .en = "Frame generation",
        .ru = "Генерация кадров",
        .help_en = "Generated frames shown between the frames the game renders: the picture moves more smoothly, the "
                   "game itself runs at the same rate (its physics and timing do not change) and input reacts about "
                   "one frame later. Needs an upscaler (its depth and motion). FSR: any GPU; DLSS: GeForce RTX 40/50. "
                   "Paused in menus, loading screens and camera cuts.",
        .help_ru = "Между кадрами, которые рендерит игра, показываются сгенерированные: картинка движется плавнее, "
                   "сама игра работает с прежней частотой (физика и тайминги не меняются), ввод реагирует примерно на "
                   "кадр позже. Нужен апскейлер (его глубина и движение). FSR — любая видеокарта; DLSS — GeForce "
                   "RTX 40/50. В меню, на загрузках и при сменах камеры приостанавливается.",
        .why_en = "Only SecondSonPC's own GPU backend (--gpu-backend sspc) has frame generation.",
        .why_ru = "Генерация кадров есть только в собственном графическом бэкенде SecondSonPC (--gpu-backend sspc).",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.frame_generation; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.frame_generation = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "upscaling.frameGenMultiplier", .table = "Graphics", .key = "frameGenMultiplier",
        .category = Category::Upscaling, .type = Type::Choice, .apply = Apply::Live, .needs = Capability::DlssFrameGen,
        .choices = kFrameGenMultipliers, .en = "Frame generation multiplier", .ru = "Множитель генерации",
        .help_en = "Frames shown per rendered frame. x3 and x4 are DLSS Multi Frame Generation (GeForce RTX 50); FSR "
                   "generates one frame (x2).",
        .help_ru = "Сколько кадров показывается на каждый отрендеренный. ×3 и ×4 — DLSS Multi Frame Generation "
                   "(GeForce RTX 50); FSR генерирует один кадр (×2).",
        .why_en = "x3 / x4 need DLSS Frame Generation, which this GPU or driver does not support (GeForce RTX 40/50, "
                  "Hardware-accelerated GPU scheduling); FSR Frame Generation is always x2.",
        .why_ru = "×3/×4 требуют DLSS Frame Generation, которую эта видеокарта или драйвер не поддерживает (GeForce "
                  "RTX 40/50, аппаратное планирование GPU); FSR Frame Generation всегда ×2.",
        .get = [](const Config& c, const Desc&) -> Value { return std::to_string(c.pc.frame_gen_multiplier); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.frame_gen_multiplier = static_cast<u32>(std::stoul(std::get<std::string>(x)));
        },
    });
    v.push_back(Desc{
        .id = "upscaling.reflex", .table = "Graphics", .key = "reflex", .category = Category::Upscaling,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::Reflex, .en = "NVIDIA Reflex",
        .ru = "NVIDIA Reflex",
        .help_en = "NVIDIA Reflex low latency: the game starts each frame just in time for the GPU instead of queuing "
                   "frames ahead, so the input reaches the screen sooner. Any GeForce (GTX 900 and newer); not with "
                   "FSR frame generation (its own swapchain).",
        .help_ru = "Режим низкой задержки NVIDIA Reflex: игра начинает каждый кадр ровно к моменту, когда GPU "
                   "освобождается, а не копит кадры в очереди, — ввод быстрее доходит до экрана. Любая GeForce "
                   "(GTX 900 и новее); не вместе с генерацией кадров FSR (у неё свой swapchain).",
        .why_en = "Reflex needs an NVIDIA GPU with VK_NV_low_latency2 (a recent GeForce driver) and SecondSonPC's own GPU "
                  "backend.",
        .why_ru = "Reflex требует видеокарту NVIDIA с VK_NV_low_latency2 (свежий драйвер GeForce) и собственный "
                  "графический бэкенд SecondSonPC.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.reflex; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.reflex = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "upscaling.output", .category = Category::Upscaling, .type = Type::Bool, .apply = Apply::Unavailable,
        .en = "Output resolution: the game's resolution", .ru = "Выходное разрешение: разрешение игры",
        .why_en = "The upscaler writes the game's own display buffer, under its HUD: its size is Display - Resolution "
                  "(the window then shows that picture).",
        .why_ru = "Апскейлер пишет в собственный буфер дисплея игры, под её интерфейс: его размер — «Экран» → "
                  "«Разрешение» (окно затем показывает эту картинку).",
    });
    struct Missing {
        std::string_view id, en, ru, why_en, why_ru;
        u32 cap;
    };
    constexpr Missing kMissing[] = {
        {"graphics.textures", "Texture quality", "Качество текстур",
         "The game loads one set of textures; another quality needs replaced resources or filtering in the backend "
         "(not researched).",
         "Игра загружает один набор текстур; другое качество требует подмены ресурсов или фильтрации в бэкенде "
         "(не исследовано).",
         0},
        {"graphics.anisotropy", "Anisotropic filtering", "Анизотропная фильтрация",
         "Possible in SecondSonPC's backend through its samplers (P4.6); not on the bridge.",
         "Реализуемо в собственном бэкенде через его семплеры (P4.6); не в мосте.", 0},
        {"graphics.shadows", "Shadow quality", "Качество теней",
         "The game sets its shadow map sizes; changing them is a game or backend patch (not researched).",
         "Размер теневых карт задаёт игра; изменение — патч игры или бэкенда (не исследовано).", 0},
        {"graphics.effects", "Effects quality", "Качество эффектов",
         "The game has no such option; not researched.", "В игре нет такого параметра; не исследовано.", 0},
        {"graphics.antiAliasing", "Anti-aliasing", "Сглаживание",
         "The game does its own anti-aliasing; a separate option needs backend work (P4.6+).",
         "Сглаживание выполняет сама игра; отдельная настройка требует работы в бэкенде (P4.6+).", 0},
        {"graphics.depthOfField", "Depth of field", "Глубина резкости",
         "The game's bokeh depth of field has its own off switch, but switching it (even with a forced radius) "
         "changed nothing seen in play; not offered until a scene shows it (R11_POSTFX.md).",
         "У боке-глубины резкости игры есть свой выключатель, но его переключение (даже с принудительным радиусом) "
         "в игре ничего видимого не меняло; не предлагается, пока не найдена сцена с ней (R11_POSTFX.md).",
         0},
    };
    for (const auto& m : kMissing) {
        v.push_back(Desc{.id = m.id, .category = Category::Graphics, .type = Type::Bool, .apply = Apply::Unavailable,
                         .needs = m.cap, .en = m.en, .ru = m.ru, .why_en = m.why_en, .why_ru = m.why_ru});
    }

    // ------------------------------------------------------------------ Audio
    v.push_back(Desc{
        .id = "audio.volume", .table = "Audio", .key = "volume", .category = Category::Audio, .type = Type::Int,
        .apply = Apply::Live, .needs = Capability::LiveVolume, .min = 0, .max = 100, .ui_min = 0, .ui_max = 100,
        .step = 5, .unit = "%", .en = "Master volume", .ru = "Общая громкость",
        .help_en = "SecondSonPC's master volume, on top of the game's own sound settings (music, effects, speech are "
                   "in the game's Options - Audio). Heard at once.",
        .help_ru = "Общая громкость SecondSonPC поверх настроек звука самой игры (музыка, эффекты и речь — в "
                   "«Параметры» → «Звук» игры). Слышно сразу.",
        .why_en = "SecondSonPC's own audio output (--native audio) has no master volume yet; the bridge's has.",
        .why_ru = "У собственного аудиовывода SecondSonPC (--native audio) пока нет общей громкости; у моста есть.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.volume); },
        .set = [](Config& c, const Desc&, const Value& x) { c.volume = static_cast<u32>(std::get<s64>(x)); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "audio.device", .table = "Audio", .key = "device", .category = Category::Audio, .type = Type::Text,
        .apply = Apply::Restart, .needs = Capability::LiveVolume, .en = "Output device", .ru = "Устройство вывода",
        .help_en = "The playback device. System default: the Windows default device.",
        .help_ru = "Устройство воспроизведения. «По умолчанию» — устройство Windows по умолчанию.",
        .why_en = "SecondSonPC's own audio output (--native audio) always follows the Windows default device.",
        .why_ru = "Собственный аудиовывод SecondSonPC (--native audio) всегда следует устройству Windows по "
                  "умолчанию.",
        .get = [](const Config& c, const Desc&) -> Value { return c.audio_device; },
        .set = [](Config& c, const Desc&, const Value& x) { c.audio_device = std::get<std::string>(x); },
        .legacy_parse = true,
    });

    // ------------------------------------------------------------------ Controls (mouse)
    v.push_back(Desc{
        .id = "input.mouseSensitivity", .table = "Input", .key = "mouseSensitivity", .category = Category::Controls,
        .type = Type::Float, .apply = Apply::Live, .min = 0.01, .max = 100.0, .ui_min = 0.1, .ui_max = 5.0,
        .step = 0.05, .unit = "x", .en = "Mouse sensitivity", .ru = "Чувствительность мыши",
        .help_en = "How fast the mouse turns the camera. The mouse drives the game's right stick, so the game's own "
                   "Aim Sensitivity acts on it too.",
        .help_ru = "Скорость поворота камеры мышью. Мышь управляет правым стиком игры, поэтому «Чувствительность "
                   "прицела» в настройках игры тоже на неё действует.",
        .get = [](const Config& c, const Desc&) -> Value { return c.input.mouse_sensitivity; },
        .set = [](Config& c, const Desc&, const Value& x) { c.input.mouse_sensitivity = std::get<double>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "input.invertY", .table = "Input", .key = "invertY", .category = Category::Controls, .type = Type::Bool,
        .apply = Apply::Live, .en = "Invert mouse vertically", .ru = "Инвертировать мышь по вертикали",
        .help_en = "Mouse up turns the camera down. The game's own inversion option also acts on the mouse.",
        .help_ru = "Мышь вверх — камера вниз. Инверсия в настройках игры тоже действует на мышь.",
        .get = [](const Config& c, const Desc&) -> Value { return c.input.invert_y; },
        .set = [](Config& c, const Desc&, const Value& x) { c.input.invert_y = std::get<bool>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "input.mouseDeadzone", .table = "Input", .key = "mouseDeadzone", .category = Category::Controls,
        .type = Type::Float, .apply = Apply::Live, .min = 0.0, .max = 0.9, .ui_min = 0.0, .ui_max = 0.5,
        .step = 0.01, .en = "Mouse dead zone", .ru = "Мёртвая зона мыши",
        .help_en = "The stick deflection a slow mouse movement starts from (skips the game's dead zone). Lower: "
                   "smoother slow movements.",
        .help_ru = "Отклонение стика, с которого начинается поворот при медленном движении мыши (обход мёртвой зоны "
                   "игры). Меньше — плавнее медленные движения.",
        .get = [](const Config& c, const Desc&) -> Value { return c.input.mouse_deadzone; },
        .set = [](Config& c, const Desc&, const Value& x) { c.input.mouse_deadzone = std::get<double>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "input.captureMouse", .table = "Input", .key = "captureMouse", .category = Category::Controls,
        .type = Type::Bool, .apply = Apply::Live, .en = "Capture the cursor", .ru = "Захватывать курсор",
        .help_en = "The cursor is hidden and kept in the window while it has focus; Esc gives it back and takes it "
                   "again. Off: keyboard only.",
        .help_ru = "Курсор скрыт и удерживается в окне, пока оно активно; Esc отпускает и снова захватывает. "
                   "Выключено — только клавиатура.",
        .get = [](const Config& c, const Desc&) -> Value { return c.input.capture_mouse; },
        .set = [](Config& c, const Desc&, const Value& x) { c.input.capture_mouse = std::get<bool>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "input.motionSensitivity", .table = "Input", .key = "motionSensitivity",
        .category = Category::Controls, .type = Type::Float, .apply = Apply::Live, .min = 0.01, .max = 100.0,
        .ui_min = 0.1, .ui_max = 5.0, .step = 0.05, .unit = "x", .en = "Motion mode sensitivity (G)",
        .ru = "Чувствительность наклона (G)",
        .help_en = "Motion mode (graffiti): how fast the mouse turns the controller held sideways.",
        .help_ru = "Режим G (граффити): скорость поворота контроллера, удерживаемого боком, мышью.",
        .get = [](const Config& c, const Desc&) -> Value { return c.input.motion_sensitivity; },
        .set = [](Config& c, const Desc&, const Value& x) { c.input.motion_sensitivity = std::get<double>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "input.sideways", .table = "Input", .key = "sideways", .category = Category::Controls,
        .type = Type::Choice, .apply = Apply::Live, .choices = kSideways, .en = "Controller position in motion mode",
        .ru = "Положение контроллера в режиме G",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.input.sideways_right_up ? "right-up" : "left-up");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.input.sideways_right_up = std::get<std::string>(x) == "right-up";
        },
        .legacy_parse = true,
    });

    // ------------------------------------------------------------------ Keyboard (bindings)
    for (size_t a = 0; a < Input::kActionCount; ++a) {
        const auto action = static_cast<Input::Action>(a);
        r.binding_ids[a] = fmt::format("keyboard.{}", Input::ActionName(action));
        const auto& t = kActionText[a];
        v.push_back(Desc{.id = r.binding_ids[a], .table = "Keyboard", .key = Input::ActionName(action),
                         .category = Category::Keyboard, .type = Type::Binding, .apply = Apply::Live, .en = t.en,
                         .ru = t.ru, .help_en = t.help_en, .help_ru = t.help_ru, .get = &GetBinding,
                         .set = &SetBinding, .action = static_cast<s32>(a), .legacy_parse = true});
    }

    // ------------------------------------------------------------------ Gamepad
    v.push_back(Desc{
        .id = "gamepad.vibration", .table = "Gamepad", .key = "vibration", .category = Category::Gamepad,
        .type = Type::Int, .apply = Apply::Live, .needs = Capability::RealController, .min = 0, .max = 100,
        .ui_min = 0, .ui_max = 100, .step = 10, .unit = "%", .en = "Vibration", .ru = "Вибрация",
        .help_en = "Rumble strength of a real controller relative to what the game asks for; 0 % turns it off. "
                   "Applies from the game's next rumble command.",
        .help_ru = "Сила вибрации реального контроллера относительно запроса игры; 0 % — выключена. Применяется со "
                   "следующей команды вибрации игры.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.vibration); },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.vibration = static_cast<u32>(std::get<s64>(x)); },
    });
    v.push_back(Desc{
        .id = "gamepad.cameraSensitivity", .table = "Gamepad", .key = "cameraSensitivity",
        .category = Category::Gamepad, .type = Type::Float, .apply = Apply::Live,
        .needs = Capability::RealController, .min = 0.25, .max = 2.0, .ui_min = 0.25, .ui_max = 2.0,
        .step = 0.05, .unit = "x", .en = "Camera sensitivity (stick)", .ru = "Чувствительность камеры (стик)",
        .help_en = "Multiplies the right stick deflection of a real controller. Acts on top of the game's Aim "
                   "Sensitivity.",
        .help_ru = "Множитель отклонения правого стика реального контроллера. Действует поверх «Чувствительности "
                   "прицела» в настройках игры.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.camera_sensitivity; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.camera_sensitivity = std::get<double>(x); },
    });
    v.push_back(Desc{
        .id = "gamepad.invertCameraX", .table = "Gamepad", .key = "invertCameraX", .category = Category::Gamepad,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::RealController,
        .en = "Invert camera horizontally", .ru = "Инвертировать камеру по горизонтали",
        .help_en = "Mirrors the right stick of a real controller. The game has its own inversion option: do not turn "
                   "on both.",
        .help_ru = "Зеркалит правый стик реального контроллера. В настройках игры есть своя инверсия — не включайте "
                   "обе.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.invert_camera_x; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.invert_camera_x = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "gamepad.invertCameraY", .table = "Gamepad", .key = "invertCameraY", .category = Category::Gamepad,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::RealController,
        .en = "Invert camera vertically", .ru = "Инвертировать камеру по вертикали",
        .help_en = "Mirrors the right stick of a real controller. The game has its own inversion option: do not turn "
                   "on both.",
        .help_ru = "Зеркалит правый стик реального контроллера. В настройках игры есть своя инверсия — не включайте "
                   "обе.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.invert_camera_y; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.invert_camera_y = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "gamepad.menuButton", .table = "Gamepad", .key = "menuButton", .category = Category::Gamepad,
        .type = Type::Choice, .apply = Apply::Live, .choices = kMenuButtons, .en = "Controller shortcut to this menu",
        .ru = "Быстрый вызов с контроллера",
        .help_en = "This window opens from the game's Settings screen (Options / Esc), its last item \"Additional "
                   "Settings\" (✕, Space or a click); here, a shortcut besides it. The PS (Guide) button never reaches "
                   "a PS4 game, so it is free; on Windows the Xbox Guide button may open the Game Bar, and Steam may "
                   "take the PS button.",
        .help_ru = "Это окно открывается из экрана «Настройки» игры (Options / Esc), последний пункт «Дополнительные "
                   "настройки» (✕, Пробел или щелчок); здесь — дополнительный быстрый вызов. Кнопка PS (Guide) не "
                   "доходит до игр PS4, поэтому свободна; в Windows кнопка Guide Xbox может открыть Game Bar, а Steam "
                   "— забрать кнопку PS.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.menu_button; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.menu_button = std::get<std::string>(x); },
    });

    // ------------------------------------------------------------------ Interface
    v.push_back(Desc{
        .id = "interface.language", .table = "Interface", .key = "language", .category = Category::Interface,
        .type = Type::Choice, .apply = Apply::Live, .choices = kUiLanguages, .en = "Menu language",
        .ru = "Язык меню SecondSonPC",
        .help_en = "The language of this menu. The game's own language is in the Game section.",
        .help_ru = "Язык этого меню. Язык самой игры — в разделе «Игра».",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.ui_language; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.ui_language = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "interface.scale", .table = "Interface", .key = "scale", .category = Category::Interface,
        .type = Type::Int, .apply = Apply::Live, .min = 75, .max = 200, .ui_min = 75, .ui_max = 200, .step = 5,
        .unit = "%", .en = "Interface scale", .ru = "Масштаб интерфейса",
        .help_en = "The size of this window: 100 % follows the game window's height (900 x 700 in a 1080-pixel tall "
                   "window) or the system's display scale, whichever is larger; it never exceeds the game window.",
        .help_ru = "Размер этого окна: при 100 % — по высоте окна игры (900 × 700 в окне высотой 1080 пикселей) или "
                   "по системному масштабу экрана, если он больше; за пределы окна игры не выходит.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.ui_scale); },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.ui_scale = static_cast<u32>(std::get<s64>(x)); },
    });
    v.push_back(Desc{
        .id = "interface.prompts", .table = "Interface", .key = "prompts", .category = Category::Interface,
        .type = Type::Choice, .apply = Apply::Live, .choices = kPrompts, .en = "Button hints",
        .ru = "Подсказки кнопок",
        .help_en = "Button icons in the game's hints and in this menu. Automatic: the device used last (keyboard and "
                   "mouse show the keys bound to each action; an Xbox controller its buttons; a PlayStation controller "
                   "the game's own icons). Connecting a controller does not switch them, using it does.",
        .help_ru = "Значки кнопок в подсказках игры и в этом меню. «Автоматически» — по последнему устройству: для "
                   "клавиатуры и мыши — клавиши, назначенные действиям; для контроллера Xbox — его кнопки; для "
                   "контроллера PlayStation — оригинальные значки игры. Подключение контроллера их не переключает, "
                   "переключает его использование.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.prompts; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.prompts = std::get<std::string>(x); },
    });

    // ------------------------------------------------------------------ Shaders and cache (P4.14, native backend)
    v.push_back(Desc{
        .id = "shaders.precompile", .table = "ShaderCache", .key = "precompile", .category = Category::Shaders,
        .type = Type::Bool, .apply = Apply::Restart, .en = "Prepare shaders at start", .ru = "Готовить шейдеры при запуске",
        .help_en = "Pipelines met in earlier runs are compiled before the game starts, so that they do not stop it "
                   "later. Places never visited are still prepared on the way. Takes effect after a restart.",
        .help_ru = "Конвейеры, встреченные в прошлых запусках, компилируются до начала игры, чтобы не останавливать её "
                   "потом. Места, где игра ещё не была, по-прежнему готовятся на ходу. Вступает в силу после "
                   "перезапуска.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.shader_precompile; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.shader_precompile = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "shaders.showProgress", .table = "ShaderCache", .key = "showProgress", .category = Category::Shaders,
        .type = Type::Bool, .apply = Apply::Restart, .en = "Show the preparation's progress",
        .ru = "Показывать прогресс подготовки",
        .help_en = "A panel over the window while the shaders are prepared; Esc there starts the game at once (the rest "
                   "is prepared in the background). Off: the start waits without it.",
        .help_ru = "Панель поверх окна, пока шейдеры готовятся; Esc на ней сразу запускает игру (остальное готовится в "
                   "фоне). Выключено: запуск ждёт без панели.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.shader_precompile_progress; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.shader_precompile_progress = std::get<bool>(x); },
        .requires_on = "shaders.precompile",
    });
    v.push_back(Desc{
        .id = "shaders.predictAll", .table = "ShaderCache", .key = "predictAll", .category = Category::Shaders,
        .type = Type::Bool, .apply = Apply::Restart, .en = "Compile all the game's shaders before it starts",
        .ru = "Компилировать все шейдеры игры до запуска",
        .help_en = "Before the game starts, a console window compiles the shaders found in the game's files and shows "
                   "the progress (a few minutes once; afterwards a second). Takes effect after a restart.",
        .help_ru = "Перед запуском игры окно консоли компилирует шейдеры, найденные в файлах игры, и показывает "
                   "прогресс (один раз — несколько минут, потом — секунда). Вступает в силу после перезапуска.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.shader_predict; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.shader_predict = std::get<bool>(x); },
    });
    v.push_back(Desc{
        .id = "shaders.maxSize", .table = "ShaderCache", .key = "maxSizeMiB", .category = Category::Shaders,
        .type = Type::Int, .apply = Apply::Restart, .min = 256, .max = 16384, .ui_min = 256, .ui_max = 8192,
        .step = 256, .unit = "MiB", .en = "Shader cache size", .ru = "Размер кэша шейдеров",
        .help_en = "The cache file grows up to this size; past it nothing more is added (Clear starts it again). "
                   "Takes effect after a restart.",
        .help_ru = "Файл кэша растёт до этого размера; дальше новое не добавляется («Очистить» начинает его заново). "
                   "Вступает в силу после перезапуска.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.shader_cache_max_mib); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.shader_cache_max_mib = static_cast<u32>(std::get<s64>(x));
        },
    });
    v.push_back(Desc{
        .id = "shaders.threads", .table = "ShaderCache", .key = "precompileThreads", .category = Category::Shaders,
        .type = Type::Int, .apply = Apply::Restart, .min = 0, .max = 16, .ui_min = 0, .ui_max = 12, .step = 1,
        .en = "Preparation threads (0 = automatic)", .ru = "Потоков подготовки (0 = автоматически)",
        .help_en = "How many processor threads compile shaders at start. Automatic: half the logical processors, at "
                   "most 6; while the game runs, 2 at a low priority.",
        .help_ru = "Сколько потоков процессора компилируют шейдеры при запуске. Автоматически — половина логических "
                   "процессоров, не больше 6; во время игры — 2 с пониженным приоритетом.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.shader_precompile_threads); },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.pc.shader_precompile_threads = static_cast<u32>(std::get<s64>(x));
        },
    });

    // ------------------------------------------------------------------ Game (the emulated PS4 sees these)
    v.push_back(Desc{
        .id = "game.language", .table = "Language", .key = "system", .category = Category::Game,
        .type = Type::Choice, .apply = Apply::Restart, .choices = kGameLanguages, .en = "Game language",
        .ru = "Язык игры",
        .help_en = "The emulated PS4's system language: the game's text and voices. Takes effect after a restart.",
        .help_ru = "Системный язык эмулируемой PS4: текст и озвучка игры. Вступает в силу после перезапуска.",
        .get = [](const Config& c, const Desc&) -> Value { return c.language_locale; },
        .set = [](Config& c, const Desc&, const Value& x) {
            if (const auto* lang = Runtime::FindLanguageByLocale(std::get<std::string>(x))) {
                c.language_locale = std::string(lang->locale);
                c.language_id = lang->id;
            }
        },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "game.enterButton", .table = "System", .key = "enterButton", .category = Category::Game,
        .type = Type::Choice, .apply = Apply::Restart, .choices = kEnterButtons, .en = "Confirm button",
        .ru = "Кнопка подтверждения",
        .help_en = "Which button confirms in the game's menus (a PS4 system setting). Takes effect after a restart.",
        .help_ru = "Какой кнопкой игра подтверждает выбор в меню (системная настройка PS4). Вступает в силу после "
                   "перезапуска.",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.enter_button_cross ? "cross" : "circle");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.enter_button_cross = std::get<std::string>(x) == "cross";
        },
        .legacy_parse = true,
    });

    // ------------------------------------------------------------------ Experiments (settings/experiments.h)
    // Read once by their subsystems at start: all Restart. Not SecondSonPC.toml keys (legacy_parse: its parser skips
    // them) - the launcher's environment is their default, the user file their override.
    const auto experiments = Experiments();
    r.experiment_ids.reserve(experiments.size());
    for (const auto& e : experiments) {
        r.experiment_ids.push_back(ExperimentId(e.env));
    }
    for (size_t i = 0; i < experiments.size(); ++i) {
        const Experiment& e = experiments[i];
        std::string_view requires_on;
        for (size_t j = 0; j < experiments.size() && !e.requires_env.empty(); ++j) {
            if (experiments[j].env == e.requires_env) {
                requires_on = r.experiment_ids[j];
            }
        }
        v.push_back(Desc{
            .id = r.experiment_ids[i], .table = "Experiments", .key = e.env, .category = Category::Experiments,
            .type = e.type, .apply = Apply::Restart, .choices = e.choices, .en = e.en, .ru = e.ru,
            .help_en = e.help_en, .help_ru = e.help_ru, .get = GetExperiment, .set = SetExperiment,
            .legacy_parse = true, .requires_on = requires_on,
        });
    }
}

const Registry& Instance() {
    static Registry registry;
    static const bool built = (Build(registry), true);
    (void)built;
    return registry;
}

bool ValidInput(Input::InputCode in) {
    switch (in.kind) {
    case Input::InputCode::Kind::Key:
        return in.code > 0 && in.code < 512;
    case Input::InputCode::Kind::MouseButton:
        return in.code > 0 && in.code < 32;
    case Input::InputCode::Kind::MouseWheel:
        return in.code < 4;
    }
    return false;
}

} // namespace

std::span<const Desc> All() {
    return Instance().all;
}

const Desc* Find(std::string_view id) {
    for (const auto& d : Instance().all) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

const Desc* FindByKey(std::string_view table, std::string_view key) {
    for (const auto& d : Instance().all) {
        if (!d.table.empty() && d.table == table && d.key == key) {
            return &d;
        }
    }
    return nullptr;
}

std::string_view CategoryId(Category category) {
    constexpr std::array<std::string_view, kCategoryCount> kIds{"display",  "graphics", "audio",     "controls",
                                                                "keyboard", "gamepad",  "interface", "game",
                                                                "shaders",  "upscaling", "experiments"};
    return kIds[static_cast<size_t>(category)];
}

std::string_view CategoryLabel(Category category, bool ru) {
    constexpr std::array<std::string_view, kCategoryCount> kEn{"Display", "Graphics", "Audio",     "Mouse",
                                                               "Keys",    "Controller", "Interface", "Game",
                                                               "Shaders & cache", "Upscaling & frame generation",
                                                               "Experiments"};
    constexpr std::array<std::string_view, kCategoryCount> kRu{"Экран",   "Графика",   "Звук",      "Мышь",
                                                               "Клавиши", "Геймпад",   "Интерфейс", "Игра",
                                                               "Шейдеры и кэш", "Масштабирование и генерация кадров",
                                                               "Эксперименты"};
    return (ru ? kRu : kEn)[static_cast<size_t>(category)];
}

std::string_view ActionLabel(Input::Action action, bool ru) {
    const auto& t = kActionText[static_cast<size_t>(action)];
    return ru ? t.ru : t.en;
}

std::string_view ChoiceLabel(const Desc& desc, std::string_view value, bool ru) {
    for (const auto& c : desc.choices) {
        if (c.value == value) {
            return ru ? c.ru : c.en;
        }
    }
    return {};
}

bool Validate(const Desc& desc, const Value& value, std::string& error) {
    switch (desc.type) {
    case Type::Bool:
        if (!std::holds_alternative<bool>(value)) {
            error = "true/false expected";
            return false;
        }
        return true;
    case Type::Int: {
        if (!std::holds_alternative<s64>(value)) {
            error = "an integer expected";
            return false;
        }
        const auto x = static_cast<double>(std::get<s64>(value));
        if (x < desc.min || x > desc.max) {
            error = fmt::format("an integer in [{}, {}] expected", desc.min, desc.max);
            return false;
        }
        return true;
    }
    case Type::Float: {
        if (!std::holds_alternative<double>(value)) {
            error = "a number expected";
            return false;
        }
        const double x = std::get<double>(value);
        if (!std::isfinite(x) || x < desc.min || x > desc.max) {
            error = fmt::format("a number in [{}, {}] expected", desc.min, desc.max);
            return false;
        }
        return true;
    }
    case Type::Choice: {
        if (!std::holds_alternative<std::string>(value)) {
            error = "a string expected";
            return false;
        }
        const auto& s = std::get<std::string>(value);
        if (std::ranges::none_of(desc.choices, [&](const Choice& c) { return c.value == s; }) &&
            (desc.accepts == nullptr || !desc.accepts(s))) {
            std::string list;
            for (const auto& c : desc.choices) {
                list += fmt::format("{}\"{}\"", list.empty() ? "" : ", ", c.value);
            }
            error = fmt::format("one of {} expected{}", list,
                                desc.id == "display.frameRateLimit" ? " (or a number 20..1000)"
                                : desc.id == "display.resolution"   ? " (or \"WxH\")"
                                                                    : "");
            return false;
        }
        return true;
    }
    case Type::Text: {
        if (!std::holds_alternative<std::string>(value)) {
            error = "a string expected";
            return false;
        }
        const auto& s = std::get<std::string>(value);
        if (s.size() > 256 ||
            std::ranges::any_of(s, [](char ch) { return static_cast<unsigned char>(ch) < 0x20; })) {
            error = "up to 256 bytes without control characters expected";
            return false;
        }
        return true;
    }
    case Type::Binding: {
        if (!std::holds_alternative<Bindings>(value)) {
            error = "a key name or a list of key names expected";
            return false;
        }
        const auto& b = std::get<Bindings>(value);
        if (b.size() > 4) {
            error = "at most 4 inputs per action";
            return false;
        }
        for (const auto& in : b) {
            if (!ValidInput(in)) {
                error = "an unknown input";
                return false;
            }
            if (Input::IsReservedInput(in)) {
                error = fmt::format("{} is SecondSonPC's own hotkey (F10 FPS, F11 full screen)",
                                    Input::InputName(in));
                return false;
            }
        }
        return true;
    }
    }
    error = "unknown type";
    return false;
}

bool Equal(const Value& a, const Value& b) {
    return a == b;
}

std::string Describe(const Value& value) {
    if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
    }
    if (const auto* i = std::get_if<s64>(&value)) {
        return std::to_string(*i);
    }
    if (const auto* d = std::get_if<double>(&value)) {
        return fmt::format("{}", *d);
    }
    if (const auto* s = std::get_if<std::string>(&value)) {
        return fmt::format("\"{}\"", *s);
    }
    std::string out = "[";
    for (const auto& in : std::get<Bindings>(value)) {
        out += fmt::format("{}{}", out.size() > 1 ? ", " : "", Input::InputName(in));
    }
    return out + "]";
}

} // namespace SecondSon::Settings
