#include "LayersPanel.h"

#include <gas/core/Document.h>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

QToolButton* makeButton(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

} // namespace

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::DoubleClicked);
    // Keyboard focus would turn single-key tool shortcuts into item searches.
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setMinimumHeight(130); // ~4 rows even when the docks below are tall

    opacity_ = new QSpinBox(this);
    opacity_->setRange(0, 100);
    opacity_->setSuffix(tr("%"));
    opacity_->setKeyboardTracking(false);
    opacity_->setFocusPolicy(Qt::ClickFocus);
    lock_ = new QCheckBox(tr("Lock"), this);
    lock_->setFocusPolicy(Qt::NoFocus);
    lock_->setToolTip(tr("Prevent painting on this layer"));

    blendMode_ = new QComboBox(this);
    blendMode_->setFocusPolicy(Qt::NoFocus);
    blendMode_->setToolTip(tr("How this layer mixes with the layers below"));
    for (int m = 0; m < gas::kBlendModeCount; ++m) {
        blendMode_->addItem(tr(gas::blendModeName(static_cast<gas::BlendMode>(m)).data()), m);
    }
    auto* blendRow = new QHBoxLayout;
    blendRow->addWidget(new QLabel(tr("Blend"), this));
    blendRow->addWidget(blendMode_, 1);

    auto* props = new QHBoxLayout;
    props->addWidget(new QLabel(tr("Opacity"), this));
    props->addWidget(opacity_);
    props->addStretch();
    props->addWidget(lock_);

    addButton_ = makeButton(this, tr("New"), tr("New layer"));
    duplicateButton_ = makeButton(this, tr("Dup"), tr("Duplicate layer"));
    deleteButton_ = makeButton(this, tr("Del"), tr("Delete layer"));
    upButton_ = makeButton(this, QStringLiteral("▲"), tr("Move layer up"));
    downButton_ = makeButton(this, QStringLiteral("▼"), tr("Move layer down"));
    auto* buttons = new QHBoxLayout;
    for (QToolButton* b : {addButton_, duplicateButton_, deleteButton_}) {
        buttons->addWidget(b);
    }
    buttons->addStretch();
    buttons->addWidget(upButton_);
    buttons->addWidget(downButton_);

    addMaskButton_ = makeButton(this, tr("Add Mask"),
                                tr("Add a layer mask (from the selection if there is one)"));
    editMask_ = new QCheckBox(tr("Edit mask"), this);
    editMask_->setFocusPolicy(Qt::NoFocus);
    editMask_->setToolTip(tr("Paint on the mask: black hides, white or erasing shows"));
    maskEnabled_ = new QCheckBox(tr("On"), this);
    maskEnabled_->setFocusPolicy(Qt::NoFocus);
    maskEnabled_->setToolTip(tr("Enable or disable the mask without deleting it"));
    applyMaskButton_ = makeButton(this, tr("Apply"), tr("Bake the mask into the layer's transparency"));
    removeMaskButton_ = makeButton(this, tr("Remove"), tr("Delete the mask"));
    auto* maskRow = new QHBoxLayout;
    maskRow->addWidget(addMaskButton_);
    maskRow->addWidget(editMask_);
    maskRow->addWidget(maskEnabled_);
    maskRow->addStretch();
    maskRow->addWidget(applyMaskButton_);
    maskRow->addWidget(removeMaskButton_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(blendRow);
    layout->addLayout(props);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addLayout(maskRow);

    connect(list_, &QListWidget::itemChanged, this, &LayersPanel::onItemChanged);
    connect(list_, &QListWidget::currentRowChanged, this, &LayersPanel::onCurrentRowChanged);
    connect(opacity_, &QSpinBox::valueChanged, this, &LayersPanel::setOpacity);
    connect(lock_, &QCheckBox::toggled, this, &LayersPanel::setLocked);
    connect(blendMode_, &QComboBox::currentIndexChanged, this, &LayersPanel::setBlendMode);
    connect(addButton_, &QToolButton::clicked, this, [this] {
        document_->addLayer();
        Q_EMIT documentChanged();
    });
    connect(duplicateButton_, &QToolButton::clicked, this, [this] {
        if (document_->duplicateLayer(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });
    connect(deleteButton_, &QToolButton::clicked, this, [this] {
        if (document_->removeLayer(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });
    connect(upButton_, &QToolButton::clicked, this, [this] { moveActive(+1); });
    connect(downButton_, &QToolButton::clicked, this, [this] { moveActive(-1); });
    connect(addMaskButton_, &QToolButton::clicked, this, [this] {
        if (document_->addLayerMask(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });
    connect(editMask_, &QCheckBox::toggled, this, [this](bool on) {
        document_->setEditingMask(on);
        Q_EMIT documentChanged();
    });
    connect(maskEnabled_, &QCheckBox::toggled, this, &LayersPanel::setMaskEnabled);
    connect(applyMaskButton_, &QToolButton::clicked, this, [this] {
        if (document_->applyLayerMask(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });
    connect(removeMaskButton_, &QToolButton::clicked, this, [this] {
        if (document_->removeLayerMask(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });

    setEnabled(false);
}

void LayersPanel::setDocument(gas::Document* document) {
    document_ = document;
    setEnabled(document_ != nullptr);
    refresh();
}

// The list shows the top layer first; document index 0 is the bottom.
int LayersPanel::rowToIndex(int row) const { return document_->layerCount() - 1 - row; }
int LayersPanel::indexToRow(int index) const { return document_->layerCount() - 1 - index; }

void LayersPanel::refresh() {
    const QSignalBlocker blockList(list_);
    list_->clear();
    if (!document_) {
        return;
    }
    for (int row = 0; row < document_->layerCount(); ++row) {
        const gas::Layer& layer = document_->layer(rowToIndex(row));
        const gas::LayerProps& p = layer.props;
        auto* item = new QListWidgetItem(QString::fromStdString(p.name), list_);
        if (layer.mask) {
            // Masked layers are tinted blue (the name itself stays plain for renaming).
            item->setData(Qt::ToolTipRole, tr("Has a layer mask%1. Double-click to rename.")
                                               .arg(p.maskEnabled ? QString() : tr(" (disabled)")));
        }
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setCheckState(p.visible ? Qt::Checked : Qt::Unchecked);
        if (!layer.mask) {
            item->setToolTip(tr("Double-click to rename; checkbox toggles visibility"));
        }
        item->setForeground(layer.mask && p.maskEnabled ? QBrush(QColor(0x3d, 0x8b, 0xd6)) : QBrush());
        QFont font = item->font();
        font.setItalic(p.locked); // locked layers read differently at a glance
        item->setFont(font);
        if (layer.vector) {
            // Vector layers carry a small node-diamond icon.
            static const QIcon vectorIcon = [] {
                QPixmap pm(14, 14);
                pm.fill(Qt::transparent);
                QPainter painter(&pm);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(QPen(QColor(255, 150, 20), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(QPolygonF({QPointF(7, 1.5), QPointF(12.5, 7), QPointF(7, 12.5), QPointF(1.5, 7)}));
                painter.setBrush(QColor(255, 150, 20));
                painter.drawRect(QRectF(5.5, 5.5, 3, 3));
                return QIcon(pm);
            }();
            item->setIcon(vectorIcon);
            item->setToolTip(tr("Vector layer: edit with the Pen, Shape and Node tools. Double-click to rename."));
        }
    }
    const int active = document_->activeLayerIndex();
    list_->setCurrentRow(indexToRow(active));

    const gas::LayerProps& p = document_->activeLayer().props;
    {
        const QSignalBlocker b1(opacity_), b2(lock_), b5(blendMode_);
        opacity_->setValue(static_cast<int>(std::lround(p.opacity * 100.0)));
        lock_->setChecked(p.locked);
        blendMode_->setCurrentIndex(static_cast<int>(p.blendMode));
    }
    const bool hasMask = document_->activeLayer().mask.has_value();
    {
        const QSignalBlocker b3(editMask_), b4(maskEnabled_);
        editMask_->setChecked(document_->editingMask());
        maskEnabled_->setChecked(hasMask && p.maskEnabled);
    }
    addMaskButton_->setEnabled(!hasMask);
    editMask_->setEnabled(hasMask);
    maskEnabled_->setEnabled(hasMask);
    applyMaskButton_->setEnabled(hasMask);
    removeMaskButton_->setEnabled(hasMask);

    const int count = document_->layerCount();
    deleteButton_->setEnabled(count > 1);
    duplicateButton_->setEnabled(count < gas::Document::kMaxLayers);
    addButton_->setEnabled(count < gas::Document::kMaxLayers);
    upButton_->setEnabled(active < count - 1);
    downButton_->setEnabled(active > 0);
}

void LayersPanel::onItemChanged(QListWidgetItem* item) {
    const int index = rowToIndex(list_->row(item));
    gas::LayerProps p = document_->layer(index).props;
    p.visible = item->checkState() == Qt::Checked;
    const std::string name = item->text().trimmed().toStdString();
    if (!name.empty()) {
        p.name = name;
    }
    document_->setLayerProps(index, p);
    Q_EMIT documentChanged(); // refresh() also restores an emptied name
}

void LayersPanel::onCurrentRowChanged(int row) {
    if (row < 0) {
        return;
    }
    document_->setActiveLayer(rowToIndex(row));
    Q_EMIT documentChanged();
}

void LayersPanel::setOpacity(int percent) {
    gas::LayerProps p = document_->activeLayer().props;
    p.opacity = percent / 100.0;
    if (document_->setLayerProps(document_->activeLayerIndex(), p)) {
        Q_EMIT documentChanged();
    }
}

void LayersPanel::setLocked(bool locked) {
    gas::LayerProps p = document_->activeLayer().props;
    p.locked = locked;
    if (document_->setLayerProps(document_->activeLayerIndex(), p)) {
        Q_EMIT documentChanged();
    }
}

void LayersPanel::moveActive(int delta) {
    const int from = document_->activeLayerIndex();
    if (document_->moveLayer(from, from + delta)) {
        Q_EMIT documentChanged();
    }
}

void LayersPanel::setBlendMode(int comboIndex) {
    gas::LayerProps p = document_->activeLayer().props;
    p.blendMode = static_cast<gas::BlendMode>(blendMode_->itemData(comboIndex).toInt());
    if (document_->setLayerProps(document_->activeLayerIndex(), p)) {
        Q_EMIT documentChanged();
    }
}

void LayersPanel::setMaskEnabled(bool enabled) {
    gas::LayerProps p = document_->activeLayer().props;
    p.maskEnabled = enabled;
    if (document_->setLayerProps(document_->activeLayerIndex(), p)) {
        Q_EMIT documentChanged();
    }
}
