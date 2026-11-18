#include "gas/core/Pencil.h"

#include "gas/core/TiledImage.h"

#include <cstdlib>

namespace gas {

void drawLine(TiledImage& image, Point a, Point b, Rgba8 color) {
    const int dx = std::abs(b.x - a.x);
    const int dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1;
    const int sy = a.y < b.y ? 1 : -1;
    int err = dx + dy;
    Point p = a;
    for (;;) {
        image.setPixel(p, color);
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

void PencilTool::begin(TiledImage& image, Point p) {
    image.setPixel(p, color_);
    last_ = p;
}

void PencilTool::moveTo(TiledImage& image, Point p) {
    if (!last_) {
        begin(image, p);
        return;
    }
    if (p == *last_) {
        return;
    }
    drawLine(image, *last_, p, color_);
    last_ = p;
}

void PencilTool::end() { last_.reset(); }

} // namespace gas
