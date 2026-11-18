// v1.0 QA: the sample project set round-trips exactly, damaged files never
// crash the loader, and long random edit sessions undo/redo exactly.
#include "gas/core/Brush.h"
#include "gas/core/Document.h"
#include "gas/core/ProjectIO.h"
#include "gas/core/SampleProjects.h"
#include "gas/core/Selection.h"
#include "gas/core/Slicing.h"
#include "gas/core/Vector.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>

using namespace gas;

namespace {

struct Rng { // SplitMix64: same sequence on every platform
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    double unit() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }
};

bool sameImage(const TiledImage& a, const TiledImage& b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        return false;
    }
    for (int ty = 0; ty < a.tilesY(); ++ty) {
        for (int tx = 0; tx < a.tilesX(); ++tx) {
            const TiledImage::Tile* ta = a.tile(tx, ty);
            const TiledImage::Tile* tb = b.tile(tx, ty);
            if (ta == tb) {
                continue;
            }
            const IRect r{tx * TiledImage::kTileSize, ty * TiledImage::kTileSize, std::min(TiledImage::kTileSize, a.width() - tx * TiledImage::kTileSize),
                          std::min(TiledImage::kTileSize, a.height() - ty * TiledImage::kTileSize)};
            if (copyPixels(a, r) != copyPixels(b, r)) {
                return false;
            }
        }
    }
    return true;
}

// The file bytes with a recomputed CRC trailer, so mutations reach the parser.
void fixCrc(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4) {
        return;
    }
    const std::size_t body = bytes.size() - 4;
    const uLong crc = crc32(0L, bytes.data(), static_cast<uInt>(body));
    for (int i = 0; i < 4; ++i) {
        bytes[body + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
}

// Content of the document independent of the view (active layer and frame).
std::vector<std::uint8_t> signature(Document& doc) {
    const int layer = doc.activeLayerIndex(), frame = doc.activeFrame();
    doc.setActiveLayer(0);
    doc.setActiveFrame(0);
    std::vector<std::uint8_t> bytes = serializeProject(doc);
    doc.setActiveLayer(layer);
    doc.setActiveFrame(frame);
    return bytes;
}

} // namespace

TEST(SampleProjects, BuildTheWholeSet) {
    const std::vector<SampleProject> samples = buildSampleProjects();
    ASSERT_EQ(samples.size(), 5u);
    for (const SampleProject& s : samples) {
        ASSERT_TRUE(s.document) << s.name;
        EXPECT_FALSE(s.document->isModified()) << s.name;
        EXPECT_FALSE(s.document->flattenedImage().snapshot().empty()) << s.name;
    }
    const Document& hero = *samples[0].document;
    EXPECT_EQ(hero.frameCount(), 6);
    EXPECT_EQ(hero.tags().size(), 2u);
    const Document& paint = *samples[1].document;
    EXPECT_EQ(paint.layerCount(), 10);
    EXPECT_TRUE(paint.layer(4).mask.has_value());
    const Document& ui = *samples[2].document;
    EXPECT_TRUE(ui.isVectorLayer(1));
    EXPECT_EQ(ui.slices()[0].border.left, 24);
}

TEST(SampleProjects, SaveLoadSaveIsByteExact) {
    for (const SampleProject& s : buildSampleProjects()) {
        SCOPED_TRACE(s.name);
        const std::vector<std::uint8_t> first = serializeProject(*s.document);
        LoadResult loaded = parseProject(first.data(), first.size());
        ASSERT_TRUE(loaded.document) << loaded.error;
        Document& doc = *loaded.document;
        EXPECT_EQ(serializeProject(doc), first);
        ASSERT_EQ(doc.frameCount(), s.document->frameCount());
        for (int f = 0; f < doc.frameCount(); ++f) {
            EXPECT_TRUE(sameImage(doc.flattenedFrame(f), s.document->flattenedFrame(f))) << "frame " << f;
        }
        EXPECT_EQ(doc.slices().size(), s.document->slices().size());
        EXPECT_EQ(doc.tags(), s.document->tags());
        for (int l = 0; l < doc.layerCount(); ++l) {
            EXPECT_EQ(doc.layer(l).props, s.document->layer(l).props);
            EXPECT_EQ(doc.isVectorLayer(l), s.document->isVectorLayer(l));
        }
    }
}

TEST(SampleProjects, UndoEverythingThenRedoEverything) {
    for (SampleProject& s : buildSampleProjects()) {
        SCOPED_TRACE(s.name);
        Document& doc = *s.document;
        const TiledImage finalImage = doc.flattenedImage();
        int steps = 0;
        while (doc.canUndo()) {
            ASSERT_TRUE(doc.undo());
            ++steps;
        }
        EXPECT_GE(steps, 3);
        EXPECT_TRUE(doc.flattenedImage().snapshot().empty() ||
                    sameImage(doc.flattenedImage(), Document(doc.width(), doc.height()).flattenedImage()));
        while (doc.canRedo()) {
            ASSERT_TRUE(doc.redo());
        }
        doc.setActiveFrame(0);
        EXPECT_TRUE(sameImage(doc.flattenedImage(), finalImage));
        EXPECT_FALSE(doc.isModified());
    }
}

TEST(Fuzz, DamagedProjectsNeverCrash) {
    std::vector<SampleProject> samples = buildSampleProjects();
    // The small samples: pixel hero (frames, tags, effects), UI kit (vectors,
    // borders) and the tileset (slices).
    for (int which : {0, 2, 4}) {
        const std::vector<std::uint8_t> good = serializeProject(*samples[static_cast<std::size_t>(which)].document);
        SCOPED_TRACE(samples[static_cast<std::size_t>(which)].name);
        Rng rng{static_cast<std::uint64_t>(which) * 7919u + 1u};
        int parsed = 0;
        for (int iter = 0; iter < 250; ++iter) {
            std::vector<std::uint8_t> bytes = good;
            const int kind = rng.below(4);
            if (kind == 0) { // flip a few bytes
                for (int k = 1 + rng.below(4); k > 0; --k) {
                    bytes[static_cast<std::size_t>(rng.below(static_cast<int>(bytes.size() - 4)))] ^=
                        static_cast<std::uint8_t>(1 + rng.below(255));
                }
            } else if (kind == 1) { // truncate (and keep a trailer)
                bytes.resize(static_cast<std::size_t>(8 + rng.below(static_cast<int>(bytes.size() - 8))));
            } else if (kind == 2) { // overwrite a 32-bit field with an extreme value
                const std::size_t at = static_cast<std::size_t>(8 + rng.below(static_cast<int>(bytes.size() - 16)));
                const std::uint32_t v = rng.below(2) ? 0xFFFFFFFFu : 0x7FFFFFFFu - static_cast<std::uint32_t>(rng.below(4));
                std::memcpy(bytes.data() + at, &v, 4);
            } else { // duplicate a random block
                const std::size_t at = static_cast<std::size_t>(8 + rng.below(static_cast<int>(bytes.size() - 16)));
                const std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(1 + rng.below(64)),
                                                              bytes.size() - 4 - at);
                bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(at + len));
            }
            fixCrc(bytes);
            LoadResult r = parseProject(bytes.data(), bytes.size());
            if (r.document) {
                // Whatever loads must be a usable document.
                ++parsed;
                Document& d = *r.document;
                for (int f = 0; f < d.frameCount(); ++f) {
                    (void)d.flattenedFrame(f);
                }
                const std::vector<std::uint8_t> again = serializeProject(d);
                EXPECT_TRUE(parseProject(again.data(), again.size()).document) << "iteration " << iter;
            } else {
                EXPECT_FALSE(r.error.empty());
            }
        }
        SUCCEED() << parsed << " of 250 mutated files still loaded";
    }
    // Garbage and edge sizes.
    for (std::size_t n : {0u, 1u, 7u, 12u, 15u, 16u, 64u}) {
        std::vector<std::uint8_t> bytes(n, 0xAB);
        if (n >= 4) {
            std::memcpy(bytes.data(), "GASP", std::min<std::size_t>(4, n));
        }
        fixCrc(bytes);
        EXPECT_FALSE(parseProject(bytes.data(), bytes.size()).document) << n;
    }
}

TEST(Stress, RandomEditSessionUndoesAndRedoesExactly) {
    Document doc(320, 200);
    BrushTool brush;
    Rng rng{20261118};
    std::vector<std::vector<std::uint8_t>> states{signature(doc)};
    int token = 1;
    for (int step = 0; step < 120; ++step) {
        const int op = rng.below(12);
        bool changed = false;
        switch (op) {
        case 0:
        case 1:
        case 2: { // brush stroke on the active layer
            TiledImage* img = doc.beginEdit();
            if (!img) {
                break;
            }
            brush.settings().diameter = 2 + rng.unit() * 30;
            brush.setColor({static_cast<std::uint8_t>(rng.below(256)), static_cast<std::uint8_t>(rng.below(256)),
                            static_cast<std::uint8_t>(rng.below(256)), 255});
            brush.setSeed(rng.next());
            PointF p{rng.unit() * 320, rng.unit() * 200};
            brush.begin(*img, p);
            for (int k = 0; k < 8; ++k) {
                p = {p.x + (rng.unit() - 0.5) * 80, p.y + (rng.unit() - 0.5) * 60};
                brush.moveTo(*img, p);
            }
            brush.end();
            changed = doc.endEdit("Brush");
            break;
        }
        case 3:
            if (doc.layerCount() < 6) {
                doc.addLayer();
                changed = true;
            }
            break;
        case 4: changed = doc.layerCount() > 1 && doc.removeLayer(rng.below(doc.layerCount())); break;
        case 5: {
            LayerProps p = doc.layer(doc.activeLayerIndex()).props;
            p.opacity = 0.2 + 0.8 * rng.unit();
            p.blendMode = static_cast<BlendMode>(rng.below(6));
            p.locked = false;
            changed = doc.setLayerProps(doc.activeLayerIndex(), p);
            break;
        }
        case 6:
            changed = doc.moveLayer(rng.below(doc.layerCount()), rng.below(doc.layerCount()));
            break;
        case 7:
            if (doc.frameCount() < 4) {
                changed = rng.below(2) ? doc.addFrame() : doc.duplicateFrame(doc.activeFrame());
            } else {
                changed = doc.removeFrame(rng.below(doc.frameCount()));
            }
            break;
        case 8: { // select a rectangle, fill it, deselect (three steps)
            const int x = rng.below(280), y = rng.below(160);
            doc.setSelection(Selection::rect(320, 200, {x, y, 40, 30}));
            states.push_back(signature(doc));
            if (doc.fillSelected({static_cast<std::uint8_t>(rng.below(256)), 40, 90, 255})) {
                states.push_back(signature(doc));
            }
            changed = doc.deselect();
            break;
        }
        case 9: { // vector layer edit (merged drag of two updates = one step)
            if (!doc.isVectorLayer(doc.activeLayerIndex())) {
                changed = doc.addVectorLayer({{ellipsePath({rng.unit() * 200, rng.unit() * 120, 60, 40}), {}}});
                break;
            }
            VectorShapes shapes = *doc.layer(doc.activeLayerIndex()).vector;
            shapes.push_back({rectanglePath({rng.unit() * 250, rng.unit() * 150, 30, 20}, 4), {}});
            ++token;
            doc.setVectorShapes(doc.activeLayerIndex(), shapes, "Drag", token);
            shapes.back() = transformedShape(shapes.back(), Affine::translate(5, 3));
            changed = doc.setVectorShapes(doc.activeLayerIndex(), shapes, "Drag", token);
            break;
        }
        case 10:
            changed = doc.setSlices({{"s" + std::to_string(step), {rng.below(200), rng.below(100), 50, 40},
                                      {rng.unit(), rng.unit()}, {rng.below(10), 0, rng.below(10), 0}}});
            break;
        case 11:
            doc.setActiveLayer(rng.below(doc.layerCount())); // view changes only
            doc.setActiveFrame(rng.below(doc.frameCount()));
            break;
        default: break;
        }
        if (changed) {
            states.push_back(signature(doc));
        }
    }
    ASSERT_GE(states.size(), 60u);
    // Walk all the way back, checking every intermediate state, then forward.
    for (std::size_t i = states.size() - 1; i > 0; --i) {
        ASSERT_TRUE(doc.undo()) << "undo to state " << i - 1;
        ASSERT_EQ(signature(doc), states[i - 1]) << "after undo to state " << i - 1;
    }
    EXPECT_FALSE(doc.canUndo());
    for (std::size_t i = 1; i < states.size(); ++i) {
        ASSERT_TRUE(doc.redo());
        ASSERT_EQ(signature(doc), states[i]) << "after redo to state " << i;
    }
    // And the final state survives save/load.
    const std::vector<std::uint8_t> bytes = serializeProject(doc);
    LoadResult loaded = parseProject(bytes.data(), bytes.size());
    ASSERT_TRUE(loaded.document);
    EXPECT_EQ(signature(*loaded.document), states.back());
}

TEST(Fuzz, HugeVectorCoordinatesAreClamped) {
    // Found by the fuzzer: a damaged file with enormous coordinates and stroke
    // widths took seconds to rasterize. Shapes are now clamped on every path in.
    Document doc(256, 256);
    VectorShape s;
    s.path.nodes = {{{-1e300, 5}, {-1e300, 5}, {1e12, -1e12}}, {{1e15, 9}, {-1e15, 3e14}, {1e15, 9}}};
    s.style.stroke = true;
    s.style.strokeWidth = 1e9;
    s.style.join = LineJoin::Round;
    s.style.cap = LineCap::Round;
    ASSERT_TRUE(doc.addVectorLayer({s}));
    const VectorShape& stored = doc.layer(doc.activeLayerIndex()).vector->front();
    EXPECT_EQ(stored.path.nodes[0].p.x, -kMaxVectorCoordinate);
    EXPECT_EQ(stored.path.nodes[1].in.y, kMaxVectorCoordinate);
    EXPECT_EQ(stored.style.strokeWidth, kMaxStrokeWidth);
    // And it survives a save/load round trip unchanged.
    const std::vector<std::uint8_t> bytes = serializeProject(doc);
    LoadResult loaded = parseProject(bytes.data(), bytes.size());
    ASSERT_TRUE(loaded.document);
    EXPECT_EQ(*loaded.document->layer(1).vector, *doc.layer(1).vector);
}
