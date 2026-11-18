#include "gas/core/Fill.h"

#include "gas/core/Blend.h"
#include "gas/core/TiledImage.h"

#include <vector>

namespace gas {

long long floodFill(TiledImage& image, Point seed, Rgba8 color, int tolerance) {
    if (!image.contains(seed)) {
        return 0;
    }
    const Rgba8 target = image.pixel(seed);
    if (tolerance <= 0 && colorDistance(target, color) == 0) {
        return 0; // nothing would change
    }

    const int w = image.width();
    const int h = image.height();
    std::vector<bool> visited(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), false);
    const auto idx = [w](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    };
    // Only unvisited pixels are tested, and those still hold their original colour.
    const auto matches = [&](int x, int y) {
        return !visited[idx(x, y)] && colorDistance(image.pixel({x, y}), target) <= tolerance;
    };

    long long changed = 0;
    std::vector<Point> stack{seed};
    while (!stack.empty()) {
        const Point p = stack.back();
        stack.pop_back();
        if (!matches(p.x, p.y)) {
            continue;
        }
        // Grow the horizontal span containing p.
        int left = p.x;
        while (left > 0 && matches(left - 1, p.y)) {
            --left;
        }
        int right = p.x;
        while (right < w - 1 && matches(right + 1, p.y)) {
            ++right;
        }
        for (int x = left; x <= right; ++x) {
            visited[idx(x, p.y)] = true;
            image.setPixel({x, p.y}, color);
        }
        changed += right - left + 1;

        // Queue one seed per run of matching pixels in the rows above and below.
        for (const int ny : {p.y - 1, p.y + 1}) {
            if (ny < 0 || ny >= h) {
                continue;
            }
            bool inRun = false;
            for (int x = left; x <= right; ++x) {
                const bool m = matches(x, ny);
                if (m && !inRun) {
                    stack.push_back({x, ny});
                }
                inRun = m;
            }
        }
    }
    return changed;
}

std::optional<Rgba8> pickColor(const TiledImage& image, Point p) {
    if (!image.contains(p)) {
        return std::nullopt;
    }
    const Rgba8 c = image.pixel(p);
    if (c.a == 0) {
        return std::nullopt;
    }
    return c;
}

} // namespace gas
