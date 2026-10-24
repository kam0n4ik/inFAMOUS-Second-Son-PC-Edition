# R10 — собственный AJM (libSceAjm) и декодирование ATRAC9

Агент **ajm**, ветка `feature/r10-ajm` (от `origin/main` 6d74185), worktree `E:\SecondSonPC-r10-ajm`.
Сырые данные: `E:\research\r10\runs\ajm\`. Метки: **[E]** измерено, **[A]** анализ (Ghidra/код), **[H]** гипотеза.

## 0. Итог в двух словах

- Все 11 импортов `libSceAjm` игры обслуживаются собственным модулем `src/runtime/audio/ajm.cpp` при
  `SSPC_R10_AJM=native` — shadPS4 при этом не вызывается (в журнале привязки — `NATIVE`, счётчик импортов — владелец `NATIVE`).
  ATRAC9 декодирует собственная копия LibAtrac9 (`third_party/LibAtrac9`, MIT) с переименованными символами.
- По умолчанию (`SSPC_R10_AJM` не задан или `bridge`) — всё как было (оракул shadPS4). `verify` — оба пути на одном входе.
- **Главная находка [E]:** в реальных прогонах игра вызывает только `sceAjmInitialize` (1) и `sceAjmModuleRegister`
  (1, ATRAC9). Ни одного экземпляра декодера и ни одного batch ни в одной из 7 сцен/маршрутов (город calm/fight, сохранение
  владельца, сцена перед роликом). Вся музыка и все диалоги игры — Wwise Vorbis (§1.2), их декодирует сама игра (Wwise в
  eboot) без AJM. Поэтому декодирование ATRAC9 проверено не на игровом потоке, а на настоящем ATRAC9-файле игры
  (`sce_sys\snd0.at9`, 40,5 с музыки) **ровно той последовательностью вызовов, которую строит код игры** (§2): 1189 batch,
  15 192 кадра ATRAC9, 15,6 МБ PCM, **0 расхождений** PCM и sideband с оракулом; независимая сверка с FFmpeg — ±1 LSB.

## 1. Импорты и реальные вызовы

### 1.1. Число вызовов (счётчик импортов `calls.json`, прогоны R10) [E]

| функция | NID | city calm (verify, 172 с) | city fight (native, 152 с) | user calm (native) | video (native) |
|---|---|---:|---:|---:|---:|
| `sceAjmInitialize` | `dl+4eHSzUu4` | 1 | 1 | 1 | 1 |
| `sceAjmModuleRegister` | `Q3dyFuwGn64` | 1 | 1 | 1 | 1 |
| `sceAjmInstanceCreate` | `AxoDrINp4J8` | 0 | 0 | 0 | 0 |
| `sceAjmInstanceDestroy` | `RbLbuKv8zho` | 0 | 0 | 0 | 0 |
| `sceAjmBatchJobControlBufferRa` | `dmDybN--Fn8` | 0 | 0 | 0 | 0 |
| `sceAjmBatchJobRunSplitBufferRa` | `7jdAXK+2fMo` | 0 | 0 | 0 | 0 |
| `sceAjmBatchStartBuffer` | `fFFkk0xfGWs` | 0 | 0 | 0 | 0 |
| `sceAjmBatchWait` | `-qLsfDAywIY` | 0 | 0 | 0 | 0 |
| `sceAjmBatchErrorDump` | `WfAiBW8Wcek` | 0 | 0 | 0 | 0 |
| `sceAjmModuleUnregister` | `Wi7DtlLV+KI` | 0 | 0 | 0 | 0 |
| `sceAjmFinalize` | `MHur6qCsUus` | 0 | 0 | 0 | 0 |
| (`sceAudioOutOutputs`, для масштаба) | | 7 662 | 6 767 | 5 441 | 5 497 |

То же в прогонах R9 (`E:\research\resolution\oldwt\logs\calls.json`, `E:\research\video_hang\oldexe\wt\logs\calls.json`,
путь моста): Initialize 1, ModuleRegister 1, остальное 0. Unregister/Finalize вызываются только при штатном выходе из
игры (§2.1), прогоны закрывают окно раньше.

### 1.2. Почему batch-ей нет: ресурсы игры [E]

Разбор PSARC (`tools/r10/psarc_codec_scan.py`: TOC → zlib-блоки → RIFF `fmt `, поиск GUID ATRAC9):

| архив | WEM | кодек (fmt tag) |
|---|---:|---|
| `english_a.psarc` (диалоги) | 1450 | 0xFFFF (Wwise Vorbis): 1ch 48k/24k/32k, 6ch 32k, 2ch 32k |
| `english_b.psarc` | 2310 | 0xFFFF |
| `english_a_p.psarc` | 131 | 0xFFFF |
| `music_a/b/c.psarc` (музыка) | 68 / 438 / 130 | 0xFFFF, 4ch 48k (2 файла 2ch) |

GUID ATRAC9 (`D2 42 E1 47 …`) не встретился ни в одном файле этих архивов. Единственный ATRAC9 в дистрибутиве —
`sce_sys\snd0.at9` (музыка экрана PS4; игра его не читает). Звуки эффектов лежат в пакетах `*.xpps` (KCAP) — их я не
распаковывал; но и в бою (city fight, user) ни одного `sceAjmInstanceCreate` — источников ATRAC9 игра не создаёт.

## 2. Как игра использует AJM (Ghidra, `eboot.bin`, проект `E:\tools\ghidra-projects\scripts`) [A]

Импорты подписаны (`ps4import::*`, слоты GOT `0xd9ed98…0xd9ede8`). Все вызовы — в одном подсистеме: Wwise-источник
ATRAC9 (кодек-плагин Wwise с ID 0xC = `AKCODECID_ATRAC9`).

### 2.1. Вызовы и их места

| место | что делает |
|---|---|
| `0x8573b0` (← `0x8461a0`, инициализация звука) | `0x819ae0(0, 0xC, 0x858160, 0x857d70)` — регистрирует в Wwise фабрики кодека 0xC (ATRAC9), затем `0x872b60` |
| `0x872b60` | `sceAjmInitialize(0, &ctx)`; `sceAjmModuleRegister(ctx, 1 /*ATRAC9*/, 0)` (результаты не проверяются); `ctx` — глобальный `0x7fa4508` |
| `0x872b90` (← `0x8573f0` ← `0x846260`, завершение) | `sceAjmModuleUnregister(ctx, 1)`; `sceAjmFinalize(ctx)` |
| `0x857d70` (фабрика) | источник 0x138 байт, vtable `0xd97610` |
| `0x857fb0` (vtable +0xC8) | `sceAjmInstanceCreate(ctx, 1, flags, &src->inst /*+0xF8*/)`, `flags = (popcount(channel_mask & 0x3FFFF) << 3) | 1` — версия 1, формат S16 (0), без флагов кодека |
| `0x858020` / `0x8580c0` (vtable +0xD0) | при первом запуске голоса: `sceAjmBatchJobControlBufferRa(job, inst, 0x6001, src+0x12C /*4 байта config + 4*/, 8, src+0xF0 /*Result*/, 8, ra)`; резерв в batch — **0x40** байт; затем `0x8736c0` |
| `0x8736c0` | `sceAjmBatchJobRunSplitBufferRa(job, inst, 0xC00000001001, src+0x78 /*1–2 входа*/, n_in, src+0x98 /*1–2 выхода*/, n_out, src+0xB8, 0x38, ra)`; резерв **0x30 + 0x10·(n_in+n_out)** |
| `0x872c70` (← `0x857460`, кадр звука, перед `sceAudioOutOutputs`) | на стеке buffer `n·0x40 + m·0x70`, все задания кадра → `sceAjmBatchStartBuffer(ctx, buf, size, 0x28, &err, &id)` → `sceAjmBatchWait(ctx, id, 0xFFFFFFFF, &err)`; при ошибке старта/ожидания — `fputs("DECODING ERROR!!\n")` + `sceAjmBatchErrorDump(&batch_info, &err)`; переполнение → `err.code = 0x80930012` (JOB_CREATION) |
| `0x873200` (vtable +0x58) | `sceAjmInstanceDestroy(ctx, inst)`, если голос запускался |

Если голосов ATRAC9 нет, `0x872c70` выходит до `StartBuffer` — это и видно в счётчике (§1.1).

### 2.2. ABI и структуры (основания)

- **Флаги задания** (u64): версия 0–2, кодек 3–10, run 11–12, control 13–15, sideband 45–47.
  `0x6001` = версия 1 + control `Reset|Initialize`. `0xC00000001001` = версия 1 + run `MultipleFrames` + sideband
  `Format|Stream`. [A: значения из кода игры; раскладка — оракул `ajm.h` `AjmJobFlags`]
- **Sideband выхода run-задания** = 0x38 байт = `Result`(8) + `Stream`(16: consumed s32, written s32, total u64) +
  `Format`(24) + `MFrame`(8) — ровно размер, который передаёт игра. [A: размер из игры; порядок — оракул]
- **Sideband входа control** = 8 байт: 4 байта config ATRAC9 + 4 резерв (`src+0x12C`). [A]
- **`AjmBuffer`** = {указатель, u64 размер}, массивы на `src+0x78` (вход) и `src+0x98` (выход). [A]
- **Формат batch-буфера**: заголовок чанка 4 байта (ident 6 бит, payload 20 бит); задание = {hdr(ident 0, payload =
  instance), u32 размер тела}; буферы 16 байт {hdr, u32 size, ptr}; флаги 8 байт {hdr(payload = старшие 32 бита), u32
  младшие}; ident 1/2/3/4/6/7/17/18 — вход run / вход control / флаги control / флаги run / RA / inline / выход run /
  выход control. [A: оракул; подтверждение — размеры, которые резервирует игра: control 8+16+16+8+16 = **0x40**, run
  8+16+16·n+8+16·m+16 = **0x30+0x10·(n+m)** — совпадают байт в байт только при этой раскладке и при ненулевом RA]
- **`SceAjmBatchError`** = {s32 code, ptr job, u32 cmd_offset, ptr job_ra} (32 байта): игра заполняет его сама при
  переполнении (`local_158/150/148/140`). [A]
- **`sceAjmBatchErrorDump(info, err)`**: `info` = {buffer, offset, size, current_job, current_ra} — то, что игра держит
  для построения batch. [A]
- Выход: кольцо 0x1000 сэмплов S16 на голос; запрос — целые суперкадры (`+0x70`/`+0x72` источника); 1 или 2 выхода (стык
  кольца), 1 или 2 входа (стык петли). Игра не читает `consumed/written` из sideband (позиции считает сама) — значит
  декодер обязан записать ровно запрошенное число сэмплов. [A]
- Коды ошибок `0x80930001…17` и биты результата (`NOT_INITIALIZED 1 … NOT_ENOUGH_ROOM 0x10 … CODEC_ERROR 0x40000000,
  FATAL 0x80000000`) — таблицы оракула (`ajm_error.h`, `ajm_result.h`); `0x80930012` подтверждён кодом игры. [A]

## 3. Реализация (`src/runtime/audio/ajm.cpp`, коммит d36f62b)

- **Включение**: `SSPC_R10_AJM=native|verify|bridge` (по умолчанию bridge). Группа экспорта `ajm`
  (`Audio::AjmExports()` в `runtime/exports.cpp`, отдельной строкой; при bridge — пустой список → импорты остаются
  мосту). `--bridge ajm` по-прежнему возвращает группу мосту. Общие сгенерированные таблицы не тронуты.
- **Контексты** (`sceAjmInitialize/Finalize`): id с 1, `reserved != 0` / null → `INVALID_PARAMETER`; Finalize освобождает
  экземпляры и незабранные batch-и.
- **Модули**: регистрируется только ATRAC9 (кодек 1); другой кодек < 23 → `CODEC_NOT_SUPPORTED` [H: у консоли MP3/AAC
  есть, у нас декодера нет — честный отказ вместо пустышки; игра регистрирует только ATRAC9]; повтор →
  `CODEC_ALREADY_REGISTERED`; Unregister незарегистрированного → `CODEC_NOT_REGISTERED`.
- **Экземпляры**: id = индекс (1…0x2FFF) | кодек << 14 (как оракул); версия 0 → `WRONG_REVISION_FLAG`; формат > Float →
  `FLAG_NOT_SUPPORTED` [H]; флаги кодека ATRAC9: разбор RIFF (0x1), неинтерливный выход (0x100).
- **Построители заданий** (`…ControlBufferRa`, `…RunSplitBufferRa`): пишут только в batch-буфер игры, раскладка §2.2,
  маски флагов оракула (`0x6000_0000_E7FF` control, `0xC001_8007` статистика, `0xE000_0000_1FFF` run).
- **`sceAjmBatchStartBuffer`**: размер не кратен 8 → `MALFORMED_BATCH`; полный разбор и проверка всех заданий **до**
  выполнения (неизвестный чанк, лишние флаги, выход за размер → `MALFORMED_BATCH`; неизвестный экземпляр →
  `INVALID_INSTANCE` [H: оракул тут падает ASSERT-ом]) с заполнением `SceAjmBatchError` (job, offset, RA); затем
  задания выполняются **синхронно** в потоке вызова. Обоснование: игра всегда сразу ждёт batch с бесконечным таймаутом
  (§2.1), так что поток-воркер оракула лишь добавлял переключение потоков. `sceAjmBatchWait` забирает результат
  (`INVALID_BATCH` для неизвестного id). Не более 0x400 незабранных batch-ей (`OUT_OF_MEMORY`, как оракул).
- **Задание экземпляра** — поведение оракула (`AjmInstance::ExecuteJob` + `AjmAt9Decoder`), переписанное: sideband входа
  (format, gapless, resample, init), Reset/Initialize, gapless (total/skip/skipped, петля `gapless_loop`), цикл
  MultipleFrames до `NOT_ENOUGH_ROOM`/`PARTIAL_INPUT`, суперкадры (пропуск хвоста суперкадра, дочитывание суперкадра в
  конце gapless), sideband выхода (Result, Stream consumed/written/total, Format с маской каналов и битрейтом, Gapless,
  MFrame, CodecInfo). Структура sideband, которая не помещается, не пишется и даёт бит `SIDEBAND_TRUNCATED` [H].
  Статистический экземпляр 0x80000 — нулевая загрузка движка (декодер на CPU) и объёмы памяти оракула.
- **Без лишних копий**: оракул копирует весь вход каждого задания в `std::vector` и декодирует каждый кадр во
  внутренний буфер с последующим `memcpy`. Здесь вход читается на месте (копия только суперкадра, который пересекает
  стык двух входных буферов — у игры входы кратны суперкадру, так что не бывает); кадр декодируется **прямо в буфер
  игры**, если он целиком помещается в текущий выходной буфер, выровнен и не обрезается skip/gapless (у игры — все
  кадры, кроме попадающих на стык кольца).
- **LibAtrac9**: `third_party/LibAtrac9` — исходники `shadps4-emu/ext-LibAtrac9` @ 946e05a (форк
  Thealexbarney/LibAtrac9 с выходом S32/F32 и неинтерливным выходом), без изменений; **лицензия MIT** (Alex Barney,
  `third_party/LibAtrac9/LICENSE`), совместима с GPL проекта. Своя статическая библиотека `sspc_atrac9`; все 67 внешних
  символов переименованы (`SspcAt9_*`) принудительно включаемым `sspc_atrac9_prefix.h`, поэтому копия не сталкивается
  при линковке с LibAtrac9 моста и имеет свои глобальные таблицы (проверено `llvm-nm`: ни одного внешнего символа без
  префикса). `build.ps1` кладёт лицензию в пакет: `licenses\LibAtrac9.txt`. Граница runtime/bridge: отчёт сборки —
  «own runtime: 415 files … all SecondSonPC's; no bridge namespace».
- **Потокобезопасность**: один мьютекс на модуль; LibAtrac9 при инициализации переписывает свои глобальные таблицы —
  всё под тем же мьютексом.

### 3.1. verify (`SSPC_R10_AJM=verify`)

Каждая экспортируемая функция наблюдает функцию оракула того же импорта (механизм наблюдателей загрузчика,
`Export::original`). Игре отвечает собственный AJM; оракул повторяет состояние (свой контекст, свои экземпляры, карта
id). Каждый batch: разбирается, для оракула строится теневой batch с его id экземпляров, теми же входами, **копиями**
выходных буферов (с текущим содержимым, чтобы незаписанные байты тоже сравнивались) и обнулёнными копиями sideband;
оракул выполняет его (Start + Wait), затем собственный AJM — batch игры; сравниваются байт в байт все выходные буферы и
весь sideband. Счётчики в журнале (`R10 AJM verify (...)`): batch, задания, кадры (из MFrame), байты PCM/sideband,
расхождения, max |Δ| сэмпла S16, отказы оракула; первые 20 расхождений — с дампом sideband.
`SSPC_R10_AJM_DUMP=<папка>` пишет оба PCM-потока по экземплярам (`ajm_c<ctx>_<inst>_own.pcm` / `_oracle.pcm`),
`tools/r10/ajm_pcm_compare.py` сравнивает и пишет WAV.

`SSPC_R10_AJM_SELFTEST=<файл.at9>` (только verify): при `sceAjmInitialize` игры, в **отдельном** контексте (контекст
игры не затрагивается), RIFF-файл ATRAC9 проходит два прохода через verify:
1. **как игра**: экземпляр `(каналы << 3) | 1`, control-задание `0x6001` с config из `fmt `, затем run-задания
   `0xC00000001001` с sideband 0x38 по 2 суперкадра в кольцо 0x1000 сэмплов (2 выхода на стыке кольца), каждое 7-е —
   с входом из двух буферов, RA ненулевой;
2. **RIFF + gapless**: экземпляр с флагом разбора RIFF, весь файл как вход, sideband `Gapless|Format|Stream` + MFrame.

## 4. Проверка

### 4.1. verify на данных игры (`snd0.at9`: ATRAC9 2 канала, 48 кГц, суперкадр 256 байт / 1024 сэмпла, 40,5 с) [E]

| прогон | batch | заданий (run) | кадров ATRAC9 | байт PCM | байт sideband | расхождений PCM / sideband | отказов оракула |
|---|---:|---:|---:|---:|---:|---|---:|
| verify_city_calm | 1189 | 1189 (1188) | 15 192 | 15 577 088 | 68 440 | 0 / 0 | 0 |
| verify_city_calm2 | 1189 | 1189 (1188) | 15 192 | 15 577 088 | 68 440 | 0 / 0 | 0 |
| final_verify_city_fight (exe d36f62b) | 1189 | 1189 (1188) | 15 192 | 15 577 088 | 68 440 | 0 / 0 | 0 |

Дампы PCM обоих путей (`E:\research\r10\runs\ajm\func\verify_dump\`, `.wav`): проход 1 — 7 778 304 байт
(1 944 576 сэмплов на канал, 40,5 с), проход 2 — 7 776 000 байт = ровно `fact.sample_length` 1 944 000 (gapless
обрезает задержку кодера 256 сэмплов и хвост; проход 2 ≡ проход 1 со сдвигом 256 сэмплов — проверено). Own и oracle
**идентичны побайтно** (`ajm_pcm_compare.py`: IDENTICAL, оба прогона). Сигнал не тишина: пик 11 936, RMS ≈ 1145.

**Независимый эталон** (не LibAtrac9): FFmpeg 7 (`C:\ffmpeg\ffmpeg -i snd0.at9 -f s16le`), его собственный декодер
ATRAC9: та же длина 1 944 576; собственный AJM = **−FFmpeg** (противоположная полярность у двух декодеров — у LibAtrac9 и
оракула она одна), после смены знака: max |Δ| = **1 LSB**, 99,99 % сэмплов точно, SNR **103,5 дБ** (разница — округление
float→S16).

### 4.2. Игровые прогоны (`r9run.ps1`, стенд R9, `E:\research\r10\runs\ajm\func\`) [E]

| прогон | режим | сцена / маршрут | итог | `[Error]` в журнале | AJM у игры |
|---|---|---|---|---:|---|
| verify_city_calm | verify + selftest | city / calm | первый запуск worktree: 2 мин инициализации моста съели окно — игра жила ~10 с (не показателен) | 0 | Init 1, Register 1 |
| verify_city_calm2 | verify + selftest | city / calm, 170 с | closed-by-test, геймплей на скриншотах | 0 | Init 1, Register 1 |
| native_city_fight | native | city / fight | closed-by-test, 152 с | 0 | Init 1, Register 1 (NATIVE) |
| native_user_calm | native | user / calm | closed-by-test, 121 с | 0 | Init 1, Register 1 (NATIVE) |
| native_video | native | video / calm (сохранение перед роликом) | closed-by-test, 121 с | 0 | Init 1, Register 1 (NATIVE) |
| final_verify_city_fight | verify + selftest | city / fight, 151 с | closed-by-test | 0 | Init 1, Register 1 (OBSERVED) |
| final_bridge_city_calm | bridge (по умолчанию) | city / calm, 121 с | closed-by-test | 0 | Init 1, Register 1 (BRIDGE) — как до R10 |
| final_native_city_calm | native | city / calm, 121 с | closed-by-test | 0 | Init 1, Register 1 (NATIVE) |

Прогоны `final_*` — на exe из коммита d36f62b; ранние — на exe с тем же поведением до двух косметических правок
(удалена неиспользуемая константа, строка экспорта перенесена).

Звук во всех прогонах выводился (`sceAudioOutOutputs` 5–7,7 тыс. вызовов, порт 0 8ch float → WASAPI), падений нет.

### 4.3. Что НЕ проверено

- **NOT TESTED: декодирование ATRAC9 игровыми голосами** — в игре их нет ни в одной проверенной сцене (§1), сравнивать
  нечего; проверено на `snd0.at9` с последовательностью вызовов игры (§4.1).
- **NOT TESTED на слух**: музыка/эффекты/диалоги — Vorbis Wwise, через AJM не идут, мои изменения их не касаются; на
  стенде никто не слушал. Щелчков/рассинхрона от AJM быть не может, пока игра не создаёт голосов ATRAC9.
- Пакеты эффектов `*.xpps` (KCAP) на ATRAC9 не просмотрены (только рантайм-счётчик: 0 экземпляров в бою).
- Штатный выход из игры (Unregister/Finalize) — прогоны закрывают окно; функции просты, проверены selftest-ом
  (Finalize его контекста).
- Ветки с MP3/AAC, resample, статистикой — не используются игрой; MP3/AAC не реализованы (честный `CODEC_NOT_SUPPORTED`).

## 5. Рекомендации для `play.cmd` и риски

- В `play.cmd` можно добавить `SSPC_R10_AJM=native` — это убирает зависимость от AJM shadPS4; на звук игры влияния нет
  (игра делает лишь 2 вызова). Оставить мост по умолчанию до слияния тоже безопасно.
- Риски: (1) если в какой-то не проверенной сцене появятся голоса ATRAC9, они пойдут через собственный путь — его
  поведение байт-в-байт равно оракулу на реальном ATRAC9, но на игровом потоке не наблюдалось; при сомнении —
  `SSPC_R10_AJM=verify` покажет расхождения в журнале. (2) Синхронный batch выполняется в аудиопотоке игры (как и
  ожидание у оракула) — время то же, минус переключение потоков. (3) В режиме verify selftest добавляет ~0,35 с к
  инициализации (замерено по журналу: 08:50:04.395 → 08:50:04.734).

## 6. Коммиты

| хеш | что |
|---|---|
| d36f62b | код: `src/runtime/audio/ajm.cpp`, `third_party/LibAtrac9` (+ LICENSE, README, prefix), CMakeLists, build.ps1 (лицензия в пакет), exports.cpp (1 строка + объявление), `tools/r10/ajm_pcm_compare.py` |
| (этот) | docs: `docs/R10_AJM.md` |
