#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <linux/limits.h>

#include <glib-unix.h>

#include "MainWindow.h"
#include "app/AccountManager.h"
#include "tk/gst_hw_probe.h"
#include "tk/i18n.h"
#include "tk/single_instance.h"
#include <tesseract/launch_args.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace
{

// SIGINT/SIGTERM default to killing the process outright, which skips every
// C++ destructor — including the one that flushes the Rust SDK's
// session/token state to disk. If a background OAuth token refresh has
// completed but not yet persisted at that exact moment, the next launch
// restores a stale, already-superseded refresh token, the homeserver rejects
// it, and Tesseract's own (correct) unrecoverable-auth-error handler wipes
// the entire local account. `g_unix_signal_add` is safe to use directly
// (unlike a raw POSIX signal handler) — GLib defers the actual callback to
// run on the main loop, not in signal-handler context — so it can just quit
// the application normally, letting `g_application_run` return and every
// `Client` destructor (and each `ClientFfi::Drop`) run to completion.
gboolean quit_on_unix_signal(gpointer data)
{
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

void install_graceful_shutdown_signal_handlers(GtkApplication* app)
{
    g_unix_signal_add(SIGINT, quit_on_unix_signal, app);
    g_unix_signal_add(SIGTERM, quit_on_unix_signal, app);
}

} // namespace

static std::string locale_dir()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
    {
        return {};
    }
    buf[n] = '\0';
    std::filesystem::path p{std::string(buf, static_cast<std::size_t>(n))};
    return (p.parent_path().parent_path() / "share" / "locale").string();
}

int main(int argc, char** argv)
{
    setlocale(LC_ALL, "");

    if (!getenv("GST_DEBUG"))
        setenv("GST_DEBUG", "0", 1);

    // Load persisted settings before set_locale so the saved language
    // preference is available when choosing the locale.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    // Mirror Qt6: probe GStreamer hardware decoders once at startup and cache
    // the results so broken hardware elements are demoted before first use.
    std::filesystem::create_directories(tesseract::cache_dir());
    tk::gst::apply_hw_decoder_cache(tesseract::cache_dir().string());

    // Determine locale name: use saved preference if non-auto, else derive
    // from the environment (setlocale already called above).
    std::string lang = tesseract::Settings::instance().language;
    if (lang == "auto" || lang.empty())
    {
        const char* lc_messages = setlocale(LC_MESSAGES, nullptr);
        lang = (lc_messages && lc_messages[0] != 'C' && lc_messages[0] != '\0')
                   ? lc_messages
                   : "en";
        // Strip encoding suffix if present (e.g. "es_MX.UTF-8" -> "es_MX")
        auto dot = lang.find('.');
        if (dot != std::string::npos)
            lang.erase(dot);
    }

    std::string ldir = locale_dir();
    std::string i18n_dir;
    if (!ldir.empty())
    {
        // ldir is <prefix>/share/locale; i18n dir is <prefix>/share/tesseract/i18n
        std::filesystem::path p{ldir};
        i18n_dir = (p.parent_path() / "tesseract" / "i18n").string();
    }
    tk::set_locale(i18n_dir, lang);

    // Parse argv once (order-independent: --autostart and a matrix URI may
    // appear together or alone). Shared with the other shells via
    // client/src/launch_args.cpp instead of each hand-rolling its own
    // single-arg check.
    static std::string startup_uri;
    static bool start_hidden = false;
    {
        tesseract::LaunchArgs launch = tesseract::parse_launch_args(
            std::vector<std::string>(argv + 1, argv + argc));
        start_hidden = launch.autostart;
        if (launch.matrix_uri)
            startup_uri = *launch.matrix_uri;

        // Strip every recognised arg out of argv before GApplication sees
        // it — otherwise G_APPLICATION_HANDLES_OPEN treats a leftover
        // "--autostart" (or the URI, once handled locally) as a file to
        // open and fires the "open" signal with garbage.
        std::vector<char*> filtered{argv[0]};
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--autostart" || (launch.matrix_uri && a == *launch.matrix_uri))
                continue;
            filtered.push_back(argv[i]);
        }
        argc = static_cast<int>(filtered.size());
        for (int i = 0; i < argc; ++i)
            argv[i] = filtered[static_cast<std::size_t>(i)];
    }

    // Single-instance guard shared with the Qt6 build (same flock path, same
    // activation-socket protocol) — GApplication's own D-Bus uniqueness only
    // catches a second GTK launch, not a Qt one already running (or vice
    // versa), which could otherwise open the same matrix-sdk store twice.
    if (!tk::acquire_single_instance_lock().acquired)
    {
        // --autostart has no meaningful action against an already-running
        // instance — exit quietly without forwarding anything.
        if (!start_hidden)
        {
            const char* tok = getenv("XDG_ACTIVATION_TOKEN");
            tk::forward_activation_request(tok ? tok : "", startup_uri);
        }
        return 0;
    }

    GtkApplication* app =
        gtk_application_new("org.tesseract.gtk", G_APPLICATION_HANDLES_OPEN);
    install_graceful_shutdown_signal_handlers(app);

    tesseract::AccountManager account_manager;
    std::unique_ptr<gtk4::MainWindow> window;

    // Listen for activation requests forwarded by a later launch of either
    // backend (we're the lock holder from here on) and raise/route them the
    // same way GTK's own "activate"/"open" D-Bus signals do below.
    auto activation_listener = std::make_unique<tk::ActivationListener>(
        [&window](std::string token, std::string uri)
        {
            if (!token.empty())
                setenv("XDG_ACTIVATION_TOKEN", token.c_str(), 1);
            if (window)
            {
                window->present();
                if (!uri.empty())
                    window->open_matrix_link(uri);
            }
        });
    if (activation_listener->fd() >= 0)
    {
        g_unix_fd_add(
            activation_listener->fd(), G_IO_IN,
            +[](gint, GIOCondition, gpointer data) -> gboolean
            {
                static_cast<tk::ActivationListener*>(data)->on_readable();
                return G_SOURCE_CONTINUE;
            },
            activation_listener.get());
    }

    struct ActivateData {
        tesseract::AccountManager* account_manager;
        std::unique_ptr<gtk4::MainWindow>* window;
    };
    ActivateData activate_data{&account_manager, &window};

    g_signal_connect(
        app, "activate",
        G_CALLBACK(
            +[](GtkApplication* app, gpointer data)
            {
                auto& d = *static_cast<ActivateData*>(data);
                auto& win = *d.window;
                if (!win)
                {
                    win = std::make_unique<gtk4::MainWindow>(
                        *d.account_manager, app, start_hidden);
                    if (!startup_uri.empty())
                    {
                        win->open_matrix_link(startup_uri);
                        startup_uri.clear();
                    }
                }
                else
                {
                    win->present(); // second-instance launch raises the existing window
                }
            }),
        &activate_data);

    // Handles matrix: URIs dispatched via D-Bus activation (xdg-open / second instance).
    g_signal_connect(
        app, "open",
        G_CALLBACK(
            +[](GApplication*, GFile** files, gint n_files, const gchar*, gpointer data)
            {
                auto& win = *static_cast<ActivateData*>(data)->window;
                if (n_files > 0 && win)
                {
                    char* uri = g_file_get_uri(files[0]);
                    if (uri)
                    {
                        win->open_matrix_link(std::string(uri));
                        g_free(uri);
                    }
                }
            }),
        &activate_data);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
