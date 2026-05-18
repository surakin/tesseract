#include <clocale>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <linux/limits.h>
#include <libintl.h>

#include "MainWindow.h"

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
    std::string ldir = locale_dir();
    if (!ldir.empty())
    {
        bindtextdomain("tesseract", ldir.c_str());
    }
    bind_textdomain_codeset("tesseract", "UTF-8");
    textdomain("tesseract");
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
