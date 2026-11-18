#pragma once

#include "gas/core/Color.h"

namespace gas {

// Alpha policy: pixels are *stored* as straight (non-premultiplied) RGBA8,
// matching PNG and the canvas upload format. Blend math converts to
// premultiplied internally, so fully transparent pixels never leak colour.
// A pixel with alpha 0 is normalised to all-zero.

// Source-over of `src` onto `dst`; `coverage` (0..1) scales the source alpha.
Rgba8 blendOver(Rgba8 dst, Rgba8 src, double coverage);

// Removes `amount` (0..1) of dst's alpha, keeping its colour.
Rgba8 eraseAlpha(Rgba8 dst, double amount);

// Linear interpolation from `a` (t = 0) to `b` (t = 1) in premultiplied space.
Rgba8 mix(Rgba8 a, Rgba8 b, double t);

// Largest per-channel difference; all fully transparent pixels compare equal.
int colorDistance(Rgba8 a, Rgba8 b);

// Layer-mask value (0 = hidden, 255 = shown) stored in an RGBA mask pixel:
// its luminance composited over white, so unpainted (transparent) = shown
// and erasing the mask reveals.
std::uint8_t maskValue(Rgba8 maskPixel);

} // namespace gas
