#pragma once

// Settings panel section: theme selection + room-list grouping options.
// A SettingsPage with a "Theme" group (Light/Dark/System radio group) and a
// "Room list" group (checkbox + period combobox).

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tesseract/settings.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/radio.h"
#include "tk/slider.h"

#include <functional>

namespace tesseract::views
{

class FontsEditor;
class MessageLayoutPreview;
class RoomListPreview;

class AppearanceSection : public SettingsPage
{
public:
    // Every control here is bound to its Settings field through `binder`.
    explicit AppearanceSection(SettingBinder& binder);
    ~AppearanceSection() override;

private:
    // Readout text and enabled state of the timeline column controls.
    void refresh_column_controls_();

    tk::RadioGroup*  picker_             = nullptr;
    tk::ComboBox*    accent_combo_       = nullptr;
    tk::CheckButton* group_unread_cb_    = nullptr;
    tk::CheckButton* group_inactive_cb_  = nullptr;
    tk::ComboBox*    period_combo_       = nullptr;
    tk::CheckButton* autoscroll_cb_      = nullptr;
    tk::CheckButton* show_membership_events_cb_ = nullptr;
    tk::CheckButton* show_deleted_messages_cb_ = nullptr;
    tk::CheckButton* show_sender_status_cb_     = nullptr;
    tk::CheckButton* animate_avatars_cb_        = nullptr;
    tk::ComboBox*    message_layout_combo_      = nullptr;
    tk::Label*       message_layout_desc_       = nullptr;
    MessageLayoutPreview* message_layout_preview_ = nullptr;
    tk::VBox*        layout_options_bubbles_    = nullptr; // per-layout options: Bubbles
    tk::CheckButton* bubbles_own_avatar_cb_     = nullptr;
    tk::Slider*      column_width_slider_       = nullptr;
    tk::Label*       column_width_value_        = nullptr;
    tk::ComboBox*    column_anchor_combo_       = nullptr;
    tk::RadioGroup*       room_list_density_group_ = nullptr;
    RoomListPreview*      room_list_preview_       = nullptr;
    FontsEditor*          fonts_editor_            = nullptr;
};

} // namespace tesseract::views
