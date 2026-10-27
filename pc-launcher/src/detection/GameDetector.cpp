#include "detection/GameDetector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <windows.h>

#include "settings/TomlDocument.h"

namespace Launcher {

namespace {

Evidence Ev(const QString& profile, bool positive, const QString& ru, const QString& en) {
    return Evidence{profile, positive, ru, en};
}

/// A path from a .toml: absolute, or relative to the .toml's folder (app\config.cpp resolve()).
QString Resolve(const QString& root, const QString& value) {
    if (value.isEmpty()) {
        return QDir::cleanPath(root);
    }
    const QString native = QDir::fromNativeSeparators(value);
    if (QDir::isAbsolutePath(native)) {
        return QDir::cleanPath(native);
    }
    return QDir::cleanPath(QDir(root).absoluteFilePath(native));
}

} // namespace

QString LauncherDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return QFileInfo(QString::fromStdWString(buffer)).absolutePath();
        }
        buffer.resize(buffer.size() * 2);
    }
}

Candidate GameDetector::inspect(const GameProfile& p, const QString& rootIn) const {
    Candidate c;
    c.profile = &p;
    c.root = QDir::cleanPath(rootIn);
    const QDir root(c.root);

    // 1. the product's configuration file (the runtime finds its root by it: app\paths.cpp RootDir)
    QString gamePath = QStringLiteral(".");
    QString userData = p.defaultUserData, logs = p.defaultLogs, cache = p.defaultShaderCache;
    const QString cfg = root.filePath(p.configFileName);
    if (QFileInfo(cfg).isFile()) {
        c.configFound = true;
        c.configPath = cfg;
        QFile f(cfg);
        TomlDocument doc;
        QString error;
        if (f.open(QIODevice::ReadOnly) && doc.parse(f.read(4 * 1024 * 1024), &error)) {
            c.configValid = true;
            gamePath = doc.value(QStringLiteral("Game"), QStringLiteral("path"), gamePath).toString();
            userData = doc.value(QStringLiteral("Paths"), QStringLiteral("userData"), userData).toString();
            logs = doc.value(QStringLiteral("Paths"), QStringLiteral("logs"), logs).toString();
            cache = doc.value(QStringLiteral("ShaderCache"), QStringLiteral("dir"), cache).toString();
            c.evidence << Ev(p.id, true, QStringLiteral("Найден файл конфигурации %1").arg(QDir::toNativeSeparators(cfg)),
                             QStringLiteral("Configuration file found: %1").arg(QDir::toNativeSeparators(cfg)));
        } else {
            c.configError = error.isEmpty() ? f.errorString() : error;
            c.evidence << Ev(p.id, false,
                             QStringLiteral("%1 не читается: %2").arg(p.configFileName, c.configError),
                             QStringLiteral("%1 cannot be read: %2").arg(p.configFileName, c.configError));
        }
    }
    c.gameDir = Resolve(c.root, gamePath);
    c.userDataDir = Resolve(c.root, userData);
    c.logsDir = Resolve(c.root, logs);
    c.shaderCacheDir = Resolve(c.root, cache);

    // 2. the product's executable (a player's install: next to the .toml; a build: build\bin)
    for (const auto& dir : p.exeSearchDirs) {
        const QString exe = QDir::cleanPath(root.filePath(dir + u'/' + p.exeName));
        if (QFileInfo(exe).isFile()) {
            c.exeFound = true;
            c.exePath = exe;
            c.evidence << Ev(p.id, true, QStringLiteral("Найден исполняемый файл %1").arg(QDir::toNativeSeparators(exe)),
                             QStringLiteral("Executable found: %1").arg(QDir::toNativeSeparators(exe)));
            break;
        }
    }

    // 3. the game's own files and its title id
    const QDir game(c.gameDir);
    const QString sfoPath = game.filePath(QStringLiteral("sce_sys/param.sfo"));
    if (QFileInfo(sfoPath).isFile()) {
        c.sfo = ParamSfo::load(sfoPath);
        const QString tid = c.sfo.titleId();
        if (!c.sfo.valid) {
            c.evidence << Ev(p.id, false, QStringLiteral("param.sfo повреждён: %1").arg(c.sfo.error),
                             QStringLiteral("param.sfo is damaged: %1").arg(c.sfo.error));
        } else if (p.titleIds.contains(tid)) {
            c.titleConfirmed = true;
            c.evidence << Ev(p.id, true,
                             QStringLiteral("Title ID %1 в %2 — %3").arg(tid, QDir::toNativeSeparators(sfoPath), p.gameName),
                             QStringLiteral("Title ID %1 in %2 is %3").arg(tid, QDir::toNativeSeparators(sfoPath), p.gameName));
        } else {
            for (const auto& other : profiles_) {
                if (other.titleIds.contains(tid)) {
                    c.foreignTitle = tid;
                }
            }
            c.evidence << Ev(p.id, false,
                             QStringLiteral("Title ID %1 в %2 — не %3").arg(tid, QDir::toNativeSeparators(sfoPath), p.gameName),
                             QStringLiteral("Title ID %1 in %2 is not %3").arg(tid, QDir::toNativeSeparators(sfoPath), p.gameName));
        }
    } else if (c.configFound || c.exeFound) {
        c.evidence << Ev(p.id, false,
                         QStringLiteral("Нет %1 (папка игры из [Game] path)").arg(QDir::toNativeSeparators(sfoPath)),
                         QStringLiteral("No %1 (the game folder of [Game] path)").arg(QDir::toNativeSeparators(sfoPath)));
    }
    if (c.titleConfirmed || c.configFound || c.exeFound) {
        if (!QFileInfo(game.filePath(QStringLiteral("eboot.bin"))).isFile()) {
            c.missingFiles << QStringLiteral("eboot.bin");
        }
        if (!QFileInfo(sfoPath).isFile()) {
            c.missingFiles << QStringLiteral("sce_sys\\param.sfo");
        }
        if (!QFileInfo(game.filePath(QStringLiteral("sce_module"))).isDir()) {
            c.missingFiles << QStringLiteral("sce_module\\");
        }
        for (const auto& m : c.missingFiles) {
            c.evidence << Ev(p.id, false,
                             QStringLiteral("Нет файла игры %1 в %2").arg(m, QDir::toNativeSeparators(c.gameDir)),
                             QStringLiteral("Game file %1 is missing in %2").arg(m, QDir::toNativeSeparators(c.gameDir)));
        }
    }
    return c;
}

DetectionResult GameDetector::detect(const QString& startDirIn, int levelsUp) const {
    DetectionResult r;
    r.startDir = QDir::cleanPath(QFileInfo(startDirIn).absoluteFilePath());
    QString dir = r.startDir;
    for (int level = 0; level <= levelsUp && !dir.isEmpty(); ++level) {
        r.searchedDirs << dir;
        QList<Candidate> present;
        for (const auto& p : profiles_) {
            Candidate c = inspect(p, dir);
            r.evidence << c.evidence;
            if (c.configFound || c.exeFound || c.titleConfirmed) {
                present << c;
            }
        }
        // the game's own files of a title no profile knows: noted, never chosen
        const ParamSfo bare = ParamSfo::load(QDir(dir).filePath(QStringLiteral("sce_sys/param.sfo")));
        if (bare.valid) {
            bool known = false;
            for (const auto& p : profiles_) {
                known = known || p.titleIds.contains(bare.titleId());
            }
            if (!known) {
                r.evidence << Ev({}, false,
                                 QStringLiteral("В %1 игра с Title ID %2 — не поддерживается").arg(QDir::toNativeSeparators(dir), bare.titleId()),
                                 QStringLiteral("%1 holds a game with Title ID %2 - not supported").arg(QDir::toNativeSeparators(dir), bare.titleId()));
            }
        }
        if (!present.isEmpty()) {
            const bool conflict = std::any_of(present.begin(), present.end(),
                                              [](const Candidate& c) { return !c.foreignTitle.isEmpty(); });
            r.candidates = present;
            if (present.size() > 1 || conflict) {
                r.status = DetectionResult::Status::Ambiguous;
                r.evidence << Ev({}, false,
                                 QStringLiteral("В %1 есть признаки нескольких игр или они противоречат друг другу").arg(QDir::toNativeSeparators(dir)),
                                 QStringLiteral("%1 has signs of more than one game, or signs that contradict each other").arg(QDir::toNativeSeparators(dir)));
                return r;
            }
            r.chosen = 0;
            r.status = present.front().complete() ? DetectionResult::Status::Detected
                                                  : DetectionResult::Status::Incomplete;
            return r;
        }
        const QString parent = QFileInfo(dir).dir().absolutePath();
        if (parent == dir || QDir(dir).isRoot()) {
            break;
        }
        dir = QDir::cleanPath(parent);
    }
    r.status = DetectionResult::Status::NotFound;
    return r;
}

QString DetectionResult::report(bool ru) const {
    QStringList out;
    const char* names[] = {"Detected", "Incomplete", "Ambiguous", "NotFound"};
    out << (ru ? QStringLiteral("Результат: %1") : QStringLiteral("Result: %1")).arg(QLatin1String(names[int(status)]));
    out << (ru ? QStringLiteral("Папка лаунчера: %1") : QStringLiteral("Launcher folder: %1")).arg(QDir::toNativeSeparators(startDir));
    for (const auto& d : searchedDirs) {
        out << (ru ? QStringLiteral("Проверено: %1") : QStringLiteral("Checked: %1")).arg(QDir::toNativeSeparators(d));
    }
    for (const auto& e : evidence) {
        out << QStringLiteral("%1 %2").arg(e.positive ? QStringLiteral("[+]") : QStringLiteral("[-]"), ru ? e.textRu : e.textEn);
    }
    return out.join(u'\n');
}

} // namespace Launcher
