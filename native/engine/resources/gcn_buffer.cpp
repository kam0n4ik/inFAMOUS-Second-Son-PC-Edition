#include "resources/gcn_buffer.h"

#include <algorithm>
#include <cstring>

namespace sspc::res {
namespace {

template <class T>
T Load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

float FromBits(u32 v, u32 bits, NumFormat nfmt, bool& ok) {
    const u32 max_u = bits >= 32 ? 0xffffffffu : (1u << bits) - 1;
    const s32 sign_shift = 32 - static_cast<s32>(bits);
    const s32 sv = bits >= 32 ? static_cast<s32>(v) : (static_cast<s32>(v << sign_shift) >> sign_shift);
    switch (nfmt) {
        case NumFormat::Unorm: return static_cast<float>(v) / static_cast<float>(max_u);
        case NumFormat::Snorm: {
            const float max_s = static_cast<float>((1u << (bits - 1)) - 1);
            return std::max(static_cast<float>(sv) / max_s, -1.0f);
        }
        case NumFormat::SnormNz:
            return (2.0f * static_cast<float>(sv) + 1.0f) / static_cast<float>(max_u);
        case NumFormat::Uscaled:
        case NumFormat::Uint: return static_cast<float>(v);
        case NumFormat::Sscaled:
        case NumFormat::Sint: return static_cast<float>(sv);
        case NumFormat::Float:
            if (bits == 16) return HalfToFloat(static_cast<u16>(v));
            if (bits == 32) {
                float f;
                std::memcpy(&f, &v, 4);
                return f;
            }
            ok = false;
            return 0;
    }
    ok = false;
    return 0;
}

}  // namespace

float HalfToFloat(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000) << 16;
    u32 exp = (h >> 10) & 0x1f;
    u32 mant = h & 0x3ff;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // subnormal
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

VSharp DecodeVSharp(const u32 dw[4]) {
    VSharp v;
    v.base = dw[0] | (static_cast<u64>(dw[1] & 0xfff) << 32);
    v.stride = (dw[1] >> 16) & 0x3fff;
    v.num_records = dw[2];
    v.dst_sel = {static_cast<u8>(dw[3] & 7), static_cast<u8>((dw[3] >> 3) & 7),
                 static_cast<u8>((dw[3] >> 6) & 7), static_cast<u8>((dw[3] >> 9) & 7)};
    v.nfmt = static_cast<NumFormat>((dw[3] >> 12) & 7);
    v.dfmt = static_cast<DataFormat>((dw[3] >> 15) & 0xf);
    return v;
}

const char* ToString(DataFormat f) {
    static const char* const kNames[] = {"invalid", "8", "16", "8_8", "32", "16_16", "10_11_11",
                                         "11_11_10", "10_10_10_2", "2_10_10_10", "8_8_8_8", "32_32",
                                         "16_16_16_16", "32_32_32", "32_32_32_32"};
    const auto i = static_cast<size_t>(f);
    return i < std::size(kNames) ? kNames[i] : "reserved";
}

const char* ToString(NumFormat f) {
    static const char* const kNames[] = {"unorm", "snorm", "uscaled", "sscaled",
                                         "uint", "sint", "snorm_nz", "float"};
    return kNames[static_cast<size_t>(f) & 7];
}

u32 ElementSize(DataFormat f) {
    switch (f) {
        case DataFormat::D8: return 1;
        case DataFormat::D16:
        case DataFormat::D8_8: return 2;
        case DataFormat::D32:
        case DataFormat::D16_16:
        case DataFormat::D10_11_11:
        case DataFormat::D11_11_10:
        case DataFormat::D10_10_10_2:
        case DataFormat::D2_10_10_10:
        case DataFormat::D8_8_8_8: return 4;
        case DataFormat::D32_32:
        case DataFormat::D16_16_16_16: return 8;
        case DataFormat::D32_32_32: return 12;
        case DataFormat::D32_32_32_32: return 16;
        default: return 0;
    }
}

u32 ComponentCount(DataFormat f) {
    switch (f) {
        case DataFormat::D8:
        case DataFormat::D16:
        case DataFormat::D32: return 1;
        case DataFormat::D8_8:
        case DataFormat::D16_16:
        case DataFormat::D32_32: return 2;
        case DataFormat::D10_11_11:
        case DataFormat::D11_11_10:
        case DataFormat::D32_32_32: return 3;
        case DataFormat::D10_10_10_2:
        case DataFormat::D2_10_10_10:
        case DataFormat::D8_8_8_8:
        case DataFormat::D16_16_16_16:
        case DataFormat::D32_32_32_32: return 4;
        default: return 0;
    }
}

bool DecodeElement(const u8* p, DataFormat dfmt, NumFormat nfmt, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    bool ok = true;
    switch (dfmt) {
        case DataFormat::D8:
        case DataFormat::D8_8:
        case DataFormat::D8_8_8_8:
            if (nfmt == NumFormat::Float) return false;
            for (u32 i = 0; i < ComponentCount(dfmt); ++i) out[i] = FromBits(p[i], 8, nfmt, ok);
            return ok;
        case DataFormat::D16:
        case DataFormat::D16_16:
        case DataFormat::D16_16_16_16:
            for (u32 i = 0; i < ComponentCount(dfmt); ++i) out[i] = FromBits(Load<u16>(p + 2 * i), 16, nfmt, ok);
            return ok;
        case DataFormat::D32:
        case DataFormat::D32_32:
        case DataFormat::D32_32_32:
        case DataFormat::D32_32_32_32:
            for (u32 i = 0; i < ComponentCount(dfmt); ++i) out[i] = FromBits(Load<u32>(p + 4 * i), 32, nfmt, ok);
            return ok;
        case DataFormat::D2_10_10_10: {
            // Component X in the least significant bits (A2B10G10R10 order).
            if (nfmt == NumFormat::Float) return false;
            const u32 v = Load<u32>(p);
            out[0] = FromBits(v & 0x3ff, 10, nfmt, ok);
            out[1] = FromBits((v >> 10) & 0x3ff, 10, nfmt, ok);
            out[2] = FromBits((v >> 20) & 0x3ff, 10, nfmt, ok);
            out[3] = FromBits(v >> 30, 2, nfmt, ok);
            return ok;
        }
        case DataFormat::D10_10_10_2: {
            if (nfmt == NumFormat::Float) return false;
            const u32 v = Load<u32>(p);
            out[0] = FromBits(v & 0x3, 2, nfmt, ok);
            out[1] = FromBits((v >> 2) & 0x3ff, 10, nfmt, ok);
            out[2] = FromBits((v >> 12) & 0x3ff, 10, nfmt, ok);
            out[3] = FromBits(v >> 22, 10, nfmt, ok);
            return ok;
        }
        default: return false;
    }
}

void ApplyDstSel(const float in[4], const std::array<u8, 4>& sel, float out[4]) {
    for (int i = 0; i < 4; ++i) {
        const u8 s = sel[i];
        out[i] = s == 0 ? 0.0f : s == 1 ? 1.0f : (s >= 4 ? in[s - 4] : 0.0f);
    }
}

}  // namespace sspc::res
