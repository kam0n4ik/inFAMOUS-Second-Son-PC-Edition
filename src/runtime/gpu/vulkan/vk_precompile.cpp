// SecondSonPC — the driver's pipeline cache file and the start-up precompiler (P4.14). See vk_precompile.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_precompile.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>

#include <fmt/format.h>
#include <xxhash.h>

#include "base/log.h"
#include "runtime/gpu/r9/pcache.h"
#include "runtime/gpu/shader_trace.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Vk {

namespace {

constexpr char kMagic[8] = {'S', 'S', 'P', 'C', 'V', 'K', 'P', 'C'};
constexpr u32 kVersion = 1;

struct CacheHeader {
    char magic[8];
    u32 version;
    u32 vendor_id;
    u32 device_id;
    u32 driver_version;
    u8 uuid[VK_UUID_SIZE];
    u64 data_bytes;
    u64 data_hash;   ///< XXH3-64 of the data
    u64 header_hash; ///< XXH3-64 of the bytes before it
};
static_assert(sizeof(CacheHeader) == 64);

double TscMs(u64 ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
}

} // namespace

// ---- the pipeline cache file

PipelineCacheFile::PipelineCacheFile(DeviceContext& ctx, GS::ShaderDiskCache& owner)
    : ctx_(ctx), owner_(owner), file_(owner.Dir() / "vulkan.bin") {}

PipelineCacheFile::~PipelineCacheFile() {
    if (cache_ != VK_NULL_HANDLE) {
        Fn().vkDestroyPipelineCache(ctx_.Device(), cache_, nullptr);
    }
}

bool PipelineCacheFile::Load() {
    const u64 t0 = __rdtsc();
    std::vector<u8> data;
    // SSPC_DIAG_PIPELINE_CACHE_DRIVER=<hex>: the file is checked as if the driver had this version (a driver update,
    // without changing the driver); SSPC_DIAG_PIPELINE_CACHE_TEAR=<bytes>: the next save stops there and the process
    // ends (a crash while saving)
    u32 driver = ctx_.DriverVersion();
    if (char v[24] = {}; GetEnvironmentVariableA("SSPC_DIAG_PIPELINE_CACHE_DRIVER", v, sizeof(v)) > 0) {
        driver = static_cast<u32>(std::strtoul(v, nullptr, 16));
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_PIPELINE_CACHE_DRIVER={:#x} - the pipeline cache file is checked "
                               "against this driver version",
                 driver);
    }
    if (char v[24] = {}; GetEnvironmentVariableA("SSPC_DIAG_PIPELINE_CACHE_TEAR", v, sizeof(v)) > 0) {
        tear_ = std::max<u64>(std::strtoull(v, nullptr, 10), 1);
        crash_after_tear_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_PIPELINE_CACHE_TEAR={} - the next save is cut and the process ends",
                 tear_);
    }
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        note_ = "no file: an empty cache";
    } else {
        CacheHeader h{};
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in || std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0 ||
            h.header_hash != XXH3_64bits(&h, offsetof(CacheHeader, header_hash))) {
            note_ = "a damaged or foreign file: an empty cache";
        } else if (h.version != kVersion) {
            note_ = "another format version: an empty cache";
        } else if (h.vendor_id != ctx_.VendorId() || h.device_id != ctx_.DeviceId()) {
            note_ = fmt::format("written for another device ({:04x}:{:04x}): an empty cache", h.vendor_id, h.device_id);
        } else if (h.driver_version != driver ||
                   std::memcmp(h.uuid, ctx_.PipelineCacheUuid().data(), VK_UUID_SIZE) != 0) {
            note_ = fmt::format("written by another driver ({:#x}, now {:#x}): an empty cache", h.driver_version, driver);
        } else if (h.data_bytes > (1ull << 31)) {
            note_ = "an implausible size: an empty cache";
        } else {
            data.resize(h.data_bytes);
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!in || XXH3_64bits(data.data(), data.size()) != h.data_hash) {
                note_ = "data cut short or damaged: an empty cache";
                data.clear();
            }
        }
    }
    VkPipelineCacheCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                 .pNext = nullptr,
                                 .flags = 0,
                                 .initialDataSize = data.size(),
                                 .pInitialData = data.empty() ? nullptr : data.data()};
    VkResult r = Fn().vkCreatePipelineCache(ctx_.Device(), &ci, nullptr, &cache_);
    if (r != VK_SUCCESS && !data.empty()) {
        // the driver refused the data (its own checks): start empty rather than without a cache
        note_ = fmt::format("the driver refused the data ({}): an empty cache", static_cast<int>(r));
        data.clear();
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        r = Fn().vkCreatePipelineCache(ctx_.Device(), &ci, nullptr, &cache_);
    }
    if (r != VK_SUCCESS) {
        cache_ = VK_NULL_HANDLE;
        note_ = fmt::format("vkCreatePipelineCache failed ({}): no pipeline cache", static_cast<int>(r));
        return false;
    }
    if (!data.empty()) {
        loaded_bytes_ = data.size();
        last_bytes_ = data.size();
        note_ = fmt::format("{:.1f} MiB loaded", static_cast<double>(data.size()) / (1 << 20));
        // SSPC_DIAG_PCACHE_SIZE=1 (R9-N): what the driver holds of it once created
        if (char v[4] = {}; GetEnvironmentVariableA("SSPC_DIAG_PCACHE_SIZE", v, sizeof(v)) == 1 && v[0] == '1') {
            size_t held = 0;
            const VkResult q = Fn().vkGetPipelineCacheData(ctx_.Device(), cache_, &held, nullptr);
            note_ += fmt::format(" (DIAGNOSTIC: the driver reports {} bytes, result {})", held, static_cast<int>(q));
        }
    }
    ShaderTrace::Event(ShaderTrace::Kind::CacheLoad, t0, __rdtsc(), 1, data.size());
    return true;
}

bool PipelineCacheFile::Save() {
    if (cache_ == VK_NULL_HANDLE) {
        return false;
    }
    std::scoped_lock lock{save_mutex_};
    if (R9::PipelineCacheMode() != 0) {
        return owner_.WithWriteOwnership([this] { return SaveR9(); });
    }
    return owner_.WithWriteOwnership([this] { return SaveOwned(); });
}

bool PipelineCacheFile::SaveR9() {
    const u64 t0 = __rdtsc();
    size_t size = 0;
    if (Fn().vkGetPipelineCacheData(ctx_.Device(), cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
        return false;
    }
    if (size == last_bytes_) {
        skipped_.fetch_add(1);
        return true; // (nothing new in the driver's cache: the file holds it)
    }
    // the driver's data written as it comes: no zero-filled buffer, no second copy with the header
    auto data = std::make_unique_for_overwrite<u8[]>(size);
    if (Fn().vkGetPipelineCacheData(ctx_.Device(), cache_, &size, data.get()) != VK_SUCCESS) {
        return false;
    }
    CacheHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.vendor_id = ctx_.VendorId();
    h.device_id = ctx_.DeviceId();
    h.driver_version = ctx_.DriverVersion();
    std::memcpy(h.uuid, ctx_.PipelineCacheUuid().data(), VK_UUID_SIZE);
    h.data_bytes = size;
    h.data_hash = XXH3_64bits(data.get(), size);
    h.header_hash = XXH3_64bits(&h, offsetof(CacheHeader, header_hash));
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        const u64 total = sizeof(h) + size;
        const u64 bytes = tear_ != 0 ? std::min<u64>(std::exchange(tear_, 0), total) : total;
        out.write(reinterpret_cast<const char*>(&h), static_cast<std::streamsize>(std::min<u64>(bytes, sizeof(h))));
        if (bytes > sizeof(h)) {
            out.write(reinterpret_cast<const char*>(data.get()), static_cast<std::streamsize>(bytes - sizeof(h)));
        }
        if (!out || bytes != total) {
            if (crash_after_tear_) {
                out.flush();
                TerminateProcess(GetCurrentProcess(), 0xDEAD); // (the diagnostic crash while saving)
            }
            return false;
        }
    }
    if (!MoveFileExW(tmp.wstring().c_str(), file_.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SSPC_LOG(GPU, Warning, "pipeline cache: {} not replaced ({})", file_.string(), GetLastError());
        return false;
    }
    last_bytes_ = size;
    saved_bytes_.store(size);
    saves_.fetch_add(1);
    const u64 t1 = __rdtsc();
    ShaderTrace::Event(ShaderTrace::Kind::CacheSave, t0, t1, 0, size);
    SSPC_LOG(GPU, Info, "pipeline cache: {:.1f} MiB saved in {:.0f} ms (R9-F1: module reads not held back)",
             static_cast<double>(size) / (1 << 20),
             static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1)));
    return true;
}

bool PipelineCacheFile::SaveOwned() {
    const u64 t0 = __rdtsc();
    size_t size = 0;
    if (Fn().vkGetPipelineCacheData(ctx_.Device(), cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
        return false;
    }
    std::vector<u8> data(size);
    if (Fn().vkGetPipelineCacheData(ctx_.Device(), cache_, &size, data.data()) != VK_SUCCESS) {
        return false;
    }
    data.resize(size);
    CacheHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.vendor_id = ctx_.VendorId();
    h.device_id = ctx_.DeviceId();
    h.driver_version = ctx_.DriverVersion();
    std::memcpy(h.uuid, ctx_.PipelineCacheUuid().data(), VK_UUID_SIZE);
    h.data_bytes = data.size();
    h.data_hash = XXH3_64bits(data.data(), data.size());
    h.header_hash = XXH3_64bits(&h, offsetof(CacheHeader, header_hash));
    // atomically: the whole file under a temporary name, then renamed over the old one (an interrupted save leaves the
    // old file and a temporary one the next save replaces)
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        std::vector<u8> all(sizeof(h) + data.size());
        std::memcpy(all.data(), &h, sizeof(h));
        std::memcpy(all.data() + sizeof(h), data.data(), data.size());
        const u64 bytes = tear_ != 0 ? std::min<u64>(std::exchange(tear_, 0), all.size()) : all.size();
        out.write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(bytes));
        if (!out || bytes != all.size()) {
            if (crash_after_tear_) {
                out.flush();
                TerminateProcess(GetCurrentProcess(), 0xDEAD); // (the diagnostic crash while saving)
            }
            return false; // (self-test: the process "died" here - the old file is untouched)
        }
    }
    if (!MoveFileExW(tmp.wstring().c_str(), file_.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SSPC_LOG(GPU, Warning, "pipeline cache: {} not replaced ({})", file_.string(), GetLastError());
        return false;
    }
    saved_bytes_.store(data.size());
    saves_.fetch_add(1);
    last_bytes_ = data.size();
    ShaderTrace::Event(ShaderTrace::Kind::CacheSave, t0, __rdtsc(), 0, data.size());
    return true;
}

// ---- the precompiler

PipelinePrecompiler::PipelinePrecompiler(PipelineFactory& factory, GS::ShaderDiskCache& disk,
                                         std::vector<GS::PipelineDesc> descs, const PrecompileOptions& options)
    : factory_(factory), disk_(disk), options_(options) {
    count_ = static_cast<u32>(descs.size());
    entries_ = std::make_unique<Entry[]>(count_);
    for (u32 i = 0; i < count_; ++i) {
        entries_[i].desc = std::move(descs[i]);
        entries_[i].hash = entries_[i].desc.Hash();
        index_.emplace(entries_[i].hash, i);
    }
}

PipelinePrecompiler::~PipelinePrecompiler() {
    Cancel();
    Join();
    for (u32 i = 0; i < count_; ++i) {
        if (entries_[i].obj) {
            factory_.Destroy(*entries_[i].obj);
        }
    }
}

void PipelinePrecompiler::Start() {
    start_tsc_ = __rdtsc();
    u32 threads = options_.threads;
    if (threads == 0) {
        threads = std::clamp<u32>(std::thread::hardware_concurrency() / 2, 1, 6);
    }
    threads = std::min<u32>(threads, std::max<u32>(count_, 1));
    running_.store(threads);
    for (u32 i = 0; i < threads; ++i) {
        workers_.emplace_back([this, i] { Worker(i); });
    }
}

void PipelinePrecompiler::SetBackground() {
    background_.store(true, std::memory_order_release);
}

void PipelinePrecompiler::Cancel() {
    cancel_.store(true, std::memory_order_release);
}

void PipelinePrecompiler::Join() {
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

bool PipelinePrecompiler::Build(Entry& e, std::vector<u32>& vs, std::vector<u32>& ps) {
    const GS::PipelineDesc& d = e.desc;
    if (!disk_.LoadSpirv(d.vs, vs) || (!d.compute && d.has_ps && !disk_.LoadSpirv(d.ps, ps))) {
        missing_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const VkDevice dev = factory_.Device();
    VkShaderModule vm = VK_NULL_HANDLE, pm = VK_NULL_HANDLE;
    const auto make = [dev](const std::vector<u32>& code, VkShaderModule& out) {
        const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0,
                                          .codeSize = code.size() * 4,
                                          .pCode = code.data()};
        return Fn().vkCreateShaderModule(dev, &ci, nullptr, &out) == VK_SUCCESS;
    };
    bool ok = make(vs, vm) && (d.compute || !d.has_ps || make(ps, pm));
    if (ok) {
        auto obj = std::make_unique<PipelineObjects>();
        u32 flags = 0;
        std::string error;
        const u64 t0 = __rdtsc();
        ok = factory_.Build(d, vm, pm, *obj, flags, error);
        const u64 t1 = __rdtsc();
        e.ns = static_cast<u64>(TscMs(t1 - t0) * 1e6);
        build_ns_.fetch_add(e.ns, std::memory_order_relaxed);
        ShaderTrace::Event(ShaderTrace::Kind::Precompile, t0, t1, e.hash, 0, flags | (ok ? 0u : ShaderTrace::kPipeFailed));
        StallTrace::Span(StallTrace::Ev::PipeBuild, t0, t1, e.hash, 0, 2); // (R10 timeline)
        // SSPC_DIAG_PRECOMPILE_LOG=<n> (R9-N): the first n pipelines built - their time and the driver's feedback
        static const u32 log_first = [] {
            char v[16] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_PRECOMPILE_LOG", v, sizeof(v)) > 0
                       ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                       : 0u;
        }();
        static std::atomic<u32> logged{0};
        if (log_first != 0 && logged.load(std::memory_order_relaxed) < log_first) {
            const u32 n = logged.fetch_add(1, std::memory_order_relaxed);
            if (n < log_first) {
                SSPC_LOG(GPU, Info, "DIAGNOSTIC precompile #{}: {} {:016x} {:.1f} ms, feedback {}, cache hit {}", n,
                         d.compute ? "compute" : "graphics", e.hash, static_cast<double>(e.ns) / 1e6,
                         (flags & ShaderTrace::kPipeFeedback) != 0, (flags & ShaderTrace::kPipeCacheHit) != 0);
            }
        }
        if (ok) {
            e.obj = std::move(obj);
        } else if (failed_.load(std::memory_order_relaxed) < 4) {
            SSPC_LOG(GPU, Warning, "shader precompiler: a pipeline of the manifest failed: {}", error);
        }
    }
    if (vm != VK_NULL_HANDLE) {
        Fn().vkDestroyShaderModule(dev, vm, nullptr);
    }
    if (pm != VK_NULL_HANDLE) {
        Fn().vkDestroyShaderModule(dev, pm, nullptr);
    }
    return ok;
}

void PipelinePrecompiler::Worker(u32 index) {
    SetThreadDescription(GetCurrentThread(), (L"SecondSonPC:Precompile" + std::to_wstring(index)).c_str());
    ShaderTrace::NameThread("SecondSonPC:Precompile");
    StallTrace::RegisterThread(StallTrace::Role::Worker); // (R10 timeline)
    bool lowered = false;
    std::vector<u32> vs, ps;
    while (!cancel_.load(std::memory_order_acquire)) {
        if (background_.load(std::memory_order_acquire)) {
            if (index >= options_.background_threads) {
                break; // (the game runs: fewer workers)
            }
            if (!lowered) {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                lowered = true;
            }
        }
        const u32 i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) {
            break;
        }
        Entry& e = entries_[i];
        u8 expected = kPending;
        if (!e.state.compare_exchange_strong(expected, kBuilding, std::memory_order_acq_rel)) {
            continue; // (claimed by the recording thread)
        }
        const bool ok = Build(e, vs, ps);
        (ok ? built_ : failed_).fetch_add(1, std::memory_order_relaxed);
        e.state.store(ok ? kReady : kFailed, std::memory_order_release);
        e.state.notify_all();
    }
    if (running_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        end_tsc_.store(__rdtsc(), std::memory_order_release);
    }
}

bool PipelinePrecompiler::Has(const GS::PipelineDesc& desc, u64 hash) const {
    for (auto [a, b] = index_.equal_range(hash); a != b; ++a) {
        if (entries_[a->second].desc == desc) {
            return true;
        }
    }
    return false;
}

PipelineObjects* PipelinePrecompiler::Take(const GS::PipelineDesc& desc, u64 hash, bool& waited, u64& wait_ns) {
    waited = false;
    wait_ns = 0;
    Entry* e = nullptr;
    for (auto [a, b] = index_.equal_range(hash); a != b; ++a) {
        if (entries_[a->second].desc == desc) {
            e = &entries_[a->second];
            break;
        }
    }
    if (e == nullptr) {
        return nullptr;
    }
    u8 s = e->state.load(std::memory_order_acquire);
    if (s == kPending) {
        if (e->state.compare_exchange_strong(s, kClaimed, std::memory_order_acq_rel)) {
            claimed_.fetch_add(1, std::memory_order_relaxed);
            return nullptr; // (no worker reached it: the caller builds it now, as without the precompiler)
        }
    }
    if (s == kBuilding) {
        waited = true;
        const u64 t0 = __rdtsc();
        while (s == kBuilding) {
            e->state.wait(kBuilding, std::memory_order_acquire);
            s = e->state.load(std::memory_order_acquire);
        }
        wait_ns = static_cast<u64>(TscMs(__rdtsc() - t0) * 1e6);
        waited_.fetch_add(1, std::memory_order_relaxed);
        wait_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
    }
    if (s != kReady) {
        return nullptr;
    }
    if (!e->taken) {
        e->taken = true;
        taken_.fetch_add(1, std::memory_order_relaxed);
    }
    return e->obj.get();
}

u32 PipelinePrecompiler::NeverTaken() const {
    u32 n = 0;
    for (u32 i = 0; i < count_; ++i) {
        n += entries_[i].obj && !entries_[i].taken ? 1 : 0;
    }
    return n;
}

PrecompileProgress PipelinePrecompiler::Progress() const {
    PrecompileProgress p;
    p.total = count_;
    p.built = built_.load(std::memory_order_relaxed);
    p.failed = failed_.load(std::memory_order_relaxed);
    p.claimed = claimed_.load(std::memory_order_relaxed);
    p.taken = taken_.load(std::memory_order_relaxed);
    p.waited = waited_.load(std::memory_order_relaxed);
    p.missing_spirv = missing_.load(std::memory_order_relaxed);
    p.build_ns = build_ns_.load(std::memory_order_relaxed);
    p.wait_ns = wait_ns_.load(std::memory_order_relaxed);
    const u64 end = end_tsc_.load(std::memory_order_acquire);
    p.finished = end != 0 || p.Done() >= p.total;
    p.elapsed_ms = start_tsc_ == 0 ? 0.0 : TscMs((end != 0 ? end : __rdtsc()) - start_tsc_);
    return p;
}

} // namespace SecondSon::Gpu::Vk
