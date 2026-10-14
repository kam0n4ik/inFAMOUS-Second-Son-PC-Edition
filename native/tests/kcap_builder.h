// Builds synthetic KCAP packages for tests, following the layout documented in
// docs/native/RESOURCE_FORMATS.md (header, segment table at 0xC8, root-size qword, 12-byte blocks,
// data region with a pool-2 service stream holding KNLI and END chunks).
#pragma once

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "core/types.h"

namespace sspc::test {

class KcapBuilder {
public:
    // Segment payloads (data offsets: cpu at 0, gpu after cpu aligned to 0x100, service after gpu).
    std::vector<u8> cpu, gpu;
    // Links: {kind, location}; the 32-bit value at `location` must already hold the target offset.
    std::vector<std::pair<u32, u32>> links;
    bool terminate_stream = true;
    u32 virtual_block_size = 0;  // > 0: add a type-9 block after the GPU segment (no file data)

    u32 GpuBase() const { return Align(static_cast<u32>(cpu.size()), 0x100); }

    template <class T>
    void Put(std::vector<u8>& seg, u32 off, const T& v) {
        if (seg.size() < off + sizeof(T)) seg.resize(off + sizeof(T));
        std::memcpy(seg.data() + off, &v, sizeof(T));
    }

    static std::vector<u8> EncodeLinkStream(std::vector<u32> locations) {
        std::sort(locations.begin(), locations.end());
        std::vector<u8> out;
        auto push = [&](u16 v) {
            out.push_back(static_cast<u8>(v));
            out.push_back(static_cast<u8>(v >> 8));
        };
        s64 base = -1;
        for (u32 loc : locations) {
            const s64 window = static_cast<s64>(loc >> 17) << 17;
            if (window != base) {
                push(static_cast<u16>(0xC000 | (loc >> 17)));
                base = window;
            }
            push(static_cast<u16>((loc - base) / 4));
        }
        return out;
    }

    std::vector<u8> Build() const {
        const u32 gpu_base = GpuBase();
        std::vector<u8> data(gpu_base + gpu.size(), 0);
        std::memcpy(data.data(), cpu.data(), cpu.size());
        if (!gpu.empty()) std::memcpy(data.data() + gpu_base, gpu.data(), gpu.size());
        const u32 svc_base = Align(static_cast<u32>(data.size()), 0x10);
        data.resize(svc_base, 0);

        // Service stream: KNLI {count, (kind, len16)...} streams..., END.
        std::vector<u8> svc;
        auto put32 = [&](std::vector<u8>& b, u32 v) {
            b.insert(b.end(), reinterpret_cast<u8*>(&v), reinterpret_cast<u8*>(&v) + 4);
        };
        std::vector<u8> knli;
        std::vector<std::vector<u8>> streams;
        std::vector<u32> kinds;
        for (u32 kind = 0; kind <= 4; ++kind) {
            std::vector<u32> locs;
            for (auto& [k, l] : links) {
                if (k == kind) locs.push_back(l);
            }
            if (locs.empty()) continue;
            kinds.push_back(kind);
            streams.push_back(EncodeLinkStream(locs));
        }
        put32(knli, static_cast<u32>(kinds.size()));
        for (size_t i = 0; i < kinds.size(); ++i) {
            put32(knli, kinds[i]);
            put32(knli, static_cast<u32>(streams[i].size() / 2));
        }
        for (const auto& s : streams) knli.insert(knli.end(), s.begin(), s.end());
        while (knli.size() % 4) knli.push_back(0);
        put32(svc, 0x494c4e4b);
        put32(svc, static_cast<u32>(knli.size()));
        svc.insert(svc.end(), knli.begin(), knli.end());
        if (terminate_stream) {
            put32(svc, 0x454e4420);
            put32(svc, 0);
        }
        data.insert(data.end(), svc.begin(), svc.end());

        struct Seg {
            u8 flags;
            u16 pool;
            u32 size, offset;
            u16 block_type;
        };
        std::vector<Seg> segs = {{0x01, 0, static_cast<u32>(cpu.size()), 0, 1}};
        if (!gpu.empty()) segs.push_back({0x12, 1, static_cast<u32>(gpu.size()), gpu_base, 18});
        segs.push_back({0x03, 2, static_cast<u32>(svc.size()), svc_base, 2});
        const u32 seg_count = static_cast<u32>(segs.size());
        // One block per segment, plus an optional load-time (virtual, type 9) block after the GPU data.
        const u32 block_count = seg_count + (virtual_block_size ? 1 : 0);
        const u32 header_size = 0xc8 + seg_count * 0x28 + block_count * 12;
        std::vector<u8> file(header_size, 0);
        auto w16 = [&](u32 o, u16 v) { std::memcpy(file.data() + o, &v, 2); };
        auto w32 = [&](u32 o, u32 v) { std::memcpy(file.data() + o, &v, 4); };
        auto w64 = [&](u32 o, u64 v) { std::memcpy(file.data() + o, &v, 8); };
        std::memcpy(file.data(), "KCAP", 4);
        w16(4, 13);
        w16(6, 14);
        w32(0x18, 0x98);
        w32(0x1c, header_size - 0x98);
        w32(0x28, header_size);
        w32(0x2c, static_cast<u32>(data.size()));
        w64(0xa0, seg_count);
        w64(0xb0, block_count);
        u32 next_block = 0;
        for (u32 i = 0; i < seg_count; ++i) {
            const u32 o = 0xc8 + i * 0x28;
            const bool with_virtual = virtual_block_size && segs[i].pool == 1;
            file[o] = segs[i].flags;
            file[o + 1] = 0xff;
            w16(o + 2, segs[i].pool);
            w32(o + 4, segs[i].size);
            w64(o + 8, segs[i].offset);
            w64(o + 0x18, next_block);
            w64(o + 0x20, with_virtual ? 2 : 1);
            const u32 bo = 0xc8 + seg_count * 0x28 + next_block * 12;
            w32(bo, segs[i].size);
            w32(bo + 4, segs[i].offset);
            w16(bo + 8, 0x10);
            w16(bo + 10, segs[i].block_type);
            ++next_block;
            if (with_virtual) {
                const u32 vo = 0xc8 + seg_count * 0x28 + next_block * 12;
                w32(vo, virtual_block_size);
                w32(vo + 4, segs[i].offset + segs[i].size);
                w16(vo + 8, 0x10);
                w16(vo + 10, 9);
                ++next_block;
            }
        }
        file.insert(file.end(), data.begin(), data.end());
        return file;
    }

    static u32 Align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
};

}  // namespace sspc::test
