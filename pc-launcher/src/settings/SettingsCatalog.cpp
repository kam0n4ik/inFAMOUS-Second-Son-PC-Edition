#include "settings/SettingsCatalog.h"

#include "profiles/GameProfile.h"

namespace Launcher {

namespace {

using S = SettingSection;
using T = SettingType;

SettingDef Def(QString id, QString table, QString key, S section, T type, QString ru, QString en) {
    SettingDef d;
    d.id = std::move(id);
    d.table = std::move(table);
    d.key = std::move(key);
    d.section = section;
    d.type = type;
    d.ru = std::move(ru);
    d.en = std::move(en);
    return d;
}

SettingDef& Help(SettingDef& d, QString ru, QString en) {
    d.helpRu = std::move(ru);
    d.helpEn = std::move(en);
    return d;
}

SettingDef Experiment(QString env, QString groupRu, QString groupEn, QString on, QString off, QString ru, QString en,
                      QString helpRu, QString helpEn) {
    SettingDef d = Def(QStringLiteral("experiments.") + env, QStringLiteral("Experiments"), env, S::Experiments,
                       T::Bool, std::move(ru), std::move(en));
    d.groupRu = std::move(groupRu);
    d.groupEn = std::move(groupEn);
    d.experiment = true;
    d.on = std::move(on);
    d.off = std::move(off);
    d.helpRu = std::move(helpRu);
    d.helpEn = std::move(helpEn);
    return d;
}

QList<SettingDef> Build() {
    QList<SettingDef> v;
    const QString gScreenRu = QStringLiteral("Монитор и окно"), gScreenEn = QStringLiteral("Display and window");
    const QString gSyncRu = QStringLiteral("Синхронизация и кадры"), gSyncEn = QStringLiteral("Sync and frame rate");
    const QString gResRu = QStringLiteral("Разрешение и интерфейс"), gResEn = QStringLiteral("Resolution and HUD");

    // ------------------------------------------------------------------ Display
    {
        auto d = Def("display.monitor", "Display", "monitor", S::Display, T::Text, "Монитор", "Display");
        d.groupRu = gScreenRu; d.groupEn = gScreenEn;
        d.builtinDefault = QString();
        Help(d, "Монитор, на котором открывается окно игры (имя — как у SDL в игре). Полноэкранный режим занимает этот монитор. "
                "Частота обновления — частота рабочего стола монитора: игра её не меняет.",
             "The display the game window opens on (named as the game's SDL names it). Full screen covers that display. "
             "The refresh rate is the display's desktop rate: the game does not change it.");
        v << d;
    }
    {
        auto d = Def("display.fullscreen", "Display", "fullscreen", S::Display, T::Bool, "Режим окна", "Window mode");
        d.groupRu = gScreenRu; d.groupEn = gScreenEn;
        d.builtinDefault = false;
        Help(d, "«В окне» или «Полноэкранный без рамки» (окно поверх рабочего стола, быстрый Alt+Tab). F11 переключает режим "
                "во время игры. Эксклюзивного режима Vulkan в рантайме нет.",
             "Windowed, or borderless full screen (a window over the desktop, quick Alt+Tab). F11 toggles it while playing. "
             "The runtime has no Vulkan exclusive mode.");
        v << d;
    }
    {
        auto d = Def("display.fullscreenMode", "Display", "fullscreenMode", S::Display, T::Choice, "Тип полноэкранного режима",
                     "Full screen type");
        d.builtinDefault = QStringLiteral("borderless");
        d.choices = {{"borderless", "Без рамки", "Borderless"}, {"exclusive", "Эксклюзивный", "Exclusive"}};
        v << d; // (shown inside the "Window mode" row)
    }
    {
        auto d = Def("display.width", "Display", "width", S::Display, T::Int, "Ширина окна", "Window width");
        d.min = 640; d.max = 7680; d.builtinDefault = qint64(1280);
        v << d;
        auto h = Def("display.height", "Display", "height", S::Display, T::Int, "Высота окна", "Window height");
        h.min = 360; h.max = 4320; h.builtinDefault = qint64(720);
        v << h;
    }
    {
        auto d = Def("display.presentMode", "Display", "presentMode", S::Display, T::Choice, "Вертикальная синхронизация",
                     "Vertical sync");
        d.groupRu = gSyncRu; d.groupEn = gSyncEn;
        d.builtinDefault = QStringLiteral("mailbox");
        d.choices = {{"fifo", "Вкл.", "On"},
                     {"immediate", "Выкл.", "Off"},
                     {"mailbox", "Выкл., без разрывов (Mailbox)", "Off, without tearing (Mailbox)"}};
        Help(d, "«Вкл.» (FIFO) — без разрывов, не выше частоты монитора. «Выкл.» (IMMEDIATE) — кадр сразу, возможны разрывы. "
                "«Выкл., без разрывов» (MAILBOX) — новейший кадр на каждом обновлении. Ограничение FPS работает в любом режиме.",
             "On (FIFO): no tearing, at most the display's refresh rate. Off (IMMEDIATE): frames at once, tearing possible. "
             "Off without tearing (MAILBOX): the newest frame at each refresh. The FPS limit works in every mode.");
        v << d;
    }
    {
        auto d = Def("display.frameRateLimit", "Display", "frameRateLimit", S::Display, T::Choice, "Ограничение FPS",
                     "Frame rate limit");
        d.groupRu = gSyncRu; d.groupEn = gSyncEn;
        d.builtinDefault = QStringLiteral("60");
        d.acceptsFrameRate = true;
        const quint32 free = GameCaps::FreeFrameRate;
        d.choices = {{"console", "60 (кадр по vblank PS4)", "60 (PS4 vblank)"},
                     {"30", "30", "30"}, {"40", "40", "40"}, {"50", "50", "50"}, {"60", "60", "60"},
                     {"90", "90", "90", 0, free}, {"120", "120", "120", 0, free}, {"144", "144", "144", 0, free},
                     {"165", "165", "165", 0, free}, {"240", "240", "240", 0, free},
                     {"unlimited", "Без ограничения", "Unlimited", 0, free}};
        Help(d, "«60 (кадр по vblank PS4)» — как на консоли. Число или «Без ограничения» — кадр показывается сразу, как готов, "
                "не чаще заданного (часы игры — реальное время). Ограничение 30 к/с самой игры действует.",
             "60 (PS4 vblank): as the console. A number or Unlimited: a frame is shown as soon as it is ready, at most that "
             "many a second (the game's clock is real time). The game's own 30 fps lock still applies.");
        v << d;
    }
    {
        auto d = Def("display.showFps", "Display", "showFps", S::Display, T::Bool, "Счётчик FPS", "FPS counter");
        d.groupRu = gSyncRu; d.groupEn = gSyncEn;
        d.builtinDefault = true;
        Help(d, "Кадры в секунду и время кадра в левом верхнем углу. F10 переключает его во время игры.",
             "Frames per second and the frame time in the top left corner. F10 switches it while playing.");
        v << d;
    }
    {
        auto d = Def("display.resolution", "Display", "resolution", S::Display, T::Choice, "Разрешение", "Resolution");
        d.groupRu = gResRu; d.groupEn = gResEn;
        d.builtinDefault = QStringLiteral("auto");
        d.needsGame = GameCaps::ResolutionPatch;
        d.acceptsResolution = true;
        d.choices = {{"auto", "Автоматически (окно / монитор)", "Automatic (the window / the display)"},
                     {"1280x720", "1280 × 720 (16:9)", "1280 × 720 (16:9)"}, {"1600x900", "1600 × 900 (16:9)", "1600 × 900 (16:9)"},
                     {"1920x1080", "1920 × 1080 (16:9)", "1920 × 1080 (16:9)"}, {"2560x1440", "2560 × 1440 (16:9)", "2560 × 1440 (16:9)"},
                     {"3840x2160", "3840 × 2160 (16:9)", "3840 × 2160 (16:9)"}, {"1280x800", "1280 × 800 (16:10)", "1280 × 800 (16:10)"},
                     {"1680x1050", "1680 × 1050 (16:10)", "1680 × 1050 (16:10)"}, {"1920x1200", "1920 × 1200 (16:10)", "1920 × 1200 (16:10)"},
                     {"2560x1600", "2560 × 1600 (16:10)", "2560 × 1600 (16:10)"}, {"2560x1080", "2560 × 1080 (21:9)", "2560 × 1080 (21:9)"},
                     {"3440x1440", "3440 × 1440 (21:9)", "3440 × 1440 (21:9)"}, {"3840x1600", "3840 × 1600 (21:9)", "3840 × 1600 (21:9)"},
                     {"3840x1080", "3840 × 1080 (32:9)", "3840 × 1080 (32:9)"}, {"5120x1440", "5120 × 1440 (32:9)", "5120 × 1440 (32:9)"},
                     {"800x600", "800 × 600 (4:3)", "800 × 600 (4:3)"}, {"1024x768", "1024 × 768 (4:3)", "1024 × 768 (4:3)"},
                     {"1440x1080", "1440 × 1080 (4:3)", "1440 × 1080 (4:3)"}, {"1600x1200", "1600 × 1200 (4:3)", "1600 × 1200 (4:3)"}};
        Help(d, "Собственное разрешение игры: картинка и интерфейс рисуются в этом размере. «Автоматически» — размер окна, "
                "а в полноэкранном режиме — монитора. Вступает в силу при следующем запуске.",
             "The game's own resolution: its picture and HUD are drawn at this size. Automatic: the window's size, or the "
             "display's in full screen. Takes effect at the next start.");
        v << d;
    }
    {
        auto d = Def("display.aspectRatio", "Display", "aspectRatio", S::Display, T::Choice, "Соотношение сторон",
                     "Aspect ratio");
        d.groupRu = gResRu; d.groupEn = gResEn;
        d.builtinDefault = QStringLiteral("auto");
        d.needsGame = GameCaps::ResolutionPatch;
        d.choices = {{"auto", "Автоматически (по разрешению)", "Automatic (as the resolution)"}, {"16:9", "16:9", "16:9"},
                     {"16:10", "16:10", "16:10"}, {"21:9", "21:9", "21:9"}, {"32:9", "32:9", "32:9"}, {"4:3", "4:3", "4:3"}};
        Help(d, "Форма картинки. Шире — по бокам видно больше (Hor+); ничего не растягивается. Со следующего запуска.",
             "The picture's shape. Wider shows more at the sides (Hor+); nothing is stretched. From the next start.");
        v << d;
    }
    {
        auto d = Def("display.hudSafeArea", "Display", "hudSafeArea", S::Display, T::Int, "Безопасная зона HUD",
                     "HUD safe area");
        d.groupRu = gResRu; d.groupEn = gResEn;
        d.min = 85; d.max = 100; d.step = 1; d.unit = QStringLiteral("%");
        d.builtinDefault = qint64(100);
        d.needsGame = GameCaps::RenderSwitch;
        Help(d, "Насколько интерфейс отступает от краёв картинки, как «Область отображения» PS4: 100 % — у самых краёв.",
             "How far the HUD keeps from the picture's edges, as the PS4's Display Area: 100 % at the edges.");
        v << d;
    }
    {
        auto d = Def("display.hudArea", "Display", "hudArea", S::Display, T::Choice, "Область HUD", "HUD area");
        d.groupRu = gResRu; d.groupEn = gResEn;
        d.builtinDefault = QStringLiteral("full");
        d.needsGame = GameCaps::RenderSwitch;
        d.choices = {{"full", "Вся картинка", "Whole picture"}, {"21:9", "Центр 21:9", "Centre 21:9"}, {"16:9", "Центр 16:9", "Centre 16:9"}};
        Help(d, "На широкой картинке (21:9, 32:9): интерфейс у краёв или в центральной части.",
             "On a wide picture (21:9, 32:9): the HUD at the edges or in the centre part.");
        v << d;
    }

    // ------------------------------------------------------------------ Rendering
    const QString gUpRu = QStringLiteral("Масштабирование"), gUpEn = QStringLiteral("Upscaling");
    const QString gQRu = QStringLiteral("Качество"), gQEn = QStringLiteral("Quality");
    const QString gShRu = QStringLiteral("Кэш шейдеров"), gShEn = QStringLiteral("Shader cache");
    {
        auto d = Def("graphics.renderScale", "Graphics", "renderScale", S::Rendering, T::Choice, "Масштаб рендеринга",
                     "Render scale");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = QStringLiteral("auto");
        d.needsGame = GameCaps::RenderSwitch;
        d.requiresOn = QStringLiteral("upscaling.upscaler=off");
        d.choices = {{"auto", "Автоматически (до 1080 строк)", "Automatic (up to 1080 lines)"}, {"50", "50 %", "50 %"},
                     {"60", "60 %", "60 %"}, {"67", "67 %", "67 %"}, {"75", "75 %", "75 %"}, {"85", "85 %", "85 %"},
                     {"100", "100 % (родное)", "100 % (native)"}};
        Help(d, "Разрешение 3D-сцены относительно разрешения игры; интерфейс рисуется поверх чётким. С апскейлером сцену "
                "задаёт его качество.",
             "The 3D scene's resolution against the game's; the HUD is drawn sharp on top. With an upscaler its quality "
             "sets the scene instead.");
        v << d;
    }
    {
        auto d = Def("upscaling.upscaler", "Graphics", "upscaler", S::Rendering, T::Choice, "Технология масштабирования",
                     "Upscaling technology");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = QStringLiteral("off");
        d.needsGame = GameCaps::RenderSwitch;
        d.choices = {{"off", "Выключено (масштаб рендеринга)", "Off (Render scale)"},
                     {"dlss", "NVIDIA DLSS", "NVIDIA DLSS", SysCaps::Dlss},
                     {"fsr", "AMD FSR 3.1", "AMD FSR 3.1", SysCaps::Fsr}};
        Help(d, "Игра рендерит сцену в пониженном разрешении, апскейлер восстанавливает картинку. DLSS нужна GeForce RTX; "
                "FSR 3.1 работает на любой видеокарте.",
             "The game renders the scene at a lower resolution and the upscaler rebuilds the picture. DLSS needs a GeForce "
             "RTX; FSR 3.1 runs on any GPU.");
        v << d;
    }
    {
        auto d = Def("upscaling.quality", "Graphics", "upscaleQuality", S::Rendering, T::Choice, "Качество масштабирования",
                     "Upscaling quality");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = QStringLiteral("quality");
        d.needsGame = GameCaps::RenderSwitch;
        d.requiresNot = QStringLiteral("upscaling.upscaler=off");
        d.choices = {{"native", "Нативное сглаживание (DLAA / FSR native)", "Native AA (DLAA / FSR native)"},
                     {"quality", "Качество (67 %)", "Quality (67 %)"}, {"balanced", "Баланс (59 %)", "Balanced (59 %)"},
                     {"performance", "Производительность (50 %)", "Performance (50 %)"},
                     {"ultra", "Ультрапроизводительность (33 %)", "Ultra Performance (33 %)"}};
        v << d;
    }
    {
        auto d = Def("upscaling.sharpness", "Graphics", "upscaleSharpness", S::Rendering, T::Int, "Резкость изображения",
                     "Image sharpness");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.min = 0; d.max = 100; d.step = 5; d.unit = QStringLiteral("%");
        d.builtinDefault = qint64(50);
        d.needsGame = GameCaps::RenderSwitch;
        d.requiresNot = QStringLiteral("upscaling.upscaler=off");
        Help(d, "Контрастно-адаптивная резкость (AMD RCAS) после масштабирования. 0 — без неё.",
             "Contrast-adaptive sharpening (AMD RCAS) of the upscaled picture. 0: none.");
        v << d;
    }
    {
        auto d = Def("upscaling.frameGeneration", "Graphics", "frameGeneration", S::Rendering, T::Choice, "Генерация кадров",
                     "Frame generation");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = QStringLiteral("off");
        d.needsGame = GameCaps::RenderSwitch;
        d.requiresNot = QStringLiteral("upscaling.upscaler=off");
        d.choices = {{"off", "Выключено", "Off"},
                     {"fsr", "AMD FSR Frame Generation", "AMD FSR Frame Generation", SysCaps::Fsr},
                     {"dlss", "NVIDIA DLSS Frame Generation", "NVIDIA DLSS Frame Generation", SysCaps::DlssFrameGen}};
        Help(d, "Между кадрами игры показываются сгенерированные: плавнее, ввод примерно на кадр позже. Нужен апскейлер.",
             "Generated frames between the game's: smoother, input about one frame later. Needs an upscaler.");
        v << d;
    }
    {
        auto d = Def("upscaling.frameGenMultiplier", "Graphics", "frameGenMultiplier", S::Rendering, T::Choice,
                     "Множитель генерации", "Frame generation multiplier");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = QStringLiteral("2");
        d.needsGame = GameCaps::RenderSwitch;
        d.needsSys = SysCaps::DlssFrameGen;
        d.requiresNot = QStringLiteral("upscaling.frameGeneration=off");
        d.choices = {{"2", "×2", "x2"}, {"3", "×3 (DLSS, RTX 50)", "x3 (DLSS, RTX 50)"}, {"4", "×4 (DLSS, RTX 50)", "x4 (DLSS, RTX 50)"}};
        v << d;
    }
    {
        auto d = Def("upscaling.reflex", "Graphics", "reflex", S::Rendering, T::Bool, "NVIDIA Reflex", "NVIDIA Reflex");
        d.groupRu = gUpRu; d.groupEn = gUpEn;
        d.builtinDefault = true;
        d.needsGame = GameCaps::RenderSwitch;
        d.needsSys = SysCaps::Reflex;
        Help(d, "Режим низкой задержки NVIDIA: игра начинает кадр к моменту, когда GPU освобождается.",
             "NVIDIA low latency: the game starts each frame just in time for the GPU.");
        v << d;
    }
    {
        auto d = Def("graphics.anisotropy", "Graphics", "anisotropy", S::Rendering, T::Choice, "Анизотропная фильтрация",
                     "Anisotropic filtering");
        d.groupRu = gQRu; d.groupEn = gQEn;
        d.builtinDefault = QStringLiteral("game");
        d.choices = {{"game", "Как в игре (в основном 4x)", "As in the game (mostly 4x)"}, {"2", "2x", "2x"}, {"4", "4x", "4x"},
                     {"8", "8x", "8x"}, {"16", "16x", "16x"}};
        Help(d, "Чёткость текстур под углом. Меняются только текстуры, которые игра сама фильтрует анизотропно "
                "(делает графический бэкенд рантайма — работает в обеих играх).",
             "Sharpness of textures seen at an angle. Only the textures the game filters anisotropically change (done by "
             "the runtime's GPU backend: works in both games).");
        v << d;
    }
    {
        auto d = Def("graphics.antiAliasing", "Graphics", "antiAliasing", S::Rendering, T::Choice, "Сглаживание", "Anti-aliasing");
        d.groupRu = gQRu; d.groupEn = gQEn;
        d.builtinDefault = QStringLiteral("game");
        d.needsGame = GameCaps::CameraHooks;
        d.choices = {{"game", "Как в игре (SMAA T2x, при нагрузке FXAA)", "As in the game (SMAA T2x, FXAA when slow)"},
                     {"t2x", "SMAA T2x", "SMAA T2x"}, {"fxaa", "FXAA", "FXAA"}, {"off", "Выключено", "Off"}};
        v << d;
    }
    {
        auto d = Def("graphics.shadowQuality", "Graphics", "shadowQuality", S::Rendering, T::Choice, "Качество теней",
                     "Shadow quality");
        d.groupRu = gQRu; d.groupEn = gQEn;
        d.builtinDefault = QStringLiteral("original");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"low", "Низкое (1024)", "Low (1024)"}, {"original", "Исходное (2048)", "Original (2048)"},
                     {"high", "Высокое (4096)", "High (4096)"}};
        Help(d, "Размер теневых карт солнца и дождя. Со следующего запуска.", "Shadow map sizes of the sun and the rain. From the next start.");
        v << d;
    }
    {
        auto d = Def("graphics.textureQuality", "Graphics", "textureQuality", S::Rendering, T::Choice, "Качество текстур",
                     "Texture quality");
        d.groupRu = gQRu; d.groupEn = gQEn;
        d.builtinDefault = QStringLiteral("high");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"low", "Низкое (текстуры города в половинном размере)", "Low (half-size city textures)"},
                     {"medium", "Среднее (полный размер на 2 ближайших кварталах)", "Medium (full size on 2 nearest blocks)"},
                     {"high", "Высокое (как в игре, 4 квартала)", "High (as in the game, 4 blocks)"}};
        Help(d, "Полноразмерные текстуры города на ближайших кварталах. Со следующего запуска.",
             "Full-size city textures on the nearest blocks. From the next start.");
        v << d;
    }
    {
        auto d = Def("shaders.precompile", "ShaderCache", "precompile", S::Rendering, T::Bool, "Готовить шейдеры при запуске",
                     "Prepare shaders at start");
        d.groupRu = gShRu; d.groupEn = gShEn;
        d.builtinDefault = true;
        Help(d, "Конвейеры из прошлых запусков компилируются до начала игры, чтобы не останавливать её потом.",
             "Pipelines met in earlier runs are compiled before the game starts, so that they do not stop it later.");
        v << d;
    }
    {
        auto d = Def("shaders.showProgress", "ShaderCache", "showProgress", S::Rendering, T::Bool,
                     "Показывать прогресс подготовки", "Show the preparation's progress");
        d.groupRu = gShRu; d.groupEn = gShEn;
        d.builtinDefault = true;
        d.requiresOn = QStringLiteral("shaders.precompile");
        v << d;
    }
    {
        auto d = Def("shaders.predictAll", "ShaderCache", "predictAll", S::Rendering, T::Bool,
                     "Компилировать все шейдеры игры до запуска", "Compile all the game's shaders before it starts");
        d.groupRu = gShRu; d.groupEn = gShEn;
        d.builtinDefault = true;
        Help(d, "Перед игрой окно консоли компилирует шейдеры, найденные в файлах игры (один раз — несколько минут).",
             "Before the game a console window compiles the shaders found in the game's files (a few minutes once).");
        v << d;
    }
    {
        auto d = Def("shaders.maxSize", "ShaderCache", "maxSizeMiB", S::Rendering, T::Int, "Размер кэша шейдеров",
                     "Shader cache size");
        d.groupRu = gShRu; d.groupEn = gShEn;
        d.min = 256; d.max = 8192; d.step = 256; d.unit = QStringLiteral("MiB");
        d.builtinDefault = qint64(2048);
        Help(d, "Файл кэша растёт до этого размера; дальше новое не добавляется.",
             "The cache file grows up to this size; past it nothing more is added.");
        v << d;
    }
    {
        auto d = Def("shaders.threads", "ShaderCache", "precompileThreads", S::Rendering, T::Int,
                     "Потоков подготовки (0 = автоматически)", "Preparation threads (0 = automatic)");
        d.groupRu = gShRu; d.groupEn = gShEn;
        d.min = 0; d.max = 12; d.step = 1;
        d.builtinDefault = qint64(0);
        v << d;
    }

    // ------------------------------------------------------------------ Effects (Second Son's engine hooks)
    const QString gPostRu = QStringLiteral("Постобработка"), gPostEn = QStringLiteral("Post-processing");
    const QString gFxRu = QStringLiteral("Частицы и погода"), gFxEn = QStringLiteral("Particles and weather");
    const QString gCamRu = QStringLiteral("Камера"), gCamEn = QStringLiteral("Camera");
    {
        auto d = Def("graphics.motionBlur", "Graphics", "motionBlur", S::Effects, T::Bool, "Размытие в движении", "Motion blur");
        d.groupRu = gPostRu; d.groupEn = gPostEn;
        d.builtinDefault = true;
        d.needsGame = GameCaps::EngineHooks;
        Help(d, "Собственное размытие игры при быстром движении камеры и объектов.",
             "The game's own blur of fast camera and object movement.");
        v << d;
    }
    {
        auto d = Def("graphics.motionBlurStrength", "Graphics", "motionBlurStrength", S::Effects, T::Int, "Сила размытия",
                     "Motion blur strength");
        d.groupRu = gPostRu; d.groupEn = gPostEn;
        d.min = 10; d.max = 100; d.step = 10; d.unit = QStringLiteral("%");
        d.builtinDefault = qint64(100);
        d.needsGame = GameCaps::EngineHooks;
        d.requiresOn = QStringLiteral("graphics.motionBlur");
        v << d;
    }
    {
        auto d = Def("graphics.reflectionQuality", "Graphics", "reflectionQuality", S::Effects, T::Choice, "Качество отражений",
                     "Reflection quality");
        d.groupRu = gPostRu; d.groupEn = gPostEn;
        d.builtinDefault = QStringLiteral("original");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"original", "Исходное", "Original"}, {"low", "Низкое (без экранных отражений)", "Low (no screen-space reflections)"}};
        Help(d, "«Низкое» — без экранных отражений в мокрой дороге и лужах (около 0,4 мс GPU на кадр).",
             "Low: without the screen-space reflections in wet roads and puddles (about 0.4 ms of GPU a frame).");
        v << d;
    }
    {
        auto d = Def("graphics.smokeQuality", "Graphics", "smokeQuality", S::Effects, T::Choice, "Качество дыма",
                     "Smoke quality");
        d.groupRu = gFxRu; d.groupEn = gFxEn;
        d.builtinDefault = QStringLiteral("high");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"high", "Высокое (оригинал)", "High (original)"}, {"medium", "Среднее (65 %)", "Medium (65 %)"},
                     {"low", "Низкое (35 %)", "Low (35 %)"}, {"verylow", "Очень низкое (20 %)", "Very low (20 %)"}};
        Help(d, "Сколько частиц создают эмиттеры дыма игры. Огонь, искры и логика способностей не меняются.",
             "How many particles the game's smoke emitters spawn. Fire, sparks and the powers' logic do not change.");
        v << d;
    }
    {
        auto d = Def("graphics.particleResolution", "Graphics", "particleResolution", S::Effects, T::Choice,
                     "Разрешение огня и дыма", "Fire and smoke resolution");
        d.groupRu = gFxRu; d.groupEn = gFxEn;
        d.builtinDefault = QStringLiteral("auto");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"auto", "Авто (как в игре)", "Auto (as the game)"}, {"full", "Полное", "Full"},
                     {"half", "Половина", "Half"}, {"quarter", "Четверть", "Quarter"}};
        v << d;
    }
    {
        auto d = Def("graphics.rainShadows", "Graphics", "rainShadows", S::Effects, T::Choice, "Тени дождя", "Rain shadows");
        d.groupRu = gFxRu; d.groupEn = gFxEn;
        d.builtinDefault = QStringLiteral("original");
        d.needsGame = GameCaps::EngineHooks;
        d.choices = {{"original", "Исходные", "Original"}, {"off", "Выключены (ничто не укрыто от дождя)", "Off (nothing covered from the rain)"}};
        v << d;
    }
    {
        auto d = Def("graphics.fov", "Graphics", "fov", S::Effects, T::Int, "Поле зрения", "Field of view");
        d.groupRu = gCamRu; d.groupEn = gCamEn;
        d.min = 80; d.max = 130; d.step = 5; d.unit = QStringLiteral("%");
        d.builtinDefault = qint64(100);
        d.needsGame = GameCaps::CameraHooks | GameCaps::RenderSwitch;
        Help(d, "Поле зрения камеры игрока относительно собственного. В кат-сценах — своё.",
             "The player camera's field of view against the game's own. Cut-scenes keep theirs.");
        v << d;
    }

    // ------------------------------------------------------------------ Game
    {
        auto d = Def("game.language", "Language", "system", S::Game, T::Choice, "Язык игры", "Game language");
        d.builtinDefault = QStringLiteral("en-US");
        d.choices = {{"ru-RU", "Русский", "Russian"}, {"en-US", "English", "English"}};
        Help(d, "Системный язык эмулируемой PS4: текст и озвучка игры. Не зависит от языка лаунчера. Со следующего запуска.",
             "The emulated PS4's system language: the game's text and voices. Independent of the launcher's language. "
             "From the next start.");
        v << d;
    }
    {
        auto d = Def("game.enterButton", "System", "enterButton", S::Game, T::Choice, "Кнопка подтверждения", "Confirm button");
        d.builtinDefault = QStringLiteral("cross");
        d.choices = {{"cross", "Крест", "Cross"}, {"circle", "Круг", "Circle"}};
        v << d;
    }
    {
        auto d = Def("interface.language", "Interface", "language", S::Game, T::Choice, "Язык меню ПК в игре",
                     "In-game PC menu language");
        d.builtinDefault = QStringLiteral("auto");
        d.choices = {{"auto", "Как в игре", "As the game"}, {"ru", "Русский", "Русский"}, {"en", "English", "English"}};
        v << d;
    }

    // ------------------------------------------------------------------ Experiments (experiments.cpp): applied at start
    const QString gCpRu = QStringLiteral("Обработчик команд и потоки"), gCpEn = QStringLiteral("Command processor and threads");
    const QString gPipeRu = QStringLiteral("Конвейеры и память"), gPipeEn = QStringLiteral("Pipelines and memory");
    const QString gQueueRu = QStringLiteral("Очереди GPU и ровность кадров (compute)"), gQueueEn = QStringLiteral("GPU queues and frame pacing (compute)");
    const QString gDiagRu = QStringLiteral("Диагностика"), gDiagEn = QStringLiteral("Diagnostics");
    {
        SettingDef d = Def("experiments.SSPC_CP_MODE", "Experiments", "SSPC_CP_MODE", S::Experiments, T::Choice,
                           "Режим обработчика команд", "Command processor mode");
        d.groupRu = gCpRu; d.groupEn = gCpEn;
        d.experiment = true;
        d.unsetShows = QStringLiteral("parallel");
        d.choices = {{"native", "Нативный (play.cmd)", "Native (play.cmd)"}, {"parallel", "Параллельный", "Parallel"},
                     {"legacy", "Прежний (один поток)", "Legacy (one thread)"}};
        Help(d, "Как обрабатываются команды игры для видеокарты. «Нативный» — собственный обработчик (play.cmd); остальные — "
                "старые пути, медленнее.",
             "How the game's GPU commands are processed. Native: the runtime's own processor (play.cmd); the others are "
             "older, slower paths.");
        v << d;
    }
    v << Experiment("SSPC_P53_FRONTEND_THREAD", gCpRu, gCpEn, "1", "0", "Отдельный поток подготовки", "Frontend thread",
                    "Подготовка отрисовки идёт в отдельном потоке. Включено в play.cmd.",
                    "Draw preparation runs on a thread of its own. On in play.cmd.");
    v << Experiment("SSPC_VK_EMIT_THREAD", gCpRu, gCpEn, "1", "0", "Второй поток записи Vulkan", "Second Vulkan recording thread",
                    "Команды Vulkan передаются драйверу в отдельном потоке. Включено в play.cmd.",
                    "Vulkan commands are passed to the driver on a thread of their own. On in play.cmd.");
    {
        auto d = Experiment("SSPC_VK_EMIT_BATCH", gCpRu, gCpEn, "1", "0", "Пакетная запись", "Batched recording",
                            "Поток записи публикует команды один раз на запись. Включено в play.cmd.",
                            "The recording thread publishes its commands once per record. On in play.cmd.");
        d.requiresOn = QStringLiteral("experiments.SSPC_VK_EMIT_THREAD");
        v << d;
    }
    {
        auto d = Experiment("SSPC_R11_EARLY_SUBMIT", gCpRu, gCpEn, "256", "0", "Ранняя отправка батчей (R11)",
                            "Early batch submission (R11)",
                            "Батч уходит на видеокарту при 256 командах, если она простаивает. Включено в play.cmd Second Son.",
                            "A batch goes to an idle GPU at 256 commands. On in Second Son's play.cmd.");
        d.needsGame = GameCaps::EngineHooks;
        v << d;
    }
    {
        auto d = Experiment("SSPC_NATIVE_GBUFFER", gCpRu, gCpEn, "native", "0", "Нативная отрисовка G-буфера",
                            "Native G-buffer draws",
                            "Отрисовки G-буфера готовятся из данных движка. Включено в play.cmd Second Son.",
                            "The G-buffer's draws prepared from the engine's own data. On in Second Son's play.cmd.");
        d.needsGame = GameCaps::EngineHooks;
        v << d;
    }
    v << Experiment("SSPC_ASYNC_PIPELINES", gPipeRu, gPipeEn, "1", "0", "Сборка конвейеров в фоне", "Background pipeline builds",
                    "Новые конвейеры собираются в фоне, не останавливая кадр. Включено в play.cmd.",
                    "New pipelines are built in the background instead of stopping the frame. On in play.cmd.");
    v << Experiment("SSPC_R9_PCACHE", gPipeRu, gPipeEn, "1", "0", "Без фризов кэша конвейеров (F1)",
                    "No pipeline cache freezes (F1)",
                    "Кэш конвейеров драйвера сохраняется без остановки игры. Включено в play.cmd.",
                    "The driver's pipeline cache is saved without stopping the game. On in play.cmd.");
    v << Experiment("SSPC_PIPE_PREFETCH", gPipeRu, gPipeEn, "1", "0", "Предсборка compute-конвейеров (pipeline prefetch)",
                    "Compute pipeline prefetch",
                    "Эксперимент, выключено. С пустым кэшем шейдеров меньше фризов (Neon Dash 12,9 → 26,1 FPS, один прогон); "
                    "с прогретым кэшем ничего не меняет.",
                    "Experiment, off. Fewer freezes with an empty shader cache (Neon Dash 12.9 -> 26.1 FPS, one run); "
                    "changes nothing with a warm cache.");
    {
        auto d = Experiment("SSPC_R10_SCHED", gPipeRu, gPipeEn, "1", "0", "Общий пул сборки конвейеров (R10)",
                            "Work pool for pipeline builds (R10)",
                            "Эксперимент, выключено. Сборки конвейеров через общий пул; с прогретым кэшем без выигрыша.",
                            "Experiment, off. Pipeline builds share one pool; no gain with a warm cache.");
        d.requiresOn = QStringLiteral("experiments.SSPC_PIPE_PREFETCH");
        v << d;
    }
    v << Experiment("SSPC_FB_LAZY_WB", gPipeRu, gPipeEn, "1", "0", "Данные видеокарты — в видеопамяти до чтения (FB-7)",
                    "GPU writes stay in video memory until read (FB-7)",
                    "Эксперимент, выключено. Большие диапазоны, которые пишет видеокарта, не копируются в память процессора "
                    "каждый кадр (R12: +25 % FPS в одной сцене, в остальных без изменений).",
                    "Experiment, off. Large GPU-written ranges are no longer copied back every frame (R12: +25 % FPS in one "
                    "scene, neutral elsewhere).");
    {
        auto d = Experiment("SSPC_REA_HEAP_READS", gPipeRu, gPipeEn, "1", "0", "Кучи вершин — только для чтения (R12)",
                            "Vertex heaps read-only (R12)",
                            "Эксперимент, выключено. Вместе с предыдущим: кучи вершин игры (≥ 256 МиБ) не проверяются на запись "
                            "видеокартой каждый кадр.",
                            "Experiment, off. With the previous switch: the game's vertex heaps (>= 256 MiB) are not checked "
                            "for GPU writes every frame.");
        d.requiresOn = QStringLiteral("experiments.SSPC_FB_LAZY_WB");
        v << d;
    }
    {
        auto d = Experiment("SSPC_FB_ASYNC_COMPUTE", gQueueRu, gQueueEn, "1", "0", "AQ: compute в отдельной очереди",
                            "AQ: compute on its own queue",
                            "Эксперимент (экспериментальный профиль). Compute игры идёт в отдельной очереди Vulkan.",
                            "Experiment (the experimental profile). The game's compute runs on a Vulkan queue of its own.");
        d.warnRu = QStringLiteral("Линия AQ в прошлом приводила к потере устройства и зависанию (128 fences); выключена по умолчанию.");
        d.warnEn = QStringLiteral("The AQ line has caused device loss and a hang (128 fences) before; off by default.");
        v << d;
    }
    {
        auto d = Experiment("SSPC_R10_AQ_MAIN_FREE", gQueueRu, gQueueEn, "1", "0", "MAIN_FREE: графика ждёт только свой compute",
                            "MAIN_FREE: graphics waits only for its compute",
                            "Эксперимент вместе с AQ. Быстрее в сохранении и бою, но в Neon Dash 1 % low ниже.",
                            "Experiment with AQ. Faster in the save and the fight, a lower 1 % low in Neon Dash.");
        d.requiresOn = QStringLiteral("experiments.SSPC_FB_ASYNC_COMPUTE");
        v << d;
    }
    {
        auto d = Experiment("SSPC_REA_DEFER_WAIT_B", gQueueRu, gQueueEn, "1", "0", "REA-3: ожидание compute на видеокарте",
                            "REA-3: compute wait on the GPU",
                            "Эксперимент. Видеокарта сама выполняет второе ожидание async compute; включать вместе с AQ и MAIN_FREE.",
                            "Experiment. The GPU takes the game's second async compute wait; use it with AQ and MAIN_FREE.");
        d.needsGame = GameCaps::EngineHooks;
        v << d;
    }
    v << Experiment("SSPC_R10_SPREAD", gQueueRu, gQueueEn, "1", "0", "Разносить тяжёлые потоки по ядрам (R10)",
                    "Spread heavy threads over cores (R10)",
                    "Эксперимент, выключено. Тяжёлые потоки получают разные физические ядра. Выигрыша не замерено.",
                    "Experiment, off. The heavy threads get different physical cores. No gain measured.");
    v << Experiment("SSPC_R8_CP_PROFILE_CSV", gDiagRu, gDiagEn, "1", "0", "Профайлер кадров (диагностика фризов)",
                    "Frame profiler (stutter diagnostics)",
                    "Диагностика, выключено. При выходе — logs\\r8_cp_profile.csv. Стоит части FPS и ~200 МБ ОЗУ.",
                    "Diagnostics, off. At exit logs\\r8_cp_profile.csv. Costs some FPS and ~200 MB of RAM.");
    return v;
}

} // namespace

const QList<SettingDef>& Catalog() {
    static const QList<SettingDef> catalog = Build();
    return catalog;
}

const SettingDef* FindSetting(const QString& id) {
    for (const auto& d : Catalog()) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

} // namespace Launcher
