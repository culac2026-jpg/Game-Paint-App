#pragma once

#include <cmath>

namespace gas {

struct Point {
    int x = 0;
    int y = 0;
    friend bool operator==(const Point&, const Point&) = default;
};

struct PointF {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const PointF&, const PointF&) = default;
};

// Pixel that contains a document-space position (floor, not round).
inline Point toPixel(PointF p) {
    return {static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))};
}

} // namespace gas
