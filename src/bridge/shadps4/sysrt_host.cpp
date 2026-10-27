// SecondSonPC — R10 sysrt adapters (TEMPORARY, removed with the bridge's dialog and socket libraries).
//   sysrt-dialog: the bridge's MsgDialog / ErrorDialog (and its SaveDataDialog with --bridge savedata) check the
//                 bridge's own CommonDialog::g_isInitialized; SecondSonPC's sceCommonDialogInitialize sets it there too.
//                 SecondSonPC's save data dialog reads that flag and shares the bridge's g_isUsed (always).
//   sysrt-net:    the bridge's sockets, epoll and resolvers (imported, never called offline) keep their own host
//                 start-up and pool table and their own errno: SecondSonPC's sceNetInit starts the bridge's host
//                 networking, its pools are mirrored into the bridge's table (same ids: both take the first free of
//                 3000..3030; a resolver of the bridge holding one keeps it alive: ENOTEMPTY), and the bridge's
//                 SetErrno (patch-free hook of net.cpp) also writes SecondSonPC's errno, the one sceNetErrnoLoc gives
//                 (and still the kernel errno, as the hook it replaces).
//                 Not covered: the resolvers' own errno writes (ResolverReturn), which bypass the hook.
#include "bridge/shadps4/sysrt_host.h"

#include <algorithm>

#include "app/config.h"
#include "base/log.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/net/net.h"
#include "core/libraries/system/commondialog.h"
#include "runtime/net/net_core.h"
#include "runtime/system/common_dialog.h"

namespace SecondSon::Bridge {

namespace {

bool Listed(const std::vector<std::string>& v, std::string_view g) {
    return std::find(v.begin(), v.end(), g) != v.end();
}

// Replaces net_kernel.cpp's hook (InstallKernelIntegration, at the kernel's RegisterLib: before this), so it does what
// that one did too: the guest's kernel errno (__Error(), SecondSonPC's since patch 0023) gets the value as well.
void MirrorNetErrno(int orbis_errno) {
    *Libraries::Kernel::__Error() = orbis_errno;
    Runtime::Net::SetErrno(orbis_errno);
}

} // namespace

void InstallSysrtHost(const App::Config& cfg) {
    // SecondSonPC's save data dialog (group "savedata") beside the bridge's MsgDialog / ErrorDialog: the layer's state
    // and the one-dialog-at-a-time flag are the bridge's (the bridge's sceCommonDialogInitialize sets the first, or the
    // listener below mirrors SecondSonPC's into it; its dialogs take and give back the second)
    Runtime::CommonDialog::SetBridgeInitializedSource([] { return Libraries::CommonDialog::g_isInitialized; });
    Runtime::CommonDialog::SetSharedUseFlag(&Libraries::CommonDialog::g_isUsed);
    if (Listed(cfg.native_groups, "sysrt-dialog")) {
        Runtime::CommonDialog::SetInitializeListener([] { Libraries::CommonDialog::g_isInitialized = true; });
        SSPC_LOG(Bridge, Info, "sysrt-dialog NATIVE: the bridge's dialogs see SecondSonPC's CommonDialog state");
    }
    if (Listed(cfg.native_groups, "sysrt-net")) {
        Runtime::Net::BridgeHooks hooks;
        hooks.init = [] { Libraries::Net::sceNetInit(); };
        hooks.pool_created = [](s32, const char* name, s32 size, s32 flags) {
            return Libraries::Net::sceNetPoolCreate(name, size, flags);
        };
        hooks.pool_destroy = [](s32 id) {
            const s32 r = Libraries::Net::sceNetPoolDestroy(id);
            return r == Runtime::Net::kErrorBase + Runtime::Net::kEbadf ? 0 : r; // (not mirrored: nothing to keep)
        };
        Runtime::Net::SetBridgeHooks(hooks);
        Libraries::Net::SetKernelErrnoHook(&MirrorNetErrno);
        SSPC_LOG(Bridge, Info, "sysrt-net NATIVE: the bridge's socket layer mirrors SecondSonPC's pools and errno");
    }
}

} // namespace SecondSon::Bridge
