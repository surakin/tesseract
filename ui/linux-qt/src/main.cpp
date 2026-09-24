#include <QApplication>
#include <QDBusInterface>
#include <QDir>
#include <QIcon>
#include <QLocale>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include "MainWindow.h"
#include "app/AccountManager.h"
#include "app/Launch.h"
#include "tk/gst_hw_probe.h"
#include "tk/qt_accessible.h"
#include "tk/i18n.h"
#include "tk/single_instance.h"
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

    // Shared startup pipeline (ui/shared/app/Launch.h): parses argv, selects
    // --profile, loads settings + locale, and handles --help / --version /
    // --logoutall. Runs before QApplication so those exit without touching
    // the display. Qt's own switches (-platform, -style, ...) are skipped by
    // the parser and still reach QApplication below via the untouched argv.
    tesseract::LaunchHooks hooks;
    hooks.detect_system_lang = []
    { return QLocale::system().name().toStdString(); };
    hooks.i18n_dir = []
    {
        // <prefix>/bin/tesseract -> <prefix>/share/tesseract/i18n, the same
        // location QCoreApplication::applicationDirPath() resolved before
        // this ran ahead of QApplication.
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec)
        {
            return std::string{};
        }
        return (exe.parent_path() / ".." / "share" / "tesseract" / "i18n").string();
    };
    hooks.acquire_instance_lock = []
    { return tk::acquire_single_instance_lock().acquired; };
    const tesseract::LaunchPlan plan = tesseract::prepare_launch(
        std::vector<std::string>(argv + 1, argv + argc), hooks);
    if (plan.exit_code)
    {
        return *plan.exit_code;
    }
    const tesseract::LaunchArgs& launch = plan.args;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    const bool screenshot_mode = launch.screenshot_dir.has_value();
#endif

    // Single-instance guard shared with the GTK4 build (same flock path,
    // same activation-socket protocol — see ui/shared/tk/single_instance.h)
    // so launching one backend while the other already holds the lock is
    // detected too, not just two instances of the same backend.
    if (
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        !screenshot_mode &&
#endif
        !tk::acquire_single_instance_lock().acquired)
    {
        // A hidden/autostart launch with nothing to forward has no meaningful
        // action against an already-running instance — exit quietly.
        if (!plan.should_raise_existing_instance())
        {
            return 0;
        }
        // Another instance holds the lock. Forward any compositor-issued
        // XDG_ACTIVATION_TOKEN (plus matrix URI / launch action) so the
        // existing window can raise itself and act on it, then exit.
        tk::forward_activation_request(tk::activation_request_for(launch));
        return 0;
    }

    QApplication app(argc, argv);
    // Native QAccessibleInterface bridge — see ui/linux-qt/tk/qt_accessible.h.
    tk::qt6::install_accessible_factory();
    app.setApplicationName("Tesseract");
    // Matches the desktop file basename packages actually install
    // (packaging/tesseract.desktop.in is renamed tesseract-matrix.desktop —
    // see packaging/debian/rules, packaging/arch/PKGBUILD.in — same name
    // LinuxAutostartQt.cpp already uses). Read back via desktopFileName() in
    // LinuxNotifier.cpp to populate the "desktop-entry" hint on legacy D-Bus
    // notifications — required for the daemon's ActivationToken signal to
    // know which app to mint a Wayland xdg_activation_v1 token for (see
    // KDE's own knotifications/src/notifybypopup.cpp, and GNOME Shell's
    // equivalent convention). Also sets the Wayland surface's app_id.
    app.setDesktopFileName(QStringLiteral("tesseract-matrix"));

    // Self-register this process's identity with xdg-desktop-portal. An
    // unpackaged/non-Flatpak binary has no reliable way for the portal to
    // resolve an app-id via cgroup/desktop-file heuristics alone (KDE's own
    // docs call that detection "unlikely to ever be foolproof"), which can
    // silently break portal-backed features — e.g. notifications requested
    // via org.freedesktop.portal.Notification with no resolvable identity
    // simply not appearing. Must be this process's first portal call of any
    // kind (a later or repeated call errors); it's also a no-op error when
    // already running sandboxed, where identity is already known — so this
    // is unconditional and best-effort, not gated to dev builds only.
    {
        QDBusInterface registry("org.freedesktop.portal.Desktop",
                                "/org/freedesktop/portal/desktop",
                                "org.freedesktop.host.portal.Registry");
        registry.call("Register", QStringLiteral("tesseract-matrix"),
                      QVariantMap{});
    }

    install_graceful_shutdown_signal_handlers(app);

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
    qt6::MainWindow window{account_manager, nullptr, plan.start_hidden()
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                           , screenshot_mode
#endif
    };
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (screenshot_mode)
    {
        window.show();
        window.captureScreenshots(*launch.screenshot_dir);
        return app.exec();
    }
#endif
    if (!plan.start_hidden())
    {
        window.show();
        window.activateOnStartup();
    }
    // Else: stays hidden until MainWindow::doLogin()'s async restore
    // completes — hidden (tray-only) on a successful silent restore, or
    // force-shown if there's no saved session to restore.

    if (launch.matrix_uri)
    {
        window.openMatrixLink(*launch.matrix_uri);
    }
    // Held by ShellBase until the main content is showing.
    window.dispatchLaunchAction(launch.action,
                                launch.room_id.value_or(std::string{}));

    return app.exec();

}
