#pragma once

#include "views/CallOverlayWidget.h"
#include <functional>

namespace tesseract { class ShellBase; }

namespace tesseract
{

// Base class for the call pop-out window. Each instance owns the
// CallOverlayWidget that fills the secondary window. The platform subclass
// creates a native window + tk::*::Surface, sets call_overlay_widget_, and
// calls wire_call_overlay() to connect providers.
//
// Fired when the user closes the window (not when the call ends).
// ShellBase wires this to switch mode back to Docked.
class CallWindowBase
{
public:
    explicit CallWindowBase(ShellBase* shell);
    virtual ~CallWindowBase() = default;

    using PostDelayedFn = views::CallOverlayWidget::PostDelayedFn;

    views::CallOverlayWidget* call_overlay_widget() const;

    // Wire providers into the call overlay widget and switch it to Popout mode.
    // Call after the subclass has created the surface and set call_overlay_widget_.
    void wire_call_overlay(
        PostDelayedFn                                               post_delayed,
        std::function<void()>                                       repaint_requester,
        std::function<const tk::Image*(const std::string&)>        avatar_provider,
        std::function<std::string(const std::string&)>             display_name_provider);

    // Fired when the user closes the native window. Wire to ShellBase to
    // switch the call back to Docked mode.
    std::function<void()> on_window_closed;

    virtual void bring_to_front()              = 0;
    virtual void close_window()                = 0;
    virtual void apply_theme(const tk::Theme&) = 0;
    virtual void request_relayout()            = 0;

    // Request a repaint of the popout window's surface without re-running
    // layout. Used by the video-frame repaint path; much cheaper than
    // request_relayout() for high-frequency calls (30fps video).
    // Default: falls back to request_relayout() for platforms that have
    // not yet overridden this.
    virtual void request_repaint() { request_relayout(); }

    // Destroy this window safely, deferring the actual deletion if the
    // platform requires it (Qt6: deleteLater() — never delete a QWidget
    // synchronously from inside its own event handler). The caller must
    // have already nulled on_window_closed and called close_window().
    // Default: delete this immediately (safe for non-Qt platforms).
    virtual void schedule_delete() { delete this; }

protected:
    ShellBase*                shell_               = nullptr;
    views::CallOverlayWidget* call_overlay_widget_ = nullptr;
};

} // namespace tesseract
