#pragma once

#include <gas/core/Recovery.h>

#include <QDialog>

#include <vector>

class QListWidget;
class QPushButton;

// Shown at startup when autosaved sessions survived a crash: the user can
// recover one into the window, delete them, or keep them for later.
class RecoveryDialog : public QDialog {
    Q_OBJECT

public:
    enum class Choice { Later, Recover };

    RecoveryDialog(const gas::RecoveryStore& store, std::vector<gas::RecoveryEntry> entries,
                   QWidget* parent = nullptr);

    Choice choice() const { return choice_; }
    // The entry to recover (valid when choice() == Recover).
    const gas::RecoveryEntry& selected() const;

private:
    void rebuild();
    void deleteSelected();

    const gas::RecoveryStore& store_;
    std::vector<gas::RecoveryEntry> entries_;
    QListWidget* list_ = nullptr;
    QPushButton* recoverButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    Choice choice_ = Choice::Later;
};
