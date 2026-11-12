#include "views/EncryptionSetupOverlay.h"
#include "tk/canvas.h"
#include "icons.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/loading_spinner.h"
#include "tk/theme.h"
#include "views/media_utils.h" // rect_contains
#include "views/sas_emoji_tiles.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>

namespace tesseract::views
{

namespace
{
// Shortest recovery passphrase accepted, in characters. A one-character
// passphrase protected the whole key backup against the homeserver operator.
constexpr std::size_t kMinRecoveryPassphraseChars = 12;

constexpr float kCardW = 480.0f;
constexpr float kCardPad = 32.0f;
constexpr float kCardRad = 10.0f;
constexpr float kEncryptionSetupBtnH = 38.0f;
constexpr float kEncryptionSetupBtnHPad = 18.0f;
constexpr float kBtnRad = 6.0f;
constexpr float kRowGap = 12.0f;
constexpr float kChevronPx = 16.0f; // Choose-step row chevron
// ResetApproving: when to suggest the browser approved a different account.
constexpr long long kResetHintAfterMs = 60'000;

// Width a text button needs to fit its single-line label plus side padding.
// Shared by the child tk::Button geometry and the bare text links so both keep
// the original hand-painted widths.
float button_width(tk::PaintCtx& ctx, const std::string& label)
{
    tk::TextStyle st;
    st.role = tk::FontRole::UiSemibold;
    auto lo = ctx.factory.build_text(label, st);
    return (lo ? lo->measure().w : 0.0f) + 2.0f * kEncryptionSetupBtnHPad;
}

// Draw a left-aligned body / label paragraph; returns the rendered height so
// callers can stack content below it.
float paint_paragraph(tk::PaintCtx& ctx, tk::Rect area, const std::string& text, tk::FontRole role,
                      tk::Color color)
{
    tk::TextStyle st;
    st.role = role;
    st.wrap = true;
    st.max_width = area.w;
    auto lo = ctx.factory.build_text(text, st);
    if (!lo)
    {
        return 0.0f;
    }
    ctx.canvas.draw_text(*lo, {area.x, area.y}, color);
    return lo->measure().h;
}

} // namespace

// Recover › Choose's option row: a bordered card with a title, a one-line
// hint, and a chevron. A real button (focusable, Enter/Space, AT Button
// named by the title with the hint as its description); the recommended
// card gets the accent border.
class OptionCardButton : public tk::Button
{
protected:
    OptionCardButton() : tk::Button("", {}, tk::Button::Variant::Subtle)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(OptionCardButton)

public:
    void set_content(std::string title, std::string hint, bool recommended)
    {
        if (title != label())
        {
            set_label(title);
        }
        hint_ = std::move(hint);
        recommended_ = recommended;
    }

    std::string access_description() const override
    {
        return hint_;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        auto& c = ctx.canvas;
        const tk::Rect r = bounds();
        c.fill_rounded_rect(r, kBtnRad,
                            pressed()   ? pal.subtle_pressed
                            : hovered() ? pal.subtle_hover
                                        : pal.bg);
        c.stroke_rounded_rect(r, kBtnRad, recommended_ ? pal.accent : pal.border,
                              recommended_ ? 1.5f : 1.0f);
        tk::TextStyle ts;
        ts.role = tk::FontRole::SidebarName;
        if (auto lo = ctx.factory.build_text(label(), ts))
        {
            c.draw_text(*lo, {r.x + 14.0f, r.y + 11.0f}, pal.text_primary);
        }
        tk::TextStyle hs;
        hs.role = tk::FontRole::Caption;
        hs.trim = tk::TextTrim::Ellipsis;
        hs.max_width = r.w - 48.0f;
        if (auto lo = ctx.factory.build_text(hint_, hs))
        {
            c.draw_text(*lo, {r.x + 14.0f, r.y + 35.0f}, pal.text_secondary);
        }
        chevron_.draw(c, ctx.factory, kChevronRightSvg,
                      {r.x + r.w - 14.0f - kChevronPx, r.y, kChevronPx, r.h}, kChevronPx,
                      pal.text_muted);
    }

private:
    std::string hint_;
    bool recommended_ = false;
    tk::IconCache chevron_;
};

EncryptionSetupOverlay::EncryptionSetupOverlay(Mode mode)
    : mode_(mode), step_(initial_step_(mode)), done_kind_(initial_done_kind_(mode))
{
    // Filled action buttons. They are positioned, labelled, and (for primary)
    // enabled/disabled per-step in paint(); both start hidden.
    auto prim = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Primary);
    primary_button_ = add_child(std::move(prim));
    primary_button_->set_visible(false);
    primary_button_->set_on_click(
        [this]
        {
            fire_primary_();
        });

    auto copy = tk::create_widget<tk::Button>(this, tk::tr("Copy"), std::function<void()>{},
                                              tk::Button::Variant::Subtle);
    copy_button_ = add_child(std::move(copy));
    copy_button_->set_visible(false);
    copy_button_->set_on_click(
        [this]
        {
            if (!on_copy_to_clipboard)
            {
                return;
            }
            if (step_ == Step::ResetApproving)
            {
                on_copy_to_clipboard(reset_approval_url_);
                link_copied_ = true;
                if (host())
                {
                    host()->request_repaint();
                }
            }
            else
            {
                on_copy_to_clipboard(recovery_key_);
            }
        });

    auto save = tk::create_widget<tk::Button>(this, tk::tr("Save to file\xe2\x80\xa6"),
                                              std::function<void()>{}, tk::Button::Variant::Subtle);
    save_button_ = add_child(std::move(save));
    save_button_->set_visible(false);
    save_button_->set_on_click(
        [this]
        {
            if (on_save_to_file)
            {
                on_save_to_file(recovery_key_);
            }
        });

    auto make_link = [this](std::string label, std::function<void()> on_click)
    {
        auto link = tk::create_widget<tk::Button>(this, std::move(label), std::move(on_click),
                                                  tk::Button::Variant::Link);
        tk::Button* raw = add_child(std::move(link));
        raw->set_visible(false);
        return raw;
    };
    skip_link_ = make_link(tk::tr("Skip for now"),
                           [this]
                           {
                               skip_();
                           });
    passphrase_link_ = make_link(tk::tr("Use a recovery passphrase instead"),
                                 [this]
                                 {
                                     use_passphrase_();
                                 });
    back_link_ = make_link(tk::tr("Back"),
                           [this]
                           {
                               go_back_();
                           });
    lost_link_ = make_link(tk::tr("I've lost my recovery key and other devices"),
                           [this]
                           {
                               advance_step_(Step::LostAccess);
                           });
    reject_link_ = make_link(tk::tr("Not me"),
                             [this]
                             {
                                 reject_();
                             });
    remove_key_link_ = make_link(tk::tr("Sign out and delete the key"),
                                 [this]
                                 {
                                     sign_out_removing_key_();
                                 });
    turn_off_link_ = make_link(tk::tr("Turn off key storage"),
                               [this]
                               {
                                   turn_off_backup_();
                               });

    device_card_ = add_child(tk::create_widget<OptionCardButton>(this));
    device_card_->set_visible(false);
    device_card_->set_on_click(
        [this]
        {
            choose_other_device_();
        });
    key_card_ = add_child(tk::create_widget<OptionCardButton>(this));
    key_card_->set_visible(false);
    key_card_->set_on_click(
        [this]
        {
            advance_step_(Step::EnterKey);
        });

    auto key_saved = tk::create_widget<tk::CheckButton>(this, tk::tr("I've saved my recovery key"));
    key_saved_cb_ = add_child(std::move(key_saved));
    key_saved_cb_->set_visible(false);
    key_saved_cb_->on_change = [this](bool checked)
    {
        key_saved_checked_ = checked;
        if (host())
        {
            host()->request_repaint();
        }
    };

    auto keep = tk::create_widget<tk::CheckButton>(this, tk::tr("Also keep it on this device"));
    keep_on_device_cb_ = add_child(std::move(keep));
    keep_on_device_cb_->set_visible(false);
    keep_on_device_cb_->on_change = [this](bool checked)
    {
        keep_on_device_checked_ = checked;
        if (host())
        {
            host()->request_repaint();
        }
    };

    if (host())
    {
        // Typing changes whether Continue / Verify is enabled (and whether
        // the "don't match" hint shows), both computed in paint().
        auto repaint = [this](const std::string&)
        {
            if (host())
            {
                host()->request_repaint();
            }
        };
        auto submit = [this]
        {
            if (primary_enabled_)
            {
                fire_primary_();
            }
        };

        auto passphrase = tk::create_widget<tk::TextField>(this, 36.0f);
        passphrase->set_password(true);
        passphrase->set_accessible_name(tk::tr("Recovery passphrase"));
        passphrase->set_visible(false);
        passphrase->set_on_changed(repaint);
        passphrase_field_ = add_child(std::move(passphrase));

        auto confirm = tk::create_widget<tk::TextField>(this, 36.0f);
        confirm->set_password(true);
        confirm->set_accessible_name(tk::tr("Confirm recovery passphrase"));
        confirm->set_visible(false);
        confirm->set_on_changed(repaint);
        confirm->set_on_submit(submit);
        passphrase_confirm_field_ = add_child(std::move(confirm));

        auto key = tk::create_widget<tk::TextField>(this, 36.0f);
        key->set_password(false);
        key->set_accessible_name(tk::tr("Recovery key or recovery passphrase"));
        key->set_visible(false);
        key->set_on_changed(repaint);
        key->set_on_submit(submit);
        key_field_ = add_child(std::move(key));
    }
}

std::string EncryptionSetupOverlay::format_recovery_key(const std::string& key)
{
    std::string out;
    int in_group = 0;
    for (char ch : key)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            continue;
        }
        if (in_group == 4)
        {
            out += ' ';
            in_group = 0;
        }
        out += ch;
        ++in_group;
    }
    return out;
}

void EncryptionSetupOverlay::key_save_result(bool ok, const std::string& error)
{
    save_failed_ = !ok;
    if (ok)
    {
        save_status_ = tk::tr("Saved.");
        key_saved_checked_ = true;
    }
    else
    {
        save_status_ = error.empty() ? tk::tr("Couldn't save the file.")
                                     : tk::trf(tk::tr("Couldn't save the file: {0}"), {error});
    }
    announce_(save_status_, !ok);
    if (host())
    {
        host()->request_repaint();
    }
}

void EncryptionSetupOverlay::hide_native_fields()
{
    if (passphrase_field_)
    {
        passphrase_field_->set_visible(false);
    }
    if (passphrase_confirm_field_)
    {
        passphrase_confirm_field_->set_visible(false);
    }
    if (key_field_)
    {
        key_field_->set_visible(false);
    }
}

// ── advance_progress ─────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_progress(uint8_t step, const std::string& recovery_key,
                                              uint32_t backed_up, uint32_t total)
{
    if (step == 7) // an existing key backup on the server: ask before replacing it
    {
        replace_backup_pending_ = true;
        advance_step_(Step::Intro);
        error_msg_ = tk::tr("This account already has key storage on the server. "
                            "Replacing it can make older messages unreadable on your "
                            "other devices. Only continue if you can't use your old "
                            "recovery key.");
        announce_(error_msg_, true);
        return;
    }
    if (step == 4 || step == 5)
    {
        replace_backup_pending_ = false;
    }
    update_progress_label_(step, backed_up, total);
    if (step == 4) // Done
    {
        recovery_key_ = recovery_key;
        if (recovery_key_.empty() || mode_ == Mode::Recover)
        {
            advance_step_(Step::Done);
        }
        else
        {
            advance_step_(Step::ShowKey);
        }
    }
    else if (step == 5) // Fatal error
    {
        // advance_step_ clears error_msg_, so capture and restore afterwards.
        std::string msg = recovery_key; // error text passed in recovery_key field
        if (mode_ == Mode::Recover)
        {
            advance_step_(Step::EnterKey);
        }
        else
        {
            advance_step_(passphrase_mode_ ? Step::PassphraseEntry : Step::Intro);
        }
        error_msg_ = std::move(msg);
        announce_(error_msg_, true);
    }
}

void EncryptionSetupOverlay::update_progress_label_(uint8_t step, uint32_t backed_up,
                                                    uint32_t total)
{
    // The individual stages (creating the backup, generating the key,
    // uploading room keys) mean nothing to a new user: show one label, and a
    // progress bar for the one stage that has a count.
    progress_fraction_ = -1.0f;
    switch (step)
    {
    case 0:
    case 1:
    case 2:
        progress_label_ = mode_ == Mode::Recover ? tk::tr("Unlocking your messages\xe2\x80\xa6")
                                                 : tk::tr("Securing your messages\xe2\x80\xa6");
        break;
    case 3:
        progress_label_ = tk::tr("Securing your messages\xe2\x80\xa6");
        if (total > 0)
        {
            progress_fraction_ =
                std::clamp(static_cast<float>(backed_up) / static_cast<float>(total), 0.0f, 1.0f);
        }
        break;
    case 4:
        progress_label_ = tk::tr("Done.");
        break;
    default:
        break;
    }
}

// ── Step transitions ──────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_step_(Step next)
{
    error_msg_.clear();
    step_ = next;
    if (next == Step::Progress || next == Step::ResetApproving)
    {
        progress_start_ = std::chrono::steady_clock::now();
    }
    if (next == Step::EnterKey && key_field_)
    {
        // Host::request_focus() requires visible_in_tree(), and arrange()
        // (triggered below via on_layout_changed) hasn't run yet this frame —
        // show it explicitly first so the focus request doesn't silently
        // no-op. The subsequent arrange() pass redundantly re-shows/
        // repositions it, which is harmless.
        key_field_->set_visible(true);
        key_field_->set_focused(true);
    }
    if (next == Step::PassphraseEntry && passphrase_field_)
    {
        passphrase_field_->set_visible(true); // same reason as key_field_ above
        passphrase_field_->set_focused(true);
    }
    // The visible native field set changes with the step (EnterKey shows the
    // key field; leaving it hides it). Ask the shell to relayout so its
    // on-layout pass shows/hides the NativeTextField accordingly.
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

void EncryptionSetupOverlay::fire_primary_()
{
    switch (step_)
    {
    case Step::Intro:
        if (mode_ == Mode::AutoSetupNotice)
        {
            if (on_close)
            {
                on_close(); // "OK"
            }
            break;
        }
        if (mode_ == Mode::SaveKey)
        {
            advance_step_(Step::ShowKey);
            break;
        }
        if (mode_ != Mode::Fresh)
        {
            advance_step_(Step::Choose);
            break;
        }
        if (replace_backup_pending_)
        {
            // Confirmed: replace the backup that setup stopped at.
            advance_step_(Step::Progress);
            if (on_replace_backup)
            {
                on_replace_backup(enable_passphrase_);
            }
            break;
        }
        // Fresh: the default path generates a recovery key straight away.
        passphrase_mode_ = false;
        enable_passphrase_.clear();
        advance_step_(Step::Progress);
        if (on_enable_recovery)
        {
            on_enable_recovery(std::string());
        }
        break;

    case Step::PassphraseEntry:
    {
        if (!passphrases_match_())
        {
            return;
        }
        // The passphrase is the only thing protecting the key backup from
        // the homeserver operator, so a short one is refused outright.
        {
            const std::string typed = passphrase_field_->text();
            std::size_t chars = 0;
            for (unsigned char c : typed)
            {
                if ((c & 0xC0) != 0x80)
                {
                    ++chars; // count UTF-8 code points
                }
            }
            if (chars < kMinRecoveryPassphraseChars)
            {
                error_msg_ =
                    tk::trf(tk::tr("A recovery passphrase must be at least {0} characters."),
                            {std::to_string(kMinRecoveryPassphraseChars)});
                announce_(error_msg_, true);
                return;
            }
        }
        std::string pass = passphrase_field_->text();
        enable_passphrase_ = pass;
        advance_step_(Step::Progress);
        if (replace_backup_pending_)
        {
            if (on_replace_backup)
            {
                on_replace_backup(std::move(pass));
            }
        }
        else if (on_enable_recovery)
        {
            on_enable_recovery(std::move(pass));
        }
        break;
    }

    case Step::EnterKey:
    {
        std::string key = key_field_ ? key_field_->text() : std::string();
        if (key.empty())
        {
            return;
        }
        advance_step_(Step::Progress);
        if (on_recover)
        {
            on_recover(std::move(key));
        }
    }
    break;

    case Step::Progress:
        break; // not user-dismissible

    case Step::ShowKey:
        if (!key_saved_checked_)
        {
            return;
        }
        advance_step_(Step::Done);
        if (on_key_saved)
        {
            on_key_saved(mode_ == Mode::SaveKey && keep_on_device_checked_);
        }
        break;

    case Step::Done:
        if (before_sign_out_ && on_sign_out)
        {
            on_sign_out(false);
        }
        else if (on_close)
        {
            on_close();
        }
        break;

    case Step::ResetApproving:
        // Button is "Cancel" (or "Close" once an error is shown): abort the
        // in-progress reset. ShellBase hides the overlay.
        if (on_cancel_reset)
        {
            on_cancel_reset();
        }
        break;

    case Step::Choose:
        break; // the option rows act, not a primary button

    case Step::LostAccess:
        if (on_reset_encryption)
        {
            on_reset_encryption();
        }
        break;

    case Step::WaitingForOtherDevice:
        // Button is "Cancel".
        if (on_cancel_verification)
        {
            on_cancel_verification();
        }
        return_to_start();
        break;

    case Step::IncomingRequest:
        advance_step_(Step::WaitingForOtherDevice);
        if (on_accept_request)
        {
            on_accept_request();
        }
        break;

    case Step::CompareCodes:
        advance_step_(Step::Confirming);
        announce_(tk::tr("Confirming\xe2\x80\xa6"));
        if (on_sas_match)
        {
            on_sas_match();
        }
        break;

    case Step::Confirming:
        break; // waiting for the SDK

    case Step::VerifyFailed:
        if (can_retry_)
        {
            advance_step_(Step::WaitingForOtherDevice);
            if (on_retry_verification)
            {
                on_retry_verification();
            }
        }
        else if (on_close)
        {
            on_close();
        }
        break;
    }
}

// ── Interactive verification ──────────────────────────────────────────────────

void EncryptionSetupOverlay::set_has_verified_other_device(bool has)
{
    if (has_other_device_ == has)
    {
        return;
    }
    has_other_device_ = has;
    // The Choose card grows / shrinks by a row.
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

void EncryptionSetupOverlay::show_incoming_request(std::string peer, bool own_device)
{
    peer_ = std::move(peer);
    incoming_own_device_ = own_device;
    // Someone else started this flow: a failure offers Close, never a retry
    // that would broadcast a request of our own.
    can_retry_ = false;
    // Remember where the user was so declining / cancelling goes back there
    // (e.g. the "all set" Done step) rather than to the start of setup.
    if (!in_verification_step() && step_ != Step::VerifyFailed)
    {
        resume_step_ = step_;
    }
    advance_step_(Step::IncomingRequest);
}

void EncryptionSetupOverlay::show_outgoing_user_request(std::string peer)
{
    peer_ = std::move(peer);
    incoming_own_device_ = false;
    can_retry_ = true;
    if (!in_verification_step() && step_ != Step::VerifyFailed)
    {
        resume_step_ = step_;
    }
    advance_step_(Step::WaitingForOtherDevice);
}

void EncryptionSetupOverlay::show_sas(VerificationSas sas)
{
    sas_ = std::move(sas);
    advance_step_(Step::CompareCodes);
}

void EncryptionSetupOverlay::verification_done(DoneKind kind)
{
    resume_step_.reset();
    done_kind_ = kind;
    advance_step_(Step::Done);
    announce_(done_text_());
}

void EncryptionSetupOverlay::verification_failed(std::string reason, bool can_retry)
{
    if (step_ == Step::VerifyFailed)
    {
        return;
    }
    can_retry_ = can_retry;
    advance_step_(Step::VerifyFailed);
    error_msg_ =
        reason.empty() ? tk::tr("The request was cancelled or timed out.") : std::move(reason);
    announce_(error_msg_, true);
}

void EncryptionSetupOverlay::verification_cancelled(const std::string& cancel_code, bool can_retry)
{
    std::string msg;
    if (cancel_code == "m.accepted")
    {
        msg = tk::tr("Another of your devices already took this request. You can close this.");
    }
    else if (cancel_code == "m.user")
    {
        msg = tk::tr("The other device cancelled the confirmation.");
    }
    else if (cancel_code == "m.timeout")
    {
        msg = tk::tr("The request timed out. Please try again.");
    }
    else if (cancel_code == "m.mismatched_sas" || cancel_code == "m.key_mismatch" ||
             cancel_code == "m.mismatched_commitment")
    {
        msg = tk::tr("The codes didn't match, so nothing was confirmed. If you "
                     "didn't start this, someone else may be trying to get into "
                     "your account.");
    }
    else
    {
        msg = tk::tr("Something went wrong while confirming. Please try again.");
    }
    verification_failed(std::move(msg), can_retry);
}

bool EncryptionSetupOverlay::in_verification_step() const
{
    switch (step_)
    {
    case Step::WaitingForOtherDevice:
    case Step::IncomingRequest:
    case Step::CompareCodes:
    case Step::Confirming:
        return true;
    default:
        return false;
    }
}

bool EncryptionSetupOverlay::busy() const
{
    return step_ == Step::Progress || step_ == Step::ShowKey || step_ == Step::ResetApproving ||
           step_ == Step::Confirming;
}

void EncryptionSetupOverlay::choose_other_device_()
{
    can_retry_ = true; // this device started it, so it can start it again
    // Our own devices this time: drop any other user a previous flow named.
    peer_.clear();
    incoming_own_device_ = true;
    advance_step_(Step::WaitingForOtherDevice);
    if (on_request_sas)
    {
        on_request_sas();
    }
}

void EncryptionSetupOverlay::return_to_start()
{
    if (resume_step_)
    {
        const Step back = *resume_step_;
        resume_step_.reset();
        advance_step_(back);
        return;
    }
    if (mode_ == Mode::Recover)
    {
        advance_step_(Step::Choose);
    }
    else if (mode_ == Mode::Fresh || mode_ == Mode::SaveKey)
    {
        advance_step_(Step::Intro);
    }
    else if (on_close)
    {
        on_close();
    }
}

// ── Simulation helpers ────────────────────────────────────────────────────────

void EncryptionSetupOverlay::simulate_primary_action()
{
    fire_primary_();
}
void EncryptionSetupOverlay::simulate_skip()
{
    skip_();
}

void EncryptionSetupOverlay::skip_() const
{
    if (before_sign_out_ && on_sign_out)
    {
        on_sign_out(false); // "Sign out without saving"
    }
    else if (on_close)
    {
        on_close();
    }
}

void EncryptionSetupOverlay::sign_out_removing_key_() const
{
    if (before_sign_out_ && on_sign_out)
    {
        on_sign_out(true);
    }
}

void EncryptionSetupOverlay::turn_off_backup_()
{
    if (mode_ != Mode::AutoSetupNotice)
    {
        return;
    }
    progress_label_ = tk::tr("Turning off key storage\xe2\x80\xa6");
    progress_fraction_ = -1.0f;
    advance_step_(Step::Progress);
    if (on_turn_off_backup)
    {
        on_turn_off_backup();
    }
}
void EncryptionSetupOverlay::simulate_select_passphrase_mode()
{
    use_passphrase_();
}
void EncryptionSetupOverlay::simulate_back()
{
    go_back_();
}
void EncryptionSetupOverlay::simulate_backdrop_click()
{
    // Any point outside the (centred) card: just inside the top-left corner.
    on_pointer_down({1.0f, 1.0f});
    on_pointer_up({1.0f, 1.0f}, true);
}
void EncryptionSetupOverlay::simulate_check_key_saved()
{
    key_saved_checked_ = true;
}
void EncryptionSetupOverlay::simulate_sas_link()
{
    choose_other_device_();
}
void EncryptionSetupOverlay::simulate_choose_recovery_key()
{
    advance_step_(Step::EnterKey);
}
void EncryptionSetupOverlay::simulate_choose_lost_access()
{
    advance_step_(Step::LostAccess);
}
void EncryptionSetupOverlay::simulate_secondary()
{
    reject_();
}

void EncryptionSetupOverlay::reject_()
{
    if (step_ == Step::IncomingRequest)
    {
        if (on_decline_request)
        {
            on_decline_request();
        }
    }
    else if (step_ == Step::CompareCodes)
    {
        if (on_sas_mismatch)
        {
            on_sas_mismatch();
        }
        verification_failed(tk::tr("The codes didn't match, so nothing was confirmed. If you "
                                   "didn't start this, someone else may be trying to get into "
                                   "your account."),
                            can_retry_);
    }
}

void EncryptionSetupOverlay::go_back_()
{
    passphrase_mode_ = false;
    if (step_ == Step::VerifyFailed)
    {
        return_to_start(); // honours where an incoming request interrupted
    }
    else
    {
        advance_step_(mode_ == Mode::Fresh || mode_ == Mode::SaveKey ? Step::Intro : Step::Choose);
    }
}

void EncryptionSetupOverlay::use_passphrase_()
{
    passphrase_mode_ = true;
    advance_step_(Step::PassphraseEntry);
}

std::string EncryptionSetupOverlay::done_text_() const
{
    switch (done_kind_)
    {
    case DoneKind::Protected:
        return tk::tr("You're all set. Your messages are protected.");
    case DoneKind::Unlocked:
        return tk::tr("You're all set. Your encrypted messages are "
                      "available on this device.");
    case DoneKind::OtherDeviceConfirmed:
        return tk::tr("Your other device is confirmed and can now read "
                      "your encrypted messages.");
    case DoneKind::UserVerified:
        return tk::tr("Verified. You can trust you're talking to the "
                      "right person.");
    case DoneKind::BackupOff:
        return tk::tr("Key storage is off. Your encrypted messages are kept only "
                      "on your devices.");
    }
    return {};
}

void EncryptionSetupOverlay::announce_(const std::string& text, bool assertive)
{
    if (host())
    {
        host()->announce(text, assertive ? tk::Host::Politeness::Assertive
                                         : tk::Host::Politeness::Polite);
    }
}

void EncryptionSetupOverlay::on_theme_changed(const tk::Theme& t)
{
    if (passphrase_field_)
    {
        passphrase_field_->set_text_color(t.palette.text_primary);
    }
    if (passphrase_confirm_field_)
    {
        passphrase_confirm_field_->set_text_color(t.palette.text_primary);
    }
    if (key_field_)
    {
        key_field_->set_text_color(t.palette.text_primary);
    }
}

void EncryptionSetupOverlay::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v)
    {
        hide_native_fields();
        replace_backup_pending_ = false; // dismissed without confirming
    }
}

// ── Field-rect accessors ──────────────────────────────────────────────────────

bool EncryptionSetupOverlay::passphrase_field_rect_visible() const
{
    return visible() && step_ == Step::PassphraseEntry;
}

tk::Rect EncryptionSetupOverlay::passphrase_field_rect_value() const
{
    if (!passphrase_field_rect_visible())
    {
        return {};
    }
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 152.0f, card.w - 2.0f * kCardPad, 36.0f};
}

tk::Rect EncryptionSetupOverlay::passphrase_confirm_field_rect_value() const
{
    if (!passphrase_field_rect_visible())
    {
        return {};
    }
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 222.0f, card.w - 2.0f * kCardPad, 36.0f};
}

bool EncryptionSetupOverlay::passphrases_match_() const
{
    // Without native fields (tests constructed with no Host) there is nothing
    // to compare; treat that as "not ready".
    if (!passphrase_field_ || !passphrase_confirm_field_)
    {
        return false;
    }
    const std::string a = passphrase_field_->text();
    return !a.empty() && a == passphrase_confirm_field_->text();
}

bool EncryptionSetupOverlay::backdrop_closes_() const
{
    // Only steps with nothing to lose close on a stray backdrop click. ShowKey
    // in particular must not: dismissing it before the key is saved throws
    // the only copy of the key away.
    // SaveKey's ShowKey is just a look at a key Tesseract keeps in secure
    // storage, unless that's the only copy.
    if (step_ == Step::ShowKey)
    {
        return mode_ == Mode::SaveKey && !key_only_in_memory_;
    }
    return step_ == Step::Intro || step_ == Step::Choose || step_ == Step::EnterKey ||
           step_ == Step::Done || step_ == Step::VerifyFailed;
}

bool EncryptionSetupOverlay::key_field_rect_visible() const
{
    return visible() && step_ == Step::EnterKey;
}

tk::Rect EncryptionSetupOverlay::key_field_rect_value() const
{
    if (!key_field_rect_visible())
    {
        return {};
    }
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 120.0f, card.w - 2.0f * kCardPad, 36.0f};
}

// ── Layout / paint ────────────────────────────────────────────────────────────

float EncryptionSetupOverlay::step_card_height_() const
{
    switch (step_)
    {
    case Step::Intro:
        return mode_ == Mode::Fresh             ? (replace_backup_pending_ ? 370.0f : 290.0f)
               : mode_ == Mode::SaveKey         ? (before_sign_out_ ? 380.0f : 310.0f)
               : mode_ == Mode::AutoSetupNotice ? 310.0f
                                                : 250.0f;
    case Step::PassphraseEntry:
        return 400.0f;
    case Step::EnterKey:
        return 300.0f;
    case Step::Progress:
        return 220.0f;
    case Step::ShowKey:
        return mode_ == Mode::SaveKey ? 520.0f : 410.0f;
    case Step::Done:
        return 240.0f;
    case Step::ResetApproving:
        return 340.0f;
    case Step::Choose:
        return has_other_device_ ? 398.0f : 324.0f;
    case Step::LostAccess:
        return 300.0f;
    case Step::WaitingForOtherDevice:
        return 270.0f;
    case Step::IncomingRequest:
        return 250.0f;
    // The emoji grid is only there when the peer also offered `emoji`.
    case Step::CompareCodes:
        return sas_.emojis.empty() ? 300.0f : 490.0f;
    case Step::Confirming:
        return 220.0f;
    case Step::VerifyFailed:
        return 270.0f;
    }
    return 280.0f;
}

tk::Rect EncryptionSetupOverlay::card_bounds() const
{
    auto b = bounds();
    float card_h = step_card_height_();
    float cx = b.x + (b.w - kCardW) * 0.5f;
    float cy = b.y + (b.h - card_h) * 0.5f;
    return {cx, cy, kCardW, card_h};
}

tk::Size EncryptionSetupOverlay::measure(tk::LayoutCtx& /*ctx*/, tk::Size avail)
{
    return avail;
}

void EncryptionSetupOverlay::arrange(tk::LayoutCtx& /*ctx*/, tk::Rect b)
{
    bounds_ = b;
}

// Genuine override, kept intentionally (see the paint_children()-automation
// work in ui/shared/tk/widget.h): each step repositions, relabels, and
// selectively shows/hides the same handful of shared controls (primary/copy/
// save buttons, passphrase/key fields) inline with its own step-specific text
// and layout — a real per-frame state machine, not chrome wrapped strictly
// before/after a fixed set of children. Left as a documented exception
// rather than force-fit into paint_before_children()/paint_after_children().
void EncryptionSetupOverlay::paint(tk::PaintCtx& ctx)
{
    auto b = bounds();
    auto& c = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Forget clickable regions from the previous frame so a control that is
    // not drawn this step can't be hit-tested with a stale rect. The child
    // action buttons are hidden up front and re-shown by the steps that use
    // them, so a vanished button is skipped by hit-testing too.
    primary_enabled_ = true;
    access_title_.clear();
    access_body_.clear();
    access_rows_.clear();
    // The links and option cards are hidden *after* the step below has
    // placed the ones it uses (not hidden-then-reshown like the buttons
    // just below), so a focused link doesn't flicker invisible every frame.
    std::vector<tk::Button*> placed;
    if (primary_button_)
    {
        primary_button_->set_visible(false);
    }
    if (copy_button_)
    {
        copy_button_->set_visible(false);
    }
    if (save_button_)
    {
        save_button_->set_visible(false);
    }
    if (key_saved_cb_)
    {
        key_saved_cb_->set_visible(false);
    }
    if (keep_on_device_cb_)
    {
        keep_on_device_cb_->set_visible(false);
    }
    // Unlike the buttons above (stateless canvas widgets — redundant
    // set_visible() is harmless), the text fields wrap a real native OS
    // control. Hiding one that's about to stay the active field for this
    // exact step and immediately reshowing it below would be a genuine
    // hide→show round trip within a single paint() call — on Qt that silently
    // drops real keyboard focus on the hide and never restores it on the
    // reshow (see tk::TextField::set_visible's own same-value guard, which
    // only catches *identical* repeated calls, not this two-call sequence).
    // So only hide the fields that AREN'T staying active; the step-specific
    // code below still unconditionally calls set_visible(true) on the active
    // ones, which is a same-value no-op when they already were.
    const bool key_field_wanted = step_ == Step::EnterKey;
    const bool passphrase_fields_wanted = step_ == Step::PassphraseEntry;
    if (!passphrase_fields_wanted)
    {
        if (passphrase_field_)
        {
            passphrase_field_->set_visible(false);
        }
        if (passphrase_confirm_field_)
        {
            passphrase_confirm_field_->set_visible(false);
        }
    }
    if (key_field_ && !key_field_wanted)
    {
        key_field_->set_visible(false);
    }

    // Position, style, and paint the reusable primary button for the current
    // step. The label/enabled state are refreshed every frame; widths come from
    // button_width() so they match the old hand-painted geometry exactly.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    auto place_primary = [&](tk::Rect r, const std::string& label, bool enabled)
    {
        if (!primary_button_)
        {
            return;
        }
        primary_button_->set_visible(true);
        if (primary_button_->label() != label)
        {
            primary_button_->set_label(label);
        }
        primary_button_->set_enabled(enabled);
        primary_button_->arrange(lc, r);
        primary_button_->paint(ctx);
    };
    auto place_button = [&](tk::Button* btn, tk::Rect r)
    {
        if (!btn)
        {
            return;
        }
        placed.push_back(btn);
        btn->set_visible(true);
        btn->arrange(lc, r);
        btn->paint(ctx);
    };
    auto place_field = [&](tk::TextField* field, tk::Rect fr)
    {
        if (fr.empty())
        {
            return;
        }
        c.fill_rounded_rect(fr, kBtnRad, pal.bg);
        c.stroke_rounded_rect(fr, kBtnRad, pal.border, 1.0f);
        if (!field)
        {
            return;
        }
        field->set_visible(true);
        field->arrange(lc, fr);
        field->paint(ctx);
    };

    // Dim backdrop + card.
    c.fill_rect(b, tk::Color{0, 0, 0, 128});
    auto card = card_bounds();
    c.fill_rounded_rect(card, kCardRad, pal.chrome_bg);

    const float cx = card.x + kCardPad;
    const float cw = card.w - 2.0f * kCardPad;
    const float by = card.y + card.h - kCardPad - kEncryptionSetupBtnH; // bottom button row

    // ── Title (steps that have one) ──────────────────────────────────────────
    float content_y = card.y + kCardPad;
    auto draw_title = [&](const std::string& title)
    {
        access_title_ = title;
        tk::TextStyle st;
        st.role = tk::FontRole::Title;
        auto lo = ctx.factory.build_text(title, st);
        if (lo)
        {
            c.draw_text(*lo, {cx, content_y}, pal.text_primary);
            content_y += lo->measure().h + 16.0f;
        }
    };

    // Self-driving spinner dots centred at (card centre, y).
    auto draw_spinner = [&](float y)
    {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - progress_start_)
                                    .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(c, {card.x + card.w * 0.5f, y}, phase,
                              /*radius=*/16.0f, /*dot_r=*/3.0f, pal.accent);
        if (host())
        {
            host()->request_repaint();
        }
    };
    auto place_right_primary = [&](const std::string& label, bool enabled)
    {
        float w = button_width(ctx, label);
        place_primary({card.x + card.w - kCardPad - w, by, w, kEncryptionSetupBtnH}, label,
                      enabled);
    };
    // A bare link whose text starts at the content's left edge.
    auto left_link = [&](tk::Button* link, float y, const std::string& label)
    {
        if (!link)
        {
            return;
        }
        if (link->label() != label)
        {
            link->set_label(label);
        }
        place_button(link, {cx - kEncryptionSetupBtnHPad, y, button_width(ctx, label),
                            kEncryptionSetupBtnH});
    };
    // Painted-only text an AT should still be able to read.
    auto note = [&](const std::string& text, tk::Rect r, tk::Role role = tk::Role::StaticText)
    {
        if (!text.empty())
        {
            access_rows_.push_back({role, text, r});
        }
    };

    switch (step_)
    {
    // ── Choose (Recover) ─────────────────────────────────────────────────
    case Step::Choose:
    {
        draw_title(tk::tr("Confirm it's you"));
        paint_paragraph(ctx, {cx, content_y, cw, 0},
                        tk::tr("Choose how to unlock your encrypted messages "
                               "on this device."),
                        tk::FontRole::Body, pal.text_secondary);

        access_body_ = tk::tr("Choose how to unlock your encrypted messages "
                              "on this device.");

        // Option rows: a bordered card each, title + one-line hint.
        constexpr float kRowH = 64.0f;
        float row_y = card.y + 128.0f;
        auto place_card = [&](OptionCardButton* card_btn, const std::string& title,
                              const std::string& hint, bool recommended)
        {
            if (!card_btn)
            {
                return;
            }
            card_btn->set_content(title, hint, recommended);
            place_button(card_btn, {cx, row_y, cw, kRowH});
            row_y += kRowH + 10.0f;
        };
        if (has_other_device_)
        {
            place_card(device_card_, tk::tr("Use another device"),
                       tk::tr("Approve from a device where you're "
                              "already signed in"),
                       true);
        }
        place_card(key_card_, tk::tr("Enter recovery key"),
                   tk::tr("Or the recovery passphrase you chose instead"), !has_other_device_);

        left_link(lost_link_, row_y - 2.0f, tk::tr("I've lost my recovery key and other devices"));

        left_link(skip_link_, by, tk::tr("Skip for now"));
        break;
    }

    // ── LostAccess (Recover) ─────────────────────────────────────────────
    case Step::LostAccess:
    {
        draw_title(tk::tr("Reset digital identity?"));
        access_body_ = tk::tr("If you've lost both your recovery key and every other "
                              "signed-in device, you can start over with a new recovery "
                              "key. Messages you can't read now will stay unreadable, "
                              "and people you chat with will see that your digital "
                              "identity changed.");
        paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                        pal.text_secondary);
        place_right_primary(tk::tr("Reset digital identity"), true);
        left_link(back_link_, by, tk::tr("Back"));
        break;
    }

    // ── WaitingForOtherDevice ────────────────────────────────────────────
    case Step::WaitingForOtherDevice:
    {
        const bool we_asked = mode_ == Mode::Recover || can_retry_;
        if (we_asked && !incoming_own_device_)
        {
            draw_title(tk::trf(tk::tr("Waiting for {0}"), {peer_}));
            access_body_ = tk::trf(tk::tr("{0} needs to accept the verification "
                                          "request in their Matrix app."),
                                   {peer_});
            paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                            pal.text_secondary);
        }
        else
        {
            draw_title(we_asked ? tk::tr("Check your other device")
                                : tk::tr("Starting verification\xe2\x80\xa6"));
            if (we_asked)
            {
                access_body_ = tk::tr("Open Tesseract (or another Matrix app) on a "
                                      "device where you're signed in and accept "
                                      "the request.");
                paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                                pal.text_secondary);
            }
        }
        draw_spinner(by - 40.0f);
        place_right_primary(tk::tr("Cancel"), true);
        break;
    }

    // ── IncomingRequest ──────────────────────────────────────────────────
    case Step::IncomingRequest:
    {
        draw_title(incoming_own_device_ ? tk::tr("Was this you?") : tk::tr("Verification request"));
        const std::string body =
            incoming_own_device_
                ? tk::trf(tk::tr("Your device \xe2\x80\x9c{0}\xe2\x80\x9d wants to "
                                 "confirm it's you so it can read your "
                                 "encrypted messages."),
                          {peer_})
                : tk::trf(tk::tr("{0} wants to verify you. You'll compare a "
                                 "few numbers to make sure you're talking to "
                                 "the right person."),
                          {peer_});
        access_body_ = body;
        paint_paragraph(ctx, {cx, content_y, cw, 0}, body, tk::FontRole::Body, pal.text_secondary);
        place_right_primary(tk::tr("Continue"), true);
        left_link(reject_link_, by, incoming_own_device_ ? tk::tr("Not me") : tk::tr("Decline"));
        break;
    }

    // ── CompareCodes ─────────────────────────────────────────────────────
    // The decimal numbers are the primary comparison (MSC4405 deprecates
    // emoji SAS); the emoji grid follows when the peer still offers it.
    case Step::CompareCodes:
    {
        draw_title(tk::tr("Compare numbers"));
        access_body_ = incoming_own_device_
                           ? tk::tr("Check that the other device shows the same numbers in "
                                    "the same order.")
                           : tk::trf(tk::tr("Check that {0} sees the same numbers in the same "
                                            "order."),
                                     {peer_});
        content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                                     pal.text_secondary);
        content_y += 14.0f;
        const tk::Rect dec_rect{cx, content_y, cw, sas_decimal_row_height()};
        paint_sas_decimal_row(ctx, dec_rect, sas_.decimals);
        note(tk::trf(tk::tr("Numbers: {0}, {1}, {2}"),
                     {std::to_string(sas_.decimals[0]), std::to_string(sas_.decimals[1]),
                      std::to_string(sas_.decimals[2])}),
             dec_rect);
        content_y += sas_decimal_row_height();
        if (!sas_.emojis.empty())
        {
            content_y += 18.0f;
            const std::string emoji_caption = tk::tr("Or compare these emoji:");
            const float cap_h = paint_paragraph(ctx, {cx, content_y, cw, 0}, emoji_caption,
                                                tk::FontRole::Caption, pal.text_secondary);
            note(emoji_caption, {cx, content_y, cw, cap_h});
            content_y += cap_h;
            const tk::Rect grid_rect{cx, content_y + 10.0f, cw, sas_emoji_grid_height()};
            paint_sas_emoji_grid(ctx, grid_rect, sas_.emojis);
            // One row per emoji, in reading order, named by its label so
            // the comparison works by ear.
            for (std::size_t i = 0; i < sas_.emojis.size(); ++i)
            {
                note(tk::trf(tk::tr("{0}. {1}"),
                             {std::to_string(i + 1), sas_.emojis[i].description}),
                     grid_rect);
            }
        }
        place_right_primary(tk::tr("They match"), true);
        left_link(reject_link_, by, tk::tr("They don't match"));
        break;
    }

    // ── Confirming ───────────────────────────────────────────────────────
    case Step::Confirming:
    {
        const float scy = card.y + card.h * 0.42f;
        draw_spinner(scy);
        tk::TextStyle st;
        st.role = tk::FontRole::Body;
        access_title_ = tk::tr("Confirming\xe2\x80\xa6");
        if (auto lo = ctx.factory.build_text(access_title_, st))
        {
            tk::Size sz = lo->measure();
            c.draw_text(*lo, {card.x + (card.w - sz.w) * 0.5f, scy + 34.0f}, pal.text_secondary);
        }
        break;
    }

    // ── VerifyFailed ─────────────────────────────────────────────────────
    case Step::VerifyFailed:
    {
        draw_title(tk::tr("Couldn't confirm"));
        access_body_ = error_msg_;
        paint_paragraph(ctx, {cx, content_y, cw, 0}, error_msg_, tk::FontRole::Body,
                        pal.text_secondary);
        place_right_primary(can_retry_ ? tk::tr("Try again") : tk::tr("Close"), true);
        if (mode_ != Mode::Verify)
        {
            left_link(back_link_, by, tk::tr("Back"));
        }
        break;
    }

    // ── Intro ────────────────────────────────────────────────────────────
    case Step::Intro:
    {
        const bool fresh = mode_ == Mode::Fresh;
        const bool save_key = mode_ == Mode::SaveKey;
        const bool sign_out = save_key && before_sign_out_;
        const bool notice = mode_ == Mode::AutoSetupNotice;
        draw_title(notice     ? tk::tr("Key storage is now on")
                   : fresh    ? tk::tr("Protect your messages")
                   : sign_out ? tk::tr("Save your recovery key before you sign out")
                   : save_key ? tk::tr("Keep your messages safe")
                              : tk::tr("Confirm it's you"));
        const std::string body =
            notice     ? tk::tr("Tesseract turned on key storage for your "
                                "encrypted messages and made a recovery key for your "
                                "account. You'll need that key to read your messages "
                                "if you sign in on a new device or lose this one, so "
                                "a reminder at the top of the window will help you "
                                "save it.")
            : fresh    ? tk::tr("Your messages are end-to-end encrypted. A recovery "
                                "key lets you read them again if you sign in on a "
                                "new device or lose this one.")
            : sign_out ? tk::tr("Your recovery key isn't saved anywhere but this "
                                "device yet. Without it, you won't be able to read "
                                "your old messages after you sign in again. If you "
                                "sign out without saving it, it stays on this "
                                "computer so you can sign back in; on a shared "
                                "computer, delete it instead.")
            : save_key ? tk::tr("Tesseract made a recovery key for your account. If "
                                "you lose this device or sign out, it's the only way "
                                "to read your old messages again. Keep a copy "
                                "somewhere safe, like a password manager, or print "
                                "it out.")
                       : tk::tr("To read your encrypted messages on this device, "
                                "confirm it's you with your recovery key or with "
                                "another device where you're signed in.");
        access_body_ = body;
        content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, body, tk::FontRole::Body,
                                     pal.text_secondary);

        // Inline error (e.g. enable_recovery failed) — shown so the user
        // sees why setup bounced back here.
        if (!error_msg_.empty())
        {
            const float eh = paint_paragraph(ctx, {cx, content_y + 10.0f, cw, 0}, error_msg_,
                                             tk::FontRole::Small, pal.destructive);
            note(error_msg_, {cx, content_y + 10.0f, cw, eh});
        }

        // Advanced option: a passphrase the user picks, instead of a
        // generated key. Deliberately a quiet link, not an upfront choice.
        if (fresh)
        {
            left_link(passphrase_link_, by - kEncryptionSetupBtnH - 6.0f,
                      tk::tr("Use a recovery passphrase instead"));
        }

        const std::string prim = notice                             ? tk::tr("OK")
                                 : fresh && replace_backup_pending_ ? tk::tr("Replace key storage")
                                 : fresh                            ? tk::tr("Create recovery key")
                                 : save_key                         ? tk::tr("Show my recovery key")
                                                                    : tk::tr("Continue");
        float pw = button_width(ctx, prim);
        place_primary({card.x + card.w - kCardPad - pw, by, pw, kEncryptionSetupBtnH}, prim, true);

        if (sign_out)
        {
            left_link(remove_key_link_, by - kEncryptionSetupBtnH - 6.0f,
                      tk::tr("Sign out and delete the key"));
        }
        if (notice)
        {
            left_link(turn_off_link_, by, tk::tr("Turn off key storage"));
        }
        if (!notice)
        {
            left_link(skip_link_, by,
                      sign_out   ? tk::tr("Sign out without saving")
                      : save_key ? tk::tr("Remind me later")
                                 : tk::tr("Skip for now"));
        }
        break;
    }

    // ── PassphraseEntry (Fresh only) ─────────────────────────────────────
    case Step::PassphraseEntry:
    {
        draw_title(tk::tr("Choose a recovery passphrase"));
        access_body_ = tk::tr("Pick something memorable that you don't use "
                              "anywhere else.");
        paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                        pal.text_secondary);

        tk::Rect f1 = passphrase_field_rect_value();
        tk::Rect f2 = passphrase_confirm_field_rect_value();
        paint_paragraph(ctx, {cx, f1.y - 20.0f, cw, 0}, tk::tr("Recovery passphrase"),
                        tk::FontRole::Small, pal.text_muted);
        place_field(passphrase_field_, f1);
        paint_paragraph(ctx, {cx, f2.y - 20.0f, cw, 0}, tk::tr("Confirm recovery passphrase"),
                        tk::FontRole::Small, pal.text_muted);
        place_field(passphrase_confirm_field_, f2);

        // Mismatch hint only once the confirmation has been typed, so the
        // user isn't scolded before they've had a chance.
        const std::string confirm =
            passphrase_confirm_field_ ? passphrase_confirm_field_->text() : std::string();
        std::string hint = error_msg_;
        if (hint.empty() && !confirm.empty() && !passphrases_match_())
        {
            hint = tk::tr("The recovery passphrases don't match.");
        }
        if (!hint.empty())
        {
            const float hh = paint_paragraph(ctx, {cx, f2.y + f2.h + 10.0f, cw, 0}, hint,
                                             tk::FontRole::Small, pal.destructive);
            note(hint, {cx, f2.y + f2.h + 10.0f, cw, hh});
        }

        primary_enabled_ = passphrases_match_();
        const std::string cont = tk::tr("Continue");
        float cwid = button_width(ctx, cont);
        place_primary({card.x + card.w - kCardPad - cwid, by, cwid, kEncryptionSetupBtnH}, cont,
                      primary_enabled_);

        left_link(back_link_, by, tk::tr("Back"));
        break;
    }

    // ── EnterKey (Recover only) ──────────────────────────────────────────
    case Step::EnterKey:
    {
        draw_title(tk::tr("Enter your recovery key"));
        access_body_ = tk::tr("If you set a recovery passphrase instead, enter that.");
        paint_paragraph(ctx, {cx, card.y + 92.0f, cw, 0}, access_body_, tk::FontRole::Small,
                        pal.text_muted);

        place_field(key_field_, key_field_rect_value()); // card.y + 120

        if (!error_msg_.empty())
        {
            const float eh = paint_paragraph(ctx, {cx, card.y + 168.0f, cw, 0}, error_msg_,
                                             tk::FontRole::Small, pal.destructive);
            note(error_msg_, {cx, card.y + 168.0f, cw, eh});
        }

        left_link(back_link_, by, tk::tr("Back"));

        const std::string key = key_field_ ? key_field_->text() : std::string();
        primary_enabled_ = !key.empty();
        const std::string verify = tk::tr("Unlock");
        float vw = button_width(ctx, verify);
        place_primary({card.x + card.w - kCardPad - vw, by, vw, kEncryptionSetupBtnH}, verify,
                      primary_enabled_);
        break;
    }

    // ── Progress ─────────────────────────────────────────────────────────
    case Step::Progress:
    {
        const float scx = card.x + card.w * 0.5f;
        const float scy = card.y + card.h * 0.38f;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - progress_start_)
                                    .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(c, {scx, scy}, phase, /*radius=*/16.0f,
                              /*dot_r=*/3.0f, pal.accent);
        // Status label centred below the spinner.
        float below = scy + 16.0f + 18.0f;
        if (!progress_label_.empty())
        {
            tk::TextStyle st;
            st.role = tk::FontRole::Body;
            auto lo = ctx.factory.build_text(progress_label_, st);
            if (lo)
            {
                tk::Size sz = lo->measure();
                c.draw_text(*lo, {scx - sz.w * 0.5f, below}, pal.text_secondary);
                below += sz.h + 14.0f;
            }
        }
        access_title_ = progress_label_;
        {
            // A busy/progress node an AT can poll; percent in the name
            // since these rows carry no numeric value.
            std::string pname = progress_label_;
            if (progress_fraction_ >= 0.0f)
            {
                pname =
                    tk::trf(tk::tr("{0} {1}%"),
                            {progress_label_,
                             std::to_string(static_cast<int>(progress_fraction_ * 100.0f + 0.5f))});
            }
            note(pname, {card.x + kCardPad, scy - 16.0f, cw, below - scy + 16.0f},
                 tk::Role::ProgressBar);
        }
        // Thin determinate bar while room keys upload.
        if (progress_fraction_ >= 0.0f)
        {
            const float bw = cw * 0.6f;
            tk::Rect track{scx - bw * 0.5f, below, bw, 4.0f};
            c.fill_rounded_rect(track, 2.0f, pal.border);
            tk::Rect fill = track;
            fill.w = std::max(track.h, track.w * progress_fraction_);
            c.fill_rounded_rect(fill, 2.0f, pal.accent);
        }
        if (host())
        {
            host()->request_repaint(); // self-drive the spinner
        }
        break;
    }

    // ── ShowKey (Fresh only) ─────────────────────────────────────────────
    case Step::ShowKey:
    {
        draw_title(tk::tr("Save your recovery key"));
        access_body_ = tk::tr("Keep it somewhere safe, like a password manager. You'll "
                              "need it to read your messages if you sign in on a new "
                              "device.");
        content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, access_body_, tk::FontRole::Body,
                                     pal.text_secondary);
        content_y += 14.0f;

        // Key box, grouped in fours so it can be read out / compared.
        tk::Rect box{cx, content_y, cw, 58.0f};
        c.fill_rounded_rect(box, kBtnRad, pal.bg);
        c.stroke_rounded_rect(box, kBtnRad, pal.border, 1.0f);
        {
            tk::TextStyle st;
            st.role = tk::FontRole::Body;
            st.wrap = true;
            st.max_width = box.w - 24.0f;
            auto lo = ctx.factory.build_text(format_recovery_key(recovery_key_), st);
            if (lo)
            {
                c.draw_text(*lo, {box.x + 12.0f, box.y + 10.0f}, pal.text_primary);
            }
            // Read group by group ("ABCD EFGH …") so it can be copied
            // down by ear.
            note(tk::trf(tk::tr("Recovery key: {0}"), {format_recovery_key(recovery_key_)}), box);
        }

        // Copy (+ Save to file… when the shell offers it) below the box,
        // right-aligned.
        const float row_y = box.y + box.h + 10.0f;
        float right = card.x + card.w - kCardPad;
        if (copy_button_)
        {
            if (copy_button_->label() != tk::tr("Copy"))
            {
                copy_button_->set_label(tk::tr("Copy"));
            }
            float w = button_width(ctx, copy_button_->label());
            right -= w;
            place_button(copy_button_, {right, row_y, w, kEncryptionSetupBtnH});
            right -= kRowGap;
        }
        if (save_button_ && on_save_to_file)
        {
            float w = button_width(ctx, save_button_->label());
            right -= w;
            place_button(save_button_, {right, row_y, w, kEncryptionSetupBtnH});
            right -= kRowGap;
        }
        if (!save_status_.empty())
        {
            tk::TextStyle st;
            st.role = tk::FontRole::Small;
            st.wrap = true;
            st.max_width = std::max(0.0f, right - cx);
            auto lo = ctx.factory.build_text(save_status_, st);
            if (lo)
            {
                const float sy = row_y + (kEncryptionSetupBtnH - lo->measure().h) * 0.5f;
                c.draw_text(*lo, {cx, sy}, save_failed_ ? pal.destructive : pal.text_muted);
                note(save_status_, {cx, sy, lo->measure().w, lo->measure().h});
            }
        }

        // "I've saved my recovery key" checkbox (a real tk::CheckButton,
        // synced silently from key_saved_checked_).
        if (key_saved_cb_)
        {
            key_saved_cb_->set_checked(key_saved_checked_);
            key_saved_cb_->set_visible(true);
            key_saved_cb_->arrange(lc, {cx, row_y + kEncryptionSetupBtnH + 12.0f, cw, 24.0f});
            key_saved_cb_->paint(ctx);
        }

        // SaveKey: opt in to keeping the key in the OS secure storage
        // (off by default — Tesseract otherwise deletes its copy).
        if (mode_ == Mode::SaveKey && keep_on_device_cb_)
        {
            const float ky = row_y + kEncryptionSetupBtnH + 12.0f + 24.0f + 12.0f;
            keep_on_device_cb_->set_checked(keep_on_device_checked_);
            keep_on_device_cb_->set_visible(true);
            keep_on_device_cb_->arrange(lc, {cx, ky, cw, 24.0f});
            keep_on_device_cb_->paint(ctx);
            const std::string why =
                tk::tr("Tesseract keeps it in this computer's secure storage, so "
                       "it can unlock your messages by itself if you reinstall. "
                       "Anyone who can open that storage could read your "
                       "messages too.");
            const tk::Rect wr{cx + 28.0f, ky + 28.0f, cw - 28.0f, 0};
            const float wh = paint_paragraph(ctx, wr, why, tk::FontRole::Small, pal.text_muted);
            note(why, {wr.x, wr.y, wr.w, wh});
        }

        // A look at a key kept in secure storage can be left unsaved.
        if (mode_ == Mode::SaveKey && !key_only_in_memory_)
        {
            left_link(back_link_, by, tk::tr("Back"));
        }

        primary_enabled_ = key_saved_checked_;
        const std::string cont = tk::tr("Continue");
        float cwid = button_width(ctx, cont);
        place_primary({card.x + card.w - kCardPad - cwid, by, cwid, kEncryptionSetupBtnH}, cont,
                      primary_enabled_);
        break;
    }

    // ── Done ─────────────────────────────────────────────────────────────
    case Step::Done:
    {
        // Accent checkmark disc.
        const float disc = 56.0f;
        tk::Rect dr{card.x + (card.w - disc) * 0.5f, card.y + 40.0f, disc, disc};
        c.fill_rounded_rect(dr, disc * 0.5f, pal.accent);
        done_check_icon_.draw(c, ctx.factory, kCheckSvg, dr, 30.0f, pal.text_on_accent);
        const std::string body = done_text_();
        access_title_ = body;
        {
            tk::TextStyle st;
            st.role = tk::FontRole::Body;
            st.wrap = true;
            st.max_width = cw;
            st.halign = tk::TextHAlign::Center;
            auto lo = ctx.factory.build_text(body, st);
            if (lo)
            {
                tk::Size sz = lo->measure();
                c.draw_text(*lo, {card.x + (card.w - sz.w) * 0.5f, dr.y + dr.h + 18.0f},
                            pal.text_secondary);
            }
        }
        const std::string close = before_sign_out_ ? tk::tr("Sign out") : tk::tr("Close");
        float clw = button_width(ctx, close);
        place_primary({card.x + (card.w - clw) * 0.5f, by, clw, kEncryptionSetupBtnH}, close, true);
        break;
    }

    // ── ResetApproving (cross-signing reset awaiting browser approval) ───
    case Step::ResetApproving:
    {
        draw_title(tk::tr("Resetting digital identity"));

        // The approval page acts on whichever account the *browser* is
        // signed in as — nothing in the server-issued link pins it to
        // ours — so name the account, offer the link for a private
        // window, and nudge if approval never arrives.
        const bool failed = !error_msg_.empty();
        std::string body;
        if (failed)
        {
            body = error_msg_;
        }
        else if (reset_account_.empty())
        {
            body = tk::tr("Approve the digital identity reset in the browser window "
                          "that just opened. This will continue once you've confirmed.");
        }
        else
        {
            body = tk::trf(tk::tr("Approve the digital identity reset as {0} in the browser window "
                                  "that just opened. If the browser is signed in "
                                  "as a different account, sign out there first, "
                                  "or copy the link into a private window."),
                           {reset_account_});
        }
        access_body_ = body;
        content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, body, tk::FontRole::Body,
                                     failed ? pal.destructive : pal.text_secondary);

        // Spinner (only while waiting; hidden once an error is shown).
        if (!failed)
        {
            const float scy = content_y + 34.0f;
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - progress_start_)
                                        .count();
            const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
            tk::draw_spinner_dots(c, {card.x + card.w * 0.5f, scy}, phase,
                                  /*radius=*/16.0f, /*dot_r=*/3.0f, pal.accent);
            if (host())
            {
                host()->request_repaint(); // self-drive the spinner
            }

            if (elapsed_ms >= kResetHintAfterMs && !reset_account_.empty())
            {
                const std::string hint =
                    tk::trf(tk::tr("Still waiting? Check that you approved it as "
                                   "{0}, not another account."),
                            {reset_account_});
                const float hh = paint_paragraph(ctx, {cx, scy + 30.0f, cw, 0}, hint,
                                                 tk::FontRole::Small, pal.text_muted);
                note(hint, {cx, scy + 30.0f, cw, hh});
            }

            // Copy the approval link (for a private window / other
            // profile signed in as the right account).
            if (!reset_approval_url_.empty() && copy_button_)
            {
                const std::string copy_lbl =
                    link_copied_ ? tk::tr("Link copied") : tk::tr("Copy link");
                if (copy_button_->label() != copy_lbl)
                {
                    copy_button_->set_label(copy_lbl);
                }
                place_button(copy_button_, {cx - kEncryptionSetupBtnHPad, by,
                                            button_width(ctx, copy_lbl), kEncryptionSetupBtnH});
            }
        }

        // Cancel (or Close once an error is shown), bottom-right.
        const std::string lbl = failed ? tk::tr("Close") : tk::tr("Cancel");
        float lw = button_width(ctx, lbl);
        place_primary({card.x + card.w - kCardPad - lw, by, lw, kEncryptionSetupBtnH}, lbl, true);
        break;
    }
    }

    for (tk::Button* b : {skip_link_, passphrase_link_, back_link_, lost_link_, reject_link_,
                          remove_key_link_, turn_off_link_, static_cast<tk::Button*>(device_card_),
                          static_cast<tk::Button*>(key_card_)})
    {
        if (b && std::find(placed.begin(), placed.end(), b) == placed.end())
        {
            b->set_visible(false);
        }
    }
}

// ── Pointer handling ──────────────────────────────────────────────────────────

bool EncryptionSetupOverlay::on_pointer_down(tk::Point local)
{
    if (!visible())
    {
        return false;
    }

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    backdrop_press_ = false;

    // During Progress / Confirming the operation is in flight, and during
    // ResetApproving only the Cancel/Close child button acts: swallow
    // backdrop / stray clicks so nothing can be dismissed mid-flight.
    if (step_ == Step::Progress || step_ == Step::Confirming || step_ == Step::ResetApproving)
    {
        return true;
    }

    // Every control on the card is a child tk::Button / CheckButton /
    // TextField the host dispatches directly; only the backdrop lands here.
    if (!rect_contains(card_bounds(), w))
    {
        backdrop_press_ = true;
    }

    return true; // modal: always consume
}

void EncryptionSetupOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (backdrop_press_ && inside_self && backdrop_closes_() && !rect_contains(card_bounds(), w))
    {
        if (on_close)
        {
            on_close();
        }
    }
    backdrop_press_ = false;
    if (host())
    {
        host()->request_repaint();
    }
}

bool EncryptionSetupOverlay::on_key_down(const tk::KeyEvent& e)
{
    if (!visible())
    {
        return false;
    }
    if (e.key == tk::Key::Escape)
    {
        if (backdrop_closes_() && on_close)
        {
            on_close();
        }
        return true;
    }
    return false;
}

// ── Accessibility rows ────────────────────────────────────────────────────────

tk::Role EncryptionSetupOverlay::access_role_for_widget_row(std::size_t i) const
{
    return i < access_rows_.size() ? access_rows_[i].role : tk::Role::None;
}

std::string EncryptionSetupOverlay::access_name_for_widget_row(std::size_t i) const
{
    return i < access_rows_.size() ? access_rows_[i].name : std::string();
}

tk::Rect EncryptionSetupOverlay::access_rect_for_widget_row(std::size_t i) const
{
    return i < access_rows_.size() ? access_rows_[i].rect : tk::Rect{};
}

} // namespace tesseract::views
