#include "MediaPipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <strmif.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <codecapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;
#endif

namespace maxwatch {

namespace {

double monotonicNowSeconds() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

#ifdef _WIN32
int64_t currentUnixMillis() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void secureZeroPcm(std::vector<int16_t>& pcm) {
    if (!pcm.empty()) {
        SecureZeroMemory(pcm.data(), pcm.size() * sizeof(int16_t));
        pcm.clear();
    }
}
#endif

} // namespace

// MARK: - MotionDetector

MotionDetector::MotionDetector() {
    setSensitivityPercent(78.0);
}

int MotionDetector::adaptiveCellDeltaFloor(double meanLuma) {
    if (meanLuma >= 50.0) return 14;
    if (meanLuma <= 18.0) return 9;
    double t = (meanLuma - 18.0) / 32.0;
    return 9 + static_cast<int>(std::lround(t * 5.0));
}

void MotionDetector::setSensitivityPercent(double pct) {
    double clamped = std::clamp(pct, 10.0, 100.0);
    sensitivityPercent_.store(clamped);
    double norm = std::clamp((100.0 - clamped) / 90.0, 0.0, 1.0);
    double thresh = 0.25 + std::pow(norm, 1.65) * 10.25;
    thresholdPercent_.store(thresh);
}

std::wstring MotionDetector::sensitivityLabelWide() const {
    double pct = sensitivityPercent_.load();
    double thresh = thresholdPercent_.load();
    const wchar_t* desc = L"Heavy Motion Only";
    if (pct >= 88.0) desc = L"Ultra Sensitive";
    else if (pct >= 68.0) desc = L"Balanced";
    else if (pct >= 40.0) desc = L"Moderate";

    wchar_t buf[128];
    std::swprintf(buf, 128, L"Sensitivity: %.0f%% (%ls \x2022 %.2f%% trigger)", pct, desc, thresh);
    return std::wstring(buf);
}

void MotionDetector::resetWarmup(double warmupSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    prevGrid_.clear();
    prevMean_ = 0.0;
    consecutiveHits_ = 0;
    warmupUntilSec_ = monotonicNowSeconds() + warmupSeconds;
}

MotionDetector::EvalResult MotionDetector::analyzeNV12Luma(
    const uint8_t* planeY,
    int strideY,
    int width,
    int height,
    double nowMonotonicSec
) {
    EvalResult res{};
    if (!planeY || width < 64 || height < 64) return res;

    constexpr int kCols = 80;
    constexpr int kRows = 45;
    constexpr int kCount = kCols * kRows;

    std::vector<uint8_t> grid(kCount, 0);
    int64_t sum = 0;

    for (int r = 0; r < kRows; ++r) {
        int y = std::min(height - 2, (r * (height - 2)) / kRows);
        const uint8_t* row0 = planeY + y * strideY;
        const uint8_t* row1 = planeY + (y + 1) * strideY;
        for (int c = 0; c < kCols; ++c) {
            int x = std::min(width - 2, (c * (width - 2)) / kCols);
            int avg = (static_cast<int>(row0[x]) + static_cast<int>(row0[x + 1]) +
                       static_cast<int>(row1[x]) + static_cast<int>(row1[x + 1])) >> 2;
            grid[r * kCols + c] = static_cast<uint8_t>(avg);
            sum += avg;
        }
    }

    double mean = static_cast<double>(sum) / static_cast<double>(kCount);
    int cellFloor = adaptiveCellDeltaFloor(mean);
    double scorePct = 0.0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(prevGrid_.size()) == kCount) {
        int globalShift = static_cast<int>(std::lround((mean - prevMean_) * 0.65));
        int changed = 0;
        for (int i = 0; i < kCount; ++i) {
            int adjusted = static_cast<int>(grid[i]) - globalShift;
            int diff = std::abs(adjusted - static_cast<int>(prevGrid_[i]));
            if (diff > cellFloor) changed++;
        }
        scorePct = (static_cast<double>(changed) / static_cast<double>(kCount)) * 100.0;
    }
    prevGrid_ = std::move(grid);
    prevMean_ = mean;

    double thresh = thresholdPercent_.load();
    bool hit = false;
    if (nowMonotonicSec >= warmupUntilSec_ && scorePct >= thresh) {
        consecutiveHits_++;
        hit = (consecutiveHits_ >= 2);
    } else {
        consecutiveHits_ = 0;
    }

    res.scorePercent = scorePct;
    res.meanLuma = mean;
    res.triggered = hit;
    return res;
}

// MARK: - SoundSensitivityCheckpoint & SoundDetector

double SoundSensitivityCheckpoint::thresholdDB() const {
    return SoundDetector::thresholdDBForPercent(percent);
}

const std::array<SoundSensitivityCheckpoint, 5>& SoundSensitivityCheckpoint::all() {
    static const std::array<SoundSensitivityCheckpoint, 5> kCheckpoints{{
        {15.0, L"Door Slam / Shout / Alarm", L"Only loud impacts, shouting, glass break, or smoke alarms"},
        {35.0, L"Loud Voice / Knock / Bark", L"Raised voice, firm door knock, dog bark, dropped object"},
        {55.0, L"Normal Talking Voice",      L"Everyday conversation in room, phone/TV audio, door opening"},
        {78.0, L"Quiet Voice / Footsteps",   L"Soft talking across room, footsteps, keys, doorknob turn"},
        {95.0, L"Whisper / Soft Rustle",     L"Faint whisper, tiptoeing, fabric or paper rustle (quiet rooms)"}
    }};
    return kCheckpoints;
}

SoundSensitivityCheckpoint SoundSensitivityCheckpoint::nearest(double pct) {
    double clamped = std::clamp(pct, 10.0, 100.0);
    const auto& cps = all();
    SoundSensitivityCheckpoint best = cps[2];
    double bestDist = 1e9;
    for (const auto& cp : cps) {
        double d = std::abs(cp.percent - clamped);
        if (d < bestDist) {
            bestDist = d;
            best = cp;
        }
    }
    return best;
}

double SoundSensitivityCheckpoint::snapIfClose(double rawPercent, double tolerance) {
    double clamped = std::clamp(rawPercent, 10.0, 100.0);
    for (const auto& cp : all()) {
        if (std::abs(cp.percent - clamped) <= tolerance) {
            return cp.percent;
        }
    }
    return clamped;
}

SoundDetector::SoundDetector() {
    setSensitivityPercent(kDefaultSensitivityPercent);
}

double SoundDetector::thresholdDBForPercent(double percent) {
    double clamped = std::clamp(percent, 10.0, 100.0);
    // 10% -> -10.0 dB, 55% -> -30.0 dB ("Normal Talking Voice"), 100% -> -50.0 dB
    return -10.0 - ((clamped - 10.0) / 90.0) * 40.0;
}

double SoundDetector::meterFractionForDB(double db) {
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

void SoundDetector::setEnabled(bool enabled) {
    enabled_.store(enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        recentWindowHits_ = {false, false, false};
    }
}

void SoundDetector::setSensitivityPercent(double pct) {
    double clamped = std::clamp(pct, 10.0, 100.0);
    sensitivityPercent_.store(clamped);
    thresholdDB_.store(thresholdDBForPercent(clamped));
}

std::wstring SoundDetector::readoutLabelWide() const {
    double pct = sensitivityPercent_.load();
    double thresh = thresholdDB_.load();
    auto cp = SoundSensitivityCheckpoint::nearest(pct);
    wchar_t buf[180];
    if (enabled_.load()) {
        std::swprintf(buf, 180, L"%.0f%% \x2022 \"%ls\" (%.0f dB) + Motion", pct, cp.title, thresh);
    } else {
        std::swprintf(buf, 180, L"Off (Motion Only \x2022 %.0f%% = \"%ls\")", pct, cp.title);
    }
    return std::wstring(buf);
}

void SoundDetector::reset(double warmupSeconds) {
    resetUntilTimestamp(monotonicNowSeconds() + warmupSeconds);
}

void SoundDetector::resetUntilTimestamp(double warmupUntilMonotonicSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    prevIn_ = 0.0;
    prevOut_ = 0.0;
    windowSqSum_ = 0.0;
    windowSampleCount_ = 0;
    recentWindowHits_ = {false, false, false};
    latestFilteredDB_ = -60.0f;
    warmupUntilSec_ = warmupUntilMonotonicSec;
}

void SoundDetector::applyAcousticGrace(double graceSeconds) {
    applyAcousticGraceUntil(monotonicNowSeconds() + graceSeconds);
}

void SoundDetector::applyAcousticGraceUntil(double untilMonotonicSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (untilMonotonicSec > warmupUntilSec_) {
        warmupUntilSec_ = untilMonotonicSec;
    }
    recentWindowHits_ = {false, false, false};
}

SoundEvaluation SoundDetector::analyzePCM16Mono(
    const int16_t* samples,
    size_t count,
    int sampleRate,
    double nowMonotonicSec
) {
    std::lock_guard<std::mutex> lock(mutex_);
    double thresh = thresholdDB_.load();
    bool active = enabled_.load();

    if (!samples || count == 0) {
        return SoundEvaluation{latestFilteredDB_, static_cast<float>(thresh), false};
    }

    int windowTarget = std::max(240, sampleRate / 20); // 50 ms window (2400 @ 48 kHz)
    bool didTrigger = false;
    float chunkPeakDB = -60.0f;
    bool completedWindow = false;

    for (size_t i = 0; i < count; ++i) {
        double x = static_cast<double>(samples[i]) / 32768.0;
        double y = kHpfAlpha * (prevOut_ + x - prevIn_);
        prevIn_ = x;
        prevOut_ = y;

        windowSqSum_ += y * y;
        windowSampleCount_++;

        if (windowSampleCount_ >= windowTarget) {
            double rms = std::sqrt(windowSqSum_ / static_cast<double>(windowSampleCount_));
            double winDB = (rms > 0.00001) ? std::clamp(20.0 * std::log10(rms), -60.0, 0.0) : -60.0;
            latestFilteredDB_ = static_cast<float>(winDB);
            if (!completedWindow || latestFilteredDB_ > chunkPeakDB) {
                chunkPeakDB = latestFilteredDB_;
            }
            completedWindow = true;

            bool isHit = active && (nowMonotonicSec >= warmupUntilSec_) && (winDB >= thresh);
            recentWindowHits_[0] = recentWindowHits_[1];
            recentWindowHits_[1] = recentWindowHits_[2];
            recentWindowHits_[2] = isHit;

            int hitCount = (recentWindowHits_[0] ? 1 : 0) +
                           (recentWindowHits_[1] ? 1 : 0) +
                           (recentWindowHits_[2] ? 1 : 0);
            if (active && nowMonotonicSec >= warmupUntilSec_ && hitCount >= 2) {
                didTrigger = true;
            }

            windowSqSum_ = 0.0;
            windowSampleCount_ = 0;
        }
    }

    float reportedDB = completedWindow ? chunkPeakDB : latestFilteredDB_;
    return SoundEvaluation{reportedDB, static_cast<float>(thresh), didTrigger};
}

// MARK: - Color Conversions (BT.709 Video Range NV12 <-> BGRA32)

void convertNV12ToBGRA(
    const uint8_t* planeY, int strideY,
    const uint8_t* planeUV, int strideUV,
    int width, int height,
    uint8_t* dstBGRA, int dstStrideBGRA
) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = planeY + y * strideY;
        const uint8_t* uvRow = planeUV + (y >> 1) * strideUV;
        uint8_t* dst = dstBGRA + y * dstStrideBGRA;
        for (int x = 0; x < width; ++x) {
            int Y = std::max(0, static_cast<int>(yRow[x]) - 16);
            int uvIdx = (x & ~1);
            int Cb = static_cast<int>(uvRow[uvIdx]) - 128;
            int Cr = static_cast<int>(uvRow[uvIdx + 1]) - 128;

            int yScaled = 298 * Y + 128;
            int r = (yScaled + 459 * Cr) >> 8;
            int g = (yScaled - 55 * Cb - 136 * Cr) >> 8;
            int b = (yScaled + 541 * Cb) >> 8;

            dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
}

void convertBGRAToNV12(
    const uint8_t* srcBGRA, int srcStrideBGRA,
    int width, int height,
    uint8_t* dstY, int dstStrideY,
    uint8_t* dstUV, int dstStrideUV
) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = srcBGRA + y * srcStrideBGRA;
        uint8_t* yRow = dstY + y * dstStrideY;
        for (int x = 0; x < width; ++x) {
            int b = src[x * 4 + 0];
            int g = src[x * 4 + 1];
            int r = src[x * 4 + 2];
            int yVal = 16 + ((47 * r + 157 * g + 16 * b + 128) >> 8);
            yRow[x] = static_cast<uint8_t>(std::clamp(yVal, 16, 235));
        }
    }
    int halfH = height >> 1;
    int halfW = width >> 1;
    for (int r2 = 0; r2 < halfH; ++r2) {
        const uint8_t* src0 = srcBGRA + (r2 * 2) * srcStrideBGRA;
        const uint8_t* src1 = srcBGRA + (r2 * 2 + 1) * srcStrideBGRA;
        uint8_t* uvRow = dstUV + r2 * dstStrideUV;
        for (int c2 = 0; c2 < halfW; ++c2) {
            int x0 = c2 * 2 * 4;
            int b = (src0[x0 + 0] + src0[x0 + 4] + src1[x0 + 0] + src1[x0 + 4] + 2) >> 2;
            int g = (src0[x0 + 1] + src0[x0 + 5] + src1[x0 + 1] + src1[x0 + 5] + 2) >> 2;
            int r = (src0[x0 + 2] + src0[x0 + 6] + src1[x0 + 2] + src1[x0 + 6] + 2) >> 2;
            int cb = 128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8);
            int cr = 128 + ((112 * r - 102 * g - 10 * b + 128) >> 8);
            uvRow[c2 * 2 + 0] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
            uvRow[c2 * 2 + 1] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
        }
    }
}

#ifdef _WIN32

void ensureMediaFoundationInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_FULL);
    });
}

namespace {

struct ScopedComThread {
    HRESULT hr;
    ScopedComThread() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ScopedComThread() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

constexpr DWORD kMfFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kMfFirstAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kMfAllStreams       = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMfMediaSource      = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

// Zero-Disk-Plaintext COM IMFByteStream + IMFAttributes backed purely by VaultClipReader in RAM!
class VaultMemoryByteStream final : public IMFByteStream, public IMFAttributes {
public:
    explicit VaultMemoryByteStream(std::shared_ptr<VaultClipReader> reader)
        : refCount_(1), reader_(std::move(reader)), pos_(0) {
        if (reader_) {
            reader_->ensureStandardMp4Layout();
        }
        MFCreateAttributes(&attrs_, 4);
        if (attrs_) {
            attrs_->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
            attrs_->SetString(MF_BYTESTREAM_ORIGIN_NAME, L"vault_clip.mp4");
        }
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
            *ppv = static_cast<IMFByteStream*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IMFAttributes)) {
            *ppv = static_cast<IMFAttributes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refCount_);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return c;
    }

    // IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override {
        if (!pdwCapabilities) return E_POINTER;
        *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
        return S_OK;
    }

    STDMETHODIMP GetLength(QWORD* pqwLength) override {
        if (!pqwLength) return E_POINTER;
        *pqwLength = static_cast<QWORD>(std::max<int64_t>(0, reader_->standardMp4Length()));
        return S_OK;
    }

    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }

    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override {
        if (!pqwPosition) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *pqwPosition = static_cast<QWORD>(pos_);
        return S_OK;
    }

    STDMETHODIMP SetCurrentPosition(QWORD qwPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t total = reader_->standardMp4Length();
        pos_ = std::clamp<int64_t>(static_cast<int64_t>(qwPosition), 0, total);
        return S_OK;
    }

    STDMETHODIMP IsEndOfStream(BOOL* pfEndOfStream) override {
        if (!pfEndOfStream) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *pfEndOfStream = (pos_ >= reader_->standardMp4Length()) ? TRUE : FALSE;
        return S_OK;
    }

    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* pcbRead) override {
        if (!pb || !pcbRead) return E_POINTER;
        *pcbRead = 0;
        if (cb == 0) return S_OK;

        int64_t startPos = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startPos = pos_;
        }

        try {
            auto chunk = reader_->readStandardMp4Range(startPos, cb);
            ULONG got = static_cast<ULONG>(chunk.size());
            if (got > 0) {
                std::memcpy(pb, chunk.data(), got);
                crypto::secureZeroVec(chunk);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pos_ = startPos + static_cast<int64_t>(got);
            }
            *pcbRead = got;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    class ReadOpState final : public IUnknown {
    public:
        ULONG bytesRead = 0;
        explicit ReadOpState(ULONG n) : bytesRead(n), ref_(1) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown)) {
                *ppv = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
        STDMETHODIMP_(ULONG) Release() override {
            ULONG c = InterlockedDecrement(&ref_);
            if (c == 0) delete this;
            return c;
        }
    private:
        ULONG ref_;
    };

    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) override {
        if (!pb || !pCallback) return E_POINTER;
        ULONG got = 0;
        HRESULT hr = Read(pb, cb, &got);
        ComPtr<IUnknown> opState;
        opState.Attach(new ReadOpState(got));
        ComPtr<IMFAsyncResult> asyncRes;
        HRESULT hrRes = MFCreateAsyncResult(opState.Get(), pCallback, punkState, &asyncRes);
        if (FAILED(hrRes)) return hrRes;
        asyncRes->SetStatus(hr);
        return MFInvokeCallback(asyncRes.Get());
    }

    STDMETHODIMP EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) override {
        if (!pResult || !pcbRead) return E_POINTER;
        *pcbRead = 0;
        HRESULT st = pResult->GetStatus();
        if (FAILED(st)) return st;
        ComPtr<IUnknown> obj;
        if (SUCCEEDED(pResult->GetObject(&obj)) && obj) {
            auto* op = static_cast<ReadOpState*>(obj.Get());
            *pcbRead = op->bytesRead;
        }
        return S_OK;
    }

    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_NOTIMPL; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_NOTIMPL; }

    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* pqwCurrentPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t total = reader_->standardMp4Length();
        int64_t base = (origin == msoCurrent) ? pos_ : 0;
        pos_ = std::clamp<int64_t>(base + offset, 0, total);
        if (pqwCurrentPosition) *pqwCurrentPosition = static_cast<QWORD>(pos_);
        return S_OK;
    }

    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override { return S_OK; }

    // Forward IMFAttributes to inner attrs_
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_ ? attrs_->GetItem(k, v) : E_FAIL; }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_ ? attrs_->GetItemType(k, t) : E_FAIL; }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_ ? attrs_->CompareItem(k, v, r) : E_FAIL; }
    STDMETHODIMP Compare(IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attrs_ ? attrs_->Compare(t, m, r) : E_FAIL; }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_ ? attrs_->GetUINT32(k, v) : E_FAIL; }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_ ? attrs_->GetUINT64(k, v) : E_FAIL; }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_ ? attrs_->GetDouble(k, v) : E_FAIL; }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_ ? attrs_->GetGUID(k, v) : E_FAIL; }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return attrs_ ? attrs_->GetStringLength(k, l) : E_FAIL; }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 c, UINT32* l) override { return attrs_ ? attrs_->GetString(k, s, c, l) : E_FAIL; }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return attrs_ ? attrs_->GetAllocatedString(k, s, l) : E_FAIL; }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return attrs_ ? attrs_->GetBlobSize(k, s) : E_FAIL; }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 cb, UINT32* s) override { return attrs_ ? attrs_->GetBlob(k, b, cb, s) : E_FAIL; }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return attrs_ ? attrs_->GetAllocatedBlob(k, b, s) : E_FAIL; }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return attrs_ ? attrs_->GetUnknown(k, r, p) : E_FAIL; }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_ ? attrs_->SetItem(k, v) : E_FAIL; }
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_ ? attrs_->DeleteItem(k) : E_FAIL; }
    STDMETHODIMP DeleteAllItems() override { return attrs_ ? attrs_->DeleteAllItems() : E_FAIL; }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_ ? attrs_->SetUINT32(k, v) : E_FAIL; }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_ ? attrs_->SetUINT64(k, v) : E_FAIL; }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_ ? attrs_->SetDouble(k, v) : E_FAIL; }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_ ? attrs_->SetGUID(k, v) : E_FAIL; }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_ ? attrs_->SetString(k, v) : E_FAIL; }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 cb) override { return attrs_ ? attrs_->SetBlob(k, b, cb) : E_FAIL; }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_ ? attrs_->SetUnknown(k, u) : E_FAIL; }
    STDMETHODIMP LockStore() override { return attrs_ ? attrs_->LockStore() : E_FAIL; }
    STDMETHODIMP UnlockStore() override { return attrs_ ? attrs_->UnlockStore() : E_FAIL; }
    STDMETHODIMP GetCount(UINT32* c) override { return attrs_ ? attrs_->GetCount(c) : E_FAIL; }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_ ? attrs_->GetItemByIndex(i, k, v) : E_FAIL; }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return attrs_ ? attrs_->CopyAllItems(d) : E_FAIL; }

private:
    ~VaultMemoryByteStream() = default;
    ULONG refCount_;
    std::shared_ptr<VaultClipReader> reader_;
    ComPtr<IMFAttributes> attrs_;
    std::mutex mutex_;
    int64_t pos_;
};

ComPtr<IMFTransform> createSyncH264EncoderMFT(int width, int height, std::vector<uint8_t>& outSeqHeaderSps, std::vector<uint8_t>& outSeqHeaderPps) {
    MFT_REGISTER_TYPE_INFO inType{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inType,
        &outType,
        &ppActivate,
        &count
    );
    if (FAILED(hr) || count == 0) {
        return nullptr;
    }

    ComPtr<IMFTransform> mft;
    for (UINT32 i = 0; i < count && !mft; ++i) {
        ComPtr<IMFTransform> candidate;
        if (SUCCEEDED(ppActivate[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
            mft = candidate;
        }
    }
    for (UINT32 i = 0; i < count; ++i) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);
    if (!mft) return nullptr;

    // Configure low-latency, zero-B-frame, 8-frame keyframe GOP via ICodecAPI
    ComPtr<ICodecAPI> codecApi;
    mft.As(&codecApi);
    auto applyLowLatencyGop = [&]() {
        if (!codecApi) return;
        VARIANT vGop{};
        vGop.vt = VT_UI4;
        vGop.ulVal = 8; // Keyframe every ~0.26s for zero-lag fast ball scrubbing!
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &vGop);

        VARIANT vB{};
        vB.vt = VT_UI4;
        vB.ulVal = 0; // Zero B-frames
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vB);

        VARIANT vLowLat{};
        vLowLat.vt = VT_BOOL;
        vLowLat.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &vLowLat);
    };
    applyLowLatencyGop();

    double scale = std::clamp(static_cast<double>(width * height) / (1920.0 * 1080.0), 0.30, 1.50);
    UINT32 bitrate = static_cast<UINT32>(12000000.0 * scale);

    // IMPORTANT: Microsoft H.264 Encoder MFT requires SetOutputType BEFORE SetInputType!
    bool outConfigured = false;
    const UINT32 profiles[3] = {eAVEncH264VProfile_Main, eAVEncH264VProfile_High, eAVEncH264VProfile_Base};
    for (UINT32 prof : profiles) {
        ComPtr<IMFMediaType> mtOut;
        MFCreateMediaType(&mtOut);
        mtOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mtOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        mtOut->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        mtOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mtOut->SetUINT32(MF_MT_MPEG2_PROFILE, prof);
        MFSetAttributeSize(mtOut.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(mtOut.Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(mtOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(mft->SetOutputType(0, mtOut.Get(), 0))) {
            outConfigured = true;
            break;
        }
    }
    if (!outConfigured) return nullptr;

    ComPtr<IMFMediaType> mtIn;
    MFCreateMediaType(&mtIn);
    mtIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mtIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    mtIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(mtIn.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(mtIn.Get(), MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(mtIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(mft->SetInputType(0, mtIn.Get(), 0))) {
        return nullptr;
    }

    // Re-apply GOP & zero-B-frame properties AFTER SetOutputType + SetInputType because
    // Microsoft's software h264enc.dll resets GOPSize back to default during media type negotiation!
    applyLowLatencyGop();

    // Extract MF_MT_MPEG_SEQUENCE_HEADER if already available
    ComPtr<IMFMediaType> curOut;
    if (SUCCEEDED(mft->GetOutputCurrentType(0, &curOut)) && curOut) {
        UINT8* blob = nullptr;
        UINT32 blobSize = 0;
        if (SUCCEEDED(curOut->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &blobSize)) && blob && blobSize > 4) {
            auto parsed = parseAnnexBAccessUnit(blob, blobSize);
            if (!parsed.sps.empty()) outSeqHeaderSps = parsed.sps;
            if (!parsed.pps.empty()) outSeqHeaderPps = parsed.pps;
            CoTaskMemFree(blob);
        }
    }

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return mft;
}

ComPtr<IMFTransform> createSyncAacEncoderMFT(uint16_t& outChannels, std::vector<uint8_t>& outAsc) {
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Audio, MFAudioFormat_PCM};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, MFAudioFormat_AAC};

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inInfo,
        &outInfo,
        &ppActivate,
        &count
    );
    if (FAILED(hr) || count == 0) return nullptr;

    ComPtr<IMFTransform> mft;
    for (UINT32 i = 0; i < count && !mft; ++i) {
        ComPtr<IMFTransform> candidate;
        if (SUCCEEDED(ppActivate[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
            mft = candidate;
        }
    }
    for (UINT32 i = 0; i < count; ++i) ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);
    if (!mft) return nullptr;

    // Try 1-channel 48kHz PCM first, fallback to 2-channel 48kHz PCM
    for (uint16_t ch : {uint16_t(1), uint16_t(2)}) {
        ComPtr<IMFMediaType> mtIn;
        MFCreateMediaType(&mtIn);
        mtIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mtIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        mtIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        mtIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        mtIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        mtIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
        mtIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * ch * 2);

        if (FAILED(mft->SetInputType(0, mtIn.Get(), 0))) continue;

        // Pick matching 48kHz raw AAC output type (preferring 16000 bytes/sec = 128 kbps)
        ComPtr<IMFMediaType> chosenOut;
        for (DWORD idx = 0;; ++idx) {
            ComPtr<IMFMediaType> avail;
            if (FAILED(mft->GetOutputAvailableType(0, idx, &avail)) || !avail) break;
            GUID sub{};
            UINT32 sr = 0, outCh = 0, payload = 0, avgBytes = 0;
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            avail->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            avail->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outCh);
            if (FAILED(avail->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload))) payload = 0;
            avail->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avgBytes);
            if (sub == MFAudioFormat_AAC && sr == 48000 && outCh == ch && payload == 0) {
                chosenOut = avail;
                if (avgBytes == 16000) break;
            }
        }
        if (!chosenOut) {
            ComPtr<IMFMediaType> manualOut;
            MFCreateMediaType(&manualOut);
            manualOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            manualOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            manualOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            manualOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            manualOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            manualOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
            manualOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            chosenOut = manualOut;
        }

        if (SUCCEEDED(mft->SetOutputType(0, chosenOut.Get(), 0))) {
            outChannels = ch;
            outAsc = Fmp4SegmentMuxer::makeDefaultAacSpecificConfig(48000, ch);
            ComPtr<IMFMediaType> curOut;
            if (SUCCEEDED(mft->GetOutputCurrentType(0, &curOut)) && curOut) {
                UINT8* ud = nullptr;
                UINT32 udLen = 0;
                if (SUCCEEDED(curOut->GetAllocatedBlob(MF_MT_USER_DATA, &ud, &udLen)) && ud) {
                    // HEAACWAVEINFO header is 12 bytes, followed by AudioSpecificConfig!
                    if (udLen >= 14) {
                        outAsc.assign(ud + 12, ud + 14);
                    }
                    CoTaskMemFree(ud);
                }
            }
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            return mft;
        }
    }

    return nullptr;
}

void drainMftOutputs(IMFTransform* mft, const std::function<void(IMFSample*)>& onSample) {
    if (!mft) return;
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    mft->GetOutputStreamInfo(0, &streamInfo);
    bool mftProvidesSamples = (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = 0;
        ComPtr<IMFSample> allocSample;
        if (!mftProvidesSamples) {
            MFCreateSample(&allocSample);
            ComPtr<IMFMediaBuffer> memBuf;
            DWORD cb = std::max<DWORD>(streamInfo.cbSize, 256 * 1024);
            MFCreateMemoryBuffer(cb, &memBuf);
            allocSample->AddBuffer(memBuf.Get());
            outBuf.pSample = allocSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &outBuf, &status);
        if (outBuf.pEvents) outBuf.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> avail;
            if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &avail)) && avail) {
                mft->SetOutputType(0, avail.Get(), 0);
                mft->GetOutputStreamInfo(0, &streamInfo);
                mftProvidesSamples = (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            }
            continue;
        }
        if (FAILED(hr)) {
            if (mftProvidesSamples && outBuf.pSample) outBuf.pSample->Release();
            break;
        }

        if (outBuf.pSample) {
            onSample(outBuf.pSample);
            if (mftProvidesSamples) {
                outBuf.pSample->Release();
            }
        }
    }
}

} // namespace

// MARK: - VaultSegmentWriter

VaultSegmentWriter::VaultSegmentWriter(
    std::unique_ptr<VaultClipFileWriter> clipWriter,
    int width,
    int height,
    bool hasAudio
) : clipWriter_(std::move(clipWriter)),
    width_(std::clamp(width, 320, 1920) & ~1),
    height_(std::clamp(height, 240, 1080) & ~1),
    hasAudio_(hasAudio) {
    ensureMediaFoundationInitialized();
    workerThread_ = std::thread(&VaultSegmentWriter::workerLoop, this);
}

VaultSegmentWriter::~VaultSegmentWriter() {
    stopAndFinalizeSynchronously(2.5);
}

void VaultSegmentWriter::appendNV12Frame(
    const uint8_t* planeY, int strideY,
    const uint8_t* planeUV, int strideUV,
    int srcWidth, int srcHeight,
    int64_t pts100ns,
    int64_t captureUnixMs
) {
    if (!planeY || !planeUV) return;

    size_t ySize = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    size_t uvSize = ySize / 2;
    std::vector<uint8_t> tight(ySize + uvSize, 0);
    uint8_t* dstY = tight.data();
    uint8_t* dstUV = tight.data() + ySize;

    int copyW = std::min(srcWidth, width_);
    int copyH = std::min(srcHeight, height_);
    if (copyW < width_ || copyH < height_) {
        std::memset(dstY, 16, ySize);
        std::memset(dstUV, 128, uvSize);
    }
    for (int r = 0; r < copyH; ++r) {
        std::memcpy(dstY + r * width_, planeY + r * strideY, copyW);
    }
    int uvCopyH = copyH >> 1;
    int uvCopyBytes = copyW & ~1;
    for (int r = 0; r < uvCopyH; ++r) {
        std::memcpy(dstUV + r * width_, planeUV + r * strideUV, uvCopyBytes);
    }

    {
        std::lock_guard<std::mutex> sLock(stampMutex_);
        stamper_.stampBottomLeftNV12(dstY, width_, dstUV, width_, width_, height_, captureUnixMs);
    }

    std::lock_guard<std::mutex> qLock(queueMutex_);
    if (stopRequested_ || abandonRequested_) return;
    if (videoQueue_.size() < 140) {
        QueuedVideo qv;
        qv.nv12 = std::move(tight);
        qv.pts100ns = pts100ns;
        videoQueue_.push_back(std::move(qv));
        queueCv_.notify_one();
    }
}

void VaultSegmentWriter::appendAudioPCM16Mono48k(
    const int16_t* samples,
    size_t sampleCount,
    int64_t pts100ns
) {
    if (!hasAudio_ || !samples || sampleCount == 0) return;
    std::lock_guard<std::mutex> qLock(queueMutex_);
    if (stopRequested_ || abandonRequested_) return;
    if (audioQueue_.size() < 300) {
        QueuedAudio qa;
        qa.pcm.assign(samples, samples + sampleCount);
        qa.pts100ns = pts100ns;
        audioQueue_.push_back(std::move(qa));
        queueCv_.notify_one();
    }
}

void VaultSegmentWriter::stopAndFinalizeSynchronously(double timeoutSeconds) {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (!workerFinished_ && !stopRequested_ && !abandonRequested_) {
            stopRequested_ = true;
            queueCv_.notify_all();
        }
        doneCv_.wait_for(
            lock,
            std::chrono::duration<double>(timeoutSeconds),
            [this]() { return workerFinished_; }
        );
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void VaultSegmentWriter::abandonWithoutFinalizingForPowerCutTest() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        abandonRequested_ = true;
        queueCv_.notify_all();
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void VaultSegmentWriter::workerLoop() {
    ScopedComThread comScope;

    std::vector<uint8_t> sps, pps;
    auto h264Mft = createSyncH264EncoderMFT(width_, height_, sps, pps);

    uint16_t aacChannels = 1;
    std::vector<uint8_t> aacAsc;
    ComPtr<IMFTransform> aacMft;
    if (hasAudio_) {
        aacMft = createSyncAacEncoderMFT(aacChannels, aacAsc);
    }
    bool effectiveAudio = hasAudio_ && (aacMft != nullptr);

    Fmp4SegmentMuxer muxer(
        width_, height_, effectiveAudio, 48000, aacChannels,
        [this](const std::vector<uint8_t>& segBytes, bool isInit) {
            try {
                clipWriter_->appendSegment(
                    segBytes,
                    isInit ? VaultSegmentKind::Initialization : VaultSegmentKind::MediaFragment
                );
            } catch (...) {
            }
        }
    );
    muxer.setCodecPrivateData(sps, pps, aacAsc);

    ComPtr<ICodecAPI> h264CodecApi;
    if (h264Mft) h264Mft.As(&h264CodecApi);

    bool sessionStarted = false;
    int64_t firstVideoPts100ns = 0;
    int64_t latestVideoRel100ns = -1;
    uint64_t videoFrameIdx = 0;

    std::vector<int16_t> pcmAccum;
    pcmAccum.reserve(4096);
    int64_t totalFedAudioFrames48k = 0;

    auto feedAacBlock1024 = [&](const int16_t* mono1024) {
        if (!aacMft) return;
        size_t bytes = 1024 * sizeof(int16_t) * aacChannels;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buf))) return;
        BYTE* dst = nullptr;
        if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
            if (aacChannels == 1) {
                std::memcpy(dst, mono1024, 1024 * sizeof(int16_t));
            } else {
                auto* out16 = reinterpret_cast<int16_t*>(dst);
                for (int i = 0; i < 1024; ++i) {
                    out16[i * 2 + 0] = mono1024[i];
                    out16[i * 2 + 1] = mono1024[i];
                }
            }
            buf->Unlock();
            buf->SetCurrentLength(static_cast<DWORD>(bytes));
        }
        ComPtr<IMFSample> smp;
        MFCreateSample(&smp);
        smp->AddBuffer(buf.Get());
        LONGLONG sampleTime100ns = (totalFedAudioFrames48k * 10000000LL) / 48000LL;
        LONGLONG sampleDur100ns = (1024LL * 10000000LL) / 48000LL;
        smp->SetSampleTime(sampleTime100ns);
        smp->SetSampleDuration(sampleDur100ns);
        totalFedAudioFrames48k += 1024;

        if (SUCCEEDED(aacMft->ProcessInput(0, smp.Get(), 0))) {
            drainMftOutputs(aacMft.Get(), [&](IMFSample* outSmp) {
                ComPtr<IMFMediaBuffer> contig;
                if (SUCCEEDED(outSmp->ConvertToContiguousBuffer(&contig)) && contig) {
                    BYTE* p = nullptr;
                    DWORD cb = 0;
                    if (SUCCEEDED(contig->Lock(&p, nullptr, &cb)) && p && cb > 0) {
                        std::vector<uint8_t> aacPkt(p, p + cb);
                        contig->Unlock();
                        muxer.appendAudioSample(aacPkt, 1024);
                    }
                }
            });
        }
    };

    auto pumpPcmSamples = [&](const int16_t* ptr, size_t count) {
        if (!effectiveAudio || count == 0) return;
        pcmAccum.insert(pcmAccum.end(), ptr, ptr + count);
        while (pcmAccum.size() >= 1024) {
            feedAacBlock1024(pcmAccum.data());
            pcmAccum.erase(pcmAccum.begin(), pcmAccum.begin() + 1024);
        }
    };

    auto processEncodedVideoSample = [&](IMFSample* outSmp) {
        ComPtr<IMFMediaBuffer> contig;
        if (FAILED(outSmp->ConvertToContiguousBuffer(&contig)) || !contig) return;
        BYTE* p = nullptr;
        DWORD cb = 0;
        if (FAILED(contig->Lock(&p, nullptr, &cb)) || !p || cb == 0) return;

        auto parsed = parseAnnexBAccessUnit(p, cb);
        contig->Unlock();

        if (!parsed.sps.empty() || !parsed.pps.empty()) {
            muxer.setCodecPrivateData(parsed.sps, parsed.pps, aacAsc);
        }
        UINT32 cleanPoint = 0;
        outSmp->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
        bool isKey = parsed.hasKeyframeIDR || (cleanPoint != 0);

        LONGLONG outPts100ns = 0;
        outSmp->GetSampleTime(&outPts100ns);
        uint64_t pts90k = static_cast<uint64_t>(std::max<LONGLONG>(0, (outPts100ns * 9LL) / 1000LL));

        if (!parsed.avccLengthPrefixedPayload.empty()) {
            muxer.appendVideoSample(parsed.avccLengthPrefixedPayload, pts90k, 3000, isKey);
        }
    };

    while (true) {
        QueuedVideo vItem;
        QueuedAudio aItem;
        bool hasV = false;
        bool hasA = false;
        bool shouldAbandon = false;
        bool shouldStop = false;

        int64_t fedAudioRel100ns = (
            (totalFedAudioFrames48k + static_cast<int64_t>(pcmAccum.size())) * 10000000LL
        ) / 48000LL;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [&]() {
                return abandonRequested_ || stopRequested_ || !videoQueue_.empty() || !audioQueue_.empty();
            });
            if (abandonRequested_) {
                shouldAbandon = true;
            } else {
                bool preferAudio = !audioQueue_.empty() &&
                    (videoQueue_.empty() ||
                     (sessionStarted && fedAudioRel100ns <= latestVideoRel100ns));
                if (preferAudio) {
                    aItem = std::move(audioQueue_.front());
                    audioQueue_.pop_front();
                    hasA = true;
                } else if (!videoQueue_.empty()) {
                    vItem = std::move(videoQueue_.front());
                    videoQueue_.pop_front();
                    hasV = true;
                } else if (stopRequested_) {
                    shouldStop = true;
                }
            }
        }

        if (shouldAbandon) {
            // Hard power-cut simulation: exit immediately without flushing tail or updating header!
            std::lock_guard<std::mutex> lock(queueMutex_);
            workerFinished_ = true;
            doneCv_.notify_all();
            return;
        }

        if (hasV && h264Mft) {
            if (!sessionStarted) {
                firstVideoPts100ns = vItem.pts100ns;
                sessionStarted = true;
            }
            int64_t rel100ns = vItem.pts100ns - firstVideoPts100ns;
            if (rel100ns < 0) rel100ns = 0;
            if (latestVideoRel100ns >= 0 && rel100ns <= latestVideoRel100ns) {
                rel100ns = latestVideoRel100ns + 333333LL; // Monotonic 30fps fallback
            }
            latestVideoRel100ns = rel100ns;
            recordedDurationSec_.store(static_cast<double>(rel100ns) / 10000000.0);

            // Force keyframe on frame 0 and every 8 frames (and at 1.0s boundaries)
            if (h264CodecApi && (videoFrameIdx % 8 == 0 || videoFrameIdx % 30 == 0)) {
                VARIANT vKey{};
                vKey.vt = VT_UI4;
                vKey.ulVal = 1;
                h264CodecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &vKey);
            }
            videoFrameIdx++;

            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(MFCreateMemoryBuffer(static_cast<DWORD>(vItem.nv12.size()), &buf))) {
                BYTE* dst = nullptr;
                if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
                    std::memcpy(dst, vItem.nv12.data(), vItem.nv12.size());
                    buf->Unlock();
                    buf->SetCurrentLength(static_cast<DWORD>(vItem.nv12.size()));

                    ComPtr<IMFSample> smp;
                    MFCreateSample(&smp);
                    smp->AddBuffer(buf.Get());
                    smp->SetSampleTime(rel100ns);
                    smp->SetSampleDuration(333333LL);

                    if (SUCCEEDED(h264Mft->ProcessInput(0, smp.Get(), 0))) {
                        // Refresh SPS/PPS from current output media type if needed
                        if (!muxer.isInitEmitted()) {
                            ComPtr<IMFMediaType> curOut;
                            if (SUCCEEDED(h264Mft->GetOutputCurrentType(0, &curOut)) && curOut) {
                                UINT8* blob = nullptr;
                                UINT32 blobSize = 0;
                                if (SUCCEEDED(curOut->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &blobSize)) && blob) {
                                    auto seq = parseAnnexBAccessUnit(blob, blobSize);
                                    muxer.setCodecPrivateData(seq.sps, seq.pps, aacAsc);
                                    CoTaskMemFree(blob);
                                }
                            }
                        }
                        drainMftOutputs(h264Mft.Get(), processEncodedVideoSample);
                    }
                }
            }

            // Audio gap watchdog: if audio stalls > 0.35s behind video, feed silent 48kHz PCM!
            if (effectiveAudio) {
                double videoSec = static_cast<double>(rel100ns) / 10000000.0;
                double audioSec = static_cast<double>(totalFedAudioFrames48k + static_cast<int64_t>(pcmAccum.size())) / 48000.0;
                double gap = videoSec - audioSec;
                if (gap > 0.35) {
                    size_t silentCount = static_cast<size_t>(std::min(0.25, gap - 0.10) * 48000.0);
                    if (silentCount >= 1024) {
                        std::vector<int16_t> silence(silentCount, 0);
                        pumpPcmSamples(silence.data(), silence.size());
                    }
                }
            }
        } else if (hasA && sessionStarted && effectiveAudio) {
            if (aItem.pts100ns >= firstVideoPts100ns) {
                pumpPcmSamples(aItem.pcm.data(), aItem.pcm.size());
            }
        } else if (shouldStop) {
            break;
        }
    }

    // Drain encoders and finalize cleanly!
    if (sessionStarted) {
        if (effectiveAudio && latestVideoRel100ns > 0) {
            double targetSec = static_cast<double>(latestVideoRel100ns + 333333LL) / 10000000.0;
            double audioSec = static_cast<double>(totalFedAudioFrames48k + static_cast<int64_t>(pcmAccum.size())) / 48000.0;
            if (targetSec > audioSec) {
                size_t padSamples = static_cast<size_t>((targetSec - audioSec) * 48000.0) + 1024;
                std::vector<int16_t> silence(padSamples, 0);
                pumpPcmSamples(silence.data(), silence.size());
            }
            if (aacMft) {
                aacMft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                drainMftOutputs(aacMft.Get(), [&](IMFSample* outSmp) {
                    ComPtr<IMFMediaBuffer> contig;
                    if (SUCCEEDED(outSmp->ConvertToContiguousBuffer(&contig)) && contig) {
                        BYTE* p = nullptr;
                        DWORD cb = 0;
                        if (SUCCEEDED(contig->Lock(&p, nullptr, &cb)) && p && cb > 0) {
                            std::vector<uint8_t> aacPkt(p, p + cb);
                            contig->Unlock();
                            muxer.appendAudioSample(aacPkt, 1024);
                        }
                    }
                });
            }
        }
        if (h264Mft) {
            h264Mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            drainMftOutputs(h264Mft.Get(), processEncodedVideoSample);
        }
        muxer.flushFinalFragment();
    }

    double finalDur = std::max(0.1, muxer.totalEmittedVideoSeconds());
    try {
        clipWriter_->finalizeAndClose(finalDur, sessionStarted && muxer.isInitEmitted());
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        workerFinished_ = true;
    }
    doneCv_.notify_all();
}

// MARK: - CaptureEngine (Live Webcam + WASAPI Microphone)

CaptureEngine::CaptureEngine() {
    ensureMediaFoundationInitialized();
}

CaptureEngine::~CaptureEngine() {
    stopCapture();
}

std::vector<DeviceInfo> CaptureEngine::enumerateVideoDevices() {
    ensureMediaFoundationInitialized();
    std::vector<DeviceInfo> list;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return list;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &ppDevices, &count))) return list;

    for (UINT32 i = 0; i < count; ++i) {
        LPWSTR name = nullptr, symLink = nullptr;
        UINT32 nameLen = 0, linkLen = 0;
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symLink, &linkLen);
        if (symLink) {
            DeviceInfo info;
            info.id = symLink;
            info.friendlyName = name ? name : L"USB / Integrated Camera";
            list.push_back(std::move(info));
        }
        if (name) CoTaskMemFree(name);
        if (symLink) CoTaskMemFree(symLink);
        ppDevices[i]->Release();
    }
    CoTaskMemFree(ppDevices);
    return list;
}

std::vector<DeviceInfo> CaptureEngine::enumerateAudioDevices() {
    ensureMediaFoundationInitialized();
    std::vector<DeviceInfo> list;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return list;
    }

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)) || !coll) {
        return list;
    }

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev)) || !dev) continue;
        LPWSTR devId = nullptr;
        if (FAILED(dev->GetId(&devId)) || !devId) continue;

        std::wstring friendly = L"Microphone";
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                friendly = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }

        DeviceInfo info;
        info.id = devId;
        info.friendlyName = friendly;
        list.push_back(std::move(info));
        CoTaskMemFree(devId);
    }
    return list;
}

void CaptureEngine::startCapture(
    const std::wstring& preferredVideoId,
    const std::wstring& preferredAudioId,
    bool recordAudioEnabled
) {
    stopCapture();
    sampleAudioEnabled_.store(recordAudioEnabled);
    running_.store(true);
    videoThread_ = std::thread(&CaptureEngine::videoCaptureLoop, this, preferredVideoId, false);

    if (recordAudioEnabled) {
        audioRunning_.store(true);
        audioThread_ = std::thread(&CaptureEngine::audioCaptureLoop, this, preferredAudioId);
    } else {
        micActive_.store(false);
        latestAudioDb_.store(-60.0f);
    }
}

void CaptureEngine::startSyntheticCameraForSmokeTest(bool recordAudioEnabled) {
    stopCapture();
    sampleAudioEnabled_.store(recordAudioEnabled);
    running_.store(true);
    videoThread_ = std::thread(&CaptureEngine::videoCaptureLoop, this, L"__SYNTHETIC__", true);
}

void CaptureEngine::stopCapture() {
    running_.store(false);
    audioRunning_.store(false);
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    cameraActive_.store(false);
    micActive_.store(false);
    {
        std::lock_guard<std::mutex> lock(writerMutex_);
        wipePreRollLocked();
    }
}

std::wstring CaptureEngine::activeSummaryWide() const {
    std::lock_guard<std::mutex> lock(summaryMutex_);
    return activeSummary_;
}

bool CaptureEngine::copyLatestPreviewFrame(BgraFrame& outFrame) const {
    std::lock_guard<std::mutex> lock(previewMutex_);
    if (latestPreview_.pixels.empty() || latestPreview_.width <= 0 || latestPreview_.height <= 0) {
        return false;
    }
    if (outFrame.sequence == latestPreview_.sequence) {
        return true;
    }
    outFrame = latestPreview_;
    return true;
}

void CaptureEngine::wipePreRollLocked() {
    for (auto& item : preRollVideo_) {
        crypto::secureZeroVec(item.nv12);
    }
    preRollVideo_.clear();

    for (auto& item : preRollAudio_) {
        secureZeroPcm(item.pcm);
    }
    preRollAudio_.clear();
    preRollAudioSamples_ = 0;
}

void CaptureEngine::setArmedForPreRoll(bool armed) {
    armedForPreRoll_.store(armed);
    if (!armed) {
        std::lock_guard<std::mutex> lock(writerMutex_);
        wipePreRollLocked();
    }
}

void CaptureEngine::applyLockKeyAcousticGrace(double graceSeconds) {
    soundDetector_.applyAcousticGrace(graceSeconds);
}

void CaptureEngine::setSampleAudioEnabled(bool enabled) {
    sampleAudioEnabled_.store(enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(writerMutex_);
        for (auto& item : preRollAudio_) {
            secureZeroPcm(item.pcm);
        }
        preRollAudio_.clear();
        preRollAudioSamples_ = 0;
    }
}

void CaptureEngine::attachActiveSegmentWriter(std::shared_ptr<VaultSegmentWriter> writer) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    if (writer && armedForPreRoll_.load() && !preRollVideo_.empty()) {
        double vSpanSec = 1.0 / 30.0;
        if (preRollVideo_.size() >= 2) {
            vSpanSec = std::clamp(
                static_cast<double>(preRollVideo_.back().pts100ns - preRollVideo_.front().pts100ns) / 10000000.0,
                0.05, 1.25
            );
        }
        size_t maxTargetAudioSamples = static_cast<size_t>(vSpanSec * 48000.0) + 2400;
        while (preRollAudioSamples_ > maxTargetAudioSamples && preRollAudio_.size() > 1) {
            preRollAudioSamples_ -= preRollAudio_.front().pcm.size();
            secureZeroPcm(preRollAudio_.front().pcm);
            preRollAudio_.pop_front();
        }

        for (auto& v : preRollVideo_) {
            if (v.width > 0 && v.height > 0 && v.nv12.size() >= static_cast<size_t>(v.width * v.height * 3 / 2)) {
                const uint8_t* yP = v.nv12.data();
                const uint8_t* uvP = v.nv12.data() + v.width * v.height;
                writer->appendNV12Frame(yP, v.width, uvP, v.width, v.width, v.height, v.pts100ns, v.captureUnixMs);
            }
            crypto::secureZeroVec(v.nv12);
        }
        preRollVideo_.clear();

        if (sampleAudioEnabled_.load()) {
            for (auto& a : preRollAudio_) {
                if (!a.pcm.empty()) {
                    writer->appendAudioPCM16Mono48k(a.pcm.data(), a.pcm.size(), a.pts100ns);
                }
                secureZeroPcm(a.pcm);
            }
        } else {
            for (auto& a : preRollAudio_) {
                secureZeroPcm(a.pcm);
            }
        }
        preRollAudio_.clear();
        preRollAudioSamples_ = 0;
    } else {
        wipePreRollLocked();
    }
    activeWriter_ = std::move(writer);
}

std::shared_ptr<VaultSegmentWriter> CaptureEngine::detachActiveSegmentWriter() {
    std::lock_guard<std::mutex> lock(writerMutex_);
    auto cur = activeWriter_;
    activeWriter_.reset();
    return cur;
}

std::shared_ptr<VaultSegmentWriter> CaptureEngine::currentSegmentWriter() const {
    std::lock_guard<std::mutex> lock(writerMutex_);
    return activeWriter_;
}

void CaptureEngine::routeCapturedVideoFrame(
    const uint8_t* yPlane, int yStride,
    const uint8_t* uvPlane, int uvStride,
    int w, int h,
    int64_t pts100ns
) {
    if (!yPlane || !uvPlane || w <= 0 || h <= 0) return;

    std::shared_ptr<VaultSegmentWriter> wr;
    {
        std::lock_guard<std::mutex> lock(writerMutex_);
        wr = activeWriter_;
        if (!wr && armedForPreRoll_.load()) {
            size_t yBytes = static_cast<size_t>(w) * static_cast<size_t>(h);
            size_t uvBytes = yBytes / 2;
            PreRollVideoItem item;
            item.width = w;
            item.height = h;
            item.pts100ns = pts100ns;
            item.captureUnixMs = currentUnixMillis();
            item.nv12.resize(yBytes + uvBytes);

            uint8_t* dstY = item.nv12.data();
            uint8_t* dstUV = item.nv12.data() + yBytes;
            for (int r = 0; r < h; ++r) {
                std::memcpy(dstY + r * w, yPlane + r * yStride, static_cast<size_t>(w));
            }
            int uvH = h >> 1;
            int uvCopy = w & ~1;
            for (int r = 0; r < uvH; ++r) {
                std::memcpy(dstUV + r * w, uvPlane + r * uvStride, static_cast<size_t>(uvCopy));
            }

            preRollVideo_.push_back(std::move(item));
            // Keep up to ~1.25s (~38 frames @ 30fps) in RAM ring buffer
            while (preRollVideo_.size() > 38 ||
                   (preRollVideo_.size() >= 2 &&
                    (preRollVideo_.back().pts100ns - preRollVideo_.front().pts100ns) > 12500000LL)) {
                crypto::secureZeroVec(preRollVideo_.front().nv12);
                preRollVideo_.pop_front();
            }
            return;
        }
    }

    if (wr) {
        wr->appendNV12Frame(yPlane, yStride, uvPlane, uvStride, w, h, pts100ns);
    }
}

void CaptureEngine::routeCapturedAudioChunk(const int16_t* samples, size_t count, int64_t pts100ns) {
    if (!sampleAudioEnabled_.load() || !samples || count == 0) return;

    std::shared_ptr<VaultSegmentWriter> wr;
    {
        std::lock_guard<std::mutex> lock(writerMutex_);
        wr = activeWriter_;
        if (!wr && armedForPreRoll_.load()) {
            PreRollAudioItem item;
            item.pcm.assign(samples, samples + count);
            item.pts100ns = pts100ns;
            preRollAudioSamples_ += count;
            preRollAudio_.push_back(std::move(item));

            // Keep up to 1.25s (60,000 samples @ 48 kHz) in RAM ring buffer
            constexpr size_t kMaxPreRollSamples = 60000;
            while (preRollAudioSamples_ > kMaxPreRollSamples && preRollAudio_.size() > 1) {
                preRollAudioSamples_ -= preRollAudio_.front().pcm.size();
                secureZeroPcm(preRollAudio_.front().pcm);
                preRollAudio_.pop_front();
            }
            return;
        }
    }

    if (wr) {
        wr->appendAudioPCM16Mono48k(samples, count, pts100ns);
    }
}

void CaptureEngine::videoCaptureLoop(std::wstring videoDeviceId, bool syntheticMode) {
    ScopedComThread comScope;

    if (syntheticMode) {
        int w = 1280, h = 720;
        activeWidth_.store(w);
        activeHeight_.store(h);
        cameraActive_.store(true);
        cameraPermissionDenied_.store(false);
        {
            std::lock_guard<std::mutex> lock(summaryMutex_);
            activeSummary_ = L"Integrated HD Webcam (1280x720) \x2022 Studio Microphone";
        }
        micActive_.store(sampleAudioEnabled_.load());

        std::vector<uint8_t> nv12(w * h * 3 / 2, 0);
        uint8_t* yPlane = nv12.data();
        uint8_t* uvPlane = nv12.data() + w * h;
        uint64_t frameCounter = 0;
        auto startSteady = std::chrono::steady_clock::now();

        while (running_.load()) {
            // Render rich dark studio room gradient + moving test subject so motion detector triggers realistically
            for (int r = 0; r < h; ++r) {
                uint8_t baseLuma = static_cast<uint8_t>(38 + (r * 52) / h);
                std::memset(yPlane + r * w, baseLuma, w);
            }
            for (int r2 = 0; r2 < h / 2; ++r2) {
                uint8_t* uv = uvPlane + r2 * w;
                for (int c2 = 0; c2 < w / 2; ++c2) {
                    uv[c2 * 2 + 0] = 132;
                    uv[c2 * 2 + 1] = 124;
                }
            }
            int boxX = static_cast<int>((frameCounter * 16) % (w - 220));
            for (int r = 180; r < 520; ++r) {
                for (int c = boxX; c < boxX + 200; ++c) {
                    yPlane[r * w + c] = 212;
                }
            }

            int64_t pts100ns = static_cast<int64_t>(frameCounter) * 333333LL;
            routeCapturedVideoFrame(yPlane, w, uvPlane, w, w, h, pts100ns);
            if (sampleAudioEnabled_.load() && frameCounter % 3 == 0) {
                std::vector<int16_t> silentPcm(4800, 0);
                routeCapturedAudioChunk(silentPcm.data(), silentPcm.size(), pts100ns);
            }

            if (frameCounter % 3 == 0) {
                auto ev = motionDetector_.analyzeNV12Luma(yPlane, w, w, h, monotonicNowSeconds());
                latestMotionPct_.store(ev.scorePercent);
                latestMeanLuma_.store(ev.meanLuma);
                if (sampleAudioEnabled_.load()) latestAudioDb_.store(-38.0f);
                if (ev.triggered && onMotionTriggered) {
                    onMotionTriggered();
                }
            }

            std::vector<uint8_t> bgra(w * h * 4);
            convertNV12ToBGRA(yPlane, w, uvPlane, w, w, h, bgra.data(), w * 4);
            {
                std::lock_guard<std::mutex> pLock(previewMutex_);
                latestPreview_.width = w;
                latestPreview_.height = h;
                latestPreview_.ptsSeconds = static_cast<double>(frameCounter) / 30.0;
                latestPreview_.sequence = ++previewSeq_;
                latestPreview_.pixels = std::move(bgra);
            }

            frameCounter++;
            auto nextDeadline = startSteady + std::chrono::microseconds(frameCounter * 33333ULL);
            auto nowSteady = std::chrono::steady_clock::now();
            if (nextDeadline > nowSteady) {
                std::this_thread::sleep_for(nextDeadline - nowSteady);
            }
        }
        return;
    }

    auto vDevices = enumerateVideoDevices();
    if (vDevices.empty()) {
        cameraActive_.store(false);
        std::lock_guard<std::mutex> lock(summaryMutex_);
        activeSummary_ = L"No Camera Detected";
        return;
    }

    DeviceInfo chosen = vDevices.front();
    for (const auto& d : vDevices) {
        if (d.id == videoDeviceId) {
            chosen = d;
            break;
        }
    }

    ComPtr<IMFAttributes> devAttrs;
    MFCreateAttributes(&devAttrs, 2);
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, chosen.id.c_str());

    ComPtr<IMFMediaSource> mediaSource;
    HRESULT hrSrc = MFCreateDeviceSource(devAttrs.Get(), &mediaSource);
    if (FAILED(hrSrc) || !mediaSource) {
        cameraActive_.store(false);
        if (hrSrc == E_ACCESSDENIED || hrSrc == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) {
            cameraPermissionDenied_.store(true);
        }
        std::lock_guard<std::mutex> lock(summaryMutex_);
        activeSummary_ = L"Camera permission needed or busy in another app";
        return;
    }

    // Enable continuous auto-exposure & low-light compensation on UVC webcams for dark/night rooms
    {
        ComPtr<IAMCameraControl> camCtrl;
        if (SUCCEEDED(mediaSource.As(&camCtrl)) && camCtrl) {
            long minExp = 0, maxExp = 0, stepExp = 0, defExp = 0, capsFlags = 0;
            if (SUCCEEDED(camCtrl->GetRange(CameraControl_Exposure, &minExp, &maxExp, &stepExp, &defExp, &capsFlags))) {
                if ((capsFlags & CameraControl_Flags_Auto) != 0) {
                    camCtrl->Set(CameraControl_Exposure, defExp, CameraControl_Flags_Auto);
                }
            }
            long minFocus = 0, maxFocus = 0, stepFocus = 0, defFocus = 0, focusCaps = 0;
            if (SUCCEEDED(camCtrl->GetRange(CameraControl_Focus, &minFocus, &maxFocus, &stepFocus, &defFocus, &focusCaps))) {
                if ((focusCaps & CameraControl_Flags_Auto) != 0) {
                    camCtrl->Set(CameraControl_Focus, defFocus, CameraControl_Flags_Auto);
                }
            }
        }
    }

    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 2);
    // Enables automatic MJPG / YUY2 -> NV12 decoding inside IMFSourceReader for USB 1080p webcams!
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(mediaSource.Get(), readerAttrs.Get(), &reader)) || !reader) {
        mediaSource->Shutdown();
        cameraActive_.store(false);
        return;
    }

    // Select best landscape native format up to 1920x1080 @ ~30fps
    UINT32 bestW = 0, bestH = 0;
    DWORD bestNativeIdx = 0;
    bool foundNative = false;
    for (DWORD idx = 0;; ++idx) {
        ComPtr<IMFMediaType> nativeMt;
        if (FAILED(reader->GetNativeMediaType(kMfFirstVideoStream, idx, &nativeMt)) || !nativeMt) {
            break;
        }
        UINT32 w = 0, h = 0, fpsNum = 30, fpsDen = 1;
        MFGetAttributeSize(nativeMt.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(nativeMt.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        double fps = fpsDen > 0 ? static_cast<double>(fpsNum) / static_cast<double>(fpsDen) : 30.0;
        if (w >= h && w <= 1920 && h <= 1080 && fps >= 24.0) {
            if (w * h >= bestW * bestH) {
                bestW = w;
                bestH = h;
                bestNativeIdx = idx;
                foundNative = true;
            }
        }
    }
    if (foundNative) {
        ComPtr<IMFMediaType> bestMt;
        if (SUCCEEDED(reader->GetNativeMediaType(kMfFirstVideoStream, bestNativeIdx, &bestMt))) {
            reader->SetCurrentMediaType(kMfFirstVideoStream, nullptr, bestMt.Get());
        }
    }

    // Request NV12 output first (zero-copy into luma motion detector + H.264 encoder), fallback to RGB32
    bool outputIsNV12 = false;
    {
        ComPtr<IMFMediaType> reqNv12;
        MFCreateMediaType(&reqNv12);
        reqNv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        reqNv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(reader->SetCurrentMediaType(kMfFirstVideoStream, nullptr, reqNv12.Get()))) {
            outputIsNV12 = true;
        } else {
            ComPtr<IMFMediaType> reqRgb;
            MFCreateMediaType(&reqRgb);
            reqRgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            reqRgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (FAILED(reader->SetCurrentMediaType(kMfFirstVideoStream, nullptr, reqRgb.Get()))) {
                mediaSource->Shutdown();
                cameraActive_.store(false);
                return;
            }
        }
    }

    UINT32 actualW = 1280, actualH = 720;
    INT32 defaultStride = 0;
    {
        ComPtr<IMFMediaType> curMt;
        if (SUCCEEDED(reader->GetCurrentMediaType(kMfFirstVideoStream, &curMt)) && curMt) {
            MFGetAttributeSize(curMt.Get(), MF_MT_FRAME_SIZE, &actualW, &actualH);
            UINT32 rawStride = 0;
            if (SUCCEEDED(curMt->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
                defaultStride = static_cast<INT32>(rawStride);
            }
        }
    }
    actualW &= ~1U;
    actualH &= ~1U;
    if (defaultStride == 0) {
        defaultStride = outputIsNV12 ? static_cast<INT32>(actualW) : static_cast<INT32>(actualW * 4);
    }

    activeWidth_.store(static_cast<int>(actualW));
    activeHeight_.store(static_cast<int>(actualH));
    cameraActive_.store(true);
    cameraPermissionDenied_.store(false);

    {
        std::lock_guard<std::mutex> lock(summaryMutex_);
        wchar_t buf[256];
        std::swprintf(
            buf, 256, L"%ls (%ux%u) \x2022 %ls",
            chosen.friendlyName.c_str(), actualW, actualH,
            sampleAudioEnabled_.load() ? L"48 kHz AAC Audio" : L"Audio Off"
        );
        activeSummary_ = buf;
    }

    uint64_t frameCounter = 0;
    std::vector<uint8_t> scratchNV12;

    while (running_.load()) {
        DWORD streamIdx = 0, flags = 0;
        LONGLONG llTimestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(
            kMfFirstVideoStream,
            0,
            &streamIdx,
            &flags,
            &llTimestamp,
            &sample
        );
        if (FAILED(hr)) {
            Sleep(20);
            continue;
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) || !sample) {
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) continue;

        BYTE* rawPtr = nullptr;
        DWORD curLen = 0;
        if (FAILED(buffer->Lock(&rawPtr, nullptr, &curLen)) || !rawPtr) continue;

        int w = static_cast<int>(actualW);
        int h = static_cast<int>(actualH);

        const uint8_t* yPlane = nullptr;
        const uint8_t* uvPlane = nullptr;
        int yStride = w;
        int uvStride = w;

        std::vector<uint8_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

        if (outputIsNV12) {
            int s = std::abs(defaultStride);
            if (curLen >= static_cast<DWORD>(s * h * 3 / 2)) {
                yPlane = rawPtr;
                uvPlane = rawPtr + s * h;
                yStride = s;
                uvStride = s;
                convertNV12ToBGRA(yPlane, yStride, uvPlane, uvStride, w, h, bgra.data(), w * 4);
            }
        } else {
            int pitch = defaultStride;
            int absPitch = std::abs(pitch);
            if (curLen >= static_cast<DWORD>(absPitch * h)) {
                const uint8_t* topRow = (pitch < 0) ? (rawPtr + absPitch * (h - 1)) : rawPtr;
                for (int r = 0; r < h; ++r) {
                    const uint8_t* srcRow = topRow + r * pitch;
                    uint8_t* dstRow = bgra.data() + r * w * 4;
                    std::memcpy(dstRow, srcRow, w * 4);
                    for (int x = 0; x < w; ++x) dstRow[x * 4 + 3] = 255;
                }
                scratchNV12.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3 / 2);
                uint8_t* dstY = scratchNV12.data();
                uint8_t* dstUV = scratchNV12.data() + w * h;
                convertBGRAToNV12(bgra.data(), w * 4, w, h, dstY, w, dstUV, w);
                yPlane = dstY;
                uvPlane = dstUV;
                yStride = w;
                uvStride = w;
            }
        }

        if (yPlane && uvPlane) {
            routeCapturedVideoFrame(yPlane, yStride, uvPlane, uvStride, w, h, llTimestamp);

            frameCounter++;
            if (frameCounter % 3 == 0) {
                auto ev = motionDetector_.analyzeNV12Luma(yPlane, yStride, w, h, monotonicNowSeconds());
                latestMotionPct_.store(ev.scorePercent);
                latestMeanLuma_.store(ev.meanLuma);
                if (ev.triggered && onMotionTriggered) {
                    onMotionTriggered();
                }
            }
        }

        buffer->Unlock();

        {
            std::lock_guard<std::mutex> pLock(previewMutex_);
            latestPreview_.width = w;
            latestPreview_.height = h;
            latestPreview_.ptsSeconds = static_cast<double>(llTimestamp) / 10000000.0;
            latestPreview_.sequence = ++previewSeq_;
            latestPreview_.pixels = std::move(bgra);
        }
    }

    reader.Reset();
    mediaSource->Shutdown();
}

void CaptureEngine::audioCaptureLoop(std::wstring audioDeviceId) {
    ScopedComThread comScope;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        micActive_.store(false);
        return;
    }

    ComPtr<IMMDevice> device;
    if (!audioDeviceId.empty()) {
        enumerator->GetDevice(audioDeviceId.c_str(), &device);
    }
    if (!device) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device))) {
            enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
    }
    if (!device) {
        micActive_.store(false);
        return;
    }

    ComPtr<IAudioClient> audioClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()))) || !audioClient) {
        micActive_.store(false);
        return;
    }

    // Request normalized 48 kHz 16-bit Signed Mono PCM via Windows Audio Engine resampler!
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = 96000;
    wfx.cbSize = 0;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 500000, 0, &wfx, nullptr))) {
        micActive_.store(false);
        return;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    if (FAILED(audioClient->GetService(IID_PPV_ARGS(&captureClient))) || !captureClient) {
        micActive_.store(false);
        return;
    }

    if (FAILED(audioClient->Start())) {
        micActive_.store(false);
        return;
    }
    micActive_.store(true);

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);

    while (audioRunning_.load() && running_.load()) {
        UINT32 packetFrames = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
        if (packetFrames == 0) {
            Sleep(8);
            continue;
        }

        while (packetFrames > 0 && audioRunning_.load()) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD pktFlags = 0;
            UINT64 qpcPos100ns = 0;
            if (FAILED(captureClient->GetBuffer(&data, &numFrames, &pktFlags, nullptr, &qpcPos100ns))) {
                break;
            }

            std::vector<int16_t> samples(numFrames, 0);
            if (!(pktFlags & AUDCLNT_BUFFERFLAGS_SILENT) && data && numFrames > 0) {
                std::memcpy(samples.data(), data, numFrames * sizeof(int16_t));
            }
            captureClient->ReleaseBuffer(numFrames);

            if (sampleAudioEnabled_.load() && !samples.empty()) {
                int64_t pts = static_cast<int64_t>(qpcPos100ns);
                if (pts <= 0) {
                    LARGE_INTEGER nowQpc{};
                    QueryPerformanceCounter(&nowQpc);
                    pts = (nowQpc.QuadPart * 10000000LL) / qpcFreq.QuadPart;
                }
                routeCapturedAudioChunk(samples.data(), samples.size(), pts);

                auto soundEv = soundDetector_.analyzePCM16Mono(
                    samples.data(),
                    samples.size(),
                    48000,
                    monotonicNowSeconds()
                );
                latestAudioDb_.store(std::clamp(soundEv.filteredDB, -60.0f, 0.0f));
                if (soundEv.triggered && onSoundTriggered) {
                    onSoundTriggered();
                }
                secureZeroPcm(samples);
            } else {
                latestAudioDb_.store(-60.0f);
                secureZeroPcm(samples);
            }

            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
        }
    }

    audioClient->Stop();
    micActive_.store(false);
}

// MARK: - VaultPlayerEngine (Zero-Disk-Plaintext RAM Player + 1x/4x/12x + Fast Ball Scrubbing)

VaultPlayerEngine::VaultPlayerEngine() {
    ensureMediaFoundationInitialized();
}

VaultPlayerEngine::~VaultPlayerEngine() {
    unloadClip();
}

bool VaultPlayerEngine::loadClip(std::shared_ptr<VaultClipReader> reader, std::string* outError) {
    unloadClip();
    if (!reader || reader->totalPlaintextLength() <= 0) {
        if (outError) *outError = "Clip reader is empty.";
        return false;
    }

    // Quickly validate stream & measure exact duration before starting worker thread
    reader->ensureStandardMp4Layout();
    ComPtr<IMFByteStream> testBs;
    testBs.Attach(new VaultMemoryByteStream(reader));

    ComPtr<IMFAttributes> srAttrs;
    MFCreateAttributes(&srAttrs, 2);
    srAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> sr;
    HRESULT hr = MFCreateSourceReaderFromByteStream(testBs.Get(), srAttrs.Get(), &sr);
    if (FAILED(hr) || !sr) {
        if (outError) *outError = "MFCreateSourceReaderFromByteStream failed on encrypted RAM stream.";
        return false;
    }

    double dur = std::max(0.2, reader->standardMp4DurationSeconds());
    PROPVARIANT pvDur;
    PropVariantInit(&pvDur);
    if (SUCCEEDED(sr->GetPresentationAttribute(kMfMediaSource, MF_PD_DURATION, &pvDur))) {
        if (pvDur.vt == VT_UI8 && pvDur.uhVal.QuadPart > 0) {
            double mfDur = static_cast<double>(pvDur.uhVal.QuadPart) / 10000000.0;
            if (mfDur > 0.05) dur = std::max(dur, mfDur);
        }
        PropVariantClear(&pvDur);
    }
    sr.Reset();
    testBs.Reset();

    durationSec_.store(dur);
    currentSec_.store(0.0);
    isPlaying_.store(false);
    initialFrameReady_.store(false);
    hasPendingScrub_.store(true);
    pendingScrubSec_.store(0.0); // Decode initial frame at t = 0.0 immediately!
    workerStop_.store(false);
    clipLoaded_.store(true);

    playerThread_ = std::thread(&VaultPlayerEngine::playerWorkerLoop, this, std::move(reader));

    // Wait up to 4.0s for the initial frame at t = 0.0 to be decoded into RAM so callers see an immediate frame
    {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wakeCv_.wait_for(lock, std::chrono::milliseconds(4000), [this]() {
            return initialFrameReady_.load() || workerStop_.load();
        });
    }
    return true;
}

void VaultPlayerEngine::unloadClip() {
    isPlaying_.store(false);
    workerStop_.store(true);
    wakeCv_.notify_all();
    if (playerThread_.joinable()) {
        playerThread_.join();
    }
    clipLoaded_.store(false);
    initialFrameReady_.store(false);
    currentSec_.store(0.0);
    durationSec_.store(0.0);
    {
        std::lock_guard<std::mutex> fLock(frameMutex_);
        crypto::secureZeroVec(currentFrame_.pixels);
        currentFrame_ = BgraFrame{};
    }
}

void VaultPlayerEngine::play(float speedRate) {
    speedRate_.store(std::clamp(speedRate, 0.25f, 16.0f));
    isPlaying_.store(true);
    wakeCv_.notify_all();
}

void VaultPlayerEngine::pause() {
    isPlaying_.store(false);
    wakeCv_.notify_all();
}

void VaultPlayerEngine::setSpeedRate(float speedRate) {
    speedRate_.store(std::clamp(speedRate, 0.25f, 16.0f));
    wakeCv_.notify_all();
}

void VaultPlayerEngine::scrubToSeconds(double targetSeconds) {
    double clamped = std::clamp(targetSeconds, 0.0, std::max(0.0, durationSec_.load()));
    pendingScrubSec_.store(clamped);
    hasPendingScrub_.store(true);
    wakeCv_.notify_all();
}

bool VaultPlayerEngine::copyLatestVideoFrame(BgraFrame& outFrame) const {
    std::lock_guard<std::mutex> fLock(frameMutex_);
    if (currentFrame_.pixels.empty() || currentFrame_.width <= 0 || currentFrame_.height <= 0) {
        return false;
    }
    if (outFrame.sequence == currentFrame_.sequence) {
        return true;
    }
    outFrame = currentFrame_;
    return true;
}

void VaultPlayerEngine::playerWorkerLoop(std::shared_ptr<VaultClipReader> reader) {
    ScopedComThread comScope;

    ComPtr<IMFByteStream> memStream;
    memStream.Attach(new VaultMemoryByteStream(reader));

    ComPtr<IMFAttributes> srAttrs;
    MFCreateAttributes(&srAttrs, 2);
    srAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromByteStream(memStream.Get(), srAttrs.Get(), &sr)) || !sr) {
        return;
    }

    sr->SetStreamSelection(kMfAllStreams, FALSE);
    sr->SetStreamSelection(kMfFirstVideoStream, TRUE);

    ComPtr<IMFMediaType> vidOut;
    MFCreateMediaType(&vidOut);
    vidOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vidOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(sr->SetCurrentMediaType(kMfFirstVideoStream, nullptr, vidOut.Get()))) {
        return;
    }

    const UINT32 headerW = static_cast<UINT32>(std::max(16, static_cast<int>(reader->header().width)));
    const UINT32 headerH = static_cast<UINT32>(std::max(16, static_cast<int>(reader->header().height)));
    UINT32 surfW = headerW;
    UINT32 surfH = headerH;
    INT32 vidStride = 0;

    auto refreshVideoOutputFormat = [&]() {
        ComPtr<IMFMediaType> curVid;
        if (SUCCEEDED(sr->GetCurrentMediaType(kMfFirstVideoStream, &curVid)) && curVid) {
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(curVid.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                surfW = w;
                surfH = h;
            }
            UINT32 rawStride = 0;
            if (SUCCEEDED(curVid->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)) && rawStride != 0) {
                vidStride = static_cast<INT32>(rawStride);
            } else {
                LONG calcStride = 0;
                if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, surfW, &calcStride)) && calcStride != 0) {
                    vidStride = static_cast<INT32>(calcStride);
                }
            }
        }
        if (vidStride == 0) vidStride = static_cast<INT32>(surfW * 4);
    };
    refreshVideoOutputFormat();

    // Optional Audio Stream + WASAPI Audio Render Client
    bool hasAudioStream = false;
    UINT32 audioChannels = 1;
    if (reader->header().hasAudio()) {
        if (SUCCEEDED(sr->SetStreamSelection(kMfFirstAudioStream, TRUE))) {
            ComPtr<IMFMediaType> audOut;
            MFCreateMediaType(&audOut);
            audOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            audOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            audOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            audOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
            audOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
            audOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 96000);
            if (SUCCEEDED(sr->SetCurrentMediaType(kMfFirstAudioStream, nullptr, audOut.Get()))) {
                hasAudioStream = true;
                audioChannels = 1;
            } else {
                audOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                audOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                audOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
                if (SUCCEEDED(sr->SetCurrentMediaType(kMfFirstAudioStream, nullptr, audOut.Get()))) {
                    hasAudioStream = true;
                    audioChannels = 2;
                } else {
                    sr->SetStreamSelection(kMfFirstAudioStream, FALSE);
                }
            }
        }
    }

    ComPtr<IAudioClient> renderAudioClient;
    ComPtr<IAudioRenderClient> renderClient;
    UINT32 renderBufFrames = 0;
    if (hasAudioStream) {
        ComPtr<IMMDeviceEnumerator> enumDev;
        ComPtr<IMMDevice> outDev;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumDev))) &&
            SUCCEEDED(enumDev->GetDefaultAudioEndpoint(eRender, eMultimedia, &outDev)) && outDev) {
            if (SUCCEEDED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(renderAudioClient.GetAddressOf())))) {
                WAVEFORMATEX wfx{};
                wfx.wFormatTag = WAVE_FORMAT_PCM;
                wfx.nChannels = static_cast<WORD>(audioChannels);
                wfx.nSamplesPerSec = 48000;
                wfx.wBitsPerSample = 16;
                wfx.nBlockAlign = static_cast<WORD>(audioChannels * 2);
                wfx.nAvgBytesPerSec = 48000 * wfx.nBlockAlign;
                DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                if (SUCCEEDED(renderAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wfx, nullptr)) &&
                    SUCCEEDED(renderAudioClient->GetBufferSize(&renderBufFrames)) &&
                    SUCCEEDED(renderAudioClient->GetService(IID_PPV_ARGS(&renderClient)))) {
                    renderAudioClient->Start();
                } else {
                    renderClient.Reset();
                    renderAudioClient.Reset();
                }
            }
        }
        if (!renderClient) {
            sr->SetStreamSelection(kMfFirstAudioStream, FALSE);
            hasAudioStream = false;
        }
    }

    auto publishVideoSample = [&](IMFSample* smp, LONGLONG ts100ns) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(smp->ConvertToContiguousBuffer(&buf)) || !buf) return;
        BYTE* raw = nullptr;
        DWORD cb = 0;
        if (FAILED(buf->Lock(&raw, nullptr, &cb)) || !raw) return;

        int fullW = static_cast<int>(surfW);
        int fullH = static_cast<int>(surfH);
        int outW = std::min(fullW, static_cast<int>(headerW));
        int outH = std::min(fullH, static_cast<int>(headerH));
        int pitch = vidStride;
        int absPitch = std::abs(pitch);
        if (outW > 0 && outH > 0 && cb >= static_cast<DWORD>(absPitch * fullH)) {
            std::vector<uint8_t> pixels(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
            const uint8_t* top = (pitch < 0) ? (raw + absPitch * (fullH - 1)) : raw;
            for (int r = 0; r < outH; ++r) {
                const uint8_t* srcRow = top + r * pitch;
                uint8_t* dstRow = pixels.data() + r * outW * 4;
                std::memcpy(dstRow, srcRow, static_cast<size_t>(outW) * 4);
                for (int x = 0; x < outW; ++x) dstRow[x * 4 + 3] = 255;
            }
            double sec = std::max(0.0, static_cast<double>(ts100ns) / 10000000.0);
            {
                std::lock_guard<std::mutex> fLock(frameMutex_);
                currentFrame_.width = outW;
                currentFrame_.height = outH;
                currentFrame_.ptsSeconds = sec;
                currentFrame_.sequence = ++frameSeq_;
                currentFrame_.pixels = std::move(pixels);
            }
            currentSec_.store(sec);
            if (sec > durationSec_.load()) durationSec_.store(sec);
            if (!initialFrameReady_.exchange(true)) {
                wakeCv_.notify_all();
            }
        }
        buf->Unlock();
    };

    auto seekAndDecodeExact = [&](double targetSec) {
        LONGLONG target100ns = static_cast<LONGLONG>(std::max(0.0, targetSec) * 10000000.0);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = target100ns;
        sr->Flush(kMfAllStreams);
        sr->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);

        // Decode forward from nearest preceding keyframe right up to target100ns
        ComPtr<IMFSample> bestSample;
        LONGLONG bestTs = target100ns;
        for (int step = 0; step < 90; ++step) {
            if (hasPendingScrub_.load()) break; // Newer drag target arrived -> chase newer target immediately!
            DWORD sIdx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            HRESULT hr = sr->ReadSample(kMfFirstVideoStream, 0, &sIdx, &flags, &ts, &smp);
            if (FAILED(hr)) break;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                refreshVideoOutputFormat();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!smp) continue;
            bestSample = smp;
            bestTs = ts;
            if (ts + 150000LL >= target100ns) break;
        }
        if (bestSample) {
            publishVideoSample(bestSample.Get(), bestTs);
        } else {
            currentSec_.store(targetSec);
        }
    };

    bool clockAnchored = false;
    double anchorWallSec = 0.0;
    double anchorMediaSec = 0.0;
    float anchorSpeed = 1.0f;

    while (!workerStop_.load()) {
        if (hasPendingScrub_.exchange(false)) {
            double target = pendingScrubSec_.load();
            seekAndDecodeExact(target);
            clockAnchored = false;
            continue;
        }

        if (!isPlaying_.load()) {
            clockAnchored = false;
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wakeCv_.wait_for(lock, std::chrono::milliseconds(30), [&]() {
                return workerStop_.load() || isPlaying_.load() || hasPendingScrub_.load();
            });
            continue;
        }

        float spd = speedRate_.load();
        if (!clockAnchored || std::abs(spd - anchorSpeed) > 0.01f) {
            anchorWallSec = monotonicNowSeconds();
            anchorMediaSec = currentSec_.load();
            anchorSpeed = spd;
            clockAnchored = true;
        }

        DWORD streamIdx = 0, flags = 0;
        LONGLONG ts100ns = 0;
        ComPtr<IMFSample> sample;
        DWORD streamToRead = (hasAudioStream && renderClient && spd <= 4.0f && !isMuted_.load())
            ? static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM)
            : static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

        HRESULT hr = sr->ReadSample(streamToRead, 0, &streamIdx, &flags, &ts100ns, &sample);
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && streamIdx == 0) {
            refreshVideoOutputFormat();
        }
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            isPlaying_.store(false);
            clockAnchored = false;
            if (onClipEnded && !workerStop_.load()) {
                onClipEnded();
            }
            continue;
        }
        if (!sample) continue;

        if (streamIdx == 0) { // Video stream
            double sampleSec = std::max(0.0, static_cast<double>(ts100ns) / 10000000.0);
            double desiredWallSec = anchorWallSec + (sampleSec - anchorMediaSec) / static_cast<double>(anchorSpeed);
            double nowWall = monotonicNowSeconds();

            if (desiredWallSec > nowWall + 0.002) {
                double waitSec = std::min(0.050, desiredWallSec - nowWall);
                std::unique_lock<std::mutex> lock(wakeMutex_);
                wakeCv_.wait_for(lock, std::chrono::duration<double>(waitSec), [&]() {
                    return workerStop_.load() || !isPlaying_.load() || hasPendingScrub_.load() ||
                           std::abs(speedRate_.load() - anchorSpeed) > 0.01f;
                });
                if (workerStop_.load() || hasPendingScrub_.load() || !isPlaying_.load()) continue;
            }
            publishVideoSample(sample.Get(), ts100ns);
        } else if (renderClient && spd <= 4.0f && !isMuted_.load()) { // Audio stream (1x normal or 4x varispeed)
            ComPtr<IMFMediaBuffer> aBuf;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&aBuf)) && aBuf) {
                BYTE* p = nullptr;
                DWORD cb = 0;
                if (SUCCEEDED(aBuf->Lock(&p, nullptr, &cb)) && p && cb >= audioChannels * 2) {
                    UINT32 inFrames = cb / (audioChannels * 2);
                    int strideStep = (spd > 2.0f) ? 4 : 1; // 4x varispeed decimation
                    UINT32 outFrames = inFrames / strideStep;
                    UINT32 padding = 0;
                    if (outFrames > 0 && SUCCEEDED(renderAudioClient->GetCurrentPadding(&padding)) &&
                        padding + outFrames <= renderBufFrames) {
                        BYTE* dst = nullptr;
                        if (SUCCEEDED(renderClient->GetBuffer(outFrames, &dst)) && dst) {
                            const int16_t* s16 = reinterpret_cast<const int16_t*>(p);
                            int16_t* d16 = reinterpret_cast<int16_t*>(dst);
                            if (strideStep == 1) {
                                std::memcpy(d16, s16, outFrames * audioChannels * 2);
                            } else {
                                for (UINT32 f = 0; f < outFrames; ++f) {
                                    for (UINT32 ch = 0; ch < audioChannels; ++ch) {
                                        int sum = 0;
                                        for (int k = 0; k < strideStep; ++k) {
                                            sum += s16[(f * strideStep + k) * audioChannels + ch];
                                        }
                                        d16[f * audioChannels + ch] = static_cast<int16_t>(sum / strideStep);
                                    }
                                }
                            }
                            renderClient->ReleaseBuffer(outFrames, 0);
                        }
                    }
                    aBuf->Unlock();
                }
            }
        }
    }

    if (renderAudioClient) renderAudioClient->Stop();
    sr.Reset();
    memStream.Reset();
}

#endif // _WIN32

} // namespace maxwatch
