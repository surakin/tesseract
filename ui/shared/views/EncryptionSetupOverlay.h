#pragma once

#include "tk/widget.h"
#include "tk/canvas.h"
#include "tk/controls.h"

#include <chrono>
#include <functional>
#include <string>

namespace tesseract::views
{

class EncryptionSetupOverlay : public tk::Widget
{
public:
    enum class Mode { Fresh, Recover };
    enum class Step {
        Intro,
        ChooseMethod,
        EnterKey,
        Progress,
        ShowKey,
        Done,
        // Cross-signing reset: waiting for the user to approve the reset in
        // their browser. On success the overlay advances into the Fresh
        // recovery-key flow (ChooseMethod → …).
        ResetApproving,
    };

    explicit EncryptionSetupOverlay(Mode mode);

    // ── Callbacks wired by ShellBase ──────────────────────────────────────
    std::function<void()>              on_close;
    std::function<void(std::string)>   on_enable_recovery; // passphrase or ""
    std::function<void(std::string)>   on_recover;         // key or passphrase
    std::function<void()>              on_request_sas;
    std::function<void(std::string)>   on_copy_to_clipboard;
    // Fired when the user clicks Cancel on the ResetApproving step (aborts an
    // in-progress cross-signing reset).
    std::function<void()>              on_cancel_reset;

    // Fired when a step / mode change alters which native text field should be
    // visible (e.g. Intro→EnterKey, or toggling the passphrase option). The
    // shell must respond by relaying out its surface so the on-layout pass
    // repositions/shows the NativeTextField. Same contract as
    // RoomView::on_layout_changed.
    std::function<void()>              on_layout_changed;

    // ── Host hooks (NativeTextField rects) ───────────────────────────────
    std::function<tk::Rect()>      passphrase_field_rect;
    std::function<bool()>          passphrase_field_visible;
    std::function<tk::Rect()>      key_field_rect;
    std::function<bool()>          key_field_visible;
    std::function<std::string()>   get_passphrase;
    std::function<std::string()>   get_key_input;

    // ── Driven by ShellBase after on_enable_recovery/on_recover fires ────
    void advance_progress(uint8_t step,
                          const std::string& recovery_key,
                          uint32_t backed_up,
                          uint32_t total);

    // ── Cross-signing reset flow (driven by ShellBase) ───────────────────
    // Enter the "approve in your browser…" wait step. The caller should have
    // already reset(Mode::Fresh) + re-wired callbacks so the post-approval
    // hand-off lands in the Fresh recovery-key flow.
    void begin_reset_wait() { advance_step_(Step::ResetApproving); }
    // The reset was approved — start the Fresh recovery-key setup.
    void reset_approved()   { advance_step_(Step::ChooseMethod); }
    // The reset failed / was cancelled server-side; show the error and turn
    // the button into a Close.
    void report_reset_error(const std::string& msg)
    {
        error_msg_ = msg;
    }

    // ── Accessors ─────────────────────────────────────────────────────────
    Step        step()          const { return step_; }
    Mode        mode()          const { return mode_; }
    std::string recovery_key()  const { return recovery_key_; }
    std::string error_msg()     const { return error_msg_; }
    std::string progress_label()const { return progress_label_; }

    bool     passphrase_field_rect_visible() const;
    tk::Rect passphrase_field_rect_value()   const;
    bool     key_field_rect_visible()        const;
    tk::Rect key_field_rect_value()          const;

    // ── tk::Widget interface ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size avail) override;
    void     arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;
    bool     on_pointer_down(tk::Point world) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // Reset mode and step so the overlay can be reused for a different mode.
    void reset(Mode mode)
    {
        mode_                = mode;
        step_                = Step::Intro;
        recovery_key_.clear();
        error_msg_.clear();
        progress_label_.clear();
        key_saved_checked_   = false;
        passphrase_mode_     = false;
        // Clear all callbacks so the caller can re-wire fresh ones.
        on_close             = {};
        on_enable_recovery   = {};
        on_recover           = {};
        on_request_sas       = {};
        on_copy_to_clipboard = {};
        on_cancel_reset      = {};
        on_layout_changed    = {};
        passphrase_field_rect    = {};
        passphrase_field_visible = {};
        key_field_rect       = {};
        key_field_visible    = {};
        get_passphrase       = {};
        get_key_input        = {};
    }

    // ── Test helpers ──────────────────────────────────────────────────────
    void simulate_primary_action();
    void simulate_skip();
    void simulate_select_passphrase_mode();
    void simulate_check_key_saved();
    void simulate_sas_link();

private:
    void     advance_step_(Step next);
    void     fire_primary_();
    tk::Rect card_bounds() const;
    float    step_card_height_() const;
    void     update_progress_label_(uint8_t step, uint32_t backed_up, uint32_t total);

    Mode        mode_;
    Step        step_            = Step::Intro;
    std::string recovery_key_;
    std::string error_msg_;
    std::string progress_label_;
    bool        key_saved_checked_  = false;
    bool        passphrase_mode_    = false;

    // The filled action buttons are child tk::Button widgets (positioned +
    // styled per-step in paint()). primary_button_ is reused across every step
    // that has a bottom-right action (Continue / Verify / Close / …);
    // copy_button_ appears only on the ShowKey step.
    tk::Button* primary_button_ = nullptr;
    tk::Button* copy_button_    = nullptr;

    // ── Layout rects computed during paint(), hit-tested in pointer handlers ──
    // These cover the non-button affordances only (text links, the two toggle
    // pills, and the "I've saved this key" checkbox).
    tk::Rect secondary_link_{};   // "Skip for now" / "Close"
    tk::Rect back_link_{};
    tk::Rect sas_link_{};
    tk::Rect checkbox_rect_{};
    tk::Rect key_toggle_rect_{};
    tk::Rect pass_toggle_rect_{};
    bool     primary_enabled_ = true;  // recomputed per-step in paint()

    // ── Press tracking (mirror ImageViewerOverlay) ──────────────────────────
    bool press_secondary_   = false;
    bool press_back_        = false;
    bool press_sas_         = false;
    bool press_checkbox_    = false;
    bool press_key_toggle_  = false;
    bool press_pass_toggle_ = false;
    bool backdrop_press_    = false;

    // Captured in paint() so pointer handlers can schedule a relayout+repaint
    // (toggling passphrase mode must refresh the native field's visibility).
    tk::Host* host_ = nullptr;

    // Spinner animation clock; reset when the Progress step is entered.
    std::chrono::steady_clock::time_point progress_start_{};
};

} // namespace tesseract::views
