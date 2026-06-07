#pragma once

// Cross-platform error/info alert overlay. Mounted at container level (e.g.
// LoginView, MainAppWidget) and painted last so it sits above all other
// content. Differs from ConfirmDialog in two key ways: backdrop clicks are
// NOT dismissible (the user must explicitly choose an action), and it
// supports an optional secondary action button alongside the primary one.
//
// Typical use: show a "Connection Error" alert with "Retry" (primary) and
// "Sign In Again" (secondary) so the user is never silently dropped to the
// login view after a transient network failure.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class AlertDialog : public tk::Widget
{
public:
    struct Options
    {
        std::string title;
        std::string body;                      // optional; may be empty
        std::string primary_label   = "OK";
        std::string secondary_label;           // empty = no secondary button
    };

    AlertDialog();
    ~AlertDialog() override = default;

    // Show the dialog. `secondary_cb` may be null when no secondary button is
    // needed. A new open() while already shown replaces the contents.
    void open(Options opts,
              std::function<void()> primary_cb,
              std::function<void()> secondary_cb = {});
    void close();
    bool is_open() const { return open_; }

    // Fires when the dialog opens or closes so the host can re-query native
    // overlay rects (e.g. hide the compose textarea while the dialog is up).
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    // Backdrop clicks are intentionally ignored — the user must choose an
    // action. on_pointer_down still consumes events so nothing behind leaks.
    bool     on_pointer_down(tk::Point local) override;

private:
    bool open_ = false;

    Options opts_;
    std::function<void()> primary_cb_;
    std::function<void()> secondary_cb_;

    // Child buttons — owned via add_child, raw pointers borrowed back.
    tk::Button* primary_btn_   = nullptr;
    tk::Button* secondary_btn_ = nullptr;

    // Layout rects (world-space, updated each arrange()).
    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};

    // Cached text layouts — reset on open() so they rebuild on next paint.
    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> body_layout_;

    static constexpr float kCardW    = 360.0f;
    static constexpr float kCardPad  = 20.0f;
    static constexpr float kBtnH     = 36.0f;
    static constexpr float kBtnGap   = 8.0f;
    static constexpr float kTitleH   = 22.0f;
    static constexpr float kTitleGap = 12.0f;
    static constexpr float kBodyGap  = 20.0f;
};

} // namespace tesseract::views
