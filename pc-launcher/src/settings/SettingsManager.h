// inFAMOUS PC Launcher — the game's PC settings: read from the runtime's own files, shown, edited, checked and written
// back to the user file the runtime reads (%LOCALAPPDATA%\<product>\settings.toml), in its format, keeping every
// line the launcher does not change. docs\SETTINGS_INTEGRATION.md.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include "settings/SettingsCatalog.h"
#include "settings/TomlDocument.h"
#include "system/SystemInfo.h"

namespace Launcher {

struct GameProfile;

class SettingsManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sections READ sections NOTIFY structureChanged)
    Q_PROPERTY(int revision READ revision NOTIFY valuesChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY valuesChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY valuesChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY editableChanged)
    Q_PROPERTY(QString lockReason READ lockReason NOTIFY editableChanged)
    Q_PROPERTY(QString userFile READ userFile NOTIFY structureChanged)
    Q_PROPERTY(QString productFile READ productFile NOTIFY structureChanged)
    Q_PROPERTY(QString fileProblem READ fileProblem NOTIFY valuesChanged)
    Q_PROPERTY(QString launchProfile READ launchProfile WRITE setLaunchProfile NOTIFY launchProfileChanged)
    Q_PROPERTY(QString launcherStateFile READ launcherStateFile NOTIFY structureChanged)

public:
    explicit SettingsManager(QObject* parent = nullptr);

    /// The game: its profile, the product's .toml (the defaults) and the folder of its executable (upscaler DLLs).
    void setGame(const GameProfile* profile, const QString& productToml, const QString& exeDir,
                 const QList<GpuInfo>& gpus, const QList<DisplayInfo>& displays);
    void setRussian(bool ru);
    void setLocked(bool locked, const QString& reason);

    /// %LOCALAPPDATA% as the runtime reads it (GetEnvironmentVariableW("LOCALAPPDATA")).
    static QString LocalAppData();
    static QString UserFileFor(const QString& productId);

    QVariantList sections() const { return sections_; }
    int revision() const { return revision_; }
    bool dirty() const { return !draft_.isEmpty(); }
    int pendingCount() const { return static_cast<int>(draft_.size()); }
    bool editable() const { return !locked_; }
    QString lockReason() const { return lockReason_; }
    QString userFile() const { return userFile_; }
    QString productFile() const { return productFile_; }
    QString fileProblem() const { return fileProblem_; }
    QString launchProfile() const { return launchProfile_; }
    void setLaunchProfile(const QString& profile);
    QString launcherStateFile() const;

    Q_INVOKABLE QVariant value(const QString& id) const;
    Q_INVOKABLE void setValue(const QString& id, const QVariant& value);
    /// Matters right now (its "requires" setting is on, it is not overridden by another one).
    Q_INVOKABLE bool isActive(const QString& id) const;
    Q_INVOKABLE QString inactiveReason(const QString& id) const;    // another setting turns it off
    Q_INVOKABLE QString unavailableReason(const QString& id) const; // not for this game / this PC
    Q_INVOKABLE bool isModified(const QString& id) const; // differs from the game's default
    Q_INVOKABLE bool isPending(const QString& id) const;  // changed here, not yet saved
    Q_INVOKABLE QString fileIssue(const QString& id) const; // the user file's value was not usable
    Q_INVOKABLE QString valueText(const QString& id) const;

    Q_INVOKABLE bool save();
    Q_INVOKABLE void revert();
    Q_INVOKABLE void resetSection(const QString& sectionId);
    Q_INVOKABLE void reload();
    /// Unknown keys / bad values of the user file, for the diagnostics page ("" = none).
    Q_INVOKABLE QString checkUserFile() const;
    Q_INVOKABLE QString lastError() const { return lastError_; }

    /// The environment the game is started with: the launch profile's variables (launch.cpp kStandard /
    /// kExperimental), as the built-in launcher sets them. The user file's [Experiments] override them in the runtime.
    QList<QPair<QString, QString>> launchEnvironment() const;

    /// Values for tests and the report.
    QVariant savedValue(const QString& id) const;
    QVariant defaultValue(const QString& id) const;
    quint32 sysCaps() const { return sysCaps_; }

signals:
    void structureChanged();
    void valuesChanged();
    void editableChanged();
    void launchProfileChanged();
    void saved(bool ok, const QString& message);

private:
    struct Virtual; // composite rows
    void loadFiles();
    void buildSections();
    QVariant rawSaved(const SettingDef& d, QString* issue) const;
    QVariant fileDefault(const SettingDef& d) const;
    QVariant current(const QString& id) const;
    void setRaw(const QString& id, const QVariant& v);
    QString availability(const SettingDef& d) const;
    QString choiceReason(const ChoiceDef& c) const;
    QVariantMap rowFor(const SettingDef& d) const;
    QString label(const SettingDef& d) const { return ru_ ? d.ru : d.en; }

    const GameProfile* profile_ = nullptr;
    QString productFile_, userFile_, exeDir_;
    TomlDocument defaults_, user_;
    bool userFileOk_ = true;
    QString fileProblem_, lastError_;
    QHash<QString, QVariant> draft_; // id -> the TOML value to write
    QList<GpuInfo> gpus_;
    QList<DisplayInfo> displays_;
    quint32 sysCaps_ = 0;
    bool ru_ = true;
    bool locked_ = false;
    QString lockReason_;
    QString launchProfile_ = QStringLiteral("standard");
    QVariantList sections_;
    int revision_ = 0;
};

} // namespace Launcher
