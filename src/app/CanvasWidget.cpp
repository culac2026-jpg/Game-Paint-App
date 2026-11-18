#include "CanvasWidget.h"

#include "VectorTools.h"

#include <gas/core/Document.h>
#include <gas/core/Fill.h>

#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QTabletEvent>
#include <QWheelEvent>

#include <algorithm>
#include <array>
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
    case Tool::Smudge: return CanvasWidget::tr("Smudge");
    case Tool::Blur: return CanvasWidget::tr("Blur");
    case Tool::Slice: return CanvasWidget::tr("Slice");
    case Tool::Pen: return CanvasWidget::tr("Pen");
    case Tool::Shape: return CanvasWidget::tr("Shape");
    case Tool::Node: return CanvasWidget::tr("Node");
    }
    return {};
}


} // namespace

CanvasWidget::CanvasWidget(QWidget* parent)
    : QOpenGLWidget(parent), scratchTile_(std::make_unique<gas::TiledImage::Tile>()) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    eraser_.settings().diameter = 24.0;
    smudge_.settings().diameter = 24.0;
    smudge_.settings().opacity = 0.6; // strength
    smudge_.settings().hardness = 0.5;
    blur_.settings().diameter = 30.0;
    blur_.settings().opacity = 0.5;
    blur_.settings().hardness = 0.3;
    blur_.settings().spacing = 0.15;
    setForegroundColor(foreground_);
    vector_ = new VectorEditor(this);
    connect(vector_, &VectorEditor::edited, this, &CanvasWidget::documentEdited);
    connect(vector_, &VectorEditor::layerChanged, this, &CanvasWidget::documentEdited);
    connect(vector_, &VectorEditor::refused, this, &CanvasWidget::editRefused);
    connect(vector_, &VectorEditor::selectionChanged, this, [this] { update(); });
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
        const QString name = QString::fromStdString(p.name);
        Q_EMIT editRefused(p.locked    ? tr("\"%1\" is locked").arg(name)
                           : !p.visible ? tr("\"%1\" is hidden").arg(name)
                                        : tr("\"%1\" is a vector layer: edit it with the Pen (P), Shape (J) or "
                                             "Node (A) tool, or use Layer > Rasterize Vector Layer").arg(name));
    }
    return img;
}

void CanvasWidget::finishStroke() {
    if (!editImage_) {
        return;
    }
    if (gas::StrokeTool* active = strokeTool()) {
        if (streamline_ > 0.0 && rawStroke_.size() >= 3) {
            // StreamLine: throw the live stroke away and redraw it along the smoothed path.
            active->end();
            smoother_.finish();
            // A stroke that ends near its start closes the shape. "Near" is what
            // looks closed: about 24 screen px, or the brush size, whichever is more.
            double closeDistance = 24.0 / viewport_.zoom();
            if (usesBrushSettings(tool_)) {
                closeDistance = std::max(closeDistance, brushTool()->effectiveDiameter());
            }
            const std::vector<gas::StrokeSample> path = gas::streamlinePath(rawStroke_, streamline_, closeDistance);
            document_->revertEdit();
            active->begin(*editImage_, path.front().pos, path.front().pressure);
            for (std::size_t i = 1; i < path.size(); ++i) {
                active->moveTo(*editImage_, path[i].pos, path[i].pressure);
            }
            active->end();
        } else {
            // The stabilizer lags behind the pointer; finish at the release point.
            if (const auto tail = smoother_.finish()) {
                active->moveTo(*editImage_, tail->pos, tail->pressure);
            }
            active->end();
        }
    }
    rawStroke_.clear();
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
    case Tool::Smudge: return &smudge_;
    case Tool::Blur: return &blur_;
    default: return nullptr;
    }
}

const gas::BrushTool* CanvasWidget::brushTool() const {
    switch (tool_) {
    case Tool::Brush: return &brush_;
    case Tool::Eraser: return &eraser_;
    case Tool::Smudge: return &smudge_;
    case Tool::Blur: return &blur_;
    default: return nullptr;
    }
}

bool CanvasWidget::vectorBusy() const { return vector_ && vector_->busy(); }

QPointF CanvasWidget::docToScreenPoint(gas::PointF doc) const { return toQt(viewport_.docToScreen(doc)); }

void CanvasWidget::setTool(Tool tool) {
    finishStroke();
    vector_->toolChanged();
    if (transform_ && tool != Tool::Move) {
        commitTransform();
    }
    dragging_ = false;
    dragPoints_.clear();
    picking_ = false;
    sliceDrag_.reset();
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
    switch (tool) {
    case Tool::Eraser: return eraser_.settings();
    case Tool::Smudge: return smudge_.settings();
    case Tool::Blur: return blur_.settings();
    default: return brush_.settings();
    }
}

void CanvasWidget::setDocument(gas::Document* document) {
    lastStrokeEnd_.reset();
    lineDrag_.reset();
    sliceDrag_.reset();
    setSelectedSlice(-1);
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
    vector_->setDocument(document);
    tileCache_.clear();
    onionCache_.clear();
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
    if (symmetryDocSize_ != QSize(document_->width(), document_->height())) {
        resetSymmetryCenter(); // new document, crop, or undo of a crop
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
        drawOnionSkin(p, canvasRect, visible);

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
    drawSlices(p);
    if (isVectorTool(tool_) && !transform_) {
        vector_->paint(p, tool_);
    }
    drawSymmetryGuides(p);
    drawLinePreview(p);
    drawSelectionPreview(p);
    drawBrushOutline(p);
}

void CanvasWidget::drawBrushOutline(QPainter& p) const {
    if (!cursorPos_ || spaceHeld_ || panning_ || transform_ || !usesBrushSettings(tool_)) {
        return;
    }
    const gas::BrushTool& b = *brushTool();
    const gas::BrushSettings& s = b.settings();
    const double r = b.effectiveDiameter() / 2.0 * viewport_.zoom();
    if (r < 3.0) {
        return; // the crosshair cursor is enough
    }
    // The tip's outline at the brush's angle and roundness (direction-following
    // and jitter are per dab, so the cursor shows the base shape).
    gas::BrushShape shape = s.shape;
    if (shape == gas::BrushShape::Custom && (!s.tip.tip || s.tip.tip->empty())) {
        shape = gas::BrushShape::Round;
    }
    const double angle = s.angle * std::numbers::pi / 180.0;
    const double cs = std::cos(angle), sn = std::sin(angle);
    const double roundness = std::clamp(s.roundness, 0.05, 1.0);
    QPolygonF outline;
    for (const gas::PointF& v : gas::brushShapeOutline(shape)) {
        const double x = v.x * r, y = v.y * r * roundness;
        outline << *cursorPos_ + QPointF(cs * x - sn * y, sn * x + cs * y);
    }
    // Dark + light lines so the outline reads on any background.
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 170), 2.0));
    p.drawPolygon(outline);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
    p.drawPolygon(outline);
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

double CanvasWidget::pressureOf(const QMouseEvent* event) const {
    const QPointingDevice* device = event->pointingDevice();
    const bool pen = device && (device->type() == QInputDevice::DeviceType::Stylus ||
                                device->type() == QInputDevice::DeviceType::Airbrush);
    return pen ? tabletPressure_ : 1.0;
}

// Tablet events are only used for their pressure: ignoring them makes Qt
// deliver the same input as mouse events, which drive every tool.
void CanvasWidget::tabletEvent(QTabletEvent* event) {
    tabletPressure_ = std::clamp(event->pressure(), 0.0, 1.0);
    event->ignore();
}

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
    if (isVectorTool(tool_)) {
        vector_->press(tool_, event->position(), event->modifiers());
        updateCursorShape();
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
    case Tool::Slice:
        slicePress(event->position(), event->modifiers());
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
        if (symmetry_.enabled() && nearSymmetryHandle(event->position())) {
            draggingSymmetry_ = true; // move the mirror centre instead of painting
            break;
        }
        strokeTool()->setSymmetry(symmetry_);
        if (lineMode_ || (event->modifiers() & Qt::ControlModifier)) {
            const gas::PointF start = docAt(event->position());
            lineDrag_ = LineDrag{start, start, start};
            break;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && lastStrokeEnd_) {
            // Shift+click: straight segment from the previous stroke's end, then keep drawing.
            if (gas::TiledImage* img = beginEdit()) {
                editImage_ = img;
                smoother_.setSmoothing(0.0);
                const gas::PointF at = docAt(event->position());
                strokeTool()->begin(*img, *lastStrokeEnd_, 1.0);
                strokeTool()->moveTo(*img, at, pressureOf(event));
                smoother_.begin({at, pressureOf(event)});
                rawStroke_ = {{*lastStrokeEnd_, 1.0}, {at, pressureOf(event)}};
                lastStrokeEnd_ = at;
                document_->syncEdit();
            }
            break;
        }
        if (gas::TiledImage* img = beginEdit()) {
            editImage_ = img;
            const bool brushLike = usesBrushSettings(tool_);
            // StreamLine also smooths live, so the preview is close to the final stroke.
            smoother_.setSmoothing(std::max(brushLike ? brushSettings(tool_).smoothing : 0.0, streamline_ * 0.7));
            const gas::StrokeSample s = smoother_.begin({docAt(event->position()), pressureOf(event)});
            rawStroke_ = {{docAt(event->position()), pressureOf(event)}};
            strokeTool()->begin(*img, s.pos, s.pressure);
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
    } else if (isVectorTool(tool_)) {
        vector_->move(tool_, event->position(), event->modifiers(), event->buttons() & Qt::LeftButton);
        updateCursorShape();
    } else if (picking_ && document_) {
        pickColorAt(event->position());
    } else if (sliceDrag_) {
        sliceDragTo(event->position(), event->modifiers());
    } else if (tool_ == Tool::Slice) {
        updateCursorShape(); // hover feedback over slices and handles
    } else if (lineDrag_) {
        lineDrag_->raw = docAt(event->position());
        updateLineEnd(event->modifiers());
    } else if (draggingSymmetry_ && document_) {
        const gas::PointF d = docAt(event->position());
        const auto snap = [](double v, int max) { return std::clamp(std::round(v * 2.0) / 2.0, 0.0, double(max)); };
        symmetry_.center = {snap(d.x, document_->width()), snap(d.y, document_->height())};
    } else if (editImage_) {
        const gas::StrokeSample raw{docAt(event->position()), pressureOf(event)};
        rawStroke_.push_back(raw);
        const gas::StrokeSample s = smoother_.add(raw);
        strokeTool()->moveTo(*editImage_, s.pos, s.pressure);
        lastStrokeEnd_ = docAt(event->position());
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
    if (isVectorTool(tool_)) {
        vector_->release(tool_, event->position(), event->modifiers());
        updateCursorShape();
        return;
    }
    picking_ = false;
    draggingSymmetry_ = false;
    if (sliceDrag_) {
        finishSliceDrag();
        return;
    }
    if (lineDrag_) {
        commitLine();
        return;
    }
    if (editImage_) {
        lastStrokeEnd_ = docAt(event->position());
    }
    finishStroke();
}

void CanvasWidget::setLineMode(bool on) {
    lineMode_ = on;
    update();
}

void CanvasWidget::updateLineEnd(Qt::KeyboardModifiers mods) {
    if (!lineDrag_) {
        return;
    }
    lineDrag_->end = (mods & Qt::ShiftModifier) ? gas::snapAngle(lineDrag_->start, lineDrag_->raw, 15.0)
                                                : lineDrag_->raw;
    update();
}

// Paints the previewed line as one stroke (one undo step).
void CanvasWidget::commitLine() {
    const LineDrag line = *lineDrag_;
    lineDrag_.reset();
    gas::StrokeTool* tool = strokeTool();
    gas::TiledImage* img = tool ? beginEdit() : nullptr;
    if (!img) {
        update();
        return;
    }
    tool->begin(*img, line.start, 1.0);
    tool->moveTo(*img, line.end, 1.0);
    tool->end();
    lastStrokeEnd_ = line.end;
    std::string label = CanvasWidget::tr("Line").toStdString();
    if (document_->editingMask()) {
        label += " (Mask)";
    }
    if (document_->endEdit(label)) {
        Q_EMIT documentEdited();
    }
    update();
}

// The pending straight line (and its mirror copies) at the brush's width.
void CanvasWidget::drawLinePreview(QPainter& p) const {
    if (!lineDrag_ || !document_) {
        return;
    }
    const gas::BrushTool* brush = brushTool();
    const double width = std::max(1.0, (brush ? brush->effectiveDiameter() : 1.0) * viewport_.zoom());
    const bool paints = tool_ == Tool::Pencil || tool_ == Tool::Brush;
    QColor colour = paints ? QColor(foreground_.r, foreground_.g, foreground_.b, 150) : QColor(128, 128, 128, 140);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const gas::SymmetryCopy& copy : symmetry_.copies()) {
        const QPointF a = toQt(viewport_.docToScreen(copy.map(lineDrag_->start, symmetry_.center)));
        const QPointF b = toQt(viewport_.docToScreen(copy.map(lineDrag_->end, symmetry_.center)));
        p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        p.setPen(QPen(QColor(0, 0, 0, 120), 1.0, Qt::DashLine));
        p.drawLine(a, b); // thin centre line so pale colours still show
    }
    p.restore();
}

void CanvasWidget::setSymmetryMode(gas::SymmetryMode mode) {
    symmetry_.mode = mode;
    update();
}

void CanvasWidget::setSymmetrySegments(int segments) {
    symmetry_.segments = std::clamp(segments, gas::Symmetry::kMinSegments, gas::Symmetry::kMaxSegments);
    update();
}

void CanvasWidget::resetSymmetryCenter() {
    if (!document_) {
        return;
    }
    symmetryDocSize_ = QSize(document_->width(), document_->height());
    symmetry_.center = {document_->width() / 2.0, document_->height() / 2.0};
    update();
}

bool CanvasWidget::nearSymmetryHandle(QPointF widgetPos) const {
    return distance(toQt(viewport_.docToScreen(symmetry_.center)), widgetPos) <= 8.0;
}

// Dashed mirror axes (or the sector lines of radial modes) and the centre handle.
void CanvasWidget::drawSymmetryGuides(QPainter& p) const {
    if (!document_ || !symmetry_.enabled()) {
        return;
    }
    const QPointF c = toQt(viewport_.docToScreen(symmetry_.center));
    const double len = std::hypot(document_->width(), document_->height()) * viewport_.zoom();
    QVector<QLineF> lines;
    const auto ray = [&](double angle) { lines << QLineF(c, c + QPointF(std::cos(angle), std::sin(angle)) * len); };
    constexpr double pi = std::numbers::pi;
    switch (symmetry_.mode) {
    case gas::SymmetryMode::Vertical: ray(-pi / 2); ray(pi / 2); break;
    case gas::SymmetryMode::Horizontal: ray(0); ray(pi); break;
    case gas::SymmetryMode::Quad: for (int k = 0; k < 4; ++k) ray(k * pi / 2); break;
    default: {
        const int n = std::clamp(symmetry_.segments, gas::Symmetry::kMinSegments, gas::Symmetry::kMaxSegments);
        for (int k = 0; k < n; ++k) {
            ray(-pi / 2 + 2 * pi * k / n);
        }
        break;
    }
    }
    const QRectF canvasRect(toQt(viewport_.docToScreen({0, 0})),
                            toQt(viewport_.docToScreen({double(document_->width()), double(document_->height())})));
    p.save();
    p.setClipRect(canvasRect);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen dark(QColor(0, 0, 0, 150), 1.0, Qt::DashLine);
    QPen light(QColor(80, 200, 255, 220), 1.0, Qt::DashLine);
    light.setDashOffset(3); // interleave so the line reads on any colour
    p.setPen(dark);
    p.drawLines(lines);
    p.setPen(light);
    p.drawLines(lines);
    p.setClipping(false);
    p.setPen(QPen(QColor(0, 0, 0, 180), 1.0));
    p.setBrush(QColor(80, 200, 255, 200));
    p.drawEllipse(c, 5.0, 5.0);
    p.restore();
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (transform_ && event->button() == Qt::LeftButton) {
        const std::vector<QPointF> c = transformCorners();
        if (QPolygonF({c[0], c[1], c[2], c[3]}).containsPoint(event->position(), Qt::OddEvenFill)) {
            commitTransform();
            return;
        }
    }
    if (isVectorTool(tool_) && !transform_ && event->button() == Qt::LeftButton &&
        vector_->doubleClick(tool_, event->position())) {
        update();
        return;
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
    if (isVectorTool(tool_) && !transform_ && vector_->key(tool_, event)) {
        update();
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
    } else if (sliceDrag_ && event->key() == Qt::Key_Escape) {
        sliceDrag_.reset();
        update();
        return;
    } else if (tool_ == Tool::Slice && !sliceDrag_ && selectedSlice_ >= 0) {
        const int step = (event->modifiers() & Qt::ShiftModifier) ? 10 : 1;
        switch (event->key()) {
        case Qt::Key_Left: nudgeSlice(-step, 0); return;
        case Qt::Key_Right: nudgeSlice(step, 0); return;
        case Qt::Key_Up: nudgeSlice(0, -step); return;
        case Qt::Key_Down: nudgeSlice(0, step); return;
        case Qt::Key_Backspace: deleteSelectedSlice(); return;
        case Qt::Key_Escape: setSelectedSlice(-1); return;
        default: break;
        }
    } else if (lineDrag_ && event->key() == Qt::Key_Escape) {
        lineDrag_.reset();
        update();
        return;
    } else if (lineDrag_ && event->key() == Qt::Key_Shift) {
        updateLineEnd(event->modifiers() | Qt::ShiftModifier);
    } else if (dragging_ && event->key() == Qt::Key_Escape) {
        dragging_ = false;
        dragPoints_.clear();
        update();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (lineDrag_ && event->key() == Qt::Key_Shift) {
        updateLineEnd(event->modifiers() & ~Qt::KeyboardModifiers(Qt::ShiftModifier));
    }
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
    } else if (isVectorTool(tool_)) {
        setCursor(vector_->cursor(tool_));
    } else if (tool_ == Tool::Slice && document_) {
        Qt::CursorShape shape = Qt::CrossCursor;
        if (sliceDrag_) {
            shape = sliceDrag_->mode == SliceDrag::Mode::Move ? Qt::SizeAllCursor : Qt::CrossCursor;
        } else if (cursorPos_) {
            static constexpr Qt::CursorShape kHandleCursors[8] = {
                Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
            if (nearSlicePivot(*cursorPos_)) {
                shape = Qt::PointingHandCursor;
            } else if (const int b = sliceBorderAt(*cursorPos_, QGuiApplication::keyboardModifiers() & Qt::AltModifier);
                       b >= 0) {
                shape = b == 0 || b == 2 ? Qt::SplitHCursor : Qt::SplitVCursor;
            } else if (const int h = sliceHandleAt(*cursorPos_); h >= 0) {
                shape = kHandleCursors[h];
            } else if (sliceAt(*cursorPos_) >= 0) {
                shape = Qt::SizeAllCursor;
            }
        }
        setCursor(shape);
    } else if (tool_ == Tool::Fill || tool_ == Tool::Eyedropper) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
    update(); // brush outline visibility depends on the same state
}

// ---------------------------------------------------------------- slices

namespace {

constexpr double kSliceHandleRadius = 5.0;

} // namespace

void CanvasWidget::setSelectedSlice(int index) {
    if (!document_ || index < 0 || index >= static_cast<int>(document_->slices().size())) {
        index = -1;
    }
    if (index != selectedSlice_) {
        selectedSlice_ = index;
        Q_EMIT selectedSliceChanged(index);
    }
    update();
}

void CanvasWidget::setShowSlices(bool on) {
    showSlices_ = on;
    update();
}

bool CanvasWidget::deleteSelectedSlice() {
    if (!document_ || isBusy() || selectedSlice_ < 0 || selectedSlice_ >= static_cast<int>(document_->slices().size())) {
        return false;
    }
    std::vector<gas::Slice> slices = document_->slices();
    slices.erase(slices.begin() + selectedSlice_);
    const int next = std::min(selectedSlice_, static_cast<int>(slices.size()) - 1);
    if (!document_->setSlices(std::move(slices), "Delete Slice")) {
        return false;
    }
    selectedSlice_ = next; // always announce: the index now names another slice
    Q_EMIT selectedSliceChanged(selectedSlice_);
    update();
    Q_EMIT documentEdited();
    return true;
}

QRectF CanvasWidget::sliceScreenRect(const gas::IRect& r) const {
    return QRectF(toQt(viewport_.docToScreen({double(r.x), double(r.y)})),
                  toQt(viewport_.docToScreen({double(r.right()), double(r.bottom())})));
}

QPointF CanvasWidget::slicePivotScreen(const gas::Slice& s) const {
    return toQt(viewport_.docToScreen({s.rect.x + s.pivot.x * s.rect.w, s.rect.y + s.pivot.y * s.rect.h}));
}

namespace {

// TL, T, TR, R, BR, B, BL, L
std::array<QPointF, 8> handlePoints(const QRectF& r) {
    const QPointF c = r.center();
    return {r.topLeft(),     QPointF(c.x(), r.top()),    r.topRight(),   QPointF(r.right(), c.y()),
            r.bottomRight(), QPointF(c.x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), c.y())};
}

} // namespace

int CanvasWidget::sliceAt(QPointF pos) const {
    if (!document_) {
        return -1;
    }
    const std::vector<gas::Slice>& slices = document_->slices();
    const gas::PointF d = docAt(pos);
    const auto inside = [&](int i) {
        const gas::IRect& r = slices[static_cast<std::size_t>(i)].rect;
        return d.x >= r.x && d.y >= r.y && d.x < r.right() && d.y < r.bottom();
    };
    if (selectedSlice_ >= 0 && selectedSlice_ < static_cast<int>(slices.size()) && inside(selectedSlice_)) {
        return selectedSlice_;
    }
    for (int i = static_cast<int>(slices.size()) - 1; i >= 0; --i) {
        if (inside(i)) {
            return i;
        }
    }
    return -1;
}

int CanvasWidget::sliceHandleAt(QPointF pos) const {
    if (!document_ || selectedSlice_ < 0 || selectedSlice_ >= static_cast<int>(document_->slices().size())) {
        return -1;
    }
    const auto pts = handlePoints(sliceScreenRect(document_->slices()[static_cast<std::size_t>(selectedSlice_)].rect));
    for (int i = 0; i < 8; ++i) {
        if (distance(pts[static_cast<std::size_t>(i)], pos) <= kSliceHandleRadius + 1.0) {
            return i;
        }
    }
    return -1;
}

bool CanvasWidget::nearSlicePivot(QPointF pos) const {
    if (!document_ || selectedSlice_ < 0 || selectedSlice_ >= static_cast<int>(document_->slices().size())) {
        return false;
    }
    return distance(slicePivotScreen(document_->slices()[static_cast<std::size_t>(selectedSlice_)]), pos) <= 7.0;
}

int CanvasWidget::sliceBorderAt(QPointF pos, bool alt) const {
    const std::vector<gas::Slice>& slices = document_->slices();
    if (selectedSlice_ < 0 || selectedSlice_ >= static_cast<int>(slices.size())) {
        return -1;
    }
    const gas::Slice& s = slices[static_cast<std::size_t>(selectedSlice_)];
    const QRectF r = sliceScreenRect(s.rect);
    const double z = viewport_.zoom();
    // Guide positions in screen space: L, T, R, B.
    const double guides[4] = {r.left() + s.border.left * z, r.top() + s.border.top * z,
                              r.right() - s.border.right * z, r.bottom() - s.border.bottom * z};
    const int values[4] = {s.border.left, s.border.top, s.border.right, s.border.bottom};
    constexpr double kReach = 4.0;
    for (int k = 0; k < 4; ++k) {
        if (values[k] == 0 && !alt) {
            continue; // an unset guide lies on the edge; Alt+drag pulls it out
        }
        const bool vertical = k == 0 || k == 2;
        const double along = vertical ? pos.y() : pos.x();
        const double lo = vertical ? r.top() : r.left(), hi = vertical ? r.bottom() : r.right();
        if (along >= lo && along <= hi && std::abs((vertical ? pos.x() : pos.y()) - guides[k]) <= kReach) {
            return k;
        }
    }
    return -1;
}

void CanvasWidget::slicePress(QPointF pos, Qt::KeyboardModifiers mods) {
    const gas::PointF d = docAt(pos);
    const std::vector<gas::Slice>& slices = document_->slices();
    using Mode = SliceDrag::Mode;
    if (selectedSlice_ >= 0 && selectedSlice_ < static_cast<int>(slices.size())) {
        const gas::Slice& s = slices[static_cast<std::size_t>(selectedSlice_)];
        if (nearSlicePivot(pos)) {
            sliceDrag_ = SliceDrag{Mode::Pivot, selectedSlice_, -1, d, s, s};
            return;
        }
        if (const int b = sliceBorderAt(pos, mods & Qt::AltModifier); b >= 0) {
            sliceDrag_ = SliceDrag{Mode::Border, selectedSlice_, b, d, s, s};
            return;
        }
        if (const int h = sliceHandleAt(pos); h >= 0) {
            sliceDrag_ = SliceDrag{Mode::Resize, selectedSlice_, h, d, s, s};
            return;
        }
    }
    if (const int hit = sliceAt(pos); hit >= 0) {
        setSelectedSlice(hit);
        const gas::Slice& s = slices[static_cast<std::size_t>(hit)];
        sliceDrag_ = SliceDrag{Mode::Move, hit, -1, d, s, s};
        return;
    }
    setSelectedSlice(-1);
    gas::Slice fresh;
    fresh.rect = {static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), 0, 0};
    sliceDrag_ = SliceDrag{Mode::Create, -1, -1, d, fresh, fresh};
}

void CanvasWidget::sliceDragTo(QPointF pos, Qt::KeyboardModifiers mods) {
    SliceDrag& drag = *sliceDrag_;
    const gas::PointF d = docAt(pos);
    const int W = document_->width(), H = document_->height();
    const auto clampX = [W](long v) { return static_cast<int>(std::clamp<long>(v, 0, W)); };
    const auto clampY = [H](long v) { return static_cast<int>(std::clamp<long>(v, 0, H)); };
    const gas::IRect r0 = drag.start.rect;
    gas::IRect& r = drag.current.rect;
    using Mode = SliceDrag::Mode;
    switch (drag.mode) {
    case Mode::Create: {
        // Corners snap to pixel boundaries; a click without a drag stays empty.
        const int ax = clampX(std::lround(drag.startDoc.x)), ay = clampY(std::lround(drag.startDoc.y));
        const int bx = clampX(std::lround(d.x)), by = clampY(std::lround(d.y));
        r = {std::min(ax, bx), std::min(ay, by), std::abs(bx - ax), std::abs(by - ay)};
        break;
    }
    case Mode::Move: {
        const long dx = std::lround(d.x - drag.startDoc.x), dy = std::lround(d.y - drag.startDoc.y);
        r.x = static_cast<int>(std::clamp<long>(r0.x + dx, 0, std::max(0, W - r0.w)));
        r.y = static_cast<int>(std::clamp<long>(r0.y + dy, 0, std::max(0, H - r0.h)));
        break;
    }
    case Mode::Resize: {
        int left = r0.x, top = r0.y, right = r0.right(), bottom = r0.bottom();
        const int h = drag.handle;
        const int px = clampX(std::lround(d.x)), py = clampY(std::lround(d.y));
        if (h == 0 || h == 6 || h == 7) {
            left = std::min(px, right - 1);
        }
        if (h == 2 || h == 3 || h == 4) {
            right = std::max(px, left + 1);
        }
        if (h == 0 || h == 1 || h == 2) {
            top = std::min(py, bottom - 1);
        }
        if (h == 4 || h == 5 || h == 6) {
            bottom = std::max(py, top + 1);
        }
        // Keep the pivot on the same canvas point while the box changes.
        const double pxAbs = r0.x + drag.start.pivot.x * r0.w, pyAbs = r0.y + drag.start.pivot.y * r0.h;
        r = {left, top, right - left, bottom - top};
        drag.current.pivot = {(pxAbs - r.x) / r.w, (pyAbs - r.y) / r.h};
        break;
    }
    case Mode::Border: {
        // Whole pixels measured from the guide's own edge, never crossing the opposite guide.
        gas::SliceBorder& b = drag.current.border;
        const gas::SliceBorder& b0 = drag.start.border;
        switch (drag.handle) {
        case 0: b.left = std::clamp(static_cast<int>(std::lround(d.x - r.x)), 0, r.w - b0.right); break;
        case 1: b.top = std::clamp(static_cast<int>(std::lround(d.y - r.y)), 0, r.h - b0.bottom); break;
        case 2: b.right = std::clamp(static_cast<int>(std::lround(r.right() - d.x)), 0, r.w - b0.left); break;
        case 3: b.bottom = std::clamp(static_cast<int>(std::lround(r.bottom() - d.y)), 0, r.h - b0.top); break;
        default: break;
        }
        break;
    }
    case Mode::Pivot: {
        // Half-pixel steps; Shift snaps each axis to the nearest edge or centre.
        double fx = (std::round(d.x * 2.0) / 2.0 - r.x) / r.w;
        double fy = (std::round(d.y * 2.0) / 2.0 - r.y) / r.h;
        if (mods & Qt::ShiftModifier) {
            fx = std::round(fx * 2.0) / 2.0;
            fy = std::round(fy * 2.0) / 2.0;
        }
        drag.current.pivot = {std::clamp(fx, -4.0, 5.0), std::clamp(fy, -4.0, 5.0)};
        break;
    }
    }
}

void CanvasWidget::finishSliceDrag() {
    const SliceDrag drag = *sliceDrag_;
    sliceDrag_.reset();
    std::vector<gas::Slice> slices = document_->slices();
    using Mode = SliceDrag::Mode;
    bool changed = false;
    int select = drag.index;
    if (drag.mode == Mode::Create) {
        if (drag.current.rect.empty()) {
            update(); // just a click on empty canvas: deselected
            return;
        }
        std::vector<std::string> names;
        for (const gas::Slice& s : slices) {
            names.push_back(s.name);
        }
        gas::Slice fresh = drag.current;
        for (int n = static_cast<int>(slices.size());; ++n) {
            fresh.name = QStringLiteral("sprite_%1").arg(n, 2, 10, QLatin1Char('0')).toStdString();
            if (std::find(names.begin(), names.end(), fresh.name) == names.end()) {
                break;
            }
        }
        slices.push_back(fresh);
        changed = document_->setSlices(std::move(slices), "New Slice");
        select = static_cast<int>(document_->slices().size()) - 1;
    } else if (drag.index >= 0 && drag.index < static_cast<int>(slices.size()) &&
               !(drag.current == slices[static_cast<std::size_t>(drag.index)])) {
        slices[static_cast<std::size_t>(drag.index)] = drag.current;
        const char* label = drag.mode == Mode::Move     ? "Move Slice"
                            : drag.mode == Mode::Resize ? "Resize Slice"
                            : drag.mode == Mode::Border ? "9-Slice Border"
                                                        : "Slice Pivot";
        changed = document_->setSlices(std::move(slices), label);
    }
    if (changed) {
        setSelectedSlice(select);
        Q_EMIT documentEdited();
    }
    updateCursorShape();
}

void CanvasWidget::nudgeSlice(int dx, int dy) {
    std::vector<gas::Slice> slices = document_->slices();
    if (selectedSlice_ < 0 || selectedSlice_ >= static_cast<int>(slices.size())) {
        return;
    }
    gas::IRect& r = slices[static_cast<std::size_t>(selectedSlice_)].rect;
    r.x = std::clamp(r.x + dx, 0, std::max(0, document_->width() - r.w));
    r.y = std::clamp(r.y + dy, 0, std::max(0, document_->height() - r.h));
    if (document_->setSlices(std::move(slices), "Nudge Slice", true)) {
        Q_EMIT documentEdited();
    }
}

// Slice boxes with names, the selected one's handles and every pivot.
void CanvasWidget::drawSlices(QPainter& p) const {
    if (!document_ || (!showSlices_ && tool_ != Tool::Slice) || transform_) {
        return;
    }
    std::vector<gas::Slice> slices = document_->slices();
    int selected = selectedSlice_;
    if (sliceDrag_) {
        if (sliceDrag_->mode == SliceDrag::Mode::Create) {
            if (!sliceDrag_->current.rect.empty()) {
                slices.push_back(sliceDrag_->current);
                selected = static_cast<int>(slices.size()) - 1;
            }
        } else if (sliceDrag_->index >= 0 && sliceDrag_->index < static_cast<int>(slices.size())) {
            slices[static_cast<std::size_t>(sliceDrag_->index)] = sliceDrag_->current;
        }
    }
    const bool editing = tool_ == Tool::Slice;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont font = p.font();
    font.setPointSizeF(8.0);
    p.setFont(font);
    const QFontMetrics fm(font);
    for (int i = 0; i < static_cast<int>(slices.size()); ++i) {
        const gas::Slice& s = slices[static_cast<std::size_t>(i)];
        const QRectF r = sliceScreenRect(s.rect);
        const bool isSel = i == selected;
        const QColor colour = isSel ? QColor(255, 150, 20) : QColor(40, 190, 255);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 110), 3.0));
        p.drawRect(r);
        p.setPen(QPen(colour, 1.0));
        p.drawRect(r);
        // Name tag above the box when there is room.
        const QString name = QString::fromStdString(s.name);
        if (r.width() >= 24.0 && r.height() >= 10.0) {
            const QString text = fm.elidedText(name, Qt::ElideRight, std::max(24, static_cast<int>(r.width())));
            const QRectF tag(r.left(), r.top() - fm.height() - 2, fm.horizontalAdvance(text) + 6, fm.height() + 2);
            p.fillRect(tag, QColor(0, 0, 0, 150));
            p.setPen(colour);
            p.drawText(tag.adjusted(3, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
        // 9-slice guides (dashed), inside the box.
        if (!s.border.empty() && (editing || isSel)) {
            const double z = viewport_.zoom();
            QPen guide(QColor(255, 60, 200, isSel ? 230 : 140), 1.0, Qt::DashLine);
            p.setPen(guide);
            if (s.border.left > 0) {
                p.drawLine(QPointF(r.left() + s.border.left * z, r.top()), QPointF(r.left() + s.border.left * z, r.bottom()));
            }
            if (s.border.right > 0) {
                p.drawLine(QPointF(r.right() - s.border.right * z, r.top()),
                           QPointF(r.right() - s.border.right * z, r.bottom()));
            }
            if (s.border.top > 0) {
                p.drawLine(QPointF(r.left(), r.top() + s.border.top * z), QPointF(r.right(), r.top() + s.border.top * z));
            }
            if (s.border.bottom > 0) {
                p.drawLine(QPointF(r.left(), r.bottom() - s.border.bottom * z),
                           QPointF(r.right(), r.bottom() - s.border.bottom * z));
            }
        }
        if (editing && isSel) {
            p.setPen(QPen(Qt::black, 1.0));
            p.setBrush(Qt::white);
            for (const QPointF& h : handlePoints(r)) {
                p.drawRect(QRectF(h.x() - kSliceHandleRadius + 1, h.y() - kSliceHandleRadius + 1,
                                  2 * kSliceHandleRadius - 2, 2 * kSliceHandleRadius - 2));
            }
        }
        if (editing || isSel) {
            p.setBrush(Qt::NoBrush);
            // Pivot: crosshair in a circle, on top of the handles.
            const QPointF c = slicePivotScreen(s);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
            p.drawEllipse(c, 4.0, 4.0);
            p.setPen(QPen(isSel ? colour : QColor(255, 255, 255, 200), 1.5));
            p.drawEllipse(c, 4.0, 4.0);
            p.drawLine(c - QPointF(7, 0), c + QPointF(7, 0));
            p.drawLine(c - QPointF(0, 7), c + QPointF(0, 7));
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }
    p.restore();
}

// ---------------------------------------------------------------- onion skin

void CanvasWidget::setOnionSkin(bool on, int before, int after) {
    onion_ = on;
    onionBefore_ = std::clamp(before, 0, 10);
    onionAfter_ = std::clamp(after, 0, 10);
    update();
}

const QImage& CanvasWidget::onionImage(int frame, bool earlier) {
    if (document_->historyGeneration() != onionGeneration_) {
        onionCache_.clear(); // any edit, undo or frame change may alter other frames
        onionGeneration_ = document_->historyGeneration();
    }
    const int key = frame * 2 + (earlier ? 1 : 0);
    auto it = onionCache_.find(key);
    if (it != onionCache_.end()) {
        return *it;
    }
    const gas::TiledImage img = document_->flattenedFrame(frame);
    QImage out(img.width(), img.height(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const QColor tint = earlier ? QColor(255, 50, 50) : QColor(50, 110, 255);
    for (int ty = 0; ty < img.tilesY(); ++ty) {
        for (int tx = 0; tx < img.tilesX(); ++tx) {
            const gas::TiledImage::Tile* t = img.tile(tx, ty);
            if (!t) {
                continue;
            }
            const int w = std::min(kTile, img.width() - tx * kTile), h = std::min(kTile, img.height() - ty * kTile);
            for (int y = 0; y < h; ++y) {
                auto* line = reinterpret_cast<QRgb*>(out.scanLine(ty * kTile + y)) + tx * kTile;
                for (int x = 0; x < w; ++x) {
                    const gas::Rgba8 s = t->pixels[static_cast<std::size_t>(y * kTile + x)];
                    if (s.a == 0) {
                        continue;
                    }
                    // Half way towards the tint keeps the drawing readable.
                    const int r = (s.r + tint.red()) / 2, g = (s.g + tint.green()) / 2, b = (s.b + tint.blue()) / 2;
                    line[x] = qPremultiply(qRgba(r, g, b, s.a));
                }
            }
        }
    }
    return *onionCache_.insert(key, std::move(out));
}

void CanvasWidget::drawOnionSkin(QPainter& p, const QRectF& canvasRect, const QRectF& visible) {
    if (!onion_ || !document_ || document_->frameCount() < 2) {
        return;
    }
    const int active = document_->activeFrame();
    p.save();
    p.setClipRect(visible);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Farthest first, so nearer frames sit on top.
    for (int k = onionBefore_; k >= 1; --k) {
        if (active - k >= 0) {
            p.setOpacity(0.5 / k);
            p.drawImage(canvasRect, onionImage(active - k, true));
        }
    }
    for (int k = onionAfter_; k >= 1; --k) {
        if (active + k < document_->frameCount()) {
            p.setOpacity(0.5 / k);
            p.drawImage(canvasRect, onionImage(active + k, false));
        }
    }
    p.restore();
}
