#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace maxwatch {

struct AnnexBParseResult {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> avccLengthPrefixedPayload;
    bool hasKeyframeIDR = false;
};

// Parses an Annex-B H.264 access unit (00 00 00 01 / 00 00 01 start codes),
// extracts SPS (NAL 7) and PPS (NAL 8) if present, and converts slice/SEI NALs
// into 4-byte big-endian length-prefixed AVCC format required by ISO-BMFF MP4/fMP4.
AnnexBParseResult parseAnnexBAccessUnit(const uint8_t* data, size_t len);

class Fmp4SegmentMuxer {
public:
    using SegmentCallback = std::function<void(const std::vector<uint8_t>& bytes, bool isInitSegment)>;

    Fmp4SegmentMuxer(
        int width,
        int height,
        bool hasAudio,
        uint32_t audioSampleRate,
        uint16_t audioChannels,
        SegmentCallback onSegment
    );

    void setCodecPrivateData(
        const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps,
        const std::vector<uint8_t>& audioSpecificConfig = {}
    );

    bool isInitEmitted() const { return initEmitted_; }

    // Appends an AVCC length-prefixed H.264 sample with PTS in 90 kHz ticks.
    // Automatically emits the initialization segment on first keyframe once SPS/PPS are known,
    // and flushes a 1.0-second `moof + mdat` fragment whenever >= 90,000 ticks (1.0s) have elapsed at a keyframe.
    void appendVideoSample(
        const std::vector<uint8_t>& avccSample,
        uint64_t pts90k,
        uint32_t defaultDuration90k,
        bool isKeyframe
    );

    // Appends a raw AAC-LC frame (typically 1024 PCM samples @ 48 kHz).
    void appendAudioSample(
        const std::vector<uint8_t>& rawAacFrame,
        uint32_t sampleDurationFrames = 1024
    );

    // Flushes any remaining buffered sub-second video/audio frames into a final `moof + mdat` fragment.
    void flushFinalFragment();

    double totalEmittedVideoSeconds() const {
        return static_cast<double>(totalVideoTicks90k_) / 90000.0;
    }

    static std::vector<uint8_t> makeDefaultAacSpecificConfig(uint32_t sampleRate, uint16_t channels);

private:
    struct PendingVideoSample {
        std::vector<uint8_t> data;
        uint64_t pts90k = 0;
        uint32_t duration90k = 3000;
        bool isKeyframe = false;
    };

    struct PendingAudioSample {
        std::vector<uint8_t> data;
        uint32_t durationSamples = 1024;
    };

    void emitInitSegmentIfNeeded();
    void emitCurrentFragment();

    std::vector<uint8_t> buildInitSegmentBytes() const;
    std::vector<uint8_t> buildMediaFragmentBytes(uint32_t sequenceNumber);

    int width_;
    int height_;
    bool hasAudio_;
    uint32_t audioSampleRate_;
    uint16_t audioChannels_;
    SegmentCallback onSegment_;

    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<uint8_t> aacAsc_;

    bool initEmitted_ = false;
    uint32_t nextSequenceNumber_ = 1;

    uint64_t fragmentBaseVideoPts90k_ = 0;
    uint64_t totalVideoTicks90k_ = 0;
    uint64_t nextAudioDecodeSamples_ = 0;

    std::vector<PendingVideoSample> pendingVideo_;
    std::vector<PendingAudioSample> pendingAudio_;
};

// Represents an in-RAM, zero-temp-file standard unfragmented ISO-BMFF MP4 index (`ftyp + moov`)
// whose `co64` chunk offsets point directly into the encrypted `mdat` payloads of `VaultClipReader`.
// Enables native frame-accurate `MF_PD_DURATION`, instant `stss` keyframe scrubbing via `SetCurrentPosition`,
// >4 GB multi-hour playback without Windows `mfmp4srcsnk.dll` fragmented-MP4 limits, and faststart `.mp4` export.
struct VirtualStandardMp4Layout {
    bool valid = false;
    std::vector<uint8_t> synthesizedHeader;      // Complete unfragmented `ftyp + moov`
    uint64_t origInitPlaintextLen = 0;           // Byte offset where media fragments start in raw VaultClipReader plaintext
    uint64_t mediaPlaintextLen = 0;              // Total byte length of all media fragments
    double durationSeconds = 0.0;                // Exact presentation duration in seconds
    std::vector<uint64_t> freeFourccMediaOffsets; // Offsets (relative to media start) where 4-byte 'free' replaces 'moof'/'sidx'

    uint64_t totalVirtualSize() const {
        return static_cast<uint64_t>(synthesizedHeader.size()) + mediaPlaintextLen;
    }
};

VirtualStandardMp4Layout buildVirtualStandardMp4Layout(
    const uint8_t* initBytes,
    size_t initLen,
    size_t mediaFragmentCount,
    const std::function<std::vector<uint8_t>(size_t fragmentIndex)>& readMediaFragment
);

void applyFreeFourccPatchesToMediaSlice(
    const VirtualStandardMp4Layout& layout,
    uint64_t sliceMediaOffset,
    uint8_t* buf,
    size_t bufLen
);

} // namespace maxwatch
