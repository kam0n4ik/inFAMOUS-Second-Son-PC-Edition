// SecondSonPC — self-test of the save data dialog with SecondSonPC's libSceSaveData (see save_data_dialog.h): the
// game's calls, laid out as eboot.bin lays them out (0x1e5720..0x1e6497: the parameter block at object + 8, its items
// at + 0xc8, the result on the stack), and the answers given through the view as a user gives them. Own code.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <thread>

#include <fmt/format.h>

#include "runtime/exports.h"
#include "runtime/savedata/savedata.h"
#include "runtime/savedata/savedata_dialog.h"
#include "ui/save_data_dialog.h"

namespace SecondSon::Ui {

namespace {

namespace fs = std::filesystem;
namespace D = SaveData::Dialog;

// ---- the guest's structures, as the game fills them

struct DirName {
    char data[32];
};
struct MountPoint {
    char data[16];
};
struct Mount2 {
    s32 user_id;
    s32 pad0;
    const DirName* dir_name;
    u64 blocks;
    u32 mount_mode;
    u8 reserved[32];
    s32 pad1;
};
struct MountResult {
    MountPoint mount_point;
    u64 required_blocks;
    u32 unused;
    u32 mount_status;
    u8 reserved[28];
    s32 pad;
};
struct SearchCond {
    s32 user_id;
    s32 pad;
    const void* title_id;
    const DirName* dir_name;
    u32 key;
    u32 order;
    u8 reserved[32];
};
struct SearchResult {
    u32 hit_num;
    s32 pad0;
    DirName* dir_names;
    u32 dir_names_num;
    u32 set_num;
    void* params;
    void* infos;
    u8 reserved[12];
    s32 pad1;
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
struct NewItem {
    const char* title;
    const void* icon_buf;
    u64 icon_size;
    u8 reserved[32];
};
struct Items {
    s32 user_id;
    s32 pad0;
    const void* title_id;
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
struct UserMsg {
    u32 button_type;
    u32 msg_type;
    const char* msg;
    u8 reserved[32];
};
struct SysMsg {
    u32 msg_type;
    s32 pad;
    u64 value;
    u8 reserved[32];
};
struct ErrorCode {
    u32 code;
    u8 reserved[32];
};
struct DialogParam {
    u64 base_size;
    u8 base_reserved[36];
    u32 base_magic;
    s32 size;
    u32 mode;
    u32 disp_type;
    s32 pad;
    const void* anim;
    const Items* items;
    const UserMsg* user_msg;
    const SysMsg* sys_msg;
    const ErrorCode* error_code;
    const void* progress_bar;
    void* user_data;
    const void* option;
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
// (the offsets eboot.bin writes: 0x1e5780 size, 0x1e5817 mode, 0x1e57ea type, 0x1e57f8 items, 0x1e581e user message,
// 0x1e5bc8 system message, 0x1e5f73 error code; items 0x1e6275 dir names, 0x1e6283 count, 0x1e6380 new item,
// 0x1e628a focus, 0x1e6295 style; result 0x1e6436 result, 0x1e6458 button, 0x1e63e1 dir name, 0x1e63ef param)
static_assert(sizeof(DialogParam) == 0x98 && offsetof(DialogParam, size) == 0x30 && offsetof(DialogParam, mode) == 0x34 &&
              offsetof(DialogParam, disp_type) == 0x38 && offsetof(DialogParam, items) == 0x48 &&
              offsetof(DialogParam, user_msg) == 0x50 && offsetof(DialogParam, sys_msg) == 0x58 &&
              offsetof(DialogParam, error_code) == 0x60);
static_assert(offsetof(Items, dir_name) == 0x10 && offsetof(Items, dir_name_num) == 0x18 &&
              offsetof(Items, new_item) == 0x20 && offsetof(Items, focus_pos) == 0x28 &&
              offsetof(Items, item_style) == 0x38 && offsetof(NewItem, icon_buf) == 8 && offsetof(NewItem, icon_size) == 16);
static_assert(sizeof(DialogResult) == 0x48 && offsetof(DialogResult, result) == 4 &&
              offsetof(DialogResult, button_id) == 8 && offsetof(DialogResult, dir_name) == 0x10 &&
              offsetof(DialogResult, param) == 0x18 && sizeof(SaveParam) == 0x530);
static_assert(sizeof(Mount2) == 64 && sizeof(MountResult) == 64 && sizeof(SearchCond) == 64 &&
              sizeof(SearchResult) == 56);

constexpr u32 kList = 1, kUserMsg = 2, kSystemMsg = 3, kErrorCode = 4;
constexpr u32 kSave = 1, kLoad = 2;
constexpr u32 kStatusRunning = 2, kStatusFinished = 3;
constexpr s32 kUser = 1000;

const void* Native(std::string_view nid) {
    for (const auto& e : Runtime::Exports()) {
        if (nid == e.nid && e.original == nullptr) {
            return e.function;
        }
    }
    return nullptr;
}

template <typename R, typename... A>
R Call(const void* f, A... a) {
    using Fn = R(SSPC_SYSV_ABI*)(A...);
    return reinterpret_cast<Fn>(const_cast<void*>(f))(a...);
}

struct Api {
    const void* sd_init = Native("TywrFKCoLGY");
    const void* mount = Native("0z45PIH+SNI");
    const void* umount = Native("BMR4F-Uek3E");
    const void* search = Native("dyIhnXq-0SM");
    const void* set_param = Native("85zul--eGXs");
    const void* cd_init = Native("uoUpLGNkygk");
    const void* init = Native("s9e3+YpRnzw");
    const void* open = Native("4tPhsP6FpDI");
    const void* update = Native("KK3Bdg1RWK0");
    const void* get_result = Native("yEiJ-qqr6Cg");
    const void* terminate = Native("YuH2FA7azqQ");
    bool Ok() const {
        return sd_init && mount && umount && search && set_param && cd_init && init && open && update && get_result &&
               terminate;
    }
    u32 Init() const {
        return Call<u32>(init);
    }
    u32 Open(const DialogParam* p) const {
        return Call<u32>(open, p);
    }
    u32 Update() const {
        return Call<u32>(update);
    }
    u32 Result(DialogResult* r) const {
        return Call<u32>(get_result, r);
    }
    u32 Terminate() const {
        return Call<u32>(terminate);
    }
};

DirName Name(std::string_view s) {
    DirName d{};
    std::memcpy(d.data, s.data(), std::min(s.size(), sizeof(d.data) - 1));
    return d;
}

std::atomic<u32> g_host_calls{0};
bool TestHost() {
    g_host_calls.fetch_add(1);
    return true;
}

/// A save written as the game writes one (0x1e7ae7 Mount2, the file, 0x1e7d7d SetParam, 0x1e7dfc Umount).
std::string WriteSave(const Api& api, std::string_view dir, std::string_view title, std::string_view subtitle,
                      u32 mode) {
    const DirName name = Name(dir);
    Mount2 m{};
    m.user_id = kUser;
    m.dir_name = &name;
    m.blocks = 0x60;
    m.mount_mode = mode;
    MountResult mr{};
    const u32 r = Call<u32>(api.mount, &m, &mr);
    if (r != 0) {
        return fmt::format("Mount2 {} mode {:#x} -> {:#x}", dir, mode, r);
    }
    std::ofstream(SaveData::SaveDir(kUser, "", dir) / "dis.dat", std::ios::binary) << std::string(4096, 'x');
    SaveParam p{};
    std::memcpy(p.title, title.data(), title.size());
    std::memcpy(p.sub_title, subtitle.data(), subtitle.size());
    if (const u32 sp = Call<u32>(api.set_param, &mr.mount_point, 0u, static_cast<const void*>(&p), u64{sizeof(p)});
        sp != 0) {
        return fmt::format("SetParam -> {:#x}", sp);
    }
    if (const u32 u = Call<u32>(api.umount, &mr.mount_point); u != 0) {
        return fmt::format("Umount -> {:#x}", u);
    }
    return {};
}

struct Dialog {
    DialogParam p{};
    Items items{};
    UserMsg user{};
    SysMsg sys{};
    ErrorCode err{};
    NewItem new_item{};
    Dialog(u32 mode, u32 type) {
        p.base_size = 0x30;
        p.size = sizeof(DialogParam);
        p.mode = mode;
        p.disp_type = type;
        items.user_id = kUser;
        p.items = &items;
    }
};

/// The view as the host keeps it: opened on the running request, input, the answer to the runtime.
struct User {
    SaveDataDialogView view;
    EstimateMeasure measure;
    DrawList list;
    std::optional<D::Request> Wait() {
        for (int i = 0; i < 400; ++i) {
            if (auto r = D::Running()) {
                view.Open(*r, {});
                view.SetEnv({.width = 1920, .height = 1080, .ru = true});
                view.Build(measure, list);
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }
    bool Answer() {
        view.Build(measure, list);
        const auto a = view.TakeAnswer();
        return a.has_value() && D::Respond(view.Id(), *a);
    }
};

} // namespace

void RunSaveDataDialogChecks(const SaveDialogCheckFn& check, const fs::path& scratch) {
    const Api api;
    check("SecondSonPC exports libSceSaveData and libSceSaveDataDialog (the five functions the game imports)", api.Ok(),
          "");
    if (!api.Ok()) {
        return;
    }
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const SaveData::Settings before = SaveData::Current();
    SaveData::Settings s;
    s.home = scratch / "home";
    s.title_dir = "CUSA00004";
    s.firmware = 0x4000000; // the game's SDK (eboot SDK version 0x4008061)
    s.language_id = 8;
    SaveData::Configure(s);
    Call<u32>(api.sd_init, static_cast<void*>(nullptr));
    Call<s32>(api.cd_init); // (the layer once per process: 0 or "already initialised")

    // ---- no host (an automated run): the dialog cannot be shown - finished at once as cancelled, never left running
    D::SetHost(nullptr);
    {
        Dialog d(kList, kLoad);
        const u32 i = api.Init(), o = api.Open(&d.p), st = api.Update();
        DialogResult r{};
        DirName dn = Name("unchanged");
        r.dir_name = &dn;
        const u32 g = api.Result(&r);
        check("no host: Open succeeds and the dialog is FINISHED at once (the game's 250 ms poll ends)",
              i == 0 && o == 0 && st == kStatusFinished, fmt::format("init {:#x} open {:#x} status {}", i, o, st));
        check("no host: the result is USER_CANCELED with no save chosen (nothing saved or loaded for the user)",
              g == 0 && r.result == 1 && dn.data[0] == 0, fmt::format("get {:#x} result {} dir '{}'", g, r.result, dn.data));
        check("Terminate after it", api.Terminate() == 0, "");
    }

    D::SetHost(&TestHost);
    // ---- saves as the game writes them: an autosave and a manual save in slot User0 (0x1e5a32: "infamousss" + User/Auto + n)
    const std::string w1 = WriteSave(api, "infamousssAuto0", "Seattle", "Autosave", 4 | 2);
    const std::string w2 = WriteSave(api, "infamousssUser0", "Seattle", "Manual", 4 | 2);
    check("Mount2 (create) + SetParam + Umount write two saves", w1.empty() && w2.empty(), w1 + w2);
    {
        // the manual save is the newer one
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(SaveData::SaveDir(kUser, "", "infamousssAuto0") / "sce_sys" / "param.sfo",
                            now - std::chrono::hours(2), ec);
        fs::last_write_time(SaveData::SaveDir(kUser, "", "infamousssUser0") / "sce_sys" / "param.sfo",
                            now - std::chrono::hours(1), ec);
    }
    DirName found[8]{};
    {
        const DirName pattern = Name("infamousss%");
        SearchCond c{};
        c.user_id = kUser;
        c.dir_name = &pattern;
        SearchResult sr{};
        sr.dir_names = found;
        sr.dir_names_num = 8;
        const u32 r = Call<u32>(api.search, &c, &sr);
        check("DirNameSearch infamousss% finds both", r == 0 && sr.hit_num == 2, fmt::format("{:#x}, {} hits", r, sr.hit_num));
    }

    // ---- Load (0x1e6a70 -> the list 0x1e6140 with type LOAD, focus DATALATEST): the game's thread polls as it does
    {
        Dialog d(kList, kLoad);
        d.items.dir_name = found;
        d.items.dir_name_num = 2;
        d.items.focus_pos = 4;
        std::atomic<bool> done{false};
        DialogResult r{};
        DirName dn{};
        SaveParam param{};
        r.dir_name = &dn;
        r.param = &param;
        u32 polls = 0, get = 0xFFFFFFFF;
        std::thread game([&] {
            api.Init();
            api.Open(&d.p);
            while (api.Update() != kStatusFinished) { // (the game: usleep(250000) between two)
                ++polls;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            get = api.Result(&r);
            api.Terminate();
            done = true;
        });
        User u;
        const auto req = u.Wait();
        const bool listed = req && req->items.size() == 2 && req->items[req->focus].dir_name == "infamousssUser0" &&
                            req->items[0].title == "Seattle" && req->items[0].size_bytes >= 4096;
        check("Load: the host is told, the list shows both saves (title, size) with the newest focused", listed,
              req ? fmt::format("{} items, focus {}", req->items.size(), req->focus) : std::string("no dialog"));
        const bool running = api.Update() == kStatusRunning;
        u.view.OnNav(Nav::Accept); // the focused (newest) save
        const bool answered = u.Answer();
        game.join();
        check("Load: RUNNING while the user looks, FINISHED once a save is chosen; the game's loop ends",
              running && answered && done && polls > 0, fmt::format("polls {}", polls));
        check("Load: the result names the chosen save and its param (0x1e6467: the slot from the name)",
              get == 0 && r.result == 0 && std::string_view(dn.data) == "infamousssUser0" &&
                  std::string_view(param.title) == "Seattle" && std::string_view(param.sub_title) == "Manual",
              fmt::format("get {:#x} result {} dir '{}' title '{}'", get, r.result, dn.data, param.title));
    }

    // ---- Save (the list with type SAVE and a new item for the first free slot, 0x1e6302..0x1e6395)
    {
        Dialog d(kList, kSave);
        d.items.dir_name = found;
        d.items.dir_name_num = 2;
        d.items.focus_pos = 4;
        d.items.new_item = &d.new_item;
        api.Init();
        api.Open(&d.p);
        User u;
        const auto req = u.Wait();
        check("Save: the list has the new item first, then the saves",
              req && req->items.size() == 3 && req->items[0].is_new && !req->items[1].is_new, "");
        for (int i = 0; i < 4; ++i) {
            u.view.OnNav(Nav::Up);
        }
        u.view.OnNav(Nav::Accept);
        const bool answered = u.Answer();
        DialogResult r{};
        DirName dn = Name("x");
        SaveParam param{};
        std::memcpy(param.title, "kept", 5);
        r.dir_name = &dn;
        r.param = &param;
        const u32 g = api.Result(&r);
        check("Save: the new item gives result OK with an empty dir name (the game takes its free slot) and no param",
              answered && g == 0 && r.result == 0 && dn.data[0] == 0 && std::string_view(param.title) == "kept",
              fmt::format("result {} dir '{}'", r.result, dn.data));
        api.Terminate();
    }

    // ---- the overwrite question (0x1e59c0: SYSTEM_MSG OVERWRITE about the chosen slot): Yes / No / Back
    for (const auto& [nav, want_result, want_button, what] :
         {std::tuple{std::vector<Nav>{Nav::Accept}, 0u, 1u, "Yes: OK + YES (the game saves)"},
          std::tuple{std::vector<Nav>{Nav::Right, Nav::Accept}, 0u, 2u, "No: OK + NO (the game shows the list again)"},
          std::tuple{std::vector<Nav>{Nav::Back}, 1u, 0u, "Back: USER_CANCELED (the game leaves)"}}) {
        Dialog d(kSystemMsg, kSave);
        const DirName slot = Name("infamousssUser0");
        d.items.dir_name = &slot;
        d.items.dir_name_num = 1;
        d.sys.msg_type = 3;
        d.p.sys_msg = &d.sys;
        api.Init();
        api.Open(&d.p);
        User u;
        const auto req = u.Wait();
        for (const Nav n : nav) {
            u.view.OnNav(n);
        }
        const bool answered = u.Answer();
        DialogResult r{};
        DirName dn{};
        r.dir_name = &dn;
        api.Result(&r);
        check(fmt::format("overwrite question, {}", what),
              req && req->items.size() == 1 && answered && r.result == want_result && r.button_id == want_button &&
                  std::string_view(dn.data) == "infamousssUser0",
              fmt::format("result {} button {} dir '{}'", r.result, r.button_id, dn.data));
        api.Terminate();
    }

    // ---- "Do you want to save before loading?" (0x1e6d7c, USER_MSG Yes/No): No goes on to the load list (0x1e6e96)
    {
        Dialog d(kUserMsg, kLoad);
        d.user.button_type = 1;
        d.user.msg = "Do you want to save before loading?";
        d.p.user_msg = &d.user;
        d.p.items = nullptr;
        api.Init();
        api.Open(&d.p);
        User u;
        const auto req = u.Wait();
        u.view.OnNav(Nav::Right);
        u.view.OnNav(Nav::Accept);
        const bool answered = u.Answer();
        DialogResult r{};
        api.Result(&r);
        check("save-before-loading question, No: OK + NO (0x1e5899: 0, the game loads without saving)",
              req && req->text == "Do you want to save before loading?" && answered && r.result == 0 &&
                  r.button_id == 2,
              fmt::format("result {} button {}", r.result, r.button_id));
        api.Terminate();
    }

    // ---- an error after a failed save (0x1e5e10: ERROR_CODE) and the corrupted message: OK
    for (const u32 mode : {kErrorCode, kSystemMsg}) {
        Dialog d(mode, kLoad);
        d.err.code = 0x809F000B;
        d.sys.msg_type = 6;
        d.p.error_code = &d.err;
        d.p.sys_msg = &d.sys;
        d.p.items = nullptr;
        api.Init();
        api.Open(&d.p);
        User u;
        u.Wait();
        u.view.OnNav(Nav::Accept);
        const bool answered = u.Answer();
        DialogResult r{};
        api.Result(&r);
        check(mode == kErrorCode ? "error message: OK closes it" : "corrupted message: OK closes it",
              answered && r.result == 0 && r.button_id == 1, fmt::format("result {} button {}", r.result, r.button_id));
        api.Terminate();
    }

    // ---- Back on the list, a click on a save, the mouse wheel
    {
        Dialog d(kList, kLoad);
        d.items.dir_name = found;
        d.items.dir_name_num = 2;
        api.Init();
        api.Open(&d.p);
        User u;
        u.Wait();
        u.view.OnNav(Nav::Back);
        const bool answered = u.Answer();
        DialogResult r{};
        DirName dn = Name("x");
        r.dir_name = &dn;
        api.Result(&r);
        check("list, Back: USER_CANCELED with an empty dir name (0x1e6440: -1, nothing loaded)",
              answered && r.result == 1 && dn.data[0] == 0, fmt::format("result {}", r.result));
        api.Terminate();
    }
    {
        Dialog d(kList, kLoad);
        d.items.dir_name = found;
        d.items.dir_name_num = 2;
        api.Init();
        api.Open(&d.p);
        User u;
        u.Wait();
        // the second row: below the first one (the layout of a 1920x1080 window: rows 112 px from y 246)
        u.view.OnMouseButton(900, 246 + 112 + 50, true);
        u.view.OnMouseButton(900, 246 + 112 + 50, false);
        const bool answered = u.Answer();
        DialogResult r{};
        DirName dn{};
        r.dir_name = &dn;
        api.Result(&r);
        check("list, a click on the second save chooses it", answered && r.result == 0 &&
                                                                  std::string_view(dn.data) == "infamousssUser0",
              fmt::format("dir '{}'", dn.data));
        api.Terminate();
    }

    // ---- the state machine (the oracle's errors) and a dialog the game closes itself
    {
        Dialog d(kList, kLoad);
        const u32 open_first = api.Open(&d.p);
        const u32 i1 = api.Init(), i2 = api.Init();
        api.Open(&d.p);
        DialogResult r{};
        const u32 g = api.Result(&r);
        const auto running = D::Running();
        const u32 calls = g_host_calls.load();
        const u32 t = api.Terminate();
        const bool gone = !D::Running().has_value();
        const bool stale = running && !D::Respond(running->id, {.kind = D::Answer::Kind::Back});
        check("Open before Initialize: INVALID_STATE; Initialize twice: ALREADY_INITIALIZED; GetResult while "
              "running: NOT_FINISHED",
              open_first == 0x80B80006 && i1 == 0 && i2 == 0x80B80004 && g == 0x80B80005,
              fmt::format("{:#x} {:#x} {:#x} {:#x}", open_first, i1, i2, g));
        check("Terminate while shown closes it (the host is told; a late answer is refused); Terminate again: "
              "NOT_INITIALIZED",
              t == 0 && gone && stale && g_host_calls.load() > calls && api.Terminate() == 0x80B80003, "");
    }

    D::SetHost(nullptr);
    SaveData::Configure(before);
}

} // namespace SecondSon::Ui
