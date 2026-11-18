#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"
#include "gas/core/TiledImage.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace gas {

// Non-destructive layer effects. A layer's effect stack is part of its
// LayerProps; the document renders "pixels + effects" into a cache and never
// modifies the layer's own pixels.
//
// Rendering order (like layer styles in other editors):
//   1. Filters modify the layer content, in stack order:
//      ColorAdjust, Blur, Pixelate.
//   2. Styles are derived from the filtered content's alpha and stacked:
//      DropShadow < OuterGlow < Outline < content (+ ColorOverlay) < InnerShadow.
// Numeric values are stored in project files: append only, never reorder.
enum class EffectType : std::uint8_t {
    ColorAdjust,
    Blur,
    Pixelate,
    Outline,
    DropShadow,
    InnerShadow,
    OuterGlow,
    ColorOverlay,
};
inline constexpr int kEffectTypeCount = 8;
std::string_view effectName(EffectType type);

// One effect. Fields not used by a type are ignored (and kept, so switching
// types in a UI doesn't lose values).
struct Effect {
    EffectType type = EffectType::Outline;
    bool enabled = true;
    Rgba8 color = Rgba8::black(); // Outline, shadows, glow, overlay
    double opacity = 1.0;         // 0..1
    double size = 1.0;            // px: outline width, blur radius, glow size, pixelate cell
    double offsetX = 0.0;         // px: shadows
    double offsetY = 0.0;
    bool smooth = false;          // Outline: anti-aliased edge (off = crisp, for pixel art)
    // ColorAdjust
    double hue = 0.0;        // degrees, -180..180
    double saturation = 0.0; // -1..1
    double lightness = 0.0;  // -1..1
    double contrast = 0.0;   // -1..1

    static Effect make(EffectType type); // sensible defaults per type

    friend bool operator==(const Effect&, const Effect&) = default;
};

inline constexpr double kMaxEffectSize = 128.0;
inline constexpr double kMaxEffectOffset = 256.0;

// Clamps every field into its valid range.
Effect sanitized(Effect e);

bool hasActiveEffects(const std::vector<Effect>& effects);

// How far (px) the stack can move pixels or read neighbours: changing a source
// pixel can change output pixels up to this distance away.
int effectMargin(const std::vector<Effect>& effects);

// Renders `effects` applied to `source` (optionally multiplied by a layer-mask
// value image, see maskValue) into the tiles of `out` covering `tileArea`
// (tile coordinates). Those tiles of `out` are replaced; others untouched.
void renderEffects(const TiledImage& source, const TiledImage* mask, const std::vector<Effect>& effects,
                   IRect tileArea, TiledImage& out);

} // namespace gas
