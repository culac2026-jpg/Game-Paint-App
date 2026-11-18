#include "gas/core/Document.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gas {

namespace {

constexpr int kTile = TiledImage::kTileSize;

void checkSize(int width, int height) {
    if (width <= 0 || height <= 0 || width > Document::kMaxSize || height > Document::kMaxSize) {
        throw std::invalid_argument("Document size must be between 1 and 16384 px");
    }
}

bool sameContent(const TiledImage::TilePtr& a, const TiledImage::TilePtr& b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return std::memcmp(a->pixels.data(), b->pixels.data(), sizeof(a->pixels)) == 0;
}

} // namespace

Document::Document(int width, int height) : width_(width), height_(height) {
    checkSize(width, height);
    layers_.push_back({{nextLayerName()}, TiledImage(width, height)});
    ++createdLayers_;
}

Document::Document(int width, int height, std::vector<Layer> layers, int activeLayer)
    : width_(width), height_(height), layers_(std::move(layers)) {
    checkSize(width, height);
    if (layers_.empty() || layers_.size() > static_cast<std::size_t>(kMaxLayers)) {
        throw std::invalid_argument("Document needs between 1 and 1000 layers");
    }
    for (const Layer& l : layers_) {
        if (l.image.width() != width || l.image.height() != height) {
            throw std::invalid_argument("Layer size does not match the document");
        }
    }
    activeLayer_ = std::clamp(activeLayer, 0, layerCount() - 1);
    createdLayers_ = layerCount();
}

Document::~Document() = default;

std::string Document::nextLayerName() const { return "Layer " + std::to_string(createdLayers_ + 1); }

void Document::setActiveLayer(int index) {
    if (index >= 0 && index < layerCount()) {
        activeLayer_ = index;
    }
}

// ---------------------------------------------------------------- history

void Document::execute(Command cmd) {
    cmd.redo();
    const bool atCleanState = cleanIndex_ == static_cast<long long>(undo_.size());
    const bool hadRedo = !redo_.empty();
    redo_.clear();
    // Merge consecutive property edits of one layer, but never into the step
    // that matches the saved file (that would hide the change from isModified),
    // nor right after an undo (the top step is then older than the undone one).
    if (cmd.mergeLayer >= 0 && !undo_.empty() && undo_.back().mergeLayer == cmd.mergeLayer &&
        !atCleanState && !hadRedo) {
        *undo_.back().mergeAfter = *cmd.mergeAfter;
        return;
    }
    if (cleanIndex_ > static_cast<long long>(undo_.size())) {
        cleanIndex_ = -1; // the saved state was in the discarded redo branch
    }
    undo_.push_back(std::move(cmd));
    enforceHistoryLimit();
}

void Document::enforceHistoryLimit() {
    // Always keep the newest step so the last action can be undone.
    while (undo_.size() > 1 && historyMemory() > historyLimit_) {
        undo_.pop_front();
        cleanIndex_ = cleanIndex_ > 0 ? cleanIndex_ - 1 : -1;
    }
}

std::size_t Document::historyMemory() const {
    std::size_t total = 0;
    for (const Command& c : undo_) {
        total += c.bytes;
    }
    for (const Command& c : redo_) {
        total += c.bytes;
    }
    return total;
}

void Document::setHistoryMemoryLimit(std::size_t bytes) {
    historyLimit_ = bytes;
    enforceHistoryLimit();
}

std::string Document::undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
std::string Document::redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

bool Document::undo() {
    if (!canUndo()) {
        return false;
    }
    Command cmd = std::move(undo_.back());
    undo_.pop_back();
    cmd.undo();
    redo_.push_back(std::move(cmd));
    return true;
}

bool Document::redo() {
    if (!canRedo()) {
        return false;
    }
    Command cmd = std::move(redo_.back());
    redo_.pop_back();
    cmd.redo();
    undo_.push_back(std::move(cmd));
    return true;
}

bool Document::isModified() const { return cleanIndex_ != static_cast<long long>(undo_.size()); }

void Document::markSaved() { cleanIndex_ = static_cast<long long>(undo_.size()); }

// ---------------------------------------------------------------- layer edits

void Document::addLayer() {
    const int index = activeLayer_ + 1;
    const int prevActive = activeLayer_;
    Layer layer{{nextLayerName()}, TiledImage(width_, height_)};
    ++createdLayers_;
    execute({"New Layer",
             [this, index, prevActive] {
                 layers_.erase(layers_.begin() + index);
                 activeLayer_ = prevActive;
                 structureDirty_ = true;
             },
             [this, index, layer] {
                 layers_.insert(layers_.begin() + index, layer);
                 activeLayer_ = index;
                 structureDirty_ = true;
             }});
}

bool Document::duplicateLayer(int index) {
    if (index < 0 || index >= layerCount() || layerCount() >= kMaxLayers) {
        return false;
    }
    Layer copy = layers_[static_cast<std::size_t>(index)]; // shares tiles (copy-on-write)
    copy.props.name += " copy";
    const int prevActive = activeLayer_;
    const int at = index + 1;
    execute({"Duplicate Layer",
             [this, at, prevActive] {
                 layers_.erase(layers_.begin() + at);
                 activeLayer_ = prevActive;
                 structureDirty_ = true;
             },
             [this, at, copy] {
                 layers_.insert(layers_.begin() + at, copy);
                 activeLayer_ = at;
                 structureDirty_ = true;
             }});
    return true;
}

bool Document::removeLayer(int index) {
    if (index < 0 || index >= layerCount() || layerCount() <= 1) {
        return false;
    }
    const Layer removed = layers_[static_cast<std::size_t>(index)];
    const int prevActive = activeLayer_;
    execute({"Delete Layer",
             [this, index, removed, prevActive] {
                 layers_.insert(layers_.begin() + index, removed);
                 activeLayer_ = prevActive;
                 structureDirty_ = true;
             },
             [this, index] {
                 layers_.erase(layers_.begin() + index);
                 activeLayer_ = std::max(0, index - 1); // select the layer below
                 structureDirty_ = true;
             }});
    return true;
}

bool Document::moveLayer(int from, int to) {
    if (from < 0 || to < 0 || from >= layerCount() || to >= layerCount() || from == to) {
        return false;
    }
    const int prevActive = activeLayer_;
    const auto move = [this](int a, int b) {
        Layer l = std::move(layers_[static_cast<std::size_t>(a)]);
        layers_.erase(layers_.begin() + a);
        layers_.insert(layers_.begin() + b, std::move(l));
        structureDirty_ = true;
    };
    execute({"Move Layer",
             [move, from, to, prevActive, this] {
                 move(to, from);
                 activeLayer_ = prevActive;
             },
             [move, from, to, this] {
                 move(from, to);
                 activeLayer_ = to;
             }});
    return true;
}

bool Document::setLayerProps(int index, const LayerProps& props) {
    if (index < 0 || index >= layerCount()) {
        return false;
    }
    LayerProps clean = props;
    clean.opacity = std::clamp(clean.opacity, 0.0, 1.0);
    const LayerProps before = layers_[static_cast<std::size_t>(index)].props;
    if (clean == before) {
        return false;
    }
    auto after = std::make_shared<LayerProps>(clean);
    Command cmd{"Layer Properties",
                [this, index, before] {
                    layers_[static_cast<std::size_t>(index)].props = before;
                    structureDirty_ = true;
                },
                [this, index, after] {
                    layers_[static_cast<std::size_t>(index)].props = *after;
                    structureDirty_ = true;
                }};
    cmd.mergeLayer = index;
    cmd.mergeAfter = after;
    execute(std::move(cmd));
    return true;
}

// ---------------------------------------------------------------- pixel edits

TiledImage* Document::beginEdit() {
    if (edit_) {
        throw std::logic_error("beginEdit() called twice without endEdit()");
    }
    Layer& l = layers_[static_cast<std::size_t>(activeLayer_)];
    if (l.props.locked || !l.props.visible) {
        return nullptr;
    }
    // Holding the snapshot makes every tile the tool touches copy-on-write,
    // so `before` stays exactly as it was.
    edit_ = EditSession{activeLayer_, l.image.snapshot()};
    return &l.image;
}

bool Document::endEdit(const std::string& label) {
    if (!edit_) {
        return false;
    }
    EditSession session = std::move(*edit_);
    edit_.reset();
    TiledImage& image = layers_[static_cast<std::size_t>(session.layer)].image;
    const std::vector<TiledImage::TilePtr> after = image.snapshot();

    struct Change {
        int tx, ty;
        TiledImage::TilePtr before, after;
    };
    std::vector<Change> changes;
    std::size_t bytes = 0;
    const int tx = image.tilesX();
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (sameContent(session.before[i], after[i])) {
            continue;
        }
        changes.push_back({static_cast<int>(i) % tx, static_cast<int>(i) / tx, session.before[i], after[i]});
        bytes += (session.before[i] ? sizeof(TiledImage::Tile) : 0) +
                 (after[i] ? sizeof(TiledImage::Tile) : 0);
    }
    if (changes.empty()) {
        return false;
    }
    const int layer = session.layer;
    // The tiles are already in place, so the first redo just re-sets them.
    Command cmd{label,
                [this, layer, changes] {
                    for (const Change& c : changes) {
                        layers_[static_cast<std::size_t>(layer)].image.setTile(c.tx, c.ty, c.before);
                    }
                },
                [this, layer, changes] {
                    for (const Change& c : changes) {
                        layers_[static_cast<std::size_t>(layer)].image.setTile(c.tx, c.ty, c.after);
                    }
                }};
    cmd.bytes = bytes;
    execute(std::move(cmd));
    return true;
}

// ---------------------------------------------------------------- rendering

bool Document::compositeTile(int tx, int ty, TiledImage::Tile& out) const {
    bool any = false;
    for (const Layer& l : layers_) {
        if (!l.props.visible || l.props.opacity <= 0.0) {
            continue;
        }
        const TiledImage::Tile* t = l.image.tile(tx, ty);
        if (!t) {
            continue;
        }
        const bool opaqueLayer = l.props.opacity >= 1.0;
        if (!any) {
            any = true;
            if (opaqueLayer) {
                out = *t;
                continue;
            }
            out.pixels.fill(Rgba8::transparent());
        }
        for (std::size_t i = 0; i < out.pixels.size(); ++i) {
            const Rgba8 s = t->pixels[i];
            if (s.a == 0) {
                continue;
            }
            if (s.a == 255 && opaqueLayer) {
                out.pixels[i] = s;
            } else {
                out.pixels[i] = blendOver(out.pixels[i], s, l.props.opacity);
            }
        }
    }
    return any;
}

Rgba8 Document::compositePixel(Point p) const {
    Rgba8 out = Rgba8::transparent();
    for (const Layer& l : layers_) {
        if (l.props.visible && l.props.opacity > 0.0) {
            out = blendOver(out, l.image.pixel(p), l.props.opacity);
        }
    }
    return out;
}

std::vector<Rgba8> Document::flatten() const {
    std::vector<Rgba8> pixels(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
    auto tile = std::make_unique<TiledImage::Tile>();
    for (int ty = 0; ty < tilesY(); ++ty) {
        for (int tx = 0; tx < tilesX(); ++tx) {
            if (!compositeTile(tx, ty, *tile)) {
                continue; // stays transparent
            }
            const int w = std::min(kTile, width_ - tx * kTile);
            const int h = std::min(kTile, height_ - ty * kTile);
            for (int y = 0; y < h; ++y) {
                const auto src = tile->pixels.begin() + y * kTile;
                const auto dst = static_cast<std::size_t>(ty * kTile + y) * static_cast<std::size_t>(width_) +
                                 static_cast<std::size_t>(tx * kTile);
                std::copy(src, src + w, pixels.begin() + static_cast<std::ptrdiff_t>(dst));
            }
        }
    }
    return pixels;
}

std::vector<Point> Document::takeDirtyTiles() {
    const int tx = tilesX();
    std::vector<bool> seen(static_cast<std::size_t>(tx * tilesY()), false);
    for (Layer& l : layers_) {
        for (const Point& t : l.image.takeDirtyTiles()) {
            seen[static_cast<std::size_t>(t.y * tx + t.x)] = true;
        }
    }
    if (structureDirty_) {
        structureDirty_ = false;
        std::fill(seen.begin(), seen.end(), true);
    }
    std::vector<Point> result;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (seen[i]) {
            result.push_back({static_cast<int>(i) % tx, static_cast<int>(i) / tx});
        }
    }
    return result;
}

} // namespace gas
