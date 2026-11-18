#pragma once

#include "gas/core/Color.h"

#include <cstdint>
#include <string_view>

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

// Blend modes of the W3C Compositing and Blending spec (same formulas as
// CSS/SVG/PDF, so results match other editors). The numeric values are stored
// in project files: append only, never reorder.
enum class BlendMode : std::uint8_t {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    LinearDodge, // "Add"
    LinearBurn,
    Hue,
    Saturation,
    Color,
    Luminosity,
};
inline constexpr int kBlendModeCount = 18;
std::string_view blendModeName(BlendMode mode);

// Composites `src` onto `dst` with the blend mode; `coverage` scales the source
// alpha. Where dst is transparent the source shows unblended (as in the spec).
// Normal is exactly blendOver(). Pure double math, so results are deterministic.
Rgba8 blendPixel(Rgba8 dst, Rgba8 src, BlendMode mode, double coverage);

// Layer-mask value (0 = hidden, 255 = shown) stored in an RGBA mask pixel:
// its luminance composited over white, so unpainted (transparent) = shown
// and erasing the mask reveals.
std::uint8_t maskValue(Rgba8 maskPixel);

} // namespace gas
