#pragma once

#include "tk/canvas.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views {

// Full-window lightbox overlay. Paints a dark semi-transparent backdrop over
// the entire surface with the selected image centred and scaled to fit.
//
// Usage:
//   1. Set the image provider (same lambda as MessageListView).
//   2. Wire on_close to hide the host surface/window.
//   3. Call open() when the user clicks an image thumbnail.
//   4. The shell handles Escape natively by calling close() when is_open().
//
// Zoom: scroll wheel zooms in/out anchored at cursor position (1× = fit,
//       max 8×). Click-drag pans when zoomed in. No zoom below fit level.
class ImageViewerOverlay : public tk::Widget {
public:
    // Show the overlay for the given image or sticker.
    void open(std::string media_url, std::string body,
              int natural_w, int natural_h);
    // Hide the overlay and reset zoom/pan state.
    void close();
    bool is_open() const { return is_open_; }

    // Same provider lambda used by MessageListView — returns a cached tk::Image*.
    // Called during paint; may return nullptr if bytes are still loading.
    void set_image_provider(
        std::function<const tk::Image*(const std::string&)> fn);

    // Fires when the overlay should be dismissed (× button, outside click).
    // The shell responds by hiding the host surface/window.
    std::function<void()> on_close;

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint  (tk::PaintCtx&)                   override;

    bool on_pointer_down(tk::Point local)                    override;
    void on_pointer_up  (tk::Point local, bool inside_self)  override;
    void on_pointer_drag(tk::Point local)                    override;
    bool on_wheel       (tk::Point local, float dx, float dy) override;

private:
    void recompute_image_rect();
    void clamp_pan();

    bool        is_open_   = false;
    std::string media_url_;
    std::string body_;
    int         natural_w_ = 0;
    int         natural_h_ = 0;

    std::function<const tk::Image*(const std::string&)> image_provider_;

    // Base fit-to-window size at zoom 1.0. Stored in arrange and reused in
    // paint / on_wheel to avoid calling fit_media on every frame.
    tk::Size base_{};

    // Zoom level: 1.0 = fit to window, clamped [1.0, 8.0].
    // pan_x/y are pixel offsets of the image centre from the viewport centre.
    float    zoom_  = 1.0f;
    float    pan_x_ = 0.0f;
    float    pan_y_ = 0.0f;

    tk::Rect image_rect_{};  // world-space image bounds (zoom + pan applied)
    tk::Rect close_btn_{};   // × button in top-right corner

    bool      press_close_   = false;
    bool      press_outside_ = false;
    bool      press_drag_    = false;
    tk::Point drag_last_{};
};

} // namespace tesseract::views
