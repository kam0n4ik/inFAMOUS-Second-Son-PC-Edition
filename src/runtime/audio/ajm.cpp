// SecondSonPC — libSceAjm, the console's audio decoder service (R10, docs/R10_AJM.md), group "ajm". Selected by
// SSPC_R10_AJM (default "bridge": the oracle, third_party/shadPS4/src/core/libraries/ajm/*, serves the game as before):
//   native  SecondSonPC's own AJM below serves the 11 functions the game imports; shadPS4 is not called;
//   verify  the own AJM serves the game AND every batch is decoded once more by the oracle on the same input, into
//           shadow copies of the output and sideband buffers; PCM and sideband are compared byte for byte (counts in
//           the log). SSPC_R10_AJM_SELFTEST=<file.at9> also decodes that RIFF ATRAC9 file through both, with the
//           game's own call sequence, when the game initializes AJM; SSPC_R10_AJM_DUMP=<dir> writes both PCM streams.
// The game (Wwise's ATRAC9 source, eboot 0x857d70..0x8736c0, docs/R10_AJM.md §2) registers ATRAC9 only, creates one
// instance per voice (S16, interleaved, channels from the voice's mask), initializes it with a control job (Reset |
// Initialize, the 4 config bytes) and decodes with split-buffer run jobs (MultipleFrames; sideband Result + Stream +
// Format + MFrame, 0x38 bytes) into a ring of 0x1000 samples - all jobs of one audio frame in one batch, started
// and waited for at once (infinite timeout). A batch is therefore processed synchronously in sceAjmBatchStartBuffer:
// sceAjmBatchWait finds it done (the console's AJM runs it on the ACP while the game waits for it the same way).
// The batch buffer format (chunk idents, sizes, the flag masks) and the per-job results are the oracle's, which match
// the job sizes the game reserves (0x40 for a control job, 0x30 + 0x10 per buffer for a run job). LibAtrac9 (MIT,
// third_party/LibAtrac9) is SecondSonPC's own copy, with renamed symbols.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <sspc_atrac9_prefix.h>
#include <src/libatrac9.h>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Audio {

namespace {

// ---- error codes (sceAjm* return values) and job result bits (sideband Result.result); the oracle's tables,
// third_party/shadPS4/src/core/libraries/ajm/ajm_error.h, ajm_result.h
constexpr s32 kOk = 0;
constexpr s32 kErrInvalidContext = static_cast<s32>(0x80930002);
constexpr s32 kErrInvalidInstance = static_cast<s32>(0x80930003);
constexpr s32 kErrInvalidBatch = static_cast<s32>(0x80930004);
constexpr s32 kErrInvalidParameter = static_cast<s32>(0x80930005);
constexpr s32 kErrOutOfMemory = static_cast<s32>(0x80930006);
constexpr s32 kErrOutOfResources = static_cast<s32>(0x80930007);
constexpr s32 kErrCodecNotSupported = static_cast<s32>(0x80930008);
constexpr s32 kErrCodecAlreadyRegistered = static_cast<s32>(0x80930009);
constexpr s32 kErrCodecNotRegistered = static_cast<s32>(0x8093000A);
constexpr s32 kErrWrongRevisionFlag = static_cast<s32>(0x8093000B);
constexpr s32 kErrFlagNotSupported = static_cast<s32>(0x8093000C);
constexpr s32 kErrMalformedBatch = static_cast<s32>(0x80930011);
constexpr s32 kErrInvalidAddress = static_cast<s32>(0x80930016);

constexpr s32 kResNotInitialized = 0x00000001;
constexpr s32 kResInvalidData = 0x00000002;
constexpr s32 kResInvalidParameter = 0x00000004;
constexpr s32 kResPartialInput = 0x00000008;
constexpr s32 kResNotEnoughRoom = 0x00000010;
constexpr s32 kResSidebandTruncated = 0x00000100;
constexpr s32 kResCodecError = 0x40000000;
constexpr s32 kResFatal = static_cast<s32>(0x80000000);

constexpr u32 kCodecAt9 = 1;
constexpr u32 kCodecMax = 23;
constexpr u32 kInstanceStatistics = 0x80000;
constexpr u32 kInstanceIndexMask = 0x3FFF;
constexpr u32 kMaxInstances = 0x2FFF;
constexpr u32 kMaxBatches = 0x400;

// ---- the batch buffer: 4-byte chunk headers (ident 6 bits, payload 20 bits), 8-byte job/flag chunks, 16-byte
// buffer chunks (header, size, address)
enum : u32 {
    kIdentJob = 0,
    kIdentInputRunBuf = 1,
    kIdentInputControlBuf = 2,
    kIdentControlFlags = 3,
    kIdentRunFlags = 4,
    kIdentReturnAddress = 6,
    kIdentInlineBuf = 7,
    kIdentOutputRunBuf = 17,
    kIdentOutputControlBuf = 18,
};
constexpr u32 Header(u32 ident, u32 payload) {
    return (ident & 0x3F) | ((payload & 0xFFFFF) << 6);
}
constexpr u32 IdentOf(u32 header) {
    return header & 0x3F;
}
constexpr u32 PayloadOf(u32 header) {
    return (header >> 6) & 0xFFFFF;
}

// Job flags (u64): version 0-2, codec 3-10, run flags 11-12, control flags 13-15, sideband flags 45-47.
constexpr u64 kRunGetCodecInfo = 1, kRunMultipleFrames = 2;
constexpr u64 kControlReset = 1, kControlInitialize = 2, kControlResample = 4;
constexpr u64 kSidebandGapless = 1, kSidebandFormat = 2, kSidebandStream = 4;
constexpr u64 RunFlags(u64 f) {
    return (f >> 11) & 3;
}
constexpr u64 ControlFlags(u64 f) {
    return (f >> 13) & 7;
}
constexpr u64 SidebandFlags(u64 f) {
    return (f >> 45) & 7;
}
// Statistics flags (the statistics instance's job): memory 0, engine-per-codec 15, engine 16 (of bits 15..31).
constexpr u64 kStatMemory = 1, kStatEnginePerCodec = 1 << 15, kStatEngine = 1 << 16;

// Instance flags (u64): version 0-2, channels 3-6, format 7-9, gapless loop 10, codec flags 32-59.
// AT9 codec flags: parse a RIFF header in the input 0x1, non-interleaved output 0x100.
constexpr u64 kAt9ParseRiff = 1, kAt9NonInterleaved = 0x100;

enum class Encoding : u32 { S16 = 0, S32 = 1, Float = 2 };

struct AjmBuffer {
    u8* address;
    u64 size;
};
struct BatchError {
    s32 error_code;
    const void* job_address;
    u32 command_offset;
    const void* job_ra;
};
static_assert(sizeof(BatchError) == 32);
struct SidebandResult {
    s32 result;
    s32 internal_result;
};
struct SidebandStream {
    s32 input_consumed;
    s32 output_written;
    u64 total_decoded_samples;
};
struct SidebandFormat {
    u32 num_channels;
    u32 channel_mask;
    u32 sample_rate;
    u32 encoding;
    u32 bitrate;
    u32 reserved;
};
struct SidebandGapless {
    u32 total_samples;
    u16 skip_samples;
    u16 skipped_samples;
};
struct SidebandMFrame {
    u32 num_frames;
    u32 reserved;
};
struct At9CodecInfo {
    u32 super_frame_size;
    u32 frames_in_super_frame;
    u32 next_frame_size;
    u32 frame_samples;
};
struct StatEngine {
    float usage_batch;
    float usage_interval[3];
};
struct StatEnginePerCodec {
    u8 codec_count;
    u8 codec_id[3];
    float codec_percentage[3];
};
struct StatMemory {
    u32 instance_free, buffer_free, batch_size, input_size, output_size, small_size;
};
static_assert(sizeof(SidebandStream) == 16 && sizeof(SidebandFormat) == 24 && sizeof(SidebandGapless) == 8);

u32 ChannelMask(u32 channels) {
    switch (channels) {
    case 1:
        return 0x0004;
    case 2:
        return 0x0003;
    case 4:
        return 0x0033;
    case 6:
        return 0x060F;
    case 8:
        return 0x063F;
    default:
        return 0;
    }
}

u32 PcmSize(Encoding e) {
    return e == Encoding::S16 ? 2 : 4;
}

// ---- one job, as parsed from a batch (pointers into the game's memory: nothing is copied)
struct Job {
    u32 instance = 0;
    u64 flags = 0;
    const u8* job_address = nullptr; // the job chunk
    const void* ra = nullptr;
    bool control = false; // a control job (ControlFlags chunk), else a run job (RunFlags)
    std::array<AjmBuffer, 8> inputs{};
    u32 num_inputs = 0;
    std::array<AjmBuffer, 8> outputs{};
    u32 num_outputs = 0;
    AjmBuffer sideband_in{};
    AjmBuffer sideband_out{};
};

// The input of a run job: its buffers one after the other. A frame is handed to the decoder where it lies; only a
// superframe that straddles two buffers is copied (into the instance's staging buffer).
class InputCursor {
public:
    InputCursor(const Job& job) {
        for (u32 i = 0; i < job.num_inputs; ++i) {
            if (job.inputs[i].address != nullptr && job.inputs[i].size != 0) {
                parts_[n_++] = job.inputs[i];
                total_ += job.inputs[i].size;
            }
        }
    }
    u64 Total() const {
        return total_;
    }
    u64 Remaining() const {
        return total_ - consumed_;
    }
    // At least `need` contiguous bytes (fewer when the input ends) and how many lie there.
    const u8* Peek(u64 need, u64* avail, std::vector<u8>& staging) const {
        if (part_ >= n_) {
            *avail = 0;
            return nullptr;
        }
        const u64 here = parts_[part_].size - offset_;
        if (here >= need || here == Remaining()) {
            *avail = here;
            return parts_[part_].address + offset_;
        }
        const u64 n = std::min(need, Remaining());
        staging.resize(std::max<size_t>(staging.size(), n));
        u64 got = 0;
        for (u32 p = part_, off = static_cast<u32>(offset_); got < n && p < n_; ++p, off = 0) {
            const u64 c = std::min(n - got, parts_[p].size - off);
            std::memcpy(staging.data() + got, parts_[p].address + off, c);
            got += c;
        }
        *avail = n;
        return staging.data();
    }
    void Advance(u64 n) {
        n = std::min(n, Remaining());
        consumed_ += n;
        while (n != 0 && part_ < n_) {
            const u64 c = std::min(n, parts_[part_].size - offset_);
            offset_ += c;
            n -= c;
            if (offset_ == parts_[part_].size) {
                ++part_;
                offset_ = 0;
            }
        }
        while (part_ < n_ && offset_ == parts_[part_].size) {
            ++part_;
            offset_ = 0;
        }
    }

private:
    std::array<AjmBuffer, 8> parts_{};
    u32 n_ = 0;
    u32 part_ = 0;
    u64 offset_ = 0;
    u64 total_ = 0;
    u64 consumed_ = 0;
};

// The output of a run job: its buffers one after the other, written in whole samples.
class OutputCursor {
public:
    OutputCursor(const Job& job) {
        for (u32 i = 0; i < job.num_outputs; ++i) {
            if (job.outputs[i].address != nullptr && job.outputs[i].size != 0) {
                parts_[n_++] = job.outputs[i];
                total_ += job.outputs[i].size;
            }
        }
    }
    u64 Total() const {
        return total_;
    }
    u64 Remaining() const {
        return total_ - written_;
    }
    // The current buffer's free space (for a frame decoded straight into it).
    u8* Contiguous(u64* bytes) const {
        if (part_ >= n_) {
            *bytes = 0;
            return nullptr;
        }
        *bytes = parts_[part_].size - offset_;
        return parts_[part_].address + offset_;
    }
    void Advance(u64 n) {
        written_ += n;
        while (n != 0 && part_ < n_) {
            const u64 c = std::min(n, parts_[part_].size - offset_);
            offset_ += c;
            n -= c;
            if (offset_ == parts_[part_].size) {
                ++part_;
                offset_ = 0;
            }
        }
    }
    // Copies up to `count` samples of `sample` bytes; returns the samples written.
    u64 Write(const u8* src, u64 count, u32 sample) {
        u64 done = 0;
        while (done < count && part_ < n_) {
            const u64 room = (parts_[part_].size - offset_) / sample;
            if (room == 0) { // (less than one sample left in this buffer: the oracle splits it, never happens here)
                written_ += parts_[part_].size - offset_;
                ++part_;
                offset_ = 0;
                continue;
            }
            const u64 c = std::min(count - done, room);
            std::memcpy(parts_[part_].address + offset_, src + done * sample, c * sample);
            Advance(c * sample);
            done += c;
        }
        return done;
    }

private:
    std::array<AjmBuffer, 8> parts_{};
    u32 n_ = 0;
    u32 part_ = 0;
    u64 offset_ = 0;
    u64 total_ = 0;
    u64 written_ = 0;
};

struct Gapless {
    SidebandGapless init{};
    SidebandGapless current{};
    bool IsEnd() const {
        return init.total_samples != 0 && current.total_samples == 0;
    }
    void Reset() {
        current.total_samples = init.total_samples;
        current.skip_samples = init.skip_samples;
        current.skipped_samples = 0;
    }
};

struct DecodeResult {
    s32 result = 0;
    s32 internal_result = 0;
    u32 frames = 0;
    u32 samples = 0; // per channel
    bool is_reset = false;
};

// ---- the ATRAC9 decoder of one instance (LibAtrac9)
class At9Decoder {
public:
    At9Decoder(Encoding format, u64 codec_flags) : format_(format), flags_(codec_flags), handle_(Atrac9GetHandle()) {}
    ~At9Decoder() {
        Atrac9ReleaseHandle(handle_);
    }
    At9Decoder(const At9Decoder&) = delete;
    At9Decoder& operator=(const At9Decoder&) = delete;

    // A reset decoder is usable only when its config bytes are valid and LibAtrac9 took them; otherwise every frame
    // answers "not initialized" (LibAtrac9 itself would run on a handle with no frame config).
    void Reset() {
        Atrac9ReleaseHandle(handle_);
        handle_ = Atrac9GetHandle();
        info_ = {};
        initialized_ = false;
        if (handle_ != nullptr && ValidConfig(config_) && Atrac9InitDecoder(handle_, config_) == 0) {
            Atrac9GetCodecInfo(handle_, &info_);
            initialized_ = info_.channels > 0 && info_.frameSamples > 0 && info_.superframeSize > 0;
        }
        num_frames_ = 0;
        superframe_remain_ = initialized_ ? static_cast<u32>(info_.superframeSize) : 0;
    }
    // Returns false (the instance stays uninitialized) when the config bytes are not a valid ATRAC9 config.
    bool Initialize(const u8 config[4]) {
        std::memcpy(config_, config, 4);
        Reset();
        if (!initialized_) {
            SSPC_LOG(Audio, Warning, "AJM: ATRAC9 config {:02x} {:02x} {:02x} {:02x} rejected", config[0], config[1],
                     config[2], config[3]);
            pcm_.clear();
            return false;
        }
        pcm_.assign(static_cast<size_t>(info_.frameSamples) * info_.channels * PcmSize(format_), 0);
        return true;
    }
    u32 MinimumInputSize() const {
        return superframe_remain_;
    }
    u32 NextFrameSize(const Gapless& g) const {
        const u32 frame = static_cast<u32>(info_.frameSamples);
        const u32 skip = std::min<u32>(g.current.skip_samples, frame);
        const u32 samples =
            g.init.total_samples != 0 ? std::min<u32>(g.current.total_samples, frame - skip) : frame - skip;
        return samples * static_cast<u32>(info_.channels) * PcmSize(format_);
    }
    SidebandFormat Format() const {
        const u32 den = static_cast<u32>(info_.framesInSuperframe * info_.frameSamples);
        return SidebandFormat{
            .num_channels = static_cast<u32>(info_.channels),
            .channel_mask = ChannelMask(static_cast<u32>(info_.channels)),
            .sample_rate = static_cast<u32>(info_.samplingRate),
            .encoding = static_cast<u32>(format_),
            .bitrate = den != 0 ? static_cast<u32>((static_cast<u32>(info_.samplingRate) *
                                                     static_cast<u32>(info_.superframeSize) * 8) / den)
                                : 0,
            .reserved = 0,
        };
    }
    At9CodecInfo CodecInfo() const {
        return At9CodecInfo{static_cast<u32>(info_.superframeSize), static_cast<u32>(info_.framesInSuperframe),
                            superframe_remain_, static_cast<u32>(info_.frameSamples)};
    }
    DecodeResult Process(InputCursor& in, OutputCursor& out, Gapless& gapless);

private:
    // The config word (LibAtrac9 ReadConfigData, MSB first): header 0xFE; sample rate index 4 bits, channel config
    // index 3 bits, a validation bit that must be 0. LibAtrac9 indexes its 6-entry ChannelConfigs table with the
    // index unchecked, so 6 and 7 are refused here.
    static bool ValidConfig(const u8 c[4]) {
        return c[0] == 0xFE && ((c[1] >> 1) & 7) < 6 && (c[1] & 1) == 0;
    }
    // (LibAtrac9's bit reader keeps the length in bits in an int: a frame never needs more than this)
    static constexpr u64 kMaxDecodeInput = 0x0FFFFFFF;
    int DecodeTo(const u8* src, u64 size, u8* dst, int* used) {
        const int planar = (flags_ & kAt9NonInterleaved) != 0;
        const int n = static_cast<int>(std::min<u64>(size, kMaxDecodeInput));
        switch (format_) {
        case Encoding::S16:
            return Atrac9Decode(handle_, src, n, reinterpret_cast<short*>(dst), used, planar);
        case Encoding::S32:
            return Atrac9DecodeS32(handle_, src, n, reinterpret_cast<int*>(dst), used, planar);
        case Encoding::Float:
            return Atrac9DecodeF32(handle_, src, n, reinterpret_cast<float*>(dst), used, planar);
        }
        return -1;
    }
    bool ParseRiff(InputCursor& in, Gapless& gapless);

    const Encoding format_;
    const u64 flags_;
    void* handle_;
    u8 config_[4]{};
    bool initialized_ = false;
    u32 superframe_remain_ = 0;
    u32 num_frames_ = 0;
    Atrac9CodecInfo info_{};
    std::vector<u8> pcm_;     // one frame, when it cannot be decoded straight into the output
    std::vector<u8> staging_; // input that straddles two buffers
};

// A RIFF/WAVE ATRAC9 header at the input (the "fmt " chunk's config bytes, the "fact" chunk's length and delay),
// up to the "data" chunk.
bool At9Decoder::ParseRiff(InputCursor& in, Gapless& gapless) {
    static constexpr u8 kGuid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                     0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};
    auto read = [&](void* dst, u64 n) {
        u64 avail = 0;
        const u8* p = in.Peek(n, &avail, staging_);
        if (p == nullptr || avail < n) {
            return false;
        }
        std::memcpy(dst, p, n);
        in.Advance(n);
        return true;
    };
    u32 riff[3];
    if (!read(riff, 12) || riff[0] != 0x46464952 || riff[2] != 0x45564157) { // "RIFF", "WAVE"
        return false;
    }
    for (;;) {
        u32 chunk[2];
        if (!read(chunk, 8)) {
            return false;
        }
        if (chunk[0] == 0x61746164) { // "data"
            return true;
        }
        if (chunk[0] == 0x20746D66 && chunk[1] == 52) { // "fmt " (WAVEFORMATEXTENSIBLE + version + config)
            u8 fmt[52];
            if (!read(fmt, 52)) {
                return false;
            }
            if (fmt[0] != 0xFE || fmt[1] != 0xFF || std::memcmp(fmt + 24, kGuid, 16) != 0) {
                return false;
            }
            if (!Initialize(fmt + 44)) {
                return false;
            }
        } else if (chunk[0] == 0x74636166 && chunk[1] == 12) { // "fact": sample length, encoder delay
            u32 fact[3];
            if (!read(fact, 12)) {
                return false;
            }
            gapless.init.total_samples = fact[0];
            gapless.init.skip_samples = static_cast<u16>(fact[1]);
            gapless.Reset();
        } else {
            if (in.Remaining() < chunk[1]) {
                return false;
            }
            in.Advance(chunk[1]);
        }
    }
}

DecodeResult At9Decoder::Process(InputCursor& in, OutputCursor& out, Gapless& gapless) {
    DecodeResult r{};
    if ((flags_ & kAt9ParseRiff) != 0 && in.Remaining() >= 4) {
        u64 avail = 0;
        u32 magic = 0;
        if (const u8* p = in.Peek(4, &avail, staging_); p != nullptr && avail >= 4) {
            std::memcpy(&magic, p, 4);
        }
        if (magic == 0x46464952) {
            if (!ParseRiff(in, gapless)) {
                r.result = kResInvalidData;
                return r;
            }
            r.is_reset = true;
        }
    }
    if (!initialized_) {
        r.result = kResNotInitialized;
        return r;
    }
    const u32 channels = static_cast<u32>(info_.channels);
    const u32 frame = static_cast<u32>(info_.frameSamples);
    const u32 sample = PcmSize(format_);
    const u64 frame_bytes = static_cast<u64>(frame) * channels * sample;

    // the frame lies within what is left of its superframe
    u64 avail = 0;
    const u8* src = in.Peek(std::max<u32>(superframe_remain_, 1), &avail, staging_);
    if (src == nullptr) {
        r.result = kResPartialInput;
        return r;
    }
    u32 skip = 0;
    if (gapless.current.skip_samples > 0) {
        skip = std::min<u32>(frame, gapless.current.skip_samples);
    }
    const u64 cap = gapless.init.total_samples != 0 ? static_cast<u64>(gapless.current.total_samples) * channels
                                                    : ~0ull;
    // Straight into the output when the whole frame lands there unchanged (no skip, no cut, aligned room).
    u64 room = 0;
    u8* dst = out.Contiguous(&room);
    const bool direct = skip == 0 && cap >= static_cast<u64>(frame) * channels && dst != nullptr &&
                        room >= frame_bytes && (reinterpret_cast<uintptr_t>(dst) % sample) == 0;
    int used = 0;
    const int ret = DecodeTo(src, avail, direct ? dst : pcm_.data(), &used);
    if (ret != 0) {
        SSPC_LOG(Audio, Error, "AJM: ATRAC9 frame decode failed ({:#x})", ret);
        r.result = kResCodecError | kResFatal;
        r.internal_result = ret;
        return r;
    }
    r.frames = 1;
    in.Advance(static_cast<u64>(used));
    superframe_remain_ -= std::min<u32>(superframe_remain_, static_cast<u32>(used));
    if (gapless.current.skip_samples > 0) {
        gapless.current.skip_samples = static_cast<u16>(gapless.current.skip_samples - skip);
    }

    u64 pcm_written = 0;
    if (direct) {
        out.Advance(frame_bytes);
        pcm_written = static_cast<u64>(frame) * channels;
    } else {
        const u64 offset = static_cast<u64>(skip) * channels;
        const u64 total = static_cast<u64>(frame) * channels;
        const u64 count = std::min(total - std::min(total, offset), cap);
        pcm_written = out.Write(pcm_.data() + offset * sample, count, sample);
    }
    r.samples = static_cast<u32>(pcm_written / std::max<u32>(channels, 1));
    gapless.current.skipped_samples = static_cast<u16>(gapless.current.skipped_samples + frame - r.samples);
    if (gapless.init.total_samples != 0) {
        gapless.current.total_samples -= std::min(gapless.current.total_samples, r.samples);
    }

    num_frames_ += 1;
    const u32 per_superframe = static_cast<u32>(std::max(info_.framesInSuperframe, 1));
    if (num_frames_ % per_superframe == 0) {
        in.Advance(superframe_remain_); // the superframe's padding
        superframe_remain_ = static_cast<u32>(info_.superframeSize);
        num_frames_ = 0;
    } else if (gapless.IsEnd()) {
        // the end of the stream: the rest of the superframe is decoded (and dropped)
        while (num_frames_ % per_superframe != 0) {
            u64 a = 0;
            const u8* s = in.Peek(std::max<u32>(superframe_remain_, 1), &a, staging_);
            if (s == nullptr || Atrac9Decode(handle_, s, static_cast<int>(std::min<u64>(a, kMaxDecodeInput)),
                                             reinterpret_cast<short*>(pcm_.data()), &used,
                                             (flags_ & kAt9NonInterleaved) != 0) != 0) {
                break;
            }
            in.Advance(static_cast<u64>(used));
            superframe_remain_ -= std::min<u32>(superframe_remain_, static_cast<u32>(used));
            r.frames += 1;
            num_frames_ += 1;
        }
        in.Advance(superframe_remain_);
        superframe_remain_ = static_cast<u32>(info_.superframeSize);
        num_frames_ = 0;
    }
    return r;
}

// ---- an instance (one decoder voice)
class Instance {
public:
    Instance(u32 id, u64 flags)
        : id_(id), flags_(flags), decoder_(static_cast<Encoding>((flags >> 7) & 7), (flags >> 32) & 0xFFFFFFF) {}

    void Execute(const Job& job);
    u32 Id() const {
        return id_;
    }

private:
    void ResetState() {
        total_samples_ = 0;
        gapless_.Reset();
        decoder_.Reset();
    }

    const u32 id_;
    const u64 flags_;
    At9Decoder decoder_;
    Gapless gapless_{};
    u64 total_samples_ = 0;
};

// A byte range of the sideband: structures are taken in order, while they fit.
class Sideband {
public:
    explicit Sideband(AjmBuffer b) : p_(b.address), n_(b.address != nullptr ? b.size : 0) {}
    template <class T>
    T* Take() {
        if (p_ == nullptr || n_ - used_ < sizeof(T)) {
            if (p_ != nullptr && used_ < n_) {
                truncated_ = true;
            }
            return nullptr;
        }
        T* t = reinterpret_cast<T*>(p_ + used_);
        used_ += sizeof(T);
        return t;
    }
    bool Empty() const {
        return p_ == nullptr || used_ >= n_;
    }
    bool Truncated() const {
        return truncated_;
    }

private:
    u8* p_;
    u64 n_;
    u64 used_ = 0;
    bool truncated_ = false;
};

void Instance::Execute(const Job& job) {
    const u64 control = ControlFlags(job.flags);
    const u64 run = RunFlags(job.flags);
    const u64 side = SidebandFlags(job.flags);

    // sideband input: format, gapless, resample parameters, initialization parameters (in this order)
    Sideband in_side(job.sideband_in);
    const SidebandFormat* in_format = (side & kSidebandFormat) && !in_side.Empty() ? in_side.Take<SidebandFormat>()
                                                                                  : nullptr;
    const SidebandGapless* in_gapless =
        (side & kSidebandGapless) && !in_side.Empty() ? in_side.Take<SidebandGapless>() : nullptr;
    const u64* in_resample = (control & kControlResample) ? in_side.Take<u64>() : nullptr;
    const u8* in_init = (control & kControlInitialize) ? reinterpret_cast<const u8*>(in_side.Take<u64>()) : nullptr;
    (void)in_format;   // (the decoder's output format is the instance's)
    (void)in_resample; // (no resampler: the game never asks for one)

    // sideband output: result, stream, format, gapless, multiple frames, codec info
    Sideband out_side(job.sideband_out);
    SidebandResult* res = out_side.Take<SidebandResult>();
    SidebandStream* stream = (side & kSidebandStream) && !out_side.Empty() ? out_side.Take<SidebandStream>() : nullptr;
    SidebandFormat* format = (side & kSidebandFormat) && !out_side.Empty() ? out_side.Take<SidebandFormat>() : nullptr;
    SidebandGapless* gapless =
        (side & kSidebandGapless) && !out_side.Empty() ? out_side.Take<SidebandGapless>() : nullptr;
    SidebandMFrame* mframe = (run & kRunMultipleFrames) && !out_side.Empty() ? out_side.Take<SidebandMFrame>() : nullptr;
    At9CodecInfo* codec_info = (run & kRunGetCodecInfo) && !out_side.Empty() ? out_side.Take<At9CodecInfo>() : nullptr;
    SidebandResult r{};
    if (stream != nullptr) {
        *stream = {};
    }
    if (format != nullptr) {
        *format = {};
    }
    if (gapless != nullptr) {
        *gapless = {};
    }
    if (mframe != nullptr) {
        *mframe = {};
    }

    if (control & kControlReset) {
        ResetState();
    }
    if (in_init != nullptr && !decoder_.Initialize(in_init)) {
        r.result |= kResInvalidData;
    }
    if (in_gapless != nullptr) {
        const SidebandGapless p = *in_gapless;
        const u32 processed = gapless_.init.total_samples - gapless_.current.total_samples;
        if (p.total_samples != 0 || p.skip_samples == 0) {
            if (p.total_samples >= processed) {
                const s64 diff = static_cast<s64>(gapless_.init.total_samples) - p.total_samples;
                gapless_.init.total_samples = p.total_samples;
                gapless_.current.total_samples = static_cast<u32>(gapless_.current.total_samples - diff);
            } else {
                r.result |= kResInvalidParameter;
            }
        }
        const u32 skipped = static_cast<u32>(gapless_.init.skip_samples - gapless_.current.skip_samples);
        if (p.skip_samples != 0 || p.total_samples == 0) {
            if (p.skip_samples >= skipped) {
                const s32 diff = static_cast<s32>(gapless_.init.skip_samples) - p.skip_samples;
                gapless_.init.skip_samples = p.skip_samples;
                gapless_.current.skip_samples = static_cast<u16>(gapless_.current.skip_samples - diff);
            } else {
                r.result |= kResInvalidParameter;
            }
        }
    }

    InputCursor in(job);
    OutputCursor out(job);
    u32 frames = 0;
    if (in.Total() != 0) {
        const bool gapless_loop = ((flags_ >> 10) & 1) != 0;
        for (;;) {
            if (gapless_loop && gapless_.IsEnd()) {
                gapless_.Reset();
                total_samples_ = 0;
            }
            if (!gapless_.IsEnd() && out.Remaining() < decoder_.NextFrameSize(gapless_)) {
                r.result |= kResNotEnoughRoom;
            }
            if (in.Remaining() < decoder_.MinimumInputSize()) {
                r.result |= kResPartialInput;
            }
            if (r.result != 0) {
                break;
            }
            const DecodeResult d = decoder_.Process(in, out, gapless_);
            // (as the oracle: the frame that follows a parsed RIFF header restarts the count without itself)
            total_samples_ = d.is_reset ? 0 : total_samples_ + d.samples;
            frames += d.frames;
            if (d.result != 0) {
                r.result |= d.result;
                r.internal_result = d.internal_result;
                break;
            }
            if ((run & kRunMultipleFrames) == 0) {
                break;
            }
        }
    }

    if (mframe != nullptr) {
        mframe->num_frames = frames;
    }
    if (stream != nullptr) {
        stream->input_consumed = static_cast<s32>(in.Total() - in.Remaining());
        stream->output_written = static_cast<s32>(out.Total() - out.Remaining());
        stream->total_decoded_samples = total_samples_;
    }
    if (format != nullptr) {
        *format = decoder_.Format();
    }
    if (gapless != nullptr) {
        *gapless = gapless_.current;
    }
    if (codec_info != nullptr) {
        *codec_info = decoder_.CodecInfo();
    }
    if (out_side.Truncated()) {
        r.result |= kResSidebandTruncated;
    }
    if (res != nullptr) {
        *res = r;
    }
}

// The statistics instance (0x80000): this AJM runs on the CPU in the caller's thread, no ACP engine time is used.
void ExecuteStatistics(const Job& job) {
    Sideband out_side(job.sideband_out);
    SidebandResult* res = out_side.Take<SidebandResult>();
    const u64 stat = (job.flags >> 15) & 0x1FFFF;
    if (stat & kStatEngine) {
        if (StatEngine* e = out_side.Take<StatEngine>()) {
            *e = {};
        }
    }
    if (stat & kStatEnginePerCodec) {
        if (StatEnginePerCodec* e = out_side.Take<StatEnginePerCodec>()) {
            *e = {};
        }
    }
    if (stat & kStatMemory) {
        if (StatMemory* m = out_side.Take<StatMemory>()) {
            *m = StatMemory{0x400000, 0x400000, 0x4200, 0x2000, 0x2000, 0x400}; // (the oracle's)
        }
    }
    if (res != nullptr) {
        *res = SidebandResult{out_side.Truncated() ? kResSidebandTruncated : 0, 0};
    }
}

// ---- contexts
struct Context {
    std::array<bool, kCodecMax> registered{};
    std::map<u32, std::unique_ptr<Instance>> instances; // by index
    u32 next_index = 1;
    std::map<u32, s32> batches; // finished batches by id, until waited for: their result
    u32 next_batch = 1;
};

std::mutex g_mutex;
std::map<u32, std::unique_ptr<Context>> g_contexts;
u32 g_next_context = 1;

Context* FindContext(u32 id) {
    const auto it = g_contexts.find(id);
    return it != g_contexts.end() ? it->second.get() : nullptr;
}

void SetBatchError(BatchError* e, s32 code, const u8* job, u32 offset, const void* ra) {
    if (e != nullptr) {
        *e = BatchError{code, job, offset, ra};
    }
}

// Parses the batch into jobs (nothing is executed when the batch is malformed or names an unknown instance).
s32 ParseBatch(Context& ctx, u8* batch, u32 size, std::vector<Job>& jobs, BatchError* error) {
    u32 at = 0;
    while (at < size) {
        if (size - at < 8) {
            SetBatchError(error, kErrMalformedBatch, batch + at, at, nullptr);
            return kErrMalformedBatch;
        }
        u32 head, body;
        std::memcpy(&head, batch + at, 4);
        std::memcpy(&body, batch + at + 4, 4);
        const u8* chunk = batch + at;
        at += 8;
        if (body > size - at) {
            SetBatchError(error, kErrMalformedBatch, chunk, at - 8, nullptr);
            return kErrMalformedBatch;
        }
        if (IdentOf(head) == kIdentInlineBuf) { // sideband data placed in the batch itself
            at += body;
            continue;
        }
        if (IdentOf(head) != kIdentJob) {
            SetBatchError(error, kErrMalformedBatch, chunk, at - 8, nullptr);
            return kErrMalformedBatch;
        }
        Job job;
        job.instance = PayloadOf(head);
        job.job_address = chunk;
        bool have_flags = false;
        const u32 end = at + body;
        while (at < end) {
            if (end - at < 8) { // (every sub-chunk is at least a header and a size)
                SetBatchError(error, kErrMalformedBatch, chunk, at, job.ra);
                return kErrMalformedBatch;
            }
            u32 h;
            std::memcpy(&h, batch + at, 4);
            const u32 ident = IdentOf(h);
            const bool is_flags = ident == kIdentControlFlags || ident == kIdentRunFlags;
            const u32 len = is_flags ? 8 : 16;
            if (end - at < len) {
                SetBatchError(error, kErrMalformedBatch, chunk, at, job.ra);
                return kErrMalformedBatch;
            }
            u32 bsize = 0;
            u8* baddr = nullptr;
            std::memcpy(&bsize, batch + at + 4, 4);
            if (!is_flags) {
                std::memcpy(&baddr, batch + at + 8, 8);
            }
            const AjmBuffer buf{baddr, bsize};
            bool ok = true;
            switch (ident) {
            case kIdentControlFlags:
            case kIdentRunFlags:
                ok = !have_flags;
                have_flags = true;
                job.control = ident == kIdentControlFlags;
                job.flags = (static_cast<u64>(PayloadOf(h)) << 32) | bsize;
                break;
            case kIdentReturnAddress:
                job.ra = baddr;
                break;
            case kIdentInputRunBuf:
                ok = job.num_inputs < job.inputs.size();
                if (ok) {
                    job.inputs[job.num_inputs++] = buf;
                }
                break;
            case kIdentOutputRunBuf:
                ok = job.num_outputs < job.outputs.size();
                if (ok) {
                    job.outputs[job.num_outputs++] = buf;
                }
                break;
            case kIdentInputControlBuf:
                ok = job.sideband_in.address == nullptr;
                if (baddr != nullptr && bsize != 0) {
                    job.sideband_in = buf;
                }
                break;
            case kIdentOutputControlBuf:
                ok = job.sideband_out.address == nullptr;
                if (baddr != nullptr && bsize != 0) {
                    job.sideband_out = buf;
                }
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                SetBatchError(error, kErrMalformedBatch, chunk, at, job.ra);
                return kErrMalformedBatch;
            }
            at += len;
        }
        if (!have_flags) {
            SetBatchError(error, kErrMalformedBatch, chunk, at, job.ra);
            return kErrMalformedBatch;
        }
        if (job.instance != kInstanceStatistics && !ctx.instances.contains(job.instance & kInstanceIndexMask)) {
            SetBatchError(error, kErrInvalidInstance, chunk, static_cast<u32>(chunk - batch), job.ra);
            return kErrInvalidInstance;
        }
        jobs.push_back(job);
    }
    return kOk;
}

void ExecuteJobs(Context& ctx, std::span<const Job> jobs) {
    for (const Job& job : jobs) {
        if (job.instance == kInstanceStatistics) {
            ExecuteStatistics(job);
        } else {
            ctx.instances.at(job.instance & kInstanceIndexMask)->Execute(job);
        }
    }
}

// ---- the batch job builders (they only write the caller's batch buffer)
u8* PutBuffer(u8* p, u32 ident, u64 size, const void* address) {
    const u32 h = Header(ident, 0);
    const u32 s = static_cast<u32>(size);
    std::memcpy(p, &h, 4);
    std::memcpy(p + 4, &s, 4);
    std::memcpy(p + 8, &address, 8);
    return p + 16;
}
u8* PutFlags(u8* p, u32 ident, u64 flags) {
    const u32 h = Header(ident, static_cast<u32>(flags >> 32));
    const u32 low = static_cast<u32>(flags);
    std::memcpy(p, &h, 4);
    std::memcpy(p + 4, &low, 4);
    return p + 8;
}
u8* FinishJob(u8* job, u32 instance, u8* end) {
    const u32 h = Header(kIdentJob, instance);
    const u32 size = static_cast<u32>(end - (job + 8));
    std::memcpy(job, &h, 4);
    std::memcpy(job + 4, &size, 4);
    return end;
}

u8* EncodeControl(u8* buffer, u32 instance, u64 flags, const void* side_in, u64 side_in_size, const void* side_out,
                  u64 side_out_size, const void* ra) {
    u8* p = buffer + 8;
    if (ra != nullptr) {
        p = PutBuffer(p, kIdentReturnAddress, 0, ra);
    }
    p = PutBuffer(p, kIdentInputControlBuf, side_in_size, side_in);
    flags &= instance == kInstanceStatistics ? 0x0000'0000'C001'8007ull : 0x0000'6000'0000'E7FFull;
    p = PutFlags(p, kIdentControlFlags, flags);
    p = PutBuffer(p, kIdentOutputControlBuf, side_out_size, side_out);
    return FinishJob(buffer, instance, p);
}

u8* EncodeRunSplit(u8* buffer, u32 instance, u64 flags, const AjmBuffer* in, u64 n_in, const AjmBuffer* out,
                   u64 n_out, const void* side_out, u64 side_out_size, const void* ra) {
    u8* p = buffer + 8;
    if (ra != nullptr) {
        p = PutBuffer(p, kIdentReturnAddress, 0, ra);
    }
    for (u64 i = 0; i < n_in; ++i) {
        p = PutBuffer(p, kIdentInputRunBuf, in[i].size, in[i].address);
    }
    p = PutFlags(p, kIdentRunFlags, flags & 0x0000'E000'0000'1FFFull);
    for (u64 i = 0; i < n_out; ++i) {
        p = PutBuffer(p, kIdentOutputRunBuf, out[i].size, out[i].address);
    }
    p = PutBuffer(p, kIdentOutputControlBuf, side_out_size, side_out);
    return FinishJob(buffer, instance, p);
}

// ---- the native exports
// An exception (std::bad_alloc from a container) never unwinds into the guest's SysV frames: the export answers
// SCE_AJM_ERROR_OUT_OF_MEMORY instead.
s32 ExportFailed() {
    std::fprintf(stderr, "AJM: an export failed on an exception (out of memory)\n");
    return kErrOutOfMemory;
}

SSPC_SYSV_ABI s32 Initialize(s64 reserved, u32* out_context) try {
    if (out_context == nullptr || reserved != 0) {
        return kErrInvalidParameter;
    }
    std::lock_guard lock(g_mutex);
    const u32 id = g_next_context++;
    g_contexts.emplace(id, std::make_unique<Context>());
    *out_context = id;
    SSPC_LOG(Audio, Info, "AJM: context {} (own AJM)", id);
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI s32 Finalize(u32 context) try {
    std::lock_guard lock(g_mutex);
    const auto it = g_contexts.find(context);
    if (it == g_contexts.end()) {
        return kErrInvalidContext;
    }
    SSPC_LOG(Audio, Info, "AJM: context {} finalized ({} instances, {} batches left)", context,
             it->second->instances.size(), it->second->batches.size());
    g_contexts.erase(it);
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI s32 ModuleRegister(u32 context, u32 codec, s64 reserved) try {
    if (reserved != 0) {
        return kErrInvalidParameter;
    }
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    if (codec >= kCodecMax) {
        return kErrInvalidParameter;
    }
    if (codec != kCodecAt9) { // (MP3/AAC: the game registers ATRAC9 only, this AJM has no other decoder)
        SSPC_LOG(Audio, Warning, "AJM: codec {} is not supported (ATRAC9 only)", codec);
        return kErrCodecNotSupported;
    }
    if (ctx->registered[codec]) {
        return kErrCodecAlreadyRegistered;
    }
    ctx->registered[codec] = true;
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI s32 ModuleUnregister(u32 context, u32 codec) try {
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    if (codec >= kCodecMax) {
        return kErrInvalidParameter;
    }
    if (!ctx->registered[codec]) {
        return kErrCodecNotRegistered;
    }
    ctx->registered[codec] = false;
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI s32 InstanceCreate(u32 context, u32 codec, u64 flags, u32* out_instance) try {
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    if (codec >= kCodecMax || out_instance == nullptr) {
        return kErrInvalidParameter;
    }
    if ((flags & 7) == 0) {
        return kErrWrongRevisionFlag;
    }
    if (!ctx->registered[codec]) {
        return kErrCodecNotRegistered;
    }
    if (((flags >> 7) & 7) > static_cast<u32>(Encoding::Float)) {
        return kErrFlagNotSupported;
    }
    if (ctx->instances.size() >= kMaxInstances) {
        return kErrOutOfResources;
    }
    u32 index = ctx->next_index;
    while (ctx->instances.contains(index)) { // (indices wrap within 1..kMaxInstances)
        index = index % kMaxInstances + 1;
    }
    ctx->next_index = index % kMaxInstances + 1;
    const u32 id = index | (codec << 14);
    ctx->instances.emplace(index, std::make_unique<Instance>(id, flags));
    *out_instance = id;
    SSPC_LOG(Audio, Debug, "AJM: instance {:#x} (flags {:#x})", id, flags);
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI s32 InstanceDestroy(u32 context, u32 instance) try {
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    if (ctx->instances.erase(instance & kInstanceIndexMask) == 0) {
        return kErrInvalidInstance;
    }
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

SSPC_SYSV_ABI void* BatchJobControlBufferRa(void* buffer, u32 instance, u64 flags, void* side_in, u64 side_in_size,
                                            void* side_out, u64 side_out_size, void* ra) {
    return EncodeControl(static_cast<u8*>(buffer), instance, flags, side_in, side_in_size, side_out, side_out_size,
                         ra);
}

SSPC_SYSV_ABI void* BatchJobRunSplitBufferRa(void* buffer, u32 instance, u64 flags, const AjmBuffer* in, u64 n_in,
                                             const AjmBuffer* out, u64 n_out, void* side_out, u64 side_out_size,
                                             void* ra) {
    return EncodeRunSplit(static_cast<u8*>(buffer), instance, flags, in, n_in, out, n_out, side_out, side_out_size,
                          ra);
}

// Parses and runs a batch; the caller holds g_mutex.
s32 StartLocked(Context& ctx, u8* batch, u32 size, BatchError* error, u32* out_id, std::vector<Job>& jobs) {
    if ((size & 7) != 0) {
        SetBatchError(error, kErrMalformedBatch, batch, 0, nullptr);
        return kErrMalformedBatch;
    }
    if (batch == nullptr && size != 0) {
        return kErrInvalidAddress;
    }
    if (out_id == nullptr) {
        return kErrInvalidParameter;
    }
    if (ctx.batches.size() >= kMaxBatches) {
        return kErrOutOfMemory;
    }
    if (const s32 r = ParseBatch(ctx, batch, size, jobs, error); r != kOk) {
        return r;
    }
    ExecuteJobs(ctx, jobs);
    u32 id = ctx.next_batch;
    while (ctx.batches.contains(id)) {
        id = id % kMaxBatches + 1;
    }
    ctx.next_batch = id % kMaxBatches + 1;
    ctx.batches.emplace(id, kOk);
    *out_id = id;
    return kOk;
}

SSPC_SYSV_ABI s32 BatchStartBuffer(u32 context, u8* batch, u32 size, s32 priority, BatchError* error, u32* out_id);
SSPC_SYSV_ABI s32 BatchWait(u32 context, u32 batch, u32 timeout, BatchError* error) try {
    (void)timeout; // (a batch is done when sceAjmBatchStartBuffer returns)
    (void)error;
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    const auto it = ctx->batches.find(batch);
    if (it == ctx->batches.end()) {
        return kErrInvalidBatch;
    }
    const s32 r = it->second;
    ctx->batches.erase(it);
    return r;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

// The game calls it after a failed start or wait, with its SceAjmBatchInfo and the batch's error (eboot 0x872c70).
struct BatchInfo {
    void* buffer;
    u64 offset;
    u64 size;
    void* current_job;
    void* current_ra;
};
SSPC_SYSV_ABI s32 BatchErrorDump(const BatchInfo* info, const BatchError* error) try {
    if (error == nullptr) {
        return kErrInvalidParameter;
    }
    SSPC_LOG(Audio, Error, "AJM batch error {:#x}: job {} (offset {:#x}, ra {}), batch {} {:#x}/{:#x} bytes",
             static_cast<u32>(error->error_code), error->job_address, error->command_offset, error->job_ra,
             info != nullptr ? info->buffer : nullptr, info != nullptr ? info->offset : 0,
             info != nullptr ? info->size : 0);
    return kOk;
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

// ======================= verify: the oracle on the same input =======================
using FnInit = s32 SSPC_SYSV_ABI (*)(s64, u32*);
using FnFinalize = s32 SSPC_SYSV_ABI (*)(u32);
using FnModule = s32 SSPC_SYSV_ABI (*)(u32, u32, s64);
using FnUnregister = s32 SSPC_SYSV_ABI (*)(u32, u32);
using FnCreate = s32 SSPC_SYSV_ABI (*)(u32, u32, u64, u32*);
using FnDestroy = s32 SSPC_SYSV_ABI (*)(u32, u32);
using FnStart = s32 SSPC_SYSV_ABI (*)(u32, u8*, u32, s32, BatchError*, u32*);
using FnWait = s32 SSPC_SYSV_ABI (*)(u32, u32, u32, BatchError*);

enum OracleSlot { kOInit, kOFinalize, kOModule, kOUnregister, kOCreate, kODestroy, kOStart, kOWait, kOControl,
                  kORun, kODump, kOCount };
void* g_oracle[kOCount] = {};

enum class Mode { Bridge, Native, Verify };
Mode g_mode = Mode::Bridge;

struct VerifyStats {
    u64 batches = 0, jobs = 0, run_jobs = 0, frames = 0, pcm_bytes = 0, sideband_bytes = 0;
    u64 pcm_mismatch_jobs = 0, pcm_mismatch_bytes = 0, sideband_mismatch_jobs = 0, oracle_failures = 0;
    s32 max_abs_diff = 0;
};
VerifyStats g_vs;
std::map<u32, u32> g_context_map;               // own context -> oracle context
std::map<std::pair<u32, u32>, u32> g_instance_map; // (own context, own instance) -> oracle instance
std::filesystem::path g_dump_dir;
std::map<std::pair<u32, u32>, std::pair<std::unique_ptr<std::ofstream>, std::unique_ptr<std::ofstream>>> g_dumps;
u64 g_logged_mismatches = 0;

void LogVerify(const char* when) {
    SSPC_LOG(Audio, Warning,
             "R10 AJM verify ({}): batches {}, jobs {} (run {}), frames {}, PCM bytes {}, sideband bytes {}; "
             "mismatches: PCM jobs {} ({} bytes, max |diff| {}), sideband jobs {}; oracle failures {}",
             when, g_vs.batches, g_vs.jobs, g_vs.run_jobs, g_vs.frames, g_vs.pcm_bytes, g_vs.sideband_bytes,
             g_vs.pcm_mismatch_jobs, g_vs.pcm_mismatch_bytes, g_vs.max_abs_diff, g_vs.sideband_mismatch_jobs,
             g_vs.oracle_failures);
}

void DumpPcm(u32 context, u32 instance, const Job& job, const std::vector<std::vector<u8>>& shadow, u64 own_written,
             u64 oracle_written) {
    if (g_dump_dir.empty()) {
        return;
    }
    auto& d = g_dumps[{context, instance}];
    if (!d.first) {
        std::error_code ec;
        std::filesystem::create_directories(g_dump_dir, ec);
        const auto base = g_dump_dir / fmt::format("ajm_c{}_{:05x}", context, instance);
        d.first = std::make_unique<std::ofstream>(base.string() + "_own.pcm", std::ios::binary);
        d.second = std::make_unique<std::ofstream>(base.string() + "_oracle.pcm", std::ios::binary);
    }
    for (u32 i = 0; i < job.num_outputs; ++i) {
        if (job.outputs[i].address == nullptr) {
            continue;
        }
        const u64 a = std::min<u64>(own_written, job.outputs[i].size);
        d.first->write(reinterpret_cast<const char*>(job.outputs[i].address), static_cast<std::streamsize>(a));
        own_written -= a;
        const u64 b = std::min<u64>(oracle_written, shadow[i].size());
        d.second->write(reinterpret_cast<const char*>(shadow[i].data()), static_cast<std::streamsize>(b));
        oracle_written -= b;
    }
}

// Runs the batch through the oracle into shadow copies, the own AJM into the game's buffers, and compares them.
s32 VerifyStart(u32 context, u8* batch, u32 size, s32 priority, BatchError* error, u32* out_id) {
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    std::vector<Job> jobs;
    {
        BatchError scratch{};
        std::vector<Job> probe;
        if (const s32 r = ((size & 7) == 0 && batch != nullptr) ? ParseBatch(*ctx, batch, size, probe, &scratch)
                                                                 : kErrMalformedBatch;
            r != kOk) {
            return StartLocked(*ctx, batch, size, error, out_id, jobs); // (the own error; the oracle is not asked)
        }
        jobs = std::move(probe);
    }
    const auto cit = g_context_map.find(context);
    // shadows: the game's output buffers and output sideband as they are now (so untouched bytes compare equal)
    std::vector<std::vector<std::vector<u8>>> out_shadow(jobs.size());
    std::vector<std::vector<u8>> side_shadow(jobs.size());
    std::vector<u8> shadow_batch(static_cast<size_t>(size) + 64 * jobs.size() + 64);
    u8* p = shadow_batch.data();
    bool mappable = cit != g_context_map.end();
    for (size_t j = 0; j < jobs.size() && mappable; ++j) {
        const Job& job = jobs[j];
        u32 oinst = job.instance;
        if (job.instance != kInstanceStatistics) {
            const auto iit = g_instance_map.find({context, job.instance});
            if (iit == g_instance_map.end()) {
                mappable = false;
                break;
            }
            oinst = iit->second;
        }
        if (job.sideband_out.address != nullptr) { // (as the outputs: bytes neither side writes compare equal)
            side_shadow[j].assign(job.sideband_out.address, job.sideband_out.address + job.sideband_out.size);
        }
        std::array<AjmBuffer, 8> outs{};
        for (u32 i = 0; i < job.num_outputs; ++i) {
            const AjmBuffer& o = job.outputs[i];
            if (o.address != nullptr) {
                out_shadow[j].emplace_back(o.address, o.address + o.size);
            } else {
                out_shadow[j].emplace_back();
            }
            outs[i] = AjmBuffer{o.address != nullptr ? out_shadow[j][i].data() : nullptr, o.size};
        }
        if (!job.control) {
            p = EncodeRunSplit(p, oinst, job.flags, job.inputs.data(), job.num_inputs, outs.data(), job.num_outputs,
                               side_shadow[j].empty() ? nullptr : side_shadow[j].data(), side_shadow[j].size(),
                               job.ra);
        } else {
            p = EncodeControl(p, oinst, job.flags, job.sideband_in.address, job.sideband_in.size,
                              side_shadow[j].empty() ? nullptr : side_shadow[j].data(), side_shadow[j].size(),
                              job.ra);
        }
    }
    s32 oracle_rc = -1;
    if (mappable && !jobs.empty()) {
        u32 obatch = 0;
        BatchError oerr{};
        oracle_rc = reinterpret_cast<FnStart>(g_oracle[kOStart])(cit->second, shadow_batch.data(),
                                                                 static_cast<u32>(p - shadow_batch.data()), priority,
                                                                 &oerr, &obatch);
        if (oracle_rc == kOk) {
            oracle_rc = reinterpret_cast<FnWait>(g_oracle[kOWait])(cit->second, obatch, 0xFFFFFFFFu, &oerr);
        }
        if (oracle_rc != kOk) {
            ++g_vs.oracle_failures;
            SSPC_LOG(Audio, Error, "R10 AJM verify: the oracle failed the batch ({:#x})", static_cast<u32>(oracle_rc));
        }
    }
    std::vector<Job> run_jobs;
    const s32 rc = StartLocked(*ctx, batch, size, error, out_id, run_jobs);
    if (rc != kOk || oracle_rc != kOk) {
        return rc;
    }
    ++g_vs.batches;
    for (size_t j = 0; j < jobs.size(); ++j) {
        const Job& job = jobs[j];
        ++g_vs.jobs;
        const u64 sb = job.sideband_out.size;
        g_vs.sideband_bytes += sb;
        const bool side_ok = sb == 0 || std::memcmp(job.sideband_out.address, side_shadow[j].data(), sb) == 0;
        // the frames: the multiple-frames sideband (Result, [Stream], [Format], [Gapless], MFrame)
        u64 own_written = 0, oracle_written = 0;
        const u64 side = SidebandFlags(job.flags);
        if ((side & kSidebandStream) && sb >= 24) {
            SidebandStream a, b;
            std::memcpy(&a, job.sideband_out.address + 8, sizeof(a));
            std::memcpy(&b, side_shadow[j].data() + 8, sizeof(b));
            own_written = static_cast<u64>(std::max(a.output_written, 0));
            oracle_written = static_cast<u64>(std::max(b.output_written, 0));
        }
        if (RunFlags(job.flags) & kRunMultipleFrames) {
            const u64 off = 8 + ((side & kSidebandStream) ? 16 : 0) + ((side & kSidebandFormat) ? 24 : 0) +
                            ((side & kSidebandGapless) ? 8 : 0);
            if (sb >= off + 8) {
                u32 n;
                std::memcpy(&n, job.sideband_out.address + off, 4);
                g_vs.frames += n;
            }
        }
        if (job.num_outputs != 0) {
            ++g_vs.run_jobs;
        }
        u64 bad = 0;
        u64 first_bad = ~0ull;
        u64 base = 0;
        for (u32 i = 0; i < job.num_outputs; ++i) {
            const u8* a = job.outputs[i].address;
            const u8* b = out_shadow[j][i].data();
            const u64 n = a != nullptr ? job.outputs[i].size : 0;
            if (n == 0) {
                continue;
            }
            g_vs.pcm_bytes += n;
            if (std::memcmp(a, b, n) != 0) {
                for (u64 k = 0; k < n; ++k) {
                    if (a[k] != b[k]) {
                        ++bad;
                        first_bad = std::min(first_bad, base + k);
                    }
                }
                for (u64 k = 0; k + 1 < n; k += 2) { // (as S16 samples: the game's format)
                    s16 x, y;
                    std::memcpy(&x, a + k, 2);
                    std::memcpy(&y, b + k, 2);
                    g_vs.max_abs_diff = std::max(g_vs.max_abs_diff, std::abs(static_cast<s32>(x) - y));
                }
            }
            base += n;
        }
        if (bad != 0) {
            ++g_vs.pcm_mismatch_jobs;
            g_vs.pcm_mismatch_bytes += bad;
        }
        if (!side_ok) {
            ++g_vs.sideband_mismatch_jobs;
        }
        if ((bad != 0 || !side_ok) && g_logged_mismatches++ < 20) {
            std::string a, b;
            for (u64 k = 0; k < std::min<u64>(sb, 0x40); k += 4) {
                u32 x, y;
                std::memcpy(&x, job.sideband_out.address + k, 4);
                std::memcpy(&y, side_shadow[j].data() + k, 4);
                a += fmt::format(" {:08x}", x);
                b += fmt::format(" {:08x}", y);
            }
            SSPC_LOG(Audio, Error,
                     "R10 AJM verify: instance {:#x} flags {:#x}: PCM {} bytes differ (first at {}), sideband {}\n"
                     "  own   {}\n  oracle{}",
                     job.instance, job.flags, bad, first_bad == ~0ull ? 0 : first_bad, side_ok ? "equal" : "differs",
                     a, b);
        }
        if (job.instance != kInstanceStatistics && job.num_outputs != 0) {
            DumpPcm(context, job.instance, job, out_shadow[j], own_written, oracle_written);
        }
    }
    if (g_vs.batches == 1 || g_vs.batches % 2000 == 0) {
        LogVerify(g_vs.batches == 1 ? "first batch" : "running");
        for (auto& [_, d] : g_dumps) {
            d.first->flush();
            d.second->flush();
        }
    }
    return rc;
}

SSPC_SYSV_ABI s32 BatchStartBuffer(u32 context, u8* batch, u32 size, s32 priority, BatchError* error, u32* out_id) try {
    if (g_mode == Mode::Verify) {
        return VerifyStart(context, batch, size, priority, error, out_id);
    }
    std::lock_guard lock(g_mutex);
    Context* ctx = FindContext(context);
    if (ctx == nullptr) {
        return kErrInvalidContext;
    }
    std::vector<Job> jobs;
    jobs.reserve(16);
    return StartLocked(*ctx, batch, size, error, out_id, jobs);
} catch (...) { // (no C++ exception crosses into the guest)
    return ExportFailed();
}

// verify wrappers of the state functions: the own result is the game's; the oracle follows it
void SelfTest();

// (the own call's result is returned whatever happens on the oracle's side; an exception there is a verify failure)
void OracleFailed() {
    std::lock_guard lock(g_mutex);
    ++g_vs.oracle_failures;
}

SSPC_SYSV_ABI s32 VInitialize(s64 reserved, u32* out_context) {
    const s32 r = Initialize(reserved, out_context);
    if (r == kOk) {
        bool failed = false;
        try {
            u32 o = 0;
            if (reinterpret_cast<FnInit>(g_oracle[kOInit])(0, &o) == kOk) {
                std::lock_guard lock(g_mutex);
                g_context_map[*out_context] = o;
            } else {
                failed = true;
            }
        } catch (...) {
            failed = true;
        }
        if (failed) {
            OracleFailed();
        }
        // (SelfTest calls VInitialize itself: the exchange makes that inner call skip it)
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) {
            try {
                SelfTest(); // (its own context: the game's sees nothing of it)
            } catch (...) {
                SSPC_LOG(Audio, Error, "R10 AJM selftest: stopped by an exception");
            }
        }
    }
    return r;
}
SSPC_SYSV_ABI s32 VFinalize(u32 context) {
    const s32 r = Finalize(context);
    std::lock_guard lock(g_mutex);
    const auto it = g_context_map.find(context);
    // the context's oracle instances and dump files go with it (erasing allocates nothing)
    for (auto iit = g_instance_map.begin(); iit != g_instance_map.end();) {
        if (iit->first.first != context) {
            ++iit;
            continue;
        }
        if (it != g_context_map.end()) {
            reinterpret_cast<FnDestroy>(g_oracle[kODestroy])(it->second, iit->second);
        }
        iit = g_instance_map.erase(iit);
    }
    for (auto d = g_dumps.begin(); d != g_dumps.end();) {
        d = d->first.first == context ? g_dumps.erase(d) : std::next(d);
    }
    if (it != g_context_map.end()) {
        reinterpret_cast<FnFinalize>(g_oracle[kOFinalize])(it->second);
        g_context_map.erase(it);
    }
    try {
        LogVerify("finalize");
    } catch (...) {
    }
    return r;
}
SSPC_SYSV_ABI s32 VModuleRegister(u32 context, u32 codec, s64 reserved) {
    const s32 r = ModuleRegister(context, codec, reserved);
    std::lock_guard lock(g_mutex);
    if (const auto it = g_context_map.find(context); r == kOk && it != g_context_map.end()) {
        reinterpret_cast<FnModule>(g_oracle[kOModule])(it->second, codec, reserved);
    }
    return r;
}
SSPC_SYSV_ABI s32 VModuleUnregister(u32 context, u32 codec) {
    const s32 r = ModuleUnregister(context, codec);
    std::lock_guard lock(g_mutex);
    if (const auto it = g_context_map.find(context); r == kOk && it != g_context_map.end()) {
        reinterpret_cast<FnUnregister>(g_oracle[kOUnregister])(it->second, codec);
    }
    return r;
}
SSPC_SYSV_ABI s32 VInstanceCreate(u32 context, u32 codec, u64 flags, u32* out_instance) {
    const s32 r = InstanceCreate(context, codec, flags, out_instance);
    bool failed = false;
    try {
        std::lock_guard lock(g_mutex);
        if (const auto it = g_context_map.find(context); r == kOk && it != g_context_map.end()) {
            u32 o = 0;
            if (reinterpret_cast<FnCreate>(g_oracle[kOCreate])(it->second, codec, flags, &o) == kOk) {
                g_instance_map[{context, *out_instance}] = o; // (may throw: the instance then goes unverified)
            } else {
                failed = true;
            }
        }
    } catch (...) {
        failed = true;
    }
    if (failed) {
        OracleFailed();
    }
    return r;
}
SSPC_SYSV_ABI s32 VInstanceDestroy(u32 context, u32 instance) {
    const s32 r = InstanceDestroy(context, instance);
    std::lock_guard lock(g_mutex);
    const auto it = g_context_map.find(context);
    const auto iit = g_instance_map.find({context, instance});
    if (r == kOk && it != g_context_map.end() && iit != g_instance_map.end()) {
        reinterpret_cast<FnDestroy>(g_oracle[kODestroy])(it->second, iit->second);
        g_instance_map.erase(iit);
        if (const auto d = g_dumps.find({context, instance}); d != g_dumps.end()) {
            g_dumps.erase(d); // (closes the dump files: an index may be reused)
        }
    }
    return r;
}

// SSPC_R10_AJM_SELFTEST=<file.at9>: a RIFF ATRAC9 file decoded through the verify path with the game's sequence -
// an S16 interleaved instance, a Reset | Initialize control job with the file's config bytes, then run jobs as the
// game's Wwise source builds them (0xc00000001001: MultipleFrames, Stream | Format; 0x38 bytes of sideband) of
// whole superframes into a 0x1000-sample ring (two output buffers where it wraps); every 7th job also takes its input
// as two buffers. A second pass decodes the whole file with RIFF parsing and gapless sideband.
// The selftest's error cases, on the own AJM alone (a context the oracle does not know, so it is never asked): config
// bytes LibAtrac9 must not see (channel config index 6/7, a set validation bit, a wrong header) leave the instance
// uninitialized - the control job answers "invalid data", a run job after it "not initialized", nothing is decoded -
// and a job whose last sub-chunk is cut short is a malformed batch.
void NegativeTest() {
    u32 ctx = 0;
    if (Initialize(0, &ctx) != kOk || ModuleRegister(ctx, kCodecAt9, 0) != kOk) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest (errors): no context");
        return;
    }
    u32 ok = 0, total = 0;
    alignas(16) std::array<u8, 0x200> batch{};
    BatchError err{};
    u32 id = 0;
    std::vector<u8> in(0x400, 0x5A);
    std::vector<s16> pcm(0x800, 0x1234);
    static constexpr u8 kBad[][4] = {{0xFE, 0x7E, 0x00, 0x00},  // 48 kHz, channel config 7
                                     {0xFE, 0x7C, 0x00, 0x00},  // channel config 6
                                     {0xFE, 0x75, 0x00, 0x00},  // validation bit set
                                     {0x00, 0x74, 0x00, 0x00}}; // no 0xFE header
    for (const auto& cfg : kBad) {
        u32 inst = 0;
        if (InstanceCreate(ctx, kCodecAt9, (2ull << 3) | 1, &inst) != kOk) {
            break;
        }
        u64 init = 0;
        std::memcpy(&init, cfg, 4);
        SidebandResult cres{-1, -1};
        u8* end = static_cast<u8*>(BatchJobControlBufferRa(batch.data(), inst, 0x6001, &init, 8, &cres, 8, nullptr));
        const bool control_ok = BatchStartBuffer(ctx, batch.data(), static_cast<u32>(end - batch.data()), 0x28, &err,
                                                 &id) == kOk &&
                                BatchWait(ctx, id, 0xFFFFFFFFu, &err) == kOk && (cres.result & kResInvalidData) != 0;
        struct alignas(8) Side {
            SidebandResult res;
            SidebandStream stream;
        } side{};
        AjmBuffer i{in.data(), in.size()};
        AjmBuffer o{reinterpret_cast<u8*>(pcm.data()), pcm.size() * 2};
        end = static_cast<u8*>(BatchJobRunSplitBufferRa(batch.data(), inst, 0x800000001001ull, &i, 1, &o, 1, &side,
                                                        sizeof(side), nullptr));
        const bool run_ok = BatchStartBuffer(ctx, batch.data(), static_cast<u32>(end - batch.data()), 0x28, &err,
                                             &id) == kOk &&
                            BatchWait(ctx, id, 0xFFFFFFFFu, &err) == kOk &&
                            (side.res.result & kResNotInitialized) != 0 && side.stream.output_written == 0 &&
                            pcm[0] == 0x1234;
        total += 2;
        ok += (control_ok ? 1 : 0) + (run_ok ? 1 : 0);
        (void)InstanceDestroy(ctx, inst);
    }
    { // a job of 4 body bytes: its one sub-chunk has no room for a header and a size
        u32 inst = 0;
        if (InstanceCreate(ctx, kCodecAt9, (2ull << 3) | 1, &inst) == kOk) {
            batch.fill(0);
            const u32 h = Header(kIdentJob, inst);
            const u32 body = 4;
            std::memcpy(batch.data(), &h, 4);
            std::memcpy(batch.data() + 4, &body, 4);
            ++total;
            ok += BatchStartBuffer(ctx, batch.data(), 16, 0x28, &err, &id) == kErrMalformedBatch ? 1 : 0;
            (void)InstanceDestroy(ctx, inst);
        }
    }
    (void)Finalize(ctx);
    SSPC_LOG(Audio, Warning, "R10 AJM selftest (errors): {}/{} cases as expected", ok, total);
}

void SelfTest() {
    const char* path = std::getenv("SSPC_R10_AJM_SELFTEST");
    if (path == nullptr || *path == 0) {
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 12 + 8 + 52 + 8) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest: cannot read {}", path);
        return;
    }
    // the chunks
    u8 config[4]{};
    u32 channels = 0, block = 0, block_samples = 0;
    size_t data = 0, data_size = 0;
    for (size_t at = 12; at + 8 <= file.size();) {
        u32 tag, len;
        std::memcpy(&tag, &file[at], 4);
        std::memcpy(&len, &file[at + 4], 4);
        if (tag == 0x20746D66 && len >= 52 && at + 8 + 52 <= file.size()) {
            u16 ch, align, spb;
            std::memcpy(&ch, &file[at + 8 + 2], 2);
            std::memcpy(&align, &file[at + 8 + 12], 2);
            std::memcpy(&spb, &file[at + 8 + 18], 2);
            channels = ch;
            block = align;
            block_samples = spb;
            std::memcpy(config, &file[at + 8 + 44], 4);
        }
        if (tag == 0x61746164) {
            data = at + 8;
            data_size = std::min<size_t>(len, file.size() - data);
            break;
        }
        at += 8 + len;
    }
    if (data == 0 || block == 0 || block_samples == 0 || channels == 0) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest: {} is not an ATRAC9 RIFF file", path);
        return;
    }
    VerifyStats before;
    {
        std::lock_guard lock(g_mutex);
        before = g_vs;
    }
    u32 ctx = 0;
    if (VInitialize(0, &ctx) != kOk || VModuleRegister(ctx, kCodecAt9, 0) != kOk) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest: no context");
        return;
    }
    u32 inst = 0;
    if (VInstanceCreate(ctx, kCodecAt9, (static_cast<u64>(channels) << 3) | 1, &inst) != kOk) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest: instance create failed");
        (void)VFinalize(ctx);
        return;
    }
    alignas(16) std::array<u8, 0x400> batch{};
    BatchError err{};
    u32 id = 0;
    struct alignas(8) Side {
        u8 b[0x38];
    } side{}, cside{};
    u64 init = 0;
    std::memcpy(&init, config, 4);
    u8* end = static_cast<u8*>(BatchJobControlBufferRa(batch.data(), inst, 0x6001, &init, 8, &cside, 8,
                                                         reinterpret_cast<void*>(&SelfTest)));
    if (BatchStartBuffer(ctx, batch.data(), static_cast<u32>(end - batch.data()), 0x28, &err, &id) != kOk ||
        BatchWait(ctx, id, 0xFFFFFFFFu, &err) != kOk) {
        SSPC_LOG(Audio, Error, "R10 AJM selftest: control job failed");
    }
    std::vector<s16> ring(0x1000 * channels);
    u32 write = 0;
    size_t pos = 0;
    u32 n = 0;
    while (pos < data_size) {
        // two superframes (or what is left) per job, as the ring has room
        const u32 sfs = std::min<u32>(2, static_cast<u32>((data_size - pos) / block));
        if (sfs == 0) {
            break;
        }
        const u32 samples = sfs * block_samples;
        AjmBuffer in[2];
        u64 n_in = 1;
        const u64 in_bytes = static_cast<u64>(sfs) * block;
        if (n % 7 == 6 && sfs == 2) {
            in[0] = AjmBuffer{&file[data + pos], block};
            in[1] = AjmBuffer{&file[data + pos + block], block};
            n_in = 2;
        } else {
            in[0] = AjmBuffer{&file[data + pos], in_bytes};
        }
        AjmBuffer out[2];
        u64 n_out = 1;
        const u32 first = std::min<u32>(samples, 0x1000 - write);
        out[0] = AjmBuffer{reinterpret_cast<u8*>(&ring[static_cast<size_t>(write) * channels]),
                           static_cast<u64>(first) * channels * 2};
        if (first < samples) {
            out[1] = AjmBuffer{reinterpret_cast<u8*>(ring.data()), static_cast<u64>(samples - first) * channels * 2};
            n_out = 2;
        }
        end = static_cast<u8*>(BatchJobRunSplitBufferRa(batch.data(), inst, 0xc00000001001ull, in, n_in, out, n_out,
                                                        &side, sizeof(side), reinterpret_cast<void*>(&SelfTest)));
        if (BatchStartBuffer(ctx, batch.data(), static_cast<u32>(end - batch.data()), 0x28, &err, &id) != kOk ||
            BatchWait(ctx, id, 0xFFFFFFFFu, &err) != kOk) {
            SSPC_LOG(Audio, Error, "R10 AJM selftest: run job {} failed", n);
            break;
        }
        write = (write + samples) % 0x1000;
        pos += in_bytes;
        ++n;
    }
    (void)VInstanceDestroy(ctx, inst);
    u64 pass1_frames = 0;
    {
        std::lock_guard lock(g_mutex);
        pass1_frames = g_vs.frames - before.frames;
    }

    // pass 2: the whole file, RIFF parsed by the decoder, gapless sideband (Result, Stream, Format, Gapless, MFrame)
    u32 inst2 = 0;
    if (VInstanceCreate(ctx, kCodecAt9, (static_cast<u64>(channels) << 3) | (kAt9ParseRiff << 32) | 1, &inst2) ==
        kOk) {
        std::vector<s16> pcm(static_cast<size_t>(block_samples) * 8 * channels);
        size_t at = 0;
        for (u32 k = 0; k < 100000 && at < file.size(); ++k) {
            struct alignas(8) Side2 {
                u8 b[8 + 16 + 24 + 8 + 8];
            } s2{};
            AjmBuffer in{&file[at], file.size() - at};
            AjmBuffer out{reinterpret_cast<u8*>(pcm.data()), pcm.size() * 2};
            end = static_cast<u8*>(BatchJobRunSplitBufferRa(batch.data(), inst2, 0xe00000001001ull, &in, 1, &out, 1,
                                                            &s2, sizeof(s2), nullptr));
            if (BatchStartBuffer(ctx, batch.data(), static_cast<u32>(end - batch.data()), 0x28, &err, &id) != kOk ||
                BatchWait(ctx, id, 0xFFFFFFFFu, &err) != kOk) {
                break;
            }
            SidebandStream st;
            std::memcpy(&st, s2.b + 8, sizeof(st));
            if (st.input_consumed <= 0) {
                break;
            }
            at += static_cast<size_t>(st.input_consumed);
        }
        (void)VInstanceDestroy(ctx, inst2);
    }
    (void)VFinalize(ctx);
    SSPC_LOG(Audio, Warning, "R10 AJM selftest {}: {} ch, superframe {} bytes / {} samples, {} game-style jobs ({} "
                             "frames); then the RIFF/gapless pass",
             path, channels, block, block_samples, n, pass1_frames);
    {
        std::lock_guard lock(g_mutex);
        LogVerify("selftest");
    }
    NegativeTest();
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

Mode ReadMode() {
    const char* v = std::getenv("SSPC_R10_AJM");
    const std::string s = v != nullptr ? v : "";
    if (s == "native") {
        return Mode::Native;
    }
    if (s == "verify") {
        return Mode::Verify;
    }
    if (!s.empty() && s != "bridge") {
        SSPC_LOG(Audio, Error, "SSPC_R10_AJM={}: not native|verify|bridge (bridge)", s);
    }
    return Mode::Bridge;
}

#define AJ(nid, name, fn, slot)                                                                                     \
    Runtime::Export {                                                                                               \
        nid, "libSceAjm", "libSceAjm", name, Fn(&fn), verify ? &g_oracle[slot] : nullptr, "ajm"                     \
    }

std::vector<Runtime::Export> Collect() {
    g_mode = ReadMode();
    if (g_mode == Mode::Bridge) {
        return {};
    }
    const bool verify = g_mode == Mode::Verify;
    if (verify) {
        if (const char* d = std::getenv("SSPC_R10_AJM_DUMP"); d != nullptr && *d != 0) {
            g_dump_dir = d;
        }
    }
    SSPC_LOG(Audio, Warning, "R10: libSceAjm is SecondSonPC's own ({}){}", verify ? "verify against the oracle" : "native",
             g_dump_dir.empty() ? "" : ", PCM dumps in " + g_dump_dir.string());
    // In verify mode every entry observes the oracle's function of the same import (its address lands in g_oracle).
    return {
        AJ("dl+4eHSzUu4", "sceAjmInitialize", VInitialize, kOInit),
        AJ("MHur6qCsUus", "sceAjmFinalize", VFinalize, kOFinalize),
        AJ("Q3dyFuwGn64", "sceAjmModuleRegister", VModuleRegister, kOModule),
        AJ("Wi7DtlLV+KI", "sceAjmModuleUnregister", VModuleUnregister, kOUnregister),
        AJ("AxoDrINp4J8", "sceAjmInstanceCreate", VInstanceCreate, kOCreate),
        AJ("RbLbuKv8zho", "sceAjmInstanceDestroy", VInstanceDestroy, kODestroy),
        AJ("fFFkk0xfGWs", "sceAjmBatchStartBuffer", BatchStartBuffer, kOStart),
        AJ("-qLsfDAywIY", "sceAjmBatchWait", BatchWait, kOWait),
        AJ("dmDybN--Fn8", "sceAjmBatchJobControlBufferRa", BatchJobControlBufferRa, kOControl),
        AJ("7jdAXK+2fMo", "sceAjmBatchJobRunSplitBufferRa", BatchJobRunSplitBufferRa, kORun),
        AJ("WfAiBW8Wcek", "sceAjmBatchErrorDump", BatchErrorDump, kODump),
    };
}
#undef AJ

} // namespace

std::span<const Runtime::Export> AjmExports() {
    static const std::vector<Runtime::Export> all = [] {
        auto v = Collect();
        if (g_mode == Mode::Native) { // (native: the plain functions, no oracle bound behind them)
            for (auto& e : v) {
                e.original = nullptr;
                if (e.function == Fn(&VInitialize)) e.function = Fn(&Initialize);
                if (e.function == Fn(&VFinalize)) e.function = Fn(&Finalize);
                if (e.function == Fn(&VModuleRegister)) e.function = Fn(&ModuleRegister);
                if (e.function == Fn(&VModuleUnregister)) e.function = Fn(&ModuleUnregister);
                if (e.function == Fn(&VInstanceCreate)) e.function = Fn(&InstanceCreate);
                if (e.function == Fn(&VInstanceDestroy)) e.function = Fn(&InstanceDestroy);
            }
        }
        return v;
    }();
    return all;
}

} // namespace SecondSon::Audio
