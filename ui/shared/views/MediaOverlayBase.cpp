#include "MediaOverlayBase.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/svg.h"

#include <algorithm>
#include <cmath>

namespace tesseract::views
{

static constexpr float kCloseBtnS = 36.0f; // close / save button square size
static constexpr float kBtnIconPx = 20.0f; // logical icon size inside a button

// ── layout ───────────────────────────────────────────────────────────────

void MediaOverlayBase::layout_chrome_(tk::Rect b)
{
    close_btn_ = {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f, kCloseBtnS,
                  kCloseBtnS};
    save_btn_ = {close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS,
                 kCloseBtnS};
}

// ── paint ────────────────────────────────────────────────────────────────

void MediaOverlayBase::paint_scrim_(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds(), tk::Color::rgba(0, 0, 0, 210));
}

void MediaOverlayBase::draw_icon_(tk::PaintCtx& ctx, tk::Rect box,
                                  float logical_px,
                                  std::unique_ptr<tk::Image>& cache,
                                  std::span<const std::uint8_t> svg,
                                  tk::Color tint)
{
    auto& cv = ctx.canvas;
    if (!cache)
        cache = tk::rasterize_svg(
            ctx.factory, svg,
            std::max(1, int(std::lround(logical_px * icon_scale_))), tint);
    if (cache)
        cv.draw_image(*cache, {box.x + (box.w - logical_px) * 0.5f,
                               box.y + (box.h - logical_px) * 0.5f, logical_px,
                               logical_px});
}

void MediaOverlayBase::sync_icon_scale_(tk::PaintCtx& ctx)
{
    // Lucide icons are rasterized at physical-pixel resolution and tinted; the
    // cache is invalidated whenever the canvas DPI scale changes.
    const float icon_scale = ctx.canvas.scale_factor();
    if (icon_scale != icon_scale_)
    {
        icon_scale_ = icon_scale;
        close_icon_.reset();
        save_icon_.reset();
        on_icon_scale_changed_();
    }
}

void MediaOverlayBase::paint_chrome_buttons_(tk::PaintCtx& ctx)
{
    auto& cv = ctx.canvas;

    sync_icon_scale_(ctx);

    const tk::Color icon_tint = tk::Color::rgba(255, 255, 255, 220);

    // × close button
    cv.fill_rounded_rect(close_btn_, kCloseBtnS * 0.5f,
                         tk::Color::rgba(255, 255, 255, 30));
    draw_icon_(ctx, close_btn_, kBtnIconPx, close_icon_, kCloseSvg, icon_tint);

    // ⬇ save button
    cv.fill_rounded_rect(save_btn_, kCloseBtnS * 0.5f, tk::Color{0, 0, 0, 160});
    draw_icon_(ctx, save_btn_, kBtnIconPx, save_icon_, kDownloadSvg, icon_tint);
}

// ── dismiss ──────────────────────────────────────────────────────────────

void MediaOverlayBase::dismiss_()
{
    is_open_ = false;
    if (on_close)
    {
        on_close();
    }
}

// ── pointer dispatch ───────────────────────────────────────────────────────

bool MediaOverlayBase::handle_pointer_down_(tk::Point local)
{
    if (!is_open_)
    {
        return false;
    }

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    // Chrome buttons take priority over content interactions.
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

    // Forward to the subclass content; if it declines, treat as outside tap.
    if (on_content_pointer_down_(w, local))
    {
        return true;
    }
    press_outside_ = true;
    return true;
}

void MediaOverlayBase::handle_pointer_up_(tk::Point local, bool inside_self)
{
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (press_close_)
    {
        press_close_ = false;
        if (inside_self && rect_contains(close_btn_, w))
        {
            dismiss_();
        }
        return;
    }
    if (press_save_)
    {
        press_save_ = false;
        if (inside_self && on_save && rect_contains(save_btn_, w))
        {
            fire_save_();
        }
        return;
    }
    if (on_content_pointer_up_(w, local, inside_self))
    {
        return;
    }
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
        {
            dismiss_();
        }
        return;
    }
}

} // namespace tesseract::views
