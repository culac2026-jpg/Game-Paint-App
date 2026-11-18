#include <gas/core/Brush.h>
#include <gas/core/BrushPresets.h>
#include <gas/core/StrokeSmoother.h>

#include <gtest/gtest.h>

#include <set>

using gas::BrushSettings;
using gas::BrushTool;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};

// Number of painted pixels in column x / row y.
int columnCount(const TiledImage& img, int x) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        n += img.pixel({x, y}).a > 127 ? 1 : 0;
    }
    return n;
}
int rowCount(const TiledImage& img, int y) {
    int n = 0;
    for (int x = 0; x < img.width(); ++x) {
        n += img.pixel({x, y}).a > 127 ? 1 : 0;
    }
    return n;
}

void stroke(BrushTool& brush, TiledImage& img, gas::PointF a, gas::PointF b, double pa = 1.0, double pb = 1.0) {
    brush.begin(img, a, pa);
    brush.moveTo(img, b, pb);
    brush.end();
}

bool sameImage(const TiledImage& a, const TiledImage& b) {
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel({x, y}) != b.pixel({x, y})) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST(BrushDynamics, PressureControlsSize) {
    TiledImage img(300, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 30;
    brush.settings().hardness = 1.0;
    brush.settings().pressureSize = 1.0;
    stroke(brush, img, {10, 50}, {290, 50}, 0.2, 1.0); // tapering in
    const int thin = columnCount(img, 30);
    const int thick = columnCount(img, 280);
    EXPECT_LE(thin, 12);
    EXPECT_GE(thick, 26);
    EXPECT_LT(columnCount(img, 100), columnCount(img, 200)); // monotonic taper
}

TEST(BrushDynamics, PressureControlsOpacity) {
    TiledImage img(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 20;
    brush.settings().hardness = 1.0;
    brush.settings().pressureOpacity = 1.0;
    brush.begin(img, {50, 50}, 0.5);
    brush.end();
    EXPECT_NEAR(img.pixel({50, 50}).a, 128, 1);
}

TEST(BrushDynamics, MouseInputIgnoresPressureSettingsAtFullPressure) {
    TiledImage a(200, 60), b(200, 60);
    BrushTool plain, dynamic;
    for (BrushTool* t : {&plain, &dynamic}) {
        t->setColor(kRed);
        t->settings().diameter = 16;
    }
    dynamic.settings().pressureSize = 1.0;
    dynamic.settings().pressureOpacity = 1.0;
    stroke(plain, a, {10, 30}, {190, 30});
    stroke(dynamic, b, {10, 30}, {190, 30});
    EXPECT_TRUE(sameImage(a, b));
}

TEST(BrushDynamics, EllipticalTipAndAngle) {
    TiledImage img(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 40;
    brush.settings().hardness = 1.0;
    brush.settings().roundness = 0.25;
    brush.begin(img, {50, 50});
    brush.end();
    EXPECT_NEAR(rowCount(img, 50), 40, 2);
    EXPECT_NEAR(columnCount(img, 50), 10, 2);

    TiledImage rotated(100, 100);
    brush.settings().angle = 90;
    brush.begin(rotated, {50, 50});
    brush.end();
    EXPECT_NEAR(rowCount(rotated, 50), 10, 2);
    EXPECT_NEAR(columnCount(rotated, 50), 40, 2);
}

TEST(BrushDynamics, FollowDirectionTurnsTip) {
    const auto width = [](bool follow) {
        TiledImage img(100, 100);
        BrushTool brush;
        brush.setColor(kRed);
        brush.settings().diameter = 30;
        brush.settings().hardness = 1.0;
        brush.settings().roundness = 0.2;
        brush.settings().followDirection = follow;
        stroke(brush, img, {50, 10}, {50, 90}); // straight down
        return rowCount(img, 50);
    };
    EXPECT_GE(width(false), 24);      // major axis horizontal: wide (dabs 3 px apart dip between centres)
    EXPECT_NEAR(width(true), 6, 2);   // major axis follows the stroke: thin
}

TEST(BrushDynamics, JitterIsDeterministicPerSeed) {
    const auto run = [](std::uint64_t seed) {
        auto img = std::make_unique<TiledImage>(300, 120);
        BrushTool brush;
        brush.setColor(kRed);
        brush.settings().diameter = 10;
        brush.settings().spacing = 0.5;
        brush.settings().scatter = 2.0;
        brush.settings().sizeJitter = 0.8;
        brush.settings().opacityJitter = 0.5;
        brush.setSeed(seed);
        stroke(brush, *img, {20, 60}, {280, 60});
        return img;
    };
    const auto a = run(42), b = run(42), c = run(7);
    EXPECT_TRUE(sameImage(*a, *b));
    EXPECT_FALSE(sameImage(*a, *c));
    // Scatter puts paint well off the stroke line.
    int offLine = 0;
    for (int x = 0; x < 300; ++x) {
        offLine += a->pixel({x, 60 - 14}).a > 0 || a->pixel({x, 60 + 14}).a > 0 ? 1 : 0;
    }
    EXPECT_GT(offLine, 0);
}

TEST(BrushDynamics, StrokeOpacityCapHoldsWithDynamics) {
    TiledImage img(200, 200);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 30;
    brush.settings().opacity = 0.5;
    brush.settings().opacityJitter = 0.5;
    brush.settings().sizeJitter = 0.5;
    brush.begin(img, {20, 20});
    brush.moveTo(img, {180, 180});
    brush.moveTo(img, {20, 180});
    brush.moveTo(img, {180, 20}); // crosses itself
    brush.end();
    for (int y = 0; y < 200; y += 3) {
        for (int x = 0; x < 200; x += 3) {
            ASSERT_LE(img.pixel({x, y}).a, 128) << x << "," << y;
        }
    }
}

TEST(BrushDynamics, BrushBlendMode) {
    TiledImage img(50, 50);
    for (int y = 0; y < 50; ++y) {
        for (int x = 0; x < 50; ++x) {
            img.setPixel({x, y}, Rgba8{200, 100, 50, 255});
        }
    }
    BrushTool brush;
    brush.setColor(Rgba8{100, 150, 250, 255});
    brush.settings().diameter = 20;
    brush.settings().hardness = 1.0;
    brush.settings().blendMode = gas::BlendMode::Multiply;
    brush.begin(img, {25, 25});
    brush.end();
    EXPECT_EQ(img.pixel({25, 25}), (Rgba8{78, 59, 49, 255}));
}

TEST(StrokeSmoother, OffIsIdentityOnLags) {
    gas::StrokeSmoother off(0.0);
    off.begin({{0, 0}});
    EXPECT_EQ(off.add({{10, 0}}).pos.x, 10);
    EXPECT_FALSE(off.finish().has_value());

    gas::StrokeSmoother on(0.8);
    on.begin({{0, 0}, 1.0});
    const gas::StrokeSample s = on.add({{10, 0}, 0.0});
    EXPECT_GT(s.pos.x, 0);
    EXPECT_LT(s.pos.x, 5);
    EXPECT_LT(s.pressure, 1.0);
    const auto tail = on.finish();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->pos.x, 10); // stroke still ends at the release point
}

TEST(StrokeSmoother, ReducesJitter) {
    gas::StrokeSmoother sm(0.7);
    sm.begin({{0, 50}});
    double maxDev = 0;
    for (int i = 1; i < 200; ++i) {
        const double noisy = 50 + ((i % 2) ? 6.0 : -6.0); // zig-zag hand shake
        const gas::StrokeSample s = sm.add({{double(i), noisy}});
        if (i > 20) {
            maxDev = std::max(maxDev, std::abs(s.pos.y - 50));
        }
    }
    EXPECT_LT(maxDev, 3.0);
}

TEST(BrushPresets, BuiltinsAreValidAndUnique) {
    const auto& presets = gas::builtinBrushPresets();
    ASSERT_GE(presets.size(), 6u);
    std::set<std::string> names;
    for (const auto& p : presets) {
        names.insert(p.name);
        EXPECT_GE(p.settings.diameter, 1.0);
        EXPECT_GT(p.settings.opacity, 0.0);
    }
    EXPECT_EQ(names.size(), presets.size());
}

TEST(BrushPresets, RoundTrip) {
    std::vector<gas::BrushPreset> presets = gas::builtinBrushPresets();
    gas::BrushPreset custom{"Mine, with = signs", {}};
    custom.settings.diameter = 33.25;
    custom.settings.angle = -12.5;
    custom.settings.followDirection = true;
    custom.settings.blendMode = gas::BlendMode::Overlay;
    custom.settings.smoothing = 0.1;
    presets.push_back(custom);
    EXPECT_EQ(gas::parseBrushPresets(gas::serializeBrushPresets(presets)), presets);
}

TEST(BrushPresets, ParserIsTolerant) {
    const auto presets = gas::parseBrushPresets(
        "garbage line\n"
        "[preset]\r\n"
        "name = Big\r\n"
        "diameter = 99999\n"     // clamped
        "opacity = nan\n"        // ignored
        "future_key = 3\n"       // unknown: ignored
        "blend_mode = 200\n"     // unknown mode: Normal
        "[preset]\n"
        "diameter = 5\n");       // no name: dropped
    ASSERT_EQ(presets.size(), 1u);
    EXPECT_EQ(presets[0].name, "Big");
    EXPECT_EQ(presets[0].settings.diameter, BrushSettings::kMaxDiameter);
    EXPECT_EQ(presets[0].settings.opacity, 1.0);
    EXPECT_EQ(presets[0].settings.blendMode, gas::BlendMode::Normal);
    EXPECT_TRUE(gas::parseBrushPresets("").empty());
}
