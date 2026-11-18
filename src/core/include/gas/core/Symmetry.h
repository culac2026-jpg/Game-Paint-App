#pragma once

#include "gas/core/Geometry.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace gas {

// Mirror painting. Values may be stored in settings files: append only.
enum class SymmetryMode : std::uint8_t {
    Off,
    Vertical,     // mirror left <-> right across a vertical axis
    Horizontal,   // mirror top <-> bottom across a horizontal axis
    Quad,         // both axes (4 copies)
    Radial,       // `segments` rotated copies
    Kaleidoscope, // `segments` rotated copies, each also mirrored
};
inline constexpr int kSymmetryModeCount = 6;
std::string_view symmetryModeName(SymmetryMode mode);

// One copy of the stroke: optionally flipped across the vertical line through
// the centre, then rotated about the centre.
struct SymmetryCopy {
    double rotation = 0.0; // radians
    bool flip = false;

    PointF map(PointF p, PointF center) const;
    // Maps a direction / tip angle (radians, same convention as the canvas: y down).
    double mapAngle(double angle) const;
    // Multiples of 90 degrees map the pixel grid onto itself.
    bool gridAligned() const;
};

struct Symmetry {
    static constexpr int kMinSegments = 2;
    static constexpr int kMaxSegments = 64;

    SymmetryMode mode = SymmetryMode::Off;
    PointF center{0.0, 0.0}; // document space; on a pixel edge or centre for exact pixel mirroring
    int segments = 6;        // Radial / Kaleidoscope

    bool enabled() const { return mode != SymmetryMode::Off; }
    // Every copy of a stroke, the identity first (just the identity when off).
    std::vector<SymmetryCopy> copies() const;

    friend bool operator==(const Symmetry&, const Symmetry&) = default;
};

} // namespace gas
