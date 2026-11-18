#include <gas/core/Viewport.h>

#include <gtest/gtest.h>

using gas::PointF;
using gas::Viewport;

TEST(Viewport, RoundTripsCoordinates) {
    Viewport v;
    v.setZoom(3.5);
    v.setOffset({40.0, -12.0});
    const PointF doc{17.25, 99.0};
    const PointF back = v.screenToDoc(v.docToScreen(doc));
    EXPECT_DOUBLE_EQ(back.x, doc.x);
    EXPECT_DOUBLE_EQ(back.y, doc.y);
}

TEST(Viewport, ZoomAtKeepsAnchorFixed) {
    Viewport v;
    v.setOffset({100.0, 50.0});
    const PointF anchor{320.0, 240.0};
    const PointF before = v.screenToDoc(anchor);
    v.zoomAt(anchor, 4.0);
    const PointF after = v.screenToDoc(anchor);
    EXPECT_DOUBLE_EQ(v.zoom(), 4.0);
    EXPECT_NEAR(after.x, before.x, 1e-9);
    EXPECT_NEAR(after.y, before.y, 1e-9);
}

TEST(Viewport, ZoomIsClamped) {
    Viewport v;
    v.setZoom(1000.0);
    EXPECT_DOUBLE_EQ(v.zoom(), Viewport::kMaxZoom);
    v.zoomAt({0, 0}, 1e-9);
    EXPECT_DOUBLE_EQ(v.zoom(), Viewport::kMinZoom);
}

TEST(Viewport, FitCentersDocument) {
    Viewport v;
    v.fit(2000, 1000, 1032.0, 1032.0, 16.0);
    EXPECT_DOUBLE_EQ(v.zoom(), 0.5);
    const PointF topLeft = v.docToScreen({0, 0});
    const PointF bottomRight = v.docToScreen({2000, 1000});
    EXPECT_DOUBLE_EQ(topLeft.x, 16.0);
    EXPECT_DOUBLE_EQ(topLeft.y, 266.0);
    EXPECT_DOUBLE_EQ(bottomRight.x, 1016.0);
    EXPECT_DOUBLE_EQ(bottomRight.y, 766.0);
}

TEST(Viewport, PixelUnderCursorUsesFloor) {
    Viewport v;
    v.setZoom(8.0);
    // Screen x=-1 is doc -0.125 -> pixel -1, not 0.
    EXPECT_EQ(gas::toPixel(v.screenToDoc({-1.0, 7.9})), (gas::Point{-1, 0}));
    EXPECT_EQ(gas::toPixel(v.screenToDoc({8.0, 8.0})), (gas::Point{1, 1}));
}
