#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QLocalSocket>
#include <QLocale>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>
#include "MainWindow.h"
#include "app/AccountManager.h"
#include "tk/gst_hw_probe.h"
#include "tk/i18n.h"
extern "C" {
}
#include <tesseract/client.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace
{

// SIGINT/SIGTERM (Ctrl+C, `kill`, session-manager shutdown, ...) default to
// killing the process outright, which skips every C++ destructor — including
// the one that flushes the Rust SDK's session/token state to disk. If a
// background OAuth token refresh has completed but not yet persisted at that
// exact moment, the next launch restores a stale, already-superseded refresh
// token, the homeserver rejects it, and Tesseract's own (correct) unrecoverable-
// auth-error handler wipes the entire local account. Route these signals
// through the normal, already-graceful "Quit" path instead: `main()`'s stack
// unwinds normally, `window`'s destructor tears down every `Client`, and each
// `ClientFfi::Drop` gets to run to completion.
//
// Signal handlers can only safely call async-signal-safe functions — no Qt
// API is on that list — so this uses the standard self-pipe trick: the
// handler just writes a byte to a socket pair, and a `QSocketNotifier`
// running on the main thread's event loop reads it and calls `qApp->quit()`.
int g_shutdown_signal_fd[2] = {-1, -1};

extern "C" void handle_shutdown_signal(int)
{
    char one = 1;
    // write() is async-signal-safe; errors are unrecoverable here anyway.
    [[maybe_unused]] auto n = ::write(g_shutdown_signal_fd[1], &one, sizeof(one));
}

void install_graceful_shutdown_signal_handlers(QApplication& app)
{
    // SOCK_NONBLOCK on both ends: the drain loop below reads until it empties
    // the pipe. On a blocking socket, once it reads the one byte a signal
    // wrote, the *next* read() has nothing left to consume and blocks
    // forever waiting for another signal — so app.quit() below is never
    // reached and Ctrl+C appears to freeze the app. Non-blocking makes that
    // read fail with EAGAIN instead, so the loop actually terminates.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, g_shutdown_signal_fd) != 0)
    {
        return; // best-effort: fall back to default (abrupt) signal behavior
    }
    auto* notifier = new QSocketNotifier(
        g_shutdown_signal_fd[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated,
                      [&app](QSocketDescriptor, QSocketNotifier::Type)
                      {
                          char buf[16];
                          while (::read(g_shutdown_signal_fd[0], buf, sizeof(buf)) > 0)
                          {
                          }
                          app.quit();
                      });

    struct sigaction sa{};
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

} // namespace

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
    install_graceful_shutdown_signal_handlers(app);

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
