#pragma once

// Full-window settings screen. Composes:
//   • A fixed-height top bar with a "← Back" button.
//   • A tk::SideTabView below it with sections:
//       – Account (avatar, display name, Matrix ID)
//       – Appearance (theme selection + room-list grouping)
//       – Notifications (enable/disable toggle)
//       – Media (full-media prefetch toggle)
//       – Privacy / Server / Sessions / About
//       – Emojis & Stickers (personal pack editor + known-pack subscriptions)
//       – Language (locale selection, takes effect after restart)
//
// The shell constructs SettingsView once, wires the public callbacks, and
// shows/hides it by mounting/unmounting the widget's surface. Before
// showing, call load_persisted_settings() to sync every
// tesseract::Settings::instance() preference in one shot, plus
// set_account_info() / set_image_provider() and anything else that needs
// shell/Client state (device lists, server info, ...) — those aren't
// covered by load_persisted_settings() since the view has no access to them.

#include "views/settings/AboutSection.h"
#include "views/settings/AccountSection.h"
#include "views/settings/AdvancedSection.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/DevicesSection.h"
#include "views/settings/ImagePacksSection.h"
#include "views/settings/LanguageSection.h"
#include "views/settings/MediaSection.h"
#include "views/settings/NotificationsSection.h"
#include "views/settings/PrivacySection.h"
#include "views/settings/ServerSection.h"
#include "views/ConfirmDialog.h"

#include "app/SettingsController.h"

#include "tk/controls.h"
#include "tk/host.h"
#include "tk/side_tab_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include "tesseract/settings.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class SettingsView : public tk::Widget
{
protected:
    // host() is nullable — when null (e.g. unit tests constructing the view
    // directly), the four AccountSection editable fields are skipped. See
    // AccountSection::AccountSection().
    SettingsView();
    TK_WIDGET_FACTORY_FRIEND(SettingsView)

public:
    ~SettingsView() override = default;

    // ----- Account section --------------------------------------------------

    // Populate the Account section with the signed-in user's info.
    void set_account_info(std::string display_name, std::string user_id,
                          std::string avatar_mxc);

    // Wire up the avatar image cache from the shell.
    void set_image_provider(AccountSection::ImageProvider provider);

    // ----- Appearance section -----------------------------------------------

    // Silently initialise the theme radio buttons from persisted settings.
    void set_theme_pref(tesseract::Settings::ThemePreference pref);

    // Room list grouping (forwarded from AppearanceSection).
    void set_group_inactive_pref(bool enabled);
    void set_group_unread_pref(bool enabled);
    void set_inactive_period_pref(int days);
    void set_autoscroll_unread_pref(bool enabled);

    // Timeline: show room join/leave events (forwarded from AppearanceSection).
    void set_show_membership_events_pref(bool enabled);

    // ----- Notifications section --------------------------------------------

    // Silently initialise the notifications checkbox from persisted settings.
    void set_notifications_enabled(bool enabled);

    // Silently initialise the hide-content checkbox from persisted settings.
    void set_hide_content_enabled(bool enabled);

    // Silently initialise the image-preview checkbox from persisted settings.
    void set_image_previews_enabled(bool enabled);

    // ----- Media section ----------------------------------------------------

    // Silently initialise the prefetch checkbox from persisted settings.
    void set_prefetch_enabled(bool enabled);

    // Silently initialise the MSC4278 media-preview controls (from the
    // Settings mirror of the account-data config).
    void set_media_previews_pref(tesseract::Settings::MediaPreviews mode);
    void set_invite_avatars_pref(bool enabled);

    // Populate device combos in the Media section.
    // Call once when the settings view is first opened.
    void set_audio_input_devices(std::vector<tk::DeviceListing> devices);
    void set_audio_output_devices(std::vector<tk::DeviceListing> devices);
    void set_camera_devices(std::vector<tk::DeviceListing> devices);

    // Silently pre-select the stored device (does not fire callbacks).
    void set_selected_audio_input(const std::string& id);
    void set_selected_audio_output(const std::string& id);
    void set_selected_camera(const std::string& id);

    // Fires when the user picks a different device; empty = system default.
    std::function<void(std::string)> on_audio_input_changed;
    std::function<void(std::string)> on_audio_output_changed;
    std::function<void(std::string)> on_camera_changed;

    // ----- Privacy section --------------------------------------------------

    // Silently initialise the presence checkbox from persisted settings.
    void set_send_presence_pref(bool enabled);

    // Silently initialise the "index messages for search" checkbox.
    void set_index_messages_pref(bool enabled);

#ifdef TESSERACT_GITHUB_REPO
    // Silently initialise the "check for updates" checkbox.
    void set_check_for_updates_pref(bool enabled);
#endif

    // ----- Advanced section (hidden tab, revealed via About's "Advanced" button) --

    // Silently initialise the "Use historical MSC2545 compatibility" checkbox.
    void set_msc2545_legacy_compat_pref(bool enabled);

    // Update the search-index stats line under the checkbox (shown only while
    // enabled). Driven by the shell on settings-open and a slow poll.
    void set_search_index_stats(const tesseract::SearchIndexStats& stats,
                                bool enabled);

    // ----- Server section ---------------------------------------------------

    // Populate the Server section with the connected server's info.
    void set_server_info(const tesseract::ServerInfo& info);

    // ----- Sessions section -------------------------------------------------

    // Mark which device id is the current session in the Sessions tab.
    void set_current_device_id(std::string id);

    // ----- Emojis & Stickers section -----------------------------------------

    // Personal-pack passthrough (baseline load resets the editor's dirty bit).
    void set_user_pack_images(std::vector<tesseract::ImagePackImage> images);
    void set_user_pack_image_provider(ImagePackImageProvider p);
    void set_user_pack_tile_preview(std::uint64_t local_id,
                                    std::shared_ptr<tk::Image> image);
    void set_user_pack_saving(bool saving);
    void set_user_pack_save_result(bool ok, std::string error);

    // Fired when the user pastes/drops a new image into the personal-pack
    // editor. Wired by each shell (mirrors RoomSettingsView's
    // on_image_pack_pending_image_added) to decode the bytes off-thread and
    // push the preview back via set_user_pack_tile_preview.
    std::function<void(std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)>
        on_user_pack_pending_image_added;

    // Known-packs passthrough — every room-sourced pack the aggregator
    // knows about, with is_subscribed reflecting the account-wide
    // m.image_pack.rooms event.
    void set_known_packs(std::vector<tesseract::ImagePack> all_room_packs);

    // Exposed so each shell can pass it to
    // ShellBase::handle_user_pack_pending_image_added_ as the decode target,
    // mirroring RoomSettingsView's shape (there, the shell reaches the
    // per-room editor via room_settings_view() itself as the target).
    UserPackEditor* user_pack_editor() const;

    // Wire the controller: sets controller callbacks → AccountSection state,
    // and AccountSection click callbacks → SettingsView output callbacks.
    void set_controller(tesseract::SettingsController* controller);

    // Push every persisted preference from tesseract::Settings::instance()
    // into its section setter. Call once per settings-open. Does not touch
    // account info, server info, device lists, or anything requiring
    // shell/Client state — those remain the shell's responsibility.
    void load_persisted_settings();

    // Plug a relayout/repaint callback that the device async callbacks
    // invoke after mutating the widget tree (the alternative is requiring
    // every shell to re-wire the controller's device callbacks the same
    // way Qt6/GTK4 already do for `on_name_changed`). Each shell should
    // call this once, typically right after `set_controller`, with a
    // lambda that calls `surface_->relayout()`.
    void set_request_repaint(std::function<void()> cb);

    // State-forwarding methods (called by shells via controller callbacks):
    void set_name_busy(bool busy);
    void set_name_error(std::string error);
    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);
    void set_avatar_url(std::string mxc);
    void set_display_name_text(std::string name);

    // Test-observed geometry — see AccountSection::name_field_rect().
    tk::Rect name_field_rect() const;

    // The self-owned display-name field, or null when constructed without
    // a Host. set_controller() wires its on_submit.
    tk::TextField* name_field() const;

    // ----- Extended profile fields (MSC4133) --------------------------------

    // Push the currently-fetched extended profile into the Account section.
    void set_extended_profile(const tesseract::ExtendedProfile& profile);

    // Enable / disable all extended-profile fields.
    // Called from set_server_info() and forwarded to AccountSection.
    void set_profile_fields_editable(bool editable);

    // Mark a specific field as in-flight (busy spinner / disabled overlay).
    // key is the MSC unstable key string (e.g. "io.fsky.nyx.pronouns").
    void set_profile_field_busy(const std::string& key, bool busy);

    // Show an inline error under the given field. Pass "" to clear.
    void set_profile_field_error(const std::string& key, std::string error);

    // The three self-owned extended-profile fields, or null when
    // constructed without a Host. set_controller() wires their on_submit.
    tk::TextField* pronouns_field() const;
    tk::TextField* tz_field() const;
    tk::TextField* bio_field() const;

    // Fired when a profile field value changes (on_submit from its
    // self-owned field, wired by set_controller()). key = MSC key string,
    // value_json = JSON or "null".
    std::function<void(std::string key, std::string value_json)>
        on_profile_field_changed;

    // Fired when the user clicks the avatar disc (for shell to delegate to controller).
    std::function<void()> on_avatar_upload_requested;

    // Fired when the user clicks the X chip (for shell to delegate to controller).
    std::function<void()> on_avatar_remove_requested;

    // ----- Callbacks wired by the shell -------------------------------------

    // Fired when the user clicks "← Back".
    std::function<void()> on_close;

    // Fired after the user confirms the logout dialog.
    std::function<void()> on_logout;

    // Fired when the user selects a different theme. Named distinctly from
    // tk::Widget::on_theme_changed() (the app-theme-applied hook this class
    // also overrides below) to avoid a name collision — this one fires the
    // other way, up from the Appearance tab's picker to the shell.
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_preference_changed;

    // Fired when the user toggles room-list grouping of inactive rooms.
    std::function<void(bool)> on_group_inactive_changed;

    // Fired when the user toggles room-list grouping of unread rooms.
    std::function<void(bool)> on_group_unread_changed;

    // Fired when the user changes the inactivity threshold (days).
    std::function<void(int)>  on_inactive_period_changed;

    // Fired when the user toggles auto-scroll-to-unread in the room list.
    std::function<void(bool)> on_autoscroll_unread_changed;

    // Fired when the user toggles "show room join/leave events" in Timeline.
    std::function<void(bool)> on_show_membership_events_changed;

    // Fired when the user toggles notifications.
    std::function<void(bool)> on_notifications_changed;

    // Fired when the user toggles "hide message content in notifications".
    std::function<void(bool)> on_hide_content_changed;

    // Fired when the user toggles image/sticker notification previews.
    std::function<void(bool)> on_image_previews_changed;

    // Fired when the user toggles full-media pre-fetch.
    std::function<void(bool)> on_prefetch_changed;

    // Fired when the user changes the MSC4278 media-preview mode.
    std::function<void(tesseract::Settings::MediaPreviews)>
        on_media_previews_changed;

    // Fired when the user toggles the MSC4278 "Show avatars in invites" option.
    std::function<void(bool)> on_invite_avatars_changed;

    // Fired when the user toggles the "Send and receive presence status" option.
    std::function<void(bool)> on_send_presence_changed;

    // Fired when the user toggles "Index messages for search". The shell
    // persists the setting and calls Client::set_search_indexing_enabled().
    std::function<void(bool)> on_index_messages_changed;

#ifdef TESSERACT_GITHUB_REPO
    // Fired when the user toggles "Check for updates automatically".
    std::function<void(bool)> on_check_for_updates_changed;
#endif

    // Fired when the user toggles "Use historical MSC2545 compatibility".
    std::function<void(bool)> on_msc2545_legacy_compat_changed;

    // Fired when the active settings tab changes (so shells can relayout
    // native overlays whose visibility depends on the selected tab).
    std::function<void()> on_tab_changed;

    // Fired after the user confirms "Clear all caches" in the About section.
    // Wire to ShellBase::clear_all_caches_() in each shell's settings setup.
    std::function<void()> on_clear_caches;

    // Fired after the user confirms "Reset cryptographic identity" in the
    // Privacy section. Wire to ShellBase::begin_crypto_identity_reset_() in
    // each shell (closing the settings UI first — the reset overlay lives on
    // the main window).
    std::function<void()> on_reset_identity;

    // Update the About section's storage size labels and hit/miss stats.
    // Call on the UI thread after ShellBase::compute_cache_sizes_() posts its result.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                         uint64_t memory_bytes,
                         uint64_t mem_hits   = 0, uint64_t mem_misses  = 0,
                         uint64_t disk_hits  = 0, uint64_t disk_misses = 0);

    // Fired when the user selects a different language (BCP47 code or "auto").
    std::function<void(std::string)> on_language_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    // Height of the back-bar strip at the top of the view.
    static constexpr float kBarHeight = 48.0f;

    // Index of the hidden "Advanced" bottom tab in tabs_ — computed from the
    // final tab registration order in the constructor (9 top tabs, indices
    // 0-8; bottom tabs About=9, Advanced=10). Kept hidden via
    // tabs_->set_tab_visible(kAdvancedTabIdx, false) until the About tab's
    // "Advanced" button reveals it.
    static constexpr int kAdvancedTabIdx = 10;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    tk::Button* back_btn_ = nullptr;
    tk::SideTabView* tabs_ = nullptr;
    AccountSection* account_ = nullptr;
    AppearanceSection* appearance_ = nullptr;
    NotificationsSection* notifications_ = nullptr;
    MediaSection*    media_          = nullptr;
    PrivacySection*  privacy_        = nullptr;
    ServerSection*   server_section_ = nullptr;
    DevicesSection*  devices_        = nullptr;
    AboutSection*    about_          = nullptr;
    AdvancedSection* advanced_       = nullptr;
    ConfirmDialog*   confirm_dialog_ = nullptr;
    LanguageSection* language_       = nullptr;
    ImagePacksSection* image_packs_  = nullptr;

    bool modal_was_open_ = false;

    std::function<void()> request_repaint_;
};

} // namespace tesseract::views
