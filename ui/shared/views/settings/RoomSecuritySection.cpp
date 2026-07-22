#include "RoomSecuritySection.h"

#include "SettingsGroup.h"

#include "tk/i18n.h"

#include <memory>
#include <string>
#include <utility>

namespace tesseract::views
{

RoomSecuritySection::RoomSecuritySection()
{
    // ── Encryption ───────────────────────────────────────────────────────
    auto* enc_group = add_group(tk::tr("Encryption"));
    encryption_group_ = enc_group;
    auto enc_check = tk::create_widget<tk::CheckButton>(this, tk::tr("Encrypt this room"));
    encryption_check_ = enc_group->add_widget(std::move(enc_check));
    encryption_check_->on_change = [this](bool checked)
    {
        refresh_encryption_warning_();
        if (on_encryption_changed) on_encryption_changed(checked);
    };

    auto enc_warning = tk::create_widget<tk::Label>(
        this, tk::tr("Once enabled, encryption cannot be turned off."), tk::FontRole::Body);
    enc_warning->set_colour(tk::Color::rgb(0xcc3333));
    encryption_warning_ = enc_group->add_widget(std::move(enc_warning));
    encryption_warning_->set_visible(false);

    // ── Who can join ─────────────────────────────────────────────────────
    auto* join_group = add_group(tk::tr("Who can join"));
    auto jr_combo = tk::create_widget<tk::ComboBox>(this);
    jr_combo->set_options({
        {tk::tr("Public"),      "public"},
        {tk::tr("Invite only"), "invite"},
        {tk::tr("Knock"),       "knock"},
    });
    join_rule_combo_ = join_group->add_widget(std::move(jr_combo));
    join_rule_combo_->on_changed = [this](std::string v)
    {
        staged_join_rule_ = v;
        if (on_join_rule_changed) on_join_rule_changed(std::move(v));
    };
    auto jr_readonly = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    join_rule_readonly_ = join_group->add_widget(std::move(jr_readonly));
    join_rule_readonly_->set_visible(false);

    // ── Guest access ─────────────────────────────────────────────────────
    auto* guest_group = add_group(tk::tr("Guest Access"));
    auto guest_check = tk::create_widget<tk::CheckButton>(this, tk::tr("Allow guests to join"));
    guest_access_check_ = guest_group->add_widget(std::move(guest_check));
    guest_access_check_->on_change = [this](bool checked)
    {
        if (on_guest_access_changed) on_guest_access_changed(checked);
    };

    // ── History visibility ───────────────────────────────────────────────
    auto* history_group = add_group(tk::tr("History Visibility"));
    auto hv_combo = tk::create_widget<tk::ComboBox>(this);
    hv_combo->set_options({
        {tk::tr("Anyone"),                  "world_readable"},
        {tk::tr("Members"),                 "shared"},
        {tk::tr("Members (since invited)"), "invited"},
        {tk::tr("Members (since joined)"),  "joined"},
    });
    history_combo_ = history_group->add_widget(std::move(hv_combo));
    history_combo_->on_changed = [this](std::string v)
    {
        staged_history_visibility_ = v;
        if (on_history_visibility_changed) on_history_visibility_changed(std::move(v));
    };
}

void RoomSecuritySection::refresh_encryption_enabled_()
{
    encryption_check_->set_enabled(can_encryption_ && !committing_ && !already_encrypted_);
}

void RoomSecuritySection::refresh_encryption_warning_()
{
    // Shown whenever the checkbox reads as checked — whether because the
    // room is already encrypted, or the user just staged turning it on —
    // surfacing the "can't undo" consequence exactly when it's relevant,
    // not as constant noise.
    const bool was_visible = encryption_warning_->visible();
    const bool now_visible = encryption_check_->checked();
    encryption_warning_->set_visible(now_visible);
    // FlexBox::arrange() skips invisible children entirely, so a widget
    // that just became visible has stale bounds_ (paints at {0,0,0,0},
    // i.e. the app's top-left corner) until the next full arrange() pass —
    // request one whenever the visibility actually flips.
    if (was_visible != now_visible && on_layout_changed)
        on_layout_changed();
}

void RoomSecuritySection::set_encryption(bool is_encrypted)
{
    encryption_check_->set_checked(is_encrypted);
    // Authoritative per call, not an OR-latch: a fresh open() for a
    // different, unencrypted room must re-enable the checkbox.
    already_encrypted_ = is_encrypted;
    refresh_encryption_enabled_();
    refresh_encryption_warning_();
}

void RoomSecuritySection::set_join_rule(std::string join_rule)
{
    staged_join_rule_ = std::move(join_rule);
    const bool managed_elsewhere =
        staged_join_rule_ == "restricted" || staged_join_rule_ == "knock_restricted";
    const bool combo_was_visible = join_rule_combo_->visible();
    join_rule_combo_->set_visible(!managed_elsewhere);
    join_rule_readonly_->set_visible(managed_elsewhere);
    if (!managed_elsewhere)
    {
        join_rule_combo_->set_selected_value(
            staged_join_rule_.empty() ? "invite" : staged_join_rule_);
    }
    else
    {
        const std::string label = staged_join_rule_ == "restricted"
            ? tk::tr("Restricted") : tk::tr("Knock+Restricted");
        join_rule_readonly_->set_text(
            tk::trf(tk::tr("{0} (managed elsewhere)"), {label}));
    }
    // See refresh_encryption_warning_'s comment — a widget that just became
    // visible needs a fresh arrange() pass or it paints at stale bounds_.
    if (combo_was_visible == managed_elsewhere && on_layout_changed)
        on_layout_changed();
}

void RoomSecuritySection::set_guest_access(bool allow)
{
    guest_access_check_->set_checked(allow);
}

void RoomSecuritySection::set_history_visibility(std::string visibility)
{
    staged_history_visibility_ = std::move(visibility);
    history_combo_->set_selected_value(
        staged_history_visibility_.empty() ? "shared" : staged_history_visibility_);
}

void RoomSecuritySection::set_field_permissions(bool can_encryption, bool can_join_rule,
                                                bool can_guest_access,
                                                bool can_history_visibility)
{
    can_encryption_   = can_encryption;
    can_join_rule_    = can_join_rule;
    can_guest_access_ = can_guest_access;
    can_history_      = can_history_visibility;
    refresh_encryption_enabled_();
    join_rule_combo_->set_enabled(can_join_rule_ && !committing_);
    guest_access_check_->set_enabled(can_guest_access_ && !committing_);
    history_combo_->set_enabled(can_history_ && !committing_);
}

void RoomSecuritySection::set_committing(bool committing)
{
    committing_ = committing;
    refresh_encryption_enabled_();
    join_rule_combo_->set_enabled(can_join_rule_ && !committing_);
    guest_access_check_->set_enabled(can_guest_access_ && !committing_);
    history_combo_->set_enabled(can_history_ && !committing_);
}

void RoomSecuritySection::set_encryption_field_visible(bool visible)
{
    const bool was_visible = encryption_group_->visible();
    encryption_group_->set_visible(visible);
    // See refresh_encryption_warning_'s comment — a widget that just became
    // visible needs a fresh arrange() pass or it paints at stale bounds_.
    if (was_visible != visible && on_layout_changed)
        on_layout_changed();
}

} // namespace tesseract::views
