#pragma once

// Shared LoginView — the visual skeleton that every platform host wraps.
// Composes a centred VBox of title + caption + (host-overlaid native
// homeserver text field) + sign-in button + status. The native text
// field is layered on top by the host because IME / selection stays
// native per the toolkit architecture (see the plan's open questions).
//
// This phase ships the visuals only. OAuth / Client wiring lands when
// the per-platform Hosts come online and can post the "begin"/"await"
// completion events back onto the UI thread.

#include "tk/canvas.h"
#include "tk/widget.h"
#include "tk/layout.h"
#include "tk/controls.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tesseract::views {

class LoginView : public tk::Widget {
public:
    LoginView();

    enum class State { Form, Waiting };

    void set_state(State s);
    State state() const { return state_; }

    // Status banner shown beneath the form (errors during phase = Form,
    // progress text during phase = Waiting). Empty string hides it.
    void set_status(std::string message, std::optional<tk::Color> colour = {});

    // Reflects the host-managed native text field; the View itself does
    // not own a TextField widget yet (deferred until tk::TextField + the
    // IME-passthrough host glue land).
    void set_homeserver_label(std::string url);
    const std::string& homeserver_label() const { return homeserver_label_; }

    // The host wires these to the OAuth flow. Defaults are no-ops so the
    // visual tree can be exercised standalone.
    std::function<void()> on_sign_in;
    std::function<void()> on_cancel;

    // Rect inside the LoginView (in widget-local coordinates) that the
    // host should place a native edit control over. Valid after the
    // first arrange() pass.
    tk::Rect homeserver_field_rect() const { return homeserver_field_rect_; }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    void rebuild_tree();

    State        state_       = State::Form;
    std::string  homeserver_label_ = "matrix.org";

    // Borrowed: all of these are owned by `card_` which is owned by `this`.
    tk::VBox*    card_         = nullptr;
    tk::Label*   title_lbl_    = nullptr;
    tk::Label*   caption_lbl_  = nullptr;
    tk::Label*   hs_field_lbl_ = nullptr;
    tk::Button*  sign_in_btn_  = nullptr;
    tk::Button*  cancel_btn_   = nullptr;
    tk::Label*   status_lbl_   = nullptr;

    tk::Rect     homeserver_field_rect_{};
};

} // namespace tesseract::views
