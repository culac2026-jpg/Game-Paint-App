#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"
#include "gas/core/StrokeTool.h"

#include <optional>
#include <vector>

namespace gas {

class TiledImage;

// Draws a 1-pixel line from a to b inclusive (Bresenham), clipped to the image.
void drawLine(TiledImage& image, Point a, Point b, Rgba8 color);

// Hard 1-pixel pencil that overwrites pixels (no blending). Each moveTo()
// connects to the previous pixel so fast pointer motion never leaves gaps.
class PencilTool : public StrokeTool {
public:
    void setColor(Rgba8 color) { color_ = color; }
    Rgba8 color() const { return color_; }

    void begin(TiledImage& image, PointF p, double pressure = 1.0) override;
    void moveTo(TiledImage& image, PointF p, double pressure = 1.0) override;
    void end() override;
    bool isStroking() const override { return last_.has_value(); }

private:
    void plot(TiledImage& image, Point p);

    Rgba8 color_ = Rgba8::black();
    std::optional<Point> last_;
    std::vector<SymmetryCopy> copies_{SymmetryCopy{}};
    bool gridAligned_ = true;
};

} // namespace gas
