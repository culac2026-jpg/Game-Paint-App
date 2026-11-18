#include "gas/core/Transform.h"

#include "gas/core/Blend.h"
#include "gas/core/TiledImage.h"

#include <cmath>

namespace gas {

Rgba8 FloatingImage::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= rect.w || y >= rect.h) {
        return Rgba8::transparent();
    }
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(rect.w) + static_cast<std::size_t>(x)];
}

namespace {

Rgba8 sampleBilinear(const FloatingImage& src, double u, double v) {
    // u, v in local pixel units; pixel centres sit at +0.5.
    const double fx = u - 0.5, fy = v - 0.5;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    const double ax = fx - ix, ay = fy - iy;
    const Rgba8 p00 = src.at(ix, iy), p10 = src.at(ix + 1, iy);
    const Rgba8 p01 = src.at(ix, iy + 1), p11 = src.at(ix + 1, iy + 1);
    double w[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
    const Rgba8* p[4] = {&p00, &p10, &p01, &p11};
    double a = 0, r = 0, g = 0, b = 0;
    for (int i = 0; i < 4; ++i) {
        const double pa = p[i]->a / 255.0 * w[i]; // premultiplied weights
        a += pa;
        r += p[i]->r * pa;
        g += p[i]->g * pa;
        b += p[i]->b * pa;
    }
    if (a <= 0.0) {
        return Rgba8::transparent();
    }
    const auto byte = [](double x) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0, 255.0)));
    };
    const std::uint8_t alpha = byte(a * 255.0);
    if (alpha == 0) {
        return Rgba8::transparent();
    }
    return {byte(r / a), byte(g / a), byte(b / a), alpha};
}

} // namespace

IRect transformInto(TiledImage& dst, const FloatingImage& src, const Affine& m, Interpolation interpolation) {
    if (src.rect.empty() || std::abs(m.determinant()) < 1e-12) {
        return {};
    }
    const IRect area = mappedBounds(m, src.rect).intersected({0, 0, dst.width(), dst.height()});
    if (area.empty()) {
        return {};
    }
    const Affine inv = m.inverted();
    for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
            const PointF s = inv.map({x + 0.5, y + 0.5});
            const double u = s.x - src.rect.x, v = s.y - src.rect.y;
            const Rgba8 c = interpolation == Interpolation::Nearest
                                ? src.at(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)))
                                : sampleBilinear(src, u, v);
            if (c.a == 0) {
                continue;
            }
            dst.setPixel({x, y}, c.a == 255 ? c : blendOver(dst.pixel({x, y}), c, 1.0));
        }
    }
    return area;
}

} // namespace gas
