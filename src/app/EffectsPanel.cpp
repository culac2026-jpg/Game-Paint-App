#include "EffectsPanel.h"

#include <gas/core/Document.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QToolButton* makeButton(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QDoubleSpinBox* makeSpin(QWidget* parent, double min, double max, int decimals, const QString& suffix) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(min, max);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    box->setFocusPolicy(Qt::ClickFocus);
    return box;
}

bool usesColor(gas::EffectType t) {
    using T = gas::EffectType;
    return t == T::Outline || t == T::DropShadow || t == T::InnerShadow || t == T::OuterGlow || t == T::ColorOverlay;
}
bool usesSize(gas::EffectType t) { return t != gas::EffectType::ColorAdjust && t != gas::EffectType::ColorOverlay; }
bool usesOffset(gas::EffectType t) { return t == gas::EffectType::DropShadow || t == gas::EffectType::InnerShadow; }

QString sizeLabel(gas::EffectType t) {
    switch (t) {
    case gas::EffectType::Outline: return EffectsPanel::tr("Width");
    case gas::EffectType::Pixelate: return EffectsPanel::tr("Cell size");
    case gas::EffectType::Blur:
    case gas::EffectType::DropShadow:
    case gas::EffectType::InnerShadow: return EffectsPanel::tr("Blur");
    default: return EffectsPanel::tr("Size");
    }
}

QString effectLabel(gas::EffectType t) { return EffectsPanel::tr(gas::effectName(t).data()); }

} // namespace

EffectsPanel::EffectsPanel(QWidget* parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setToolTip(tr("Checkbox toggles an effect. Effects never change the layer's pixels."));
    list_->setMinimumHeight(90);

    addButton_ = makeButton(this, tr("Add"), tr("Add an effect"));
    addButton_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(addButton_);
    for (int t = 0; t < gas::kEffectTypeCount; ++t) {
        const auto type = static_cast<gas::EffectType>(t);
        menu->addAction(effectLabel(type), this, [this, type] { addEffect(type); });
    }
    addButton_->setMenu(menu);
    removeButton_ = makeButton(this, tr("Remove"), tr("Remove the selected effect"));
    upButton_ = makeButton(this, QStringLiteral("▲"), tr("Move up (applied later)"));
    downButton_ = makeButton(this, QStringLiteral("▼"), tr("Move down (applied earlier)"));
    rasterizeButton_ = makeButton(this, tr("Rasterize"), tr("Bake the effects into the layer's pixels"));
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addButton_);
    buttons->addWidget(removeButton_);
    buttons->addStretch();
    buttons->addWidget(upButton_);
    buttons->addWidget(downButton_);

    colorButton_ = makeButton(this, QString(), tr("Effect colour"));
    colorButton_->setAutoRaise(false);
    opacity_ = makeSpin(this, 0, 100, 0, tr("%"));
    size_ = makeSpin(this, 0, gas::kMaxEffectSize, 1, tr(" px"));
    offsetX_ = makeSpin(this, -gas::kMaxEffectOffset, gas::kMaxEffectOffset, 0, tr(" px"));
    offsetY_ = makeSpin(this, -gas::kMaxEffectOffset, gas::kMaxEffectOffset, 0, tr(" px"));
    smooth_ = new QCheckBox(tr("Anti-aliased (off = crisp pixels)"), this);
    smooth_->setFocusPolicy(Qt::NoFocus);
    hue_ = makeSpin(this, -180, 180, 0, QStringLiteral("°"));
    saturation_ = makeSpin(this, -100, 100, 0, tr("%"));
    lightness_ = makeSpin(this, -100, 100, 0, tr("%"));
    contrast_ = makeSpin(this, -100, 100, 0, tr("%"));

    form_ = new QFormLayout;
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->addRow(tr("Colour"), colorButton_);
    form_->addRow(tr("Opacity"), opacity_);
    form_->addRow(tr("Size"), size_);
    form_->addRow(tr("Offset X"), offsetX_);
    form_->addRow(tr("Offset Y"), offsetY_);
    form_->addRow(QString(), smooth_);
    form_->addRow(tr("Hue"), hue_);
    form_->addRow(tr("Saturation"), saturation_);
    form_->addRow(tr("Lightness"), lightness_);
    form_->addRow(tr("Contrast"), contrast_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addLayout(form_);
    layout->addWidget(rasterizeButton_, 0, Qt::AlignRight);

    connect(list_, &QListWidget::itemChanged, this, &EffectsPanel::onItemChanged);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        current_ = row;
        syncEditor();
    });
    connect(removeButton_, &QToolButton::clicked, this, [this] {
        const int at = current_;
        editEffects([at](std::vector<gas::Effect>& fx) {
            if (at >= 0 && at < static_cast<int>(fx.size())) {
                fx.erase(fx.begin() + at);
            }
        });
    });
    connect(upButton_, &QToolButton::clicked, this, [this] { moveCurrent(+1); });
    connect(downButton_, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
    connect(rasterizeButton_, &QToolButton::clicked, this, [this] {
        if (document_->rasterizeLayerEffects(document_->activeLayerIndex())) {
            Q_EMIT documentChanged();
        }
    });
    connect(colorButton_, &QToolButton::clicked, this, &EffectsPanel::chooseColor);
    const auto bind = [this](QDoubleSpinBox* box, double scale, double gas::Effect::*member) {
        connect(box, &QDoubleSpinBox::valueChanged, this,
                [this, scale, member](double v) { editCurrent([&](gas::Effect& e) { e.*member = v / scale; }); });
    };
    bind(opacity_, 100, &gas::Effect::opacity);
    bind(size_, 1, &gas::Effect::size);
    bind(offsetX_, 1, &gas::Effect::offsetX);
    bind(offsetY_, 1, &gas::Effect::offsetY);
    bind(hue_, 1, &gas::Effect::hue);
    bind(saturation_, 100, &gas::Effect::saturation);
    bind(lightness_, 100, &gas::Effect::lightness);
    bind(contrast_, 100, &gas::Effect::contrast);
    connect(smooth_, &QCheckBox::toggled, this, [this](bool on) { editCurrent([on](gas::Effect& e) { e.smooth = on; }); });

    setEnabled(false);
}

void EffectsPanel::setDocument(gas::Document* document) {
    document_ = document;
    current_ = -1;
    setEnabled(document_ != nullptr);
    refresh();
}

std::vector<gas::Effect> EffectsPanel::effects() const {
    return document_ ? document_->activeLayer().props.effects : std::vector<gas::Effect>{};
}

bool EffectsPanel::editEffects(const std::function<void(std::vector<gas::Effect>&)>& edit) {
    if (!document_) {
        return false;
    }
    gas::LayerProps p = document_->activeLayer().props;
    edit(p.effects);
    if (!document_->setLayerProps(document_->activeLayerIndex(), p)) {
        return false;
    }
    Q_EMIT documentChanged();
    return true;
}

bool EffectsPanel::editCurrent(const std::function<void(gas::Effect&)>& edit) {
    const int at = current_;
    return editEffects([&](std::vector<gas::Effect>& fx) {
        if (at >= 0 && at < static_cast<int>(fx.size())) {
            edit(fx[static_cast<std::size_t>(at)]);
        }
    });
}

void EffectsPanel::addEffect(gas::EffectType type) {
    // The list shows the last-applied effect on top, like the layer list.
    if (editEffects([type](std::vector<gas::Effect>& fx) { fx.push_back(gas::Effect::make(type)); })) {
        current_ = static_cast<int>(effects().size()) - 1;
        refresh();
    }
}

void EffectsPanel::moveCurrent(int delta) {
    const int from = current_, to = current_ + delta;
    const int count = static_cast<int>(effects().size());
    if (from < 0 || to < 0 || from >= count || to >= count) {
        return;
    }
    if (editEffects([from, to](std::vector<gas::Effect>& fx) {
            std::swap(fx[static_cast<std::size_t>(from)], fx[static_cast<std::size_t>(to)]);
        })) {
        current_ = to;
        refresh();
    }
}

void EffectsPanel::onItemChanged(QListWidgetItem* item) {
    const int count = list_->count();
    const int index = count - 1 - list_->row(item);
    const bool on = item->checkState() == Qt::Checked;
    editEffects([index, on](std::vector<gas::Effect>& fx) {
        if (index >= 0 && index < static_cast<int>(fx.size())) {
            fx[static_cast<std::size_t>(index)].enabled = on;
        }
    });
}

void EffectsPanel::refresh() {
    const std::vector<gas::Effect> fx = effects();
    const int count = static_cast<int>(fx.size());
    if (current_ >= count) {
        current_ = count - 1;
    }
    if (current_ < 0 && count > 0) {
        current_ = count - 1;
    }
    {
        const QSignalBlocker block(list_);
        list_->clear();
        for (int row = 0; row < count; ++row) {
            const gas::Effect& e = fx[static_cast<std::size_t>(count - 1 - row)]; // top = applied last
            auto* item = new QListWidgetItem(effectLabel(e.type), list_);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(e.enabled ? Qt::Checked : Qt::Unchecked);
        }
        list_->setCurrentRow(current_ >= 0 ? count - 1 - current_ : -1);
    }
    const bool layerOk = document_ != nullptr;
    addButton_->setEnabled(layerOk);
    removeButton_->setEnabled(current_ >= 0);
    upButton_->setEnabled(current_ >= 0 && current_ < count - 1);
    downButton_->setEnabled(current_ > 0);
    rasterizeButton_->setEnabled(layerOk && gas::hasActiveEffects(fx));
    syncEditor();
}

void EffectsPanel::syncEditor() {
    // The list row maps back to the stack index (list shows top = last).
    const std::vector<gas::Effect> fx = effects();
    const int row = list_->currentRow();
    if (row >= 0) {
        current_ = list_->count() - 1 - row;
    }
    const bool valid = current_ >= 0 && current_ < static_cast<int>(fx.size());
    const gas::Effect e = valid ? fx[static_cast<std::size_t>(current_)] : gas::Effect{};
    const gas::EffectType t = e.type;
    const auto show = [this](QWidget* field, bool visible) { form_->setRowVisible(field, visible); };
    show(colorButton_, valid && usesColor(t));
    show(opacity_, valid && usesColor(t));
    show(size_, valid && usesSize(t));
    show(offsetX_, valid && usesOffset(t));
    show(offsetY_, valid && usesOffset(t));
    show(smooth_, valid && t == gas::EffectType::Outline);
    for (QDoubleSpinBox* box : {hue_, saturation_, lightness_, contrast_}) {
        show(box, valid && t == gas::EffectType::ColorAdjust);
    }
    if (!valid) {
        return;
    }
    if (auto* label = qobject_cast<QLabel*>(form_->labelForField(size_))) {
        label->setText(sizeLabel(t));
    }
    const QSignalBlocker b1(opacity_), b2(size_), b3(offsetX_), b4(offsetY_), b5(smooth_), b6(hue_), b7(saturation_),
        b8(lightness_), b9(contrast_);
    opacity_->setValue(e.opacity * 100);
    size_->setValue(e.size);
    offsetX_->setValue(e.offsetX);
    offsetY_->setValue(e.offsetY);
    smooth_->setChecked(e.smooth);
    hue_->setValue(e.hue);
    saturation_->setValue(e.saturation * 100);
    lightness_->setValue(e.lightness * 100);
    contrast_->setValue(e.contrast * 100);

    QPixmap pm(36, 14);
    pm.fill(QColor(e.color.r, e.color.g, e.color.b, e.color.a));
    QPainter p(&pm);
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    p.end();
    colorButton_->setIcon(QIcon(pm));
    colorButton_->setIconSize(pm.size());
}

void EffectsPanel::chooseColor() {
    const std::vector<gas::Effect> fx = effects();
    if (current_ < 0 || current_ >= static_cast<int>(fx.size())) {
        return;
    }
    const gas::Rgba8 c = fx[static_cast<std::size_t>(current_)].color;
    const QColor chosen =
        QColorDialog::getColor(QColor(c.r, c.g, c.b, c.a), this, tr("Effect Colour"), QColorDialog::ShowAlphaChannel);
    if (chosen.isValid()) {
        const gas::Rgba8 n{static_cast<std::uint8_t>(chosen.red()), static_cast<std::uint8_t>(chosen.green()),
                           static_cast<std::uint8_t>(chosen.blue()), static_cast<std::uint8_t>(chosen.alpha())};
        editCurrent([n](gas::Effect& e) { e.color = n; });
    }
}
