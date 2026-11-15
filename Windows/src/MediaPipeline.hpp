#pragma once

#include "Fmp4Muxer.hpp"
#include "TimestampStamper.hpp"
#include "VaultCrypto.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace maxwatch {

struct DeviceInfo {
    std::wstring id;
    std::wstring friendlyName;
};

struct BgraFrame {
    int width = 0;
    int height = 0;
    double ptsSeconds = 0.0;
    uint64_t sequence = 0;
    std::vector<uint8_t> pixels; // width * height * 4 (BGRA32)
};

// Studio Motion Detector (exact match to macOS AppController luma-grid & sensitivity curve)
class MotionDetector {
public:
    MotionDetector();

    void setSensitivityPercent(double pct);
    double sensitivityPercent() const { return sensitivityPercent_.load(); }
    double triggerThresholdPercent() const { return thresholdPercent_.load(); }
    std::wstring sensitivityLabelWide() const;

    void resetWarmup(double warmupSeconds = 1.1);

    struct EvalResult {
        double scorePercent = 0.0;
        bool triggered = false;
    };

    EvalResult analyzeNV12Luma(
        const uint8_t* planeY,
        int strideY,
        int width,
        int height,
        double nowMonotonicSec
    );

private:
    std::atomic<double> sensitivityPercent_{78.0};
    std::atomic<double> thresholdPercent_{1.50};

    std::mutex mutex_;
    std::vector<uint8_t> prevGrid_;
    double prevMean_ = 0.0;
    int consecutiveHits_ = 0;
    double warmupUntilSec_ = 0.0;
};

// Color conversion utilities
void convertNV12ToBGRA(
    const uint8_t* planeY, int strideY,
    const uint8_t* planeUV, int strideUV,
    int width, int height,
    uint8_t* dstBGRA, int dstStrideBGRA
);

void convertBGRAToNV12(
    const uint8_t* srcBGRA, int srcStrideBGRA,
    int width, int height,
    uint8_t* dstY, int dstStrideY,
    uint8_t* dstUV, int dstStrideUV
);

#ifdef _WIN32

void ensureMediaFoundationInitialized();

// Real-Time 1-Second fMP4 -> AES-256-GCM Encrypted Segment Writer (Windows Media Foundation H.264 + AAC)
class VaultSegmentWriter {
public:
    VaultSegmentWriter(
        std::unique_ptr<VaultClipFileWriter> clipWriter,
        int width,
        int height,
        bool hasAudio
    );
    ~VaultSegmentWriter();

    VaultSegmentWriter(const VaultSegmentWriter&) = delete;
    VaultSegmentWriter& operator=(const VaultSegmentWriter&) = delete;

    const std::wstring& filePath() const { return clipWriter_->filePath(); }
    int width() const { return width_; }
    int height() const { return height_; }
    bool hasAudio() const { return hasAudio_; }

    int flushedSegmentCount() const { return clipWriter_->writtenChunkCount(); }
    double recordedDurationSeconds() const { return recordedDurationSec_.load(); }

    void appendNV12Frame(
        const uint8_t* planeY, int strideY,
        const uint8_t* planeUV, int strideUV,
        int srcWidth, int srcHeight,
        int64_t pts100ns,
        int64_t captureUnixMs = -1
    );

    void appendAudioPCM16Mono48k(
        const int16_t* samples,
        size_t sampleCount,
        int64_t pts100ns
    );

    void stopAndFinalizeSynchronously(double timeoutSeconds = 3.0);

    // Abandons the background encoder immediately without writing a final trailer or setting cleanlyFinalized,
    // used exclusively by --selftest to simulate sudden hard power loss mid-recording!
    void abandonWithoutFinalizingForPowerCutTest();

private:
    struct QueuedVideo {
        std::vector<uint8_t> nv12; // tight width_ * height_ * 3 / 2
        int64_t pts100ns = 0;
    };
    struct QueuedAudio {
        std::vector<int16_t> pcm;
        int64_t pts100ns = 0;
    };

    void workerLoop();

    std::unique_ptr<VaultClipFileWriter> clipWriter_;
    int width_ = 1280;
    int height_ = 720;
    bool hasAudio_ = true;

    FrameTimestampStamper stamper_;
    std::mutex stampMutex_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueuedVideo> videoQueue_;
    std::deque<QueuedAudio> audioQueue_;
    bool stopRequested_ = false;
    bool abandonRequested_ = false;
    bool workerFinished_ = false;
    std::condition_variable doneCv_;

    std::atomic<double> recordedDurationSec_{0.0};
    std::thread workerThread_;
};

// Live Webcam + WASAPI Microphone Capture Engine
class CaptureEngine {
public:
    CaptureEngine();
    ~CaptureEngine();

    static std::vector<DeviceInfo> enumerateVideoDevices();
    static std::vector<DeviceInfo> enumerateAudioDevices();

    void startCapture(
        const std::wstring& preferredVideoId,
        const std::wstring& preferredAudioId,
        bool recordAudioEnabled
    );
    void stopCapture();

    // Synthetic test camera for headless CI UI verification when no hardware UVC camera is attached
    void startSyntheticCameraForSmokeTest(bool recordAudioEnabled);

    bool isCameraOpen() const { return cameraActive_.load(); }
    bool isCameraPermissionDenied() const { return cameraPermissionDenied_.load(); }
    bool isMicOpen() const { return micActive_.load(); }
    int activeWidth() const { return activeWidth_.load(); }
    int activeHeight() const { return activeHeight_.load(); }
    std::wstring activeSummaryWide() const;

    bool copyLatestPreviewFrame(BgraFrame& outFrame) const;
    double latestMotionPercent() const { return latestMotionPct_.load(); }
    float latestAudioDb() const { return latestAudioDb_.load(); }

    MotionDetector& motionDetector() { return motionDetector_; }

    void attachActiveSegmentWriter(std::shared_ptr<VaultSegmentWriter> writer);
    std::shared_ptr<VaultSegmentWriter> detachActiveSegmentWriter();
    std::shared_ptr<VaultSegmentWriter> currentSegmentWriter() const;

    void setSampleAudioEnabled(bool enabled) { sampleAudioEnabled_.store(enabled); }

    std::function<void()> onMotionTriggered;

private:
    void videoCaptureLoop(std::wstring videoDeviceId, bool syntheticMode);
    void audioCaptureLoop(std::wstring audioDeviceId);

    MotionDetector motionDetector_;

    std::atomic<bool> running_{false};
    std::atomic<bool> audioRunning_{false};
    std::atomic<bool> cameraActive_{false};
    std::atomic<bool> cameraPermissionDenied_{false};
    std::atomic<bool> micActive_{false};
    std::atomic<bool> sampleAudioEnabled_{true};
    std::atomic<int> activeWidth_{1280};
    std::atomic<int> activeHeight_{720};
    std::atomic<double> latestMotionPct_{0.0};
    std::atomic<float> latestAudioDb_{-60.0f};

    mutable std::mutex summaryMutex_;
    std::wstring activeSummary_ = L"Initializing camera...";

    mutable std::mutex previewMutex_;
    BgraFrame latestPreview_;
    uint64_t previewSeq_ = 0;

    mutable std::mutex writerMutex_;
    std::shared_ptr<VaultSegmentWriter> activeWriter_;

    std::thread videoThread_;
    std::thread audioThread_;
};

// Zero-Disk-Plaintext RAM Player + Zero-Lag Ball Scrub Chase Engine (1x / 4x / 12x)
class VaultPlayerEngine {
public:
    VaultPlayerEngine();
    ~VaultPlayerEngine();

    bool loadClip(std::shared_ptr<VaultClipReader> reader, std::string* outError = nullptr);
    void unloadClip();

    bool hasClipLoaded() const { return clipLoaded_.load(); }
    double durationSeconds() const { return durationSec_.load(); }
    double currentSeconds() const { return currentSec_.load(); }
    bool isPlaying() const { return isPlaying_.load(); }

    void play(float speedRate);
    void pause();
    void setSpeedRate(float speedRate);
    float speedRate() const { return speedRate_.load(); }

    void setMuted(bool muted) { isMuted_.store(muted); }
    bool isMuted() const { return isMuted_.load(); }

    // Zero-Lag Scrub Chase Engine: coalesces rapid drag updates onto latest target timestamp
    void scrubToSeconds(double targetSeconds);

    bool copyLatestVideoFrame(BgraFrame& outFrame) const;

    std::function<void()> onClipEnded;

private:
    void playerWorkerLoop(std::shared_ptr<VaultClipReader> reader);

    std::atomic<bool> workerStop_{false};
    std::atomic<bool> clipLoaded_{false};
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> isMuted_{false};
    std::atomic<float> speedRate_{1.0f};
    std::atomic<double> durationSec_{0.0};
    std::atomic<double> currentSec_{0.0};

    std::atomic<bool> hasPendingScrub_{false};
    std::atomic<double> pendingScrubSec_{0.0};

    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;

    mutable std::mutex frameMutex_;
    BgraFrame currentFrame_;
    uint64_t frameSeq_ = 0;

    std::thread playerThread_;
};

#endif // _WIN32

} // namespace maxwatch
