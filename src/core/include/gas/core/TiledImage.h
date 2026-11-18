#pragma once

#include "gas/core/Color.h"
#include "gas/core/Geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gas {

// RGBA raster stored as sparse 256x256 tiles. Unallocated tiles read as
// fully transparent. Writing a pixel allocates its tile and marks it dirty;
// the renderer drains dirty tiles to refresh only what changed.
//
// Tiles are shared copy-on-write: copying a TiledImage, or holding a tile from
// sharedTile()/snapshot(), is cheap pointer sharing, and the next write to a
// shared tile clones it first. Undo snapshots rely on this.
class TiledImage {
public:
    static constexpr int kTileSize = 256;

    struct Tile {
        std::array<Rgba8, kTileSize * kTileSize> pixels{};
    };
    using TilePtr = std::shared_ptr<const Tile>;

    TiledImage(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    int tilesX() const { return tilesX_; }
    int tilesY() const { return tilesY_; }
    int tileCount() const { return tilesX_ * tilesY_; }

    bool contains(Point p) const {
        return p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_;
    }

    Rgba8 pixel(Point p) const;
    // Out-of-bounds writes are ignored.
    void setPixel(Point p, Rgba8 color);

    // Null when the tile has never been written.
    const Tile* tile(int tx, int ty) const;
    TilePtr sharedTile(int tx, int ty) const;

    // Bulk-write access: allocates (or un-shares) the tile and marks it dirty.
    // tx/ty must be valid tile coordinates.
    Tile& editTile(int tx, int ty);

    // Replaces a whole tile (null = transparent) and marks it dirty.
    void setTile(int tx, int ty, TilePtr tile);

    // All tile pointers, row-major (index = ty * tilesX + tx). Holding the
    // result keeps those tiles immutable: later edits clone them.
    std::vector<TilePtr> snapshot() const;

    // Index of pixel (x, y) inside its tile's pixel array.
    static constexpr int pixelIndex(int x, int y) {
        return (y % kTileSize) * kTileSize + x % kTileSize;
    }

    // Returns tiles written since the last call (tile coordinates), and clears the set.
    std::vector<Point> takeDirtyTiles();

    // Changes on every write; unique across all images (copies share it until
    // one of them is written), so caches can key on it.
    std::uint64_t revision() const { return revision_; }

private:
    int tileIndex(int tx, int ty) const { return ty * tilesX_ + tx; }
    void markDirty(int tx, int ty);

    int width_;
    int height_;
    int tilesX_;
    int tilesY_;
    std::vector<std::shared_ptr<Tile>> tiles_;
    std::vector<bool> dirtyFlags_;
    std::vector<Point> dirtyList_;
    std::uint64_t revision_;
};

} // namespace gas
