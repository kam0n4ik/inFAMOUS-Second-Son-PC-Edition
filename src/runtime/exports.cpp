// SecondSonPC — export table. NIDs/library/module strings are the game's own import keys
// (docs/generated/imports.csv, prx_imports.csv; the loader checks them against its import table).
#include "runtime/exports.h"

#include <vector>

#include "runtime/filesystem/file_system.h"
#include "runtime/filesystem/fs_observer.h"
#include "runtime/gpu/gnm/gnm_driver.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/kernel/clock.h"
#include "runtime/kernel/event_queues.h"
#include "runtime/kernel/guest_errno.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/kernel_memory.h"
#include "runtime/kernel/signal.h"
#include "runtime/kernel/thread.h"
#include "runtime/libc/libc_stdio.h"
#include "runtime/net/net_core.h"
#include "runtime/pad/pad.h"
#include "runtime/system/common_dialog.h"
#include "runtime/savedata/savedata.h"
#include "runtime/savedata/savedata_dialog.h"
#include "runtime/system/system_service.h"
#include "runtime/user/user_service.h"

namespace SecondSon::Loader {
std::span<const Runtime::Export> RtldExports(); // loader/rtld.cpp
}

namespace SecondSon::Media {
std::span<const Runtime::Export> VideodecExports(); // runtime/media/videodec.cpp
}

namespace SecondSon::Audio {
std::span<const Runtime::Export> AudioOutExports(); // runtime/audio/audio_out.cpp
std::span<const Runtime::Export> AjmExports();      // runtime/audio/ajm.cpp (R10: empty unless SSPC_R10_AJM)
}

namespace SecondSon::Runtime {

namespace Sysmodule {
std::span<const Export> SysmoduleExports(); // runtime/sysmodule/sysmodule.cpp
}
namespace PlayGo {
std::span<const Export> PlayGoExports(); // runtime/playgo/playgo.cpp
}
namespace AppContent {
std::span<const Export> AppContentExports(); // runtime/app_content/app_content.cpp
}
namespace Net {
std::span<const Export> NetUtilExports(); // runtime/net/net_util.cpp
}
namespace Np {
std::span<const Export> NpManagerExports(); // runtime/np/np_manager.cpp
}
namespace Offline {
std::span<const Export> OfflineExports(); // runtime/np/offline_services.cpp
}
namespace Libc {
std::span<const Export> LibcMemExports(); // runtime/libc/libc_mem.cpp
}

namespace {

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kSystemExports[] = {
    {"fZo48un7LK4", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetInt",
     Fn(&SystemService::ParamGetInt), nullptr, "system", true},
    {"SsC-m-S9JTA", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetString",
     Fn(&SystemService::ParamGetString), nullptr, "system", true},
    {"rPo6tV8D9bM", "libSceSystemService", "libSceSystemService", "sceSystemServiceGetStatus",
     Fn(&SystemService::GetStatus), nullptr, "system"},
    {"656LMQSrg6U", "libSceSystemService", "libSceSystemService", "sceSystemServiceReceiveEvent",
     Fn(&SystemService::ReceiveEvent), nullptr, "system"},
    {"Vo5V8KAwCmk", "libSceSystemService", "libSceSystemService", "sceSystemServiceHideSplashScreen",
     Fn(&SystemService::HideSplashScreen), nullptr, "system"},
    {"1n37q1Bvc5Y", "libSceSystemService", "libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo",
     Fn(&SystemService::GetDisplaySafeAreaInfo), nullptr, "system"},
};

// Observers in front of the BRIDGE file functions (called by the game's libSceFios2.prx). They
// stay active while the file functions are BRIDGE.
const Export kFilesystemObservers[] = {
    {"1G3lF1Gg1k8", "libkernel", "libkernel", "sceKernelOpen", Fn(&Filesystem::ObserveOpen),
     &Filesystem::g_open_original, "fs", true},
    {"eV9wAD2riIA", "libkernel", "libkernel", "sceKernelStat", Fn(&Filesystem::ObserveStat),
     &Filesystem::g_stat_original, "fs", true},
};

std::vector<Export> Collect() {
    std::vector<Export> all;
    // Order matters: the first entry for an import key wins (native FileSystem before the Phase 0
    // observers of the same functions, which stay for --loader shadps4 / --bridge fs).
    for (const auto group : {std::span<const Export>(kSystemExports), Loader::RtldExports(),
                             Sysmodule::SysmoduleExports(), SaveData::Exports(), SaveData::Dialog::Exports(),
                             Audio::AudioOutExports(), Media::VideodecExports(), Filesystem::FileSystemExports(),
                             UserService::UserServiceExports(), PlayGo::PlayGoExports(),
                             AppContent::AppContentExports(), Pad::PadExports(),
                             Kernel::KernelExports(), Kernel::SemaphoreExports(),
                             Kernel::EventFlagExports(), Kernel::EventQueues::NativeExports(), Kernel::KernelUtilExports(),
                             Kernel::MutexExports(), Kernel::CondvarExports(), Kernel::RwlockExports(),
                             Kernel::DirectMemoryExports(), Kernel::ProcessExports(), Kernel::Threads::ThreadExports(),
                             Kernel::Signals::SignalExports(), Kernel::GuestErrno::Exports(),
                             Audio::AjmExports(), // R10 (empty unless SSPC_R10_AJM=native|verify)
                             Kernel::MemoryApi::Exports(), Kernel::Clock::Exports(),
                             Net::NetUtilExports(), Np::NpManagerExports(), Offline::OfflineExports(), Libc::LibcMemExports(), Gpu::Gnm::GnmExports(), Gpu::VideoOut::Exports(),
                             std::span<const Export>(kFilesystemObservers)}) {
        all.insert(all.end(), group.begin(), group.end());
    }
    // R10 sysrt (docs/R10_SYSRT.md): CommonDialog, libSceNet's library state, libSceLibcInternal's stdout and heap
    // trace information - experimental groups sysrt-dialog / sysrt-net / sysrt-libc (SSPC_R10_SYSRT=native)
    for (const auto group : {CommonDialog::CommonDialogExports(), Net::NetCoreExports(), Libc::LibcStdioExports()}) {
        all.insert(all.end(), group.begin(), group.end());
    }
    return all;
}

} // namespace

std::span<const Export> Exports() {
    static const std::vector<Export> all = Collect();
    return all;
}

} // namespace SecondSon::Runtime
