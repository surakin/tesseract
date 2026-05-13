// This translation unit is compiled with GTK3 + libayatana-appindicator
// include paths (see ui/linux-gtk/CMakeLists.txt — separate static library
// `tesseract_gtk_tray`). It must not include any GTK4 headers; the rest of
// the GTK4 shell sees this code only through the type-erased header.

#include "LinuxGtkTrayIcon.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>   // GTK3 (pulled in by app-indicator.h)
#include <gio/gio.h>

#include <cstdlib>
#include <string>

namespace {

bool status_notifier_host_present() {
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus) { g_clear_error(&err); return false; }

    GVariant* result = g_dbus_connection_call_sync(
        bus,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)",
                      "org.kde.StatusNotifierWatcher",
                      "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        nullptr,
        &err);

    g_object_unref(bus);
    if (!result) { g_clear_error(&err); return false; }

    GVariant* inner = nullptr;
    g_variant_get(result, "(v)", &inner);
    const bool present = inner && g_variant_get_boolean(inner);
    if (inner) g_variant_unref(inner);
    g_variant_unref(result);
    return present;
}

void on_show_activate(GtkMenuItem*, gpointer data) {
    auto* fn = static_cast<std::function<void()>*>(data);
    if (fn && *fn) (*fn)();
}

void on_quit_activate(GtkMenuItem*, gpointer data) {
    auto* fn = static_cast<std::function<void()>*>(data);
    if (fn && *fn) (*fn)();
}

// Resolve the path to the application icon. The build copies the SVG into
// TESSERACT_ICON_SEARCH_PATH/hicolor/scalable/apps/tesseract.svg; prefer that
// during dev, fall back to the install-tree icon-theme name "tesseract" which
// app_indicator_set_icon_full will resolve through hicolor.
const char* resolve_icon_path() {
#ifdef TESSERACT_ICON_SEARCH_PATH
    static const std::string dev_path =
        std::string(TESSERACT_ICON_SEARCH_PATH) +
        "/hicolor/scalable/apps/tesseract.svg";
    if (g_file_test(dev_path.c_str(), G_FILE_TEST_EXISTS))
        return dev_path.c_str();
#endif
    return "tesseract";
}

} // namespace

LinuxGtkTrayIcon::LinuxGtkTrayIcon(std::function<void()> on_show,
                                   std::function<void()> on_quit)
    : on_show_(std::move(on_show)),
      on_quit_(std::move(on_quit))
{
    if (!status_notifier_host_present()) {
        // No StatusNotifier host (no extension on GNOME, no system tray on
        // a minimal compositor, etc.). Leave available_ = false so the shell
        // falls back to a real quit on window close.
        return;
    }

    // libayatana-appindicator's API takes GtkMenu* — these are GTK3 widgets.
    // gtk_init() may already have been called transitively; calling it again
    // is a no-op. We don't run a GTK3 main loop — the indicator's D-Bus
    // worker runs independently of GTK's loop.
    if (!gtk_init_check(nullptr, nullptr)) {
        return;
    }

    AppIndicator* ind = app_indicator_new(
        "io.gnomos.Tesseract",
        resolve_icon_path(),
        APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    if (!ind) return;
    indicator_ = ind;

    GtkWidget* menu = gtk_menu_new();
    menu_ = menu;

    GtkWidget* show_item = gtk_menu_item_new_with_label("Show App");
    g_signal_connect(show_item, "activate",
                     G_CALLBACK(on_show_activate),
                     static_cast<gpointer>(&on_show_));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);

    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate",
                     G_CALLBACK(on_quit_activate),
                     static_cast<gpointer>(&on_quit_));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    app_indicator_set_menu(ind, GTK_MENU(menu));
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(ind, "Tesseract");

    available_ = true;
}

LinuxGtkTrayIcon::~LinuxGtkTrayIcon() {
    if (indicator_) {
        app_indicator_set_status(static_cast<AppIndicator*>(indicator_),
                                  APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(static_cast<AppIndicator*>(indicator_));
        indicator_ = nullptr;
    }
    if (menu_) {
        gtk_widget_destroy(static_cast<GtkWidget*>(menu_));
        menu_ = nullptr;
    }
}

void LinuxGtkTrayIcon::set_tooltip(const std::string& text) {
    if (indicator_) {
        app_indicator_set_title(static_cast<AppIndicator*>(indicator_),
                                 text.c_str());
    }
}
