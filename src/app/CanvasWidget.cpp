#include "CanvasWidget.h"

#include <gas/core/Document.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kTile = gas::TiledImage::kTileSize;
constexpr int kCheckerSize = 8; // screen pixels, independent of zoom
constexpr double kButtonZoomStep = 2.0;
constexpr double kWheelZoomBase = 1.0015; // ~1.2x per 120-unit wheel notch

const QColor kBackground(0x2b, 0x2b, 0x2b);
const QColor kCheckerLight(0xcc, 0xcc, 0xcc);
const QColor kCheckerDark(0x99, 0x99, 0x99);

quint64 tileKey(int tx, int ty) {
    return (static_cast<quint64>(static_cast<quint32>(ty)) << 32) | static_cast<quint32>(tx);
}

QPixmap makeCheckerPixmap() {
    QPixmap pm(kCheckerSize * 2, kCheckerSize * 2);
    pm.fill(kCheckerLight);
    QPainter p(&pm);
    p.fillRect(0, 0, kCheckerSize, kCheckerSize, kCheckerDark);
    p.fillRect(kCheckerSize, kCheckerSize, kCheckerSize, kCheckerSize, kCheckerDark);
    return pm;
}

QPointF toQt(gas::PointF p) { return {p.x, p.y}; }
gas::PointF fromQt(QPointF p) { return {p.x(), p.y()}; }

} // namespace

CanvasWidget::CanvasWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    updateCursorShape();
}

void CanvasWidget::setDocument(gas::Document* document) {
    pencil_.end();
    document_ = document;
    tileCache_.clear();
    if (document_) {
        // Drop stale dirty state, then cache every tile that already has pixels.
        document_->image().takeDirtyTiles();
        const auto& img = document_->image();
        for (int ty = 0; ty < img.tilesY(); ++ty) {
            for (int tx = 0; tx < img.tilesX(); ++tx) {
                cacheTile(tx, ty);
            }
        }
    }
    fitPending_ = true;
    if (width() > 0 && height() > 0 && isVisible()) {
        zoomToFit();
    }
    update();
}

void CanvasWidget::zoomIn() { zoomAroundCenter(kButtonZoomStep); }
void CanvasWidget::zoomOut() { zoomAroundCenter(1.0 / kButtonZoomStep); }

void CanvasWidget::zoomActual() {
    if (!document_) {
        return;
    }
    viewport_.setZoom(1.0);
    viewport_.center(document_->width(), document_->height(), width(), height());
    update();
    Q_EMIT viewChanged();
}

void CanvasWidget::zoomToFit() {
    if (!document_) {
        return;
    }
    viewport_.fit(document_->width(), document_->height(), width(), height());
    fitPending_ = false;
    update();
    Q_EMIT viewChanged();
}

void CanvasWidget::zoomAroundCenter(double factor) {
    viewport_.zoomAt({width() / 2.0, height() / 2.0}, factor);
    update();
    Q_EMIT viewChanged();
}

void CanvasWidget::resizeGL(int w, int h) {
    if (fitPending_ && document_ && w > 0 && h > 0) {
        zoomToFit();
    }
}

void CanvasWidget::cacheTile(int tx, int ty) {
    const auto& img = document_->image();
    const gas::TiledImage::Tile* tile = img.tile(tx, ty);
    if (!tile) {
        tileCache_.remove(tileKey(tx, ty));
        return;
    }
    // Edge tiles are cropped so nothing outside the document gets drawn.
    const int w = std::min(kTile, img.width() - tx * kTile);
    const int h = std::min(kTile, img.height() - ty * kTile);
    const QImage view(reinterpret_cast<const uchar*>(tile->pixels.data()), kTile, kTile,
                      kTile * static_cast<int>(sizeof(gas::Rgba8)), QImage::Format_RGBA8888);
    tileCache_.insert(tileKey(tx, ty), view.copy(0, 0, w, h));
}

void CanvasWidget::syncDirtyTiles() {
    if (!document_) {
        return;
    }
    for (const gas::Point& t : document_->image().takeDirtyTiles()) {
        cacheTile(t.x, t.y);
    }
}

void CanvasWidget::paintGL() {
    syncDirtyTiles();

    QPainter p(this);
    p.fillRect(rect(), kBackground);
    if (!document_) {
        return;
    }

    const QRectF canvasRect(toQt(viewport_.docToScreen({0, 0})),
                            toQt(viewport_.docToScreen({static_cast<double>(document_->width()),
                                                        static_cast<double>(document_->height())})));
    const QRectF visible = canvasRect.intersected(QRectF(rect()));
    if (visible.isEmpty()) {
        return;
    }

    // Transparency checkerboard, anchored to the canvas corner so it pans with it.
    static const QPixmap checker = makeCheckerPixmap();
    p.setBrushOrigin(canvasRect.topLeft());
    p.fillRect(visible, QBrush(checker));

    // Tiles, nearest-neighbour so pixel art stays crisp at any zoom.
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setClipRect(visible);
    p.translate(viewport_.offset().x, viewport_.offset().y);
    p.scale(viewport_.zoom(), viewport_.zoom());

    const gas::PointF docMin = viewport_.screenToDoc(fromQt(visible.topLeft()));
    const gas::PointF docMax = viewport_.screenToDoc(fromQt(visible.bottomRight()));
    const auto& img = document_->image();
    const int tx0 = std::max(0, static_cast<int>(std::floor(docMin.x / kTile)));
    const int ty0 = std::max(0, static_cast<int>(std::floor(docMin.y / kTile)));
    const int tx1 = std::min(img.tilesX() - 1, static_cast<int>(std::floor(docMax.x / kTile)));
    const int ty1 = std::min(img.tilesY() - 1, static_cast<int>(std::floor(docMax.y / kTile)));
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const auto it = tileCache_.constFind(tileKey(tx, ty));
            if (it != tileCache_.constEnd()) {
                p.drawImage(QPointF(tx * kTile, ty * kTile), *it);
            }
        }
    }
}

gas::Point CanvasWidget::pixelAt(QPointF widgetPos) const {
    return gas::toPixel(viewport_.screenToDoc(fromQt(widgetPos)));
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    const bool panButton = event->button() == Qt::MiddleButton ||
                           (event->button() == Qt::LeftButton && spaceHeld_);
    if (panButton) {
        panning_ = true;
        lastPanPos_ = event->position();
        updateCursorShape();
        return;
    }
    if (event->button() == Qt::LeftButton && document_) {
        pencil_.begin(document_->image(), pixelAt(event->position()));
        update();
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (document_) {
        const gas::Point px = pixelAt(event->position());
        cursorPixel_ = document_->image().contains(px) ? std::optional(px) : std::nullopt;
        Q_EMIT cursorMoved();
    }

    if (panning_) {
        const QPointF delta = event->position() - lastPanPos_;
        lastPanPos_ = event->position();
        viewport_.panBy(delta.x(), delta.y());
        update();
        return;
    }
    if (pencil_.isStroking() && document_) {
        pencil_.moveTo(document_->image(), pixelAt(event->position()));
        update();
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning_ = false;
        updateCursorShape();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        pencil_.end();
    }
}

void CanvasWidget::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    viewport_.zoomAt(fromQt(event->position()), std::pow(kWheelZoomBase, delta));
    update();
    Q_EMIT viewChanged();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = true;
        updateCursorShape();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        updateCursorShape();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void CanvasWidget::leaveEvent(QEvent* event) {
    cursorPixel_.reset();
    Q_EMIT cursorMoved();
    QOpenGLWidget::leaveEvent(event);
}

void CanvasWidget::updateCursorShape() {
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (spaceHeld_) {
        setCursor(Qt::OpenHandCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
}
