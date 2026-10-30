#include "pin.h"

#include "clipboard.h"
#include "codec.h"
#include "settings_dialog.h"

#include <windowsx.h>
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;

namespace snip {
namespace {

constexpr int kFrame = 3;
constexpr int kItem = 26;
constexpr int kGap = 4;
constexpr int kPad = 6;
constexpr int kGroupGap = 12;
constexpr int kMinWidth = 220;
constexpr int kMinView = 40;
constexpr int kHandle = 7;
constexpr COLORREF kCyan = RGB(0, 0xAA, 0xAA);
constexpr COLORREF kGray = RGB(0x3C, 0x3C, 0x3C);
constexpr COLORREF kToolbarColor = RGB(32, 32, 40);
constexpr ARGB kIconColor = 0xFFE1E1EB;
constexpr ARGB kIslandFill = 0x66000000; // 40 % black
constexpr UINT kCopyDone = WM_APP + 10;
constexpr UINT_PTR kToastTimer = 2;

const COLORREF kBasicColors[6] = {
    RGB(0xFF, 0x00, 0x00), RGB(0xFF, 0x8C, 0x1B), RGB(0xFD, 0xFF, 0x17),
    RGB(0x22, 0xD8, 0x00), RGB(0x07, 0x00, 0xD8), RGB(0xBB, 0x00, 0xFF),
};

enum ItemId {
    kPointer = 1,
    kBox,
    kEllipse,
    kArrow,
    kHighlight,
    kPen,
    kMarker,
    kRedact,
    kText,
    kCrop,
    kSizeMinus,
    kSizeValue,
    kSizePlus,
    kColor,
    kUndo,
    kSave,
    kCopy,
    kSettings,
    kLastToolItem = kCrop
};

constexpr int kSwatchFirst = 30;

struct Item {
    int id = 0;
    bool secondRow = false;
    RECT rect{};
};

struct Pin {
    HWND window = nullptr;
    Document document;
    Settings settings;
    Paths paths;

    Tool tool = Tool::Pointer;
    COLORREF color = RGB(0xEB, 0x46, 0x46);
    int penWidth = 3;
    int markerSize = 12;
    int textSize = 16;
    int dpi = 96;

    double scaleX = 1.0;
    double scaleY = 1.0;
    int viewWidth = 0;
    int viewHeight = 0;
    int toolbarWidth = 0;
    int toolbarHeight = 0;
    bool secondRow = false;
    std::vector<Item> items;

    bool alwaysOnTop = true;
    int hoverIsland = 0;
    HWND hexEditor = nullptr;
    WNDPROC hexProc = nullptr;
    RECT islandPin{};
    RECT islandClose{};

    bool dragging = false;
    bool preview = false;
    bool moving = false;
    int resizing = 0; // handle id 1..8
    POINT start{};
    POINT current{};
    POINT moveCursor{};
    RECT moveRect{};
    RECT resizeWinRect{};
    RECT resizeImgRect{};
    POINT resizeStart{};
    std::vector<POINT> stroke;

    HWND editor = nullptr;
    WNDPROC editorProc = nullptr;
    bool committing = false;

    Dib back;
    bool busy = false;
    bool pendingCopy = false;
    std::wstring toast;
    ULONGLONG toastUntil = 0;

    int Scaled(int value) const { return MulDiv(value, dpi, 96); }

    RECT ImageRect() const { return RECT{kFrame, kFrame, kFrame + viewWidth, kFrame + viewHeight}; }

    int ActiveSize() const {
        if (tool == Tool::Text) return textSize;
        if (tool == Tool::Marker || tool == Tool::Highlight) return markerSize;
        return penWidth;
    }

    bool ToolUsesStyle() const {
        return tool == Tool::Box || tool == Tool::Ellipse || tool == Tool::Arrow ||
               tool == Tool::Highlight || tool == Tool::Pen || tool == Tool::Marker ||
               tool == Tool::Text;
    }

    void AdjustSize(int delta) {
        if (tool == Tool::Text) {
            textSize = std::clamp(textSize + delta, 8, 72);
        } else if (tool == Tool::Marker || tool == Tool::Highlight) {
            markerSize = std::clamp(markerSize + delta, 2, 60);
        } else {
            penWidth = std::clamp(penWidth + delta, 1, 24);
        }
    }
};

std::vector<Pin*> g_pins;

void PositionHexEdit(Pin& pin);
void SyncHexEdit(Pin& pin);
void Layout(Pin& pin, bool moveWindow);

int ScaledBy(int value, int dpi) { return MulDiv(value, dpi, 96); }

void BuildItems(Pin& pin, int clientWidth) {
    pin.items.clear();
    int item = pin.Scaled(kItem);
    int gap = pin.Scaled(kGap);
    int pad = pin.Scaled(kPad);
    int groupGap = pin.Scaled(kGroupGap);

    int x = pad;
    int y = pad;

    auto place = [&](int id, int group, int previousGroup) {
        if (previousGroup != -1 && previousGroup != group) x += groupGap;
        pin.items.push_back(Item{id, false, RECT{x, y, x + item, y + item}});
        x += item + gap;
    };

    const int order[10] = {kPointer, kBox, kEllipse, kArrow, kHighlight, kPen, kMarker, kRedact, kText, kCrop};
    const int groups[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
    int previous = -1;
    for (int i = 0; i < 10; ++i) {
        place(order[i], groups[i], previous);
        previous = groups[i];
    }
    place(kUndo, 5, previous);
    place(kSave, 5, 5);
    place(kCopy, 5, 5);

    pin.toolbarWidth = x - gap + pad;

    int rowY = pad + item + gap;
    pin.items.push_back(Item{kColor, true, RECT{pad, rowY, pad + item, rowY + item}});
    int swatchX = pad + item + ScaledBy(8, pin.dpi) + ScaledBy(88, pin.dpi) + ScaledBy(8, pin.dpi);
    for (int i = 0; i < 6; ++i) {
        pin.items.push_back(Item{kSwatchFirst + i, true, RECT{swatchX, rowY, swatchX + item, rowY + item}});
        swatchX += item + ScaledBy(2, pin.dpi);
    }
    int right = pin.toolbarWidth - pad;
    auto placeRight = [&](int id) {
        int left = right - item;
        pin.items.push_back(Item{id, true, RECT{left, rowY, right, rowY + item}});
        right = left - gap;
    };
    placeRight(kSettings);
    right -= groupGap - gap;
    placeRight(kSizePlus);
    placeRight(kSizeValue);
    placeRight(kSizeMinus);

    pin.toolbarHeight = rowY + item + pad;
    (void)clientWidth;
}

bool PointIn(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

POINT ToImage(const Pin& pin, POINT client) {
    POINT point{static_cast<LONG>((client.x - kFrame) / pin.scaleX),
                static_cast<LONG>((client.y - kFrame) / pin.scaleY)};
    point.x = std::clamp<LONG>(point.x, 0, pin.document.image.width);
    point.y = std::clamp<LONG>(point.y, 0, pin.document.image.height);
    return point;
}

RECT MakeRect(POINT a, POINT b) {
    return RECT{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

RECT HandleRect(const Pin& pin, int handle) {
    RECT image = pin.ImageRect();
    int cx = (image.left + image.right) / 2;
    int cy = (image.top + image.bottom) / 2;
    // 1 = bottom (height), 2 = bottom-right (ratio), 3 = right (width)
    POINT points[3] = {{cx, image.bottom}, {image.right, image.bottom}, {image.right, cy}};
    POINT p = points[handle - 1];
    int half = ScaledBy(kHandle, pin.dpi) / 2;
    return RECT{p.x - half, p.y - half, p.x - half + ScaledBy(kHandle, pin.dpi),
                p.y - half + ScaledBy(kHandle, pin.dpi)};
}

void Layout(Pin& pin, bool moveWindow);

void UpdateIslands(Pin& pin) {
    int size = pin.Scaled(22);
    int margin = pin.Scaled(8);
    int pinWidth = pin.Scaled(66);
    pin.islandPin = RECT{kFrame + margin, kFrame + margin, kFrame + margin + pinWidth, kFrame + margin + size};
    pin.islandClose = RECT{kFrame + pin.viewWidth - margin - pin.Scaled(26), kFrame + margin,
                           kFrame + pin.viewWidth - margin, kFrame + margin + size};
}

void ApplyGeometry(Pin& pin, bool moveWindow) {
    int clientWidth = std::max(pin.toolbarWidth, pin.viewWidth + kFrame * 2);
    BuildItems(pin, clientWidth);
    int clientHeight = pin.viewHeight + kFrame * 2 + pin.toolbarHeight;
    UpdateIslands(pin);

    RECT current{};
    GetWindowRect(pin.window, &current);
    SetWindowPos(pin.window, nullptr, moveWindow ? current.left : current.left, current.top,
                 std::max(clientWidth, pin.toolbarWidth), clientHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    PositionHexEdit(pin);
    InvalidateRect(pin.window, nullptr, FALSE);
}

void Layout(Pin& pin, bool moveWindow) {
    pin.secondRow = pin.ToolUsesStyle();
    ApplyGeometry(pin, moveWindow);
}

void ForceForeground(HWND window) {
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    DWORD currentThread = GetCurrentThreadId();
    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != currentThread) {
        attached = AttachThreadInput(foregroundThread, currentThread, TRUE) != FALSE;
    }
    SetForegroundWindow(window);
    BringWindowToTop(window);
    SetFocus(window);
    if (attached) AttachThreadInput(foregroundThread, currentThread, FALSE);
}

// ---------------------------------------------------------------- copy / save

struct CopyJob {
    Dib image;
    Settings settings;
    HWND window = nullptr;
    std::wstring dropPath;
};

void StartCopy(Pin& pin);

void CopyFinished(Pin& pin, bool ok) {
    pin.busy = false;
    LogFmt("pin: copy %s", ok ? "done" : "failed");
    if (pin.pendingCopy) {
        pin.pendingCopy = false;
        StartCopy(pin);
    }
}

void StartCopy(Pin& pin) {
    if (pin.busy) {
        pin.pendingCopy = true;
        return;
    }

    auto* job = new CopyJob();
    job->image = pin.document.Flatten();
    if (!job->image.bitmap) {
        delete job;
        LogLine("pin: flatten failed");
        return;
    }
    job->settings = pin.settings;
    job->window = pin.window;
    job->dropPath = UniqueFile(pin.paths.tempDir, L"Snip", L".webp");
    pin.busy = true;

    std::thread([job]() {
        const uint8_t* pixels = static_cast<const uint8_t*>(job->image.pixels);
        std::vector<uint8_t> webp = EncodeWebP(pixels, job->image.width, job->image.height,
                                               job->image.stride, job->settings.webpQuality);
        bool ok = false;
        if (!webp.empty()) {
            HANDLE file = CreateFileW(job->dropPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, webp.data(), static_cast<DWORD>(webp.size()), &written, nullptr);
                CloseHandle(file);
            }
            bool strict = _wcsicmp(job->settings.clipboardMode.c_str(), L"Strict") == 0;
            bool imageOnly = _wcsicmp(job->settings.clipboardMode.c_str(), L"ImageOnly") == 0;
            ClipboardReport report = SetClipboardImage(nullptr, pixels, job->image.width, job->image.height,
                                                       job->image.stride, webp, job->dropPath, !strict, !strict,
                                                       !imageOnly);
            LogFmt("pin: copy %s", report.Summary().c_str());
            ok = report.ok;
        } else {
            LogLine("pin: webp encode failed");
        }
        if (job->window != nullptr) PostMessageW(job->window, kCopyDone, ok ? 1 : 0, 0);
        delete job;
    }).detach();
}

void ShowToast(Pin& pin, const std::wstring& text) {
    pin.toast = text;
    pin.toastUntil = GetTickCount64() + 2200;
    SetTimer(pin.window, kToastTimer, 200, nullptr);
    InvalidateRect(pin.window, nullptr, FALSE);
}

bool SavePinAs(Pin& pin) {
    Dib flattened;
    const Dib* source = &pin.document.image;
    if (pin.document.HasShapes()) {
        flattened = pin.document.Flatten();
        source = &flattened;
    }

    CreateDirectoryW(pin.settings.saveDirectory.c_str(), nullptr);
    std::wstring suggested = UniqueFile(pin.settings.saveDirectory, L"Snip", L"." + pin.settings.saveFormat);

    wchar_t file[MAX_PATH] = {};
    wcsncpy_s(file, suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = pin.window;
    dialog.lpstrFilter = L"WebP image (*.webp)\0*.webp\0PNG image (*.png)\0*.png\0"
                         L"JPEG image (*.jpg)\0*.jpg\0Bitmap (*.bmp)\0*.bmp\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = pin.settings.saveDirectory.c_str();
    dialog.lpstrDefExt = pin.settings.saveFormat.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetSaveFileNameW(&dialog)) return false;

    std::wstring path = file;
    std::wstring extension = L"webp";
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"bmp" || ext == L"webp") {
            extension = (ext == L"jpeg") ? L"jpg" : ext;
        }
    }

    if (!SaveImage(path, static_cast<const uint8_t*>(source->pixels), source->width, source->height,
                   source->stride, extension)) {
        LogFmt("pin: save failed %s", WideToUtf8(path).c_str());
        ShowToast(pin, L"Save failed");
        return false;
    }

    size_t slash = path.find_last_of(L'\\');
    LogFmt("pin: saved %s", WideToUtf8(path).c_str());
    ShowToast(pin, L"Saved " + path.substr(slash == std::wstring::npos ? 0 : slash + 1));
    return true;
}

// ---------------------------------------------------------------- text editors

void FinishText(Pin& pin, bool commit);

LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* pin = reinterpret_cast<Pin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pin != nullptr) {
        if (message == WM_KEYDOWN) {
            if (wparam == VK_RETURN) { FinishText(*pin, true); return 0; }
            if (wparam == VK_ESCAPE) { FinishText(*pin, false); return 0; }
        } else if (message == WM_KILLFOCUS) {
            FinishText(*pin, true);
            return 0;
        }
    }
    return CallWindowProcW(pin != nullptr ? pin->editorProc : DefWindowProcW, window, message, wparam, lparam);
}

void StartText(Pin& pin, POINT clientPoint) {
    if (pin.editor != nullptr) FinishText(pin, true);

    HWND editor = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  clientPoint.x, clientPoint.y, 260, 26, pin.window,
                                  reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    if (editor == nullptr) return;

    HFONT font = CreateFontW(-pin.textSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(editor, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    pin.editor = editor;
    pin.editorProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(editor, GWLP_WNDPROC,
                                                                 reinterpret_cast<LONG_PTR>(EditProc)));
    SetWindowLongPtrW(editor, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&pin));
    SetFocus(editor);
    InvalidateRect(editor, nullptr, TRUE);
}

void FinishText(Pin& pin, bool commit) {
    if (pin.editor == nullptr || pin.committing) return;
    pin.committing = true;

    if (commit) {
        int length = GetWindowTextLengthW(pin.editor);
        if (length > 0) {
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(pin.editor, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));

            RECT rect{};
            GetWindowRect(pin.editor, &rect);
            POINT topLeft{rect.left, rect.top};
            ScreenToClient(pin.window, &topLeft);

            Shape shape;
            shape.tool = Tool::Text;
            shape.text = text;
            shape.size = pin.textSize;
            shape.color = pin.color;
            shape.rect = RECT{static_cast<LONG>((topLeft.x - kFrame) / pin.scaleX),
                              static_cast<LONG>((topLeft.y - kFrame) / pin.scaleY),
                              static_cast<LONG>((topLeft.x - kFrame) / pin.scaleX + text.size() * pin.textSize * 0.62f) + 8,
                              static_cast<LONG>((topLeft.y - kFrame) / pin.scaleY + pin.textSize + 6)};
            pin.document.AddShape(shape);
            StartCopy(pin);
        }
    }

    HWND editor = pin.editor;
    pin.editor = nullptr;
    DestroyWindow(editor);
    pin.committing = false;
    InvalidateRect(pin.window, nullptr, FALSE);
}

// ---------------------------------------------------------------- colour

bool ParseHexColor(const std::wstring& text, COLORREF& out) {
    std::wstring digits;
    for (wchar_t c : text) {
        if (iswxdigit(c)) digits.push_back(c);
    }
    if (digits.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long value = wcstoul(digits.c_str(), &end, 16);
    if (end == nullptr || *end != 0) return false;
    out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

void ApplyHex(Pin& pin) {
    if (pin.hexEditor == nullptr) return;
    wchar_t buffer[32] = {};
    GetWindowTextW(pin.hexEditor, buffer, 32);
    COLORREF parsed = 0;
    if (ParseHexColor(buffer, parsed)) {
        pin.color = parsed;
        InvalidateRect(pin.window, nullptr, FALSE);
    }
    SetFocus(pin.window);
}

void SyncHexEdit(Pin& pin) {
    if (pin.hexEditor == nullptr) return;
    wchar_t buffer[16];
    swprintf_s(buffer, L"#%02X%02X%02X", GetRValue(pin.color), GetGValue(pin.color), GetBValue(pin.color));
    SetWindowTextW(pin.hexEditor, buffer);
}

void PositionHexEdit(Pin& pin) {
    if (pin.hexEditor == nullptr) return;
    int top = pin.viewHeight + kFrame * 2 + pin.Scaled(kPad) + pin.Scaled(kItem) + pin.Scaled(kGap);
    SetWindowPos(pin.hexEditor, nullptr, pin.Scaled(kPad) + pin.Scaled(kItem) + pin.Scaled(8), top + pin.Scaled(2),
                 pin.Scaled(88), pin.Scaled(22), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK HexProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* pin = reinterpret_cast<Pin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pin != nullptr && message == WM_KEYDOWN) {
        if (wparam == VK_RETURN) { ApplyHex(*pin); return 0; }
        if (wparam == VK_ESCAPE) { SetFocus(pin->window); return 0; }
    }
    return CallWindowProcW(pin != nullptr ? pin->hexProc : DefWindowProcW, window, message, wparam, lparam);
}

// ---------------------------------------------------------------- icons

const wchar_t* IconFontFamily() {
    static std::wstring name;
    if (!name.empty()) return name.c_str();
    name = L"Segoe MDL2 Assets";
    InstalledFontCollection collection;
    int count = collection.GetFamilyCount();
    if (count > 0) {
        std::vector<FontFamily> families(count);
        int found = 0;
        collection.GetFamilies(count, families.data(), &found);
        for (int i = 0; i < found; ++i) {
            WCHAR familyName[LF_FACESIZE] = {};
            families[i].GetFamilyName(familyName);
            if (wcscmp(familyName, L"Segoe Fluent Icons") == 0) {
                name = L"Segoe Fluent Icons";
                break;
            }
        }
    }
    return name.c_str();
}

RECT PointerOpaqueBounds(HICON icon, int size) {
    RECT bounds{0, 0, size, size};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap != nullptr && pixels != nullptr) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        PatBlt(dc, 0, 0, size, size, BLACKNESS);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        GdiFlush();

        auto* data = static_cast<const uint32_t*>(pixels);
        int minX = size, minY = size, maxX = -1, maxY = -1;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                uint32_t px = data[y * size + x];
                if ((px & 0x00FFFFFFu) != 0) {
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX >= minX && maxY >= minY) bounds = RECT{minX, minY, maxX + 1, maxY + 1};
        SelectObject(dc, old);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bounds;
}

HICON PointerIcon(int size) {
    static HICON cached = nullptr;
    static int cachedSize = 0;
    if (cached != nullptr && cachedSize == size) return cached;
    if (cached != nullptr) {
        DestroyIcon(cached);
        cached = nullptr;
    }

    const wchar_t* candidates[3] = {
        L"C:\\Windows\\Cursors\\aero_arrow_xl.cur",
        L"C:\\Windows\\Cursors\\aero_arrow_l.cur",
        L"C:\\Windows\\Cursors\\aero_arrow.cur",
    };
    for (const wchar_t* path : candidates) {
        HICON icon = reinterpret_cast<HICON>(
            LoadImageW(nullptr, path, IMAGE_CURSOR, size, size, LR_LOADFROMFILE));
        if (icon != nullptr) {
            cached = icon;
            cachedSize = size;
            return cached;
        }
    }
    return LoadCursorW(nullptr, IDC_ARROW); // shared resource, never destroyed
}

void DrawGlyphIcon(Graphics& graphics, wchar_t glyph, const RECT& box, const SolidBrush& brush) {
    FontFamily family(IconFontFamily());
    if (family.GetLastStatus() != Ok) return;
    Font font(&family, (box.bottom - box.top) * 0.62f, FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) return;
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    RectF area(static_cast<REAL>(box.left), static_cast<REAL>(box.top),
               static_cast<REAL>(box.right - box.left), static_cast<REAL>(box.bottom - box.top));
    graphics.DrawString(&glyph, 1, &font, area, &format, &brush);
}

void DrawIcon(Graphics& graphics, int id, const RECT& box, const Pin& pin) {
    const int inset = std::max(4, static_cast<int>(box.right - box.left) / 5);
    RectF inner(static_cast<REAL>(box.left + inset), static_cast<REAL>(box.top + inset),
                static_cast<REAL>(box.right - box.left - inset * 2),
                static_cast<REAL>(box.bottom - box.top - inset * 2));

    SolidBrush brush(kIconColor);
    Pen pen(Color(255, 225, 225, 235), 1.6f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);

    switch (id) {
        case kPointer: {
            PointF points[3] = {PointF(inner.X + 2, inner.Y), PointF(inner.X + 2, inner.Y + inner.Height),
                                PointF(inner.X + inner.Width * 0.62f, inner.Y + inner.Height * 0.72f)};
            graphics.FillPolygon(&brush, points, 3);
            break;
        }
        case kBox:
            graphics.DrawRectangle(&pen, inner);
            break;
        case kEllipse:
            graphics.DrawEllipse(&pen, inner);
            break;
        case kArrow: {
            graphics.DrawLine(&pen, inner.X, inner.Y + inner.Height, inner.X + inner.Width, inner.Y);
            PointF to(inner.X + inner.Width, inner.Y);
            PointF head[3] = {to, PointF(to.X - 7, to.Y + 1), PointF(to.X - 1, to.Y + 7)};
            graphics.FillPolygon(&brush, head, 3);
            break;
        }
        case kHighlight: {
            Pen high(Color(150, 255, 230, 60), 5.0f);
            high.SetStartCap(LineCapRound);
            high.SetEndCap(LineCapRound);
            graphics.DrawLine(&high, inner.X, inner.Y + inner.Height, inner.X + inner.Width, inner.Y);
            break;
        }
        case kPen:
            DrawGlyphIcon(graphics, L'\xED63', box, brush);
            break;
        case kMarker:
            DrawGlyphIcon(graphics, L'\xED64', box, brush);
            break;
        case kRedact:
            graphics.FillRectangle(&brush, inner);
            break;
        case kText: {
            FontFamily family(L"Segoe UI");
            Font font(&family, (box.bottom - box.top) * 0.72f, FontStyleBold, UnitPixel);
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF area(static_cast<REAL>(box.left), static_cast<REAL>(box.top),
                       static_cast<REAL>(box.right - box.left), static_cast<REAL>(box.bottom - box.top));
            graphics.DrawString(L"T", -1, &font, area, &format, &brush);
            break;
        }
        case kCrop:
            DrawGlyphIcon(graphics, L'\xE7A8', box, brush);
            break;
        case kSizeMinus:
            DrawGlyphIcon(graphics, L'\xE738', box, brush);
            break;
        case kSizePlus:
            DrawGlyphIcon(graphics, L'\xE710', box, brush);
            break;
        case kSizeValue: {
            FontFamily family(L"Segoe UI");
            Font font(&family, (box.bottom - box.top) * 0.48f, FontStyleBold, UnitPixel);
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF area(static_cast<REAL>(box.left), static_cast<REAL>(box.top),
                       static_cast<REAL>(box.right - box.left), static_cast<REAL>(box.bottom - box.top));
            std::wstring label = std::to_wstring(pin.ActiveSize());
            graphics.DrawString(label.c_str(), -1, &font, area, &format, &brush);
            break;
        }
        case kColor: {
            SolidBrush swatch(Color(255, GetRValue(pin.color), GetGValue(pin.color), GetBValue(pin.color)));
            graphics.FillRectangle(&swatch, inner);
            graphics.DrawRectangle(&pen, inner);
            break;
        }
        case kUndo:
            DrawGlyphIcon(graphics, L'\xE7A7', box, brush);
            break;
        case kSave:
            DrawGlyphIcon(graphics, L'\xE74E', box, brush);
            break;
        case kSettings:
            DrawGlyphIcon(graphics, L'\xE713', box, brush);
            break;
        case kCopy: {
            RectF backRect(inner.X, inner.Y + 3, inner.Width - 3, inner.Height - 3);
            RectF frontRect(inner.X + 3, inner.Y, inner.Width - 3, inner.Height - 3);
            graphics.DrawRectangle(&pen, backRect);
            graphics.DrawRectangle(&pen, frontRect);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- painting

void DrawRounded(Graphics& graphics, const RECT& box, float radius, ARGB fill, ARGB border) {
    GraphicsPath path;
    RectF rect(static_cast<REAL>(box.left) + 0.5f, static_cast<REAL>(box.top) + 0.5f,
               static_cast<REAL>(box.right - box.left) - 1.0f, static_cast<REAL>(box.bottom - box.top) - 1.0f);
    float d = radius * 2;
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    Color fillColor(fill);
    SolidBrush brush(fillColor);
    graphics.FillPath(&brush, &path);
    Pen pen(Color(border), 1.0f);
    graphics.DrawPath(&pen, &path);
}

void RenderPin(Pin& pin) {
    RECT client{};
    GetClientRect(pin.window, &client);
    if (pin.back.width != client.right || pin.back.height != client.bottom) {
        pin.back.Create(GetDC(pin.window), client.right, client.bottom);
    }
    if (pin.back.bitmap == nullptr) return;

    HDC dc = pin.back.dc;
    HBRUSH background = CreateSolidBrush(kToolbarColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    RECT frameRect{0, 0, pin.viewWidth + kFrame * 2, pin.viewHeight + kFrame * 2};
    HBRUSH cyan = CreateSolidBrush(kCyan);
    FillRect(dc, &frameRect, cyan);
    DeleteObject(cyan);
    RECT grayRect = frameRect;
    InflateRect(&grayRect, -1, -1);
    HBRUSH gray = CreateSolidBrush(kGray);
    FillRect(dc, &grayRect, gray);
    DeleteObject(gray);
    RECT innerCyan = grayRect;
    InflateRect(&innerCyan, -1, -1);
    HBRUSH cyan2 = CreateSolidBrush(kCyan);
    FillRect(dc, &innerCyan, cyan2);
    DeleteObject(cyan2);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, kFrame, kFrame, pin.viewWidth, pin.viewHeight, pin.document.image.dc, 0, 0,
               pin.document.image.width, pin.document.image.height, SRCCOPY);

    RECT pointerIcon{};
    {
        Graphics graphics(dc);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        Matrix transform(static_cast<REAL>(pin.scaleX), 0.0f, 0.0f, static_cast<REAL>(pin.scaleY),
                         static_cast<REAL>(kFrame), static_cast<REAL>(kFrame));
        graphics.SetTransform(&transform);
        graphics.SetClip(Rect(0, 0, pin.document.image.width, pin.document.image.height));
        pin.document.DrawShapes(graphics);

        if (pin.dragging && pin.preview) {
            if (pin.tool == Tool::Crop) {
                RECT selection = MakeRect(pin.start, pin.current);
                Pen pen(Color(255, 0, 170, 170), 1.0f);
                graphics.DrawRectangle(&pen, static_cast<REAL>(selection.left), static_cast<REAL>(selection.top),
                                       static_cast<REAL>(selection.right - selection.left),
                                       static_cast<REAL>(selection.bottom - selection.top));
            } else {
                Shape previewShape;
                previewShape.tool = pin.tool;
                previewShape.color = pin.color;
                previewShape.size = pin.ActiveSize();
                previewShape.from = pin.start;
                previewShape.to = pin.current;
                previewShape.rect = MakeRect(pin.start, pin.current);
                previewShape.points = pin.stroke;
                Document::DrawShape(graphics, previewShape);
            }
        }
        graphics.ResetClip();
        graphics.ResetTransform();

        // toolbar (fixed width, bottom-left)
        const int toolbarTop = pin.viewHeight + kFrame * 2;
        for (size_t i = 0; i < pin.items.size(); ++i) {
            const Item& item = pin.items[i];
            RECT box = item.rect;
            box.top += toolbarTop;
            box.bottom += toolbarTop;

            bool activeTool = (item.id >= kPointer && item.id <= kCrop) &&
                              (static_cast<int>(pin.tool) + kPointer == item.id);
            if (activeTool) {
                SolidBrush highlight(Color(255, 52, 52, 68));
                graphics.FillRectangle(&highlight, RectF(static_cast<REAL>(box.left), static_cast<REAL>(box.top),
                                                         static_cast<REAL>(box.right - box.left),
                                                         static_cast<REAL>(box.bottom - box.top)));
            }

            if (item.id >= kSwatchFirst) {
                int index = item.id - kSwatchFirst;
                RECT swatch = box;
                InflateRect(&swatch, -2, -2);
                COLORREF colour = kBasicColors[index];
                SolidBrush fill(Color(255, GetRValue(colour), GetGValue(colour), GetBValue(colour)));
                graphics.FillRectangle(&fill, RectF(static_cast<REAL>(swatch.left), static_cast<REAL>(swatch.top),
                                                    static_cast<REAL>(swatch.right - swatch.left),
                                                    static_cast<REAL>(swatch.bottom - swatch.top)));
                bool selected = pin.color == colour;
                Pen border(selected ? Color(255, 255, 255, 255) : Color(255, 90, 90, 110), selected ? 2.0f : 1.0f);
                graphics.DrawRectangle(&border, static_cast<REAL>(swatch.left), static_cast<REAL>(swatch.top),
                                       static_cast<REAL>(swatch.right - swatch.left - 1),
                                       static_cast<REAL>(swatch.bottom - swatch.top - 1));
                if (selected) {
                    SolidBrush dark(Color(255, 20, 20, 20));
                    DrawGlyphIcon(graphics, L'\xE73E', swatch, dark);
                }
                continue;
            }

            if (item.id == kPointer) {
                pointerIcon = box; // drawn with the real system cursor after this block
            } else if (item.id == kSizeValue) {
                RECT padded = box;
                padded.left -= pin.Scaled(3);
                padded.right += pin.Scaled(3);
                DrawIcon(graphics, item.id, padded, pin);
            } else {
                DrawIcon(graphics, item.id, box, pin);
            }

            if (!item.secondRow && i + 1 < pin.items.size() && !pin.items[i + 1].secondRow &&
                pin.items[i + 1].id == kUndo) {
                int dividerX = (item.rect.right + pin.items[i + 1].rect.left) / 2;
                Pen divider(Color(255, 92, 92, 112), 1.0f);
                graphics.DrawLine(&divider, static_cast<REAL>(dividerX),
                                  static_cast<REAL>(toolbarTop + pin.Scaled(4)),
                                  static_cast<REAL>(dividerX),
                                  static_cast<REAL>(toolbarTop + pin.Scaled(kItem) + pin.Scaled(kPad) - pin.Scaled(4)));
            }
        }

        // frame handles
        for (int handle = 1; handle <= 8; ++handle) {
            RECT box = HandleRect(pin, handle);
            SolidBrush fill(Color(255, 250, 250, 252));
            graphics.FillRectangle(&fill, RectF(static_cast<REAL>(box.left), static_cast<REAL>(box.top),
                                                static_cast<REAL>(box.right - box.left),
                                                static_cast<REAL>(box.bottom - box.top)));
            Pen outline(Color(255, 0, 0xAA, 0xAA), 1.0f);
            graphics.DrawRectangle(&outline, static_cast<REAL>(box.left), static_cast<REAL>(box.top),
                                   static_cast<REAL>(box.right - box.left - 1),
                                   static_cast<REAL>(box.bottom - box.top - 1));
        }

        // islands
        ARGB pinFill = pin.hoverIsland == 1 ? 0xC0000000 : kIslandFill;
        ARGB closeFill = pin.hoverIsland == 2 ? 0xC0000000 : kIslandFill;
        DrawRounded(graphics, pin.islandPin, 6.0f, pinFill, 0xFF5C5C74);
        DrawRounded(graphics, pin.islandClose, 6.0f, closeFill, 0xFF5C5C74);

        SolidBrush islandBrush(Color(255, 232, 232, 240));
        RECT pinIcon{pin.islandPin.left + 5, pin.islandPin.top + 3, pin.islandPin.left + 23, pin.islandPin.bottom - 3};
        DrawGlyphIcon(graphics, pin.alwaysOnTop ? L'\xE840' : L'\xE77A', pinIcon, islandBrush);
        {
            FontFamily family(L"Segoe UI");
            Font font(&family, 12.0f, FontStyleRegular, UnitPixel);
            std::wstring label = pin.alwaysOnTop ? L"Pin" : L"Unpin";
            StringFormat format;
            format.SetLineAlignment(StringAlignmentCenter);
            format.SetAlignment(StringAlignmentNear);
            RectF area(static_cast<REAL>(pin.islandPin.left + 24), static_cast<REAL>(pin.islandPin.top),
                       static_cast<REAL>(pin.islandPin.right - pin.islandPin.left - 26),
                       static_cast<REAL>(pin.islandPin.bottom - pin.islandPin.top));
            graphics.DrawString(label.c_str(), -1, &font, area, &format, &islandBrush);
        }
        DrawGlyphIcon(graphics, L'\xE8BB', pin.islandClose, islandBrush);

        // toast
        if (!pin.toast.empty()) {
            FontFamily family(L"Segoe UI");
            Font font(&family, 13.0f, FontStyleRegular, UnitPixel);
            RectF measure;
            graphics.MeasureString(pin.toast.c_str(), -1, &font, PointF(0, 0), &measure);
            int toastWidth = static_cast<int>(measure.Width) + 24;
            int toastHeight = static_cast<int>(measure.Height) + 12;
            RECT toast{kFrame + (pin.viewWidth - toastWidth) / 2, kFrame + 12,
                       kFrame + (pin.viewWidth + toastWidth) / 2, kFrame + 12 + toastHeight};
            DrawRounded(graphics, toast, 6.0f, 0xD0202028, 0xFF00AAAA);
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF area(static_cast<REAL>(toast.left), static_cast<REAL>(toast.top),
                       static_cast<REAL>(toast.right - toast.left), static_cast<REAL>(toast.bottom - toast.top));
            SolidBrush toastBrush(Color(255, 240, 240, 245));
            graphics.DrawString(pin.toast.c_str(), -1, &font, area, &format, &toastBrush);
        }

        graphics.Flush();
    }

    if (pointerIcon.right > pointerIcon.left) {
        int size = std::min(static_cast<int>(pointerIcon.right - pointerIcon.left),
                            static_cast<int>(pointerIcon.bottom - pointerIcon.top)) - 2;
        HICON arrow = PointerIcon(size);
        if (arrow != nullptr) {
            static RECT cachedBounds{};
            static int cachedFor = 0;
            if (cachedFor != size) {
                cachedBounds = PointerOpaqueBounds(arrow, size);
                cachedFor = size;
            }
            int boxWidth = pointerIcon.right - pointerIcon.left;
            int boxHeight = pointerIcon.bottom - pointerIcon.top;
            int glyphWidth = cachedBounds.right - cachedBounds.left;
            int glyphHeight = cachedBounds.bottom - cachedBounds.top;
            int x = pointerIcon.left + (boxWidth - glyphWidth) / 2 - cachedBounds.left;
            int y = pointerIcon.top + (boxHeight - glyphHeight) / 2 - cachedBounds.top;
            DrawIconEx(dc, x, y, arrow, size, size, 0, nullptr, DI_NORMAL);
        }
    }
}

void PaintPin(Pin& pin, HDC target) {
    RenderPin(pin);
    RECT client{};
    GetClientRect(pin.window, &client);
    if (pin.back.bitmap != nullptr) {
        BitBlt(target, 0, 0, client.right, client.bottom, pin.back.dc, 0, 0, SRCCOPY);
    }
}

// ---------------------------------------------------------------- actions

void CropToNewPin(Pin& pin, RECT rect) {
    Dib flattened = pin.document.Flatten();
    if (flattened.bitmap == nullptr) return;

    Document fresh;
    if (!fresh.CropFrom(flattened, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top)) return;

    RECT window{};
    GetWindowRect(pin.window, &window);
    RECT target{window.left + kFrame + static_cast<LONG>(rect.left * pin.scaleX),
                window.top + kFrame + static_cast<LONG>(rect.top * pin.scaleY),
                window.left + kFrame + static_cast<LONG>(rect.right * pin.scaleX),
                window.top + kFrame + static_cast<LONG>(rect.bottom * pin.scaleY)};

    Settings settings = pin.settings;
    settings.pinOffset = 0;
    settings.autoCopy = false; // the new pin stays off the clipboard until it is edited or copied
    CreatePin(std::move(fresh), settings, pin.paths, target);
}

void CommitShape(Pin& pin) {
    Shape shape;
    shape.tool = pin.tool;
    shape.color = pin.color;
    shape.size = pin.ActiveSize();

    switch (pin.tool) {
        case Tool::Box:
        case Tool::Ellipse:
        case Tool::Redact: {
            RECT rect = MakeRect(pin.start, pin.current);
            if (rect.right - rect.left < 4 && rect.bottom - rect.top < 4) return;
            shape.rect = rect;
            break;
        }
        case Tool::Arrow:
        case Tool::Highlight: {
            int dx = pin.current.x - pin.start.x;
            int dy = pin.current.y - pin.start.y;
            if (dx * dx + dy * dy < 16) return;
            shape.from = pin.start;
            shape.to = pin.current;
            shape.rect = MakeRect(pin.start, pin.current);
            break;
        }
        case Tool::Pen:
        case Tool::Marker: {
            if (pin.stroke.size() < 2) return;
            shape.points = pin.stroke;
            shape.rect = MakeRect(pin.stroke.front(), pin.stroke.back());
            break;
        }
        case Tool::Crop: {
            RECT rect = MakeRect(pin.start, pin.current);
            if (rect.right - rect.left < 16 || rect.bottom - rect.top < 16) return;
            CropToNewPin(pin, rect);
            return;
        }
        default:
            return;
    }

    pin.document.AddShape(shape);
    StartCopy(pin);
    InvalidateRect(pin.window, nullptr, FALSE);
}

void CycleColor(Pin& pin) {
    CHOOSECOLORW choose{};
    static COLORREF custom[16] = {};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = pin.window;
    choose.rgbResult = pin.color;
    choose.lpCustColors = custom;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&choose)) pin.color = choose.rgbResult;
    InvalidateRect(pin.window, nullptr, FALSE);
}

void HandleToolbarClick(Pin& pin, int id) {
    if (id >= kPointer && id <= kCrop) {
        pin.tool = static_cast<Tool>(id - kPointer);
        Layout(pin, false);
        return;
    }

    switch (id) {
        case kSizeMinus:
            pin.AdjustSize(-1);
            InvalidateRect(pin.window, nullptr, FALSE);
            break;
        case kSizePlus:
            pin.AdjustSize(1);
            InvalidateRect(pin.window, nullptr, FALSE);
            break;
        case kColor:
            CycleColor(pin);
            SyncHexEdit(pin);
            break;
        case kUndo:
            if (pin.document.Undo()) StartCopy(pin);
            InvalidateRect(pin.window, nullptr, FALSE);
            break;
        case kSave:
            SavePinAs(pin);
            break;
        case kCopy:
            StartCopy(pin);
            break;
        case kSettings: {
            HWND resident = FindWindowW(L"SniPin.Resident", nullptr);
            if (resident != nullptr) PostMessageW(resident, WM_APP + 3, 0, 0);
            else if (ShowSettingsDialog(pin.window, pin.settings)) SaveSettings(pin.paths, pin.settings);
            break;
        }
        default:
            break;
    }
}

void StartMove(Pin& pin, HWND window, LPARAM lparam) {
    pin.moving = true;
    POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ClientToScreen(window, &cursor);
    pin.moveCursor = cursor;
    GetWindowRect(window, &pin.moveRect);
    SetCapture(window);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
}

int HandleAt(const Pin& pin, POINT point) {
    for (int handle = 1; handle <= 3; ++handle) {
        RECT box = HandleRect(pin, handle);
        InflateRect(&box, 2, 2);
        if (PointIn(box, point)) return handle;
    }
    return 0;
}

// ---------------------------------------------------------------- window

LRESULT CALLBACK PinProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* pin = reinterpret_cast<Pin*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (pin == nullptr) break;
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintPin(*pin, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SETCURSOR: {
            if (pin == nullptr) break;
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            if (PointIn(pin->islandPin, cursor) || PointIn(pin->islandClose, cursor)) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            int handle = HandleAt(*pin, cursor);
            if (handle != 0) {
                static const LPCTSTR cursors[3] = {IDC_SIZENS, IDC_SIZENWSE, IDC_SIZEWE};
                SetCursor(LoadCursorW(nullptr, cursors[handle - 1]));
                return TRUE;
            }
            if (PointIn(pin->ImageRect(), cursor)) {
                SetCursor(LoadCursorW(nullptr, pin->tool == Tool::Pointer ? IDC_SIZEALL : IDC_CROSS));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            if (pin == nullptr) return 0;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT client{};
            GetClientRect(window, &client);

            if (PointIn(pin->islandPin, point)) {
                pin->alwaysOnTop = !pin->alwaysOnTop;
                SetWindowPos(window, pin->alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                pin->secondRow = pin->ToolUsesStyle();
                ApplyGeometry(*pin, false);
                return 0;
            }
            if (PointIn(pin->islandClose, point)) {
                DestroyWindow(window);
                return 0;
            }

            for (const Item& item : pin->items) {
                RECT hit = item.rect;
                hit.top += pin->viewHeight + kFrame * 2;
                hit.bottom += pin->viewHeight + kFrame * 2;
                if (PointIn(hit, point)) {
                    if (item.id >= kSwatchFirst) {
                        pin->color = kBasicColors[item.id - kSwatchFirst];
                        SyncHexEdit(*pin);
                        InvalidateRect(window, nullptr, FALSE);
                    } else if (item.id != kSizeValue) {
                        HandleToolbarClick(*pin, item.id);
                    }
                    return 0;
                }
            }

            int handle = HandleAt(*pin, point);
            if (handle != 0) {
                pin->resizing = handle;
                pin->start = point;
                POINT startScreen = point;
                ClientToScreen(window, &startScreen);
                pin->resizeStart = startScreen;
                GetWindowRect(window, &pin->resizeWinRect);
                RECT image = pin->ImageRect();
                POINT origin{image.left, image.top};
                ClientToScreen(window, &origin);
                pin->resizeImgRect = RECT{origin.x, origin.y, origin.x + pin->viewWidth,
                                          origin.y + pin->viewHeight};
                SetCapture(window);
                return 0;
            }

            int toolbarTop = pin->viewHeight + kFrame * 2;
            if (point.y >= toolbarTop) {
                StartMove(*pin, window, lparam);
                return 0;
            }

            if (pin->tool == Tool::Pointer) {
                StartMove(*pin, window, lparam);
                return 0;
            }

            if (!PointIn(pin->ImageRect(), point)) return 0;

            if (pin->tool == Tool::Text) {
                StartText(*pin, point);
                return 0;
            }

            pin->dragging = true;
            pin->preview = false;
            pin->start = pin->current = ToImage(*pin, point);
            pin->stroke.clear();
            pin->stroke.push_back(pin->start);
            SetCapture(window);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (pin == nullptr) return 0;

            if (pin->moving) {
                POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window, &cursor);
                SetWindowPos(window, nullptr,
                             pin->moveRect.left + (cursor.x - pin->moveCursor.x),
                             pin->moveRect.top + (cursor.y - pin->moveCursor.y),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            }

            if (pin->resizing != 0) {
                POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window, &cursor);
                int dx = cursor.x - pin->resizeStart.x;
                int dy = cursor.y - pin->resizeStart.y;

                RECT image = pin->resizeImgRect;
                int handle = pin->resizing;
                int minimum = pin->Scaled(kMinView);

                if (handle == 3) {          // right: width only
                    image.right = std::max<LONG>(image.left + minimum, image.right + dx);
                    pin->scaleX = static_cast<double>(image.right - image.left) / pin->document.image.width;
                    pin->viewWidth = image.right - image.left;
                    pin->viewHeight = static_cast<int>(pin->document.image.height * pin->scaleY);
                } else if (handle == 1) {   // bottom: height only
                    image.bottom = std::max<LONG>(image.top + minimum, image.bottom + dy);
                    pin->scaleY = static_cast<double>(image.bottom - image.top) / pin->document.image.height;
                    pin->viewHeight = image.bottom - image.top;
                    pin->viewWidth = static_cast<int>(pin->document.image.width * pin->scaleX);
                } else {                     // corner: keep the aspect ratio
                    int wantedWidth = std::max<int>(minimum, static_cast<int>(image.right) + dx - static_cast<int>(image.left));
                    int wantedHeight = std::max<int>(minimum, static_cast<int>(image.bottom) + dy - static_cast<int>(image.top));
                    double byWidth = static_cast<double>(wantedWidth) / pin->document.image.width;
                    double byHeight = static_cast<double>(wantedHeight) / pin->document.image.height;
                    double scale = std::max(byWidth, byHeight);
                    pin->scaleX = scale;
                    pin->scaleY = scale;
                    pin->viewWidth = std::max(1, static_cast<int>(pin->document.image.width * scale));
                    pin->viewHeight = std::max(1, static_cast<int>(pin->document.image.height * scale));
                }

                pin->secondRow = pin->ToolUsesStyle();
                BuildItems(*pin, std::max(pin->toolbarWidth, pin->viewWidth + kFrame * 2));
                int clientWidth = std::max(pin->toolbarWidth, pin->viewWidth + kFrame * 2);
                int clientHeight = pin->viewHeight + kFrame * 2 + pin->toolbarHeight;
                SetWindowPos(window, nullptr, image.left - kFrame, image.top - kFrame, clientWidth, clientHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                UpdateIslands(*pin);
                PositionHexEdit(*pin);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                int hover = PointIn(pin->islandPin, point) ? 1 : (PointIn(pin->islandClose, point) ? 2 : 0);
                if (hover != pin->hoverIsland) {
                    pin->hoverIsland = hover;
                    InvalidateRect(window, nullptr, FALSE);
                }
            }

            if (pin->dragging) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                pin->current = ToImage(*pin, point);
                if (pin->tool == Tool::Pen || pin->tool == Tool::Marker) {
                    if (pin->stroke.empty() || pin->stroke.back().x != pin->current.x ||
                        pin->stroke.back().y != pin->current.y) {
                        pin->stroke.push_back(pin->current);
                    }
                }
                pin->preview = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (pin == nullptr) return 0;
            if (pin->moving) {
                pin->moving = false;
                ReleaseCapture();
                return 0;
            }
            if (pin->resizing != 0) {
                pin->resizing = 0;
                ReleaseCapture();
                return 0;
            }
            if (!pin->dragging) return 0;
            ReleaseCapture();
            pin->dragging = false;
            pin->preview = false;
            CommitShape(*pin);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (pin == nullptr) return 0;
            if (wparam == VK_ESCAPE) {
                if (pin->editor != nullptr) { FinishText(*pin, false); return 0; }
                if (pin->tool != Tool::Pointer) {
                    pin->tool = Tool::Pointer;
                    Layout(*pin, false);
                }
                return 0;
            }
            if (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN) {
                const int step = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10 : 1;
                RECT rect{};
                GetWindowRect(window, &rect);
                int dx = wparam == VK_LEFT ? -step : (wparam == VK_RIGHT ? step : 0);
                int dy = wparam == VK_UP ? -step : (wparam == VK_DOWN ? step : 0);
                SetWindowPos(window, nullptr, rect.left + dx, rect.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                if (wparam == 'C') { StartCopy(*pin); return 0; }
                if (wparam == 'S') { SavePinAs(*pin); return 0; }
                if (wparam == 'Z') {
                    if (pin->document.Undo()) StartCopy(*pin);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (pin != nullptr && wparam == kToastTimer) {
                if (GetTickCount64() > pin->toastUntil) {
                    KillTimer(window, kToastTimer);
                    pin->toast.clear();
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case kCopyDone:
            if (pin != nullptr) CopyFinished(*pin, wparam != 0);
            return 0;
        case WM_CTLCOLOREDIT: {
            if (pin == nullptr) break;
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(240, 240, 245));
            SetBkColor(dc, RGB(24, 24, 28));
            static HBRUSH editBrush = CreateSolidBrush(RGB(24, 24, 28));
            return reinterpret_cast<LRESULT>(editBrush);
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY: {
            if (pin != nullptr) {
                if (pin->editor != nullptr) {
                    HWND editor = pin->editor;
                    pin->editor = nullptr;
                    DestroyWindow(editor);
                }
                if (pin->hexEditor != nullptr) {
                    HWND editor = pin->hexEditor;
                    pin->hexEditor = nullptr;
                    DestroyWindow(editor);
                }
                g_pins.erase(std::remove(g_pins.begin(), g_pins.end(), pin), g_pins.end());
                if (g_pins.empty()) PostQuitMessage(0);
                delete pin;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool CreatePin(Document&& document, const Settings& settings, const Paths& paths, const RECT& screenRect) {
    auto* pin = new Pin();
    pin->document = std::move(document);
    pin->settings = settings;
    pin->paths = paths;
    pin->penWidth = std::clamp(settings.penWidth, 1, 24);
    pin->markerSize = std::clamp(settings.markerSize, 2, 60);
    pin->textSize = std::clamp(settings.textSize, 8, 72);

    HMONITOR targetMonitor = MonitorFromRect(&screenRect, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(targetMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) pin->dpi = dpiX;
    if (DpiOverride() > 0) pin->dpi = DpiOverride();

    pin->viewWidth = pin->document.image.width;
    pin->viewHeight = pin->document.image.height;
    pin->scaleX = 1.0;
    pin->scaleY = 1.0;
    pin->secondRow = pin->ToolUsesStyle();

    BuildItems(*pin, pin->viewWidth + kFrame * 2);
    int clientWidth = std::max(pin->toolbarWidth, pin->viewWidth + kFrame * 2);
    int clientHeight = pin->viewHeight + kFrame * 2 + pin->toolbarHeight;

    int x = screenRect.left + std::clamp(settings.pinOffset, 0, 200);
    int y = screenRect.top + std::clamp(settings.pinOffset, 0, 200);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(targetMonitor, &monitor)) {
        if (x + clientWidth > monitor.rcWork.right) x = monitor.rcWork.right - clientWidth;
        if (y + clientHeight > monitor.rcWork.bottom) y = monitor.rcWork.bottom - clientHeight;
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = PinProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"SniPin.Pin";
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogFmt("pin: RegisterClassExW failed %lu", GetLastError());
        delete pin;
        return false;
    }

    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SniPin.Pin", L"SniPin",
                                  WS_POPUP, x, y, clientWidth, clientHeight, nullptr, nullptr,
                                  windowClass.hInstance, pin);
    if (window == nullptr) {
        LogFmt("pin: CreateWindowExW failed %lu", GetLastError());
        delete pin;
        return false;
    }

    pin->window = window;
    UpdateIslands(*pin);

    HWND hex = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               0, 0, 10, 10, window, reinterpret_cast<HMENU>(2), GetModuleHandleW(nullptr),
                               nullptr);
    if (hex != nullptr) {
        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SendMessageW(hex, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        pin->hexEditor = hex;
        pin->hexProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hex, GWLP_WNDPROC,
                                                                   reinterpret_cast<LONG_PTR>(HexProc)));
        SetWindowLongPtrW(hex, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pin));
        SyncHexEdit(*pin);
    }
    PositionHexEdit(*pin);

    g_pins.push_back(pin);

    ShowWindow(window, SW_SHOW);
    ForceForeground(window);
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);

    LogFmt("pin: created %dx%d at %d,%d client=%dx%d", pin->viewWidth, pin->viewHeight, x, y,
           clientWidth, clientHeight);

    if (settings.autoCopy) StartCopy(*pin);
    return true;
}

int PinCount() {
    return static_cast<int>(g_pins.size());
}

} // namespace snip
