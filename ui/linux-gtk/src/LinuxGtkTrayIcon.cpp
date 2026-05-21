// This translation unit is compiled with GTK3 + libayatana-appindicator
// include paths (see ui/linux-gtk/CMakeLists.txt — separate static library
// `tesseract_gtk_tray`). It must not include any GTK4 headers; the rest of
// the GTK4 shell sees this code only through the type-erased header.

#include "LinuxGtkTrayIcon.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h> // GTK3 (pulled in by app-indicator.h)
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>
#include <string>

namespace
{

bool status_notifier_host_present()
{
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus)
    {
        g_clear_error(&err);
        return false;
    }

    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.StatusNotifierWatcher",
                      "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &err);

    g_object_unref(bus);
    if (!result)
    {
        g_clear_error(&err);
        return false;
    }

    GVariant* inner = nullptr;
    g_variant_get(result, "(v)", &inner);
    const bool present = inner && g_variant_get_boolean(inner);
    if (inner)
    {
        g_variant_unref(inner);
    }
    g_variant_unref(result);
    return present;
}

void on_show_activate(GtkMenuItem*, gpointer data)
{
    auto* fn = static_cast<std::function<void()>*>(data);
    if (fn && *fn)
    {
        (*fn)();
    }
}

void on_quit_activate(GtkMenuItem*, gpointer data)
{
    auto* fn = static_cast<std::function<void()>*>(data);
    if (fn && *fn)
    {
        (*fn)();
    }
}

// Resolve the path to the application icon. The build copies the SVG into
// TESSERACT_ICON_SEARCH_PATH/hicolor/scalable/apps/tesseract.svg; prefer that
// during dev, fall back to the install-tree icon-theme name "tesseract" which
// app_indicator_set_icon_full will resolve through hicolor.
const char* resolve_icon_path()
{
#ifdef TESSERACT_ICON_SEARCH_PATH
    static const std::string dev_path =
        std::string(TESSERACT_ICON_SEARCH_PATH) +
        "/hicolor/scalable/apps/tesseract.svg";
    if (g_file_test(dev_path.c_str(), G_FILE_TEST_EXISTS))
    {
        return dev_path.c_str();
    }
#endif
    return nullptr;
}

// Render one tray variant (base icon + optional coloured dot in the bottom-
// right) to `out_path`. Returns true on success. dot_rgb < 0 → no overlay.
bool render_variant_png(GdkPixbuf* base, int side, std::int32_t dot_rgb,
                        const std::string& out_path)
{
    if (!base)
    {
        return false;
    }
    cairo_surface_t* surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
    {
        if (surf)
        {
            cairo_surface_destroy(surf);
        }
        return false;
    }
    cairo_t* cr = cairo_create(surf);

    // Paint the base.
    gdk_cairo_set_source_pixbuf(cr, base, 0.0, 0.0);
    cairo_paint(cr);

    if (dot_rgb >= 0)
    {
        // Dot at ~38% of side, anchored bottom-right with a small inset so
        // the white outline isn't clipped.
        const double dot   = std::max(8, side * 38 / 100);
        const double inset = std::max(1.0, side / 32.0);
        const double cx    = side - dot / 2.0 - inset;
        const double cy    = side - dot / 2.0 - inset;
        const double r     = dot / 2.0;

        const double rr = ((dot_rgb >> 16) & 0xFF) / 255.0;
        const double gg = ((dot_rgb >> 8)  & 0xFF) / 255.0;
        const double bb = ( dot_rgb        & 0xFF) / 255.0;

        cairo_set_source_rgb(cr, rr, gg, bb);
        cairo_arc(cr, cx, cy, r, 0.0, 2.0 * G_PI);
        cairo_fill_preserve(cr);
        // White outline; legible on both light and dark trays.
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, std::max(1.0, side / 32.0));
        cairo_stroke(cr);
    }

    cairo_status_t write_status =
        cairo_surface_write_to_png(surf, out_path.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return write_status == CAIRO_STATUS_SUCCESS;
}

} // namespace

LinuxGtkTrayIcon::LinuxGtkTrayIcon(std::function<void()> on_show,
                                   std::function<void()> on_quit)
    : on_show_(std::move(on_show)), on_quit_(std::move(on_quit))
{
    if (!status_notifier_host_present())
    {
        // No StatusNotifier host (no extension on GNOME, no system tray on
        // a minimal compositor, etc.). Leave available_ = false so the shell
        // falls back to a real quit on window close.
        return;
    }

    // libayatana-appindicator's API takes GtkMenu* — these are GTK3 widgets.
    // gtk_init() may already have been called transitively; calling it again
    // is a no-op. We don't run a GTK3 main loop — the indicator's D-Bus
    // worker runs independently of GTK's loop.
    if (!gtk_init_check(nullptr, nullptr))
    {
        return;
    }

    // Pre-render the three icon variants (normal, unread, mention) to PNGs
    // in a per-pid runtime dir and let AppIndicator pick by path. AppIndicator
    // only accepts icon names or file paths, so an in-memory composite is not
    // an option; we pay the cost once at startup and just swap path strings
    // on state change.
    const char* base_svg = resolve_icon_path();
    if (base_svg)
    {
        // $XDG_RUNTIME_DIR (or the GLib fallback) is the canonical place for
        // per-user, per-session ephemeral files like these.
        const char* runtime_root = g_get_user_runtime_dir();
        if (runtime_root)
        {
            char* dir = g_strdup_printf("%s/tesseract-%ld", runtime_root,
                                        static_cast<long>(getpid()));
            // g_mkdir_with_parents returns 0 on success (incl. already exists).
            if (g_mkdir_with_parents(dir, 0700) == 0)
            {
                runtime_dir_ = dir;
            }
            g_free(dir);
        }

        if (!runtime_dir_.empty())
        {
            constexpr int kSide = 64;
            GError* err = nullptr;
            GdkPixbuf* base = gdk_pixbuf_new_from_file_at_scale(
                base_svg, kSide, kSide, TRUE, &err);
            if (base)
            {
                const std::string normal_path  = runtime_dir_ + "/tesseract-normal.png";
                const std::string unread_path  = runtime_dir_ + "/tesseract-unread.png";
                const std::string mention_path = runtime_dir_ + "/tesseract-mention.png";

                if (render_variant_png(base, kSide, -1, normal_path))
                {
                    normal_icon_path_ = normal_path;
                }
                if (render_variant_png(base, kSide, 0x0084FF, unread_path))
                {
                    unread_icon_path_ = unread_path;
                }
                if (render_variant_png(base, kSide, 0xD93636, mention_path))
                {
                    mention_icon_path_ = mention_path;
                }
                g_object_unref(base);
            }
            else if (err)
            {
                g_clear_error(&err);
            }
        }
    }

    // Pick the initial icon path: prefer our pre-rendered "normal" PNG, then
    // the installed SVG, then fall back to icon-theme name "tesseract".
    const char* initial_icon = !normal_icon_path_.empty()
                                   ? normal_icon_path_.c_str()
                                   : (base_svg ? base_svg : "tesseract");
    AppIndicator* ind =
        app_indicator_new("io.gnomos.Tesseract", initial_icon,
                          APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    if (!ind)
    {
        return;
    }
    indicator_ = ind;

    GtkWidget* menu = gtk_menu_new();
    menu_ = menu;

    GtkWidget* show_item = gtk_menu_item_new_with_label("Show App");
    g_signal_connect(show_item, "activate", G_CALLBACK(on_show_activate),
                     static_cast<gpointer>(&on_show_));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);

    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_activate),
                     static_cast<gpointer>(&on_quit_));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    app_indicator_set_menu(ind, GTK_MENU(menu));
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(ind, "Tesseract");

    available_ = true;
}

LinuxGtkTrayIcon::~LinuxGtkTrayIcon()
{
    if (indicator_)
    {
        app_indicator_set_status(static_cast<AppIndicator*>(indicator_),
                                 APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(static_cast<AppIndicator*>(indicator_));
        indicator_ = nullptr;
    }
    if (menu_)
    {
        gtk_widget_destroy(static_cast<GtkWidget*>(menu_));
        menu_ = nullptr;
    }
    // Best-effort cleanup of the per-pid runtime dir. Leftovers on crash are
    // harmless because $XDG_RUNTIME_DIR is wiped on logout, and the next run
    // gets a fresh PID-suffixed directory anyway.
    if (!normal_icon_path_.empty())
    {
        std::remove(normal_icon_path_.c_str());
    }
    if (!unread_icon_path_.empty())
    {
        std::remove(unread_icon_path_.c_str());
    }
    if (!mention_icon_path_.empty())
    {
        std::remove(mention_icon_path_.c_str());
    }
    if (!runtime_dir_.empty())
    {
        g_rmdir(runtime_dir_.c_str());
    }
}

void LinuxGtkTrayIcon::set_tooltip(const std::string& text)
{
    if (indicator_)
    {
        app_indicator_set_title(static_cast<AppIndicator*>(indicator_),
                                text.c_str());
    }
}

void LinuxGtkTrayIcon::set_unread(bool has_unread, bool has_highlight)
{
    if (!indicator_)
    {
        return;
    }
    // Highlight wins over plain unread, mirroring the per-room color logic
    // in RoomListView. Fall back to whatever variant rendered successfully:
    // if a variant is missing (couldn't render the PNG), prefer the others
    // so we still show *some* indicator rather than silently leaving the
    // icon stale.
    const std::string* pick = nullptr;
    if (has_highlight && !mention_icon_path_.empty())
    {
        pick = &mention_icon_path_;
    }
    else if ((has_unread || has_highlight) && !unread_icon_path_.empty())
    {
        pick = &unread_icon_path_;
    }
    else if (!normal_icon_path_.empty())
    {
        pick = &normal_icon_path_;
    }
    if (!pick)
    {
        return;
    }
    app_indicator_set_icon_full(static_cast<AppIndicator*>(indicator_),
                                pick->c_str(), "Tesseract");
}
