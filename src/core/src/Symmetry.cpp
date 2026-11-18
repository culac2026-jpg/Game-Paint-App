#include "gas/core/Symmetry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gas {

namespace {

// cos/sin with exact values at multiples of 90 degrees, so axis mirrors map
// pixel centres onto pixel centres without rounding noise.
void cosSin(double angle, double& cs, double& sn) {
    cs = std::cos(angle);
    sn = std::sin(angle);
    for (double* v : {&cs, &sn}) {
        if (std::abs(*v) < 1e-12) {
            *v = 0.0;
        } else if (std::abs(std::abs(*v) - 1.0) < 1e-12) {
            *v = *v > 0 ? 1.0 : -1.0;
        }
    }
}

} // namespace

std::string_view symmetryModeName(SymmetryMode mode) {
    static constexpr std::string_view names[kSymmetryModeCount] = {"Off",  "Left | Right", "Top | Bottom",
                                                                   "4-way", "Radial",      "Kaleidoscope"};
    const auto i = static_cast<int>(mode);
    return i >= 0 && i < kSymmetryModeCount ? names[i] : "Off";
}

PointF SymmetryCopy::map(PointF p, PointF center) const {
    double dx = p.x - center.x;
    const double dy = p.y - center.y;
    if (flip) {
        dx = -dx;
    }
    double cs, sn;
    cosSin(rotation, cs, sn);
    return {center.x + dx * cs - dy * sn, center.y + dx * sn + dy * cs};
}

double SymmetryCopy::mapAngle(double angle) const { return (flip ? std::numbers::pi - angle : angle) + rotation; }

bool SymmetryCopy::gridAligned() const {
    const double quarter = rotation / (std::numbers::pi / 2.0);
    return std::abs(quarter - std::round(quarter)) < 1e-9;
}

std::vector<SymmetryCopy> Symmetry::copies() const {
    constexpr double pi = std::numbers::pi;
    const int n = std::clamp(segments, kMinSegments, kMaxSegments);
    std::vector<SymmetryCopy> out{SymmetryCopy{}};
    switch (mode) {
    case SymmetryMode::Off: break;
    case SymmetryMode::Vertical: out.push_back({0.0, true}); break;
    // Flip across the vertical axis, then a half turn = flip across the horizontal axis.
    case SymmetryMode::Horizontal: out.push_back({pi, true}); break;
    case SymmetryMode::Quad:
        out.push_back({0.0, true});
        out.push_back({pi, true});
        out.push_back({pi, false});
        break;
    case SymmetryMode::Radial:
    case SymmetryMode::Kaleidoscope:
        for (int k = 1; k < n; ++k) {
            out.push_back({2.0 * pi * k / n, false});
        }
        if (mode == SymmetryMode::Kaleidoscope) {
            for (int k = 0; k < n; ++k) {
                out.push_back({2.0 * pi * k / n, true});
            }
        }
        break;
    }
    return out;
}

} // namespace gas
