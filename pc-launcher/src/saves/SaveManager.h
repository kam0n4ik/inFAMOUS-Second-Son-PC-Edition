// inFAMOUS PC Launcher — the game's saves where the runtime keeps them: <userData>\home\<user>\savedata\<TitleID>\<dir>
// (sce_sys\param.sfo of each: the title, the game's own subtitle and the time it was written) and the copies in
// <userData>\savedata_backups\<stamp> (the same layout as home: the built-in launcher's and play.cmd's copies).
// Only this game's Title ID is listed, copied or restored; nothing is ever deleted except a folder this manager
// itself replaced during a restore (after it was copied to a new backup).
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace Launcher {

struct GameProfile;

struct SaveEntry {
    QString user, dir, path, title, subtitle, detail, iconPath;
    qint64 bytes = 0;
    qint64 modifiedMs = 0;
};

struct BackupEntry {
    QString name, path;
    qint64 bytes = 0;
    qint64 createdMs = 0;
    int saves = 0;        // save folders of this game in it
    bool hasTitle = false;
};

class SaveManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList saves READ saves NOTIFY changed)
    Q_PROPERTY(QVariantList backups READ backups NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString savesRoot READ savesRoot NOTIFY changed)
    Q_PROPERTY(QString backupsRoot READ backupsRoot NOTIFY changed)
    Q_PROPERTY(QString totalSize READ totalSize NOTIFY changed)
    Q_PROPERTY(bool locked READ locked NOTIFY busyChanged)

public:
    explicit SaveManager(QObject* parent = nullptr);

    void setGame(const GameProfile* profile, const QString& userDataDir);
    void setRussian(bool ru) { ru_ = ru; }
    void setLocked(bool locked); // the game is running: no restore

    QVariantList saves() const;
    QVariantList backups() const;
    bool busy() const { return busy_; }
    bool locked() const { return locked_; }
    QString savesRoot() const;
    QString backupsRoot() const;
    QString totalSize() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void backupNow();
    /// Restores this game's saves from a backup folder: a new backup of the current state first, then for each user
    /// <home>\<user>\savedata\<TitleID> is replaced as one rename. Other titles and the trophies are not touched.
    Q_INVOKABLE void restore(const QString& backupPath);
    Q_INVOKABLE bool reveal(const QString& path) const;

    // synchronous forms (the worker's and the tests')
    static QList<SaveEntry> ScanSaves(const QString& userDataDir, const QStringList& titleIds);
    static QList<BackupEntry> ScanBackups(const QString& userDataDir, const QStringList& titleIds);
    static bool Backup(const QString& userDataDir, const QString& suffix, QString* createdPath, QString* error);
    static bool Restore(const QString& userDataDir, const QStringList& titleIds, const QString& backupPath,
                        QString* safetyBackup, QString* error);

signals:
    void changed();
    void busyChanged();
    void operationFinished(bool ok, const QString& message);

private:
    void setBusy(bool b);

    const GameProfile* profile_ = nullptr;
    QString userData_;
    QList<SaveEntry> saves_;
    QList<BackupEntry> backups_;
    bool busy_ = false;
    bool locked_ = false;
    bool ru_ = true;
    quint64 generation_ = 0;
};

} // namespace Launcher
