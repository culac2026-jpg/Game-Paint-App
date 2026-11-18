#include <gas/core/Document.h>
#include <gas/core/Fill.h>
#include <gas/core/ProjectIO.h>

#include <gtest/gtest.h>
#include <zlib.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using gas::Document;
using gas::LayerProps;
using gas::Rgba8;
using gas::TiledImage;

namespace {

const Rgba8 kRed{255, 0, 0, 255};

class ProjectIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("gas_tests_" + std::to_string(stamp));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path dir_;
};

std::vector<std::uint8_t> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void dot(Document& doc, gas::Point p, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    img->setPixel(p, c);
    doc.endEdit("Dot");
}

// A layered document with varied content across several tiles.
std::unique_ptr<Document> makeSample() {
    auto doc = std::make_unique<Document>(700, 300);
    std::mt19937 rng(42);
    for (int l = 0; l < 3; ++l) {
        if (l > 0) {
            doc->addLayer();
        }
        TiledImage* img = doc->beginEdit();
        for (int i = 0; i < 500; ++i) {
            const auto c = static_cast<std::uint8_t>(rng() & 0xFF);
            img->setPixel({static_cast<int>(rng() % 700), static_cast<int>(rng() % 300)},
                          Rgba8{c, static_cast<std::uint8_t>(255 - c), static_cast<std::uint8_t>(l * 80),
                                static_cast<std::uint8_t>(c | 1)});
        }
        doc->endEdit("Noise");
    }
    LayerProps p = doc->layer(1).props;
    p.name = "Ánh sáng – highlights"; // non-ASCII name
    p.opacity = 0.35;
    p.locked = true;
    doc->setLayerProps(1, p);
    LayerProps q = doc->layer(2).props;
    q.visible = false;
    doc->setLayerProps(2, q);
    doc->setActiveLayer(1);
    return doc;
}

void expectSameDocument(const Document& a, const Document& b) {
    ASSERT_EQ(a.width(), b.width());
    ASSERT_EQ(a.height(), b.height());
    ASSERT_EQ(a.layerCount(), b.layerCount());
    EXPECT_EQ(a.activeLayerIndex(), b.activeLayerIndex());
    for (int i = 0; i < a.layerCount(); ++i) {
        const auto& la = a.layer(i);
        const auto& lb = b.layer(i);
        EXPECT_EQ(la.props.name, lb.props.name);
        EXPECT_EQ(la.props.visible, lb.props.visible);
        EXPECT_EQ(la.props.locked, lb.props.locked);
        EXPECT_NEAR(la.props.opacity, lb.props.opacity, 1e-6);
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                ASSERT_EQ(la.image.pixel({x, y}), lb.image.pixel({x, y}))
                    << "layer " << i << " at " << x << "," << y;
            }
        }
    }
}

struct DecodedPng {
    int width = 0, height = 0;
    std::vector<Rgba8> pixels;
    std::string error;
};

// Minimal decoder for what encodePng writes: 8-bit RGBA, filter 0, no interlace.
DecodedPng decodePng(const std::vector<std::uint8_t>& png) {
    DecodedPng out;
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8) != 0) {
        out.error = "bad signature";
        return out;
    }
    const auto be32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(png[at]) << 24 | static_cast<std::uint32_t>(png[at + 1]) << 16 |
               static_cast<std::uint32_t>(png[at + 2]) << 8 | png[at + 3];
    };
    std::vector<std::uint8_t> idat;
    for (std::size_t pos = 8; pos + 12 <= png.size();) {
        const std::uint32_t len = be32(pos);
        const std::string type(reinterpret_cast<const char*>(&png[pos + 4]), 4);
        const std::uint8_t* data = &png[pos + 8];
        const uLong crc = crc32(crc32(0, nullptr, 0), &png[pos + 4], 4 + len);
        if (crc != be32(pos + 8 + len)) {
            out.error = "bad CRC in " + type;
            return out;
        }
        if (type == "IHDR") {
            out.width = static_cast<int>(be32(pos + 8));
            out.height = static_cast<int>(be32(pos + 12));
            if (data[8] != 8 || data[9] != 6 || data[12] != 0) {
                out.error = "unexpected IHDR format";
                return out;
            }
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + len);
        }
        pos += 12 + len;
    }
    const std::size_t stride = 1 + static_cast<std::size_t>(out.width) * 4;
    std::vector<std::uint8_t> raw(stride * static_cast<std::size_t>(out.height));
    uLongf rawSize = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawSize, idat.data(), static_cast<uLong>(idat.size())) != Z_OK ||
        rawSize != raw.size()) {
        out.error = "inflate failed";
        return out;
    }
    for (int y = 0; y < out.height; ++y) {
        const std::uint8_t* row = &raw[static_cast<std::size_t>(y) * stride];
        if (row[0] != 0) {
            out.error = "unexpected filter";
            return out;
        }
        for (int x = 0; x < out.width; ++x) {
            const std::uint8_t* p = row + 1 + x * 4;
            out.pixels.push_back({p[0], p[1], p[2], p[3]});
        }
    }
    return out;
}

} // namespace

TEST_F(ProjectIOTest, RoundTripPreservesLayersAndPixels) {
    const auto doc = makeSample();
    const fs::path file = dir_ / "sample.gas";
    const gas::IoResult saved = gas::saveProject(*doc, file);
    ASSERT_TRUE(saved.ok) << saved.error;
    EXPECT_FALSE(fs::exists(dir_ / "sample.gas.tmp"));

    const gas::LoadResult loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    expectSameDocument(*doc, *loaded.document);
    EXPECT_FALSE(loaded.document->isModified());
    EXPECT_FALSE(loaded.document->canUndo());
    EXPECT_EQ(loaded.document->flatten(), doc->flatten());
}

TEST_F(ProjectIOTest, UndoHistoryStateSurvivesSaveOpen) {
    // Plan acceptance: 20+ undo/redo steps, verified after Save/Open.
    Document doc(256, 256);
    for (int i = 0; i < 22; ++i) {
        dot(doc, {i * 11, i * 5}, Rgba8{static_cast<std::uint8_t>(i * 11), 50, 90, 255});
    }
    for (int i = 0; i < 7; ++i) {
        doc.undo();
    }
    const fs::path file = dir_ / "history.gas";
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->flatten(), doc.flatten());
    EXPECT_EQ(loaded.document->compositePixel({14 * 11, 14 * 5}).a, 255);
    EXPECT_EQ(loaded.document->compositePixel({15 * 11, 15 * 5}).a, 0); // undone before save
}

TEST_F(ProjectIOTest, EmptyTilesAreNotStored) {
    Document doc(4096, 4096); // 256 tiles
    const fs::path file = dir_ / "empty.gas";
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    EXPECT_LT(fs::file_size(file), 200u);
}

TEST_F(ProjectIOTest, SaveOverwritesExistingFileAtomically) {
    const fs::path file = dir_ / "over.gas";
    Document a(32, 32);
    ASSERT_TRUE(gas::saveProject(a, file).ok);
    Document b(64, 16);
    const gas::IoResult second = gas::saveProject(b, file);
    ASSERT_TRUE(second.ok) << second.error;
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->width(), 64);
}

TEST_F(ProjectIOTest, UnicodePathsWork) {
    const fs::path file = dir_ / fs::path(u8"dự án – nhân vật.gas");
    Document doc(16, 16);
    dot(doc, {3, 3}, kRed);
    const gas::IoResult saved = gas::saveProject(doc, file);
    ASSERT_TRUE(saved.ok) << saved.error;
    ASSERT_TRUE(fs::exists(file));
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->compositePixel({3, 3}), kRed);
}

TEST_F(ProjectIOTest, RejectsDamagedFiles) {
    const auto doc = makeSample();
    const fs::path file = dir_ / "good.gas";
    ASSERT_TRUE(gas::saveProject(*doc, file).ok);
    const std::vector<std::uint8_t> good = readAll(file);

    // Flipped byte in the middle -> checksum mismatch.
    auto flipped = good;
    flipped[flipped.size() / 2] ^= 0x40;
    writeAll(dir_ / "flipped.gas", flipped);
    auto r = gas::loadProject(dir_ / "flipped.gas");
    EXPECT_FALSE(r.document);
    EXPECT_NE(r.error.find("damaged"), std::string::npos) << r.error;

    // Truncated file.
    writeAll(dir_ / "cut.gas", {good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 3)});
    EXPECT_FALSE(gas::loadProject(dir_ / "cut.gas").document);

    // Not a project at all.
    writeAll(dir_ / "text.gas", {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', '!', '!', '!', '!'});
    r = gas::loadProject(dir_ / "text.gas");
    EXPECT_FALSE(r.document);
    EXPECT_NE(r.error.find("Not a GameArt"), std::string::npos) << r.error;

    // Missing file.
    EXPECT_FALSE(gas::loadProject(dir_ / "nope.gas").document);
}

TEST_F(ProjectIOTest, RejectsNewerFormatVersion) {
    Document doc(8, 8);
    const fs::path file = dir_ / "future.gas";
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    auto bytes = readAll(file);
    bytes[8] = 99; // version field (little-endian u32 after the 8-byte magic)
    // Re-seal the checksum so only the version is "wrong".
    const uLong crc = crc32(crc32(0, nullptr, 0), bytes.data(), static_cast<uInt>(bytes.size() - 4));
    for (int i = 0; i < 4; ++i) {
        bytes[bytes.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
    writeAll(file, bytes);
    const auto r = gas::loadProject(file);
    EXPECT_FALSE(r.document);
    EXPECT_NE(r.error.find("newer version"), std::string::npos) << r.error;
}

TEST_F(ProjectIOTest, LayerMasksRoundTrip) {
    Document doc(300, 300);
    doc.fillSelected(kRed);
    doc.setSelection(gas::Selection::rect(300, 300, {10, 10, 100, 100}));
    ASSERT_TRUE(doc.addLayerMask(0));
    auto p = doc.layer(0).props;
    p.maskEnabled = false;
    doc.setLayerProps(0, p);
    doc.addLayer(); // a second layer without a mask

    const fs::path file = dir_ / "masks.gas";
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    const Document& d = *loaded.document;
    ASSERT_TRUE(d.layer(0).mask.has_value());
    EXPECT_FALSE(d.layer(1).mask.has_value());
    EXPECT_FALSE(d.layer(0).props.maskEnabled);
    for (int y = 0; y < 300; y += 7) {
        for (int x = 0; x < 300; x += 7) {
            ASSERT_EQ(d.layer(0).mask->pixel({x, y}), doc.layer(0).mask->pixel({x, y})) << x << "," << y;
        }
    }
    EXPECT_EQ(d.flatten(), doc.flatten());
}

TEST_F(ProjectIOTest, LoadsVersion1Files) {
    // A v1 file is a v2 file without LMSK chunks; patch the version field.
    Document doc(40, 40);
    dot(doc, {3, 4}, kRed);
    const fs::path file = dir_ / "v1.gas";
    ASSERT_TRUE(gas::saveProject(doc, file).ok);
    auto bytes = readAll(file);
    bytes[8] = 1;
    const uLong crc = crc32(crc32(0, nullptr, 0), bytes.data(), static_cast<uInt>(bytes.size() - 4));
    for (int i = 0; i < 4; ++i) {
        bytes[bytes.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
    writeAll(file, bytes);
    const auto loaded = gas::loadProject(file);
    ASSERT_TRUE(loaded.document) << loaded.error;
    EXPECT_EQ(loaded.document->compositePixel({3, 4}), kRed);
}

TEST_F(ProjectIOTest, PngExportKeepsSizeColoursAndTransparency) {
    // Plan acceptance: PNG keeps exact size, colours and transparent pixels.
    Document doc(300, 7);
    dot(doc, {0, 0}, kRed);
    dot(doc, {299, 6}, Rgba8{10, 20, 30, 40}); // semi-transparent stays straight alpha
    doc.addLayer();
    dot(doc, {150, 3}, Rgba8{0, 255, 0, 255});
    doc.addLayer();
    dot(doc, {5, 5}, Rgba8{1, 1, 1, 255});
    doc.setLayerProps(2, [&] { auto p = doc.layer(2).props; p.visible = false; return p; }());

    const fs::path file = dir_ / "out.png";
    const gas::IoResult r = gas::exportPng(doc, file);
    ASSERT_TRUE(r.ok) << r.error;
    const DecodedPng png = decodePng(readAll(file));
    ASSERT_TRUE(png.error.empty()) << png.error;
    EXPECT_EQ(png.width, 300);
    EXPECT_EQ(png.height, 7);
    EXPECT_EQ(png.pixels, doc.flatten());
    EXPECT_EQ(png.pixels[0], kRed);
    EXPECT_EQ(png.pixels[6 * 300 + 299], (Rgba8{10, 20, 30, 40}));
    EXPECT_EQ(png.pixels[3 * 300 + 150], (Rgba8{0, 255, 0, 255}));
    EXPECT_EQ(png.pixels[5 * 300 + 5], Rgba8::transparent()); // hidden layer excluded
    EXPECT_EQ(png.pixels[1], Rgba8::transparent());
}
