#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocalSocket>
#include <QTranslator>
#include <QLocale>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    // Single-instance guard via a per-user lock file.
    // flock() releases automatically when the process exits or the fd closes.
    std::string lock_path =
        "/tmp/tesseract-" + std::to_string(getuid()) + ".lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        // Another instance holds the lock.  Forward any compositor-issued
        // XDG_ACTIVATION_TOKEN so the existing window can raise itself on
        // Wayland, then exit.
        close(lock_fd);
        QCoreApplication app(argc, argv);
        const QString act_name = QStringLiteral("tesseract-activate-")
                                 + QString::number(getuid());
        QLocalSocket sock;
        sock.connectToServer(act_name);
        if (sock.waitForConnected(200))
        {
            const char* tok = std::getenv("XDG_ACTIVATION_TOKEN");
            sock.write(QByteArray(tok ? tok : "").append('\n'));
            sock.flush();
            sock.waitForBytesWritten(200);
        }
        return 0;
    }

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
    window.activateOnStartup();

    return app.exec();

}
