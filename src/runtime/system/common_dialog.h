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

/// For SecondSonPC's own dialog libraries (the save data dialog): while sceCommonDialogInitialize is the bridge's
/// (sysrt-dialog BRIDGE) the layer's state is the bridge's flag, read through `source` (src/bridge/shadps4/sysrt_host.cpp).
using StateSource = bool (*)();
void SetBridgeInitializedSource(StateSource source);
/// Initialized(), or the bridge's state when it owns sceCommonDialogInitialize.
bool LayerInitialized();
/// One system dialog at a time (the oracle's CommonDialog::g_isUsed): a dialog library takes it at its Initialize and
/// gives it back at its Terminate. While MsgDialog / ErrorDialog are the bridge's the flag is the bridge's own
/// (`flag`, set once by the bridge before the game runs), so that they and SecondSonPC's dialogs exclude each other.
void SetSharedUseFlag(bool* flag);
bool TryUse(); ///< false: another dialog library holds it (SCE_COMMON_DIALOG_ERROR_BUSY)
void EndUse();

std::span<const Export> CommonDialogExports();

} // namespace SecondSon::Runtime::CommonDialog
