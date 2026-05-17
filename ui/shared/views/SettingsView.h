#pragma once

// Full-window settings screen. Composes:
//   • A fixed-height top bar with a "← Back" button.
//   • A tk::SideTabView below it with three sections:
//       – Account (avatar, display name, Matrix ID)
//       – Appearance (theme selection)
//       – Notifications (enable/disable toggle)
//
// The shell constructs SettingsView once, wires the public callbacks, and
// shows/hides it by mounting/unmounting the widget's surface. Before
// showing, call set_account_info(), set_notifications_enabled(), and
// set_theme_pref() to sync state with persisted settings.

#include "views/settings/AccountSection.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/NotificationsSection.h"

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
    void set_account_info(std::string display_name,
                          std::string user_id,
                          std::string avatar_mxc);

    // Wire up the avatar image cache from the shell.
    void set_image_provider(AccountSection::ImageProvider provider);

    // ----- Appearance section -----------------------------------------------

    // Silently initialise the theme radio buttons from persisted settings.
    void set_theme_pref(tesseract::Settings::ThemePreference pref);

    // ----- Notifications section --------------------------------------------

    // Silently initialise the notifications checkbox from persisted settings.
    void set_notifications_enabled(bool enabled);

    // ----- Callbacks wired by the shell -------------------------------------

    // Fired when the user clicks "← Back".
    std::function<void()>                                    on_close;

    // Fired when the user selects a different theme.
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;

    // Fired when the user toggles notifications.
    std::function<void(bool)>                                on_notifications_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    // Height of the back-bar strip at the top of the view.
    static constexpr float kBarHeight = 48.0f;

    // Child widgets — owned via add_child, raw pointers borrowed back.
    tk::Button*          back_btn_    = nullptr;
    tk::SideTabView*     tabs_        = nullptr;
    AccountSection*      account_     = nullptr;
    AppearanceSection*   appearance_  = nullptr;
    NotificationsSection* notifications_ = nullptr;
};

} // namespace tesseract::views
