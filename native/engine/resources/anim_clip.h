// SecondSonPC native runtime - animation clips of inFAMOUS Second Son ('70AE' blobs).
//
// Reconstructed from the game's clip sampler (eboot 0x71d50), clip decoder (0x887e60) and track
// decoders (0x88a070/0x88a520 48-bit rotations, 0x88c250 constant translations, 0x88f200/0x88fd70/
// 0x891fe0/0x892a80 variable-width formats); evidence in docs/native/RESOURCE_FORMATS.md
// "Animation clips". Clips live in type-0 blocks of character/shared packages (core_common:
// 4444 clips) and address the joints of a skeleton by index (Delsin: the first 413 of 653 joints).
//
// A clip is split into segments of up to a few dozen frames. Every animated track stores its value
// at the segment's first frame ("info"), keys at the frames flagged in the segment's bit set, and
// takes the value after the last frame from the next segment's info. Values between keys are
// interpolated (slerp for rotations, lerp for translations) exactly like the game's sampler.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "core/types.h"

namespace sspc::res {

// Local transform of a joint relative to its parent, in the game's units (centimetres). Scale is
// 1 for every deforming joint of the analysed rigs (Delsin: only the leaf helper joint 5 has one).
struct JointPose {
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1, 1, 1};
};

// 32-bit format word of a variable-width track: three stored components, each {mantissa bits:5,
// exponent bits:4, sign bit:1} starting at bit 2 + 10*c, and the dropped quaternion component in
// bits 0..1. Components are stored in the order 2, 1, 0.
struct TrackFormat {
    u32 word = 0;
    u8 mantissa[3] = {};
    u8 exponent[3] = {};
    u8 sign[3] = {};
    u8 dropped = 0;
    u32 bits = 0;  // bits of one stored value
    static TrackFormat FromWord(u32 word);
};

// One stored component: sign, exponent and mantissa fields MSB first. Exponent bits 0 = signed
// fixed point scaled by 1/(2^m - 1); otherwise a small float with exponent bias 128 - 2^(e-1).
float DecodeComponent(u32 sign, u32 exponent, u32 mantissa, u32 sign_bits, u32 exponent_bits, u32 mantissa_bits);

class AnimClip {
public:
    // Parses and validates the clip blob at `offset` of `data` and copies it (the clip does not keep
    // a view of `data`). Returns nullopt with `error` set when anything is out of range or uses a
    // track format that is not reconstructed.
    static std::optional<AnimClip> Parse(std::span<const u8> data, u32 offset, std::string* error = nullptr);

    u16 SkeletonJoints() const { return skeleton_joints_; }  // header +0x0E
    u32 KeyCount() const { return key_count_; }               // frames incl. the last (+0x10)
    u32 SegmentCount() const { return static_cast<u32>(segments_.size()); }
    float FrameRate() const { return 30.0f; }
    float DurationSeconds() const { return key_count_ > 1 ? (key_count_ - 1) / FrameRate() : 0.0f; }
    u32 SizeBytes() const { return static_cast<u32>(blob_.size()) - kPad; }
    u32 RotationTracks() const { return static_cast<u32>(anim_[0].size()); }
    u32 TranslationTracks() const { return static_cast<u32>(anim_[1].size()); }
    u32 ScaleTracks() const { return static_cast<u32>(anim_[2].size()); }
    u32 ConstantRotations() const { return static_cast<u32>(const_rot_.size()); }
    u32 ConstantTranslations() const { return static_cast<u32>(const_trans_.size()); }
    u32 Flags() const { return flags_; }
    const std::vector<std::pair<u16, Vec3>>& ConstantTranslationValues() const { return const_trans_; }

    // Writes the clip's rotations and translations at `phase` (0 = first frame, 1 = last frame) into
    // `pose`, indexed by joint; joints the clip has no track for keep their values.
    void Sample(float phase, std::span<JointPose> pose) const;

private:
    static constexpr u32 kPad = 16;  // zero bytes after the blob so bit reads never leave the buffer
    struct Track {
        u16 joint = 0;
        TrackFormat format;  // variable-width tracks only
        u32 info_bit = 0;    // position of the track's value inside a segment's info section
    };
    struct SegTrack {
        u32 first_frame = 0;  // index into frames_
        u32 key_count = 0;
        u64 key_bit = 0;      // position of the first key inside the segment's key section
    };
    struct Segment {
        u16 start = 0, len = 0;
        u32 info[3] = {};  // blob offsets of the rotation/translation/scale info sections
        u32 keys[3] = {};  // blob offsets of the rotation/translation/scale key sections
        u32 tracks = 0;    // index of this segment's first SegTrack (rotations, translations, scales)
    };

    u32 ReadBits(u32 byte_base, u64 bit, u32 count) const;
    Quat DecodeQuat48(u32 byte_offset) const;
    void DecodeVariable(const TrackFormat& f, u32 byte_base, u64 bit, float out[3]) const;
    Quat VariableRotation(const TrackFormat& f, u32 byte_base, u64 bit) const;
    // Value index i of track k in segment s: 0 = the segment's info, 1..n = keys, n + 1 = the next
    // segment's info. Vectors: kind 1 translation, kind 2 scale.
    Quat RotationAt(u32 s, u32 k, u32 i) const;
    Vec3 VectorAt(u32 kind, u32 s, u32 k, u32 i) const;

    std::vector<u8> blob_;
    u16 skeleton_joints_ = 0;
    u32 key_count_ = 0;
    u32 flags_ = 0;
    bool variable_[3] = {};  // variable-width rotations / translations / scales
    std::vector<std::pair<u16, Quat>> const_rot_;
    std::vector<std::pair<u16, Vec3>> const_trans_;
    std::vector<std::pair<u16, Vec3>> const_scale_;
    std::vector<Track> anim_[3];  // animated rotation, translation and scale tracks
    std::vector<Segment> segments_;
    std::vector<SegTrack> seg_tracks_;
    std::vector<u8> frames_;  // local key frames (1..len) of every SegTrack, concatenated
};

// Offsets of every 16-byte aligned '70AE' clip blob in `data`.
std::vector<u32> FindClipBlobs(std::span<const u8> data);

}  // namespace sspc::res
