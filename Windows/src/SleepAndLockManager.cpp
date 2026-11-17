#include "SleepAndLockManager.hpp"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <winioctl.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace maxwatch {

namespace {

#ifdef _WIN32

// Pure local kernel / power / DDC-CI brightness control (imports ZERO network DLLs!)
#ifndef IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS
#define IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x0125, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS
#define IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x0126, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS
#define IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x0127, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#pragma pack(push, 1)
struct LcdDisplayBrightness {
    UCHAR ucDisplayPolicy;
    UCHAR ucACBrightness;
    UCHAR ucDCBrightness;
};

struct DxvaPhysicalMonitor {
    HANDLE hPhysicalMonitor;
    WCHAR szPhysicalMonitorDescription[128];
};
#pragma pack(pop)

// GUID_VIDEO_SUBGROUP: {7516b95f-f776-4464-8c53-06167f40cc99}
constexpr GUID kGuidVideoSubgroup = {
    0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}
};
// GUID_DEVICE_POWER_POLICY_VIDEO_BRIGHTNESS: {aded5e82-b909-4619-9949-f5d71dac0bcb}
constexpr GUID kGuidVideoBrightness = {
    0xaded5e82, 0xb909, 0x4619, {0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb}
};

class Win32DisplayBrightnessBackend final : public IDisplayBrightnessBackend {
public:
    Win32DisplayBrightnessBackend() {
        powrMod_ = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (powrMod_) {
            fnGetActiveScheme_ = reinterpret_cast<PowerGetActiveSchemeFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerGetActiveScheme"))
            );
            fnReadAC_ = reinterpret_cast<PowerReadValueFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerReadACValueIndex"))
            );
            fnReadDC_ = reinterpret_cast<PowerReadValueFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerReadDCValueIndex"))
            );
            fnWriteAC_ = reinterpret_cast<PowerWriteValueFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerWriteACValueIndex"))
            );
            fnWriteDC_ = reinterpret_cast<PowerWriteValueFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerWriteDCValueIndex"))
            );
            fnSetActiveScheme_ = reinterpret_cast<PowerSetActiveSchemeFn>(
                reinterpret_cast<void*>(GetProcAddress(powrMod_, "PowerSetActiveScheme"))
            );
        }

        dxvaMod_ = LoadLibraryExW(L"dxva2.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dxvaMod_) {
            fnGetNumPhys_ = reinterpret_cast<GetNumPhysFn>(
                reinterpret_cast<void*>(GetProcAddress(dxvaMod_, "GetNumberOfPhysicalMonitorsFromHMONITOR"))
            );
            fnGetPhys_ = reinterpret_cast<GetPhysFn>(
                reinterpret_cast<void*>(GetProcAddress(dxvaMod_, "GetPhysicalMonitorsFromHMONITOR"))
            );
            fnGetMonBright_ = reinterpret_cast<GetMonBrightFn>(
                reinterpret_cast<void*>(GetProcAddress(dxvaMod_, "GetMonitorBrightness"))
            );
            fnSetMonBright_ = reinterpret_cast<SetMonBrightFn>(
                reinterpret_cast<void*>(GetProcAddress(dxvaMod_, "SetMonitorBrightness"))
            );
            fnDestroyPhys_ = reinterpret_cast<DestroyPhysFn>(
                reinterpret_cast<void*>(GetProcAddress(dxvaMod_, "DestroyPhysicalMonitors"))
            );
        }
    }

    ~Win32DisplayBrightnessBackend() override {
        if (dxvaMod_) FreeLibrary(dxvaMod_);
        if (powrMod_) FreeLibrary(powrMod_);
    }

    void captureCurrentBrightnessOnce() override {
        hasSavedLcd_ = false;
        hasSavedPowerScheme_ = false;
        savedMonitorBrightness_.clear();

        // 1. Snapshot internal laptop/tablet panel via \\.\LCD IOCTL
        HANDLE hLcd = CreateFileW(
            L"\\\\.\\LCD",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hLcd != INVALID_HANDLE_VALUE) {
            LcdDisplayBrightness cur{};
            DWORD bytesRet = 0;
            if (DeviceIoControl(
                    hLcd,
                    IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS,
                    nullptr,
                    0,
                    &cur,
                    sizeof(cur),
                    &bytesRet,
                    nullptr
                ) && bytesRet >= sizeof(cur)) {
                savedLcdAC_ = cur.ucACBrightness;
                savedLcdDC_ = cur.ucDCBrightness;
                hasSavedLcd_ = true;
            }
            UCHAR levels[256]{};
            DWORD supBytes = 0;
            lcdMaxLevel_ = 100;
            if (DeviceIoControl(
                    hLcd,
                    IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS,
                    nullptr,
                    0,
                    levels,
                    sizeof(levels),
                    &supBytes,
                    nullptr
                ) && supBytes > 0) {
                UCHAR maxVal = 0;
                for (DWORD i = 0; i < supBytes; ++i) {
                    if (levels[i] > maxVal) maxVal = levels[i];
                }
                if (maxVal > 0) lcdMaxLevel_ = maxVal;
            }
            CloseHandle(hLcd);
        }

        // 2. Snapshot Windows 10/11 active power-policy AC/DC display brightness
        if (fnGetActiveScheme_ && fnReadAC_ && fnReadDC_) {
            GUID* scheme = nullptr;
            if (fnGetActiveScheme_(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
                DWORD acVal = 0, dcVal = 0;
                if (fnReadAC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, &acVal) == ERROR_SUCCESS &&
                    fnReadDC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, &dcVal) == ERROR_SUCCESS) {
                    savedPowerAC_ = acVal;
                    savedPowerDC_ = dcVal;
                    hasSavedPowerScheme_ = true;
                }
                LocalFree(scheme);
            }
        }

        // 3. Snapshot external DDC/CI desktop monitors if supported
        if (fnGetNumPhys_ && fnGetPhys_ && fnGetMonBright_ && fnDestroyPhys_) {
            EnumDisplayMonitors(
                nullptr,
                nullptr,
                &Win32DisplayBrightnessBackend::enumCaptureMonitorProc,
                reinterpret_cast<LPARAM>(this)
            );
        }
    }

    void applyMaxBrightness100() override {
        // 1. Boost internal LCD panel to max supported level (100%)
        HANDLE hLcd = CreateFileW(
            L"\\\\.\\LCD",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hLcd != INVALID_HANDLE_VALUE) {
            LcdDisplayBrightness boost{};
            boost.ucDisplayPolicy = 0x03; // DISPLAYPOLICY_BOTH
            boost.ucACBrightness = lcdMaxLevel_;
            boost.ucDCBrightness = lcdMaxLevel_;
            DWORD bytesRet = 0;
            DeviceIoControl(
                hLcd,
                IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS,
                &boost,
                sizeof(boost),
                nullptr,
                0,
                &bytesRet,
                nullptr
            );
            CloseHandle(hLcd);
        }

        // 2. Boost Windows 10/11 active power-scheme video brightness to 100%
        if (fnGetActiveScheme_ && fnWriteAC_ && fnWriteDC_ && fnSetActiveScheme_) {
            GUID* scheme = nullptr;
            if (fnGetActiveScheme_(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
                fnWriteAC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, 100);
                fnWriteDC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, 100);
                fnSetActiveScheme_(nullptr, scheme);
                LocalFree(scheme);
            }
        }

        // 3. Boost external DDC/CI monitors to max
        if (fnGetNumPhys_ && fnGetPhys_ && fnGetMonBright_ && fnSetMonBright_ && fnDestroyPhys_) {
            EnumDisplayMonitors(
                nullptr,
                nullptr,
                &Win32DisplayBrightnessBackend::enumBoostMonitorProc,
                reinterpret_cast<LPARAM>(this)
            );
        }
    }

    void restoreCapturedBrightness() override {
        if (hasSavedLcd_) {
            HANDLE hLcd = CreateFileW(
                L"\\\\.\\LCD",
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
            if (hLcd != INVALID_HANDLE_VALUE) {
                LcdDisplayBrightness rest{};
                rest.ucDisplayPolicy = 0x03; // DISPLAYPOLICY_BOTH
                rest.ucACBrightness = savedLcdAC_;
                rest.ucDCBrightness = savedLcdDC_;
                DWORD bytesRet = 0;
                DeviceIoControl(
                    hLcd,
                    IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS,
                    &rest,
                    sizeof(rest),
                    nullptr,
                    0,
                    &bytesRet,
                    nullptr
                );
                CloseHandle(hLcd);
            }
            hasSavedLcd_ = false;
        }

        if (hasSavedPowerScheme_ && fnGetActiveScheme_ && fnWriteAC_ && fnWriteDC_ && fnSetActiveScheme_) {
            GUID* scheme = nullptr;
            if (fnGetActiveScheme_(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
                fnWriteAC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, savedPowerAC_);
                fnWriteDC_(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, savedPowerDC_);
                fnSetActiveScheme_(nullptr, scheme);
                LocalFree(scheme);
            }
            hasSavedPowerScheme_ = false;
        }

        if (!savedMonitorBrightness_.empty() && fnGetNumPhys_ && fnGetPhys_ && fnSetMonBright_ && fnDestroyPhys_) {
            restoreIndex_ = 0;
            EnumDisplayMonitors(
                nullptr,
                nullptr,
                &Win32DisplayBrightnessBackend::enumRestoreMonitorProc,
                reinterpret_cast<LPARAM>(this)
            );
            savedMonitorBrightness_.clear();
        }
    }

private:
    using PowerGetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
    using PowerReadValueFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD);
    using PowerWriteValueFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, DWORD);
    using PowerSetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);

    using GetNumPhysFn = BOOL(WINAPI*)(HMONITOR, LPDWORD);
    using GetPhysFn = BOOL(WINAPI*)(HMONITOR, DWORD, DxvaPhysicalMonitor*);
    using GetMonBrightFn = BOOL(WINAPI*)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
    using SetMonBrightFn = BOOL(WINAPI*)(HANDLE, DWORD);
    using DestroyPhysFn = BOOL(WINAPI*)(DWORD, DxvaPhysicalMonitor*);

    static BOOL CALLBACK enumCaptureMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
        auto* self = reinterpret_cast<Win32DisplayBrightnessBackend*>(lParam);
        DWORD count = 0;
        if (!self->fnGetNumPhys_(hMon, &count) || count == 0 || count > 16) return TRUE;
        std::vector<DxvaPhysicalMonitor> mons(count);
        if (self->fnGetPhys_(hMon, count, mons.data())) {
            for (DWORD i = 0; i < count; ++i) {
                DWORD minB = 0, curB = 0, maxB = 0;
                if (self->fnGetMonBright_(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                    self->savedMonitorBrightness_.push_back(curB);
                }
            }
            self->fnDestroyPhys_(count, mons.data());
        }
        return TRUE;
    }

    static BOOL CALLBACK enumBoostMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
        auto* self = reinterpret_cast<Win32DisplayBrightnessBackend*>(lParam);
        DWORD count = 0;
        if (!self->fnGetNumPhys_(hMon, &count) || count == 0 || count > 16) return TRUE;
        std::vector<DxvaPhysicalMonitor> mons(count);
        if (self->fnGetPhys_(hMon, count, mons.data())) {
            for (DWORD i = 0; i < count; ++i) {
                DWORD minB = 0, curB = 0, maxB = 0;
                if (self->fnGetMonBright_(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                    self->fnSetMonBright_(mons[i].hPhysicalMonitor, maxB);
                }
            }
            self->fnDestroyPhys_(count, mons.data());
        }
        return TRUE;
    }

    static BOOL CALLBACK enumRestoreMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
        auto* self = reinterpret_cast<Win32DisplayBrightnessBackend*>(lParam);
        DWORD count = 0;
        if (!self->fnGetNumPhys_(hMon, &count) || count == 0 || count > 16) return TRUE;
        std::vector<DxvaPhysicalMonitor> mons(count);
        if (self->fnGetPhys_(hMon, count, mons.data())) {
            for (DWORD i = 0; i < count; ++i) {
                if (self->restoreIndex_ < self->savedMonitorBrightness_.size()) {
                    self->fnSetMonBright_(mons[i].hPhysicalMonitor, self->savedMonitorBrightness_[self->restoreIndex_++]);
                }
            }
            self->fnDestroyPhys_(count, mons.data());
        }
        return TRUE;
    }

    HMODULE powrMod_ = nullptr;
    PowerGetActiveSchemeFn fnGetActiveScheme_ = nullptr;
    PowerReadValueFn fnReadAC_ = nullptr;
    PowerReadValueFn fnReadDC_ = nullptr;
    PowerWriteValueFn fnWriteAC_ = nullptr;
    PowerWriteValueFn fnWriteDC_ = nullptr;
    PowerSetActiveSchemeFn fnSetActiveScheme_ = nullptr;

    HMODULE dxvaMod_ = nullptr;
    GetNumPhysFn fnGetNumPhys_ = nullptr;
    GetPhysFn fnGetPhys_ = nullptr;
    GetMonBrightFn fnGetMonBright_ = nullptr;
    SetMonBrightFn fnSetMonBright_ = nullptr;
    DestroyPhysFn fnDestroyPhys_ = nullptr;

    bool hasSavedLcd_ = false;
    UCHAR savedLcdAC_ = 60;
    UCHAR savedLcdDC_ = 60;
    UCHAR lcdMaxLevel_ = 100;

    bool hasSavedPowerScheme_ = false;
    DWORD savedPowerAC_ = 60;
    DWORD savedPowerDC_ = 60;

    std::vector<DWORD> savedMonitorBrightness_;
    size_t restoreIndex_ = 0;
};

#else

class NoOpDisplayBrightnessBackend final : public IDisplayBrightnessBackend {
public:
    void captureCurrentBrightnessOnce() override {}
    void applyMaxBrightness100() override {}
    void restoreCapturedBrightness() override {}
};

#endif

} // namespace

std::shared_ptr<IDisplayBrightnessBackend> createSystemDisplayBrightnessBackend() {
#ifdef _WIN32
    return std::make_shared<Win32DisplayBrightnessBackend>();
#else
    return std::make_shared<NoOpDisplayBrightnessBackend>();
#endif
}

SleepAndLockManager::SleepAndLockManager(
    std::shared_ptr<IDisplayBrightnessBackend> customBrightnessBackend,
    bool enableOsPowerRequests
)
    : brightnessBackend_(
          customBrightnessBackend ? std::move(customBrightnessBackend)
                                  : createSystemDisplayBrightnessBackend()
      ),
      enableOsPowerRequests_(enableOsPowerRequests) {}

SleepAndLockManager::~SleepAndLockManager() {
    setArmedPreventingIdleSleep(false);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        restoreBrightnessIfNeededLocked();
    }
#ifdef _WIN32
    detachFromWindow();
#endif
}

#ifdef _WIN32
void SleepAndLockManager::attachToWindow(HWND hwnd) {
    detachFromWindow();
    hwnd_ = hwnd;
    if (hwnd_) {
        if (WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION)) {
            wtsRegistered_ = true;
        }
    }
}

void SleepAndLockManager::detachFromWindow() {
    if (hwnd_ && shutdownGuardActive_) {
        ShutdownBlockReasonDestroy(hwnd_);
        shutdownGuardActive_ = false;
    }
    if (hwnd_ && wtsRegistered_) {
        WTSUnRegisterSessionNotification(hwnd_);
        wtsRegistered_ = false;
    }
    hwnd_ = nullptr;
}

void SleepAndLockManager::handleSessionChange(WPARAM wParam) {
    if (wParam == WTS_SESSION_LOCK) {
        isScreenLocked_.store(true);
        reconcilePowerAndBrightness(true);
        if (onScreenLocked) onScreenLocked();
    } else if (wParam == WTS_SESSION_UNLOCK) {
        isScreenLocked_.store(false);
        reconcilePowerAndBrightness(false);
        if (onScreenUnlocked) onScreenUnlocked();
    } else if (wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_SESSION_LOGOFF) {
        isScreenLocked_.store(true);
        reconcilePowerAndBrightness(false);
        if (onScreenLocked) onScreenLocked();
    }
}

void SleepAndLockManager::setRecordingShutdownGuard(bool activeRecording) {
    if (activeRecording && isScreenLocked_.load()) {
        reconcilePowerAndBrightness(true);
    }
    if (!hwnd_) return;
    if (activeRecording && !shutdownGuardActive_) {
        ShutdownBlockReasonCreate(
            hwnd_,
            L"Max Watch is encrypting and saving your active security clip to disk..."
        );
        shutdownGuardActive_ = true;
    } else if (!activeRecording && shutdownGuardActive_) {
        ShutdownBlockReasonDestroy(hwnd_);
        shutdownGuardActive_ = false;
    }
}
#else
void SleepAndLockManager::setRecordingShutdownGuard(bool activeRecording) {
    if (activeRecording && isScreenLocked_.load()) {
        reconcilePowerAndBrightness(true);
    }
}
#endif

void SleepAndLockManager::setArmedPreventingIdleSleep(bool active) {
    isArmed_.store(active);
    reconcilePowerAndBrightness(active && isScreenLocked_.load());
}

void SleepAndLockManager::setMaxBrightnessWhenLocked(bool enabled) {
    bool prev = maxBrightnessWhenLocked_.exchange(enabled);
    if (prev != enabled) {
        reconcilePowerAndBrightness(enabled && isScreenLocked_.load());
    }
}

void SleepAndLockManager::periodicMaintenanceTick(double nowMonotonicSec) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!shouldBoostLockedBrightness()) return;
    if ((nowMonotonicSec - lastReinforcedAtSec_) >= 2.0) {
        lastReinforcedAtSec_ = nowMonotonicSec;
        if (brightnessBackend_) {
            brightnessBackend_->applyMaxBrightness100();
        }
#ifdef _WIN32
        if (enableOsPowerRequests_) {
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        }
#endif
    }
}

void SleepAndLockManager::simulateScreenLockForTest(bool locked) {
    isScreenLocked_.store(locked);
    reconcilePowerAndBrightness(locked);
    if (locked) {
        if (onScreenLocked) onScreenLocked();
    } else {
        if (onScreenUnlocked) onScreenUnlocked();
    }
}

void SleepAndLockManager::reconcilePowerAndBrightness(bool forceDisplayWakePulse) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const bool wantSystemAwake = isArmed_.load();
    const bool wantLockedBright = shouldBoostLockedBrightness();

#ifdef _WIN32
    if (enableOsPowerRequests_) {
        if (wantSystemAwake) {
            if (powerRequestHandle_ == INVALID_HANDLE_VALUE) {
                REASON_CONTEXT ctx{};
                ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
                ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
                ctx.Reason.SimpleReasonString = const_cast<LPWSTR>(
                    L"Max Watch Armed Offline Motion & Sound Security Recording"
                );
                powerRequestHandle_ = PowerCreateRequest(&ctx);
            }
            if (powerRequestHandle_ != INVALID_HANDLE_VALUE && !hasSystemPowerRequest_) {
                PowerSetRequest(powerRequestHandle_, PowerRequestSystemRequired);
                PowerSetRequest(powerRequestHandle_, PowerRequestExecutionRequired);
                hasSystemPowerRequest_ = true;
            }
            if (wantLockedBright) {
                if (powerRequestHandle_ != INVALID_HANDLE_VALUE && !hasDisplayPowerRequest_) {
                    PowerSetRequest(powerRequestHandle_, PowerRequestDisplayRequired);
                    hasDisplayPowerRequest_ = true;
                }
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
                if (forceDisplayWakePulse) {
                    DWORD_PTR res = 0;
                    SendMessageTimeoutW(
                        HWND_BROADCAST,
                        WM_SYSCOMMAND,
                        SC_MONITORPOWER,
                        static_cast<LPARAM>(-1),
                        SMTO_ABORTIFHUNG,
                        150,
                        &res
                    );
                }
            } else {
                if (powerRequestHandle_ != INVALID_HANDLE_VALUE && hasDisplayPowerRequest_) {
                    PowerClearRequest(powerRequestHandle_, PowerRequestDisplayRequired);
                    hasDisplayPowerRequest_ = false;
                }
                if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED) == 0) {
                    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
                }
            }
        } else {
            if (powerRequestHandle_ != INVALID_HANDLE_VALUE) {
                if (hasDisplayPowerRequest_) {
                    PowerClearRequest(powerRequestHandle_, PowerRequestDisplayRequired);
                    hasDisplayPowerRequest_ = false;
                }
                if (hasSystemPowerRequest_) {
                    PowerClearRequest(powerRequestHandle_, PowerRequestExecutionRequired);
                    PowerClearRequest(powerRequestHandle_, PowerRequestSystemRequired);
                    hasSystemPowerRequest_ = false;
                }
                CloseHandle(powerRequestHandle_);
                powerRequestHandle_ = INVALID_HANDLE_VALUE;
            }
            SetThreadExecutionState(ES_CONTINUOUS);
        }
    }
#else
    (void)enableOsPowerRequests_;
    (void)forceDisplayWakePulse;
    (void)wantSystemAwake;
#endif

    if (wantLockedBright) {
        if (!brightnessBoostActive_.load()) {
            if (brightnessBackend_) {
                brightnessBackend_->captureCurrentBrightnessOnce();
            }
            brightnessBoostActive_.store(true);
        }
        if (brightnessBackend_) {
            brightnessBackend_->applyMaxBrightness100();
        }
    } else {
        restoreBrightnessIfNeededLocked();
    }
}

void SleepAndLockManager::restoreBrightnessIfNeededLocked() {
    if (!brightnessBoostActive_.load()) return;
    brightnessBoostActive_.store(false);
    if (brightnessBackend_) {
        brightnessBackend_->restoreCapturedBrightness();
    }
}

} // namespace maxwatch
