#pragma once

// Full-window settings screen. Composes:
//   • A fixed-height top bar with a "← Back" button.
//   • A tk::SideTabView below it with sections:
//       – Account (avatar, display name, Matrix ID)
//       – Appearance (theme selection + room-list grouping)
//       – Notifications (enable/disable toggle)
//       – Media (full-media prefetch toggle)
//       – Server / Sessions / About
//
// The shell constructs SettingsView once, wires the public callbacks, and
// shows/hides it by mounting/unmounting the widget's surface. Before
// showing, call set_account_info(), set_theme_pref(),
// set_notifications_enabled(), set_image_previews_enabled(),
// set_prefetch_enabled(), set_group_inactive_pref(), and
// set_inactive_period_pref() to sync state with persisted settings.

#include "views/settings/AboutSection.h"
#include "views/settings/AccountSection.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/DevicesSection.h"
#include "views/settings/MediaSection.h"
#include "views/settings/NotificationsSection.h"
#include "views/settings/ServerSection.h"
#include "views/ConfirmDialog.h"

#include "app/SettingsController.h"

#include "tk/controls.h"
#include "tk/side_tab_view.h"
#include "tk/widget.h"

#include "tesseract/settings.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class SettingsView : public tk::Widget
{
public:
    SettingsView();
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
    void set_inactive_period_pref(int days);

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

    // ----- Server section ---------------------------------------------------

    // Populate the Server section with the connected server's info.
    void set_server_info(const tesseract::ServerInfo& info);

    // ----- Sessions section -------------------------------------------------

    // Mark which device id is the current session in the Sessions tab.
    void set_current_device_id(std::string id);

    // Wire the controller: sets controller callbacks → AccountSection state,
    // and AccountSection click callbacks → SettingsView output callbacks.
    void set_controller(tesseract::SettingsController* controller);

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

    // Returns the world-coordinate rect for the host to position the
    // name NativeTextField overlay. Empty when not editable.
    tk::Rect name_field_rect() const;

    // Fired when the user clicks the avatar disc (for shell to delegate to controller).
    std::function<void()> on_avatar_upload_requested;

    // Fired when the user clicks the X chip (for shell to delegate to controller).
    std::function<void()> on_avatar_remove_requested;

    // ----- Callbacks wired by the shell -------------------------------------

    // Fired when the user clicks "← Back".
    std::function<void()> on_close;

    // Fired after the user confirms the logout dialog.
    std::function<void()> on_logout;

    // Fired when the user selects a different theme.
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;

    // Fired when the user toggles room-list grouping of inactive rooms.
    std::function<void(bool)> on_group_inactive_changed;

    // Fired when the user changes the inactivity threshold (days).
    std::function<void(int)>  on_inactive_period_changed;

    // Fired when the user toggles notifications.
    std::function<void(bool)> on_notifications_changed;

    // Fired when the user toggles "hide message content in notifications".
    std::function<void(bool)> on_hide_content_changed;

    // Fired when the user toggles image/sticker notification previews.
    std::function<void(bool)> on_image_previews_changed;

    // Fired when the user toggles full-media pre-fetch.
    std::function<void(bool)> on_prefetch_changed;

    // Fired when the active settings tab changes (so shells can relayout
    // native overlays whose visibility depends on the selected tab).
    std::function<void()> on_tab_changed;

    // Fired after the user confirms "Clear all caches" in the About section.
    // Wire to ShellBase::clear_all_caches_() in each shell's settings setup.
    std::function<void()> on_clear_caches;

    // Update the About section's storage size labels.
    // Call on the UI thread after ShellBase::compute_cache_sizes_() posts its result.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes);

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    // Height of the back-bar strip at the top of the view.
    static constexpr float kBarHeight = 48.0f;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    tk::Button* back_btn_ = nullptr;
    tk::SideTabView* tabs_ = nullptr;
    AccountSection* account_ = nullptr;
    AppearanceSection* appearance_ = nullptr;
    NotificationsSection* notifications_ = nullptr;
    MediaSection* media_ = nullptr;
    ServerSection* server_section_ = nullptr;
    DevicesSection*  devices_        = nullptr;
    AboutSection*    about_          = nullptr;
    ConfirmDialog*   confirm_dialog_ = nullptr;

    std::function<void()> request_repaint_;
};

} // namespace tesseract::views
