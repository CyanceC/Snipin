#include "core.h"

#include "settings_dialog.h"

#include <cstdio>

namespace snip {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kMenuSnip = 101;
constexpr UINT kMenuSnipFull = 102;
constexpr UINT kMenuSettings = 103;
constexpr UINT kMenuOpenFolder = 104;
constexpr UINT kMenuExit = 105;
constexpr int kHotkeyId = 1;
constexpr int kStatsTimer = 1;
constexpr int kJanitorTimer = 2;
constexpr UINT kJanitorPeriodMs = 5 * 60 * 1000;
constexpr UINT kStatsPeriodMs = 60 * 1000;
constexpr unsigned long long kPayloadRetention100ns = 2ull * 24 * 60 * 60 * 10000000ull;

std::wstring ClipboardFilePath() {
    std::wstring result;
    if (!OpenClipboard(nullptr)) return result;

    HANDLE data = GetClipboardData(CF_HDROP);
    if (data != nullptr) {
        wchar_t buffer[MAX_PATH] = {};
        if (DragQueryFileW(reinterpret_cast<HDROP>(data), 0, buffer, MAX_PATH) > 0) result = buffer;
    }
    CloseClipboard();
    return result;
}

class Resident {
public:
    Resident(Paths& paths, Settings& settings) : paths_(paths), settings_(settings) {}
    ~Resident() { UnregisterHotkey(window_, kHotkeyId); }

    int Run();

private:
    static LRESULT CALLBACK WndProcStatic(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT Dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    bool CreateHiddenWindow();
    void BuildMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayTip();
    void ShowContextMenu();
    void SpawnWorker(bool fullScreen);
    void OpenSettings();
    void RegisterGlobalHotkey();
    void RunJanitor();

    Paths& paths_;
    Settings& settings_;
    HWND window_ = nullptr;
    MenuHandle menu_;
    IconHandle icon_;
    NOTIFYICONDATAW iconData_{};
    UINT taskbarCreatedMessage_ = 0;
    Handle job_;
    Handle worker_;
    Hotkey hotkey_;
    std::wstring hotkeyLabel_ = L"no hotkey";
};

LRESULT CALLBACK Resident::WndProcStatic(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self = reinterpret_cast<Resident*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr) return self->Dispatch(window, message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

bool Resident::CreateHiddenWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProcStatic;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"SniPin.Resident";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogFmt("resident: RegisterClassExW failed %lu", GetLastError());
        return false;
    }

    window_ = CreateWindowExW(0, L"SniPin.Resident", L"SniPin", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, windowClass.hInstance, this);
    if (window_ == nullptr) {
        LogFmt("resident: CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    return true;
}

void Resident::BuildMenu() {
    menu_.reset(CreatePopupMenu());
    if (!menu_) return;

    std::wstring snipLabel = L"Snip region   (" + hotkeyLabel_ + L")";
    AppendMenuW(menu_.get(), MF_STRING, kMenuSnip, snipLabel.c_str());
    AppendMenuW(menu_.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu_.get(), MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu_.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu_.get(), MF_STRING, kMenuExit, L"Exit");
}

void Resident::AddTrayIcon() {
    iconData_.cbSize = sizeof(iconData_);
    iconData_.hWnd = window_;
    iconData_.uID = 1;
    iconData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    iconData_.uCallbackMessage = kTrayMessage;
    iconData_.hIcon = icon_.get();
    wcscpy_s(iconData_.szTip, L"SniPin");

    if (!Shell_NotifyIconW(NIM_ADD, &iconData_)) {
        LogFmt("resident: NIM_ADD failed %lu", GetLastError());
        return;
    }
    LogLine("tray icon added");
    UpdateTrayTip();
}

void Resident::UpdateTrayTip() {
    std::wstring tip = L"SniPin \x2014 " + hotkeyLabel_;
    wcsncpy_s(iconData_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &iconData_);
}

void Resident::RemoveTrayIcon() {
    if (Shell_NotifyIconW(NIM_DELETE, &iconData_)) LogLine("tray icon removed");
    else LogFmt("resident: NIM_DELETE failed %lu", GetLastError());
}

void Resident::ShowContextMenu() {
    if (!menu_) return;

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    UINT command = TrackPopupMenu(menu_.get(), TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  cursor.x, cursor.y, 0, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);

    switch (command) {
        case kMenuSnip: SpawnWorker(false); break;
        case kMenuSettings:
            OpenSettings();
            break;
        case kMenuOpenFolder:
            CreateDirectoryW(settings_.saveDirectory.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", settings_.saveDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kMenuExit: DestroyWindow(window_); break;
        default: break;
    }
}

void Resident::OpenSettings() {
    Settings updated = settings_;
    if (!ShowSettingsDialog(window_, updated)) return;

    settings_ = updated;
    SaveSettings(paths_, settings_);
    ApplyAutoStart(paths_, settings_.startWithWindows);
    RegisterGlobalHotkey();
    BuildMenu();
    UpdateTrayTip();
    LogLine("settings: updated from dialog");
}

void Resident::SpawnWorker(bool fullScreen) {
    std::wstring commandLine = L"\"" + paths_.exePath + L"\" --capture";
    if (fullScreen) commandLine += L" --full";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    BOOL created = CreateProcessW(paths_.exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                                  CREATE_SUSPENDED, nullptr, paths_.exeDir.c_str(), &startup, &process);
    if (!created) {
        LogFmt("spawn failed: %lu", GetLastError());
        return;
    }

    // Grant foreground rights before the child runs so its overlay can raise itself.
    AllowSetForegroundWindow(process.dwProcessId);
    if (job_) AssignProcessToJobObject(job_.get(), process.hProcess);

    ResumeThread(process.hThread);
    QueryPerformanceCounter(&finished);

    CloseHandle(process.hThread);
    worker_.reset(process.hProcess);

    double elapsedMs = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart;
    LogFmt("spawn ok pid=%lu in %.2f ms (full=%d)", process.dwProcessId, elapsedMs, fullScreen ? 1 : 0);
}

void Resident::RegisterGlobalHotkey() {
    Hotkey desired;
    if (!ParseHotkey(settings_.hotkey, desired)) desired = DefaultHotkey();

    if (TryRegisterHotkey(window_, kHotkeyId, desired)) {
        hotkey_ = desired;
    } else if (TryRegisterHotkey(window_, kHotkeyId, DefaultHotkey())) {
        hotkey_ = DefaultHotkey();
    } else {
        Hotkey alternative{MOD_CONTROL | MOD_ALT, 'S'};
        if (TryRegisterHotkey(window_, kHotkeyId, alternative)) {
            hotkey_ = alternative;
        } else {
            hotkey_.key = 0;
        }
    }

    hotkeyLabel_ = hotkey_.IsValid() ? hotkey_.Format() : L"no hotkey";
    if (!hotkey_.IsValid() || hotkey_ != desired) {
        // Windows refused the configured shortcut; persist the one we actually got.
        settings_.hotkey = hotkey_.IsValid() ? hotkeyLabel_ : L"PrintScreen";
        SaveSettings(paths_, settings_);
    }
    LogFmt("hotkey: %s", WideToUtf8(hotkeyLabel_).c_str());
}

void Resident::RunJanitor() {
    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER now{};
    now.LowPart = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;

    std::wstring clipboardFile = ClipboardFilePath();
    std::wstring pattern = paths_.tempDir + L"\\*.webp";

    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;

    int removed = 0;
    do {
        ULARGE_INTEGER written{};
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;

        bool stale = (now.QuadPart > written.QuadPart) && (now.QuadPart - written.QuadPart > kPayloadRetention100ns);
        if (!stale) continue;

        std::wstring path = paths_.tempDir + L"\\" + entry.cFileName;
        if (!clipboardFile.empty() && _wcsicmp(path.c_str(), clipboardFile.c_str()) == 0) continue;
        if (DeleteFileW(path.c_str())) ++removed;
    } while (FindNextFileW(search, &entry));

    FindClose(search);
    if (removed > 0) LogFmt("janitor: removed %d stale payload(s)", removed);
}

LRESULT Resident::Dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_HOTKEY:
            if (static_cast<int>(wparam) == kHotkeyId) SpawnWorker(false);
            return 0;

        case WM_APP + 2: // second launch asked for a snip
            SpawnWorker(wparam != 0);
            return 0;

        case WM_APP + 3: // second launch asked for settings
            OpenSettings();
            return 0;

        case kTrayMessage:
            if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) ShowContextMenu();
            else if (lparam == WM_LBUTTONDBLCLK) SpawnWorker(false);
            return 0;

        case WM_TIMER:
            if (wparam == kStatsTimer) LogStats("resident");
            else if (wparam == kJanitorTimer) RunJanitor();
            return 0;

        case WM_DESTROY:
            LogStats("resident-stop");
            KillTimer(window, kStatsTimer);
            KillTimer(window, kJanitorTimer);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int Resident::Run() {
    if (!CreateHiddenWindow()) return 1;

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    job_.reset(CreateJobObjectW(nullptr, nullptr));
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    icon_.reset(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)));
    if (!icon_) icon_.reset(LoadIconW(nullptr, IDI_APPLICATION));

    // Normalise whatever was on disk (clamped values, repaired paths) so the file always
    // matches what the app actually uses.
    SaveSettings(paths_, settings_);

    RegisterGlobalHotkey();
    BuildMenu();
    AddTrayIcon();
    ApplyAutoStart(paths_, settings_.startWithWindows);

    LogStats("resident-start");
    LogFmt("resident ready: portable=%d data=%s", paths_.portable ? 1 : 0,
           WideToUtf8(paths_.dataDir).c_str());

    SetTimer(window_, kStatsTimer, kStatsPeriodMs, nullptr);
    SetTimer(window_, kJanitorTimer, kJanitorPeriodMs, nullptr);
    RunJanitor();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return 0;
}

} // namespace

int RunResident(Paths& paths, Settings& settings) {
    Resident resident(paths, settings);
    return resident.Run();
}

} // namespace snip
