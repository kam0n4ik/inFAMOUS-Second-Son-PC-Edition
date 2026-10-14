// SecondSonPC native runtime - KCAP package reader (.xpps files of inFAMOUS Second Son).
//
// A KCAP package is a relocatable memory image: a header with segment and block tables, then the
// data region (all segments back to back). Pointers inside the data are stored as 32-bit
// data-relative offsets and listed in a service stream (pool-2 segment) that the game walks at
// load time (eboot 0x30a310). Format facts and their evidence: docs/native/RESOURCE_FORMATS.md.
//
// The reader never executes or patches anything: it validates every table against the file size
// and exposes the link table so that tools and loaders can follow pointers safely.
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/file.h"
#include "core/types.h"

namespace sspc::res {

enum class KcapError : u8 {
    Ok = 0,
    TooSmall,
    BadMagic,
    UnsupportedVersion,
    BadHeaderSize,
    SizeMismatch,
    BadSegmentCount,
    BadBlockCount,
    SegmentOutOfRange,
    SegmentBlockRange,
    BlockOutOfRange,
    ChunkOutOfRange,
    StreamNotTerminated,
    LinkTableInvalid,
    CannotOpen,
};
const char* ToString(KcapError e);

// Service stream chunk tags (u32 as stored, i.e. the ASCII text reversed in the file).
inline constexpr u32 kTagDic = 0x43494420;   // ' DIC' type-hash fixups
inline constexpr u32 kTagKnli = 0x494c4e4b;  // 'KNLI' internal links (eboot 0x30a4a0)
inline constexpr u32 kTagKnlx = 0x584c4e4b;  // 'KNLX' external links (eboot 0x30a700)
inline constexpr u32 kTagPyto = 0x4f545950;  // 'PYTO' type-index fixups (eboot 0x30a910)
inline constexpr u32 kTagPlps = 0x53504c50;  // 'PLPS' property splices (eboot 0x30b1c0)
inline constexpr u32 kTagEnd = 0x454e4420;   // ' DNE' end of stream

struct KcapHeader {
    u16 version = 0;
    u16 kind = 0;
    u32 header_size = 0;
    u32 data_size = 0;
    std::array<u8, 16> hash{};
    u32 segment_count = 0;
    u32 block_count = 0;
    u64 root_size = 0;  // u64 after the segment table; equals the size of the unblocked root region
};

struct KcapSegment {
    u8 flags = 0;
    u16 pool = 0;  // 0 CPU, 1 GPU, 2 service
    u32 size = 0;
    u32 offset = 0;  // data-relative
    u32 first_block = 0;
    u32 block_count = 0;
};

struct KcapBlock {
    u16 align = 0;
    u16 type = 0;
    u32 size = 0;
    u32 offset = 0;  // data-relative
};

struct KcapChunk {
    u32 tag = 0;
    u32 payload = 0;  // data-relative offset of the payload
    u32 size = 0;
};

// What the game writes at a link location after relocation (eboot 0x30a4a0).
enum class LinkKind : u8 {
    Pointer = 0,      // 64-bit CPU pointer (u32 offset stored in the low half)
    TextureBase = 1,  // T# base address (address >> 8)
    BufferBase = 2,   // V# base address, 48 bits split over dword0 and dword1[11:0]
    Pointer64 = 3,    // 64-bit pointer
    Address256 = 4,   // u32 = address >> 8
};

struct KcapLink {
    u32 location = 0;  // data-relative offset of the patched field
    u32 target = 0;    // data-relative offset the field points to
    LinkKind kind = LinkKind::Pointer;
};

class KcapPackage {
public:
    // Parses and validates. The bytes must outlive the package (it keeps a view).
    KcapError Parse(std::span<const u8> file);

    const KcapHeader& Header() const { return header_; }
    std::span<const u8> Data() const { return data_; }
    const std::vector<KcapSegment>& Segments() const { return segments_; }
    const std::vector<KcapBlock>& Blocks() const { return blocks_; }
    const std::vector<KcapChunk>& Chunks() const { return chunks_; }
    const std::vector<KcapLink>& Links() const { return links_; }  // sorted by location
    // Links whose location or target fell outside the data region (the game skips/nulls them).
    u32 InvalidLinkCount() const { return invalid_links_; }

    int SegmentIndexOf(u32 offset) const;
    bool InRange(u64 offset, u64 size) const { return offset <= data_.size() && size <= data_.size() - offset; }

    template <class T>
    bool Read(u64 offset, T& out) const {
        if (!InRange(offset, sizeof(T))) return false;
        std::memcpy(&out, data_.data() + offset, sizeof(T));
        return true;
    }

    const KcapLink* LinkAt(u32 location) const;
    // Target of a pointer link (kind Pointer/Pointer64) stored at `location`.
    std::optional<u32> PointerAt(u32 location) const;
    const KcapChunk* FindChunk(u32 tag) const;

private:
    KcapError ParseServiceStream(const KcapSegment& seg);
    KcapError ParseLinks(const KcapChunk& chunk);

    KcapHeader header_;
    std::span<const u8> data_;
    std::vector<KcapSegment> segments_;
    std::vector<KcapBlock> blocks_;
    std::vector<KcapChunk> chunks_;
    std::vector<KcapLink> links_;
    u32 invalid_links_ = 0;
};

// Package together with its memory-mapped file.
class KcapFile {
public:
    KcapError Open(const std::wstring& path);
    const KcapPackage& Package() const { return package_; }

private:
    file::MappedFile map_;
    KcapPackage package_;
};

// Decoder of one KNLI sub-stream (eboot 0x5d2260): u16 codes -> data offsets, 4-byte granular.
// Returns false at the end of the stream.
class LinkStreamDecoder {
public:
    LinkStreamDecoder(const u8* begin, const u8* end) : cur_(begin), end_(end) {}
    bool Next(u32& out);

private:
    const u8* cur_;
    const u8* end_;
    s32 base_ = -1;
    s32 last_ = -1;
    s32 run_ = 0;
};

}  // namespace sspc::res
