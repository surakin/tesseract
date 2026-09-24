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
#include "app/Launch.h"
#include "tk/gst_hw_probe.h"
#include "tk/i18n.h"
#include "tk/single_instance.h"
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

    // Shared startup pipeline (ui/shared/app/Launch.h): parses argv, selects
    // --profile, loads settings + locale, and handles --help / --version /
    // --logoutall before any GTK/GApplication state exists.
    tesseract::LaunchHooks hooks;
    hooks.detect_system_lang = []
    {
        // Derive from the environment (setlocale already called above).
        const char* lc_messages = setlocale(LC_MESSAGES, nullptr);
        std::string lang =
            (lc_messages && lc_messages[0] != 'C' && lc_messages[0] != '\0')
                ? lc_messages
                : "en";
        // Strip encoding suffix if present (e.g. "es_MX.UTF-8" -> "es_MX")
        auto dot = lang.find('.');
        if (dot != std::string::npos)
        {
            lang.erase(dot);
        }
        return lang;
    };
    hooks.i18n_dir = []
    {
        // locale_dir() is <prefix>/share/locale; the i18n dir is
        // <prefix>/share/tesseract/i18n.
        std::string ldir = locale_dir();
        if (ldir.empty())
        {
            return std::string{};
        }
        return (std::filesystem::path{ldir}.parent_path() / "tesseract" / "i18n")
            .string();
    };
    hooks.acquire_instance_lock = []
    { return tk::acquire_single_instance_lock().acquired; };
    static const tesseract::LaunchPlan plan = tesseract::prepare_launch(
        std::vector<std::string>(argv + 1, argv + argc), hooks);
    if (plan.exit_code)
    {
        return *plan.exit_code;
    }

    // Mirror Qt6: probe GStreamer hardware decoders once at startup and cache
    // the results so broken hardware elements are demoted before first use.
    std::filesystem::create_directories(tesseract::cache_dir());
    tk::gst::apply_hw_decoder_cache(tesseract::cache_dir().string());

    static std::string startup_uri = plan.args.matrix_uri.value_or(std::string{});
    static const bool start_hidden = plan.start_hidden();
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    static const std::filesystem::path screenshot_dir =
        plan.args.screenshot_dir.value_or(std::string{});
#endif

    // Every launch intent was consumed above, so GApplication gets only the
    // program name. Forwarding the rest would make G_APPLICATION_HANDLES_OPEN
    // treat leftovers ("--autostart", the URI, ...) as files to open and fire
    // the "open" signal with garbage — and GApplication's own option parser
    // would reject flags it doesn't know.
    argc = 1;

    // Single-instance guard shared with the Qt6 build (same flock path, same
    // activation-socket protocol) — GApplication's own D-Bus uniqueness only
    // catches a second GTK launch, not a Qt one already running (or vice
    // versa), which could otherwise open the same matrix-sdk store twice.
    if (
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        screenshot_dir.empty() &&
#endif
        !tk::acquire_single_instance_lock().acquired)
    {
        // A hidden/autostart launch with nothing to forward has no meaningful
        // action against an already-running instance — exit quietly.
        if (plan.should_raise_existing_instance())
        {
            tk::forward_activation_request(tk::activation_request_for(plan.args));
        }
        return 0;
    }

    // A named profile gets its own application id: GApplication's D-Bus
    // uniqueness would otherwise hand this launch to a running instance of a
    // *different* profile. The "p_" prefix keeps the element valid even when
    // the profile name starts with a digit.
    std::string app_id = "org.tesseract.gtk";
    if (!tesseract::profile().empty())
    {
        app_id += ".p_" + tesseract::profile();
    }
    GtkApplication* app =
        gtk_application_new(app_id.c_str(), G_APPLICATION_HANDLES_OPEN);
    install_graceful_shutdown_signal_handlers(app);

    tesseract::AccountManager account_manager;
    std::unique_ptr<gtk4::MainWindow> window;

    // Listen for activation requests forwarded by a later launch of either
    // backend (we're the lock holder from here on) and raise/route them the
    // same way GTK's own "activate"/"open" D-Bus signals do below.
    auto activation_listener = std::make_unique<tk::ActivationListener>(
        [&window](tk::ActivationRequest req)
        {
            if (!req.token.empty())
                setenv("XDG_ACTIVATION_TOKEN", req.token.c_str(), 1);
            if (window)
            {
                window->present();
                if (!req.uri.empty())
                    window->open_matrix_link(req.uri);
                window->dispatch_launch_action(
                    tesseract::launch_action_from_option_id(req.action),
                    std::move(req.room_id));
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
                        *d.account_manager, app, start_hidden
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                        , screenshot_dir
#endif
                        );
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                    if (!screenshot_dir.empty())
                        win->start_screenshot_mode();
#endif
                    if (!startup_uri.empty())
                    {
                        win->open_matrix_link(startup_uri);
                        startup_uri.clear();
                    }
                    // Held by ShellBase until the main content is showing.
                    win->dispatch_launch_action(
                        plan.args.action,
                        plan.args.room_id.value_or(std::string{}));
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
