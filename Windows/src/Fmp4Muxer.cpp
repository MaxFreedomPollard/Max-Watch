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
                                    uint8_t profile = sps_.size() > 1 ? sps_[1] : 0x4D;
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
                                    if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
                                        w8(out, 0xFD); // 4:2:0 chroma_format
                                        w8(out, 0xF8); // 8-bit luma
                                        w8(out, 0xF8); // 8-bit chroma
                                        w8(out, 0x00); // 0 SPS Ext
                                    }
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

namespace {

uint32_t r24be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

uint32_t r32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t r64be(const uint8_t* p) {
    return (static_cast<uint64_t>(r32be(p)) << 32) | static_cast<uint64_t>(r32be(p + 4));
}

void patch64be(std::vector<uint8_t>& b, size_t offset, uint64_t v) {
    for (int s = 56, idx = 0; s >= 0; s -= 8, ++idx) {
        b[offset + static_cast<size_t>(idx)] = static_cast<uint8_t>((v >> s) & 0xFF);
    }
}

bool nextIsoBox(
    const uint8_t* buf,
    size_t len,
    size_t& cursor,
    size_t& boxStart,
    size_t& payloadStart,
    size_t& boxEnd,
    char outType[5]
) {
    if (cursor + 8 > len) return false;
    uint32_t sz32 = r32be(buf + cursor);
    std::memcpy(outType, buf + cursor + 4, 4);
    outType[4] = '\0';

    uint64_t totalSz = sz32;
    size_t hdrLen = 8;
    if (sz32 == 1) {
        if (cursor + 16 > len) return false;
        totalSz = r64be(buf + cursor + 8);
        hdrLen = 16;
    } else if (sz32 == 0) {
        totalSz = len - cursor;
    }
    if (totalSz < hdrLen || cursor + totalSz > len) {
        return false;
    }
    boxStart = cursor;
    payloadStart = cursor + hdrLen;
    boxEnd = cursor + static_cast<size_t>(totalSz);
    cursor = boxEnd;
    return true;
}

struct ParsedTrexDefaults {
    uint32_t trackId = 0;
    uint32_t defaultDuration = 0;
    uint32_t defaultSize = 0;
    uint32_t defaultFlags = 0;
};

struct IndexedMp4Track {
    bool present = false;
    bool isVideo = false;
    uint32_t trackId = 0;
    uint32_t timescale = 90000;
    uint32_t widthFixed16 = 0;
    uint32_t heightFixed16 = 0;
    std::vector<uint8_t> stsdBox;

    uint32_t trexDefaultDuration = 0;
    uint32_t trexDefaultSize = 0;
    uint32_t trexDefaultFlags = 0;

    std::vector<uint32_t> sampleDurations;
    std::vector<uint32_t> sampleSizes;
    std::vector<int32_t> sampleCto;
    bool hasNonZeroCto = false;
    std::vector<uint32_t> syncSamples1Based;

    std::vector<uint64_t> chunkOffsetsInMedia;
    std::vector<uint32_t> chunkSampleCounts;
    uint64_t totalDurationTicks = 0;
};

void writeSttsFromDurations(std::vector<uint8_t>& stbl, const std::vector<uint32_t>& durs) {
    struct Entry { uint32_t count; uint32_t delta; };
    std::vector<Entry> rle;
    for (uint32_t d : durs) {
        if (!rle.empty() && rle.back().delta == d) {
            rle.back().count++;
        } else {
            rle.push_back({1, d});
        }
    }
    BoxScope stts(stbl, "stts");
    w32(stbl, 0);
    w32(stbl, static_cast<uint32_t>(rle.size()));
    for (const auto& e : rle) {
        w32(stbl, e.count);
        w32(stbl, e.delta);
    }
    stts.finish();
}

void writeCttsFromOffsets(std::vector<uint8_t>& stbl, const std::vector<int32_t>& ctos) {
    struct Entry { uint32_t count; int32_t offset; };
    std::vector<Entry> rle;
    for (int32_t c : ctos) {
        if (!rle.empty() && rle.back().offset == c) {
            rle.back().count++;
        } else {
            rle.push_back({1, c});
        }
    }
    BoxScope ctts(stbl, "ctts");
    w8(stbl, 1); // version 1 supports signed composition offsets
    w24(stbl, 0);
    w32(stbl, static_cast<uint32_t>(rle.size()));
    for (const auto& e : rle) {
        w32(stbl, e.count);
        w32(stbl, static_cast<uint32_t>(e.offset));
    }
    ctts.finish();
}

void writeStssBox(std::vector<uint8_t>& stbl, const std::vector<uint32_t>& syncs) {
    BoxScope stss(stbl, "stss");
    w32(stbl, 0);
    w32(stbl, static_cast<uint32_t>(syncs.size()));
    for (uint32_t s : syncs) {
        w32(stbl, s);
    }
    stss.finish();
}

void writeStscFromChunkCounts(std::vector<uint8_t>& stbl, const std::vector<uint32_t>& counts) {
    struct Entry { uint32_t firstChunk1Based; uint32_t samplesPerChunk; };
    std::vector<Entry> rle;
    for (size_t i = 0; i < counts.size(); ++i) {
        uint32_t spc = counts[i];
        if (rle.empty() || rle.back().samplesPerChunk != spc) {
            rle.push_back({static_cast<uint32_t>(i + 1), spc});
        }
    }
    BoxScope stsc(stbl, "stsc");
    w32(stbl, 0);
    w32(stbl, static_cast<uint32_t>(rle.size()));
    for (const auto& e : rle) {
        w32(stbl, e.firstChunk1Based);
        w32(stbl, e.samplesPerChunk);
        w32(stbl, 1); // sample_description_index = 1
    }
    stsc.finish();
}

void writeStszFromSizes(std::vector<uint8_t>& stbl, const std::vector<uint32_t>& sizes) {
    BoxScope stsz(stbl, "stsz");
    w32(stbl, 0);
    w32(stbl, 0); // variable sample sizes
    w32(stbl, static_cast<uint32_t>(sizes.size()));
    for (uint32_t s : sizes) {
        w32(stbl, s);
    }
    stsz.finish();
}

size_t writeStcoPlaceholder(std::vector<uint8_t>& stbl, const std::vector<uint64_t>& mediaOffsets) {
    BoxScope stco(stbl, "stco");
    w32(stbl, 0);
    w32(stbl, static_cast<uint32_t>(mediaOffsets.size()));
    size_t entriesStart = stbl.size();
    for (uint64_t off : mediaOffsets) {
        w32(stbl, static_cast<uint32_t>(off & 0xFFFFFFFFULL));
    }
    stco.finish();
    return entriesStart;
}

size_t writeCo64Placeholder(std::vector<uint8_t>& stbl, const std::vector<uint64_t>& mediaOffsets) {
    BoxScope co64(stbl, "co64");
    w32(stbl, 0);
    w32(stbl, static_cast<uint32_t>(mediaOffsets.size()));
    size_t entriesStart = stbl.size();
    for (uint64_t off : mediaOffsets) {
        w64(stbl, off);
    }
    co64.finish();
    return entriesStart;
}

} // namespace

VirtualStandardMp4Layout buildVirtualStandardMp4Layout(
    const uint8_t* initBytes,
    size_t initLen,
    size_t mediaFragmentCount,
    const std::function<std::vector<uint8_t>(size_t fragmentIndex)>& readMediaFragment
) {
    VirtualStandardMp4Layout layout;
    if (!initBytes || initLen < 24 || mediaFragmentCount == 0 || !readMediaFragment) {
        return layout;
    }

    IndexedMp4Track vidTrack;
    IndexedMp4Track audTrack;
    std::vector<ParsedTrexDefaults> trexList;

    // 1. Parse `moov` inside `initBytes`
    size_t topCur = 0;
    size_t bStart = 0, pStart = 0, bEnd = 0;
    char bType[5] = {0};
    while (nextIsoBox(initBytes, initLen, topCur, bStart, pStart, bEnd, bType)) {
        if (std::strcmp(bType, "moov") != 0) continue;

        size_t moovCur = pStart;
        size_t mbStart = 0, mpStart = 0, mbEnd = 0;
        char mbType[5] = {0};
        while (nextIsoBox(initBytes, bEnd, moovCur, mbStart, mpStart, mbEnd, mbType)) {
            if (std::strcmp(mbType, "trak") == 0) {
                uint32_t trackId = 0;
                uint32_t widthFixed = 0, heightFixed = 0;
                uint32_t timescale = 0;
                char handler[5] = {0};
                std::vector<uint8_t> stsdVerbatim;

                size_t tCur = mpStart;
                size_t tbStart = 0, tpStart = 0, tbEnd = 0;
                char tbType[5] = {0};
                while (nextIsoBox(initBytes, mbEnd, tCur, tbStart, tpStart, tbEnd, tbType)) {
                    if (std::strcmp(tbType, "tkhd") == 0 && tpStart + 4 <= tbEnd) {
                        uint8_t ver = initBytes[tpStart];
                        if (ver == 0 && tpStart + 84 <= tbEnd) {
                            trackId = r32be(initBytes + tpStart + 12);
                            widthFixed = r32be(initBytes + tpStart + 76);
                            heightFixed = r32be(initBytes + tpStart + 80);
                        } else if (ver == 1 && tpStart + 96 <= tbEnd) {
                            trackId = r32be(initBytes + tpStart + 20);
                            widthFixed = r32be(initBytes + tpStart + 88);
                            heightFixed = r32be(initBytes + tpStart + 92);
                        }
                    } else if (std::strcmp(tbType, "mdia") == 0) {
                        size_t mdCur = tpStart;
                        size_t mdbStart = 0, mdpStart = 0, mdbEnd = 0;
                        char mdbType[5] = {0};
                        while (nextIsoBox(initBytes, tbEnd, mdCur, mdbStart, mdpStart, mdbEnd, mdbType)) {
                            if (std::strcmp(mdbType, "mdhd") == 0 && mdpStart + 4 <= mdbEnd) {
                                uint8_t ver = initBytes[mdpStart];
                                if (ver == 0 && mdpStart + 16 <= mdbEnd) {
                                    timescale = r32be(initBytes + mdpStart + 12);
                                } else if (ver == 1 && mdpStart + 24 <= mdbEnd) {
                                    timescale = r32be(initBytes + mdpStart + 20);
                                }
                            } else if (std::strcmp(mdbType, "hdlr") == 0 && mdpStart + 12 <= mdbEnd) {
                                std::memcpy(handler, initBytes + mdpStart + 8, 4);
                                handler[4] = '\0';
                            } else if (std::strcmp(mdbType, "minf") == 0) {
                                size_t miCur = mdpStart;
                                size_t mibStart = 0, mipStart = 0, mibEnd = 0;
                                char mibType[5] = {0};
                                while (nextIsoBox(initBytes, mdbEnd, miCur, mibStart, mipStart, mibEnd, mibType)) {
                                    if (std::strcmp(mibType, "stbl") == 0) {
                                        size_t stCur = mipStart;
                                        size_t sbStart = 0, spStart = 0, sbEnd = 0;
                                        char sbType[5] = {0};
                                        while (nextIsoBox(initBytes, mibEnd, stCur, sbStart, spStart, sbEnd, sbType)) {
                                            if (std::strcmp(sbType, "stsd") == 0) {
                                                stsdVerbatim.assign(initBytes + sbStart, initBytes + sbEnd);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (trackId != 0 && !stsdVerbatim.empty()) {
                    if (std::strcmp(handler, "vide") == 0 && !vidTrack.present) {
                        vidTrack.present = true;
                        vidTrack.isVideo = true;
                        vidTrack.trackId = trackId;
                        vidTrack.timescale = timescale > 0 ? timescale : 90000;
                        vidTrack.widthFixed16 = widthFixed;
                        vidTrack.heightFixed16 = heightFixed;
                        vidTrack.stsdBox = std::move(stsdVerbatim);
                    } else if (std::strcmp(handler, "soun") == 0 && !audTrack.present) {
                        audTrack.present = true;
                        audTrack.isVideo = false;
                        audTrack.trackId = trackId;
                        audTrack.timescale = timescale > 0 ? timescale : 48000;
                        audTrack.stsdBox = std::move(stsdVerbatim);
                    }
                }
            } else if (std::strcmp(mbType, "mvex") == 0) {
                size_t mvCur = mpStart;
                size_t xStart = 0, xpStart = 0, xEnd = 0;
                char xType[5] = {0};
                while (nextIsoBox(initBytes, mbEnd, mvCur, xStart, xpStart, xEnd, xType)) {
                    if (std::strcmp(xType, "trex") == 0 && xpStart + 24 <= xEnd) {
                        ParsedTrexDefaults td;
                        td.trackId = r32be(initBytes + xpStart + 4);
                        td.defaultDuration = r32be(initBytes + xpStart + 12);
                        td.defaultSize = r32be(initBytes + xpStart + 16);
                        td.defaultFlags = r32be(initBytes + xpStart + 20);
                        trexList.push_back(td);
                    }
                }
            }
        }
    }

    if (!vidTrack.present) {
        return layout;
    }

    for (const auto& td : trexList) {
        if (td.trackId == vidTrack.trackId) {
            vidTrack.trexDefaultDuration = td.defaultDuration;
            vidTrack.trexDefaultSize = td.defaultSize;
            vidTrack.trexDefaultFlags = td.defaultFlags;
        } else if (audTrack.present && td.trackId == audTrack.trackId) {
            audTrack.trexDefaultDuration = td.defaultDuration;
            audTrack.trexDefaultSize = td.defaultSize;
            audTrack.trexDefaultFlags = td.defaultFlags;
        }
    }

    // 2. Scan each media fragment (`moof + mdat`) and record exact sample & chunk offsets
    uint64_t fragOffsetInMedia = 0;
    for (size_t fIdx = 0; fIdx < mediaFragmentCount; ++fIdx) {
        std::vector<uint8_t> frag = readMediaFragment(fIdx);
        if (frag.empty()) break;

        size_t fCur = 0;
        size_t fbStart = 0, fpStart = 0, fbEnd = 0;
        char fbType[5] = {0};
        while (nextIsoBox(frag.data(), frag.size(), fCur, fbStart, fpStart, fbEnd, fbType)) {
            if (std::strcmp(fbType, "moof") == 0 ||
                std::strcmp(fbType, "sidx") == 0 ||
                std::strcmp(fbType, "styp") == 0) {
                layout.freeFourccMediaOffsets.push_back(fragOffsetInMedia + fbStart + 4);
            }
            if (std::strcmp(fbType, "moof") != 0) {
                continue;
            }

            size_t moofCur = fpStart;
            size_t trStart = 0, trpStart = 0, trEnd = 0;
            char trType[5] = {0};
            while (nextIsoBox(frag.data(), fbEnd, moofCur, trStart, trpStart, trEnd, trType)) {
                if (std::strcmp(trType, "traf") != 0) continue;

                // Pass 1 inside `traf`: locate `tfhd`
                bool foundTfhd = false;
                uint32_t tfhdTrackId = 0;
                uint32_t tfhdFlags = 0;
                uint64_t explicitBaseOffset = 0;
                uint32_t defDur = 0, defSize = 0, defFlags = 0;

                size_t c1 = trpStart;
                size_t s1 = 0, p1 = 0, e1 = 0;
                char t1[5] = {0};
                while (nextIsoBox(frag.data(), trEnd, c1, s1, p1, e1, t1)) {
                    if (std::strcmp(t1, "tfhd") == 0 && p1 + 8 <= e1) {
                        foundTfhd = true;
                        tfhdFlags = r24be(frag.data() + p1 + 1);
                        tfhdTrackId = r32be(frag.data() + p1 + 4);

                        IndexedMp4Track* trk = nullptr;
                        if (tfhdTrackId == vidTrack.trackId) trk = &vidTrack;
                        else if (audTrack.present && tfhdTrackId == audTrack.trackId) trk = &audTrack;

                        if (trk) {
                            defDur = trk->trexDefaultDuration;
                            defSize = trk->trexDefaultSize;
                            defFlags = trk->trexDefaultFlags;
                        }

                        size_t off = p1 + 8;
                        if ((tfhdFlags & 0x000001) && off + 8 <= e1) {
                            explicitBaseOffset = r64be(frag.data() + off);
                            off += 8;
                        }
                        if ((tfhdFlags & 0x000002) && off + 4 <= e1) {
                            off += 4;
                        }
                        if ((tfhdFlags & 0x000008) && off + 4 <= e1) {
                            defDur = r32be(frag.data() + off);
                            off += 4;
                        }
                        if ((tfhdFlags & 0x000010) && off + 4 <= e1) {
                            defSize = r32be(frag.data() + off);
                            off += 4;
                        }
                        if ((tfhdFlags & 0x000020) && off + 4 <= e1) {
                            defFlags = r32be(frag.data() + off);
                            off += 4;
                        }
                        break;
                    }
                }
                if (!foundTfhd) continue;

                IndexedMp4Track* targetTrack = nullptr;
                if (tfhdTrackId == vidTrack.trackId) targetTrack = &vidTrack;
                else if (audTrack.present && tfhdTrackId == audTrack.trackId) targetTrack = &audTrack;
                if (!targetTrack) continue;

                int64_t trafBaseInMedia = static_cast<int64_t>(fragOffsetInMedia + fbStart);
                if (tfhdFlags & 0x000001) {
                    trafBaseInMedia = static_cast<int64_t>(explicitBaseOffset) - static_cast<int64_t>(initLen);
                }
                int64_t runningPosInMedia = trafBaseInMedia;

                // Pass 2 inside `traf`: iterate `trun` boxes
                size_t c2 = trpStart;
                size_t s2 = 0, p2 = 0, e2 = 0;
                char t2[5] = {0};
                while (nextIsoBox(frag.data(), trEnd, c2, s2, p2, e2, t2)) {
                    if (std::strcmp(t2, "trun") != 0 || p2 + 8 > e2) continue;
                    uint8_t trunVer = frag[p2];
                    uint32_t trunFlags = r24be(frag.data() + p2 + 1);
                    uint32_t sampleCount = r32be(frag.data() + p2 + 4);
                    size_t off = p2 + 8;

                    if ((trunFlags & 0x000001) && off + 4 <= e2) {
                        int32_t dataOff = static_cast<int32_t>(r32be(frag.data() + off));
                        off += 4;
                        runningPosInMedia = trafBaseInMedia + dataOff;
                    }
                    uint32_t firstFlags = defFlags;
                    if ((trunFlags & 0x000004) && off + 4 <= e2) {
                        firstFlags = r32be(frag.data() + off);
                        off += 4;
                    }

                    if (sampleCount == 0 || runningPosInMedia < 0) continue;

                    uint64_t chunkStartMedia = static_cast<uint64_t>(runningPosInMedia);
                    uint32_t validSamplesInRun = 0;
                    for (uint32_t i = 0; i < sampleCount; ++i) {
                        uint32_t sDur = defDur;
                        if (trunFlags & 0x000100) {
                            if (off + 4 > e2) break;
                            sDur = r32be(frag.data() + off);
                            off += 4;
                        }
                        uint32_t sSize = defSize;
                        if (trunFlags & 0x000200) {
                            if (off + 4 > e2) break;
                            sSize = r32be(frag.data() + off);
                            off += 4;
                        }
                        uint32_t sFlags = (i == 0 && (trunFlags & 0x000004)) ? firstFlags : defFlags;
                        if (trunFlags & 0x000400) {
                            if (off + 4 > e2) break;
                            sFlags = r32be(frag.data() + off);
                            off += 4;
                        }
                        int32_t sCto = 0;
                        if (trunFlags & 0x000800) {
                            if (off + 4 > e2) break;
                            uint32_t rawCto = r32be(frag.data() + off);
                            off += 4;
                            sCto = (trunVer == 1) ? static_cast<int32_t>(rawCto) : static_cast<int32_t>(rawCto);
                        }

                        if (sDur == 0) sDur = targetTrack->isVideo ? 3000U : 1024U;
                        if (runningPosInMedia + static_cast<int64_t>(sSize) > static_cast<int64_t>(fragOffsetInMedia + frag.size())) {
                            break;
                        }

                        targetTrack->sampleDurations.push_back(sDur);
                        targetTrack->sampleSizes.push_back(sSize);
                        targetTrack->totalDurationTicks += sDur;

                        if (targetTrack->isVideo) {
                            bool isSync = ((sFlags & 0x00010000U) == 0) || targetTrack->sampleSizes.size() == 1;
                            if (isSync) {
                                targetTrack->syncSamples1Based.push_back(static_cast<uint32_t>(targetTrack->sampleSizes.size()));
                            }
                            targetTrack->sampleCto.push_back(sCto);
                            if (sCto != 0) targetTrack->hasNonZeroCto = true;
                        }

                        runningPosInMedia += static_cast<int64_t>(sSize);
                        validSamplesInRun++;
                    }

                    if (validSamplesInRun > 0) {
                        targetTrack->chunkOffsetsInMedia.push_back(chunkStartMedia);
                        targetTrack->chunkSampleCounts.push_back(validSamplesInRun);
                    }
                }
            }
        }

        fragOffsetInMedia += frag.size();
        std::fill(frag.begin(), frag.end(), uint8_t(0));
    }

    if (vidTrack.sampleSizes.empty() || vidTrack.chunkOffsetsInMedia.empty()) {
        return layout;
    }

    // 3. Synthesize standard unfragmented `ftyp + moov` header
    constexpr uint32_t kMovieTimescale = 90000;
    uint64_t vidMovieDur = (vidTrack.totalDurationTicks * kMovieTimescale) / std::max<uint32_t>(1, vidTrack.timescale);
    uint64_t audMovieDur = (audTrack.present && !audTrack.sampleSizes.empty())
        ? ((audTrack.totalDurationTicks * kMovieTimescale) / std::max<uint32_t>(1, audTrack.timescale))
        : 0;
    uint64_t movieDur = vidMovieDur > 0 ? vidMovieDur : audMovieDur;
    bool includeAudio = audTrack.present && !audTrack.sampleSizes.empty() && !audTrack.chunkOffsetsInMedia.empty();
    uint32_t nextTrackId = std::max(vidTrack.trackId, includeAudio ? audTrack.trackId : 1U) + 1U;

    std::vector<uint8_t> hdr;
    hdr.reserve(2048 + (vidTrack.sampleSizes.size() + audTrack.sampleSizes.size()) * 8);

    // ftyp
    {
        BoxScope ftyp(hdr, "ftyp");
        wFourCC(hdr, "isom");
        w32(hdr, 0x00000200);
        wFourCC(hdr, "isom");
        wFourCC(hdr, "iso2");
        wFourCC(hdr, "avc1");
        wFourCC(hdr, "mp41");
        ftyp.finish();
    }

    const bool use64BitOffsets = (fragOffsetInMedia >= 0x70000000ULL);
    size_t vidChunkEntriesPos = 0;
    size_t audChunkEntriesPos = 0;

    // moov
    {
        BoxScope moov(hdr, "moov");

        // mvhd
        {
            BoxScope mvhd(hdr, "mvhd");
            if (movieDur > 0xFFFFFFFFULL) {
                w8(hdr, 1);
                w24(hdr, 0);
                w64(hdr, 0);
                w64(hdr, 0);
                w32(hdr, kMovieTimescale);
                w64(hdr, movieDur);
            } else {
                w8(hdr, 0);
                w24(hdr, 0);
                w32(hdr, 0);
                w32(hdr, 0);
                w32(hdr, kMovieTimescale);
                w32(hdr, static_cast<uint32_t>(movieDur));
            }
            w32(hdr, 0x00010000);
            w16(hdr, 0x0100);
            w16(hdr, 0);
            wZeros(hdr, 8);
            writeUnityMatrix(hdr);
            wZeros(hdr, 24);
            w32(hdr, nextTrackId);
            mvhd.finish();
        }

        // Video trak
        {
            BoxScope trak(hdr, "trak");
            {
                BoxScope tkhd(hdr, "tkhd");
                if (vidMovieDur > 0xFFFFFFFFULL) {
                    w8(hdr, 1);
                    w24(hdr, 0x000003);
                    w64(hdr, 0);
                    w64(hdr, 0);
                    w32(hdr, vidTrack.trackId);
                    w32(hdr, 0);
                    w64(hdr, vidMovieDur);
                } else {
                    w8(hdr, 0);
                    w24(hdr, 0x000003);
                    w32(hdr, 0);
                    w32(hdr, 0);
                    w32(hdr, vidTrack.trackId);
                    w32(hdr, 0);
                    w32(hdr, static_cast<uint32_t>(vidMovieDur));
                }
                wZeros(hdr, 8);
                w16(hdr, 0);
                w16(hdr, 0);
                w16(hdr, 0);
                w16(hdr, 0);
                writeUnityMatrix(hdr);
                w32(hdr, vidTrack.widthFixed16);
                w32(hdr, vidTrack.heightFixed16);
                tkhd.finish();
            }
            {
                BoxScope mdia(hdr, "mdia");
                {
                    BoxScope mdhd(hdr, "mdhd");
                    if (vidTrack.totalDurationTicks > 0xFFFFFFFFULL) {
                        w8(hdr, 1);
                        w24(hdr, 0);
                        w64(hdr, 0);
                        w64(hdr, 0);
                        w32(hdr, vidTrack.timescale);
                        w64(hdr, vidTrack.totalDurationTicks);
                    } else {
                        w8(hdr, 0);
                        w24(hdr, 0);
                        w32(hdr, 0);
                        w32(hdr, 0);
                        w32(hdr, vidTrack.timescale);
                        w32(hdr, static_cast<uint32_t>(vidTrack.totalDurationTicks));
                    }
                    w16(hdr, 0x55C4);
                    w16(hdr, 0);
                    mdhd.finish();
                }
                {
                    BoxScope hdlr(hdr, "hdlr");
                    w32(hdr, 0);
                    w32(hdr, 0);
                    wFourCC(hdr, "vide");
                    wZeros(hdr, 12);
                    const char* name = "VideoHandler";
                    hdr.insert(hdr.end(), name, name + std::strlen(name) + 1);
                    hdlr.finish();
                }
                {
                    BoxScope minf(hdr, "minf");
                    {
                        BoxScope vmhd(hdr, "vmhd");
                        w8(hdr, 0);
                        w24(hdr, 0x000001);
                        w16(hdr, 0);
                        wZeros(hdr, 6);
                        vmhd.finish();
                    }
                    writeSelfContainedDinf(hdr);
                    {
                        BoxScope stbl(hdr, "stbl");
                        hdr.insert(hdr.end(), vidTrack.stsdBox.begin(), vidTrack.stsdBox.end());
                        writeSttsFromDurations(hdr, vidTrack.sampleDurations);
                        if (vidTrack.hasNonZeroCto) {
                            writeCttsFromOffsets(hdr, vidTrack.sampleCto);
                        }
                        writeStssBox(hdr, vidTrack.syncSamples1Based);
                        writeStscFromChunkCounts(hdr, vidTrack.chunkSampleCounts);
                        writeStszFromSizes(hdr, vidTrack.sampleSizes);
                        vidChunkEntriesPos = use64BitOffsets
                            ? writeCo64Placeholder(hdr, vidTrack.chunkOffsetsInMedia)
                            : writeStcoPlaceholder(hdr, vidTrack.chunkOffsetsInMedia);
                        stbl.finish();
                    }
                    minf.finish();
                }
                mdia.finish();
            }
            trak.finish();
        }

        // Optional Audio trak
        if (includeAudio) {
            BoxScope trak(hdr, "trak");
            {
                BoxScope tkhd(hdr, "tkhd");
                if (audMovieDur > 0xFFFFFFFFULL) {
                    w8(hdr, 1);
                    w24(hdr, 0x000003);
                    w64(hdr, 0);
                    w64(hdr, 0);
                    w32(hdr, audTrack.trackId);
                    w32(hdr, 0);
                    w64(hdr, audMovieDur);
                } else {
                    w8(hdr, 0);
                    w24(hdr, 0x000003);
                    w32(hdr, 0);
                    w32(hdr, 0);
                    w32(hdr, audTrack.trackId);
                    w32(hdr, 0);
                    w32(hdr, static_cast<uint32_t>(audMovieDur));
                }
                wZeros(hdr, 8);
                w16(hdr, 0);
                w16(hdr, 0);
                w16(hdr, 0x0100);
                w16(hdr, 0);
                writeUnityMatrix(hdr);
                w32(hdr, 0);
                w32(hdr, 0);
                tkhd.finish();
            }
            {
                BoxScope mdia(hdr, "mdia");
                {
                    BoxScope mdhd(hdr, "mdhd");
                    if (audTrack.totalDurationTicks > 0xFFFFFFFFULL) {
                        w8(hdr, 1);
                        w24(hdr, 0);
                        w64(hdr, 0);
                        w64(hdr, 0);
                        w32(hdr, audTrack.timescale);
                        w64(hdr, audTrack.totalDurationTicks);
                    } else {
                        w8(hdr, 0);
                        w24(hdr, 0);
                        w32(hdr, 0);
                        w32(hdr, 0);
                        w32(hdr, audTrack.timescale);
                        w32(hdr, static_cast<uint32_t>(audTrack.totalDurationTicks));
                    }
                    w16(hdr, 0x55C4);
                    w16(hdr, 0);
                    mdhd.finish();
                }
                {
                    BoxScope hdlr(hdr, "hdlr");
                    w32(hdr, 0);
                    w32(hdr, 0);
                    wFourCC(hdr, "soun");
                    wZeros(hdr, 12);
                    const char* name = "SoundHandler";
                    hdr.insert(hdr.end(), name, name + std::strlen(name) + 1);
                    hdlr.finish();
                }
                {
                    BoxScope minf(hdr, "minf");
                    {
                        BoxScope smhd(hdr, "smhd");
                        w32(hdr, 0);
                        w16(hdr, 0);
                        w16(hdr, 0);
                        smhd.finish();
                    }
                    writeSelfContainedDinf(hdr);
                    {
                        BoxScope stbl(hdr, "stbl");
                        hdr.insert(hdr.end(), audTrack.stsdBox.begin(), audTrack.stsdBox.end());
                        writeSttsFromDurations(hdr, audTrack.sampleDurations);
                        writeStscFromChunkCounts(hdr, audTrack.chunkSampleCounts);
                        writeStszFromSizes(hdr, audTrack.sampleSizes);
                        audChunkEntriesPos = use64BitOffsets
                            ? writeCo64Placeholder(hdr, audTrack.chunkOffsetsInMedia)
                            : writeStcoPlaceholder(hdr, audTrack.chunkOffsetsInMedia);
                        stbl.finish();
                    }
                    minf.finish();
                }
                mdia.finish();
            }
            trak.finish();
        }

        moov.finish();
    }

    // 4. Emit ONE unified top-level `mdat` box header spanning 100% of `fragOffsetInMedia`
    //    so strict demuxers (Windows Media Foundation `mfmp4srcsnk.dll`, QuickTime, VLC)
    //    see a textbook [ftyp][moov][mdat] unfragmented MP4 layout!
    if (8ULL + fragOffsetInMedia <= 0xFFFFFFFFULL && !use64BitOffsets) {
        w32(hdr, static_cast<uint32_t>(8ULL + fragOffsetInMedia));
        wFourCC(hdr, "mdat");
    } else {
        w32(hdr, 1); // 64-bit ISO BMFF largesize box
        wFourCC(hdr, "mdat");
        w64(hdr, 16ULL + fragOffsetInMedia);
    }

    // 5. Patch exact file offsets into `stco` / `co64` now that `hdr.size()` (including `mdat` header) is finalized!
    uint64_t finalHdrSize = static_cast<uint64_t>(hdr.size());
    for (size_t i = 0; i < vidTrack.chunkOffsetsInMedia.size(); ++i) {
        uint64_t absOff = finalHdrSize + vidTrack.chunkOffsetsInMedia[i];
        if (use64BitOffsets) {
            patch64be(hdr, vidChunkEntriesPos + i * 8, absOff);
        } else {
            patch32(hdr, vidChunkEntriesPos + i * 4, static_cast<uint32_t>(absOff));
        }
    }
    if (includeAudio) {
        for (size_t i = 0; i < audTrack.chunkOffsetsInMedia.size(); ++i) {
            uint64_t absOff = finalHdrSize + audTrack.chunkOffsetsInMedia[i];
            if (use64BitOffsets) {
                patch64be(hdr, audChunkEntriesPos + i * 8, absOff);
            } else {
                patch32(hdr, audChunkEntriesPos + i * 4, static_cast<uint32_t>(absOff));
            }
        }
    }

    layout.valid = true;
    layout.synthesizedHeader = std::move(hdr);
    layout.origInitPlaintextLen = static_cast<uint64_t>(initLen);
    layout.mediaPlaintextLen = fragOffsetInMedia;
    layout.durationSeconds = static_cast<double>(vidTrack.totalDurationTicks) / static_cast<double>(std::max<uint32_t>(1, vidTrack.timescale));
    return layout;
}

void applyFreeFourccPatchesToMediaSlice(
    const VirtualStandardMp4Layout& layout,
    uint64_t sliceMediaOffset,
    uint8_t* buf,
    size_t bufLen
) {
    if (!buf || bufLen == 0 || layout.freeFourccMediaOffsets.empty()) return;
    uint64_t sliceEnd = sliceMediaOffset + static_cast<uint64_t>(bufLen);
    static const uint8_t kFree[4] = {'f', 'r', 'e', 'e'};

    auto it = std::lower_bound(
        layout.freeFourccMediaOffsets.begin(),
        layout.freeFourccMediaOffsets.end(),
        sliceMediaOffset > 4 ? (sliceMediaOffset - 4) : 0ULL
    );
    for (; it != layout.freeFourccMediaOffsets.end(); ++it) {
        uint64_t patchPos = *it;
        if (patchPos >= sliceEnd) break;
        for (uint64_t k = 0; k < 4; ++k) {
            uint64_t p = patchPos + k;
            if (p >= sliceMediaOffset && p < sliceEnd) {
                buf[static_cast<size_t>(p - sliceMediaOffset)] = kFree[k];
            }
        }
    }
}

} // namespace maxwatch
