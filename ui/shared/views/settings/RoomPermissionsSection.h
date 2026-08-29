#pragma once

// The "Permissions" tab of RoomSettingsView: the aggregate "who can do X"
// rules from the room's m.room.power_levels event (default role for new
// members, invite/kick/ban, message/settings/permissions defaults, and
// @room notifications) — mirrors Element's "Roles & Permissions" tab.
// Deliberately does NOT expose a per-member power-level override editor;
// that would need a searchable member list and is a structurally different
// feature.
//
// Every row is edited via a Default/Moderator/Admin tk::ComboBox. When the
// room's actual value doesn't match one of those three presets, a 4th
// "Custom (N)" option is synthesized and selected — reuses the exact
// tk::ComboBox + stoi-on-change pattern AppearanceSection's inactive-period
// setting already uses, and the same "extra option for a value the picker
// doesn't normally offer" idea RoomSecuritySection's join-rule combo
// already uses for Restricted/Knock-Restricted.
//
// Unlike RoomSecuritySection's four independently-gated fields, Matrix has
// no finer granularity than "can this user send m.room.power_levels at
// all" — so all 10 rows share one all-or-nothing set_field_permissions
// gate.
//
// The "Calls" group is additionally hidden outright (independent of the
// set_field_permissions gate) via set_calls_supported() when the server
// has no LiveKit/MatrixRTC backend at all — mirrors how the call button
// itself is hidden behind ShellBase's server_info_.supports_calls, so
// admins aren't offered a permission for a feature that doesn't work on
// their server.

#include "SettingsPage.h"

#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/form_layout.h"

#include <tesseract/types.h>

#include <functional>

namespace tesseract::views
{

class RoomPermissionsSection : public SettingsPage
{
public:
    RoomPermissionsSection();
    ~RoomPermissionsSection() override = default;

    // Silent seeding (no callback fired) — called by RoomSettingsView::open()
    // and set_permissions_state().
    void set_permissions(const tesseract::RoomPermissions& p);

    // Single all-or-nothing gate — Matrix has no per-action granularity for
    // who can edit permissions, unlike Security & Privacy's four
    // independent fields.
    void set_field_permissions(bool can_edit);
    void set_committing(bool committing);

    // Hides the whole "Calls" group when the server has no call backend
    // (see class comment above). Independent of set_field_permissions —
    // even an admin who CAN edit permissions has nothing to edit here if
    // calls don't work on this server at all.
    void set_calls_supported(bool supported);

    // Shows/hides the page-level "would lock you out" warning — driven by
    // RoomSettingsView, since evaluating it needs the current user's own
    // power level (RoomOwnPowerLevel), which this widget has no access to.
    // A page-level banner rather than a per-row one because the condition
    // can be caused by either the Default Role or Change Permissions row.
    void set_would_lock_out_self(bool would_lock_out);

    // Fired on user interaction with the full updated struct (one callback
    // for all 10 rows, since they're homogeneous same-shaped ints, unlike
    // Security's four semantically-different fields).
    std::function<void(tesseract::RoomPermissions)> on_permissions_changed;

    // Fired whenever the lockout warning's visibility changes at runtime —
    // FlexBox::arrange() skips invisible children entirely, so a widget
    // that just became visible has stale bounds_ until the next full
    // arrange() pass. Mirrors RoomSecuritySection::on_layout_changed.
    std::function<void()> on_layout_changed;

    // Accessor used by tests to inspect the warning's visibility (mirrors
    // RoomSecuritySection::encryption_warning()).
    tk::Widget* lockout_warning() const { return lockout_warning_; }

    // Accessors used by tests to simulate user interaction and inspect
    // state (mirrors RoomSecuritySection's accessors).
    tk::ComboBox* default_role_combo() const { return default_role_combo_; }
    tk::ComboBox* send_messages_combo() const { return send_messages_combo_; }
    tk::ComboBox* remove_messages_combo() const { return remove_messages_combo_; }
    tk::ComboBox* invite_users_combo() const { return invite_users_combo_; }
    tk::ComboBox* kick_users_combo() const { return kick_users_combo_; }
    tk::ComboBox* ban_users_combo() const { return ban_users_combo_; }
    tk::ComboBox* change_settings_combo() const { return change_settings_combo_; }
    tk::ComboBox* change_permissions_combo() const { return change_permissions_combo_; }
    tk::ComboBox* notify_everyone_combo() const { return notify_everyone_combo_; }
    tk::ComboBox* start_calls_combo() const { return start_calls_combo_; }

    // Accessor used by tests to inspect the Calls group's visibility.
    tk::Widget* calls_group() const;

private:
    void refresh_enabled_();

    // Shared across the four per-group FormLayouts (Default Role, Messages,
    // Membership, Advanced) so their combo boxes all start at the same
    // horizontal offset instead of each group sizing its label column to
    // only its own rows.
    tk::FormLayoutGroup label_group_;

    tk::Label*    lockout_warning_         = nullptr;
    tk::ComboBox* default_role_combo_       = nullptr;
    tk::ComboBox* send_messages_combo_      = nullptr;
    tk::ComboBox* remove_messages_combo_    = nullptr;
    tk::ComboBox* invite_users_combo_       = nullptr;
    tk::ComboBox* kick_users_combo_         = nullptr;
    tk::ComboBox* ban_users_combo_          = nullptr;
    tk::ComboBox* change_settings_combo_    = nullptr;
    tk::ComboBox* change_permissions_combo_ = nullptr;
    tk::ComboBox* notify_everyone_combo_    = nullptr;
    tk::ComboBox* start_calls_combo_        = nullptr;

    // Borrowed — owned via SettingsPage::content_'s add_child(). Kept (unlike
    // the other groups, which are only local variables in the constructor)
    // so set_calls_supported() can toggle its visibility later.
    SettingsGroup* calls_group_ = nullptr;

    tesseract::RoomPermissions current_;
    bool can_edit_   = false;
    bool committing_ = false;
};

} // namespace tesseract::views
