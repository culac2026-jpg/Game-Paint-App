#include "MainWindow.h"

#include <gas/core/Document.h>

#include <QActionGroup>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <cmath>

namespace {

constexpr int kDefaultCanvasSize = 1024;
constexpr int kMaxCanvasSize = 16384;

struct ToolInfo {
    Tool tool;
    const char* name;
    Qt::Key key;
};

constexpr ToolInfo kTools[] = {
    {Tool::Pencil, QT_TRANSLATE_NOOP("MainWindow", "Pencil"), Qt::Key_N},
    {Tool::Brush, QT_TRANSLATE_NOOP("MainWindow", "Brush"), Qt::Key_B},
    {Tool::Eraser, QT_TRANSLATE_NOOP("MainWindow", "Eraser"), Qt::Key_E},
    {Tool::Fill, QT_TRANSLATE_NOOP("MainWindow", "Fill"), Qt::Key_G},
    {Tool::Eyedropper, QT_TRANSLATE_NOOP("MainWindow", "Eyedropper"), Qt::Key_I},
};

QColor toQColor(gas::Rgba8 c) { return QColor(c.r, c.g, c.b, c.a); }

gas::Rgba8 fromQColor(const QColor& c) {
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
}

QSpinBox* makeSpinBox(QWidget* parent, int min, int max, const QString& suffix, const QString& tip) {
    auto* box = new QSpinBox(parent);
    box->setRange(min, max);
    box->setSuffix(suffix);
    box->setToolTip(tip);
    box->setKeyboardTracking(false);
    box->setFocusPolicy(Qt::ClickFocus); // keep tool shortcuts working
    return box;
}

int percent(double fraction) { return static_cast<int>(std::lround(fraction * 100.0)); }

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("GameArt Studio %1").arg(QStringLiteral(GAS_VERSION)));

    document_ = std::make_unique<gas::Document>(kDefaultCanvasSize, kDefaultCanvasSize);
    canvas_ = new CanvasWidget(this);
    canvas_->setDocument(document_.get());
    setCentralWidget(canvas_);

    zoomLabel_ = new QLabel(this);
    cursorLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(cursorLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);

    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::updateStatus);
    connect(canvas_, &CanvasWidget::cursorMoved, this, &MainWindow::updateStatus);
    connect(canvas_, &CanvasWidget::foregroundColorChanged, this, &MainWindow::updateColorSwatch);

    createActions();
    createToolBars();
    selectTool(canvas_->tool());
    updateColorSwatch();
    updateStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Canvas..."), QKeySequence::New, this, &MainWindow::newCanvas);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, canvas_, &CanvasWidget::zoomIn);
    view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, canvas_, &CanvasWidget::zoomOut);
    view->addAction(tr("&Actual Pixels"), QKeySequence(Qt::CTRL | Qt::Key_1), canvas_,
                    &CanvasWidget::zoomActual);
    view->addAction(tr("&Fit on Screen"), QKeySequence(Qt::CTRL | Qt::Key_0), canvas_,
                    &CanvasWidget::zoomToFit);
}

void MainWindow::createToolBars() {
    // Tool palette (left) + Tools menu share the same checkable actions.
    QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    QToolBar* palette = addToolBar(tr("Tools"));
    palette->setObjectName(QStringLiteral("toolPalette"));
    addToolBar(Qt::LeftToolBarArea, palette);
    palette->setMovable(false);
    palette->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* group = new QActionGroup(this);
    for (const ToolInfo& info : kTools) {
        QAction* a = group->addAction(tr(info.name));
        a->setCheckable(true);
        a->setShortcut(QKeySequence(info.key));
        a->setToolTip(QStringLiteral("%1 (%2)").arg(a->text(), a->shortcut().toString()));
        a->setChecked(info.tool == canvas_->tool());
        const Tool tool = info.tool;
        connect(a, &QAction::triggered, this, [this, tool] { selectTool(tool); });
        palette->addAction(a);
        toolsMenu->addAction(a);
    }
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Foreground &Color..."), QKeySequence(Qt::Key_C), this,
                         &MainWindow::chooseColor);
    toolsMenu->addAction(tr("&Smaller Brush"), QKeySequence(Qt::Key_BracketLeft), this,
                         [this] { adjustBrushSize(1.0 / 1.25); });
    toolsMenu->addAction(tr("&Larger Brush"), QKeySequence(Qt::Key_BracketRight), this,
                         [this] { adjustBrushSize(1.25); });

    // Tool options (top).
    QToolBar* options = addToolBar(tr("Tool Options"));
    options->setObjectName(QStringLiteral("toolOptions"));
    options->setMovable(false);

    colorButton_ = new QToolButton(options);
    colorButton_->setToolTip(tr("Foreground colour (C)"));
    connect(colorButton_, &QToolButton::clicked, this, &MainWindow::chooseColor);
    options->addWidget(colorButton_);
    options->addSeparator();

    sizeBox_ = makeSpinBox(options, 1, static_cast<int>(gas::BrushSettings::kMaxDiameter), tr(" px"),
                           tr("Brush size ([ / ])"));
    opacityBox_ = makeSpinBox(options, 1, 100, tr("%"), tr("Opacity"));
    hardnessBox_ = makeSpinBox(options, 0, 100, tr("%"), tr("Hardness"));
    toleranceBox_ = makeSpinBox(options, 0, 255, QString(), tr("Colour tolerance (0-255)"));

    const auto addLabeled = [options](const QString& label, QWidget* w) {
        QAction* labelAction = options->addWidget(new QLabel(QStringLiteral(" %1 ").arg(label), options));
        QAction* widgetAction = options->addWidget(w);
        return QList<QAction*>{labelAction, widgetAction};
    };
    brushOptionActions_ << addLabeled(tr("Size"), sizeBox_) << addLabeled(tr("Opacity"), opacityBox_)
                        << addLabeled(tr("Hardness"), hardnessBox_);
    fillOptionActions_ = addLabeled(tr("Tolerance"), toleranceBox_);

    connect(sizeBox_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->brushSettings(canvas_->tool()).diameter = v; canvas_->update(); });
    connect(opacityBox_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->brushSettings(canvas_->tool()).opacity = v / 100.0; });
    connect(hardnessBox_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->brushSettings(canvas_->tool()).hardness = v / 100.0; });
    connect(toleranceBox_, &QSpinBox::valueChanged, this, [this](int v) { canvas_->setFillTolerance(v); });
}

void MainWindow::selectTool(Tool tool) {
    canvas_->setTool(tool);
    syncToolOptions();
}

void MainWindow::syncToolOptions() {
    const Tool tool = canvas_->tool();
    const bool isBrush = tool == Tool::Brush || tool == Tool::Eraser;
    for (QAction* a : brushOptionActions_) {
        a->setVisible(isBrush);
    }
    for (QAction* a : fillOptionActions_) {
        a->setVisible(tool == Tool::Fill);
    }

    if (isBrush) {
        const gas::BrushSettings& s = canvas_->brushSettings(tool);
        const QSignalBlocker b1(sizeBox_), b2(opacityBox_), b3(hardnessBox_);
        sizeBox_->setValue(static_cast<int>(std::lround(s.diameter)));
        opacityBox_->setValue(percent(s.opacity));
        hardnessBox_->setValue(percent(s.hardness));
    }
    const QSignalBlocker b4(toleranceBox_);
    toleranceBox_->setValue(canvas_->fillTolerance());
}

void MainWindow::adjustBrushSize(double factor) {
    const Tool tool = canvas_->tool();
    if (tool != Tool::Brush && tool != Tool::Eraser) {
        return;
    }
    const int current = sizeBox_->value();
    int next = static_cast<int>(std::lround(current * factor));
    if (next == current) {
        next += factor > 1.0 ? 1 : -1; // always move at least 1 px
    }
    sizeBox_->setValue(next); // clamped by the spin box, propagates via valueChanged
}

void MainWindow::newCanvas() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Canvas"));
    auto* width = new QSpinBox(&dialog);
    auto* height = new QSpinBox(&dialog);
    for (QSpinBox* box : {width, height}) {
        box->setRange(1, kMaxCanvasSize);
        box->setSuffix(tr(" px"));
    }
    width->setValue(document_->width());
    height->setValue(document_->height());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QFormLayout(&dialog);
    layout->addRow(tr("Width:"), width);
    layout->addRow(tr("Height:"), height);
    layout->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    // Detach the canvas before the old document is destroyed.
    auto next = std::make_unique<gas::Document>(width->value(), height->value());
    canvas_->setDocument(next.get());
    document_ = std::move(next);
    canvas_->zoomToFit();
}

void MainWindow::chooseColor() {
    const QColor chosen = QColorDialog::getColor(toQColor(canvas_->foregroundColor()), this,
                                                 tr("Foreground Colour"), QColorDialog::ShowAlphaChannel);
    if (chosen.isValid()) {
        canvas_->setForegroundColor(fromQColor(chosen));
        updateColorSwatch();
    }
}

void MainWindow::updateColorSwatch() {
    constexpr int kSize = 20;
    QPixmap pm(kSize, kSize);
    pm.fill(Qt::white);
    QPainter p(&pm);
    // Checker behind the colour so alpha is visible.
    p.fillRect(0, 0, kSize / 2, kSize / 2, Qt::lightGray);
    p.fillRect(kSize / 2, kSize / 2, kSize / 2, kSize / 2, Qt::lightGray);
    p.fillRect(pm.rect(), toQColor(canvas_->foregroundColor()));
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    p.end();
    colorButton_->setIcon(QIcon(pm));
}

void MainWindow::updateStatus() {
    zoomLabel_->setText(tr("%1%").arg(canvas_->zoom() * 100.0, 0, 'f', 1));
    if (const auto p = canvas_->cursorPixel()) {
        cursorLabel_->setText(tr("X: %1  Y: %2").arg(p->x).arg(p->y));
    } else {
        cursorLabel_->clear();
    }
}
