#include "gas/core/StrokeSmoother.h"

#include <algorithm>
#include <cmath>

namespace gas {

namespace {

// Points every `step` px along the polyline (pressure interpolated), ending on the last point.
std::vector<StrokeSample> resample(const std::vector<StrokeSample>& in, double step) {
    std::vector<StrokeSample> out{in.front()};
    double carry = 0.0; // distance travelled since the last output point
    for (std::size_t i = 1; i < in.size(); ++i) {
        const StrokeSample& a = in[i - 1];
        const StrokeSample& b = in[i];
        const double len = std::hypot(b.pos.x - a.pos.x, b.pos.y - a.pos.y);
        double t = step - carry;
        while (t <= len) {
            const double f = t / len;
            out.push_back({{a.pos.x + (b.pos.x - a.pos.x) * f, a.pos.y + (b.pos.y - a.pos.y) * f},
                           a.pressure + (b.pressure - a.pressure) * f});
            t += step;
        }
        carry = len - (t - step);
    }
    const StrokeSample& last = in.back();
    if (out.back().pos.x != last.pos.x || out.back().pos.y != last.pos.y) {
        out.push_back(last);
    }
    return out;
}

// One Gaussian pass; the window is cut symmetrically near the ends so they stay put.
std::vector<StrokeSample> gaussian(const std::vector<StrokeSample>& in, int radius) {
    const int n = static_cast<int>(in.size());
    const double sigma = std::max(1.0, radius / 2.0);
    std::vector<double> weight(static_cast<std::size_t>(radius) + 1);
    for (int k = 0; k <= radius; ++k) {
        weight[static_cast<std::size_t>(k)] = std::exp(-0.5 * (k / sigma) * (k / sigma));
    }
    std::vector<StrokeSample> out(in.size());
    for (int i = 0; i < n; ++i) {
        const int w = std::min({radius, i, n - 1 - i});
        double sx = 0, sy = 0, sp = 0, sw = 0;
        for (int k = -w; k <= w; ++k) {
            const double wk = weight[static_cast<std::size_t>(std::abs(k))];
            const StrokeSample& s = in[static_cast<std::size_t>(i + k)];
            sx += wk * s.pos.x;
            sy += wk * s.pos.y;
            sp += wk * s.pressure;
            sw += wk;
        }
        out[static_cast<std::size_t>(i)] = {{sx / sw, sy / sw}, sp / sw};
    }
    return out;
}

// One Gaussian pass over a closed loop (indices wrap), so there is no seam.
std::vector<StrokeSample> gaussianCyclic(const std::vector<StrokeSample>& in, int radius) {
    const int n = static_cast<int>(in.size());
    radius = std::min(radius, (n - 1) / 2);
    const double sigma = std::max(1.0, radius / 2.0);
    std::vector<StrokeSample> out(in.size());
    for (int i = 0; i < n; ++i) {
        double sx = 0, sy = 0, sp = 0, sw = 0;
        for (int k = -radius; k <= radius; ++k) {
            const double wk = std::exp(-0.5 * (k / sigma) * (k / sigma));
            const StrokeSample& s = in[static_cast<std::size_t>(((i + k) % n + n) % n)];
            sx += wk * s.pos.x;
            sy += wk * s.pos.y;
            sp += wk * s.pressure;
            sw += wk;
        }
        out[static_cast<std::size_t>(i)] = {{sx / sw, sy / sw}, sp / sw};
    }
    return out;
}

double distance(const PointF& a, const PointF& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// If the stroke comes back to where it started (a closed shape), returns the
// index of the last point to keep: the tail point nearest the start, so a
// small overshoot past the start is dropped. Returns 0 for an open stroke.
std::size_t closingPoint(const std::vector<StrokeSample>& pts, double closeDistance) {
    const PointF start = pts.front().pos;
    double length = 0, farthest = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        length += distance(pts[i - 1].pos, pts[i].pos);
        farthest = std::max(farthest, distance(pts[i].pos, start));
    }
    if (farthest < 3 * closeDistance || length < 8 * closeDistance) {
        return 0; // too small to be a deliberate loop
    }
    // Only the last quarter of the stroke (by length) may close it.
    std::size_t best = 0;
    double bestDist = closeDistance, travelled = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        travelled += distance(pts[i - 1].pos, pts[i].pos);
        if (travelled < 0.75 * length) {
            continue;
        }
        const double d = distance(pts[i].pos, start);
        if (d <= bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

} // namespace

std::vector<StrokeSample> streamlinePath(const std::vector<StrokeSample>& raw, double strength,
                                         double closeDistance) {
    strength = std::clamp(strength, 0.0, 1.0);
    std::vector<StrokeSample> pts;
    for (const StrokeSample& s : raw) {
        if (pts.empty() || s.pos.x != pts.back().pos.x || s.pos.y != pts.back().pos.y) {
            pts.push_back(s);
        }
    }
    const int radius = static_cast<int>(std::lround(strength * 40.0));
    if (pts.size() < 3 || radius < 1) {
        return raw;
    }
    // A stroke that ends where it began closes the shape: smooth it as a loop
    // so the join is as smooth as the rest, with no kink or gap.
    closeDistance = std::max(closeDistance, 0.5 * radius);
    if (const std::size_t last = closingPoint(pts, closeDistance)) {
        pts.resize(last + 1);
        pts.push_back(pts.front()); // the closing segment, resampled like the others
        pts = resample(pts, 1.0);
        pts.pop_back();             // == front: the loop wraps instead
        if (pts.size() >= 3) {
            pts = gaussianCyclic(gaussianCyclic(pts, radius), radius);
            pts.push_back(pts.front()); // end exactly on the start
            return pts;
        }
    }
    pts = resample(pts, 1.0);
    pts = gaussian(pts, radius);
    return gaussian(pts, radius); // a second pass irons out what the first leaves
}

} // namespace gas
