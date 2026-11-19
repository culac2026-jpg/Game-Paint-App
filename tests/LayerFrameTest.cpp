#include <gas/core/Document.h>
#include <gas/core/ProjectIO.h>
#include <gas/core/Selection.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>

using gas::Document;
using gas::LayerFrame;
using gas::Rgba8;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};

// 100x100 document whose only layer has a red bar in [20, 60) x [40, 50).
std::unique_ptr<Document> barDoc() {
    auto doc = std::make_unique<Document>(100, 100);
    gas::TiledImage* img = doc->beginEdit();
    for (int y = 40; y < 50; ++y) {
        for (int x = 20; x < 60; ++x) {
            img->setPixel({x, y}, kRed);
        }
    }
    doc->endEdit("Paint");
    doc->markSaved();
    return doc;
}

// The bar's box with the anchor on its left end (the "joint"), turned 90 degrees.
LayerFrame turnedFrame() {
    LayerFrame f;
    f.rect = {20, 40, 40, 10};
    f.pivot = {20, 45};
    f.angle = std::acos(-1.0) / 2;
    return f;
}

} // namespace

TEST(LayerFrame, CommitStoresTheFrameInTheSameStep) {
    auto owned = barDoc();
    Document& doc = *owned;
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_TRUE(doc.transformLiftsWholeLayer());
    const LayerFrame f = turnedFrame();
    doc.setTransform(f.matrix());
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest, "Transform", f));
    ASSERT_TRUE(doc.layer(0).props.frame.has_value());
    EXPECT_EQ(*doc.layer(0).props.frame, f);
    EXPECT_EQ(doc.layer(0).image.pixel({24, 60}), kRed); // turned down around (20, 45)
    EXPECT_EQ(doc.undoLabel(), "Transform");
    ASSERT_TRUE(doc.undo()); // one step: pixels and frame together
    EXPECT_FALSE(doc.layer(0).props.frame.has_value());
    EXPECT_EQ(doc.layer(0).image.pixel({50, 45}), kRed);
    EXPECT_FALSE(doc.isModified());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(*doc.layer(0).props.frame, f);
}

TEST(LayerFrame, MovingOnlyTheAnchorIsItsOwnStep) {
    auto owned = barDoc();
    Document& doc = *owned;
    ASSERT_TRUE(doc.beginTransform());
    LayerFrame f;
    f.rect = {20, 40, 40, 10};
    f.pivot = {20, 45}; // anchor moved, pixels not
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest, "Transform", f));
    EXPECT_EQ(doc.undoLabel(), "Set Anchor");
    EXPECT_EQ(doc.layer(0).image.pixel({50, 45}), kRed);
    EXPECT_EQ(doc.layer(0).props.frame->anchor().x, 20.0);
    // The same frame again changes nothing.
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_FALSE(doc.commitTransform(gas::Interpolation::Nearest, "Transform", f));
    ASSERT_TRUE(doc.undo());
    EXPECT_FALSE(doc.layer(0).props.frame.has_value());
    EXPECT_FALSE(doc.canUndo() && doc.undoLabel() == "Set Anchor");
}

TEST(LayerFrame, PartialSelectionDoesNotLiftTheWholeLayer) {
    auto owned = barDoc();
    Document& doc = *owned;
    doc.setSelection(gas::Selection::rect(100, 100, {20, 40, 10, 10}));
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_FALSE(doc.transformLiftsWholeLayer());
    doc.cancelTransform();
    doc.setSelection(gas::Selection::rect(100, 100, {10, 30, 60, 30})); // covers the whole bar
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_TRUE(doc.transformLiftsWholeLayer());
    doc.cancelTransform();
}

TEST(LayerFrame, SurvivesSaveOpenAndFollowsCrop) {
    auto owned = barDoc();
    Document& doc = *owned;
    ASSERT_TRUE(doc.beginTransform());
    const LayerFrame f = turnedFrame();
    doc.setTransform(f.matrix());
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest, "Transform", f));
    const auto bytes = gas::serializeProject(doc);
    gas::LoadResult loaded = gas::parseProject(bytes.data(), bytes.size());
    ASSERT_NE(loaded.document, nullptr) << loaded.error;
    ASSERT_TRUE(loaded.document->layer(0).props.frame.has_value());
    EXPECT_EQ(*loaded.document->layer(0).props.frame, f); // f64: exact
    // Cropping keeps the box and anchor on the same pixels.
    ASSERT_TRUE(doc.crop({10, 5, 80, 80}));
    const LayerFrame& c = *doc.layer(0).props.frame;
    EXPECT_DOUBLE_EQ(c.anchor().x, f.anchor().x - 10);
    EXPECT_DOUBLE_EQ(c.anchor().y, f.anchor().y - 5);
}

TEST(LayerFrame, BadValuesAreRejected) {
    LayerFrame f = turnedFrame();
    f.offset.x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(gas::sanitizedFrame(f).has_value());
    f = turnedFrame();
    f.sx = 0.0; // would collapse the box
    ASSERT_TRUE(gas::sanitizedFrame(f).has_value());
    EXPECT_DOUBLE_EQ(gas::sanitizedFrame(f)->sx, 0.01);
}
