// SecondSonPC — libSceSaveDataDialog (NATIVE, group "savedata" with libSceSaveData; `--bridge savedata` gives both
// back to the oracle, third_party/shadPS4/src/core/libraries/save_data/dialog/*). The system's save list and its
// messages over the game: the game's Save / Load (eboot.bin 0x1e5720..0x1e6497) open one and poll
// sceSaveDataDialogUpdateStatus every 250 ms until it is FINISHED, with no time limit. The oracle draws it with its
// ImGui layer, which exists only on the bridge's presenter (patch 0032): with SecondSonPC's GPU backend nothing drew
// it, the status stayed RUNNING, and Save / Load from the game's menu waited forever. Here the runtime keeps the
// dialog's state and answers the game; a host shows it (the menu overlay, ui/save_data_dialog.h) and gives the user's
// answer back. With no host (automated runs, no window) a dialog cannot be shown: it is finished at once as cancelled
// by the user, and the log says so - the game's own cancel path, never a choice made for the user.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::SaveData::Dialog {

enum class Mode : u32 { Invalid = 0, List = 1, UserMsg = 2, SystemMsg = 3, ErrorCode = 4, ProgressBar = 5 };
enum class Type : u32 { Save = 1, Load = 2, Delete = 3 };
enum class ButtonType : u32 { Ok = 0, YesNo = 1, None = 2, OkCancel = 3 };
/// SCE_SAVE_DATA_DIALOG_SYSMSG_TYPE_*
enum class SysMsg : u32 {
    NoData = 1,
    Confirm = 2,
    Overwrite = 3,
    NoSpace = 4,
    Progress = 5,
    FileCorrupted = 6,
    Finished = 7,
    NoSpaceContinuable = 8,
    CorruptedAndDeleted = 10,
    CorruptedAndCreated = 11,
    CorruptedAndRestore = 13,
    TotalSizeExceeded = 14,
};
/// SCE_COMMON_DIALOG_STATUS_*
enum class Status : u32 { None = 0, Initialized = 1, Running = 2, Finished = 3 };

/// A save the dialog shows (read from its folder when the dialog opened), or the list's "new save" item.
struct Item {
    std::string dir_name; ///< "" for the new item
    bool is_new = false;
    std::string title, subtitle, detail;
    u32 user_param = 0;
    s64 mtime = 0;       ///< param.sfo's last write, seconds since 1970 (UTC)
    u64 size_bytes = 0;  ///< the save folder's files
    bool corrupted = false;
    std::vector<u8> icon_png; ///< icon0.png / the game's icon for a new save / the game's save_data.png
};

/// The running dialog as the host shows it.
struct Request {
    u64 id = 0; ///< a new id for every sceSaveDataDialogOpen
    Mode mode = Mode::Invalid;
    Type type = Type::Save;
    u32 item_style = 0; ///< SCE_SAVE_DATA_DIALOG_ITEM_STYLE_*: 0 title/date-size/subtitle, 1 title/subtitle/date-size, 2 title/date-size
    std::vector<Item> items; ///< LIST: the new item first (when the game gives one), then the saves; a message: the save it is about (at most one)
    size_t focus = 0;        ///< the item focused when the list opens
    bool back_enabled = true; ///< LIST / the progress message: whether Back may close it
    // USER_MSG
    ButtonType buttons = ButtonType::Ok;
    bool error_text = false;
    std::string text;
    // SYSTEM_MSG
    SysMsg sys = SysMsg::NoData;
    u64 sys_value = 0; ///< blocks (NOSPACE*)
    // ERROR_CODE
    u32 error_code = 0;
    // PROGRESS_BAR
    u32 progress = 0; ///< percent
};

/// The buttons a message shows, left to right (a LIST has none: its items are chosen).
enum class Choice : u8 { Ok, Yes, No, Cancel };
std::vector<Choice> Choices(const Request& request);
/// Back (Esc, the controller's back button, a right click) closes the dialog as cancelled (false: it stays).
bool CanGoBack(const Request& request);

struct Answer {
    enum class Kind : u8 { Item, Choice, Back };
    Kind kind = Kind::Back;
    size_t item = 0;           ///< Kind::Item: index into Request::items
    Choice choice = Choice::Ok; ///< Kind::Choice
};

/// Called (any thread) whenever a dialog opens or is finished / closed: the host looks at Running(). Returns whether
/// the host can show dialogs at all; false at an open finishes it at once as cancelled.
using Host = bool (*)();
void SetHost(Host host);
/// The dialog waiting for the user (none when no dialog runs, or it is finished).
std::optional<Request> Running();
/// The user's answer to dialog `id` (main thread). false: it is not running any more, or the answer does not fit it
/// (Back where Back is not allowed, an item of a message).
bool Respond(u64 id, const Answer& answer);
/// The host could not show dialog `id` after all (its overlay failed): finished as cancelled.
void HostFailed(u64 id);

std::span<const Runtime::Export> Exports();

} // namespace SecondSon::SaveData::Dialog
