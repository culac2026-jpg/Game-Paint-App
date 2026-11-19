#pragma once

#include "gas/core/Color.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gas {

class Document;

// Native project file (.gas), little-endian:
//
//   "GASPROJ\x1A"  u32 formatVersion
//   chunks: u32 fourcc, u32 payloadSize, payload   (unknown chunks are skipped)
//     "DOCM" u32 width, u32 height, u32 activeLayer, u32 layerCount
//     "LAYR" one per layer, bottom to top:
//            u16 nameLength, UTF-8 name, u8 flags (1 = visible, 2 = locked),
//            f32 opacity, TILES
//     "LMSK" (version 2+) optional, after its layer: u32 layerIndex,
//            u8 flags (1 = mask enabled), TILES
//     "LPRP" (version 3+) optional, after its layer: u32 layerIndex,
//            u8 blendMode (BlendMode value; unknown = Normal); readers
//            ignore any further bytes, so fields can be appended
//     "LEFX" (version 4+) optional, after its layer: u32 layerIndex,
//            u16 effectCount, then per effect: u8 type, u8 flags
//            (1 = enabled, 2 = smooth), u8 r, g, b, a (colour), u16 floatCount,
//            f32 x floatCount: opacity, size, offsetX, offsetY, hue,
//            saturation, lightness, contrast (extra floats are skipped;
//            effects of unknown type are dropped)
//     "SLCS" (version 5+) optional: u32 sliceCount, then per slice:
//            u16 nameLength, UTF-8 name, i32 x, y, w, h, f32 pivotX, pivotY
//            (readers ignore bytes after the slice list)
//     "FRMS" (version 6+) optional (absent = one frame): u32 frameCount,
//            u32 activeFrame, u32 duration (ms) x frameCount. LAYR tiles are
//            then frame 0's cels.
//     "CELS" (version 6+) one per non-empty cel of frames 1..n-1:
//            u32 layerIndex, u32 frame, TILES
//     "TAGS" (version 6+) optional: u32 tagCount, then per tag: u16 nameLength,
//            UTF-8 name, u32 from, u32 to, u8 direction (TagDirection)
//     "VECT" (version 7+) optional, after its layer (whose LAYR/CELS tiles
//            are then left out; the shapes are rendered on load): u32 layerIndex,
//            u32 shapeCount, then per shape: u8 flags (1 = fill, 2 = stroke,
//            4 = antialias, 8 = closed), u8 join, u8 cap, u8 r, g, b, a (fill),
//            u8 r, g, b, a (stroke), f64 strokeWidth, u32 nodeCount, then per
//            node f64 x, y, inX, inY, outX, outY
//     "SLBD" (version 7+) optional, after SLCS: u32 count, then per slice (in
//            SLCS order) i32 left, top, right, bottom (9-slice border)
//     "LFRM" (version 8+) optional, after its layer: u32 layerIndex, f64 rectX,
//            rectY, rectW, rectH, pivotX, pivotY, offsetX, offsetY, angle
//            (radians), scaleX, scaleY (the layer's remembered transform box
//            and anchor, see LayerFrame)
//     "END "
//   u32 CRC-32 of every preceding byte
//
//   TILES = u32 tileCount, then per tile:
//           u32 tx, u32 ty, u32 compressedSize, zlib(256*256 RGBA8 straight alpha)
//
// Only non-empty tiles are stored. Saves are atomic: written to a temporary
// sibling file, then renamed over the target, so a failed save never damages
// an existing file. Undo history and the selection are not saved.
// Version history: 1 = v0.3 (layers), 2 = v0.4 (layer masks), 3 = v0.5 (blend modes), 4 = v0.6 (layer effects), 5 = v0.7 (slices), 6 = v0.8 (animation frames and tags), 7 = v0.9 (vector layers, 9-slice borders), 8 = v1.0.2 (layer transform frames). Older versions
// still load; newer ones are refused.
inline constexpr std::uint32_t kProjectFormatVersion = 8;
inline constexpr const char* kProjectExtension = ".gas";

struct IoResult {
    bool ok = false;
    std::string error;
};

struct LoadResult {
    std::unique_ptr<Document> document; // null on failure
    std::string error;
};

struct SaveOptions {
    // Keep the file being replaced as "<path>.bak" (one generation).
    bool backup = false;
};

IoResult saveProject(const Document& document, const std::filesystem::path& path, SaveOptions options = {});
LoadResult loadProject(const std::filesystem::path& path);
// The in-memory form of a .gas file (what saveProject writes / loadProject
// reads). parseProject never trusts its input: damaged or hostile bytes give
// an error, never a crash or an unbounded allocation. serializeProject
// returns an empty vector if the document can't be saved.
std::vector<std::uint8_t> serializeProject(const Document& document);
LoadResult parseProject(const std::uint8_t* data, std::size_t size);
// "<path>.bak": the previous version kept by SaveOptions::backup.
std::filesystem::path backupPathFor(const std::filesystem::path& path);

// Flattened visible layers as an 8-bit RGBA PNG (straight alpha, exact pixels).
IoResult exportPng(const Document& document, const std::filesystem::path& path);
std::vector<std::uint8_t> encodePng(int width, int height, const std::vector<Rgba8>& pixels);
// SVG of the visible layers (bottom to top, with opacity and blend mode):
// vector layers as editable <path>s, everything else (raster layers, and
// vector layers with a mask or effects) as embedded PNG images of their
// rendered content.
std::string svgDocument(const Document& document);
IoResult exportSvg(const Document& document, const std::filesystem::path& path);

// Writes via a temporary sibling + rename. With `backup`, an existing file is
// first renamed to `backup` (and put back if the final rename fails).
IoResult writeFileAtomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                         const std::filesystem::path& backup = {});

} // namespace gas
