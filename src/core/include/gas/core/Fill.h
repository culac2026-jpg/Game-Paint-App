#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"

#include <optional>

namespace gas {

class TiledImage;

// Contiguous flood fill from `seed`, replacing every pixel whose
// colorDistance() to the seed colour is <= tolerance (0..255).
// 4-connected, so it cannot leak through 1-px diagonal (8-connected) lines
// such as those drawn by the pencil. Returns the number of pixels changed.
long long floodFill(TiledImage& image, Point seed, Rgba8 color, int tolerance = 0);

// Colour under `p`, or nullopt when outside the image or fully transparent.
std::optional<Rgba8> pickColor(const TiledImage& image, Point p);

} // namespace gas
