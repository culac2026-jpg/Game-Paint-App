#include "gas/core/Slicing.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace gas {

namespace {

constexpr int T = TiledImage::kTileSize;

bool solid(const TiledImage::Tile* t, int x, int y, int threshold) {
    return t && t->pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))].a > threshold;
}

// Whether a and b are closer than d px (overlapping boxes count).
bool near(const IRect& a, const IRect& b, int d) {
    return a.x - d < b.right() && b.x - d < a.right() && a.y - d < b.bottom() && b.y - d < a.bottom();
}

} // namespace

IRect trimmedRect(const TiledImage& image, IRect rect, int alphaThreshold) {
    rect = rect.intersected({0, 0, image.width(), image.height()});
    if (rect.empty()) {
        return {};
    }
    int x0 = rect.right(), y0 = rect.bottom(), x1 = rect.x - 1, y1 = rect.y - 1;
    for (int ty = rect.y / T; ty * T < rect.bottom(); ++ty) {
        for (int tx = rect.x / T; tx * T < rect.right(); ++tx) {
            const TiledImage::Tile* t = image.tile(tx, ty);
            if (!t) {
                continue; // empty tiles are skipped wholesale
            }
            const IRect area = rect.intersected({tx * T, ty * T, T, T});
            for (int y = area.y; y < area.bottom(); ++y) {
                for (int x = area.x; x < area.right(); ++x) {
                    if (solid(t, x, y, alphaThreshold)) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
                }
            }
        }
    }
    return x1 < x0 ? IRect{} : IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::vector<IRect> gridSlices(const TiledImage& image, const GridSliceOptions& o) {
    std::vector<IRect> out;
    if (o.cellWidth <= 0 || o.cellHeight <= 0) {
        return out;
    }
    const int stepX = o.cellWidth + std::max(0, o.spacingX);
    const int stepY = o.cellHeight + std::max(0, o.spacingY);
    for (int y = std::max(0, o.offsetY); y + o.cellHeight <= image.height(); y += stepY) {
        for (int x = std::max(0, o.offsetX); x + o.cellWidth <= image.width(); x += stepX) {
            const IRect cell{x, y, o.cellWidth, o.cellHeight};
            if (!o.skipEmpty || !trimmedRect(image, cell, o.alphaThreshold).empty()) {
                out.push_back(cell);
            }
        }
    }
    return out;
}

std::vector<IRect> autoSlices(const TiledImage& image, const AutoSliceOptions& o) {
    const IRect bounds = trimmedRect(image, {0, 0, image.width(), image.height()}, o.alphaThreshold);
    std::vector<IRect> boxes;
    if (bounds.empty()) {
        return boxes;
    }
    // Flood-fill labelling inside the content bounds (8-connected).
    const auto w = static_cast<std::size_t>(bounds.w);
    const auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x); };
    std::vector<std::uint8_t> state(w * static_cast<std::size_t>(bounds.h), 0); // 0 empty, 1 solid, 2 visited
    for (int y = bounds.y; y < bounds.bottom(); ++y) {
        for (int x = bounds.x; x < bounds.right(); ++x) {
            if (solid(image.tile(x / T, y / T), x, y, o.alphaThreshold)) {
                state[at(x - bounds.x, y - bounds.y)] = 1;
            }
        }
    }
    std::vector<Point> stack;
    for (int y = 0; y < bounds.h; ++y) {
        for (int x = 0; x < bounds.w; ++x) {
            if (state[at(x, y)] != 1) {
                continue;
            }
            state[at(x, y)] = 2;
            stack.assign(1, {x, y});
            int x0 = x, x1 = x, y0 = y, y1 = y, count = 0;
            while (!stack.empty()) {
                const Point p = stack.back();
                stack.pop_back();
                ++count;
                x0 = std::min(x0, p.x);
                x1 = std::max(x1, p.x);
                y0 = std::min(y0, p.y);
                y1 = std::max(y1, p.y);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = p.x + dx, ny = p.y + dy;
                        if (nx >= 0 && ny >= 0 && nx < bounds.w && ny < bounds.h && state[at(nx, ny)] == 1) {
                            state[at(nx, ny)] = 2;
                            stack.push_back({nx, ny});
                        }
                    }
                }
            }
            if (count >= std::max(1, o.minPixels)) {
                boxes.push_back({bounds.x + x0, bounds.y + y0, x1 - x0 + 1, y1 - y0 + 1});
            }
        }
    }
    // Merge boxes within mergeDistance until nothing changes (a merged box can reach new neighbours).
    const int d = std::max(0, o.mergeDistance) + 1;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            for (std::size_t j = i + 1; j < boxes.size();) {
                if (near(boxes[i], boxes[j], d)) {
                    boxes[i] = boxes[i].united(boxes[j]);
                    boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
                    changed = true;
                } else {
                    ++j;
                }
            }
        }
    }
    sortReadingOrder(boxes);
    return boxes;
}

void sortReadingOrder(std::vector<IRect>& rects) {
    std::sort(rects.begin(), rects.end(), [](const IRect& a, const IRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    // Greedy rows: a rect joins the current row while its vertical centre is above the row's bottom.
    std::size_t start = 0;
    while (start < rects.size()) {
        int bottom = rects[start].bottom();
        std::size_t end = start + 1;
        while (end < rects.size() && 2 * rects[end].y + rects[end].h < 2 * bottom) {
            bottom = std::max(bottom, rects[end].bottom());
            ++end;
        }
        std::sort(rects.begin() + static_cast<std::ptrdiff_t>(start), rects.begin() + static_cast<std::ptrdiff_t>(end),
                  [](const IRect& a, const IRect& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
        start = end;
    }
}

std::vector<Slice> namedSlices(const std::vector<IRect>& rects, const std::string& base, PointF pivot,
                               int firstIndex) {
    std::vector<Slice> out;
    out.reserve(rects.size());
    const int last = firstIndex + static_cast<int>(rects.size()) - 1;
    const std::size_t digits = std::max<std::size_t>(2, std::to_string(std::max(0, last)).size());
    for (std::size_t i = 0; i < rects.size(); ++i) {
        std::string number = std::to_string(firstIndex + static_cast<int>(i));
        number.insert(0, digits - std::min(digits, number.size()), '0');
        out.push_back({base + "_" + number, rects[i], pivot, {}});
    }
    return out;
}

std::string uniqueName(const std::string& name, const std::vector<std::string>& taken) {
    const std::unordered_set<std::string> set(taken.begin(), taken.end());
    if (!set.contains(name)) {
        return name;
    }
    for (int k = 2;; ++k) {
        std::string candidate = name + "_" + std::to_string(k);
        if (!set.contains(candidate)) {
            return candidate;
        }
    }
}

std::vector<Rgba8> copyPixels(const TiledImage& image, IRect rect) {
    const auto w = static_cast<std::size_t>(std::max(0, rect.w));
    std::vector<Rgba8> out(w * static_cast<std::size_t>(std::max(0, rect.h)));
    const IRect inside = rect.intersected({0, 0, image.width(), image.height()});
    for (int y = inside.y; y < inside.bottom(); ++y) {
        for (int x = inside.x; x < inside.right(); ++x) {
            const TiledImage::Tile* t = image.tile(x / T, y / T);
            if (t) {
                out[static_cast<std::size_t>(y - rect.y) * w + static_cast<std::size_t>(x - rect.x)] =
                    t->pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))];
            }
        }
    }
    return out;
}

namespace {

// Source column (or row) for output position `o` along one axis.
int nineSliceSource(int o, int out, int size, int a, int b, bool tile) {
    if (a + b > out && a + b > 0) { // corners only, scaled down together
        const int ca = static_cast<int>(std::lround(static_cast<double>(a) * out / (a + b)));
        if (o < ca) {
            return std::min(a - 1, o * a / std::max(1, ca));
        }
        const int cb = std::max(1, out - ca); // mirrored, so both corners keep their outer pixels
        return size - 1 - std::min(b - 1, (out - 1 - o) * b / cb);
    }
    if (o < a) {
        return o;
    }
    if (o >= out - b) {
        return size - (out - o);
    }
    const int mid = size - a - b, outMid = out - a - b;
    if (mid <= 0) {
        return std::clamp(a, 0, size - 1);
    }
    return a + (tile ? (o - a) % mid : static_cast<int>(static_cast<long long>(o - a) * mid / outMid));
}

} // namespace

std::vector<Rgba8> nineSliceScale(const std::vector<Rgba8>& src, int w, int h, SliceBorder border, int outW,
                                  int outH, bool tile) {
    std::vector<Rgba8> out;
    if (w <= 0 || h <= 0 || outW <= 0 || outH <= 0 ||
        src.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        return out;
    }
    const int left = std::clamp(border.left, 0, w), right = std::clamp(border.right, 0, w - left);
    const int top = std::clamp(border.top, 0, h), bottom = std::clamp(border.bottom, 0, h - top);
    std::vector<int> xs(static_cast<std::size_t>(outW));
    for (int x = 0; x < outW; ++x) {
        xs[static_cast<std::size_t>(x)] = std::clamp(nineSliceSource(x, outW, w, left, right, tile), 0, w - 1);
    }
    out.resize(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH));
    for (int y = 0; y < outH; ++y) {
        const int sy = std::clamp(nineSliceSource(y, outH, h, top, bottom, tile), 0, h - 1);
        for (int x = 0; x < outW; ++x) {
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) + static_cast<std::size_t>(x)] =
                src[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(xs[static_cast<std::size_t>(x)])];
        }
    }
    return out;
}

} // namespace gas
