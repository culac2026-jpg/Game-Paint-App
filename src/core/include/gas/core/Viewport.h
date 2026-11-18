#pragma once

#include "gas/core/Geometry.h"

namespace gas {

// Maps between document pixels and widget (screen) coordinates:
//   screen = doc * zoom + offset
// Kept free of Qt so zoom/pan math is unit-testable.
class Viewport {
public:
    static constexpr double kMinZoom = 1.0 / 32.0;
    static constexpr double kMaxZoom = 64.0;

    double zoom() const { return zoom_; }
    PointF offset() const { return offset_; }

    PointF docToScreen(PointF doc) const;
    PointF screenToDoc(PointF screen) const;

    void setZoom(double zoom);
    void setOffset(PointF offset) { offset_ = offset; }
    void panBy(double dx, double dy);

    // Multiplies zoom by factor (clamped) keeping the doc point under `anchor` fixed.
    void zoomAt(PointF anchor, double factor);

    // Largest zoom that shows the whole document inside the view with a margin, centered.
    void fit(int docWidth, int docHeight, double viewWidth, double viewHeight, double margin = 16.0);

    // Centers the document at the current zoom.
    void center(int docWidth, int docHeight, double viewWidth, double viewHeight);

private:
    double zoom_ = 1.0;
    PointF offset_{};
};

} // namespace gas
