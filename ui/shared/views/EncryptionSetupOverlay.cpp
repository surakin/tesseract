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
constexpr float kCardW    = 480.0f;
constexpr float kCardPad  = 32.0f;
constexpr float kCardRad  = 10.0f;
constexpr float kEncryptionSetupBtnH     = 38.0f;
constexpr float kEncryptionSetupBtnHPad  = 18.0f;
constexpr float kBtnRad   = 6.0f;
constexpr float kRowGap   = 12.0f;
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

// A bare, background-less text link (accent-coloured, centred in `r`). Used for
// the "Skip for now" / "Back" / "Use a passphrase instead" / "Use another
// device instead" affordances — the filled action buttons are tk::Button
// children, not these.
void paint_link(tk::PaintCtx& ctx, tk::Rect r, const std::string& label)
{
    const auto& pal = ctx.theme.palette;
    tk::TextStyle st;
    st.role = tk::FontRole::UiSemibold;
    auto lo = ctx.factory.build_text(label, st);
    if (!lo) return;
    tk::Size sz = lo->measure();
    ctx.canvas.draw_text(*lo,
                         {r.x + (r.w - sz.w) * 0.5f, r.y + (r.h - sz.h) * 0.5f},
                         pal.accent);
}

// Draw a left-aligned body / label paragraph; returns the rendered height so
// callers can stack content below it.
float paint_paragraph(tk::PaintCtx& ctx, tk::Rect area, const std::string& text,
                      tk::FontRole role, tk::Color color)
{
    tk::TextStyle st;
    st.role      = role;
    st.wrap      = true;
    st.max_width = area.w;
    auto lo = ctx.factory.build_text(text, st);
    if (!lo) return 0.0f;
    ctx.canvas.draw_text(*lo, {area.x, area.y}, color);
    return lo->measure().h;
}

} // namespace

EncryptionSetupOverlay::EncryptionSetupOverlay(Mode mode)
    : mode_(mode), step_(initial_step_(mode)), done_kind_(initial_done_kind_(mode))
{
    // Filled action buttons. They are positioned, labelled, and (for primary)
    // enabled/disabled per-step in paint(); both start hidden.
    auto prim = tk::create_widget<tk::Button>(this,
        "", std::function<void()>{}, tk::Button::Variant::Primary);
    primary_button_ = add_child(std::move(prim));
    primary_button_->set_visible(false);
    primary_button_->set_on_click([this] { fire_primary_(); });

    auto copy = tk::create_widget<tk::Button>(this,
        tk::tr("Copy"), std::function<void()>{}, tk::Button::Variant::Subtle);
    copy_button_ = add_child(std::move(copy));
    copy_button_->set_visible(false);
    copy_button_->set_on_click(
        [this]
        {
            if (!on_copy_to_clipboard) return;
            if (step_ == Step::ResetApproving)
            {
                on_copy_to_clipboard(reset_approval_url_);
                link_copied_ = true;
                if (host()) host()->request_repaint();
            }
            else
            {
                on_copy_to_clipboard(recovery_key_);
            }
        });

    auto save = tk::create_widget<tk::Button>(this,
        tk::tr("Save to file\xe2\x80\xa6"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    save_button_ = add_child(std::move(save));
    save_button_->set_visible(false);
    save_button_->set_on_click(
        [this] { if (on_save_to_file) on_save_to_file(recovery_key_); });

    if (host())
    {
        // Typing changes whether Continue / Verify is enabled (and whether
        // the "don't match" hint shows), both computed in paint().
        auto repaint = [this](const std::string&)
        {
            if (host()) host()->request_repaint();
        };
        auto submit = [this] { if (primary_enabled_) fire_primary_(); };

        auto passphrase = tk::create_widget<tk::TextField>(this, 36.0f);
        passphrase->set_password(true);
        passphrase->set_visible(false);
        passphrase->set_on_changed(repaint);
        passphrase_field_ = add_child(std::move(passphrase));

        auto confirm = tk::create_widget<tk::TextField>(this, 36.0f);
        confirm->set_password(true);
        confirm->set_visible(false);
        confirm->set_on_changed(repaint);
        confirm->set_on_submit(submit);
        passphrase_confirm_field_ = add_child(std::move(confirm));

        auto key = tk::create_widget<tk::TextField>(this, 36.0f);
        key->set_password(false);
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
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
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
        save_status_       = tk::tr("Saved.");
        key_saved_checked_ = true;
    }
    else
    {
        save_status_ = error.empty() ? tk::tr("Couldn't save the file.")
                                     : tk::trf(tk::tr("Couldn't save the file: {0}"),
                                               {error});
    }
    if (host()) host()->request_repaint();
}

void EncryptionSetupOverlay::hide_native_fields()
{
    if (passphrase_field_)         passphrase_field_->set_visible(false);
    if (passphrase_confirm_field_) passphrase_confirm_field_->set_visible(false);
    if (key_field_)                key_field_->set_visible(false);
}

// ── advance_progress ─────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_progress(uint8_t step,
                                               const std::string& recovery_key,
                                               uint32_t backed_up,
                                               uint32_t total)
{
    update_progress_label_(step, backed_up, total);
    if (step == 4) // Done
    {
        recovery_key_ = recovery_key;
        if (recovery_key_.empty() || mode_ == Mode::Recover)
            advance_step_(Step::Done);
        else
            advance_step_(Step::ShowKey);
    }
    else if (step == 5) // Fatal error
    {
        // advance_step_ clears error_msg_, so capture and restore afterwards.
        std::string msg = recovery_key; // error text passed in recovery_key field
        if (mode_ == Mode::Recover)
            advance_step_(Step::EnterKey);
        else
            advance_step_(passphrase_mode_ ? Step::PassphraseEntry : Step::Intro);
        error_msg_ = std::move(msg);
    }
}

void EncryptionSetupOverlay::update_progress_label_(uint8_t step,
                                                     uint32_t backed_up,
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
            progress_label_ = mode_ == Mode::Recover
                                  ? tk::tr("Unlocking your messages\xe2\x80\xa6")
                                  : tk::tr("Securing your messages\xe2\x80\xa6");
            break;
        case 3:
            progress_label_ = tk::tr("Securing your messages\xe2\x80\xa6");
            if (total > 0)
                progress_fraction_ = std::clamp(
                    static_cast<float>(backed_up) / static_cast<float>(total),
                    0.0f, 1.0f);
            break;
        case 4: progress_label_ = tk::tr("Done."); break;
        default: break;
    }
}

// ── Step transitions ──────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_step_(Step next)
{
    error_msg_.clear();
    step_ = next;
    if (next == Step::Progress || next == Step::ResetApproving)
        progress_start_ = std::chrono::steady_clock::now();
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
    if (on_layout_changed) on_layout_changed();
}

void EncryptionSetupOverlay::fire_primary_()
{
    switch (step_)
    {
        case Step::Intro:
            if (mode_ != Mode::Fresh)
            {
                advance_step_(Step::Choose);
                break;
            }
            // Fresh: the default path generates a recovery key straight away.
            passphrase_mode_ = false;
            advance_step_(Step::Progress);
            if (on_enable_recovery) on_enable_recovery(std::string());
            break;

        case Step::PassphraseEntry:
        {
            if (!passphrases_match_()) return;
            std::string pass = passphrase_field_->text();
            advance_step_(Step::Progress);
            if (on_enable_recovery)
                on_enable_recovery(std::move(pass));
            break;
        }

        case Step::EnterKey:
            {
                std::string key = key_field_ ? key_field_->text() : std::string();
                if (key.empty()) return;
                advance_step_(Step::Progress);
                if (on_recover)
                    on_recover(std::move(key));
            }
            break;

        case Step::Progress:
            break; // not user-dismissible

        case Step::ShowKey:
            if (!key_saved_checked_) return;
            advance_step_(Step::Done);
            break;

        case Step::Done:
            if (on_close) on_close();
            break;

        case Step::ResetApproving:
            // Button is "Cancel" (or "Close" once an error is shown): abort the
            // in-progress reset. ShellBase hides the overlay.
            if (on_cancel_reset) on_cancel_reset();
            break;

        case Step::Choose:
            break; // the option rows act, not a primary button

        case Step::LostAccess:
            if (on_reset_encryption) on_reset_encryption();
            break;

        case Step::WaitingForOtherDevice:
            // Button is "Cancel".
            if (on_cancel_verification) on_cancel_verification();
            return_to_start();
            break;

        case Step::IncomingRequest:
            advance_step_(Step::WaitingForOtherDevice);
            if (on_accept_request) on_accept_request();
            break;

        case Step::CompareEmoji:
            advance_step_(Step::Confirming);
            if (on_sas_match) on_sas_match();
            break;

        case Step::Confirming:
            break; // waiting for the SDK

        case Step::VerifyFailed:
            if (can_retry_)
            {
                advance_step_(Step::WaitingForOtherDevice);
                if (on_retry_verification) on_retry_verification();
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
    if (has_other_device_ == has) return;
    has_other_device_ = has;
    // The Choose card grows / shrinks by a row.
    if (on_layout_changed) on_layout_changed();
}

void EncryptionSetupOverlay::show_incoming_request(std::string peer, bool own_device)
{
    peer_                = std::move(peer);
    incoming_own_device_ = own_device;
    // Someone else started this flow: a failure offers Close, never a retry
    // that would broadcast a request of our own.
    can_retry_           = false;
    // Remember where the user was so declining / cancelling goes back there
    // (e.g. the "all set" Done step) rather than to the start of setup.
    if (!in_verification_step() && step_ != Step::VerifyFailed)
        resume_step_ = step_;
    advance_step_(Step::IncomingRequest);
}

void EncryptionSetupOverlay::show_emojis(std::vector<VerificationEmoji> emojis)
{
    emojis_ = std::move(emojis);
    advance_step_(Step::CompareEmoji);
}

void EncryptionSetupOverlay::verification_done(DoneKind kind)
{
    resume_step_.reset();
    done_kind_ = kind;
    advance_step_(Step::Done);
}

void EncryptionSetupOverlay::verification_failed(std::string reason, bool can_retry)
{
    if (step_ == Step::VerifyFailed) return;
    can_retry_ = can_retry;
    advance_step_(Step::VerifyFailed);
    error_msg_ = reason.empty()
                     ? tk::tr("The request was cancelled or timed out.")
                     : std::move(reason);
}

bool EncryptionSetupOverlay::in_verification_step() const
{
    switch (step_)
    {
        case Step::WaitingForOtherDevice:
        case Step::IncomingRequest:
        case Step::CompareEmoji:
        case Step::Confirming:
            return true;
        default:
            return false;
    }
}

bool EncryptionSetupOverlay::busy() const
{
    return step_ == Step::Progress || step_ == Step::ShowKey ||
           step_ == Step::ResetApproving || step_ == Step::Confirming;
}

void EncryptionSetupOverlay::choose_other_device_()
{
    can_retry_ = true; // this device started it, so it can start it again
    advance_step_(Step::WaitingForOtherDevice);
    if (on_request_sas) on_request_sas();
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
        advance_step_(Step::Choose);
    else if (mode_ == Mode::Fresh)
        advance_step_(Step::Intro);
    else if (on_close)
        on_close();
}

// ── Simulation helpers ────────────────────────────────────────────────────────

void EncryptionSetupOverlay::simulate_primary_action()         { fire_primary_(); }
void EncryptionSetupOverlay::simulate_skip()                    { if (on_close) on_close(); }
void EncryptionSetupOverlay::simulate_select_passphrase_mode()
{
    passphrase_mode_ = true;
    advance_step_(Step::PassphraseEntry);
}
void EncryptionSetupOverlay::simulate_back()
{
    passphrase_mode_ = false;
    advance_step_(mode_ == Mode::Fresh ? Step::Intro : Step::Choose);
}
void EncryptionSetupOverlay::simulate_backdrop_click()
{
    // Any point outside the (centred) card: just inside the top-left corner.
    on_pointer_down({1.0f, 1.0f});
    on_pointer_up({1.0f, 1.0f}, true);
}
void EncryptionSetupOverlay::simulate_check_key_saved()         { key_saved_checked_ = true; }
void EncryptionSetupOverlay::simulate_sas_link()                { choose_other_device_(); }
void EncryptionSetupOverlay::simulate_choose_recovery_key()     { advance_step_(Step::EnterKey); }
void EncryptionSetupOverlay::simulate_choose_lost_access()      { advance_step_(Step::LostAccess); }
void EncryptionSetupOverlay::simulate_secondary() { reject_(); }

void EncryptionSetupOverlay::reject_()
{
    if (step_ == Step::IncomingRequest)
    {
        if (on_decline_request) on_decline_request();
    }
    else if (step_ == Step::CompareEmoji)
    {
        if (on_sas_mismatch) on_sas_mismatch();
        verification_failed(
            tk::tr("The emoji didn't match, so nothing was confirmed. If you "
                   "didn't start this, someone else may be trying to get into "
                   "your account."),
            can_retry_);
    }
}

void EncryptionSetupOverlay::on_theme_changed(const tk::Theme& t)
{
    if (passphrase_field_)
        passphrase_field_->set_text_color(t.palette.text_primary);
    if (passphrase_confirm_field_)
        passphrase_confirm_field_->set_text_color(t.palette.text_primary);
    if (key_field_)
        key_field_->set_text_color(t.palette.text_primary);
}

void EncryptionSetupOverlay::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v) hide_native_fields();
}

// ── Field-rect accessors ──────────────────────────────────────────────────────

bool EncryptionSetupOverlay::passphrase_field_rect_visible() const
{
    return visible() && step_ == Step::PassphraseEntry;
}

tk::Rect EncryptionSetupOverlay::passphrase_field_rect_value() const
{
    if (!passphrase_field_rect_visible()) return {};
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 152.0f, card.w - 2.0f * kCardPad, 36.0f};
}

tk::Rect EncryptionSetupOverlay::passphrase_confirm_field_rect_value() const
{
    if (!passphrase_field_rect_visible()) return {};
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 222.0f, card.w - 2.0f * kCardPad, 36.0f};
}

bool EncryptionSetupOverlay::passphrases_match_() const
{
    // Without native fields (tests constructed with no Host) there is nothing
    // to compare; treat that as "not ready".
    if (!passphrase_field_ || !passphrase_confirm_field_) return false;
    const std::string a = passphrase_field_->text();
    return !a.empty() && a == passphrase_confirm_field_->text();
}

bool EncryptionSetupOverlay::backdrop_closes_() const
{
    // Only steps with nothing to lose close on a stray backdrop click. ShowKey
    // in particular must not: dismissing it before the key is saved throws
    // the only copy of the key away.
    return step_ == Step::Intro || step_ == Step::Choose || step_ == Step::EnterKey ||
           step_ == Step::Done || step_ == Step::VerifyFailed;
}

bool EncryptionSetupOverlay::key_field_rect_visible() const
{
    return visible() && step_ == Step::EnterKey;
}

tk::Rect EncryptionSetupOverlay::key_field_rect_value() const
{
    if (!key_field_rect_visible()) return {};
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 120.0f, card.w - 2.0f * kCardPad, 36.0f};
}

// ── Layout / paint ────────────────────────────────────────────────────────────

float EncryptionSetupOverlay::step_card_height_() const
{
    switch (step_)
    {
        case Step::Intro:        return mode_ == Mode::Fresh ? 290.0f : 250.0f;
        case Step::PassphraseEntry: return 400.0f;
        case Step::EnterKey:     return 300.0f;
        case Step::Progress:     return 220.0f;
        case Step::ShowKey:      return 410.0f;
        case Step::Done:         return 240.0f;
        case Step::ResetApproving: return 340.0f;
        case Step::Choose:       return has_other_device_ ? 398.0f : 324.0f;
        case Step::LostAccess:   return 300.0f;
        case Step::WaitingForOtherDevice: return 270.0f;
        case Step::IncomingRequest: return 250.0f;
        case Step::CompareEmoji: return 390.0f;
        case Step::Confirming:   return 220.0f;
        case Step::VerifyFailed: return 270.0f;
    }
    return 280.0f;
}

tk::Rect EncryptionSetupOverlay::card_bounds() const
{
    auto  b      = bounds();
    float card_h = step_card_height_();
    float cx     = b.x + (b.w - kCardW) * 0.5f;
    float cy     = b.y + (b.h - card_h) * 0.5f;
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
    auto        b   = bounds();
    auto&       c   = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Forget clickable regions from the previous frame so a control that is
    // not drawn this step can't be hit-tested with a stale rect. The child
    // action buttons are hidden up front and re-shown by the steps that use
    // them, so a vanished button is skipped by hit-testing too.
    secondary_link_ = passphrase_link_ = back_link_ =
        checkbox_rect_ = device_row_ = key_row_ = lost_link_ = reject_link_ = {};
    primary_enabled_ = true;
    if (primary_button_) primary_button_->set_visible(false);
    if (copy_button_) copy_button_->set_visible(false);
    if (save_button_) save_button_->set_visible(false);
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
    const bool key_field_wanted        = step_ == Step::EnterKey;
    const bool passphrase_fields_wanted = step_ == Step::PassphraseEntry;
    if (!passphrase_fields_wanted)
    {
        if (passphrase_field_) passphrase_field_->set_visible(false);
        if (passphrase_confirm_field_) passphrase_confirm_field_->set_visible(false);
    }
    if (key_field_ && !key_field_wanted)
        key_field_->set_visible(false);

    // Position, style, and paint the reusable primary button for the current
    // step. The label/enabled state are refreshed every frame; widths come from
    // button_width() so they match the old hand-painted geometry exactly.
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    auto place_primary = [&](tk::Rect r, const std::string& label, bool enabled)
    {
        if (!primary_button_) return;
        primary_button_->set_visible(true);
        if (primary_button_->label() != label) primary_button_->set_label(label);
        primary_button_->set_enabled(enabled);
        primary_button_->arrange(lc, r);
        primary_button_->paint(ctx);
    };
    auto place_button = [&](tk::Button* btn, tk::Rect r)
    {
        if (!btn) return;
        btn->set_visible(true);
        btn->arrange(lc, r);
        btn->paint(ctx);
    };
    auto place_field = [&](tk::TextField* field, tk::Rect fr)
    {
        if (fr.empty()) return;
        c.fill_rounded_rect(fr, kBtnRad, pal.bg);
        c.stroke_rounded_rect(fr, kBtnRad, pal.border, 1.0f);
        if (!field) return;
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
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - progress_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(c, {card.x + card.w * 0.5f, y}, phase,
                              /*radius=*/16.0f, /*dot_r=*/3.0f, pal.accent);
        if (host()) host()->request_repaint();
    };
    auto place_right_primary = [&](const std::string& label, bool enabled)
    {
        float w = button_width(ctx, label);
        place_primary({card.x + card.w - kCardPad - w, by, w, kEncryptionSetupBtnH},
                      label, enabled);
    };
    // A bare link whose text starts at the content's left edge.
    auto left_link = [&](float y, const std::string& label) -> tk::Rect
    {
        tk::Rect r{cx - kEncryptionSetupBtnHPad, y, button_width(ctx, label),
                   kEncryptionSetupBtnH};
        paint_link(ctx, r, label);
        return r;
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

            // Option rows: a bordered card each, title + one-line hint.
            constexpr float kRowH = 64.0f;
            float row_y = card.y + 128.0f;
            auto draw_row = [&](const std::string& title, const std::string& hint,
                                bool recommended) -> tk::Rect
            {
                tk::Rect r{cx, row_y, cw, kRowH};
                c.fill_rounded_rect(r, kBtnRad, pal.bg);
                c.stroke_rounded_rect(r, kBtnRad, recommended ? pal.accent : pal.border,
                                      recommended ? 1.5f : 1.0f);
                tk::TextStyle ts;
                ts.role = tk::FontRole::SidebarName;
                if (auto lo = ctx.factory.build_text(title, ts))
                    c.draw_text(*lo, {r.x + 14.0f, r.y + 11.0f}, pal.text_primary);
                tk::TextStyle hs;
                hs.role      = tk::FontRole::Caption;
                hs.trim      = tk::TextTrim::Ellipsis;
                hs.max_width = r.w - 48.0f;
                if (auto lo = ctx.factory.build_text(hint, hs))
                    c.draw_text(*lo, {r.x + 14.0f, r.y + 35.0f}, pal.text_secondary);
                chevron_icon_.draw(c, ctx.factory, kChevronRightSvg,
                                   {r.x + r.w - 14.0f - kChevronPx, r.y, kChevronPx, r.h},
                                   kChevronPx, pal.text_muted);
                row_y += kRowH + 10.0f;
                return r;
            };
            if (has_other_device_)
                device_row_ = draw_row(tk::tr("Use another device"),
                                       tk::tr("Approve from a device where you're "
                                              "already signed in"),
                                       true);
            key_row_ = draw_row(tk::tr("Enter recovery key"),
                                tk::tr("Or the passphrase you chose instead"),
                                !has_other_device_);

            lost_link_ = left_link(row_y - 2.0f,
                                   tk::tr("I've lost my recovery key and other devices"));

            secondary_link_ = left_link(by, tk::tr("Skip for now"));
            break;
        }

        // ── LostAccess (Recover) ─────────────────────────────────────────────
        case Step::LostAccess:
        {
            draw_title(tk::tr("Reset encryption?"));
            paint_paragraph(
                ctx, {cx, content_y, cw, 0},
                tk::tr("If you've lost both your recovery key and every other "
                       "signed-in device, you can start over with a new recovery "
                       "key. Messages you can't read now will stay unreadable, "
                       "and people you chat with will see that your security "
                       "details changed."),
                tk::FontRole::Body, pal.text_secondary);
            place_right_primary(tk::tr("Reset encryption"), true);
            back_link_ = left_link(by, tk::tr("Back"));
            break;
        }

        // ── WaitingForOtherDevice ────────────────────────────────────────────
        case Step::WaitingForOtherDevice:
        {
            const bool we_asked = mode_ == Mode::Recover || can_retry_;
            draw_title(we_asked ? tk::tr("Check your other device")
                                : tk::tr("Starting verification\xe2\x80\xa6"));
            if (we_asked)
                paint_paragraph(ctx, {cx, content_y, cw, 0},
                                tk::tr("Open Tesseract (or another Matrix app) on a "
                                       "device where you're signed in and accept "
                                       "the request."),
                                tk::FontRole::Body, pal.text_secondary);
            draw_spinner(by - 40.0f);
            place_right_primary(tk::tr("Cancel"), true);
            break;
        }

        // ── IncomingRequest ──────────────────────────────────────────────────
        case Step::IncomingRequest:
        {
            draw_title(incoming_own_device_ ? tk::tr("Was this you?")
                                            : tk::tr("Verification request"));
            const std::string body =
                incoming_own_device_
                    ? tk::trf(tk::tr("Your device \xe2\x80\x9c{0}\xe2\x80\x9d wants to "
                                     "confirm it's you so it can read your "
                                     "encrypted messages."),
                              {peer_})
                    : tk::trf(tk::tr("{0} wants to verify you. You'll compare emoji "
                                     "to make sure you're talking to the right "
                                     "person."),
                              {peer_});
            paint_paragraph(ctx, {cx, content_y, cw, 0}, body, tk::FontRole::Body,
                            pal.text_secondary);
            place_right_primary(tk::tr("Continue"), true);
            reject_link_ = left_link(by, incoming_own_device_ ? tk::tr("Not me")
                                                              : tk::tr("Decline"));
            break;
        }

        // ── CompareEmoji ─────────────────────────────────────────────────────
        case Step::CompareEmoji:
        {
            draw_title(tk::tr("Compare emoji"));
            content_y += paint_paragraph(
                ctx, {cx, content_y, cw, 0},
                tk::tr("Check that the other device shows the same emoji in the "
                       "same order."),
                tk::FontRole::Body, pal.text_secondary);
            paint_sas_emoji_grid(ctx, {cx, content_y + 14.0f, cw, sas_emoji_grid_height()},
                                 emojis_);
            place_right_primary(tk::tr("They match"), true);
            reject_link_ = left_link(by, tk::tr("They don't match"));
            break;
        }

        // ── Confirming ───────────────────────────────────────────────────────
        case Step::Confirming:
        {
            const float scy = card.y + card.h * 0.42f;
            draw_spinner(scy);
            tk::TextStyle st;
            st.role = tk::FontRole::Body;
            if (auto lo = ctx.factory.build_text(tk::tr("Confirming\xe2\x80\xa6"), st))
            {
                tk::Size sz = lo->measure();
                c.draw_text(*lo, {card.x + (card.w - sz.w) * 0.5f, scy + 34.0f},
                            pal.text_secondary);
            }
            break;
        }

        // ── VerifyFailed ─────────────────────────────────────────────────────
        case Step::VerifyFailed:
        {
            draw_title(tk::tr("Couldn't confirm"));
            paint_paragraph(ctx, {cx, content_y, cw, 0}, error_msg_, tk::FontRole::Body,
                            pal.text_secondary);
            place_right_primary(can_retry_ ? tk::tr("Try again") : tk::tr("Close"), true);
            if (mode_ != Mode::Verify)
                back_link_ = left_link(by, tk::tr("Back"));
            break;
        }

        // ── Intro ────────────────────────────────────────────────────────────
        case Step::Intro:
        {
            const bool fresh = mode_ == Mode::Fresh;
            draw_title(fresh ? tk::tr("Protect your messages")
                             : tk::tr("Confirm it's you"));
            const std::string body =
                fresh
                    ? tk::tr("Your messages are end-to-end encrypted. A recovery "
                             "key lets you read them again if you sign in on a "
                             "new device or lose this one.")
                    : tk::tr("To read your encrypted messages on this device, "
                             "confirm it's you with your recovery key or with "
                             "another device where you're signed in.");
            content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, body,
                                         tk::FontRole::Body, pal.text_secondary);

            // Inline error (e.g. enable_recovery failed) — shown so the user
            // sees why setup bounced back here.
            if (!error_msg_.empty())
                paint_paragraph(ctx, {cx, content_y + 10.0f, cw, 0}, error_msg_,
                                tk::FontRole::Small, pal.destructive);

            // Advanced option: a passphrase the user picks, instead of a
            // generated key. Deliberately a quiet link, not an upfront choice.
            if (fresh)
            {
                const std::string pl = tk::tr("Use a passphrase instead");
                passphrase_link_ = {cx - kEncryptionSetupBtnHPad,
                                    by - kEncryptionSetupBtnH - 6.0f,
                                    button_width(ctx, pl), kEncryptionSetupBtnH};
                paint_link(ctx, passphrase_link_, pl);
            }

            const std::string prim =
                fresh ? tk::tr("Create recovery key") : tk::tr("Continue");
            float pw = button_width(ctx, prim);
            place_primary({card.x + card.w - kCardPad - pw, by, pw, kEncryptionSetupBtnH},
                          prim, true);

            const std::string skip = tk::tr("Skip for now");
            float             sw   = button_width(ctx, skip);
            secondary_link_        = {cx - kEncryptionSetupBtnHPad, by, sw,
                                      kEncryptionSetupBtnH};
            paint_link(ctx, secondary_link_, skip);
            break;
        }

        // ── PassphraseEntry (Fresh only) ─────────────────────────────────────
        case Step::PassphraseEntry:
        {
            draw_title(tk::tr("Choose a passphrase"));
            paint_paragraph(ctx, {cx, content_y, cw, 0},
                            tk::tr("Pick something memorable that you don't use "
                                   "anywhere else."),
                            tk::FontRole::Body, pal.text_secondary);

            tk::Rect f1 = passphrase_field_rect_value();
            tk::Rect f2 = passphrase_confirm_field_rect_value();
            paint_paragraph(ctx, {cx, f1.y - 20.0f, cw, 0}, tk::tr("Passphrase"),
                            tk::FontRole::Small, pal.text_muted);
            place_field(passphrase_field_, f1);
            paint_paragraph(ctx, {cx, f2.y - 20.0f, cw, 0},
                            tk::tr("Confirm passphrase"),
                            tk::FontRole::Small, pal.text_muted);
            place_field(passphrase_confirm_field_, f2);

            // Mismatch hint only once the confirmation has been typed, so the
            // user isn't scolded before they've had a chance.
            const std::string confirm =
                passphrase_confirm_field_ ? passphrase_confirm_field_->text()
                                          : std::string();
            std::string hint = error_msg_;
            if (hint.empty() && !confirm.empty() && !passphrases_match_())
                hint = tk::tr("The passphrases don't match.");
            if (!hint.empty())
                paint_paragraph(ctx, {cx, f2.y + f2.h + 10.0f, cw, 0}, hint,
                                tk::FontRole::Small, pal.destructive);

            primary_enabled_ = passphrases_match_();
            const std::string cont = tk::tr("Continue");
            float cwid = button_width(ctx, cont);
            place_primary({card.x + card.w - kCardPad - cwid, by, cwid, kEncryptionSetupBtnH},
                          cont, primary_enabled_);

            const std::string back = tk::tr("Back");
            back_link_ = {cx - kEncryptionSetupBtnHPad, by, button_width(ctx, back),
                          kEncryptionSetupBtnH};
            paint_link(ctx, back_link_, back);
            break;
        }

        // ── EnterKey (Recover only) ──────────────────────────────────────────
        case Step::EnterKey:
        {
            draw_title(tk::tr("Enter your recovery key"));
            paint_paragraph(ctx, {cx, card.y + 92.0f, cw, 0},
                            tk::tr("If you set a passphrase instead, enter that."),
                            tk::FontRole::Small, pal.text_muted);

            place_field(key_field_, key_field_rect_value()); // card.y + 120

            if (!error_msg_.empty())
                paint_paragraph(ctx, {cx, card.y + 168.0f, cw, 0}, error_msg_,
                                tk::FontRole::Small, pal.destructive);

            back_link_ = left_link(by, tk::tr("Back"));

            const std::string key = key_field_ ? key_field_->text() : std::string();
            primary_enabled_ = !key.empty();
            const std::string verify = tk::tr("Unlock");
            float vw = button_width(ctx, verify);
            place_primary({card.x + card.w - kCardPad - vw, by, vw, kEncryptionSetupBtnH},
                          verify, primary_enabled_);
            break;
        }

        // ── Progress ─────────────────────────────────────────────────────────
        case Step::Progress:
        {
            const float scx = card.x + card.w * 0.5f;
            const float scy = card.y + card.h * 0.38f;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - progress_start_)
                    .count();
            const float phase =
                static_cast<float>(elapsed_ms % 1000) / 1000.0f;
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
            if (host()) host()->request_repaint(); // self-drive the spinner
            break;
        }

        // ── ShowKey (Fresh only) ─────────────────────────────────────────────
        case Step::ShowKey:
        {
            draw_title(tk::tr("Save your recovery key"));
            content_y += paint_paragraph(
                ctx, {cx, content_y, cw, 0},
                tk::tr("Keep it somewhere safe, like a password manager. You'll "
                       "need it to read your messages if you sign in on a new "
                       "device."),
                tk::FontRole::Body, pal.text_secondary);
            content_y += 14.0f;

            // Key box, grouped in fours so it can be read out / compared.
            tk::Rect box{cx, content_y, cw, 58.0f};
            c.fill_rounded_rect(box, kBtnRad, pal.bg);
            c.stroke_rounded_rect(box, kBtnRad, pal.border, 1.0f);
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Body;
                st.wrap      = true;
                st.max_width = box.w - 24.0f;
                auto lo = ctx.factory.build_text(format_recovery_key(recovery_key_), st);
                if (lo)
                    c.draw_text(*lo, {box.x + 12.0f, box.y + 10.0f},
                                pal.text_primary);
            }

            // Copy (+ Save to file… when the shell offers it) below the box,
            // right-aligned.
            const float row_y = box.y + box.h + 10.0f;
            float right = card.x + card.w - kCardPad;
            if (copy_button_)
            {
                if (copy_button_->label() != tk::tr("Copy"))
                    copy_button_->set_label(tk::tr("Copy"));
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
                st.role      = tk::FontRole::Small;
                st.wrap      = true;
                st.max_width = std::max(0.0f, right - cx);
                auto lo = ctx.factory.build_text(save_status_, st);
                if (lo)
                    c.draw_text(*lo,
                                {cx, row_y + (kEncryptionSetupBtnH - lo->measure().h) * 0.5f},
                                save_failed_ ? pal.destructive : pal.text_muted);
            }

            // "I've saved my recovery key" checkbox.
            const float cb_box = 18.0f;
            checkbox_rect_ = {cx, row_y + kEncryptionSetupBtnH + 12.0f, cw, 24.0f};
            tk::Rect box2{checkbox_rect_.x,
                          checkbox_rect_.y + (checkbox_rect_.h - cb_box) * 0.5f,
                          cb_box, cb_box};
            if (key_saved_checked_)
            {
                c.fill_rounded_rect(box2, 4.0f, pal.accent);
                tk::TextStyle st;
                st.role = tk::FontRole::UiSemibold;
                auto lo = ctx.factory.build_text("\xE2\x9C\x93", st);
                if (lo)
                {
                    tk::Size sz = lo->measure();
                    c.draw_text(*lo,
                                {box2.x + (box2.w - sz.w) * 0.5f,
                                 box2.y + (box2.h - sz.h) * 0.5f},
                                pal.text_on_accent);
                }
            }
            else
            {
                c.stroke_rounded_rect(box2, 4.0f, pal.border, 1.5f);
            }
            {
                tk::TextStyle st;
                st.role = tk::FontRole::Body;
                auto lo = ctx.factory.build_text(tk::tr("I've saved my recovery key"), st);
                if (lo)
                    c.draw_text(*lo,
                                {box2.x + cb_box + 10.0f,
                                 checkbox_rect_.y +
                                     (checkbox_rect_.h - lo->measure().h) * 0.5f},
                                pal.text_primary);
            }

            primary_enabled_ = key_saved_checked_;
            const std::string cont = tk::tr("Continue");
            float cwid = button_width(ctx, cont);
            place_primary({card.x + card.w - kCardPad - cwid, by, cwid, kEncryptionSetupBtnH},
                          cont, primary_enabled_);
            break;
        }

        // ── Done ─────────────────────────────────────────────────────────────
        case Step::Done:
        {
            // Accent checkmark disc.
            const float disc = 56.0f;
            tk::Rect    dr{card.x + (card.w - disc) * 0.5f, card.y + 40.0f,
                        disc, disc};
            c.fill_rounded_rect(dr, disc * 0.5f, pal.accent);
            {
                tk::TextStyle st;
                st.role = tk::FontRole::Title;
                auto lo = ctx.factory.build_text("\xE2\x9C\x93", st);
                if (lo)
                {
                    tk::Size sz = lo->measure();
                    c.draw_text(*lo,
                                {dr.x + (dr.w - sz.w) * 0.5f,
                                 dr.y + (dr.h - sz.h) * 0.5f},
                                pal.text_on_accent);
                }
            }
            std::string body;
            switch (done_kind_)
            {
                case DoneKind::Protected:
                    body = tk::tr("You're all set. Your messages are protected.");
                    break;
                case DoneKind::Unlocked:
                    body = tk::tr("You're all set. Your encrypted messages are "
                                  "available on this device.");
                    break;
                case DoneKind::OtherDeviceConfirmed:
                    body = tk::tr("Your other device is confirmed and can now read "
                                  "your encrypted messages.");
                    break;
                case DoneKind::UserVerified:
                    body = tk::tr("Verified. You can trust you're talking to the "
                                  "right person.");
                    break;
            }
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Body;
                st.wrap      = true;
                st.max_width = cw;
                st.halign    = tk::TextHAlign::Center;
                auto lo      = ctx.factory.build_text(body, st);
                if (lo)
                {
                    tk::Size sz = lo->measure();
                    c.draw_text(*lo,
                                {card.x + (card.w - sz.w) * 0.5f,
                                 dr.y + dr.h + 18.0f},
                                pal.text_secondary);
                }
            }
            const std::string close = tk::tr("Close");
            float clw = button_width(ctx, close);
            place_primary({card.x + (card.w - clw) * 0.5f, by, clw, kEncryptionSetupBtnH},
                          close, true);
            break;
        }

        // ── ResetApproving (cross-signing reset awaiting browser approval) ───
        case Step::ResetApproving:
        {
            draw_title(tk::tr("Resetting encryption"));

            // The approval page acts on whichever account the *browser* is
            // signed in as — nothing in the server-issued link pins it to
            // ours — so name the account, offer the link for a private
            // window, and nudge if approval never arrives.
            const bool failed = !error_msg_.empty();
            std::string body;
            if (failed)
                body = error_msg_;
            else if (reset_account_.empty())
                body = tk::tr("Approve the reset in the browser window that just "
                              "opened. This will continue once you've confirmed.");
            else
                body = tk::trf(tk::tr("Approve the reset as {0} in the browser window "
                                      "that just opened. If the browser is signed in "
                                      "as a different account, sign out there first, "
                                      "or copy the link into a private window."),
                               {reset_account_});
            content_y += paint_paragraph(ctx, {cx, content_y, cw, 0}, body,
                                         tk::FontRole::Body,
                                         failed ? pal.destructive : pal.text_secondary);

            // Spinner (only while waiting; hidden once an error is shown).
            if (!failed)
            {
                const float scy = content_y + 34.0f;
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - progress_start_)
                        .count();
                const float phase =
                    static_cast<float>(elapsed_ms % 1000) / 1000.0f;
                tk::draw_spinner_dots(c, {card.x + card.w * 0.5f, scy}, phase,
                                      /*radius=*/16.0f, /*dot_r=*/3.0f, pal.accent);
                if (host()) host()->request_repaint(); // self-drive the spinner

                if (elapsed_ms >= kResetHintAfterMs && !reset_account_.empty())
                    paint_paragraph(
                        ctx, {cx, scy + 30.0f, cw, 0},
                        tk::trf(tk::tr("Still waiting? Check that you approved it as "
                                       "{0}, not another account."),
                                {reset_account_}),
                        tk::FontRole::Small, pal.text_muted);

                // Copy the approval link (for a private window / other
                // profile signed in as the right account).
                if (!reset_approval_url_.empty() && copy_button_)
                {
                    const std::string copy_lbl =
                        link_copied_ ? tk::tr("Link copied") : tk::tr("Copy link");
                    if (copy_button_->label() != copy_lbl) copy_button_->set_label(copy_lbl);
                    place_button(copy_button_, {cx - kEncryptionSetupBtnHPad, by,
                                                button_width(ctx, copy_lbl),
                                                kEncryptionSetupBtnH});
                }
            }

            // Cancel (or Close once an error is shown), bottom-right.
            const std::string lbl = failed ? tk::tr("Close") : tk::tr("Cancel");
            float lw = button_width(ctx, lbl);
            place_primary({card.x + card.w - kCardPad - lw, by, lw, kEncryptionSetupBtnH}, lbl,
                          true);
            break;
        }
    }
}

// ── Pointer handling ──────────────────────────────────────────────────────────

bool EncryptionSetupOverlay::on_pointer_down(tk::Point local)
{
    if (!visible()) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    press_secondary_ = press_back_ = press_checkbox_ =
        press_passphrase_ = press_device_row_ = press_key_row_ = press_lost_ =
            press_reject_ = backdrop_press_ = false;

    // During Progress / Confirming the operation is in flight: swallow every
    // click.
    if (step_ == Step::Progress || step_ == Step::Confirming) return true;

    // During ResetApproving only the Cancel/Close child button acts; swallow
    // backdrop / stray clicks so the wait can't be dismissed without aborting.
    if (step_ == Step::ResetApproving) return true;

    // The filled Primary / Copy / Save buttons are tk::Button children; the
    // host dispatches their presses directly, so they never reach this
    // handler. We only track the bare links, the checkbox, and the backdrop.
    if (rect_contains(secondary_link_, w))
        press_secondary_ = true;
    else if (rect_contains(passphrase_link_, w))
        press_passphrase_ = true;
    else if (rect_contains(back_link_, w))
        press_back_ = true;
    else if (rect_contains(checkbox_rect_, w))
        press_checkbox_ = true;
    else if (rect_contains(device_row_, w))
        press_device_row_ = true;
    else if (rect_contains(key_row_, w))
        press_key_row_ = true;
    else if (rect_contains(lost_link_, w))
        press_lost_ = true;
    else if (rect_contains(reject_link_, w))
        press_reject_ = true;
    else if (!rect_contains(card_bounds(), w))
        backdrop_press_ = true;

    return true; // modal: always consume
}

void EncryptionSetupOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    auto hit = [&](const tk::Rect& r) { return inside_self && rect_contains(r, w); };

    // Primary / Copy / Save are tk::Button children and fire their own
    // on_click; this handler only runs for the bare links, checkbox, and
    // backdrop dismiss.
    if (press_secondary_ && hit(secondary_link_))
    {
        if (on_close) on_close();
    }
    else if (press_passphrase_ && hit(passphrase_link_))
    {
        passphrase_mode_ = true;
        advance_step_(Step::PassphraseEntry);
    }
    else if (press_back_ && hit(back_link_))
    {
        passphrase_mode_ = false;
        if (step_ == Step::VerifyFailed)
            return_to_start(); // honours where an incoming request interrupted
        else
            advance_step_(mode_ == Mode::Fresh ? Step::Intro : Step::Choose);
    }
    else if (press_device_row_ && hit(device_row_))
        choose_other_device_();
    else if (press_key_row_ && hit(key_row_))
        advance_step_(Step::EnterKey);
    else if (press_lost_ && hit(lost_link_))
        advance_step_(Step::LostAccess);
    else if (press_reject_ && hit(reject_link_))
        reject_();
    else if (press_checkbox_ && hit(checkbox_rect_))
        key_saved_checked_ = !key_saved_checked_;
    else if (backdrop_press_ && backdrop_closes_() &&
             !rect_contains(card_bounds(), w))
    {
        if (on_close) on_close();
    }

    press_secondary_ = press_back_ = press_checkbox_ =
        press_passphrase_ = press_device_row_ = press_key_row_ = press_lost_ =
            press_reject_ = backdrop_press_ = false;

    if (host()) host()->request_repaint();
}

} // namespace tesseract::views
