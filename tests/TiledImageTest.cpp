#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <stdexcept>

using gas::Point;
using gas::Rgba8;
using gas::TiledImage;

TEST(TiledImage, TileGridRoundsUp) {
    TiledImage img(257, 512);
    EXPECT_EQ(img.tilesX(), 2);
    EXPECT_EQ(img.tilesY(), 2);
}

TEST(TiledImage, RejectsNonPositiveSize) {
    EXPECT_THROW(TiledImage(0, 10), std::invalid_argument);
    EXPECT_THROW(TiledImage(10, -1), std::invalid_argument);
}

TEST(TiledImage, UnwrittenPixelsAreTransparentAndUnallocated) {
    TiledImage img(512, 512);
    EXPECT_EQ(img.pixel({100, 300}), Rgba8::transparent());
    EXPECT_EQ(img.tile(0, 1), nullptr);
}

TEST(TiledImage, SetPixelRoundTripsAcrossTiles) {
    TiledImage img(600, 600);
    const Rgba8 red{255, 0, 0, 255};
    const Rgba8 halfBlue{0, 0, 255, 128};
    img.setPixel({0, 0}, red);
    img.setPixel({599, 599}, halfBlue);
    img.setPixel({256, 255}, red);
    EXPECT_EQ(img.pixel({0, 0}), red);
    EXPECT_EQ(img.pixel({599, 599}), halfBlue);
    EXPECT_EQ(img.pixel({256, 255}), red);
    EXPECT_EQ(img.pixel({255, 255}), Rgba8::transparent());
}

TEST(TiledImage, OutOfBoundsIsIgnored) {
    TiledImage img(10, 10);
    img.setPixel({-1, 0}, Rgba8::black());
    img.setPixel({10, 5}, Rgba8::black());
    EXPECT_EQ(img.pixel({-1, 0}), Rgba8::transparent());
    EXPECT_TRUE(img.takeDirtyTiles().empty());
}

TEST(TiledImage, DirtyTilesAreUniqueAndDrained) {
    TiledImage img(1024, 1024);
    img.setPixel({1, 1}, Rgba8::black());
    img.setPixel({2, 2}, Rgba8::black());
    img.setPixel({700, 300}, Rgba8::black());

    const auto dirty = img.takeDirtyTiles();
    ASSERT_EQ(dirty.size(), 2u);
    EXPECT_EQ(dirty[0], (Point{0, 0}));
    EXPECT_EQ(dirty[1], (Point{2, 1}));
    EXPECT_TRUE(img.takeDirtyTiles().empty());

    img.setPixel({3, 3}, Rgba8::black());
    EXPECT_EQ(img.takeDirtyTiles().size(), 1u);
}
