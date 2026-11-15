#ifdef _WIN32

#include "AppWindow.hpp"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using Microsoft::WRL::ComPtr;

namespace maxwatch {

namespace {

constexpr wchar_t kMainWndClass[] = L"MaxWatchStudioWindowClass";
constexpr wchar_t kModalWndClass[] = L"MaxWatchPasswordModalClass";
constexpr UINT_PTR kTimerIdMainUI = 1;
constexpr wchar_t kRegPrefsSubkey[] = L"Software\\MaxWatch";

std::string wideToUtf8String(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWideString(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

D2D1_COLOR_F rgba(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

std::wstring formatClockMMSS(double sec, bool withTenths = false) {
    double clamped = std::max(0.0, sec);
    int mins = static_cast<int>(clamped) / 60;
    double rem = clamped - mins * 60.0;
    wchar_t buf[64]{};
    if (withTenths) {
        std::swprintf(buf, 64, L"%02d:%04.1f", mins, rem);
    } else {
        std::swprintf(buf, 64, L"%02d:%02d", mins, static_cast<int>(rem));
    }
    return buf;
}

void applyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// -----------------------------------------------------------------------------
// Clean Native Dark-Styled Password Modal Helper
// -----------------------------------------------------------------------------

enum class PasswordModalKind {
    CreateVault,
    UnlockVault,
    ManageOrResetVault
};

struct PasswordModalState {
    PasswordModalKind kind = PasswordModalKind::UnlockVault;
    VaultManager* vault = nullptr;
    bool accepted = false;
    bool requestedReset = false;
    std::string resultPassword;

    HWND hwnd = nullptr;
    HWND edit1 = nullptr;
    HWND edit2 = nullptr;
    HWND edit3 = nullptr;
    HWND statusLabel = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HBRUSH bgBrush = nullptr;
    HBRUSH editBrush = nullptr;
};

constexpr int IDC_MODAL_EDIT1  = 3001;
constexpr int IDC_MODAL_EDIT2  = 3002;
constexpr int IDC_MODAL_EDIT3  = 3003;
constexpr int IDC_MODAL_RESET  = 3004;

void readEditUtf8Secure(HWND hEdit, std::string& outStr) {
    wchar_t wbuf[256]{};
    int len = GetWindowTextW(hEdit, wbuf, 255);
    if (len > 0) {
        outStr = wideToUtf8String(std::wstring(wbuf, static_cast<size_t>(len)));
    } else {
        outStr.clear();
    }
    SecureZeroMemory(wbuf, sizeof(wbuf));
}

LRESULT CALLBACK PasswordModalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PasswordModalState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC: {
        if (st && st->bgBrush) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(236, 240, 248));
            SetBkColor(hdc, RGB(19, 24, 36));
            return reinterpret_cast<LRESULT>(st->bgBrush);
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        if (st && st->editBrush) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(30, 38, 56));
            return reinterpret_cast<LRESULT>(st->editBrush);
        }
        break;
    }
    case WM_COMMAND: {
        if (!st) break;
        WORD id = LOWORD(wParam);
        if (id == IDOK) {
            std::string p1, p2, p3;
            if (st->edit1) readEditUtf8Secure(st->edit1, p1);
            if (st->edit2) readEditUtf8Secure(st->edit2, p2);
            if (st->edit3) readEditUtf8Secure(st->edit3, p3);

            if (st->kind == PasswordModalKind::CreateVault) {
                if (p1.size() < 4) {
                    SetWindowTextW(st->statusLabel, L"Password must be at least 4 characters.");
                } else if (p1 != p2) {
                    SetWindowTextW(st->statusLabel, L"Passwords do not match. Please try again.");
                } else {
                    st->resultPassword = p1;
                    st->accepted = true;
                    DestroyWindow(hwnd);
                }
            } else if (st->kind == PasswordModalKind::UnlockVault) {
                if (p1.empty()) {
                    SetWindowTextW(st->statusLabel, L"Please enter your vault password.");
                } else if (st->vault && !st->vault->unlock(p1)) {
                    SetWindowTextW(st->statusLabel, L"Incorrect password. Vault remains locked.");
                    SetWindowTextW(st->edit1, L"");
                    SetFocus(st->edit1);
                } else {
                    st->resultPassword = p1;
                    st->accepted = true;
                    DestroyWindow(hwnd);
                }
            } else if (st->kind == PasswordModalKind::ManageOrResetVault) {
                if (p2.size() < 4) {
                    SetWindowTextW(st->statusLabel, L"New password must be at least 4 characters.");
                } else if (p2 != p3) {
                    SetWindowTextW(st->statusLabel, L"New password confirmation does not match.");
                } else if (st->vault) {
                    try {
                        st->vault->changePassword(p1, p2);
                        st->accepted = true;
                        MessageBoxW(
                            hwnd,
                            L"Your AES-256 vault password was updated. All existing encrypted clips remain intact and playable with your new password.",
                            L"Vault Password Updated",
                            MB_OK | MB_ICONINFORMATION
                        );
                        DestroyWindow(hwnd);
                    } catch (const VaultException&) {
                        SetWindowTextW(st->statusLabel, L"Current password is incorrect.");
                    }
                }
            }

            if (!p1.empty()) crypto::secureZero(p1.data(), p1.size());
            if (!p2.empty()) crypto::secureZero(p2.data(), p2.size());
            if (!p3.empty()) crypto::secureZero(p3.data(), p3.size());
            return 0;
        }
        if (id == IDCANCEL) {
            st->accepted = false;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_MODAL_RESET && st->vault) {
            int res = MessageBoxW(
                hwnd,
                L"Because Max Watch uses true zero-backdoor AES-256-GCM encryption with zero cloud servers, a forgotten password cannot be recovered.\n\n"
                L"Do you want to permanently zero-wipe and delete all existing encrypted clips and start a brand-new Vault?",
                L"Reset Forgotten Vault & Delete All Clips?",
                MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2
            );
            if (res == IDYES) {
                st->vault->resetVaultDeletingAllClips();
                st->requestedReset = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) st->accepted = false;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void runPasswordModalWindow(HINSTANCE hInst, HWND parent, PasswordModalState& state) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PasswordModalWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kModalWndClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    auto S = [dpi](int v) -> int { return MulDiv(v, static_cast<int>(dpi), 96); };

    state.bgBrush = CreateSolidBrush(RGB(19, 24, 36));
    state.editBrush = CreateSolidBrush(RGB(30, 38, 56));
    state.fontTitle = CreateFontW(-S(16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.fontBody = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    int dlgW = S(460);
    int dlgH = (state.kind == PasswordModalKind::ManageOrResetVault) ? S(365)
             : (state.kind == PasswordModalKind::CreateVault ? S(315) : S(265));

    RECT pr{};
    if (parent && GetWindowRect(parent, &pr)) {
        // center over parent
    } else {
        pr = {100, 100, 1000, 700};
    }
    int posX = pr.left + ((pr.right - pr.left) - dlgW) / 2;
    int posY = pr.top + ((pr.bottom - pr.top) - dlgH) / 2;

    const wchar_t* winTitle =
        (state.kind == PasswordModalKind::CreateVault) ? L"Create AES-256 Vault Password"
      : (state.kind == PasswordModalKind::UnlockVault) ? L"Unlock Encrypted Video Vault"
      : L"Vault Password Settings";

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kModalWndClass,
        winTitle,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        posX, posY, dlgW, dlgH,
        parent, nullptr, hInst, &state
    );
    state.hwnd = hDlg;
    applyDarkTitleBar(hDlg);

    auto makeStatic = [&](const wchar_t* txt, int x, int y, int w, int h, bool bold = false) -> HWND {
        HWND c = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE, S(x), S(y), S(w), S(h), hDlg, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? state.fontTitle : state.fontBody), TRUE);
        return c;
    };
    auto makePassEdit = [&](int id, int x, int y, int w, int h) -> HWND {
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
            S(x), S(y), S(w), S(h), hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(state.fontBody), TRUE);
        return c;
    };
    auto makeBtn = [&](const wchar_t* txt, int id, int x, int y, int w, int h, bool defBtn = false) -> HWND {
        HWND c = CreateWindowExW(0, L"BUTTON", txt,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (defBtn ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
            S(x), S(y), S(w), S(h), hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(state.fontBody), TRUE);
        return c;
    };

    if (state.kind == PasswordModalKind::CreateVault) {
        makeStatic(L"Set Your Local AES-256 Vault Password", 22, 16, 405, 24, true);
        makeStatic(L"All motion clips encrypt in RAM before touching disk. Nobody can watch or delete footage without this password (100% offline, minimum 4 characters).",
            22, 44, 405, 38, false);
        makeStatic(L"New Password:", 22, 92, 140, 20);
        state.edit1 = makePassEdit(IDC_MODAL_EDIT1, 168, 89, 255, 26);
        makeStatic(L"Confirm Password:", 22, 128, 140, 20);
        state.edit2 = makePassEdit(IDC_MODAL_EDIT2, 168, 125, 255, 26);
        state.statusLabel = makeStatic(L"", 22, 164, 405, 22);
        makeBtn(L"Create Encrypted Vault", IDOK, 243, 202, 180, 32, true);
        makeBtn(L"Cancel", IDCANCEL, 145, 202, 88, 32, false);
    } else if (state.kind == PasswordModalKind::UnlockVault) {
        makeStatic(L"Enter Vault Password", 22, 16, 405, 24, true);
        makeStatic(L"Enter your password to decrypt clips in RAM for Play All, fast timeline ball scrubbing, Save & Next, or Delete & Next.",
            22, 44, 405, 38, false);
        makeStatic(L"Vault Password:", 22, 94, 130, 20);
        state.edit1 = makePassEdit(IDC_MODAL_EDIT1, 158, 91, 265, 26);
        state.statusLabel = makeStatic(L"", 22, 128, 405, 22);
        makeBtn(L"Forgot / Reset...", IDC_MODAL_RESET, 22, 162, 128, 32, false);
        makeBtn(L"Cancel", IDCANCEL, 212, 162, 85, 32, false);
        makeBtn(L"Unlock Vault", IDOK, 305, 162, 118, 32, true);
    } else {
        makeStatic(L"Change Vault Password or Reset Vault", 22, 16, 405, 24, true);
        makeStatic(L"Re-wraps your Curve25519 vault key in O(1) time without re-encrypting existing recordings.",
            22, 44, 405, 34, false);
        makeStatic(L"Current Password:", 22, 86, 145, 20);
        state.edit1 = makePassEdit(IDC_MODAL_EDIT1, 172, 83, 250, 26);
        makeStatic(L"New Password:", 22, 122, 145, 20);
        state.edit2 = makePassEdit(IDC_MODAL_EDIT2, 172, 119, 250, 26);
        makeStatic(L"Confirm New:", 22, 158, 145, 20);
        state.edit3 = makePassEdit(IDC_MODAL_EDIT3, 172, 155, 250, 26);
        state.statusLabel = makeStatic(L"", 22, 192, 405, 22);
        makeBtn(L"Reset Forgotten Vault...", IDC_MODAL_RESET, 22, 226, 168, 32, false);
        makeBtn(L"Cancel", IDCANCEL, 204, 226, 82, 32, false);
        makeBtn(L"Update Password", IDOK, 294, 226, 128, 32, true);
    }

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    if (state.edit1) SetFocus(state.edit1);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        SetActiveWindow(parent);
    }

    if (state.fontTitle) DeleteObject(state.fontTitle);
    if (state.fontBody) DeleteObject(state.fontBody);
    if (state.bgBrush) DeleteObject(state.bgBrush);
    if (state.editBrush) DeleteObject(state.editBrush);
}

} // namespace

// =============================================================================
// AppWindow Implementation
// =============================================================================

AppWindow::AppWindow(const std::wstring& customVaultDir, bool uiSmokeTestMode, const std::wstring& smokeOutDir)
    : uiSmokeTestMode_(uiSmokeTestMode),
      smokeOutDir_(smokeOutDir),
      vault_(customVaultDir),
      lastVaultInteractionSteady_(std::chrono::steady_clock::now()) {
    loadPreferences();

    sleepAndLock_.onScreenLocked = [this]() {
        if (vault_.isUnlocked()) {
            lockVaultImmediately(true);
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    };
    sleepAndLock_.onScreenUnlocked = [this]() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    };
    sleepAndLock_.onSystemWillPowerOff = [this]() {
        stopCurrentRecordingClipSynchronously();
        vault_.lock();
    };

    capture_.onMotionTriggered = [this]() {
        if (hwnd_ && !isTerminating_) {
            PostMessageW(hwnd_, WM_APP_MOTION_TRIGGERED, 0, 0);
        }
    };

    player_.onClipEnded = [this]() {
        if (hwnd_ && !isTerminating_) {
            PostMessageW(hwnd_, WM_APP_PLAYER_CLIP_ENDED, 0, 0);
        }
    };
}

AppWindow::~AppWindow() {
    isTerminating_ = true;
    removeTrayIcon();
    stopCurrentRecordingClipSynchronously();
    capture_.stopCapture();
    player_.unloadClip();
    sleepAndLock_.detachFromWindow();
    vault_.lock();
    if (hAccel_) {
        DestroyAcceleratorTable(hAccel_);
        hAccel_ = nullptr;
    }
    if (trayHicon_) {
        DestroyIcon(trayHicon_);
        trayHicon_ = nullptr;
    }
}

void AppWindow::loadPreferences() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPrefsSubkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"RecordAudioEnabled", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS) {
            isAudioRecordingEnabled_ = (val != 0);
        }
        sz = sizeof(DWORD);
        DWORD sens = 78;
        if (RegQueryValueExW(hKey, L"SensitivityPercent", nullptr, nullptr, reinterpret_cast<LPBYTE>(&sens), &sz) == ERROR_SUCCESS) {
            if (sens >= 10 && sens <= 100) {
                capture_.motionDetector().setSensitivityPercent(static_cast<double>(sens));
            }
        }
        RegCloseKey(hKey);
    }
    capture_.setSampleAudioEnabled(isAudioRecordingEnabled_);
}

void AppWindow::savePreferences() const {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPrefsSubkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD audioVal = isAudioRecordingEnabled_ ? 1 : 0;
        DWORD sensVal = static_cast<DWORD>( std::lround(capture_.motionDetector().sensitivityPercent()) );
        RegSetValueExW(hKey, L"RecordAudioEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&audioVal), sizeof(DWORD));
        RegSetValueExW(hKey, L"SensitivityPercent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sensVal), sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

bool AppWindow::createAndShow(HINSTANCE hInstance, int nCmdShow) {
    hInstance_ = hInstance;
    if (!ensureD2DFactories()) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kMainWndClass;
    RegisterClassExW(&wc);

    buildMenuBar();
    createAcceleratorTable();

    UINT dpi = GetDpiForSystem();
    int initW = MulDiv(1300, static_cast<int>(dpi), 96);
    int initH = MulDiv(820, static_cast<int>(dpi), 96);

    RECT wr{0, 0, initW, initH};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, TRUE, 0);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;
    int posX = std::max(0, (screenW - winW) / 2);
    int posY = std::max(0, (screenH - winH) / 2);

    hwnd_ = CreateWindowExW(
        0,
        kMainWndClass,
        L"Max Watch - Offline Motion Security & Encrypted Video Vault",
        WS_OVERLAPPEDWINDOW,
        posX, posY, winW, winH,
        nullptr,
        hMenuBar_,
        hInstance,
        this
    );
    if (!hwnd_) return false;

    applyDarkTitleBar(hwnd_);
    sleepAndLock_.attachToWindow(hwnd_);
    initTrayIcon();

    videoDevices_ = CaptureEngine::enumerateVideoDevices();
    audioDevices_ = CaptureEngine::enumerateAudioDevices();

    if (uiSmokeTestMode_) {
        capture_.startSyntheticCameraForSmokeTest(true);
    } else {
        std::wstring camId = videoDevices_.empty() ? L"" : videoDevices_[0].id;
        std::wstring micId = audioDevices_.empty() ? L"" : audioDevices_[0].id;
        capture_.startCapture(camId, micId, isAudioRecordingEnabled_);
    }

    refreshVaultUIState();
    SetTimer(hwnd_, kTimerIdMainUI, 80, nullptr);

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

int AppWindow::runMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (hAccel_ && hwnd_ && TranslateAcceleratorW(hwnd_, hAccel_, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void AppWindow::buildMenuBar() {
    hMenuBar_ = CreateMenu();

    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IDM_APP_ABOUT, L"&About Max Watch");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_APP_PASSWORD_SETTINGS, L"Vault &Password Settings...\tCtrl+,");
    AppendMenuW(fileMenu, MF_STRING, IDM_APP_LOCK_VAULT, L"&Lock Vault Immediately\tCtrl+L");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_APP_QUIT, L"&Quit Max Watch\tCtrl+Q");
    AppendMenuW(hMenuBar_, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");

    HMENU recMenu = CreatePopupMenu();
    AppendMenuW(recMenu, MF_STRING, IDM_REC_TOGGLE_ARM, L"&Start / Stop Motion Detection\tCtrl+R");
    AppendMenuW(recMenu, MF_STRING | (isAudioRecordingEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_REC_TOGGLE_AUDIO, L"Record Microphone &Audio\tCtrl+Shift+A");
    AppendMenuW(recMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(recMenu, MF_STRING, IDM_REC_CHOOSE_SAVE_FOLDER, L"Choose '&Save & Next' Folder...\tCtrl+Shift+S");
    AppendMenuW(recMenu, MF_STRING, IDM_REC_EXPORT_MP4, L"&Save Current Clip As MP4...\tCtrl+E");
    AppendMenuW(hMenuBar_, MF_POPUP, reinterpret_cast<UINT_PTR>(recMenu), L"&Recorder");

    HMENU playMenu = CreatePopupMenu();
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_ALL, L"&Play All Clips\tCtrl+P");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_PAUSE, L"Play / P&ause\tSpace");
    AppendMenuW(playMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_SPEED_1X, L"Normal Speed (&1x)\tCtrl+1");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_SPEED_4X, L"Fast Speed (&4x)\tCtrl+2");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_SPEED_12X, L"Ultra Speed (1&2x)\tCtrl+3");
    AppendMenuW(playMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_MUTE_TOGGLE, L"&Mute / Unmute Playback Audio\tCtrl+Shift+M");
    AppendMenuW(playMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_PREV_CLIP, L"Pre&vious Clip\t[");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_NEXT_CLIP, L"&Next Clip\t]");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_SAVE_AND_NEXT, L"&Save & Next Clip\tCtrl+S");
    AppendMenuW(playMenu, MF_STRING, IDM_PLAY_DELETE_AND_NEXT, L"&Delete & Next Clip\tCtrl+Del");
    AppendMenuW(hMenuBar_, MF_POPUP, reinterpret_cast<UINT_PTR>(playMenu), L"&Playback");

    HMENU winMenu = CreatePopupMenu();
    AppendMenuW(winMenu, MF_STRING, IDM_WIN_SWITCH_MODE, L"Switch Live Camera / Vault Player");
    AppendMenuW(winMenu, MF_STRING, IDM_WIN_SHOW_MAIN, L"Bring Main Window to Front");
    AppendMenuW(hMenuBar_, MF_POPUP, reinterpret_cast<UINT_PTR>(winMenu), L"&Window");
}

void AppWindow::createAcceleratorTable() {
    ACCEL accels[] = {
        {FVIRTKEY | FCONTROL, 'Q', IDM_APP_QUIT},
        {FVIRTKEY | FCONTROL, 'L', IDM_APP_LOCK_VAULT},
        {FVIRTKEY | FCONTROL, VK_OEM_COMMA, IDM_APP_PASSWORD_SETTINGS},
        {FVIRTKEY | FCONTROL, 'R', IDM_REC_TOGGLE_ARM},
        {FVIRTKEY | FCONTROL | FSHIFT, 'A', IDM_REC_TOGGLE_AUDIO},
        {FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_REC_CHOOSE_SAVE_FOLDER},
        {FVIRTKEY | FCONTROL, 'E', IDM_REC_EXPORT_MP4},
        {FVIRTKEY | FCONTROL, 'P', IDM_PLAY_ALL},
        {FVIRTKEY, VK_SPACE, IDM_PLAY_PAUSE},
        {FVIRTKEY | FCONTROL, '1', IDM_PLAY_SPEED_1X},
        {FVIRTKEY | FCONTROL, '2', IDM_PLAY_SPEED_4X},
        {FVIRTKEY | FCONTROL, '3', IDM_PLAY_SPEED_12X},
        {FVIRTKEY | FCONTROL | FSHIFT, 'M', IDM_PLAY_MUTE_TOGGLE},
        {FVIRTKEY, VK_OEM_4, IDM_PLAY_PREV_CLIP},
        {FVIRTKEY, VK_OEM_6, IDM_PLAY_NEXT_CLIP},
        {FVIRTKEY | FCONTROL, 'S', IDM_PLAY_SAVE_AND_NEXT},
        {FVIRTKEY | FCONTROL, VK_DELETE, IDM_PLAY_DELETE_AND_NEXT}
    };
    hAccel_ = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));
}

HICON AppWindow::createDynamicMFHicon(bool armed, bool recording) {
    if (!wicFactory_ || !d2dFactory_) {
        return LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
    }

    const UINT sz = 32;
    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(wicFactory_->CreateBitmap(sz, sz, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBmp))) {
        return LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2dFactory_->CreateWicBitmapRenderTarget(wicBmp.Get(), props, &rt))) {
        return LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
    }

    rt->BeginDraw();
    rt->Clear(rgba(0, 0, 0, 0));
    D2D1_COLOR_F dotCol = recording ? rgba(1.0f, 0.23f, 0.19f, 1.0f) : rgba(0.19f, 0.82f, 0.35f, 1.0f);
    drawMFLogoBadge(rt.Get(), D2D1::RectF(1.0f, 4.0f, 31.0f, 28.0f), (armed || recording) ? &dotCol : nullptr);
    rt->EndDraw();

    std::vector<uint8_t> bgra(sz * sz * 4, 0);
    WICRect rc{0, 0, static_cast<INT>(sz), static_cast<INT>(sz)};
    wicBmp->CopyPixels(&rc, sz * 4, static_cast<UINT>(bgra.size()), bgra.data());

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = static_cast<LONG>(sz);
    bi.bV5Height = -static_cast<LONG>(sz);
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* dibBits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !dibBits) {
        if (hColor) DeleteObject(hColor);
        return LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
    }
    std::memcpy(dibBits, bgra.data(), bgra.size());

    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return hIcon ? hIcon : LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APPICON));
}

void AppWindow::initTrayIcon() {
    trayHicon_ = createDynamicMFHicon(false, false);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON_CALLBACK;
    nid.hIcon = trayHicon_;
    wcscpy_s(nid.szTip, L"Max Watch - 100% Offline Security");
    trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid) == TRUE);
}

void AppWindow::updateTrayIcon() {
    if (!trayAdded_) return;
    HICON nextIcon = createDynamicMFHicon(isArmed_, isRecordingClip_);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = nextIcon;
    if (isRecordingClip_) {
        wcscpy_s(nid.szTip, L"Max Watch - RECORDING MOTION (Vault Locked)");
    } else if (isArmed_) {
        wcscpy_s(nid.szTip, L"Max Watch - ARMED & MONITORING");
    } else {
        wcscpy_s(nid.szTip, L"Max Watch - 100% Offline Security");
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (trayHicon_ && trayHicon_ != nextIcon) {
        DestroyIcon(trayHicon_);
    }
    trayHicon_ = nextIcon;
}

void AppWindow::removeTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void AppWindow::showTrayContextMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_WIN_SHOW_MAIN, L"Open Max Watch");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_REC_TOGGLE_ARM,
                isArmed_ ? L"Stop Motion Detection" : L"Start Motion Detection");
    AppendMenuW(m, MF_STRING | (isAudioRecordingEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_REC_TOGGLE_AUDIO, L"Record Microphone Audio");
    AppendMenuW(m, MF_STRING, IDM_PLAY_ALL, L"Play All Clips...");
    AppendMenuW(m, MF_STRING, IDM_APP_LOCK_VAULT, L"Lock Vault Now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_APP_QUIT, L"Quit Max Watch");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

// -----------------------------------------------------------------------------
// Direct2D / DirectWrite Setup & Rendering
// -----------------------------------------------------------------------------

bool AppWindow::ensureD2DFactories() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
            return false;
        }
    }
    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
    }
    if (!wicFactory_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_));
    }

    auto makeFmt = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        if (!out) {
            dwriteFactory_->CreateTextFormat(
                family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size, L"en-us", &out
            );
            if (out) {
                out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    };

    makeFmt(L"Segoe UI", 16.0f, DWRITE_FONT_WEIGHT_BOLD, fmtBrandTitle_);
    makeFmt(L"Segoe UI", 11.2f, DWRITE_FONT_WEIGHT_MEDIUM, fmtSubSmall_);
    makeFmt(L"Segoe UI", 12.2f, DWRITE_FONT_WEIGHT_REGULAR, fmtBodyRegular_);
    makeFmt(L"Segoe UI", 12.4f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmtBodySemiBold_);
    makeFmt(L"Segoe UI", 13.5f, DWRITE_FONT_WEIGHT_BOLD, fmtBodyBold_);
    makeFmt(L"Segoe UI", 14.2f, DWRITE_FONT_WEIGHT_BOLD, fmtSectionTitle_);
    makeFmt(L"Segoe UI", 20.0f, DWRITE_FONT_WEIGHT_BOLD, fmtShieldTitle_);
    makeFmt(L"Consolas", 11.8f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmtMonoSemiBold_);
    makeFmt(L"Consolas", 12.4f, DWRITE_FONT_WEIGHT_BOLD, fmtMonoBold_);
    return true;
}

bool AppWindow::ensureHwndRenderTarget() {
    if (renderTarget_) return true;
    if (!hwnd_ || !d2dFactory_) return false;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT widthPx = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    UINT heightPx = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

    UINT dpi = GetDpiForWindow(hwnd_);
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = static_cast<float>(dpi);
    rtProps.dpiY = static_cast<float>(dpi);

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(widthPx, heightPx)),
        &renderTarget_
    );
    return SUCCEEDED(hr);
}

void AppWindow::discardDeviceResources() {
    renderTarget_.Reset();
}

void AppWindow::renderWindow() {
    if (!ensureHwndRenderTarget()) return;

    D2D1_SIZE_F szDip = renderTarget_->GetSize();
    renderTarget_->BeginDraw();
    renderSceneToTarget(renderTarget_.Get(), szDip.width, szDip.height);
    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

bool AppWindow::saveCurrentSceneToPng(const std::wstring& filePath, UINT32 widthPx, UINT32 heightPx) {
    if (!ensureD2DFactories() || !wicFactory_) return false;

    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmap(widthPx, heightPx, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBitmap))) {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> offscreenRt;
    if (FAILED(d2dFactory_->CreateWicBitmapRenderTarget(wicBitmap.Get(), props, &offscreenRt))) {
        return false;
    }

    offscreenRt->BeginDraw();
    renderSceneToTarget(offscreenRt.Get(), static_cast<float>(widthPx), static_cast<float>(heightPx));
    if (FAILED(offscreenRt->EndDraw())) {
        return false;
    }

    std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());
    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(widthPx, heightPx))) return false;

    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pf);
    if (FAILED(frame->WriteSource(wicBitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

void AppWindow::drawMFLogoBadge(ID2D1RenderTarget* rt, D2D1_RECT_F rect, const D2D1_COLOR_F* dotColor) {
    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, whiteBrush, dotBrush;
    rt->CreateSolidColorBrush(rgba(0.08f, 0.11f, 0.17f, 0.96f), &bgBrush);
    rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.28f), &borderBrush);
    rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 1.0f), &whiteBrush);

    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(rect, 5.0f, 5.0f);
    rt->FillRoundedRectangle(pill, bgBrush.Get());
    rt->DrawRoundedRectangle(pill, borderBrush.Get(), 1.0f);

    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    float padX = w * 0.14f;
    float padY = h * 0.21f;
    float leftX = rect.left + padX;
    float rightX = rect.right - padX - (dotColor ? 4.5f : 0.0f);
    float topY = rect.top + padY;
    float botY = rect.bottom - padY;
    float capH = botY - topY;
    float totalW = rightX - leftX;
    float gap = totalW * 0.08f;
    float mW = totalW * 0.53f;
    float fW = totalW - mW - gap;
    float stroke = capH * 0.20f;

    ComPtr<ID2D1PathGeometry> geom;
    if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&geom))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(geom->Open(&sink))) {
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            float mMid = leftX + mW * 0.5f;
            sink->BeginFigure(D2D1::Point2F(leftX, botY), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(leftX, topY));
            sink->AddLine(D2D1::Point2F(leftX + stroke, topY));
            sink->AddLine(D2D1::Point2F(mMid, botY - capH * 0.44f));
            sink->AddLine(D2D1::Point2F(leftX + mW - stroke, topY));
            sink->AddLine(D2D1::Point2F(leftX + mW, topY));
            sink->AddLine(D2D1::Point2F(leftX + mW, botY));
            sink->AddLine(D2D1::Point2F(leftX + mW - stroke * 0.94f, botY));
            sink->AddLine(D2D1::Point2F(leftX + mW - stroke * 0.94f, topY + stroke * 1.4f));
            sink->AddLine(D2D1::Point2F(mMid + stroke * 0.2f, botY - capH * 0.24f));
            sink->AddLine(D2D1::Point2F(mMid - stroke * 0.2f, botY - capH * 0.24f));
            sink->AddLine(D2D1::Point2F(leftX + stroke * 0.94f, topY + stroke * 1.4f));
            sink->AddLine(D2D1::Point2F(leftX + stroke * 0.94f, botY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);

            float fL = leftX + mW + gap;
            float fR = fL + fW;
            float midTop = botY - capH * 0.56f;
            float midBot = midTop + stroke * 0.90f;
            sink->BeginFigure(D2D1::Point2F(fL, botY), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(fL, topY));
            sink->AddLine(D2D1::Point2F(fR, topY));
            sink->AddLine(D2D1::Point2F(fR, topY + stroke));
            sink->AddLine(D2D1::Point2F(fL + stroke, topY + stroke));
            sink->AddLine(D2D1::Point2F(fL + stroke, midTop));
            sink->AddLine(D2D1::Point2F(fL + fW * 0.88f, midTop));
            sink->AddLine(D2D1::Point2F(fL + fW * 0.88f, midBot));
            sink->AddLine(D2D1::Point2F(fL + stroke, midBot));
            sink->AddLine(D2D1::Point2F(fL + stroke, botY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);

            sink->Close();
            rt->FillGeometry(geom.Get(), whiteBrush.Get());
        }
    }

    if (dotColor) {
        rt->CreateSolidColorBrush(*dotColor, &dotBrush);
        D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(rect.right - 5.2f, rect.top + 5.2f), 3.1f, 3.1f);
        rt->FillEllipse(el, dotBrush.Get());
    }
}

float AppWindow::measureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt, float maxWidth) {
    if (!dwriteFactory_ || !fmt || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, maxWidth, 100.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void AppWindow::drawTextLine(
    ID2D1RenderTarget* rt,
    const std::wstring& text,
    IDWriteTextFormat* fmt,
    D2D1_RECT_F rect,
    D2D1_COLOR_F color,
    DWRITE_TEXT_ALIGNMENT align,
    bool wordWrap
) {
    if (!fmt || text.empty()) return;
    fmt->SetTextAlignment(align);
    fmt->SetWordWrapping(wordWrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(color, &br);
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, br.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void AppWindow::drawButton(
    ID2D1RenderTarget* rt,
    int hitId,
    D2D1_RECT_F rect,
    const std::wstring& label,
    ButtonStyle style,
    bool enabled,
    float /*fontSize*/
) {
    if (hitId != 0) {
        hitZones_.push_back({hitId, rect, enabled});
    }

    bool hovered = enabled && (hoveredHitId_ == hitId) && (hitId != 0);
    bool pressed = enabled && (pressedHitId_ == hitId) && (hitId != 0);

    D2D1_COLOR_F bg, border, fg;
    switch (style) {
    case ButtonStyle::Green:
        bg = rgba(0.11f, 0.44f, 0.24f, 0.95f);
        border = rgba(0.26f, 0.85f, 0.46f, 0.55f);
        fg = rgba(1, 1, 1, 1);
        break;
    case ButtonStyle::Red:
        bg = rgba(0.58f, 0.13f, 0.16f, 0.95f);
        border = rgba(1.0f, 0.36f, 0.38f, 0.55f);
        fg = rgba(1, 1, 1, 1);
        break;
    case ButtonStyle::Cyan:
        bg = rgba(0.08f, 0.38f, 0.56f, 0.95f);
        border = rgba(0.28f, 0.82f, 0.98f, 0.55f);
        fg = rgba(1, 1, 1, 1);
        break;
    case ButtonStyle::Teal:
        bg = rgba(0.09f, 0.36f, 0.42f, 0.95f);
        border = rgba(0.28f, 0.85f, 0.80f, 0.50f);
        fg = rgba(1, 1, 1, 1);
        break;
    case ButtonStyle::Yellow:
        bg = rgba(0.54f, 0.41f, 0.08f, 0.95f);
        border = rgba(0.98f, 0.82f, 0.28f, 0.65f);
        fg = rgba(1, 1, 1, 1);
        break;
    case ButtonStyle::Neutral:
    default:
        bg = rgba(0.14f, 0.18f, 0.26f, 0.95f);
        border = rgba(1.0f, 1.0f, 1.0f, 0.18f);
        fg = rgba(0.95f, 0.97f, 1.0f, 1.0f);
        break;
    }

    if (!enabled) {
        bg.a *= 0.45f;
        border.a *= 0.35f;
        fg.a *= 0.45f;
    } else if (pressed) {
        bg.r *= 0.80f; bg.g *= 0.80f; bg.b *= 0.80f;
    } else if (hovered) {
        bg.r = std::min(1.0f, bg.r + 0.06f);
        bg.g = std::min(1.0f, bg.g + 0.06f);
        bg.b = std::min(1.0f, bg.b + 0.06f);
        border.a = std::min(1.0f, border.a + 0.25f);
    }

    ComPtr<ID2D1SolidColorBrush> bgBr, borderBr;
    rt->CreateSolidColorBrush(bg, &bgBr);
    rt->CreateSolidColorBrush(border, &borderBr);

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 7.0f, 7.0f);
    rt->FillRoundedRectangle(rr, bgBr.Get());
    rt->DrawRoundedRectangle(rr, borderBr.Get(), 1.0f);

    drawTextLine(rt, label, fmtBodySemiBold_.Get(), rect, fg, DWRITE_TEXT_ALIGNMENT_CENTER, false);
}

void AppWindow::drawMeterBar(
    ID2D1RenderTarget* rt,
    D2D1_RECT_F rect,
    double fraction,
    double thresholdFraction,
    D2D1_COLOR_F fillColor
) {
    float h = rect.bottom - rect.top;
    float w = rect.right - rect.left;
    float r = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> bgBr, fillBr, borderBr, threshBr;
    rt->CreateSolidColorBrush(rgba(0.11f, 0.14f, 0.19f, 1.0f), &bgBr);
    rt->CreateSolidColorBrush(fillColor, &fillBr);
    rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.14f), &borderBr);

    D2D1_ROUNDED_RECT bgRr = D2D1::RoundedRect(rect, r, r);
    rt->FillRoundedRectangle(bgRr, bgBr.Get());

    float clamped = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    float fillW = clamped * w;
    if (fillW > 1.5f) {
        D2D1_RECT_F fRect = D2D1::RectF(rect.left, rect.top, rect.left + std::min(w, std::max(h, fillW)), rect.bottom);
        rt->FillRoundedRectangle(D2D1::RoundedRect(fRect, r, r), fillBr.Get());
    }

    if (thresholdFraction >= 0.0) {
        float tx = rect.left + std::clamp(static_cast<float>(thresholdFraction) * w, 2.0f, w - 2.0f);
        rt->CreateSolidColorBrush(rgba(1.0f, 0.84f, 0.15f, 1.0f), &threshBr);
        rt->DrawLine(D2D1::Point2F(tx, rect.top), D2D1::Point2F(tx, rect.bottom), threshBr.Get(), 2.5f);
    }

    rt->DrawRoundedRectangle(bgRr, borderBr.Get(), 1.0f);
}

void AppWindow::drawSensitivitySlider(ID2D1RenderTarget* rt, D2D1_RECT_F rect) {
    sensitivityRectDip_ = rect;
    hitZones_.push_back({static_cast<int>(HitId::RecSensitivitySlider), rect, true});

    float trackY = (rect.top + rect.bottom) * 0.5f;
    float leftX = rect.left + 8.0f;
    float rightX = rect.right - 8.0f;
    float span = std::max(1.0f, rightX - leftX);

    double sens = capture_.motionDetector().sensitivityPercent();
    float norm = static_cast<float>(std::clamp((sens - 10.0) / 90.0, 0.0, 1.0));
    float knobX = leftX + norm * span;

    ComPtr<ID2D1SolidColorBrush> trBg, trFill, knobBr, knobRing;
    rt->CreateSolidColorBrush(rgba(0.14f, 0.18f, 0.25f, 1.0f), &trBg);
    rt->CreateSolidColorBrush(rgba(0.20f, 0.76f, 0.96f, 1.0f), &trFill);
    rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 1.0f), &knobBr);
    rt->CreateSolidColorBrush(rgba(0.20f, 0.78f, 0.98f, 0.85f), &knobRing);

    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(leftX, trackY - 3.5f, rightX, trackY + 3.5f), 3.5f, 3.5f), trBg.Get());
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(leftX, trackY - 3.5f, knobX, trackY + 3.5f), 3.5f, 3.5f), trFill.Get());

    float rad = (draggingSensitivity_ || hoveredHitId_ == static_cast<int>(HitId::RecSensitivitySlider)) ? 8.5f : 7.2f;
    D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX, trackY), rad, rad);
    rt->FillEllipse(knob, knobBr.Get());
    rt->DrawEllipse(knob, knobRing.Get(), 2.0f);
}

void AppWindow::drawScrubberTimeline(ID2D1RenderTarget* rt, D2D1_RECT_F rect) {
    scrubberRectDip_ = rect;
    bool enabled = player_.hasClipLoaded() && player_.durationSeconds() > 0.01;
    hitZones_.push_back({static_cast<int>(HitId::PlayerScrubberTrack), rect, enabled});

    float leftX = rect.left + 14.0f;
    float rightX = rect.right - 14.0f;
    float trackW = std::max(1.0f, rightX - leftX);
    float trackY = rect.top + (rect.bottom - rect.top) * 0.56f;

    double dur = std::max(0.001, player_.durationSeconds());
    double cur = draggingScrubber_ ? scrubDragSeconds_ : player_.currentSeconds();
    float frac = enabled ? static_cast<float>(std::clamp(cur / dur, 0.0, 1.0)) : 0.0f;
    float ballX = leftX + frac * trackW;

    ComPtr<ID2D1SolidColorBrush> trBg, trBorder, trFill, glowBr, ballBr, ringBr;
    rt->CreateSolidColorBrush(rgba(0.14f, 0.17f, 0.23f, 1.0f), &trBg);
    rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.12f), &trBorder);
    rt->CreateSolidColorBrush(rgba(0.18f, 0.78f, 1.0f, 1.0f), &trFill);

    D2D1_ROUNDED_RECT trackRr = D2D1::RoundedRect(D2D1::RectF(leftX, trackY - 4.0f, rightX, trackY + 4.0f), 4.0f, 4.0f);
    rt->FillRoundedRectangle(trackRr, trBg.Get());
    if (ballX > leftX + 1.0f) {
        D2D1_ROUNDED_RECT fillRr = D2D1::RoundedRect(D2D1::RectF(leftX, trackY - 4.0f, ballX, trackY + 4.0f), 4.0f, 4.0f);
        rt->FillRoundedRectangle(fillRr, trFill.Get());
    }
    rt->DrawRoundedRectangle(trackRr, trBorder.Get(), 1.0f);

    float ballR = draggingScrubber_ ? 10.5f : (hoveredHitId_ == static_cast<int>(HitId::PlayerScrubberTrack) ? 9.5f : 8.5f);
    rt->CreateSolidColorBrush(rgba(0.15f, 0.78f, 1.0f, draggingScrubber_ ? 0.36f : 0.20f), &glowBr);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ballX, trackY), ballR + 4.5f, ballR + 4.5f), glowBr.Get());

    rt->CreateSolidColorBrush(rgba(0.98f, 0.99f, 1.0f, enabled ? 1.0f : 0.45f), &ballBr);
    rt->CreateSolidColorBrush(rgba(0.10f, 0.65f, 0.98f, enabled ? 1.0f : 0.45f), &ringBr);
    D2D1_ELLIPSE ballEl = D2D1::Ellipse(D2D1::Point2F(ballX, trackY), ballR, ballR);
    rt->FillEllipse(ballEl, ballBr.Get());
    rt->DrawEllipse(ballEl, ringBr.Get(), 2.2f);

    if (draggingScrubber_ && enabled) {
        wchar_t tbuf[32]{};
        if (cur >= 60.0) {
            int m = static_cast<int>(cur) / 60;
            std::swprintf(tbuf, 32, L"%d:%04.1f", m, cur - m * 60.0);
        } else {
            std::swprintf(tbuf, 32, L"%.1fs", cur);
        }
        std::wstring pillTxt = tbuf;
        float tw = measureTextWidth(pillTxt, fmtMonoBold_.Get(), 200.0f) + 14.0f;
        float px = std::clamp(ballX - tw * 0.5f, rect.left + 2.0f, rect.right - tw - 2.0f);
        D2D1_RECT_F pillR = D2D1::RectF(px, trackY - ballR - 24.0f, px + tw, trackY - ballR - 5.0f);
        ComPtr<ID2D1SolidColorBrush> pBg;
        rt->CreateSolidColorBrush(rgba(0.07f, 0.10f, 0.16f, 0.95f), &pBg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(pillR, 5.0f, 5.0f), pBg.Get());
        rt->DrawRoundedRectangle(D2D1::RoundedRect(pillR, 5.0f, 5.0f), ringBr.Get(), 1.0f);
        drawTextLine(rt, pillTxt, fmtMonoBold_.Get(), pillR, rgba(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void AppWindow::drawAspectFitFrame(ID2D1RenderTarget* rt, D2D1_RECT_F bounds, const BgraFrame& frame, bool /*isPlayer*/) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.size() < static_cast<size_t>(frame.width) * frame.height * 4) {
        return;
    }

    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f
    );

    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(rt->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height)),
            frame.pixels.data(),
            static_cast<UINT32>(frame.width * 4),
            bmpProps,
            &bmp))) {
        return;
    }

    float boxW = std::max(1.0f, bounds.right - bounds.left);
    float boxH = std::max(1.0f, bounds.bottom - bounds.top);
    float scale = std::min(boxW / static_cast<float>(frame.width), boxH / static_cast<float>(frame.height));
    float drawW = static_cast<float>(frame.width) * scale;
    float drawH = static_cast<float>(frame.height) * scale;
    float dx = bounds.left + (boxW - drawW) * 0.5f;
    float dy = bounds.top + (boxH - drawH) * 0.5f;

    rt->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->DrawBitmap(bmp.Get(), D2D1::RectF(dx, dy, dx + drawW, dy + drawH), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt->PopAxisAlignedClip();
}

void AppWindow::renderSceneToTarget(ID2D1RenderTarget* rt, float widthDip, float heightDip) {
    hitZones_.clear();

    rt->Clear(rgba(0.05f, 0.06f, 0.09f, 1.0f));

    // =========================================================================
    // 1. Top Studio Header Bar (height = 62)
    // =========================================================================
    const float topBarH = 62.0f;
    D2D1_RECT_F topBarRect = D2D1::RectF(0, 0, widthDip, topBarH);
    {
        ComPtr<ID2D1SolidColorBrush> barBr, divBr;
        rt->CreateSolidColorBrush(rgba(0.08f, 0.10f, 0.15f, 1.0f), &barBr);
        rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.09f), &divBr);
        rt->FillRectangle(topBarRect, barBr.Get());
        rt->DrawLine(D2D1::Point2F(0, topBarH - 0.5f), D2D1::Point2F(widthDip, topBarH - 0.5f), divBr.Get(), 1.0f);

        D2D1_COLOR_F dotCol = isRecordingClip_ ? rgba(1.0f, 0.23f, 0.19f, 1.0f) : rgba(0.19f, 0.82f, 0.35f, 1.0f);
        drawMFLogoBadge(rt, D2D1::RectF(16.0f, 17.0f, 58.0f, 45.0f), (isArmed_ || isRecordingClip_) ? &dotCol : nullptr);

        drawTextLine(rt, L"Max Watch", fmtBrandTitle_.Get(), D2D1::RectF(68.0f, 10.0f, 340.0f, 32.0f), rgba(1, 1, 1, 1));
        drawTextLine(rt, L"Free 100% offline security • Gift from Max Freedom Pollard",
                     fmtSubSmall_.Get(), D2D1::RectF(68.0f, 32.0f, 378.0f, 50.0f), rgba(0.68f, 0.70f, 0.75f, 1.0f));

        float bx = 386.0f;
        const float by = 14.0f;
        const float bh = 34.0f;

        std::wstring armTxt = isArmed_ ? L"■ Stop Motion Detection" : L"● Start Motion Detection";
        float armW = 186.0f;
        drawButton(rt, static_cast<int>(HitId::HeaderArm), D2D1::RectF(bx, by, bx + armW, by + bh),
                   armTxt, isArmed_ ? ButtonStyle::Red : ButtonStyle::Green);
        bx += armW + 10.0f;

        int clipCount = vault_.countVaultClips();
        wchar_t playBuf[64]{};
        std::swprintf(playBuf, 64, vault_.isUnlocked() ? L"▶ Play All (%d)" : L"🔒 Play All (%d)", clipCount);
        float playW = 128.0f;
        drawButton(rt, static_cast<int>(HitId::HeaderPlayAll), D2D1::RectF(bx, by, bx + playW, by + bh),
                   playBuf, ButtonStyle::Cyan);
        bx += playW + 10.0f;

        std::wstring modeTxt = isInPlayerMode_ ? L"Live Camera" : L"Vault Player";
        float modeW = 108.0f;
        drawButton(rt, static_cast<int>(HitId::HeaderModeSwitch), D2D1::RectF(bx, by, bx + modeW, by + bh),
                   modeTxt, ButtonStyle::Neutral);
        bx += modeW + 10.0f;

        std::wstring lockBtnTxt = !vault_.isInitialized() ? L"🔑 Set Vault Password"
                                : (vault_.isUnlocked() ? L"🔒 Lock Vault" : L"🔓 Unlock Vault");
        ButtonStyle lockStyle = vault_.isUnlocked() ? ButtonStyle::Green : ButtonStyle::Neutral;
        float lockW = !vault_.isInitialized() ? 162.0f : 126.0f;
        drawButton(rt, static_cast<int>(HitId::HeaderVaultLock), D2D1::RectF(bx, by, bx + lockW, by + bh),
                   lockBtnTxt, lockStyle);
        bx += lockW + 12.0f;

        std::wstring pillTxt;
        D2D1_COLOR_F pillColor;
        if (isRecordingClip_) {
            pillTxt = L"● RECORDING • LOCKED VAULT";
            pillColor = rgba(1.0f, 0.34f, 0.34f, 1.0f);
        } else if (isArmed_) {
            pillTxt = sleepAndLock_.isScreenCurrentlyLocked()
                ? L"● ARMED • SCREEN LOCKED"
                : L"● ARMED • MONITORING";
            pillColor = rgba(0.24f, 0.90f, 0.48f, 1.0f);
        } else {
            pillTxt = L"100% OFFLINE • READY";
            pillColor = rgba(0.32f, 0.86f, 0.82f, 1.0f);
        }
        if (bx < widthDip - 18.0f) {
            drawTextLine(rt, pillTxt, fmtMonoBold_.Get(), D2D1::RectF(bx, 0, widthDip - 18.0f, topBarH),
                         pillColor, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    // =========================================================================
    // 2. Right Encrypted Vault Sidebar (width = 306)
    // =========================================================================
    const float sidebarW = 306.0f;
    const float sidebarX = widthDip - sidebarW;
    D2D1_RECT_F sidebarRect = D2D1::RectF(sidebarX, topBarH, widthDip, heightDip);
    {
        ComPtr<ID2D1SolidColorBrush> sbBg, sbBorder;
        rt->CreateSolidColorBrush(rgba(0.07f, 0.08f, 0.12f, 1.0f), &sbBg);
        rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.08f), &sbBorder);
        rt->FillRectangle(sidebarRect, sbBg.Get());
        rt->DrawLine(D2D1::Point2F(sidebarX + 0.5f, topBarH), D2D1::Point2F(sidebarX + 0.5f, heightDip), sbBorder.Get(), 1.0f);

        drawTextLine(rt, L"Encrypted Video Vault", fmtSectionTitle_.Get(),
                     D2D1::RectF(sidebarX + 14.0f, topBarH + 12.0f, widthDip - 14.0f, topBarH + 34.0f), rgba(1, 1, 1, 1));
        drawTextLine(rt, L"AES-256-GCM • Zero Plaintext on Disk", fmtSubSmall_.Get(),
                     D2D1::RectF(sidebarX + 14.0f, topBarH + 34.0f, widthDip - 14.0f, topBarH + 52.0f),
                     rgba(0.35f, 0.85f, 0.78f, 1.0f));

        D2D1_RECT_F pwBtnRect = D2D1::RectF(sidebarX + 14.0f, heightDip - 44.0f, widthDip - 14.0f, heightDip - 14.0f);
        drawButton(rt, static_cast<int>(HitId::SidebarPasswordSettings), pwBtnRect,
                   L"Vault Password Settings...", ButtonStyle::Neutral);

        if (!vault_.isUnlocked()) {
            D2D1_RECT_F cardR = D2D1::RectF(sidebarX + 14.0f, topBarH + 66.0f, widthDip - 14.0f, topBarH + 282.0f);
            ComPtr<ID2D1SolidColorBrush> cBg, cBd;
            rt->CreateSolidColorBrush(rgba(0.10f, 0.12f, 0.18f, 1.0f), &cBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.10f), &cBd);
            rt->FillRoundedRectangle(D2D1::RoundedRect(cardR, 10.0f, 10.0f), cBg.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(cardR, 10.0f, 10.0f), cBd.Get(), 1.0f);

            drawTextLine(rt, L"🔒 Vault Locked", fmtSectionTitle_.Get(),
                         D2D1::RectF(cardR.left, cardR.top + 18.0f, cardR.right, cardR.top + 42.0f),
                         rgba(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);

            int cnt = vault_.countVaultClips();
            wchar_t cntBuf[96]{};
            std::swprintf(cntBuf, 96, L"%d encrypted clip%s stored safely offline.", cnt, cnt == 1 ? L"" : L"s");
            drawTextLine(rt, cntBuf, fmtBodyRegular_.Get(),
                         D2D1::RectF(cardR.left + 14.0f, cardR.top + 48.0f, cardR.right - 14.0f, cardR.top + 88.0f),
                         rgba(0.78f, 0.80f, 0.85f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER, true);

            drawButton(rt, static_cast<int>(HitId::SidebarUnlockBtn),
                       D2D1::RectF(cardR.left + 32.0f, cardR.top + 96.0f, cardR.right - 32.0f, cardR.top + 128.0f),
                       L"Unlock Vault / Play All", ButtonStyle::Cyan);

            drawTextLine(rt, L"New motion clips encrypt automatically even while locked or when your PC screen is locked (Win+L).",
                         fmtSubSmall_.Get(),
                         D2D1::RectF(cardR.left + 14.0f, cardR.top + 138.0f, cardR.right - 14.0f, cardR.bottom - 10.0f),
                         rgba(0.62f, 0.65f, 0.72f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER, true);
        } else {
            sidebarListRectDip_ = D2D1::RectF(sidebarX + 12.0f, topBarH + 64.0f, widthDip - 12.0f, pwBtnRect.top - 12.0f);
            ComPtr<ID2D1SolidColorBrush> listBg, selBg, hovBg, rowDiv;
            rt->CreateSolidColorBrush(rgba(0.09f, 0.11f, 0.16f, 1.0f), &listBg);
            rt->CreateSolidColorBrush(rgba(0.12f, 0.34f, 0.52f, 0.85f), &selBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.06f), &hovBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.06f), &rowDiv);

            rt->FillRoundedRectangle(D2D1::RoundedRect(sidebarListRectDip_, 8.0f, 8.0f), listBg.Get());

            if (unlockedClips_.empty()) {
                drawTextLine(rt, L"No recorded motion clips yet.", fmtBodyRegular_.Get(), sidebarListRectDip_,
                             rgba(0.62f, 0.66f, 0.72f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                const float rowH = 52.0f;
                float totalContentH = static_cast<float>(unlockedClips_.size()) * rowH;
                float viewH = std::max(1.0f, sidebarListRectDip_.bottom - sidebarListRectDip_.top);
                sidebarScrollOffsetDip_ = std::clamp(sidebarScrollOffsetDip_, 0.0f, std::max(0.0f, totalContentH - viewH));

                rt->PushAxisAlignedClip(sidebarListRectDip_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                for (size_t i = 0; i < unlockedClips_.size(); ++i) {
                    float ry = sidebarListRectDip_.top + static_cast<float>(i) * rowH - sidebarScrollOffsetDip_;
                    if (ry + rowH < sidebarListRectDip_.top || ry > sidebarListRectDip_.bottom) continue;

                    D2D1_RECT_F rowR = D2D1::RectF(sidebarListRectDip_.left, ry, sidebarListRectDip_.right, ry + rowH);
                    int rowHitId = static_cast<int>(HitId::SidebarClipRowBase) + static_cast<int>(i);

                    D2D1_RECT_F clippedHit = D2D1::RectF(
                        rowR.left,
                        std::max(rowR.top, sidebarListRectDip_.top),
                        rowR.right,
                        std::min(rowR.bottom, sidebarListRectDip_.bottom)
                    );
                    if (clippedHit.bottom > clippedHit.top) {
                        hitZones_.push_back({rowHitId, clippedHit, true});
                    }

                    if (static_cast<int>(i) == currentClipIndex_) {
                        rt->FillRectangle(rowR, selBg.Get());
                    } else if (hoveredHitId_ == rowHitId) {
                        rt->FillRectangle(rowR, hovBg.Get());
                    }

                    const auto& item = unlockedClips_[i];
                    std::wstring stamp = ClipTimestampFormatter::formatCompactWide(item.createdUnixMs);
                    wchar_t line1[128]{};
                    std::swprintf(line1, 128, L"#%zu  %s", i + 1, stamp.c_str());

                    double mb = static_cast<double>(item.fileSizeBytes) / 1048576.0;
                    wchar_t line2[160]{};
                    std::swprintf(
                        line2, 160, L"%.0fs • %.1f MB • %dx%d%s%s",
                        item.estimatedDurationSeconds,
                        mb,
                        item.width, item.height,
                        item.hasAudio ? L"" : L" • No Audio",
                        item.cleanlyFinalized ? L"" : L" • Recovered"
                    );

                    drawTextLine(rt, line1, fmtBodySemiBold_.Get(),
                                 D2D1::RectF(rowR.left + 10.0f, rowR.top + 6.0f, rowR.right - 8.0f, rowR.top + 27.0f),
                                 rgba(1, 1, 1, 1));
                    drawTextLine(rt, line2, fmtSubSmall_.Get(),
                                 D2D1::RectF(rowR.left + 10.0f, rowR.top + 27.0f, rowR.right - 8.0f, rowR.bottom - 5.0f),
                                 rgba(0.70f, 0.74f, 0.80f, 1.0f));
                    rt->DrawLine(D2D1::Point2F(rowR.left + 8.0f, rowR.bottom - 0.5f),
                                 D2D1::Point2F(rowR.right - 8.0f, rowR.bottom - 0.5f), rowDiv.Get(), 1.0f);
                }
                rt->PopAxisAlignedClip();
            }
        }
    }

    // =========================================================================
    // 3. Main Stage Container (0 .. sidebarX, topBarH .. heightDip)
    // =========================================================================
    const float stageLeft = 0.0f;
    const float stageRight = sidebarX;
    const float stageTop = topBarH;
    const float stageBottom = heightDip;

    if (!isInPlayerMode_) {
        // ---------------------------------------------------------------------
        // A. LIVE CAMERA & MOTION STUDIO STAGE
        // ---------------------------------------------------------------------
        const float deckH = 148.0f;
        D2D1_RECT_F deckR = D2D1::RectF(stageLeft + 14.0f, stageBottom - 14.0f - deckH, stageRight - 14.0f, stageBottom - 14.0f);
        D2D1_RECT_F previewR = D2D1::RectF(stageLeft + 14.0f, stageTop + 14.0f, stageRight - 14.0f, deckR.top - 12.0f);

        ComPtr<ID2D1SolidColorBrush> pvBg, pvBd, deckBg, deckBd;
        rt->CreateSolidColorBrush(rgba(0.03f, 0.04f, 0.06f, 1.0f), &pvBg);
        rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.10f), &pvBd);
        rt->FillRoundedRectangle(D2D1::RoundedRect(previewR, 12.0f, 12.0f), pvBg.Get());

        BgraFrame liveFrame;
        if (capture_.copyLatestPreviewFrame(liveFrame)) {
            drawAspectFitFrame(rt, previewR, liveFrame, false);
        } else if (!capture_.isCameraPermissionDenied()) {
            drawTextLine(rt, capture_.activeSummaryWide(), fmtBodySemiBold_.Get(), previewR,
                         rgba(0.65f, 0.70f, 0.78f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rt->DrawRoundedRectangle(D2D1::RoundedRect(previewR, 12.0f, 12.0f), pvBd.Get(), 1.0f);

        // Top-Left HUD Banner
        std::wstring hudText;
        if (capture_.isCameraPermissionDenied()) {
            hudText = L"Camera permission needed: click 'Open Windows Camera & Mic Settings' to allow access.";
        } else if (isRecordingClip_) {
            hudText = L"● MOTION RECORDING ACTIVE • " + capture_.activeSummaryWide();
        } else if (isArmed_) {
            hudText = L"● ARMED & MONITORING • " + capture_.activeSummaryWide();
        } else {
            hudText = L"LIVE MONITOR • " + capture_.activeSummaryWide();
        }
        float hudW = std::min((previewR.right - previewR.left) - 250.0f, measureTextWidth(hudText, fmtBodySemiBold_.Get(), 900.0f) + 26.0f);
        hudW = std::max(220.0f, hudW);
        D2D1_RECT_F hudBoxR = D2D1::RectF(previewR.left + 12.0f, previewR.top + 12.0f, previewR.left + 12.0f + hudW, previewR.top + 44.0f);
        {
            ComPtr<ID2D1SolidColorBrush> hBg, hBd;
            rt->CreateSolidColorBrush(rgba(0.04f, 0.06f, 0.10f, 0.82f), &hBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.14f), &hBd);
            rt->FillRoundedRectangle(D2D1::RoundedRect(hudBoxR, 8.0f, 8.0f), hBg.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(hudBoxR, 8.0f, 8.0f), hBd.Get(), 1.0f);
            drawTextLine(rt, hudText, fmtBodySemiBold_.Get(),
                         D2D1::RectF(hudBoxR.left + 12.0f, hudBoxR.top, hudBoxR.right - 12.0f, hudBoxR.bottom),
                         rgba(1, 1, 1, 1));
        }

        // Top-Right Red REC Badge (when recording motion)
        if (isRecordingClip_) {
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - recordingStartedSteady_).count();
            std::wstring recStr = L"● REC " + formatClockMMSS(elapsed, false) + L" • 1s Power-Cut-Safe Vault Sync";
            float rw = measureTextWidth(recStr, fmtMonoBold_.Get(), 500.0f) + 24.0f;
            D2D1_RECT_F recR = D2D1::RectF(previewR.right - 12.0f - rw, previewR.top + 12.0f, previewR.right - 12.0f, previewR.top + 44.0f);
            ComPtr<ID2D1SolidColorBrush> rBg;
            rt->CreateSolidColorBrush(rgba(0.72f, 0.08f, 0.12f, 0.92f), &rBg);
            rt->FillRoundedRectangle(D2D1::RoundedRect(recR, 8.0f, 8.0f), rBg.Get());
            drawTextLine(rt, recStr, fmtMonoBold_.Get(), recR, rgba(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Bottom-Left Live Unambiguous Timestamp Pill
        {
            std::wstring liveTs = ClipTimestampFormatter::formatWide();
            float tsw = measureTextWidth(liveTs, fmtMonoSemiBold_.Get(), 600.0f) + 20.0f;
            D2D1_RECT_F tsR = D2D1::RectF(previewR.left + 12.0f, previewR.bottom - 36.0f, previewR.left + 12.0f + tsw, previewR.bottom - 10.0f);
            ComPtr<ID2D1SolidColorBrush> tsBg, tsBd;
            rt->CreateSolidColorBrush(rgba(0.04f, 0.06f, 0.10f, 0.76f), &tsBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.16f), &tsBd);
            rt->FillRoundedRectangle(D2D1::RoundedRect(tsR, 6.0f, 6.0f), tsBg.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(tsR, 6.0f, 6.0f), tsBd.Get(), 1.0f);
            drawTextLine(rt, liveTs, fmtMonoSemiBold_.Get(), tsR, rgba(0.96f, 0.97f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (capture_.isCameraPermissionDenied()) {
            float cx = (previewR.left + previewR.right) * 0.5f;
            float cy = (previewR.top + previewR.bottom) * 0.5f;
            drawButton(rt, static_cast<int>(HitId::RecOpenPrivacySettings),
                       D2D1::RectF(cx - 165.0f, cy - 18.0f, cx + 165.0f, cy + 18.0f),
                       L"Open Windows Camera & Mic Settings", ButtonStyle::Yellow);
        }

        // Bottom Studio Deck
        rt->CreateSolidColorBrush(rgba(0.08f, 0.10f, 0.15f, 1.0f), &deckBg);
        rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.09f), &deckBd);
        rt->FillRoundedRectangle(D2D1::RoundedRect(deckR, 12.0f, 12.0f), deckBg.Get());
        rt->DrawRoundedRectangle(D2D1::RoundedRect(deckR, 12.0f, 12.0f), deckBd.Get(), 1.0f);

        const float row1Y = deckR.top + 14.0f;
        const float row2Y = row1Y + 34.0f;
        const float row3Y = row2Y + 34.0f;
        const float labelL = deckR.left + 16.0f;
        const float meterL = labelL + 148.0f;
        const float meterW = 260.0f;
        const float readoutL = meterL + meterW + 12.0f;
        const float rightCtlW = 216.0f;
        const float rightCtlL = deckR.right - 16.0f - rightCtlW;

        // Row 1: Live Motion + Meter + Readout + Camera Popup
        drawTextLine(rt, L"Live Motion:", fmtBodySemiBold_.Get(), D2D1::RectF(labelL, row1Y, meterL, row1Y + 24.0f), rgba(1, 1, 1, 1));
        double curMotion = capture_.latestMotionPercent();
        double trigThresh = capture_.motionDetector().triggerThresholdPercent();
        D2D1_COLOR_F mCol = (curMotion >= trigThresh) ? rgba(1.0f, 0.35f, 0.25f, 1.0f) : rgba(0.20f, 0.84f, 0.42f, 1.0f);
        drawMeterBar(rt, D2D1::RectF(meterL, row1Y + 5.0f, meterL + meterW, row1Y + 19.0f),
                     std::min(1.0, curMotion / 12.0), std::min(1.0, trigThresh / 12.0), mCol);

        wchar_t mBuf[96]{};
        std::swprintf(mBuf, 96, L"%.1f%% (Trigger: %.2f%%)", curMotion, trigThresh);
        drawTextLine(rt, mBuf, fmtMonoSemiBold_.Get(), D2D1::RectF(readoutL, row1Y, rightCtlL - 10.0f, row1Y + 24.0f),
                     rgba(0.84f, 0.86f, 0.92f, 1.0f));

        std::wstring camBtnLabel = videoDevices_.empty()
            ? (uiSmokeTestMode_ ? L"Cam: Studio Camera ▾" : L"No Camera Detected ▾")
            : (L"Cam: " + videoDevices_[std::clamp(selectedVideoIdx_, 0, static_cast<int>(videoDevices_.size()) - 1)].friendlyName + L" ▾");
        drawButton(rt, static_cast<int>(HitId::RecCameraPopup),
                   D2D1::RectF(rightCtlL, row1Y - 1.0f, rightCtlL + rightCtlW, row1Y + 25.0f),
                   camBtnLabel, ButtonStyle::Neutral);

        // Row 2: Sensitivity Scale + Slider + Readout + Mic Popup
        drawTextLine(rt, L"Motion Detect Scale:", fmtBodySemiBold_.Get(), D2D1::RectF(labelL, row2Y, meterL, row2Y + 24.0f), rgba(1, 1, 1, 1));
        drawSensitivitySlider(rt, D2D1::RectF(meterL, row2Y, meterL + meterW, row2Y + 24.0f));
        drawTextLine(rt, capture_.motionDetector().sensitivityLabelWide(), fmtMonoSemiBold_.Get(),
                     D2D1::RectF(readoutL, row2Y, rightCtlL - 10.0f, row2Y + 24.0f),
                     rgba(0.32f, 0.84f, 0.98f, 1.0f));

        std::wstring micBtnLabel = !isAudioRecordingEnabled_
            ? L"Mic: Disabled (Video-Only)"
            : (audioDevices_.empty()
                ? (uiSmokeTestMode_ ? L"Mic: Studio Mic ▾" : L"No Mic Detected ▾")
                : (L"Mic: " + audioDevices_[std::clamp(selectedAudioIdx_, 0, static_cast<int>(audioDevices_.size()) - 1)].friendlyName + L" ▾"));
        drawButton(rt, static_cast<int>(HitId::RecMicPopup),
                   D2D1::RectF(rightCtlL, row2Y - 1.0f, rightCtlL + rightCtlW, row2Y + 25.0f),
                   micBtnLabel, ButtonStyle::Neutral, isAudioRecordingEnabled_ && !audioDevices_.empty());

        // Row 3: Mic Audio + Meter + Readout + Audio ON/OFF Segmented Switch
        drawTextLine(rt, L"Mic Audio:", fmtBodySemiBold_.Get(), D2D1::RectF(labelL, row3Y, meterL, row3Y + 24.0f), rgba(1, 1, 1, 1));
        float db = capture_.latestAudioDb();
        double audioFrac = isAudioRecordingEnabled_ ? std::clamp((static_cast<double>(db) + 60.0) / 60.0, 0.0, 1.0) : 0.0;
        drawMeterBar(rt, D2D1::RectF(meterL, row3Y + 5.0f, meterL + meterW, row3Y + 19.0f),
                     audioFrac, -1.0, rgba(0.22f, 0.80f, 0.84f, 1.0f));

        std::wstring audTxt;
        if (!isAudioRecordingEnabled_) {
            audTxt = L"Audio Off (Video-Only)";
        } else if (!capture_.isMicOpen()) {
            audTxt = L"Mic Off (Video-Only)";
        } else {
            wchar_t ab[64]{};
            std::swprintf(ab, 64, L"48 kHz AAC (%.0f dB)", db);
            audTxt = ab;
        }
        drawTextLine(rt, audTxt, fmtMonoSemiBold_.Get(), D2D1::RectF(readoutL, row3Y, rightCtlL - 10.0f, row3Y + 24.0f),
                     rgba(0.80f, 0.84f, 0.90f, 1.0f));

        float halfSeg = (rightCtlW - 4.0f) * 0.5f;
        drawButton(rt, static_cast<int>(HitId::RecAudioOnSeg),
                   D2D1::RectF(rightCtlL, row3Y - 1.0f, rightCtlL + halfSeg, row3Y + 25.0f),
                   L"🔊 Audio ON", isAudioRecordingEnabled_ ? ButtonStyle::Teal : ButtonStyle::Neutral);
        drawButton(rt, static_cast<int>(HitId::RecAudioOffSeg),
                   D2D1::RectF(rightCtlL + halfSeg + 4.0f, row3Y - 1.0f, rightCtlL + rightCtlW, row3Y + 25.0f),
                   L"🔇 Audio OFF", !isAudioRecordingEnabled_ ? ButtonStyle::Red : ButtonStyle::Neutral);

        // Footer Note
        drawTextLine(
            rt,
            L"Locked-Screen Ready: Press Win+L to lock your PC while Max Watch keeps recording. Saves every 1s against power loss & flushes cleanly on exit.",
            fmtSubSmall_.Get(),
            D2D1::RectF(labelL, deckR.bottom - 29.0f, deckR.right - 16.0f, deckR.bottom - 7.0f),
            rgba(0.96f, 0.82f, 0.38f, 0.95f)
        );
    } else {
        // ---------------------------------------------------------------------
        // B. ENCRYPTED VAULT PLAYER STAGE
        // ---------------------------------------------------------------------
        if (!vault_.isUnlocked()) {
            float cx = (stageLeft + stageRight) * 0.5f;
            float cy = (stageTop + stageBottom) * 0.5f;
            D2D1_RECT_F cardR = D2D1::RectF(cx - 255.0f, cy - 122.0f, cx + 255.0f, cy + 122.0f);

            ComPtr<ID2D1SolidColorBrush> cBg, cBd;
            rt->CreateSolidColorBrush(rgba(0.08f, 0.10f, 0.15f, 1.0f), &cBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.12f), &cBd);
            rt->FillRoundedRectangle(D2D1::RoundedRect(cardR, 14.0f, 14.0f), cBg.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(cardR, 14.0f, 14.0f), cBd.Get(), 1.0f);

            drawTextLine(rt, L"🔒 Password-Protected Video Vault", fmtShieldTitle_.Get(),
                         D2D1::RectF(cardR.left + 20.0f, cardR.top + 24.0f, cardR.right - 20.0f, cardR.top + 58.0f),
                         rgba(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);

            drawTextLine(
                rt,
                L"All recordings are encrypted on the fly with AES-256-GCM. Unlock with your password to browse clips, drag the fast-scrub ball, or run Play All at 1x, 4x, or 12x speed.",
                fmtBodyRegular_.Get(),
                D2D1::RectF(cardR.left + 28.0f, cardR.top + 66.0f, cardR.right - 28.0f, cardR.top + 142.0f),
                rgba(0.78f, 0.81f, 0.86f, 1.0f),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                true
            );

            drawButton(rt, static_cast<int>(HitId::PlayerLockedUnlockBtn),
                       D2D1::RectF(cx - 200.0f, cardR.top + 162.0f, cx + 18.0f, cardR.top + 198.0f),
                       L"🔑 Enter Password & Play All", ButtonStyle::Cyan);
            drawButton(rt, static_cast<int>(HitId::PlayerLockedBackCamBtn),
                       D2D1::RectF(cx + 30.0f, cardR.top + 162.0f, cx + 200.0f, cardR.top + 198.0f),
                       L"Back to Live Camera", ButtonStyle::Neutral);
        } else {
            D2D1_RECT_F hdrR = D2D1::RectF(stageLeft + 14.0f, stageTop + 14.0f, stageRight - 14.0f, stageTop + 66.0f);
            D2D1_RECT_F ctlR = D2D1::RectF(stageLeft + 14.0f, stageBottom - 140.0f, stageRight - 14.0f, stageBottom - 14.0f);
            D2D1_RECT_F vidR = D2D1::RectF(stageLeft + 14.0f, hdrR.bottom + 10.0f, stageRight - 14.0f, ctlR.top - 10.0f);

            ComPtr<ID2D1SolidColorBrush> hdrBg, ctlBg, ctlBd, vidBg, vidBd;
            rt->CreateSolidColorBrush(rgba(0.08f, 0.11f, 0.16f, 1.0f), &hdrBg);
            rt->CreateSolidColorBrush(rgba(0.08f, 0.10f, 0.15f, 1.0f), &ctlBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.09f), &ctlBd);
            rt->CreateSolidColorBrush(rgba(0.0f, 0.0f, 0.0f, 1.0f), &vidBg);
            rt->CreateSolidColorBrush(rgba(1.0f, 1.0f, 1.0f, 0.10f), &vidBd);

            // Top Player Header
            rt->FillRoundedRectangle(D2D1::RoundedRect(hdrR, 10.0f, 10.0f), hdrBg.Get());

            float rEdge = hdrR.right - 12.0f;
            const float hBtnY1 = hdrR.top + 10.0f;
            const float hBtnY2 = hdrR.bottom - 10.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerLockAndCamBtn),
                       D2D1::RectF(rEdge - 166.0f, hBtnY1, rEdge, hBtnY2),
                       L"🔒 Lock & Live Camera", ButtonStyle::Green);
            rEdge -= 174.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerSaveAsBtn),
                       D2D1::RectF(rEdge - 86.0f, hBtnY1, rEdge, hBtnY2),
                       L"Save As...", ButtonStyle::Neutral);
            rEdge -= 94.0f;

            std::wstring folderLabel = chosenSaveFolder_.empty()
                ? L"📁 Save Folder: Choose..."
                : (L"📁 Save Folder: " + std::filesystem::path(chosenSaveFolder_).filename().wstring());
            float folderW = std::clamp(measureTextWidth(folderLabel, fmtBodySemiBold_.Get(), 260.0f) + 22.0f, 168.0f, 220.0f);
            drawButton(rt, static_cast<int>(HitId::PlayerChooseFolderBtn),
                       D2D1::RectF(rEdge - folderW, hBtnY1, rEdge, hBtnY2),
                       folderLabel, ButtonStyle::Neutral);
            rEdge -= (folderW + 8.0f);

            bool muted = player_.isMuted();
            drawButton(rt, static_cast<int>(HitId::PlayerAudioMuteBtn),
                       D2D1::RectF(rEdge - 118.0f, hBtnY1, rEdge, hBtnY2),
                       muted ? L"🔇 Audio: OFF" : L"🔊 Audio: ON",
                       muted ? ButtonStyle::Red : ButtonStyle::Teal);
            rEdge -= 128.0f;

            drawTextLine(rt, nowPlayingTitle_, fmtBodyBold_.Get(),
                         D2D1::RectF(hdrR.left + 14.0f, hdrR.top + 6.0f, std::max(hdrR.left + 60.0f, rEdge), hdrR.top + 28.0f),
                         rgba(1, 1, 1, 1));
            drawTextLine(rt, nowPlayingMeta_, fmtSubSmall_.Get(),
                         D2D1::RectF(hdrR.left + 14.0f, hdrR.top + 28.0f, std::max(hdrR.left + 60.0f, rEdge), hdrR.bottom - 5.0f),
                         rgba(0.70f, 0.74f, 0.80f, 1.0f));

            // Center RAM Video Viewport
            rt->FillRoundedRectangle(D2D1::RoundedRect(vidR, 12.0f, 12.0f), vidBg.Get());
            BgraFrame pFrame;
            if (player_.copyLatestVideoFrame(pFrame)) {
                drawAspectFitFrame(rt, vidR, pFrame, true);
            } else {
                drawTextLine(rt, unlockedClips_.empty()
                                 ? L"Vault is currently empty - arm motion detection to record clips."
                                 : L"Select a clip on the right or click Play All.",
                             fmtBodySemiBold_.Get(), vidR, rgba(0.62f, 0.66f, 0.74f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(vidR, 12.0f, 12.0f), vidBd.Get(), 1.0f);

            // Bottom Scrubber & Transport Controls Card
            rt->FillRoundedRectangle(D2D1::RoundedRect(ctlR, 12.0f, 12.0f), ctlBg.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(ctlR, 12.0f, 12.0f), ctlBd.Get(), 1.0f);

            drawTextLine(rt, L"Drag Ball to Scrub Fast:", fmtBodySemiBold_.Get(),
                         D2D1::RectF(ctlR.left + 16.0f, ctlR.top + 20.0f, ctlR.left + 170.0f, ctlR.top + 52.0f),
                         rgba(0.88f, 0.90f, 0.94f, 1.0f));

            double curT = draggingScrubber_ ? scrubDragSeconds_ : player_.currentSeconds();
            double totT = player_.durationSeconds();
            std::wstring timeStr = formatClockMMSS(curT, true) + L" / " + formatClockMMSS(totT, true);
            drawTextLine(rt, timeStr, fmtMonoBold_.Get(),
                         D2D1::RectF(ctlR.right - 166.0f, ctlR.top + 20.0f, ctlR.right - 16.0f, ctlR.top + 52.0f),
                         rgba(0.28f, 0.86f, 0.98f, 1.0f), DWRITE_TEXT_ALIGNMENT_TRAILING);

            drawScrubberTimeline(rt, D2D1::RectF(ctlR.left + 172.0f, ctlR.top + 6.0f, ctlR.right - 172.0f, ctlR.top + 62.0f));

            // Transport Button Row
            float tx = ctlR.left + 14.0f;
            const float ty1 = ctlR.bottom - 48.0f;
            const float ty2 = ctlR.bottom - 14.0f;
            bool hasClips = !unlockedClips_.empty();

            drawButton(rt, static_cast<int>(HitId::PlayerPlayAllBtn), D2D1::RectF(tx, ty1, tx + 98.0f, ty2),
                       L"▶ Play All", ButtonStyle::Cyan, hasClips);
            tx += 104.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerPrevBtn), D2D1::RectF(tx, ty1, tx + 72.0f, ty2),
                       L"⏮ Prev", ButtonStyle::Neutral, hasClips && currentClipIndex_ > 0);
            tx += 78.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerPlayPauseBtn), D2D1::RectF(tx, ty1, tx + 106.0f, ty2),
                       player_.isPlaying() ? L"⏸ Pause" : L"⏯ Play/Pause", ButtonStyle::Neutral, player_.hasClipLoaded());
            tx += 112.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerNextBtn), D2D1::RectF(tx, ty1, tx + 74.0f, ty2),
                       L"Next ⏭", ButtonStyle::Neutral,
                       hasClips && currentClipIndex_ >= 0 && currentClipIndex_ + 1 < static_cast<int>(unlockedClips_.size()));
            tx += 82.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerSaveAndNextBtn), D2D1::RectF(tx, ty1, tx + 124.0f, ty2),
                       L"💾 Save & Next", ButtonStyle::Green, hasClips);
            tx += 132.0f;

            drawButton(rt, static_cast<int>(HitId::PlayerDeleteAndNextBtn), D2D1::RectF(tx, ty1, tx + 132.0f, ty2),
                       L"🗑 Delete & Next", ButtonStyle::Red, hasClips && currentClipIndex_ >= 0);

            // Right-aligned Speed Control: Normal (1x) | 4x | 12x
            float spRight = ctlR.right - 14.0f;
            drawButton(rt, static_cast<int>(HitId::PlayerSpeed12xSeg), D2D1::RectF(spRight - 48.0f, ty1, spRight, ty2),
                       L"12x", (std::abs(selectedPlaybackSpeed_ - 12.0f) < 0.1f) ? ButtonStyle::Cyan : ButtonStyle::Neutral);
            spRight -= 52.0f;
            drawButton(rt, static_cast<int>(HitId::PlayerSpeed4xSeg), D2D1::RectF(spRight - 44.0f, ty1, spRight, ty2),
                       L"4x", (std::abs(selectedPlaybackSpeed_ - 4.0f) < 0.1f) ? ButtonStyle::Cyan : ButtonStyle::Neutral);
            spRight -= 48.0f;
            drawButton(rt, static_cast<int>(HitId::PlayerSpeed1xSeg), D2D1::RectF(spRight - 94.0f, ty1, spRight, ty2),
                       L"Normal (1x)", (std::abs(selectedPlaybackSpeed_ - 1.0f) < 0.1f) ? ButtonStyle::Cyan : ButtonStyle::Neutral);
            spRight -= 100.0f;

            if (spRight - 52.0f > tx + 136.0f) {
                drawTextLine(rt, L"Speed:", fmtBodySemiBold_.Get(), D2D1::RectF(spRight - 52.0f, ty1, spRight, ty2),
                             rgba(0.82f, 0.85f, 0.90f, 1.0f), DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Mouse & Window Message Handling
// -----------------------------------------------------------------------------

D2D1_POINT_2F AppWindow::clientPixelsToDip(int xPx, int yPx) const {
    UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    float scale = static_cast<float>(dpi) / 96.0f;
    if (scale <= 0.01f) scale = 1.0f;
    return D2D1::Point2F(static_cast<float>(xPx) / scale, static_cast<float>(yPx) / scale);
}

int AppWindow::hitTestAt(D2D1_POINT_2F ptDip) const {
    for (auto it = hitZones_.rbegin(); it != hitZones_.rend(); ++it) {
        if (!it->enabled) continue;
        if (ptDip.x >= it->rect.left && ptDip.x <= it->rect.right &&
            ptDip.y >= it->rect.top && ptDip.y <= it->rect.bottom) {
            return it->id;
        }
    }
    return 0;
}

void AppWindow::onMouseDown(D2D1_POINT_2F ptDip) {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    int hit = hitTestAt(ptDip);
    pressedHitId_ = hit;

    if (hit == static_cast<int>(HitId::RecSensitivitySlider)) {
        draggingSensitivity_ = true;
        SetCapture(hwnd_);
        onMouseMove(ptDip);
        return;
    }

    if (hit == static_cast<int>(HitId::PlayerScrubberTrack) && player_.hasClipLoaded()) {
        draggingScrubber_ = true;
        wasPlayingBeforeScrub_ = player_.isPlaying();
        if (wasPlayingBeforeScrub_) player_.pause();
        SetCapture(hwnd_);
        onMouseMove(ptDip);
        return;
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::onMouseMove(D2D1_POINT_2F ptDip) {
    if (draggingSensitivity_) {
        float leftX = sensitivityRectDip_.left + 8.0f;
        float rightX = sensitivityRectDip_.right - 8.0f;
        float frac = std::clamp((ptDip.x - leftX) / std::max(1.0f, rightX - leftX), 0.0f, 1.0f);
        double pct = std::round(10.0 + frac * 90.0);
        capture_.motionDetector().setSensitivityPercent(pct);
        savePreferences();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (draggingScrubber_) {
        lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
        float leftX = scrubberRectDip_.left + 14.0f;
        float rightX = scrubberRectDip_.right - 14.0f;
        float frac = std::clamp((ptDip.x - leftX) / std::max(1.0f, rightX - leftX), 0.0f, 1.0f);
        scrubDragSeconds_ = frac * player_.durationSeconds();
        player_.scrubToSeconds(scrubDragSeconds_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int newHover = hitTestAt(ptDip);
    if (newHover != hoveredHitId_) {
        hoveredHitId_ = newHover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::onMouseUp(D2D1_POINT_2F ptDip) {
    if (draggingSensitivity_) {
        draggingSensitivity_ = false;
        ReleaseCapture();
        pressedHitId_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (draggingScrubber_) {
        draggingScrubber_ = false;
        ReleaseCapture();
        player_.scrubToSeconds(scrubDragSeconds_);
        if (wasPlayingBeforeScrub_) {
            player_.play(selectedPlaybackSpeed_);
        }
        pressedHitId_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int releasedOn = hitTestAt(ptDip);
    int activated = (pressedHitId_ != 0 && pressedHitId_ == releasedOn) ? releasedOn : 0;
    pressedHitId_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (activated != 0) {
        dispatchHitCommand(activated);
    }
}

void AppWindow::onMouseWheel(short wheelDelta, D2D1_POINT_2F ptDip) {
    if (vault_.isUnlocked() &&
        ptDip.x >= sidebarListRectDip_.left && ptDip.x <= sidebarListRectDip_.right &&
        ptDip.y >= sidebarListRectDip_.top && ptDip.y <= sidebarListRectDip_.bottom) {
        lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
        sidebarScrollOffsetDip_ -= (static_cast<float>(wheelDelta) / 120.0f) * 44.0f;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::dispatchHitCommand(int hitId) {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();

    if (hitId >= static_cast<int>(HitId::SidebarClipRowBase) &&
        hitId < static_cast<int>(HitId::PlayerLockedUnlockBtn)) {
        int idx = hitId - static_cast<int>(HitId::SidebarClipRowBase);
        if (vault_.isUnlocked() && idx >= 0 && idx < static_cast<int>(unlockedClips_.size())) {
            isPlayAllSequential_ = false;
            setStageMode(true);
            loadAndPlayClip(idx, true);
        }
        return;
    }

    switch (static_cast<HitId>(hitId)) {
    case HitId::HeaderArm:              toggleArmedAction(); break;
    case HitId::HeaderPlayAll:
    case HitId::SidebarUnlockBtn:
    case HitId::PlayerLockedUnlockBtn:
    case HitId::PlayerPlayAllBtn:       playAllAction(); break;
    case HitId::HeaderModeSwitch:       setStageMode(!isInPlayerMode_); break;
    case HitId::HeaderVaultLock:
        if (!vault_.isInitialized()) {
            ensureVaultInitializedInteractive();
        } else if (vault_.isUnlocked()) {
            lockVaultImmediately(false);
        } else {
            playAllAction();
        }
        break;
    case HitId::RecOpenPrivacySettings: openWindowsPrivacySettings(); break;
    case HitId::RecCameraPopup:         showCameraPopupMenu(); break;
    case HitId::RecMicPopup:            showMicPopupMenu(); break;
    case HitId::RecAudioOnSeg:          setRecordAudioEnabled(true); break;
    case HitId::RecAudioOffSeg:         setRecordAudioEnabled(false); break;
    case HitId::SidebarPasswordSettings:showPasswordSettingsDialog(); break;
    case HitId::PlayerLockedBackCamBtn:
    case HitId::PlayerLockAndCamBtn:    setStageMode(false); break;
    case HitId::PlayerAudioMuteBtn:     togglePlayerMuteAction(); break;
    case HitId::PlayerChooseFolderBtn:  promptForSaveFolderInteractive(); break;
    case HitId::PlayerSaveAsBtn:        exportCurrentClipSaveAsAction(); break;
    case HitId::PlayerPrevBtn:          prevClipAction(); break;
    case HitId::PlayerPlayPauseBtn:     togglePlayPauseAction(); break;
    case HitId::PlayerNextBtn:          nextClipAction(false); break;
    case HitId::PlayerSaveAndNextBtn:   saveAndNextAction(); break;
    case HitId::PlayerDeleteAndNextBtn: deleteAndNextAction(); break;
    case HitId::PlayerSpeed1xSeg:       setPlaybackSpeedAction(1.0f); break;
    case HitId::PlayerSpeed4xSeg:       setPlaybackSpeedAction(4.0f); break;
    case HitId::PlayerSpeed12xSeg:      setPlaybackSpeedAction(12.0f); break;
    default: break;
    }
}

void AppWindow::handleMenuCommand(WORD cmdId) {
    if (cmdId >= IDM_CAM_DEVICE_BASE && cmdId < IDM_CAM_DEVICE_BASE + 100) {
        int idx = cmdId - IDM_CAM_DEVICE_BASE;
        if (idx >= 0 && idx < static_cast<int>(videoDevices_.size())) {
            selectedVideoIdx_ = idx;
            if (!isRecordingClip_) {
                std::wstring micId = (selectedAudioIdx_ >= 0 && selectedAudioIdx_ < static_cast<int>(audioDevices_.size()))
                    ? audioDevices_[selectedAudioIdx_].id : L"";
                capture_.startCapture(videoDevices_[idx].id, micId, isAudioRecordingEnabled_);
            } else {
                needsCaptureRefreshAfterClip_ = true;
            }
        }
        return;
    }
    if (cmdId >= IDM_MIC_DEVICE_BASE && cmdId < IDM_MIC_DEVICE_BASE + 100) {
        int idx = cmdId - IDM_MIC_DEVICE_BASE;
        if (idx >= 0 && idx < static_cast<int>(audioDevices_.size())) {
            selectedAudioIdx_ = idx;
            if (!isRecordingClip_) {
                std::wstring camId = (selectedVideoIdx_ >= 0 && selectedVideoIdx_ < static_cast<int>(videoDevices_.size()))
                    ? videoDevices_[selectedVideoIdx_].id : L"";
                capture_.startCapture(camId, audioDevices_[idx].id, isAudioRecordingEnabled_);
            } else {
                needsCaptureRefreshAfterClip_ = true;
            }
        }
        return;
    }

    switch (cmdId) {
    case IDM_APP_ABOUT:             showAboutDialog(); break;
    case IDM_APP_PASSWORD_SETTINGS: showPasswordSettingsDialog(); break;
    case IDM_APP_LOCK_VAULT:        lockVaultImmediately(false); break;
    case IDM_APP_QUIT:              quitApplicationCleanly(); break;
    case IDM_REC_TOGGLE_ARM:        toggleArmedAction(); break;
    case IDM_REC_TOGGLE_AUDIO:      setRecordAudioEnabled(!isAudioRecordingEnabled_); break;
    case IDM_REC_CHOOSE_SAVE_FOLDER:promptForSaveFolderInteractive(); break;
    case IDM_REC_EXPORT_MP4:        exportCurrentClipSaveAsAction(); break;
    case IDM_PLAY_ALL:              playAllAction(); break;
    case IDM_PLAY_PAUSE:            if (isInPlayerMode_ && vault_.isUnlocked()) togglePlayPauseAction(); break;
    case IDM_PLAY_SPEED_1X:         setPlaybackSpeedAction(1.0f); break;
    case IDM_PLAY_SPEED_4X:         setPlaybackSpeedAction(4.0f); break;
    case IDM_PLAY_SPEED_12X:        setPlaybackSpeedAction(12.0f); break;
    case IDM_PLAY_MUTE_TOGGLE:      togglePlayerMuteAction(); break;
    case IDM_PLAY_PREV_CLIP:        prevClipAction(); break;
    case IDM_PLAY_NEXT_CLIP:        nextClipAction(false); break;
    case IDM_PLAY_SAVE_AND_NEXT:    saveAndNextAction(); break;
    case IDM_PLAY_DELETE_AND_NEXT:  deleteAndNextAction(); break;
    case IDM_WIN_SWITCH_MODE:       setStageMode(!isInPlayerMode_); break;
    case IDM_WIN_SHOW_MAIN:
        ShowWindow(hwnd_, SW_RESTORE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        break;
    default: break;
    }
}

// -----------------------------------------------------------------------------
// Core Recorder, Vault & Player Actions
// -----------------------------------------------------------------------------

void AppWindow::setStageMode(bool playerMode) {
    isInPlayerMode_ = playerMode;
    if (!playerMode) {
        // Security invariant: Live Camera / Monitoring mode ALWAYS keeps Vault strictly LOCKED!
        unloadActivePlayerClip();
        vault_.lock();
    } else {
        lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    }
    refreshVaultUIState();
}

void AppWindow::refreshVaultUIState() {
    if (vault_.isUnlocked()) {
        auto activeW = capture_.currentSegmentWriter();
        std::wstring activePath = activeW ? activeW->filePath() : L"";
        unlockedClips_ = vault_.listSummaries(activePath);
        if (currentClipIndex_ >= static_cast<int>(unlockedClips_.size())) {
            currentClipIndex_ = static_cast<int>(unlockedClips_.size()) - 1;
        }
    } else {
        unlockedClips_.clear();
        currentClipIndex_ = -1;
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool AppWindow::showCreatePasswordModal(std::string& outPassword) {
    PasswordModalState st;
    st.kind = PasswordModalKind::CreateVault;
    st.vault = &vault_;
    runPasswordModalWindow(hInstance_, hwnd_, st);
    if (st.accepted) {
        outPassword = st.resultPassword;
        crypto::secureZero(st.resultPassword.data(), st.resultPassword.size());
        return true;
    }
    return false;
}

bool AppWindow::showUnlockPasswordModal(std::string& outPassword, bool& outRequestedReset) {
    PasswordModalState st;
    st.kind = PasswordModalKind::UnlockVault;
    st.vault = &vault_;
    runPasswordModalWindow(hInstance_, hwnd_, st);
    outRequestedReset = st.requestedReset;
    if (st.accepted) {
        outPassword = st.resultPassword;
        crypto::secureZero(st.resultPassword.data(), st.resultPassword.size());
        return true;
    }
    return false;
}

void AppWindow::showChangeOrResetPasswordModal() {
    PasswordModalState st;
    st.kind = PasswordModalKind::ManageOrResetVault;
    st.vault = &vault_;
    runPasswordModalWindow(hInstance_, hwnd_, st);
    if (st.requestedReset) {
        unloadActivePlayerClip();
        refreshVaultUIState();
        ensureVaultInitializedInteractive();
    }
}

bool AppWindow::ensureVaultInitializedInteractive() {
    if (vault_.isInitialized()) return true;
    std::string pass;
    if (!showCreatePasswordModal(pass)) return false;
    try {
        vault_.initializeVault(pass);
        crypto::secureZero(pass.data(), pass.size());
        vault_.lock();
        refreshVaultUIState();
        return true;
    } catch (const std::exception& ex) {
        crypto::secureZero(pass.data(), pass.size());
        MessageBoxW(hwnd_, utf8ToWideString(ex.what()).c_str(), L"Vault Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

bool AppWindow::ensureVaultUnlockedInteractive() {
    if (vault_.isUnlocked()) return true;
    if (!vault_.isInitialized()) {
        if (!ensureVaultInitializedInteractive()) return false;
    }
    std::string pass;
    bool didReset = false;
    bool ok = showUnlockPasswordModal(pass, didReset);
    if (!pass.empty()) crypto::secureZero(pass.data(), pass.size());
    if (didReset) {
        unloadActivePlayerClip();
        refreshVaultUIState();
        return false;
    }
    refreshVaultUIState();
    return ok && vault_.isUnlocked();
}

void AppWindow::lockVaultImmediately(bool keepCameraStage) {
    unloadActivePlayerClip();
    vault_.lock();
    isPlayAllSequential_ = false;
    if (!keepCameraStage) {
        isInPlayerMode_ = false;
    }
    refreshVaultUIState();
}

void AppWindow::toggleArmedAction() {
    if (isArmed_) {
        isArmed_ = false;
        stopCurrentRecordingClipSynchronously();
        sleepAndLock_.setArmedPreventingIdleSleep(false);
        updateTrayIcon();
        refreshVaultUIState();
        return;
    }

    if (!ensureVaultInitializedInteractive()) {
        return;
    }

    // Security invariant: arming switches to Live Camera and locks the vault immediately!
    setStageMode(false);
    vault_.lock();
    capture_.motionDetector().resetWarmup(1.1);
    isArmed_ = true;
    sleepAndLock_.setArmedPreventingIdleSleep(true);
    updateTrayIcon();
    refreshVaultUIState();
}

void AppWindow::setRecordAudioEnabled(bool enabled) {
    isAudioRecordingEnabled_ = enabled;
    capture_.setSampleAudioEnabled(enabled);
    savePreferences();

    if (hMenuBar_) {
        CheckMenuItem(hMenuBar_, IDM_REC_TOGGLE_AUDIO, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    }

    if (isRecordingClip_) {
        needsCaptureRefreshAfterClip_ = true;
    } else if (!uiSmokeTestMode_) {
        std::wstring camId = (selectedVideoIdx_ >= 0 && selectedVideoIdx_ < static_cast<int>(videoDevices_.size()))
            ? videoDevices_[selectedVideoIdx_].id : L"";
        std::wstring micId = (selectedAudioIdx_ >= 0 && selectedAudioIdx_ < static_cast<int>(audioDevices_.size()))
            ? audioDevices_[selectedAudioIdx_].id : L"";
        capture_.startCapture(camId, micId, enabled);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::showCameraPopupMenu() {
    videoDevices_ = CaptureEngine::enumerateVideoDevices();
    HMENU m = CreatePopupMenu();
    if (videoDevices_.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No Camera Detected");
    } else {
        for (size_t i = 0; i < videoDevices_.size() && i < 99; ++i) {
            UINT flags = MF_STRING | (static_cast<int>(i) == selectedVideoIdx_ ? MF_CHECKED : 0);
            AppendMenuW(m, flags, IDM_CAM_DEVICE_BASE + i, videoDevices_[i].friendlyName.c_str());
        }
    }
    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

void AppWindow::showMicPopupMenu() {
    audioDevices_ = CaptureEngine::enumerateAudioDevices();
    HMENU m = CreatePopupMenu();
    if (audioDevices_.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No Microphone Detected");
    } else {
        for (size_t i = 0; i < audioDevices_.size() && i < 99; ++i) {
            UINT flags = MF_STRING | (static_cast<int>(i) == selectedAudioIdx_ ? MF_CHECKED : 0);
            AppendMenuW(m, flags, IDM_MIC_DEVICE_BASE + i, audioDevices_[i].friendlyName.c_str());
        }
    }
    POINT pt{};
    GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

void AppWindow::openWindowsPrivacySettings() {
    ShellExecuteW(hwnd_, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
}

void AppWindow::handleMotionTriggeredOnMain() {
    if (!isArmed_ || isTerminating_) return;

    auto now = std::chrono::steady_clock::now();
    auto minHold = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(kMinimumClipDurationSec)
    );
    auto newDeadline = now + minHold;

    if (isRecordingClip_) {
        if (newDeadline > recordingDeadlineSteady_) {
            recordingDeadlineSteady_ = newDeadline;
        }
        return;
    }

    // Strict locked-while-saving room-tamper guard
    if (vault_.isUnlocked()) {
        double idleSec = std::chrono::duration<double>(now - lastVaultInteractionSteady_).count();
        bool unattendedPlayer = isInPlayerMode_ && !player_.isPlaying() && !draggingScrubber_ && (idleSec > 20.0);
        if (!isInPlayerMode_ || unattendedPlayer) {
            lockVaultImmediately(false);
        }
    }

    try {
        int w = std::max(320, capture_.activeWidth());
        int h = std::max(240, capture_.activeHeight());
        bool recAudio = isAudioRecordingEnabled_ && capture_.isMicOpen();

        auto clipWriter = vault_.createClipWriter(w, h, recAudio);
        auto segWriter = std::make_shared<VaultSegmentWriter>(std::move(clipWriter), w, h, recAudio);
        capture_.attachActiveSegmentWriter(segWriter);

        isRecordingClip_ = true;
        recordingStartedSteady_ = now;
        recordingDeadlineSteady_ = newDeadline;
        sleepAndLock_.setRecordingShutdownGuard(true);
        updateTrayIcon();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } catch (...) {
    }
}

void AppWindow::stopCurrentRecordingClipSynchronously() {
    auto activeWriter = capture_.detachActiveSegmentWriter();
    isRecordingClip_ = false;
    sleepAndLock_.setRecordingShutdownGuard(false);

    if (activeWriter) {
        activeWriter->stopAndFinalizeSynchronously(3.0);
    }

    if (!isInPlayerMode_ && vault_.isUnlocked()) {
        vault_.lock();
    }

    if (needsCaptureRefreshAfterClip_ && !isTerminating_ && !uiSmokeTestMode_) {
        needsCaptureRefreshAfterClip_ = false;
        std::wstring camId = (selectedVideoIdx_ >= 0 && selectedVideoIdx_ < static_cast<int>(videoDevices_.size()))
            ? videoDevices_[selectedVideoIdx_].id : L"";
        std::wstring micId = (selectedAudioIdx_ >= 0 && selectedAudioIdx_ < static_cast<int>(audioDevices_.size()))
            ? audioDevices_[selectedAudioIdx_].id : L"";
        capture_.startCapture(camId, micId, isAudioRecordingEnabled_);
    }

    updateTrayIcon();
    refreshVaultUIState();
}

void AppWindow::playAllAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    isInPlayerMode_ = true;

    if (!ensureVaultUnlockedInteractive()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    refreshVaultUIState();
    if (unlockedClips_.empty()) {
        nowPlayingTitle_ = L"Vault Unlocked - 0 Recorded Clips";
        nowPlayingMeta_ = L"Arm motion detection on Live Camera to capture encrypted clips.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    isPlayAllSequential_ = true;
    int startIdx = (currentClipIndex_ >= 0 && currentClipIndex_ < static_cast<int>(unlockedClips_.size()))
        ? currentClipIndex_ : 0;
    loadAndPlayClip(startIdx, true);
}

void AppWindow::loadAndPlayClip(int index, bool autoPlay) {
    if (!vault_.isUnlocked() || index < 0 || index >= static_cast<int>(unlockedClips_.size())) {
        return;
    }
    currentClipIndex_ = index;
    const auto& sum = unlockedClips_[index];

    try {
        std::shared_ptr<VaultClipReader> reader(vault_.openClipReader(sum.filePath).release());
        std::string err;
        if (!player_.loadClip(reader, &err)) {
            nowPlayingTitle_ = L"Unable to open clip";
            nowPlayingMeta_ = utf8ToWideString(err);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        wchar_t tBuf[192]{};
        std::swprintf(tBuf, 192, L"Clip %d of %zu  •  %s",
                      index + 1, unlockedClips_.size(),
                      ClipTimestampFormatter::formatWide(sum.createdUnixMs).c_str());
        nowPlayingTitle_ = tBuf;

        double mb = static_cast<double>(sum.fileSizeBytes) / 1048576.0;
        wchar_t mBuf[192]{};
        std::swprintf(mBuf, 192, L"%dx%d • %.1f MB • %s • Decrypted in RAM (0 temp files)",
                      sum.width, sum.height, mb,
                      sum.hasAudio ? L"48 kHz AAC" : L"Silent Video-Only");
        nowPlayingMeta_ = mBuf;

        if (autoPlay) {
            player_.play(selectedPlaybackSpeed_);
        }
    } catch (const std::exception& ex) {
        nowPlayingMeta_ = L"Error: " + utf8ToWideString(ex.what());
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::unloadActivePlayerClip() {
    player_.unloadClip();
}

void AppWindow::togglePlayPauseAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (!player_.hasClipLoaded()) {
        if (!unlockedClips_.empty()) {
            loadAndPlayClip(std::max(0, currentClipIndex_), true);
        }
        return;
    }
    if (player_.isPlaying()) {
        player_.pause();
    } else {
        if (player_.currentSeconds() >= player_.durationSeconds() - 0.08) {
            player_.scrubToSeconds(0.0);
        }
        player_.play(selectedPlaybackSpeed_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::setPlaybackSpeedAction(float speed) {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    selectedPlaybackSpeed_ = speed;
    player_.setSpeedRate(speed);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::togglePlayerMuteAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    player_.setMuted(!player_.isMuted());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::prevClipAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (vault_.isUnlocked() && currentClipIndex_ > 0) {
        loadAndPlayClip(currentClipIndex_ - 1, true);
    }
}

void AppWindow::nextClipAction(bool fromAutoEnd) {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (!vault_.isUnlocked()) return;

    if (currentClipIndex_ + 1 < static_cast<int>(unlockedClips_.size())) {
        loadAndPlayClip(currentClipIndex_ + 1, true);
    } else if (fromAutoEnd) {
        isPlayAllSequential_ = false;
        player_.pause();
        nowPlayingMeta_ = L"Play All finished (" + std::to_wstring(unlockedClips_.size()) + L" clips).";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool AppWindow::promptForSaveFolderInteractive() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose Folder on Your PC for 'Save & Next'");
    dlg->SetOkButtonLabel(L"Choose Save Folder");

    if (FAILED(dlg->Show(hwnd_))) {
        return false;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item)) || !item) return false;

    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return false;
    std::wstring picked(rawPath);
    CoTaskMemFree(rawPath);

    if (vault_.isPathInsideVault(picked)) {
        MessageBoxW(
            hwnd_,
            L"Please choose a folder on your PC outside the internal Encrypted Vault (for example Desktop, Videos, or Downloads).",
            L"Choose Folder Outside Vault",
            MB_OK | MB_ICONWARNING
        );
        return false;
    }

    chosenSaveFolder_ = picked;
    nowPlayingMeta_ = L"Save & Next destination set to '" + std::filesystem::path(picked).filename().wstring() + L"'.";
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void AppWindow::saveAndNextAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (!vault_.isUnlocked()) {
        playAllAction();
        return;
    }
    if (unlockedClips_.empty()) {
        MessageBoxW(hwnd_, L"Your encrypted vault does not have any recorded clips yet.", L"No Clips to Save", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int targetIdx = (currentClipIndex_ >= 0 && currentClipIndex_ < static_cast<int>(unlockedClips_.size()))
        ? currentClipIndex_ : 0;
    if (currentClipIndex_ != targetIdx) {
        loadAndPlayClip(targetIdx, false);
    }

    if (chosenSaveFolder_.empty()) {
        bool wasPlaying = player_.isPlaying();
        if (wasPlaying) player_.pause();
        if (!promptForSaveFolderInteractive()) {
            if (wasPlaying) player_.play(selectedPlaybackSpeed_);
            return;
        }
    }

    try {
        const auto& sum = unlockedClips_[targetIdx];
        std::wstring savedPath = vault_.exportClipToDirectory(sum.filePath, chosenSaveFolder_);
        std::wstring savedName = std::filesystem::path(savedPath).filename().wstring();
        std::wstring folderName = std::filesystem::path(chosenSaveFolder_).filename().wstring();

        if (targetIdx + 1 < static_cast<int>(unlockedClips_.size())) {
            loadAndPlayClip(targetIdx + 1, true);
            nowPlayingMeta_ = L"Saved " + savedName + L" to '" + folderName + L"' • Playing next clip";
        } else {
            player_.pause();
            isPlayAllSequential_ = false;
            nowPlayingMeta_ = L"Saved " + savedName + L" to '" + folderName + L"' • Reached final clip";
        }
    } catch (const std::exception& ex) {
        MessageBoxW(hwnd_, utf8ToWideString(ex.what()).c_str(), L"Save & Next Failed", MB_OK | MB_ICONERROR);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::deleteAndNextAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (!vault_.isUnlocked() || currentClipIndex_ < 0 || currentClipIndex_ >= static_cast<int>(unlockedClips_.size())) {
        return;
    }

    std::wstring doomedPath = unlockedClips_[currentClipIndex_].filePath;
    int resumeIdx = currentClipIndex_;

    unloadActivePlayerClip();
    try {
        vault_.secureDeleteClip(doomedPath);
    } catch (...) {}

    refreshVaultUIState();
    if (unlockedClips_.empty()) {
        isPlayAllSequential_ = false;
        nowPlayingTitle_ = L"All clips deleted - Vault is empty";
        nowPlayingMeta_ = L"Securely zero-wiped and removed from disk.";
    } else if (resumeIdx < static_cast<int>(unlockedClips_.size())) {
        loadAndPlayClip(resumeIdx, true);
        nowPlayingMeta_ = L"Deleted previous clip • Playing next clip";
    } else {
        int lastIdx = static_cast<int>(unlockedClips_.size()) - 1;
        loadAndPlayClip(lastIdx, false);
        nowPlayingMeta_ = L"Deleted final clip • Reached end of vault";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::exportCurrentClipSaveAsAction() {
    lastVaultInteractionSteady_ = std::chrono::steady_clock::now();
    if (!vault_.isUnlocked() || currentClipIndex_ < 0 || currentClipIndex_ >= static_cast<int>(unlockedClips_.size())) {
        MessageBoxW(hwnd_, L"Unlock the vault and select a clip to export a decrypted MP4 copy.", L"Select Clip First", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto& sum = unlockedClips_[currentClipIndex_];
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return;
    }

    COMDLG_FILTERSPEC spec[] = {{L"MP4 Video (*.mp4)", L"*.mp4"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"mp4");
    std::wstring defName = std::filesystem::path(sum.fileName).stem().wstring() + L".mp4";
    dlg->SetFileName(defName.c_str());
    dlg->SetTitle(L"Save Decrypted MP4 Clip As...");

    if (FAILED(dlg->Show(hwnd_))) return;

    ComPtr<IShellItem> res;
    if (FAILED(dlg->GetResult(&res)) || !res) return;
    PWSTR rawDest = nullptr;
    if (FAILED(res->GetDisplayName(SIGDN_FILESYSPATH, &rawDest)) || !rawDest) return;
    std::wstring dest(rawDest);
    CoTaskMemFree(rawDest);

    try {
        vault_.exportClipAsMP4(sum.filePath, dest);
        nowPlayingMeta_ = L"Saved " + std::filesystem::path(dest).filename().wstring();
    } catch (const std::exception& ex) {
        MessageBoxW(hwnd_, utf8ToWideString(ex.what()).c_str(), L"Export Failed", MB_OK | MB_ICONERROR);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::showPasswordSettingsDialog() {
    if (!vault_.isInitialized()) {
        ensureVaultInitializedInteractive();
        return;
    }
    showChangeOrResetPasswordModal();
}

void AppWindow::showAboutDialog() {
    const wchar_t* text =
        L"Max Watch (v1.3.0)\n\n"
        L"A free, 100% offline, encrypted motion-detection security camera & video vault for Mac and Windows - gifted to the community by Max Freedom Pollard.\n\n"
        L"• Zero network access (zero network socket DLLs imported)\n"
        L"• Vault stays strictly locked while arming & saving clips against room tampering\n"
        L"• Works continuously even while your PC screen is locked (Win+L)\n"
        L"• Unambiguous international computer-time stamp on every clip\n"
        L"• One-click Audio ON / OFF switch for video-with-audio or silent video-only capture\n"
        L"• AES-256-GCM encrypted vault + password-locked Play All\n"
        L"• 1-second power-cut-safe recording + clean exit save\n"
        L"• Fast-drag timeline scrubber ball + 1x / 4x / 12x playback\n"
        L"• One-click Save & Next (to any PC folder) + Delete & Next\n\n"
        L"Stay safe, stay peaceful, and take good care of each other.";
    MessageBoxW(hwnd_, text, L"About Max Watch (v1.3.0)", MB_OK | MB_ICONINFORMATION);
}

void AppWindow::quitApplicationCleanly() {
    isTerminating_ = true;
    KillTimer(hwnd_, kTimerIdMainUI);
    sleepAndLock_.setArmedPreventingIdleSleep(false);
    stopCurrentRecordingClipSynchronously();
    capture_.stopCapture();
    unloadActivePlayerClip();
    vault_.lock();
    removeTrayIcon();
    DestroyWindow(hwnd_);
}

void AppWindow::onPeriodicTimerTick() {
    auto now = std::chrono::steady_clock::now();

    // Check recording deadline
    if (isRecordingClip_ && now >= recordingDeadlineSteady_) {
        stopCurrentRecordingClipSynchronously();
    }

    // 90s idle auto-lock when Vault Player is paused & untouched
    if (vault_.isUnlocked() && !uiSmokeTestMode_) {
        if (player_.isPlaying() || draggingScrubber_) {
            lastVaultInteractionSteady_ = now;
        } else {
            double idleSec = std::chrono::duration<double>(now - lastVaultInteractionSteady_).count();
            if (idleSec >= 90.0) {
                lockVaultImmediately(false);
            }
        }
    }

    if (uiSmokeTestMode_) {
        runSmokeTestStep();
    }

    if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::runSmokeTestStep() {
    smokeTickCounter_++;
    std::filesystem::path outDir = smokeOutDir_.empty()
        ? (std::filesystem::temp_directory_path() / L"MaxWatch_SmokeOut")
        : std::filesystem::path(smokeOutDir_);

    if (smokeTickCounter_ == 3) {
        if (!vault_.isInitialized()) {
            vault_.initializeVault("smoke-pass-1234");
        }
        capture_.motionDetector().resetWarmup(0.0);
        capture_.motionDetector().setSensitivityPercent(92.0);
        isArmed_ = true;
        handleMotionTriggeredOnMain();
    } else if (smokeTickCounter_ == 34) {
        // ~2.7 seconds into active synthetic motion recording: render & export Live Motion Studio screenshot!
        saveCurrentSceneToPng((outDir / L"windows_live_studio.png").wstring(), 1300, 800);
    } else if (smokeTickCounter_ == 42) {
        // Finalize clip, unlock vault, switch to Vault Player, load clip, scrub to 1.2s
        isArmed_ = false;
        stopCurrentRecordingClipSynchronously();
        vault_.unlock("smoke-pass-1234");
        isInPlayerMode_ = true;
        refreshVaultUIState();
        if (!unlockedClips_.empty()) {
            loadAndPlayClip(0, false);
            player_.scrubToSeconds(1.2);
        }
    } else if (smokeTickCounter_ == 54) {
        saveCurrentSceneToPng((outDir / L"windows_vault_player.png").wstring(), 1300, 800);
    } else if (smokeTickCounter_ >= 57) {
        quitApplicationCleanly();
    }
}

// -----------------------------------------------------------------------------
// Win32 Window Procedure
// -----------------------------------------------------------------------------

LRESULT CALLBACK AppWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AppWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        renderWindow();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) {
            if (vault_.isUnlocked()) {
                lockVaultImmediately(false);
            }
        } else if (renderTarget_) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            if (w > 0 && h > 0) {
                renderTarget_->Resize(D2D1::SizeU(w, h));
            }
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        mmi->ptMinTrackSize.x = MulDiv(1150, static_cast<int>(dpi), 96);
        mmi->ptMinTrackSize.y = MulDiv(680, static_cast<int>(dpi), 96);
        return 0;
    }

    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wParam);
        if (renderTarget_) {
            renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        }
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd_, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        onMouseDown(clientPixelsToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSEMOVE:
        onMouseMove(clientPixelsToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
        return 0;
    case WM_LBUTTONUP:
        onMouseUp(clientPixelsToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);
        onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), clientPixelsToDip(pt.x, pt.y));
        return 0;
    }

    case WM_COMMAND:
        handleMenuCommand(LOWORD(wParam));
        return 0;

    case WM_TIMER:
        if (wParam == kTimerIdMainUI) {
            onPeriodicTimerTick();
        }
        return 0;

    case WM_WTSSESSION_CHANGE:
        sleepAndLock_.handleSessionChange(wParam);
        return 0;

    case WM_APP_MOTION_TRIGGERED:
        handleMotionTriggeredOnMain();
        return 0;

    case WM_APP_PLAYER_CLIP_ENDED:
        if (isPlayAllSequential_) {
            nextClipAction(true);
        } else {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_TRAYICON_CALLBACK:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd_, SW_RESTORE);
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            showTrayContextMenu();
        }
        return 0;

    case WM_CLOSE:
        if (vault_.isUnlocked()) {
            lockVaultImmediately(false);
        }
        if (isArmed_ || isRecordingClip_) {
            // Keep monitoring/recording safely in the Windows System Tray!
            ShowWindow(hwnd_, SW_HIDE);
            if (!trayBalloonShownOnce_ && trayAdded_) {
                trayBalloonShownOnce_ = true;
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd_;
                nid.uID = 1;
                nid.uFlags = NIF_INFO;
                wcscpy_s(nid.szInfoTitle, L"Max Watch Armed in System Tray");
                wcscpy_s(nid.szInfo, L"Motion detection continues in the background. Click the MF tray icon to reopen or choose Quit Max Watch (Ctrl+Q).");
                nid.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
            return 0;
        }
        quitApplicationCleanly();
        return 0;

    case WM_QUERYENDSESSION:
        stopCurrentRecordingClipSynchronously();
        vault_.lock();
        return TRUE;

    case WM_ENDSESSION:
        if (wParam == TRUE) {
            stopCurrentRecordingClipSynchronously();
            vault_.lock();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace maxwatch

#endif // _WIN32
