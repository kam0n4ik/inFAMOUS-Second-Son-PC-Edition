// SecondSonPC — the persistent shader cache (P4.14). See shader_disk_cache.h.
#include <windows.h>

#include "runtime/gpu/shader/shader_disk_cache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <fmt/format.h>
#include <xxhash.h>

#include "base/log.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_capture.h"
#include "runtime/gpu/shader_trace.h"

#ifndef SSPC_NATIVE_SHADER_GENERATION // (CMakeLists.txt: the sources that decide a translation, hashed)
#error "SSPC_NATIVE_SHADER_GENERATION is not defined"
#endif

namespace SecondSon::Gpu::Shader {

namespace {

constexpr char kFileMagic[8] = {'S', 'S', 'P', 'C', 'S', 'H', 'C', '1'};
constexpr u32 kFormatVersion = 1;
constexpr u32 kRecMagic = 0x43525053; // "SPRC"
constexpr u32 kMaxRecordBytes = 64u << 20;
constexpr u8 kProgram = 'P', kSpirv = 'S', kModule = 'M', kPipeline = 'L', kUsage = 'U';

struct FileHeader {
    char magic[8];
    u32 version;
    u32 header_bytes;
    char generation[32];
    u64 created;
    u64 hash; ///< XXH3-64 of the bytes before it
};
static_assert(sizeof(FileHeader) == 64);

struct RecHeader {
    u32 magic;
    u8 type;
    u8 pad[3];
    u32 size;
    u32 reserved;
    u64 hash; ///< XXH3-64 of the payload
};
static_assert(sizeof(RecHeader) == 24);

// The serializers below write every field of these structures; a field added to one of them must be added here too
// (and kFormatVersion raised). The sizes catch an addition.
static_assert(sizeof(SharpLocation) == 52, "SharpLocation changed: update the disk cache's serializer");
static_assert(sizeof(BufferResource) == 64, "BufferResource changed: update the disk cache's serializer");
static_assert(sizeof(ImageResource) == 60, "ImageResource changed: update the disk cache's serializer");
static_assert(sizeof(SamplerResource) == 60, "SamplerResource changed: update the disk cache's serializer");
static_assert(sizeof(FetchAttribute) == 32, "FetchAttribute changed: update the disk cache's serializer");

class Out {
public:
    std::vector<u8> b;
    void U8(u8 v) { b.push_back(v); }
    void U16(u16 v) { Raw(&v, 2); }
    void U32(u32 v) { Raw(&v, 4); }
    void U64(u64 v) { Raw(&v, 8); }
    void Raw(const void* p, size_t n) {
        const auto* c = static_cast<const u8*>(p);
        b.insert(b.end(), c, c + n);
    }
    void Words(std::span<const u32> w) {
        U32(static_cast<u32>(w.size()));
        Raw(w.data(), w.size_bytes());
    }
    void Bytes(std::span<const u8> v) {
        U32(static_cast<u32>(v.size()));
        Raw(v.data(), v.size());
    }
};

class In {
public:
    explicit In(std::span<const u8> d, size_t at = 0) : data(d), pos(at) {}
    std::span<const u8> data;
    size_t pos = 0;
    bool ok = true;
    bool Raw(void* p, size_t n) {
        if (!ok || pos + n > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(p, data.data() + pos, n);
        pos += n;
        return true;
    }
    u8 U8() { u8 v = 0; Raw(&v, 1); return v; }
    u16 U16() { u16 v = 0; Raw(&v, 2); return v; }
    u32 U32() { u32 v = 0; Raw(&v, 4); return v; }
    u64 U64() { u64 v = 0; Raw(&v, 8); return v; }
    void Words(std::vector<u32>& w) {
        const u32 n = U32();
        if (!ok || n > (data.size() - pos) / 4) {
            ok = false;
            return;
        }
        w.resize(n);
        Raw(w.data(), n * 4ull);
    }
    void Bytes(std::vector<u8>& v) {
        const u32 n = U32();
        if (!ok || n > data.size() - pos) {
            ok = false;
            return;
        }
        v.resize(n);
        Raw(v.data(), n);
    }
    /// A count of elements of at least `min_bytes` each that fits in what is left (0 and not ok otherwise).
    u32 Count(size_t min_bytes) {
        const u32 n = U32();
        if (!ok || n > (data.size() - pos) / std::max<size_t>(min_bytes, 1)) {
            ok = false;
            return 0;
        }
        return n;
    }
};

void PutIdentity(Out& o, const ProgramIdentity& id) {
    o.U64(id.hash.lo);
    o.U64(id.hash.hi);
    o.U32(id.length);
    o.U8(static_cast<u8>(id.stage));
}
ProgramIdentity GetIdentity(In& r) {
    ProgramIdentity id;
    id.hash.lo = r.U64();
    id.hash.hi = r.U64();
    id.length = r.U32();
    id.stage = static_cast<HwStage>(r.U8());
    if (static_cast<u8>(id.stage) >= static_cast<u8>(HwStage::Count)) {
        r.ok = false;
    }
    return id;
}

void PutSharp(Out& o, const SharpLocation& s) {
    o.U8(static_cast<u8>(s.summary));
    o.U8(s.dwords);
    o.U8(s.load_mask);
    o.Raw(s.offsets.data(), sizeof(s.offsets));
    o.Raw(s.immediates.data(), sizeof(s.immediates));
}
void GetSharp(In& r, SharpLocation& s) {
    s.summary = static_cast<SharpLocation::Summary>(r.U8());
    s.dwords = r.U8();
    s.load_mask = r.U8();
    r.Raw(s.offsets.data(), sizeof(s.offsets));
    r.Raw(s.immediates.data(), sizeof(s.immediates));
}

void PutFetch(Out& o, const FetchShader& f) {
    o.U8(f.present ? 1 : 0);
    o.U32(f.sgpr_base);
    o.U32(static_cast<u32>(f.attributes.size()));
    for (const FetchAttribute& a : f.attributes) {
        o.U32(a.dest_vgpr);
        o.U32(a.num_elements);
        o.U32(a.sgpr_base);
        o.U32(a.dword_offset);
        o.U32(a.inst_offset);
        o.U32(a.instance_data);
        o.U32(a.data_format);
        o.U32(a.num_format);
    }
    o.U32(static_cast<u32>(f.vertex_offset_sgpr));
    o.U32(static_cast<u32>(f.instance_offset_sgpr));
    o.U32(f.size);
}
void GetFetch(In& r, FetchShader& f) {
    f.present = r.U8() != 0;
    f.sgpr_base = r.U32();
    f.attributes.resize(r.Count(32));
    for (FetchAttribute& a : f.attributes) {
        a.dest_vgpr = r.U32();
        a.num_elements = r.U32();
        a.sgpr_base = r.U32();
        a.dword_offset = r.U32();
        a.inst_offset = r.U32();
        a.instance_data = r.U32();
        a.data_format = r.U32();
        a.num_format = r.U32();
    }
    f.vertex_offset_sgpr = static_cast<s32>(r.U32());
    f.instance_offset_sgpr = static_cast<s32>(r.U32());
    f.size = r.U32();
}

void PutReflection(Out& o, const ShaderReflection& x) {
    o.U8(static_cast<u8>(x.hw));
    o.U8(static_cast<u8>(x.sw));
    o.U64(x.footer_hash);
    o.U32(static_cast<u32>(x.buffers.size()));
    for (const BufferResource& b : x.buffers) {
        PutSharp(o, b.sharp);
        o.U8(static_cast<u8>(b.kind));
        o.U8(b.is_written ? 1 : 0);
        o.U8(b.is_formatted ? 1 : 0);
        o.U8(static_cast<u8>(b.post_op));
        o.U32(b.post_op_dw1_mask);
        o.U32(b.used_types);
    }
    o.U32(static_cast<u32>(x.images.size()));
    for (const ImageResource& i : x.images) {
        PutSharp(o, i.sharp);
        o.U8(static_cast<u8>((i.is_depth ? 1 : 0) | (i.is_atomic ? 2 : 0) | (i.is_array ? 4 : 0) |
                             (i.is_written ? 8 : 0) | (i.is_r128 ? 16 : 0)));
        o.U8(i.constant_mip_index);
        o.U8(static_cast<u8>(i.mip_fallback));
        o.U8(static_cast<u8>(i.post_op));
    }
    o.U32(static_cast<u32>(x.samplers.size()));
    for (const SamplerResource& s : x.samplers) {
        PutSharp(o, s.sharp);
        o.U8(static_cast<u8>(s.post_op));
        o.U16(s.post_op_tsharp_dw3);
        o.U8(s.is_depth ? 1 : 0);
    }
    o.U32(static_cast<u32>(x.fmasks.size()));
    for (const FmaskResource& f : x.fmasks) {
        o.U16(f.sharp_idx);
    }
    o.U32(x.flatbuf_dwords);
    o.Bytes(x.srt_walker);
    PutFetch(o, x.fetch);
    o.U16(x.user_data_used);
    o.U8(x.mrt_mask);
    o.U8(static_cast<u8>((x.has_discard ? 1 : 0) | (x.uses_dma ? 2 : 0) | (x.has_storage_images ? 4 : 0) |
                         (x.uses_lane_id ? 8 : 0) | (x.uses_shared ? 16 : 0) | (x.uses_fp16 ? 32 : 0) |
                         (x.uses_fp64 ? 64 : 0) | (x.has_bitwise_xor ? 128 : 0)));
    o.U32(x.shared_memory_size);
    o.U32(static_cast<u32>(x.tess_consts_dword_offset));
    o.U32(x.start.unified);
    o.U32(x.start.buffer);
    o.U32(x.end.unified);
    o.U32(x.end.buffer);
}
void GetReflection(In& r, ShaderReflection& x) {
    x.hw = static_cast<HwStage>(r.U8());
    x.sw = static_cast<SwStage>(r.U8());
    x.footer_hash = r.U64();
    x.buffers.resize(r.Count(52 + 12));
    for (BufferResource& b : x.buffers) {
        GetSharp(r, b.sharp);
        b.kind = static_cast<BufferKind>(r.U8());
        b.is_written = r.U8() != 0;
        b.is_formatted = r.U8() != 0;
        b.post_op = static_cast<SharpPostOp>(r.U8());
        b.post_op_dw1_mask = r.U32();
        b.used_types = r.U32();
    }
    x.images.resize(r.Count(52 + 4));
    for (ImageResource& i : x.images) {
        GetSharp(r, i.sharp);
        const u8 f = r.U8();
        i.is_depth = (f & 1) != 0;
        i.is_atomic = (f & 2) != 0;
        i.is_array = (f & 4) != 0;
        i.is_written = (f & 8) != 0;
        i.is_r128 = (f & 16) != 0;
        i.constant_mip_index = r.U8();
        i.mip_fallback = static_cast<MipFallback>(r.U8());
        i.post_op = static_cast<SharpPostOp>(r.U8());
    }
    x.samplers.resize(r.Count(52 + 4));
    for (SamplerResource& s : x.samplers) {
        GetSharp(r, s.sharp);
        s.post_op = static_cast<SharpPostOp>(r.U8());
        s.post_op_tsharp_dw3 = r.U16();
        s.is_depth = r.U8() != 0;
    }
    x.fmasks.resize(r.Count(2));
    for (FmaskResource& f : x.fmasks) {
        f.sharp_idx = r.U16();
    }
    x.flatbuf_dwords = r.U32();
    r.Bytes(x.srt_walker);
    GetFetch(r, x.fetch);
    x.user_data_used = r.U16();
    x.mrt_mask = r.U8();
    const u8 f = r.U8();
    x.has_discard = (f & 1) != 0;
    x.uses_dma = (f & 2) != 0;
    x.has_storage_images = (f & 4) != 0;
    x.uses_lane_id = (f & 8) != 0;
    x.uses_shared = (f & 16) != 0;
    x.uses_fp16 = (f & 32) != 0;
    x.uses_fp64 = (f & 64) != 0;
    x.has_bitwise_xor = (f & 128) != 0;
    x.shared_memory_size = r.U32();
    x.tess_consts_dword_offset = static_cast<s32>(r.U32());
    x.start.unified = r.U32();
    x.start.buffer = r.U32();
    x.end.unified = r.U32();
    x.end.buffer = r.U32();
}

/// The module record's first part (what the index keeps): identity .. key.
void PutModuleHead(Out& o, const ProgramIdentity& id, bool layout, u64 footer_hash, const RuntimeParams& runtime,
                   const BindingStart& start, std::span<const u32, 16> user_data, const SpecializationKey& key) {
    PutIdentity(o, id);
    o.U8(layout ? 1 : 0);
    o.U64(footer_hash);
    o.U32(static_cast<u32>(sizeof(RuntimeParams)));
    o.Raw(&runtime, sizeof(RuntimeParams));
    o.U32(start.unified);
    o.U32(start.buffer);
    o.Raw(user_data.data(), 64);
    o.Words(layout ? std::span<const u32>{} : std::span<const u32>(key.words));
    o.U64(layout ? 0 : key.hash);
}
bool GetModuleHead(In& r, DiskModule& m, u64& footer_hash) {
    m.program = GetIdentity(r);
    m.layout = r.U8() != 0;
    footer_hash = r.U64();
    if (r.U32() != sizeof(RuntimeParams)) {
        return false;
    }
    r.Raw(&m.runtime, sizeof(RuntimeParams));
    m.start.unified = r.U32();
    m.start.buffer = r.U32();
    r.Raw(m.user_data.data(), 64);
    r.Words(m.key.words);
    m.key.hash = r.U64();
    return r.ok;
}

bool ReadAt(HANDLE h, u64 offset, void* dst, u32 bytes) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return ReadFile(h, dst, bytes, &got, &ov) != 0 && got == bytes;
}

bool WriteAt(HANDLE h, u64 offset, const void* src, u32 bytes) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD put = 0;
    return WriteFile(h, src, bytes, &put, &ov) != 0 && put == bytes;
}

double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

// ---- PipelineDesc

void PipelineDesc::Serialize(std::vector<u8>& out) const {
    Out o;
    o.U8(compute ? 1 : 0);
    o.U8(has_ps ? 1 : 0);
    o.U64(vs.lo);
    o.U64(vs.hi);
    o.U64(ps.lo);
    o.U64(ps.hi);
    o.U32(static_cast<u32>(sizeof(GraphicsPipelineState)));
    o.Raw(&state, sizeof(GraphicsPipelineState));
    o.U32(static_cast<u32>(sizeof(RuntimeParams)));
    o.Raw(&fs_runtime, sizeof(RuntimeParams));
    o.U32(static_cast<u32>(bindings.size()));
    o.Raw(bindings.data(), bindings.size() * 8);
    out = std::move(o.b);
}

bool PipelineDesc::Deserialize(std::span<const u8> in, size_t& pos) {
    In r(in, pos);
    compute = r.U8() != 0;
    has_ps = r.U8() != 0;
    vs.lo = r.U64();
    vs.hi = r.U64();
    ps.lo = r.U64();
    ps.hi = r.U64();
    if (r.U32() != sizeof(GraphicsPipelineState)) {
        return false;
    }
    r.Raw(&state, sizeof(GraphicsPipelineState));
    if (r.U32() != sizeof(RuntimeParams)) {
        return false;
    }
    r.Raw(&fs_runtime, sizeof(RuntimeParams));
    bindings.resize(r.Count(8));
    r.Raw(bindings.data(), bindings.size() * 8);
    pos = r.pos;
    return r.ok;
}

u64 PipelineDesc::Hash() const {
    std::vector<u8> b;
    Serialize(b);
    return XXH3_64bits(b.data(), b.size());
}

bool PipelineDesc::operator==(const PipelineDesc& o) const {
    return compute == o.compute && has_ps == o.has_ps && vs == o.vs && ps == o.ps &&
           std::memcmp(&state, &o.state, sizeof(state)) == 0 &&
           std::memcmp(&fs_runtime, &o.fs_runtime, sizeof(fs_runtime)) == 0 && bindings == o.bindings;
}

SpirvHash HashSpirv(std::span<const u32> spirv) {
    const XXH128_hash_t h = XXH3_128bits(spirv.data(), spirv.size_bytes());
    return {h.low64, h.high64};
}

std::string ShaderCacheGeneration(const DeviceProfile& profile) {
    DeviceProfile p = profile;
    p.source.clear(); // (the device's name and driver: the SPIR-V depends on the fields, not on them)
    std::vector<u8> bytes;
    SerializeProfile(p, bytes);
    const std::string s = fmt::format("{}|{:016x}|{}", SSPC_NATIVE_SHADER_GENERATION,
                                      XXH3_64bits(bytes.data(), bytes.size()), kFormatVersion);
    return fmt::format("{:016x}", XXH3_64bits(s.data(), s.size()));
}

// ---- the cache

ShaderDiskCache::ShaderDiskCache() = default;

ShaderDiskCache::~ShaderDiskCache() {
    Close();
}

bool ShaderDiskCache::Open(const DiskCacheOptions& options, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    const u64 tsc0 = __rdtsc();
    options_ = options;
    std::error_code ec;
    std::filesystem::create_directories(options_.dir, ec);
    if (ec) {
        error = fmt::format("cannot create {}: {}", options_.dir.string(), ec.message());
        return false;
    }
    if (!options_.read_only) {
        // one writer per directory: a lock file held open without sharing (deleted when this process's handle closes,
        // also when it dies)
        const HANDLE lock = CreateFileW((options_.dir / "lock").wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock != INVALID_HANDLE_VALUE) {
            lock_ = lock;
            writable_ = true;
        } else {
            SSPC_LOG(GPU, Warning, "shader cache: {} is in use by another process - read only for this run",
                     options_.dir.string());
        }
    }
    if (!OpenFile(error) || !Scan(error)) {
        Close();
        return false;
    }
    // SSPC_DIAG_SHADER_CACHE_TEAR=<bytes>: the next record is written only that far and the process ends at once - a
    // crash in the middle of a write (the next start must cut the torn record and keep the rest)
    if (char v[24] = {}; writable_ && GetEnvironmentVariableA("SSPC_DIAG_SHADER_CACHE_TEAR", v, sizeof(v)) > 0) {
        tear_next_ = std::max<u64>(std::strtoull(v, nullptr, 10), 1);
        crash_after_tear_ = true;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_DIAG_SHADER_CACHE_TEAR={} - the next record written is cut and the "
                               "process ends",
                 tear_next_.load());
    }
    stats_.open_ms = MsSince(t0);
    stats_.file_bytes = end_;
    ShaderTrace::Event(ShaderTrace::Kind::CacheLoad, tsc0, __rdtsc(), 0, end_);
    open_ = true;
    stop_ = false;
    if (writable_) {
        writer_ = std::thread([this] { WriterLoop(); });
    }
    return true;
}

bool ShaderDiskCache::OpenFile(std::string& error) {
    const auto path = options_.dir / "shaders.pack";
    const HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | (writable_ ? GENERIC_WRITE : 0),
                                 FILE_SHARE_READ | (writable_ ? 0 : FILE_SHARE_WRITE), nullptr,
                                 writable_ ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = fmt::format("cannot open {} ({})", path.string(), GetLastError());
        return false;
    }
    file_ = h;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    end_ = static_cast<u64>(size.QuadPart);
    if (end_ < sizeof(FileHeader)) {
        return CreateFresh(end_ == 0 ? "new" : "shorter than its header", error);
    }
    FileHeader hd{};
    if (!ReadAt(h, 0, &hd, sizeof(hd))) {
        return CreateFresh("unreadable header", error);
    }
    if (std::memcmp(hd.magic, kFileMagic, sizeof(kFileMagic)) != 0) {
        return CreateFresh("not a shader cache file", error);
    }
    if (hd.hash != XXH3_64bits(&hd, offsetof(FileHeader, hash))) {
        return CreateFresh("damaged header", error);
    }
    if (hd.version != kFormatVersion || hd.header_bytes != sizeof(FileHeader)) {
        return CreateFresh("another format version", error);
    }
    if (std::string_view(hd.generation, strnlen(hd.generation, sizeof(hd.generation))) != options_.generation) {
        return CreateFresh("another generation", error);
    }
    return true;
}

bool ShaderDiskCache::CreateFresh(const char* reason, std::string& error) {
    const bool had = end_ != 0;
    stats_.recreated = true;
    stats_.recreate_reason = reason;
    if (!writable_) {
        error = fmt::format("the cache file is unusable ({}) and this process may not rewrite it", reason);
        return false;
    }
    const auto path = options_.dir / "shaders.pack";
    if (had) {
        // kept once for a look (a damaged or foreign file is not deleted blindly; the next one replaces it)
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
        const auto bad = options_.dir / "shaders.pack.bad";
        MoveFileExW(path.wstring().c_str(), bad.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
        SSPC_LOG(GPU, Warning, "shader cache: {} ({}): kept as {}, a new one started", path.string(), reason,
                 bad.string());
        const HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error = fmt::format("cannot create {} ({})", path.string(), GetLastError());
            return false;
        }
        file_ = h;
    }
    FileHeader hd{};
    std::memcpy(hd.magic, kFileMagic, sizeof(kFileMagic));
    hd.version = kFormatVersion;
    hd.header_bytes = sizeof(FileHeader);
    std::memcpy(hd.generation, options_.generation.data(), std::min(options_.generation.size(), sizeof(hd.generation)));
    hd.created = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    hd.hash = XXH3_64bits(&hd, offsetof(FileHeader, hash));
    const HANDLE h = static_cast<HANDLE>(file_);
    if (!WriteAt(h, 0, &hd, sizeof(hd))) {
        error = fmt::format("cannot write {} ({})", path.string(), GetLastError());
        return false;
    }
    LARGE_INTEGER at{};
    at.QuadPart = sizeof(FileHeader);
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    end_ = sizeof(FileHeader);
    return true;
}

bool ShaderDiskCache::Scan(std::string& error) {
    (void)error;
    const HANDLE h = static_cast<HANDLE>(file_);
    const u64 size = end_;
    u64 pos = sizeof(FileHeader);
    std::vector<u8> payload;
    std::string cut;
    while (pos < size) {
        RecHeader rh{};
        if (size - pos < sizeof(rh) || !ReadAt(h, pos, &rh, sizeof(rh))) {
            cut = "a record header cut short";
            break;
        }
        if (rh.magic != kRecMagic || rh.size > kMaxRecordBytes || pos + sizeof(rh) + rh.size > size) {
            cut = rh.magic != kRecMagic ? "a damaged record header" : "a record cut short";
            break;
        }
        payload.resize(rh.size);
        if (!ReadAt(h, pos + sizeof(rh), payload.data(), rh.size)) {
            cut = "an unreadable record";
            break;
        }
        if (XXH3_64bits(payload.data(), payload.size()) != rh.hash) {
            cut = "a record whose checksum does not match";
            break;
        }
        const u32 index = static_cast<u32>(records_.size());
        records_.push_back({pos, rh.size, rh.type});
        if (!IndexRecord(index, rh.type, payload)) {
            ++stats_.skipped_records; // (a well-formed record of a type or shape this build does not use)
        }
        ++stats_.records;
        pos += sizeof(rh) + rh.size;
    }
    if (pos < size) {
        stats_.torn_bytes = size - pos;
        SSPC_LOG(GPU, Warning, "shader cache: {} at offset {} - the last {} bytes are cut off{}", cut, pos, size - pos,
                 writable_ ? "" : " (not in the file: read only)");
        if (writable_) {
            LARGE_INTEGER at{};
            at.QuadPart = static_cast<LONGLONG>(pos);
            SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
        }
        end_ = pos;
    }
    return true;
}

bool ShaderDiskCache::IndexRecord(u32 index, u8 type, std::span<const u8> payload) {
    In r(payload);
    switch (type) {
    case kProgram: {
        const ProgramIdentity id = GetIdentity(r);
        if (!r.ok) {
            return false;
        }
        programs_[id].has_code = true;
        ++stats_.programs;
        return true;
    }
    case kSpirv: {
        const SpirvHash hash{r.U64(), r.U64()};
        if (!r.ok) {
            return false;
        }
        spirv_.try_emplace(hash, index);
        ++stats_.spirv;
        return true;
    }
    case kModule: {
        DiskModule m;
        u64 footer = 0;
        if (!GetModuleHead(r, m, footer)) {
            return false;
        }
        module_digests_.insert(XXH3_64bits(payload.data(), r.pos));
        programs_[m.program].stubs.push_back({index, m.layout, m.runtime, m.start, std::move(m.key)});
        ++stats_.modules;
        return true;
    }
    case kUsage: {
        const u32 n = r.Count(8);
        if (!r.ok) {
            return false;
        }
        ++runs_;
        for (u32 i = 0; i < n; ++i) {
            Use& u = uses_[r.U64()];
            u.last_run = runs_;
            ++u.runs;
        }
        return r.ok;
    }
    case kPipeline: {
        PipelineDesc d;
        size_t pos = 0;
        if (!d.Deserialize(payload, pos)) {
            return false;
        }
        const u64 hash = d.Hash();
        pipeline_index_.emplace(hash, static_cast<u32>(pipelines_.size()));
        pipelines_.push_back(std::move(d));
        ++stats_.pipelines;
        return true;
    }
    default:
        return false;
    }
}

bool ShaderDiskCache::ReadRecord(u32 index, u8 type, std::vector<u8>& payload) {
    RecordRef ref;
    {
        std::scoped_lock lock{mutex_};
        if (index >= records_.size()) {
            return false;
        }
        ref = records_[index];
    }
    if (ref.offset == 0 || ref.type != type) {
        return false; // (queued in this run, not written yet)
    }
    RecHeader rh{};
    payload.resize(ref.size);
    std::scoped_lock io{io_mutex_};
    const HANDLE h = static_cast<HANDLE>(file_);
    if (h == nullptr || !ReadAt(h, ref.offset, &rh, sizeof(rh)) || rh.magic != kRecMagic || rh.type != type ||
        rh.size != ref.size || !ReadAt(h, ref.offset + sizeof(rh), payload.data(), ref.size)) {
        return false;
    }
    return XXH3_64bits(payload.data(), payload.size()) == rh.hash;
}

std::vector<ModuleStub> ShaderDiskCache::Stubs(const ProgramIdentity& program) const {
    std::scoped_lock lock{mutex_};
    const auto it = programs_.find(program);
    return it != programs_.end() ? it->second.stubs : std::vector<ModuleStub>{};
}

bool ShaderDiskCache::LoadModule(u32 record, DiskModule& out, std::string& error) {
    const u64 t0 = __rdtsc();
    std::vector<u8> payload;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        std::scoped_lock lock{mutex_};
        ++stats_.module_load_failures;
        return false;
    };
    if (!ReadRecord(record, kModule, payload)) {
        return fail(fmt::format("module record {} unreadable or damaged", record));
    }
    In r(payload);
    u64 footer = 0;
    if (!GetModuleHead(r, out, footer)) {
        return fail(fmt::format("module record {} malformed", record));
    }
    out.spirv_hash.lo = r.U64();
    out.spirv_hash.hi = r.U64();
    TranslationResult& res = out.result;
    res = {};
    GetReflection(r, res.reflection);
    r.Words(res.flatbuf);
    r.Words(res.fetch_code);
    res.vertex_sharps.resize(r.Count(16));
    r.Raw(res.vertex_sharps.data(), res.vertex_sharps.size() * 16);
    res.translate_us = r.U64();
    res.emit_us = r.U64();
    if (!r.ok || r.pos != payload.size()) {
        return fail(fmt::format("module record {} malformed", record));
    }
    if (!LoadSpirv(out.spirv_hash, res.spirv)) {
        return fail(fmt::format("module record {}: its SPIR-V is missing or damaged", record));
    }
    {
        std::scoped_lock lock{mutex_};
        ++stats_.module_loads;
    }
    ShaderTrace::Event(ShaderTrace::Kind::DiskLoad, t0, __rdtsc(), footer, res.spirv.size() * 4,
                       static_cast<u32>(out.program.stage));
    return true;
}

bool ShaderDiskCache::LoadSpirv(const SpirvHash& hash, std::vector<u32>& out) {
    u32 index = ~0u;
    {
        std::scoped_lock lock{mutex_};
        if (const auto it = spirv_.find(hash); it != spirv_.end()) {
            index = it->second;
        }
    }
    std::vector<u8> payload;
    bool ok = index != ~0u && ReadRecord(index, kSpirv, payload);
    if (ok) {
        In r(payload);
        const SpirvHash h{r.U64(), r.U64()};
        r.Words(out);
        ok = r.ok && h == hash && HashSpirv(out) == hash; // (the content checked too, not only the record)
    }
    std::scoped_lock lock{mutex_};
    ++(ok ? stats_.spirv_loads : stats_.spirv_load_failures);
    return ok;
}

bool ShaderDiskCache::HasSpirv(const SpirvHash& hash) const {
    std::scoped_lock lock{mutex_};
    return spirv_.contains(hash);
}

u32 ShaderDiskCache::QueueLocked(u8 type, std::vector<u8> payload) {
    const u64 bytes = sizeof(RecHeader) + payload.size();
    if (end_ + queued_bytes_ + bytes > options_.max_bytes) {
        ++stats_.dropped_full;
        return ~0u;
    }
    const u32 index = static_cast<u32>(records_.size());
    records_.push_back({0, static_cast<u32>(payload.size()), type});
    queued_bytes_ += bytes;
    queue_.emplace_back(index, std::move(payload));
    cv_.notify_one();
    return index;
}

void ShaderDiskCache::AddModule(const Program& program, const Module& module, bool layout) {
    if (!writable_ || module.result.spirv.empty()) {
        return;
    }
    Out o;
    PutModuleHead(o, program.id, layout, program.footer_hash, module.runtime, module.start, module.user_data,
                  module.key);
    const u64 digest = XXH3_64bits(o.b.data(), o.b.size());
    {
        std::scoped_lock lock{mutex_};
        if (module_digests_.contains(digest)) {
            return;
        }
    }
    const TranslationResult& res = module.result;
    o.U64(module.spirv_hash.lo);
    o.U64(module.spirv_hash.hi);
    PutReflection(o, res.reflection);
    o.Words(res.flatbuf);
    o.Words(res.fetch_code);
    o.U32(static_cast<u32>(res.vertex_sharps.size()));
    o.Raw(res.vertex_sharps.data(), res.vertex_sharps.size() * 16);
    o.U64(res.translate_us);
    o.U64(res.emit_us);
    std::scoped_lock lock{mutex_};
    if (!module_digests_.insert(digest).second) {
        return;
    }
    ProgramEntry& pe = programs_[program.id];
    if (!pe.has_code) {
        Out p;
        PutIdentity(p, program.id);
        p.U64(program.footer_hash);
        p.Words(program.code);
        if (QueueLocked(kProgram, std::move(p.b)) != ~0u) {
            pe.has_code = true;
            ++stats_.written_programs;
        }
    }
    if (!spirv_.contains(module.spirv_hash)) {
        Out s;
        s.U64(module.spirv_hash.lo);
        s.U64(module.spirv_hash.hi);
        s.Words(res.spirv);
        if (const u32 i = QueueLocked(kSpirv, std::move(s.b)); i != ~0u) {
            spirv_.emplace(module.spirv_hash, i);
            ++stats_.written_spirv;
        }
    }
    if (QueueLocked(kModule, std::move(o.b)) != ~0u) {
        ++stats_.written_modules;
    }
}

std::vector<PipelineDesc> ShaderDiskCache::Pipelines() const {
    std::scoped_lock lock{mutex_};
    return pipelines_;
}

std::vector<PipelineDesc> ShaderDiskCache::PipelinesByUse() const {
    std::scoped_lock lock{mutex_};
    std::vector<std::pair<Use, u32>> order;
    order.reserve(pipelines_.size());
    for (u32 i = 0; i < pipelines_.size(); ++i) {
        const auto it = uses_.find(pipelines_[i].Hash());
        order.emplace_back(it != uses_.end() ? it->second : Use{}, i);
    }
    std::ranges::stable_sort(order, [](const auto& a, const auto& b) {
        return a.first.last_run != b.first.last_run ? a.first.last_run > b.first.last_run : a.first.runs > b.first.runs;
    });
    std::vector<PipelineDesc> out;
    out.reserve(order.size());
    for (const auto& [use, i] : order) {
        out.push_back(pipelines_[i]);
    }
    return out;
}

void ShaderDiskCache::AddUsage(std::span<const u64> hashes) {
    if (!writable_ || hashes.empty()) {
        return;
    }
    Out o;
    o.U32(static_cast<u32>(hashes.size()));
    o.Raw(hashes.data(), hashes.size_bytes());
    std::scoped_lock lock{mutex_};
    QueueLocked(kUsage, std::move(o.b));
}

std::vector<ProgramIdentity> ShaderDiskCache::ProgramIdentities() const {
    std::scoped_lock lock{mutex_};
    std::vector<ProgramIdentity> out;
    out.reserve(programs_.size());
    for (const auto& [id, entry] : programs_) {
        out.push_back(id);
    }
    return out;
}

u32 ShaderDiskCache::Runs() const {
    std::scoped_lock lock{mutex_};
    return runs_;
}

bool ShaderDiskCache::HasPipeline(const PipelineDesc& desc, u64 hash) const {
    std::scoped_lock lock{mutex_};
    const auto [a, b] = pipeline_index_.equal_range(hash);
    return std::any_of(a, b, [&](const auto& kv) { return pipelines_[kv.second] == desc; });
}

void ShaderDiskCache::AddPipeline(const PipelineDesc& desc, u64 hash, u64 create_ns) {
    if (!writable_) {
        return;
    }
    Out o;
    desc.Serialize(o.b);
    o.U64(create_ns);
    std::scoped_lock lock{mutex_};
    const auto [x, y] = pipeline_index_.equal_range(hash);
    if (std::any_of(x, y, [&](const auto& kv) { return pipelines_[kv.second] == desc; })) {
        return;
    }
    if (QueueLocked(kPipeline, std::move(o.b)) != ~0u) {
        pipeline_index_.emplace(hash, static_cast<u32>(pipelines_.size()));
        pipelines_.push_back(desc);
        ++stats_.written_pipelines;
    }
}

void ShaderDiskCache::SetPeriodic(void (*fn)(void*), void* ctx, u32 period_ms) {
    std::scoped_lock lock{mutex_};
    periodic_ = fn;
    periodic_ctx_ = ctx;
    periodic_ms_ = period_ms;
}

bool ShaderDiskCache::WriteOne(u32 index, const std::vector<u8>& payload, u8 type) {
    RecHeader rh{};
    rh.magic = kRecMagic;
    rh.type = type;
    rh.size = static_cast<u32>(payload.size());
    rh.hash = XXH3_64bits(payload.data(), payload.size());
    std::vector<u8> buf(sizeof(rh) + payload.size());
    std::memcpy(buf.data(), &rh, sizeof(rh));
    std::memcpy(buf.data() + sizeof(rh), payload.data(), payload.size());
    std::scoped_lock io{io_mutex_};
    const HANDLE h = static_cast<HANDLE>(file_);
    const u64 offset = end_;
    u32 bytes = static_cast<u32>(buf.size());
    const u64 tear = tear_next_.exchange(0);
    if (tear != 0) {
        bytes = static_cast<u32>(std::min<u64>(tear, bytes)); // (self-test: a write the process's end cut)
    }
    if (h == nullptr || !WriteAt(h, offset, buf.data(), bytes)) {
        return false;
    }
    end_ = offset + bytes;
    if (tear != 0) {
        if (crash_after_tear_) {
            FlushFileBuffers(h);
            TerminateProcess(GetCurrentProcess(), 0xDEAD); // (the diagnostic crash: nothing else is written)
        }
        return false;
    }
    std::scoped_lock lock{mutex_};
    records_[index].offset = offset;
    return true;
}

void ShaderDiskCache::WriterLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:ShaderCacheIO");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // SetPeriodic may run immediately after Open starts this thread. Its first read needs
    // the same mutex as every later read; thread creation does not order later setters.
    std::unique_lock lock{mutex_};
    auto next_periodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(periodic_ms_);
    bool failed = false; // a write failed (or the self-test's cut): nothing more is written, the next open cuts it
    while (true) {
        const auto wait = std::chrono::milliseconds(periodic_ != nullptr ? std::max<u32>(periodic_ms_, 100) : 1000);
        cv_.wait_for(lock, wait, [&] { return stop_ || !queue_.empty(); });
        if (periodic_ != nullptr && !stop_ && std::chrono::steady_clock::now() >= next_periodic) {
            next_periodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(periodic_ms_);
            const auto fn = periodic_;
            void* ctx = periodic_ctx_;
            lock.unlock();
            fn(ctx);
            lock.lock();
        }
        if (queue_.empty()) {
            if (stop_) {
                break;
            }
            continue;
        }
        std::deque<std::pair<u32, std::vector<u8>>> batch;
        batch.swap(queue_);
        writing_ = true;
        std::vector<u8> types;
        types.reserve(batch.size());
        for (const auto& item : batch) {
            types.push_back(records_[item.first].type);
        }
        lock.unlock();
        const u64 t0 = __rdtsc();
        u64 records = 0, bytes = 0, errors = 0;
        for (size_t i = 0; i < batch.size() && !failed; ++i) {
            if (WriteOne(batch[i].first, batch[i].second, types[i])) {
                ++records;
                bytes += sizeof(RecHeader) + batch[i].second.size();
            } else {
                ++errors;
                failed = true;
            }
        }
        ShaderTrace::Event(ShaderTrace::Kind::DiskWrite, t0, __rdtsc(), records, bytes);
        lock.lock();
        for (const auto& item : batch) {
            queued_bytes_ -= sizeof(RecHeader) + item.second.size();
        }
        stats_.written_records += records;
        stats_.written_bytes += bytes;
        stats_.write_errors += errors;
        writing_ = false;
        idle_cv_.notify_all();
    }
    writing_ = false;
    idle_cv_.notify_all();
}

void ShaderDiskCache::Flush() {
    std::unique_lock lock{mutex_};
    if (!writer_.joinable()) {
        return;
    }
    cv_.notify_one();
    idle_cv_.wait(lock, [&] { return queue_.empty() && !writing_; });
}

bool ShaderDiskCache::WithWriteOwnership(const std::function<bool()>& fn) {
    std::scoped_lock io{io_mutex_};
    return lock_ != nullptr && file_ != nullptr && writable_ && fn();
}

void ShaderDiskCache::Close() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
    std::scoped_lock io{io_mutex_};
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    if (lock_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(lock_));
        lock_ = nullptr;
    }
    open_ = false;
}

DiskCacheStats ShaderDiskCache::Stats() const {
    std::scoped_lock lock{mutex_};
    DiskCacheStats s = stats_;
    s.file_bytes = end_;
    return s;
}

} // namespace SecondSon::Gpu::Shader
