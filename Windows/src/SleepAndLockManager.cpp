#include "SleepAndLockManager.hpp"

#ifdef _WIN32
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace maxwatch {

SleepAndLockManager::SleepAndLockManager() = default;

SleepAndLockManager::~SleepAndLockManager() {
    setArmedPreventingIdleSleep(false);
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
        if (onScreenLocked) onScreenLocked();
    } else if (wParam == WTS_SESSION_UNLOCK) {
        isScreenLocked_.store(false);
        if (onScreenUnlocked) onScreenUnlocked();
    } else if (wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_SESSION_LOGOFF) {
        isScreenLocked_.store(true);
        if (onScreenLocked) onScreenLocked();
    }
}

void SleepAndLockManager::setRecordingShutdownGuard(bool activeRecording) {
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
#endif

void SleepAndLockManager::setArmedPreventingIdleSleep(bool active) {
#ifdef _WIN32
    if (active) {
        if (!hasSleepRequest_.load()) {
            REASON_CONTEXT ctx{};
            ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
            ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
            ctx.Reason.SimpleReasonString = const_cast<LPWSTR>(
                L"Max Watch Armed Offline Motion Security Recording"
            );
            HANDLE hReq = PowerCreateRequest(&ctx);
            if (hReq != INVALID_HANDLE_VALUE) {
                PowerSetRequest(hReq, PowerRequestSystemRequired);
                // PowerRequestExecutionRequired keeps background capture & encryption threads running
                // even during Windows 10/11 Modern Standby (S0 Low Power Idle) when screen is off/locked!
                PowerSetRequest(hReq, PowerRequestExecutionRequired);
                powerRequestHandle_ = hReq;
            }
            if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED) == 0) {
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
            }
            hasSleepRequest_.store(true);
        }
    } else {
        if (hasSleepRequest_.load()) {
            if (powerRequestHandle_ != INVALID_HANDLE_VALUE) {
                PowerClearRequest(powerRequestHandle_, PowerRequestExecutionRequired);
                PowerClearRequest(powerRequestHandle_, PowerRequestSystemRequired);
                CloseHandle(powerRequestHandle_);
                powerRequestHandle_ = INVALID_HANDLE_VALUE;
            }
            SetThreadExecutionState(ES_CONTINUOUS);
            hasSleepRequest_.store(false);
        }
    }
#else
    hasSleepRequest_.store(active);
#endif
}

void SleepAndLockManager::simulateScreenLockForTest(bool locked) {
    isScreenLocked_.store(locked);
    if (locked) {
        if (onScreenLocked) onScreenLocked();
    } else {
        if (onScreenUnlocked) onScreenUnlocked();
    }
}

} // namespace maxwatch
