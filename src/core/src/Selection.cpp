#include "gas/core/Selection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace gas {

namespace {

constexpr int kTile = Selection::kTileSize;
constexpr int kSubRows = 4; // vertical supersamples per pixel row when rasterising polygons

int ceilDiv(int a, int b) { return (a + b - 1) / b; }

bool allEqual(const Selection::MaskTile& t, std::uint8_t v) {
    return std::all_of(t.begin(), t.end(), [v](std::uint8_t x) { return x == v; });
}

} // namespace

const Selection::TilePtr& Selection::fullTile() {
    static const TilePtr full = [] {
        auto t = std::make_shared<MaskTile>();
        t->fill(255);
        return TilePtr(std::move(t));
    }();
    return full;
}

Selection::Selection(int width, int height) : width_(width), height_(height) {
    if (width < 0 || height < 0) {
        throw std::invalid_argument("Selection size must not be negative");
    }
    tilesX_ = ceilDiv(width, kTile);
    tilesY_ = ceilDiv(height, kTile);
    tiles_.resize(static_cast<std::size_t>(tilesX_) * static_cast<std::size_t>(tilesY_));
}

bool Selection::interiorTile(int tx, int ty) const {
    return (tx + 1) * kTile <= width_ && (ty + 1) * kTile <= height_;
}

void Selection::store(int tx, int ty, std::shared_ptr<MaskTile> tile) {
    TilePtr& slot = tiles_[static_cast<std::size_t>(tileIndex(tx, ty))];
    if (!tile || allEqual(*tile, 0)) {
        slot = nullptr;
    } else if (interiorTile(tx, ty) && allEqual(*tile, 255)) {
        slot = fullTile();
    } else {
        slot = std::move(tile);
    }
}

void Selection::fillAll() {
    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            if (interiorTile(tx, ty)) {
                tiles_[static_cast<std::size_t>(tileIndex(tx, ty))] = fullTile();
                continue;
            }
            // Edge tile: only the part inside the canvas is selected.
            auto t = std::make_shared<MaskTile>();
            t->fill(0);
            const int w = std::min(kTile, width_ - tx * kTile);
            const int h = std::min(kTile, height_ - ty * kTile);
            for (int y = 0; y < h; ++y) {
                std::memset(t->data() + y * kTile, 255, static_cast<std::size_t>(w));
            }
            store(tx, ty, std::move(t));
        }
    }
}

Selection Selection::all(int width, int height) {
    Selection s(width, height);
    s.fillAll();
    return s;
}

Selection Selection::rect(int width, int height, IRect r) {
    Selection s(width, height);
    r = r.intersected({0, 0, width, height});
    if (r.empty()) {
        return s;
    }
    for (int ty = r.y / kTile; ty <= (r.bottom() - 1) / kTile; ++ty) {
        for (int tx = r.x / kTile; tx <= (r.right() - 1) / kTile; ++tx) {
            const IRect tileRect{tx * kTile, ty * kTile, kTile, kTile};
            const IRect part = r.intersected(tileRect);
            if (part == tileRect && s.interiorTile(tx, ty)) {
                s.tiles_[static_cast<std::size_t>(s.tileIndex(tx, ty))] = fullTile();
                continue;
            }
            auto t = std::make_shared<MaskTile>();
            t->fill(0);
            for (int y = part.y; y < part.bottom(); ++y) {
                std::memset(t->data() + (y - tileRect.y) * kTile + (part.x - tileRect.x), 255,
                            static_cast<std::size_t>(part.w));
            }
            s.store(tx, ty, std::move(t));
        }
    }
    return s;
}

Selection Selection::ellipse(int width, int height, RectF b, bool antialias) {
    if (b.w <= 0 || b.h <= 0) {
        return Selection(width, height);
    }
    const double rx = b.w / 2.0, ry = b.h / 2.0;
    const double cx = b.x + rx, cy = b.y + ry;
    // Enough vertices that the polygon is within a fraction of a pixel of the curve.
    const int n = std::clamp(static_cast<int>(std::ceil(2.0 * std::numbers::pi * std::max(rx, ry))), 16, 4096);
    std::vector<PointF> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * std::numbers::pi * i / n;
        pts.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return polygon(width, height, pts, antialias);
}

Selection Selection::polygon(int width, int height, const std::vector<PointF>& points, bool antialias) {
    Selection s(width, height);
    if (points.size() < 3 || width == 0 || height == 0) {
        return s;
    }
    double minY = points[0].y, maxY = points[0].y, minX = points[0].x, maxX = points[0].x;
    for (const PointF& p : points) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }
    const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int y1 = std::min(height, static_cast<int>(std::ceil(maxY)));
    const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    const int x1 = std::min(width, static_cast<int>(std::ceil(maxX)));
    if (y0 >= y1 || x0 >= x1) {
        return s;
    }
    const int spanW = x1 - x0;

    // Coverage is accumulated row by row, then written into tiles.
    std::vector<std::shared_ptr<MaskTile>> rowTiles(static_cast<std::size_t>(s.tilesX_));
    std::vector<double> acc(static_cast<std::size_t>(spanW));
    std::vector<double> crossings;
    const std::size_t n = points.size();

    const auto addSpan = [&](double xa, double xb, double weight) {
        xa = std::clamp(xa - x0, 0.0, double(spanW));
        xb = std::clamp(xb - x0, 0.0, double(spanW));
        if (xb <= xa) {
            return;
        }
        const int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
        if (ia == ib) {
            acc[static_cast<std::size_t>(ia)] += (xb - xa) * weight;
            return;
        }
        acc[static_cast<std::size_t>(ia)] += (ia + 1 - xa) * weight;
        for (int i = ia + 1; i < ib; ++i) {
            acc[static_cast<std::size_t>(i)] += weight;
        }
        if (ib < spanW) {
            acc[static_cast<std::size_t>(ib)] += (xb - ib) * weight;
        }
    };

    for (int y = y0; y < y1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int sub = 0; sub < kSubRows; ++sub) {
            const double sy = y + (sub + 0.5) / kSubRows;
            crossings.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const PointF& p = points[i];
                const PointF& q = points[(i + 1) % n];
                if ((p.y <= sy && sy < q.y) || (q.y <= sy && sy < p.y)) {
                    crossings.push_back(p.x + (sy - p.y) * (q.x - p.x) / (q.y - p.y));
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                addSpan(crossings[i], crossings[i + 1], 1.0 / kSubRows);
            }
        }
        // Write the row into its tiles.
        const int ty = y / kTile;
        for (int i = 0; i < spanW; ++i) {
            const double c = std::clamp(acc[static_cast<std::size_t>(i)], 0.0, 1.0);
            const auto v = static_cast<std::uint8_t>(antialias ? std::lround(c * 255.0) : (c >= 0.5 ? 255 : 0));
            if (v == 0) {
                continue;
            }
            const int x = x0 + i;
            auto& t = rowTiles[static_cast<std::size_t>(x / kTile)];
            if (!t) {
                t = std::make_shared<MaskTile>();
                t->fill(0);
            }
            (*t)[static_cast<std::size_t>((y % kTile) * kTile + x % kTile)] = v;
        }
        // Flush tiles at the end of a tile row (or of the shape).
        if (y % kTile == kTile - 1 || y == y1 - 1) {
            for (int tx = 0; tx < s.tilesX_; ++tx) {
                if (rowTiles[static_cast<std::size_t>(tx)]) {
                    s.store(tx, ty, std::move(rowTiles[static_cast<std::size_t>(tx)]));
                    rowTiles[static_cast<std::size_t>(tx)].reset();
                }
            }
        }
    }
    return s;
}

bool Selection::isEmpty() const {
    return std::all_of(tiles_.begin(), tiles_.end(), [](const TilePtr& t) { return !t; });
}

std::uint8_t Selection::coverage(Point p) const {
    if (p.x < 0 || p.y < 0 || p.x >= width_ || p.y >= height_) {
        return 0;
    }
    const TilePtr& t = tileAt(p.x / kTile, p.y / kTile);
    return t ? (*t)[static_cast<std::size_t>((p.y % kTile) * kTile + p.x % kTile)] : 0;
}

std::optional<IRect> Selection::bounds() const {
    IRect result;
    for (int ty = 0; ty < tilesY_; ++ty) {
        for (int tx = 0; tx < tilesX_; ++tx) {
            const TilePtr& t = tileAt(tx, ty);
            if (!t) {
                continue;
            }
            if (isFull(t)) {
                result = result.united({tx * kTile, ty * kTile, kTile, kTile});
                continue;
            }
            int minX = kTile, minY = kTile, maxX = -1, maxY = -1;
            for (int y = 0; y < kTile; ++y) {
                for (int x = 0; x < kTile; ++x) {
                    if ((*t)[static_cast<std::size_t>(y * kTile + x)]) {
                        minX = std::min(minX, x);
                        maxX = std::max(maxX, x);
                        minY = std::min(minY, y);
                        maxY = std::max(maxY, y);
                    }
                }
            }
            if (maxX >= 0) {
                result = result.united({tx * kTile + minX, ty * kTile + minY, maxX - minX + 1, maxY - minY + 1});
            }
        }
    }
    if (result.empty()) {
        return std::nullopt;
    }
    return result.intersected({0, 0, width_, height_});
}

Selection Selection::combined(const Selection& other, Op op) const {
    if (other.width_ != width_ || other.height_ != height_) {
        throw std::invalid_argument("Selection sizes differ");
    }
    if (op == Op::Replace) {
        return other;
    }
    Selection out(width_, height_);
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        const TilePtr& a = tiles_[i];
        const TilePtr& b = other.tiles_[i];
        TilePtr& dst = out.tiles_[i];
        switch (op) {
        case Op::Add:
            if (!a || isFull(b)) {
                dst = b;
                continue;
            }
            if (!b || isFull(a)) {
                dst = a;
                continue;
            }
            break;
        case Op::Subtract:
            if (!a || isFull(b)) {
                dst = nullptr;
                continue;
            }
            if (!b) {
                dst = a;
                continue;
            }
            break;
        case Op::Intersect:
            if (!a || !b) {
                dst = nullptr;
                continue;
            }
            if (isFull(a)) {
                dst = b;
                continue;
            }
            if (isFull(b)) {
                dst = a;
                continue;
            }
            break;
        case Op::Replace:
            break;
        }
        auto t = std::make_shared<MaskTile>();
        for (std::size_t p = 0; p < t->size(); ++p) {
            const int va = (*a)[p], vb = (*b)[p];
            int v = 0;
            switch (op) {
            case Op::Add: v = std::max(va, vb); break;
            case Op::Subtract: v = (va * (255 - vb) + 127) / 255; break;
            case Op::Intersect: v = std::min(va, vb); break;
            case Op::Replace: break;
            }
            (*t)[p] = static_cast<std::uint8_t>(v);
        }
        const int tx = static_cast<int>(i) % tilesX_, ty = static_cast<int>(i) / tilesX_;
        out.store(tx, ty, std::move(t));
    }
    return out;
}

Selection Selection::inverted() const {
    Selection full = all(width_, height_);
    return full.combined(*this, Op::Subtract);
}

Selection Selection::transformed(const Affine& m, Interpolation interpolation) const {
    Selection out(width_, height_);
    const std::optional<IRect> src = bounds();
    if (!src) {
        return out;
    }
    const IRect dst = mappedBounds(m, *src).intersected({0, 0, width_, height_});
    if (dst.empty()) {
        return out;
    }
    const Affine inv = m.inverted();
    std::vector<std::shared_ptr<MaskTile>> rowTiles(static_cast<std::size_t>(tilesX_));
    const auto sample = [this](int x, int y) { return static_cast<double>(coverage({x, y})); };
    for (int y = dst.y; y < dst.bottom(); ++y) {
        for (int x = dst.x; x < dst.right(); ++x) {
            const PointF s = inv.map({x + 0.5, y + 0.5});
            double v;
            if (interpolation == Interpolation::Nearest) {
                v = sample(static_cast<int>(std::floor(s.x)), static_cast<int>(std::floor(s.y)));
            } else {
                const double fx = s.x - 0.5, fy = s.y - 0.5;
                const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
                const double ax = fx - ix, ay = fy - iy;
                v = (sample(ix, iy) * (1 - ax) + sample(ix + 1, iy) * ax) * (1 - ay) +
                    (sample(ix, iy + 1) * (1 - ax) + sample(ix + 1, iy + 1) * ax) * ay;
            }
            const auto b = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
            if (b == 0) {
                continue;
            }
            auto& t = rowTiles[static_cast<std::size_t>(x / kTile)];
            if (!t) {
                t = std::make_shared<MaskTile>();
                t->fill(0);
            }
            (*t)[static_cast<std::size_t>((y % kTile) * kTile + x % kTile)] = b;
        }
        if (y % kTile == kTile - 1 || y == dst.bottom() - 1) {
            for (int tx = 0; tx < tilesX_; ++tx) {
                if (rowTiles[static_cast<std::size_t>(tx)]) {
                    out.store(tx, y / kTile, std::move(rowTiles[static_cast<std::size_t>(tx)]));
                    rowTiles[static_cast<std::size_t>(tx)].reset();
                }
            }
        }
    }
    return out;
}

Selection Selection::cropped(IRect area) const {
    Selection out(area.w, area.h);
    // Per pixel: slow but simple, and crop is a rare, explicit command.
    std::vector<std::shared_ptr<MaskTile>> built(out.tiles_.size());
    for (int y = 0; y < area.h; ++y) {
        for (int x = 0; x < area.w; ++x) {
            const std::uint8_t v = coverage({area.x + x, area.y + y});
            if (v == 0) {
                continue;
            }
            auto& t = built[static_cast<std::size_t>(out.tileIndex(x / kTile, y / kTile))];
            if (!t) {
                t = std::make_shared<MaskTile>();
                t->fill(0);
            }
            (*t)[static_cast<std::size_t>((y % kTile) * kTile + x % kTile)] = v;
        }
    }
    for (int ty = 0; ty < out.tilesY_; ++ty) {
        for (int tx = 0; tx < out.tilesX_; ++tx) {
            out.store(tx, ty, std::move(built[static_cast<std::size_t>(out.tileIndex(tx, ty))]));
        }
    }
    return out;
}

std::vector<Selection::Segment> Selection::outline() const {
    const auto on = [this](int x, int y) { return coverage({x, y}) >= 128; };
    // Unit edges: horizontal ones keyed (y, x) for the edge above pixel row y,
    // vertical ones keyed (x, y) for the edge left of pixel column x.
    std::vector<std::pair<int, int>> horizontal, vertical;
    const auto check = [&](int x, int y) {
        const bool here = on(x, y);
        if (here != on(x - 1, y)) {
            vertical.push_back({x, y});
        }
        if (here != on(x, y - 1)) {
            horizontal.push_back({y, x});
        }
    };
    // Edges are found from the pixel right of / below them. Uniform tiles have
    // no interior edges, so only their first row and column need checking;
    // the extra virtual row/column of tiles catches the canvas' far borders.
    for (int ty = 0; ty <= tilesY_; ++ty) {
        for (int tx = 0; tx <= tilesX_; ++tx) {
            const bool real = tx < tilesX_ && ty < tilesY_;
            const TilePtr* t = real ? &tileAt(tx, ty) : nullptr;
            const int bx = tx * kTile, by = ty * kTile;
            if (t && *t && !isFull(*t)) {
                for (int y = by; y < by + kTile; ++y) {
                    for (int x = bx; x < bx + kTile; ++x) {
                        check(x, y);
                    }
                }
            } else {
                for (int i = 0; i < kTile; ++i) {
                    check(bx + i, by); // top row
                    if (i > 0) {
                        check(bx, by + i); // left column
                    }
                }
            }
        }
    }

    std::vector<Segment> result;
    const auto merge = [&result](std::vector<std::pair<int, int>>& units, bool isHorizontal) {
        std::sort(units.begin(), units.end());
        units.erase(std::unique(units.begin(), units.end()), units.end());
        for (std::size_t i = 0; i < units.size();) {
            std::size_t j = i + 1;
            while (j < units.size() && units[j].first == units[i].first &&
                   units[j].second == units[j - 1].second + 1) {
                ++j;
            }
            const int line = units[i].first, from = units[i].second, to = units[j - 1].second + 1;
            result.push_back(isHorizontal ? Segment{from, line, to, line} : Segment{line, from, line, to});
            i = j;
        }
    };
    merge(horizontal, true);
    merge(vertical, false);
    return result;
}

bool operator==(const Selection& a, const Selection& b) {
    if (a.width_ != b.width_ || a.height_ != b.height_) {
        return false;
    }
    for (std::size_t i = 0; i < a.tiles_.size(); ++i) {
        const auto& ta = a.tiles_[i];
        const auto& tb = b.tiles_[i];
        if (ta == tb) {
            continue;
        }
        if (!ta || !tb || *ta != *tb) {
            return false;
        }
    }
    return true;
}

} // namespace gas
