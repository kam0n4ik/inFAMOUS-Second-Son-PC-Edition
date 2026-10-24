// SecondSonPC — libSceCommonDialog: the system dialog layer's initialisation (R10 sysrt, docs/R10_SYSRT.md).
// Group "sysrt-dialog" (experimental: BRIDGE unless --native sysrt-dialog or SSPC_R10_SYSRT=native).
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Runtime::CommonDialog {

/// libSceCommonDialog's error codes (SCE_COMMON_DIALOG_ERROR_*, the oracle's commondialog.h).
constexpr s32 kErrNotSystemInitialized = static_cast<s32>(0x80B80001);
constexpr s32 kErrAlreadySystemInitialized = static_cast<s32>(0x80B80002);

/// Called once, by the sceCommonDialogInitialize call that initialises the layer. While the per-dialog libraries
/// (MsgDialog, SaveDataDialog, ErrorDialog) stay the bridge's, their "system initialised" check reads the bridge's own
/// flag: the bridge sets this listener and mirrors the state there (src/bridge/shadps4/sysrt_host.cpp).
using InitializeListener = void (*)();
void SetInitializeListener(InitializeListener listener);

/// The layer is initialised (sceCommonDialogInitialize succeeded once in this process).
bool Initialized();

std::span<const Export> CommonDialogExports();

} // namespace SecondSon::Runtime::CommonDialog
