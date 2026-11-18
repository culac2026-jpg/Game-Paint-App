#include "TimelinePanel.h"

#include "CanvasWidget.h"

#include <gas/core/Document.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kNameWidth = 120;
constexpr int kCellWidth = 24;
constexpr int kTagLane = 18;
constexpr int kNumberRow = 18;
constexpr int kLayerRow = 20;

QColor tagColour(int index) {
    static const QColor colours[] = {QColor(70, 140, 230), QColor(230, 120, 60), QColor(90, 180, 90),
                                     QColor(200, 90, 200), QColor(220, 190, 60), QColor(80, 190, 190)};
    return colours[index % 6];
}

} // namespace

// ---------------------------------------------------------------- grid

class TimelineGrid : public QWidget {
public:
    // Overlapping tags go on separate lanes (first lane that is free over the range).
    std::vector<int> lanes(int* count = nullptr) const {
        std::vector<int> out;
        int n = 1;
        if (const gas::Document* d = panel_->document_) {
            const auto& tags = d->tags();
            for (std::size_t i = 0; i < tags.size(); ++i) {
                int lane = 0;
                for (bool clash = true; clash; lane += clash ? 1 : 0) {
                    clash = false;
                    for (std::size_t j = 0; j < i; ++j) {
                        if (out[j] == lane && tags[j].from <= tags[i].to && tags[i].from <= tags[j].to) {
                            clash = true;
                        }
                    }
                }
                out.push_back(lane);
                n = std::max(n, lane + 1);
            }
        }
        if (count) {
            *count = n;
        }
        return out;
    }
    int tagRowHeight() const {
        int n = 1;
        lanes(&n);
        return n * kTagLane;
    }

    explicit TimelineGrid(TimelinePanel* panel) : QWidget(panel), panel_(panel) { setMouseTracking(false); }

    QSize sizeHint() const override {
        const gas::Document* d = panel_->document_;
        const int frames = d ? d->frameCount() : 1;
        const int layers = d ? d->layerCount() : 1;
        return {kNameWidth + frames * kCellWidth + 1, tagRowHeight() + kNumberRow + layers * kLayerRow + 1};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

    void sync() {
        const gas::Document* d = panel_->document_;
        if (d && (d->historyGeneration() != generation_ || d->frameCount() * d->layerCount() != static_cast<int>(filled_.size()))) {
            generation_ = d->historyGeneration();
            filled_.assign(static_cast<std::size_t>(d->frameCount() * d->layerCount()), 0);
            for (int l = 0; l < d->layerCount(); ++l) {
                for (int f = 0; f < d->frameCount(); ++f) {
                    filled_[static_cast<std::size_t>(l * d->frameCount() + f)] = d->celEmpty(l, f) ? 0 : 1;
                }
            }
        }
        setFixedSize(sizeHint());
        update();
    }
    // Cel fill state may change mid-stroke without a history step; refresh on demand.
    void invalidate() { generation_ = ~0u; }

    int frameAt(int x) const { return x < kNameWidth ? -1 : (x - kNameWidth) / kCellWidth; }
    int tagAt(QPoint p) const {
        const gas::Document* d = panel_->document_;
        if (!d || p.y() >= tagRowHeight()) {
            return -1;
        }
        const int f = frameAt(p.x());
        const std::vector<int> lane = lanes();
        for (int i = static_cast<int>(d->tags().size()) - 1; i >= 0; --i) {
            const gas::FrameTag& t = d->tags()[static_cast<std::size_t>(i)];
            if (f >= t.from && f <= t.to && p.y() / kTagLane == lane[static_cast<std::size_t>(i)]) {
                return i;
            }
        }
        return -1;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.base());
        const gas::Document* d = panel_->document_;
        if (!d) {
            return;
        }
        const int frames = d->frameCount(), layers = d->layerCount();
        const int tagRow = tagRowHeight();
        const int top = tagRow + kNumberRow;
        const std::vector<int> lane = lanes();
        // Selected range and active frame columns.
        if (panel_->rangeStart_ >= 0) {
            const int a = std::min(panel_->rangeStart_, panel_->rangeEnd_), b = std::max(panel_->rangeStart_, panel_->rangeEnd_);
            p.fillRect(kNameWidth + a * kCellWidth, tagRow, (b - a + 1) * kCellWidth, height() - tagRow,
                       QColor(120, 160, 255, 50));
        }
        p.fillRect(kNameWidth + d->activeFrame() * kCellWidth, tagRow, kCellWidth, height() - tagRow,
                   QColor(80, 170, 255, 70));
        // Tags.
        for (int i = 0; i < static_cast<int>(d->tags().size()); ++i) {
            const gas::FrameTag& t = d->tags()[static_cast<std::size_t>(i)];
            const QRect r(kNameWidth + t.from * kCellWidth + 1, lane[static_cast<std::size_t>(i)] * kTagLane + 2,
                          (t.to - t.from + 1) * kCellWidth - 2, kTagLane - 4);
            p.fillRect(r, tagColour(i));
            p.setPen(Qt::white);
            static const char* arrows[gas::kTagDirectionCount] = {"▶ ", "◀ ", "⇄ "};
            const QString text = QString::fromUtf8(arrows[static_cast<int>(t.direction)]) + QString::fromStdString(t.name);
            p.drawText(r.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(text, Qt::ElideRight, r.width() - 5));
        }
        // Frame numbers (1-based, like most animation tools).
        p.setPen(pal.color(QPalette::Text));
        for (int f = 0; f < frames; ++f) {
            p.drawText(QRect(kNameWidth + f * kCellWidth, tagRow, kCellWidth, kNumberRow), Qt::AlignCenter,
                       QString::number(f + 1));
        }
        // Layer rows, top layer first.
        for (int row = 0; row < layers; ++row) {
            const int l = layers - 1 - row;
            const int y = top + row * kLayerRow;
            if (l == d->activeLayerIndex()) {
                p.fillRect(0, y, width(), kLayerRow, QColor(80, 170, 255, 40));
            }
            p.setPen(pal.color(d->layer(l).props.visible ? QPalette::Text : QPalette::PlaceholderText));
            p.drawText(QRect(6, y, kNameWidth - 10, kLayerRow), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(QString::fromStdString(d->layer(l).props.name), Qt::ElideRight,
                                                  kNameWidth - 10));
            p.setRenderHint(QPainter::Antialiasing, true);
            for (int f = 0; f < frames; ++f) {
                const QPointF c(kNameWidth + f * kCellWidth + kCellWidth / 2.0, y + kLayerRow / 2.0);
                const bool filled = static_cast<std::size_t>(l * frames + f) < filled_.size() &&
                                    filled_[static_cast<std::size_t>(l * frames + f)];
                p.setPen(QPen(pal.color(QPalette::Text), 1.2));
                p.setBrush(filled ? pal.color(QPalette::Text) : Qt::NoBrush);
                p.drawEllipse(c, 4.0, 4.0);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        // Grid lines.
        p.setPen(QColor(128, 128, 128, 70));
        for (int f = 0; f <= frames; ++f) {
            const int x = kNameWidth + f * kCellWidth;
            p.drawLine(x, tagRow, x, height());
        }
        for (int row = 0; row <= layers; ++row) {
            p.drawLine(0, top + row * kLayerRow, width(), top + row * kLayerRow);
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        const gas::Document* d = panel_->document_;
        if (!d || e->button() != Qt::LeftButton) {
            return;
        }
        const QPoint pos = e->position().toPoint();
        const int f = frameAt(pos.x());
        const int tagRow = tagRowHeight();
        if (pos.y() < tagRow) {
            return; // tags: double-click edits
        }
        if (f >= 0 && f < d->frameCount()) {
            if (pos.y() < tagRow + kNumberRow && (e->modifiers() & Qt::ShiftModifier)) {
                if (panel_->rangeStart_ < 0) {
                    panel_->rangeStart_ = d->activeFrame();
                }
                panel_->rangeEnd_ = f;
            } else {
                panel_->rangeStart_ = panel_->rangeEnd_ = -1;
            }
            panel_->showFrame(f);
        }
        const int row = (pos.y() - tagRow - kNumberRow) / kLayerRow;
        if (pos.y() >= tagRow + kNumberRow && row >= 0 && row < d->layerCount()) {
            panel_->document_->setActiveLayer(d->layerCount() - 1 - row);
            Q_EMIT panel_->viewChanged();
        }
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int t = tagAt(e->position().toPoint());
        if (t >= 0) {
            panel_->editTag(t);
        }
    }

private:
    TimelinePanel* panel_;
    unsigned generation_ = ~0u;
    std::vector<char> filled_;
};

// ---------------------------------------------------------------- panel

namespace {

QToolButton* toolButton(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoRaise(true);
    return b;
}

} // namespace

TimelinePanel::TimelinePanel(CanvasWidget* canvas, QWidget* parent) : QWidget(parent), canvas_(canvas) {
    auto* first = toolButton(this, QStringLiteral("⇤"), tr("First frame"));
    auto* prev = toolButton(this, QStringLiteral("◀"), tr("Previous frame (,)"));
    playButton_ = toolButton(this, QStringLiteral("▶"), tr("Play / stop (P): plays the tag under the current frame, "
                                                                "or every frame"));
    auto* next = toolButton(this, QStringLiteral("▶|"), tr("Next frame (.)"));
    next->setText(QStringLiteral("▶∣"));
    prev->setText(QStringLiteral("∣◀"));
    auto* last = toolButton(this, QStringLiteral("⇥"), tr("Last frame"));
    auto* add = toolButton(this, tr("+ Frame"), tr("New empty frame after this one (Alt+N)"));
    auto* dup = toolButton(this, tr("Duplicate"), tr("Copy this frame (Alt+D)"));
    auto* del = toolButton(this, tr("Delete"), tr("Delete this frame"));
    auto* left = toolButton(this, tr("← Move"), tr("Move this frame one step earlier"));
    auto* right = toolButton(this, tr("Move →"), tr("Move this frame one step later"));
    durationBox_ = new QSpinBox(this);
    durationBox_->setRange(gas::Document::kMinFrameDuration, gas::Document::kMaxFrameDuration);
    durationBox_->setSuffix(tr(" ms"));
    durationBox_->setKeyboardTracking(false);
    durationBox_->setFocusPolicy(Qt::ClickFocus);
    durationBox_->setToolTip(tr("How long this frame shows"));
    auto* allDurations = toolButton(this, tr("All"), tr("Give every frame this duration"));
    onionBox_ = new QCheckBox(tr("Onion skin"), this);
    onionBox_->setFocusPolicy(Qt::NoFocus);
    onionBox_->setToolTip(tr("Show neighbouring frames faintly: earlier in red, later in blue (F3)"));
    onionBefore_ = new QSpinBox(this);
    onionAfter_ = new QSpinBox(this);
    for (QSpinBox* b : {onionBefore_, onionAfter_}) {
        b->setRange(0, 5);
        b->setValue(1);
        b->setFocusPolicy(Qt::ClickFocus);
    }
    onionBefore_->setToolTip(tr("Earlier frames shown"));
    onionAfter_->setToolTip(tr("Later frames shown"));
    auto* tag = toolButton(this, tr("Tag..."), tr("Name the selected frame range (Shift+click frame numbers), "
                                                  "e.g. \"walk\"; double-click a tag to edit it"));
    infoLabel_ = new QLabel(this);

    // Two rows so the dock fits beside the side panels: transport + frame edits,
    // then timing / onion skin / tags.
    auto* barWidget = new QWidget(this);
    barWidget->setMinimumWidth(1); // very narrow windows clip the controls instead of growing
    auto* rows = new QVBoxLayout(barWidget);
    rows->setContentsMargins(4, 2, 4, 2);
    rows->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(2);
    for (QWidget* w : std::initializer_list<QWidget*>{first, prev, playButton_, next, last}) {
        bar->addWidget(w);
    }
    bar->addSpacing(10);
    for (QWidget* w : std::initializer_list<QWidget*>{add, dup, del, left, right}) {
        bar->addWidget(w);
    }
    bar->addStretch(1);
    bar->addWidget(infoLabel_);
    auto* bar2 = new QHBoxLayout;
    bar2->setSpacing(4);
    bar2->addWidget(new QLabel(tr("Duration"), this));
    bar2->addWidget(durationBox_);
    bar2->addWidget(allDurations);
    bar2->addSpacing(12);
    bar2->addWidget(onionBox_);
    bar2->addWidget(onionBefore_);
    bar2->addWidget(onionAfter_);
    bar2->addSpacing(12);
    bar2->addWidget(tag);
    bar2->addStretch(1);
    rows->addLayout(bar);
    rows->addLayout(bar2);

    grid_ = new TimelineGrid(this);
    scroll_ = new QScrollArea(this);
    scroll_->setWidget(grid_);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setFocusPolicy(Qt::NoFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(barWidget);
    layout->addWidget(scroll_, 1);

    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &TimelinePanel::tick);
    connect(first, &QToolButton::clicked, this, [this] { showFrame(0); });
    connect(last, &QToolButton::clicked, this, [this] {
        if (document_) {
            showFrame(document_->frameCount() - 1);
        }
    });
    connect(prev, &QToolButton::clicked, this, &TimelinePanel::previousFrame);
    connect(next, &QToolButton::clicked, this, &TimelinePanel::nextFrame);
    connect(playButton_, &QToolButton::clicked, this, &TimelinePanel::togglePlay);
    connect(add, &QToolButton::clicked, this, &TimelinePanel::addFrame);
    connect(dup, &QToolButton::clicked, this, &TimelinePanel::duplicateFrame);
    connect(del, &QToolButton::clicked, this, &TimelinePanel::removeFrame);
    connect(left, &QToolButton::clicked, this, [this] {
        if (canEdit() && document_->moveFrame(document_->activeFrame(), document_->activeFrame() - 1)) {
            Q_EMIT documentChanged();
        }
    });
    connect(right, &QToolButton::clicked, this, [this] {
        if (canEdit() && document_->moveFrame(document_->activeFrame(), document_->activeFrame() + 1)) {
            Q_EMIT documentChanged();
        }
    });
    connect(durationBox_, &QSpinBox::valueChanged, this, [this](int ms) {
        if (canEdit() && document_->setFrameDuration(document_->activeFrame(), ms)) {
            Q_EMIT documentChanged();
        }
    });
    connect(allDurations, &QToolButton::clicked, this, [this] {
        if (!canEdit()) {
            return;
        }
        std::vector<gas::Frame> frames = document_->frames();
        for (gas::Frame& f : frames) {
            f.duration = durationBox_->value();
        }
        if (document_->setFrames(std::move(frames))) {
            Q_EMIT documentChanged();
        }
    });
    connect(onionBox_, &QCheckBox::toggled, this, &TimelinePanel::syncOnion);
    connect(onionBefore_, &QSpinBox::valueChanged, this, &TimelinePanel::syncOnion);
    connect(onionAfter_, &QSpinBox::valueChanged, this, &TimelinePanel::syncOnion);
    connect(tag, &QToolButton::clicked, this, &TimelinePanel::newTag);
    connect(canvas_, &CanvasWidget::documentEdited, this, [this] {
        grid_->invalidate();
        refresh();
    });
    refresh();
}

void TimelinePanel::setDocument(gas::Document* document) {
    stop();
    document_ = document;
    rangeStart_ = rangeEnd_ = -1;
    grid_->invalidate();
    refresh();
}

bool TimelinePanel::canEdit() const { return document_ && !canvas_->isBusy() && !playing_; }

void TimelinePanel::refresh() {
    if (!document_) {
        return;
    }
    if (rangeStart_ >= document_->frameCount() || rangeEnd_ >= document_->frameCount()) {
        rangeStart_ = rangeEnd_ = -1;
    }
    {
        const QSignalBlocker block(durationBox_);
        durationBox_->setValue(document_->frames()[static_cast<std::size_t>(document_->activeFrame())].duration);
    }
    int total = 0;
    for (const gas::Frame& f : document_->frames()) {
        total += f.duration;
    }
    infoLabel_->setText(tr("Frame %1 / %2   %3 s").arg(document_->activeFrame() + 1).arg(document_->frameCount()).arg(total / 1000.0, 0, 'f', 2));
    grid_->sync();
    // Keep the active frame column visible.
    scroll_->ensureVisible(kNameWidth + document_->activeFrame() * kCellWidth + kCellWidth / 2, kTagLane, kCellWidth, 0);
}

void TimelinePanel::showFrame(int frame) {
    if (!document_ || canvas_->isBusy()) {
        return;
    }
    if (canvas_->isTransforming()) {
        canvas_->commitTransform();
    }
    if (document_->setActiveFrame(std::clamp(frame, 0, document_->frameCount() - 1))) {
        refresh();
        Q_EMIT viewChanged();
    }
}

void TimelinePanel::nextFrame() {
    if (document_) {
        showFrame((document_->activeFrame() + 1) % document_->frameCount());
    }
}

void TimelinePanel::previousFrame() {
    if (document_) {
        showFrame((document_->activeFrame() + document_->frameCount() - 1) % document_->frameCount());
    }
}

void TimelinePanel::addFrame() {
    if (canEdit() && document_->addFrame()) {
        Q_EMIT documentChanged();
    }
}

void TimelinePanel::duplicateFrame() {
    if (canEdit() && document_->duplicateFrame(document_->activeFrame())) {
        Q_EMIT documentChanged();
    }
}

void TimelinePanel::removeFrame() {
    if (canEdit() && document_->removeFrame(document_->activeFrame())) {
        Q_EMIT documentChanged();
    }
}

void TimelinePanel::togglePlay() {
    if (playing_) {
        stop();
        return;
    }
    if (!document_ || document_->frameCount() < 2) {
        return;
    }
    if (canvas_->isTransforming()) {
        canvas_->commitTransform();
    }
    playing_ = true;
    pingDirection_ = 1;
    const int start = document_->activeFrame();
    playFrom_ = 0;
    playTo_ = document_->frameCount() - 1;
    playDirection_ = static_cast<int>(gas::TagDirection::Forward);
    int best = document_->frameCount() + 1; // innermost (shortest) tag at the start frame wins
    for (const gas::FrameTag& t : document_->tags()) {
        if (start >= t.from && start <= t.to && t.to - t.from < best) {
            best = t.to - t.from;
            playFrom_ = t.from;
            playTo_ = t.to;
            playDirection_ = static_cast<int>(t.direction);
        }
    }
    playButton_->setText(QStringLiteral("■"));
    timer_.start(document_->frames()[static_cast<std::size_t>(document_->activeFrame())].duration);
}

void TimelinePanel::stop() {
    playing_ = false;
    timer_.stop();
    playButton_->setText(QStringLiteral("▶"));
}

int TimelinePanel::playbackNext(int frame) {
    const int last = document_->frameCount() - 1;
    const int from = std::min(playFrom_, last), to = std::min(playTo_, last);
    const auto direction = static_cast<gas::TagDirection>(playDirection_);
    if (frame < from || frame > to) {
        return from;
    }
    if (from == to) {
        return from;
    }
    switch (direction) {
    case gas::TagDirection::Forward: return frame >= to ? from : frame + 1;
    case gas::TagDirection::Reverse: return frame <= from ? to : frame - 1;
    case gas::TagDirection::PingPong:
        if (frame + pingDirection_ > to || frame + pingDirection_ < from) {
            pingDirection_ = -pingDirection_;
        }
        return frame + pingDirection_;
    }
    return from;
}

void TimelinePanel::tick() {
    if (!playing_ || !document_) {
        return;
    }
    if (!canvas_->isBusy()) { // never switch frames under a stroke
        const int next = playbackNext(document_->activeFrame());
        if (document_->setActiveFrame(next)) {
            refresh();
            Q_EMIT viewChanged();
        }
    }
    timer_.start(document_->frames()[static_cast<std::size_t>(document_->activeFrame())].duration);
}

void TimelinePanel::setOnionSkin(bool on) { onionBox_->setChecked(on); }

void TimelinePanel::syncOnion() {
    canvas_->setOnionSkin(onionBox_->isChecked(), onionBefore_->value(), onionAfter_->value());
}

namespace {

// Name / range / direction editor; returns false if cancelled. `remove` set when "Delete" was chosen.
bool tagDialog(QWidget* parent, gas::FrameTag& tag, int frameCount, bool existing, bool& remove) {
    QDialog d(parent);
    d.setWindowTitle(existing ? TimelinePanel::tr("Edit Tag") : TimelinePanel::tr("New Tag"));
    auto* form = new QFormLayout(&d);
    auto* name = new QLineEdit(QString::fromStdString(tag.name), &d);
    auto* from = new QSpinBox(&d);
    auto* to = new QSpinBox(&d);
    for (QSpinBox* b : {from, to}) {
        b->setRange(1, frameCount);
    }
    from->setValue(tag.from + 1);
    to->setValue(tag.to + 1);
    auto* direction = new QComboBox(&d);
    direction->addItems({TimelinePanel::tr("Forward"), TimelinePanel::tr("Reverse"), TimelinePanel::tr("Ping-pong")});
    direction->setCurrentIndex(static_cast<int>(tag.direction));
    form->addRow(TimelinePanel::tr("Name:"), name);
    form->addRow(TimelinePanel::tr("From frame:"), from);
    form->addRow(TimelinePanel::tr("To frame:"), to);
    form->addRow(TimelinePanel::tr("Playback:"), direction);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    remove = false;
    if (existing) {
        QPushButton* del = buttons->addButton(TimelinePanel::tr("Delete Tag"), QDialogButtonBox::DestructiveRole);
        QObject::connect(del, &QPushButton::clicked, &d, [&] {
            remove = true;
            d.accept();
        });
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(buttons);
    if (d.exec() != QDialog::Accepted) {
        return false;
    }
    tag.name = name->text().trimmed().toStdString();
    tag.from = from->value() - 1;
    tag.to = to->value() - 1;
    tag.direction = static_cast<gas::TagDirection>(direction->currentIndex());
    return true;
}

} // namespace

void TimelinePanel::newTag() {
    if (!canEdit()) {
        return;
    }
    gas::FrameTag tag;
    tag.name = "tag" + std::to_string(document_->tags().size() + 1);
    if (rangeStart_ >= 0) {
        tag.from = std::min(rangeStart_, rangeEnd_);
        tag.to = std::max(rangeStart_, rangeEnd_);
    } else {
        tag.from = 0;
        tag.to = document_->frameCount() - 1;
    }
    bool remove = false;
    if (!tagDialog(this, tag, document_->frameCount(), false, remove) || tag.name.empty()) {
        return;
    }
    std::vector<gas::FrameTag> tags = document_->tags();
    tags.push_back(tag);
    if (document_->setTags(std::move(tags), "New Tag")) {
        rangeStart_ = rangeEnd_ = -1;
        Q_EMIT documentChanged();
    }
}

void TimelinePanel::editTag(int index) {
    if (!canEdit() || index < 0 || index >= static_cast<int>(document_->tags().size())) {
        return;
    }
    std::vector<gas::FrameTag> tags = document_->tags();
    bool remove = false;
    if (!tagDialog(this, tags[static_cast<std::size_t>(index)], document_->frameCount(), true, remove)) {
        return;
    }
    if (remove) {
        tags.erase(tags.begin() + index);
    }
    if (document_->setTags(std::move(tags), remove ? "Delete Tag" : "Edit Tag")) {
        Q_EMIT documentChanged();
    }
}
