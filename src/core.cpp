#include "core.h"

#include <shlobj.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>

namespace snip {

namespace {

std::wstring g_logPath;
std::wstring g_dataDir;
int g_dpiOverride = 0;

constexpr unsigned long long kLogRotateBytes = 1000ull * 1000ull;

std::wstring Trim(const std::wstring& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && iswspace(text[start])) ++start;
    while (end > start && iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

bool DirectoryWritable(const std::wstring& dir) {
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::wstring probe = dir + L"\\.write-test";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

std::wstring KnownFolder(int csidl) {
    wchar_t buffer[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, 0, buffer))) return L"";
    return buffer;
}

void WriteAll(HANDLE file, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr) || written == 0) return;
        bytes += written;
        size -= written;
    }
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!out.empty() && (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) || read != out.size())) {
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
}

void WriteWholeFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteAll(file, text.data(), text.size());
    CloseHandle(file);
}

// ------------------------------------------------------------------ json

std::string TrimAscii(std::string text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) ++start;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    return text.substr(start, end - start);
}

std::string UpperAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return text;
}

// Returns the raw token after "key": (string contents unescaped, number, true/false).
bool LookupValue(const std::string& json, const char* key, std::string& out) {
    if (json.empty()) return false;

    std::string needle = std::string("\"") + key + "\"";
    size_t at = UpperAscii(json).find(UpperAscii(needle));
    if (at == std::string::npos) return false;

    size_t colon = json.find(':', at + needle.size());
    if (colon == std::string::npos) return false;

    size_t i = colon + 1;
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size()) return false;

    if (json[i] == '"') {
        ++i;
        std::string text;
        while (i < json.size()) {
            char c = json[i++];
            if (c == '"') break;
            if (c == '\\' && i < json.size()) {
                char escape = json[i++];
                switch (escape) {
                    case 'n': text.push_back('\n'); break;
                    case 'r': text.push_back('\r'); break;
                    case 't': text.push_back('\t'); break;
                    case 'u':
                        if (i + 4 <= json.size()) {
                            std::string hex = json.substr(i, 4);
                            wchar_t code = static_cast<wchar_t>(strtoul(hex.c_str(), nullptr, 16));
                            text += WideToUtf8(std::wstring(1, code));
                            i += 4;
                        }
                        break;
                    default: text.push_back(escape); break;
                }
                continue;
            }
            text.push_back(c);
        }
        out = text;
        return true;
    }

    size_t end = i;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && !isspace(static_cast<unsigned char>(json[end]))) ++end;
    out = json.substr(i, end - i);
    return true;
}

std::string JsonQuote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out += "\"";
    return out;
}

std::string GetStringOr(const std::string& json, const char* key, const std::string& fallback) {
    std::string value;
    return LookupValue(json, key, value) ? value : fallback;
}

int GetIntOr(const std::string& json, const char* key, int fallback) {
    std::string value;
    if (!LookupValue(json, key, value)) return fallback;
    char* end = nullptr;
    long parsed = strtol(value.c_str(), &end, 10);
    return (end != nullptr && end != value.c_str()) ? static_cast<int>(parsed) : fallback;
}

bool GetBoolOr(const std::string& json, const char* key, bool fallback) {
    std::string value;
    if (!LookupValue(json, key, value)) return fallback;
    if (_stricmp(value.c_str(), "true") == 0) return true;
    if (_stricmp(value.c_str(), "false") == 0) return false;
    return fallback;
}

// ------------------------------------------------------------------ hotkey names

struct KeyName {
    const wchar_t* name;
    UINT key;
};

const KeyName kKeyNames[] = {
    {L"printscreen", 0x2C}, {L"prtsc", 0x2C}, {L"snapshot", 0x2C},
    {L"insert", 0x2D}, {L"delete", 0x2E}, {L"home", 0x24}, {L"end", 0x23},
    {L"pageup", 0x21}, {L"pagedown", 0x22}, {L"pause", 0x13},
    {L"scrolllock", 0x91}, {L"space", 0x20}, {L"tab", 0x09}, {L"backspace", 0x08},
};

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

} // namespace

// ---------------------------------------------------------------- text

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- paths

Paths Paths::Init() {
    Paths paths;
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    paths.exePath = module;

    size_t slash = paths.exePath.find_last_of(L"\\/");
    paths.exeDir = (slash == std::wstring::npos) ? L"." : paths.exePath.substr(0, slash);

    std::wstring portable = paths.exeDir + L"\\SniPin-data";
    if (DirectoryWritable(portable)) {
        paths.dataDir = portable;
        paths.portable = true;
    } else {
        paths.dataDir = KnownFolder(CSIDL_LOCAL_APPDATA) + L"\\SniPin";
        CreateDirectoryW(paths.dataDir.c_str(), nullptr);
        paths.portable = false;
    }

    paths.tempDir = paths.dataDir + L"\\temp";
    CreateDirectoryW(paths.tempDir.c_str(), nullptr);
    paths.settingsPath = paths.dataDir + L"\\settings.json";
    paths.logPath = paths.dataDir + L"\\snipin.log";
    return paths;
}

// ---------------------------------------------------------------- logging

void LogInit(const Paths& paths) {
    g_logPath = paths.logPath;
    g_dataDir = paths.dataDir;
}

void LogLine(const std::string& message) {
    if (g_logPath.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &info)) {
        unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > kLogRotateBytes) {
            std::wstring previous = g_logPath + L".1";
            DeleteFileW(previous.c_str());
            MoveFileW(g_logPath.c_str(), previous.c_str());
        }
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    std::string line = std::string(stamp) + message + "\r\n";
    WriteAll(file, line.data(), line.size());
    CloseHandle(file);
}

void LogLine(const std::wstring& message) {
    LogLine(WideToUtf8(message));
}

void LogFmt(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogLine(std::string(buffer));
}

// ---------------------------------------------------------------- accounting

ResourceStats SampleStats() {
    ResourceStats stats;
    stats.gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    stats.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    GetProcessHandleCount(GetCurrentProcess(), &stats.handles);

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        stats.privateBytes = counters.PrivateUsage;
    }
    return stats;
}

void SetDpiOverride(int dpi) { g_dpiOverride = dpi; }
int DpiOverride() { return g_dpiOverride; }

double ElapsedMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to, const LARGE_INTEGER& frequency) {
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

void LogStats(const char* tag) {
    ResourceStats stats = SampleStats();
    LogFmt("stats %s: gdi=%lu user=%lu handles=%lu private=%.1f MB", tag, stats.gdiObjects,
           stats.userObjects, stats.handles, static_cast<double>(stats.privateBytes) / (1024.0 * 1024.0));
}

// ---------------------------------------------------------------- hotkey

Hotkey DefaultHotkey() {
    Hotkey hotkey;
    hotkey.modifiers = 0;
    hotkey.key = 0x2C; // PrintScreen
    return hotkey;
}

bool Hotkey::IsValid() const {
    if (key == 0) return false;
    if (modifiers != 0) return true;
    return key == 0x2C || key == 0x13 || key == 0x91 || (key >= 0x70 && key <= 0x87) || key == 0x2D;
}

std::wstring Hotkey::Format() const {
    if (key == 0) return L"None";

    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    if (modifiers & MOD_WIN) text += L"Win+";

    if (key >= 'A' && key <= 'Z') {
        text += static_cast<wchar_t>(key);
    } else if (key >= '0' && key <= '9') {
        text += static_cast<wchar_t>(key);
    } else if (key >= 0x70 && key <= 0x87) {
        text += L"F" + std::to_wstring(key - 0x70 + 1);
    } else {
        switch (key) {
            case 0x2C: text += L"PrintScreen"; break;
            case 0x2D: text += L"Insert"; break;
            case 0x2E: text += L"Delete"; break;
            case 0x24: text += L"Home"; break;
            case 0x23: text += L"End"; break;
            case 0x21: text += L"PageUp"; break;
            case 0x22: text += L"PageDown"; break;
            case 0x13: text += L"Pause"; break;
            case 0x91: text += L"ScrollLock"; break;
            case VK_SPACE: text += L"Space"; break;
            case VK_TAB: text += L"Tab"; break;
            case VK_BACK: text += L"Backspace"; break;
            default: text += L"Key" + std::to_wstring(key); break;
        }
    }
    return text;
}

bool ParseHotkey(const std::wstring& text, Hotkey& out) {
    std::wstring trimmed = Trim(text);
    if (trimmed.empty()) return false;

    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t c : trimmed) {
        if (c == L'+') { parts.push_back(Trim(current)); current.clear(); }
        else current.push_back(c);
    }
    parts.push_back(Trim(current));

    UINT modifiers = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::wstring part = Lower(parts[i]);
        bool last = (i + 1 == parts.size());

        if (!last) {
            if (part == L"ctrl" || part == L"control") modifiers |= MOD_CONTROL;
            else if (part == L"alt") modifiers |= MOD_ALT;
            else if (part == L"shift") modifiers |= MOD_SHIFT;
            else if (part == L"win" || part == L"windows") modifiers |= MOD_WIN;
            else return false;
            continue;
        }

        UINT key = 0;
        if (part.size() == 1) {
            wchar_t c = static_cast<wchar_t>(towupper(part[0]));
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) key = static_cast<UINT>(c);
        }
        if (key == 0 && part.size() >= 2 && (part[0] == L'f')) {
            int number = _wtoi(part.c_str() + 1);
            if (number >= 1 && number <= 24) key = static_cast<UINT>(0x70 + number - 1);
        }
        if (key == 0) {
            for (const KeyName& entry : kKeyNames) {
                if (part == entry.name) { key = entry.key; break; }
            }
        }
        if (key == 0) return false;

        Hotkey candidate;
        candidate.modifiers = modifiers;
        candidate.key = key;
        if (!candidate.IsValid()) return false;
        out = candidate;
        return true;
    }
    return false;
}

bool TryRegisterHotkey(HWND window, int id, const Hotkey& hotkey) {
    UnregisterHotkey(window, id);
    if (!hotkey.IsValid()) return false;
    return RegisterHotKey(window, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
}

void UnregisterHotkey(HWND window, int id) {
    UnregisterHotKey(window, id);
}

// ---------------------------------------------------------------- settings

void ValidateSettings(Settings& settings) {
    if (settings.webpQuality < 1) settings.webpQuality = 1;
    if (settings.webpQuality > 100) settings.webpQuality = 100;
    if (settings.pinOffset < 0) settings.pinOffset = 0;
    if (settings.pinOffset > 200) settings.pinOffset = 200;
    if (settings.penWidth < 1) settings.penWidth = 1;
    if (settings.penWidth > 24) settings.penWidth = 24;
    if (settings.markerSize < 4) settings.markerSize = 4;
    if (settings.markerSize > 48) settings.markerSize = 48;
    if (settings.textSize < 8) settings.textSize = 8;
    if (settings.textSize > 72) settings.textSize = 72;

    Hotkey parsed;
    if (!ParseHotkey(settings.hotkey, parsed)) settings.hotkey = L"PrintScreen";

    std::wstring format = Lower(settings.saveFormat);
    if (format == L".webp") format = L"webp";
    if (format != L"webp" && format != L"png" && format != L"jpg" && format != L"bmp") format = L"webp";
    settings.saveFormat = format;

    if (settings.saveDirectory.empty()) {
        std::wstring pictures = KnownFolder(CSIDL_MYPICTURES);
        settings.saveDirectory = pictures.empty() ? (KnownFolder(CSIDL_PROFILE) + L"\\Pictures\\SniPin")
                                                  : (pictures + L"\\SniPin");
    }

    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(settings.saveDirectory.c_str(), MAX_PATH, full, nullptr) > 0) {
        settings.saveDirectory = full;
    }

    // A folder on a drive that no longer exists (removed USB stick, network share) is not
    // usable: fall back to Pictures\\SniPin instead of failing every save.
    if (settings.saveDirectory.size() >= 2 && settings.saveDirectory[1] == L':') {
        wchar_t root[4] = {settings.saveDirectory[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) {
            std::wstring pictures = KnownFolder(CSIDL_MYPICTURES);
            settings.saveDirectory = pictures.empty() ? (KnownFolder(CSIDL_PROFILE) + L"\\Pictures\\SniPin")
                                                      : (pictures + L"\\SniPin");
        }
    }
}

Settings LoadSettings(const Paths& paths) {
    Settings settings;
    ValidateSettings(settings);

    std::string json;
    if (!ReadWholeFile(paths.settingsPath, json)) return settings;

    settings.clipboardMode = Utf8ToWide(GetStringOr(json, "ClipboardMode", "Smart"));
    settings.webpQuality = GetIntOr(json, "WebPQuality", settings.webpQuality);
    settings.saveFormat = Utf8ToWide(GetStringOr(json, "SaveFormat", "webp"));
    settings.saveDirectory = Utf8ToWide(GetStringOr(json, "SaveDirectory", WideToUtf8(settings.saveDirectory)));
    settings.alwaysOnTop = GetBoolOr(json, "AlwaysOnTop", settings.alwaysOnTop);
    settings.autoCopy = GetBoolOr(json, "AutoCopy", settings.autoCopy);
    settings.startWithWindows = GetBoolOr(json, "StartWithWindows", settings.startWithWindows);
    settings.hotkey = Utf8ToWide(GetStringOr(json, "Hotkey", "PrintScreen"));
    settings.pinOffset = GetIntOr(json, "PinOffset", settings.pinOffset);
    settings.penWidth = GetIntOr(json, "PenWidth", settings.penWidth);
    settings.markerSize = GetIntOr(json, "MarkerSize", settings.markerSize);
    settings.textSize = GetIntOr(json, "TextSize", settings.textSize);

    ValidateSettings(settings);
    return settings;
}

void SaveSettings(const Paths& paths, const Settings& settings) {
    Settings valid = settings;
    ValidateSettings(valid);

    std::string json = "{\r\n";
    json += "  \"ClipboardMode\": " + JsonQuote(WideToUtf8(valid.clipboardMode)) + ",\r\n";
    json += "  \"WebPQuality\": " + std::to_string(valid.webpQuality) + ",\r\n";
    json += "  \"SaveFormat\": " + JsonQuote(WideToUtf8(valid.saveFormat)) + ",\r\n";
    json += "  \"SaveDirectory\": " + JsonQuote(WideToUtf8(valid.saveDirectory)) + ",\r\n";
    json += std::string("  \"AlwaysOnTop\": ") + (valid.alwaysOnTop ? "true" : "false") + ",\r\n";
    json += std::string("  \"AutoCopy\": ") + (valid.autoCopy ? "true" : "false") + ",\r\n";
    json += std::string("  \"StartWithWindows\": ") + (valid.startWithWindows ? "true" : "false") + ",\r\n";
    json += "  \"Hotkey\": " + JsonQuote(WideToUtf8(valid.hotkey)) + ",\r\n";
    json += "  \"PinOffset\": " + std::to_string(valid.pinOffset) + ",\r\n";
    json += "  \"PenWidth\": " + std::to_string(valid.penWidth) + ",\r\n";
    json += "  \"MarkerSize\": " + std::to_string(valid.markerSize) + ",\r\n";
    json += "  \"TextSize\": " + std::to_string(valid.textSize) + "\r\n";
    json += "}\r\n";

    WriteWholeFile(paths.settingsPath, json);
}

std::wstring UniqueFile(const std::wstring& directory, const std::wstring& prefix, const std::wstring& extension) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04d-%02d-%02d_%02d-%02d-%02d", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);

    std::wstring base = directory + L"\\" + prefix + L"_" + stamp;
    std::wstring path = base + extension;
    int counter = 2;
    while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && counter < 1000) {
        path = base + L"_" + std::to_wstring(counter++) + extension;
    }
    return path;
}

void ApplyAutoStart(const Paths& paths, bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        std::wstring command = L"\"" + paths.exePath + L"\"";
        RegSetValueExW(key, L"SniPin", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"SniPin");
    }
    RegCloseKey(key);
}

} // namespace snip
