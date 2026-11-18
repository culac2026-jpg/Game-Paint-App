#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gas {

namespace {

std::uint8_t toByte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

struct Rgb {
    double r, g, b;
};

double lum(Rgb c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

Rgb clipColor(Rgb c) {
    const double l = lum(c);
    const double n = std::min({c.r, c.g, c.b});
    const double x = std::max({c.r, c.g, c.b});
    if (n < 0.0) {
        c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)};
    }
    if (x > 1.0) {
        c = {l + (c.r - l) * (1 - l) / (x - l), l + (c.g - l) * (1 - l) / (x - l), l + (c.b - l) * (1 - l) / (x - l)};
    }
    return c;
}

Rgb setLum(Rgb c, double l) {
    const double d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}

double sat(Rgb c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

Rgb setSat(Rgb c, double s) {
    double* ch[3] = {&c.r, &c.g, &c.b};
    std::sort(ch, ch + 3, [](const double* a, const double* b) { return *a < *b; });
    double& mn = *ch[0];
    double& md = *ch[1];
    double& mx = *ch[2];
    if (mx > mn) {
        md = (md - mn) * s / (mx - mn);
        mx = s;
    } else {
        md = mx = 0.0;
    }
    mn = 0.0;
    return c;
}

// Separable blend function B(cb, cs) on straight 0..1 values.
double blendChannel(BlendMode mode, double cb, double cs) {
    switch (mode) {
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay: return blendChannel(BlendMode::HardLight, cs, cb);
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::ColorDodge:
        if (cb == 0.0) {
            return 0.0;
        }
        return cs >= 1.0 ? 1.0 : std::min(1.0, cb / (1.0 - cs));
    case BlendMode::ColorBurn:
        if (cb >= 1.0) {
            return 1.0;
        }
        return cs <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - cb) / cs);
    case BlendMode::HardLight:
        return cs <= 0.5 ? cb * 2.0 * cs : blendChannel(BlendMode::Screen, cb, 2.0 * cs - 1.0);
    case BlendMode::SoftLight: {
        if (cs <= 0.5) {
            return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        }
        const double d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : std::sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (d - cb);
    }
    case BlendMode::Difference: return std::abs(cb - cs);
    case BlendMode::Exclusion: return cb + cs - 2.0 * cb * cs;
    case BlendMode::LinearDodge: return std::min(1.0, cb + cs);
    case BlendMode::LinearBurn: return std::max(0.0, cb + cs - 1.0);
    default: return cs; // Normal (non-separable modes are handled separately)
    }
}

Rgb blendRgb(BlendMode mode, Rgb cb, Rgb cs) {
    switch (mode) {
    case BlendMode::Hue: return setLum(setSat(cs, sat(cb)), lum(cb));
    case BlendMode::Saturation: return setLum(setSat(cb, sat(cs)), lum(cb));
    case BlendMode::Color: return setLum(cs, lum(cb));
    case BlendMode::Luminosity: return setLum(cb, lum(cs));
    default:
        return {blendChannel(mode, cb.r, cs.r), blendChannel(mode, cb.g, cs.g), blendChannel(mode, cb.b, cs.b)};
    }
}

} // namespace

std::string_view blendModeName(BlendMode mode) {
    static constexpr std::string_view names[kBlendModeCount] = {
        "Normal",      "Multiply",   "Screen",     "Overlay",      "Darken",     "Lighten",
        "Color Dodge", "Color Burn", "Hard Light", "Soft Light",   "Difference", "Exclusion",
        "Add",         "Linear Burn", "Hue",       "Saturation",   "Color",      "Luminosity"};
    const auto i = static_cast<int>(mode);
    return i >= 0 && i < kBlendModeCount ? names[i] : "Normal";
}

Rgba8 blendPixel(Rgba8 dst, Rgba8 src, BlendMode mode, double coverage) {
    if (mode == BlendMode::Normal || dst.a == 0) {
        return blendOver(dst, src, coverage); // nothing to blend with: plain source-over
    }
    const double sa = (src.a / 255.0) * std::clamp(coverage, 0.0, 1.0);
    if (sa <= 0.0) {
        return dst;
    }
    const double da = dst.a / 255.0;
    const Rgb cs{src.r / 255.0, src.g / 255.0, src.b / 255.0};
    const Rgb cb{dst.r / 255.0, dst.g / 255.0, dst.b / 255.0};
    const Rgb b = blendRgb(mode, cb, cs);
    const double oa = sa + da * (1.0 - sa);
    // Spec: Cs' = (1 - ab) Cs + ab B(Cb, Cs), then source-over in premultiplied space.
    const auto channel = [&](double s, double d, double bl) {
        const double mixed = (1.0 - da) * s + da * std::clamp(bl, 0.0, 1.0);
        return toByte((sa * mixed + da * d * (1.0 - sa)) / oa * 255.0);
    };
    const std::uint8_t a = toByte(oa * 255.0);
    if (a == 0) {
        return Rgba8::transparent();
    }
    return {channel(cs.r, cb.r, b.r), channel(cs.g, cb.g, b.g), channel(cs.b, cb.b, b.b), a};
}

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
