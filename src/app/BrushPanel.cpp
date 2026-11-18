#include "BrushPanel.h"

#include "CanvasWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Combo item data: index into builtins (>= 0) or user presets (encoded as -1 - i).
constexpr int kNoPreset = 1 << 30;

QString presetFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/brush-presets.txt");
}

} // namespace

BrushPanel::BrushPanel(CanvasWidget* canvas, QWidget* parent) : QWidget(parent), canvas_(canvas) {
    presets_ = new QComboBox(this);
    presets_->setFocusPolicy(Qt::ClickFocus);
    presets_->setToolTip(tr("Load a brush preset"));
    saveButton_ = new QToolButton(this);
    saveButton_->setText(tr("Save..."));
    saveButton_->setToolTip(tr("Save the current brush as a preset"));
    deleteButton_ = new QToolButton(this);
    deleteButton_->setText(tr("Delete"));
    deleteButton_->setToolTip(tr("Delete a user preset"));
    for (QToolButton* b : {saveButton_, deleteButton_}) {
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
    }
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(presets_, 1);
    presetRow->addWidget(saveButton_);
    presetRow->addWidget(deleteButton_);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto addField = [&](const QString& label, double gas::BrushSettings::*member, double min, double max,
                              double scale, const QString& suffix, const QString& tip) {
        auto* box = new QDoubleSpinBox(this);
        box->setRange(min, max);
        box->setDecimals(0);
        box->setSuffix(suffix);
        box->setToolTip(tip);
        box->setKeyboardTracking(false);
        box->setFocusPolicy(Qt::ClickFocus);
        form->addRow(label, box);
        fields_.push_back({box, member, scale});
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, member, scale](double v) {
            if (gas::BrushSettings* s = settings()) {
                s->*member = v / scale;
                Q_EMIT settingsChanged();
            }
        });
    };
    const QString pct = tr("%");
    addField(tr("Spacing"), &gas::BrushSettings::spacing, 1, 1000, 100, pct,
             tr("Distance between dabs, in percent of the dab size"));
    addField(tr("Roundness"), &gas::BrushSettings::roundness, 5, 100, 100, pct,
             tr("100% = round tip; lower values flatten it"));
    addField(tr("Angle"), &gas::BrushSettings::angle, -180, 180, 1, QStringLiteral("°"),
             tr("Rotation of a flattened tip"));
    followDirection_ = new QCheckBox(tr("Follow stroke direction"), this);
    followDirection_->setFocusPolicy(Qt::NoFocus);
    form->addRow(QString(), followDirection_);
    addField(tr("Pressure → size"), &gas::BrushSettings::pressureSize, 0, 100, 100, pct,
             tr("How much a light pen touch shrinks the dab"));
    addField(tr("Pressure → opacity"), &gas::BrushSettings::pressureOpacity, 0, 100, 100, pct,
             tr("How much a light pen touch fades the dab"));
    addField(tr("Size jitter"), &gas::BrushSettings::sizeJitter, 0, 100, 100, pct, tr("Random dab size"));
    addField(tr("Opacity jitter"), &gas::BrushSettings::opacityJitter, 0, 100, 100, pct, tr("Random dab opacity"));
    addField(tr("Angle jitter"), &gas::BrushSettings::angleJitter, 0, 100, 100, pct,
             tr("Random tip rotation (100% = any angle)"));
    addField(tr("Scatter"), &gas::BrushSettings::scatter, 0, 400, 100, pct,
             tr("Random offset from the path, in percent of the size"));
    addField(tr("Smoothing"), &gas::BrushSettings::smoothing, 0, 100, 100, pct,
             tr("Stabilizer: the brush trails the pointer to iron out shaky lines"));
    blendMode_ = new QComboBox(this);
    blendMode_->setFocusPolicy(Qt::ClickFocus);
    for (int m = 0; m < gas::kBlendModeCount; ++m) {
        blendMode_->addItem(tr(gas::blendModeName(static_cast<gas::BlendMode>(m)).data()), m);
    }
    form->addRow(tr("Blend"), blendMode_);

    hint_ = new QLabel(tr("Select the Brush or Eraser to edit its settings."), this);
    hint_->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(presetRow);
    layout->addLayout(form);
    layout->addWidget(hint_);
    layout->addStretch();

    connect(presets_, &QComboBox::activated, this, &BrushPanel::applyPreset);
    connect(saveButton_, &QToolButton::clicked, this, &BrushPanel::savePreset);
    connect(deleteButton_, &QToolButton::clicked, this, &BrushPanel::deletePreset);
    connect(followDirection_, &QCheckBox::toggled, this, [this](bool on) {
        if (gas::BrushSettings* s = settings()) {
            s->followDirection = on;
            Q_EMIT settingsChanged();
        }
    });
    connect(blendMode_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (gas::BrushSettings* s = settings()) {
            s->blendMode = static_cast<gas::BlendMode>(blendMode_->itemData(i).toInt());
            Q_EMIT settingsChanged();
        }
    });

    loadUserPresets();
    rebuildPresetList();
    refresh();
}

bool BrushPanel::brushToolActive() const {
    return canvas_->tool() == Tool::Brush || canvas_->tool() == Tool::Eraser;
}

gas::BrushSettings* BrushPanel::settings() const {
    return brushToolActive() ? &canvas_->brushSettings(canvas_->tool()) : nullptr;
}

void BrushPanel::refresh() {
    const gas::BrushSettings* s = settings();
    for (QWidget* w : findChildren<QWidget*>()) {
        if (w != hint_) {
            w->setEnabled(s != nullptr);
        }
    }
    hint_->setVisible(s == nullptr);
    if (!s) {
        return;
    }
    for (const Field& f : fields_) {
        const QSignalBlocker block(f.box);
        f.box->setValue(s->*f.member * f.scale);
    }
    {
        const QSignalBlocker b1(followDirection_), b2(blendMode_);
        followDirection_->setChecked(s->followDirection);
        blendMode_->setCurrentIndex(static_cast<int>(s->blendMode));
    }
    blendMode_->setEnabled(canvas_->tool() == Tool::Brush); // the eraser only removes alpha
    deleteButton_->setEnabled(!userPresets_.empty());
}

void BrushPanel::rebuildPresetList() {
    const QSignalBlocker block(presets_);
    presets_->clear();
    presets_->addItem(tr("Presets..."), kNoPreset);
    const auto& builtins = gas::builtinBrushPresets();
    for (int i = 0; i < static_cast<int>(builtins.size()); ++i) {
        presets_->addItem(tr(builtins[static_cast<std::size_t>(i)].name.c_str()), i);
    }
    if (!userPresets_.empty()) {
        presets_->insertSeparator(presets_->count());
        for (int i = 0; i < static_cast<int>(userPresets_.size()); ++i) {
            presets_->addItem(QString::fromStdString(userPresets_[static_cast<std::size_t>(i)].name), -1 - i);
        }
    }
    presets_->setCurrentIndex(0);
}

void BrushPanel::applyPreset(int comboIndex) {
    const int id = presets_->itemData(comboIndex).toInt();
    gas::BrushSettings* s = settings();
    if (s && id != kNoPreset) {
        const auto& builtins = gas::builtinBrushPresets();
        *s = id >= 0 ? builtins.at(static_cast<std::size_t>(id)).settings
                     : userPresets_.at(static_cast<std::size_t>(-1 - id)).settings;
        refresh();
        Q_EMIT settingsChanged();
    }
    const QSignalBlocker block(presets_);
    presets_->setCurrentIndex(0); // a "load" menu, not a sticky selection
}

void BrushPanel::savePreset() {
    const gas::BrushSettings* s = settings();
    if (!s) {
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Save Brush Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const std::string key = name.toStdString();
    const auto it = std::find_if(userPresets_.begin(), userPresets_.end(),
                                 [&](const gas::BrushPreset& p) { return p.name == key; });
    if (it != userPresets_.end()) {
        it->settings = *s; // same name: overwrite
    } else {
        userPresets_.push_back({key, *s});
    }
    storeUserPresets();
    rebuildPresetList();
    refresh();
}

void BrushPanel::deletePreset() {
    if (userPresets_.empty()) {
        return;
    }
    QStringList names;
    for (const gas::BrushPreset& p : userPresets_) {
        names << QString::fromStdString(p.name);
    }
    bool ok = false;
    const QString name =
        QInputDialog::getItem(this, tr("Delete Brush Preset"), tr("Preset:"), names, 0, false, &ok);
    if (!ok) {
        return;
    }
    userPresets_.erase(userPresets_.begin() + names.indexOf(name));
    storeUserPresets();
    rebuildPresetList();
    refresh();
}

void BrushPanel::loadUserPresets() {
    QFile file(presetFilePath());
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray text = file.readAll();
        userPresets_ = gas::parseBrushPresets(std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
    }
}

bool BrushPanel::storeUserPresets() {
    const QString path = presetFilePath();
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile file(path); // atomic, like project saves
    const std::string text = gas::serializeBrushPresets(userPresets_);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), static_cast<qint64>(text.size())) < 0 ||
        !file.commit()) {
        QMessageBox::warning(this, tr("Brush Presets"), tr("Could not save presets to\n%1").arg(path));
        return false;
    }
    return true;
}
