#include "ImageViewerOverlay.h"
#include "media_utils.h"

#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tesseract::views {

// ── helpers ──────────────────────────────────────────────────────────────

static constexpr float kMarginX   = 64.0f;  // horizontal clearance from edge
static constexpr float kMarginY   = 96.0f;  // vertical clearance (caption space)
static constexpr float kZoomStep  = 1.15f;
static constexpr float kZoomMax   = 8.0f;
static constexpr float kCloseBtnS = 36.0f;

// ── public API ───────────────────────────────────────────────────────────

void ImageViewerOverlay::open(std::string media_url, std::string body,
                               int natural_w, int natural_h) {
    media_url_ = std::move(media_url);
    body_      = std::move(body);
    natural_w_ = natural_w;
    natural_h_ = natural_h;
    zoom_      = 1.0f;
    pan_x_     = 0.0f;
    pan_y_     = 0.0f;
    is_open_   = true;
    // Geometry is recomputed in paint() using current bounds.
}

void ImageViewerOverlay::close() {
    is_open_ = false;
    zoom_    = 1.0f;
    pan_x_   = 0.0f;
    pan_y_   = 0.0f;
    if (on_close) on_close();
}

void ImageViewerOverlay::set_image_provider(
    std::function<const tk::Image*(const std::string&)> fn) {
    image_provider_ = std::move(fn);
}

// ── layout ───────────────────────────────────────────────────────────────

tk::Size ImageViewerOverlay::measure(tk::LayoutCtx&, tk::Size constraints) {
    return constraints;  // fills the entire surface
}

void ImageViewerOverlay::arrange(tk::LayoutCtx& lc, tk::Rect b) {
    tk::Widget::arrange(lc, b);
    base_ = fit_media(static_cast<float>(natural_w_),
                       static_cast<float>(natural_h_),
                       b.w - kMarginX, b.h - kMarginY);
    recompute_image_rect();
    close_btn_ = { b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f,
                   kCloseBtnS, kCloseBtnS };
}

// ── private helpers ───────────────────────────────────────────────────────

void ImageViewerOverlay::recompute_image_rect() {
    const tk::Rect b = bounds();
    float iw = base_.w * zoom_;
    float ih = base_.h * zoom_;
    float cx = b.x + b.w * 0.5f + pan_x_;
    float cy = b.y + b.h * 0.5f + pan_y_;
    image_rect_ = { cx - iw * 0.5f, cy - ih * 0.5f, iw, ih };
}

void ImageViewerOverlay::clamp_pan() {
    const tk::Rect b = bounds();
    float ex = std::max(0.0f, (base_.w * zoom_ - b.w) * 0.5f + 32.0f);
    float ey = std::max(0.0f, (base_.h * zoom_ - b.h) * 0.5f + 32.0f);
    pan_x_ = std::clamp(pan_x_, -ex, ex);
    pan_y_ = std::clamp(pan_y_, -ey, ey);
}

// ── paint ─────────────────────────────────────────────────────────────────

void ImageViewerOverlay::paint(tk::PaintCtx& ctx) {
    if (!is_open_) return;

    const tk::Rect b = bounds();

    // Recompute geometry here too — zoom/pan may have changed since arrange.
    base_ = fit_media(static_cast<float>(natural_w_),
                       static_cast<float>(natural_h_),
                       b.w - kMarginX, b.h - kMarginY);
    recompute_image_rect();
    close_btn_ = { b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f,
                   kCloseBtnS, kCloseBtnS };

    auto& cv = ctx.canvas;

    // Dark backdrop
    cv.fill_rect(b, tk::Color::rgba(0, 0, 0, 210));

    // Image or placeholder
    const tk::Image* img = (image_provider_ && !media_url_.empty())
                           ? image_provider_(media_url_)
                           : nullptr;
    if (img) {
        cv.push_clip_rounded_rect(image_rect_, 4.0f);
        cv.draw_image(*img, image_rect_);
        cv.pop_clip();
    } else {
        cv.fill_rounded_rect(image_rect_, 4.0f, ctx.theme.palette.chrome_bg);
        cv.stroke_rounded_rect(image_rect_, 4.0f, ctx.theme.palette.border, 1.0f);
    }

    // Caption below image
    if (!body_.empty()) {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = b.w - kMarginX;
        auto lo = ctx.factory.build_text(body_, st);
        if (lo) {
            tk::Size sz = lo->measure();
            float tx = b.x + (b.w - sz.w) * 0.5f;
            float ty = image_rect_.y + image_rect_.h + 8.0f;
            cv.draw_text(*lo, { tx, ty },
                         tk::Color::rgba(255, 255, 255, 210));
        }
    }

    // × close button
    cv.fill_rounded_rect(close_btn_, kCloseBtnS * 0.5f,
                          tk::Color::rgba(255, 255, 255, 30));
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::UiSemibold;
        st.max_width = kCloseBtnS;
        auto lo = ctx.factory.build_text("\xC3\x97", st);  // UTF-8 ×
        if (lo) {
            tk::Size sz = lo->measure();
            float tx = close_btn_.x + (close_btn_.w - sz.w) * 0.5f;
            float ty = close_btn_.y + (close_btn_.h - sz.h) * 0.5f;
            cv.draw_text(*lo, { tx, ty },
                         tk::Color::rgba(255, 255, 255, 220));
        }
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ImageViewerOverlay::on_pointer_down(tk::Point local) {
    if (!is_open_) return false;

    tk::Point w { local.x + bounds().x, local.y + bounds().y };

    if (rect_contains(close_btn_, w)) {
        press_close_ = true;
        return true;
    }
    if (rect_contains(image_rect_, w)) {
        if (zoom_ > 1.0f) {
            press_drag_ = true;
            drag_last_  = local;
        }
        // Always consume — prevents row-click passthrough when unzoomed.
        return true;
    }
    press_outside_ = true;
    return true;
}

void ImageViewerOverlay::on_pointer_up(tk::Point local, bool inside_self) {
    if (press_drag_) {
        press_drag_ = false;
        return;
    }
    if (press_close_) {
        press_close_ = false;
        if (inside_self) {
            tk::Point w { local.x + bounds().x, local.y + bounds().y };
            if (rect_contains(close_btn_, w)) close();
        }
        return;
    }
    if (press_outside_) {
        press_outside_ = false;
        if (inside_self) close();
        return;
    }
}

void ImageViewerOverlay::on_pointer_move(tk::Point local) {
    if (!press_drag_) return;
    pan_x_ += local.x - drag_last_.x;
    pan_y_ += local.y - drag_last_.y;
    drag_last_ = local;
    clamp_pan();
    // Host repaints after every pointer_move dispatch.
}

bool ImageViewerOverlay::on_wheel(tk::Point local, float /*dx*/, float dy) {
    if (!is_open_) return false;

    // dy < 0 = wheel up = zoom in; dy > 0 = wheel down = zoom out
    float factor   = std::pow(kZoomStep, -dy);
    float new_zoom = std::clamp(zoom_ * factor, 1.0f, kZoomMax);
    if (new_zoom == zoom_) return true;

    // Anchor zoom at cursor position
    const tk::Rect b = bounds();
    tk::Point w { local.x + b.x, local.y + b.y };
    float old_iw = base_.w * zoom_;
    float old_ih = base_.h * zoom_;
    float frac_x = (old_iw > 0.0f)
                   ? (w.x - (b.x + b.w * 0.5f + pan_x_)) / old_iw : 0.0f;
    float frac_y = (old_ih > 0.0f)
                   ? (w.y - (b.y + b.h * 0.5f + pan_y_)) / old_ih : 0.0f;

    zoom_  = new_zoom;
    pan_x_ = w.x - (b.x + b.w * 0.5f) - frac_x * base_.w * zoom_;
    pan_y_ = w.y - (b.y + b.h * 0.5f) - frac_y * base_.h * zoom_;

    if (zoom_ <= 1.0f) { zoom_ = 1.0f; pan_x_ = 0.0f; pan_y_ = 0.0f; }
    else                { clamp_pan(); }

    return true;
}

} // namespace tesseract::views
