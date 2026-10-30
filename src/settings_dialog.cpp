#include "settings_dialog.h"

#include "resource.h"

#include <commctrl.h>
#include <shlobj.h>
#include <string>

namespace snip {
namespace {

struct DialogState {
    Settings* settings = nullptr;
    Hotkey hotkey;
    WNDPROC hotkeyProc = nullptr;
    bool silent = false;      // automated test: no message boxes
    bool okApplied = false;   // set when the OK path accepted the values
};

const wchar_t* kModes[] = {L"Smart", L"Strict", L"ImageOnly"};
const wchar_t* kFormats[] = {L"webp", L"png", L"jpg", L"bmp"};

int IndexOf(const wchar_t* const* values, int count, const std::wstring& value, int fallback) {
    for (int i = 0; i < count; ++i) {
        if (_wcsicmp(values[i], value.c_str()) == 0) return i;
    }
    return fallback;
}

void SetComboItems(HWND dialog, int id, const wchar_t* const* values, int count, int selected) {
    HWND combo = GetDlgItem(dialog, id);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(values[i]));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void FillControls(HWND dialog, DialogState& state) {
    Settings& settings = *state.settings;

    state.hotkey.modifiers = 0;
    state.hotkey.key = 0;
    Hotkey parsed;
    if (ParseHotkey(settings.hotkey, parsed)) state.hotkey = parsed;
    SetDlgItemTextW(dialog, IDC_HOTKEY, state.hotkey.IsValid() ? state.hotkey.Format().c_str() : L"PrintScreen");

    SetComboItems(dialog, IDC_MODE, kModes, 3, IndexOf(kModes, 3, settings.clipboardMode, 0));
    SetComboItems(dialog, IDC_FORMAT, kFormats, 4, IndexOf(kFormats, 4, settings.saveFormat, 0));

    SetDlgItemInt(dialog, IDC_QUALITY, settings.webpQuality, FALSE);
    SetDlgItemInt(dialog, IDC_OFFSET, settings.pinOffset, FALSE);
    SetDlgItemInt(dialog, IDC_PEN, settings.penWidth, FALSE);
    SetDlgItemInt(dialog, IDC_MARKER, settings.markerSize, FALSE);
    SetDlgItemInt(dialog, IDC_TEXTSIZE, settings.textSize, FALSE);

    SetDlgItemTextW(dialog, IDC_FOLDER, settings.saveDirectory.c_str());

    CheckDlgButton(dialog, IDC_AUTOCOPY, settings.autoCopy ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_ONTOP, settings.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_AUTOSTART, settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED);
}

bool ReadRange(HWND dialog, DialogState& state, int id, int low, int high, const wchar_t* name, int& out) {
    BOOL ok = FALSE;
    int value = static_cast<int>(GetDlgItemInt(dialog, id, &ok, FALSE));
    if (!ok || value < low || value > high) {
        if (!state.silent) {
            std::wstring message = std::wstring(name) + L" must be between " + std::to_wstring(low) +
                                   L" and " + std::to_wstring(high) + L".";
            MessageBoxW(dialog, message.c_str(), L"SniPin", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, id));
        }
        return false;
    }
    out = value;
    return true;
}

int ComboIndex(HWND dialog, int id) {
    int index = static_cast<int>(SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0));
    return index < 0 ? 0 : index;
}

LRESULT CALLBACK HotkeyEditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    HWND dialog = GetParent(window);
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
        case WM_CHAR:
            return 0;
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            // Let the dialog keep its navigation keys and its default button.
            if (wparam == VK_TAB || wparam == VK_RETURN || wparam == VK_ESCAPE) break;
            if (wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == VK_MENU || wparam == VK_LWIN ||
                wparam == VK_RWIN) {
                return 0;
            }
            UINT modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;

            Hotkey candidate{modifiers, static_cast<UINT>(wparam)};
            if (!candidate.IsValid()) return 0;

            state->hotkey = candidate;
            SetWindowTextW(window, candidate.Format().c_str());
            return 0;
        }
        default:
            break;
    }
    return CallWindowProcW(state->hotkeyProc, window, message, wparam, lparam);
}

INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<DialogState*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            FillControls(dialog, *state);

            HWND hotkey = GetDlgItem(dialog, IDC_HOTKEY);
            state->hotkeyProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hotkey, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HotkeyEditProc)));

            SendDlgItemMessageW(dialog, IDC_QUALITY_SPIN, UDM_SETRANGE32, 1, 100);
            SendDlgItemMessageW(dialog, IDC_OFFSET_SPIN, UDM_SETRANGE32, 0, 200);
            SendDlgItemMessageW(dialog, IDC_PEN_SPIN, UDM_SETRANGE32, 1, 24);
            SendDlgItemMessageW(dialog, IDC_MARKER_SPIN, UDM_SETRANGE32, 4, 48);
            SendDlgItemMessageW(dialog, IDC_TEXTSIZE_SPIN, UDM_SETRANGE32, 8, 72);

            // Center on the work area (the owner is a hidden 0x0 window).
            RECT box{};
            GetWindowRect(dialog, &box);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
                int width = box.right - box.left;
                int height = box.bottom - box.top;
                int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
                int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
                SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDOK: {
                    if (state == nullptr) break;

                    Settings updated = *state->settings;
                    if (!ReadRange(dialog, *state, IDC_QUALITY, 1, 100, L"Quality", updated.webpQuality)) return TRUE;
                    if (!ReadRange(dialog, *state, IDC_OFFSET, 0, 200, L"Offset", updated.pinOffset)) return TRUE;
                    if (!ReadRange(dialog, *state, IDC_PEN, 1, 24, L"Pen width", updated.penWidth)) return TRUE;
                    if (!ReadRange(dialog, *state, IDC_MARKER, 4, 48, L"Marker size", updated.markerSize)) return TRUE;
                    if (!ReadRange(dialog, *state, IDC_TEXTSIZE, 8, 72, L"Text size", updated.textSize)) return TRUE;

                    wchar_t folder[MAX_PATH] = {};
                    GetDlgItemTextW(dialog, IDC_FOLDER, folder, MAX_PATH);
                    if (folder[0] == L'\0') {
                        MessageBoxW(dialog, L"Choose a folder for saved snips.", L"SniPin",
                                    MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dialog, IDC_FOLDER));
                        return TRUE;
                    }

                    Hotkey check = state->hotkey;
                    if (!check.IsValid()) {
                        MessageBoxW(dialog, L"Press a valid shortcut in the Hotkey field.", L"SniPin",
                                    MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dialog, IDC_HOTKEY));
                        return TRUE;
                    }

                    updated.hotkey = check.Format();
                    updated.clipboardMode = kModes[ComboIndex(dialog, IDC_MODE)];
                    updated.saveFormat = kFormats[ComboIndex(dialog, IDC_FORMAT)];
                    updated.saveDirectory = folder;
                    updated.autoCopy = IsDlgButtonChecked(dialog, IDC_AUTOCOPY) == BST_CHECKED;
                    updated.alwaysOnTop = IsDlgButtonChecked(dialog, IDC_ONTOP) == BST_CHECKED;
                    updated.startWithWindows = IsDlgButtonChecked(dialog, IDC_AUTOSTART) == BST_CHECKED;

                    ValidateSettings(updated);
                    *state->settings = updated;
                    state->okApplied = true;
                    EndDialog(dialog, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                case IDC_RESET: {
                    if (state == nullptr) break;
                    Settings defaults;
                    ValidateSettings(defaults);
                    *state->settings = defaults;
                    FillControls(dialog, *state);
                    return TRUE;
                }
                case IDC_OPENFOLDER: {
                    wchar_t folder[MAX_PATH] = {};
                    GetDlgItemTextW(dialog, IDC_FOLDER, folder, MAX_PATH);
                    if (folder[0] != L'\0') ShellExecuteW(nullptr, L"open", folder, nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                }
                case IDC_BROWSE: {
                    BROWSEINFOW info{};
                    info.hwndOwner = dialog;
                    info.lpszTitle = L"Choose a folder for saved snips";
                    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST item = SHBrowseForFolderW(&info);
                    if (item != nullptr) {
                        wchar_t path[MAX_PATH] = {};
                        if (SHGetPathFromIDListW(item, path)) SetDlgItemTextW(dialog, IDC_FOLDER, path);
                        CoTaskMemFree(item);
                    }
                    return TRUE;
                }
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

} // namespace

bool TestSettingsDialogValidation(Settings& settings) {
    DialogState state;
    state.settings = &settings;
    state.silent = true;

    HWND dialog = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
                                     SettingsProc, reinterpret_cast<LPARAM>(&state));
    if (dialog == nullptr) {
        LogLine("dialog test: CreateDialogParam failed");
        return false;
    }

    SetDlgItemTextW(dialog, IDC_QUALITY, L"999");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    bool blocked = IsWindow(dialog) != FALSE;

    SetDlgItemTextW(dialog, IDC_QUALITY, L"100");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    bool applied = state.okApplied && settings.webpQuality == 100;

    if (IsWindow(dialog)) DestroyWindow(dialog);

    LogFmt("dialog test: blocked=%d applied=%d", blocked ? 1 : 0, applied ? 1 : 0);
    return blocked && applied;
}

bool ShowSettingsDialog(HWND owner, Settings& settings) {
    DialogState state;
    state.settings = &settings;

    INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                                     SettingsProc, reinterpret_cast<LPARAM>(&state));
    return result == IDOK;
}

} // namespace snip
