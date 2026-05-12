#pragma once

// Shared recovery banner. A fixed-height inline strip shown above the
// message list when the SDK reports `client.needs_recovery() == true`.
// Paints a tinted backdrop + status label + Verify and Dismiss buttons;
// the password key entry is a host-overlaid NativeTextField (IME stays
// native, matching the LoginView pattern).

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <string>

namespace tesseract::views {

class RecoveryBanner : public tk::Widget {
public:
    RecoveryBanner();
    ~RecoveryBanner() override = default;

    enum class State {
        /// Initial prompt: "Verify this device:" + visible key entry +
        /// Verify button. Host-overlay is visible and editable.
        Form,
        /// Worker thread is running `client.recover(key)`. Key entry +
        /// Verify hidden; label reads "Verifying…".
        Verifying,
        /// Recover succeeded; the SDK is now pulling key backups.
        /// Label reads "Downloading historical keys…" or
        /// "Importing keys from backup… N imported." once N > 0.
        Importing,
        /// Recover failed; label shows the error, key entry +
        /// Verify return so the user can retry.
        Failed,
    };

    void  set_state(State s);
    State state() const { return state_; }

    /// Sets the trailing detail for the Failed state's label. Ignored in
    /// other states.
    void set_failure_message(std::string msg);

    /// Updates the imported-keys count for the Importing state. Zero
    /// reverts the label to "Downloading historical keys…".
    void set_import_progress(std::uint64_t imported);

    /// Host hook: rect inside the banner (widget-local coordinates) for
    /// the host to overlay a password NSTextField / GtkEntry / QLineEdit
    /// / Win32 EDIT. Empty rect (w == 0) in Verifying / Importing states
    /// — the host should hide the overlay then.
    tk::Rect recovery_key_field_rect() const;

    /// Whether the host should display the key field overlay right now.
    /// `true` in Form / Failed; `false` in Verifying / Importing.
    bool recovery_key_field_visible() const;

    std::function<void(/*key*/ const std::string&)> on_verify;
    std::function<void()>                            on_dismiss;

    /// Host bridge: integration code routes its NativeTextField's text
    /// into the banner before firing the verify button. Stored here so
    /// the verify-button click can pull the latest key without the
    /// integration needing to thread the field reference through.
    void set_current_key(std::string key) { current_key_ = std::move(key); }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    void apply_state();
    std::string label_text() const;

    State              state_         = State::Form;
    std::string        failure_msg_;
    std::uint64_t      imported_keys_ = 0;
    std::string        current_key_;

    tk::Label*         label_     = nullptr;   // borrowed
    tk::Button*        verify_    = nullptr;   // borrowed
    tk::Button*        dismiss_   = nullptr;   // borrowed

    tk::Rect           label_rect_   {};
    tk::Rect           key_field_rect_{};
    tk::Rect           verify_rect_  {};
    tk::Rect           dismiss_rect_ {};
};

} // namespace tesseract::views
