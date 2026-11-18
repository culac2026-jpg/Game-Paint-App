#pragma once

#include <algorithm>
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

// Integer pixel rectangle [x, x + w) x [y, y + h).
struct IRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }  // exclusive
    int bottom() const { return y + h; } // exclusive
    bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < right() && p.y < bottom(); }

    IRect intersected(const IRect& o) const {
        const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return x1 > x0 && y1 > y0 ? IRect{x0, y0, x1 - x0, y1 - y0} : IRect{};
    }
    IRect united(const IRect& o) const {
        if (empty()) {
            return o;
        }
        if (o.empty()) {
            return *this;
        }
        const int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        return {x0, y0, std::max(right(), o.right()) - x0, std::max(bottom(), o.bottom()) - y0};
    }
    friend bool operator==(const IRect&, const IRect&) = default;
};

struct RectF {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

// 2D affine map:  x' = a*x + c*y + tx,  y' = b*x + d*y + ty.
struct Affine {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    static Affine translate(double dx, double dy) { return {1, 0, 0, 1, dx, dy}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotate(double radians) {
        const double cs = std::cos(radians), sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0, 0};
    }
    // `m` applied around `pivot` instead of the origin.
    static Affine around(PointF pivot, const Affine& m) {
        return translate(pivot.x, pivot.y) * m * translate(-pivot.x, -pivot.y);
    }

    PointF map(PointF p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    double determinant() const { return a * d - b * c; }
    bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }

    Affine inverted() const {
        const double det = determinant();
        const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }

    // (A * B).map(p) == A.map(B.map(p))
    friend Affine operator*(const Affine& m, const Affine& n) {
        return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
                m.b * n.c + m.d * n.d, m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty};
    }
};

// Axis-aligned pixel bounds covering the image of `r` under `m`.
inline IRect mappedBounds(const Affine& m, const IRect& r) {
    if (r.empty()) {
        return {};
    }
    const PointF corners[4] = {m.map({double(r.x), double(r.y)}), m.map({double(r.right()), double(r.y)}),
                               m.map({double(r.x), double(r.bottom())}),
                               m.map({double(r.right()), double(r.bottom())})};
    double x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
    for (const PointF& p : corners) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    // Small epsilon so exact integer edges don't grow by a pixel from rounding noise.
    const int ix0 = static_cast<int>(std::floor(x0 + 1e-9)), iy0 = static_cast<int>(std::floor(y0 + 1e-9));
    const int ix1 = static_cast<int>(std::ceil(x1 - 1e-9)), iy1 = static_cast<int>(std::ceil(y1 - 1e-9));
    return {ix0, iy0, ix1 - ix0, iy1 - iy0};
}

enum class Interpolation { Nearest, Bilinear };

// `to` rotated about `from` to the nearest multiple of `stepDegrees` (same
// length), for straight lines snapped to 0/15/30/45/... degrees.
inline PointF snapAngle(PointF from, PointF to, double stepDegrees) {
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double len = std::hypot(dx, dy);
    if (len <= 0.0 || stepDegrees <= 0.0) {
        return to;
    }
    const double step = stepDegrees * 3.14159265358979323846 / 180.0;
    const double angle = std::round(std::atan2(dy, dx) / step) * step;
    double cs = std::cos(angle), sn = std::sin(angle);
    // Exact axes, so horizontal/vertical lines stay on one pixel row/column.
    if (std::abs(cs) < 1e-12) cs = 0.0;
    if (std::abs(sn) < 1e-12) sn = 0.0;
    return {from.x + len * cs, from.y + len * sn};
}

} // namespace gas
