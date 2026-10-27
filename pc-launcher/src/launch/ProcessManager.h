// inFAMOUS PC Launcher — starting the game and following its process: the same start as the game's built-in launcher
// (core\src\launcher\launch.cpp StartGame: "<exe>" --no-launcher, its own console, the launch profile's variables,
// the saves copied to userdata\savedata_backups first), then the process watched without polling its state: a wait
// on its handle (QWinEventNotifier). "Running" is claimed only once the game's own window exists. Closing the launcher
// never ends the game.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTimer>

class QWinEventNotifier;

namespace Launcher {

struct GameProfile;

struct LaunchSpec {
    const GameProfile* profile = nullptr;
    QString exePath;
    QString workingDir;  // the root (the folder of the product's .toml), as play.cmd
    QString gameDir;     // eboot.bin, sce_sys, sce_module
    QString userDataDir; // saves: <userData>\home -> <userData>\savedata_backups\<stamp>
    QStringList args;
    QList<QPair<QString, QString>> env;
    bool backupSaves = true;
};

class ProcessManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged) // idle preparing starting running error
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QString details READ details NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool gameRunning READ gameRunning NOTIFY stateChanged)
    Q_PROPERTY(qint64 pid READ pid NOTIFY stateChanged)
    Q_PROPERTY(QString commandLine READ commandLine NOTIFY stateChanged)

public:
    explicit ProcessManager(QObject* parent = nullptr);
    ~ProcessManager() override;

    void setRussian(bool ru) { ru_ = ru; }
    QString state() const { return state_; }
    QString message() const { return message_; }
    QString details() const { return details_; }
    bool busy() const { return state_ == QLatin1String("preparing") || state_ == QLatin1String("starting") || gameRunning(); }
    bool gameRunning() const { return process_ != nullptr; }
    qint64 pid() const { return pid_; }
    QString commandLine() const { return commandLine_; }

    /// Checks, copies the saves (in a worker thread) and starts the game. The result arrives as signals.
    void launch(const LaunchSpec& spec);
    /// Follows a copy of this exe that is already running (started before the launcher, or by it before a restart).
    bool attachIfRunning(const QString& exePath, const QString& windowTitle);
    Q_INVOKABLE void acknowledge(); // the error / exit notice was read

    /// The environment block's text for a spec (tests): the launcher's environment with the spec's variables set.
    static QStringList BuildEnvironment(const QList<QPair<QString, QString>>& env);
    static QString QuoteArg(const QString& arg);

signals:
    void stateChanged();
    void started(qint64 pid);
    void windowShown();
    /// `crashedEarly`: the process ended before its window appeared and with an error code.
    void finished(int exitCode, qint64 seconds, bool crashedEarly);
    void failed(const QString& message, const QString& details);

private:
    void setState(const QString& state, const QString& message, const QString& details = {});
    void startProcess(const LaunchSpec& spec, const QString& backupNote);
    void watch(void* process, qint64 pid);
    void onExited();
    void pollWindow();

    QString state_ = QStringLiteral("idle");
    QString message_, details_, commandLine_;
    void* process_ = nullptr; // HANDLE
    qint64 pid_ = 0;
    QWinEventNotifier* notifier_ = nullptr;
    QTimer windowTimer_;
    int windowPolls_ = 0;
    bool windowSeen_ = false;
    QDateTime startedAt_;
    bool ru_ = true;
    quint64 generation_ = 0;
};

} // namespace Launcher
