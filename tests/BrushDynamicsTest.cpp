#include <gas/core/Brush.h>
#include <gas/core/BrushPresets.h>
#include <gas/core/Selection.h>
#include <gas/core/StrokeSmoother.h>

#include <gtest/gtest.h>

#include <cmath>
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

namespace {

// One hard dab of `shape` (diameter 20) at the centre of a 100x100 image.
std::unique_ptr<TiledImage> stampShape(gas::BrushShape shape, double angle = 0.0) {
    auto img = std::make_unique<TiledImage>(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 20;
    brush.settings().hardness = 1.0;
    brush.settings().shape = shape;
    brush.settings().angle = angle;
    brush.begin(*img, {50, 50});
    brush.end();
    return img;
}

bool painted(const TiledImage& img, int x, int y) { return img.pixel({x, y}).a > 127; }

} // namespace

TEST(BrushShapes, BuiltInShapes) {
    const auto round = stampShape(gas::BrushShape::Round);
    const auto square = stampShape(gas::BrushShape::Square);
    // Pixel (58,58) has its centre 8.5 px from the dab centre on both axes.
    EXPECT_FALSE(painted(*round, 58, 58));
    EXPECT_TRUE(painted(*square, 58, 58));

    const auto diamond = stampShape(gas::BrushShape::Diamond);
    EXPECT_TRUE(painted(*diamond, 57, 50));  // on an axis, inside the tip
    EXPECT_FALSE(painted(*diamond, 56, 56)); // diagonal: |x|+|y| = 13 > 10

    const auto triangle = stampShape(gas::BrushShape::Triangle); // apex up
    EXPECT_TRUE(painted(*triangle, 50, 42));  // towards the apex (y = 40)
    EXPECT_TRUE(painted(*triangle, 50, 53));
    EXPECT_FALSE(painted(*triangle, 50, 57)); // below the base (y = 55)

    const auto star = stampShape(gas::BrushShape::Star); // a point straight up
    EXPECT_TRUE(painted(*star, 50, 50));
    EXPECT_TRUE(painted(*star, 50, 42)); // along a point
    // Between two points (at -54 degrees) 7 px out is past the inner radius 4.5.
    EXPECT_FALSE(painted(*star, 50 + static_cast<int>(std::lround(7 * std::cos(-0.9425))),
                         50 + static_cast<int>(std::lround(7 * std::sin(-0.9425)))));

    const auto hexagon = stampShape(gas::BrushShape::Hexagon);
    EXPECT_TRUE(painted(*hexagon, 58, 50));  // vertex direction (x axis)
    EXPECT_FALSE(painted(*hexagon, 50, 59)); // flat side at y = 8.66
}

TEST(BrushShapes, AngleRotatesShape) {
    const auto square = stampShape(gas::BrushShape::Square);
    const auto turned = stampShape(gas::BrushShape::Square, 45); // now a diamond
    EXPECT_TRUE(painted(*square, 58, 58));
    EXPECT_FALSE(painted(*turned, 58, 58));
    EXPECT_TRUE(painted(*turned, 62, 50)); // corner on the x axis reaches ~14 px
}

TEST(BrushShapes, EdgesStayAntialiased) {
    const auto star = stampShape(gas::BrushShape::Star);
    int partial = 0;
    for (int y = 35; y < 65; ++y) {
        for (int x = 35; x < 65; ++x) {
            const int a = star->pixel({x, y}).a;
            partial += a > 0 && a < 255 ? 1 : 0;
        }
    }
    EXPECT_GT(partial, 20);
}

TEST(BrushShapes, CustomTipFromImage) {
    // An "L" drawn on a layer becomes the tip.
    TiledImage src(64, 64);
    for (int y = 10; y < 30; ++y) {
        src.setPixel({10, y}, kRed);
        src.setPixel({11, y}, kRed);
    }
    for (int x = 10; x < 30; ++x) {
        src.setPixel({x, 28}, kRed);
        src.setPixel({x, 29}, kRed);
    }
    const gas::BrushTip tip = gas::brushTipFromImage(src, {0, 0, 64, 64});
    ASSERT_EQ(tip.width, 20); // trimmed to the content
    ASSERT_EQ(tip.height, 20);
    EXPECT_EQ(tip.alpha[0], 255);       // top-left of the L
    EXPECT_EQ(tip.alpha[10], 0);        // empty interior
    EXPECT_TRUE(gas::brushTipFromImage(src, {40, 40, 10, 10}).empty());

    TiledImage img(100, 100);
    BrushTool brush;
    brush.setColor(kRed);
    brush.settings().shape = gas::BrushShape::Custom;
    brush.settings().tip.tip = std::make_shared<const gas::BrushTip>(tip);
    brush.settings().diameter = 40; // 2x the tip
    brush.begin(img, {50, 50});
    brush.end();
    EXPECT_TRUE(painted(img, 32, 35));   // vertical bar of the L (left edge)
    EXPECT_TRUE(painted(img, 60, 67));   // horizontal bar (bottom)
    EXPECT_FALSE(painted(img, 55, 45));  // inside the L's corner: empty

    // A custom shape without a tip falls back to round.
    TiledImage fallback(100, 100);
    BrushTool plain;
    plain.settings().shape = gas::BrushShape::Custom;
    plain.settings().diameter = 20;
    plain.begin(fallback, {50, 50});
    plain.end();
    EXPECT_TRUE(painted(fallback, 50, 50));
}

TEST(BrushShapes, TipFromSelectionAndDownscale) {
    TiledImage src(1200, 300);
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 1200; ++x) {
            src.setPixel({x, y}, kRed);
        }
    }
    const auto sel = gas::Selection::rect(1200, 300, {100, 100, 50, 50});
    const gas::BrushTip small = gas::brushTipFromImage(src, {0, 0, 1200, 300}, &sel);
    EXPECT_EQ(small.width, 50);
    EXPECT_EQ(small.height, 50);
    const gas::BrushTip big = gas::brushTipFromImage(src, {0, 0, 1200, 300});
    EXPECT_LE(big.width, gas::BrushTip::kMaxSize);
    EXPECT_EQ(big.width, 400); // factor 3
    EXPECT_EQ(big.height, 100);
}

TEST(BrushShapes, PresetsKeepShapeAndTip) {
    gas::BrushPreset p{"Stamp", {}};
    p.settings.shape = gas::BrushShape::Custom;
    p.settings.tip.tip = std::make_shared<const gas::BrushTip>(gas::BrushTip{3, 2, {0, 64, 128, 192, 255, 7}});
    gas::BrushPreset q{"Stars", {}};
    q.settings.shape = gas::BrushShape::Star;
    const auto parsed = gas::parseBrushPresets(gas::serializeBrushPresets({p, q}));
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0], p); // tips compare by content
    EXPECT_EQ(parsed[1], q);
    EXPECT_EQ(gas::parseBrushPresets("[preset]\nname = X\ntip = 2 2 !!!!\n")[0].settings.tip.tip, nullptr);
}
