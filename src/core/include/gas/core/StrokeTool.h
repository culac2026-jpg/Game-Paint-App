#pragma once

#include "gas/core/Geometry.h"
#include "gas/core/Symmetry.h"

namespace gas {

class TiledImage;

// A tool driven by a pointer drag: begin on press, moveTo per motion event,
// end on release. Positions are in document space (sub-pixel); `pressure`
// (0..1) comes from a pen tablet, 1 for the mouse.
class StrokeTool {
public:
    virtual ~StrokeTool() = default;

    virtual void begin(TiledImage& image, PointF p, double pressure = 1.0) = 0;
    virtual void moveTo(TiledImage& image, PointF p, double pressure = 1.0) = 0;
    virtual void end() = 0;
    virtual bool isStroking() const = 0;

    // Mirror painting; read when a stroke begins.
    void setSymmetry(const Symmetry& symmetry) { symmetry_ = symmetry; }
    const Symmetry& symmetry() const { return symmetry_; }

private:
    Symmetry symmetry_;
};

} // namespace gas
