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
    return t->pixels[static_cast<std::size_t>((p.y % kTileSize) * kTileSize + p.x % kTileSize)];
}

void TiledImage::setPixel(Point p, Rgba8 color) {
    if (!contains(p)) {
        return;
    }
    const int tx = p.x / kTileSize;
    const int ty = p.y / kTileSize;
    const auto index = static_cast<std::size_t>(tileIndex(tx, ty));
    auto& t = tiles_[index];
    if (!t) {
        t = std::make_unique<Tile>();
    }
    t->pixels[static_cast<std::size_t>((p.y % kTileSize) * kTileSize + p.x % kTileSize)] = color;
    if (!dirtyFlags_[index]) {
        dirtyFlags_[index] = true;
        dirtyList_.push_back({tx, ty});
    }
}

const TiledImage::Tile* TiledImage::tile(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= tilesX_ || ty >= tilesY_) {
        return nullptr;
    }
    return tiles_[static_cast<std::size_t>(tileIndex(tx, ty))].get();
}

std::vector<Point> TiledImage::takeDirtyTiles() {
    for (const Point& t : dirtyList_) {
        dirtyFlags_[static_cast<std::size_t>(tileIndex(t.x, t.y))] = false;
    }
    return std::exchange(dirtyList_, {});
}

} // namespace gas
