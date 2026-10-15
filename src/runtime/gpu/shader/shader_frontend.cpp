// SecondSonPC — the shader frontend (P4.3). See shader_frontend.h.
#include <windows.h>

#include "runtime/gpu/shader/shader_frontend.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>

#include <fmt/format.h>
#include <xxhash.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader_trace.h"
#include "runtime/gpu/stall_trace.h"

namespace SecondSon::Gpu::Shader {

// ---- clock and histogram

namespace {

double TicksPerNs() {
    static const double value = [] {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const u64 c0 = __rdtsc();
        while (Clock::now() - t0 < std::chrono::milliseconds(20)) {
        }
        const auto t1 = Clock::now();
        const u64 c1 = __rdtsc();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        return ns > 0 ? static_cast<double>(c1 - c0) / ns : 1.0;
    }();
    return value;
}

u32 Bucket(u64 ns) {
    if (ns < 2) {
        return 0;
    }
    const u32 e = 63 - static_cast<u32>(std::countl_zero(ns)); // floor(log2)
    const u32 frac = e >= 3 ? static_cast<u32>((ns >> (e - 3)) & 7) : static_cast<u32>((ns << (3 - e)) & 7);
    return std::min<u32>(e * 8 + frac, 511);
}

u64 BucketUpper(u32 b) {
    const u32 e = b / 8, frac = b % 8;
    const double v = std::ldexp(1.0 + (frac + 1) / 8.0, static_cast<int>(e));
    return static_cast<u64>(v);
}

} // namespace

const bool g_fine_timing = [] {
    char v[4] = {};
    return GetEnvironmentVariableA("SSPC_GPU_TIMINGS", v, sizeof(v)) == 1 && v[0] == '1';
}();

const u32 g_fine_every = [] {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_GPU_TIMINGS", v, sizeof(v));
    const std::string_view s(v, n < sizeof(v) ? n : 0);
    if (!s.starts_with("sample")) {
        return 0u;
    }
    const u32 every = s.size() > 7 && s[6] == ':' ? static_cast<u32>(std::strtoul(v + 7, nullptr, 10)) : 32u;
    return std::max<u32>(every, 1);
}();

thread_local bool g_fine_now = false;

u64 TicksToNs(u64 ticks) {
    return static_cast<u64>(static_cast<double>(ticks) / TicksPerNs());
}

void Histogram::Add(u64 ns) {
    ++buckets_[Bucket(ns)];
    ++count_;
    sum_ += ns;
    max_ = std::max(max_, ns);
}

u64 Histogram::Percentile(double p) const {
    if (count_ == 0) {
        return 0;
    }
    const u64 rank = static_cast<u64>(std::ceil(p * static_cast<double>(count_)));
    u64 seen = 0;
    for (u32 b = 0; b < buckets_.size(); ++b) {
        seen += buckets_[b];
        if (seen >= rank) {
            return std::min(BucketUpper(b), max_);
        }
    }
    return max_;
}

void Histogram::Merge(const Histogram& o) {
    for (size_t i = 0; i < buckets_.size(); ++i) {
        buckets_[i] += o.buckets_[i];
    }
    count_ += o.count_;
    sum_ += o.sum_;
    max_ = std::max(max_, o.max_);
}

std::string Histogram::Text() const {
    if (count_ == 0) {
        return "n 0";
    }
    return fmt::format("n {}, mean {:.0f} ns, P50/P95/P99 {}/{}/{} ns, max {} ns", count_, MeanNs(), Percentile(0.5),
                       Percentile(0.95), Percentile(0.99), max_);
}

std::string_view StageStatusName(StageStatus status) {
    static constexpr std::string_view kNames[] = {"ready",     "pending",     "failed", "no program",
                                                  "fetch shader", "unsupported"};
    static_assert(std::size(kNames) == static_cast<size_t>(StageStatus::Count));
    const auto i = static_cast<size_t>(status);
    return i < std::size(kNames) ? kNames[i] : "?";
}

// ---- guest memory in the game

namespace {

class GameReader final : public GuestReader {
public:
    bool Read(u64 addr, void* dst, u64 bytes) override {
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        gm->CopySparse(addr, static_cast<u8*>(dst), bytes); // never faults: unmapped pages read as zeros
        return true;
    }
};

/// SSPC_DIAG_STAGE_MEMO=verify: what a stage's preparation produced - its module, key, flattened user data (every
/// descriptor the SRT walk loaded), buffer and vertex bindings. Never 0.
u64 StageFingerprint(const StageView& s) {
    u64 h = 0xCBF29CE484222325ull;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(reinterpret_cast<uintptr_t>(s.module));
    mix(s.key.hash);
    mix(s.end.unified | (u64{s.end.buffer} << 32));
    for (const u32 w : s.plan.flat) {
        mix(w);
    }
    for (const BufferBinding& b : s.plan.buffers) {
        mix(b.address);
        mix(b.size);
    }
    for (const VertexBinding& v : s.plan.vertices) {
        mix(v.read[0] | (u64{v.read[1]} << 32));
        mix(v.read[2] | (u64{v.read[3]} << 32));
        mix(v.address);
    }
    mix(s.plan.images.size() | (s.plan.samplers.size() << 32));
    return h | 1;
}

// ---- P5.3 Native Render Frontend

/// What the executor reads of a stage the frontend prepared (the parallel processor's CopyStage, plus the key).
void CopyView(StageView& d, const StageView& s) {
    d.status = s.status;
    d.address = s.address;
    d.program = s.program;
    d.footer_hash = s.footer_hash;
    d.module = s.module;
    d.runtime = s.runtime;
    d.start = s.start;
    d.end = s.end;
    d.plan = s.plan;
    d.key = s.key;
    d.program_error = s.program_error;
}

bool SameDesc(const auto& a, const auto& b) {
    return a.raw == b.raw;
}

/// Two preparations of one stage give the executor the same thing: module, key, the next stage's bindings, the
/// flattened user data past the registers (the registers themselves are the draw's own: the executor writes them),
/// every resource.
bool SameNative(const StageView& a, const StageView& b) {
    const ResourceBindingPlan& p = a.plan;
    const ResourceBindingPlan& q = b.plan;
    if (a.module != b.module || !(a.key == b.key) || !(a.end == b.end) || !(a.start == b.start) ||
        p.flat.size() != q.flat.size() || p.buffers.size() != q.buffers.size() || p.images.size() != q.images.size() ||
        p.samplers.size() != q.samplers.size() || p.vertices.size() != q.vertices.size() ||
        p.status_counts != q.status_counts) {
        return false;
    }
    if (p.flat.size() > 16 && std::memcmp(p.flat.data() + 16, q.flat.data() + 16, (p.flat.size() - 16) * 4) != 0) {
        return false;
    }
    for (size_t i = 0; i < p.buffers.size(); ++i) {
        const BufferBinding &x = p.buffers[i], &y = q.buffers[i];
        if (x.binding != y.binding || x.kind != y.kind || !SameDesc(x.desc, y.desc) || x.address != y.address ||
            x.size != y.size || x.written != y.written || x.formatted != y.formatted || x.status != y.status) {
            return false;
        }
    }
    for (size_t i = 0; i < p.images.size(); ++i) {
        const ImageBinding &x = p.images[i], &y = q.images[i];
        if (x.binding != y.binding || x.count != y.count || !SameDesc(x.desc, y.desc) || x.view_type != y.view_type ||
            x.storage != y.storage || x.depth != y.depth || x.array != y.array || x.atomic != y.atomic ||
            x.fmask != y.fmask || x.status != y.status) {
            return false;
        }
    }
    for (size_t i = 0; i < p.samplers.size(); ++i) {
        const SamplerBinding &x = p.samplers[i], &y = q.samplers[i];
        if (x.binding != y.binding || !SameDesc(x.desc, y.desc) || x.status != y.status) {
            return false;
        }
    }
    for (size_t i = 0; i < p.vertices.size(); ++i) {
        const VertexBinding &x = p.vertices[i], &y = q.vertices[i];
        if (x.attribute != y.attribute || !(x.fetch == y.fetch) || x.read != y.read || !SameDesc(x.desc, y.desc) ||
            x.address != y.address || x.size != y.size || x.status != y.status) {
            return false;
        }
    }
    return true;
}

/// A resource of the preparation lies in [lo, hi) (the snapshot): a later draw with the same bytes elsewhere would get
/// it at the old place - not cacheable.
bool PlanTouches(const ResourceBindingPlan& p, u64 lo, u64 hi) {
    const auto in = [&](u64 a, u64 n) { return a < hi && lo < a + std::max<u64>(n, 1); };
    for (const BufferBinding& b : p.buffers) {
        if (b.kind == BufferKind::Guest && b.address != 0 && in(b.address, b.size)) {
            return true;
        }
    }
    for (const VertexBinding& v : p.vertices) {
        if (v.address != 0 && in(v.address, v.size)) {
            return true;
        }
    }
    for (const ImageBinding& im : p.images) {
        if (im.desc.Base() != 0 && in(im.desc.Base(), 256)) {
            return true;
        }
    }
    return false;
}

/// The user data registers a preparation with reflection `r` depended on: those its SRT walk read (`plan.srt`), those
/// holding (part of) a descriptor the reflection reads from the flattened user data's first 16 dwords, the fetch
/// shader's and the vertex buffer tables' pointers. Any other register reaches the shader only through those 16
/// dwords, which the executor writes from the draw itself (StageView::native_shared).
u32 UserDataUsed(const ShaderReflection& r, const ResourceBindingPlan& plan, const FetchShader* fetch) {
    u32 m = plan.srt.user_data_read;
    const auto at = [&](u32 dword) {
        if (dword < 16) {
            m |= 1u << dword;
        }
    };
    const auto sharp = [&](const SharpLocation& l, u32 dwords) { // (the dwords FetchSharp reads)
        if (l.summary == SharpLocation::Summary::SingleLoad) {
            for (u32 i = 0; i < dwords; ++i) {
                at(u32{l.offsets[0]} + i);
            }
        } else if (l.summary == SharpLocation::Summary::MultiLoad) {
            for (u32 i = 0; i < dwords && i < l.offsets.size(); ++i) {
                if ((l.load_mask >> i) & 1) {
                    at(l.offsets[i]);
                }
            }
        }
    };
    for (const BufferResource& b : r.buffers) {
        if (b.kind == BufferKind::Guest) {
            sharp(b.sharp, 4);
        }
    }
    for (const ImageResource& im : r.images) {
        sharp(im.sharp, im.is_r128 ? 4 : 8);
    }
    for (const SamplerResource& s : r.samplers) {
        sharp(s.sharp, 4);
        if (s.post_op == SharpPostOp::DisableAnisoIfSingleLod) {
            at(s.post_op_tsharp_dw3);
        }
    }
    if (r.fetch.present) {
        at(r.fetch.sgpr_base);
        at(r.fetch.sgpr_base + 1);
    }
    if (fetch != nullptr && fetch->present) {
        for (const FetchAttribute& a : fetch->attributes) {
            at(a.sgpr_base);
            at(a.sgpr_base + 1);
        }
    }
    return m;
}

} // namespace

GuestReader& GameMemory() {
    static GameReader reader;
    return reader;
}

// ---- the frontend

ShaderFrontend::ShaderFrontend(const FrontendOptions& options) : options_(options) {
    translator_ = options_.translator != nullptr ? options_.translator : GetShaderTranslator();
    programs_.SetValidation(options_.validation);
    memo_on_ = options_.stage_memo;
    native_on_ = options_.native_stages != NativeStages::Off && options_.native_passes != 0;
    if (char v[4] = {}; memo_on_ && GetEnvironmentVariableA("SSPC_DIAG_MEMO_STATS", v, sizeof(v)) == 1 && v[0] == '1') {
        memo_diag_ = true;
    }
    if (char v[8] = {}; memo_on_ && GetEnvironmentVariableA("SSPC_DIAG_STAGE_MEMO", v, sizeof(v)) == 6 &&
                        std::string_view(v) == "verify") {
        memo_verify_ = true;
    }
    (void)TicksToNs(0); // the TSC calibration (20 ms) now, not at the first draw
    if (options_.async && options_.translate) {
        worker_ = std::thread([this] { WorkerLoop(); });
    }
}

ShaderFrontend::~ShaderFrontend() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::string ShaderFrontend::MemoReport() const {
    if (!memo_diag_) {
        return {};
    }
    static constexpr const char* kStage[] = {"ps", "vs", "gs", "es", "hs", "ls", "cs"};
    std::vector<std::pair<u64, u64>> v(memo_ud_masks_.begin(), memo_ud_masks_.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string t = fmt::format("stage memo misses by input (SSPC_DIAG_MEMO_STATS): address {}, binding start {}, "
                                "runtime {}, frame {}, not ready {}; user data dwords that differ (top):",
                                memo_other_by_[0], memo_other_by_[1], memo_other_by_[2], memo_other_by_[3],
                                memo_other_by_[4]);
    t += fmt::format(" misses an 8-entry memo would have had: ps {}, vs {};", memo_would_hit_[0], memo_would_hit_[1]);
    for (size_t i = 0; i < v.size() && i < 16; ++i) {
        const u32 stage = static_cast<u32>(v[i].first >> 16);
        t += fmt::format(" {} {:#06x}: {};", stage < std::size(kStage) ? kStage[stage] : "?", v[i].first & 0xFFFF,
                         v[i].second);
    }
    return t + "\n";
}

std::string ShaderFrontend::NativeReport() const {
    if (!native_on_) {
        return {};
    }
    const FrontendStats& st = stats_;
    u64 bytes = 0, cached = 0;
    for (const auto& [h, e] : native_) {
        bytes += sizeof(NativeEntry) + e.snapshot.capacity() * 4;
        if (e.view != nullptr) {
            ++cached;
            const ResourceBindingPlan& p = e.view->plan;
            bytes += sizeof(StageView) + p.flat.capacity() * 4 + p.buffers.capacity() * sizeof(BufferBinding) +
                     p.images.capacity() * sizeof(ImageBinding) + p.samplers.capacity() * sizeof(SamplerBinding) +
                     p.vertices.capacity() * sizeof(VertexBinding) + e.view->key.words.capacity() * 4;
        }
    }
    const double rate = st.native_lookups != 0 ? 100.0 * static_cast<double>(st.native_hits) /
                                                     static_cast<double>(st.native_lookups)
                                               : 0.0;
    std::string t = fmt::format(
        "native render frontend (P5.3, {}; passes {:#x}): engine-described stages looked up {}, taken from the cache {} "
        "({:.1f} %), prepared and cached {}, not cacheable {} (a read outside the snapshot {}, a resource in it {}, a "
        "failed read {}), known not cacheable {}, not looked up {} (pointers disagree / snapshot unreadable), hash "
        "collisions {}; entries {} ({} cached, {:.1f} MiB), evicted {}; first sightings not cached {} (cached at "
        "sighting {}); shapes {}, their masks grown {}, user data registers keyed per cached preparation {:.2f} "
        "(besides the snapshot pointers)",
        options_.native_stages == NativeStages::Verify ? "verify" : "on", options_.native_passes, st.native_lookups,
        st.native_hits, rate, st.native_learned, st.native_uncacheable, st.native_uncacheable_by[0],
        st.native_uncacheable_by[1], st.native_uncacheable_by[2], st.native_known_uncacheable, st.native_unkeyed,
        st.native_collisions, native_.size(), cached, static_cast<double>(bytes) / (1 << 20), st.native_evicted,
        st.native_first_seen, std::max(options_.native_admit, 1u), native_masks_.size(), st.native_mask_grew,
        st.native_learned != 0
            ? static_cast<double>(st.native_keyed_registers) / static_cast<double>(st.native_learned)
            : 0.0);
    if (options_.native_stages == NativeStages::Verify) {
        const double vrate = st.native_lookups != 0 ? 100.0 * static_cast<double>(st.native_verified) /
                                                          static_cast<double>(st.native_lookups)
                                                    : 0.0;
        t += fmt::format("; verify: hits prepared in full {} ({:.1f} % of the lookups), different {}; misses: inputs "
                         "never seen {}, the same inputs and bytes {} (evicted / not yet admitted), other bytes {} "
                         "(dwords differing 1: {}, 2: {}, 3-4: {}, 5-8: {}, more: {}; only first dwords of 4-dword "
                         "groups: {})",
                         st.native_verified, vrate, st.native_mismatches, st.native_miss_new, st.native_miss_same,
                         st.native_miss_bytes, st.native_miss_dwords[0], st.native_miss_dwords[1],
                         st.native_miss_dwords[2], st.native_miss_dwords[3], st.native_miss_dwords[4],
                         st.native_miss_word0);
        t += fmt::format("; of the inputs never seen: a new shape {}, else keyed user data registers differing:",
                         st.native_miss_new_shape);
        for (u32 i = 0; i < 16; ++i) {
            if (st.native_miss_ud[i] != 0) {
                t += fmt::format(" s{} {}", i, st.native_miss_ud[i]);
            }
        }
    }
    return t + "\n";
}

const StageView& ShaderFrontend::Stage(HwStage stage) const {
    const size_t si = static_cast<size_t>(stage);
    return shared_[si] != nullptr ? *shared_[si] : stages_[si];
}

void ShaderFrontend::NewFrame() {
    programs_.NewEpoch();
    ++stats_.epochs;
    ++memory_gen_;
    ++native_frame_;
    if (native_on_ && native_.size() > options_.native_max_entries) {
        NativeEvict();
    }
    if (native_seen_.size() > size_t{options_.native_max_entries} * 2) {
        native_seen_.clear(); // (inputs seen once long ago: a sighting again counts as the first)
    }
    if (native_last_snap_.size() > size_t{options_.native_max_entries} * 2) {
        native_last_snap_.clear();
    }
    if (native_last_ud_.size() > size_t{options_.native_max_entries} * 2) {
        native_last_ud_.clear();
    }
    if (native_masks_.size() > size_t{options_.native_max_entries} * 2) {
        native_masks_.clear(); // (learned again; a cached key holds its own mask, so none is taken wrongly)
    }
    // fold what the worker counted
    std::scoped_lock lock{mutex_};
    stats_.translations += std::exchange(w_translations_, 0);
    stats_.translation_failures += std::exchange(w_failures_, 0);
    stats_.translate_us += std::exchange(w_translate_us_, 0);
    stats_.emit_us += std::exchange(w_emit_us_, 0);
    stats_.spirv_bytes += std::exchange(w_spirv_bytes_, 0);
    times_.translate.Merge(w_translate_);
    w_translate_ = {};
}

void ShaderFrontend::SetCodeTracker(TrackCodeFn track, void* ctx) {
    code_track_ = track;
    code_track_ctx_ = ctx;
    programs_.SetCodeTrust(options_.code_trust, track != nullptr ? &TrackCode : nullptr, this);
    fetches_.SetCodeTrust(options_.code_trust, track != nullptr ? &TrackCode : nullptr, this);
}

bool ShaderFrontend::TrackCode(void* self, u64 addr, u64 size) {
    auto* f = static_cast<ShaderFrontend*>(self);
    if (!f->code_track_(f->code_track_ctx_, addr, size)) {
        return false;
    }
    for (u64 b = addr >> 16; b <= (addr + size - 1) >> 16; ++b) {
        f->code_blocks_.insert(b);
    }
    return true;
}

void ShaderFrontend::GpuWrote(u64 addr, u64 size) {
    if (code_blocks_.empty() || size == 0) {
        return;
    }
    ++gpu_writes_checked_;
    const u64 first = addr >> 16, last = (addr + size - 1) >> 16;
    bool hit = last - first > 4096; // (a huge range: let the caches' own range checks decide)
    for (u64 b = first; !hit && b <= last; ++b) {
        hit = code_blocks_.contains(b);
    }
    if (!hit) {
        return;
    }
    const u64 before = programs_.Stats().invalidations + fetches_.invalidations;
    Invalidate(addr, size);
    if (programs_.Stats().invalidations + fetches_.invalidations != before) {
        ++gpu_code_writes_; // the GPU or the processor wrote over trusted shader code
    }
}

void ShaderFrontend::NoteGpuWrites(const StageView& s) {
    if (code_blocks_.empty()) {
        return;
    }
    for (const BufferBinding& b : s.plan.buffers) {
        if (b.kind == BufferKind::Guest && b.written && b.status == ResourceStatus::Ok && b.size != 0) {
            GpuWrote(b.address, b.size);
        }
    }
}

void ShaderFrontend::Invalidate(u64 addr, u64 size) {
    programs_.Invalidate(addr, size);
    if (options_.code_trust != CodeTrust::Off) {
        fetches_.Invalidate(addr, size); // (P5.2: off, the fetch shaders keep the per-frame validation alone)
    }
    stage_epoch_.fill(0); // no stage keeps its program without asking the cache
    ++memory_gen_;        // nor its memoized preparation (P4.7)
}

void ShaderFrontend::Stop() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    idle_cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void ShaderFrontend::Drain() {
    std::unique_lock lock{mutex_};
    idle_cv_.wait(lock, [&] { return (queue_.empty() && running_ == 0) || stop_; });
}

Module* ShaderFrontend::NewModule(Program& program, const RuntimeParams& runtime, const BindingStart& start,
                                  std::span<const u32, 16> user_data) {
    auto m = std::make_unique<Module>();
    m->id = next_module_++;
    m->runtime = runtime;
    m->start = start;
    std::copy(user_data.begin(), user_data.end(), m->user_data.begin());
    m->queued_tick = Ticks();
    Module* raw = m.get();
    program.modules.push_back(std::move(m));
    return raw;
}

bool ShaderFrontend::Enqueue(Job job) {
    if (job.module->disk_record != 0 && disk_ != nullptr && LoadFromDisk(*job.module)) {
        return true; // (P4.14: its translation from an earlier run)
    }
    if (translator_ == nullptr) {
        translator_ = options_.translator != nullptr ? options_.translator : GetShaderTranslator();
    }
    if (!options_.translate || translator_ == nullptr || options_.profile == nullptr) {
        return false;
    }
    if (options_.may_translate != nullptr &&
        !options_.may_translate(options_.may_translate_ctx, job.program->footer_hash)) {
        ++stats_.gated; // asked again at a later draw
        return false;
    }
    {
        std::scoped_lock lock{mutex_};
        if (stop_) {
            return false;
        }
    }
    job.module->queued = true;
    ++stats_.queued;
    if (!options_.async) {
        Run(job);
        return true;
    }
    {
        std::scoped_lock lock{mutex_};
        if (queue_.size() >= options_.max_queue) {
            job.module->queued = false;
            --stats_.queued;
            ++stats_.queue_full;
            return false;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void ShaderFrontend::Run(Job& job) {
    Module& m = *job.module;
    TranslationRequest req;
    req.hw = m.runtime.hw;
    req.sw = m.runtime.sw;
    req.address = job.address;
    req.code = job.program->code;
    req.user_data = m.user_data;
    req.runtime = m.runtime;
    req.profile = options_.profile;
    req.start = m.start;
    req.memory = job.memory;
    req.flatbuf = job.flatbuf;
    req.fetch_code = job.fetch_code;
    req.vertex_sharps = job.vertex_sharps;
    req.footer_hash = job.program->footer_hash;
    const u64 t0 = Ticks();
    TranslationResult result;
    try {
        result = translator_->Translate(req);
    } catch (const std::exception& e) {
        result = {};
        result.error = TranslationError::Emit;
        result.detail = std::string("the translator threw: ") + e.what();
    } catch (...) {
        result = {};
        result.error = TranslationError::Emit;
        result.detail = "the translator threw";
    }
    const u64 t1 = Ticks();
    StallTrace::Span(StallTrace::Ev::ShaderTranslate, t0, t1, job.program->footer_hash); // (P4.12)
    const u64 ns = TicksToNs(t1 - t0);
    const bool ok = result.Ok();
    runs_total_.fetch_add(1, std::memory_order_relaxed);
    run_ns_total_.fetch_add(ns, std::memory_order_relaxed);
    if (ns > run_ns_max_.load(std::memory_order_relaxed)) {
        run_ns_max_.store(ns, std::memory_order_relaxed); // (one writer at a time in the native backend: synchronous)
    }
    ShaderTrace::Event(ShaderTrace::Kind::Translate, t0, t1, job.program->footer_hash, result.spirv.size() * 4,
                       static_cast<u32>(m.runtime.hw));
    {
        std::scoped_lock lock{mutex_};
        ++w_translations_;
        w_failures_ += ok ? 0 : 1;
        w_translate_us_ += result.translate_us;
        w_emit_us_ += result.emit_us;
        w_spirv_bytes_ += result.spirv.size() * 4;
        w_translate_.Add(ns);
    }
    m.result = std::move(result);
    if (ok) {
        m.spirv_hash = HashSpirv(m.result.spirv);
        if (disk_ != nullptr) {
            disk_->AddModule(*job.program, m, !m.keyed); // (P4.14: a copy queued for the cache's writer)
        }
    }
    m.state.store(ok ? ModuleState::Ready : ModuleState::Failed, std::memory_order_release);
}

void ShaderFrontend::RestoreFromDisk(Program& program) {
    static constexpr std::array<u32, 16> kNoUserData{};
    for (ModuleStub& st : disk_->Stubs(program.id)) {
        Module* m = nullptr;
        if (st.layout) {
            if (program.FindLayout(st.runtime) != nullptr) {
                continue; // (two runs' first modules of one runtime: the first record stands)
            }
            m = NewModule(program, st.runtime, st.start, kNoUserData);
            program.layouts.push_back({st.runtime, m});
        } else {
            u64 collisions = 0;
            if (program.FindModule(st.key, collisions) != nullptr) {
                continue;
            }
            m = NewModule(program, st.runtime, st.start, kNoUserData);
            m->key = std::move(st.key);
            m->keyed = true;
        }
        m->disk_record = st.record + 1;
        ++stats_.disk_stubs;
    }
}

bool ShaderFrontend::LoadFromDisk(Module& m) {
    const u32 record = m.disk_record - 1;
    m.disk_record = 0; // (read once: a failure translates it)
    DiskModule dm;
    std::string error;
    bool ok = disk_->LoadModule(record, dm, error);
    // the record must be the module it was found for: the same runtime, start and (keyed) key
    if (ok && (std::memcmp(&dm.runtime, &m.runtime, sizeof(RuntimeParams)) != 0 || !(dm.start == m.start) ||
               dm.layout == m.keyed || (m.keyed && !(dm.key == m.key)))) {
        ok = false;
        error = fmt::format("record {} is not the module it was indexed as", record);
    }
    if (!ok) {
        if (stats_.disk_load_failures++ < 8) {
            SSPC_LOG(GPU, Warning, "shader cache: {} - translated instead", error);
        }
        return false;
    }
    m.user_data = dm.user_data;
    m.spirv_hash = dm.spirv_hash;
    stats_.disk_saved_us += dm.result.translate_us + dm.result.emit_us;
    m.result = std::move(dm.result);
    m.from_disk = true;
    m.queued = true;
    ++stats_.disk_loads;
    disk_loads_total_.fetch_add(1, std::memory_order_relaxed);
    m.state.store(ModuleState::Ready, std::memory_order_release);
    return true;
}

void ShaderFrontend::WorkerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) {
                break;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        Run(job);
        {
            std::scoped_lock lock{mutex_};
            --running_;
        }
        idle_cv_.notify_all();
    }
    idle_cv_.notify_all();
}

void ShaderFrontend::Prepare(Module& m) {
    m.prepared = true;
    const ShaderReflection& r = m.result.reflection;
    if (!r.srt_walker.empty() && !m.srt.Compile(r.srt_walker, m.srt_error)) {
        ++stats_.srt_compile_failures;
    }
    if (!m.keyed) {
        // the first module of a layout: its key from what its translation saw (its flattened user data and V#s)
        ResourceBindingPlan plan;
        plan.flat = m.result.flatbuf;
        const FetchShader* fetch = r.fetch.present ? &r.fetch : nullptr;
        DecodeResources(r, m.start, fetch, m.user_data, 0, VertexSource{nullptr, m.result.vertex_sharps}, plan);
        BuildSpecializationKey(m.runtime, m.start, r, fetch, plan, m.key);
        m.keyed = true;
    }
}

StageStatus ShaderFrontend::PrepareStage(StageView& s, HwStage hw, SwStage sw, u64 addr,
                                         std::span<const u32, 16> user_data, const BindingStart& start,
                                         GuestReader& memory, bool may_queue, const Cp::EngineTable* engine) {
    const size_t si = static_cast<size_t>(hw);
    shared_[si].reset();
    // P4.7: the stage's last ready preparation again (its StageView holds it) when nothing it was built from changed
    StageMemo& memo = memo_[si];
    u64 verify = 0; // SSPC_DIAG_STAGE_MEMO=verify: a hit prepared in full anyway - the fingerprint of what it would give
    if (memo_on_ && memo.valid) {
        const bool same_inputs = memo.addr == addr && memo.start == start && memo.runtime_ver == runtime_ver_[si] &&
                                 memo.program_epoch == programs_.Epoch() && s.status == StageStatus::Ready &&
                                 s.module != nullptr;
        const bool same_ud = std::memcmp(memo.user_data.data(), user_data.data(), sizeof(memo.user_data)) == 0;
        if (same_inputs && same_ud && memo.memory_gen == memory_gen_ &&
            s.module->state.load(std::memory_order_acquire) == ModuleState::Ready) {
            if (!memo_verify_) {
                ++stats_.memo_hits;
                ++s.module->uses;
                NoteGpuWrites(s); // (P5.2: its writes happen again with this draw)
                return StageStatus::Ready;
            }
            verify = StageFingerprint(s);
            ++stats_.memo_verified;
            ++stats_.memo_mismatches; // (taken back below when the full preparation is ready and the same)
        } else {
            ++(!same_inputs ? stats_.memo_miss_other
                            : !same_ud ? stats_.memo_miss_user_data : stats_.memo_miss_memory);
        }
        if (verify == 0 && memo_diag_) { // SSPC_DIAG_MEMO_STATS=1: why the stages miss (gpu_native.txt)
            if (same_inputs && !same_ud) {
                u32 mask = 0;
                for (u32 i = 0; i < 16; ++i) {
                    mask |= memo.user_data[i] != user_data[i] ? 1u << i : 0u;
                }
                ++memo_ud_masks_[(u64{si} << 16) | mask];
            } else if (!same_inputs) {
                ++memo_other_by_[memo.addr != addr                          ? 0
                                 : !(memo.start == start)                   ? 1
                                 : memo.runtime_ver != runtime_ver_[si]     ? 2
                                 : memo.program_epoch != programs_.Epoch() ? 3
                                                                            : 4];
            }
            // would a memo of the stage's last 8 preparations have had it (a draw alternating between programs)?
            u64 h = addr ^ (u64{start.unified} << 48) ^ (u64{start.buffer} << 40) ^ (runtime_ver_[si] << 20);
            for (const u32 w : user_data) {
                h = (h ^ w) * 0x100000001B3ull;
            }
            const u64 g = memory_gen_.load(std::memory_order_relaxed);
            auto& hist = memo_hist_[si];
            if (std::ranges::any_of(hist, [&](const auto& e) { return e.first == h && e.second == g; })) {
                ++memo_would_hit_[si];
            }
            hist[memo_hist_next_[si]++ % hist.size()] = {h, g};
        }
    }
    memo.valid = false;
    s.start = start;
    s.end = start;
    s.module = nullptr;
    // the program
    const u64 tp = FineTicks();
    const bool keep = options_.validation == Validation::PerFrame && s.program != nullptr && s.address == addr &&
                      stage_epoch_[si] == programs_.Epoch();
    if (keep) {
        ++stats_.program_reuse;
    } else {
        const auto l = programs_.Get(memory, addr, hw);
        if (l.created && disk_ != nullptr) {
            RestoreFromDisk(*l.program); // (P4.14)
        }
        s.address = addr;
        s.program = l.program;
        s.footer_hash = l.footer_hash;
        s.program_error = l.error;
        stage_epoch_[si] = programs_.Epoch();
    }
    if (g_fine_timing || g_fine_now) {
        times_.program.Add(TicksToNs(Ticks() - tp));
    }
    if (s.program == nullptr) {
        stage_epoch_[si] = 0;
        return StageStatus::NoProgram;
    }
    Program& program = *s.program;
    (void)sw;

    // the layout: the first module for these runtime parameters
    Layout* layout = program.FindLayout(s.runtime);
    if (layout == nullptr) {
        program.layouts.push_back({s.runtime, NewModule(program, s.runtime, start, user_data)});
        layout = &program.layouts.back();
        ++stats_.layouts;
    }
    if (layout->module->state.load(std::memory_order_acquire) == ModuleState::Failed &&
        layout->retries < options_.layout_retries &&
        (layout->module->result.error == TranslationError::Srt ||
         layout->module->result.error == TranslationError::FetchShader)) {
        // a read of guest memory the game was rewriting: a new first translation from this draw's state
        ++layout->retries;
        ++stats_.layout_retries;
        layout->module = NewModule(program, s.runtime, start, user_data);
    }
    Module& first = *layout->module;
    if (!first.queued && may_queue && first.state.load(std::memory_order_acquire) == ModuleState::Pending) {
        if (first.disk_record != 0) {
            std::copy(user_data.begin(), user_data.end(), first.user_data.begin()); // (the record replaces them)
        }
        Enqueue(Job{&program, &first, addr, &memory, {}, {}, {}});
    }
    const ModuleState fs = first.state.load(std::memory_order_acquire);
    if (fs == ModuleState::Pending) {
        return StageStatus::Pending;
    }
    if (fs == ModuleState::Failed) {
        s.module = &first;
        return StageStatus::Failed;
    }
    if (!first.prepared) {
        Prepare(first);
    }
    const ShaderReflection& r = first.result.reflection;

    // the draw's fetch shader
    const FetchShader* fetch = nullptr;
    const FetchCache::Entry* fe = nullptr;
    if (r.fetch.present) {
        const u32 b = r.fetch.sgpr_base;
        const u64 faddr = b + 1 < 16 ? ((user_data[b] | (u64{user_data[b + 1]} << 32)) & 0xFFFFFFFFFFFFull) : 0;
        fe = &fetches_.Get(memory, faddr, b, programs_.Epoch(), options_.validation);
        if (!fe->ok) {
            return StageStatus::FetchError;
        }
        fetch = &fe->fetch;
    }

    // P5.3 Native Render Frontend: the engine described this stage's resource table - the preparation of a draw with
    // exactly these inputs is taken again (no SRT walk, decoding, key or module search), or this one is cached
    NativeKey nkey;
    u64 nbase = 0, nhash = 0, nshape = 0, nsnap = 0, ninputs = 0;
    u32 nptrs = 0;
    NativeEntry* nhit = nullptr; // verify: the cached preparation the full one is compared with
    bool nmiss = false;          // looked up, not found: the preparation below may be cached (native_admit)
    if (engine != nullptr && native_on_) {
        const u64 fver = fe != nullptr ? fe->version : 0;
        if (NativeKeyOf(hw, *engine, user_data, memory, s, first, fe, fver, start, nkey, nbase, nshape, nptrs, nsnap)) {
            ++stats_.native_lookups;
            const auto ms = native_masks_.find(nshape);
            NativeMask(nkey, user_data, nptrs, ms != native_masks_.end() ? ms->second : 0xFFFFu, nsnap, ninputs, nhash);
            const auto it = native_.find(nhash);
            if (it == native_.end()) {
                nmiss = true;
            } else if (!(it->second.key == nkey) || it->second.snapshot != native_snap_[si]) {
                ++stats_.native_collisions; // (another key or other bytes with this hash: prepared, not cached)
            } else if (it->second.view == nullptr) {
                ++stats_.native_known_uncacheable;
                it->second.last_frame = native_frame_;
            } else if (it->second.view->module->state.load(std::memory_order_acquire) == ModuleState::Ready) {
                it->second.last_frame = native_frame_;
                if (options_.native_stages != NativeStages::Verify) {
                    ++stats_.native_hits;
                    shared_[si] = it->second.view;
                    ++it->second.view->module->uses;
                    NoteGpuWrites(*it->second.view); // (P5.2: its writes happen again with this draw)
                    return StageStatus::Ready;
                }
                nhit = &it->second; // (the full preparation below is the draw's; compared at the end)
            }
        }
    }
    NativeRecorder recorder(memory, nbase, nbase + u64{nkey.dwords} * 4);
    GuestReader& rmem = nmiss ? static_cast<GuestReader&>(recorder) : memory;

    // resources and the key
    const u64 tr = FineTicks();
    if (!ResolveResources(r, first.srt, start, fetch, user_data, addr, rmem, s.plan)) {
        ++stats_.srt_walk_failures;
    }
    if (!first.srt_error.empty()) {
        ++stats_.srt_walk_failures; // a walker SecondSonPC cannot interpret: nothing past the registers resolved
        s.plan.flat.resize(16);
        DecodeResources(r, start, fetch, user_data, addr, VertexSource{&rmem, {}}, s.plan);
    }
    stats_.srt_reads += s.plan.srt.reads;
    stats_.srt_faults += s.plan.srt.faults;
    for (size_t i = 0; i < s.plan.status_counts.size(); ++i) {
        stats_.resources[i] += s.plan.status_counts[i];
    }
    u32 nused = nmiss ? UserDataUsed(r, s.plan, fetch) : 0; // (the key and the module depend on this plan too)
    const u64 tk = FineTicks();
    if (g_fine_timing || g_fine_now) {
        times_.resources.Add(TicksToNs(tk - tr));
    }
    BuildSpecializationKey(s.runtime, start, r, fetch, s.plan, s.key);
    if (g_fine_timing || g_fine_now) {
        times_.key.Add(TicksToNs(Ticks() - tk));
    }
    s.end = EndOf(start, s.plan);

    // the module
    Module* m = program.FindModule(s.key, stats_.module_collisions);
    if (m == nullptr) {
        ++stats_.module_misses;
        m = NewModule(program, s.runtime, start, user_data);
        m->key = s.key;
        m->keyed = true;
    } else if (m->state.load(std::memory_order_acquire) == ModuleState::Ready) {
        ++stats_.module_hits;
    }
    s.module = m;
    if (!m->queued && may_queue && m->state.load(std::memory_order_acquire) == ModuleState::Pending) {
        if (m->disk_record != 0) {
            std::copy(user_data.begin(), user_data.end(), m->user_data.begin()); // (the record replaces them)
        }
        Job job{&program, m, addr, &memory, s.plan.flat, fe != nullptr ? fe->code : std::vector<u32>{}, {}};
        for (const VertexBinding& v : s.plan.vertices) {
            job.vertex_sharps.push_back(v.read);
        }
        Enqueue(std::move(job));
    }
    const ModuleState ms = m->state.load(std::memory_order_acquire);
    if (ms == ModuleState::Pending) {
        return StageStatus::Pending;
    }
    if (ms == ModuleState::Failed) {
        return StageStatus::Failed;
    }
    if (!m->prepared) {
        Prepare(*m);
        times_.wait.Add(TicksToNs(Ticks() - m->queued_tick));
    }
    const ShaderReflection& mr = m->result.reflection;
    if (mr.buffers.size() != r.buffers.size() || mr.images.size() != r.images.size() ||
        mr.samplers.size() != r.samplers.size() || mr.flatbuf_dwords != r.flatbuf_dwords) {
        // the specialization changed the resources (an FMask image, ...): the module's own reflection decides the
        // plan and the next stage's bindings
        ++stats_.layout_mismatch;
        ResolveResources(mr, m->srt, start, fetch, user_data, addr, rmem, s.plan);
        s.end = EndOf(start, s.plan);
        if (nmiss) {
            nused |= UserDataUsed(mr, s.plan, fetch);
        }
    }
    ++m->uses;
    if (verify != 0 && StageFingerprint(s) == verify) {
        --stats_.memo_mismatches;
    }
    if (nhit != nullptr) {
        ++stats_.native_verified;
        if (!SameNative(s, *nhit->view)) {
            ++stats_.native_mismatches;
            if (stats_.native_mismatches <= 8) {
                SSPC_LOG(GPU, Warning,
                         "native frontend (verify): the cached preparation of a {} stage (program {:#x}, snapshot {} "
                         "dwords) differs from its full preparation",
                         hw == HwStage::Ps ? "PS" : "VS", addr, nkey.dwords);
            }
        }
    } else if (nmiss) {
        // the registers this preparation read: the shape's mask grows to them, the key is made again if it changed
        const auto [mi, fresh] = native_masks_.try_emplace(nshape, nused);
        if (!fresh && (mi->second | nused) != mi->second) {
            mi->second |= nused;
            ++stats_.native_mask_grew;
        }
        if (nkey.ud_mask != mi->second) {
            NativeMask(nkey, user_data, nptrs, mi->second, nsnap, ninputs, nhash);
        }
        if (options_.native_stages == NativeStages::Verify) {
            NativeMissWhy(nshape, ninputs, nkey, native_snap_[si]); // (under the mask the key ends up with)
        }
        // cached at the n-th sighting of these inputs
        bool learn = options_.native_admit <= 1;
        if (!learn) {
            if (const auto seen = native_seen_.find(nhash); seen == native_seen_.end()) {
                native_seen_.emplace(nhash, 1u);
            } else if (++seen->second >= options_.native_admit) {
                native_seen_.erase(seen);
                learn = true;
            }
        }
        if (!learn) {
            ++stats_.native_first_seen; // (prepared as before, not cached)
            nmiss = false;
        }
    }
    if (nmiss) {
        // cached only when everything read on the way was the snapshot's and none of the resources lies in it (and the
        // walk left the registers' part of the flattened data alone: the executor writes the draw's own there)
        const u32 why = recorder.Outside() || s.plan.srt.user_data_written          ? 0
                        : PlanTouches(s.plan, nbase, nbase + u64{nkey.dwords} * 4) ? 1
                        : s.plan.srt.faults != 0 || !s.plan.error.empty()          ? 2
                                                                                    : 4;
        if (why == 2) {
            ++stats_.native_uncacheable; // (a failed read may succeed next time: nothing remembered)
            ++stats_.native_uncacheable_by[2];
        } else {
            NativeEntry& e = native_[nhash];
            e.key = nkey;
            e.snapshot = native_snap_[si];
            e.last_frame = native_frame_;
            if (why == 4) {
                auto v = std::make_shared<StageView>();
                CopyView(*v, s);
                v->status = StageStatus::Ready;
                v->native_shared = true;
                e.view = v;
                shared_[si] = std::move(v); // (the record shares it too: no copy of the preparation)
                ++stats_.native_learned;
                stats_.native_keyed_registers += static_cast<u64>(std::popcount(nkey.ud_mask & ~nptrs & 0xFFFFu));
            } else {
                ++stats_.native_uncacheable;
                ++stats_.native_uncacheable_by[why];
            }
        }
    }
    // (the generation after the reads: a read that waited for a GPU write saw it)
    memo.valid = true;
    memo.memory_gen = memory_gen_;
    memo.program_epoch = programs_.Epoch();
    memo.runtime_ver = runtime_ver_[si];
    memo.addr = addr;
    memo.start = start;
    std::memcpy(memo.user_data.data(), user_data.data(), sizeof(memo.user_data));
    NoteGpuWrites(s); // (P5.2)
    return StageStatus::Ready;
}

bool ShaderFrontend::NativeKeyOf(HwStage hw, const Cp::EngineTable& t, std::span<const u32, 16> user_data,
                                 GuestReader& memory, const StageView& s, const Module& first, const void* fetch,
                                 u64 fetch_version, const BindingStart& start, NativeKey& key, u64& base, u64& shape,
                                 u32& pointers, u64& snap_hash) {
    key.address = s.address;
    key.program = s.program;
    key.first = &first;
    key.fetch = fetch;
    key.fetch_version = fetch_version;
    key.start = start;
    key.stage = static_cast<u32>(hw);
    key.dwords = t.refs != 0 ? t.dwords : 0;
    key.user_data = {};
    key.ud_mask = 0;
    key.reserved = 0;
    // every pointer into the snapshot agrees on its start (the commit's layout: a pointer, `offset` dwords past it)
    base = 0;
    pointers = 0;
    for (u32 i = 0; i < t.refs && i < t.sgpr.size(); ++i) {
        const u32 sg = t.sgpr[i];
        if (sg + 1 >= 16) {
            ++stats_.native_unkeyed;
            return false;
        }
        const u64 pointer = u64{user_data[sg]} | (u64{user_data[sg + 1] & 0xFFFF} << 32);
        const u64 at = pointer - u64{t.offset[i]} * 4;
        if (i != 0 && at != base) {
            ++stats_.native_unkeyed;
            return false;
        }
        base = at;
        key.user_data[sg] = u32{t.offset[i]} * 4;
        key.user_data[sg + 1] = user_data[sg + 1] & 0xFFFF0000u;
        pointers |= 3u << sg;
    }
    std::vector<u32>& snap = native_snap_[static_cast<size_t>(hw)];
    snap.resize(key.dwords);
    if (key.dwords != 0 && (base == 0 || !memory.Read(base, snap.data(), u64{key.dwords} * 4))) {
        ++stats_.native_unkeyed;
        return false;
    }
    static_assert(std::has_unique_object_representations_v<NativeKey>, "NativeKey is hashed as bytes");
    shape = XXH3_64bits(&key, sizeof(key));
    snap_hash = XXH3_64bits(snap.data(), snap.size() * 4);
    return true;
}

void ShaderFrontend::NativeMask(NativeKey& key, std::span<const u32, 16> user_data, u32 pointers, u32 mask,
                                u64 snap_hash, u64& inputs, u64& hash) {
    key.ud_mask = mask & 0xFFFFu;
    for (u32 i = 0; i < 16; ++i) {
        if (((pointers >> i) & 1) == 0) {
            key.user_data[i] = ((mask >> i) & 1) != 0 ? user_data[i] : 0;
        }
    }
    inputs = XXH3_64bits(&key, sizeof(key));
    hash = XXH3_64bits_withSeed(&key, sizeof(key), snap_hash);
}

void ShaderFrontend::NativeMissWhy(u64 shape, u64 inputs, const NativeKey& key, const std::vector<u32>& snapshot) {
    const auto [it, fresh] = native_last_snap_.try_emplace(inputs);
    std::vector<u32>& last = it->second;
    if (fresh) {
        ++stats_.native_miss_new;
        const auto [su, sfresh] = native_last_ud_.try_emplace(shape, key.user_data);
        if (sfresh) {
            ++stats_.native_miss_new_shape;
        } else {
            for (u32 i = 0; i < 16; ++i) {
                if (su->second[i] != key.user_data[i]) {
                    ++stats_.native_miss_ud[i];
                }
            }
            su->second = key.user_data;
        }
    } else if (last == snapshot) {
        ++stats_.native_miss_same; // (evicted, or not yet admitted)
    } else {
        ++stats_.native_miss_bytes;
        u32 differ = 0;
        bool word0 = true;
        for (size_t i = 0; i < snapshot.size() && i < last.size(); ++i) {
            if (last[i] != snapshot[i]) {
                ++differ;
                word0 = word0 && i % 4 == 0;
            }
        }
        differ += static_cast<u32>(snapshot.size() > last.size() ? snapshot.size() - last.size()
                                                                 : last.size() - snapshot.size());
        ++stats_.native_miss_dwords[differ <= 1 ? 0 : differ == 2 ? 1 : differ <= 4 ? 2 : differ <= 8 ? 3 : 4];
        if (word0 && snapshot.size() == last.size()) {
            ++stats_.native_miss_word0;
        }
    }
    last = snapshot;
}

void ShaderFrontend::NativeEvict() {
    // the preparations no draw took lately go - in the last 30 frames, else 8, else 2; if even that is not enough,
    // all of them (a scene change)
    for (const u64 window : {u64{30}, u64{8}, u64{2}}) {
        const u64 keep_from = native_frame_ > window ? native_frame_ - window : 0;
        for (auto it = native_.begin(); it != native_.end();) {
            if (it->second.last_frame < keep_from) {
                it = native_.erase(it);
                ++stats_.native_evicted;
            } else {
                ++it;
            }
        }
        if (native_.size() <= options_.native_max_entries) {
            return;
        }
    }
    if (native_.size() > options_.native_max_entries) {
        stats_.native_evicted += native_.size();
        native_.clear();
    }
}

const DrawPreparation& ShaderFrontend::PrepareDraw(const Cp::DrawDescriptor& d, const Cp::RegisterFile& regs,
                                                   GuestReader& memory) {
    const u64 t0 = FineTicks();
    ++stats_.draws;
    prep_ = {};
    shared_[static_cast<size_t>(HwStage::Ps)].reset();
    shared_[static_cast<size_t>(HwStage::Vs)].reset();
    // P5.3: the engine described the draw, in a pass the Native Render Frontend takes
    const bool native = native_on_ && d.engine_pass != Cp::kNoEnginePass &&
                        ((options_.native_passes >> Engine::Render::PassSlot(d.engine_pass)) & 1) != 0;
    if (DecodeStages(regs.Get(Reg::VgtShaderStagesEn)) != StageConfig::Vs) {
        ++stats_.unsupported;
        prep_.unsupported = true;
        if (g_fine_timing || g_fine_now) {
            times_.draw.Add(TicksToNs(Ticks() - t0));
        }
        return prep_;
    }
    BindingStart start{};
    bool ready = true;
    static constexpr std::pair<HwStage, SwStage> kStages[] = {{HwStage::Ps, SwStage::Fragment},
                                                              {HwStage::Vs, SwStage::Vertex}};
    for (const auto& [hw, sw] : kStages) {
        const size_t si = static_cast<size_t>(hw);
        StageView& s = stages_[si];
        // the runtime parameters: rebuilt when their register groups changed (and for indirect draws, whose SH
        // offsets come with the packet)
        if (!runtime_valid_[si] || (d.changed & kRuntimeGroups) != 0 || d.indirect) {
            const u32 vtx = d.indirect ? d.base_vtx_loc : 0, inst = d.indirect ? d.start_inst_loc : 0;
            runtime_valid_[si] = BuildGraphicsRuntime(regs, hw, sw, vtx, inst, s.runtime);
            ++runtime_ver_[si];
            ++stats_.runtime_builds;
        } else {
            ++stats_.runtime_reuse;
        }
        const auto cpst = static_cast<size_t>(hw); // Cp::Stage has the graphics stages in HwStage's order
        const std::span<const u32, 16> ud(d.user_data[cpst]);
        const Cp::EngineTable* engine = !native ? nullptr : hw == HwStage::Ps ? &d.engine_ps : &d.engine_vs;
        s.status = runtime_valid_[si]
                       ? PrepareStage(s, hw, sw, d.shaders[cpst].address, ud, start, memory, ready, engine)
                       : StageStatus::Unsupported;
        ++stats_.stage_status[static_cast<size_t>(s.status)];
        ready &= s.status == StageStatus::Ready;
        start = Stage(hw).end; // (the shared preparation's, when the native cache gave one)
    }
    if (ready) {
        const u64 tp = FineTicks();
        const StageView& ps = Stage(HwStage::Ps);
        const StageView& vs = Stage(HwStage::Vs);
        const u32 ps_id = ps.module->id, vs_id = vs.module->id;
        if (last_pipeline_ != 0 && ps_id == last_ps_ && vs_id == last_vs_ &&
            (d.changed & PipelineStateGroups()) == 0) {
            ++stats_.pipeline_reuse;
            prep_.pipeline = last_pipeline_;
            prep_.pipeline_reused = true;
        } else {
            GraphicsPipelineState state;
            BuildGraphicsPipelineState(regs, vs_id, ps_id, ps.module->result.reflection.mrt_mask, state);
            bool created = false;
            prep_.pipeline = pipelines_.GetGraphics(state, created);
            last_pipeline_ = prep_.pipeline;
            last_ps_ = ps_id;
            last_vs_ = vs_id;
        }
        if (g_fine_timing || g_fine_now) {
            times_.pipeline.Add(TicksToNs(Ticks() - tp));
        }
        prep_.ready = true;
        ++stats_.ready;
    } else {
        ++stats_.waiting;
        last_pipeline_ = 0;
    }
    if (g_fine_timing || g_fine_now) {
        times_.draw.Add(TicksToNs(Ticks() - t0));
    }
    return prep_;
}

const DrawPreparation& ShaderFrontend::PrepareDispatch(const Cp::DispatchDescriptor& d,
                                                       std::span<const u32, Cp::kComputeCount> cs,
                                                       GuestReader& memory) {
    const u64 t0 = FineTicks();
    ++stats_.dispatches;
    prep_ = {};
    StageView& s = stages_[static_cast<size_t>(HwStage::Cs)];
    BuildComputeRuntime(cs, s.runtime);
    ++runtime_ver_[static_cast<size_t>(HwStage::Cs)];
    ++stats_.runtime_builds;
    const std::span<const u32, 16> ud(d.user_data);
    s.status = PrepareStage(s, HwStage::Cs, SwStage::Compute, d.shader.address, ud, BindingStart{}, memory, true);
    ++stats_.stage_status[static_cast<size_t>(s.status)];
    if (s.status == StageStatus::Ready) {
        bool created = false;
        prep_.pipeline = pipelines_.GetCompute(s.module->id, created);
        prep_.ready = true;
        ++stats_.ready;
    } else {
        ++stats_.waiting;
    }
    if (g_fine_timing || g_fine_now) {
        times_.dispatch.Add(TicksToNs(Ticks() - t0));
    }
    return prep_;
}

u64 ShaderFrontend::MemoryBytes() const {
    u64 bytes = programs_.CodeBytes() + pipelines_.MemoryBytes();
    for (const Program* p : programs_.All()) {
        bytes += sizeof(Program);
        for (const auto& m : p->modules) {
            if (m->state.load(std::memory_order_acquire) == ModuleState::Pending) {
                bytes += sizeof(Module) + m->key.words.capacity() * 4; // the worker may be filling the result
                continue;
            }
            bytes += sizeof(Module) + m->key.words.capacity() * 4 + m->result.spirv.capacity() * 4 +
                     m->result.flatbuf.capacity() * 4 + m->result.reflection.srt_walker.capacity() +
                     (m->result.reflection.buffers.capacity() + m->result.reflection.images.capacity() +
                      m->result.reflection.samplers.capacity()) * 48;
        }
    }
    return bytes;
}

std::string ShaderFrontend::Report() const {
    const FrontendStats& st = stats_;
    const ProgramCacheStats& pc = programs_.Stats();
    std::string t;
    t += fmt::format("draws {}, dispatches {}: ready {}, waiting {}, unsupported stage configuration {}\n", st.draws,
                     st.dispatches, st.ready, st.waiting, st.unsupported);
    t += "stage results:";
    for (size_t i = 0; i < st.stage_status.size(); ++i) {
        t += fmt::format(" {}={}", StageStatusName(static_cast<StageStatus>(i)), st.stage_status[i]);
    }
    t += "\n";
    t += fmt::format("programs: {} distinct ({} addresses, {} KiB of code); lookups {}, kept by the stage {}, trusted "
                     "in the epoch {}, read {} (same {}, changed {}, new {}), {} KiB read, invalidated {}; errors:",
                     programs_.Programs(), programs_.Addresses(), programs_.CodeBytes() / 1024, pc.lookups,
                     st.program_reuse, pc.trusted, pc.reads, pc.same, pc.changed, pc.new_programs,
                     pc.bytes_read / 1024, pc.invalidations);
    for (size_t i = 1; i < pc.errors.size(); ++i) {
        t += fmt::format(" {}={}", ProgramErrorName(static_cast<ProgramError>(i)), pc.errors[i]);
    }
    t += fmt::format("\nfetch shaders: {} (reads {}, changed {})\n", fetches_.Size(), fetches_.reads,
                     fetches_.changed);
    t += fmt::format("code trust (P5.2, SSPC_P52_CODE_TRUST={}): programs trusted by write tracking {}, missed writes "
                     "{}; fetch shaders trusted {}, invalidated {}, missed writes {}; GPU/processor writes checked {}, "
                     "over tracked code {} (code blocks {})\n",
                     options_.code_trust == CodeTrust::On       ? "1"
                     : options_.code_trust == CodeTrust::Verify ? "verify"
                                                                : "0",
                     pc.trusted_tracked, pc.trust_mismatches, fetches_.trusted_tracked, fetches_.invalidations,
                     fetches_.trust_mismatches, gpu_writes_checked_, gpu_code_writes_, code_blocks_.size());
    t += fmt::format("runtime parameters: built {}, kept {}\n", st.runtime_builds, st.runtime_reuse);
    t += fmt::format("modules: {} (layouts {}); key hits {}, misses {}, hash collisions {}, layout changed by the "
                     "specialization {}\n",
                     next_module_ - 1, st.layouts, st.module_hits, st.module_misses, st.module_collisions,
                     st.layout_mismatch);
    t += fmt::format("translations: queued {}, queue full {}, waiting for the bridge's translation {}, done {}, "
                     "failed {}, first translations retried {}; translate {} ms, emit {} ms, SPIR-V {} KiB\n",
                     st.queued, st.queue_full, st.gated, st.translations, st.translation_failures, st.layout_retries,
                     st.translate_us / 1000, st.emit_us / 1000, st.spirv_bytes / 1024);
    t += fmt::format("disk cache (P4.14): modules found for programs met {}, read instead of translated {} (saving {} ms "
                     "of translation as recorded), unusable records {}\n",
                     st.disk_stubs, st.disk_loads, st.disk_saved_us / 1000, st.disk_load_failures);
    t += fmt::format("SRT walks: guest reads {}, refused reads (0) {}, walkers not interpreted {}, walks failed {}\n",
                     st.srt_reads, st.srt_faults, st.srt_compile_failures, st.srt_walk_failures);
    t += "resources:";
    for (size_t i = 0; i < st.resources.size(); ++i) {
        t += fmt::format(" {}={}", ResourceStatusName(static_cast<ResourceStatus>(i)), st.resources[i]);
    }
    const PipelineCacheStats& ps = pipelines_.Stats();
    t += fmt::format("\npipelines: {} (graphics {}, compute {}); lookups {}, hits {}, misses {}, hash collisions {}, "
                     "kept without a lookup {}\n",
                     pipelines_.Size(), ps.graphics, ps.compute, ps.lookups, ps.hits, ps.misses, ps.collisions,
                     st.pipeline_reuse);
    t += fmt::format("memory held: {} KiB\n", MemoryBytes() / 1024);
    t += fmt::format("time per draw (whole preparation): {}\n", times_.draw.Text());
    t += fmt::format("  program lookup: {}\n", times_.program.Text());
    t += fmt::format("  resources (SRT walk + descriptors): {}\n", times_.resources.Text());
    t += fmt::format("  specialization key: {}\n", times_.key.Text());
    t += fmt::format("  pipeline (state + lookup, when looked up): {}\n", times_.pipeline.Text());
    t += fmt::format("time per dispatch: {}\n", times_.dispatch.Text());
    t += fmt::format("translation (worker, request to result): {}\n", times_.translate.Text());
    t += fmt::format("wait from queueing to first use: {}\n", times_.wait.Text());
    return t;
}

} // namespace SecondSon::Gpu::Shader
