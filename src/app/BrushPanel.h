#pragma once

#include <gas/core/BrushPresets.h>

#include <QWidget>

#include <vector>

class CanvasWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QToolButton;

// Presets and dynamics of the active Brush / Eraser (size, opacity and
// hardness live on the tool options bar). User presets are kept in a text file
// in the app data folder, next to the built-in ones.
class BrushPanel : public QWidget {
    Q_OBJECT

public:
    explicit BrushPanel(CanvasWidget* canvas, QWidget* parent = nullptr);

    // Re-reads the active tool's settings (call when the tool changes).
    void refresh();

Q_SIGNALS:
    // The panel changed the brush settings (e.g. a preset changed the size).
    void settingsChanged();

private:
    struct Field {
        QDoubleSpinBox* box;
        double gas::BrushSettings::*member;
        double scale; // displayed = value * scale
    };

    bool brushToolActive() const;
    gas::BrushSettings* settings() const;
    void applyPreset(int comboIndex);
    void savePreset();
    void deletePreset();
    void rebuildPresetList();
    void loadUserPresets();
    bool storeUserPresets();

    CanvasWidget* canvas_;
    std::vector<gas::BrushPreset> userPresets_;
    QComboBox* presets_ = nullptr;
    QToolButton* saveButton_ = nullptr;
    QToolButton* deleteButton_ = nullptr;
    std::vector<Field> fields_;
    QCheckBox* followDirection_ = nullptr;
    QComboBox* blendMode_ = nullptr;
    QLabel* hint_ = nullptr;
};
