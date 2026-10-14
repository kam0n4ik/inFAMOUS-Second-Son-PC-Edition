// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) for file integrity checks.
#pragma once

#include <array>
#include <span>

#include "core/types.h"

namespace sspc {

constexpr std::array<u32, 256> MakeCrc32Table() {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}

inline u32 Crc32(std::span<const u8> data, u32 crc = 0) {
    static constexpr auto kTable = MakeCrc32Table();
    crc = ~crc;
    for (u8 b : data) crc = kTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}  // namespace sspc
