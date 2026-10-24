# R10 — итоговый отчёт (координатор)

Ветки пяти агентов от `origin/main` `6d74185`, сведены в `feature/r10-integration` (worktree `E:\SecondSonPC-r10-ajm`).
Push не выполнялся. Стенд: Ryzen 5 5500 (6C/12T), RTX 2060 6 ГБ, 32 ГБ, окно 1280×720, пресет `game/play.cmd`.
Сырые данные — `E:\research\r10\runs\<агент>\`, журнал находок — `E:\research\r10\SHARED_NOTES.md`.
Метки: **[E]** измерено, **[A]** анализ, **[H]** гипотеза. Тестирование свёрнуто по решению владельца 24.10 ~20:20:
всё, что не домерено, помечено **NOT TESTED**.

## 1. Что вошло в `play.cmd` и что нет

| флаг | направление | в `play.cmd` | в `play_experimental.cmd` | основание |
|---|---|---|---|---|
| `SSPC_R10_AJM=native` | свой libSceAjm / ATRAC9 | **да** | да (через play.cmd) | 15 192 кадра ATRAC9, 0 расхождений с оракулом; native-прогоны город/сейв/видео без ошибок |
| `SSPC_R10_SYSRT=native` | CommonDialog / libSceNet / libc | **да** | да | self-test 102/0; 5 native-прогонов (запуск, загрузка, геймплей, меню, автосейв) без ошибок |
| `SSPC_R10_AQ_MAIN_FREE=1` (+ REA-3 + AQ) | ровный pacing | нет | **да** | бой/сейв/город лучше; Neon Dash: 1% low и P99 хуже базы (правило ТЗ) |
| `SSPC_PIPE_PREFETCH=1` | предзагрузка compute-конвейеров | нет | нет | холодный кэш neon FPS ×2, p99 ÷4 — n = 1, NOT TESTED как рекомендация |
| `SSPC_R10_SCHED=1` | пул work-stealing | нет | нет | на тёплом кэше без выигрыша; только вместе с prefetch |
| `SSPC_R10_SPREAD=1` | разнос тяжёлых потоков | нет | нет | выигрыша нет, traverse 1% low −2 FPS (n = 2) |
| `SSPC_R10_RUN_PS=1` (+`_MATERIAL`) | run меша/теней со своим PS | нет | нет | город fight 45,9 → 43,1 FPS, calm 51,8 → 38,3 |
| `SSPC_R10_NATIVE_DRAWS=native` | draw теней/меша из данных движка | нет | нет | FPS без изменений, фронтенд +0,75 мс; меш — 6 расхождений VB |

Всё новое — за флагами, выключенными по умолчанию; без флагов поведение прежнее (откат — убрать переменную
или `bridge`).

## 2. pacing — REA-3 / AQ, ровная подача кадров (`feature/r10-pacing`)

Причина неровности [E]: при AQ каждый батч main-очереди ждал последний батч compute-lane → перекрытие очередей 2–3 мс
на кадр ~28 мс, кадры чередовались 17/40 мс (lag1 −0,73). REA-3 без AQ на одной очереди структурно не лечится.
Исправление `SSPC_R10_AQ_MAIN_FREE=1`: main ждёт только lane-батчи, от которых зависит (конфликт диапазонов, GDS/LDS,
образы/неизвестные доступы, гостевые ожидания, fence ворот REA-3). Перекрытие 8–10 мс, lag1 +0,15, доля интервалов
< 22 мс 28 % → 3 %.

Медианы, FPS / 1% low / P99 (мс):

| сцена | база (play.cmd) | REA-3 + AQ | REA-3 + AQ + MAIN_FREE |
|---|---|---|---|
| сейв владельца, n = 4 | 31,57 / 19,0 / 38,5 | 34,75 / 17,6 / 48,2 | **37,56 / 19,7 / 38,5** |
| бой (город), n = 4 | 46,59 / 24,8 / 31,2 | 50,39 / 25,3 / 31,5 | **53,04 / 26,9 / 27,8** |
| город, n = 2 | 48,44 / 22,7 / 33,7 | 50,21 / 19,2 / 35,3 | 50,74 / 23,2 / 32,7 |
| Neon Dash, n = 2 | 38,92 / 18,9 / 42,9 | 36,62 / 13,1 / 56,9 | 38,06 / 16,3 / 48,8 — **хуже базы** |
| пляж, n = 2 | 58,41 / 50,7 / 19,7 | 58,93 / 51,0 / — | 58,76 / 48,3 / 20,0 — паритет (упор ~59 FPS) |
| порт, деревня | NOT TESTED | NOT TESTED | NOT TESTED |

Кадров > 50 мс в минуту в Neon: база 8,6, REA-3+AQ 57, MAIN_FREE 18,8. Причина в Neon [E]: 13 400 compute-батчей
остаются на main (в других сценах 2–76), ~33 000 разрезов батчей, CP 13,1 мс против 8,7.
Vulkan Validation + Synchronization Validation на финальном коде (сейв и бой): **0 сообщений**; device lost / [Error] — 0.
**Soak ≥ 20 мин — NOT TESTED** (в R9 REA-3 зависал примерно через 16 мин).

Коммиты: `57dea06` (код MAIN_FREE), `a4b3582` (tools/r10), `5211f19` (play_experimental.cmd, README), `98c9669`
(docs/R10_PACING.md), `35e9cfe` (play.cmd: знак % в блоке).

## 3. cp — меньше работы CP по draw (`feature/r10-cp`)

Сделано: `SSPC_R10_NATIVE_DRAWS=verify|guarded|native` — тени 0x43d480 и меш 0x1a1960 из состояния CUE движка без
обхода SRT (тем же механизмом, что нативный G-буфер 0x1a1770); `SSPC_R10_RUN_PS=1|verify` (+`_MATERIAL`) — draw меша/
теней со своим PS user data присоединяется к run; диагностика `SSPC_R10_T2_PARTS`.

Проверки [E]: тени — 8,58 млн и 6,60 млн сравнений стадий, 0 расхождений; меш — 5 из 7,42 млн и 1 из 4,26 млн
расхождений (VB 1 одной VS 0x2d2f4f100; какой источник прав — не выяснено → меш в native небезопасен); RUN_PS —
25,8 млн и 21,4 млн членов run, 0 расхождений.

A/B ABBA n = 4, FPS (медиана):

| сцена | база | RUN_PS + _MATERIAL |
|---|---|---|
| город, бой | 45,89 | 43,06 |
| город, спокойно | 51,77 | 38,27 |
| сейв владельца | 31,01 | 31,29 (шум) |

`NATIVE_DRAWS=native` (n = 1): 45,32 против 45,60 — без изменений. Профиль этапов CP (город, бой) [E]: T1 PM4 7,7 мс,
T1 ждёт полное кольцо 9,1 мс, фронтенд busy 14,2 мс, T2 busy 16,8 мс (BindStage ~6 мс). Узкое место — запись T2 и кольцо
T1→T2, не подготовка на фронтенде. Validation для обоих флагов — NOT TESTED (флаги не рекомендованы).

Коммиты: `a5baace`, `1980b84` (код), `c1a4338` (docs/R10_CP.md).

## 4. sched — CPU/GPU Work Scheduler (`feature/r10-sched`)

Сделано: `SSPC_PIPE_PREFETCH` (перенос незакоммиченного R9-N), таймлайн в stall trace (FrontWait/FrontPrep/EmitWait/
EmitRun/PipeBuild/InputEvent/PresentDone/SchedTask) и замер задержки ввод → present (`SSPC_R10_LATENCY`), пул
work-stealing для рабочих конвейеров с дренажами (`SSPC_R10_SCHED`), разнос тяжёлых потоков (`SSPC_R10_SPREAD`),
журнал affinity, разбор загрузки ядер по ETW.

Тёплый кэш (как играет владелец) [E]: ни один флаг sched не меняет FPS и 1% low сверх шума — предел в
последовательной цепочке CP → T2 → GPU. Холодный кэш, Neon, n = 1:

| конфиг | FPS | 1% low | p99 | max |
|---|---|---|---|---|
| база | 12,9 | 0,32 | 1676 мс | 5,5 с |
| `SSPC_PIPE_PREFETCH=1` | 26,1 | 1,26 | 407 мс | 2,5 с |
| prefetch + `SSPC_R10_SCHED=1` | 25,9 | 1,40 | 388 мс | 1,55 с |

Регрессия первой версии пула (мир не рисовался, refused 2553) исправлена дренажами: refused 0, мир на месте.

Загрузка ядер (бой, ETW) [E]: заняты все 12 логических CPU (24–66 %), игра — 471 % одного CPU из 1200 % (39 % машины),
ожидание CPU после пробуждения ~3 мкс — конкуренции за ядра нет. Самый загруженный — главный поток игры
(guest:Game:Main, 98 % одного CPU), дальше GpuEmit 70 %, GpuRecord 65 %, GpuFrontend 64 %, GpuNative 63 %, пять
JobWorker по ~17 %. Разнести главный поток игры по ядрам без переписывания логики движка нельзя.
Задержка ввода: ~2,8 интервала кадра от события SDL до возврата vkQueuePresentKHR при 56 FPS; SPREAD её не меняет.

Коммиты: `fdfd929` (PIPE_PREFETCH), `b0ed130` (таймлайн, LATENCY), `b427cbc` (пул, SPREAD, AFFINITY_LOG), `16ca862`,
`2641205`, `4e18557` (tools/r10), `ae9ac87` (docs/R10_CPU_GPU_SCHEDULER.md).

Тёплый кэш, ABBA, медианы (A — play.cmd; B — `SSPC_R10_SCHED=1` + `SSPC_PIPE_PREFETCH=1`; S — `SSPC_R10_SPREAD=1`;
R0 — `SSPC_REFLEX=0`), FPS / 1% low / p99 (мс):

| сцена (n) | A | B | S |
|---|---|---|---|
| город, бой (3) | 46,60 / 25,00 / 31,8 | 46,52 / 26,10 / 31,6 | 46,04 / 24,68 / 31,7 |
| traverse (2) | 49,80 / 24,05 / 31,5 | 48,95 / 21,75 / 34,8 | 48,74 / 21,79 / 33,5 |
| Neon (2) | 35,74 / 16,23 / 48,3 | 36,18 / 20,47 / 42,2 | — |

На тёплом кэше в пуле 0 задач и prefetch ставит в очередь 0 — разница A/B шум; в Neon A ещё достраивал 11–24
графических конвейера, B — ни одного. Мир в B рисуется полностью (`E:\research\r10\runs\sched\ab_neon\shots_A1_B1_A2_B2.png`).
Задержка ввод → present (27 поворотов камеры): A p50 46,5–47,7 / p95 55–60 мс; R0 47,0–48,8 / 61–62; S 46,6–49,6 / 57–61.
Validation + Sync (val_B): 0 сообщений, но пул в этом прогоне не получил ни одной задачи — пути пула под валидацией
NOT TESTED. Пляж S, порт, деревня, полёт, холодная серия n ≥ 3 — NOT TESTED.

## 5. ajm — свой AJM / ATRAC9 (`feature/r10-ajm`)

Все 11 импортов libSceAjm обслуживает `src/runtime/audio/ajm.cpp` при `SSPC_R10_AJM=native`; ATRAC9 декодирует своя
копия LibAtrac9 (`third_party/LibAtrac9`, **MIT**, символы переименованы `SspcAt9_*`, не пересекается с копией моста).
Находка [E]: в игре вызываются только `sceAjmInitialize` и `sceAjmModuleRegister` — музыка и диалоги идут через Wwise
Vorbis в eboot. Декодирование проверено на ATRAC9-файле игры (`sce_sys\snd0.at9`, 40,5 с) той же последовательностью
вызовов, что строит код игры: 1189 batch, 15 192 кадра, 15,6 МБ PCM, **0 расхождений** PCM и sideband с оракулом,
сверка с FFmpeg ±1 LSB; self-test ошибок 9/9.

Коммиты: `d36f62b` (код), `9f24434` (tools), `e9ccf2f` (docs), `f2838b8` (исправления ревью), `b39ff28` (docs).

## 6. sysrt — CommonDialog / libSceNet / libc (`feature/r10-sysrt`)

`sceCommonDialogInitialize` (0 / `0x80B80002` повторно), состояние диалогов моста через адаптер; свои `sceNetInit/Term/
ErrnoLoc/PoolCreate/PoolDestroy/InetNtop/InetPton` (пулы id 3000.., errno на поток, офлайн без ложного успеха);
libc `vprintf` (va_list SysV), `fflush` и `_Stdout`, `sceLibcHeapGetTraceInfo`.
Self-test: 102 проверки, 0 расхождений. Владельцы 417 импортов: NATIVE / BRIDGE / COMMON_STUB / UNRESOLVED
338 / 59 / 18 / 2 → **350 / 49 / 17 / 1**; вызываемых за обычный запуск не у SecondSonPC — 9 → **2** (оба Ajm, закрыты
флагом `SSPC_R10_AJM=native`). Таблица пересчитывается автоматически: `tools/r10/sysrt_deps.py` (из
`import_table.inc`, binding table журнала и calls.json прогона) → `E:\research\r10\runs\sysrt\deps_final.{md,csv}`.

| прогон | режим | итог | FPS / 1% low | ошибок | автосейв |
|---|---|---|---|---|---|
| n_control | native: запуск → загрузка → геймплей → меню → «Загрузка» | exit 0 | 47,5 / 28,6 | 0 | да |
| n_user_calm / b_user_calm | native / bridge | exit 0 | 29,8 / 26,4 · 30,2 / 19,3 | 0 | да |
| n_city_menu / b_city_menu | native / bridge | exit 0 | 52,4 / 27,0 · 51,7 / 25,0 | 0 | да |

Коммиты: `a18986e` (код), `02bf2eb`, `0ae9d9a` (tools), `7263b71`, `de0f98b` (docs).

**Находка вне R10:** ручное «Сохранение»/«Загрузка» из настроек не работает ни в bridge, ни в native — диалог
сохранений моста не рисуется на презентере SecondSonPC и всё время отвечает RUNNING. Автосохранения работают.
Нужна отдельная задача (свой SaveDataDialog).

## 7. Сводка ветки `feature/r10-integration`

| коммит | что |
|---|---|
| `d24d032` | merge feature/r10-sysrt |
| `da0197e` | merge feature/r10-cp |
| `997eede`, `ccd8c49` | merge feature/r10-pacing |
| `b185783` | merge feature/r10-sched (конфликты CMakeLists.txt и stall_trace.{h,cpp}: оставлены добавления обоих) |
| `6a9c599` | merge feature/r10-sched (итоговые tools и docs) |
| `0e05aa3` | `game/play.cmd`: `SSPC_R10_AJM=native`, `SSPC_R10_SYSRT=native` |

Сборка `feature/r10-integration` (build_wt.ps1, exe 20:31:31): без ошибок. Смоук-прогон
`E:\research\r10\runs\coordinator\integ_fight` (город, бой, 150 с, пресет + `SSPC_R10_AJM=native` +
`SSPC_R10_SYSRT=native`) [E]: exit 0, [Critical]/[Error] 0, device lost 0; в журнале привязки Ajm и группы sysrt —
`NATIVE`, `guest stdout: FIOS2 ...`, `sceNetPoolCreate(...) = 3000`; мир, персонаж, HUD и миникарта рисуются (screen.png).
**FPS этого прогона не валиден:** в 20:27 запустился посторонний `java.exe` (Gradle-демон, не R10) — фоновая загрузка CPU
77 % против 44–50 % в чистых прогонах (35,5 FPS). Производительность сведённой сборки — NOT TESTED.

## 8. Не проверено и риски

- **NOT TESTED:** порт и деревня (все агенты); soak ≥ 20 мин для MAIN_FREE; холодная серия PIPE_PREFETCH с n ≥ 3;
  Validation для флагов cp и SCHED; ручное «Загрузка» в bridge.
- MAIN_FREE: валидация шла на 6–8 FPS — редкие гонки на полной скорости слой мог не увидеть; построчной сверки кадров
  не было; изменена логика `Timeline::DenseLocked`.
- ab2 pacing: часть конфигураций n = 1 (грязные прогоны отброшены).
- Сведённая сборка: функционально проверена одним прогоном (§7), FPS сведённой сборки не замерен.
- Флаги cp/sched/pacing в сведённой сборке выключены; их код компилируется вместе (общие правки stall trace).

## 9. Дальше

1. Главный поток игры (98 % одного ядра): профиль по функциям → вынести наши вызовы и ожидания, ускорять горячие
   функции движка нативно.
2. Запись T2 (~17 мс busy в городе) — распараллеливание записи command buffers.
3. Neon Dash при AQ: держать compute с GDS/LDS на lane, чтобы MAIN_FREE можно было включить в play.cmd.
4. Свой SaveDataDialog.
