#pragma once

#include <QTimer>
#include <QWidget>

#include <vector>

namespace gas {
class Document;
}

class CanvasWidget;
class QCheckBox;
class QLabel;
class QScrollArea;
class QSpinBox;
class QToolButton;
class TimelineGrid;

// Bottom dock: frame controls, playback, onion skin, durations and tags, over
// a layer x frame grid (filled dot = painted cel). Click a cell to show that
// frame and layer; Shift+click frame numbers to select a range for a tag;
// double-click a tag to edit it.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    TimelinePanel(CanvasWidget* canvas, QWidget* parent = nullptr);

    void setDocument(gas::Document* document);
    void refresh();
    bool isPlaying() const { return playing_; }
    QSize sizeHint() const override { return {600, 200}; }

public Q_SLOTS:
    void togglePlay();
    void stop();
    void nextFrame();
    void previousFrame();
    void addFrame();
    void duplicateFrame();
    void removeFrame();
    void newTag();
    void setOnionSkin(bool on);

Q_SIGNALS:
    // An undoable change (frames, durations, tags).
    void documentChanged();
    // The shown frame or active layer changed (not an undo step).
    void viewChanged();

private:
    friend class TimelineGrid;
    void showFrame(int frame);
    void tick();
    // Frame after `frame` in the range chosen when playback started.
    int playbackNext(int frame);
    void editTag(int index);
    void syncOnion();
    bool canEdit() const;

    CanvasWidget* canvas_;
    gas::Document* document_ = nullptr;
    TimelineGrid* grid_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QSpinBox* durationBox_ = nullptr;
    QCheckBox* onionBox_ = nullptr;
    QSpinBox* onionBefore_ = nullptr;
    QSpinBox* onionAfter_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QTimer timer_;
    bool playing_ = false;
    int pingDirection_ = 1; // ping-pong playback direction
    int playFrom_ = 0, playTo_ = 0; // range played (the innermost tag at the start frame, else all)
    int playDirection_ = 0;         // gas::TagDirection
    int rangeStart_ = -1;   // frame range selected in the header (for tags)
    int rangeEnd_ = -1;
};
