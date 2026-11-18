#pragma once

#include "gas/core/Document.h"
#include "gas/core/Geometry.h"
#include "gas/core/TiledImage.h"

#include <string>
#include <vector>

namespace gas {

// Pixels with alpha above `alphaThreshold` count as content.
struct GridSliceOptions {
    int cellWidth = 32;
    int cellHeight = 32;
    int offsetX = 0; // top-left of the first cell
    int offsetY = 0;
    int spacingX = 0; // gap between cells
    int spacingY = 0;
    bool skipEmpty = true; // drop cells without content
    int alphaThreshold = 0;
};

struct AutoSliceOptions {
    int alphaThreshold = 0;
    // Parts whose boxes are at most this far apart (px) become one sprite, so
    // detached bits (a sword, a speck of dust) stay with their character.
    int mergeDistance = 2;
    int minPixels = 4; // smaller islands are ignored as noise
};

// Cells of a regular grid that fit inside the image, row by row.
std::vector<IRect> gridSlices(const TiledImage& image, const GridSliceOptions& options);
// Bounding boxes of 8-connected content regions (merged per mergeDistance), in reading order.
std::vector<IRect> autoSlices(const TiledImage& image, const AutoSliceOptions& options);
// Tight bounds of the content inside `rect`; empty if it has none.
IRect trimmedRect(const TiledImage& image, IRect rect, int alphaThreshold = 0);
// Sorts rects into rows (top to bottom), then left to right within a row.
void sortReadingOrder(std::vector<IRect>& rects);
// Slices named "<base>_<index>" (zero-padded to at least 2 digits), starting at `firstIndex`.
std::vector<Slice> namedSlices(const std::vector<IRect>& rects, const std::string& base, PointF pivot,
                               int firstIndex = 0);
// `name` made unique against `taken` by appending _2, _3, ...
std::string uniqueName(const std::string& name, const std::vector<std::string>& taken);
// Row-major copy of `rect` (pixels outside the image are transparent).
std::vector<Rgba8> copyPixels(const TiledImage& image, IRect rect);
// 9-slice resize of a w x h sprite to outW x outH (nearest sampling, like a
// game engine's sliced sprite): corners keep their size (shrunk together when
// the target is smaller than the borders), edges stretch along their side and
// the centre both ways; with `tile` edges and centre repeat instead.
std::vector<Rgba8> nineSliceScale(const std::vector<Rgba8>& src, int w, int h, SliceBorder border, int outW,
                                  int outH, bool tile = false);

} // namespace gas
