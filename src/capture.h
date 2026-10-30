#pragma once

#include "core.h"

namespace snip {

struct Dib {
    HBITMAP bitmap = nullptr;
    HDC dc = nullptr;
    HBITMAP previous = nullptr;
    void* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;

    Dib() = default;
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
    Dib(Dib&& other) noexcept { MoveFrom(other); }
    Dib& operator=(Dib&& other) noexcept {
        if (this != &other) { Release(); MoveFrom(other); }
        return *this;
    }
    ~Dib() { Release(); }

    bool Create(HDC reference, int w, int h);
    void Release();

    void MoveFrom(Dib& other) noexcept {
        bitmap = other.bitmap; dc = other.dc; previous = other.previous; pixels = other.pixels;
        width = other.width; height = other.height; stride = other.stride;
        other.bitmap = nullptr; other.dc = nullptr; other.previous = nullptr; other.pixels = nullptr;
        other.width = other.height = other.stride = 0;
    }

    const uint8_t* Row(int y) const {
        return static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * stride;
    }
};

struct OverlayResult {
    bool accepted = false;
    RECT rect{}; // virtual-screen coordinates
};

RECT VirtualScreenRect();
Dib CaptureScreenRect(const RECT& screenRect);
bool ShowOverlay(const Dib& frozen, const RECT& virtualRect, bool defaultFullScreen, OverlayResult& result);

} // namespace snip
