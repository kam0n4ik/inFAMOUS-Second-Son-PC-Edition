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

constexpr std::string_view kCpEn = "Command processor", kCpRu = "Обработчик команд GPU";
constexpr std::string_view kVkEn = "Vulkan recording", kVkRu = "Запись команд Vulkan";
constexpr std::string_view kPipeEn = "Pipelines and memory", kPipeRu = "Конвейеры и память";
constexpr std::string_view kQueueEn = "GPU queues and frame pacing", kQueueRu = "Очереди GPU и ровность кадров";
constexpr std::string_view kCpuEn = "Processor threads", kCpuRu = "Потоки процессора";
constexpr std::string_view kEngineEn = "Draws from the engine's data (R10)", kEngineRu = "Отрисовка из данных движка (R10)";
constexpr std::string_view kSysEn = "System libraries", kSysRu = "Системные библиотеки";

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
