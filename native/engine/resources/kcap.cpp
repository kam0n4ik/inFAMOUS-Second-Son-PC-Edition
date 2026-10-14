#include "resources/kcap.h"

#include <algorithm>
#include <cstring>

namespace sspc::res {
namespace {

constexpr u32 kMagic = 0x5041434b;  // "KCAP"
constexpr u16 kVersion = 13;
constexpr u32 kSegmentTable = 0xc8;
constexpr u32 kSegmentSize = 0x28;
constexpr u32 kBlockSize = 12;
constexpr u32 kMaxSegments = 64;
constexpr u32 kMaxBlocks = 1u << 20;

template <class T>
T Load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

}  // namespace

const char* ToString(KcapError e) {
    switch (e) {
        case KcapError::Ok: return "ok";
        case KcapError::TooSmall: return "file too small for a KCAP header";
        case KcapError::BadMagic: return "bad magic (not KCAP)";
        case KcapError::UnsupportedVersion: return "unsupported KCAP version";
        case KcapError::BadHeaderSize: return "header size out of range";
        case KcapError::SizeMismatch: return "header size + data size does not match the file";
        case KcapError::BadSegmentCount: return "segment table does not fit";
        case KcapError::BadBlockCount: return "block table does not fit";
        case KcapError::SegmentOutOfRange: return "segment outside the data region";
        case KcapError::SegmentBlockRange: return "segment block range outside the block table";
        case KcapError::BlockOutOfRange: return "block outside the data region";
        case KcapError::ChunkOutOfRange: return "service chunk outside its segment";
        case KcapError::StreamNotTerminated: return "service stream has no END chunk";
        case KcapError::LinkTableInvalid: return "KNLI link table malformed";
        case KcapError::CannotOpen: return "cannot open file";
    }
    return "unknown";
}

bool LinkStreamDecoder::Next(u32& out) {
    if (run_ > 0) {
        --run_;
        last_ += 4;
        out = static_cast<u32>(last_);
        return true;
    }
    while (end_ - cur_ >= 2) {
        const s16 v = Load<s16>(cur_);
        cur_ += 2;
        const u32 code = static_cast<u16>(v);
        if (v >= 0) {
            last_ = static_cast<s32>(code) * 4 + base_;
            out = static_cast<u32>(last_);
            return last_ >= 0;
        }
        if ((code & 0x4000) == 0) {
            // Run of (code & 0x3fff) consecutive slots; the eboot returns the first one even when
            // the count is 0 (run_ becomes -1).
            run_ = static_cast<s32>(code & 0x3fff) - 1;
            last_ += 4;
            out = static_cast<u32>(last_);
            return last_ >= 0;
        }
        base_ = static_cast<s32>((code & 0x3fff) << 17);
    }
    return false;
}

KcapError KcapPackage::Parse(std::span<const u8> file) {
    *this = KcapPackage{};
    if (file.size() < kSegmentTable) return KcapError::TooSmall;
    const u8* p = file.data();
    if (Load<u32>(p) != kMagic) return KcapError::BadMagic;
    header_.version = Load<u16>(p + 4);
    header_.kind = Load<u16>(p + 6);
    if (header_.version != kVersion) return KcapError::UnsupportedVersion;
    header_.header_size = Load<u32>(p + 0x28);
    header_.data_size = Load<u32>(p + 0x2c);
    std::memcpy(header_.hash.data(), p + 0x78, 16);
    if (header_.header_size < kSegmentTable || header_.header_size > file.size()) {
        return KcapError::BadHeaderSize;
    }
    if (static_cast<u64>(header_.header_size) + header_.data_size != file.size()) {
        return KcapError::SizeMismatch;
    }
    const u64 seg_count = Load<u64>(p + 0xa0);
    const u64 block_count = Load<u64>(p + 0xb0);
    if (seg_count > kMaxSegments ||
        kSegmentTable + seg_count * kSegmentSize + 8 > header_.header_size) {
        return KcapError::BadSegmentCount;
    }
    const u64 block_table = kSegmentTable + seg_count * kSegmentSize + 8;
    if (block_count > kMaxBlocks || block_table + block_count * kBlockSize > header_.header_size) {
        return KcapError::BadBlockCount;
    }
    header_.segment_count = static_cast<u32>(seg_count);
    header_.block_count = static_cast<u32>(block_count);
    header_.root_size = Load<u64>(p + kSegmentTable + seg_count * kSegmentSize);
    data_ = file.subspan(header_.header_size, header_.data_size);

    blocks_.resize(header_.block_count);
    for (u32 i = 0; i < header_.block_count; ++i) {
        const u8* b = p + block_table + static_cast<u64>(i) * kBlockSize;
        KcapBlock& blk = blocks_[i];
        blk.align = Load<u16>(b);
        blk.type = Load<u16>(b + 2);
        blk.size = Load<u32>(b + 4);
        blk.offset = Load<u32>(b + 8);
        if (!InRange(blk.offset, blk.size)) return KcapError::BlockOutOfRange;
    }

    segments_.resize(header_.segment_count);
    for (u32 i = 0; i < header_.segment_count; ++i) {
        const u8* s = p + kSegmentTable + static_cast<u64>(i) * kSegmentSize;
        KcapSegment& seg = segments_[i];
        seg.flags = s[0];
        seg.pool = Load<u16>(s + 2);
        seg.size = Load<u32>(s + 4);
        const u64 offset = Load<u64>(s + 8);
        const u64 first = Load<u64>(s + 0x18);
        const u64 count = Load<u64>(s + 0x20);
        if (offset > header_.data_size || !InRange(offset, seg.size)) return KcapError::SegmentOutOfRange;
        if (first > header_.block_count || count > header_.block_count - first) {
            return KcapError::SegmentBlockRange;
        }
        seg.offset = static_cast<u32>(offset);
        seg.first_block = static_cast<u32>(first);
        seg.block_count = static_cast<u32>(count);
    }

    for (const KcapSegment& seg : segments_) {
        if (seg.pool == 2) {
            if (KcapError e = ParseServiceStream(seg); e != KcapError::Ok) return e;
        }
    }
    for (const KcapChunk& c : chunks_) {
        if (c.tag == kTagKnli) {
            if (KcapError e = ParseLinks(c); e != KcapError::Ok) return e;
        }
    }
    std::sort(links_.begin(), links_.end(),
              [](const KcapLink& a, const KcapLink& b) { return a.location < b.location; });
    return KcapError::Ok;
}

KcapError KcapPackage::ParseServiceStream(const KcapSegment& seg) {
    u64 o = seg.offset;
    const u64 end = static_cast<u64>(seg.offset) + seg.size;
    while (o + 8 <= end) {
        KcapChunk c;
        c.tag = Load<u32>(data_.data() + o);
        c.size = Load<u32>(data_.data() + o + 4);
        c.payload = static_cast<u32>(o + 8);
        if (c.size > end - (o + 8)) return KcapError::ChunkOutOfRange;
        chunks_.push_back(c);
        if (c.tag == kTagEnd) return KcapError::Ok;
        o += 8 + static_cast<u64>(c.size);
    }
    return KcapError::StreamNotTerminated;
}

KcapError KcapPackage::ParseLinks(const KcapChunk& chunk) {
    const u8* payload = data_.data() + chunk.payload;
    if (chunk.size < 4) return KcapError::LinkTableInvalid;
    const u32 n = Load<u32>(payload);
    if (n > 64 || 4 + static_cast<u64>(n) * 8 > chunk.size) return KcapError::LinkTableInvalid;
    u64 stream = 4 + static_cast<u64>(n) * 8;
    for (u32 i = 0; i < n; ++i) {
        const u32 kind = Load<u32>(payload + 4 + i * 8);
        const s32 len16 = Load<s32>(payload + 8 + i * 8);
        if (kind > 4 || len16 < 0 || stream + static_cast<u64>(len16) * 2 > chunk.size) {
            return KcapError::LinkTableInvalid;
        }
        LinkStreamDecoder dec(payload + stream, payload + stream + static_cast<u64>(len16) * 2);
        const u32 field = (kind == 0 || kind == 3) ? 8 : 4;
        for (u32 loc; dec.Next(loc);) {
            u32 target = 0;
            if (!InRange(loc, field) || !Read(loc, target) || target >= data_.size()) {
                ++invalid_links_;
                continue;
            }
            links_.push_back({loc, target, static_cast<LinkKind>(kind)});
        }
        stream += static_cast<u64>(len16) * 2;
    }
    return KcapError::Ok;
}

int KcapPackage::SegmentIndexOf(u32 offset) const {
    for (size_t i = 0; i < segments_.size(); ++i) {
        const auto& s = segments_[i];
        if (offset >= s.offset && offset - s.offset < s.size) return static_cast<int>(i);
    }
    return -1;
}

const KcapLink* KcapPackage::LinkAt(u32 location) const {
    auto it = std::lower_bound(links_.begin(), links_.end(), location,
                               [](const KcapLink& l, u32 v) { return l.location < v; });
    return (it != links_.end() && it->location == location) ? &*it : nullptr;
}

std::optional<u32> KcapPackage::PointerAt(u32 location) const {
    const KcapLink* l = LinkAt(location);
    if (!l || (l->kind != LinkKind::Pointer && l->kind != LinkKind::Pointer64)) return std::nullopt;
    return l->target;
}

const KcapChunk* KcapPackage::FindChunk(u32 tag) const {
    for (const auto& c : chunks_) {
        if (c.tag == tag) return &c;
    }
    return nullptr;
}

KcapError KcapFile::Open(const std::wstring& path) {
    if (!map_.Open(path)) return KcapError::CannotOpen;
    return package_.Parse(map_.Bytes());
}

}  // namespace sspc::res
