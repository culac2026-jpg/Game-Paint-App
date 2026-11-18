#include "MainWindow.h"

#include "LayersPanel.h"

#include <gas/core/Document.h>
#include <gas/core/ProjectIO.h>

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
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
    {Tool::RectSelect, QT_TRANSLATE_NOOP("MainWindow", "Rect Select"), Qt::Key_M},
    {Tool::EllipseSelect, QT_TRANSLATE_NOOP("MainWindow", "Ellipse Select"), Qt::Key_O},
    {Tool::Lasso, QT_TRANSLATE_NOOP("MainWindow", "Lasso"), Qt::Key_L},
    {Tool::Move, QT_TRANSLATE_NOOP("MainWindow", "Move"), Qt::Key_V},
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

// Qt paths are UTF-16; going through wstring keeps non-ASCII names intact on Windows.
std::filesystem::path toFsPath(const QString& path) { return std::filesystem::path(path.toStdWString()); }

const QString kProjectFilter = QObject::tr("GameArt Studio project (*.gas)");

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    canvas_ = new CanvasWidget(this);
    setCentralWidget(canvas_);

    zoomLabel_ = new QLabel(this);
    cursorLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(cursorLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);

    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::updateStatus);
    connect(canvas_, &CanvasWidget::cursorMoved, this, &MainWindow::updateStatus);
    connect(canvas_, &CanvasWidget::foregroundColorChanged, this, &MainWindow::updateColorSwatch);
    connect(canvas_, &CanvasWidget::documentEdited, this, &MainWindow::onDocumentChanged);
    connect(canvas_, &CanvasWidget::editRefused, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 3000); });
    connect(canvas_, &CanvasWidget::transformModeChanged, this, [this](bool active) {
        syncToolOptions();
        onDocumentChanged();
        if (active) {
            statusBar()->showMessage(
                tr("Transform: drag inside to move, corners to scale (Shift keeps ratio), outside to rotate "
                   "(Shift snaps 15\u00B0). Enter applies, Esc cancels."));
        } else {
            statusBar()->clearMessage();
        }
    });

    createActions();
    createToolBars();
    createDocks();
    setDocument(std::make_unique<gas::Document>(kDefaultCanvasSize, kDefaultCanvasSize), QString());
    selectTool(canvas_->tool());
    updateColorSwatch();
    updateStatus();
    canvas_->setFocus(); // so tool shortcuts and Enter/Esc reach the canvas
}

MainWindow::~MainWindow() {
    // Views hold raw pointers into the document; detach them first.
    canvas_->setDocument(nullptr);
    layersPanel_->setDocument(nullptr);
}

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Canvas..."), QKeySequence::New, this, &MainWindow::newCanvas);
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::open);
    file->addSeparator();
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addAction(tr("&Export PNG..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this,
                    &MainWindow::exportPng);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = edit->addAction(tr("&Undo"), this, &MainWindow::undo);
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = edit->addAction(tr("&Redo"), this, &MainWindow::redo);
    redoAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    edit->addSeparator();
    documentActions_ << edit->addAction(tr("C&lear"), QKeySequence::Delete, this,
                                        [this] { runCommand(document_->eraseSelected()); });
    documentActions_ << edit->addAction(tr("&Fill with Foreground"), QKeySequence(Qt::ALT | Qt::Key_Backspace),
                                        this,
                                        [this] { runCommand(document_->fillSelected(canvas_->foregroundColor())); });
    edit->addSeparator();
    documentActions_ << edit->addAction(tr("Free &Transform"), QKeySequence(Qt::CTRL | Qt::Key_T), this,
                                        [this] { canvas_->beginFreeTransform(); });
    documentActions_ << edit->addAction(tr("Flip &Horizontal"), this,
                                        [this] { runCommand(document_->flipContent(true)); });
    documentActions_ << edit->addAction(tr("Flip &Vertical"), this,
                                        [this] { runCommand(document_->flipContent(false)); });
    documentActions_ << edit->addAction(tr("Rotate 90\u00B0 Clock&wise"), this,
                                        [this] { runCommand(document_->rotateContent90(true)); });
    documentActions_ << edit->addAction(tr("Rotate 90\u00B0 &Counter-clockwise"), this,
                                        [this] { runCommand(document_->rotateContent90(false)); });

    QMenu* select = menuBar()->addMenu(tr("&Select"));
    documentActions_ << select->addAction(tr("&All"), QKeySequence::SelectAll, this,
                                          [this] { runCommand(document_->selectAll()); });
    documentActions_ << select->addAction(tr("&Deselect"), QKeySequence(Qt::CTRL | Qt::Key_D), this,
                                          [this] { runCommand(document_->deselect()); });
    documentActions_ << select->addAction(tr("&Inverse"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), this,
                                          [this] { runCommand(document_->invertSelection()); });

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    documentActions_ << image->addAction(tr("&Crop to Selection"), this, &MainWindow::cropToSelection);

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

    selectionModeBox_ = new QComboBox(options);
    selectionModeBox_->addItem(tr("New"), static_cast<int>(gas::Selection::Op::Replace));
    selectionModeBox_->addItem(tr("Add (Shift)"), static_cast<int>(gas::Selection::Op::Add));
    selectionModeBox_->addItem(tr("Subtract (Alt)"), static_cast<int>(gas::Selection::Op::Subtract));
    selectionModeBox_->addItem(tr("Intersect (Shift+Alt)"), static_cast<int>(gas::Selection::Op::Intersect));
    selectionModeBox_->setFocusPolicy(Qt::ClickFocus);
    antialiasBox_ = new QCheckBox(tr("Anti-alias"), options);
    antialiasBox_->setFocusPolicy(Qt::NoFocus);
    antialiasBox_->setToolTip(tr("Soft edges for ellipse and lasso selections"));
    selectionOptionActions_ << addLabeled(tr("Mode"), selectionModeBox_) << options->addWidget(antialiasBox_);
    connect(selectionModeBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        canvas_->setSelectionOp(static_cast<gas::Selection::Op>(selectionModeBox_->itemData(i).toInt()));
    });
    connect(antialiasBox_, &QCheckBox::toggled, this, [this](bool on) { canvas_->setAntialiasSelection(on); });

    interpolationBox_ = new QComboBox(options);
    interpolationBox_->addItem(tr("Nearest (pixel art)"), static_cast<int>(gas::Interpolation::Nearest));
    interpolationBox_->addItem(tr("Bilinear (smooth)"), static_cast<int>(gas::Interpolation::Bilinear));
    interpolationBox_->setFocusPolicy(Qt::ClickFocus);
    interpolationBox_->setToolTip(tr("Resampling used when a transform is applied"));
    transformOptionActions_ = addLabeled(tr("Resample"), interpolationBox_);
    auto* applyButton = new QToolButton(options);
    applyButton->setText(tr("Apply"));
    applyButton->setToolTip(tr("Apply the transform (Enter)"));
    auto* cancelButton = new QToolButton(options);
    cancelButton->setText(tr("Cancel"));
    cancelButton->setToolTip(tr("Cancel the transform (Esc)"));
    transformApplyActions_ << options->addWidget(applyButton) << options->addWidget(cancelButton);
    connect(applyButton, &QToolButton::clicked, canvas_, &CanvasWidget::commitTransform);
    connect(cancelButton, &QToolButton::clicked, canvas_, &CanvasWidget::cancelTransform);
    connect(interpolationBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        canvas_->setTransformInterpolation(static_cast<gas::Interpolation>(interpolationBox_->itemData(i).toInt()));
    });

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
    const bool transforming = canvas_->isTransforming();
    const bool isBrush = tool == Tool::Brush || tool == Tool::Eraser;
    const bool isSelect = tool == Tool::RectSelect || tool == Tool::EllipseSelect || tool == Tool::Lasso;
    for (QAction* a : brushOptionActions_) {
        a->setVisible(isBrush && !transforming);
    }
    for (QAction* a : fillOptionActions_) {
        a->setVisible(tool == Tool::Fill && !transforming);
    }
    for (QAction* a : selectionOptionActions_) {
        a->setVisible(isSelect && !transforming);
    }
    for (QAction* a : transformOptionActions_) {
        a->setVisible(tool == Tool::Move || transforming);
    }
    for (QAction* a : transformApplyActions_) {
        a->setVisible(transforming);
    }
    {
        const QSignalBlocker b5(antialiasBox_);
        antialiasBox_->setChecked(canvas_->antialiasSelection());
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
    if (!maybeSave()) {
        return;
    }
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
    setDocument(std::make_unique<gas::Document>(width->value(), height->value()), QString());
}

void MainWindow::createDocks() {
    layersPanel_ = new LayersPanel(this);
    auto* dock = new QDockWidget(tr("Layers"), this);
    dock->setObjectName(QStringLiteral("layersDock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setWidget(layersPanel_);
    dock->setMinimumWidth(220);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    // Queued: the panel's own item signals must finish before it rebuilds its list.
    connect(layersPanel_, &LayersPanel::documentChanged, this, &MainWindow::onDocumentChanged,
            Qt::QueuedConnection);
}

void MainWindow::setDocument(std::unique_ptr<gas::Document> document, const QString& path) {
    // Point the views at the new document before the old one is destroyed.
    canvas_->setDocument(document.get());
    layersPanel_->setDocument(document.get());
    document_ = std::move(document);
    filePath_ = path;
    onDocumentChanged();
}

void MainWindow::onDocumentChanged() {
    const QString name = filePath_.isEmpty() ? tr("Untitled") : QFileInfo(filePath_).fileName();
    setWindowTitle(QStringLiteral("%1[*] - GameArt Studio %2").arg(name, QStringLiteral(GAS_VERSION)));
    setWindowModified(document_->isModified());

    undoAction_->setEnabled(document_->canUndo());
    undoAction_->setText(document_->canUndo()
                             ? tr("&Undo %1").arg(QString::fromStdString(document_->undoLabel()))
                             : tr("&Undo"));
    redoAction_->setEnabled(document_->canRedo());
    redoAction_->setText(document_->canRedo()
                             ? tr("&Redo %1").arg(QString::fromStdString(document_->redoLabel()))
                             : tr("&Redo"));
    const bool transforming = canvas_->isTransforming();
    for (QAction* a : documentActions_) {
        a->setEnabled(!transforming);
    }
    layersPanel_->setEnabled(!transforming);
    layersPanel_->refresh();
    canvas_->update();
}

void MainWindow::settleCanvas() {
    if (canvas_->isTransforming()) {
        canvas_->commitTransform();
    }
}

void MainWindow::runCommand(bool changed) {
    if (changed) {
        onDocumentChanged();
    }
}

void MainWindow::cropToSelection() {
    const auto bounds = document_->selection().bounds();
    if (!bounds) {
        statusBar()->showMessage(tr("Make a selection to crop to"), 3000);
        return;
    }
    if (document_->crop(*bounds)) {
        onDocumentChanged();
        canvas_->zoomToFit();
    }
}

void MainWindow::undo() {
    if (!canvas_->isBusy() && document_->undo()) {
        onDocumentChanged();
    }
}

void MainWindow::redo() {
    if (!canvas_->isBusy() && document_->redo()) {
        onDocumentChanged();
    }
}

bool MainWindow::maybeSave() {
    settleCanvas();
    if (!document_ || !document_->isModified()) {
        return true;
    }
    const QString name = filePath_.isEmpty() ? tr("Untitled") : QFileInfo(filePath_).fileName();
    const auto choice = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("Save changes to \"%1\" before closing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save) {
        return save();
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::open() {
    if (!maybeSave()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), QFileInfo(filePath_).path(),
                                                      kProjectFilter);
    if (!path.isEmpty()) {
        openFile(path);
    }
}

bool MainWindow::openFile(const QString& path) {
    gas::LoadResult result = gas::loadProject(toFsPath(path));
    if (!result.document) {
        QMessageBox::warning(this, tr("Open Failed"),
                             tr("Could not open \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)));
        return false;
    }
    setDocument(std::move(result.document), path);
    statusBar()->showMessage(tr("Opened %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

bool MainWindow::save() { return filePath_.isEmpty() ? saveAs() : saveTo(filePath_); }

bool MainWindow::saveAs() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Project As"), filePath_.isEmpty() ? tr("Untitled.gas") : filePath_, kProjectFilter);
    if (path.isEmpty()) {
        return false;
    }
    if (!path.endsWith(QLatin1String(gas::kProjectExtension), Qt::CaseInsensitive)) {
        path += QLatin1String(gas::kProjectExtension);
    }
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path) {
    settleCanvas();
    if (canvas_->isBusy()) {
        return false;
    }
    const gas::IoResult result = gas::saveProject(*document_, toFsPath(path));
    if (!result.ok) {
        QMessageBox::warning(this, tr("Save Failed"),
                             tr("Could not save \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)));
        return false;
    }
    filePath_ = path;
    document_->markSaved();
    onDocumentChanged();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

void MainWindow::exportPng() {
    settleCanvas();
    QString suggested = filePath_.isEmpty() ? tr("Untitled.png") : filePath_;
    if (suggested.endsWith(QLatin1String(gas::kProjectExtension), Qt::CaseInsensitive)) {
        suggested.chop(static_cast<int>(std::char_traits<char>::length(gas::kProjectExtension)));
        suggested += QStringLiteral(".png");
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".png");
    }
    const gas::IoResult result = gas::exportPng(*document_, toFsPath(path));
    if (!result.ok) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not export \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)));
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3000);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
    }
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
