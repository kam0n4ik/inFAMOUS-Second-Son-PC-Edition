#include <cstring>
#include <random>

#include "kcap_builder.h"
#include "resources/kcap.h"
#include "test.h"

using namespace sspc;
using namespace sspc::res;
using sspc::test::KcapBuilder;

namespace {

// Package with two pointers in the CPU segment and one into the GPU segment.
KcapBuilder SimpleBuilder() {
    KcapBuilder b;
    b.cpu.resize(0x200, 0);
    b.gpu.resize(0x100, 0xAB);
    b.Put<u32>(b.cpu, 0x10, 0x100);              // pointer -> 0x100
    b.Put<u32>(b.cpu, 0x18, 0x180);              // pointer -> 0x180
    b.Put<u32>(b.cpu, 0x40, b.GpuBase() + 0x20);  // buffer base -> GPU
    b.links = {{0, 0x10}, {0, 0x18}, {2, 0x40}};
    return b;
}

std::vector<u8> Simple() { return SimpleBuilder().Build(); }

KcapError ParseBytes(const std::vector<u8>& bytes) {
    KcapPackage p;
    return p.Parse(bytes);
}

void Write32(std::vector<u8>& f, size_t off, u32 v) { std::memcpy(f.data() + off, &v, 4); }
void Write64(std::vector<u8>& f, size_t off, u64 v) { std::memcpy(f.data() + off, &v, 8); }

}  // namespace

TEST(kcap_valid_synthetic_package_parses) {
    const auto bytes = Simple();
    KcapPackage p;
    CHECK_EQ(p.Parse(bytes), KcapError::Ok);
    CHECK_EQ(p.Segments().size(), static_cast<size_t>(3));
    CHECK_EQ(p.Segments()[1].pool, 1);
    CHECK_EQ(p.Links().size(), static_cast<size_t>(3));
    CHECK_EQ(p.InvalidLinkCount(), 0u);
    CHECK(p.PointerAt(0x10).value_or(0) == 0x100);
    CHECK(p.PointerAt(0x18).value_or(0) == 0x180);
    CHECK(!p.PointerAt(0x40).has_value());  // buffer link is not a CPU pointer
    const KcapLink* l = p.LinkAt(0x40);
    CHECK(l && l->kind == LinkKind::BufferBase && p.SegmentIndexOf(l->target) == 1);
    CHECK(p.FindChunk(kTagEnd) != nullptr);
}

TEST(kcap_rejects_bad_magic_version_and_sizes) {
    auto f = Simple();
    f[0] = 'X';
    CHECK_EQ(ParseBytes(f), KcapError::BadMagic);
    f = Simple();
    f[4] = 12;
    CHECK_EQ(ParseBytes(f), KcapError::UnsupportedVersion);
    f = Simple();
    f.pop_back();
    CHECK_EQ(ParseBytes(f), KcapError::SizeMismatch);  // truncated file
    f = Simple();
    Write32(f, 0x28, static_cast<u32>(f.size() + 1));
    CHECK_EQ(ParseBytes(f), KcapError::BadHeaderSize);
    f = Simple();
    Write32(f, 0x28, 0x10);
    CHECK_EQ(ParseBytes(f), KcapError::BadHeaderSize);
    CHECK_EQ(ParseBytes(std::vector<u8>(0x20, 0)), KcapError::TooSmall);
    CHECK_EQ(ParseBytes({}), KcapError::TooSmall);
}

TEST(kcap_rejects_tables_that_do_not_fit) {
    auto f = Simple();
    Write64(f, 0xa0, 1000);  // segment count
    CHECK_EQ(ParseBytes(f), KcapError::BadSegmentCount);
    f = Simple();
    Write64(f, 0xa0, 0xffffffffffffffffull);  // overflow attempt
    CHECK_EQ(ParseBytes(f), KcapError::BadSegmentCount);
    f = Simple();
    Write64(f, 0xb0, 100000);  // block count
    CHECK_EQ(ParseBytes(f), KcapError::BadBlockCount);
}

TEST(kcap_rejects_out_of_range_segments_and_blocks) {
    auto f = Simple();
    Write32(f, 0xc8 + 4, 0x7fffffff);  // segment 0 size
    CHECK_EQ(ParseBytes(f), KcapError::SegmentOutOfRange);
    f = Simple();
    Write64(f, 0xc8 + 8, 0xfffffff0ull);  // segment 0 offset
    CHECK_EQ(ParseBytes(f), KcapError::SegmentOutOfRange);
    f = Simple();
    Write64(f, 0xc8 + 0x20, 50);  // segment 0 block count
    CHECK_EQ(ParseBytes(f), KcapError::SegmentBlockRange);
    f = Simple();
    const size_t block0 = 0xc8 + 3 * 0x28 + 8;
    Write32(f, block0 + 8, 0xfffffff0u);  // block 0 offset
    CHECK_EQ(ParseBytes(f), KcapError::BlockOutOfRange);
}

TEST(kcap_rejects_broken_service_stream) {
    KcapBuilder b = SimpleBuilder();
    b.terminate_stream = false;
    CHECK_EQ(ParseBytes(b.Build()), KcapError::StreamNotTerminated);

    auto f = Simple();
    KcapPackage p;
    CHECK_EQ(p.Parse(f), KcapError::Ok);
    const u32 header = p.Header().header_size;
    const u32 svc = p.Segments()[2].offset;
    Write32(f, header + svc + 4, 0x10000000);  // KNLI chunk size larger than the segment
    CHECK_EQ(ParseBytes(f), KcapError::ChunkOutOfRange);
    f = Simple();
    Write32(f, header + svc + 8, 1000);  // KNLI sub-stream count
    CHECK_EQ(ParseBytes(f), KcapError::LinkTableInvalid);
    f = Simple();
    Write32(f, header + svc + 12, 9);  // unknown link kind
    CHECK_EQ(ParseBytes(f), KcapError::LinkTableInvalid);
}

TEST(kcap_links_with_out_of_range_targets_are_counted_not_followed) {
    KcapBuilder b = SimpleBuilder();
    b.Put<u32>(b.cpu, 0x20, 0x7ffffff0);  // target far outside the data
    b.links.push_back({0, 0x20});
    KcapPackage p;
    const auto bytes = b.Build();
    CHECK_EQ(p.Parse(bytes), KcapError::Ok);
    CHECK_EQ(p.InvalidLinkCount(), 1u);
    CHECK(!p.PointerAt(0x20).has_value());
}

TEST(kcap_link_stream_decoder_matches_eboot_semantics) {
    // base window 0, offset 0x10/4 = 4; run of 3 after it; new window 1 (0x20000) + 8.
    const u16 codes[] = {0xC000, 4, static_cast<u16>(0x8000 | 3), 0xC001, 2};
    LinkStreamDecoder d(reinterpret_cast<const u8*>(codes), reinterpret_cast<const u8*>(codes) + sizeof(codes));
    u32 v;
    std::vector<u32> got;
    while (d.Next(v)) got.push_back(v);
    const std::vector<u32> want = {0x10, 0x14, 0x18, 0x1c, 0x20008};
    CHECK(got == want);
    // A run code with count 0 still yields one slot (the eboot sets the counter to -1).
    const u16 zero_run[] = {0xC000, 1, 0x8000};
    LinkStreamDecoder z(reinterpret_cast<const u8*>(zero_run), reinterpret_cast<const u8*>(zero_run) + sizeof(zero_run));
    got.clear();
    while (z.Next(v)) got.push_back(v);
    CHECK((got == std::vector<u32>{4, 8}));
    // Without a base code the result is negative and decoding stops (no wrap-around).
    const u16 no_base[] = {0};
    LinkStreamDecoder nb(reinterpret_cast<const u8*>(no_base), reinterpret_cast<const u8*>(no_base) + 2);
    CHECK(!nb.Next(v));
}

TEST(kcap_random_corruption_never_crashes) {
    const auto good = Simple();
    std::mt19937 rng(1234);
    int ok = 0, rejected = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        auto f = good;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k) f[rng() % f.size()] ^= static_cast<u8>(1u << (rng() % 8));
        if (rng() % 10 == 0) f.resize(rng() % f.size());
        KcapPackage p;
        const KcapError e = p.Parse(f);
        if (e == KcapError::Ok) {
            ++ok;
            for (const auto& l : p.Links()) CHECK(p.InRange(l.location, 4) && l.target < p.Data().size());
        } else {
            ++rejected;
        }
    }
    CHECK(ok + rejected == 3000);
    std::printf("    corrupted packages: %d parsed, %d rejected\n", ok, rejected);
}
