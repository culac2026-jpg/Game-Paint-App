#include <gas/core/Brush.h>
#include <gas/core/BrushPresets.h>
#include <gas/core/Selection.h>
#include <gas/core/TiledImage.h>

#include <gtest/gtest.h>

#include <algorithm>

using gas::BrushMode;
using gas::BrushTool;
using gas::Rgba8;
using gas::TiledImage;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};

void stroke(BrushTool& b, TiledImage& img, gas::PointF from, gas::PointF to) {
    b.begin(img, from);
    b.moveTo(img, to);
    b.end();
}

// Pixels in row y between x0 and x1 with alpha in (lo, hi].
int countAlpha(const TiledImage& img, int y, int x0, int x1, int lo, int hi) {
    int n = 0;
    for (int x = x0; x <= x1; ++x) {
        const int a = img.pixel({x, y}).a;
        n += a > lo && a <= hi ? 1 : 0;
    }
    return n;
}

// Left half red, right half blue.
TiledImage twoColours() {
    TiledImage img(100, 40);
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 100; ++x) {
            img.setPixel({x, y}, x < 50 ? kRed : kBlue);
        }
    }
    return img;
}

} // namespace

TEST(BrushMedia, GrainLeavesPaperTexture) {
    TiledImage img(200, 40);
    BrushTool b;
    b.setColor(kRed);
    b.settings().diameter = 20;
    b.settings().hardness = 1.0;
    b.settings().grain = 0.9;
    stroke(b, img, {20, 20}, {180, 20});
    // Inside the stroke some pixels are full and some are clearly lighter.
    EXPECT_GT(countAlpha(img, 20, 30, 170, 240, 255), 10);
    EXPECT_GT(countAlpha(img, 20, 30, 170, 0, 160), 10);

    // Grain is anchored to the canvas: scrubbing back and forth in one stroke doesn't fill the pits.
    TiledImage twice(200, 40);
    b.begin(twice, {20, 20});
    for (int i = 0; i < 3; ++i) {
        b.moveTo(twice, {180, 20});
        b.moveTo(twice, {20, 20});
    }
    b.end();
    for (int x = 30; x <= 170; ++x) {
        EXPECT_EQ(twice.pixel({x, 20}).a, img.pixel({x, 20}).a) << x;
    }
}

TEST(BrushMedia, ParticlesSprayDots) {
    TiledImage img(100, 100);
    BrushTool b;
    b.setColor(kRed);
    b.settings().diameter = 40;
    b.settings().hardness = 0.0;
    b.settings().particles = 0.1;
    b.begin(img, {50, 50});
    b.end();
    int painted = 0, empty = 0;
    for (int y = 40; y < 60; ++y) {
        for (int x = 40; x < 60; ++x) {
            (img.pixel({x, y}).a > 0 ? painted : empty)++;
        }
    }
    EXPECT_GT(painted, 3);
    EXPECT_GT(empty, painted); // dots, not a solid disc
    // Deterministic for the same seed.
    TiledImage again(100, 100);
    BrushTool b2;
    b2.setColor(kRed);
    b2.settings() = b.settings();
    b2.begin(again, {50, 50});
    b2.end();
    for (int y = 30; y < 70; ++y) {
        for (int x = 30; x < 70; ++x) {
            ASSERT_EQ(again.pixel({x, y}), img.pixel({x, y}));
        }
    }
}

TEST(BrushMedia, WetMixPicksUpColour) {
    TiledImage img = twoColours();
    BrushTool b;
    b.setColor({255, 255, 255, 255});
    b.settings().diameter = 10;
    b.settings().hardness = 1.0;
    b.settings().mix = 0.6;
    stroke(b, img, {10, 20}, {90, 20});
    // Early in the stroke the white paint has turned reddish; on the blue side it carries red in.
    const Rgba8 early = img.pixel({20, 20});
    EXPECT_GT(early.r, 200);
    EXPECT_LT(early.g, 200);
    const Rgba8 crossing = img.pixel({55, 20});
    EXPECT_GT(crossing.r, 60);

    // On an empty layer the brush paints its own colour.
    TiledImage empty(100, 40);
    stroke(b, empty, {10, 20}, {90, 20});
    EXPECT_EQ(empty.pixel({50, 20}), (Rgba8{255, 255, 255, 255}));
}

TEST(BrushMedia, BristleTipStreaks) {
    TiledImage img(200, 60);
    BrushTool b;
    b.setColor(kRed);
    b.settings().shape = gas::BrushShape::Bristle;
    b.settings().followDirection = true;
    b.settings().diameter = 40;
    b.settings().hardness = 1.0;
    stroke(b, img, {20, 30}, {180, 30});
    // Across the stroke the alpha varies (streaks); along it, it stays steady.
    int minA = 255, maxA = 0;
    for (int y = 16; y <= 44; ++y) {
        minA = std::min<int>(minA, img.pixel({100, y}).a);
        maxA = std::max<int>(maxA, img.pixel({100, y}).a);
    }
    EXPECT_GT(maxA - minA, 100);
    for (int x = 60; x < 140; ++x) {
        EXPECT_EQ(img.pixel({x, 25}).a, img.pixel({100, 25}).a);
    }
}

TEST(BrushMedia, SmudgeDragsColour) {
    TiledImage img = twoColours();
    BrushTool smudge(BrushMode::Smudge);
    smudge.settings().diameter = 12;
    smudge.settings().hardness = 0.8;
    smudge.settings().opacity = 0.8;
    stroke(smudge, img, {40, 20}, {65, 20});
    // Red has been dragged into the blue half; nothing outside the path changed.
    const Rgba8 p = img.pixel({56, 20});
    EXPECT_GT(p.r, 60);
    EXPECT_LT(p.b, 230);
    EXPECT_EQ(img.pixel({56, 2}), kBlue);
    EXPECT_EQ(img.pixel({10, 20}), kRed);
}

TEST(BrushMedia, BlurSoftensEdges) {
    TiledImage img = twoColours();
    BrushTool blur(BrushMode::Blur);
    blur.settings().diameter = 16;
    blur.settings().hardness = 0.5;
    blur.settings().opacity = 1.0;
    stroke(blur, img, {50, 10}, {50, 30});
    stroke(blur, img, {50, 10}, {50, 30});
    // Pixels at the seam are now in between red and blue.
    const Rgba8 a = img.pixel({49, 20}), b = img.pixel({50, 20});
    EXPECT_LT(a.r, 250);
    EXPECT_GT(a.b, 5);
    EXPECT_GT(b.r, 5);
    // Uniform areas stay the same and alpha is kept.
    EXPECT_EQ(img.pixel({44, 20}).a, 255);
    EXPECT_EQ(img.pixel({20, 20}), kRed);
}

TEST(BrushMedia, PresetsKeepMediaSettings) {
    gas::BrushPreset p{"Media", {}};
    p.settings.grain = 0.5;
    p.settings.grainScale = 4;
    p.settings.particles = 0.25;
    p.settings.mix = 0.75;
    p.settings.shape = gas::BrushShape::Bristle;
    const auto parsed = gas::parseBrushPresets(gas::serializeBrushPresets({p}));
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0], p);

    // The paint-program media set is built in.
    for (const char* name : {"Brush", "Calligraphy Brush", "Calligraphy Pen", "Airbrush", "Oil Brush", "Crayon",
                             "Marker", "Natural Pencil", "Watercolor"}) {
        const auto& all = gas::builtinBrushPresets();
        EXPECT_TRUE(std::any_of(all.begin(), all.end(), [&](const gas::BrushPreset& b) { return b.name == name; }))
            << name;
    }
}

TEST(SelectionFeather, SoftensEdges) {
    const auto sel = gas::Selection::rect(120, 120, {30, 30, 60, 60});
    const auto soft = sel.feathered(8);
    EXPECT_EQ(soft.coverage({60, 60}), 255);
    const int edge = soft.coverage({30, 60});
    EXPECT_GT(edge, 70);
    EXPECT_LT(edge, 200);
    EXPECT_GT(soft.coverage({26, 60}), 0);   // spreads outside
    EXPECT_LT(soft.coverage({33, 60}), 255); // and fades inside
    EXPECT_EQ(soft.coverage({2, 2}), 0);
    EXPECT_TRUE(gas::Selection(120, 120).feathered(5).isEmpty());
}
