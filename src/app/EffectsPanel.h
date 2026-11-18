#pragma once

#include <gas/core/Effects.h>

#include <QWidget>

#include <functional>

namespace gas {
class Document;
}

class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QListWidget;
class QListWidgetItem;
class QToolButton;

// Non-destructive effect stack of the active layer: add / remove / reorder /
// toggle effects and edit the selected one's parameters. Every change is a
// gas::Document::setLayerProps call, so it is undoable (slider-style runs of
// edits merge into one step) and the layer's pixels are never touched.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget* parent = nullptr);

    void setDocument(gas::Document* document); // non-owning
    void refresh();

Q_SIGNALS:
    void documentChanged();

private:
    std::vector<gas::Effect> effects() const;
    // Applies `edit` to a copy of the stack and commits it; returns whether it changed.
    bool editEffects(const std::function<void(std::vector<gas::Effect>&)>& edit);
    bool editCurrent(const std::function<void(gas::Effect&)>& edit);
    void addEffect(gas::EffectType type);
    void moveCurrent(int delta);
    void onItemChanged(QListWidgetItem* item);
    void syncEditor();
    void chooseColor();

    gas::Document* document_ = nullptr;
    int current_ = -1;
    QListWidget* list_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QToolButton* downButton_ = nullptr;
    QToolButton* rasterizeButton_ = nullptr;

    QFormLayout* form_ = nullptr;
    QToolButton* colorButton_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QDoubleSpinBox* offsetX_ = nullptr;
    QDoubleSpinBox* offsetY_ = nullptr;
    QCheckBox* smooth_ = nullptr;
    QDoubleSpinBox* hue_ = nullptr;
    QDoubleSpinBox* saturation_ = nullptr;
    QDoubleSpinBox* lightness_ = nullptr;
    QDoubleSpinBox* contrast_ = nullptr;
};
