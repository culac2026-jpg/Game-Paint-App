#include "CanvasWidget.h"

#include <gas/core/Document.h>
#include <gas/core/Fill.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr int kTile = gas::TiledImage::kTileSize;
constexpr int kCheckerSize = 8; // screen pixels, independent of zoom
constexpr double kButtonZoomStep = 2.0;
constexpr double kWheelZoomBase = 1.0015; // ~1.2x per 120-unit wheel notch
constexpr double kHandleRadius = 8.0;     // screen px for grabbing a transform corner
constexpr double kMinScale = 0.01;

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

double distance(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

// Pixel-aligned rectangle spanned by two drag points.
gas::IRect dragRect(gas::PointF a, gas::PointF b) {
    const int x0 = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int y0 = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int x1 = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int y1 = static_cast<int>(std::lround(std::max(a.y, b.y)));
    return {x0, y0, x1 - x0, y1 - y0};
}

// White line under a black dashed one: readable on any background.
void drawAnts(QPainter& p, const QList<QLineF>& lines, int offset) {
    QPen white(Qt::white, 1.0);
    white.setCosmetic(true);
    p.setPen(white);
    p.drawLines(lines);
    QPen black(Qt::black, 1.0);
    black.setCosmetic(true);
    black.setDashPattern({4.0, 4.0});
    black.setDashOffset(offset);
    p.setPen(black);
    p.drawLines(lines);
}

QString toolLabel(Tool tool) {
    switch (tool) {
    case Tool::Pencil: return CanvasWidget::tr("Pencil");
    case Tool::Brush: return CanvasWidget::tr("Brush");
    case Tool::Eraser: return CanvasWidget::tr("Eraser");
    case Tool::Fill: return CanvasWidget::tr("Fill");
    case Tool::Eyedropper: return CanvasWidget::tr("Eyedropper");
    case Tool::RectSelect: return CanvasWidget::tr("Rectangular Select");
    case Tool::EllipseSelect: return CanvasWidget::tr("Elliptical Select");
    case Tool::Lasso: return CanvasWidget::tr("Lasso Select");
    case Tool::Move: return CanvasWidget::tr("Move");
    }
    return {};
}


} // namespace

CanvasWidget::CanvasWidget(QWidget* parent)
    : QOpenGLWidget(parent), scratchTile_(std::make_unique<gas::TiledImage::Tile>()) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    eraser_.settings().diameter = 24.0;
    setForegroundColor(foreground_);
    updateCursorShape();

    antsTimer_.setInterval(150);
    connect(&antsTimer_, &QTimer::timeout, this, [this] {
        if (document_ && !document_->selection().isEmpty() && !transform_) {
            antsOffset_ = (antsOffset_ + 1) % 8;
            update();
        }
    });
    antsTimer_.start();
}

// Starts an undoable edit of the active layer, or explains why it can't.
gas::TiledImage* CanvasWidget::beginEdit() {
    gas::TiledImage* img = document_->beginEdit();
    if (!img) {
        const gas::LayerProps& p = document_->activeLayer().props;
        Q_EMIT editRefused(p.locked ? tr("\"%1\" is locked").arg(QString::fromStdString(p.name))
                                    : tr("\"%1\" is hidden").arg(QString::fromStdString(p.name)));
    }
    return img;
}

void CanvasWidget::finishStroke() {
    if (!editImage_) {
        return;
    }
    if (gas::StrokeTool* active = strokeTool()) {
        active->end();
    }
    editImage_ = nullptr;
    std::string label = toolLabel(tool_).toStdString();
    if (document_->editingMask()) {
        label += " (Mask)";
    }
    if (document_->endEdit(label)) {
        Q_EMIT documentEdited();
    }
}

gas::StrokeTool* CanvasWidget::strokeTool() {
    switch (tool_) {
    case Tool::Pencil: return &pencil_;
    case Tool::Brush: return &brush_;
    case Tool::Eraser: return &eraser_;
    default: return nullptr;
    }
}

void CanvasWidget::setTool(Tool tool) {
    finishStroke();
    if (transform_ && tool != Tool::Move) {
        commitTransform();
    }
    dragging_ = false;
    dragPoints_.clear();
    picking_ = false;
    tool_ = tool;
    updateCursorShape();
    update();
}

void CanvasWidget::setForegroundColor(gas::Rgba8 color) {
    foreground_ = color;
    pencil_.setColor(color);
    brush_.setColor(color);
}

gas::BrushSettings& CanvasWidget::brushSettings(Tool tool) {
    return tool == Tool::Eraser ? eraser_.settings() : brush_.settings();
}

void CanvasWidget::setDocument(gas::Document* document) {
    if (document_) {
        finishStroke();
        if (transform_) {
            document_->cancelTransform();
            transform_.reset();
            Q_EMIT transformModeChanged(false);
        }
    }
    picking_ = false;
    dragging_ = false;
    dragPoints_.clear();
    document_ = document;
    tileCache_.clear();
    outline_.clear();
    outlineGeneration_ = ~0u;
    if (document_) {
        docWidth_ = document_->width();
        docHeight_ = document_->height();
        document_->markAllDirty(); // rebuilt on the next paint
    }
    zoomToFit(); // defers itself until the widget has a real size
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
    if (!isVisible() || width() <= 1 || height() <= 1) {
        fitPending_ = true; // not laid out yet; resizeGL() fits once the real size is known
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

// ---------------------------------------------------------------- rendering

void CanvasWidget::cacheTile(int tx, int ty) {
    if (!document_->compositeTile(tx, ty, *scratchTile_)) {
        tileCache_.remove(tileKey(tx, ty));
        return;
    }
    // Edge tiles are cropped so nothing outside the document gets drawn.
    const int w = std::min(kTile, document_->width() - tx * kTile);
    const int h = std::min(kTile, document_->height() - ty * kTile);
    const QImage view(reinterpret_cast<const uchar*>(scratchTile_->pixels.data()), kTile, kTile,
                      kTile * static_cast<int>(sizeof(gas::Rgba8)), QImage::Format_RGBA8888);
    tileCache_.insert(tileKey(tx, ty), view.copy(0, 0, w, h));
}

// A crop (or its undo) changes the canvas size: every cached tile is stale.
void CanvasWidget::syncDocumentSize() {
    if (document_ && (document_->width() != docWidth_ || document_->height() != docHeight_)) {
        docWidth_ = document_->width();
        docHeight_ = document_->height();
        tileCache_.clear();
        document_->markAllDirty();
    }
}

void CanvasWidget::syncDirtyTiles() {
    if (!document_) {
        return;
    }
    syncDocumentSize();
    for (const gas::Point& t : document_->takeDirtyTiles()) {
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
    if (!visible.isEmpty()) {
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
        const int tx0 = std::max(0, static_cast<int>(std::floor(docMin.x / kTile)));
        const int ty0 = std::max(0, static_cast<int>(std::floor(docMin.y / kTile)));
        const int tx1 = std::min(document_->tilesX() - 1, static_cast<int>(std::floor(docMax.x / kTile)));
        const int ty1 = std::min(document_->tilesY() - 1, static_cast<int>(std::floor(docMax.y / kTile)));
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                const auto it = tileCache_.constFind(tileKey(tx, ty));
                if (it != tileCache_.constEnd()) {
                    p.drawImage(QPointF(tx * kTile, ty * kTile), *it);
                }
            }
        }
        p.resetTransform();
        p.setClipping(false);
    }

    if (transform_) {
        drawTransformBox(p);
    } else {
        drawSelection(p);
    }
    drawSelectionPreview(p);
    drawBrushOutline(p);
}

void CanvasWidget::drawBrushOutline(QPainter& p) const {
    if (!cursorPos_ || spaceHeld_ || panning_ || transform_ || (tool_ != Tool::Brush && tool_ != Tool::Eraser)) {
        return;
    }
    const gas::BrushTool& b = tool_ == Tool::Eraser ? eraser_ : brush_;
    const double r = b.effectiveDiameter() / 2.0 * viewport_.zoom();
    if (r < 3.0) {
        return; // the crosshair cursor is enough
    }
    // Dark + light rings so the outline reads on any background.
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
    p.drawEllipse(*cursorPos_, r + 0.5, r + 0.5);
    p.setPen(QPen(QColor(255, 255, 255, 200), 1.0));
    p.drawEllipse(*cursorPos_, r - 0.5, r - 0.5);
    p.setRenderHint(QPainter::Antialiasing, false);
}

// Marching ants along the selection boundary (pixels with coverage >= 50%).
void CanvasWidget::drawSelection(QPainter& p) {
    if (outlineGeneration_ != document_->selectionGeneration()) {
        outlineGeneration_ = document_->selectionGeneration();
        outline_ = document_->selection().outline();
    }
    if (outline_.empty()) {
        return;
    }
    QList<QLineF> lines;
    lines.reserve(static_cast<qsizetype>(outline_.size()));
    const QRectF bounds = QRectF(rect()).adjusted(-2, -2, 2, 2);
    for (const gas::Selection::Segment& s : outline_) {
        const QPointF a = toQt(viewport_.docToScreen({double(s.x0), double(s.y0)}));
        const QPointF b = toQt(viewport_.docToScreen({double(s.x1), double(s.y1)}));
        if (!QRectF(a, b).normalized().adjusted(-1, -1, 1, 1).intersects(bounds)) {
            continue;
        }
        lines.append({a, b});
    }
    drawAnts(p, lines, antsOffset_);
}

void CanvasWidget::drawSelectionPreview(QPainter& p) const {
    if (!dragging_ || dragPoints_.size() < 2) {
        return;
    }
    QPainterPath path;
    if (tool_ == Tool::Lasso) {
        path.moveTo(toQt(viewport_.docToScreen(dragPoints_.front())));
        for (std::size_t i = 1; i < dragPoints_.size(); ++i) {
            path.lineTo(toQt(viewport_.docToScreen(dragPoints_[i])));
        }
    } else {
        const gas::IRect r = dragRect(dragPoints_[0], dragPoints_[1]);
        const QRectF sr(toQt(viewport_.docToScreen({double(r.x), double(r.y)})),
                        toQt(viewport_.docToScreen({double(r.right()), double(r.bottom())})));
        if (tool_ == Tool::EllipseSelect) {
            path.addEllipse(sr);
        } else {
            path.addRect(sr);
        }
    }
    p.setBrush(Qt::NoBrush);
    QPen white(Qt::white, 1.0);
    p.setPen(white);
    p.drawPath(path);
    QPen black(Qt::black, 1.0);
    black.setDashPattern({4.0, 4.0});
    p.setPen(black);
    p.drawPath(path);
}

void CanvasWidget::drawTransformBox(QPainter& p) const {
    const std::vector<QPointF> c = transformCorners();
    QList<QLineF> lines{{c[0], c[1]}, {c[1], c[2]}, {c[2], c[3]}, {c[3], c[0]}};
    drawAnts(p, lines, 0);
    p.setPen(QPen(Qt::black, 1.0));
    p.setBrush(Qt::white);
    for (const QPointF& pt : c) {
        p.drawRect(QRectF(pt.x() - 4, pt.y() - 4, 8, 8));
    }
    const QPointF centre = (c[0] + c[2]) / 2.0;
    p.drawEllipse(centre, 3.0, 3.0);
}

// ---------------------------------------------------------------- coordinates

gas::Point CanvasWidget::pixelAt(QPointF widgetPos) const { return gas::toPixel(docAt(widgetPos)); }

gas::PointF CanvasWidget::docAt(QPointF widgetPos) const { return viewport_.screenToDoc(fromQt(widgetPos)); }

// Samples the composite of all visible layers, ignoring fully transparent pixels.
void CanvasWidget::pickColorAt(QPointF widgetPos) {
    const gas::Point p = pixelAt(widgetPos);
    if (p.x < 0 || p.y < 0 || p.x >= document_->width() || p.y >= document_->height()) {
        return;
    }
    const gas::Rgba8 c = document_->compositePixel(p);
    if (c.a != 0 && c != foreground_) {
        setForegroundColor(c);
        Q_EMIT foregroundColorChanged(c);
    }
}

// ---------------------------------------------------------------- selection tools

void CanvasWidget::finishSelectionDrag(bool shift, bool alt) {
    dragging_ = false;
    std::vector<gas::PointF> pts = std::move(dragPoints_);
    dragPoints_.clear();
    update();
    if (pts.size() < 2) {
        return;
    }
    gas::Selection::Op op = selectionOp_;
    if (shift && alt) {
        op = gas::Selection::Op::Intersect;
    } else if (shift) {
        op = gas::Selection::Op::Add;
    } else if (alt) {
        op = gas::Selection::Op::Subtract;
    }

    const int w = document_->width(), h = document_->height();
    std::optional<gas::Selection> shape;
    if (tool_ == Tool::Lasso) {
        if (pts.size() >= 3) {
            shape = gas::Selection::polygon(w, h, pts, antialiasSelection_);
        }
    } else {
        const gas::IRect r = dragRect(pts[0], pts[1]);
        if (!r.empty()) {
            shape = tool_ == Tool::RectSelect
                        ? gas::Selection::rect(w, h, r)
                        : gas::Selection::ellipse(w, h, {double(r.x), double(r.y), double(r.w), double(r.h)},
                                                  antialiasSelection_);
        }
    }
    bool changed = false;
    if (!shape || shape->isEmpty()) {
        // A plain click (no area) deselects, like most editors.
        if (op == gas::Selection::Op::Replace) {
            changed = document_->deselect();
        }
    } else {
        const gas::Selection result =
            op == gas::Selection::Op::Replace ? *shape : document_->selection().combined(*shape, op);
        changed = document_->setSelection(result, toolLabel(tool_).toStdString());
    }
    if (changed) {
        Q_EMIT documentEdited();
    }
}

// ---------------------------------------------------------------- transform

gas::Affine CanvasWidget::transformMatrix() const {
    const TransformState& t = *transform_;
    const gas::PointF c{t.source.x + t.source.w / 2.0, t.source.y + t.source.h / 2.0};
    return gas::Affine::translate(c.x + t.offset.x, c.y + t.offset.y) * gas::Affine::rotate(t.angle) *
           gas::Affine::scale(t.sx, t.sy) * gas::Affine::translate(-c.x, -c.y);
}

std::vector<QPointF> CanvasWidget::transformCorners() const {
    const gas::Affine m = transformMatrix();
    const gas::IRect& r = transform_->source;
    const gas::PointF pts[4] = {{double(r.x), double(r.y)},
                                {double(r.right()), double(r.y)},
                                {double(r.right()), double(r.bottom())},
                                {double(r.x), double(r.bottom())}};
    std::vector<QPointF> out;
    for (const gas::PointF& pt : pts) {
        out.push_back(toQt(viewport_.docToScreen(m.map(pt))));
    }
    return out;
}

bool CanvasWidget::beginFreeTransform() {
    if (!document_ || transform_) {
        return transform_.has_value();
    }
    finishStroke();
    if (!document_->beginTransform()) {
        const gas::LayerProps& p = document_->activeLayer().props;
        const QString name = QString::fromStdString(p.name);
        Q_EMIT editRefused(p.locked    ? tr("\"%1\" is locked").arg(name)
                           : !p.visible ? tr("\"%1\" is hidden").arg(name)
                                        : tr("Nothing to transform"));
        return false;
    }
    transform_ = TransformState{};
    transform_->source = document_->transformSourceRect();
    updateCursorShape();
    update();
    Q_EMIT transformModeChanged(true);
    return true;
}

void CanvasWidget::applyTransform() {
    document_->setTransform(transformMatrix());
    update();
}

void CanvasWidget::commitTransform() {
    if (!transform_) {
        return;
    }
    transform_.reset();
    document_->commitTransform(interpolation_);
    updateCursorShape();
    update();
    Q_EMIT transformModeChanged(false);
    Q_EMIT documentEdited();
}

void CanvasWidget::cancelTransform() {
    if (!transform_) {
        return;
    }
    transform_.reset();
    document_->cancelTransform();
    updateCursorShape();
    update();
    Q_EMIT transformModeChanged(false);
    Q_EMIT documentEdited();
}

void CanvasWidget::transformPress(QPointF pos, Qt::KeyboardModifiers) {
    TransformState& t = *transform_;
    const std::vector<QPointF> c = transformCorners();
    t.startDoc = docAt(pos);
    t.offset0 = t.offset;
    t.angle0 = t.angle;
    t.sx0 = t.sx;
    t.sy0 = t.sy;
    t.corner = -1;
    for (int i = 0; i < 4; ++i) {
        if (distance(pos, c[static_cast<std::size_t>(i)]) <= kHandleRadius) {
            t.corner = i;
        }
    }
    if (t.corner >= 0) {
        t.drag = TransformState::Drag::Scale;
    } else if (QPolygonF({c[0], c[1], c[2], c[3]}).containsPoint(pos, Qt::OddEvenFill)) {
        t.drag = TransformState::Drag::Move;
    } else {
        t.drag = TransformState::Drag::Rotate;
    }
}

void CanvasWidget::transformDrag(QPointF pos, Qt::KeyboardModifiers mods) {
    TransformState& t = *transform_;
    const gas::PointF doc = docAt(pos);
    const gas::PointF centre{t.source.x + t.source.w / 2.0 + t.offset0.x, t.source.y + t.source.h / 2.0 + t.offset0.y};
    switch (t.drag) {
    case TransformState::Drag::None: return;
    case TransformState::Drag::Move:
        // Whole-pixel moves keep pixel art exact.
        t.offset = {t.offset0.x + std::round(doc.x - t.startDoc.x), t.offset0.y + std::round(doc.y - t.startDoc.y)};
        break;
    case TransformState::Drag::Rotate: {
        const double a0 = std::atan2(t.startDoc.y - centre.y, t.startDoc.x - centre.x);
        const double a1 = std::atan2(doc.y - centre.y, doc.x - centre.x);
        t.angle = t.angle0 + (a1 - a0);
        if (mods & Qt::ShiftModifier) {
            const double step = std::numbers::pi / 12.0; // 15 degrees
            t.angle = std::round(t.angle / step) * step;
        }
        break;
    }
    case TransformState::Drag::Scale: {
        // Scale about the centre, measured along the box's own (rotated) axes.
        const double dx = doc.x - centre.x, dy = doc.y - centre.y;
        const double cs = std::cos(-t.angle), sn = std::sin(-t.angle);
        const double lx = cs * dx - sn * dy, ly = sn * dx + cs * dy;
        static constexpr int signX[4] = {-1, 1, 1, -1};
        static constexpr int signY[4] = {-1, -1, 1, 1};
        double sx = lx / (signX[t.corner] * t.source.w / 2.0);
        double sy = ly / (signY[t.corner] * t.source.h / 2.0);
        if (mods & Qt::ShiftModifier) { // keep the aspect ratio
            const double k = (std::abs(sx / t.sx0) + std::abs(sy / t.sy0)) / 2.0;
            sx = t.sx0 * k * (sx < 0 ? -1 : 1) * (t.sx0 < 0 ? -1 : 1);
            sy = t.sy0 * k * (sy < 0 ? -1 : 1) * (t.sy0 < 0 ? -1 : 1);
        }
        const auto clampScale = [](double s) { return std::abs(s) < kMinScale ? (s < 0 ? -kMinScale : kMinScale) : s; };
        t.sx = clampScale(sx);
        t.sy = clampScale(sy);
        break;
    }
    }
    applyTransform();
}

// ---------------------------------------------------------------- input

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    const bool panButton = event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceHeld_);
    if (panButton) {
        panning_ = true;
        lastPanPos_ = event->position();
        updateCursorShape();
        update();
        return;
    }
    if (event->button() != Qt::LeftButton || !document_) {
        return;
    }
    if (transform_) {
        transformPress(event->position(), event->modifiers());
        update();
        return;
    }
    switch (tool_) {
    case Tool::Fill:
        if (gas::TiledImage* img = beginEdit()) {
            gas::floodFill(*img, pixelAt(event->position()), foreground_, fillTolerance_);
            if (document_->endEdit(toolLabel(tool_).toStdString())) {
                Q_EMIT documentEdited();
            }
        }
        break;
    case Tool::Eyedropper:
        picking_ = true;
        pickColorAt(event->position());
        break;
    case Tool::RectSelect:
    case Tool::EllipseSelect:
    case Tool::Lasso:
        dragging_ = true;
        dragMods_ = event->modifiers();
        dragPoints_ = {docAt(event->position()), docAt(event->position())};
        break;
    case Tool::Move:
        // Drag the selected pixels (or the whole layer) and commit on release.
        if (beginFreeTransform()) {
            transformPress(event->position(), event->modifiers());
            transform_->drag = TransformState::Drag::Move;
            transform_->oneShot = true;
        }
        break;
    default:
        if (gas::TiledImage* img = beginEdit()) {
            editImage_ = img;
            strokeTool()->begin(*img, docAt(event->position()));
            document_->syncEdit();
        }
        break;
    }
    update();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    cursorPos_ = event->position();
    if (document_) {
        const gas::Point px = pixelAt(event->position());
        const bool inside = px.x >= 0 && px.y >= 0 && px.x < document_->width() && px.y < document_->height();
        cursorPixel_ = inside ? std::optional(px) : std::nullopt;
        Q_EMIT cursorMoved();
    }

    if (panning_) {
        const QPointF delta = event->position() - lastPanPos_;
        lastPanPos_ = event->position();
        viewport_.panBy(delta.x(), delta.y());
    } else if (transform_) {
        if (transform_->drag != TransformState::Drag::None && (event->buttons() & Qt::LeftButton)) {
            transformDrag(event->position(), event->modifiers());
        } else {
            updateCursorShape(); // hover feedback over handles
        }
    } else if (dragging_) {
        const gas::PointF doc = docAt(event->position());
        if (tool_ == Tool::Lasso) {
            const QPointF last = toQt(viewport_.docToScreen(dragPoints_.back()));
            if (distance(last, event->position()) >= 2.0) {
                dragPoints_.push_back(doc);
            }
        } else {
            dragPoints_[1] = doc;
        }
    } else if (picking_ && document_) {
        pickColorAt(event->position());
    } else if (editImage_) {
        strokeTool()->moveTo(*editImage_, docAt(event->position()));
        document_->syncEdit(); // push the clipped result into the layer for display
    }
    update(); // also repaints the brush outline
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning_ = false;
        updateCursorShape();
        update();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (transform_) {
        transform_->drag = TransformState::Drag::None;
        if (transform_->oneShot) {
            commitTransform();
        }
        return;
    }
    if (dragging_) {
        finishSelectionDrag(dragMods_.testFlag(Qt::ShiftModifier), dragMods_.testFlag(Qt::AltModifier));
        return;
    }
    picking_ = false;
    finishStroke();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (transform_ && event->button() == Qt::LeftButton) {
        const std::vector<QPointF> c = transformCorners();
        if (QPolygonF({c[0], c[1], c[2], c[3]}).containsPoint(event->position(), Qt::OddEvenFill)) {
            commitTransform();
            return;
        }
    }
    mousePressEvent(event);
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
    if (transform_) {
        const double step = (event->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter: commitTransform(); return;
        case Qt::Key_Escape: cancelTransform(); return;
        case Qt::Key_Left: transform_->offset.x -= step; applyTransform(); return;
        case Qt::Key_Right: transform_->offset.x += step; applyTransform(); return;
        case Qt::Key_Up: transform_->offset.y -= step; applyTransform(); return;
        case Qt::Key_Down: transform_->offset.y += step; applyTransform(); return;
        default: break;
        }
    } else if (dragging_ && event->key() == Qt::Key_Escape) {
        dragging_ = false;
        dragPoints_.clear();
        update();
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
    cursorPos_.reset();
    Q_EMIT cursorMoved();
    update();
    QOpenGLWidget::leaveEvent(event);
}

void CanvasWidget::updateCursorShape() {
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (spaceHeld_) {
        setCursor(Qt::OpenHandCursor);
    } else if (transform_) {
        Qt::CursorShape shape = Qt::PointingHandCursor; // outside the box: rotate
        if (cursorPos_) {
            const std::vector<QPointF> c = transformCorners();
            if (QPolygonF({c[0], c[1], c[2], c[3]}).containsPoint(*cursorPos_, Qt::OddEvenFill)) {
                shape = Qt::SizeAllCursor;
            }
            for (const QPointF& pt : c) {
                if (distance(*cursorPos_, pt) <= kHandleRadius) {
                    shape = Qt::SizeFDiagCursor;
                }
            }
        }
        setCursor(shape);
    } else if (tool_ == Tool::Move) {
        setCursor(Qt::SizeAllCursor);
    } else if (tool_ == Tool::Fill || tool_ == Tool::Eyedropper) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
    update(); // brush outline visibility depends on the same state
}
