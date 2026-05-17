#include <QApplication>
#include <QIcon>
#include <QTranslator>
#include <QLocale>
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Tesseract");

    QTranslator translator;
    if (translator.load(QLocale(), "tesseract", "_",
            app.applicationDirPath() + "/../share/translations"))
    {
        app.installTranslator(&translator);
    }
    app.setOrganizationName("tesseract");
    app.setWindowIcon(QIcon(":/icons/tesseract.svg"));

    qt6::MainWindow window;
    window.show();

    return app.exec();
}
