#include "RecoveryDialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

RecoveryDialog::RecoveryDialog(const gas::RecoveryStore& store, std::vector<gas::RecoveryEntry> entries,
                               QWidget* parent)
    : QDialog(parent), store_(store), entries_(std::move(entries)) {
    setWindowTitle(tr("Recover Unsaved Work"));
    auto* intro = new QLabel(tr("GameArt Studio did not close normally. These documents were autosaved and can "
                                "be recovered:"),
                             this);
    intro->setWordWrap(true);
    list_ = new QListWidget(this);
    list_->setMinimumSize(520, 160);
    auto* buttons = new QDialogButtonBox(this);
    recoverButton_ = buttons->addButton(tr("Recover"), QDialogButtonBox::AcceptRole);
    deleteButton_ = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    QPushButton* later = buttons->addButton(tr("Not Now"), QDialogButtonBox::RejectRole);
    later->setToolTip(tr("Keep the autosaves; you will be asked again next time"));
    recoverButton_->setDefault(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(list_, 1);
    layout->addWidget(buttons);

    connect(recoverButton_, &QPushButton::clicked, this, [this] {
        if (list_->currentRow() >= 0) {
            choice_ = Choice::Recover;
            accept();
        }
    });
    connect(list_, &QListWidget::itemDoubleClicked, recoverButton_, &QPushButton::click);
    connect(deleteButton_, &QPushButton::clicked, this, &RecoveryDialog::deleteSelected);
    connect(later, &QPushButton::clicked, this, &QDialog::reject);
    rebuild();
}

const gas::RecoveryEntry& RecoveryDialog::selected() const {
    return entries_.at(static_cast<std::size_t>(std::max(0, list_->currentRow())));
}

void RecoveryDialog::rebuild() {
    list_->clear();
    const QLocale locale;
    for (const gas::RecoveryEntry& e : entries_) {
        const QString original = QString::fromUtf8(reinterpret_cast<const char*>(e.originalPath.data()),
                                                   static_cast<qsizetype>(e.originalPath.size()));
        const QString title = QString::fromStdString(e.title);
        const QString when = locale.toString(QDateTime::fromSecsSinceEpoch(e.savedAt), QLocale::ShortFormat);
        auto* item = new QListWidgetItem(
            tr("%1\n    autosaved %2  •  %3 KiB  •  %4")
                .arg(title.isEmpty() ? tr("Untitled") : title, when)
                .arg(static_cast<double>(e.bytes) / 1024.0, 0, 'f', 0)
                .arg(original.isEmpty() ? tr("never saved") : QFileInfo(original).absoluteFilePath()),
            list_);
        item->setToolTip(QString::fromStdString(e.project.string()));
    }
    list_->setCurrentRow(entries_.empty() ? -1 : 0);
    recoverButton_->setEnabled(!entries_.empty());
    deleteButton_->setEnabled(!entries_.empty());
}

void RecoveryDialog::deleteSelected() {
    const int row = list_->currentRow();
    if (row < 0) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete Autosave"),
                              tr("Delete this autosave? Its unsaved changes will be lost.")) != QMessageBox::Yes) {
        return;
    }
    store_.remove(entries_[static_cast<std::size_t>(row)].session);
    entries_.erase(entries_.begin() + row);
    if (entries_.empty()) {
        reject();
        return;
    }
    rebuild();
}
