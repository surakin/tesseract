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

namespace tk
{
class AnimImageCache;
}

namespace tk::macos
{

class Host;

class Surface
{
public:
    explicit Surface(const Theme& theme = Theme::light(),
                     bool transparent = false);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Returns the underlying NSView* as an opaque pointer. From .mm
    // code, `(__bridge NSView*)surface.view_handle()` is the embeddable
    // view. The Surface retains the view; callers must NOT release it.
    void* view_handle() const;

    tk::Host& host();
    CanvasFactory& factory();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange + repaint on the existing root. The
    // NSView's `-layout` already triggers this on resize.
    void relayout();
    void set_theme(const Theme& t);

    // Pushes `scale` through the whole widget tree via
    // Widget::apply_scale_change() (see widget.h) — call once
    // NSWindowDidChangeBackingPropertiesNotification fires so native-control
    // image captures (tk::NativeTextField/NativeTextArea) don't stay
    // stale/blurry.
    void apply_scale_change(float scale);

    // Fired at the tail of apply_scale_change() above, with the new scale.
    // Lets integration code (the owning shell) track the display's current
    // scale — see ShellBase::set_current_scale_()'s doc comment — without
    // needing its own separate DPI-change plumbing.
    void set_on_scale_changed(std::function<void(float)> cb);

    // Animated-image partial repaints. Point the surface at the shell's
    // animation cache once at setup; then call update_anim_regions() from the
    // animation timer instead of relayout() to invalidate only the rects where
    // animated images were drawn on the last paint.
    void set_anim_cache(const AnimImageCache* cache);
    void update_anim_regions();

    // Callback fired at the tail of every relayout. Use this to align
    // NSTextField overlays with shared widget rects.
    void set_on_layout(std::function<void()> cb);

    // Called when a drop fails because the file could not be read.
    void set_on_file_drop_error(FileDropErrorHandler cb);

    // Install a right-click handler. Receives surface-local widget coordinates.
    void set_on_right_click(std::function<void(tk::Point)> cb);

private:
    std::unique_ptr<Host> host_;
    std::function<void(float)> on_scale_changed_;
};

} // namespace tk::macos
