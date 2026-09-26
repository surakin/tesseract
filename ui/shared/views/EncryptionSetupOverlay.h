#pragma once

#include "tk/widget.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/svg.h"
#include "tk/text_field.h"

#include <chrono>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <tesseract/types.h>

namespace tesseract::views
{

// The single place every encryption interaction happens: setting up a
// recovery key (Fresh), unlocking this device with a recovery key or another
// device (Recover), and answering verification requests from the user's
// other devices or other users (Verify). ShellBase drives it; there is no
// other verification UI (EncryptionReminderBanner only reopens this).
class EncryptionSetupOverlay : public tk::Widget
{
public:
    enum class Mode { Fresh, Recover, Verify };
    enum class Step {
        Intro,
        // Recover only: pick how to unlock (another device / recovery key /
        // lost everything).
        Choose,
        // Recover only: explains resetting encryption before starting it.
        LostAccess,
        // Fresh only, reached via the "Use a passphrase instead" link on
        // Intro: pick + confirm a passphrase instead of a generated key.
        PassphraseEntry,
        EnterKey,
        Progress,
        ShowKey,
        Done,
        // Cross-signing reset: waiting for the user to approve the reset in
        // their browser. On success the overlay advances into the Fresh
        // recovery-key flow (Intro → …).
        ResetApproving,
        // ── Interactive (SAS emoji) verification ──
        WaitingForOtherDevice, // request sent / accepted, waiting for keys
        IncomingRequest,       // "Was this you?"
        CompareEmoji,
        Confirming,            // "They match" pressed, waiting for the SDK
        VerifyFailed,
    };
    // What the Done step congratulates the user on.
    enum class DoneKind {
        Protected,            // Fresh setup finished
        Unlocked,             // this device can now read encrypted messages
        OtherDeviceConfirmed, // we confirmed another of the user's devices
        UserVerified,         // we verified (or were verified by) another user
    };

protected:
    // host() is nullable: when null, the passphrase/key fields are simply not
    // constructed — lets tests that don't care about a native field
    // default-construct without a Host.
    explicit EncryptionSetupOverlay(Mode mode);
    TK_WIDGET_FACTORY_FRIEND(EncryptionSetupOverlay)

public:
    // ── Callbacks wired by ShellBase ──────────────────────────────────────
    std::function<void()>              on_close;
    std::function<void(std::string)>   on_enable_recovery; // passphrase or ""
    std::function<void(std::string)>   on_recover;         // key or passphrase
    std::function<void()>              on_request_sas;
    std::function<void(std::string)>   on_copy_to_clipboard;
    // Optional: when set, ShowKey offers a "Save to file…" button that fires
    // this with the recovery key. The shell reports the outcome back through
    // key_save_result().
    std::function<void(std::string)>   on_save_to_file;
    // Fired when the user clicks Cancel on the ResetApproving step (aborts an
    // in-progress cross-signing reset).
    std::function<void()>              on_cancel_reset;
    // Recover › "I've lost…" › Reset encryption.
    std::function<void()>              on_reset_encryption;

    // ── Interactive verification ──────────────────────────────────────────
    std::function<void()>              on_accept_request;      // IncomingRequest › Continue
    std::function<void()>              on_decline_request;     // IncomingRequest › Not me
    std::function<void()>              on_sas_match;           // CompareEmoji › They match
    std::function<void()>              on_sas_mismatch;        // CompareEmoji › They don't match
    std::function<void()>              on_cancel_verification; // WaitingForOtherDevice › Cancel
    std::function<void()>              on_retry_verification;  // VerifyFailed › Try again

    // Fired when a step / mode change alters which native text field should be
    // visible (e.g. Intro→EnterKey, or toggling the passphrase option). The
    // shell must respond by relaying out its surface so the on-layout pass
    // repositions/shows the NativeTextField. Same contract as
    // RoomView::on_layout_changed.
    std::function<void()>              on_layout_changed;

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
    // The account being reset, named on the approval step (the browser
    // approves for whichever account *it* is signed in as).
    void set_reset_account(std::string user_id) { reset_account_ = std::move(user_id); }
    // The server-issued approval link, offered as "Copy link".
    void set_reset_approval_url(std::string url)
    {
        reset_approval_url_ = std::move(url);
        link_copied_        = false;
    }
    // The reset was approved — start the Fresh recovery-key setup.
    void reset_approved()   { advance_step_(Step::Intro); }
    // The reset failed / was cancelled server-side; show the error and turn
    // the button into a Close.
    void report_reset_error(const std::string& msg)
    {
        error_msg_ = msg;
    }

    // ── Interactive verification (driven by ShellBase) ────────────────────
    // Recover's Choose step offers "Use another device" only once this is
    // known to be true (ShellBase fetches the device list asynchronously).
    void set_has_verified_other_device(bool has);
    bool has_verified_other_device() const { return has_other_device_; }
    // `peer` is the requesting device's name (own_device) or user's name.
    void show_incoming_request(std::string peer, bool own_device);
    void show_waiting()   { advance_step_(Step::WaitingForOtherDevice); }
    // Late-arriving friendlier name for the IncomingRequest peer.
    void set_peer(std::string peer) { peer_ = std::move(peer); }
    // Back to where an incoming request interrupted the user, if it did;
    // otherwise to the mode's first step (Recover: Choose, Fresh: Intro). In
    // Verify mode there is nothing to go back to, so fire on_close.
    void return_to_start();
    void show_emojis(std::vector<VerificationEmoji> emojis);
    void verification_done(DoneKind kind);
    // No-op once already on VerifyFailed (a local "don't match" already
    // explained it; the SDK's own cancel echo follows).
    void verification_failed(std::string reason, bool can_retry);
    // True while an interactive verification is on screen — where an
    // incoming request would otherwise yank the user out of something.
    bool in_verification_step() const;
    // Steps that must never be pre-empted (the recovery key could be lost
    // or an operation is mid-flight).
    bool busy() const;

    // Outcome of an on_save_to_file request. A successful save also ticks
    // the "I've saved my recovery key" checkbox.
    void key_save_result(bool ok, const std::string& error);

    // Force-hide every native field (the shell calls this while a fullscreen
    // viewer/camera overlay is open — see passphrase_field() below).
    void hide_native_fields();

    // The recovery key split into space-separated groups of 4 for display.
    static std::string format_recovery_key(const std::string& key);

    // ── Accessors ─────────────────────────────────────────────────────────
    Step        step()          const { return step_; }
    Mode        mode()          const { return mode_; }
    std::string recovery_key()  const { return recovery_key_; }
    std::string error_msg()     const { return error_msg_; }
    std::string progress_label()const { return progress_label_; }
    DoneKind    done_kind()     const { return done_kind_; }
    const std::vector<VerificationEmoji>& emojis() const { return emojis_; }
    // Key-backup upload progress in [0, 1], or < 0 when there is no count to
    // show (every stage except "backing up keys").
    float       progress_fraction() const { return progress_fraction_; }
    bool        key_saved_checked() const { return key_saved_checked_; }
    bool        primary_enabled()   const { return primary_enabled_; }

    // Borrowed pointers so the shell can force-hide these fields while a
    // fullscreen viewer/camera overlay is open (native OS controls always
    // paint above the canvas, regardless of tree z-order) — not covered by
    // MainAppWidget's own any_modal_open_() gating for that specific case.
    tk::TextField* passphrase_field() const { return passphrase_field_; }
    tk::TextField* key_field()        const { return key_field_; }
    tk::TextField* passphrase_confirm_field() const { return passphrase_confirm_field_; }

    // Shadows tk::Widget::set_visible (not virtual — same idiom as
    // tk::TextField's own shadow) so hiding the overlay also hides the
    // passphrase/key fields' native controls. tk::Widget::set_visible does
    // not cascade to children by design.
    void set_visible(bool v);

    void on_theme_changed(const tk::Theme& t) override;

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
        step_                = initial_step_(mode);
        done_kind_           = initial_done_kind_(mode);
        has_other_device_    = false;
        emojis_.clear();
        peer_.clear();
        incoming_own_device_ = true;
        can_retry_           = false;
        resume_step_.reset();
        reset_account_.clear();
        reset_approval_url_.clear();
        link_copied_         = false;
        recovery_key_.clear();
        error_msg_.clear();
        progress_label_.clear();
        progress_fraction_   = -1.0f;
        save_status_.clear();
        key_saved_checked_   = false;
        passphrase_mode_     = false;
        // Clear all callbacks so the caller can re-wire fresh ones.
        on_close             = {};
        on_enable_recovery   = {};
        on_recover           = {};
        on_request_sas       = {};
        on_copy_to_clipboard = {};
        on_save_to_file      = {};
        on_cancel_reset      = {};
        on_reset_encryption  = {};
        on_accept_request    = {};
        on_decline_request   = {};
        on_sas_match         = {};
        on_sas_mismatch      = {};
        on_cancel_verification = {};
        on_retry_verification  = {};
        on_layout_changed    = {};
        if (passphrase_field_) passphrase_field_->set_text("");
        if (passphrase_confirm_field_) passphrase_confirm_field_->set_text("");
        if (key_field_)        key_field_->set_text("");
    }

    // ── Test helpers ──────────────────────────────────────────────────────
    void simulate_primary_action();
    void simulate_skip();
    // Follows the "Use a passphrase instead" link (Intro → PassphraseEntry).
    void simulate_select_passphrase_mode();
    void simulate_back();
    // Presses and releases on the dim backdrop outside the card.
    void simulate_backdrop_click();
    void simulate_check_key_saved();
    // Recover › Choose › "Use another device".
    void simulate_sas_link();
    void simulate_choose_recovery_key();
    void simulate_choose_lost_access();
    // The secondary link on IncomingRequest / CompareEmoji (Not me / They
    // don't match).
    void simulate_secondary();

private:
    static Step initial_step_(Mode m)
    {
        return m == Mode::Fresh ? Step::Intro
             : m == Mode::Recover ? Step::Choose
                                  : Step::IncomingRequest;
    }
    static DoneKind initial_done_kind_(Mode m)
    {
        return m == Mode::Fresh ? DoneKind::Protected : DoneKind::Unlocked;
    }
    void     advance_step_(Step next);
    void     fire_primary_();
    tk::Rect card_bounds() const;
    float    step_card_height_() const;
    void     update_progress_label_(uint8_t step, uint32_t backed_up, uint32_t total);
    bool     passphrase_field_rect_visible() const;
    tk::Rect passphrase_field_rect_value()   const;
    tk::Rect passphrase_confirm_field_rect_value() const;
    // Whether both passphrase fields are filled in and agree.
    bool     passphrases_match_() const;
    bool     backdrop_closes_() const;
    void     choose_other_device_();
    void     reject_();
    bool     key_field_rect_visible()        const;
    tk::Rect key_field_rect_value()          const;

    Mode        mode_;
    Step        step_            = Step::Intro;
    std::string recovery_key_;
    std::string error_msg_;
    std::string progress_label_;
    float       progress_fraction_  = -1.0f;
    std::string save_status_;       // "Saved to …" / save error, ShowKey only
    bool        save_failed_        = false;
    bool        key_saved_checked_  = false;
    bool        passphrase_mode_    = false;
    DoneKind    done_kind_          = DoneKind::Protected;
    bool        has_other_device_   = false;
    std::vector<VerificationEmoji> emojis_;
    std::string peer_;
    bool        incoming_own_device_ = true;
    bool        can_retry_           = false;
    std::string reset_account_;      // ResetApproving: whose reset this is
    std::string reset_approval_url_; // ResetApproving: "Copy link" payload
    bool        link_copied_ = false;
    // Where an incoming request interrupted the user; see return_to_start().
    std::optional<Step> resume_step_;

    // The filled action buttons are child tk::Button widgets (positioned +
    // styled per-step in paint()). primary_button_ is reused across every step
    // that has a bottom-right action (Continue / Verify / Close / …);
    // copy_button_ and save_button_ appear only on the ShowKey step.
    tk::Button* primary_button_ = nullptr;
    tk::Button* copy_button_    = nullptr;
    tk::Button* save_button_    = nullptr;

    // ── Layout rects computed during paint(), hit-tested in pointer handlers ──
    // These cover the non-button affordances only (text links and the
    // "I've saved my recovery key" checkbox).
    tk::Rect secondary_link_{};   // "Skip for now"
    tk::Rect passphrase_link_{};  // "Use a passphrase instead"
    tk::Rect back_link_{};
    tk::Rect checkbox_rect_{};
    tk::Rect device_row_{};       // Choose › Use another device
    tk::Rect key_row_{};          // Choose › Enter recovery key
    tk::Rect lost_link_{};        // Choose › I've lost…
    tk::Rect reject_link_{};      // Not me / They don't match / Decline
    bool     primary_enabled_ = true;  // recomputed per-step in paint()

    // ── Press tracking (mirror ImageViewerOverlay) ──────────────────────────
    bool press_secondary_   = false;
    bool press_back_        = false;
    bool press_checkbox_    = false;
    bool press_passphrase_  = false;
    bool press_device_row_  = false;
    bool press_key_row_     = false;
    bool press_lost_        = false;
    bool press_reject_      = false;
    bool backdrop_press_    = false;

    // Borrowed — owned via add_child(). Null when constructed without a
    // Host (e.g. in tests that don't exercise the native field).
    tk::TextField* passphrase_field_         = nullptr;
    tk::TextField* passphrase_confirm_field_ = nullptr;
    tk::TextField* key_field_                = nullptr;

    // The "›" on each Choose-step option row (Lucide chevron-right).
    tk::IconCache chevron_icon_;

    // Spinner animation clock; reset when the Progress step is entered.
    std::chrono::steady_clock::time_point progress_start_{};
};

} // namespace tesseract::views
