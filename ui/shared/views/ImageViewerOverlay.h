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
// Zoom: opens zoomed to fit — an oversized image shrinks so the whole of
//       it is visible; an image that already fits opens at true 1:1
//       (zoom 1.0 = one image pixel per screen pixel; fit_zoom_ is never
//       above 1.0, so small images are not upscaled). Scroll wheel zooms
//       anchored at the cursor, clamped to [fit, 8×]. Click-drag pans
//       whenever the image is larger than the viewport.
class ImageViewerOverlay : public tk::Widget {
public:
    // Show the overlay for the given image or sticker.
    void open(std::string media_url, std::string body,
              int natural_w, int natural_h);
    // Hide the overlay and reset zoom/pan state.
    void close();
    bool is_open() const { return is_open_; }

    // On-screen image bounds after zoom + pan (valid once arrange/paint has
    // run while open). Exposed for shells and tests.
    tk::Rect image_rect() const { return image_rect_; }

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
    // Set base_ to the native image size and fit_zoom_ to the
    // whole-image-fit ratio for the current bounds. On the first pass
    // after open() set zoom_ = fit_zoom_; otherwise re-clamp zoom_.
    void recompute_base_(tk::Rect b);
    void recompute_image_rect();
    void clamp_pan();

    bool        is_open_   = false;
    std::string media_url_;
    std::string body_;
    int         natural_w_ = 0;
    int         natural_h_ = 0;

    std::function<const tk::Image*(const std::string&)> image_provider_;

    // Native image size (zoom 1.0 = 1:1 pixels). Set in arrange/paint and
    // reused in on_wheel. base_ * zoom_ is the on-screen image size.
    tk::Size base_{};

    // Lowest allowed zoom: the ratio at which the whole image fits the
    // viewport (≤ 1.0; 1.0 when the image already fits at 1:1).
    float    fit_zoom_ = 1.0f;

    // Zoom level: 1.0 = true 1:1 (native pixels), clamped [fit_zoom_, 8.0].
    // pan_x/y are pixel offsets of the image centre from the viewport centre.
    float    zoom_  = 1.0f;
    float    pan_x_ = 0.0f;
    float    pan_y_ = 0.0f;

    // One-shot: set by open(), consumed by the first recompute_base_ to
    // start zoom_ at fit_zoom_ (zoom-to-fit on open). Cleared thereafter so
    // window resizes only re-clamp zoom rather than snapping back to fit.
    bool     open_at_fit_ = false;

    tk::Rect image_rect_{};  // world-space image bounds (zoom + pan applied)
    tk::Rect close_btn_{};   // × button in top-right corner

    bool      press_close_   = false;
    bool      press_outside_ = false;
    bool      press_drag_    = false;
    tk::Point drag_last_{};
};

} // namespace tesseract::views
