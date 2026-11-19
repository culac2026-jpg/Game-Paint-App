#pragma once

#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/Symmetry.h>
#include <gas/core/Pencil.h>
#include <gas/core/Selection.h>
#include <gas/core/StrokeSmoother.h>
#include <gas/core/Viewport.h>

#include <QHash>
#include <QImage>
#include <QOpenGLWidget>
#include <QTimer>

#include <memory>
#include <array>
#include <optional>
#include <vector>

namespace gas {
class Document;
}

class VectorEditor;

enum class Tool {
    Pencil, Brush, Eraser, Fill, Eyedropper, RectSelect, EllipseSelect, Lasso, Move, Smudge, Blur, Slice,
    Pen, Shape, Node
};

// Tools that edit vector layers (see VectorEditor).
inline bool isVectorTool(Tool t) { return t == Tool::Pen || t == Tool::Shape || t == Tool::Node; }

// Tools driven by a gas::BrushTool (size/opacity/hardness, tip, dynamics).
inline bool usesBrushSettings(Tool t) {
    return t == Tool::Brush || t == Tool::Eraser || t == Tool::Smudge || t == Tool::Blur;
}

// Viewport onto a gas::Document: checkerboard, nearest-neighbour tile
// rendering, wheel zoom around the cursor, middle-drag / Space+drag panning,
// selection outline ("marching ants"), the free-transform box, and dispatch of
// the left button to the active tool.
class CanvasWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);

    // Non-owning; pass nullptr to detach.
    void setDocument(gas::Document* document);

    double zoom() const { return viewport_.zoom(); }
    QPointF docToScreenPoint(gas::PointF doc) const;
    gas::PointF screenToDocPoint(QPointF screen) const { return docAt(screen); }
    VectorEditor* vectorEditor() const { return vector_; }
    std::optional<gas::Point> cursorPixel() const { return cursorPixel_; }

    Tool tool() const { return tool_; }
    void setTool(Tool tool);

    gas::Rgba8 foregroundColor() const { return foreground_; }
    void setForegroundColor(gas::Rgba8 color);

    // Settings of the Brush or Eraser tool (each keeps its own).
    gas::BrushSettings& brushSettings(Tool tool);

    // Mirror painting for the stroke tools. The centre starts in the middle of
    // the canvas and can be dragged by its handle (snaps to half pixels).
    const gas::Symmetry& symmetry() const { return symmetry_; }
    void setSymmetryMode(gas::SymmetryMode mode);
    void setSymmetrySegments(int segments);
    void resetSymmetryCenter();

    // Straight lines for the stroke tools: in line mode (or with Ctrl held at
    // the press) a drag previews a line and paints it on release; Shift snaps
    // it to 15-degree steps. Shift+click joins the last stroke's end to the click.
    bool lineMode() const { return lineMode_; }
    void setLineMode(bool on);

    // StreamLine (0..1) for the stroke tools: the stroke trails the pointer
    // while drawing and is redrawn along a fully smoothed path on release.
    double streamline() const { return streamline_; }
    void setStreamline(double strength) { streamline_ = std::clamp(strength, 0.0, 1.0); }

    // --- Slices (sprite regions) ---
    // Slice tool: drag on empty canvas to add a slice, drag a slice to move it,
    // its handles to resize, its pivot marker to move the pivot (Shift snaps to
    // edges/centre); arrows nudge, Delete removes (deleteSelectedSlice).
    int selectedSlice() const { return selectedSlice_; }
    void setSelectedSlice(int index); // -1 = none
    bool deleteSelectedSlice();
    bool showSlices() const { return showSlices_; }
    void setShowSlices(bool on);

    // Onion skin: up to `before` earlier frames tinted red and `after` later
    // frames tinted blue, fading with distance, under the current frame.
    void setOnionSkin(bool on, int before, int after);
    bool onionSkin() const { return onion_; }

    int fillTolerance() const { return fillTolerance_; }
    void setFillTolerance(int tolerance) { fillTolerance_ = tolerance; }

    // Selection tools: how a new shape combines with the current selection
    // (Shift = add, Alt = subtract, Shift+Alt = intersect override it per drag).
    gas::Selection::Op selectionOp() const { return selectionOp_; }
    void setSelectionOp(gas::Selection::Op op) { selectionOp_ = op; }
    bool antialiasSelection() const { return antialiasSelection_; }
    void setAntialiasSelection(bool on) { antialiasSelection_ = on; }

    // --- Free transform (Ctrl+T) ---
    bool isTransforming() const { return transform_.has_value(); }
    bool beginFreeTransform(); // false (with editRefused) if nothing to transform
    void commitTransform();
    void cancelTransform();
    // Numeric access for the options bar (angle in degrees, anchor in canvas px).
    double transformAngle() const;
    double transformScaleX() const;
    double transformScaleY() const;
    gas::PointF transformPivot() const; // where the anchor is now (canvas space)
    void setTransformAngle(double degrees);
    void setTransformScale(double sx, double sy);
    void setTransformPivot(gas::PointF doc); // moves the anchor only, never the pixels
    void centerTransformPivot();
    // Steps back/forward through the changes made since the transform began;
    // false when there is nothing to undo/redo.
    bool undoTransformStep();
    bool redoTransformStep();
    gas::Interpolation transformInterpolation() const { return interpolation_; }
    void setTransformInterpolation(gas::Interpolation i) { interpolation_ = i; }

    // True while a stroke, drag or transform is in progress (undo etc. must wait).
    bool isBusy() const {
        return editImage_ != nullptr || dragging_ || transform_.has_value() || sliceDrag_.has_value() ||
               vectorBusy();
    }

    void zoomIn();
    void zoomOut();
    void zoomActual();
    void zoomToFit();

Q_SIGNALS:
    void viewChanged();
    void cursorMoved();
    void foregroundColorChanged(gas::Rgba8 color);
    // The document changed through the canvas (stroke, selection, transform...).
    void documentEdited();
    // A tool could not run (e.g. the layer is locked); `reason` is user-facing.
    void editRefused(const QString& reason);
    void transformModeChanged(bool active);
    void transformChanged(); // any transform parameter, including the anchor
    void selectedSliceChanged(int index);

protected:
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;

private:
    // Free-transform parameters: M = T(pivot + offset) * R(angle) * S(sx, sy) * T(-pivot).
    // The pivot (anchor) is a source-space point; it shows at pivot + offset and
    // rotation/scaling happen around it.
    struct TransformState {
        gas::RectF source; // the box before M (the layer frame's box, or the lifted bounds)
        // Pixels get M * restInverse: the layer frame's own matrix is undone first,
        // so the box can start rotated/scaled where it was left.
        gas::Affine restInverse{};
        bool wholeLayer = false; // the layer frame applies (and is stored on commit)
        bool hadFrame = false;
        gas::PointF pivot{};
        gas::PointF offset{};
        double angle = 0.0;
        double sx = 1.0, sy = 1.0;
        enum class Drag { None, Move, Scale, Rotate, Pivot } drag = Drag::None;
        int handle = -1; // while scaling: 0..3 = corners TL TR BR BL, 4..7 = edges T R B L
        gas::PointF startDoc{};
        // Values when the drag began.
        gas::PointF offset0{};
        double angle0 = 0.0, sx0 = 1.0, sy0 = 1.0;
        bool oneShot = false; // Move tool: commit on release
    };
    // In-transform undo/redo (Ctrl+Z/Y while the box is open): one entry per
    // drag, field edit or nudge.
    struct TransformParams {
        gas::PointF pivot, offset;
        double angle, sx, sy;
        friend bool operator==(const TransformParams&, const TransformParams&) = default;
    };
    TransformParams transformParams() const;
    void restoreTransformParams(const TransformParams& p);
    void pushTransformUndo();
    void movePivotTo(gas::PointF doc); // no undo entry (used while dragging)

    bool vectorBusy() const;
    gas::StrokeTool* strokeTool();
    const gas::BrushTool* brushTool() const;
    gas::TiledImage* beginEdit();
    void finishStroke();
    void finishSelectionDrag(bool shift, bool alt);
    gas::Point pixelAt(QPointF widgetPos) const;
    gas::PointF docAt(QPointF widgetPos) const;
    // Pen pressure for a mouse event synthesized from a tablet, else 1.
    double pressureOf(const QMouseEvent* event) const;
    void pickColorAt(QPointF widgetPos);
    void cacheTile(int tx, int ty);
    void syncDirtyTiles();
    void syncDocumentSize();
    void drawBrushOutline(QPainter& p) const;
    void drawSymmetryGuides(QPainter& p) const;
    void drawLinePreview(QPainter& p) const;
    void updateLineEnd(Qt::KeyboardModifiers mods);
    void commitLine();
    bool nearSymmetryHandle(QPointF widgetPos) const;
    void drawSelection(QPainter& p);
    void drawSelectionPreview(QPainter& p) const;
    void drawTransformBox(QPainter& p) const;
    void zoomAroundCenter(double factor);

    // Slice tool
    void slicePress(QPointF pos, Qt::KeyboardModifiers mods);
    int sliceBorderAt(QPointF pos, bool alt) const; // 9-slice guide of the selected slice: 0..3 = L, T, R, B
    void sliceDragTo(QPointF pos, Qt::KeyboardModifiers mods);
    void finishSliceDrag();
    void nudgeSlice(int dx, int dy);
    void drawSlices(QPainter& p) const;
    void drawOnionSkin(QPainter& p, const QRectF& canvasRect, const QRectF& visible);
    const QImage& onionImage(int frame, bool earlier);
    QRectF sliceScreenRect(const gas::IRect& r) const;
    QPointF slicePivotScreen(const gas::Slice& s) const;
    int sliceAt(QPointF pos) const;       // topmost slice under the point (selected first), or -1
    int sliceHandleAt(QPointF pos) const; // resize handle of the selected slice, or -1
    bool nearSlicePivot(QPointF pos) const;
    void updateCursorShape();

    gas::Affine transformMatrix() const;
    std::vector<QPointF> transformCorners() const; // screen space: corners TL TR BR BL, then edges T R B L
    std::array<gas::PointF, 8> transformHandlePoints() const; // the same in source space
    int transformHandleAt(QPointF pos) const;                 // 0..7, or -1
    bool nearTransformPivot(QPointF pos) const;
    void applyTransform();
    void transformPress(QPointF pos, Qt::KeyboardModifiers mods);
    void transformDrag(QPointF pos, Qt::KeyboardModifiers mods);

    gas::Document* document_ = nullptr;
    VectorEditor* vector_ = nullptr;
    gas::TiledImage* editImage_ = nullptr; // active layer during a stroke
    gas::Viewport viewport_;
    QHash<quint64, QImage> tileCache_;
    std::unique_ptr<gas::TiledImage::Tile> scratchTile_;
    bool fitPending_ = true;
    int docWidth_ = 0, docHeight_ = 0; // size the cache was built for

    Tool tool_ = Tool::Brush;
    gas::Rgba8 foreground_ = gas::Rgba8::black();
    gas::PencilTool pencil_;
    gas::BrushTool brush_{gas::BrushMode::Paint};
    gas::BrushTool eraser_{gas::BrushMode::Erase};
    gas::BrushTool smudge_{gas::BrushMode::Smudge};
    gas::BrushTool blur_{gas::BrushMode::Blur};
    int fillTolerance_ = 0;
    bool picking_ = false;
    gas::Symmetry symmetry_;
    QSize symmetryDocSize_;        // canvas size the centre was set for (recentred when it changes)
    bool draggingSymmetry_ = false;
    bool lineMode_ = false;
    double streamline_ = 0.0;
    std::vector<gas::StrokeSample> rawStroke_; // pointer samples of the current stroke (for StreamLine)
    struct LineDrag {
        gas::PointF start, raw, end; // raw = pointer, end = after snapping
    };
    std::optional<LineDrag> lineDrag_;
    std::optional<gas::PointF> lastStrokeEnd_; // for Shift+click
    gas::StrokeSmoother smoother_;
    double tabletPressure_ = 1.0; // from the latest tablet event

    // Selection tools
    gas::Selection::Op selectionOp_ = gas::Selection::Op::Replace;
    bool antialiasSelection_ = true;
    bool dragging_ = false;
    Qt::KeyboardModifiers dragMods_; // modifiers when the selection drag began
    std::vector<gas::PointF> dragPoints_; // rect/ellipse: [start, current]; lasso: the path
    unsigned outlineGeneration_ = ~0u;
    std::vector<gas::Selection::Segment> outline_;
    QTimer antsTimer_;
    int antsOffset_ = 0;

    // Slices
    struct SliceDrag {
        enum class Mode { Create, Move, Resize, Pivot, Border } mode = Mode::Create;
        int index = -1;  // slice being edited (-1 while creating)
        int handle = -1; // Resize: 0..7 = TL, T, TR, R, BR, B, BL, L; Border: 0..3 = L, T, R, B
        gas::PointF startDoc;
        gas::Slice start;
        gas::Slice current; // preview, committed on release
    };
    std::optional<SliceDrag> sliceDrag_;
    int selectedSlice_ = -1;
    bool showSlices_ = true;

    // Onion skin (cached tinted composites of other frames, keyed by frame*2 + earlier)
    bool onion_ = false;
    int onionBefore_ = 1, onionAfter_ = 1;
    QHash<int, QImage> onionCache_;
    unsigned onionGeneration_ = ~0u;

    // Transform
    std::optional<TransformState> transform_;
    std::vector<TransformParams> transformUndo_, transformRedo_;
    gas::Interpolation interpolation_ = gas::Interpolation::Nearest;

    bool spaceHeld_ = false;
    bool panning_ = false;
    QPointF lastPanPos_;
    std::optional<QPointF> cursorPos_;
    std::optional<gas::Point> cursorPixel_;
};
