#include "gas/core/Pencil.h"

#include "gas/core/TiledImage.h"

#include <algorithm>
#include <cstdlib>

namespace gas {

namespace {

// Bresenham from a to b inclusive, calling fn for each pixel.
template <typename Fn>
void forEachLinePixel(Point a, Point b, Fn&& fn) {
    const int dx = std::abs(b.x - a.x);
    const int dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1;
    const int sy = a.y < b.y ? 1 : -1;
    int err = dx + dy;
    Point p = a;
    for (;;) {
        fn(p);
        if (p == b) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            p.x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            p.y += sy;
        }
    }
}

} // namespace

void drawLine(TiledImage& image, Point a, Point b, Rgba8 color) {
    forEachLinePixel(a, b, [&](Point p) { image.setPixel(p, color); });
}

// Pixel p and its mirror copies (pixel centres mapped, then floored).
void PencilTool::plot(TiledImage& image, Point p) {
    const PointF center = symmetry().center;
    for (const SymmetryCopy& copy : copies_) {
        image.setPixel(toPixel(copy.map({p.x + 0.5, p.y + 0.5}, center)), color_);
    }
}

void PencilTool::begin(TiledImage& image, PointF pos, double /*pressure*/) {
    copies_ = symmetry().copies();
    gridAligned_ = std::all_of(copies_.begin(), copies_.end(), [](const SymmetryCopy& c) { return c.gridAligned(); });
    const Point p = toPixel(pos);
    plot(image, p);
    last_ = p;
}

void PencilTool::moveTo(TiledImage& image, PointF pos, double /*pressure*/) {
    if (!last_) {
        begin(image, pos);
        return;
    }
    const Point p = toPixel(pos);
    if (p == *last_) {
        return;
    }
    if (gridAligned_) {
        // Mirror pixel by pixel, so the copies are exact mirror images.
        forEachLinePixel(*last_, p, [&](Point q) { plot(image, q); });
    } else {
        // Rotated copies: draw each as its own connected line (no gaps).
        const PointF center = symmetry().center;
        for (const SymmetryCopy& copy : copies_) {
            const auto mapped = [&](Point q) { return toPixel(copy.map({q.x + 0.5, q.y + 0.5}, center)); };
            drawLine(image, mapped(*last_), mapped(p), color_);
        }
    }
    last_ = p;
}

void PencilTool::end() { last_.reset(); }

} // namespace gas
