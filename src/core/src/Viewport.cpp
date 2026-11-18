#include "gas/core/Viewport.h"

#include <algorithm>

namespace gas {

PointF Viewport::docToScreen(PointF doc) const {
    return {doc.x * zoom_ + offset_.x, doc.y * zoom_ + offset_.y};
}

PointF Viewport::screenToDoc(PointF screen) const {
    return {(screen.x - offset_.x) / zoom_, (screen.y - offset_.y) / zoom_};
}

void Viewport::setZoom(double zoom) { zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom); }

void Viewport::panBy(double dx, double dy) {
    offset_.x += dx;
    offset_.y += dy;
}

void Viewport::zoomAt(PointF anchor, double factor) {
    const PointF docAnchor = screenToDoc(anchor);
    setZoom(zoom_ * factor);
    offset_ = {anchor.x - docAnchor.x * zoom_, anchor.y - docAnchor.y * zoom_};
}

void Viewport::fit(int docWidth, int docHeight, double viewWidth, double viewHeight, double margin) {
    const double availW = std::max(1.0, viewWidth - 2.0 * margin);
    const double availH = std::max(1.0, viewHeight - 2.0 * margin);
    setZoom(std::min(availW / docWidth, availH / docHeight));
    center(docWidth, docHeight, viewWidth, viewHeight);
}

void Viewport::center(int docWidth, int docHeight, double viewWidth, double viewHeight) {
    offset_ = {(viewWidth - docWidth * zoom_) / 2.0, (viewHeight - docHeight * zoom_) / 2.0};
}

} // namespace gas
