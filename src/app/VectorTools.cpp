#include "VectorTools.h"

#include "CanvasWidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr double kNodeRadius = 6.0; // screen px for grabbing nodes and handles
const QColor kOutline(40, 190, 255);
const QColor kSelected(255, 150, 20);

gas::PointF lerp(gas::PointF a, gas::PointF b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
double dist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
double halfPixel(double v) { return std::round(v * 2.0) / 2.0; }
gas::PointF snapHalf(gas::PointF p) { return {halfPixel(p.x), halfPixel(p.y)}; }
gas::PointF snapWhole(gas::PointF p) { return {std::round(p.x), std::round(p.y)}; }
bool hasHandles(const gas::PathNode& n) { return !(n.in == n.p) || !(n.out == n.p); }

QColor qcolour(gas::Rgba8 c) { return QColor(c.r, c.g, c.b, c.a); }
gas::Rgba8 rgba(const QColor& c) {
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
}

gas::PointF bezier(gas::PointF p0, gas::PointF c1, gas::PointF c2, gas::PointF p3, double t) {
    const double u = 1 - t;
    return {u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p3.x,
            u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p3.y};
}

} // namespace

VectorEditor::VectorEditor(CanvasWidget* canvas) : QObject(canvas), canvas_(canvas) {
    settings_.style.fillColor = {70, 140, 230, 255};
    settings_.style.strokeColor = {20, 30, 50, 255};
}

void VectorEditor::setDocument(gas::Document* document) {
    document_ = document;
    pen_.reset();
    drag_ = Drag::None;
    layer_ = shape_ = node_ = hover_ = -1;
    Q_EMIT selectionChanged();
}

QPointF VectorEditor::screen(gas::PointF p) const { return canvas_->docToScreenPoint(p); }
gas::PointF VectorEditor::docAt(QPointF p) const { return canvas_->screenToDocPoint(p); }

const gas::VectorShapes* VectorEditor::shapes() const {
    if (!document_ || layer_ < 0 || !document_->isVectorLayer(layer_)) {
        return nullptr;
    }
    return document_->layer(layer_).vector.get();
}

int VectorEditor::selectedShape() const {
    const gas::VectorShapes* s = shapes();
    return s && layer_ == document_->activeLayerIndex() && shape_ >= 0 && shape_ < static_cast<int>(s->size())
               ? shape_
               : -1;
}

std::optional<gas::VectorStyle> VectorEditor::selectedStyle() const {
    const int i = selectedShape();
    if (i < 0) {
        return std::nullopt;
    }
    return (*shapes())[static_cast<std::size_t>(i)].style;
}

int VectorEditor::editableLayer() const {
    if (!document_) {
        return -1;
    }
    const int i = document_->activeLayerIndex();
    const gas::LayerProps& p = document_->layer(i).props;
    return document_->isVectorLayer(i) && p.visible && !p.locked ? i : -1;
}

void VectorEditor::select(int layer, int shape, int node) {
    const bool changed = layer != layer_ || shape != shape_;
    layer_ = layer;
    shape_ = shape;
    node_ = node;
    if (changed) {
        styleToken_ = nudgeToken_ = 0;
        Q_EMIT selectionChanged();
    }
}

bool VectorEditor::setShapes(gas::VectorShapes shapes, const std::string& label, int token) {
    if (layer_ < 0 || !document_->setVectorShapes(layer_, std::move(shapes), label, token)) {
        return false;
    }
    Q_EMIT edited();
    return true;
}

void VectorEditor::documentChanged() {
    if (busy()) {
        return;
    }
    const gas::VectorShapes* s = shapes();
    if (layer_ >= 0 && (!s || layer_ != document_->activeLayerIndex() || shape_ >= static_cast<int>(s->size()))) {
        select(-1, -1);
    } else if (s && shape_ >= 0 && node_ >= static_cast<int>((*s)[static_cast<std::size_t>(shape_)].path.nodes.size())) {
        node_ = -1;
    }
    hover_ = -1;
}

void VectorEditor::toolChanged() {
    finishPath();
    drag_ = Drag::None;
    hover_ = -1;
}

// ---------------------------------------------------------------- creating shapes

bool VectorEditor::commitShape(gas::VectorShape shape, const std::string& label) {
    if (!document_ || document_->isTransforming()) {
        return false;
    }
    const int active = document_->activeLayerIndex();
    if (const int layer = editableLayer(); layer >= 0) {
        gas::VectorShapes list = *document_->layer(layer).vector;
        list.push_back(std::move(shape));
        if (!document_->setVectorShapes(layer, std::move(list), label)) {
            return false;
        }
        select(layer, static_cast<int>(document_->layer(layer).vector->size()) - 1);
    } else {
        if (document_->isVectorLayer(active)) {
            const gas::LayerProps& p = document_->layer(active).props;
            Q_EMIT refused(p.locked ? tr("\"%1\" is locked").arg(QString::fromStdString(p.name))
                                    : tr("\"%1\" is hidden").arg(QString::fromStdString(p.name)));
            return false;
        }
        if (!document_->addVectorLayer({std::move(shape)}, label)) {
            return false;
        }
        select(document_->activeLayerIndex(), 0);
        Q_EMIT layerChanged();
    }
    Q_EMIT edited();
    return true;
}

gas::VectorShape VectorEditor::newShape(gas::PointF a, gas::PointF b, Qt::KeyboardModifiers mods) const {
    gas::VectorShape s;
    s.style = settings_.style;
    if (settings_.kind == ShapeKind::Line) {
        a = snapHalf(a);
        b = (mods & Qt::ShiftModifier) ? snapHalf(gas::snapAngle(a, b, 45.0)) : snapHalf(b);
        s.path.nodes = {gas::PathNode::corner(a), gas::PathNode::corner(b)};
        if (!s.style.stroke) { // a line needs a stroke: use the fill colour
            s.style.stroke = true;
            s.style.strokeColor = s.style.fillColor;
        }
        s.style.fill = false;
        return s;
    }
    // Box corners on whole pixels, so edges are crisp.
    a = snapWhole(a);
    b = snapWhole(b);
    double w = b.x - a.x, h = b.y - a.y;
    if (mods & Qt::ShiftModifier) { // square / circle
        const double side = std::max(std::abs(w), std::abs(h));
        w = w < 0 ? -side : side;
        h = h < 0 ? -side : side;
    }
    gas::RectF r{a.x, a.y, w, h};
    if (mods & Qt::AltModifier) { // from the centre
        r = {a.x - w, a.y - h, 2 * w, 2 * h};
    }
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    switch (settings_.kind) {
    case ShapeKind::Rectangle: s.path = gas::rectanglePath(r, settings_.cornerRadius); break;
    case ShapeKind::Ellipse: s.path = gas::ellipsePath(r); break;
    case ShapeKind::Polygon:
    case ShapeKind::Star: {
        const gas::PointF c{r.x + r.w / 2, r.y + r.h / 2};
        s.path = gas::polygonPath(c, std::min(r.w, r.h) / 2, settings_.sides,
                                  settings_.kind == ShapeKind::Star ? settings_.innerRatio : 1.0);
        break;
    }
    case ShapeKind::Line: break;
    }
    return s;
}

void VectorEditor::finishPath() {
    if (!pen_) {
        return;
    }
    gas::VectorPath path = std::move(*pen_);
    pen_.reset();
    drag_ = Drag::None;
    if (path.nodes.size() >= 2) {
        gas::VectorShape s;
        s.path = std::move(path);
        s.style = settings_.style;
        if (!s.path.closed && !s.style.stroke && s.path.nodes.size() == 2) {
            s.style.stroke = true; // a two-node open path would be invisible
            s.style.strokeColor = s.style.fillColor;
            s.style.fill = false;
        }
        commitShape(std::move(s), "Pen Path");
    }
    canvas_->update();
}

void VectorEditor::cancel() {
    pen_.reset();
    if (drag_ == Drag::Create) {
        drag_ = Drag::None;
    }
    canvas_->update();
}

// ---------------------------------------------------------------- node editing helpers

int VectorEditor::nodeAt(QPointF pos) const {
    const int i = selectedShape();
    if (i < 0) {
        return -1;
    }
    const gas::VectorPath& path = (*shapes())[static_cast<std::size_t>(i)].path;
    int best = -1;
    double bestD = kNodeRadius;
    for (int n = 0; n < static_cast<int>(path.nodes.size()); ++n) {
        const double d = dist(screen(path.nodes[static_cast<std::size_t>(n)].p), pos);
        if (d <= bestD) {
            best = n;
            bestD = d;
        }
    }
    return best;
}

int VectorEditor::handleAt(QPointF pos, bool* isOut) const {
    const int i = selectedShape();
    if (i < 0) {
        return -1;
    }
    const gas::VectorPath& path = (*shapes())[static_cast<std::size_t>(i)].path;
    // The selected node's handles and the facing handles of its neighbours.
    const int n = static_cast<int>(path.nodes.size());
    std::vector<std::pair<int, bool>> candidates;
    if (node_ >= 0 && node_ < n) {
        candidates = {{node_, false}, {node_, true}};
        if (path.closed || node_ > 0) {
            candidates.push_back({(node_ + n - 1) % n, true});
        }
        if (path.closed || node_ + 1 < n) {
            candidates.push_back({(node_ + 1) % n, false});
        }
    }
    for (const auto& [k, out] : candidates) {
        const gas::PathNode& node = path.nodes[static_cast<std::size_t>(k)];
        const gas::PointF h = out ? node.out : node.in;
        if (!(h == node.p) && dist(screen(h), pos) <= kNodeRadius) {
            *isOut = out;
            return k;
        }
    }
    return -1;
}

void VectorEditor::toggleSmooth(int node) {
    const int i = selectedShape();
    if (i < 0) {
        return;
    }
    gas::VectorShapes list = *shapes();
    gas::VectorPath& path = list[static_cast<std::size_t>(i)].path;
    const int n = static_cast<int>(path.nodes.size());
    gas::PathNode& nd = path.nodes[static_cast<std::size_t>(node)];
    if (hasHandles(nd)) {
        nd.in = nd.out = nd.p; // corner
    } else {
        // Smooth: handles along the line between the neighbours, a third of the way.
        const gas::PointF prev = path.nodes[static_cast<std::size_t>((node + n - 1) % n)].p;
        const gas::PointF next = path.nodes[static_cast<std::size_t>((node + 1) % n)].p;
        const bool hasPrev = path.closed || node > 0, hasNext = path.closed || node + 1 < n;
        const gas::PointF a = hasPrev ? prev : nd.p, b = hasNext ? next : nd.p;
        const gas::PointF t{(b.x - a.x) / 6, (b.y - a.y) / 6};
        nd.in = {nd.p.x - t.x, nd.p.y - t.y};
        nd.out = {nd.p.x + t.x, nd.p.y + t.y};
    }
    setShapes(std::move(list), "Convert Node", 0);
}

bool VectorEditor::insertNodeAt(QPointF pos) {
    const int i = selectedShape();
    if (i < 0) {
        return false;
    }
    gas::VectorShapes list = *shapes();
    gas::VectorPath& path = list[static_cast<std::size_t>(i)].path;
    const std::size_t n = path.nodes.size();
    const std::size_t segments = path.closed ? n : (n > 0 ? n - 1 : 0);
    double bestD = kNodeRadius * 1.5, bestT = 0;
    std::size_t bestSeg = segments;
    for (std::size_t s = 0; s < segments; ++s) {
        const gas::PathNode& a = path.nodes[s];
        const gas::PathNode& b = path.nodes[(s + 1) % n];
        for (int k = 1; k < 64; ++k) {
            const double t = k / 64.0;
            const double d = dist(screen(bezier(a.p, a.out, b.in, b.p, t)), pos);
            if (d < bestD) {
                bestD = d;
                bestT = t;
                bestSeg = s;
            }
        }
    }
    if (bestSeg == segments) {
        return false;
    }
    gas::PathNode& a = path.nodes[bestSeg];
    gas::PathNode& b = path.nodes[(bestSeg + 1) % n];
    gas::PathNode mid;
    if (a.out == a.p && b.in == b.p) {
        mid = gas::PathNode::corner(lerp(a.p, b.p, bestT));
    } else { // de Casteljau split keeps the curve's shape
        const gas::PointF p01 = lerp(a.p, a.out, bestT), p12 = lerp(a.out, b.in, bestT), p23 = lerp(b.in, b.p, bestT);
        const gas::PointF p012 = lerp(p01, p12, bestT), p123 = lerp(p12, p23, bestT);
        mid = {lerp(p012, p123, bestT), p012, p123};
        a.out = p01;
        b.in = p23;
    }
    path.nodes.insert(path.nodes.begin() + static_cast<std::ptrdiff_t>(bestSeg + 1), mid);
    if (setShapes(std::move(list), "Add Node", 0)) {
        node_ = static_cast<int>(bestSeg + 1);
        return true;
    }
    return false;
}

bool VectorEditor::deleteSelection() {
    const int i = selectedShape();
    if (i < 0 || busy()) {
        return false;
    }
    gas::VectorShapes list = *shapes();
    gas::VectorPath& path = list[static_cast<std::size_t>(i)].path;
    const std::size_t minNodes = path.closed ? 3 : 2;
    if (node_ >= 0 && path.nodes.size() > minNodes) {
        path.nodes.erase(path.nodes.begin() + node_);
        node_ = -1;
        return setShapes(std::move(list), "Delete Node", 0);
    }
    list.erase(list.begin() + i);
    const int layer = layer_;
    if (document_->setVectorShapes(layer, std::move(list), "Delete Shape")) {
        select(-1, -1);
        Q_EMIT edited();
        return true;
    }
    return false;
}

void VectorEditor::raiseSelection(int steps) {
    const int i = selectedShape();
    if (i < 0 || busy()) {
        return;
    }
    gas::VectorShapes list = *shapes();
    const int to = std::clamp(i + steps, 0, static_cast<int>(list.size()) - 1);
    if (to == i) {
        return;
    }
    gas::VectorShape s = list[static_cast<std::size_t>(i)];
    list.erase(list.begin() + i);
    list.insert(list.begin() + to, std::move(s));
    if (setShapes(std::move(list), steps > 0 ? "Bring Shape Forward" : "Send Shape Backward", 0)) {
        shape_ = to;
    }
}

void VectorEditor::applyStyle(const gas::VectorStyle& style) {
    const int i = selectedShape();
    if (i < 0 || busy()) {
        return;
    }
    gas::VectorShapes list = *shapes();
    list[static_cast<std::size_t>(i)].style = style;
    if (styleToken_ == 0) {
        styleToken_ = nextToken_++;
    }
    setShapes(std::move(list), "Shape Style", styleToken_);
}

// ---------------------------------------------------------------- events

bool VectorEditor::press(Tool tool, QPointF pos, Qt::KeyboardModifiers mods) {
    if (!document_) {
        return false;
    }
    const gas::PointF d = docAt(pos);
    pressMods_ = mods;
    moved_ = false;
    nudgeToken_ = 0;
    if (tool == Tool::Pen) {
        if (pen_ && pen_->nodes.size() >= 2 && dist(screen(pen_->nodes.front().p), pos) <= kNodeRadius + 1) {
            pen_->closed = true;
            finishPath();
            return true;
        }
        if (!pen_) {
            pen_ = gas::VectorPath{};
        }
        gas::PointF at = snapHalf(d);
        if ((mods & Qt::ShiftModifier) && !pen_->nodes.empty()) {
            at = snapHalf(gas::snapAngle(pen_->nodes.back().p, d, 45.0));
        }
        pen_->nodes.push_back(gas::PathNode::corner(at));
        drag_ = Drag::Pen;
        startDoc_ = at;
        return true;
    }
    if (tool == Tool::Shape) {
        drag_ = Drag::Create;
        startDoc_ = d;
        createEnd_ = d;
        createMods_ = mods;
        return true;
    }
    if (tool != Tool::Node) {
        return false;
    }
    // Node tool: handles, then nodes of the selected shape, then shapes.
    token_ = nextToken_++;
    if (selectedShape() >= 0) {
        startShapes_ = *shapes();
        startDoc_ = d;
        bool out = false;
        if (const int h = handleAt(pos, &out); h >= 0) {
            node_ = h;
            handleOut_ = out;
            breakHandles_ = (mods & Qt::AltModifier) != 0;
            drag_ = Drag::Handle;
            return true;
        }
        if (const int n = nodeAt(pos); n >= 0) {
            node_ = n;
            if (mods & Qt::AltModifier) { // Alt+drag a node: pull out new smooth handles
                handleOut_ = true;
                breakHandles_ = false;
                drag_ = Drag::Handle;
            } else {
                drag_ = Drag::Node;
            }
            return true;
        }
    }
    // Pick a shape: the active vector layer first, then any visible vector layer from the top.
    std::vector<int> layers;
    if (document_->isVectorLayer(document_->activeLayerIndex())) {
        layers.push_back(document_->activeLayerIndex());
    }
    for (int l = document_->layerCount() - 1; l >= 0; --l) {
        if (l != document_->activeLayerIndex() && document_->isVectorLayer(l)) {
            layers.push_back(l);
        }
    }
    const double tolerance = kNodeRadius / std::max(0.01, canvas_->zoom());
    for (int l : layers) {
        const gas::Layer& layer = document_->layer(l);
        if (!layer.props.visible) {
            continue;
        }
        const int hit = gas::hitTestShapes(*layer.vector, d, tolerance);
        if (hit < 0) {
            continue;
        }
        if (l != document_->activeLayerIndex()) {
            document_->setActiveLayer(l);
            Q_EMIT layerChanged();
        }
        select(l, hit, -1);
        if (!layer.props.locked) {
            startShapes_ = *layer.vector;
            startDoc_ = d;
            drag_ = Drag::Shape;
        }
        return true;
    }
    select(-1, -1);
    return true;
}

void VectorEditor::move(Tool tool, QPointF pos, Qt::KeyboardModifiers mods, bool leftDown) {
    if (!document_) {
        return;
    }
    hoverPos_ = pos;
    const gas::PointF d = docAt(pos);
    if (!leftDown || drag_ == Drag::None) {
        if (tool == Tool::Node && drag_ == Drag::None) {
            const int active = document_->activeLayerIndex();
            hover_ = document_->isVectorLayer(active)
                         ? gas::hitTestShapes(*document_->layer(active).vector, d,
                                              kNodeRadius / std::max(0.01, canvas_->zoom()))
                         : -1;
        }
        return;
    }
    if (dist(screen(startDoc_), pos) > 2.0) {
        moved_ = true;
    }
    switch (drag_) {
    case Drag::Pen: {
        if (!pen_ || !moved_) {
            return;
        }
        gas::PathNode& n = pen_->nodes.back();
        n.out = d;
        n.in = {2 * n.p.x - d.x, 2 * n.p.y - d.y};
        return;
    }
    case Drag::Create:
        createEnd_ = d;
        createMods_ = mods;
        return;
    case Drag::Shape: {
        if (!moved_) {
            return;
        }
        double dx = std::round(d.x - startDoc_.x), dy = std::round(d.y - startDoc_.y); // whole pixels
        if (mods & Qt::ShiftModifier) {
            (std::abs(dx) > std::abs(dy) ? dy : dx) = 0; // axis lock
        }
        gas::VectorShapes list = startShapes_;
        gas::VectorShape& s = list[static_cast<std::size_t>(shape_)];
        s = gas::transformedShape(s, gas::Affine::translate(dx, dy));
        s.style.strokeWidth = startShapes_[static_cast<std::size_t>(shape_)].style.strokeWidth; // exact
        setShapes(std::move(list), "Move Shape", token_);
        return;
    }
    case Drag::Node: {
        if (!moved_) {
            return;
        }
        gas::VectorShapes list = startShapes_;
        gas::PathNode& n = list[static_cast<std::size_t>(shape_)].path.nodes[static_cast<std::size_t>(node_)];
        const gas::PointF to = snapHalf({n.p.x + d.x - startDoc_.x, n.p.y + d.y - startDoc_.y});
        const double dx = to.x - n.p.x, dy = to.y - n.p.y;
        n = {to, {n.in.x + dx, n.in.y + dy}, {n.out.x + dx, n.out.y + dy}};
        setShapes(std::move(list), "Move Node", token_);
        return;
    }
    case Drag::Handle: {
        if (!moved_) {
            return;
        }
        gas::VectorShapes list = startShapes_;
        gas::PathNode& n = list[static_cast<std::size_t>(shape_)].path.nodes[static_cast<std::size_t>(node_)];
        gas::PointF& h = handleOut_ ? n.out : n.in;
        gas::PointF& other = handleOut_ ? n.in : n.out;
        h = d;
        const bool pulling = !hasHandles(startShapes_[static_cast<std::size_t>(shape_)]
                                             .path.nodes[static_cast<std::size_t>(node_)]);
        if (pulling) {
            other = {2 * n.p.x - d.x, 2 * n.p.y - d.y}; // new symmetric handles
        } else if (!breakHandles_ && !(other == n.p)) {
            // Smooth node: keep the opposite handle collinear (its own length).
            const double len = std::hypot(other.x - n.p.x, other.y - n.p.y);
            const double hx = d.x - n.p.x, hy = d.y - n.p.y, hl = std::hypot(hx, hy);
            if (hl > 1e-9) {
                other = {n.p.x - hx / hl * len, n.p.y - hy / hl * len};
            }
        }
        setShapes(std::move(list), "Edit Handle", token_);
        return;
    }
    case Drag::None: return;
    }
}

void VectorEditor::release(Tool tool, QPointF, Qt::KeyboardModifiers) {
    const Drag drag = drag_;
    drag_ = Drag::None;
    if (drag == Drag::Create && tool == Tool::Shape) {
        gas::VectorShape s = newShape(startDoc_, createEnd_, createMods_);
        const gas::RectF b = gas::pathControlBounds(s.path);
        if (std::max(b.w, b.h) >= 1.0) {
            commitShape(std::move(s), "New Shape");
        }
    } else if (drag == Drag::Handle && !moved_ && (pressMods_ & Qt::AltModifier)) {
        toggleSmooth(node_); // Alt+click on a node
    }
    canvas_->update();
}

bool VectorEditor::doubleClick(Tool tool, QPointF pos) {
    if (tool == Tool::Pen && pen_) {
        finishPath(); // the first click of the double-click placed the last node
        return true;
    }
    if (tool == Tool::Node && selectedShape() >= 0 && nodeAt(pos) < 0) {
        return insertNodeAt(pos);
    }
    return false;
}

bool VectorEditor::key(Tool tool, QKeyEvent* event) {
    if (tool == Tool::Pen && pen_) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter: finishPath(); return true;
        case Qt::Key_Escape: cancel(); return true;
        case Qt::Key_Backspace:
            pen_->nodes.pop_back();
            if (pen_->nodes.empty()) {
                pen_.reset();
            }
            canvas_->update();
            return true;
        default: return false;
        }
    }
    if (tool == Tool::Shape && drag_ == Drag::Create && event->key() == Qt::Key_Escape) {
        cancel();
        return true;
    }
    if (tool != Tool::Node || busy()) {
        return false;
    }
    const int i = selectedShape();
    if (event->key() == Qt::Key_Escape && i >= 0) {
        select(-1, -1);
        canvas_->update();
        return true;
    }
    if (i < 0) {
        return false;
    }
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        deleteSelection();
        return true;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_BracketRight) {
            raiseSelection(1);
            return true;
        }
        if (event->key() == Qt::Key_BracketLeft) {
            raiseSelection(-1);
            return true;
        }
    }
    const double step = (event->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    double dx = 0, dy = 0;
    switch (event->key()) {
    case Qt::Key_Left: dx = -step; break;
    case Qt::Key_Right: dx = step; break;
    case Qt::Key_Up: dy = -step; break;
    case Qt::Key_Down: dy = step; break;
    default: return false;
    }
    gas::VectorShapes list = *shapes();
    gas::VectorShape& s = list[static_cast<std::size_t>(i)];
    const double width = s.style.strokeWidth;
    if (node_ >= 0) {
        gas::PathNode& n = s.path.nodes[static_cast<std::size_t>(node_)];
        n = {{n.p.x + dx, n.p.y + dy}, {n.in.x + dx, n.in.y + dy}, {n.out.x + dx, n.out.y + dy}};
    } else {
        s = gas::transformedShape(s, gas::Affine::translate(dx, dy));
        s.style.strokeWidth = width;
    }
    if (nudgeToken_ == 0) {
        nudgeToken_ = nextToken_++;
    }
    setShapes(std::move(list), "Nudge Shape", nudgeToken_);
    return true;
}

Qt::CursorShape VectorEditor::cursor(Tool tool) const {
    if (tool == Tool::Node) {
        if (drag_ == Drag::Shape) {
            return Qt::SizeAllCursor;
        }
        if (hoverPos_) {
            bool out = false;
            if (nodeAt(*hoverPos_) >= 0 || handleAt(*hoverPos_, &out) >= 0) {
                return Qt::PointingHandCursor;
            }
        }
        return hover_ >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor;
    }
    return Qt::CrossCursor;
}

// ---------------------------------------------------------------- drawing

namespace {

QPainterPath screenPath(const gas::VectorPath& path, const std::function<QPointF(gas::PointF)>& map) {
    QPainterPath q;
    q.setFillRule(Qt::WindingFill);
    const std::size_t n = path.nodes.size();
    if (n == 0) {
        return q;
    }
    q.moveTo(map(path.nodes[0].p));
    const std::size_t segments = path.closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const gas::PathNode& a = path.nodes[i];
        const gas::PathNode& b = path.nodes[(i + 1) % n];
        if (a.out == a.p && b.in == b.p) {
            q.lineTo(map(b.p));
        } else {
            q.cubicTo(map(a.out), map(b.in), map(b.p));
        }
    }
    if (path.closed) {
        q.closeSubpath();
    }
    return q;
}

} // namespace

void VectorEditor::drawShape(QPainter& p, const gas::VectorShape& shape, double opacity) const {
    const auto map = [this](gas::PointF d) { return screen(d); };
    const QPainterPath q = screenPath(shape.path, map);
    p.save();
    p.setOpacity(opacity);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (shape.style.fill && shape.path.nodes.size() >= 3) {
        QPainterPath closed = q;
        closed.closeSubpath();
        p.fillPath(closed, qcolour(shape.style.fillColor));
    }
    if (shape.style.stroke) {
        static constexpr Qt::PenJoinStyle kJoins[] = {Qt::MiterJoin, Qt::RoundJoin, Qt::BevelJoin};
        static constexpr Qt::PenCapStyle kCaps[] = {Qt::FlatCap, Qt::RoundCap, Qt::SquareCap};
        QPen pen(qcolour(shape.style.strokeColor), shape.style.strokeWidth * canvas_->zoom());
        pen.setJoinStyle(kJoins[static_cast<int>(shape.style.join)]);
        pen.setCapStyle(kCaps[static_cast<int>(shape.style.cap)]);
        pen.setMiterLimit(gas::kMiterLimit);
        p.strokePath(q, pen);
    }
    p.restore();
}

void VectorEditor::drawNodes(QPainter& p, const gas::VectorPath& path, int selectedNode) const {
    const int n = static_cast<int>(path.nodes.size());
    p.setRenderHint(QPainter::Antialiasing, true);
    // Handles of the selected node and the facing handles of its neighbours.
    const auto drawHandle = [&](const gas::PathNode& node, bool out) {
        const gas::PointF h = out ? node.out : node.in;
        if (h == node.p) {
            return;
        }
        p.setPen(QPen(kOutline, 1.0));
        p.drawLine(screen(node.p), screen(h));
        p.setBrush(Qt::white);
        p.drawEllipse(screen(h), 3.5, 3.5);
    };
    if (selectedNode >= 0 && selectedNode < n) {
        const gas::PathNode& node = path.nodes[static_cast<std::size_t>(selectedNode)];
        drawHandle(node, false);
        drawHandle(node, true);
        if (path.closed || selectedNode > 0) {
            drawHandle(path.nodes[static_cast<std::size_t>((selectedNode + n - 1) % n)], true);
        }
        if (path.closed || selectedNode + 1 < n) {
            drawHandle(path.nodes[static_cast<std::size_t>((selectedNode + 1) % n)], false);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int i = 0; i < n; ++i) {
        const QPointF c = screen(path.nodes[static_cast<std::size_t>(i)].p);
        p.setPen(QPen(Qt::black, 1.0));
        p.setBrush(i == selectedNode ? kSelected : QColor(Qt::white));
        p.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
    }
}

void VectorEditor::paint(QPainter& p, Tool tool) const {
    if (!document_) {
        return;
    }
    p.save();
    const auto map = [this](gas::PointF d) { return screen(d); };
    if (tool == Tool::Node || tool == Tool::Pen || tool == Tool::Shape) {
        const int active = document_->activeLayerIndex();
        if (tool == Tool::Node && hover_ >= 0 && document_->isVectorLayer(active) &&
            hover_ < static_cast<int>(document_->layer(active).vector->size()) && hover_ != selectedShape()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(kOutline, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawPath(screenPath((*document_->layer(active).vector)[static_cast<std::size_t>(hover_)].path, map));
        }
        if (const int i = selectedShape(); i >= 0) {
            const gas::VectorPath& path = (*shapes())[static_cast<std::size_t>(i)].path;
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 120), 3.0));
            p.drawPath(screenPath(path, map));
            p.setPen(QPen(kSelected, 1.0));
            p.drawPath(screenPath(path, map));
            if (tool == Tool::Node) {
                drawNodes(p, path, node_);
            }
        }
    }
    if (tool == Tool::Pen && pen_) {
        gas::VectorShape preview;
        preview.path = *pen_;
        preview.style = settings_.style;
        drawShape(p, preview, 0.8);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(kOutline, 1.0));
        p.drawPath(screenPath(*pen_, map));
        if (hoverPos_ && drag_ != Drag::Pen && !pen_->nodes.empty()) {
            QPen rubber(kOutline, 1.0, Qt::DashLine);
            p.setPen(rubber);
            p.drawLine(screen(pen_->nodes.back().p), *hoverPos_);
        }
        drawNodes(p, *pen_, static_cast<int>(pen_->nodes.size()) - 1);
        if (pen_->nodes.size() >= 2 && hoverPos_ && dist(screen(pen_->nodes.front().p), *hoverPos_) <= kNodeRadius + 1) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(kSelected, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(screen(pen_->nodes.front().p), 7.0, 7.0); // "click to close"
        }
    }
    if (tool == Tool::Shape && drag_ == Drag::Create) {
        const gas::VectorShape s = newShape(startDoc_, createEnd_, createMods_);
        drawShape(p, s, 0.8);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(kOutline, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(screenPath(s.path, map));
    }
    p.restore();
}

// ---------------------------------------------------------------- options bar

namespace {

void paintSwatch(QToolButton* b, gas::Rgba8 c, bool enabled) {
    QPixmap pm(22, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    // Checkerboard under translucent colours.
    for (int y = 0; y < 16; y += 4) {
        for (int x = 0; x < 22; x += 4) {
            p.fillRect(x, y, 4, 4, ((x + y) / 4) % 2 ? QColor(200, 200, 200) : QColor(255, 255, 255));
        }
    }
    p.fillRect(pm.rect(), qcolour(c));
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    if (!enabled) {
        p.setPen(QPen(Qt::red, 2));
        p.drawLine(2, 14, 20, 2);
    }
    p.end();
    b->setIcon(QIcon(pm));
    b->setIconSize(pm.size());
}

} // namespace

VectorOptions::VectorOptions(VectorEditor* editor, QWidget* parent) : QWidget(parent), editor_(editor) {
    style_ = editor_->settings().style;
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    shapeGroup_ = new QWidget(this);
    auto* shapeRow = new QHBoxLayout(shapeGroup_);
    shapeRow->setContentsMargins(0, 0, 8, 0);
    shapeRow->setSpacing(4);
    kind_ = new QComboBox(shapeGroup_);
    kind_->addItems({tr("Rectangle"), tr("Ellipse"), tr("Polygon"), tr("Star"), tr("Line")});
    kind_->setFocusPolicy(Qt::NoFocus);
    radiusLabel_ = new QLabel(tr("Radius"), shapeGroup_);
    radius_ = new QSpinBox(shapeGroup_);
    radius_->setRange(0, 4096);
    radius_->setSuffix(tr(" px"));
    radius_->setToolTip(tr("Corner radius of new rectangles"));
    sidesLabel_ = new QLabel(tr("Sides"), shapeGroup_);
    sides_ = new QSpinBox(shapeGroup_);
    sides_->setRange(3, 64);
    sides_->setValue(editor_->settings().sides);
    innerLabel_ = new QLabel(tr("Inner"), shapeGroup_);
    inner_ = new QSpinBox(shapeGroup_);
    inner_->setRange(5, 95);
    inner_->setSuffix(tr("%"));
    inner_->setValue(static_cast<int>(editor_->settings().innerRatio * 100));
    inner_->setToolTip(tr("Inner radius of star points"));
    for (QWidget* w : std::initializer_list<QWidget*>{kind_, radiusLabel_, radius_, sidesLabel_, sides_, innerLabel_, inner_}) {
        shapeRow->addWidget(w);
    }
    for (QSpinBox* b : {radius_, sides_, inner_}) {
        b->setFocusPolicy(Qt::ClickFocus);
    }
    row->addWidget(shapeGroup_);

    fill_ = new QCheckBox(tr("Fill"), this);
    fillColour_ = new QToolButton(this);
    fillColour_->setToolTip(tr("Fill colour"));
    stroke_ = new QCheckBox(tr("Stroke"), this);
    strokeColour_ = new QToolButton(this);
    strokeColour_->setToolTip(tr("Stroke colour"));
    width_ = new QDoubleSpinBox(this);
    width_->setRange(0.25, 500.0);
    width_->setSingleStep(0.5);
    width_->setDecimals(2);
    width_->setSuffix(tr(" px"));
    width_->setToolTip(tr("Stroke width"));
    width_->setKeyboardTracking(false);
    join_ = new QComboBox(this);
    join_->addItems({tr("Miter"), tr("Round"), tr("Bevel")});
    join_->setToolTip(tr("How stroke corners are drawn"));
    antialias_ = new QCheckBox(tr("Anti-alias"), this);
    antialias_->setToolTip(tr("Off = hard pixel edges (pixel art)"));
    for (QWidget* w : std::initializer_list<QWidget*>{fill_, stroke_, antialias_, fillColour_, strokeColour_, join_}) {
        w->setFocusPolicy(Qt::NoFocus);
    }
    width_->setFocusPolicy(Qt::ClickFocus);
    for (QWidget* w :
         std::initializer_list<QWidget*>{fill_, fillColour_, stroke_, strokeColour_, width_, join_, antialias_}) {
        row->addWidget(w);
    }

    connect(kind_, &QComboBox::currentIndexChanged, this, [this](int i) {
        editor_->settings().kind = static_cast<ShapeKind>(i);
        setTool(Tool::Shape);
    });
    connect(radius_, &QSpinBox::valueChanged, this, [this](int v) { editor_->settings().cornerRadius = v; });
    connect(sides_, &QSpinBox::valueChanged, this, [this](int v) { editor_->settings().sides = v; });
    connect(inner_, &QSpinBox::valueChanged, this, [this](int v) { editor_->settings().innerRatio = v / 100.0; });
    connect(fill_, &QCheckBox::toggled, this, [this](bool on) {
        style_.fill = on;
        pushStyle();
    });
    connect(stroke_, &QCheckBox::toggled, this, [this](bool on) {
        style_.stroke = on;
        pushStyle();
    });
    connect(antialias_, &QCheckBox::toggled, this, [this](bool on) {
        style_.antialias = on;
        pushStyle();
    });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        style_.strokeWidth = v;
        pushStyle();
    });
    connect(join_, &QComboBox::currentIndexChanged, this, [this](int i) {
        style_.join = static_cast<gas::LineJoin>(i);
        pushStyle();
    });
    const auto pick = [this](bool fill) {
        const QColor chosen = QColorDialog::getColor(qcolour(fill ? style_.fillColor : style_.strokeColor), this,
                                                     fill ? tr("Fill Colour") : tr("Stroke Colour"),
                                                     QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) {
            return;
        }
        (fill ? style_.fillColor : style_.strokeColor) = rgba(chosen);
        (fill ? style_.fill : style_.stroke) = true; // picking a colour turns it on
        syncFromSelection();
        pushStyle();
    };
    connect(fillColour_, &QToolButton::clicked, this, [pick] { pick(true); });
    connect(strokeColour_, &QToolButton::clicked, this, [pick] { pick(false); });
    connect(editor_, &VectorEditor::selectionChanged, this, [this] {
        if (const auto s = editor_->selectedStyle()) {
            style_ = *s;
        }
        syncFromSelection();
    });
    syncFromSelection();
}

void VectorOptions::setTool(Tool tool) {
    shapeGroup_->setVisible(tool == Tool::Shape);
    const auto kind = static_cast<ShapeKind>(kind_->currentIndex());
    radiusLabel_->setVisible(kind == ShapeKind::Rectangle);
    radius_->setVisible(kind == ShapeKind::Rectangle);
    sidesLabel_->setVisible(kind == ShapeKind::Polygon || kind == ShapeKind::Star);
    sides_->setVisible(kind == ShapeKind::Polygon || kind == ShapeKind::Star);
    innerLabel_->setVisible(kind == ShapeKind::Star);
    inner_->setVisible(kind == ShapeKind::Star);
}

void VectorOptions::syncFromSelection() {
    const QSignalBlocker b1(fill_), b2(stroke_), b3(width_), b4(join_), b5(antialias_);
    fill_->setChecked(style_.fill);
    stroke_->setChecked(style_.stroke);
    width_->setValue(style_.strokeWidth);
    join_->setCurrentIndex(static_cast<int>(style_.join));
    antialias_->setChecked(style_.antialias);
    updateSwatches();
}

void VectorOptions::updateSwatches() {
    paintSwatch(fillColour_, style_.fillColor, style_.fill);
    paintSwatch(strokeColour_, style_.strokeColor, style_.stroke);
}

void VectorOptions::pushStyle() {
    updateSwatches();
    editor_->settings().style = style_; // new shapes use it
    editor_->applyStyle(style_);        // and the selected shape takes it
}
