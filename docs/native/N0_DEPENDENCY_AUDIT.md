# N0 — аудит зависимостей эталонного проекта (E:\SecondSonPC)

Дата: 2026-10-15. Состояние эталона: ветка `feature/p52-gnmx`, HEAD `747a7f7`, в рабочем дереве много
незакоммиченных правок (P5.3/P5.4) — **не тронуты**. Аудит только читал файлы.

Категории: **A** — полностью нативный Windows-код; **B** — свой код, переносим после адаптации; **C** — существует
ради эмуляции PS4 (ядро, Gnm, PM4, гостевая память, загрузка eboot); **D** — код/архитектура shadPS4; **E** — неизвестно.

## 1. Главное

* `SecondSonPC.exe` эталона — это **CMake-цель самого shadPS4** (`cmake/shadps4_bootstrap.cmake`: добавляет
  `third_party/shadPS4`, убирает его `main.cpp`, добавляет `src/app/main.cpp` и `src/bridge/shadps4/*.cpp`). Без
  моста сборка невозможна (`CMakeLists.txt`: `SSPC_SHADPS4_BRIDGE=OFF` → FATAL_ERROR).
* `third_party/shadPS4` — **GPL-2.0-or-later**, в git не хранится (`.gitignore`), закреплён на `0fe263a4` + 34 патча
  (`patches/shadps4/*.patch`, 7 244 строки).
* Граница кода (`cmake/sspc_boundary_check.cmake`) запрещает заголовки shadPS4 вне `src/bridge/`, но даже «чистый»
  код берёт fmt, toml11, SDL3, Zydis, xxHash и Vulkan-Headers из `third_party/shadPS4/externals`, а ~16 собственных
  файлов (~13 тыс. строк) помечены GPL «ported from shadPS4».
* Старт: `src/app/main.cpp:116` → конфиг TOML → `Bridge::RunGame` (`src/bridge/shadps4/bootstrap.cpp:1160`) → свой
  `Loader::Linker` грузит `/app0/eboot.bin` + `libc.prx` + `libSceFios2.prx` (LLE) → HLE-библиотеки shadPS4 →
  прыжок в точку входа гостя. GPU: по умолчанию рендерер shadPS4, `--gpu-backend sspc` — свой PM4-процессор и
  Vulkan-исполнитель, **но трансляция шейдеров всегда через shader_recompiler shadPS4**.

## 2. Карта компонентов

| Компонент | Путь | Что делает | Кат. | Перенос в нативный рантайм |
|---|---|---|---|---|
| Лог | `src/base/log.*` (171 стр.) | файловый лог по категориям | A | идея да; код зависит от fmt из shadPS4 → написан заново (`native/engine/core/log.*`, std::format) |
| Жизненный цикл | `src/base/lifecycle.*` | этапы выхода, сторож | A | не нужен на этом этапе |
| Падения | `src/base/crash.*` (994) | VEH/UEF, minidump, классификация гостевых сбоев | B | переписан без гостевых видов (`native/engine/core/crash.*`) |
| VRAM-монитор | `src/base/vram_monitor.*` | DXGI-бюджет | A | можно перенести позже |
| Пути | `src/app/paths.*` | корень exe | A | своя реализация `file::ExecutableDirectory` |
| Оверлей UI | `src/ui/overlay_win32.*`, `draw_list.h` | D2D/DirectWrite поверх окна | A | пригоден для будущего меню |
| Меню настроек | `src/ui/settings_menu.*` | модель меню F1 | B | позже, с новым реестром |
| Хост меню | `src/ui/menu_controller.*` | SDL3, связан с pad и native_backend | B | требует развязки |
| Настройки | `src/settings/*` (2 395) | реестр TOML, атомарное сохранение | B | каркас общий, содержимое под PS4 |
| Конфиг | `src/app/config.*` | `SecondSonPC.toml` | B | PS4-поля (eboot, neoMode, язык PS4) |
| Ввод | `src/input/*` | SDL3 → **виртуальный DualShock 4** (биты Orbis) | B | нативному движку нужны действия → написан `native/engine/platform/input.*` (XInput + клавиатура/мышь) |
| Язык | `src/runtime/language.*` | локаль ↔ id языка PS4 | B | частично |
| PSF | `src/runtime/psf/param_sfo.*` | чтение param.sfo | B | как читатель данных |
| Индекс шейдеров | `src/runtime/gpu/shader/game_shader_index.*`, `gcn_program.*` | поиск футеров OrbShdr в .xpps | B | для инструментов; GCN-код всё равно надо транслировать |
| Vulkan-основа | `src/runtime/gpu/vulkan/vk_api.h`, `vk_foundation.*` | своя таблица функций | A/B | идея повторена, код написан заново (`native/engine/renderer/vk_api.*`) |
| Vulkan-исполнитель | `vk_executor.cpp`, `vk_formats.*`, `vk_pipelines.cpp` | исполнение гостевых команд | C+D (GPL) | **не переносится** |
| Командный процессор | `src/runtime/gpu/command_processor*`, `pm4*`, `parallel_cp*`, `page_tracker*` … (~30 тыс.) | PM4/Gnm | C | **не переносится** (запрещено ТЗ §3) |
| Тайлинг | `src/runtime/gpu/image_layout.*` | GCN-тайлинг, форматы | D (GPL) | **не копировать**; детайлинг для текстур писать с нуля по публичной документации AMD |
| Ядро | `src/runtime/kernel/*` (5 333) | libkernel HLE | C | нет |
| Память | `src/runtime/memory/*` (3 096) | гостевая память | C | нет |
| CPU | `src/runtime/cpu/*` (2 568) | патчер `fs:`, SSE4a | C | нет |
| Загрузчик | `src/loader/*` (3 498) | SELF/ELF, импорты | C | нет |
| ФС/PlayGo/sysmodule | `src/runtime/filesystem/*`, `playgo/*`, `sysmodule/*` | гостевые пути | C/D | нет |
| Движковые хуки | `src/engine/*` (5 874) | хуки eboot, задачи на гостевых данных, трасса рендера | C | **как документация** структур движка (render_layout.h, job_system.h) |
| Мост | `src/bridge/shadps4/*` (23 630) | адаптер shadPS4 | D | нет |
| Ghidra-скрипты | `tools/native-reconstruction/ghidra/*` | pyghidra-запросы к eboot | инструмент | **используются** для исследования (только чтение) |

## 3. Знания, перенесённые в нативный проект (как документация, не код)

* `docs/native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md`: DrawSingle `0x1a1960` — геометрия `+0x58` IB,
  `+0x60` V#, `+0x68` число индексов, TriList, 16-битные индексы. **Подтверждено на файлах** (см. RESOURCE_FORMATS.md).
* `docs/PHASE4_PC_INPUT_UI_RESEARCH.md` §10: заголовок KCAP, T# текстур в CPU-сегменте, тайлинг 13 у атласа.
  Раскладка блоков там описана неверно (сдвиг на 8 байт) — исправлено в этой работе по коду загрузчика.
* `RENDER_ENGINE_PASSES.md`: 41 проход рендера — ориентир для будущего нативного рендерера.

## 4. Вывод для нативного рантайма

Переносить код эталона почти нечего: всё, что работает, либо PS4-специфично (C), либо собрано на библиотеках из
дерева shadPS4/GPL (D). Нативный рантайм (`native/`) написан заново, без единой сторонней библиотеки, кроме
системных (Win32, XInput, vulkan-1.dll) и заголовков Vulkan SDK (Apache-2.0/MIT). Ценность эталона — **знания**
о данных и движке и возможность сверять поведение.
