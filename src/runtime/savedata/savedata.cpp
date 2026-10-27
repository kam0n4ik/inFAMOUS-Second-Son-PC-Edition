// SecondSonPC — libSceSaveData (NATIVE, group "savedata"; `--bridge savedata` gives it back to the oracle,
// third_party/shadPS4/src/core/libraries/save_data/savedata.cpp + save_instance.cpp, whose behaviour this is for the
// game's SDK firmware 4.00). The eight functions the game imports: Initialize3, Terminate, Mount2, Umount,
// DirNameSearch, SetParam, SaveIcon, Delete. The files are the oracle's (savedata.h). One deliberate difference: a
// save left marked "corrupted" (the game stopped while it was mounted for writing) with no backup still has its
// param.sfo read - the oracle then mounts it with an empty one and writes that back at the unmount.
#include "runtime/savedata/savedata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/filesystem/vfs.h"
#include "runtime/psf/param_sfo.h"

namespace SecondSon::SaveData {

namespace {

namespace fs = std::filesystem;

enum Err : u32 {
    kOk = 0,
    kParameter = 0x809F0000,
    kNotInitialized = 0x809F0001,
    kBusy = 0x809F0003,
    kNotMounted = 0x809F0004,
    kExists = 0x809F0007,
    kNotFound = 0x809F0008,
    kNoSpaceFs = 0x809F000A,
    kInternal = 0x809F000B,
    kMountFull = 0x809F000C,
    kBadMounted = 0x809F000D,
    kBroken = 0x809F000F,
    kInvalidLoginUser = 0x809F0011,
};

constexpr u32 kFw160 = 0x1600000, kFw170 = 0x1700000, kFw250 = 0x2500000, kFw400 = 0x4000000,
              kFw450 = 0x4500000;
constexpr u32 kModeRdonly = 1u << 0, kModeCreate = 1u << 2, kModeDestructOff = 1u << 3, kModeCopyIcon = 1u << 4,
              kModeCreate2 = 1u << 5;
constexpr u64 kBlockSize = 32768;
constexpr s64 kBlocksMin2 = 96;   // 3 MiB
constexpr s64 kBlocksMax = 32768; // 1 GiB
constexpr size_t kSlots = 16;

// ---- the guest's structures (the console's layout)

struct DirName {
    char data[32];
};
struct TitleId {
    char data[10];
    char pad[6];
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
    u32 mount_status; // (+4.5)
    u8 reserved[28];
    s32 pad;
};
struct Param {
    char title[128];
    char sub_title[128];
    char detail[1024];
    u32 user_param;
    s32 pad;
    s64 mtime;
    u8 reserved[32];
};
struct Icon {
    void* buf;
    u64 buf_size;
    u64 data_size;
    u8 reserved[32];
};
struct Delete {
    s32 user_id;
    s32 pad0;
    const TitleId* title_id;
    const DirName* dir_name;
    u32 unused;
    u8 reserved[32];
    s32 pad1;
};
struct SearchCond {
    s32 user_id;
    s32 pad;
    const TitleId* title_id;
    const DirName* dir_name;
    u32 key;
    u32 order;
    u8 reserved[32];
};
struct SearchInfo {
    u64 blocks;
    u64 free_blocks;
    u8 reserved[32];
};
struct SearchResult {
    u32 hit_num;
    s32 pad0;
    DirName* dir_names;
    u32 dir_names_num;
    u32 set_num;       // (+1.7)
    Param* params;     // (+1.7)
    SearchInfo* infos; // (+2.5)
    u8 reserved[12];
    s32 pad1;
};
static_assert(sizeof(Mount2) == 64 && sizeof(MountResult) == 64 && sizeof(Param) == 1328 && sizeof(Icon) == 56 &&
              sizeof(Delete) == 64 && sizeof(SearchCond) == 64 && sizeof(SearchInfo) == 48 &&
              sizeof(SearchResult) == 56);

template <size_t N>
std::string_view Text(const char (&s)[N]) {
    return {s, strnlen(s, N)};
}

template <size_t N>
void PutText(char (&dst)[N], std::string_view s) {
    std::memset(dst, 0, N);
    std::memcpy(dst, s.data(), std::min(s.size(), N - 1));
}

// ---- state

Settings g_settings;
std::mutex g_mutex;
bool g_initialized = false;

struct Instance {
    std::string dir_name;
    s32 user_id = 0;
    fs::path save_path;
    std::string mount_point;
    Psf::ParamSfo sfo;
    bool read_only = false;
};
std::array<std::optional<Instance>, kSlots> g_slots;

fs::path TitlePath(s32 user, std::string_view title) {
    return g_settings.home / std::to_string(user) / "savedata" / std::string(title);
}
fs::path SfoPath(const fs::path& save) {
    return save / "sce_sys" / "param.sfo";
}
fs::path CorruptPath(const fs::path& save) {
    return save / "sce_sys" / "corrupted";
}

u64 MaxBlocks(const Psf::ParamSfo& sfo) {
    const auto v = sfo.GetBinary("SAVEDATA_BLOCKS");
    if (!v || v->size() < 8) {
        return kBlocksMax;
    }
    u64 blocks = 0;
    std::memcpy(&blocks, v->data(), 8);
    return blocks;
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

u64 DirectorySize(const fs::path& dir) {
    u64 size = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            size += e.file_size(ec);
        }
    }
    return size;
}

std::string_view DefaultTitle(int language) {
    switch (language) {
    case 0: return "セーブデータ";
    case 2: return "Données sauvegardées";
    case 3: return "Datos guardados";
    case 4: return "Gespeicherte Daten";
    case 5: return "Dati salvati";
    case 6: return "Opgeslagen data";
    case 7: return "Dados guardados";
    case 8: return "Сохраненные данные";
    case 9: return "저장 데이터";
    case 10: return "保存数据";
    case 12: return "Tallennetut tiedot";
    case 13: return "Sparade data";
    case 14: return "Gemte data";
    case 15: return "Lagrede data";
    case 16: return "Zapisane dane";
    case 17: return "Dados salvos";
    case 19: return "Kayıtlı Veriler";
    default: return "Saved Data";
    }
}

/// The oracle's dir name pattern: % any run, _ one character (both sides lower-cased by the caller).
bool Match(std::string_view s, std::string_view p) {
    size_t i = 0, j = 0;
    while (i < s.size() && j < p.size()) {
        if (p[j] == '%') {
            for (size_t k = i; k <= s.size(); ++k) {
                if (Match(s.substr(k), p.substr(j + 1))) {
                    return true;
                }
            }
            return false;
        }
        if (p[j] != '_' && p[j] != s[i]) {
            return false;
        }
        ++i;
        ++j;
    }
    return i == s.size() && j == p.size();
}

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

/// The oracle's Backup::Restore: the files of <save>\sce_backup replace the save's.
bool RestoreBackup(const fs::path& save) {
    std::error_code ec;
    const fs::path backup = save / "sce_backup";
    if (!fs::exists(backup, ec)) {
        return false;
    }
    SSPC_LOG(SaveData, Warning, "restoring the backup of {}", save.string());
    for (const auto& e : fs::directory_iterator(save, ec)) {
        if (e.path().filename() != "sce_backup") {
            fs::remove_all(e.path(), ec);
        }
    }
    for (const auto& e : fs::directory_iterator(backup, ec)) {
        fs::copy(e.path(), save / e.path().filename(), fs::copy_options::recursive, ec);
    }
    return true;
}

// ---- the API

u32 MountLocked(const Mount2* m, MountResult* result) {
    if (m->user_id < 0) {
        return kInvalidLoginUser;
    }
    const std::string dir_name(Text(m->dir_name->data));
    const u32 mode = m->mount_mode;
    const bool read_only = (mode & kModeRdonly) != 0;
    const bool create = (mode & kModeCreate) != 0;
    const bool create_if_missing = (mode & kModeCreate2) != 0 && g_settings.firmware >= kFw450;
    const bool copy_icon = (mode & kModeCopyIcon) != 0;
    const bool ignore_corrupt = (mode & kModeDestructOff) != 0 || g_settings.firmware < kFw160;
    if (create && create_if_missing) {
        return kParameter;
    }

    // the first free slot; a save already mounted in a slot before it is BUSY (as the oracle)
    int slot = -1;
    for (size_t i = 0; i < g_slots.size(); ++i) {
        if (g_slots[i].has_value()) {
            if (g_slots[i]->dir_name == dir_name) {
                return kBusy;
            }
        } else {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        return kMountFull;
    }

    Instance inst;
    inst.dir_name = dir_name;
    inst.user_id = m->user_id;
    inst.save_path = TitlePath(m->user_id, g_settings.title_dir) / dir_name;
    inst.mount_point = "/savedata" + std::to_string(slot);
    inst.read_only = read_only;
    std::error_code ec;
    const fs::path sfo_path = SfoPath(inst.save_path);
    const bool exists = fs::exists(sfo_path, ec);
    if (!create && !create_if_missing && !exists) {
        return kNotFound;
    }
    if (create && exists) {
        return kExists;
    }
    const s64 blocks = std::clamp<s64>(static_cast<s64>(m->blocks), kBlocksMin2, kBlocksMax);
    if (!exists) {
        const fs::path root = g_settings.home / std::to_string(m->user_id) / "savedata";
        fs::create_directories(root, ec);
        const auto space = fs::space(root, ec);
        const u64 requested = static_cast<u64>(blocks) * kBlockSize;
        if (!ec && requested > space.available) {
            result->required_blocks = (requested - space.available) / kBlockSize;
            return kNoSpaceFs;
        }
        fs::create_directories(inst.save_path / "sce_sys", ec);
        if (ec) {
            SSPC_LOG(SaveData, Error, "cannot create {}: {}", inst.save_path.string(), ec.message());
            return kInternal;
        }
        const u8 zero8[8] = {};
        inst.sfo.SetBinary("ACCOUNT_ID", zero8);
        inst.sfo.SetString("MAINTITLE", DefaultTitle(g_settings.language_id));
        inst.sfo.SetString("SUBTITLE", "");
        inst.sfo.SetString("DETAIL", "");
        inst.sfo.SetString("SAVEDATA_DIRECTORY", dir_name);
        inst.sfo.SetInteger("SAVEDATA_LIST_PARAM", 0);
        inst.sfo.SetString("TITLE_ID", g_settings.title_dir);
        u8 b[8] = {};
        const u64 ub = static_cast<u64>(blocks);
        std::memcpy(b, &ub, 8);
        inst.sfo.SetBinary("SAVEDATA_BLOCKS", b);
        if (!inst.sfo.Save(sfo_path)) {
            SSPC_LOG(SaveData, Error, "cannot write {}", sfo_path.string());
            return kInternal;
        }
        if (copy_icon && !g_settings.icon.empty() && fs::exists(g_settings.icon, ec)) {
            const fs::path to = inst.save_path / "sce_sys" / "icon0.png";
            fs::remove(to, ec);
            fs::copy_file(g_settings.icon, to, ec);
        }
        SSPC_LOG(SaveData, Info, "created save {}", inst.save_path.string());
    } else {
        std::string error;
        const bool marked = !ignore_corrupt && fs::exists(CorruptPath(inst.save_path), ec);
        bool loaded = inst.sfo.Load(sfo_path, error);
        if ((marked || !loaded) && RestoreBackup(inst.save_path)) {
            loaded = inst.sfo.Load(sfo_path, error);
        } else if (marked) {
            SSPC_LOG(SaveData, Warning,
                     "{} was left marked corrupted (the game stopped while it was mounted); no backup: mounted as "
                     "it is",
                     inst.save_path.string());
        }
        if (!loaded) {
            SSPC_LOG(SaveData, Error, "{}", error);
            return kBroken;
        }
    }
    if (!ignore_corrupt && !read_only) {
        std::ofstream{CorruptPath(inst.save_path), std::ios::binary};
    }
    auto& vfs = Filesystem::GetVfs();
    if (vfs.IsMounted(inst.mount_point)) {
        vfs.Unmount(inst.mount_point);
    }
    vfs.Mount(inst.mount_point, inst.save_path, read_only);
    PutText(result->mount_point.data, inst.mount_point);
    if (g_settings.firmware >= kFw450) {
        result->mount_status = create_if_missing && !exists ? 1 : 0;
    }
    g_slots[slot].emplace(std::move(inst));
    return kOk;
}

u32 UmountLocked(const MountPoint* mp) {
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (mp == nullptr) {
        return kParameter;
    }
    const std::string_view point = Text(mp->data);
    for (auto& slot : g_slots) {
        if (!slot.has_value() || slot->mount_point != point) {
            continue;
        }
        const bool saved = slot->sfo.Save(SfoPath(slot->save_path));
        if (!saved) {
            SSPC_LOG(SaveData, Error, "cannot write {}", SfoPath(slot->save_path).string());
        }
        std::error_code ec;
        fs::remove(CorruptPath(slot->save_path), ec);
        Filesystem::GetVfs().Unmount(slot->mount_point);
        slot.reset();
        return saved ? kOk : kInternal;
    }
    return kNotFound;
}

Instance* Mounted(const MountPoint* mp) {
    const std::string_view point = Text(mp->data);
    for (auto& slot : g_slots) {
        if (slot.has_value() && slot->mount_point == point) {
            return &*slot;
        }
    }
    return nullptr;
}

SSPC_SYSV_ABI u32 Initialize3(void*) {
    std::scoped_lock lock{g_mutex};
    g_initialized = true;
    return kOk;
}

SSPC_SYSV_ABI u32 Terminate() {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    for (auto& slot : g_slots) {
        if (slot.has_value()) {
            if (g_settings.firmware >= kFw400) {
                return kBusy;
            }
            slot->sfo.Save(SfoPath(slot->save_path));
            Filesystem::GetVfs().Unmount(slot->mount_point);
            slot.reset();
        }
    }
    g_initialized = false;
    return kOk;
}

SSPC_SYSV_ABI u32 SaveMount2(const Mount2* mount, MountResult* result) {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (mount == nullptr || mount->dir_name == nullptr || result == nullptr) {
        return kParameter;
    }
    const u32 r = MountLocked(mount, result);
    SSPC_LOG(SaveData, Info, "sceSaveDataMount2 {} mode {:#x} -> {:#x}{}", Text(mount->dir_name->data),
             mount->mount_mode, r, r == kOk ? fmt::format(" ({})", Text(result->mount_point.data)) : "");
    return r;
}

SSPC_SYSV_ABI u32 SaveUmount(const MountPoint* mp) {
    std::scoped_lock lock{g_mutex};
    const u32 r = UmountLocked(mp);
    SSPC_LOG(SaveData, Info, "sceSaveDataUmount {} -> {:#x}", mp != nullptr ? Text(mp->data) : "(null)", r);
    return r;
}

SSPC_SYSV_ABI u32 SaveDelete(const Delete* del) {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (del == nullptr || del->dir_name == nullptr) {
        return kParameter;
    }
    const std::string_view dir_name = Text(del->dir_name->data);
    if (dir_name.empty()) {
        return kParameter;
    }
    for (const auto& slot : g_slots) {
        if (slot.has_value() && slot->dir_name == dir_name) {
            return kBusy;
        }
    }
    const fs::path save = TitlePath(del->user_id, g_settings.title_dir) / std::string(dir_name);
    std::error_code ec;
    if (fs::exists(save, ec)) {
        fs::remove_all(save, ec);
        if (ec) {
            SSPC_LOG(SaveData, Error, "cannot delete {}: {}", save.string(), ec.message());
            return kInternal;
        }
    }
    SSPC_LOG(SaveData, Info, "sceSaveDataDelete {}", dir_name);
    return kOk;
}

SSPC_SYSV_ABI u32 DirNameSearch(const SearchCond* cond, SearchResult* result) {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (cond == nullptr || result == nullptr || cond->key > 5 || cond->order > 1) {
        return kParameter;
    }
    const std::string title =
        cond->title_id == nullptr ? g_settings.title_dir : std::string(Text(cond->title_id->data));
    const fs::path title_path = TitlePath(cond->user_id, title);
    std::error_code ec;
    if (!fs::exists(title_path, ec)) {
        result->hit_num = 0;
        if (g_settings.firmware >= kFw170) {
            result->set_num = 0;
        }
        return kOk;
    }
    std::vector<std::string> dirs;
    for (const auto& e : fs::directory_iterator(title_path, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_directory(ec) && !name.starts_with("sce_") && fs::exists(SfoPath(e.path()), ec)) {
            dirs.push_back(name);
        }
    }
    if (cond->dir_name != nullptr) {
        const std::string pattern = Lower(Text(cond->dir_name->data));
        if (!pattern.empty()) {
            std::erase_if(dirs, [&](const std::string& d) { return !Match(Lower(d), pattern); });
        }
    }
    struct Info {
        Psf::ParamSfo sfo;
        u64 max_blocks = 0;
        u64 free_blocks = 0;
        s64 mtime = 0;
    };
    std::unordered_map<std::string, Info> info;
    for (const std::string& d : dirs) {
        const fs::path dir = title_path / d;
        Info i;
        std::string error;
        if (!i.sfo.Load(SfoPath(dir), error)) {
            SSPC_LOG(SaveData, Error, "{}", error);
        }
        i.max_blocks = MaxBlocks(i.sfo);
        i.free_blocks = i.max_blocks - DirectorySize(dir) / kBlockSize;
        i.mtime = Mtime(SfoPath(dir));
        info.emplace(d, std::move(i));
    }
    const auto at = [&](const std::string& d) -> const Info& { return info.at(d); };
    switch (cond->key) {
    case 0:
        std::ranges::stable_sort(dirs);
        break;
    case 1:
        std::ranges::stable_sort(dirs, {}, [&](const std::string& d) {
            return at(d).sfo.GetInteger("SAVEDATA_LIST_PARAM").value_or(0);
        });
        break;
    case 2:
        std::ranges::stable_sort(dirs, {}, [&](const std::string& d) { return at(d).max_blocks; });
        break;
    case 3:
        std::ranges::stable_sort(dirs, {}, [&](const std::string& d) { return at(d).mtime; });
        break;
    case 5:
        std::ranges::stable_sort(dirs, {}, [&](const std::string& d) { return at(d).free_blocks; });
        break;
    default:
        break;
    }
    if (cond->order == 1) {
        std::ranges::reverse(dirs);
    }
    const size_t count = std::min<size_t>(result->dir_names_num, dirs.size());
    if (g_settings.firmware >= kFw170) {
        result->hit_num = static_cast<u32>(dirs.size());
        result->set_num = static_cast<u32>(count);
    } else {
        result->hit_num = static_cast<u32>(count);
    }
    for (size_t i = 0; i < count; ++i) {
        PutText(result->dir_names[i].data, dirs[i]);
        const Info& in = at(dirs[i]);
        if (g_settings.firmware >= kFw170 && result->params != nullptr) {
            Param& p = result->params[i];
            std::memset(&p, 0, sizeof(p));
            PutText(p.title, in.sfo.GetString("MAINTITLE").value_or("Unknown"));
            PutText(p.sub_title, in.sfo.GetString("SUBTITLE").value_or(""));
            PutText(p.detail, in.sfo.GetString("DETAIL").value_or(""));
            p.user_param = in.sfo.GetInteger("SAVEDATA_LIST_PARAM").value_or(0);
            p.mtime = in.mtime;
        }
        if (g_settings.firmware >= kFw250 && result->infos != nullptr) {
            result->infos[i].blocks = in.max_blocks;
            result->infos[i].free_blocks = in.free_blocks;
        }
    }
    return kOk;
}

SSPC_SYSV_ABI u32 SetParam(const MountPoint* mp, u32 type, const void* buf, u64 size) {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (type > 4 || mp == nullptr || buf == nullptr) {
        return kParameter;
    }
    Instance* inst = Mounted(mp);
    if (inst == nullptr) {
        return kNotMounted;
    }
    const auto text = [&](size_t max) {
        const auto* s = static_cast<const char*>(buf);
        return std::string(s, strnlen(s, max));
    };
    switch (type) {
    case 0: {
        if (size != sizeof(Param)) {
            return kParameter;
        }
        const auto* p = static_cast<const Param*>(buf);
        inst->sfo.SetString("MAINTITLE", Text(p->title));
        inst->sfo.SetString("SUBTITLE", Text(p->sub_title));
        inst->sfo.SetString("DETAIL", Text(p->detail));
        inst->sfo.SetInteger("SAVEDATA_LIST_PARAM", p->user_param);
        break;
    }
    case 1:
        inst->sfo.SetString("MAINTITLE", text(128));
        break;
    case 2:
        inst->sfo.SetString("SUBTITLE", text(128));
        break;
    case 3:
        inst->sfo.SetString("DETAIL", text(1024));
        break;
    case 4:
        inst->sfo.SetInteger("SAVEDATA_LIST_PARAM", *static_cast<const u32*>(buf));
        break;
    default:
        break;
    }
    return kOk;
}

SSPC_SYSV_ABI u32 SaveIcon(const MountPoint* mp, const Icon* icon) {
    std::scoped_lock lock{g_mutex};
    if (!g_initialized) {
        return kNotInitialized;
    }
    if (mp == nullptr || icon == nullptr || icon->buf == nullptr) {
        return kParameter;
    }
    Instance* inst = Mounted(mp);
    if (inst == nullptr) {
        return kNotMounted;
    }
    if (inst->read_only) {
        return kBadMounted;
    }
    std::ofstream out(inst->save_path / "sce_sys" / "icon0.png", std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(icon->buf),
              static_cast<std::streamsize>(std::min(icon->buf_size, icon->data_size)));
    return out ? kOk : kInternal;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define SD(nid, name, fn) Runtime::Export{nid, "libSceSaveData", "libSceSaveData", name, Fn(&fn), nullptr, "savedata"}
const Runtime::Export kExports[] = {
    SD("TywrFKCoLGY", "sceSaveDataInitialize3", Initialize3),
    SD("yKDy8S5yLA0", "sceSaveDataTerminate", Terminate),
    SD("0z45PIH+SNI", "sceSaveDataMount2", SaveMount2),
    SD("BMR4F-Uek3E", "sceSaveDataUmount", SaveUmount),
    SD("S1GkePI17zQ", "sceSaveDataDelete", SaveDelete),
    SD("dyIhnXq-0SM", "sceSaveDataDirNameSearch", DirNameSearch),
    SD("85zul--eGXs", "sceSaveDataSetParam", SetParam),
    SD("c88Yy54Mx0w", "sceSaveDataSaveIcon", SaveIcon),
};
#undef SD

} // namespace

void Configure(const Settings& settings) {
    std::scoped_lock lock{g_mutex};
    g_settings = settings;
}

Settings Current() {
    std::scoped_lock lock{g_mutex};
    return g_settings;
}

fs::path SaveDir(s32 user, std::string_view title, std::string_view dir) {
    std::scoped_lock lock{g_mutex};
    return TitlePath(user, title.empty() ? std::string_view(g_settings.title_dir) : title) / std::string(dir);
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::SaveData
