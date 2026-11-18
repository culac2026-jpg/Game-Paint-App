#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/StrokeSmoother.h>

#include <gtest/gtest.h>

#include <cmath>

using gas::StrokeSample;

namespace {

// Shaky horizontal line: y = 50 +- 3 px, flipping every 2 px.
std::vector<StrokeSample> shakyLine() {
    std::vector<StrokeSample> raw;
    for (int i = 0; i <= 100; ++i) {
        raw.push_back({{10.0 + 2 * i, 50.0 + ((i / 2) % 2 ? 3.0 : -3.0)}, 1.0});
    }
    return raw;
}

} // namespace

TEST(StreamLine, IronsOutJitterAndKeepsEnds) {
    const auto raw = shakyLine();
    const auto smooth = gas::streamlinePath(raw, 0.5);
    ASSERT_GT(smooth.size(), 100u);
    EXPECT_EQ(smooth.front().pos.x, raw.front().pos.x);
    EXPECT_EQ(smooth.front().pos.y, raw.front().pos.y);
    EXPECT_EQ(smooth.back().pos.x, raw.back().pos.x);
    EXPECT_EQ(smooth.back().pos.y, raw.back().pos.y);
    double worst = 0;
    for (const StrokeSample& s : smooth) {
        if (s.pos.x > 40 && s.pos.x < 180) {
            worst = std::max(worst, std::abs(s.pos.y - 50.0));
        }
    }
    EXPECT_LT(worst, 0.6); // was 3
    // Stronger = smoother.
    double worstStrong = 0;
    for (const StrokeSample& s : gas::streamlinePath(raw, 1.0)) {
        if (s.pos.x > 60 && s.pos.x < 160) {
            worstStrong = std::max(worstStrong, std::abs(s.pos.y - 50.0));
        }
    }
    EXPECT_LE(worstStrong, worst);
}

TEST(StreamLine, KeepsTheShapeOfCurves) {
    // A wobbly circle of radius 60 stays a circle of about that radius.
    std::vector<StrokeSample> raw;
    for (int i = 0; i <= 360; i += 3) {
        const double a = i * 3.14159265358979 / 180;
        const double r = 60 + ((i / 3) % 2 ? 2.5 : -2.5);
        raw.push_back({{100 + r * std::cos(a), 100 + r * std::sin(a)}, 1.0});
    }
    const auto smooth = gas::streamlinePath(raw, 0.5);
    for (std::size_t i = smooth.size() / 4; i < smooth.size() * 3 / 4; ++i) {
        const double r = std::hypot(smooth[i].pos.x - 100, smooth[i].pos.y - 100);
        EXPECT_NEAR(r, 60, 2.0);
    }
}

TEST(StreamLine, OffOrTooShortReturnsInput) {
    const auto raw = shakyLine();
    EXPECT_EQ(gas::streamlinePath(raw, 0.0).size(), raw.size());
    const std::vector<StrokeSample> two{{{0, 0}, 1}, {{5, 5}, 1}};
    EXPECT_EQ(gas::streamlinePath(two, 1.0).size(), 2u);
}

TEST(StreamLine, RevertEditRedrawsAsOneStep) {
    gas::Document doc(64, 64);
    gas::TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    gas::BrushTool brush;
    brush.begin(*img, {10, 10});
    brush.moveTo(*img, {50, 50});
    brush.end();
    doc.revertEdit();
    EXPECT_EQ(img->pixel({30, 30}).a, 0); // the first pass is gone
    brush.begin(*img, {10, 50});
    brush.moveTo(*img, {50, 50});
    brush.end();
    EXPECT_TRUE(doc.endEdit("Brush"));
    EXPECT_EQ(doc.layer(0).image.pixel({30, 30}).a, 0);
    EXPECT_GT(doc.layer(0).image.pixel({30, 50}).a, 0);
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.layer(0).image.pixel({30, 50}).a, 0);

    // Reverting everything leaves no undo step.
    img = doc.beginEdit();
    brush.begin(*img, {10, 10});
    brush.end();
    doc.revertEdit();
    EXPECT_FALSE(doc.endEdit("Brush"));
}
