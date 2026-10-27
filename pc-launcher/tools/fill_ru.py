# inFAMOUS PC Launcher - fills translations/launcher_ru.ts with the Russian text of every qsTr() string.
# Run after "cmake --build <build> --target update_translations"; strings missing here stay untranslated and are listed.
import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape, unescape

RU = {
    "%1 in %2 generation(s)": "%1 в поколениях: %2",
    "%1 · Title ID %2 · %3": "%1 · Title ID %2 · %3",
    "%1 · no saves of this game": "%1 · сохранений этой игры нет",
    "%1 · saves: %2 · %3": "%1 · сохранений: %2 · %3",
    "A copy is made before every start (as the game's own launcher does) and before every restore. Old copies are never deleted automatically.":
        "Копия делается перед каждым запуском (как во встроенном лаунчере игры) и перед каждым восстановлением. Старые копии никогда не удаляются автоматически.",
    "Back up now": "Создать копию",
    "Backups": "Резервные копии",
    "Bring the launcher back after the game": "Возвращать лаунчер после игры",
    "Cancel": "Отмена",
    "Change in Settings → Experimental": "Меняется в Настройки → Экспериментальные",
    "Changed": "Изменено",
    "Check the configuration": "Проверить конфигурацию",
    "Check the game files": "Проверить файлы игры",
    "Cinematic": "Cinematic",
    "Clear": "Очистить",
    "Clear the shader cache": "Очистить кэш шейдеров",
    "Clear the shader cache?": "Очистить кэш шейдеров?",
    "Close": "Закрыть",
    "Configuration": "Конфигурация",
    "Copied.": "Скопировано.",
    "Copy": "Копировать",
    "Copy report": "Копировать отчёт",
    "Copy the details": "Скопировать подробности",
    "Could not find the files of a supported game. Put the launcher in the Second Son PC Edition or First Light PC Edition folder.":
        "Не удалось обнаружить файлы поддерживаемой игры. Поместите лаунчер в папку Second Son PC Edition или First Light PC Edition",
    "Counting…": "Подсчёт…",
    "Defaults": "По умолчанию",
    "Design": "Оформление",
    "Diagnostics": "Диагностика",
    "Dismiss": "Скрыть",
    "Experimental": "Экспериментальный",
    "Game files": "Файлы игры",
    "Game folder": "Папка игры",
    "Game is running": "Игра запущена",
    "Game log": "Журнал игры",
    "Game saves": "Сохранения игры",
    "Home": "Главная",
    "In the file: %1 (the default is used)": "В файле: %1 (используется значение по умолчанию)",
    "Interface": "Интерфейс",
    "Last launch": "Последний запуск",
    "Launch profile": "Профиль запуска",
    "Launcher": "Лаунчер",
    "Launcher %1 · %2": "Лаунчер %1 · %2",
    "Launcher language": "Язык лаунчера",
    "Launcher log": "Журнал лаунчера",
    "Minimize the launcher": "Сворачивать лаунчер",
    "Modern Desktop": "Modern Desktop",
    "Neither SecondSonPC.toml / SecondSonPC.exe nor FirstLightPC.toml / FirstLightPC.exe, nor a game's sce_sys/param.sfo, is in this folder or the two above it.":
        "Ни SecondSonPC.toml / SecondSonPC.exe, ни FirstLightPC.toml / FirstLightPC.exe, ни sce_sys/param.sfo игры нет ни в этой папке, ни в двух папках выше.",
    "Neon Control": "Neon Control",
    "No backups yet.": "Резервных копий пока нет.",
    "No saves yet. They appear here after the game saves for the first time.": "Сохранений пока нет. Они появятся здесь после первого сохранения в игре.",
    "No smoke, particles, glow or glass; transitions without animation. Less work for the GPU.":
        "Без дыма, частиц, свечения и стекла; переходы без анимации. Меньше нагрузки на видеокарту.",
    "No supported game found": "Поддерживаемая игра не найдена",
    "Not saved": "Не сохранено",
    "Not verified": "Не проверено",
    "OK": "ОК",
    "Only the cache's own generation folders in %1 are removed (%2). The next start prepares the shaders again, which takes a few minutes. While the game runs, the cache is cleared at its next start instead.":
        "Удаляются только папки поколений самого кэша в %1 (%2). При следующем запуске шейдеры будут подготовлены заново — это займёт несколько минут. Если игра запущена, кэш очистится при её следующем запуске.",
    "Only what the launcher could establish; everything else is shown as unknown.": "Только то, что лаунчер смог установить; остальное показано как «Не определено».",
    "Open the cache folder": "Открыть папку кэша",
    "Open the folder": "Открыть папку",
    "Open the log": "Открыть журнал",
    "Open the logs folder": "Открыть папку журналов",
    "PC Edition · version %1 · %2": "PC Edition · версия %1 · %2",
    "Play": "Играть",
    "Preparing…": "Подготовка…",
    "Profile": "Профиль",
    "Quick actions": "Быстрые действия",
    "Reduce visual effects": "Уменьшить визуальные эффекты",
    "Refresh": "Обновить",
    "Restore": "Восстановить",
    "Restore the saves of %1?": "Восстановить сохранения от %1?",
    "Revert": "Отменить",
    "Save": "Сохранить",
    "Saves": "Сохранения",
    "Saves folder": "Папка сохранений",
    "Search again": "Искать снова",
    "Searched from: %1": "Поиск от папки: %1",
    "Settings": "Настройки",
    "Settings folder": "Папка настроек",
    "Shader cache": "Кэш шейдеров",
    "Show the game log": "Показать журнал игры",
    "Some of the game's files are missing: %1": "Не хватает части файлов игры: %1",
    "Standard": "Стандартный",
    "Starting…": "Запуск…",
    "Switches at once. The game's own language is in Settings → Game.": "Переключается сразу. Язык самой игры — в Настройки → Игра.",
    "The current saves of %1 are copied to a new backup first, then replaced by this copy. Only this game's saves (Title ID %2) change; the copy itself stays.":
        "Сначала текущие сохранения %1 копируются в новую резервную копию, затем заменяются этой копией. Меняются только сохранения этой игры (Title ID %2); сама копия остаётся.",
    "The folder %1 has signs of more than one supported game, or signs that contradict each other. Choose the game to use; nothing is changed in the folder.":
        "В папке %1 есть признаки нескольких поддерживаемых игр или признаки, противоречащие друг другу. Выберите игру; в папке ничего не меняется.",
    "The game is running: saves can be viewed and backed up, not restored.": "Игра запущена: сохранения можно смотреть и копировать, но не восстанавливать.",
    "The launcher stops its animations and stays out of the way; closing it never closes the game.":
        "Лаунчер останавливает анимации и не мешает; закрытие лаунчера никогда не закрывает игру.",
    "The launcher's own preferences (they do not change the game)": "Параметры самого лаунчера (на игру не влияют)",
    "The line under a save's name is the game's own description of it.": "Строка под названием сохранения — описание из самой игры.",
    "The report is in the clipboard.": "Отчёт скопирован в буфер обмена.",
    "Three layouts of the same launcher: Cinematic (the art in full), Neon Control (a side rail and tiles), Modern Desktop (a sidebar and cards).":
        "Три раскладки одного лаунчера: Cinematic (арт на весь экран), Neon Control (боковая панель и плитки), Modern Desktop (сайдбар и карточки).",
    "Tools": "Инструменты",
    "Unavailable": "Недоступно",
    "Unsaved changes: %1. The game reads them at its next start.": "Несохранённых изменений: %1. Игра прочитает их при следующем запуске.",
    "Use %1": "Выбрать %1",
    "Version %1": "Версия %1",
    "Version %1 · %2": "Версия %1 · %2",
    "When the game ends, the launcher's window returns (always after a failed start).": "Когда игра закрывается, окно лаунчера возвращается (после неудачного запуска — всегда).",
    "Which game is this?": "Какая это игра?",
    "While the game runs": "Пока игра запущена",
    "Written to %1 — the file the game reads; unknown keys and comments are kept": "Записывается в %1 — файл, который читает игра; неизвестные ключи и комментарии сохраняются",
    "as play.cmd": "как play.cmd",
    "counting…": "подсчёт…",
}
PLURAL = {
    "%n copies": ["%n копия", "%n копии", "%n копий"],
    "%n generation(s)": ["%n поколение", "%n поколения", "%n поколений"],
}

ts = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "translations" / "launcher_ru.ts")
text = ts.read_text(encoding="utf-8")
missing = []


def fill(m):
    block = m.group(0)
    src = unescape(re.search(r"<source>(.*?)</source>", block, re.S).group(1), {"&apos;": "'", "&quot;": '"'})
    if 'numerus="yes"' in block:
        forms = PLURAL.get(src)
        if not forms:
            missing.append(src)
            return block
        tr = "<translation>\n" + "".join(f"            <numerusform>{escape(f)}</numerusform>\n" for f in forms) + "        </translation>"
    else:
        if src not in RU:
            missing.append(src)
            return block
        tr = f"<translation>{escape(RU[src])}</translation>"
    return re.sub(r"<translation[^>]*?(/>|>.*?</translation>)", tr, block, flags=re.S)


text = re.sub(r"<message[^>]*>.*?</message>", fill, text, flags=re.S)
ts.write_text(text, encoding="utf-8", newline="\n")
print(f"{ts}: untranslated {len(missing)}")
for s in missing:
    print("  " + s)
sys.exit(1 if missing else 0)
