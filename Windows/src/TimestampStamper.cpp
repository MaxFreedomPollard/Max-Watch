#include "TimestampStamper.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
using Microsoft::WRL::ComPtr;
#endif

namespace maxwatch {

namespace {

int64_t nowUnixMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::wstring utf8ToWideLocal(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string computeShortTimezoneAbbrev(const std::tm& localTm, std::time_t epochSec) {
#ifdef _WIN32
    DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
    DWORD tzState = GetDynamicTimeZoneInformation(&dtzi);
    const wchar_t* tzName = (tzState == TIME_ZONE_ID_DAYLIGHT && dtzi.DaylightName[0] != L'\0')
        ? dtzi.DaylightName
        : dtzi.StandardName;

    // Map common Windows timezone names ("Pacific Standard Time" -> "PST", "Pacific Daylight Time" -> "PDT",
    // "Eastern Standard Time" -> "EST", "Central European Standard Time" -> "CEST"/"CET", etc.)
    // If already short (<= 5 ASCII chars), use as-is; if multi-word English "... Time", take uppercase initials;
    // otherwise format clean UTC offset (e.g. "UTC+02" or "UTC-05") so non-Latin Windows locales never corrupt timestamps!
    if (tzName && tzName[0] != L'\0') {
        size_t wlen = std::wcslen(tzName);
        bool allAscii = true;
        for (size_t i = 0; i < wlen; ++i) {
            if (tzName[i] < 0x20 || tzName[i] > 0x7E) { allAscii = false; break; }
        }
        if (allAscii && wlen <= 5) {
            std::string direct;
            for (size_t i = 0; i < wlen; ++i) direct.push_back(static_cast<char>(tzName[i]));
            return direct;
        }
        if (allAscii) {
            std::string initials;
            bool startOfWord = true;
            for (size_t i = 0; i < wlen; ++i) {
                char c = static_cast<char>(tzName[i]);
                if (c == ' ' || c == '-') {
                    startOfWord = true;
                } else if (startOfWord) {
                    if (c >= 'A' && c <= 'Z') initials.push_back(c);
                    startOfWord = false;
                }
            }
            if (initials.size() >= 2 && initials.size() <= 5) {
                return initials;
            }
        }
    }

    // Fallback: exact computer UTC offset (e.g. UTC-08 or UTC+05:30)
    std::tm gmtTm{};
    gmtime_s(&gmtTm, &epochSec);
    // Compute offset minutes between local and UTC
    LONG totalBiasMin = dtzi.Bias;
    if (tzState == TIME_ZONE_ID_DAYLIGHT) totalBiasMin += dtzi.DaylightBias;
    else if (tzState == TIME_ZONE_ID_STANDARD) totalBiasMin += dtzi.StandardBias;
    int offsetMin = -static_cast<int>(totalBiasMin);
    char sign = (offsetMin >= 0) ? '+' : '-';
    int absMin = std::abs(offsetMin);
    int hh = absMin / 60;
    int mm = absMin % 60;
    char buf[24];
    if (mm == 0) std::snprintf(buf, sizeof(buf), "UTC%c%d", sign, hh);
    else std::snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, hh, mm);
    (void)localTm;
    return std::string(buf);
#else
    (void)epochSec;
    char zbuf[16] = {0};
    std::strftime(zbuf, sizeof(zbuf), "%Z", &localTm);
    return zbuf[0] ? std::string(zbuf) : std::string("UTC");
#endif
}

std::string formatImpl(int64_t unixMs, bool use12Hour, bool includeTzAndBullet) {
    static const char* kMonths[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (unixMs < 0) unixMs = nowUnixMs();
    std::time_t sec = static_cast<std::time_t>(unixMs / 1000);
    std::tm loc{};
#ifdef _WIN32
    localtime_s(&loc, &sec);
#else
    localtime_r(&sec, &loc);
#endif
    int day = loc.tm_mday;
    const char* mon = kMonths[std::clamp(loc.tm_mon, 0, 11)];
    int year = loc.tm_year + 1900;

    char timePart[32];
    if (use12Hour) {
        int h12 = loc.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        const char* ampm = (loc.tm_hour >= 12) ? "PM" : "AM";
        std::snprintf(timePart, sizeof(timePart), "%d:%02d:%02d %s", h12, loc.tm_min, loc.tm_sec, ampm);
    } else {
        std::snprintf(timePart, sizeof(timePart), "%02d:%02d:%02d", loc.tm_hour, loc.tm_min, loc.tm_sec);
    }

    char out[128];
    if (includeTzAndBullet) {
        std::string tz = computeShortTimezoneAbbrev(loc, sec);
        // UTF-8 bullet "•" = \xE2\x80\xA2
        std::snprintf(out, sizeof(out), "%d %s %04d \xE2\x80\xA2 %s %s", day, mon, year, timePart, tz.c_str());
    } else {
        std::snprintf(out, sizeof(out), "%d %s %04d, %s", day, mon, year, timePart);
    }
    return std::string(out);
}

} // namespace

bool ClipTimestampFormatter::systemUses12HourClock() {
#ifdef _WIN32
    wchar_t buf[8] = {0};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, buf, 8) > 0) {
        // LOCALE_ITIME: "0" = AM/PM 12-hour format, "1" = 24-hour format
        return buf[0] == L'0';
    }
    return true;
#else
    return true;
#endif
}

std::string ClipTimestampFormatter::formatUtf8(int64_t unixMs) {
    return formatImpl(unixMs, systemUses12HourClock(), true);
}

std::wstring ClipTimestampFormatter::formatWide(int64_t unixMs) {
    return utf8ToWideLocal(formatUtf8(unixMs));
}

std::string ClipTimestampFormatter::formatCompactUtf8(int64_t unixMs) {
    return formatImpl(unixMs, systemUses12HourClock(), false);
}

std::wstring ClipTimestampFormatter::formatCompactWide(int64_t unixMs) {
    return utf8ToWideLocal(formatCompactUtf8(unixMs));
}

std::string ClipTimestampFormatter::formatExplicitUtf8(int64_t unixMs, bool use12Hour) {
    return formatImpl(unixMs, use12Hour, true);
}

void FrameTimestampStamper::stampBottomLeftNV12(
    uint8_t* planeY,
    int strideY,
    uint8_t* planeUV,
    int strideUV,
    int frameWidth,
    int frameHeight,
    int64_t unixMs
) {
    if (!planeY || !planeUV || frameWidth < 160 || frameHeight < 120) return;
    if (unixMs < 0) unixMs = nowUnixMs();

    int64_t sec = unixMs / 1000;
    if (sec != cachedSecond_ || frameWidth != cachedFrameW_ || frameHeight != cachedFrameH_) {
        rebuildStamp(ClipTimestampFormatter::formatWide(unixMs), frameWidth, frameHeight);
        cachedSecond_ = sec;
        cachedFrameW_ = frameWidth;
        cachedFrameH_ = frameHeight;
    }

    if (stampW_ <= 0 || stampH_ <= 0 || stampW_ + 8 >= frameWidth || stampH_ + 8 >= frameHeight) {
        return;
    }

    int marginX = (std::max(10, static_cast<int>(std::lround(frameWidth * 0.010))) & ~1);
    int marginBottom = (std::max(10, static_cast<int>(std::lround(frameHeight * 0.013))) & ~1);
    int originX = std::min(marginX, (frameWidth - stampW_) & ~1);
    int originY = std::max(0, (frameHeight - marginBottom - stampH_) & ~1);

    const uint8_t* src = stampYCbCrA_.data();

    // 1. Blend Luma (Plane 0)
    for (int r = 0; r < stampH_; ++r) {
        uint8_t* dstRowY = planeY + (originY + r) * strideY + originX;
        const uint8_t* srcRow = src + r * stampW_ * 4;
        for (int c = 0; c < stampW_; ++c) {
            int sA = srcRow[c * 4 + 3];
            if (sA == 0) continue;
            int sY = srcRow[c * 4 + 0];
            int dY = dstRowY[c];
            dstRowY[c] = static_cast<uint8_t>((sY * sA + dY * (255 - sA) + 127) / 255);
        }
    }

    // 2. Blend 2x2 Subsampled Chroma (Plane 1: interleaved Cb, Cr)
    int halfH = stampH_ >> 1;
    int halfW = stampW_ >> 1;
    int uvOriginY = originY >> 1;
    for (int r2 = 0; r2 < halfH; ++r2) {
        uint8_t* dstRowUV = planeUV + (uvOriginY + r2) * strideUV + originX;
        const uint8_t* srcRow = src + (r2 * 2) * stampW_ * 4;
        for (int c2 = 0; c2 < halfW; ++c2) {
            const uint8_t* sp = srcRow + (c2 * 2) * 4;
            int sA = sp[3];
            if (sA == 0) continue;
            int sCb = sp[1];
            int sCr = sp[2];
            int uvIdx = c2 * 2;
            int dCb = dstRowUV[uvIdx];
            int dCr = dstRowUV[uvIdx + 1];
            dstRowUV[uvIdx]     = static_cast<uint8_t>((sCb * sA + dCb * (255 - sA) + 127) / 255);
            dstRowUV[uvIdx + 1] = static_cast<uint8_t>((sCr * sA + dCr * (255 - sA) + 127) / 255);
        }
    }
}

void FrameTimestampStamper::rebuildStamp(const std::wstring& text, int frameW, int frameH) {
#ifdef _WIN32
    float fontSize = std::clamp(std::round(static_cast<float>(frameH) * 0.0165f), 11.0f, 26.0f);

    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        return;
    }

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"en-us",
        &fmt
    ))) {
        return;
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        fmt.Get(),
        static_cast<float>(frameW),
        static_cast<float>(frameH),
        &layout
    ))) {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;

    int padX = std::max(7, static_cast<int>(std::lround(fontSize * 0.55f)));
    int padY = std::max(4, static_cast<int>(std::lround(fontSize * 0.30f)));
    int rawW = std::min(frameW - 16, static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + padX * 2);
    int rawH = std::min(frameH - 16, static_cast<int>(std::ceil(metrics.height)) + padY * 2);
    int w = std::max(4, (rawW + 1) & ~1);
    int h = std::max(4, (rawH + 1) & ~1);

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        return;
    }

    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wicBmp))) {
        return;
    }

    ComPtr<ID2D1Factory> d2dFactory;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts, reinterpret_cast<void**>(d2dFactory.GetAddressOf())))) {
        return;
    }

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2dFactory->CreateWicBitmapRenderTarget(wicBmp.Get(), rtProps, &rt))) {
        return;
    }

    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0.0f));

    float radius = std::max(4.0f, std::round(fontSize * 0.34f));
    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(w) - 0.5f, static_cast<float>(h) - 0.5f),
        radius, radius
    );

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.72f), &bgBrush);
    rt->FillRoundedRectangle(pill, bgBrush.Get());

    ComPtr<ID2D1SolidColorBrush> borderBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), &borderBrush);
    rt->DrawRoundedRectangle(pill, borderBrush.Get(), 1.0f);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &textBrush);
    rt->DrawTextLayout(D2D1::Point2F(static_cast<float>(padX), static_cast<float>(padY)), layout.Get(), textBrush.Get());

    if (FAILED(rt->EndDraw())) return;

    std::vector<uint8_t> bgra(w * h * 4, 0);
    WICRect rc{0, 0, w, h};
    if (FAILED(wicBmp->CopyPixels(&rc, w * 4, static_cast<UINT>(bgra.size()), bgra.data()))) {
        return;
    }

    // Convert premultiplied BGRA -> BT.709 Video-Range [Y, Cb, Cr, A]
    std::vector<uint8_t> out(w * h * 4, 0);
    int pixelCount = w * h;
    for (int i = 0; i < pixelCount; ++i) {
        int idx = i * 4;
        int bPremul = bgra[idx + 0];
        int gPremul = bgra[idx + 1];
        int rPremul = bgra[idx + 2];
        int a = bgra[idx + 3];
        if (a == 0) {
            out[idx + 0] = 16;
            out[idx + 1] = 128;
            out[idx + 2] = 128;
            out[idx + 3] = 0;
            continue;
        }
        int r = std::min(255, (rPremul * 255 + (a >> 1)) / a);
        int g = std::min(255, (gPremul * 255 + (a >> 1)) / a);
        int b = std::min(255, (bPremul * 255 + (a >> 1)) / a);

        int yVal  = std::clamp(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8), 16, 235);
        int cbVal = std::clamp(128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8), 16, 240);
        int crVal = std::clamp(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8), 16, 240);

        out[idx + 0] = static_cast<uint8_t>(yVal);
        out[idx + 1] = static_cast<uint8_t>(cbVal);
        out[idx + 2] = static_cast<uint8_t>(crVal);
        out[idx + 3] = static_cast<uint8_t>(a);
    }

    stampW_ = w;
    stampH_ = h;
    stampYCbCrA_ = std::move(out);
#else
    (void)text; (void)frameW; (void)frameH;
#endif
}

} // namespace maxwatch
