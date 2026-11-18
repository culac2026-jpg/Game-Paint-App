#include "gas/core/Brush.h"

#include "gas/core/Blend.h"
#include "gas/core/Selection.h"

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

struct Vec {
    double x, y;
};

double cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }

// Polygon outlines with circumradius 1 (y points down, so -90 degrees is up).
std::vector<Vec> regularPolygon(int sides, double startDegrees, double innerRadius = 0.0) {
    std::vector<Vec> pts;
    const int n = innerRadius > 0 ? sides * 2 : sides;
    for (int i = 0; i < n; ++i) {
        const double a = (startDegrees + 360.0 * i / n) * std::numbers::pi / 180.0;
        const double r = (innerRadius > 0 && (i % 2)) ? innerRadius : 1.0;
        pts.push_back({r * std::cos(a), r * std::sin(a)});
    }
    return pts;
}

const std::vector<Vec>& polygonFor(BrushShape shape) {
    static const std::vector<Vec> triangle = regularPolygon(3, -90);
    static const std::vector<Vec> hexagon = regularPolygon(6, 0);
    static const std::vector<Vec> star = regularPolygon(5, -90, 0.45);
    static const std::vector<Vec> none;
    switch (shape) {
    case BrushShape::Triangle: return triangle;
    case BrushShape::Hexagon: return hexagon;
    case BrushShape::Star: return star;
    default: return none;
    }
}

// Gauge (Minkowski) function of a polygon that is star-shaped around the
// origin: 1 on the outline, scaling linearly along every ray from the centre.
double polygonGauge(const std::vector<Vec>& poly, Vec p) {
    if (p.x == 0.0 && p.y == 0.0) {
        return 0.0;
    }
    double gauge = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec a = poly[i];
        const Vec b = poly[(i + 1) % poly.size()];
        const Vec e{b.x - a.x, b.y - a.y};
        const double denom = cross(p, e);
        if (std::abs(denom) < 1e-12) {
            continue;
        }
        const double s = cross(a, p) / denom;
        const double ae = cross(a, e);
        if (s < -1e-9 || s > 1 + 1e-9 || ae * denom <= 0) {
            continue; // the ray misses this edge (or hits it behind the centre)
        }
        gauge = std::max(gauge, denom / ae);
    }
    return gauge;
}

// Distance-like value (px) whose outline is at `radius`, in the tip's frame.
double shapeDistance(BrushShape shape, double lx, double ly, double radius) {
    switch (shape) {
    case BrushShape::Square: return std::max(std::abs(lx), std::abs(ly));
    case BrushShape::Diamond: return std::abs(lx) + std::abs(ly);
    case BrushShape::Triangle:
    case BrushShape::Hexagon:
    case BrushShape::Star:
        return radius * polygonGauge(polygonFor(shape), {lx / radius, ly / radius});
    default: return std::hypot(lx, ly);
    }
}

double unit(std::uint64_t& state) { return static_cast<double>(splitmix(state) >> 11) * 0x1.0p-53; }

// Deterministic per-pixel random value in [0, 1).
double hashUnit(int x, int y, std::uint64_t seed) {
    std::uint64_t s = seed ^ ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                              static_cast<std::uint32_t>(y));
    return unit(s);
}

double valueNoise(double x, double y, std::uint64_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const double tx = smoothstep(x - fx), ty = smoothstep(y - fy);
    const double a = hashUnit(ix, iy, seed), b = hashUnit(ix + 1, iy, seed);
    const double c = hashUnit(ix, iy + 1, seed), d = hashUnit(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// Paper texture: a canvas-anchored coverage factor, so every stroke over the
// same spot skips the same "pits" (crossing strokes don't fill them in).
double grainFactor(int x, int y, double scale, double grain) {
    const double px = x + 0.5, py = y + 0.5;
    const double n = 0.6 * valueNoise(px / scale, py / scale, 0x51EDull) +
                     0.4 * valueNoise(px / (scale * 0.4), py / (scale * 0.4), 0xA77Eull);
    return std::clamp(1.0 - grain * (n - 0.25) * 2.0, 0.0, 1.0);
}

// Streak profile across a bristle tip (v in [-1, 1]); fixed, so strokes are repeatable.
double bristleProfile(double v) {
    struct Bristle {
        double pos, halfWidth, strength;
    };
    static const std::array<Bristle, 16> bristles = [] {
        std::array<Bristle, 16> b{};
        std::uint64_t state = 0xB415713ull;
        for (Bristle& one : b) {
            one.pos = -0.95 + 1.9 * unit(state);
            one.halfWidth = 0.07 + 0.08 * unit(state);
            one.strength = 0.5 + 0.5 * unit(state);
        }
        return b;
    }();
    double best = 0.12; // a little paint between the streaks
    for (const Bristle& b : bristles) {
        const double t = 1.0 - std::abs(v - b.pos) / b.halfWidth;
        if (t > 0) {
            best = std::max(best, b.strength * std::sqrt(t));
        }
    }
    return best;
}

using Premul = std::array<float, 4>;

Premul premultiply(Rgba8 p) {
    const float a = p.a / 255.0f;
    return {p.r / 255.0f * a, p.g / 255.0f * a, p.b / 255.0f * a, a};
}

Rgba8 unpremultiply(const Premul& p) {
    if (p[3] <= 0.5f / 255.0f) {
        return Rgba8::transparent();
    }
    const auto ch = [&](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v / p[3], 0.0f, 1.0f) * 255.0f));
    };
    return {ch(p[0]), ch(p[1]), ch(p[2]), static_cast<std::uint8_t>(std::lround(std::clamp(p[3], 0.0f, 1.0f) * 255.0f))};
}

std::uint8_t clampByte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0))); }

} // namespace

std::string_view brushShapeName(BrushShape shape) {
    static constexpr std::string_view names[kBrushShapeCount] = {"Round", "Square", "Diamond", "Triangle",
                                                                 "Hexagon", "Star", "Custom", "Bristle"};
    const auto i = static_cast<int>(shape);
    return i >= 0 && i < kBrushShapeCount ? names[i] : "Round";
}

std::vector<PointF> brushShapeOutline(BrushShape shape) {
    std::vector<PointF> pts;
    switch (shape) {
    case BrushShape::Square:
    case BrushShape::Custom: pts = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}; break;
    case BrushShape::Diamond: pts = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}}; break;
    case BrushShape::Triangle:
    case BrushShape::Hexagon:
    case BrushShape::Star:
        for (const Vec& v : polygonFor(shape)) {
            pts.push_back({v.x, v.y});
        }
        break;
    default:
        for (int i = 0; i < 48; ++i) {
            const double a = 2 * std::numbers::pi * i / 48;
            pts.push_back({std::cos(a), std::sin(a)});
        }
        break;
    }
    return pts;
}

double BrushTip::sample(double u, double v) const {
    if (empty()) {
        return 0.0;
    }
    const double half = std::max(width, height) / 2.0;
    // Pixel-centre coordinates in the tip image.
    const double px = u * half + width / 2.0 - 0.5;
    const double py = v * half + height / 2.0 - 0.5;
    const int x0 = static_cast<int>(std::floor(px)), y0 = static_cast<int>(std::floor(py));
    const double fx = px - x0, fy = py - y0;
    const auto at = [&](int x, int y) -> double {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0.0;
        }
        return alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] / 255.0;
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
           (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

BrushTip brushTipFromImage(const TiledImage& image, IRect area, const Selection* selection) {
    area = area.intersected({0, 0, image.width(), image.height()});
    BrushTip tip;
    if (area.empty()) {
        return tip;
    }
    // Coverage over the area, and its painted bounds.
    std::vector<double> cov(static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h), 0.0);
    int minX = area.w, minY = area.h, maxX = -1, maxY = -1;
    for (int y = 0; y < area.h; ++y) {
        for (int x = 0; x < area.w; ++x) {
            double c = image.pixel({area.x + x, area.y + y}).a / 255.0;
            if (selection && c > 0) {
                c *= selection->coverage({area.x + x, area.y + y}) / 255.0;
            }
            if (c > 0) {
                cov[static_cast<std::size_t>(y * area.w + x)] = c;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0) {
        return tip;
    }
    const int w = maxX - minX + 1, h = maxY - minY + 1;
    const int factor = (std::max(w, h) + BrushTip::kMaxSize - 1) / BrushTip::kMaxSize; // box downscale
    tip.width = (w + factor - 1) / factor;
    tip.height = (h + factor - 1) / factor;
    tip.alpha.assign(static_cast<std::size_t>(tip.width) * static_cast<std::size_t>(tip.height), 0);
    for (int ty = 0; ty < tip.height; ++ty) {
        for (int tx = 0; tx < tip.width; ++tx) {
            double sum = 0.0;
            for (int y = ty * factor; y < std::min(h, (ty + 1) * factor); ++y) {
                for (int x = tx * factor; x < std::min(w, (tx + 1) * factor); ++x) {
                    sum += cov[static_cast<std::size_t>((minY + y) * area.w + minX + x)];
                }
            }
            tip.alpha[static_cast<std::size_t>(ty * tip.width + tx)] = clampByte(sum / (factor * factor) * 255.0);
        }
    }
    return tip;
}

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
    strokeSeed_ = seed_ * 0xD1B54A32D192ED03ull + 1;
    dabIndex_ = 0;
    copies_ = symmetry().copies();
    paints_.assign(copies_.size(), {double(color_.r), double(color_.g), double(color_.b)});
    if (mode_ == BrushMode::Smudge) {
        carriedSize_ = 2 * static_cast<int>(std::ceil(effectiveDiameter() * std::numbers::sqrt2 / 2.0 + 2.0)) + 1;
        carried_.assign(copies_.size(),
                        std::vector<Premul>(static_cast<std::size_t>(carriedSize_) * static_cast<std::size_t>(carriedSize_)));
        carriedLoaded_.assign(copies_.size(), 0);
    }
    splitmix(seed_); // the next stroke gets fresh randomness
    last_ = p;
    lastPressure_ = pressure;
    const Dab dab = makeDab(p, pressure, 0.0);
    stampSymmetric(image, dab);
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
        stampSymmetric(image, dab);
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

Rgba8 BrushTool::originalPixel(const TiledImage& image, int x, int y) const {
    const auto it = strokeTiles_.find(tileKey(x / kTile, y / kTile));
    const TiledImage::TilePtr tile = it != strokeTiles_.end() ? it->second.original : image.sharedTile(x / kTile, y / kTile);
    return tile ? tile->pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))] : Rgba8::transparent();
}

void BrushTool::stampSymmetric(TiledImage& image, const Dab& dab) {
    const PointF center = symmetry().center;
    for (std::size_t k = 0; k < copies_.size(); ++k) {
        Dab copy = dab;
        copy.center = copies_[k].map(dab.center, center);
        copy.angle = copies_[k].mapAngle(dab.angle);
        copy.flipped = copies_[k].flip;
        copyIndex_ = k;
        stampDab(image, copy); // one shared coverage mask: overlapping copies don't stack
    }
}

void BrushTool::stampDab(TiledImage& image, const Dab& dab) {
    if (dab.diameter < kMinDabDiameter || dab.opacity <= 0.0) {
        return;
    }
    const PointF center = dab.center;
    BrushShape shape = settings_.shape;
    const BrushTip* tip = settings_.tip.tip.get();
    if (shape == BrushShape::Custom && (!tip || tip->empty())) {
        shape = BrushShape::Round; // no tip defined yet
    }
    // Square corners and custom tips reach sqrt(2) times the radius.
    const bool cornered = shape == BrushShape::Square || shape == BrushShape::Custom;
    const double radius = dab.diameter / 2.0;
    const double reach = radius * (cornered ? std::numbers::sqrt2 : 1.0) + 1.0;
    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - reach)));
    const int x1 = std::min(image.width() - 1, static_cast<int>(std::ceil(center.x + reach)));
    const int y1 = std::min(image.height() - 1, static_cast<int>(std::ceil(center.y + reach)));
    if (x0 > x1 || y0 > y1) {
        return; // dab entirely off-canvas
    }
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    const double roundness = std::clamp(settings_.roundness, 0.05, 1.0);
    const bool circle = shape == BrushShape::Round && roundness >= 1.0;
    const double cs = std::cos(dab.angle), sn = std::sin(dab.angle);
    const double grain = std::clamp(settings_.grain, 0.0, 1.0);
    const double grainScale = std::clamp(settings_.grainScale, 0.5, 64.0);
    const double particles = std::clamp(settings_.particles, 0.0, 1.0);
    const std::uint64_t dabSeed = strokeSeed_ ^ (++dabIndex_ * 0x9E3779B97F4A7C15ull);

    // Into the tip's frame, with the minor axis stretched back to full size.
    const auto local = [&](double px, double py, double& lx, double& ly) {
        lx = cs * px + sn * py;
        ly = (-sn * px + cs * py) / roundness;
        if (dab.flipped) {
            ly = -ly;
        }
    };
    const auto tipCoverage = [&](double ox, double oy) -> double {
        if (circle) {
            return dabCoverage(std::hypot(ox, oy), dab.diameter, settings_.hardness);
        }
        double lx, ly;
        local(ox, oy, lx, ly);
        if (shape == BrushShape::Custom) {
            return tip->sample(lx / radius, ly / radius);
        }
        if (shape == BrushShape::Bristle) {
            return dabCoverage(std::hypot(lx, ly), dab.diameter, settings_.hardness) * bristleProfile(ly / radius);
        }
        const auto dist = [&](double px, double py) {
            double ux, uy;
            local(px, py, ux, uy);
            return shapeDistance(shape, ux, uy, radius);
        };
        // |grad d| in px, so every outline stays ~1 px soft whatever the shape,
        // roundness or angle.
        const double edgeScale = std::hypot(dist(ox + 0.5, oy) - dist(ox - 0.5, oy),
                                            dist(ox, oy + 0.5) - dist(ox, oy - 0.5));
        return dabCoverage(dist(ox, oy), dab.diameter, settings_.hardness, std::max(edgeScale, 0.25));
    };

    // 1. Coverage of every pixel in the dab's box (tip x opacity x media).
    coverage_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f);
    bool any = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double t = tipCoverage(x + 0.5 - center.x, y + 0.5 - center.y);
            if (t <= 0.0) {
                continue;
            }
            double c = t * dab.opacity;
            if (particles > 0.0) {
                c = hashUnit(x, y, dabSeed) < particles * t ? dab.opacity : 0.0; // sprayed dots
            }
            if (grain > 0.0 && c > 0.0) {
                c *= grainFactor(x, y, grainScale, grain);
            }
            if (c > 0.0) {
                coverage_[static_cast<std::size_t>((y - y0) * w + (x - x0))] = static_cast<float>(c);
                any = true;
            }
        }
    }
    if (!any) {
        return;
    }
    if (mode_ == BrushMode::Smudge || mode_ == BrushMode::Blur) {
        retouchDab(image, dab, x0, y0, w, h);
        return;
    }

    // 2. Wet mix: pick up the colour under the dab (as it was before the stroke).
    Rgba8 paint = color_;
    const double mix = std::clamp(settings_.mix, 0.0, 1.0);
    if (mode_ == BrushMode::Paint && mix > 0.0) {
        double weight = 0, alpha = 0, r = 0, g = 0, b = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double c = coverage_[static_cast<std::size_t>((y - y0) * w + (x - x0))];
                if (c <= 0) {
                    continue;
                }
                const Rgba8 o = originalPixel(image, x, y);
                const double a = c * o.a / 255.0;
                weight += c;
                alpha += a;
                r += a * o.r;
                g += a * o.g;
                b += a * o.b;
            }
        }
        // `mix` is the pickup per tip diameter travelled, so the spacing doesn't
        // change how far the paint carries. Transparent areas don't dilute it,
        // so it works on an empty layer.
        const double step = std::clamp(settings_.spacing, 0.01, 1.0);
        const double pickup =
            alpha > 1e-6 ? 1.0 - std::pow(1.0 - std::min(mix * alpha / weight, 0.999), step) : 0.0;
        const double reload = 1.0 - std::pow(1.0 - 0.3 * (1.0 - mix), step);
        const double sampled[3] = {alpha > 1e-6 ? r / alpha : 0, alpha > 1e-6 ? g / alpha : 0, alpha > 1e-6 ? b / alpha : 0};
        const double base[3] = {double(color_.r), double(color_.g), double(color_.b)};
        std::array<double, 3>& carriedPaint = paints_[copyIndex_];
        for (std::size_t k = 0; k < 3; ++k) {
            carriedPaint[k] += (sampled[k] - carriedPaint[k]) * pickup;
            carriedPaint[k] += (base[k] - carriedPaint[k]) * reload; // the brush slowly reloads
        }
        paint = {clampByte(carriedPaint[0]), clampByte(carriedPaint[1]), clampByte(carriedPaint[2]), color_.a};
    }

    // 3. Paint / erase, one tile at a time so per-tile lookups happen once.
    const double opacity = std::clamp(settings_.opacity, 0.0, 1.0);
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
                    const float coverage = coverage_[static_cast<std::size_t>((y - y0) * w + (x - x0))];
                    const auto i = static_cast<std::size_t>(TiledImage::pixelIndex(x, y));
                    float& m = (*st.mask)[i];
                    if (coverage <= m) {
                        continue;
                    }
                    m = coverage;
                    const Rgba8 original = st.original ? st.original->pixels[i] : Rgba8::transparent();
                    const double amount = m * opacity;
                    const Rgba8 out = mode_ == BrushMode::Paint ? blendPixel(original, paint, settings_.blendMode, amount)
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

// Smudge / Blur: works on the current pixels, so passes build up.
void BrushTool::retouchDab(TiledImage& image, const Dab& dab, int x0, int y0, int w, int h) {
    const float strength = static_cast<float>(std::clamp(settings_.opacity, 0.0, 1.0));
    // Blur radius grows with the tip (30 px tip -> 4 px box).
    const int r = mode_ == BrushMode::Blur ? std::clamp(static_cast<int>(std::lround(dab.diameter / 8.0)), 1, 6) : 0;
    // Source region: the dab box grown by the blur radius.
    const int gx0 = std::max(0, x0 - r), gy0 = std::max(0, y0 - r);
    const int gx1 = std::min(image.width() - 1, x0 + w - 1 + r), gy1 = std::min(image.height() - 1, y0 + h - 1 + r);
    const int gw = gx1 - gx0 + 1, gh = gy1 - gy0 + 1;
    const auto at = [&](int x, int y) { return static_cast<std::size_t>((y - gy0) * gw + (x - gx0)); };
    std::vector<Premul> src(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh));
    for (int y = gy0; y <= gy1; ++y) {
        for (int x = gx0; x <= gx1; ++x) {
            src[at(x, y)] = premultiply(image.pixel({x, y}));
        }
    }
    const auto write = [&](int x, int y, const Premul& v) {
        const Rgba8 out = unpremultiply(v);
        if (!(out == image.pixel({x, y}))) {
            image.setPixel({x, y}, out);
        }
    };

    if (mode_ == BrushMode::Blur) {
        // Box average (separable), over the pixels that exist.
        std::vector<Premul> horiz(src.size());
        std::vector<float> count(src.size());
        for (int y = gy0; y <= gy1; ++y) {
            for (int x = gx0; x <= gx1; ++x) {
                Premul sum{};
                int n = 0;
                for (int k = std::max(gx0, x - r); k <= std::min(gx1, x + r); ++k, ++n) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum[c] += src[at(k, y)][c];
                    }
                }
                horiz[at(x, y)] = sum;
                count[at(x, y)] = static_cast<float>(n);
            }
        }
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                const float cov = coverage_[static_cast<std::size_t>((y - y0) * w + (x - x0))] * strength;
                if (cov <= 0) {
                    continue;
                }
                Premul sum{};
                float n = 0;
                for (int k = std::max(gy0, y - r); k <= std::min(gy1, y + r); ++k) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum[c] += horiz[at(x, k)][c];
                    }
                    n += count[at(x, k)];
                }
                Premul out = src[at(x, y)];
                for (std::size_t c = 0; c < 4; ++c) {
                    out[c] += (sum[c] / n - out[c]) * cov;
                }
                write(x, y, out);
            }
        }
        return;
    }

    // Smudge: the tip carries colour; each dab blends it into the canvas and
    // picks some of the canvas up (strength = how far the colour travels).
    // Offset of each pixel centre from the dab centre, rounded symmetrically
    // (lround), so mirror copies index mirrored buffer cells.
    const int half = carriedSize_ / 2;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            const float cov = coverage_[static_cast<std::size_t>((y - y0) * w + (x - x0))];
            const int kx = static_cast<int>(std::lround(x + 0.5 - dab.center.x)) + half;
            const int ky = static_cast<int>(std::lround(y + 0.5 - dab.center.y)) + half;
            if (cov <= 0 || kx < 0 || ky < 0 || kx >= carriedSize_ || ky >= carriedSize_) {
                continue;
            }
            Premul& acc = carried_[copyIndex_][static_cast<std::size_t>(ky * carriedSize_ + kx)];
            const Premul& s = src[at(x, y)];
            if (!carriedLoaded_[copyIndex_]) {
                acc = s; // the first dab only picks the colour up
                continue;
            }
            Premul out = s;
            for (std::size_t c = 0; c < 4; ++c) {
                acc[c] = acc[c] * strength + s[c] * (1.0f - strength);
                out[c] += (acc[c] - s[c]) * cov;
            }
            write(x, y, out);
        }
    }
    carriedLoaded_[copyIndex_] = 1;
}

} // namespace gas
