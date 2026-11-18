#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"
#include "gas/core/TiledImage.h"

#include <string>
#include <vector>

namespace gas {

// A path node: anchor `p` with absolute cubic Bezier handles. A handle equal
// to its anchor means "no handle", so both handles at the anchor = a corner.
struct PathNode {
    PointF p;
    PointF in;  // controls the segment arriving at p
    PointF out; // controls the segment leaving p

    static PathNode corner(PointF at) { return {at, at, at}; }
    friend bool operator==(const PathNode&, const PathNode&) = default;
};

struct VectorPath {
    std::vector<PathNode> nodes;
    bool closed = false;

    friend bool operator==(const VectorPath&, const VectorPath&) = default;
};

// Stored in project files: append only.
enum class LineJoin { Miter, Round, Bevel };
enum class LineCap { Butt, Round, Square };
inline constexpr int kLineJoinCount = 3;
inline constexpr int kLineCapCount = 3;
inline constexpr double kMiterLimit = 4.0; // miter length / half width, as in SVG's default

struct VectorStyle {
    bool fill = true;
    Rgba8 fillColor{0, 0, 0, 255};
    bool stroke = false;
    Rgba8 strokeColor{0, 0, 0, 255};
    double strokeWidth = 2.0; // px, centred on the path
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;
    // Off = hard edges (pixel centres inside the shape), for pixel art.
    bool antialias = true;

    friend bool operator==(const VectorStyle&, const VectorStyle&) = default;
};

// One filled and/or stroked path. Fills use the nonzero rule and always close
// the path; open paths are stroked as drawn.
struct VectorShape {
    VectorPath path;
    VectorStyle style;

    friend bool operator==(const VectorShape&, const VectorShape&) = default;
};
using VectorShapes = std::vector<VectorShape>;

// --- Shape builders (closed paths, clockwise on screen) ---
// Rectangle with optional corner radius (clamped to half the shorter side).
VectorPath rectanglePath(RectF r, double radius = 0.0);
VectorPath ellipsePath(RectF r);
// Regular polygon (`sides` 3..64) around `centre`, first corner straight up,
// turned by `rotation` radians. innerRatio < 1 makes a star with `sides`
// points whose inner corners lie at radius * innerRatio.
VectorPath polygonPath(PointF centre, double radius, int sides, double innerRatio = 1.0, double rotation = 0.0);

// --- Geometry ---
// The path as a polyline (Bezier segments subdivided until within `tolerance`
// px); a closed path does not repeat its first point.
std::vector<PointF> flattenPath(const VectorPath& path, double tolerance = 0.2);
// Bounds of the anchors and handles (contain the curve).
RectF pathControlBounds(const VectorPath& path);
// Pixels the shape may touch: fill plus stroke (with miters and caps) plus anti-aliasing.
IRect shapeBounds(const VectorShape& shape);
// Every point mapped by `m`; the stroke width scales by sqrt(|det m|).
VectorShape transformedShape(const VectorShape& shape, const Affine& m);
VectorShapes transformedShapes(const VectorShapes& shapes, const Affine& m);
// Whether `p` lies in the fill, or within the stroke (at least `tolerance` px
// from the outline for shapes with no fill or a thin stroke).
bool hitShape(const VectorShape& shape, PointF p, double tolerance);
// Topmost shape under `p`, or -1.
int hitTestShapes(const VectorShapes& shapes, PointF p, double tolerance);

// --- Rendering ---
// Draws the shapes in order (fill, then stroke) over `dst` (source-over).
void renderShapes(TiledImage& dst, const VectorShapes& shapes);
// The shapes rendered on a transparent image of the given size.
TiledImage rasterizeShapes(const VectorShapes& shapes, int width, int height);

// --- SVG ---
// Path data ("M x y C ... Z"), locale independent.
std::string svgPathData(const VectorPath& path);
// A complete <path .../> element with the shape's fill and stroke.
std::string svgPathElement(const VectorShape& shape);

} // namespace gas
