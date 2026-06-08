#pragma once
#include <tesseract/tray_icon.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Pure-D-Bus StatusNotifierItem tray for the GTK4 shell.
//
// The previous implementation (LinuxGtkTrayIcon) used libayatana-appindicator3,
// a GTK3 library; linking it pulled libgtk-3 into the GTK4 process, which makes
// gtk_init() abort ("GTK 2/3 symbols detected ... not supported"). This version
// speaks the org.kde.StatusNotifierItem + com.canonical.dbusmenu protocols
// directly over GDBus and renders its icon with gdk-pixbuf + cairo — none of
// which pull GTK into the process. All GDBus/GVariant state is hidden behind an
// Impl so this header stays free of gio/cairo includes.
class GtkSniTrayIcon final : public tesseract::ITrayIcon
{
public:
    GtkSniTrayIcon(std::function<void()> on_show, std::function<void()> on_quit);
    ~GtkSniTrayIcon() override;

    bool is_available() const override
    {
        return available_;
    }
    void set_tooltip(const std::string& text) override;
    void set_unread(bool has_unread, bool has_highlight) override;

    // Rebuild the context menu. `window_items` is a list of (label, callback)
    // pairs — one per open main window — prepended before the Quit action.
    // Emits LayoutUpdated so the host re-queries. Call from the UI thread only.
    void rebuild_menu(
        std::vector<std::pair<std::string, std::function<void()>>> window_items);

    // Opaque; defined in the .cpp. Public so the file-local GDBus vtable
    // callbacks (which receive it as user_data) can name the type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};
