#include "system/SystemInfo.h"

#include <QRegularExpression>
#include <QSettings>

#include <windows.h>

#include <dxgi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

namespace Launcher::System {

namespace {

QString RegString(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        return QString::fromWCharArray(buf).trimmed();
    }
    return {};
}

DWORD RegDword(HKEY root, const wchar_t* path, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

// ---- Vulkan, loaded at run time (the launcher itself never needs it)
using PFN_GetProc = void* (__stdcall*)(void*, const char*);
struct VkAppInfo {
    int sType;
    const void* pNext;
    const char* app;
    uint32_t appVersion;
    const char* engine;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceInfo {
    int sType;
    const void* pNext;
    uint32_t flags;
    const VkAppInfo* app;
    uint32_t layers;
    const char* const* layerNames;
    uint32_t exts;
    const char* const* extNames;
};
struct VkDeviceProps { // VkPhysicalDeviceProperties up to deviceName
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int deviceType;
    char deviceName[256];
    uint8_t rest[1024]; // pipelineCacheUUID, limits, sparse properties
};

QString VkVersion(uint32_t v) {
    return QStringLiteral("%1.%2.%3").arg(v >> 22 & 0x7F).arg(v >> 12 & 0x3FF).arg(v & 0xFFF);
}

} // namespace

SystemInfo Query() {
    SystemInfo s;
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    QString product = RegString(HKEY_LOCAL_MACHINE, nt, L"ProductName");
    const QString display = RegString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion");
    const QString build = RegString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber");
    const DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR");
    if (build.toInt() >= 22000) {
        product.replace(QStringLiteral("Windows 10"), QStringLiteral("Windows 11")); // (ProductName still says 10)
    }
    s.windows = QStringLiteral("%1%2 (build %3.%4)")
                    .arg(product.isEmpty() ? QStringLiteral("Windows") : product,
                         display.isEmpty() ? QString() : u' ' + display, build)
                    .arg(ubr);
    s.cpu = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    s.logicalCores = static_cast<int>(si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.ramBytes = ms.ullTotalPhys;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                s.gpus.push_back({QString::fromWCharArray(d.Description), d.VendorId, d.DeviceId,
                                  static_cast<quint64>(d.DedicatedVideoMemory)});
            }
            adapter.Reset();
        }
    }
    return s;
}

VulkanInfo QueryVulkan() {
    VulkanInfo v;
    HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (lib == nullptr) {
        v.error = QStringLiteral("vulkan-1.dll not found");
        return v;
    }
    v.loaderFound = true;
    auto gipa = reinterpret_cast<PFN_GetProc>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    if (gipa == nullptr) {
        v.error = QStringLiteral("vkGetInstanceProcAddr missing");
        FreeLibrary(lib);
        return v;
    }
    using PFN_Ver = int(__stdcall*)(uint32_t*);
    using PFN_Create = int(__stdcall*)(const VkInstanceInfo*, const void*, void**);
    using PFN_Destroy = void(__stdcall*)(void*, const void*);
    using PFN_Enum = int(__stdcall*)(void*, uint32_t*, void**);
    using PFN_Props = void(__stdcall*)(void*, VkDeviceProps*);
    uint32_t api = (1u << 22);
    if (auto ver = reinterpret_cast<PFN_Ver>(gipa(nullptr, "vkEnumerateInstanceVersion"))) {
        ver(&api);
    }
    v.apiVersion = VkVersion(api);
    auto create = reinterpret_cast<PFN_Create>(gipa(nullptr, "vkCreateInstance"));
    VkAppInfo app{0 /*VK_STRUCTURE_TYPE_APPLICATION_INFO*/, nullptr, "inFAMOUS PC Launcher", 1, nullptr, 0,
                  (1u << 22) | (1u << 12)};
    VkInstanceInfo info{1 /*VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO*/, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    void* instance = nullptr;
    if (create == nullptr || create(&info, nullptr, &instance) != 0 || instance == nullptr) {
        v.error = QStringLiteral("vkCreateInstance failed");
        FreeLibrary(lib);
        return v;
    }
    auto destroy = reinterpret_cast<PFN_Destroy>(gipa(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_Enum>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_Props>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    uint32_t n = 0;
    if (enumerate != nullptr && props != nullptr && enumerate(instance, &n, nullptr) == 0 && n > 0) {
        std::vector<void*> devs(n);
        enumerate(instance, &n, devs.data());
        for (uint32_t i = 0; i < n; ++i) {
            VkDeviceProps p{};
            props(devs[i], &p);
            QString drv;
            if (p.vendorID == 0x10DE) { // NVIDIA's packing
                drv = QStringLiteral("%1.%2").arg(p.driverVersion >> 22 & 0x3FF).arg(p.driverVersion >> 14 & 0xFF, 2, 10, QChar(u'0'));
            } else {
                drv = VkVersion(p.driverVersion);
            }
            v.devices << QStringLiteral("%1 (Vulkan %2, driver %3)")
                             .arg(QString::fromUtf8(p.deviceName), VkVersion(p.apiVersion), drv);
        }
    }
    if (destroy != nullptr) {
        destroy(instance, nullptr);
    }
    FreeLibrary(lib);
    return v;
}

QList<DisplayInfo> Displays() {
    // friendly names by GDI device name (SDL3's WIN_GetDisplayNameVista: QueryDisplayConfig target names)
    QHash<QString, QString> friendly;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) ==
            ERROR_SUCCESS) {
            for (UINT32 i = 0; i < pathCount; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = paths[i].targetInfo.adapterId;
                tgt.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
                    DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS && tgt.monitorFriendlyDeviceName[0] != 0) {
                    friendly.insert(QString::fromWCharArray(src.viewGdiDeviceName),
                                    QString::fromWCharArray(tgt.monitorFriendlyDeviceName));
                }
            }
        }
    }
    QList<DisplayInfo> list;
    // primary first, then the others in the adapters' order (SDL's two passes)
    for (int pass = 0; pass < 2; ++pass) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
            const bool primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0 || primary != (pass == 0)) {
                continue;
            }
            DisplayInfo d;
            d.device = QString::fromWCharArray(adapter.DeviceName);
            d.primary = primary;
            d.name = friendly.value(d.device);
            if (d.name.isEmpty()) {
                DISPLAY_DEVICEW mon{};
                mon.cb = sizeof(mon);
                if (EnumDisplayDevicesW(adapter.DeviceName, 0, &mon, 0)) {
                    d.name = QString::fromWCharArray(mon.DeviceString);
                }
            }
            if (d.name.isEmpty()) {
                d.name = d.device;
            }
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                d.width = static_cast<int>(mode.dmPelsWidth);
                d.height = static_cast<int>(mode.dmPelsHeight);
                d.refreshHz = static_cast<int>(mode.dmDisplayFrequency);
            }
            list.push_back(d);
        }
    }
    // a second display of one model: "<name> (2)" (menu_controller.cpp Displays)
    for (int i = 0; i < list.size(); ++i) {
        int same = 0;
        for (int j = 0; j < i; ++j) {
            if (list[j].name == list[i].name || list[j].name.startsWith(list[i].name + QStringLiteral(" ("))) {
                ++same;
            }
        }
        if (same > 0) {
            list[i].name = QStringLiteral("%1 (%2)").arg(list[i].name).arg(same + 1);
        }
    }
    return list;
}

bool HasNvidia(const QList<GpuInfo>& gpus) {
    return std::any_of(gpus.begin(), gpus.end(), [](const GpuInfo& g) { return g.vendorId == 0x10DE; });
}

bool HasNvidiaRtx(const QList<GpuInfo>& gpus) {
    return std::any_of(gpus.begin(), gpus.end(),
                       [](const GpuInfo& g) { return g.vendorId == 0x10DE && g.name.contains(QStringLiteral("RTX")); });
}

bool HasNvidiaRtx40Plus(const QList<GpuInfo>& gpus) {
    static const QRegularExpression re(QStringLiteral("RTX\\s*(?:PRO\\s*)?(\\d{2})\\d{2}"));
    for (const auto& g : gpus) {
        const auto m = re.match(g.name);
        if (g.vendorId == 0x10DE && m.hasMatch() && m.captured(1).toInt() >= 40) {
            return true;
        }
    }
    return false;
}

bool IsProcessRunning(const QString& exeFileName, quint32 exceptPid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::wstring name = exeFileName.toStdWString();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID != exceptPid && _wcsicmp(pe.szExeFile, name.c_str()) == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

} // namespace Launcher::System
