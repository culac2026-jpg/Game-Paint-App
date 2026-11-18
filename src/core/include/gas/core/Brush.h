#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"
#include "gas/core/StrokeTool.h"
#include "gas/core/TiledImage.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace gas {

struct BrushSettings {
    double diameter = 12.0; // px, clamped to [1, kMaxDiameter]
    double opacity = 1.0;   // 0..1, the most alpha one stroke can add/remove
    double hardness = 0.8;  // 0..1, fraction of the radius at full strength
    double spacing = 0.1;   // dab distance as a fraction of the diameter

    static constexpr double kMaxDiameter = 1000.0;
};

enum class BrushMode { Paint, Erase };

// Coverage (0..1) of a round dab of the given diameter/hardness at distance
// `d` from its centre. The outer edge is anti-aliased over ~1 px.
double dabCoverage(double d, double diameter, double hardness);

// Round brush that stamps dabs along the pointer path.
//
// Overlapping dabs never stack: each stroke keeps a coverage mask holding the
// max coverage seen per pixel, and every touched pixel is recomputed from the
// (copy-on-write shared) tile as it was when the stroke first reached it. So one
// stroke never exceeds `opacity`, however often it crosses itself.
class BrushTool : public StrokeTool {
public:
    explicit BrushTool(BrushMode mode = BrushMode::Paint) : mode_(mode) {}

    BrushMode mode() const { return mode_; }
    BrushSettings& settings() { return settings_; }
    const BrushSettings& settings() const { return settings_; }
    void setColor(Rgba8 color) { color_ = color; }
    Rgba8 color() const { return color_; }

    void begin(TiledImage& image, PointF p) override;
    void moveTo(TiledImage& image, PointF p) override;
    void end() override;
    bool isStroking() const override { return last_.has_value(); }

    double effectiveDiameter() const;

private:
    using Mask = std::array<float, TiledImage::kTileSize * TiledImage::kTileSize>;
    struct StrokeTile {
        TiledImage::TilePtr original; // shared copy-on-write; null = was transparent
        std::unique_ptr<Mask> mask;
    };

    void stampDab(TiledImage& image, PointF center);
    StrokeTile& strokeTile(const TiledImage& image, int tx, int ty);

    BrushMode mode_;
    BrushSettings settings_;
    Rgba8 color_ = Rgba8::black();

    std::optional<PointF> last_;
    double distanceToNextDab_ = 0.0;
    std::unordered_map<std::uint64_t, StrokeTile> strokeTiles_;
};

} // namespace gas
