#pragma once

#include "MediaViewerItem.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

// What a page may ask of the MediaViewerOverlay that hosts it. Implemented
// privately by the overlay; pages never see the overlay type itself.
class MediaViewerPageHost
{
public:
    virtual ~MediaViewerPageHost() = default;

    // The overlay's bounds (root-surface coordinates).
    virtual tk::Rect host_bounds() const = 0;
    virtual bool host_fullscreen() const = 0;
    // False while the full-screen chrome is auto-hidden. Pages gate their own
    // always-on controls on it.
    virtual bool host_chrome_shown() const = 0;
    // Evaluate the full-screen auto-hide timer (call before host_chrome_shown()
    // when it is read ahead of the overlay's own chrome layout).
    virtual void host_refresh_chrome_autohide() = 0;
    virtual void host_request_repaint() = 0;
    // Resolve a thumbnail / image cache key to a decoded image (may be null).
    virtual const tk::Image* host_image(const std::string& key) const = 0;
    // Create an Icon-variant tk::Button child of the overlay. The page keeps
    // the returned pointer and shows/hides it as it (de)activates.
    virtual tk::Button* host_add_icon_button() = 0;
    // Create a labelled (Primary variant) tk::Button child of the overlay.
    virtual tk::Button* host_add_label_button(std::string label) = 0;
    // The page's player actually started producing sound / frames (after a load
    // or a resume). The overlay forwards it as on_playback_started.
    virtual void host_playback_started() = 0;
};

// One kind of viewer content (image, video, ...). A non-widget strategy object
// owned by MediaViewerOverlay: the overlay keeps the scrim, chrome buttons,
// pointer routing and paint order, and forwards to the active page. A page's
// buttons are real tk::Button children of the overlay, hidden while the page
// is inactive so they cannot be hit, tabbed to or reached by accessibility.
class MediaViewerPage
{
public:
    virtual ~MediaViewerPage() = default;

    // Make this page current for `item`: reset per-item state, show buttons.
    virtual void activate(const MediaViewerItem& item) = 0;
    // Stop playback, drop per-item state and hide the page's buttons.
    virtual void deactivate() = 0;

    // Geometry for the current bounds (also called again from paint).
    virtual void arrange(tk::LayoutCtx& lc, tk::Rect bounds) = 0;
    // Media content (and caption); drawn above the scrim.
    virtual void paint_content(tk::PaintCtx& ctx) = 0;
    // Transport controls; drawn above the content, below the shared chrome.
    virtual void paint_controls(tk::PaintCtx&) {}

    // Press that no button claimed. Return true to consume (not an outside tap).
    virtual bool on_content_pointer_down(tk::Point world, tk::Point local) = 0;
    virtual bool on_content_pointer_up(tk::Point /*world*/, tk::Point /*local*/,
                                       bool /*inside_self*/)
    {
        return false;
    }
    virtual void on_pointer_drag(tk::Point /*local*/) {}
    virtual bool on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/,
                          bool /*is_touchpad*/)
    {
        return true;
    }
    virtual bool on_key(const tk::KeyEvent&) { return false; }
    virtual void on_fullscreen_changed(bool /*fullscreen*/) {}

    // Whether the overlay should show its copy-to-clipboard chrome button.
    virtual bool wants_copy_button() const { return false; }
    // True while the pointer is on one of the page's own buttons.
    virtual bool controls_hovered() const { return false; }
};

} // namespace tesseract::views
