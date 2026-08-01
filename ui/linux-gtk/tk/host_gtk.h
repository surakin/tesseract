#pragma once

// GTK4 host. The Surface owns a GtkOverlay containing a GtkDrawingArea
// (rendered into by the shared widget tree via the Cairo+Pango canvas
// backend) plus native overlay widgets (GtkEntry et al.) positioned over
// the canvas via margins.

#include "canvas.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#include <functional>
#include <memory>

typedef struct _GtkWidget GtkWidget;

namespace tk
{
class AnimImageCache;
}

namespace tk::gtk4
{

class Host;

// Embed `widget()` (a GtkOverlay) into your normal GTK4 widget tree.
// Everything inside paints through the shared toolkit; child native
// widgets like GtkEntry layer on top of the canvas via NativeTextField.
class Surface
{
public:
    explicit Surface(const Theme& theme = Theme::light(),
                     bool transparent = false);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // The GtkOverlay you embed in your normal GTK4 layout. Owned by the
    // Surface; the Surface destructor unrefs the toplevel.
    GtkWidget* widget() const;

    tk::Host& host();
    CanvasFactory& factory();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange + repaint on the existing root. Call
    // after mutating widget state in a way that affects layout. The
    // GtkDrawingArea's "resize" signal also calls this automatically.
    void relayout();
    void set_theme(const Theme& t);

    // Pushes `scale` through the whole widget tree via
    // Widget::apply_scale_change() (see widget.h) — call once the GTK
    // scale-factor-change signal fires so native-control image captures
    // (tk::NativeTextField/NativeTextArea) don't stay stale/blurry.
    void apply_scale_change(float scale);

    // Fired at the tail of apply_scale_change() above, with the new scale.
    // Lets integration code (the owning shell) track the display's current
    // scale — see ShellBase::set_current_scale_()'s doc comment — without
    // needing its own separate DPI-change plumbing.
    void set_on_scale_changed(std::function<void(float)> cb);

    // Animated-image partial repaints. Point the surface at the shell's
    // animation cache once at setup; then call update_anim_regions() from the
    // animation timer to invalidate only the rects where animated images were
    // drawn on the last paint. GTK4 has no partial-invalidation API for a
    // single widget, but each currently-animating, currently-visible image
    // gets its own small overlay GtkDrawingArea (see host_gtk.cpp's
    // live_overlays_/sync_anim_overlays_), so invalidating just those doesn't
    // force the rest of the tree to redraw. Every Surface that draws
    // animated content needs this called once — not just GifPopup.
    void set_anim_cache(const AnimImageCache* cache);
    void update_anim_regions();

    // Callback fired at the tail of every relayout (initial, resize,
    // explicit). Use this from integration code to keep native overlays
    // — GtkEntry positions etc. — aligned with the shared widget tree.
    // Single-slot: a second caller silently replaces the first's callback,
    // so use this only where a Surface is known to have exactly one
    // interested subsystem.
    void set_on_layout(std::function<void()> cb);

    // Same trigger point as set_on_layout, but additive — for a subsystem
    // that can't assume it's the only thing interested in relayout (e.g.
    // the accessibility bridge, which must coexist with whatever a given
    // Surface's owner already wired via set_on_layout). Mirrors
    // tk::qt6::Surface::add_layout_listener.
    void add_layout_listener(std::function<void()> cb);

    // Called when a drop fails because the file could not be read.
    void set_on_file_drop_error(FileDropErrorHandler cb);

private:
    std::unique_ptr<Host> host_;
    std::function<void(float)> on_scale_changed_;
};

} // namespace tk::gtk4
