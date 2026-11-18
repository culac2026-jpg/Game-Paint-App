#include "gas/core/Effects.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gas {

namespace {

constexpr int kTile = TiledImage::kTileSize;

using Plane = std::vector<float>;

int boxRadius(double size) { return static_cast<int>(std::ceil(std::max(0.0, size) / 3.0)); }
int ceilPx(double v) { return static_cast<int>(std::ceil(std::abs(v))); }

bool isFilter(EffectType t) { return t == EffectType::ColorAdjust || t == EffectType::Blur || t == EffectType::Pixelate; }

// A working region in premultiplied float planes.
struct Region {
    IRect rect;
    Plane r, g, b, a;

    explicit Region(IRect rc) : rect(rc) {
        const auto n = static_cast<std::size_t>(rc.w) * static_cast<std::size_t>(rc.h);
        r.assign(n, 0.0f);
        g.assign(n, 0.0f);
        b.assign(n, 0.0f);
        a.assign(n, 0.0f);
    }
    std::size_t size() const { return a.size(); }
    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y - rect.y) * static_cast<std::size_t>(rect.w) +
               static_cast<std::size_t>(x - rect.x);
    }
};

// Three passes of a running-sum box blur ~ Gaussian. Outside the region is 0.
void boxBlur(Plane& p, int w, int h, int radius) {
    if (radius <= 0) {
        return;
    }
    Plane tmp(p.size());
    const float norm = 1.0f / static_cast<float>(2 * radius + 1);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) { // horizontal: p -> tmp
            const float* row = &p[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)];
            float* dst = &tmp[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)];
            double sum = 0.0;
            for (int x = 0; x <= std::min(radius, w - 1); ++x) {
                sum += row[x];
            }
            for (int x = 0; x < w; ++x) {
                dst[x] = static_cast<float>(sum) * norm;
                if (x + radius + 1 < w) {
                    sum += row[x + radius + 1];
                }
                if (x - radius >= 0) {
                    sum -= row[x - radius];
                }
            }
        }
        for (int x = 0; x < w; ++x) { // vertical: tmp -> p
            double sum = 0.0;
            const auto at = [&](int y) -> float& { return tmp[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
            for (int y = 0; y <= std::min(radius, h - 1); ++y) {
                sum += at(y);
            }
            for (int y = 0; y < h; ++y) {
                p[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                    static_cast<float>(sum) * norm;
                if (y + radius + 1 < h) {
                    sum += at(y + radius + 1);
                }
                if (y - radius >= 0) {
                    sum -= at(y - radius);
                }
            }
        }
    }
}

// 1-D squared Euclidean distance transform (Felzenszwalb & Huttenlocher).
void edt1d(const std::vector<double>& f, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z, int n) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
        if (f[static_cast<std::size_t>(q)] == inf) {
            continue;
        }
        if (f[static_cast<std::size_t>(v[0])] == inf) {
            v[0] = q;
            continue;
        }
        double s;
        for (;;) {
            const int vk = v[static_cast<std::size_t>(k)];
            s = ((f[static_cast<std::size_t>(q)] + q * q) - (f[static_cast<std::size_t>(vk)] + vk * vk)) / (2.0 * (q - vk));
            if (s <= z[static_cast<std::size_t>(k)] && k > 0) {
                --k;
            } else {
                break;
            }
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k) + 1] = inf;
    }
    if (f[static_cast<std::size_t>(v[0])] == inf) {
        std::fill(d.begin(), d.begin() + n, inf);
        return;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k) + 1] < q) {
            ++k;
        }
        const int vk = v[static_cast<std::size_t>(k)];
        d[static_cast<std::size_t>(q)] = (q - vk) * (q - vk) + f[static_cast<std::size_t>(vk)];
    }
}

// Distance (px, between pixel centres) from each pixel to the nearest pixel with alpha >= 0.5.
std::vector<float> distanceToContent(const Plane& alpha, int w, int h) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const int n = std::max(w, h);
    std::vector<double> grid(alpha.size()), f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)),
        z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        grid[i] = alpha[i] >= 0.5f ? 0.0 : inf;
    }
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y * w + x)];
        }
        edt1d(f, d, v, z, h);
        for (int y = 0; y < h; ++y) {
            grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(y)];
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            f[static_cast<std::size_t>(x)] = grid[static_cast<std::size_t>(y * w + x)];
        }
        edt1d(f, d, v, z, w);
        for (int x = 0; x < w; ++x) {
            grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(x)];
        }
    }
    std::vector<float> out(alpha.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(std::sqrt(grid[i]));
    }
    return out;
}

// Alpha plane sampled at (x - dx, y - dy); `outside` where that falls out of the region.
Plane shifted(const Plane& a, int w, int h, int dx, int dy, float outside) {
    Plane out(a.size(), outside);
    for (int y = 0; y < h; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) {
            continue;
        }
        for (int x = 0; x < w; ++x) {
            const int sx = x - dx;
            if (sx >= 0 && sx < w) {
                out[static_cast<std::size_t>(y * w + x)] = a[static_cast<std::size_t>(sy * w + sx)];
            }
        }
    }
    return out;
}

float hueToRgb(float p, float q, float t) {
    if (t < 0) {
        t += 1;
    }
    if (t > 1) {
        t -= 1;
    }
    if (t < 1.0f / 6) {
        return p + (q - p) * 6 * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3) {
        return p + (q - p) * (2.0f / 3 - t) * 6;
    }
    return p;
}

void adjustColor(float& r, float& g, float& b, const Effect& e) {
    if (e.hue != 0.0 || e.saturation != 0.0 || e.lightness != 0.0) {
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        float h = 0, s = 0, l = (mx + mn) / 2;
        if (mx > mn) {
            const float d = mx - mn;
            s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
            if (mx == r) {
                h = (g - b) / d + (g < b ? 6 : 0);
            } else if (mx == g) {
                h = (b - r) / d + 2;
            } else {
                h = (r - g) / d + 4;
            }
            h /= 6;
        }
        h += static_cast<float>(e.hue / 360.0);
        h -= std::floor(h);
        s = std::clamp(s * static_cast<float>(1.0 + e.saturation), 0.0f, 1.0f);
        const auto L = static_cast<float>(e.lightness);
        l = L >= 0 ? l + (1 - l) * L : l * (1 + L);
        if (s <= 0) {
            r = g = b = l;
        } else {
            const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            const float p = 2 * l - q;
            r = hueToRgb(p, q, h + 1.0f / 3);
            g = hueToRgb(p, q, h);
            b = hueToRgb(p, q, h - 1.0f / 3);
        }
    }
    if (e.contrast != 0.0) {
        const auto k = static_cast<float>(e.contrast >= 0 ? 1.0 / (1.0 - 0.99 * e.contrast) : 1.0 + e.contrast);
        r = std::clamp((r - 0.5f) * k + 0.5f, 0.0f, 1.0f);
        g = std::clamp((g - 0.5f) * k + 0.5f, 0.0f, 1.0f);
        b = std::clamp((b - 0.5f) * k + 0.5f, 0.0f, 1.0f);
    }
}

void applyFilter(Region& reg, const Effect& e) {
    const int w = reg.rect.w, h = reg.rect.h;
    switch (e.type) {
    case EffectType::ColorAdjust:
        for (std::size_t i = 0; i < reg.size(); ++i) {
            const float a = reg.a[i];
            if (a <= 0) {
                continue;
            }
            float r = reg.r[i] / a, g = reg.g[i] / a, b = reg.b[i] / a;
            adjustColor(r, g, b, e);
            reg.r[i] = r * a;
            reg.g[i] = g * a;
            reg.b[i] = b * a;
        }
        break;
    case EffectType::Blur: {
        const int radius = boxRadius(e.size);
        for (Plane* p : {&reg.r, &reg.g, &reg.b, &reg.a}) {
            boxBlur(*p, w, h, radius);
        }
        break;
    }
    case EffectType::Pixelate: {
        const int cell = std::max(1, static_cast<int>(std::lround(e.size)));
        // Cells are aligned to the canvas origin, so results don't depend on the region.
        const int cx0 = static_cast<int>(std::floor(static_cast<double>(reg.rect.x) / cell));
        const int cy0 = static_cast<int>(std::floor(static_cast<double>(reg.rect.y) / cell));
        for (int cy = cy0; cy * cell < reg.rect.bottom(); ++cy) {
            for (int cx = cx0; cx * cell < reg.rect.right(); ++cx) {
                const IRect c = IRect{cx * cell, cy * cell, cell, cell}.intersected(reg.rect);
                double sr = 0, sg = 0, sb = 0, sa = 0;
                for (int y = c.y; y < c.bottom(); ++y) {
                    for (int x = c.x; x < c.right(); ++x) {
                        const std::size_t i = reg.index(x, y);
                        sr += reg.r[i];
                        sg += reg.g[i];
                        sb += reg.b[i];
                        sa += reg.a[i];
                    }
                }
                const double n = static_cast<double>(c.w) * c.h;
                for (int y = c.y; y < c.bottom(); ++y) {
                    for (int x = c.x; x < c.right(); ++x) {
                        const std::size_t i = reg.index(x, y);
                        reg.r[i] = static_cast<float>(sr / n);
                        reg.g[i] = static_cast<float>(sg / n);
                        reg.b[i] = static_cast<float>(sb / n);
                        reg.a[i] = static_cast<float>(sa / n);
                    }
                }
            }
        }
        break;
    }
    default: break;
    }
}

// dst = src over dst, both premultiplied; src colour is constant `c` (straight 0..1) with alpha `a`.
void overColor(Region& dst, std::size_t i, const float c[3], float a) {
    if (a <= 0) {
        return;
    }
    const float k = 1 - a;
    dst.r[i] = c[0] * a + dst.r[i] * k;
    dst.g[i] = c[1] * a + dst.g[i] * k;
    dst.b[i] = c[2] * a + dst.b[i] * k;
    dst.a[i] = a + dst.a[i] * k;
}

void colorOf(const Effect& e, float c[3], float& alpha) {
    c[0] = e.color.r / 255.0f;
    c[1] = e.color.g / 255.0f;
    c[2] = e.color.b / 255.0f;
    alpha = static_cast<float>(e.opacity) * (e.color.a / 255.0f);
}

std::uint8_t toByte(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// Renders one region whose central part `keep` is written to `out`.
TiledImage::TilePtr renderTile(const TiledImage& source, const TiledImage* mask, const std::vector<Effect>& effects,
                               IRect keep, IRect read) {
    Region content(read);
    bool any = false;
    for (int ty = read.y / kTile; ty <= (read.bottom() - 1) / kTile; ++ty) {
        for (int tx = read.x / kTile; tx <= (read.right() - 1) / kTile; ++tx) {
            const TiledImage::Tile* t = source.tile(tx, ty);
            if (!t) {
                continue;
            }
            const TiledImage::Tile* m = mask ? mask->tile(tx, ty) : nullptr;
            const IRect part = read.intersected({tx * kTile, ty * kTile, kTile, kTile});
            for (int y = part.y; y < part.bottom(); ++y) {
                for (int x = part.x; x < part.right(); ++x) {
                    const auto pi = static_cast<std::size_t>(TiledImage::pixelIndex(x, y));
                    const Rgba8 p = t->pixels[pi];
                    if (p.a == 0) {
                        continue;
                    }
                    float a = p.a / 255.0f;
                    if (m) {
                        a *= maskValue(m->pixels[pi]) / 255.0f;
                    }
                    const std::size_t i = content.index(x, y);
                    content.r[i] = p.r / 255.0f * a;
                    content.g[i] = p.g / 255.0f * a;
                    content.b[i] = p.b / 255.0f * a;
                    content.a[i] = a;
                    any = any || a > 0;
                }
            }
        }
    }
    if (!any) {
        return nullptr;
    }
    for (const Effect& e : effects) {
        if (e.enabled && isFilter(e.type)) {
            applyFilter(content, e);
        }
    }

    const int w = read.w, h = read.h;
    bool needDistance = false;
    for (const Effect& e : effects) {
        needDistance = needDistance || (e.enabled && (e.type == EffectType::Outline || e.type == EffectType::OuterGlow));
    }
    std::vector<float> dist;
    if (needDistance) {
        dist = distanceToContent(content.a, w, h);
    }

    Region out(read);
    const auto layer = [&](EffectType type) {
        for (const Effect& e : effects) {
            if (!e.enabled || e.type != type) {
                continue;
            }
            float c[3], alpha;
            colorOf(e, c, alpha);
            if (alpha <= 0) {
                continue;
            }
            if (type == EffectType::DropShadow) {
                Plane s = shifted(content.a, w, h, static_cast<int>(std::lround(e.offsetX)),
                                  static_cast<int>(std::lround(e.offsetY)), 0.0f);
                boxBlur(s, w, h, boxRadius(e.size));
                for (std::size_t i = 0; i < s.size(); ++i) {
                    overColor(out, i, c, s[i] * alpha);
                }
            } else if (type == EffectType::OuterGlow) {
                const auto size = static_cast<float>(std::max(e.size, 0.01));
                for (std::size_t i = 0; i < dist.size(); ++i) {
                    if (dist[i] < size) {
                        const float t = 1 - dist[i] / size;
                        overColor(out, i, c, t * t * alpha);
                    }
                }
            } else if (type == EffectType::Outline) {
                const auto size = static_cast<float>(e.size);
                for (std::size_t i = 0; i < dist.size(); ++i) {
                    const float cov = e.smooth ? std::clamp(size + 0.5f - dist[i], 0.0f, 1.0f)
                                               : (dist[i] <= size + 1e-4f ? 1.0f : 0.0f);
                    overColor(out, i, c, cov * alpha);
                }
            }
        }
    };
    layer(EffectType::DropShadow);
    layer(EffectType::OuterGlow);
    layer(EffectType::Outline);

    // Content, with colour overlays.
    for (const Effect& e : effects) {
        if (!e.enabled || e.type != EffectType::ColorOverlay) {
            continue;
        }
        float c[3], k;
        colorOf(e, c, k);
        for (std::size_t i = 0; i < content.size(); ++i) {
            const float a = content.a[i];
            content.r[i] += (c[0] * a - content.r[i]) * k;
            content.g[i] += (c[1] * a - content.g[i]) * k;
            content.b[i] += (c[2] * a - content.b[i]) * k;
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float k = 1 - content.a[i];
        out.r[i] = content.r[i] + out.r[i] * k;
        out.g[i] = content.g[i] + out.g[i] * k;
        out.b[i] = content.b[i] + out.b[i] * k;
        out.a[i] = content.a[i] + out.a[i] * k;
    }

    for (const Effect& e : effects) {
        if (!e.enabled || e.type != EffectType::InnerShadow) {
            continue;
        }
        float c[3], alpha;
        colorOf(e, c, alpha);
        Plane inv(content.a.size());
        for (std::size_t i = 0; i < inv.size(); ++i) {
            inv[i] = 1 - content.a[i];
        }
        Plane s = shifted(inv, w, h, static_cast<int>(std::lround(e.offsetX)), static_cast<int>(std::lround(e.offsetY)),
                          1.0f);
        boxBlur(s, w, h, boxRadius(e.size));
        for (std::size_t i = 0; i < s.size(); ++i) {
            overColor(out, i, c, std::min(1.0f, s[i]) * content.a[i] * alpha); // clipped to the content
        }
    }

    auto tile = std::make_shared<TiledImage::Tile>();
    bool painted = false;
    for (int y = keep.y; y < keep.bottom(); ++y) {
        for (int x = keep.x; x < keep.right(); ++x) {
            const std::size_t i = out.index(x, y);
            const std::uint8_t a8 = toByte(out.a[i]);
            if (a8 == 0) {
                continue;
            }
            const float a = out.a[i];
            tile->pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))] = {toByte(out.r[i] / a), toByte(out.g[i] / a),
                                                                                   toByte(out.b[i] / a), a8};
            painted = true;
        }
    }
    return painted ? TiledImage::TilePtr(std::move(tile)) : nullptr;
}

} // namespace

std::string_view effectName(EffectType type) {
    static constexpr std::string_view names[kEffectTypeCount] = {
        "Color Adjust", "Blur", "Pixelate", "Outline", "Drop Shadow", "Inner Shadow", "Outer Glow", "Color Overlay"};
    const auto i = static_cast<int>(type);
    return i >= 0 && i < kEffectTypeCount ? names[i] : "Effect";
}

Effect Effect::make(EffectType type) {
    Effect e;
    e.type = type;
    switch (type) {
    case EffectType::ColorAdjust: break;
    case EffectType::Blur: e.size = 3; break;
    case EffectType::Pixelate: e.size = 4; break;
    case EffectType::Outline: e.size = 1; break;
    case EffectType::DropShadow:
        e.opacity = 0.6;
        e.size = 4;
        e.offsetX = e.offsetY = 4;
        break;
    case EffectType::InnerShadow:
        e.opacity = 0.5;
        e.size = 3;
        e.offsetX = e.offsetY = 2;
        break;
    case EffectType::OuterGlow:
        e.color = {255, 230, 120, 255};
        e.opacity = 0.8;
        e.size = 8;
        break;
    case EffectType::ColorOverlay: e.color = {255, 255, 255, 255}; break;
    }
    return e;
}

Effect sanitized(Effect e) {
    const auto fix = [](double v, double lo, double hi, double fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    if (static_cast<int>(e.type) >= kEffectTypeCount) {
        e.type = EffectType::Outline;
    }
    e.opacity = fix(e.opacity, 0, 1, 1);
    e.size = fix(e.size, 0, kMaxEffectSize, 1);
    e.offsetX = fix(e.offsetX, -kMaxEffectOffset, kMaxEffectOffset, 0);
    e.offsetY = fix(e.offsetY, -kMaxEffectOffset, kMaxEffectOffset, 0);
    e.hue = fix(e.hue, -180, 180, 0);
    e.saturation = fix(e.saturation, -1, 1, 0);
    e.lightness = fix(e.lightness, -1, 1, 0);
    e.contrast = fix(e.contrast, -1, 1, 0);
    return e;
}

bool hasActiveEffects(const std::vector<Effect>& effects) {
    return std::any_of(effects.begin(), effects.end(), [](const Effect& e) { return e.enabled; });
}

int effectMargin(const std::vector<Effect>& effects) {
    int margin = 0;
    for (const Effect& e : effects) {
        if (!e.enabled) {
            continue;
        }
        switch (e.type) {
        case EffectType::ColorAdjust:
        case EffectType::ColorOverlay: break;
        case EffectType::Blur: margin += 3 * boxRadius(e.size); break;
        case EffectType::Pixelate: margin += std::max(1, static_cast<int>(std::lround(e.size))); break;
        case EffectType::Outline:
        case EffectType::OuterGlow: margin += ceilPx(e.size) + 1; break;
        case EffectType::DropShadow:
        case EffectType::InnerShadow:
            margin += std::max(ceilPx(e.offsetX), ceilPx(e.offsetY)) + 1 + 3 * boxRadius(e.size);
            break;
        }
    }
    return margin;
}

void renderEffects(const TiledImage& source, const TiledImage* mask, const std::vector<Effect>& effects,
                   IRect tileArea, TiledImage& out) {
    const int margin = effectMargin(effects);
    const IRect canvas{0, 0, source.width(), source.height()};
    for (int ty = tileArea.y; ty < tileArea.bottom(); ++ty) {
        for (int tx = tileArea.x; tx < tileArea.right(); ++tx) {
            const IRect keep = IRect{tx * kTile, ty * kTile, kTile, kTile}.intersected(canvas);
            if (keep.empty()) {
                continue;
            }
            const IRect read = IRect{keep.x - margin, keep.y - margin, keep.w + 2 * margin, keep.h + 2 * margin}
                                   .intersected(canvas);
            out.setTile(tx, ty, renderTile(source, mask, effects, keep, read));
        }
    }
}

} // namespace gas
