#pragma once

#include "gas/core/Blend.h"
#include "gas/core/Effects.h"
#include "gas/core/Selection.h"
#include "gas/core/TiledImage.h"
#include "gas/core/Transform.h"
#include "gas/core/Vector.h"

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
    double opacity = 1.0;    // 0..1
    bool maskEnabled = true; // ignored when the layer has no mask
    BlendMode blendMode = BlendMode::Normal;
    std::vector<Effect> effects; // non-destructive effect stack (see Effects.h)

    friend bool operator==(const LayerProps&, const LayerProps&) = default;
};

// 9-slice insets in pixels from each edge of a slice (all 0 = not 9-sliced).
struct SliceBorder {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool empty() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }
    friend bool operator==(const SliceBorder&, const SliceBorder&) = default;
};

// A named sprite region for slicing/atlas export. The pivot is relative to
// the rect (0,0 = top-left, 1,1 = bottom-right; may lie outside it).
struct Slice {
    std::string name;
    IRect rect;
    PointF pivot{0.5, 0.5};
    SliceBorder border; // 9-slice: corners keep their size, edges and centre stretch

    friend bool operator==(const Slice&, const Slice&) = default;
};

// One animation frame; layers hold one cel (image) per frame.
struct Frame {
    int duration = 100; // ms

    friend bool operator==(const Frame&, const Frame&) = default;
};

// Playback direction of a tag. Stored in files: append only.
enum class TagDirection { Forward, Reverse, PingPong };
inline constexpr int kTagDirectionCount = 3;

// A named frame range (e.g. "walk" = frames 0..7), inclusive.
struct FrameTag {
    std::string name;
    int from = 0;
    int to = 0;
    TagDirection direction = TagDirection::Forward;

    friend bool operator==(const FrameTag&, const FrameTag&) = default;
};

struct EffectCache;

struct Layer {
    Layer(LayerProps p, TiledImage img, std::optional<TiledImage> m = std::nullopt)
        : props(std::move(p)), image(std::move(img)), mask(std::move(m)) {}

    LayerProps props;
    TiledImage image;
    // Non-destructive layer mask, painted like any image; see maskValue()
    // (unpainted = shown, black = hidden).
    std::optional<TiledImage> mask;
    // One image per animation frame. `image` is the active frame's cel (what
    // tools and rendering use); the document keeps cels[activeFrame] in sync
    // with it after every operation. Masks and effects belong to the layer.
    std::vector<TiledImage> cels;
    // Vector layer: the shapes, from which `image` is always rendered (raster
    // tools refuse to paint it; its mask and effects work as usual). Shapes
    // belong to the layer, so every frame shows the same picture. Null = raster.
    std::shared_ptr<const VectorShapes> vector;
    // Rendered "pixels + mask + effects", rebuilt lazily by the document. It
    // validates itself against the content, so copies may share it safely.
    mutable std::shared_ptr<EffectCache> effectCache;
};

// A layered raster document with linear undo/redo.
//
// Every change goes through the document so it can be undone: structure and
// property edits via the layer methods, pixel edits by bracketing tool work
// with beginEdit()/endEdit(). Undo steps hold copy-on-write tile pointers, so
// a step costs only the tiles it actually changed. History is capped by an
// estimate of the memory it holds; the oldest steps are dropped first.
//
// While a selection exists, pixel edits cannot change anything outside it:
// tools draw into a scratch copy, and syncEdit()/endEdit() write
// mix(original, scratch, coverage) into the real layer.
class Document {
public:
    static constexpr int kMaxSize = 16384;
    static constexpr int kMaxLayers = 1000;
    static constexpr std::size_t kDefaultHistoryLimit = std::size_t{1} << 30; // 1 GiB

    // A document with one empty layer named "Layer 1".
    Document(int width, int height);
    // From existing layers (e.g. a loaded file); throws std::invalid_argument
    // if empty, too many, or any layer's (or mask's) size differs from the document.
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
    // New layer above the active one holding `image` (e.g. an imported picture);
    // false if its size differs from the document's.
    bool addLayer(const std::string& name, TiledImage image, const std::string& label = "Import Layer");
    bool duplicateLayer(int index);   // copy placed above the original
    bool removeLayer(int index);      // the last remaining layer can't be removed
    bool moveLayer(int from, int to); // to = final index of the moved layer
    // Consecutive edits to the same layer's properties merge into one step.
    bool setLayerProps(int index, const LayerProps& props);

    // --- Layer masks ---
    // Adds a mask showing everything, or only the selection if one exists,
    // and starts editing it.
    bool addLayerMask(int index);
    bool removeLayerMask(int index);
    // Bakes the mask into the layer's alpha and removes it.
    bool applyLayerMask(int index);
    // Bakes the layer's effects (and mask) into its pixels and clears them.
    bool rasterizeLayerEffects(int index);
    // Whether pixel edits target the active layer's mask (a view choice).
    bool editingMask() const;
    void setEditingMask(bool editMask) { editingMask_ = editMask; }

    // --- Vector layers ---
    bool isVectorLayer(int index) const;
    // New vector layer above the active one (becomes active).
    bool addVectorLayer(VectorShapes shapes = {}, const std::string& label = "New Vector Layer");
    // Replaces the shapes of vector layer `index` and re-renders it. Calls with
    // the same non-zero `mergeToken` (and layer) fold into one step, e.g. a drag.
    bool setVectorShapes(int index, VectorShapes shapes, const std::string& label = "Edit Shapes", int mergeToken = 0);
    // Keeps the rendered pixels and drops the shapes (a normal raster layer).
    bool rasterizeVectorLayer(int index);

    // --- Selection (each change is an undo step) ---
    const Selection& selection() const { return selection_; }
    bool setSelection(Selection selection, const std::string& label = "Select");
    bool selectAll();
    bool deselect();
    bool invertSelection();
    // Increments whenever the selection changes (incl. undo/redo), for view caches.
    unsigned selectionGeneration() const { return selectionGeneration_; }

    // --- Pixel edits on the active layer (or its mask) ---
    // Returns the image to draw into, or nullptr if the active layer is locked,
    // hidden or a vector layer (unless its mask is being edited), or another
    // edit/transform is in progress. Must be balanced by endEdit().
    TiledImage* beginEdit();
    // Pushes the tool's latest changes through the selection clip into the
    // layer, so the canvas can show them mid-stroke. Cheap when nothing changed.
    void syncEdit();
    // Records one undo step named `label` if any pixel changed; returns whether it did.
    bool endEdit(const std::string& label);
    // Puts the edited image back as it was at beginEdit(); the edit stays open
    // (used to redraw a stroke, e.g. after StreamLine smoothing).
    void revertEdit();
    bool isEditing() const { return edit_.has_value(); }

    // Clears / fills the selected pixels (the whole layer if nothing is selected).
    bool eraseSelected();
    bool fillSelected(Rgba8 color);

    // --- Transform of the selected pixels (or the whole layer) ---
    // Lifts the content out of the active layer into a floating image; the
    // preview updates with setTransform() until commit or cancel. On a vector
    // layer the whole content is lifted (the selection is ignored) and the
    // commit transforms the shapes instead of the pixels.
    bool beginTransform();
    bool isTransforming() const { return transform_.has_value(); }
    IRect transformSourceRect() const; // where the floating content started
    Affine currentTransform() const;
    void setTransform(const Affine& m);
    // One undo step (pixels + selection). An identity transform cancels instead.
    bool commitTransform(Interpolation interpolation, const std::string& label = "Transform");
    void cancelTransform();
    // One-shot helpers (begin, set, commit); exact for pixel art.
    bool flipContent(bool horizontal);
    bool rotateContent90(bool clockwise);

    // --- Animation frames (each an undo step unless noted) ---
    static constexpr int kMaxFrames = 10000;
    static constexpr int kMinFrameDuration = 1;
    static constexpr int kMaxFrameDuration = 60000;
    int frameCount() const { return static_cast<int>(frames_.size()); }
    int activeFrame() const { return activeFrame_; }
    const std::vector<Frame>& frames() const { return frames_; }
    // Switches the frame being shown and edited (a view choice, not an undo
    // step); false while an edit or transform is open.
    bool setActiveFrame(int index);
    bool addFrame();                // empty frame after the active one; becomes active
    bool duplicateFrame(int index); // copy (sharing tiles) right after it; becomes active
    bool removeFrame(int index);    // the last remaining frame can't be removed
    bool moveFrame(int from, int to);
    bool setFrameDuration(int index, int ms);
    bool setFrames(std::vector<Frame> frames, const std::string& label = "Frame Durations"); // same count
    const std::vector<FrameTag>& tags() const { return tags_; }
    // Replaces all tags (ranges clamped to the frames, empty names dropped).
    bool setTags(std::vector<FrameTag> tags, const std::string& label = "Edit Tags");
    // Layer `layer`'s cel in frame `frame` (the live image for the active frame).
    const TiledImage& cel(int layer, int frame) const;
    bool celEmpty(int layer, int frame) const;
    // Composite of the visible layers in another frame (onion skin, export).
    TiledImage flattenedFrame(int frame) const;
    // For loaders: frames, cels ([layer][frame], each the document's size) and
    // tags without an undo step; throws std::invalid_argument on a mismatch.
    void restoreAnimation(std::vector<Frame> frames, std::vector<std::vector<TiledImage>> cels,
                          std::vector<FrameTag> tags, int activeFrame);
    // Increments on every undoable change, undo and redo (for view caches of other frames).
    unsigned historyGeneration() const { return historyGeneration_; }
    // Increments when frames, tags or the active frame change.
    unsigned framesGeneration() const { return framesGeneration_; }

    // --- Slices (sprite regions; saved with the project) ---
    const std::vector<Slice>& slices() const { return slices_; }
    // Replaces all slices as one undo step. Rects are clipped to the canvas
    // (empty ones dropped), pivots clamped to [-4, 5]. With `merge`, consecutive
    // calls with the same label fold into one step (e.g. dragging a pivot).
    bool setSlices(std::vector<Slice> slices, const std::string& label = "Edit Slices", bool merge = false);
    // For loaders: sets the slices without an undo step.
    void restoreSlices(std::vector<Slice> slices);
    // Increments whenever the slices change (incl. undo/redo), for views.
    unsigned slicesGeneration() const { return slicesGeneration_; }
    // The composite of all visible layers as a sparse tiled image.
    TiledImage flattenedImage() const;

    // --- Canvas ---
    // Crops every layer (and mask) to `area`; deselects; slices move with the
    // content (clipped, empty ones dropped). One undo step.
    bool crop(IRect area);

    // --- History ---
    bool canUndo() const { return !undo_.empty() && !busy(); }
    bool canRedo() const { return !redo_.empty() && !busy(); }
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
    // Marks the document as differing from any file (e.g. recovered from an autosave).
    void markUnsaved() { cleanIndex_ = -1; }

    // --- Rendering ---
    // Composites visible layers into `out`; false (out untouched) if the tile is empty.
    bool compositeTile(int tx, int ty, TiledImage::Tile& out) const;
    Rgba8 compositePixel(Point p) const;
    // Row-major width*height composite of all visible layers.
    std::vector<Rgba8> flatten() const;
    // Tiles whose composite changed since the last call.
    std::vector<Point> takeDirtyTiles();
    void markAllDirty() { structureDirty_ = true; }
    // What layer `index` contributes before opacity/blend: its pixels, or
    // pixels + mask + effects when it has active effects.
    const TiledImage& renderedLayer(int index) const;

private:
    struct VectorState;
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
        // Mergeable slice edits: the shared "after" value.
        std::shared_ptr<std::vector<Slice>> mergeSlices;
        // Frame the step was made in; undo/redo switch to it first, so a step
        // always lands on the cels it was recorded against. Steps that restore
        // the whole animation state (frame edits) keep -1.
        int frame = -1;
        bool animationState = false;
        // Mergeable vector edits: the shared "after" state, its layer and token.
        std::shared_ptr<VectorState> mergeVector;
        int mergeVectorLayer = -1;
        int mergeToken = 0;
    };
    struct VectorState {
        std::shared_ptr<const VectorShapes> shapes;
        std::vector<TiledImage::TilePtr> tiles;
    };
    struct TileChange {
        int tx, ty;
        TiledImage::TilePtr before, after;
    };
    struct EditSession {
        int layer;
        bool mask;
        std::vector<TiledImage::TilePtr> before;
        std::optional<TiledImage> scratch; // only while a selection clips the edit
    };
    struct TransformSession {
        int layer;
        std::vector<TiledImage::TilePtr> before; // layer before lifting
        std::vector<TiledImage::TilePtr> hole;   // layer with the content lifted out
        FloatingImage floating;
        Selection selectionBefore;
        Affine m;
        IRect previewArea; // pixels the current preview wrote over the hole
    };

    bool busy() const { return edit_.has_value() || transform_.has_value(); }
    struct AnimationState {
        std::vector<Layer> layers;
        std::vector<Frame> frames;
        std::vector<FrameTag> tags;
        int activeFrame;
    };
    // cels[activeFrame_] = image for every layer.
    void syncCels();
    // Shows frame `index` (no undo step).
    void switchFrame(int index);
    // Gives a layer one cel per frame with `image` as the active one (every
    // cel, for a vector layer).
    void normalizeLayer(Layer& layer) const;
    static void fillVectorCels(Layer& layer);
    AnimationState animationState();
    // Replaces layers/frames/tags/active frame as one undo step.
    void executeAnimation(const std::string& label, AnimationState after);
    std::vector<FrameTag> sanitizedTags(std::vector<FrameTag> tags, int frameCount) const;
    static bool compositeLayersTile(const Document& doc, const std::vector<Layer>& layers, int tx, int ty,
                                    TiledImage::Tile& out);
    TiledImage& targetImage(int layer, bool mask);
    // Applies `cmd` (runs redo) and records it.
    void execute(Command cmd);
    void enforceHistoryLimit();
    std::string nextLayerName() const;
    std::vector<Slice> sanitizedSlices(std::vector<Slice> slices) const;
    // Tiles of `image` that differ in content from `before`.
    static std::vector<TileChange> diffTiles(const std::vector<TiledImage::TilePtr>& before,
                                             const TiledImage& image, std::size_t& bytes);
    Command tileCommand(const std::string& label, int layer, bool mask, std::vector<TileChange> changes,
                        std::size_t bytes);
    // Replaces layer `index` wholesale as one undo step.
    void replaceLayer(int index, Layer after, const std::string& label);
    void restoreTiles(TiledImage& image, const std::vector<TiledImage::TilePtr>& from, IRect area);
    const TiledImage& effectImage(const Layer& layer) const;

    int width_;
    int height_;
    std::vector<Layer> layers_;
    int activeLayer_ = 0;
    bool structureDirty_ = true;
    bool editingMask_ = false;
    Selection selection_;
    unsigned selectionGeneration_ = 0;
    std::vector<Slice> slices_;
    unsigned slicesGeneration_ = 0;
    std::vector<Frame> frames_{Frame{}};
    int activeFrame_ = 0;
    std::vector<FrameTag> tags_;
    unsigned historyGeneration_ = 0;
    unsigned framesGeneration_ = 0;

    std::optional<EditSession> edit_;
    std::optional<TransformSession> transform_;
    std::deque<Command> undo_;
    std::vector<Command> redo_;
    std::size_t historyLimit_ = kDefaultHistoryLimit;
    long long cleanIndex_ = 0; // undo_.size() at last save; -1 = unreachable
    int createdLayers_ = 0;    // for default names
};

} // namespace gas
