#include "AppWindow.hpp"
#include "MediaPipeline.hpp"
#include "SelfTest.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void bindWin32StdHandlesForCli() {
    auto bindOne = [](DWORD stdHandleId, FILE* targetStream, const char* conFile) {
        HANDLE h = GetStdHandle(stdHandleId);
        DWORD ftype = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
        if (ftype == FILE_TYPE_PIPE || ftype == FILE_TYPE_DISK || ftype == FILE_TYPE_CHAR) {
            FILE* nulDummy = nullptr;
            freopen_s(&nulDummy, "nul", "w", targetStream);
            int osf = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_TEXT);
            if (osf != -1) {
                FILE* wrapped = _fdopen(osf, "w");
                if (wrapped) {
                    _dup2(_fileno(wrapped), _fileno(targetStream));
                    setvbuf(targetStream, nullptr, _IONBF, 0);
                    return;
                }
            }
        }
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* dummy = nullptr;
            freopen_s(&dummy, conFile, "w", targetStream);
            setvbuf(targetStream, nullptr, _IONBF, 0);
        }
    };

    bindOne(STD_OUTPUT_HANDLE, stdout, "CONOUT$");
    bindOne(STD_ERROR_HANDLE, stderr, "CONOUT$");
    std::cout.clear();
    std::cerr.clear();
    std::wcout.clear();
    std::wcerr.clear();
    std::ios::sync_with_stdio(true);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }

    bool runSelftest = false;
    std::wstring selftestDir;
    bool runUiSmoke = false;
    std::wstring uiSmokeDir;
    std::wstring customVaultDir;

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--selftest") {
            runSelftest = true;
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) {
                selftestDir = args[++i];
            }
        } else if (args[i] == L"--ui-smoke-test") {
            runUiSmoke = true;
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0) {
                uiSmokeDir = args[++i];
            }
        } else if (args[i] == L"--vault-dir" && i + 1 < args.size()) {
            customVaultDir = args[++i];
        }
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    if (runSelftest) {
        bindWin32StdHandlesForCli();
        int rc = maxwatch::runAutomatedSelfTest(selftestDir);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return rc;
    }

    if (runUiSmoke) {
        bindWin32StdHandlesForCli();
        std::cout << "=== Max Watch Windows Direct2D Studio UI Smoke Test ===" << std::endl;
        std::filesystem::path outPath = uiSmokeDir.empty()
            ? (std::filesystem::temp_directory_path() / L"MaxWatch_SmokeOut")
            : std::filesystem::path(uiSmokeDir);
        std::filesystem::path tempVault = outPath / L"temp_smoke_vault";
        std::error_code ec;
        std::filesystem::remove_all(tempVault, ec);
        std::filesystem::create_directories(outPath, ec);

        {
            maxwatch::AppWindow app(tempVault.wstring(), true, outPath.wstring());
            if (!app.createAndShow(hInstance, SW_SHOW)) {
                std::cerr << "[UI SMOKE FAILED] Could not create AppWindow" << std::endl;
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return 3;
            }
            app.runMessageLoop();
        }

        std::filesystem::remove_all(tempVault, ec);
        auto pLive = outPath / L"windows_live_studio.png";
        auto pPlay = outPath / L"windows_vault_player.png";
        if (!std::filesystem::exists(pLive) || std::filesystem::file_size(pLive) < 15000 ||
            !std::filesystem::exists(pPlay) || std::filesystem::file_size(pPlay) < 15000) {
            std::cerr << "[UI SMOKE FAILED] Rendered PNG screenshots missing or empty" << std::endl;
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return 4;
        }
        std::cout << "Saved Live Studio screenshot: " << pLive.string()
                  << " (" << std::filesystem::file_size(pLive) << " bytes)" << std::endl;
        std::cout << "Saved Vault Player screenshot: " << pPlay.string()
                  << " (" << std::filesystem::file_size(pPlay) << " bytes)" << std::endl;
        std::cout << "=== WINDOWS UI SMOKE TEST PASSED 100% ===" << std::endl;
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 0;
    }

    // Single-instance check for normal interactive desktop launch
    HANDLE hSingleMutex = CreateMutexW(nullptr, FALSE, L"Local\\MaxWatch_SingleInstance_Mutex_v1");
    if (hSingleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"MaxWatchStudioWindowClass", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(hSingleMutex);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 0;
    }

    int exitCode = 0;
    {
        maxwatch::AppWindow app(customVaultDir, false, L"");
        if (!app.createAndShow(hInstance, nCmdShow)) {
            MessageBoxW(nullptr, L"Max Watch failed to initialize Direct2D graphics.", L"Max Watch", MB_OK | MB_ICONERROR);
            exitCode = 1;
        } else {
            exitCode = app.runMessageLoop();
        }
    }

    if (hSingleMutex) CloseHandle(hSingleMutex);
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return exitCode;
}
