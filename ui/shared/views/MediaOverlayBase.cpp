#include "MediaOverlayBase.h"
#include "icons.h"

#include "tk/svg.h"

#include <algorithm>
#include <cmath>

namespace tesseract::views
{

static constexpr float kCloseBtnS = 36.0f; // close / save button square size
static constexpr float kMediaOverlayBtnIconPx = 20.0f; // logical icon size inside a button

// Fixed, backdrop-tuned fill states for the chrome buttons — the scrim is
// always near-black regardless of the app's light/dark theme, so these
// bypass tk::Button's default theme-driven fill (see Button::FillOverride).
static constexpr tk::Color kCloseFillRest      = tk::Color::rgba(255, 255, 255, 30);
static constexpr tk::Color kCloseFillHover     = tk::Color::rgba(255, 255, 255, 80);
static constexpr tk::Color kCloseFillPressed   = tk::Color::rgba(255, 255, 255, 115);
static constexpr tk::Color kDarkPillFillRest    = tk::Color::rgba(0, 0, 0, 160);
static constexpr tk::Color kDarkPillFillHover   = tk::Color::rgba(60, 60, 60, 185);
static constexpr tk::Color kDarkPillFillPressed = tk::Color::rgba(95, 95, 95, 200);

// ── construction ─────────────────────────────────────────────────────────

MediaOverlayBase::MediaOverlayBase()
{
    auto close = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                               tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] { dismiss_(); });
    close_btn_->set_fill_override(tk::Button::FillOverride{
        kCloseFillRest, kCloseFillHover, kCloseFillPressed});

    auto save = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    save_btn_ = add_child(std::move(save));
    save_btn_->set_on_click([this] { if (on_save) fire_save_(); });
    save_btn_->set_fill_override(tk::Button::FillOverride{
        kDarkPillFillRest, kDarkPillFillHover, kDarkPillFillPressed});

    auto copy = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    copy_btn_ = add_child(std::move(copy));
    copy_btn_->set_on_click([this] { if (on_copy && wants_copy_button_()) fire_copy_(); });
    copy_btn_->set_fill_override(tk::Button::FillOverride{
        kDarkPillFillRest, kDarkPillFillHover, kDarkPillFillPressed});
}

// ── layout ───────────────────────────────────────────────────────────────

void MediaOverlayBase::layout_chrome_(tk::LayoutCtx& lc, tk::Rect b)
{
    close_btn_->arrange(lc, {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f,
                             kCloseBtnS, kCloseBtnS});
    save_btn_->arrange(lc, {close_btn_->bounds().x - kCloseBtnS - 4.0f,
                            b.y + 8.0f, kCloseBtnS, kCloseBtnS});
    const bool want_copy = wants_copy_button_();
    if (want_copy)
    {
        copy_btn_->arrange(lc, {save_btn_->bounds().x - kCloseBtnS - 4.0f,
                                b.y + 8.0f, kCloseBtnS, kCloseBtnS});
    }

    close_btn_->set_visible(is_open_);
    save_btn_->set_visible(is_open_);
    copy_btn_->set_visible(is_open_ && want_copy);
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
        copy_icon_.reset();
        on_icon_scale_changed_();
    }
}

void MediaOverlayBase::paint_chrome_buttons_(tk::PaintCtx& ctx)
{
    auto& cv = ctx.canvas;

    sync_icon_scale_(ctx);

    const tk::Color icon_tint = tk::Color::rgba(255, 255, 255, 220);

    // × close button. Button's own fill (via FillOverride) draws the full
    // resting/hover/pressed pill; the clip only shapes it into a circle.
    cv.push_clip_rounded_rect(close_btn_->bounds(), kCloseBtnS * 0.5f);
    close_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, close_btn_->bounds(), kMediaOverlayBtnIconPx, close_icon_, kCloseSvg, icon_tint);

    // ⬇ save button
    cv.push_clip_rounded_rect(save_btn_->bounds(), kCloseBtnS * 0.5f);
    save_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, save_btn_->bounds(), kMediaOverlayBtnIconPx, save_icon_, kDownloadSvg, icon_tint);

    // ⧉ copy-to-clipboard button (image overlay only)
    if (wants_copy_button_())
    {
        cv.push_clip_rounded_rect(copy_btn_->bounds(), kCloseBtnS * 0.5f);
        copy_btn_->paint(ctx);
        cv.pop_clip();
        draw_icon_(ctx, copy_btn_->bounds(), kMediaOverlayBtnIconPx, copy_icon_, kCopySvg, icon_tint);
    }
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

    // Only reached when no chrome button claimed the press first (real
    // tk::Button children get first refusal via Widget::dispatch_pointer_down).
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

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
