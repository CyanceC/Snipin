#include "core.h"

#include "capture.h"
#include "codec.h"
#include "document.h"
#include "pin.h"

namespace snip {

int RunWorker(const Paths& paths, bool fullScreen) {
    LogFmt("worker: start full=%d pid=%lu", fullScreen ? 1 : 0, GetCurrentProcessId());

    Settings settings = LoadSettings(paths);
    GdiPlusScope gdiplus;

    LARGE_INTEGER frequency{}, captureStart{}, captureEnd{}, overlayEnd{}, pinEnd{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&captureStart);

    RECT virtualRect = VirtualScreenRect();
    Dib frozen = CaptureScreenRect(virtualRect);
    if (!frozen.bitmap) {
        LogLine("worker: screen capture failed");
        return 1;
    }
    QueryPerformanceCounter(&captureEnd);

    OverlayResult overlay;
    if (!ShowOverlay(frozen, virtualRect, fullScreen, overlay) || !overlay.accepted) {
        LogLine("worker: cancelled");
        return 0;
    }
    QueryPerformanceCounter(&overlayEnd);

    RECT selection = overlay.rect;
    int width = selection.right - selection.left;
    int height = selection.bottom - selection.top;
    if (width <= 0 || height <= 0) {
        LogLine("worker: empty selection");
        return 1;
    }

    Document document;
    if (!document.CropFrom(frozen, selection.left - virtualRect.left, selection.top - virtualRect.top,
                           width, height)) {
        LogLine("worker: crop failed");
        return 1;
    }
    frozen.Release(); // the document owns the pixels now

    if (!CreatePin(std::move(document), settings, paths, selection)) {
        LogLine("worker: pin creation failed");
        return 1;
    }
    QueryPerformanceCounter(&pinEnd);

    LogFmt("worker: %dx%d capture=%.1fms overlay=%.1fms pin=%.1fms", width, height,
           ElapsedMs(captureStart, captureEnd, frequency), ElapsedMs(captureEnd, overlayEnd, frequency),
           ElapsedMs(overlayEnd, pinEnd, frequency));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    LogLine("worker: exit");
    return 0;
}

} // namespace snip
