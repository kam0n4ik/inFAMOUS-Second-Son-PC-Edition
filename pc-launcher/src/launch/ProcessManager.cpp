#include "launch/ProcessManager.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QWinEventNotifier>
#include <QtConcurrent/QtConcurrentRun>

#include <windows.h>

#include <tlhelp32.h>

#include "profiles/GameProfile.h"
#include "system/FileUtils.h"
#include "system/SystemInfo.h"

namespace Launcher {

namespace {

struct Prepared {
    QString error, details, backupNote;
};

QString Native(const QString& p) {
    return QDir::toNativeSeparators(p);
}

Prepared Prepare(const LaunchSpec& spec, bool ru) {
    Prepared r;
    if (spec.exePath.isEmpty() || !QFileInfo(spec.exePath).isFile()) {
        r.error = ru ? QStringLiteral("Исполняемый файл игры не найден") : QStringLiteral("The game's executable was not found");
        r.details = Native(spec.exePath.isEmpty() && spec.profile ? spec.profile->exeName : spec.exePath);
        return r;
    }
    QStringList missing;
    const QDir game(spec.gameDir);
    if (!QFileInfo(game.filePath(QStringLiteral("eboot.bin"))).isFile()) missing << Native(game.filePath(QStringLiteral("eboot.bin")));
    if (!QFileInfo(game.filePath(QStringLiteral("sce_sys/param.sfo"))).isFile()) missing << Native(game.filePath(QStringLiteral("sce_sys/param.sfo")));
    if (!QFileInfo(game.filePath(QStringLiteral("sce_module"))).isDir()) missing << Native(game.filePath(QStringLiteral("sce_module")));
    if (!missing.isEmpty()) {
        r.error = ru ? QStringLiteral("Не найдены необходимые игровые файлы") : QStringLiteral("Required game files were not found");
        r.details = missing.join(u'\n');
        return r;
    }
    if (System::IsProcessRunning(QFileInfo(spec.exePath).fileName())) {
        r.error = ru ? QStringLiteral("Игра уже запущена") : QStringLiteral("The game is already running");
        r.details = QFileInfo(spec.exePath).fileName();
        return r;
    }
    // the saves first, as the built-in launcher and play.cmd do (launch.cpp BackupSaves): a copy of home
    const QString home = QDir(spec.userDataDir).filePath(QStringLiteral("home"));
    if (spec.backupSaves && !spec.userDataDir.isEmpty() && QFileInfo(home).isDir()) {
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
        const QString dst = QDir(spec.userDataDir).filePath(QStringLiteral("savedata_backups/") + stamp);
        QString error;
        if (!QFileInfo::exists(dst) && !Files::CopyTree(home, dst, &error)) {
            r.backupNote = (ru ? QStringLiteral("Копия сохранений не сделана: %1") : QStringLiteral("The saves were not copied: %1")).arg(error);
        } else {
            r.backupNote = Native(dst);
        }
    }
    return r;
}

struct FindWindowCtx {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"ConsoleWindowClass") == 0 || wcscmp(cls, L"PseudoConsoleWindow") == 0) {
        return TRUE; // the game's console (shader preparation), not the game
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    ctx->found = hwnd;
    return FALSE;
}

} // namespace

ProcessManager::ProcessManager(QObject* parent) : QObject(parent) {
    windowTimer_.setInterval(1000);
    connect(&windowTimer_, &QTimer::timeout, this, &ProcessManager::pollWindow);
}

ProcessManager::~ProcessManager() {
    // the game goes on: only the launcher's handle is closed
    if (notifier_) notifier_->setEnabled(false);
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
}

void ProcessManager::setState(const QString& state, const QString& message, const QString& details) {
    state_ = state;
    message_ = message;
    details_ = details;
    emit stateChanged();
}

QString ProcessManager::QuoteArg(const QString& arg) {
    if (!arg.isEmpty() && !arg.contains(QRegularExpression(QStringLiteral("[\\s\"]")))) return arg;
    // CommandLineToArgvW rules: backslashes before a quote are doubled
    QString out = QStringLiteral("\"");
    int slashes = 0;
    for (const QChar c : arg) {
        if (c == u'\\') {
            ++slashes;
            continue;
        }
        if (c == u'"') {
            out += QString(slashes * 2 + 1, u'\\');
            out += c;
        } else {
            out += QString(slashes, u'\\');
            out += c;
        }
        slashes = 0;
    }
    out += QString(slashes * 2, u'\\');
    return out + u'"';
}

QStringList ProcessManager::BuildEnvironment(const QList<QPair<QString, QString>>& env) {
    QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    for (const auto& [name, value] : env) {
        if (value.isEmpty()) pe.remove(name);
        else pe.insert(name, value);
    }
    QStringList lines;
    for (const auto& key : pe.keys()) {
        lines << key + u'=' + pe.value(key);
    }
    // the block is sorted by name, case-insensitively (CreateProcess documentation)
    std::sort(lines.begin(), lines.end(), [](const QString& a, const QString& b) {
        return a.section(u'=', 0, 0).compare(b.section(u'=', 0, 0), Qt::CaseInsensitive) < 0;
    });
    return lines;
}

void ProcessManager::launch(const LaunchSpec& spec) {
    if (busy()) return;
    setState(QStringLiteral("preparing"), ru_ ? QStringLiteral("Проверка файлов и копия сохранений…") : QStringLiteral("Checking files and copying the saves…"));
    const bool ru = ru_;
    const quint64 gen = ++generation_;
    auto* watcher = new QFutureWatcher<Prepared>(this);
    connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, watcher, spec, gen] {
        const Prepared p = watcher->result();
        watcher->deleteLater();
        if (gen != generation_) return;
        if (!p.error.isEmpty()) {
            setState(QStringLiteral("error"), p.error, p.details);
            emit failed(p.error, p.details);
            return;
        }
        startProcess(spec, p.backupNote);
    });
    watcher->setFuture(QtConcurrent::run([spec, ru] { return Prepare(spec, ru); }));
}

void ProcessManager::startProcess(const LaunchSpec& spec, const QString& backupNote) {
    QString cmd = QuoteArg(QDir::toNativeSeparators(spec.exePath));
    for (const auto& a : spec.args) cmd += u' ' + QuoteArg(a);
    commandLine_ = cmd;
    std::wstring cmdW = cmd.toStdWString();
    std::wstring envBlock;
    for (const auto& line : BuildEnvironment(spec.env)) {
        envBlock += line.toStdWString();
        envBlock.push_back(L'\0');
    }
    envBlock.push_back(L'\0');
    const std::wstring exe = QDir::toNativeSeparators(spec.exePath).toStdWString();
    const std::wstring dir = QDir::toNativeSeparators(spec.workingDir).toStdWString();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdW.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        envBlock.data(), dir.c_str(), &si, &pi)) {
        const DWORD code = GetLastError();
        const QString msg = ru_ ? QStringLiteral("Произошла ошибка запуска. Откройте журнал для подробностей")
                                : QStringLiteral("The game failed to start. Open the log for details");
        const QString details = QStringLiteral("CreateProcessW: %1 (Win32 %2)\n%3").arg(qt_error_string(int(code))).arg(code).arg(cmd);
        setState(QStringLiteral("error"), msg, details);
        emit failed(msg, details);
        return;
    }
    CloseHandle(pi.hThread);
    startedAt_ = QDateTime::currentDateTime();
    watch(pi.hProcess, pi.dwProcessId);
    QString note = ru_ ? QStringLiteral("Процесс создан (PID %1), ждём окно игры…") : QStringLiteral("Process created (PID %1), waiting for the game window…");
    setState(QStringLiteral("starting"), note.arg(pi.dwProcessId),
             backupNote.isEmpty() ? QString() : (ru_ ? QStringLiteral("Копия сохранений: %1") : QStringLiteral("Saves copied to %1")).arg(backupNote));
    emit started(pid_);
}

void ProcessManager::watch(void* process, qint64 pid) {
    process_ = process;
    pid_ = pid;
    windowSeen_ = false;
    windowPolls_ = 0;
    delete notifier_;
    notifier_ = new QWinEventNotifier(static_cast<HANDLE>(process), this);
    connect(notifier_, &QWinEventNotifier::activated, this, &ProcessManager::onExited);
    windowTimer_.start();
}

bool ProcessManager::attachIfRunning(const QString& exePath, const QString& windowTitle) {
    Q_UNUSED(windowTitle);
    if (gameRunning() || exePath.isEmpty()) return false;
    const QString want = QDir::cleanPath(QFileInfo(exePath).absoluteFilePath());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool attached = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !attached; ok = Process32NextW(snap, &pe)) {
        if (QString::fromWCharArray(pe.szExeFile).compare(QFileInfo(exePath).fileName(), Qt::CaseInsensitive) != 0) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h == nullptr) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n) &&
            QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path, int(n)))).compare(want, Qt::CaseInsensitive) == 0) {
            startedAt_ = QDateTime::currentDateTime();
            watch(h, pe.th32ProcessID);
            attached = true;
            setState(QStringLiteral("starting"), ru_ ? QStringLiteral("Игра уже запущена (PID %1)").arg(pe.th32ProcessID)
                                                     : QStringLiteral("The game is already running (PID %1)").arg(pe.th32ProcessID));
            pollWindow();
        } else {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return attached;
}

void ProcessManager::pollWindow() {
    if (!process_) {
        windowTimer_.stop();
        return;
    }
    FindWindowCtx ctx{static_cast<DWORD>(pid_), nullptr};
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found != nullptr) {
        windowSeen_ = true;
        windowTimer_.stop();
        setState(QStringLiteral("running"), ru_ ? QStringLiteral("Игра запущена") : QStringLiteral("The game is running"), details_);
        emit windowShown();
        return;
    }
    // the shader preparation can take minutes before the window: keep looking, more slowly after a minute
    if (++windowPolls_ == 60) windowTimer_.setInterval(3000);
}

void ProcessManager::onExited() {
    if (!process_) return;
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    windowTimer_.stop();
    windowTimer_.setInterval(1000);
    const qint64 seconds = startedAt_.secsTo(QDateTime::currentDateTime());
    const bool early = !windowSeen_ && code != 0;
    if (early) {
        const QString msg = ru_ ? QStringLiteral("Произошла ошибка запуска. Откройте журнал для подробностей")
                                : QStringLiteral("The game failed to start. Open the log for details");
        setState(QStringLiteral("error"), msg,
                 (ru_ ? QStringLiteral("Процесс завершился с кодом 0x%1 до появления окна игры.") : QStringLiteral("The process ended with code 0x%1 before the game window appeared."))
                     .arg(QString::number(code, 16).toUpper()));
    } else {
        setState(QStringLiteral("idle"), {});
    }
    emit finished(int(code), seconds, early);
}

void ProcessManager::acknowledge() {
    if (state_ == QLatin1String("error")) setState(QStringLiteral("idle"), {});
}

} // namespace Launcher
