#pragma once

#include "gas/core/Geometry.h"

namespace gas {

class TiledImage;

// A tool driven by a pointer drag: begin on press, moveTo per motion event,
// end on release. Positions are in document space (sub-pixel).
class StrokeTool {
public:
    virtual ~StrokeTool() = default;

    virtual void begin(TiledImage& image, PointF p) = 0;
    virtual void moveTo(TiledImage& image, PointF p) = 0;
    virtual void end() = 0;
    virtual bool isStroking() const = 0;
};

} // namespace gas
