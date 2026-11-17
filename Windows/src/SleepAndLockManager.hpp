#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

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

// Abstract hardware display brightness backend (allows deterministic --selftest verification
// while using pure offline \\.\LCD IOCTL + powrprof.dll + dxva2.dll on Windows 10/11).
class IDisplayBrightnessBackend {
public:
    virtual ~IDisplayBrightnessBackend() = default;
    virtual void captureCurrentBrightnessOnce() = 0;
    virtual void applyMaxBrightness100() = 0;
    virtual void restoreCapturedBrightness() = 0;
};

std::shared_ptr<IDisplayBrightnessBackend> createSystemDisplayBrightnessBackend();

class SleepAndLockManager {
public:
    explicit SleepAndLockManager(
        std::shared_ptr<IDisplayBrightnessBackend> customBrightnessBackend = nullptr,
        bool enableOsPowerRequests = true
    );
    ~SleepAndLockManager();

    SleepAndLockManager(const SleepAndLockManager&) = delete;
    SleepAndLockManager& operator=(const SleepAndLockManager&) = delete;

#ifdef _WIN32
    void attachToWindow(HWND hwnd);
    void detachFromWindow();
    void handleSessionChange(WPARAM wParam);
    void setRecordingShutdownGuard(bool activeRecording);
#else
    void setRecordingShutdownGuard(bool activeRecording);
#endif

    // Prevents Windows idle system sleep and Modern Standby (S0ix) suspension while armed.
    // When maxBrightnessWhenLockedEnabled() is true (default ON) and the screen is locked (Win+L),
    // also keeps the display awake at 100% brightness for pitch-dark night illumination!
    void setArmedPreventingIdleSleep(bool active);

    // Checked by default (`true`) so arming + locking screen illuminates dark rooms out of the box.
    void setMaxBrightnessWhenLocked(bool enabled);
    bool isMaxBrightnessWhenLocked() const { return maxBrightnessWhenLocked_.load(); }

    // Called periodically (e.g. from UI timer) to defeat ambient-light auto-dimming while armed & locked.
    void periodicMaintenanceTick(double nowMonotonicSec);

    bool isArmedPreventingSleep() const { return isArmed_.load(); }
    bool isScreenCurrentlyLocked() const { return isScreenLocked_.load(); }
    bool isBrightnessBoostActive() const { return brightnessBoostActive_.load(); }
    bool shouldBoostLockedBrightness() const {
        return isArmed_.load() && isScreenLocked_.load() && maxBrightnessWhenLocked_.load();
    }

    // Deterministic simulation hook for automated --selftest verification
    void simulateScreenLockForTest(bool locked);

    std::function<void()> onScreenLocked;
    std::function<void()> onScreenUnlocked;
    std::function<void()> onSystemWillPowerOff;

private:
    void reconcilePowerAndBrightness(bool forceDisplayWakePulse);
    void restoreBrightnessIfNeededLocked();

    std::shared_ptr<IDisplayBrightnessBackend> brightnessBackend_;
    bool enableOsPowerRequests_ = true;

    std::mutex stateMutex_;
    std::atomic<bool> isArmed_{false};
    std::atomic<bool> isScreenLocked_{false};
    std::atomic<bool> maxBrightnessWhenLocked_{true}; // Default ON!
    std::atomic<bool> brightnessBoostActive_{false};
    double lastReinforcedAtSec_ = -1000.0;

#ifdef _WIN32
    HWND hwnd_ = nullptr;
    bool wtsRegistered_ = false;
    bool shutdownGuardActive_ = false;
    HANDLE powerRequestHandle_ = INVALID_HANDLE_VALUE;
    bool hasSystemPowerRequest_ = false;
    bool hasDisplayPowerRequest_ = false;
#endif
};

} // namespace maxwatch
