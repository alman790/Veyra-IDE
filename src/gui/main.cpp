#include "MainWindow.hpp"
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QSettings>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Veyra");
    QApplication::setApplicationName("Veyra IDE");
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPixelSize(14);
    app.setFont(font);

    const auto root = QSettings().value("workspace/root").toString();
    if (!root.isEmpty() && QDir(root).exists())
        QDir::setCurrent(root);
    MainWindow window;
    QApplication::setWindowIcon(window.windowIcon());
    window.show();
    return app.exec();
}
