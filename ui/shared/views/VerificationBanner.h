#pragma once

// Shared device-verification banner. Shown above the message list when the
// SDK reports that this device's cross-signing identity is Unverified.
// The user walks through a 7-emoji SAS comparison with another signed-in
// device to complete verification. Mirrors RecoveryBanner in layout and style.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <tesseract/types.h>

namespace tesseract::views
{

class VerificationBanner : public tk::Widget
{
public:
    VerificationBanner();
    ~VerificationBanner() override = default;

    enum class State
    {
        /// Unverified prompt: "Verify this device" + [Verify] [Dismiss] — 48 px
        Prompt,
        /// Another device sent a request: "Another device wants to verify" +
        /// [Accept] [Decline] — 48 px
        IncomingRequest,
        /// Waiting for the remote device to respond — 48 px
        Waiting,
        /// Both devices have exchanged keys; 7 emoji tiles shown — 124 px
        ShowEmojis,
        /// User confirmed match; waiting for SDK to sign — 48 px
        Confirming,
        /// Verification complete — "Device verified ✓" — 48 px
        Done,
        /// Flow cancelled — reason shown — 48 px
        Cancelled,
    };

    void set_state(State s);
    State state() const
    {
        return state_;
    }

    /// Supply the 7 emoji received from on_sas_ready. Switches state to
    /// ShowEmojis automatically.
    void set_emojis(const std::vector<VerificationEmoji>& emojis);

    /// Cancellation reason for the Cancelled state.
    void set_cancel_reason(std::string reason);

    // Button callbacks
    std::function<void()> on_verify;   // Prompt: start verification
    std::function<void()> on_accept;   // IncomingRequest: accept + start SAS
    std::function<void()> on_match;    // ShowEmojis: emojis match
    std::function<void()> on_mismatch; // ShowEmojis: emojis don't match
    std::function<void()> on_cancel;   // Waiting: cancel flow
    std::function<void()> on_dismiss;  // Prompt / Cancelled: hide banner
    std::function<void()> on_done;     // Done: fired after brief pause
    std::function<void()>
        on_use_recovery_key; // Prompt: switch to recovery key flow

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    static constexpr float kHeightNormal = 48.0f;
    static constexpr float kHeightShowEmoji = 124.0f;
    static constexpr int kEmojiCount = 7;

    void apply_state();
    std::string label_text() const;

    State state_ = State::Prompt;
    std::string cancel_reason_;
    std::vector<VerificationEmoji> emojis_; // up to 7 entries in ShowEmojis

    tk::Label* label_ = nullptr;    // borrowed
    tk::Button* primary_ = nullptr; // borrowed — context-dependent action
    tk::Button* secondary_ =
        nullptr; // borrowed — context-dependent dismiss/decline
    tk::Button* dismiss_ =
        nullptr; // borrowed — always visible in Prompt/Cancelled
    tk::Button* link_ =
        nullptr; // borrowed — "Use recovery key" in Prompt state

    // Cached layout rects (computed in arrange)
    tk::Rect label_rect_{};
    tk::Rect primary_rect_{};
    tk::Rect secondary_rect_{};
    tk::Rect dismiss_rect_{};
    tk::Rect link_rect_{};
    // One rect per emoji tile in ShowEmojis state
    std::array<tk::Rect, kEmojiCount> emoji_rects_{};
    std::array<tk::Rect, kEmojiCount> emoji_label_rects_{};
};

} // namespace tesseract::views
