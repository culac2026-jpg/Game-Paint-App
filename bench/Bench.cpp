// gas_bench: performance numbers for the v1.0 acceptance criteria (stroke
// latency and peak RAM on large canvases, composite and file times).
//   gas_bench                    run all benchmarks
//   gas_bench --write-samples D  also write the sample projects to folder D
// Build the release preset for meaningful numbers.
#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/Effects.h>
#include <gas/core/ProjectIO.h>
#include <gas/core/SampleProjects.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace gas;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

double peakMiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0;
}

double currentMiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0;
}

struct Stats {
    double mean = 0, p95 = 0, max = 0;
};

Stats stats(std::vector<double> v) {
    Stats s;
    if (v.empty()) {
        return s;
    }
    std::sort(v.begin(), v.end());
    for (double x : v) {
        s.mean += x;
    }
    s.mean /= static_cast<double>(v.size());
    s.p95 = v[std::min(v.size() - 1, static_cast<std::size_t>(static_cast<double>(v.size()) * 0.95))];
    s.max = v.back();
    return s;
}

// What the canvas does per pointer event: tool step, syncEdit, then
// re-composite the dirty tiles.
Stats strokeLatency(Document& doc, BrushTool& brush, double radius, int events) {
    std::vector<double> times;
    TiledImage::Tile scratch;
    TiledImage* img = doc.beginEdit();
    if (!img) {
        return {};
    }
    const double cx = doc.width() / 2.0, cy = doc.height() / 2.0;
    for (int i = 0; i <= events; ++i) {
        // A fast spiral: ~40 px between events, the speed of a quick flick.
        const double t = static_cast<double>(i) / events;
        const double a = t * 6 * std::numbers::pi;
        const PointF p{cx + std::cos(a) * radius * (0.3 + 0.7 * t), cy + std::sin(a) * radius * (0.3 + 0.7 * t)};
        const auto start = Clock::now();
        if (i == 0) {
            brush.begin(*img, p, 1.0);
        } else {
            brush.moveTo(*img, p, 1.0);
        }
        doc.syncEdit();
        for (const Point& tile : doc.takeDirtyTiles()) {
            doc.compositeTile(tile.x, tile.y, scratch);
        }
        times.push_back(ms(Clock::now() - start));
    }
    brush.end();
    const auto start = Clock::now();
    doc.endEdit("Brush");
    std::printf("    endEdit (undo step): %.2f ms\n", ms(Clock::now() - start));
    return times.empty() ? Stats{} : stats(times);
}

void printStats(const char* label, const Stats& s) {
    std::printf("  %-44s mean %6.2f ms   p95 %6.2f ms   max %6.2f ms\n", label, s.mean, s.p95, s.max);
}

double fullComposite(const Document& doc) {
    TiledImage::Tile scratch;
    const auto start = Clock::now();
    for (int ty = 0; ty < doc.tilesY(); ++ty) {
        for (int tx = 0; tx < doc.tilesX(); ++tx) {
            doc.compositeTile(tx, ty, scratch);
        }
    }
    return ms(Clock::now() - start);
}

void benchStrokes() {
    std::printf("\n[1] Stroke latency, 2048 x 2048 canvas (per pointer event: dab + sync + composite)\n");
    for (double diameter : {12.0, 48.0, 160.0}) {
        Document doc(2048, 2048);
        BrushTool brush;
        brush.settings().diameter = diameter;
        brush.settings().hardness = 0.6;
        brush.setColor({30, 80, 200, 255});
        char label[64];
        std::snprintf(label, sizeof label, "brush %.0f px, 400 events", diameter);
        const Stats s = strokeLatency(doc, brush, 900, 400);
        printStats(label, s);
    }
    {
        Document doc(2048, 2048);
        for (int i = 0; i < 9; ++i) {
            doc.addLayer();
        }
        doc.setActiveLayer(5);
        BrushTool brush;
        brush.settings().diameter = 48;
        brush.setColor({200, 40, 40, 255});
        printStats("brush 48 px, 10 layers", strokeLatency(doc, brush, 900, 400));
    }
    {
        Document doc(2048, 2048);
        BrushTool brush(BrushMode::Smudge);
        brush.settings().diameter = 48;
        printStats("smudge 48 px", strokeLatency(doc, brush, 900, 200));
    }
}

void benchComposite() {
    std::printf("\n[2] Full composite, 2048 x 2048, 10 painted layers\n");
    Document doc(2048, 2048);
    BrushTool brush;
    brush.settings().diameter = 300;
    for (int l = 0; l < 10; ++l) {
        if (l > 0) {
            doc.addLayer();
        }
        brush.setColor({static_cast<std::uint8_t>(25 * l), 120, static_cast<std::uint8_t>(250 - 20 * l), 255});
        TiledImage* img = doc.beginEdit();
        brush.begin(*img, {0, 100.0 + 190 * l});
        brush.moveTo(*img, {2047, 300.0 + 150 * l});
        brush.end();
        doc.endEdit("Brush");
        LayerProps p = doc.layer(l).props;
        p.opacity = 0.8;
        p.blendMode = l % 3 == 0 ? BlendMode::Multiply : BlendMode::Normal;
        doc.setLayerProps(l, p);
    }
    std::printf("  all tiles:                       %8.1f ms\n", fullComposite(doc));
    LayerProps p = doc.layer(3).props;
    p.effects = {Effect::make(EffectType::DropShadow), Effect::make(EffectType::Outline)};
    doc.setLayerProps(3, p);
    std::printf("  + 2 effects, first render:       %8.1f ms\n", fullComposite(doc));
    std::printf("  + 2 effects, cached:             %8.1f ms\n", fullComposite(doc));
    auto start = Clock::now();
    while (doc.canUndo()) {
        doc.undo();
    }
    const double undoAll = ms(Clock::now() - start);
    start = Clock::now();
    while (doc.canRedo()) {
        doc.redo();
    }
    std::printf("  undo all / redo all (%d steps):  %6.1f / %6.1f ms\n", static_cast<int>(doc.undoSteps()), undoAll,
                ms(Clock::now() - start));
}

void benchFiles(const std::filesystem::path& outDir) {
    std::printf("\n[3] Sample projects: build, save, load\n");
    std::printf("  %-22s %9s %9s %9s %10s\n", "project", "build ms", "save ms", "load ms", "file KiB");
    const auto start = Clock::now();
    std::vector<SampleProject> samples = buildSampleProjects();
    const double buildMs = ms(Clock::now() - start);
    const std::filesystem::path dir =
        outDir.empty() ? std::filesystem::temp_directory_path() / "gas_bench_samples" : outDir;
    std::filesystem::create_directories(dir);
    for (const SampleProject& s : samples) {
        const std::filesystem::path file = dir / (s.name + ".gas");
        auto t = Clock::now();
        const IoResult saved = saveProject(*s.document, file);
        const double saveMs = ms(Clock::now() - t);
        t = Clock::now();
        const LoadResult loaded = loadProject(file);
        const double loadMs = ms(Clock::now() - t);
        std::printf("  %-22s %9s %9.1f %9.1f %10.1f%s\n", s.name.c_str(), "", saveMs, loadMs,
                    saved.ok ? static_cast<double>(std::filesystem::file_size(file)) / 1024.0 : 0.0,
                    loaded.document ? "" : "  LOAD FAILED");
    }
    std::printf("  (building all samples: %.0f ms)\n", buildMs);
    if (outDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    } else {
        std::printf("  written to %s\n", dir.string().c_str());
    }
}

void benchBigCanvas() {
    std::printf("\n[4] Memory: 16384 x 16384 canvas, 3 layers, strokes across it\n");
    const double before = currentMiB();
    Document doc(16384, 16384);
    doc.addLayer();
    doc.addLayer();
    BrushTool brush;
    brush.settings().diameter = 64;
    TiledImage* img = doc.beginEdit();
    brush.begin(*img, {0, 0});
    brush.moveTo(*img, {16383, 16383});
    brush.end();
    doc.endEdit("Brush");
    std::printf("  working set +%.0f MiB (sparse tiles: only touched 256 px tiles are stored)\n",
                currentMiB() - before);
    const auto start = Clock::now();
    doc.selectAll();
    doc.deselect();
    std::printf("  select all + deselect: %.1f ms\n", ms(Clock::now() - start));
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path samplesDir;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--write-samples" && i + 1 < argc) {
            samplesDir = argv[++i];
        }
    }
#ifdef NDEBUG
    std::printf("GameArt Studio benchmark (release build)\n");
#else
    std::printf("GameArt Studio benchmark (DEBUG build: numbers are much slower than release)\n");
#endif
    benchStrokes();
    benchComposite();
    benchFiles(samplesDir);
    benchBigCanvas();
    std::printf("\nPeak working set: %.0f MiB\n", peakMiB());
    return 0;
}
