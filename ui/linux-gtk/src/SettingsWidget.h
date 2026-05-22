#pragma once
#include <gtk/gtk.h>
#include <tesseract/settings.h>
#include <functional>
#include <memory>
#include <string>
#include "app/SettingsController.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/settings/AccountSection.h"
#include "views/SettingsView.h"

namespace gtk4
{

class SettingsWidget
{
public:
    SettingsWidget();
    ~SettingsWidget() = default;

    SettingsWidget(const SettingsWidget&) = delete;
    SettingsWidget& operator=(const SettingsWidget&) = delete;

    // Returns the GtkWidget* to add to GtkStack (the surface widget).
    GtkWidget* widget() const;

    // Apply a new theme to the surface.
    void set_theme(const tk::Theme& t);

    // Push current account info and settings into the view before showing.
    void populate(std::string display_name, std::string user_id,
                  std::string avatar_mxc,
                  tesseract::views::AccountSection::ImageProvider provider,
                  tesseract::Settings::ThemePreference theme_pref,
                  bool notifications_enabled);

    // Forward server capability info into the shared SettingsView.
    void set_server_info(const tesseract::ServerInfo& info);

    // Wire the SettingsController and create the NativeTextField for name editing.
    void set_controller(tesseract::SettingsController* ctrl,
                        const std::string& current_display_name);

    // Callbacks — set by MainWindow before use.
    std::function<void()> on_close;
    std::function<void()> on_logout;
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;
    std::function<void(bool)> on_notifications_changed;
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(int)>  on_inactive_period_changed;

    void set_group_inactive_pref(bool enabled);
    void set_inactive_period_pref(int days);

private:
    std::unique_ptr<tk::gtk4::Surface> surface_;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    tesseract::SettingsController* controller_ = nullptr;
    std::unique_ptr<tk::NativeTextField> name_field_;
};

} // namespace gtk4
