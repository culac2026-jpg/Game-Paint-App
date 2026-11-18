#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/Fill.h>
#include <gas/core/Pencil.h>

#include <gtest/gtest.h>

#include <stdexcept>

using gas::Document;
using gas::LayerProps;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};
const Rgba8 kBlue{0, 0, 255, 255};

// Fills the active layer's whole canvas as one undo step.
void fillActive(Document& doc, Rgba8 color) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    gas::floodFill(*img, {0, 0}, color);
    doc.endEdit("Fill");
}

void dot(Document& doc, gas::Point p, Rgba8 color) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    img->setPixel(p, color);
    doc.endEdit("Dot");
}

} // namespace

TEST(Document, StartsWithOneUnmodifiedLayer) {
    Document doc(64, 32);
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_EQ(doc.layer(0).props.name, "Layer 1");
    EXPECT_FALSE(doc.isModified());
    EXPECT_FALSE(doc.canUndo());
    EXPECT_THROW(Document(0, 10), std::invalid_argument);
    EXPECT_THROW(Document(Document::kMaxSize + 1, 10), std::invalid_argument);
}

TEST(Document, PixelEditUndoRedo) {
    Document doc(300, 300);
    dot(doc, {10, 10}, kRed);
    EXPECT_TRUE(doc.isModified());
    EXPECT_EQ(doc.undoLabel(), "Dot");
    EXPECT_EQ(doc.compositePixel({10, 10}), kRed);

    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.compositePixel({10, 10}), Rgba8::transparent());
    EXPECT_FALSE(doc.isModified());

    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.compositePixel({10, 10}), kRed);
}

TEST(Document, EditWithoutChangesRecordsNothing) {
    Document doc(32, 32);
    doc.beginEdit();
    EXPECT_FALSE(doc.endEdit("Nothing"));
    EXPECT_FALSE(doc.canUndo());

    fillActive(doc, kRed);
    TiledImage* img = doc.beginEdit();
    gas::floodFill(*img, {0, 0}, kRed); // same colour: no-op
    EXPECT_FALSE(doc.endEdit("Fill"));
    EXPECT_EQ(doc.undoSteps(), 1);
}

TEST(Document, BrushStrokeIsOneUndoStep) {
    Document doc(600, 600);
    gas::BrushTool brush;
    brush.setColor(kBlue);
    brush.settings().diameter = 30;
    TiledImage* img = doc.beginEdit();
    brush.begin(*img, {20, 20});
    brush.moveTo(*img, {580, 580}); // crosses several tiles
    brush.end();
    ASSERT_TRUE(doc.endEdit("Brush"));
    EXPECT_EQ(doc.undoSteps(), 1);
    EXPECT_EQ(doc.compositePixel({300, 300}).a, 255);
    doc.undo();
    EXPECT_EQ(doc.compositePixel({300, 300}).a, 0);
}

TEST(Document, TwentyFiveUndosAndRedosRestoreExactly) {
    // Plan acceptance: at least 20 consecutive undo/redo steps.
    Document doc(256, 256);
    std::vector<std::vector<Rgba8>> states{doc.flatten()};
    for (int i = 0; i < 25; ++i) {
        if (i % 5 == 4) {
            doc.addLayer();
        }
        const auto v = static_cast<std::uint8_t>(i * 10);
        dot(doc, {i * 7, i * 3}, Rgba8{v, 100, 200, 255});
        states.push_back(doc.flatten());
    }
    EXPECT_EQ(doc.undoSteps(), 30); // 25 dots + 5 new layers

    for (int i = 25; i > 0; --i) {
        EXPECT_EQ(doc.flatten(), states[static_cast<std::size_t>(i)]) << "before undo " << i;
        doc.undo();
        if (i % 5 == 0) {
            doc.undo(); // the matching New Layer step
        }
    }
    EXPECT_EQ(doc.flatten(), states[0]);
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_FALSE(doc.isModified());

    while (doc.redo()) {
    }
    EXPECT_EQ(doc.flatten(), states.back());
    EXPECT_EQ(doc.layerCount(), 6);
}

TEST(Document, NewEditDiscardsRedo) {
    Document doc(16, 16);
    dot(doc, {1, 1}, kRed);
    doc.undo();
    EXPECT_TRUE(doc.canRedo());
    dot(doc, {2, 2}, kBlue);
    EXPECT_FALSE(doc.canRedo());
    EXPECT_EQ(doc.compositePixel({1, 1}), Rgba8::transparent());
}

TEST(Document, ModifiedTracksSavedState) {
    Document doc(16, 16);
    dot(doc, {1, 1}, kRed);
    doc.markSaved();
    EXPECT_FALSE(doc.isModified());
    doc.undo();
    EXPECT_TRUE(doc.isModified());
    doc.redo();
    EXPECT_FALSE(doc.isModified());

    doc.undo();
    dot(doc, {2, 2}, kBlue); // saved state is now unreachable
    EXPECT_TRUE(doc.isModified());
    doc.undo();
    EXPECT_TRUE(doc.isModified());
}

TEST(Document, LayerStructureEditsAreUndoable) {
    Document doc(32, 32);
    fillActive(doc, kRed);
    doc.addLayer();
    EXPECT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.activeLayerIndex(), 1);
    EXPECT_EQ(doc.layer(1).props.name, "Layer 2");
    fillActive(doc, kBlue);
    EXPECT_EQ(doc.compositePixel({0, 0}), kBlue);

    ASSERT_TRUE(doc.moveLayer(1, 0)); // blue below red
    EXPECT_EQ(doc.compositePixel({0, 0}), kRed);
    EXPECT_EQ(doc.activeLayerIndex(), 0);
    doc.undo();
    EXPECT_EQ(doc.compositePixel({0, 0}), kBlue);

    ASSERT_TRUE(doc.removeLayer(1));
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_EQ(doc.compositePixel({0, 0}), kRed);
    EXPECT_FALSE(doc.removeLayer(0)); // last layer stays
    doc.undo();
    EXPECT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.compositePixel({0, 0}), kBlue);

    ASSERT_TRUE(doc.duplicateLayer(0));
    EXPECT_EQ(doc.layer(1).props.name, "Layer 1 copy");
    EXPECT_EQ(doc.layer(1).image.tile(0, 0), doc.layer(0).image.tile(0, 0)); // shared
    doc.undo();
    EXPECT_EQ(doc.layerCount(), 2);
}

TEST(Document, DuplicateIsIndependentOfOriginal) {
    Document doc(32, 32);
    fillActive(doc, kRed);
    doc.duplicateLayer(0);
    fillActive(doc, kBlue); // edits the copy (now active)
    EXPECT_EQ(doc.layer(0).image.pixel({0, 0}), kRed);
    EXPECT_EQ(doc.layer(1).image.pixel({0, 0}), kBlue);
}

TEST(Document, PropertyEditsMergeAndUndo) {
    Document doc(8, 8);
    LayerProps p = doc.layer(0).props;
    for (double o : {0.9, 0.8, 0.7, 0.5}) {
        p.opacity = o;
        doc.setLayerProps(0, p);
    }
    EXPECT_EQ(doc.undoSteps(), 1); // merged
    EXPECT_DOUBLE_EQ(doc.layer(0).props.opacity, 0.5);
    doc.undo();
    EXPECT_DOUBLE_EQ(doc.layer(0).props.opacity, 1.0);
    doc.redo();
    EXPECT_DOUBLE_EQ(doc.layer(0).props.opacity, 0.5);

    p.opacity = 2.0; // clamped
    doc.setLayerProps(0, p);
    EXPECT_DOUBLE_EQ(doc.layer(0).props.opacity, 1.0);
    EXPECT_FALSE(doc.setLayerProps(0, doc.layer(0).props)); // no change
}

TEST(Document, PropertyEditAfterUndoStartsNewStep) {
    Document doc(8, 8);
    LayerProps p = doc.layer(0).props;
    p.name = "A";
    doc.setLayerProps(0, p);
    p.name = "B";
    doc.setLayerProps(0, p);
    doc.undo(); // back to "Layer 1"
    p.name = "C";
    doc.setLayerProps(0, p);
    EXPECT_EQ(doc.undoSteps(), 1);
    doc.undo();
    EXPECT_EQ(doc.layer(0).props.name, "Layer 1");
}

TEST(Document, LockedAndHiddenLayersRefuseEdits) {
    Document doc(8, 8);
    LayerProps p = doc.layer(0).props;
    p.locked = true;
    doc.setLayerProps(0, p);
    EXPECT_EQ(doc.beginEdit(), nullptr);
    EXPECT_FALSE(doc.isEditing());
    p.locked = false;
    p.visible = false;
    doc.setLayerProps(0, p);
    EXPECT_EQ(doc.beginEdit(), nullptr);
}

TEST(Document, UndoBlockedWhileEditing) {
    Document doc(8, 8);
    dot(doc, {1, 1}, kRed);
    doc.beginEdit();
    EXPECT_FALSE(doc.canUndo());
    EXPECT_FALSE(doc.undo());
    doc.endEdit("x");
    EXPECT_TRUE(doc.canUndo());
}

TEST(Document, HistoryMemoryLimitDropsOldestSteps) {
    Document doc(256, 256);
    for (int i = 0; i < 10; ++i) {
        dot(doc, {i, i}, Rgba8{static_cast<std::uint8_t>(i), 0, 0, 255});
    }
    EXPECT_EQ(doc.undoSteps(), 10);
    // Each dot step holds up to two 256 KiB tiles; allow about three steps.
    doc.setHistoryMemoryLimit(3 * 2 * sizeof(TiledImage::Tile));
    EXPECT_LE(doc.undoSteps(), 3);
    EXPECT_GE(doc.undoSteps(), 1);
    EXPECT_LE(doc.historyMemory(), 3 * 2 * sizeof(TiledImage::Tile));
    while (doc.undo()) {
    }
    EXPECT_EQ(doc.compositePixel({0, 0}).a, 255); // too old to undo
    EXPECT_TRUE(doc.isModified());                // the clean state was dropped
}

TEST(Document, TenLayersCompositeByOpacityVisibilityAndOrder) {
    // Plan acceptance: 10 layers with opacity, visibility and order composite correctly.
    Document doc(4, 4);
    for (int i = 1; i < 10; ++i) {
        doc.addLayer();
    }
    ASSERT_EQ(doc.layerCount(), 10);
    for (int i = 0; i < 10; ++i) {
        doc.setActiveLayer(i);
        const auto v = static_cast<std::uint8_t>(i * 25);
        dot(doc, {0, 0}, Rgba8{v, v, v, 255});
    }
    EXPECT_EQ(doc.compositePixel({0, 0}), (Rgba8{225, 225, 225, 255})); // top layer wins

    LayerProps top = doc.layer(9).props;
    top.visible = false;
    doc.setLayerProps(9, top);
    EXPECT_EQ(doc.compositePixel({0, 0}), (Rgba8{200, 200, 200, 255}));

    LayerProps l8 = doc.layer(8).props;
    l8.opacity = 0.5;
    doc.setLayerProps(8, l8);
    const Rgba8 mixed = doc.compositePixel({0, 0}); // 50% of 200 over 175
    EXPECT_NEAR(mixed.r, 188, 1);
    EXPECT_EQ(mixed.a, 255);

    doc.moveLayer(0, 9); // the black bottom layer moves to the top
    EXPECT_EQ(doc.compositePixel({0, 0}), (Rgba8{0, 0, 0, 255}));

    // compositeTile must agree with compositePixel.
    auto t = std::make_unique<TiledImage::Tile>();
    ASSERT_TRUE(doc.compositeTile(0, 0, *t));
    EXPECT_EQ(t->pixels[0], doc.compositePixel({0, 0}));
}

TEST(Document, FlattenCropsEdgeTiles) {
    Document doc(300, 10);
    dot(doc, {299, 9}, kRed);
    const auto px = doc.flatten();
    ASSERT_EQ(px.size(), 3000u);
    EXPECT_EQ(px.back(), kRed);
    EXPECT_EQ(px.front(), Rgba8::transparent());
}

TEST(Document, DirtyTilesCoverEditsAndStructureChanges) {
    Document doc(600, 300); // 3x2 tiles
    EXPECT_EQ(doc.takeDirtyTiles().size(), 6u); // initial: everything
    EXPECT_TRUE(doc.takeDirtyTiles().empty());
    dot(doc, {300, 10}, kRed);
    const auto dirty = doc.takeDirtyTiles();
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], (gas::Point{1, 0}));
    doc.undo();
    EXPECT_EQ(doc.takeDirtyTiles().size(), 1u);
    doc.addLayer();
    EXPECT_EQ(doc.takeDirtyTiles().size(), 6u);
}
