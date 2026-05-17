#pragma once
#include <QWidget>

#include <tesseract/settings.h>

#include <string>

#include "tk/host_qt.h"
#include "views/settings/AccountSection.h"
#include "views/SettingsView.h"

namespace qt6 {

/// Full-window settings screen shown inside the main-window content stack.
/// Hosts the shared `tesseract::views::SettingsView` inside a
/// `tk::qt6::Surface` child.  Follows the same wrapper pattern as LoginView.
class SettingsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWidget(QWidget* parent = nullptr);

    /// Push current account info and settings into the shared view before
    /// making this widget the visible content-stack page.
    /// Apply a new theme to the surface (called from MainWindow::apply_theme_ui_).
    void set_theme(const tk::Theme& t);

    void populate(std::string display_name,
                  std::string user_id,
                  std::string avatar_mxc,
                  tesseract::views::AccountSection::ImageProvider provider,
                  tesseract::Settings::ThemePreference theme_pref,
                  bool notifications_enabled);

signals:
    void settingsClosed();
    void themeChanged(tesseract::Settings::ThemePreference pref);
    void notificationsChanged(bool enabled);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    tk::qt6::Surface*                surface_       = nullptr;
    tesseract::views::SettingsView*  settings_view_ = nullptr;  // borrowed
};

} // namespace qt6
