#pragma once

// Cross-platform modal confirmation overlay. Mounted at MainAppWidget level
// (painted last so it sits above every other view including the lightboxes)
// and used by any widget that wants to gate a destructive action behind a
// yes/no prompt. The same pattern as RoomInfoPanel: closed-by-default,
// visibility tied to open state so the hit-test walks past it when idle.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class ConfirmDialog : public tk::Widget
{
public:
    struct Options
    {
        std::string title;
        std::string body;                   // optional; may be empty
        std::string confirm_label = "Confirm";
        std::string cancel_label  = "Cancel";
        bool        destructive   = false;  // red confirm button
    };

    ConfirmDialog();
    ~ConfirmDialog() override = default;

    // Show the dialog with the given options. `on_confirm` fires when the
    // user clicks the confirm button. Cancel + backdrop click dismiss the
    // dialog without firing the callback. A new open() while the dialog is
    // already up replaces the contents (mirrors RoomInfoPanel::open).
    void open(Options opts, std::function<void()> on_confirm);
    void close();
    bool is_open() const { return open_; }

    // Fires when the dialog opens or closes. MainAppWidget routes this into
    // the shared layout-changed chain so the shells re-query rect accessors
    // (specifically: hide the native compose textarea + room-search field
    // while the dialog covers them).
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

private:
    bool open_ = false;

    Options opts_;
    std::function<void()> on_confirm_;

    // Child buttons — owned via add_child, raw pointers borrowed back.
    tk::Button* confirm_btn_ = nullptr;
    tk::Button* cancel_btn_  = nullptr;

    // Layout rects in world-space, updated each arrange().
    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};

    // Cached layouts so we don't rebuild every paint.
    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> body_layout_;

    bool press_backdrop_ = false;

    static constexpr float kCardW   = 360.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kBtnH    = 36.0f;
    static constexpr float kBtnGap  = 8.0f;
    static constexpr float kTitleH  = 22.0f;
    static constexpr float kTitleGap = 12.0f;
    static constexpr float kBodyGap  = 20.0f;
};

} // namespace tesseract::views
