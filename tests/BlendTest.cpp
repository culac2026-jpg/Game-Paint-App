#include <gas/core/Blend.h>

#include <gtest/gtest.h>

using gas::Rgba8;

namespace {
const Rgba8 kWhite{255, 255, 255, 255};
const Rgba8 kRed{255, 0, 0, 255};
} // namespace

TEST(Blend, OpaqueSourceReplaces) {
    EXPECT_EQ(gas::blendOver(kWhite, kRed, 1.0), kRed);
    EXPECT_EQ(gas::blendOver(Rgba8::transparent(), kRed, 1.0), kRed);
}

TEST(Blend, ZeroCoverageKeepsDestination) {
    EXPECT_EQ(gas::blendOver(kWhite, kRed, 0.0), kWhite);
}

TEST(Blend, HalfBlackOverWhiteIsGray) {
    const Rgba8 out = gas::blendOver(kWhite, Rgba8::black(), 0.5);
    EXPECT_EQ(out.a, 255);
    EXPECT_NEAR(out.r, 128, 1);
    EXPECT_EQ(out.r, out.g);
    EXPECT_EQ(out.g, out.b);
}

TEST(Blend, OverTransparentKeepsSourceColour) {
    // Straight alpha: the transparent destination's RGB must not darken the result.
    const Rgba8 out = gas::blendOver(Rgba8::transparent(), kRed, 0.25);
    EXPECT_EQ(out.r, 255);
    EXPECT_EQ(out.g, 0);
    EXPECT_NEAR(out.a, 64, 1);
}

TEST(Blend, EraseScalesAlphaAndNormalises) {
    const Rgba8 half = gas::eraseAlpha(kRed, 0.5);
    EXPECT_EQ(half.r, 255);
    EXPECT_NEAR(half.a, 128, 1);
    EXPECT_EQ(gas::eraseAlpha(kRed, 1.0), Rgba8::transparent());
}

TEST(Blend, ColorDistanceTreatsTransparentAsEqual) {
    EXPECT_EQ(gas::colorDistance(Rgba8{10, 20, 30, 0}, Rgba8::transparent()), 0);
    EXPECT_EQ(gas::colorDistance(kRed, kWhite), 255);
    EXPECT_EQ(gas::colorDistance(Rgba8{100, 100, 100, 255}, Rgba8{90, 104, 100, 250}), 10);
}
