#include <gas/core/Brush.h>
#include <gas/core/Geometry.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <cmath>

using gas::PointF;

TEST(StraightLine, SnapAngle) {
    const PointF a{10, 10};
    // Nearly horizontal snaps to exactly horizontal, keeping the length.
    PointF p = gas::snapAngle(a, {50, 12}, 15);
    EXPECT_EQ(p.y, 10.0);
    EXPECT_NEAR(p.x, 10 + std::hypot(40, 2), 1e-9);
    // Nearly vertical (down).
    p = gas::snapAngle(a, {11, 60}, 15);
    EXPECT_EQ(p.x, 10.0);
    // 45 degrees.
    p = gas::snapAngle(a, {40, 38}, 15);
    EXPECT_NEAR(p.x - a.x, p.y - a.y, 1e-9);
    // 30 degrees up-left stays 30 degrees.
    p = gas::snapAngle(a, {10 - 30, 10 - 17.6}, 15);
    EXPECT_NEAR(std::atan2(p.y - a.y, p.x - a.x) * 180 / 3.14159265358979, -150, 1e-9);
    // Degenerate input is returned unchanged.
    EXPECT_EQ(gas::snapAngle(a, a, 15).x, a.x);
}

TEST(StraightLine, BrushLineIsStraight) {
    // A straight brush line (begin + one moveTo) has no gaps and stays on its row.
    gas::TiledImage img(300, 40);
    gas::BrushTool brush;
    brush.settings().diameter = 6;
    brush.settings().hardness = 1.0;
    const PointF a{10, 20.5};
    const PointF b = gas::snapAngle(a, {290, 22}, 15);
    brush.begin(img, a);
    brush.moveTo(img, b);
    brush.end();
    for (int x = 12; x < 285; ++x) {
        ASSERT_EQ(img.pixel({x, 20}).a, 255) << x;
        ASSERT_EQ(img.pixel({x, 20 - 5}).a, img.pixel({x, 20 + 5}).a) << x; // symmetric about the row
    }
}
