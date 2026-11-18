#include "gas/core/Document.h"

#include "gas/core/Blend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gas {

struct EffectCache {
    std::optional<TiledImage> rendered;
    std::vector<TiledImage::TilePtr> source, mask; // what `rendered` was built from
    std::uint64_t sourceRevision = 0, maskRevision = 0;
    bool hadMask = false;
    std::vector<Effect> effects;
};

namespace {

constexpr int kTile = TiledImage::kTileSize;

LayerProps namedProps(std::string name) {
    LayerProps p;
    p.name = std::move(name);
    return p;
}

int marginTiles(const std::vector<Effect>& effects) { return (effectMargin(effects) + kTile - 1) / kTile; }

// Marks every tile within `reach` tiles of (tx, ty).
void markAround(std::vector<bool>& marks, int tilesX, int tilesY, int tx, int ty, int reach) {
    for (int y = std::max(0, ty - reach); y <= std::min(tilesY - 1, ty + reach); ++y) {
        for (int x = std::max(0, tx - reach); x <= std::min(tilesX - 1, tx + reach); ++x) {
            marks[static_cast<std::size_t>(y * tilesX + x)] = true;
        }
    }
}

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

// Bounds of the non-transparent pixels of `image`.
IRect contentBounds(const TiledImage& image) {
    IRect result;
    for (int ty = 0; ty < image.tilesY(); ++ty) {
        for (int tx = 0; tx < image.tilesX(); ++tx) {
            const TiledImage::Tile* t = image.tile(tx, ty);
            if (!t) {
                continue;
            }
            int minX = kTile, minY = kTile, maxX = -1, maxY = -1;
            for (int y = 0; y < kTile; ++y) {
                for (int x = 0; x < kTile; ++x) {
                    if (t->pixels[static_cast<std::size_t>(y * kTile + x)].a != 0) {
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
    return result.intersected({0, 0, image.width(), image.height()});
}

// The pixels of `src` inside `area`, as an image of size area.w x area.h.
TiledImage croppedImage(const TiledImage& src, IRect area) {
    TiledImage out(area.w, area.h);
    if (area.x % kTile == 0 && area.y % kTile == 0) {
        // Tile-aligned: share tiles instead of copying pixels.
        for (int ty = 0; ty < out.tilesY(); ++ty) {
            for (int tx = 0; tx < out.tilesX(); ++tx) {
                if (auto t = src.sharedTile(tx + area.x / kTile, ty + area.y / kTile)) {
                    out.setTile(tx, ty, std::move(t));
                }
            }
        }
    } else {
        for (int y = 0; y < area.h; ++y) {
            for (int x = 0; x < area.w; ++x) {
                const Rgba8 p = src.pixel({area.x + x, area.y + y});
                if (p.a != 0) {
                    out.setPixel({x, y}, p);
                }
            }
        }
    }
    out.takeDirtyTiles();
    return out;
}

} // namespace

Document::Document(int width, int height) : width_(width), height_(height), selection_(width, height) {
    checkSize(width, height);
    layers_.emplace_back(namedProps(nextLayerName()), TiledImage(width, height));
    normalizeLayer(layers_.back());
    ++createdLayers_;
}

Document::Document(int width, int height, std::vector<Layer> layers, int activeLayer)
    : width_(width), height_(height), layers_(std::move(layers)), selection_(width, height) {
    checkSize(width, height);
    if (layers_.empty() || layers_.size() > static_cast<std::size_t>(kMaxLayers)) {
        throw std::invalid_argument("Document needs between 1 and 1000 layers");
    }
    for (const Layer& l : layers_) {
        if (l.image.width() != width || l.image.height() != height ||
            (l.mask && (l.mask->width() != width || l.mask->height() != height))) {
            throw std::invalid_argument("Layer size does not match the document");
        }
    }
    activeLayer_ = std::clamp(activeLayer, 0, layerCount() - 1);
    createdLayers_ = layerCount();
    for (Layer& l : layers_) {
        l.cels.clear(); // loaders add frames with restoreAnimation()
        normalizeLayer(l);
    }
}

Document::~Document() = default;

std::string Document::nextLayerName() const { return "Layer " + std::to_string(createdLayers_ + 1); }

void Document::setActiveLayer(int index) {
    if (index >= 0 && index < layerCount() && !busy()) {
        activeLayer_ = index;
    }
}

TiledImage& Document::targetImage(int layer, bool mask) {
    Layer& l = layers_[static_cast<std::size_t>(layer)];
    return mask ? *l.mask : l.image;
}

// ---------------------------------------------------------------- history

void Document::execute(Command cmd) {
    if (!cmd.animationState) {
        cmd.frame = activeFrame_;
    }
    cmd.redo();
    syncCels();
    ++historyGeneration_;
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
    if (cmd.mergeSlices && !undo_.empty() && undo_.back().mergeSlices && undo_.back().label == cmd.label &&
        !atCleanState && !hadRedo) {
        *undo_.back().mergeSlices = *cmd.mergeSlices;
        return;
    }
    if (cmd.mergeVector && cmd.mergeToken != 0 && !undo_.empty() && undo_.back().mergeVector &&
        undo_.back().mergeToken == cmd.mergeToken && undo_.back().mergeVectorLayer == cmd.mergeVectorLayer &&
        undo_.back().label == cmd.label && !atCleanState && !hadRedo) {
        *undo_.back().mergeVector = *cmd.mergeVector;
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
    if (cmd.frame >= 0 && cmd.frame != activeFrame_ && cmd.frame < frameCount()) {
        switchFrame(cmd.frame);
    }
    cmd.undo();
    syncCels();
    ++historyGeneration_;
    redo_.push_back(std::move(cmd));
    return true;
}

bool Document::redo() {
    if (!canRedo()) {
        return false;
    }
    Command cmd = std::move(redo_.back());
    redo_.pop_back();
    if (cmd.frame >= 0 && cmd.frame != activeFrame_ && cmd.frame < frameCount()) {
        switchFrame(cmd.frame);
    }
    cmd.redo();
    syncCels();
    ++historyGeneration_;
    undo_.push_back(std::move(cmd));
    return true;
}

bool Document::isModified() const { return cleanIndex_ != static_cast<long long>(undo_.size()); }

void Document::markSaved() { cleanIndex_ = static_cast<long long>(undo_.size()); }

// ---------------------------------------------------------------- layer edits

void Document::addLayer() {
    if (busy()) {
        return;
    }
    const int index = activeLayer_ + 1;
    const int prevActive = activeLayer_;
    Layer layer(namedProps(nextLayerName()), TiledImage(width_, height_));
    normalizeLayer(layer);
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

bool Document::addLayer(const std::string& name, TiledImage image, const std::string& label) {
    if (busy() || image.width() != width_ || image.height() != height_ || layerCount() >= kMaxLayers) {
        return false;
    }
    image.takeDirtyTiles();
    const int index = activeLayer_ + 1;
    const int prevActive = activeLayer_;
    Layer layer(namedProps(name.empty() ? nextLayerName() : name), std::move(image));
    normalizeLayer(layer);
    ++createdLayers_;
    execute({label,
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
    return true;
}

bool Document::duplicateLayer(int index) {
    if (busy() || index < 0 || index >= layerCount() || layerCount() >= kMaxLayers) {
        return false;
    }
    Layer copy = layers_[static_cast<std::size_t>(index)]; // shares tiles (copy-on-write)
    copy.props.name += " copy";
    copy.effectCache.reset(); // two live layers sharing one cache would keep rebuilding it
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
    if (busy() || index < 0 || index >= layerCount() || layerCount() <= 1) {
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
    if (busy() || from < 0 || to < 0 || from >= layerCount() || to >= layerCount() || from == to) {
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
    if (busy() || index < 0 || index >= layerCount()) {
        return false;
    }
    LayerProps clean = props;
    // Float precision, as stored in project files (exact Save/Open round trip).
    clean.opacity = static_cast<float>(std::isfinite(clean.opacity) ? std::clamp(clean.opacity, 0.0, 1.0) : 1.0);
    for (Effect& e : clean.effects) {
        e = sanitized(e);
    }
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

void Document::replaceLayer(int index, Layer after, const std::string& label) {
    after.cels.resize(frames_.size(), TiledImage(width_, height_));
    after.cels[static_cast<std::size_t>(activeFrame_)] = after.image;
    fillVectorCels(after);
    const Layer before = layers_[static_cast<std::size_t>(index)];
    execute({label,
             [this, index, before] {
                 layers_[static_cast<std::size_t>(index)] = before;
                 structureDirty_ = true;
             },
             [this, index, after] {
                 layers_[static_cast<std::size_t>(index)] = after;
                 structureDirty_ = true;
             }});
}

// ---------------------------------------------------------------- layer masks

bool Document::addLayerMask(int index) {
    if (busy() || index < 0 || index >= layerCount() || layers_[static_cast<std::size_t>(index)].mask) {
        return false;
    }
    TiledImage mask(width_, height_); // unpainted = everything shown
    if (!selection_.isEmpty()) {
        // Reveal only the selection: black where unselected, grey ramps on soft edges.
        auto black = std::make_shared<TiledImage::Tile>();
        black->pixels.fill(Rgba8::black());
        const TiledImage::TilePtr blackTile = black;
        for (int ty = 0; ty < mask.tilesY(); ++ty) {
            for (int tx = 0; tx < mask.tilesX(); ++tx) {
                const Selection::MaskTile* s = selection_.tile(tx, ty);
                if (!s) {
                    mask.setTile(tx, ty, blackTile);
                } else if (!selection_.tileFullySelected(tx, ty)) {
                    auto t = std::make_shared<TiledImage::Tile>();
                    for (std::size_t i = 0; i < t->pixels.size(); ++i) {
                        const std::uint8_t v = (*s)[i];
                        t->pixels[i] = {v, v, v, 255};
                    }
                    mask.setTile(tx, ty, std::move(t));
                }
            }
        }
        mask.takeDirtyTiles();
    }
    Layer after = layers_[static_cast<std::size_t>(index)];
    after.mask = std::move(mask);
    after.props.maskEnabled = true;
    replaceLayer(index, std::move(after), "Add Layer Mask");
    editingMask_ = true;
    return true;
}

bool Document::removeLayerMask(int index) {
    if (busy() || index < 0 || index >= layerCount() || !layers_[static_cast<std::size_t>(index)].mask) {
        return false;
    }
    Layer after = layers_[static_cast<std::size_t>(index)];
    after.mask.reset();
    replaceLayer(index, std::move(after), "Delete Layer Mask");
    return true;
}

bool Document::applyLayerMask(int index) {
    if (busy() || index < 0 || index >= layerCount() || !layers_[static_cast<std::size_t>(index)].mask) {
        return false;
    }
    Layer after = layers_[static_cast<std::size_t>(index)];
    if (after.props.maskEnabled) {
        for (TiledImage& img : after.cels) { // the mask belongs to the layer: every frame
            for (int ty = 0; ty < img.tilesY(); ++ty) {
                for (int tx = 0; tx < img.tilesX(); ++tx) {
                    const TiledImage::Tile* m = after.mask->tile(tx, ty);
                    if (!m || !img.tile(tx, ty)) {
                        continue; // unpainted mask = fully shown; empty layer tile stays empty
                    }
                    TiledImage::Tile& t = img.editTile(tx, ty);
                    for (std::size_t i = 0; i < t.pixels.size(); ++i) {
                        t.pixels[i] = eraseAlpha(t.pixels[i], 1.0 - maskValue(m->pixels[i]) / 255.0);
                    }
                }
            }
            img.takeDirtyTiles();
        }
        after.image = after.cels[static_cast<std::size_t>(activeFrame_)];
    }
    after.mask.reset();
    after.vector.reset(); // the pixels no longer match the shapes
    replaceLayer(index, std::move(after), "Apply Layer Mask");
    return true;
}

bool Document::rasterizeLayerEffects(int index) {
    if (busy() || index < 0 || index >= layerCount() ||
        !hasActiveEffects(layers_[static_cast<std::size_t>(index)].props.effects)) {
        return false;
    }
    Layer after = layers_[static_cast<std::size_t>(index)];
    for (int f = 0; f < frameCount(); ++f) {
        if (f == activeFrame_) {
            continue;
        }
        Layer frameLayer = after; // render this frame's cel with the layer's effects
        frameLayer.image = after.cels[static_cast<std::size_t>(f)];
        frameLayer.effectCache.reset();
        TiledImage rendered = effectImage(frameLayer);
        rendered.takeDirtyTiles();
        after.cels[static_cast<std::size_t>(f)] = std::move(rendered);
    }
    after.image = renderedLayer(index); // shares the rendered tiles
    after.image.takeDirtyTiles();
    after.mask.reset();
    after.props.effects.clear();
    after.effectCache.reset();
    after.vector.reset();
    replaceLayer(index, std::move(after), "Rasterize Effects");
    return true;
}

bool Document::editingMask() const { return editingMask_ && activeLayer().mask.has_value(); }

// ---------------------------------------------------------------- vector layers

bool Document::isVectorLayer(int index) const {
    return index >= 0 && index < layerCount() && layers_[static_cast<std::size_t>(index)].vector != nullptr;
}

bool Document::addVectorLayer(VectorShapes shapes, const std::string& label) {
    if (busy() || layerCount() >= kMaxLayers) {
        return false;
    }
    const int index = activeLayer_ + 1;
    const int prevActive = activeLayer_;
    shapes = sanitizedShapes(std::move(shapes));
    Layer layer(namedProps("Vector " + std::to_string(createdLayers_ + 1)), rasterizeShapes(shapes, width_, height_));
    layer.vector = std::make_shared<const VectorShapes>(std::move(shapes));
    normalizeLayer(layer);
    ++createdLayers_;
    execute({label,
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
    return true;
}

bool Document::setVectorShapes(int index, VectorShapes shapes, const std::string& label, int mergeToken) {
    if (busy() || !isVectorLayer(index)) {
        return false;
    }
    Layer& l = layers_[static_cast<std::size_t>(index)];
    shapes = sanitizedShapes(std::move(shapes));
    if (*l.vector == shapes) {
        return false;
    }
    VectorState before{l.vector, l.image.snapshot()};
    const TiledImage rendered = rasterizeShapes(shapes, width_, height_);
    auto after = std::make_shared<VectorState>(
        VectorState{std::make_shared<const VectorShapes>(std::move(shapes)), rendered.snapshot()});
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < after->tiles.size(); ++i) {
        if (sameContent(before.tiles[i], after->tiles[i])) {
            after->tiles[i] = before.tiles[i]; // unchanged tiles keep their pointer (no redraw, no memory)
        } else if (after->tiles[i]) {
            bytes += sizeof(TiledImage::Tile);
        }
    }
    const auto apply = [this, index](const VectorState& s) {
        Layer& layer = layers_[static_cast<std::size_t>(index)];
        layer.vector = s.shapes;
        const int tx = layer.image.tilesX();
        for (std::size_t i = 0; i < s.tiles.size(); ++i) {
            const int x = static_cast<int>(i) % tx, y = static_cast<int>(i) / tx;
            if (layer.image.sharedTile(x, y) != s.tiles[i]) {
                layer.image.setTile(x, y, s.tiles[i]);
            }
        }
    };
    Command cmd{label, [apply, before = std::move(before)] { apply(before); }, [apply, after] { apply(*after); }};
    cmd.bytes = bytes;
    cmd.mergeVector = after;
    cmd.mergeVectorLayer = index;
    cmd.mergeToken = mergeToken;
    execute(std::move(cmd));
    return true;
}

bool Document::rasterizeVectorLayer(int index) {
    if (busy() || !isVectorLayer(index)) {
        return false;
    }
    Layer after = layers_[static_cast<std::size_t>(index)];
    after.vector.reset();
    replaceLayer(index, std::move(after), "Rasterize Vector Layer");
    return true;
}

// ---------------------------------------------------------------- selection

bool Document::setSelection(Selection selection, const std::string& label) {
    if (busy() || selection.width() != width_ || selection.height() != height_ || selection == selection_) {
        return false;
    }
    const Selection before = selection_;
    execute({label,
             [this, before] {
                 selection_ = before;
                 ++selectionGeneration_;
             },
             [this, after = std::move(selection)] {
                 selection_ = after;
                 ++selectionGeneration_;
             }});
    return true;
}

bool Document::selectAll() { return setSelection(Selection::all(width_, height_), "Select All"); }
bool Document::deselect() { return setSelection(Selection(width_, height_), "Deselect"); }
bool Document::invertSelection() { return setSelection(selection_.inverted(), "Invert Selection"); }

// ---------------------------------------------------------------- pixel edits

TiledImage* Document::beginEdit() {
    if (busy()) {
        return nullptr;
    }
    Layer& l = layers_[static_cast<std::size_t>(activeLayer_)];
    if (l.props.locked || !l.props.visible) {
        return nullptr;
    }
    const bool mask = editingMask();
    if (l.vector && !mask) {
        return nullptr; // shapes are edited with the vector tools
    }
    TiledImage& target = mask ? *l.mask : l.image;
    // Holding the snapshot makes every tile the tool touches copy-on-write,
    // so `before` stays exactly as it was.
    edit_ = EditSession{activeLayer_, mask, target.snapshot(), std::nullopt};
    if (selection_.isEmpty()) {
        return &target;
    }
    edit_->scratch = target; // shares tiles; the tool writes here, syncEdit() clips into target
    edit_->scratch->takeDirtyTiles();
    return &*edit_->scratch;
}

void Document::revertEdit() {
    if (!edit_) {
        return;
    }
    const auto restore = [&](TiledImage& img) {
        for (int ty = 0; ty < img.tilesY(); ++ty) {
            for (int tx = 0; tx < img.tilesX(); ++tx) {
                const TiledImage::TilePtr& before = edit_->before[static_cast<std::size_t>(ty * img.tilesX() + tx)];
                if (img.sharedTile(tx, ty) != before) {
                    img.setTile(tx, ty, before);
                }
            }
        }
    };
    restore(targetImage(edit_->layer, edit_->mask));
    if (edit_->scratch) {
        restore(*edit_->scratch);
    }
}

void Document::syncEdit() {
    if (!edit_ || !edit_->scratch) {
        return;
    }
    TiledImage& scratch = *edit_->scratch;
    TiledImage& target = targetImage(edit_->layer, edit_->mask);
    for (const Point& t : scratch.takeDirtyTiles()) {
        const Selection::MaskTile* cov = selection_.tile(t.x, t.y);
        if (!cov) {
            continue; // nothing selected here: the target keeps its original tile
        }
        if (selection_.tileFullySelected(t.x, t.y)) {
            target.setTile(t.x, t.y, scratch.sharedTile(t.x, t.y));
            continue;
        }
        // Always recompute from the original, so repeated syncs never compound.
        const TiledImage::TilePtr& before = edit_->before[static_cast<std::size_t>(t.y * scratch.tilesX() + t.x)];
        const TiledImage::Tile* drawn = scratch.tile(t.x, t.y);
        auto out = std::make_shared<TiledImage::Tile>();
        for (std::size_t i = 0; i < out->pixels.size(); ++i) {
            const Rgba8 b = before ? before->pixels[i] : Rgba8::transparent();
            const Rgba8 a = drawn ? drawn->pixels[i] : Rgba8::transparent();
            const std::uint8_t c = (*cov)[i];
            out->pixels[i] = c == 255 ? a : c == 0 ? b : mix(b, a, c / 255.0);
        }
        target.setTile(t.x, t.y, std::move(out));
    }
}

std::vector<Document::TileChange> Document::diffTiles(const std::vector<TiledImage::TilePtr>& before,
                                                      const TiledImage& image, std::size_t& bytes) {
    std::vector<TileChange> changes;
    bytes = 0;
    const std::vector<TiledImage::TilePtr> after = image.snapshot();
    const int tx = image.tilesX();
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (sameContent(before[i], after[i])) {
            continue;
        }
        changes.push_back({static_cast<int>(i) % tx, static_cast<int>(i) / tx, before[i], after[i]});
        bytes += (before[i] ? sizeof(TiledImage::Tile) : 0) + (after[i] ? sizeof(TiledImage::Tile) : 0);
    }
    return changes;
}

Document::Command Document::tileCommand(const std::string& label, int layer, bool mask,
                                        std::vector<TileChange> changes, std::size_t bytes) {
    // The tiles are already in place, so the first redo just re-sets them.
    auto shared = std::make_shared<const std::vector<TileChange>>(std::move(changes));
    Command cmd{label,
                [this, layer, mask, shared] {
                    for (const TileChange& c : *shared) {
                        targetImage(layer, mask).setTile(c.tx, c.ty, c.before);
                    }
                },
                [this, layer, mask, shared] {
                    for (const TileChange& c : *shared) {
                        targetImage(layer, mask).setTile(c.tx, c.ty, c.after);
                    }
                }};
    cmd.bytes = bytes;
    return cmd;
}

bool Document::endEdit(const std::string& label) {
    if (!edit_) {
        return false;
    }
    syncEdit();
    EditSession session = std::move(*edit_);
    edit_.reset();
    std::size_t bytes = 0;
    std::vector<TileChange> changes = diffTiles(session.before, targetImage(session.layer, session.mask), bytes);
    if (changes.empty()) {
        return false;
    }
    execute(tileCommand(label, session.layer, session.mask, std::move(changes), bytes));
    return true;
}

bool Document::eraseSelected() { return fillSelected(Rgba8::transparent()); }

bool Document::fillSelected(Rgba8 color) {
    TiledImage* img = beginEdit();
    if (!img) {
        return false;
    }
    const IRect area = selection_.bounds().value_or(IRect{0, 0, width_, height_});
    const bool clear = color.a == 0;
    for (int ty = area.y / kTile; ty <= (area.bottom() - 1) / kTile; ++ty) {
        for (int tx = area.x / kTile; tx <= (area.right() - 1) / kTile; ++tx) {
            const IRect part = area.intersected({tx * kTile, ty * kTile, kTile, kTile});
            if (clear && part.w == kTile && part.h == kTile) {
                img->setTile(tx, ty, nullptr); // whole tile: drop it
                continue;
            }
            TiledImage::Tile& t = img->editTile(tx, ty);
            for (int y = part.y; y < part.bottom(); ++y) {
                for (int x = part.x; x < part.right(); ++x) {
                    t.pixels[static_cast<std::size_t>(TiledImage::pixelIndex(x, y))] = clear ? Rgba8::transparent()
                                                                                              : color;
                }
            }
        }
    }
    return endEdit(clear ? "Clear" : "Fill");
}

// ---------------------------------------------------------------- transform

void Document::restoreTiles(TiledImage& image, const std::vector<TiledImage::TilePtr>& from, IRect area) {
    if (area.empty()) {
        return;
    }
    for (int ty = area.y / kTile; ty <= (area.bottom() - 1) / kTile; ++ty) {
        for (int tx = area.x / kTile; tx <= (area.right() - 1) / kTile; ++tx) {
            const auto& tile = from[static_cast<std::size_t>(ty * image.tilesX() + tx)];
            if (image.sharedTile(tx, ty) != tile) {
                image.setTile(tx, ty, tile);
            }
        }
    }
}

bool Document::beginTransform() {
    if (busy()) {
        return false;
    }
    Layer& l = layers_[static_cast<std::size_t>(activeLayer_)];
    if (l.props.locked || !l.props.visible) {
        return false;
    }
    TiledImage& img = l.image;
    const bool useSelection = !selection_.isEmpty() && !l.vector;
    const IRect area = useSelection ? selection_.bounds().value_or(IRect{}) : contentBounds(img);
    if (area.empty()) {
        return false;
    }

    TransformSession s{activeLayer_, img.snapshot(), {}, {}, selection_, Affine{}, {}};
    s.floating.rect = area;
    s.floating.pixels.resize(static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h));
    bool anything = false;
    for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
            const Rgba8 p = img.pixel({x, y});
            const std::uint8_t c = useSelection ? selection_.coverage({x, y}) : 255;
            if (p.a == 0 || c == 0) {
                continue;
            }
            Rgba8 lifted = p;
            if (c < 255) {
                lifted = eraseAlpha(p, 1.0 - c / 255.0); // the selected share of the pixel
            }
            s.floating.pixels[static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.w) +
                              static_cast<std::size_t>(x - area.x)] = lifted;
            img.setPixel({x, y}, c == 255 ? Rgba8::transparent() : eraseAlpha(p, c / 255.0));
            anything = anything || lifted.a != 0;
        }
    }
    if (!anything) {
        restoreTiles(img, s.before, area);
        img.takeDirtyTiles();
        return false; // only transparent pixels selected
    }
    s.hole = img.snapshot();
    s.previewArea = transformInto(img, s.floating, s.m, Interpolation::Nearest);
    transform_ = std::move(s);
    return true;
}

IRect Document::transformSourceRect() const { return transform_ ? transform_->floating.rect : IRect{}; }

Affine Document::currentTransform() const { return transform_ ? transform_->m : Affine{}; }

void Document::setTransform(const Affine& m) {
    if (!transform_) {
        return;
    }
    TiledImage& img = layers_[static_cast<std::size_t>(transform_->layer)].image;
    restoreTiles(img, transform_->hole, transform_->previewArea);
    transform_->m = m;
    transform_->previewArea = transformInto(img, transform_->floating, m, Interpolation::Nearest);
}

bool Document::commitTransform(Interpolation interpolation, const std::string& label) {
    if (!transform_) {
        return false;
    }
    if (transform_->m.isIdentity()) {
        cancelTransform();
        return false;
    }
    TransformSession s = std::move(*transform_);
    transform_.reset();
    TiledImage& img = layers_[static_cast<std::size_t>(s.layer)].image;
    if (const auto shapes = layers_[static_cast<std::size_t>(s.layer)].vector) {
        restoreTiles(img, s.before, {0, 0, width_, height_});
        img.takeDirtyTiles();
        markAllDirty();
        return setVectorShapes(s.layer, transformedShapes(*shapes, s.m), label);
    }
    restoreTiles(img, s.hole, s.previewArea);
    transformInto(img, s.floating, s.m, interpolation);

    std::size_t bytes = 0;
    std::vector<TileChange> changes = diffTiles(s.before, img, bytes);
    Command pixels = tileCommand(label, s.layer, false, std::move(changes), bytes);
    const Selection selBefore = s.selectionBefore;
    const Selection selAfter =
        selBefore.isEmpty() ? selBefore : selBefore.transformed(s.m, interpolation);
    Command cmd{label,
                [this, undoPixels = pixels.undo, selBefore] {
                    undoPixels();
                    selection_ = selBefore;
                    ++selectionGeneration_;
                },
                [this, redoPixels = pixels.redo, selAfter] {
                    redoPixels();
                    selection_ = selAfter;
                    ++selectionGeneration_;
                }};
    cmd.bytes = bytes;
    execute(std::move(cmd));
    return true;
}

void Document::cancelTransform() {
    if (!transform_) {
        return;
    }
    TiledImage& img = layers_[static_cast<std::size_t>(transform_->layer)].image;
    restoreTiles(img, transform_->before, {0, 0, width_, height_});
    transform_.reset();
}

bool Document::flipContent(bool horizontal) {
    if (!beginTransform()) {
        return false;
    }
    const IRect r = transformSourceRect();
    const PointF centre{r.x + r.w / 2.0, r.y + r.h / 2.0};
    setTransform(Affine::around(centre, horizontal ? Affine::scale(-1, 1) : Affine::scale(1, -1)));
    return commitTransform(Interpolation::Nearest, horizontal ? "Flip Horizontal" : "Flip Vertical");
}

bool Document::rotateContent90(bool clockwise) {
    if (!beginTransform()) {
        return false;
    }
    const IRect r = transformSourceRect();
    const PointF centre{r.x + r.w / 2.0, r.y + r.h / 2.0};
    // Exact matrices (no trig rounding). With y pointing down, +90 degrees turns clockwise.
    const Affine turn = clockwise ? Affine{0, 1, -1, 0, 0, 0} : Affine{0, -1, 1, 0, 0, 0};
    Affine m = Affine::around(centre, turn);
    // When w and h differ in parity the turned block would straddle pixels, so
    // snap it to the grid. Integer division truncates toward zero, and since
    // trunc(a) + trunc(-a) == 0, any two quarter turns cancel exactly (no drift).
    const double exactLeft = centre.x - r.h / 2.0, exactTop = centre.y - r.w / 2.0;
    const int left = r.x + (r.w - r.h) / 2, top = r.y + (r.h - r.w) / 2;
    m = Affine::translate(left - exactLeft, top - exactTop) * m;
    setTransform(m);
    return commitTransform(Interpolation::Nearest, clockwise ? "Rotate 90° CW" : "Rotate 90° CCW");
}

// ---------------------------------------------------------------- canvas

// ---------------------------------------------------------------- frames

void Document::syncCels() {
    for (Layer& l : layers_) {
        if (l.cels.size() != frames_.size()) {
            l.cels.resize(frames_.size(), TiledImage(width_, height_));
        }
        l.cels[static_cast<std::size_t>(activeFrame_)] = l.image;
        fillVectorCels(l);
    }
}

void Document::fillVectorCels(Layer& layer) {
    if (layer.vector) {
        for (TiledImage& c : layer.cels) {
            c = layer.image;
        }
    }
}

void Document::switchFrame(int index) {
    syncCels();
    activeFrame_ = index;
    for (Layer& l : layers_) {
        l.image = l.cels[static_cast<std::size_t>(index)];
    }
    structureDirty_ = true;
    ++framesGeneration_;
}

void Document::normalizeLayer(Layer& layer) const {
    layer.cels.resize(frames_.size(), TiledImage(width_, height_));
    layer.cels[static_cast<std::size_t>(activeFrame_)] = layer.image;
    fillVectorCels(layer);
}

bool Document::setActiveFrame(int index) {
    if (busy() || index < 0 || index >= frameCount()) {
        return false;
    }
    if (index != activeFrame_) {
        switchFrame(index);
    }
    return true;
}

Document::AnimationState Document::animationState() {
    syncCels();
    return {layers_, frames_, tags_, activeFrame_};
}

void Document::executeAnimation(const std::string& label, AnimationState after) {
    for (Layer& l : after.layers) {
        fillVectorCels(l); // new frames of a vector layer show its shapes too
        l.image = l.cels[static_cast<std::size_t>(after.activeFrame)];
    }
    auto before = std::make_shared<const AnimationState>(animationState());
    auto next = std::make_shared<const AnimationState>(std::move(after));
    const auto apply = [this](const AnimationState& s) {
        layers_ = s.layers;
        frames_ = s.frames;
        tags_ = s.tags;
        activeFrame_ = s.activeFrame;
        structureDirty_ = true;
        ++framesGeneration_;
    };
    Command cmd{label, [apply, before] { apply(*before); }, [apply, next] { apply(*next); }};
    cmd.animationState = true;
    execute(std::move(cmd));
}

namespace {

// Tag ranges after inserting one frame at `at` (after frame at - 1).
void shiftTagsForInsert(std::vector<FrameTag>& tags, int at) {
    for (FrameTag& t : tags) {
        if (t.from >= at) { // entirely after the insertion point: shifts
            ++t.from;
            ++t.to;
        } else if (t.to >= at - 1) { // inserted inside the tag or right after its end: grows
            ++t.to;
        }
    }
}

} // namespace

bool Document::addFrame() {
    if (busy() || frameCount() >= kMaxFrames) {
        return false;
    }
    AnimationState s = animationState();
    const int at = activeFrame_ + 1;
    for (Layer& l : s.layers) {
        l.cels.insert(l.cels.begin() + at, TiledImage(width_, height_));
    }
    s.frames.insert(s.frames.begin() + at, frames_[static_cast<std::size_t>(activeFrame_)]);
    shiftTagsForInsert(s.tags, at);
    s.activeFrame = at;
    executeAnimation("New Frame", std::move(s));
    return true;
}

bool Document::duplicateFrame(int index) {
    if (busy() || index < 0 || index >= frameCount() || frameCount() >= kMaxFrames) {
        return false;
    }
    AnimationState s = animationState();
    const int at = index + 1;
    for (Layer& l : s.layers) {
        TiledImage copy = l.cels[static_cast<std::size_t>(index)]; // shares tiles (copy-on-write)
        l.cels.insert(l.cels.begin() + at, std::move(copy));
    }
    s.frames.insert(s.frames.begin() + at, frames_[static_cast<std::size_t>(index)]);
    shiftTagsForInsert(s.tags, at);
    s.activeFrame = at;
    executeAnimation("Duplicate Frame", std::move(s));
    return true;
}

bool Document::removeFrame(int index) {
    if (busy() || index < 0 || index >= frameCount() || frameCount() <= 1) {
        return false;
    }
    AnimationState s = animationState();
    for (Layer& l : s.layers) {
        l.cels.erase(l.cels.begin() + index);
    }
    s.frames.erase(s.frames.begin() + index);
    std::vector<FrameTag> tags;
    for (FrameTag t : s.tags) {
        if (t.from == index && t.to == index) {
            continue; // its only frame is gone
        }
        if (t.from > index) {
            --t.from;
        }
        if (t.to >= index) {
            --t.to;
        }
        tags.push_back(std::move(t));
    }
    s.tags = std::move(tags);
    if (s.activeFrame > index || s.activeFrame >= static_cast<int>(s.frames.size())) {
        --s.activeFrame;
    }
    executeAnimation("Delete Frame", std::move(s));
    return true;
}

bool Document::moveFrame(int from, int to) {
    if (busy() || from < 0 || to < 0 || from >= frameCount() || to >= frameCount() || from == to) {
        return false;
    }
    AnimationState s = animationState();
    const auto move = [from, to](auto& v) {
        auto item = std::move(v[static_cast<std::size_t>(from)]);
        v.erase(v.begin() + from);
        v.insert(v.begin() + to, std::move(item));
    };
    for (Layer& l : s.layers) {
        move(l.cels);
    }
    move(s.frames);
    // The active frame follows its content.
    const int a = activeFrame_;
    if (a == from) {
        s.activeFrame = to;
    } else if (from < a && a <= to) {
        s.activeFrame = a - 1;
    } else if (to <= a && a < from) {
        s.activeFrame = a + 1;
    }
    executeAnimation("Move Frame", std::move(s));
    return true;
}

bool Document::setFrameDuration(int index, int ms) {
    if (index < 0 || index >= frameCount()) {
        return false;
    }
    std::vector<Frame> frames = frames_;
    frames[static_cast<std::size_t>(index)].duration = ms;
    return setFrames(std::move(frames), "Frame Duration");
}

bool Document::setFrames(std::vector<Frame> frames, const std::string& label) {
    if (busy() || frames.size() != frames_.size()) {
        return false;
    }
    for (Frame& f : frames) {
        f.duration = std::clamp(f.duration, kMinFrameDuration, kMaxFrameDuration);
    }
    if (frames == frames_) {
        return false;
    }
    AnimationState s = animationState();
    s.frames = std::move(frames);
    executeAnimation(label, std::move(s));
    return true;
}

std::vector<FrameTag> Document::sanitizedTags(std::vector<FrameTag> tags, int count) const {
    std::vector<FrameTag> out;
    for (FrameTag& t : tags) {
        if (t.name.empty()) {
            continue;
        }
        t.from = std::clamp(t.from, 0, count - 1);
        t.to = std::clamp(t.to, 0, count - 1);
        if (t.from > t.to) {
            std::swap(t.from, t.to);
        }
        const int d = static_cast<int>(t.direction);
        if (d < 0 || d >= kTagDirectionCount) {
            t.direction = TagDirection::Forward;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool Document::setTags(std::vector<FrameTag> tags, const std::string& label) {
    tags = sanitizedTags(std::move(tags), frameCount());
    if (busy() || tags == tags_) {
        return false;
    }
    AnimationState s = animationState();
    s.tags = std::move(tags);
    executeAnimation(label, std::move(s));
    return true;
}

const TiledImage& Document::cel(int layer, int frame) const {
    const Layer& l = layers_.at(static_cast<std::size_t>(layer));
    return frame == activeFrame_ ? l.image : l.cels.at(static_cast<std::size_t>(frame));
}

bool Document::celEmpty(int layer, int frame) const {
    const TiledImage& img = cel(layer, frame);
    for (int ty = 0; ty < img.tilesY(); ++ty) {
        for (int tx = 0; tx < img.tilesX(); ++tx) {
            if (img.tile(tx, ty)) {
                return false;
            }
        }
    }
    return true;
}

TiledImage Document::flattenedFrame(int frame) const {
    if (frame == activeFrame_) {
        return flattenedImage();
    }
    std::vector<Layer> layers = layers_;
    for (Layer& l : layers) {
        l.image = l.cels.at(static_cast<std::size_t>(frame));
        l.effectCache.reset(); // a private cache: don't thrash the active frame's
    }
    TiledImage out(width_, height_);
    for (int ty = 0; ty < tilesY(); ++ty) {
        for (int tx = 0; tx < tilesX(); ++tx) {
            auto tile = std::make_shared<TiledImage::Tile>();
            if (compositeLayersTile(*this, layers, tx, ty, *tile)) {
                out.setTile(tx, ty, std::move(tile));
            }
        }
    }
    out.takeDirtyTiles();
    return out;
}

void Document::restoreAnimation(std::vector<Frame> frames, std::vector<std::vector<TiledImage>> cels,
                                std::vector<FrameTag> tags, int activeFrame) {
    if (frames.empty() || frames.size() > static_cast<std::size_t>(kMaxFrames) || cels.size() != layers_.size()) {
        throw std::invalid_argument("Invalid animation data");
    }
    for (const auto& layerCels : cels) {
        if (layerCels.size() != frames.size()) {
            throw std::invalid_argument("Cel count does not match the frames");
        }
        for (const TiledImage& img : layerCels) {
            if (img.width() != width_ || img.height() != height_) {
                throw std::invalid_argument("Cel size does not match the document");
            }
        }
    }
    for (Frame& f : frames) {
        f.duration = std::clamp(f.duration, kMinFrameDuration, kMaxFrameDuration);
    }
    frames_ = std::move(frames);
    activeFrame_ = std::clamp(activeFrame, 0, frameCount() - 1);
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        layers_[i].cels = std::move(cels[i]);
        layers_[i].image = layers_[i].cels[static_cast<std::size_t>(activeFrame_)];
    }
    tags_ = sanitizedTags(std::move(tags), frameCount());
    structureDirty_ = true;
    ++framesGeneration_;
}

// ---------------------------------------------------------------- slices

std::vector<Slice> Document::sanitizedSlices(std::vector<Slice> slices) const {
    std::vector<Slice> out;
    out.reserve(slices.size());
    const auto clampPivot = [](double v) {
        return static_cast<double>(static_cast<float>(std::isfinite(v) ? std::clamp(v, -4.0, 5.0) : 0.5)); // as stored
    };
    for (Slice& s : slices) {
        s.rect = s.rect.intersected({0, 0, width_, height_});
        if (s.rect.empty()) {
            continue;
        }
        s.pivot = {clampPivot(s.pivot.x), clampPivot(s.pivot.y)};
        // Insets stay inside the rect and the opposite pair never crosses.
        SliceBorder& b = s.border;
        b.left = std::clamp(b.left, 0, s.rect.w);
        b.right = std::clamp(b.right, 0, s.rect.w - b.left);
        b.top = std::clamp(b.top, 0, s.rect.h);
        b.bottom = std::clamp(b.bottom, 0, s.rect.h - b.top);
        out.push_back(std::move(s));
    }
    return out;
}

bool Document::setSlices(std::vector<Slice> slices, const std::string& label, bool merge) {
    slices = sanitizedSlices(std::move(slices));
    if (busy() || slices == slices_) {
        return false;
    }
    std::vector<Slice> before = slices_;
    auto after = std::make_shared<std::vector<Slice>>(std::move(slices));
    Command cmd{label,
                [this, before = std::move(before)] {
                    slices_ = before;
                    ++slicesGeneration_;
                },
                [this, after] {
                    slices_ = *after;
                    ++slicesGeneration_;
                }};
    if (merge) {
        cmd.mergeSlices = after;
    }
    execute(std::move(cmd));
    return true;
}

void Document::restoreSlices(std::vector<Slice> slices) {
    slices_ = sanitizedSlices(std::move(slices));
    ++slicesGeneration_;
}

TiledImage Document::flattenedImage() const {
    TiledImage out(width_, height_);
    for (int ty = 0; ty < tilesY(); ++ty) {
        for (int tx = 0; tx < tilesX(); ++tx) {
            auto tile = std::make_shared<TiledImage::Tile>();
            if (compositeTile(tx, ty, *tile)) {
                out.setTile(tx, ty, std::move(tile));
            }
        }
    }
    out.takeDirtyTiles();
    return out;
}

bool Document::crop(IRect area) {
    area = area.intersected({0, 0, width_, height_});
    if (busy() || area.empty() || area == IRect{0, 0, width_, height_}) {
        return false;
    }
    syncCels();
    std::vector<Layer> cropped;
    cropped.reserve(layers_.size());
    for (const Layer& l : layers_) {
        Layer c(l.props, croppedImage(l.image, area));
        if (l.mask) {
            c.mask = croppedImage(*l.mask, area);
        }
        if (l.vector) {
            c.vector = std::make_shared<const VectorShapes>(
                transformedShapes(*l.vector, Affine::translate(-area.x, -area.y)));
            c.image = rasterizeShapes(*c.vector, area.w, area.h);
            c.cels.assign(frames_.size(), c.image);
            cropped.push_back(std::move(c));
            continue;
        }
        for (int f = 0; f < frameCount(); ++f) {
            c.cels.push_back(f == activeFrame_ ? c.image : croppedImage(l.cels[static_cast<std::size_t>(f)], area));
        }
        cropped.push_back(std::move(c));
    }
    std::vector<Slice> movedSlices;
    for (Slice s : slices_) {
        s.rect = s.rect.intersected(area);
        if (!s.rect.empty()) {
            s.rect.x -= area.x;
            s.rect.y -= area.y;
            movedSlices.push_back(std::move(s));
        }
    }
    struct State {
        int width, height;
        std::vector<Layer> layers;
        Selection selection;
        std::vector<Slice> slices;
    };
    auto before = std::make_shared<const State>(State{width_, height_, layers_, selection_, slices_});
    auto after = std::make_shared<const State>(
        State{area.w, area.h, std::move(cropped), Selection(area.w, area.h), std::move(movedSlices)});
    const auto apply = [this](const State& s) {
        width_ = s.width;
        height_ = s.height;
        layers_ = s.layers;
        selection_ = s.selection;
        slices_ = s.slices;
        ++selectionGeneration_;
        ++slicesGeneration_;
        structureDirty_ = true;
    };
    execute({"Crop", [apply, before] { apply(*before); }, [apply, after] { apply(*after); }});
    return true;
}

// ---------------------------------------------------------------- rendering

const TiledImage& Document::renderedLayer(int index) const {
    const Layer& l = layers_.at(static_cast<std::size_t>(index));
    return hasActiveEffects(l.props.effects) ? effectImage(l) : l.image;
}

const TiledImage& Document::effectImage(const Layer& l) const {
    if (!l.effectCache) {
        l.effectCache = std::make_shared<EffectCache>();
    }
    EffectCache& c = *l.effectCache;
    const TiledImage* mask = (l.mask && l.props.maskEnabled) ? &*l.mask : nullptr;
    const bool full = !c.rendered || c.rendered->width() != l.image.width() ||
                      c.rendered->height() != l.image.height() || c.effects != l.props.effects ||
                      c.hadMask != (mask != nullptr);
    if (!full && c.sourceRevision == l.image.revision() && (!mask || c.maskRevision == mask->revision())) {
        return *c.rendered;
    }
    const int tx = l.image.tilesX(), ty = l.image.tilesY();
    const int reach = marginTiles(l.props.effects);
    std::vector<TiledImage::TilePtr> source = l.image.snapshot();
    std::vector<TiledImage::TilePtr> maskTiles = mask ? mask->snapshot() : std::vector<TiledImage::TilePtr>{};
    std::vector<bool> marks(static_cast<std::size_t>(tx * ty), false);
    if (full) {
        c.rendered.emplace(l.image.width(), l.image.height());
        for (int y = 0; y < ty; ++y) {
            for (int x = 0; x < tx; ++x) {
                if (source[static_cast<std::size_t>(y * tx + x)]) {
                    markAround(marks, tx, ty, x, y, reach);
                }
            }
        }
    } else {
        // Copy-on-write makes any edited tile a new pointer, so a pointer diff finds every change.
        for (int y = 0; y < ty; ++y) {
            for (int x = 0; x < tx; ++x) {
                const auto i = static_cast<std::size_t>(y * tx + x);
                if (source[i] != c.source[i] || (mask && maskTiles[i] != c.mask[i])) {
                    markAround(marks, tx, ty, x, y, reach);
                }
            }
        }
    }
    for (int y = 0; y < ty; ++y) {
        for (int x = 0; x < tx; ++x) {
            if (marks[static_cast<std::size_t>(y * tx + x)]) {
                renderEffects(l.image, mask, l.props.effects, {x, y, 1, 1}, *c.rendered);
            }
        }
    }
    c.rendered->takeDirtyTiles();
    c.source = std::move(source);
    c.mask = std::move(maskTiles);
    c.sourceRevision = l.image.revision();
    c.maskRevision = mask ? mask->revision() : 0;
    c.hadMask = mask != nullptr;
    c.effects = l.props.effects;
    return *c.rendered;
}

bool Document::compositeTile(int tx, int ty, TiledImage::Tile& out) const {
    return compositeLayersTile(*this, layers_, tx, ty, out);
}

bool Document::compositeLayersTile(const Document& doc, const std::vector<Layer>& layers, int tx, int ty,
                                   TiledImage::Tile& out) {
    bool any = false;
    for (const Layer& l : layers) {
        if (!l.props.visible || l.props.opacity <= 0.0) {
            continue;
        }
        const bool effects = hasActiveEffects(l.props.effects);
        const TiledImage::Tile* t = (effects ? doc.effectImage(l) : l.image).tile(tx, ty);
        if (!t) {
            continue;
        }
        // With effects the mask is already part of the rendered image.
        const TiledImage::Tile* m = (!effects && l.mask && l.props.maskEnabled) ? l.mask->tile(tx, ty) : nullptr;
        const bool opaqueLayer = l.props.opacity >= 1.0;
        const BlendMode mode = l.props.blendMode;
        if (!any) {
            any = true;
            // Nothing below yet, so every blend mode reduces to a plain copy.
            if (opaqueLayer && !m) {
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
            double coverage = l.props.opacity;
            if (m) {
                const std::uint8_t mv = maskValue(m->pixels[i]);
                if (mv == 0) {
                    continue;
                }
                coverage *= mv / 255.0;
            }
            if (mode != BlendMode::Normal) {
                out.pixels[i] = blendPixel(out.pixels[i], s, mode, coverage);
            } else if (s.a == 255 && coverage >= 1.0) {
                out.pixels[i] = s;
            } else {
                out.pixels[i] = blendOver(out.pixels[i], s, coverage);
            }
        }
    }
    return any;
}

Rgba8 Document::compositePixel(Point p) const {
    Rgba8 out = Rgba8::transparent();
    for (const Layer& l : layers_) {
        if (!l.props.visible || l.props.opacity <= 0.0) {
            continue;
        }
        double coverage = l.props.opacity;
        if (hasActiveEffects(l.props.effects)) {
            out = blendPixel(out, effectImage(l).pixel(p), l.props.blendMode, coverage);
            continue;
        }
        if (l.mask && l.props.maskEnabled) {
            coverage *= maskValue(l.mask->pixel(p)) / 255.0;
        }
        out = blendPixel(out, l.image.pixel(p), l.props.blendMode, coverage);
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
        // Effects reach beyond the edited tiles (outlines, shadows, blur).
        const int reach = hasActiveEffects(l.props.effects) ? marginTiles(l.props.effects) : 0;
        for (const Point& t : l.image.takeDirtyTiles()) {
            markAround(seen, tx, tilesY(), t.x, t.y, reach);
        }
        if (l.mask) {
            for (const Point& t : l.mask->takeDirtyTiles()) {
                markAround(seen, tx, tilesY(), t.x, t.y, reach);
            }
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
