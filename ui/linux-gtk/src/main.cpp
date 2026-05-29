#include <clocale>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <linux/limits.h>

#include "MainWindow.h"
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

    GtkApplication* app =
        gtk_application_new("org.tesseract.gtk", G_APPLICATION_DEFAULT_FLAGS);

    std::unique_ptr<gtk4::MainWindow> window;

    g_signal_connect(
        app, "activate",
        G_CALLBACK(
            +[](GtkApplication* app, gpointer data)
            {
                auto& win =
                    *static_cast<std::unique_ptr<gtk4::MainWindow>*>(data);
                if (!win)
                {
                    win = std::make_unique<gtk4::MainWindow>(app);
                }
                else
                {
                    win->present(); // second-instance launch raises the existing window
                }
            }),
        &window);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
