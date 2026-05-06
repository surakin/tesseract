#include <QApplication>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Matrix Client");
    app.setOrganizationName("matrix");

    qt6::MainWindow window;
    window.show();

    return app.exec();
}
