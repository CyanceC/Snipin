#include "clipboard.h"

#include "codec.h"

#include <shlobj.h>
#include <cstring>

namespace snip {
namespace {

constexpr uint32_t kCustomDropEffect = 0;

std::vector<uint8_t> MakeDropFiles(const std::wstring& path) {
    const size_t headerSize = 20;
    std::vector<uint8_t> data(headerSize + (path.size() + 2) * sizeof(wchar_t));

    auto* drop = reinterpret_cast<DROPFILES*>(data.data());
    drop->pFiles = static_cast<DWORD>(headerSize);
    drop->fWide = TRUE;

    wchar_t* text = reinterpret_cast<wchar_t*>(data.data() + headerSize);
    memcpy(text, path.c_str(), path.size() * sizeof(wchar_t));
    text[path.size()] = L'\0';
    text[path.size() + 1] = L'\0';
    return data;
}

std::vector<uint8_t> MakeDib(const uint8_t* bgra, int width, int height, int stride) {
    const int rowBytes = width * 4;
    std::vector<uint8_t> dib(40 + static_cast<size_t>(rowBytes) * height);

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height; // bottom-up
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = rowBytes * height;

    uint8_t* destination = dib.data() + 40;
    for (int y = 0; y < height; ++y) {
        const uint8_t* source = bgra + static_cast<size_t>(height - 1 - y) * stride;
        uint8_t* row = destination + static_cast<size_t>(y) * rowBytes;
        memcpy(row, source, rowBytes);
        for (int x = 0; x < width; ++x) row[x * 4 + 3] = 0xFF;
    }
    return dib;
}

bool OpenWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

std::wstring FormatName(uint32_t format) {
    switch (format) {
        case CF_TEXT: return L"CF_TEXT";
        case CF_BITMAP: return L"CF_BITMAP";
        case CF_DIB: return L"CF_DIB";
        case CF_UNICODETEXT: return L"CF_UNICODETEXT";
        case CF_HDROP: return L"CF_HDROP";
        case CF_DIBV5: return L"CF_DIBV5";
        default: break;
    }

    if (format >= 0xC000) {
        wchar_t name[256] = {};
        int length = GetClipboardFormatNameW(format, name, 256);
        if (length > 0) return name;
    }
    return L"CF_" + std::to_wstring(format);
}

bool IsGlobalFormat(uint32_t format) {
    return format == CF_HDROP || format == CF_DIB || format == CF_DIBV5 || format >= 0xC000;
}

bool SetBytes(uint32_t format, const std::vector<uint8_t>& bytes) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (memory == nullptr) {
        LogFmt("clipboard: GlobalAlloc(%zu) failed", bytes.size());
        return false;
    }

    void* pointer = GlobalLock(memory);
    if (pointer == nullptr) {
        GlobalFree(memory);
        return false;
    }
    memcpy(pointer, bytes.data(), bytes.size());
    GlobalUnlock(memory);

    if (SetClipboardData(format, memory) == nullptr) {
        LogFmt("clipboard: SetClipboardData(0x%X) failed %lu", format, GetLastError());
        GlobalFree(memory);
        return false;
    }
    return true;
}

} // namespace

std::string ClipboardReport::Summary() const {
    if (!ok) return "failed: " + error;

    std::string text = "ok webp=" + std::to_string(webpBytes) + "B";
    if (pngBytes > 0) text += " png=" + std::to_string(pngBytes) + "B";
    if (dibBytes > 0) text += " dib=" + std::to_string(dibBytes) + "B";
    text += " formats=[";
    for (size_t i = 0; i < formats.size(); ++i) {
        if (i > 0) text += ", ";
        text += formats[i];
    }
    text += "]";
    return text;
}

ClipboardReport SetClipboardImage(HWND owner, const uint8_t* bgra, int width, int height, int stride,
                                  const std::vector<uint8_t>& webp, const std::wstring& dropPath,
                                  bool includePng, bool includeDib, bool includeDrop) {
    ClipboardReport report;
    report.dropPath = dropPath;
    report.webpBytes = static_cast<int>(webp.size());

    std::vector<uint8_t> png;
    std::vector<uint8_t> dib;
    if (includePng) {
        png = EncodePng(bgra, width, height, stride);
        report.pngBytes = static_cast<int>(png.size());
        if (png.empty()) {
            report.error = "png encode failed";
            return report;
        }
    }
    if (includeDib) {
        dib = MakeDib(bgra, width, height, stride);
        report.dibBytes = static_cast<int>(dib.size());
    }

    if (includeDrop && dropPath.empty()) {
        report.error = "no drop path";
        return report;
    }

    if (!OpenWithRetry(owner)) {
        report.error = "OpenClipboard failed";
        return report;
    }

    bool ok = false;
    do {
        if (!EmptyClipboard()) {
            report.error = "EmptyClipboard failed";
            break;
        }
        if (includeDrop) {
            std::vector<uint8_t> dropFiles = MakeDropFiles(dropPath);
            if (!SetBytes(CF_HDROP, dropFiles)) {
                report.error = "CF_HDROP failed";
                break;
            }
        }
        if (!png.empty() && !SetBytes(RegisterClipboardFormatW(L"PNG"), png)) {
            report.error = "PNG failed";
            break;
        }
        if (!dib.empty() && !SetBytes(CF_DIB, dib)) {
            report.error = "CF_DIB failed";
            break;
        }
        SetBytes(RegisterClipboardFormatW(L"WebP"), webp);
        SetBytes(RegisterClipboardFormatW(L"image/webp"), webp);

        uint32_t effect = 1; // DROPEFFECT_COPY
        std::vector<uint8_t> effectBytes(sizeof(effect));
        memcpy(effectBytes.data(), &effect, sizeof(effect));
        SetBytes(RegisterClipboardFormatW(L"Preferred DropEffect"), effectBytes);

        ok = true;
    } while (false);

    CloseClipboard();
    report.ok = ok;
    report.formats = EnumerateClipboardFormats();
    return report;
}

std::vector<std::string> EnumerateClipboardFormats() {
    std::vector<std::string> formats;
    if (!OpenWithRetry(nullptr)) return formats;

    uint32_t format = 0;
    while ((format = EnumClipboardFormats(format)) != 0) {
        std::string entry = WideToUtf8(FormatName(format));
        if (IsGlobalFormat(format)) {
            HANDLE data = GetClipboardData(format);
            if (data != nullptr) entry += "(" + std::to_string(GlobalSize(data)) + ")";
            else entry += "(?)";
        } else {
            entry += "(?)";
        }
        formats.push_back(entry);
    }

    CloseClipboard();
    return formats;
}

std::wstring ReadClipboardDropPath() {
    std::wstring path;
    if (!OpenWithRetry(nullptr)) return path;

    HANDLE data = GetClipboardData(CF_HDROP);
    if (data != nullptr) {
        wchar_t buffer[MAX_PATH] = {};
        if (DragQueryFileW(reinterpret_cast<HDROP>(data), 0, buffer, MAX_PATH) > 0) path = buffer;
    }

    CloseClipboard();
    return path;
}

} // namespace snip
