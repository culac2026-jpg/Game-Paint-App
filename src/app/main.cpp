#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GameArt Studio"));
    QApplication::setApplicationVersion(QStringLiteral(GAS_VERSION));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    window.offerRecovery(); // autosaves left behind by a crash
    // Paths on the command line (e.g. from "Open with"): a project is opened,
    // images are imported (several become one sheet with a slice each).
    const QStringList args = QApplication::arguments().mid(1);
    if (args.size() == 1 && args.front().endsWith(QStringLiteral(".gas"), Qt::CaseInsensitive)) {
        window.openFile(args.front());
    } else if (!args.isEmpty()) {
        window.importImageFiles(args);
    }
    return QApplication::exec();
}
