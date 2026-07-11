#pragma once
#include <gtk/gtk.h>
#include <tesseract/settings.h>
#include <cstdint>
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

    // The hosted shared SettingsView (borrowed). Used by the shell to push
    // search-index stats.
    tesseract::views::SettingsView* settings_view() const { return settings_view_; }

    // Forward server capability info into the shared SettingsView.
    void set_server_info(const tesseract::ServerInfo& info);

    // Push the fetched extended profile into the Account section.
    void set_extended_profile(const tesseract::ExtendedProfile& profile);

    // Mark a profile field as busy (in-flight write) or idle.
    void set_profile_field_busy(const std::string& key, bool busy);

    // Show an inline error for the given profile field. Pass "" to clear.
    void set_profile_field_error(const std::string& key, std::string error);

    // Callback — set by MainWindow; fires when the user edits a field.
    // key = MSC key string, value_json = serialised JSON or "null".
    std::function<void(std::string key, std::string value_json)>
        on_profile_field_changed;

    // Update the Storage size labels and hit/miss stats in the About section.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                         uint64_t memory_bytes,
                         uint64_t mem_hits   = 0, uint64_t mem_misses  = 0,
                         uint64_t disk_hits  = 0, uint64_t disk_misses = 0);

    // Wire the SettingsController and create the NativeTextField for name editing.
    void set_controller(tesseract::SettingsController* ctrl,
                        const std::string& current_display_name);

    // Callbacks — set by MainWindow before use.
    std::function<void()> on_close;
    std::function<void()> on_logout;
    std::function<void()> on_clear_caches;
    std::function<void()> on_reset_identity;
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;
    std::function<void(bool)> on_notifications_changed;
    std::function<void(bool)> on_send_presence_changed;
    std::function<void(bool)> on_index_messages_changed;
#ifdef TESSERACT_GITHUB_REPO
    std::function<void(bool)> on_check_for_updates_changed;
#endif
    std::function<void(tesseract::Settings::MediaPreviews)>
        on_media_previews_changed;
    std::function<void(bool)> on_invite_avatars_changed;
    std::function<void(bool)> on_group_inactive_changed;
    std::function<void(bool)> on_group_unread_changed;
    std::function<void(int)>  on_inactive_period_changed;
    std::function<void(bool)> on_autoscroll_unread_changed;
    std::function<void(bool)> on_show_membership_events_changed;
    // Fired after the user changes their own avatar via Settings. The
    // string is the new mxc URL (or empty for removal). MainWindow uses
    // this to update ShellBase::my_avatar_url_ and repaint the sidebar
    // UserInfo strip — the shared SettingsView only updates its own
    // AccountSection chip.
    std::function<void(std::string)> on_local_avatar_changed;

    void set_group_inactive_pref(bool enabled);
    void set_group_unread_pref(bool enabled);
    void set_inactive_period_pref(int days);
    void set_autoscroll_unread_pref(bool enabled);
    void set_show_membership_events_pref(bool enabled);

    // Repaint the surface on every ~60Hz animation tick while this widget is
    // visible, so animated stickers in the Emojis & Stickers tab advance
    // without needing mouse movement — mirrors
    // sticker_picker_surface_->relayout()'s brute-force-repaint idiom (GTK4's
    // partial-region AnimDamageSink path isn't wired up for this view; see
    // ImagePackTileGridBase, which doesn't call ctx.anim_damage->note_image()).
    void update_anim_regions();

private:
    std::unique_ptr<tk::gtk4::Surface> surface_;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    tesseract::SettingsController* controller_ = nullptr;
    std::unique_ptr<tk::NativeTextField> name_field_;
    // Extended-profile NativeTextField overlays (MSC4133).
    std::unique_ptr<tk::NativeTextField> pronouns_field_;
    std::unique_ptr<tk::NativeTextField> tz_field_;
    std::unique_ptr<tk::NativeTextField> bio_field_;
    GtkWidget* cache_tooltip_popover_ = nullptr; // lazy-created GtkPopover
    GtkWidget* cache_tooltip_label_   = nullptr; // borrowed child of popover
};

} // namespace gtk4
