#include "RoomPermissionsSection.h"

#include "SettingsGroup.h"

#include "tk/form_layout.h"
#include "tk/i18n.h"

#include <memory>
#include <string>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr int64_t kLevelDefault   = 0;
constexpr int64_t kLevelModerator = 50;
constexpr int64_t kLevelAdmin     = 100;

// Shared by all 9 rows: seeds a combo's option list from the room's actual
// value, synthesizing a 4th "Custom (N)" option (selected) when that value
// doesn't match one of the three presets — mirrors RoomSecuritySection's
// join-rule "managed elsewhere" fallback idea, just expressed as a 4th
// combo option instead of a separate read-only label, since a non-preset
// power level is still a value the user might deliberately want to keep.
void set_level_combo(tk::ComboBox* combo, int64_t level)
{
    std::vector<tk::ComboBox::Option> opts = {
        {tk::tr("Default"),   "0"},
        {tk::tr("Moderator"), "50"},
        {tk::tr("Admin"),     "100"},
    };
    const std::string value = std::to_string(level);
    if (level != kLevelDefault && level != kLevelModerator && level != kLevelAdmin)
    {
        opts.push_back({tk::trf(tk::tr("Custom ({0})"), {value}), value});
    }
    combo->set_options(std::move(opts));
    combo->set_selected_value(value);
}

} // namespace

RoomPermissionsSection::RoomPermissionsSection()
{
    auto lockout_warning = tk::create_widget<tk::Label>(
        this, tk::tr("This change would prevent you from editing permissions again."),
        tk::FontRole::Body);
    lockout_warning->set_colour(tk::Color::rgb(0xcc3333));
    lockout_warning_ = add_widget(std::move(lockout_warning));
    lockout_warning_->set_visible(false);

    auto wire = [this](tk::ComboBox* combo, int64_t tesseract::RoomPermissions::* field)
    {
        combo->on_changed = [this, field](std::string value)
        {
            current_.*field = std::stoll(value);
            if (on_permissions_changed) on_permissions_changed(current_);
        };
    };

    // ── Default Role ─────────────────────────────────────────────────────
    auto* role_group = add_group(tk::tr("Default Role"));
    auto* role_form = role_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    role_form->set_label_gap(8.0f).set_spacing(8.0f).set_label_group(&label_group_);
    default_role_combo_ =
        role_form->add_row(tk::tr("New members"), tk::create_widget<tk::ComboBox>(this));
    wire(default_role_combo_, &tesseract::RoomPermissions::default_role);

    // ── Messages ──────────────────────────────────────────────────────────
    auto* messages_group = add_group(tk::tr("Messages"));
    auto* messages_form = messages_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    messages_form->set_label_gap(8.0f).set_spacing(8.0f).set_label_group(&label_group_);
    send_messages_combo_ = messages_form->add_row(
        tk::tr("Send messages"), tk::create_widget<tk::ComboBox>(this));
    wire(send_messages_combo_, &tesseract::RoomPermissions::send_messages);
    remove_messages_combo_ = messages_form->add_row(
        tk::tr("Remove messages sent by others"), tk::create_widget<tk::ComboBox>(this));
    wire(remove_messages_combo_, &tesseract::RoomPermissions::remove_messages);

    // ── Membership ────────────────────────────────────────────────────────
    auto* membership_group = add_group(tk::tr("Membership"));
    auto* membership_form = membership_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    membership_form->set_label_gap(8.0f).set_spacing(8.0f).set_label_group(&label_group_);
    invite_users_combo_ = membership_form->add_row(
        tk::tr("Invite users"), tk::create_widget<tk::ComboBox>(this));
    wire(invite_users_combo_, &tesseract::RoomPermissions::invite_users);
    kick_users_combo_ = membership_form->add_row(
        tk::tr("Kick users"), tk::create_widget<tk::ComboBox>(this));
    wire(kick_users_combo_, &tesseract::RoomPermissions::kick_users);
    ban_users_combo_ = membership_form->add_row(
        tk::tr("Ban users"), tk::create_widget<tk::ComboBox>(this));
    wire(ban_users_combo_, &tesseract::RoomPermissions::ban_users);

    // ── Advanced ──────────────────────────────────────────────────────────
    auto* advanced_group = add_group(tk::tr("Advanced"));
    auto* advanced_form = advanced_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    advanced_form->set_label_gap(8.0f).set_spacing(8.0f).set_label_group(&label_group_);
    change_settings_combo_ = advanced_form->add_row(
        tk::tr("Change settings"), tk::create_widget<tk::ComboBox>(this));
    wire(change_settings_combo_, &tesseract::RoomPermissions::change_settings);
    change_permissions_combo_ = advanced_form->add_row(
        tk::tr("Change permissions"), tk::create_widget<tk::ComboBox>(this));
    wire(change_permissions_combo_, &tesseract::RoomPermissions::change_permissions);
    notify_everyone_combo_ = advanced_form->add_row(
        tk::tr("Notify everyone (@room)"), tk::create_widget<tk::ComboBox>(this));
    wire(notify_everyone_combo_, &tesseract::RoomPermissions::notify_everyone);
}

void RoomPermissionsSection::set_permissions(const tesseract::RoomPermissions& p)
{
    current_ = p;
    set_level_combo(default_role_combo_, p.default_role);
    set_level_combo(send_messages_combo_, p.send_messages);
    set_level_combo(remove_messages_combo_, p.remove_messages);
    set_level_combo(invite_users_combo_, p.invite_users);
    set_level_combo(kick_users_combo_, p.kick_users);
    set_level_combo(ban_users_combo_, p.ban_users);
    set_level_combo(change_settings_combo_, p.change_settings);
    set_level_combo(change_permissions_combo_, p.change_permissions);
    set_level_combo(notify_everyone_combo_, p.notify_everyone);
}

void RoomPermissionsSection::refresh_enabled_()
{
    const bool enabled = can_edit_ && !committing_;
    default_role_combo_->set_enabled(enabled);
    send_messages_combo_->set_enabled(enabled);
    remove_messages_combo_->set_enabled(enabled);
    invite_users_combo_->set_enabled(enabled);
    kick_users_combo_->set_enabled(enabled);
    ban_users_combo_->set_enabled(enabled);
    change_settings_combo_->set_enabled(enabled);
    change_permissions_combo_->set_enabled(enabled);
    notify_everyone_combo_->set_enabled(enabled);
}

void RoomPermissionsSection::set_field_permissions(bool can_edit)
{
    can_edit_ = can_edit;
    refresh_enabled_();
}

void RoomPermissionsSection::set_committing(bool committing)
{
    committing_ = committing;
    refresh_enabled_();
}

void RoomPermissionsSection::set_would_lock_out_self(bool would_lock_out)
{
    const bool was_visible = lockout_warning_->visible();
    lockout_warning_->set_visible(would_lock_out);
    // FlexBox::arrange() skips invisible children entirely, so a widget
    // that just became visible has stale bounds_ (paints at {0,0,0,0},
    // i.e. the app's top-left corner) until the next full arrange() pass —
    // request one whenever the visibility actually flips. Mirrors
    // RoomSecuritySection::refresh_encryption_warning_.
    if (was_visible != would_lock_out && on_layout_changed)
        on_layout_changed();
}

} // namespace tesseract::views
