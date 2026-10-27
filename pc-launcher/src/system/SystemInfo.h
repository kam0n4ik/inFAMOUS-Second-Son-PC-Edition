// inFAMOUS PC Launcher — the computer: Windows, processor, memory, video adapters (DXGI), Vulkan (vulkan-1.dll loaded
// on demand, never required) and the displays under the names the game's [Display] monitor uses (SDL's).
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace Launcher {

struct DisplayInfo {
    QString name;      // as SDL names it: the monitor's friendly name, "<name> (2)" for a second one of one model
    QString device;    // \\.\DISPLAY1
    int width = 0, height = 0, refreshHz = 0;
    bool primary = false;
};

struct GpuInfo {
    QString name;
    quint32 vendorId = 0, deviceId = 0;
    quint64 dedicatedVideoMemory = 0;
};

struct VulkanInfo {
    bool loaderFound = false;
    QString apiVersion;    // the instance version of the loader
    QStringList devices;   // "NVIDIA GeForce RTX 2060 (Vulkan 1.3.x, driver ...)"
    QString error;
};

struct SystemInfo {
    QString windows;     // "Windows 10 Pro 22H2 (build 19045.6093)"
    QString cpu;
    int logicalCores = 0;
    quint64 ramBytes = 0;
    QList<GpuInfo> gpus; // hardware adapters (no "Microsoft Basic Render Driver")
};

namespace System {

SystemInfo Query();
VulkanInfo QueryVulkan();
QList<DisplayInfo> Displays();

bool HasNvidiaRtx(const QList<GpuInfo>& gpus);
bool HasNvidia(const QList<GpuInfo>& gpus);
/// GeForce RTX 40 / 50 (DLSS Frame Generation): the series from the adapter's name.
bool HasNvidiaRtx40Plus(const QList<GpuInfo>& gpus);

/// Is a process with this executable file name running (any folder)? `exceptPid` is skipped.
bool IsProcessRunning(const QString& exeFileName, quint32 exceptPid = 0);

} // namespace System

} // namespace Launcher
