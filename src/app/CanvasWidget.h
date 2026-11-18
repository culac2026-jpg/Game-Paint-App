#pragma once

#include <gas/core/Pencil.h>
#include <gas/core/Viewport.h>

#include <QHash>
#include <QImage>
#include <QOpenGLWidget>

#include <optional>

namespace gas {
class Document;
}

// Viewport onto a gas::Document: checkerboard, nearest-neighbour tile
// rendering, wheel zoom around the cursor, middle-drag / Space+drag panning,
// and the pencil on the left button.
class CanvasWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);

    // Non-owning; pass nullptr to detach.
    void setDocument(gas::Document* document);

    double zoom() const { return viewport_.zoom(); }
    std::optional<gas::Point> cursorPixel() const { return cursorPixel_; }

    gas::Rgba8 pencilColor() const { return pencil_.color(); }
    void setPencilColor(gas::Rgba8 color) { pencil_.setColor(color); }

    void zoomIn();
    void zoomOut();
    void zoomActual();
    void zoomToFit();

Q_SIGNALS:
    void viewChanged();
    void cursorMoved();

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
    gas::Point pixelAt(QPointF widgetPos) const;
    void cacheTile(int tx, int ty);
    void syncDirtyTiles();
    void zoomAroundCenter(double factor);
    void updateCursorShape();

    gas::Document* document_ = nullptr;
    gas::Viewport viewport_;
    gas::PencilTool pencil_;
    QHash<quint64, QImage> tileCache_;
    bool fitPending_ = true;

    bool spaceHeld_ = false;
    bool panning_ = false;
    QPointF lastPanPos_;
    std::optional<gas::Point> cursorPixel_;
};
