#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdint>

namespace snip {

// ---------------------------------------------------------------- RAII leases

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = other.value_; other.value_ = nullptr; }
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

class GdiObject {
public:
    GdiObject() = default;
    explicit GdiObject(HGDIOBJ value) : value_(value) {}
    ~GdiObject() { reset(); }
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    HGDIOBJ get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void reset(HGDIOBJ value = nullptr) {
        if (value_ != nullptr) DeleteObject(value_);
        value_ = value;
    }
private:
    HGDIOBJ value_ = nullptr;
};

class MenuHandle {
public:
    MenuHandle() = default;
    explicit MenuHandle(HMENU value) : value_(value) {}
    ~MenuHandle() { reset(); }
    MenuHandle(const MenuHandle&) = delete;
    MenuHandle& operator=(const MenuHandle&) = delete;
    HMENU get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void reset(HMENU value = nullptr) {
        if (value_ != nullptr) DestroyMenu(value_);
        value_ = value;
    }
private:
    HMENU value_ = nullptr;
};

class IconHandle {
public:
    IconHandle() = default;
    explicit IconHandle(HICON value) : value_(value) {}
    ~IconHandle() { reset(); }
    IconHandle(const IconHandle&) = delete;
    IconHandle& operator=(const IconHandle&) = delete;
    HICON get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void reset(HICON value = nullptr) {
        if (value_ != nullptr) DestroyIcon(value_);
        value_ = value;
    }
private:
    HICON value_ = nullptr;
};

// ---------------------------------------------------------------- text

std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

// ---------------------------------------------------------------- paths

struct Paths {
    std::wstring exePath;
    std::wstring exeDir;
    std::wstring dataDir;
    std::wstring tempDir;
    std::wstring settingsPath;
    std::wstring logPath;
    bool portable = true;

    static Paths Init();
};

// ---------------------------------------------------------------- logging

void LogInit(const Paths& paths);
void LogLine(const std::string& message);
void LogLine(const std::wstring& message);
void LogFmt(const char* format, ...);

// ---------------------------------------------------------------- accounting

struct ResourceStats {
    DWORD gdiObjects = 0;
    DWORD userObjects = 0;
    DWORD handles = 0;
    unsigned long long privateBytes = 0;
};

ResourceStats SampleStats();
void LogStats(const char* tag);

double ElapsedMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to, const LARGE_INTEGER& frequency);

// ---------------------------------------------------------------- hotkey

struct Hotkey {
    UINT modifiers = 0;
    UINT key = 0;

    bool IsValid() const;
    std::wstring Format() const;
    bool operator==(const Hotkey& other) const { return modifiers == other.modifiers && key == other.key; }
};

Hotkey DefaultHotkey();
bool ParseHotkey(const std::wstring& text, Hotkey& out);
bool TryRegisterHotkey(HWND window, int id, const Hotkey& hotkey);
void UnregisterHotkey(HWND window, int id);

// ---------------------------------------------------------------- settings

struct Settings {
    std::wstring clipboardMode = L"Smart";
    int webpQuality = 100;
    std::wstring saveFormat = L"webp";
    std::wstring saveDirectory;
    bool alwaysOnTop = true;
    bool autoCopy = true;
    bool startWithWindows = false;
    std::wstring hotkey = L"PrintScreen";
    int pinOffset = 32;
    int penWidth = 3;
    int markerSize = 11;
    int textSize = 15;
};

Settings LoadSettings(const Paths& paths);
void SaveSettings(const Paths& paths, const Settings& settings);
void ValidateSettings(Settings& settings);
void ApplyAutoStart(const Paths& paths, bool enabled);

/// "prefix_2026-10-29_20-15-03.ext" inside directory, suffixed if it already exists.
std::wstring UniqueFile(const std::wstring& directory, const std::wstring& prefix, const std::wstring& extension);

/// Test/debug override for the UI scale (0 = use the monitor's real DPI).
void SetDpiOverride(int dpi);
int DpiOverride();

// ---------------------------------------------------------------- modes

int RunResident(Paths& paths, Settings& settings);
int RunWorker(const Paths& paths, bool fullScreen);
int RunSelfTest(const Paths& paths, const std::wstring& outDir);

} // namespace snip
