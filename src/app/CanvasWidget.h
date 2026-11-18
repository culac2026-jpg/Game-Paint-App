#pragma once

#include <gas/core/Brush.h>
#include <gas/core/Pencil.h>
#include <gas/core/Selection.h>
#include <gas/core/Viewport.h>

#include <QHash>
#include <QImage>
#include <QOpenGLWidget>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

namespace gas {
class Document;
}

enum class Tool { Pencil, Brush, Eraser, Fill, Eyedropper, RectSelect, EllipseSelect, Lasso, Move };

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
    std::optional<gas::Point> cursorPixel() const { return cursorPixel_; }

    Tool tool() const { return tool_; }
    void setTool(Tool tool);

    gas::Rgba8 foregroundColor() const { return foreground_; }
    void setForegroundColor(gas::Rgba8 color);

    // Settings of the Brush or Eraser tool (each keeps its own).
    gas::BrushSettings& brushSettings(Tool tool);

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
    gas::Interpolation transformInterpolation() const { return interpolation_; }
    void setTransformInterpolation(gas::Interpolation i) { interpolation_ = i; }

    // True while a stroke, drag or transform is in progress (undo etc. must wait).
    bool isBusy() const { return editImage_ != nullptr || dragging_ || transform_.has_value(); }

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

private:
    // Free-transform parameters: M = T(centre + offset) * R(angle) * S(sx, sy) * T(-centre).
    struct TransformState {
        gas::IRect source;
        gas::PointF offset{};
        double angle = 0.0;
        double sx = 1.0, sy = 1.0;
        enum class Drag { None, Move, Scale, Rotate } drag = Drag::None;
        int corner = -1; // 0..3 = TL, TR, BR, BL while scaling
        gas::PointF startDoc{};
        // Values when the drag began.
        gas::PointF offset0{};
        double angle0 = 0.0, sx0 = 1.0, sy0 = 1.0;
        bool oneShot = false; // Move tool: commit on release
    };

    gas::StrokeTool* strokeTool();
    gas::TiledImage* beginEdit();
    void finishStroke();
    void finishSelectionDrag(bool shift, bool alt);
    gas::Point pixelAt(QPointF widgetPos) const;
    gas::PointF docAt(QPointF widgetPos) const;
    void pickColorAt(QPointF widgetPos);
    void cacheTile(int tx, int ty);
    void syncDirtyTiles();
    void syncDocumentSize();
    void drawBrushOutline(QPainter& p) const;
    void drawSelection(QPainter& p);
    void drawSelectionPreview(QPainter& p) const;
    void drawTransformBox(QPainter& p) const;
    void zoomAroundCenter(double factor);
    void updateCursorShape();

    gas::Affine transformMatrix() const;
    std::vector<QPointF> transformCorners() const; // screen space, TL TR BR BL
    void applyTransform();
    void transformPress(QPointF pos, Qt::KeyboardModifiers mods);
    void transformDrag(QPointF pos, Qt::KeyboardModifiers mods);

    gas::Document* document_ = nullptr;
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
    int fillTolerance_ = 0;
    bool picking_ = false;

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

    // Transform
    std::optional<TransformState> transform_;
    gas::Interpolation interpolation_ = gas::Interpolation::Nearest;

    bool spaceHeld_ = false;
    bool panning_ = false;
    QPointF lastPanPos_;
    std::optional<QPointF> cursorPos_;
    std::optional<gas::Point> cursorPixel_;
};
