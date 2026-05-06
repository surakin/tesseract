#include "MainWindow.h"
#include <memory>

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new(
        "org.matrix.client.gtk", G_APPLICATION_DEFAULT_FLAGS);

    std::unique_ptr<gtk4::MainWindow> window;

    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
        auto*& win = *static_cast<std::unique_ptr<gtk4::MainWindow>*>(data);
        if (!win) win = std::make_unique<gtk4::MainWindow>(app);
    }), &window);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
