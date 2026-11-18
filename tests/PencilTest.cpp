#include <gas/core/Pencil.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <cstdlib>

using gas::PencilTool;
using gas::Point;
using gas::Rgba8;
using gas::TiledImage;

namespace {

int countPainted(const TiledImage& img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            n += img.pixel({x, y}).a != 0 ? 1 : 0;
        }
    }
    return n;
}

// Every painted pixel except the endpoints has a painted 8-neighbour on
// both sides, i.e. the stroke has no gaps.
bool isEightConnected(const TiledImage& img, Point from, Point to) {
    Point p = from;
    Point prev{-1000, -1000};
    while (p != to) {
        bool advanced = false;
        for (int dy = -1; dy <= 1 && !advanced; ++dy) {
            for (int dx = -1; dx <= 1 && !advanced; ++dx) {
                const Point n{p.x + dx, p.y + dy};
                if ((dx || dy) && n != prev && img.pixel(n).a != 0 &&
                    std::abs(to.x - n.x) + std::abs(to.y - n.y) <
                        std::abs(to.x - p.x) + std::abs(to.y - p.y)) {
                    prev = p;
                    p = n;
                    advanced = true;
                }
            }
        }
        if (!advanced) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(DrawLine, SinglePoint) {
    TiledImage img(8, 8);
    gas::drawLine(img, {3, 4}, {3, 4}, Rgba8::black());
    EXPECT_EQ(countPainted(img), 1);
    EXPECT_EQ(img.pixel({3, 4}), Rgba8::black());
}

TEST(DrawLine, HorizontalVerticalDiagonalAreInclusive) {
    TiledImage img(32, 32);
    gas::drawLine(img, {0, 0}, {9, 0}, Rgba8::black());
    EXPECT_EQ(countPainted(img), 10);

    TiledImage img2(32, 32);
    gas::drawLine(img2, {5, 20}, {5, 11}, Rgba8::black());
    EXPECT_EQ(countPainted(img2), 10);

    TiledImage img3(32, 32);
    gas::drawLine(img3, {0, 0}, {9, 9}, Rgba8::black());
    EXPECT_EQ(countPainted(img3), 10);
    EXPECT_EQ(img3.pixel({5, 5}), Rgba8::black());
}

TEST(DrawLine, SteepLineHasNoGaps) {
    TiledImage img(64, 64);
    gas::drawLine(img, {2, 60}, {13, 1}, Rgba8::black());
    EXPECT_EQ(countPainted(img), 60); // one pixel per major-axis step
    EXPECT_TRUE(isEightConnected(img, {2, 60}, {13, 1}));
}

TEST(DrawLine, ClipsOutsideImage) {
    TiledImage img(10, 10);
    gas::drawLine(img, {-5, 5}, {14, 5}, Rgba8::black());
    EXPECT_EQ(countPainted(img), 10);
}

TEST(PencilTool, FastMotionStaysConnected) {
    TiledImage img(300, 300);
    PencilTool pencil;
    // Sparse samples, as a fast mouse would deliver them.
    pencil.begin(img, {10, 10});
    pencil.moveTo(img, {120, 40});
    pencil.moveTo(img, {280, 270});
    pencil.end();

    EXPECT_FALSE(pencil.isStroking());
    EXPECT_TRUE(isEightConnected(img, {10, 10}, {120, 40}));
    EXPECT_TRUE(isEightConnected(img, {120, 40}, {280, 270}));
}

TEST(PencilTool, UsesSelectedColor) {
    TiledImage img(4, 4);
    PencilTool pencil;
    const Rgba8 green{0, 200, 0, 255};
    pencil.setColor(green);
    pencil.begin(img, {1, 1});
    pencil.end();
    EXPECT_EQ(img.pixel({1, 1}), green);
}
