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

#include "EmojiPicker.h"

#include "views/settings/AboutSection.h"
#include "views/settings/AccountSection.h"
#include "views/settings/AdvancedSection.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/DevicesSection.h"
#include "views/settings/GeneralSection.h"
#include "views/settings/ImagePacksSection.h"
#include "views/settings/LanguageSection.h"
#include "views/settings/SettingBinder.h"
#include "views/settings/NetworkSection.h"
#include "views/settings/MediaSection.h"
#include "views/settings/NotificationsSection.h"
#include "views/settings/PrivacySection.h"
#include "views/settings/ServerSection.h"
#include "views/ConfirmDialog.h"

#include "views/settings/SettingsController.h"

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
    // Unwires every callback set_controller() installed on ctrl_ (if any),
    // so a SettingsController async operation still in flight when this
    // view is destroyed (e.g. loading image packs, saving an avatar) can't
    // call back into now-freed AccountSection/ImagePacksSection/etc. state.
    // SettingsController outlives this view — it's bound once at login and
    // survives Settings being closed and reopened, even destroyed and
    // lazily rebuilt (see ui/shared/app/DeferredTeardown.h) — but its
    // pending network operations don't get cancelled just because nobody's
    // listening anymore, so this view has to make sure "nobody's listening"
    // actually means nobody's listening.
    ~SettingsView() override;

    // ----- General section ---------------------------------------------------

    // Silently initialise the "launch at login" checkbox. Sourced from
    // IAutostart::is_enabled() (actual OS state), not the persisted
    // Settings bool directly. Also refreshes the persisted bool, which is
    // only a cache of the OS registration (see ShellBase::handle_setting_changed_).
    void set_launch_at_login_pref(bool enabled);

    // Report whether a tray icon exists so the close-action dropdown can drop
    // the "hide to tray" option, which would be unreachable without one. The
    // shell also resolves a stored HideToTray to Quit in that case.
    void set_tray_available(bool available);

    // Show / hide the Low Power Mode setting. The shell calls this once with
    // IPowerMonitor::has_battery() — hidden on a batteryless desktop.
    void set_low_power_available(bool available);

    // ----- Account section --------------------------------------------------

    // Populate the Account section with the signed-in user's info.
    void set_account_info(std::string display_name, std::string user_id,
                          std::string avatar_mxc);

    // Wire up the avatar image cache from the shell.
    void set_image_provider(AccountSection::ImageProvider provider);

    // ----- Appearance section -----------------------------------------------

    // ----- Notifications section --------------------------------------------

    // ----- Mentions & Keywords (push-rule-backed, account-wide) -------------

    void set_mentions_enabled_ui(bool enabled);
    void set_room_mentions_enabled_ui(bool enabled);
    void set_notify_all_messages_ui(bool enabled);
    void set_notification_keywords_ui(std::vector<std::string> keywords);
    void set_mentions_loading_ui(bool loading);
    void set_keyword_error_ui(std::string error);

    // ----- Media section ----------------------------------------------------

    // Populate device combos in the Media section.
    // Call once when the settings view is first opened.
    void set_audio_input_devices(std::vector<tk::DeviceListing> devices);
    void set_audio_output_devices(std::vector<tk::DeviceListing> devices);
    void set_camera_devices(std::vector<tk::DeviceListing> devices);

    // Silently pre-select a device (the stored ids are bound to Settings and
    // re-loaded by load_persisted_settings(); this is for re-selecting after
    // the option list was rebuilt).
    void set_selected_audio_input(const std::string& id);
    void set_selected_audio_output(const std::string& id);
    void set_selected_camera(const std::string& id);

    // Enumerate the audio and camera devices through this view's host and
    // select the ones stored in Settings. Call once after creating the view.
    void load_device_lists();

    // ----- Privacy section --------------------------------------------------

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

    // Relayout the surface hosting this view (through its tk::Host, so no
    // shell needs to know which native surface that is).
    void request_relayout();

    // State-forwarding methods (called by shells via controller callbacks):
    void set_name_busy(bool busy);
    void set_name_error(std::string error);
    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);
    void set_avatar_url(std::string mxc);
    void set_avatar_preview(std::shared_ptr<tk::Image> image);
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

    // The self-owned extended-profile fields, or null when constructed
    // without a Host. set_controller() wires their on_submit/on_changed.
    // pronouns_editor() is a repeatable-row widget (see PronounsEditor), not
    // a single TextField, since MSC4247 pronouns can have several language
    // entries.
    PronounsEditor* pronouns_editor() const;
    TimezonePicker* tz_field() const;
    tk::TextField* bio_field() const;
    StatusEditor* status_editor() const; // MSC4426 own-status editor

    // Select the Account tab (index 0). Used by the shells to deep-link into
    // settings from the sidebar status line. Fires on_tab_changed if the
    // selection actually changes.
    void show_account_section();

    // Appearance is the fourth tab added (see the constructor's add_tab order).
    static constexpr int kAppearanceTabIdx = 3;
    // Select the Appearance tab. Used by screenshot mode's settings scene.
    void show_appearance_section();
    std::string selected_tab_label() const;

    // Settings reached from the login screen, before any account exists:
    // only the account-independent tabs (Network, Language, About) stay
    // visible, Network is selected, and About's account actions are hidden.
    // Passing false restores the full set (Advanced stays hidden).
    void set_signed_out_mode(bool on);
    bool tab_visible(int idx) const;

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

    // Fired after the user changed any bound preference. The value is already
    // in tesseract::Settings::instance(); the receiver saves it and applies
    // whatever the key needs (see ShellBase::handle_setting_changed_).
    std::function<void(SettingKey)> on_setting_changed;

    // The account's default emoji skin tone, read each time the status-emoji
    // picker opens; on_emoji_skin_tone_changed fires when the user picks a
    // new one from that picker's tone menu.
    std::function<tesseract::emoji::SkinTone()> emoji_skin_tone_provider;
    std::function<void(tesseract::emoji::SkinTone)> on_emoji_skin_tone_changed;

    // Show / hide the network section's "Restart now" row.
    void set_proxy_restart_pending(bool pending);

    // Fired when the user clicks "Open Activity Monitor" (Advanced tab).
    std::function<void()> on_open_activity_monitor;

    // Fired when the active settings tab changes (so shells can relayout
    // native overlays whose visibility depends on the selected tab).
    std::function<void()> on_tab_changed;

    // Fired when the About tab becomes the selected tab. ShellBase wires it
    // to kick the live cache-size refresh (see refresh_cache_sizes_poll_()).
    std::function<void()> on_about_tab_shown;

    // True while the About tab is the selected tab.
    bool about_tab_selected() const;

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

    // Fired by the language section's "Restart now" button.
    std::function<void()> on_restart_requested;

    // Show / hide the language section's "Restart now" button.
    void set_language_restart_pending(bool pending);

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    void paint_after_children(tk::PaintCtx&) override;
    void paint_overlay(tk::PaintCtx&) override;
    void access_detached_children(std::vector<tk::Widget*>& out) const override
    {
        if (status_emoji_picker_visible_ && status_emoji_picker_)
            out.push_back(status_emoji_picker_.get());
    }
    void on_theme_changed(const tk::Theme&) override;

private:
    // Height of the back-bar strip at the top of the view.
    static constexpr float kBarHeight = 48.0f;

    // Set by set_controller(); read by ~SettingsView() to unwire the
    // callbacks it installed. Non-owning — see set_controller()'s doc
    // comment for the lifetime relationship.
    tesseract::SettingsController* ctrl_ = nullptr;

    // Bound preference controls: every section registers its Settings-backed
    // controls here; changes arrive as on_changed(key). Declared before the
    // sections' raw pointers and torn down with the view.
    SettingBinder binder_;
    // View-local reactions to a preference change (restart rows, repaint, ...).
    void react_locally_(SettingKey key);

    // MSC4426 status-emoji picker. Not a tree child (Host popup, like
    // RoomView's) — see show_status_emoji_picker_().
    void show_status_emoji_picker_(tk::Rect world_anchor);
    void hide_status_emoji_picker_();

    // Index of the hidden "Advanced" bottom tab in tabs_ — computed from the
    // final tab registration order in the constructor (11 top tabs, indices
    // 0-10; bottom tabs About=11, Advanced=12). Kept hidden via
    // tabs_->set_tab_visible(kAdvancedTabIdx, false) until the About tab's
    // "Advanced" button reveals it.
    static constexpr int kAdvancedTabIdx = 12;
    static constexpr int kAboutTabIdx = 11;
    // Account is the first tab added (see the constructor) and hence index 0.
    static constexpr int kAccountTabIdx = 0;
    static constexpr int kNetworkTabIdx = 7;
    static constexpr int kLanguageTabIdx = 10;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    tk::Button* back_btn_ = nullptr;
    tk::SideTabView* tabs_ = nullptr;
    AccountSection* account_ = nullptr;
    GeneralSection* general_ = nullptr;
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
    NetworkSection*  network_        = nullptr;
    ImagePacksSection* image_packs_  = nullptr;

    std::unique_ptr<EmojiPicker> status_emoji_picker_;
    bool status_emoji_picker_visible_ = false;

    bool modal_was_open_ = false;
};

} // namespace tesseract::views
