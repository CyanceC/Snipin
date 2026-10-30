#pragma once

#include "core.h"
#include <vector>

namespace snip {

struct ClipboardReport {
    bool ok = false;
    std::string error;
    std::wstring dropPath;
    int webpBytes = 0;
    int pngBytes = 0;
    int dibBytes = 0;
    std::vector<std::string> formats;

    std::string Summary() const;
};

/// Publishes a snip in the same shape as the C# build: file drop + PNG + DIB + custom WebP.
ClipboardReport SetClipboardImage(HWND owner, const uint8_t* bgra, int width, int height, int stride,
                                  const std::vector<uint8_t>& webp, const std::wstring& dropPath,
                                  bool includePng, bool includeDib, bool includeDrop);

std::vector<std::string> EnumerateClipboardFormats();
std::wstring ReadClipboardDropPath();

} // namespace snip
