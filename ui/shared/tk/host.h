#pragma once

// Per-platform integration surface. The shared widget tree is owned by
// a Host: the platform-native window, event pump, and bridge to
// off-thread callbacks (Rust SDK → UI thread) all live behind this
// interface. The toolkit owns paint and layout; the host owns:
//
//   - the native window / view / HWND
//   - the run loop ("repaint please", "run this on the UI thread")
//   - menus, file dialogs, accessibility, IME (kept native by design)
//   - native edit-control overlays for text input, since IME stays
//     native until tk::TextField + the IME-passthrough hybrid lands

#include "canvas.h"

#include <functional>
#include <memory>
#include <string>

namespace tk {

// A borrowed handle to a native text input control overlaid on top of
// the canvas, positioned to align with a tk::Widget's rect. Caller
// destroys to remove the overlay. Hosts back this with QLineEdit,
// GtkEntry, Win32 EDIT, UITextField respectively.
class NativeTextField {
public:
    virtual ~NativeTextField() = default;

    // Position + size of the overlay in widget-tree coordinates (the
    // same coordinate space root_widget->arrange() operates in).
    virtual void set_rect       (Rect r)              = 0;
    virtual void set_text       (std::string text)    = 0;
    virtual std::string text    () const              = 0;
    virtual void set_placeholder(std::string text)    = 0;
    virtual void set_focused    (bool focused)        = 0;
    virtual void set_visible    (bool visible)        = 0;
    virtual void set_enabled    (bool enabled)        = 0;

    // Password / obscured-entry mode. Used by the recovery-banner key
    // field so the user's recovery secret isn't shoulder-surfed.
    virtual void set_password   (bool password)       = 0;

    // Text-change + submit (Enter) hooks. Hosts invoke these on the UI
    // thread; the callable can mutate the widget tree freely.
    virtual void set_on_changed(std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit (std::function<void()>)                    = 0;
};

// Multi-line variant. Auto-grows up to a host-clamped envelope so the
// compose bar's height tracks the text content. Backs the ComposeBar's
// input affordance; IME / selection stay native.
class NativeTextArea {
public:
    virtual ~NativeTextArea() = default;

    virtual void set_rect       (Rect r)              = 0;
    virtual void set_text       (std::string text)    = 0;
    virtual std::string text    () const              = 0;
    virtual void set_placeholder(std::string text)    = 0;
    virtual void set_focused    (bool focused)        = 0;
    virtual void set_visible    (bool visible)        = 0;
    virtual void set_enabled    (bool enabled)        = 0;

    // Push the natural content height up to the host on every change so
    // the parent layout can resize the compose envelope inside its
    // [min, max] clamp.
    virtual float natural_height() const = 0;

    virtual void set_on_changed       (std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit        (std::function<void()>)                    = 0;
    virtual void set_on_height_changed(std::function<void(float)>)               = 0;
};

class Host {
public:
    virtual ~Host() = default;

    // Schedule a repaint of the entire root widget. Cheap to call
    // multiple times — the host coalesces.
    virtual void request_repaint() = 0;

    // Post `task` to be run on the UI thread. The most common caller is
    // Rust async completion handlers that finish on a worker thread and
    // need to mutate widgets. Safe to call from any thread.
    virtual void post_to_ui(std::function<void()> task) = 0;

    // Allocate a native text input control parented to the host's
    // surface. The returned NativeTextField is owned by the caller; let
    // the unique_ptr destruct to remove the overlay.
    virtual std::unique_ptr<NativeTextField> make_text_field() = 0;

    // Multi-line variant — IME-friendly, expanding inside the host's
    // clamp. Used by the shared ComposeBar for the message-input row.
    virtual std::unique_ptr<NativeTextArea>  make_text_area()  = 0;
};

} // namespace tk
