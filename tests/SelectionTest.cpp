#include <gas/core/Selection.h>

#include <gtest/gtest.h>

#include <numbers>

using gas::IRect;
using gas::Selection;

namespace {

int countAtLeast(const Selection& s, int threshold) {
    int n = 0;
    for (int y = 0; y < s.height(); ++y) {
        for (int x = 0; x < s.width(); ++x) {
            n += s.coverage({x, y}) >= threshold ? 1 : 0;
        }
    }
    return n;
}

double totalCoverage(const Selection& s) {
    double sum = 0;
    for (int y = 0; y < s.height(); ++y) {
        for (int x = 0; x < s.width(); ++x) {
            sum += s.coverage({x, y}) / 255.0;
        }
    }
    return sum;
}

} // namespace

TEST(Selection, DefaultAndNewAreEmpty) {
    EXPECT_TRUE(Selection().isEmpty());
    Selection s(100, 50);
    EXPECT_TRUE(s.isEmpty());
    EXPECT_FALSE(s.bounds().has_value());
    EXPECT_EQ(s.coverage({10, 10}), 0);
}

TEST(Selection, AllCoversExactlyTheCanvas) {
    const Selection s = Selection::all(300, 260); // edge tiles are partial
    EXPECT_EQ(s.bounds(), (IRect{0, 0, 300, 260}));
    EXPECT_EQ(s.coverage({299, 259}), 255);
    EXPECT_EQ(s.coverage({300, 0}), 0);
    EXPECT_EQ(countAtLeast(s, 255), 300 * 260);
}

TEST(Selection, RectIsExactAndClipped) {
    const Selection s = Selection::rect(600, 600, {250, 10, 300, 20});
    EXPECT_EQ(s.bounds(), (IRect{250, 10, 300, 20}));
    EXPECT_EQ(countAtLeast(s, 255), 300 * 20);
    EXPECT_EQ(countAtLeast(s, 1), 300 * 20); // hard edges
    const Selection clipped = Selection::rect(100, 100, {-10, -10, 30, 30});
    EXPECT_EQ(clipped.bounds(), (IRect{0, 0, 20, 20}));
    EXPECT_TRUE(Selection::rect(100, 100, {200, 200, 10, 10}).isEmpty());
}

TEST(Selection, EllipseAreaAndAntialiasing) {
    const Selection aa = Selection::ellipse(200, 200, {20, 20, 160, 100}, true);
    EXPECT_NEAR(totalCoverage(aa), std::numbers::pi * 80 * 50, 80);
    EXPECT_EQ(aa.coverage({100, 70}), 255); // centre
    EXPECT_EQ(aa.coverage({21, 21}), 0);    // corner of the bounding box
    EXPECT_GT(countAtLeast(aa, 1) - countAtLeast(aa, 255), 50); // soft edge pixels exist

    const Selection hard = Selection::ellipse(200, 200, {20, 20, 160, 100}, false);
    EXPECT_EQ(countAtLeast(hard, 1), countAtLeast(hard, 255)); // only 0 or 255
    EXPECT_NEAR(countAtLeast(hard, 255), std::numbers::pi * 80 * 50, 120);
}

TEST(Selection, LassoPolygonFillsTriangle) {
    const Selection tri = Selection::polygon(100, 100, {{10, 10}, {90, 10}, {10, 90}}, true);
    EXPECT_NEAR(totalCoverage(tri), 80.0 * 80.0 / 2.0, 20);
    EXPECT_EQ(tri.coverage({20, 20}), 255);
    EXPECT_EQ(tri.coverage({80, 80}), 0);
    EXPECT_TRUE(Selection::polygon(100, 100, {{1, 1}, {5, 5}}, true).isEmpty()); // < 3 points
}

TEST(Selection, CombineOps) {
    const Selection a = Selection::rect(40, 10, {0, 0, 20, 10});
    const Selection b = Selection::rect(40, 10, {10, 0, 20, 10});
    EXPECT_EQ(a.combined(b, Selection::Op::Add).bounds(), (IRect{0, 0, 30, 10}));
    EXPECT_EQ(a.combined(b, Selection::Op::Subtract).bounds(), (IRect{0, 0, 10, 10}));
    EXPECT_EQ(a.combined(b, Selection::Op::Intersect).bounds(), (IRect{10, 0, 10, 10}));
    EXPECT_EQ(a.combined(b, Selection::Op::Replace), b);
    EXPECT_TRUE(a.combined(a, Selection::Op::Subtract).isEmpty());
}

TEST(Selection, InvertIsExactComplement) {
    const Selection s = Selection::rect(300, 300, {100, 100, 50, 50});
    const Selection inv = s.inverted();
    EXPECT_EQ(inv.coverage({120, 120}), 0);
    EXPECT_EQ(inv.coverage({0, 0}), 255);
    EXPECT_EQ(countAtLeast(inv, 255), 300 * 300 - 50 * 50);
    EXPECT_EQ(inv.inverted(), s);
    EXPECT_EQ(Selection(64, 64).inverted(), Selection::all(64, 64));
}

TEST(Selection, TransformedMovesAndFlips) {
    const Selection s = Selection::rect(100, 100, {10, 20, 5, 3});
    const Selection moved = s.transformed(gas::Affine::translate(30, 40), gas::Interpolation::Nearest);
    EXPECT_EQ(moved.bounds(), (IRect{40, 60, 5, 3}));
    const Selection flipped =
        s.transformed(gas::Affine::around({50, 50}, gas::Affine::scale(-1, 1)), gas::Interpolation::Nearest);
    EXPECT_EQ(flipped.bounds(), (IRect{85, 20, 5, 3}));
}

TEST(Selection, Cropped) {
    const Selection s = Selection::rect(300, 300, {100, 100, 50, 50});
    const Selection c = s.cropped({90, 90, 100, 100});
    EXPECT_EQ(c.width(), 100);
    EXPECT_EQ(c.bounds(), (IRect{10, 10, 50, 50}));
}

TEST(Selection, OutlineOfRectIsFourMergedEdges) {
    const Selection s = Selection::rect(600, 600, {100, 200, 300, 50}); // spans tiles
    auto segs = s.outline();
    ASSERT_EQ(segs.size(), 4u);
    using Seg = Selection::Segment;
    const std::vector<Seg> expected{{100, 200, 400, 200}, {100, 250, 400, 250}, {100, 200, 100, 250},
                                    {400, 200, 400, 250}};
    for (const Seg& e : expected) {
        EXPECT_NE(std::find(segs.begin(), segs.end(), e), segs.end()) << e.x0 << "," << e.y0 << " " << e.x1 << "," << e.y1;
    }
}

TEST(Selection, OutlineOfSelectAllIsCanvasBorderAndCheap) {
    // 16384^2 select-all must not scan every pixel: uniform tiles are skipped.
    const Selection s = Selection::all(16384, 16384);
    const auto segs = s.outline();
    EXPECT_EQ(segs.size(), 4u);
}
