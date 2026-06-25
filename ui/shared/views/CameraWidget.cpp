#include "CameraWidget.h"

#include "tk/canvas.h"
#include "tk/host.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace tesseract::views
{

CameraWidget::CameraWidget() = default;

CameraWidget::~CameraWidget()
{
    if (capture_)
        capture_->stop();
}

void CameraWidget::open()
{
    if (opened_)
        return;
    opened_ = true;

    capture_ = tk::VideoCapture::create();
    if (!capture_)
    {
        // No camera — dismiss on the next paint cycle.
        return;
    }

    capture_->set_bgra_callback(
        [this](const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
        {
            std::lock_guard<std::mutex> lk(frame_mu_);
            last_bgra_.assign(bgra, bgra + w * h * 4);
            frame_w_ = w;
            frame_h_ = h;
        });

    capture_->start();
    start_ = std::chrono::steady_clock::now();
}

void CameraWidget::dismiss()
{
    do_dismiss_();
}

tk::Size CameraWidget::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void CameraWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    Widget::arrange(ctx, bounds);
}

void CameraWidget::paint(tk::PaintCtx& ctx)
{
    const tk::Rect bounds = bounds_;

    // No camera — fire on_dismissed without showing any UI.
    if (opened_ && !capture_)
    {
        do_dismiss_();
        return;
    }

    if (!opened_ || dismissed_)
        return;

    // Dark scrim.
    ctx.canvas.fill_rect(bounds, tk::Color::rgba(0, 0, 0, 210));

    // Live camera preview (mirrored — flip_h=true for selfie feel).
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        if (!last_bgra_.empty() && frame_w_ > 0 && frame_h_ > 0)
        {
            // Centre-fit the preview, preserving the camera aspect ratio.
            const float cam_w  = static_cast<float>(frame_w_);
            const float cam_h  = static_cast<float>(frame_h_);
            const float scale  = std::min(bounds.w / cam_w, bounds.h / cam_h);
            const float pw     = cam_w * scale;
            const float ph     = cam_h * scale;
            const tk::Rect preview{
                bounds.x + (bounds.w - pw) * 0.5f,
                bounds.y + (bounds.h - ph) * 0.5f,
                pw, ph};

            ctx.canvas.draw_bgra_premult_pixels(
                last_bgra_.data(), frame_w_, frame_h_, preview, /*flip_h=*/true);
        }
    }

    // Countdown number.
    const auto now     = std::chrono::steady_clock::now();
    const float elapsed =
        std::chrono::duration<float>(now - start_).count();
    const float remaining = kCountdownSecs - elapsed;

    if (remaining > 0.0f && !captured_)
    {
        const int digit = static_cast<int>(std::ceil(remaining));
        const std::string label = std::to_string(digit);

        tk::TextStyle st;
        st.role      = tk::FontRole::BigEmoji;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.max_width = -1.0f;
        if (auto lo = ctx.factory.build_text(label, st))
        {
            const tk::Size sz = lo->measure();
            const float tx = bounds.x + (bounds.w - sz.w) * 0.5f;
            const float ty = bounds.y + (bounds.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*lo, {tx, ty}, tk::Color::rgba(255, 255, 255, 230));
        }
    }

    // Cancel hint at the bottom.
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::Body;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.max_width = -1.0f;
        if (auto lo = ctx.factory.build_text("Click anywhere to cancel", st))
        {
            const tk::Size sz = lo->measure();
            const float tx = bounds.x + (bounds.w - sz.w) * 0.5f;
            const float ty = bounds.y + bounds.h - sz.h - 16.0f;
            ctx.canvas.draw_text(*lo, {tx, ty}, tk::Color::rgba(255, 255, 255, 140));
        }
    }

    // Trigger capture when countdown expires.
    if (remaining <= 0.0f && !captured_)
    {
        do_capture_();
        return;
    }

    // Keep repainting to animate the live preview and countdown.
    if (ctx.host)
        ctx.host->request_repaint();
}

bool CameraWidget::on_pointer_down(tk::Point /*local*/)
{
    do_dismiss_();
    return true;
}

void CameraWidget::do_capture_()
{
    if (captured_)
        return;
    captured_ = true;

    std::vector<std::uint8_t> bgra;
    std::uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        bgra = last_bgra_;
        w    = frame_w_;
        h    = frame_h_;
    }

    if (on_frame_captured && !bgra.empty())
        on_frame_captured(std::move(bgra), w, h);

    do_dismiss_();
}

void CameraWidget::do_dismiss_()
{
    if (dismissed_)
        return;
    dismissed_ = true;

    if (capture_)
    {
        capture_->stop();
        capture_.reset();
    }

    if (on_dismissed)
        on_dismissed();
}

} // namespace tesseract::views
