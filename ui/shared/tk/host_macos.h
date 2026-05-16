#pragma once

// macOS host. The Surface owns an NSView subclass that paints into a
// CoreGraphics + CoreText canvas (see tk::cg) and hosts NSTextField
// overlays as subviews positioned over the canvas.
//
// The .h stays pure C++; the .mm provides the concrete NSView + bridging
// casts. Caller code in .mm files reads the NSView via
// `__bridge NSView* view = (__bridge NSView*)surface.view_handle()`.

#include "canvas.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#include <functional>
#include <memory>

namespace tk::macos {

class Host;

class Surface {
public:
    explicit Surface(const Theme& theme = Theme::light(),
                     bool transparent   = false);
    ~Surface();
    Surface(const Surface&)            = delete;
    Surface& operator=(const Surface&) = delete;

    // Returns the underlying NSView* as an opaque pointer. From .mm
    // code, `(__bridge NSView*)surface.view_handle()` is the embeddable
    // view. The Surface retains the view; callers must NOT release it.
    void* view_handle() const;

    tk::Host&   host();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange + repaint on the existing root. The
    // NSView's `-layout` already triggers this on resize.
    void relayout();

    // Callback fired at the tail of every relayout. Use this to align
    // NSTextField overlays with shared widget rects.
    void set_on_layout(std::function<void()> cb);

    // Install a drag-and-drop handler. When set, the underlying NSView
    // accepts any file drop (and in-app image data) and invokes the
    // callback once per dropped file with raw bytes, OS-supplied MIME,
    // and basename. The shell dispatches by MIME. Pass {} to disable.
    void set_on_file_drop(FileDropHandler cb);
    // Deprecated alias.
    void set_on_image_drop(FileDropHandler cb) { set_on_file_drop(std::move(cb)); }

    // Install a right-click handler. Receives surface-local widget coordinates.
    void set_on_right_click(std::function<void(tk::Point)> cb);

private:
    std::unique_ptr<Host> host_;
};

} // namespace tk::macos
