#include "views/EncryptionSetupOverlay.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/loading_spinner.h"
#include "tk/theme.h"
#include "views/media_utils.h" // rect_contains

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
constexpr float kBtnH     = 38.0f;
constexpr float kBtnHPad  = 18.0f;
constexpr float kBtnRad   = 6.0f;
constexpr float kRowGap   = 12.0f;

// Width a text button needs to fit its single-line label plus side padding.
// Shared by the child tk::Button geometry and the bare text links so both keep
// the original hand-painted widths.
float button_width(tk::PaintCtx& ctx, const std::string& label)
{
    tk::TextStyle st;
    st.role = tk::FontRole::UiSemibold;
    auto lo = ctx.factory.build_text(label, st);
    return (lo ? lo->measure().w : 0.0f) + 2.0f * kBtnHPad;
}

// Fill (or, for links, don't) `r` and centre `label` inside it.
// A bare, background-less text link (accent-coloured, centred in `r`). Used for
// the "Skip for now" / "Back" / "Verify with another device" affordances — the
// filled action buttons are tk::Button children, not these.
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
    : mode_(mode)
{
    // Filled action buttons. They are positioned, labelled, and (for primary)
    // enabled/disabled per-step in paint(); both start hidden.
    auto prim = std::make_unique<tk::Button>(
        "", std::function<void()>{}, tk::Button::Variant::Primary);
    primary_button_ = add_child(std::move(prim));
    primary_button_->set_visible(false);
    primary_button_->set_on_click([this] { fire_primary_(); });

    auto copy = std::make_unique<tk::Button>(
        "Copy", std::function<void()>{}, tk::Button::Variant::Subtle);
    copy_button_ = add_child(std::move(copy));
    copy_button_->set_visible(false);
    copy_button_->set_on_click(
        [this] { if (on_copy_to_clipboard) on_copy_to_clipboard(recovery_key_); });
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
        advance_step_(mode_ == Mode::Fresh ? Step::ChooseMethod : Step::EnterKey);
        error_msg_ = std::move(msg);
    }
}

void EncryptionSetupOverlay::update_progress_label_(uint8_t step,
                                                     uint32_t backed_up,
                                                     uint32_t total)
{
    switch (step)
    {
        case 0: progress_label_ = "Starting\xe2\x80\xa6";                break;
        case 1: progress_label_ = "Creating backup\xe2\x80\xa6";         break;
        case 2: progress_label_ = "Generating recovery key\xe2\x80\xa6"; break;
        case 3:
            progress_label_ = "Backing up keys ("
                            + std::to_string(backed_up) + " / "
                            + std::to_string(total) + ")\xe2\x80\xa6";
            break;
        case 4: progress_label_ = "Done."; break;
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
            advance_step_(mode_ == Mode::Fresh ? Step::ChooseMethod : Step::EnterKey);
            break;

        case Step::ChooseMethod:
        {
            std::string pass = passphrase_mode_ ? get_passphrase() : "";
            advance_step_(Step::Progress);
            if (on_enable_recovery)
                on_enable_recovery(std::move(pass));
            break;
        }

        case Step::EnterKey:
            {
                std::string key = get_key_input();
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
    }
}

// ── Simulation helpers ────────────────────────────────────────────────────────

void EncryptionSetupOverlay::simulate_primary_action()         { fire_primary_(); }
void EncryptionSetupOverlay::simulate_skip()                    { if (on_close) on_close(); }
void EncryptionSetupOverlay::simulate_select_passphrase_mode()  { passphrase_mode_ = true; }
void EncryptionSetupOverlay::simulate_check_key_saved()         { key_saved_checked_ = true; }
void EncryptionSetupOverlay::simulate_sas_link()                { if (on_request_sas) on_request_sas(); }

// ── Field-rect accessors ──────────────────────────────────────────────────────

bool EncryptionSetupOverlay::passphrase_field_rect_visible() const
{
    return visible() && step_ == Step::ChooseMethod && passphrase_mode_;
}

tk::Rect EncryptionSetupOverlay::passphrase_field_rect_value() const
{
    if (!passphrase_field_rect_visible()) return {};
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 160.0f, card.w - 2.0f * kCardPad, 36.0f};
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
        case Step::Intro:        return 240.0f;
        case Step::ChooseMethod: return 320.0f;
        case Step::EnterKey:     return 300.0f;
        case Step::Progress:     return 220.0f;
        case Step::ShowKey:      return 340.0f;
        case Step::Done:         return 240.0f;
        case Step::ResetApproving: return 240.0f;
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

void EncryptionSetupOverlay::paint(tk::PaintCtx& ctx)
{
    auto        b   = bounds();
    auto&       c   = ctx.canvas;
    const auto& pal = ctx.theme.palette;
    host_           = ctx.host;

    // Forget clickable regions from the previous frame so a control that is
    // not drawn this step can't be hit-tested with a stale rect. The child
    // action buttons are hidden up front and re-shown by the steps that use
    // them, so a vanished button is skipped by hit-testing too.
    secondary_link_ = back_link_ = sas_link_ =
        checkbox_rect_ = key_toggle_rect_ = pass_toggle_rect_ = {};
    primary_enabled_ = true;
    if (primary_button_) primary_button_->set_visible(false);
    if (copy_button_) copy_button_->set_visible(false);

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
    auto place_copy = [&](tk::Rect r)
    {
        if (!copy_button_) return;
        copy_button_->set_visible(true);
        copy_button_->arrange(lc, r);
        copy_button_->paint(ctx);
    };

    // Dim backdrop + card.
    c.fill_rect(b, tk::Color{0, 0, 0, 128});
    auto card = card_bounds();
    c.fill_rounded_rect(card, kCardRad, pal.chrome_bg);

    const float cx = card.x + kCardPad;
    const float cw = card.w - 2.0f * kCardPad;
    const float by = card.y + card.h - kCardPad - kBtnH; // bottom button row

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

    switch (step_)
    {
        // ── Intro ────────────────────────────────────────────────────────────
        case Step::Intro:
        {
            draw_title(mode_ == Mode::Fresh ? "Secure your messages"
                                            : "Verify this device");
            const std::string body =
                mode_ == Mode::Fresh
                    ? "Set up end-to-end encryption so your messages stay "
                      "private and you can restore them on new devices."
                    : "This device needs to be verified before it can read "
                      "your encrypted messages.";
            paint_paragraph(ctx, {cx, content_y, cw, 0}, body,
                            tk::FontRole::Body, pal.text_secondary);

            const std::string prim =
                mode_ == Mode::Fresh ? "Set up encryption" : "Verify";
            float pw = button_width(ctx, prim);
            place_primary({card.x + card.w - kCardPad - pw, by, pw, kBtnH}, prim,
                          true);

            const std::string skip = "Skip for now";
            float             sw   = button_width(ctx, skip);
            secondary_link_        = {cx, by, sw, kBtnH};
            paint_link(ctx, secondary_link_, skip);
            break;
        }

        // ── ChooseMethod (Fresh only) ────────────────────────────────────────
        case Step::ChooseMethod:
        {
            draw_title("Secure your messages");
            paint_paragraph(ctx, {cx, content_y, cw, 0},
                            "Choose how to protect your recovery key.",
                            tk::FontRole::Body, pal.text_secondary);

            // Two toggle pills.
            const float pill_y = card.y + 90.0f;
            const float pill_h = 40.0f;
            const float pill_w = (cw - kRowGap) * 0.5f;
            key_toggle_rect_   = {cx, pill_y, pill_w, pill_h};
            pass_toggle_rect_  = {cx + pill_w + kRowGap, pill_y, pill_w, pill_h};
            auto draw_pill = [&](tk::Rect r, const std::string& label, bool sel)
            {
                if (sel)
                    c.fill_rounded_rect(r, kBtnRad, pal.accent);
                else
                    c.stroke_rounded_rect(r, kBtnRad, pal.border, 1.0f);
                tk::TextStyle st;
                st.role = tk::FontRole::Body;
                auto lo = ctx.factory.build_text(label, st);
                if (lo)
                {
                    tk::Size sz = lo->measure();
                    c.draw_text(*lo,
                                {r.x + (r.w - sz.w) * 0.5f,
                                 r.y + (r.h - sz.h) * 0.5f},
                                sel ? pal.text_on_accent : pal.text_primary);
                }
            };
            draw_pill(key_toggle_rect_, "Recovery key", !passphrase_mode_);
            draw_pill(pass_toggle_rect_, "Passphrase", passphrase_mode_);

            // Passphrase entry affordance (native field overlaid by the shell).
            if (passphrase_mode_)
            {
                paint_paragraph(ctx, {cx, card.y + 138.0f, cw, 0}, "Passphrase",
                                tk::FontRole::Small, pal.text_muted);
                tk::Rect fr = passphrase_field_rect_value();
                if (!fr.empty())
                {
                    c.fill_rounded_rect(fr, kBtnRad, pal.bg);
                    c.stroke_rounded_rect(fr, kBtnRad, pal.border, 1.0f);
                }
            }

            // Inline error (e.g. enable_recovery failed) — shown above the
            // action row so the user sees why setup bounced back to this step.
            if (!error_msg_.empty())
                paint_paragraph(ctx, {cx, by - 44.0f, cw, 0}, error_msg_,
                                tk::FontRole::Small, pal.destructive);

            const std::string cont = "Continue";
            float cwid = button_width(ctx, cont);
            place_primary({card.x + card.w - kCardPad - cwid, by, cwid, kBtnH},
                          cont, true);

            float bw   = button_width(ctx, "Back");
            back_link_ = {cx, by, bw, kBtnH};
            paint_link(ctx, back_link_, "Back");
            break;
        }

        // ── EnterKey (Recover only) ──────────────────────────────────────────
        case Step::EnterKey:
        {
            draw_title("Verify this device");
            paint_paragraph(ctx, {cx, card.y + 92.0f, cw, 0},
                            "Enter your recovery key or passphrase",
                            tk::FontRole::Small, pal.text_muted);

            tk::Rect fr = key_field_rect_value(); // card.y + 120
            if (!fr.empty())
            {
                c.fill_rounded_rect(fr, kBtnRad, pal.bg);
                c.stroke_rounded_rect(fr, kBtnRad, pal.border, 1.0f);
            }

            if (!error_msg_.empty())
                paint_paragraph(ctx, {cx, card.y + 168.0f, cw, 0}, error_msg_,
                                tk::FontRole::Small, pal.destructive);

            // "Verify with another device instead" link.
            const std::string sas = "Verify with another device instead";
            float             sw  = button_width(ctx, sas);
            sas_link_             = {cx, by, sw, kBtnH};
            paint_link(ctx, sas_link_, sas);

            const std::string key = get_key_input();
            primary_enabled_ = !key.empty();
            float vw = button_width(ctx, "Verify");
            place_primary({card.x + card.w - kCardPad - vw, by, vw, kBtnH},
                          "Verify", primary_enabled_);
            break;
        }

        // ── Progress ─────────────────────────────────────────────────────────
        case Step::Progress:
        {
            const float scx = card.x + card.w * 0.5f;
            const float scy = card.y + card.h * 0.42f;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - progress_start_)
                    .count();
            const float phase =
                static_cast<float>(elapsed_ms % 1000) / 1000.0f;
            tk::draw_spinner_dots(c, {scx, scy}, phase, /*radius=*/16.0f,
                                  /*dot_r=*/3.0f, pal.accent);
            // Status label centred below the spinner.
            if (!progress_label_.empty())
            {
                tk::TextStyle st;
                st.role = tk::FontRole::Body;
                auto lo = ctx.factory.build_text(progress_label_, st);
                if (lo)
                {
                    tk::Size sz = lo->measure();
                    c.draw_text(*lo,
                                {scx - sz.w * 0.5f, scy + 16.0f + 18.0f},
                                pal.text_secondary);
                }
            }
            if (host_) host_->request_repaint(); // self-drive the spinner
            break;
        }

        // ── ShowKey (Fresh only) ─────────────────────────────────────────────
        case Step::ShowKey:
        {
            draw_title("Save your recovery key");
            content_y += paint_paragraph(
                ctx, {cx, content_y, cw, 0},
                "Store this key somewhere safe. You'll need it to restore "
                "encrypted messages on a new device.",
                tk::FontRole::Body, pal.text_secondary);
            content_y += 14.0f;

            // Monospace-ish key box.
            tk::Rect box{cx, content_y, cw, 58.0f};
            c.fill_rounded_rect(box, kBtnRad, pal.bg);
            c.stroke_rounded_rect(box, kBtnRad, pal.border, 1.0f);
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Body;
                st.wrap      = true;
                st.max_width = box.w - 24.0f;
                auto lo      = ctx.factory.build_text(recovery_key_, st);
                if (lo)
                    c.draw_text(*lo, {box.x + 12.0f, box.y + 10.0f},
                                pal.text_primary);
            }

            // Copy button below the box, right-aligned.
            float copyw = button_width(ctx, "Copy");
            place_copy({card.x + card.w - kCardPad - copyw,
                        box.y + box.h + 10.0f, copyw, kBtnH});

            // "I've saved this key" checkbox.
            const float cb_box = 18.0f;
            checkbox_rect_ = {cx, card.y + 232.0f, cw, 24.0f};
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
                auto lo = ctx.factory.build_text("I've saved this key", st);
                if (lo)
                    c.draw_text(*lo,
                                {box2.x + cb_box + 10.0f,
                                 checkbox_rect_.y +
                                     (checkbox_rect_.h - lo->measure().h) * 0.5f},
                                pal.text_primary);
            }

            primary_enabled_ = key_saved_checked_;
            float cont = button_width(ctx, "Continue");
            place_primary({card.x + card.w - kCardPad - cont, by, cont, kBtnH},
                          "Continue", primary_enabled_);
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
            const std::string body =
                mode_ == Mode::Fresh
                    ? "Encryption is set up. Your messages are protected."
                    : "Device verified.";
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Body;
                st.wrap      = true;
                st.max_width = cw;
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
            float clw = button_width(ctx, "Close");
            place_primary({card.x + (card.w - clw) * 0.5f, by, clw, kBtnH},
                          "Close", true);
            break;
        }

        // ── ResetApproving (cross-signing reset awaiting browser approval) ───
        case Step::ResetApproving:
        {
            draw_title("Resetting your identity");

            const bool failed = !error_msg_.empty();
            const std::string body =
                failed
                    ? error_msg_
                    : "Approve the reset in the browser window that just "
                      "opened. This page will continue once you've confirmed.";
            paint_paragraph(ctx, {cx, content_y, cw, 0}, body,
                            tk::FontRole::Body,
                            failed ? pal.destructive : pal.text_secondary);

            // Spinner (only while waiting; hidden once an error is shown).
            if (!failed)
            {
                const float scx = card.x + card.w * 0.5f;
                const float scy = card.y + card.h * 0.55f;
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - progress_start_)
                        .count();
                const float phase =
                    static_cast<float>(elapsed_ms % 1000) / 1000.0f;
                tk::draw_spinner_dots(c, {scx, scy}, phase, /*radius=*/16.0f,
                                      /*dot_r=*/3.0f, pal.accent);
                if (host_) host_->request_repaint(); // self-drive the spinner
            }

            // Cancel (or Close once an error is shown), bottom-right.
            const std::string lbl = failed ? "Close" : "Cancel";
            float lw = button_width(ctx, lbl);
            place_primary({card.x + card.w - kCardPad - lw, by, lw, kBtnH}, lbl,
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

    press_secondary_ = press_back_ = press_sas_ =
        press_checkbox_ = press_key_toggle_ = press_pass_toggle_ =
            backdrop_press_ = false;

    // During Progress the operation is in flight: swallow every click.
    if (step_ == Step::Progress) return true;

    // During ResetApproving only the Cancel/Close child button acts; swallow
    // backdrop / stray clicks so the wait can't be dismissed without aborting.
    if (step_ == Step::ResetApproving) return true;

    // The filled Primary / Copy buttons are tk::Button children; the host
    // dispatches their presses directly, so they never reach this handler. We
    // only track the bare links, the toggle pills, the checkbox, and the
    // backdrop here.
    if (rect_contains(secondary_link_, w))
        press_secondary_ = true;
    else if (rect_contains(back_link_, w))
        press_back_ = true;
    else if (rect_contains(sas_link_, w))
        press_sas_ = true;
    else if (rect_contains(checkbox_rect_, w))
        press_checkbox_ = true;
    else if (rect_contains(key_toggle_rect_, w))
        press_key_toggle_ = true;
    else if (rect_contains(pass_toggle_rect_, w))
        press_pass_toggle_ = true;
    else if (!rect_contains(card_bounds(), w))
        backdrop_press_ = true; // click outside the card → dismiss

    return true; // modal: always consume
}

void EncryptionSetupOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    auto hit = [&](const tk::Rect& r) { return inside_self && rect_contains(r, w); };

    // Primary / Copy are tk::Button children and fire their own on_click; this
    // handler only runs for the bare links, toggle pills, checkbox, and
    // backdrop dismiss.
    if (press_secondary_ && hit(secondary_link_))
    {
        if (on_close) on_close();
    }
    else if (press_back_ && hit(back_link_))
        advance_step_(Step::Intro);
    else if (press_sas_ && hit(sas_link_))
    {
        if (on_request_sas) on_request_sas();
    }
    else if (press_checkbox_ && hit(checkbox_rect_))
        key_saved_checked_ = !key_saved_checked_;
    else if (press_key_toggle_ && hit(key_toggle_rect_))
    {
        passphrase_mode_ = false;
        if (on_layout_changed) on_layout_changed(); // hides passphrase field
    }
    else if (press_pass_toggle_ && hit(pass_toggle_rect_))
    {
        passphrase_mode_ = true;
        if (on_layout_changed) on_layout_changed(); // reveals passphrase field
    }
    else if (backdrop_press_ && step_ != Step::Progress &&
             !rect_contains(card_bounds(), w))
    {
        if (on_close) on_close();
    }

    press_secondary_ = press_back_ = press_sas_ =
        press_checkbox_ = press_key_toggle_ = press_pass_toggle_ =
            backdrop_press_ = false;

    // Relayout so native-field visibility (passphrase toggle) and button
    // states refresh immediately.
    if (host_) host_->request_repaint();
}

} // namespace tesseract::views
