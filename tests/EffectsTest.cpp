#include <gas/core/Document.h>
#include <gas/core/Effects.h>
#include <gas/core/ProjectIO.h>

#include <gtest/gtest.h>

#include <filesystem>

using gas::Document;
using gas::Effect;
using gas::EffectType;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};
const Rgba8 kBlack{0, 0, 0, 255};

void dot(Document& doc, gas::Point p, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    img->setPixel(p, c);
    doc.endEdit("Dot");
}

void block(Document& doc, gas::IRect r, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    for (int y = r.y; y < r.bottom(); ++y) {
        for (int x = r.x; x < r.right(); ++x) {
            img->setPixel({x, y}, c);
        }
    }
    doc.endEdit("Block");
}

void setEffects(Document& doc, int layer, std::vector<Effect> effects) {
    auto p = doc.layer(layer).props;
    p.effects = std::move(effects);
    ASSERT_TRUE(doc.setLayerProps(layer, p));
}

// Same layers rendered from scratch (no incremental cache).
std::vector<Rgba8> freshRender(const Document& doc) {
    std::vector<gas::Layer> layers;
    for (int i = 0; i < doc.layerCount(); ++i) {
        gas::Layer l = doc.layer(i);
        l.effectCache.reset();
        layers.push_back(std::move(l));
    }
    Document copy(doc.width(), doc.height(), std::move(layers), 0);
    return copy.flatten();
}

} // namespace

TEST(Effects, NonDestructive) {
    Document doc(64, 64);
    block(doc, {20, 20, 10, 10}, kRed);
    const auto pixelsBefore = doc.layer(0).image.snapshot();
    const auto compositeBefore = doc.flatten();

    Effect outline = Effect::make(EffectType::Outline);
    outline.size = 2;
    Effect shadow = Effect::make(EffectType::DropShadow);
    setEffects(doc, 0, {shadow, outline});
    EXPECT_NE(doc.flatten(), compositeBefore);
    EXPECT_EQ(doc.layer(0).image.snapshot(), pixelsBefore); // same tiles: pixels untouched

    // Tweaking parameters many times never degrades anything.
    for (int i = 0; i < 5; ++i) {
        outline.size = 1 + i;
        setEffects(doc, 0, {shadow, outline});
        doc.flatten();
    }
    setEffects(doc, 0, {});
    EXPECT_EQ(doc.flatten(), compositeBefore);
    EXPECT_EQ(doc.layer(0).image.snapshot(), pixelsBefore);
}

TEST(Effects, EditsAreUndoableAndMerge) {
    Document doc(32, 32);
    dot(doc, {10, 10}, kRed);
    const int steps = doc.undoSteps();
    Effect e = Effect::make(EffectType::Outline);
    for (int i = 1; i <= 4; ++i) { // like dragging a slider
        e.size = i;
        setEffects(doc, 0, {e});
    }
    EXPECT_EQ(doc.undoSteps(), steps + 1);
    doc.undo();
    EXPECT_TRUE(doc.layer(0).props.effects.empty());
    EXPECT_EQ(doc.compositePixel({11, 10}), Rgba8::transparent());
}

TEST(Effects, PixelOutline) {
    Document doc(32, 32);
    dot(doc, {10, 10}, kRed);
    Effect e = Effect::make(EffectType::Outline);
    e.size = 1;
    setEffects(doc, 0, {e});
    EXPECT_EQ(doc.compositePixel({10, 10}), kRed);
    EXPECT_EQ(doc.compositePixel({11, 10}), kBlack);
    EXPECT_EQ(doc.compositePixel({10, 9}), kBlack);
    EXPECT_EQ(doc.compositePixel({11, 11}), Rgba8::transparent()); // 4-connected at size 1
    EXPECT_EQ(doc.compositePixel({12, 10}), Rgba8::transparent());

    e.size = 1.5;
    setEffects(doc, 0, {e});
    EXPECT_EQ(doc.compositePixel({11, 11}), kBlack); // diagonals at 1.5
    EXPECT_EQ(doc.compositePixel({12, 10}), Rgba8::transparent());
}

TEST(Effects, DropShadowOffsetAndOpacity) {
    Document doc(64, 64);
    block(doc, {10, 10, 8, 8}, kRed);
    Effect s = Effect::make(EffectType::DropShadow);
    s.size = 0; // hard shadow
    s.offsetX = 5;
    s.offsetY = 3;
    s.opacity = 0.5;
    setEffects(doc, 0, {s});
    EXPECT_EQ(doc.compositePixel({14, 14}), kRed); // content stays on top
    const Rgba8 sh = doc.compositePixel({21, 19});
    EXPECT_EQ(sh.r, 0);
    EXPECT_NEAR(sh.a, 128, 1);
    EXPECT_EQ(doc.compositePixel({23, 21}), Rgba8::transparent());

    s.size = 6; // blurred: soft falloff
    setEffects(doc, 0, {s});
    EXPECT_GT(doc.compositePixel({24, 21}).a, 0);
    EXPECT_LT(doc.compositePixel({24, 21}).a, doc.compositePixel({20, 17}).a);
}

TEST(Effects, OuterGlowFadesWithDistance) {
    Document doc(64, 64);
    block(doc, {28, 28, 8, 8}, kRed);
    Effect g = Effect::make(EffectType::OuterGlow);
    g.size = 10;
    g.opacity = 1.0;
    setEffects(doc, 0, {g});
    const int a1 = doc.compositePixel({37, 31}).a, a5 = doc.compositePixel({41, 31}).a,
              a12 = doc.compositePixel({48, 31}).a;
    EXPECT_GT(a1, a5);
    EXPECT_GT(a5, 0);
    EXPECT_EQ(a12, 0);
}

TEST(Effects, InnerShadowStaysInside) {
    Document doc(64, 64);
    block(doc, {20, 20, 20, 20}, Rgba8{255, 255, 255, 255});
    Effect s = Effect::make(EffectType::InnerShadow);
    s.size = 0;
    s.offsetX = 3;
    s.offsetY = 3;
    s.opacity = 1.0;
    setEffects(doc, 0, {s});
    EXPECT_EQ(doc.compositePixel({21, 21}), kBlack);                          // top-left edge shadowed
    EXPECT_EQ(doc.compositePixel({30, 30}), (Rgba8{255, 255, 255, 255}));     // interior untouched
    EXPECT_EQ(doc.compositePixel({38, 38}), (Rgba8{255, 255, 255, 255}));     // far edge lit
    EXPECT_EQ(doc.compositePixel({18, 18}), Rgba8::transparent());            // nothing outside
}

TEST(Effects, ColorAdjustAndOverlay) {
    Document doc(16, 16);
    block(doc, {0, 0, 4, 4}, kRed);
    Effect adj = Effect::make(EffectType::ColorAdjust);
    adj.hue = 180;
    setEffects(doc, 0, {adj});
    EXPECT_EQ(doc.compositePixel({1, 1}), (Rgba8{0, 255, 255, 255}));
    adj.hue = 0;
    adj.saturation = -1;
    setEffects(doc, 0, {adj});
    const Rgba8 grey = doc.compositePixel({1, 1});
    EXPECT_EQ(grey.r, grey.g);
    EXPECT_EQ(grey.g, grey.b);

    Effect ov = Effect::make(EffectType::ColorOverlay);
    ov.color = {0, 0, 255, 255};
    setEffects(doc, 0, {ov});
    EXPECT_EQ(doc.compositePixel({1, 1}), (Rgba8{0, 0, 255, 255}));
    EXPECT_EQ(doc.compositePixel({8, 8}), Rgba8::transparent()); // alpha kept
}

TEST(Effects, BlurAndPixelate) {
    Document doc(64, 64);
    dot(doc, {32, 32}, kRed);
    Effect blur = Effect::make(EffectType::Blur);
    blur.size = 6;
    setEffects(doc, 0, {blur});
    EXPECT_LT(doc.compositePixel({32, 32}).a, 255);
    EXPECT_GT(doc.compositePixel({34, 32}).a, 0);

    Document px(16, 16);
    block(px, {0, 0, 2, 4}, kRed); // half of the first 4x4 cell
    Effect p = Effect::make(EffectType::Pixelate);
    p.size = 4;
    setEffects(px, 0, {p});
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            EXPECT_NEAR(px.compositePixel({x, y}).a, 128, 1);
        }
    }
    EXPECT_EQ(px.compositePixel({5, 5}), Rgba8::transparent());
}

TEST(Effects, IncrementalRenderMatchesFullRender) {
    // Content and effects crossing tile borders (256 px); repaint near them.
    Document doc(600, 600);
    block(doc, {240, 240, 30, 30}, kRed);
    Effect outline = Effect::make(EffectType::Outline);
    outline.size = 3;
    outline.smooth = true;
    Effect shadow = Effect::make(EffectType::DropShadow);
    shadow.offsetX = 12;
    shadow.offsetY = -9;
    Effect glow = Effect::make(EffectType::OuterGlow);
    setEffects(doc, 0, {shadow, glow, outline});
    doc.flatten(); // build the cache
    block(doc, {250, 500, 20, 20}, kRed);
    block(doc, {255, 255, 3, 3}, Rgba8{0, 255, 0, 255});
    TiledImage* img = doc.beginEdit(); // erase across the tile corner
    for (int y = 250; y < 262; ++y) {
        for (int x = 250; x < 262; ++x) {
            img->setPixel({x, y}, Rgba8::transparent());
        }
    }
    doc.endEdit("Erase");
    EXPECT_EQ(doc.flatten(), freshRender(doc));
    doc.undo();
    EXPECT_EQ(doc.flatten(), freshRender(doc));
}

TEST(Effects, DirtyTilesCoverEffectReach) {
    Document doc(768, 768);
    Effect glow = Effect::make(EffectType::OuterGlow);
    glow.size = 20;
    setEffects(doc, 0, {glow});
    doc.takeDirtyTiles();
    dot(doc, {258, 258}, kRed); // just inside tile (1,1); glow reaches tile (0,0)
    const auto dirty = doc.takeDirtyTiles();
    EXPECT_NE(std::find(dirty.begin(), dirty.end(), gas::Point{0, 0}), dirty.end());
    EXPECT_NE(std::find(dirty.begin(), dirty.end(), gas::Point{1, 1}), dirty.end());
}

TEST(Effects, MaskShapesTheEffects) {
    Document doc(64, 64);
    block(doc, {10, 10, 20, 20}, kRed);
    doc.setSelection(gas::Selection::rect(64, 64, {10, 10, 10, 20})); // left half
    ASSERT_TRUE(doc.addLayerMask(0));
    doc.deselect();
    Effect outline = Effect::make(EffectType::Outline);
    setEffects(doc, 0, {outline});
    EXPECT_EQ(doc.compositePixel({20, 15}), kBlack);              // outline at the mask edge
    EXPECT_EQ(doc.compositePixel({25, 15}), Rgba8::transparent()); // masked part hidden
}

TEST(Effects, RasterizeBakesAndUndoes) {
    Document doc(64, 64);
    block(doc, {10, 10, 8, 8}, kRed);
    setEffects(doc, 0, {Effect::make(EffectType::Outline), Effect::make(EffectType::DropShadow)});
    const auto withEffects = doc.flatten();
    ASSERT_TRUE(doc.rasterizeLayerEffects(0));
    EXPECT_TRUE(doc.layer(0).props.effects.empty());
    EXPECT_EQ(doc.flatten(), withEffects);
    EXPECT_EQ(doc.layer(0).image.pixel({18, 12}), kBlack); // outline is now real pixels
    doc.undo();
    EXPECT_EQ(doc.layer(0).props.effects.size(), 2u);
    EXPECT_EQ(doc.layer(0).image.pixel({18, 12}), Rgba8::transparent());
}

TEST(Effects, DisabledEffectsAreIgnored) {
    Document doc(32, 32);
    dot(doc, {10, 10}, kRed);
    Effect e = Effect::make(EffectType::Outline);
    e.enabled = false;
    setEffects(doc, 0, {e});
    EXPECT_EQ(doc.compositePixel({11, 10}), Rgba8::transparent());
    EXPECT_EQ(gas::effectMargin({e}), 0);
    EXPECT_FALSE(doc.rasterizeLayerEffects(0));
}

TEST(Effects, SavedInProjectFile) {
    const auto dir = std::filesystem::temp_directory_path() / "gas_effects_test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "fx.gas";
    Document doc(64, 64);
    block(doc, {10, 10, 8, 8}, kRed);
    Effect a = Effect::make(EffectType::DropShadow);
    a.offsetX = -3;
    a.color = {10, 20, 30, 200};
    Effect b = Effect::make(EffectType::ColorAdjust);
    b.hue = 45;
    b.enabled = false;
    Effect c = Effect::make(EffectType::Outline);
    c.smooth = true;
    c.size = 2.5;
    setEffects(doc, 0, {a, b, c});
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    const auto& fx = loaded.document->layer(0).props.effects;
    ASSERT_EQ(fx.size(), 3u);
    EXPECT_EQ(fx[0].type, EffectType::DropShadow);
    EXPECT_EQ(fx[0].offsetX, -3);
    EXPECT_EQ(fx[0].color, (Rgba8{10, 20, 30, 200}));
    EXPECT_FALSE(fx[1].enabled);
    EXPECT_EQ(fx[1].hue, 45);
    EXPECT_TRUE(fx[2].smooth);
    EXPECT_EQ(fx[2].size, 2.5);
    EXPECT_EQ(loaded.document->flatten(), doc.flatten());
    std::filesystem::remove_all(dir);
}
