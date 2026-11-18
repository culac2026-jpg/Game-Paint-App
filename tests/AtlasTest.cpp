#include "gas/core/Atlas.h"
#include "gas/core/Document.h"
#include "gas/core/Slicing.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace gas;
namespace fs = std::filesystem;

namespace {

// A sprite with a solid w x h body inset by `margin` transparent px, coloured by `seed`.
AtlasSprite makeSprite(const std::string& name, int w, int h, int margin, std::uint8_t seed) {
    AtlasSprite s{name, w + 2 * margin, h + 2 * margin, {}, {0.5, 1.0}};
    s.pixels.resize(static_cast<std::size_t>(s.width * s.height));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            s.pixels[static_cast<std::size_t>((y + margin) * s.width + x + margin)] =
                Rgba8{seed, static_cast<std::uint8_t>(x * 7), static_cast<std::uint8_t>(y * 5), 255};
        }
    }
    return s;
}

Rgba8 pagePixel(const AtlasPage& p, int x, int y) {
    return p.pixels[static_cast<std::size_t>(y * p.width + x)];
}

// Every frame's packed pixels equal the source sprite's pixels at its source rect.
void expectFramesMatch(const Atlas& atlas, const std::vector<AtlasSprite>& sprites) {
    for (std::size_t i = 0; i < sprites.size(); ++i) {
        const AtlasFrame& f = atlas.frames[i];
        const AtlasPage& page = atlas.pages[static_cast<std::size_t>(f.page)];
        for (int y = 0; y < f.frame.h; ++y) {
            for (int x = 0; x < f.frame.w; ++x) {
                const Rgba8 src = sprites[i].pixels[static_cast<std::size_t>((f.source.y + y) * sprites[i].width +
                                                                             f.source.x + x)];
                ASSERT_EQ(pagePixel(page, f.frame.x + x, f.frame.y + y), src) << f.name << " " << x << "," << y;
            }
        }
    }
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(Atlas, PacksWithoutOverlapRespectingPaddingAndBorder) {
    std::vector<AtlasSprite> sprites;
    for (int i = 0; i < 40; ++i) {
        sprites.push_back(makeSprite("s" + std::to_string(i), 5 + i % 13 * 3, 4 + i % 7 * 5, 0,
                                     static_cast<std::uint8_t>(i)));
    }
    AtlasSettings s;
    s.padding = 3;
    s.border = 2;
    s.extrude = 0;
    s.trim = false;
    s.powerOfTwo = true;
    const Atlas atlas = packAtlas(sprites, s);
    ASSERT_TRUE(atlas.ok()) << atlas.error;
    ASSERT_EQ(atlas.pages.size(), 1u);
    const AtlasPage& page = atlas.pages[0];
    EXPECT_EQ(page.width & (page.width - 1), 0);
    EXPECT_EQ(page.height & (page.height - 1), 0);
    for (std::size_t i = 0; i < atlas.frames.size(); ++i) {
        const IRect a = atlas.frames[i].frame;
        EXPECT_GE(a.x, 2);
        EXPECT_GE(a.y, 2);
        EXPECT_LE(a.right(), page.width - 2);
        EXPECT_LE(a.bottom(), page.height - 2);
        for (std::size_t j = i + 1; j < atlas.frames.size(); ++j) {
            const IRect b = atlas.frames[j].frame;
            const IRect grown{a.x - 3, a.y - 3, a.w + 6, a.h + 6};
            EXPECT_TRUE(grown.intersected(b).empty()) << i << " vs " << j;
        }
    }
    expectFramesMatch(atlas, sprites);
}

TEST(Atlas, TrimKeepsSourceGeometry) {
    const std::vector<AtlasSprite> sprites{makeSprite("hero", 10, 20, 4, 1), AtlasSprite{"empty", 8, 8, {}, {}}};
    AtlasSettings s;
    s.trim = true;
    s.padding = 0;
    std::vector<AtlasSprite> withPixels = sprites;
    withPixels[1].pixels.assign(64, Rgba8{});
    const Atlas atlas = packAtlas(withPixels, s);
    ASSERT_TRUE(atlas.ok());
    const AtlasFrame& f = atlas.frames[0];
    EXPECT_TRUE(f.trimmed);
    EXPECT_EQ(f.source, (IRect{4, 4, 10, 20}));
    EXPECT_EQ(f.sourceWidth, 18);
    EXPECT_EQ(f.sourceHeight, 28);
    EXPECT_EQ(f.frame.w, 10);
    EXPECT_EQ(atlas.frames[1].frame.w, 1); // fully transparent -> 1x1
    expectFramesMatch(atlas, withPixels);
}

TEST(Atlas, ExtrudeRepeatsEdgePixels) {
    const std::vector<AtlasSprite> sprites{makeSprite("a", 4, 3, 0, 9)};
    AtlasSettings s;
    s.extrude = 2;
    s.padding = 0;
    s.powerOfTwo = false;
    const Atlas atlas = packAtlas(sprites, s);
    ASSERT_TRUE(atlas.ok());
    const AtlasFrame& f = atlas.frames[0];
    const AtlasPage& p = atlas.pages[0];
    EXPECT_EQ(f.frame.x, 2);
    EXPECT_EQ(p.width, 8);
    EXPECT_EQ(p.height, 7);
    EXPECT_EQ(pagePixel(p, 0, 0), pagePixel(p, 2, 2));             // corner
    EXPECT_EQ(pagePixel(p, 7, 3), pagePixel(p, 5, 3));             // right edge
    EXPECT_EQ(pagePixel(p, 3, 6), pagePixel(p, 3, 4));             // bottom edge
    expectFramesMatch(atlas, sprites);
}

TEST(Atlas, OverflowGoesToMorePagesAndTooBigFails) {
    std::vector<AtlasSprite> sprites;
    for (int i = 0; i < 10; ++i) {
        sprites.push_back(makeSprite("t" + std::to_string(i), 60, 60, 0, static_cast<std::uint8_t>(i)));
    }
    AtlasSettings s;
    s.maxWidth = 128;
    s.maxHeight = 128;
    s.padding = 2;
    s.trim = false;
    const Atlas atlas = packAtlas(sprites, s);
    ASSERT_TRUE(atlas.ok()) << atlas.error;
    EXPECT_EQ(atlas.pages.size(), 3u); // 4 + 4 + 2
    for (const AtlasPage& p : atlas.pages) {
        EXPECT_LE(p.width, 128);
        EXPECT_LE(p.height, 128);
    }
    EXPECT_EQ(atlas.pages[2].width * atlas.pages[2].height, 64 * 128); // the last page shrinks
    expectFramesMatch(atlas, sprites);

    sprites.push_back(makeSprite("huge", 200, 10, 0, 1));
    const Atlas bad = packAtlas(sprites, s);
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.error.find("huge"), std::string::npos);
}

TEST(Atlas, DuplicateNamesBecomeUnique) {
    const Atlas atlas = packAtlas({makeSprite("a", 2, 2, 0, 1), makeSprite("a", 2, 2, 0, 2)}, AtlasSettings{});
    EXPECT_EQ(atlas.frames[0].name, "a");
    EXPECT_EQ(atlas.frames[1].name, "a_2");
}

TEST(Atlas, MetadataPivotsAndCoordinates) {
    // 18x28 source, opaque body at (4,4) 10x20; pivot at bottom centre of the source.
    AtlasSettings s;
    s.trim = true;
    s.padding = 0;
    s.border = 0;
    s.powerOfTwo = false;
    const Atlas atlas = packAtlas({makeSprite("hero", 10, 20, 4, 1)}, s);
    ASSERT_TRUE(atlas.ok());
    const AtlasFrame& f = atlas.frames[0];
    ASSERT_EQ(f.frame, (IRect{0, 0, 10, 20}));

    const std::string json = atlasJson(atlas, 0, "sheet.png");
    EXPECT_NE(json.find("\"frame\": {\"x\": 0, \"y\": 0, \"w\": 10, \"h\": 20}"), std::string::npos);
    EXPECT_NE(json.find("\"spriteSourceSize\": {\"x\": 4, \"y\": 4, \"w\": 10, \"h\": 20}"), std::string::npos);
    EXPECT_NE(json.find("\"sourceSize\": {\"w\": 18, \"h\": 28}"), std::string::npos);
    EXPECT_NE(json.find("\"pivot\": {\"x\": 0.5, \"y\": 1}"), std::string::npos);
    EXPECT_NE(json.find("\"image\": \"sheet.png\""), std::string::npos);

    // Cocos: trimmed centre (9,14) vs source centre (9,14) -> offset {0,0}.
    const std::string plist = cocosPlist(atlas, 0, "sheet.png");
    EXPECT_NE(plist.find("<string>{{0,0},{10,20}}</string>"), std::string::npos);
    EXPECT_NE(plist.find("<string>{0,0}</string>"), std::string::npos);
    EXPECT_NE(plist.find("<string>{{4,4},{10,20}}</string>"), std::string::npos);
    EXPECT_NE(plist.find("<string>{18,28}</string>"), std::string::npos);

    // Unity: pivot in source px = (9, 28); in the trimmed rect: x (9-4)/10 = 0.5,
    // y up: 1 - (28-4)/20 = -0.2 (below the trimmed rect, on the same pixel).
    const std::string meta = unityMeta(atlas, 0, s, "sheet.png");
    EXPECT_NE(meta.find("spriteMode: 2"), std::string::npos);
    EXPECT_NE(meta.find("name: hero"), std::string::npos);
    EXPECT_NE(meta.find("pivot: {x: 0.5, y: -0.2}"), std::string::npos);
    EXPECT_NE(meta.find("alignment: 9"), std::string::npos);
    EXPECT_NE(meta.find("        y: 0\n        width: 10\n        height: 20"), std::string::npos);
    EXPECT_EQ(meta, unityMeta(atlas, 0, s, "sheet.png")); // stable guid/ids across exports
}

TEST(Atlas, UnityRectsStartAtTheBottom) {
    AtlasSettings s;
    s.trim = false;
    s.padding = 0;
    s.powerOfTwo = false;
    const Atlas atlas = packAtlas({makeSprite("tall", 4, 30, 0, 1), makeSprite("small", 4, 4, 0, 2)}, s);
    ASSERT_TRUE(atlas.ok());
    const AtlasFrame& small = atlas.frames[1];
    const int pageH = atlas.pages[0].height;
    const std::string meta = unityMeta(atlas, 0, s, "x.png");
    const std::string expect = "name: small\n      rect:\n        serializedVersion: 2\n        x: " +
                               std::to_string(small.frame.x) + "\n        y: " +
                               std::to_string(pageH - small.frame.y - small.frame.h) + "\n";
    EXPECT_NE(meta.find(expect), std::string::npos) << meta;
}

TEST(Atlas, PresetsMatchTheirEngines) {
    EXPECT_EQ(atlasPreset(AtlasPreset::Unity).format, AtlasFormat::UnityMeta);
    EXPECT_TRUE(atlasPreset(AtlasPreset::UnityPixelArt).pointFilter);
    EXPECT_EQ(atlasPreset(AtlasPreset::CocosCreator).format, AtlasFormat::CocosPlist);
    EXPECT_EQ(atlasPreset(AtlasPreset::Generic).format, AtlasFormat::JsonHash);
    const std::string meta = unityMeta(packAtlas({makeSprite("p", 2, 2, 0, 1)}, atlasPreset(AtlasPreset::UnityPixelArt)),
                                       0, atlasPreset(AtlasPreset::UnityPixelArt), "p.png");
    EXPECT_NE(meta.find("filterMode: 0"), std::string::npos);
    EXPECT_NE(meta.find("spritePixelsToUnits: 16"), std::string::npos);
}

// Acceptance (v0.7): import a sheet -> slice -> edit -> repack, with pivots and padding per preset.
TEST(Atlas, ImportSliceEditRepack) {
    Document doc(256, 64);
    TiledImage* img = doc.beginEdit();
    for (int k = 0; k < 4; ++k) { // four frames on a 64 px grid, bodies of different sizes
        for (int y = 10; y < 50 + k * 3; ++y) {
            for (int x = 16 + k; x < 40; ++x) {
                img->setPixel({k * 64 + x, y}, Rgba8{static_cast<std::uint8_t>(60 * k), 128, static_cast<std::uint8_t>(x), 255});
            }
        }
    }
    doc.endEdit("Import");
    GridSliceOptions grid;
    grid.cellWidth = 64;
    grid.cellHeight = 64;
    ASSERT_TRUE(doc.setSlices(namedSlices(gridSlices(doc.flattenedImage(), grid), "run", {0.5, 1.0}), "Grid Slice"));
    ASSERT_EQ(doc.slices().size(), 4u);
    // Edit: paint a mark on frame 2.
    img = doc.beginEdit();
    img->setPixel({128 + 30, 30}, Rgba8{1, 2, 3, 255});
    doc.endEdit("Paint");

    const AtlasSettings s = atlasPreset(AtlasPreset::Unity);
    const std::vector<AtlasSprite> sprites = spritesFromSlices(doc);
    const Atlas atlas = packAtlas(sprites, s);
    ASSERT_TRUE(atlas.ok()) << atlas.error;
    ASSERT_EQ(atlas.frames.size(), 4u);
    expectFramesMatch(atlas, sprites);
    // The pivot stays on the same canvas pixel row for every frame (feet on the cell's bottom).
    for (const AtlasFrame& f : atlas.frames) {
        EXPECT_EQ(f.sourceHeight, 64);
        EXPECT_DOUBLE_EQ(f.pivot.y * f.sourceHeight, 64.0);
        EXPECT_EQ(f.name.substr(0, 4), "run_");
    }
    const AtlasFrame& f2 = atlas.frames[2];
    EXPECT_EQ(pagePixel(atlas.pages[0], f2.frame.x + 30 - f2.source.x, f2.frame.y + 30 - f2.source.y), (Rgba8{1, 2, 3, 255}));

    const fs::path dir = fs::temp_directory_path() / "gas_atlas_test";
    fs::remove_all(dir);
    std::vector<fs::path> written;
    ASSERT_TRUE(exportAtlas(atlas, s, dir / "run.png", &written).ok);
    ASSERT_EQ(written.size(), 2u);
    EXPECT_TRUE(fs::exists(dir / "run.png"));
    EXPECT_NE(readFile(dir / "run.png.meta").find("name: run_03"), std::string::npos);

    written.clear();
    ASSERT_TRUE(exportSlicePngs(doc, dir / "frames", true, 2, &written).ok);
    EXPECT_EQ(written.size(), 4u);
    EXPECT_TRUE(fs::exists(dir / "frames" / "run_00.png"));
    written.clear();
    ASSERT_TRUE(exportLayerPngs(doc, dir / "layers", &written).ok);
    EXPECT_EQ(written.size(), 1u);
    fs::remove_all(dir);
}

TEST(Atlas, MultiPageFileNames) {
    EXPECT_EQ(atlasPagePath("out/a.png", 0, 1), fs::path("out/a.png"));
    EXPECT_EQ(atlasPagePath("out/a.png", 1, 3), fs::path("out/a_1.png"));
    EXPECT_EQ(safeFileName("a/b:c?. "), "a_b_c_");
}
