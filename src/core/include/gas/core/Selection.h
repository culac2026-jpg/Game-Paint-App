#pragma once

#include "gas/core/Geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gas {

// Per-pixel selection coverage (0 = unselected, 255 = fully selected), stored
// as immutable shared 256x256 tiles, so copies are cheap (undo stores them).
// All-zero tiles are null and fully-selected interior tiles share one
// constant tile, so "Select All" on a huge canvas costs almost nothing.
//
// An empty selection means "nothing selected"; tools then affect the whole layer.
class Selection {
public:
    static constexpr int kTileSize = 256;
    using MaskTile = std::array<std::uint8_t, kTileSize * kTileSize>;
    enum class Op { Replace, Add, Subtract, Intersect };

    Selection() = default; // 0x0, empty
    Selection(int width, int height);

    static Selection all(int width, int height);
    static Selection rect(int width, int height, IRect r);
    static Selection ellipse(int width, int height, RectF bounds, bool antialias);
    // Freehand lasso: polygon through `points` (closed automatically), even-odd fill.
    static Selection polygon(int width, int height, const std::vector<PointF>& points, bool antialias);

    int width() const { return width_; }
    int height() const { return height_; }
    bool isEmpty() const;
    std::uint8_t coverage(Point p) const;
    // Bounds of every pixel with coverage > 0.
    std::optional<IRect> bounds() const;

    // Tile-level access for bulk operations (tile grid matches TiledImage's).
    // Null = nothing selected in that tile.
    const MaskTile* tile(int tx, int ty) const { return tileAt(tx, ty).get(); }
    bool tileFullySelected(int tx, int ty) const { return isFull(tileAt(tx, ty)); }

    Selection combined(const Selection& other, Op op) const;
    Selection inverted() const;
    // Coverage resampled through `m` (document space to document space).
    Selection transformed(const Affine& m, Interpolation interpolation) const;
    // The part inside `area`, as a selection of size area.w x area.h.
    Selection cropped(IRect area) const;

    // Coverage blurred by about `radius` px (3-pass box blur), for soft edges.
    Selection feathered(double radius) const;

    // Boundary of the region with coverage >= 128, as merged axis-aligned unit
    // edges on the pixel grid (for drawing marching ants).
    struct Segment {
        int x0, y0, x1, y1;
        friend bool operator==(const Segment&, const Segment&) = default;
    };
    std::vector<Segment> outline() const;

    friend bool operator==(const Selection& a, const Selection& b);

private:
    using TilePtr = std::shared_ptr<const MaskTile>;

    int tileIndex(int tx, int ty) const { return ty * tilesX_ + tx; }
    const TilePtr& tileAt(int tx, int ty) const { return tiles_[static_cast<std::size_t>(tileIndex(tx, ty))]; }
    bool isFull(const TilePtr& t) const { return t == fullTile(); }
    // Whether tile (tx, ty) lies entirely inside the canvas.
    bool interiorTile(int tx, int ty) const;
    // Stores `tile`, normalising all-zero to null and all-255 interior tiles to the shared full tile.
    void store(int tx, int ty, std::shared_ptr<MaskTile> tile);
    // Every tile of the canvas set to fully selected (edge tiles masked to the canvas).
    void fillAll();
    static const TilePtr& fullTile();

    int width_ = 0;
    int height_ = 0;
    int tilesX_ = 0;
    int tilesY_ = 0;
    std::vector<TilePtr> tiles_;
};

} // namespace gas
