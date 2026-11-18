#pragma once

#include <gas/core/Geometry.h>

#include <QWidget>

namespace gas {
class Document;
}

class CanvasWidget;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

// Standard pivot positions (plus "Custom") for slice editors and dialogs.
void fillPivotCombo(QComboBox* combo);
gas::PointF pivotFromCombo(const QComboBox* combo, gas::PointF fallback);
void selectPivotInCombo(QComboBox* combo, gas::PointF pivot);

// Dock listing the document's slices (sprite regions) with the selected one's
// name, rect, pivot and 9-slice border; buttons run auto/grid slicing, trim,
// delete, the 9-slice preview and export.
// The canvas owns the selection (Slice tool), so the panel follows it.
class SlicesPanel : public QWidget {
    Q_OBJECT

public:
    SlicesPanel(CanvasWidget* canvas, QWidget* parent = nullptr);

    void setDocument(gas::Document* document);
    // Re-reads the slices (call after any document change).
    void refresh();

Q_SIGNALS:
    void documentChanged();
    void autoSliceRequested();
    void gridSliceRequested();
    void exportAtlasRequested();
    void nineSlicePreviewRequested();

private:
    void syncFields();
    // Replaces the selected slice with `edit` applied (merge = fold repeated spin edits).
    template <typename Fn>
    void editSelected(const char* label, bool merge, Fn&& edit);
    void trim();
    void applyPivotToAll();
    void clearAll();

    CanvasWidget* canvas_;
    gas::Document* document_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* xBox_ = nullptr;
    QSpinBox* yBox_ = nullptr;
    QSpinBox* wBox_ = nullptr;
    QSpinBox* hBox_ = nullptr;
    QComboBox* pivotBox_ = nullptr;
    QDoubleSpinBox* pivotXBox_ = nullptr;
    QDoubleSpinBox* pivotYBox_ = nullptr;
    QSpinBox* borderBoxes_[4] = {}; // 9-slice: left, top, right, bottom
    QPushButton* previewButton_ = nullptr;
    QWidget* fields_ = nullptr;
    QPushButton* trimButton_ = nullptr;
    QPushButton* pivotAllButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
};
