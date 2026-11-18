#include <gas/core/Brush.h>
#include <gas/core/Pencil.h>
#include <gas/core/Symmetry.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <algorithm>

using gas::BrushTool;
using gas::PointF;
using gas::Rgba8;
using gas::Symmetry;
using gas::SymmetryMode;
using gas::TiledImage;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};

Symmetry sym(SymmetryMode mode, PointF center, int segments = 6) {
    Symmetry s;
    s.mode = mode;
    s.center = center;
    s.segments = segments;
    return s;
}

void expectNear(PointF a, PointF b) {
    EXPECT_NEAR(a.x, b.x, 1e-9);
    EXPECT_NEAR(a.y, b.y, 1e-9);
}

// Max channel difference between pixel (x, y) and its mapped partner.
template <typename Map>
int maxMirrorError(const TiledImage& img, Map map) {
    int worst = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const auto [mx, my] = map(x, y);
            const Rgba8 a = img.pixel({x, y}), b = img.pixel({mx, my});
            worst = std::max({worst, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
        }
    }
    return worst;
}

int painted(const TiledImage& img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            n += img.pixel({x, y}).a > 0 ? 1 : 0;
        }
    }
    return n;
}

} // namespace

TEST(Symmetry, Copies) {
    const PointF c{50, 50}, p{10, 20};
    EXPECT_EQ(sym(SymmetryMode::Off, c).copies().size(), 1u);

    auto v = sym(SymmetryMode::Vertical, c).copies();
    ASSERT_EQ(v.size(), 2u);
    expectNear(v[0].map(p, c), p);
    expectNear(v[1].map(p, c), {90, 20});

    auto h = sym(SymmetryMode::Horizontal, c).copies();
    ASSERT_EQ(h.size(), 2u);
    expectNear(h[1].map(p, c), {10, 80});
    EXPECT_EQ(h[1].map(p, c).y, 80.0); // exact, not just close

    auto q = sym(SymmetryMode::Quad, c).copies();
    ASSERT_EQ(q.size(), 4u);
    expectNear(q[3].map(p, c), {90, 80});

    auto r = sym(SymmetryMode::Radial, c, 4).copies();
    ASSERT_EQ(r.size(), 4u);
    expectNear(r[1].map({80, 50}, c), {50, 80}); // quarter turn (y down = clockwise on screen)
    EXPECT_TRUE(r[1].gridAligned());
    EXPECT_FALSE(sym(SymmetryMode::Radial, c, 6).copies()[1].gridAligned());

    EXPECT_EQ(sym(SymmetryMode::Kaleidoscope, c, 5).copies().size(), 10u);
    EXPECT_EQ(sym(SymmetryMode::Radial, c, 1000).copies().size(), static_cast<std::size_t>(Symmetry::kMaxSegments));

    // Angles: a rightward direction mirrors to leftward.
    EXPECT_NEAR(v[1].mapAngle(0.0), std::numbers::pi, 1e-12);
}

TEST(Symmetry, PencilMirrorsExactly) {
    TiledImage img(100, 60);
    gas::PencilTool pencil;
    pencil.setColor(kRed);
    pencil.setSymmetry(sym(SymmetryMode::Vertical, {50, 30}));
    pencil.begin(img, {10.5, 10.5});
    pencil.moveTo(img, {37.2, 23.9});
    pencil.moveTo(img, {44.0, 50.0});
    pencil.end();
    EXPECT_EQ(maxMirrorError(img, [](int x, int y) { return std::pair{99 - x, y}; }), 0);
    EXPECT_GT(painted(img), 80);

    // Odd width, axis through a pixel centre.
    TiledImage odd(41, 41);
    pencil.setSymmetry(sym(SymmetryMode::Quad, {20.5, 20.5}));
    pencil.begin(odd, {3.5, 5.5});
    pencil.moveTo(odd, {17.5, 12.5});
    pencil.end();
    EXPECT_EQ(maxMirrorError(odd, [](int x, int y) { return std::pair{40 - x, y}; }), 0);
    EXPECT_EQ(maxMirrorError(odd, [](int x, int y) { return std::pair{x, 40 - y}; }), 0);
}

TEST(Symmetry, PencilRadialHasNoGaps) {
    TiledImage img(101, 101);
    gas::PencilTool pencil;
    pencil.setColor(kRed);
    pencil.setSymmetry(sym(SymmetryMode::Radial, {50.5, 50.5}, 6));
    pencil.begin(img, {55, 50});
    pencil.moveTo(img, {95, 50});
    pencil.end();
    // Each of the 6 rays is a connected line of ~40 px.
    EXPECT_GE(painted(img), 6 * 36); // rays share a few pixels near the centre
}

TEST(Symmetry, BrushMirrorsShapeAngleAndJitter) {
    TiledImage img(120, 80);
    BrushTool brush;
    brush.setColor(kRed);
    auto& s = brush.settings();
    s.diameter = 14;
    s.shape = gas::BrushShape::Triangle; // not symmetric about its own x axis
    s.roundness = 0.6;
    s.angle = 30;
    s.angleJitter = 0.3;
    s.sizeJitter = 0.4;
    s.scatter = 0.5;
    s.spacing = 0.5;
    brush.setSymmetry(sym(SymmetryMode::Vertical, {60, 40}));
    brush.begin(img, {15, 20});
    brush.moveTo(img, {40, 60});
    brush.moveTo(img, {50, 15});
    brush.end();
    EXPECT_LE(maxMirrorError(img, [](int x, int y) { return std::pair{119 - x, y}; }), 2);

    TiledImage img2(120, 80);
    brush.setSymmetry(sym(SymmetryMode::Horizontal, {60, 40}));
    brush.begin(img2, {15, 20});
    brush.moveTo(img2, {100, 30});
    brush.end();
    EXPECT_LE(maxMirrorError(img2, [](int x, int y) { return std::pair{x, 79 - y}; }), 2);
}

TEST(Symmetry, OverlappingCopiesDoNotStack) {
    TiledImage img(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 10;
    brush.settings().hardness = 1.0;
    brush.settings().opacity = 0.5;
    brush.setSymmetry(sym(SymmetryMode::Vertical, {50, 50}));
    brush.begin(img, {50, 20}); // on the axis: both copies cover the same pixels
    brush.moveTo(img, {50, 80});
    brush.end();
    EXPECT_NEAR(img.pixel({50, 50}).a, 128, 1);
}

TEST(Symmetry, RadialBrushAndSmudge) {
    TiledImage img(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 6;
    brush.settings().hardness = 1.0;
    brush.setSymmetry(sym(SymmetryMode::Radial, {50, 50}, 4));
    brush.begin(img, {80, 50});
    brush.end();
    for (const auto [x, y] : {std::pair{80, 50}, {50, 80}, {20, 50}, {50, 20}}) {
        EXPECT_EQ(img.pixel({x, y}), kRed) << x << "," << y;
    }

    // Smudge keeps a carried buffer per copy: result is still mirror-symmetric.
    TiledImage art(100, 60);
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 100; ++x) {
            art.setPixel({x, y}, (std::min(x, 99 - x) / 10 + y / 10) % 2 ? kRed : Rgba8{0, 0, 255, 255}); // mirror-symmetric
        }
    }
    BrushTool smudge(gas::BrushMode::Smudge);
    smudge.settings().diameter = 12;
    smudge.settings().opacity = 0.8;
    smudge.setSymmetry(sym(SymmetryMode::Vertical, {50, 30}));
    smudge.begin(art, {12, 15});
    smudge.moveTo(art, {38, 44});
    smudge.end();
    EXPECT_LE(maxMirrorError(art, [](int x, int y) { return std::pair{99 - x, y}; }), 2);
}
