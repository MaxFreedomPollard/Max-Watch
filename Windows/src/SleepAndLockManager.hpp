#pragma once

#include <atomic>
#include <functional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace maxwatch {

class SleepAndLockManager {
public:
    SleepAndLockManager();
    ~SleepAndLockManager();

    SleepAndLockManager(const SleepAndLockManager&) = delete;
    SleepAndLockManager& operator=(const SleepAndLockManager&) = delete;

#ifdef _WIN32
    void attachToWindow(HWND hwnd);
    void detachFromWindow();
    void handleSessionChange(WPARAM wParam);
    void setRecordingShutdownGuard(bool activeRecording);
#endif

    // Prevents Windows idle system sleep and Modern Standby (S0ix) suspension while armed,
    // while allowing the display to turn off or lock (Win+L) seamlessly.
    void setArmedPreventingIdleSleep(bool active);

    bool isArmedPreventingSleep() const { return hasSleepRequest_.load(); }
    bool isScreenCurrentlyLocked() const { return isScreenLocked_.load(); }

    // Deterministic simulation hook for automated --selftest verification
    void simulateScreenLockForTest(bool locked);

    std::function<void()> onScreenLocked;
    std::function<void()> onScreenUnlocked;
    std::function<void()> onSystemWillPowerOff;

private:
    std::atomic<bool> hasSleepRequest_{false};
    std::atomic<bool> isScreenLocked_{false};
#ifdef _WIN32
    HWND hwnd_ = nullptr;
    bool wtsRegistered_ = false;
    bool shutdownGuardActive_ = false;
    HANDLE powerRequestHandle_ = INVALID_HANDLE_VALUE;
#endif
};

} // namespace maxwatch
