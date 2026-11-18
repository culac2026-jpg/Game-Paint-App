#include "gas/core/Document.h"
#include "gas/core/Plugins.h"
#include "gas/core/Selection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace gas;
namespace fs = std::filesystem;

namespace {

void paint(Document& doc, Point p, Rgba8 c) {
    TiledImage* img = doc.beginEdit();
    ASSERT_NE(img, nullptr);
    img->setPixel(p, c);
    doc.endEdit("Paint");
}

fs::path samplePlugin() { return fs::path(reinterpret_cast<const char8_t*>(GAS_SAMPLE_PLUGIN)); }

// --- an in-process plugin used to test the host itself

int32_t addOne(void*, GasImage* img, const double* values, char*, int32_t) {
    const int amount = static_cast<int>(values[0]);
    for (int y = 0; y < img->height; ++y) {
        for (int x = 0; x < img->width; ++x) {
            std::uint8_t* p = img->pixels + y * img->stride + x * 4;
            p[0] = static_cast<std::uint8_t>(std::min(255, p[0] + amount));
            p[3] = 255; // fills transparent pixels too
        }
    }
    return 0;
}

int32_t failing(void*, GasImage* img, const double*, char* error, int32_t size) {
    img->pixels[0] = 77; // must be discarded
    std::snprintf(error, static_cast<std::size_t>(size), "nope");
    return 3;
}

int32_t originCheck(void* user, GasImage* img, const double*, char*, int32_t) {
    auto* origin = static_cast<std::pair<int, int>*>(user);
    *origin = {img->originX, img->originY};
    return 0;
}

std::pair<int, int> gOrigin;

const GasParam kAmount[] = {{"amount", "Amount", GAS_PARAM_INT, 0, 50, 10, nullptr}};

int32_t testInit(GasHost* host) {
    host->setPluginName(host, "Test");
    GasFilter f{};
    f.structSize = sizeof f;
    f.id = "test.add";
    f.name = "Add Red";
    f.paramCount = 1;
    f.params = kAmount;
    f.apply = &addOne;
    EXPECT_EQ(host->registerFilter(host, &f), 0);
    EXPECT_NE(host->registerFilter(host, &f), 0); // duplicate id
    f.id = "test.fail";
    f.name = "Fail";
    f.paramCount = 0;
    f.params = nullptr;
    f.apply = &failing;
    EXPECT_EQ(host->registerFilter(host, &f), 0);
    f.id = "test.origin";
    f.name = "Origin";
    f.apply = &originCheck;
    f.user = &gOrigin;
    EXPECT_EQ(host->registerFilter(host, &f), 0);
    GasFilter bad{};
    bad.structSize = sizeof bad;
    bad.id = "test.bad"; // no name, no function
    EXPECT_NE(host->registerFilter(host, &bad), 0);
    return 0;
}

int32_t brokenInit(GasHost* host) {
    GasFilter f{};
    f.structSize = sizeof f;
    f.id = "broken.filter";
    f.name = "Broken";
    f.apply = &addOne;
    host->registerFilter(host, &f);
    return 5; // fails after registering
}

} // namespace

TEST(Plugins, InProcessFilterIsOneUndoStep) {
    PluginHost host;
    ASSERT_TRUE(host.registerPlugin("test", &testInit));
    ASSERT_EQ(host.filters().size(), 3u);
    EXPECT_EQ(host.plugins().back().name, "Test");
    EXPECT_EQ(host.filters()[0].plugin, "Test");
    const PluginFilter* add = host.findFilter("test.add");
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->defaults(), std::vector<double>{10});

    Document doc(300, 20); // spans two tiles
    paint(doc, {1, 1}, {100, 0, 0, 255});
    ASSERT_TRUE(applyPluginFilter(doc, *add, {20}).ok);
    EXPECT_EQ(doc.flattenedImage().pixel({1, 1}).r, 120);
    EXPECT_EQ(doc.flattenedImage().pixel({299, 19}), (Rgba8{20, 0, 0, 255}));
    EXPECT_EQ(doc.undoLabel(), "Add Red");
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.flattenedImage().pixel({1, 1}).r, 100);
    EXPECT_EQ(doc.flattenedImage().pixel({299, 19}).a, 0);
    // Values are clamped to the parameter range.
    ASSERT_TRUE(applyPluginFilter(doc, *add, {1000}).ok);
    EXPECT_EQ(doc.flattenedImage().pixel({1, 1}).r, 150);
}

TEST(Plugins, FilterStaysInsideTheSelection) {
    PluginHost host;
    host.registerPlugin("test", &testInit);
    Document doc(64, 64);
    doc.setSelection(Selection::rect(64, 64, {10, 12, 5, 6}));
    gOrigin = {-1, -1};
    ASSERT_TRUE(applyPluginFilter(doc, *host.findFilter("test.origin"), {}).ok);
    EXPECT_EQ(gOrigin, std::make_pair(10, 12)); // the image covers the selection bounds
    ASSERT_TRUE(applyPluginFilter(doc, *host.findFilter("test.add"), {}).ok);
    EXPECT_EQ(doc.flattenedImage().pixel({10, 12}), (Rgba8{10, 0, 0, 255}));
    EXPECT_EQ(doc.flattenedImage().pixel({14, 17}).a, 255);
    EXPECT_EQ(doc.flattenedImage().pixel({15, 12}).a, 0);
    EXPECT_EQ(doc.flattenedImage().pixel({9, 12}).a, 0);
}

TEST(Plugins, FailingFilterChangesNothing) {
    PluginHost host;
    host.registerPlugin("test", &testInit);
    Document doc(16, 16);
    const bool couldUndo = doc.canUndo();
    const IoResult r = applyPluginFilter(doc, *host.findFilter("test.fail"), {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "nope");
    EXPECT_EQ(doc.flattenedImage().pixel({0, 0}).a, 0);
    EXPECT_EQ(doc.canUndo(), couldUndo);
    EXPECT_FALSE(doc.isEditing());
}

TEST(Plugins, LockedOrVectorLayersAreRefused) {
    PluginHost host;
    host.registerPlugin("test", &testInit);
    Document doc(16, 16);
    LayerProps p = doc.layer(0).props;
    p.locked = true;
    doc.setLayerProps(0, p);
    EXPECT_FALSE(applyPluginFilter(doc, *host.findFilter("test.add"), {}).ok);
    doc.addVectorLayer();
    EXPECT_FALSE(applyPluginFilter(doc, *host.findFilter("test.add"), {}).ok);
}

TEST(Plugins, FailedInitRegistersNothing) {
    PluginHost host;
    EXPECT_FALSE(host.registerPlugin("broken", &brokenInit));
    EXPECT_TRUE(host.filters().empty());
    ASSERT_EQ(host.plugins().size(), 1u);
    EXPECT_FALSE(host.plugins()[0].loaded);
    EXPECT_FALSE(host.plugins()[0].error.empty());
}

TEST(Plugins, MissingOrInvalidLibrariesReportErrors) {
    PluginHost host;
    EXPECT_FALSE(host.loadLibrary("does-not-exist.dll"));
    ASSERT_EQ(host.plugins().size(), 1u);
    EXPECT_FALSE(host.plugins()[0].error.empty());
    host.loadDirectory("no/such/folder"); // no crash, nothing added
    EXPECT_EQ(host.plugins().size(), 1u);
}

TEST(Plugins, SamplePluginLoadsFromItsDll) {
    PluginHost host;
    ASSERT_TRUE(host.loadLibrary(samplePlugin())) << (host.plugins().empty() ? "" : host.plugins()[0].error);
    const PluginInfo& info = host.plugins()[0];
    EXPECT_EQ(info.name, "Sample Filters");
    EXPECT_EQ(info.filters, 3);
    EXPECT_EQ(info.exporters, 1);
    ASSERT_NE(host.findFilter("sample.invert"), nullptr);
    ASSERT_NE(host.findFilter("sample.grayscale"), nullptr);
    const PluginFilter* posterize = host.findFilter("sample.posterize");
    ASSERT_NE(posterize, nullptr);
    ASSERT_EQ(posterize->params.size(), 2u);
    EXPECT_EQ(posterize->params[0].type, GAS_PARAM_INT);
    EXPECT_EQ(host.findFilter("sample.grayscale")->params[0].choices.size(), 3u);

    Document doc(8, 8);
    paint(doc, {2, 2}, {200, 100, 0, 255});
    ASSERT_TRUE(applyPluginFilter(doc, *host.findFilter("sample.invert"), {0}).ok);
    EXPECT_EQ(doc.flattenedImage().pixel({2, 2}), (Rgba8{55, 155, 255, 255}));
    EXPECT_EQ(doc.flattenedImage().pixel({0, 0}).a, 0); // empty stays empty
    ASSERT_TRUE(applyPluginFilter(doc, *host.findFilter("sample.grayscale"), {1, 100}).ok);
    const Rgba8 g = doc.flattenedImage().pixel({2, 2});
    EXPECT_EQ(g.r, g.g);
    EXPECT_EQ(g.g, g.b);
    EXPECT_EQ(g.r, 155); // average of 55, 155, 255
}

TEST(Plugins, SampleExporterWritesSlicesCsv) {
    PluginHost host;
    host.loadDirectory(samplePlugin().parent_path());
    const PluginExporter* csv = host.findExporter("sample.slices-csv");
    ASSERT_NE(csv, nullptr);
    EXPECT_EQ(csv->extension, ".csv");
    Document doc(64, 64);
    doc.setSlices({{"hero \"idle\"", {0, 0, 32, 32}, {0.5, 1.0}, {}}, {"panel", {32, 0, 32, 32}, {0, 0}, {4, 5, 6, 7}}});
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path file = fs::temp_directory_path() / fs::path(u8"gas_plugin_" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(stamp).c_str())) + u8"_tệp.csv");
    ASSERT_TRUE(runPluginExporter(doc, *csv, file).ok);
    std::ostringstream text;
    text << std::ifstream(file).rdbuf();
    fs::remove(file);
    EXPECT_NE(text.str().find("\"hero \"\"idle\"\"\",0,0,32,32,0.5000,1.0000,0,0,0,0"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("\"panel\",32,0,32,32,0.0000,0.0000,4,5,6,7"), std::string::npos);
}

TEST(Plugins, ExporterSeesTheDocument) {
    PluginHost host;
    static int frames = 0, duration = 0, red = 0, layerAlpha = 0;
    static std::string layerName;
    host.registerPlugin("probe", [](GasHost* h) -> int32_t {
        GasExporter e{};
        e.structSize = sizeof e;
        e.id = "probe";
        e.name = "Probe";
        e.write = [](void*, const GasDocument* d, const char*, char*, int32_t) -> int32_t {
            frames = d->frameCount;
            duration = d->frameDuration(d, 1);
            GasImage img{};
            if (d->renderFrame(d, 1, &img) != 0) {
                return 1;
            }
            red = img.pixels[(3 * img.stride) + 3 * 4];
            GasLayerInfo info{};
            d->layerInfo(d, 0, &info);
            layerName = info.name;
            GasImage layer{};
            d->renderLayer(d, 0, 0, &layer);
            layerAlpha = layer.pixels[(3 * layer.stride) + 3 * 4 + 3];
            return d->renderFrame(d, 99, &img) == 0 ? 1 : 0; // out of range must fail
        };
        return h->registerExporter(h, &e);
    });
    Document doc(8, 8);
    doc.addFrame();
    doc.setFrameDuration(1, 250);
    paint(doc, {3, 3}, {222, 0, 0, 255});
    ASSERT_TRUE(runPluginExporter(doc, *host.findExporter("probe"), "unused").ok);
    EXPECT_EQ(frames, 2);
    EXPECT_EQ(duration, 250);
    EXPECT_EQ(red, 222);
    EXPECT_EQ(layerName, "Layer 1");
    EXPECT_EQ(layerAlpha, 0); // frame 0 of the layer is empty
}
