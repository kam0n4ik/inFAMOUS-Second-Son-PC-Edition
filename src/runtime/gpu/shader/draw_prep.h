// SecondSonPC — speculative draw preparation on worker threads (SSPC_PORT_DRAW_PREP=1, off by default).
//
// Port of the DrawPreparation technique of Supermedo's uncharted_pc (inherited there from deadinside28's bbport;
// GPL-2.0-or-later like shadPS4): gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.{h,cpp}. There a scanner replays
// the register writes of every submitted command buffer and workers prepare each direct draw ahead of the GPU thread
// (pipeline, resource sharps); the GPU thread computes the flattened user data itself and takes a prepared draw only
// when it equals the worker's; workers never create or compile anything.
//
// Here the parallel processor's thread 1 is the scanner: with the frontend thread (SSPC_P53_FRONTEND_THREAD=1) its
// records carry each draw's descriptor and the register journal up to its packet. Workers replay the journal into
// their own register files and prepare draws ahead of the frontend thread through ShaderFrontend::SpeculateDraw, which
// reads only what the frontend thread published (PrepTables below: fixed arrays of sequence-locked slots - no access to
// the frontend's maps and vectors) and objects that are never freed (programs, modules, fetch entries, pipeline
// states). The frontend thread takes a worker's stage only when its inputs are the stage's (program, first module,
// runtime parameters, user data, binding start, fetch shader version) and when the SRT walk it runs again itself and
// every guest read of the worker's resource decoding give the same bytes; otherwise the stage is prepared as before.
#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_resources.h"

namespace SecondSon::Gpu::Shader {

/// A slot one thread writes and others read: readers copy it word by word between two reads of its sequence (odd while
/// it is written) and drop a copy a write tore. Words are accessed atomically (relaxed): no data race.
template <typename T>
class SeqSlot {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;

public:
    /// The single writer.
    void Store(const T& v) {
        std::array<u64, kWords> tmp{};
        std::memcpy(tmp.data(), &v, sizeof(T));
        const u32 s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) {
            std::atomic_ref<u64>(words_[i]).store(tmp[i], std::memory_order_relaxed);
        }
        seq_.store(s + 2, std::memory_order_release);
    }
    /// Any thread: false when never written or torn by a write.
    bool Load(T& out) const {
        const u32 s1 = seq_.load(std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1) != 0) {
            return false;
        }
        std::array<u64, kWords> tmp{};
        for (size_t i = 0; i < kWords; ++i) {
            tmp[i] = std::atomic_ref<u64>(words_[i]).load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) {
            return false;
        }
        std::memcpy(&out, tmp.data(), sizeof(T));
        return true;
    }

private:
    std::atomic<u32> seq_{0};
    alignas(8) mutable std::array<u64, kWords> words_{};
};

/// What the frontend thread publishes for the workers (it writes, they read). Every pointer is to an object the
/// frontend never frees: entries of its program, fetch and pipeline caches and modules are never removed.
struct PrepTables {
    struct ProgramPub {
        u64 key = 0; ///< address << 3 | stage
        Program* program = nullptr;
    };
    struct LayoutPub {
        Program* program = nullptr;
        Module* first = nullptr;
        RuntimeParams runtime;
    };
    struct ModulePub {
        Program* program = nullptr;
        u64 hash = 0; ///< the specialization key's
        Module* module = nullptr;
    };
    struct PipelinePub {
        u64 hash = 0;
        u32 id = 0;
        u32 pad = 0;
        GraphicsPipelineState state;
    };
    /// A fetch shader by (address, user data pair): an immutable copy for each version of its cache entry.
    struct FetchPub {
        mutable std::shared_mutex lock;
        u64 key = 0;
        const FetchCache::Entry* entry = nullptr;
        u64 version = 0;
        bool ok = false;
        std::shared_ptr<const FetchShader> fetch;
    };
    static constexpr u32 kPrograms = 8192, kLayouts = 8192, kModules = 8192, kPipelines = 4096, kFetches = 4096;
    std::array<SeqSlot<ProgramPub>, kPrograms> programs;
    std::array<SeqSlot<LayoutPub>, kLayouts> layouts;
    std::array<SeqSlot<ModulePub>, kModules> modules;
    std::array<SeqSlot<PipelinePub>, kPipelines> pipelines;
    std::array<FetchPub, kFetches> fetches;

    static u64 Mix(u64 x) {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDull;
        x ^= x >> 33;
        return x;
    }
    static u64 LayoutHash(const Program* p, const RuntimeParams& r);
    static u64 StateHash(const GraphicsPipelineState& s);
};

/// One guest read of a worker's resource decoding (the frontend thread reads the same bytes again).
struct PrepRead {
    u64 addr = 0;
    u32 bytes = 0;
    u32 offset = 0; ///< into SpecStage::read_bytes
};

/// A worker's preparation of one graphics stage.
struct SpecStage {
    bool ok = false;
    u64 addr = 0;
    std::array<u32, 16> user_data{};
    BindingStart start, end;
    RuntimeParams runtime;
    const Program* program = nullptr;
    const Module* first = nullptr;
    const FetchCache::Entry* fetch_entry = nullptr; ///< nullptr: no fetch shader
    u64 fetch_version = 0;
    ResourceBindingPlan plan; ///< plan.flat: the worker's SRT walk; the rest its decoding
    SpecializationKey key;
    Module* module = nullptr;
    std::vector<PrepRead> reads;
    std::vector<u8> read_bytes;
};

/// A worker's preparation of one draw (PS, VS in the frontend's order).
struct SpecDraw {
    bool ok = false;
    std::array<SpecStage, 2> stages;
    u32 pipeline = 0; ///< 0: not in the published pipelines (the frontend thread builds the state)
    u32 vs_module = 0, ps_module = 0;
};

/// Why SpeculateDraw gave no preparation (counted).
enum class SpecReject : u8 {
    None,
    Native,       ///< the Native Render Frontend's pass (its own cache)
    Stages,       ///< not a VS+PS draw
    Runtime,      ///< unsupported runtime parameters
    Program,      ///< the program at the address not published
    Layout,       ///< no published, prepared first module for these runtime parameters
    Fetch,        ///< the fetch shader not published or not valid
    Walk,         ///< the SRT walk failed
    Module,       ///< no published, ready module for the key
    Reflection,   ///< the module's own reflection differs from the layout's (prepared again on the frontend thread)
    Count,
};

} // namespace SecondSon::Gpu::Shader
