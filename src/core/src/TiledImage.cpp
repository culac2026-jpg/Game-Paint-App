#include "gas/core/TiledImage.h"

#include <stdexcept>
#include <utility>

namespace gas {

namespace {
int ceilDiv(int a, int b) { return (a + b - 1) / b; }
} // namespace

TiledImage::TiledImage(int width, int height)
    : width_(width), height_(height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("TiledImage dimensions must be positive");
    }
    tilesX_ = ceilDiv(width, kTileSize);
    tilesY_ = ceilDiv(height, kTileSize);
    const auto count = static_cast<std::size_t>(tilesX_) * static_cast<std::size_t>(tilesY_);
    tiles_.resize(count);
    dirtyFlags_.resize(count, false);
}

Rgba8 TiledImage::pixel(Point p) const {
    if (!contains(p)) {
        return Rgba8::transparent();
    }
    const Tile* t = tiles_[static_cast<std::size_t>(tileIndex(p.x / kTileSize, p.y / kTileSize))].get();
    if (!t) {
        return Rgba8::transparent();
    }
    return t->pixels[static_cast<std::size_t>(pixelIndex(p.x, p.y))];
}

void TiledImage::setPixel(Point p, Rgba8 color) {
    if (!contains(p)) {
        return;
    }
    editTile(p.x / kTileSize, p.y / kTileSize).pixels[static_cast<std::size_t>(pixelIndex(p.x, p.y))] =
        color;
}

void TiledImage::markDirty(int tx, int ty) {
    const auto index = static_cast<std::size_t>(tileIndex(tx, ty));
    if (!dirtyFlags_[index]) {
        dirtyFlags_[index] = true;
        dirtyList_.push_back({tx, ty});
    }
}

TiledImage::Tile& TiledImage::editTile(int tx, int ty) {
    auto& t = tiles_[static_cast<std::size_t>(tileIndex(tx, ty))];
    if (!t) {
        t = std::make_shared<Tile>();
    } else if (t.use_count() > 1) {
        t = std::make_shared<Tile>(*t); // copy-on-write
    }
    markDirty(tx, ty);
    return *t;
}

void TiledImage::setTile(int tx, int ty, TilePtr tile) {
    // Safe: a tile reachable through a TilePtr is shared, so editTile() clones
    // it before any write.
    tiles_[static_cast<std::size_t>(tileIndex(tx, ty))] = std::const_pointer_cast<Tile>(std::move(tile));
    markDirty(tx, ty);
}

const TiledImage::Tile* TiledImage::tile(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= tilesX_ || ty >= tilesY_) {
        return nullptr;
    }
    return tiles_[static_cast<std::size_t>(tileIndex(tx, ty))].get();
}

TiledImage::TilePtr TiledImage::sharedTile(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= tilesX_ || ty >= tilesY_) {
        return nullptr;
    }
    return tiles_[static_cast<std::size_t>(tileIndex(tx, ty))];
}

std::vector<TiledImage::TilePtr> TiledImage::snapshot() const {
    return {tiles_.begin(), tiles_.end()};
}

std::vector<Point> TiledImage::takeDirtyTiles() {
    for (const Point& t : dirtyList_) {
        dirtyFlags_[static_cast<std::size_t>(tileIndex(t.x, t.y))] = false;
    }
    return std::exchange(dirtyList_, {});
}

} // namespace gas
