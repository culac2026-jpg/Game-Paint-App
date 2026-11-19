#include <gas/core/Document.h>
#include <gas/core/Selection.h>

#include <gtest/gtest.h>

#include <memory>

using gas::Document;
using gas::Rgba8;
using gas::Selection;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};

// 100x100 document whose only layer is red in [20, 80) x [20, 80).
std::unique_ptr<Document> redSquareDoc() {
    auto owned = std::make_unique<Document>(100, 100);
    Document& doc = *owned;
    gas::TiledImage* img = doc.beginEdit();
    for (int y = 20; y < 80; ++y) {
        for (int x = 20; x < 80; ++x) {
            img->setPixel({x, y}, kRed);
        }
    }
    doc.endEdit("Paint");
    doc.markSaved();
    return owned;
}

// Lasso triangle over the square's left half.
Selection lasso() { return Selection::polygon(100, 100, {{10, 10}, {50, 10}, {10, 90}}, true); }

} // namespace

TEST(Clipboard, CopySelectedKeepsPositionAndCoverage) {
    auto owned = redSquareDoc();
    Document& doc = *owned;
    doc.setSelection(lasso());
    const gas::TiledImage copy = doc.copySelected();
    EXPECT_EQ(copy.pixel({25, 25}), kRed);           // inside the lasso and the square
    EXPECT_EQ(copy.pixel({70, 70}).a, 0);            // in the square, outside the lasso
    EXPECT_EQ(copy.pixel({15, 15}).a, 0);            // in the lasso, nothing painted
    // Anti-aliased edge: partly selected pixels are partly opaque.
    bool partial = false;
    for (int x = 20; x < 50; ++x) {
        const int a = copy.pixel({x, 40}).a;
        partial = partial || (a > 0 && a < 255);
    }
    EXPECT_TRUE(partial);
    // No selection = the whole layer.
    doc.deselect();
    EXPECT_EQ(doc.copySelected().pixel({70, 70}), kRed);
}

TEST(Clipboard, LayerViaCopyIsOneStep) {
    auto owned = redSquareDoc();
    Document& doc = *owned;
    doc.setSelection(lasso());
    ASSERT_TRUE(doc.layerFromSelection(false));
    ASSERT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.activeLayerIndex(), 1);
    EXPECT_EQ(doc.layer(1).props.name, doc.layer(0).props.name + " copy");
    EXPECT_EQ(doc.layer(1).image.pixel({25, 25}), kRed);
    EXPECT_EQ(doc.layer(1).image.pixel({70, 70}).a, 0);
    EXPECT_EQ(doc.layer(0).image.pixel({25, 25}), kRed); // the source keeps its pixels
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.layerCount(), 1);
}

TEST(Clipboard, LayerViaCutMovesPixelsInOneStep) {
    auto owned = redSquareDoc();
    Document& doc = *owned;
    doc.setSelection(lasso());
    doc.markSaved();
    ASSERT_TRUE(doc.layerFromSelection(true));
    ASSERT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.layer(1).image.pixel({25, 25}), kRed);
    EXPECT_EQ(doc.layer(0).image.pixel({25, 25}).a, 0); // cut out of the source
    EXPECT_EQ(doc.layer(0).image.pixel({70, 70}), kRed); // outside the lasso untouched
    // Together the two layers still cover the square fully at the cut edge.
    for (int x = 20; x < 50; ++x) {
        const int a0 = doc.layer(0).image.pixel({x, 40}).a;
        const int a1 = doc.layer(1).image.pixel({x, 40}).a;
        EXPECT_GE(a0 + a1, 250) << x;
    }
    EXPECT_EQ(doc.undoLabel(), "Layer via Cut");
    ASSERT_TRUE(doc.undo()); // one undo restores everything
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_EQ(doc.layer(0).image.pixel({25, 25}), kRed);
    EXPECT_FALSE(doc.isModified());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.layer(0).image.pixel({25, 25}).a, 0);
    EXPECT_EQ(doc.layer(1).image.pixel({25, 25}), kRed);
}

TEST(Clipboard, NothingToCopyOrLockedSourceChangesNothing) {
    auto owned = redSquareDoc();
    Document& doc = *owned;
    doc.setSelection(Selection::rect(100, 100, {82, 82, 10, 10})); // empty area
    EXPECT_FALSE(doc.layerFromSelection(false));
    EXPECT_FALSE(doc.layerFromSelection(true));
    doc.setSelection(lasso());
    gas::LayerProps props = doc.layer(0).props;
    props.locked = true;
    doc.setLayerProps(0, props);
    const auto steps = doc.undoLabel();
    EXPECT_FALSE(doc.layerFromSelection(true)); // can't erase a locked layer
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_EQ(doc.undoLabel(), steps);
    EXPECT_TRUE(doc.layerFromSelection(false)); // copying from it is fine
}

TEST(Clipboard, PasteAddsLayerAndDeselectsInOneStep) {
    auto owned = redSquareDoc();
    Document& doc = *owned;
    doc.setSelection(lasso());
    gas::TiledImage pasted(100, 100);
    pasted.setPixel({5, 5}, kRed);
    ASSERT_TRUE(doc.pasteLayer("Pasted", pasted));
    EXPECT_EQ(doc.layerCount(), 2);
    EXPECT_EQ(doc.layer(1).props.name, "Pasted");
    EXPECT_TRUE(doc.selection().isEmpty());
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.layerCount(), 1);
    EXPECT_FALSE(doc.selection().isEmpty()); // the selection comes back too
    EXPECT_FALSE(doc.pasteLayer("Empty", gas::TiledImage(100, 100)));
}
