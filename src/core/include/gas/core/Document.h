#pragma once

#include "gas/core/TiledImage.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gas {

struct LayerProps {
    std::string name;
    bool visible = true;
    bool locked = false;
    double opacity = 1.0; // 0..1

    friend bool operator==(const LayerProps&, const LayerProps&) = default;
};

struct Layer {
    LayerProps props;
    TiledImage image;
};

// A layered raster document with linear undo/redo.
//
// Every change goes through the document so it can be undone: structure and
// property edits via the layer methods, pixel edits by bracketing tool work
// with beginEdit()/endEdit(). Undo steps hold copy-on-write tile pointers, so
// a step costs only the tiles it actually changed. History is capped by an
// estimate of the memory it holds; the oldest steps are dropped first.
class Document {
public:
    static constexpr int kMaxSize = 16384;
    static constexpr int kMaxLayers = 1000;
    static constexpr std::size_t kDefaultHistoryLimit = std::size_t{1} << 30; // 1 GiB

    // A document with one empty layer named "Layer 1".
    Document(int width, int height);
    // From existing layers (e.g. a loaded file); throws std::invalid_argument
    // if empty, too many, or any layer's size differs from the document.
    Document(int width, int height, std::vector<Layer> layers, int activeLayer);
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    int tilesX() const { return layers_.front().image.tilesX(); }
    int tilesY() const { return layers_.front().image.tilesY(); }

    // Layers are ordered bottom (index 0) to top.
    int layerCount() const { return static_cast<int>(layers_.size()); }
    const Layer& layer(int index) const { return layers_.at(static_cast<std::size_t>(index)); }
    int activeLayerIndex() const { return activeLayer_; }
    const Layer& activeLayer() const { return layer(activeLayer_); }
    void setActiveLayer(int index); // a view choice, not an undo step

    // --- Undoable layer edits (each is one undo step; invalid calls return false) ---
    void addLayer();                  // new empty layer above the active one
    bool duplicateLayer(int index);   // copy placed above the original
    bool removeLayer(int index);      // the last remaining layer can't be removed
    bool moveLayer(int from, int to); // to = final index of the moved layer
    // Consecutive edits to the same layer's properties merge into one step.
    bool setLayerProps(int index, const LayerProps& props);

    // --- Pixel edits on the active layer ---
    // Returns the image to draw into, or nullptr if the active layer is locked
    // or hidden. Must be balanced by endEdit().
    TiledImage* beginEdit();
    // Records one undo step named `label` if any pixel changed; returns whether it did.
    bool endEdit(const std::string& label);
    bool isEditing() const { return edit_.has_value(); }

    // --- History ---
    bool canUndo() const { return !undo_.empty() && !isEditing(); }
    bool canRedo() const { return !redo_.empty() && !isEditing(); }
    std::string undoLabel() const;
    std::string redoLabel() const;
    bool undo();
    bool redo();
    int undoSteps() const { return static_cast<int>(undo_.size()); }
    int redoSteps() const { return static_cast<int>(redo_.size()); }
    std::size_t historyMemory() const;
    void setHistoryMemoryLimit(std::size_t bytes);

    // Modified = the current state differs from the last markSaved() (or creation).
    bool isModified() const;
    void markSaved();

    // --- Rendering ---
    // Composites visible layers into `out`; false (out untouched) if the tile is empty.
    bool compositeTile(int tx, int ty, TiledImage::Tile& out) const;
    Rgba8 compositePixel(Point p) const;
    // Row-major width*height composite of all visible layers.
    std::vector<Rgba8> flatten() const;
    // Tiles whose composite changed since the last call.
    std::vector<Point> takeDirtyTiles();
    void markAllDirty() { structureDirty_ = true; }

private:
    struct Command {
        Command(std::string l, std::function<void()> u, std::function<void()> r)
            : label(std::move(l)), undo(std::move(u)), redo(std::move(r)) {}

        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
        std::size_t bytes = 0;
        // Property edits: which layer, and the shared "after" value merging updates.
        int mergeLayer = -1;
        std::shared_ptr<LayerProps> mergeAfter;
    };
    struct EditSession {
        int layer;
        std::vector<TiledImage::TilePtr> before;
    };

    // Applies `cmd` (runs redo) and records it.
    void execute(Command cmd);
    void enforceHistoryLimit();
    std::string nextLayerName() const;

    int width_;
    int height_;
    std::vector<Layer> layers_;
    int activeLayer_ = 0;
    bool structureDirty_ = true;

    std::optional<EditSession> edit_;
    std::deque<Command> undo_;
    std::vector<Command> redo_;
    std::size_t historyLimit_ = kDefaultHistoryLimit;
    long long cleanIndex_ = 0; // undo_.size() at last save; -1 = unreachable
    int createdLayers_ = 0;    // for default names
};

} // namespace gas
