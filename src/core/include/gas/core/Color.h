#pragma once

#include <cstdint>

namespace gas {

// 8-bit RGBA, straight (non-premultiplied) alpha, byte order R,G,B,A.
// The straight vs. premultiplied policy for compositing/export is still open
// (see CLAUDE.md); v0.1 tools only overwrite pixels, so no blending happens yet.
struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;

    friend bool operator==(const Rgba8&, const Rgba8&) = default;

    static constexpr Rgba8 transparent() { return {0, 0, 0, 0}; }
    static constexpr Rgba8 black() { return {0, 0, 0, 255}; }
};

static_assert(sizeof(Rgba8) == 4, "Rgba8 must be tightly packed");

} // namespace gas
