#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <linux/limits.h>

#include "MainWindow.h"
#include "app/AccountManager.h"
#include "tk/i18n.h"
#include <tesseract/paths.h>
#include <tesseract/settings.h>

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

    // Check for a matrix: URI on the command line before GApplication takes over argv.
    static std::string startup_uri;
    if (argc >= 2)
    {
        std::string arg = argv[1];
        if (tesseract::Client::parse_matrix_link(arg).kind
            != tesseract::Client::MatrixLink::Kind::Unknown)
        {
            startup_uri = arg;
            // Remove the URI from argv so GApplication doesn't treat it as a file.
            argc = 1;
        }
    }

    GtkApplication* app =
        gtk_application_new("org.tesseract.gtk", G_APPLICATION_HANDLES_OPEN);

    tesseract::AccountManager account_manager;
    std::unique_ptr<gtk4::MainWindow> window;

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
                    win = std::make_unique<gtk4::MainWindow>(*d.account_manager, app);
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
