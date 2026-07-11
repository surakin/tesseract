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

    /// The hosted shared SettingsView (borrowed). Used by the shell to push
    /// search-index stats.
    tesseract::views::SettingsView* settings_view() const { return settings_view_; }

    /// Forward server capability info into the shared SettingsView.
    void set_server_info(const tesseract::ServerInfo& info);

    /// Push the fetched extended profile into the Account section.
    void set_extended_profile(const tesseract::ExtendedProfile& profile);

    /// Mark a profile field as busy (in-flight write) or idle.
    void set_profile_field_busy(const std::string& key, bool busy);

    /// Show an inline error for the given profile field. Pass "" to clear.
    void set_profile_field_error(const std::string& key, std::string error);

    /// Update the Storage size labels and hit/miss stats in the About section.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                         uint64_t memory_bytes,
                         uint64_t mem_hits   = 0, uint64_t mem_misses  = 0,
                         uint64_t disk_hits  = 0, uint64_t disk_misses = 0);

    void set_controller(tesseract::SettingsController* ctrl,
                        const std::string& current_display_name);

    /// Silently initialise the "show room join/leave events" checkbox.
    void set_show_membership_events_pref(bool enabled);

    /// Repaint just the regions covering animated stickers (Emojis &
    /// Stickers tab). Called from MainWindow::repaint_anim_frame_ on every
    /// ~60Hz animation tick while this widget is visible — mirrors
    /// mainAppSurface_->update_anim_regions(), since this widget hosts its
    /// own separate tk::qt6::Surface the shell's generic animation-tick
    /// invalidation doesn't otherwise know about.
    void update_anim_regions();

signals:
    void settingsClosed();
    void logoutRequested();
    void themeChanged(tesseract::Settings::ThemePreference pref);
    void notificationsChanged(bool enabled);
    void presenceChanged(bool enabled);
    void indexMessagesChanged(bool enabled);
#ifdef TESSERACT_GITHUB_REPO
    void checkForUpdatesChanged(bool enabled);
#endif
    void mediaPreviewsChanged(tesseract::Settings::MediaPreviews mode);
    void inviteAvatarsChanged(bool enabled);
    void roomListGroupingChanged();
    // Fired after the "show room join/leave events" preference is persisted.
    // MainWindow applies it to the Rust client and re-subscribes the active
    // room so the change takes effect immediately.
    void membershipEventsPrefChanged(bool enabled);
    void clearCachesRequested();
    void resetIdentityRequested();
    // Fired after the user changes their own avatar via Settings. The
    // string is the new mxc URL (or empty for removal). MainWindow uses
    // this to update ShellBase::my_avatar_url_ and repaint the sidebar
    // UserInfo strip — the shared SettingsView only updates its own
    // AccountSection chip.
    void localAvatarChanged(QString new_mxc);
    // Fired when the user submits an extended profile field (MSC4133).
    // key = MSC unstable key string, value_json = JSON value or "null".
    void profileFieldChanged(QString key, QString value_json);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    tk::qt6::Surface* surface_ = nullptr;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    tesseract::SettingsController* controller_ = nullptr;
    std::unique_ptr<tk::NativeTextField> name_field_;
    // Extended-profile NativeTextField overlays (MSC4133).
    std::unique_ptr<tk::NativeTextField> pronouns_field_;
    std::unique_ptr<tk::NativeTextField> tz_field_;
    std::unique_ptr<tk::NativeTextField> bio_field_;
};

} // namespace qt6
