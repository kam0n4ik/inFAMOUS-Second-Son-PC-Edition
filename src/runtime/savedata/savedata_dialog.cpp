// SecondSonPC — libSceSaveDataDialog (see savedata_dialog.h). The five functions the game imports: Initialize, Open,
// UpdateStatus, GetResult, Terminate. The structures and the result are the oracle's (savedatadialog_ui.h/.cpp).
// Differences from the oracle, from what the game does with the result (eboot.bin):
//   - a "No" of a Yes/No message is result OK with button NO, and Back (cancel) is USER_CANCELED. The oracle gives
//     USER_CANCELED for "No" below firmware 4.50 (this game's SDK is 4.00), but the game tells the three apart:
//     0x1e5882..0x1e58a1 returns -1 for USER_CANCELED, 1 for YES and 0 for anything else, and its "Do you want to save
//     before loading?" (0x1e6d7c) goes on to the load list on 0 (0x1e6e96) and abandons the load on -1 (0x1e6ea5); its
//     overwrite question (0x1e59c0) goes back to the save list on 0 (0x1e6e3b) and leaves on -1;
//   - an OK-only message answers OK (button OK) at every firmware: the game ignores those results (0x1e5ca0, 0x1e5e10);
//   - DATAHEAD / DATATAIL focus the first / last save, not the list's first / last item (the new item);
//   - a game's items pointer may be null for a message (the oracle reads it unconditionally).
#include "runtime/savedata/savedata_dialog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/psf/param_sfo.h"
#include "runtime/savedata/savedata.h"
#include "runtime/system/common_dialog.h"

namespace SecondSon::SaveData::Dialog {

namespace {

namespace fs = std::filesystem;

// SCE_COMMON_DIALOG_ERROR_* (the oracle's commondialog.h)
enum Err : u32 {
    kOk = 0,
    kNotSystemInitialized = 0x80B80001,
    kNotInitialized = 0x80B80003,
    kAlreadyInitialized = 0x80B80004,
    kNotFinished = 0x80B80005,
    kInvalidState = 0x80B80006,
    kBusy = 0x80B80008,
    kParamInvalid = 0x80B8000A,
    kArgNull = 0x80B8000D,
};
constexpr u32 kResultOk = 0, kResultUserCanceled = 1;
constexpr u32 kButtonInvalid = 0, kButtonOk = 1, kButtonNo = 2;
constexpr u32 kOptionBackDisable = 1;
enum class FocusPos : u32 { ListHead = 0, ListTail = 1, DataHead = 2, DataTail = 3, DataLatest = 4, DataOldest = 5, DirName = 6 };

// ---- the guest's structures (the console's layout, the oracle's savedatadialog_ui.h)

struct DirName {
    char data[32];
};
struct TitleId {
    char data[10];
    char pad[6];
};
struct SaveParam {
    char title[128];
    char sub_title[128];
    char detail[1024];
    u32 user_param;
    s32 pad;
    s64 mtime;
    u8 reserved[32];
};
struct BaseParam {
    u64 size;
    u8 reserved[36];
    u32 magic;
};
struct NewItem {
    const char* title;
    const void* icon_buf;
    u64 icon_size;
    u8 reserved[32];
};
struct Items {
    s32 user_id;
    s32 pad0;
    const TitleId* title_id;
    const DirName* dir_name;
    u32 dir_name_num;
    s32 pad1;
    const NewItem* new_item;
    u32 focus_pos;
    s32 pad2;
    const DirName* focus_dir_name;
    u32 item_style;
    u8 reserved[32];
};
struct UserMsgParam {
    u32 button_type;
    u32 msg_type; // 0 normal, 1 error
    const char* msg;
    u8 reserved[32];
};
struct SysMsgParam {
    u32 msg_type;
    s32 pad;
    u64 value;
    u8 reserved[32];
};
struct ErrorCodeParam {
    u32 error_code;
    u8 reserved[32];
};
struct ProgressBarParam {
    u32 bar_type;
    s32 pad;
    const char* msg;
    u32 sys_msg_type;
    u8 reserved[28];
};
struct OptionParam {
    u32 back;
    u8 reserved[32];
};
struct DialogParam {
    BaseParam base;
    s32 size;
    u32 mode;
    u32 disp_type;
    s32 pad;
    const void* anim_param;
    const Items* items;
    const UserMsgParam* user_msg;
    const SysMsgParam* sys_msg;
    const ErrorCodeParam* error_code;
    const ProgressBarParam* progress_bar;
    void* user_data;
    const OptionParam* option;
    u8 reserved[24];
};
struct DialogResult {
    u32 mode;
    u32 result;
    u32 button_id;
    s32 pad;
    DirName* dir_name;
    SaveParam* param;
    void* user_data;
    u8 reserved[32];
};
static_assert(sizeof(BaseParam) == 0x30 && sizeof(SaveParam) == 1328 && sizeof(NewItem) == 56 &&
              sizeof(Items) == 0x60 && sizeof(UserMsgParam) == 48 && sizeof(SysMsgParam) == 48 &&
              sizeof(DialogParam) == 0x98 && sizeof(DialogResult) == 0x48);
static_assert(offsetof(DialogParam, size) == 0x30 && offsetof(DialogParam, items) == 0x48 &&
              offsetof(DialogParam, user_data) == 0x70 && offsetof(Items, new_item) == 0x20 &&
              offsetof(Items, item_style) == 0x38 && offsetof(DialogResult, dir_name) == 0x10);

template <size_t N>
std::string Text(const char (&s)[N]) {
    return {s, strnlen(s, N)};
}

template <size_t N>
void PutText(char (&dst)[N], std::string_view s) {
    std::memset(dst, 0, N);
    std::memcpy(dst, s.data(), std::min(s.size(), N - 1));
}

// ---- state

std::mutex g_mutex;
Status g_status = Status::None;
std::optional<Request> g_request;  // the running dialog
u64 g_next_id = 1;
void* g_user_data = nullptr;
struct Result {
    u32 mode = 0;
    u32 result = kResultOk;
    u32 button = kButtonInvalid;
    std::string dir_name;
    std::optional<Item> param; // the save chosen in a list (its param.sfo values)
};
Result g_result;
std::atomic<Host> g_host{nullptr};

std::string_view ModeName(Mode m) {
    switch (m) {
    case Mode::List: return "LIST";
    case Mode::UserMsg: return "USER_MSG";
    case Mode::SystemMsg: return "SYSTEM_MSG";
    case Mode::ErrorCode: return "ERROR_CODE";
    case Mode::ProgressBar: return "PROGRESS_BAR";
    default: return "INVALID";
    }
}

std::string_view TypeName(Type t) {
    switch (t) {
    case Type::Save: return "SAVE";
    case Type::Load: return "LOAD";
    case Type::Delete: return "DELETE";
    default: return "?";
    }
}

std::vector<u8> ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

u64 FolderSize(const fs::path& dir) {
    u64 size = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            size += e.file_size(ec);
        }
    }
    return size;
}

s64 Mtime(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    if (ec) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
        .count();
}

/// A save as the dialog shows it; nullopt when the folder has no param.sfo (the oracle skips it).
std::optional<Item> ReadSave(s32 user, std::string_view title, std::string_view dir) {
    const fs::path path = SaveDir(user, title, dir);
    const fs::path sfo_path = path / "sce_sys" / "param.sfo";
    std::error_code ec;
    if (!fs::exists(sfo_path, ec)) {
        return std::nullopt;
    }
    Item item;
    item.dir_name = std::string(dir);
    Psf::ParamSfo sfo;
    if (std::string error; !sfo.Load(sfo_path, error)) {
        SSPC_LOG(SaveData, Warning, "save data dialog: {}", error);
    }
    item.title = sfo.GetString("MAINTITLE").value_or("Unknown");
    item.subtitle = sfo.GetString("SUBTITLE").value_or("");
    item.detail = sfo.GetString("DETAIL").value_or("");
    item.user_param = sfo.GetInteger("SAVEDATA_LIST_PARAM").value_or(0);
    item.mtime = Mtime(sfo_path);
    item.size_bytes = FolderSize(path);
    item.corrupted = fs::exists(path / "sce_sys" / "corrupted", ec);
    item.icon_png = ReadAll(path / "sce_sys" / "icon0.png");
    return item;
}

/// The request of a valid DialogParam (the caller checked mode and the pointers its mode needs).
Request Build(const DialogParam& p) {
    Request r;
    r.mode = static_cast<Mode>(p.mode);
    r.type = static_cast<Type>(p.disp_type);
    if (p.option != nullptr) {
        r.back_enabled = p.option->back != kOptionBackDisable;
    }
    const Items* items = p.items;
    if (items != nullptr) {
        r.item_style = items->item_style;
        const std::string title = items->title_id != nullptr ? Text(items->title_id->data) : std::string{};
        if (r.mode == Mode::List && r.type == Type::Save && items->new_item != nullptr) {
            Item n;
            n.is_new = true;
            const NewItem& ni = *items->new_item;
            n.title = ni.title != nullptr ? std::string(ni.title) : std::string{};
            if (ni.icon_buf != nullptr && ni.icon_size != 0 && ni.icon_size <= (16u << 20)) {
                const auto* b = static_cast<const u8*>(ni.icon_buf);
                n.icon_png.assign(b, b + ni.icon_size);
            } else if (const auto icon = Current().icon; !icon.empty()) {
                n.icon_png = ReadAll(icon);
            }
            r.items.push_back(std::move(n));
        }
        const size_t first_save = r.items.size();
        for (u32 i = 0; items->dir_name != nullptr && i < items->dir_name_num; ++i) {
            const std::string dir = Text(items->dir_name[i].data);
            if (dir.empty()) {
                continue;
            }
            if (auto item = ReadSave(items->user_id, title, dir)) {
                r.items.push_back(std::move(*item));
            }
            if (r.mode != Mode::List && r.items.size() > first_save) {
                break; // a message shows the one save it is about
            }
        }
        // the focus (LIST)
        const size_t saves = r.items.size() - first_save;
        switch (static_cast<FocusPos>(items->focus_pos)) {
        case FocusPos::ListHead:
            r.focus = 0;
            break;
        case FocusPos::ListTail:
            r.focus = r.items.empty() ? 0 : r.items.size() - 1;
            break;
        case FocusPos::DataHead:
            r.focus = saves != 0 ? first_save : 0;
            break;
        case FocusPos::DataTail:
            r.focus = saves != 0 ? r.items.size() - 1 : 0;
            break;
        case FocusPos::DataLatest:
        case FocusPos::DataOldest: {
            const bool latest = static_cast<FocusPos>(items->focus_pos) == FocusPos::DataLatest;
            r.focus = 0;
            for (size_t i = first_save; i < r.items.size(); ++i) {
                const bool better = r.focus < first_save ||
                                    (latest ? r.items[i].mtime > r.items[r.focus].mtime
                                            : r.items[i].mtime < r.items[r.focus].mtime);
                if (better) {
                    r.focus = i;
                }
            }
            break;
        }
        case FocusPos::DirName:
            r.focus = 0;
            if (items->focus_dir_name != nullptr) {
                const std::string want = Text(items->focus_dir_name->data);
                for (size_t i = 0; i < r.items.size(); ++i) {
                    if (r.items[i].dir_name == want) {
                        r.focus = i;
                        break;
                    }
                }
            }
            break;
        default:
            r.focus = 0;
            break;
        }
    }
    switch (r.mode) {
    case Mode::UserMsg:
        r.buttons = static_cast<ButtonType>(p.user_msg->button_type);
        r.error_text = p.user_msg->msg_type == 1;
        r.text = p.user_msg->msg != nullptr ? std::string(p.user_msg->msg) : std::string{};
        break;
    case Mode::SystemMsg:
        r.sys = static_cast<SysMsg>(p.sys_msg->msg_type);
        r.sys_value = p.sys_msg->value;
        if (r.sys == SysMsg::Progress && p.option == nullptr) {
            r.back_enabled = false; // the progress message shows Cancel only when the game asks for it
        }
        break;
    case Mode::ErrorCode:
        r.error_code = p.error_code->error_code;
        break;
    case Mode::ProgressBar:
        r.text = p.progress_bar->msg != nullptr ? std::string(p.progress_bar->msg) : std::string{};
        r.back_enabled = false;
        break;
    default:
        break;
    }
    return r;
}

std::string Describe(const Request& r) {
    std::string s = fmt::format("{} {}", ModeName(r.mode), TypeName(r.type));
    switch (r.mode) {
    case Mode::List: {
        s += fmt::format(": {} item(s)", r.items.size());
        for (const auto& i : r.items) {
            s += i.is_new ? " [new]" : fmt::format(" {}", i.dir_name);
        }
        if (!r.items.empty()) {
            s += fmt::format(", focus {}", r.items[r.focus].is_new ? std::string("[new]") : r.items[r.focus].dir_name);
        }
        break;
    }
    case Mode::UserMsg:
        s += fmt::format(": buttons {}, \"{}\"", static_cast<u32>(r.buttons), r.text);
        break;
    case Mode::SystemMsg:
        s += fmt::format(": message {}{}", static_cast<u32>(r.sys),
                         r.sys_value != 0 ? fmt::format(" (value {})", r.sys_value) : std::string{});
        break;
    case Mode::ErrorCode:
        s += fmt::format(": error {:#x}", r.error_code);
        break;
    default:
        break;
    }
    if (r.mode != Mode::List && !r.items.empty()) {
        s += fmt::format(" ({})", r.items.front().dir_name);
    }
    return s;
}

/// Finishes the running dialog (g_mutex held).
void FinishLocked(u32 result, u32 button, std::string dir_name, std::optional<Item> param) {
    g_result.mode = g_request ? static_cast<u32>(g_request->mode) : 0;
    g_result.result = result;
    g_result.button = button;
    g_result.dir_name = std::move(dir_name);
    g_result.param = std::move(param);
    g_request.reset();
    g_status = Status::Finished;
}

void NotifyHost() {
    if (const Host host = g_host.load(std::memory_order_acquire)) {
        host();
    }
}

// ---- the API

SSPC_SYSV_ABI u32 Initialize() {
    std::scoped_lock lock{g_mutex};
    if (!CommonDialog::LayerInitialized()) {
        return kNotSystemInitialized;
    }
    if (g_status != Status::None) {
        return kAlreadyInitialized;
    }
    if (!CommonDialog::TryUse()) {
        return kBusy;
    }
    g_status = Status::Initialized;
    return kOk;
}

SSPC_SYSV_ABI u32 Open(const DialogParam* param) {
    {
        std::scoped_lock lock{g_mutex};
        if (g_status != Status::Initialized && g_status != Status::Finished) {
            SSPC_LOG(SaveData, Warning, "sceSaveDataDialogOpen in status {}: refused", static_cast<u32>(g_status));
            return kInvalidState;
        }
    }
    if (param == nullptr) {
        return kArgNull;
    }
    const Mode mode = static_cast<Mode>(param->mode);
    const bool needs_items = mode == Mode::List;
    const bool ok = param->size == static_cast<s32>(sizeof(DialogParam)) && param->base.size == sizeof(BaseParam) &&
                    param->mode >= 1 && param->mode <= 5 && param->disp_type >= 1 && param->disp_type <= 3 &&
                    (!needs_items || param->items != nullptr) &&
                    (mode != Mode::UserMsg || param->user_msg != nullptr) &&
                    (mode != Mode::SystemMsg || param->sys_msg != nullptr) &&
                    (mode != Mode::ErrorCode || param->error_code != nullptr) &&
                    (mode != Mode::ProgressBar || param->progress_bar != nullptr);
    if (!ok) {
        SSPC_LOG(SaveData, Error, "sceSaveDataDialogOpen: invalid parameters (size {:#x}, mode {}, type {})",
                 param->size, param->mode, param->disp_type);
        return kParamInvalid;
    }
    Request request = Build(*param); // (reads the saves' files: outside the lock)
    u64 id = 0;
    {
        std::scoped_lock lock{g_mutex};
        if (g_status != Status::Initialized && g_status != Status::Finished) {
            return kInvalidState;
        }
        id = request.id = g_next_id++;
        g_result = {};
        g_user_data = param->user_data;
        SSPC_LOG(SaveData, Info, "sceSaveDataDialogOpen #{} {}", id, Describe(request));
        g_request = std::move(request);
        g_status = Status::Running;
    }
    const Host host = g_host.load(std::memory_order_acquire);
    if (host == nullptr || !host()) {
        HostFailed(id);
    }
    return kOk;
}

SSPC_SYSV_ABI u32 UpdateStatus() {
    std::scoped_lock lock{g_mutex};
    return static_cast<u32>(g_status);
}

SSPC_SYSV_ABI u32 GetResult(DialogResult* result) {
    std::scoped_lock lock{g_mutex};
    if (g_status != Status::Finished) {
        return kNotFinished;
    }
    if (result == nullptr) {
        return kArgNull;
    }
    result->mode = g_result.mode;
    result->result = g_result.result;
    result->button_id = g_result.button;
    if (result->dir_name != nullptr) {
        PutText(result->dir_name->data, g_result.dir_name);
    }
    if (result->param != nullptr && g_result.param.has_value()) {
        SaveParam& p = *result->param;
        std::memset(&p, 0, sizeof(p));
        PutText(p.title, g_result.param->title);
        PutText(p.sub_title, g_result.param->subtitle);
        PutText(p.detail, g_result.param->detail);
        p.user_param = g_result.param->user_param;
        p.mtime = g_result.param->mtime;
    }
    result->user_data = g_user_data;
    SSPC_LOG(SaveData, Info, "sceSaveDataDialogGetResult: mode {}, result {}, button {}, dir \"{}\"", g_result.mode,
             g_result.result == kResultOk ? "OK" : "USER_CANCELED", g_result.button, g_result.dir_name);
    return kOk;
}

SSPC_SYSV_ABI u32 Terminate() {
    bool closed = false;
    {
        std::scoped_lock lock{g_mutex};
        if (g_status == Status::Running) {
            // (sceSaveDataDialogClose, as the oracle: finished without an answer)
            FinishLocked(kResultOk, kButtonInvalid, {}, std::nullopt);
            closed = true;
        }
        if (g_status == Status::None) {
            return kNotInitialized;
        }
        g_status = Status::None;
        g_result = {};
        CommonDialog::EndUse();
    }
    if (closed) {
        SSPC_LOG(SaveData, Warning, "sceSaveDataDialogTerminate closed a dialog the user had not answered");
        NotifyHost();
    }
    return kOk;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define SDD(nid, name, fn)                                                                                             \
    Runtime::Export{nid, "libSceSaveDataDialog", "libSceSaveDataDialog", name, Fn(&fn), nullptr, "savedata"}
const Runtime::Export kExports[] = {
    SDD("s9e3+YpRnzw", "sceSaveDataDialogInitialize", Initialize),
    SDD("4tPhsP6FpDI", "sceSaveDataDialogOpen", Open),
    SDD("KK3Bdg1RWK0", "sceSaveDataDialogUpdateStatus", UpdateStatus),
    SDD("yEiJ-qqr6Cg", "sceSaveDataDialogGetResult", GetResult),
    SDD("YuH2FA7azqQ", "sceSaveDataDialogTerminate", Terminate),
};
#undef SDD

} // namespace

std::vector<Choice> Choices(const Request& r) {
    switch (r.mode) {
    case Mode::UserMsg:
        switch (r.buttons) {
        case ButtonType::Ok: return {Choice::Ok};
        case ButtonType::YesNo: return {Choice::Yes, Choice::No};
        case ButtonType::OkCancel: return {Choice::Ok, Choice::Cancel};
        default: return {};
        }
    case Mode::SystemMsg:
        switch (r.sys) {
        case SysMsg::Confirm:
        case SysMsg::Overwrite: return {Choice::Yes, Choice::No};
        case SysMsg::Progress: return r.back_enabled ? std::vector<Choice>{Choice::Cancel} : std::vector<Choice>{};
        case SysMsg::CorruptedAndDeleted:
        case SysMsg::CorruptedAndCreated:
        case SysMsg::CorruptedAndRestore:
            return r.back_enabled ? std::vector<Choice>{Choice::Ok, Choice::Cancel} : std::vector<Choice>{Choice::Ok};
        default: return {Choice::Ok};
        }
    case Mode::ErrorCode: return {Choice::Ok};
    default: return {};
    }
}

bool CanGoBack(const Request& r) {
    switch (r.mode) {
    case Mode::List: return r.back_enabled;
    case Mode::UserMsg: return r.buttons != ButtonType::None;
    case Mode::SystemMsg:
        return r.sys == SysMsg::Progress ? r.back_enabled : !Choices(r).empty();
    case Mode::ErrorCode: return true;
    default: return false;
    }
}

void SetHost(Host host) {
    g_host.store(host, std::memory_order_release);
}

std::optional<Request> Running() {
    std::scoped_lock lock{g_mutex};
    if (g_status != Status::Running) {
        return std::nullopt;
    }
    return g_request;
}

bool Respond(u64 id, const Answer& a) {
    std::string what;
    {
        std::scoped_lock lock{g_mutex};
        if (g_status != Status::Running || !g_request || g_request->id != id) {
            return false;
        }
        const Request& r = *g_request;
        const std::string about = r.mode != Mode::List && !r.items.empty() ? r.items.front().dir_name : std::string{};
        switch (a.kind) {
        case Answer::Kind::Item: {
            if (r.mode != Mode::List || a.item >= r.items.size()) {
                return false;
            }
            Item chosen = r.items[a.item];
            what = chosen.is_new ? std::string("the new save") : fmt::format("save {}", chosen.dir_name);
            const std::string dir = chosen.dir_name;
            FinishLocked(kResultOk, kButtonInvalid, dir,
                         chosen.is_new ? std::nullopt : std::optional<Item>(std::move(chosen)));
            break;
        }
        case Answer::Kind::Choice: {
            const auto choices = Choices(r);
            if (std::find(choices.begin(), choices.end(), a.choice) == choices.end()) {
                return false;
            }
            switch (a.choice) {
            case Choice::Ok:
            case Choice::Yes:
                what = a.choice == Choice::Ok ? "OK" : "Yes";
                FinishLocked(kResultOk, kButtonOk, about, std::nullopt);
                break;
            case Choice::No:
                what = "No";
                FinishLocked(kResultOk, kButtonNo, about, std::nullopt);
                break;
            case Choice::Cancel:
                what = "Cancel";
                FinishLocked(kResultUserCanceled, kButtonInvalid, about, std::nullopt);
                break;
            }
            break;
        }
        case Answer::Kind::Back:
            if (!CanGoBack(r)) {
                return false;
            }
            what = "Back";
            FinishLocked(kResultUserCanceled, kButtonInvalid, r.mode == Mode::List ? std::string{} : about,
                         std::nullopt);
            break;
        }
        SSPC_LOG(SaveData, Info, "save data dialog #{}: the user chose {}", id, what);
    }
    NotifyHost();
    return true;
}

void HostFailed(u64 id) {
    {
        std::scoped_lock lock{g_mutex};
        if (g_status != Status::Running || !g_request || g_request->id != id) {
            return;
        }
        SSPC_LOG(SaveData, Error,
                 "save data dialog #{} cannot be shown (no window or overlay in this run): finished as cancelled by "
                 "the user - nothing was chosen, saved or loaded",
                 id);
        FinishLocked(kResultUserCanceled, kButtonInvalid, {}, std::nullopt);
    }
    NotifyHost();
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::SaveData::Dialog
