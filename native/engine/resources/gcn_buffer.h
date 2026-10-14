// SecondSonPC native runtime - GCN buffer resource descriptors (V#) and vertex element decoding.
//
// The game's packages store vertex streams as raw GPU buffers described by 128-bit V#
// descriptors. This file decodes the descriptor fields and converts elements to float, following
// the public AMD GCN3 ISA documentation (buffer data/number formats). It runs on the CPU at load
// time only; nothing here is a GPU command or shader translation.
#pragma once

#include <array>
#include <span>

#include "core/types.h"

namespace sspc::res {

enum class DataFormat : u8 {
    Invalid = 0,
    D8 = 1,
    D16 = 2,
    D8_8 = 3,
    D32 = 4,
    D16_16 = 5,
    D10_11_11 = 6,
    D11_11_10 = 7,
    D10_10_10_2 = 8,
    D2_10_10_10 = 9,
    D8_8_8_8 = 10,
    D32_32 = 11,
    D16_16_16_16 = 12,
    D32_32_32 = 13,
    D32_32_32_32 = 14,
};

enum class NumFormat : u8 {
    Unorm = 0,
    Snorm = 1,
    Uscaled = 2,
    Sscaled = 3,
    Uint = 4,
    Sint = 5,
    SnormNz = 6,  // "SNORM_OGL": (2v + 1) / (2^n - 1)
    Float = 7,
};

struct VSharp {
    u64 base = 0;  // in a package: the data-relative offset written by the KNLI kind-2 link
    u32 stride = 0;
    u32 num_records = 0;
    std::array<u8, 4> dst_sel{};  // 0 = 0, 1 = 1, 4..7 = X..W
    NumFormat nfmt = NumFormat::Unorm;
    DataFormat dfmt = DataFormat::Invalid;
};

VSharp DecodeVSharp(const u32 dw[4]);
const char* ToString(DataFormat f);
const char* ToString(NumFormat f);

u32 ElementSize(DataFormat f);     // bytes per element, 0 for Invalid/unknown
u32 ComponentCount(DataFormat f);  // 0 for Invalid/unknown

// Converts one element at `bytes` (at least ElementSize bytes) to up to 4 floats (missing
// components are 0, alpha 1). Returns false for unsupported format combinations.
bool DecodeElement(const u8* bytes, DataFormat dfmt, NumFormat nfmt, float out[4]);
void ApplyDstSel(const float in[4], const std::array<u8, 4>& sel, float out[4]);

float HalfToFloat(u16 h);

}  // namespace sspc::res
