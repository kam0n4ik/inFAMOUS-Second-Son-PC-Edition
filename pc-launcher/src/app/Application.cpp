#include "app/Application.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include "profiles/GameProfile.h"
#include "system/FileUtils.h"
#include "system/Log.h"

namespace Launcher {

namespace {

QString Native(const QString& p) {
    return QDir::toNativeSeparators(p);
}

QSettings::Format IniFormat() {
    return QSettings::IniFormat;
}

} // namespace

Application::Application(const Options& options, QObject* parent)
    : QObject(parent), options_(options), detector_(AllProfiles()),
      prefs_(Log::Dir() + QStringLiteral("/launcher.ini"), IniFormat()) {
    language_ = prefs_.value(QStringLiteral("ui/language"), QStringLiteral("ru")).toString() == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("ru");
    design_ = prefs_.value(QStringLiteral("ui/concept"), QStringLiteral("cinematic")).toString();
    if (design_ != QLatin1String("neon") && design_ != QLatin1String("desktop")) design_ = QStringLiteral("cinematic");
    reduced_ = prefs_.value(QStringLiteral("ui/reducedEffects"), false).toBool();
    minimizeOnLaunch_ = prefs_.value(QStringLiteral("launch/minimize"), true).toBool();
    restoreAfterExit_ = prefs_.value(QStringLiteral("launch/restoreAfterExit"), true).toBool();
    // the screenshot options win and are not remembered
    if (!options_.lang.isEmpty()) language_ = options_.lang == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("ru");
    if (!options_.design.isEmpty()) design_ = options_.design;
    if (options_.reduced) reduced_ = true;

    connect(&process_, &ProcessManager::started, this, [this] { onGameStarted(); });
    connect(&process_, &ProcessManager::finished, this, &Application::onGameFinished);
    connect(&process_, &ProcessManager::failed, this, [this](const QString& msg, const QString& details) {
        Log::Error(QStringLiteral("launch failed: %1 (%2)").arg(msg, details));
    });
    connect(&process_, &ProcessManager::windowShown, this, [this] {
        Log::Info(QStringLiteral("game window shown (PID %1)").arg(process_.pid()));
    });
    connect(&saves_, &SaveManager::operationFinished, this, [this](bool ok, const QString& msg) {
        Log::Info(QStringLiteral("saves: %1 %2").arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"), msg));
        emit message(msg, ok ? QStringLiteral("ok") : QStringLiteral("error"));
    });
    connect(&settings_, &SettingsManager::saved, this, [this](bool ok, const QString& msg) {
        if (ok && msg.isEmpty()) return;
        Log::Info(QStringLiteral("settings: %1 %2").arg(ok ? QStringLiteral("written to") : QStringLiteral("not written:"), msg));
        emit message(ok ? (ru() ? QStringLiteral("Настройки сохранены: %1") : QStringLiteral("Settings saved: %1")).arg(msg) : msg,
                     ok ? QStringLiteral("ok") : QStringLiteral("error"));
    });
}

void Application::attach(QQmlApplicationEngine* engine) {
    engine_ = engine;
    applyLanguage();
    connect(engine, &QQmlApplicationEngine::objectCreated, this, [this](QObject* obj) {
        auto* window = qobject_cast<QQuickWindow*>(obj);
        if (!window) return;
        connect(window, &QQuickWindow::sceneGraphInitialized, this, [this, window] {
            const auto api = window->rendererInterface()->graphicsApi();
            graphicsApi_ = api == QSGRendererInterface::Direct3D11 ? QStringLiteral("Direct3D 11")
                         : api == QSGRendererInterface::Vulkan ? QStringLiteral("Vulkan")
                         : api == QSGRendererInterface::OpenGL ? QStringLiteral("OpenGL")
                         : api == QSGRendererInterface::Software ? QStringLiteral("Software") : QStringLiteral("?");
            Log::Info(QStringLiteral("scene graph: %1").arg(graphicsApi_));
            emit graphicsApiChanged();
        }, Qt::QueuedConnection);
    });
}

int Application::WriteDetectReport(const QString& startDir, const QString& file) {
    const GameDetector detector(AllProfiles());
    const DetectionResult r = detector.detect(startDir);
    QString text = r.report(true);
    if (const Candidate* c = r.game()) {
        text += QStringLiteral("\nprofile=%1\nroot=%2\nexe=%3\ngame=%4\nuserdata=%5\nlogs=%6\ncache=%7\n")
                    .arg(c->profile->id, Native(c->root), Native(c->exePath), Native(c->gameDir), Native(c->userDataDir),
                         Native(c->logsDir), Native(c->shaderCacheDir));
    }
    text += QStringLiteral("\nstatus=%1\n").arg(int(r.status));
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 10;
    f.write(text.toUtf8());
    return int(r.status);
}

void Application::detect() {
    const QString start = options_.gameRoot.isEmpty() ? LauncherDir() : QFileInfo(options_.gameRoot).absoluteFilePath();
    result_ = detector_.detect(start);
    Log::Info(QStringLiteral("detection from %1:\n%2").arg(Native(start), result_.report(false)));
    game_ = nullptr;
    if (const Candidate* c = result_.game()) {
        useCandidate(*c);
    } else {
        emit gameChanged();
    }
}

void Application::redetect() {
    if (process_.gameRunning()) return;
    detect();
}

void Application::chooseGame(const QString& profileId, const QString& root) {
    const GameProfile* p = FindProfile(profileId);
    if (p == nullptr) return;
    Log::Info(QStringLiteral("the user chose %1 in %2").arg(profileId, Native(root)));
    useCandidate(detector_.inspect(*p, QDir::fromNativeSeparators(root)));
}

void Application::useCandidate(const Candidate& c) {
    chosen_ = c;
    game_ = &chosen_;
    // the background and the icon: the game's own pictures (sce_sys), never anything downloaded
    background_.clear();
    icon_.clear();
    for (const auto& rel : c.profile->artCandidates) {
        const QString path = QDir(c.gameDir).filePath(rel);
        if (QFileInfo(path).isFile()) {
            background_ = QUrl::fromLocalFile(path);
            break;
        }
    }
    const QString icon = QDir(c.gameDir).filePath(QStringLiteral("sce_sys/icon0.png"));
    if (QFileInfo(icon).isFile()) icon_ = QUrl::fromLocalFile(icon);

    const QString exeDir = c.exeFound ? QFileInfo(c.exePath).absolutePath() : QString();
    settings_.setRussian(ru());
    settings_.setGame(c.profile, c.configPath, exeDir, System::Query().gpus, System::Displays());
    saves_.setRussian(ru());
    saves_.setGame(c.profile, c.userDataDir);
    process_.setRussian(ru());
    diagnostics_.setRussian(ru());
    diagnostics_.setGame(game_, &settings_);
    Log::Info(QStringLiteral("game: %1 (%2) root %3, exe %4").arg(c.profile->gameName, c.profile->id, Native(c.root), Native(c.exePath)));
    emit gameChanged();
    emit lastLaunchChanged();
    if (!screenshotMode() && c.exeFound && process_.attachIfRunning(c.exePath, c.profile->windowTitle)) {
        onGameStarted();
    }
    saves_.refresh();
    diagnostics_.refresh();
}

QString Application::status() const {
    switch (result_.status) {
    case DetectionResult::Status::Detected: return QStringLiteral("detected");
    case DetectionResult::Status::Incomplete: return QStringLiteral("incomplete");
    case DetectionResult::Status::Ambiguous: return game_ ? QStringLiteral("detected") : QStringLiteral("ambiguous");
    case DetectionResult::Status::NotFound: return QStringLiteral("notfound");
    }
    return {};
}

QVariantList Application::detectionCandidates() const {
    QVariantList out;
    for (const auto& c : result_.candidates) {
        QStringList ev;
        for (const auto& e : c.evidence) ev << QStringLiteral("%1 %2").arg(e.positive ? QStringLiteral("+") : QStringLiteral("−"), ru() ? e.textRu : e.textEn);
        out << QVariantMap{{QStringLiteral("id"), c.profile->id}, {QStringLiteral("name"), c.profile->gameName},
                           {QStringLiteral("root"), Native(c.root)}, {QStringLiteral("titleId"), c.sfo.titleId()},
                           {QStringLiteral("complete"), c.complete()}, {QStringLiteral("evidence"), ev}};
    }
    return out;
}

QVariantList Application::detectionEvidence() const {
    QVariantList out;
    for (const auto& e : result_.evidence) {
        out << QVariantMap{{QStringLiteral("positive"), e.positive}, {QStringLiteral("text"), ru() ? e.textRu : e.textEn}};
    }
    return out;
}

QString Application::searchedFolder() const {
    return Native(result_.startDir);
}

#define GAME_FIELD(name, expr) \
    QString Application::name() const { return game_ && game_->profile ? (expr) : QString(); }
GAME_FIELD(profileId, game_->profile->id)
GAME_FIELD(themeId, game_->profile->themeId)
GAME_FIELD(gameName, game_->profile->gameName)
GAME_FIELD(shortName, game_->profile->shortName)
GAME_FIELD(brand, game_->profile->brand)
GAME_FIELD(productId, game_->profile->productId)
GAME_FIELD(titleId, game_->sfo.titleId().isEmpty() ? game_->profile->titleIds.value(0) : game_->sfo.titleId())
GAME_FIELD(gameRoot, Native(game_->root))
GAME_FIELD(exePath, Native(game_->exePath))
#undef GAME_FIELD

QString Application::gameVersion() const {
    if (game_ == nullptr) return {};
    const QString v = game_->sfo.text(QStringLiteral("APP_VER"));
    return v.isEmpty() ? (ru() ? QStringLiteral("Не определено") : QStringLiteral("Unknown")) : v;
}

QStringList Application::missingFiles() const {
    QStringList out;
    if (game_ == nullptr) return out;
    if (!game_->configFound) out << game_->profile->configFileName;
    else if (!game_->configValid) out << game_->profile->configFileName + QStringLiteral(" (") + game_->configError + u')';
    if (!game_->exeFound) out << game_->profile->exeName;
    if (!game_->titleConfirmed) out << QStringLiteral("sce_sys\\param.sfo (TITLE_ID %1)").arg(game_->profile->titleIds.join(u'/'));
    for (const auto& m : game_->missingFiles) if (!out.contains(m)) out << m;
    return out;
}

QString Application::launcherVersion() const {
    return QCoreApplication::applicationVersion();
}

void Application::setLanguage(const QString& lang) {
    const QString l = lang == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("ru");
    if (l == language_) return;
    language_ = l;
    if (!screenshotMode()) prefs_.setValue(QStringLiteral("ui/language"), l);
    applyLanguage();
    settings_.setRussian(ru());
    saves_.setRussian(ru());
    process_.setRussian(ru());
    diagnostics_.setRussian(ru());
    emit languageChanged();
    emit gameChanged();
    emit lastLaunchChanged();
    emit saves_.changed();
}

void Application::applyLanguage() {
    QCoreApplication::removeTranslator(&translator_);
    if (ru()) {
        if (translator_.load(QStringLiteral(":/i18n/launcher_ru.qm"))) QCoreApplication::installTranslator(&translator_);
        else Log::Warning(QStringLiteral("the Russian translation is missing"));
    }
    if (engine_) engine_->retranslate();
}

void Application::setDesign(const QString& c) {
    const QString v = (c == QLatin1String("neon") || c == QLatin1String("desktop")) ? c : QStringLiteral("cinematic");
    if (v == design_) return;
    design_ = v;
    if (!screenshotMode()) prefs_.setValue(QStringLiteral("ui/concept"), v);
    emit designChanged();
}

void Application::setReducedEffects(bool on) {
    if (on == reduced_) return;
    reduced_ = on;
    if (!screenshotMode()) prefs_.setValue(QStringLiteral("ui/reducedEffects"), on);
    emit prefsChanged();
}

void Application::setMinimizeOnLaunch(bool on) {
    minimizeOnLaunch_ = on;
    prefs_.setValue(QStringLiteral("launch/minimize"), on);
    emit prefsChanged();
}

void Application::setRestoreAfterExit(bool on) {
    restoreAfterExit_ = on;
    prefs_.setValue(QStringLiteral("launch/restoreAfterExit"), on);
    emit prefsChanged();
}

QString Application::formatDate(const QDateTime& t) const {
    if (!t.isValid()) return ru() ? QStringLiteral("Не определено") : QStringLiteral("Unknown");
    return QLocale(ru() ? QLocale::Russian : QLocale::English).toString(t, ru() ? QStringLiteral("d MMMM yyyy, HH:mm") : QStringLiteral("MMMM d, yyyy, HH:mm"));
}

QString Application::lastLaunchText() const {
    if (game_ == nullptr) return {};
    const QString key = QStringLiteral("last/") + game_->profile->id;
    const QDateTime when = prefs_.value(key + QStringLiteral("/time")).toDateTime();
    if (!when.isValid()) return ru() ? QStringLiteral("Ещё не запускалась · через этот лаунчер") : QStringLiteral("Not launched yet · from this launcher");
    const qint64 secs = prefs_.value(key + QStringLiteral("/seconds"), -1).toLongLong();
    QString text = formatDate(when);
    if (secs >= 0) {
        const qint64 h = secs / 3600, m = (secs % 3600) / 60;
        text += QStringLiteral(" · ") + (h > 0 ? (ru() ? QStringLiteral("%1 ч %2 мин") : QStringLiteral("%1 h %2 min")).arg(h).arg(m)
                                               : (ru() ? QStringLiteral("%1 мин") : QStringLiteral("%1 min")).arg(std::max<qint64>(m, secs > 0 ? 1 : 0)));
    }
    if (prefs_.value(key + QStringLiteral("/failed"), false).toBool()) {
        text += ru() ? QStringLiteral(" · завершилась с ошибкой") : QStringLiteral(" · ended with an error");
    }
    return text;
}

void Application::play() {
    if (game_ == nullptr || process_.busy()) return;
    // 1. the settings changed here go to the file first (the game reads it at its start)
    if (settings_.dirty() && !settings_.save()) {
        emit message(settings_.lastError(), QStringLiteral("error"));
        return;
    }
    LaunchSpec spec;
    spec.profile = game_->profile;
    spec.exePath = game_->exePath;
    spec.workingDir = game_->root;
    spec.gameDir = game_->gameDir;
    spec.userDataDir = game_->userDataDir;
    spec.args = game_->profile->launchArgs;
    spec.env = settings_.launchEnvironment();
    Log::Info(QStringLiteral("play: %1, profile %2").arg(Native(spec.exePath), settings_.launchProfile()));
    process_.launch(spec);
}

void Application::onGameStarted() {
    const QString reason = ru() ? QStringLiteral("Игра запущена: настройки и сохранения доступны только для просмотра, пока она не закрыта.")
                                : QStringLiteral("The game is running: settings and saves are read-only until it is closed.");
    settings_.setLocked(true, reason);
    saves_.setLocked(true);
    diagnostics_.setGameRunning(true);
    Log::Info(QStringLiteral("game process %1: %2").arg(process_.pid()).arg(process_.commandLine()));
    if (game_) prefs_.setValue(QStringLiteral("last/") + game_->profile->id + QStringLiteral("/time"), QDateTime::currentDateTime());
    if (minimizeOnLaunch_ && !screenshotMode()) emit requestMinimize();
}

void Application::onGameFinished(int exitCode, qint64 seconds, bool early) {
    Log::Info(QStringLiteral("game ended: exit code 0x%1 after %2 s%3").arg(QString::number(uint(exitCode), 16)).arg(seconds)
                  .arg(early ? QStringLiteral(" (before its window appeared)") : QString()));
    if (game_) {
        const QString key = QStringLiteral("last/") + game_->profile->id;
        prefs_.setValue(key + QStringLiteral("/seconds"), seconds);
        prefs_.setValue(key + QStringLiteral("/exitCode"), exitCode);
        prefs_.setValue(key + QStringLiteral("/failed"), early);
    }
    settings_.setLocked(false, {});
    saves_.setLocked(false);
    diagnostics_.setGameRunning(false);
    saves_.refresh();
    diagnostics_.refresh();
    emit lastLaunchChanged();
    if (restoreAfterExit_ || early) emit requestRestore();
}

void Application::setNotice(const QString& n) {
    notice_ = n;
    emit noticeChanged();
}

void Application::clearNotice() {
    setNotice({});
}

void Application::takeScreenshot(QObject* object) {
    auto* item = qobject_cast<QQuickItem*>(object);
    if (item == nullptr || options_.screenshot.isEmpty()) return;
    // the size in logical pixels: grabToImage applies the window's device pixel ratio (--scale) itself
    const auto grab = item->grabToImage(QSize(qRound(item->width()), qRound(item->height())));
    if (!grab) {
        Log::Error(QStringLiteral("screenshot: grabToImage failed"));
        QCoreApplication::exit(3);
        return;
    }
    const QString file = options_.screenshot;
    connect(grab.data(), &QQuickItemGrabResult::ready, this, [grab, file] {
        const bool ok = grab->saveToFile(file);
        Log::Info(QStringLiteral("screenshot %1: %2 (%3x%4)").arg(ok ? QStringLiteral("saved") : QStringLiteral("NOT saved"), Native(file))
                      .arg(grab->image().width()).arg(grab->image().height()));
        QCoreApplication::exit(ok ? 0 : 4);
    });
}

} // namespace Launcher
