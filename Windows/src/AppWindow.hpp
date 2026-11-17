#pragma once

#ifdef _WIN32

#include "MediaPipeline.hpp"
#include "SleepAndLockManager.hpp"
#include "TimestampStamper.hpp"
#include "VaultCrypto.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace maxwatch {

class AppWindow {
public:
    explicit AppWindow(const std::wstring& customVaultDir = L"", bool uiSmokeTestMode = false, const std::wstring& smokeOutDir = L"");
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool createAndShow(HINSTANCE hInstance, int nCmdShow);
    int runMessageLoop();

private:
    enum class HitId {
        None = 0,
        HeaderArm,
        HeaderPlayAll,
        HeaderModeSwitch,
        HeaderVaultLock,
        RecOpenPrivacySettings,
        RecCameraPopup,
        RecMicPopup,
        RecAudioOnSeg,
        RecAudioOffSeg,
        RecSensitivitySlider,
        RecWakeOnSoundToggle,
        RecSoundSensitivitySlider,
        RecMaxBrightnessToggle,
        SidebarUnlockBtn,
        SidebarPasswordSettings,
        SidebarClipRowBase, // + index
        PlayerLockedUnlockBtn = 1000,
        PlayerLockedBackCamBtn,
        PlayerAudioMuteBtn,
        PlayerChooseFolderBtn,
        PlayerSaveAsBtn,
        PlayerLockAndCamBtn,
        PlayerScrubberTrack,
        PlayerPlayAllBtn,
        PlayerPrevBtn,
        PlayerPlayPauseBtn,
        PlayerNextBtn,
        PlayerSaveAndNextBtn,
        PlayerDeleteAndNextBtn,
        PlayerSpeed1xSeg,
        PlayerSpeed4xSeg,
        PlayerSpeed12xSeg
    };

    enum class TriggerKind {
        Motion,
        Sound
    };

    enum class ButtonStyle {
        Neutral,
        Green,
        Red,
        Cyan,
        Teal,
        Yellow
    };

    struct HitZone {
        int id = 0;
        D2D1_RECT_F rect{};
        bool enabled = true;
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Menu, Accelerators & System Tray
    void buildMenuBar();
    void createAcceleratorTable();
    void initTrayIcon();
    void updateTrayIcon();
    void removeTrayIcon();
    void showTrayContextMenu();
    HICON createDynamicMFHicon(bool armed, bool recording);

    // Preferences
    void loadPreferences();
    void savePreferences() const;

    // Direct2D Rendering
    bool ensureD2DFactories();
    bool ensureHwndRenderTarget();
    void discardDeviceResources();
    void renderWindow();
    void renderSceneToTarget(ID2D1RenderTarget* rt, float widthDip, float heightDip);
    bool saveCurrentSceneToPng(const std::wstring& filePath, UINT32 widthPx = 1300, UINT32 heightPx = 800);

    // Drawing Helpers
    void drawMFLogoBadge(ID2D1RenderTarget* rt, D2D1_RECT_F rect, const D2D1_COLOR_F* dotColor);
    void drawButton(
        ID2D1RenderTarget* rt,
        int hitId,
        D2D1_RECT_F rect,
        const std::wstring& label,
        ButtonStyle style,
        bool enabled = true,
        float fontSize = 12.2f
    );
    void drawCheckboxRow(
        ID2D1RenderTarget* rt,
        int hitId,
        D2D1_RECT_F rect,
        bool checked,
        const std::wstring& label,
        D2D1_COLOR_F accentColor,
        bool enabled = true
    );
    void drawMeterBar(
        ID2D1RenderTarget* rt,
        D2D1_RECT_F rect,
        double fraction,
        double thresholdFraction,
        D2D1_COLOR_F fillColor
    );
    void drawSensitivitySlider(ID2D1RenderTarget* rt, D2D1_RECT_F rect);
    void drawSoundSensitivitySlider(ID2D1RenderTarget* rt, D2D1_RECT_F rect);
    void drawSoundCheckpointPopup(ID2D1RenderTarget* rt, D2D1_RECT_F sliderRect, float stageLeft, float stageRight);
    void drawScrubberTimeline(ID2D1RenderTarget* rt, D2D1_RECT_F rect);
    void drawAspectFitFrame(ID2D1RenderTarget* rt, D2D1_RECT_F bounds, const BgraFrame& frame, bool isPlayer);
    void drawTextLine(
        ID2D1RenderTarget* rt,
        const std::wstring& text,
        IDWriteTextFormat* fmt,
        D2D1_RECT_F rect,
        D2D1_COLOR_F color,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
        bool wordWrap = false
    );
    float measureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth = 2000.0f);

    // Hit Testing & Mouse Input
    D2D1_POINT_2F clientPixelsToDip(int xPx, int yPx) const;
    int hitTestAt(D2D1_POINT_2F ptDip) const;
    void onMouseDown(D2D1_POINT_2F ptDip);
    void onMouseMove(D2D1_POINT_2F ptDip);
    void onMouseUp(D2D1_POINT_2F ptDip);
    void onMouseWheel(short wheelDelta, D2D1_POINT_2F ptDip);
    void dispatchHitCommand(int hitId);
    void handleMenuCommand(WORD cmdId);

    // Core App Actions & Security State Machine
    void setStageMode(bool playerMode);
    void refreshVaultUIState();
    void toggleArmedAction();
    void setRecordAudioEnabled(bool enabled);
    void setWakeOnSoundEnabled(bool enabled);
    void setMaxBrightnessWhenLockedEnabled(bool enabled);
    void showCameraPopupMenu();
    void showMicPopupMenu();
    void openWindowsPrivacySettings();
    void handleMotionTriggeredOnMain();
    void handleTriggerOnMain(TriggerKind kind);
    bool hasSufficientVaultDiskSpace(uint64_t minimumBytes = 200ULL * 1024ULL * 1024ULL) const;
    void stopCurrentRecordingClipSynchronously();
    void onPeriodicTimerTick();
    void runSmokeTestStep();

    // Vault & Playback Actions
    bool ensureVaultInitializedInteractive();
    bool ensureVaultUnlockedInteractive();
    void lockVaultImmediately(bool keepCameraStage);
    void playAllAction();
    void loadAndPlayClip(int index, bool autoPlay);
    void unloadActivePlayerClip();
    void togglePlayPauseAction();
    void setPlaybackSpeedAction(float speed);
    void togglePlayerMuteAction();
    void prevClipAction();
    void nextClipAction(bool fromAutoEnd = false);
    bool promptForSaveFolderInteractive();
    void saveAndNextAction();
    void deleteAndNextAction();
    void exportCurrentClipSaveAsAction();
    void showPasswordSettingsDialog();
    void showAboutDialog();
    void quitApplicationCleanly();

    // Modal Password Prompts
    bool showCreatePasswordModal(std::string& outPassword);
    bool showUnlockPasswordModal(std::string& outPassword, bool& outRequestedReset);
    void showChangeOrResetPasswordModal();

private:
    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HMENU hMenuBar_ = nullptr;
    HACCEL hAccel_ = nullptr;
    HICON trayHicon_ = nullptr;
    bool trayAdded_ = false;
    bool trayBalloonShownOnce_ = false;
    bool isTerminating_ = false;

    bool uiSmokeTestMode_ = false;
    std::wstring smokeOutDir_;
    int smokeTickCounter_ = 0;

    VaultManager vault_;
    SleepAndLockManager sleepAndLock_;
    CaptureEngine capture_;
    VaultPlayerEngine player_;

    std::vector<DeviceInfo> videoDevices_;
    std::vector<DeviceInfo> audioDevices_;
    int selectedVideoIdx_ = 0;
    int selectedAudioIdx_ = 0;

    bool isAudioRecordingEnabled_ = true;
    bool isWakeOnSoundEnabled_ = false; // Strictly additive to motion detection — NEVER replaces motion!
    bool needsCaptureRefreshAfterClip_ = false;
    bool isArmed_ = false;
    bool isRecordingClip_ = false;
    bool hasMotionTriggerInActiveClip_ = false;
    bool hasSoundTriggerInActiveClip_ = false;
    bool lowDiskSpaceWarning_ = false;
    std::chrono::steady_clock::time_point recordingStartedSteady_{};
    std::chrono::steady_clock::time_point recordingDeadlineSteady_{};
    std::chrono::steady_clock::time_point lastMaintenanceTickSteady_{};
    static constexpr double kMinimumClipDurationSec = 60.0;

    // Player & Sidebar State
    bool isInPlayerMode_ = false;
    bool isPlayAllSequential_ = false;
    std::vector<EncryptedClipSummary> unlockedClips_;
    int currentClipIndex_ = -1;
    float selectedPlaybackSpeed_ = 1.0f;
    std::wstring chosenSaveFolder_;
    std::wstring nowPlayingTitle_ = L"Encrypted Vault Player - Select a clip or click Play All";
    std::wstring nowPlayingMeta_ = L"Decrypted on-the-fly in RAM • Zero temporary files written to disk";
    std::chrono::steady_clock::time_point lastVaultInteractionSteady_;

    // Interactive Mouse / Slider / Scrubber / Scroll State
    std::vector<HitZone> hitZones_;
    int hoveredHitId_ = 0;
    int pressedHitId_ = 0;
    bool draggingSensitivity_ = false;
    bool draggingSoundSensitivity_ = false;
    bool draggingScrubber_ = false;
    bool wasPlayingBeforeScrub_ = false;
    double scrubDragSeconds_ = 0.0;
    D2D1_RECT_F sensitivityRectDip_{};
    D2D1_RECT_F soundSensitivityRectDip_{};
    D2D1_RECT_F scrubberRectDip_{};
    D2D1_RECT_F sidebarListRectDip_{};
    float sidebarScrollOffsetDip_ = 0.0f;

    // Direct2D / DirectWrite / WIC Factories & Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBrandTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtSubSmall_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBodyRegular_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBodySemiBold_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBodyBold_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtSectionTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtShieldTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtMonoSemiBold_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtMonoBold_;
};

} // namespace maxwatch

#endif // _WIN32
