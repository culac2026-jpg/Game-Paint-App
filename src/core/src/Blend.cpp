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

int colorDistance(Rgba8 a, Rgba8 b) {
    if (a.a == 0 && b.a == 0) {
        return 0;
    }
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b),
                     std::abs(a.a - b.a)});
}

} // namespace gas
