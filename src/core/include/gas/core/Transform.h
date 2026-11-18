#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"

#include <vector>

namespace gas {

class TiledImage;

// A free-standing block of pixels positioned in document space, e.g. the
// content lifted out of a layer for a transform. Selection coverage is
// already folded into alpha.
struct FloatingImage {
    IRect rect; // where the pixels sit in the document before transforming
    std::vector<Rgba8> pixels; // rect.w * rect.h, row-major, straight alpha

    Rgba8 at(int x, int y) const; // local coordinates; transparent outside
};

// Composites `src` mapped through `m` (document space -> document space)
// onto `dst` with source-over, sampling at destination pixel centres.
// Nearest keeps pixel art exact (flips and 90-degree turns are lossless);
// Bilinear interpolates in premultiplied space. Returns the pixels touched.
IRect transformInto(TiledImage& dst, const FloatingImage& src, const Affine& m, Interpolation interpolation);

} // namespace gas
