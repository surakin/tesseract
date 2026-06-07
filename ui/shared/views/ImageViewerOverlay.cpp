#include "ImageViewerOverlay.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/svg.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tesseract::views
{

// ── helpers ──────────────────────────────────────────────────────────────

static constexpr float kMarginX = 64.0f; // horizontal clearance from edge
static constexpr float kMarginY = 96.0f; // vertical clearance (caption space)
static constexpr float kZoomStep = 1.15f;
static constexpr float kZoomMax = 8.0f;
static constexpr float kCloseBtnS = 36.0f;

// ── public API ───────────────────────────────────────────────────────────

void ImageViewerOverlay::open(std::string media_url, std::string display_key,
                              std::string body, int natural_w, int natural_h)
{
    media_url_ = std::move(media_url);
    display_key_ = std::move(display_key);
    body_ = std::move(body);
    natural_w_ = natural_w;
    natural_h_ = natural_h;
    zoom_ = 1.0f; // provisional until geometry (fit_zoom_) is known
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    is_open_ = true;
    // Open zoomed to fit: oversized images shrink to the viewport, images
    // that already fit stay at 1:1 (fit_zoom_ is capped at 1.0). Resolved
    // on the first recompute_base_ once bounds — and thus fit_zoom_ — exist.
    open_at_fit_ = true;
    is_loading_    = true;
    loading_start_ = std::chrono::steady_clock::now();
    // Geometry is recomputed in paint() using current bounds.
}

void ImageViewerOverlay::close()
{
    is_open_ = false;
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    if (on_close)
    {
        on_close();
    }
}

void ImageViewerOverlay::set_image_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    image_provider_ = std::move(fn);
}

void ImageViewerOverlay::set_repaint_requester(std::function<void()> fn)
{
    request_repaint_ = std::move(fn);
}

// ── layout ───────────────────────────────────────────────────────────────

tk::Size ImageViewerOverlay::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void ImageViewerOverlay::arrange(tk::LayoutCtx& lc, tk::Rect b)
{
    tk::Widget::arrange(lc, b);
    recompute_base_(b);
    recompute_image_rect();
    close_btn_ = {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f, kCloseBtnS,
                  kCloseBtnS};
    save_btn_  = {close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS, kCloseBtnS};
}

// ── private helpers ───────────────────────────────────────────────────────

void ImageViewerOverlay::recompute_base_(tk::Rect b)
{
    const float avail_w = std::max(1.0f, b.w - kMarginX);
    const float avail_h = std::max(1.0f, b.h - kMarginY);
    // When natural_w/h was unknown at open() (e.g. avatar clicks — Matrix
    // m.room.member events don't carry width/height info), probe the
    // image_provider for an already-decoded tk::Image and use its real
    // pixel dimensions so the viewport doesn't stretch the placeholder
    // to the surface width.
    int nw = natural_w_;
    int nh = natural_h_;
    if ((nw <= 0 || nh <= 0) && image_provider_)
    {
        const tk::Image* probe = !media_url_.empty()
                                     ? image_provider_(media_url_)
                                     : nullptr;
        if (!probe && !display_key_.empty())
            probe = image_provider_(display_key_);
        if (probe && probe->width() > 0 && probe->height() > 0)
        {
            nw = probe->width();
            nh = probe->height();
        }
    }
    if (nw > 0 && nh > 0)
    {
        // zoom 1.0 == native pixels (true 1:1). fit_zoom_ is the factor at
        // which the whole image fits the viewport (≤ 1.0; never upscale
        // the floor above 1:1).
        base_ = {static_cast<float>(nw), static_cast<float>(nh)};
        fit_zoom_ = std::min({1.0f, avail_w / base_.w, avail_h / base_.h});
    }
    else
    {
        // Unknown intrinsic size — fall back to a reasonable placeholder.
        base_ = {avail_w, avail_h * 0.5f};
        fit_zoom_ = 1.0f;
    }
    if (open_at_fit_)
    {
        // First geometry pass after open(): start zoomed to fit. If we don't
        // know the real dimensions yet (open() was called with 0×0 and the
        // image isn't cached), keep the latch armed so we re-fit once the
        // image arrives — otherwise we'd lock to the placeholder zoom and
        // the real image would draw at the wrong size on subsequent paints.
        zoom_ = fit_zoom_;
        if (nw > 0 && nh > 0)
            open_at_fit_ = false;
    }
    else
    {
        zoom_ = std::clamp(zoom_, fit_zoom_, kZoomMax);
    }
}

void ImageViewerOverlay::recompute_image_rect()
{
    const tk::Rect b = bounds();
    float iw = base_.w * zoom_;
    float ih = base_.h * zoom_;
    float cx = b.x + b.w * 0.5f + pan_x_;
    float cy = b.y + b.h * 0.5f + pan_y_;
    image_rect_ = {cx - iw * 0.5f, cy - ih * 0.5f, iw, ih};
}

void ImageViewerOverlay::clamp_pan()
{
    const tk::Rect b = bounds();
    float ex = std::max(0.0f, (base_.w * zoom_ - b.w) * 0.5f + 32.0f);
    float ey = std::max(0.0f, (base_.h * zoom_ - b.h) * 0.5f + 32.0f);
    pan_x_ = std::clamp(pan_x_, -ex, ex);
    pan_y_ = std::clamp(pan_y_, -ey, ey);
}

// ── paint ─────────────────────────────────────────────────────────────────

void ImageViewerOverlay::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
    {
        return;
    }

    const tk::Rect b = bounds();

    // Recompute geometry here too — zoom/pan may have changed since arrange.
    recompute_base_(b);
    recompute_image_rect();
    close_btn_ = {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f, kCloseBtnS,
                  kCloseBtnS};
    save_btn_  = {close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS, kCloseBtnS};

    auto& cv = ctx.canvas;

    // Dark backdrop
    cv.fill_rect(b, tk::Color::rgba(0, 0, 0, 210));

    // Image or placeholder.  Try full-res first; fall back to the thumbnail
    // cache key while the full-res fetch is still in flight.
    const tk::Image* img = nullptr;
    std::string drawn_key;
    if (image_provider_ && !media_url_.empty())
    {
        img = image_provider_(media_url_);
        if (img)
        {
            drawn_key = media_url_;
        }
    }
    if (!img && image_provider_ && !display_key_.empty())
    {
        img = image_provider_(display_key_);
        if (img)
        {
            drawn_key = display_key_;
        }
    }
    // is_loading_ is cleared here (in paint) rather than via a separate
    // callback because ImageViewerOverlay has no direct "image ready" hook —
    // it polls image_provider_ on each frame. Once it returns non-null the
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
        constexpr float kRadius = 14.0f;
        constexpr float kDotR   = 3.0f;
        constexpr int   kN      = 8;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loading_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        for (int i = 0; i < kN; ++i)
        {
            const float angle =
                (static_cast<float>(i) / kN + phase) * 2.0f * 3.14159265f;
            const float dx    = std::cos(angle) * kRadius;
            const float dy    = std::sin(angle) * kRadius;
            const float t     = static_cast<float>(i) / kN;
            const auto  alpha = static_cast<uint8_t>(40.0f + 215.0f * t);
            cv.fill_rounded_rect({cx + dx - kDotR, cy + dy - kDotR,
                                   kDotR * 2.0f, kDotR * 2.0f},
                                 kDotR, tk::Color{220, 220, 220, alpha});
        }
        // Self-drive animation: schedules a layout+redraw every frame while
        // loading. Note request_repaint_() triggers relayout() (not just a
        // redraw), so spinner animation runs one full measure/arrange pass
        // per frame. This matches the existing video-player on_frame pattern.
        if (request_repaint_)
            request_repaint_();
    }

    // Caption below image
    if (!body_.empty())
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = b.w - kMarginX;
        auto lo = ctx.factory.build_text(body_, st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            float tx = b.x + (b.w - sz.w) * 0.5f;
            float ty = image_rect_.y + image_rect_.h + 8.0f;
            cv.draw_text(*lo, {tx, ty}, tk::Color::rgba(255, 255, 255, 210));
        }
    }

    // Close / download buttons — Lucide icons tinted white. Re-rasterized when
    // the canvas DPI scale changes so they stay crisp.
    constexpr float kIconPx = 20.0f;
    const float icon_scale = cv.scale_factor();
    if (icon_scale != icon_scale_)
    {
        icon_scale_ = icon_scale;
        close_icon_.reset();
        save_icon_.reset();
    }
    const int icon_phys = std::max(1, int(std::lround(kIconPx * icon_scale)));
    auto draw_btn_icon = [&](tk::Rect btn, std::unique_ptr<tk::Image>& cache,
                             std::span<const std::uint8_t> svg)
    {
        if (!cache)
            cache = tk::rasterize_svg(ctx.factory, svg, icon_phys,
                                      tk::Color::rgba(255, 255, 255, 220));
        if (cache)
            cv.draw_image(*cache, {btn.x + (btn.w - kIconPx) * 0.5f,
                                   btn.y + (btn.h - kIconPx) * 0.5f, kIconPx,
                                   kIconPx});
    };

    // × close button
    cv.fill_rounded_rect(close_btn_, kCloseBtnS * 0.5f,
                         tk::Color::rgba(255, 255, 255, 30));
    draw_btn_icon(close_btn_, close_icon_, kCloseSvg);

    // ⬇ save button
    cv.fill_rounded_rect(save_btn_, kCloseBtnS * 0.5f,
                         tk::Color{0, 0, 0, 160});
    draw_btn_icon(save_btn_, save_icon_, kDownloadSvg);
}

// ── pointer events ────────────────────────────────────────────────────────

bool ImageViewerOverlay::on_pointer_down(tk::Point local)
{
    if (!is_open_)
    {
        return false;
    }

    tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (rect_contains(close_btn_, w))
    {
        press_close_ = true;
        return true;
    }
    if (rect_contains(save_btn_, w))
    {
        press_save_ = true;
        return true;
    }
    if (rect_contains(image_rect_, w))
    {
        // Pan whenever the image is larger than the viewport (true at
        // 1:1 for any image bigger than the window, not only when zoomed).
        const tk::Rect b = bounds();
        if (base_.w * zoom_ > b.w || base_.h * zoom_ > b.h)
        {
            press_drag_ = true;
            drag_last_ = local;
        }
        // Always consume — prevents row-click passthrough when unzoomed.
        return true;
    }
    press_outside_ = true;
    return true;
}

void ImageViewerOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    if (press_drag_)
    {
        press_drag_ = false;
        return;
    }
    if (press_close_)
    {
        press_close_ = false;
        if (inside_self)
        {
            tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(close_btn_, w))
            {
                close();
            }
        }
        return;
    }
    if (press_save_)
    {
        press_save_ = false;
        if (inside_self && on_save)
        {
            tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(save_btn_, w))
            {
                on_save(media_url_, body_);
            }
        }
        return;
    }
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
        {
            close();
        }
        return;
    }
}

void ImageViewerOverlay::on_pointer_drag(tk::Point local)
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

bool ImageViewerOverlay::on_wheel(tk::Point local, float /*dx*/, float dy)
{
    if (!is_open_)
    {
        return false;
    }

    // dy < 0 = wheel up = zoom in; dy > 0 = wheel down = zoom out.
    // Clamp to ±1 so one physical notch always steps by kZoomStep regardless
    // of how the host reports wheel magnitude (Qt6: ±15/notch, Win32: ±90/notch,
    // GTK DISCRETE: ±1/notch). Sub-notch values (smooth-scroll trackpads) are
    // preserved proportionally.
    float factor = std::pow(kZoomStep, -std::clamp(dy, -1.0f, 1.0f));
    float new_zoom = std::clamp(zoom_ * factor, fit_zoom_, kZoomMax);
    if (new_zoom == zoom_)
    {
        return true;
    }

    // Anchor zoom at cursor position
    const tk::Rect b = bounds();
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

} // namespace tesseract::views
