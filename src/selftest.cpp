#include "core.h"

#include "capture.h"
#include "clipboard.h"
#include "codec.h"
#include "settings_dialog.h"
#include "document.h"

#include <algorithm>
#include <cstring>

#include <string>
#include <vector>

namespace snip {
namespace {

struct Check {
    std::string name;
    bool ok;
    std::string detail;
};

struct Report {
    std::vector<std::pair<std::string, std::string>> props;
    std::vector<Check> checks;
    int failures = 0;

    void Prop(const std::string& key, const std::string& jsonValue) { props.emplace_back(key, jsonValue); }

    void CheckIt(const std::string& name, bool ok, const std::string& detail = "") {
        checks.push_back(Check{name, ok, detail});
        if (!ok) ++failures;
        LogFmt("selftest: %s %s %s", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    }

    std::string ToJson() const {
        std::string json = "{\r\n";
        for (const auto& prop : props) {
            json += "  \"" + prop.first + "\": " + prop.second + ",\r\n";
        }
        json += "  \"failures\": " + std::to_string(failures) + ",\r\n";
        json += "  \"checks\": [\r\n";
        for (size_t i = 0; i < checks.size(); ++i) {
            json += "    {\"check\": \"" + checks[i].name + "\", \"ok\": " +
                    (checks[i].ok ? "true" : "false") + ", \"detail\": \"" + checks[i].detail + "\"}";
            if (i + 1 < checks.size()) json += ",";
            json += "\r\n";
        }
        json += "  ]\r\n}\r\n";
        return json;
    }
};

std::string Quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    return WriteBytes(path, bytes);
}

} // namespace

int RunSelfTest(const Paths& paths, const std::wstring& outDir) {
    CreateDirectoryW(outDir.c_str(), nullptr);
    LogFmt("selftest: start outDir=%s", WideToUtf8(outDir).c_str());

    Report report;
    GdiPlusScope gdiplus;

    LARGE_INTEGER frequency{}, start{}, end{};
    QueryPerformanceFrequency(&frequency);

    RECT virtualRect = VirtualScreenRect();
    report.Prop("virtualBounds", Quote(std::to_string(virtualRect.left) + "," + std::to_string(virtualRect.top) +
                                       " " + std::to_string(virtualRect.right - virtualRect.left) + "x" +
                                       std::to_string(virtualRect.bottom - virtualRect.top)));
    report.CheckIt("virtual screen is non-empty", virtualRect.right > virtualRect.left && virtualRect.bottom > virtualRect.top);

    QueryPerformanceCounter(&start);
    Dib frozen = CaptureScreenRect(virtualRect);
    QueryPerformanceCounter(&end);
    if (!frozen.bitmap) {
        report.Prop("fatal", Quote("screen capture failed"));
        report.failures++;
        WriteText(outDir + L"\\selftest.json", report.ToJson());
        return 1;
    }
    report.Prop("fullCaptureMs", std::to_string(static_cast<long long>(ElapsedMs(start, end, frequency))));
    report.CheckIt("full capture matches virtual screen",
                   frozen.width == virtualRect.right - virtualRect.left &&
                   frozen.height == virtualRect.bottom - virtualRect.top);

    // 800x450 region, cropped from the frozen frame vs captured directly.
    int regionWidth = (std::min)(800, frozen.width - 20);
    int regionHeight = (std::min)(450, frozen.height - 20);
    int offsetX = 120;
    int offsetY = 120;

    RECT regionRect{virtualRect.left + offsetX, virtualRect.top + offsetY,
                    virtualRect.left + offsetX + regionWidth, virtualRect.top + offsetY + regionHeight};

    // Deterministic crop check on a synthetic image (the live screen can change between
    // two captures, which made the old direct-capture comparison flaky).
    {
        Dib synthetic;
        HDC screen = GetDC(nullptr);
        bool ok = synthetic.Create(screen, 96, 64);
        ReleaseDC(nullptr, screen);
        bool cropOk = false;
        if (ok) {
            for (int y = 0; y < synthetic.height; ++y) {
                auto* row = static_cast<uint32_t*>(synthetic.pixels) + static_cast<size_t>(y) * (synthetic.stride / 4);
                for (int x = 0; x < synthetic.width; ++x) {
                    row[x] = 0xFF000000u | (static_cast<uint32_t>(x & 0xFF) << 16) |
                             (static_cast<uint32_t>(y & 0xFF) << 8) | 0x11;
                }
            }
            Document probe;
            if (probe.CropFrom(synthetic, 8, 6, 20, 10)) {
                cropOk = true;
                for (int y = 0; y < 10 && cropOk; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(probe.image.Row(y));
                    for (int x = 0; x < 20; ++x) {
                        uint32_t expected = 0xFF000000u | (static_cast<uint32_t>((x + 8) & 0xFF) << 16) |
                                            (static_cast<uint32_t>((y + 6) & 0xFF) << 8) | 0x11;
                        if (row[x] != expected) { cropOk = false; break; }
                    }
                }
            }
        }
        report.CheckIt("crop copies the requested pixels", cropOk);
    }

    const uint8_t* pixels = frozen.Row(offsetY) + static_cast<size_t>(offsetX) * 4;

    QueryPerformanceCounter(&start);
    std::vector<uint8_t> lossless = EncodeWebP(pixels, regionWidth, regionHeight, frozen.stride, 100);
    QueryPerformanceCounter(&end);
    report.Prop("webpLosslessMs", std::to_string(static_cast<long long>(ElapsedMs(start, end, frequency))));


    QueryPerformanceCounter(&start);
    std::vector<uint8_t> q90 = EncodeWebP(pixels, regionWidth, regionHeight, frozen.stride, 90);
    QueryPerformanceCounter(&end);
    report.Prop("webpQ90Ms", std::to_string(static_cast<long long>(ElapsedMs(start, end, frequency))));

    std::vector<uint8_t> png = EncodePng(pixels, regionWidth, regionHeight, frozen.stride);

    report.Prop("region", Quote(std::to_string(regionWidth) + "x" + std::to_string(regionHeight)));
    report.Prop("webpLosslessBytes", std::to_string(lossless.size()));
    report.Prop("webpQ90Bytes", std::to_string(q90.size()));
    report.Prop("pngBytes", std::to_string(png.size()));
    report.CheckIt("webp lossless is smaller than png", !lossless.empty() && !png.empty() && lossless.size() < png.size(),
                   std::to_string(lossless.size()) + " < " + std::to_string(png.size()));

    WriteBytes(outDir + L"\\selftest.webp", lossless);
    WriteBytes(outDir + L"\\selftest_q90.webp", q90);
    WriteBytes(outDir + L"\\selftest.png", png);

    // Clipboard payload, same shape as the app writes it.
    std::wstring dropPath = outDir + L"\\Snip_selftest.webp";
    WriteBytes(dropPath, lossless);

    ClipboardReport clipboard = SetClipboardImage(nullptr, pixels, regionWidth, regionHeight, frozen.stride,
                                                  lossless, dropPath, true, true, true);
    report.Prop("clipboard", Quote(clipboard.Summary()));
    report.CheckIt("clipboard write succeeded", clipboard.ok, clipboard.error);

    bool hasDrop = false, hasPng = false, hasDib = false, hasWebp = false;
    for (const std::string& name : clipboard.formats) {
        if (name.rfind("CF_HDROP", 0) == 0) hasDrop = true;
        if (name.rfind("PNG(", 0) == 0) hasPng = true;
        if (name.rfind("CF_DIB(", 0) == 0) hasDib = true;
        if (name.rfind("WebP(", 0) == 0) hasWebp = true;
    }
    report.CheckIt("clipboard has CF_HDROP", hasDrop);
    report.CheckIt("clipboard has PNG", hasPng);
    report.CheckIt("clipboard has CF_DIB", hasDib);
    report.CheckIt("clipboard has WebP", hasWebp);

    std::wstring readPath = ReadClipboardDropPath();
    report.Prop("dropPath", Quote(WideToUtf8(readPath)));
    report.CheckIt("drop path round-trips", _wcsicmp(readPath.c_str(), dropPath.c_str()) == 0);
    report.CheckIt("drop file exists", GetFileAttributesW(readPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    // Hotkey parse/format parity.
    bool hotkeysOk = true;
    const struct { const wchar_t* text; const wchar_t* expected; } hotkeyCases[] = {
        {L"Ctrl+Alt+S", L"Ctrl+Alt+S"},
        {L"PrintScreen", L"PrintScreen"},
        {L"F9", L"F9"},
        {L"Shift+Win+D", L"Shift+Win+D"},
        {L"ctrl + shift + a", L"Ctrl+Shift+A"},
    };
    for (const auto& item : hotkeyCases) {
        Hotkey parsed;
        if (!ParseHotkey(item.text, parsed) || parsed.Format() != item.expected) hotkeysOk = false;
    }
    Hotkey invalid;
    if (ParseHotkey(L"Bogus+Key", invalid) || ParseHotkey(L"Ctrl+", invalid)) hotkeysOk = false;
    report.CheckIt("hotkey parse/format round-trips", hotkeysOk);

    // Settings write -> reload parity (restore the real file afterwards).
    std::string originalSettings;
    HANDLE originalFile = CreateFileW(paths.settingsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (originalFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        GetFileSizeEx(originalFile, &size);
        originalSettings.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ReadFile(originalFile, originalSettings.data(), static_cast<DWORD>(originalSettings.size()), &read, nullptr);
        CloseHandle(originalFile);
    }

    Settings custom;
    custom.clipboardMode = L"Strict";
    custom.webpQuality = 87;
    custom.saveFormat = L"png";
    custom.pinOffset = 44;
    custom.penWidth = 5;
    custom.markerSize = 17;
    custom.textSize = 22;
    custom.hotkey = L"Ctrl+Alt+S";
    custom.saveDirectory = outDir;
    SaveSettings(paths, custom);
    Settings reloaded = LoadSettings(paths);
    bool settingsSame = reloaded.clipboardMode == L"Strict" && reloaded.webpQuality == 87 &&
                        reloaded.saveFormat == L"png" && reloaded.pinOffset == 44 &&
                        reloaded.penWidth == 5 && reloaded.markerSize == 17 &&
                        reloaded.textSize == 22 && reloaded.hotkey == L"Ctrl+Alt+S";
    report.CheckIt("settings round-trip", settingsSame);

    if (!originalSettings.empty()) WriteText(paths.settingsPath, originalSettings);
    else DeleteFileW(paths.settingsPath.c_str());

    // The dropped file must be exactly the encoded bytes.
    bool dropBytes = false;
    {
        std::string fileBytes;
        HANDLE file = CreateFileW(dropPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            GetFileSizeEx(file, &size);
            fileBytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ReadFile(file, fileBytes.data(), static_cast<DWORD>(fileBytes.size()), &read, nullptr);
            CloseHandle(file);
        }
        dropBytes = fileBytes.size() == lossless.size() &&
                    memcmp(fileBytes.data(), lossless.data(), lossless.size()) == 0;
    }
    report.CheckIt("drop file bytes match the encoded webp", dropBytes);

    // Flatten must only change pixels inside the shape.
    {
        Document document;
        bool insideOnly = false;
        if (document.CropFrom(frozen, offsetX, offsetY, regionWidth, regionHeight)) {
            Shape box;
            box.tool = Tool::Box;
            box.size = 3;
            box.rect = RECT{60, 60, regionWidth - 60, regionHeight - 60};
            document.AddShape(box);
            Dib flat = document.Flatten();
            int outside = 0;
            int inside = 0;
            for (int y = 0; y < regionHeight; ++y) {
                const uint8_t* src = pixels + static_cast<size_t>(y) * frozen.stride;
                const uint8_t* dst = static_cast<const uint8_t*>(flat.pixels) + static_cast<size_t>(y) * flat.stride;
                for (int x = 0; x < regionWidth; ++x) {
                    if (memcmp(src + x * 4, dst + x * 4, 3) == 0) continue;
                    bool in = x >= box.rect.left - 1 && x <= box.rect.right + 1 &&
                              y >= box.rect.top - 1 && y <= box.rect.bottom + 1;
                    if (in) ++inside; else ++outside;
                }
            }
            insideOnly = outside == 0 && inside > 100;
            report.Prop("flattenChangedPixels", std::to_string(inside));
        }
        report.CheckIt("flatten edits only inside the shape", insideOnly);
    }

    Settings dialogSettings = LoadSettings(paths);
    bool dialogOk = TestSettingsDialogValidation(dialogSettings);
    report.CheckIt("settings dialog blocks out-of-range values", dialogOk);

    std::string json = report.ToJson();
    WriteText(outDir + L"\\selftest.json", json);

    LogFmt("selftest: done, %d failure(s)", report.failures);
    return report.failures == 0 ? 0 : 1;
}

} // namespace snip
