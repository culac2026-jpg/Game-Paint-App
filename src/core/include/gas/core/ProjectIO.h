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
//     "END "
//   u32 CRC-32 of every preceding byte
//
//   TILES = u32 tileCount, then per tile:
//           u32 tx, u32 ty, u32 compressedSize, zlib(256*256 RGBA8 straight alpha)
//
// Only non-empty tiles are stored. Saves are atomic: written to a temporary
// sibling file, then renamed over the target, so a failed save never damages
// an existing file. Undo history and the selection are not saved.
// Version history: 1 = v0.3 (layers), 2 = v0.4 (layer masks), 3 = v0.5 (blend modes). Older versions
// still load; newer ones are refused.
inline constexpr std::uint32_t kProjectFormatVersion = 3;
inline constexpr const char* kProjectExtension = ".gas";

struct IoResult {
    bool ok = false;
    std::string error;
};

struct LoadResult {
    std::unique_ptr<Document> document; // null on failure
    std::string error;
};

IoResult saveProject(const Document& document, const std::filesystem::path& path);
LoadResult loadProject(const std::filesystem::path& path);

// Flattened visible layers as an 8-bit RGBA PNG (straight alpha, exact pixels).
IoResult exportPng(const Document& document, const std::filesystem::path& path);
std::vector<std::uint8_t> encodePng(int width, int height, const std::vector<Rgba8>& pixels);

// Writes via a temporary sibling + rename. Exposed for tests.
IoResult writeFileAtomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

} // namespace gas
