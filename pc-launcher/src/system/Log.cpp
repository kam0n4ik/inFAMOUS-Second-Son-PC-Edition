#include "system/Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

namespace Launcher::Log {

namespace {

QMutex g_mutex;
QFile* g_file = nullptr;

void Write(const char* level, const QString& text) {
    const QString line = QStringLiteral("[%1] [%2] %3\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                  QLatin1String(level), text);
    QMutexLocker lock(&g_mutex);
    if (g_file) {
        g_file->write(line.toUtf8());
        g_file->flush();
    }
}

void QtHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    switch (type) {
    case QtDebugMsg: Write("Debug", msg); break;
    case QtInfoMsg: Write("Info", msg); break;
    case QtWarningMsg: Write("Warning", msg); break;
    case QtCriticalMsg:
    case QtFatalMsg: Write("Error", msg); break;
    }
}

} // namespace

QString Dir() {
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    return QDir::fromNativeSeparators(base.isEmpty() ? QDir::homePath() + QStringLiteral("/AppData/Local") : base) +
           QStringLiteral("/inFAMOUS PC Launcher");
}

QString Path() {
    return Dir() + QStringLiteral("/launcher.log");
}

void Init() {
    QDir().mkpath(Dir());
    const QString path = Path();
    if (QFile::exists(path)) {
        QFile::remove(Dir() + QStringLiteral("/launcher.prev.log"));
        QFile::rename(path, Dir() + QStringLiteral("/launcher.prev.log"));
    }
    g_file = new QFile(path);
    if (!g_file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        delete g_file;
        g_file = nullptr;
    }
    qInstallMessageHandler(QtHandler);
}

void Info(const QString& text) { Write("Info", text); }
void Warning(const QString& text) { Write("Warning", text); }
void Error(const QString& text) { Write("Error", text); }

} // namespace Launcher::Log
