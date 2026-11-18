#pragma once

#include "CanvasWidget.h"

#include <gas/core/Atlas.h>

#include <QHash>
#include <QMainWindow>

#include <memory>

namespace gas {
class Document;
class PluginHost;
class RecoveryStore;
}

class BrushPanel;
class EffectsPanel;
class LayersPanel;
class SlicesPanel;
class TimelinePanel;
class VectorOptions;
class QAction;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QLockFile;
class QMenu;
class QTimer;
class QSpinBox;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens a project file; returns false (after telling the user) on failure.
    bool openFile(const QString& path);
    // Imports image files: one -> a new document; several -> a sheet with one slice each.
    void importImageFiles(const QStringList& paths);
    // Offers autosaved documents left behind by a crash (call once after show()).
    void offerRecovery();

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
    // v0.7 asset pipeline
    void importImages();  // one image -> new document; several -> a sheet with one slice each
    void importAsLayer(); // an image as a new layer of the current document
    void autoSlice();
    void gridSlice();
    void exportAtlas();
    void exportSlices();
    void exportLayers();
    void exportFrames();
    void exportSvg();
    // Crash recovery: autosave the document into this session's slot.
    void autosave();
    void clearAutosave();
    QString recoveryFolder() const;
    // Plugins.
    void loadPlugins();
    void runPluginFilter(int index);
    void runPluginExporter(int index);
    QString userPluginFolder() const;
    void nineSlicePreview();
    void runFrameCommand(void (TimelinePanel::*command)());
    QString documentBaseName() const; // file stem, or "sprite"
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
    SlicesPanel* slicesPanel_ = nullptr;
    QDockWidget* slicesDock_ = nullptr;
    TimelinePanel* timelinePanel_ = nullptr;
    QAction* onionAction_ = nullptr;
    VectorOptions* vectorOptions_ = nullptr;
    std::unique_ptr<gas::RecoveryStore> recovery_;
    std::string session_;
    std::unique_ptr<QLockFile> sessionLock_;
    QTimer* autosaveTimer_ = nullptr;
    unsigned autosavedGeneration_ = ~0u;
    bool autosaveExists_ = false;
    std::unique_ptr<gas::PluginHost> plugins_;
    QMenu* pluginsMenu_ = nullptr;
    QHash<int, std::vector<double>> filterValues_;
    QAction* vectorOptionsAction_ = nullptr;
    int atlasContent_ = -1;   // 0 = slices of this frame, 1 = animation frames (-1 = pick by document)
    int atlasFramePivot_ = 1; // pivot preset for whole-canvas frames (bottom centre)
    gas::AtlasSettings atlasSettings_ = gas::atlasPreset(gas::AtlasPreset::Unity);
    int atlasPreset_ = 0;
    QString atlasPath_;   // last atlas PNG
    QString exportDir_;   // last batch-export folder
    QString importDir_;   // last import folder
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
    QComboBox* symmetryBox_ = nullptr;
    QSpinBox* segmentsBox_ = nullptr;
    QList<QAction*> symmetryOptionActions_; // shown for stroke tools
    QList<QAction*> segmentsOptionActions_;  // radial modes only
    QAction* lineModeAction_ = nullptr;
    QSpinBox* streamlineBox_ = nullptr;
    int lastStreamline_ = 50; // restored by the toggle shortcut
    QCheckBox* antialiasBox_ = nullptr;
    QList<QAction*> selectionOptionActions_; // shown for selection tools
    QComboBox* interpolationBox_ = nullptr;
    QList<QAction*> transformOptionActions_; // shown for Move / while transforming
    QList<QAction*> transformApplyActions_;  // shown while transforming
};
