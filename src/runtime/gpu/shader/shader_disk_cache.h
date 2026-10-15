// SecondSonPC — the native backend's persistent shader cache (Phase 4, P4.14; docs/PHASE4_SHADER_PRECOMPILER.md §3,
// §5): the frontend's translations (GCN -> SPIR-V, with everything a module is) and the pipelines the executor
// created, kept between runs, so that a known program is not translated again and a known pipeline can be created
// before the game starts.
//
// What a record means is what the frontend's in-process caches mean (shader_cache.h): a program by content
// (ProgramIdentity), its layout modules by runtime parameters, its keyed modules by SpecializationKey. A run that
// finds records for a program it meets for the first time behaves as if it had translated them itself earlier in the
// same process - the module a key finds is the module that key had then. Valid only for the generation they were
// written by (the translator's code, the frontend's code, the device profile, this format): another generation
// lives in another directory and is never read.
//
// One file, shaders.pack, append-only:
//   header (64 bytes)  "SSPCSHC1", format version, header size, generation (32 chars), creation time, XXH3 of it
//   records            {"SPRC", type, payload size, XXH3-64 of the payload} + payload
//     'P' program      identity, footer hash, its code (kept for later preparation)
//     'S' SPIR-V       its XXH3-128, the words (shared by every module and pipeline with the same SPIR-V)
//     'M' module       identity, layout flag, runtime parameters, binding start, user data, key, SPIR-V hash,
//                      reflection, flattened user data, fetch shader code, vertex V#s (a translation's inputs and
//                      outputs: it can be translated again from them)
//     'L' pipeline     a PipelineDesc (what vkCreate*Pipelines needs, no handle) and the time it took
//     'U' usage        at a run's end: the pipelines it used (description hashes) - the next start prepares the ones
//                      used last and most often first
// At open every record is read and checked; the first bad one and everything after it is cut off (a write cut by
// the process's end leaves a torn last record, nothing else). New records are appended by the cache's own thread;
// a record is in the file whole or not at all for the next run. A second process on the same directory reads it
// and writes nothing (a lock file). Over the size limit nothing more is added (counted).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

struct Program;
struct Module;

/// A pipeline as vkCreate*Pipelines makes it, without a handle or a process-local id: the stages by SPIR-V content,
/// the fixed-function state, the fragment stage's runtime parameters (the auxiliary stages, blending adjustments and
/// sample shading read them), the descriptor set layout's bindings. Two equal descriptions are the same pipeline.
struct PipelineDesc {
    bool compute = false;
    bool has_ps = false;
    SpirvHash vs;            ///< compute: the compute stage
    SpirvHash ps;
    GraphicsPipelineState state; ///< module ids 0 (graphics; zero for compute)
    RuntimeParams fs_runtime;    ///< the fragment stage's (zero without one)
    std::vector<u64> bindings;   ///< (binding << 32) | (VkDescriptorType << 16) | count, in the layout's order

    void Serialize(std::vector<u8>& out) const;
    bool Deserialize(std::span<const u8> in, size_t& pos);
    u64 Hash() const; ///< XXH3-64 of the serialized bytes
    bool operator==(const PipelineDesc& o) const;
};

/// What the index keeps of a module record (enough for the frontend to find it by layout or key).
struct ModuleStub {
    u32 record = 0;
    bool layout = false; ///< the first module of its runtime parameters (keyed later, from its own result)
    RuntimeParams runtime;
    BindingStart start;
    SpecializationKey key; ///< keyed modules
};

/// A module record's content.
struct DiskModule {
    ProgramIdentity program;
    bool layout = false;
    RuntimeParams runtime;
    BindingStart start;
    std::array<u32, 16> user_data{};
    SpecializationKey key;
    SpirvHash spirv_hash;
    TranslationResult result; ///< spirv, reflection, flatbuf, fetch_code, vertex_sharps, times
};

struct DiskCacheOptions {
    std::filesystem::path dir;  ///< this generation's directory
    std::string generation;     ///< written into and checked against the file's header (at most 32 characters)
    u64 max_bytes = 2048ull << 20;
    bool read_only = false;     ///< never write (a second process; tests)
};

struct DiskCacheStats {
    // at open
    u64 file_bytes = 0, records = 0, programs = 0, modules = 0, spirv = 0, pipelines = 0;
    u64 torn_bytes = 0;         ///< cut off at open (a torn or damaged tail)
    u64 skipped_records = 0;    ///< of an unknown type (kept)
    double open_ms = 0.0;
    bool recreated = false;     ///< the file was missing, of another format/generation or damaged at its header
    std::string recreate_reason;
    // since
    u64 module_loads = 0, module_load_failures = 0, spirv_loads = 0, spirv_load_failures = 0;
    u64 written_records = 0, written_bytes = 0, written_modules = 0, written_pipelines = 0, written_spirv = 0,
        written_programs = 0;
    u64 dropped_full = 0;       ///< records not added: the size limit
    u64 write_errors = 0;
};

class ShaderDiskCache {
public:
    ShaderDiskCache();
    ~ShaderDiskCache(); ///< Close()
    ShaderDiskCache(const ShaderDiskCache&) = delete;
    ShaderDiskCache& operator=(const ShaderDiskCache&) = delete;

    /// Opens (or creates) the cache in options.dir. false + `error`: not usable at all (the caller runs without it).
    bool Open(const DiskCacheOptions& options, std::string& error);
    /// Writes what is queued and closes the files. Idempotent.
    void Close();
    bool IsOpen() const { return open_; }
    bool Writable() const { return writable_; }
    /// Serializes an auxiliary persistent write with Close. The writer lock remains held
    /// throughout fn; a reader or closed cache never invokes it. fn must not call this cache.
    bool WithWriteOwnership(const std::function<bool()>& fn);
    const std::filesystem::path& Dir() const { return options_.dir; }

    // ---- the frontend (its owner thread)
    /// The module records of a program (none: an empty list).
    std::vector<ModuleStub> Stubs(const ProgramIdentity& program) const;
    /// A module record and its SPIR-V. false + `error`: unreadable or damaged (the caller translates instead).
    bool LoadModule(u32 record, DiskModule& out, std::string& error);
    /// A translated module (its result filled, state about to be published): queued for writing unless the cache has
    /// it already. `layout`: the first module of its runtime parameters. The program's code goes along the first time.
    void AddModule(const Program& program, const Module& module, bool layout);

    // ---- pipelines
    /// The pipeline records, in the order they were first met.
    std::vector<PipelineDesc> Pipelines() const;
    /// The same, those the last runs used first (the latest run's, then by how many runs used them, then first met).
    std::vector<PipelineDesc> PipelinesByUse() const;
    /// The pipelines this run used (their description hashes): one usage record (the end of a run).
    void AddUsage(std::span<const u64> hashes);
    /// Usage records read at open (runs that recorded what they used).
    u32 Runs() const;
    /// The programs the cache has modules or code of (the offline index's coverage).
    std::vector<ProgramIdentity> ProgramIdentities() const;
    bool HasPipeline(const PipelineDesc& desc, u64 hash) const;
    /// A pipeline the executor created (queued for writing unless known). Any thread.
    void AddPipeline(const PipelineDesc& desc, u64 hash, u64 create_ns);
    /// A SPIR-V blob by content (checked against its hash). Any thread.
    bool LoadSpirv(const SpirvHash& hash, std::vector<u32>& out);
    bool HasSpirv(const SpirvHash& hash) const;

    /// `fn(ctx)` on the cache's writer thread every `period_ms` while open (the pipeline cache's periodic save).
    void SetPeriodic(void (*fn)(void*), void* ctx, u32 period_ms);

    DiskCacheStats Stats() const;
    /// Self-test: the next appended record is cut after `bytes` of it and the writer stops (an interrupted write).
    void TestTearNextWrite(u64 bytes) { tear_next_ = bytes; }
    /// Writes the queued records now (tests; Close does it too).
    void Flush();

private:
    struct RecordRef {
        u64 offset = 0; ///< of the record header (0: queued, not in the file yet)
        u32 size = 0;   ///< payload bytes
        u8 type = 0;
    };
    struct ProgramEntry {
        bool has_code = false;
        std::vector<ModuleStub> stubs;
    };
    bool OpenFile(std::string& error);
    bool CreateFresh(const char* reason, std::string& error);
    bool Scan(std::string& error);
    bool IndexRecord(u32 index, u8 type, std::span<const u8> payload);
    bool ReadRecord(u32 index, u8 type, std::vector<u8>& payload);
    /// Queues a record; returns its index. (mutex_ held)
    u32 QueueLocked(u8 type, std::vector<u8> payload);
    void WriterLoop();
    bool WriteOne(u32 index, const std::vector<u8>& payload, u8 type);

    DiskCacheOptions options_;
    bool open_ = false, writable_ = false;
    void* file_ = nullptr;  ///< HANDLE: reads and appends (positional, under io_mutex_)
    void* lock_ = nullptr;  ///< HANDLE of the lock file (exclusive, deleted on close)
    std::atomic<u64> end_{0}; ///< the file's size (the next record's offset; written under io_mutex_)
    std::mutex io_mutex_;

    mutable std::mutex mutex_; ///< the index and the queue
    std::vector<RecordRef> records_;
    std::unordered_map<ProgramIdentity, ProgramEntry, ProgramIdentityHash> programs_;
    std::unordered_set<u64> module_digests_;
    std::unordered_map<SpirvHash, u32, SpirvHashHasher> spirv_;
    std::vector<PipelineDesc> pipelines_;
    std::unordered_multimap<u64, u32> pipeline_index_; ///< hash -> pipelines_ index
    struct Use {
        u32 last_run = 0, runs = 0;
    };
    std::unordered_map<u64, Use> uses_; ///< description hash -> the runs that used it
    u32 runs_ = 0;
    u64 queued_bytes_ = 0;
    std::deque<std::pair<u32, std::vector<u8>>> queue_;
    std::condition_variable cv_, idle_cv_;
    bool stop_ = false;
    bool writing_ = false;
    std::thread writer_;
    void (*periodic_)(void*) = nullptr;
    void* periodic_ctx_ = nullptr;
    u32 periodic_ms_ = 0;
    std::atomic<u64> tear_next_{0};
    bool crash_after_tear_ = false; ///< SSPC_DIAG_SHADER_CACHE_TEAR: the process ends right after the cut write
    DiskCacheStats stats_;
};

/// The persistent caches' generation for this build and device profile: the translator's code (the bridge's
/// recompiler trees and patches), the frontend's code, the profile's fields, the format (16 hex digits).
std::string ShaderCacheGeneration(const DeviceProfile& profile);

} // namespace SecondSon::Gpu::Shader
