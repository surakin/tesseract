#pragma once
#include <QWidget>
#include <cstdint>

#include <tesseract/settings.h>

#include <string>

#include "app/SettingsController.h"
#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/settings/AccountSection.h"
#include "views/SettingsView.h"

namespace qt6
{

/// Full-window settings screen shown inside the main-window content stack.
/// Hosts the shared `tesseract::views::SettingsView` inside a
/// `tk::qt6::Surface` child.  Follows the same wrapper pattern as LoginView.
class SettingsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWidget(QWidget* parent = nullptr);

    /// Apply a new theme to the surface (called from MainWindow::apply_theme_ui_).
    void set_theme(const tk::Theme& t);

    /// Push current account info and settings into the shared view before
    /// making this widget the visible content-stack page.
    void populate(std::string display_name, std::string user_id,
                  std::string avatar_mxc,
                  tesseract::views::AccountSection::ImageProvider provider,
                  tesseract::Settings::ThemePreference theme_pref,
                  bool notifications_enabled);

    /// Forward server capability info into the shared SettingsView.
    void set_server_info(const tesseract::ServerInfo& info);

    /// Update the Storage size labels in the About section.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                         uint64_t memory_bytes);

    void set_controller(tesseract::SettingsController* ctrl,
                        const std::string& current_display_name);

signals:
    void settingsClosed();
    void logoutRequested();
    void themeChanged(tesseract::Settings::ThemePreference pref);
    void notificationsChanged(bool enabled);
    void presenceChanged(bool enabled);
    void mediaPreviewsChanged(tesseract::Settings::MediaPreviews mode);
    void inviteAvatarsChanged(bool enabled);
    void roomListGroupingChanged();
    void clearCachesRequested();
    // Fired after the user changes their own avatar via Settings. The
    // string is the new mxc URL (or empty for removal). MainWindow uses
    // this to update ShellBase::my_avatar_url_ and repaint the sidebar
    // UserInfo strip — the shared SettingsView only updates its own
    // AccountSection chip.
    void localAvatarChanged(QString new_mxc);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    tk::qt6::Surface* surface_ = nullptr;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    tesseract::SettingsController* controller_ = nullptr;
    std::unique_ptr<tk::NativeTextField> name_field_;
};

} // namespace qt6
