#pragma once
#include <gtk/gtk.h>
#include <tesseract/settings.h>
#include <functional>
#include <memory>
#include <string>
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

    // Callbacks — set by MainWindow before use.
    std::function<void()> on_close;
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;
    std::function<void(bool)> on_notifications_changed;

private:
    std::unique_ptr<tk::gtk4::Surface> surface_;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
};

} // namespace gtk4
