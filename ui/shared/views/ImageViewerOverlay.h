#pragma once

#include "MediaOverlayBase.h"
#include "Toast.h"

#include "tk/canvas.h"
#include "tk/widget.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

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
class ImageViewerOverlay : public MediaOverlayBase
{
public:
    ImageViewerOverlay();
    ~ImageViewerOverlay() override;

    // Show the overlay for the given image or sticker.
    // `display_key` is the thumbnail cache key — shown immediately while the
    // full-res `media_url` is still in flight (may be empty for stickers /
    // images with no server thumbnail).
    void open(std::string media_url, std::string display_key, std::string body,
              int natural_w, int natural_h);
    // Hide the overlay and reset zoom/pan state.
    void close();

    // On-screen image bounds after zoom + pan (valid once arrange/paint has
    // run while open). Exposed for shells and tests.
    tk::Rect image_rect() const
    {
        return image_rect_;
    }

    // Same provider lambda used by MessageListView — returns a cached tk::Image*.
    // Called during paint; may return nullptr if bytes are still loading.
    void
    set_image_provider(std::function<const tk::Image*(const std::string&)> fn);

    // Must be set by the shell so the loading spinner can trigger repaints of
    // the hosting surface while bytes are in flight. Typically:
    //   [this]{ mainAppSurface_->relayout(); }
    void set_repaint_requester(std::function<void()> fn);

    // Timer used to auto-dismiss the copy-confirmation toast. Wire to the
    // host's post_delayed (see ShellBase / RoomWindowBase). Without it a
    // shown toast never hides on its own.
    void set_post_delayed(std::function<void(int, std::function<void()>)> fn);

    // Show a self-dismissing toast pill over the lightbox (e.g. "Copied to
    // clipboard"). Auto-hides after a short delay via post_delayed_; safe to
    // call even if the overlay is later destroyed before the timer fires.
    void show_toast(std::string message);

    // on_close / on_save are inherited from MediaOverlayBase. For images the
    // save callback receives (media_url_, body_) — source URL + filename hint.

    // Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    void on_pointer_drag(tk::Point local) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

protected:
    bool on_content_pointer_down_(tk::Point world, tk::Point local) override;
    bool on_content_pointer_up_(tk::Point world, tk::Point local,
                                bool inside_self) override;
    void fire_save_() override;
    bool wants_copy_button_() const override
    {
        return true;
    }
    void fire_copy_() override;
    void dismiss_() override;

private:
    // Set base_ to the native image size and fit_zoom_ to the
    // whole-image-fit ratio for the current bounds. On the first pass
    // after open() set zoom_ = fit_zoom_; otherwise re-clamp zoom_.
    void recompute_base_(tk::Rect b);
    void recompute_image_rect();
    void clamp_pan();

    std::string media_url_;
    std::string display_key_; // thumbnail cache key — fallback while full-res loads
    std::string body_;
    int natural_w_ = 0;
    int natural_h_ = 0;

    std::function<const tk::Image*(const std::string&)> image_provider_;
    std::function<void()> request_repaint_;
    std::function<void(int, std::function<void()>)> post_delayed_;

    // Copy-confirmation toast pill (owned via the widget tree). Painted last so
    // it sits above the image + chrome. Liveness token guards the deferred
    // auto-hide against the overlay being destroyed first (pop-out windows).
    Toast* toast_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // Loading state: set on open(), cleared once image_provider_ returns non-null.
    bool is_loading_ = false;
    std::chrono::steady_clock::time_point loading_start_{};

    // Native image size (zoom 1.0 = 1:1 pixels). Set in arrange/paint and
    // reused in on_wheel. base_ * zoom_ is the on-screen image size.
    tk::Size base_{};

    // Lowest allowed zoom: the ratio at which the whole image fits the
    // viewport (≤ 1.0; 1.0 when the image already fits at 1:1).
    float fit_zoom_ = 1.0f;

    // Zoom level: 1.0 = true 1:1 (native pixels), clamped [fit_zoom_, 8.0].
    // pan_x/y are pixel offsets of the image centre from the viewport centre.
    float zoom_ = 1.0f;
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;

    // One-shot: set by open(), consumed by the first recompute_base_ to
    // start zoom_ at fit_zoom_ (zoom-to-fit on open). Cleared thereafter so
    // window resizes only re-clamp zoom rather than snapping back to fit.
    bool open_at_fit_ = false;

    tk::Rect image_rect_{}; // world-space image bounds (zoom + pan applied)

    bool press_drag_ = false;
    tk::Point drag_last_{};
};

} // namespace tesseract::views
