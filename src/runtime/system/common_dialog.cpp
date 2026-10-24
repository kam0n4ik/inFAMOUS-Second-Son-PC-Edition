// SecondSonPC — libSceCommonDialog (R10 sysrt; oracle: third_party/shadPS4/src/core/libraries/system/commondialog.cpp).
// The game imports one function of it, sceCommonDialogInitialize (eboot.bin 0x15d7d9, once at start, before its first
// dialog library is loaded; the result is not checked there). The layer is initialised once per process: the first
// call succeeds, every later one returns SCE_COMMON_DIALOG_ERROR_ALREADY_SYSTEM_INITIALIZED and changes nothing, as the
// oracle. There is no termination function: the state lasts until the process ends.
#include "runtime/system/common_dialog.h"

#include <atomic>

#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::CommonDialog {

namespace {

std::atomic<bool> g_initialized{false};
std::atomic<InitializeListener> g_listener{nullptr};

SSPC_SYSV_ABI s32 Initialize() {
    if (g_initialized.exchange(true, std::memory_order_acq_rel)) {
        SSPC_LOG(SystemService, Info, "sceCommonDialogInitialize: already initialised");
        return kErrAlreadySystemInitialized;
    }
    if (const auto listener = g_listener.load(std::memory_order_acquire)) {
        listener();
    }
    SSPC_LOG(SystemService, Info, "sceCommonDialogInitialize: system dialog layer initialised");
    return 0;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"uoUpLGNkygk", "libSceCommonDialog", "libSceCommonDialog", "sceCommonDialogInitialize", Fn(&Initialize), nullptr,
     "sysrt-dialog", false, true},
};

} // namespace

void SetInitializeListener(InitializeListener listener) {
    g_listener.store(listener, std::memory_order_release);
}

bool Initialized() {
    return g_initialized.load(std::memory_order_acquire);
}

std::span<const Export> CommonDialogExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::CommonDialog
