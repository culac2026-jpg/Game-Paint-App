#pragma once

#include <QMainWindow>

#include <memory>

namespace gas {
class Document;
}

class CanvasWidget;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void createActions();
    void newCanvas();
    void chooseColor();
    void updateStatus();

    std::unique_ptr<gas::Document> document_;
    CanvasWidget* canvas_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* cursorLabel_ = nullptr;
};
