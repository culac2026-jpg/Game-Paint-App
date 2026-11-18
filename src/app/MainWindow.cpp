#include "MainWindow.h"

#include "CanvasWidget.h"

#include <gas/core/Document.h>

#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenuBar>
#include <QSpinBox>
#include <QStatusBar>

namespace {
constexpr int kDefaultCanvasSize = 1024;
constexpr int kMaxCanvasSize = 16384;
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

    createActions();
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

    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("Pencil &Color..."), QKeySequence(Qt::Key_C), this, &MainWindow::chooseColor);
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
    const gas::Rgba8 c = canvas_->pencilColor();
    const QColor chosen = QColorDialog::getColor(QColor(c.r, c.g, c.b, c.a), this, tr("Pencil Color"),
                                                 QColorDialog::ShowAlphaChannel);
    if (chosen.isValid()) {
        canvas_->setPencilColor({static_cast<std::uint8_t>(chosen.red()),
                                 static_cast<std::uint8_t>(chosen.green()),
                                 static_cast<std::uint8_t>(chosen.blue()),
                                 static_cast<std::uint8_t>(chosen.alpha())});
    }
}

void MainWindow::updateStatus() {
    zoomLabel_->setText(tr("%1%").arg(canvas_->zoom() * 100.0, 0, 'f', 1));
    if (const auto p = canvas_->cursorPixel()) {
        cursorLabel_->setText(tr("X: %1  Y: %2").arg(p->x).arg(p->y));
    } else {
        cursorLabel_->clear();
    }
}
