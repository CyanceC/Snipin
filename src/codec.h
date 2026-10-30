#pragma once

#include "core.h"
#include <vector>

namespace snip {

/// GDI+ is only used to encode PNG for the clipboard; it needs process-wide startup.
class GdiPlusScope {
public:
    GdiPlusScope();
    ~GdiPlusScope();
    GdiPlusScope(const GdiPlusScope&) = delete;
    GdiPlusScope& operator=(const GdiPlusScope&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    ULONG_PTR token_ = 0;
};

std::vector<uint8_t> EncodeWebP(const uint8_t* bgra, int width, int height, int stride, int quality);
std::vector<uint8_t> EncodePng(const uint8_t* bgra, int width, int height, int stride);

/// Saves to .png/.jpg/.bmp through GDI+ (format is "png", "jpg" or "bmp").
bool SaveImage(const std::wstring& path, const uint8_t* bgra, int width, int height, int stride,
               const std::wstring& format);

} // namespace snip
