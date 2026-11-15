#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace maxwatch {

class ClipTimestampFormatter {
public:
    // Auto-detects whether the user's Windows system clock uses 12-hour (AM/PM) or 24-hour format
    static bool systemUses12HourClock();

    // Example: "15 Nov 2026 • 12:15:34 AM PST" or "15 Nov 2026 • 00:15:34 CET"
    // Never uses ambiguous numeric slashes ("/"), always spells out 3-letter English month + 4-digit year.
    static std::string formatUtf8(int64_t unixMs = -1);
    static std::wstring formatWide(int64_t unixMs = -1);

    // Example: "15 Nov 2026, 12:15:34 AM" or "15 Nov 2026, 00:15:34"
    static std::string formatCompactUtf8(int64_t unixMs = -1);
    static std::wstring formatCompactWide(int64_t unixMs = -1);

    // Explicit 12h / 24h formatting helper for deterministic self-tests
    static std::string formatExplicitUtf8(int64_t unixMs, bool use12Hour);
};

// 1-Hz Cached NV12 Bottom-Left Timestamp Stamper
// Mirrors macOS FrameTimestampStamper: renders a dark translucent rounded pill + crisp antialiased
// monospace-digit timestamp, pre-converts to BT.709 Video-Range [Y, Cb, Cr, Alpha], and blends
// exclusively onto the bottom-left corner of NV12 video frames.
class FrameTimestampStamper {
public:
    FrameTimestampStamper() = default;

    void stampBottomLeftNV12(
        uint8_t* planeY,
        int strideY,
        uint8_t* planeUV,
        int strideUV,
        int frameWidth,
        int frameHeight,
        int64_t unixMs = -1
    );

private:
    void rebuildStamp(const std::wstring& text, int frameW, int frameH);

    int64_t cachedSecond_ = INT64_MIN;
    int cachedFrameW_ = 0;
    int cachedFrameH_ = 0;
    int stampW_ = 0;
    int stampH_ = 0;
    // 4 bytes per pixel: [Y, Cb, Cr, Alpha] in BT.709 Video Range
    std::vector<uint8_t> stampYCbCrA_;
};

} // namespace maxwatch
