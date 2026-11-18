#include "gas/core/Document.h"
#include "gas/core/ProjectIO.h"
#include "gas/core/Slicing.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace gas;

namespace {

void fillRect(TiledImage& img, IRect r, Rgba8 c) {
    for (int y = r.y; y < r.bottom(); ++y) {
        for (int x = r.x; x < r.right(); ++x) {
            img.setPixel({x, y}, c);
        }
    }
}

constexpr Rgba8 kRed{255, 0, 0, 255};

} // namespace

TEST(Slicing, TrimmedRectFindsContent) {
    TiledImage img(600, 400);
    EXPECT_TRUE(trimmedRect(img, {0, 0, 600, 400}).empty());
    fillRect(img, {250, 10, 20, 5}, kRed); // crosses a tile boundary
    img.setPixel({300, 300}, {0, 0, 0, 10});
    EXPECT_EQ(trimmedRect(img, {0, 0, 600, 400}), (IRect{250, 10, 51, 291}));
    EXPECT_EQ(trimmedRect(img, {0, 0, 600, 400}, 10), (IRect{250, 10, 20, 5}));
    EXPECT_EQ(trimmedRect(img, {260, 0, 100, 100}), (IRect{260, 10, 10, 5}));
}

TEST(Slicing, GridSkipsEmptyCellsAndHonoursSpacing) {
    TiledImage img(70, 40);
    fillRect(img, {1, 1, 2, 2}, kRed);   // cell (0,0)
    fillRect(img, {40, 25, 2, 2}, kRed); // cell (1,1) with offset 2 / spacing 4: x 38..70? see below
    GridSliceOptions o;
    o.cellWidth = 32;
    o.cellHeight = 16;
    o.offsetX = 0;
    o.offsetY = 0;
    o.spacingX = 4;
    o.spacingY = 4;
    o.skipEmpty = false;
    // x: 0, 36 (36+32 = 68 <= 70); y: 0, 20 (20+16 = 36 <= 40)
    std::vector<IRect> all = gridSlices(img, o);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[1], (IRect{36, 0, 32, 16}));
    EXPECT_EQ(all[2], (IRect{0, 20, 32, 16}));
    o.skipEmpty = true;
    std::vector<IRect> used = gridSlices(img, o);
    ASSERT_EQ(used.size(), 2u);
    EXPECT_EQ(used[0], (IRect{0, 0, 32, 16}));
    EXPECT_EQ(used[1], (IRect{36, 20, 32, 16}));
}

TEST(Slicing, AutoFindsIslandsInReadingOrder) {
    TiledImage img(300, 300);
    fillRect(img, {200, 12, 10, 10}, kRed); // row 1, right (slightly lower top)
    fillRect(img, {10, 10, 20, 20}, kRed);  // row 1, left
    fillRect(img, {32, 15, 3, 3}, kRed);    // 2 px from the left one -> merged into it
    fillRect(img, {50, 100, 10, 30}, kRed); // row 2
    img.setPixel({150, 200}, kRed);         // speck, ignored
    // Diagonal neighbours are connected (8-connectivity).
    img.setPixel({270, 270}, kRed);
    img.setPixel({271, 271}, kRed);
    img.setPixel({272, 272}, kRed);
    img.setPixel({273, 273}, kRed);
    AutoSliceOptions o;
    o.mergeDistance = 2;
    o.minPixels = 4;
    const std::vector<IRect> r = autoSlices(img, o);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0], (IRect{10, 10, 25, 20}));
    EXPECT_EQ(r[1], (IRect{200, 12, 10, 10}));
    EXPECT_EQ(r[2], (IRect{50, 100, 10, 30}));
    EXPECT_EQ(r[3], (IRect{270, 270, 4, 4}));

    o.mergeDistance = 0; // the 2 px gap now separates them (3x3 = 9 px >= minPixels)
    EXPECT_EQ(autoSlices(img, o).size(), 5u);
}

TEST(Slicing, NamesAreNumberedAndUnique) {
    const std::vector<Slice> s = namedSlices({{0, 0, 1, 1}, {1, 0, 1, 1}}, "hero", {0.5, 1.0}, 9);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].name, "hero_09");
    EXPECT_EQ(s[1].name, "hero_10");
    EXPECT_EQ(s[1].pivot, (PointF{0.5, 1.0}));
    EXPECT_EQ(uniqueName("a", {"a", "a_2"}), "a_3");
    EXPECT_EQ(uniqueName("b", {"a"}), "b");
}

TEST(DocumentSlices, UndoRedoClipAndMerge) {
    Document doc(100, 80);
    EXPECT_TRUE(doc.setSlices({{"a", {90, 70, 20, 20}, {0.5, 0.5}}, {"gone", {200, 200, 5, 5}, {0, 0}}}));
    ASSERT_EQ(doc.slices().size(), 1u);
    EXPECT_EQ(doc.slices()[0].rect, (IRect{90, 70, 10, 10})); // clipped to the canvas
    EXPECT_FALSE(doc.setSlices(doc.slices()));               // no change, no step

    std::vector<Slice> s = doc.slices();
    for (int i = 0; i < 5; ++i) {
        s[0].pivot.y = 0.1 * i;
        doc.setSlices(s, "Slice Pivot", true);
    }
    EXPECT_EQ(doc.undoSteps(), 2); // the five pivot edits merged into one
    EXPECT_DOUBLE_EQ(doc.slices()[0].pivot.y, 0.4);
    doc.undo();
    EXPECT_DOUBLE_EQ(doc.slices()[0].pivot.y, 0.5);
    doc.undo();
    EXPECT_TRUE(doc.slices().empty());
    const unsigned g = doc.slicesGeneration();
    doc.redo();
    EXPECT_EQ(doc.slices().size(), 1u);
    EXPECT_NE(doc.slicesGeneration(), g);
}

TEST(DocumentSlices, CropMovesSlices) {
    Document doc(100, 100);
    doc.setSlices({{"in", {20, 20, 10, 10}, {0.5, 1}}, {"edge", {5, 5, 20, 20}, {0, 0}}, {"out", {0, 0, 5, 5}, {0, 0}}});
    ASSERT_TRUE(doc.crop({10, 10, 50, 50}));
    ASSERT_EQ(doc.slices().size(), 2u);
    EXPECT_EQ(doc.slices()[0].rect, (IRect{10, 10, 10, 10}));
    EXPECT_EQ(doc.slices()[1].rect, (IRect{0, 0, 15, 15}));
    doc.undo();
    EXPECT_EQ(doc.slices().size(), 3u);
    EXPECT_EQ(doc.slices()[0].rect, (IRect{20, 20, 10, 10}));
}

TEST(DocumentSlices, SurviveSaveAndOpen) {
    const auto path = std::filesystem::temp_directory_path() / "gas_slices_test.gas";
    {
        Document doc(64, 32);
        doc.setSlices({{"hero_idle", {0, 0, 32, 32}, {0.5, 1.0}}, {"Kiếm", {32, 4, 16, 20}, {0.25, -0.5}}});
        ASSERT_TRUE(saveProject(doc, path).ok);
    }
    LoadResult r = loadProject(path);
    ASSERT_TRUE(r.document) << r.error;
    ASSERT_EQ(r.document->slices().size(), 2u);
    EXPECT_EQ(r.document->slices()[0].name, "hero_idle");
    EXPECT_EQ(r.document->slices()[1].name, "Kiếm");
    EXPECT_EQ(r.document->slices()[1].rect, (IRect{32, 4, 16, 20}));
    EXPECT_FLOAT_EQ(static_cast<float>(r.document->slices()[1].pivot.y), -0.5f);
    EXPECT_EQ(r.document->undoSteps(), 0); // loading is not an edit
    std::filesystem::remove(path);
}

TEST(DocumentSlices, FlattenedImageMatchesComposite) {
    Document doc(300, 10);
    TiledImage* img = doc.beginEdit();
    fillRect(*img, {290, 2, 5, 5}, kRed);
    doc.endEdit("Paint");
    const TiledImage flat = doc.flattenedImage();
    EXPECT_EQ(flat.pixel({292, 3}), kRed);
    EXPECT_EQ(flat.tile(0, 0), nullptr); // empty tiles stay sparse
}
