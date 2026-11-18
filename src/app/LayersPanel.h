#pragma once

#include <QWidget>

namespace gas {
class Document;
}

class QCheckBox;
class QListWidget;
class QListWidgetItem;
class QSpinBox;
class QToolButton;

// Layer list (top layer first) with visibility checkboxes, inline rename,
// opacity, lock, and add / duplicate / delete / reorder buttons. Every change
// goes through gas::Document so it is undoable.
class LayersPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);

    void setDocument(gas::Document* document); // non-owning
    // Re-reads the document (call after undo/redo or any external change).
    void refresh();

Q_SIGNALS:
    // The panel changed the document or the active layer.
    void documentChanged();

private:
    int rowToIndex(int row) const;
    int indexToRow(int index) const;
    void onItemChanged(QListWidgetItem* item);
    void onCurrentRowChanged(int row);
    void setOpacity(int percent);
    void setLocked(bool locked);
    void moveActive(int delta);

    gas::Document* document_ = nullptr;
    QListWidget* list_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QCheckBox* lock_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* duplicateButton_ = nullptr;
    QToolButton* deleteButton_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QToolButton* downButton_ = nullptr;
};
