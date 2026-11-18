#include "MainWindow.h"

#include "BrushPanel.h"
#include "EffectsPanel.h"
#include "LayersPanel.h"
#include "SlicesPanel.h"
#include "TimelinePanel.h"
#include "VectorTools.h"
#include "PluginUi.h"
#include "RecoveryDialog.h"

#include <gas/core/Document.h>
#include <gas/core/Plugins.h>
#include <gas/core/ProjectIO.h>
#include <gas/core/Recovery.h>
#include <gas/core/Slicing.h>

#include <QActionGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QLockFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLineEdit>
#include <QPushButton>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstring>

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
    {Tool::Smudge, QT_TRANSLATE_NOOP("MainWindow", "Smudge"), Qt::Key_S},
    {Tool::Blur, QT_TRANSLATE_NOOP("MainWindow", "Blur"), Qt::Key_R},
    {Tool::Slice, QT_TRANSLATE_NOOP("MainWindow", "Slice"), Qt::Key_K},
    {Tool::Pen, QT_TRANSLATE_NOOP("MainWindow", "Pen"), Qt::Key_P},
    {Tool::Shape, QT_TRANSLATE_NOOP("MainWindow", "Shape"), Qt::Key_J},
    {Tool::Node, QT_TRANSLATE_NOOP("MainWindow", "Node"), Qt::Key_A},
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

gas::LayerProps layerNamed(const QString& name) {
    gas::LayerProps props;
    props.name = name.toStdString();
    return props;
}

// Draws `image` into `dst` with its top-left at (ox, oy); pixels outside are clipped.
void blitImage(gas::TiledImage& dst, const QImage& image, int ox, int oy) {
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888); // straight alpha, like the document
    for (int y = 0; y < src.height(); ++y) {
        const uchar* line = src.constScanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const gas::Rgba8 p{line[4 * x], line[4 * x + 1], line[4 * x + 2], line[4 * x + 3]};
            if (p.a != 0 && dst.contains({ox + x, oy + y})) {
                dst.setPixel({ox + x, oy + y}, p);
            }
        }
    }
}

QString imageFilter() {
    QStringList patterns;
    for (const QByteArray& f : QImageReader::supportedImageFormats()) {
        patterns << QStringLiteral("*.%1").arg(QString::fromLatin1(f));
    }
    return QObject::tr("Images (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

// A small modal dialog with a form and OK/Cancel.
struct FormDialog {
    explicit FormDialog(QWidget* parent, const QString& title) : dialog(parent), form(new QFormLayout(&dialog)) {
        dialog.setWindowTitle(title);
    }
    bool exec() {
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        return dialog.exec() == QDialog::Accepted;
    }
    QSpinBox* spin(const QString& label, int min, int max, int value, const QString& suffix = {}) {
        auto* box = new QSpinBox(&dialog);
        box->setRange(min, max);
        box->setValue(value);
        box->setSuffix(suffix);
        form->addRow(label, box);
        return box;
    }
    QCheckBox* check(const QString& text, bool on) {
        auto* box = new QCheckBox(text, &dialog);
        box->setChecked(on);
        form->addRow(QString(), box);
        return box;
    }
    QDialog dialog;
    QFormLayout* form;
};

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
    loadPlugins();
    setDocument(std::make_unique<gas::Document>(kDefaultCanvasSize, kDefaultCanvasSize), QString());

    // Crash recovery: this window autosaves into its own session slot, guarded
    // by a lock file so other running windows never offer it for recovery.
    recovery_ = std::make_unique<gas::RecoveryStore>(toFsPath(recoveryFolder()));
    session_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    QDir().mkpath(recoveryFolder());
    sessionLock_ = std::make_unique<QLockFile>(recoveryFolder() + QStringLiteral("/%1.lock").arg(QString::fromStdString(session_)));
    sessionLock_->setStaleLockTime(0);
    sessionLock_->tryLock(0);
    autosaveTimer_ = new QTimer(this);
    // Every minute; GAS_AUTOSAVE_SECONDS overrides it (QA, slow disks).
    const int autosaveSeconds = qEnvironmentVariableIntValue("GAS_AUTOSAVE_SECONDS");
    autosaveTimer_->setInterval((autosaveSeconds > 0 ? autosaveSeconds : 60) * 1000);
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer_->start();
    selectTool(canvas_->tool());
    updateColorSwatch();
    updateStatus();
    canvas_->setFocus(); // so tool shortcuts and Enter/Esc reach the canvas
}

MainWindow::~MainWindow() {
    // Views hold raw pointers into the document; detach them first.
    canvas_->setDocument(nullptr);
    layersPanel_->setDocument(nullptr);
    effectsPanel_->setDocument(nullptr);
    slicesPanel_->setDocument(nullptr);
    timelinePanel_->setDocument(nullptr);
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
    file->addAction(tr("Export S&VG..."), this, &MainWindow::exportSvg);
    file->addSeparator();
    file->addAction(tr("&Import Images..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this,
                    &MainWindow::importImages);
    documentActions_ << file->addAction(tr("Import as &Layer..."), this, &MainWindow::importAsLayer);
    documentActions_ << file->addAction(tr("Export &Atlas..."), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E), this,
                                        &MainWindow::exportAtlas);
    documentActions_ << file->addAction(tr("Export S&lices as PNGs..."), this, &MainWindow::exportSlices);
    documentActions_ << file->addAction(tr("Export La&yers as PNGs..."), this, &MainWindow::exportLayers);
    documentActions_ << file->addAction(tr("Export &Frames as PNGs..."), this, &MainWindow::exportFrames);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = edit->addAction(tr("&Undo"), this, &MainWindow::undo);
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = edit->addAction(tr("&Redo"), this, &MainWindow::redo);
    redoAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    edit->addSeparator();
    documentActions_ << edit->addAction(tr("C&lear"), QKeySequence::Delete, this, [this] {
        if (canvas_->tool() == Tool::Slice) {
            canvas_->deleteSelectedSlice(); // Delete removes the selected slice, never pixels
            return;
        }
        if (isVectorTool(canvas_->tool())) {
            canvas_->vectorEditor()->deleteSelection(); // the selected node or shape
            return;
        }
        runCommand(document_->eraseSelected());
    });
    documentActions_ << edit->addAction(tr("&Fill with Foreground"), QKeySequence(Qt::ALT | Qt::Key_Backspace),
                                        this,
                                        [this] { runCommand(document_->fillSelected(canvas_->foregroundColor())); });
    documentActions_ << edit->addAction(tr("Define &Brush Tip"), this, &MainWindow::defineBrushTip);
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
    documentActions_ << select->addAction(tr("&Feather..."), QKeySequence(Qt::SHIFT | Qt::Key_F6), this,
                                          &MainWindow::featherSelection);

    QMenu* layerMenu = menuBar()->addMenu(tr("&Layer"));
    documentActions_ << layerMenu->addAction(tr("New &Vector Layer"), this, [this] {
        settleCanvas();
        runCommand(document_->addVectorLayer());
    });
    documentActions_ << layerMenu->addAction(tr("&Rasterize Vector Layer"), this, [this] {
        settleCanvas();
        runCommand(document_->rasterizeVectorLayer(document_->activeLayerIndex()));
    });
    layerMenu->addSeparator();
    documentActions_ << layerMenu->addAction(tr("Bring Shape &Forward"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
                                             this, [this] { canvas_->vectorEditor()->raiseSelection(1); });
    documentActions_ << layerMenu->addAction(tr("Send Shape &Backward"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
                                             this, [this] { canvas_->vectorEditor()->raiseSelection(-1); });

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    documentActions_ << image->addAction(tr("&Crop to Selection"), this, &MainWindow::cropToSelection);
    image->addSeparator();
    documentActions_ << image->addAction(tr("&Auto Slice..."), this, &MainWindow::autoSlice);
    documentActions_ << image->addAction(tr("&Grid Slice..."), this, &MainWindow::gridSlice);

    QMenu* anim = menuBar()->addMenu(tr("&Animation"));
    documentActions_ << anim->addAction(tr("&New Frame"), QKeySequence(Qt::ALT | Qt::Key_N), this,
                                        [this] { runFrameCommand(&TimelinePanel::addFrame); });
    documentActions_ << anim->addAction(tr("&Duplicate Frame"), QKeySequence(Qt::ALT | Qt::Key_D), this,
                                        [this] { runFrameCommand(&TimelinePanel::duplicateFrame); });
    documentActions_ << anim->addAction(tr("De&lete Frame"), this,
                                        [this] { runFrameCommand(&TimelinePanel::removeFrame); });
    anim->addSeparator();
    anim->addAction(tr("&Previous Frame"), QKeySequence(Qt::Key_Comma), this,
                    [this] { runFrameCommand(&TimelinePanel::previousFrame); });
    anim->addAction(tr("Ne&xt Frame"), QKeySequence(Qt::Key_Period), this,
                    [this] { runFrameCommand(&TimelinePanel::nextFrame); });
    anim->addAction(tr("P&lay / Stop"), QKeySequence(Qt::ALT | Qt::Key_P), this,
                    [this] { timelinePanel_->togglePlay(); });
    anim->addSeparator();
    onionAction_ = anim->addAction(tr("&Onion Skin"));
    onionAction_->setCheckable(true);
    onionAction_->setShortcut(QKeySequence(Qt::Key_F3));
    documentActions_ << anim->addAction(tr("New &Tag..."), this, [this] { runFrameCommand(&TimelinePanel::newTag); });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Reset &Symmetry Centre"), canvas_, &CanvasWidget::resetSymmetryCenter);
    QAction* showSlices = view->addAction(tr("Show S&lices"));
    showSlices->setCheckable(true);
    showSlices->setChecked(canvas_->showSlices());
    showSlices->setToolTip(tr("Show slice boxes with every tool (the Slice tool always shows them)"));
    connect(showSlices, &QAction::toggled, canvas_, &CanvasWidget::setShowSlices);
    view->addSeparator();
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
        toolActions_.insert(static_cast<int>(tool), a);
        connect(a, &QAction::triggered, this, [this, tool] { selectTool(tool); });
        palette->addAction(a);
        toolsMenu->addAction(a);
    }
    toolsMenu->addSeparator();
    lineModeAction_ = toolsMenu->addAction(tr("Straight &Line"));
    lineModeAction_->setCheckable(true);
    lineModeAction_->setShortcut(QKeySequence(Qt::Key_U));
    lineModeAction_->setToolTip(tr("Straight line mode (U): every drag paints a straight line.\n"
                                   "Without it: hold Ctrl while dragging for one line.\n"
                                   "Shift snaps the angle to 15° steps; Shift+click joins the last stroke to the click."));
    connect(lineModeAction_, &QAction::toggled, canvas_, &CanvasWidget::setLineMode);
    toolsMenu->addAction(tr("Toggle Stream&Line"), QKeySequence(Qt::Key_Q), this, [this] {
        if (streamlineBox_->value() > 0) {
            lastStreamline_ = streamlineBox_->value();
            streamlineBox_->setValue(0);
        } else {
            streamlineBox_->setValue(lastStreamline_);
        }
        statusBar()->showMessage(tr("StreamLine %1%").arg(streamlineBox_->value()), 2000);
    });
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
    opacityLabel_ = qobject_cast<QLabel*>(options->widgetForAction(brushOptionActions_[2]));
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

    symmetryBox_ = new QComboBox(options);
    symmetryBox_->setFocusPolicy(Qt::NoFocus);
    symmetryBox_->setToolTip(tr("Mirror painting. Drag the round handle on the canvas to move the centre."));
    for (int m = 0; m < gas::kSymmetryModeCount; ++m) {
        symmetryBox_->addItem(tr(gas::symmetryModeName(static_cast<gas::SymmetryMode>(m)).data()), m);
    }
    segmentsBox_ = makeSpinBox(options, gas::Symmetry::kMinSegments, gas::Symmetry::kMaxSegments, QString(),
                               tr("Number of radial copies"));
    segmentsBox_->setValue(canvas_->symmetry().segments);
    auto* lineButton = new QToolButton(options);
    lineButton->setDefaultAction(lineModeAction_); // its own widget, so hiding it keeps the menu entry
    lineButton->setFocusPolicy(Qt::NoFocus);
    streamlineBox_ = makeSpinBox(options, 0, 100, tr("%"),
                                 tr("StreamLine (Q toggles): smooths hand-drawn strokes into clean curves.\n"
                                    "The stroke trails the pointer, then is redrawn along a smoothed path on release."));
    streamlineBox_->setValue(0);
    connect(streamlineBox_, &QSpinBox::valueChanged, this, [this](int v) { canvas_->setStreamline(v / 100.0); });
    symmetryOptionActions_ << options->addSeparator() << addLabeled(tr("StreamLine"), streamlineBox_)
                           << options->addWidget(lineButton) << addLabeled(tr("Symmetry"), symmetryBox_);
    segmentsOptionActions_ = addLabeled(tr("Segments"), segmentsBox_);
    connect(symmetryBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        canvas_->setSymmetryMode(static_cast<gas::SymmetryMode>(symmetryBox_->itemData(i).toInt()));
        syncToolOptions();
    });
    connect(segmentsBox_, &QSpinBox::valueChanged, canvas_, &CanvasWidget::setSymmetrySegments);
    connect(selectionModeBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        canvas_->setSelectionOp(static_cast<gas::Selection::Op>(selectionModeBox_->itemData(i).toInt()));
    });
    connect(antialiasBox_, &QCheckBox::toggled, this, [this](bool on) { canvas_->setAntialiasSelection(on); });

    vectorOptions_ = new VectorOptions(canvas_->vectorEditor(), options);
    vectorOptionsAction_ = options->addWidget(vectorOptions_);

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
    if (tool == Tool::Slice && slicesDock_) {
        slicesDock_->raise();
        statusBar()->showMessage(tr("Slice: drag to add a sprite region, drag a box to move it, its handles to resize, "
                                    "the cross to move the pivot (Shift snaps). Arrows nudge, Delete removes."),
                                 6000);
    }
    if (tool == Tool::Pen) {
        statusBar()->showMessage(tr("Pen: click for corners, drag for curves; click the first node to close, "
                                    "Enter or double-click to finish, Backspace removes a node, Esc cancels."),
                                 6000);
    } else if (tool == Tool::Shape) {
        statusBar()->showMessage(tr("Shape: drag a box (Shift = square, Alt = from the centre). Pick the shape, fill "
                                    "and stroke on the options bar."),
                                 6000);
    } else if (tool == Tool::Node) {
        statusBar()->showMessage(tr("Node: click a shape to select and drag it; drag nodes and handles; Alt+click a "
                                    "node = corner/smooth; double-click the outline adds a node; Delete removes."),
                                 6000);
    }
    if (QAction* a = toolActions_.value(static_cast<int>(tool))) {
        a->setChecked(true); // keeps the palette in sync when the tool changes programmatically
    }
    syncToolOptions();
}

void MainWindow::syncToolOptions() {
    const Tool tool = canvas_->tool();
    const bool transforming = canvas_->isTransforming();
    const bool isBrush = usesBrushSettings(tool);
    const bool retouch = tool == Tool::Smudge || tool == Tool::Blur;
    opacityLabel_->setText(retouch ? tr(" Strength ") : tr(" Opacity "));
    opacityBox_->setToolTip(retouch ? tr("Strength") : tr("Opacity"));
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
    const bool strokeTool = (isBrush || tool == Tool::Pencil) && !transforming;
    const gas::SymmetryMode symmetryMode = canvas_->symmetry().mode;
    const bool radial = symmetryMode == gas::SymmetryMode::Radial || symmetryMode == gas::SymmetryMode::Kaleidoscope;
    for (QAction* a : symmetryOptionActions_) {
        a->setVisible(strokeTool);
    }
    for (QAction* a : segmentsOptionActions_) {
        a->setVisible(strokeTool && radial);
    }
    for (QAction* a : transformOptionActions_) {
        a->setVisible(tool == Tool::Move || transforming);
    }
    for (QAction* a : transformApplyActions_) {
        a->setVisible(transforming);
    }
    vectorOptionsAction_->setVisible(isVectorTool(tool) && !transforming);
    vectorOptions_->setTool(tool);
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
    if (brushPanel_) {
        brushPanel_->refresh();
    }
    const QSignalBlocker b4(toleranceBox_);
    toleranceBox_->setValue(canvas_->fillTolerance());
}

void MainWindow::adjustBrushSize(double factor) {
    const Tool tool = canvas_->tool();
    if (!usesBrushSettings(tool)) {
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
    brushPanel_ = new BrushPanel(canvas_, this);
    auto* brushDock = new QDockWidget(tr("Brush"), this);
    brushDock->setObjectName(QStringLiteral("brushDock"));
    brushDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    auto* brushScroll = new QScrollArea(brushDock);
    brushScroll->setWidget(brushPanel_);
    brushScroll->setWidgetResizable(true);
    brushScroll->setFrameShape(QFrame::NoFrame);
    brushDock->setWidget(brushScroll);
    addDockWidget(Qt::RightDockWidgetArea, brushDock);
    effectsPanel_ = new EffectsPanel(this);
    auto* effectsDock = new QDockWidget(tr("Effects"), this);
    effectsDock->setObjectName(QStringLiteral("effectsDock"));
    effectsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    effectsDock->setWidget(effectsPanel_);
    tabifyDockWidget(brushDock, effectsDock);
    slicesPanel_ = new SlicesPanel(canvas_, this);
    slicesDock_ = new QDockWidget(tr("Slices"), this);
    slicesDock_->setObjectName(QStringLiteral("slicesDock"));
    slicesDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    slicesDock_->setWidget(slicesPanel_);
    tabifyDockWidget(effectsDock, slicesDock_);
    brushDock->raise();
    connect(slicesPanel_, &SlicesPanel::documentChanged, this, &MainWindow::onDocumentChanged, Qt::QueuedConnection);
    connect(slicesPanel_, &SlicesPanel::autoSliceRequested, this, &MainWindow::autoSlice);
    connect(slicesPanel_, &SlicesPanel::gridSliceRequested, this, &MainWindow::gridSlice);
    connect(slicesPanel_, &SlicesPanel::exportAtlasRequested, this, &MainWindow::exportAtlas);
    connect(slicesPanel_, &SlicesPanel::nineSlicePreviewRequested, this, &MainWindow::nineSlicePreview);

    timelinePanel_ = new TimelinePanel(canvas_, this);
    auto* timelineDock = new QDockWidget(tr("Timeline"), this);
    timelineDock->setObjectName(QStringLiteral("timelineDock"));
    timelineDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    timelineDock->setWidget(timelinePanel_);
    // Side docks keep the full height; the timeline sits under the canvas only.
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
    connect(timelinePanel_, &TimelinePanel::documentChanged, this, &MainWindow::onDocumentChanged, Qt::QueuedConnection);
    connect(timelinePanel_, &TimelinePanel::viewChanged, this, [this] {
        layersPanel_->refresh();
        canvas_->update();
        updateStatus();
    });
    connect(onionAction_, &QAction::toggled, timelinePanel_, &TimelinePanel::setOnionSkin);
    connect(effectsPanel_, &EffectsPanel::documentChanged, this, &MainWindow::onDocumentChanged,
            Qt::QueuedConnection);
    // Once laid out, give the layer list most of the height (sizes set before show() don't stick).
    QTimer::singleShot(0, this, [this, dock, brushDock] { resizeDocks({dock, brushDock}, {420, 300}, Qt::Vertical); });
    connect(brushPanel_, &BrushPanel::settingsChanged, this, [this] {
        syncToolOptions(); // a preset may change size / opacity / hardness
        canvas_->update(); // brush outline
    });

    // Queued: the panel's own item signals must finish before it rebuilds its list.
    connect(layersPanel_, &LayersPanel::documentChanged, this, &MainWindow::onDocumentChanged,
            Qt::QueuedConnection);
}

void MainWindow::setDocument(std::unique_ptr<gas::Document> document, const QString& path) {
    // Point the views at the new document before the old one is destroyed.
    canvas_->setDocument(document.get());
    layersPanel_->setDocument(document.get());
    effectsPanel_->setDocument(document.get());
    slicesPanel_->setDocument(document.get());
    timelinePanel_->setDocument(document.get());
    document_ = std::move(document);
    filePath_ = path;
    autosavedGeneration_ = ~0u;
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
    canvas_->vectorEditor()->documentChanged();
    layersPanel_->setEnabled(!transforming);
    layersPanel_->refresh();
    effectsPanel_->setEnabled(!transforming);
    effectsPanel_->refresh();
    slicesPanel_->setEnabled(!transforming);
    slicesPanel_->refresh();
    timelinePanel_->setEnabled(!transforming);
    timelinePanel_->refresh();
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

void MainWindow::featherSelection() {
    settleCanvas();
    if (document_->selection().isEmpty()) {
        statusBar()->showMessage(tr("Feather needs a selection"), 3000);
        return;
    }
    bool ok = false;
    const double radius =
        QInputDialog::getDouble(this, tr("Feather Selection"), tr("Feather radius (px):"), 6.0, 0.5, 250.0, 1, &ok);
    if (ok) {
        runCommand(document_->setSelection(document_->selection().feathered(radius), "Feather"));
    }
}

// The active layer's pixels under the selection (or the whole layer) become
// the Brush tool's custom tip.
void MainWindow::defineBrushTip() {
    const gas::Selection& sel = document_->selection();
    const gas::IRect area = sel.bounds().value_or(gas::IRect{0, 0, document_->width(), document_->height()});
    gas::BrushTip tip = gas::brushTipFromImage(document_->activeLayer().image, area, sel.isEmpty() ? nullptr : &sel);
    if (tip.empty()) {
        statusBar()->showMessage(tr("Nothing painted there to make a brush tip from"), 3000);
        return;
    }
    const int size = std::max(tip.width, tip.height);
    gas::BrushSettings& s = canvas_->brushSettings(Tool::Brush);
    s.tip.tip = std::make_shared<const gas::BrushTip>(std::move(tip));
    s.shape = gas::BrushShape::Custom;
    s.diameter = std::clamp<double>(size, 1.0, gas::BrushSettings::kMaxDiameter);
    s.spacing = std::max(s.spacing, 0.25); // stamps read better spaced out
    selectTool(Tool::Brush);
    statusBar()->showMessage(tr("Brush tip defined (%1 px)").arg(size), 3000);
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
    timelinePanel_->stop();
    if (!canvas_->isBusy() && document_->undo()) {
        onDocumentChanged();
    }
}

void MainWindow::redo() {
    timelinePanel_->stop();
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
        const std::filesystem::path backup = gas::backupPathFor(toFsPath(path));
        std::error_code ec;
        if (std::filesystem::exists(backup, ec)) {
            // The previous save is kept as a backup: offer it.
            const auto choice = QMessageBox::warning(
                this, tr("Open Failed"),
                tr("Could not open \"%1\":\n%2\n\nOpen the backup of the previous save instead?")
                    .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)),
                QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Open);
            if (choice == QMessageBox::Open) {
                gas::LoadResult fromBackup = gas::loadProject(backup);
                if (fromBackup.document) {
                    setDocument(std::move(fromBackup.document), path);
                    document_->markUnsaved(); // saving replaces the damaged file
                    onDocumentChanged();
                    statusBar()->showMessage(tr("Opened the backup of %1").arg(QFileInfo(path).fileName()), 5000);
                    return true;
                }
                QMessageBox::warning(this, tr("Open Failed"),
                                     tr("The backup could not be opened either:\n%1")
                                         .arg(QString::fromStdString(fromBackup.error)));
            }
            return false;
        }
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
    // The file being replaced is kept as "<name>.gas.bak".
    const gas::IoResult result = gas::saveProject(*document_, toFsPath(path), {.backup = true});
    if (!result.ok) {
        QMessageBox::warning(this, tr("Save Failed"),
                             tr("Could not save \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)));
        return false;
    }
    filePath_ = path;
    document_->markSaved();
    clearAutosave(); // the file now holds everything
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
        // A clean exit leaves nothing to recover.
        autosaveTimer_->stop();
        clearAutosave();
        if (sessionLock_) {
            sessionLock_->unlock();
        }
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
    zoomLabel_->setText(document_ && document_->frameCount() > 1
                            ? tr("Frame %1/%2   %3%").arg(document_->activeFrame() + 1).arg(document_->frameCount())
                                  .arg(canvas_->zoom() * 100.0, 0, 'f', 1)
                            : tr("%1%").arg(canvas_->zoom() * 100.0, 0, 'f', 1));
    if (const auto p = canvas_->cursorPixel()) {
        cursorLabel_->setText(tr("X: %1  Y: %2").arg(p->x).arg(p->y));
    } else {
        cursorLabel_->clear();
    }
}

// ---------------------------------------------------------------- asset pipeline (v0.7)

QString MainWindow::documentBaseName() const {
    const QString stem = QFileInfo(filePath_).completeBaseName();
    return stem.isEmpty() ? QStringLiteral("sprite") : stem;
}

void MainWindow::importImages() {
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Images"), importDir_, imageFilter());
    if (!paths.isEmpty() && maybeSave()) {
        importImageFiles(paths);
    }
}

void MainWindow::importImageFiles(const QStringList& paths) {
    if (paths.isEmpty()) {
        return;
    }
    importDir_ = QFileInfo(paths.front()).path();
    QList<QImage> images;
    QStringList names, failed;
    for (const QString& path : paths) {
        QImage img(path);
        if (img.isNull() || img.width() > kMaxCanvasSize || img.height() > kMaxCanvasSize) {
            failed << QFileInfo(path).fileName();
            continue;
        }
        images << img;
        names << QFileInfo(path).completeBaseName();
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, tr("Import"), tr("Could not import:\n%1").arg(failed.join(QLatin1Char('\n'))));
    }
    if (images.isEmpty()) {
        return;
    }
    if (images.size() == 1) {
        gas::TiledImage img(images[0].width(), images[0].height());
        blitImage(img, images[0], 0, 0);
        std::vector<gas::Layer> layers;
        layers.emplace_back(layerNamed(names[0]), std::move(img));
        setDocument(std::make_unique<gas::Document>(images[0].width(), images[0].height(), std::move(layers), 0),
                    QString());
        canvas_->zoomToFit();
        statusBar()->showMessage(tr("Imported %1 (%2\u00D7%3)").arg(names[0]).arg(images[0].width()).arg(images[0].height()),
                                 4000);
        return;
    }
    // Several images: a grid of equal cells (the largest image's size), each
    // image centred horizontally and standing on its cell's bottom edge, with
    // one slice per cell so the pivots line up for animation.
    int cellW = 1, cellH = 1;
    bool sameSize = true;
    for (const QImage& img : images) {
        cellW = std::max(cellW, img.width());
        cellH = std::max(cellH, img.height());
        sameSize = sameSize && img.size() == images[0].size();
    }
    const int n = static_cast<int>(images.size());
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    cols = std::min(cols, std::max(1, kMaxCanvasSize / cellW));
    const int rows = (n + cols - 1) / cols;
    if (static_cast<long long>(rows) * cellH > kMaxCanvasSize) {
        QMessageBox::warning(this, tr("Import"), tr("Too many images for one canvas (max %1 px).").arg(kMaxCanvasSize));
        return;
    }
    gas::TiledImage sheet(cols * cellW, rows * cellH);
    std::vector<gas::Slice> slices;
    std::vector<std::string> taken;
    const gas::PointF pivot = sameSize ? gas::PointF{0.5, 0.5} : gas::PointF{0.5, 1.0};
    for (int i = 0; i < n; ++i) {
        const int cx = (i % cols) * cellW, cy = (i / cols) * cellH;
        blitImage(sheet, images[i], cx + (cellW - images[i].width()) / 2, cy + cellH - images[i].height());
        const std::string name = gas::uniqueName(names[i].toStdString(), taken);
        taken.push_back(name);
        slices.push_back({name, {cx, cy, cellW, cellH}, pivot, {}});
    }
    std::vector<gas::Layer> layers;
    layers.emplace_back(layerNamed(tr("Sprites")), std::move(sheet));
    auto doc = std::make_unique<gas::Document>(cols * cellW, rows * cellH, std::move(layers), 0);
    doc->restoreSlices(std::move(slices));
    setDocument(std::move(doc), QString());
    canvas_->zoomToFit();
    statusBar()->showMessage(tr("Imported %1 images as a %2\u00D7%3 sheet with one slice each").arg(n).arg(cols).arg(rows),
                             5000);
}

void MainWindow::importAsLayer() {
    settleCanvas();
    const QString path = QFileDialog::getOpenFileName(this, tr("Import as Layer"), importDir_, imageFilter());
    if (path.isEmpty()) {
        return;
    }
    importDir_ = QFileInfo(path).path();
    const QImage image(path);
    if (image.isNull()) {
        QMessageBox::warning(this, tr("Import"), tr("Could not read \"%1\".").arg(QFileInfo(path).fileName()));
        return;
    }
    gas::TiledImage img(document_->width(), document_->height());
    blitImage(img, image, 0, 0);
    if (document_->addLayer(QFileInfo(path).completeBaseName().toStdString(), std::move(img))) {
        onDocumentChanged();
        if (image.width() > document_->width() || image.height() > document_->height()) {
            statusBar()->showMessage(tr("The image is larger than the canvas and was clipped"), 4000);
        }
    }
}

void MainWindow::autoSlice() {
    settleCanvas();
    if (canvas_->isBusy()) {
        return;
    }
    static gas::AutoSliceOptions last;
    static int lastPivot = 0;
    FormDialog d(this, tr("Auto Slice"));
    auto* name = new QLineEdit(documentBaseName(), &d.dialog);
    d.form->addRow(tr("Name prefix:"), name);
    QSpinBox* merge = d.spin(tr("Merge parts closer than:"), 0, 256, last.mergeDistance, tr(" px"));
    merge->setToolTip(tr("Detached parts within this distance become one sprite"));
    QSpinBox* minPixels = d.spin(tr("Ignore islands under:"), 1, 100000, last.minPixels, tr(" px"));
    QSpinBox* alpha = d.spin(tr("Alpha threshold:"), 0, 254, last.alphaThreshold);
    alpha->setToolTip(tr("Pixels with alpha at or below this count as empty"));
    auto* pivot = new QComboBox(&d.dialog);
    fillPivotCombo(pivot);
    pivot->removeItem(pivot->count() - 1); // no "Custom" here
    pivot->setCurrentIndex(lastPivot);
    d.form->addRow(tr("Pivot:"), pivot);
    QCheckBox* replace = d.check(tr("Replace existing slices"), true);
    if (!d.exec()) {
        return;
    }
    last.mergeDistance = merge->value();
    last.minPixels = minPixels->value();
    last.alphaThreshold = alpha->value();
    lastPivot = pivot->currentIndex();
    const std::vector<gas::IRect> rects = gas::autoSlices(document_->flattenedImage(), last);
    if (rects.empty()) {
        statusBar()->showMessage(tr("No sprites found (the visible layers are empty)"), 4000);
        return;
    }
    std::vector<gas::Slice> slices = replace->isChecked() ? std::vector<gas::Slice>{} : document_->slices();
    std::vector<std::string> taken;
    for (const gas::Slice& s : slices) {
        taken.push_back(s.name);
    }
    const std::string base = name->text().trimmed().isEmpty() ? "sprite" : name->text().trimmed().toStdString();
    for (gas::Slice s : gas::namedSlices(rects, base, pivotFromCombo(pivot, {0.5, 0.5}), static_cast<int>(slices.size()))) {
        s.name = gas::uniqueName(s.name, taken);
        taken.push_back(s.name);
        slices.push_back(std::move(s));
    }
    if (document_->setSlices(std::move(slices), "Auto Slice")) {
        onDocumentChanged();
        slicesDock_->raise();
    }
    statusBar()->showMessage(tr("Found %1 sprites").arg(rects.size()), 4000);
}

void MainWindow::gridSlice() {
    settleCanvas();
    if (canvas_->isBusy()) {
        return;
    }
    static gas::GridSliceOptions last;
    static int lastPivot = 0;
    FormDialog d(this, tr("Grid Slice"));
    auto* name = new QLineEdit(documentBaseName(), &d.dialog);
    d.form->addRow(tr("Name prefix:"), name);
    QSpinBox* cw = d.spin(tr("Cell width:"), 1, document_->width(), std::min(last.cellWidth, document_->width()), tr(" px"));
    QSpinBox* ch = d.spin(tr("Cell height:"), 1, document_->height(), std::min(last.cellHeight, document_->height()), tr(" px"));
    QSpinBox* ox = d.spin(tr("Offset X:"), 0, document_->width() - 1, last.offsetX, tr(" px"));
    QSpinBox* oy = d.spin(tr("Offset Y:"), 0, document_->height() - 1, last.offsetY, tr(" px"));
    QSpinBox* sx = d.spin(tr("Spacing X:"), 0, 4096, last.spacingX, tr(" px"));
    QSpinBox* sy = d.spin(tr("Spacing Y:"), 0, 4096, last.spacingY, tr(" px"));
    auto* info = new QLabel(&d.dialog);
    d.form->addRow(QString(), info);
    const auto updateInfo = [&] {
        const int stepX = cw->value() + sx->value(), stepY = ch->value() + sy->value();
        const int cols = std::max(0, (document_->width() - ox->value() + sx->value()) / stepX);
        const int rows = std::max(0, (document_->height() - oy->value() + sy->value()) / stepY);
        info->setText(tr("%1 columns \u00D7 %2 rows").arg(cols).arg(rows));
    };
    for (QSpinBox* box : {cw, ch, ox, oy, sx, sy}) {
        QObject::connect(box, &QSpinBox::valueChanged, &d.dialog, updateInfo);
    }
    updateInfo();
    QCheckBox* skip = d.check(tr("Skip empty cells"), last.skipEmpty);
    auto* pivot = new QComboBox(&d.dialog);
    fillPivotCombo(pivot);
    pivot->removeItem(pivot->count() - 1);
    pivot->setCurrentIndex(lastPivot);
    d.form->addRow(tr("Pivot:"), pivot);
    QCheckBox* replace = d.check(tr("Replace existing slices"), true);
    if (!d.exec()) {
        return;
    }
    last.cellWidth = cw->value();
    last.cellHeight = ch->value();
    last.offsetX = ox->value();
    last.offsetY = oy->value();
    last.spacingX = sx->value();
    last.spacingY = sy->value();
    last.skipEmpty = skip->isChecked();
    lastPivot = pivot->currentIndex();
    const std::vector<gas::IRect> rects = gas::gridSlices(document_->flattenedImage(), last);
    if (rects.empty()) {
        statusBar()->showMessage(tr("No cells (all empty, or the cell is larger than the canvas)"), 4000);
        return;
    }
    std::vector<gas::Slice> slices = replace->isChecked() ? std::vector<gas::Slice>{} : document_->slices();
    std::vector<std::string> taken;
    for (const gas::Slice& s : slices) {
        taken.push_back(s.name);
    }
    const std::string base = name->text().trimmed().isEmpty() ? "sprite" : name->text().trimmed().toStdString();
    for (gas::Slice s : gas::namedSlices(rects, base, pivotFromCombo(pivot, {0.5, 0.5}), static_cast<int>(slices.size()))) {
        s.name = gas::uniqueName(s.name, taken);
        taken.push_back(s.name);
        slices.push_back(std::move(s));
    }
    if (document_->setSlices(std::move(slices), "Grid Slice")) {
        onDocumentChanged();
        slicesDock_->raise();
    }
    statusBar()->showMessage(tr("%1 slices").arg(rects.size()), 4000);
}

void MainWindow::exportAtlas() {
    settleCanvas();
    if (document_->slices().empty() && document_->frameCount() < 2) {
        QMessageBox::information(this, tr("Export Atlas"),
                                 tr("There are no slices yet. Use the Slice tool (K), Image > Auto Slice or "
                                    "Image > Grid Slice to mark the sprites first, or add animation frames."));
        return;
    }
    FormDialog d(this, tr("Export Atlas"));
    auto* content = new QComboBox(&d.dialog);
    content->addItem(tr("Slices of this frame"));
    content->addItem(tr("Animation frames (every frame%1)")
                         .arg(document_->slices().empty() ? tr(", whole canvas") : tr(" of every slice")));
    if (atlasContent_ < 0) {
        atlasContent_ = document_->frameCount() > 1 ? 1 : 0;
    }
    content->setCurrentIndex(document_->slices().empty() ? 1 : atlasContent_);
    if (document_->slices().empty()) {
        content->setEnabled(false);
    }
    d.form->addRow(tr("Content:"), content);
    auto* framePivot = new QComboBox(&d.dialog);
    fillPivotCombo(framePivot);
    framePivot->removeItem(framePivot->count() - 1);
    framePivot->setCurrentIndex(atlasFramePivot_);
    framePivot->setToolTip(tr("One pivot for every frame, relative to the canvas (frames without slices)"));
    d.form->addRow(tr("Frame pivot:"), framePivot);
    auto* layout = new QComboBox(&d.dialog);
    layout->addItem(tr("Packed (MaxRects)"));
    layout->addItem(tr("Grid (equal cells, in order)"));
    layout->setCurrentIndex(atlasSettings_.grid ? 1 : 0);
    d.form->addRow(tr("Layout:"), layout);
    QSpinBox* columns = d.spin(tr("Grid columns:"), 0, 1000, atlasSettings_.columns);
    columns->setSpecialValueText(tr("Auto"));
    auto* preset = new QComboBox(&d.dialog);
    for (int i = 0; i < gas::kAtlasPresetCount; ++i) {
        preset->addItem(tr(gas::atlasPresetName(static_cast<gas::AtlasPreset>(i)).data()));
    }
    d.form->addRow(tr("Preset:"), preset);
    auto* format = new QComboBox(&d.dialog);
    for (int i = 0; i < gas::kAtlasFormatCount; ++i) {
        format->addItem(tr(gas::atlasFormatName(static_cast<gas::AtlasFormat>(i)).data()));
    }
    d.form->addRow(tr("Metadata:"), format);
    auto* maxSize = new QComboBox(&d.dialog);
    for (int s = 256; s <= kMaxCanvasSize; s *= 2) {
        maxSize->addItem(QStringLiteral("%1 \u00D7 %1").arg(s), s);
    }
    d.form->addRow(tr("Max page size:"), maxSize);
    QSpinBox* padding = d.spin(tr("Padding:"), 0, 64, 0, tr(" px"));
    padding->setToolTip(tr("Transparent pixels between sprites"));
    QSpinBox* border = d.spin(tr("Border:"), 0, 64, 0, tr(" px"));
    border->setToolTip(tr("Transparent pixels along the page edges"));
    QSpinBox* extrude = d.spin(tr("Extrude:"), 0, 16, 0, tr(" px"));
    extrude->setToolTip(tr("Repeat each sprite's edge pixels outward, so filtering and mipmaps never bleed in a neighbour"));
    QCheckBox* trim = d.check(tr("Trim transparent edges (offsets kept in the metadata)"), true);
    QCheckBox* pot = d.check(tr("Power-of-two page sizes"), true);
    QCheckBox* point = d.check(tr("Unity: point filter, no compression (pixel art)"), false);
    QSpinBox* ppu = d.spin(tr("Unity pixels per unit:"), 1, 10000, 100);

    auto* path = new QLineEdit(&d.dialog);
    auto* browse = new QPushButton(tr("Browse..."), &d.dialog);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(path, 1);
    pathRow->addWidget(browse);
    d.form->addRow(tr("File:"), pathRow);
    if (atlasPath_.isEmpty()) {
        const QString dir = filePath_.isEmpty() ? QDir::homePath() : QFileInfo(filePath_).path();
        atlasPath_ = QDir(dir).filePath(documentBaseName() + QStringLiteral(".png"));
    }
    path->setText(QDir::toNativeSeparators(atlasPath_));
    QObject::connect(browse, &QPushButton::clicked, &d.dialog, [&] {
        const QString chosen = QFileDialog::getSaveFileName(&d.dialog, tr("Atlas Image"), path->text(), tr("PNG image (*.png)"));
        if (!chosen.isEmpty()) {
            path->setText(QDir::toNativeSeparators(chosen));
        }
    });
    auto* note = new QLabel(&d.dialog);
    note->setWordWrap(true);
    d.form->addRow(note);

    const auto load = [&](const gas::AtlasSettings& s) {
        format->setCurrentIndex(static_cast<int>(s.format));
        maxSize->setCurrentIndex(std::max(0, maxSize->findData(s.maxWidth)));
        padding->setValue(s.padding);
        border->setValue(s.border);
        extrude->setValue(s.extrude);
        trim->setChecked(s.trim);
        pot->setChecked(s.powerOfTwo);
        point->setChecked(s.pointFilter);
        ppu->setValue(s.pixelsPerUnit);
    };
    const auto syncNote = [&] {
        const auto f = static_cast<gas::AtlasFormat>(format->currentIndex());
        const bool frames = content->currentIndex() == 1;
        framePivot->setEnabled(frames && document_->slices().empty());
        columns->setEnabled(layout->currentIndex() == 1);
        trim->setEnabled(layout->currentIndex() == 0);
        point->setEnabled(f == gas::AtlasFormat::UnityMeta);
        ppu->setEnabled(f == gas::AtlasFormat::UnityMeta);
        switch (f) {
        case gas::AtlasFormat::UnityMeta:
            note->setText(tr("Writes name.png + name.png.meta. Copy both into Assets/ (replace any existing meta) and "
                             "Unity imports a Multiple sprite sheet with these names and pivots."));
            break;
        case gas::AtlasFormat::CocosPlist:
            note->setText(tr("Writes name.png + name.plist. Drop both into the Cocos Creator assets panel to get a "
                             "Sprite Atlas. Pivots are not part of this format (set the node anchor)."));
            break;
        case gas::AtlasFormat::JsonHash:
            note->setText(tr("Writes name.png + name.json (JSON hash, with pivots), readable by Phaser, PixiJS and "
                             "most engines' atlas importers."));
            break;
        }
    };
    preset->setCurrentIndex(atlasPreset_);
    load(atlasSettings_);
    syncNote();
    QObject::connect(preset, &QComboBox::activated, &d.dialog, [&](int i) {
        load(gas::atlasPreset(static_cast<gas::AtlasPreset>(i)));
        syncNote();
    });
    QObject::connect(format, &QComboBox::currentIndexChanged, &d.dialog, syncNote);
    QObject::connect(content, &QComboBox::currentIndexChanged, &d.dialog, syncNote);
    QObject::connect(layout, &QComboBox::currentIndexChanged, &d.dialog, syncNote);
    syncNote();
    if (!d.exec()) {
        return;
    }
    gas::AtlasSettings s;
    s.format = static_cast<gas::AtlasFormat>(format->currentIndex());
    s.maxWidth = s.maxHeight = maxSize->currentData().toInt();
    s.padding = padding->value();
    s.border = border->value();
    s.extrude = extrude->value();
    s.trim = trim->isChecked();
    s.powerOfTwo = pot->isChecked();
    s.pointFilter = point->isChecked();
    s.pixelsPerUnit = ppu->value();
    s.grid = layout->currentIndex() == 1;
    s.columns = columns->value();
    atlasSettings_ = s;
    atlasContent_ = content->currentIndex();
    atlasFramePivot_ = framePivot->currentIndex();
    const bool animation = content->currentIndex() == 1;
    atlasPreset_ = preset->currentIndex();
    QString file = QDir::fromNativeSeparators(path->text().trimmed());
    if (file.isEmpty()) {
        return;
    }
    if (!file.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        file += QStringLiteral(".png");
    }
    atlasPath_ = file;

    gas::Atlas atlas = gas::packAtlas(
        animation ? gas::spritesFromFrames(*document_, QFileInfo(file).completeBaseName().toStdString(),
                                           pivotFromCombo(framePivot, {0.5, 1.0}))
                  : gas::spritesFromSlices(*document_),
        s);
    if (animation) {
        atlas.tags = gas::tagsForFrameSprites(*document_);
    }
    std::vector<std::filesystem::path> written;
    const gas::IoResult result = gas::exportAtlas(atlas, s, toFsPath(file), &written);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Export Failed"), tr("Could not export the atlas:\n%1").arg(QString::fromStdString(result.error)));
        return;
    }
    QStringList sizes;
    for (const gas::AtlasPage& p : atlas.pages) {
        sizes << QStringLiteral("%1\u00D7%2").arg(p.width).arg(p.height);
    }
    statusBar()->showMessage(tr("Exported %1 sprites on %2 page(s) (%3) to %4")
                                 .arg(atlas.frames.size())
                                 .arg(atlas.pages.size())
                                 .arg(sizes.join(QStringLiteral(", ")), QFileInfo(file).fileName()),
                             6000);
}

void MainWindow::exportSlices() {
    settleCanvas();
    if (document_->slices().empty()) {
        statusBar()->showMessage(tr("There are no slices to export"), 4000);
        return;
    }
    static bool lastTrim = false;
    static int lastScale = 1;
    FormDialog d(this, tr("Export Slices as PNGs"));
    auto* folder = new QLineEdit(&d.dialog);
    auto* browse = new QPushButton(tr("Browse..."), &d.dialog);
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    d.form->addRow(tr("Folder:"), row);
    if (exportDir_.isEmpty()) {
        exportDir_ = QDir(filePath_.isEmpty() ? QDir::homePath() : QFileInfo(filePath_).path()).filePath(documentBaseName());
    }
    folder->setText(QDir::toNativeSeparators(exportDir_));
    QObject::connect(browse, &QPushButton::clicked, &d.dialog, [&] {
        const QString chosen = QFileDialog::getExistingDirectory(&d.dialog, tr("Export Folder"), folder->text());
        if (!chosen.isEmpty()) {
            folder->setText(QDir::toNativeSeparators(chosen));
        }
    });
    QCheckBox* trim = d.check(tr("Trim transparent edges"), lastTrim);
    QSpinBox* scale = d.spin(tr("Scale (nearest neighbour):"), 1, 16, lastScale, tr("\u00D7"));
    if (!d.exec() || folder->text().trimmed().isEmpty()) {
        return;
    }
    lastTrim = trim->isChecked();
    lastScale = scale->value();
    exportDir_ = QDir::fromNativeSeparators(folder->text().trimmed());
    std::vector<std::filesystem::path> written;
    const gas::IoResult r = gas::exportSlicePngs(*document_, toFsPath(exportDir_), lastTrim, lastScale, &written);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Export Failed"), QString::fromStdString(r.error));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 PNGs to %2").arg(written.size()).arg(QDir::toNativeSeparators(exportDir_)), 6000);
}

void MainWindow::exportLayers() {
    settleCanvas();
    const QString start = exportDir_.isEmpty() ? (filePath_.isEmpty() ? QDir::homePath() : QFileInfo(filePath_).path()) : exportDir_;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export Layers to Folder"), start);
    if (dir.isEmpty()) {
        return;
    }
    exportDir_ = dir;
    std::vector<std::filesystem::path> written;
    const gas::IoResult r = gas::exportLayerPngs(*document_, toFsPath(dir), &written);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Export Failed"), QString::fromStdString(r.error));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 layers to %2").arg(written.size()).arg(QDir::toNativeSeparators(dir)), 6000);
}

void MainWindow::runFrameCommand(void (TimelinePanel::*command)()) {
    settleCanvas();
    if (canvas_->isBusy()) {
        return;
    }
    (timelinePanel_->*command)();
}

void MainWindow::exportFrames() {
    settleCanvas();
    static int lastScale = 1;
    FormDialog d(this, tr("Export Frames as PNGs"));
    auto* folder = new QLineEdit(&d.dialog);
    auto* browse = new QPushButton(tr("Browse..."), &d.dialog);
    auto* row = new QHBoxLayout;
    row->addWidget(folder, 1);
    row->addWidget(browse);
    d.form->addRow(tr("Folder:"), row);
    if (exportDir_.isEmpty()) {
        exportDir_ = QDir(filePath_.isEmpty() ? QDir::homePath() : QFileInfo(filePath_).path()).filePath(documentBaseName());
    }
    folder->setText(QDir::toNativeSeparators(exportDir_));
    QObject::connect(browse, &QPushButton::clicked, &d.dialog, [&] {
        const QString chosen = QFileDialog::getExistingDirectory(&d.dialog, tr("Export Folder"), folder->text());
        if (!chosen.isEmpty()) {
            folder->setText(QDir::toNativeSeparators(chosen));
        }
    });
    auto* name = new QLineEdit(documentBaseName(), &d.dialog);
    d.form->addRow(tr("File name prefix:"), name);
    QSpinBox* scale = d.spin(tr("Scale (nearest neighbour):"), 1, 16, lastScale, tr("\u00D7"));
    if (!d.exec() || folder->text().trimmed().isEmpty()) {
        return;
    }
    lastScale = scale->value();
    exportDir_ = QDir::fromNativeSeparators(folder->text().trimmed());
    const std::string base = name->text().trimmed().isEmpty() ? "frame" : name->text().trimmed().toStdString();
    std::vector<std::filesystem::path> written;
    const gas::IoResult r = gas::exportFramePngs(*document_, toFsPath(exportDir_), base, lastScale, &written);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Export Failed"), QString::fromStdString(r.error));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 frames to %2").arg(written.size()).arg(QDir::toNativeSeparators(exportDir_)), 6000);
}

void MainWindow::exportSvg() {
    settleCanvas();
    QString suggested = filePath_.isEmpty() ? tr("Untitled.svg") : filePath_;
    if (suggested.endsWith(QLatin1String(gas::kProjectExtension), Qt::CaseInsensitive)) {
        suggested.chop(static_cast<int>(std::char_traits<char>::length(gas::kProjectExtension)));
        suggested += QStringLiteral(".svg");
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Export SVG"), suggested, tr("SVG image (*.svg)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".svg");
    }
    const gas::IoResult result = gas::exportSvg(*document_, toFsPath(path));
    if (!result.ok) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not export \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(result.error)));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 (vector layers as paths, raster layers as images)")
                                 .arg(QFileInfo(path).fileName()),
                             5000);
}

void MainWindow::nineSlicePreview() {
    const int index = canvas_->selectedSlice();
    if (index < 0 || index >= static_cast<int>(document_->slices().size())) {
        return;
    }
    const gas::Slice slice = document_->slices()[static_cast<std::size_t>(index)];
    const std::vector<gas::Rgba8> pixels = gas::copyPixels(document_->flattenedImage(), slice.rect);
    static int lastW = 0, lastH = 0, lastZoom = 0; // 0 = fit the preview area
    static bool lastTile = false;

    QDialog d(this);
    d.setWindowTitle(tr("9-Slice Preview: %1").arg(QString::fromStdString(slice.name)));
    auto* width = new QSpinBox(&d);
    auto* height = new QSpinBox(&d);
    for (QSpinBox* b : {width, height}) {
        b->setRange(1, 2048);
        b->setSuffix(tr(" px"));
    }
    width->setValue(lastW > 0 ? lastW : slice.rect.w * 3);
    height->setValue(lastH > 0 ? lastH : slice.rect.h * 2);
    auto* zoom = new QSpinBox(&d);
    zoom->setRange(1, 16);
    zoom->setPrefix(QStringLiteral("×"));
    zoom->setValue(lastZoom > 0 ? lastZoom
                                : std::clamp(560 / std::max(width->value(), height->value()), 1, 8));
    auto* tile = new QCheckBox(tr("Tile edges and centre"), &d);
    tile->setChecked(lastTile);
    auto* view = new QLabel(&d);
    view->setAlignment(Qt::AlignCenter);
    auto* scroll = new QScrollArea(&d);
    scroll->setWidget(view);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(480, 320);
    scroll->setBackgroundRole(QPalette::Dark);
    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Size"), &d));
    controls->addWidget(width);
    controls->addWidget(new QLabel(QStringLiteral("×"), &d));
    controls->addWidget(height);
    controls->addSpacing(12);
    controls->addWidget(new QLabel(tr("Zoom"), &d));
    controls->addWidget(zoom);
    controls->addSpacing(12);
    controls->addWidget(tile);
    controls->addStretch(1);
    auto* note = new QLabel(slice.border.empty()
                                ? tr("This slice has no 9-slice border yet: it simply stretches.")
                                : tr("Border L %1, T %2, R %3, B %4 px")
                                      .arg(slice.border.left)
                                      .arg(slice.border.top)
                                      .arg(slice.border.right)
                                      .arg(slice.border.bottom),
                            &d);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    auto* layout = new QVBoxLayout(&d);
    layout->addLayout(controls);
    layout->addWidget(scroll, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);

    const auto render = [&] {
        const int w = width->value(), h = height->value(), z = zoom->value();
        const std::vector<gas::Rgba8> out =
            gas::nineSliceScale(pixels, slice.rect.w, slice.rect.h, slice.border, w, h, tile->isChecked());
        QImage img(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y) {
            std::memcpy(img.scanLine(y), out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w),
                        static_cast<std::size_t>(w) * sizeof(gas::Rgba8));
        }
        // On a checkerboard, nearest-neighbour zoom like the canvas.
        QPixmap pm(w * z, h * z);
        QPainter p(&pm);
        for (int y = 0; y < pm.height(); y += 8) {
            for (int x = 0; x < pm.width(); x += 8) {
                p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(0x99, 0x99, 0x99) : QColor(0xcc, 0xcc, 0xcc));
            }
        }
        p.drawImage(QRect(0, 0, w * z, h * z), img);
        p.end();
        view->setPixmap(pm);
    };
    for (QSpinBox* b : {width, height, zoom}) {
        QObject::connect(b, &QSpinBox::valueChanged, &d, render);
    }
    QObject::connect(tile, &QCheckBox::toggled, &d, render);
    render();
    d.exec();
    lastW = width->value();
    lastH = height->value();
    lastZoom = zoom->value();
    lastTile = tile->isChecked();
}

// ---------------------------------------------------------------- crash recovery

QString MainWindow::recoveryFolder() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/recovery");
}

void MainWindow::autosave() {
    if (!document_ || !recovery_) {
        return;
    }
    if (!document_->isModified()) {
        clearAutosave(); // e.g. undone back to the saved state
        return;
    }
    if (document_->historyGeneration() == autosavedGeneration_ || canvas_->isBusy() || canvas_->isTransforming()) {
        return; // nothing new, or mid-stroke / mid-transform: try again next tick
    }
    const QString title = filePath_.isEmpty() ? tr("Untitled") : QFileInfo(filePath_).fileName();
    const QByteArray original = filePath_.toUtf8();
    const gas::IoResult r =
        recovery_->save(*document_, session_, std::u8string(original.begin(), original.end()), title.toStdString(),
                        QDateTime::currentSecsSinceEpoch());
    if (r.ok) {
        autosavedGeneration_ = document_->historyGeneration();
        autosaveExists_ = true;
        statusBar()->showMessage(tr("Autosaved"), 1500);
    } else {
        statusBar()->showMessage(tr("Autosave failed: %1").arg(QString::fromStdString(r.error)), 5000);
    }
}

void MainWindow::clearAutosave() {
    if (recovery_ && autosaveExists_) {
        recovery_->remove(session_);
        autosaveExists_ = false;
    }
    autosavedGeneration_ = ~0u;
}

void MainWindow::offerRecovery() {
    if (!recovery_) {
        return;
    }
    // Sessions whose lock can be taken belong to windows that are gone.
    std::vector<gas::RecoveryEntry> orphans;
    for (gas::RecoveryEntry& e : recovery_->entries()) {
        if (e.session == session_) {
            continue;
        }
        QLockFile probe(recoveryFolder() + QStringLiteral("/%1.lock").arg(QString::fromStdString(e.session)));
        probe.setStaleLockTime(0); // only a dead owner makes a lock stale, never its age
        if (probe.tryLock(0)) {
            probe.unlock(); // removes the lock file
            orphans.push_back(std::move(e));
        }
    }
    if (orphans.empty()) {
        return;
    }
    RecoveryDialog dialog(*recovery_, std::move(orphans), this);
    if (dialog.exec() != QDialog::Accepted || dialog.choice() != RecoveryDialog::Choice::Recover) {
        return;
    }
    const gas::RecoveryEntry entry = dialog.selected();
    if (!maybeSave()) {
        return;
    }
    gas::LoadResult loaded = gas::loadProject(entry.project);
    if (!loaded.document) {
        QMessageBox::warning(this, tr("Recovery Failed"),
                             tr("The autosave could not be opened:\n%1").arg(QString::fromStdString(loaded.error)));
        return;
    }
    const QString original = QString::fromUtf8(reinterpret_cast<const char*>(entry.originalPath.data()),
                                               static_cast<qsizetype>(entry.originalPath.size()));
    setDocument(std::move(loaded.document), original);
    document_->markUnsaved(); // differs from the file on disk (if any) until saved
    onDocumentChanged();
    // Move it into this session's slot before deleting the old one, so a
    // second crash can't lose it.
    autosave();
    if (autosaveExists_) {
        recovery_->remove(entry.session);
    }
    statusBar()->showMessage(tr("Recovered \"%1\". Save it to keep the changes.")
                                 .arg(original.isEmpty() ? tr("Untitled") : QFileInfo(original).fileName()),
                             8000);
}

// ---------------------------------------------------------------- plugins

QString MainWindow::userPluginFolder() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/plugins");
}

void MainWindow::loadPlugins() {
    plugins_ = std::make_unique<gas::PluginHost>();
    plugins_->loadDirectory(toFsPath(QCoreApplication::applicationDirPath() + QStringLiteral("/plugins")));
    QDir().mkpath(userPluginFolder());
    plugins_->loadDirectory(toFsPath(userPluginFolder()));

    pluginsMenu_ = menuBar()->addMenu(tr("&Plugins"));
    const auto& filters = plugins_->filters();
    for (int i = 0; i < static_cast<int>(filters.size()); ++i) {
        const gas::PluginFilter& f = filters[static_cast<std::size_t>(i)];
        QAction* a = pluginsMenu_->addAction(QString::fromStdString(f.name) + (f.params.empty() ? QString()
                                                                                                : QStringLiteral("...")),
                                             this, [this, i] { runPluginFilter(i); });
        a->setToolTip(QString::fromStdString(f.description));
        a->setStatusTip(QString::fromStdString(f.description));
        documentActions_ << a;
    }
    if (filters.empty()) {
        pluginsMenu_->addAction(tr("(no filter plugins)"))->setEnabled(false);
    }
    const auto& exporters = plugins_->exporters();
    if (!exporters.empty()) {
        pluginsMenu_->addSeparator();
        for (int i = 0; i < static_cast<int>(exporters.size()); ++i) {
            documentActions_ << pluginsMenu_->addAction(
                tr("Export %1...").arg(QString::fromStdString(exporters[static_cast<std::size_t>(i)].name)), this,
                [this, i] { runPluginExporter(i); });
        }
    }
    pluginsMenu_->addSeparator();
    pluginsMenu_->addAction(tr("Open Plugins Folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPluginFolder()));
    });
    pluginsMenu_->addAction(tr("About Plugins..."), this,
                            [this] { showPluginInfo(this, *plugins_, QDir::toNativeSeparators(userPluginFolder())); });
}

void MainWindow::runPluginFilter(int index) {
    const gas::PluginFilter& filter = plugins_->filters().at(static_cast<std::size_t>(index));
    settleCanvas();
    if (canvas_->isBusy()) {
        return;
    }
    auto it = filterValues_.find(index);
    std::vector<double> values = it != filterValues_.end() ? *it : filter.defaults();
    if (!filter.params.empty()) {
        if (!editFilterParams(this, filter, values)) {
            return;
        }
        filterValues_[index] = values;
    }
    const gas::IoResult r = gas::applyPluginFilter(*document_, filter, values);
    if (!r.ok) {
        QMessageBox::warning(this, QString::fromStdString(filter.name), QString::fromStdString(r.error));
    }
    onDocumentChanged();
}

void MainWindow::runPluginExporter(int index) {
    const gas::PluginExporter& exporter = plugins_->exporters().at(static_cast<std::size_t>(index));
    settleCanvas();
    const QString ext = QString::fromStdString(exporter.extension);
    QString suggested = filePath_.isEmpty() ? tr("Untitled") : filePath_;
    if (suggested.endsWith(QLatin1String(gas::kProjectExtension), Qt::CaseInsensitive)) {
        suggested.chop(static_cast<int>(std::char_traits<char>::length(gas::kProjectExtension)));
    }
    suggested += ext;
    const QString filter = exporter.fileFilter.empty() ? tr("All files (*)") : QString::fromStdString(exporter.fileFilter);
    QString path = QFileDialog::getSaveFileName(this, tr("Export %1").arg(QString::fromStdString(exporter.name)),
                                                suggested, filter);
    if (path.isEmpty()) {
        return;
    }
    if (!ext.isEmpty() && !path.endsWith(ext, Qt::CaseInsensitive)) {
        path += ext;
    }
    const gas::IoResult r = gas::runPluginExporter(*document_, exporter, toFsPath(path));
    if (!r.ok) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not export \"%1\":\n%2")
                                 .arg(QFileInfo(path).fileName(), QString::fromStdString(r.error)));
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3000);
}
