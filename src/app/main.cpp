#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GameArt Studio"));
    QApplication::setApplicationVersion(QStringLiteral(GAS_VERSION));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    // A project path on the command line (e.g. from "Open with") is opened directly.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) {
        window.openFile(args.at(1));
    }
    return QApplication::exec();
}
