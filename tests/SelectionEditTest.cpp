// Document-level selection, transform, crop and layer-mask behaviour.
#include <gas/core/Blend.h>
#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/Fill.h>
#include <gas/core/Pencil.h>

#include <gtest/gtest.h>

using gas::Document;
using gas::IRect;
using gas::Rgba8;
using gas::Selection;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};
const Rgba8 kBlue{0, 0, 255, 255};
const Rgba8 kGreen{0, 255, 0, 255};

// A recognisable pattern: each pixel's colour encodes its position.
void paintPattern(Document& doc) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    for (int y = 0; y < doc.height(); ++y) {
        for (int x = 0; x < doc.width(); ++x) {
            img->setPixel({x, y}, Rgba8{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 77, 255});
        }
    }
    doc.endEdit("Pattern");
}

// Every pixel with zero selection coverage must equal `before`.
void expectUnchangedOutside(const Document& doc, const std::vector<Rgba8>& before, const Selection& sel) {
    const std::vector<Rgba8> after = doc.flatten();
    for (int y = 0; y < doc.height(); ++y) {
        for (int x = 0; x < doc.width(); ++x) {
            if (sel.coverage({x, y}) == 0) {
                const auto i = static_cast<std::size_t>(y * doc.width() + x);
                ASSERT_EQ(after[i], before[i]) << "pixel " << x << "," << y << " changed outside the selection";
            }
        }
    }
}

} // namespace

TEST(DocumentSelection, ChangesAreUndoable) {
    Document doc(100, 100);
    EXPECT_TRUE(doc.selection().isEmpty());
    const unsigned g0 = doc.selectionGeneration();
    ASSERT_TRUE(doc.setSelection(Selection::rect(100, 100, {10, 10, 20, 20})));
    EXPECT_NE(doc.selectionGeneration(), g0);
    EXPECT_FALSE(doc.setSelection(doc.selection())); // unchanged: no step
    ASSERT_TRUE(doc.invertSelection());
    EXPECT_EQ(doc.selection().coverage({15, 15}), 0);
    doc.undo();
    EXPECT_EQ(doc.selection().coverage({15, 15}), 255);
    ASSERT_TRUE(doc.deselect());
    EXPECT_TRUE(doc.selection().isEmpty());
    doc.undo();
    doc.undo();
    EXPECT_TRUE(doc.selection().isEmpty());
    EXPECT_FALSE(doc.setSelection(Selection(50, 50))); // wrong size
}

TEST(DocumentSelection, BrushCannotPaintOutsideSelection) {
    // Plan acceptance: selection must not affect pixels outside it.
    Document doc(600, 300);
    paintPattern(doc);
    const Selection sel = Selection::ellipse(600, 300, {200, 50, 200, 200}, true); // soft edges
    doc.setSelection(sel);
    const std::vector<Rgba8> before = doc.flatten();

    gas::BrushTool brush;
    brush.setColor(kRed);
    brush.settings().diameter = 80;
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    brush.begin(*img, {0, 150});
    for (int x = 0; x <= 600; x += 7) {
        brush.moveTo(*img, {double(x), 150 + (x % 50) - 25.0});
        doc.syncEdit(); // mid-stroke syncs must not compound
    }
    brush.end();
    ASSERT_TRUE(doc.endEdit("Brush"));

    expectUnchangedOutside(doc, before, sel);
    EXPECT_EQ(doc.compositePixel({300, 150}), kRed); // centre fully selected
    // Soft-edge pixels the brush covered get a blend: neither the original nor full red.
    int blended = 0;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 600; ++x) {
            const std::uint8_t c = sel.coverage({x, y});
            const Rgba8 now = doc.compositePixel({x, y});
            if (c > 30 && c < 220 && now != kRed && now != before[static_cast<std::size_t>(y * 600 + x)]) {
                ++blended;
            }
        }
    }
    EXPECT_GT(blended, 10);

    doc.undo();
    EXPECT_EQ(doc.flatten(), before);
}

TEST(DocumentSelection, RepeatedSyncsGiveSameResultAsOne) {
    const auto run = [](bool syncOften) {
        Document doc(256, 64);
        doc.setSelection(Selection::ellipse(256, 64, {0, 0, 256, 64}, true));
        gas::PencilTool pencil;
        pencil.setColor(kBlue);
        TiledImage* img = doc.beginEdit();
        pencil.begin(*img, {0, 32});
        for (int x = 0; x < 256; ++x) {
            pencil.moveTo(*img, {double(x), 32});
            if (syncOften) {
                doc.syncEdit();
            }
        }
        doc.endEdit("Pencil");
        return doc.flatten();
    };
    EXPECT_EQ(run(true), run(false));
}

TEST(DocumentSelection, FillAndClearRespectSelection) {
    Document doc(300, 300);
    paintPattern(doc);
    const Selection sel = Selection::rect(300, 300, {50, 60, 200, 100});
    doc.setSelection(sel);
    const auto before = doc.flatten();

    ASSERT_TRUE(doc.fillSelected(kGreen));
    expectUnchangedOutside(doc, before, sel);
    EXPECT_EQ(doc.compositePixel({100, 100}), kGreen);

    ASSERT_TRUE(doc.eraseSelected());
    expectUnchangedOutside(doc, before, sel);
    EXPECT_EQ(doc.compositePixel({100, 100}), Rgba8::transparent());

    // Flood fill on transparent area is clipped too.
    TiledImage* img = doc.beginEdit();
    gas::floodFill(*img, {100, 100}, kBlue);
    doc.endEdit("Fill");
    expectUnchangedOutside(doc, before, sel);
}

TEST(DocumentSelection, NoSelectionEditsWholeLayer) {
    Document doc(64, 64);
    ASSERT_TRUE(doc.fillSelected(kRed));
    EXPECT_EQ(doc.compositePixel({0, 0}), kRed);
    EXPECT_EQ(doc.compositePixel({63, 63}), kRed);
    ASSERT_TRUE(doc.eraseSelected());
    EXPECT_EQ(doc.compositePixel({5, 5}), Rgba8::transparent());
}

TEST(DocumentTransform, MoveSelectionLeavesOutsideUntouched) {
    // Plan acceptance: moving a selection is undo-safe and touches nothing else.
    Document doc(300, 200);
    paintPattern(doc);
    const auto original = doc.flatten();
    const Selection sel = Selection::rect(300, 200, {20, 30, 40, 50});
    doc.setSelection(sel);

    ASSERT_TRUE(doc.beginTransform());
    EXPECT_EQ(doc.transformSourceRect(), (IRect{20, 30, 40, 50}));
    EXPECT_FALSE(doc.canUndo()); // blocked mid-transform
    doc.setTransform(gas::Affine::translate(100, 10));
    doc.setTransform(gas::Affine::translate(150, 20)); // previews replace each other
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest));

    // Moved block holds the original pixels.
    EXPECT_EQ(doc.compositePixel({170, 50}), original[30 * 300 + 20]);
    EXPECT_EQ(doc.compositePixel({209, 99}), original[79 * 300 + 59]);
    // Hole is transparent.
    EXPECT_EQ(doc.compositePixel({25, 35}), Rgba8::transparent());
    // Pixels outside both the source and destination are unchanged.
    const auto after = doc.flatten();
    const IRect src{20, 30, 40, 50}, dst{170, 50, 40, 50};
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 300; ++x) {
            if (!src.contains({x, y}) && !dst.contains({x, y})) {
                ASSERT_EQ(after[static_cast<std::size_t>(y * 300 + x)], original[static_cast<std::size_t>(y * 300 + x)])
                    << x << "," << y;
            }
        }
    }
    // The selection followed the pixels.
    EXPECT_EQ(doc.selection().bounds(), dst);

    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.flatten(), original);
    EXPECT_EQ(doc.selection().bounds(), src);
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.flatten(), after);
}

TEST(DocumentTransform, CancelRestoresEverything) {
    Document doc(300, 300);
    paintPattern(doc);
    const auto original = doc.flatten();
    doc.setSelection(Selection::ellipse(300, 300, {50, 50, 120, 90}, true));
    const int steps = doc.undoSteps();
    ASSERT_TRUE(doc.beginTransform());
    doc.setTransform(gas::Affine::around({110, 95}, gas::Affine::rotate(0.7)));
    doc.cancelTransform();
    EXPECT_FALSE(doc.isTransforming());
    EXPECT_EQ(doc.flatten(), original);
    EXPECT_EQ(doc.undoSteps(), steps);
}

TEST(DocumentTransform, IdentityCommitIsNoOp) {
    Document doc(64, 64);
    paintPattern(doc);
    const auto original = doc.flatten();
    const int steps = doc.undoSteps();
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_FALSE(doc.commitTransform(gas::Interpolation::Bilinear));
    EXPECT_EQ(doc.flatten(), original);
    EXPECT_EQ(doc.undoSteps(), steps);
}

TEST(DocumentTransform, WholeLayerWhenNothingSelected) {
    Document doc(100, 100);
    TiledImage* img = doc.beginEdit();
    img->setPixel({10, 10}, kRed);
    img->setPixel({12, 15}, kBlue);
    doc.endEdit("Dots");
    ASSERT_TRUE(doc.beginTransform());
    EXPECT_EQ(doc.transformSourceRect(), (IRect{10, 10, 3, 6})); // content bounds
    doc.setTransform(gas::Affine::translate(50, 50));
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest));
    EXPECT_EQ(doc.compositePixel({60, 60}), kRed);
    EXPECT_EQ(doc.compositePixel({62, 65}), kBlue);
    EXPECT_EQ(doc.compositePixel({10, 10}), Rgba8::transparent());
    EXPECT_TRUE(doc.selection().isEmpty());
}

TEST(DocumentTransform, EmptyLayerCannotTransform) {
    Document doc(32, 32);
    EXPECT_FALSE(doc.beginTransform());
    doc.setSelection(Selection::rect(32, 32, {0, 0, 8, 8}));
    EXPECT_FALSE(doc.beginTransform()); // only transparent pixels selected
    EXPECT_FALSE(doc.isTransforming());
}

TEST(DocumentTransform, FlipsAndRotationsAreLossless) {
    Document doc(64, 48);
    paintPattern(doc);
    const auto original = doc.flatten();
    doc.setSelection(Selection::rect(64, 48, {8, 4, 20, 13})); // odd/even sizes

    ASSERT_TRUE(doc.flipContent(true));
    EXPECT_EQ(doc.undoLabel(), "Flip Horizontal");
    EXPECT_EQ(doc.compositePixel({27, 4}), original[4 * 64 + 8]); // left edge -> right edge
    EXPECT_EQ(doc.selection().bounds(), (IRect{8, 4, 20, 13}));
    ASSERT_TRUE(doc.flipContent(true));
    EXPECT_EQ(doc.flatten(), original);

    ASSERT_TRUE(doc.rotateContent90(true));
    const IRect turned = *doc.selection().bounds();
    EXPECT_EQ(turned.w, 13);
    EXPECT_EQ(turned.h, 20);
    ASSERT_TRUE(doc.rotateContent90(false));
    EXPECT_EQ(doc.selection().bounds(), (IRect{8, 4, 20, 13}));
    // Four quarter turns of a pixel block come back exactly.
    const auto beforeTurns = doc.flatten();
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(doc.rotateContent90(true));
    }
    EXPECT_EQ(doc.flatten(), beforeTurns);
}

TEST(DocumentTransform, ScaleDoublesBlock) {
    Document doc(64, 64);
    TiledImage* img = doc.beginEdit();
    img->setPixel({4, 4}, kRed);
    doc.endEdit("Dot");
    doc.setSelection(Selection::rect(64, 64, {4, 4, 1, 1}));
    ASSERT_TRUE(doc.beginTransform());
    doc.setTransform(gas::Affine::around({4, 4}, gas::Affine::scale(3, 3)));
    ASSERT_TRUE(doc.commitTransform(gas::Interpolation::Nearest));
    for (int y = 4; y < 7; ++y) {
        for (int x = 4; x < 7; ++x) {
            EXPECT_EQ(doc.compositePixel({x, y}), kRed) << x << "," << y;
        }
    }
    EXPECT_EQ(doc.compositePixel({7, 7}), Rgba8::transparent());
    EXPECT_EQ(doc.selection().bounds(), (IRect{4, 4, 3, 3}));
}

TEST(DocumentCrop, CropsAllLayersAndUndoes) {
    Document doc(600, 400);
    paintPattern(doc);
    doc.addLayer();
    TiledImage* img = doc.beginEdit();
    img->setPixel({300, 300}, kRed);
    doc.endEdit("Dot");
    const auto original = doc.flatten();

    ASSERT_TRUE(doc.crop({290, 280, 100, 50}));
    EXPECT_EQ(doc.width(), 100);
    EXPECT_EQ(doc.height(), 50);
    EXPECT_EQ(doc.compositePixel({10, 20}), kRed);
    EXPECT_EQ(doc.compositePixel({0, 0}), original[280 * 600 + 290]);
    EXPECT_TRUE(doc.selection().isEmpty());
    EXPECT_EQ(doc.flatten().size(), 5000u);

    doc.undo();
    EXPECT_EQ(doc.width(), 600);
    EXPECT_EQ(doc.flatten(), original);
    doc.redo();
    EXPECT_EQ(doc.width(), 100);

    // Tile-aligned crops share tiles instead of copying.
    Document big(1024, 1024);
    paintPattern(big);
    ASSERT_TRUE(big.crop({256, 512, 300, 300}));
    EXPECT_EQ(big.compositePixel({0, 0}), (Rgba8{0, 0, 77, 255})); // (256 & 255, 512 & 255)
    EXPECT_FALSE(big.crop({0, 0, 300, 300})); // whole canvas: no-op
}

TEST(LayerMask, HidesAndRevealsWithoutTouchingPixels) {
    Document doc(64, 64);
    ASSERT_TRUE(doc.fillSelected(kRed));
    ASSERT_TRUE(doc.addLayerMask(0));
    EXPECT_TRUE(doc.editingMask());
    EXPECT_EQ(doc.compositePixel({5, 5}), kRed); // fresh mask shows everything

    // Paint black on the mask: hidden in the composite, pixels untouched.
    TiledImage* m = doc.beginEdit();
    ASSERT_NE(m, nullptr);
    gas::drawLine(*m, {0, 5}, {63, 5}, Rgba8::black());
    doc.endEdit("Mask");
    EXPECT_EQ(doc.compositePixel({5, 5}), Rgba8::transparent());
    EXPECT_EQ(doc.layer(0).image.pixel({5, 5}), kRed);
    EXPECT_EQ(doc.compositePixel({5, 6}), kRed);

    // Disable the mask: everything shows again.
    auto p = doc.layer(0).props;
    p.maskEnabled = false;
    doc.setLayerProps(0, p);
    EXPECT_EQ(doc.compositePixel({5, 5}), kRed);
    doc.undo();
    EXPECT_EQ(doc.compositePixel({5, 5}), Rgba8::transparent());

    // Apply bakes it into alpha; undo brings the mask back.
    ASSERT_TRUE(doc.applyLayerMask(0));
    EXPECT_FALSE(doc.layer(0).mask.has_value());
    EXPECT_EQ(doc.layer(0).image.pixel({5, 5}).a, 0);
    EXPECT_EQ(doc.layer(0).image.pixel({5, 6}), kRed);
    doc.undo();
    EXPECT_TRUE(doc.layer(0).mask.has_value());
    EXPECT_EQ(doc.layer(0).image.pixel({5, 5}), kRed);

    ASSERT_TRUE(doc.removeLayerMask(0));
    EXPECT_EQ(doc.compositePixel({5, 5}), kRed);
    EXPECT_FALSE(doc.editingMask());
}

TEST(LayerMask, FromSelectionRevealsOnlySelection) {
    Document doc(300, 300);
    doc.fillSelected(kBlue);
    doc.setSelection(Selection::rect(300, 300, {100, 100, 50, 50}));
    ASSERT_TRUE(doc.addLayerMask(0));
    EXPECT_EQ(doc.compositePixel({120, 120}), kBlue);
    EXPECT_EQ(doc.compositePixel({10, 10}), Rgba8::transparent());
    EXPECT_EQ(doc.compositePixel({290, 290}), Rgba8::transparent());
}

TEST(LayerMask, ValueMapping) {
    EXPECT_EQ(gas::maskValue(Rgba8::transparent()), 255); // unpainted = shown
    EXPECT_EQ(gas::maskValue(Rgba8::black()), 0);
    EXPECT_EQ(gas::maskValue(Rgba8{255, 255, 255, 255}), 255);
    EXPECT_NEAR(gas::maskValue(Rgba8{0, 0, 0, 128}), 127, 1); // half-painted black
}
