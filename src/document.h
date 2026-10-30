#pragma once

#include "capture.h"

#include <string>
#include <vector>

namespace Gdiplus { class Graphics; }

namespace snip {

enum class Tool {
    Pointer = 0,
    Box,
    Ellipse,
    Arrow,
    Highlight,
    Pen,
    Marker,
    Redact,
    Text,
    Crop,
};

struct Shape {
    Tool tool = Tool::Box;
    RECT rect{};               // bounding box (rect tools)
    POINT from{};              // line tools: tail
    POINT to{};                // line tools: head
    std::vector<POINT> points; // freehand strokes
    std::wstring text;
    int size = 3;
    COLORREF color = RGB(235, 70, 70);
};

class Document {
public:
    Dib image;
    std::vector<Shape> shapes;

    bool CropFrom(const Dib& source, int x, int y, int width, int height);
    bool CropTo(int x, int y, int width, int height);
    void AddShape(const Shape& shape);
    bool Undo();
    bool HasShapes() const { return !shapes.empty(); }

    /// Copy of the image with every shape painted on top.
    Dib Flatten() const;

    void DrawShapes(Gdiplus::Graphics& graphics) const;
    static void DrawShape(Gdiplus::Graphics& graphics, const Shape& shape);
};

} // namespace snip
