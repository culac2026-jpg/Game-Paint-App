#include "SlicesPanel.h"

#include "CanvasWidget.h"

#include <gas/core/Document.h>
#include <gas/core/Slicing.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace {

struct PivotPreset {
    const char* name;
    double x, y;
};

constexpr PivotPreset kPivots[] = {
    {QT_TRANSLATE_NOOP("SlicesPanel", "Center"), 0.5, 0.5},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Bottom center"), 0.5, 1.0},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Top left"), 0.0, 0.0},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Top center"), 0.5, 0.0},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Top right"), 1.0, 0.0},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Left center"), 0.0, 0.5},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Right center"), 1.0, 0.5},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Bottom left"), 0.0, 1.0},
    {QT_TRANSLATE_NOOP("SlicesPanel", "Bottom right"), 1.0, 1.0},
};
constexpr int kCustomPivot = static_cast<int>(std::size(kPivots));

QSpinBox* coordBox(QWidget* parent, int min) {
    auto* box = new QSpinBox(parent);
    box->setRange(min, gas::Document::kMaxSize);
    box->setKeyboardTracking(false);
    box->setSuffix(QStringLiteral(" px"));
    return box;
}

QDoubleSpinBox* pivotCoordBox(QWidget* parent, const QString& tip) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(-4.0, 5.0);
    box->setDecimals(3);
    box->setSingleStep(0.05);
    box->setKeyboardTracking(false);
    box->setToolTip(tip);
    return box;
}

} // namespace

void fillPivotCombo(QComboBox* combo) {
    for (const PivotPreset& p : kPivots) {
        combo->addItem(SlicesPanel::tr(p.name));
    }
    combo->addItem(SlicesPanel::tr("Custom"));
}

gas::PointF pivotFromCombo(const QComboBox* combo, gas::PointF fallback) {
    const int i = combo->currentIndex();
    return i >= 0 && i < kCustomPivot ? gas::PointF{kPivots[i].x, kPivots[i].y} : fallback;
}

void selectPivotInCombo(QComboBox* combo, gas::PointF pivot) {
    int index = kCustomPivot;
    for (int i = 0; i < kCustomPivot; ++i) {
        if (std::abs(kPivots[i].x - pivot.x) < 1e-6 && std::abs(kPivots[i].y - pivot.y) < 1e-6) {
            index = i;
        }
    }
    const QSignalBlocker block(combo);
    combo->setCurrentIndex(index);
}

SlicesPanel::SlicesPanel(CanvasWidget* canvas, QWidget* parent) : QWidget(parent), canvas_(canvas) {
    list_ = new QListWidget(this);
    list_->setFocusPolicy(Qt::NoFocus); // keep single-key tool shortcuts working
    list_->setMinimumHeight(120);

    auto* autoButton = new QPushButton(tr("Auto..."), this);
    autoButton->setToolTip(tr("Find every separate sprite automatically (connected opaque pixels)"));
    auto* gridButton = new QPushButton(tr("Grid..."), this);
    gridButton->setToolTip(tr("Cut the canvas into equal cells"));
    exportButton_ = new QPushButton(tr("Export Atlas..."), this);
    exportButton_->setToolTip(tr("Pack the slices into a sprite atlas (PNG + metadata)"));

    fields_ = new QWidget(this);
    nameEdit_ = new QLineEdit(fields_);
    xBox_ = coordBox(fields_, 0);
    yBox_ = coordBox(fields_, 0);
    wBox_ = coordBox(fields_, 1);
    hBox_ = coordBox(fields_, 1);
    pivotBox_ = new QComboBox(fields_);
    pivotBox_->setFocusPolicy(Qt::ClickFocus);
    fillPivotCombo(pivotBox_);
    pivotXBox_ = pivotCoordBox(fields_, tr("Pivot X (0 = left edge, 1 = right edge)"));
    pivotYBox_ = pivotCoordBox(fields_, tr("Pivot Y (0 = top edge, 1 = bottom edge)"));

    auto* rectRow = new QGridLayout;
    rectRow->setContentsMargins(0, 0, 0, 0);
    rectRow->addWidget(new QLabel(tr("X"), fields_), 0, 0);
    rectRow->addWidget(xBox_, 0, 1);
    rectRow->addWidget(new QLabel(tr("Y"), fields_), 0, 2);
    rectRow->addWidget(yBox_, 0, 3);
    rectRow->addWidget(new QLabel(tr("W"), fields_), 1, 0);
    rectRow->addWidget(wBox_, 1, 1);
    rectRow->addWidget(new QLabel(tr("H"), fields_), 1, 2);
    rectRow->addWidget(hBox_, 1, 3);
    auto* pivotRow = new QHBoxLayout;
    pivotRow->setContentsMargins(0, 0, 0, 0);
    pivotRow->addWidget(pivotXBox_);
    pivotRow->addWidget(pivotYBox_);
    static const char* kBorderTips[4] = {QT_TRANSLATE_NOOP("SlicesPanel", "Left border (px)"),
                                         QT_TRANSLATE_NOOP("SlicesPanel", "Top border (px)"),
                                         QT_TRANSLATE_NOOP("SlicesPanel", "Right border (px)"),
                                         QT_TRANSLATE_NOOP("SlicesPanel", "Bottom border (px)")};
    auto* borderRow = new QHBoxLayout;
    borderRow->setContentsMargins(0, 0, 0, 0);
    for (int k = 0; k < 4; ++k) {
        borderBoxes_[k] = new QSpinBox(fields_);
        borderBoxes_[k]->setRange(0, gas::Document::kMaxSize);
        borderBoxes_[k]->setKeyboardTracking(false);
        borderBoxes_[k]->setToolTip(tr(kBorderTips[k]));
        borderRow->addWidget(borderBoxes_[k]);
    }
    previewButton_ = new QPushButton(tr("Preview..."), fields_);
    previewButton_->setToolTip(tr("See the slice stretched like a 9-slice sprite in a game engine"));
    auto* form = new QFormLayout(fields_);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Name"), nameEdit_);
    form->addRow(rectRow);
    form->addRow(tr("Pivot"), pivotBox_);
    form->addRow(QString(), pivotRow);
    auto* nineLabel = new QLabel(tr("9-slice"), fields_);
    nineLabel->setToolTip(tr("Borders in pixels (left, top, right, bottom): corners keep their size, edges and centre "
                             "stretch. With the Slice tool, drag the dashed guides; Alt+drag an edge to add one."));
    form->addRow(nineLabel, borderRow);
    form->addRow(QString(), previewButton_);

    trimButton_ = new QPushButton(tr("Trim"), this);
    trimButton_->setToolTip(tr("Shrink the selected slice (or every slice if none is selected) to its opaque "
                               "pixels; pivots stay on the same canvas point"));
    pivotAllButton_ = new QPushButton(tr("Pivot to All"), this);
    pivotAllButton_->setToolTip(tr("Give every slice the selected slice's pivot"));
    deleteButton_ = new QPushButton(tr("Delete"), this);
    clearButton_ = new QPushButton(tr("Clear All"), this);

    auto* top = new QHBoxLayout;
    top->addWidget(autoButton);
    top->addWidget(gridButton);
    auto* bottom = new QGridLayout;
    bottom->addWidget(trimButton_, 0, 0);
    bottom->addWidget(pivotAllButton_, 0, 1);
    bottom->addWidget(deleteButton_, 1, 0);
    bottom->addWidget(clearButton_, 1, 1);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(list_, 1);
    layout->addWidget(fields_);
    layout->addLayout(bottom);
    layout->addWidget(exportButton_);

    connect(autoButton, &QPushButton::clicked, this, &SlicesPanel::autoSliceRequested);
    connect(gridButton, &QPushButton::clicked, this, &SlicesPanel::gridSliceRequested);
    connect(exportButton_, &QPushButton::clicked, this, &SlicesPanel::exportAtlasRequested);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        canvas_->setSelectedSlice(row);
        syncFields();
    });
    connect(canvas_, &CanvasWidget::selectedSliceChanged, this, [this](int index) {
        const QSignalBlocker block(list_);
        list_->setCurrentRow(index);
        syncFields();
    });
    connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
        const std::string name = nameEdit_->text().trimmed().toStdString();
        if (!name.empty()) {
            editSelected("Rename Slice", false, [&](gas::Slice& s) { s.name = name; });
        }
    });
    connect(xBox_, &QSpinBox::valueChanged, this,
            [this](int v) { editSelected("Slice Position", true, [v](gas::Slice& s) { s.rect.x = v; }); });
    connect(yBox_, &QSpinBox::valueChanged, this,
            [this](int v) { editSelected("Slice Position", true, [v](gas::Slice& s) { s.rect.y = v; }); });
    connect(wBox_, &QSpinBox::valueChanged, this,
            [this](int v) { editSelected("Slice Size", true, [v](gas::Slice& s) { s.rect.w = v; }); });
    connect(hBox_, &QSpinBox::valueChanged, this,
            [this](int v) { editSelected("Slice Size", true, [v](gas::Slice& s) { s.rect.h = v; }); });
    connect(pivotXBox_, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { editSelected("Slice Pivot", true, [v](gas::Slice& s) { s.pivot.x = v; }); });
    connect(pivotYBox_, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { editSelected("Slice Pivot", true, [v](gas::Slice& s) { s.pivot.y = v; }); });
    connect(pivotBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i < kCustomPivot) {
            editSelected("Slice Pivot", false, [i](gas::Slice& s) { s.pivot = {kPivots[i].x, kPivots[i].y}; });
        }
    });
    for (int k = 0; k < 4; ++k) {
        connect(borderBoxes_[k], &QSpinBox::valueChanged, this, [this, k](int v) {
            editSelected("9-Slice Border", true, [k, v](gas::Slice& s) {
                int* fields[4] = {&s.border.left, &s.border.top, &s.border.right, &s.border.bottom};
                *fields[k] = v;
            });
        });
    }
    connect(previewButton_, &QPushButton::clicked, this, &SlicesPanel::nineSlicePreviewRequested);
    connect(trimButton_, &QPushButton::clicked, this, &SlicesPanel::trim);
    connect(pivotAllButton_, &QPushButton::clicked, this, &SlicesPanel::applyPivotToAll);
    connect(deleteButton_, &QPushButton::clicked, this, [this] {
        if (canvas_->deleteSelectedSlice()) {
            Q_EMIT documentChanged();
        }
    });
    connect(clearButton_, &QPushButton::clicked, this, &SlicesPanel::clearAll);
    refresh();
}

void SlicesPanel::setDocument(gas::Document* document) {
    document_ = document;
    refresh();
}

void SlicesPanel::refresh() {
    const QSignalBlocker block(list_);
    list_->clear();
    if (document_) {
        for (const gas::Slice& s : document_->slices()) {
            list_->addItem(QStringLiteral("%1   %2×%3%4")
                               .arg(QString::fromStdString(s.name))
                               .arg(s.rect.w)
                               .arg(s.rect.h)
                               .arg(s.border.empty() ? QString() : tr("   9-slice")));
        }
        canvas_->setSelectedSlice(canvas_->selectedSlice()); // drops an index that no longer exists
        list_->setCurrentRow(canvas_->selectedSlice());
    }
    const bool any = document_ && !document_->slices().empty();
    clearButton_->setEnabled(any);
    trimButton_->setEnabled(any);
    exportButton_->setEnabled(any);
    syncFields();
}

void SlicesPanel::syncFields() {
    const int i = canvas_->selectedSlice();
    const bool has = document_ && i >= 0 && i < static_cast<int>(document_->slices().size());
    fields_->setEnabled(has);
    deleteButton_->setEnabled(has);
    pivotAllButton_->setEnabled(has);
    if (!has) {
        return;
    }
    const gas::Slice& s = document_->slices()[static_cast<std::size_t>(i)];
    const QSignalBlocker b1(nameEdit_), b2(xBox_), b3(yBox_), b4(wBox_), b5(hBox_), b6(pivotXBox_), b7(pivotYBox_);
    const int borders[4] = {s.border.left, s.border.top, s.border.right, s.border.bottom};
    for (int k = 0; k < 4; ++k) {
        const QSignalBlocker block(borderBoxes_[k]);
        borderBoxes_[k]->setMaximum(k % 2 == 0 ? s.rect.w : s.rect.h);
        borderBoxes_[k]->setValue(borders[k]);
    }
    if (!nameEdit_->hasFocus()) {
        nameEdit_->setText(QString::fromStdString(s.name));
    }
    xBox_->setMaximum(document_->width() - 1);
    yBox_->setMaximum(document_->height() - 1);
    wBox_->setMaximum(document_->width());
    hBox_->setMaximum(document_->height());
    xBox_->setValue(s.rect.x);
    yBox_->setValue(s.rect.y);
    wBox_->setValue(s.rect.w);
    hBox_->setValue(s.rect.h);
    pivotXBox_->setValue(s.pivot.x);
    pivotYBox_->setValue(s.pivot.y);
    selectPivotInCombo(pivotBox_, s.pivot);
}

template <typename Fn>
void SlicesPanel::editSelected(const char* label, bool merge, Fn&& edit) {
    const int i = canvas_->selectedSlice();
    if (!document_ || canvas_->isBusy() || i < 0 || i >= static_cast<int>(document_->slices().size())) {
        return;
    }
    std::vector<gas::Slice> slices = document_->slices();
    edit(slices[static_cast<std::size_t>(i)]);
    if (document_->setSlices(std::move(slices), label, merge)) {
        Q_EMIT documentChanged();
    } else {
        syncFields(); // e.g. clipped back to the canvas: show the real values
    }
}

void SlicesPanel::trim() {
    if (!document_ || canvas_->isBusy()) {
        return;
    }
    const gas::TiledImage image = document_->flattenedImage();
    std::vector<gas::Slice> slices = document_->slices();
    const int selected = canvas_->selectedSlice();
    for (int i = 0; i < static_cast<int>(slices.size()); ++i) {
        if (selected >= 0 && i != selected) {
            continue;
        }
        gas::Slice& s = slices[static_cast<std::size_t>(i)];
        const gas::IRect t = gas::trimmedRect(image, s.rect);
        if (t.empty() || !s.border.empty()) {
            continue; // nothing painted, or a 9-slice sprite (its borders are measured from the full rect)
        }
        const double px = s.rect.x + s.pivot.x * s.rect.w, py = s.rect.y + s.pivot.y * s.rect.h;
        s.rect = t;
        s.pivot = {(px - t.x) / t.w, (py - t.y) / t.h};
    }
    if (document_->setSlices(std::move(slices), "Trim Slices")) {
        Q_EMIT documentChanged();
    }
}

void SlicesPanel::applyPivotToAll() {
    const int i = canvas_->selectedSlice();
    if (!document_ || canvas_->isBusy() || i < 0 || i >= static_cast<int>(document_->slices().size())) {
        return;
    }
    std::vector<gas::Slice> slices = document_->slices();
    const gas::PointF pivot = slices[static_cast<std::size_t>(i)].pivot;
    for (gas::Slice& s : slices) {
        s.pivot = pivot;
    }
    if (document_->setSlices(std::move(slices), "Slice Pivots")) {
        Q_EMIT documentChanged();
    }
}

void SlicesPanel::clearAll() {
    if (document_ && !canvas_->isBusy() && document_->setSlices({}, "Clear Slices")) {
        canvas_->setSelectedSlice(-1);
        Q_EMIT documentChanged();
    }
}
