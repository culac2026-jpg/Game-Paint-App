#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"

#include <array>
#include <memory>
#include <vector>

namespace gas {

// RGBA raster stored as sparse 256x256 tiles. Unallocated tiles read as
// fully transparent. Writing a pixel allocates its tile and marks it dirty;
// the renderer drains dirty tiles to refresh only what changed.
class TiledImage {
public:
    static constexpr int kTileSize = 256;

    struct Tile {
        std::array<Rgba8, kTileSize * kTileSize> pixels{};
    };

    TiledImage(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    int tilesX() const { return tilesX_; }
    int tilesY() const { return tilesY_; }

    bool contains(Point p) const {
        return p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_;
    }

    Rgba8 pixel(Point p) const;
    // Out-of-bounds writes are ignored.
    void setPixel(Point p, Rgba8 color);

    // Null when the tile has never been written.
    const Tile* tile(int tx, int ty) const;

    // Returns tiles written since the last call (tile coordinates), and clears the set.
    std::vector<Point> takeDirtyTiles();

private:
    int tileIndex(int tx, int ty) const { return ty * tilesX_ + tx; }

    int width_;
    int height_;
    int tilesX_;
    int tilesY_;
    std::vector<std::unique_ptr<Tile>> tiles_;
    std::vector<bool> dirtyFlags_;
    std::vector<Point> dirtyList_;
};

} // namespace gas
