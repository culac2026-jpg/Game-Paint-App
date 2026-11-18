#include <gas/core/Brush.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <numbers>

using gas::BrushMode;
using gas::BrushTool;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};

double totalCoverage(const TiledImage& img) {
    double sum = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            sum += img.pixel({x, y}).a / 255.0;
        }
    }
    return sum;
}

int maxAlpha(const TiledImage& img) {
    int m = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            m = std::max(m, static_cast<int>(img.pixel({x, y}).a));
        }
    }
    return m;
}

BrushTool makeBrush(double diameter, double hardness, double opacity, BrushMode mode = BrushMode::Paint) {
    BrushTool b(mode);
    b.settings().diameter = diameter;
    b.settings().hardness = hardness;
    b.settings().opacity = opacity;
    b.setColor(kRed);
    return b;
}

void fillOpaque(TiledImage& img, Rgba8 c) {
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            img.setPixel({x, y}, c);
        }
    }
}

} // namespace

TEST(DabCoverage, HardEdgeIsAntiAliased) {
    EXPECT_DOUBLE_EQ(gas::dabCoverage(0.0, 10.0, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(gas::dabCoverage(5.0, 10.0, 1.0), 0.5);
    EXPECT_DOUBLE_EQ(gas::dabCoverage(6.0, 10.0, 1.0), 0.0);
}

TEST(DabCoverage, SoftBrushFallsOffMonotonically) {
    double prev = 1.0;
    for (double d = 0.0; d <= 6.0; d += 0.25) {
        const double c = gas::dabCoverage(d, 10.0, 0.0);
        EXPECT_LE(c, prev + 1e-12);
        prev = c;
    }
    EXPECT_LT(gas::dabCoverage(4.0, 10.0, 0.0), gas::dabCoverage(4.0, 10.0, 1.0));
}

TEST(Brush, SingleHardDabCoversCircleArea) {
    TiledImage img(64, 64);
    BrushTool b = makeBrush(20.0, 1.0, 1.0);
    b.begin(img, {32.0, 32.0});
    b.end();
    EXPECT_EQ(img.pixel({31, 31}), kRed);
    EXPECT_EQ(img.pixel({32, 50}).a, 0);
    EXPECT_NEAR(totalCoverage(img), std::numbers::pi * 10.0 * 10.0, 3.0);
}

TEST(Brush, StrokeNeverExceedsOpacityWhenCrossingItself) {
    TiledImage img(128, 128);
    BrushTool b = makeBrush(16.0, 1.0, 0.5);
    b.begin(img, {20.0, 64.0});
    b.moveTo(img, {108.0, 64.0});
    b.moveTo(img, {64.0, 20.0});
    b.moveTo(img, {64.0, 108.0}); // crosses the first segment
    b.end();
    EXPECT_NEAR(maxAlpha(img), 128, 1);
}

TEST(Brush, SeparateStrokesDoBuildUp) {
    TiledImage img(64, 64);
    BrushTool b = makeBrush(16.0, 1.0, 0.5);
    b.begin(img, {32.0, 32.0});
    b.end();
    b.begin(img, {32.0, 32.0});
    b.end();
    EXPECT_NEAR(img.pixel({32, 32}).a, 191, 1); // 1 - 0.5 * 0.5 = 0.75
}

TEST(Brush, FastMotionLeavesNoGaps) {
    TiledImage img(400, 100);
    BrushTool b = makeBrush(4.0, 1.0, 1.0);
    b.begin(img, {10.5, 50.5});
    b.moveTo(img, {390.5, 50.5}); // one huge jump, like a fast mouse
    b.end();
    for (int x = 10; x <= 390; ++x) {
        ASSERT_EQ(img.pixel({x, 50}).a, 255) << "gap at x=" << x;
    }
}

TEST(Brush, SpacingCarriesAcrossTinySegments) {
    // Many sub-pixel moves must produce the same stroke as one move.
    TiledImage a(100, 40);
    TiledImage b(100, 40);
    BrushTool one = makeBrush(6.0, 1.0, 1.0);
    one.begin(a, {10.0, 20.0});
    one.moveTo(a, {90.0, 20.0});
    one.end();
    BrushTool many = makeBrush(6.0, 1.0, 1.0);
    many.begin(b, {10.0, 20.0});
    for (double x = 10.1; x <= 90.0 + 1e-9; x += 0.1) {
        many.moveTo(b, {x, 20.0});
    }
    many.end();
    EXPECT_NEAR(totalCoverage(a), totalCoverage(b), 2.0);
}

TEST(Brush, DabsOffCanvasAreClipped) {
    TiledImage img(16, 16);
    BrushTool b = makeBrush(20.0, 1.0, 1.0);
    b.begin(img, {-5.0, -5.0});
    b.moveTo(img, {-100.0, 300.0});
    b.end();
    EXPECT_EQ(img.pixel({0, 0}), kRed);
    EXPECT_EQ(img.pixel({15, 15}).a, 0);
}

TEST(Eraser, FullOpacityClearsToTransparent) {
    TiledImage img(32, 32);
    fillOpaque(img, kRed);
    BrushTool e = makeBrush(10.0, 1.0, 1.0, BrushMode::Erase);
    e.begin(img, {16.0, 16.0});
    e.end();
    EXPECT_EQ(img.pixel({16, 16}), Rgba8::transparent());
    EXPECT_EQ(img.pixel({0, 0}), kRed);
}

TEST(Eraser, PartialOpacityIsCappedPerStroke) {
    TiledImage img(64, 64);
    fillOpaque(img, kRed);
    BrushTool e = makeBrush(12.0, 1.0, 0.5, BrushMode::Erase);
    e.begin(img, {10.0, 32.0});
    e.moveTo(img, {54.0, 32.0});
    e.moveTo(img, {10.0, 32.0}); // back over the same pixels
    e.end();
    const Rgba8 p = img.pixel({32, 32});
    EXPECT_NEAR(p.a, 128, 1);
    EXPECT_EQ(p.r, 255); // colour preserved
}
