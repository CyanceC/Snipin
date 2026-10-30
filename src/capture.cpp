#include "capture.h"

#include <algorithm>
#include <windowsx.h>

namespace snip {
namespace {

constexpr wchar_t kOverlayClass[] = L"SniPin.Overlay";
constexpr COLORREF kAccent = RGB(0, 153, 255);

struct OverlayContext {
    const Dib* frozen = nullptr;
    const Dib* dimmed = nullptr;
    Dib* back = nullptr;
    RECT virtualRect{};
    OverlayResult* result = nullptr;
    HWND window = nullptr;
    bool dragging = false;
    POINT start{};
    POINT current{};
    RECT lastSelection{};
    bool hasLastSelection = false;
    bool defaultFullScreen = false;
};

RECT SelectionRect(const OverlayContext& context) {
    RECT rect{};
    rect.left = std::min(context.start.x, context.current.x);
    rect.top = std::min(context.start.y, context.current.y);
    rect.right = std::max(context.start.x, context.current.x);
    rect.bottom = std::max(context.start.y, context.current.y);
    return rect;
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

RECT BadgeRect(HDC reference, HFONT font, const RECT& selection, int clientWidth) {
    wchar_t text[64];
    swprintf_s(text, L"%d x %d", selection.right - selection.left, selection.bottom - selection.top);

    SIZE size{};
    HGDIOBJ old = SelectObject(reference, font);
    GetTextExtentPoint32W(reference, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(reference, old);

    const int paddingX = 10;
    const int paddingY = 5;
    int width = size.cx + paddingX * 2;
    int height = size.cy + paddingY * 2;
    int x = std::clamp<int>(static_cast<int>(selection.left), 4, std::max(4, clientWidth - width - 4));
    int y = static_cast<int>(selection.top) - height - 6;
    if (y < 4) y = std::min(static_cast<int>(selection.bottom) + 6, static_cast<int>(selection.top) + 6);
    return RECT{x, y, x + width, y + height};
}

void DrawBadge(HDC dc, HFONT font, const RECT& selection, int clientWidth) {
    wchar_t text[64];
    swprintf_s(text, L"%d x %d", selection.right - selection.left, selection.bottom - selection.top);
    RECT box = BadgeRect(dc, font, selection, clientWidth);
    Fill(dc, box, RGB(20, 20, 24));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 245));
    DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

RECT HintRect(HDC reference, HFONT font, int width) {
    const wchar_t* hint = L"Drag to snip   \x00B7   Enter: this screen   \x00B7   Shift+Enter: all   \x00B7   Esc: cancel";
    SIZE size{};
    HGDIOBJ old = SelectObject(reference, font);
    GetTextExtentPoint32W(reference, hint, static_cast<int>(wcslen(hint)), &size);
    SelectObject(reference, old);
    return RECT{(width - size.cx) / 2 - 12, 20, (width + size.cx) / 2 + 12, 20 + size.cy + 10};
}

void DrawHint(HDC dc, HFONT font, int width) {
    const wchar_t* hint = L"Drag to snip   \x00B7   Enter: this screen   \x00B7   Shift+Enter: all   \x00B7   Esc: cancel";
    RECT box = HintRect(dc, font, width);
    Fill(dc, box, RGB(18, 18, 22));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 230, 235));
    DrawTextW(dc, hint, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void Finish(OverlayContext& context, bool accepted, const RECT& rect) {
    if (context.result != nullptr) {
        context.result->accepted = accepted;
        context.result->rect = rect;
    }
    DestroyWindow(context.window);
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* context = reinterpret_cast<OverlayContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_PAINT: {
            if (context == nullptr) break;
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT dirty = paint.rcPaint;
            RECT client{};
            GetClientRect(window, &client);
            if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
                EndPaint(window, &paint);
                return 0;
            }

            // Compose the dirty rectangle off-screen, then blit it in one go.
            HDC target = (context->back != nullptr && context->back->bitmap != nullptr) ? context->back->dc : dc;
            BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                   context->dimmed->dc, dirty.left, dirty.top, SRCCOPY);

            HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            RECT intersection{};

            if (context->dragging) {
                RECT selection = SelectionRect(*context);
                if (selection.right > selection.left && selection.bottom > selection.top) {
                    if (IntersectRect(&intersection, &dirty, &selection)) {
                        BitBlt(target, intersection.left, intersection.top,
                               intersection.right - intersection.left, intersection.bottom - intersection.top,
                               context->frozen->dc, intersection.left, intersection.top, SRCCOPY);
                    }

                    RECT border = selection;
                    InflateRect(&border, 2, 2);
                    if (IntersectRect(&intersection, &dirty, &border)) {
                        HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
                        HGDIOBJ oldPen = SelectObject(target, pen);
                        HGDIOBJ oldBrush = SelectObject(target, GetStockObject(NULL_BRUSH));
                        Rectangle(target, selection.left, selection.top, selection.right, selection.bottom);
                        SelectObject(target, oldBrush);
                        SelectObject(target, oldPen);
                        DeleteObject(pen);
                    }

                    RECT badge = BadgeRect(target, font, selection, client.right);
                    if (IntersectRect(&intersection, &dirty, &badge)) {
                        DrawBadge(target, font, selection, client.right);
                    }
                }
            } else {
                RECT hint = HintRect(target, font, client.right);
                if (IntersectRect(&intersection, &dirty, &hint)) DrawHint(target, font, client.right);
            }

            DeleteObject(font);

            if (target != dc) {
                BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                       target, dirty.left, dirty.top, SRCCOPY);
            }

            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (context != nullptr && context->dragging) {
                RECT previous = SelectionRect(*context);
                context->current.x = GET_X_LPARAM(lparam);
                context->current.y = GET_Y_LPARAM(lparam);
                RECT now = SelectionRect(*context);

                RECT dirty = previous;
                UnionRect(&dirty, &dirty, &now);
                InflateRect(&dirty, 10, 10);
                dirty.top = std::max<LONG>(0, dirty.top - 44); // room for the size badge
                InvalidateRect(window, &dirty, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (context != nullptr) {
                context->dragging = true;
                context->start.x = context->current.x = GET_X_LPARAM(lparam);
                context->start.y = context->current.y = GET_Y_LPARAM(lparam);
                SetCapture(window);
                RECT dirty{context->current.x - 10, context->current.y - 54,
                           context->current.x + 10, context->current.y + 10};
                InvalidateRect(window, &dirty, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (context != nullptr && context->dragging) {
                ReleaseCapture();
                RECT selection = SelectionRect(*context);
                context->dragging = false;
                if (selection.right - selection.left >= 4 && selection.bottom - selection.top >= 4) {
                    RECT screen{context->virtualRect.left + selection.left,
                                context->virtualRect.top + selection.top,
                                context->virtualRect.left + selection.right,
                                context->virtualRect.top + selection.bottom};
                    Finish(*context, true, screen);
                } else {
                    RECT dirty = selection;
                    InflateRect(&dirty, 10, 54);
                    InvalidateRect(window, &dirty, FALSE);
                }
            }
            return 0;
        case WM_RBUTTONUP:
            if (context != nullptr) Finish(*context, false, RECT{});
            return 0;
        case WM_KEYDOWN:
            if (context != nullptr) {
                if (wparam == VK_ESCAPE) {
                    Finish(*context, false, RECT{});
                } else if (wparam == VK_RETURN) {
                    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                        Finish(*context, true, context->virtualRect);
                    } else {
                        POINT cursor{};
                        GetCursorPos(&cursor);
                        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO info{};
                        info.cbSize = sizeof(info);
                        if (GetMonitorInfoW(monitor, &info)) Finish(*context, true, info.rcMonitor);
                        else Finish(*context, true, context->virtualRect);
                    }
                }
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

RECT VirtualScreenRect() {
    RECT rect{};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

bool Dib::Create(HDC reference, int w, int h) {
    Release();
    if (w <= 0 || h <= 0) return false;

    dc = CreateCompatibleDC(reference);
    if (dc == nullptr) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        Release();
        return false;
    }

    previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));
    width = w;
    height = h;
    stride = w * 4;
    return true;
}

void Dib::Release() {
    if (dc != nullptr) {
        if (previous != nullptr) SelectObject(dc, previous);
        DeleteDC(dc);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
    bitmap = nullptr;
    dc = nullptr;
    previous = nullptr;
    pixels = nullptr;
    width = height = stride = 0;
}

Dib CaptureScreenRect(const RECT& screenRect) {
    Dib dib;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return dib;

    int w = screenRect.right - screenRect.left;
    int h = screenRect.bottom - screenRect.top;
    if (dib.Create(screen, w, h)) {
        BitBlt(dib.dc, 0, 0, w, h, screen, screenRect.left, screenRect.top, SRCCOPY | CAPTUREBLT);
    }
    ReleaseDC(nullptr, screen);
    return dib;
}

bool ShowOverlay(const Dib& frozen, const RECT& virtualRect, bool defaultFullScreen, OverlayResult& result) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = OverlayProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    windowClass.lpszClassName = kOverlayClass;
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogFmt("overlay: RegisterClassExW failed %lu", GetLastError());
        return false;
    }

    // Pre-dim once: 53 % brightness over a black layer, so painting only needs a BitBlt.
    Dib dimmed;
    if (dimmed.Create(frozen.dc, frozen.width, frozen.height)) {
        const auto* source = static_cast<const uint32_t*>(frozen.pixels);
        auto* target = static_cast<uint32_t*>(dimmed.pixels);
        const size_t count = static_cast<size_t>(frozen.width) * frozen.height;
        for (size_t i = 0; i < count; ++i) {
            uint32_t pixel = source[i];
            uint32_t r = ((pixel >> 16) & 0xFF) * 53 / 100;
            uint32_t g = ((pixel >> 8) & 0xFF) * 53 / 100;
            uint32_t b = (pixel & 0xFF) * 53 / 100;
            target[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    Dib back;
    back.Create(frozen.dc, frozen.width, frozen.height);

    OverlayContext context;
    context.frozen = &frozen;
    context.dimmed = &dimmed;
    context.back = &back;
    context.virtualRect = virtualRect;
    context.result = &result;
    context.defaultFullScreen = defaultFullScreen;

    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"SniPin",
                                  WS_POPUP, virtualRect.left, virtualRect.top,
                                  virtualRect.right - virtualRect.left, virtualRect.bottom - virtualRect.top,
                                  nullptr, nullptr, instance, &context);
    if (window == nullptr) {
        LogFmt("overlay: CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    context.window = window;

    RECT windowRect{};
    GetWindowRect(window, &windowRect);
    LogFmt("overlay: window=%ld,%ld..%ld,%ld virtual=%ld,%ld..%ld,%ld", windowRect.left, windowRect.top,
           windowRect.right, windowRect.bottom, virtualRect.left, virtualRect.top, virtualRect.right,
           virtualRect.bottom);

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);

    if (defaultFullScreen) {
        Finish(context, true, virtualRect);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result.accepted;
}

} // namespace snip
