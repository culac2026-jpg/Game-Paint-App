#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GameArt Studio"));
    QApplication::setApplicationVersion(QStringLiteral(GAS_VERSION));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    return QApplication::exec();
}
