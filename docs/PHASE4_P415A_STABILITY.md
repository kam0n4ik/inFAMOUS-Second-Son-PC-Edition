# P4.15A — исправления и проверка стабильности

Дата работы: 12–13 октября 2026, Europe/Moscow.
Статус: **проверка продолжается; фаза не завершена**. Полный selftest имеет один воспроизводимый сбой на исходной и новой сборках. A/B и игровые проверки ещё не завершены. P4.15B не начата.

## Исходное состояние и восстановление

Основной репозиторий: E:\SecondSonPC. Исходная ветка master, HEAD b179ae37d9d55a957b5f8746bd22f975ca965354. До изменений — 14 dirty tracked файлов и untracked P4.14, локальные изменения nested shadPS4 (HEAD 0fe263a4760dfbfa973366890061749b4af0de97). Пользовательские изменения не заменены состоянием HEAD.
После проверенного бэкапа создана ветка work/p415a-stability. Коммитов и push в этой работе нет.

Исходный бэкап: E:\SecondSonPC_Backups\P415A_20261012_234725.
snapshot.zip: 102108 файлов, 4677606906 байт до сжатия, 1830379801 байт архив.
Архив полностью извлечён в restore-verification, SHA-256 каждого сохранённого файла проверен. RESTORE-VERIFIED.txt подтверждает проверку.
Сохранены main.bundle, filtered v3 nested.bundle, raw nested .git с доступными объектами, рабочие деревья, двоичные diff, refs, HEAD, worktrees, manifest-sha256.csv, исключения и версии инструментов.
Nested bundle не выдаётся за полную историю promisor-репозитория: использован blob:none; доступные Git-объекты дополнительно сохранены в архиве.
Исключены воспроизводимые build/toolchain/temp и тяжёлые диагностические файлы; точный перечень в exclusions.csv. Недоступные файлы установленной tools\pydeps отмечены отдельно; исходники не исключались молча.
Две более ранние неудачные попытки бэкапа сохранены, не считаются резервной копией.

Проверенные source checkpoint:
- source-baseline_20261012_235644
- R4_source_20261013_001023
- R3_source_20261013_001024
- R2_source_20261013_001035
- R1_source_20261013_001036

Они находятся в E:\SecondSonPC_Backups\P415A_checkpoints; каждый содержит source.zip, извлечение и manifest с проверенными SHA-256. Это промежуточные исходники, не подтверждение прохождения тестов и не замена полного первоначального бэкапа.

## R4 — синхронизация periodic_ms_

Файлы: src/runtime/gpu/shader/shader_disk_cache.cpp; src/bridge/shadps4/selftest_shader.cpp.
WriterLoop читал periodic_ms_ до захвата mutex_, тогда как SetPeriodic меняет его под mutex_ уже после запуска writer. Само создание thread не упорядочивает последующие вызовы setter.
Первое чтение перенесено под тот же mutex. Все чтения/записи periodic_ms_ теперь имеют mutex happens-before; дополнительные GPU-блокировки не добавлены.
Добавлен стресс-тест: 32 открытия кэша, два конкурентных setter по 10000 вызовов на открытие (640000 изменений), требование фактического выполнения callback и join writer до уничтожения callback context.
Новый тест прошёл. Стресс не является доказательством отсутствия data race; основание корректности — общий mutex. ThreadSanitizer на этой Windows/clang-cl сборке не запускался.

## R3 — единый владелец persistent cache

Файлы: shader_disk_cache.h/.cpp, vulkan/vk_precompile.h/.cpp, native_backend.cpp.
PipelineCacheFile получает ссылку на ShaderDiskCache; владелец должен переживать pipeline cache. Save держит save_mutex и выполняет публикацию через WithWriteOwnership, который под io_mutex проверяет действующий writer lock и открытый pack. Close использует тот же io_mutex при закрытии handles.
Таким образом, lease удерживается через запись .tmp и atomic MoveFileEx; read-only и закрытый cache не могут вызвать запись. Periodic callback регистрируется только writer. Shutdown Save проходит тот же gate. Формат и checksums не изменены.
Проверено: настоящий VkPipelineCache, contended read-only owner, 1000 запрещённых Save во втором процессе, writer продолжает сохранять; оборванный .tmp сохраняет прежний vulkan.bin; последующая запись восстанавливается; несовместимая simulated driver version отвергается; после Close запись запрещена.
Два отдельных процесса: E:\SecondSonPC_AI\p415a\pairs\20261012_211918\result.txt — PASS, оба exit 0.
Реальная смена GPU/драйвера не выполнялась. Сохранены проверки vendor/device/UUID/driver и checksum; аппаратная миграция между разными GPU остаётся непроверенной.

## R2 — ошибка submit не является completion

Файлы: vulkan/vk_device_context.h/.cpp, vk_executor.h/.cpp, vk_resources.cpp, native_backend.cpp.
У DeviceContext первое fatal VkResult сохраняется атомарно для executor/presenter/timeline/backend.
В in_flight и диагностические submitted события попадают только успешные vkQueueSubmit2. Неотправленная batch помещается в abandoned_ и сохраняет свои ресурсы до teardown; не попадает в free_ и не публикует completion.
После failure Begin и Poll прекращают работу, ожидания timeline возвращают false без ожидания значения неотправленной batch. Перед submit под queue mutex повторно проверяется failure state. Backend завершает работу с кодом 5 и сохраняет реальный VkResult; специальная device-lost диагностика остаётся для DEVICE_LOST.
Fault injection: VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST. Проверены quarantine, отсутствие in-flight/free, нулевой completed_batches, отказ следующего submit, немедленный выход timeline wait. Успешная отправка проверена отдельным контрольным случаем.
Никакие draw/dispatch, barriers, точные shader/pipeline keys, wave mode или guest tracking ради FPS не отключались.

## R1 — swapchain и presentation engine lifetime

Файлы: vulkan/vk_presenter.h/.cpp, vk_device_context.h/.cpp.
Command fence недостаточен для доказательства завершения использования presentation engine. Native presentation теперь требует VK_EXT_swapchain_maintenance1 и feature swapchainMaintenance1. Целевая RTX 2060 поддерживает EXT и KHR варианты; фактически включён EXT. На GPU без EXT/feature startup отказывает с явной причиной; небезопасного fallback нет. Это осознанное ограничение совместимости.
Каждый слот имеет отдельные acquire и present fence и явные pending/submitted flags. WaitSlot проверяет все реально поставленные операции и очищает ссылки Frame на fence под frames_mutex до повторного reset.
VK_TIMEOUT сохраняет объекты и допускает повторное retirement; другие ошибки переводят backend в failure state. Recreate не уничтожает старые ресурсы после неуспешного ожидания. Для результатов present, которые всё равно ставят queue operations, учитывается pending present fence.
Проверяются reset/begin/end/submit; screenshot capture при неуспешном ожидании удерживает buffer/cb/fence. Исходный Frame не публикуется как свободный до окончания capture.
Нет DeviceWaitIdle на каждый кадр. При shutdown ожидаются только поставленные операции; если ограниченное ожидание не завершается и device loss не подтверждён, процесс завершает работу с кодом 5 без уничтожения потенциально используемых объектов.
Fault injection проверяет TIMEOUT, DEVICE_LOST, OUT_OF_HOST_MEMORY при retirement, запрет reset/destroy/submit после неуспешного ожидания, повтор после TIMEOUT, slot reuse, acquire OUT_OF_DATE, resize и нормальное уничтожение.
Ручные alt-tab/fullscreen/minimize/restore с реальной игрой пока не подтверждены.

Основание политики:
- https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
- https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html
- https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html

## Воспроизводимость

Рабочая инфраструктура: E:\SecondSonPC_AI\p415a.
build.ps1 использует E:\SecondSonPC_AI\build\p415a, отдельный install prefix и существующие зависимости без установки новых. TEMP/TMP/TMPDIR, Python/pip/ccache paths задаются только дочернему окружению на E:. Global Windows/NVIDIA settings не менялись.
run.ps1 копирует EXE в уникальный run, создаёт SecondSonPC.toml, явный --settings на E:, userdata и shader/driver cache, сохраняет stdout/stderr, source/binary hashes, git state, peak memory, elapsed/shutdown/exit/forced.
Bridge runtime user/home предварительно создаётся в тестовом каталоге: иначе CreateDefaultUsers показывает диалог миграции даже для пустого профиля. Первый selftest остановился на этом диалоге; диагностический стек сохранён, попытка исключена из успешных тестов.
Пользователь разрешил неконтролируемые служебные записи Windows/драйвера на C:. Абсолютное отсутствие системных записей на C: не заявляется.
Защищённые play.cmd и tools\manual_run.ps1 содержали изменения до этой работы; их не редактировали. E:\SecondSonPC-task2 и игровые файлы не редактировали.

Baseline EXE SHA256:
2F5012F2F8B58E111C1061EDF3963E446FF7DBE6E50F11F936431212E4AE10C7
Candidate1 EXE SHA256:
61480B1BB39E6CECCBA1563E52B3F3B29C1B53C5FB1724F1A0B319C588369D5D

Сборка candidate1: build-logs\fixes2-build.txt — PASS. Первая попытка fixes была отклонена boundary checker из-за неоднозначного namespace Shader в тесте. Исправлено на принятый GS alias, проверка не ослаблена.
Диагностический режим --gpu-stability-test находится в vk_stability_tests.cpp; реальные Vulkan-объекты, подмена таблицы функций только в явно запущенном тесте, восстановление функций до teardown. Validation обязана реально включиться; skip считается провалом теста.
candidate1_stability_user1022_1_20261012_211741614: **60 checks, 0 failures**, Validation enabled, 0 errors.

Полные selftests:
- baseline_selftest_user1022_2_20261012_211210705: 1396 checks, 1 failure.
- candidate1_selftest_user1022_1_20261012_211806932: 1397 checks, 1 failure; новый R4 прошёл.
Одинаковый проблемный случай NativeThreads(stacks): ожидается достижение 1024 slots, но создание thread вернуло 0x80020023 после 998 (baseline) / 987 (candidate) threads. Исходные журналы показывают system commit около предела 32655 MiB. Это согласуется с ресурсным ограничением хоста, но тест не помечается PASS и не переписывается под удобный результат.
Настройки pagefile и чужие процессы не изменялись.

## A/B

План: beach (пляж), early (гавань), user1022 (деревня), три повторения каждой сборки по 90 s. Первое — с пустым application cache, затем два тёплых; новый driver-cache каталог на каждый запуск. Порядок A/B чередуется. Сохранения и действия одинаковые, Parallel CP и Block Upload выключены.
Основная метрика — gpu_frames.csv, а не frametimes.csv bridge: у native backend последний может содержать только header.
Весь прогон и steady window 62–88 s считаются отдельно; 1% Low — обратное среднему медленнейшего 1% интервалов, p95/p99 nearest-rank.
metrics.py создаёт ab-results.json и ab-results.md: FPS, 1% Low, p95/p99, большие кадры, max, CPU/record/GPU busy, compilation, memory, startup/shutdown и fingerprints.
Первый вне серии пляжный прогон пересекался со сборкой; он исключён из контрольного A/B. Прерванные запуски без успешного run.json также исключаются.
Прерванный baseline beach repeat 2 запускался повторно; его кэш мог дополнительно прогреться. Поэтому введён repeat 3 и cold/warm результаты не смешиваются.
Итоговая таблица будет добавлена после завершения серии. Прирост FPS и отсутствие регрессии пока не заявлены.

## Откат

1. Завершить только процессы SecondSonPC, использующие восстанавливаемый каталог, сохранив новые нужные данные.
2. Не применять reset --hard/clean и не распаковывать архив поверх живого проекта.
3. Следовать RESTORE.md исходного бэкапа: восстановить snapshot.zip в новый каталог на E:, проверить все SHA-256 из manifest-sha256.csv.
4. Для выборочного отката R1–R4 сравнить файлы с restore-verification\main, сохранив все изменения владельца после бэкапа. Одной сменой HEAD исходное dirty состояние восстановить нельзя.
5. Raw Git/worktree registrations сохраняют исходные пути; не запускать worktree prune/remove из восстановленной копии. Для работы с историей использовать проверенный bundle в отдельном clone.
6. Проверить защищённые файлы, nested изменения и savedata до переключения запуска.

## Готовность к P4.15B

Пока не рекомендована: не закрыты все критерии P4.15A. После проверок следующая фаза должна восстанавливать точные KCAP metadata для compute; GPL сам по себе compute stalls не решит. Сохранять точные runtime/profile/specialization keys и проверять offline результат против runtime. Эта работа здесь не реализуется.

