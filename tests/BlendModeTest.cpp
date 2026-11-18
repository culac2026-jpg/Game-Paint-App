// Golden values for the W3C blend modes (hand-computed from the spec formulas).
#include <gas/core/Blend.h>
#include <gas/core/Document.h>
#include <gas/core/ProjectIO.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

using gas::BlendMode;
using gas::Rgba8;

namespace {

const Rgba8 kDst{200, 100, 50, 255};
const Rgba8 kSrc{100, 150, 250, 255};

Rgba8 blend(BlendMode mode, Rgba8 dst = kDst, Rgba8 src = kSrc) { return gas::blendPixel(dst, src, mode, 1.0); }

} // namespace

TEST(BlendMode, SeparableGoldenValues) {
    EXPECT_EQ(blend(BlendMode::Normal), kSrc);
    EXPECT_EQ(blend(BlendMode::Multiply), (Rgba8{78, 59, 49, 255}));
    EXPECT_EQ(blend(BlendMode::Screen), (Rgba8{222, 191, 251, 255}));
    EXPECT_EQ(blend(BlendMode::Darken), (Rgba8{100, 100, 50, 255}));
    EXPECT_EQ(blend(BlendMode::Lighten), (Rgba8{200, 150, 250, 255}));
    EXPECT_EQ(blend(BlendMode::Difference), (Rgba8{100, 50, 200, 255}));
    EXPECT_EQ(blend(BlendMode::LinearDodge), (Rgba8{255, 250, 255, 255}));
    EXPECT_EQ(blend(BlendMode::LinearBurn), (Rgba8{45, 0, 45, 255}));
    EXPECT_EQ(blend(BlendMode::Overlay).r, 188);
    // Exclusion: cb + cs - 2 cb cs  ->  r = 200 + 100 - 2*78.43 = 143.1
    EXPECT_EQ(blend(BlendMode::Exclusion).r, 143);
}

TEST(BlendMode, IdentitiesHold) {
    const Rgba8 white{255, 255, 255, 255}, black{0, 0, 0, 255};
    EXPECT_EQ(blend(BlendMode::Multiply, kDst, white), kDst);
    EXPECT_EQ(blend(BlendMode::Screen, kDst, black), kDst);
    EXPECT_EQ(blend(BlendMode::LinearDodge, kDst, black), kDst);
    EXPECT_EQ(blend(BlendMode::Difference, kDst, kDst), black);
    EXPECT_EQ(blend(BlendMode::ColorDodge, kDst, black), kDst);
    EXPECT_EQ(blend(BlendMode::ColorBurn, kDst, white), kDst);
    // Soft light with 50% grey leaves the backdrop (almost) unchanged.
    const Rgba8 sl = blend(BlendMode::SoftLight, kDst, Rgba8{128, 128, 128, 255});
    EXPECT_NEAR(sl.r, 200, 1);
    EXPECT_NEAR(sl.g, 100, 1);
}

TEST(BlendMode, NonSeparableWithGreySource) {
    // Grey has no hue/saturation: Hue, Saturation and Color all give a grey
    // with the backdrop's luminosity (0.3*200 + 0.59*100 + 0.11*40 = 123.4).
    const Rgba8 dst{200, 100, 40, 255};
    const Rgba8 grey{128, 128, 128, 255};
    EXPECT_EQ(blend(BlendMode::Hue, dst, grey), (Rgba8{123, 123, 123, 255}));
    EXPECT_EQ(blend(BlendMode::Saturation, dst, grey), (Rgba8{123, 123, 123, 255}));
    EXPECT_EQ(blend(BlendMode::Color, dst, grey), (Rgba8{123, 123, 123, 255}));
    // Luminosity takes the source's luminosity and keeps the backdrop's hue.
    const Rgba8 lum = blend(BlendMode::Luminosity, dst, grey);
    EXPECT_NEAR(0.3 * lum.r + 0.59 * lum.g + 0.11 * lum.b, 128, 1.0);
    EXPECT_GT(lum.r, lum.g);
    EXPECT_GT(lum.g, lum.b);
}

TEST(BlendMode, AlphaHandling) {
    // Semi-transparent source over opaque backdrop: lerp toward B.
    EXPECT_EQ(blend(BlendMode::Multiply, kDst, Rgba8{100, 150, 250, 128}).r, 139);
    // Opaque source over semi-transparent backdrop: (1 - ab) Cs + ab B.
    const Rgba8 r = blend(BlendMode::Multiply, Rgba8{200, 100, 50, 128}, kSrc);
    EXPECT_EQ(r.r, 89);
    EXPECT_EQ(r.a, 255);
    // Nothing below: every mode shows the source as is.
    for (int m = 0; m < gas::kBlendModeCount; ++m) {
        EXPECT_EQ(gas::blendPixel(Rgba8::transparent(), kSrc, static_cast<BlendMode>(m), 1.0), kSrc) << m;
        EXPECT_EQ(gas::blendPixel(kDst, kSrc, static_cast<BlendMode>(m), 0.0), kDst) << m;
    }
}

TEST(BlendMode, DeterministicAndNamed) {
    std::set<std::string> names;
    for (int m = 0; m < gas::kBlendModeCount; ++m) {
        const auto mode = static_cast<BlendMode>(m);
        names.insert(std::string(gas::blendModeName(mode)));
        for (int v = 0; v < 256; v += 17) {
            const Rgba8 d{static_cast<std::uint8_t>(v), 90, static_cast<std::uint8_t>(255 - v), 200};
            EXPECT_EQ(gas::blendPixel(d, kSrc, mode, 0.7), gas::blendPixel(d, kSrc, mode, 0.7));
        }
    }
    EXPECT_EQ(names.size(), static_cast<std::size_t>(gas::kBlendModeCount));
}

TEST(BlendMode, LayerCompositeUsesMode) {
    gas::Document doc(8, 8);
    doc.fillSelected(kDst);
    doc.addLayer();
    doc.fillSelected(kSrc);
    auto p = doc.layer(1).props;
    p.blendMode = BlendMode::Multiply;
    doc.setLayerProps(1, p);
    EXPECT_EQ(doc.compositePixel({3, 3}), (Rgba8{78, 59, 49, 255}));
    gas::TiledImage::Tile tile;
    ASSERT_TRUE(doc.compositeTile(0, 0, tile));
    EXPECT_EQ(tile.pixels[0], (Rgba8{78, 59, 49, 255}));
    doc.undo();
    EXPECT_EQ(doc.compositePixel({3, 3}), kSrc);
}

TEST(BlendMode, SavedInProjectFile) {
    const auto dir = std::filesystem::temp_directory_path() / "gas_blend_test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "blend.gas";
    gas::Document doc(8, 8);
    doc.fillSelected(kDst);
    doc.addLayer();
    doc.fillSelected(kSrc);
    auto p = doc.layer(1).props;
    p.blendMode = BlendMode::Screen;
    doc.setLayerProps(1, p);
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->layer(1).props.blendMode, BlendMode::Screen);
    EXPECT_EQ(loaded.document->layer(0).props.blendMode, BlendMode::Normal);
    EXPECT_EQ(loaded.document->flatten(), doc.flatten());
    std::filesystem::remove_all(dir);
}
