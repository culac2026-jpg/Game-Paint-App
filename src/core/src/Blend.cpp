#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gas {

namespace {

std::uint8_t toByte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

} // namespace

Rgba8 blendOver(Rgba8 dst, Rgba8 src, double coverage) {
    const double sa = (src.a / 255.0) * std::clamp(coverage, 0.0, 1.0);
    if (sa <= 0.0) {
        return dst;
    }
    const double da = dst.a / 255.0;
    const double oa = sa + da * (1.0 - sa);
    const auto channel = [&](std::uint8_t s, std::uint8_t d) {
        // Premultiply, composite, un-premultiply.
        return toByte((s * sa + d * da * (1.0 - sa)) / oa);
    };
    const std::uint8_t a = toByte(oa * 255.0);
    if (a == 0) {
        return Rgba8::transparent();
    }
    return {channel(src.r, dst.r), channel(src.g, dst.g), channel(src.b, dst.b), a};
}

Rgba8 eraseAlpha(Rgba8 dst, double amount) {
    const std::uint8_t a = toByte(dst.a * (1.0 - std::clamp(amount, 0.0, 1.0)));
    if (a == 0) {
        return Rgba8::transparent();
    }
    return {dst.r, dst.g, dst.b, a};
}

Rgba8 mix(Rgba8 a, Rgba8 b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (t <= 0.0) {
        return a;
    }
    if (t >= 1.0) {
        return b;
    }
    const double wa = (a.a / 255.0) * (1.0 - t);
    const double wb = (b.a / 255.0) * t;
    const double oa = wa + wb;
    const std::uint8_t alpha = toByte(oa * 255.0);
    if (alpha == 0) {
        return Rgba8::transparent();
    }
    const auto channel = [&](std::uint8_t ca, std::uint8_t cb) { return toByte((ca * wa + cb * wb) / oa); };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), alpha};
}

std::uint8_t maskValue(Rgba8 p) {
    if (p.a == 0) {
        return 255;
    }
    const int lum = (299 * p.r + 587 * p.g + 114 * p.b + 500) / 1000;
    return static_cast<std::uint8_t>((lum * p.a + 255 * (255 - p.a) + 127) / 255);
}

int colorDistance(Rgba8 a, Rgba8 b) {
    if (a.a == 0 && b.a == 0) {
        return 0;
    }
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b),
                     std::abs(a.a - b.a)});
}

} // namespace gas
