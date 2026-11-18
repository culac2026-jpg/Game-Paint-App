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

// Largest per-channel difference; all fully transparent pixels compare equal.
int colorDistance(Rgba8 a, Rgba8 b);

} // namespace gas
