// SecondSonPC — experiment/bbport-gpu: bbport's GPU library hosted by SecondSonPC. See bbport_host.h.
#include <windows.h>

#include "bridge/shadps4/bbport_host.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#include "base/log.h"
#include "bridge/shadps4/native_memory.h"
#include "core/signals.h"
#include "loader/bindings.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/bbport/sspc_host.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Bridge::Bbport {

namespace {

namespace Mem = SecondSon::Memory;

// bbgpu.h's configuration (C layout, the library's header is not on this side's include path)
struct BbGpuConfig {
    const char* title;
    const char* serial;
    const char* user_dir;
    uint32_t sdk_version;
    uint32_t psf_attributes;
    int32_t width, height;
};
using InitFn = int (*)(const BbGpuConfig*);
using VoidFn = void (*)();
using ResolveFn = uintptr_t (*)(const char*);
using CountFn = unsigned (*)();

HMODULE g_dll = nullptr;
BbSspcSetHostFn g_set_host = nullptr;
BbSspcExceptionFn g_exception = nullptr;
ResolveFn g_resolve = nullptr;
Mem::VirtualMemory* g_vm = nullptr;

std::atomic<u64> g_generation{2};
BbSspcRange g_map = nullptr, g_unmap = nullptr, g_invalidate = nullptr;
std::atomic<bool> g_attached{false};

// ---- guest memory on SecondSonPC's manager (bbport runtime_memory.c's semantics)

/// bbport's store_once: aligned 2/4/8-byte pieces as single stores (a label the guest polls is never written twice).
void StoreOnce(u8* dst, const u8* src, u64 n) {
    if (n > 64) {
        std::memcpy(dst, src, n);
        return;
    }
    while (n != 0) {
        const uintptr_t at = reinterpret_cast<uintptr_t>(dst);
        if (n >= 8 && (at & 7) == 0) {
            u64 v;
            std::memcpy(&v, src, 8);
            reinterpret_cast<std::atomic<u64>*>(dst)->store(v, std::memory_order_release);
            dst += 8, src += 8, n -= 8;
        } else if (n >= 4 && (at & 3) == 0) {
            u32 v;
            std::memcpy(&v, src, 4);
            reinterpret_cast<std::atomic<u32>*>(dst)->store(v, std::memory_order_release);
            dst += 4, src += 4, n -= 4;
        } else if (n >= 2 && (at & 1) == 0) {
            u16 v;
            std::memcpy(&v, src, 2);
            reinterpret_cast<std::atomic<u16>*>(dst)->store(v, std::memory_order_release);
            dst += 2, src += 2, n -= 2;
        } else {
            reinterpret_cast<std::atomic<u8>*>(dst)->store(*src, std::memory_order_release);
            ++dst, ++src, --n;
        }
    }
}

int WriteBacking(uintptr_t address, const void* data, uint64_t size) {
    const u8* src = static_cast<const u8*>(data);
    for (u64 at = address, end = address + size; at < end;) {
        u8* host = nullptr;
        u64 run_begin = 0, run_size = 0;
        if (!g_vm->BackingRun(at, &host, &run_begin, &run_size) || host == nullptr || at < run_begin ||
            at >= run_begin + run_size) {
            return 0;
        }
        const u64 n = std::min(end, run_begin + run_size) - at;
        StoreOnce(host + (at - run_begin), src + (at - address), n);
        at += n;
    }
    return 1;
}

bool QueryAt(u64 address, Mem::QueryInfo& q) {
    return g_vm->Query(address, false, &q) == 0 && q.start <= address && address < q.end;
}

u64 MappedLength(u64 address, u64 size) {
    u64 length = 0;
    while (length < size) {
        Mem::QueryInfo q;
        if (!QueryAt(address + length, q) || !q.is_committed) {
            break;
        }
        length = q.end - address;
    }
    return std::min(length, size);
}

uint64_t Clamp(uintptr_t address, uint64_t size) {
    return MappedLength(address, size);
}

int IsMapped(uintptr_t address, uint64_t size) {
    return MappedLength(address, size) >= size ? 1 : 0;
}

int Region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped) {
    Mem::QueryInfo q;
    if (QueryAt(address, q)) {
        *start = q.start;
        *end = q.end;
        *mapped = q.is_committed ? 1 : 0;
        return 1;
    }
    if (g_vm->Query(address, true, &q) == 0 && q.start > address) {
        *start = address; // (a gap: its start is not known here - the address itself)
        *end = q.start;
        *mapped = 0;
        return 1;
    }
    return 0;
}

const uint64_t* Generation() {
    return reinterpret_cast<const uint64_t*>(&g_generation);
}

void GpuProtect(uintptr_t address, uint64_t size, int read, int write) {
    for (u64 at = address, end = address + size; at < end;) {
        u64 start = 0, area_end = 0;
        u32 prot = 0;
        if (g_vm->QueryProtection(at, &start, &area_end, &prot) != 0 || area_end <= at) {
            at = (at + 0x1000) & ~0xFFFull; // (a gap)
            continue;
        }
        const u64 b = std::min(end, area_end);
        // never beyond the guest's own protection
        const Mem::HostAccess access = write && (prot & Mem::Prot::kCpuWrite) ? Mem::HostAccess::ReadWrite
                                       : (read || write) && (prot & Mem::Prot::kCpuRead) ? Mem::HostAccess::Read
                                                                                         : Mem::HostAccess::None;
        g_vm->ProtectTracking(at, b - at, access);
        at = b;
    }
}

int VmaInfo(uintptr_t address, int* prot, int* type, uintptr_t* end) {
    Mem::QueryInfo q;
    if (!QueryAt(address, q) || !q.is_committed) {
        return 0;
    }
    *prot = q.protection;
    *type = q.is_direct ? q.memory_type : -1;
    *end = q.end;
    return 1;
}

// BB_GUEST_IN_PLACE only (off by default): no direct memory is lent to the GPU in place here.
int DirectPhys(uintptr_t, uint64_t*, uintptr_t*) {
    return 0;
}

class Listener final : public Gpu::MemoryListener {
public:
    void OnUnmap(u64 addr, u64 size) override {
        if (g_unmap != nullptr) {
            g_unmap(addr, size);
        }
    }
    void OnRegister(u64, u64) override {
        g_generation.fetch_add(2, std::memory_order_acq_rel); // (bbport's region caches: read again)
    }
    void OnMap(u64 addr, u64 size) override {
        if (g_map != nullptr) {
            g_map(addr, size);
        }
    }
    void OnGuestWrite(u64 addr, u64 size) override {
        if (g_invalidate != nullptr) {
            g_invalidate(addr, size);
        }
    }
};
Listener g_listener;

void SetGpuHooks(BbSspcRange map, BbSspcRange unmap, BbSspcRange invalidate) {
    g_map = map;
    g_unmap = unmap;
    g_invalidate = invalidate;
}

uint64_t ProcessTimeUs() {
    return Kernel::Clock::ProcessTimeUs();
}
uint64_t ProcessTimeCounter() {
    return Kernel::Clock::ProcessTimeCounter();
}
uint64_t TscFrequency() {
    return Kernel::Clock::TscFrequency();
}
int32_t* GuestErrno() {
    // (the library sets it only in its POSIX kqueue/kevent failures; the game's own errno cell is not reached)
    static thread_local int32_t value = 0;
    return &value;
}
void Restart() {
    SSPC_LOG(GPU, Warning, "bbport: the library asked for a restart (not supported in SecondSonPC; ignored)");
}

bool OnAccessViolation(void* context, void*) {
    return g_exception != nullptr && g_exception(context) != 0;
}

// ---- SSPC_FLIP_LOG

std::mutex g_flip_mu;
std::FILE* g_flip_file = nullptr;
std::vector<std::pair<double, int>> g_flip_buf;
u64 g_flip_n = 0;
LARGE_INTEGER g_t0{}, g_freq{};

void NoteFlip(int kind) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - g_t0.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
    std::scoped_lock lock{g_flip_mu};
    g_flip_buf.emplace_back(ms, kind);
    if (g_flip_file != nullptr && g_flip_buf.size() >= 64) {
        for (const auto& [t, k] : g_flip_buf) {
            std::fprintf(g_flip_file, "%llu,%.3f,%d\n", static_cast<unsigned long long>(++g_flip_n), t, k);
        }
        std::fflush(g_flip_file);
        g_flip_buf.clear();
    }
}

using VideoOutFlip = s32 SSPC_SYSV_ABI (*)(s32, s32, s32, s64);
using GnmFlip = s32 SSPC_SYSV_ABI (*)(u32, u32**, u32*, u32**, u32*, u32, u32, u32, s64);
using GnmFlipW = s32 SSPC_SYSV_ABI (*)(u32, u32, u32**, u32*, u32**, u32*, u32, u32, u32, s64);
VideoOutFlip g_vo_flip = nullptr;
GnmFlip g_gnm_flip = nullptr;
GnmFlipW g_gnm_flip_w = nullptr;

SSPC_SYSV_ABI s32 VideoOutFlipWrap(s32 handle, s32 index, s32 mode, s64 arg) {
    NoteFlip(0);
    return g_vo_flip(handle, index, mode, arg);
}
SSPC_SYSV_ABI s32 GnmFlipWrap(u32 count, u32** dcb, u32* dcb_sizes, u32** ccb, u32* ccb_sizes, u32 vo, u32 buf,
                              u32 mode, s64 arg) {
    NoteFlip(1);
    return g_gnm_flip(count, dcb, dcb_sizes, ccb, ccb_sizes, vo, buf, mode, arg);
}
SSPC_SYSV_ABI s32 GnmFlipWWrap(u32 workload, u32 count, u32** dcb, u32* dcb_sizes, u32** ccb, u32* ccb_sizes, u32 vo,
                               u32 buf, u32 mode, s64 arg) {
    NoteFlip(2);
    return g_gnm_flip_w(workload, count, dcb, dcb_sizes, ccb, ccb_sizes, vo, buf, mode, arg);
}

bool FlipLogOn() {
    static const bool on = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("SSPC_FLIP_LOG", v, sizeof(v)) == 1 && v[0] == '1';
    }();
    return on;
}

} // namespace

void SetFlipLogDir(const std::string& logs_dir) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    if (!FlipLogOn()) {
        return;
    }
    const auto file = std::filesystem::path(logs_dir) / "flips.csv";
    g_flip_file = _wfopen(file.wstring().c_str(), L"w");
    if (g_flip_file != nullptr) {
        std::fprintf(g_flip_file, "n,t_ms,kind\n");
        SSPC_LOG(GPU, Info, "SSPC_FLIP_LOG=1: every flip call -> {}", file.string());
    }
}

void* WrapFlip(const char* nid, void* implementation) {
    if (!FlipLogOn() || implementation == nullptr) {
        return implementation;
    }
    const std::string_view n = nid;
    if (n == "U46NwOiJpys") {
        g_vo_flip = reinterpret_cast<VideoOutFlip>(implementation);
        return reinterpret_cast<void*>(&VideoOutFlipWrap);
    }
    if (n == "xbxNatawohc") {
        g_gnm_flip = reinterpret_cast<GnmFlip>(implementation);
        return reinterpret_cast<void*>(&GnmFlipWrap);
    }
    if (n == "Ga6r7H6Y0RI") {
        g_gnm_flip_w = reinterpret_cast<GnmFlipW>(implementation);
        return reinterpret_cast<void*>(&GnmFlipWWrap);
    }
    return implementation;
}

bool Start(const App::Config& cfg, const std::string& title, const std::string& serial, u32 sdk_version,
           u32 psf_attributes, std::string* error) {
    g_vm = GetNativeGame().vm;
    if (g_vm == nullptr) {
        *error = "SecondSonPC's memory manager is not installed (--native memory)";
        return false;
    }
    // the library logs to stderr/stdout (the UCRT's, shared with it): to <logs>bport.log
    const auto lib_log = (cfg.logs_dir / "bbport.log").wstring();
    if (_wfreopen(lib_log.c_str(), L"w", stderr) != nullptr) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        _wfreopen(lib_log.c_str(), L"a", stdout);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path() / "bbport";
    const auto dll = dir / "bbgpu-sspc.dll";
    if (!std::filesystem::exists(dll)) {
        *error = fmt::format("{} not found (built by E:\\BBPort-GPU-Test, branch sspc-integration)", dll.string());
        return false;
    }
    AddDllDirectory(dir.wstring().c_str());
    g_dll = LoadLibraryExW(dll.wstring().c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                               LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (g_dll == nullptr) {
        *error = fmt::format("LoadLibrary {} failed ({})", dll.string(), GetLastError());
        return false;
    }
    const auto get = [](const char* name) { return GetProcAddress(g_dll, name); };
    g_set_host = reinterpret_cast<BbSspcSetHostFn>(get("bbgpu_sspc_set_host"));
    g_exception = reinterpret_cast<BbSspcExceptionFn>(get("bbgpu_sspc_exception"));
    g_resolve = reinterpret_cast<ResolveFn>(get("bbgpu_resolve"));
    const auto register_kernel = reinterpret_cast<VoidFn>(get("bbgpu_register_kernel"));
    const auto init = reinterpret_cast<InitFn>(get("bbgpu_init"));
    const auto count = reinterpret_cast<CountFn>(get("bbgpu_symbol_count"));
    if (!g_set_host || !g_exception || !g_resolve || !register_kernel || !init) {
        *error = "bbgpu-sspc.dll lacks a bbgpu_* export";
        return false;
    }
    static const BbSspcHost host{
        .version = BBGPU_SSPC_HOST_VERSION,
        .reserved = 0,
        .write_backing = &WriteBacking,
        .clamp = &Clamp,
        .region = &Region,
        .generation = &Generation,
        .gpu_protect = &GpuProtect,
        .vma_info = &VmaInfo,
        .direct_phys = &DirectPhys,
        .is_mapped = &IsMapped,
        .set_gpu_hooks = &SetGpuHooks,
        .process_time_us = &ProcessTimeUs,
        .process_time_counter = &ProcessTimeCounter,
        .tsc_frequency = &TscFrequency,
        .guest_errno = &GuestErrno,
        .restart = &Restart,
        .log = nullptr,
    };
    if (g_set_host(&host) == 0) {
        *error = "bbgpu-sspc.dll refused the host table (version)";
        return false;
    }
    // its page tracking first among the access violation handlers (SecondSonPC's GPU backend is not running)
    Core::Signals::Instance()->RegisterAccessViolationHandler(&OnAccessViolation, 0);
    register_kernel();
    const std::string user_dir = (cfg.user_data_dir / "bbport").string();
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);
    // Bloodborne's own upscaling (FSR 3.1 NativeAA with motion vectors made for that game, its jitter) stays off: the
    // game's frames as they are, at the game's resolution (the library reads these through the shared UCRT)
    const auto set_default = [](const char* name, const char* value) {
        if (std::getenv(name) == nullptr) {
            _putenv_s(name, value);
        }
    };
    set_default("BB_UPSCALER", "off");
    set_default("BB_JITTER", "0");
    set_default("BB_OBJECT_MOTION", "0");
    set_default("BB_CONFIG", (user_dir + "/bbport.ini").c_str()); // (a test run may name its own: show_fps=1)
    const BbGpuConfig gc{.title = title.c_str(),
                         .serial = serial.c_str(),
                         .user_dir = user_dir.c_str(),
                         .sdk_version = sdk_version,
                         .psf_attributes = psf_attributes,
                         .width = static_cast<int32_t>(cfg.window_width),
                         .height = static_cast<int32_t>(cfg.window_height)};
    // Two SDLs in one process (SecondSonPC's static one, the library's SDL3.dll): its windows get a class of their
    // own (SDL_app is SecondSonPC's window class on the executable's module already)
    if (HMODULE sdl = GetModuleHandleW(L"SDL3.dll"); sdl != nullptr) {
        using RegisterApp = bool (*)(const char*, uint32_t, void*);
        if (const auto reg = reinterpret_cast<RegisterApp>(GetProcAddress(sdl, "SDL_RegisterApp"))) {
            if (!reg("SecondSonPC_bbport", 0, sdl)) {
                SSPC_LOG(GPU, Warning, "bbport: SDL_RegisterApp failed");
            }
        }
    }
    SSPC_LOG(GPU, Warning, "EXPERIMENT: bbport's GPU library ({}) - window, Vulkan device and command processor",
             dll.string());
    if (init(&gc) != 0) {
        *error = "bbgpu_init failed";
        return false;
    }
    SSPC_LOG(GPU, Info, "bbport: initialized, {} symbols", count ? count() : 0);
    return true;
}

bool Attach(std::string* error) {
    if (g_attached.exchange(true)) {
        return true;
    }
    if (!Gpu::Claim(Gpu::Resource::MemoryListener, Gpu::BackendKind::Sspc, error) ||
        !Gpu::Claim(Gpu::Resource::WriteTracking, Gpu::BackendKind::Sspc, error)) {
        return false;
    }
    // the mappings so far, then every change (bbport replays them when its hooks are set)
    u64 replayed = 0;
    if (g_map != nullptr) {
        for (const auto& a : g_vm->Areas(false)) {
            if (a.type != Mem::VmaType::Free && a.type != Mem::VmaType::Reserved &&
                a.type != Mem::VmaType::PoolReserved && (a.prot & Mem::Prot::kGpuReadWrite) != 0) {
                g_map(a.base, a.size);
                ++replayed;
            }
        }
    }
    if (!Gpu::GetGuestMemory()->SetListener(&g_listener, Gpu::BackendKind::Sspc, error)) {
        return false;
    }
    SSPC_LOG(GPU, Info, "bbport: attached to the guest memory ({} GPU-visible areas replayed)", replayed);
    return true;
}

std::vector<Runtime::Export> Exports() {
    std::vector<Runtime::Export> out;
    if (g_resolve == nullptr) {
        return out;
    }
    u32 served = 0, stubbed = 0, kernel = 0;
    std::string kernel_names;
    for (const auto& row : Loader::Bindings::ImportTable()) {
        if (row.phase0 == Loader::Phase0::Lle) {
            continue;
        }
        const std::string_view lib = row.library;
        const bool gpu_lib = lib == "libSceGnmDriver" || lib == "libSceVideoOut";
        if (!gpu_lib && lib != "libkernel" && lib != "libScePosix") {
            continue;
        }
        const std::string key = fmt::format("{}#{}#{}", row.nid, row.library, row.module);
        void* f = reinterpret_cast<void*>(g_resolve(key.c_str()));
        if (f == nullptr && !gpu_lib) {
            continue;
        }
        if (f == nullptr) {
            f = reinterpret_cast<void*>(Loader::PendingImportStub(row.nid, row.library));
            ++stubbed;
            SSPC_LOG(GPU, Warning, "bbport: {} ({}) not in the library: a logging stub", row.name, row.library);
        } else {
            ++(gpu_lib ? served : kernel);
            if (!gpu_lib) {
                kernel_names += fmt::format(" {}", row.name);
            }
        }
        out.push_back({row.nid, row.library, row.module, row.name, WrapFlip(row.nid, f), nullptr, "bbport", false,
                       false});
    }
    SSPC_LOG(GPU, Info, "bbport: serves {} GnmDriver/VideoOut imports ({} stubbed), {} of the kernel's:{}", served,
             stubbed, kernel, kernel_names);
    return out;
}

} // namespace SecondSon::Bridge::Bbport
