#pragma once

#include "CanvasWidget.h"

#include <QHash>
#include <QMainWindow>

#include <memory>

namespace gas {
class Document;
}

class BrushPanel;
class EffectsPanel;
class LayersPanel;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens a project file; returns false (after telling the user) on failure.
    bool openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void createToolBars();
    void createDocks();
    void selectTool(Tool tool);
    void syncToolOptions();
    void adjustBrushSize(double factor);

    void newCanvas();
    void open();
    bool save();
    bool saveAs();
    bool saveTo(const QString& path);
    void exportPng();
    // Asks to save unsaved changes; false if the user cancelled.
    bool maybeSave();
    void setDocument(std::unique_ptr<gas::Document> document, const QString& path);

    void undo();
    void redo();
    // Commits a pending free transform so file/menu commands see final pixels.
    void settleCanvas();
    // Refreshes the views if a document command changed anything.
    void runCommand(bool changed);
    void cropToSelection();
    void defineBrushTip();
    void featherSelection();
    // Re-syncs every view of the document (title, undo actions, layers, canvas).
    void onDocumentChanged();

    void chooseColor();
    void updateColorSwatch();
    void updateStatus();

    std::unique_ptr<gas::Document> document_;
    QString filePath_; // empty until saved/opened
    CanvasWidget* canvas_ = nullptr;
    LayersPanel* layersPanel_ = nullptr;
    BrushPanel* brushPanel_ = nullptr;
    EffectsPanel* effectsPanel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* opacityLabel_ = nullptr;
    QLabel* cursorLabel_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QHash<int, QAction*> toolActions_; // palette action per Tool
    QList<QAction*> documentActions_; // disabled while a transform is open

    QToolButton* colorButton_ = nullptr;
    QSpinBox* sizeBox_ = nullptr;
    QSpinBox* opacityBox_ = nullptr;
    QSpinBox* hardnessBox_ = nullptr;
    QSpinBox* toleranceBox_ = nullptr;
    QList<QAction*> brushOptionActions_; // shown for Brush / Eraser
    QList<QAction*> fillOptionActions_;  // shown for Fill
    QComboBox* selectionModeBox_ = nullptr;
    QCheckBox* antialiasBox_ = nullptr;
    QList<QAction*> selectionOptionActions_; // shown for selection tools
    QComboBox* interpolationBox_ = nullptr;
    QList<QAction*> transformOptionActions_; // shown for Move / while transforming
    QList<QAction*> transformApplyActions_;  // shown while transforming
};
