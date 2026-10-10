#pragma once

#include "MediaViewerPage.h"

#include "tk/canvas.h"
#include "tk/widget.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

// Image page of MediaViewerOverlay: the selected image centred and scaled to
// fit, with a caption underneath. Non-widget — the overlay owns the scrim,
// chrome buttons and pointer routing and forwards to this page.
//
// Zoom: opens zoomed to fit — an oversized image shrinks so the whole of
//       it is visible; an image that already fits opens at true 1:1
//       (zoom 1.0 = one image pixel per screen pixel; fit_zoom_ is never
//       above 1.0, so small images are not upscaled). Scroll wheel zooms
//       anchored at the cursor, clamped to [fit, 8x]. Click-drag pans
//       whenever the image is larger than the viewport.
class ImageViewerPage : public MediaViewerPage
{
public:
    explicit ImageViewerPage(MediaViewerPageHost& host);
    ~ImageViewerPage() override;

    void activate(const MediaViewerItem& item) override;
    void deactivate() override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_content(tk::PaintCtx&) override;
    bool on_content_pointer_down(tk::Point world, tk::Point local) override;
    bool on_content_pointer_up(tk::Point world, tk::Point local,
                               bool inside_self) override;
    void on_pointer_drag(tk::Point local) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad) override;
    // + / - zoom around the centre, 0 back to fit, arrows pan while zoomed.
    bool on_key(const tk::KeyEvent& e) override;
    void on_fullscreen_changed(bool fullscreen) override;
    bool wants_copy_button() const override
    {
        return true;
    }

    // On-screen image bounds after zoom + pan (valid once arrange/paint has
    // run while active).
    tk::Rect image_rect() const
    {
        return image_rect_;
    }

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
    // True when open() was given real w/h (from Matrix info.w/h). When
    // false (e.g. avatar clicks — m.room.member carries no size metadata),
    // recompute_base_ sizes the box to cover ~75% of the viewport off
    // whatever image is currently decoded (thumbnail first, full-res once
    // it lands) instead of the fixed metadata size.
    bool dims_known_ = false;
    // Dimensions of whatever image (or metadata) zoom_/fit_zoom_ were last
    // computed against. zoom_ is a multiplier of THESE pixels, so whenever
    // the resolved image's actual size changes — the thumbnail-to-full-res
    // transition being the main case, since dims_known_ images never change
    // dimensions after open() — recompute_base_ must redo the fit rather
    // than merely clamp the now-stale zoom_.
    tk::Size fitted_dims_{};

    MediaViewerPageHost& host_;
    bool active_ = false;

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
