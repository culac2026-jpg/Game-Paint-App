#pragma once

#include "gas/core/Blend.h"
#include "gas/core/Color.h"
#include "gas/core/Geometry.h"
#include "gas/core/StrokeTool.h"
#include "gas/core/TiledImage.h"

#include <array>
#include <string_view>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace gas {

class Selection;

// Shape of the brush tip. Values are stored in preset files: append only.
// Bristle = a round tip made of streaks (use with followDirection for oil-paint strokes).
enum class BrushShape : std::uint8_t { Round, Square, Diamond, Triangle, Hexagon, Star, Custom, Bristle };
inline constexpr int kBrushShapeCount = 8;
std::string_view brushShapeName(BrushShape shape);

// A custom tip: an 8-bit coverage image (255 = full paint). The dab diameter
// maps to its longer side; it is centred and sampled bilinearly.
struct BrushTip {
    static constexpr int kMaxSize = 512;

    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha; // row-major width * height

    bool empty() const { return width <= 0 || height <= 0; }
    // Coverage 0..1 at (u, v), both in [-1, 1] across the longer side.
    double sample(double u, double v) const;

    friend bool operator==(const BrushTip&, const BrushTip&) = default;
};

// Outline of a shape with circumradius 1 (y down), for drawing the brush
// cursor. Custom tips return their unit bounding square.
std::vector<PointF> brushShapeOutline(BrushShape shape);

// Tip from the alpha of `image` inside `area`, times the selection coverage if
// given. Trimmed to its content and box-downscaled to fit kMaxSize. Empty if
// nothing is painted there.
BrushTip brushTipFromImage(const TiledImage& image, IRect area, const Selection* selection = nullptr);

// Shared, immutable tip; compares by content so settings stay comparable.
struct BrushTipRef {
    std::shared_ptr<const BrushTip> tip;

    friend bool operator==(const BrushTipRef& a, const BrushTipRef& b) {
        return a.tip == b.tip || (a.tip && b.tip && *a.tip == *b.tip);
    }
};

struct BrushSettings {
    double diameter = 12.0; // px, clamped to [1, kMaxDiameter]
    double opacity = 1.0;   // 0..1, the most alpha one stroke can add/remove
    double hardness = 0.8;  // 0..1, fraction of the radius at full strength
    double spacing = 0.1;   // dab distance as a fraction of the dab diameter

    // Tip shape
    BrushShape shape = BrushShape::Round;
    BrushTipRef tip;              // used when shape == Custom (falls back to Round if empty)
    double roundness = 1.0;       // 0.05..1, minor/major axis ratio
    double angle = 0.0;           // degrees, rotation of the tip's major axis
    bool followDirection = false; // add the stroke direction to `angle`

    // Dynamics (all 0..1; 0 = off)
    double pressureSize = 0.0;    // at zero pressure the dab shrinks by this fraction
    double pressureOpacity = 0.0; // at zero pressure the dab fades by this fraction
    double sizeJitter = 0.0;      // random per-dab size reduction
    double opacityJitter = 0.0;   // random per-dab opacity reduction
    double angleJitter = 0.0;     // random rotation, up to +-180 degrees at 1
    double scatter = 0.0;         // random offset, in diameters (0..4)

    // Media (all 0 = off)
    double grain = 0.0;       // 0..1, paper texture anchored to the canvas (crayon, pencil)
    double grainScale = 3.0;  // px, size of the grain features (0.5..64)
    double particles = 0.0;   // 0..1, airbrush: each dab sprays random dots at this density
    double mix = 0.0;         // 0..1, wet paint: colour picked up from the layer per tip diameter travelled (Paint mode)

    double smoothing = 0.0; // stabilizer strength, applied by the input layer (StrokeSmoother)
    BlendMode blendMode = BlendMode::Normal; // Paint mode only

    static constexpr double kMaxDiameter = 1000.0;

    friend bool operator==(const BrushSettings&, const BrushSettings&) = default;
};

// Paint and Erase recompute pixels from the stroke's coverage mask. Smudge
// (drag colour along, like a finger) and Blur (soften what's under the tip)
// work progressively on the current pixels; for them `opacity` is the strength.
enum class BrushMode { Paint, Erase, Smudge, Blur };

// Coverage (0..1) of a round dab of the given diameter/hardness at distance
// `d` from its centre. The outer edge is anti-aliased over ~1 px; `edgeScale`
// is how many units of `d` one pixel spans there (>1 on the short axis of a
// flattened tip, so its edge stays 1 px soft instead of aliasing).
double dabCoverage(double d, double diameter, double hardness, double edgeScale = 1.0);

// Brush that stamps dabs along the pointer path.
//
// Overlapping dabs never stack: each stroke keeps a coverage mask holding the
// max (dab coverage x dab opacity) seen per pixel, and every touched pixel is
// recomputed from the (copy-on-write shared) tile as it was when the stroke
// first reached it. So one stroke never exceeds `opacity`, however often it
// crosses itself, and the result does not depend on how events are batched.
//
// Random dynamics come from a small deterministic generator seeded per stroke
// (setSeed), so the same input always produces the same pixels.
class BrushTool : public StrokeTool {
public:
    explicit BrushTool(BrushMode mode = BrushMode::Paint) : mode_(mode) {}

    BrushMode mode() const { return mode_; }
    BrushSettings& settings() { return settings_; }
    const BrushSettings& settings() const { return settings_; }
    void setColor(Rgba8 color) { color_ = color; }
    Rgba8 color() const { return color_; }
    // Seed of the next stroke; each stroke advances it.
    void setSeed(std::uint64_t seed) { seed_ = seed; }

    void begin(TiledImage& image, PointF p, double pressure = 1.0) override;
    void moveTo(TiledImage& image, PointF p, double pressure = 1.0) override;
    void end() override;
    bool isStroking() const override { return last_.has_value(); }

    // Diameter at full pressure, before jitter (for the cursor outline).
    double effectiveDiameter() const;

private:
    using Mask = std::array<float, TiledImage::kTileSize * TiledImage::kTileSize>;
    struct StrokeTile {
        TiledImage::TilePtr original; // shared copy-on-write; null = was transparent
        std::unique_ptr<Mask> mask;
    };
    struct Dab {
        PointF center;
        double diameter;
        double opacity; // 0..1, multiplies the tip coverage
        double angle;   // radians
    };

    Dab makeDab(PointF p, double pressure, double direction);
    double stepAfter(const Dab& dab) const;
    void stampDab(TiledImage& image, const Dab& dab);
    void retouchDab(TiledImage& image, const Dab& dab, int x0, int y0, int w, int h);
    Rgba8 originalPixel(const TiledImage& image, int x, int y) const;
    StrokeTile& strokeTile(const TiledImage& image, int tx, int ty);
    double random(); // 0..1

    BrushMode mode_;
    BrushSettings settings_;
    Rgba8 color_ = Rgba8::black();
    std::uint64_t seed_ = 0x9E3779B97F4A7C15ull;
    std::uint64_t rng_ = 0;

    std::optional<PointF> last_;
    double lastPressure_ = 1.0;
    double distanceToNextDab_ = 0.0;
    std::unordered_map<std::uint64_t, StrokeTile> strokeTiles_;

    std::uint64_t strokeSeed_ = 0; // per-stroke noise seed (particles)
    std::uint64_t dabIndex_ = 0;
    std::vector<float> coverage_;   // per-dab scratch, dab bounding box
    std::array<double, 3> paint_{}; // wet-mix colour carried by the brush
    // Smudge: premultiplied colour carried by the tip, centred on the dab.
    std::vector<std::array<float, 4>> carried_;
    int carriedSize_ = 0;
    bool carriedLoaded_ = false;
};

} // namespace gas
