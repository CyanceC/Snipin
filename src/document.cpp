#include "document.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Gdiplus;

namespace snip {
namespace {

PointF ArrowHead(PointF from, PointF to, float angleOffset, float length) {
    float angle = std::atan2(to.Y - from.Y, to.X - from.X) + angleOffset;
    return PointF(to.X + length * std::cos(angle), to.Y + length * std::sin(angle));
}

std::vector<Point> ToPoints(const std::vector<POINT>& source) {
    std::vector<Point> points;
    points.reserve(source.size());
    for (const POINT& point : source) points.emplace_back(point.x, point.y);
    return points;
}

} // namespace

bool Document::CropFrom(const Dib& source, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (x < 0 || y < 0 || x + width > source.width || y + height > source.height) return false;

    if (!image.Create(source.dc, width, height)) return false;
    for (int row = 0; row < height; ++row) {
        const uint8_t* from = source.Row(y + row) + static_cast<size_t>(x) * 4;
        uint8_t* to = static_cast<uint8_t*>(image.pixels) + static_cast<size_t>(row) * image.stride;
        memcpy(to, from, static_cast<size_t>(width) * 4);
    }
    shapes.clear();
    return true;
}

bool Document::CropTo(int x, int y, int width, int height) {
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > image.width) width = image.width - x;
    if (y + height > image.height) height = image.height - y;
    if (width < 8 || height < 8) return false;

    Dib cropped;
    if (!cropped.Create(image.dc, width, height)) return false;
    for (int row = 0; row < height; ++row) {
        const uint8_t* from = image.Row(y + row) + static_cast<size_t>(x) * 4;
        uint8_t* to = static_cast<uint8_t*>(cropped.pixels) + static_cast<size_t>(row) * cropped.stride;
        memcpy(to, from, static_cast<size_t>(width) * 4);
    }

    image = std::move(cropped);
    shapes.clear(); // annotations were positioned against the old frame
    return true;
}

void Document::AddShape(const Shape& shape) {
    shapes.push_back(shape);
}

bool Document::Undo() {
    if (shapes.empty()) return false;
    shapes.pop_back();
    return true;
}

void Document::DrawShape(Graphics& graphics, const Shape& shape) {
    const RectF rect(static_cast<REAL>(shape.rect.left), static_cast<REAL>(shape.rect.top),
                     static_cast<REAL>(shape.rect.right - shape.rect.left),
                     static_cast<REAL>(shape.rect.bottom - shape.rect.top));

    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const BYTE r = GetRValue(shape.color);
    const BYTE g = GetGValue(shape.color);
    const BYTE b = GetBValue(shape.color);

    switch (shape.tool) {
        case Tool::Box: {
            Pen pen(Color(255, r, g, b), static_cast<REAL>(std::max(1, shape.size)));
            graphics.DrawRectangle(&pen, rect);
            break;
        }
        case Tool::Ellipse: {
            Pen pen(Color(255, r, g, b), static_cast<REAL>(std::max(1, shape.size)));
            graphics.DrawEllipse(&pen, rect);
            break;
        }
        case Tool::Arrow: {
            const PointF from(static_cast<REAL>(shape.from.x), static_cast<REAL>(shape.from.y));
            const PointF to(static_cast<REAL>(shape.to.x), static_cast<REAL>(shape.to.y));
            Pen pen(Color(255, r, g, b), static_cast<REAL>(std::max(1, shape.size)));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            graphics.DrawLine(&pen, from, to);

            const float head = 8.0f + shape.size * 1.8f;
            PointF points[3] = {
                to,
                ArrowHead(from, to, 2.55f, head),
                ArrowHead(from, to, -2.55f, head),
            };
            SolidBrush brush(Color(255, r, g, b));
            graphics.FillPolygon(&brush, points, 3);
            break;
        }
        case Tool::Highlight: {
            Pen pen(Color(105, r, g, b), static_cast<REAL>(std::max(2, shape.size)));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            graphics.DrawLine(&pen, static_cast<REAL>(shape.from.x), static_cast<REAL>(shape.from.y),
                              static_cast<REAL>(shape.to.x), static_cast<REAL>(shape.to.y));
            break;
        }
        case Tool::Marker: {
            if (shape.points.size() < 2) break;
            Pen pen(Color(105, r, g, b), static_cast<REAL>(std::max(4, shape.size)));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            pen.SetLineJoin(LineJoinRound);
            const std::vector<Point> points = ToPoints(shape.points);
            graphics.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
            break;
        }
        case Tool::Pen: {
            if (shape.points.size() < 2) break;
            Pen pen(Color(255, r, g, b), static_cast<REAL>(std::max(1, shape.size)));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            pen.SetLineJoin(LineJoinRound);
            const std::vector<Point> points = ToPoints(shape.points);
            graphics.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
            break;
        }
        case Tool::Redact: {
            SolidBrush brush(Color(255, 0, 0, 0));
            graphics.FillRectangle(&brush, rect);
            break;
        }
        case Tool::Text: {
            if (shape.text.empty()) break;
            FontFamily family(L"Segoe UI");
            Font font(&family, static_cast<REAL>(std::max(8, shape.size)), FontStyleRegular, UnitPixel);
            if (font.GetLastStatus() != Ok) break;
            SolidBrush shadow(Color(150, 0, 0, 0));
            graphics.DrawString(shape.text.c_str(), -1, &font, PointF(rect.X + 2.0f, rect.Y + 2.0f), &shadow);
            SolidBrush brush(Color(255, r, g, b));
            graphics.DrawString(shape.text.c_str(), -1, &font, PointF(rect.X, rect.Y), &brush);
            break;
        }
        case Tool::Crop:
        case Tool::Pointer:
            break;
    }
}

void Document::DrawShapes(Graphics& graphics) const {
    for (const Shape& shape : shapes) DrawShape(graphics, shape);
}

Dib Document::Flatten() const {
    Dib result;
    if (!result.Create(image.dc, image.width, image.height)) return result;

    for (int row = 0; row < image.height; ++row) {
        memcpy(static_cast<uint8_t*>(result.pixels) + static_cast<size_t>(row) * result.stride,
               image.Row(row), static_cast<size_t>(image.width) * 4);
    }

    if (!shapes.empty()) {
        Graphics graphics(result.dc);
        graphics.SetCompositingMode(CompositingModeSourceOver);
        DrawShapes(graphics);
        graphics.Flush();
    }
    return result;
}

} // namespace snip
