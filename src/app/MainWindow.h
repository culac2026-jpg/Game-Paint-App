#pragma once

#include "CanvasWidget.h"

#include <QMainWindow>

#include <memory>

namespace gas {
class Document;
}

class QAction;
class QLabel;
class QSpinBox;
class QToolButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void createActions();
    void createToolBars();
    void selectTool(Tool tool);
    void syncToolOptions();
    void adjustBrushSize(double factor);
    void newCanvas();
    void chooseColor();
    void updateColorSwatch();
    void updateStatus();

    std::unique_ptr<gas::Document> document_;
    CanvasWidget* canvas_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* cursorLabel_ = nullptr;

    QToolButton* colorButton_ = nullptr;
    QSpinBox* sizeBox_ = nullptr;
    QSpinBox* opacityBox_ = nullptr;
    QSpinBox* hardnessBox_ = nullptr;
    QSpinBox* toleranceBox_ = nullptr;
    QList<QAction*> brushOptionActions_; // shown for Brush / Eraser
    QList<QAction*> fillOptionActions_;  // shown for Fill
};
