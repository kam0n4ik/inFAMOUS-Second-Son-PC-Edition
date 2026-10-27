// inFAMOUS PC Launcher — fixture folders for the tests: a game folder laid out as a release (or the dev tree) of
// Second Son PC Edition / First Light PC Edition, with the stub game as its exe. Everything lives in a QTemporaryDir.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace Fixture {

inline bool Write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(data);
    return true;
}

inline QString SsSfo() { return QStringLiteral(LAUNCHER_SS_SFO); }
inline QString FlSfo() { return QStringLiteral(LAUNCHER_FL_SFO); }
inline bool HaveSfos() { return QFileInfo(SsSfo()).isFile() && QFileInfo(FlSfo()).isFile(); }

struct Options {
    bool config = true;
    bool exe = true;
    bool exeInBuildBin = false;
    bool eboot = true;
    bool sceModule = true;
    QString sfo; // "" = none
    QByteArray extraToml;
};

/// Lays out `product` ("SecondSonPC" / "FirstLightPC") in `root`.
inline bool MakeGame(const QString& root, const QString& product, const Options& o) {
    QDir().mkpath(root);
    if (o.config) {
        QByteArray toml = "# fixture\n[Game]\npath = \".\"\n\n[Display]\nwidth = 1280\nheight = 720\n";
        toml += o.extraToml;
        if (!Write(root + "/" + product + ".toml", toml)) return false;
    }
    if (o.exe) {
        const QString exe = root + (o.exeInBuildBin ? "/build/bin/" : "/") + product + ".exe";
        QDir().mkpath(QFileInfo(exe).absolutePath());
        QFile::remove(exe);
        if (!QFile::copy(QStringLiteral(LAUNCHER_STUB), exe)) return false;
    }
    if (o.eboot && !Write(root + "/eboot.bin", "stub")) return false;
    if (o.sceModule) QDir().mkpath(root + "/sce_module");
    if (!o.sfo.isEmpty()) {
        QDir().mkpath(root + "/sce_sys");
        QFile::remove(root + "/sce_sys/param.sfo");
        if (!QFile::copy(o.sfo, root + "/sce_sys/param.sfo")) return false;
    }
    return true;
}

inline QByteArray Read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace Fixture
