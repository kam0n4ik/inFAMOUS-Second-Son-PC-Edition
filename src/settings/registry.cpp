// SecondSonPC — the registry of PC settings (see registry.h). The table below is the one place that says which
// settings exist, where they are stored and how they apply; the menu, the user file and the tests read it.
#include "settings/registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "input/binding_rules.h"
#include "runtime/language.h"

namespace SecondSon::Settings {

namespace {

using App::Config;

// ---- choices

constexpr Choice kFullscreenModes[] = {
    {"borderless", "Borderless", "Без рамки"},
    {"exclusive", "Exclusive", "Эксклюзивный"},
};
constexpr Choice kPresentModes[] = {
    {"fifo", "On (FIFO)", "Вкл. (FIFO)"},
    {"mailbox", "Fast (Mailbox)", "Быстрая (Mailbox)"},
    {"immediate", "Off (Immediate)", "Выкл. (Immediate)"},
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
};

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
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "display.fullscreenMode", .table = "Display", .key = "fullscreenMode", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Live, .needs = Capability::Window, .choices = kFullscreenModes,
        .en = "Full screen type", .ru = "Тип полноэкранного режима",
        .help_en = "Borderless: a window over the desktop, quick Alt+Tab. Exclusive: the display is given to the game "
                   "at its current resolution.",
        .help_ru = "«Без рамки» — окно поверх рабочего стола, быстрый Alt+Tab. «Эксклюзивный» — дисплей отдаётся "
                   "игре в его текущем разрешении.",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.exclusive_fullscreen ? "exclusive" : "borderless");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            c.exclusive_fullscreen = std::get<std::string>(x) == "exclusive";
        },
        .legacy_parse = true, .requires_on = "display.fullscreen",
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
        .help_en = "The display the game window opens on. Empty: the primary display.",
        .help_ru = "Монитор, на котором открывается окно игры. Пусто — основной монитор.",
        .get = [](const Config& c, const Desc&) -> Value { return c.pc.monitor; },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.monitor = std::get<std::string>(x); },
    });
    v.push_back(Desc{
        .id = "display.presentMode", .table = "Display", .key = "presentMode", .category = Category::Display,
        .type = Type::Choice, .apply = Apply::Restart, .needs = Capability::BridgePresenter,
        .choices = kPresentModes, .en = "Vertical sync", .ru = "Вертикальная синхронизация",
        .help_en = "How frames are shown (Vulkan present mode). FIFO: classic sync, no tearing. Mailbox: no tearing, "
                   "lower latency. Immediate: no sync, tearing possible. Takes effect after a restart: changing it "
                   "while playing means recreating the swapchain inside the graphics backend.",
        .help_ru = "Способ показа кадров (Vulkan present mode). FIFO — классическая синхронизация без разрывов; "
                   "Mailbox — без разрывов и с меньшей задержкой; Immediate — без синхронизации, возможны разрывы. "
                   "Вступает в силу после перезапуска: смена на лету требует пересоздания swapchain в графическом "
                   "бэкенде.",
        .why_en = "SecondSonPC's own presenter (--gpu-backend sspc) always picks Mailbox for now; passing the setting "
                  "to it is P4.6 integration.",
        .why_ru = "Собственный презентер (--gpu-backend sspc) пока всегда выбирает Mailbox; передача настройки — "
                  "интеграция P4.6.",
        .get = [](const Config& c, const Desc&) -> Value {
            return std::string(c.present_mode == "Fifo" ? "fifo" : c.present_mode == "Immediate" ? "immediate"
                                                                                                    : "mailbox");
        },
        .set = [](Config& c, const Desc&, const Value& x) {
            const auto& s = std::get<std::string>(x);
            c.present_mode = s == "fifo" ? "Fifo" : s == "immediate" ? "Immediate" : "Mailbox";
        },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "display.showFps", .table = "Display", .key = "showFps", .category = Category::Display,
        .type = Type::Bool, .apply = Apply::Live, .needs = Capability::BridgePresenter, .en = "FPS counter",
        .ru = "Счётчик FPS", .help_en = "A frame counter in the corner of the screen (F10).",
        .help_ru = "Счётчик кадров в углу экрана (F10).", .why_en = kNeedsBridgePresenterEn,
        .why_ru = kNeedsBridgePresenterRu,
        .get = [](const Config& c, const Desc&) -> Value { return c.show_fps; },
        .set = [](Config& c, const Desc&, const Value& x) { c.show_fps = std::get<bool>(x); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "display.frameLimit", .category = Category::Display, .type = Type::Bool,
        .apply = Apply::Unavailable, .needs = Capability::FrameLimiter, .en = "Frame rate limit",
        .ru = "Ограничение FPS",
        .why_en = "Needs a limiter in the frame presentation path (VideoOut/presenter) - the graphics backend, P4.6. "
                  "The game itself offers a 30 fps limit (Options - Game - Frame Rate).",
        .why_ru = "Нужен ограничитель в пути показа кадров (VideoOut/презентер) — это графический бэкенд, этап P4.6. "
                  "В самой игре есть ограничение 30 к/с (Параметры → Игра → Частота кадров).",
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
    struct Missing {
        std::string_view id, en, ru, why_en, why_ru;
        u32 cap;
    };
    constexpr Missing kMissing[] = {
        {"graphics.renderScale", "Render resolution", "Разрешение рендеринга",
         "The game's render targets are fixed at 1920x1080; another size means recreating the targets and scaling "
         "the viewports in SecondSonPC's backend (P4.6+). The window only scales the finished frame now.",
         "Цели рендеринга игры фиксированы на 1920×1080; другой размер требует пересоздания целей и масштабирования "
         "вьюпортов в собственном бэкенде (P4.6+). Сейчас окно только масштабирует готовый кадр.",
         Capability::RenderScale},
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
        {"graphics.motionBlur", "Motion blur", "Размытие в движении",
         "The game has no such option; turning it off means changing the game's shaders (not researched).",
         "В игре нет такого параметра; отключение — правка шейдеров игры (не исследовано).", 0},
        {"graphics.depthOfField", "Depth of field", "Глубина резкости",
         "The game has no such option; turning it off means changing the game's shaders (not researched).",
         "В игре нет такого параметра; отключение — правка шейдеров игры (не исследовано).", 0},
        {"graphics.upscaler", "DLSS / FSR 2+ / XeSS", "DLSS / FSR 2+ / XeSS",
         "Temporal upscalers need motion vectors and depth from the game and SecondSonPC's own backend: a phase of its "
         "own.",
         "Временным масштабировщикам нужны векторы движения и глубина от игры и собственный бэкенд — отдельная фаза.",
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
        .step = 5, .unit = "%", .en = "Volume", .ru = "Громкость",
        .help_en = "SecondSonPC's master volume, on top of the game's own sound settings.",
        .help_ru = "Общая громкость SecondSonPC поверх настроек звука самой игры.",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.volume); },
        .set = [](Config& c, const Desc&, const Value& x) { c.volume = static_cast<u32>(std::get<s64>(x)); },
        .legacy_parse = true,
    });
    v.push_back(Desc{
        .id = "audio.device", .table = "Audio", .key = "device", .category = Category::Audio, .type = Type::Text,
        .apply = Apply::Restart, .en = "Output device", .ru = "Устройство вывода",
        .help_en = "The playback device. Empty: the Windows default. Takes effect after a restart.",
        .help_ru = "Устройство воспроизведения. Пусто — устройство Windows по умолчанию. Вступает в силу после "
                   "перезапуска.",
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
        .type = Type::Choice, .apply = Apply::Live, .choices = kMenuButtons, .en = "Open this menu from a controller",
        .ru = "Открытие меню с контроллера",
        .help_en = "The PS (Guide) button never reaches a PS4 game, so it is free; on Windows the Xbox Guide button "
                   "may open the Game Bar, and Steam may take the PS button.",
        .help_ru = "Кнопка PS (Guide) не доходит до игр PS4, поэтому свободна; в Windows кнопка Guide Xbox может "
                   "открыть Game Bar, а Steam — забрать кнопку PS.",
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
        .help_en = "The size of this menu relative to the window height (100 % = a 1080-pixel tall window).",
        .help_ru = "Размер этого меню относительно высоты окна (100 % — окно высотой 1080 пикселей).",
        .get = [](const Config& c, const Desc&) -> Value { return static_cast<s64>(c.pc.ui_scale); },
        .set = [](Config& c, const Desc&, const Value& x) { c.pc.ui_scale = static_cast<u32>(std::get<s64>(x)); },
    });
    v.push_back(Desc{
        .id = "interface.prompts", .table = "Interface", .key = "prompts", .category = Category::Interface,
        .type = Type::Choice, .apply = Apply::Live, .choices = kPrompts, .en = "Button hints in this menu",
        .ru = "Подсказки кнопок в меню",
        .help_en = "Button symbols in SecondSonPC's menu. Automatic: the last device used. The game's own hints stay "
                   "DualShock 4 symbols (replacing them is a separate task).",
        .help_ru = "Обозначения кнопок в меню SecondSonPC. «Автоматически» — по последнему устройству ввода. "
                   "Подсказки самой игры остаются символами DualShock 4 (их замена — отдельная задача).",
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
                                                                "shaders"};
    return kIds[static_cast<size_t>(category)];
}

std::string_view CategoryLabel(Category category, bool ru) {
    constexpr std::array<std::string_view, kCategoryCount> kEn{"Display", "Graphics", "Audio",     "Mouse",
                                                               "Keys",    "Controller", "Interface", "Game",
                                                               "Shaders & cache"};
    constexpr std::array<std::string_view, kCategoryCount> kRu{"Экран",   "Графика",   "Звук",      "Мышь",
                                                               "Клавиши", "Геймпад",   "Интерфейс", "Игра",
                                                               "Шейдеры и кэш"};
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
        if (std::ranges::none_of(desc.choices, [&](const Choice& c) { return c.value == s; })) {
            std::string list;
            for (const auto& c : desc.choices) {
                list += fmt::format("{}\"{}\"", list.empty() ? "" : ", ", c.value);
            }
            error = fmt::format("one of {} expected", list);
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
                error = fmt::format("{} is SecondSonPC's own hotkey (F1 menu, F10 FPS, F11 full screen)",
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
