#pragma once

#include "tk/theme.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract
{

// Base class for a plain secondary top-level window that hosts one tk widget
// tree (e.g. the Activity Monitor). The platform subclass creates the native
// window plus a tk::*::Surface and mounts the root widget passed to
// ShellBase::create_aux_window_().
class AuxWindowBase
{
public:
    virtual ~AuxWindowBase() = default;

    // Fired when the user closes the native window.
    std::function<void()> on_window_closed;

    virtual void bring_to_front()                = 0;
    virtual void close_window()                  = 0;
    virtual void apply_theme(const tk::Theme&)   = 0;
    virtual void apply_scale_change(float scale) = 0;
    virtual void request_relayout()              = 0;
    virtual void request_repaint() { request_relayout(); }

    // Destroy safely, deferring deletion where the platform requires it (Qt6:
    // deleteLater()). The caller must already have nulled on_window_closed and
    // called close_window(). Default: delete immediately.
    virtual void schedule_delete() { delete this; }
};

} // namespace tesseract
