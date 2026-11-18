#include "gas/core/StrokeSmoother.h"

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

} // namespace

std::vector<StrokeSample> streamlinePath(const std::vector<StrokeSample>& raw, double strength) {
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
    pts = resample(pts, 1.0);
    pts = gaussian(pts, radius);
    return gaussian(pts, radius); // a second pass irons out what the first leaves
}

} // namespace gas
