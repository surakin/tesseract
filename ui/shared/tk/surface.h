#pragma once

#include "canvas.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#include <functional>
#include <memory>

namespace tk
{

class AnimImageCache;

// What every backend's native Surface (tk::win32::Surface, tk::gtk4::Surface,
// tk::qt6::Surface, tk::macos::Surface) offers regardless of toolkit: it owns a
// Host, paints one widget tree, and can be re-themed, relayouted and told
// where its animated images are. The native widget itself (HWND, GtkWidget*,
// QWidget, NSView) stays on the concrete class; this is the part the shared
// shell code can use without knowing which backend it is.
class SurfaceBase
{
public:
    virtual ~SurfaceBase() = default;

    virtual Host& host() = 0;
    virtual CanvasFactory& factory() = 0;
    virtual const Theme& theme() const = 0;

    // Mount the widget tree this surface paints; measures, arranges and paints.
    virtual void set_root(std::unique_ptr<Widget> root) = 0;
    virtual Widget* root() const = 0;

    // Re-run measure + arrange + repaint after widget state changed.
    virtual void relayout() = 0;
    // Switch the surface's theme (and repaint). The widget tree is told
    // separately with root()->apply_theme(); see apply_theme() below. The
    // surface keeps a pointer, so `t` must outlive it (e.g.
    // ShellBase::current_theme_ or Theme::light()/dark()).
    virtual void set_theme(const Theme& t) = 0;
    // The usual theme change: this surface and its widget tree.
    void apply_theme(const Theme& t)
    {
        set_theme(t);
        if (auto* r = root())
        {
            r->apply_theme(t);
        }
    }

    // The display scale changed: push it through the widget tree, and tell the
    // owner (see ShellBase::set_current_scale_).
    virtual void apply_scale_change(float scale) = 0;
    virtual void set_on_scale_changed(std::function<void(float)> cb) = 0;

    // Animated-image partial repaints: point the surface at the shell's
    // animation cache once, then call update_anim_regions() from the
    // animation timer.
    virtual void set_anim_cache(const AnimImageCache* cache) = 0;
    virtual void update_anim_regions() = 0;

    // Called at the tail of every relayout.
    virtual void add_layout_listener(std::function<void()> cb) = 0;

    // Called when a drop fails because the file could not be read.
    virtual void set_on_file_drop_error(FileDropErrorHandler cb) = 0;
};

} // namespace tk
