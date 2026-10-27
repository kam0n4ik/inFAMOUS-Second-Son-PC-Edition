#include "saves/SaveManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <windows.h>

#include "detection/ParamSfo.h"
#include "profiles/GameProfile.h"
#include "system/FileUtils.h"

namespace Launcher {

namespace {

QString Native(const QString& p) {
    return QDir::toNativeSeparators(p);
}

qint64 StampMs(const QString& name, qint64 fallback) {
    // 20261027_141152 (the built-in launcher, play.cmd), 2026-10-10_03-44-58_... (older scripts)
    static const QRegularExpression a(QStringLiteral("^(\\d{8})_(\\d{6})"));
    static const QRegularExpression b(QStringLiteral("^(\\d{4}-\\d{2}-\\d{2})_(\\d{2}-\\d{2}-\\d{2})"));
    QDateTime t;
    if (const auto m = a.match(name); m.hasMatch()) {
        t = QDateTime::fromString(m.captured(1) + m.captured(2), QStringLiteral("yyyyMMddHHmmss"));
    } else if (const auto m2 = b.match(name); m2.hasMatch()) {
        t = QDateTime::fromString(m2.captured(1) + u'_' + m2.captured(2), QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    }
    return t.isValid() ? t.toMSecsSinceEpoch() : fallback;
}

QString UniqueDir(const QString& base) {
    QString p = base;
    for (int i = 2; QFileInfo::exists(p); ++i) p = base + QStringLiteral("_%1").arg(i);
    return p;
}

} // namespace

SaveManager::SaveManager(QObject* parent) : QObject(parent) {}

void SaveManager::setGame(const GameProfile* profile, const QString& userDataDir) {
    profile_ = profile;
    userData_ = userDataDir;
    saves_.clear();
    backups_.clear();
    emit changed();
}

void SaveManager::setLocked(bool locked) {
    locked_ = locked;
    emit busyChanged();
}

void SaveManager::setBusy(bool b) {
    busy_ = b;
    emit busyChanged();
}

QString SaveManager::savesRoot() const {
    return userData_.isEmpty() ? QString() : Native(QDir(userData_).filePath(QStringLiteral("home")));
}

QString SaveManager::backupsRoot() const {
    return userData_.isEmpty() ? QString() : Native(QDir(userData_).filePath(QStringLiteral("savedata_backups")));
}

QString SaveManager::totalSize() const {
    qint64 b = 0;
    for (const auto& s : saves_) b += s.bytes;
    return Files::HumanSize(b, ru_);
}

QList<SaveEntry> SaveManager::ScanSaves(const QString& userDataDir, const QStringList& titleIds) {
    QList<SaveEntry> out;
    const QDir home(QDir(userDataDir).filePath(QStringLiteral("home")));
    for (const QFileInfo& user : home.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (user.fileName() == QLatin1String("trophies")) continue;
        for (const auto& tid : titleIds) {
            const QDir titleDir(user.absoluteFilePath() + QStringLiteral("/savedata/") + tid);
            for (const QFileInfo& dir : titleDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                SaveEntry e;
                e.user = user.fileName();
                e.dir = dir.fileName();
                e.path = dir.absoluteFilePath();
                const ParamSfo sfo = ParamSfo::load(dir.absoluteFilePath() + QStringLiteral("/sce_sys/param.sfo"));
                if (sfo.valid) {
                    e.title = sfo.text(QStringLiteral("MAINTITLE"));
                    e.subtitle = sfo.text(QStringLiteral("SUBTITLE"));
                    e.detail = sfo.text(QStringLiteral("DETAIL"));
                }
                const QString icon = dir.absoluteFilePath() + QStringLiteral("/sce_sys/icon0.png");
                if (QFileInfo(icon).isFile()) e.iconPath = icon;
                const auto stats = Files::Measure(dir.absoluteFilePath());
                e.bytes = stats.bytes;
                e.modifiedMs = stats.newestMs;
                out << e;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.modifiedMs > b.modifiedMs; });
    return out;
}

QList<BackupEntry> SaveManager::ScanBackups(const QString& userDataDir, const QStringList& titleIds) {
    QList<BackupEntry> out;
    const QDir root(QDir(userDataDir).filePath(QStringLiteral("savedata_backups")));
    for (const QFileInfo& b : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (Files::IsReparsePoint(b.absoluteFilePath())) continue;
        BackupEntry e;
        e.name = b.fileName();
        e.path = b.absoluteFilePath();
        e.createdMs = StampMs(e.name, b.lastModified().toMSecsSinceEpoch());
        const QDir dir(e.path);
        for (const QFileInfo& user : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const auto& tid : titleIds) {
                const QDir t(user.absoluteFilePath() + QStringLiteral("/savedata/") + tid);
                if (t.exists()) {
                    const int n = static_cast<int>(t.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
                    e.saves += n;
                    e.hasTitle = e.hasTitle || n > 0;
                    e.bytes += Files::Measure(t.absolutePath()).bytes;
                }
            }
        }
        out << e;
    }
    std::sort(out.begin(), out.end(), [](const BackupEntry& a, const BackupEntry& b) { return a.createdMs > b.createdMs; });
    return out;
}

bool SaveManager::Backup(const QString& userDataDir, const QString& suffix, QString* createdPath, QString* error) {
    const QString home = QDir(userDataDir).filePath(QStringLiteral("home"));
    if (!QFileInfo(home).isDir()) {
        if (error) *error = QStringLiteral("no saves: %1").arg(Native(home));
        return false;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString dst = UniqueDir(QDir(userDataDir).filePath(QStringLiteral("savedata_backups/") + stamp + suffix));
    if (!Files::CopyTree(home, dst, error)) return false;
    if (createdPath) *createdPath = dst;
    return true;
}

bool SaveManager::Restore(const QString& userDataDir, const QStringList& titleIds, const QString& backupPath,
                          QString* safetyBackup, QString* error) {
    const QString backupsRoot = QDir(userDataDir).filePath(QStringLiteral("savedata_backups"));
    if (!Files::IsInside(backupsRoot, backupPath) || QDir::cleanPath(backupPath).compare(QDir::cleanPath(backupsRoot), Qt::CaseInsensitive) == 0) {
        if (error) *error = QStringLiteral("not a backup of this game's folder: %1").arg(Native(backupPath));
        return false;
    }
    // what the backup holds of this game
    struct Item { QString user, title, from; };
    QList<Item> items;
    for (const QFileInfo& user : QDir(backupPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const auto& tid : titleIds) {
            const QString from = user.absoluteFilePath() + QStringLiteral("/savedata/") + tid;
            if (QFileInfo(from).isDir() && !QDir(from).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                items << Item{user.fileName(), tid, from};
            }
        }
    }
    if (items.isEmpty()) {
        if (error) *error = QStringLiteral("the backup has no saves of %1").arg(titleIds.join(u'/'));
        return false;
    }
    // 1. the current state is copied first (it can be restored the same way)
    const QString home = QDir(userDataDir).filePath(QStringLiteral("home"));
    if (QFileInfo(home).isDir()) {
        if (!Backup(userDataDir, QStringLiteral("_before_restore"), safetyBackup, error)) return false;
    }
    // 2. each title folder: a copy next to it, then two renames
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    for (const auto& it : items) {
        const QString live = home + u'/' + it.user + QStringLiteral("/savedata/") + it.title;
        const QString incoming = UniqueDir(live + QStringLiteral(".restore-") + stamp);
        const QString old = UniqueDir(live + QStringLiteral(".replaced-") + stamp);
        if (!Files::CopyTree(it.from, incoming, error)) {
            QString ignore;
            Files::RemoveTreeInside(home, incoming, &ignore);
            return false;
        }
        const bool hadLive = QFileInfo(live).isDir();
        if (hadLive && !MoveFileExW(Native(live).toStdWString().c_str(), Native(old).toStdWString().c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (error) *error = QStringLiteral("cannot move %1 aside (Win32 %2): is the game running?").arg(Native(live)).arg(GetLastError());
            QString ignore;
            Files::RemoveTreeInside(home, incoming, &ignore);
            return false;
        }
        if (!MoveFileExW(Native(incoming).toStdWString().c_str(), Native(live).toStdWString().c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (error) *error = QStringLiteral("cannot put the restored folder in place (Win32 %1)").arg(GetLastError());
            if (hadLive) MoveFileExW(Native(old).toStdWString().c_str(), Native(live).toStdWString().c_str(), MOVEFILE_WRITE_THROUGH);
            return false;
        }
        // the replaced folder is in the safety backup of step 1
        if (hadLive) {
            QString ignore;
            Files::RemoveTreeInside(home, old, &ignore);
        }
    }
    return true;
}

void SaveManager::refresh() {
    if (profile_ == nullptr || userData_.isEmpty()) return;
    setBusy(true);
    const quint64 gen = ++generation_;
    const QString ud = userData_;
    const QStringList tids = profile_->titleIds;
    using Result = QPair<QList<SaveEntry>, QList<BackupEntry>>;
    auto* w = new QFutureWatcher<Result>(this);
    connect(w, &QFutureWatcher<Result>::finished, this, [this, w, gen] {
        w->deleteLater();
        if (gen != generation_) return;
        saves_ = w->result().first;
        backups_ = w->result().second;
        setBusy(false);
        emit changed();
    });
    w->setFuture(QtConcurrent::run([ud, tids] { return Result(ScanSaves(ud, tids), ScanBackups(ud, tids)); }));
}

void SaveManager::backupNow() {
    if (profile_ == nullptr || busy_) return;
    setBusy(true);
    const QString ud = userData_;
    const bool ru = ru_;
    auto* w = new QFutureWatcher<QPair<bool, QString>>(this);
    connect(w, &QFutureWatcher<QPair<bool, QString>>::finished, this, [this, w] {
        w->deleteLater();
        setBusy(false);
        emit operationFinished(w->result().first, w->result().second);
        refresh();
    });
    w->setFuture(QtConcurrent::run([ud, ru] {
        QString created, error;
        const bool ok = Backup(ud, QStringLiteral("_launcher"), &created, &error);
        return qMakePair(ok, ok ? (ru ? QStringLiteral("Копия создана: %1") : QStringLiteral("Backup created: %1")).arg(Native(created))
                                : (ru ? QStringLiteral("Копия не создана: %1") : QStringLiteral("No backup was made: %1")).arg(error));
    }));
}

void SaveManager::restore(const QString& backupPath) {
    if (profile_ == nullptr || busy_) return;
    if (locked_) {
        emit operationFinished(false, ru_ ? QStringLiteral("Закройте игру перед восстановлением сохранений.")
                                          : QStringLiteral("Close the game before restoring saves."));
        return;
    }
    setBusy(true);
    const QString ud = userData_;
    const QStringList tids = profile_->titleIds;
    const bool ru = ru_;
    auto* w = new QFutureWatcher<QPair<bool, QString>>(this);
    connect(w, &QFutureWatcher<QPair<bool, QString>>::finished, this, [this, w] {
        w->deleteLater();
        setBusy(false);
        emit operationFinished(w->result().first, w->result().second);
        refresh();
    });
    w->setFuture(QtConcurrent::run([ud, tids, backupPath, ru] {
        QString safety, error;
        const bool ok = Restore(ud, tids, QDir::fromNativeSeparators(backupPath), &safety, &error);
        return qMakePair(ok, ok ? (ru ? QStringLiteral("Сохранения восстановлены. Прежние скопированы в %1") : QStringLiteral("Saves restored. The previous ones are in %1")).arg(Native(safety))
                                : (ru ? QStringLiteral("Восстановление не выполнено: %1") : QStringLiteral("Nothing was restored: %1")).arg(error));
    }));
}

bool SaveManager::reveal(const QString& path) const {
    return Files::Reveal(QDir::fromNativeSeparators(path));
}

QVariantList SaveManager::saves() const {
    QVariantList list;
    for (const auto& s : saves_) {
        list << QVariantMap{{QStringLiteral("user"), s.user}, {QStringLiteral("dir"), s.dir}, {QStringLiteral("path"), Native(s.path)},
                            {QStringLiteral("title"), s.title}, {QStringLiteral("subtitle"), s.subtitle}, {QStringLiteral("detail"), s.detail},
                            {QStringLiteral("icon"), s.iconPath.isEmpty() ? QString() : QUrl::fromLocalFile(s.iconPath).toString()},
                            {QStringLiteral("size"), Files::HumanSize(s.bytes, ru_)},
                            {QStringLiteral("modified"), s.modifiedMs > 0 ? QDateTime::fromMSecsSinceEpoch(s.modifiedMs) : QDateTime()}};
    }
    return list;
}

QVariantList SaveManager::backups() const {
    QVariantList list;
    for (const auto& b : backups_) {
        list << QVariantMap{{QStringLiteral("name"), b.name}, {QStringLiteral("path"), Native(b.path)},
                            {QStringLiteral("size"), Files::HumanSize(b.bytes, ru_)}, {QStringLiteral("saves"), b.saves},
                            {QStringLiteral("hasTitle"), b.hasTitle},
                            {QStringLiteral("created"), QDateTime::fromMSecsSinceEpoch(b.createdMs)}};
    }
    return list;
}

} // namespace Launcher
