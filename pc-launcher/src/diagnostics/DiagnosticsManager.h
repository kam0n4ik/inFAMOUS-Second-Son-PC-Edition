// inFAMOUS PC Launcher — what the diagnostics page shows and copies: the game (from its own files), the PC runtime,
// the computer (Windows, CPU, RAM, GPUs through DXGI, Vulkan through vulkan-1.dll), the device the game used last
// (its log), the shader cache (its real files) and the logs. Anything not established is "Не определено"/"Unknown".
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include "detection/GameDetector.h"
#include "system/SystemInfo.h"

namespace Launcher {

class SettingsManager;

struct CacheInfo {
    QString root;       // <[ShaderCache] dir>\<TitleID>
    qint64 bytes = 0;
    qint64 files = 0;
    int generations = 0; // folders of the cache's own (generation / shaders.pack / vulkan.bin)
    bool inUse = false;  // a generation's `lock` file
};

struct LogInfo {
    QString path;
    qint64 bytes = 0;
    QString sessionStarted; // "2026-10-27 14:11:53" (the log's first line)
    QString vulkanDevice;   // "NVIDIA GeForce RTX 2060 (driver 0x95054000)"
};

class DiagnosticsManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString cacheSize READ cacheSize NOTIFY cacheChanged)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY cacheChanged)
    Q_PROPERTY(int cacheGenerations READ cacheGenerations NOTIFY cacheChanged)
    Q_PROPERTY(bool cacheKnown READ cacheKnown NOTIFY cacheChanged)
    Q_PROPERTY(QString cacheDir READ cacheDir NOTIFY cacheChanged)
    Q_PROPERTY(QString logPath READ logPath NOTIFY changed)
    Q_PROPERTY(QString launcherLogPath READ launcherLogPath CONSTANT)

public:
    explicit DiagnosticsManager(QObject* parent = nullptr);

    void setGame(const Candidate* game, SettingsManager* settings);
    void setRussian(bool ru);
    void setGameRunning(bool running) { gameRunning_ = running; }

    QVariantList items() const { return items_; }
    bool loading() const { return loading_; }
    QString cacheSize() const;
    qint64 cacheBytes() const { return cache_.bytes; }
    int cacheGenerations() const { return cache_.generations; }
    bool cacheKnown() const { return cacheKnown_; }
    QString cacheDir() const;
    QString logPath() const;
    QString launcherLogPath() const;

    Q_INVOKABLE void refresh();       // everything (the system part once per run)
    Q_INVOKABLE void refreshCache();  // the shader cache's size only
    Q_INVOKABLE QString report() const;
    Q_INVOKABLE void copyReport() const;
    Q_INVOKABLE void copyText(const QString& text) const;
    Q_INVOKABLE QString logTail(int lines = 400) const;
    Q_INVOKABLE QString launcherLogTail(int lines = 300) const;
    Q_INVOKABLE bool revealLog() const;
    Q_INVOKABLE bool revealGameFolder() const;
    Q_INVOKABLE bool revealConfigFolder() const;
    Q_INVOKABLE bool revealCache() const;
    Q_INVOKABLE QString checkFiles() const;
    Q_INVOKABLE QString checkConfig() const;
    /// Removes the cache generations (the folders the runtime's --clear-shader-cache removes, plus native_own's), or, while
    /// the game runs, leaves the runtime's clear_shaders.request for its next start. Returns the message to show.
    Q_INVOKABLE QString clearShaderCache();

    static CacheInfo MeasureCache(const QString& cacheBase, const QStringList& titleIds);
    /// The generation folders the launcher may remove (never one with a `lock` file).
    static QStringList CacheGenerations(const QString& cacheBase, const QStringList& titleIds, int* inUse = nullptr);
    static LogInfo ReadLog(const QString& path);

signals:
    void changed();
    void loadingChanged();
    void cacheChanged();

private:
    void rebuild();

    const Candidate* game_ = nullptr;
    SettingsManager* settings_ = nullptr;
    bool ru_ = true;
    bool loading_ = false;
    bool systemLoaded_ = false;
    bool cacheKnown_ = false;
    bool gameRunning_ = false;
    SystemInfo system_;
    VulkanInfo vulkan_;
    QList<DisplayInfo> displays_;
    CacheInfo cache_;
    LogInfo log_;
    QVariantList items_;
    quint64 generation_ = 0;
};

} // namespace Launcher
