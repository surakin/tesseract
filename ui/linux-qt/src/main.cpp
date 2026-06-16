#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QLocalSocket>
#include <QLocale>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include "MainWindow.h"
#include "app/AccountManager.h"
#include "tk/gst_hw_probe.h"
#include "tk/i18n.h"
extern "C" {
#include <libavutil/log.h>
}
#include <tesseract/client.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

int main(int argc, char* argv[])
{
#ifdef NDEBUG
    // Silence Qt multimedia category noise (FFmpeg backend banner).
    QLoggingCategory::setFilterRules("qt.multimedia.*=false");
    // "spaVisitChoice: parse error ..." is a bare qWarning() inside Qt's
    // PipeWire SPA code — not a named category — so filter rules can't catch
    // it.  Install a handler that drops those lines and writes everything else
    // to stderr via qFormatLogMessage (replicates the Qt default handler).
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx,
                              const QString& msg) {
        if (msg.startsWith(QLatin1String("spaVisitChoice:")))
            return;
        fprintf(stderr, "%s\n",
                qFormatLogMessage(type, ctx, msg).toLocal8Bit().constData());
        fflush(stderr);
        if (type == QtFatalMsg)
            abort();
    });
#endif

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
            if (argc >= 2)
            {
                std::string arg = argv[1];
                if (tesseract::Client::parse_matrix_link(arg).kind
                    != tesseract::Client::MatrixLink::Kind::Unknown)
                {
                    sock.write(QByteArray::fromStdString(arg).append('\n'));
                }
            }
            sock.flush();
            sock.waitForBytesWritten(200);
        }
        return 0;
    }

    QApplication app(argc, argv);
    app.setApplicationName("Tesseract");

    // Load persisted settings before set_locale so the saved language
    // preference is available when choosing the locale.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    {
        std::string lang = tesseract::Settings::instance().language;
        if (lang == "auto" || lang.empty())
        {
            lang = QLocale::system().name().toStdString();
        }
        tk::set_locale(
            (app.applicationDirPath() + "/../share/tesseract/i18n").toStdString(),
            lang);
    }
    app.setOrganizationName("tesseract");
    app.setWindowIcon(QIcon(":/icons/tesseract.svg"));

    // Silence FFmpeg / gst-libav diagnostic output (e.g. "Input #0, mov,mp4...")
    // before GStreamer (and its av_log hook) are initialised.
    av_log_set_level(AV_LOG_QUIET);
    if (!getenv("GST_DEBUG"))
        setenv("GST_DEBUG", "0", 1);

    {
        const QString cache_dir =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache_dir);
        tk::gst::apply_hw_decoder_cache(cache_dir.toStdString());
    }

    tesseract::AccountManager account_manager;
    qt6::MainWindow window{account_manager};
    window.show();
    window.activateOnStartup();

    if (argc >= 2)
    {
        std::string arg = argv[1];
        if (tesseract::Client::parse_matrix_link(arg).kind
            != tesseract::Client::MatrixLink::Kind::Unknown)
        {
            window.openMatrixLink(arg);
        }
    }

    return app.exec();

}
