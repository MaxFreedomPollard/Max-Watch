#include "Fmp4Muxer.hpp"

#include <algorithm>
#include <cstring>

namespace maxwatch {

namespace {

void w8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void w16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void w24(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void w32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void w64(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>((v >> s) & 0xFF));
}
void wFourCC(std::vector<uint8_t>& b, const char cc[4]) {
    b.insert(b.end(), cc, cc + 4);
}
void wZeros(std::vector<uint8_t>& b, size_t count) {
    b.insert(b.end(), count, 0);
}
void patch32(std::vector<uint8_t>& b, size_t offset, uint32_t v) {
    b[offset + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    b[offset + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    b[offset + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[offset + 3] = static_cast<uint8_t>(v & 0xFF);
}

struct BoxScope {
    std::vector<uint8_t>& buf;
    size_t start;
    BoxScope(std::vector<uint8_t>& b, const char type[4]) : buf(b), start(b.size()) {
        w32(buf, 0);
        wFourCC(buf, type);
    }
    void finish() {
        patch32(buf, start, static_cast<uint32_t>(buf.size() - start));
    }
};

void writeUnityMatrix(std::vector<uint8_t>& b) {
    w32(b, 0x00010000); w32(b, 0);          w32(b, 0);
    w32(b, 0);          w32(b, 0x00010000); w32(b, 0);
    w32(b, 0);          w32(b, 0);          w32(b, 0x40000000);
}

void writeEmptySampleTables(std::vector<uint8_t>& stbl) {
    {
        BoxScope stts(stbl, "stts");
        w32(stbl, 0); // version/flags
        w32(stbl, 0); // entry_count
        stts.finish();
    }
    {
        BoxScope stsc(stbl, "stsc");
        w32(stbl, 0);
        w32(stbl, 0);
        stsc.finish();
    }
    {
        BoxScope stsz(stbl, "stsz");
        w32(stbl, 0);
        w32(stbl, 0); // sample_size
        w32(stbl, 0); // sample_count
        stsz.finish();
    }
    {
        BoxScope stco(stbl, "stco");
        w32(stbl, 0);
        w32(stbl, 0);
        stco.finish();
    }
}

void writeSelfContainedDinf(std::vector<uint8_t>& minf) {
    BoxScope dinf(minf, "dinf");
    {
        BoxScope dref(minf, "dref");
        w32(minf, 0); // version/flags
        w32(minf, 1); // entry_count = 1
        {
            BoxScope url(minf, "url ");
            w8(minf, 0);
            w24(minf, 0x000001); // self-contained
            url.finish();
        }
        dref.finish();
    }
    dinf.finish();
}

} // namespace

AnnexBParseResult parseAnnexBAccessUnit(const uint8_t* data, size_t len) {
    AnnexBParseResult res;
    if (!data || len < 4) return res;

    auto findStartCode = [&](size_t from, size_t& scPos, size_t& scLen) -> bool {
        for (size_t i = from; i + 3 <= len; ++i) {
            if (data[i] == 0 && data[i + 1] == 0) {
                if (data[i + 2] == 1) {
                    scPos = i;
                    scLen = 3;
                    return true;
                }
                if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
                    scPos = i;
                    scLen = 4;
                    return true;
                }
            }
        }
        return false;
    };

    size_t scPos = 0, scLen = 0;
    if (!findStartCode(0, scPos, scLen)) {
        return res;
    }

    while (true) {
        size_t nalStart = scPos + scLen;
        size_t nextPos = 0, nextLen = 0;
        size_t nalEnd = len;
        bool hasNext = findStartCode(nalStart, nextPos, nextLen);
        if (hasNext) {
            nalEnd = nextPos;
        }
        while (nalEnd > nalStart && data[nalEnd - 1] == 0) {
            // Trim trailing zero padding bytes between NAL units if any
            // (safe for SPS/PPS/slices because RBSP always ends with rbsp_stop_one_bit != 0)
            nalEnd--;
        }
        if (nalEnd > nalStart) {
            uint8_t nalType = data[nalStart] & 0x1F;
            size_t nalSize = nalEnd - nalStart;
            if (nalType == 7) {
                res.sps.assign(data + nalStart, data + nalEnd);
            } else if (nalType == 8) {
                res.pps.assign(data + nalStart, data + nalEnd);
            } else if (nalType != 9) { // Skip Access Unit Delimiter (AUD) if present
                if (nalType == 5) {
                    res.hasKeyframeIDR = true;
                }
                w32(res.avccLengthPrefixedPayload, static_cast<uint32_t>(nalSize));
                res.avccLengthPrefixedPayload.insert(
                    res.avccLengthPrefixedPayload.end(),
                    data + nalStart,
                    data + nalEnd
                );
            }
        }
        if (!hasNext) break;
        scPos = nextPos;
        scLen = nextLen;
    }
    return res;
}

std::vector<uint8_t> Fmp4SegmentMuxer::makeDefaultAacSpecificConfig(uint32_t sampleRate, uint16_t channels) {
    // ISO/IEC 14496-3 AudioSpecificConfig for AAC-LC (audioObjectType = 2)
    static const uint32_t kRates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    uint8_t srIdx = 3; // default 48000 Hz
    for (uint8_t i = 0; i < 13; ++i) {
        if (kRates[i] == sampleRate) {
            srIdx = i;
            break;
        }
    }
    uint8_t chCfg = static_cast<uint8_t>(std::clamp<int>(channels, 1, 2));
    uint8_t b0 = static_cast<uint8_t>((2 << 3) | ((srIdx >> 1) & 0x07));
    uint8_t b1 = static_cast<uint8_t>(((srIdx & 0x01) << 7) | ((chCfg & 0x0F) << 3));
    return {b0, b1};
}

Fmp4SegmentMuxer::Fmp4SegmentMuxer(
    int width,
    int height,
    bool hasAudio,
    uint32_t audioSampleRate,
    uint16_t audioChannels,
    SegmentCallback onSegment
) : width_(std::max(16, width)),
    height_(std::max(16, height)),
    hasAudio_(hasAudio),
    audioSampleRate_(audioSampleRate > 0 ? audioSampleRate : 48000),
    audioChannels_(audioChannels > 0 ? audioChannels : 1),
    onSegment_(std::move(onSegment)) {
    aacAsc_ = makeDefaultAacSpecificConfig(audioSampleRate_, audioChannels_);
}

void Fmp4SegmentMuxer::setCodecPrivateData(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    const std::vector<uint8_t>& audioSpecificConfig
) {
    if (!sps.empty()) sps_ = sps;
    if (!pps.empty()) pps_ = pps;
    if (audioSpecificConfig.size() >= 2) {
        aacAsc_.assign(audioSpecificConfig.begin(), audioSpecificConfig.begin() + 2);
    }
}

void Fmp4SegmentMuxer::appendVideoSample(
    const std::vector<uint8_t>& avccSample,
    uint64_t pts90k,
    uint32_t defaultDuration90k,
    bool isKeyframe
) {
    if (avccSample.empty()) return;
    if (!initEmitted_) {
        if (sps_.empty() || pps_.empty() || !isKeyframe) {
            return;
        }
        emitInitSegmentIfNeeded();
        fragmentBaseVideoPts90k_ = pts90k;
    }

    // Flush completed >= 1.0 second fragment cleanly at keyframe boundary!
    if (isKeyframe && !pendingVideo_.empty()) {
        uint64_t firstPts = pendingVideo_.front().pts90k;
        if (pts90k >= firstPts && (pts90k - firstPts) >= 90000ULL) {
            for (size_t i = 0; i + 1 < pendingVideo_.size(); ++i) {
                if (pendingVideo_[i + 1].pts90k > pendingVideo_[i].pts90k) {
                    pendingVideo_[i].duration90k = static_cast<uint32_t>(pendingVideo_[i + 1].pts90k - pendingVideo_[i].pts90k);
                }
            }
            if (pts90k > pendingVideo_.back().pts90k) {
                pendingVideo_.back().duration90k = static_cast<uint32_t>(pts90k - pendingVideo_.back().pts90k);
            }
            emitCurrentFragment();
            fragmentBaseVideoPts90k_ = pts90k;
        }
    }

    if (pendingVideo_.empty()) {
        fragmentBaseVideoPts90k_ = pts90k;
    }

    PendingVideoSample s;
    s.data = avccSample;
    s.pts90k = pts90k;
    s.duration90k = defaultDuration90k > 0 ? defaultDuration90k : 3000;
    s.isKeyframe = isKeyframe;
    pendingVideo_.push_back(std::move(s));
}

void Fmp4SegmentMuxer::appendAudioSample(
    const std::vector<uint8_t>& rawAacFrame,
    uint32_t sampleDurationFrames
) {
    if (!hasAudio_ || rawAacFrame.empty()) return;
    PendingAudioSample a;
    a.data = rawAacFrame;
    a.durationSamples = sampleDurationFrames > 0 ? sampleDurationFrames : 1024;
    pendingAudio_.push_back(std::move(a));
}

void Fmp4SegmentMuxer::flushFinalFragment() {
    if (!initEmitted_ || pendingVideo_.empty()) return;
    for (size_t i = 0; i + 1 < pendingVideo_.size(); ++i) {
        if (pendingVideo_[i + 1].pts90k > pendingVideo_[i].pts90k) {
            pendingVideo_[i].duration90k = static_cast<uint32_t>(pendingVideo_[i + 1].pts90k - pendingVideo_[i].pts90k);
        }
    }
    emitCurrentFragment();
}

void Fmp4SegmentMuxer::emitInitSegmentIfNeeded() {
    if (initEmitted_) return;
    auto initBytes = buildInitSegmentBytes();
    initEmitted_ = true;
    if (onSegment_) {
        onSegment_(initBytes, true);
    }
}

void Fmp4SegmentMuxer::emitCurrentFragment() {
    if (pendingVideo_.empty()) return;
    uint64_t fragVideoTicks = 0;
    for (const auto& v : pendingVideo_) fragVideoTicks += v.duration90k;

    auto fragBytes = buildMediaFragmentBytes(nextSequenceNumber_++);
    totalVideoTicks90k_ += fragVideoTicks;

    uint64_t fragAudioSamples = 0;
    for (const auto& a : pendingAudio_) fragAudioSamples += a.durationSamples;
    nextAudioDecodeSamples_ += fragAudioSamples;

    pendingVideo_.clear();
    pendingAudio_.clear();

    if (onSegment_) {
        onSegment_(fragBytes, false);
    }
}

std::vector<uint8_t> Fmp4SegmentMuxer::buildInitSegmentBytes() const {
    std::vector<uint8_t> out;
    out.reserve(1024);

    // 1. ftyp
    {
        BoxScope ftyp(out, "ftyp");
        wFourCC(out, "isom");
        w32(out, 0x00000200);
        wFourCC(out, "isom");
        wFourCC(out, "iso5");
        wFourCC(out, "iso6");
        wFourCC(out, "mp41");
        wFourCC(out, "avc1");
        ftyp.finish();
    }

    // 2. moov
    {
        BoxScope moov(out, "moov");

        // mvhd
        {
            BoxScope mvhd(out, "mvhd");
            w32(out, 0);          // version 0, flags 0
            w32(out, 0);          // creation_time
            w32(out, 0);          // modification_time
            w32(out, 90000);      // timescale
            w32(out, 0);          // duration = 0 (fragmented)
            w32(out, 0x00010000); // rate 1.0
            w16(out, 0x0100);     // volume 1.0
            w16(out, 0);
            wZeros(out, 8);
            writeUnityMatrix(out);
            wZeros(out, 24);
            w32(out, hasAudio_ ? 3 : 2); // next_track_ID
            mvhd.finish();
        }

        // Video Track (ID = 1)
        {
            BoxScope trak(out, "trak");
            {
                BoxScope tkhd(out, "tkhd");
                w8(out, 0);
                w24(out, 0x000003); // enabled | in_movie
                w32(out, 0);
                w32(out, 0);
                w32(out, 1);        // track_ID = 1
                w32(out, 0);
                w32(out, 0);        // duration = 0
                wZeros(out, 8);
                w16(out, 0);        // layer
                w16(out, 0);        // alternate_group
                w16(out, 0);        // volume = 0 for video
                w16(out, 0);
                writeUnityMatrix(out);
                w32(out, static_cast<uint32_t>(width_) << 16);
                w32(out, static_cast<uint32_t>(height_) << 16);
                tkhd.finish();
            }
            {
                BoxScope mdia(out, "mdia");
                {
                    BoxScope mdhd(out, "mdhd");
                    w32(out, 0);
                    w32(out, 0);
                    w32(out, 0);
                    w32(out, 90000); // video timescale = 90 kHz
                    w32(out, 0);
                    w16(out, 0x55C4); // "und"
                    w16(out, 0);
                    mdhd.finish();
                }
                {
                    BoxScope hdlr(out, "hdlr");
                    w32(out, 0);
                    w32(out, 0);
                    wFourCC(out, "vide");
                    wZeros(out, 12);
                    const char* name = "VideoHandler";
                    out.insert(out.end(), name, name + std::strlen(name) + 1);
                    hdlr.finish();
                }
                {
                    BoxScope minf(out, "minf");
                    {
                        BoxScope vmhd(out, "vmhd");
                        w8(out, 0);
                        w24(out, 0x000001);
                        w16(out, 0);
                        wZeros(out, 6);
                        vmhd.finish();
                    }
                    writeSelfContainedDinf(out);
                    {
                        BoxScope stbl(out, "stbl");
                        {
                            BoxScope stsd(out, "stsd");
                            w32(out, 0);
                            w32(out, 1); // entry_count = 1
                            {
                                BoxScope avc1(out, "avc1");
                                wZeros(out, 6);
                                w16(out, 1); // data_reference_index
                                wZeros(out, 16);
                                w16(out, static_cast<uint16_t>(width_));
                                w16(out, static_cast<uint16_t>(height_));
                                w32(out, 0x00480000);
                                w32(out, 0x00480000);
                                w32(out, 0);
                                w16(out, 1); // frame_count
                                wZeros(out, 32);
                                w16(out, 0x0018);
                                w16(out, 0xFFFF);
                                {
                                    BoxScope avcC(out, "avcC");
                                    uint8_t profile = sps_.size() > 1 ? sps_[1] : 0x64;
                                    uint8_t compat  = sps_.size() > 2 ? sps_[2] : 0x00;
                                    uint8_t level   = sps_.size() > 3 ? sps_[3] : 0x28;
                                    w8(out, 1);       // configurationVersion
                                    w8(out, profile);
                                    w8(out, compat);
                                    w8(out, level);
                                    w8(out, 0xFF);    // 4-byte NAL unit length
                                    w8(out, 0xE1);    // 1 SPS
                                    w16(out, static_cast<uint16_t>(sps_.size()));
                                    out.insert(out.end(), sps_.begin(), sps_.end());
                                    w8(out, 1);       // 1 PPS
                                    w16(out, static_cast<uint16_t>(pps_.size()));
                                    out.insert(out.end(), pps_.begin(), pps_.end());
                                    avcC.finish();
                                }
                                avc1.finish();
                            }
                            stsd.finish();
                        }
                        writeEmptySampleTables(out);
                        stbl.finish();
                    }
                    minf.finish();
                }
                mdia.finish();
            }
            trak.finish();
        }

        // Optional Audio Track (ID = 2)
        if (hasAudio_) {
            BoxScope trak(out, "trak");
            {
                BoxScope tkhd(out, "tkhd");
                w8(out, 0);
                w24(out, 0x000003);
                w32(out, 0);
                w32(out, 0);
                w32(out, 2); // track_ID = 2
                w32(out, 0);
                w32(out, 0);
                wZeros(out, 8);
                w16(out, 0);
                w16(out, 0);
                w16(out, 0x0100); // volume = 1.0
                w16(out, 0);
                writeUnityMatrix(out);
                w32(out, 0);
                w32(out, 0);
                tkhd.finish();
            }
            {
                BoxScope mdia(out, "mdia");
                {
                    BoxScope mdhd(out, "mdhd");
                    w32(out, 0);
                    w32(out, 0);
                    w32(out, 0);
                    w32(out, audioSampleRate_);
                    w32(out, 0);
                    w16(out, 0x55C4);
                    w16(out, 0);
                    mdhd.finish();
                }
                {
                    BoxScope hdlr(out, "hdlr");
                    w32(out, 0);
                    w32(out, 0);
                    wFourCC(out, "soun");
                    wZeros(out, 12);
                    const char* name = "SoundHandler";
                    out.insert(out.end(), name, name + std::strlen(name) + 1);
                    hdlr.finish();
                }
                {
                    BoxScope minf(out, "minf");
                    {
                        BoxScope smhd(out, "smhd");
                        w32(out, 0);
                        w16(out, 0);
                        w16(out, 0);
                        smhd.finish();
                    }
                    writeSelfContainedDinf(out);
                    {
                        BoxScope stbl(out, "stbl");
                        {
                            BoxScope stsd(out, "stsd");
                            w32(out, 0);
                            w32(out, 1);
                            {
                                BoxScope mp4a(out, "mp4a");
                                wZeros(out, 6);
                                w16(out, 1);
                                wZeros(out, 8);
                                w16(out, audioChannels_);
                                w16(out, 16);
                                w16(out, 0);
                                w16(out, 0);
                                w32(out, audioSampleRate_ << 16);
                                {
                                    BoxScope esds(out, "esds");
                                    w32(out, 0); // version/flags
                                    uint8_t ascLen = static_cast<uint8_t>(aacAsc_.size());
                                    uint8_t decCfgLen = static_cast<uint8_t>(13 + 2 + ascLen);
                                    uint8_t esDescLen = static_cast<uint8_t>(3 + 2 + decCfgLen + 3);
                                    // ES_Descriptor (tag 0x03)
                                    w8(out, 0x03);
                                    w8(out, esDescLen);
                                    w16(out, 2); // ES_ID
                                    w8(out, 0);  // flags
                                    // DecoderConfigDescriptor (tag 0x04)
                                    w8(out, 0x04);
                                    w8(out, decCfgLen);
                                    w8(out, 0x40); // Audio ISO/IEC 14496-3 (AAC)
                                    w8(out, 0x15); // audioStream
                                    w24(out, 0);   // bufferSizeDB
                                    w32(out, 128000);
                                    w32(out, 128000);
                                    // DecoderSpecificInfo (tag 0x05)
                                    w8(out, 0x05);
                                    w8(out, ascLen);
                                    out.insert(out.end(), aacAsc_.begin(), aacAsc_.end());
                                    // SLConfigDescriptor (tag 0x06)
                                    w8(out, 0x06);
                                    w8(out, 1);
                                    w8(out, 0x02);
                                    esds.finish();
                                }
                                mp4a.finish();
                            }
                            stsd.finish();
                        }
                        writeEmptySampleTables(out);
                        stbl.finish();
                    }
                    minf.finish();
                }
                mdia.finish();
            }
            trak.finish();
        }

        // mvex (Movie Extends)
        {
            BoxScope mvex(out, "mvex");
            {
                BoxScope trex1(out, "trex");
                w32(out, 0);
                w32(out, 1); // track_ID = 1
                w32(out, 1); // default_sample_description_index
                w32(out, 3000);
                w32(out, 0);
                w32(out, 0x01010000);
                trex1.finish();
            }
            if (hasAudio_) {
                BoxScope trex2(out, "trex");
                w32(out, 0);
                w32(out, 2); // track_ID = 2
                w32(out, 1);
                w32(out, 1024);
                w32(out, 0);
                w32(out, 0x02000000);
                trex2.finish();
            }
            mvex.finish();
        }

        moov.finish();
    }

    return out;
}

std::vector<uint8_t> Fmp4SegmentMuxer::buildMediaFragmentBytes(uint32_t sequenceNumber) {
    size_t videoBytes = 0;
    for (const auto& v : pendingVideo_) videoBytes += v.data.size();
    size_t audioBytes = 0;
    for (const auto& a : pendingAudio_) audioBytes += a.data.size();

    std::vector<uint8_t> out;
    out.reserve(512 + videoBytes + audioBytes);

    size_t moofStart = out.size();
    size_t videoDataOffsetPatchPos = 0;
    size_t audioDataOffsetPatchPos = 0;
    bool includeAudioTraf = hasAudio_ && !pendingAudio_.empty();

    {
        BoxScope moof(out, "moof");
        {
            BoxScope mfhd(out, "mfhd");
            w32(out, 0);
            w32(out, sequenceNumber);
            mfhd.finish();
        }

        // Video Track Fragment (Track 1)
        {
            BoxScope traf(out, "traf");
            {
                BoxScope tfhd(out, "tfhd");
                w8(out, 0);
                w24(out, 0x020020); // default-base-is-moof | default-sample-flags-present
                w32(out, 1);        // track_ID = 1
                w32(out, 0x01010000);
                tfhd.finish();
            }
            {
                BoxScope tfdt(out, "tfdt");
                w8(out, 1); // 64-bit baseMediaDecodeTime
                w24(out, 0);
                w64(out, fragmentBaseVideoPts90k_);
                tfdt.finish();
            }
            {
                BoxScope trun(out, "trun");
                w8(out, 0);
                w24(out, 0x000701); // data-offset | duration | size | sample-flags
                w32(out, static_cast<uint32_t>(pendingVideo_.size()));
                videoDataOffsetPatchPos = out.size();
                w32(out, 0); // patched once moof size is finalized
                for (const auto& v : pendingVideo_) {
                    w32(out, v.duration90k);
                    w32(out, static_cast<uint32_t>(v.data.size()));
                    w32(out, v.isKeyframe ? 0x02000000 : 0x01010000);
                }
                trun.finish();
            }
            traf.finish();
        }

        // Audio Track Fragment (Track 2)
        if (includeAudioTraf) {
            BoxScope traf(out, "traf");
            {
                BoxScope tfhd(out, "tfhd");
                w8(out, 0);
                w24(out, 0x020020);
                w32(out, 2); // track_ID = 2
                w32(out, 0x02000000);
                tfhd.finish();
            }
            {
                BoxScope tfdt(out, "tfdt");
                w8(out, 1);
                w24(out, 0);
                w64(out, nextAudioDecodeSamples_);
                tfdt.finish();
            }
            {
                BoxScope trun(out, "trun");
                w8(out, 0);
                w24(out, 0x000301); // data-offset | duration | size
                w32(out, static_cast<uint32_t>(pendingAudio_.size()));
                audioDataOffsetPatchPos = out.size();
                w32(out, 0);
                for (const auto& a : pendingAudio_) {
                    w32(out, a.durationSamples);
                    w32(out, static_cast<uint32_t>(a.data.size()));
                }
                trun.finish();
            }
            traf.finish();
        }

        moof.finish();
    }

    uint32_t moofSize = static_cast<uint32_t>(out.size() - moofStart);
    uint32_t videoPayloadStartFromMoof = moofSize + 8;
    patch32(out, videoDataOffsetPatchPos, videoPayloadStartFromMoof);
    if (includeAudioTraf) {
        uint32_t audioPayloadStartFromMoof = videoPayloadStartFromMoof + static_cast<uint32_t>(videoBytes);
        patch32(out, audioDataOffsetPatchPos, audioPayloadStartFromMoof);
    }

    // mdat
    uint32_t mdatSize = static_cast<uint32_t>(8 + videoBytes + audioBytes);
    w32(out, mdatSize);
    wFourCC(out, "mdat");
    for (const auto& v : pendingVideo_) {
        out.insert(out.end(), v.data.begin(), v.data.end());
    }
    if (includeAudioTraf) {
        for (const auto& a : pendingAudio_) {
            out.insert(out.end(), a.data.begin(), a.data.end());
        }
    }

    return out;
}

} // namespace maxwatch
