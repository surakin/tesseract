#include "views/EncryptionSetupOverlay.h"
#include "tk/canvas.h"
#include "tk/theme.h"

#include <string>

namespace tesseract::views
{

namespace
{
constexpr float kCardW   = 480.0f;
constexpr float kCardPad = 32.0f;
} // namespace

EncryptionSetupOverlay::EncryptionSetupOverlay(Mode mode)
    : mode_(mode)
{
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
            std::string pass = passphrase_mode_ ? passphrase_input_ : "";
            advance_step_(Step::Progress);
            if (on_enable_recovery)
                on_enable_recovery(std::move(pass));
            break;
        }

        case Step::EnterKey:
            if (key_input_.empty()) return;
            {
                std::string key = key_input_;
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

tk::Rect EncryptionSetupOverlay::card_bounds() const
{
    auto b      = bounds();
    float card_h = 280.0f;
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
    auto b   = bounds();
    auto& c  = ctx.canvas;
    auto& th = ctx.theme;

    // Dim backdrop.
    c.fill_rect(b, tk::Color{0, 0, 0, 128});

    // Card background.
    auto card = card_bounds();
    c.fill_rect(card, th.palette.chrome_bg);

    // Title.
    const char* title = (mode_ == Mode::Fresh)
        ? "Secure your messages"
        : "Verify this device";
    tk::TextStyle st;
    st.role = tk::FontRole::Title;
    auto lay = ctx.factory.build_text(title, st);
    if (lay)
        c.draw_text(*lay, {card.x + kCardPad, card.y + kCardPad},
                    th.palette.text_primary);
}

bool EncryptionSetupOverlay::on_pointer_down(tk::Point /*world*/) { return visible(); }
void EncryptionSetupOverlay::on_pointer_up(tk::Point /*local*/, bool /*inside_self*/) {}

} // namespace tesseract::views
