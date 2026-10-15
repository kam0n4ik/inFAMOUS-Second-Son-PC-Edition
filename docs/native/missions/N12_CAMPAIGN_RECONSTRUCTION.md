# N12 — оригинальная кампания, миссии и граффити: итоговый отчёт (2026-10-15)

Рабочая копия `E:\SecondSonPC-missions` (отдельный worktree), ветка `feature/native-missions` от `806ec6f`
(вершина N11 в `feature/native-rebirth`, проверена: 48/48 тестов). Каталоги `E:\SecondSonPC`, `E:\SecondSonPC-native`,
`E:\SecondSonPC-rebirth` не изменялись. Push не выполнялся. Подробности: `MISSION_FORMAT_RESEARCH.md`,
`GRAFFITI_RESEARCH.md`, `N12_TEST_REPORT.md`.

## 1. Итог в двух фразах

Найдена и задокументирована оригинальная система миссий (хеш имён движка, классы и рефлексия, объекты пакетов,
линковка скомпилированных скриптов splice, виды пакетов, тексты); написан нативный Mission Runtime (события,
цели, условия, действия, последовательности, триггеры, сохранения). Первая миссия игры VOR восстановлена до первого
интерактивного шага: игрок управляет Делсином на крыше консервного завода, рисует **оригинальное граффити
`graffiti_a8_family`** в две стадии (данные, слои, маркеры, камера — из игры) и получает следующую цель с оригинальным
текстом. Это **реконструкция по данным**, а не исполнение сценария игры: виртуальная машина splice не декодирована,
переходы между шагами заданы нативным определением.

## 2. Найденные реальные структуры игры

* Хеш имён `h·101 + lower(c)` (словарь eboot `0xa5ff70`, 4 458 значений перечислений, 2 709 имён полей, строковые
  ключи скриптов); классы `cid_<CamelName>`; таблица имён классов `0xdac130`; рефлексия `0xd057a0` (имена, типы,
  **смещения полей**, методы).
* Объекты пакета: каталог `{n, n, указатели с +8}`, заголовок (тип по DIC, OID, флаги, преобразование).
* **PLPS = таблица линковки кода splice** (три таблицы: члены, методы, глобальные функции); правила чтения
  непосредственных операндов и аргументов вызовов.
* Виды пакетов: миссии 0xA (65), катсцены 0xF (825), граффити 0x19 (59) — гипотеза N11 подтверждена.
* Миссия VOR = начало игры: объекты, точки героя, объёмы, брифинги, реплики, катсцены (`MISSION_FORMAT_RESEARCH.md` §6).
* Граффити: `GRAFFITI_MINIGAME_DATA` (размеры, камера, стадии `ADD_STAGE`, маркеры `LO_ARRAY`), установка через
  `PROXY_SITE` с тем же OID, механизм (трафарет + маска распыления), текстуры слоёв (`GRAFFITI_RESEARCH.md`).
* Тексты `GAMETEXT` (12 091 запись, реплики с заголовком).

## 3. Исследованные функции eboot

`0x2a6e40`, `0x4cfcb0`, `0x30b1c0`, `0x30a910`, `0x30bc00`, `0x5ac180`, `0x2e1f20`, `0x5ad8d0`, `0x277700`,
`0x1f930`/`0x1ee960`, `0x1eeb00…0x1eec10`, `0x2e53d0`, `0x1ed990`, `0x1edb60` (таблица с назначением —
`MISSION_FORMAT_RESEARCH.md` §8). Создание/загрузка миссии, смена состояния и сохранение прогресса в коде eboot
**не найдены** (вызываются из VM splice, она не разобрана) — см. §9.

## 4. Подтверждённые события оригинальной кампании (порядок по данным)

1. Вступительный текст (`u_vor_intro_narrative`, `u_vor_intro_stinger`) → катсцена `vor_taggin`.
2. Делсин на мостике щита крыши рыбоконсервного завода (`HERO_MATCH_POINT` 0x21278fb67dc6d9a0), граффити
   `graffiti_a8_family` (карикатура шерифа Реджи), подсказка «Stencil Art» / «Spray-paint».
3. Реплики `m_vor_tagreg_02_hro`, катсцена `vor_spotlight`, реплики `m_vor_ohnocop_01..03_hro`.
4. «Find a safe way down off the fish cannery roof» → «Find the back door out of the fish cannery» → «Move along
   the beach to the Longhouse» → катсцены `vor_brothers`, `vor_meetbet`.

Шаги 1–4 подтверждены ключами текстов в коде `msn_vor` и объектами миссии; **точные условия переходов — нет**.

## 5. Что реализовано нативно

| Подсистема | Файлы |
|---|---|
| Чтение оригинальных данных: объекты, DIC, PLPS и операнды, реестр типов | `resources/game_objects.*`, `resources/type_registry.*` (+`type_registry_table.inc`), `core/name_hash.h` |
| Тексты игры | `resources/game_text.*` |
| Определение граффити, установка в чанке | `resources/graffiti_data.*` |
| Сводка пакета миссии (точки героя, объёмы, ключи текстов) | `resources/mission_data.*` |
| Шина событий | `gameplay/events.*` |
| Определения миссий (текстовый формат `sspc-mission 1`, проверка) | `gameplay/mission/mission_definition.*` |
| MissionManager, MissionInstance-состояние, SequencePlayer, триггеры once/repeat, флаги, XP, приостановка, провал/повтор, сериализация | `gameplay/mission/mission_runtime.*` |
| Взаимодействия и объёмы-триггеры | `gameplay/interaction.*` |
| Мини-игра граффити (камера, курсор, маски, маркеры, стадии, события, сохранение) | `gameplay/graffiti.*` |
| Фрагмент пролога (связка, HUD, отладка, сохранение/загрузка) | `gameplay/prologue.*`, `config/missions/vor_prologue.mission` |
| Рендер: декали с маской, динамические R8-текстуры (безопасное обновление в кадре), экранный текст | `renderer/renderer.*`, `renderer/text_overlay.*`, `shaders/decal.frag`, `shaders/overlay.*` |
| Сохранения: версия, разделы, CRC-32, атомарная запись | `core/save_file.*` |
| Ввод: Interact, Spray, Cancel, ToggleDebugView, QuickSave/Load, кнопки мыши, курки XInput | `platform/input.*` |

Состояния миссии: NotStarted, Active, ObjectiveCompleted, Completed, Failed, Suspended. События: вход/выход из
области, взаимодействие, доступность взаимодействия, анимация/действие/диалог завершены, сюжетный триггер,
смена состояния объекта, события граффити, завершение последовательности. Цепочки «условие → действие → ожидание →
следующий шаг» с отменой и восстановлением после загрузки (индекс действия сдвигается до выполнения — действие после
загрузки не повторяется).

## 6. Что является приближением (честный список)

* **Логика миссии** — нативное определение `vor_prologue.mission` (порядок, тексты и объекты оригинальные,
  переходы и тайминги свои). Катсцены `vor_taggin`/`vor_spotlight` не воспроизводятся (сообщение в HUD).
* **Здание консервного завода и щит не рисуются**: это модели объектов `OBJECT_DATA` чанка `a8`, а не экземпляры
  сцены чанка; их формат не разобран. Мостик (на высоте точки героя, по ширине стены) и подложка щита — серые
  приближения в коде (`PrologueSlice::ApproximateCollision`); поэтому оригинальные объёмы «спустился с крыши»
  (на полу завода, z ≈ 10 м) недостижимы — добавлен **резервный нативный объём** под крышей.
* Начало системы объектов чанка `a8` = (100, −200, 0) м выведено по камере граффити (общее правило не доказано).
* Объём `VOLUME_BUTTON`: масштаб принят за полуразмеры в метрах (единичная фигура не разобрана).
* Граффити: композиция слоёв, кисть, правило завершения стадии (все маркеры), проявление, курсор и переходы камеры —
  нативные; анимации рисования Делсина нет (idle); встряхивания баллончика нет.
* Кнопка взаимодействия поставлена у стены (у оригинальной `USE_BUTTON` родитель не разобран; радиус 2 м — из данных).
* HUD, подписи кнопок ПК вместо значков PS4, отладочная панель — нативные.

## 7. Используемые оригинальные ресурсы (локальная копия пользователя)

`art\cache\msn_vor.sprig.xpps`, `graffiti_a8_family.xpps`, `a8.xpps`, `a3.xpps` (мир), `core_village.sprig.xpps`,
`core_common.sprig.xpps` (общие), `lang_english_text.xpps` (тексты), `hero.xpps` и пакеты N11 (персонаж, анимации).
В репозиторий игровые данные не входят; добавлена только таблица 346 пар «хеш имени типа → имя».

## 8. Проверки

Release/Debug/ASan: 71/71 тест (48 прежних + 23 новых); игровые прогоны пролога под Vulkan Validation (0 сообщений)
и под ASan; регрессия N11 (три чанка, валидация 0); аудит зависимостей PASS. Подробно — `N12_TEST_REPORT.md`.

## 9. Оставшиеся ограничения и следующий шаг

1. Декодировать VM splice (опкоды, ожидания, сигналы) — тогда определения миссий можно строить из кода игры.
2. Модели `OBJECT_DATA` (здание завода, щит) и иерархия объектов чанка (родители, начало системы чанка).
3. Катсцены (`#tandem#`: камера, анимации персонажей, звук).
4. Анимации граффити, встряхивание, оригинальный шейдер краски, PS4-сохранения — не начаты.
5. Геймпад проверен тестами и кодом, живой прогон с контроллером не выполнялся.

## 10. Изменённые файлы

Новые: `engine/core/{name_hash.h,save_file.*}`, `engine/resources/{type_registry.*,type_registry_table.inc,game_objects.*,
game_text.*,graffiti_data.*,mission_data.*}`, `engine/gameplay/{events.*,interaction.*,graffiti.*,prologue.*,
mission/mission_definition.*,mission/mission_runtime.*}`, `engine/renderer/text_overlay.*`,
`shaders/{decal.frag,overlay.vert,overlay.frag}`, `config/missions/vor_prologue.mission`,
`tests/{test_mission,test_graffiti,test_n12_assets}.cpp`, `tools/research/missions/*.py`, `docs/native/missions/*`,
`docs/native/n12/*.png`.
Изменённые: `engine/renderer/{renderer.*,vk_api.h}`, `engine/platform/input.*`, `game/main.cpp`,
`tools/asset_inspector/main.cpp` (команда `near`), `config/SecondSonPC.ini` (`[Prologue]`, новые клавиши),
`CMakeLists.txt`, `docs/native/{RESOURCE_FORMATS,REVERSE_ENGINEERING,IMPLEMENTATION_LOG,KNOWN_LIMITATIONS}.md`.

## 11. Коммиты

См. `git log feature/native-missions` (коммиты N12 поверх `806ec6f`; хеши — в сообщении итогов сессии).

## 12. Как запустить и проверить

```
powershell -ExecutionPolicy Bypass -File E:\SecondSonPC-missions\native\build.ps1 -Config Release -Test
cd /d E:\SecondSonPC-missions\native\build\release\bin
SecondSonPC.exe --config E:\SecondSonPC-missions\native\config\SecondSonPC.ini
```
Пролог включён по умолчанию (`[Prologue] Enabled = true`; `--no-prologue` возвращает сцену N11). Управление:
WASD — движение, мышь — камера, **E / X** — «Stencil Art» у щита, мышь/стик — курсор, **ЛКМ / F / RT** — краска,
**Q / B** — выйти, **F4** — отладочная панель, **F6/F9** — сохранить/загрузить (`saves\prologue.sav`), F3 — свободная
камера. Автосценарий со снимками:
`SecondSonPC.exe --config ... --prologue-autoplay --frames 9000 --shot 32s:done.bmp --debug-view`.
Сохранение/загрузка из командной строки: `--save-at <кадр>:<файл>`, `--load-save <файл>`.
