#include "core.h"

#include <commctrl.h>

#include "settings_dialog.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    snip::Paths paths = snip::Paths::Init();
    snip::LogInit(paths);

    bool capture = false;
    bool snipNow = false;
    bool quit = false;
    bool selfTest = false;
    bool settingsRequested = false;
    int dpiOverride = 0;
    std::wstring selfTestDir;
    bool fullScreen = false;
    bool version = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--capture") == 0) capture = true;
        else if (_wcsicmp(argv[i], L"--snip") == 0) snipNow = true;
        else if (_wcsicmp(argv[i], L"--full") == 0) fullScreen = true;
        else if (_wcsicmp(argv[i], L"--quit") == 0) quit = true;
        else if (_wcsicmp(argv[i], L"--settings") == 0 || _wcsicmp(argv[i], L"-s") == 0) settingsRequested = true;
        else if (_wcsicmp(argv[i], L"--dpi") == 0 && i + 1 < argc) dpiOverride = _wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--selftest") == 0) {
            selfTest = true;
            if (i + 1 < argc) selfTestDir = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--version") == 0) version = true;
    }
    if (argv != nullptr) LocalFree(argv);

    if (version) {
        MessageBoxW(nullptr, L"SniPin 0.2.15\nOne exe, two roles.\n\nSniPin.exe        resident (tray)\n--capture          capture worker\n--snip             ask the resident to snip\n--settings         open settings\n--quit             stop the resident", L"SniPin", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (dpiOverride > 0) snip::SetDpiOverride(dpiOverride);

    if (selfTest) {
        if (selfTestDir.empty()) selfTestDir = paths.dataDir + L"\\selftest";
        return snip::RunSelfTest(paths, selfTestDir);
    }

    if (capture) return snip::RunWorker(paths, fullScreen);

    if (settingsRequested) {
        HWND resident = FindWindowW(L"SniPin.Resident", nullptr);
        if (resident != nullptr) {
            PostMessageW(resident, WM_APP + 3, 0, 0);
            return 0;
        }
        snip::Settings settings = snip::LoadSettings(paths);
        if (snip::ShowSettingsDialog(nullptr, settings)) {
            snip::SaveSettings(paths, settings);
            snip::ApplyAutoStart(paths, settings.startWithWindows);
        }
        return 0;
    }

    if (quit) {
        HWND resident = FindWindowW(L"SniPin.Resident", nullptr);
        if (resident != nullptr) PostMessageW(resident, WM_CLOSE, 0, 0);
        return 0;
    }

    // A second launch (double-click, Start menu, shortcut) asks the resident to snip.
    if (snipNow) {
        HWND resident = FindWindowW(L"SniPin.Resident", nullptr);
        if (resident != nullptr) {
            PostMessageW(resident, WM_APP + 2, fullScreen ? 1 : 0, 0);
            return 0;
        }
        return snip::RunWorker(paths, fullScreen);
    }

    snip::Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\SniPin.Serve"));
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        snip::LogLine("resident: another instance is already running");
        return 0;
    }

    snip::Settings settings = snip::LoadSettings(paths);
    return snip::RunResident(paths, settings);
}
