// SecondSonPC — the experiments page (see experiments.h): the table of switches and their start-up hand-over.
#include "settings/experiments.h"

#include <cstdlib>
#include <windows.h>

#include "app/config.h"

namespace SecondSon::Settings {

namespace {

constexpr Choice kCpModes[] = {
    {"native", "Native (play.cmd)", "Нативный (play.cmd)"},
    {"parallel", "Parallel", "Параллельный"},
    {"legacy", "Legacy (one thread)", "Прежний (один поток)"},
};
constexpr Choice kCpRings[] = {
    {"", "Automatic", "Автоматически"},
    {"128", "128 records (play.cmd)", "128 записей (play.cmd)"},
    {"512", "512 records", "512 записей"},
};
constexpr Choice kWeatherStates[] = {
    {"", "Off (the game's own)", "Выключено (как в игре)"},
    {"log", "Journal only", "Только журнал"},
    {"dry", "Dry surfaces", "Сухие поверхности"},
};
constexpr Choice kReflDiags[] = {
    {"", "Off", "Выключено"},
    {"ssroff", "No screen-space reflections", "Без экранных отражений"},
    {"ssrflip", "Screen-space reflections every other frame", "Экранные отражения через кадр"},
    {"ssrcycle", "Screen-space reflections every other second", "Экранные отражения через секунду"},
    {"planaroff", "No planar reflections", "Без планарных отражений"},
    {"wateroff", "No water reflection (black)", "Без отражения воды (чёрное)"},
};
constexpr Choice kReflStats[] = {
    {"", "Off", "Выключено"},
    {"5", "Every 5 s", "Раз в 5 с"},
};
constexpr Choice kRainStats[] = {
    {"", "Off", "Выключено"},
    {"5", "Every 5 s", "Раз в 5 с"},
};
constexpr Choice kRainDistances[] = {
    {"", "Off (the game's 10000)", "Выключено (как в игре, 10000)"},
    {"5000", "5000", "5000"},
    {"2500", "2500", "2500"},
};
constexpr Choice kRainCycles[] = {
    {"", "Off", "Выключено"},
    {"10", "Every 10 s", "Каждые 10 с"},
};
constexpr Choice kWriterDetails[] = {
    {"", "Off", "Выключено"},
    {"2edafa400", "Tiled lighting shader", "Шейдер тайлового освещения"},
};
constexpr Choice kReadersOf[] = {
    {"", "Off", "Выключено"},
    {"2edc12000", "Per-tile light list", "Список света по тайлам"},
    {"26d558000", "Particle pool", "Пул частиц"},
};

constexpr std::string_view kCpEn = "Command processor", kCpRu = "Обработчик команд GPU";
constexpr std::string_view kVkEn = "Vulkan recording", kVkRu = "Запись команд Vulkan";
constexpr std::string_view kPipeEn = "Pipelines and memory", kPipeRu = "Конвейеры и память";
constexpr std::string_view kQueueEn = "GPU queues and frame pacing", kQueueRu = "Очереди GPU и ровность кадров";
constexpr std::string_view kCpuEn = "Processor threads", kCpuRu = "Потоки процессора";
constexpr std::string_view kEngineEn = "Draws from the engine's data (R10)", kEngineRu = "Отрисовка из данных движка (R10)";
constexpr std::string_view kSysEn = "System libraries", kSysRu = "Системные библиотеки";
constexpr std::string_view kStandEn = "Stand tools (R12)", kStandRu = "Инструменты стенда (R12)";
constexpr std::string_view kFxEn = "Particles and weather (R12)", kFxRu = "Частицы и погода (R12)";

constexpr Experiment kExperiments[] = {
    // ---- command processor (play.cmd)
    {.env = "SSPC_CP_MODE", .group_en = kCpEn, .group_ru = kCpRu, .type = Type::Choice, .choices = kCpModes,
     .unset_shows = "parallel", .en = "Command processor mode", .ru = "Режим обработчика команд",
     .help_en = "How the game's GPU commands are processed. Native: SecondSonPC's own processor with the engine's "
                "data (play.cmd). Parallel and legacy are the older paths, slower.",
     .help_ru = "Как обрабатываются команды игры для видеокарты. «Нативный» — собственный обработчик SecondSonPC с "
                "данными движка (play.cmd). «Параллельный» и «Прежний» — старые пути, медленнее."},
    {.env = "SSPC_CITY_PERFORMANCE", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "City optimizations", .ru = "Оптимизации города",
     .help_en = "The measured CPU optimizations of the city's draws (hot locks, geometry runs, the engine's draw "
                "tags). On in play.cmd.",
     .help_ru = "Замеренные оптимизации отрисовки города на процессоре (горячие блокировки, серии геометрии, метки "
                "отрисовки движка). Включено в play.cmd."},
    {.env = "SSPC_REA_MERGE_RUNS", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "Merge resource runs", .ru = "Объединять серии ресурсов",
     .help_en = "Neighbouring resource ranges are bound as one. On in play.cmd.",
     .help_ru = "Соседние диапазоны ресурсов подключаются одним. Включено в play.cmd."},
    {.env = "SSPC_CP_ROUND_NODRAIN", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "No drain between rounds", .ru = "Без слива между раундами",
     .help_en = "The command processor does not wait for the GPU between its rounds. On in play.cmd.",
     .help_ru = "Обработчик команд не ждёт видеокарту между своими раундами. Включено в play.cmd."},
    {.env = "SSPC_P411_ASYNC_ACQUIRE", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "Asynchronous frame acquire", .ru = "Асинхронное получение кадра",
     .help_en = "The next swapchain image is acquired off the command processor's thread. On in play.cmd.",
     .help_ru = "Следующий кадр swapchain получается не в потоке обработчика команд. Включено в play.cmd."},
    {.env = "SSPC_P53_FRONTEND_THREAD", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "Frontend thread", .ru = "Отдельный поток подготовки",
     .help_en = "Draw preparation runs on a thread of its own (city 28.6 -> 36.1 FPS with the ring). On in play.cmd.",
     .help_ru = "Подготовка отрисовки идёт в отдельном потоке (город 28,6 → 36,1 FPS вместе с кольцом). Включено в "
                "play.cmd."},
    {.env = "SSPC_CP_ASYNC_WRITES", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "Asynchronous memory writes", .ru = "Асинхронные записи в память",
     .help_en = "The GPU's writes the game polls are done without stopping the processor. On in play.cmd.",
     .help_ru = "Записи видеокарты, которые опрашивает игра, выполняются без остановки обработчика. Включено в "
                "play.cmd."},
    {.env = "SSPC_CP_ASYNC_FLIP", .group_en = kCpEn, .group_ru = kCpRu, .on = "1", .off = "0",
     .en = "Asynchronous flip", .ru = "Асинхронная смена кадра",
     .help_en = "A finished frame is shown without stopping the processor. On in play.cmd.",
     .help_ru = "Готовый кадр выводится без остановки обработчика. Включено в play.cmd."},
    {.env = "SSPC_CP_RING", .group_en = kCpEn, .group_ru = kCpRu, .type = Type::Choice, .choices = kCpRings,
     .unset_shows = "", .en = "Command ring", .ru = "Кольцо команд",
     .help_en = "How many prepared records may wait for the recording thread. 128 in play.cmd.",
     .help_ru = "Сколько подготовленных записей может ждать поток записи. 128 в play.cmd."},
    {.env = "SSPC_NATIVE_GBUFFER", .group_en = kCpEn, .group_ru = kCpRu, .on = "native", .off = "0",
     .requires_env = "SSPC_CITY_PERFORMANCE", .en = "Native G-buffer draws", .ru = "Нативная отрисовка G-буфера",
     .help_en = "The G-buffer's instanced draws (0x1a1770) prepared from the engine's own data instead of walking "
                "its resource tables. On in play.cmd.",
     .help_ru = "Инстансные отрисовки G-буфера (0x1a1770) готовятся из данных движка, без обхода его таблиц "
                "ресурсов. Включено в play.cmd."},

    // ---- Vulkan recording (play.cmd)
    {.env = "SSPC_VK_EMIT_THREAD", .group_en = kVkEn, .group_ru = kVkRu, .on = "1", .off = "0",
     .en = "Second recording thread", .ru = "Второй поток записи",
     .help_en = "Vulkan commands are passed to the driver on a thread of their own. On in play.cmd.",
     .help_ru = "Команды Vulkan передаются драйверу в отдельном потоке. Включено в play.cmd."},
    {.env = "SSPC_VK_EMIT_BATCH", .group_en = kVkEn, .group_ru = kVkRu, .on = "1", .off = "0",
     .requires_env = "SSPC_VK_EMIT_THREAD", .en = "Batched recording", .ru = "Пакетная запись",
     .help_en = "The recording thread publishes its commands once per record (city 39.1 -> 41.5 FPS). On in play.cmd.",
     .help_ru = "Поток записи публикует команды один раз на запись (город 39,1 → 41,5 FPS). Включено в play.cmd."},
    {.env = "SSPC_R11_EARLY_SUBMIT", .group_en = kVkEn, .group_ru = kVkRu, .on = "256", .off = "0",
     .en = "Early batch submission (R11)", .ru = "Ранняя отправка батчей (R11)",
     .help_en = "A batch goes to the GPU once it holds 256 commands while the GPU has nothing queued, instead of at "
                "500. Owner's save 31.3 -> 32.4 FPS, Neon Dash frames over 33 ms 590 -> 186 a minute; the fight within "
                "noise (3 runs each). On in play.cmd.",
     .help_ru = "Батч уходит на видеокарту, как только в нём 256 команд и видеокарта простаивает, а не по 500. "
                "Сохранение 31,3 → 32,4 FPS, в Neon Dash кадров дольше 33 мс 590 → 186 в минуту; бой в пределах "
                "шума (по 3 прогона). Включено в play.cmd."},
    {.env = "SSPC_R11_ASYNC_SUBMIT", .group_en = kVkEn, .group_ru = kVkRu, .on = "1", .off = "0",
     .requires_env = "SSPC_VK_EMIT_THREAD", .en = "Submissions on their own thread (R11)",
     .ru = "Отправка батчей в отдельном потоке (R11)",
     .help_en = "Experiment, off. vkQueueSubmit2 is made by a thread of its own, not the recording thread (its time "
                "there per batch 47 -> 7 us). No FPS change measured: the recording thread is not the limit.",
     .help_ru = "Эксперимент, выключено. vkQueueSubmit2 вызывается в отдельном потоке, а не в потоке записи (время "
                "в нём на батч 47 → 7 мкс). Изменения FPS не замерено: поток записи не ограничивает кадр."},

    // ---- pipelines and memory
    {.env = "SSPC_ASYNC_PIPELINES", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "Background pipeline builds", .ru = "Сборка конвейеров в фоне",
     .help_en = "New graphics pipelines are built on background threads instead of stopping the frame; their draws "
                "wait until ready. On in play.cmd.",
     .help_ru = "Новые графические конвейеры собираются в фоне, не останавливая кадр; их отрисовка ждёт готовности. "
                "Включено в play.cmd."},
    {.env = "SSPC_R9_PCACHE", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "No pipeline cache freezes (F1)", .ru = "Без фризов кэша конвейеров (F1)",
     .help_en = "The driver's pipeline cache is saved without stopping the game (fight 1 % low 7.5 -> 25.6). On in "
                "play.cmd.",
     .help_ru = "Кэш конвейеров драйвера сохраняется без остановки игры (бой: 1 % low 7,5 → 25,6). Включено в "
                "play.cmd."},
    {.env = "SSPC_R9_HEAP_WINDOW", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "No vertex heap re-imports (F3)", .ru = "Без повторного импорта кучи вершин (F3)",
     .help_en = "Large heaps are imported with headroom, so they are not imported again as they grow. On in play.cmd.",
     .help_ru = "Большие кучи импортируются с запасом и не импортируются заново по мере роста. Включено в play.cmd."},
    {.env = "SSPC_PIPE_PREFETCH", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "Compute pipeline prefetch (R10)", .ru = "Предсборка compute-конвейеров (R10)",
     .help_en = "Experiment, off. New compute pipelines start building as soon as their dispatch is prepared. With "
                "an empty shader cache (first play, a new driver) Neon Dash went 12.9 -> 26.1 FPS and its longest "
                "freeze 5.5 -> 2.5 s (one run); with a warm cache it changes nothing.",
     .help_ru = "Эксперимент, выключено. Новые compute-конвейеры начинают собираться, как только подготовлен их "
                "dispatch. С пустым кэшем шейдеров (первое прохождение, новый драйвер) Neon Dash 12,9 → 26,1 FPS, "
                "самый долгий фриз 5,5 → 2,5 с (один прогон); с прогретым кэшем ничего не меняет."},
    {.env = "SSPC_R12_LIGHT_LIST_KEEP", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "Light index list stays in video memory (R12)", .ru = "Список источников света — только в видеопамяти (R12)",
     .help_en = "Experiment, off. The per-tile light list the GPU builds every frame (6.25 MiB) is no longer copied "
                "back to the CPU's memory: only the GPU reads it (0 CPU reads measured); a CPU access still gets it "
                "written back first. The picture is the same. No FPS change (user 32.85 -> 32.79, fight 46.83 -> "
                "46.94, n = 3): its write-back is cheap - the particle pools are what costs (see below). "
                "Measurements: docs/R12_LIGHTS.md.",
     .help_ru = "Эксперимент, выключено. Список источников света по тайлам, который видеокарта строит каждый кадр "
                "(6,25 МиБ), больше не копируется обратно в память процессора: его читает только видеокарта "
                "(чтений процессором — 0); при обращении процессора он сначала записывается обратно. Картинка та же. "
                "FPS не меняется (user 32,85 → 32,79, бой 46,83 → 46,94, n = 3): его write-back дешёвый, дорого "
                "стоят пулы частиц (см. ниже). Замеры — docs/R12_LIGHTS.md."},
    {.env = "SSPC_FB_LAZY_WB", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .en = "GPU writes stay in video memory until read (FB-7)",
     .ru = "Данные видеокарты — в видеопамяти до чтения (FB-7)",
     .help_en = "Experiment, off. Large ranges the GPU writes every frame (the particle pools, the light list) are "
                "no longer copied back to the CPU's memory each frame; a CPU access waits for its 64 KiB block "
                "(2-9 ms, ~60-170 a run). R12 (n = 3): user 33.14 -> 41.43 FPS (+25 %), 1 % low 27.9 -> 32.6; "
                "city 47.83 -> 48.20, fight neutral; Neon Dash within its spread. The picture is the same "
                "(poison test, Validation 0).",
     .help_ru = "Эксперимент, выключено. Большие диапазоны, которые видеокарта пишет каждый кадр (пулы частиц, "
                "список света), больше не копируются в память процессора каждый кадр; обращение процессора ждёт "
                "свой блок 64 КиБ (2–9 мс, ~60–170 за прогон). R12 (n = 3): user 33,14 → 41,43 FPS (+25 %), "
                "1 % low 27,9 → 32,6; город 47,83 → 48,20, бой без изменений; Neon Dash — в пределах разброса. "
                "Картинка та же (тест отравления, Validation 0)."},
    {.env = "SSPC_REA_HEAP_READS", .group_en = kPipeEn, .group_ru = kPipeRu, .on = "1", .off = "0",
     .requires_env = "SSPC_FB_LAZY_WB", .en = "Vertex heap reads skip write-backs (REA-2)",
     .ru = "Чтение кучи вершин без write-back (REA-2)",
     .help_en = "Experiment, off. With the previous switch: the game's vertex-data heaps (read-only, >= 256 MiB) "
                "no longer force the write-back of what the GPU wrote inside their range. R12 (n = 3, both): user "
                "41.75 FPS (+26 %), city 50.13 (+4.8 %), 1 % low 20.5 -> 24.2; fight neutral.",
     .help_ru = "Эксперимент, выключено. Вместе с предыдущим: кучи вершин игры (только чтение, ≥ 256 МиБ) больше "
                "не заставляют записывать обратно то, что видеокарта записала в их диапазоне. R12 (n = 3, оба "
                "флага): user 41,75 FPS (+26 %), город 50,13 (+4,8 %), 1 % low 20,5 → 24,2; бой без изменений."},

    // ---- GPU queues and frame pacing (play_experimental.cmd)
    {.env = "SSPC_REA_DEFER_WAIT_B", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .en = "REA-3: compute wait on the GPU", .ru = "REA-3: ожидание compute на видеокарте",
     .help_en = "Experiment (play_experimental.cmd). The GPU takes the game's second async compute wait of a frame. "
                "Faster, but alone it makes frames uneven; use it with AQ and MAIN_FREE.",
     .help_ru = "Эксперимент (play_experimental.cmd). Видеокарта сама выполняет второе ожидание async compute за "
                "кадр. Быстрее, но в одиночку делает кадры неровными; включать вместе с AQ и MAIN_FREE."},
    {.env = "SSPC_FB_ASYNC_COMPUTE", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .en = "AQ: compute on its own queue", .ru = "AQ: compute в отдельной очереди",
     .help_en = "Experiment (play_experimental.cmd). The game's compute runs on a Vulkan queue of its own, beside the "
                "graphics.",
     .help_ru = "Эксперимент (play_experimental.cmd). Compute игры идёт в отдельной очереди Vulkan, рядом с "
                "графикой."},
    {.env = "SSPC_R10_AQ_MAIN_FREE", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .requires_env = "SSPC_FB_ASYNC_COMPUTE", .en = "MAIN_FREE: graphics waits only for its compute (R10)",
     .ru = "MAIN_FREE: графика ждёт только свой compute (R10)",
     .help_en = "Experiment (play_experimental.cmd), with REA-3 and AQ. A graphics batch waits only for the compute it "
                "depends on. Owner's save 31.6 -> 37.6 FPS, fight 46.6 -> 53.0 FPS with fewer drops; Neon Dash has "
                "a lower 1 % low (16.3 against 18.9). A 20-minute run is not tested.",
     .help_ru = "Эксперимент (play_experimental.cmd), вместе с REA-3 и AQ. Графика ждёт только тот compute, от "
                "которого зависит. Сохранение 31,6 → 37,6 FPS, бой 46,6 → 53,0 FPS и меньше провалов; в Neon Dash "
                "1 % low ниже (16,3 против 18,9). 20-минутный прогон не проверен."},
    {.env = "SSPC_R11_AQ_IMAGES", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .requires_env = "SSPC_FB_ASYNC_COMPUTE", .en = "AQ: compute that samples textures (R11)",
     .ru = "AQ: compute с текстурами в своей очереди (R11)",
     .help_en = "Experiment (play_experimental.cmd). A dispatch that only samples textures already in place goes to "
                "the compute queue instead of splitting the batch (Neon Dash splits 28285 -> about 8000). With "
                "MAIN_FREE: save 31.7 -> 39.0 FPS, fight 45.8 -> 52.5 FPS; Neon Dash's 1 % low not better than "
                "play.cmd. 27-minute run without errors.",
     .help_ru = "Эксперимент (play_experimental.cmd). Dispatch, который только читает готовые текстуры, идёт в "
                "очередь compute, а не разрезает батч (в Neon Dash разрезов 28285 → около 8000). Вместе с MAIN_FREE: "
                "сохранение 31,7 → 39,0 FPS, бой 45,8 → 52,5 FPS; 1 % low в Neon Dash не лучше play.cmd. 27 минут "
                "без ошибок."},
    {.env = "SSPC_R11_AQ_ORDER_EPOCH", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .requires_env = "SSPC_R10_AQ_MAIN_FREE", .en = "MAIN_FREE: order waits only across frames' epochs (R11)",
     .ru = "MAIN_FREE: ожидание порядка только между эпохами (R11)",
     .help_en = "Experiment (play_experimental.cmd). A graphics batch does not wait for the compute queue's batch of "
                "the same guest epoch when it holds only that queue's dispatches (about 4500 waits a run fewer). "
                "Measured together with the option above.",
     .help_ru = "Эксперимент (play_experimental.cmd). Батч графики не ждёт батч очереди compute той же гостевой "
                "эпохи, если в нём только dispatch этой очереди (около 4500 ожиданий за прогон меньше). Замерено "
                "вместе с пунктом выше."},
    {.env = "SSPC_R11_FAST_LABEL", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .requires_env = "SSPC_CP_ROUND_NODRAIN", .en = "Completed compute seen sooner (R11)",
     .ru = "Готовый compute виден раньше (R11)",
     .help_en = "Experiment, off. The GPU's completion labels reach the game every 100 us instead of at the round's "
                "end (0.9 -> 0.09 ms of the game's compute wait). Alone: fight +1.6 %, save +1.0 %; with early "
                "submission only the fight gains (47.8 against 46.4 FPS), the save and Neon Dash do not.",
     .help_ru = "Эксперимент, выключено. Метки завершения работы видеокарты доходят до игры каждые 100 мкс, а не в "
                "конце раунда (ожидание compute в игре 0,9 → 0,09 мс). Отдельно: бой +1,6 %, сохранение +1,0 %; "
                "вместе с ранней отправкой батчей выигрыш только в бою (47,8 против 46,4 FPS), в сохранении и Neon "
                "Dash — нет."},
    {.env = "SSPC_R11_LABEL_WAIT", .group_en = kQueueEn, .group_ru = kQueueRu, .on = "1", .off = "0",
     .en = "Game's compute wait asleep (R11)", .ru = "Ожидание compute в игре без спина (R11)",
     .help_en = "Experiment, off. The game's wait for its compute (0x1695e0) spins 20 us, then sleeps until the label "
                "is written. Fight +3.3 % with the option above, but Neon Dash -3.4 %.",
     .help_ru = "Эксперимент, выключено. Ожидание compute в игре (0x1695e0) крутится 20 мкс, затем спит до записи "
                "метки. Бой +3,3 % вместе с пунктом выше, но Neon Dash −3,4 %."},

    // ---- processor threads (R10 sched)
    {.env = "SSPC_R10_SCHED", .group_en = kCpuEn, .group_ru = kCpuRu, .on = "1", .off = "0",
     .requires_env = "SSPC_PIPE_PREFETCH", .en = "Work pool for pipeline builds (R10)",
     .ru = "Общий пул сборки конвейеров (R10)",
     .help_en = "Experiment, off. Pipeline builds share one work-stealing pool. With an empty cache the longest "
                "freeze in Neon Dash 2.5 -> 1.55 s on top of prefetch (one run); with a warm cache no gain.",
     .help_ru = "Эксперимент, выключено. Сборки конвейеров идут через общий пул с перехватом задач. С пустым кэшем "
                "самый долгий фриз в Neon Dash 2,5 → 1,55 с поверх предсборки (один прогон); с прогретым — без "
                "выигрыша."},
    {.env = "SSPC_R10_SPREAD", .group_en = kCpuEn, .group_ru = kCpuRu, .on = "1", .off = "0",
     .en = "Spread heavy threads over cores (R10)", .ru = "Разносить тяжёлые потоки по ядрам (R10)",
     .help_en = "Experiment, off. The runtime's heavy threads get different physical cores. No gain measured; "
                "traverse 1 % low 2 FPS lower. May help on other processors.",
     .help_ru = "Эксперимент, выключено. Тяжёлые потоки получают разные физические ядра. Выигрыша не замерено; "
                "в перемещении по городу 1 % low ниже на 2 FPS. Может помочь на других процессорах."},

    // ---- draws from the engine's data (R10 cp)
    {.env = "SSPC_R10_RUN_PS", .group_en = kEngineEn, .group_ru = kEngineRu, .on = "1", .off = "0",
     .en = "Runs across pixel shader changes (R10)", .ru = "Серии через смену пиксельного шейдера (R10)",
     .help_en = "Experiment, off. Mesh and shadow draws with another pixel shader's data join a run. The picture is "
                "the same (checked on 25.8 million draws), but the city is slower (fight 45.9 -> 43.1 FPS).",
     .help_ru = "Эксперимент, выключено. Отрисовки мешей и теней с другими данными пиксельного шейдера входят в "
                "серию. Картинка та же (проверено на 25,8 млн отрисовок), но город медленнее (бой 45,9 → 43,1 FPS)."},
    {.env = "SSPC_R10_RUN_PS_MATERIAL", .group_en = kEngineEn, .group_ru = kEngineRu, .on = "1", .off = "0",
     .requires_env = "SSPC_R10_RUN_PS", .en = "...with their own textures (R10)", .ru = "...и со своими текстурами (R10)",
     .help_en = "Experiment, off. Also draws with textures and samplers of their own join the run.",
     .help_ru = "Эксперимент, выключено. В серию входят и отрисовки со своими текстурами и сэмплерами."},
    {.env = "SSPC_R10_NATIVE_DRAWS", .group_en = kEngineEn, .group_ru = kEngineRu, .on = "native", .off = "0",
     .also_env = "SSPC_R10_NATIVE_DRAWS_SOURCES", .also_on = "shadow", .requires_env = "SSPC_CITY_PERFORMANCE",
     .en = "Native shadow draws (R10)", .ru = "Нативная отрисовка теней (R10)",
     .help_en = "Experiment, off. The shadow casters' draws (0x43d480) prepared from the engine's state, without "
                "walking its resource tables (15.2 million checks, no difference). No FPS change measured.",
     .help_ru = "Эксперимент, выключено. Отрисовки теней (0x43d480) готовятся из состояния движка, без обхода "
                "таблиц ресурсов (15,2 млн проверок, расхождений нет). Изменения FPS не замерено."},

    // ---- particles and weather (R12)
    {.env = "SSPC_R12_PSORT", .group_en = kFxEn, .group_ru = kFxRu, .on = "1", .off = "0",
     .en = "Fast particle sort (R12)", .ru = "Быстрая сортировка частиц (R12)",
     .help_en = "Experiment, off. The particles' ranking shader (0x2edaa0c00, 80 runs a frame in the owner's save) "
                "without the emulation of the console's lane masks: 8.5 -> 1.5 ms of GPU a frame there, the same results.",
     .help_ru = "Эксперимент, выключено. Шейдер ранжирования частиц (0x2edaa0c00, 80 запусков за кадр в сохранении "
                "владельца) без эмуляции масок линий консоли: 8,5 -> 1,5 мс видеокарты за кадр, результат тот же."},

    // ---- system libraries (play.cmd)
    {.env = "SSPC_R10_AJM", .group_en = kSysEn, .group_ru = kSysRu, .on = "native", .off = "bridge",
     .en = "Own audio decoder (AJM / ATRAC9)", .ru = "Свой декодер звука (AJM / ATRAC9)",
     .help_en = "libSceAjm on SecondSonPC's own code (LibAtrac9, MIT) instead of shadPS4's; 15 192 frames decoded "
                "the same. On in play.cmd. Off: shadPS4's.",
     .help_ru = "libSceAjm на собственном коде SecondSonPC (LibAtrac9, MIT) вместо shadPS4; 15 192 кадра "
                "декодируются одинаково. Включено в play.cmd. Выключено — как в shadPS4."},
    {.env = "SSPC_R10_SYSRT", .group_en = kSysEn, .group_ru = kSysRu, .on = "native", .off = "bridge",
     .en = "Own dialogs, network and libc", .ru = "Свои диалоги, сеть и libc",
     .help_en = "CommonDialog, libSceNet and a few libc functions on SecondSonPC's own code (102 self-test checks). "
                "On in play.cmd. Off: shadPS4's.",
     .help_ru = "CommonDialog, libSceNet и часть libc на собственном коде SecondSonPC (102 проверки самотеста). "
                "Включено в play.cmd. Выключено — как в shadPS4."},

    // ---- stand tools (R12 weather, docs/R12_WEATHER.md)
    {.env = "SSPC_R12_WEATHER", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kWeatherStates, .unset_shows = "", .en = "Weather state for measurements",
     .ru = "Состояние погоды для замеров",
     .help_en = "Stand tool, off. Journal only: writes the engine's weather values to weather.log (~2 us a frame). "
                "Dry surfaces: wetness and puddles at 0 in the time of day the game loaded, its own values restored "
                "on exit; saves are never written. Rain and night come from the save, not from this switch. Takes "
                "effect at the next start.",
     .help_ru = "Инструмент стенда, выключено. «Только журнал» — значения погоды движка пишутся в weather.log "
                "(~2 мкс на кадр). «Сухие поверхности» — мокрота и лужи 0 во времени суток, которое загрузила "
                "игра; на выходе возвращаются её значения, сохранения не меняются. Дождь и ночь задаёт сохранение, "
                "а не этот переключатель. Действует со следующего запуска."},
    // R12 reflections (docs/R12_REFLECTIONS.md); the player's switch is [Graphics] reflectionQuality
    {.env = "SSPC_R12_REFL_STATS", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kReflStats, .unset_shows = "", .en = "Reflection statistics", .ru = "Статистика отражений",
     .help_en = "Diagnostics, off. A log line every few seconds: the reflection passes' calls and the time of day's "
                "reflection values. The picture is unchanged.",
     .help_ru = "Диагностика, выключено. Строка журнала раз в несколько секунд: вызовы проходов отражений и "
                "значения отражений времени суток. Картинка не меняется."},
    {.env = "SSPC_R12_REFL_DIAG", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kReflDiags, .unset_shows = "", .en = "Reflection diagnostics (changes the picture)",
     .ru = "Диагностика отражений (меняет картинку)",
     .help_en = "Diagnostics for A/B runs, off. Skips one reflection pass to measure its cost; without the water "
                "reflection the water turns black (the game has no other path), so it is not a quality level.",
     .help_ru = "Диагностика для A/B, выключено. Пропускает один проход отражений, чтобы измерить его цену; без "
                "отражения воды вода чёрная (другого пути у игры нет), поэтому это не уровень качества."},
    {.env = "SSPC_R12_REFL_HOOK", .group_en = kStandEn, .group_ru = kStandRu, .on = "0", .off = "",
     .en = "No reflection probe", .ru = "Без пробы отражений",
     .help_en = "Diagnostics, off. The reflection probe is not installed: the game's own path, and «Reflection "
                "quality» has no effect.",
     .help_ru = "Диагностика, выключено. Проба отражений не ставится: путь игры как есть, пункт «Качество "
                "отражений» не действует."},
    // R12 rain shadows (docs/R12_RAIN_SHADOWS.md); the player's switch is [Graphics] rainShadows
    {.env = "SSPC_R12_RAIN_STATS", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kRainStats, .unset_shows = "", .en = "Rain shadow statistics", .ru = "Статистика теней дождя",
     .help_en = "Diagnostics, off. A log line every few seconds: the rain shadow maps' updates, emptied and "
                "released maps, their cascades, the wetness and the rain shadow distance. The picture is unchanged.",
     .help_ru = "Диагностика, выключено. Строка журнала раз в несколько секунд: обновления, опустошённые и "
                "освобождённые карты теней дождя, их каскады, мокрота и дальность теней дождя. Картинка не "
                "меняется."},
    {.env = "SSPC_R12_RAIN_DIST", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kRainDistances, .unset_shows = "", .en = "Rain shadow distance (changes the picture)",
     .ru = "Дальность теней дождя (меняет картинку)",
     .help_en = "Diagnostics for A/B runs, off. Writes the engine's own CSM1_RAIN_SHADOW_DISTANCE (10000 in the "
                "game) before each rain shadow update, to measure what a shorter map saves. Not a quality level.",
     .help_ru = "Диагностика для A/B, выключено. Записывает собственный параметр движка CSM1_RAIN_SHADOW_DISTANCE "
                "(в игре 10000) перед каждым обновлением теней дождя, чтобы измерить, что даёт меньшая карта. Не "
                "уровень качества."},
    {.env = "SSPC_R12_RAIN_CYCLE", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kRainCycles, .unset_shows = "", .en = "Rain shadows on and off (changes the picture)",
     .ru = "Тени дождя вкл/выкл по очереди (меняет картинку)",
     .help_en = "Diagnostics, off. Switches the rain shadows off and back on every few seconds - the path of the "
                "live switch, over and over, to check it in motion. Overrides «Rain shadows».",
     .help_ru = "Диагностика, выключено. Каждые несколько секунд выключает и снова включает тени дождя — путь "
                "переключения на лету, много раз подряд, для проверки в движении. Перекрывает пункт «Тени дождя»."},
    {.env = "SSPC_R12_RAIN_DIAG", .group_en = kStandEn, .group_ru = kStandRu, .on = "release", .off = "",
     .en = "Rain shadows off by releasing the maps (breaks the picture)",
     .ru = "Тени дождя выкл. освобождением карт (ломает картинку)",
     .help_en = "Diagnostics, off. «Rain shadows: Off» takes the game's own dry-weather path (the maps are released) "
                "while it rains: on this emulator the whole frame turns grey and loses its haze. Kept only to show "
                "why the setting empties the map instead.",
     .help_ru = "Диагностика, выключено. «Тени дождя: выключены» идут путём игры для сухой погоды (карты "
                "освобождаются) во время дождя: на этом эмуляторе весь кадр становится серым и теряет дымку. "
                "Оставлено только чтобы показать, почему пункт вместо этого опустошает карту."},
    {.env = "SSPC_R12_RAIN_HOOK", .group_en = kStandEn, .group_ru = kStandRu, .on = "0", .off = "",
     .en = "No rain shadow hooks", .ru = "Без перехвата теней дождя",
     .help_en = "Diagnostics, off. The rain shadow hooks are not installed: the game's own path, and «Rain "
                "shadows» has no effect.",
     .help_ru = "Диагностика, выключено. Перехват теней дождя не ставится: путь игры как есть, пункт «Тени "
                "дождя» не действует."},
    // R12 lights (docs/R12_LIGHTS.md)
    {.env = "SSPC_R12_LIGHT_CENSUS", .group_en = kStandEn, .group_ru = kStandRu, .on = "1", .off = "",
     .en = "Light census", .ru = "Перепись источников света",
     .help_en = "Diagnostics, off. A log line every 10 s: the lights the engine puts into the frame by kind, with a "
                "shadow, the shadow atlas's draws, the glows' occlusion queries and the largest GPU writers. The "
                "picture is unchanged.",
     .help_ru = "Диагностика, выключено. Строка журнала раз в 10 с: источники света в кадре по видам, с тенью, "
                "draw атласа теней, occlusion queries свечений и самые большие записи видеокарты. Картинка не "
                "меняется."},
    {.env = "SSPC_R12_WRITER_DETAIL", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kWriterDetails, .unset_shows = "", .requires_env = "SSPC_R12_LIGHT_CENSUS",
     .en = "Writes of one shader", .ru = "Записи одного шейдера",
     .help_en = "Diagnostics, off. With the light census: each distinct buffer range the chosen shader writes is "
                "logged once.",
     .help_ru = "Диагностика, выключено. Вместе с переписью света: каждый диапазон буферов, который пишет "
                "выбранный шейдер, пишется в журнал один раз."},
    {.env = "SSPC_R12_READERS_OF", .group_en = kStandEn, .group_ru = kStandRu, .type = Type::Choice,
     .choices = kReadersOf, .unset_shows = "", .requires_env = "SSPC_R12_LIGHT_CENSUS",
     .en = "Shaders reading a range", .ru = "Шейдеры, читающие диапазон",
     .help_en = "Diagnostics, off. With the light census: the shaders that bind the chosen range are logged.",
     .help_ru = "Диагностика, выключено. Вместе с переписью света: в журнал пишутся шейдеры, которые подключают "
                "выбранный диапазон."},
    {.env = "SSPC_R12_LAZY_TRACE", .group_en = kStandEn, .group_ru = kStandRu, .on = "1", .off = "",
     .requires_env = "SSPC_FB_LAZY_WB", .en = "Log the waits for kept-back blocks",
     .ru = "Журнал ожиданий отложенных блоков",
     .help_en = "Diagnostics, off. With FB-7: each CPU wait for a block kept in video memory is logged with its "
                "time (at most 4000), to match long frames with these waits.",
     .help_ru = "Диагностика, выключено. Вместе с FB-7: каждое ожидание процессором блока, оставленного в "
                "видеопамяти, пишется в журнал со временем (не больше 4000), чтобы сопоставить их с длинными "
                "кадрами."},
};

std::string GetEnv(std::string_view name) {
    char v[256] = {};
    const DWORD n = GetEnvironmentVariableA(std::string(name).c_str(), v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string{};
}

void PutEnv(std::string_view name, std::string_view value) {
    // _putenv_s changes the CRT's copy (std::getenv) and the process's (GetEnvironmentVariableA); "" removes it
    _putenv_s(std::string(name).c_str(), std::string(value).c_str());
}

} // namespace

std::span<const Experiment> Experiments() {
    return kExperiments;
}

const Experiment* FindExperiment(std::string_view env) {
    for (const auto& e : kExperiments) {
        if (e.env == env) {
            return &e;
        }
    }
    return nullptr;
}

std::string ExperimentId(std::string_view env) {
    return "experiments." + std::string(env);
}

void ReadExperiments(App::Config& config) {
    for (const auto& e : kExperiments) {
        config.pc.experiments.insert_or_assign(std::string(e.env), GetEnv(e.env));
    }
}

std::vector<std::string> ExportExperiments(const App::Config& config, const Overrides& overrides) {
    std::vector<std::string> out;
    for (const auto& e : kExperiments) {
        if (!overrides.contains(ExperimentId(e.env))) {
            continue; // as the launcher set it
        }
        const auto it = config.pc.experiments.find(e.env);
        const std::string value = it != config.pc.experiments.end() ? it->second : std::string{};
        PutEnv(e.env, value);
        out.push_back(std::string(e.env) + "=" + value);
        if (!e.also_env.empty()) {
            const std::string_view also = e.type == Type::Bool && value == e.on ? e.also_on : std::string_view{};
            PutEnv(e.also_env, also);
            out.push_back(std::string(e.also_env) + "=" + std::string(also));
        }
    }
    return out;
}

} // namespace SecondSon::Settings
