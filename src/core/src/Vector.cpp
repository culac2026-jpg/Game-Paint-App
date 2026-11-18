#include "gas/core/Vector.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gas {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKappa = 0.5522847498307936; // cubic approximation of a quarter circle
constexpr int kTile = TiledImage::kTileSize;
constexpr int kSubRows = 5; // vertical anti-aliasing samples per pixel row

using Contour = std::vector<PointF>;

PointF operator+(PointF a, PointF b) { return {a.x + b.x, a.y + b.y}; }
PointF operator-(PointF a, PointF b) { return {a.x - b.x, a.y - b.y}; }
PointF operator*(PointF a, double s) { return {a.x * s, a.y * s}; }
double dot(PointF a, PointF b) { return a.x * b.x + a.y * b.y; }
double cross(PointF a, PointF b) { return a.x * b.y - a.y * b.x; }
double length(PointF a) { return std::hypot(a.x, a.y); }

double distanceToSegment(PointF p, PointF a, PointF b) {
    const PointF ab = b - a;
    const double len2 = dot(ab, ab);
    const double t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return length(p - (a + ab * t));
}

double signedArea(const Contour& c) {
    double s = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        s += cross(c[i], c[(i + 1) % c.size()]);
    }
    return s / 2;
}

// Same orientation for every stroke piece, so nonzero filling unions them.
void addOriented(std::vector<Contour>& out, Contour c) {
    if (c.size() < 3) {
        return;
    }
    if (signedArea(c) < 0) {
        std::reverse(c.begin(), c.end());
    }
    out.push_back(std::move(c));
}

Contour disc(PointF centre, double r) {
    const int n = std::clamp(static_cast<int>(std::ceil(2 * kPi * r / 1.5)), 8, 96);
    Contour c;
    c.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n;
        c.push_back({centre.x + r * std::cos(a), centre.y + r * std::sin(a)});
    }
    return c;
}

void flattenCubic(std::vector<PointF>& out, PointF p0, PointF c1, PointF c2, PointF p3, double tol, int depth) {
    const PointF chord = p3 - p0;
    const double len = length(chord);
    double d1, d2;
    if (len < 1e-12) {
        d1 = length(c1 - p0);
        d2 = length(c2 - p0);
    } else {
        d1 = std::abs(cross(c1 - p0, chord)) / len;
        d2 = std::abs(cross(c2 - p0, chord)) / len;
    }
    if (depth >= 16 || std::max(d1, d2) <= tol) {
        out.push_back(p3);
        return;
    }
    // de Casteljau split at t = 0.5
    const PointF a = (p0 + c1) * 0.5, b = (c1 + c2) * 0.5, c = (c2 + p3) * 0.5;
    const PointF ab = (a + b) * 0.5, bc = (b + c) * 0.5, mid = (ab + bc) * 0.5;
    flattenCubic(out, p0, a, ab, mid, tol, depth + 1);
    flattenCubic(out, mid, bc, c, p3, tol, depth + 1);
}

// Pieces whose nonzero union is the stroke of polyline `pts`.
std::vector<Contour> strokeContours(std::vector<PointF> pts, bool closed, const VectorStyle& st) {
    std::vector<Contour> out;
    const double hw = st.strokeWidth / 2;
    if (hw <= 0) {
        return out;
    }
    // Drop repeated points (zero-length segments have no direction).
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](PointF a, PointF b) { return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9; }),
              pts.end());
    if (closed && pts.size() > 1 && length(pts.front() - pts.back()) < 1e-9) {
        pts.pop_back();
    }
    if (pts.size() == 1) {
        if (st.cap == LineCap::Round) {
            addOriented(out, disc(pts[0], hw));
        } else if (st.cap == LineCap::Square) {
            const PointF p = pts[0];
            addOriented(out, {{p.x - hw, p.y - hw}, {p.x + hw, p.y - hw}, {p.x + hw, p.y + hw}, {p.x - hw, p.y + hw}});
        }
        return out;
    }
    if (pts.size() < 2) {
        return out;
    }
    if (pts.size() == 2) {
        closed = false; // a closed two-point path is just a line
    }
    const std::size_t n = pts.size();
    const std::size_t segments = closed ? n : n - 1;
    const auto dir = [&](std::size_t i) { // unit direction of segment i
        const PointF d = pts[(i + 1) % n] - pts[i];
        return d * (1.0 / length(d));
    };
    for (std::size_t i = 0; i < segments; ++i) {
        const PointF a = pts[i], b = pts[(i + 1) % n], d = dir(i);
        const PointF nrm{-d.y * hw, d.x * hw};
        addOriented(out, {a + nrm, b + nrm, b - nrm, a - nrm});
    }
    // Joins at interior vertices (every vertex of a closed path).
    for (std::size_t v = closed ? 0 : 1; v < (closed ? n : n - 1); ++v) {
        const std::size_t in = (v + segments - 1) % segments, outSeg = v % segments;
        const PointF d0 = dir(in), d1 = dir(outSeg), p = pts[v];
        const double turn = cross(d0, d1);
        if (std::abs(turn) < 1e-9 && dot(d0, d1) > 0) {
            continue; // straight on: the segment quads already meet
        }
        if (st.join == LineJoin::Round) {
            addOriented(out, disc(p, hw));
            continue;
        }
        const double side = turn > 0 ? -1.0 : 1.0; // outer side of the turn
        const PointF n0 = PointF{-d0.y, d0.x} * (hw * side), n1 = PointF{-d1.y, d1.x} * (hw * side);
        const PointF p0 = p + n0, p1 = p + n1;
        if (st.join == LineJoin::Miter) {
            const PointF sum = n0 + n1;
            const double sumLen = length(sum);
            if (sumLen > 1e-9) {
                const PointF m = sum * (1.0 / sumLen);
                const double miter = hw * hw / dot(m, n0); // distance from p to the tip
                if (miter / hw <= kMiterLimit) {
                    addOriented(out, {p, p0, p + m * miter, p1});
                    continue;
                }
            }
        }
        addOriented(out, {p, p0, p1}); // bevel (or a miter over the limit)
    }
    if (!closed) {
        const PointF a = pts.front(), b = pts.back();
        if (st.cap == LineCap::Round) {
            addOriented(out, disc(a, hw));
            addOriented(out, disc(b, hw));
        } else if (st.cap == LineCap::Square) {
            const PointF ds = dir(0) * hw, de = dir(n - 2) * hw;
            const PointF ns{-ds.y, ds.x}, ne{-de.y, de.x};
            addOriented(out, {a - ds + ns, a + ns, a - ns, a - ds - ns});
            addOriented(out, {b + ne, b + de + ne, b + de - ne, b - ne});
        }
    }
    return out;
}

struct Edge {
    double x0, y0, x1, y1; // y0 != y1
    double ya, yb;         // min / max y
    int dir;               // +1 downwards, -1 upwards
};

// Nonzero coverage of `contours`, row by row inside `clip`: calls
// sink(y, x0, coverage) with coverage[i] in [0, 1] for pixel x0 + i.
template <class Sink>
void scanContours(const std::vector<Contour>& contours, IRect clip, bool antialias, Sink&& sink) {
    std::vector<Edge> edges;
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (const Contour& c : contours) {
        if (c.size() < 3) {
            continue;
        }
        for (std::size_t i = 0; i < c.size(); ++i) {
            const PointF p = c[i], q = c[(i + 1) % c.size()];
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
            if (p.y == q.y || !std::isfinite(p.x + p.y + q.x + q.y)) {
                continue;
            }
            edges.push_back({p.x, p.y, q.x, q.y, std::min(p.y, q.y), std::max(p.y, q.y), q.y > p.y ? 1 : -1});
        }
    }
    if (edges.empty()) {
        return;
    }
    const int x0 = std::max(clip.x, static_cast<int>(std::floor(std::max(minX, -1e9))));
    const int x1 = std::min(clip.right(), static_cast<int>(std::ceil(std::min(maxX, 1e9))));
    const int y0 = std::max(clip.y, static_cast<int>(std::floor(std::max(minY, -1e9))));
    const int y1 = std::min(clip.bottom(), static_cast<int>(std::ceil(std::min(maxY, 1e9))));
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    const int spanW = x1 - x0;
    // Edges bucketed by the rows they cross.
    std::vector<std::vector<std::size_t>> rows(static_cast<std::size_t>(y1 - y0));
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const int ra = std::max(y0, static_cast<int>(std::floor(edges[i].ya)));
        const int rb = std::min(y1 - 1, static_cast<int>(std::ceil(edges[i].yb)) - 1);
        for (int r = ra; r <= rb; ++r) {
            rows[static_cast<std::size_t>(r - y0)].push_back(i);
        }
    }
    std::vector<double> acc(static_cast<std::size_t>(spanW));
    std::vector<std::pair<double, int>> crossings;
    const int samples = antialias ? kSubRows : 1;
    const double weight = 1.0 / samples;
    const auto addSpan = [&](double xa, double xb) {
        if (!antialias) { // pixels whose centre lies in [xa, xb)
            const int ia = std::max(0, static_cast<int>(std::ceil(xa - x0 - 0.5)));
            const int ib = std::min(spanW, static_cast<int>(std::ceil(xb - x0 - 0.5)));
            for (int i = ia; i < ib; ++i) {
                acc[static_cast<std::size_t>(i)] = 1.0;
            }
            return;
        }
        xa = std::clamp(xa - x0, 0.0, double(spanW));
        xb = std::clamp(xb - x0, 0.0, double(spanW));
        if (xb <= xa) {
            return;
        }
        const int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
        if (ia == ib) {
            acc[static_cast<std::size_t>(ia)] += (xb - xa) * weight;
            return;
        }
        acc[static_cast<std::size_t>(ia)] += (ia + 1 - xa) * weight;
        for (int i = ia + 1; i < ib; ++i) {
            acc[static_cast<std::size_t>(i)] += weight;
        }
        if (ib < spanW) {
            acc[static_cast<std::size_t>(ib)] += (xb - ib) * weight;
        }
    };
    for (int y = y0; y < y1; ++y) {
        const std::vector<std::size_t>& bucket = rows[static_cast<std::size_t>(y - y0)];
        if (bucket.empty()) {
            continue;
        }
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int sub = 0; sub < samples; ++sub) {
            const double sy = y + (sub + 0.5) / samples;
            crossings.clear();
            for (std::size_t i : bucket) {
                const Edge& e = edges[i];
                if (e.ya <= sy && sy < e.yb) {
                    crossings.push_back({e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
                }
            }
            std::sort(crossings.begin(), crossings.end());
            int winding = 0;
            double start = 0;
            for (const auto& [x, d] : crossings) {
                const int before = winding;
                winding += d;
                if (before == 0 && winding != 0) {
                    start = x;
                } else if (before != 0 && winding == 0) {
                    addSpan(start, x);
                }
            }
        }
        sink(y, x0, acc);
    }
}

void paintContours(TiledImage& dst, const std::vector<Contour>& contours, Rgba8 color, bool antialias) {
    if (color.a == 0) {
        return;
    }
    scanContours(contours, {0, 0, dst.width(), dst.height()}, antialias,
                 [&](int y, int x0, const std::vector<double>& cov) {
                     TiledImage::Tile* tile = nullptr;
                     int tileX = -1;
                     for (std::size_t i = 0; i < cov.size(); ++i) {
                         const double c = std::min(1.0, cov[i]);
                         if (c < 1.0 / 512) {
                             continue;
                         }
                         const int x = x0 + static_cast<int>(i);
                         if (x / kTile != tileX) {
                             tileX = x / kTile;
                             tile = &dst.editTile(tileX, y / kTile);
                         }
                         Rgba8& px = tile->pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))];
                         px = blendOver(px, color, c);
                     }
                 });
}

bool windingInside(const std::vector<PointF>& pts, PointF p) {
    int winding = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const PointF a = pts[i], b = pts[(i + 1) % pts.size()];
        if (a.y <= p.y) {
            if (b.y > p.y && cross(b - a, p - a) > 0) {
                ++winding;
            }
        } else if (b.y <= p.y && cross(b - a, p - a) < 0) {
            --winding;
        }
    }
    return winding != 0;
}

std::string num(double v) {
    v = std::round(v * 1000) / 1000;
    if (std::abs(v - std::round(v)) < 1e-9) {
        return std::to_string(static_cast<long long>(std::llround(v)));
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    std::replace(s.begin(), s.end(), ',', '.'); // comma-decimal C locales
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    return s;
}

std::string hexColour(Rgba8 c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

} // namespace

// ---------------------------------------------------------------- builders

VectorPath rectanglePath(RectF r, double radius) {
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    radius = std::clamp(radius, 0.0, std::min(r.w, r.h) / 2);
    const double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    VectorPath p;
    p.closed = true;
    if (radius <= 0) {
        p.nodes = {PathNode::corner({x0, y0}), PathNode::corner({x1, y0}), PathNode::corner({x1, y1}),
                   PathNode::corner({x0, y1})};
        return p;
    }
    const double k = radius * kKappa;
    const auto node = [](PointF at, PointF in, PointF out) { return PathNode{at, in, out}; };
    p.nodes = {
        node({x0 + radius, y0}, {x0 + radius - k, y0}, {x0 + radius, y0}),
        node({x1 - radius, y0}, {x1 - radius, y0}, {x1 - radius + k, y0}),
        node({x1, y0 + radius}, {x1, y0 + radius - k}, {x1, y0 + radius}),
        node({x1, y1 - radius}, {x1, y1 - radius}, {x1, y1 - radius + k}),
        node({x1 - radius, y1}, {x1 - radius + k, y1}, {x1 - radius, y1}),
        node({x0 + radius, y1}, {x0 + radius, y1}, {x0 + radius - k, y1}),
        node({x0, y1 - radius}, {x0, y1 - radius + k}, {x0, y1 - radius}),
        node({x0, y0 + radius}, {x0, y0 + radius}, {x0, y0 + radius - k}),
    };
    // Where two corners meet (radius = half the side) drop the zero-length edge's duplicate nodes.
    if (std::abs(r.w - 2 * radius) < 1e-9 || std::abs(r.h - 2 * radius) < 1e-9) {
        std::vector<PathNode> merged;
        for (const PathNode& n : p.nodes) {
            if (!merged.empty() && length(merged.back().p - n.p) < 1e-9) {
                merged.back().out = n.out;
            } else {
                merged.push_back(n);
            }
        }
        if (merged.size() > 1 && length(merged.back().p - merged.front().p) < 1e-9) {
            merged.front().in = merged.back().in;
            merged.pop_back();
        }
        p.nodes = std::move(merged);
    }
    return p;
}

VectorPath ellipsePath(RectF r) {
    const double rx = r.w / 2, ry = r.h / 2, cx = r.x + rx, cy = r.y + ry;
    const double kx = rx * kKappa, ky = ry * kKappa;
    VectorPath p;
    p.closed = true;
    p.nodes = {
        {{cx, cy - ry}, {cx - kx, cy - ry}, {cx + kx, cy - ry}},
        {{cx + rx, cy}, {cx + rx, cy - ky}, {cx + rx, cy + ky}},
        {{cx, cy + ry}, {cx + kx, cy + ry}, {cx - kx, cy + ry}},
        {{cx - rx, cy}, {cx - rx, cy + ky}, {cx - rx, cy - ky}},
    };
    return p;
}

VectorPath polygonPath(PointF centre, double radius, int sides, double innerRatio, double rotation) {
    sides = std::clamp(sides, 3, 64);
    const bool star = innerRatio < 1.0;
    const int count = star ? sides * 2 : sides;
    VectorPath p;
    p.closed = true;
    for (int i = 0; i < count; ++i) {
        const double a = -kPi / 2 + rotation + 2 * kPi * i / count;
        const double r = star && (i % 2) ? radius * std::max(0.0, innerRatio) : radius;
        double x = std::cos(a), y = std::sin(a);
        if (std::abs(x) < 1e-12) x = 0;
        if (std::abs(y) < 1e-12) y = 0;
        p.nodes.push_back(PathNode::corner({centre.x + r * x, centre.y + r * y}));
    }
    return p;
}

// ---------------------------------------------------------------- geometry

std::vector<PointF> flattenPath(const VectorPath& path, double tolerance) {
    std::vector<PointF> out;
    const std::size_t n = path.nodes.size();
    if (n == 0) {
        return out;
    }
    tolerance = std::max(tolerance, 0.01);
    out.push_back(path.nodes[0].p);
    const std::size_t segments = path.closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const PathNode& a = path.nodes[i];
        const PathNode& b = path.nodes[(i + 1) % n];
        if (a.out == a.p && b.in == b.p) {
            out.push_back(b.p);
        } else {
            flattenCubic(out, a.p, a.out, b.in, b.p, tolerance, 0);
        }
    }
    if (path.closed && out.size() > 1) {
        out.pop_back(); // the closing segment ends where the path began
    }
    return out;
}

RectF pathControlBounds(const VectorPath& path) {
    if (path.nodes.empty()) {
        return {};
    }
    double x0 = path.nodes[0].p.x, x1 = x0, y0 = path.nodes[0].p.y, y1 = y0;
    for (const PathNode& n : path.nodes) {
        for (PointF q : {n.p, n.in, n.out}) {
            x0 = std::min(x0, q.x);
            x1 = std::max(x1, q.x);
            y0 = std::min(y0, q.y);
            y1 = std::max(y1, q.y);
        }
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

IRect shapeBounds(const VectorShape& shape) {
    if (shape.path.nodes.empty()) {
        return {};
    }
    const RectF b = pathControlBounds(shape.path);
    double ext = 1.0;
    if (shape.style.stroke) {
        const double hw = shape.style.strokeWidth / 2;
        double reach = shape.style.join == LineJoin::Miter ? hw * kMiterLimit : hw;
        if (shape.style.cap == LineCap::Square) {
            reach = std::max(reach, hw * std::sqrt(2.0));
        }
        ext += reach;
    }
    const int x0 = static_cast<int>(std::floor(b.x - ext)), y0 = static_cast<int>(std::floor(b.y - ext));
    const int x1 = static_cast<int>(std::ceil(b.x + b.w + ext)), y1 = static_cast<int>(std::ceil(b.y + b.h + ext));
    return {x0, y0, x1 - x0, y1 - y0};
}

VectorShape transformedShape(const VectorShape& shape, const Affine& m) {
    VectorShape out = shape;
    for (PathNode& n : out.path.nodes) {
        n.p = m.map(n.p);
        n.in = m.map(n.in);
        n.out = m.map(n.out);
    }
    out.style.strokeWidth = shape.style.strokeWidth * std::sqrt(std::abs(m.determinant()));
    return out;
}

VectorShapes transformedShapes(const VectorShapes& shapes, const Affine& m) {
    VectorShapes out;
    out.reserve(shapes.size());
    for (const VectorShape& s : shapes) {
        out.push_back(transformedShape(s, m));
    }
    return out;
}

bool hitShape(const VectorShape& shape, PointF p, double tolerance) {
    const std::vector<PointF> pts = flattenPath(shape.path);
    if (pts.empty()) {
        return false;
    }
    if (shape.style.fill && pts.size() >= 3 && windingInside(pts, p)) {
        return true;
    }
    double d = pts.size() == 1 ? length(p - pts[0]) : 1e300;
    const bool closed = shape.path.closed || shape.style.fill;
    const std::size_t segments = closed ? pts.size() : pts.size() - 1;
    for (std::size_t i = 0; i < segments && pts.size() > 1; ++i) {
        d = std::min(d, distanceToSegment(p, pts[i], pts[(i + 1) % pts.size()]));
    }
    const double reach = shape.style.stroke ? std::max(shape.style.strokeWidth / 2, tolerance) : tolerance;
    return d <= reach;
}

int hitTestShapes(const VectorShapes& shapes, PointF p, double tolerance) {
    for (int i = static_cast<int>(shapes.size()) - 1; i >= 0; --i) {
        if (hitShape(shapes[static_cast<std::size_t>(i)], p, tolerance)) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- rendering

void renderShapes(TiledImage& dst, const VectorShapes& shapes) {
    for (const VectorShape& s : shapes) {
        const std::vector<PointF> pts = flattenPath(s.path);
        if (pts.empty()) {
            continue;
        }
        if (s.style.fill && pts.size() >= 3) {
            paintContours(dst, {pts}, s.style.fillColor, s.style.antialias);
        }
        if (s.style.stroke) {
            paintContours(dst, strokeContours(pts, s.path.closed, s.style), s.style.strokeColor, s.style.antialias);
        }
    }
}

TiledImage rasterizeShapes(const VectorShapes& shapes, int width, int height) {
    TiledImage img(width, height);
    renderShapes(img, shapes);
    img.takeDirtyTiles();
    return img;
}

// ---------------------------------------------------------------- SVG

std::string svgPathData(const VectorPath& path) {
    std::string d;
    const std::size_t n = path.nodes.size();
    if (n == 0) {
        return d;
    }
    const auto pt = [](PointF p) { return num(p.x) + " " + num(p.y); };
    d = "M" + pt(path.nodes[0].p);
    const std::size_t segments = path.closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const PathNode& a = path.nodes[i];
        const PathNode& b = path.nodes[(i + 1) % n];
        if (a.out == a.p && b.in == b.p) {
            if (path.closed && i + 1 == n) {
                break; // Z draws the closing line
            }
            d += " L" + pt(b.p);
        } else {
            d += " C" + pt(a.out) + " " + pt(b.in) + " " + pt(b.p);
        }
    }
    if (path.closed) {
        d += " Z";
    }
    return d;
}

std::string svgPathElement(const VectorShape& shape) {
    const VectorStyle& st = shape.style;
    std::string e = "<path d=\"" + svgPathData(shape.path) + "\"";
    if (st.fill && st.fillColor.a > 0) {
        e += " fill=\"" + hexColour(st.fillColor) + "\"";
        if (st.fillColor.a < 255) {
            e += " fill-opacity=\"" + num(st.fillColor.a / 255.0) + "\"";
        }
    } else {
        e += " fill=\"none\"";
    }
    if (st.stroke && st.strokeColor.a > 0 && st.strokeWidth > 0) {
        static constexpr const char* kJoins[kLineJoinCount] = {"miter", "round", "bevel"};
        static constexpr const char* kCaps[kLineCapCount] = {"butt", "round", "square"};
        e += " stroke=\"" + hexColour(st.strokeColor) + "\" stroke-width=\"" + num(st.strokeWidth) + "\"";
        if (st.strokeColor.a < 255) {
            e += " stroke-opacity=\"" + num(st.strokeColor.a / 255.0) + "\"";
        }
        e += std::string(" stroke-linejoin=\"") + kJoins[static_cast<int>(st.join)] + "\"";
        e += std::string(" stroke-linecap=\"") + kCaps[static_cast<int>(st.cap)] + "\"";
    }
    if (!st.antialias) {
        e += " shape-rendering=\"crispEdges\"";
    }
    return e + "/>";
}

} // namespace gas
