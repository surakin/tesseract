#pragma once

// The "Security & Privacy" tab of RoomSettingsView: encryption, join rule,
// guest access, and history visibility. Unlike Media (personal account
// data, no permission gating), these fields are room STATE EVENTS and
// participate in RoomSettingsView's staged Accept/Cancel commit flow with
// per-field power-level gating — mirrors RoomGeneralSection's
// set_field_permissions/set_committing shape, just built from stock
// SettingsGroup-wrapped controls (no bespoke Content widget needed: every
// field here is a plain CheckButton/ComboBox, unlike General's avatar disc
// + NativeTextField overlays).
//
// Encryption is one-directional: matrix-sdk has no "disable encryption"
// operation, so once the room is (or becomes) encrypted, the checkbox
// becomes permanently checked and disabled (see refresh_encryption_enabled_).
// A warning label appears under the checkbox whenever it reads as checked
// (staged on, or already encrypted), surfacing the "can't undo" consequence
// at the moment it becomes relevant.
//
// Join rule only ever offers Public/Invite/Knock as editable choices. If
// the room's current join rule is Restricted or Knock Restricted (which
// carry an allow-list this UI doesn't manage), the combo is replaced with
// read-only text so an allow-list we don't understand is never clobbered.

#include "SettingsPage.h"

#include "tk/combobox.h"
#include "tk/controls.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class RoomSecuritySection : public SettingsPage
{
public:
    RoomSecuritySection();
    ~RoomSecuritySection() override = default;

    // Silent seeding (no callbacks fired) — called by RoomSettingsView::open().
    void set_encryption(bool is_encrypted);
    void set_join_rule(std::string join_rule); // "public"|"invite"|"knock"|"restricted"|"knock_restricted"|"private"|""
    void set_guest_access(bool allow);
    void set_history_visibility(std::string visibility);

    // Independent per-field permission gating.
    void set_field_permissions(bool can_encryption, bool can_join_rule,
                               bool can_guest_access,
                               bool can_history_visibility);
    void set_committing(bool committing);

    // Fired on user interaction; RoomSettingsView copies these straight
    // into its own staged_*_ members. No server write happens here.
    std::function<void(bool)>        on_encryption_changed;
    std::function<void(std::string)> on_join_rule_changed;
    std::function<void(bool)>        on_guest_access_changed;
    std::function<void(std::string)> on_history_visibility_changed;

    // Fired whenever a control's visibility changes at runtime (the
    // encryption warning label appearing/disappearing, the join-rule
    // combo/read-only-label swap) — FlexBox::arrange() skips invisible
    // children entirely, so a widget that just became visible has stale
    // (typically {0,0,0,0}) bounds_ until the next full arrange() pass.
    // RoomSettingsView forwards this to its own on_layout_changed, which
    // the shell already wires to a full relayout.
    std::function<void()> on_layout_changed;

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

    // Accessors used by tests to simulate user interaction and inspect
    // state (mirrors RoomMediaSection::override_combo()).
    tk::CheckButton* encryption_checkbox() const { return encryption_check_; }
    tk::Widget*      encryption_warning() const { return encryption_warning_; }
    tk::ComboBox*    join_rule_combo() const { return join_rule_combo_; }
    tk::Widget*      join_rule_readonly() const { return join_rule_readonly_; }
    tk::CheckButton* guest_access_checkbox() const { return guest_access_check_; }
    tk::ComboBox*    history_visibility_combo() const { return history_combo_; }

private:
    void refresh_encryption_enabled_();
    void refresh_encryption_warning_();

    tk::CheckButton* encryption_check_   = nullptr;
    tk::Label*       encryption_warning_ = nullptr;
    tk::ComboBox*    join_rule_combo_    = nullptr;
    tk::Label*       join_rule_readonly_ = nullptr;
    tk::CheckButton* guest_access_check_ = nullptr;
    tk::ComboBox*    history_combo_      = nullptr;

    std::string staged_join_rule_;
    std::string staged_history_visibility_;

    bool already_encrypted_ = false;
    bool can_encryption_    = false;
    bool can_join_rule_     = false;
    bool can_guest_access_  = false;
    bool can_history_       = false;
    bool committing_        = false;
};

} // namespace tesseract::views
