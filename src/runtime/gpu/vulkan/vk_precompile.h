// SecondSonPC — pipelines prepared before they are needed (Phase 4, P4.14; docs/PHASE4_SHADER_PRECOMPILER.md §4, §6):
// the driver's VkPipelineCache kept in a file, and the start-up precompiler that creates every pipeline of the
// persistent cache's manifest (shader_disk_cache.h) on worker threads before the game starts, so that the recording
// thread takes them instead of waiting for the driver.
//
// The precompiler creates only what an earlier run created: each record is a complete PipelineDesc (stages by SPIR-V,
// fixed-function state, bindings) and its pipeline is built by the same code the executor's are (PipelineFactory::
// Build). Nothing is guessed. A pipeline the recording thread needs is taken only when its description is equal in
// every byte; one still being built by a worker is waited for (it would take as long to build again); one no worker
// has started is claimed by the recording thread and built on demand as before. Entries never move once made, their
// states change by compare-and-swap: a pipeline is built once.
//
// The VkPipelineCache file (vulkan.bin next to shaders.pack): a header with the device's vendor/device id, driver
// version and pipelineCacheUUID and an XXH3 of the data; data for another device or driver, a damaged file or data the
// driver refuses start an empty cache. Saved atomically (a temporary file renamed over it) at the stop and every few
// minutes while new pipelines are created.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk {

namespace GS = Gpu::Shader;

class DeviceContext;
class PipelineFactory;
struct PipelineObjects;

/// The driver's pipeline cache and its file.
class PipelineCacheFile {
public:
    /// owner must outlive this object. Save holds its writer lease through atomic publication.
    PipelineCacheFile(DeviceContext& ctx, GS::ShaderDiskCache& owner);
    ~PipelineCacheFile(); ///< destroys the cache (not saved: Save() first)
    PipelineCacheFile(const PipelineCacheFile&) = delete;
    PipelineCacheFile& operator=(const PipelineCacheFile&) = delete;

    /// Creates the cache with the file's data when it fits this device and driver. false: no cache at all.
    bool Load();
    /// Writes the cache's data (temporary file + rename). Any thread; one save at a time.
    bool Save();
    VkPipelineCache Handle() const { return cache_; }
    const std::string& LoadNote() const { return note_; } ///< what Load found ("loaded 12 MiB", "another driver", ...)
    u64 LoadedBytes() const { return loaded_bytes_; }
    u64 SavedBytes() const { return saved_bytes_.load(); }
    u32 Saves() const { return saves_.load(); }
    u32 SavesSkipped() const { return skipped_.load(); } ///< R9-F1: the driver's data as large as the last one

    /// Self-test: the next Save() stops after writing `bytes` of the temporary file (an interrupted save).
    void TestTearNextSave(u64 bytes) { tear_ = bytes; }

private:
    bool SaveOwned();
    DeviceContext& ctx_;
    GS::ShaderDiskCache& owner_;
    std::filesystem::path file_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::string note_;
    u64 loaded_bytes_ = 0;
    std::atomic<u64> saved_bytes_{0};
    std::atomic<u32> saves_{0};
    std::atomic<u32> skipped_{0};
    u64 last_bytes_ = 0; ///< R9-F1: the driver's data loaded or last saved (save_mutex_)
    bool SaveR9(); ///< R9-F1 (SSPC_R9_PCACHE=1): SaveOwned without the copies, skipped when the size is the same
    std::mutex save_mutex_;
    u64 tear_ = 0;
    bool crash_after_tear_ = false; ///< SSPC_DIAG_PIPELINE_CACHE_TEAR
};

struct PrecompileOptions {
    u32 threads = 0;            ///< 0: half the logical processors, at most 6
    u32 background_threads = 2; ///< once the game runs (SetBackground)
};

struct PrecompileProgress {
    u32 total = 0, built = 0, failed = 0, claimed = 0; ///< claimed: by the recording thread before a worker
    u32 taken = 0;           ///< taken by the recording thread (built here)
    u32 waited = 0;          ///< ... that it had to wait for
    u32 missing_spirv = 0;   ///< records whose stages' SPIR-V is not in the cache (failed)
    double elapsed_ms = 0.0; ///< since Start (until finished)
    u64 build_ns = 0;        ///< the workers' creation time, summed
    u64 wait_ns = 0;         ///< the recording thread's waits for workers
    bool finished = false;
    u32 Done() const { return built + failed + claimed; }
};

class PipelinePrecompiler {
public:
    PipelinePrecompiler(PipelineFactory& factory, GS::ShaderDiskCache& disk, std::vector<GS::PipelineDesc> descs,
                        const PrecompileOptions& options);
    ~PipelinePrecompiler(); ///< Cancel + Join; destroys the pipelines it built
    PipelinePrecompiler(const PipelinePrecompiler&) = delete;
    PipelinePrecompiler& operator=(const PipelinePrecompiler&) = delete;

    void Start();
    /// The game runs: fewer workers, lower priority.
    void SetBackground();
    /// No new pipeline is started; Join waits for the ones being built.
    void Cancel();
    void Join();
    PrecompileProgress Progress() const;

    /// The recording thread: the pipeline built for `desc` (nullptr: not one of this precompiler's, failed, or claimed
    /// now for the caller to build). `waited`/`wait_ns`: it was being built and the caller waited.
    PipelineObjects* Take(const GS::PipelineDesc& desc, u64 hash, bool& waited, u64& wait_ns);
    /// Whether `desc` is one of this precompiler's (any thread: the index and the descriptions do not change after
    /// the constructor). R9-N: the frontend does not prefetch those.
    bool Has(const GS::PipelineDesc& desc, u64 hash) const;
    /// Pipelines built and never taken (at the stop: what this run did not need).
    u32 NeverTaken() const;

private:
    enum : u8 { kPending, kBuilding, kReady, kFailed, kClaimed };
    struct Entry {
        GS::PipelineDesc desc;
        u64 hash = 0;
        std::atomic<u8> state{kPending};
        std::unique_ptr<PipelineObjects> obj;
        u64 ns = 0;
        bool taken = false; ///< (the recording thread's)
    };
    void Worker(u32 index);
    bool Build(Entry& e, std::vector<u32>& vs, std::vector<u32>& ps);

    PipelineFactory& factory_;
    GS::ShaderDiskCache& disk_;
    PrecompileOptions options_;
    std::unique_ptr<Entry[]> entries_;
    u32 count_ = 0;
    std::unordered_multimap<u64, u32> index_; ///< hash -> entry (made in the constructor, read only after)
    std::vector<std::thread> workers_;
    std::atomic<u32> next_{0};
    std::atomic<u32> built_{0}, failed_{0}, claimed_{0}, taken_{0}, waited_{0}, missing_{0}, running_{0};
    std::atomic<u64> build_ns_{0}, wait_ns_{0};
    std::atomic<bool> cancel_{false}, background_{false};
    u64 start_tsc_ = 0;
    std::atomic<u64> end_tsc_{0};
};

} // namespace SecondSon::Gpu::Vk
