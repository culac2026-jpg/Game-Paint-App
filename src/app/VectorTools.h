#pragma once

#include <gas/core/Document.h>
#include <gas/core/Vector.h>

#include <QObject>
#include <QPointF>
#include <QWidget>

#include <optional>

class CanvasWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QKeyEvent;
class QLabel;
class QPainter;
class QSpinBox;
class QToolButton;
enum class Tool;

// Shapes made by the Shape tool. Append only (kept in settings).
enum class ShapeKind { Rectangle, Ellipse, Polygon, Star, Line };

struct VectorToolSettings {
    gas::VectorStyle style;       // used for new shapes
    ShapeKind kind = ShapeKind::Rectangle;
    double cornerRadius = 0.0;    // rectangles
    int sides = 5;                // polygons and stars
    double innerRatio = 0.5;      // stars
};

// Pen, Shape and Node tools on the canvas. New shapes go into the active
// vector layer, or a new vector layer is added above the active raster layer.
//   Pen:   click = corner node, drag = smooth node; click the first node to
//          close, Enter or double-click finishes an open path, Backspace removes
//          the last node, Esc cancels; Shift snaps to 45 degrees.
//   Shape: drag a box (Shift = square / 45-degree line, Alt = from the centre).
//   Node:  click a shape to select it (any visible vector layer) and drag it;
//          drag nodes and handles; Alt+click a node toggles corner/smooth, Alt+drag
//          a handle breaks the pair; double-click the outline inserts a node;
//          Delete removes the node (or the shape), arrows nudge, Ctrl+[ / Ctrl+]
//          change the stacking order, Esc deselects.
class VectorEditor : public QObject {
    Q_OBJECT

public:
    explicit VectorEditor(CanvasWidget* canvas);

    void setDocument(gas::Document* document);
    VectorToolSettings& settings() { return settings_; }

    // The selected shape of the active vector layer, if any.
    int selectedShape() const;
    std::optional<gas::VectorStyle> selectedStyle() const;
    // Restyles the selected shape (consecutive calls merge into one undo step).
    void applyStyle(const gas::VectorStyle& style);
    bool deleteSelection();
    void raiseSelection(int steps);

    // A drag is in progress (the document must not change underneath).
    bool busy() const { return drag_ != Drag::None; }
    bool hasPendingPath() const { return pen_.has_value(); }
    void finishPath(); // commits the Pen path (if it has 2+ nodes)
    void cancel();     // drops the Pen path / a Shape drag
    void toolChanged();
    // After undo/redo or other document edits: drops a stale selection.
    void documentChanged();

    bool press(Tool tool, QPointF pos, Qt::KeyboardModifiers mods);
    void move(Tool tool, QPointF pos, Qt::KeyboardModifiers mods, bool leftDown);
    void release(Tool tool, QPointF pos, Qt::KeyboardModifiers mods);
    bool doubleClick(Tool tool, QPointF pos);
    bool key(Tool tool, QKeyEvent* event);
    void paint(QPainter& p, Tool tool) const;
    Qt::CursorShape cursor(Tool tool) const;

Q_SIGNALS:
    void edited();           // an undoable change was made
    void selectionChanged(); // the selected shape changed (options bar shows its style)
    void layerChanged();     // the active layer was switched (picking a shape in another layer)
    void refused(const QString& reason);

private:
    enum class Drag { None, Shape, Node, Handle, Pen, Create };

    int editableLayer() const; // active vector layer if visible and unlocked, else -1
    bool commitShape(gas::VectorShape shape, const std::string& label);
    gas::VectorShape newShape(gas::PointF a, gas::PointF b, Qt::KeyboardModifiers mods) const;
    void select(int layer, int shape, int node = -1);
    bool setShapes(gas::VectorShapes shapes, const std::string& label, int token);
    const gas::VectorShapes* shapes() const;
    QPointF screen(gas::PointF p) const;
    gas::PointF docAt(QPointF p) const;
    int nodeAt(QPointF pos) const;                     // node of the selected shape near pos
    int handleAt(QPointF pos, bool* isOut) const;      // handle of the selected node near pos
    void toggleSmooth(int node);
    bool insertNodeAt(QPointF pos);
    void drawShape(QPainter& p, const gas::VectorShape& shape, double opacity) const;
    void drawNodes(QPainter& p, const gas::VectorPath& path, int selectedNode) const;

    CanvasWidget* canvas_;
    gas::Document* document_ = nullptr;
    VectorToolSettings settings_;

    int layer_ = -1, shape_ = -1, node_ = -1; // selection
    int hover_ = -1;                          // shape under the pointer (Node tool)
    int nextToken_ = 1;
    int styleToken_ = 0, nudgeToken_ = 0;

    Drag drag_ = Drag::None;
    bool moved_ = false;
    bool handleOut_ = false;
    bool breakHandles_ = false;
    Qt::KeyboardModifiers pressMods_;
    gas::PointF startDoc_{};
    gas::VectorShapes startShapes_;
    int token_ = 0;

    std::optional<gas::VectorPath> pen_; // path being drawn with the Pen
    std::optional<QPointF> hoverPos_;
    gas::PointF createEnd_{};
    Qt::KeyboardModifiers createMods_;
};

// Options bar for the vector tools: shape kind and geometry (Shape tool), fill,
// stroke, width, join and anti-aliasing. Edits restyle the selected shape and
// become the style of new shapes.
class VectorOptions : public QWidget {
    Q_OBJECT

public:
    VectorOptions(VectorEditor* editor, QWidget* parent = nullptr);
    void setTool(Tool tool);
    void syncFromSelection();

private:
    void pushStyle();
    void updateSwatches();

    VectorEditor* editor_;
    QWidget* shapeGroup_ = nullptr;
    QComboBox* kind_ = nullptr;
    QLabel* radiusLabel_ = nullptr;
    QSpinBox* radius_ = nullptr;
    QLabel* sidesLabel_ = nullptr;
    QSpinBox* sides_ = nullptr;
    QLabel* innerLabel_ = nullptr;
    QSpinBox* inner_ = nullptr;
    QCheckBox* fill_ = nullptr;
    QToolButton* fillColour_ = nullptr;
    QCheckBox* stroke_ = nullptr;
    QToolButton* strokeColour_ = nullptr;
    QDoubleSpinBox* width_ = nullptr;
    QComboBox* join_ = nullptr;
    QCheckBox* antialias_ = nullptr;
    gas::VectorStyle style_;
};
