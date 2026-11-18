#include "gas/core/Atlas.h"
#include "gas/core/Document.h"
#include "gas/core/ProjectIO.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace gas;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};

void paint(Document& doc, Point p, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    img->setPixel(p, c);
    doc.endEdit("Paint");
}

Rgba8 at(const Document& doc, int frame, Point p) { return doc.flattenedFrame(frame).pixel(p); }

} // namespace

TEST(Animation, NewDocumentHasOneFrame) {
    Document doc(16, 16);
    EXPECT_EQ(doc.frameCount(), 1);
    EXPECT_EQ(doc.activeFrame(), 0);
    EXPECT_FALSE(doc.removeFrame(0));
    EXPECT_TRUE(doc.celEmpty(0, 0));
}

TEST(Animation, FramesHoldTheirOwnPixels) {
    Document doc(16, 16);
    paint(doc, {1, 1}, kRed);
    ASSERT_TRUE(doc.addFrame());
    EXPECT_EQ(doc.frameCount(), 2);
    EXPECT_EQ(doc.activeFrame(), 1);
    EXPECT_EQ(doc.compositePixel({1, 1}).a, 0); // the new frame is empty
    paint(doc, {2, 2}, kBlue);
    EXPECT_EQ(at(doc, 0, {1, 1}), kRed);
    EXPECT_EQ(at(doc, 0, {2, 2}).a, 0);
    EXPECT_EQ(at(doc, 1, {2, 2}), kBlue);
    ASSERT_TRUE(doc.setActiveFrame(0));
    EXPECT_EQ(doc.compositePixel({1, 1}), kRed);
    EXPECT_EQ(doc.compositePixel({2, 2}).a, 0);
}

TEST(Animation, UndoJumpsToTheFrameOfTheStep) {
    Document doc(16, 16);
    doc.addFrame(); // frame 1 active
    paint(doc, {3, 3}, kRed);
    doc.setActiveFrame(0); // view change, not a step
    EXPECT_EQ(doc.undoLabel(), "Paint");
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.activeFrame(), 1); // switched to where the stroke was made
    EXPECT_EQ(at(doc, 1, {3, 3}).a, 0);
    doc.setActiveFrame(0);
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.activeFrame(), 1);
    EXPECT_EQ(at(doc, 1, {3, 3}), kRed);
    EXPECT_EQ(at(doc, 0, {3, 3}).a, 0);
    ASSERT_TRUE(doc.undo()); // paint
    ASSERT_TRUE(doc.undo()); // new frame
    EXPECT_EQ(doc.frameCount(), 1);
    EXPECT_EQ(doc.activeFrame(), 0);
}

TEST(Animation, DuplicateSharesThenDiverges) {
    Document doc(300, 300);
    paint(doc, {260, 5}, kRed);
    ASSERT_TRUE(doc.duplicateFrame(0));
    EXPECT_EQ(doc.activeFrame(), 1);
    EXPECT_EQ(doc.cel(0, 0).sharedTile(1, 0), doc.cel(0, 1).sharedTile(1, 0)); // copy-on-write
    paint(doc, {260, 5}, kBlue);
    EXPECT_EQ(at(doc, 0, {260, 5}), kRed);
    EXPECT_EQ(at(doc, 1, {260, 5}), kBlue);
}

TEST(Animation, RemoveAndMoveKeepContentAndActiveFrame) {
    Document doc(8, 8);
    for (int f = 0; f < 4; ++f) { // frame f has a pixel at x = f
        if (f > 0) {
            doc.addFrame();
        }
        paint(doc, {f, 0}, kRed);
    }
    doc.setActiveFrame(1);
    ASSERT_TRUE(doc.moveFrame(1, 3)); // order now 0 2 3 1
    EXPECT_EQ(doc.activeFrame(), 3);  // follows its content
    EXPECT_EQ(at(doc, 3, {1, 0}), kRed);
    EXPECT_EQ(at(doc, 1, {2, 0}), kRed);
    ASSERT_TRUE(doc.removeFrame(3));
    EXPECT_EQ(doc.frameCount(), 3);
    EXPECT_EQ(doc.activeFrame(), 2);
    EXPECT_EQ(at(doc, 2, {3, 0}), kRed);
    doc.undo();
    doc.undo();
    EXPECT_EQ(at(doc, 1, {1, 0}), kRed); // original order back
    EXPECT_EQ(doc.frameCount(), 4);
}

TEST(Animation, DurationsAreClampedUndoableSteps) {
    Document doc(8, 8);
    doc.addFrame();
    EXPECT_TRUE(doc.setFrameDuration(1, 250));
    EXPECT_EQ(doc.frames()[1].duration, 250);
    EXPECT_TRUE(doc.setFrameDuration(0, 999999));
    EXPECT_EQ(doc.frames()[0].duration, Document::kMaxFrameDuration);
    EXPECT_EQ(doc.frames()[1].duration, 250);
    EXPECT_FALSE(doc.setFrameDuration(5, 10));
    doc.undo();
    EXPECT_EQ(doc.frames()[0].duration, 100);
    // New frames copy the active frame's duration.
    doc.setActiveFrame(1);
    doc.addFrame();
    EXPECT_EQ(doc.frames()[2].duration, 250);
}

TEST(Animation, TagsFollowFrameEdits) {
    Document doc(8, 8);
    for (int i = 0; i < 5; ++i) {
        doc.addFrame(); // 6 frames
    }
    ASSERT_TRUE(doc.setTags({{"idle", 0, 1, TagDirection::Forward},
                             {"walk", 2, 5, TagDirection::PingPong},
                             {"", 0, 0, TagDirection::Forward},
                             {"bad", 9, 3, TagDirection::Reverse}}));
    ASSERT_EQ(doc.tags().size(), 3u); // nameless dropped
    EXPECT_EQ(doc.tags()[2].from, 3); // clamped and ordered
    EXPECT_EQ(doc.tags()[2].to, 5);
    doc.setActiveFrame(0);
    doc.addFrame(); // inserted at 1, inside "idle"
    EXPECT_EQ(doc.tags()[0].to, 2);
    EXPECT_EQ(doc.tags()[1].from, 3);
    EXPECT_EQ(doc.tags()[1].to, 6);
    ASSERT_TRUE(doc.removeFrame(0));
    EXPECT_EQ(doc.tags()[0].from, 0);
    EXPECT_EQ(doc.tags()[0].to, 1);
    EXPECT_EQ(doc.tags()[1].from, 2);
}

TEST(Animation, LayersAndCropCoverEveryFrame) {
    Document doc(64, 64);
    doc.addFrame();
    doc.addFrame(); // 3 frames, active 2
    doc.addLayer();
    EXPECT_EQ(doc.layer(1).cels.size(), 3u);
    doc.setActiveFrame(0);
    paint(doc, {40, 40}, kRed);
    doc.setActiveFrame(2);
    paint(doc, {41, 41}, kBlue);
    ASSERT_TRUE(doc.crop({32, 32, 16, 16}));
    EXPECT_EQ(at(doc, 0, {8, 8}), kRed);
    EXPECT_EQ(at(doc, 2, {9, 9}), kBlue);
    EXPECT_EQ(doc.cel(1, 1).width(), 16);
    doc.undo();
    EXPECT_EQ(at(doc, 0, {40, 40}), kRed);
}

TEST(Animation, SaveOpenRoundTrip) {
    const auto path = std::filesystem::temp_directory_path() / "gas_animation_test.gas";
    {
        Document doc(32, 32);
        paint(doc, {1, 1}, kRed);
        doc.addFrame(); // frame 1 stays empty
        doc.addFrame();
        paint(doc, {2, 2}, kBlue);
        doc.setFrameDuration(2, 40);
        doc.setTags({{"run", 1, 2, TagDirection::PingPong}});
        doc.setActiveFrame(1);
        ASSERT_TRUE(saveProject(doc, path).ok);
    }
    LoadResult r = loadProject(path);
    ASSERT_TRUE(r.document) << r.error;
    const Document& doc = *r.document;
    ASSERT_EQ(doc.frameCount(), 3);
    EXPECT_EQ(doc.activeFrame(), 1);
    EXPECT_EQ(doc.frames()[2].duration, 40);
    EXPECT_EQ(at(doc, 0, {1, 1}), kRed);
    EXPECT_TRUE(doc.celEmpty(0, 1));
    EXPECT_EQ(at(doc, 2, {2, 2}), kBlue);
    ASSERT_EQ(doc.tags().size(), 1u);
    EXPECT_EQ(doc.tags()[0], (FrameTag{"run", 1, 2, TagDirection::PingPong}));
    std::filesystem::remove(path);
}

// Acceptance (v0.8): a multi-frame sprite sheet with one consistent pivot.
TEST(Animation, ExportSheetWithUniformPivot) {
    Document doc(32, 32);
    for (int f = 0; f < 4; ++f) { // a 6x8 body hopping: its bottom rises with the frame
        if (f > 0) {
            doc.addFrame();
        }
        TiledImage* img = doc.beginEdit();
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 6; ++x) {
                img->setPixel({13 + x, 24 - 2 * f + y}, Rgba8{static_cast<std::uint8_t>(40 * f), 200, 80, 255});
            }
        }
        doc.endEdit("Paint");
        doc.setFrameDuration(f, 80 + 10 * f);
    }
    doc.setTags({{"hop", 0, 3, TagDirection::Forward}});
    const PointF pivot{0.5, 1.0}; // feet at the canvas bottom centre

    // Grid strip: equal untrimmed cells, so the pivot is the same pixel everywhere.
    AtlasSettings grid = atlasPreset(AtlasPreset::Generic);
    grid.grid = true;
    grid.columns = 4;
    grid.padding = 1;
    Atlas sheet = packAtlas(spritesFromFrames(doc, "hero", pivot), grid);
    ASSERT_TRUE(sheet.ok()) << sheet.error;
    ASSERT_EQ(sheet.frames.size(), 4u);
    EXPECT_EQ(sheet.pages[0].width, 4 * 32 + 3);
    EXPECT_EQ(sheet.pages[0].height, 32);
    for (int f = 0; f < 4; ++f) {
        const AtlasFrame& fr = sheet.frames[static_cast<std::size_t>(f)];
        EXPECT_EQ(fr.frame, (IRect{f * 33, 0, 32, 32}));
        EXPECT_EQ(fr.pivot, pivot);
        EXPECT_EQ(fr.duration, 80 + 10 * f);
        EXPECT_EQ(fr.name, "hero_0" + std::to_string(f));
        // The body stays where it was painted inside its cell.
        EXPECT_EQ(sheet.pages[0].pixels[static_cast<std::size_t>((24 - 2 * f) * sheet.pages[0].width + fr.frame.x + 13)].g,
                  200);
    }
    sheet.tags = tagsForFrameSprites(doc);
    const std::string json = atlasJson(sheet, 0, "hero.png");
    EXPECT_NE(json.find("\"duration\": 110"), std::string::npos);
    EXPECT_NE(json.find("\"frameTags\": [\n      {\"name\": \"hop\", \"from\": 0, \"to\": 3, \"direction\": \"forward\"}"),
              std::string::npos);

    // Packed + trimmed (Unity): frames differ in trim, yet the pivot stays on
    // the same canvas pixel (16, 32) in every frame.
    const AtlasSettings unity = atlasPreset(AtlasPreset::Unity);
    const Atlas packed = packAtlas(spritesFromFrames(doc, "hero", pivot), unity);
    ASSERT_TRUE(packed.ok());
    for (const AtlasFrame& fr : packed.frames) {
        EXPECT_TRUE(fr.trimmed);
        EXPECT_DOUBLE_EQ(fr.pivot.x * fr.sourceWidth, 16.0);
        EXPECT_DOUBLE_EQ(fr.pivot.y * fr.sourceHeight, 32.0);
    }
    const std::string meta = unityMeta(packed, 0, unity, "hero.png");
    // Frame 3's body bottom is at y = 26 (6 px above the canvas bottom), 8 px tall:
    // Unity pivot y = 1 - (32 - 18) / 8 = -0.75.
    EXPECT_NE(meta.find("name: hero_03"), std::string::npos);
    EXPECT_NE(meta.find("pivot: {x: 0.5, y: -0.75}"), std::string::npos);

    // Per-slice animation: frames x slices, tags per slice.
    doc.setSlices({{"body", {8, 8, 16, 24}, {0.5, 1.0}}});
    const std::vector<AtlasSprite> sliced = spritesFromFrames(doc, "hero", pivot);
    ASSERT_EQ(sliced.size(), 4u);
    EXPECT_EQ(sliced[2].name, "body_02");
    EXPECT_EQ(sliced[2].width, 16);
    EXPECT_EQ(tagsForFrameSprites(doc)[0].name, "body_hop");
}

TEST(Animation, ExportFramePngs) {
    Document doc(4, 4);
    doc.addFrame();
    doc.addFrame();
    const auto dir = std::filesystem::temp_directory_path() / "gas_frames_test";
    std::filesystem::remove_all(dir);
    std::vector<std::filesystem::path> written;
    ASSERT_TRUE(exportFramePngs(doc, dir, "walk", 2, &written).ok);
    EXPECT_EQ(written.size(), 3u);
    EXPECT_TRUE(std::filesystem::exists(dir / "walk_02.png"));
    std::filesystem::remove_all(dir);
}
