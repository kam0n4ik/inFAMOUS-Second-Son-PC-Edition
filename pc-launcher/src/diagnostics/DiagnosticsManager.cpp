#include "diagnostics/DiagnosticsManager.h"

#include <windows.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

#include "profiles/GameProfile.h"
#include "settings/SettingsManager.h"
#include "system/FileUtils.h"
#include "system/Log.h"

namespace Launcher {

namespace {

QString Native(const QString& p) {
    return QDir::toNativeSeparators(p);
}

QVariantMap Item(const QString& group, const QString& label, const QString& value, const QString& state = QStringLiteral("ok"),
                 const QString& path = {}) {
    return QVariantMap{{QStringLiteral("group"), group}, {QStringLiteral("label"), label}, {QStringLiteral("value"), value},
                       {QStringLiteral("state"), state}, {QStringLiteral("path"), path}};
}

// the runtime holds <generation>\lock open without sharing (delete on close) while it writes there: a lock that can be
// opened is a stale one left by a crash, and the generation is free (the runtime itself treats it so)
bool HasLock(const QString& dir) {
    const std::wstring path = QDir::toNativeSeparators(dir + QStringLiteral("/lock")).toStdWString();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(h);
    return false;
}

} // namespace

DiagnosticsManager::DiagnosticsManager(QObject* parent) : QObject(parent) {}

void DiagnosticsManager::setGame(const Candidate* game, SettingsManager* settings) {
    game_ = game;
    settings_ = settings;
    cacheKnown_ = false;
    log_ = {};
    rebuild();
}

void DiagnosticsManager::setRussian(bool ru) {
    ru_ = ru;
    rebuild();
    emit cacheChanged();
}

QString DiagnosticsManager::cacheDir() const {
    if (game_ == nullptr || game_->profile == nullptr) return {};
    return Native(QDir(game_->shaderCacheDir).filePath(game_->profile->titleIds.value(0)));
}

QString DiagnosticsManager::cacheSize() const {
    return cacheKnown_ ? Files::HumanSize(cache_.bytes, ru_) : QString();
}

QString DiagnosticsManager::logPath() const {
    if (game_ == nullptr || game_->profile == nullptr) return {};
    return Native(QDir(game_->logsDir).filePath(game_->profile->logFileName));
}

QString DiagnosticsManager::launcherLogPath() const {
    return Native(Log::Path());
}

QStringList DiagnosticsManager::CacheGenerations(const QString& cacheBase, const QStringList& titleIds, int* inUse) {
    QStringList out;
    if (inUse) *inUse = 0;
    for (const auto& tid : titleIds) {
        const QString game = QDir(cacheBase).filePath(tid);
        // <game>\<generation> with its `generation` marker (the bridge's), <game>\native|native_own\<generation> with a
        // cache file (the native backend's) — exactly what the runtime itself removes
        const auto scan = [&](const QString& dir, const QStringList& markers) {
            for (const QFileInfo& g : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (Files::IsReparsePoint(g.absoluteFilePath())) continue;
                bool marked = false;
                for (const auto& m : markers) marked = marked || QFileInfo(g.absoluteFilePath() + u'/' + m).isFile();
                if (!marked) continue;
                if (HasLock(g.absoluteFilePath())) {
                    if (inUse) ++*inUse;
                    continue;
                }
                out << g.absoluteFilePath();
            }
        };
        scan(game, {QStringLiteral("generation")});
        scan(game + QStringLiteral("/native"), {QStringLiteral("shaders.pack"), QStringLiteral("vulkan.bin")});
        scan(game + QStringLiteral("/native_own"), {QStringLiteral("shaders.pack"), QStringLiteral("vulkan.bin")});
    }
    return out;
}

CacheInfo DiagnosticsManager::MeasureCache(const QString& cacheBase, const QStringList& titleIds) {
    CacheInfo c;
    for (const auto& tid : titleIds) {
        c.root = QDir(cacheBase).filePath(tid);
        const auto s = Files::Measure(c.root);
        c.bytes += s.bytes;
        c.files += s.files;
    }
    int inUse = 0;
    c.generations = static_cast<int>(CacheGenerations(cacheBase, titleIds, &inUse).size()) + inUse;
    c.inUse = inUse > 0;
    return c;
}

LogInfo DiagnosticsManager::ReadLog(const QString& path) {
    LogInfo info;
    info.path = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return info;
    info.bytes = f.size();
    // the head only: the session line and the device line come in the first seconds (a log can be 40 MB)
    const QByteArray head = f.read(1024 * 1024);
    const QString text = QString::fromUtf8(head);
    static const QRegularExpression session(QStringLiteral("session started (\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})"));
    if (const auto m = session.match(text); m.hasMatch()) info.sessionStarted = m.captured(1);
    static const QRegularExpression device(QStringLiteral("Vulkan device ([^\\r\\n]+)"));
    if (const auto m = device.match(text); m.hasMatch()) info.vulkanDevice = m.captured(1).trimmed();
    return info;
}

void DiagnosticsManager::refresh() {
    if (loading_) return;
    loading_ = true;
    emit loadingChanged();
    const bool needSystem = !systemLoaded_;
    const QString base = game_ ? game_->shaderCacheDir : QString();
    const QStringList tids = game_ && game_->profile ? game_->profile->titleIds : QStringList();
    const QString log = logPath();
    struct Result {
        SystemInfo system;
        VulkanInfo vulkan;
        QList<DisplayInfo> displays;
        CacheInfo cache;
        LogInfo log;
    };
    const quint64 gen = ++generation_;
    auto* w = new QFutureWatcher<Result>(this);
    connect(w, &QFutureWatcher<Result>::finished, this, [this, w, needSystem, gen] {
        w->deleteLater();
        if (gen != generation_) return;
        const Result r = w->result();
        if (needSystem) {
            system_ = r.system;
            vulkan_ = r.vulkan;
            displays_ = r.displays;
            systemLoaded_ = true;
        }
        cache_ = r.cache;
        cacheKnown_ = true;
        log_ = r.log;
        loading_ = false;
        emit loadingChanged();
        emit cacheChanged();
        rebuild();
    });
    w->setFuture(QtConcurrent::run([needSystem, base, tids, log] {
        Result r;
        if (needSystem) {
            r.system = System::Query();
            r.vulkan = System::QueryVulkan();
            r.displays = System::Displays();
        }
        if (!base.isEmpty()) r.cache = MeasureCache(base, tids);
        if (!log.isEmpty()) r.log = ReadLog(QDir::fromNativeSeparators(log));
        return r;
    }));
}

void DiagnosticsManager::refreshCache() {
    if (game_ == nullptr) return;
    const QString base = game_->shaderCacheDir;
    const QStringList tids = game_->profile->titleIds;
    auto* w = new QFutureWatcher<CacheInfo>(this);
    connect(w, &QFutureWatcher<CacheInfo>::finished, this, [this, w] {
        w->deleteLater();
        cache_ = w->result();
        cacheKnown_ = true;
        emit cacheChanged();
        rebuild();
    });
    w->setFuture(QtConcurrent::run([base, tids] { return MeasureCache(base, tids); }));
}

void DiagnosticsManager::rebuild() {
    items_.clear();
    const QString unknown = ru_ ? QStringLiteral("Не определено") : QStringLiteral("Unknown");
    const auto T = [this](const char* ru, const char* en) { return ru_ ? QString::fromUtf8(ru) : QString::fromUtf8(en); };
    const QString gGame = T("Игра", "Game"), gRun = T("PC Runtime", "PC runtime"), gSys = T("Компьютер", "Computer"),
                  gCache = T("Кэш шейдеров", "Shader cache"), gLauncher = T("Лаунчер", "Launcher");
    if (game_ && game_->profile) {
        const GameProfile& p = *game_->profile;
        items_ << Item(gGame, T("Игра", "Game"), p.gameName + QStringLiteral(" — PC Edition"));
        items_ << Item(gGame, QStringLiteral("Product ID"), p.productId);
        const QString tid = game_->sfo.titleId();
        items_ << Item(gGame, QStringLiteral("Title ID"),
                       tid.isEmpty() ? unknown : tid + (game_->titleConfirmed ? T(" (подтверждён param.sfo)", " (confirmed by param.sfo)") : QString()),
                       tid.isEmpty() ? QStringLiteral("unknown") : (game_->titleConfirmed ? QStringLiteral("ok") : QStringLiteral("warn")));
        const QString appVer = game_->sfo.text(QStringLiteral("APP_VER"));
        const QString ver = game_->sfo.text(QStringLiteral("VERSION"));
        items_ << Item(gGame, T("Версия игры", "Game version"),
                       appVer.isEmpty() ? unknown : QStringLiteral("%1 (APP_VER), %2 (VERSION)").arg(appVer, ver.isEmpty() ? unknown : ver),
                       appVer.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("ok"));
        const QString content = game_->sfo.text(QStringLiteral("CONTENT_ID"));
        items_ << Item(gGame, QStringLiteral("Content ID"), content.isEmpty() ? unknown : content, content.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("ok"));
        items_ << Item(gGame, T("Папка игры", "Game folder"), Native(game_->gameDir), game_->gameFilesOk() ? QStringLiteral("ok") : QStringLiteral("warn"), game_->gameDir);
        items_ << Item(gGame, T("Папка PC Edition", "PC Edition folder"), Native(game_->root), QStringLiteral("ok"), game_->root);
        items_ << Item(gGame, T("Исполняемый файл", "Executable"), game_->exeFound ? Native(game_->exePath) : T("Не найден", "Not found"),
                       game_->exeFound ? QStringLiteral("ok") : QStringLiteral("error"), game_->exePath);

        QString runtimeVersion = unknown;
        QString build;
        if (game_->exeFound) {
            const QFileInfo exe(game_->exePath);
            build = QLocale(ru_ ? QLocale::Russian : QLocale::English).toString(exe.lastModified(), QLocale::ShortFormat) +
                    QStringLiteral(", ") + Files::HumanSize(exe.size(), ru_);
        }
        items_ << Item(gRun, T("Версия PC Runtime", "PC runtime version"), runtimeVersion + T(" — в exe нет собственного номера версии (его VERSIONINFO 0.19.1 — от shadPS4)",
                                                                                            ": the exe carries no version of its own (its VERSIONINFO 0.19.1 is shadPS4's)"),
                       QStringLiteral("unknown"));
        items_ << Item(gRun, T("Сборка exe", "Exe build"), build.isEmpty() ? unknown : build, build.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("ok"));
        items_ << Item(gRun, T("Конфигурация", "Configuration"), game_->configFound ? Native(game_->configPath) : T("Не найдена", "Not found"),
                       game_->configValid ? QStringLiteral("ok") : QStringLiteral("error"), game_->configPath);
        if (settings_) {
            const bool exists = QFileInfo::exists(settings_->userFile());
            items_ << Item(gRun, T("Настройки пользователя", "User settings"),
                           Native(settings_->userFile()) + (exists ? QString() : T(" (ещё нет)", " (none yet)")),
                           settings_->fileProblem().isEmpty() ? QStringLiteral("ok") : QStringLiteral("error"), settings_->userFile());
            items_ << Item(gRun, T("Профиль запуска", "Launch profile"),
                           settings_->launchProfile() == QLatin1String("experimental") ? T("Экспериментальный", "Experimental") : T("Стандартный", "Standard"));
        }
        const bool logExists = QFileInfo::exists(QDir::fromNativeSeparators(logPath()));
        items_ << Item(gRun, T("Журнал игры", "Game log"),
                       logExists ? QStringLiteral("%1 (%2)").arg(logPath(), Files::HumanSize(log_.bytes, ru_)) : T("Ещё не создан", "Not created yet"),
                       logExists ? QStringLiteral("ok") : QStringLiteral("unknown"), QDir::fromNativeSeparators(logPath()));
        items_ << Item(gRun, T("Последний сеанс игры", "Last game session"), log_.sessionStarted.isEmpty() ? unknown : log_.sessionStarted,
                       log_.sessionStarted.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("ok"));
        items_ << Item(gRun, T("Активное устройство (по журналу)", "Active device (from the log)"),
                       log_.vulkanDevice.isEmpty() ? unknown : log_.vulkanDevice, log_.vulkanDevice.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("ok"));
    }
    if (systemLoaded_) {
        items_ << Item(gSys, QStringLiteral("Windows"), system_.windows.isEmpty() ? unknown : system_.windows);
        items_ << Item(gSys, T("Процессор", "CPU"), system_.cpu.isEmpty() ? unknown : QStringLiteral("%1 (%2 %3)").arg(system_.cpu).arg(system_.logicalCores).arg(T("потоков", "threads")));
        items_ << Item(gSys, T("Оперативная память", "Memory"), system_.ramBytes == 0 ? unknown : Files::HumanSize(qint64(system_.ramBytes), ru_));
        if (system_.gpus.isEmpty()) items_ << Item(gSys, T("Видеокарта", "GPU"), unknown, QStringLiteral("unknown"));
        for (const auto& g : system_.gpus) {
            items_ << Item(gSys, T("Видеокарта", "GPU"),
                           QStringLiteral("%1 (%2 VRAM, %3:%4)").arg(g.name, Files::HumanSize(qint64(g.dedicatedVideoMemory), ru_))
                               .arg(g.vendorId, 4, 16, QLatin1Char('0')).arg(g.deviceId, 4, 16, QLatin1Char('0')));
        }
        if (!vulkan_.loaderFound) {
            items_ << Item(gSys, QStringLiteral("Vulkan"), T("vulkan-1.dll не найден — игре нужен драйвер с Vulkan", "vulkan-1.dll not found: the game needs a Vulkan driver"), QStringLiteral("error"));
        } else {
            items_ << Item(gSys, QStringLiteral("Vulkan"), QStringLiteral("%1 %2").arg(T("загрузчик", "loader"), vulkan_.apiVersion.isEmpty() ? unknown : vulkan_.apiVersion),
                           vulkan_.error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("warn"));
            for (const auto& d : vulkan_.devices) items_ << Item(gSys, T("Устройство Vulkan", "Vulkan device"), d);
        }
        for (const auto& m : displays_) {
            items_ << Item(gSys, T("Монитор", "Display"), QStringLiteral("%1 — %2 × %3, %4 %5%6").arg(m.name).arg(m.width).arg(m.height).arg(m.refreshHz)
                                                             .arg(T("Гц", "Hz"), m.primary ? T(", основной", ", primary") : QString()));
        }
    } else {
        items_ << Item(gSys, T("Компьютер", "Computer"), T("Сбор сведений…", "Collecting…"), QStringLiteral("unknown"));
    }
    if (game_) {
        items_ << Item(gCache, T("Папка", "Folder"), cacheDir(), QFileInfo::exists(QDir::fromNativeSeparators(cacheDir())) ? QStringLiteral("ok") : QStringLiteral("unknown"),
                       QDir::fromNativeSeparators(cacheDir()));
        items_ << Item(gCache, T("Размер", "Size"),
                       cacheKnown_ ? QStringLiteral("%1 (%2 %3)").arg(Files::HumanSize(cache_.bytes, ru_)).arg(cache_.files).arg(T("файлов", "files")) : T("Подсчёт…", "Counting…"),
                       cacheKnown_ ? QStringLiteral("ok") : QStringLiteral("unknown"));
        items_ << Item(gCache, T("Поколения кэша", "Cache generations"),
                       cacheKnown_ ? QString::number(cache_.generations) + (cache_.inUse ? T(" (часть используется игрой)", " (some in use by the game)") : QString()) : unknown,
                       cacheKnown_ ? QStringLiteral("ok") : QStringLiteral("unknown"));
        items_ << Item(gCache, T("Число конвейеров", "Pipeline count"), unknown + T(" — хранится внутри shaders.pack, надёжно не читается снаружи", ": kept inside shaders.pack, not reliably readable from outside"),
                       QStringLiteral("unknown"));
    }
    items_ << Item(gLauncher, T("Версия лаунчера", "Launcher version"), QCoreApplication::applicationVersion());
    items_ << Item(gLauncher, QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    items_ << Item(gLauncher, T("Журнал лаунчера", "Launcher log"), launcherLogPath(), QStringLiteral("ok"), Log::Path());
    emit changed();
}

QString DiagnosticsManager::report() const {
    QString out = QStringLiteral("inFAMOUS PC Launcher %1 — %2\n").arg(QCoreApplication::applicationVersion(),
                                                                    QDateTime::currentDateTime().toString(Qt::ISODate));
    QString group;
    for (const auto& v : items_) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("group")).toString() != group) {
            group = m.value(QStringLiteral("group")).toString();
            out += QStringLiteral("\n[%1]\n").arg(group);
        }
        out += QStringLiteral("%1: %2\n").arg(m.value(QStringLiteral("label")).toString(), m.value(QStringLiteral("value")).toString());
    }
    if (settings_) {
        const QString check = settings_->checkUserFile();
        if (!check.isEmpty()) out += QStringLiteral("\n[%1]\n%2\n").arg(ru_ ? QStringLiteral("Проверка настроек") : QStringLiteral("Settings check"), check);
    }
    return out;
}

void DiagnosticsManager::copyReport() const {
    QGuiApplication::clipboard()->setText(report());
}

void DiagnosticsManager::copyText(const QString& text) const {
    QGuiApplication::clipboard()->setText(text);
}

QString DiagnosticsManager::logTail(int lines) const {
    return Files::Tail(QDir::fromNativeSeparators(logPath()), 256 * 1024, lines);
}

QString DiagnosticsManager::launcherLogTail(int lines) const {
    return Files::Tail(Log::Path(), 128 * 1024, lines);
}

bool DiagnosticsManager::revealLog() const {
    const QString p = QDir::fromNativeSeparators(logPath());
    return QFileInfo::exists(p) ? Files::Reveal(p) : (game_ && Files::Reveal(game_->logsDir));
}

bool DiagnosticsManager::revealGameFolder() const {
    return game_ && Files::Reveal(game_->root);
}

bool DiagnosticsManager::revealConfigFolder() const {
    if (settings_ == nullptr) return false;
    const QString user = settings_->userFile();
    if (QFileInfo::exists(user)) return Files::Reveal(user);
    return game_ && Files::Reveal(game_->configPath);
}

bool DiagnosticsManager::revealCache() const {
    const QString dir = QDir::fromNativeSeparators(cacheDir());
    if (QFileInfo(dir).isDir()) return Files::Reveal(dir);
    return game_ && Files::Reveal(game_->shaderCacheDir);
}

QString DiagnosticsManager::checkFiles() const {
    if (game_ == nullptr || game_->profile == nullptr) return {};
    const GameDetector detector(AllProfiles());
    const Candidate c = detector.inspect(*game_->profile, game_->root);
    QStringList out;
    const auto line = [&](bool ok, const QString& text) { out << QStringLiteral("%1 %2").arg(ok ? QStringLiteral("[OK]") : QStringLiteral("[!!]"), text); };
    line(c.configFound && c.configValid, ru_ ? QStringLiteral("Конфигурация %1").arg(Native(c.configPath.isEmpty() ? game_->profile->configFileName : c.configPath))
                                            : QStringLiteral("Configuration %1").arg(Native(c.configPath.isEmpty() ? game_->profile->configFileName : c.configPath)));
    line(c.exeFound, ru_ ? QStringLiteral("Исполняемый файл %1").arg(c.exeFound ? Native(c.exePath) : game_->profile->exeName)
                         : QStringLiteral("Executable %1").arg(c.exeFound ? Native(c.exePath) : game_->profile->exeName));
    line(c.titleConfirmed, QStringLiteral("param.sfo TITLE_ID %1").arg(c.sfo.titleId().isEmpty() ? QStringLiteral("—") : c.sfo.titleId()));
    for (const char* f : {"eboot.bin", "sce_sys/param.sfo", "sce_module"}) {
        const QString path = QDir(c.gameDir).filePath(QLatin1String(f));
        line(QFileInfo::exists(path), Native(path));
    }
    // the runtime's DLLs next to the exe (upscalers; their absence only disables those settings)
    if (c.exeFound) {
        const QDir exeDir = QFileInfo(c.exePath).absoluteDir();
        for (const char* dll : {"amd_fidelityfx_vk.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "NvLowLatencyVk.dll", "sspc_dlss.dll"}) {
            const bool ok = QFileInfo(exeDir.filePath(QLatin1String(dll))).isFile();
            out << QStringLiteral("%1 %2").arg(ok ? QStringLiteral("[OK]") : QStringLiteral("[--]"), Native(exeDir.filePath(QLatin1String(dll))));
        }
    }
    return out.join(u'\n');
}

QString DiagnosticsManager::checkConfig() const {
    if (settings_ == nullptr) return {};
    QStringList out;
    out << (ru_ ? QStringLiteral("Файл по умолчанию: %1") : QStringLiteral("Defaults: %1")).arg(Native(settings_->productFile()));
    out << (ru_ ? QStringLiteral("Файл пользователя: %1") : QStringLiteral("User file: %1")).arg(Native(settings_->userFile()));
    const QString check = settings_->checkUserFile();
    out << (check.isEmpty() ? (ru_ ? QStringLiteral("Замечаний нет: все ключи известны рантайму, значения допустимы.")
                                   : QStringLiteral("No remarks: every key is known to the runtime and every value is valid."))
                            : check);
    return out.join(u'\n');
}

QString DiagnosticsManager::clearShaderCache() {
    if (game_ == nullptr || game_->profile == nullptr) return {};
    const QString base = game_->shaderCacheDir;
    if (gameRunning_) {
        // the runtime's own request, applied at its next start before the cache is opened (native_backend.cpp)
        QDir().mkpath(base);
        QFile f(QDir(base).filePath(QStringLiteral("clear_shaders.request")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("requested in inFAMOUS PC Launcher\n");
            return ru_ ? QStringLiteral("Игра запущена — кэш будет очищен при её следующем запуске.")
                       : QStringLiteral("The game is running: the cache is cleared at its next start.");
        }
        return ru_ ? QStringLiteral("Не удалось записать запрос очистки: %1").arg(f.errorString())
                   : QStringLiteral("Could not write the clear request: %1").arg(f.errorString());
    }
    int inUse = 0;
    const QStringList gens = CacheGenerations(base, game_->profile->titleIds, &inUse);
    int removed = 0;
    QStringList errors;
    for (const auto& g : gens) {
        QString error;
        // strictly inside <cache>\<TitleID>: never the cache root, never through a link
        const QString titleRoot = QDir(base).filePath(game_->profile->titleIds.value(0));
        if (Files::RemoveTreeInside(titleRoot, g, &error)) ++removed;
        else errors << error;
    }
    Log::Info(QStringLiteral("shader cache: %1 generation(s) removed under %2%3").arg(removed).arg(Native(base), errors.isEmpty() ? QString() : QStringLiteral(" (errors: ") + errors.join(QStringLiteral("; ")) + u')'));
    refreshCache();
    QString msg = ru_ ? QStringLiteral("Удалено поколений кэша: %1.").arg(removed) : QStringLiteral("Cache generations removed: %1.").arg(removed);
    if (inUse > 0) msg += ru_ ? QStringLiteral(" Используемые игрой оставлены.") : QStringLiteral(" Those in use were left.");
    if (!errors.isEmpty()) msg += u' ' + errors.first();
    return msg;
}

} // namespace Launcher
