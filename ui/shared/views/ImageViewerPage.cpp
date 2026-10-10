#include "ImageViewerPage.h"
#include "shortcut_registry.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/loading_spinner.h"
#include "tk/svg.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tesseract::views
{

// ── helpers ──────────────────────────────────────────────────────────────

namespace
{
constexpr float kImageViewerMarginX = 64.0f; // horizontal clearance from edge
constexpr float kImageViewerMarginY = 96.0f; // vertical clearance (caption space)
constexpr float kZoomStep = 1.15f;
constexpr float kZoomMax = 8.0f;
} // namespace

// ── full-screen ──────────────────────────────────────────────────────────

void ImageViewerPage::on_fullscreen_changed(bool /*fullscreen*/)
{
    // Re-fit the image to the new viewport (the letterbox area changes a lot
    // when the margins collapse / restore).
    open_at_fit_ = true;
    host_.host_request_repaint();
}

// ── public API ───────────────────────────────────────────────────────────

ImageViewerPage::ImageViewerPage(MediaViewerPageHost& host) : host_(host) {}

ImageViewerPage::~ImageViewerPage() = default;

void ImageViewerPage::activate(const MediaViewerItem& item)
{
    media_url_ = item.source;
    display_key_ = item.thumbnail;
    body_ = item.caption;
    natural_w_ = item.width;
    natural_h_ = item.height;
    dims_known_ = item.width > 0 && item.height > 0;
    fitted_dims_ = {};
    zoom_ = 1.0f; // provisional until geometry (fit_zoom_) is known
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    press_drag_ = false;
    active_ = true;
    // Open zoomed to fit: oversized images shrink to the viewport, images
    // that already fit stay at 1:1 (fit_zoom_ is capped at 1.0). Resolved
    // on the first recompute_base_ once bounds — and thus fit_zoom_ — exist.
    open_at_fit_ = true;
    is_loading_    = true;
    loading_start_ = std::chrono::steady_clock::now();
    // Geometry is recomputed in paint() using current bounds.
}

void ImageViewerPage::deactivate()
{
    active_ = false;
    press_drag_ = false;
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

// ── layout ───────────────────────────────────────────────────────────────

void ImageViewerPage::arrange(tk::LayoutCtx& /*lc*/, tk::Rect b)
{
    recompute_base_(b);
    recompute_image_rect();
}

// ── private helpers ───────────────────────────────────────────────────────

void ImageViewerPage::recompute_base_(tk::Rect b)
{
    const float margin_x = host_.host_fullscreen() ? 0.0f : kImageViewerMarginX;
    const float margin_y = host_.host_fullscreen() ? 0.0f : kImageViewerMarginY;
    const float avail_w = std::max(1.0f, b.w - margin_x);
    const float avail_h = std::max(1.0f, b.h - margin_y);

    // `nw`/`nh` are the dimensions of whatever is currently driving the
    // fit: the real metadata when known, or otherwise whatever
    // host image lookup currently resolves (thumbnail first, full-res once it
    // lands).
    float nw = 0.0f;
    float nh = 0.0f;
    if (dims_known_)
    {
        nw = static_cast<float>(natural_w_);
        nh = static_cast<float>(natural_h_);
    }
    else
    {
        // Real dimensions were unknown at open() (e.g. avatar clicks —
        // Matrix m.room.member events carry no width/height info).
        const tk::Image* probe = nullptr;
        if (!media_url_.empty())
            probe = host_.host_image(media_url_);
        if (!probe && !display_key_.empty())
            probe = host_.host_image(display_key_);
        if (probe && probe->width() > 0 && probe->height() > 0)
        {
            nw = static_cast<float>(probe->width());
            nh = static_cast<float>(probe->height());
        }
    }

    if (nw > 0 && nh > 0)
    {
        base_ = {nw, nh};
        if (dims_known_)
        {
            // zoom 1.0 == native pixels (true 1:1). fit_zoom_ is the factor
            // at which the whole image fits the viewport (≤ 1.0; never
            // upscale the floor above 1:1).
            fit_zoom_ = std::min({1.0f, avail_w / nw, avail_h / nh});
        }
        else
        {
            // Cover ~75% of the viewport (upscaling allowed, unlike the
            // known-dims path above) rather than opening at the source
            // image's tiny native pixel size.
            tk::Size cover = fit_media_cover(nw, nh, b.w * 0.75f, b.h * 0.75f);
            fit_zoom_ = std::min(cover.w / nw, cover.h / nh);
        }
    }
    else
    {
        // Nothing decoded yet at all — placeholder box, no aspect to honor.
        base_ = dims_known_ ? tk::Size{avail_w, avail_h * 0.5f}
                             : tk::Size{b.w * 0.75f, b.h * 0.75f};
        fit_zoom_ = 1.0f;
    }

    // zoom_ is a multiplier of the pixels in `base_`, so it's only
    // meaningful relative to the image it was last fit for. dims_known_
    // images never change size after open(), but the unknown-dims case
    // does — a bigger/sharper image replaces a smaller one as it loads —
    // and merely clamping a zoom_ computed for the old size against the new
    // fit_zoom_ leaves it stale (clamp only raises a too-low zoom_, it never
    // lowers an now-too-high one), so redo the fit from scratch whenever the
    // resolved source dimensions actually change. open_at_fit_ forces the
    // same on first open and on fullscreen toggles (margins change, not
    // dims).
    const bool dims_changed = nw != fitted_dims_.w || nh != fitted_dims_.h;
    if (open_at_fit_ || dims_changed)
    {
        zoom_ = fit_zoom_;
        fitted_dims_ = {nw, nh};
        open_at_fit_ = false;
    }
    else
    {
        // fit_zoom_ can exceed kZoomMax when covering 75% of the viewport
        // needs upscaling a small thumbnail by more than kZoomMax (e.g. a
        // 96x96 avatar thumbnail on a large screen) — std::clamp requires
        // lo <= hi, so widen the ceiling to match whenever that happens
        // rather than asserting.
        zoom_ = std::clamp(zoom_, fit_zoom_, std::max(fit_zoom_, kZoomMax));
    }
}

void ImageViewerPage::recompute_image_rect()
{
    const tk::Rect b = host_.host_bounds();
    float iw = base_.w * zoom_;
    float ih = base_.h * zoom_;
    float cx = b.x + b.w * 0.5f + pan_x_;
    float cy = b.y + b.h * 0.5f + pan_y_;
    image_rect_ = {cx - iw * 0.5f, cy - ih * 0.5f, iw, ih};
}

void ImageViewerPage::clamp_pan()
{
    const tk::Rect b = host_.host_bounds();
    float ex = std::max(0.0f, (base_.w * zoom_ - b.w) * 0.5f + 32.0f);
    float ey = std::max(0.0f, (base_.h * zoom_ - b.h) * 0.5f + 32.0f);
    pan_x_ = std::clamp(pan_x_, -ex, ex);
    pan_y_ = std::clamp(pan_y_, -ey, ey);
}

// ── paint ─────────────────────────────────────────────────────────────────

void ImageViewerPage::paint_content(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }

    // Geometry was recomputed by arrange() just before this paint pass
    // (zoom/pan may have changed since the last layout).
    const tk::Rect b = host_.host_bounds();

    auto& cv = ctx.canvas;

    // Image or placeholder.  Try full-res first; fall back to the thumbnail
    // cache key while the full-res fetch is still in flight.
    const tk::Image* img = nullptr;
    std::string drawn_key;
    if (!media_url_.empty())
    {
        img = host_.host_image(media_url_);
        if (img)
        {
            drawn_key = media_url_;
        }
    }
    if (!img && !display_key_.empty())
    {
        img = host_.host_image(display_key_);
        if (img)
        {
            drawn_key = display_key_;
        }
    }
    // is_loading_ is cleared here (in paint) rather than via a separate
    // callback because ImageViewerPage has no direct "image ready" hook —
    // it polls the host image lookup on each frame. Once it returns non-null the
    // loading state is complete.
    if (img)
    {
        is_loading_ = false;
        cv.push_clip_rounded_rect(image_rect_, 4.0f);
        cv.draw_image(*img, image_rect_);
        cv.pop_clip();
        if (ctx.anim_damage)
        {
            ctx.anim_damage->note_image(drawn_key, image_rect_);
        }
    }
    else
    {
        cv.fill_rounded_rect(image_rect_, 4.0f, ctx.theme.palette.chrome_bg);
        cv.stroke_rounded_rect(image_rect_, 4.0f, ctx.theme.palette.border,
                               1.0f);

        // Spinning-dots loading indicator
        const float cx = image_rect_.x + image_rect_.w * 0.5f;
        const float cy = image_rect_.y + image_rect_.h * 0.5f;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loading_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(cv, {cx, cy}, phase, /*radius=*/14.0f,
                              /*dot_r=*/3.0f, tk::Color{220, 220, 220, 255});
        // Self-drive animation: schedules a layout+redraw every frame while
        // loading. Note a repaint request triggers relayout() (not just a
        // redraw), so spinner animation runs one full measure/arrange pass
        // per frame. This matches the existing video-player on_frame pattern.
        host_.host_request_repaint();
    }

    // Caption below image (hidden in full-screen — the image fills the window)
    if (!body_.empty() && !host_.host_fullscreen())
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = b.w - kImageViewerMarginX;
        auto lo = ctx.factory.build_text(body_, st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            float tx = b.x + (b.w - sz.w) * 0.5f;
            float ty = image_rect_.y + image_rect_.h + 8.0f;
            cv.draw_text(*lo, {tx, ty}, tk::Color::rgba(255, 255, 255, 210));
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ImageViewerPage::on_content_pointer_down(tk::Point w, tk::Point local)
{
    if (rect_contains(image_rect_, w))
    {
        // Pan whenever the image is larger than the viewport (true at
        // 1:1 for any image bigger than the window, not only when zoomed).
        const tk::Rect b = host_.host_bounds();
        if (base_.w * zoom_ > b.w || base_.h * zoom_ > b.h)
        {
            press_drag_ = true;
            drag_last_ = local;
        }
        // Always consume — prevents row-click passthrough when unzoomed.
        return true;
    }
    return false;
}

bool ImageViewerPage::on_content_pointer_up(tk::Point /*w*/,
                                                tk::Point /*local*/,
                                                bool /*inside_self*/)
{
    if (press_drag_)
    {
        press_drag_ = false;
        return true;
    }
    return false;
}

void ImageViewerPage::on_pointer_drag(tk::Point local)
{
    if (!press_drag_)
    {
        return;
    }
    pan_x_ += local.x - drag_last_.x;
    pan_y_ += local.y - drag_last_.y;
    drag_last_ = local;
    clamp_pan();
}

bool ImageViewerPage::on_wheel(tk::Point local, float /*dx*/, float dy, bool /*is_touchpad*/)
{
    if (!dims_known_ && fitted_dims_.w <= 0)
    {
        // Real dimensions are unknown and nothing has resolved even once
        // yet (still the loading-spinner placeholder) — there's no image
        // to zoom. Swallow the event rather than let it silently change
        // zoom_ for a placeholder box that's about to be replaced anyway.
        return true;
    }

    // dy < 0 = wheel up = zoom in; dy > 0 = wheel down = zoom out.
    // Clamp to ±1 so one physical notch always steps by kZoomStep regardless
    // of how the host reports wheel magnitude (Qt6: ±15/notch, Win32: ±90/notch,
    // GTK DISCRETE: ±1/notch). Sub-notch values (smooth-scroll trackpads) are
    // preserved proportionally.
    float factor = std::pow(kZoomStep, -std::clamp(dy, -1.0f, 1.0f));
    // fit_zoom_ can exceed kZoomMax for a small thumbnail upscaled to cover
    // 75% of the viewport (see recompute_base_) — widen the ceiling to
    // match rather than violate std::clamp's lo <= hi precondition.
    float new_zoom =
        std::clamp(zoom_ * factor, fit_zoom_, std::max(fit_zoom_, kZoomMax));
    if (new_zoom == zoom_)
    {
        return true;
    }

    // Anchor zoom at cursor position
    const tk::Rect b = host_.host_bounds();
    tk::Point w{local.x + b.x, local.y + b.y};
    float old_iw = base_.w * zoom_;
    float old_ih = base_.h * zoom_;
    float frac_x =
        (old_iw > 0.0f) ? (w.x - (b.x + b.w * 0.5f + pan_x_)) / old_iw : 0.0f;
    float frac_y =
        (old_ih > 0.0f) ? (w.y - (b.y + b.h * 0.5f + pan_y_)) / old_ih : 0.0f;

    zoom_ = new_zoom;
    pan_x_ = w.x - (b.x + b.w * 0.5f) - frac_x * base_.w * zoom_;
    pan_y_ = w.y - (b.y + b.h * 0.5f) - frac_y * base_.h * zoom_;

    // Centre when the whole image fits the viewport at the new zoom;
    // otherwise keep the cursor-anchored pan within bounds.
    if (base_.w * zoom_ <= b.w && base_.h * zoom_ <= b.h)
    {
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
    }
    else
    {
        clamp_pan();
    }

    return true;
}

bool ImageViewerPage::on_key(const tk::KeyEvent& e)
{
    const tk::Rect b = host_.host_bounds();
    const tk::Point centre_local{b.w * 0.5f, b.h * 0.5f};
    if (matches(ShortcutId::ImageZoomIn, e))
        return on_wheel(centre_local, 0.0f, -1.0f, false);
    if (matches(ShortcutId::ImageZoomOut, e))
        return on_wheel(centre_local, 0.0f, 1.0f, false);
    if (matches(ShortcutId::ImageFit, e))
    {
        zoom_ = fit_zoom_;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return true;
    }
    if (e.alt || e.meta || e.ctrl)
        return false;
    constexpr float kPanStep = 48.0f;
    float dx = 0.0f, dy = 0.0f;
    switch (e.key)
    {
    case tk::Key::Left: dx = kPanStep; break;
    case tk::Key::Right: dx = -kPanStep; break;
    case tk::Key::Up: dy = kPanStep; break;
    case tk::Key::Down: dy = -kPanStep; break;
    default: return false;
    }
    if (base_.w * zoom_ <= b.w && base_.h * zoom_ <= b.h)
        return false; // whole image visible — nothing to pan
    pan_x_ += dx;
    pan_y_ += dy;
    clamp_pan();
    return true;
}

} // namespace tesseract::views
