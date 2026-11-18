#pragma once

#include <gas/core/Brush.h>
#include <gas/core/Pencil.h>
#include <gas/core/Viewport.h>

#include <QHash>
#include <QImage>
#include <QOpenGLWidget>

#include <memory>
#include <optional>

namespace gas {
class Document;
}

enum class Tool { Pencil, Brush, Eraser, Fill, Eyedropper };

// Viewport onto a gas::Document: checkerboard, nearest-neighbour tile
// rendering, wheel zoom around the cursor, middle-drag / Space+drag panning,
// and dispatch of the left button to the active tool.
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

    // True while a stroke is in progress (undo etc. must wait).
    bool isBusy() const { return editImage_ != nullptr; }

    void zoomIn();
    void zoomOut();
    void zoomActual();
    void zoomToFit();

Q_SIGNALS:
    void viewChanged();
    void cursorMoved();
    void foregroundColorChanged(gas::Rgba8 color);
    // A tool finished an undoable edit of the document.
    void documentEdited();
    // A tool could not run (e.g. the layer is locked); `reason` is user-facing.
    void editRefused(const QString& reason);

protected:
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    gas::StrokeTool* strokeTool();
    gas::TiledImage* beginEdit();
    void finishStroke();
    gas::Point pixelAt(QPointF widgetPos) const;
    gas::PointF docAt(QPointF widgetPos) const;
    void pickColorAt(QPointF widgetPos);
    void cacheTile(int tx, int ty);
    void syncDirtyTiles();
    void drawBrushOutline(QPainter& p) const;
    void zoomAroundCenter(double factor);
    void updateCursorShape();

    gas::Document* document_ = nullptr;
    gas::TiledImage* editImage_ = nullptr; // active layer during a stroke
    gas::Viewport viewport_;
    QHash<quint64, QImage> tileCache_;
    std::unique_ptr<gas::TiledImage::Tile> scratchTile_;
    bool fitPending_ = true;

    Tool tool_ = Tool::Brush;
    gas::Rgba8 foreground_ = gas::Rgba8::black();
    gas::PencilTool pencil_;
    gas::BrushTool brush_{gas::BrushMode::Paint};
    gas::BrushTool eraser_{gas::BrushMode::Erase};
    int fillTolerance_ = 0;
    bool picking_ = false;

    bool spaceHeld_ = false;
    bool panning_ = false;
    QPointF lastPanPos_;
    std::optional<QPointF> cursorPos_;
    std::optional<gas::Point> cursorPixel_;
};
