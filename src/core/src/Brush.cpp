#include "gas/core/Brush.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gas {

namespace {

constexpr int kTile = TiledImage::kTileSize;
constexpr double kMinSpacingPx = 0.5;
constexpr double kMinDabDiameter = 0.5; // smaller dabs are skipped

std::uint64_t tileKey(int tx, int ty) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ty)) << 32) |
           static_cast<std::uint32_t>(tx);
}

double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

// SplitMix64: tiny, fast and identical on every platform (unlike <random> distributions).
std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

double dabCoverage(double d, double diameter, double hardness, double edgeScale) {
    const double radius = diameter / 2.0;
    // Anti-aliased hard edge: 1 inside, 0 outside, linear across the boundary pixel.
    const double edge = std::clamp((radius - d) / std::max(edgeScale, 1e-6) + 0.5, 0.0, 1.0);
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

double BrushTool::random() {
    return static_cast<double>(splitmix(rng_) >> 11) * (1.0 / 9007199254740992.0); // 53-bit
}

BrushTool::Dab BrushTool::makeDab(PointF p, double pressure, double direction) {
    const BrushSettings& s = settings_;
    pressure = std::clamp(pressure, 0.0, 1.0);
    const double base = effectiveDiameter();
    Dab dab{p, base, 1.0, s.angle * std::numbers::pi / 180.0};
    dab.diameter *= 1.0 - std::clamp(s.pressureSize, 0.0, 1.0) * (1.0 - pressure);
    dab.opacity *= 1.0 - std::clamp(s.pressureOpacity, 0.0, 1.0) * (1.0 - pressure);
    if (s.followDirection) {
        dab.angle += direction;
    }
    // Always draw the same number of random values per dab, so turning one
    // dynamic on doesn't reshuffle the others.
    const double rSize = random(), rOpacity = random(), rAngle = random(), rx = random(), ry = random();
    dab.diameter *= 1.0 - std::clamp(s.sizeJitter, 0.0, 1.0) * rSize;
    dab.opacity *= 1.0 - std::clamp(s.opacityJitter, 0.0, 1.0) * rOpacity;
    dab.angle += std::clamp(s.angleJitter, 0.0, 1.0) * std::numbers::pi * (2.0 * rAngle - 1.0);
    const double scatter = std::clamp(s.scatter, 0.0, 4.0) * base;
    dab.center.x += scatter * (2.0 * rx - 1.0);
    dab.center.y += scatter * (2.0 * ry - 1.0);
    return dab;
}

double BrushTool::stepAfter(const Dab& dab) const {
    return std::max(kMinSpacingPx, std::clamp(settings_.spacing, 0.01, 10.0) * std::max(dab.diameter, 1.0));
}

void BrushTool::begin(TiledImage& image, PointF p, double pressure) {
    strokeTiles_.clear();
    rng_ = seed_;
    splitmix(seed_); // the next stroke gets fresh randomness
    last_ = p;
    lastPressure_ = pressure;
    const Dab dab = makeDab(p, pressure, 0.0);
    stampDab(image, dab);
    distanceToNextDab_ = stepAfter(dab);
}

void BrushTool::moveTo(TiledImage& image, PointF p, double pressure) {
    if (!last_) {
        begin(image, p, pressure);
        return;
    }
    const double dx = p.x - last_->x;
    const double dy = p.y - last_->y;
    const double length = std::hypot(dx, dy);
    if (length <= 0.0) {
        lastPressure_ = pressure;
        return;
    }
    const double direction = std::atan2(dy, dx);
    // Walk the segment, stamping a dab every step (which depends on the dab's
    // size); the leftover distance carries into the next segment so spacing
    // stays even across events. Pressure is interpolated along the segment.
    double travelled = distanceToNextDab_;
    while (travelled <= length) {
        const double t = travelled / length;
        const Dab dab = makeDab({last_->x + dx * t, last_->y + dy * t},
                                lastPressure_ + (pressure - lastPressure_) * t, direction);
        stampDab(image, dab);
        travelled += stepAfter(dab);
    }
    distanceToNextDab_ = travelled - length;
    last_ = p;
    lastPressure_ = pressure;
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

void BrushTool::stampDab(TiledImage& image, const Dab& dab) {
    if (dab.diameter < kMinDabDiameter || dab.opacity <= 0.0) {
        return;
    }
    const PointF center = dab.center;
    const double reach = dab.diameter / 2.0 + 1.0;
    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - reach)));
    const int x1 = std::min(image.width() - 1, static_cast<int>(std::ceil(center.x + reach)));
    const int y1 = std::min(image.height() - 1, static_cast<int>(std::ceil(center.y + reach)));
    if (x0 > x1 || y0 > y1) {
        return; // dab entirely off-canvas
    }
    const double opacity = std::clamp(settings_.opacity, 0.0, 1.0);
    const double roundness = std::clamp(settings_.roundness, 0.05, 1.0);
    const bool round = roundness >= 1.0;
    const double cs = std::cos(dab.angle), sn = std::sin(dab.angle);

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
                    const double ox = x + 0.5 - center.x, oy = y + 0.5 - center.y;
                    double d;
                    double edgeScale = 1.0;
                    if (round) {
                        d = std::hypot(ox, oy);
                    } else {
                        // Into the tip's frame, then stretch the minor axis back to a circle.
                        const double lx = cs * ox + sn * oy;
                        const double ly = -sn * ox + cs * oy;
                        const double sy = ly / (roundness * roundness);
                        d = std::hypot(lx, ly / roundness);
                        // |grad d|: d-units per pixel at this point (1 along the major axis, 1/roundness across).
                        edgeScale = d > 1e-9 ? std::hypot(lx, sy) / d : 1.0;
                    }
                    const auto coverage = static_cast<float>(
                        dabCoverage(d, dab.diameter, settings_.hardness, edgeScale) * dab.opacity);
                    const auto i = static_cast<std::size_t>(TiledImage::pixelIndex(x, y));
                    float& m = (*st.mask)[i];
                    if (coverage <= m) {
                        continue;
                    }
                    m = coverage;
                    const Rgba8 original = st.original ? st.original->pixels[i] : Rgba8::transparent();
                    const double amount = m * opacity;
                    const Rgba8 out = mode_ == BrushMode::Paint
                                          ? blendPixel(original, color_, settings_.blendMode, amount)
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
