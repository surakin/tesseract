#pragma once

// Shared compose bar. A bottom-anchored strip with:
//   - emoji button (left of the input)
//   - text input area (host overlays a NativeTextArea on text_area_rect())
//   - send button (right of the input)
//
// The widget paints the background, separator, and buttons; the host is
// responsible for mounting a tk::NativeTextArea at text_area_rect() so
// IME / selection / undo stay native. The widget auto-grows between
// kMinHeight and kMaxHeight based on the natural content height reported
// by the host's NativeTextArea.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views {

class ComposeBar : public tk::Widget {
public:
    ComposeBar();
    ~ComposeBar() override = default;

    static constexpr float kMinHeight = 56.0f;
    static constexpr float kMaxHeight = 160.0f;

    /// Rect inside the compose bar (widget-local coordinates, same space
    /// `root->arrange` operates in) for the host to overlay a
    /// NativeTextArea. Empty when the widget hasn't been arranged yet.
    tk::Rect text_area_rect() const { return text_area_rect_; }

    /// Host bridge: integration code pushes the latest natural height of
    /// the NativeTextArea here on every `on_height_changed` callback.
    /// The compose bar clamps to [kMinHeight, kMaxHeight] internally; the
    /// parent layout grows by re-measuring after this call.
    void set_text_area_natural_height(float h);
    float natural_height() const { return natural_height_; }

    /// Latest text — pushed by integration code on every `on_changed`
    /// callback from the NativeTextArea. Used to gate the send button.
    void set_current_text(std::string text);
    const std::string& current_text() const { return current_text_; }

    void  set_enabled(bool e);
    bool  enabled() const { return enabled_; }

    /// Fires when the send button is clicked or NativeTextArea's submit
    /// callback fires (host wires both to the same target). Receives the
    /// current text — integration trims + sends through the SDK.
    std::function<void(const std::string&)> on_send;

    /// Fires when the emoji button is clicked.
    std::function<void()>                    on_emoji;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    void refresh_send_enabled();

    tk::Button* emoji_btn_  = nullptr;   // borrowed (owned by Widget tree)
    tk::Button* send_btn_   = nullptr;   // borrowed
    tk::Rect    text_area_rect_{};
    tk::Rect    emoji_rect_{};
    tk::Rect    send_rect_{};

    float       natural_height_ = kMinHeight;
    std::string current_text_;
    bool        enabled_        = true;
};

} // namespace tesseract::views
