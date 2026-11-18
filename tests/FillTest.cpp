#include <gas/core/Fill.h>
#include <gas/core/Pencil.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

using gas::Point;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};
const Rgba8 kBlue{0, 0, 255, 255};

int countColor(const TiledImage& img, Rgba8 c) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            n += img.pixel({x, y}) == c ? 1 : 0;
        }
    }
    return n;
}

} // namespace

TEST(FloodFill, FillsEmptyCanvasAcrossTiles) {
    TiledImage img(300, 280);
    EXPECT_EQ(gas::floodFill(img, {5, 5}, kRed), 300LL * 280LL);
    EXPECT_EQ(countColor(img, kRed), 300 * 280);
}

TEST(FloodFill, DoesNotLeakThroughDiagonalLines) {
    // A diamond outline drawn with the pencil is only 8-connected; a
    // 4-connected fill must stay inside it.
    TiledImage img(64, 64);
    const Point top{32, 4}, right{60, 32}, bottom{32, 60}, left{4, 32};
    gas::drawLine(img, top, right, Rgba8::black());
    gas::drawLine(img, right, bottom, Rgba8::black());
    gas::drawLine(img, bottom, left, Rgba8::black());
    gas::drawLine(img, left, top, Rgba8::black());

    gas::floodFill(img, {32, 32}, kRed);
    EXPECT_EQ(img.pixel({32, 32}), kRed);
    EXPECT_EQ(img.pixel({0, 0}).a, 0);
    EXPECT_EQ(img.pixel({63, 63}).a, 0);
    EXPECT_EQ(img.pixel({8, 8}).a, 0);
}

TEST(FloodFill, ReplacesOnlyContiguousRegion) {
    TiledImage img(20, 10);
    gas::drawLine(img, {10, 0}, {10, 9}, Rgba8::black()); // wall
    gas::floodFill(img, {2, 2}, kRed);
    EXPECT_EQ(countColor(img, kRed), 10 * 10);
    EXPECT_EQ(img.pixel({15, 5}).a, 0);
}

TEST(FloodFill, ToleranceIncludesSimilarColours) {
    const auto makeRamp = [] {
        TiledImage img(10, 1);
        for (int x = 0; x < 10; ++x) {
            const auto v = static_cast<std::uint8_t>(100 + x * 5); // 100,105,...,145
            img.setPixel({x, 0}, {v, v, v, 255});
        }
        return img;
    };
    TiledImage strict = makeRamp();
    TiledImage loose = makeRamp();
    EXPECT_EQ(gas::floodFill(strict, {0, 0}, kBlue, 0), 1);
    EXPECT_EQ(gas::floodFill(loose, {0, 0}, kBlue, 20), 5); // 100..120
}

TEST(FloodFill, SameColourAndOutOfBoundsAreNoOps) {
    TiledImage img(8, 8);
    gas::floodFill(img, {0, 0}, kRed);
    EXPECT_EQ(gas::floodFill(img, {3, 3}, kRed), 0);
    EXPECT_EQ(gas::floodFill(img, {-1, 3}, kBlue), 0);
    EXPECT_EQ(gas::floodFill(img, {8, 0}, kBlue), 0);
}

TEST(FloodFill, RefillWithToleranceTerminates) {
    // New colour within tolerance of the target must not loop forever.
    TiledImage img(50, 50);
    gas::floodFill(img, {0, 0}, Rgba8{100, 100, 100, 255});
    EXPECT_EQ(gas::floodFill(img, {0, 0}, Rgba8{101, 100, 100, 255}, 10), 2500);
}

TEST(PickColor, ReturnsOpaqueAndSkipsTransparent) {
    TiledImage img(4, 4);
    img.setPixel({1, 2}, kRed);
    EXPECT_EQ(gas::pickColor(img, {1, 2}), kRed);
    EXPECT_FALSE(gas::pickColor(img, {0, 0}).has_value());
    EXPECT_FALSE(gas::pickColor(img, {-1, 0}).has_value());
}
