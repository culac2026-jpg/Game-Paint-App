#pragma once

#include "gas/core/Brush.h"

#include <string>
#include <string_view>
#include <vector>

namespace gas {

struct BrushPreset {
    std::string name;
    BrushSettings settings;

    friend bool operator==(const BrushPreset&, const BrushPreset&) = default;
};

// Presets shipped with the app.
const std::vector<BrushPreset>& builtinBrushPresets();

// Plain-text preset file, locale independent:
//   [preset]
//   name = Ink Pen
//   diameter = 8
//   ...
// Unknown keys are ignored and values are clamped, so files from newer
// versions still load. Presets without a name are dropped.
std::string serializeBrushPresets(const std::vector<BrushPreset>& presets);
std::vector<BrushPreset> parseBrushPresets(std::string_view text);

} // namespace gas
