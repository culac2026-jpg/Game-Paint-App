#include "gas/core/Atlas.h"
#include "gas/core/Document.h"
#include "gas/core/ProjectIO.h"
#include "gas/core/Slicing.h"
#include "gas/core/Vector.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace gas;

namespace {

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};

VectorShape filled(VectorPath path, Rgba8 colour = kRed, bool antialias = true) {
    VectorShape s;
    s.path = std::move(path);
    s.style.fillColor = colour;
    s.style.antialias = antialias;
    return s;
}

VectorShape stroked(VectorPath path, double width, Rgba8 colour = kBlue) {
    VectorShape s;
    s.path = std::move(path);
    s.style.fill = false;
    s.style.stroke = true;
    s.style.strokeColor = colour;
    s.style.strokeWidth = width;
    return s;
}

VectorPath line(PointF a, PointF b) { return {{PathNode::corner(a), PathNode::corner(b)}, false}; }

int countAlpha(const TiledImage& img, int minAlpha = 1) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            n += img.pixel({x, y}).a >= minAlpha ? 1 : 0;
        }
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------- geometry

TEST(Vector, ShapeBuilders) {
    EXPECT_EQ(rectanglePath({0, 0, 10, 6}).nodes.size(), 4u);
    EXPECT_EQ(rectanglePath({0, 0, 10, 6}, 2).nodes.size(), 8u);
    EXPECT_EQ(rectanglePath({0, 0, 10, 6}, 99).nodes.size(), 6u); // pill: radius clamped to 3, top/bottom edges stay
    EXPECT_EQ(rectanglePath({0, 0, 6, 6}, 3).nodes.size(), 4u);   // circle-like: no zero-length edges
    const VectorPath star = polygonPath({10, 10}, 8, 5, 0.5);
    ASSERT_EQ(star.nodes.size(), 10u);
    EXPECT_EQ(star.nodes[0].p, (PointF{10, 2})); // first point straight up
    EXPECT_DOUBLE_EQ(std::hypot(star.nodes[1].p.x - 10, star.nodes[1].p.y - 10), 4.0);
    EXPECT_EQ(polygonPath({0, 0}, 1, 2).nodes.size(), 3u); // at least a triangle
}

TEST(Vector, FlattenedCircleStaysWithinTolerance) {
    const std::vector<PointF> pts = flattenPath(ellipsePath({0, 0, 200, 200}), 0.2);
    EXPECT_GT(pts.size(), 16u);
    for (PointF p : pts) {
        EXPECT_NEAR(std::hypot(p.x - 100, p.y - 100), 100.0, 0.1); // kappa error is ~0.03%
    }
}

TEST(Vector, HitTestAndTransform) {
    const VectorShapes shapes = {filled(rectanglePath({0, 0, 10, 10})), stroked(line({20, 5}, {30, 5}), 4)};
    EXPECT_EQ(hitTestShapes(shapes, {5, 5}, 1), 0);
    EXPECT_EQ(hitTestShapes(shapes, {25, 6.5}, 1), 1); // inside the 4 px stroke
    EXPECT_EQ(hitTestShapes(shapes, {25, 9}, 1), -1);
    EXPECT_EQ(hitTestShapes(shapes, {15, 5}, 1), -1);
    const VectorShape big = transformedShape(shapes[1], Affine::scale(2, 2));
    EXPECT_DOUBLE_EQ(big.style.strokeWidth, 8.0);
    EXPECT_EQ(big.path.nodes[1].p, (PointF{60, 10}));
    EXPECT_EQ(shapeBounds(shapes[0]), (IRect{-1, -1, 12, 12}));
}

// ---------------------------------------------------------------- rendering

TEST(Vector, FillCoverageIsExactOnPixelEdges) {
    const TiledImage img = rasterizeShapes({filled(rectanglePath({2, 3, 4, 2}))}, 10, 10);
    EXPECT_EQ(countAlpha(img), 8);
    EXPECT_EQ(countAlpha(img, 255), 8);
    EXPECT_EQ(img.pixel({2, 3}), kRed);
    EXPECT_EQ(img.pixel({5, 4}), kRed);
    EXPECT_EQ(img.pixel({6, 4}).a, 0);
}

TEST(Vector, HalfPixelEdgesAreAntialiased) {
    const TiledImage img = rasterizeShapes({filled(rectanglePath({1.5, 0, 2, 4}))}, 6, 4);
    EXPECT_NEAR(img.pixel({1, 1}).a, 128, 1);
    EXPECT_EQ(img.pixel({2, 1}).a, 255);
    EXPECT_NEAR(img.pixel({3, 1}).a, 128, 1);
    // Without anti-aliasing a pixel is in when its centre is: [1.5, 3.5) -> x = 1, 2.
    const TiledImage hard = rasterizeShapes({filled(rectanglePath({1.5, 0, 2, 4}), kRed, false)}, 6, 4);
    EXPECT_EQ(hard.pixel({1, 1}), kRed);
    EXPECT_EQ(hard.pixel({2, 1}), kRed);
    EXPECT_EQ(hard.pixel({3, 1}).a, 0);
    EXPECT_EQ(countAlpha(hard), 8);
}

TEST(Vector, StrokeWidthAndCaps) {
    VectorShape s = stroked(line({2, 5}, {10, 5}), 2);
    TiledImage img = rasterizeShapes({s}, 16, 10);
    EXPECT_EQ(countAlpha(img), 16); // rows 4-5, x 2..9, butt caps
    EXPECT_EQ(img.pixel({2, 4}), kBlue);
    EXPECT_EQ(img.pixel({9, 5}), kBlue);
    EXPECT_EQ(img.pixel({1, 5}).a, 0);
    EXPECT_EQ(img.pixel({10, 5}).a, 0);
    s.style.cap = LineCap::Square;
    img = rasterizeShapes({s}, 16, 10);
    EXPECT_EQ(countAlpha(img), 20); // one extra pixel column at each end
    EXPECT_EQ(img.pixel({10, 4}), kBlue);
    s.style.cap = LineCap::Round;
    img = rasterizeShapes({s}, 16, 10);
    EXPECT_GT(img.pixel({1, 4}).a, 0);
    EXPECT_LT(img.pixel({1, 4}).a, 255);
}

TEST(Vector, OverlappingStrokePiecesNeverDoubleBlend) {
    // A translucent zig-zag: joins and segment quads overlap, but the union is
    // painted once, so every fully covered pixel has exactly the colour's alpha.
    VectorShape s = stroked({{PathNode::corner({5, 5}), PathNode::corner({20, 20}), PathNode::corner({35, 5})}, false},
                            6, Rgba8{0, 0, 255, 128});
    for (LineJoin join : {LineJoin::Miter, LineJoin::Round, LineJoin::Bevel}) {
        s.style.join = join;
        const TiledImage img = rasterizeShapes({s}, 40, 30);
        int maxAlpha = 0;
        for (int y = 0; y < 30; ++y) {
            for (int x = 0; x < 40; ++x) {
                maxAlpha = std::max<int>(maxAlpha, img.pixel({x, y}).a);
            }
        }
        EXPECT_EQ(maxAlpha, 128) << static_cast<int>(join);
        EXPECT_EQ(img.pixel({20, 19}).a, 128); // the join itself
    }
}

TEST(Vector, MiterJoinsKeepSharpCorners) {
    VectorShape box = stroked(rectanglePath({4, 4, 10, 10}), 2);
    const TiledImage miter = rasterizeShapes({box}, 20, 20);
    EXPECT_EQ(miter.pixel({3, 3}), kBlue); // the outer corner pixel
    EXPECT_EQ(countAlpha(miter, 255), 12 * 12 - 8 * 8);
    box.style.join = LineJoin::Round;
    EXPECT_LT(rasterizeShapes({box}, 20, 20).pixel({3, 3}).a, 255);
    box.style.join = LineJoin::Bevel;
    EXPECT_LT(rasterizeShapes({box}, 20, 20).pixel({3, 3}).a, 255);
}

TEST(Vector, SvgPathData) {
    EXPECT_EQ(svgPathData(rectanglePath({0, 0, 2, 1.5})), "M0 0 L2 0 L2 1.5 L0 1.5 Z");
    EXPECT_EQ(svgPathData(line({1, 2}, {3, 4})), "M1 2 L3 4");
    const std::string e = svgPathElement(stroked(line({0, 0}, {1, 1}), 3, Rgba8{255, 128, 0, 128}));
    EXPECT_NE(e.find("fill=\"none\""), std::string::npos);
    EXPECT_NE(e.find("stroke=\"#ff8000\" stroke-width=\"3\" stroke-opacity=\"0.502\""), std::string::npos);
    EXPECT_NE(svgPathData(ellipsePath({0, 0, 10, 10})).find(" C"), std::string::npos);
}

// ---------------------------------------------------------------- document

TEST(VectorLayer, EditsAreUndoableAndRefuseRasterTools) {
    Document doc(32, 32);
    ASSERT_TRUE(doc.addVectorLayer({filled(rectanglePath({0, 0, 4, 4}))}));
    EXPECT_TRUE(doc.isVectorLayer(1));
    EXPECT_FALSE(doc.isVectorLayer(0));
    EXPECT_EQ(doc.compositePixel({1, 1}), kRed);
    EXPECT_EQ(doc.beginEdit(), nullptr); // raster tools can't paint shapes
    EXPECT_FALSE(doc.fillSelected(kBlue));

    ASSERT_TRUE(doc.setVectorShapes(1, {filled(rectanglePath({10, 10, 4, 4}), kBlue)}, "Move"));
    EXPECT_EQ(doc.compositePixel({1, 1}).a, 0);
    EXPECT_EQ(doc.compositePixel({11, 11}), kBlue);
    EXPECT_FALSE(doc.setVectorShapes(1, *doc.layer(1).vector)); // no change, no step
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.compositePixel({1, 1}), kRed);
    EXPECT_EQ(doc.compositePixel({11, 11}).a, 0);
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.compositePixel({11, 11}), kBlue);

    // A drag: one token = one step.
    const int steps = doc.undoSteps();
    for (int x = 0; x < 5; ++x) {
        doc.setVectorShapes(1, {filled(rectanglePath({double(x), 20, 4, 4}))}, "Drag", 7);
    }
    EXPECT_EQ(doc.undoSteps(), steps + 1);
    doc.setVectorShapes(1, {filled(rectanglePath({9, 20, 4, 4}))}, "Drag", 8); // next drag
    EXPECT_EQ(doc.undoSteps(), steps + 2);
    doc.undo();
    doc.undo();
    EXPECT_EQ(doc.compositePixel({11, 11}), kBlue);

    ASSERT_TRUE(doc.rasterizeVectorLayer(1));
    EXPECT_FALSE(doc.isVectorLayer(1));
    EXPECT_EQ(doc.compositePixel({11, 11}), kBlue);
    EXPECT_NE(doc.beginEdit(), nullptr);
    doc.endEdit("x");
}

TEST(VectorLayer, MaskStaysPaintable) {
    Document doc(16, 16);
    doc.addVectorLayer({filled(rectanglePath({0, 0, 16, 16}))});
    ASSERT_TRUE(doc.addLayerMask(1));
    TiledImage* mask = doc.beginEdit();
    ASSERT_NE(mask, nullptr);
    mask->setPixel({3, 3}, Rgba8::black());
    doc.endEdit("Mask");
    EXPECT_EQ(doc.compositePixel({3, 3}).a, 0);
    EXPECT_TRUE(doc.isVectorLayer(1));
    ASSERT_TRUE(doc.applyLayerMask(1)); // baking the mask makes it raster
    EXPECT_FALSE(doc.isVectorLayer(1));
}

TEST(VectorLayer, SameShapesInEveryFrame) {
    Document doc(16, 16);
    doc.addVectorLayer({filled(rectanglePath({0, 0, 4, 4}))});
    doc.addFrame();
    EXPECT_EQ(doc.compositePixel({1, 1}), kRed); // the new frame shows the shapes
    doc.setVectorShapes(1, {filled(rectanglePath({8, 8, 4, 4}), kBlue)});
    EXPECT_EQ(doc.flattenedFrame(0).pixel({9, 9}), kBlue);
    EXPECT_EQ(doc.flattenedFrame(0).pixel({1, 1}).a, 0);
    doc.undo();
    EXPECT_EQ(doc.flattenedFrame(1).pixel({1, 1}), kRed);
}

TEST(VectorLayer, CropAndTransformMoveTheShapes) {
    Document doc(40, 40);
    doc.addVectorLayer({filled(rectanglePath({10, 10, 6, 4}))});
    ASSERT_TRUE(doc.flipContent(true)); // flip about the content's centre: same place
    EXPECT_EQ(doc.compositePixel({10, 10}), kRed);
    ASSERT_TRUE(doc.beginTransform());
    doc.setTransform(Affine::translate(5, 0) * Affine::around({13, 12}, Affine::scale(2, 2)));
    ASSERT_TRUE(doc.commitTransform(Interpolation::Nearest));
    // Scaled about the centre to 12 x 8 at (7, 8), then moved 5 right: (12, 8).
    const RectF b = pathControlBounds(doc.layer(1).vector->front().path);
    EXPECT_DOUBLE_EQ(b.x, 12);
    EXPECT_DOUBLE_EQ(b.y, 8);
    EXPECT_DOUBLE_EQ(b.w, 12);
    EXPECT_DOUBLE_EQ(b.h, 8);
    EXPECT_EQ(doc.compositePixel({12, 8}), kRed);
    EXPECT_EQ(doc.compositePixel({23, 15}), kRed);
    EXPECT_EQ(doc.compositePixel({24, 16}).a, 0);
    ASSERT_TRUE(doc.crop({10, 5, 20, 20}));
    EXPECT_DOUBLE_EQ(pathControlBounds(doc.layer(1).vector->front().path).x, 2);
    EXPECT_DOUBLE_EQ(pathControlBounds(doc.layer(1).vector->front().path).y, 3);
    EXPECT_EQ(doc.compositePixel({2, 3}), kRed);
    doc.undo();
    EXPECT_EQ(doc.width(), 40);
    EXPECT_EQ(doc.compositePixel({12, 8}), kRed);
}

TEST(VectorLayer, SaveOpenKeepsShapesExactly) {
    const auto path = std::filesystem::temp_directory_path() / "gas_vector_test.gas";
    VectorShapes shapes = {filled(ellipsePath({1.25, 2.5, 13.1, 7.3})), stroked(polygonPath({20, 20}, 7, 5, 0.4), 1.5)};
    shapes[1].style.join = LineJoin::Round;
    shapes[1].style.cap = LineCap::Square;
    shapes[1].style.antialias = false;
    shapes[0].style.stroke = true;
    {
        Document doc(32, 32);
        doc.addVectorLayer(shapes);
        doc.addFrame();
        doc.setSlices({{"panel", {0, 0, 32, 32}, {0.5, 0.5}, {4, 5, 6, 7}}});
        ASSERT_TRUE(saveProject(doc, path).ok);
    }
    LoadResult r = loadProject(path);
    ASSERT_TRUE(r.document) << r.error;
    const Document& doc = *r.document;
    ASSERT_TRUE(doc.isVectorLayer(1));
    EXPECT_EQ(*doc.layer(1).vector, shapes);
    EXPECT_EQ(doc.cel(1, 1).pixel({7, 6}), rasterizeShapes(shapes, 32, 32).pixel({7, 6}));
    EXPECT_FALSE(doc.celEmpty(1, 0));
    ASSERT_EQ(doc.slices().size(), 1u);
    EXPECT_EQ(doc.slices()[0].border, (SliceBorder{4, 5, 6, 7}));
    std::filesystem::remove(path);
}

TEST(VectorLayer, SvgExport) {
    Document doc(20, 10);
    TiledImage* img = doc.beginEdit();
    img->setPixel({3, 4}, kRed);
    doc.endEdit("Dot");
    doc.addVectorLayer({filled(rectanglePath({1, 1, 5, 5}), kBlue)});
    LayerProps p = doc.layer(1).props;
    p.opacity = 0.5;
    p.blendMode = BlendMode::Multiply;
    p.name = "Icon <1>";
    doc.setLayerProps(1, p);
    const std::string svg = svgDocument(doc);
    EXPECT_NE(svg.find("viewBox=\"0 0 20 10\""), std::string::npos);
    EXPECT_NE(svg.find("<image x=\"3\" y=\"4\" width=\"1\" height=\"1\""), std::string::npos); // raster layer, trimmed
    EXPECT_NE(svg.find("opacity=\"0.500\" style=\"mix-blend-mode:multiply\""), std::string::npos);
    EXPECT_NE(svg.find("<title>Icon &lt;1&gt;</title>"), std::string::npos);
    EXPECT_NE(svg.find("<path d=\"M1 1 L6 1 L6 6 L1 6 Z\" fill=\"#0000ff\"/>"), std::string::npos);
    EXPECT_LT(svg.find("<image"), svg.find("<path")); // bottom layer first
}

// ---------------------------------------------------------------- 9-slice

TEST(NineSlice, BordersAreSanitized) {
    Document doc(64, 64);
    doc.setSlices({{"a", {0, 0, 10, 8}, {0.5, 0.5}, {6, -2, 9, 20}}});
    EXPECT_EQ(doc.slices()[0].border, (SliceBorder{6, 0, 4, 8}));
}

TEST(NineSlice, ScaleKeepsCornersAndStretchesTheRest) {
    // 4x4: corners 1 px (values 1..4), edges 5..8, centre 9.
    const Rgba8 c[10] = {{}, {1, 0, 0, 255}, {2, 0, 0, 255}, {3, 0, 0, 255}, {4, 0, 0, 255}, {5, 0, 0, 255},
                         {6, 0, 0, 255}, {7, 0, 0, 255}, {8, 0, 0, 255}, {9, 0, 0, 255}};
    const std::vector<Rgba8> src = {c[1], c[5], c[5], c[2], //
                                    c[7], c[9], c[9], c[8], //
                                    c[7], c[9], c[9], c[8], //
                                    c[3], c[6], c[6], c[4]};
    const std::vector<Rgba8> out = nineSliceScale(src, 4, 4, {1, 1, 1, 1}, 10, 6);
    ASSERT_EQ(out.size(), 60u);
    const auto at = [&](int x, int y) { return out[static_cast<std::size_t>(y * 10 + x)].r; };
    EXPECT_EQ(at(0, 0), 1);
    EXPECT_EQ(at(9, 0), 2);
    EXPECT_EQ(at(0, 5), 3);
    EXPECT_EQ(at(9, 5), 4);
    for (int x = 1; x < 9; ++x) {
        EXPECT_EQ(at(x, 0), 5);
        EXPECT_EQ(at(x, 5), 6);
        EXPECT_EQ(at(x, 3), 9);
    }
    EXPECT_EQ(at(0, 2), 7);
    EXPECT_EQ(at(9, 4), 8);
    // Smaller than the borders: corners shrink together.
    const std::vector<Rgba8> tiny = nineSliceScale(src, 4, 4, {2, 2, 2, 2}, 2, 2);
    EXPECT_EQ(tiny[0].r, 1);
    EXPECT_EQ(tiny[3].r, 4);
}

// Acceptance (v0.9): make an icon and a panel with vector tools, and export
// them as game assets (9-slice border in the engine metadata, SVG source).
TEST(NineSlice, IconAndPanelExport) {
    Document doc(96, 48);
    VectorShape panel = filled(rectanglePath({2, 2, 44, 44}, 8), Rgba8{40, 60, 90, 255});
    panel.style.stroke = true;
    panel.style.strokeColor = Rgba8{230, 200, 90, 255};
    panel.style.strokeWidth = 3;
    VectorShape star = filled(polygonPath({72, 24}, 20, 5, 0.45), Rgba8{250, 210, 60, 255});
    star.style.stroke = true;
    star.style.strokeColor = Rgba8{120, 70, 0, 255};
    star.style.join = LineJoin::Round;
    ASSERT_TRUE(doc.addVectorLayer({panel, star}));
    ASSERT_TRUE(doc.setSlices({{"panel", {0, 0, 48, 48}, {0.5, 0.5}, {12, 12, 12, 12}},
                               {"icon_star", {48, 0, 48, 48}, {0.5, 0.5}, {}}}));

    const AtlasSettings unity = atlasPreset(AtlasPreset::Unity);
    const Atlas atlas = packAtlas(spritesFromSlices(doc), unity);
    ASSERT_TRUE(atlas.ok()) << atlas.error;
    ASSERT_EQ(atlas.frames.size(), 2u);
    const AtlasFrame& panelFrame = atlas.frames[0];
    EXPECT_FALSE(panelFrame.trimmed); // a bordered sprite keeps its full rect
    EXPECT_EQ(panelFrame.frame.w, 48);
    EXPECT_EQ(panelFrame.border, (SliceBorder{12, 12, 12, 12}));
    EXPECT_TRUE(atlas.frames[1].trimmed); // the icon is trimmed as usual
    const std::string meta = unityMeta(atlas, 0, unity, "ui.png");
    EXPECT_NE(meta.find("name: panel"), std::string::npos);
    EXPECT_NE(meta.find("border: {x: 12, y: 12, z: 12, w: 12}"), std::string::npos);
    EXPECT_NE(meta.find("border: {x: 0, y: 0, z: 0, w: 0}"), std::string::npos); // the icon
    EXPECT_NE(atlasJson(atlas, 0, "ui.png").find("\"border\": {\"left\": 12, \"top\": 12"), std::string::npos);

    // The panel scaled as an engine would: corners and the gold frame stay crisp.
    const std::vector<Rgba8> panelPixels = copyPixels(doc.flattenedImage(), {0, 0, 48, 48});
    const std::vector<Rgba8> wide = nineSliceScale(panelPixels, 48, 48, {12, 12, 12, 12}, 160, 48);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 12; ++x) { // left corner columns unchanged
            EXPECT_EQ(wide[static_cast<std::size_t>(y * 160 + x)], panelPixels[static_cast<std::size_t>(y * 48 + x)]);
        }
    }

    const auto dir = std::filesystem::temp_directory_path() / "gas_ui_export";
    std::filesystem::remove_all(dir);
    std::vector<std::filesystem::path> written;
    std::filesystem::create_directories(dir);
    ASSERT_TRUE(exportAtlas(atlas, unity, dir / "ui.png", &written).ok);
    EXPECT_TRUE(std::filesystem::exists(dir / "ui.png"));
    EXPECT_TRUE(std::filesystem::exists(dir / "ui.png.meta"));
    ASSERT_TRUE(exportSvg(doc, dir / "ui.svg").ok);
    std::string svg;
    {
        std::ifstream in(dir / "ui.svg");
        std::stringstream text;
        text << in.rdbuf();
        svg = text.str();
    }
    EXPECT_NE(svg.find("<path d=\"M10 2 L38 2 C"), std::string::npos); // the rounded panel stays a path
    EXPECT_NE(svg.find("stroke=\"#e6c85a\" stroke-width=\"3\""), std::string::npos);
    std::filesystem::remove_all(dir);
}
