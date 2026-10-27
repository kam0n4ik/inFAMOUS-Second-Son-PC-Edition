// inFAMOUS PC Launcher — entry point. One Launcher.exe for both games: which game it serves is found from the folder
// it stands in (docs\GAME_DETECTION.md), never from the working directory or a path built in.
//
// Options (for the tests and the screenshots; a player needs none):
//   --game-root <dir>        look for the game in <dir> instead of the launcher's folder
//   --detect-report <file>   detect, write the result to <file> and exit (exit code = the status, 0 = found)
//   --screenshot <png>       render once and save the window's content: --page home|settings|saves|diagnostics,
//                            --concept cinematic|neon|desktop, --size WxH, --scale 1.5, --lang ru|en, --reduced,
//                            --dialog notfound|ambiguous|confirm|log
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>

#include <windows.h>

#include "app/Application.h"
#include "launcher_version.h"
#include "system/Log.h"

namespace {

QString ArgValue(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], name) == 0) return QString::fromLocal8Bit(argv[i + 1]);
    }
    return {};
}

bool HasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], name) == 0) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // command-line paths may hold any characters: take them from the wide command line, not the ANSI argv
    QStringList wargs;
    {
        int n = 0;
        LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 0; i < n; ++i) wargs << QString::fromWCharArray(w[i]);
        LocalFree(w);
    }
    const auto wide = [&](const char* name) {
        const qsizetype i = wargs.indexOf(QString::fromLatin1(name));
        return i >= 0 && i + 1 < wargs.size() ? wargs[i + 1] : QString();
    };

    if (HasArg(argc, argv, "--scale")) {
        qputenv("QT_SCALE_FACTOR", ArgValue(argc, argv, "--scale").toLatin1());
    }
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    // Direct3D 11 through Qt's RHI: the scene graph renders on the GPU, with no OpenGL on Windows
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("inFAMOUS PC Launcher"));
    QGuiApplication::setOrganizationName(QStringLiteral("inFAMOUS PC Edition"));
    QGuiApplication::setApplicationVersion(QStringLiteral(LAUNCHER_VERSION));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/launcher.ico")));

    if (!wide("--detect-report").isEmpty()) {
        const QString root = wide("--game-root");
        return Launcher::Application::WriteDetectReport(root.isEmpty() ? Launcher::LauncherDir() : root, wide("--detect-report"));
    }

    Launcher::Log::Init();
    Launcher::Log::Info(QStringLiteral("inFAMOUS PC Launcher %1 (Qt %2), %3").arg(QStringLiteral(LAUNCHER_VERSION), QString::fromLatin1(qVersion()),
                                                                              QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/RobotoCondensed.ttf"));

    Launcher::Options options;
    options.gameRoot = wide("--game-root");
    options.screenshot = wide("--screenshot");
    if (!wide("--page").isEmpty()) options.page = wide("--page");
    options.design = wide("--concept");
    options.lang = wide("--lang");
    options.dialog = wide("--dialog");
    options.reduced = wargs.contains(QStringLiteral("--reduced"));
    const QString size = wide("--size");
    if (const qsizetype x = size.indexOf(u'x'); x > 0) options.size = QSize(size.left(x).toInt(), size.mid(x + 1).toInt());

    Launcher::Application launcher(options);
    QQmlApplicationEngine engine;
    launcher.attach(&engine);
    launcher.detect();
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &launcher);
    engine.rootContext()->setContextProperty(QStringLiteral("GameSettings"), launcher.settings());
    engine.rootContext()->setContextProperty(QStringLiteral("Game"), launcher.process());
    engine.rootContext()->setContextProperty(QStringLiteral("Saves"), launcher.saves());
    engine.rootContext()->setContextProperty(QStringLiteral("Diag"), launcher.diagnostics());
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(2); }, Qt::QueuedConnection);
    engine.loadFromModule("Launcher", "Main");
    return QGuiApplication::exec();
}
