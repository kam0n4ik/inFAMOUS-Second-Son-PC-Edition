// SecondSonPC — the resources a GCN program reads, as SecondSonPC's shader frontend resolves them (Phase 4, P4.3;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §7).
//
// A program finds its resources through descriptors: V# (buffer, 4 dwords), T# (image, 8 or 4 dwords), S# (sampler,
// 4 dwords). They are in the user data registers ("embedded"), or in guest memory tables the user data points to
// (pointer-based, the shader resource table walked by the program's SRT walk), or — for vertex attributes — in the
// vertex buffer table the fetch shader loads from. The translator reports where each descriptor is (the reflection's
// SharpLocation in the flattened user data); this module turns that, the draw's user data and guest memory into a
// ResourceBindingPlan for the executor (P4.4) and a SpecializationKey for the module cache.
//
// Nothing here dereferences a guest address. The SRT walk is the translator's x86 walker, decoded once into micro-ops
// and interpreted with bounded reads (the bridge runs the same code natively, an access violation handler patching a
// faulting load into a zero for good; SecondSonPC reads through the memory contract: a read it cannot make yields 0
// and is counted, and the next walk reads again). A descriptor that cannot be resolved is reported with its reason,
// never replaced by a silent null.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

// ---- descriptors (Sea Islands ISA tables 8.5 V#, 8.11 T#, 8.12 S#)

struct BufferDesc {
    std::array<u32, 4> raw{};
    u64 Base() const { return raw[0] | (u64{raw[1] & 0xFFu} << 32); } ///< 40 bits
    u32 Stride() const { return (raw[1] >> 16) & 0x3FFF; }
    bool CacheSwizzle() const { return (raw[1] >> 30) & 1; }
    bool SwizzleEnable() const { return (raw[1] >> 31) & 1; }
    u32 NumRecords() const { return raw[2]; }
    u32 DstSel(u32 c) const { return (raw[3] >> (3 * c)) & 7; }
    u32 NumFormat() const { return (raw[3] >> 12) & 7; }
    u32 DataFormat() const { return (raw[3] >> 15) & 0xF; }
    u32 ElementSize() const { return (raw[3] >> 19) & 3; }
    u32 IndexStride() const { return (raw[3] >> 21) & 3; }
    bool AddTidEnable() const { return (raw[3] >> 23) & 1; }
    u32 Mtype() const { return (raw[3] >> 27) & 7; }
    u32 Type() const { return raw[3] >> 30; } ///< 0 for a buffer (the T#'s type field overlaps)
    bool IsBuffer() const { return Type() == 0; }
    /// Bytes the descriptor covers: NUM_RECORDS bytes when STRIDE is 0, else STRIDE * NUM_RECORDS.
    u64 Size() const { return Stride() == 0 ? NumRecords() : u64{Stride()} * NumRecords(); }
};

struct ImageDesc {
    std::array<u32, 8> raw{};
    u64 Base() const { return (raw[0] | (u64{raw[1] & 0x3Fu} << 32)) << 8; } ///< 38-bit field, 256-byte units
    u32 DataFormat() const { return (raw[1] >> 20) & 0x3F; }
    u32 NumFormat() const { return (raw[1] >> 26) & 0xF; }
    u32 Width() const { return (raw[2] & 0x3FFF) + 1; }
    u32 Height() const { return ((raw[2] >> 14) & 0x3FFF) + 1; }
    u32 DstSel(u32 c) const { return (raw[3] >> (3 * c)) & 7; }
    u32 BaseLevel() const { return (raw[3] >> 12) & 0xF; }
    u32 LastLevel() const { return (raw[3] >> 16) & 0xF; }
    u32 TilingIndex() const { return (raw[3] >> 20) & 0x1F; }
    bool Pow2Pad() const { return (raw[3] >> 25) & 1; }
    u32 Type() const { return raw[3] >> 28; } ///< 8..15 for an image (bit 3 set)
    bool IsImage() const { return (Type() & 8) != 0; }
    u32 Depth() const { return (raw[4] & 0x1FFF) + 1; }
    u32 Pitch() const { return ((raw[4] >> 13) & 0x3FFF) + 1; }
    u32 BaseArray() const { return raw[5] & 0x1FFF; }
    u32 LastArray() const { return (raw[5] >> 13) & 0x1FFF; }
};
inline constexpr u32 kImageType2D = 9, kImageType2DArray = 13, kImageTypeCube = 11;
inline constexpr u32 kFirstFmaskFormat = 44, kLastFmaskFormat = 56;

struct SamplerDesc {
    std::array<u32, 4> raw{};
    u32 ClampX() const { return raw[0] & 7; }
    u32 ClampY() const { return (raw[0] >> 3) & 7; }
    u32 ClampZ() const { return (raw[0] >> 6) & 7; }
    u32 MaxAniso() const { return (raw[0] >> 9) & 7; }
    u32 DepthCompare() const { return (raw[0] >> 12) & 7; }
    bool ForceUnnormalized() const { return (raw[0] >> 15) & 1; }
    bool ForceDegamma() const { return (raw[0] >> 20) & 1; }
    u32 FilterMode() const { return (raw[0] >> 29) & 3; }
    u32 MinLod() const { return raw[1] & 0xFFF; }
    u32 MaxLod() const { return (raw[1] >> 12) & 0xFFF; }
    u32 MagFilter() const { return (raw[2] >> 20) & 3; }
    u32 MinFilter() const { return (raw[2] >> 22) & 3; }
    u32 MipFilter() const { return (raw[2] >> 26) & 3; }
    u32 BorderColorPtr() const { return raw[3] & 0xFFF; }
    u32 BorderColorType() const { return raw[3] >> 30; }
    /// The fields' value ranges (what the bridge's Sampler::Valid checks).
    bool InRange() const { return MaxAniso() <= 4 && FilterMode() <= 2 && MipFilter() <= 2; }
    bool Empty() const { return (raw[0] | raw[1] | raw[2] | raw[3]) == 0; }
};

// ---- the fetch shader

/// Parses a fetch shader's code (the driver's form: S_LOAD_DWORDX4 of V#s, BUFFER/TBUFFER_LOAD_FORMAT, V_ADD_I32 of
/// the vertex/instance offset, S_WAITCNT, V_MOV_B32 of 1.0, ending with S_SETPC_B64). false with `error` for any
/// other instruction or no S_SETPC_B64 inside `code` (the bridge asserts in both cases).
bool ParseFetchShaderCode(std::span<const u32> code, u32 sgpr_base, FetchShader& out, std::string& error);

// ---- the SRT walk

/// The translator's SRT walker (x86 it generated), decoded once into micro-ops and interpreted with bounded reads.
class SrtProgram {
public:
    /// Decodes `code`. false with `error` (naming the instruction) for anything the generator does not emit.
    bool Compile(std::span<const u8> code, std::string& error);
    bool Empty() const { return ops_.empty(); }
    size_t Size() const { return ops_.size(); }

    struct RunStats {
        u32 reads = 0;  ///< guest memory reads
        u32 faults = 0; ///< reads the memory refused (each yields 0, as the bridge's patched load does)
        u32 user_data_read = 0; ///< bit i: the walk read user data register i (P5.3 Native Render Frontend)
        bool user_data_written = false; ///< the walk stored into the flattened data's first 16 dwords (the registers)
    };
    /// Runs the walk: `user_data` (16 registers), `flat` the flattened buffer (its first 16 dwords already the user
    /// data). false with `error` for an out-of-range flat/user data offset or stack misuse (nothing is written past
    /// `flat`).
    bool Run(std::span<const u32, 16> user_data, std::span<u32> flat, GuestReader& memory, RunStats& stats,
             std::string& error) const;

    struct Op {
        u8 code = 0;
        s32 disp = 0;
        u64 imm = 0;
    };

private:
    std::vector<Op> ops_;
};

// ---- resolution

enum class ResourceStatus : u8 {
    Ok,
    Null,        ///< no descriptor (all zero / NUM_RECORDS 0 / base 0): the executor binds a null resource
    WrongType,   ///< the type bits say it is not this kind (a T# where a V# is read, ...)
    OutOfRange,  ///< its memory leaves the 40-bit GPU space
    BadLocation, ///< the reflection points outside the flattened user data
    Unresolved,  ///< a read on the way to it failed (SRT walk, vertex buffer table)
    Count,
};
std::string_view ResourceStatusName(ResourceStatus status);

struct BufferBinding {
    u32 binding = 0;
    BufferKind kind = BufferKind::Guest;
    BufferDesc desc;
    u64 address = 0; ///< after the post-op (inline constant buffers: + the program's address)
    u64 size = 0;
    bool written = false;
    bool formatted = false;
    ResourceStatus status = ResourceStatus::Ok;
};
struct ImageBinding {
    u32 binding = 0;
    u32 count = 1; ///< bindings (a storage image with a dynamic mip index binds every level)
    ImageDesc desc;
    u32 view_type = 0;
    bool storage = false, depth = false, array = false, atomic = false, fmask = false;
    ResourceStatus status = ResourceStatus::Ok;
};
struct SamplerBinding {
    u32 binding = 0;
    SamplerDesc desc;
    ResourceStatus status = ResourceStatus::Ok;
};
struct VertexBinding {
    u32 attribute = 0;
    FetchAttribute fetch;
    std::array<u32, 4> read{}; ///< the V# as read from the vertex buffer table (before a typed load's format)
    BufferDesc desc;
    u64 address = 0;
    u64 size = 0;
    ResourceStatus status = ResourceStatus::Ok;
};

/// Everything a stage's resources resolve to for one draw or dispatch (reused between draws: no allocation once warm).
struct ResourceBindingPlan {
    HwStage stage = HwStage::Vs;
    std::vector<u32> flat; ///< the flattened user data
    std::vector<BufferBinding> buffers;
    std::vector<ImageBinding> images;
    std::vector<SamplerBinding> samplers;
    std::vector<VertexBinding> vertices;
    std::array<u32, static_cast<size_t>(ResourceStatus::Count)> status_counts{};
    SrtProgram::RunStats srt;
    std::string error; ///< a failed SRT walk
    void Clear();
};

/// Where the vertex attributes' V#s come from: the vertex buffer table in guest memory (the draw), or the V#s a
/// translation read (its result, to key the module it produced on exactly what it saw).
struct VertexSource {
    GuestReader* memory = nullptr;
    std::span<const std::array<u32, 4>> given;
};

/// Resolves `reflection`'s resources for `user_data`: the SRT walk (`srt`, compiled from the reflection's walker)
/// into plan.flat, then every descriptor. `start`: the stage's first bindings in this pipeline. `fetch`: the draw's
/// fetch shader (its attributes; nullptr: none). `program_address`: the program's guest address (inline constant
/// buffers). false only when the SRT walk itself failed (error set); unresolvable descriptors are reported per
/// resource.
bool ResolveResources(const ShaderReflection& reflection, const SrtProgram& srt, const BindingStart& start,
                      const FetchShader* fetch, std::span<const u32, 16> user_data, u64 program_address,
                      GuestReader& memory, ResourceBindingPlan& plan);

/// The same from a flattened user data already made (a translation's), the V#s from `vertices`.
void DecodeResources(const ShaderReflection& reflection, const BindingStart& start, const FetchShader* fetch,
                     std::span<const u32, 16> user_data, u64 program_address, const VertexSource& vertices,
                     ResourceBindingPlan& plan);

/// The bindings the next stage starts at.
BindingStart EndOf(const BindingStart& start, const ResourceBindingPlan& plan);

/// A descriptor from the flattened user data (SingleLoad: consecutive dwords; MultiLoad: per dword an offset or an
/// immediate). false: the location is invalid or outside `flat`.
bool FetchSharp(const SharpLocation& location, std::span<const u32> flat, std::span<u32> out);

// ---- specialization

/// The words a module depends on beyond its code: the stage's runtime parameters, its first bindings, and the
/// translation-relevant fields of every descriptor it reads (docs §6.3). Equal keys (hash, then every word) mean the
/// translator produces the same module; the key is a superset of the bridge's StageSpecialization (it adds the fields
/// the translator reads that the bridge's key omits: storage image format, buffer ADD_TID_ENABLE/MTYPE, FMask size).
struct SpecializationKey {
    std::vector<u32> words;
    u64 hash = 0;
    bool operator==(const SpecializationKey& other) const { return hash == other.hash && words == other.words; }
};
void BuildSpecializationKey(const RuntimeParams& runtime, const BindingStart& start, const ShaderReflection& reflection,
                            const FetchShader* fetch, const ResourceBindingPlan& plan, SpecializationKey& out);

} // namespace SecondSon::Gpu::Shader
