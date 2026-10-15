#include "resources/anim_clip.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace sspc::res {
namespace {

constexpr u32 kClipMagic = 0x45413037;  // '70AE'
constexpr u32 kHeaderSize = 0x60;
constexpr float kQuat15Scale = 4.315969e-05f;  // sqrt(2) / 32767
constexpr float kQuat15Bias = -0.70710677f;
constexpr u32 kMaxKeys = 1u << 16;

u16 Rd16(std::span<const u8> d, u64 o) {
    u16 v;
    std::memcpy(&v, d.data() + o, 2);
    return v;
}
u32 Rd32(std::span<const u8> d, u64 o) {
    u32 v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

// Missing component of a unit quaternion; the others fill x..w in order, skipping `dropped`.
Quat FillQuat(const float v[3], u32 dropped) {
    const float s = 1.0f - v[0] * v[0] - v[1] * v[1] - v[2] * v[2];
    const float w = std::sqrt(std::clamp(s, 1.17549435e-38f, 1.0f));
    float q[4];
    for (u32 c = 0, i = 0; c < 4; ++c) q[c] = c == dropped ? w : v[i++];
    return {q[0], q[1], q[2], q[3]};
}

}  // namespace

TrackFormat TrackFormat::FromWord(u32 word) {
    TrackFormat f;
    f.word = word;
    f.dropped = static_cast<u8>(word & 3);
    for (u32 c = 0; c < 3; ++c) {
        const u32 base = 2 + 10 * c;
        f.mantissa[c] = static_cast<u8>((word >> base) & 31);
        f.exponent[c] = static_cast<u8>((word >> (base + 5)) & 15);
        f.sign[c] = static_cast<u8>((word >> (base + 9)) & 1);
        f.bits += f.mantissa[c] + f.exponent[c] + f.sign[c];
    }
    return f;
}

float DecodeComponent(u32 sign, u32 exponent, u32 mantissa, u32 sign_bits, u32 exponent_bits, u32 mantissa_bits) {
    if (exponent_bits == 0) {
        if (mantissa_bits == 0) return 0.0f;
        const float scale = 1.0f / static_cast<float>((1u << mantissa_bits) - 1);
        const s64 v = (sign_bits && sign) ? static_cast<s64>(mantissa) - (s64{1} << mantissa_bits) : mantissa;
        return static_cast<float>(v) * scale;
    }
    const u32 biased = exponent + 128u - (1u << (exponent_bits - 1));
    u32 m = mantissa;
    if (mantissa_bits > 23) m >>= (mantissa_bits - 23);
    const u32 mbits = std::min<u32>(mantissa_bits, 23);
    const u32 word = ((sign_bits ? sign : 0) << 31) | ((biased & 0xff) << 23) | ((m << (23 - mbits)) & 0x7fffff);
    return std::bit_cast<float>(word);
}

u32 AnimClip::ReadBits(u32 byte_base, u64 bit, u32 count) const {
    if (count == 0) return 0;
    const u64 byte = byte_base + (bit >> 3);
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | blob_[static_cast<size_t>(byte) + i];
    return static_cast<u32>((v << (bit & 7)) >> (64 - count));
}

Quat AnimClip::DecodeQuat48(u32 o) const {
    const u32 a = (u32{blob_[o]} << 8) | blob_[o + 1];
    const u32 w = (u32{blob_[o + 2]} << 24) | (u32{blob_[o + 3]} << 16) | (u32{blob_[o + 4]} << 8) | blob_[o + 5];
    const float v[3] = {static_cast<float>(a) * kQuat15Scale + kQuat15Bias,
                        static_cast<float>(w >> 17) * kQuat15Scale + kQuat15Bias,
                        static_cast<float>((w >> 2) & 0x7fff) * kQuat15Scale + kQuat15Bias};
    return FillQuat(v, w & 3);
}

void AnimClip::DecodeVariable(const TrackFormat& f, u32 byte_base, u64 bit, float out[3]) const {
    for (u32 i = 0; i < 3; ++i) {
        const u32 c = 2 - i;  // stored order: component 2, 1, 0
        const u32 s = ReadBits(byte_base, bit, f.sign[c]);
        bit += f.sign[c];
        const u32 e = ReadBits(byte_base, bit, f.exponent[c]);
        bit += f.exponent[c];
        const u32 m = ReadBits(byte_base, bit, f.mantissa[c]);
        bit += f.mantissa[c];
        out[i] = DecodeComponent(s, e, m, f.sign[c], f.exponent[c], f.mantissa[c]);
    }
}

Quat AnimClip::VariableRotation(const TrackFormat& f, u32 byte_base, u64 bit) const {
    float v[3];
    DecodeVariable(f, byte_base, bit, v);
    return FillQuat(v, f.dropped);
}

Quat AnimClip::RotationAt(u32 s, u32 k, u32 i) const {
    const Segment& seg = segments_[s];
    const SegTrack& st = seg_tracks_[seg.tracks + k];
    const Track& t = anim_[0][k];
    if (i > st.key_count) return RotationAt(s + 1, k, 0);
    if (variable_[0]) {
        if (i == 0) return VariableRotation(t.format, seg.info[0], t.info_bit);
        return VariableRotation(t.format, seg.keys[0], st.key_bit + static_cast<u64>(i - 1) * t.format.bits);
    }
    if (i == 0) return DecodeQuat48(seg.info[0] + 6 * k);
    return DecodeQuat48(seg.keys[0] + static_cast<u32>(st.key_bit) + 6 * (i - 1));
}

Vec3 AnimClip::VectorAt(u32 kind, u32 s, u32 k, u32 i) const {
    const Segment& seg = segments_[s];
    u32 base = seg.tracks;
    for (u32 q = 0; q < kind; ++q) base += static_cast<u32>(anim_[q].size());
    const SegTrack& st = seg_tracks_[base + k];
    const Track& t = anim_[kind][k];
    if (i > st.key_count) return VectorAt(kind, s + 1, k, 0);
    float v[3];
    if (i == 0) DecodeVariable(t.format, seg.info[kind], t.info_bit, v);
    else DecodeVariable(t.format, seg.keys[kind], st.key_bit + static_cast<u64>(i - 1) * t.format.bits, v);
    return {v[0], v[1], v[2]};
}

void AnimClip::Sample(float phase, std::span<JointPose> pose) const {
    for (const auto& [j, q] : const_rot_) {
        if (j < pose.size()) pose[j].rotation = q;
    }
    for (const auto& [j, t] : const_trans_) {
        if (j < pose.size()) pose[j].translation = t;
    }
    for (const auto& [j, sc] : const_scale_) {
        if (j < pose.size()) pose[j].scale = sc;
    }
    if (segments_.empty()) return;
    // Sampler 0x71d50: frame = (keys - 1) * phase; the segment is the last one starting at or before it.
    const float frame = static_cast<float>(key_count_ - 1) * std::clamp(phase, 0.0f, 1.0f);
    const u32 fi = std::min(static_cast<u32>(frame), key_count_ - 1);
    u32 s = 0;
    {
        u32 lo = 0, hi = static_cast<u32>(segments_.size());
        while (hi - lo > 1) {
            const u32 mid = (lo + hi) / 2;
            if (segments_[mid].start <= fi) lo = mid;
            else hi = mid;
        }
        s = lo;
    }
    const Segment& seg = segments_[s];
    u32 local = fi - seg.start;
    float frac = frame - static_cast<float>(fi);
    if (local > seg.len) {
        local = seg.len;
        frac = 1.0f;
    }
    const bool has_next = s + 1 < segments_.size();
    u32 kind_base = seg.tracks;
    for (u32 kind = 0; kind < 3; kind_base += static_cast<u32>(anim_[kind].size()), ++kind) {
        const u32 n = static_cast<u32>(anim_[kind].size());
        for (u32 k = 0; k < n; ++k) {
            const SegTrack& st = seg_tracks_[kind_base + k];
            const u8* f = frames_.data() + st.first_frame;
            // Previous value: last key at or before `local` (0 = the segment's info).
            u32 prev = 0;
            while (prev < st.key_count && f[prev] <= local) ++prev;
            const u32 prev_frame = prev ? f[prev - 1] : 0;
            u32 next_frame = 0;
            bool has_next_value = true;
            if (prev < st.key_count) next_frame = f[prev];
            else if (has_next) next_frame = seg.len + 1u;
            else has_next_value = false;
            const u16 joint = anim_[kind][k].joint;
            if (joint >= pose.size()) continue;
            Vec3& vec = kind == 1 ? pose[joint].translation : pose[joint].scale;
            if (!has_next_value || next_frame == prev_frame) {
                if (kind == 0) pose[joint].rotation = RotationAt(s, k, prev);
                else vec = VectorAt(kind, s, k, prev);
                continue;
            }
            const float w = (static_cast<float>(local) + frac - static_cast<float>(prev_frame)) /
                            static_cast<float>(next_frame - prev_frame);
            if (kind == 0) {
                pose[joint].rotation = Slerp(RotationAt(s, k, prev), RotationAt(s, k, prev + 1), w);
            } else {
                vec = Lerp(VectorAt(kind, s, k, prev), VectorAt(kind, s, k, prev + 1), w);
            }
        }
    }
}

std::optional<AnimClip> AnimClip::Parse(std::span<const u8> data, u32 offset, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<AnimClip> {
        if (error) *error = why;
        return std::nullopt;
    };
    const u64 size = data.size();
    if (offset > size || size - offset < kHeaderSize) return fail("clip header out of range");
    std::span<const u8> d = data.subspan(offset);
    if (Rd32(d, 0) != kClipMagic) return fail("not a '70AE' clip");
    AnimClip c;
    c.skeleton_joints_ = Rd16(d, 0x0e);
    c.key_count_ = Rd16(d, 0x10);
    const u32 seg_count = Rd16(d, 0x12);
    u32 counts[8];
    for (u32 i = 0; i < 8; ++i) counts[i] = Rd16(d, 0x16 + 2 * i);
    c.flags_ = Rd32(d, 0x2c);
    if (c.key_count_ == 0 || c.key_count_ > kMaxKeys || seg_count == 0 || seg_count > c.key_count_) {
        return fail(std::format("bad key/segment count {}/{}", c.key_count_, seg_count));
    }
    for (u32 kind = 0; kind < 3; ++kind) c.variable_[kind] = (c.flags_ & (1u << kind)) != 0;
    if (counts[5] && !c.variable_[1]) return fail("fixed-format animated translations are not reconstructed");
    if (counts[6] && !c.variable_[2]) return fail("fixed-format animated scales are not reconstructed");
    u64 end = kHeaderSize;  // furthest byte the decoder reads
    auto rel = [&](u32 field) -> u64 {
        const u32 v = Rd32(d, field);
        return v ? field + static_cast<u64>(v) : 0;
    };
    auto need = [&](u64 at, u64 bytes) {
        end = std::max(end, at + bytes);
        return at + bytes <= d.size();
    };

    // Joint lists at +0x60: constant rot (padded to 8), constant trans/scale/float, animated
    // rot/trans/scale/float (padded to 4).
    std::vector<u16> lists[8];
    u64 p = kHeaderSize;
    for (u32 i = 0; i < 8; ++i) {
        const u32 pad = i == 0 ? 8 : 4;
        if (!need(p, counts[i] * 2ull)) return fail("joint list out of range");
        lists[i].resize(counts[i]);
        for (u32 j = 0; j < counts[i]; ++j) {
            lists[i][j] = Rd16(d, p + 2 * j);
            if (lists[i][j] >= c.skeleton_joints_) return fail("joint index beyond the clip's skeleton");
        }
        p += ((counts[i] + pad - 1) / pad * pad) * 2ull;
    }

    // Format words (+0x50): per variable kind {constant word, one word per animated track}.
    std::vector<TrackFormat> formats[3];
    if (c.flags_ & 0xf) {
        u64 fp = rel(0x50);
        if (!fp) return fail("missing format table");
        for (u32 kind = 0; kind < 4; ++kind) {
            if (!(c.flags_ & (1u << kind))) continue;
            const u32 n = counts[4 + kind] + 1;
            if (!need(fp, n * 4ull)) return fail("format table out of range");
            if (kind < 3) {
                for (u32 i = 0; i < n; ++i) formats[kind].push_back(TrackFormat::FromWord(Rd32(d, fp + 4ull * i)));
            }
            fp += n * 4ull;
        }
    }

    // Constant tracks (+0x40 rotations, +0x44 translations, +0x48 scales): one shared format word
    // in the variable-width formats, otherwise 48-bit quaternions / three floats (0x88c250).
    for (u32 kind = 0; kind < 3; ++kind) {
        if (!counts[kind]) continue;
        const u64 cp = rel(0x40 + 4 * kind);
        if (!cp) return fail("missing constant track data");
        const u64 bytes = c.variable_[kind] ? (static_cast<u64>(formats[kind][0].bits) * counts[kind] + 7) / 8
                                            : counts[kind] * (kind == 0 ? 6ull : 12ull);
        if (!need(cp, bytes)) return fail("constant track data out of range");
    }

    // Segment tables (+0x38 offsets {u32, u32 clip offset}, +0x3c frames {u16 start, u16 len}).
    const u64 seg_offsets = rel(0x38), seg_frames = rel(0x3c);
    if (!seg_offsets || !seg_frames || !need(seg_offsets, seg_count * 8ull) || !need(seg_frames, seg_count * 4ull)) {
        return fail("segment tables out of range");
    }
    for (u32 kind = 0; kind < 3; ++kind) {
        c.anim_[kind].resize(counts[4 + kind]);
        u32 bit = 0;
        for (u32 k = 0; k < counts[4 + kind]; ++k) {
            Track& t = c.anim_[kind][k];
            t.joint = lists[4 + kind][k];
            if (c.variable_[kind]) {
                t.format = formats[kind][k + 1];
                t.info_bit = bit;
                bit += t.format.bits;
            }
        }
    }
    const u32 tracks_total = counts[4] + counts[5] + counts[6] + counts[7];
    c.segments_.resize(seg_count);
    for (u32 s = 0; s < seg_count; ++s) {
        Segment& seg = c.segments_[s];
        seg.start = Rd16(d, seg_frames + 4ull * s);
        seg.len = Rd16(d, seg_frames + 4ull * s + 2);
        if (seg.start >= c.key_count_ || (s > 0 && seg.start <= c.segments_[s - 1].start)) return fail("bad segment frames");
        const u64 so = Rd32(d, seg_offsets + 8ull * s + 4);
        if (!need(so, 0x20)) return fail("segment header out of range");
        // Segment header: section sizes {info rot, trans, scale, float, keys rot, trans, scale, float}.
        u32 sz[8];
        for (u32 i = 0; i < 8; ++i) sz[i] = Rd32(d, so + 4ull * i);
        const u64 info0 = so + 0x20, info1 = info0 + sz[0], info2 = info1 + sz[1];
        const u64 bitset = info2 + sz[2] + sz[3];
        const u64 bitset_bytes = (static_cast<u64>(tracks_total) * seg.len + 7) / 8;
        const u64 keys0 = bitset + bitset_bytes, keys1 = keys0 + sz[4], keys2 = keys1 + sz[5];
        const u64 keys3 = (keys2 + sz[6] + 3) & ~u64{3};
        if (!need(keys3, sz[7]) || keys2 > 0xffffffffull || offset + keys3 > size) return fail("segment data out of range");
        seg.info[0] = static_cast<u32>(info0);
        seg.info[1] = static_cast<u32>(info1);
        seg.info[2] = static_cast<u32>(info2);
        seg.keys[0] = static_cast<u32>(keys0);
        seg.keys[1] = static_cast<u32>(keys1);
        seg.keys[2] = static_cast<u32>(keys2);
        seg.tracks = static_cast<u32>(c.seg_tracks_.size());
        // Key frames of every track (bit set rows of `len` bits, MSB first; rows ordered by kind).
        u64 row0 = 0;
        for (u32 kind = 0; kind < 3; row0 += static_cast<u64>(counts[4 + kind]) * seg.len, ++kind) {
            u64 key_pos = 0;  // bits (variable) or bytes (48-bit rotations)
            for (u32 k = 0; k < counts[4 + kind]; ++k) {
                SegTrack st;
                st.first_frame = static_cast<u32>(c.frames_.size());
                st.key_bit = key_pos;
                const u64 row = row0 + static_cast<u64>(k) * seg.len;
                for (u32 j = 0; j < seg.len; ++j) {
                    const u64 b = row + j;
                    if ((d[bitset + (b >> 3)] >> (7 - (b & 7))) & 1) {
                        c.frames_.push_back(static_cast<u8>(std::min<u32>(j + 1, 255)));
                        ++st.key_count;
                    }
                }
                if (seg.len > 254 && st.key_count) return fail("segment longer than 254 frames");
                key_pos += st.key_count * (c.variable_[kind] ? c.anim_[kind][k].format.bits : 6ull);
                c.seg_tracks_.push_back(st);
            }
            // Info and key sections must hold every value this kind's tracks address.
            const u64 info_bytes = sz[kind];
            const u64 key_bytes = sz[4 + kind];
            if (c.variable_[kind]) {
                u64 info_bits = 0;
                for (const Track& t : c.anim_[kind]) info_bits += t.format.bits;
                if (info_bits > info_bytes * 8 || key_pos > key_bytes * 8) return fail("track data exceeds its section");
            } else if (counts[4 + kind] * 6ull > info_bytes || key_pos > key_bytes) {
                return fail("track data exceeds its section");
            }
        }
    }
    const u64 total = end;
    if (offset + total > size) return fail("clip extends past the data");
    c.blob_.assign(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(total));
    c.blob_.resize(c.blob_.size() + kPad, 0);

    // Constant tracks decoded once.
    if (counts[0]) {
        const u32 cp = static_cast<u32>(rel(0x40));
        for (u32 i = 0; i < counts[0]; ++i) {
            const Quat q = c.variable_[0] ? c.VariableRotation(formats[0][0], cp, static_cast<u64>(i) * formats[0][0].bits)
                                          : c.DecodeQuat48(cp + 6 * i);
            c.const_rot_.emplace_back(lists[0][i], q);
        }
    }
    for (u32 kind = 1; kind < 3; ++kind) {
        const u32 cp = counts[kind] ? static_cast<u32>(rel(0x40 + 4 * kind)) : 0;
        auto& out = kind == 1 ? c.const_trans_ : c.const_scale_;
        for (u32 i = 0; i < counts[kind]; ++i) {
            float v[3];
            if (c.variable_[kind]) c.DecodeVariable(formats[kind][0], cp, static_cast<u64>(i) * formats[kind][0].bits, v);
            else std::memcpy(v, c.blob_.data() + cp + 12ull * i, 12);
            out.emplace_back(lists[kind][i], Vec3{v[0], v[1], v[2]});
        }
    }
    return c;
}

std::vector<u32> FindClipBlobs(std::span<const u8> data) {
    std::vector<u32> out;
    for (u64 o = 0; o + kHeaderSize <= data.size(); o += 16) {
        if (Rd32(data, o) == kClipMagic) out.push_back(static_cast<u32>(o));
    }
    return out;
}

}  // namespace sspc::res
