// inFAMOUS PC Launcher — the application: finds the game (GameDetector), gives the QML interface its profile, theme and
// state, and connects the managers (settings, launch, saves, diagnostics). It knows no game by name: everything that
// differs comes from the chosen GameProfile.
#pragma once

#include <QObject>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QTranslator>
#include <QUrl>
#include <QVariantList>

#include "detection/GameDetector.h"
#include "diagnostics/DiagnosticsManager.h"
#include "launch/ProcessManager.h"
#include "saves/SaveManager.h"
#include "settings/SettingsManager.h"

class QQmlApplicationEngine;
class QQuickWindow;

namespace Launcher {

struct Options {
    QString gameRoot;       // --game-root: look here instead of the launcher's folder
    QString screenshot;     // --screenshot <png>
    QString page = QStringLiteral("home");
    QString design;         // --concept cinematic|neon|desktop
    QString lang;           // --lang ru|en
    QSize size;             // --size WxH (logical)
    bool reduced = false;   // --reduced
    QString dialog;         // --dialog notfound|ambiguous|confirm|log (screenshots of the dialogs)
};

class Application : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY gameChanged)
    Q_PROPERTY(bool hasGame READ hasGame NOTIFY gameChanged)
    Q_PROPERTY(QString detectionReport READ detectionReport NOTIFY gameChanged)
    Q_PROPERTY(QVariantList detectionCandidates READ detectionCandidates NOTIFY gameChanged)
    Q_PROPERTY(QVariantList detectionEvidence READ detectionEvidence NOTIFY gameChanged)
    Q_PROPERTY(QString searchedFolder READ searchedFolder NOTIFY gameChanged)
    Q_PROPERTY(QString profileId READ profileId NOTIFY gameChanged)
    Q_PROPERTY(QString themeId READ themeId NOTIFY gameChanged)
    Q_PROPERTY(QString gameName READ gameName NOTIFY gameChanged)
    Q_PROPERTY(QString shortName READ shortName NOTIFY gameChanged)
    Q_PROPERTY(QString brand READ brand NOTIFY gameChanged)
    Q_PROPERTY(QString productId READ productId NOTIFY gameChanged)
    Q_PROPERTY(QString titleId READ titleId NOTIFY gameChanged)
    Q_PROPERTY(QString gameVersion READ gameVersion NOTIFY gameChanged)
    Q_PROPERTY(QString gameRoot READ gameRoot NOTIFY gameChanged)
    Q_PROPERTY(QString exePath READ exePath NOTIFY gameChanged)
    Q_PROPERTY(QStringList missingFiles READ missingFiles NOTIFY gameChanged)
    Q_PROPERTY(QUrl background READ background NOTIFY gameChanged)
    Q_PROPERTY(QUrl gameIcon READ gameIcon NOTIFY gameChanged)
    Q_PROPERTY(QString launcherVersion READ launcherVersion CONSTANT)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString concept READ design WRITE setDesign NOTIFY designChanged)
    Q_PROPERTY(bool reducedEffects READ reducedEffects WRITE setReducedEffects NOTIFY prefsChanged)
    Q_PROPERTY(bool minimizeOnLaunch READ minimizeOnLaunch WRITE setMinimizeOnLaunch NOTIFY prefsChanged)
    Q_PROPERTY(bool restoreAfterExit READ restoreAfterExit WRITE setRestoreAfterExit NOTIFY prefsChanged)
    Q_PROPERTY(QString lastLaunchText READ lastLaunchText NOTIFY lastLaunchChanged)
    Q_PROPERTY(bool screenshotMode READ screenshotMode CONSTANT)
    Q_PROPERTY(QString initialPage READ initialPage CONSTANT)
    Q_PROPERTY(QString initialDialog READ initialDialog CONSTANT)
    Q_PROPERTY(QSize contentSize READ contentSize CONSTANT)
    Q_PROPERTY(QString graphicsApi READ graphicsApi NOTIFY graphicsApiChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    Application(const Options& options, QObject* parent = nullptr);

    void attach(QQmlApplicationEngine* engine);
    void detect();
    static int WriteDetectReport(const QString& startDir, const QString& file);

    SettingsManager* settings() { return &settings_; }
    ProcessManager* process() { return &process_; }
    SaveManager* saves() { return &saves_; }
    DiagnosticsManager* diagnostics() { return &diagnostics_; }

    QString status() const;
    bool hasGame() const { return game_ != nullptr; }
    QString detectionReport() const { return result_.report(ru()); }
    QVariantList detectionCandidates() const;
    QVariantList detectionEvidence() const;
    QString searchedFolder() const;
    QString profileId() const;
    QString themeId() const;
    QString gameName() const;
    QString shortName() const;
    QString brand() const;
    QString productId() const;
    QString titleId() const;
    QString gameVersion() const;
    QString gameRoot() const;
    QString exePath() const;
    QStringList missingFiles() const;
    QUrl background() const { return background_; }
    QUrl gameIcon() const { return icon_; }
    QString launcherVersion() const;
    QString language() const { return language_; }
    void setLanguage(const QString& lang);
    QString design() const { return design_; }
    void setDesign(const QString& c);
    bool reducedEffects() const { return reduced_; }
    void setReducedEffects(bool on);
    bool minimizeOnLaunch() const { return minimizeOnLaunch_; }
    void setMinimizeOnLaunch(bool on);
    bool restoreAfterExit() const { return restoreAfterExit_; }
    void setRestoreAfterExit(bool on);
    QString lastLaunchText() const;
    bool screenshotMode() const { return !options_.screenshot.isEmpty(); }
    QString initialPage() const { return options_.page; }
    QString initialDialog() const { return options_.dialog; }
    QSize contentSize() const { return options_.size; }
    QString graphicsApi() const { return graphicsApi_; }
    QString notice() const { return notice_; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void chooseGame(const QString& profileId, const QString& root);
    Q_INVOKABLE void redetect();
    Q_INVOKABLE void openGameLog() { diagnostics_.revealLog(); }
    Q_INVOKABLE void takeScreenshot(QObject* item);
    Q_INVOKABLE void clearNotice();
    Q_INVOKABLE QString formatDate(const QDateTime& t) const;

signals:
    void gameChanged();
    void languageChanged();
    void designChanged();
    void prefsChanged();
    void lastLaunchChanged();
    void graphicsApiChanged();
    void noticeChanged();
    void requestMinimize();
    void requestRestore();
    void message(const QString& text, const QString& kind); // a toast: info | ok | error

private:
    bool ru() const { return language_ != QLatin1String("en"); }
    void useCandidate(const Candidate& c);
    void applyLanguage();
    void onGameStarted();
    void onGameFinished(int exitCode, qint64 seconds, bool early);
    void setNotice(const QString& n);

    Options options_;
    QQmlApplicationEngine* engine_ = nullptr;
    GameDetector detector_;
    DetectionResult result_;
    Candidate chosen_;
    const Candidate* game_ = nullptr;
    QUrl background_, icon_;
    QSettings prefs_;
    QTranslator translator_;
    QString language_ = QStringLiteral("ru");
    QString design_ = QStringLiteral("cinematic");
    bool reduced_ = false;
    bool minimizeOnLaunch_ = true;
    bool restoreAfterExit_ = true;
    QString graphicsApi_;
    QString notice_;

    SettingsManager settings_;
    ProcessManager process_;
    SaveManager saves_;
    DiagnosticsManager diagnostics_;
};

} // namespace Launcher
