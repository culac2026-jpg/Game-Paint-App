#include "gas/core/Brush.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>

namespace gas {

namespace {

constexpr int kTile = TiledImage::kTileSize;
constexpr double kMinSpacingPx = 0.5;

std::uint64_t tileKey(int tx, int ty) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ty)) << 32) |
           static_cast<std::uint32_t>(tx);
}

double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

} // namespace

double dabCoverage(double d, double diameter, double hardness) {
    const double radius = diameter / 2.0;
    // Anti-aliased hard edge: 1 inside, 0 outside, linear across the boundary pixel.
    const double edge = std::clamp(radius - d + 0.5, 0.0, 1.0);
    const double inner = radius * std::clamp(hardness, 0.0, 1.0);
    if (d <= inner || radius <= inner) {
        return edge;
    }
    const double t = std::clamp((d - inner) / (radius - inner), 0.0, 1.0);
    return std::min(edge, 1.0 - smoothstep(t));
}

double BrushTool::effectiveDiameter() const {
    return std::clamp(settings_.diameter, 1.0, BrushSettings::kMaxDiameter);
}

void BrushTool::begin(TiledImage& image, PointF p) {
    strokeTiles_.clear();
    last_ = p;
    stampDab(image, p);
    distanceToNextDab_ = std::max(kMinSpacingPx, settings_.spacing * effectiveDiameter());
}

void BrushTool::moveTo(TiledImage& image, PointF p) {
    if (!last_) {
        begin(image, p);
        return;
    }
    const double step = std::max(kMinSpacingPx, settings_.spacing * effectiveDiameter());
    const double dx = p.x - last_->x;
    const double dy = p.y - last_->y;
    const double length = std::hypot(dx, dy);
    if (length <= 0.0) {
        return;
    }
    // Walk the segment, stamping every `step` px; the leftover distance
    // carries into the next segment so spacing stays even across events.
    double travelled = distanceToNextDab_;
    while (travelled <= length) {
        const double t = travelled / length;
        stampDab(image, {last_->x + dx * t, last_->y + dy * t});
        travelled += step;
    }
    distanceToNextDab_ = travelled - length;
    last_ = p;
}

void BrushTool::end() {
    last_.reset();
    strokeTiles_.clear();
}

BrushTool::StrokeTile& BrushTool::strokeTile(const TiledImage& image, int tx, int ty) {
    auto [it, inserted] = strokeTiles_.try_emplace(tileKey(tx, ty));
    if (inserted) {
        it->second.original = image.sharedTile(tx, ty);
        it->second.mask = std::make_unique<Mask>();
        it->second.mask->fill(0.0f);
    }
    return it->second;
}

void BrushTool::stampDab(TiledImage& image, PointF center) {
    const double diameter = effectiveDiameter();
    const double reach = diameter / 2.0 + 1.0;
    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - reach)));
    const int x1 = std::min(image.width() - 1, static_cast<int>(std::ceil(center.x + reach)));
    const int y1 = std::min(image.height() - 1, static_cast<int>(std::ceil(center.y + reach)));
    if (x0 > x1 || y0 > y1) {
        return; // dab entirely off-canvas
    }
    const double opacity = std::clamp(settings_.opacity, 0.0, 1.0);

    // Process the dab one tile at a time so per-tile lookups happen once.
    for (int ty = y0 / kTile; ty <= y1 / kTile; ++ty) {
        for (int tx = x0 / kTile; tx <= x1 / kTile; ++tx) {
            StrokeTile& st = strokeTile(image, tx, ty);
            TiledImage::Tile* target = nullptr; // fetched on first change
            const int px0 = std::max(x0, tx * kTile);
            const int py0 = std::max(y0, ty * kTile);
            const int px1 = std::min(x1, tx * kTile + kTile - 1);
            const int py1 = std::min(y1, ty * kTile + kTile - 1);
            for (int y = py0; y <= py1; ++y) {
                for (int x = px0; x <= px1; ++x) {
                    const double d = std::hypot(x + 0.5 - center.x, y + 0.5 - center.y);
                    const auto coverage = static_cast<float>(dabCoverage(d, diameter, settings_.hardness));
                    const auto i = static_cast<std::size_t>(TiledImage::pixelIndex(x, y));
                    float& m = (*st.mask)[i];
                    if (coverage <= m) {
                        continue;
                    }
                    m = coverage;
                    const Rgba8 original = st.original ? st.original->pixels[i] : Rgba8::transparent();
                    const double amount = m * opacity;
                    const Rgba8 out = mode_ == BrushMode::Paint ? blendOver(original, color_, amount)
                                                                : eraseAlpha(original, amount);
                    if (!target) {
                        target = &image.editTile(tx, ty);
                    }
                    target->pixels[i] = out;
                }
            }
        }
    }
}

} // namespace gas
