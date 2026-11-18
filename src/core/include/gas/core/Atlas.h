#pragma once

#include "gas/core/Color.h"
#include "gas/core/Document.h"
#include "gas/core/Geometry.h"
#include "gas/core/ProjectIO.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gas {

// Metadata written next to each atlas page. Append only (stored in settings).
enum class AtlasFormat {
    JsonHash,   // TexturePacker-style JSON hash (Phaser, PixiJS, many engine importers)
    CocosPlist, // Cocos Creator / cocos2d-x sprite atlas (.plist, format 2)
    UnityMeta,  // Unity TextureImporter .png.meta with a Multiple-mode sprite sheet
};
inline constexpr int kAtlasFormatCount = 3;
std::string_view atlasFormatName(AtlasFormat format);

enum class AtlasPreset { Unity, UnityPixelArt, CocosCreator, Generic };
inline constexpr int kAtlasPresetCount = 4;
std::string_view atlasPresetName(AtlasPreset preset);

struct AtlasSettings {
    int maxWidth = 2048; // a page never exceeds this; more sprites go to more pages
    int maxHeight = 2048;
    int padding = 2;  // transparent px between sprites
    int border = 0;   // transparent px along the page edges
    int extrude = 0;  // edge pixels repeated outward, so filtering never samples a neighbour
    bool trim = true; // pack only the opaque part; the metadata keeps the original size
    bool powerOfTwo = true;
    // Grid layout instead of MaxRects: equal cells (the largest sprite), input
    // order kept row by row, no trimming, e.g. classic animation strips.
    bool grid = false;
    int columns = 0; // grid columns per page (0 = about square)
    AtlasFormat format = AtlasFormat::JsonHash;
    // Unity only:
    bool pointFilter = false; // Point filtering and no compression (pixel art)
    int pixelsPerUnit = 100;

    friend bool operator==(const AtlasSettings&, const AtlasSettings&) = default;
};
AtlasSettings atlasPreset(AtlasPreset preset);

struct AtlasSprite {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<Rgba8> pixels; // row-major, straight alpha
    PointF pivot{0.5, 0.5};    // relative to the sprite, top-left origin
    int duration = 0;          // animation frame time in ms (0 = not a frame)
};

struct AtlasFrame {
    std::string name; // unique within the atlas
    int page = 0;
    IRect frame;      // packed pixels in the page (without extrusion)
    IRect source;     // which part of the original sprite they are (trim offset + size)
    int sourceWidth = 0;
    int sourceHeight = 0;
    PointF pivot;     // relative to the original sprite, top-left origin
    bool trimmed = false;
    int duration = 0;
};

struct AtlasPage {
    int width = 0;
    int height = 0;
    std::vector<Rgba8> pixels;
};

struct Atlas {
    std::vector<AtlasPage> pages;
    std::vector<AtlasFrame> frames; // same order as the input sprites
    std::vector<FrameTag> tags;     // animation ranges over `frames` (set by the caller; JSON only)
    std::string error;              // non-empty = failed (e.g. a sprite bigger than a page)
    bool ok() const { return error.empty(); }
};

// MaxRects (best short side fit) packing; pages shrink to the smallest size
// that holds their sprites (a power of two per side when asked).
Atlas packAtlas(const std::vector<AtlasSprite>& sprites, const AtlasSettings& settings);

// One sprite per document slice, cut from the composite of the visible layers.
std::vector<AtlasSprite> spritesFromSlices(const Document& document);
// Animation export: every frame of every slice ("<slice>_<frame>", slice by
// slice), or every whole frame ("<baseName>_<frame>", with `pivot`) when the
// document has no slices; durations come from the frames.
std::vector<AtlasSprite> spritesFromFrames(const Document& document, const std::string& baseName, PointF pivot);
// The document's tags mapped onto spritesFromFrames() order (per slice: "<slice>_<tag>").
std::vector<FrameTag> tagsForFrameSprites(const Document& document);

// Page metadata in each format; `imageFile` is the page's PNG file name.
std::string atlasJson(const Atlas& atlas, int page, const std::string& imageFile);
std::string cocosPlist(const Atlas& atlas, int page, const std::string& imageFile);
std::string unityMeta(const Atlas& atlas, int page, const AtlasSettings& settings, const std::string& imageFile);

// File names for a page: "<stem>.png" for a single page, "<stem>_<n>.png" for several.
std::filesystem::path atlasPagePath(const std::filesystem::path& pngPath, int page, int pageCount);
// Writes every page PNG plus its metadata (name.json / name.plist / name.png.meta).
IoResult exportAtlas(const Atlas& atlas, const AtlasSettings& settings, const std::filesystem::path& pngPath,
                     std::vector<std::filesystem::path>* written = nullptr);

// Batch export: one PNG per slice (optionally trimmed; scaled up by an integer
// factor with nearest neighbour) into `folder`, named after the slice.
IoResult exportSlicePngs(const Document& document, const std::filesystem::path& folder, bool trim, int scale,
                         std::vector<std::filesystem::path>* written = nullptr);
// Batch export: one canvas-sized PNG per layer (pixels + mask + effects, opacity applied).
IoResult exportLayerPngs(const Document& document, const std::filesystem::path& folder,
                         std::vector<std::filesystem::path>* written = nullptr);
// Batch export: one PNG per animation frame ("<baseName>_<frame>.png").
IoResult exportFramePngs(const Document& document, const std::filesystem::path& folder, const std::string& baseName,
                         int scale, std::vector<std::filesystem::path>* written = nullptr);
// A file-system-safe version of `name` (reserved characters become '_').
std::string safeFileName(const std::string& name);

} // namespace gas
