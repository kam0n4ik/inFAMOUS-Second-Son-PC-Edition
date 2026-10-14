# Зависимости нативной сборки

| Что | Откуда | Лицензия | Время |
|---|---|---|---|
| Win32 (user32, shell32, kernel32) | Windows | система | импорт |
| MSVC runtime (msvcp140, vcruntime140, ucrt) | Visual C++ Redistributable | Microsoft | импорт |
| vulkan-1.dll | драйвер / Vulkan Runtime | система | LoadLibrary |
| xinput1_4.dll | Windows | система | LoadLibrary |
| dbghelp.dll | Windows | система | LoadLibrary (только при падении) |
| Заголовки Vulkan, glslc | LunarG Vulkan SDK (`E:\VulkanSDK`) | Apache-2.0 / MIT | только сборка |
| Windows SDK 10.0.26100 | NuGet-пакеты (`E:\SecondSonPC\toolchain\winsdk`) | Microsoft | только сборка |
| MSVC 14.44, CMake, Ninja | `E:\BuildTools` | Microsoft / BSD / Apache | только сборка |

Сторонних библиотек в коде нет. Из эталонного проекта не используется ни один файл (граница проверяется CMake).
Игровые данные (`.xpps`) не входят в программу — путь задаёт пользователь (`[Game] DataDir`).

Проверка: `sspc_depaudit SecondSonPC.exe --modules <список из --modules>` — отчёт с PASS / NOT PROVEN / FAIL.
Последний прогон (мир a1, валидация): все пункты PASS; несистемные DLL в процессе — слой валидации (по запросу)
и хуки OBS (внедряются OBS).
