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
// all" — so all 9 rows share one all-or-nothing set_field_permissions gate.

#include "SettingsPage.h"

#include "tk/combobox.h"
#include "tk/controls.h"

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

    // Shows/hides the page-level "would lock you out" warning — driven by
    // RoomSettingsView, since evaluating it needs the current user's own
    // power level (RoomOwnPowerLevel), which this widget has no access to.
    // A page-level banner rather than a per-row one because the condition
    // can be caused by either the Default Role or Change Permissions row.
    void set_would_lock_out_self(bool would_lock_out);

    // Fired on user interaction with the full updated struct (one callback
    // for all 9 rows, since they're homogeneous same-shaped ints, unlike
    // Security's four semantically-different fields).
    std::function<void(tesseract::RoomPermissions)> on_permissions_changed;

    // Fired whenever the lockout warning's visibility changes at runtime —
    // FlexBox::arrange() skips invisible children entirely, so a widget
    // that just became visible has stale bounds_ until the next full
    // arrange() pass. Mirrors RoomSecuritySection::on_layout_changed.
    std::function<void()> on_layout_changed;

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

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

private:
    void refresh_enabled_();

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

    tesseract::RoomPermissions current_;
    bool can_edit_   = false;
    bool committing_ = false;
};

} // namespace tesseract::views
